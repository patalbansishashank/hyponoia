/*
 * model_fetch.h — fetch-on-first-use for the `ask` lane's embedding weights.
 *
 * NEXT-STEPS.md §2.1 costs the `ask` lane at "~600 MB against today's 31 MB
 * blob"; runs/ASK/T2-inference-runtime.json measured the alternatives and
 * closed the question: EMBED is out on size (the cheapest quantisation that
 * holds cosine >= 0.99 is Q6_K at 495 MB — 23% off Q8_0, not an order of
 * magnitude), SHIP-BESIDE is the same bytes while breaking "one file, no
 * runtime dependencies", so the weights are FETCHED, at Q8_0, verified by
 * SHA-256. That charges the 639 MB to the population that runs `ask` and to
 * nobody else — the other fifteen tools have no opinion about embeddings.
 *
 * ── Three properties this module must not break ─────────────────────
 *
 * 1. NO NETWORK BY DEFAULT. src/daemon/application.c deleted the production
 *    update-check for exactly this reason: "a build that ships no provider
 *    makes no network request by default -- the property, not just the absence
 *    of a call". Nothing here is reachable from the MCP or daemon path. The
 *    only caller is `hyponoia fetch-model`, a command a person types, and it
 *    still refuses to run unattended without --yes. `ask` REPORTS that the
 *    weights are missing (hyp_model_unavailable_text) and never fetches them.
 *
 * 2. NO NEW SYSCALL SURFACE. The bytes are moved by `curl` through
 *    hyp_exec_no_shell, the shape src/cli/cli.c already uses. Hyponoia keeps
 *    the parts that must not be delegated — the pinned URL, the pinned digest,
 *    the verification, the atomic placement — and gains no socket(), no
 *    connect(), no dlopen(). scripts/security-network-source-audit.py and
 *    scripts/security-vendored.sh both stay clean by construction.
 *
 * 3. FAIL CLOSED ON THE DIGEST. Bytes land on a `.part` sibling and are
 *    renamed into place ONLY after the SHA-256 matches. An interrupted or
 *    corrupted fetch therefore cannot be mistaken for a complete one by a
 *    later run, and a file that fails the digest is deleted rather than left
 *    where the next run would trust it.
 *
 * The weights are DATA. They are never marked executable and never executed —
 * which is what separates this from the in-process updater the project
 * deliberately removed (see hyp_cmd_update: download+extract+chmod+exec in a
 * shipped binary is a dropper shape; download+verify+rename of a GGUF is not).
 */
#ifndef HYP_MODEL_FETCH_H
#define HYP_MODEL_FETCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "foundation/constants.h" /* HYP_SZ_* */

/* ── The pinned artifacts ─────────────────────────────────────────────
 *
 * Compile-time constants, all four of them per model, because "fetch the
 * latest" is how a verified download becomes an unverified one. The revision is
 * a git commit on the GGUF repository, NOT the safetensors revision 97b0c614…
 * that §2 pinned — a GGUF is a different artifact built from those weights, so
 * it carries its own identity and its own digest.
 *
 * The digest is the one T2 actually measured against (min cosine 0.999276 vs
 * sentence-transformers fp32 over 45 texts) and is byte-identical to the LFS
 * sha256 Hugging Face publishes for the same file. Both were checked.
 */
/* voyage-4-nano, replacing Qwen3-Embedding-0.6B (NEXT-STEPS.md §2.10 step 1).
 * Smaller (346M against 596M), Apache 2.0, and better on public C/C++: it beats
 * Qwen3 on 7 of 8 CLARC splits, significantly on all seven. */
#define HYP_MODEL_ASK_MODEL "voyage-4-nano"
#define HYP_MODEL_ASK_QUANT "Q8_0"
#define HYP_MODEL_ASK_FILE HYP_MODEL_ASK_MODEL "-q8_0.gguf"
/* UNOFFICIAL. Voyage publishes the weights but no GGUF, and this conversion is
 * a third party's. It is pinned by revision AND digest like every other
 * artifact here, which makes the supply chain auditable, NOT absent — a
 * first-party conversion should replace it before any public release. */
