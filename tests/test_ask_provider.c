/*
 * test_ask_provider.c — the provider table's invariants (NEXT-STEPS.md §2.10 step 2).
 *
 * No network. What is worth pinning here is not that HTTP works, it is that the
 * table cannot quietly acquire a row that lies: a provider whose asymmetry
 * values are missing or identical would send the same request for documents and
 * queries and return plausible vectors from the wrong leg. That failure has no
 * downstream signal, which is why it is a test.
 */
#include "ask/ask_provider.h"

#include <string.h>

#include "foundation/compat.h"
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
         * exists for — §2.6 measured Jina's adapter worth +31% RR over sending
         * no task at all. */
        ASSERT_TRUE(strcmp(t[i].asym_document, t[i].asym_query) != 0);
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

SUITE(ask_provider) {
    RUN_TEST(ask_provider_table_rows_are_complete_and_asymmetric);
    RUN_TEST(ask_provider_lookup_refuses_unknown_names);
    RUN_TEST(ask_provider_contract_names_both_sides_of_the_asymmetry);
    RUN_TEST(ask_provider_key_is_read_from_the_environment_and_never_echoed);
    RUN_TEST(ask_provider_declared_but_unwired_refuses_instead_of_guessing);
}
