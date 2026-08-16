/*
 * command_surface.h — THE table of top-level CLI commands. Every end reads
 * this one.
 *
 * ─── Why one table, and what two would cost ────────────────────────────
 *
 * A top-level command has two independent facts about it:
 *
 *   1. it is DISPATCHED — handle_subcommand()'s if-chain (main.c) runs it.
 *   2. it takes a ROLE — hyp_daemon_process_role() (daemon/bootstrap.c) says
 *      whether the process needs a daemon, a coordinated CLI lease, or
 *      nothing at all.
 *
 * Split those across two hand-written enumerations and the failure mode is
 * silent in the worst possible direction: a command declared in (1) and
 * missing from (2) falls through to the client role, so the binary attaches to
 * a daemon and serves the MCP protocol on stdin. With stdin closed that is
 * exit 0 and no output — a command that appears to succeed while doing
 * nothing; with a terminal attached it is a hang. Neither is an error a user
 * can bisect, and neither end's own tests can see it, because each end is
 * correct in isolation.
 *
 * That failure is observed, not theorised: it reached users through a command
 * that downloads a model, and patching that one command per-instance left the
 * mechanism intact for the next one. A comment warning of a trap is not a
 * gate, which is why this is a table and not a warning.
 *
 * So the command surface is one table, and both ends EXPAND it:
 *
 *   - bootstrap.c generates the whole argv classifier from it. There is no
 *     second list of stateless commands, because there is no second list.
 *   - main.c generates handle_subcommand()'s if-chain from it. A row whose
 *     dispatch column says SUBCOMMAND must have a HYP_CLI_CMD_BODY_<id> macro,
 *     or the expansion leaves an undeclared identifier and the build fails
 *     NAMING THE ROW. A compile-time failure beats a test failure.
 *   - main.c generates the help listing from the `usage` column, so a command
 *     cannot be reachable and undocumented by accident. `usage` is NULL for
 *     the rows that are deliberately internal, which makes "not in help" a
 *     stated fact in the table rather than an omission nobody can see.
 *
 * ─── Fail closed, so the trap cannot reopen ────────────────────────────
 *
 * Expanding one table removes the drift between the two ends but not the
 * mechanism that made drift silent. That is the second half: an argument that
 * matches no row and is not one of the arguments an MCP client legitimately
 * carries (HYP_CLI_CLIENT_ARGUMENTS below) yields
 * HYP_DAEMON_PROCESS_UNKNOWN_COMMAND, and main() refuses by name. The MCP
 * client role is now something argv EARNS, never what is left over.
 *
 * A confident error beats a plausible wrong answer, and "attached to the
 * daemon and served nothing" is the most plausible-looking wrong answer this
 * binary had available.
 *
 * ─── Reading a row ─────────────────────────────────────────────────────
 *
 *   id        C identifier for the row. Tokens carry `-` and `--`, which
 *             cannot be pasted into an identifier, so the id is separate and
 *             it is what a build error names.
 *   token     the argv spelling, byte-exact.
 *   role      the hyp_daemon_process_role_t this token selects. Taken from
 *             what hyp_daemon_process_role() actually returns — the set is
 *             derived from the classifier, not invented for this table.
 *   help      HYP_CLI_HELP_DOWNGRADES when `--help` after the token turns the
 *             invocation into a STATELESS help print instead of the row's
 *             role; HYP_CLI_HELP_PLAIN when the token's role is unconditional.
 *   dispatch  where the command is actually run. SUBCOMMAND means
 *             handle_subcommand() in main.c; MAIN_ROLE means a role branch in
 *             main() owns it and handle_subcommand() must NOT — a branch there
 *             for a MAIN_ROLE token is unreachable by construction, because
 *             the classifier routes the token away from handle_subcommand()'s
 *             three callers before it can be consulted.
 *   usage     the help block for this command, or NULL for internal commands
 *             that help deliberately does not list.
 *
 * Row order is presentation order in `--help`. It carries no classification
 * meaning: tokens are pairwise distinct (asserted), so at most one row can
 * match any argument and the order in which rows are tried cannot change the
 * answer. What DOES matter is argument order — the first argument matching any
 * row decides the role, which is why `hyp cli search_code install` stays a
 * local CLI call and cannot be re-steered by its own opaque payload.
 */
