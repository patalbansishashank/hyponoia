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
 *              already computed, so the prefix of a wider vector, renormalised,
 *              IS the smaller vector, and one pass serves every dimension.
 * REEMBEDS   — the requested dimension is a separate output head. Vectors at
 *              two widths are DIFFERENT SPACES and each costs its own pass.
 *              No row carries this today; the value stays because the
 *              distinction is real and a future provider may need it.
 * FIXED      — no dimension parameter; take the native width.
 *
 * THIS TAG IS A STATEMENT ABOUT SPACES, NOT ABOUT WHETHER A WIDTH IS WORTH
 * STORING. Those are separate measurements. Voyage was tagged REEMBEDS from
 * 2026-08-12 (ec6bde21) to 2026-08-15 because §2.7 had paid for two passes to
 * learn that voyage-code-3 at 2048 buys nothing (MRR@10 0.57323 at 1024 ->
 * 0.56189 at 2048 on the frozen 60); the utility result stands, but running
 * two passes was a choice, not a property of the API. §2.15 asked the API
 * (engine/xmodel/dims.json): voyage-4-large's output_dimension=1024 is the
 * renormalised first 1024 of its own output_dimension=2048, min cosine
 * 1.000000. That is TRUNCATES.
 *
 * NO CODE PATH READS THIS FIELD. The adapter below always requests HYP_ASK_DIM
 * from the provider and never truncates a wider vector locally, so the tag
 * cannot make a build wrong — which is also why nothing failed while it was.
 * It is pinned by tests/test_ask_provider.c so it cannot drift silently. */
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

/* Wrap a provider as an ordinary encoder, so the escalation build runs through
 * the SAME embed pass as the local one — same batching, same reuse-by-hash,
 * same truncation disclosure, same provenance stamping. The only thing that
 * differs is where the vectors come from.
 *
 * `key_env` is the NAME of an environment variable; the key is read once here
 * and held for the life of the encoder, never copied into config or a log.
 * Returns NULL with a sentence in `err` if the provider is unknown or unwired,
 * or the variable is unset.
 *
 * Declared here rather than in ask_encoder.h to keep the encoder interface free
 * of any provider concept; this is an adapter, not a second kind of encoder. */
struct hyp_ask_encoder;
struct hyp_ask_encoder *hyp_ask_provider_encoder_create(const char *provider_name,
                                                        const char *model, const char *key_env,
                                                        char *err, size_t errlen);

#endif /* HYP_ASK_PROVIDER_H */
