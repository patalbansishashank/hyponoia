/*
 * ask_cmd.c — `hyponoia embed`, the opt-in entry point for the `ask` lane's
 * second pass.
 *
 * It is a SUBCOMMAND rather than a flag on index_repository because that is
 * what makes "the structural index is untouched" a property of the build and
 * not a claim about a branch. Nothing here runs unless it is asked for by name.
 */

#include "ask/ask_cmd.h"
#include "ask/ask_batch.h"
#include "ask/ask_embed.h"
#include "ask/ask_encoder.h"
#include "ask/ask_llama.h"
#include "semantic/ask_embed.h" /* HYP_ASK_MODEL_ID_MAX */
#include "cli/model_fetch.h"
#include "ask/ask_vectors.h"
#include "ask/ask_provider.h"
#include "cli/cli.h"
#include "foundation/platform.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { AC_LINEBUF = 512 };

static void ask_embed_usage(void) {
    printf("Usage:\n"
           "  hyponoia embed --project <name> [options]\n"
           "  hyponoia embed --status --project <name>\n"
           "\n"
           "Builds per-declaration embedding vectors for the `ask` lane. This is a\n"
           "SECOND pass, opt-in and separate from index_repository: it scales with\n"
           "declaration count (minutes, not the seconds the structural index takes),\n"
           "and running it never touches the structural index.\n"
           "\n"
           "Vectors are written to <cache>/vectors/<project>.db — a file of their own,\n"
           "so a re-index does not destroy work that is still valid.\n"
           "\n"
           "Options:\n"
           "  --project <name>       Indexed project to embed. Required.\n"
           "  --repo-path <path>     Source root. Default: the graph's recorded root.\n"
           "  --status               Report the stored index and exit.\n"
           "  --verify-batching      Encode a fixture alone and in groups of --max-docs and\n"
           "                         compare; PASS means a vector does not depend on what\n"
           "                         shared its forward pass. No project needed, nothing\n"
           "                         written. HYP_ASK_VERIFY_TEXTS=<file> (NUL-separated)\n"
           "                         substitutes real declarations for the fixture.\n"
           "  --force                Re-encode even byte-identical declarations.\n"
           "  --allow-model-change   Discard vectors from a different model/dim/window.\n"
           "  --limit <n>            Stop after n declarations (partial index).\n"
           "  --whole-file-spans drop|keep\n"
           "                         What to do with a declaration whose span covers its\n"
           "                         whole file — the extractor's per-file `Module` node.\n"
           "                         Default drop, and it is a retrieval decision, not a\n"
           "                         saving: such a row contains every other document in\n"
           "                         the file, so it competes with all of them. On the\n"
           "                         measured corpus they are 1.1%% of rows and 48%% of\n"
           "                         tokens, they outrank the right answer on 25 of 60\n"
           "                         benchmark questions, and both truncated rows are\n"
           "                         whole-file spans. A file whose ONLY row is its\n"
           "                         whole-file span keeps it either way, so a\n"
           "                         CMakeLists.txt does not become invisible.\n"
           "                         Also settable with HYP_ASK_WHOLE_FILE_SPANS.\n"
           "  --budget <slots>       Padded-slot ceiling per forward pass (default %d).\n"
           "  --max-docs <n>         Documents per forward pass (default %d).\n"
           "  --device auto|gpu|cpu  Which device to encode on. Default auto: GPU when\n"
           "                         one can be used safely, else CPU. `gpu` refuses\n"
           "                         rather than falling back silently.\n"
           "  --stub                 Use the deterministic stub encoder.\n"
           "  --stub-no-truncation-report\n"
           "                         Stub that cannot report token counts, so the\n"
           "                         truncation state comes out UNKNOWN.\n"
           "\n"
           "Device matters more than any other knob here: %.2f declarations/s on GPU\n"
           "against %.3f on CPU, measured on this corpus — about 3 minutes versus 45.\n"
           "Every run prints which device it actually used, and the index records it.\n"
           "\n"
           "Without an inference backend, --stub is the only encoder available. A stub\n"
           "index carries model id `%s` and can never be silently\n"
           "mistaken for a real one: its vectors have the right shape and no retrieval\n"
           "quality at all.\n",
           HYP_ASK_TOKEN_BUDGET, HYP_ASK_MAX_DOCS, HYP_ASK_GPU_DOCS_PER_SEC,
           HYP_ASK_CPU_DOCS_PER_SEC, HYP_ASK_STUB_MODEL_ID);
}

/* ── --verify-batching ──────────────────────────────────────────── */

