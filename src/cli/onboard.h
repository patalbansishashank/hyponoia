/*
 * onboard.h — onboarding (NEXT-STEPS §4 Phase 1, Track B).
 *
 * Detect, propose, confirm. Never interrogate. The four units live here
 * together because they share one vocabulary and one plan struct:
 *
 *   B1  hyp_onboard_probe        what this machine, this build and this corpus
 *                                actually are — measured, never asked.
 *   B2  hyp_onboard_flow         the proposal and the FOUR human questions.
 *   B3  hyp_onboard_toml_write   the record of that conversation. Optional,
 *                                never a prerequisite — B4 is the proof.
 *   B4  hyp_onboard_resolve      the zero-config path: no TOML, no Multica,
 *                                no flags -> this repo, workspace of one, role
 *                                member, local nano, CPU, no shared feeds.
 *
 * ── The GPU answer is a RUN, not a belief ──
 *
 * §3 retracted a GPU path whose batched vectors were wrong, and the failure
 * survived a CPU-vs-GPU comparison because both sides were batched the same
 * wrong way. So "does GPU work" is answered by RUNNING the batching self-check
 * (`hyponoia embed --verify-batching`'s exact machinery,
 * hyp_ask_encoder_check_batching) on an encoder created on the GPU of THIS
 * build. A build with HYP_ASK_GPU=none reports that about itself; a card that
 * cannot be created on reports the refusal; only a check that ran and passed
 * reports VERIFIED. The report carries the check counts so a verdict without
 * its evidence is a shape the tests reject.
 *
 * ── The key variable ──
 *
 * Config stores the NAME of an environment variable, never a key (cli.h,
 * HYP_CONFIG_ASK_ESC_KEY_ENV). The probe reports whether that NAME is set in
 * this process's environment. The VALUE is never read into a buffer, never
 * printed, never stored — the only expression that touches it is a NULL check.
 */
#ifndef HYP_CLI_ONBOARD_H
#define HYP_CLI_ONBOARD_H

#include "ask/ask_encoder.h"
#include "foundation/constants.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ── Roles (Track A's vocabulary; A5: unknown reads as `reference`) ── */

/* member is index-and-editable; reference is the A5 default for undeclared
 * intent because wrong-but-read-only is recoverable and wrong-but-editable
 * means an agent rewrites a dependency. The other three are DERIVED by A2-A4
 * and appear here only so a TOML that declares them round-trips. */
#define HYP_ONBOARD_ROLE_MEMBER "member"
#define HYP_ONBOARD_ROLE_REFERENCE "reference"

bool hyp_onboard_role_valid(const char *role);

/* ── B1 · The probe ─────────────────────────────────────────────── */

typedef enum {
    /* This binary has no inference backend at all; nothing could be run. */
    HYP_ONBOARD_GPU_NOT_IN_BUILD = 0,
    /* Backend present, weights absent: the check CANNOT RUN, so capability is
     * unknown — which is reported as unknown, never as yes. */
    HYP_ONBOARD_GPU_NO_MODEL,
    /* The encoder refused to come up on a GPU (no device, no readable VRAM
     * ceiling, or HYP_ASK_GPU=none in this build's backend). */
    HYP_ONBOARD_GPU_UNUSABLE,
    /* The batching check RAN on a GPU encoder and FAILED — the §3 failure
     * class, caught before anything is indexed with it. */
    HYP_ONBOARD_GPU_FAILED,
    /* The batching check ran on a GPU encoder and passed. The only state that
     * proposes the GPU, and it requires gpu_check_ran. */
    HYP_ONBOARD_GPU_VERIFIED,
} hyp_onboard_gpu_t;

const char *hyp_onboard_gpu_str(hyp_onboard_gpu_t g);

typedef struct {
    const char *repo_path; /* required */
    /* Test seam: run the GPU check against THIS encoder instead of creating
     * the real one. Borrowed, not owned. The verdict still comes from RUNNING
     * the check — the seam replaces the encoder, never the evidence. */
    hyp_ask_encoder_t *encoder;
    /* 0 = default sample size for the cost measurement. */
    int sample_files;
} hyp_onboard_probe_opts_t;

