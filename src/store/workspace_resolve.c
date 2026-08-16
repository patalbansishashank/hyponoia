/*
 * workspace_resolve.c — the one resolver (§4 A6) and the one workspace open
 * path it feeds (§4 A1). See workspace_resolve.h for the contract: precedence,
 * the TOML read format, deterministic id derivation, and the collision gate.
 */
#include "store/workspace_resolve.h"

#include "foundation/compat_fs.h" /* hyp_canonical_path, hyp_fopen */
#include "foundation/constants.h"
#include "foundation/platform.h" /* hyp_file_exists, hyp_is_dir, hyp_normalize_path_sep */
#include "foundation/sha256.h"
#include "foundation/str_util.h" /* hyp_validate_project_name */
#include "pipeline/pipeline.h"   /* hyp_project_name_from_path — THE slug function */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    WSR_TOML_MAX_BYTES = 65536, /* matches userconfig's MAX_CONFIG_SIZE */
    WSR_ID_HASH_HEX = 16,       /* "ws-" + 16 hex chars of SHA-256 */
};

/* ── Small helpers ───────────────────────────────────────────────── */

static void wsr_err(char *err, size_t err_sz, const char *fmt, ...) {
    if (!err || err_sz == 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(err, err_sz, fmt, ap);
    va_end(ap);
}

bool hyp_wsr_role_known(const char *role) {
    if (!role) {
        return false;
    }
    return strcmp(role, HYP_WSR_ROLE_MEMBER) == 0 || strcmp(role, HYP_WSR_ROLE_VENDORED) == 0 ||
           strcmp(role, HYP_WSR_ROLE_GENERATED) == 0 || strcmp(role, HYP_WSR_ROLE_UPSTREAM) == 0 ||
           strcmp(role, HYP_WSR_ROLE_REFERENCE) == 0;
}

const char *hyp_wsr_err_str(hyp_wsr_err_t err) {
    switch (err) {
    case HYP_WSR_OK:
        return "ok";
    case HYP_WSR_ERR_NULL:
        return "required argument is NULL";
    case HYP_WSR_ERR_TOML_READ:
        return "workspace TOML cannot be read";
    case HYP_WSR_ERR_TOML_PARSE:
        return "workspace TOML is outside the published subset";
    case HYP_WSR_ERR_NAME:
        return "workspace name is not a valid slug";
    case HYP_WSR_ERR_NO_REPOS:
        return "workspace declares no repos";
    case HYP_WSR_ERR_TOO_MANY_REPOS:
        return "workspace declares more repos than the cap";
    case HYP_WSR_ERR_REPO_PATH:
        return "a member repo path is missing or not a directory";
    case HYP_WSR_ERR_ROLE:
        return "a member role is outside the vocabulary";
    case HYP_WSR_ERR_SLUG:
        return "a member slug could not be derived";
    case HYP_WSR_ERR_SLUG_COLLISION:
        return "two member repos slug to the same name";
    case HYP_WSR_ERR_PROVIDER:
        return "the workspace provider could not answer";
    default:
        return "unknown workspace resolution error";
    }
}

/* ── TOML reading (the published subset, fail closed) ────────────── */

typedef struct {
    char name[HYP_ADDR_SLUG_MAX + 1]; /* "" = no name declared */
    hyp_wsr_member_spec_t specs[HYP_WSR_MAX_REPOS];
    int count;
} wsr_toml_t;

/* Parse one basic-string value: leading '"' at *p, escapes \\ and \" only.
 * Advances *p past the closing quote. False on any other shape. */
static bool wsr_parse_string(const char **p, char *out, size_t out_sz) {
    const char *c = *p;
    if (*c != '"') {
        return false;
    }
    c++;
    size_t n = 0;
    while (*c && *c != '"') {
        char ch = *c;
        if (ch == '\\') {
            c++;
            if (*c == '\\' || *c == '"') {
                ch = *c;
            } else {
                return false; /* unknown escape — refuse, never guess */
            }
        }
        if (n + 1 >= out_sz) {
            return false; /* over-long — refused, not truncated */
        }
        out[n++] = ch;
        c++;
    }
    if (*c != '"') {
        return false; /* unterminated */
    }
    out[n] = '\0';
    *p = c + 1;
    return true;
}

static const char *wsr_skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return p;
}

