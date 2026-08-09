/*
 * repro_lsp_ordered_signatures.c — Counted positional signature transport into
 * the project-wide LSP registries.
 *
 * The public HYPDefinition.param_types array is a legacy type-usage projection:
 * it is not positional.  Cross-LSP registration instead receives the internal
 * HYPLSPDef.signature_param_types + signature_param_count carrier.  These tests
 * start at the production registry-population boundary so a GREEN result means
 * the registry consumer, not merely extraction, retained every argument slot.
 *
 * Contracts exercised per typed language:
 *   - non-palindromic [integer, UNKNOWN, string] catches reordering;
 *   - [integer, integer] catches accidental deduplication;
 *   - [integer, string, UNKNOWN] catches NULL-termination at an unknown slot;
 *   - only the exact text "?" means UNKNOWN; a language-valid nullable type is
 *     still a real, non-UNKNOWN type;
 *   - signature_param_count is authoritative.  The non-palindromic carrier has
 *     a fourth, out-of-count bool entry which must not be scanned or merged, and
 *     a non-NULL carrier with count zero must materialize zero parameters.
 *
 * JavaScript has no source annotations in its primary carrier, so its contract
 * is three explicit UNKNOWN slots (rather than silently collapsing to zero).
 * Every listed frontend now consumes the counted carrier. These cases are
 * permanent gating guards against reordering, deduplication, or truncation.
 */

#include "test_framework.h"

#include "arena.h"
#include "hyp.h"
#include "lang_specs.h"
#include "lsp/c_lsp.h"
#include "lsp/cs_lsp.h"
#include "lsp/go_lsp.h"
#include "lsp/java_lsp.h"
#include "lsp/kotlin_lsp.h"
#include "lsp/php_lsp.h"
#include "lsp/py_lsp.h"
#include "lsp/rust_lsp.h"
#include "lsp/ts_lsp.h"
#include "lsp/type_registry.h"
#include "lsp/type_rep.h"
#include "tree_sitter/api.h"

#include <string.h>

typedef HYPTypeRegistry *(*CrossRegistryBuilder)(HYPArena *, HYPLSPDef *, int);

typedef struct {
    const char *tag;
    HYPLanguage lang;
    CrossRegistryBuilder build;
    const char *integer_text;
    const char *string_text;
    const char *nullable_text;
    const char *return_text;
} OrderedSignatureLanguage;

static TSTree *parse_lsp_fixture(HYPLanguage language, const char *source) {
    const TSLanguage *grammar = hyp_ts_language(language);
    TSParser *parser = ts_parser_new();
    if (!grammar || !parser || !ts_parser_set_language(parser, grammar)) {
        if (parser)
            ts_parser_delete(parser);
        return NULL;
    }
    TSTree *tree = ts_parser_parse_string(parser, NULL, source, (uint32_t)strlen(source));
    ts_parser_delete(parser);
    return tree;
}

static TSNode find_first_lsp_node_of_kind(TSNode node, const char *kind) {
    if (strcmp(ts_node_type(node), kind) == 0)
        return node;

    uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; i++) {
        TSNode found = find_first_lsp_node_of_kind(ts_node_child(node, i), kind);
        if (!ts_node_is_null(found))
            return found;
    }

    TSNode missing = {0};
    return missing;
}

static HYPTypeRegistry *alloc_test_registry(HYPArena *arena) {
    HYPTypeRegistry *registry = (HYPTypeRegistry *)hyp_arena_alloc(arena, sizeof(*registry));
    if (!registry)
        return NULL;
    hyp_registry_init(registry, arena);
    return registry;
}

static HYPTypeRegistry *build_java_cross_registry_for_contract(HYPArena *arena, HYPLSPDef *defs,
                                                               int def_count) {
    HYPTypeRegistry *registry = alloc_test_registry(arena);
    if (registry)
        hyp_java_register_lsp_defs(arena, registry, defs, def_count);
    return registry;
}

static HYPTypeRegistry *build_kotlin_cross_registry_for_contract(HYPArena *arena, HYPLSPDef *defs,
                                                                 int def_count) {
    HYPTypeRegistry *registry = alloc_test_registry(arena);
    if (registry)
        hyp_kotlin_register_lsp_defs(arena, registry, defs, def_count, NULL);
    return registry;
}

static HYPTypeRegistry *build_php_cross_registry_for_contract(HYPArena *arena, HYPLSPDef *defs,
                                                              int def_count) {
    HYPTypeRegistry *registry = alloc_test_registry(arena);
    if (registry)
        hyp_php_register_lsp_defs(arena, NULL, registry, defs, def_count);
    return registry;
}

static int func_param_count(const HYPType *signature) {
    if (!signature || signature->kind != HYP_TYPE_FUNC || !signature->data.func.param_types)
        return 0;
    int count = 0;
    while (signature->data.func.param_types[count])
        count++;
    return count;
}

static const char *last_type_component(const char *name) {
    if (!name)
        return NULL;
    const char *leaf = name;
    for (const char *cursor = name; *cursor; cursor++) {
        if (*cursor == '.' || *cursor == ':')
            leaf = cursor + 1;
    }
    return leaf;
}

static const char *type_atom_name(const HYPType *type) {
    if (!type)
        return NULL;
    switch (type->kind) {
    case HYP_TYPE_BUILTIN:
        return type->data.builtin.name;
    case HYP_TYPE_NAMED:
        return type->data.named.qualified_name;
    case HYP_TYPE_TYPE_PARAM:
        return type->data.type_param.name;
    case HYP_TYPE_TEMPLATE:
        return type->data.template_type.template_name;
    case HYP_TYPE_ALIAS:
        return type->data.alias.alias_qn;
    default:
        return NULL;
    }
}

