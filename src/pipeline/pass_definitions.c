/*
 * pass_definitions.c — Extract definitions from source files.
 *
 * For each discovered file:
 *   1. Read source content from disk
 *   2. Call hyp_extract_file() to get defs, calls, imports
 *   3. Create Function/Class/Method/Variable/Module nodes in graph buffer
 *   4. Register callables in the function registry
 *   5. Store import maps and call sites for later passes
 *
 * Depends on: extraction layer (hyp.h), graph_buffer, pipeline internals
 */
#include "foundation/constants.h"

enum { PD_RING = 4, PD_RING_MASK = 3, PD_JSON_MARGIN = 10, PD_ESC_MARGIN = 3, PD_ESC_SPACE = 2 };
/* Fixed bytes around a serialized JSON field: ,"key":"value" / ,"key":[...]
 * -> comma + 2 key quotes + colon + 2 value quotes (resp. brackets). */
enum { PD_JSON_FIELD_OVERHEAD = 6 };
#include "pipeline/pipeline.h"
#include <stdint.h>
#include "pipeline/pipeline_internal.h"
#include "pipeline/pass_workspace_calls.h"
#include "graph_buffer/graph_buffer.h"
#include "foundation/log.h"
#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/limits.h"
#include "hyp.h"
#include "arena.h"
#include "iris_export_xml.h"
#include "simhash/minhash.h"
#include "semantic/ast_profile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Read entire file into heap-allocated buffer. Returns NULL on error.
 * Caller must free(). Sets *out_len to byte count. *out_size receives the
 * on-disk size and *out_status the failure reason, so the caller can attribute
 * a skip to the right phase/reason (read vs oversized) instead of a silent
 * drop. Both out params may be NULL. */
static char *read_file(const char *path, int *out_len, long *out_size,
                       hyp_read_status_t *out_status) {
    if (out_size) {
        *out_size = 0;
    }
    if (out_status) {
        *out_status = HYP_READ_OK;
    }
    FILE *f = hyp_fopen(path, "rb");
    if (!f) {
        if (out_status) {
            *out_status = HYP_READ_OPEN_FAIL;
        }
        return NULL;
    }

    (void)fseek(f, 0, SEEK_END);
    long size = ftell(f);
    (void)fseek(f, 0, SEEK_SET);
    if (out_size) {
        *out_size = size;
    }

    if (size <= 0) {
        (void)fclose(f);
        if (out_status) {
            *out_status = HYP_READ_EMPTY;
        }
        return NULL;
    }
    if (size > hyp_max_file_bytes()) { /* generous, env-configurable cap (B4) */
        (void)fclose(f);
        if (out_status) {
            *out_status = HYP_READ_OVERSIZED;
        }
        return NULL;
    }

    /* +16 padding: tree-sitter's lexer peeks a few bytes past the final UTF-8
     * character when computing lookahead, reading beyond the logical end.
     * Over-allocate and zero the tail so that read stays in-bounds (ASan
     * flags it as a heap-buffer-overflow otherwise; harmless but real UB). */
    enum { HYP_TS_LOOKAHEAD_PAD = 16 };
    char *buf = malloc((size_t)size + HYP_TS_LOOKAHEAD_PAD);
    if (!buf) {
        (void)fclose(f);
        if (out_status) {
            *out_status = HYP_READ_OOM;
        }
        return NULL;
    }

    size_t nread = fread(buf, SKIP_ONE, size, f);
    (void)fclose(f);

    if (nread > (size_t)size) {
        nread = (size_t)size;
    }
    memset(buf + nread, 0, HYP_TS_LOOKAHEAD_PAD);
    *out_len = (int)nread;
    return buf;
}

/* Format int to string for logging. Thread-safe via TLS. */
static const char *itoa_log(int val) {
    static HYP_TLS char bufs[PD_RING][HYP_SZ_32];
    static HYP_TLS int idx = 0;
    int i = idx;
    idx = (idx + SKIP_ONE) & PD_RING_MASK;
    snprintf(bufs[i], sizeof(bufs[i]), "%d", val);
    return bufs[i];
}

/* Append a JSON-escaped string value to buf at position *pos.
 * Writes: ,"key":"escaped_value"
 * Handles: \, ", \n, \r, \t */
static int def_json_escape_char(char *buf, size_t avail, char ch) {
    char esc = 0;
    switch (ch) {
    case '"':
        esc = '"';
        break;
    case '\\':
        esc = '\\';
        break;
    case '\n':
        esc = 'n';
        break;
    case '\r':
        esc = 'r';
        break;
    case '\t':
        esc = 't';
        break;
    default:
        if (avail >= SKIP_ONE) {
            /* Any other raw control byte (e.g. form feed) is invalid inside a
             * JSON string — degrade to a space. */
            buf[0] = ((unsigned char)ch < 0x20) ? ' ' : ch;
        }
        return SKIP_ONE;
    }
    if (avail >= PD_ESC_SPACE) {
        buf[0] = '\\';
        buf[SKIP_ONE] = esc;
    }
    return PD_ESC_SPACE;
}

