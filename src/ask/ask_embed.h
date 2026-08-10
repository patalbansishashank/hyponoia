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
 * ~45 minutes on a CPU, against ten seconds for the whole structural index.
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
/* Guard is HYP_ASK_EMBED_PASS_H, not HYP_ASK_EMBED_H: src/semantic/ask_embed.h
 * — a DIFFERENT header, the tool's inference boundary — used the latter, and
 * two files with the same basename had the same guard. Including both left the
 * second one silently empty, which is how a translation unit that asked for
 * HYP_ASK_MODEL_ID_MAX got a compile error naming a constant that is plainly
 * defined in a header it plainly includes. Found by src/ask/ask_cmd.c, the
 * first file to need both. */
#ifndef HYP_ASK_EMBED_PASS_H
#define HYP_ASK_EMBED_PASS_H

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

/* Measured throughput, in declarations per second, from track 2's ladder over
 * whole lld/ELF declarations with llama.cpp + Qwen3-Embedding-0.6B at Q8_0.
 *
 * GPU IS THE PRIMARY PATH. 24.09 against 1.581 is 15.2x — lld/ELF's 4,117
 * declarations are ~3 minutes against ~45, and on a corpus of any real size the
 * CPU path is not slow, it is unusable. An opt-in pass nobody can afford to opt
 * into is not a feature. Track 2 recommended shipping the in-binary lane
 * CPU-only on install-story grounds (see ask_batch.h for what a GPU build costs
 * in linking); that recommendation was weighed and overridden. The CPU path
 * REMAINS as a fallback and is never removed — it is what runs where there is
 * no usable device, and the pass says which one ran.
 *
 * Used ONLY to tell the user what they are about to wait for, before they wait
 * for it. Corpus-shaped numbers, and the command says so. */
#define HYP_ASK_CPU_DOCS_PER_SEC 1.581
#define HYP_ASK_GPU_DOCS_PER_SEC 24.093

/* ═════════════════════════════════════════════════════════════════════════
 * WHOLE-FILE SPANS (NEXT-STEPS §2.2 lever 3)
 * ═════════════════════════════════════════════════════════════════════════
 *
 * The extractor emits one `Module` node per file whose span is line 1 to EOF
 * (internal/hyp/extract_defs.c, hyp_extract_definitions — "always first
 * definition"). Embedded whole, that row is a document containing every other
 * document in the file, and a document that contains everything matches
 * everything: on the pinned lld/ELF corpus a whole-file row outranks the gold
 * declaration on 25 of the 60 frozen queries, while NO class, struct or enum
 * ever outranks its own method. They are 1.1% of rows and 48.2% of tokens, and
 * both truncated rows on that corpus are whole-file spans.
 *
 * DROP versus DEMOTE was measured rather than argued, and the measurement is
 * that the choice does not exist on the ranking:
 *
 *   - dropping the 47 rows moves the enlarged-population median from 14.0 to
 *     12.5 and recall@10 from 0.467 to 0.483, 34 queries improved and 0
 *     worsened;
 *   - a query-time score penalty reproduces those numbers EXACTLY once it
 *     exceeds the largest margin by which a whole-file row outscores a gold
 *     (0.193 on this corpus), and reproduces every headline metric from 0.05.
 *
 * So demotion buys nothing the drop does not, and it keeps the whole cost: the
 * 48% of tokens, the entire seq_len-32768 forward-pass bucket (20 of 20
 * documents there are whole-file rows), and the truncation set. The row is
 * dropped at INDEX time, where the cost is.
 *
 * ONE EXEMPTION, AND IT IS THE ONE THAT MATTERS. A file whose only span-bearing
 * node is its whole-file row would become unreachable — on lld/ELF that is
 * CMakeLists.txt and README.md, which is precisely the "where is X configured"
 * question a file-level row is legitimately the answer to. Those rows are kept.
 * They cost 528 tokens between them and change no benchmark number.
 *
 * KEEP restores the previous behaviour. Flipping the knob does not require a
 * rebuild of anything: the pass prunes rows it did not see, so KEEP -> DROP
 * costs one scan, and DROP -> KEEP costs one scan plus the whole-file rows. */
