/*
 * test_orphan_corpus.c — GENERATED data for the G1 anchor corpus.
 *
 * See test_orphan_corpus.h for what this is and why the span text is
 * carried verbatim instead of being re-parsed.
 */
#include "test_orphan_corpus.h"

#include <stddef.h>

const g1_text_t g1_texts[] = {
    {
        "3f4c1f498985bc970ec6a2441a8562db",
        "static char *handle_query_graph(hyp_mcp_server_t *srv, const char *args) {\n"
        "    char *query = hyp_mcp_get_string_arg(args, \"query\");\n"
        "    char *project = get_project_arg(args);\n"
        "    hyp_store_t *store = resolve_store(srv, project);\n"
        "    int max_rows = hyp_mcp_get_int_arg(args, \"max_rows\", 0);\n"
        "\n"
        "    /* graph=\"missed\" (#963): run the SAME cypher against the derived\n"
        "     * miss-graph view (shadow project \"<project>::missed\") instead of the\n"
        "     * code graph — file structure of not-fully-indexed files only. */\n"
        "    char *graph_arg = hyp_mcp_get_string_arg(args, \"graph\");\n"
        "    bool missed_graph = graph_arg && strcmp(graph_arg, \"missed\") == 0;\n"
        "    free(graph_arg);\n"
        "\n"
        "    if (!query) {\n"
        "        free(project);\n"
        "        return hyp_mcp_text_result(\"query is required\", true);\n"
        "    }\n"
        "    if (missed_graph && !project) {\n"
        "        free(query);\n"
        "        return hyp_mcp_text_result(\"project is required when graph=\\\"missed\\\"\", true);\n"
        "    }\n"
        "    if (!store) {\n"
        "        char *_err = build_project_list_error(\"project not found or not indexed\");\n"
        "        char *_res = hyp_mcp_text_result(_err, true);\n"
        "        free(_err);\n"
        "        free(project);\n"
        "        free(query);\n"
        "        return _res;\n"
        "    }\n"
        "\n"
        "    char *not_indexed = verify_project_indexed(store, project);\n"
        "    if (not_indexed) {\n"
        "        free(project);\n"
        "        free(query);\n"
        "        return not_indexed;\n"
        "    }\n"
        "\n"
        "    char covproj[HYP_SZ_512];\n"
        "    const char *cypher_project = project;\n"
        "    if (missed_graph) {\n"
        "        hyp_store_coverage_shadow_project(covproj, sizeof(covproj), project);\n"
        "        cypher_project = covproj;\n"
        "    }\n"
        "\n"
        "    hyp_cypher_result_t result = {0};\n"
        "    int rc = hyp_cypher_execute(store, query, cypher_project, max_rows, &result);\n"
        "\n"
        "    if (rc < 0) {\n"
        "        char *err_msg = result.error ? result.error : \"query execution failed\";\n"
        "        char *resp = hyp_mcp_text_result(err_msg, true);\n"
        "        hyp_cypher_result_free(&result);\n"
        "        free(query);\n"
        "        free(project);\n"
        "        return resp;\n"
        "    }\n"
        "\n"
        "    /* Response encoding: TOON table by default (the columns double as the\n"
        "     * table header); format:\"json\" restores the legacy columns/rows arrays. */\n"
        "    char *qg_format = hyp_mcp_get_string_arg(args, \"format\");\n"
        "    bool qg_legacy_json = qg_format && strcmp(qg_format, \"json\") == 0;\n"
        "    free(qg_format);\n"
        "\n"
        "    char *json = NULL;\n"
        "    if (!qg_legacy_json) {\n"
        "        hyp_sb_t sb;\n"
        "        hyp_sb_init(&sb);\n"
        "        hyp_tree_table_header(&sb, \"rows\", result.row_count, (const char *const *)result.columns,\n"
        "                              result.col_count);\n"
        "        for (int r = 0; r < result.row_count; r++) {\n"
        "            hyp_tree_row_begin(&sb);\n"
        "            for (int c = 0; c < result.col_count; c++) {\n"
        "                hyp_tree_cell_str(&sb, result.rows[r][c], c == 0);\n"
        "            }\n"
        "            hyp_tree_row_end(&sb);\n"
        "        }\n"
        "        hyp_tree_scalar_int(&sb, \"total\", result.row_count);\n"
        "        if (result.warning) {\n"
        "            hyp_tree_scalar_str(&sb, \"warning\", result.warning);\n"
        "        }\n"
        "        if (result.row_count == 0) {\n"
        "            hyp_tree_scalar_str(&sb, \"hint\",\n"
        "                                \"Query returned no results. Use get_graph_schema() to see \"\n"
        "                                \"available labels and edge types.\");\n"
        "        }\n"
        "        json = hyp_sb_finish(&sb);\n"
        "    } else {\n"
        "        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);\n"
        "        yyjson_mut_val *root = yyjson_mut_obj(doc);\n"
        "        yyjson_mut_doc_set_root(doc, root);\n"
        "\n"
        "        /* columns */\n"
        "        yyjson_mut_val *cols = yyjson_mut_arr(doc);\n"
        "        for (int i = 0; i < result.col_count; i++) {\n"
        "            yyjson_mut_arr_add_str(doc, cols, result.columns[i]);\n"
        "        }\n"
        "        yyjson_mut_obj_add_val(doc, root, \"columns\", cols);\n"
        "\n"
        "        /* rows */\n"
        "        yyjson_mut_val *rows = yyjson_mut_arr(doc);\n"
        "        for (int r = 0; r < result.row_count; r++) {\n"
        "            yyjson_mut_val *row = yyjson_mut_arr(doc);\n"
        "            for (int c = 0; c < result.col_count; c++) {\n"
        "                yyjson_mut_arr_add_str(doc, row, result.rows[r][c]);\n"
        "            }\n"
        "            yyjson_mut_arr_add_val(rows, row);\n"
        "        }\n"
        "        yyjson_mut_obj_add_val(doc, root, \"rows\", rows);\n"
        "        yyjson_mut_obj_add_int(doc, root, \"total\", result.row_count);\n"
        "        if (result.warning) {\n"
        "            yyjson_mut_obj_add_str(doc, root, \"warning\", result.warning);\n"
        "        }\n"
        "\n"
        "        if (result.row_count == 0) {\n"
        "            yyjson_mut_obj_add_str(\n"
        "                doc, root, \"hint\",\n"
        "                \"Query returned no results. Use get_graph_schema() to see available labels and \"\n"
        "                \"edge types.\");\n"
        "        }\n"
        "\n"
        "        json = yy_doc_to_str(doc);\n"
        "        yyjson_mut_doc_free(doc);\n"
        "    }\n"
        "    hyp_cypher_result_free(&result);\n"
        "    free(query);\n"
        "    free(project);\n"
        "\n"
        "    char *res = hyp_mcp_text_result(json ? json : \"out of memory\", json == NULL);\n"
        "    free(json);\n"
        "    return res;\n"
        "}",
        130,
    },
    {
        "883bb47425e9690c21483e885e204032",
        "static void release_request_store(hyp_mcp_server_t *srv) {\n"
        "    if (!srv || !srv->owns_store || !srv->store || !hyp_store_db_path(srv->store)) {\n"
        "        return;\n"
        "    }\n"
        "    hyp_store_close(srv->store);\n"
        "    srv->store = NULL;\n"
        "    free(srv->current_project);\n"
        "    srv->current_project = NULL;\n"
        "    /* The close above frees a connection's worth of page cache. Ask the\n"
        "     * allocator to hand those pages back now, which keeps a long-lived daemon\n"
        "     * flat across thousands of request-scoped stores (#581). This only became\n"
        "     * meaningful once the Windows interposer made the pages mimalloc's: an\n"
        "     * earlier attempt aimed at the CRT heap instead and could not release\n"
        "     * them. POSIX already purges on free, so this is a no-op there. */\n"
        "    hyp_mem_collect();\n"
        "}",
        16,
    },
    {
        "b2f5a5f1a8c98236bc4c2372e49c556e",
        "static void adr_list_sections_from_content(yyjson_mut_doc *doc, yyjson_mut_val *root_obj,\n"
        "                                           const char *content) {\n"
        "    yyjson_mut_val *sections = yyjson_mut_arr(doc);\n"
        "    const char *p = content;\n"
        "    while (p && *p) {\n"
        "        const char *eol = strchr(p, '\\n');\n"
        "        size_t linelen = eol ? (size_t)(eol - p) : strlen(p);\n"
        "        while (linelen > 0 && p[linelen - SKIP_ONE] == '\\r') {\n"
        "            linelen--;\n"
        "        }\n"
        "        if (linelen > 0 && p[0] == '#') {\n"
        "            char hdr[CBM_SZ_1K];\n"
        "            if (linelen >= sizeof(hdr)) {\n"
        "                linelen = sizeof(hdr) - SKIP_ONE;\n"
        "            }\n"
        "            memcpy(hdr, p, linelen);\n"
        "            hdr[linelen] = '\\0';\n"
        "            yyjson_mut_arr_add_strcpy(doc, sections, hdr);\n"
        "        }\n"
        "        if (!eol) {\n"
        "            break;\n"
        "        }\n"
        "        p = eol + SKIP_ONE;\n"
        "    }\n"
        "    yyjson_mut_obj_add_val(doc, root_obj, \"sections\", sections);\n"
        "}",
        26,
    },
    {
        "1d7b1909c814be5bb3b312f3d5b0804d",
        "static bool check_inline_props(const cbm_node_t *n, const cbm_prop_filter_t *props, int count) {\n"
        "    for (int i = 0; i < count; i++) {\n"
        "        const char *actual = node_prop(n, props[i].key);\n"
        "        if (strcmp(actual, props[i].value) != 0) {\n"
        "            return false;\n"
        "        }\n"
        "    }\n"
        "    return true;\n"
        "}",
        9,
    },
    {
        "7c007c18ad1a81d595285d75960bd240",
        "static bool mcp_cross_repo_create_project_store(const char *cache, const char *project,\n"
        "                                                const char *root_path) {\n"
        "    char db_path[HYP_SZ_1K];\n"
        "    snprintf(db_path, sizeof(db_path), \"%s/%s.db\", cache, project);\n"
        "    hyp_store_t *store = hyp_store_open_path(db_path);\n"
        "    if (!store) {\n"
        "        return false;\n"
        "    }\n"
        "    bool created = hyp_store_upsert_project(store, project, root_path) == HYP_STORE_OK;\n"
        "    hyp_store_close(store);\n"
        "    return created;\n"
        "}",
        12,
    },
    {
        "4b3987980defd07c0e9bbb3268c97850",
        "static const char *json_str_at(yyjson_val *obj, const char *key) {\n"
        "    yyjson_val *v = obj ? yyjson_obj_get(obj, key) : NULL;\n"
        "    return v && yyjson_is_str(v) ? yyjson_get_str(v) : NULL;\n"
        "}",
        4,
    },
    {
        "481a372583e7498caa760e7e9f978b6e",
        "void cbm_run_c_lsp_cross(\n"
        "    CBMArena* arena,\n"
        "    const char* source, int source_len,\n"
        "    const char* module_qn,\n"
        "    bool cpp_mode,\n"
        "    CBMLSPDef* defs, int def_count,\n"
        "    const char** include_paths, const char** include_ns_qns, int include_count,\n"
        "    CBMResolvedCallArray* out) {\n"
        "\n"
        "    if (!source || source_len == 0 || !out) return;\n"
        "\n"
        "    CBMTypeRegistry reg;\n"
        "    cbm_registry_init(&reg, arena);\n"
        "\n"
        "    // Register stdlib\n"
        "    cbm_c_stdlib_register(&reg, arena);\n"
        "    if (cpp_mode) cbm_cpp_stdlib_register(&reg, arena);\n"
        "\n"
        "    // Register all defs\n"
        "    for (int i = 0; i < def_count; i++) {\n"
        "        CBMLSPDef* d = &defs[i];\n"
        "        if (!d->qualified_name || !d->short_name) continue;\n"
        "\n"
        "        if (d->label && (strcmp(d->label, \"Class\") == 0 || strcmp(d->label, \"Type\") == 0 ||\n"
        "                         strcmp(d->label, \"Interface\") == 0)) {\n"
        "            CBMRegisteredType rt;\n"
        "            memset(&rt, 0, sizeof(rt));\n"
        "            rt.qualified_name = cbm_arena_strdup(arena, d->qualified_name);\n"
        "            rt.short_name = cbm_arena_strdup(arena, d->short_name);\n"
        "            rt.is_interface = d->is_interface;\n"
        "\n"
        "            // Embedded/base types\n"
        "            if (d->embedded_types) {\n"
        "                // Parse \"|\"-separated list\n"
        "                const char* src = d->embedded_types;\n"
        "                const char* embeds[32];\n"
        "                int embed_count = 0;\n"
        "                while (*src && embed_count < 31) {\n"
        "                    const char* sep = strchr(src, '|');\n"
        "                    if (sep) {\n"
        "                        embeds[embed_count++] = cbm_arena_strndup(arena, src, sep - src);\n"
        "                        src = sep + 1;\n"
        "                    } else {\n"
        "                        embeds[embed_count++] = cbm_arena_strdup(arena, src);\n"
        "                        break;\n"
        "                    }\n"
        "                }\n"
        "                if (embed_count > 0) {\n"
        "                    const char** arr = (const char**)cbm_arena_alloc(arena, (embed_count + 1) * sizeof(const char*));\n"
        "                    for (int j = 0; j < embed_count; j++) arr[j] = embeds[j];\n"
        "                    arr[embed_count] = NULL;\n"
        "                    rt.embedded_types = arr;\n"
        "                }\n"
        "            }\n"
        "\n"
        "            // Field defs\n"
        "            if (d->field_defs) {\n"
        "                const char* fsrc = d->field_defs;\n"
        "                const char* fnames[64];\n"
        "                const CBMType* ftypes[64];\n"
        "                int fcount = 0;\n"
        "                while (*fsrc && fcount < 63) {\n"
        "                    const char* sep = strchr(fsrc, '|');\n"
        "                    const char* end = sep ? sep : fsrc + strlen(fsrc);\n"
        "                    char* pair = cbm_arena_strndup(arena, fsrc, end - fsrc);\n"
        "                    char* colon = strchr(pair, ':');\n"
        "                    if (colon) {\n"
        "                        *colon = '\\0';\n"
        "                        fnames[fcount] = pair;\n"
        "                        ftypes[fcount] = c_parse_return_type_text(arena, colon + 1,\n"
        "                            d->def_module_qn ? d->def_module_qn : module_qn);\n"
        "                        fcount++;\n"
        "                    }\n"
        "                    if (!sep) break;\n"
        "                    fsrc = sep + 1;\n"
        "                }\n"
        "                if (fcount > 0) {\n"
        "                    const char** fnarr = (const char**)cbm_arena_alloc(arena, (fcount + 1) * sizeof(const char*));\n"
        "                    const CBMType** ftarr = (const CBMType**)cbm_arena_alloc(arena, (fcount + 1) * sizeof(const CBMType*));\n"
        "                    for (int j = 0; j < fcount; j++) { fnarr[j] = fnames[j]; ftarr[j] = ftypes[j]; }\n"
        "                    fnarr[fcount] = NULL;\n"
        "                    ftarr[fcount] = NULL;\n"
        "                    rt.field_names = fnarr;\n"
        "                    rt.field_types = ftarr;\n"
        "                }\n"
        "            }\n"
        "\n"
        "            cbm_registry_add_type(&reg, rt);\n"
        "        }\n"
        "\n"
        "        if (d->label && (strcmp(d->label, \"Function\") == 0 || strcmp(d->label, \"Method\") == 0)) {\n"
        "            CBMRegisteredFunc rf;\n"
        "            memset(&rf, 0, sizeof(rf));\n"
        "            rf.min_params = -1;\n"
        "            rf.qualified_name = cbm_arena_strdup(arena, d->qualified_name);\n"
        "            rf.short_name = cbm_arena_strdup(arena, d->short_name);\n"
        "\n"
        "            const char* def_module = d->def_module_qn ? d->def_module_qn : module_qn;\n"
        "\n"
        "            // Return types\n"
        "            if (d->return_types) {\n"
        "                // Parse \"|\"-separated\n"
        "                const char* rsrc = d->return_types;\n"
        "                const CBMType* rets[16];\n"
        "                int rcount = 0;\n"
        "                while (*rsrc && rcount < 15) {\n"
        "                    const char* sep = strchr(rsrc, '|');\n"
        "                    const char* end = sep ? sep : rsrc + strlen(rsrc);\n"
        "                    char* rt_text = cbm_arena_strndup(arena, rsrc, end - rsrc);\n"
        "                    rets[rcount++] = c_parse_return_type_text(arena, rt_text, def_module);\n"
        "                    if (!sep) break;\n"
        "                    rsrc = sep + 1;\n"
        "                }\n"
        "                if (rcount > 0) {\n"
        "                    const CBMType** rarr = (const CBMType**)cbm_arena_alloc(arena, (rcount + 1) * sizeof(const CBMType*));\n"
        "                    for (int j = 0; j < rcount; j++) rarr[j] = rets[j];\n"
        "                    rarr[rcount] = NULL;\n"
        "                    rf.signature = cbm_type_func(arena, NULL, NULL, rarr);\n"
        "                }\n"
        "            }\n"
        "            if (!rf.signature) rf.signature = cbm_type_func(arena, NULL, NULL, NULL);\n"
        "\n"
        "            if (d->receiver_type) {\n"
        "                rf.receiver_type = cbm_arena_strdup(arena, d->receiver_type);\n"
        "            }\n"
        "\n"
        "            cbm_registry_add_func(&reg, rf);\n"
        "        }\n"
        "    }\n"
        "\n"
        "    // Parse the source with tree-sitter\n"
        "    TSParser* parser = ts_parser_new();\n"
        "    if (!parser) return;\n"
        "\n"
        "    const TSLanguage* ts_lang = cpp_mode ?\n"
        "        tree_sitter_cpp() : tree_sitter_c();\n"
        "    ts_parser_set_language(parser, ts_lang);\n"
        "\n"
        "    TSTree* tree = ts_parser_parse_string(parser, NULL, source, source_len);\n"
        "    ts_parser_delete(parser);\n"
        "    if (!tree) return;\n"
        "\n"
        "    TSNode root = ts_tree_root_node(tree);\n"
        "\n"
        "    // Initialize context and run\n"
        "    CLSPContext ctx;\n"
        "    c_lsp_init(&ctx, arena, source, source_len, &reg, module_qn, cpp_mode, out);\n"
        "\n"
        "    // Add include mappings\n"
        "    for (int i = 0; i < include_count; i++) {\n"
        "        c_lsp_add_include(&ctx, include_paths[i], include_ns_qns[i]);\n"
        "    }\n"
        "\n"
        "    c_lsp_process_file(&ctx, root);\n"
        "\n"
        "    ts_tree_delete(tree);\n"
        "}",
        157,
    },
    {
        "95ac705b4bd195a467ffe63a34ee98dd",
        "cbm_daemon_runtime_application_status_t cbm_daemon_runtime_client_application_request(\n"
        "    cbm_daemon_runtime_client_t *client, const void *request, uint32_t request_length,\n"
        "    uint8_t **response_out, uint32_t *response_length_out, uint32_t timeout_ms) {\n"
        "    if (response_out) {\n"
        "        *response_out = NULL;\n"
        "    }\n"
        "    if (response_length_out) {\n"
        "        *response_length_out = 0;\n"
        "    }\n"
        "    if (!client || !response_out || !response_length_out ||\n"
        "        timeout_ms == CBM_DAEMON_IPC_WAIT_FOREVER ||\n"
        "        request_length > CBM_DAEMON_RUNTIME_APPLICATION_PAYLOAD_MAX ||\n"
        "        (request_length > 0 && !request)) {\n"
        "        return CBM_DAEMON_RUNTIME_APPLICATION_TRANSPORT_ERROR;\n"
        "    }\n"
        "    cbm_daemon_runtime_application_token_t request_token =\n"
        "        CBM_DAEMON_RUNTIME_APPLICATION_TOKEN_INVALID;\n"
        "    if (!cbm_daemon_runtime_client_application_token_reserve(client, &request_token)) {\n"
        "        return CBM_DAEMON_RUNTIME_APPLICATION_TRANSPORT_ERROR;\n"
        "    }\n"
        "    return cbm_daemon_runtime_client_application_request_tagged(client, request_token, request,\n"
        "                                                                request_length, response_out,\n"
        "                                                                response_length_out, timeout_ms);\n"
        "}",
        24,
    },
    {
        "a5cae8f7cffafb6defc602e0d0335a7a",
        "static void write_diagnostics(void) {\n"
        "#ifdef HYP_DIAGNOSTICS_ENABLE_TEST_API\n"
        "    diag_test_pause_writer();\n"
        "#endif\n"
        "    if (atomic_load_explicit(&g_diag_stop, memory_order_acquire)) {\n"
        "        return;\n"
        "    }\n"
        "\n"
        "    size_t elapsed_ms = 0;\n"
        "    size_t user_ms = 0;\n"
        "    size_t sys_ms = 0;\n"
        "    size_t current_rss = 0;\n"
        "    size_t peak_rss = 0;\n"
        "    size_t current_commit = 0;\n"
        "    size_t peak_commit = 0;\n"
        "    size_t page_faults = 0;\n"
        "    mi_process_info(&elapsed_ms, &user_ms, &sys_ms, &current_rss, &peak_rss, &current_commit,\n"
        "                    &peak_commit, &page_faults);\n"
        "    /* NEVER report mimalloc's current_rss. On Linux it is not RSS at all — it\n"
        "     * is aliased to mimalloc's own committed-page counter, an int64 the vendored\n"
        "     * stats.c casts to size_t (vendored/mimalloc/src/stats.c). Under this\n"
        "     * project's tuning (arena_eager_commit=0, purge_decommits=1, purge_delay=0)\n"
        "     * that counter goes NEGATIVE, and the cast wraps it to ~2^64. The soak\n"
        "     * harness then faithfully divided 18446744073580445696 by 1 MB and reported\n"
        "     * a 17-trillion-MB leak while real RSS was 7 MB — both Linux legs of dry run\n"
        "     * 31852463409, while the Windows leg passed because it gets a real number\n"
        "     * from GetProcessMemoryInfo.\n"
        "     *\n"
        "     * The `== 0` guard could not catch it: a wrapped value is enormous, not\n"
        "     * zero. hyp_mem_rss() exists precisely for this and reads /proc/self/statm\n"
        "     * on Linux; its own comment names this trap and warns that a nonzero-but-\n"
        "     * wrong current_rss defeats exactly this shape of guard. Use it\n"
        "     * unconditionally rather than as a fallback. */\n"
        "    current_rss = hyp_mem_rss();\n"
        "\n"
        "    int fds = count_open_fds();\n"
        "    time_t now = time(NULL);\n"
        "    long uptime = (long)(now - g_start_time);\n"
        "    int qcount = atomic_load(&g_query_stats.count);\n"
        "    int qerrors = atomic_load(&g_query_stats.errors);\n"
        "    long long qtime = atomic_load(&g_query_stats.time_us);\n"
        "    long long qmax = atomic_load(&g_query_stats.max_us);\n"
        "    long long qavg = qcount > 0 ? qtime / qcount : 0;\n"
        "\n"
        "    /* Memory map: live allocator bytes on this thread's heap, a size-class\n"
        "     * histogram, and the residual the walk does NOT account for. The residual\n"
        "     * is what keeps this honest — see mem.h. */\n"
        "    diag_write_allocator_stats();\n"
        "\n"
        "    hyp_mem_map_t map;\n"
        "    (void)hyp_mem_map_collect_os(&map);\n"
        "    size_t residual =\n"
        "        map.os_committed_bytes > map.live_bytes ? map.os_committed_bytes - map.live_bytes : 0;\n"
        "    char buckets[HYP_SZ_512];\n"
        "    int bucket_length = 0;\n"
        "    for (int i = 0;\n"
        "         i < HYP_MEM_MAP_BUCKETS && bucket_length >= 0 && (size_t)bucket_length < sizeof(buckets);\n"
        "         i++) {\n"
        "        int written =\n"
        "            snprintf(buckets + bucket_length, sizeof(buckets) - (size_t)bucket_length,\n"
        "                     \"%s{\\\"limit\\\": %zu, \\\"bytes\\\": %zu, \\\"blocks\\\": %zu}\", i == 0 ? \"\" : \", \",\n"
        "                     hyp_mem_map_bucket_limit(i), map.bucket_bytes[i], map.bucket_blocks[i]);\n"
        "        if (written < 0) {\n"
        "            break;\n"
        "        }\n"
        "        bucket_length += written;\n"
        "    }\n"
        "\n"
        "    /* Phase attribution: which bracketed code path the committed bytes stayed\n"
        "     * in. Empty unless HYP_MEM_PHASES=1. */\n"
        "    char phases[HYP_SZ_1K];\n"
        "    if (hyp_mem_phase_report_json(phases, sizeof(phases)) <= 0) {\n"
        "        phases[0] = '\\0';\n"
        "    }\n"
        "\n"
        "    char snapshot[HYP_SZ_4K];\n"
        "    int length =\n"
        "        snprintf(snapshot, sizeof(snapshot),\n"
        "                 \"{\\n\"\n"
        "                 \"  \\\"uptime_s\\\": %ld,\\n\"\n"
        "                 \"  \\\"rss_bytes\\\": %zu,\\n\"\n"
        "                 \"  \\\"peak_rss_bytes\\\": %zu,\\n\"\n"
        "                 \"  \\\"heap_committed_bytes\\\": %zu,\\n\"\n"
        "                 \"  \\\"peak_committed_bytes\\\": %zu,\\n\"\n"
        "                 \"  \\\"page_faults\\\": %zu,\\n\"\n"
        "                 \"  \\\"fd_count\\\": %d,\\n\"\n"
        "                 \"  \\\"query_count\\\": %d,\\n\"\n"
        "                 \"  \\\"query_errors\\\": %d,\\n\"\n"
        "                 \"  \\\"query_total_us\\\": %lld,\\n\"\n"
        "                 \"  \\\"query_avg_us\\\": %lld,\\n\"\n"
        "                 \"  \\\"query_max_us\\\": %lld,\\n\"\n"
        "                 \"  \\\"mem_malloc_owned\\\": %s,\\n\"\n"
        "                 \"  \\\"mem_live_bytes\\\": %zu,\\n\"\n"
        "                 \"  \\\"mem_live_blocks\\\": %zu,\\n\"\n"
        "                 \"  \\\"mem_area_committed_bytes\\\": %zu,\\n\"\n"
        "                 \"  \\\"mem_residual_bytes\\\": %zu,\\n\"\n"
        "                 \"  \\\"mem_buckets\\\": [%s],\\n\"\n"
        "                 \"  \\\"mem_phases\\\": [%s],\\n\"\n"
        "                 \"  \\\"pid\\\": %d\\n\"\n"
        "                 \"}\\n\",\n"
        "                 uptime, current_rss, peak_rss, current_commit, peak_commit, page_faults, fds,\n"
        "                 qcount, qerrors, qtime, qavg, qmax,\n"
        "                 map.malloc_is_allocator_owned ? \"true\" : \"false\", map.live_bytes, map.live_blocks,\n"
        "                 map.area_committed_bytes, residual, buckets, phases, (int)getpid());\n"
        "    if (length <= 0 || (size_t)length >= sizeof(snapshot) ||\n"
        "        !diag_write_file(DIAG_SNAPSHOT_TMP_NAME, snapshot, (size_t)length, false) ||\n"
        "        !diag_native_rename(DIAG_SNAPSHOT_TMP_NAME, DIAG_SNAPSHOT_NAME)) {\n"
        "        diag_native_unlink(DIAG_SNAPSHOT_TMP_NAME);\n"
        "        return;\n"
        "    }\n"
        "    if (!atomic_load_explicit(&g_diag_stop, memory_order_acquire)) {\n"
        "        append_trajectory(uptime, current_rss, peak_rss, current_commit, peak_commit, page_faults,\n"
        "                          fds, qcount);\n"
        "    }\n"
        "}",
        115,
    },
    {
        "7b2960ff89fa67d91eb3300c2bc5dc24",
        "bool hyp_is_test_file(const char *rel_path, HYPLanguage lang) {\n"
        "    if (!rel_path) {\n"
        "        return false;\n"
        "    }\n"
        "    if (is_test_dir_path(rel_path)) {\n"
        "        return true;\n"
        "    }\n"
        "    const char *base = path_basename(rel_path);\n"
        "\n"
        "    switch (lang) {\n"
        "    case HYP_LANG_GO:\n"
        "        return has_suffix(base, \"_test.go\");\n"
        "    case HYP_LANG_PYTHON:\n"
        "        return has_prefix(base, \"test_\") || has_suffix(base, \"_test.py\");\n"
        "    case HYP_LANG_JAVASCRIPT:\n"
        "    case HYP_LANG_TYPESCRIPT:\n"
        "    case HYP_LANG_TSX: {\n"
        "        char noext[NOEXT_BUF];\n"
        "        strip_ext(base, noext, sizeof(noext));\n"
        "        return has_suffix(noext, \".test\") || has_suffix(noext, \".spec\") ||\n"
        "               has_suffix(noext, \"_test\") || has_suffix(noext, \"_spec\") ||\n"
        "               has_prefix(base, \"test_\");\n"
        "    }\n"
        "    case HYP_LANG_JAVA:\n"
        "    case HYP_LANG_KOTLIN:\n"
        "    case HYP_LANG_SCALA:\n"
        "        return has_suffix(base, \"Test.java\") || has_suffix(base, \"Tests.java\") ||\n"
        "               has_suffix(base, \"Spec.java\") || has_suffix(base, \"Test.kt\") ||\n"
        "               has_suffix(base, \"Spec.kt\") || has_suffix(base, \"Test.scala\") ||\n"
        "               has_suffix(base, \"Spec.scala\");\n"
        "    case HYP_LANG_RUST:\n"
        "        // Rust tests are typically mod tests inside the file, but test files too\n"
        "        return has_suffix(base, \"_test.rs\") || has_prefix(base, \"test_\");\n"
        "    case HYP_LANG_RUBY:\n"
        "        return has_suffix(base, \"_test.rb\") || has_suffix(base, \"_spec.rb\") ||\n"
        "               has_prefix(base, \"test_\");\n"
        "    case HYP_LANG_PHP:\n"
        "        return has_suffix(base, \"Test.php\");\n"
        "    case HYP_LANG_CSHARP:\n"
        "        return has_suffix(base, \"Tests.cs\") || has_suffix(base, \"Test.cs\");\n"
        "    case HYP_LANG_CPP:\n"
        "    case HYP_LANG_C:\n"
        "        return has_suffix(base, \"_test.c\") || has_suffix(base, \"_test.cc\") ||\n"
        "               has_suffix(base, \"_test.cpp\") || has_prefix(base, \"test_\");\n"
        "    case HYP_LANG_MATLAB:\n"
        "        return has_prefix(base, \"test_\") || has_prefix(base, \"Test\");\n"
        "    default:\n"
        "        return false;\n"
        "    }\n"
        "}",
        50,
    },
    {
        "67749b7abdcccddf7ad4288f7a1005b2",
        "static char *handle_search_graph(hyp_mcp_server_t *srv, const char *args) {\n"
        "    /* Inner phase split: every tool leaks the same ~4 MB per request, so the\n"
        "     * retainer is in what the handlers share -- store resolution or the query\n"
        "     * itself. These marks separate the two. */\n"
        "    hyp_mem_phase_mark(\"handler.args\");\n"
        "    project_choice_t choice;\n"
        "    char *project = resolve_project_arg(srv, args, &choice);\n"
        "    hyp_mem_phase_mark(\"handler.resolve_store\");\n"
        "    hyp_store_t *store = resolve_store(srv, project);\n"
        "    hyp_mem_phase_mark(\"handler.body\");\n"
        "    REQUIRE_STORE(store, project);\n"
        "\n"
        "    char *not_indexed = verify_project_indexed(store, project);\n"
        "    if (not_indexed) {\n"
        "        free(project);\n"
        "        return not_indexed;\n"
        "    }\n"
        "\n"
        "    /* Response encoding: grouped tree rows by default; format:\"json\" emits\n"
        "     * the SAME tree model as structured JSON (groups + row arrays). */\n"
        "    char *format_arg = hyp_mcp_get_string_arg(args, \"format\");\n"
        "    bool legacy_json = format_arg && strcmp(format_arg, \"json\") == 0;\n"
        "    free(format_arg);\n"
        "\n"
        "    /* BM25 path: if `query` is set, run FTS5 full-text search with ranking\n"
        "     * and return early.  The regex/vector path below is untouched for all\n"
        "     * other callers.  If FTS5 is unavailable or the query is empty after\n"
        "     * tokenization, fall through to the regex path. */\n"
        "    char *query = hyp_mcp_get_string_arg(args, \"query\");\n"
        "    if (query && query[0]) {\n"
        "        int q_limit = sg_clamp_limit(hyp_mcp_get_int_arg(args, \"limit\", SG_DEFAULT_LIMIT));\n"
        "        int q_offset = hyp_mcp_get_int_arg(args, \"offset\", 0);\n"
        "        char *q_file_pattern = hyp_mcp_get_string_arg(args, \"file_pattern\");\n"
        "        char *bm25_json =\n"
        "            bm25_search(store, project, query, q_file_pattern, q_limit, q_offset, !legacy_json);\n"
        "        free(q_file_pattern);\n"
        "        if (bm25_json) {\n"
        "            free(query);\n"
        "            char *result = hyp_mcp_text_result(bm25_json, false);\n"
        "            free(bm25_json);\n"
        "            /* The ranked path returns from inside a helper, so the disclosure\n"
        "             * is attached to the finished answer rather than written into the\n"
        "             * builder — same two fields, same order, either way. */\n"
        "            result = result_with_project_choice(result, project, &choice);\n"
        "            free(project);\n"
        "            return result;\n"
        "        }\n"
        "    }\n"
        "    free(query);\n"
        "\n"
        "    char *label = hyp_mcp_get_string_arg(args, \"label\");\n"
        "    char *name_pattern = hyp_mcp_get_string_arg(args, \"name_pattern\");\n"
        "    char *qn_pattern = hyp_mcp_get_string_arg(args, \"qn_pattern\");\n"
        "    char *file_pattern = hyp_mcp_get_string_arg(args, \"file_pattern\");\n"
        "    char *relationship = hyp_mcp_get_string_arg(args, \"relationship\");\n"
        "    bool exclude_entry_points = hyp_mcp_get_bool_arg(args, \"exclude_entry_points\");\n"
        "    bool include_connected = hyp_mcp_get_bool_arg(args, \"include_connected\");\n"
        "    int limit = sg_clamp_limit(hyp_mcp_get_int_arg(args, \"limit\", SG_DEFAULT_LIMIT));\n"
        "    int offset = hyp_mcp_get_int_arg(args, \"offset\", 0);\n"
        "    int min_degree = hyp_mcp_get_int_arg(args, \"min_degree\", HYP_NOT_FOUND);\n"
        "    int max_degree = hyp_mcp_get_int_arg(args, \"max_degree\", HYP_NOT_FOUND);\n"
        "\n"
        "    if (relationship && !validate_edge_type(relationship)) {\n"
        "        free(project);\n"
        "        free(label);\n"
        "        free(name_pattern);\n"
        "        free(qn_pattern);\n"
        "        free(file_pattern);\n"
        "        free(relationship);\n"
        "        return hyp_mcp_text_result(\"relationship must be uppercase letters and underscores\", true);\n"
        "    }\n"
        "\n"
        "    hyp_search_params_t params = {\n"
        "        .project = project,\n"
        "        .label = label,\n"
        "        .name_pattern = name_pattern,\n"
        "        .qn_pattern = qn_pattern,\n"
        "        .file_pattern = file_pattern,\n"
        "        .relationship = relationship,\n"
        "        .exclude_entry_points = exclude_entry_points,\n"
        "        .include_connected = include_connected,\n"
        "        .limit = limit,\n"
        "        .offset = offset,\n"
        "        .min_degree = min_degree,\n"
        "        .max_degree = max_degree,\n"
        "    };\n"
        "\n"
        "    if (!legacy_json) {\n"
        "        const char *fields[SG_MAX_EXTRA_FIELDS];\n"
        "        yyjson_doc *fields_owner = NULL;\n"
        "        char *sg_detail = hyp_mcp_get_string_arg(args, \"detail\");\n"
        "        bool detail_ids = sg_detail && strcmp(sg_detail, \"ids\") == 0;\n"
        "        free(sg_detail);\n"
        "        bool core_fields_requested = false;\n"
        "        int nfields = sg_parse_fields(args, fields, SG_MAX_EXTRA_FIELDS, &fields_owner,\n"
        "                                      &core_fields_requested);\n"
        "\n"
        "        hyp_vector_result_t *vresults = NULL;\n"
        "        int vcount = 0;\n"
        "        bool sq_present = false;\n"
        "        bool sq_type_error =\n"
        "            run_semantic_query_core(args, store, project, limit, &vresults, &vcount, &sq_present);\n"
        "        if (!sq_type_error) {\n"
        "            /* Semantic-only calls get semantic results only: the legacy\n"
        "             * behavior also ran the UNFILTERED regex search and prepended\n"
        "             * up to `limit` unrelated enriched nodes to the response. */\n"
        "            bool has_filters = label || name_pattern || qn_pattern || file_pattern ||\n"
        "                               relationship || exclude_entry_points ||\n"
        "                               min_degree != HYP_NOT_FOUND || max_degree != HYP_NOT_FOUND;\n"
        "            bool semantic_only = sq_present && !has_filters;\n"
        "\n"
        "            hyp_sb_t sb;\n"
        "            hyp_sb_init(&sb);\n"
        "            emit_project_choice_toon(&sb, project, &choice);\n"
        "            hyp_search_output_t tout = {0};\n"
        "            if (!semantic_only) {\n"
        "                hyp_store_search(store, &params, &tout);\n"
        "                /* Grouped tree output is THE default; the flat table remains\n"
        "                 * only for detail:\"ids\" (single column — nothing to group). */\n"
        "                if (detail_ids) {\n"
        "                    emit_search_results_toon(&sb, &tout, offset, fields, nfields, detail_ids);\n"
        "                } else {\n"
        "                    emit_search_results_tree(&sb, &tout, offset, fields, nfields, store,\n"
        "                                             relationship, include_connected);\n"
        "                }\n"
        "                if (core_fields_requested) {\n"
        "                    hyp_tree_scalar_str(\n"
        "                        &sb, \"hint\",\n"
        "                        \"some requested fields (file/name/qn/label/lines) are already core \"\n"
        "                        \"row columns and were skipped — `fields` is for extra property \"\n"
        "                        \"columns like complexity, cognitive, signature\");\n"
        "                }\n"
        "                if (tout.total == 0) {\n"
        "                    if (name_pattern && label) {\n"
        "                        hyp_tree_scalar_str(&sb, \"hint\",\n"
        "                                            \"No results. Try removing the label filter or \"\n"
        "                                            \"broadening the name_pattern regex.\");\n"
        "                    } else if (name_pattern) {\n"
        "                        hyp_tree_scalar_str(\n"
        "                            &sb, \"hint\",\n"
        "                            \"No nodes match this pattern. Check spelling or try a broader regex.\");\n"
        "                    } else if (label) {\n"
        "                        hyp_tree_scalar_str(&sb, \"hint\",\n"
        "                                            \"No nodes with this label. Available labels: \"\n"
        "                                            \"Function, Method, Class, Interface, Route, \"\n"
        "                                            \"Variable, Module, Package, File, Folder.\");\n"
        "                    }\n"
        "                }\n"
        "            }\n"
        "            if (vcount > 0) {\n"
        "                emit_semantic_results_toon(&sb, vresults, vcount);\n"
        "            } else if (semantic_only) {\n"
        "                static const char *const sem_cols[] = {\"qn\", \"label\", \"file\", \"score\"};\n"
        "                hyp_tree_table_header(&sb, \"semantic\", 0, sem_cols, 4);\n"
        "                hyp_tree_scalar_str(&sb, \"hint\",\n"
        "                                    \"No semantic matches. semantic_query needs a moderate/full \"\n"
        "                                    \"index; try broader or fewer keywords.\");\n"
        "            }\n"
        "            if (vcount > 0) {\n"
        "                hyp_store_free_vector_results(vresults, vcount);\n"
        "            }\n"
        "            if (fields_owner) {\n"
        "                yyjson_doc_free(fields_owner);\n"
        "            }\n"
        "            hyp_store_search_free(&tout);\n"
        "            free(project);\n"
        "            free(label);\n"
        "            free(name_pattern);\n"
        "            free(qn_pattern);\n"
        "            free(file_pattern);\n"
        "            free(relationship);\n"
        "            char *text = hyp_sb_finish(&sb);\n"
        "            char *result = hyp_mcp_text_result(text ? text : \"out of memory\", text == NULL);\n"
        "            free(text);\n"
        "            return result;\n"
        "        }\n"
        "        /* semantic_query type error: fall through to the shared error text. */\n"
        "        if (fields_owner) {\n"
        "            yyjson_doc_free(fields_owner);\n"
        "        }\n"
        "        free(project);\n"
        "        free(label);\n"
        "        free(name_pattern);\n"
        "        free(qn_pattern);\n"
        "        free(file_pattern);\n"
        "        free(relationship);\n"
        "        return hyp_mcp_text_result(\n"
        "            \"semantic_query must be an array of keyword strings, e.g. \"\n"
        "            \"[\\\"send\\\",\\\"pubsub\\\",\\\"publish\\\"] — not a single string. Split your query \"\n"
        "            \"into individual keywords; each is scored independently via per-keyword \"\n"
        "            \"min-cosine.\",\n"
        "            true);\n"
        "    }\n"
        "\n"
        "    hyp_search_output_t out = {0};\n"
        "    hyp_store_search(store, &params, &out);\n"
        "\n"
        "    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);\n"
        "    yyjson_mut_val *root = yyjson_mut_obj(doc);\n"
        "    yyjson_mut_doc_set_root(doc, root);\n"
        "    add_project_choice_json(doc, root, project, &choice);\n"
        "\n"
        "    /* format:\"json\" = json-stringified tree: same grouped model as the\n"
        "     * default text output, structured for parsing. include_connected adds a\n"
        "     * nested per-row `connected` array — the legacy per-node-object shape is\n"
        "     * gone. */\n"
        "    {\n"
        "        const char *jfields[SG_MAX_EXTRA_FIELDS];\n"
        "        yyjson_doc *jfields_owner = NULL;\n"
        "        int jnfields = sg_parse_fields(args, jfields, SG_MAX_EXTRA_FIELDS, &jfields_owner, NULL);\n"
        "        emit_search_results_tree_json(doc, root, &out, offset, jfields, jnfields, store,\n"
        "                                      relationship, include_connected);\n"
        "        if (jfields_owner) {\n"
        "            yyjson_doc_free(jfields_owner);\n"
        "        }\n"
        "    }\n"
        "\n"
        "    /* Add diagnostic hint when zero results */\n"
        "    if (out.total == 0) {\n"
        "        if (name_pattern && label) {\n"
        "            yyjson_mut_obj_add_str(\n"
        "                doc, root, \"hint\",\n"
        "                \"No results. Try removing the label filter or broadening the name_pattern regex.\");\n"
        "        } else if (name_pattern) {\n"
        "            yyjson_mut_obj_add_str(\n"
        "                doc, root, \"hint\",\n"
        "                \"No nodes match this pattern. Check spelling or try a broader regex.\");\n"
        "        } else if (label) {\n"
        "            yyjson_mut_obj_add_str(\n"
        "                doc, root, \"hint\",\n"
        "                \"No nodes with this label. Available labels: Function, Method, Class, \"\n"
        "                \"Interface, Route, Variable, Module, Package, File, Folder.\");\n"
        "        }\n"
        "    }\n"
        "\n"
        "    bool sq_type_error = run_semantic_query(doc, root, args, store, project, limit);\n"
        "\n"
        "    if (sq_type_error) {\n"
        "        yyjson_mut_doc_free(doc);\n"
        "        hyp_store_search_free(&out);\n"
        "        free(project);\n"
        "        free(label);\n"
        "        free(name_pattern);\n"
        "        free(qn_pattern);\n"
        "        free(file_pattern);\n"
        "        free(relationship);\n"
        "        return hyp_mcp_text_result(\n"
        "            \"semantic_query must be an array of keyword strings, e.g. \"\n"
        "            \"[\\\"send\\\",\\\"pubsub\\\",\\\"publish\\\"] — not a single string. Split your query \"\n"
        "            \"into individual keywords; each is scored independently via per-keyword \"\n"
        "            \"min-cosine.\",\n"
        "            true);\n"
        "    }\n"
        "\n"
        "    char *json = yy_doc_to_str(doc);\n"
        "    yyjson_mut_doc_free(doc);\n"
        "    hyp_store_search_free(&out);\n"
        "\n"
        "    free(project);\n"
        "    free(label);\n"
        "    free(name_pattern);\n"
        "    free(qn_pattern);\n"
        "    free(file_pattern);\n"
        "    free(relationship);\n"
        "\n"
        "    char *result = hyp_mcp_text_result(json, false);\n"
        "    free(json);\n"
        "    return result;\n"
        "}",
        269,
    },
    {
        "56ae0fbef4861b6c9e7addab1790fde7",
        "char *cbm_mcp_text_result(const char *text, bool is_error) {\n"
        "    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);\n"
        "    yyjson_mut_val *root = yyjson_mut_obj(doc);\n"
        "    yyjson_mut_doc_set_root(doc, root);\n"
        "\n"
        "    yyjson_mut_val *content = yyjson_mut_arr(doc);\n"
        "    yyjson_mut_val *item = yyjson_mut_obj(doc);\n"
        "    yyjson_mut_obj_add_str(doc, item, \"type\", \"text\");\n"
        "    yyjson_mut_obj_add_str(doc, item, \"text\", text ? text : \"\");\n"
        "    yyjson_mut_arr_add_val(content, item);\n"
        "    yyjson_mut_obj_add_val(doc, root, \"content\", content);\n"
        "\n"
        "    bool has_structured_content = false;\n"
        "    if (text) {\n"
        "        yyjson_doc *structured_doc = yyjson_read(text, strlen(text), 0);\n"
        "        if (structured_doc) {\n"
        "            yyjson_val *structured_root = yyjson_doc_get_root(structured_doc);\n"
        "            if (yyjson_is_obj(structured_root)) {\n"
        "                yyjson_mut_val *structured = yyjson_val_mut_copy(doc, structured_root);\n"
        "                if (structured) {\n"
        "                    yyjson_mut_obj_add_val(doc, root, \"structuredContent\", structured);\n"
        "                    has_structured_content = true;\n"
        "                }\n"
        "            }\n"
        "            yyjson_doc_free(structured_doc);\n"
        "        }\n"
        "    }\n"
        "    if (!has_structured_content) {\n"
        "        /* Every advertised MCP tool declares an object outputSchema, so a\n"
        "         * conforming structuredContent object is mandatory even for compact\n"
        "         * TOON/plain-text and error results. What is NOT mandatory is putting\n"
        "         * the whole payload in it a second time.\n"
        "         *\n"
        "         * It used to. For any result that is not a JSON object — which is every\n"
        "         * TOON answer, i.e. the large ones — structuredContent was\n"
        "         * {\"text\": <the entire payload>} sitting beside an identical\n"
        "         * content[0].text. Measured at 2.05x the payload on a 20k-node\n"
        "         * query_graph, so half of every reply was redundant bytes: half the\n"
        "         * usable transport budget, and double the tokens billed to every LLM\n"
        "         * caller (#1375).\n"
        "         *\n"
        "         * Nothing is lost by dropping it. structuredContent exists to carry\n"
        "         * STRUCTURE, and a string re-wrapped in a one-key object has none —\n"
        "         * a client parsing structuredContent.text learns exactly what\n"
        "         * content[0].text already told it. The empty object still satisfies\n"
        "         * outputSchema ({\"type\":\"object\",\"additionalProperties\":true}), and the\n"
        "         * JSON branch above is untouched: when a tool really does return an\n"
        "         * object, callers still get it parsed.\n"
        "         *\n"
        "         * Errors keep their payload. They are bounded and small, the duplication\n"
        "         * costs nothing measurable, and structuredContent.error is the only\n"
        "         * machine-readable form of a failure a client has. */\n"
        "        yyjson_mut_val *structured = yyjson_mut_obj(doc);\n"
        "        if (is_error) {\n"
        "            yyjson_mut_obj_add_str(doc, structured, \"error\", text ? text : \"\");\n"
        "        }\n"
        "        yyjson_mut_obj_add_val(doc, root, \"structuredContent\", structured);\n"
        "    }\n"
        "    yyjson_mut_obj_add_bool(doc, root, \"isError\", is_error);\n"
        "\n"
        "    char *out = yy_doc_to_str(doc);\n"
        "    yyjson_mut_doc_free(doc);\n"
        "    return out;\n"
        "}",
        64,
    },
    {
        "2b34f75d2d0799e0a59c8466e989b53b",
        "static cbm_daemon_runtime_application_status_t application_mcp_request(\n"
        "    cbm_daemon_application_session_t *session, const uint8_t *request, uint32_t request_length,\n"
        "    uint8_t **response_out, uint32_t *response_length_out) {\n"
        "    if (!session->context_set || request_length <= 1) {\n"
        "        return CBM_DAEMON_RUNTIME_APPLICATION_REJECTED;\n"
        "    }\n"
        "    char *message = application_text_copy(request + 1, request_length - 1);\n"
        "    if (!message) {\n"
        "        return CBM_DAEMON_RUNTIME_APPLICATION_REJECTED;\n"
        "    }\n"
        "    cbm_jsonrpc_request_t parsed = {0};\n"
        "    bool parsed_ok = cbm_jsonrpc_parse(message, &parsed) == 0;\n"
        "    bool initialize_request =\n"
        "        parsed_ok && parsed.has_id && parsed.method && strcmp(parsed.method, \"initialize\") == 0;\n"
        "    bool tool_request =\n"
        "        parsed_ok && parsed.has_id && parsed.method && strcmp(parsed.method, \"tools/call\") == 0;\n"
        "    char *response = cbm_mcp_server_handle(session->mcp, message);\n"
        "    free(message);\n"
        "    if (initialize_request && application_jsonrpc_success(response)) {\n"
        "        cbm_mutex_lock(&session->application->mutex);\n"
        "        session->pending_background_initialize = true;\n"
        "        cbm_mutex_unlock(&session->application->mutex);\n"
        "    } else if (tool_request && response) {\n"
        "        application_update_notice_inject(session, &response);\n"
        "    }\n"
        "    if (response) {\n"
        "        size_t response_length = strlen(response);\n"
        "        if (response_length > UINT32_MAX) {\n"
        "            if (parsed_ok) {\n"
        "                cbm_jsonrpc_request_free(&parsed);\n"
        "            }\n"
        "            free(response);\n"
        "            return CBM_DAEMON_RUNTIME_APPLICATION_HANDLER_ERROR;\n"
        "        }\n"
        "        /* A reply larger than one frame has to become a JSON-RPC error HERE,\n"
        "         * while the request id is still in hand. Handing it to the transport\n"
        "         * instead is indistinguishable from a dead socket at the frontend\n"
        "         * worker, which responds by _Exit()ing the process — right for a broken\n"
        "         * transport, a fatal over-reaction to a large answer. Under an MCP host\n"
        "         * that does not respawn the server, that took every tool on it out for\n"
        "         * the rest of the session, leaving exit=1 and an empty stderr to debug\n"
        "         * from (#1375). */\n"
        "        response = application_framable_response(response, parsed_ok ? &parsed : NULL);\n"
        "        if (!response) {\n"
        "            if (parsed_ok) {\n"
        "                cbm_jsonrpc_request_free(&parsed);\n"
        "            }\n"
        "            return CBM_DAEMON_RUNTIME_APPLICATION_HANDLER_ERROR;\n"
        "        }\n"
        "        response_length = strlen(response);\n"
        "        *response_out = (uint8_t *)response;\n"
        "        *response_length_out = (uint32_t)response_length;\n"
        "    }\n"
        "    if (parsed_ok) {\n"
        "        cbm_jsonrpc_request_free(&parsed);\n"
        "    }\n"
        "    application_refresh_watch(session);\n"
        "    return CBM_DAEMON_RUNTIME_APPLICATION_OK;\n"
        "}",
        59,
    },
    {
        "575be42a4e977de16555091af0aaae41",
        "void cbm_mem_init_with_cap(double ram_fraction, size_t hard_cap_bytes) {\n"
        "    int expected = 0;\n"
        "    if (!atomic_compare_exchange_strong(&g_initialized, &expected, 1)) {\n"
        "        return;\n"
        "    }\n"
        "\n"
        "    if (ram_fraction <= 0.0 || ram_fraction > MAX_RAM_FRACTION) {\n"
        "        ram_fraction = DEFAULT_RAM_FRACTION;\n"
        "    }\n"
        "\n"
        "    /* Reduce upfront memory: don't eagerly commit arenas.\n"
        "     * Force decommit on purge (MADV_FREE_REUSABLE on macOS) so RSS\n"
        "     * drops immediately instead of staying high until memory pressure. */\n"
        "#ifdef _WIN32\n"
        "    /* Decisive check: is the allocator still \"preloading\"? If so its arena\n"
        "     * machinery — including every purge path — is inert, and completing init\n"
        "     * here is what turns the options above from decoration into behaviour. */\n"
        "    if (_mi_preloading()) {\n"
        "        _mi_auto_process_init();\n"
        "        if (_mi_preloading()) {\n"
        "            /* Arena creation and every purge path stay disabled while this is\n"
        "             * set, so the process would grow until the OS killed it. Refuse. */\n"
        "            mem_setup_fatal(\"preloading\",\n"
        "                            \"allocator remained in preloading state after explicit process \"\n"
        "                            \"init: arena creation and purging are disabled, memory would \"\n"
        "                            \"never be returned to the OS\");\n"
        "        }\n"
        "        cbm_log_warn(\"mem.allocator.preloading_completed\", \"still_preloading\", \"false\", \"detail\",\n"
        "                     \"allocator was still preloading, so arena creation and purging were \"\n"
        "                     \"disabled; process init completed explicitly\");\n"
        "    } else {\n"
        "        cbm_log_info(\"mem.allocator.preloading\", \"state\", \"already_complete\");\n"
        "    }\n"
        "#endif\n"
        "\n"
        "    mem_option_set_verified(mi_option_arena_eager_commit, 0, \"arena_eager_commit\");\n"
        "    mem_option_set_verified(mi_option_purge_decommits, SKIP_ONE, \"purge_decommits\");\n"
        "    mem_option_set_verified(mi_option_purge_delay, 0, \"purge_delay\"); /* immediate */\n"
        "    /* v3 (#832): reclaim abandoned pages on ANY thread's free (=1), restoring the\n"
        "     * v2 behaviour. mimalloc v3 defaults page_reclaim_on_free=0, so pages a worker\n"
        "     * thread abandons at exit are NOT reclaimed when the main thread later frees\n"
        "     * their blocks (and mi_collect cannot touch abandoned pages) — RSS then\n"
        "     * ratchets across repeated in-process index cycles. The supervised subprocess\n"
        "     * is the primary cure (the child returns 100% RSS on exit); this is the\n"
        "     * in-process fallback for any path that stays in-process (kill switch,\n"
        "     * spawn-fail degrade, embedders). */\n"
        "    mem_option_set_verified(mi_option_page_reclaim_on_free, 1, \"page_reclaim_on_free\");\n"
        "\n"
        "    /* Every option above is inert unless ordinary malloc actually reaches this\n"
        "     * allocator. That silently stopped being true on Windows — mimalloc's\n"
        "     * static override is gated on _MSC_VER, which clang/MinGW never defines —\n"
        "     * and nothing checked, so a long-lived daemon ratcheted committed memory\n"
        "     * for months (#581). Probe it once, out loud: a real malloc asked whether\n"
        "     * mimalloc owns it. Anything but true means the tuning here is decoration\n"
        "     * and freed pages will not come back.\n"
        "     *\n"
        "     * ...but \"anything but true\" only means that where the build actually ASKED\n"
        "     * for a global override: prod builds on Windows and Linux. Everywhere else —\n"
        "     * macOS, and every test binary on any platform — the override body is\n"
        "     * compiled out, so ordinary malloc is SUPPOSED to reach libc and the probe\n"
        "     * is answering a question this build never posed. Same measurement,\n"
        "     * different meaning per build config, hence the split below. */\n"
        "    cbm_mem_ownership_audit_t audit;\n"
        "    cbm_mem_audit_ownership(&audit);\n"
        "    char owned_str[CBM_SZ_32];\n"
        "    snprintf(owned_str, sizeof(owned_str), \"%d/%d\", audit.owned_count, audit.probed_count);\n"
        "    if (!audit.all_owned && !CBM_MEM_EXPECT_GLOBAL_OVERRIDE) {\n"
        "        /* Expected: this build never asked mimalloc to replace ordinary malloc,\n"
        "         * so ordinary malloc reaching libc is the design, not a fault. Say what\n"
        "         * IS allocator-served instead, because the interesting question here is\n"
        "         * \"are the bound populations bound\", not \"did the override fire\".\n"
        "         *\n"
        "         * Reported at INFO deliberately. A warning that fires on every run of a\n"
        "         * correctly configured build is not a tripwire, it is background noise —\n"
        "         * it trained readers to ignore the one line that catches #581, and cost\n"
        "         * a user the time to file and self-close #1360. */\n"
        "        cbm_log_info(\"mem.allocator.bound_populations_only\", \"owned_classes\", owned_str,\n"
        "                     \"populations\", \"sqlite,tree_sitter\", \"detail\",\n"
        "                     \"ordinary malloc is served by the system allocator in this build by \"\n"
        "                     \"design; allocator tuning applies to the bound populations\");\n"
        "    } else if (!audit.all_owned) {\n"
        "        /* Name the classes that escaped, because \"not owned\" is actionable\n"
        "         * only if you know WHICH sizes. A class listed here either bypasses\n"
        "         * the override or is misclassified by the routing predicate, and both\n"
        "         * mean freed pages will not come back. */\n"
        "        char classes[CBM_SZ_256];\n"
        "        int length = 0;\n"
        "        for (int i = 0;\n"
        "             i < CBM_MEM_OWNERSHIP_CLASSES && length >= 0 && (size_t)length < sizeof(classes);\n"
        "             i++) {\n"
        "            if (audit.allocated[i] && !audit.owned[i]) {\n"
        "                int written = snprintf(classes + length, sizeof(classes) - (size_t)length, \"%s%zu\",\n"
        "                                       length ? \",\" : \"\", audit.probe_bytes[i]);\n"
        "                if (written < 0) {\n"
        "                    break;\n"
        "                }\n"
        "                length += written;\n"
        "            }\n"
        "        }\n"
        "        cbm_log_warn(\"mem.allocator.not_owned\", \"owned_classes\", owned_str, \"unowned_bytes\",\n"
        "                     length > 0 ? classes : \"?\", \"detail\",\n"
        "                     \"allocations in these size classes are not allocator-owned: purge and \"\n"
        "                     \"reclaim options do not apply to them and freed pages stay committed\");\n"
        "    } else {\n"
        "        cbm_log_info(\"mem.allocator.owned\", \"classes\", \"all\");\n"
        "    }\n"
        "\n"
        "    /* CBM_MEM_BUDGET_MB env override (memory analogue of CBM_WORKERS).\n"
        "     * Lets users cap the budget directly without an enclosing cgroup —\n"
        "     * useful on bare-metal hosts where cgroup memory limits are absent\n"
        "     * (#363). Explicit override > implicit RAM/cgroup detection. The budget\n"
        "     * math (fraction default, override, clamp-to-total) lives in the pure,\n"
        "     * testable cbm_mem_resolve_budget(); this path only reads the env and\n"
        "     * emits the log/warn lines. */\n"
        "    cbm_system_info_t info = cbm_system_info();\n"
        "\n"
        "    char env_buf[CBM_SZ_32];\n"
        "    const char *env = cbm_safe_getenv(\"CBM_MEM_BUDGET_MB\", env_buf, sizeof(env_buf), NULL);\n"
        "    cbm_mem_budget_t resolved =\n"
        "        cbm_mem_resolve_budget_capped(info.total_ram, ram_fraction, env, hard_cap_bytes);\n"
        "    g_budget = resolved.budget;\n"
        "\n"
        "    /* The resolver is the single source of truth for the parse + clamp; this\n"
        "     * path only surfaces its outcome as log lines. */\n"
        "    if (resolved.invalid) {\n"
        "        cbm_log_warn(\"mem.budget.env.invalid\", \"value\", env, \"fallback\", \"ram_fraction\");\n"
        "    } else if (resolved.clamped) {\n"
        "        char cap_mb[CBM_SZ_32];\n"
        "        snprintf(cap_mb, sizeof(cap_mb), \"%zu\", info.total_ram / MB_DIVISOR);\n"
        "        cbm_log_warn(\"mem.budget.clamped\", \"requested_mb\", env, \"cap_mb\", cap_mb);\n"
        "    }\n"
        "    if (resolved.hard_capped) {\n"
        "        char cap_bytes[CBM_SZ_32];\n"
        "        snprintf(cap_bytes, sizeof(cap_bytes), \"%zu\", hard_cap_bytes);\n"
        "        cbm_log_info(\"mem.budget.worker_cap\", \"cap_bytes\", cap_bytes);\n"
        "    }\n"
        "\n"
        "    char budget_mb[CBM_SZ_32];\n"
        "    char ram_mb[CBM_SZ_32];\n"
        "    snprintf(budget_mb, sizeof(budget_mb), \"%zu\", g_budget / MB_DIVISOR);\n"
        "    snprintf(ram_mb, sizeof(ram_mb), \"%zu\", info.total_ram / MB_DIVISOR);\n"
        "    cbm_log_info(\"mem.init\", \"budget_mb\", budget_mb, \"total_ram_mb\", ram_mb, \"source\",\n"
        "                 resolved.source);\n"
        "}",
        144,
    },
    {
        "9f8fb9ffba4241506f384c5f80926e2a",
        "static ask_llama_t *ask_llama_open(hyp_ask_device_pref_t pref, char *err, size_t errlen) {\n"
        "    char path[HYP_MODEL_PATH_MAX];\n"
        "    if (!hyp_model_ask_path(path, sizeof(path)) || path[0] == '\\0') {\n"
        "        (void)snprintf(err, errlen, \"the model cache directory cannot be resolved\");\n"
        "        return NULL;\n"
        "    }\n"
        "    if (!hyp_model_ask_present()) {\n"
        "        (void)snprintf(err, errlen,\n"
        "                       \"the ask lane's embedding weights are not on this machine (%s). Run `%s`.\",\n"
        "                       path, HYP_MODEL_ASK_COMMAND);\n"
        "        return NULL;\n"
        "    }\n"
        "\n"
        "    ask_llama_backend_once();\n"
        "\n"
        "    ask_llama_t *s = (ask_llama_t *)calloc(1, sizeof(*s));\n"
        "    if (!s) {\n"
        "        (void)snprintf(err, errlen, \"out of memory\");\n"
        "        return NULL;\n"
        "    }\n"
        "\n"
        "    /* WHICH DEVICE. The request is an input; what follows is the outcome, and\n"
        "     * they are recorded separately so a run that asked for a GPU and got a CPU\n"
        "     * is visible rather than merely fifteen times slower. */\n"
        "    char gpu_name[128];\n"
        "    bool gpu_available = (pref != HYP_ASK_DEVICE_CPU) && ask_llama_gpu_present(gpu_name,\n"
        "                                                                               sizeof(gpu_name));\n"
        "    double total = 0.0;\n"
        "    double used = 0.0;\n"
        "    bool vram_readable = hyp_ask_device_vram_mib(&total, &used);\n"
        "    bool use_gpu = false;\n"
        "\n"
        "    if (gpu_available) {\n"
        "        if (!vram_readable) {\n"
        "            /* A ceiling that cannot be CHECKED must never be ASSUMED — see\n"
        "             * the two lost desktop sessions in ask_batch.h. */\n"
        "            if (pref == HYP_ASK_DEVICE_GPU) {\n"
        "                (void)snprintf(err, errlen,\n"
        "                               \"--device gpu: a GPU is present (%s) but no device exposes its VRAM \"\n"
        "                               \"to this process, so the allocation ceiling cannot be checked. \"\n"
        "                               \"REFUSING rather than guessing; use --device auto or cpu\",\n"
        "                               gpu_name);\n"
        "                free(s);\n"
        "                return NULL;\n"
        "            }\n"
        "        } else {\n"
        "            s->ceiling_mib = hyp_ask_gpu_ceiling_mib(total, used);\n"
        "            hyp_ask_kv_plan_t probe;\n"
        "            /* The smallest bucket has to fit at one sequence, or there is no\n"
        "             * GPU pass to be had at all. */\n"
        "            use_gpu = hyp_ask_kv_plan(1, ASK_SEQ_BUCKETS[0], s->ceiling_mib, &probe);\n"
        "            if (!use_gpu && pref == HYP_ASK_DEVICE_GPU) {\n"
        "                (void)snprintf(err, errlen,\n"
        "                               \"--device gpu: %.0f MiB of %.0f MiB VRAM is free (this card also \"\n"
        "                               \"drives the display), giving a %.0f MiB ceiling — not enough for \"\n"
        "                               \"the backend's %.0f MiB fixed cost plus a minimum KV cache. \"\n"
        "                               \"REFUSING rather than attempting\",\n"
        "                               total - used, total, s->ceiling_mib, HYP_ASK_LLAMA_FIXED_MIB);\n"
        "                free(s);\n"
        "                return NULL;\n"
        "            }\n"
        "        }\n"
        "    } else if (pref == HYP_ASK_DEVICE_GPU) {\n"
        "        (void)snprintf(err, errlen,\n"
        "                       \"--device gpu: this build has no GPU backend it can reach. %s\",\n"
        "                       llama_supports_gpu_offload()\n"
        "                           ? \"A GPU backend is compiled in but no device answered.\"\n"
        "                           : \"This binary was built with HYP_ASK_GPU=none.\");\n"
        "        free(s);\n"
        "        return NULL;\n"
        "    }\n"
        "\n"
        "    struct llama_model_params mp = llama_model_default_params();\n"
        "    mp.n_gpu_layers = use_gpu ? ASK_NGL_ALL : 0;\n"
        "    s->model = llama_model_load_from_file(path, mp);\n"
        "    if (!s->model) {\n"
        "        (void)snprintf(err, errlen, \"llama could not load the GGUF at %s\", path);\n"
        "        free(s);\n"
        "        return NULL;\n"
        "    }\n"
        "    s->vocab = llama_model_get_vocab(s->model);\n"
        "    s->n_embd = llama_model_n_embd(s->model);\n"
        "    s->model_window = HYP_ASK_MODEL_WINDOW;\n"
        "    {\n"
        "        int trained = llama_model_n_ctx_train(s->model);\n"
        "        if (trained > 0 && trained < s->model_window) {\n"
        "            s->model_window = trained;\n"
        "        }\n"
        "    }\n"
        "    /* THE NON-CAUSAL CEILING. A bidirectional model cannot have its sequence\n"
        "     * split across micro-batches, so the longest document this lane can encode\n"
        "     * is bounded by the largest ubatch its compute buffer can afford — not by\n"
        "     * what the weights support. Measured on this card: a 8,192 ubatch prices\n"
        "     * the compute buffer at 5,284 MiB and fits; 32,768 asks for 21,135 MiB\n"
        "     * against a 9,490 MiB ceiling and aborts, on the GPU AND on CPU.\n"
        "     *\n"
        "     * So the effective window is clamped HERE, at the one place that already\n"
        "     * feeds the truncation counter — `model_window` is what the counter is\n"
        "     * denominated in, so every declaration cut by this ceiling is DISCLOSED on\n"
        "     * every answer rather than silently shortened. It is also the window §2.9\n"
        "     * measured nano at, which is why the number below is 8,192 and not a\n"
        "     * rounder guess. */\n"
        "    if (s->model_window > HYP_ASK_NONCAUSAL_MAX_SEQ) {\n"
        "        s->model_window = HYP_ASK_NONCAUSAL_MAX_SEQ;\n"
        "    }\n"
        "    s->on_gpu = use_gpu;\n"
        "\n"
        "    if (s->n_embd != HYP_ASK_DIM) {\n"
        "        (void)snprintf(err, errlen,\n"
        "                       \"the GGUF emits %d dimensions, not the %d every measured number here is \"\n"
        "                       \"about\",\n"
        "                       s->n_embd, HYP_ASK_DIM);\n"
        "        llama_model_free(s->model);\n"
        "        free(s);\n"
        "        return NULL;\n"
        "    }\n"
        "\n"
        "    /* THE SECOND ARTIFACT. Without it this encoder would emit nano's\n"
        "     * pre-projection hidden state: same width, unit length, different space,\n"
        "     * and nothing downstream could tell. So a missing or wrong-sized\n"
        "     * projection is fatal at open, never a degraded mode. */\n"
        "    if (!ask_llama_load_projection(s, err, errlen)) {\n"
        "        llama_model_free(s->model);\n"
        "        free(s);\n"
        "        return NULL;\n"
        "    }\n"
        "\n"
        "    if (use_gpu) {\n"
        "        (void)snprintf(s->device_note, sizeof(s->device_note),\n"
        "                       \"GPU (%s, Vulkan) — %.0f MiB free of %.0f MiB, ceiling %.0f MiB\", gpu_name,\n"
        "                       total - used, total, s->ceiling_mib);\n"
        "    } else if (pref == HYP_ASK_DEVICE_AUTO && gpu_available) {\n"
        "        (void)snprintf(s->device_note, sizeof(s->device_note),\n"
        "                       \"CPU (%d threads) — DOWNGRADED from the GPU that is present (%s): not \"\n"
        "                       \"enough free VRAM for a safe allocation\",\n"
        "                       ASK_CPU_THREADS, gpu_name);\n"
        "    } else {\n"
        "        (void)snprintf(s->device_note, sizeof(s->device_note), \"CPU (%d threads)\",\n"
        "                       ASK_CPU_THREADS);\n"
        "    }\n"
        "\n"
        "    /* The model id is the identity of the WEIGHTS AND their configuration —\n"
        "     * it is what `ask` refuses a mixed index on, so the quantisation and the\n"
        "     * pinned revision both belong in it. */\n"
        "    (void)snprintf(s->model_id, sizeof(s->model_id), \"%s-%s@%.12s\", HYP_MODEL_ASK_MODEL,\n"
        "                   HYP_MODEL_ASK_QUANT, HYP_MODEL_ASK_REVISION);\n"
        "\n"
        "    (void)ask_toks_reserve(s, 4096);\n"
        "    return s;\n"
        "}",
        150,
    },
    {
        "7cd2a9643bc78f7195415033c08b653a",
        "static char *handle_ask(hyp_mcp_server_t *srv, const char *args) {\n"
        "    char *project = get_project_arg(args);\n"
        "    hyp_store_t *store = resolve_store(srv, project);\n"
        "    REQUIRE_STORE(store, project);\n"
        "\n"
        "    char *not_indexed = verify_project_indexed(store, project);\n"
        "    if (not_indexed) {\n"
        "        free(project);\n"
        "        return not_indexed;\n"
        "    }\n"
        "\n"
        "    char *format_arg = hyp_mcp_get_string_arg(args, \"format\");\n"
        "    bool json = format_arg && strcmp(format_arg, \"json\") == 0;\n"
        "    free(format_arg);\n"
        "\n"
        "    /* Argument validation before availability: a malformed call is the\n"
        "     * caller's to fix whether or not the lane happens to be built today. */\n"
        "    char *question = hyp_mcp_get_string_arg(args, \"question\");\n"
        "    if (!question || !question[0]) {\n"
        "        /* Distinguish \"absent\" from \"wrong type\", because the wrong type an\n"
        "         * agent actually reaches for is the array — semantic_query's shape —\n"
        "         * and pointing at the right tool costs one sentence. */\n"
        "        bool was_array = false;\n"
        "        yyjson_doc *ad = yyjson_read(args, strlen(args), 0);\n"
        "        if (ad) {\n"
        "            yyjson_val *q = yyjson_obj_get(yyjson_doc_get_root(ad), \"question\");\n"
        "            was_array = q && yyjson_is_arr(q);\n"
        "            yyjson_doc_free(ad);\n"
        "        }\n"
        "        if (was_array) {\n"
        "            return ask_error(\"ask takes ONE natural-language string, not an array — e.g. \"\n"
        "                             \"question=\\\"how does the writer decide section ordering\\\". For an \"\n"
        "                             \"array of keywords use search_graph(semantic_query=[\\\"send\\\",\"\n"
        "                             \"\\\"publish\\\"]), which is a different lane with different result \"\n"
        "                             \"semantics.\",\n"
        "                             project, question);\n"
        "        }\n"
        "        return ask_error(\"question is required: one natural-language string, e.g. \"\n"
        "                         \"question=\\\"how does the writer decide section ordering\\\".\",\n"
        "                         project, question);\n"
        "    }\n"
        "    if (strlen(question) > ASK_QUESTION_MAX) {\n"
        "        return ask_error(\"question is too long. This lane encodes one QUESTION, not a document; \"\n"
        "                         \"if you have a body of text to match against, that is the indexing \"\n"
        "                         \"side of the lane, not the query side.\",\n"
        "                         project, question);\n"
        "    }\n"
        "\n"
        "    int limit = hyp_mcp_get_int_arg(args, \"limit\", ASK_DEFAULT_LIMIT);\n"
        "    if (limit <= 0) {\n"
        "        limit = ASK_DEFAULT_LIMIT;\n"
        "    }\n"
        "    if (limit > ASK_MAX_LIMIT) {\n"
        "        limit = ASK_MAX_LIMIT;\n"
        "    }\n"
        "\n"
        "    hyp_ask_status_t st;\n"
        "    hyp_ask_index_status(store, project, &st);\n"
        "    /* THE WEIGHTS, BEFORE THE INDEX. hyp_ask_index_status answers about the\n"
        "     * store and the linked encoder; whether that encoder's 639 MB of weights\n"
        "     * are on disk is the fetcher's question and is asked here, where the CLI\n"
        "     * layer is already reachable. Order matters: someone with neither weights\n"
        "     * nor index must be told to fetch the model, because the embed pass that\n"
        "     * builds the index is the thing that needs it. */\n"
        "    if ((st.avail == HYP_ASK_NO_INDEX || st.avail == HYP_ASK_AVAILABLE) &&\n"
        "        hyp_ask_llama_backend_installed() && !hyp_model_ask_present()) {\n"
        "        st.avail = HYP_ASK_NO_WEIGHTS;\n"
        "    }\n"
        "    if (st.avail != HYP_ASK_AVAILABLE) {\n"
        "        char *result = ask_emit_unavailable(&st, project, json);\n"
        "        free(project);\n"
        "        free(question);\n"
        "        return result;\n"
        "    }\n"
        "\n"
        "    /* Language: explicit wins, derivation is the fallback, and NEITHER falls\n"
        "     * back to a default. A question about Rust encoded behind the C++ prefix\n"
        "     * still returns unit vectors and a plausible ranking — there is no\n"
        "     * downstream signal at all, so the signal has to be here. */\n"
        "    char *lang_arg = hyp_mcp_get_string_arg(args, \"language\");\n"
        "    bool lang_explicit = lang_arg && lang_arg[0];\n"
        "    HYPLanguage lang =\n"
        "        lang_explicit ? hyp_ask_resolve_language(lang_arg) : ask_derive_language(store, project);\n"
        "    if (lang_explicit && lang == HYP_LANG_COUNT) {\n"
        "        char msg[ASK_MSG];\n"
        "        snprintf(msg, sizeof(msg),\n"
        "                 \"language \\\"%s\\\" is not recognised. Pass an extension (\\\"cpp\\\", \\\"rs\\\", \\\"py\\\") \"\n"
        "                 \"or a display name (\\\"C++\\\", \\\"Rust\\\", \\\"Python\\\"). It is not defaulted, \"\n"
        "                 \"because the wrong language renders a prefix that still ranks and still \"\n"
        "                 \"returns answers — just not the ones any measurement describes.\",\n"
        "                 lang_arg);\n"
        "        free(lang_arg);\n"
        "        return ask_error(msg, project, question);\n"
        "    }\n"
        "    free(lang_arg);\n"
        "    const char *lang_name = hyp_ask_language_display(lang);\n"
        "    if (!lang_name) {\n"
        "        return ask_error(\"could not determine which language's instruct prefix to render: no \"\n"
        "                         \"file in this project's graph maps to a known language. Pass \"\n"
        "                         \"language=\\\"...\\\" explicitly (an extension like \\\"cpp\\\" or a display \"\n"
        "                         \"name like \\\"C++\\\").\",\n"
        "                         project, question);\n"
        "    }\n"
        "\n"
        "    const hyp_ask_backend_t *backend = hyp_ask_backend();\n"
        "    float *qvec = (float *)calloc(HYP_ASK_DIM, sizeof(float));\n"
        "    if (!qvec) {\n"
        "        return ask_error(\"out of memory encoding the question\", project, question);\n"
        "    }\n"
        "    char encerr[ASK_MSG] = \"\";\n"
        "    if (backend->encode_query(lang, question, qvec, encerr, sizeof(encerr)) != 0) {\n"
        "        char msg[ASK_MSG + HYP_SZ_128];\n"
        "        snprintf(msg, sizeof(msg), \"could not encode the question: %s\",\n"
        "                 encerr[0] ? encerr : \"the encoder failed without a reason\");\n"
        "        free(qvec);\n"
        "        return ask_error(msg, project, question);\n"
        "    }\n"
        "\n"
        "    hyp_ask_hit_t *hits = NULL;\n"
        "    int hit_count = 0;\n"
        "    int rc = hyp_ask_index_search(store, project, qvec, limit, &hits, &hit_count);\n"
        "    free(qvec);\n"
        "    if (rc != HYP_STORE_OK) {\n"
        "        return ask_error(\"the semantic index could not be read. Re-run index_status; if the \"\n"
        "                         \"project is healthy the vector store needs rebuilding.\",\n"
        "                         project, question);\n"
        "    }\n"
        "\n"
        "    char trunc_text[ASK_MSG];\n"
        "    ask_truncation_text(&st, trunc_text, sizeof(trunc_text));\n"
        "    bool with_cut = st.trunc == HYP_ASK_TRUNC_SOME;\n"
        "    const char *cols[6];\n"
        "    int ncols = 0;\n"
        "    ask_cols(cols, &ncols, with_cut);\n"
        "\n"
        "    char *result = NULL;\n"
        "    if (json) {\n"
        "        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);\n"
        "        yyjson_mut_val *root = yyjson_mut_obj(doc);\n"
        "        yyjson_mut_doc_set_root(doc, root);\n"
        "        yyjson_mut_obj_add_bool(doc, root, \"available\", true);\n"
        "        yyjson_mut_obj_add_strcpy(doc, root, \"model\", st.model_id);\n"
        "        yyjson_mut_obj_add_strcpy(doc, root, \"language\", lang_name);\n"
        "        yyjson_mut_obj_add_str(doc, root, \"language_source\",\n"
        "                               lang_explicit ? \"explicit\" : \"derived\");\n"
        "        yyjson_mut_obj_add_strcpy(doc, root, \"truncation\", trunc_text);\n"
        "        yyjson_mut_obj_add_int(doc, root, \"population\", st.n_vectors);\n"
        "        yyjson_mut_val *jcols = yyjson_mut_arr(doc);\n"
        "        for (int c = 0; c < ncols; c++) {\n"
        "            yyjson_mut_arr_add_str(doc, jcols, cols[c]);\n"
        "        }\n"
        "        yyjson_mut_obj_add_val(doc, root, \"cols\", jcols);\n"
        "        yyjson_mut_val *rows = yyjson_mut_arr(doc);\n"
        "        for (int i = 0; i < hit_count; i++) {\n"
        "            char lines[HYP_SZ_32];\n"
        "            sg_lines_str(lines, sizeof(lines), hits[i].start_line, hits[i].end_line);\n"
        "            yyjson_mut_val *row = yyjson_mut_arr(doc);\n"
        "            yyjson_mut_arr_add_strcpy(doc, row, hits[i].qualified_name);\n"
        "            yyjson_mut_arr_add_strcpy(doc, row, hits[i].label);\n"
        "            yyjson_mut_arr_add_strcpy(doc, row, hits[i].file_path);\n"
        "            yyjson_mut_arr_add_strcpy(doc, row, lines);\n"
        "            yyjson_mut_arr_add_real(doc, row, hits[i].score);\n"
        "            if (with_cut) {\n"
        "                yyjson_mut_arr_add_bool(doc, row, hits[i].truncated);\n"
        "            }\n"
        "            yyjson_mut_arr_add_val(rows, row);\n"
        "        }\n"
        "        yyjson_mut_obj_add_val(doc, root, \"rows\", rows);\n"
        "        char *json_text = yy_doc_to_str(doc);\n"
        "        yyjson_mut_doc_free(doc);\n"
        "        result = hyp_mcp_text_result(json_text ? json_text : \"{}\", false);\n"
        "        free(json_text);\n"
        "    } else {\n"
        "        hyp_sb_t sb;\n"
        "        hyp_sb_init(&sb);\n"
        "        hyp_tree_scalar_bool(&sb, \"available\", true);\n"
        "        hyp_tree_scalar_str(&sb, \"model\", st.model_id);\n"
        "        hyp_tree_scalar_str(&sb, \"language\", lang_name);\n"
        "        hyp_tree_scalar_str(&sb, \"language_source\", lang_explicit ? \"explicit\" : \"derived\");\n"
        "        hyp_tree_scalar_str(&sb, \"truncation\", trunc_text);\n"
        "        hyp_tree_scalar_int(&sb, \"population\", st.n_vectors);\n"
        "        hyp_tree_table_header(&sb, \"results\", hit_count, cols, ncols);\n"
        "        for (int i = 0; i < hit_count; i++) {\n"
        "            char lines[HYP_SZ_32];\n"
        "            sg_lines_str(lines, sizeof(lines), hits[i].start_line, hits[i].end_line);\n"
        "            hyp_tree_row_begin(&sb);\n"
        "            hyp_tree_cell_str(&sb, hits[i].qualified_name, true);\n"
        "            hyp_tree_cell_str(&sb, hits[i].label, false);\n"
        "            hyp_tree_cell_str(&sb, hits[i].file_path, false);\n"
        "            hyp_tree_cell_str(&sb, lines, false);\n"
        "            hyp_tree_cell_real(&sb, hits[i].score, false);\n"
        "            if (with_cut) {\n"
        "                hyp_tree_cell_bool(&sb, hits[i].truncated, false);\n"
        "            }\n"
        "            hyp_tree_row_end(&sb);\n"
        "        }\n"
        "        if (hit_count == 0) {\n"
        "            /* A REAL empty result: the index exists, the question encoded, and\n"
        "             * nothing scored. Said in words so it cannot be confused with the\n"
        "             * unavailable answer above, which never gets this far. */\n"
        "            hyp_tree_scalar_str(&sb, \"hint\",\n"
        "                                \"the index was searched and returned nothing. Unlike \"\n"
        "                                \"available=false, this IS a statement about the code.\");\n"
        "        }\n"
        "        char *text = hyp_sb_finish(&sb);\n"
        "        result = hyp_mcp_text_result(text ? text : \"out of memory\", text == NULL);\n"
        "        free(text);\n"
        "    }\n"
        "\n"
        "    hyp_ask_free_hits(hits, hit_count);\n"
        "    free(project);\n"
        "    free(question);\n"
        "    return result;\n"
        "}",
        214,
    },
    {
        "e79d00a0a40229cdda86d51a83037817",
        "void cbm_node_free_fields(cbm_node_t *n) {\n"
        "    free((void *)n->project);\n"
        "    free((void *)n->label);\n"
        "    free((void *)n->name);\n"
        "    free((void *)n->qualified_name);\n"
        "    free((void *)n->file_path);\n"
        "    free((void *)n->properties_json);\n"
        "}",
        8,
    },
    {
        "bb8e5bcd5f6db597b09c9d4fd8391e21",
        "CBMLSPDef *cbm_pxc_collect_all_defs(CBMFileResult **cache,\n"
        "                                       const cbm_file_info_t *files, int file_count,\n"
        "                                       const char *project_name,\n"
        "                                       char **def_modules, int *out_count) {\n"
        "    int total = 0;\n"
        "    for (int i = 0; i < file_count; i++) {\n"
        "        if (cache[i]) total += cache[i]->defs.count;\n"
        "    }\n"
        "    if (total == 0) {\n"
        "        *out_count = 0;\n"
        "        return NULL;\n"
        "    }\n"
        "    CBMLSPDef *defs = (CBMLSPDef *)calloc((size_t)total, sizeof(CBMLSPDef));\n"
        "    if (!defs) {\n"
        "        *out_count = 0;\n"
        "        return NULL;\n"
        "    }\n"
        "    int idx = 0;\n"
        "    for (int fi = 0; fi < file_count; fi++) {\n"
        "        if (!cache[fi]) continue;\n"
        "        if (!def_modules[fi]) {\n"
        "            def_modules[fi] = cbm_pipeline_fqn_module(project_name, files[fi].rel_path);\n"
        "        }\n"
        "        for (int di = 0; di < cache[fi]->defs.count; di++) {\n"
        "            if (pxc_build_lsp_def(&cache[fi]->arena,\n"
        "                                   &cache[fi]->defs.items[di],\n"
        "                                   def_modules[fi],\n"
        "                                   &defs[idx]) == 0) {\n"
        "                idx++;\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "    *out_count = idx;\n"
        "    return defs;\n"
        "}",
        35,
    },
    {
        "f39b0ff6e4c855f0bbaf2af192d8d4b3",
        "TSNode cbm_resolve_func_name(TSNode node, CBMLanguage lang) {\n"
        "    enum { MAX_TEMPLATE_DEPTH = 2 };\n"
        "    for (int tmpl_depth = 0; tmpl_depth < MAX_TEMPLATE_DEPTH; tmpl_depth++) {\n"
        "        const char *kind = ts_node_type(node);\n"
        "\n"
        "        if (lang == CBM_LANG_HASKELL && strcmp(kind, \"signature\") == 0) {\n"
        "            TSNode null_node = {0};\n"
        "            return null_node;\n"
        "        }\n"
        "\n"
        "        TSNode name = func_name_node(node);\n"
        "\n"
        "        if (lang == CBM_LANG_R && strcmp(kind, \"function_definition\") == 0) {\n"
        "            return resolve_r_func_name(node);\n"
        "        }\n"
        "\n"
        "        if (!ts_node_is_null(name)) {\n"
        "            return name;\n"
        "        }\n"
        "\n"
        "        /* Swift and newer tree-sitter-kotlin: function_declaration has no `name`\n"
        "         * field; the function name is a `simple_identifier` child. */\n"
        "        if ((lang == CBM_LANG_SWIFT || lang == CBM_LANG_KOTLIN) &&\n"
        "            strcmp(kind, \"function_declaration\") == 0) {\n"
        "            TSNode si = cbm_find_child_by_kind(node, \"simple_identifier\");\n"
        "            if (!ts_node_is_null(si)) {\n"
        "                return si;\n"
        "            }\n"
        "        }\n"
        "\n"
        "        // PowerShell function_statement has no `name` field; the name is a\n"
        "        // `function_name` child node (#35).\n"
        "        if (lang == CBM_LANG_POWERSHELL && strcmp(kind, \"function_statement\") == 0) {\n"
        "            TSNode fn = cbm_find_child_by_kind(node, \"function_name\");\n"
        "            if (!ts_node_is_null(fn)) {\n"
        "                return fn;\n"
        "            }\n"
        "        }\n"
        "\n"
        "        /* Cairo / D / Odin / Squirrel: the def node has no `name` field; the name\n"
        "         * is a plain `identifier` child (same shape as the Swift/Kotlin case). */\n"
        "        if ((lang == CBM_LANG_CAIRO || lang == CBM_LANG_DLANG || lang == CBM_LANG_ODIN ||\n"
        "             lang == CBM_LANG_SQUIRREL) &&\n"
        "            (strcmp(kind, \"function_definition\") == 0 || strcmp(kind, \"function_signature\") == 0 ||\n"
        "             strcmp(kind, \"function_declaration\") == 0 ||\n"
        "             strcmp(kind, \"procedure_declaration\") == 0)) {\n"
        "            TSNode id = cbm_find_child_by_kind(node, \"identifier\");\n"
        "            if (!ts_node_is_null(id)) {\n"
        "                return id;\n"
        "            }\n"
        "        }\n"
        "\n"
        "        /* Ada: subprogram_body/_declaration carry the `name` field on a nested\n"
        "         * procedure_specification/function_specification child, not on themselves. */\n"
        "        if (lang == CBM_LANG_ADA &&\n"
        "            (strcmp(kind, \"subprogram_body\") == 0 || strcmp(kind, \"subprogram_declaration\") == 0)) {\n"
        "            TSNode spec = cbm_find_child_by_kind(node, \"procedure_specification\");\n"
        "            if (ts_node_is_null(spec)) {\n"
        "                spec = cbm_find_child_by_kind(node, \"function_specification\");\n"
        "            }\n"
        "            if (!ts_node_is_null(spec)) {\n"
        "                TSNode nm = ts_node_child_by_field_name(spec, TS_FIELD(\"name\"));\n"
        "                if (!ts_node_is_null(nm)) {\n"
        "                    return nm;\n"
        "                }\n"
        "            }\n"
        "        }\n"
        "\n"
        "        /* Pascal: defProc carries the `name` field on its `header` (declProc) child. */\n"
        "        if (lang == CBM_LANG_PASCAL && strcmp(kind, \"defProc\") == 0) {\n"
        "            TSNode hdr = ts_node_child_by_field_name(node, TS_FIELD(\"header\"));\n"
        "            if (!ts_node_is_null(hdr)) {\n"
        "                TSNode nm = ts_node_child_by_field_name(hdr, TS_FIELD(\"name\"));\n"
        "                if (!ts_node_is_null(nm)) {\n"
        "                    return nm;\n"
        "                }\n"
        "            }\n"
        "        }\n"
        "\n"
        "        /* Just: a `recipe` carries its name on the nested `recipe_header`'s\n"
        "         * `name` field (an identifier), not on the recipe node itself. */\n"
        "        if (lang == CBM_LANG_JUST && strcmp(kind, \"recipe\") == 0) {\n"
        "            TSNode hdr = cbm_find_child_by_kind(node, \"recipe_header\");\n"
        "            if (!ts_node_is_null(hdr)) {\n"
        "                TSNode nm = ts_node_child_by_field_name(hdr, TS_FIELD(\"name\"));\n"
        "                if (!ts_node_is_null(nm)) {\n"
        "                    return nm;\n"
        "                }\n"
        "            }\n"
        "        }\n"
        "\n"
        "        /* ReScript: the `function` (arrow) node — already in func_types — has no\n"
        "         * name; the binding name is on the enclosing let_binding's `pattern` field.\n"
        "         * Resolving via the parent keeps plain value let-bindings out of func_types. */\n"
        "        if (lang == CBM_LANG_RESCRIPT && strcmp(kind, \"function\") == 0) {\n"
        "            TSNode parent = ts_node_parent(node);\n"
        "            if (!ts_node_is_null(parent) && strcmp(ts_node_type(parent), \"let_binding\") == 0) {\n"
        "                TSNode pat = ts_node_child_by_field_name(parent, TS_FIELD(\"pattern\"));\n"
        "                if (!ts_node_is_null(pat)) {\n"
        "                    return pat;\n"
        "                }\n"
        "            }\n"
        "        }\n"
        "\n"
        "        /* Fortran: subroutine/function wrap an inner *_statement that carries the\n"
        "         * `name` field; the outer node walk_defs matched has no name itself. */\n"
        "        if (lang == CBM_LANG_FORTRAN &&\n"
        "            (strcmp(kind, \"subroutine\") == 0 || strcmp(kind, \"function\") == 0)) {\n"
        "            TSNode stmt = cbm_find_child_by_kind(node, \"subroutine_statement\");\n"
        "            if (ts_node_is_null(stmt)) {\n"
        "                stmt = cbm_find_child_by_kind(node, \"function_statement\");\n"
        "            }\n"
        "            if (!ts_node_is_null(stmt)) {\n"
        "                TSNode nm = ts_node_child_by_field_name(stmt, TS_FIELD(\"name\"));\n"
        "                if (!ts_node_is_null(nm)) {\n"
        "                    return nm;\n"
        "                }\n"
        "            }\n"
        "        }\n"
        "\n"
        "        /* F#: function_or_value_defn's name is on a function_declaration_left /\n"
        "         * value_declaration_left child (a bare identifier, no `name` field). */\n"
        "        if (lang == CBM_LANG_FSHARP && strcmp(kind, \"function_or_value_defn\") == 0) {\n"
        "            TSNode lhs = cbm_find_child_by_kind(node, \"function_declaration_left\");\n"
        "            if (ts_node_is_null(lhs)) {\n"
        "                lhs = cbm_find_child_by_kind(node, \"value_declaration_left\");\n"
        "            }\n"
        "            if (!ts_node_is_null(lhs)) {\n"
        "                TSNode nm = cbm_find_child_by_kind(lhs, \"identifier\");\n"
        "                if (ts_node_is_null(nm)) {\n"
        "                    nm = cbm_find_child_by_kind(lhs, \"long_identifier\");\n"
        "                }\n"
        "                if (!ts_node_is_null(nm)) {\n"
        "                    return nm;\n"
        "                }\n"
        "            }\n"
        "        }\n"
        "\n"
        "        /* Groovy: top-level function_definition carries the name on the `function`\n"
        "         * field (not `name`); fall back to the first `identifier` child. */\n"
        "        if (lang == CBM_LANG_GROOVY && strcmp(kind, \"function_definition\") == 0) {\n"
        "            TSNode fn = ts_node_child_by_field_name(node, TS_FIELD(\"function\"));\n"
        "            if (ts_node_is_null(fn)) {\n"
        "                fn = cbm_find_child_by_kind(node, \"identifier\");\n"
        "            }\n"
        "            if (!ts_node_is_null(fn)) {\n"
        "                return fn;\n"
        "            }\n"
        "        }\n"
        "\n"
        "        /* Agda (FIELD_COUNT 0): the only `function` carrying the name is the type\n"
        "         * signature line, whose lhs holds a `function_name` alias child. The body\n"
        "         * line's lhs has no function_name child -> resolves null and is skipped. */\n"
        "        if (lang == CBM_LANG_AGDA && strcmp(kind, \"function\") == 0) {\n"
        "            TSNode lhs = cbm_find_child_by_kind(node, \"lhs\");\n"
        "            if (!ts_node_is_null(lhs)) {\n"
        "                TSNode fn = cbm_find_child_by_kind(lhs, \"function_name\");\n"
        "                if (!ts_node_is_null(fn)) {\n"
        "                    return fn;\n"
        "                }\n"
        "            }\n"
        "        }\n"
        "\n"
        "        /* Pony: def nodes have no `name` field; the name is the first plain\n"
        "         * `identifier` child after the keyword/annotation/capability. */\n"
        "        if (lang == CBM_LANG_PONY &&\n"
        "            (strcmp(kind, \"method\") == 0 || strcmp(kind, \"constructor\") == 0 ||\n"
        "             strcmp(kind, \"ffi_method\") == 0)) {\n"
        "            TSNode id = cbm_find_child_by_kind(node, \"identifier\");\n"
        "            if (!ts_node_is_null(id)) {\n"
        "                return id;\n"
        "            }\n"
        "        }\n"
        "\n"
        "        /* COBOL: program_definition has no `name` field; the program name is\n"
        "         * identification_division > program_name (a leaf holding the clean name). */\n"
        "        if (lang == CBM_LANG_COBOL && strcmp(kind, \"program_definition\") == 0) {\n"
        "            TSNode iddiv = cbm_find_child_by_kind(node, \"identification_division\");\n"
        "            if (!ts_node_is_null(iddiv)) {\n"
        "                TSNode pname = cbm_find_child_by_kind(iddiv, \"program_name\");\n"
        "                if (!ts_node_is_null(pname)) {\n"
        "                    return pname;\n"
        "                }\n"
        "            }\n"
        "        }\n"
        "\n"
        "        /* Pine Script: function_declaration_statement carries the name on the\n"
        "         * `function` field (or `method` field for the method form), not `name`. */\n"
        "        if (lang == CBM_LANG_PINE && strcmp(kind, \"function_declaration_statement\") == 0) {\n"
        "            TSNode nm = ts_node_child_by_field_name(node, TS_FIELD(\"function\"));\n"
        "            if (ts_node_is_null(nm)) {\n"
        "                nm = ts_node_child_by_field_name(node, TS_FIELD(\"method\"));\n"
        "            }\n"
        "            if (!ts_node_is_null(nm)) {\n"
        "                return nm;\n"
        "            }\n"
        "        }\n"
        "\n"
        "        /* Smali (no `name` field): method_definition > method_signature >\n"
        "         * method_identifier holds the method name. */\n"
        "        if (lang == CBM_LANG_SMALI && strcmp(kind, \"method_definition\") == 0) {\n"
        "            TSNode sig = cbm_find_child_by_kind(node, \"method_signature\");\n"
        "            if (!ts_node_is_null(sig)) {\n"
        "                TSNode mid = cbm_find_child_by_kind(sig, \"method_identifier\");\n"
        "                if (!ts_node_is_null(mid)) {\n"
        "                    return mid;\n"
        "                }\n"
        "            }\n"
        "        }\n"
        "\n"
        "        /* Verilog/SystemVerilog (FIELD_COUNT 0): function/task names live on a\n"
        "         * nested *_identifier wrapper; the function name is the first\n"
        "         * simple_identifier descendant (params/returns come after the name). */\n"
        "        if ((lang == CBM_LANG_VERILOG || lang == CBM_LANG_SYSTEMVERILOG) &&\n"
        "            (strcmp(kind, \"function_declaration\") == 0 || strcmp(kind, \"task_declaration\") == 0)) {\n"
        "            TSNode si =\n"
        "                find_first_descendant_by_kind(node, \"simple_identifier\", CBM_DESCENDANT_MAX_DEPTH);\n"
        "            if (!ts_node_is_null(si)) {\n"
        "                return si;\n"
        "            }\n"
        "        }\n"
        "\n"
        "        /* VHDL: subprogram_declaration/_definition carry the name on a nested\n"
        "         * function_specification/procedure_specification child, via the\n"
        "         * `function`/`procedure` field. */\n"
        "        if (lang == CBM_LANG_VHDL && (strcmp(kind, \"subprogram_declaration\") == 0 ||\n"
        "                                      strcmp(kind, \"subprogram_definition\") == 0)) {\n"
        "            TSNode spec = cbm_find_child_by_kind(node, \"function_specification\");\n"
        "            if (ts_node_is_null(spec)) {\n"
        "                spec = cbm_find_child_by_kind(node, \"procedure_specification\");\n"
        "            }\n"
        "            if (!ts_node_is_null(spec)) {\n"
        "                TSNode nm = ts_node_child_by_field_name(spec, TS_FIELD(\"function\"));\n"
        "                if (ts_node_is_null(nm)) {\n"
        "                    nm = ts_node_child_by_field_name(spec, TS_FIELD(\"procedure\"));\n"
        "                }\n"
        "                if (!ts_node_is_null(nm)) {\n"
        "                    return nm;\n"
        "                }\n"
        "            }\n"
        "        }\n"
        "\n"
        "        /* Thrift / Cap'n Proto / Smithy (no `name` field): the def name is a\n"
        "         * plain visible `identifier` child of the statement/definition node. */\n"
        "        if (lang == CBM_LANG_THRIFT || lang == CBM_LANG_SMITHY) {\n"
        "            TSNode id = cbm_find_child_by_kind(node, \"identifier\");\n"
        "            if (!ts_node_is_null(id)) {\n"
        "                return id;\n"
        "            }\n"
        "        }\n"
        "\n"
        "        /* Cap'n Proto (FIELD_COUNT 0): name is an aliased *_identifier child. */\n"
        "        if (lang == CBM_LANG_CAPNP) {\n"
        "            const char *name_kind = NULL;\n"
        "            if (strcmp(kind, \"struct\") == 0 || strcmp(kind, \"interface\") == 0) {\n"
        "                name_kind = \"type_identifier\";\n"
        "            } else if (strcmp(kind, \"enum\") == 0) {\n"
        "                name_kind = \"enum_identifier\";\n"
        "            } else if (strcmp(kind, \"method\") == 0) {\n"
        "                name_kind = \"method_identifier\";\n"
        "            }\n"
        "            if (name_kind) {\n"
        "                TSNode id = cbm_find_child_by_kind(node, name_kind);\n"
        "                if (!ts_node_is_null(id)) {\n"
        "                    return id;\n"
        "                }\n"
        "            }\n"
        "        }\n"
        "\n"
        "        /* CMake (FIELD_COUNT 0): function(foo)/macro(foo) — the name is nested as\n"
        "         * *_command > argument_list > argument > unquoted_argument. */\n"
        "        if (lang == CBM_LANG_CMAKE &&\n"
        "            (strcmp(kind, \"function_def\") == 0 || strcmp(kind, \"macro_def\") == 0)) {\n"
        "            const char *cmd_kind =\n"
        "                strcmp(kind, \"function_def\") == 0 ? \"function_command\" : \"macro_command\";\n"
        "            TSNode cmd = cbm_find_child_by_kind(node, cmd_kind);\n"
        "            if (!ts_node_is_null(cmd)) {\n"
        "                TSNode alist = cbm_find_child_by_kind(cmd, \"argument_list\");\n"
        "                if (!ts_node_is_null(alist)) {\n"
        "                    TSNode arg = cbm_find_child_by_kind(alist, \"argument\");\n"
        "                    if (!ts_node_is_null(arg)) {\n"
        "                        TSNode uq = cbm_find_child_by_kind(arg, \"unquoted_argument\");\n"
        "                        return ts_node_is_null(uq) ? arg : uq;\n"
        "                    }\n"
        "                }\n"
        "            }\n"
        "        }\n"
        "\n"
        "        /* Puppet: function_declaration name is a plain identifier/class_identifier\n"
        "         * child (no `name` field). */\n"
        "        if (lang == CBM_LANG_PUPPET &&\n"
        "            (strcmp(kind, \"function_declaration\") == 0 || strcmp(kind, \"lambda\") == 0)) {\n"
        "            TSNode id = cbm_find_child_by_kind(node, \"identifier\");\n"
        "            if (ts_node_is_null(id)) {\n"
        "                id = cbm_find_child_by_kind(node, \"class_identifier\");\n"
        "            }\n"
        "            if (!ts_node_is_null(id)) {\n"
        "                return id;\n"
        "            }\n"
        "        }\n"
        "\n"
        "        /* Assembly (GAS): a bare `label` (`foo:`) reduces with no `name` field;\n"
        "         * its name child is aliased to `ident`. */\n"
        "        if (lang == CBM_LANG_ASSEMBLY && strcmp(kind, \"label\") == 0) {\n"
        "            TSNode id = cbm_find_child_by_kind(node, \"ident\");\n"
        "            if (!ts_node_is_null(id)) {\n"
        "                return id;\n"
        "            }\n"
        "        }\n"
        "\n"
        "        {\n"
        "            TSNode r = resolve_toplevel_arrow_name(node, kind);\n"
        "            if (!ts_node_is_null(r)) {\n"
        "                return r;\n"
        "            }\n"
        "        }\n"
        "        {\n"
        "            TSNode r = resolve_func_name_scripting(node, lang, kind);\n"
        "            if (!ts_node_is_null(r)) {\n"
        "                return r;\n"
        "            }\n"
        "        }\n"
        "        {\n"
        "            TSNode r = resolve_func_name_fp(node, lang, kind, name);\n"
        "            if (!ts_node_is_null(r)) {\n"
        "                return r;\n"
        "            }\n"
        "        }\n"
        "\n"
        "        {\n"
        "            TSNode prev = node;\n"
        "            TSNode r = resolve_func_name_c_family(&node, lang, kind);\n"
        "            if (!ts_node_is_null(r)) {\n"
        "                return r;\n"
        "            }\n"
        "            if (!ts_node_eq(prev, node)) {\n"
        "                continue; /* template unwrapped — retry */\n"
        "            }\n"
        "        }\n"
        "\n"
        "        break;\n"
        "    } /* end template depth loop */\n"
        "    TSNode null_node = {0};\n"
        "    return null_node;\n"
        "}",
        345,
    },
    {
        "a620c219a40e359f6e8ad72270d690ba",
        "char *cbm_cpp_out_of_line_parent_class(CBMArena *a, TSNode node, const char *source) {\n"
        "    // Descend the declarator chain to its qualified_identifier, if any.\n"
        "    TSNode qid = {0};\n"
        "    TSNode decl = ts_node_child_by_field_name(node, TS_FIELD(\"declarator\"));\n"
        "    for (int depth = 0; depth < DECLARATOR_DEPTH_LIMIT && !ts_node_is_null(decl); depth++) {\n"
        "        const char *dk = ts_node_type(decl);\n"
        "        if (strcmp(dk, \"qualified_identifier\") == 0 || strcmp(dk, \"scoped_identifier\") == 0) {\n"
        "            qid = decl;\n"
        "            break;\n"
        "        }\n"
        "        TSNode inner = ts_node_child_by_field_name(decl, TS_FIELD(\"declarator\"));\n"
        "        if (ts_node_is_null(inner) && ts_node_named_child_count(decl) > 0) {\n"
        "            inner = ts_node_named_child(decl, 0);\n"
        "        }\n"
        "        if (ts_node_is_null(inner)) {\n"
        "            break;\n"
        "        }\n"
        "        decl = inner;\n"
        "    }\n"
        "    if (ts_node_is_null(qid)) {\n"
        "        return NULL;\n"
        "    }\n"
        "    // The qualified_identifier's `scope` is the parent. For a nested scope\n"
        "    // (`ns::Foo`) descend through its `name` field to the innermost segment so\n"
        "    // the direct parent (\"Foo\") is returned, not the outer namespace.\n"
        "    TSNode scope = ts_node_child_by_field_name(qid, TS_FIELD(\"scope\"));\n"
        "    if (ts_node_is_null(scope)) {\n"
        "        return NULL;\n"
        "    }\n"
        "    for (int depth = 0; depth < DECLARATOR_DEPTH_LIMIT; depth++) {\n"
        "        const char *sk = ts_node_type(scope);\n"
        "        if (strcmp(sk, \"qualified_identifier\") != 0 && strcmp(sk, \"scoped_identifier\") != 0) {\n"
        "            break;\n"
        "        }\n"
        "        TSNode name = ts_node_child_by_field_name(scope, TS_FIELD(\"name\"));\n"
        "        if (ts_node_is_null(name)) {\n"
        "            break;\n"
        "        }\n"
        "        scope = name;\n"
        "    }\n"
        "    char *text = cbm_node_text(a, scope, source);\n"
        "    return (text && text[0]) ? text : NULL;\n"
        "}",
        43,
    },
    {
        "cb2a5f92a64f1467032591f071ddf683",
        "char *cbm_hook_augment_read_stdin(void) {\n"
        "    char *buf = malloc(HA_STDIN_CAP + 1);\n"
        "    if (!buf) {\n"
        "        return NULL;\n"
        "    }\n"
        "    size_t total = 0;\n"
        "    size_t n;\n"
        "    while (total < HA_STDIN_CAP && (n = fread(buf + total, 1, HA_STDIN_CAP - total, stdin)) > 0) {\n"
        "        total += n;\n"
        "    }\n"
        "    buf[total] = '\\0';\n"
        "    return buf;\n"
        "}",
        13,
    },
    {
        "03307df26d207617044ccbcfb7b66884",
        "void cbm_tree_scalar_str(cbm_sb_t *sb, const char *key, const char *val) {\n"
        "    cbm_sb_append(sb, key);\n"
        "    cbm_sb_append_n(sb, \": \", 2);\n"
        "    append_value(sb, val);\n"
        "    cbm_sb_append_n(sb, \"\\n\", 1);\n"
        "}",
        6,
    },
    {
        "58cb9c72d2994100cfc8b30108a5e513",
        "const char *hyp_activation_transaction_refusal_note(void) {\n"
        "    return g_activation_refusal_note;\n"
        "}",
        3,
    },
    {
        "bb39073094b17ec0edfbe0914424e698",
        "static void lg_free(cbm_lg_t *g) {\n"
        "    free(g->off);\n"
        "    free(g->nbr);\n"
        "    free(g->w);\n"
        "    free(g->k);\n"
        "    g->off = NULL;\n"
        "    g->nbr = NULL;\n"
        "    g->w = NULL;\n"
        "    g->k = NULL;\n"
        "}",
        10,
    },
    {
        "2d2a8578efb05167390f5b1907a12d27",
        "static void py_invalidate_possible_bindings(PyLSPContext *ctx, TSNode node, int depth) {\n"
        "    if (!ctx || ts_node_is_null(node))\n"
        "        return;\n"
        "    if (depth > 64) {\n"
        "        py_disable_callable_value_proof(ctx);\n"
        "        return;\n"
        "    }\n"
        "    const char *kind = ts_node_type(node);\n"
        "    if (strcmp(kind, \"lambda\") == 0) {\n"
        "        return;\n"
        "    }\n"
        "    /* A comprehension's iteration variables are private to it, so it is not\n"
        "     * scanned as an ordinary binder -- but a walrus inside one still binds out\n"
        "     * here, and skipping the whole node missed that. */\n"
        "    if (strcmp(kind, \"list_comprehension\") == 0 || strcmp(kind, \"set_comprehension\") == 0 ||\n"
        "        strcmp(kind, \"dictionary_comprehension\") == 0 ||\n"
        "        strcmp(kind, \"generator_expression\") == 0) {\n"
        "        py_invalidate_walrus_bindings(ctx, node, depth + 1);\n"
        "        return;\n"
        "    }\n"
        "    /* PEP 695 `type X = ...` binds X in the enclosing scope. Only the left side\n"
        "     * is a target; the right is a type expression. */\n"
        "    if (strcmp(kind, \"type_alias_statement\") == 0) {\n"
        "        TSNode left = ts_node_child_by_field_name(node, \"left\", 4);\n"
        "        if (ts_node_is_null(left) && ts_node_named_child_count(node) > 0)\n"
        "            left = ts_node_named_child(node, 0);\n"
        "        if (ts_node_is_null(left))\n"
        "            py_disable_callable_value_proof(ctx);\n"
        "        else\n"
        "            py_invalidate_binding_target(ctx, left, depth + 1);\n"
        "        return;\n"
        "    }\n"
        "    if (strcmp(kind, \"function_definition\") == 0 || strcmp(kind, \"class_definition\") == 0) {\n"
        "        TSNode name = ts_node_child_by_field_name(node, \"name\", 4);\n"
        "        if (ts_node_is_null(name))\n"
        "            py_disable_callable_value_proof(ctx);\n"
        "        else\n"
        "            py_invalidate_binding_target(ctx, name, depth + 1);\n"
        "        return;\n"
        "    }\n"
        "    if (strcmp(kind, \"decorated_definition\") == 0) {\n"
        "        TSNode definition = ts_node_child_by_field_name(node, \"definition\", 10);\n"
        "        if (ts_node_is_null(definition))\n"
        "            py_disable_callable_value_proof(ctx);\n"
        "        else\n"
        "            py_invalidate_possible_bindings(ctx, definition, depth + 1);\n"
        "        return;\n"
        "    }\n"
        "    if (strcmp(kind, \"import_statement\") == 0 ||\n"
        "        strcmp(kind, \"import_from_statement\") == 0) {\n"
        "        py_invalidate_import_bindings(ctx, node);\n"
        "        return;\n"
        "    }\n"
        "    if (strcmp(kind, \"assignment\") == 0) {\n"
        "        TSNode right = ts_node_child_by_field_name(node, \"right\", 5);\n"
        "        if (ts_node_is_null(right))\n"
        "            return;\n"
        "        py_invalidate_binding_target(ctx, ts_node_child_by_field_name(node, \"left\", 4),\n"
        "                                     depth + 1);\n"
        "        py_invalidate_possible_bindings(ctx, right, depth + 1);\n"
        "        return;\n"
        "    }\n"
        "    if (strcmp(kind, \"augmented_assignment\") == 0) {\n"
        "        TSNode left = ts_node_child_by_field_name(node, \"left\", 4);\n"
        "        TSNode right = ts_node_child_by_field_name(node, \"right\", 5);\n"
        "        py_invalidate_binding_target(ctx, left, depth + 1);\n"
        "        py_invalidate_possible_bindings(ctx, right, depth + 1);\n"
        "        return;\n"
        "    }\n"
        "    if (strcmp(kind, \"named_expression\") == 0 ||\n"
        "        strcmp(kind, \"assignment_expression\") == 0) {\n"
        "        TSNode name = ts_node_child_by_field_name(node, \"name\", 4);\n"
        "        if (ts_node_is_null(name))\n"
        "            name = ts_node_child_by_field_name(node, \"left\", 4);\n"
        "        TSNode value = ts_node_child_by_field_name(node, \"value\", 5);\n"
        "        if (ts_node_is_null(value))\n"
        "            value = ts_node_child_by_field_name(node, \"right\", 5);\n"
        "        py_invalidate_binding_target(ctx, name, depth + 1);\n"
        "        py_invalidate_possible_bindings(ctx, value, depth + 1);\n"
        "        return;\n"
        "    }\n"
        "    if (strcmp(kind, \"delete_statement\") == 0) {\n"
        "        uint32_t count = ts_node_named_child_count(node);\n"
        "        for (uint32_t i = 0; i < count; i++)\n"
        "            py_invalidate_binding_target(ctx, ts_node_named_child(node, i), depth + 1);\n"
        "        return;\n"
        "    }\n"
        "    if (strcmp(kind, \"for_statement\") == 0) {\n"
        "        TSNode left = ts_node_child_by_field_name(node, \"left\", 4);\n"
        "        py_invalidate_binding_target(ctx, left, depth + 1);\n"
        "        uint32_t count = ts_node_named_child_count(node);\n"
        "        for (uint32_t i = 0; i < count; i++) {\n"
        "            TSNode child = ts_node_named_child(node, i);\n"
        "            if (!ts_node_eq(child, left))\n"
        "                py_invalidate_possible_bindings(ctx, child, depth + 1);\n"
        "        }\n"
        "        return;\n"
        "    }\n"
        "    if (strcmp(kind, \"as_pattern\") == 0) {\n"
        "        TSNode alias = ts_node_child_by_field_name(node, \"alias\", 5);\n"
        "        uint32_t count = ts_node_named_child_count(node);\n"
        "        if (ts_node_is_null(alias) && count > 1)\n"
        "            alias = ts_node_named_child(node, count - 1);\n"
        "        py_invalidate_binding_target(ctx, alias, depth + 1);\n"
        "        for (uint32_t i = 0; i < count; i++) {\n"
        "            TSNode child = ts_node_named_child(node, i);\n"
        "            if (!ts_node_eq(child, alias))\n"
        "                py_invalidate_possible_bindings(ctx, child, depth + 1);\n"
        "        }\n"
        "        return;\n"
        "    }\n"
        "    if (strcmp(kind, \"match_statement\") == 0) {\n"
        "        TSNode subject = ts_node_child_by_field_name(node, \"subject\", 7);\n"
        "        TSNode body = ts_node_child_by_field_name(node, \"body\", 4);\n"
        "        py_invalidate_possible_bindings(ctx, subject, depth + 1);\n"
        "        uint32_t cases = ts_node_named_child_count(body);\n"
        "        for (uint32_t i = 0; i < cases; i++) {\n"
        "            TSNode clause = ts_node_named_child(body, i);\n"
        "            if (strcmp(ts_node_type(clause), \"case_clause\") != 0)\n"
        "                continue;\n"
        "            TSNode pattern = {0};\n"
        "            uint32_t count = ts_node_named_child_count(clause);\n"
        "            for (uint32_t j = 0; j < count; j++) {\n"
        "                TSNode child = ts_node_named_child(clause, j);\n"
        "                if (ts_node_is_null(pattern) && strcmp(ts_node_type(child), \"block\") != 0) {\n"
        "                    pattern = child;\n"
        "                    py_invalidate_match_pattern(ctx, pattern, depth + 1);\n"
        "                } else if (!ts_node_eq(child, pattern)) {\n"
        "                    py_invalidate_possible_bindings(ctx, child, depth + 1);\n"
        "                }\n"
        "            }\n"
        "        }\n"
        "        return;\n"
        "    }\n"
        "    uint32_t count = ts_node_named_child_count(node);\n"
        "    for (uint32_t i = 0; i < count; i++)\n"
        "        py_invalidate_possible_bindings(ctx, ts_node_named_child(node, i), depth + 1);\n"
        "}",
        138,
    },
    {
        "4051aa168f34034fa4d6664259dc547c",
        "static int macro_consume_fragment(const char *s, int len, int from, const char *frag) {\n"
        "    from = macro_skip_ws(s, len, from);\n"
        "    if (from >= len)\n"
        "        return from;\n"
        "    if (!frag)\n"
        "        frag = \"tt\";\n"
        "\n"
        "    if (strcmp(frag, \"ident\") == 0) {\n"
        "        return macro_consume_ident(s, len, from);\n"
        "    }\n"
        "    if (strcmp(frag, \"literal\") == 0) {\n"
        "        /* Numeric, string, char, bool literal. */\n"
        "        char c = s[from];\n"
        "        if (c == '\"')\n"
        "            return macro_consume_balanced(s, len, from);\n"
        "        if (c == '\\'') {\n"
        "            int q = from + 1;\n"
        "            if (q < len && s[q] == '\\\\') {\n"
        "                q++;\n"
        "                if (q < len)\n"
        "                    q++;\n"
        "            } else if (q < len)\n"
        "                q++;\n"
        "            if (q < len && s[q] == '\\'')\n"
        "                return q + 1;\n"
        "            return from;\n"
        "        }\n"
        "        if (c == '-' || (c >= '0' && c <= '9')) {\n"
        "            int p = from;\n"
        "            if (c == '-')\n"
        "                p++;\n"
        "            while (p < len && ((s[p] >= '0' && s[p] <= '9') || s[p] == '_' || s[p] == '.' ||\n"
        "                               s[p] == 'x' || s[p] == 'X' || s[p] == 'b' || s[p] == 'o' ||\n"
        "                               (s[p] >= 'a' && s[p] <= 'f') || (s[p] >= 'A' && s[p] <= 'F')))\n"
        "                p++;\n"
        "            /* Allow type suffix. */\n"
        "            int after = macro_consume_ident(s, len, p);\n"
        "            return after;\n"
        "        }\n"
        "        return macro_consume_ident(s, len, from);\n"
        "    }\n"
        "    if (strcmp(frag, \"tt\") == 0) {\n"
        "        char c = s[from];\n"
        "        if (c == '(' || c == '[' || c == '{') {\n"
        "            return macro_consume_balanced(s, len, from);\n"
        "        }\n"
        "        /* Single token: identifier, literal, or single-char punct. */\n"
        "        if (c == '\"' || c == '\\'') {\n"
        "            return macro_consume_fragment(s, len, from, \"literal\");\n"
        "        }\n"
        "        if (c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||\n"
        "            (c >= '0' && c <= '9') || c == '-') {\n"
        "            int p = macro_consume_ident(s, len, from);\n"
        "            if (p > from)\n"
        "                return p;\n"
        "            return macro_consume_fragment(s, len, from, \"literal\");\n"
        "        }\n"
        "        return from + 1;\n"
        "    }\n"
        "    /* expr / ty / path / pat / stmt / block — balance brackets, stop\n"
        "     * at top-level `,` or end of input. */\n"
        "    if (strcmp(frag, \"block\") == 0) {\n"
        "        if (s[from] == '{')\n"
        "            return macro_consume_balanced(s, len, from);\n"
        "        return from;\n"
        "    }\n"
        "    /* For expr/ty/path/pat/stmt: greedy balanced span. */\n"
        "    int depth = 0;\n"
        "    int p = from;\n"
        "    while (p < len) {\n"
        "        char c = s[p];\n"
        "        if (c == '\"' || c == '\\'') {\n"
        "            int q = macro_consume_fragment(s, len, p, \"literal\");\n"
        "            if (q == p) {\n"
        "                p++;\n"
        "                continue;\n"
        "            }\n"
        "            p = q;\n"
        "            continue;\n"
        "        }\n"
        "        if (c == '(' || c == '[' || c == '{') {\n"
        "            int q = macro_consume_balanced(s, len, p);\n"
        "            if (q == p)\n"
        "                break;\n"
        "            p = q;\n"
        "            continue;\n"
        "        }\n"
        "        if (c == ')' || c == ']' || c == '}')\n"
        "            break;\n"
        "        if (depth == 0 && c == ',')\n"
        "            break;\n"
        "        if (strcmp(frag, \"stmt\") == 0 && c == ';')\n"
        "            break;\n"
        "        p++;\n"
        "    }\n"
        "    return p;\n"
        "}",
        97,
    },
    {
        "4c13d9c93d46f90fec8c59dda6f61d72",
        "static bool has_filesystem_extension(const char *path) {\n"
        "    if (!path) {\n"
        "        return false;\n"
        "    }\n"
        "    const char *end = strpbrk(path, \"?#\");\n"
        "    if (!end) {\n"
        "        end = path + strlen(path);\n"
        "    }\n"
        "    const char *last_slash = path;\n"
        "    for (const char *p = path; p < end; p++) {\n"
        "        if (*p == '/') {\n"
        "            last_slash = p;\n"
        "        }\n"
        "    }\n"
        "    const char *dot = NULL;\n"
        "    for (const char *p = last_slash + 1; p < end; p++) {\n"
        "        if (*p == '.') {\n"
        "            dot = p;\n"
        "        }\n"
        "    }\n"
        "    if (!dot || dot == end - 1) {\n"
        "        return false;\n"
        "    }\n"
        "    char ext[32];\n"
        "    size_t ext_len = (size_t)(end - dot);\n"
        "    if (ext_len >= sizeof(ext)) {\n"
        "        return false;\n"
        "    }\n"
        "    memcpy(ext, dot, ext_len);\n"
        "    ext[ext_len] = '\\0';\n"
        "\n"
        "    static const char *const hard_file_exts[] = {\n"
        "        \".cfg\",  \".conf\",   \".credentials\", \".crt\",  \".db\",         \".env\",\n"
        "        \".ini\",  \".key\",    \".pem\",         \".pid\",  \".properties\", \".service\",\n"
        "        \".sock\", \".socket\", \".sqlite\",      \".toml\", NULL};\n"
        "    for (int i = 0; hard_file_exts[i]; i++) {\n"
        "        if (path_ext_matches(ext, hard_file_exts[i])) {\n"
        "            return true;\n"
        "        }\n"
        "    }\n"
        "    if ((path_ext_matches(ext, \".json\") || path_ext_matches(ext, \".yaml\") ||\n"
        "         path_ext_matches(ext, \".yml\") || path_ext_matches(ext, \".xml\")) &&\n"
        "        !has_http_route_marker(path)) {\n"
        "        return true;\n"
        "    }\n"
        "    return false;\n"
        "}",
        47,
    },
    {
        "52fe01d2c1e275790c3398edaef26567",
        "static ks_slot_t *ks_build(const TSLanguage *lang, const char *const *types, ks_slot_t *s) {\n"
        "    s->lang = lang;\n"
        "    s->types = types;\n"
        "    s->bits = NULL;\n"
        "    s->nsyms = 0;\n"
        "    s->exact = false;\n"
        "    uint32_t nsyms = ts_language_symbol_count(lang);\n"
        "    if (nsyms == 0) {\n"
        "        return s; /* fall back to strcmp */\n"
        "    }\n"
        "    uint64_t *bits = calloc(((size_t)nsyms + 63) / 64, sizeof(uint64_t));\n"
        "    if (!bits) {\n"
        "        return s;\n"
        "    }\n"
        "    bool all_resolved = true;\n"
        "    for (const char *const *t = types; *t; t++) {\n"
        "        uint32_t len = (uint32_t)strlen(*t);\n"
        "        /* A name may be a named node type or an anonymous token (\"for\", \"&&\"):\n"
        "         * set whichever symbol(s) exist so ts_node_symbol matches either. */\n"
        "        TSSymbol sn = ts_language_symbol_for_name(lang, *t, len, true);\n"
        "        TSSymbol sa = ts_language_symbol_for_name(lang, *t, len, false);\n"
        "        bool any = false;\n"
        "        if (sn != 0 && sn < nsyms) {\n"
        "            bits[sn >> 6] |= (uint64_t)1 << (sn & 63);\n"
        "            any = true;\n"
        "        }\n"
        "        if (sa != 0 && sa < nsyms) {\n"
        "            bits[sa >> 6] |= (uint64_t)1 << (sa & 63);\n"
        "            any = true;\n"
        "        }\n"
        "        if (!any) {\n"
        "            all_resolved = false; /* unknown name → can't represent exactly */\n"
        "        }\n"
        "    }\n"
        "    if (!all_resolved) {\n"
        "        free(bits);\n"
        "        return s; /* exact stays false */\n"
        "    }\n"
        "    s->bits = bits;\n"
        "    s->nsyms = nsyms;\n"
        "    s->exact = true;\n"
        "    return s;\n"
        "}",
        43,
    },
    {
        "72be4d8fc72f2bbc81239edcede2e8b5",
        "static void emit_property(UdlBuf *b, const char *ps, const char *pe) {\n"
        "    char pn[MAX_NAME];\n"
        "    extract_attr(ps, pe, \"name\", pn, sizeof(pn));\n"
        "    if (!pn[0])\n"
        "        return;\n"
        "    char pt[MAX_NAME] = \"\";\n"
        "    elem_content(ps, pe, \"Type\", pt, sizeof(pt));\n"
        "    PropParam params[MAX_PARAMS];\n"
        "    int np = 0;\n"
        "    const char *pp = ps;\n"
        "    while (pp < pe && np < MAX_PARAMS) {\n"
        "        const char *po = find_s(pp, pe, \"<Parameter \");\n"
        "        if (!po)\n"
        "            break;\n"
        "        const char *pg = find_s(po, pe, \">\");\n"
        "        if (!pg)\n"
        "            break;\n"
        "        extract_attr(po, pg, \"name\", params[np].param_name, MAX_NAME);\n"
        "        extract_attr(po, pg, \"value\", params[np].param_value, MAX_NAME);\n"
        "        if (!params[np].param_value[0]) {\n"
        "            char db[MAX_NAME];\n"
        "            const char *a = elem_content(po, pe, \"Parameter\", db, MAX_NAME);\n"
        "            if (a && db[0])\n"
        "                strncpy(params[np].param_value, db, MAX_NAME - 1);\n"
        "        }\n"
        "        if (params[np].param_name[0])\n"
        "            np++;\n"
        "        pp = pg + 1;\n"
        "    }\n"
        "    ub_app(b, \"Property \");\n"
        "    ub_app(b, pn);\n"
        "    if (pt[0]) {\n"
        "        ub_app(b, \" As \");\n"
        "        ub_app(b, pt);\n"
        "    }\n"
        "    if (np > 0) {\n"
        "        ub_app(b, \"(\");\n"
        "        for (int i = 0; i < np; i++) {\n"
        "            if (i > 0)\n"
        "                ub_app(b, \", \");\n"
        "            ub_app(b, params[i].param_name);\n"
        "            if (params[i].param_value[0]) {\n"
        "                ub_app(b, \" = \");\n"
        "                ub_app(b, params[i].param_value);\n"
        "            }\n"
        "        }\n"
        "        ub_app(b, \")\");\n"
        "    }\n"
        "    ub_app(b, \";\\n\\n\");\n"
        "}",
        50,
    },
    {
        "00053f0b8f7f116d0b22b1e2e7e01224",
        "static inline void ts_nstack_push(TSNodeStack *s, CBMArena *arena, TSNode node) {\n"
        "    if (s->count >= s->cap) {\n"
        "        int new_cap = s->cap ? s->cap * 2 : 512;\n"
        "        TSNode *new_items = (TSNode *)cbm_arena_alloc(arena, (size_t)new_cap * sizeof(TSNode));\n"
        "        if (!new_items) return; /* OOM: best-effort, stop growing */\n"
        "        if (s->items && s->count > 0) {\n"
        "            memcpy(new_items, s->items, (size_t)s->count * sizeof(TSNode));\n"
        "        }\n"
        "        /* Old s->items is abandoned in the arena — freed on arena_destroy. */\n"
        "        s->items = new_items;\n"
        "        s->cap = new_cap;\n"
        "    }\n"
        "    s->items[s->count++] = node;\n"
        "}",
        14,
    },
    {
        "6962ad4ea8bb6f0abcbc1222aab97c2d",
        "static inline void ts_nstack_init(TSNodeStack *s, CBMArena *arena, int initial_cap) {\n"
        "    s->items = (TSNode *)cbm_arena_alloc(arena, (size_t)initial_cap * sizeof(TSNode));\n"
        "    s->count = 0;\n"
        "    s->cap = s->items ? initial_cap : 0;\n"
        "}",
        5,
    },
    {
        "f6ebd4d1ca52b6a056d1bcded4b1bccc",
        "static int assert_lsp_strategy(const char *filename, const char *src,\n"
        "                               const char *strategy) {\n"
        "    RProj lp;\n"
        "    cbm_store_t *store = rh_index(&lp, filename, src);\n"
        "    if (!store) {\n"
        "        printf(\"  %sFAIL%s %s:%d: index failed for strategy %s\\n\", tf_red(),\n"
        "               tf_reset(), __FILE__, __LINE__, strategy);\n"
        "        rh_cleanup(&lp, store);\n"
        "        return 1;\n"
        "    }\n"
        "\n"
        "    int module_sourced = -1;\n"
        "    int callable_sourced = -1;\n"
        "    inv_count_calls_by_source(store, lp.project, &module_sourced,\n"
        "                              &callable_sourced);\n"
        "\n"
        "    int has_strategy = inv_edge_has_strategy(store, lp.project, strategy);\n"
        "\n"
        "    int rc = 0;\n"
        "\n"
        "    /* (a) callable-sourcing floor: zero Module/File-sourced CALLS edges. */\n"
        "    if (module_sourced != 0) {\n"
        "        printf(\"  %sFAIL%s %s:%d: strategy %s: %d Module-sourced CALLS \"\n"
        "               \"(expected 0)\\n\",\n"
        "               tf_red(), tf_reset(), __FILE__, __LINE__, strategy,\n"
        "               module_sourced);\n"
        "        rc = 1;\n"
        "    }\n"
        "    /* There must be a callable-sourced CALLS edge, else the fixture produced no\n"
        "     * call signal and the strategy assertion below would be vacuous. */\n"
        "    if (callable_sourced <= 0) {\n"
        "        printf(\"  %sFAIL%s %s:%d: strategy %s: no callable-sourced CALLS edge \"\n"
        "               \"(callable=%d)\\n\",\n"
        "               tf_red(), tf_reset(), __FILE__, __LINE__, strategy,\n"
        "               callable_sourced);\n"
        "        rc = 1;\n"
        "    }\n"
        "\n"
        "    /* (b) the precise per-pass invariant: the resolution strategy is present. */\n"
        "    if (!has_strategy) {\n"
        "        printf(\"  %sFAIL%s %s:%d: strategy %s ABSENT from any CALLS edge \"\n"
        "               \"properties_json\\n\",\n"
        "               tf_red(), tf_reset(), __FILE__, __LINE__, strategy);\n"
        "        rc = 1;\n"
        "    }\n"
        "\n"
        "    rh_cleanup(&lp, store);\n"
        "    return rc;\n"
        "}",
        49,
    },
    {
        "227d4d8841d3cc7c711e766b59c1162c",
        "static int assert_lsp_strategy_files(const RFile *files, int nfiles,\n"
        "                                     const char *strategy) {\n"
        "    RProj lp;\n"
        "    cbm_store_t *store = rh_index_files(&lp, files, nfiles);\n"
        "    if (!store) {\n"
        "        printf(\"  %sFAIL%s %s:%d: index failed for strategy %s\\n\", tf_red(),\n"
        "               tf_reset(), __FILE__, __LINE__, strategy);\n"
        "        rh_cleanup(&lp, store);\n"
        "        return 1;\n"
        "    }\n"
        "\n"
        "    int module_sourced = -1;\n"
        "    int callable_sourced = -1;\n"
        "    inv_count_calls_by_source(store, lp.project, &module_sourced,\n"
        "                              &callable_sourced);\n"
        "\n"
        "    int has_strategy = inv_edge_has_strategy(store, lp.project, strategy);\n"
        "\n"
        "    int rc = 0;\n"
        "\n"
        "    /* (a) callable-sourcing floor: zero Module/File-sourced CALLS edges. */\n"
        "    if (module_sourced != 0) {\n"
        "        printf(\"  %sFAIL%s %s:%d: strategy %s: %d Module-sourced CALLS \"\n"
        "               \"(expected 0)\\n\",\n"
        "               tf_red(), tf_reset(), __FILE__, __LINE__, strategy,\n"
        "               module_sourced);\n"
        "        rc = 1;\n"
        "    }\n"
        "    /* There must be a callable-sourced CALLS edge, else the fixture produced no\n"
        "     * call signal and the strategy assertion below would be vacuous. */\n"
        "    if (callable_sourced <= 0) {\n"
        "        printf(\"  %sFAIL%s %s:%d: strategy %s: no callable-sourced CALLS edge \"\n"
        "               \"(callable=%d)\\n\",\n"
        "               tf_red(), tf_reset(), __FILE__, __LINE__, strategy,\n"
        "               callable_sourced);\n"
        "        rc = 1;\n"
        "    }\n"
        "\n"
        "    /* (b) the precise per-pass invariant: the resolution strategy is present. */\n"
        "    if (!has_strategy) {\n"
        "        printf(\"  %sFAIL%s %s:%d: strategy %s ABSENT from any CALLS edge \"\n"
        "               \"properties_json\\n\",\n"
        "               tf_red(), tf_reset(), __FILE__, __LINE__, strategy);\n"
        "        rc = 1;\n"
        "    }\n"
        "\n"
        "    rh_cleanup(&lp, store);\n"
        "    return rc;\n"
        "}",
        49,
    },
    {
        "69e97d18253ac1290236d8fb1d53174b",
        "static bool suite_requested(const char *name) {\n"
        "    if (g_suite_argc <= 1) {\n"
        "        return true;\n"
        "    }\n"
        "    bool requested = false;\n"
        "    for (int i = 1; i < g_suite_argc; i++) {\n"
        "        if (strcmp(g_suite_argv[i], name) == 0) {\n"
        "            g_suite_arg_matched[i] = true;\n"
        "            requested = true;\n"
        "        }\n"
        "    }\n"
        "    return requested;\n"
        "}",
        13,
    },
    {
        "1b0818dfb3f4be498ca3cfedd114e8ab",
        "static int single_file_battery(const char *lang_tag, const char *src,\n"
        "                               CBMLanguage lang, const char *file,\n"
        "                               const char *expect_label,\n"
        "                               const char *expect_label2, const char *callee) {\n"
        "    const char *RED = tf_red();\n"
        "    const char *RST = tf_reset();\n"
        "    int fails = 0;\n"
        "\n"
        "    /* 1. extract-clean -- must hold before anything else is meaningful. */\n"
        "    if (inv_extract_clean(src, lang, file) != 1) {\n"
        "        printf(\"  %sFAIL%s  [%s] extract-clean: NULL result or has_error set\\n\",\n"
        "               RED, RST, lang_tag);\n"
        "        return 1; /* nothing else can be trusted */\n"
        "    }\n"
        "\n"
        "    CBMFileResult *r = inv_rx(src, lang, file);\n"
        "    if (!r) {\n"
        "        printf(\"  %sFAIL%s  [%s] inv_rx returned NULL after clean extract\\n\",\n"
        "               RED, RST, lang_tag);\n"
        "        return 1;\n"
        "    }\n"
        "\n"
        "    /* 2. labels-valid */\n"
        "    int bad_labels = inv_count_bad_labels(r);\n"
        "    if (bad_labels != 0) {\n"
        "        printf(\"  %sFAIL%s  [%s] labels-valid: %d def(s) with invalid label\\n\",\n"
        "               RED, RST, lang_tag, bad_labels);\n"
        "        fails++;\n"
        "    }\n"
        "\n"
        "    /* 3. fqn-wellformed */\n"
        "    int bad_fqns = inv_count_bad_fqns(r);\n"
        "    if (bad_fqns != 0) {\n"
        "        printf(\"  %sFAIL%s  [%s] fqn-wellformed: %d def(s) with malformed QN\\n\",\n"
        "               RED, RST, lang_tag, bad_fqns);\n"
        "        fails++;\n"
        "    }\n"
        "\n"
        "    /* 4. ranges-valid */\n"
        "    int bad_ranges = inv_count_bad_ranges(r);\n"
        "    if (bad_ranges != 0) {\n"
        "        printf(\"  %sFAIL%s  [%s] ranges-valid: %d def(s) with invalid range\\n\",\n"
        "               RED, RST, lang_tag, bad_ranges);\n"
        "        fails++;\n"
        "    }\n"
        "\n"
        "    /* 5. defs-present -- the function/class the fixture wrote must be extracted. */\n"
        "    if (expect_label && inv_count_label(r, expect_label) < 1) {\n"
        "        printf(\"  %sFAIL%s  [%s] defs-present: no def labelled \\\"%s\\\"\\n\",\n"
        "               RED, RST, lang_tag, expect_label);\n"
        "        fails++;\n"
        "    }\n"
        "    if (expect_label2 && inv_count_label(r, expect_label2) < 1) {\n"
        "        printf(\"  %sFAIL%s  [%s] defs-present: no def labelled \\\"%s\\\"\\n\",\n"
        "               RED, RST, lang_tag, expect_label2);\n"
        "        fails++;\n"
        "    }\n"
        "\n"
        "    /* 6. calls-extracted -- the in-body call must be captured. */\n"
        "    if (inv_has_call(r, callee) != 1) {\n"
        "        printf(\"  %sFAIL%s  [%s] calls-extracted: no call to \\\"%s\\\" found\\n\",\n"
        "               RED, RST, lang_tag, callee);\n"
        "        fails++;\n"
        "    }\n"
        "\n"
        "    cbm_free_result(r);\n"
        "    return fails ? 1 : 0;\n"
        "}",
        68,
    },
    {
        "ea039e8f7545c327d4bf0b49521b6af3",
        "static int pipeline_battery(const char *lang_tag, const char *filename,\n"
        "                            const char *src) {\n"
        "    const char *RED = tf_red();\n"
        "    const char *RST = tf_reset();\n"
        "\n"
        "    RFile files[1];\n"
        "    files[0].name = filename;\n"
        "    files[0].content = src;\n"
        "\n"
        "    RProj lp;\n"
        "    cbm_store_t *store = rh_index_files(&lp, files, 1);\n"
        "    if (!store) {\n"
        "        printf(\"  %sFAIL%s  [%s] pipeline: rh_index_files returned NULL\\n\",\n"
        "               RED, RST, lang_tag);\n"
        "        return 1;\n"
        "    }\n"
        "\n"
        "    int fails = 0;\n"
        "\n"
        "    /* 7. callable-sourcing -- mod must be 0; we also require >=1 callable-sourced\n"
        "     * edge so a fixture that produced zero CALLS edges cannot vacuously pass. */\n"
        "    int module_sourced = 0;\n"
        "    int callable_sourced = 0;\n"
        "    inv_count_calls_by_source(store, lp.project, &module_sourced,\n"
        "                              &callable_sourced);\n"
        "    if (module_sourced != 0) {\n"
        "        printf(\"  %sFAIL%s  [%s] callable-sourcing: %d in-body CALLS sourced at \"\n"
        "               \"Module (callable=%d) -- known enclosing-func gap\\n\",\n"
        "               RED, RST, lang_tag, module_sourced, callable_sourced);\n"
        "        fails++;\n"
        "    } else if (callable_sourced < 1) {\n"
        "        printf(\"  %sFAIL%s  [%s] callable-sourcing: 0 CALLS edges (fixture \"\n"
        "               \"produced no in-body call edge to attribute)\\n\",\n"
        "               RED, RST, lang_tag);\n"
        "        fails++;\n"
        "    }\n"
        "\n"
        "    /* 8. no-dangling -- every CALLS edge endpoint must resolve. */\n"
        "    int dangling = inv_count_dangling_edges(store, lp.project, \"CALLS\");\n"
        "    if (dangling != 0) {\n"
        "        printf(\"  %sFAIL%s  [%s] no-dangling: %d dangling CALLS endpoint(s)\\n\",\n"
        "               RED, RST, lang_tag, dangling);\n"
        "        fails++;\n"
        "    }\n"
        "\n"
        "    rh_cleanup(&lp, store);\n"
        "    return fails ? 1 : 0;\n"
        "}",
        48,
    },
};
const int g1_text_count = (int)(sizeof(g1_texts) / sizeof(g1_texts[0]));

