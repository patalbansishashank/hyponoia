/*
 * role.c — role derivation and the role audit. The contract, the precedence
 * and the reasoning live in role.h; this file is only the mechanics.
 *
 * Everything here is a pure function over paths: file reads, string math, no
 * subprocess, no clock, no store. Every rule reads a registry that already
 * exists somewhere else (vendored checksums, .gitignore, generated-code
 * headers, git config) — nothing in this file declares anything.
 */
#include "discover/role.h"

#include "discover/discover.h"
#include "foundation/compat_fs.h"
#include "foundation/platform.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    ROLE_LINE_MAX = 1024,
    ROLE_SHA256_HEX_LEN = 64,
    /* Ancestor walk bound. A path with more components than this is not a
     * repository anyone made on purpose. */
    ROLE_WALK_UP_MAX = 64,
    /* "generated" needs a strict majority of the sample marked. */
    ROLE_MAJORITY_FACTOR = 2,
    /* A7's "overwhelmingly": at least ROLE_OVERWHELM_NUM in _DEN marked. */
    ROLE_OVERWHELM_NUM = 9,
    ROLE_OVERWHELM_DEN = 10,
    ROLE_UPSTREAMS_INIT_CAP = 16,
    ROLE_GROW_FACTOR = 2,
    /* Grammar-manifest table columns (name | version | owner/repo | ...). */
    ROLE_COL_NAME_MAX = 128,
    ROLE_COL_NUM_MAX = 32,
};

/* The registries this module READS. These are relative locations inside a
 * repository that follows this project's conventions — naming a registry's
 * address is not enumerating its contents. */
static const char ROLE_VENDORED_MANIFEST[] = "scripts/vendored-checksums.txt";
static const char ROLE_PROVENANCE_AUDIT[] = "scripts/audit-license-provenance.py";
static const char ROLE_GRAMMAR_MANIFEST[] = "internal/hyp/vendored/grammars/MANIFEST.md";

/* Evidence is disclosure for a human, never parsed; truncation is fine. */
#define ROLE_EV(ev, cap, ...)                         \
    do {                                              \
        if ((ev) != NULL && (cap) > 0) {              \
            (void)snprintf((ev), (cap), __VA_ARGS__); \
        }                                             \
    } while (0)

/* ── Path helpers ────────────────────────────────────────────────── */

static bool role_join(char *out, size_t cap, const char *base, const char *rel) {
    int n = snprintf(out, cap, "%s/%s", base, rel);
    /* Truncation would make us reason about a DIFFERENT path; refuse. */
    return n > 0 && (size_t)n < cap;
}

/* Canonicalize, forward-slash, and strip trailing slashes. False when the
 * path does not exist — an unreadable tree yields no signals, by design. */
static bool role_canon(const char *path, char *out) {
    if (!path || !path[0]) {
        return false;
    }
    if (!hyp_canonical_path(path, out, HYP_ROLE_PATH_MAX)) {
        return false;
    }
    hyp_normalize_path_sep(out);
    size_t n = strlen(out);
    while (n > 1 && out[n - 1] == '/') {
        out[--n] = '\0';
    }
    return n > 0;
}

/* Truncate a canonical path to its parent in place. False at a root. */
static bool role_parent(char *buf) {
    char *slash = strrchr(buf, '/');
    if (!slash) {
        return false; /* "C:" or a bare name — nothing above */
    }
    if (slash == buf) {
        if (buf[1] == '\0') {
            return false; /* already "/" */
        }
        buf[1] = '\0'; /* "/x" -> "/" */
        return true;
    }
    *slash = '\0';
    return true;
}

static void role_rtrim(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
}

static const char *role_skip_blank(const char *p) {
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return p;
}

static bool role_is_abs(const char *p) {
    if (p[0] == '/' || p[0] == '\\') {
        return true;
    }
    return isalpha((unsigned char)p[0]) && p[1] == ':';
}

