/*
 * asset_pack.h — Verified external frontend assets.
 *
 * UI builds embed only a content-addressed pack name, exact size and SHA-256.
 * The uncompressed pack stays outside the native executable and is read once
 * into an immutable process-lifetime snapshot before the HTTP listener starts.
 */
#ifndef HYP_UI_ASSET_PACK_H
#define HYP_UI_ASSET_PACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cli/activation_transaction.h"

typedef enum {
    HYP_UI_ASSETS_UNAVAILABLE = 0,
    HYP_UI_ASSETS_COLD,
    HYP_UI_ASSETS_LOADING,
    HYP_UI_ASSETS_READY,
    HYP_UI_ASSETS_FAILED,
    HYP_UI_ASSETS_CANCELLED,
} hyp_ui_assets_state_t;

typedef enum {
    HYP_UI_ASSET_REVALIDATE = 1,
    HYP_UI_ASSET_IMMUTABLE = 2,
} hyp_ui_asset_cache_t;

typedef struct {
    const char *path;
    const unsigned char *data;
    size_t size;
    const char *content_type;
    hyp_ui_asset_cache_t cache;
} hyp_ui_asset_t;

/* Defined by asset_pack_stub.c for standard builds, asset_manifest_stub.c for
 * test runners, or the generated manifest for UI builds. No frontend payload
 * bytes live in any of them. */
extern const char HYP_UI_ASSET_PACK_NAME[];
extern const char HYP_UI_ASSET_SHA256[];
extern const uint64_t HYP_UI_ASSET_SIZE;

/* Compile-time capability; performs no filesystem I/O and is true before the
 * asynchronous warmup starts. */
bool hyp_ui_assets_supported(void);
const char *hyp_ui_assets_current_pack_name(void);

/* Exact binary path used to resolve the adjacent and package-share candidates.
 * Call before starting warmup. */
void hyp_ui_assets_set_binary_path(const char *path);

/* Resolve, hash and structurally validate the complete pack, then publish one
 * immutable lookup snapshot. A standard build is a successful no-op. */
bool hyp_ui_assets_warm(const char *home, char *err, size_t err_sz);
void hyp_ui_assets_request_cancel(void);
hyp_ui_assets_state_t hyp_ui_assets_state(void);

/* Returns NULL until the complete snapshot is READY. The returned view remains
 * valid for the process lifetime. */
const hyp_ui_asset_t *hyp_ui_asset_lookup(const char *path);

/* Direct CLI install publishes the verified content-addressed pack beside the
 * target executable before that executable becomes active. Uninstall removes
 * only an exact hash-verified adjacent pack. */
bool hyp_ui_assets_remove(const char *install_dir, bool dry_run, char *err, size_t err_sz);

/* Verify one explicit pack against this binary's compiled name/size/hash and
 * parser contract, without publishing a global snapshot. Standard builds
 * accept no pack and return true only through the supported() no-op paths. */
bool hyp_ui_assets_verify_file(const char *path);

/* Stage removal of the exact owned adjacent pack. Missing/standard packs are
 * successful no-ops; a foreign file is preserved and reported separately. */
bool hyp_ui_assets_stage_remove(const char *install_dir,
                                hyp_activation_transaction_t **transaction_out,
                                bool *foreign_preserved_out, char *err, size_t err_sz);

/* Commit an owned-pack removal only after the exact retained object (moved out
 * of the public name by the transaction) still verifies against this binary.
 * A same-inode rewrite after staging is rolled back and preserved. */
hyp_activation_transaction_status_t hyp_ui_assets_commit_removal(
    hyp_activation_transaction_t *transaction);

/* Stage the current verified content-addressed pack without publishing it.
 * Standard builds return true with a NULL transaction. UI callers retain the
 * transaction through the coordinated binary commit, then finalize/rollback
 * it while still holding the native activation guard. */
bool hyp_ui_assets_stage_install(const char *install_dir,
                                 hyp_activation_transaction_t **transaction_out, char *err,
                                 size_t err_sz);

/* Publish a staged pack with a post-rename parser/hash validator, binding the
 * verification to the bytes at the final public name before finalization. */
hyp_activation_transaction_status_t hyp_ui_assets_commit_install(
    hyp_activation_transaction_t *transaction);

#ifdef HYP_CLI_ENABLE_TEST_API
/* Focused lifecycle probe. Production publishes through stage_install as part
 * of the native guarded runtime-set transaction. */
bool hyp_ui_assets_install(const char *install_dir, bool dry_run, char *err, size_t err_sz);

/* Single-threaded test seams for exercising the parser with caller-created
 * fixtures whose digest is recomputed after each structural mutation. */
void hyp_ui_assets_set_manifest_for_testing(const char *name, const char *sha256, uint64_t size);
void hyp_ui_assets_reset_for_testing(void);
#endif

#endif /* HYP_UI_ASSET_PACK_H */