const g1_live_t g1_live[] = {
    {"src/mcp/mcp.c", "handle_query_graph", "hyponoia.src.mcp.mcp.handle_query_graph", "hyp1:hyponoia/hyponoia#hyponoia.src.mcp.mcp.handle_query_graph", 0, "G1-MOVE-001"},
    {"src/mcp/mcp.c", "release_request_store", "hyponoia.src.mcp.mcp.release_request_store", "hyp1:hyponoia/hyponoia#hyponoia.src.mcp.mcp.release_request_store", 1, "G1-MOVE-002"},
    {"src/mcp/mcp.c", "adr_list_sections_from_content", "hyponoia.src.mcp.mcp.adr_list_sections_from_content", "hyp1:hyponoia/hyponoia#hyponoia.src.mcp.mcp.adr_list_sections_from_content", 2, "G1-MOVE-003"},
    {"src/cypher/cypher.c", "check_inline_props", "hyponoia.src.cypher.cypher.check_inline_props", "hyp1:hyponoia/hyponoia#hyponoia.src.cypher.cypher.check_inline_props", 3, "G1-MOVE-004"},
    {"tests/test_mcp.c", "mcp_cross_repo_create_project_store", "hyponoia.tests.test_mcp.mcp_cross_repo_create_project_store", "hyp1:hyponoia/hyponoia#hyponoia.tests.test_mcp.mcp_cross_repo_create_project_store", 4, "G1-MOVE-005"},
    {"tests/test_ask.c", "json_str_at", "hyponoia.tests.test_ask.json_str_at", "hyp1:hyponoia/hyponoia#hyponoia.tests.test_ask.json_str_at", 5, "G1-MOVE-006"},
    {"internal/cbm/lsp/c_lsp.c", "cbm_run_c_lsp_cross", "hyponoia.internal.cbm.lsp.c_lsp.cbm_run_c_lsp_cross", "hyp1:hyponoia/hyponoia#hyponoia.internal.cbm.lsp.c_lsp.cbm_run_c_lsp_cross", 6, "G1-MOVE-007"},
    {"src/daemon/runtime.c", "cbm_daemon_runtime_client_application_request", "hyponoia.src.daemon.runtime.cbm_daemon_runtime_client_application_request", "hyp1:hyponoia/hyponoia#hyponoia.src.daemon.runtime.cbm_daemon_runtime_client_application_request", 7, "G1-MOVE-008"},
    {"src/foundation/diagnostics.c", "write_diagnostics", "hyponoia.src.foundation.diagnostics.write_diagnostics", "hyp1:hyponoia/hyponoia#hyponoia.src.foundation.diagnostics.write_diagnostics", 8, "G1-EDIT-001"},
    {"internal/hyp/helpers.c", "hyp_is_test_file", "hyponoia.internal.hyp.helpers.hyp_is_test_file", "hyp1:hyponoia/hyponoia#hyponoia.internal.hyp.helpers.hyp_is_test_file", 9, "G1-EDIT-002"},
    {"src/mcp/mcp.c", "handle_search_graph", "hyponoia.src.mcp.mcp.handle_search_graph", "hyp1:hyponoia/hyponoia#hyponoia.src.mcp.mcp.handle_search_graph", 10, "G1-EDIT-003"},
    {"src/mcp/mcp.c", "cbm_mcp_text_result", "hyponoia.src.mcp.mcp.cbm_mcp_text_result", "hyp1:hyponoia/hyponoia#hyponoia.src.mcp.mcp.cbm_mcp_text_result", 11, "G1-EDIT-004"},
    {"src/daemon/application.c", "application_mcp_request", "hyponoia.src.daemon.application.application_mcp_request", "hyp1:hyponoia/hyponoia#hyponoia.src.daemon.application.application_mcp_request", 12, "G1-EDIT-005"},
    {"src/foundation/mem.c", "cbm_mem_init_with_cap", "hyponoia.src.foundation.mem.cbm_mem_init_with_cap", "hyp1:hyponoia/hyponoia#hyponoia.src.foundation.mem.cbm_mem_init_with_cap", 13, "G1-EDIT-006"},
    {"src/ask/ask_llama.c", "ask_llama_open", "hyponoia.src.ask.ask_llama.ask_llama_open", "hyp1:hyponoia/hyponoia#hyponoia.src.ask.ask_llama.ask_llama_open", 14, "G1-EDIT-007"},
    {"src/mcp/mcp.c", "handle_ask", "hyponoia.src.mcp.mcp.handle_ask", "hyp1:hyponoia/hyponoia#hyponoia.src.mcp.mcp.handle_ask", 15, "G1-EDIT-008"},
    {"src/store/store.c", "cbm_node_free_fields", "hyponoia.src.store.store.cbm_node_free_fields", "hyp1:hyponoia/hyponoia#hyponoia.src.store.store.cbm_node_free_fields", 16, "G1-RENAME-001"},
    {"src/pipeline/pass_lsp_cross.c", "cbm_pxc_collect_all_defs", "hyponoia.src.pipeline.pass_lsp_cross.cbm_pxc_collect_all_defs", "hyp1:hyponoia/hyponoia#hyponoia.src.pipeline.pass_lsp_cross.cbm_pxc_collect_all_defs", 17, "G1-RENAME-002"},
    {"internal/cbm/extract_defs.c", "cbm_resolve_func_name", "hyponoia.internal.cbm.extract_defs.cbm_resolve_func_name", "hyp1:hyponoia/hyponoia#hyponoia.internal.cbm.extract_defs.cbm_resolve_func_name", 18, "G1-RENAME-003"},
    {"internal/cbm/extract_defs.c", "cbm_cpp_out_of_line_parent_class", "hyponoia.internal.cbm.extract_defs.cbm_cpp_out_of_line_parent_class", "hyp1:hyponoia/hyponoia#hyponoia.internal.cbm.extract_defs.cbm_cpp_out_of_line_parent_class", 19, "G1-RENAME-004"},
    {"src/cli/hook_augment.c", "cbm_hook_augment_read_stdin", "hyponoia.src.cli.hook_augment.cbm_hook_augment_read_stdin", "hyp1:hyponoia/hyponoia#hyponoia.src.cli.hook_augment.cbm_hook_augment_read_stdin", 20, "G1-RENAME-005"},
    {"src/mcp/compact_out.c", "cbm_tree_scalar_str", "hyponoia.src.mcp.compact_out.cbm_tree_scalar_str", "hyp1:hyponoia/hyponoia#hyponoia.src.mcp.compact_out.cbm_tree_scalar_str", 21, "G1-RENAME-006"},
    {"src/cli/activation_transaction.c", "hyp_activation_transaction_refusal_note", "hyponoia.src.cli.activation_transaction.hyp_activation_transaction_refusal_note", "hyp1:hyponoia/hyponoia#hyponoia.src.cli.activation_transaction.hyp_activation_transaction_refusal_note", 22, "G1-RENAME-007"},
    {"src/store/store.c", "lg_free", "hyponoia.src.store.store.lg_free", "hyp1:hyponoia/hyponoia#hyponoia.src.store.store.lg_free", 23, "G1-RENAME-008"},
    {"internal/hyp/lsp/py_lsp.c", "py_invalidate_possible_bindings", "hyponoia.internal.hyp.lsp.py_lsp.py_invalidate_possible_bindings", "hyp1:hyponoia/hyponoia#hyponoia.internal.hyp.lsp.py_lsp.py_invalidate_possible_bindings", 24, "G1-CROSSDIR-001"},
    {"internal/hyp/lsp/rust_lsp.c", "macro_consume_fragment", "hyponoia.internal.hyp.lsp.rust_lsp.macro_consume_fragment", "hyp1:hyponoia/hyponoia#hyponoia.internal.hyp.lsp.rust_lsp.macro_consume_fragment", 25, "G1-CROSSDIR-002"},
    {"internal/hyp/service_patterns.c", "has_filesystem_extension", "hyponoia.internal.hyp.service_patterns.has_filesystem_extension", "hyp1:hyponoia/hyponoia#hyponoia.internal.hyp.service_patterns.has_filesystem_extension", 26, "G1-CROSSDIR-003"},
    {"internal/hyp/helpers.c", "ks_build", "hyponoia.internal.hyp.helpers.ks_build", "hyp1:hyponoia/hyponoia#hyponoia.internal.hyp.helpers.ks_build", 27, "G1-CROSSDIR-004"},
    {"internal/hyp/iris_export_xml.c", "emit_property", "hyponoia.internal.hyp.iris_export_xml.emit_property", "hyp1:hyponoia/hyponoia#hyponoia.internal.hyp.iris_export_xml.emit_property", 28, "G1-CROSSDIR-005"},
    {"internal/cbm/extract_node_stack.h", "ts_nstack_push", "hyponoia.internal.cbm.extract_node_stack.ts_nstack_push", "hyp1:hyponoia/hyponoia#hyponoia.internal.cbm.extract_node_stack.ts_nstack_push", 29, "G1-FILERENAME-001"},
    {"internal/cbm/extract_node_stack.h", "ts_nstack_init", "hyponoia.internal.cbm.extract_node_stack.ts_nstack_init", "hyp1:hyponoia/hyponoia#hyponoia.internal.cbm.extract_node_stack.ts_nstack_init", 30, "G1-FILERENAME-002"},
    {"tests/repro/repro_lsp_c_cpp.c", "assert_lsp_strategy", "hyponoia.tests.repro.repro_lsp_c_cpp.assert_lsp_strategy", "hyp1:hyponoia/hyponoia#hyponoia.tests.repro.repro_lsp_c_cpp.assert_lsp_strategy", 31, "G1-COPY-001"},
    {"tests/repro/repro_lsp_java_cs.c", "assert_lsp_strategy", "hyponoia.tests.repro.repro_lsp_java_cs.assert_lsp_strategy", "hyp1:hyponoia/hyponoia#hyponoia.tests.repro.repro_lsp_java_cs.assert_lsp_strategy", 31, "G1-COPY-001"},
    {"tests/repro/repro_lsp_kt_php_rust.c", "assert_lsp_strategy", "hyponoia.tests.repro.repro_lsp_kt_php_rust.assert_lsp_strategy", "hyp1:hyponoia/hyponoia#hyponoia.tests.repro.repro_lsp_kt_php_rust.assert_lsp_strategy", 31, "G1-COPY-001"},
    {"tests/repro/repro_lsp_go_py.c", "assert_lsp_strategy_files", "hyponoia.tests.repro.repro_lsp_go_py.assert_lsp_strategy_files", "hyp1:hyponoia/hyponoia#hyponoia.tests.repro.repro_lsp_go_py.assert_lsp_strategy_files", 32, "G1-COPY-002"},
    {"tests/repro/repro_lsp_ts.c", "assert_lsp_strategy_files", "hyponoia.tests.repro.repro_lsp_ts.assert_lsp_strategy_files", "hyp1:hyponoia/hyponoia#hyponoia.tests.repro.repro_lsp_ts.assert_lsp_strategy_files", 32, "G1-COPY-002"},
    {"tests/test_main.c", "suite_requested", "hyponoia.tests.test_main.suite_requested", "hyp1:hyponoia/hyponoia#hyponoia.tests.test_main.suite_requested", 33, "G1-COPY-003"},
    {"tests/test_foundation_main.c", "suite_requested", "hyponoia.tests.test_foundation_main.suite_requested", "hyp1:hyponoia/hyponoia#hyponoia.tests.test_foundation_main.suite_requested", 33, "G1-COPY-003"},
    {"tests/repro/repro_grammar_core.c", "single_file_battery", "hyponoia.tests.repro.repro_grammar_core.single_file_battery", "hyp1:hyponoia/hyponoia#hyponoia.tests.repro.repro_grammar_core.single_file_battery", 34, "G1-COPY-004"},
    {"tests/repro/repro_grammar_scripting.c", "single_file_battery", "hyponoia.tests.repro.repro_grammar_scripting.single_file_battery", "hyp1:hyponoia/hyponoia#hyponoia.tests.repro.repro_grammar_scripting.single_file_battery", 34, "G1-COPY-004"},
    {"tests/repro/repro_grammar_core.c", "pipeline_battery", "hyponoia.tests.repro.repro_grammar_core.pipeline_battery", "hyp1:hyponoia/hyponoia#hyponoia.tests.repro.repro_grammar_core.pipeline_battery", 35, "G1-COPY-005"},
    {"tests/repro/repro_grammar_scripting.c", "pipeline_battery", "hyponoia.tests.repro.repro_grammar_scripting.pipeline_battery", "hyp1:hyponoia/hyponoia#hyponoia.tests.repro.repro_grammar_scripting.pipeline_battery", 35, "G1-COPY-005"},
    {"tests/repro/repro_grammar_functional.c", "pipeline_battery", "hyponoia.tests.repro.repro_grammar_functional.pipeline_battery", "hyp1:hyponoia/hyponoia#hyponoia.tests.repro.repro_grammar_functional.pipeline_battery", 35, "G1-COPY-005"},
};
const int g1_live_count = (int)(sizeof(g1_live) / sizeof(g1_live[0]));

