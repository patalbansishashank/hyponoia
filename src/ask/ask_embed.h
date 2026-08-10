/*
 * ask_embed.h — the opt-in second indexing pass (NEXT-STEPS §2.1, track 4).
 *
 * ═════════════════════════════════════════════════════════════════════════
 * WHY THIS IS A SEPARATE INVOCATION AND NOT A PIPELINE PASS
 * ═════════════════════════════════════════════════════════════════════════
 *
 * §2.1 requires that "the structural index keeps the speed it has today,
 * untouched", and that the embed work be opt-in because it scales with
 * declaration count: 4,117 declarations on lld/ELF are ~3 minutes on a GPU and
 * ~23 minutes on a CPU, against ten seconds for the whole structural index.
 *
 * A pass inside the pipeline, gated off by default, would satisfy the letter of
 * that. A separate entry point satisfies it as a PROPERTY: nothing in
 * src/pipeline, src/store, src/graph_buffer or internal/hyp changes at all, so
 * "the structural index did not slow down" is provable by reading the diff and
 * not only by re-running the clock. That is worth more than the convenience of
 * one command doing both, and it is why `hyponoia embed` exists as its own
 * subcommand.
 *
 * The pass therefore reads the graph database READ-ONLY, through its own SQLite
 * connection, and writes only to its own file. It cannot perturb the structural
 * index even if it crashes.
 *
 * ═════════════════════════════════════════════════════════════════════════
 * WHAT IT EMBEDS
 * ═════════════════════════════════════════════════════════════════════════
 *
 * Every node in the project that has a source span — no filtering by node kind.
 * On the pinned lld/ELF corpus that is 4,117 of 4,202 nodes: 1,264 Method,
 * 1,095 Field, 896 Function, 381 Class, 355 Variable, 47 Module, 40 Enum,
 * 39 Macro. The 85 excluded are Project, Folder, File, Branch and EnvVar nodes,
 * which have no span at all — that is a SPAN test, not a kind test, and it is
 * the only exclusion.
 *
 * For scale: the retrievable population today, through the static token-vector
 * path, is Function + Method only = 2,160. This pass makes classes, fields,
 * enums, macros, variables and modules retrievable for the first time, which
 * nearly doubles the denominator. Any recall number measured after this must
 * be reported against the original population AND the enlarged one, separately,
 * or the two effects — a bigger haystack and newly-available gold — will be
 * summed into one number that says nothing.
 *
 * The text is the verbatim source lines start_line..end_line joined with '\n'.
 * Lines are split on '\n' with a trailing '\r' removed, which is what
 * tree-sitter counts lines by. (The reference implementation uses Python's
 * str.splitlines(), which ALSO splits on \v, \f and \x1c-\x1e — so a source
 * file containing a form-feed page break would make its slice disagree with the
 * line numbers tree-sitter produced. There are none in the pinned corpus, so
 * this changes no recorded number; it is recorded here because it is a latent
 * divergence, not a deliberate one.)
 */
#ifndef HYP_ASK_EMBED_H
#define HYP_ASK_EMBED_H

#include "ask/ask_encoder.h"

#include <stdbool.h>
#include <stdint.h>

/* How many declarations the pass holds in memory before grouping and encoding
 * them. This is a WINDOW OVER DECLARATIONS, not a forward-pass size: it decides
 * how many documents the grouping rule gets to SEE at once, and therefore how
 * well it can length-sort. It is not a memory bound on the model — that is the
 * padded-slot budget in ask_batch.h, and the two must not be confused. */
#define HYP_ASK_SORT_WINDOW 4096

/* Retained source bytes before the window is flushed early, whatever the
 * document count. One generated declaration can be megabytes; a window sized
 * only in documents would let a handful of them exhaust host memory. */
#define HYP_ASK_SORT_WINDOW_BYTES (64 * 1024 * 1024)