/* Sixteen declarations of DIFFERENT token lengths, so the batches this check
 * forms are ragged the way a real length-sorted group is. Not sorted: a fixed,
 * unsorted order is what makes "which position in the batch went wrong" a
 * readable answer. */
static const char *const ASK_VERIFY_FIXTURE[] = {
    "int add(int a, int b) { return a + b; }",
    "static void clear(char *p, size_t n) {\n    memset(p, 0, n);\n}",
    "def parse(line):\n    return [int(x) for x in line.split(',') if x]",
    "struct node { struct node *next; int key; };",
    "bool is_prime(unsigned n) {\n    if (n < 2) return false;\n    for (unsigned d = 2; d * d <= "
    "n; d++)\n"
    "        if (n % d == 0) return false;\n    return true;\n}",
    "class Stack {\npublic:\n    void push(int v) { data.push_back(v); }\n    int pop() { int v = "
    "data.back(); data.pop_back(); return v; }\nprivate:\n    std::vector<int> data;\n};",
    "fn main() { println!(\"hello\"); }",
    "/* Reads the whole file into a heap buffer; caller frees. Returns NULL on any "
    "error and leaves errno set by the failing call. */\nchar *slurp(const char *path);",
    "SELECT name, count(*) FROM users GROUP BY name HAVING count(*) > 1;",
    "template <typename T> T max_of(T a, T b) { return a < b ? b : a; }",
    "void ObjFile::parseSymbols(ArrayRef<Elf_Sym> syms) {\n    for (const Elf_Sym &s : syms)\n"
    "        symbols.push_back(make<Symbol>(s));\n}",
    "x = 1",
    "async function fetchJson(url) {\n    const r = await fetch(url);\n    if (!r.ok) throw new "
    "Error(r.statusText);\n    return r.json();\n}",
    "#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))",
    "func (s *Server) Close() error {\n\ts.mu.Lock()\n\tdefer s.mu.Unlock()\n\treturn "
    "s.listener.Close()\n}",
    "enum Color { RED, GREEN, BLUE };",
};
enum {
    ASK_VERIFY_FIXTURE_COUNT = (int)(sizeof(ASK_VERIFY_FIXTURE) / sizeof(ASK_VERIFY_FIXTURE[0]))
};

/* Texts for the check: HYP_ASK_VERIFY_TEXTS names a NUL-separated file when a
 * measurement wants real declarations; otherwise the fixture above. Returns the
 * count; `*out_buf` (may be NULL) is the caller's to free. */
static int ask_verify_texts(const char ***out_texts, char **out_buf) {
    *out_buf = NULL;
    const char *path = getenv("HYP_ASK_VERIFY_TEXTS");
    if (!path || !path[0]) {
        const char **t = malloc(sizeof(*t) * (size_t)ASK_VERIFY_FIXTURE_COUNT);
        if (!t) {
            return -1;
        }
        for (int i = 0; i < ASK_VERIFY_FIXTURE_COUNT; i++) {
            t[i] = ASK_VERIFY_FIXTURE[i];
        }
        *out_texts = t;
        return ASK_VERIFY_FIXTURE_COUNT;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        (void)fprintf(stderr, "embed --verify-batching: cannot open %s\n", path);
        return -1;
    }
    size_t cap = 1 << 16;
    size_t len = 0;
    char *buf = malloc(cap + 1);
    while (buf) {
        size_t got = fread(buf + len, 1, cap - len, f);
        len += got;
        if (len < cap) {
            break;
        }
        cap *= 2;
        char *nb = realloc(buf, cap + 1);
        if (!nb) {
            free(buf);
            buf = NULL;
        } else {
            buf = nb;
        }
    }
    (void)fclose(f);
    if (!buf) {
        return -1;
    }
    buf[len] = '\0';
    int n = 0;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\0') {
            n++;
        }
    }
    if (len > 0 && buf[len - 1] != '\0') {
        n++; /* unterminated last text */
    }
    const char **t = malloc(sizeof(*t) * (size_t)(n > 0 ? n : 1));
    if (!t) {
        free(buf);
        return -1;
    }
    int k = 0;
    size_t start = 0;
    for (size_t i = 0; i <= len && k < n; i++) {
        if (i == len || buf[i] == '\0') {
            t[k++] = buf + start;
            start = i + 1;
        }
    }
    *out_texts = t;
    *out_buf = buf;
    return n;
}

