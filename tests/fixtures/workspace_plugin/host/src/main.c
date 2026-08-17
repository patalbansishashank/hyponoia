/*
 * host — the plugin HOST. It is one member of a workspace whose other members
 * are `plugin`, `bridge` and `widgets`. Every crossing below is a real one:
 * this file is compiled against headers that live in a different repository.
 */
#include "plugin_api.h"
#include "shared_config.h"
#include "util.h"

/* THE UNIT. plugin_register is declared in plugin/include/plugin_api.h and
 * defined in plugin/src/plugin.c — a different member, reached by a direct
 * source-level call with no service boundary anywhere between them. */
int host_boot(void) {
    int slot = host_reserve_slot();
    plugin_register("hello", slot);
    return slot;
}

/* NEGATIVE CONTROL 1 — looks intra-workspace, is not.
 * `render` is defined exactly once in the whole workspace, at top level, in the
 * `widgets` member. This file includes nothing from `widgets`. A matcher that
 * rendezvouses on the callee name alone mints an edge here; the crossing was
 * never declared, so there must be none. */
int host_draw(void) {
    return render();
}

/* AMBIGUITY — two members answer, so nobody does.
 * shared_config.h exists in BOTH `plugin` and `bridge`, and both declare and
 * define config_load. Two members satisfy every condition, so the call is
 * reported as ambiguous and no edge is written. */
int host_configure(void) {
    return config_load("host");
}