#ifndef HYP_CLI_COMMAND_SURFACE_H
#define HYP_CLI_COMMAND_SURFACE_H

#include "daemon/bootstrap.h"

typedef enum {
    HYP_CLI_HELP_PLAIN = 0,
    HYP_CLI_HELP_DOWNGRADES,
} hyp_cli_help_mode_t;

typedef enum {
    HYP_CLI_DISPATCH_SUBCOMMAND = 0,
    HYP_CLI_DISPATCH_MAIN_ROLE,
} hyp_cli_dispatch_site_t;

/* clang-format off */
#define HYP_CLI_COMMAND_SURFACE(X)                                                                 \
    X(cli, "cli", HYP_DAEMON_PROCESS_LOCAL_CLI, HYP_CLI_HELP_DOWNGRADES,                           \
      HYP_CLI_DISPATCH_SUBCOMMAND,                                                                 \
      "  hyponoia cli [--progress] [--json] <tool> [args]\n"                                       \
      "                                      Run one tool locally, then exit\n")                   \
    X(install, "install", HYP_DAEMON_PROCESS_STATELESS, HYP_CLI_HELP_PLAIN,                        \
      HYP_CLI_DISPATCH_SUBCOMMAND,                                                                 \
      "  hyponoia install [-y|-n] [--force] [--dry-run] [--dir=<path>] [--skip-config]\n")         \
    X(uninstall, "uninstall", HYP_DAEMON_PROCESS_STATELESS, HYP_CLI_HELP_PLAIN,                    \
      HYP_CLI_DISPATCH_SUBCOMMAND, "  hyponoia uninstall [-y|-n] [--dry-run]\n")                   \
    X(update, "update", HYP_DAEMON_PROCESS_STATELESS, HYP_CLI_HELP_PLAIN,                          \
      HYP_CLI_DISPATCH_SUBCOMMAND, "  hyponoia update [-y|-n]\n")                                  \
    X(config, "config", HYP_DAEMON_PROCESS_LOCAL_CLI, HYP_CLI_HELP_DOWNGRADES,                     \
      HYP_CLI_DISPATCH_SUBCOMMAND, "  hyponoia config <list|get|set|reset>\n")                     \
    X(onboard, "onboard", HYP_DAEMON_PROCESS_STATELESS, HYP_CLI_HELP_PLAIN,                        \
      HYP_CLI_DISPATCH_SUBCOMMAND,                                                                 \
      "  hyponoia onboard [dir] [--answers <file>] [--yes]\n"                                      \
      "                                      Probe this machine and corpus, propose a\n"           \
      "                                      workspace, ask only what only you know\n")            \
    /* `embed` reads the graph database READ-ONLY through its own connection                       \
     * and writes only to <cache>/vectors/<project>.db, so it needs neither a                      \
     * daemon nor a project mutation lease — it cannot mutate the graph at                         \
     * all. Routing an opt-in second pass through the daemon would make it                         \
     * depend on state it never touches. */                                                        \
    X(embed, "embed", HYP_DAEMON_PROCESS_STATELESS, HYP_CLI_HELP_PLAIN,                            \
      HYP_CLI_DISPATCH_SUBCOMMAND,                                                                 \
      "  hyponoia embed --project <name> [--status]\n"                                             \
      "                                      Opt-in second pass: per-declaration\n"                \
      "                                      vectors for the `ask` lane\n")                        \
    /* `fetch-model` writes ONE file into the user's cache and reads no                            \
     * project, no index and no store. It also has to work when the daemon                         \
     * cannot start, which is exactly when someone is trying to get the `ask`                      \
     * lane running. */                                                                            \
    X(fetch_model, "fetch-model", HYP_DAEMON_PROCESS_STATELESS, HYP_CLI_HELP_PLAIN,                \
      HYP_CLI_DISPATCH_SUBCOMMAND,                                                                 \
      "  hyponoia fetch-model [-y] [--force] [--verify] [--path]\n"                                \
      "                                      Download the `ask` lane's embedding model\n")         \
    /* `allow-root` writes one line of user-level config and reads nothing                         \
     * from a project. Stateless rather than daemon-routed so enrolling a root                     \
     * cannot depend on daemon state. */                                                           \
    X(allow_root, "allow-root", HYP_DAEMON_PROCESS_STATELESS, HYP_CLI_HELP_PLAIN,                  \
      HYP_CLI_DISPATCH_SUBCOMMAND,                                                                 \
      "  hyponoia allow-root [--approve-sensitive] [--list] <path>\n"                              \
      "                                      Record a directory indexing may enter\n")             \
    X(daemon, "daemon", HYP_DAEMON_PROCESS_DAEMON_CTL, HYP_CLI_HELP_DOWNGRADES,                    \
      HYP_CLI_DISPATCH_MAIN_ROLE,                                                                  \
      "  hyponoia daemon <start|stop|status> [--open] [--port=N]\n"                                \
      "                                      Keep a daemon warm for CLI calls and hooks\n")        \
    X(version, "--version", HYP_DAEMON_PROCESS_STATELESS, HYP_CLI_HELP_PLAIN,                      \
      HYP_CLI_DISPATCH_SUBCOMMAND, "  hyponoia --version    Print version\n")                      \
    X(help, "--help", HYP_DAEMON_PROCESS_STATELESS, HYP_CLI_HELP_PLAIN,                            \
      HYP_CLI_DISPATCH_SUBCOMMAND, "  hyponoia --help       Print this help\n")                    \
    /* `-h` is `--help` under another spelling; help prints one line, not two. */                  \
    X(help_short, "-h", HYP_DAEMON_PROCESS_STATELESS, HYP_CLI_HELP_PLAIN,                          \
      HYP_CLI_DISPATCH_SUBCOMMAND, NULL)                                                           \
    /* Written by the installer and read by nobody who types commands. */                          \
    X(verify_runtime_assets, "--verify-runtime-assets", HYP_DAEMON_PROCESS_STATELESS,              \
      HYP_CLI_HELP_PLAIN, HYP_CLI_DISPATCH_SUBCOMMAND, NULL)                                       \
    /* A hook adapter's entry point: spawned by an agent harness, contractually                    \
     * fail-open, and served by main()'s HOOK_CLIENT branch rather than by                         \
     * handle_subcommand(). */                                                                     \
    X(hook_augment, "hook-augment", HYP_DAEMON_PROCESS_HOOK_CLIENT, HYP_CLI_HELP_PLAIN,            \
      HYP_CLI_DISPATCH_MAIN_ROLE, NULL)
