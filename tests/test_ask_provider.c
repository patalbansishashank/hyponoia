/*
 * test_ask_provider.c — the provider table's invariants.
 *
 * No network. What is worth pinning here is not that HTTP works, it is that the
 * table cannot quietly acquire a row that lies: a provider whose asymmetry
 * values are missing or identical would send the same request for documents and
 * queries and return plausible vectors from the wrong leg. That failure has no
 * downstream signal, which is why it is a test.
 */
#include "ask/ask_provider.h"

#include <string.h>

#include "ask/ask_encoder.h"
#include "cli/cli.h"
#include "foundation/compat.h"
#include "foundation/constants.h"
#include "test_framework.h"

TEST(ask_provider_table_rows_are_complete_and_asymmetric) {
    size_t n = 0;
    const hyp_ask_provider_t *t = hyp_ask_provider_table(&n);
    ASSERT_NOT_NULL(t);
    ASSERT_TRUE(n >= 2);
    for (size_t i = 0; i < n; i++) {
        ASSERT_NOT_NULL(t[i].name);
        ASSERT_NOT_NULL(t[i].endpoint);
        ASSERT_NOT_NULL(t[i].asym_param);
        ASSERT_NOT_NULL(t[i].asym_document);
        ASSERT_NOT_NULL(t[i].asym_query);
        ASSERT_NOT_NULL(t[i].input_key);
        ASSERT_NOT_NULL(t[i].model_key);
        ASSERT_TRUE(t[i].native_dim > 0);
        ASSERT_TRUE(t[i].max_rows_per_request > 0);
        /* THE ONE THAT MATTERS. Equal values are not a typo that shows up as a
         * crash; they are a silent 50% loss of the mechanism the whole lane
         * exists for — Jina's own retrieval adapter is worth +31% RR over
         * sending no task at all. */
        ASSERT_TRUE(strcmp(t[i].asym_document, t[i].asym_query) != 0);
    }
    PASS();
}

/* THIS PIN IS THE ONLY THING THAT CAN CATCH A WRONG dim_mode, and the reason
 * is uncomfortable: almost no code path reads the field. The adapter always
 * asks the API for HYP_ASK_DIM and never cuts a wider vector itself, so a
 * mis-tagged row produces no wrong vector, no failing test and no symptom —
 * a wrong tag can sit in the table indefinitely and nothing in the product
 * will say so. One path does read it: `ask(escalate=true)` in query mode
 * refuses a REEMBEDS provider, because scoring its 1024 against a matrix
 * another model truncated to 1024 is cross-space. That makes this pin
 * load-bearing rather than documentary.
 *
 * TRUNCATES is the answer the API gives, and it is a different question from
 * whether a width is worth storing. Running a model at two widths as two
 * document passes is how an experiment can be run; it is not evidence about
 * the API. Asked directly, voyage-4-large's output_dimension=1024 IS the
 * renormalised first 1024 of its own output_dimension=2048, min cosine
 * 1.000000, every returned width unit-norm — one pass serves both. The
 * separate finding that 2048 scores 0.56189 MRR@10 against 0.57323 at 1024 is
 * about storage, and leaves the space question untouched. */
TEST(ask_provider_voyage_dimension_parameter_truncates_it_does_not_reembed) {
    const hyp_ask_provider_t *v = hyp_ask_provider_by_name("voyage");
    ASSERT_NOT_NULL(v);
    ASSERT_EQ(v->dim_mode, HYP_ASK_DIM_TRUNCATES);
    ASSERT_STR_EQ(v->dim_param, "output_dimension");
    /* Jina's TRUNCATES is measured the same way: cutting to 512 costs -10.5%
     * RR, which is a truncation cost, not a second pass. */
    const hyp_ask_provider_t *j = hyp_ask_provider_by_name("jina");
    ASSERT_NOT_NULL(j);
    ASSERT_EQ(j->dim_mode, HYP_ASK_DIM_TRUNCATES);

    /* Table-wide: a dimension parameter and a dimension mode come together. A
     * row that takes a width but is FIXED, or claims a mode with no parameter
     * to express it, is a row whose comment and whose request disagree. */
    size_t n = 0;
    const hyp_ask_provider_t *t = hyp_ask_provider_table(&n);
    for (size_t i = 0; i < n; i++) {
        ASSERT_TRUE((t[i].dim_param != NULL) == (t[i].dim_mode != HYP_ASK_DIM_FIXED));
    }
    PASS();
}