/* Escaped length of a string under def_json_escape_char's rules: escaped
 * characters expand to 2 bytes, everything else stays 1. */
static size_t def_json_escaped_len(const char *s) {
    size_t n = 0;
    for (; *s; s++) {
        switch (*s) {
        case '"':
        case '\\':
        case '\n':
        case '\r':
        case '\t':
            n += PD_ESC_SPACE;
            break;
        default:
            n += SKIP_ONE;
        }
    }
    return n;
}

/* Appends are ATOMIC: a field is emitted only if the WHOLE serialized form
 * fits (with PD_ESC_SPACE bytes reserved for the closing '}' + NUL). Cutting a
 * field mid-value produced unterminated strings/arrays — malformed properties
 * JSON that aborts every json_extract()-based consumer downstream (seen on the
 * Linux kernel: 50-param functions truncated at the 2 KB cap). Dropping an
 * oversized optional field whole keeps the JSON valid. */
static void append_json_string(char *buf, size_t bufsize, size_t *pos, const char *key,
                               const char *val) {
    if (!val || val[0] == '\0') {
        return;
    }
    /* ,"key":"<escaped>" — comma + 2 key quotes + colon + 2 value quotes */
    size_t required = strlen(key) + def_json_escaped_len(val) + PD_JSON_FIELD_OVERHEAD;
    if (*pos + required + PD_ESC_SPACE > bufsize) {
        return; /* whole field would not fit — skip it atomically */
    }
    size_t p = *pos;
    int w = snprintf(buf + p, bufsize - p, ",\"%s\":\"", key);
    if (w <= 0 || (size_t)w >= bufsize - p) {
        return;
    }
    p += (size_t)w;
    for (const char *s = val; *s && p < bufsize - PD_ESC_MARGIN; s++) {
        p += (size_t)def_json_escape_char(buf + p, bufsize - p - PD_ESC_SPACE, *s);
    }
    if (p < bufsize - SKIP_ONE) {
        buf[p++] = '"';
    }
    buf[p] = '\0';
    *pos = p;
}

/* Append a JSON array of strings: ,"key":["a","b","c"]. Atomic like
 * append_json_string: emitted only if the whole array fits. */
static void append_json_str_array(char *buf, size_t bufsize, size_t *pos, const char *key,
                                  const char **arr) {
    if (!arr || !arr[0] || *pos >= bufsize - PD_JSON_MARGIN) {
        return;
    }
    /* ,"key":[ + per item "<escaped>" + separating commas + ] */
    size_t required = strlen(key) + PD_JSON_FIELD_OVERHEAD;
    for (int i = 0; arr[i]; i++) {
        required += def_json_escaped_len(arr[i]) + PD_ESC_SPACE + (i > 0 ? SKIP_ONE : 0);
    }
    if (*pos + required + PD_ESC_SPACE > bufsize) {
        return; /* whole array would not fit — skip it atomically */
    }
    size_t p = *pos;
    int n = snprintf(buf + p, bufsize - p, ",\"%s\":[", key);
    if (n <= 0 || p + (size_t)n >= bufsize - PD_ESC_SPACE) {
        return;
    }
    p += (size_t)n;
    for (int i = 0; arr[i]; i++) {
        if (i > 0 && p < bufsize - SKIP_ONE) {
            buf[p++] = ',';
        }
        if (p < bufsize - SKIP_ONE) {
            buf[p++] = '"';
        }
        /* Full escaping (not just quote/backslash): items like C param types
         * sliced from multi-line declarations carry raw \n/\t bytes, which are
         * invalid inside JSON strings. */
        for (const char *s = arr[i]; *s && p < bufsize - PD_ESC_SPACE; s++) {
            p += (size_t)def_json_escape_char(buf + p, bufsize - p - PD_ESC_SPACE, *s);
        }
        if (p < bufsize - SKIP_ONE) {
            buf[p++] = '"';
        }
    }
    if (p < bufsize - SKIP_ONE) {
        buf[p++] = ']';
    }
    buf[p] = '\0';
    *pos = p;
}

