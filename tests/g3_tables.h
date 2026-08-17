/*
 * g3_tables.h — the Track G3 control corpus, as C tables.
 *
 * GENERATED. Every value here is copied from the mined corpus by a builder,
 * never typed: a number a reader cannot check by eye must not be transcribed.
 * Include it from test_g3_controls.c, after the two struct definitions it
 * uses. The span TEXT the addresses below refer to lives beside it in
 * tests/fixtures/g3/g3_corpus.txt.
 *
 * The commit pairs each control was mined from are in G3_PROVENANCE, as data
 * rather than as prose, so a reader can re-derive any row with git.
 */
#ifndef HYP_TESTS_G3_TABLES_H
#define HYP_TESTS_G3_TABLES_H

static const g3_node_spec_t G3_NODES[] = {
    {"g1r001_after", "hyponoia", "src/store/store.c", "Function", "cbm_node_free_fields", "g1r001_after/cbm_node_free_fields"},
    {"g1r002_after", "hyponoia", "src/pipeline/pass_lsp_cross.c", "Function", "cbm_pxc_collect_all_defs", "g1r002_after/cbm_pxc_collect_all_defs"},
    {"g1r003_after", "hyponoia", "internal/cbm/extract_defs.c", "Function", "cbm_resolve_func_name", "g1r003_after/cbm_resolve_func_name"},
    {"g1r004_after", "hyponoia", "internal/cbm/extract_defs.c", "Function", "cbm_cpp_out_of_line_parent_class", "g1r004_after/cbm_cpp_out_of_line_parent_class"},
    {"g1r005_after", "hyponoia", "src/cli/hook_augment.c", "Function", "cbm_hook_augment_read_stdin", "g1r005_after/cbm_hook_augment_read_stdin"},
    {"g1r005_after", "hyponoia", "src/cli/hook_augment.c", "Function", "cbm_hook_augment_process_for", "g1r005_after/cbm_hook_augment_process_for"},
    {"g1r006_after", "hyponoia", "src/mcp/compact_out.c", "Function", "cbm_tree_scalar_str", "g1r006_after/cbm_tree_scalar_str"},
    {"g1r006_after", "hyponoia", "src/mcp/compact_out.c", "Function", "cbm_tree_scalar_bool", "g1r006_after/cbm_tree_scalar_bool"},
    {"g1r006_after", "hyponoia", "src/mcp/compact_out.c", "Function", "cbm_tree_cell_str", "g1r006_after/cbm_tree_cell_str"},
    {"g1r006_after", "hyponoia", "src/mcp/compact_out.c", "Function", "cbm_tree_cell_bool", "g1r006_after/cbm_tree_cell_bool"},
    {"g1r007_after", "hyponoia", "src/cli/activation_transaction.c", "Function", "hyp_activation_transaction_refusal_note", "g1r007_after/hyp_activation_transaction_refusal_note"},
    {"g1r008_after", "hyponoia", "src/store/store.c", "Function", "lg_free", "g1r008_after/lg_free"},
    {"g1r008_after", "hyponoia", "src/store/store.c", "Function", "want_aspect", "g1r008_after/want_aspect"},
    {"forkA", "codebase-memory-mcp", "src/cli/hook_augment.c", "Function", "ha_sanitize_metadata", "forkA/ha_sanitize_metadata"},
    {"forkB", "hyponoia", "src/cli/hook_augment.c", "Function", "ha_sanitize_metadata", "forkB/ha_sanitize_metadata"},
    {"forkA", "codebase-memory-mcp", "src/foundation/workspace.c", "Function", "ws_volume_prefix_len", "forkA/ws_volume_prefix_len"},
    {"forkB", "hyponoia", "src/foundation/workspace.c", "Function", "ws_volume_prefix_len", "forkB/ws_volume_prefix_len"},
    {"forkA", "codebase-memory-mcp", "src/cypher/cypher.c", "Function", "parse_comparison_op", "forkA/parse_comparison_op"},
    {"forkB", "hyponoia", "src/cypher/cypher.c", "Function", "parse_comparison_op", "forkB/parse_comparison_op"},
    {"forkA", "codebase-memory-mcp", "src/foundation/slab_alloc.c", "Function", "slab_map_set", "forkA/slab_map_set"},
    {"forkB", "hyponoia", "src/foundation/slab_alloc.c", "Function", "slab_map_set", "forkB/slab_map_set"},
    {"forkA", "codebase-memory-mcp", "", "EnvVar", "HOME", ""},
    {"forkB", "hyponoia", "", "EnvVar", "HOME", ""},
    {"forkB", "hyponoia", "", "EnvVar", "HYP_CACHE_DIR", ""},
    {"unrecorded", "hyponoia", "src/pipeline/pipeline_incremental.c", "Function", "run_closure_delta", "unrecorded/run_closure_delta"},
    {"unrecorded", "hyponoia", "src/ui/layout3d.c", "Function", "hyp_layout_compute", "unrecorded/hyp_layout_compute"},
    {"unrecorded", "hyponoia", "internal/hyp/lsp/kotlin_lsp.c", "Function", "kt_repair_bodyless_interface_methods", "unrecorded/kt_repair_bodyless_interface_methods"},
    {"unrecorded", "hyponoia", "internal/hyp/lsp/php_lsp.c", "Function", "process_class_for_fields", "unrecorded/process_class_for_fields"},
    {"unrecorded", "hyponoia", "internal/hyp/lsp/rust_lsp.c", "Function", "rust_eval_expr_type", "unrecorded/rust_eval_expr_type"},
    {"unrecorded", "hyponoia", "internal/hyp/lsp/c_lsp.c", "Function", "c_neg_memo_insert", "unrecorded/c_neg_memo_insert"},
};

