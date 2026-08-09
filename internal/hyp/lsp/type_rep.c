#include "type_rep.h"
#include <stdint.h>
#include <string.h>

/* No real allocation lives in the first page; values below this are garbage
 * (e.g. small integers or truncated string bytes misread as pointers). */
enum { TR_MIN_PLAUSIBLE_PTR = 4096 };

// Singleton UNKNOWN type (no allocation needed).
static const HYPType unknown_singleton = {.kind = HYP_TYPE_UNKNOWN};

const HYPType *hyp_type_unknown(void) {
    return &unknown_singleton;
}

const HYPType *hyp_type_named(HYPArena *a, const char *qualified_name) {
    HYPType *t = (HYPType *)hyp_arena_alloc(a, sizeof(HYPType));
    if (!t)
        return &unknown_singleton;
    memset(t, 0, sizeof(HYPType));
    t->kind = HYP_TYPE_NAMED;
    t->data.named.qualified_name = hyp_arena_strdup(a, qualified_name);
    return t;
}

const HYPType *hyp_type_pointer(HYPArena *a, const HYPType *elem) {
    HYPType *t = (HYPType *)hyp_arena_alloc(a, sizeof(HYPType));
    if (!t)
        return &unknown_singleton;
    memset(t, 0, sizeof(HYPType));
    t->kind = HYP_TYPE_POINTER;
    t->data.pointer.elem = elem;
    return t;
}

const HYPType *hyp_type_slice(HYPArena *a, const HYPType *elem) {
    HYPType *t = (HYPType *)hyp_arena_alloc(a, sizeof(HYPType));
    if (!t)
        return &unknown_singleton;
    memset(t, 0, sizeof(HYPType));
    t->kind = HYP_TYPE_SLICE;
    t->data.slice.elem = elem;
    return t;
}

const HYPType *hyp_type_map(HYPArena *a, const HYPType *key, const HYPType *value) {
    HYPType *t = (HYPType *)hyp_arena_alloc(a, sizeof(HYPType));
    if (!t)
        return &unknown_singleton;
    memset(t, 0, sizeof(HYPType));
    t->kind = HYP_TYPE_MAP;
    t->data.map.key = key;
    t->data.map.value = value;
    return t;
}

const HYPType *hyp_type_channel(HYPArena *a, const HYPType *elem, int direction) {
    HYPType *t = (HYPType *)hyp_arena_alloc(a, sizeof(HYPType));
    if (!t)
        return &unknown_singleton;
    memset(t, 0, sizeof(HYPType));
    t->kind = HYP_TYPE_CHANNEL;
    t->data.channel.elem = elem;
    t->data.channel.direction = direction;
    return t;
}

const HYPType *hyp_type_func(HYPArena *a, const char **param_names, const HYPType **param_types,
                             const HYPType **return_types) {
    HYPType *t = (HYPType *)hyp_arena_alloc(a, sizeof(HYPType));
    if (!t)
        return &unknown_singleton;
    memset(t, 0, sizeof(HYPType));
    t->kind = HYP_TYPE_FUNC;

    // Copy all arrays into arena memory to avoid dangling stack pointers.
    if (return_types) {
        int count = 0;
        while (return_types[count])
            count++;
        const HYPType **arr =
            (const HYPType **)hyp_arena_alloc(a, (count + 1) * sizeof(const HYPType *));
        if (arr) {
            for (int i = 0; i < count; i++)
                arr[i] = return_types[i];
            arr[count] = NULL;
            t->data.func.return_types = arr;
        }
    }
    if (param_types) {
        int count = 0;
        while (param_types[count])
            count++;
        const HYPType **arr =
            (const HYPType **)hyp_arena_alloc(a, (count + 1) * sizeof(const HYPType *));
        if (arr) {
            for (int i = 0; i < count; i++)
                arr[i] = param_types[i];
            arr[count] = NULL;
            t->data.func.param_types = arr;
        }
    }
    if (param_names) {
        int count = 0;
        while (param_names[count])
            count++;
        const char **arr = (const char **)hyp_arena_alloc(a, (count + 1) * sizeof(const char *));
        if (arr) {
            for (int i = 0; i < count; i++)
                arr[i] = param_names[i];
            arr[count] = NULL;
            t->data.func.param_names = arr;
        }
    }
    return t;
}

const HYPType **hyp_type_materialize_signature_params(HYPArena *a, const char *const *type_texts,
                                                      int count, HYPTypeTextParser parser,
                                                      void *parser_ctx) {
    if (count <= 0)
        return NULL;

    const HYPType **types =
        (const HYPType **)hyp_arena_alloc(a, ((size_t)count + 1) * sizeof(const HYPType *));
    if (!types)
        return NULL;

    for (int i = 0; i < count; i++) {
        const char *text = type_texts ? type_texts[i] : NULL;
        if (!text || text[0] == '\0' || strcmp(text, "?") == 0) {
            types[i] = hyp_type_unknown();
            continue;
        }

        const HYPType *parsed = parser ? parser(a, text, parser_ctx) : NULL;
        types[i] = parsed ? parsed : hyp_type_unknown();
    }
    types[count] = NULL;
    return types;
}