TEST(ask_provider_lookup_refuses_unknown_names) {
    ASSERT_NOT_NULL(hyp_ask_provider_by_name("voyage"));
    ASSERT_NOT_NULL(hyp_ask_provider_by_name("jina"));
    /* Never fall back to a provider that exists — a typo must be reported, not
     * silently served by whoever happens to be first in the table. */
    ASSERT_TRUE(hyp_ask_provider_by_name("voyag") == NULL);
    ASSERT_TRUE(hyp_ask_provider_by_name("") == NULL);
    ASSERT_TRUE(hyp_ask_provider_by_name(NULL) == NULL);
    PASS();
}

TEST(ask_provider_contract_names_both_sides_of_the_asymmetry) {
    char buf[256];
    ASSERT_TRUE(hyp_ask_provider_contract(hyp_ask_provider_by_name("voyage"), buf, sizeof(buf)));
    ASSERT_STR_EQ(buf, "voyage/input_type=document|query");
    ASSERT_TRUE(hyp_ask_provider_contract(hyp_ask_provider_by_name("jina"), buf, sizeof(buf)));
    ASSERT_STR_EQ(buf, "jina/task=retrieval.passage|retrieval.query");
    /* Refuses rather than truncating: a clipped contract string would be
     * stamped on an index and compared against the full one forever after. */
    char tiny[8];
    ASSERT_FALSE(hyp_ask_provider_contract(hyp_ask_provider_by_name("voyage"), tiny, sizeof(tiny)));
    PASS();
}

TEST(ask_provider_key_is_read_from_the_environment_and_never_echoed) {
    char err[512];
    /* Unset variable: the error names the VARIABLE and cannot contain a value. */
    const char *k = hyp_ask_provider_key("HYP_TEST_DEFINITELY_UNSET_KEY_VAR", err, sizeof(err));
    ASSERT_TRUE(k == NULL);
    ASSERT_TRUE(strstr(err, "HYP_TEST_DEFINITELY_UNSET_KEY_VAR") != NULL);

    /* No variable configured at all is its own message, not the same one. */
    err[0] = '\0';
    ASSERT_TRUE(hyp_ask_provider_key(NULL, err, sizeof(err)) == NULL);
    ASSERT_TRUE(strstr(err, "key_env") != NULL);

    hyp_setenv("HYP_TEST_PROVIDER_KEY_VAR", "sk-not-a-real-key", 1);
    err[0] = '\0';
    k = hyp_ask_provider_key("HYP_TEST_PROVIDER_KEY_VAR", err, sizeof(err));
    ASSERT_NOT_NULL(k);
    ASSERT_STR_EQ(k, "sk-not-a-real-key");
    ASSERT_STR_EQ(err, "");

    /* An empty variable is "not set", not a key of length zero. */
    hyp_setenv("HYP_TEST_PROVIDER_KEY_VAR", "", 1);
    ASSERT_TRUE(hyp_ask_provider_key("HYP_TEST_PROVIDER_KEY_VAR", err, sizeof(err)) == NULL);
    PASS();
}

TEST(ask_provider_declared_but_unwired_refuses_instead_of_guessing) {
    const hyp_ask_provider_t *g = hyp_ask_provider_by_name("gemini");
    ASSERT_NOT_NULL(g);
    ASSERT_FALSE(g->implemented);
    float out[8];
    char err[512];
    const char *txt[1] = {"x"};
    /* Its body nests each input and it authenticates on a query parameter, so
     * the shared request builder would send something well-formed and wrong. */
    ASSERT_TRUE(hyp_ask_provider_embed_documents(g, "m", "k", txt, 1, 0, out, err, sizeof(err)) !=
                0);
    ASSERT_TRUE(strstr(err, "not wired") != NULL);
    PASS();
}

/* The config DB is backed up and sits beside a graph.db.zst that
 * gets shared, so a key pasted into key_env is a key handed out. Documentation
 * saying "name, not key" is not a mechanism; this is. */