static int param_matches(const HYPType *actual, const char *expected) {
    if (strcmp(expected, "?") == 0)
        return hyp_type_is_unknown(actual);
    if (!actual || hyp_type_is_unknown(actual))
        return 0;
    const char *name = type_atom_name(actual);
    const char *leaf = last_type_component(name);
    return leaf && strcmp(leaf, expected) == 0;
}

static int check_signature(const HYPTypeRegistry *registry, const char *qualified_name,
                           const char *case_name, const char *const *expected, int expected_count) {
    const HYPRegisteredFunc *func = hyp_registry_lookup_func(registry, qualified_name);
    if (!func) {
        fprintf(stderr, "  [ordered-signature] case=%s missing function=%s\n", case_name,
                qualified_name);
        return 1;
    }
    if (!func->signature || func->signature->kind != HYP_TYPE_FUNC) {
        fprintf(stderr, "  [ordered-signature] case=%s function=%s has no FUNC signature\n",
                case_name, qualified_name);
        return 1;
    }

    int actual_count = func_param_count(func->signature);
    if (actual_count != expected_count) {
        fprintf(stderr,
                "  [ordered-signature] case=%s function=%s expected-count=%d actual-count=%d\n",
                case_name, qualified_name, expected_count, actual_count);
        return 1;
    }

    for (int i = 0; i < expected_count; i++) {
        const HYPType *actual = func->signature->data.func.param_types[i];
        if (!param_matches(actual, expected[i])) {
            const char *actual_name = type_atom_name(actual);
            fprintf(stderr,
                    "  [ordered-signature] case=%s function=%s slot=%d expected=%s "
                    "actual=%s kind=%d\n",
                    case_name, qualified_name, i, expected[i],
                    actual_name ? actual_name
                                : (hyp_type_is_unknown(actual) ? "?" : "<structured>"),
                    actual ? (int)actual->kind : -1);
            return 1;
        }
    }
    return 0;
}

static int check_single_return(const HYPTypeRegistry *registry, const char *qualified_name,
                               const char *case_name, const char *expected) {
    const HYPRegisteredFunc *func = hyp_registry_lookup_func(registry, qualified_name);
    if (!func || !func->signature || func->signature->kind != HYP_TYPE_FUNC ||
        !func->signature->data.func.return_types || !func->signature->data.func.return_types[0]) {
        fprintf(stderr, "  [ordered-signature] case=%s function=%s missing return type\n",
                case_name, qualified_name);
        return 1;
    }
    const HYPType *actual = func->signature->data.func.return_types[0];
    if (!param_matches(actual, expected)) {
        const char *actual_name = type_atom_name(actual);
        fprintf(stderr,
                "  [ordered-signature] case=%s function=%s expected-return=%s actual=%s kind=%d\n",
                case_name, qualified_name, expected,
                actual_name ? actual_name : (hyp_type_is_unknown(actual) ? "?" : "<structured>"),
                (int)actual->kind);
        return 1;
    }
    return 0;
}

static int check_nullable_signature(const HYPTypeRegistry *registry, const char *qualified_name,
                                    const char *case_name) {
    const HYPRegisteredFunc *func = hyp_registry_lookup_func(registry, qualified_name);
    if (!func || !func->signature || func->signature->kind != HYP_TYPE_FUNC) {
        fprintf(stderr, "  [ordered-signature] case=%s missing FUNC signature=%s\n", case_name,
                qualified_name);
        return 1;
    }
    int actual_count = func_param_count(func->signature);
    if (actual_count != 1) {
        fprintf(stderr,
                "  [ordered-signature] case=%s function=%s expected-count=1 actual-count=%d\n",
                case_name, qualified_name, actual_count);
        return 1;
    }
    if (hyp_type_is_unknown(func->signature->data.func.param_types[0])) {
        fprintf(stderr,
                "  [ordered-signature] case=%s function=%s nullable type collapsed to UNKNOWN\n",
                case_name, qualified_name);
        return 1;
    }
    return 0;
}

static void init_function_def(HYPLSPDef *def, HYPArena *arena,
                              const OrderedSignatureLanguage *language, const char *case_name,
                              const char **param_types, int param_count) {
    memset(def, 0, sizeof(*def));
    def->qualified_name = hyp_arena_sprintf(arena, "hyp.ordered.%s.%s", language->tag, case_name);
    def->short_name = case_name;
    def->label = "Function";
    def->def_module_qn = hyp_arena_sprintf(arena, "hyp.ordered.%s", language->tag);
    def->return_types = language->return_text;
    def->signature_param_types = param_types;
    def->signature_param_count = param_count;
    def->lang = language->lang;
}