/* clang-format on */

/* Row count, for a _Static_assert that pairs this table against any array
 * sized from it. */
#define HYP_CLI_COMMAND_SURFACE_ROW_COUNT_ONE(id, token, role, help, dispatch, usage) +1
#define HYP_CLI_COMMAND_SURFACE_ROW_COUNT \
    (0 HYP_CLI_COMMAND_SURFACE(HYP_CLI_COMMAND_SURFACE_ROW_COUNT_ONE))

typedef enum {
    /* Matched whole, carries no value. */
    HYP_CLI_ARG_EXACT = 0,
    /* Matched whole; the NEXT argument is its value and is consumed with it. */
    HYP_CLI_ARG_VALUE,
    /* Matched as a prefix; the value is the remainder of the same argument. */
    HYP_CLI_ARG_PREFIX,
} hyp_cli_argument_kind_t;

/*
 * The complete argv an MCP-client process may carry. This table is what makes
 * the client role EARNED rather than left over: an argument matching neither a
 * command row nor a row here is an unclassified command, and the process
 * refuses instead of quietly serving the protocol.
 *
 * Every entry is here because a path in this binary reads it on the client
 * leg — the tool-profile forms are what the generated agent configurations
 * write, and the UI forms are what main() applies after a successful HELLO.
 * Value validity is not this table's job: `--tool-profile=nonsense` is a
 * client argument shape, and the profile parser is what refuses the value.
 */
#define HYP_CLI_CLIENT_ARGUMENTS(X)          \
    X("--profile", HYP_CLI_ARG_EXACT)        \
    X("--tool-profile", HYP_CLI_ARG_VALUE)   \
    X("--tool-profile=", HYP_CLI_ARG_PREFIX) \
    X("--ui=", HYP_CLI_ARG_PREFIX)           \
    X("--port=", HYP_CLI_ARG_PREFIX)

#endif /* HYP_CLI_COMMAND_SURFACE_H */
