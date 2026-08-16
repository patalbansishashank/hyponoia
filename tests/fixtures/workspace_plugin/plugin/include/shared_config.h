#ifndef SHARED_CONFIG_H
#define SHARED_CONFIG_H

/* Deliberately duplicated in the `bridge` member. Two members declaring and
 * defining one name under one filename is what an ambiguous crossing looks
 * like from the graph. */
int config_load(const char *scope);

#endif