/* True when the rest of the line is only whitespace or a trailing comment. */
static bool wsr_line_ends(const char *p) {
    p = wsr_skip_ws(p);
    return *p == '\0' || *p == '#';
}

static hyp_wsr_err_t wsr_toml_parse(const char *text, wsr_toml_t *out, char *err, size_t err_sz) {
    memset(out, 0, sizeof(*out));
    bool in_repo = false;
    bool have_path[HYP_WSR_MAX_REPOS] = {false};
    bool have_role[HYP_WSR_MAX_REPOS] = {false};
    bool have_name = false;

    int line_no = 0;
    const char *line = text;
    while (line) {
        line_no++;
        const char *nl = strchr(line, '\n');
        char buf[HYP_WSR_PATH_MAX + HYP_SZ_512];
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        if (len >= sizeof(buf)) {
            wsr_err(err, err_sz, "line %d: line too long", line_no);
            return HYP_WSR_ERR_TOML_PARSE;
        }
        memcpy(buf, line, len);
        buf[len] = '\0';
        if (len > 0 && buf[len - 1] == '\r') {
            buf[len - 1] = '\0';
        }
        line = nl ? nl + 1 : NULL;

        const char *p = wsr_skip_ws(buf);
        if (*p == '\0' || *p == '#') {
            continue;
        }

        if (strncmp(p, "[[repos]]", strlen("[[repos]]")) == 0) {
            p += strlen("[[repos]]");
            if (!wsr_line_ends(p)) {
                wsr_err(err, err_sz, "line %d: trailing content after [[repos]]", line_no);
                return HYP_WSR_ERR_TOML_PARSE;
            }
            if (out->count >= HYP_WSR_MAX_REPOS) {
                wsr_err(err, err_sz, "line %d: more than %d repos", line_no, HYP_WSR_MAX_REPOS);
                return HYP_WSR_ERR_TOO_MANY_REPOS;
            }
            out->count++;
            in_repo = true;
            continue;
        }

        /* key = "value" — the only remaining accepted shape. */
        const char *key_start = p;
        while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_') {
            p++;
        }
        size_t key_len = (size_t)(p - key_start);
        p = wsr_skip_ws(p);
        if (key_len == 0 || *p != '=') {
            wsr_err(err, err_sz, "line %d: not in the published subset", line_no);
            return HYP_WSR_ERR_TOML_PARSE;
        }
        p = wsr_skip_ws(p + 1);

        if (key_len == strlen("name") && strncmp(key_start, "name", key_len) == 0) {
            if (in_repo) {
                wsr_err(err, err_sz, "line %d: name must precede [[repos]]", line_no);
                return HYP_WSR_ERR_TOML_PARSE;
            }
            if (have_name) {
                wsr_err(err, err_sz, "line %d: duplicate name", line_no);
                return HYP_WSR_ERR_TOML_PARSE;
            }
            if (!wsr_parse_string(&p, out->name, sizeof(out->name)) || !wsr_line_ends(p)) {
                wsr_err(err, err_sz, "line %d: name must be a basic string", line_no);
                return HYP_WSR_ERR_TOML_PARSE;
            }
            have_name = true;
            continue;
        }

        bool is_path = key_len == strlen("path") && strncmp(key_start, "path", key_len) == 0;
        bool is_role = key_len == strlen("role") && strncmp(key_start, "role", key_len) == 0;
        if (!is_path && !is_role) {
            wsr_err(err, err_sz, "line %d: unknown key '%.*s'", line_no, (int)key_len, key_start);
            return HYP_WSR_ERR_TOML_PARSE;
        }
        if (!in_repo) {
            wsr_err(err, err_sz, "line %d: '%.*s' outside [[repos]]", line_no, (int)key_len,
                    key_start);
            return HYP_WSR_ERR_TOML_PARSE;
        }
        int idx = out->count - 1;
        bool *seen = is_path ? &have_path[idx] : &have_role[idx];
        if (*seen) {
            wsr_err(err, err_sz, "line %d: duplicate '%.*s'", line_no, (int)key_len, key_start);
            return HYP_WSR_ERR_TOML_PARSE;
        }
        char *dst = is_path ? out->specs[idx].path : out->specs[idx].role;
        size_t dst_sz = is_path ? sizeof(out->specs[idx].path) : sizeof(out->specs[idx].role);
        if (!wsr_parse_string(&p, dst, dst_sz) || !wsr_line_ends(p)) {
            wsr_err(err, err_sz, "line %d: '%.*s' must be a basic string", line_no, (int)key_len,
                    key_start);
            return HYP_WSR_ERR_TOML_PARSE;
        }
        if (is_path && dst[0] == '\0') {
            wsr_err(err, err_sz, "line %d: path must not be empty", line_no);
            return HYP_WSR_ERR_TOML_PARSE;
        }
        *seen = true;
    }

    for (int i = 0; i < out->count; i++) {
        if (!have_path[i]) {
            wsr_err(err, err_sz, "repo %d declares no path", i + 1);
            return HYP_WSR_ERR_TOML_PARSE;
        }
    }
    return HYP_WSR_OK;
}