/* Build properties JSON for a definition node. */
static void build_def_props(char *buf, size_t bufsize, const HYPDefinition *def) {
    /* The complexity/loop/recursion metrics are only meaningful for executable
     * units (Function/Method). Emitting them on the millions of Macro/Field/
     * Variable/Class/Enum nodes — where they are always zero — bloats every
     * node's properties (~150 B), inflating RAM, the gbuf merge copy and the
     * dump. Gate the block to functions; other labels keep the lean base. */
    const bool is_fn =
        def->label && (strcmp(def->label, "Function") == 0 || strcmp(def->label, "Method") == 0);
    int n;
    if (is_fn) {
        n = snprintf(buf, bufsize,
                     "{\"complexity\":%d,\"cognitive\":%d,\"loop_count\":%d,\"loop_depth\":%d,"
                     "\"self_recursive\":%s,\"param_count\":%d,\"max_access_depth\":%d,"
                     "\"linear_scan_in_loop\":%d,\"alloc_in_loop\":%d,\"recursion_in_loop\":%s,"
                     "\"unguarded_recursion\":%s,"
                     "\"lines\":%d,\"is_exported\":%s,\"is_test\":%s,\"is_entry_point\":%s",
                     def->complexity, def->cognitive, def->loop_count, def->loop_depth,
                     def->is_recursive ? "true" : "false", def->param_count, def->max_access_depth,
                     def->linear_scan_in_loop, def->alloc_in_loop,
                     def->recursion_in_loop ? "true" : "false",
                     def->unguarded_recursion ? "true" : "false", def->lines,
                     def->is_exported ? "true" : "false", def->is_test ? "true" : "false",
                     def->is_entry_point ? "true" : "false");
    } else {
        n = snprintf(buf, bufsize,
                     "{\"complexity\":%d,\"lines\":%d,\"is_exported\":%s,\"is_test\":%s,"
                     "\"is_entry_point\":%s",
                     def->complexity, def->lines, def->is_exported ? "true" : "false",
                     def->is_test ? "true" : "false", def->is_entry_point ? "true" : "false");
    }

    if (n <= 0 || (size_t)n >= bufsize) {
        buf[0] = '\0';
        return;
    }
    size_t pos = (size_t)n;
    append_json_string(buf, bufsize, &pos, "docstring", def->docstring);
    append_json_string(buf, bufsize, &pos, "signature", def->signature);
    append_json_string(buf, bufsize, &pos, "return_type", def->return_type);
    append_json_string(buf, bufsize, &pos, "parent_class", def->parent_class);
    append_json_str_array(buf, bufsize, &pos, "decorators", def->decorators);
    append_json_str_array(buf, bufsize, &pos, "base_classes", def->base_classes);
    append_json_str_array(buf, bufsize, &pos, "param_names", def->param_names);
    append_json_str_array(buf, bufsize, &pos, "param_types", def->param_types);
    append_json_string(buf, bufsize, &pos, "route_path", def->route_path);
    append_json_string(buf, bufsize, &pos, "route_method", def->route_method);

    /* MinHash fingerprint — append if present and buffer has room. */
    if (def->fingerprint && def->fingerprint_k > 0 &&
        pos + HYP_MINHASH_HEX_LEN + HYP_MINHASH_JSON_OVERHEAD < bufsize) {
        char fp_hex[HYP_MINHASH_HEX_BUF];
        hyp_minhash_to_hex((const hyp_minhash_t *)def->fingerprint, fp_hex, sizeof(fp_hex));
        append_json_string(buf, bufsize, &pos, "fp", fp_hex);
    }

    /* AST structural profile */
    if (def->structural_profile && pos + HYP_AST_PROFILE_BUF < bufsize) {
        append_json_string(buf, bufsize, &pos, "sp", def->structural_profile);
    }

    /* Body tokens */
    if (def->body_tokens && pos + HYP_SZ_512 < bufsize) {
        append_json_string(buf, bufsize, &pos, "bt", def->body_tokens);
    }

    if (pos < bufsize - SKIP_ONE) {
        buf[pos] = '}';
        buf[pos + SKIP_ONE] = '\0';
    }
}

/* Process one definition: create node, register, DEFINES + DEFINES_METHOD edges. */
static void process_def(hyp_pipeline_ctx_t *ctx, const HYPDefinition *def, const char *rel) {
    if (!def->qualified_name || !def->name) {
        return;
    }
    char props[HYP_SZ_2K];
    build_def_props(props, sizeof(props), def);
    int64_t node_id = hyp_gbuf_upsert_node(
        ctx->gbuf, def->label ? def->label : "Function", def->name, def->qualified_name,
        def->file_path ? def->file_path : rel, (int)def->start_line, (int)def->end_line, props);
    /* Register callable symbols + every type-like container (Class/Struct/
     * Interface/Enum/Type/Trait). Type-like defs must be in the registry so
     * `class Foo : IBar` (INHERITS), `impl Trait for S` (IMPLEMENTS), and method/
     * field resolution can reach them — Struct included so Rust/Go/Swift/D structs
     * resolve as type targets just as a Class did. Variable/Field defs are also
     * registered so pass_usages.c can resolve READS/WRITES accesses (rw->var_name)
     * to a Variable/Field node QN.
     * KEEP IN SYNC with pass_parallel.c and pipeline_incremental.c's seed sets. */
    if (node_id > 0 && def->label &&
        (strcmp(def->label, "Function") == 0 || strcmp(def->label, "Method") == 0 ||
         hyp_label_is_type_like(def->label) || strcmp(def->label, "Variable") == 0 ||
         strcmp(def->label, "Field") == 0)) {
        hyp_registry_add(ctx->registry, def->name, def->qualified_name, def->label);
    }
    char *file_qn = hyp_pipeline_fqn_compute(ctx->project_name, rel, "__file__");
    const hyp_gbuf_node_t *file_node = hyp_gbuf_find_by_qn(ctx->gbuf, file_qn);
    if (file_node && node_id > 0) {
        hyp_gbuf_insert_edge(ctx->gbuf, file_node->id, node_id, "DEFINES", "{}");
    }
    free(file_qn);
    if (def->parent_class && def->label && strcmp(def->label, "Method") == 0) {
        const hyp_gbuf_node_t *parent = hyp_gbuf_find_by_qn(ctx->gbuf, def->parent_class);
        if (parent && node_id > 0) {
            hyp_gbuf_insert_edge(ctx->gbuf, parent->id, node_id, "DEFINES_METHOD", "{}");
        }
    }
}

