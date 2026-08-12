/*
 * ask_provider.h — The escalation lane's API encoders (NEXT-STEPS.md §2.10 step 2).
 *
 * Two entry points, `embed_documents` and `embed_query`, and a table. The table
 * is the point: what actually differs between hosted embedding providers is not
 * their protocol — they are all "POST some JSON, get back an array of floats" —
 * it is the SPELLING OF THE ASYMMETRY PARAMETER, and whether asking for a
 * smaller dimension truncates an existing vector or re-embeds into a new space.
 *
 * WHAT DOES NOT GENERALISE IS THE CONTRACT, AND IT IS NOT CODE. This interface
 * makes a provider callable; it says nothing about whether the model behind it
 * is any good, or which of its legs to use. Five encoders have been measured
 * across §2.5-§2.9 and every one wanted a different contract: Qwen3 an instruct
 * prefix worth +0.206 MRR@10, pplx DESTROYED by that same prefix, Jina its own
 * retrieval adapter (and hurt by ours), Voyage an `input_type`, nano two literal
 * strings. §2.9 then found one model wanting OPPOSITE contracts on two corpora.
 *
 * So: adding a row to this table makes a provider reachable. It does not make it
 * trusted. Measure its legs on the frozen 60 before shipping it as an option,
 * the way §2.6, §2.7 and §2.9 each did.
 *
 * THE KEY IS NEVER STORED, LOGGED, OR PLACED IN argv. Config holds the NAME of
 * an environment variable (`ask.escalation.key_env`); the value is read at the
 * moment of use and handed to curl through a 0600 config file, because argv is
 * world-readable in /proc on Linux and a key in a `ps` listing is a leaked key.
 */
#ifndef HYP_ASK_PROVIDER_H
#define HYP_ASK_PROVIDER_H

#include <stdbool.h>
#include <stddef.h>

/* Providers this build knows how to talk to. Voyage and Jina are WIRED and
 * exercised; Gemini's row is present because §2.10 asks for the table to be
 * declared, and it is marked unimplemented rather than silently half-working —
 * a provider that returns a confident error is safer than one that returns
 * vectors from a request it built wrong. */
typedef enum {
    HYP_ASK_PROVIDER_UNKNOWN = 0,
    HYP_ASK_PROVIDER_VOYAGE = 1,
    HYP_ASK_PROVIDER_JINA = 2,
    HYP_ASK_PROVIDER_GEMINI = 3,
} hyp_ask_provider_id_t;

/* What a request for a smaller dimension actually does. This is not a detail:
 * it decides whether one document pass can serve several dimensions, and
 * whether an index at dim 1024 is comparable to one at the native width.
 *
 * TRUNCATES  — Matryoshka. The provider cuts and renormalises a vector it
 *              already computed, so the prefix of a native vector IS the
 *              smaller vector, and one pass serves every dimension.
 * REEMBEDS   — the requested dimension is a separate output head. Vectors at
 *              1024 and 2048 are DIFFERENT SPACES and each costs its own pass;
 *              §2.7 paid for both to find 2048 bought nothing (0.57323 ->
 *              0.56189 on the frozen 60).
 * FIXED      — no dimension parameter; take the native width. */
typedef enum {
    HYP_ASK_DIM_FIXED = 0,
    HYP_ASK_DIM_TRUNCATES = 1,
    HYP_ASK_DIM_REEMBEDS = 2,
} hyp_ask_dim_mode_t;

typedef struct {
    hyp_ask_provider_id_t id;
    const char *name;     /* what a user types in `config set` */
    const char *endpoint; /* full URL, POST, JSON in and out */

    /* THE ONE THING THAT GENUINELY DIFFERS. Same concept, three spellings, and
     * getting it wrong does not fail — it returns vectors from the wrong leg. */
    const char *asym_param;    /* "input_type" | "task" | "task_type" */
    const char *asym_document; /* the document-side value */
    const char *asym_query;    /* the query-side value */

    /* The JSON key holding the input array, and the key holding the model name.
     * Gemini spells both differently from the other two. */
    const char *input_key;
    const char *model_key;

    int native_dim;
    const char *dim_param; /* NULL when the provider takes no dimension */
    hyp_ask_dim_mode_t dim_mode;

    /* Per-request ceilings. Both bind: Voyage caps a single request at 1,000
     * rows AND at a token count that varies by model, and a batch over either
     * is rejected however many times it is retried. */
    int max_rows_per_request;
    int max_tokens_per_request;

    bool implemented;
} hyp_ask_provider_t;

/* Look a provider up by the name a user typed. NULL when unknown — callers must
 * report the name back rather than silently defaulting to one that exists. */
const hyp_ask_provider_t *hyp_ask_provider_by_name(const char *name);
const hyp_ask_provider_t *hyp_ask_provider_by_id(hyp_ask_provider_id_t id);

/* Every provider this build knows, for `config set` to list on a bad value. */
const hyp_ask_provider_t *hyp_ask_provider_table(size_t *count);

/* The contract token stamped on an index built through a provider, e.g.
 * "voyage/input_type=document|query". Written into `buf`; returns false if it
 * would not fit. Derived from the table rather than hand-written so the stamp
 * cannot drift from the request that was actually sent. */
bool hyp_ask_provider_contract(const hyp_ask_provider_t *p, char *buf, size_t buflen);

/* Read the key named by `key_env` out of the environment.
 *
 * Returns NULL when the variable is unset or empty, and writes a sentence to
 * `err` naming THE VARIABLE, never a value. Callers must not log the result. */
const char *hyp_ask_provider_key(const char *key_env, char *err, size_t errlen);

/* Encode `count` documents / one query. Writes `count * dim` (or `dim`)
 * float32s, unit-normalised, row i for text i IN THE ORDER GIVEN.
 *
 * `dim` may be 0 for the provider's native width. Returns 0 on success and
 * non-zero on failure, always leaving a caller-facing sentence in `err`. There
 * is no partial success: a short or reordered batch is a silent corpus
 * corruption, so the whole call fails instead. */
int hyp_ask_provider_embed_documents(const hyp_ask_provider_t *p, const char *model,
                                     const char *key, const char *const *texts, int count, int dim,
                                     float *out, char *err, size_t errlen);
int hyp_ask_provider_embed_query(const hyp_ask_provider_t *p, const char *model, const char *key,
                                 const char *text, int dim, float *out, char *err, size_t errlen);

#endif /* HYP_ASK_PROVIDER_H */
