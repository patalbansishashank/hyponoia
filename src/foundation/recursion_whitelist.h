/*
 * recursion_whitelist.h — Functions approved for recursive implementation.
 *
 * These functions use bounded recursion where iterative conversion would
 * add complexity with no practical benefit:
 *
 * Cypher recursive descent parser (bounded by query nesting depth ~5):
 *   - parse_or_expr, parse_xor_expr, parse_and_expr, parse_not_expr
 *   - parse_atom_expr, parse_post_where, hyp_parse
 *
 * Cypher expression evaluator (bounded by WHERE clause depth ~5):
 *   - eval_expr
 *
 * Glob pattern matcher (bounded by pattern nesting ~3):
 *   - glob_match, glob_match_star, glob_match_doublestar
 *   - glob_match_doublestar_slash, glob_match_doublestar_any
 *
 * R import scanner (bounded by source-file AST depth):
 *   - r_collect_imports
 *
 * Extraction descendant search (bounded by AST depth):
 *   - find_first_descendant_by_kind (Verilog/SystemVerilog name wrappers)
 *   - find_first_descendant_of (Dart/Zig import URI/string nesting)
 *
 * Role evidence scan (bounded twice, by depth and by sample count):
 *   - role_scan_walk — descends a repository tree looking for DO-NOT-EDIT
 *     markers. Returns at HYP_ROLE_SCAN_MAX_DEPTH and again once
 *     HYP_ROLE_SCAN_MAX_FILES have been sampled, so neither a deep tree nor a
 *     wide one can extend it; a symlink cycle is bounded by the depth limit
 *     rather than by cycle detection.
 *
 * To add a function: add it below AND add NOLINT(misc-no-recursion) on
 * the function definition line. The lint gate verifies both match.
 */
#define HYP_RECURSION_WHITELIST                                                               \
    "parse_or_expr", "parse_xor_expr", "parse_and_expr", "parse_not_expr", "parse_atom_expr", \
        "parse_post_where", "hyp_parse", "eval_expr", "glob_match", "glob_match_star",        \
        "glob_match_doublestar", "glob_match_doublestar_slash", "glob_match_doublestar_any",  \
        "parse_bool_expr", "parse_bool_atom", "r_collect_imports",                            \
        "find_first_descendant_by_kind", "find_first_descendant_of", "role_scan_walk"
