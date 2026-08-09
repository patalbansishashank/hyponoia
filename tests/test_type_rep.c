/*
 * test_type_rep.c — Phase 1 type-rep extensions for Python LSP.
 *
 * Covers the five new kinds (UNION, LITERAL, PROTOCOL, MODULE, CALLABLE),
 * structural equality, union normalization (flatten + dedup + collapse),
 * Optional[T] sugar, and protocol structural matching.
 */
#include "test_framework.h"
#include "hyp.h"
#include "lsp/type_rep.h"

/* ── UNION: flatten, dedup, collapse ──────────────────────────── */

TEST(typerep_union_two_distinct) {
    HYPArena a;
    hyp_arena_init(&a);
    const HYPType *i = hyp_type_builtin(&a, "int");
    const HYPType *s = hyp_type_builtin(&a, "str");
    const HYPType *m[2] = {i, s};
    const HYPType *u = hyp_type_union(&a, m, 2);
    ASSERT(hyp_type_is_union(u));
    ASSERT_EQ(u->data.union_type.count, 2);
    hyp_arena_destroy(&a);
    PASS();
}

TEST(typerep_union_dedupes_duplicates) {
    HYPArena a;
    hyp_arena_init(&a);
    const HYPType *i1 = hyp_type_builtin(&a, "int");
    const HYPType *i2 = hyp_type_builtin(&a, "int");
    const HYPType *m[2] = {i1, i2};
    const HYPType *u = hyp_type_union(&a, m, 2);
    /* dedup collapses to a single int type, not a union */
    ASSERT(!hyp_type_is_union(u));
    ASSERT_EQ(u->kind, HYP_TYPE_BUILTIN);
    ASSERT_STR_EQ(u->data.builtin.name, "int");
    hyp_arena_destroy(&a);
    PASS();
}

TEST(typerep_union_flattens_nested) {
    HYPArena a;
    hyp_arena_init(&a);
    const HYPType *i = hyp_type_builtin(&a, "int");
    const HYPType *s = hyp_type_builtin(&a, "str");
    const HYPType *b = hyp_type_builtin(&a, "bytes");
    const HYPType *inner_pair[2] = {i, s};
    const HYPType *inner = hyp_type_union(&a, inner_pair, 2);
    const HYPType *outer_pair[2] = {inner, b};
    const HYPType *u = hyp_type_union(&a, outer_pair, 2);
    /* flattening yields 3 distinct members */
    ASSERT(hyp_type_is_union(u));
    ASSERT_EQ(u->data.union_type.count, 3);
    hyp_arena_destroy(&a);
    PASS();
}

TEST(typerep_union_single_member_collapses) {
    HYPArena a;
    hyp_arena_init(&a);
    const HYPType *i = hyp_type_builtin(&a, "int");
    const HYPType *m[1] = {i};
    const HYPType *u = hyp_type_union(&a, m, 1);
    ASSERT(!hyp_type_is_union(u));
    ASSERT_EQ(u->kind, HYP_TYPE_BUILTIN);
    hyp_arena_destroy(&a);
    PASS();
}

TEST(typerep_union_empty_is_unknown) {
    HYPArena a;
    hyp_arena_init(&a);
    const HYPType *u = hyp_type_union(&a, NULL, 0);
    ASSERT(hyp_type_is_unknown(u));
    hyp_arena_destroy(&a);
    PASS();
}

TEST(typerep_optional_is_union_with_none) {
    HYPArena a;
    hyp_arena_init(&a);
    const HYPType *i = hyp_type_builtin(&a, "int");
    const HYPType *opt = hyp_type_optional(&a, i);
    ASSERT(hyp_type_is_union(opt));
    ASSERT_EQ(opt->data.union_type.count, 2);
    /* membership: int + None */
    bool has_int = false, has_none = false;
    for (int k = 0; k < opt->data.union_type.count; k++) {
        const HYPType *m = opt->data.union_type.members[k];
        if (m->kind == HYP_TYPE_BUILTIN && strcmp(m->data.builtin.name, "int") == 0)
            has_int = true;
        if (m->kind == HYP_TYPE_BUILTIN && strcmp(m->data.builtin.name, "None") == 0)
            has_none = true;
    }
    ASSERT(has_int);
    ASSERT(has_none);
    hyp_arena_destroy(&a);
    PASS();
}

/* ── LITERAL ───────────────────────────────────────────────────── */

TEST(typerep_literal_int_3) {
    HYPArena a;
    hyp_arena_init(&a);
    const HYPType *base = hyp_type_builtin(&a, "int");
    const HYPType *lit = hyp_type_literal(&a, base, "3");
    ASSERT_EQ(lit->kind, HYP_TYPE_LITERAL);
    ASSERT(hyp_type_equal(lit->data.literal.base, base));
    ASSERT_STR_EQ(lit->data.literal.literal_text, "3");
    hyp_arena_destroy(&a);
    PASS();
}