#define HYP_MODEL_ASK_REPO "jsonMartin/voyage-4-nano-gguf"
#define HYP_MODEL_ASK_REVISION "75e62c7dba5ee8156717a56920976ab128b8c693"
#define HYP_MODEL_ASK_SHA256 "d0e430fe24faba10f5bc6dd4f896e1efd0e6d65d7f0cf686a500d48cac274a4a"
#define HYP_MODEL_ASK_URL                                                           \
    "https://huggingface.co/" HYP_MODEL_ASK_REPO "/resolve/" HYP_MODEL_ASK_REVISION \
    "/" HYP_MODEL_ASK_FILE

/* Exact byte count, so a short download is caught before the digest is even
 * computed and a truncated file never reaches the hasher as a surprise. */
#define HYP_MODEL_ASK_BYTES INT64_C(371900384)

/* What a human is told the download costs. Deliberately a string: "355 MB"
 * beside a progress bar is the number that matters, and rendering it from
 * HYP_MODEL_ASK_BYTES at every call site invites three spellings of it. */
#define HYP_MODEL_ASK_SIZE_TEXT "355 MB"

/* ── The second artifact: the projection head ─────────────────────
 *
 * nano's hidden size is 1024 but it emits 2048, because a per-token
 * nn.Linear(1024, 2048, bias=False) sits between the encoder and the pooler.
 * GGUF HAS NO REPRESENTATION FOR IT, so the conversion drops it and the file
 * emits 1024 — and that raw 1024 is NOT our space, it is the pre-projection
 * hidden state. Scoring with it would be wrong and silent.
 *
 * It is applied AFTER llama.cpp pools instead, which is exact rather than
 * approximate: the head is a bias-free linear map and the pooler is a mean, so
 * they commute — mean(W.h_i) = W.mean(h_i). Verified against the reference
 * pipeline at cosine 1.0000000.
 *
 * 2048 x 1024 float32, row-major (out-major, as PyTorch stores nn.Linear
 * weights). Checked bit-identical to voyageai/voyage-4-nano's own
 * `linear.weight`: max|diff| = 0.0. */
#define HYP_MODEL_ASK_PROJ_FILE "voyage-4-nano-linear.bin"
#define HYP_MODEL_ASK_PROJ_SHA256 "6ee040fad75c000b21f8beef7584e8962ec76913639f2f46ae84b9c4c1fd7dbf"
#define HYP_MODEL_ASK_PROJ_BYTES INT64_C(8388608)
#define HYP_MODEL_ASK_PROJ_SIZE_TEXT "8 MB"
#define HYP_MODEL_ASK_PROJ_URL                                                      \
    "https://huggingface.co/" HYP_MODEL_ASK_REPO "/resolve/" HYP_MODEL_ASK_REVISION \
    "/" HYP_MODEL_ASK_PROJ_FILE
/* Rows of the projection: the width nano emits before Matryoshka truncation. */
#define HYP_MODEL_ASK_PROJ_OUT 2048

/* The exact command the unavailable answer names. One spelling, one place. */
#define HYP_MODEL_ASK_COMMAND "hyponoia fetch-model"

/* Cache layout: <cache>/models/<file>, where <cache> is HYP_CACHE_DIR or
 * ~/.cache/hyponoia (hyp_resolve_cache_dir). Under the existing convention
 * rather than beside it, so a user who has fetched 639 MB finds it where the
 * indexes already are, and `rm` on one path reclaims all of it. */
#define HYP_MODEL_DIR_NAME "models"
#define HYP_MODEL_PART_SUFFIX ".part"

enum {
    HYP_MODEL_PATH_MAX = HYP_SZ_4K,
    HYP_MODEL_MSG_MAX = HYP_SZ_1K,
    /* 64 lowercase hex characters + NUL. */
    HYP_MODEL_HEX_SIZE = HYP_SZ_64 + 1
};

/* ── One artifact, described once ────────────────────────────────────
 *
 * Everything below used to be spelled with the HYP_MODEL_ASK_* macros directly,
 * which was right while there was one model and becomes two copies of a
 * download-and-verify state machine the moment there are two. The struct is the
 * seam: the machinery is written once against it, and adding a third model is a
 * table entry rather than a second copy of the failure taxonomy.
 *
 * Every field is a compile-time constant of a shipped binary. There is no way
 * to construct one at runtime and no API that accepts a caller's own — a
 * user-supplied URL or digest would defeat the whole point of pinning both. */
