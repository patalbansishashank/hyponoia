/*
 * ask_embed.c — the backend registry for the `ask` lane.
 *
 * Deliberately the whole of it. Everything an embedding backend does lives
 * behind `hyp_ask_backend_t`; this file only decides which one is installed
 * and refuses the ones that would corrupt an index silently.
 *
 * Nothing is installed by default. That is not a stub in the "unfinished"
 * sense — it is the shipped state until llama.cpp/GGUF is settled, and it is
 * the state `ask` must handle honestly rather than by returning zero rows.
 */
#include "semantic/ask_embed.h"

#include "foundation/constants.h"

#include <stddef.h>

static const hyp_ask_backend_t *g_backend = NULL;

int hyp_ask_backend_install(const hyp_ask_backend_t *b) {
    if (!b) {
        g_backend = NULL;
        return 0;
    }
    /* Every rejection here is a mismatch that produces plausible numbers
     * rather than an error if it is let through: a nameless model cannot be
     * refused on reuse, a missing encode_query cannot answer anything, and a
     * dim mismatch turns "cosine" into an arithmetic accident. */
    if (!b->model_id || !b->model_id[0] || !b->encode_query || b->dim != HYP_ASK_DIM) {
        return HYP_NOT_FOUND;
    }
    g_backend = b;
    return 0;
}

const hyp_ask_backend_t *hyp_ask_backend(void) {
    return g_backend;
}