static int run_typed_language_contract(const OrderedSignatureLanguage *language) {
    HYPArena arena;
    hyp_arena_init(&arena);

    /* The out-of-count bool is deliberate: count=3, so it must never surface. */
    const char *nonpal_carrier[] = {language->integer_text, "?", language->string_text, "bool",
                                    NULL};
    const char *duplicate_carrier[] = {language->integer_text, language->integer_text, NULL};
    const char *trailing_carrier[] = {language->integer_text, language->string_text, "?", NULL};
    const char *nullable_carrier[] = {language->nullable_text, NULL};
    /* A stale pointer with authoritative count zero must not be scanned. */
    const char *count_zero_carrier[] = {language->integer_text, language->string_text, NULL};

    HYPLSPDef defs[5];
    init_function_def(&defs[0], &arena, language, "nonpal", nonpal_carrier, 3);
    init_function_def(&defs[1], &arena, language, "duplicate", duplicate_carrier, 2);
    init_function_def(&defs[2], &arena, language, "trailing_unknown", trailing_carrier, 3);
    init_function_def(&defs[3], &arena, language, "nullable", nullable_carrier, 1);
    init_function_def(&defs[4], &arena, language, "count_zero", count_zero_carrier, 0);

    HYPTypeRegistry *registry = language->build(&arena, defs, 5);
    if (!registry) {
        fprintf(stderr, "  [ordered-signature] lang=%s registry build failed\n", language->tag);
        hyp_arena_destroy(&arena);
        return 1;
    }

    /* C# deliberately canonicalizes source aliases in its semantic registry.
     * Keep feeding valid source spellings (`int`, `string`) while checking the
     * canonical leaf names, rather than weakening production parsing to satisfy
     * a spelling-only assertion. */
    const bool csharp_canonical = strcmp(language->tag, "csharp") == 0;
    const char *integer_expected = csharp_canonical ? "Int32" : language->integer_text;
    const char *string_expected = csharp_canonical ? "String" : language->string_text;
    const char *nonpal_expected[] = {integer_expected, "?", string_expected};
    const char *duplicate_expected[] = {integer_expected, integer_expected};
    const char *trailing_expected[] = {integer_expected, string_expected, "?"};
    int failures = 0;
    failures += check_signature(registry, defs[0].qualified_name, "nonpal", nonpal_expected, 3);
    failures +=
        check_signature(registry, defs[1].qualified_name, "duplicate", duplicate_expected, 2);
    failures +=
        check_signature(registry, defs[2].qualified_name, "trailing_unknown", trailing_expected, 3);
    failures += check_nullable_signature(registry, defs[3].qualified_name, "nullable");
    failures += check_signature(registry, defs[4].qualified_name, "count_zero", NULL, 0);

    hyp_arena_destroy(&arena);
    return failures;
}

static int run_javascript_unknown_contract(const OrderedSignatureLanguage *language) {
    HYPArena arena;
    hyp_arena_init(&arena);

    const char *unknown_carrier[] = {"?", "?", "?", "number", NULL};
    const char *count_zero_carrier[] = {"number", NULL};
    HYPLSPDef defs[2];
    init_function_def(&defs[0], &arena, language, "three_unknowns", unknown_carrier, 3);
    init_function_def(&defs[1], &arena, language, "count_zero", count_zero_carrier, 0);

    HYPTypeRegistry *registry = language->build(&arena, defs, 2);
    if (!registry) {
        fprintf(stderr, "  [ordered-signature] lang=%s registry build failed\n", language->tag);
        hyp_arena_destroy(&arena);
        return 1;
    }

    const char *expected[] = {"?", "?", "?"};
    int failures = check_signature(registry, defs[0].qualified_name, "three_unknowns", expected, 3);
    failures += check_signature(registry, defs[1].qualified_name, "count_zero", NULL, 0);
    hyp_arena_destroy(&arena);
    return failures;
}

static const OrderedSignatureLanguage C_LANGUAGE = {
    "c", HYP_LANG_C, hyp_c_build_cross_registry, "int", "string", "Thing *", "void"};
static const OrderedSignatureLanguage CPP_LANGUAGE = {
    "cpp", HYP_LANG_CPP, hyp_c_build_cross_registry, "int", "string", "std::optional<Thing>",
    "void"};
static const OrderedSignatureLanguage CUDA_LANGUAGE = {
    "cuda", HYP_LANG_CUDA, hyp_c_build_cross_registry, "int", "string", "Thing *", "void"};
static const OrderedSignatureLanguage GO_LANGUAGE = {
    "go", HYP_LANG_GO, hyp_go_build_cross_registry, "int", "string", "*Thing", "void"};
static const OrderedSignatureLanguage JS_LANGUAGE = {
    "javascript", HYP_LANG_JAVASCRIPT, hyp_ts_build_cross_registry, NULL, NULL, NULL, "void"};
static const OrderedSignatureLanguage TS_LANGUAGE = {"typescript",
                                                     HYP_LANG_TYPESCRIPT,
                                                     hyp_ts_build_cross_registry,
                                                     "number",
                                                     "string",
                                                     "Thing | null",
                                                     "void"};
static const OrderedSignatureLanguage TSX_LANGUAGE = {
    "tsx", HYP_LANG_TSX, hyp_ts_build_cross_registry, "number", "string", "Thing | null", "void"};
static const OrderedSignatureLanguage PYTHON_LANGUAGE = {
    "python", HYP_LANG_PYTHON, hyp_py_build_cross_registry, "int", "str", "Thing | None", "None"};
static const OrderedSignatureLanguage CSHARP_LANGUAGE = {
    "csharp", HYP_LANG_CSHARP, hyp_cs_build_cross_registry, "int", "string", "Thing?", "void"};
static const OrderedSignatureLanguage RUST_LANGUAGE = {
    "rust", HYP_LANG_RUST, hyp_rust_build_cross_registry, "i32", "String", "Option<Thing>", "()"};
static const OrderedSignatureLanguage JAVA_LANGUAGE = {
    "java", HYP_LANG_JAVA, build_java_cross_registry_for_contract,
    "int",  "String",      "java.util.List<?>",
    "void"};
static const OrderedSignatureLanguage KOTLIN_LANGUAGE = {
    "kotlin", HYP_LANG_KOTLIN, build_kotlin_cross_registry_for_contract, "Int", "String",
    "Thing?", "Unit"};
static const OrderedSignatureLanguage PHP_LANGUAGE = {
    "php", HYP_LANG_PHP, build_php_cross_registry_for_contract, "int", "string", "?Thing", "void"};

static void add_fixture_type(HYPTypeRegistry *registry, const char *qualified_name,
                             const char *short_name) {
    HYPRegisteredType type = {0};
    type.qualified_name = qualified_name;
    type.short_name = short_name;
    hyp_registry_add_type(registry, type);
}