static bool role_resolve_relative(const char *base, const char *rel, char *out) {
    if (role_is_abs(rel)) {
        int n = snprintf(out, HYP_ROLE_PATH_MAX, "%s", rel);
        if (n <= 0 || n >= HYP_ROLE_PATH_MAX) {
            return false;
        }
    } else if (!role_join(out, HYP_ROLE_PATH_MAX, base, rel)) {
        return false;
    }
    hyp_normalize_path_sep(out);
    return true;
}

static bool role_read_first_line(const char *path, char *out, size_t cap) {
    FILE *f = hyp_fopen(path, "rb");
    if (!f) {
        return false;
    }
    bool ok = fgets(out, (int)cap, f) != NULL;
    fclose(f);
    if (ok) {
        role_rtrim(out);
    }
    return ok;
}

/* ── Wire names ──────────────────────────────────────────────────── */

const char *hyp_role_name(hyp_role_t role) {
    switch (role) {
    case HYP_ROLE_MEMBER:
        return "member";
    case HYP_ROLE_VENDORED:
        return "vendored";
    case HYP_ROLE_GENERATED:
        return "generated";
    case HYP_ROLE_UPSTREAM:
        return "upstream";
    case HYP_ROLE_REFERENCE:
        return "reference";
    }
    return "reference"; /* fail closed */
}

const char *hyp_role_source_name(hyp_role_source_t source) {
    switch (source) {
    case HYP_ROLE_SRC_DECLARED_TOML:
        return "declared-toml";
    case HYP_ROLE_SRC_DERIVED_VENDORED_REGISTRY:
        return "derived-vendored-registry";
    case HYP_ROLE_SRC_DERIVED_GITIGNORE:
        return "derived-gitignore";
    case HYP_ROLE_SRC_DERIVED_DO_NOT_EDIT:
        return "derived-do-not-edit";
    case HYP_ROLE_SRC_DERIVED_REMOTE:
        return "derived-remote";
    case HYP_ROLE_SRC_DEFAULT_REFERENCE:
        return "default-reference";
    }
    return "default-reference"; /* fail closed */
}

bool hyp_role_parse(const char *name, hyp_role_t *out) {
    static const hyp_role_t all[] = {HYP_ROLE_MEMBER, HYP_ROLE_VENDORED, HYP_ROLE_GENERATED,
                                     HYP_ROLE_UPSTREAM, HYP_ROLE_REFERENCE};
    if (!name || !out) {
        return false;
    }
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        if (strcmp(name, hyp_role_name(all[i])) == 0) {
            *out = all[i];
            return true;
        }
    }
    return false;
}

/* ── A2 — vendored, from scripts/vendored-checksums.txt ──────────── */

/* Parse one manifest line, `<sha256 hex><spaces><path>`, in place. Returns
 * the path, or NULL for a line that is not a checksum entry. */
static char *role_manifest_entry(char *line) {
    for (int i = 0; i < ROLE_SHA256_HEX_LEN; i++) {
        if (!isxdigit((unsigned char)line[i])) {
            return NULL;
        }
    }
    if (line[ROLE_SHA256_HEX_LEN] != ' ') {
        return NULL;
    }
    char *p = line + ROLE_SHA256_HEX_LEN;
    while (*p == ' ' || *p == '*') {
        p++; /* sha256sum writes two spaces; "* " marks binary mode */
    }
    role_rtrim(p);
    return *p ? p : NULL;
}

static bool role_manifest_covers(const char *manifest_path, const char *rel, char *ev,
                                 size_t ev_cap) {
    FILE *f = hyp_fopen(manifest_path, "rb");
    if (!f) {
        return false;
    }
    size_t rel_len = strlen(rel);
    char line[ROLE_LINE_MAX];
    bool covered = false;
    while (!covered && fgets(line, sizeof line, f)) {
        const char *entry = role_manifest_entry(line);
        if (!entry) {
            continue;
        }
        if (strncmp(entry, rel, rel_len) == 0 &&
            (entry[rel_len] == '/' || entry[rel_len] == '\0')) {
            ROLE_EV(ev, ev_cap, "%s covers %s (entry %s)", manifest_path, rel, entry);
            covered = true;
        }
    }
    fclose(f);
    return covered;
}