static int ask_cmd_verify_batching(hyp_ask_encoder_t *enc, int group) {
    const char **texts = NULL;
    char *buf = NULL;
    int n = ask_verify_texts(&texts, &buf);
    if (n <= 0) {
        free(texts);
        free(buf);
        return 1;
    }
    if (group <= 0) {
        group = HYP_ASK_MAX_DOCS;
    }
    hyp_ask_batch_row_t *rows = calloc((size_t)n, sizeof(*rows));
    hyp_ask_batch_check_t chk;
    printf("embed --verify-batching: %d text(s), alone then in groups of %d, on %s\n", n, group,
           hyp_ask_encoder_device_note(enc));
    /* THE BAR, SET FROM MEASUREMENT ON BOTH SIDES.
     *
     * The failure this exists to catch (a ragged batch through per-sequence
     * KV streams, see ask_llama.c) put every row but the shortest at cosine
     * 0.49-0.91 to the same text alone. The lane's own arithmetic floor is
     * NOT 1.0: the same 96 declarations encoded ALONE on the CPU and ALONE on
     * the GPU agree at mean 0.999924, min 0.999873, and a correctly batched
     * pass sits at exactly that floor (mean 0.999919, min 0.999868, CPU and
     * GPU alike). Q8_0 weights, f16 K/V and a tiled attention kernel differ
     * in the last bits per device and per tile alignment, and Q8 activation
     * quantisation turns those bits into ~1e-4 of cosine; disabling flash
     * attention put 11 of 16 rows at exactly 1.000000, which is what shows
     * the residual is arithmetic and not a leak. So the bar is 0.999: an
     * order of magnitude above the floor's worst deficit (1.3e-4) and two
     * below the failure's best (9e-2). Four nines would fail alone-vs-alone
     * across devices, which is what the index/query split does every day. */
    const float threshold = 0.999F;
    int rc =
        rows ? hyp_ask_encoder_check_batching(enc, texts, n, group, threshold, rows, &chk) : -1;
    if (rc != 0) {
        (void)fprintf(stderr, "embed --verify-batching: the encoder failed; nothing compared\n");
        free(rows);
        free(texts);
        free(buf);
        return 1;
    }
    for (int i = 0; i < n; i++) {
        int len = hyp_ask_encoder_token_length(enc, texts[i]);
        printf("  row %3d  group %2d  pos %2d  tokens %5d  own %.6f  best_other %.6f (row %d)%s\n",
               i, i / group, i % group, len, (double)rows[i].own, (double)rows[i].best_other,
               rows[i].best_other_idx,
               rows[i].own < threshold
                   ? (rows[i].best_other_idx >= 0 && rows[i].best_other > rows[i].own
                          ? "  <- MISASSIGNED"
                          : "  <- CONTAMINATED")
                   : "");
    }
    printf("  passes %d  min own-cosine %.6f (row %d)  below %.4f: %d  misassigned: %d\n",
           chk.groups, (double)chk.min_own, chk.min_own_row, (double)threshold, chk.below,
           chk.misassigned);
    bool ok = chk.below == 0 && chk.misassigned == 0;
    printf("  verdict: %s — a document's vector %s on what shared its forward pass\n",
           ok ? "PASS" : "FAIL", ok ? "does not depend" : "DEPENDS");
    free(rows);
    free(texts);
    free(buf);
    return ok ? 0 : 1;
}

