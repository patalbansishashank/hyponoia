/*
 * integration_assets.c — Verified access to the shipped integration templates.
 *
 * See integration_assets.h for the architecture (why the bodies live in a
 * data file, the integrity model, and the ownership-reference role of the
 * content-addressed stored copy under <home>/.hyp/assets/<sha256>/).
 */
#include "cli/integration_assets.h"

#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/constants.h"
#include "foundation/platform.h"
#include "foundation/sha256.h"
#ifdef _WIN32
#include "foundation/win_utf8.h"
#endif
#include "yyjson/yyjson.h"

#include "hyp_integrations_hash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef _WIN32
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

enum {
    ASSETS_DIR_PERM = 0755,
    ASSETS_FILE_PERM = 0644,
    /* The shipped file is ~8 KB; the cap only bounds a corrupted candidate. */
    ASSETS_MAX_BYTES = HYP_SZ_64K * HYP_SZ_16,
};

/* The one actionable failure message: every "cannot use the templates" path
 * reports the same recovery, because the recovery is always the same. */
static const char assets_error_message[] =
    "integration assets missing or modified - reinstall from the release archive "
    "(expected " HYP_INTEGRATIONS_ASSET_NAME " next to the binary, in its package share "
    "directory, in $HYP_ASSETS_DIR, or under ~/.hyp/assets/" HYP_INTEGRATIONS_SHA256 ")";

typedef struct {
    const char *id;
    hyp_integration_template_t tpl;
    hyp_integration_body_t *released_storage;
} asset_entry_t;

/* Success-only cache: a failed resolve is re-attempted on every call so one
 * bad candidate never poisons a later call that could succeed. */
static struct {
    bool ok;
    char *bytes;
    size_t length;
    yyjson_doc *doc;
    asset_entry_t *entries;
    size_t entry_count;
} g_assets;

static void assets_fill_error(char *err, size_t err_sz) {
    if (err && err_sz > 0U) {
        (void)snprintf(err, err_sz, "%s", assets_error_message);
    }
}

static bool assets_exe_dir(char *out, size_t out_sz) {
    if (out_sz < HYP_SZ_2) {
        return false;
    }
#ifdef _WIN32
    char *module_path = hyp_module_path_utf8();
    size_t length = module_path ? strlen(module_path) : 0U;
    if (!module_path || length == 0U || length >= out_sz) {
        free(module_path);
        return false;
    }
    memcpy(out, module_path, length + 1U);
    free(module_path);
    hyp_normalize_path_sep(out);
#elif defined(__APPLE__)
    uint32_t self_sz = (uint32_t)out_sz;
    if (_NSGetExecutablePath(out, &self_sz) != 0) {
        return false;
    }
#else
    ssize_t length = readlink("/proc/self/exe", out, out_sz - SKIP_ONE);
    if (length <= 0 || (size_t)length >= out_sz - SKIP_ONE) {
        return false;
    }
    out[length] = '\0';
#endif
    char *slash = strrchr(out, '/');
    if (!slash || slash == out) {
        return false;
    }
    *slash = '\0';
    return true;
}

bool hyp_integration_assets_ownership_path(const char *home, char *out, size_t out_sz) {
    if (!home || !home[0]) {
        return false;
    }
    int written = snprintf(out, out_sz, "%s/.hyp/assets/%s/%s", home, HYP_INTEGRATIONS_SHA256,
                           HYP_INTEGRATIONS_ASSET_NAME);
    return written > 0 && (size_t)written < out_sz;
}

/* Read a candidate fully. Returns false for missing/unreadable/oversized. */
static bool assets_read_file(const char *path, char **out_bytes, size_t *out_length) {
    *out_bytes = NULL;
    *out_length = 0U;
    FILE *file = hyp_fopen(path, "rb");
    if (!file) {
        return false;
    }
    if (fseek(file, 0L, SEEK_END) != 0) {
        (void)fclose(file);
        return false;
    }
    long size = ftell(file);
    if (size <= 0 || size > (long)ASSETS_MAX_BYTES || fseek(file, 0L, SEEK_SET) != 0) {
        (void)fclose(file);
        return false;
    }
    char *bytes = malloc((size_t)size + SKIP_ONE);
    if (!bytes) {
        (void)fclose(file);
        return false;
    }
    size_t got = fread(bytes, 1U, (size_t)size, file);
    (void)fclose(file);
    if (got != (size_t)size) {
        free(bytes);
        return false;
    }
    bytes[got] = '\0';
    *out_bytes = bytes;
    *out_length = got;
    return true;
}