bool hyp_role_is_vendored(const char *repo_root, char *evidence, size_t evidence_cap) {
    char canon[HYP_ROLE_PATH_MAX];
    if (!role_canon(repo_root, canon)) {
        return false;
    }
    char anc[HYP_ROLE_PATH_MAX];
    memcpy(anc, canon, strlen(canon) + 1);
    /* Strictly above: a repository carrying the manifest is the DECLARER of
     * its vendored subtrees, never itself vendored by them. */
    for (int depth = 0; depth < ROLE_WALK_UP_MAX && role_parent(anc); depth++) {
        char manifest[HYP_ROLE_PATH_MAX];
        if (!role_join(manifest, sizeof manifest, anc, ROLE_VENDORED_MANIFEST)) {
            continue;
        }
        if (!hyp_file_exists(manifest)) {
            continue;
        }
        const char *rel = canon + strlen(anc);
        while (*rel == '/') {
            rel++;
        }
        if (*rel && role_manifest_covers(manifest, rel, evidence, evidence_cap)) {
            return true;
        }
    }
    return false;
}

/* ── A3, signal 1 — gitignored by the nearest enclosing repository ─ */

/* Nearest directory STRICTLY above canon that contains a .git entry
 * (directory or worktree file — either marks a repository root). */
static bool role_enclosing_repo(const char *canon, char *repo_out) {
    memcpy(repo_out, canon, strlen(canon) + 1);
    for (int depth = 0; depth < ROLE_WALK_UP_MAX && role_parent(repo_out); depth++) {
        char g[HYP_ROLE_PATH_MAX];
        if (role_join(g, sizeof g, repo_out, ".git") && hyp_file_exists(g)) {
            return true;
        }
    }
    return false;
}

/* Does dir/.gitignore ignore `rel` (a path below dir)? Each ancestor
 * component of rel is probed as a directory too, because ignoring a parent
 * ignores everything under it — exactly what a discovery walk would prune. */
static bool role_ignored_by_dir(const char *dir, const char *rel, char *ev, size_t ev_cap) {
    char gi_path[HYP_ROLE_PATH_MAX];
    if (!role_join(gi_path, sizeof gi_path, dir, ".gitignore")) {
        return false;
    }
    if (!hyp_file_exists(gi_path)) {
        return false;
    }
    hyp_gitignore_t *gi = hyp_gitignore_load(gi_path);
    if (!gi) {
        return false;
    }
    bool hit = false;
    char probe[HYP_ROLE_PATH_MAX];
    memcpy(probe, rel, strlen(rel) + 1);
    for (;;) {
        if (hyp_gitignore_matches(gi, probe, true)) {
            ROLE_EV(ev, ev_cap, "%s ignores %s", gi_path, probe);
            hit = true;
            break;
        }
        char *slash = strrchr(probe, '/');
        if (!slash) {
            break;
        }
        *slash = '\0';
    }
    hyp_gitignore_free(gi);
    return hit;
}

bool hyp_role_is_gitignored(const char *repo_root, char *evidence, size_t evidence_cap) {
    char canon[HYP_ROLE_PATH_MAX];
    if (!role_canon(repo_root, canon)) {
        return false;
    }
    char dir[HYP_ROLE_PATH_MAX];
    if (!role_enclosing_repo(canon, dir)) {
        return false;
    }
    /* Walk from the enclosing repository root down to canon's parent,
     * consulting each directory's .gitignore about the remainder. */
    for (;;) {
        const char *below = canon + strlen(dir);
        while (*below == '/') {
            below++;
        }
        if (!*below) {
            break; /* reached canon itself; its own .gitignore has no say */
        }
        if (role_ignored_by_dir(dir, below, evidence, evidence_cap)) {
            return true;
        }
        const char *slash = strchr(below, '/');
        if (!slash) {
            break; /* dir is canon's parent — the last voice there is */
        }
        size_t new_len = (size_t)(slash - canon);
        memcpy(dir, canon, new_len);
        dir[new_len] = '\0';
    }
    return false;
}

/* ── A3, signal 2 — the bounded DO-NOT-EDIT scan ─────────────────── */