static const char *const mn_g1_move_001[] = {
    NULL,
};
static const char *const mn_g1_move_002[] = {
    NULL,
};
static const char *const mn_g1_move_003[] = {
    NULL,
};
static const char *const mn_g1_move_004[] = {
    NULL,
};
static const char *const mn_g1_move_005[] = {
    NULL,
};
static const char *const mn_g1_move_006[] = {
    NULL,
};
static const char *const mn_g1_move_007[] = {
    NULL,
};
static const char *const mn_g1_move_008[] = {
    NULL,
};
static const char *const mn_g1_edit_001[] = {
    NULL,
};
static const char *const mn_g1_edit_002[] = {
    NULL,
};
static const char *const mn_g1_edit_003[] = {
    NULL,
};
static const char *const mn_g1_edit_004[] = {
    NULL,
};
static const char *const mn_g1_edit_005[] = {
    NULL,
};
static const char *const mn_g1_edit_006[] = {
    NULL,
};
static const char *const mn_g1_edit_007[] = {
    NULL,
};
static const char *const mn_g1_edit_008[] = {
    NULL,
};
static const char *const mn_g1_rename_001[] = {
    "hyp1:hyponoia/hyponoia#hyponoia.src.store.store.cbm_node_free_fields",
    NULL,
};
static const char *const mn_g1_rename_002[] = {
    "hyp1:hyponoia/hyponoia#hyponoia.src.pipeline.pass_lsp_cross.cbm_pxc_collect_all_defs",
    NULL,
};
static const char *const mn_g1_rename_003[] = {
    "hyp1:hyponoia/hyponoia#hyponoia.internal.cbm.extract_defs.cbm_resolve_func_name",
    NULL,
};
static const char *const mn_g1_rename_004[] = {
    "hyp1:hyponoia/hyponoia#hyponoia.internal.cbm.extract_defs.cbm_cpp_out_of_line_parent_class",
    NULL,
};
static const char *const mn_g1_rename_005[] = {
    "hyp1:hyponoia/hyponoia#hyponoia.src.cli.hook_augment.cbm_hook_augment_read_stdin",
    NULL,
};
static const char *const mn_g1_rename_006[] = {
    "hyp1:hyponoia/hyponoia#hyponoia.src.mcp.compact_out.cbm_tree_scalar_str",
    NULL,
};
static const char *const mn_g1_rename_007[] = {
    "hyp1:hyponoia/hyponoia#hyponoia.src.cli.activation_transaction.hyp_activation_transaction_refusal_note",
    NULL,
};
static const char *const mn_g1_rename_008[] = {
    "hyp1:hyponoia/hyponoia#hyponoia.src.store.store.lg_free",
    NULL,
};
static const char *const mn_g1_crossdir_001[] = {
    NULL,
};
static const char *const mn_g1_crossdir_002[] = {
    NULL,
};
static const char *const mn_g1_crossdir_003[] = {
    NULL,
};
static const char *const mn_g1_crossdir_004[] = {
    NULL,
};
static const char *const mn_g1_crossdir_005[] = {
    NULL,
};
static const char *const mn_g1_filerename_001[] = {
    NULL,
};
static const char *const mn_g1_filerename_002[] = {
    NULL,
};
static const char *const mn_g1_copy_001[] = {
    "hyp1:hyponoia/hyponoia#hyponoia.tests.repro.repro_lsp_java_cs.assert_lsp_strategy",
    "hyp1:hyponoia/hyponoia#hyponoia.tests.repro.repro_lsp_kt_php_rust.assert_lsp_strategy",
    NULL,
};
static const char *const mn_g1_copy_002[] = {
    "hyp1:hyponoia/hyponoia#hyponoia.tests.repro.repro_lsp_ts.assert_lsp_strategy_files",
    NULL,
};
static const char *const mn_g1_copy_003[] = {
    "hyp1:hyponoia/hyponoia#hyponoia.tests.test_foundation_main.suite_requested",
    NULL,
};
static const char *const mn_g1_copy_004[] = {
    "hyp1:hyponoia/hyponoia#hyponoia.tests.repro.repro_grammar_scripting.single_file_battery",
    NULL,
};
static const char *const mn_g1_copy_005[] = {
    "hyp1:hyponoia/hyponoia#hyponoia.tests.repro.repro_grammar_scripting.pipeline_battery",
    "hyp1:hyponoia/hyponoia#hyponoia.tests.repro.repro_grammar_functional.pipeline_battery",
    NULL,
};

