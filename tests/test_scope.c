/*
 * test_scope.c — Tests for the chunked linked-frame HYPScope.
 *
 * Phase 0 of Python LSP integration: replaces the legacy fixed 64-binding
 * array with growable per-scope chunks. Tests verify dynamic growth,
 * binding-shadowing semantics, and the parent-chain lookup contract that
 * Go and C/C++ LSP implementations rely on.
 */
#include "test_framework.h"
#include "hyp.h"
#include "lsp/scope.h"
#include "lsp/type_rep.h"

/* Build a NAMED type for a fixture string. Arena-allocated. */
static const HYPType *named_t(HYPArena *a, const char *qn) {
    return hyp_type_named(a, qn);
}

/* ── Basic API ─────────────────────────────────────────────────── */

TEST(scope_push_returns_distinct_scope) {
    HYPArena a;
    hyp_arena_init(&a);
    HYPScope *root = hyp_scope_push(&a, NULL);
    HYPScope *child = hyp_scope_push(&a, root);
    ASSERT_NOT_NULL(root);
    ASSERT_NOT_NULL(child);
    ASSERT(child != root);
    ASSERT(child->parent == root);
    hyp_arena_destroy(&a);
    PASS();
}

TEST(scope_pop_returns_parent) {
    HYPArena a;
    hyp_arena_init(&a);
    HYPScope *root = hyp_scope_push(&a, NULL);
    HYPScope *child = hyp_scope_push(&a, root);
    ASSERT(hyp_scope_pop(child) == root);
    ASSERT(hyp_scope_pop(root) == NULL);
    hyp_arena_destroy(&a);
    PASS();
}

TEST(scope_lookup_unbound_returns_unknown) {
    HYPArena a;
    hyp_arena_init(&a);
    HYPScope *s = hyp_scope_push(&a, NULL);
    const HYPType *t = hyp_scope_lookup(s, "missing");
    ASSERT_NOT_NULL(t);
    ASSERT(hyp_type_is_unknown(t));
    hyp_arena_destroy(&a);
    PASS();
}

/* ── Binding semantics ─────────────────────────────────────────── */

TEST(scope_bind_then_lookup) {
    HYPArena a;
    hyp_arena_init(&a);
    HYPScope *s = hyp_scope_push(&a, NULL);
    hyp_scope_bind(s, "x", named_t(&a, "int"));
    const HYPType *t = hyp_scope_lookup(s, "x");
    ASSERT_NOT_NULL(t);
    ASSERT(t->kind == HYP_TYPE_NAMED);
    ASSERT_STR_EQ(t->data.named.qualified_name, "int");
    hyp_arena_destroy(&a);
    PASS();
}

TEST(scope_rebind_overwrites_in_place) {
    HYPArena a;
    hyp_arena_init(&a);
    HYPScope *s = hyp_scope_push(&a, NULL);
    hyp_scope_bind(s, "x", named_t(&a, "int"));
    hyp_scope_bind(s, "x", named_t(&a, "string"));
    const HYPType *t = hyp_scope_lookup(s, "x");
    ASSERT_STR_EQ(t->data.named.qualified_name, "string");
    hyp_arena_destroy(&a);
    PASS();
}

TEST(scope_lookup_walks_parent_chain) {
    HYPArena a;
    hyp_arena_init(&a);
    HYPScope *root = hyp_scope_push(&a, NULL);
    HYPScope *child = hyp_scope_push(&a, root);
    hyp_scope_bind(root, "outer", named_t(&a, "Outer"));
    hyp_scope_bind(child, "inner", named_t(&a, "Inner"));
    const HYPType *outer_in_child = hyp_scope_lookup(child, "outer");
    const HYPType *inner_in_root = hyp_scope_lookup(root, "inner");
    ASSERT_STR_EQ(outer_in_child->data.named.qualified_name, "Outer");
    ASSERT(hyp_type_is_unknown(inner_in_root));
    hyp_arena_destroy(&a);
    PASS();
}

TEST(scope_child_shadows_parent) {
    HYPArena a;
    hyp_arena_init(&a);
    HYPScope *root = hyp_scope_push(&a, NULL);
    HYPScope *child = hyp_scope_push(&a, root);
    hyp_scope_bind(root, "x", named_t(&a, "ParentInt"));
    hyp_scope_bind(child, "x", named_t(&a, "ChildStr"));
    const HYPType *t = hyp_scope_lookup(child, "x");
    ASSERT_STR_EQ(t->data.named.qualified_name, "ChildStr");
    hyp_arena_destroy(&a);
    PASS();
}