/* Read the head of one file. False for unreadable or binary (NUL byte in the
 * head) files — those are outside the sample, not counted against it. On
 * true, *marked says whether the head carries a generated-code marker. */
static bool role_head_marked(const char *path, bool *marked) {
    FILE *f = hyp_fopen(path, "rb");
    if (!f) {
        return false;
    }
    char buf[HYP_ROLE_SCAN_HEAD_BYTES + 1];
    size_t n = fread(buf, 1, HYP_ROLE_SCAN_HEAD_BYTES, f);
    fclose(f);
    for (size_t i = 0; i < n; i++) {
        if (buf[i] == '\0') {
            return false;
        }
        buf[i] = (char)tolower((unsigned char)buf[i]);
    }
    buf[n] = '\0';
    *marked = strstr(buf, "do not edit") != NULL || strstr(buf, "@generated") != NULL;
    return true;
}

static void role_scan_walk(const char *dir, int depth, // NOLINT(misc-no-recursion)
                           hyp_role_scan_t *scan) {
    if (depth > HYP_ROLE_SCAN_MAX_DEPTH || scan->sampled >= HYP_ROLE_SCAN_MAX_FILES) {
        return;
    }
    hyp_dir_t *d = hyp_opendir(dir);
    if (!d) {
        return;
    }
    hyp_dirent_t *entry;
    while ((entry = hyp_readdir(d)) != NULL && scan->sampled < HYP_ROLE_SCAN_MAX_FILES) {
        if (strcmp(entry->name, ".") == 0 || strcmp(entry->name, "..") == 0 ||
            strcmp(entry->name, ".git") == 0) {
            continue;
        }
        char child[HYP_ROLE_PATH_MAX];
        if (!role_join(child, sizeof child, dir, entry->name)) {
            continue;
        }
        hyp_path_info_t info;
        if (hyp_path_info_utf8(child, &info) != 0 || info.is_symlink) {
            continue; /* never leave the tree through a link */
        }
        if (info.is_directory) {
            role_scan_walk(child, depth + 1, scan);
            continue;
        }
        if (!info.is_regular) {
            continue;
        }
        bool marked = false;
        if (!role_head_marked(child, &marked)) {
            continue;
        }
        scan->sampled++;
        if (marked) {
            scan->marked++;
        }
    }
    hyp_closedir(d);
}

bool hyp_role_scan_do_not_edit(const char *repo_root, hyp_role_scan_t *out) {
    if (!out) {
        return false;
    }
    out->sampled = 0;
    out->marked = 0;
    char canon[HYP_ROLE_PATH_MAX];
    if (!repo_root || !role_canon(repo_root, canon) || !hyp_is_dir(canon)) {
        return false;
    }
    role_scan_walk(canon, 0, out);
    return true;
}

bool hyp_role_scan_majority(const hyp_role_scan_t *scan) {
    return scan && scan->sampled > 0 && scan->marked * ROLE_MAJORITY_FACTOR > scan->sampled;
}

bool hyp_role_scan_overwhelming(const hyp_role_scan_t *scan) {
    return scan && scan->sampled > 0 &&
           scan->marked * ROLE_OVERWHELM_DEN >= scan->sampled * ROLE_OVERWHELM_NUM;
}

/* ── A4 — upstream, from the git remote ──────────────────────────── */

/* Locate the git config file for canon, following the worktree indirection:
 * .git may be a directory, or a file `gitdir: <path>` whose target may in
 * turn name a `commondir` where the shared config lives. Files only. */
