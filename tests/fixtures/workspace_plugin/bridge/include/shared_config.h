#ifndef SHARED_CONFIG_H
#define SHARED_CONFIG_H

/* The second answer. `host` includes "shared_config.h" and calls config_load;
 * this member and `plugin` are indistinguishable from the call site. */
int config_load(const char *scope);

#endif