const g1_fixture_t g1_fixtures[] = {
    {
        "G1-MOVE-001",
        "moved_within_file",
        "src/mcp/mcp.c",
        "handle_query_graph",
        "hyponoia.src.mcp.mcp.handle_query_graph",
        "3f4c1f498985bc970ec6a2441a8562db",
        "3f4c1f498985bc970ec6a2441a8562db@hyp1:hyponoia/hyponoia#hyponoia.src.mcp.mcp.handle_query_graph",
        "HYP_ANCHOR_RESOLVED",
        0,
        mn_g1_move_001,
        "moved +275 lines because the rerank stage was wired in above it",
    },
    {
        "G1-MOVE-002",
        "moved_within_file",
        "src/mcp/mcp.c",
        "release_request_store",
        "hyponoia.src.mcp.mcp.release_request_store",
        "883bb47425e9690c21483e885e204032",
        "883bb47425e9690c21483e885e204032@hyp1:hyponoia/hyponoia#hyponoia.src.mcp.mcp.release_request_store",
        "HYP_ANCHOR_RESOLVED",
        0,
        mn_g1_move_002,
        "moved +488 lines when the `ask` MCP tool was added above it",
    },
    {
        "G1-MOVE-003",
        "moved_within_file",
        "src/mcp/mcp.c",
        "adr_list_sections_from_content",
        "hyponoia.src.mcp.mcp.adr_list_sections_from_content",
        "b2f5a5f1a8c98236bc4c2372e49c556e",
        "b2f5a5f1a8c98236bc4c2372e49c556e@hyp1:hyponoia/hyponoia#hyponoia.src.mcp.mcp.adr_list_sections_from_content",
        "HYP_ANCHOR_RESOLVED",
        0,
        mn_g1_move_003,
        "moved +699 lines when impact analysis and cycle detection landed above it",
    },
    {
        "G1-MOVE-004",
        "moved_within_file",
        "src/cypher/cypher.c",
        "check_inline_props",
        "hyponoia.src.cypher.cypher.check_inline_props",
        "1d7b1909c814be5bb3b312f3d5b0804d",
        "1d7b1909c814be5bb3b312f3d5b0804d@hyp1:hyponoia/hyponoia#hyponoia.src.cypher.cypher.check_inline_props",
        "HYP_ANCHOR_RESOLVED",
        0,
        mn_g1_move_004,
        "moved +679 lines in the mimalloc/prescan commit",
    },
    {
        "G1-MOVE-005",
        "moved_within_file",
        "tests/test_mcp.c",
        "mcp_cross_repo_create_project_store",
        "hyponoia.tests.test_mcp.mcp_cross_repo_create_project_store",
        "7c007c18ad1a81d595285d75960bd240",
        "7c007c18ad1a81d595285d75960bd240@hyp1:hyponoia/hyponoia#hyponoia.tests.test_mcp.mcp_cross_repo_create_project_store",
        "HYP_ANCHOR_RESOLVED",
        0,
        mn_g1_move_005,
        "moved +432 lines when the project-defaulting tests were added above it",
    },
    {
        "G1-MOVE-006",
        "moved_within_file",
        "tests/test_ask.c",
        "json_str_at",
        "hyponoia.tests.test_ask.json_str_at",
        "4b3987980defd07c0e9bbb3268c97850",
        "4b3987980defd07c0e9bbb3268c97850@hyp1:hyponoia/hyponoia#hyponoia.tests.test_ask.json_str_at",
        "HYP_ANCHOR_RESOLVED",
        0,
        mn_g1_move_006,
        "moved +596 lines when the source-carrying `ask` tests were added above it",
    },
    {
        "G1-MOVE-007",
        "moved_within_file",
        "internal/cbm/lsp/c_lsp.c",
        "cbm_run_c_lsp_cross",
        "hyponoia.internal.cbm.lsp.c_lsp.cbm_run_c_lsp_cross",
        "481a372583e7498caa760e7e9f978b6e",
        "481a372583e7498caa760e7e9f978b6e@hyp1:hyponoia/hyponoia#hyponoia.internal.cbm.lsp.c_lsp.cbm_run_c_lsp_cross",
        "HYP_ANCHOR_RESOLVED",
        0,
        mn_g1_move_007,
        "moved +996 lines when the C/C++ resolver was expanded above it",
    },
    {
        "G1-MOVE-008",
        "moved_within_file",
        "src/daemon/runtime.c",
        "cbm_daemon_runtime_client_application_request",
        "hyponoia.src.daemon.runtime.cbm_daemon_runtime_client_application_request",
        "95ac705b4bd195a467ffe63a34ee98dd",
        "95ac705b4bd195a467ffe63a34ee98dd@hyp1:hyponoia/hyponoia#hyponoia.src.daemon.runtime.cbm_daemon_runtime_client_application_request",
        "HYP_ANCHOR_RESOLVED",
        0,
        mn_g1_move_008,
        "moved +313 lines in the daemon lifecycle commit",
    },
    {
        "G1-EDIT-001",
        "body_changed",
        "src/foundation/diagnostics.c",
        "write_diagnostics",
        "hyponoia.src.foundation.diagnostics.write_diagnostics",
        "2265f33c1c2c23973299f539e09ba66e",
        "2265f33c1c2c23973299f539e09ba66e@hyp1:hyponoia/hyponoia#hyponoia.src.foundation.diagnostics.write_diagnostics",
        "HYP_ANCHOR_RESOLVED_EDITED",
        0,
        mn_g1_edit_001,
        "body edited in place (start line unchanged): the wrapped-RSS meter fix",
    },
    {
        "G1-EDIT-002",
        "body_changed",
        "internal/hyp/helpers.c",
        "hyp_is_test_file",
        "hyponoia.internal.hyp.helpers.hyp_is_test_file",
        "48cfe2b316e3d51179c71d7acb5b8943",
        "48cfe2b316e3d51179c71d7acb5b8943@hyp1:hyponoia/hyponoia#hyponoia.internal.hyp.helpers.hyp_is_test_file",
        "HYP_ANCHOR_RESOLVED_EDITED",
        0,
        mn_g1_edit_002,
        "body edited AND slid +25: is_test reached the Module row and stopped",
    },
    {
        "G1-EDIT-003",
        "body_changed",
        "src/mcp/mcp.c",
        "handle_search_graph",
        "hyponoia.src.mcp.mcp.handle_search_graph",
        "4b72db3fefe443c53b34583fc1e032de",
        "4b72db3fefe443c53b34583fc1e032de@hyp1:hyponoia/hyponoia#hyponoia.src.mcp.mcp.handle_search_graph",
        "HYP_ANCHOR_RESOLVED_EDITED",
        0,
        mn_g1_edit_003,
        "body edited AND slid +109 — the discriminator against the RESOLVED cases",
    },
    {
        "G1-EDIT-004",
        "body_changed",
        "src/mcp/mcp.c",
        "cbm_mcp_text_result",
        "hyponoia.src.mcp.mcp.cbm_mcp_text_result",
        "e56d8f5d6b6bbade1a189707ff6a8ba7",
        "e56d8f5d6b6bbade1a189707ff6a8ba7@hyp1:hyponoia/hyponoia#hyponoia.src.mcp.mcp.cbm_mcp_text_result",
        "HYP_ANCHOR_RESOLVED_EDITED",
        0,
        mn_g1_edit_004,
        "body edited in place: stop repeating the payload in structuredContent",
    },
    {
        "G1-EDIT-005",
        "body_changed",
        "src/daemon/application.c",
        "application_mcp_request",
        "hyponoia.src.daemon.application.application_mcp_request",
        "9d11b65c683af5c23e85e721f27f6f7a",
        "9d11b65c683af5c23e85e721f27f6f7a@hyp1:hyponoia/hyponoia#hyponoia.src.daemon.application.application_mcp_request",
        "HYP_ANCHOR_RESOLVED_EDITED",
        0,
        mn_g1_edit_005,
        "body edited: answer an oversized MCP reply with an error, not process death",
    },
    {
        "G1-EDIT-006",
        "body_changed",
        "src/foundation/mem.c",
        "cbm_mem_init_with_cap",
        "hyponoia.src.foundation.mem.cbm_mem_init_with_cap",
        "e02ba2512500cb2f68e3b71291a9b266",
        "e02ba2512500cb2f68e3b71291a9b266@hyp1:hyponoia/hyponoia#hyponoia.src.foundation.mem.cbm_mem_init_with_cap",
        "HYP_ANCHOR_RESOLVED_EDITED",
        0,
        mn_g1_edit_006,
        "body edited: route ordinary malloc through mimalloc on Linux",
    },
    {
        "G1-EDIT-007",
        "body_changed",
        "src/ask/ask_llama.c",
        "ask_llama_open",
        "hyponoia.src.ask.ask_llama.ask_llama_open",
        "2ebfe4907030c5e416620ea8aca9b796",
        "2ebfe4907030c5e416620ea8aca9b796@hyp1:hyponoia/hyponoia#hyponoia.src.ask.ask_llama.ask_llama_open",
        "HYP_ANCHOR_RESOLVED_EDITED",
        0,
        mn_g1_edit_007,
        "body edited in place: bound the window by what one micro-batch can afford",
    },
    {
        "G1-EDIT-008",
        "body_changed",
        "src/mcp/mcp.c",
        "handle_ask",
        "hyponoia.src.mcp.mcp.handle_ask",
        "01e375ff567e7d4e22a4d29b10f9cbbd",
        "01e375ff567e7d4e22a4d29b10f9cbbd@hyp1:hyponoia/hyponoia#hyponoia.src.mcp.mcp.handle_ask",
        "HYP_ANCHOR_RESOLVED_EDITED",
        0,
        mn_g1_edit_008,
        "body edited and slid ONE line — the smallest real edit in the corpus",
    },
    {
        "G1-RENAME-001",
        "renamed",
        "src/store/store.c",
        "free_node_fields",
        "hyponoia.src.store.store.free_node_fields",
        "6646d130d9ce83e83754dba85268701c",
        "6646d130d9ce83e83754dba85268701c@hyp1:hyponoia/hyponoia#hyponoia.src.store.store.free_node_fields",
        "HYP_ANCHOR_ORPHANED",
        0,
        mn_g1_rename_001,
        "declaration line is the only changed line; same file, same line numbers",
    },
    {
        "G1-RENAME-002",
        "renamed",
        "src/pipeline/pass_lsp_cross.c",
        "pxc_collect_all_defs",
        "hyponoia.src.pipeline.pass_lsp_cross.pxc_collect_all_defs",
        "dc723ca6fce2a4a788a307010232c4c2",
        "dc723ca6fce2a4a788a307010232c4c2@hyp1:hyponoia/hyponoia#hyponoia.src.pipeline.pass_lsp_cross.pxc_collect_all_defs",
        "HYP_ANCHOR_ORPHANED",
        0,
        mn_g1_rename_002,
        "static helper promoted and prefixed; body otherwise identical",
    },
    {
        "G1-RENAME-003",
        "renamed",
        "internal/cbm/extract_defs.c",
        "resolve_func_name",
        "hyponoia.internal.cbm.extract_defs.resolve_func_name",
        "971a640ed3f7a41f7a7ade2f9637a0d3",
        "971a640ed3f7a41f7a7ade2f9637a0d3@hyp1:hyponoia/hyponoia#hyponoia.internal.cbm.extract_defs.resolve_func_name",
        "HYP_ANCHOR_ORPHANED",
        0,
        mn_g1_rename_003,
        "static helper promoted and prefixed",
    },
    {
        "G1-RENAME-004",
        "renamed",
        "internal/cbm/extract_defs.c",
        "cpp_out_of_line_parent_class",
        "hyponoia.internal.cbm.extract_defs.cpp_out_of_line_parent_class",
        "73c18280b2003ee1a6fac649e1c0e45b",
        "73c18280b2003ee1a6fac649e1c0e45b@hyp1:hyponoia/hyponoia#hyponoia.internal.cbm.extract_defs.cpp_out_of_line_parent_class",
        "HYP_ANCHOR_ORPHANED",
        0,
        mn_g1_rename_004,
        "static helper promoted and prefixed; same line numbers",
    },
    {
        "G1-RENAME-005",
        "renamed",
        "src/cli/hook_augment.c",
        "ha_read_stdin",
        "hyponoia.src.cli.hook_augment.ha_read_stdin",
        "07ffa00ef82b334890aac09a2d98f051",
        "07ffa00ef82b334890aac09a2d98f051@hyp1:hyponoia/hyponoia#hyponoia.src.cli.hook_augment.ha_read_stdin",
        "HYP_ANCHOR_ORPHANED",
        0,
        mn_g1_rename_005,
        "renamed to a name that shares no prefix with the old one",
    },
    {
        "G1-RENAME-006",
        "renamed",
        "src/mcp/compact_out.c",
        "cbm_toon_scalar_str",
        "hyponoia.src.mcp.compact_out.cbm_toon_scalar_str",
        "0c0085cf50facdc85dd5b833f30e80c7",
        "0c0085cf50facdc85dd5b833f30e80c7@hyp1:hyponoia/hyponoia#hyponoia.src.mcp.compact_out.cbm_toon_scalar_str",
        "HYP_ANCHOR_ORPHANED",
        0,
        mn_g1_rename_006,
        "THE PLAUSIBLE-NEIGHBOUR TRAP: the same commit adds cbm_tree_scalar_bool, cbm_tree_cell_str and cbm_tree_cell_bool, three more near-identical bodies",
    },
    {
        "G1-RENAME-007",
        "renamed",
        "src/cli/activation_transaction.c",
        "cbm_activation_transaction_refusal_note",
        "hyponoia.src.cli.activation_transaction.cbm_activation_transaction_refusal_note",
        "af8b575253a9b23c05de9656f2dbfb90",
        "af8b575253a9b23c05de9656f2dbfb90@hyp1:hyponoia/hyponoia#hyponoia.src.cli.activation_transaction.cbm_activation_transaction_refusal_note",
        "HYP_ANCHOR_ORPHANED",
        0,
        mn_g1_rename_007,
        "the mass rename, in a file that did NOT move: name only",
    },
    {
        "G1-RENAME-008",
        "renamed",
        "src/store/store.c",
        "louvain_free_adj",
        "hyponoia.src.store.store.louvain_free_adj",
        "b6ab0bf339b83c935f6e2b8f361c30f2",
        "b6ab0bf339b83c935f6e2b8f361c30f2@hyp1:hyponoia/hyponoia#hyponoia.src.store.store.louvain_free_adj",
        "HYP_ANCHOR_ORPHANED",
        0,
        mn_g1_rename_008,
        "renamed AND edited AND moved: nothing about it is recoverable by content",
    },
    {
        "G1-CROSSDIR-001",
        "cross_directory_move",
        "internal/cbm/lsp/py_lsp.c",
        "py_invalidate_possible_bindings",
        "hyponoia.internal.cbm.lsp.py_lsp.py_invalidate_possible_bindings",
        "2d2a8578efb05167390f5b1907a12d27",
        "2d2a8578efb05167390f5b1907a12d27@hyp1:hyponoia/hyponoia#hyponoia.internal.cbm.lsp.py_lsp.py_invalidate_possible_bindings",
        "HYP_ANCHOR_ORPHANED",
        1,
        mn_g1_crossdir_001,
        "138 lines, byte-identical, one directory over",
    },
    {
        "G1-CROSSDIR-002",
        "cross_directory_move",
        "internal/cbm/lsp/rust_lsp.c",
        "macro_consume_fragment",
        "hyponoia.internal.cbm.lsp.rust_lsp.macro_consume_fragment",
        "4051aa168f34034fa4d6664259dc547c",
        "4051aa168f34034fa4d6664259dc547c@hyp1:hyponoia/hyponoia#hyponoia.internal.cbm.lsp.rust_lsp.macro_consume_fragment",
        "HYP_ANCHOR_ORPHANED",
        1,
        mn_g1_crossdir_002,
        "97 lines, byte-identical",
    },
    {
        "G1-CROSSDIR-003",
        "cross_directory_move",
        "internal/cbm/service_patterns.c",
        "has_filesystem_extension",
        "hyponoia.internal.cbm.service_patterns.has_filesystem_extension",
        "4c13d9c93d46f90fec8c59dda6f61d72",
        "4c13d9c93d46f90fec8c59dda6f61d72@hyp1:hyponoia/hyponoia#hyponoia.internal.cbm.service_patterns.has_filesystem_extension",
        "HYP_ANCHOR_ORPHANED",
        1,
        mn_g1_crossdir_003,
        "47 lines, byte-identical",
    },
    {
        "G1-CROSSDIR-004",
        "cross_directory_move",
        "internal/cbm/helpers.c",
        "ks_build",
        "hyponoia.internal.cbm.helpers.ks_build",
        "52fe01d2c1e275790c3398edaef26567",
        "52fe01d2c1e275790c3398edaef26567@hyp1:hyponoia/hyponoia#hyponoia.internal.cbm.helpers.ks_build",
        "HYP_ANCHOR_ORPHANED",
        1,
        mn_g1_crossdir_004,
        "43 lines, byte-identical",
    },
    {
        "G1-CROSSDIR-005",
        "cross_directory_move",
        "internal/cbm/iris_export_xml.c",
        "emit_property",
        "hyponoia.internal.cbm.iris_export_xml.emit_property",
        "72be4d8fc72f2bbc81239edcede2e8b5",
        "72be4d8fc72f2bbc81239edcede2e8b5@hyp1:hyponoia/hyponoia#hyponoia.internal.cbm.iris_export_xml.emit_property",
        "HYP_ANCHOR_ORPHANED",
        1,
        mn_g1_crossdir_005,
        "50 lines, byte-identical",
    },
    {
        "G1-FILERENAME-001",
        "file_rename_same_directory",
        "internal/cbm/ts_node_stack.h",
        "ts_nstack_push",
        "hyponoia.internal.cbm.ts_node_stack.ts_nstack_push",
        "00053f0b8f7f116d0b22b1e2e7e01224",
        "00053f0b8f7f116d0b22b1e2e7e01224@hyp1:hyponoia/hyponoia#hyponoia.internal.cbm.ts_node_stack.ts_nstack_push",
        "HYP_ANCHOR_ORPHANED",
        1,
        mn_g1_filerename_001,
        "14 lines, byte-identical, same directory, renamed file",
    },
    {
        "G1-FILERENAME-002",
        "file_rename_same_directory",
        "internal/cbm/ts_node_stack.h",
        "ts_nstack_init",
        "hyponoia.internal.cbm.ts_node_stack.ts_nstack_init",
        "6962ad4ea8bb6f0abcbc1222aab97c2d",
        "6962ad4ea8bb6f0abcbc1222aab97c2d@hyp1:hyponoia/hyponoia#hyponoia.internal.cbm.ts_node_stack.ts_nstack_init",
        "HYP_ANCHOR_ORPHANED",
        1,
        mn_g1_filerename_002,
        "5 lines, byte-identical, same directory, renamed file",
    },
    {
        "G1-COPY-001",
        "copy_paste",
        "tests/repro/repro_lsp_c_cpp.c",
        "assert_lsp_strategy",
        "hyponoia.tests.repro.repro_lsp_c_cpp.assert_lsp_strategy",
        "f6ebd4d1ca52b6a056d1bcded4b1bccc",
        "f6ebd4d1ca52b6a056d1bcded4b1bccc@hyp1:hyponoia/hyponoia#hyponoia.tests.repro.repro_lsp_c_cpp.assert_lsp_strategy",
        "HYP_ANCHOR_RESOLVED",
        0,
        mn_g1_copy_001,
        "49 lines pasted into two new files in one commit; the original untouched",
    },
    {
        "G1-COPY-002",
        "copy_paste",
        "tests/repro/repro_lsp_go_py.c",
        "assert_lsp_strategy_files",
        "hyponoia.tests.repro.repro_lsp_go_py.assert_lsp_strategy_files",
        "227d4d8841d3cc7c711e766b59c1162c",
        "227d4d8841d3cc7c711e766b59c1162c@hyp1:hyponoia/hyponoia#hyponoia.tests.repro.repro_lsp_go_py.assert_lsp_strategy_files",
        "HYP_ANCHOR_RESOLVED",
        0,
        mn_g1_copy_002,
        "EXACTLY TWO addresses, one hash — §4's copy-paste case verbatim",
    },
    {
        "G1-COPY-003",
        "copy_paste",
        "tests/test_main.c",
        "suite_requested",
        "hyponoia.tests.test_main.suite_requested",
        "69e97d18253ac1290236d8fb1d53174b",
        "69e97d18253ac1290236d8fb1d53174b@hyp1:hyponoia/hyponoia#hyponoia.tests.test_main.suite_requested",
        "HYP_ANCHOR_RESOLVED",
        0,
        mn_g1_copy_003,
        "copied six weeks after the original, into a new test main",
    },
    {
        "G1-COPY-004",
        "copy_paste",
        "tests/repro/repro_grammar_core.c",
        "single_file_battery",
        "hyponoia.tests.repro.repro_grammar_core.single_file_battery",
        "1b0818dfb3f4be498ca3cfedd114e8ab",
        "1b0818dfb3f4be498ca3cfedd114e8ab@hyp1:hyponoia/hyponoia#hyponoia.tests.repro.repro_grammar_core.single_file_battery",
        "HYP_ANCHOR_RESOLVED",
        0,
        mn_g1_copy_004,
        "68 lines, the largest verbatim duplicate in the tree",
    },
    {
        "G1-COPY-005",
        "copy_paste",
        "tests/repro/repro_grammar_core.c",
        "pipeline_battery",
        "hyponoia.tests.repro.repro_grammar_core.pipeline_battery",
        "ea039e8f7545c327d4bf0b49521b6af3",
        "ea039e8f7545c327d4bf0b49521b6af3@hyp1:hyponoia/hyponoia#hyponoia.tests.repro.repro_grammar_core.pipeline_battery",
        "HYP_ANCHOR_RESOLVED",
        0,
        mn_g1_copy_005,
        "three addresses after one commit",
    },
};
const int g1_fixture_count = (int)(sizeof(g1_fixtures) / sizeof(g1_fixtures[0]));