typedef struct {
    const char *model;     /* "Qwen3-Embedding-0.6B" */
    const char *quant;     /* "Q8_0" */
    const char *file;      /* basename, on disk AND at the far end */
    const char *repo;      /* "Qwen/Qwen3-Embedding-0.6B-GGUF" */
    const char *revision;  /* pinned git commit on the GGUF repo */
    const char *sha256;    /* lowercase hex, the only door into the final name */
    int64_t bytes;         /* exact size; a short file never reaches the hasher */
    const char *size_text; /* "639 MB", the number a human is shown */
    const char *url;       /* pinned revision, never "main" */
    const char *command;   /* the exact command an unavailable answer names */
    /* "the ask lane's embedding weights" — used mid-sentence, so it carries
     * no leading capital and no full stop. */
    const char *what;
    /* What is lost while it is missing: NOTHING was searched. */
    const char *without_it;
} hyp_model_spec_t;

/* The pinned artifacts. Never NULL.
 *
 * TWO of them, and both are required: the GGUF alone emits pre-projection
 * vectors that are the right shape, unit length and in the wrong space. */
const hyp_model_spec_t *hyp_model_ask_spec(void);
const hyp_model_spec_t *hyp_model_ask_proj_spec(void);

/* ── What is on disk ─────────────────────────────────────────────── */

typedef enum {
    HYP_MODEL_PRESENT = 0, /* full-size file in place; fetched and verified */
    HYP_MODEL_ABSENT,      /* nothing here */
    HYP_MODEL_PARTIAL,     /* an interrupted fetch is resumable */
    HYP_MODEL_WRONG_SIZE,  /* a file is in place but is not the pinned artifact */
    HYP_MODEL_NO_CACHE_DIR /* neither HYP_CACHE_DIR nor HOME resolves */
} hyp_model_state_t;

typedef struct {
    hyp_model_state_t state;
    char path[HYP_MODEL_PATH_MAX];      /* final resting place; "" if unresolvable */
    char part_path[HYP_MODEL_PATH_MAX]; /* the resumable sibling; "" if unresolvable */
    int64_t bytes;                      /* size of `path`, -1 when it does not exist */
    int64_t part_bytes;                 /* resumable bytes already fetched, 0 when none */
} hyp_model_probe_t;

/* Probe is CHEAP ON PURPOSE: existence plus an exact size comparison, no
 * hashing. Re-hashing 639 MB on every `ask` would cost seconds per question to
 * re-answer something already answered — and it is sound to skip precisely
 * because the fetch path renames into `path` only after the digest passed, so
 * a full-size file at that name is a file that was verified. `--verify`
 * re-establishes it on demand for anyone who wants the check repeated. */
void hyp_model_ask_probe(hyp_model_probe_t *out);

/* Convenience over the probe: true only for HYP_MODEL_PRESENT. This is the
 * predicate the `ask` lane branches on. */
bool hyp_model_ask_present(void);

/* <cache>/models/<file>. Returns out, or NULL when the cache dir is
 * unresolvable or the path would not fit. */
const char *hyp_model_ask_path(char *out, size_t out_sz);

/* <cache>/models. Same contract. */
const char *hyp_model_ask_dir(char *out, size_t out_sz);

/* The same three, for any spec. The four ask-specific entry points above are
 * these with hyp_model_ask_spec() bound, kept because they are what the rest of
 * the tree already calls and because "is the embedding model here" is asked far
 * more often than "is model X here". */
void hyp_model_probe_spec(const hyp_model_spec_t *spec, hyp_model_probe_t *out);
bool hyp_model_spec_present(const hyp_model_spec_t *spec);
const char *hyp_model_spec_path(const hyp_model_spec_t *spec, char *out, size_t out_sz);


/* ── Verification ────────────────────────────────────────────────── */

/* Streaming SHA-256 of a whole file to lowercase hex. `out` needs
 * HYP_MODEL_HEX_SIZE bytes. Returns 0 on success, non-zero if the file cannot
 * be read to the end — a read error is NOT a digest, so it can never pass. */
int hyp_model_sha256_file(const char *path, char *out, size_t out_sz);

/* The gate. Returns 0 only when `path` hashes to `expect_hex`.
 *
 * Every other outcome — unreadable, short, one bit different — is a refusal,
 * and when `remove_on_mismatch` the file is UNLINKED, because the failure mode
 * this exists to prevent is a corrupt blob sitting at the good name until a
 * later run loads it. `err` (never NULL when err_sz > 0) gets a caller-facing
 * sentence. */