static void add_php_seeded_method(HYPArena *arena, HYPTypeRegistry *registry,
                                  const char *qualified_name, const char *short_name,
                                  const char *receiver_type, const char *thing_qn,
                                  const HYPType *return_type) {
    const char *param_names[] = {"first", "middle", "last", NULL};
    const HYPType *param_types[] = {hyp_type_builtin(arena, "int"), hyp_type_named(arena, thing_qn),
                                    hyp_type_builtin(arena, "int"), NULL};
    const HYPType *return_types[] = {return_type, NULL};
    HYPRegisteredFunc function = {0};
    function.qualified_name = qualified_name;
    function.short_name = short_name;
    function.receiver_type = receiver_type;
    function.min_params = 3;
    function.signature = hyp_type_func(arena, param_names, param_types, return_types);
    hyp_registry_add_func(registry, function);
}

static void add_csharp_seeded_method(HYPArena *arena, HYPTypeRegistry *registry,
                                     const char *qualified_name, const char *short_name,
                                     const char *receiver_type, const char *thing_qn) {
    const HYPType *param_types[] = {hyp_type_builtin(arena, "int"), hyp_type_named(arena, thing_qn),
                                    hyp_type_builtin(arena, "int"), NULL};
    const HYPType *return_types[] = {hyp_type_unknown(), NULL};
    HYPRegisteredFunc function = {0};
    function.qualified_name = qualified_name;
    function.short_name = short_name;
    function.receiver_type = receiver_type;
    function.min_params = 3;
    function.signature = hyp_type_func(arena, NULL, param_types, return_types);
    hyp_registry_add_func(registry, function);
}

typedef struct {
    int call_count;
    const char *seen[8];
} SignatureParserProbe;

static const HYPType *parse_signature_probe_type(HYPArena *arena, const char *text,
                                                 void *parser_ctx) {
    SignatureParserProbe *probe = (SignatureParserProbe *)parser_ctx;
    probe->seen[probe->call_count++] = text;
    if (strcmp(text, "parser-null") == 0)
        return NULL;
    return hyp_type_builtin(arena, text);
}

TEST(repro_lsp_signature_param_materializer_contract) {
    HYPArena arena;
    hyp_arena_init(&arena);

    SignatureParserProbe probe = {0};
    const char *ordered_texts[] = {"int", "?", "string", "outside-count"};
    const HYPType **ordered = hyp_type_materialize_signature_params(
        &arena, ordered_texts, 3, parse_signature_probe_type, &probe);
    ASSERT_NOT_NULL(ordered);
    ASSERT_EQ(probe.call_count, 2);
    ASSERT_STR_EQ(probe.seen[0], "int");
    ASSERT_STR_EQ(probe.seen[1], "string");
    ASSERT_EQ(ordered[0]->kind, HYP_TYPE_BUILTIN);
    ASSERT_STR_EQ(ordered[0]->data.builtin.name, "int");
    ASSERT_TRUE(hyp_type_is_unknown(ordered[1]));
    ASSERT_EQ(ordered[2]->kind, HYP_TYPE_BUILTIN);
    ASSERT_STR_EQ(ordered[2]->data.builtin.name, "string");
    ASSERT_NULL(ordered[3]);

    memset(&probe, 0, sizeof(probe));
    const char *duplicate_texts[] = {"int", "int"};
    const HYPType **duplicates = hyp_type_materialize_signature_params(
        &arena, duplicate_texts, 2, parse_signature_probe_type, &probe);
    ASSERT_NOT_NULL(duplicates);
    ASSERT_EQ(probe.call_count, 2);
    ASSERT_STR_EQ(duplicates[0]->data.builtin.name, "int");
    ASSERT_STR_EQ(duplicates[1]->data.builtin.name, "int");
    ASSERT_NULL(duplicates[2]);

    memset(&probe, 0, sizeof(probe));
    const char *trailing_texts[] = {"int", "string", "?"};
    const HYPType **trailing = hyp_type_materialize_signature_params(
        &arena, trailing_texts, 3, parse_signature_probe_type, &probe);
    ASSERT_NOT_NULL(trailing);
    ASSERT_EQ(probe.call_count, 2);
    ASSERT_STR_EQ(trailing[0]->data.builtin.name, "int");
    ASSERT_STR_EQ(trailing[1]->data.builtin.name, "string");
    ASSERT_TRUE(hyp_type_is_unknown(trailing[2]));
    ASSERT_NULL(trailing[3]);

    memset(&probe, 0, sizeof(probe));
    const char *sentinel_texts[] = {"?", "?Foo", "Foo?", "parser-null", "", NULL};
    const HYPType **sentinels = hyp_type_materialize_signature_params(
        &arena, sentinel_texts, 6, parse_signature_probe_type, &probe);
    ASSERT_NOT_NULL(sentinels);
    ASSERT_EQ(probe.call_count, 3);
    ASSERT_STR_EQ(probe.seen[0], "?Foo");
    ASSERT_STR_EQ(probe.seen[1], "Foo?");
    ASSERT_STR_EQ(probe.seen[2], "parser-null");
    ASSERT_TRUE(hyp_type_is_unknown(sentinels[0]));
    ASSERT_EQ(sentinels[1]->kind, HYP_TYPE_BUILTIN);
    ASSERT_STR_EQ(sentinels[1]->data.builtin.name, "?Foo");
    ASSERT_EQ(sentinels[2]->kind, HYP_TYPE_BUILTIN);
    ASSERT_STR_EQ(sentinels[2]->data.builtin.name, "Foo?");
    ASSERT_TRUE(hyp_type_is_unknown(sentinels[3]));
    ASSERT_TRUE(hyp_type_is_unknown(sentinels[4]));
    ASSERT_TRUE(hyp_type_is_unknown(sentinels[5]));
    ASSERT_NULL(sentinels[6]);

    memset(&probe, 0, sizeof(probe));
    const HYPType **missing_texts =
        hyp_type_materialize_signature_params(&arena, NULL, 3, parse_signature_probe_type, &probe);
    ASSERT_NOT_NULL(missing_texts);
    ASSERT_EQ(probe.call_count, 0);
    ASSERT_TRUE(hyp_type_is_unknown(missing_texts[0]));
    ASSERT_TRUE(hyp_type_is_unknown(missing_texts[1]));
    ASSERT_TRUE(hyp_type_is_unknown(missing_texts[2]));
    ASSERT_NULL(missing_texts[3]);
    ASSERT_NULL(hyp_type_materialize_signature_params(&arena, ordered_texts, 0,
                                                      parse_signature_probe_type, &probe));

    hyp_arena_destroy(&arena);
    PASS();
}