const HYPType *hyp_type_func_replace_returns(HYPArena *a, const HYPType *old_signature,
                                             const HYPType *const *new_return_types) {
    if (!old_signature || old_signature->kind != HYP_TYPE_FUNC)
        return hyp_type_unknown();

    /* hyp_type_func only reads and then copies this vector into arena memory. */
    return hyp_type_func(a, old_signature->data.func.param_names,
                         old_signature->data.func.param_types, (const HYPType **)new_return_types);
}

const HYPType *hyp_type_builtin(HYPArena *a, const char *name) {
    HYPType *t = (HYPType *)hyp_arena_alloc(a, sizeof(HYPType));
    if (!t)
        return &unknown_singleton;
    memset(t, 0, sizeof(HYPType));
    t->kind = HYP_TYPE_BUILTIN;
    t->data.builtin.name = hyp_arena_strdup(a, name);
    return t;
}

const HYPType *hyp_type_tuple(HYPArena *a, const HYPType **elems, int count) {
    HYPType *t = (HYPType *)hyp_arena_alloc(a, sizeof(HYPType));
    if (!t)
        return &unknown_singleton;
    memset(t, 0, sizeof(HYPType));
    t->kind = HYP_TYPE_TUPLE;
    // Copy elems array
    const HYPType **arr =
        (const HYPType **)hyp_arena_alloc(a, (count + 1) * sizeof(const HYPType *));
    if (!arr)
        return &unknown_singleton;
    for (int i = 0; i < count; i++)
        arr[i] = elems[i];
    arr[count] = NULL;
    t->data.tuple.elems = arr;
    t->data.tuple.count = count;
    return t;
}

const HYPType *hyp_type_type_param(HYPArena *a, const char *name) {
    HYPType *t = (HYPType *)hyp_arena_alloc(a, sizeof(HYPType));
    if (!t)
        return &unknown_singleton;
    memset(t, 0, sizeof(HYPType));
    t->kind = HYP_TYPE_TYPE_PARAM;
    t->data.type_param.name = hyp_arena_strdup(a, name);
    return t;
}

const HYPType *hyp_type_reference(HYPArena *a, const HYPType *elem) {
    HYPType *t = (HYPType *)hyp_arena_alloc(a, sizeof(HYPType));
    if (!t)
        return &unknown_singleton;
    memset(t, 0, sizeof(HYPType));
    t->kind = HYP_TYPE_REFERENCE;
    t->data.reference.elem = elem;
    return t;
}

const HYPType *hyp_type_rvalue_ref(HYPArena *a, const HYPType *elem) {
    HYPType *t = (HYPType *)hyp_arena_alloc(a, sizeof(HYPType));
    if (!t)
        return &unknown_singleton;
    memset(t, 0, sizeof(HYPType));
    t->kind = HYP_TYPE_RVALUE_REF;
    t->data.reference.elem = elem;
    return t;
}

const HYPType *hyp_type_template(HYPArena *a, const char *name, const HYPType **args,
                                 int arg_count) {
    HYPType *t = (HYPType *)hyp_arena_alloc(a, sizeof(HYPType));
    if (!t)
        return &unknown_singleton;
    memset(t, 0, sizeof(HYPType));
    t->kind = HYP_TYPE_TEMPLATE;
    t->data.template_type.template_name = hyp_arena_strdup(a, name);
    if (args && arg_count > 0) {
        const HYPType **arr =
            (const HYPType **)hyp_arena_alloc(a, (arg_count + 1) * sizeof(const HYPType *));
        if (arr) {
            for (int i = 0; i < arg_count; i++)
                arr[i] = args[i];
            arr[arg_count] = NULL;
            t->data.template_type.template_args = arr;
        }
    }
    t->data.template_type.arg_count = arg_count;
    return t;
}

const HYPType *hyp_type_alias(HYPArena *a, const char *alias_qn, const HYPType *underlying) {
    HYPType *t = (HYPType *)hyp_arena_alloc(a, sizeof(HYPType));
    if (!t)
        return &unknown_singleton;
    memset(t, 0, sizeof(HYPType));
    t->kind = HYP_TYPE_ALIAS;
    t->data.alias.alias_qn = hyp_arena_strdup(a, alias_qn);
    t->data.alias.underlying = underlying;
    return t;
}

// --- Python-flavored constructors -------------------------------------------

// Dedupe members by structural equality, in place. Returns new length.
// Preserves first-seen order so output is deterministic.
static int union_member_dedupe(const HYPType **scratch, int count) {
    int out = 0;
    for (int i = 0; i < count; i++) {
        bool seen = false;
        for (int j = 0; j < out; j++) {
            if (hyp_type_equal(scratch[i], scratch[j])) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            scratch[out++] = scratch[i];
        }
    }
    return out;
}