static bool role_git_config_path(const char *canon, char *out) {
    char g[HYP_ROLE_PATH_MAX];
    if (!role_join(g, sizeof g, canon, ".git")) {
        return false;
    }
    hyp_path_info_t info;
    if (hyp_path_info_utf8(g, &info) != 0) {
        return false;
    }
    if (info.is_directory) {
        return role_join(out, HYP_ROLE_PATH_MAX, g, "config");
    }
    if (!info.is_regular) {
        return false;
    }
    char line[HYP_ROLE_PATH_MAX];
    if (!role_read_first_line(g, line, sizeof line)) {
        return false;
    }
    static const char PREFIX[] = "gitdir:";
    if (strncmp(line, PREFIX, sizeof PREFIX - 1) != 0) {
        return false;
    }
    const char *target = role_skip_blank(line + sizeof PREFIX - 1);
    char gitdir[HYP_ROLE_PATH_MAX];
    if (!*target || !role_resolve_relative(canon, target, gitdir)) {
        return false;
    }
    char cd_file[HYP_ROLE_PATH_MAX];
    if (role_join(cd_file, sizeof cd_file, gitdir, "commondir") && hyp_file_exists(cd_file)) {
        char cd[HYP_ROLE_PATH_MAX];
        char common[HYP_ROLE_PATH_MAX];
        if (!role_read_first_line(cd_file, cd, sizeof cd) ||
            !role_resolve_relative(gitdir, cd, common)) {
            return false;
        }
        return role_join(out, HYP_ROLE_PATH_MAX, common, "config");
    }
    return role_join(out, HYP_ROLE_PATH_MAX, gitdir, "config");
}

static bool role_config_origin_url(const char *config_path, char *url, size_t cap) {
    FILE *f = hyp_fopen(config_path, "rb");
    if (!f) {
        return false;
    }
    static const char SECTION[] = "[remote \"origin\"]";
    static const char KEY[] = "url";
    char line[ROLE_LINE_MAX];
    bool in_origin = false;
    bool found = false;
    while (!found && fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        role_rtrim(p);
        if (*p == '[') {
            in_origin = strcmp(p, SECTION) == 0;
            continue;
        }
        if (!in_origin || strncmp(p, KEY, sizeof KEY - 1) != 0) {
            continue;
        }
        const char *q = role_skip_blank(p + sizeof KEY - 1);
        if (*q != '=') {
            continue;
        }
        q = role_skip_blank(q + 1);
        if (*q) {
            (void)snprintf(url, cap, "%s", q);
            found = true;
        }
    }
    fclose(f);
    return found;
}

/* One or more '/'-separated nonempty segments of [a-z0-9._-]. */
static bool role_coord_valid(const char *coord) {
    bool slash = false;
    size_t seg = 0;
    for (const char *p = coord; *p; p++) {
        if (*p == '/') {
            if (seg == 0) {
                return false;
            }
            slash = true;
            seg = 0;
            continue;
        }
        if (!islower((unsigned char)*p) && !isdigit((unsigned char)*p) && *p != '.' && *p != '_' &&
            *p != '-') {
            return false;
        }
        seg++;
    }
    return slash && seg > 0;
}

static bool role_coord_from_url(const char *url, char *out, size_t cap) {
    static const char SCHEME[] = "://";
    static const char GIT_SUFFIX[] = ".git";
    const char *path = NULL;
    const char *scheme = strstr(url, SCHEME);
    if (scheme) {
        const char *slash = strchr(scheme + sizeof SCHEME - 1, '/');
        if (!slash) {
            return false;
        }
        path = slash + 1;
    } else {
        /* scp form, user@host:path. Anything else — a filesystem remote —
         * has no owner/name shape and is not classifiable: fail closed. */
        const char *at = strchr(url, '@');
        const char *colon = strchr(url, ':');
        if (!at || !colon || at > colon) {
            return false;
        }
        path = colon + 1;
    }
    while (*path == '/') {
        path++;
    }
    size_t n = strlen(path);
    while (n > 0 && path[n - 1] == '/') {
        n--;
    }
    if (n >= sizeof GIT_SUFFIX - 1 &&
        strncmp(path + n - (sizeof GIT_SUFFIX - 1), GIT_SUFFIX, sizeof GIT_SUFFIX - 1) == 0) {
        n -= sizeof GIT_SUFFIX - 1;
    }
    if (n == 0 || n > HYP_ROLE_COORD_MAX || n >= cap) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        out[i] = (char)tolower((unsigned char)path[i]);
    }
    out[n] = '\0';
    return role_coord_valid(out);
}

