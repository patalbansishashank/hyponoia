/*
 * ask_provider.h — the escalation lane's API encoders.
 *
 * Two entry points, `embed_documents` and `embed_query`, and a table. The table
 * is the point: what actually differs between hosted embedding providers is not
 * their protocol — they are all "POST some JSON, get back an array of floats" —
 * it is the SPELLING OF THE ASYMMETRY PARAMETER, and whether asking for a
 * smaller dimension truncates an existing vector or re-embeds into a new space.
 *
 * WHAT DOES NOT GENERALISE IS THE CONTRACT, AND IT IS NOT CODE. This interface
 * makes a provider callable; it says nothing about whether the model behind it
 * is any good, or which of its legs to use. Every encoder measured so far
 * wanted a different contract: one gained +0.206 MRR@10 from an instruct
 * prefix, another was DESTROYED by that same prefix, a third wanted its own
 * retrieval adapter and was hurt by ours, a fourth an `input_type`, a fifth two
 * literal strings — and one model wanted OPPOSITE contracts on two corpora.
 *
 * So: adding a row to this table makes a provider reachable. It does not make it
 * trusted. Measure its legs on a held corpus before shipping it as an option.
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
 * exercised; Gemini's row is declared and marked unimplemented rather than
 * left silently half-working — a provider that returns a confident error is
 * safer than one that returns vectors from a request it built wrong. */
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
 * STORING. Those are two different measurements, and the second is the easier
 * one to mistake for the first: finding that a wider vector buys no accuracy
 * says nothing about whether the narrow one is its renormalised prefix. Only
 * asking the provider settles that. Voyage's answer is that
 * output_dimension=1024 IS the renormalised first 1024 of its own
 * output_dimension=2048, min cosine 1.000000 — so its row is TRUNCATES.
 *
 * ONE CODE PATH READS THIS FIELD: `ask(escalate=true)` in
 * ask.escalation.mode=query (src/mcp/mcp.c) refuses a provider tagged REEMBEDS,
 * because it scores a query the provider encoded at the LOCAL index's width
 * against document vectors another model truncated to that width — an operation
 * that is only the same on both sides when "smaller dimension" means "the
 * renormalised prefix of the wider vector" on both sides. A re-embedding head
 * is a different space at a different width, so the gate says no. No row
 * carries REEMBEDS, so the branch is dormant; it is what makes a future row's
 * tag load-bearing.
 *
 * A WRONG TAG CANNOT MAKE AN INDEX WRONG, and that is the hazard rather than
 * the comfort. The adapter always requests HYP_ASK_DIM from the provider and
 * never truncates a wider vector locally, so a mis-tagged row changes no
 * vector, fails no test and produces no symptom — it simply waits for the
 * dormant branch above to become live. Nothing here will catch it; the pin in
 * tests/test_ask_provider.c is what does. */
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
 * `err` naming THE VARIABLE, never a value. Callers must not log the result.
 *
 * WHOSE environment that is, is a separate question — see the custody block
 * below. This function reads THIS PROCESS'S, which is not always the asking
 * one's. */
const char *hyp_ask_provider_key(const char *key_env, char *err, size_t errlen);

