#pragma once
#include <stddef.h>
#include "arena.h"

#define HYP_MACRO_MAX_PARAMS 4
#define HYP_MACRO_TABLE_CAP 4096

typedef struct {
    const char *name;
    int param_count;
    const char *param_names[HYP_MACRO_MAX_PARAMS];
    const char *expansion;
    const char *resolved_callee;
} HYPMacroEntry;

typedef struct HYPMacroTable {
    HYPMacroEntry entries[HYP_MACRO_TABLE_CAP];
    int count;
    HYPArena arena;
} HYPMacroTable;

// Add an entry. Silently drops on overflow.
void hyp_macro_table_add(HYPMacroTable *t, HYPArena *arena, const char *name, int param_count,
                         const char **param_names, const char *expansion,
                         const char *resolved_callee);

// Look up by name. Returns NULL if not found.
const HYPMacroEntry *hyp_macro_table_find(const HYPMacroTable *t, const char *name);

// Parse a single .inc file content into the table (arena-allocated strings).
void hyp_parse_inc_file(HYPMacroTable *t, HYPArena *arena, const char *content);

// Expand a macro call: substitute args into expansion text.
// Returns arena-allocated expanded text, or NULL if no expansion.
char *hyp_macro_expand(HYPArena *arena, const HYPMacroEntry *entry, const char **args,
                       int arg_count);

// Extract a callee name from expanded text (looks for ##class(X).Method or $$Label^Routine).
// Returns arena-allocated "X.Method" or "Label^Routine", or NULL.
char *hyp_macro_extract_callee(HYPArena *arena, const char *expansion);

// Allocate and populate a new table with the hardcoded system macros.
// Caller owns the table (stack or heap).
void hyp_macro_table_init_system(HYPMacroTable *t);

// Destroy the arena inside t and free t itself. NULL-safe.
void hyp_macro_table_free(HYPMacroTable *t);