static bool assets_bytes_verified(const char *bytes, size_t length) {
    char hex[HYP_SHA256_HEX_LEN + SKIP_ONE];
    hyp_sha256_hex(bytes, length, hex);
    return strcmp(hex, HYP_INTEGRATIONS_SHA256) == 0;
}

bool hyp_integration_assets_verify_file(const char *path) {
    char *bytes = NULL;
    size_t length = 0U;
    bool verified =
        path && assets_read_file(path, &bytes, &length) && assets_bytes_verified(bytes, length);
    free(bytes);
    return verified;
}

static bool assets_parse_body(yyjson_val *value, hyp_integration_body_t *body) {
    yyjson_val *text = yyjson_is_obj(value) ? yyjson_obj_get(value, "text") : NULL;
    yyjson_val *bin = yyjson_is_obj(value) ? yyjson_obj_get(value, "bin") : NULL;
    if (!text || !yyjson_is_str(text) || (bin && !yyjson_is_str(bin))) {
        return false;
    }
    body->text = yyjson_get_str(text);
    body->bin_mode = bin ? yyjson_get_str(bin) : NULL;
    return true;
}

/* Parse verified bytes into the template table. The variant picked here is
 * the platform's ("windows" on _WIN32, else "posix", with "all" accepted for
 * platform-independent templates such as the JS adapters). */