bool hyp_role_remote_coordinate(const char *repo_root, char *out, size_t out_cap) {
    if (!repo_root || !out || out_cap == 0) {
        return false;
    }
    char canon[HYP_ROLE_PATH_MAX];
    char config[HYP_ROLE_PATH_MAX];
    char url[ROLE_LINE_MAX];
    return role_canon(repo_root, canon) && role_git_config_path(canon, config) &&
           role_config_origin_url(config, url, sizeof url) &&
           role_coord_from_url(url, out, out_cap);
}

/* Case-insensitive ASCII compare, so a caller-supplied workspace origin
 * matches whatever case its config used. */
static bool role_coord_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == *b;
}

bool hyp_role_is_upstream(const char *repo_root, const hyp_role_ctx_t *ctx, char *evidence,
                          size_t evidence_cap) {
    if (!ctx || !ctx->known_upstreams) {
        return false;
    }
    char coord[HYP_ROLE_COORD_MAX + 1];
    if (!hyp_role_remote_coordinate(repo_root, coord, sizeof coord)) {
        return false;
    }
    if (ctx->workspace_origin && role_coord_eq(coord, ctx->workspace_origin)) {
        return false; /* another checkout of this project, not its upstream */
    }
    const hyp_role_upstreams_t *set = ctx->known_upstreams;
    for (size_t i = 0; i < set->count; i++) {
        if (strcmp(set->entries[i].coord, coord) == 0) {
            ROLE_EV(evidence, evidence_cap, "origin %s is a vendoring upstream recorded in %s",
                    coord, set->entries[i].origin_file);
            return true;
        }
    }
    return false;
}

/* ── The known-upstream set ──────────────────────────────────────── */

void hyp_role_upstreams_init(hyp_role_upstreams_t *set) {
    if (set) {
        set->entries = NULL;
        set->count = 0;
        set->capacity = 0;
    }
}

void hyp_role_upstreams_free(hyp_role_upstreams_t *set) {
    if (set) {
        free(set->entries);
        hyp_role_upstreams_init(set);
    }
}

static bool role_upstreams_push(hyp_role_upstreams_t *set, const char *coord,
                                const char *origin_file) {
    for (size_t i = 0; i < set->count; i++) {
        if (strcmp(set->entries[i].coord, coord) == 0) {
            return true; /* duplicate — collapsed, not an error */
        }
    }
    if (set->count == set->capacity) {
        size_t ncap = set->capacity ? set->capacity * ROLE_GROW_FACTOR : ROLE_UPSTREAMS_INIT_CAP;
        hyp_role_upstream_entry_t *grown = realloc(set->entries, ncap * sizeof *grown);
        if (!grown) {
            return false;
        }
        set->entries = grown;
        set->capacity = ncap;
    }
    hyp_role_upstream_entry_t *e = &set->entries[set->count];
    (void)snprintf(e->coord, sizeof e->coord, "%s", coord);
    (void)snprintf(e->origin_file, sizeof e->origin_file, "%s", origin_file);
    set->count++;
    return true;
}

/* Copy [start, end) lowercased into out and validate it as a coordinate. */
static bool role_coord_capture(const char *start, const char *end, char *out, size_t cap) {
    size_t n = (size_t)(end - start);
    if (n == 0 || n > HYP_ROLE_COORD_MAX || n >= cap) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        out[i] = (char)tolower((unsigned char)start[i]);
    }
    out[n] = '\0';
    return role_coord_valid(out);
}

/* One line of the provenance audit's Python source. Two shapes carry an
 * upstream coordinate, both reused verbatim from that file:
 *   LIBS entries:               "path": ("owner/name", ...)
 *   FORKS/DISAGREEMENT entries: "name": "owner/name",
 * Validation rejects every prose string that happens to sit after a colon —
 * a coordinate is [a-z0-9._-] segments only. */