TEST(ask_provider_config_refuses_a_key_pasted_where_a_variable_name_goes) {
    char err[512];

    /* The two vendor shapes this lane actually handles, plus OpenAI's.
     * THE PREFIX IS WHAT IS UNDER TEST — the body is filler on purpose, and
     * that is a rule rather than a style: writing a fixture like this with a
     * live key puts a working key in the very file that exists to prevent one.
     * A test for "does this look like a vendor key" never needs a real one. */
    ASSERT_TRUE(hyp_config_validate(HYP_CONFIG_ASK_ESC_KEY_ENV, "pa-abc123", err, sizeof(err)) !=
                0);
    ASSERT_TRUE(hyp_config_validate(HYP_CONFIG_ASK_ESC_KEY_ENV, "jina_abc123", err, sizeof(err)) !=
                0);
    ASSERT_TRUE(hyp_config_validate(HYP_CONFIG_ASK_ESC_KEY_ENV, "sk-abc123", err, sizeof(err)) !=
                0);
    /* THE ERROR MUST NOT ECHO THE VALUE — a refusal that prints the secret
     * into a terminal scrollback and then a bug report has leaked it anyway. */
    ASSERT_TRUE(strstr(err, "sk-abc123") == NULL);

    /* Anything with punctuation an environment variable name cannot hold. */
    ASSERT_TRUE(hyp_config_validate(HYP_CONFIG_ASK_ESC_KEY_ENV, "my key", err, sizeof(err)) != 0);
    ASSERT_TRUE(hyp_config_validate(HYP_CONFIG_ASK_ESC_KEY_ENV, "", err, sizeof(err)) != 0);

    /* Real variable names pass. */
    ASSERT_EQ(hyp_config_validate(HYP_CONFIG_ASK_ESC_KEY_ENV, "VOYAGE_API_KEY", err, sizeof(err)),
              0);
    ASSERT_EQ(hyp_config_validate(HYP_CONFIG_ASK_ESC_KEY_ENV, "_private_key_var", err,
                                  sizeof(err)),
              0);
    PASS();
}

TEST(ask_provider_config_refuses_an_unknown_provider_and_names_the_known_ones) {
    char err[512];
    ASSERT_TRUE(hyp_config_validate(HYP_CONFIG_ASK_ESC_PROVIDER, "voyag", err, sizeof(err)) != 0);
    ASSERT_TRUE(strstr(err, "voyage") != NULL);
    ASSERT_TRUE(strstr(err, "jina") != NULL);
    ASSERT_EQ(hyp_config_validate(HYP_CONFIG_ASK_ESC_PROVIDER, "voyage", err, sizeof(err)), 0);

    /* A key this validator does not know is stored unchanged — it validates
     * what it knows rather than being an allow-list of every key. */
    ASSERT_EQ(hyp_config_validate("some.unrelated.key", "whatever", err, sizeof(err)), 0);
    PASS();
}

/* The mode decides WHAT LEAVES THE MACHINE on an escalated
 * question — the ~30-token question, or the whole corpus — so it is two words
 * and nothing else, and the refusal names both so a typo is one round trip. */
TEST(ask_provider_config_mode_is_query_or_index_and_names_both_on_refusal) {
    char err[512];
    ASSERT_EQ(hyp_config_validate(HYP_CONFIG_ASK_ESC_MODE, "query", err, sizeof(err)), 0);
    ASSERT_EQ(hyp_config_validate(HYP_CONFIG_ASK_ESC_MODE, "index", err, sizeof(err)), 0);
    /* The default is the measured-useful, code-stays-local mode. */
    ASSERT_STR_EQ(HYP_CONFIG_ASK_ESC_MODE_DEFAULT, "query");

    err[0] = '\0';
    ASSERT_TRUE(hyp_config_validate(HYP_CONFIG_ASK_ESC_MODE, "Query", err, sizeof(err)) != 0);
    ASSERT_TRUE(strstr(err, "'query'") != NULL);
    ASSERT_TRUE(strstr(err, "'index'") != NULL);
    ASSERT_TRUE(hyp_config_validate(HYP_CONFIG_ASK_ESC_MODE, "both", err, sizeof(err)) != 0);
    ASSERT_TRUE(hyp_config_validate(HYP_CONFIG_ASK_ESC_MODE, "", err, sizeof(err)) != 0);
    ASSERT_TRUE(hyp_config_validate(HYP_CONFIG_ASK_ESC_MODE, "auto", err, sizeof(err)) != 0);
    PASS();
}

/* ── Key custody ───────────────────────────────────────────────────
 *
 * The observation this exists for: an escalated `ask` answers for a client
 * whose environment has no key, because the daemon serving it read one out
 * of the shell that started it. Same uid and local-only, so not a privilege
 * escalation — but a spending surface nobody declared, in a lane whose entire
 * design is that money never moves by surprise.
 *
 * THE VALUE PLANTED IN THE ENVIRONMENT HERE IS THE POINT OF THE TEST: every
 * assertion below is followed by one that the refusal did not echo it. A
 * refusal is the other place a secret leaks, and this one is designed to be
 * read out loud in a bug report.
 */
#define CUSTODY_TEST_KEY_VAR "HYP_TEST_CUSTODY_KEY_VAR"
#define CUSTODY_TEST_KEY_VALUE "not-a-real-key-custody-placeholder"