// Shared by Python (hyp_type_union) and TS (`A | B`). Flattens nested UNIONs and
// dedupes members.
const HYPType *hyp_type_union(HYPArena *a, const HYPType **members, int count) {
    if (!members || count <= 0)
        return &unknown_singleton;

    // Flatten: nested UNIONs unfold their members into the parent.
    int flat_cap = count * 2 + 4;
    const HYPType **flat = (const HYPType **)hyp_arena_alloc(a, flat_cap * sizeof(const HYPType *));
    if (!flat)
        return &unknown_singleton;
    int flat_count = 0;
    for (int i = 0; i < count; i++) {
        const HYPType *m = members[i];
        if (!m || hyp_type_is_unknown(m))
            continue;
        if (m->kind == HYP_TYPE_UNION) {
            for (int j = 0; j < m->data.union_type.count; j++) {
                if (flat_count < flat_cap)
                    flat[flat_count++] = m->data.union_type.members[j];
            }
        } else {
            if (flat_count < flat_cap)
                flat[flat_count++] = m;
        }
    }
    if (flat_count == 0)
        return &unknown_singleton;

    // Dedupe by structural equality.
    int unique_count = union_member_dedupe(flat, flat_count);
    if (unique_count == 1)
        return flat[0];

    HYPType *t = (HYPType *)hyp_arena_alloc(a, sizeof(HYPType));
    if (!t)
        return &unknown_singleton;
    memset(t, 0, sizeof(HYPType));
    t->kind = HYP_TYPE_UNION;
    const HYPType **arr =
        (const HYPType **)hyp_arena_alloc(a, (unique_count + 1) * sizeof(const HYPType *));
    if (!arr)
        return &unknown_singleton;
    for (int i = 0; i < unique_count; i++)
        arr[i] = flat[i];
    arr[unique_count] = NULL;
    t->data.union_type.members = arr;
    t->data.union_type.count = unique_count;
    return t;
}

const HYPType *hyp_type_optional(HYPArena *a, const HYPType *inner) {
    if (!inner)
        return &unknown_singleton;
    const HYPType *none_t = hyp_type_builtin(a, "None");
    const HYPType *members[2] = {inner, none_t};
    return hyp_type_union(a, members, 2);
}

const HYPType *hyp_type_literal(HYPArena *a, const HYPType *base, const char *literal_text) {
    HYPType *t = (HYPType *)hyp_arena_alloc(a, sizeof(HYPType));
    if (!t)
        return &unknown_singleton;
    memset(t, 0, sizeof(HYPType));
    t->kind = HYP_TYPE_LITERAL;
    t->data.literal.base = base ? base : &unknown_singleton;
    t->data.literal.literal_text = literal_text ? hyp_arena_strdup(a, literal_text) : NULL;
    return t;
}

const HYPType *hyp_type_protocol(HYPArena *a, const char *qualified_name, const char **method_names,
                                 const HYPType **method_sigs) {
    HYPType *t = (HYPType *)hyp_arena_alloc(a, sizeof(HYPType));
    if (!t)
        return &unknown_singleton;
    memset(t, 0, sizeof(HYPType));
    t->kind = HYP_TYPE_PROTOCOL;
    t->data.protocol.qualified_name = qualified_name ? hyp_arena_strdup(a, qualified_name) : NULL;

    int n = 0;
    if (method_names) {
        while (method_names[n])
            n++;
    }
    if (n > 0) {
        const char **names = (const char **)hyp_arena_alloc(a, (n + 1) * sizeof(const char *));
        const HYPType **sigs =
            (const HYPType **)hyp_arena_alloc(a, (n + 1) * sizeof(const HYPType *));
        if (names && sigs) {
            for (int i = 0; i < n; i++) {
                names[i] = hyp_arena_strdup(a, method_names[i]);
                sigs[i] = method_sigs ? method_sigs[i] : NULL;
            }
            names[n] = NULL;
            sigs[n] = NULL;
            t->data.protocol.method_names = names;
            t->data.protocol.method_sigs = sigs;
        }
    }
    return t;
}

const HYPType *hyp_type_module(HYPArena *a, const char *module_qn) {
    HYPType *t = (HYPType *)hyp_arena_alloc(a, sizeof(HYPType));
    if (!t)
        return &unknown_singleton;
    memset(t, 0, sizeof(HYPType));
    t->kind = HYP_TYPE_MODULE;
    t->data.module.module_qn = module_qn ? hyp_arena_strdup(a, module_qn) : NULL;
    return t;
}