/* Create Channel nodes + EMITS / LISTENS_ON edges for one file's channels.
 * Mirrors the parallel path in hyp_build_registry_from_cache — keep in sync. */
/* Find the source node for a channel edge: enclosing function or file node. */
static const hyp_gbuf_node_t *find_channel_source(hyp_pipeline_ctx_t *ctx, const HYPChannel *ch,
                                                  const char *rel) {
    const hyp_gbuf_node_t *node = NULL;
    if (ch->enclosing_func_qn && ch->enclosing_func_qn[0]) {
        node = hyp_gbuf_find_by_qn(ctx->gbuf, ch->enclosing_func_qn);
    }
    if (!node) {
        char *file_qn = hyp_pipeline_fqn_compute(ctx->project_name, rel, "__file__");
        node = hyp_gbuf_find_by_qn(ctx->gbuf, file_qn);
        free(file_qn);
    }
    return node;
}

static void create_channel_edges_for_file(hyp_pipeline_ctx_t *ctx, const HYPFileResult *result,
                                          const char *rel) {
    for (int j = 0; j < result->channels.count; j++) {
        const HYPChannel *ch = &result->channels.items[j];
        if (!ch->channel_name || !ch->channel_name[0]) {
            continue;
        }
        char channel_qn[HYP_SZ_512];
        snprintf(channel_qn, sizeof(channel_qn), "__channel__%s__%s",
                 ch->transport ? ch->transport : "unknown", ch->channel_name);
        char channel_props[HYP_SZ_512];
        snprintf(channel_props, sizeof(channel_props), "{\"transport\":\"%s\",\"name\":\"%s\"}",
                 ch->transport ? ch->transport : "unknown", ch->channel_name);
        int64_t channel_id = hyp_gbuf_upsert_node(ctx->gbuf, "Channel", ch->channel_name,
                                                  channel_qn, "", 0, 0, channel_props);

        const hyp_gbuf_node_t *src_node = find_channel_source(ctx, ch, rel);
        if (src_node && channel_id > 0) {
            const char *edge_type = ch->direction == HYP_CHANNEL_EMIT ? "EMITS" : "LISTENS_ON";
            char edge_props[HYP_SZ_128];
            snprintf(edge_props, sizeof(edge_props), "{\"transport\":\"%s\"}",
                     ch->transport ? ch->transport : "unknown");
            hyp_gbuf_insert_edge(ctx->gbuf, src_node->id, channel_id, edge_type, edge_props);
        }
    }
}

/* Create CONFIGURES edges for one file's env accesses.  extract_env_accesses.c
 * records every os.Getenv / process.env / Environment.GetEnvironmentVariable
 * style access into result->env_accesses.  We materialize one EnvVar node per
 * env key and link the enclosing function (or the file node) CONFIGURES-> it,
 * so environment-driven configuration is visible even when the accessor is a
 * stdlib symbol that never resolves to an in-graph callee. */
int hyp_pipeline_create_env_configures_for_file(hyp_pipeline_ctx_t *ctx,
                                                const HYPFileResult *result, const char *rel) {
    int count = 0;
    char *file_qn = NULL;
    const hyp_gbuf_node_t *file_node = NULL;
    for (int j = 0; j < result->env_accesses.count; j++) {
        const HYPEnvAccess *ea = &result->env_accesses.items[j];
        if (!ea->env_key || !ea->env_key[0]) {
            continue;
        }
        char env_qn[HYP_SZ_512];
        snprintf(env_qn, sizeof(env_qn), "__env__%s", ea->env_key);
        char env_props[HYP_SZ_512];
        snprintf(env_props, sizeof(env_props), "{\"env_key\":\"%s\"}", ea->env_key);
        int64_t env_id =
            hyp_gbuf_upsert_node(ctx->gbuf, "EnvVar", ea->env_key, env_qn, "", 0, 0, env_props);
        if (env_id <= 0) {
            continue;
        }
        const hyp_gbuf_node_t *src = NULL;
        if (ea->enclosing_func_qn && ea->enclosing_func_qn[0]) {
            src = hyp_gbuf_find_by_qn(ctx->gbuf, ea->enclosing_func_qn);
            /* A class-level env access in a directory-module language carries
             * the DIRECTORY module QN, which hits the shared Folder/Project
             * node — attribute to this file's File node instead (#787, #842). */
            if (hyp_pipeline_node_is_dir_container(src)) {
                src = NULL;
            }
        }
        if (!src) {
            if (!file_qn) {
                file_qn = hyp_pipeline_fqn_compute(ctx->project_name, rel, "__file__");
                file_node = hyp_gbuf_find_by_qn(ctx->gbuf, file_qn);
            }
            src = file_node;
        }
        if (src && src->id != env_id) {
            hyp_gbuf_insert_edge(ctx->gbuf, src->id, env_id, "CONFIGURES",
                                 "{\"strategy\":\"env_access\"}");
            count++;
        }
    }
    free(file_qn);
    return count;
}