TEST(ask_provider_a_shared_server_refuses_to_read_the_key_until_the_owner_allows_it) {
    char err[HYP_SZ_1K];
    hyp_setenv(CUSTODY_TEST_KEY_VAR, CUSTODY_TEST_KEY_VALUE, 1);

    /* CALLER custody is the default, and there is nothing to gate: this
     * process IS the asking one. The setting is irrelevant in both positions. */
    hyp_ask_provider_clear_key_custody_for_test();
    ASSERT_EQ(hyp_ask_provider_key_custody(), HYP_ASK_KEY_CUSTODY_CALLER);
    ASSERT_STR_EQ(hyp_ask_provider_key_holder(), "");
    err[0] = '\0';
    ASSERT_FALSE(hyp_ask_provider_key_custody_refused(false, CUSTODY_TEST_KEY_VAR, err,
                                                      sizeof(err)));
    ASSERT_FALSE(hyp_ask_provider_key_custody_refused(true, CUSTODY_TEST_KEY_VAR, err,
                                                      sizeof(err)));

    /* SHARED custody, not allowed: refused, and the refusal is the whole
     * story — who is holding the key, which setting decides, and what that
     * setting does and does not cover (`ask` needs it, `embed` never did). */
    hyp_ask_provider_declare_shared_key_custody("the hyponoia daemon (pid 4242)");
    ASSERT_EQ(hyp_ask_provider_key_custody(), HYP_ASK_KEY_CUSTODY_SHARED);
    err[0] = '\0';
    ASSERT_TRUE(
        hyp_ask_provider_key_custody_refused(false, CUSTODY_TEST_KEY_VAR, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "the hyponoia daemon (pid 4242)"));
    ASSERT_NOT_NULL(strstr(err, CUSTODY_TEST_KEY_VAR));
    ASSERT_NOT_NULL(strstr(err, HYP_CONFIG_ASK_ESC_DAEMON_KEY));
    ASSERT_NOT_NULL(strstr(err, "allow"));
    ASSERT_NOT_NULL(strstr(err, "embed --escalation"));
    ASSERT_NULL(strstr(err, CUSTODY_TEST_KEY_VALUE));

    /* SHARED custody, allowed: the gate opens. */
    err[0] = '\0';
    ASSERT_FALSE(hyp_ask_provider_key_custody_refused(true, CUSTODY_TEST_KEY_VAR, err,
                                                      sizeof(err)));
    ASSERT_STR_EQ(err, "");

    hyp_ask_provider_clear_key_custody_for_test();
    hyp_unsetenv(CUSTODY_TEST_KEY_VAR);
    PASS();
}

/* THE GATE IS INSIDE encoder_create, NOT ONLY IN FRONT OF IT. Both callers ask
 * the custody question first so they can give their own remedy, but a third
 * one written later will not — and this is the assertion that keeps that from
 * costing money. It also pins the ORDER: custody is decided before the
 * provider lookup, so a shared server that may not spend refuses for that
 * reason rather than leaking which parts of the lane are misconfigured. */
TEST(ask_provider_encoder_create_enforces_custody_before_anything_else) {
    char err[HYP_SZ_1K];
    hyp_setenv(CUSTODY_TEST_KEY_VAR, CUSTODY_TEST_KEY_VALUE, 1);
    hyp_ask_provider_declare_shared_key_custody("the hyponoia daemon (pid 4242)");

    err[0] = '\0';
    ASSERT_NULL(hyp_ask_provider_encoder_create("voyage", "voyage-4-large", CUSTODY_TEST_KEY_VAR,
                                                false, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, HYP_CONFIG_ASK_ESC_DAEMON_KEY));
    ASSERT_NULL(strstr(err, CUSTODY_TEST_KEY_VALUE));

    /* Unknown provider AND refused custody: custody answers, because it is the
     * one that decides whether the key may be touched at all. */
    err[0] = '\0';
    ASSERT_NULL(hyp_ask_provider_encoder_create("no-such-provider", "m", CUSTODY_TEST_KEY_VAR,
                                                false, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, HYP_CONFIG_ASK_ESC_DAEMON_KEY));
    ASSERT_NULL(strstr(err, "not one this build knows"));

    /* Allowed, and the ordinary refusals are back in charge. */
    err[0] = '\0';
    ASSERT_NULL(hyp_ask_provider_encoder_create("no-such-provider", "m", CUSTODY_TEST_KEY_VAR, true,
                                                err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "not one this build knows"));
    ASSERT_NULL(strstr(err, CUSTODY_TEST_KEY_VALUE));

    /* Allowed and correct: an encoder, built from a key this process holds. */
    err[0] = '\0';
    hyp_ask_encoder_t *enc = hyp_ask_provider_encoder_create(
        "voyage", "voyage-4-large", CUSTODY_TEST_KEY_VAR, true, err, sizeof(err));
    ASSERT_NOT_NULL(enc);
    ASSERT_STR_EQ(err, "");
    /* The encoder's own identity must not carry the key either — it is
     * stamped onto indexes and printed in disclosures. */
    ASSERT_STR_EQ(hyp_ask_encoder_model_id(enc), "voyage/voyage-4-large");
    ASSERT_NULL(strstr(hyp_ask_encoder_model_id(enc), CUSTODY_TEST_KEY_VALUE));
    hyp_ask_encoder_destroy(enc);

    hyp_ask_provider_clear_key_custody_for_test();
    hyp_unsetenv(CUSTODY_TEST_KEY_VAR);
    PASS();
}