static int ask_cmd_status(const char *project) {
    hyp_ask_vectors_t *v = hyp_ask_vectors_open(project);
    if (!v) {
        (void)fprintf(stderr, "embed: cannot open the vector store for '%s'\n", project);
        return 1;
    }
    hyp_ask_vec_meta_t m;
    int rc = hyp_ask_vectors_get_meta(v, &m);
    if (rc == HYP_ASK_VEC_NOT_FOUND) {
        /* One of the TWO reasons the lane can be unavailable, and they want
         * different sentences. This one is "the index has not been built" and
         * the fix is to run the pass. The other is "the model has not been
         * fetched" — the backend downloads its weights on first use of this
         * lane — and the fix is to fetch it. Collapsing them into one
         * "unavailable" would send half the users to the wrong remedy. */
        printf("ask index: NOT BUILT for '%s'.\n", project);
        printf("  The `ask` lane is unavailable for this project until `hyponoia embed` runs.\n");
        /* UNKNOWN, disclosed rather than defaulted — see hyp_ask_trunc_t. */
        printf("  truncation: UNKNOWN — nothing has been measured, which is not a claim that\n"
               "  nothing would be truncated.\n");
        hyp_ask_vectors_close(v);
        return 0;
    }
    if (rc != HYP_ASK_VEC_OK) {
        (void)fprintf(stderr, "embed: %s\n", hyp_ask_vectors_error(v));
        hyp_ask_vectors_close(v);
        return 1;
    }
    printf("ask index for '%s':\n", project);
    printf("  format           %d\n", m.format);
    printf("  model            %s\n", m.model_id ? m.model_id : "?");
    printf("  dim              %d\n", m.dim);
    printf("  window           %d tokens\n", m.window_tokens);
    printf("  rows             %lld\n", (long long)m.row_count);
    printf("  graph generation %s\n", m.graph_generation ? m.graph_generation : "?");
    /* Which POPULATION was indexed. Two indexes over the same corpus under the
     * two policies rank different candidate sets, so a recall number carried
     * from one to the other is not the same measurement. */
    printf("  whole-file spans %s\n",
           (m.whole_file_spans && m.whole_file_spans[0])
               ? m.whole_file_spans
               : "keep — this index predates the policy being recorded");
    printf("  built on         %s\n",
           (m.device_note && m.device_note[0]) ? m.device_note
                                               : "UNKNOWN — this index predates device recording");
    printf("  built at         %s\n", m.built_at && m.built_at[0] ? m.built_at : "(incomplete)");
    printf("  vector bytes     %lld  (%lld x %d x 4)\n",
           (long long)(m.row_count * (int64_t)m.dim * 4), (long long)m.row_count, m.dim);
    hyp_ask_vec_meta_free(&m);

    hyp_ask_trunc_t tr;
    if (hyp_ask_vectors_truncation(v, &tr) == HYP_ASK_VEC_OK) {
        char line[AC_LINEBUF];
        hyp_ask_trunc_describe(&tr, line, sizeof(line));
        printf("  %s\n", line);
        hyp_ask_trunc_free(&tr);
    }
    hyp_ask_vectors_close(v);
    return 0;
}


/* What the GPU can be asked for on this machine, priced BEFORE anything is
 * allocated.
 *
 * The pre-flight is not politeness. On a developer machine the compute GPU is
 * usually the display GPU, and llama.cpp's failure mode for over-allocating
 * there is not an error return — amdgpu resets the device, which tears down the
 * compositor's context along with everything else. Track 2 lost the desktop
 * session twice to a single mis-sized n_ctx. Refusing costs one multiplication.
 *
 * The KV cache, not the 1.2 GB of weights, is what bounds this: n_ctx is
 * n_seq_max * seq_len and is charged whether the documents fill it or not. */