/* ── Key custody: WHOSE environment `getenv` above is reading ──────
 *
 * The rule at the top of this file — the key is never stored, never logged,
 * never in argv, read at the moment of use — is a rule about the VALUE. It
 * says nothing about whose environment the value is read out of, and that is a
 * separate question with its own answer.
 *
 * The observation that forces it: an escalated `ask` answers
 * `lane: escalation-query` for a client whose environment has no key at all.
 * The read is per request (hyp_ask_provider_encoder_create runs on every
 * escalated call), but `environ` is a snapshot taken at exec, so a daemon
 * started from a shell that exported the key holds that key for its whole
 * life — and `hyponoia`'s daemon serves EVERY MCP session, every CLI tool
 * invocation, and the graph UI's HTTP routes. Any local process of this uid
 * that can reach the socket could therefore spend against the owner's account
 * without ever holding the key.
 *
 * That is not a privilege escalation — it is same-uid and local-only, and a
 * process that can reach the socket can also read /proc/<daemon>/environ, so
 * the key was never hidden from it. It IS a spending surface nobody declared,
 * which is the failure this project's escalation lane exists to avoid: the
 * whole lane is off by default, per call, and refuses rather than falling
 * back, precisely so money never moves by surprise. A warm daemon quietly
 * lending its key to whoever connects is that surprise wearing a different
 * hat.
 *
 * So the process declares which case it is in, ONCE, before it serves
 * anything:
 *
 *   CALLER — this process is the asking one, or was spawned by it and
 *            inherited its environment: `embed --escalation`, a hook client,
 *            any direct CLI command. Reading the key here spends the caller's
 *            own key. This is the DEFAULT, so a process that says nothing is
 *            treated as the caller's own — which is the safe way round,
 *            because it is the case where no gate is needed.
 *   SHARED — this process outlives and serves OTHER processes: the daemon
 *            (and therefore the graph UI's HTTP server, which only ever runs
 *            inside it), and the index workers it forks. Its environment
 *            belongs to whoever started it, not to whoever is asking now.
 *
 * Worth knowing before judging the default: THERE IS NO IN-PROCESS MCP SERVER
 * in this build — the stdio entry point is a thin frontend to the daemon, and
 * `hyponoia cli <tool>` connects to it too — so every `ask(escalate=true)` is
 * served under SHARED custody, and default-refuse means escalated ask needs
 * one `config set` before it works. That is the intended trade: one line of
 * setup, beside the three the lane already requires, against a spending
 * surface that is on by default and mentioned nowhere.
 *
 * Under SHARED custody the key is read only when the owner has said so, via
 * `ask.escalation.daemon_key` (default `refuse`). The gate lives inside
 * hyp_ask_provider_encoder_create, below the enumeration of call sites, so a
 * future call site cannot forget it. */
typedef enum {
    HYP_ASK_KEY_CUSTODY_CALLER = 0,
    HYP_ASK_KEY_CUSTODY_SHARED = 1,
} hyp_ask_key_custody_t;

/* Declare this process a shared, long-lived server. `holder` is a short human
 * sentence naming it for a disclosure — e.g. "hyponoia daemon pid 3904334,
 * holding it since 2026-08-16T10:57:04Z". It is copied; NEVER pass anything
 * derived from a key value.
 *
 * Call it once, before the process serves its first request and before it
 * starts threads: the state is written once and read-only thereafter, which is
 * what makes it safe to read from a request thread without a lock. */
void hyp_ask_provider_declare_shared_key_custody(const char *holder);
hyp_ask_key_custody_t hyp_ask_provider_key_custody(void);
/* "" under CALLER custody. Never contains a key. */
const char *hyp_ask_provider_key_holder(void);
/* Back to CALLER. Only for tests, which drive both custodies in one process. */
void hyp_ask_provider_clear_key_custody_for_test(void);

/* The gate, exported so a caller can refuse with its OWN remedy sentence
 * rather than wrapping this one in advice about configuring a provider that is
 * already configured. True means "this process must not read the key on the
 * caller's behalf"; `err` then holds the complete refusal — who would have
 * paid, who else could, and the one command that changes it. `key_env` is a
 * variable NAME and is echoed; passing a value here would print it.
 *
 * hyp_ask_provider_encoder_create applies this itself as well, so the gate
 * holds even where nobody remembered to ask. */
bool hyp_ask_provider_key_custody_refused(bool shared_allowed, const char *key_env, char *err,
                                          size_t errlen);

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
 * `shared_key_allowed` is the owner's `ask.escalation.daemon_key` decision,
 * and it is a REQUIRED argument rather than something read from config here so
 * that ask_provider stays free of any config concept — and so that adding a
 * call site is a compile error until its author has thought about custody. It
 * is ignored under CALLER custody, where there is nothing to gate.
 *
 * Declared here rather than in ask_encoder.h to keep the encoder interface free
 * of any provider concept; this is an adapter, not a second kind of encoder. */
struct hyp_ask_encoder;
struct hyp_ask_encoder *hyp_ask_provider_encoder_create(const char *provider_name,
                                                        const char *model, const char *key_env,
                                                        bool shared_key_allowed, char *err,
                                                        size_t errlen);

#endif /* HYP_ASK_PROVIDER_H */