static const char *const ca_g1_composed_2cand[] = {
    "hyp1:hyponoia/hyponoia#hyponoia.tests.repro.repro_lsp_go_py.assert_lsp_strategy_files",
    "hyp1:hyponoia/hyponoia#hyponoia.tests.repro.repro_lsp_ts.assert_lsp_strategy_files",
    NULL,
};
static const char *const ca_g1_composed_3cand[] = {
    "hyp1:hyponoia/hyponoia#hyponoia.tests.repro.repro_lsp_c_cpp.assert_lsp_strategy",
    "hyp1:hyponoia/hyponoia#hyponoia.tests.repro.repro_lsp_java_cs.assert_lsp_strategy",
    "hyp1:hyponoia/hyponoia#hyponoia.tests.repro.repro_lsp_kt_php_rust.assert_lsp_strategy",
    NULL,
};

const g1_composed_t g1_composed[] = {
    {
        "G1-COMPOSED-2CAND",
        "hyponoia.src.store.store.free_node_fields",
        "G1-RENAME-001",
        "227d4d8841d3cc7c711e766b59c1162c",
        "G1-COPY-002",
        2,
        ca_g1_composed_2cand,
    },
    {
        "G1-COMPOSED-3CAND",
        "hyponoia.src.store.store.free_node_fields",
        "G1-RENAME-001",
        "f6ebd4d1ca52b6a056d1bcded4b1bccc",
        "G1-COPY-001",
        3,
        ca_g1_composed_3cand,
    },
};
const int g1_composed_count = (int)(sizeof(g1_composed) / sizeof(g1_composed[0]));
