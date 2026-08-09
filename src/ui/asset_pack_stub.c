/* Standard builds have no frontend pack and perform no UI asset I/O. */
#include "ui/asset_pack.h"

const char HYP_UI_ASSET_PACK_NAME[] = "";
const char HYP_UI_ASSET_SHA256[] = "";
const uint64_t HYP_UI_ASSET_SIZE = UINT64_C(0);

bool hyp_ui_assets_supported(void) {
    return false;
}

const char *hyp_ui_assets_current_pack_name(void) {
    return NULL;
}

void hyp_ui_assets_set_binary_path(const char *path) {
    (void)path;
}

bool hyp_ui_assets_warm(const char *home, char *err, size_t err_sz) {
    (void)home;
    (void)err;
    (void)err_sz;
    return true;
}

void hyp_ui_assets_request_cancel(void) {}

hyp_ui_assets_state_t hyp_ui_assets_state(void) {
    return HYP_UI_ASSETS_UNAVAILABLE;
}

const hyp_ui_asset_t *hyp_ui_asset_lookup(const char *path) {
    (void)path;
    return NULL;
}

#ifdef HYP_CLI_ENABLE_TEST_API
bool hyp_ui_assets_install(const char *install_dir, bool dry_run, char *err, size_t err_sz) {
    (void)install_dir;
    (void)dry_run;
    (void)err;
    (void)err_sz;
    return true;
}
#endif

bool hyp_ui_assets_remove(const char *install_dir, bool dry_run, char *err, size_t err_sz) {
    (void)install_dir;
    (void)dry_run;
    (void)err;
    (void)err_sz;
    return true;
}

bool hyp_ui_assets_verify_file(const char *path) {
    (void)path;
    return false;
}

bool hyp_ui_assets_stage_remove(const char *install_dir,
                                hyp_activation_transaction_t **transaction_out,
                                bool *foreign_preserved_out, char *err, size_t err_sz) {
    (void)install_dir;
    (void)err;
    (void)err_sz;
    if (transaction_out) {
        *transaction_out = NULL;
    }
    if (foreign_preserved_out) {
        *foreign_preserved_out = false;
    }
    return transaction_out != NULL;
}

bool hyp_ui_assets_stage_install(const char *install_dir,
                                 hyp_activation_transaction_t **transaction_out, char *err,
                                 size_t err_sz) {
    (void)install_dir;
    (void)err;
    (void)err_sz;
    if (transaction_out) {
        *transaction_out = NULL;
    }
    return transaction_out != NULL;
}

hyp_activation_transaction_status_t hyp_ui_assets_commit_install(
    hyp_activation_transaction_t *transaction) {
    return transaction ? HYP_ACTIVATION_TRANSACTION_INVALID_ARGUMENT
                       : HYP_ACTIVATION_TRANSACTION_OK;
}

hyp_activation_transaction_status_t hyp_ui_assets_commit_removal(
    hyp_activation_transaction_t *transaction) {
    return transaction ? HYP_ACTIVATION_TRANSACTION_INVALID_ARGUMENT
                       : HYP_ACTIVATION_TRANSACTION_OK;
}
