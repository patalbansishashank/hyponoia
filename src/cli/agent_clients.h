/*
 * agent_clients.h — Table-driven agent client MCP installation profiles.
 */
#ifndef HYP_CLI_AGENT_CLIENTS_H
#define HYP_CLI_AGENT_CLIENTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HYP_AGENT_CLIENT_QODER = 0,
    HYP_AGENT_CLIENT_KIMI,
    HYP_AGENT_CLIENT_GITLAB_DUO,
    HYP_AGENT_CLIENT_ROVO_DEV,
    HYP_AGENT_CLIENT_AMP,
    HYP_AGENT_CLIENT_DEVIN,
    HYP_AGENT_CLIENT_TABNINE,
    HYP_AGENT_CLIENT_CONTINUE,
    HYP_AGENT_CLIENT_VISUAL_STUDIO,
    HYP_AGENT_CLIENT_TRAE,
    HYP_AGENT_CLIENT_ROO_CODE,
    HYP_AGENT_CLIENT_AMAZON_Q,
    HYP_AGENT_CLIENT_CODEBUDDY,
    HYP_AGENT_CLIENT_IBM_BOB_IDE,
    HYP_AGENT_CLIENT_IBM_BOB_SHELL,
    HYP_AGENT_CLIENT_POCHI,
    HYP_AGENT_CLIENT_PI,
    HYP_AGENT_CLIENT_SOURCEGRAPH_CODY,
    HYP_AGENT_CLIENT_COUNT
} hyp_agent_client_id_t;

typedef enum {
    HYP_AGENT_STABLE = 0,
    HYP_AGENT_CONDITIONAL,
    HYP_AGENT_OPT_IN
} hyp_agent_client_stability_t;

enum {
    HYP_AGENT_CAP_MCP = UINT32_C(1) << 0,
    HYP_AGENT_CAP_INSTRUCTIONS = UINT32_C(1) << 1,
    HYP_AGENT_CAP_SKILL = UINT32_C(1) << 2,
    HYP_AGENT_CAP_AGENT = UINT32_C(1) << 3,
    HYP_AGENT_CAP_HOOK = UINT32_C(1) << 4,
    HYP_AGENT_CAP_PLUGIN = UINT32_C(1) << 5
};

typedef int (*hyp_agent_mcp_edit_fn)(hyp_agent_client_id_t id, const char *config_path,
                                     const char *binary_path);

typedef struct {
    hyp_agent_client_id_t id;
    const char *stable_id;
    const char *display_name;
    hyp_agent_client_stability_t stability;
    uint32_t capabilities;
    const char *detection_command;
    hyp_agent_mcp_edit_fn install_mcp;
    hyp_agent_mcp_edit_fn remove_mcp;
} hyp_agent_client_profile_t;

typedef bool (*hyp_agent_probe_fn)(const char *value, const void *context);

typedef struct {
    const char *home_dir;
    const char *xdg_config_home;
    const char *appdata_dir;
    const char *glab_config_dir;
    const char *kimi_code_home;
    const char *continue_config_path;
    const char *trae_config_path;
    const char *roo_config_path;
    const char *cody_config_path;
    bool is_windows;
    hyp_agent_probe_fn path_exists;
    hyp_agent_probe_fn command_exists;
    const void *probe_context;
} hyp_agent_client_resolve_options_t;

enum {
    HYP_AGENT_EDIT_ERROR = -1,
    HYP_AGENT_EDIT_OK = 0,
    HYP_AGENT_EDIT_FOREIGN = 1,
    HYP_AGENT_EDIT_NOT_APPLICABLE = 2
};

size_t hyp_agent_client_count(void);
const hyp_agent_client_profile_t *hyp_agent_client_at(size_t index);
const hyp_agent_client_profile_t *hyp_agent_client_by_id(hyp_agent_client_id_t id);
const hyp_agent_client_profile_t *hyp_agent_client_by_stable_id(const char *stable_id);

/* Resolves the documented user config path. Returns 0 on success, 1 when a
 * conditional target has no safe active path, and -1 for invalid input or an
 * ambiguous/unsupported configuration. */
int hyp_agent_client_resolve_path(hyp_agent_client_id_t id,
                                  const hyp_agent_client_resolve_options_t *options, char *path_out,
                                  size_t path_out_size);
bool hyp_agent_client_detect(hyp_agent_client_id_t id,
                             const hyp_agent_client_resolve_options_t *options);
bool hyp_agent_client_cleanup_candidate(hyp_agent_client_id_t id,
                                        const hyp_agent_client_resolve_options_t *options);

/* config_path must already have been resolved. The adapter never guesses a
 * target here. Existing same-name foreign entries fail closed with
 * HYP_AGENT_EDIT_FOREIGN. Removal requires the original installed binary path
 * and only removes the still-canonical entry. */
int hyp_agent_client_install_mcp(hyp_agent_client_id_t id, const char *config_path,
                                 const char *binary_path);
int hyp_agent_client_remove_mcp(hyp_agent_client_id_t id, const char *config_path,
                                const char *binary_path);

#ifdef __cplusplus
}
#endif

#endif /* HYP_CLI_AGENT_CLIENTS_H */