TEST(scope_callable_identity_follows_nearest_binding) {
    HYPArena a;
    hyp_arena_init(&a);
    HYPScope *root = hyp_scope_push(&a, NULL);
    HYPScope *child = hyp_scope_push(&a, root);
    const HYPType *callback_type = named_t(&a, "Callback");
    hyp_scope_bind_callable(root, "callback", callback_type, "pkg.actual");
    ASSERT_TRUE(hyp_scope_contains(child, "callback"));
    ASSERT_STR_EQ(hyp_scope_lookup_callable(child, "callback"), "pkg.actual");

    hyp_scope_bind(child, "callback", named_t(&a, "int"));
    ASSERT_NULL(hyp_scope_lookup_callable(child, "callback"));
    ASSERT_STR_EQ(hyp_scope_lookup_callable(root, "callback"), "pkg.actual");
    hyp_arena_destroy(&a);
    PASS();
}

TEST(scope_assignment_updates_or_clears_nearest_callable_only) {
    HYPArena a;
    hyp_arena_init(&a);
    HYPScope *root = hyp_scope_push(&a, NULL);
    HYPScope *child = hyp_scope_push(&a, root);
    const HYPType *callback_type = named_t(&a, "Callback");
    hyp_scope_bind_callable(root, "callback", callback_type, "pkg.actual");

    ASSERT_TRUE(hyp_scope_update_callable(child, "callback", "pkg.alternate"));
    ASSERT_STR_EQ(hyp_scope_lookup_callable(root, "callback"), "pkg.alternate");
    ASSERT(hyp_scope_lookup(root, "callback") == callback_type);
    ASSERT_TRUE(hyp_scope_update_callable(child, "callback", NULL));
    ASSERT_NULL(hyp_scope_lookup_callable(root, "callback"));
    ASSERT_FALSE(hyp_scope_update_callable(child, "missing", "pkg.never"));

    hyp_scope_bind_callable(root, "callback", callback_type, "pkg.restored");
    hyp_scope_bind_callable(child, "callback", callback_type, "pkg.child");
    ASSERT_TRUE(hyp_scope_update_callable(child, "callback", "pkg.child_new"));
    ASSERT_STR_EQ(hyp_scope_lookup_callable(child, "callback"), "pkg.child_new");
    ASSERT_STR_EQ(hyp_scope_lookup_callable(root, "callback"), "pkg.restored");
    hyp_arena_destroy(&a);
    PASS();
}

TEST(scope_checked_bind_reports_child_oom_despite_parent_name) {
    HYPArena a;
    hyp_arena_init(&a);
    HYPScope *root = hyp_scope_push(&a, NULL);
    HYPScope *ordinary_child = hyp_scope_push(&a, root);
    HYPScope *callable_child = hyp_scope_push(&a, root);
    const HYPType *callback_type = named_t(&a, "Callback");
    const HYPType *shadow_type = named_t(&a, "Shadow");
    hyp_scope_bind_callable(root, "callback", callback_type, "pkg.parent");

    int saved_nblocks = a.nblocks;
    size_t saved_used = a.used;
    a.nblocks = HYP_ARENA_MAX_BLOCKS;
    a.used = a.block_size;
    bool ordinary_bound = hyp_scope_bind_checked(ordinary_child, "callback", shadow_type);
    bool callable_bound = hyp_scope_bind_callable_checked(
        callable_child, "callback", shadow_type, "pkg.child");
    a.nblocks = saved_nblocks;
    a.used = saved_used;

    ASSERT_FALSE(ordinary_bound);
    ASSERT_FALSE(callable_bound);
    ASSERT_STR_EQ(hyp_scope_lookup_callable(ordinary_child, "callback"), "pkg.parent");
    ASSERT_STR_EQ(hyp_scope_lookup_callable(callable_child, "callback"), "pkg.parent");
    hyp_arena_destroy(&a);
    PASS();
}

/* ── Dynamic growth — the Phase 0 win ───────────────────────────── */