/* An unset variable under shared custody is a genuinely confusing message —
 * the reader is looking at their own shell, where it IS set. Naming the holder
 * is what makes it actionable instead of maddening. */
TEST(ask_provider_unset_key_under_shared_custody_names_whose_environment_was_read) {
    char err[HYP_SZ_1K];
    hyp_unsetenv(CUSTODY_TEST_KEY_VAR);

    hyp_ask_provider_clear_key_custody_for_test();
    err[0] = '\0';
    ASSERT_NULL(hyp_ask_provider_key(CUSTODY_TEST_KEY_VAR, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, "this process"));

    hyp_ask_provider_declare_shared_key_custody("the hyponoia daemon (pid 4242)");
    err[0] = '\0';
    ASSERT_NULL(hyp_ask_provider_key(CUSTODY_TEST_KEY_VAR, err, sizeof(err)));
    ASSERT_NOT_NULL(strstr(err, CUSTODY_TEST_KEY_VAR));
    ASSERT_NOT_NULL(strstr(err, "the hyponoia daemon (pid 4242)"));

    hyp_ask_provider_clear_key_custody_for_test();
    PASS();
}

/* `config set` is where the decision is made, so it is where a typo has to
 * die. "allowed", "yes", "true" and "" all mean nothing here — resolving any
 * of them to a default would be a spending decision made by a misspelling. */
TEST(ask_provider_config_daemon_key_is_allow_or_refuse_and_defaults_to_refuse) {
    char err[HYP_SZ_1K];
    ASSERT_EQ(hyp_config_validate(HYP_CONFIG_ASK_ESC_DAEMON_KEY, "allow", err, sizeof(err)), 0);
    ASSERT_EQ(hyp_config_validate(HYP_CONFIG_ASK_ESC_DAEMON_KEY, "refuse", err, sizeof(err)), 0);
    ASSERT_STR_EQ(HYP_CONFIG_ASK_ESC_DAEMON_KEY_DEFAULT, "refuse");

    const char *nearly[] = {"allowed", "yes", "true", "deny", "", "Allow"};
    for (size_t i = 0; i < sizeof(nearly) / sizeof(nearly[0]); i++) {
        err[0] = '\0';
        ASSERT_TRUE(hyp_config_validate(HYP_CONFIG_ASK_ESC_DAEMON_KEY, nearly[i], err,
                                        sizeof(err)) != 0);
        ASSERT_NOT_NULL(strstr(err, "'refuse'"));
        ASSERT_NOT_NULL(strstr(err, "'allow'"));
    }
    PASS();
}

SUITE(ask_provider) {
    RUN_TEST(ask_provider_config_mode_is_query_or_index_and_names_both_on_refusal);
    RUN_TEST(ask_provider_a_shared_server_refuses_to_read_the_key_until_the_owner_allows_it);
    RUN_TEST(ask_provider_encoder_create_enforces_custody_before_anything_else);
    RUN_TEST(ask_provider_unset_key_under_shared_custody_names_whose_environment_was_read);
    RUN_TEST(ask_provider_config_daemon_key_is_allow_or_refuse_and_defaults_to_refuse);
    RUN_TEST(ask_provider_table_rows_are_complete_and_asymmetric);
    RUN_TEST(ask_provider_voyage_dimension_parameter_truncates_it_does_not_reembed);
    RUN_TEST(ask_provider_lookup_refuses_unknown_names);
    RUN_TEST(ask_provider_contract_names_both_sides_of_the_asymmetry);
    RUN_TEST(ask_provider_key_is_read_from_the_environment_and_never_echoed);
    RUN_TEST(ask_provider_declared_but_unwired_refuses_instead_of_guessing);
    RUN_TEST(ask_provider_config_refuses_a_key_pasted_where_a_variable_name_goes);
    RUN_TEST(ask_provider_config_refuses_an_unknown_provider_and_names_the_known_ones);
}