TEST(repro_lsp_func_replace_returns_contract) {
    HYPArena arena;
    hyp_arena_init(&arena);

    const HYPType *integer = hyp_type_builtin(&arena, "int");
    const HYPType *string = hyp_type_builtin(&arena, "string");
    const HYPType *old_return = hyp_type_builtin(&arena, "old-return");
    const HYPType *new_return = hyp_type_builtin(&arena, "new-return");
    const char *param_names[] = {"first", "middle", "last", NULL};
    const HYPType *param_types[] = {integer, hyp_type_unknown(), string, NULL};
    const HYPType *old_returns[] = {old_return, NULL};
    const HYPType *old_signature = hyp_type_func(&arena, param_names, param_types, old_returns);
    ASSERT_NOT_NULL(old_signature);
    ASSERT_EQ(old_signature->kind, HYP_TYPE_FUNC);

    const HYPType *new_returns[] = {new_return, NULL};
    const HYPType *replaced = hyp_type_func_replace_returns(&arena, old_signature, new_returns);
    ASSERT_NOT_NULL(replaced);
    ASSERT_EQ(replaced->kind, HYP_TYPE_FUNC);
    ASSERT(replaced != old_signature);
    ASSERT_STR_EQ(replaced->data.func.param_names[0], "first");
    ASSERT_STR_EQ(replaced->data.func.param_names[1], "middle");
    ASSERT_STR_EQ(replaced->data.func.param_names[2], "last");
    ASSERT_NULL(replaced->data.func.param_names[3]);
    ASSERT(replaced->data.func.param_types[0] == integer);
    ASSERT_TRUE(hyp_type_is_unknown(replaced->data.func.param_types[1]));
    ASSERT(replaced->data.func.param_types[2] == string);
    ASSERT_NULL(replaced->data.func.param_types[3]);
    ASSERT(replaced->data.func.return_types[0] == new_return);
    ASSERT_NULL(replaced->data.func.return_types[1]);

    /* The source signature remains untouched. */
    ASSERT(old_signature->data.func.return_types[0] == old_return);
    ASSERT_NULL(old_signature->data.func.return_types[1]);
    ASSERT_STR_EQ(old_signature->data.func.param_names[1], "middle");
    ASSERT_TRUE(hyp_type_is_unknown(old_signature->data.func.param_types[1]));

    hyp_arena_destroy(&arena);
    PASS();
}

static int run_c_explicit_template_value_signature_contract(const char *source,
                                                            const char *registered_qn,
                                                            const char *function_node_kind) {
    TSTree *tree = parse_lsp_fixture(HYP_LANG_CPP, source);
    ASSERT_NOT_NULL(tree);

    TSNode call = find_first_lsp_node_of_kind(ts_tree_root_node(tree), "call_expression");
    ASSERT_FALSE(ts_node_is_null(call));
    TSNode function = ts_node_child_by_field_name(call, "function", 8);
    ASSERT_FALSE(ts_node_is_null(function));
    ASSERT_STR_EQ(ts_node_type(function), function_node_kind);

    HYPArena arena;
    hyp_arena_init(&arena);
    HYPTypeRegistry registry;
    hyp_registry_init(&registry, &arena);

    const char *param_names[] = {"value", "enabled", NULL};
    const HYPType *param_types[] = {hyp_type_builtin(&arena, "long"),
                                    hyp_type_builtin(&arena, "bool"), NULL};
    const HYPType *return_types[] = {hyp_type_type_param(&arena, "T"), NULL};
    const char *type_params[] = {"T", NULL};
    HYPRegisteredFunc registered = {0};
    registered.qualified_name = registered_qn;
    registered.short_name = "materialize";
    registered.signature = hyp_type_func(&arena, param_names, param_types, return_types);
    registered.type_param_names = type_params;
    registered.min_params = 2;
    hyp_registry_add_func(&registry, registered);

    HYPResolvedCallArray out = {0};
    CLSPContext ctx;
    c_lsp_init(&ctx, &arena, source, (int)strlen(source), &registry, NULL,
               /*cpp_mode=*/true, &out);
    const HYPType *value_type = c_eval_expr_type(&ctx, function);

    ASSERT_NOT_NULL(value_type);
    ASSERT_EQ(value_type->kind, HYP_TYPE_FUNC);
    ASSERT_NOT_NULL(value_type->data.func.return_types);
    ASSERT_NOT_NULL(value_type->data.func.return_types[0]);
    ASSERT_EQ(value_type->data.func.return_types[0]->kind, HYP_TYPE_BUILTIN);
    ASSERT_STR_EQ(value_type->data.func.return_types[0]->data.builtin.name, "int");

    ASSERT_NOT_NULL(value_type->data.func.param_names);
    ASSERT_STR_EQ(value_type->data.func.param_names[0], "value");
    ASSERT_STR_EQ(value_type->data.func.param_names[1], "enabled");
    ASSERT_NULL(value_type->data.func.param_names[2]);
    ASSERT_NOT_NULL(value_type->data.func.param_types);
    ASSERT_EQ(value_type->data.func.param_types[0]->kind, HYP_TYPE_BUILTIN);
    ASSERT_STR_EQ(value_type->data.func.param_types[0]->data.builtin.name, "long");
    ASSERT_EQ(value_type->data.func.param_types[1]->kind, HYP_TYPE_BUILTIN);
    ASSERT_STR_EQ(value_type->data.func.param_types[1]->data.builtin.name, "bool");
    ASSERT_NULL(value_type->data.func.param_types[2]);

    ts_tree_delete(tree);
    hyp_arena_destroy(&arena);
    return 0;
}