typedef struct {
    /* gpu — the verdict AND its evidence, together. */
    hyp_onboard_gpu_t gpu;
    bool gpu_check_ran;              /* the batching check actually executed */
    hyp_ask_batch_check_t gpu_check; /* zeroed unless gpu_check_ran */
    char gpu_device[HYP_SZ_128];     /* device_note of the encoder that ran */
    char gpu_evidence[HYP_SZ_512];   /* what was run/observed, as a sentence */

    /* key variable — NAME only, set/unset only. */
    bool key_env_named;            /* config names a variable at all */
    char key_env_name[HYP_SZ_128]; /* the NAME (empty when !key_env_named) */
    bool key_env_set;              /* getenv(name) != NULL — value untouched */

    /* corpus */
    bool corpus_walked;
    int files;
    int64_t bytes;
    double discover_ms; /* measured wall clock of the real walk */
    /* Span count is a property of an index, so it exists only after one.
     * ABSENT (spans_known false) means "look after the first index", never 0 —
     * absent and empty are different answers and only one is ever true. */
    bool spans_known;
    int64_t spans;
    char spans_method[HYP_SZ_256];

    /* first-index cost — measured where an encoder exists, and the method is
     * always stated so a constant is never mistaken for a measurement. */
    bool encode_measured;
    double encode_docs_per_sec;
    char encode_method[HYP_SZ_512];
    bool embed_estimate_known;
    double embed_seconds; /* docs_estimate / docs_per_sec, per cost_method */
    char cost_method[HYP_SZ_512];
} hyp_onboard_probe_t;

/* Run every probe. Returns 0 on success, -1 when repo_path is unusable.
 * Individual probes that cannot run report that in their own fields rather
 * than failing the whole report — a corpus walk that works is worth having
 * even when the GPU check cannot run. */
int hyp_onboard_probe(const hyp_onboard_probe_opts_t *opts, hyp_onboard_probe_t *out);

/* The machine-readable report. Heap JSON, caller frees. Unknowable numbers are
 * ABSENT keys, never 0. */
char *hyp_onboard_probe_json(const hyp_onboard_probe_t *p);

/* ── The plan (B2's outcome, B3's input) ────────────────────────── */

enum { HYP_ONBOARD_MAX_REPOS = 32 };

typedef struct {
    char path[HYP_PATH_MAX];
    char role[HYP_SZ_32];
} hyp_onboard_repo_t;

typedef struct {
    char name[HYP_SZ_128]; /* workspace name — a C1 slug */
    hyp_onboard_repo_t repos[HYP_ONBOARD_MAX_REPOS];
    int repo_count;
    /* SHOWN, editable, never asked. */
    char device[HYP_SZ_8];    /* "cpu" | "gpu" — gpu only over a VERIFIED probe */
    bool spend;               /* the escalation lane may spend money */
    char key_env[HYP_SZ_128]; /* NAME of an env var, never a value */
    bool write_toml;          /* record the plan in HYP_ONBOARD_TOML_NAME */
} hyp_onboard_plan_t;

/* ── B2 · The flow ──────────────────────────────────────────────── */

/* THE four genuinely human questions, as a table so "never interrogate" is a
 * testable property: a fifth question is a fifth row, and the count is
 * asserted. Everything else is derived by the probe/resolver and only SHOWN. */
typedef enum {
    HYP_ONBOARD_Q_REPOS = 0, /* which repos belong together */
    HYP_ONBOARD_Q_REFERENCE, /* which of them are `reference` */
    HYP_ONBOARD_Q_SPEND,     /* whether to spend money */
    HYP_ONBOARD_Q_NAME,      /* what to call it */
    HYP_ONBOARD_Q_COUNT,
} hyp_onboard_q_t;

const char *hyp_onboard_question_key(hyp_onboard_q_t q);
const char *hyp_onboard_question_prompt(hyp_onboard_q_t q);

/* Answers to the flow. NULL means "accept the proposal" — the propose-confirm
 * default, so an empty answers struct is the zero-question path. The last
 * three are EDITS to shown values, not questions; they exist because "show
 * everything else, editable" needs a seam to edit through. */
typedef struct {
    const char *repos;      /* extra repo paths, comma-separated */
    const char *reference;  /* which paths are `reference`, comma-separated */
    const char *spend;      /* "yes" | "no" */
    const char *name;       /* workspace name */
    const char *device;     /* edit: "cpu" | "gpu" (gpu refused unless VERIFIED) */
    const char *key_env;    /* edit: the NAME of the key variable */
    const char *write_toml; /* edit: "yes" | "no" (default yes) */
} hyp_onboard_answers_t;