/* Measured throughput of the in-binary lane, in declarations per second.
 *
 * Track 2 measured llama.cpp with Qwen3-Embedding-0.6B encoding whole lld/ELF
 * declarations: 1.581 docs/s on 16 CPU threads against 24.09 docs/s on the GPU
 * at the peak batch. The recommendation is to ship the in-binary lane CPU-only
 * and leave GPU indexing to the out-of-process extractor, which makes ~15x the
 * honest expectation for anyone running this command. lld/ELF's 4,117
 * declarations are about 43 minutes here.
 *
 * Used ONLY to tell the user what they are about to wait for, before they wait
 * for it. It is a corpus-shaped number and the command says so. */
#define HYP_ASK_CPU_DOCS_PER_SEC 1.581

typedef struct {
    const char *project;
    /* Repo root. NULL takes it from the graph's projects.root_path, which is
     * what an ordinary run wants. */
    const char *repo_path;
    /* Explicit graph DB path. NULL resolves <cache>/<project>.db. */
    const char *graph_db_path;
    /* Explicit vector DB path. NULL resolves <cache>/vectors/<project>.db. */
    const char *vectors_db_path;
    /* Re-encode every declaration even when its text is byte-identical to what
     * is stored. The reuse path is keyed on the content hash and is safe; this
     * exists for measuring encode throughput and for proving reuse correct. */
    bool force;
    /* Replace an index built by a different model / dim / window instead of
     * refusing. A model change requires it, because mixed vectors are
     * undetectable downstream. */
    bool allow_model_change;
    /* 0 uses the compiled defaults from ask_batch.h. */
    int token_budget;
    int max_docs;
    /* Stop after this many declarations. 0 = the whole corpus. For smoke runs;
     * an index built with a limit is a PARTIAL index and the report says so. */
    int limit;
} hyp_ask_embed_opts_t;

typedef struct {
    int64_t declarations_seen;
    int64_t embedded;
    int64_t reused;
    /* Spans the pass could not read: the file is gone, unreadable, or the span
     * runs past the end of the file. Counted rather than silently skipped —
     * the difference between "this corpus has no declaration for X" and "the
     * file holding X could not be read" is exactly the distinction the rest of
     * this engine spends its effort preserving. */
    int64_t skipped_unreadable;
    int64_t pruned;
    /* Batching cost, so a throughput change is visible without a stopwatch. */
    int64_t forward_passes;
    int64_t padded_slots;
    int64_t max_rect;
    /* The truncation counter's three states, as they were at the end of the
     * run: `truncation_known` false IS the unknown state and must not be read
     * as "none were truncated". */
    bool truncation_known;
    int64_t truncated;
    bool partial; /* limit was hit — the index does not cover the corpus */
    double elapsed_ms;
} hyp_ask_embed_report_t;

/* Run the pass.
 *
 * Returns 0 when work was done, HYP_ASK_EMBED_NO_WORK when the run completed
 * without finding a single declaration, and < 0 on failure.
 *
 * NO_WORK IS ITS OWN RETURN CODE ON PURPOSE. A run that scans an empty or
 * wrong-named project succeeds at everything it attempts and produces an index
 * of nothing; folding that into 0 would make "measured nothing" and "measured
 * successfully" the same answer. Track 2 lost a whole VRAM measurement to
 * exactly that shape — a watcher reported `docs:0` and exited 0 because its
 * stdin had been redirected away, so it silently measured nothing,
 * successfully. Zero work has to be loud. */
#define HYP_ASK_EMBED_NO_WORK 1

int hyp_ask_embed_run(const hyp_ask_encoder_t *enc, const hyp_ask_embed_opts_t *opts,
                      hyp_ask_embed_report_t *out);

/* The verbatim text of one span: lines `start`..`end` inclusive, 1-based,
 * joined with '\n', no trailing newline. Caller frees. NULL when the file
 * cannot be read or `start` is past its end. Exposed for tests, because "what
 * exactly gets embedded" is the one thing a port cannot afford to get subtly
 * wrong. */
char *hyp_ask_read_span(const char *abs_path, int start, int end);

/* sha256 of `text`, truncated to HYP_ASK_VEC_HASH_LEN hex chars. `out` must
 * hold HYP_ASK_VEC_HASH_LEN + 1 bytes. */
void hyp_ask_content_hash(const char *text, char *out);

#endif /* HYP_ASK_EMBED_H */