TEST(repro_lsp_c_unqualified_explicit_template_value_preserves_signature) {
    return run_c_explicit_template_value_signature_contract(
        "void caller(){ materialize<int>(1L, true); }", "materialize", "template_function");
}

TEST(repro_lsp_c_qualified_explicit_template_value_preserves_signature) {
    return run_c_explicit_template_value_signature_contract(
        "void caller(){ ns::materialize<int>(1L, true); }", "ns.materialize",
        "qualified_identifier");
}

TEST(repro_lsp_ordered_signatures_c_control) {
    ASSERT_EQ(run_typed_language_contract(&C_LANGUAGE), 0);
    PASS();
}

TEST(repro_lsp_ordered_signatures_cpp_control) {
    ASSERT_EQ(run_typed_language_contract(&CPP_LANGUAGE), 0);
    PASS();
}

TEST(repro_lsp_ordered_signatures_cuda_control) {
    ASSERT_EQ(run_typed_language_contract(&CUDA_LANGUAGE), 0);
    PASS();
}

TEST(repro_lsp_ordered_signatures_go) {
    ASSERT_EQ(run_typed_language_contract(&GO_LANGUAGE), 0);
    PASS();
}

TEST(repro_lsp_ordered_signatures_javascript) {
    ASSERT_EQ(run_javascript_unknown_contract(&JS_LANGUAGE), 0);
    PASS();
}

TEST(repro_lsp_ordered_signatures_typescript) {
    ASSERT_EQ(run_typed_language_contract(&TS_LANGUAGE), 0);
    PASS();
}

TEST(repro_lsp_ordered_signatures_tsx) {
    ASSERT_EQ(run_typed_language_contract(&TSX_LANGUAGE), 0);
    PASS();
}

TEST(repro_lsp_ordered_signatures_python) {
    ASSERT_EQ(run_typed_language_contract(&PYTHON_LANGUAGE), 0);
    PASS();
}

TEST(repro_lsp_ordered_signatures_csharp) {
    ASSERT_EQ(run_typed_language_contract(&CSHARP_LANGUAGE), 0);
    PASS();
}

TEST(repro_lsp_ordered_signatures_csharp_cross_return) {
    HYPArena arena;
    hyp_arena_init(&arena);

    HYPLSPDef def;
    init_function_def(&def, &arena, &CSHARP_LANGUAGE, "cross_return", NULL, 0);
    def.return_types = "System.String";
    HYPTypeRegistry *registry = hyp_cs_build_cross_registry(&arena, &def, 1);
    int failures = registry ? check_single_return(registry, def.qualified_name,
                                                  "csharp-cross-return", "String")
                            : 1;

    hyp_arena_destroy(&arena);
    ASSERT_EQ(failures, 0);
    PASS();
}

TEST(repro_lsp_ordered_signatures_rust) {
    ASSERT_EQ(run_typed_language_contract(&RUST_LANGUAGE), 0);
    PASS();
}

TEST(repro_lsp_ordered_signatures_java) {
    ASSERT_EQ(run_typed_language_contract(&JAVA_LANGUAGE), 0);
    PASS();
}

TEST(repro_lsp_ordered_signatures_kotlin) {
    ASSERT_EQ(run_typed_language_contract(&KOTLIN_LANGUAGE), 0);
    PASS();
}

TEST(repro_lsp_ordered_signatures_php) {
    ASSERT_EQ(run_typed_language_contract(&PHP_LANGUAGE), 0);
    PASS();
}

TEST(repro_lsp_ordered_signatures_csharp_ast_return_refinement) {
    const char *module_qn = "hyp.ordered.csharp.local";
    const char *subject_qn = "hyp.ordered.csharp.local.Subject";
    const char *thing_qn = "hyp.ordered.csharp.local.Thing";
    const char *result_qn = "hyp.ordered.csharp.local.Result";
    const char *method_qn = "hyp.ordered.csharp.local.Subject.method";
    const char *source =
        "class Thing {}\n"
        "class Result {}\n"
        "class Subject {\n"
        "  Result method(int first, Thing middle, int last) { return new Result(); }\n"
        "}\n";
    TSTree *tree = parse_lsp_fixture(HYP_LANG_CSHARP, source);
    ASSERT_NOT_NULL(tree);
    TSNode root = ts_tree_root_node(tree);
    ASSERT_FALSE(ts_node_has_error(root));

    HYPArena arena;
    hyp_arena_init(&arena);
    HYPTypeRegistry registry;
    hyp_registry_init(&registry, &arena);
    add_fixture_type(&registry, thing_qn, "Thing");
    add_fixture_type(&registry, result_qn, "Result");
    add_fixture_type(&registry, subject_qn, "Subject");
    add_csharp_seeded_method(&arena, &registry, method_qn, "method", subject_qn, thing_qn);

    HYPResolvedCallArray out = {0};
    CSLSPContext ctx;
    cs_lsp_init(&ctx, &arena, source, (int)strlen(source), &registry, module_qn, &out);
    hyp_cs_refine_ast_return_types(&ctx, &registry, root);

    const char *expected[] = {"int", "Thing", "int"};
    int failures =
        check_signature(&registry, method_qn, "csharp-ast-return-refinement", expected, 3);
    failures += check_single_return(&registry, method_qn, "csharp-ast-return-refinement", "Result");

    ts_tree_delete(tree);
    hyp_arena_destroy(&arena);
    ASSERT_EQ(failures, 0);
    PASS();
}

