/*
 * sync_cmd.c — `hyponoia sync`, the manual case.
 *
 * The ONLY sync surface a caller can reach on purpose, and it is a command a
 * person types. Automatic sync belongs to the watcher (see sync.h): it fires
 * when a repository changes locally, because that is when the memory is about
 * to be needed. There is deliberately no MCP tool — an agent-callable sync
 * would make being up to date depend on an agent remembering to ask, and the
 * agent that most needs the records is the one least able to know it is
 * missing them.
 */
#include "memory/sync.h"

#include "foundation/constants.h"

#include <stdio.h>
#include <string.h>

enum { SYNC_CMD_PATH_MAX = HYP_SZ_4K };

static void sync_cmd_usage(void) {
    (void)fprintf(stderr,
                  "Usage: hyponoia sync [--pull|--push] [--local <dir>] [--peer <dir>] [--json]\n"
                  "\n"
                  "  Merge this machine's record store with a peer's. Push is\n"
                  "  \"merge mine into theirs\", pull is the reverse; with neither\n"
                  "  flag both run, and the result is the union either way.\n"
                  "\n"
                  "  --local <dir>  this machine's store (default: HYP_MEMORY_DIR,\n"
                  "                 else <cache>/memory)\n"
                  "  --peer  <dir>  the store to exchange with (default: HYP_MEMORY_PEER)\n"
                  "  --json         one JSON object instead of prose\n");
}

static const char *sync_cmd_direction_name(hyp_sync_direction_t direction) {
    switch (direction) {
    case HYP_SYNC_PULL:
        return "pull";
    case HYP_SYNC_PUSH:
        return "push";
    case HYP_SYNC_BOTH:
        return "both";
    }
    return "both";
}

static const char *sync_cmd_side_name(hyp_sync_side_t side) {
    switch (side) {
    case HYP_SYNC_SIDE_LOCAL:
        return "local";
    case HYP_SYNC_SIDE_PEER:
        return "peer";
    case HYP_SYNC_SIDE_NONE:
        return "none";
    }
    return "none";
}

/* JSON strings here are digests (hex), fixed vocabulary words and a detail
 * string that can hold a path, so the only character needing care is the
 * quote. Escape it rather than emit invalid JSON a client would guess at. */
static void sync_cmd_print_escaped(const char *s) {
    for (const char *p = s; *p; p++) {
        if (*p == '"' || *p == '\\') {
            (void)printf("\\%c", *p);
        } else if ((unsigned char)*p < 0x20) {
            (void)printf("\\u%04x", (unsigned)(unsigned char)*p);
        } else {
            (void)putchar(*p);
        }
    }
}

static void sync_cmd_print_json(hyp_sync_status_t status, const hyp_sync_result_t *result) {
    (void)printf("{\"status\":\"%s\",\"direction\":\"%s\"",
                 status == HYP_SYNC_OK ? "ok" : "refused",
                 sync_cmd_direction_name(result->direction));
    if (status == HYP_SYNC_OK) {
        (void)printf(",\"agreed\":%s,\"pulled\":%zu,\"pushed\":%zu",
                     strcmp(result->local_digest, result->peer_digest) == 0 ? "true" : "false",
                     result->pulled, result->pushed);
        (void)printf(",\"local_digest\":\"%s\",\"peer_digest\":\"%s\"", result->local_digest,
                     result->peer_digest);
    } else {
        (void)printf(",\"reason\":\"");
        sync_cmd_print_escaped(hyp_sync_status_reason(status));
        (void)printf("\",\"side\":\"%s\",\"store_status\":\"",
                     sync_cmd_side_name(result->failed_side));
        sync_cmd_print_escaped(hyp_record_store_status_reason(result->store_status));
        (void)printf("\",\"detail\":\"");
        sync_cmd_print_escaped(result->detail);
        (void)printf("\"");
    }
    (void)printf("}\n");
}

int hyp_cmd_sync(int argc, char **argv) {
    hyp_sync_direction_t direction = HYP_SYNC_BOTH;
    const char *local_arg = NULL;
    const char *peer_arg = NULL;
    bool json = false;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--pull") == 0) {
            direction = HYP_SYNC_PULL;
        } else if (strcmp(argv[i], "--push") == 0) {
            direction = HYP_SYNC_PUSH;
        } else if (strcmp(argv[i], "--json") == 0) {
            json = true;
        } else if (strcmp(argv[i], "--local") == 0 && i + 1 < argc) {
            local_arg = argv[++i];
        } else if (strcmp(argv[i], "--peer") == 0 && i + 1 < argc) {
            peer_arg = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            sync_cmd_usage();
            return 0;
        } else {
            (void)fprintf(stderr, "hyponoia sync: unrecognized argument: %s\n", argv[i]);
            sync_cmd_usage();
            return 2;
        }
    }

    char local_dir[SYNC_CMD_PATH_MAX];
    if (local_arg) {
        (void)snprintf(local_dir, sizeof(local_dir), "%s", local_arg);
    } else if (!hyp_memory_store_dir(local_dir, sizeof(local_dir))) {
        (void)fprintf(stderr, "hyponoia sync: cannot resolve the local record store directory\n");
        return 1;
    }
    char peer_dir[SYNC_CMD_PATH_MAX];
    if (peer_arg) {
        (void)snprintf(peer_dir, sizeof(peer_dir), "%s", peer_arg);
    } else if (!hyp_memory_peer_dir(peer_dir, sizeof(peer_dir))) {
        /* Absent, not empty: there is no peer, which is a different answer
         * from "the peer had nothing". Say which one it is. */
        (void)fprintf(stderr, "hyponoia sync: no peer configured — pass --peer <dir> or set "
                              "HYP_MEMORY_PEER\n");
        return 1;
    }

    hyp_sync_result_t result;
    hyp_sync_status_t status = hyp_sync_dirs(local_dir, peer_dir, direction, &result);

    if (json) {
        sync_cmd_print_json(status, &result);
        return status == HYP_SYNC_OK ? 0 : 1;
    }
    if (status != HYP_SYNC_OK) {
        (void)fprintf(stderr, "hyponoia sync: refused: %s\n", hyp_sync_status_reason(status));
        if (result.failed_side != HYP_SYNC_SIDE_NONE) {
            (void)fprintf(stderr, "  side: %s (%s)\n", sync_cmd_side_name(result.failed_side),
                          hyp_record_store_status_reason(result.store_status));
        }
        if (result.detail[0]) {
            (void)fprintf(stderr, "  %s\n", result.detail);
        }
        return 1;
    }

    (void)printf("local %s\n", local_dir);
    (void)printf("peer  %s\n", peer_dir);
    if (result.agreed_before) {
        /* Not "0 behind" — a union has no total order and nothing is counted
         * against a position. The two stores hold the same records. */
        (void)printf("in agreement, nothing to merge (%s)\n", result.local_digest);
        return 0;
    }
    (void)printf("%s: pulled %zu, pushed %zu\n", sync_cmd_direction_name(direction), result.pulled,
                 result.pushed);
    if (strcmp(result.local_digest, result.peer_digest) == 0) {
        (void)printf("in agreement (%s)\n", result.local_digest);
    } else {
        (void)printf("local  %s\npeer   %s\n", result.local_digest, result.peer_digest);
    }
    return 0;
}