const HYPType *hyp_type_callable(HYPArena *a, const HYPType **param_types, int param_count,
                                 const HYPType *return_type) {
    HYPType *t = (HYPType *)hyp_arena_alloc(a, sizeof(HYPType));
    if (!t)
        return &unknown_singleton;
    memset(t, 0, sizeof(HYPType));
    t->kind = HYP_TYPE_CALLABLE;
    t->data.callable.param_count = param_count;
    t->data.callable.return_type = return_type ? return_type : &unknown_singleton;
    if (param_count > 0 && param_types) {
        const HYPType **arr =
            (const HYPType **)hyp_arena_alloc(a, (param_count + 1) * sizeof(const HYPType *));
        if (arr) {
            for (int i = 0; i < param_count; i++)
                arr[i] = param_types[i];
            arr[param_count] = NULL;
            t->data.callable.param_types = arr;
        }
    }
    return t;
}

// --- TS-specific constructors -----------------------------------------------

const HYPType *hyp_type_intersection(HYPArena *a, const HYPType **members, int count) {
    HYPType *t = (HYPType *)hyp_arena_alloc(a, sizeof(HYPType));
    if (!t)
        return &unknown_singleton;
    memset(t, 0, sizeof(HYPType));
    t->kind = HYP_TYPE_INTERSECTION;
    if (members && count > 0) {
        const HYPType **arr =
            (const HYPType **)hyp_arena_alloc(a, (count + 1) * sizeof(const HYPType *));
        if (arr) {
            for (int i = 0; i < count; i++)
                arr[i] = members[i];
            arr[count] = NULL;
            t->data.union_type.members = arr;
        }
    }
    t->data.union_type.count = count;
    return t;
}

const HYPType *hyp_type_ts_literal(HYPArena *a, const char *tag, const char *value) {
    HYPType *t = (HYPType *)hyp_arena_alloc(a, sizeof(HYPType));
    if (!t)
        return &unknown_singleton;
    memset(t, 0, sizeof(HYPType));
    t->kind = HYP_TYPE_TS_LITERAL;
    t->data.literal_ts.tag = tag ? hyp_arena_strdup(a, tag) : NULL;
    t->data.literal_ts.value = value ? hyp_arena_strdup(a, value) : NULL;
    return t;
}

const HYPType *hyp_type_indexed(HYPArena *a, const HYPType *object, const HYPType *index) {
    HYPType *t = (HYPType *)hyp_arena_alloc(a, sizeof(HYPType));
    if (!t)
        return &unknown_singleton;
    memset(t, 0, sizeof(HYPType));
    t->kind = HYP_TYPE_INDEXED;
    t->data.indexed.object = object;
    t->data.indexed.index = index;
    return t;
}

const HYPType *hyp_type_keyof(HYPArena *a, const HYPType *operand) {
    HYPType *t = (HYPType *)hyp_arena_alloc(a, sizeof(HYPType));
    if (!t)
        return &unknown_singleton;
    memset(t, 0, sizeof(HYPType));
    t->kind = HYP_TYPE_KEYOF;
    t->data.keyof.operand = operand;
    return t;
}

const HYPType *hyp_type_typeof_query(HYPArena *a, const char *expr) {
    HYPType *t = (HYPType *)hyp_arena_alloc(a, sizeof(HYPType));
    if (!t)
        return &unknown_singleton;
    memset(t, 0, sizeof(HYPType));
    t->kind = HYP_TYPE_TYPEOF_QUERY;
    t->data.typeof_query.expr = expr ? hyp_arena_strdup(a, expr) : NULL;
    return t;
}

const HYPType *hyp_type_conditional(HYPArena *a, const HYPType *check, const HYPType *extends,
                                    const HYPType *true_branch, const HYPType *false_branch) {
    HYPType *t = (HYPType *)hyp_arena_alloc(a, sizeof(HYPType));
    if (!t)
        return &unknown_singleton;
    memset(t, 0, sizeof(HYPType));
    t->kind = HYP_TYPE_CONDITIONAL;
    t->data.conditional.check = check;
    t->data.conditional.extends = extends;
    t->data.conditional.true_branch = true_branch;
    t->data.conditional.false_branch = false_branch;
    return t;
}

const HYPType *hyp_type_object_lit(HYPArena *a, const char **prop_names, const HYPType **prop_types,
                                   const HYPType *call_signature, const HYPType *index_value) {
    HYPType *t = (HYPType *)hyp_arena_alloc(a, sizeof(HYPType));
    if (!t)
        return &unknown_singleton;
    memset(t, 0, sizeof(HYPType));
    t->kind = HYP_TYPE_OBJECT_LIT;
    if (prop_names && prop_types) {
        int count = 0;
        while (prop_names[count] && prop_types[count])
            count++;
        if (count > 0) {
            const char **name_arr =
                (const char **)hyp_arena_alloc(a, (count + 1) * sizeof(const char *));
            const HYPType **type_arr =
                (const HYPType **)hyp_arena_alloc(a, (count + 1) * sizeof(const HYPType *));
            if (name_arr && type_arr) {
                for (int i = 0; i < count; i++) {
                    name_arr[i] = prop_names[i];
                    type_arr[i] = prop_types[i];
                }
                name_arr[count] = NULL;
                type_arr[count] = NULL;
                t->data.object_lit.prop_names = name_arr;
                t->data.object_lit.prop_types = type_arr;
            }
        }
    }
    t->data.object_lit.call_signature = call_signature;
    t->data.object_lit.index_value = index_value;
    return t;
}