TEST(repro_lsp_ordered_signatures_rust_ast_return_refinement) {
    const char *module_qn = "hyp.ordered.rust.local";
    const char *thing_qn = "hyp.ordered.rust.local.Thing";
    const char *result_qn = "hyp.ordered.rust.local.Result";
    const char *subject_qn = "hyp.ordered.rust.local.Subject";
    const char *top_qn = "hyp.ordered.rust.local.top";
    const char *method_qn = "hyp.ordered.rust.local.Subject.method";
    const char *source =
        "struct Thing;\n"
        "struct Result;\n"
        "struct Subject;\n"
        "fn top(first: i32, middle: Thing, last: i32) -> Result { Result }\n"
        "impl Subject {\n"
        "  fn method(&self, first: i32, middle: Thing, last: i32) -> Result { Result }\n"
        "}\n";
    TSTree *tree = parse_lsp_fixture(HYP_LANG_RUST, source);
    ASSERT_NOT_NULL(tree);
    TSNode root = ts_tree_root_node(tree);
    ASSERT_FALSE(ts_node_has_error(root));

    const char *params[] = {"i32", "Thing", "i32", NULL};
    HYPDefinition defs[5] = {0};
    defs[0].name = "Thing";
    defs[0].qualified_name = thing_qn;
    defs[0].label = "Struct";
    defs[1].name = "Result";
    defs[1].qualified_name = result_qn;
    defs[1].label = "Struct";
    defs[2].name = "Subject";
    defs[2].qualified_name = subject_qn;
    defs[2].label = "Struct";
    defs[3].name = "top";
    defs[3].qualified_name = top_qn;
    defs[3].label = "Function";
    defs[3].param_types = params;
    defs[3].signature_param_types = params;
    defs[3].signature_param_count = 3;
    defs[4].name = "method";
    defs[4].qualified_name = method_qn;
    defs[4].label = "Method";
    defs[4].parent_class = subject_qn;
    defs[4].param_types = params;
    defs[4].signature_param_types = params;
    defs[4].signature_param_count = 3;

    HYPFileResult result = {0};
    result.module_qn = module_qn;
    result.defs.items = defs;
    result.defs.count = 5;

    HYPArena arena;
    hyp_arena_init(&arena);
    HYPTypeRegistry registry;
    hyp_rust_build_local_registry(&arena, &registry, &result, module_qn, root, source);

    const char *expected[] = {"i32", "Thing", "i32"};
    int failures = check_signature(&registry, top_qn, "rust-top-return-refinement", expected, 3);
    failures += check_single_return(&registry, top_qn, "rust-top-return-refinement", "Result");
    failures += check_signature(&registry, method_qn, "rust-method-return-refinement", expected, 3);
    failures +=
        check_single_return(&registry, method_qn, "rust-method-return-refinement", "Result");

    ts_tree_delete(tree);
    hyp_arena_destroy(&arena);
    ASSERT_EQ(failures, 0);
    PASS();
}

TEST(repro_lsp_ordered_signatures_kotlin_local_registry) {
    const char *source =
        "class Thing {}\n"
        "class Result {}\n"
        "class Holder {\n"
        "  fun method(first: Int, middle: Thing?, last: Int): Result { return Result() }\n"
        "}\n"
        "fun top(first: Int, middle: Thing?, last: Int): Result { return Result() }\n";
    TSTree *tree = parse_lsp_fixture(HYP_LANG_KOTLIN, source);
    ASSERT_NOT_NULL(tree);
    TSNode root = ts_tree_root_node(tree);
    ASSERT_FALSE(ts_node_has_error(root));

    HYPArena arena;
    hyp_arena_init(&arena);
    HYPTypeRegistry registry;
    hyp_registry_init(&registry, &arena);
    HYPResolvedCallArray out = {0};
    KotlinLSPContext ctx;
    kotlin_lsp_init(&ctx, &arena, source, (int)strlen(source), &registry,
                    "hyp.ordered.kotlin.local", "hyp.ordered.kotlin.local", "hyp", "Local.kt",
                    &out);
    kotlin_lsp_process_file(&ctx, root);

    /* Kotlin intentionally erases nullability in the registry model, but
     * Thing? must still become the real named Thing rather than UNKNOWN. */
    const char *expected[] = {"Int", "Thing", "Int"};
    int failures = check_signature(&registry, "hyp.ordered.kotlin.local.Holder.method",
                                   "kotlin-local-method", expected, 3);
    failures += check_single_return(&registry, "hyp.ordered.kotlin.local.Holder.method",
                                    "kotlin-local-method", "Result");
    failures +=
        check_signature(&registry, "hyp.ordered.kotlin.local.top", "kotlin-local-top", expected, 3);
    failures += check_single_return(&registry, "hyp.ordered.kotlin.local.top", "kotlin-local-top",
                                    "Result");

    ts_tree_delete(tree);
    hyp_arena_destroy(&arena);
    ASSERT_EQ(failures, 0);
    PASS();
}