static const g3_record_spec_t G3_RECORDS[] = {
    {"6646d130d9ce83e83754dba85268701c@hyp1:hyponoia/hyponoia#hyponoia.src.store.store.free_node_fields",
     "The qualified name is not in the index after the rename, so the anchor is ORPHANED (I4: the span includes the decl line, so a rename changes the QN AND the hash). The recorded hash matches NOTHING in the after-tree, so there is not even a candidate. A resolver that re-attaches to hyponoia.src.store.store.cbm_node_free_fields has invented a link the data does not contain — C4u's exact failure mode."},
    {"dc723ca6fce2a4a788a307010232c4c2@hyp1:hyponoia/hyponoia#hyponoia.src.pipeline.pass_lsp_cross.pxc_collect_all_defs",
     "The qualified name is not in the index after the rename, so the anchor is ORPHANED (I4: the span includes the decl line, so a rename changes the QN AND the hash). The recorded hash matches NOTHING in the after-tree, so there is not even a candidate. A resolver that re-attaches to hyponoia.src.pipeline.pass_lsp_cross.cbm_pxc_collect_all_defs has invented a link the data does not contain — C4u's exact failure mode."},
    {"971a640ed3f7a41f7a7ade2f9637a0d3@hyp1:hyponoia/hyponoia#hyponoia.internal.cbm.extract_defs.resolve_func_name",
     "The qualified name is not in the index after the rename, so the anchor is ORPHANED (I4: the span includes the decl line, so a rename changes the QN AND the hash). The recorded hash matches NOTHING in the after-tree, so there is not even a candidate. A resolver that re-attaches to hyponoia.internal.cbm.extract_defs.cbm_resolve_func_name has invented a link the data does not contain — C4u's exact failure mode."},
    {"73c18280b2003ee1a6fac649e1c0e45b@hyp1:hyponoia/hyponoia#hyponoia.internal.cbm.extract_defs.cpp_out_of_line_parent_class",
     "The qualified name is not in the index after the rename, so the anchor is ORPHANED (I4: the span includes the decl line, so a rename changes the QN AND the hash). The recorded hash matches NOTHING in the after-tree, so there is not even a candidate. A resolver that re-attaches to hyponoia.internal.cbm.extract_defs.cbm_cpp_out_of_line_parent_class has invented a link the data does not contain — C4u's exact failure mode."},
    {"07ffa00ef82b334890aac09a2d98f051@hyp1:hyponoia/hyponoia#hyponoia.src.cli.hook_augment.ha_read_stdin",
     "The qualified name is not in the index after the rename, so the anchor is ORPHANED (I4: the span includes the decl line, so a rename changes the QN AND the hash). The recorded hash matches NOTHING in the after-tree, so there is not even a candidate. A resolver that re-attaches to hyponoia.src.cli.hook_augment.cbm_hook_augment_read_stdin has invented a link the data does not contain — C4u's exact failure mode."},
    {"0c0085cf50facdc85dd5b833f30e80c7@hyp1:hyponoia/hyponoia#hyponoia.src.mcp.compact_out.cbm_toon_scalar_str",
     "The qualified name is not in the index after the rename, so the anchor is ORPHANED (I4: the span includes the decl line, so a rename changes the QN AND the hash). The recorded hash matches NOTHING in the after-tree, so there is not even a candidate. A resolver that re-attaches to hyponoia.src.mcp.compact_out.cbm_tree_scalar_str has invented a link the data does not contain — C4u's exact failure mode."},
    {"af8b575253a9b23c05de9656f2dbfb90@hyp1:hyponoia/hyponoia#hyponoia.src.cli.activation_transaction.cbm_activation_transaction_refusal_note",
     "The qualified name is not in the index after the rename, so the anchor is ORPHANED (I4: the span includes the decl line, so a rename changes the QN AND the hash). The recorded hash matches NOTHING in the after-tree, so there is not even a candidate. A resolver that re-attaches to hyponoia.src.cli.activation_transaction.hyp_activation_transaction_refusal_note has invented a link the data does not contain — C4u's exact failure mode."},
    {"b6ab0bf339b83c935f6e2b8f361c30f2@hyp1:hyponoia/hyponoia#hyponoia.src.store.store.louvain_free_adj",
     "The qualified name is not in the index after the rename, so the anchor is ORPHANED (I4: the span includes the decl line, so a rename changes the QN AND the hash). The recorded hash matches NOTHING in the after-tree, so there is not even a candidate. A resolver that re-attaches to hyponoia.src.store.store.lg_free has invented a link the data does not contain — C4u's exact failure mode."},
    {"a2f88571a47f75066615070647fe3ccc@hyp1:fork-pair/codebase-memory-mcp#codebase-memory-mcp.src.cli.hook_augment.ha_sanitize_metadata",
     "Two repos, two addresses (C1: 'the same symbol in two repos gets two distinct addresses'). The span is BYTE-IDENTICAL in both, so a content-first resolver returns repo A's decision for repo B's symbol — a false positive that attaches one repo's reasoning to another's code. The address is repo-scoped, so the right answer is `anchor_status: resolved` with an EMPTY records list: the symbol is there, nothing is attached to it."},
    {"b95846577075797f5834e719407c7a1d@hyp1:fork-pair/codebase-memory-mcp#codebase-memory-mcp.src.foundation.workspace.ws_volume_prefix_len",
     "Two repos, two addresses (C1: 'the same symbol in two repos gets two distinct addresses'). The span is BYTE-IDENTICAL in both, so a content-first resolver returns repo A's decision for repo B's symbol — a false positive that attaches one repo's reasoning to another's code. The address is repo-scoped, so the right answer is `anchor_status: resolved` with an EMPTY records list: the symbol is there, nothing is attached to it."},
    {"e9af53257e22702c2458eff52849d523@hyp1:fork-pair/codebase-memory-mcp#codebase-memory-mcp.src.cypher.cypher.parse_comparison_op",
     "Two repos, two addresses (C1: 'the same symbol in two repos gets two distinct addresses'). The span is BYTE-IDENTICAL in both, so a content-first resolver returns repo A's decision for repo B's symbol — a false positive that attaches one repo's reasoning to another's code. The address is repo-scoped, so the right answer is `anchor_status: resolved` with an EMPTY records list: the symbol is there, nothing is attached to it."},
    {"ea4fc72c23a3ba6e2da077bcf5ddbce6@hyp1:fork-pair/codebase-memory-mcp#codebase-memory-mcp.src.foundation.slab_alloc.slab_map_set",
     "Two repos, two addresses (C1: 'the same symbol in two repos gets two distinct addresses'). The span is BYTE-IDENTICAL in both, so a content-first resolver returns repo A's decision for repo B's symbol — a false positive that attaches one repo's reasoning to another's code. The address is repo-scoped, so the right answer is `anchor_status: resolved` with an EMPTY records list: the symbol is there, nothing is attached to it."},
    {"hyp1:fork-pair#__env__HOME",
     "THIS ONE MUST SAY YES. `__env__HOME` is a rendezvous name — leading `__tag__`, not project-rooted (I2, shape-derived), so it is workspace-scoped and has no repo field at all. Two repos sharing it is the POINT. Running the isolation control on this address would be false-by-design, which is why §4 pairs the two. A suite that only ever says NO is a suite that has stopped discriminating."},
    {"hyp1:fork-pair#__env__CBM_CACHE_DIR",
     "Workspace scope is not a licence to guess. The rendezvous name is gone from every repo in the workspace, so the anchor is orphaned — and `__env__HYP_CACHE_DIR`, which IS present and IS the same variable renamed, is the plausible neighbour that must not be substituted."},
};