static hyp_wsr_err_t wsr_toml_read(const char *toml_path, wsr_toml_t *out, char *err,
                                   size_t err_sz) {
    FILE *f = hyp_fopen(toml_path, "rb");
    if (!f) {
        wsr_err(err, err_sz, "cannot open '%s'", toml_path);
        return HYP_WSR_ERR_TOML_READ;
    }
    char *text = malloc(WSR_TOML_MAX_BYTES + 1);
    if (!text) {
        (void)fclose(f);
        return HYP_WSR_ERR_TOML_READ;
    }
    size_t n = fread(text, 1, WSR_TOML_MAX_BYTES + 1, f);
    (void)fclose(f);
    if (n > WSR_TOML_MAX_BYTES) {
        free(text);
        wsr_err(err, err_sz, "'%s' exceeds %d bytes", toml_path, WSR_TOML_MAX_BYTES);
        return HYP_WSR_ERR_TOML_READ;
    }
    text[n] = '\0';
    if (memchr(text, '\0', n) != NULL) {
        free(text);
        wsr_err(err, err_sz, "'%s' contains a NUL byte", toml_path);
        return HYP_WSR_ERR_TOML_PARSE;
    }
    hyp_wsr_err_t rc = wsr_toml_parse(text, out, err, err_sz);
    free(text);
    return rc;
}

/* ── Members: canonicalize, slug, validate ───────────────────────── */

static bool wsr_path_is_absolute(const char *p) {
    if (p[0] == '/' || p[0] == '\\') {
        return true;
    }
    return ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':';
}

/* Resolve one spec into a member: join relative paths against base_dir,
 * canonicalize, require a directory, derive THE slug. */
static hyp_wsr_err_t wsr_member_from_spec(const hyp_wsr_member_spec_t *spec, const char *base_dir,
                                          hyp_wsr_member_t *m, char *err, size_t err_sz) {
    char joined[HYP_SZ_4K];
    if (wsr_path_is_absolute(spec->path)) {
        if ((size_t)snprintf(joined, sizeof(joined), "%s", spec->path) >= sizeof(joined)) {
            wsr_err(err, err_sz, "member path too long: '%s'", spec->path);
            return HYP_WSR_ERR_REPO_PATH;
        }
    } else if ((size_t)snprintf(joined, sizeof(joined), "%s/%s", base_dir, spec->path) >=
               sizeof(joined)) {
        wsr_err(err, err_sz, "member path too long: '%s'", spec->path);
        return HYP_WSR_ERR_REPO_PATH;
    }

    char canon[HYP_SZ_4K];
    if (!hyp_canonical_path(joined, canon, sizeof(canon)) || !hyp_is_dir(canon)) {
        wsr_err(err, err_sz, "member repo is not an existing directory: '%s'", joined);
        return HYP_WSR_ERR_REPO_PATH;
    }
    hyp_normalize_path_sep(canon);
    if ((size_t)snprintf(m->root, sizeof(m->root), "%s", canon) >= sizeof(m->root)) {
        wsr_err(err, err_sz, "member root over %d bytes: '%s'", HYP_WSR_PATH_MAX, canon);
        return HYP_WSR_ERR_REPO_PATH;
    }

    char *slug = hyp_project_name_from_path(m->root);
    if (!slug) {
        return HYP_WSR_ERR_SLUG;
    }
    bool slug_ok = hyp_validate_project_name(slug) &&
                   (size_t)snprintf(m->slug, sizeof(m->slug), "%s", slug) < sizeof(m->slug);
    free(slug);
    if (!slug_ok) {
        wsr_err(err, err_sz, "cannot derive a valid slug for '%s'", m->root);
        return HYP_WSR_ERR_SLUG;
    }

    if (spec->role[0] != '\0') {
        if (!hyp_wsr_role_known(spec->role)) {
            wsr_err(err, err_sz, "unknown role '%s' for '%s'", spec->role, m->root);
            return HYP_WSR_ERR_ROLE;
        }
        (void)snprintf(m->role, sizeof(m->role), "%s", spec->role);
    } else {
        m->role[0] = '\0'; /* absent: A2-A5 derive it later */
    }
    return HYP_WSR_OK;
}