static bool assets_adopt_bytes(char *bytes, size_t length) {
    yyjson_doc *doc = yyjson_read(bytes, length, 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *format = root && yyjson_is_obj(root) ? yyjson_obj_get(root, "format") : NULL;
    yyjson_val *templates = root && yyjson_is_obj(root) ? yyjson_obj_get(root, "templates") : NULL;
    if (!format || !yyjson_is_int(format) || yyjson_get_int(format) != 1 || !templates ||
        !yyjson_is_obj(templates)) {
        if (doc) {
            yyjson_doc_free(doc);
        }
        return false;
    }
#ifdef _WIN32
    static const char platform_variant[] = "windows";
#else
    static const char platform_variant[] = "posix";
#endif
    size_t template_count = yyjson_obj_size(templates);
    asset_entry_t *entries = calloc(template_count ? template_count : 1U, sizeof(*entries));
    if (!entries) {
        yyjson_doc_free(doc);
        return false;
    }
    size_t entry_count = 0U;
    size_t index;
    size_t max;
    yyjson_val *key;
    yyjson_val *value;
    bool parse_ok = true;
    yyjson_obj_foreach(templates, index, max, key, value) {
        if (!yyjson_is_str(key) || !yyjson_is_obj(value)) {
            parse_ok = false;
            break;
        }
        yyjson_val *variant = yyjson_obj_get(value, platform_variant);
        if (!variant) {
            variant = yyjson_obj_get(value, "all");
        }
        if (!variant) {
            /* Not offered for this platform: skip rather than reject, so a
             * future platform-scoped template does not brick the others. */
            continue;
        }
        asset_entry_t *entry = &entries[entry_count];
        entry->id = yyjson_get_str(key);
        if (!assets_parse_body(variant, &entry->tpl.current)) {
            parse_ok = false;
            break;
        }
        yyjson_val *tool_line = yyjson_obj_get(value, "tool_line");
        if (tool_line && !yyjson_is_str(tool_line)) {
            parse_ok = false;
            break;
        }
        entry->tpl.tool_line = tool_line ? yyjson_get_str(tool_line) : NULL;
        yyjson_val *released = yyjson_obj_get(value, "released");
        if (released) {
            if (!yyjson_is_arr(released)) {
                parse_ok = false;
                break;
            }
            size_t released_count = yyjson_arr_size(released);
            if (released_count > 0U) {
                entry->released_storage = calloc(released_count, sizeof(*entry->released_storage));
                if (!entry->released_storage) {
                    parse_ok = false;
                    break;
                }
                size_t released_index;
                size_t released_max;
                yyjson_val *released_value;
                size_t stored = 0U;
                yyjson_arr_foreach(released, released_index, released_max, released_value) {
                    if (!assets_parse_body(released_value, &entry->released_storage[stored])) {
                        parse_ok = false;
                        break;
                    }
                    stored++;
                }
                if (!parse_ok) {
                    entry_count++;
                    break;
                }
                entry->tpl.released = entry->released_storage;
                entry->tpl.released_count = stored;
            }
        }
        entry_count++;
    }
    if (!parse_ok) {
        for (size_t i = 0U; i < entry_count; i++) {
            free(entries[i].released_storage);
        }
        free(entries);
        yyjson_doc_free(doc);
        return false;
    }
    g_assets.bytes = bytes;
    g_assets.length = length;
    g_assets.doc = doc;
    g_assets.entries = entries;
    g_assets.entry_count = entry_count;
    g_assets.ok = true;
    return true;
}

/* Try one candidate path: read + hash-verify + parse. On success the bytes
 * are adopted into the cache (ownership transferred). */
static bool assets_try_candidate(const char *path) {
    char *bytes = NULL;
    size_t length = 0U;
    if (!assets_read_file(path, &bytes, &length)) {
        return false;
    }
    if (!assets_bytes_verified(bytes, length) || !assets_adopt_bytes(bytes, length)) {
        free(bytes);
        return false;
    }
    return true;
}

static bool assets_resolve(const char *home) {
    if (g_assets.ok) {
        return true;
    }
    /* HYP_ASSETS_DIR set = authoritative: an explicit override that is
     * missing or modified FAILS; silently falling back would hide exactly
     * the tampering the hash check exists to surface. */
    static const char env_missing[] = "\x1f"
                                      "HYP_ASSETS_DIR_MISSING"
                                      "\x1f";
    char env_buf[HYP_SZ_4K];
    char candidate[HYP_SZ_4K];
    const char *assets_dir =
        hyp_safe_getenv("HYP_ASSETS_DIR", env_buf, sizeof(env_buf), env_missing);
    if (!assets_dir) {
        return false; /* present but unrepresentable: fail, do not fall back */
    }
    if (strcmp(assets_dir, env_missing) != 0 && assets_dir[0]) {
        int written = snprintf(candidate, sizeof(candidate), "%s/%s", assets_dir,
                               HYP_INTEGRATIONS_ASSET_NAME);
        return written > 0 && (size_t)written < sizeof(candidate) &&
               assets_try_candidate(candidate);
    }
    char exe_dir[HYP_SZ_4K];
    if (assets_exe_dir(exe_dir, sizeof(exe_dir))) {
        int written =
            snprintf(candidate, sizeof(candidate), "%s/%s", exe_dir, HYP_INTEGRATIONS_ASSET_NAME);
        if (written > 0 && (size_t)written < sizeof(candidate) && assets_try_candidate(candidate)) {
            return true;
        }
        /* System package layout: <prefix>/bin/<binary> with the immutable
         * runtime asset under <prefix>/share/hyponoia/. */
        written = snprintf(candidate, sizeof(candidate), "%s/../share/hyponoia/%s",
                           exe_dir, HYP_INTEGRATIONS_ASSET_NAME);
        if (written > 0 && (size_t)written < sizeof(candidate) && assets_try_candidate(candidate)) {
            return true;
        }
        /* Source-tree layout: build/c/<binary> with assets/ at the root. */
        written = snprintf(candidate, sizeof(candidate), "%s/../../assets/%s", exe_dir,
                           HYP_INTEGRATIONS_ASSET_NAME);
        if (written > 0 && (size_t)written < sizeof(candidate) && assets_try_candidate(candidate)) {
            return true;
        }
    }
    return hyp_integration_assets_ownership_path(home, candidate, sizeof(candidate)) &&
           assets_try_candidate(candidate);
}

bool hyp_integration_assets_require(const char *home, char *err, size_t err_sz) {
    if (assets_resolve(home)) {
        return true;
    }
    assets_fill_error(err, err_sz);
    return false;
}

static bool assets_transaction_close(hyp_activation_transaction_t **transaction_io) {
    return !transaction_io || !*transaction_io ||
           hyp_activation_transaction_close(transaction_io) == HYP_ACTIVATION_TRANSACTION_OK;
}

static bool assets_stage_target(const char *target, hyp_activation_transaction_t **transaction_out,
                                char *err, size_t err_sz) {
    hyp_activation_transaction_status_t status = hyp_activation_transaction_stage_bytes(
        target, g_assets.bytes, g_assets.length, transaction_out);
    if (status == HYP_ACTIVATION_TRANSACTION_OK && *transaction_out) {
        return true;
    }
    if (err && err_sz > 0U) {
        (void)snprintf(err, err_sz, "cannot stage the verified %s copy for %s: %s",
                       HYP_INTEGRATIONS_ASSET_NAME, target,
                       hyp_activation_transaction_status_message(status));
    }
    (void)assets_transaction_close(transaction_out);
    return false;
}

static bool assets_install_validator(const char *target_path, void *context) {
    (void)context;
    return hyp_integration_assets_verify_file(target_path);
}

hyp_activation_transaction_status_t hyp_integration_assets_commit_install(
    hyp_activation_transaction_t *transaction) {
    if (!transaction) {
        return HYP_ACTIVATION_TRANSACTION_INVALID_ARGUMENT;
    }
    hyp_activation_transaction_status_t status =
        hyp_activation_transaction_commit(transaction, assets_install_validator, NULL);
#ifndef _WIN32
    if (status == HYP_ACTIVATION_TRANSACTION_OK) {
        const char *target = hyp_activation_transaction_target_path(transaction);
        if (!target || chmod(target, ASSETS_FILE_PERM) != 0) {
            hyp_activation_transaction_status_t rollback =
                hyp_activation_transaction_rollback(transaction);
            return rollback == HYP_ACTIVATION_TRANSACTION_OK
                       ? HYP_ACTIVATION_TRANSACTION_IO
                       : HYP_ACTIVATION_TRANSACTION_ROLLBACK_FAILED;
        }
    }
#endif
    return status;
}

hyp_activation_transaction_status_t hyp_integration_assets_commit_removal(
    hyp_activation_transaction_t *transaction) {
    if (!transaction) {
        return HYP_ACTIVATION_TRANSACTION_INVALID_ARGUMENT;
    }
    hyp_activation_transaction_status_t status =
        hyp_activation_transaction_commit(transaction, NULL, NULL);
    const char *retained = hyp_activation_transaction_backup_path(transaction);
    if (status != HYP_ACTIVATION_TRANSACTION_OK) {
        return status;
    }
    if (retained && hyp_integration_assets_verify_file(retained)) {
        return HYP_ACTIVATION_TRANSACTION_OK;
    }
    hyp_activation_transaction_status_t rollback = hyp_activation_transaction_rollback(transaction);
    return rollback == HYP_ACTIVATION_TRANSACTION_OK ? HYP_ACTIVATION_TRANSACTION_VALIDATION_FAILED
                                                     : HYP_ACTIVATION_TRANSACTION_ROLLBACK_FAILED;
}

bool hyp_integration_assets_stage_install(const char *home, const char *install_dir,
                                          hyp_activation_transaction_t **adjacent_transaction_out,
                                          hyp_activation_transaction_t **ownership_transaction_out,
                                          char *err, size_t err_sz) {
    if (adjacent_transaction_out) {
        *adjacent_transaction_out = NULL;
    }
    if (ownership_transaction_out) {
        *ownership_transaction_out = NULL;
    }
    if (!home || !home[0] || !install_dir || !install_dir[0] || !adjacent_transaction_out ||
        !ownership_transaction_out || !hyp_integration_assets_require(home, err, err_sz)) {
        if ((!home || !home[0] || !install_dir || !install_dir[0]) && err && err_sz > 0U) {
            (void)snprintf(err, err_sz, "integration asset install paths are unavailable");
        }
        return false;
    }

    char adjacent[HYP_SZ_4K];
    char ownership[HYP_SZ_4K];
    char ownership_parent[HYP_SZ_4K];
    int adjacent_length =
        snprintf(adjacent, sizeof(adjacent), "%s/%s", install_dir, HYP_INTEGRATIONS_ASSET_NAME);
    if (adjacent_length <= 0 || (size_t)adjacent_length >= sizeof(adjacent) ||
        !hyp_integration_assets_ownership_path(home, ownership, sizeof(ownership))) {
        assets_fill_error(err, err_sz);
        return false;
    }
    (void)snprintf(ownership_parent, sizeof(ownership_parent), "%s", ownership);
    char *slash = strrchr(ownership_parent, '/');
    if (!slash) {
        assets_fill_error(err, err_sz);
        return false;
    }
    *slash = '\0';
    if (!hyp_mkdir_p(install_dir, ASSETS_DIR_PERM) ||
        !hyp_mkdir_p(ownership_parent, ASSETS_DIR_PERM)) {
        if (err && err_sz > 0U) {
            (void)snprintf(err, err_sz, "cannot create directories for the verified %s copies",
                           HYP_INTEGRATIONS_ASSET_NAME);
        }
        return false;
    }
    if (!assets_stage_target(adjacent, adjacent_transaction_out, err, err_sz) ||
        !assets_stage_target(ownership, ownership_transaction_out, err, err_sz)) {
        bool owner_closed = assets_transaction_close(ownership_transaction_out);
        bool adjacent_closed = assets_transaction_close(adjacent_transaction_out);
        if ((!owner_closed || !adjacent_closed) && err && err_sz > 0U) {
            (void)snprintf(err, err_sz,
                           "cannot clean up a partial integration asset install transaction");
        }
        return false;
    }
    return true;
}

static bool assets_stage_owned_removal(const char *path,
                                       hyp_activation_transaction_t **transaction_out,
                                       bool *foreign_preserved_out, char *err, size_t err_sz) {
    hyp_path_info_t info;
    if (hyp_path_info_utf8(path, &info) != 0) {
        return true;
    }
    if (!info.is_regular || info.is_symlink || !hyp_integration_assets_verify_file(path)) {
        if (foreign_preserved_out) {
            *foreign_preserved_out = true;
        }
        return true;
    }
    hyp_activation_transaction_status_t status =
        hyp_activation_transaction_stage_removal(path, transaction_out);
    if (status == HYP_ACTIVATION_TRANSACTION_OK && *transaction_out) {
        /* stage_removal snapshots the exact file identity; commit revalidates
         * it, binding this digest check to the file that will be retained. */
        if (hyp_integration_assets_verify_file(path)) {
            return true;
        }
        if (!assets_transaction_close(transaction_out)) {
            if (err && err_sz > 0U) {
                (void)snprintf(err, err_sz,
                               "cannot clean up a changed integration asset removal transaction");
            }
            return false;
        }
        if (foreign_preserved_out) {
            *foreign_preserved_out = true;
        }
        return true;
    }
    if (err && err_sz > 0U) {
        (void)snprintf(err, err_sz, "cannot stage owned integration asset removal for %s: %s", path,
                       hyp_activation_transaction_status_message(status));
    }
    (void)assets_transaction_close(transaction_out);
    return false;
}

bool hyp_integration_assets_stage_remove(const char *home, const char *install_dir,
                                         hyp_activation_transaction_t **adjacent_transaction_out,
                                         hyp_activation_transaction_t **ownership_transaction_out,
                                         bool *foreign_preserved_out, char *err, size_t err_sz) {
    if (adjacent_transaction_out) {
        *adjacent_transaction_out = NULL;
    }
    if (ownership_transaction_out) {
        *ownership_transaction_out = NULL;
    }
    if (foreign_preserved_out) {
        *foreign_preserved_out = false;
    }
    if (!home || !home[0] || !install_dir || !install_dir[0] || !adjacent_transaction_out ||
        !ownership_transaction_out) {
        if (err && err_sz > 0U) {
            (void)snprintf(err, err_sz, "integration asset uninstall paths are unavailable");
        }
        return false;
    }
    char adjacent[HYP_SZ_4K];
    char ownership[HYP_SZ_4K];
    int adjacent_length =
        snprintf(adjacent, sizeof(adjacent), "%s/%s", install_dir, HYP_INTEGRATIONS_ASSET_NAME);
    if (adjacent_length <= 0 || (size_t)adjacent_length >= sizeof(adjacent) ||
        !hyp_integration_assets_ownership_path(home, ownership, sizeof(ownership))) {
        if (err && err_sz > 0U) {
            (void)snprintf(err, err_sz, "integration asset uninstall paths are too long");
        }
        return false;
    }
    if (!assets_stage_owned_removal(adjacent, adjacent_transaction_out, foreign_preserved_out, err,
                                    err_sz) ||
        !assets_stage_owned_removal(ownership, ownership_transaction_out, foreign_preserved_out,
                                    err, err_sz)) {
        bool owner_closed = assets_transaction_close(ownership_transaction_out);
        bool adjacent_closed = assets_transaction_close(adjacent_transaction_out);
        if ((!owner_closed || !adjacent_closed) && err && err_sz > 0U) {
            (void)snprintf(err, err_sz,
                           "cannot clean up a partial integration asset removal transaction");
        }
        return false;
    }
    return true;
}

#ifdef HYP_CLI_ENABLE_TEST_API
bool hyp_integration_assets_install(const char *home, bool dry_run, char *err, size_t err_sz) {
    if (!home || !home[0] || !hyp_integration_assets_require(home, err, err_sz)) {
        if (!home || !home[0]) {
            assets_fill_error(err, err_sz);
        }
        return false;
    }
    if (dry_run) {
        return true;
    }
    char stored[HYP_SZ_4K];
    char parent[HYP_SZ_4K];
    if (!hyp_integration_assets_ownership_path(home, stored, sizeof(stored))) {
        assets_fill_error(err, err_sz);
        return false;
    }
    (void)snprintf(parent, sizeof(parent), "%s", stored);
    char *slash = strrchr(parent, '/');
    if (!slash) {
        assets_fill_error(err, err_sz);
        return false;
    }
    *slash = '\0';
    if (!hyp_mkdir_p(parent, ASSETS_DIR_PERM)) {
        if (err && err_sz > 0U) {
            (void)snprintf(err, err_sz, "cannot create %s to store the verified %s copy", parent,
                           HYP_INTEGRATIONS_ASSET_NAME);
        }
        return false;
    }
    hyp_activation_transaction_t *transaction = NULL;
    if (!assets_stage_target(stored, &transaction, err, err_sz)) {
        return false;
    }
    hyp_activation_transaction_status_t status = hyp_integration_assets_commit_install(transaction);
    if (status != HYP_ACTIVATION_TRANSACTION_OK) {
        (void)assets_transaction_close(&transaction);
        if (err && err_sz > 0U) {
            (void)snprintf(err, err_sz, "cannot publish the verified %s copy to %s",
                           HYP_INTEGRATIONS_ASSET_NAME, stored);
        }
        return false;
    }
    status = hyp_activation_transaction_finalize(transaction);
    bool finalized =
        status == HYP_ACTIVATION_TRANSACTION_OK || status == HYP_ACTIVATION_TRANSACTION_DEFERRED;
    bool closed = hyp_activation_transaction_close(&transaction) == HYP_ACTIVATION_TRANSACTION_OK;
    if (!finalized || !closed) {
        if (err && err_sz > 0U) {
            (void)snprintf(err, err_sz, "cannot finalize the verified %s copy at %s",
                           HYP_INTEGRATIONS_ASSET_NAME, stored);
        }
        return false;
    }
    return true;
}
#endif

const hyp_integration_template_t *hyp_integration_template(const char *id) {
    if (!id || !assets_resolve(NULL)) {
        return NULL;
    }
    for (size_t i = 0U; i < g_assets.entry_count; i++) {
        if (strcmp(g_assets.entries[i].id, id) == 0) {
            return &g_assets.entries[i].tpl;
        }
    }
    return NULL;
}

#ifdef HYP_CLI_ENABLE_TEST_API
void hyp_integration_assets_reset_for_testing(void) {
    for (size_t i = 0U; i < g_assets.entry_count; i++) {
        free(g_assets.entries[i].released_storage);
    }
    free(g_assets.entries);
    g_assets.entries = NULL;
    g_assets.entry_count = 0U;
    if (g_assets.doc) {
        yyjson_doc_free(g_assets.doc);
        g_assets.doc = NULL;
    }
    free(g_assets.bytes);
    g_assets.bytes = NULL;
    g_assets.length = 0U;
    g_assets.ok = false;
}
#endif
