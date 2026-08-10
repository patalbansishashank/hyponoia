/*
 * ask_cmd.h — the `hyponoia embed` subcommand.
 *
 * Declared in its own header rather than in cli/cli.h so that adding the
 * opt-in embed lane costs the shared CLI header nothing.
 */
#ifndef HYP_ASK_CMD_H
#define HYP_ASK_CMD_H

/* argv here is the tail AFTER the `embed` token, matching how main.c hands
 * arguments to every other subcommand. Returns a process exit code. */
int hyp_cmd_embed(int argc, char **argv);

#endif /* HYP_ASK_CMD_H */