TEST(repro_lsp_ordered_signatures_php_return_refinement) {
    const char *module_qn = "hyp.ordered.php.local";
    const char *subject_qn = "hyp.ordered.php.local.Subject";
    const char *thing_qn = "hyp.ordered.php.local.Thing";
    const char *result_qn = "hyp.ordered.php.local.Result";
    const char *source =
        "<?php\n"
        "class Thing {}\n"
        "class Result {}\n"
        "class Subject {\n"
        "  public function declared(int $first, ?Thing $middle, int $last): Result {\n"
        "    return new Result();\n"
        "  }\n"
        "  /** @return Result */\n"
        "  public function documented(int $first, ?Thing $middle, int $last) {\n"
        "    return new Result();\n"
        "  }\n"
        "}\n";
    TSTree *tree = parse_lsp_fixture(HYP_LANG_PHP, source);
    ASSERT_NOT_NULL(tree);
    TSNode root = ts_tree_root_node(tree);
    ASSERT_FALSE(ts_node_has_error(root));

    HYPArena arena;
    hyp_arena_init(&arena);
    HYPTypeRegistry registry;
    hyp_registry_init(&registry, &arena);
    add_fixture_type(&registry, thing_qn, "Thing");
    add_fixture_type(&registry, result_qn, "Result");
    add_fixture_type(&registry, subject_qn, "Subject");
    /* Seed the state expected immediately before AST return refinement. This
     * isolates whether a declared/PHPDoc return rewrite preserves parameters;
     * it does not depend on the still-RED PHP registration materializer. */
    add_php_seeded_method(&arena, &registry, "hyp.ordered.php.local.Subject.declared", "declared",
                          subject_qn, thing_qn, hyp_type_unknown());
    add_php_seeded_method(&arena, &registry, "hyp.ordered.php.local.Subject.documented",
                          "documented", subject_qn, thing_qn, hyp_type_unknown());

    HYPResolvedCallArray out = {0};
    PHPLSPContext ctx;
    php_lsp_init(&ctx, &arena, source, (int)strlen(source), &registry, module_qn, &out);
    hyp_php_refine_lsp_registry(&ctx, &registry, root);

    const char *expected[] = {"int", "Thing", "int"};
    int failures = check_signature(&registry, "hyp.ordered.php.local.Subject.declared",
                                   "php-declared-return-refinement", expected, 3);
    failures += check_single_return(&registry, "hyp.ordered.php.local.Subject.declared",
                                    "php-declared-return-refinement", "Result");
    failures += check_signature(&registry, "hyp.ordered.php.local.Subject.documented",
                                "phpdoc-return-refinement", expected, 3);
    failures += check_single_return(&registry, "hyp.ordered.php.local.Subject.documented",
                                    "phpdoc-return-refinement", "Result");

    ts_tree_delete(tree);
    hyp_arena_destroy(&arena);
    ASSERT_EQ(failures, 0);
    PASS();
}

TEST(repro_lsp_ordered_signatures_php_trait_self_refinement) {
    const char *module_qn = "hyp.ordered.php.trait";
    const char *thing_qn = "hyp.ordered.php.trait.Thing";
    const char *trait_qn = "hyp.ordered.php.trait.Fluent";
    const char *consumer_qn = "hyp.ordered.php.trait.Consumer";
    const char *source =
        "<?php\n"
        "class Thing {}\n"
        "trait Fluent {\n"
        "  public function tap(int $first, ?Thing $middle, int $last) { return $this; }\n"
        "}\n"
        "class Consumer { use Fluent; }\n";
    TSTree *tree = parse_lsp_fixture(HYP_LANG_PHP, source);
    ASSERT_NOT_NULL(tree);
    TSNode root = ts_tree_root_node(tree);
    ASSERT_FALSE(ts_node_has_error(root));

    HYPArena arena;
    hyp_arena_init(&arena);
    HYPTypeRegistry registry;
    hyp_registry_init(&registry, &arena);
    add_fixture_type(&registry, thing_qn, "Thing");
    add_fixture_type(&registry, trait_qn, "Fluent");
    add_fixture_type(&registry, consumer_qn, "Consumer");
    /* No AST return annotation is deliberate: only trait self-substitution
     * may rebuild this seeded signature, so parameter loss has one cause. */
    add_php_seeded_method(&arena, &registry, "hyp.ordered.php.trait.Fluent.tap", "tap", trait_qn,
                          thing_qn, hyp_type_named(&arena, trait_qn));

    HYPResolvedCallArray out = {0};
    PHPLSPContext ctx;
    php_lsp_init(&ctx, &arena, source, (int)strlen(source), &registry, module_qn, &out);
    hyp_php_refine_lsp_registry(&ctx, &registry, root);

    const char *expected[] = {"int", "Thing", "int"};
    int failures = check_signature(&registry, "hyp.ordered.php.trait.Consumer.tap",
                                   "php-trait-self-refinement", expected, 3);
    failures += check_single_return(&registry, "hyp.ordered.php.trait.Consumer.tap",
                                    "php-trait-self-refinement", "Consumer");

    ts_tree_delete(tree);
    hyp_arena_destroy(&arena);
    ASSERT_EQ(failures, 0);
    PASS();
}

SUITE(repro_lsp_ordered_signatures) {
    RUN_TEST(repro_lsp_signature_param_materializer_contract);
    RUN_TEST(repro_lsp_func_replace_returns_contract);
    RUN_TEST(repro_lsp_c_unqualified_explicit_template_value_preserves_signature);
    RUN_TEST(repro_lsp_c_qualified_explicit_template_value_preserves_signature);
    RUN_TEST(repro_lsp_ordered_signatures_c_control);
    RUN_TEST(repro_lsp_ordered_signatures_cpp_control);
    RUN_TEST(repro_lsp_ordered_signatures_cuda_control);
    RUN_TEST(repro_lsp_ordered_signatures_go);
    RUN_TEST(repro_lsp_ordered_signatures_javascript);
    RUN_TEST(repro_lsp_ordered_signatures_typescript);
    RUN_TEST(repro_lsp_ordered_signatures_tsx);
    RUN_TEST(repro_lsp_ordered_signatures_python);
    RUN_TEST(repro_lsp_ordered_signatures_csharp);
    RUN_TEST(repro_lsp_ordered_signatures_csharp_cross_return);
    RUN_TEST(repro_lsp_ordered_signatures_rust);
    RUN_TEST(repro_lsp_ordered_signatures_java);
    RUN_TEST(repro_lsp_ordered_signatures_kotlin);
    RUN_TEST(repro_lsp_ordered_signatures_php);
    RUN_TEST(repro_lsp_ordered_signatures_csharp_ast_return_refinement);
    RUN_TEST(repro_lsp_ordered_signatures_rust_ast_return_refinement);
    RUN_TEST(repro_lsp_ordered_signatures_kotlin_local_registry);
    RUN_TEST(repro_lsp_ordered_signatures_php_return_refinement);
    RUN_TEST(repro_lsp_ordered_signatures_php_trait_self_refinement);
}
