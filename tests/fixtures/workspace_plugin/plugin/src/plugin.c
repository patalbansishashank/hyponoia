#include "plugin_api.h"
#include "shared_config.h"

static int plugin_slots = 0;

void plugin_register(const char *name, int slot) {
    (void)name;
    plugin_slots += slot;
}

int config_load(const char *scope) {
    (void)scope;
    return 1;
}