static void ask_report_device_plan(hyp_ask_device_pref_t pref) {
    printf("embed: device requested = %s\n", hyp_ask_device_pref_name(pref));

    if (pref != HYP_ASK_DEVICE_CPU) {
        double total = 0.0;
        double used = 0.0;
        char gpu_name[128];
        /* SAME QUESTION, SAME ORDER as hyp_ask_llama_encoder_create: the
         * backend first, sysfs second. This report used to ask ONLY the AMD
         * sysfs reader, so on an Intel or NVIDIA card it printed "no device
         * exposes its VRAM ... falling back to CPU" and the encoder, one line
         * later, used the GPU. A pre-flight that can disagree with the run it
         * precedes is worse than none. */
        bool readable = hyp_ask_llama_probe_device(gpu_name, sizeof(gpu_name), &total, &used) ||
                        hyp_ask_device_vram_mib(&total, &used);
        if (!readable) {
            printf("  GPU: no device exposes its memory to this process, so a ceiling cannot be\n"
                   "       CHECKED — and it must never be assumed. %s\n",
                   pref == HYP_ASK_DEVICE_GPU
                       ? "--device gpu will REFUSE rather than guess."
                       : "Falling back to CPU.");
        } else {
            double ceiling = hyp_ask_gpu_ceiling_mib(total, used);
            int seq_len = hyp_ask_seq_len_for_kv(HYP_ASK_MAX_DOCS, ceiling);
            hyp_ask_kv_plan_t plan;
            bool ok = hyp_ask_kv_plan(HYP_ASK_MAX_DOCS, seq_len > 0 ? seq_len : 1, ceiling, &plan);
            printf("  GPU: %.0f MiB total, %.0f MiB already in use (this card also drives the\n"
                   "       display), so the ceiling is %.0f MiB of what is FREE.\n",
                   total, used, ceiling);
            if (ok && seq_len > 0) {
                printf("       plan: n_seq_max=%d x seq_len=%d -> n_ctx=%lld, KV %.0f MiB,\n"
                       "       %.0f MiB with the backend's fixed cost. ADMISSIBLE.\n",
                       plan.n_seq_max, plan.seq_len, (long long)plan.n_ctx, plan.kv_mib,
                       plan.total_mib);
                printf("       expected throughput ~%.1f declarations/s (~%.0f min per 4,000).\n",
                       HYP_ASK_GPU_DOCS_PER_SEC, 4000.0 / HYP_ASK_GPU_DOCS_PER_SEC / 60.0);
                /* seq_len here is the LARGEST admissible per-sequence length,
                 * not a recommendation. KV is charged for n_seq_max * seq_len
                 * whether the documents fill it or not, so a backend should
                 * size seq_len to the longest document it will accept and
                 * pocket the difference. And seq_len IS the effective window:
                 * a declaration longer than it is truncated and the counter
                 * must be denominated in this number, not in the model's
                 * %d-token maximum. */
                printf("       (seq_len above is the CEILING; size it to the longest declaration\n"
                       "        instead — KV is charged for the whole rectangle. seq_len is also\n"
                       "        the effective window the truncation counter is denominated in,\n"
                       "        not the model's %d.)\n",
                       HYP_ASK_MODEL_WINDOW);
            } else {
                printf("       plan: REFUSED before allocating — %.0f MiB free cannot hold the\n"
                       "       backend's fixed cost plus a usable KV cache. %s\n",
                       ceiling,
                       pref == HYP_ASK_DEVICE_GPU ? "--device gpu will not proceed."
                                                  : "Falling back to CPU.");
            }
        }
    }
    if (pref != HYP_ASK_DEVICE_GPU) {
        printf("  CPU fallback: ~%.3f declarations/s (~%.0f min per 4,000) — %.0fx slower than\n"
               "       the GPU path. Kept as a fallback, never as a silent one.\n",
               HYP_ASK_CPU_DOCS_PER_SEC, 4000.0 / HYP_ASK_CPU_DOCS_PER_SEC / 60.0,
               HYP_ASK_GPU_DOCS_PER_SEC / HYP_ASK_CPU_DOCS_PER_SEC);
    }
    /* Flushed so the notice reaches the terminal BEFORE the wait it warns
     * about, and before any unbuffered error from the run overtakes it. A
     * warning that arrives after the thing it warned about is not a warning. */
    (void)fflush(stdout);
}

