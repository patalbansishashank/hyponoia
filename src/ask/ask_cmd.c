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
#include "ask/ask_vectors.h"

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
           "  --force                Re-encode even byte-identical declarations.\n"
           "  --allow-model-change   Discard vectors from a different model/dim/window.\n"
           "  --limit <n>            Stop after n declarations (partial index).\n"
           "  --budget <slots>       Padded-slot ceiling per forward pass (default %d).\n"
           "  --max-docs <n>         Documents per forward pass (default %d).\n"
           "  --stub                 Use the deterministic stub encoder.\n"
           "  --stub-no-truncation-report\n"
           "                         Stub that cannot report token counts, so the\n"
           "                         truncation state comes out UNKNOWN.\n"
           "\n"
           "Without an inference backend, --stub is the only encoder available. A stub\n"
           "index carries model id `%s` and can never be silently\n"
           "mistaken for a real one: its vectors have the right shape and no retrieval\n"
           "quality at all.\n",
           HYP_ASK_TOKEN_BUDGET, HYP_ASK_MAX_DOCS, HYP_ASK_STUB_MODEL_ID);
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

int hyp_cmd_embed(int argc, char **argv) {
    hyp_ask_embed_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    bool status_only = false;
    bool use_stub = false;
    bool stub_reports_truncation = true;

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
        } else if (strcmp(a, "--force") == 0) {
            opts.force = true;
        } else if (strcmp(a, "--allow-model-change") == 0) {
            opts.allow_model_change = true;
        } else if (strcmp(a, "--limit") == 0 && has_next) {
            opts.limit = (int)strtol(argv[++i], NULL, 10);
        } else if (strcmp(a, "--budget") == 0 && has_next) {
            opts.token_budget = (int)strtol(argv[++i], NULL, 10);
        } else if (strcmp(a, "--max-docs") == 0 && has_next) {
            opts.max_docs = (int)strtol(argv[++i], NULL, 10);
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

    if (!opts.project) {
        (void)fprintf(stderr, "embed: --project is required\n\n");
        ask_embed_usage();
        return 2;
    }
    if (status_only) {
        return ask_cmd_status(opts.project);
    }
    if (!use_stub) {
        /* Refusing is the right answer here. The inference runtime is a
         * separate piece of work; guessing one, or silently falling back to the
         * stub, would produce an index that looks real and retrieves nothing.
         *
         * When the backend lands this message splits in two, because the lane
         * has two distinct unavailable states: the model has not been fetched
         * (it is downloaded on first use of this lane, ~639 MB at Q8_0, SHA-256
         * verified) versus the index has not been built. They have different
         * remedies and must not share a sentence. */
        (void)fprintf(stderr,
                      "embed: no inference backend is compiled in.\n"
                      "  The document/query encoder is a separate track. Until it lands, the\n"
                      "  only encoder available is the deterministic stub — pass --stub to use\n"
                      "  it. A stub index exercises the storage, the batching and the\n"
                      "  truncation counter end to end and has NO retrieval quality.\n");
        return 3;
    }

    /* Say what the wait is before the wait, not after. The in-binary lane is
     * CPU-only and was measured at 1.581 declarations per second; lld/ELF's
     * 4,117 declarations are about 43 minutes. The stub is not the backend, so
     * this notice is about the shape of the real run, not this one. */
    printf("embed: the in-binary lane is CPU-only, measured at %.3f declarations/s\n"
           "  (~%.0f minutes per 4,000 declarations). GPU indexing is %.0fx faster and\n"
           "  belongs to the out-of-process extractor.\n",
           HYP_ASK_CPU_DOCS_PER_SEC, 4000.0 / HYP_ASK_CPU_DOCS_PER_SEC / 60.0,
           24.09 / HYP_ASK_CPU_DOCS_PER_SEC);
    /* Flushed here so the notice reaches the terminal BEFORE the wait it is
     * warning about, and before any unbuffered error from the run below
     * overtakes it. A warning that arrives after the thing it warned about is
     * not a warning. */
    (void)fflush(stdout);

    hyp_ask_encoder_t *enc =
        hyp_ask_encoder_stub_create(HYP_ASK_DIM_DEFAULT, HYP_ASK_MODEL_WINDOW,
                                    stub_reports_truncation);
    if (!enc) {
        (void)fprintf(stderr, "embed: could not create the stub encoder\n");
        return 1;
    }

    hyp_ask_embed_report_t rep;
    int rc = hyp_ask_embed_run(enc, &opts, &rep);
    hyp_ask_encoder_destroy(enc);
    if (rc < 0) {
        (void)fprintf(stderr, "embed: failed — see the log lines above\n");
        return 1;
    }
    if (rc == HYP_ASK_EMBED_NO_WORK) {
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

    printf("embed: project=%s model=%s dim=%d window=%d\n", opts.project,
           HYP_ASK_STUB_MODEL_ID, HYP_ASK_DIM_DEFAULT, HYP_ASK_MODEL_WINDOW);
    printf("  declarations seen  %lld\n", (long long)rep.declarations_seen);
    printf("  embedded           %lld\n", (long long)rep.embedded);
    printf("  reused (unchanged) %lld\n", (long long)rep.reused);
    printf("  unreadable spans   %lld\n", (long long)rep.skipped_unreadable);
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
    return 0;
}