/* Create IMPORTS edges for one file's imports.  Mirrors the resolution
 * logic in pass_parallel.c register_and_link_def — keep the two in sync. */
static int create_import_edges_for_file(hyp_pipeline_ctx_t *ctx, const HYPFileResult *result,
                                        const char *rel, HYPHashTable *namespace_map) {
    int count = 0;
    char *file_qn = hyp_pipeline_fqn_compute(ctx->project_name, rel, "__file__");
    const hyp_gbuf_node_t *source_node = hyp_gbuf_find_by_qn(ctx->gbuf, file_qn);
    if (!source_node) {
        free(file_qn);
        return 0;
    }
    for (int j = 0; j < result->imports.count; j++) {
        const HYPImport *imp = &result->imports.items[j];
        if (!imp->module_path) {
            continue;
        }
        const hyp_gbuf_node_t *target =
            hyp_pipeline_resolve_import_node(ctx, rel, file_qn, imp, namespace_map);
        if (target && target->id != source_node->id) {
            char imp_props[HYP_SZ_256];
            snprintf(imp_props, sizeof(imp_props), "{\"local_name\":\"%s\"}",
                     imp->local_name ? imp->local_name : "");
            hyp_gbuf_insert_edge(ctx->gbuf, source_node->id, target->id, "IMPORTS", imp_props);
            count++;
        } else if (!target && hyp_pipeline_ctx_records_workspace_evidence(ctx)) {
            /* The specifier names no file in THIS member, which is what
             * an include of a sibling member's header looks like from here.
             * Recorded rather than dropped; the parallel path
             * (create_imports_edges) records the same fact at the same
             * condition through the same writer. */
            hyp_pipeline_record_unresolved_import(ctx->gbuf, source_node->id, imp->module_path);
        }
    }
    free(file_qn);
    return count;
}

static bool objectscript_export_append_strings(HYPArena *arena, const char ***dst,
                                               const char *const *src) {
    if (!src) {
        return true;
    }
    int old_count = 0;
    int add_count = 0;
    while (*dst && (*dst)[old_count]) {
        old_count++;
    }
    while (src[add_count]) {
        add_count++;
    }
    const char **items = (const char **)hyp_arena_alloc(arena, (size_t)(old_count + add_count + 1) *
                                                                   sizeof(const char *));
    if (!items) {
        return false;
    }
    for (int i = 0; i < old_count; i++) {
        items[i] = (*dst)[i];
    }
    for (int i = 0; i < add_count; i++) {
        items[old_count + i] = src[i];
    }
    items[old_count + add_count] = NULL;
    *dst = items;
    return true;
}

#define OBJECTSCRIPT_EXPORT_APPEND_ARRAY(aggregate, part, field, push_fn)                   \
    do {                                                                                    \
        int expected_count = (aggregate)->field.count + (part)->field.count;                \
        for (int item_i = 0; item_i < (part)->field.count; item_i++) {                      \
            push_fn(&(aggregate)->field, &(aggregate)->arena, (part)->field.items[item_i]); \
        }                                                                                   \
        if ((aggregate)->field.count != expected_count) {                                   \
            return false;                                                                   \
        }                                                                                   \
    } while (0)

static bool objectscript_export_append_primary_arrays(HYPFileResult *aggregate,
                                                      const HYPFileResult *part) {
    OBJECTSCRIPT_EXPORT_APPEND_ARRAY(aggregate, part, defs, hyp_defs_push);
    OBJECTSCRIPT_EXPORT_APPEND_ARRAY(aggregate, part, calls, hyp_calls_push);
    OBJECTSCRIPT_EXPORT_APPEND_ARRAY(aggregate, part, imports, hyp_imports_push);
    OBJECTSCRIPT_EXPORT_APPEND_ARRAY(aggregate, part, usages, hyp_usages_push);
    OBJECTSCRIPT_EXPORT_APPEND_ARRAY(aggregate, part, throws, hyp_throws_push);
    OBJECTSCRIPT_EXPORT_APPEND_ARRAY(aggregate, part, rw, hyp_rw_push);
    OBJECTSCRIPT_EXPORT_APPEND_ARRAY(aggregate, part, type_refs, hyp_typerefs_push);
    return true;
}