TEST(scope_dynamic_growth_300_bindings) {
    HYPArena a;
    hyp_arena_init(&a);
    HYPScope *s = hyp_scope_push(&a, NULL);
    char names[300][16];
    for (int i = 0; i < 300; i++) {
        snprintf(names[i], sizeof(names[0]), "v%d", i);
        char qn[32];
        snprintf(qn, sizeof(qn), "T%d", i);
        hyp_scope_bind(s, names[i], named_t(&a, qn));
    }
    /* All 300 bindings retrievable — old 64-cap would have dropped 236 */
    for (int i = 0; i < 300; i++) {
        const HYPType *t = hyp_scope_lookup(s, names[i]);
        ASSERT_NOT_NULL(t);
        ASSERT(t->kind == HYP_TYPE_NAMED);
        char expected[32];
        snprintf(expected, sizeof(expected), "T%d", i);
        ASSERT_STR_EQ(t->data.named.qualified_name, expected);
    }
    hyp_arena_destroy(&a);
    PASS();
}

TEST(scope_growth_chunk_boundary) {
    HYPArena a;
    hyp_arena_init(&a);
    HYPScope *s = hyp_scope_push(&a, NULL);
    char names[HYP_SCOPE_CHUNK_BINDINGS + 1][16];
    /* Fill exactly one chunk + spillover: forces second chunk allocation */
    for (int i = 0; i <= HYP_SCOPE_CHUNK_BINDINGS; i++) {
        snprintf(names[i], sizeof(names[0]), "n%d", i);
        char qn[16];
        snprintf(qn, sizeof(qn), "T%d", i);
        hyp_scope_bind(s, names[i], named_t(&a, qn));
    }
    /* First-chunk binding still retrievable */
    const HYPType *first = hyp_scope_lookup(s, "n0");
    ASSERT_STR_EQ(first->data.named.qualified_name, "T0");
    /* Spillover binding (would have fallen off legacy 64-cap if cap < 17) */
    char last_name[16];
    snprintf(last_name, sizeof(last_name), "n%d", HYP_SCOPE_CHUNK_BINDINGS);
    char last_qn[16];
    snprintf(last_qn, sizeof(last_qn), "T%d", HYP_SCOPE_CHUNK_BINDINGS);
    const HYPType *last = hyp_scope_lookup(s, last_name);
    ASSERT_STR_EQ(last->data.named.qualified_name, last_qn);
    hyp_arena_destroy(&a);
    PASS();
}

TEST(scope_rebind_in_old_chunk) {
    /* After many bindings have spilled into newer chunks, rebinding a name
     * from the original chunk must overwrite that binding in place rather
     * than create a duplicate in the head chunk. */
    HYPArena a;
    hyp_arena_init(&a);
    HYPScope *s = hyp_scope_push(&a, NULL);
    hyp_scope_bind(s, "early", named_t(&a, "EarlyV1"));
    char names[100][16];
    for (int i = 0; i < 100; i++) {
        snprintf(names[i], sizeof(names[0]), "fill%d", i);
        hyp_scope_bind(s, names[i], named_t(&a, "Filler"));
    }
    hyp_scope_bind(s, "early", named_t(&a, "EarlyV2"));
    const HYPType *t = hyp_scope_lookup(s, "early");
    ASSERT_STR_EQ(t->data.named.qualified_name, "EarlyV2");
    hyp_arena_destroy(&a);
    PASS();
}

/* ── Suite registration ────────────────────────────────────────── */

SUITE(scope) {
    RUN_TEST(scope_push_returns_distinct_scope);
    RUN_TEST(scope_pop_returns_parent);
    RUN_TEST(scope_lookup_unbound_returns_unknown);
    RUN_TEST(scope_bind_then_lookup);
    RUN_TEST(scope_rebind_overwrites_in_place);
    RUN_TEST(scope_lookup_walks_parent_chain);
    RUN_TEST(scope_child_shadows_parent);
    RUN_TEST(scope_callable_identity_follows_nearest_binding);
    RUN_TEST(scope_assignment_updates_or_clears_nearest_callable_only);
    RUN_TEST(scope_checked_bind_reports_child_oom_despite_parent_name);
    RUN_TEST(scope_dynamic_growth_300_bindings);
    RUN_TEST(scope_growth_chunk_boundary);
    RUN_TEST(scope_rebind_in_old_chunk);
}