TEST(typerep_literal_equality_distinguishes_text) {
    HYPArena a;
    hyp_arena_init(&a);
    const HYPType *base = hyp_type_builtin(&a, "str");
    const HYPType *foo = hyp_type_literal(&a, base, "\"foo\"");
    const HYPType *bar = hyp_type_literal(&a, base, "\"bar\"");
    const HYPType *foo2 = hyp_type_literal(&a, base, "\"foo\"");
    ASSERT(!hyp_type_equal(foo, bar));
    ASSERT(hyp_type_equal(foo, foo2));
    hyp_arena_destroy(&a);
    PASS();
}

/* ── PROTOCOL ──────────────────────────────────────────────────── */

TEST(typerep_protocol_method_set) {
    HYPArena a;
    hyp_arena_init(&a);
    const char *methods[] = {"read", "close", NULL};
    const HYPType *proto = hyp_type_protocol(&a, "typing.IO", methods, NULL);
    ASSERT(hyp_type_is_protocol(proto));
    ASSERT_STR_EQ(proto->data.protocol.qualified_name, "typing.IO");
    ASSERT_NOT_NULL(proto->data.protocol.method_names);
    ASSERT_STR_EQ(proto->data.protocol.method_names[0], "read");
    ASSERT_STR_EQ(proto->data.protocol.method_names[1], "close");
    hyp_arena_destroy(&a);
    PASS();
}

TEST(typerep_protocol_satisfied_by_protocol_with_superset) {
    HYPArena a;
    hyp_arena_init(&a);
    const char *needed[] = {"read", "close", NULL};
    const char *have[] = {"read", "write", "close", "flush", NULL};
    const HYPType *proto = hyp_type_protocol(&a, "P1", needed, NULL);
    const HYPType *cand = hyp_type_protocol(&a, "P2", have, NULL);
    ASSERT(hyp_type_protocol_satisfied_by(proto, cand));
    hyp_arena_destroy(&a);
    PASS();
}

TEST(typerep_protocol_unsatisfied_when_method_missing) {
    HYPArena a;
    hyp_arena_init(&a);
    const char *needed[] = {"read", "close", NULL};
    const char *have[] = {"read", NULL};
    const HYPType *proto = hyp_type_protocol(&a, "P1", needed, NULL);
    const HYPType *cand = hyp_type_protocol(&a, "P2", have, NULL);
    ASSERT(!hyp_type_protocol_satisfied_by(proto, cand));
    hyp_arena_destroy(&a);
    PASS();
}

/* ── MODULE ────────────────────────────────────────────────────── */

TEST(typerep_module_carries_qn) {
    HYPArena a;
    hyp_arena_init(&a);
    const HYPType *m = hyp_type_module(&a, "os.path");
    ASSERT(hyp_type_is_module(m));
    ASSERT_STR_EQ(m->data.module.module_qn, "os.path");
    hyp_arena_destroy(&a);
    PASS();
}

TEST(typerep_module_equality_by_qn) {
    HYPArena a;
    hyp_arena_init(&a);
    const HYPType *a1 = hyp_type_module(&a, "os");
    const HYPType *a2 = hyp_type_module(&a, "os");
    const HYPType *b = hyp_type_module(&a, "sys");
    ASSERT(hyp_type_equal(a1, a2));
    ASSERT(!hyp_type_equal(a1, b));
    hyp_arena_destroy(&a);
    PASS();
}

/* ── CALLABLE ──────────────────────────────────────────────────── */

TEST(typerep_callable_with_args_and_return) {
    HYPArena a;
    hyp_arena_init(&a);
    const HYPType *i = hyp_type_builtin(&a, "int");
    const HYPType *s = hyp_type_builtin(&a, "str");
    const HYPType *params[2] = {i, s};
    const HYPType *c = hyp_type_callable(&a, params, 2, i);
    ASSERT_EQ(c->kind, HYP_TYPE_CALLABLE);
    ASSERT_EQ(c->data.callable.param_count, 2);
    ASSERT(hyp_type_equal(c->data.callable.return_type, i));
    hyp_arena_destroy(&a);
    PASS();
}

TEST(typerep_callable_elliptic_arity_minus_one) {
    /* Callable[..., R] — variadic in Python type-hint sense. */
    HYPArena a;
    hyp_arena_init(&a);
    const HYPType *r = hyp_type_builtin(&a, "int");
    const HYPType *c = hyp_type_callable(&a, NULL, -1, r);
    ASSERT_EQ(c->data.callable.param_count, -1);
    hyp_arena_destroy(&a);
    PASS();
}

TEST(typerep_callable_equality) {
    HYPArena a;
    hyp_arena_init(&a);
    const HYPType *i = hyp_type_builtin(&a, "int");
    const HYPType *s = hyp_type_builtin(&a, "str");
    const HYPType *p1[1] = {i};
    const HYPType *c1 = hyp_type_callable(&a, p1, 1, s);
    const HYPType *c2 = hyp_type_callable(&a, p1, 1, s);
    const HYPType *c3 = hyp_type_callable(&a, p1, 1, i); /* different return */
    ASSERT(hyp_type_equal(c1, c2));
    ASSERT(!hyp_type_equal(c1, c3));
    hyp_arena_destroy(&a);
    PASS();
}