static bool objectscript_export_append_secondary_arrays(HYPFileResult *aggregate,
                                                        const HYPFileResult *part) {
    OBJECTSCRIPT_EXPORT_APPEND_ARRAY(aggregate, part, env_accesses, hyp_envaccess_push);
    OBJECTSCRIPT_EXPORT_APPEND_ARRAY(aggregate, part, type_assigns, hyp_typeassign_push);
    OBJECTSCRIPT_EXPORT_APPEND_ARRAY(aggregate, part, impl_traits, hyp_impltrait_push);
    OBJECTSCRIPT_EXPORT_APPEND_ARRAY(aggregate, part, resolved_calls, hyp_resolvedcall_push);
    OBJECTSCRIPT_EXPORT_APPEND_ARRAY(aggregate, part, string_refs, hyp_stringref_push);
    OBJECTSCRIPT_EXPORT_APPEND_ARRAY(aggregate, part, infra_bindings, hyp_infrabinding_push);
    OBJECTSCRIPT_EXPORT_APPEND_ARRAY(aggregate, part, channels, hyp_channels_push);
    return true;
}

/* Preserve every generated class's parse diagnostics. The generated UDL
 * snippets all map back to one physical Studio Export file, so their compact
 * range lists can be concatenated using the ordinary comma separator. */
static bool objectscript_export_append_error_ranges(HYPFileResult *aggregate,
                                                    const HYPFileResult *part) {
    aggregate->parse_incomplete = aggregate->parse_incomplete || part->parse_incomplete;
    aggregate->error_region_count += part->error_region_count;
    if (!part->error_ranges || !part->error_ranges[0]) {
        return true;
    }
    const char *combined = NULL;
    if (aggregate->error_ranges && aggregate->error_ranges[0]) {
        combined = hyp_arena_sprintf(&aggregate->arena, "%s,%s", aggregate->error_ranges,
                                     part->error_ranges);
    } else {
        combined = hyp_arena_strdup(&aggregate->arena, part->error_ranges);
    }
    if (!combined) {
        return false;
    }
    aggregate->error_ranges = combined;
    return true;
}

/* Studio Export files may contain multiple <Class> elements, while the
 * pipeline cache has one slot per physical file. Extract each generated UDL
 * class independently (preserving the upstream parser behavior), then compose
 * every extracted carrier into one result for the normal registry/call/usage/
 * semantic passes. */
HYPFileResult *hyp_pipeline_extract_objectscript_export(
    const char *source, int source_len, const char *project_name, const char *rel_path,
    const HYPMacroTable *macro_table, const HYPReturnTypeTable *return_type_table) {
    HYPArena export_arena;
    hyp_arena_init(&export_arena);
    int class_count = 0;
    char **udl_strings = hyp_iris_export_to_udl(&export_arena, source, source_len, &class_count);
    if (!udl_strings || class_count <= 0) {
        hyp_arena_destroy(&export_arena);
        return NULL;
    }

    HYPFileResult *aggregate = (HYPFileResult *)calloc(1, sizeof(HYPFileResult));
    if (!aggregate) {
        hyp_arena_destroy(&export_arena);
        return NULL;
    }
    hyp_arena_init(&aggregate->arena);
    if (aggregate->arena.nblocks == 0) {
        hyp_free_result(aggregate);
        hyp_arena_destroy(&export_arena);
        return NULL;
    }
    aggregate->owned_results =
        (HYPFileResult **)calloc((size_t)class_count, sizeof(HYPFileResult *));
    if (!aggregate->owned_results) {
        hyp_free_result(aggregate);
        hyp_arena_destroy(&export_arena);
        return NULL;
    }
    aggregate->cached_lang = HYP_LANG_OBJECTSCRIPT_UDL;

    for (int ci = 0; ci < class_count; ci++) {
        HYPFileResult *part = hyp_extract_file_ex(
            udl_strings[ci], (int)strlen(udl_strings[ci]), HYP_LANG_OBJECTSCRIPT_UDL, project_name,
            rel_path, HYP_EXTRACT_BUDGET, NULL, NULL, macro_table, return_type_table);
        if (!part) {
            continue;
        }

        /* The aggregate has no single parse tree. Later ObjectScript Export
         * passes consume extracted carriers, not a raw-XML tree. */
        hyp_free_tree(part);
        if (!objectscript_export_append_primary_arrays(aggregate, part) ||
            !objectscript_export_append_secondary_arrays(aggregate, part) ||
            !objectscript_export_append_strings(&aggregate->arena, &aggregate->exports,
                                                part->exports) ||
            !objectscript_export_append_strings(&aggregate->arena, &aggregate->constants,
                                                part->constants) ||
            !objectscript_export_append_strings(&aggregate->arena, &aggregate->global_vars,
                                                part->global_vars) ||
            !objectscript_export_append_strings(&aggregate->arena, &aggregate->macros,
                                                part->macros) ||
            !objectscript_export_append_error_ranges(aggregate, part)) {
            goto merge_failed;
        }

        if (!aggregate->module_qn) {
            aggregate->module_qn = part->module_qn;
        }
        if (!aggregate->namespace_name) {
            aggregate->namespace_name = part->namespace_name;
        }
        if (part->has_error) {
            aggregate->has_error = true;
            if (!aggregate->error_msg) {
                aggregate->error_msg = part->error_msg;
            }
        }
        aggregate->is_test_file = aggregate->is_test_file || part->is_test_file;
        aggregate->owned_results[aggregate->owned_result_count++] = part;
        continue;

    merge_failed:
        hyp_free_result(part);
        hyp_free_result(aggregate);
        hyp_arena_destroy(&export_arena);
        return NULL;
    }

    aggregate->imports_count = aggregate->imports.count;
    hyp_arena_destroy(&export_arena);
    return aggregate;
}