int hyp_cmd_embed(int argc, char **argv) {
    hyp_ask_embed_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    bool status_only = false;
    bool verify_batching = false;
    bool use_stub = false;
    bool stub_reports_truncation = true;
    bool escalation = false;

    /* The environment is read FIRST so an explicit flag always wins. An
     * unrecognised value is refused rather than ignored: a typo that silently
     * selected the default would change which population got indexed and leave
     * no trace of having done so. */
    const char *env_wfs = getenv("HYP_ASK_WHOLE_FILE_SPANS");
    if (env_wfs && env_wfs[0] && !hyp_ask_whole_file_parse(env_wfs, &opts.whole_file_spans)) {
        (void)fprintf(stderr,
                      "embed: HYP_ASK_WHOLE_FILE_SPANS must be drop or keep (got '%s')\n", env_wfs);
        return 2;
    }

    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        bool has_next = (i + 1) < argc;
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            ask_embed_usage();
            return 0;
        } else if (strcmp(a, "--project") == 0 && has_next) {
            opts.project = argv[++i];
        } else if (strcmp(a, "--repo-path") == 0 && has_next) {
            opts.repo_path = argv[++i];
        } else if (strcmp(a, "--graph-db") == 0 && has_next) {
            opts.graph_db_path = argv[++i];
        } else if (strcmp(a, "--vectors-db") == 0 && has_next) {
            opts.vectors_db_path = argv[++i];
        } else if (strcmp(a, "--status") == 0) {
            status_only = true;
        } else if (strcmp(a, "--verify-batching") == 0) {
            verify_batching = true;
        } else if (strcmp(a, "--force") == 0) {
            opts.force = true;
        } else if (strcmp(a, "--allow-model-change") == 0) {
            opts.allow_model_change = true;
        } else if (strcmp(a, "--escalation") == 0) {
            /* EXPLICIT, ALWAYS. This pass spends tokens against somebody's
             * account, so it is a flag and never a fallback, an inference, or
             * something the local build turns on when it feels uncertain. */
            escalation = true;
        } else if (strcmp(a, "--limit") == 0 && has_next) {
            opts.limit = (int)strtol(argv[++i], NULL, 10);
        } else if (strcmp(a, "--budget") == 0 && has_next) {
            opts.token_budget = (int)strtol(argv[++i], NULL, 10);
        } else if (strcmp(a, "--max-docs") == 0 && has_next) {
            opts.max_docs = (int)strtol(argv[++i], NULL, 10);
        } else if (strcmp(a, "--device") == 0 && has_next) {
            const char *d = argv[++i];
            if (strcmp(d, "gpu") == 0) {
                opts.device_pref = HYP_ASK_DEVICE_GPU;
            } else if (strcmp(d, "cpu") == 0) {
                opts.device_pref = HYP_ASK_DEVICE_CPU;
            } else if (strcmp(d, "auto") == 0) {
                opts.device_pref = HYP_ASK_DEVICE_AUTO;
            } else {
                (void)fprintf(stderr, "embed: --device must be auto, gpu or cpu (got '%s')\n", d);
                return 2;
            }
        } else if (strcmp(a, "--whole-file-spans") == 0 && has_next) {
            const char *p = argv[++i];
            if (!hyp_ask_whole_file_parse(p, &opts.whole_file_spans)) {
                (void)fprintf(stderr,
                              "embed: --whole-file-spans must be drop or keep (got '%s')\n", p);
                return 2;
            }
        } else if (strcmp(a, "--stub") == 0) {
            use_stub = true;
        } else if (strcmp(a, "--stub-no-truncation-report") == 0) {
            use_stub = true;
            stub_reports_truncation = false;
        } else {
            (void)fprintf(stderr, "embed: unknown option '%s'\n\n", a);
            ask_embed_usage();
            return 2;
        }
    }

    if (!opts.project && !verify_batching) {
        (void)fprintf(stderr, "embed: --project is required\n\n");
        ask_embed_usage();
        return 2;
    }
    if (status_only) {
        return ask_cmd_status(opts.project);
    }
    if (verify_batching && !escalation) {
        /* No project, no index, nothing written: the encoder alone, asked the
         * one question the pass's output cannot answer about itself. */
        hyp_ask_encoder_t *enc = NULL;
        if (use_stub) {
            enc = hyp_ask_encoder_stub_create(HYP_ASK_DIM_DEFAULT, HYP_ASK_MODEL_WINDOW,
                                              stub_reports_truncation);
        } else if (!hyp_ask_llama_compiled_in()) {
            (void)fprintf(stderr,
                          "embed --verify-batching: no inference backend compiled in (%s)\n",
                          hyp_ask_llama_build_note());
            return 3;
        } else if (!hyp_model_ask_present() || !hyp_model_spec_present(hyp_model_ask_proj_spec())) {
            (void)fprintf(stderr,
                          "embed --verify-batching: the ask lane's model is not on this machine; "
                          "run `%s`\n",
                          HYP_MODEL_ASK_COMMAND);
            return 3;
        } else {
            char err[512];
            err[0] = '\0';
            enc = hyp_ask_llama_encoder_create(opts.device_pref, err, sizeof(err));
            if (!enc) {
                (void)fprintf(stderr, "embed --verify-batching: %s\n",
                              err[0] ? err : "the embedding backend could not be started");
                return 1;
            }
        }
        if (!enc) {
            (void)fprintf(stderr, "embed --verify-batching: could not create the encoder\n");
            return 1;
        }
        int vrc = ask_cmd_verify_batching(enc, opts.max_docs);
        hyp_ask_encoder_destroy(enc);
        return vrc;
    }
    /* THE TWO UNAVAILABLE STATES, SAID SEPARATELY. "No encoder in this build"
     * is fixed by a different binary; "the weights are not on this machine" is
     * fixed by one command. They have different remedies and must not share a
     * sentence. */
    /* The escalation lane needs no local weights at all, so it is decided
     * before every check that is about them. It also writes to ITS OWN file:
     * the two indexes are built by different models at different times and are
     * expected to drift apart, so they must never share a table. */
    char esc_db[HYP_SZ_4K];
    hyp_ask_encoder_t *esc_enc = NULL;
    if (escalation) {
        if (opts.vectors_db_path) {
            (void)fprintf(stderr,
                          "embed: --escalation writes the escalation lane's own index; pass one "
                          "of --escalation or --vectors-db, not both\n");
            return 2;
        }
        if (!hyp_ask_vectors_path_lane(opts.project, HYP_ASK_LANE_ESCALATION, esc_db,
                                       sizeof(esc_db))) {
            (void)fprintf(stderr, "embed: could not resolve the escalation index path\n");
            return 1;
        }
        opts.vectors_db_path = esc_db;

        /* The same cache directory `config set` writes to — hyp_config_open
         * takes the directory and returns NULL for a NULL one, so resolving it
         * here is what makes the two halves talk to the same database. */
        const char *cache_dir = hyp_resolve_cache_dir();
        hyp_config_t *cfg = cache_dir ? hyp_config_open(cache_dir) : NULL;
        char provider[64];
        char model[128];
        char key_env[128];
        (void)snprintf(provider, sizeof(provider), "%s",
                       cfg ? hyp_config_get(cfg, HYP_CONFIG_ASK_ESC_PROVIDER, "") : "");
        (void)snprintf(model, sizeof(model), "%s",
                       cfg ? hyp_config_get(cfg, HYP_CONFIG_ASK_ESC_MODEL, "") : "");
        (void)snprintf(key_env, sizeof(key_env), "%s",
                       cfg ? hyp_config_get(cfg, HYP_CONFIG_ASK_ESC_KEY_ENV, "") : "");
        if (cfg) {
            hyp_config_close(cfg);
        }
        char err[512];
        esc_enc = hyp_ask_provider_encoder_create(provider, model, key_env, err, sizeof(err));
        if (!esc_enc) {
            /* Never fall back to the local model. A caller who asked for the
             * expensive index and quietly got the cheap one has been told
             * something false about what it can search. */
            (void)fprintf(stderr,
                          "embed --escalation: %s\n\n"
                          "  Configure the lane first:\n"
                          "    hyponoia config set %s <provider>\n"
                          "    hyponoia config set %s <model>\n"
                          "    hyponoia config set %s <ENV_VAR_NAME>\n",
                          err, HYP_CONFIG_ASK_ESC_PROVIDER, HYP_CONFIG_ASK_ESC_MODEL,
                          HYP_CONFIG_ASK_ESC_KEY_ENV);
            return 3;
        }
        printf("embed --escalation: provider=%s model=%s key from $%s\n", provider, model,
               key_env);
        printf("  This pass SPENDS TOKENS against that account.\n");
    }
    if (!escalation && !use_stub && !hyp_ask_llama_compiled_in()) {
        (void)fprintf(stderr,
                      "embed: no inference backend is compiled in (%s).\n"
                      "  The only encoder this binary has is the deterministic stub — pass\n"
                      "  --stub to use it. A stub index exercises the storage, the batching\n"
                      "  and the truncation counter end to end and has NO retrieval quality.\n",
                      hyp_ask_llama_build_note());
        return 3;
    }
    /* The projection head is the SECOND required artifact, and its absence is
     * its own sentence: the GGUF alone emits a pre-projection vector that looks
     * perfectly well-formed and is in the wrong space. */
    if (!escalation && !use_stub && hyp_ask_llama_compiled_in() &&
        !hyp_model_spec_present(hyp_model_ask_proj_spec())) {
        (void)fprintf(stderr,
                      "embed: %s is missing — the weights and the projection head are both "
                      "required, and without it every vector would be in the wrong space.\n\n"
                      "  %s\n",
                      hyp_model_ask_proj_spec()->file, HYP_MODEL_ASK_COMMAND);
        return 3;
    }
    if (!escalation && !use_stub && !hyp_model_ask_present()) {
        char detail[HYP_MODEL_MSG_MAX];
        char remedy[HYP_MODEL_MSG_MAX];
        hyp_model_unavailable_text(opts.project, detail, sizeof(detail), remedy, sizeof(remedy));
        (void)fprintf(stderr, "embed: %s\n\n  %s\n", detail, remedy);
        return 3;
    }
    if (!escalation) {
        ask_report_device_plan(opts.device_pref);
    }

    hyp_ask_encoder_t *enc = NULL;
    if (escalation) {
        enc = esc_enc;
    } else if (use_stub) {
        enc = hyp_ask_encoder_stub_create(HYP_ASK_DIM_DEFAULT, HYP_ASK_MODEL_WINDOW,
                                          stub_reports_truncation);
        if (!enc) {
            (void)fprintf(stderr, "embed: could not create the stub encoder\n");
            return 1;
        }
    } else {
        char err[512];
        err[0] = '\0';
        enc = hyp_ask_llama_encoder_create(opts.device_pref, err, sizeof(err));
        if (!enc) {
            /* A refusal is an outcome, not a crash: --device gpu that cannot
             * be honoured says so here rather than running fifteen times
             * slower and looking like a success. */
            (void)fprintf(stderr, "embed: %s\n",
                          err[0] ? err : "the embedding backend could not be started");
            return 1;
        }
    }

    /* Captured before the encoder is destroyed: everything printed below is
     * about the encoder that ran, and after destroy there is nothing to ask. */
    char model_id[HYP_ASK_MODEL_ID_MAX];
    (void)snprintf(model_id, sizeof(model_id), "%s",
                   hyp_ask_encoder_model_id(enc) ? hyp_ask_encoder_model_id(enc) : "unreported");
    int dim = hyp_ask_encoder_dim(enc);
    int window = hyp_ask_encoder_window(enc);

    hyp_ask_embed_report_t rep;
    int rc = hyp_ask_embed_run(enc, &opts, &rep);
    hyp_ask_encoder_destroy(enc);
    if (rc < 0) {
        (void)fprintf(stderr, "embed: failed — see the log lines above\n");
        return 1;
    }
    if (rc == HYP_ASK_EMBED_NO_WORK) {
        hyp_ask_embed_report_free(&rep);
        /* Loud, and a non-zero exit. A run that indexed nothing must not look
         * like a run that indexed everything. */
        (void)fprintf(stderr,
                      "embed: NO DECLARATIONS FOUND for project '%s'.\n"
                      "  Nothing was embedded and no index was built. This is not a success:\n"
                      "  either the project name is wrong, or index_repository has not stored\n"
                      "  any node with a source span. `hyponoia cli list_projects` lists what\n"
                      "  is actually indexed.\n",
                      opts.project);
        return 4;
    }

    /* The model id is READ FROM THE ENCODER THAT RAN. It was a compiled-in
     * constant while the stub was the only encoder there was, and the first
     * real GPU run printed `stub/deterministic-hash-v1` over a real index —
     * a report that names the wrong model is worse than one that names none,
     * because provenance is the gate `ask` refuses a mixed index on. */
    printf("embed: project=%s model=%s dim=%d window=%d\n", opts.project, model_id, dim, window);
    /* First line of the result, not a footnote. Which device ran is the single
     * biggest determinant of what this cost. */
    printf("  DEVICE USED        %s\n", rep.device_note ? rep.device_note : "unreported");
    if (rep.device_downgraded) {
        printf("  ** a GPU was requested and NOT used — this run was ~%.0fx slower than the\n"
               "     device that was asked for **\n",
               HYP_ASK_GPU_DOCS_PER_SEC / HYP_ASK_CPU_DOCS_PER_SEC);
    }
    printf("  declarations seen  %lld\n", (long long)rep.declarations_seen);
    printf("  embedded           %lld\n", (long long)rep.embedded);
    printf("  reused (unchanged) %lld\n", (long long)rep.reused);
    printf("  unreadable spans   %lld\n", (long long)rep.skipped_unreadable);
    printf("  whole-file spans   %s — %lld dropped, %lld kept as a file's only row\n",
           hyp_ask_whole_file_name(opts.whole_file_spans), (long long)rep.skipped_whole_file,
           (long long)rep.whole_file_kept_sole);
    printf("  pruned (gone)      %lld\n", (long long)rep.pruned);
    printf("  forward passes     %lld  (%.1f docs/pass)\n", (long long)rep.forward_passes,
           rep.forward_passes ? (double)rep.embedded / (double)rep.forward_passes : 0.0);
    printf("  padded slots       %lld  (largest single rectangle %lld)\n",
           (long long)rep.padded_slots, (long long)rep.max_rect);
    printf("  elapsed            %.0f ms\n", rep.elapsed_ms);
    if (rep.partial) {
        printf("  PARTIAL: --limit stopped the run, so this index does not cover the corpus\n");
    }

    /* Disclosed on the build too, not only on answers: `embed` is the one
     * command that KNOWS the number, and `ask` can only disclose what `embed`
     * recorded. */
    hyp_ask_vectors_t *v = opts.vectors_db_path
                               ? hyp_ask_vectors_open_path(opts.project, opts.vectors_db_path)
                               : hyp_ask_vectors_open(opts.project);
    if (v) {
        hyp_ask_trunc_t tr;
        if (hyp_ask_vectors_truncation(v, &tr) == HYP_ASK_VEC_OK) {
            char line[AC_LINEBUF];
            hyp_ask_trunc_describe(&tr, line, sizeof(line));
            printf("  %s\n", line);
            hyp_ask_trunc_free(&tr);
        }
        hyp_ask_vectors_close(v);
    }
    hyp_ask_embed_report_free(&rep);
    return 0;
}