/* Anchors and addresses, verbatim from the corpus. */
#define G1_RENAME_001_ANCHOR "6646d130d9ce83e83754dba85268701c@hyp1:hyponoia/hyponoia#hyponoia.src.store.store.free_node_fields"
#define G1_RENAME_001_AFTER_ADDR "hyp1:hyponoia/hyponoia#hyponoia.src.store.store.cbm_node_free_fields"
#define G1_RENAME_001_AFTER_HASH "e79d00a0a40229cdda86d51a83037817"
#define G1_RENAME_001_TREE "g1r001_after"
#define G1_RENAME_002_ANCHOR "dc723ca6fce2a4a788a307010232c4c2@hyp1:hyponoia/hyponoia#hyponoia.src.pipeline.pass_lsp_cross.pxc_collect_all_defs"
#define G1_RENAME_002_AFTER_ADDR "hyp1:hyponoia/hyponoia#hyponoia.src.pipeline.pass_lsp_cross.cbm_pxc_collect_all_defs"
#define G1_RENAME_002_AFTER_HASH "bb8e5bcd5f6db597b09c9d4fd8391e21"
#define G1_RENAME_002_TREE "g1r002_after"
#define G1_RENAME_003_ANCHOR "971a640ed3f7a41f7a7ade2f9637a0d3@hyp1:hyponoia/hyponoia#hyponoia.internal.cbm.extract_defs.resolve_func_name"
#define G1_RENAME_003_AFTER_ADDR "hyp1:hyponoia/hyponoia#hyponoia.internal.cbm.extract_defs.cbm_resolve_func_name"
#define G1_RENAME_003_AFTER_HASH "f39b0ff6e4c855f0bbaf2af192d8d4b3"
#define G1_RENAME_003_TREE "g1r003_after"
#define G1_RENAME_004_ANCHOR "73c18280b2003ee1a6fac649e1c0e45b@hyp1:hyponoia/hyponoia#hyponoia.internal.cbm.extract_defs.cpp_out_of_line_parent_class"
#define G1_RENAME_004_AFTER_ADDR "hyp1:hyponoia/hyponoia#hyponoia.internal.cbm.extract_defs.cbm_cpp_out_of_line_parent_class"
#define G1_RENAME_004_AFTER_HASH "a620c219a40e359f6e8ad72270d690ba"
#define G1_RENAME_004_TREE "g1r004_after"
#define G1_RENAME_005_ANCHOR "07ffa00ef82b334890aac09a2d98f051@hyp1:hyponoia/hyponoia#hyponoia.src.cli.hook_augment.ha_read_stdin"
#define G1_RENAME_005_AFTER_ADDR "hyp1:hyponoia/hyponoia#hyponoia.src.cli.hook_augment.cbm_hook_augment_read_stdin"
#define G1_RENAME_005_AFTER_HASH "cb2a5f92a64f1467032591f071ddf683"
#define G1_RENAME_005_TREE "g1r005_after"
#define G1_RENAME_006_ANCHOR "0c0085cf50facdc85dd5b833f30e80c7@hyp1:hyponoia/hyponoia#hyponoia.src.mcp.compact_out.cbm_toon_scalar_str"
#define G1_RENAME_006_AFTER_ADDR "hyp1:hyponoia/hyponoia#hyponoia.src.mcp.compact_out.cbm_tree_scalar_str"
#define G1_RENAME_006_AFTER_HASH "03307df26d207617044ccbcfb7b66884"
#define G1_RENAME_006_TREE "g1r006_after"
#define G1_RENAME_007_ANCHOR "af8b575253a9b23c05de9656f2dbfb90@hyp1:hyponoia/hyponoia#hyponoia.src.cli.activation_transaction.cbm_activation_transaction_refusal_note"
#define G1_RENAME_007_AFTER_ADDR "hyp1:hyponoia/hyponoia#hyponoia.src.cli.activation_transaction.hyp_activation_transaction_refusal_note"
#define G1_RENAME_007_AFTER_HASH "58cb9c72d2994100cfc8b30108a5e513"
#define G1_RENAME_007_TREE "g1r007_after"
#define G1_RENAME_008_ANCHOR "b6ab0bf339b83c935f6e2b8f361c30f2@hyp1:hyponoia/hyponoia#hyponoia.src.store.store.louvain_free_adj"
#define G1_RENAME_008_AFTER_ADDR "hyp1:hyponoia/hyponoia#hyponoia.src.store.store.lg_free"
#define G1_RENAME_008_AFTER_HASH "bb39073094b17ec0edfbe0914424e698"
#define G1_RENAME_008_TREE "g1r008_after"
/* NC-RENAME-001 and G1-RENAME-006 are the same commit pair. */
#define NC_RENAME_001_ANCHOR G1_RENAME_006_ANCHOR
static const char *const NC_RENAME_001_MUST_NOT[] = {
    "hyp1:hyponoia/hyponoia#hyponoia.src.mcp.compact_out.cbm_tree_scalar_str",
    "hyp1:hyponoia/hyponoia#hyponoia.src.mcp.compact_out.cbm_tree_scalar_bool",
    "hyp1:hyponoia/hyponoia#hyponoia.src.mcp.compact_out.cbm_tree_cell_str",
    "hyp1:hyponoia/hyponoia#hyponoia.src.mcp.compact_out.cbm_tree_cell_bool",
};
/* NC-RENAME-002 and G1-RENAME-007 are the same commit pair. */
#define NC_RENAME_002_ANCHOR G1_RENAME_007_ANCHOR
static const char *const NC_RENAME_002_MUST_NOT[] = {
    "hyp1:hyponoia/hyponoia#hyponoia.src.cli.activation_transaction.hyp_activation_transaction_refusal_note",
};
/* NC-RENAME-003 and G1-RENAME-001 are the same commit pair. */
#define NC_RENAME_003_ANCHOR G1_RENAME_001_ANCHOR
static const char *const NC_RENAME_003_MUST_NOT[] = {
    "hyp1:hyponoia/hyponoia#hyponoia.src.store.store.cbm_node_free_fields",
};
/* NC-RENAME-004 and G1-RENAME-008 are the same commit pair. */
#define NC_RENAME_004_ANCHOR G1_RENAME_008_ANCHOR
static const char *const NC_RENAME_004_MUST_NOT[] = {
    "hyp1:hyponoia/hyponoia#hyponoia.src.store.store.lg_free",
    "hyp1:hyponoia/hyponoia#hyponoia.src.store.store.want_aspect",
};
/* NC-RENAME-005 and G1-RENAME-005 are the same commit pair. */
#define NC_RENAME_005_ANCHOR G1_RENAME_005_ANCHOR
static const char *const NC_RENAME_005_MUST_NOT[] = {
    "hyp1:hyponoia/hyponoia#hyponoia.src.cli.hook_augment.cbm_hook_augment_read_stdin",
    "hyp1:hyponoia/hyponoia#hyponoia.src.cli.hook_augment.cbm_hook_augment_process_for",
};
#define NC_ISO_001_RECORD_ANCHOR "a2f88571a47f75066615070647fe3ccc@hyp1:fork-pair/codebase-memory-mcp#codebase-memory-mcp.src.cli.hook_augment.ha_sanitize_metadata"
#define NC_ISO_001_QUERY "hyp1:fork-pair/hyponoia#hyponoia.src.cli.hook_augment.ha_sanitize_metadata"
#define NC_ISO_001_HASH "a2f88571a47f75066615070647fe3ccc"
#define NC_ISO_002_RECORD_ANCHOR "b95846577075797f5834e719407c7a1d@hyp1:fork-pair/codebase-memory-mcp#codebase-memory-mcp.src.foundation.workspace.ws_volume_prefix_len"
#define NC_ISO_002_QUERY "hyp1:fork-pair/hyponoia#hyponoia.src.foundation.workspace.ws_volume_prefix_len"
#define NC_ISO_002_HASH "b95846577075797f5834e719407c7a1d"
#define NC_ISO_003_RECORD_ANCHOR "e9af53257e22702c2458eff52849d523@hyp1:fork-pair/codebase-memory-mcp#codebase-memory-mcp.src.cypher.cypher.parse_comparison_op"
#define NC_ISO_003_QUERY "hyp1:fork-pair/hyponoia#hyponoia.src.cypher.cypher.parse_comparison_op"
#define NC_ISO_003_HASH "e9af53257e22702c2458eff52849d523"
#define NC_ISO_004_RECORD_ANCHOR "ea4fc72c23a3ba6e2da077bcf5ddbce6@hyp1:fork-pair/codebase-memory-mcp#codebase-memory-mcp.src.foundation.slab_alloc.slab_map_set"
#define NC_ISO_004_QUERY "hyp1:fork-pair/hyponoia#hyponoia.src.foundation.slab_alloc.slab_map_set"
#define NC_ISO_004_HASH "ea4fc72c23a3ba6e2da077bcf5ddbce6"
#define PC_RENDEZVOUS_001_ADDR "hyp1:fork-pair#__env__HOME"
#define NC_RENDEZVOUS_002_ADDR "hyp1:fork-pair#__env__CBM_CACHE_DIR"
#define NC_RENDEZVOUS_002_MUST_NOT "hyp1:fork-pair#__env__HYP_CACHE_DIR"
#define NC_UNRECORDED_001_ADDR "hyp1:hyponoia/hyponoia#hyponoia.src.pipeline.pipeline_incremental.run_closure_delta"
#define NC_UNRECORDED_001_SYMBOL "run_closure_delta"
#define NC_UNRECORDED_002_ADDR "hyp1:hyponoia/hyponoia#hyponoia.src.ui.layout3d.hyp_layout_compute"
#define NC_UNRECORDED_002_SYMBOL "hyp_layout_compute"
#define NC_UNRECORDED_003_ADDR "hyp1:hyponoia/hyponoia#hyponoia.internal.hyp.lsp.kotlin_lsp.kt_repair_bodyless_interface_methods"
#define NC_UNRECORDED_003_SYMBOL "kt_repair_bodyless_interface_methods"
#define NC_UNRECORDED_004_ADDR "hyp1:hyponoia/hyponoia#hyponoia.internal.hyp.lsp.php_lsp.process_class_for_fields"
#define NC_UNRECORDED_004_SYMBOL "process_class_for_fields"
#define NC_UNRECORDED_005_ADDR "hyp1:hyponoia/hyponoia#hyponoia.internal.hyp.lsp.rust_lsp.rust_eval_expr_type"
#define NC_UNRECORDED_005_SYMBOL "rust_eval_expr_type"
#define NC_UNRECORDED_006_ADDR "hyp1:hyponoia/hyponoia#hyponoia.internal.hyp.lsp.c_lsp.c_neg_memo_insert"
#define NC_UNRECORDED_006_SYMBOL "c_neg_memo_insert"