typedef enum {
    /* Default. Skip whole-file spans that are not the only row for their file. */
    HYP_ASK_WHOLE_FILE_DROP = 0,
    HYP_ASK_WHOLE_FILE_KEEP = 1,
} hyp_ask_whole_file_t;

/* "drop" / "keep". Never NULL. */
const char *hyp_ask_whole_file_name(hyp_ask_whole_file_t p);

/* Parse "drop" or "keep" into `*out`. False (and `*out` untouched) otherwise. */
bool hyp_ask_whole_file_parse(const char *s, hyp_ask_whole_file_t *out);

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
    /* What device the caller WANTS. The encoder decides what it can honour and
     * reports the answer through device_note; the pass records both so a run
     * that asked for a GPU and got a CPU is visible rather than merely slow. */
    hyp_ask_device_pref_t device_pref;
    /* Stop after this many declarations. 0 = the whole corpus. For smoke runs;
     * an index built with a limit is a PARTIAL index and the report says so. */
    int limit;
    /* What to do with a span that covers its whole file. Zero-initialising the
     * options struct selects DROP, which is the recommended default — see the
     * comment on hyp_ask_whole_file_t for the measurement that chose it. */
    hyp_ask_whole_file_t whole_file_spans;
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
    /* Whole-file spans this run declined to embed. Reported separately from
     * skipped_unreadable because they are a POLICY exclusion, not a failure —
     * and separately counted at all because a population that silently differs
     * between two indexes is a recall number nobody can compare. Always 0 under
     * HYP_ASK_WHOLE_FILE_KEEP. */
    int64_t skipped_whole_file;
    /* Whole-file spans kept BECAUSE they were the only span-bearing row for
     * their file, under a policy that would otherwise have dropped them. The
     * exemption is small and load-bearing, so it is counted rather than
     * assumed. */
    int64_t whole_file_kept_sole;
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
    /* What device actually ran, copied from the encoder. Owned; freed by
     * hyp_ask_embed_report_free. */
    char *device_note;
    bool device_was_gpu;
    /* True when the caller asked for a GPU and did not get one. The whole
     * reason device_note exists: 24.09 docs/s against 1.581 is ~3 minutes
     * against ~45, and a silent fallback is the failure that wastes an
     * afternoon. */
    bool device_downgraded;
} hyp_ask_embed_report_t;

void hyp_ask_embed_report_free(hyp_ask_embed_report_t *r);

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

/* As hyp_ask_read_span, and additionally reports the file's total line count
 * through `out_file_lines` when it is non-NULL.
 *
 * The count is what decides whether a span is a WHOLE-FILE span, and it is
 * taken here rather than from a second stat/read because this function has
 * already read the file: asking twice would let the answer change between the
 * two reads, and "is this the whole file" would then be a claim about a file
 * that no longer exists in that shape. Lines are counted the same way the span
 * is sliced — on '\n' — so the two can never disagree. A file with no trailing
 * newline still counts its last partial line. */
char *hyp_ask_read_span_lines(const char *abs_path, int start, int end, int *out_file_lines);

/* Does `start`..`end` cover every line of a file that has `file_lines` lines?
 * Line 1 to EOF, which is exactly the extractor's per-file Module span. */
bool hyp_ask_span_is_whole_file(int start, int end, int file_lines);

/* sha256 of `text`, truncated to HYP_ASK_VEC_HASH_LEN hex chars. `out` must
 * hold HYP_ASK_VEC_HASH_LEN + 1 bytes. */
void hyp_ask_content_hash(const char *text, char *out);

#endif /* HYP_ASK_EMBED_PASS_H */