/* ── SUBSTITUTION ──────────────────────────────────────────────── */

TEST(typerep_substitute_unbound_param_preserved) {
    HYPArena a;
    hyp_arena_init(&a);
    const HYPType *t = hyp_type_type_param(&a, "T");
    const char *params[] = {"T", NULL};
    const HYPType *args[] = {NULL, NULL};
    const HYPType *sub = hyp_type_substitute(&a, t, params, args);
    ASSERT(sub == t);
    hyp_arena_destroy(&a);
    PASS();
}

/* #427: type_args may be SHORTER than type_params — a class template
 * instantiated with fewer args than declared params (e.g. `Box<Widget>` for
 * `template<class T, class U, class V>`) or trailing default template args.
 * Matching a param whose index exceeds the args length must NOT index past the
 * args array's NULL terminator. Pre-fix, this read args[2] one element past the
 * 2-slot stack array (ASan stack-buffer-overflow) and returned a bogus HYPType*
 * that was later dereferenced -> SEGV in type_to_qn (c_lsp.c). The unbound
 * param must be preserved as-is. */
TEST(typerep_substitute_short_args_no_oob_issue427) {
    HYPArena a;
    hyp_arena_init(&a);
    const HYPType *t = hyp_type_type_param(&a, "V");                   /* the 3rd declared param */
    const char *params[] = {"T", "U", "V", NULL};                      /* 3 declared params */
    const HYPType *args[] = {hyp_type_type_param(&a, "Widget"), NULL}; /* 1 arg */
    const HYPType *sub = hyp_type_substitute(&a, t, params, args);
    ASSERT(sub == t); /* "V" (index 2) has no supplied arg -> preserved, no OOB */
    hyp_arena_destroy(&a);
    PASS();
}

/* A caller that forgets to NULL-terminate type_args (a stack array) makes the
 * bounded walk read uninitialized memory — and a garbage "pointer" within the
 * param count would be BOUND to a type param and woven into the resulting type
 * graph (bitcoin serialize.h: Using<Fmt>(v) with explicit args bound T to stack
 * garbage; the corrupt graph was dereferenced later -> SIGSEGV). Implausible
 * values (misaligned / null-page) must act as the terminator instead. */
TEST(typerep_substitute_rejects_garbage_args_entries) {
    HYPArena a;
    hyp_arena_init(&a);
    const HYPType *t =
        hyp_type_reference(&a, hyp_type_type_param(&a, "T")); /* T& as in Wrapper<F, T&> */
    const char *params[] = {"F", "T", NULL};
    const HYPType *args[2];
    args[0] = hyp_type_named(&a, "proj.Fmt"); /* explicit arg for F */
    args[1] = (const HYPType *)0x37;          /* simulated uninitialized stack garbage */
    const HYPType *sub = hyp_type_substitute(&a, t, params, args);
    ASSERT_NOT_NULL(sub);
    ASSERT_EQ(sub->kind, HYP_TYPE_REFERENCE);
    /* T has no real binding: it must be preserved, never the garbage value. */
    ASSERT_TRUE(sub->data.reference.elem != (const HYPType *)0x37);
    ASSERT_EQ(sub->data.reference.elem->kind, HYP_TYPE_TYPE_PARAM);
    hyp_arena_destroy(&a);
    PASS();
}

/* ── Suite ─────────────────────────────────────────────────────── */

SUITE(type_rep) {
    /* UNION */
    RUN_TEST(typerep_union_two_distinct);
    RUN_TEST(typerep_union_dedupes_duplicates);
    RUN_TEST(typerep_union_flattens_nested);
    RUN_TEST(typerep_union_single_member_collapses);
    RUN_TEST(typerep_union_empty_is_unknown);
    RUN_TEST(typerep_optional_is_union_with_none);
    /* LITERAL */
    RUN_TEST(typerep_literal_int_3);
    RUN_TEST(typerep_literal_equality_distinguishes_text);
    /* PROTOCOL */
    RUN_TEST(typerep_protocol_method_set);
    RUN_TEST(typerep_protocol_satisfied_by_protocol_with_superset);
    RUN_TEST(typerep_protocol_unsatisfied_when_method_missing);
    /* MODULE */
    RUN_TEST(typerep_module_carries_qn);
    RUN_TEST(typerep_module_equality_by_qn);
    /* CALLABLE */
    RUN_TEST(typerep_callable_with_args_and_return);
    RUN_TEST(typerep_callable_elliptic_arity_minus_one);
    RUN_TEST(typerep_callable_equality);
    /* SUBSTITUTION */
    RUN_TEST(typerep_substitute_unbound_param_preserved);
    RUN_TEST(typerep_substitute_short_args_no_oob_issue427);
    RUN_TEST(typerep_substitute_rejects_garbage_args_entries);
}