/* Where each control came from, as data: control, before commit, after
 * commit, path, and the symbol either side. Re-derive any row with
 * `git diff <before> <after> -- <path>`. */
static const char *const G3_PROVENANCE[][6] = {
    {"G1-RENAME-001", "93c0b865c4bacd24eea9f0cb81998ecc0f5032ad", "c9ec1d1eb4b0b6f39d8d851f7f25415eb062df12", "src/store/store.c", "free_node_fields", "cbm_node_free_fields"},
    {"G1-RENAME-002", "6226972f13f867e332dd8721df763946926400de", "75a0df688fef98af4d600110dba55f146ddfbabc", "src/pipeline/pass_lsp_cross.c", "pxc_collect_all_defs", "cbm_pxc_collect_all_defs"},
    {"G1-RENAME-003", "639bac9c5da1b977dbf116bedeb8cf3124a241d0", "6e6b34a9bbaa1919f8964d98e513ee82afa6c3ce", "internal/cbm/extract_defs.c", "resolve_func_name", "cbm_resolve_func_name"},
    {"G1-RENAME-004", "b9b4669a20cbf655981f15580e574328fc4f6920", "3f754cf449f941508a6dcef6ee81321a8cb408a0", "internal/cbm/extract_defs.c", "cpp_out_of_line_parent_class", "cbm_cpp_out_of_line_parent_class"},
    {"G1-RENAME-005", "a688d85cc8fda6ef09b1ed4534f967faeef63cc3", "0e00ef5702d4a67b6fed667cf66800a5a0fc75f1", "src/cli/hook_augment.c", "ha_read_stdin", "cbm_hook_augment_read_stdin"},
    {"G1-RENAME-006", "c5bffb7fe6cbe15efd3c84f513c9bcda302956e3", "3f5ae18852f5b578edcb9fe364b3423ea22748be", "src/mcp/compact_out.c", "cbm_toon_scalar_str", "cbm_tree_scalar_str"},
    {"G1-RENAME-007", "10cb0e03fbb03fc62435174df5a52cad3186c444", "f95d684123bd2ab7fdf1886ad5c6f020e0c579cf", "src/cli/activation_transaction.c", "cbm_activation_transaction_refusal_note", "hyp_activation_transaction_refusal_note"},
    {"G1-RENAME-008", "eb50569bed508b71e1f18d2f2f516fec050f441b", "955b87d3fd31552bbd8885b63f6e6194c50c50ff", "src/store/store.c", "louvain_free_adj", "lg_free"},
    {"NC-ISO-001", "10cb0e03fbb03fc62435174df5a52cad3186c444", "f95d684123bd2ab7fdf1886ad5c6f020e0c579cf", "src/cli/hook_augment.c", "ha_sanitize_metadata", "ha_sanitize_metadata"},
    {"NC-ISO-002", "10cb0e03fbb03fc62435174df5a52cad3186c444", "f95d684123bd2ab7fdf1886ad5c6f020e0c579cf", "src/foundation/workspace.c", "ws_volume_prefix_len", "ws_volume_prefix_len"},
    {"NC-ISO-003", "10cb0e03fbb03fc62435174df5a52cad3186c444", "f95d684123bd2ab7fdf1886ad5c6f020e0c579cf", "src/cypher/cypher.c", "parse_comparison_op", "parse_comparison_op"},
    {"NC-ISO-004", "10cb0e03fbb03fc62435174df5a52cad3186c444", "f95d684123bd2ab7fdf1886ad5c6f020e0c579cf", "src/foundation/slab_alloc.c", "slab_map_set", "slab_map_set"},
    {"PC-RENDEZVOUS-001", "10cb0e03fbb03fc62435174df5a52cad3186c444", "f95d684123bd2ab7fdf1886ad5c6f020e0c579cf", "", "hyp1:fork-pair#__env__HOME", ""},
    {"NC-RENDEZVOUS-002", "10cb0e03fbb03fc62435174df5a52cad3186c444", "f95d684123bd2ab7fdf1886ad5c6f020e0c579cf", "", "hyp1:fork-pair#__env__CBM_CACHE_DIR", ""},
    {"NC-UNRECORDED-001", "d3e9806f440fa7e18e3d5e02f55db000bd3df5c7", "d3e9806f440fa7e18e3d5e02f55db000bd3df5c7", "src/pipeline/pipeline_incremental.c", "run_closure_delta", "run_closure_delta"},
    {"NC-UNRECORDED-002", "d3e9806f440fa7e18e3d5e02f55db000bd3df5c7", "d3e9806f440fa7e18e3d5e02f55db000bd3df5c7", "src/ui/layout3d.c", "hyp_layout_compute", "hyp_layout_compute"},
    {"NC-UNRECORDED-003", "d3e9806f440fa7e18e3d5e02f55db000bd3df5c7", "d3e9806f440fa7e18e3d5e02f55db000bd3df5c7", "internal/hyp/lsp/kotlin_lsp.c", "kt_repair_bodyless_interface_methods", "kt_repair_bodyless_interface_methods"},
    {"NC-UNRECORDED-004", "d3e9806f440fa7e18e3d5e02f55db000bd3df5c7", "d3e9806f440fa7e18e3d5e02f55db000bd3df5c7", "internal/hyp/lsp/php_lsp.c", "process_class_for_fields", "process_class_for_fields"},
    {"NC-UNRECORDED-005", "d3e9806f440fa7e18e3d5e02f55db000bd3df5c7", "d3e9806f440fa7e18e3d5e02f55db000bd3df5c7", "internal/hyp/lsp/rust_lsp.c", "rust_eval_expr_type", "rust_eval_expr_type"},
    {"NC-UNRECORDED-006", "d3e9806f440fa7e18e3d5e02f55db000bd3df5c7", "d3e9806f440fa7e18e3d5e02f55db000bd3df5c7", "internal/hyp/lsp/c_lsp.c", "c_neg_memo_insert", "c_neg_memo_insert"},
};

#endif /* HYP_TESTS_G3_TABLES_H */