static bool role_provenance_line_coord(const char *line, char *out, size_t cap) {
    static const char TUPLE_OPEN[] = "(\"";
    const char *colon = strchr(line, ':');
    if (!colon) {
        return false;
    }
    const char *open = strstr(colon, TUPLE_OPEN);
    const char *start;
    if (open) {
        start = open + sizeof TUPLE_OPEN - 1;
    } else {
        const char *q = role_skip_blank(colon + 1);
        if (*q != '"') {
            return false;
        }
        start = q + 1;
    }
    const char *end = strchr(start, '"');
    return end && role_coord_capture(start, end, out, cap);
}

/* One column of a Markdown table row: p points past a '|'; the trimmed cell
 * text lands in buf. Returns the position past the closing '|', or NULL. */
static const char *role_md_col(const char *p, char *buf, size_t cap) {
    const char *bar = strchr(p, '|');
    if (!bar) {
        return NULL;
    }
    p = role_skip_blank(p);
    const char *end = bar;
    while (end > p && (end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }
    size_t n = (size_t)(end - p);
    if (n >= cap) {
        return NULL;
    }
    memcpy(buf, p, n);
    buf[n] = '\0';
    return bar + 1;
}

/* One row of the grammar manifest's verified-upstream table:
 *   | name | <digits> | owner/repo | `sha` | verdict | ... |
 * The same three leading columns the provenance audit's own parser keys on. */
static bool role_grammar_row_coord(const char *line, char *out, size_t cap) {
    if (line[0] != '|') {
        return false;
    }
    char name[ROLE_COL_NAME_MAX];
    char num[ROLE_COL_NUM_MAX];
    char cell[HYP_ROLE_COORD_MAX + 1];
    const char *p = role_md_col(line + 1, name, sizeof name);
    if (!p || !name[0]) {
        return false;
    }
    p = role_md_col(p, num, sizeof num);
    if (!p || !num[0]) {
        return false;
    }
    for (const char *q = num; *q; q++) {
        if (!isdigit((unsigned char)*q)) {
            return false;
        }
    }
    p = role_md_col(p, cell, sizeof cell);
    if (!p || !cell[0]) {
        return false;
    }
    return role_coord_capture(cell, cell + strlen(cell), out, cap);
}

typedef bool (*role_line_coord_fn)(const char *line, char *out, size_t cap);

static bool role_upstreams_scan_file(hyp_role_upstreams_t *set, const char *repo_root,
                                     const char *rel, role_line_coord_fn parse) {
    char path[HYP_ROLE_PATH_MAX];
    if (!role_join(path, sizeof path, repo_root, rel)) {
        return true; /* unaddressable — contributes nothing */
    }
    FILE *f = hyp_fopen(path, "rb");
    if (!f) {
        return true; /* absent registry — contributes nothing, not an error */
    }
    bool ok = true;
    char line[ROLE_LINE_MAX];
    char coord[HYP_ROLE_COORD_MAX + 1];
    while (ok && fgets(line, sizeof line, f)) {
        if (parse(line, coord, sizeof coord)) {
            ok = role_upstreams_push(set, coord, rel);
        }
    }
    fclose(f);
    return ok;
}

bool hyp_role_upstreams_load(hyp_role_upstreams_t *set, const char *repo_root) {
    if (!set || !repo_root) {
        return false;
    }
    return role_upstreams_scan_file(set, repo_root, ROLE_PROVENANCE_AUDIT,
                                    role_provenance_line_coord) &&
           role_upstreams_scan_file(set, repo_root, ROLE_GRAMMAR_MANIFEST, role_grammar_row_coord);
}

/* ── The derivation, in precedence order ─────────────────────────── */

static void role_set(hyp_role_result_t *out, hyp_role_t role, hyp_role_source_t source) {
    out->role = role;
    out->source = source;
}

bool hyp_role_derive(const char *repo_root, const hyp_role_ctx_t *ctx, hyp_role_result_t *out) {
    if (!repo_root || !out) {
        return false;
    }
    /* A5 — the fail-closed floor. Set FIRST so every early return below an
     * error path still answers reference, never garbage. */
    role_set(out, HYP_ROLE_REFERENCE, HYP_ROLE_SRC_DEFAULT_REFERENCE);
    ROLE_EV(out->evidence, sizeof out->evidence,
            "no declaration and no derivation rule fired; read-only by default");

    if (ctx && ctx->declared) {
        hyp_role_t declared;
        if (hyp_role_parse(ctx->declared, &declared)) {
            role_set(out, declared, HYP_ROLE_SRC_DECLARED_TOML);
            ROLE_EV(out->evidence, sizeof out->evidence, "declared \"%s\"", ctx->declared);
        } else {
            /* Unknown role → reference, ALWAYS. Not even a derivation may
             * reinterpret a declaration this module cannot read. */
            ROLE_EV(out->evidence, sizeof out->evidence,
                    "declared role \"%s\" is not a role; fail closed to reference", ctx->declared);
        }
        return true;
    }
    if (hyp_role_is_vendored(repo_root, out->evidence, sizeof out->evidence)) {
        role_set(out, HYP_ROLE_VENDORED, HYP_ROLE_SRC_DERIVED_VENDORED_REGISTRY);
        return true;
    }
    if (hyp_role_is_gitignored(repo_root, out->evidence, sizeof out->evidence)) {
        role_set(out, HYP_ROLE_GENERATED, HYP_ROLE_SRC_DERIVED_GITIGNORE);
        return true;
    }
    hyp_role_scan_t scan;
    if (hyp_role_scan_do_not_edit(repo_root, &scan) && hyp_role_scan_majority(&scan)) {
        role_set(out, HYP_ROLE_GENERATED, HYP_ROLE_SRC_DERIVED_DO_NOT_EDIT);
        ROLE_EV(out->evidence, sizeof out->evidence,
                "%u of %u sampled file heads carry a DO-NOT-EDIT marker", scan.marked,
                scan.sampled);
        return true;
    }
    if (hyp_role_is_upstream(repo_root, ctx, out->evidence, sizeof out->evidence)) {
        role_set(out, HYP_ROLE_UPSTREAM, HYP_ROLE_SRC_DERIVED_REMOTE);
        return true;
    }
    return true; /* the A5 default set above stands */
}

/* ── A7 — the role audit ─────────────────────────────────────────── */

static void role_audit_add(hyp_role_contradiction_t *out, size_t cap, size_t *n,
                           hyp_role_contra_kind_t kind, const char *evidence) {
    if (out && *n < cap) {
        out[*n].kind = kind;
        (void)snprintf(out[*n].evidence, sizeof out[*n].evidence, "%s", evidence);
    }
    (*n)++; /* the true count keeps growing past the cap — a count is a floor */
}

size_t hyp_role_audit(const char *repo_root, const hyp_role_result_t *resolved,
                      const hyp_role_ctx_t *ctx, hyp_role_contradiction_t *out, size_t cap) {
    if (!repo_root || !resolved) {
        return 0;
    }
    if (resolved->role != HYP_ROLE_MEMBER) {
        /* Asymmetric by design: member is the only editable role, so it is
         * the only claim whose wrongness is a hazard rather than a nuisance. */
        return 0;
    }
    size_t n = 0;
    char ev[HYP_ROLE_EVIDENCE_MAX + 1];
    ev[0] = '\0';
    if (hyp_role_is_vendored(repo_root, ev, sizeof ev)) {
        role_audit_add(out, cap, &n, HYP_ROLE_CONTRA_MEMBER_VENDORED, ev);
    }
    hyp_role_scan_t scan;
    if (hyp_role_scan_do_not_edit(repo_root, &scan) && hyp_role_scan_overwhelming(&scan)) {
        (void)snprintf(ev, sizeof ev,
                       "declared member, but %u of %u sampled file heads carry a DO-NOT-EDIT "
                       "marker",
                       scan.marked, scan.sampled);
        role_audit_add(out, cap, &n, HYP_ROLE_CONTRA_MEMBER_DO_NOT_EDIT, ev);
    }
    ev[0] = '\0';
    if (hyp_role_is_upstream(repo_root, ctx, ev, sizeof ev)) {
        role_audit_add(out, cap, &n, HYP_ROLE_CONTRA_MEMBER_UPSTREAM, ev);
    }
    return n;
}
