#ifndef PLUGIN_API_H
#define PLUGIN_API_H

/* The plugin ABI the host compiles against. `host` includes this file by name
 * from its own repository; the file lives here. */
void plugin_register(const char *name, int slot);

#endif
