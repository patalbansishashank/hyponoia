#include "scope.h"
#include <string.h>

HYPScope* hyp_scope_push(HYPArena* a, HYPScope* current) {
    HYPScope* scope = (HYPScope*)hyp_arena_alloc(a, sizeof(HYPScope));
    if (!scope) {
        return current;
    }
    memset(scope, 0, sizeof(HYPScope));
    scope->parent = current;
    scope->arena = a;
    return scope;
}

HYPScope* hyp_scope_pop(HYPScope* scope) {
    if (!scope) {
        return NULL;
    }
    return scope->parent;
}

static HYPScopeChunk* alloc_chunk(HYPScope* scope) {
    if (!scope->arena) {
        return NULL;
    }
    HYPScopeChunk* c = (HYPScopeChunk*)hyp_arena_alloc(scope->arena, sizeof(HYPScopeChunk));
    if (!c) {
        return NULL;
    }
    memset(c, 0, sizeof(HYPScopeChunk));
    c->next = scope->chunks;
    scope->chunks = c;
    return c;
}

/* Returns false when the binding could NOT be recorded in THIS frame.
 *
 * The failure that matters is arena exhaustion in alloc_chunk: the old void
 * form returned silently, so a caller that then consulted the scope CHAIN saw
 * the parent's binding for the same name and concluded the child had been
 * bound. For callable-value proof that is a fabricated identity -- the shadow
 * never took effect, yet the parent's callable looks like the child's. Callers
 * needing that distinction must use the checked form and consult the LOCAL
 * result, not a chain lookup. */
static bool hyp_scope_bind_value(HYPScope *scope, const char *name, const HYPType *type,
                                 const char *callable_qn) {
    if (!scope || !name) {
        return false;
    }
    for (HYPScopeChunk* c = scope->chunks; c != NULL; c = c->next) {
        for (int i = 0; i < c->used; i++) {
            if (c->bindings[i].name && strcmp(c->bindings[i].name, name) == 0) {
                c->bindings[i].type = type;
                c->bindings[i].callable_qn = callable_qn;
                return true;
            }
        }
    }
    HYPScopeChunk* head = scope->chunks;
    if (!head || head->used >= HYP_SCOPE_CHUNK_BINDINGS) {
        head = alloc_chunk(scope);
        if (!head) {
            return false; /* arena exhausted: the shadow did NOT take effect */
        }
    }
    head->bindings[head->used].name = name;
    head->bindings[head->used].type = type;
    head->bindings[head->used].callable_qn = callable_qn;
    head->used++;
    return true;
}

void hyp_scope_bind(HYPScope *scope, const char *name, const HYPType *type) {
    (void)hyp_scope_bind_value(scope, name, type, NULL);
}

bool hyp_scope_bind_checked(HYPScope *scope, const char *name, const HYPType *type) {
    return hyp_scope_bind_value(scope, name, type, NULL);
}

void hyp_scope_bind_callable(HYPScope *scope, const char *name, const HYPType *type,
                             const char *callable_qn) {
    (void)hyp_scope_bind_value(scope, name, type, callable_qn);
}

bool hyp_scope_bind_callable_checked(HYPScope *scope, const char *name, const HYPType *type,
                                     const char *callable_qn) {
    return hyp_scope_bind_value(scope, name, type, callable_qn);
}

const HYPType* hyp_scope_lookup(const HYPScope* scope, const char* name) {
    if (!name) {
        return hyp_type_unknown();
    }
    for (const HYPScope* s = scope; s != NULL; s = s->parent) {
        for (HYPScopeChunk* c = s->chunks; c != NULL; c = c->next) {
            for (int i = 0; i < c->used; i++) {
                if (c->bindings[i].name && strcmp(c->bindings[i].name, name) == 0) {
                    return c->bindings[i].type;
                }
            }
        }
    }
    return hyp_type_unknown();
}

bool hyp_scope_contains(const HYPScope *scope, const char *name) {
    if (!name) {
        return false;
    }
    for (const HYPScope *s = scope; s != NULL; s = s->parent) {
        for (const HYPScopeChunk *c = s->chunks; c != NULL; c = c->next) {
            for (int i = 0; i < c->used; i++) {
                if (c->bindings[i].name && strcmp(c->bindings[i].name, name) == 0) {
                    return true;
                }
            }
        }
    }
    return false;
}

const char *hyp_scope_lookup_callable(const HYPScope *scope, const char *name) {
    if (!name) {
        return NULL;
    }
    for (const HYPScope *s = scope; s != NULL; s = s->parent) {
        for (const HYPScopeChunk *c = s->chunks; c != NULL; c = c->next) {
            for (int i = 0; i < c->used; i++) {
                if (c->bindings[i].name && strcmp(c->bindings[i].name, name) == 0) {
                    return c->bindings[i].callable_qn;
                }
            }
        }
    }
    return NULL;
}

bool hyp_scope_update_callable(HYPScope *scope, const char *name, const char *callable_qn) {
    if (!name) {
        return false;
    }
    for (HYPScope *s = scope; s != NULL; s = s->parent) {
        for (HYPScopeChunk *c = s->chunks; c != NULL; c = c->next) {
            for (int i = 0; i < c->used; i++) {
                if (c->bindings[i].name && strcmp(c->bindings[i].name, name) == 0) {
                    c->bindings[i].callable_qn = callable_qn;
                    return true;
                }
            }
        }
    }
    return false;
}