const HYPType *hyp_type_infer(HYPArena *a, const char *name) {
    HYPType *t = (HYPType *)hyp_arena_alloc(a, sizeof(HYPType));
    if (!t)
        return &unknown_singleton;
    memset(t, 0, sizeof(HYPType));
    t->kind = HYP_TYPE_INFER;
    t->data.infer.name = name ? hyp_arena_strdup(a, name) : NULL;
    return t;
}

const HYPType *hyp_type_mapped(HYPArena *a, const char *key_name, const HYPType *key_constraint,
                               const HYPType *value) {
    HYPType *t = (HYPType *)hyp_arena_alloc(a, sizeof(HYPType));
    if (!t)
        return &unknown_singleton;
    memset(t, 0, sizeof(HYPType));
    t->kind = HYP_TYPE_MAPPED;
    t->data.mapped.key_name = key_name ? hyp_arena_strdup(a, key_name) : NULL;
    t->data.mapped.key_constraint = key_constraint;
    t->data.mapped.value = value;
    return t;
}

// Operations

const HYPType *hyp_type_deref(const HYPType *t) {
    if (!t)
        return t;
    // Unwrap references transparently (C++ member access through refs)
    if (t->kind == HYP_TYPE_REFERENCE || t->kind == HYP_TYPE_RVALUE_REF)
        return t->data.reference.elem;
    if (t->kind != HYP_TYPE_POINTER)
        return t;
    return t->data.pointer.elem;
}

const HYPType *hyp_type_elem(const HYPType *t) {
    if (!t)
        return hyp_type_unknown();
    switch (t->kind) {
    case HYP_TYPE_POINTER:
        return t->data.pointer.elem;
    case HYP_TYPE_SLICE:
        return t->data.slice.elem;
    case HYP_TYPE_CHANNEL:
        return t->data.channel.elem;
    case HYP_TYPE_REFERENCE:
        return t->data.reference.elem;
    case HYP_TYPE_RVALUE_REF:
        return t->data.reference.elem;
    default:
        return hyp_type_unknown();
    }
}

bool hyp_type_is_unknown(const HYPType *t) {
    if (!t)
        return true;
    /* Guard against dangling pointers from stale field_types entries.
     * Check alignment before dereferencing — misaligned pointer means garbage. */
    if (((uintptr_t)t & (_Alignof(HYPType) - 1)) != 0)
        return true;
    return t->kind == HYP_TYPE_UNKNOWN;
}

bool hyp_type_is_interface(const HYPType *t) {
    return t && t->kind == HYP_TYPE_INTERFACE;
}

bool hyp_type_is_pointer(const HYPType *t) {
    return t && t->kind == HYP_TYPE_POINTER;
}

bool hyp_type_is_reference(const HYPType *t) {
    return t && (t->kind == HYP_TYPE_REFERENCE || t->kind == HYP_TYPE_RVALUE_REF);
}

bool hyp_type_is_union(const HYPType *t) {
    return t && t->kind == HYP_TYPE_UNION;
}

bool hyp_type_is_protocol(const HYPType *t) {
    return t && t->kind == HYP_TYPE_PROTOCOL;
}

bool hyp_type_is_module(const HYPType *t) {
    return t && t->kind == HYP_TYPE_MODULE;
}

static bool str_eq_or_both_null(const char *a, const char *b) {
    if (a == b)
        return true;
    if (!a || !b)
        return false;
    return strcmp(a, b) == 0;
}