/* Proposal + answers -> plan. Pure over its inputs: prompting (if any) is the
 * CLI's job, so tests drive this non-interactively. Fails closed: an edit that
 * asserts hardware the probe did not verify, an unknown repo in `reference`,
 * or an invalid name is an error, never a silent correction. */
int hyp_onboard_flow(const char *repo_dir, const hyp_onboard_probe_t *probe,
                     const hyp_onboard_answers_t *answers, hyp_onboard_plan_t *out_plan, char *err,
                     size_t errlen);

/* ── B3 · The TOML ──────────────────────────────────────────────── */

/* The record's file name. ONE symbol so A6's resolver and this writer cannot
 * drift; if A6 lands a different name, this is the only token to change. */
#define HYP_ONBOARD_TOML_NAME "hyponoia.toml"

/* Write `plan` as A6's read shape — `name` plus [[repos]] path/role — into
 * <dir>/HYP_ONBOARD_TOML_NAME. Deterministic rendering; running it twice is
 * idempotent (an existing byte-identical file is left untouched). The plan's
 * shown-not-asked fields (device, spend, key_env) are deliberately NOT
 * recorded here: they are config, and the config database already refuses
 * secret-shaped values — a second store for them would be a second place to
 * disagree. Returns 0 on success. */
int hyp_onboard_toml_write(const char *dir, const hyp_onboard_plan_t *plan, char *err,
                           size_t errlen);

/* Read <dir>/HYP_ONBOARD_TOML_NAME. Returns 1 when present and parsed, 0 when
 * absent (a first-class answer: the zero-config path), -1 on a file that
 * exists but cannot be honoured. Unknown role values read as `reference`
 * (A5); unknown keys are ignored so a future A6 field does not break an old
 * binary. */
int hyp_onboard_toml_read(const char *dir, hyp_onboard_plan_t *out, char *err, size_t errlen);

/* ── B4 · Zero config ───────────────────────────────────────────── */

typedef struct {
    char workspace[HYP_SZ_256]; /* derived deterministically, never typed */
    bool workspace_of_one;
    char source[HYP_SZ_16]; /* "defaults" | "toml" */
    hyp_onboard_repo_t repos[HYP_ONBOARD_MAX_REPOS];
    int repo_count;
    char encoder[HYP_SZ_32]; /* "local-nano" — the in-binary lane */
    char device[HYP_SZ_8];   /* "cpu" — never a probe, never a guess */
    bool shared_feeds;       /* false until somebody says otherwise */
} hyp_onboard_resolved_t;

/* Resolve what `hyp` should do in `repo_dir` with nothing typed.
 *
 * No TOML -> the workspace of one on C1's own terms (identity.h: workspace ==
 * NULL is the same code path, and the workspace id of a workspace of one is
 * the repo's own slug), role `member`, local nano on CPU, no shared feeds.
 * The TOML is NEVER a prerequisite — its absence is the main path, not an
 * error. With a TOML: its name (or, when it names none, a name computed from
 * the sorted member set — deterministic, never "the first repo's slug"), and
 * a slug collision inside the workspace is REFUSED naming both paths (A6).
 *
 * A6 owns the eventual one-resolver-over-TOML-and-Multica; this is the
 * zero-config and TOML leg of that resolver, written so A6 can absorb it
 * rather than stand a second code path beside it. */
int hyp_onboard_resolve(const char *repo_dir, hyp_onboard_resolved_t *out, char *err,
                        size_t errlen);

/* ── The command ────────────────────────────────────────────────── */

/* `hyponoia onboard [dir] [--json] [--answers <file>] [--yes] [--no-toml]
 *                   [--propose-only]`
 * Probe, propose, ask the four questions (a tty only; an answers file or
 * --yes runs the whole flow non-interactively), confirm, optionally record.
 * Without a tty and without --answers/--yes it prints the proposal and
 * REFUSES to write — a confident refusal beats a hung prompt. */
int hyp_cmd_onboard(int argc, char **argv);

#endif /* HYP_CLI_ONBOARD_H */