#undef OBJECTSCRIPT_EXPORT_APPEND_ARRAY

int hyp_pipeline_pass_definitions(hyp_pipeline_ctx_t *ctx, const hyp_file_info_t *files,
                                  int file_count) {
    hyp_log_info("pass.start", "pass", "definitions", "files", itoa_log(file_count));

    /* Ensure extraction library is initialized */
    hyp_init();

    /* Defensive: a prior pipeline run may have left a thread-local parser whose
     * lexer holds pointers into a slab that has since been reclaimed. Drop it
     * here so the first hyp_extract_file below recreates a fresh parser. */
    hyp_destroy_thread_parser();

    int total_defs = 0;
    int total_calls = 0;
    int total_imports = 0;
    int errors = 0;

    /* Sequential pass must extract all defs (which create Module/Function/...
     * nodes) BEFORE resolving imports — otherwise a workspace import in the
     * first file processed can't find the target Module node, because the
     * target file's defs haven't been extracted yet. Result cache is
     * required for this two-phase ordering. */
    HYPFileResult **local_cache = ctx->result_cache;
    bool owns_local_cache = false;
    if (!local_cache) {
        local_cache = (HYPFileResult **)calloc((size_t)file_count, sizeof(HYPFileResult *));
        owns_local_cache = (local_cache != NULL);
    }

    /* Phase 1: extract every file and create def-derived nodes (Modules,
     * Functions, ...) so any file's IMPORTS can resolve against the
     * complete in-memory graph in Phase 2. */
    for (int i = 0; i < file_count; i++) {
        if (hyp_pipeline_check_cancel(ctx)) {
            /* Cancellation mid-extraction: release the cache this pass owns,
             * including results already extracted into it (the normal cleanup
             * at the end of the pass does the same) -- clang-analyzer caught
             * this return leaking the whole cache. */
            if (owns_local_cache) {
                for (int j = 0; j < file_count; j++) {
                    if (local_cache[j]) {
                        hyp_free_result(local_cache[j]);
                    }
                }
                free(local_cache);
            }
            return HYP_NOT_FOUND;
        }

        const char *path = files[i].path;
        const char *rel = files[i].rel_path;
        HYPLanguage lang = files[i].language;

        /* Crash-quarantine skip (Stage 3c): the supervisor's single-threaded
         * recovery re-run always lands on THIS sequential path (worker_count
         * forced to 1). This first sequential pass REPORTS a crasher as a
         * phase="crash" skip (surfacing it in skipped[]) and continues; later
         * sequential passes (calls/usages/semantic) re-extract on a cache miss
         * but hit the hard guard inside hyp_extract_file, so they no-op without
         * re-crashing and without duplicating the skip. No-op unless
         * HYP_INDEX_QUARANTINE_FILE is set. */
        if (hyp_index_is_quarantined(rel)) {
            const char *phase = hyp_index_quarantine_phase(rel);
            if (!phase) {
                phase = "crash";
            }
            const char *reason =
                (strcmp(phase, "hang") == 0) ? "quarantined after hang" : "quarantined after crash";
            hyp_pipeline_add_file_error(ctx->pipeline, rel, reason, phase);
            errors++;
            continue;
        }

        /* Read source file */
        int source_len = 0;
        long file_size = 0;
        hyp_read_status_t rst = HYP_READ_OK;
        char *source = read_file(path, &source_len, &file_size, &rst);
        if (!source) {
            errors++;
            if (rst == HYP_READ_OVERSIZED) {
                /* Never a silent drop: record the oversized skip + WARN so the
                 * file surfaces in the response/logfile with its sizes. */
                long cap = hyp_max_file_bytes();
                char reason[96];
                snprintf(reason, sizeof(reason), "oversized (%lld MB > %lld MB)",
                         (long long)(file_size / (HYP_SZ_1K * HYP_SZ_1K)),
                         (long long)(cap / (HYP_SZ_1K * HYP_SZ_1K)));
                hyp_pipeline_add_file_error(ctx->pipeline, rel, reason, "oversized");
                hyp_log_warn("index.file_oversized", "path", rel, "size_mb",
                             itoa_log((int)(file_size / (HYP_SZ_1K * HYP_SZ_1K))), "cap_mb",
                             itoa_log((int)(cap / (HYP_SZ_1K * HYP_SZ_1K))));
            } else if (rst == HYP_READ_OPEN_FAIL || rst == HYP_READ_OOM) {
                hyp_pipeline_add_file_error(ctx->pipeline, rel, "read failed", "read");
            }
            /* HYP_READ_EMPTY: benign 0-byte file — nothing to index, not reported. */
            continue;
        }

        /* Studio Export XML is transformed to one cacheable aggregate so later
         * passes see the same calls/usages/semantic carriers as native UDL. */
        HYPFileResult *result =
            lang == HYP_LANG_OBJECTSCRIPT_EXPORT
                ? hyp_pipeline_extract_objectscript_export(source, source_len, ctx->project_name,
                                                           rel, ctx->macro_table, NULL)
                : hyp_extract_file_ex(
                      source, source_len, lang, ctx->project_name, rel, HYP_EXTRACT_BUDGET, NULL,
                      NULL /* no extra defines or include paths */, ctx->macro_table, NULL);
        free(source);

        if (!result) {
            errors++;
            hyp_pipeline_add_file_error(ctx->pipeline, rel, "extract failed", "extract");
            continue;
        }
        /* Consume the previously-ignored has_error flag: a parse timeout /
         * parse failure / unsupported-grammar result carries no defs but must
         * still be reported (phase "extract", reason = the extractor's message).
         * The empty result flows through unchanged (the defs loop is a no-op). */
        if (result->has_error) {
            hyp_pipeline_add_file_error(ctx->pipeline, rel,
                                        result->error_msg ? result->error_msg : "extract failed",
                                        "extract");
            errors++;
        } else if (result->parse_incomplete) {
            /* Best-effort parse-coverage signal (#963): indexed, but with
             * ERROR/MISSING regions — see pass_parallel.c (keep in sync). */
            hyp_pipeline_add_file_error(ctx->pipeline, rel,
                                        result->error_ranges ? result->error_ranges : "unknown",
                                        "parse_partial");
        }

        /* Create nodes for each definition */
        for (int d = 0; d < result->defs.count; d++) {
            process_def(ctx, &result->defs.items[d], rel);
            total_defs++;
        }

        /* Store calls for pass_calls (we save them in the extraction results
         * for now — a future optimization would batch these) */
        total_calls += result->calls.count;

        if (local_cache) {
            local_cache[i] = result;
        } else {
            /* Cache unavailable: imports for this file can still only
             * resolve to defs already in the graph, but the file's
             * own defs are now persisted before the lookup. No namespace
             * map is available without the cache (single-file scope). */
            total_imports += create_import_edges_for_file(ctx, result, rel, NULL);
            create_channel_edges_for_file(ctx, result, rel);
            hyp_pipeline_create_env_configures_for_file(ctx, result, rel);
            hyp_free_result(result);
        }
    }

    /* Phase 2: now that all extraction results are cached and Module
     * nodes for every file are in the graph, walk the cache again to
     * create IMPORTS / channel edges. Imports resolve against the full
     * project graph. */
    if (local_cache) {
        /* Build a namespace/package → File-QN map so that namespace imports
         * (C# `using`, Java/Kotlin `import`, PHP `use`) resolve to the file
         * that declares the namespace. */
        const char **rels = (const char **)calloc((size_t)file_count, sizeof(char *));
        if (rels) {
            for (int i = 0; i < file_count; i++) {
                rels[i] = files[i].rel_path;
            }
        }
        HYPHashTable *namespace_map =
            hyp_pipeline_namespace_map_build(ctx->project_name, local_cache, rels, file_count);
        free(rels);
        for (int i = 0; i < file_count; i++) {
            if (hyp_pipeline_check_cancel(ctx)) {
                break;
            }
            HYPFileResult *result = local_cache[i];
            if (!result) {
                continue;
            }
            total_imports +=
                create_import_edges_for_file(ctx, result, files[i].rel_path, namespace_map);
            create_channel_edges_for_file(ctx, result, files[i].rel_path);
            hyp_pipeline_create_env_configures_for_file(ctx, result, files[i].rel_path);
        }
        hyp_pipeline_namespace_map_free(namespace_map);
        if (owns_local_cache) {
            for (int i = 0; i < file_count; i++) {
                if (local_cache[i]) {
                    hyp_free_result(local_cache[i]);
                }
            }
            free(local_cache);
        }
    }

    hyp_log_info("pass.done", "pass", "definitions", "defs", itoa_log(total_defs), "calls",
                 itoa_log(total_calls), "imports", itoa_log(total_imports), "errors",
                 itoa_log(errors));
    return 0;
}