bool hyp_type_equal(const HYPType *a, const HYPType *b) {
    if (a == b)
        return true;
    if (!a || !b)
        return false;
    if (a->kind != b->kind)
        return false;

    switch (a->kind) {
    case HYP_TYPE_UNKNOWN:
        return true;
    case HYP_TYPE_NAMED:
        return str_eq_or_both_null(a->data.named.qualified_name, b->data.named.qualified_name);
    case HYP_TYPE_BUILTIN:
        return str_eq_or_both_null(a->data.builtin.name, b->data.builtin.name);
    case HYP_TYPE_TYPE_PARAM:
        return str_eq_or_both_null(a->data.type_param.name, b->data.type_param.name);
    case HYP_TYPE_POINTER:
        return hyp_type_equal(a->data.pointer.elem, b->data.pointer.elem);
    case HYP_TYPE_SLICE:
        return hyp_type_equal(a->data.slice.elem, b->data.slice.elem);
    case HYP_TYPE_REFERENCE:
    case HYP_TYPE_RVALUE_REF:
        return hyp_type_equal(a->data.reference.elem, b->data.reference.elem);
    case HYP_TYPE_MAP:
        return hyp_type_equal(a->data.map.key, b->data.map.key) &&
               hyp_type_equal(a->data.map.value, b->data.map.value);
    case HYP_TYPE_CHANNEL:
        return a->data.channel.direction == b->data.channel.direction &&
               hyp_type_equal(a->data.channel.elem, b->data.channel.elem);
    case HYP_TYPE_TUPLE: {
        if (a->data.tuple.count != b->data.tuple.count)
            return false;
        for (int i = 0; i < a->data.tuple.count; i++) {
            if (!hyp_type_equal(a->data.tuple.elems[i], b->data.tuple.elems[i]))
                return false;
        }
        return true;
    }
    case HYP_TYPE_TEMPLATE: {
        if (!str_eq_or_both_null(a->data.template_type.template_name,
                                 b->data.template_type.template_name))
            return false;
        if (a->data.template_type.arg_count != b->data.template_type.arg_count)
            return false;
        for (int i = 0; i < a->data.template_type.arg_count; i++) {
            if (!hyp_type_equal(a->data.template_type.template_args[i],
                                b->data.template_type.template_args[i]))
                return false;
        }
        return true;
    }
    case HYP_TYPE_ALIAS:
        return str_eq_or_both_null(a->data.alias.alias_qn, b->data.alias.alias_qn);
    case HYP_TYPE_UNION: {
        if (a->data.union_type.count != b->data.union_type.count)
            return false;
        // Order-independent: every a-member must appear in b's set.
        for (int i = 0; i < a->data.union_type.count; i++) {
            bool found = false;
            for (int j = 0; j < b->data.union_type.count; j++) {
                if (hyp_type_equal(a->data.union_type.members[i], b->data.union_type.members[j])) {
                    found = true;
                    break;
                }
            }
            if (!found)
                return false;
        }
        return true;
    }
    case HYP_TYPE_LITERAL:
        return hyp_type_equal(a->data.literal.base, b->data.literal.base) &&
               str_eq_or_both_null(a->data.literal.literal_text, b->data.literal.literal_text);
    case HYP_TYPE_PROTOCOL:
        return str_eq_or_both_null(a->data.protocol.qualified_name,
                                   b->data.protocol.qualified_name);
    case HYP_TYPE_MODULE:
        return str_eq_or_both_null(a->data.module.module_qn, b->data.module.module_qn);
    case HYP_TYPE_CALLABLE: {
        if (a->data.callable.param_count != b->data.callable.param_count)
            return false;
        if (!hyp_type_equal(a->data.callable.return_type, b->data.callable.return_type))
            return false;
        if (a->data.callable.param_count > 0) {
            for (int i = 0; i < a->data.callable.param_count; i++) {
                if (!hyp_type_equal(a->data.callable.param_types[i],
                                    b->data.callable.param_types[i]))
                    return false;
            }
        }
        return true;
    }
    case HYP_TYPE_INTERSECTION: {
        // Same shape as UNION; compare order-independently.
        if (a->data.union_type.count != b->data.union_type.count)
            return false;
        for (int i = 0; i < a->data.union_type.count; i++) {
            bool found = false;
            for (int j = 0; j < b->data.union_type.count; j++) {
                if (hyp_type_equal(a->data.union_type.members[i], b->data.union_type.members[j])) {
                    found = true;
                    break;
                }
            }
            if (!found)
                return false;
        }
        return true;
    }
    case HYP_TYPE_TS_LITERAL:
        return str_eq_or_both_null(a->data.literal_ts.tag, b->data.literal_ts.tag) &&
               str_eq_or_both_null(a->data.literal_ts.value, b->data.literal_ts.value);
    case HYP_TYPE_INDEXED:
        return hyp_type_equal(a->data.indexed.object, b->data.indexed.object) &&
               hyp_type_equal(a->data.indexed.index, b->data.indexed.index);
    case HYP_TYPE_KEYOF:
        return hyp_type_equal(a->data.keyof.operand, b->data.keyof.operand);
    case HYP_TYPE_TYPEOF_QUERY:
        return str_eq_or_both_null(a->data.typeof_query.expr, b->data.typeof_query.expr);
    case HYP_TYPE_CONDITIONAL:
        return hyp_type_equal(a->data.conditional.check, b->data.conditional.check) &&
               hyp_type_equal(a->data.conditional.extends, b->data.conditional.extends) &&
               hyp_type_equal(a->data.conditional.true_branch, b->data.conditional.true_branch) &&
               hyp_type_equal(a->data.conditional.false_branch, b->data.conditional.false_branch);
    case HYP_TYPE_INFER:
        return str_eq_or_both_null(a->data.infer.name, b->data.infer.name);
    case HYP_TYPE_OBJECT_LIT:
    case HYP_TYPE_MAPPED:
    case HYP_TYPE_FUNC:
    case HYP_TYPE_INTERFACE:
    case HYP_TYPE_STRUCT:
        // Structural equality on these is expensive and rarely needed by callers
        // beyond pointer identity (already checked above). Treat as not-equal.
        return false;
    }
    return false;
}