static int wsr_member_cmp(const void *a, const void *b) {
    return strcmp(((const hyp_wsr_member_t *)a)->slug, ((const hyp_wsr_member_t *)b)->slug);
}

/* ── The deterministic id ────────────────────────────────────────── */

/* "ws-" + first WSR_ID_HASH_HEX hex chars of SHA-256 over the SORTED member
 * slugs, domain-tagged and length-prefixed. A pure function of the member SET:
 * neither declaration order nor "the first repo" can reach it. */
static void wsr_derive_id(const hyp_wsr_member_t *members, int count, char *out, size_t out_sz) {
    static const char domain[] = "hyp-workspace-id-v1";
    hyp_sha256_ctx c;
    hyp_sha256_init(&c);
    hyp_sha256_update(&c, domain, sizeof(domain));
    for (int i = 0; i < count; i++) {
        char lenbuf[HYP_SZ_32];
        (void)snprintf(lenbuf, sizeof(lenbuf), "%zu:", strlen(members[i].slug));
        hyp_sha256_update(&c, lenbuf, strlen(lenbuf));
        hyp_sha256_update(&c, members[i].slug, strlen(members[i].slug));
    }
    uint8_t digest[HYP_SHA256_DIGEST_LEN];
    hyp_sha256_final(&c, digest);
    static const char hex[] = "0123456789abcdef";
    char id[WSR_ID_HASH_HEX + 1];
    for (int i = 0; i < WSR_ID_HASH_HEX / 2; i++) {
        id[i * 2] = hex[digest[i] >> 4];
        id[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    id[WSR_ID_HASH_HEX] = '\0';
    (void)snprintf(out, out_sz, "ws-%s", id);
}

/* ── Resolve ─────────────────────────────────────────────────────── */

/* Shared tail for every source: sort, gate, derive, stamp. */
static hyp_wsr_err_t wsr_finish(hyp_wsr_resolved_t *out, const char *toml_name, char *err,
                                size_t err_sz) {
    qsort(out->members, (size_t)out->member_count, sizeof(out->members[0]), wsr_member_cmp);

    /* The collision gate: /a/b and /a-b both slug to a-b; inside one workspace
     * that merges two repos, so it is REFUSED, naming both paths. */
    for (int i = 1; i < out->member_count; i++) {
        if (strcmp(out->members[i - 1].slug, out->members[i].slug) == 0) {
            wsr_err(err, err_sz, "slug collision: '%s' and '%s' both slug to '%s'",
                    out->members[i - 1].root, out->members[i].root, out->members[i].slug);
            return HYP_WSR_ERR_SLUG_COLLISION;
        }
    }

    if (toml_name && toml_name[0] != '\0') {
        if (!hyp_validate_project_name(toml_name)) {
            wsr_err(err, err_sz, "workspace name '%s' is not a valid slug", toml_name);
            return HYP_WSR_ERR_NAME;
        }
        (void)snprintf(out->id, sizeof(out->id), "%s", toml_name);
    } else if (out->member_count == 1) {
        /* A workspace of one is named by its only member — which is what makes
         * A1's migration the identity function on every existing address. */
        (void)snprintf(out->id, sizeof(out->id), "%s", out->members[0].slug);
    } else {
        wsr_derive_id(out->members, out->member_count, out->id, sizeof(out->id));
    }
    return HYP_WSR_OK;
}

hyp_wsr_err_t hyp_wsr_resolve(const char *start_dir, const char *toml_path,
                              const hyp_wsr_provider_t *provider, hyp_wsr_resolved_t *out,
                              char *err, size_t err_sz) {
    if (err && err_sz > 0) {
        err[0] = '\0';
    }
    if (!out) {
        return HYP_WSR_ERR_NULL;
    }
    memset(out, 0, sizeof(*out));
    if (!start_dir) {
        return HYP_WSR_ERR_NULL;
    }

    char start_canon[HYP_SZ_4K];
    if (!hyp_canonical_path(start_dir, start_canon, sizeof(start_canon)) ||
        !hyp_is_dir(start_canon)) {
        wsr_err(err, err_sz, "start dir is not an existing directory: '%s'", start_dir);
        return HYP_WSR_ERR_REPO_PATH;
    }
    hyp_normalize_path_sep(start_canon);

    hyp_wsr_err_t rc;

    /* 1. EXPLICIT TOML governs; its failures refuse, never fall through. */
    if (toml_path) {
        wsr_toml_t *toml = calloc(HYP_ALLOC_ONE, sizeof(wsr_toml_t));
        if (!toml) {
            return HYP_WSR_ERR_NULL;
        }
        rc = wsr_toml_read(toml_path, toml, err, err_sz);
        if (rc == HYP_WSR_OK && toml->count == 0) {
            wsr_err(err, err_sz, "'%s' declares no [[repos]]", toml_path);
            rc = HYP_WSR_ERR_NO_REPOS;
        }
        if (rc != HYP_WSR_OK) {
            free(toml);
            memset(out, 0, sizeof(*out));
            return rc;
        }
        /* Relative repo paths resolve against the TOML's own directory. */
        char base[HYP_SZ_4K];
        (void)snprintf(base, sizeof(base), "%s", toml_path);
        hyp_normalize_path_sep(base);
        char *slash = strrchr(base, '/');
        if (slash) {
            *slash = '\0';
        } else {
            (void)snprintf(base, sizeof(base), ".");
        }
        for (int i = 0; i < toml->count; i++) {
            rc = wsr_member_from_spec(&toml->specs[i], base, &out->members[i], err, err_sz);
            if (rc != HYP_WSR_OK) {
                free(toml);
                memset(out, 0, sizeof(*out));
                return rc;
            }
        }
        out->member_count = toml->count;
        out->source = HYP_WSR_SOURCE_TOML;
        rc = wsr_finish(out, toml->name, err, err_sz);
        free(toml);
        if (rc != HYP_WSR_OK) {
            memset(out, 0, sizeof(*out));
        }
        return rc;
    }

    /* 2. PROVIDER. NONE falls through; ERROR refuses — "could not tell" is
     * never "no workspace". */
    if (provider && provider->resolve) {
        hyp_wsr_member_spec_t *specs = calloc(HYP_WSR_MAX_REPOS, sizeof(hyp_wsr_member_spec_t));
        if (!specs) {
            return HYP_WSR_ERR_NULL;
        }
        int count = 0;
        hyp_wsr_provider_rc_t prc =
            provider->resolve(provider->ctx, start_canon, specs, HYP_WSR_MAX_REPOS, &count);
        if (prc == HYP_WSR_PROVIDER_ERROR) {
            wsr_err(err, err_sz, "provider '%s' could not answer",
                    provider->name ? provider->name : "(unnamed)");
            free(specs);
            return HYP_WSR_ERR_PROVIDER;
        }
        if (prc == HYP_WSR_PROVIDER_FOUND) {
            if (count < 1) {
                wsr_err(err, err_sz, "provider '%s' found a workspace with no repos",
                        provider->name ? provider->name : "(unnamed)");
                free(specs);
                return HYP_WSR_ERR_NO_REPOS;
            }
            if (count > HYP_WSR_MAX_REPOS) {
                free(specs);
                return HYP_WSR_ERR_TOO_MANY_REPOS;
            }
            for (int i = 0; i < count; i++) {
                rc = wsr_member_from_spec(&specs[i], start_canon, &out->members[i], err, err_sz);
                if (rc != HYP_WSR_OK) {
                    free(specs);
                    memset(out, 0, sizeof(*out));
                    return rc;
                }
            }
            free(specs);
            out->member_count = count;
            out->source = HYP_WSR_SOURCE_PROVIDER;
            rc = wsr_finish(out, NULL, err, err_sz);
            if (rc != HYP_WSR_OK) {
                memset(out, 0, sizeof(*out));
            }
            return rc;
        }
        free(specs);
    }

    /* 3. ZERO-CONFIG: this repo, a workspace of one, role "member" (B4). The
     * same code path and the same shape — a default, not a mode. */
    hyp_wsr_member_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    if ((size_t)snprintf(spec.path, sizeof(spec.path), "%s", start_canon) >= sizeof(spec.path)) {
        wsr_err(err, err_sz, "start dir over %d bytes", HYP_WSR_PATH_MAX);
        return HYP_WSR_ERR_REPO_PATH;
    }
    (void)snprintf(spec.role, sizeof(spec.role), "%s", HYP_WSR_ROLE_MEMBER);
    rc = wsr_member_from_spec(&spec, start_canon, &out->members[0], err, err_sz);
    if (rc != HYP_WSR_OK) {
        memset(out, 0, sizeof(*out));
        return rc;
    }
    out->member_count = 1;
    out->source = HYP_WSR_SOURCE_ZERO_CONFIG;
    rc = wsr_finish(out, NULL, err, err_sz);
    if (rc != HYP_WSR_OK) {
        memset(out, 0, sizeof(*out));
    }
    return rc;
}

/* ── TOML discovery ──────────────────────────────────────────────── */

bool hyp_wsr_toml_find(const char *start_dir, char *out, size_t out_sz) {
    if (!start_dir || !out || out_sz == 0) {
        return false;
    }
    char dir[HYP_SZ_4K];
    if (!hyp_canonical_path(start_dir, dir, sizeof(dir))) {
        return false;
    }
    hyp_normalize_path_sep(dir);

    for (;;) {
        char candidate[HYP_SZ_4K];
        if ((size_t)snprintf(candidate, sizeof(candidate), "%s/%s", dir, HYP_WSR_TOML_NAME) <
                sizeof(candidate) &&
            hyp_file_exists(candidate)) {
            return (size_t)snprintf(out, out_sz, "%s", candidate) < out_sz;
        }
        char *slash = strrchr(dir, '/');
        if (!slash || slash == dir) {
            /* "/repo" -> check "/" itself, then stop. */
            if (slash && dir[1] != '\0') {
                dir[1] = '\0';
                continue;
            }
            return false;
        }
        *slash = '\0';
    }
}

/* ── Open (A1) ───────────────────────────────────────────────────── */

hyp_store_t *hyp_wsr_store_open(const hyp_wsr_resolved_t *ws, char *err, size_t err_sz) {
    if (err && err_sz > 0) {
        err[0] = '\0';
    }
    if (!ws || ws->member_count < 1 || ws->member_count > HYP_WSR_MAX_REPOS || ws->id[0] == '\0') {
        wsr_err(err, err_sz, "unresolved workspace");
        return NULL;
    }

    /* The SAME open path and file naming a per-repo project has always had:
     * <cache>/<id>.db. One layout — a workspace of one opens the
     * byte-identical file its per-repo index already lives in. */
    hyp_store_t *s = hyp_store_open(ws->id);
    if (!s) {
        wsr_err(err, err_sz, "cannot open workspace store '%s'", ws->id);
        return NULL;
    }

    hyp_workspace_repo_t rows[HYP_WSR_MAX_REPOS];
    for (int i = 0; i < ws->member_count; i++) {
        rows[i].slug = ws->members[i].slug;
        rows[i].root_path = ws->members[i].root;
        rows[i].role = ws->members[i].role[0] != '\0' ? ws->members[i].role : NULL;
    }

    /* Binding the registry IS the migration: for a pre-A1 per-repo store this
     * writes the workspace_meta/workspace_repos rows and touches nothing else
     * — the identity function on every existing address. */
    if (hyp_store_workspace_bind(s, ws->id, rows, ws->member_count, err, err_sz) != HYP_STORE_OK) {
        if (err && err_sz > 0 && err[0] == '\0') {
            (void)snprintf(err, err_sz, "%s", hyp_store_error(s));
        }
        hyp_store_close(s);
        return NULL;
    }
    return s;
}