int hyp_model_verify_file(const char *path, const char *expect_hex, bool remove_on_mismatch,
                          char *err, size_t err_sz);

/* ── The user-facing unavailable state ──────────────────────────── */

/* The second cause of `ask` being unavailable, in the two-field shape the tool
 * already emits: `detail` says what is missing, `remedy` says what to type.
 * Both are filled for the ABSENT and PARTIAL cases; calling it when the model
 * IS present yields a detail that says so, so a caller cannot accidentally
 * report a lie. `project` may be NULL. */
void hyp_model_unavailable_text(const char *project, char *detail, size_t detail_sz, char *remedy,
                                size_t remedy_sz);

/* The same, for any spec. */
void hyp_model_unavailable_text_spec(const hyp_model_spec_t *spec, const char *project,
                                     char *detail, size_t detail_sz, char *remedy,
                                     size_t remedy_sz);

/* ── The fetch ───────────────────────────────────────────────────── */

typedef enum {
    HYP_MODEL_FETCH_OK = 0,          /* fetched, verified, installed */
    HYP_MODEL_FETCH_ALREADY_PRESENT, /* nothing to do */
    HYP_MODEL_FETCH_DECLINED,        /* consent withheld, or unattended without --yes */
    HYP_MODEL_FETCH_NO_CACHE_DIR,    /* nowhere to put it */
    HYP_MODEL_FETCH_MKDIR_FAILED,    /* <cache>/models could not be created */
    HYP_MODEL_FETCH_NO_SPACE,        /* refused before transferring a byte */
    HYP_MODEL_FETCH_NO_DOWNLOADER,   /* curl is not on PATH */
    HYP_MODEL_FETCH_DNS,             /* huggingface.co did not resolve */
    HYP_MODEL_FETCH_OFFLINE,         /* resolved, could not connect */
    HYP_MODEL_FETCH_HTTP,            /* the pinned revision answered 4xx/5xx */
    HYP_MODEL_FETCH_TLS,             /* certificate or handshake failure */
    HYP_MODEL_FETCH_DISK_FULL,       /* write failed part-way */
    HYP_MODEL_FETCH_INTERRUPTED,     /* short file KEPT for the next resume */
    HYP_MODEL_FETCH_DIGEST_MISMATCH, /* verified and REJECTED; the file was deleted */
    HYP_MODEL_FETCH_INSTALL_FAILED,  /* verified but the atomic rename failed */
    HYP_MODEL_FETCH_TRANSFER_FAILED  /* curl failed some other way */
} hyp_model_fetch_result_t;

/* One line per outcome saying what to do about it. Never NULL. */
const char *hyp_model_fetch_result_text(hyp_model_fetch_result_t result);

typedef struct {
    bool assume_yes;  /* --yes: consent supplied on the command line */
    bool force;       /* --force: re-fetch over an existing copy */
    bool verify_only; /* --verify: re-hash what is there, download nothing */
    bool quiet;       /* suppress the progress bar (tests, CI) */
} hyp_model_fetch_opts_t;

/* Fetch the pinned weights. Prints its own progress and diagnostics.
 * `opts` may be NULL for all-defaults. */
hyp_model_fetch_result_t hyp_model_fetch(const hyp_model_fetch_opts_t *opts);

/* The same, for any spec. */
hyp_model_fetch_result_t hyp_model_fetch_spec(const hyp_model_spec_t *spec,
                                              const hyp_model_fetch_opts_t *opts);

/* `hyponoia fetch-model [--yes] [--force] [--verify] [--path] [--help]`.
 * Wired from main.c. Returns a process exit code. */
int hyp_cmd_fetch_model(int argc, char **argv);

#ifdef HYP_ENABLE_TEST_SEAMS
/* Replace the byte-moving step so the failure paths — DNS, 404, disk full,
 * interruption, a flipped bit — can be driven deterministically with no
 * network. Returns a curl-shaped exit code. Test-seam only: it is compiled out
 * of a shipped binary entirely, so it is not a way to redirect a real fetch.
 * NULL restores the production curl path. */
typedef int (*hyp_model_transfer_fn)(const char *url, const char *dest, int64_t resume_from,
                                     void *ud);
void hyp_model_set_transfer_for_test(hyp_model_transfer_fn fn, void *ud);
#endif

#endif /* HYP_MODEL_FETCH_H */