bool hyp_type_protocol_satisfied_by(const HYPType *proto, const HYPType *candidate) {
    if (!proto || proto->kind != HYP_TYPE_PROTOCOL)
        return false;
    if (!candidate)
        return false;
    // candidate must be a NAMED or PROTOCOL type with a method-name set we
    // can inspect. For PROTOCOL candidates, trivially satisfied if every
    // proto method appears in candidate's method list.
    if (candidate->kind == HYP_TYPE_PROTOCOL) {
        if (!proto->data.protocol.method_names)
            return true;
        for (int i = 0; proto->data.protocol.method_names[i]; i++) {
            const char *needed = proto->data.protocol.method_names[i];
            bool found = false;
            if (candidate->data.protocol.method_names) {
                for (int j = 0; candidate->data.protocol.method_names[j]; j++) {
                    if (str_eq_or_both_null(needed, candidate->data.protocol.method_names[j])) {
                        found = true;
                        break;
                    }
                }
            }
            if (!found)
                return false;
        }
        return true;
    }
    // Nominal candidates require the registry — caller's responsibility.
    return false;
}

const HYPType *hyp_type_resolve_alias(const HYPType *t) {
    for (int i = 0; i < 16 && t; i++) {
        if (t->kind != HYP_TYPE_ALIAS)
            return t;
        if (!t->data.alias.underlying)
            return t;
        t = t->data.alias.underlying;
    }
    return t;
}

// Generic substitution: recursively replace TYPE_PARAM with concrete types.
const HYPType *hyp_type_substitute(HYPArena *a, const HYPType *t, const char **type_params,
                                   const HYPType **type_args) {
    if (!t)
        return hyp_type_unknown();
    if (!type_params || !type_args)
        return t;

    /* type_args may be SHORTER than type_params — a class template instantiated
     * with fewer args than declared params, or trailing default template args
     * (e.g. `Box<Widget>` for `template<class T, class U, class V>`). Indexing
     * type_args[i] by the type_params loop index would then read past the args
     * array, yielding a bogus HYPType* that is later dereferenced -> SEGV (#427).
     * type_params is always NULL-terminated; type_args is either parallel-length
     * (some callers pass a fixed positional array that is NOT NULL-terminated) or
     * shorter-and-NULL-terminated. Bound the length walk by the param count so it
     * can never run off a non-terminated args array, then bound every type_args[i]
     * access by the result. */
    int nparams = 0;
    while (type_params[nparams]) {
        nparams++;
    }
    /* Contract: type_args must be NULL-terminated (it may be shorter than
     * type_params). A misaligned or null-page value can never be a real
     * HYPType* — it means a caller passed an unterminated array and the walk
     * is reading uninitialized memory (seen on bitcoin's serialize.h: an
     * explicit-template-arg call bound T to stack garbage that was woven into
     * the registered type graph and dereferenced later -> SIGSEGV). Treat such
     * values as the terminator so garbage can never enter a type graph. */
    int args_len = 0;
    while (args_len < nparams && type_args[args_len] &&
           ((uintptr_t)type_args[args_len] & (sizeof(void *) - 1)) == 0 &&
           (uintptr_t)type_args[args_len] >= TR_MIN_PLAUSIBLE_PTR) {
        args_len++;
    }

    switch (t->kind) {
    case HYP_TYPE_TYPE_PARAM: {
        for (int i = 0; type_params[i]; i++) {
            if (strcmp(t->data.type_param.name, type_params[i]) == 0) {
                return (i < args_len && type_args[i]) ? type_args[i] : t;
            }
        }
        return t; // unmatched param stays as-is
    }
    case HYP_TYPE_NAMED: {
        // Also substitute NAMED types matching template param names.
        // c_parse_return_type_text may parse "A" as NAMED("test.main.A")
        // instead of TYPE_PARAM("A") — check both full QN and short name.
        const char *qn = t->data.named.qualified_name;
        if (qn) {
            const char *short_name = strrchr(qn, '.');
            short_name = short_name ? short_name + 1 : qn;
            for (int i = 0; type_params[i]; i++) {
                if (strcmp(qn, type_params[i]) == 0 || strcmp(short_name, type_params[i]) == 0) {
                    return (i < args_len && type_args[i]) ? type_args[i] : t;
                }
            }
        }
        return t;
    }
    case HYP_TYPE_POINTER:
        return hyp_type_pointer(
            a, hyp_type_substitute(a, t->data.pointer.elem, type_params, type_args));
    case HYP_TYPE_REFERENCE:
        return hyp_type_reference(
            a, hyp_type_substitute(a, t->data.reference.elem, type_params, type_args));
    case HYP_TYPE_RVALUE_REF:
        return hyp_type_rvalue_ref(
            a, hyp_type_substitute(a, t->data.reference.elem, type_params, type_args));
    case HYP_TYPE_SLICE:
        return hyp_type_slice(a,
                              hyp_type_substitute(a, t->data.slice.elem, type_params, type_args));
    case HYP_TYPE_MAP:
        return hyp_type_map(a, hyp_type_substitute(a, t->data.map.key, type_params, type_args),
                            hyp_type_substitute(a, t->data.map.value, type_params, type_args));
    case HYP_TYPE_CHANNEL:
        return hyp_type_channel(
            a, hyp_type_substitute(a, t->data.channel.elem, type_params, type_args),
            t->data.channel.direction);
    case HYP_TYPE_TUPLE: {
        int count = t->data.tuple.count;
        const HYPType **elems =
            (const HYPType **)hyp_arena_alloc(a, (count + 1) * sizeof(const HYPType *));
        if (!elems)
            return t;
        for (int i = 0; i < count; i++) {
            elems[i] = hyp_type_substitute(a, t->data.tuple.elems[i], type_params, type_args);
        }
        elems[count] = NULL;
        return hyp_type_tuple(a, elems, count);
    }
    case HYP_TYPE_UNION:
    case HYP_TYPE_INTERSECTION: {
        int count = t->data.union_type.count;
        if (count <= 0 || !t->data.union_type.members)
            return t;
        const HYPType **elems =
            (const HYPType **)hyp_arena_alloc(a, (count + 1) * sizeof(const HYPType *));
        if (!elems)
            return t;
        for (int i = 0; i < count; i++) {
            elems[i] =
                hyp_type_substitute(a, t->data.union_type.members[i], type_params, type_args);
        }
        elems[count] = NULL;
        return t->kind == HYP_TYPE_UNION ? hyp_type_union(a, elems, count)
                                         : hyp_type_intersection(a, elems, count);
    }
    case HYP_TYPE_INDEXED:
        return hyp_type_indexed(
            a, hyp_type_substitute(a, t->data.indexed.object, type_params, type_args),
            hyp_type_substitute(a, t->data.indexed.index, type_params, type_args));
    case HYP_TYPE_KEYOF:
        return hyp_type_keyof(
            a, hyp_type_substitute(a, t->data.keyof.operand, type_params, type_args));
    case HYP_TYPE_CONDITIONAL:
        return hyp_type_conditional(
            a, hyp_type_substitute(a, t->data.conditional.check, type_params, type_args),
            hyp_type_substitute(a, t->data.conditional.extends, type_params, type_args),
            hyp_type_substitute(a, t->data.conditional.true_branch, type_params, type_args),
            hyp_type_substitute(a, t->data.conditional.false_branch, type_params, type_args));
    case HYP_TYPE_FUNC: {
        // Recurse into param_types and return_types. Param/return arrays may be NULL.
        const HYPType **new_params = NULL;
        const HYPType **new_returns = NULL;
        if (t->data.func.param_types) {
            int pc = 0;
            while (t->data.func.param_types[pc])
                pc++;
            new_params =
                (const HYPType **)hyp_arena_alloc(a, (size_t)(pc + 1) * sizeof(const HYPType *));
            if (!new_params)
                return t;
            for (int i = 0; i < pc; i++) {
                new_params[i] =
                    hyp_type_substitute(a, t->data.func.param_types[i], type_params, type_args);
            }
            new_params[pc] = NULL;
        }
        if (t->data.func.return_types) {
            int rc = 0;
            while (t->data.func.return_types[rc])
                rc++;
            new_returns =
                (const HYPType **)hyp_arena_alloc(a, (size_t)(rc + 1) * sizeof(const HYPType *));
            if (!new_returns)
                return t;
            for (int i = 0; i < rc; i++) {
                new_returns[i] =
                    hyp_type_substitute(a, t->data.func.return_types[i], type_params, type_args);
            }
            new_returns[rc] = NULL;
        }
        return hyp_type_func(a, t->data.func.param_names, new_params, new_returns);
    }
    case HYP_TYPE_TEMPLATE: {
        if (!t->data.template_type.template_args || t->data.template_type.arg_count == 0)
            return t;
        int ac = t->data.template_type.arg_count;
        const HYPType **new_args =
            (const HYPType **)hyp_arena_alloc(a, (size_t)(ac + 1) * sizeof(const HYPType *));
        if (!new_args)
            return t;
        for (int i = 0; i < ac; i++) {
            new_args[i] = hyp_type_substitute(a, t->data.template_type.template_args[i],
                                              type_params, type_args);
        }
        new_args[ac] = NULL;
        return hyp_type_template(a, t->data.template_type.template_name, new_args, ac);
    }
    default:
        // BUILTIN, INTERFACE, STRUCT, LITERAL, TYPEOF_QUERY, OBJECT_LIT, INFER, MAPPED,
        // ALIAS — no in-place substitution needed at v1 (or stub-only).
        return t;
    }
}
