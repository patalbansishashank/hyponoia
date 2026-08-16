/*
 * test_scrub.c — the secret scrub (D2), and the seam D1 must ingest through.
 *
 * Two disciplines run through this file:
 *
 *   - NO KEY-SHAPED LITERAL ANYWHERE. Every fake key is constructed at
 *     runtime from the shared prefix table plus an obviously-dummy suffix, so
 *     no source line of this file can ever match the pre-push history
 *     scanner's own rule (vendor prefix + >= HYP_SCRUB_MIN_BODY body bytes).
 *     A test for "does this look like a key" never needs a real one — or even
 *     a whole fake one in the source.
 *
 *   - THE TABLE DRIVES THE TESTS. The per-prefix test iterates
 *     hyp_vendor_prefixes rather than a list of its own, so a prefix added to
 *     the one table is automatically under test, and a prefix the scrub skips
 *     fails HERE, BY NAME. (That is the negative control for this unit: skip
 *     one table entry in scrub.c and this suite names the survivor.)
 */
#include "test_framework.h"

#include "../src/foundation/record.h"
#include "../src/foundation/scrub.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 2026-02-14T12:26:40.000Z — fixed and caller-supplied, as C2 requires. */
#define FIXED_MS INT64_C(1770985600000)

/* 23 key-body bytes, comfortably over HYP_SCRUB_MIN_BODY, unmistakably fake.
 * Carries no vendor prefix of its own, so the literal is scanner-inert. */
#define DUMMY_BODY "FAKEFAKEFAKEFAKEFAKE123"

/* prefix + DUMMY_BODY, built at runtime. Returns false on truncation. */
static bool make_fake_key(const char *prefix, char *out, size_t cap) {
    int n = snprintf(out, cap, "%s%s", prefix, DUMMY_BODY);
    return n > 0 && (size_t)n < cap;
}

/* ── The scrub itself ───────────────────────────────────────────────────── */

TEST(scrub_scrubs_every_vendor_prefix) {
    /* Derived from the table, never enumerated here: every prefix the config
     * guard and the push scanner know is a prefix this scrub must catch. */
    ASSERT_GTE((long long)hyp_vendor_prefix_count, 1);
    for (size_t i = 0; i < hyp_vendor_prefix_count; i++) {
        const char *pfx = hyp_vendor_prefixes[i];
        ASSERT_NOT_NULL(pfx);
        ASSERT_TRUE(pfx[0] != '\0');

        char key[128];
        ASSERT_TRUE(make_fake_key(pfx, key, sizeof(key)));
        char text[256];
        snprintf(text, sizeof(text), "the agent pasted %s into the transcript", key);

        char *clean = NULL;
        uint32_t n = 0;
        ASSERT_TRUE(hyp_scrub_text(text, &clean, &n));

        char marker[64];
        snprintf(marker, sizeof(marker), "[REDACTED:%s]", pfx);
        char why[256];
        if (n != 1 || strstr(clean, DUMMY_BODY) != NULL || strstr(clean, marker) == NULL) {
            snprintf(why, sizeof(why),
                     "prefix \"%s\" survived the scrub (redactions=%u, marker %s)", pfx,
                     (unsigned)n, strstr(clean, marker) ? "present" : "missing");
            free(clean);
            FAIL(why);
        }
        /* The prose around the key is untouched. */
        ASSERT_NOT_NULL(strstr(clean, "the agent pasted "));
        ASSERT_NOT_NULL(strstr(clean, " into the transcript"));
        free(clean);
    }
    PASS();
}

TEST(scrub_is_deterministic_and_idempotent) {
    char key[128], text[256];
    ASSERT_TRUE(make_fake_key(hyp_vendor_prefixes[0], key, sizeof(key)));
    snprintf(text, sizeof(text), "before %s after", key);

    /* Two independent calls, byte-identical output — this is what makes two
     * machines derive one record id from one transcript. */
    char *a = NULL, *b = NULL;
    uint32_t na = 0, nb = 0;
    ASSERT_TRUE(hyp_scrub_text(text, &a, &na));
    ASSERT_TRUE(hyp_scrub_text(text, &b, &nb));
    ASSERT_EQ(na, nb);
    ASSERT_STR_EQ(a, b);

    /* Scrubbing scrubbed text changes nothing: the marker is not a hit. */
    char *again = NULL;
    uint32_t n_again = 99;
    ASSERT_TRUE(hyp_scrub_text(a, &again, &n_again));
    ASSERT_EQ(n_again, 0);
    ASSERT_STR_EQ(again, a);

    free(a);
    free(b);
    free(again);
    PASS();
}

TEST(scrub_leaves_clean_text_verbatim) {
    /* The NAME of a key variable, a below-threshold fixture, and a model name
     * that starts with a vendor prefix are all legitimate transcript text. */
    const char *clean_inputs[] = {
        "set VOYAGE_API_KEY in your environment; config stores only the NAME",
        "the test fixture is pa-abc123 and it is nobody's key",
        "we embed with jina_embeddings-v3 on the local lane",
        "",
    };
    for (size_t i = 0; i < sizeof(clean_inputs) / sizeof(clean_inputs[0]); i++) {
        char *out = NULL;
        uint32_t n = 99;
        ASSERT_TRUE(hyp_scrub_text(clean_inputs[i], &out, &n));
        ASSERT_EQ(n, 0);
        ASSERT_STR_EQ(out, clean_inputs[i]);
        free(out);
    }
    PASS();
}

TEST(scrub_threshold_is_the_scanners) {
    /* HYP_SCRUB_MIN_BODY - 1 body bytes is a word; MIN_BODY is a key. The
     * boundary is asserted from the shared define, not a copied number. */
    char body_short[HYP_SCRUB_MIN_BODY];     /* MIN_BODY - 1 chars + NUL */
    char body_exact[HYP_SCRUB_MIN_BODY + 1]; /* MIN_BODY chars + NUL */
    memset(body_short, 'x', sizeof(body_short) - 1);
    body_short[sizeof(body_short) - 1] = '\0';
    memset(body_exact, 'x', sizeof(body_exact) - 1);
    body_exact[sizeof(body_exact) - 1] = '\0';

    char text[256];
    char *out = NULL;
    uint32_t n = 99;

    snprintf(text, sizeof(text), "%s%s", hyp_vendor_prefixes[0], body_short);
    ASSERT_TRUE(hyp_scrub_text(text, &out, &n));
    ASSERT_EQ(n, 0);
    ASSERT_STR_EQ(out, text);
    free(out);

    snprintf(text, sizeof(text), "%s%s", hyp_vendor_prefixes[0], body_exact);
    ASSERT_TRUE(hyp_scrub_text(text, &out, &n));
    ASSERT_EQ(n, 1);
    ASSERT_NULL(strstr(out, body_exact));
    free(out);
    PASS();
}

TEST(scrub_respects_word_boundaries) {
    /* A prefix inside a word is a word. "compa-..." must not become
     * "com[REDACTED:pa-]", which would corrupt ordinary prose. */
    char tail[64], text[256];
    memset(tail, 'z', 24);
    tail[24] = '\0';

    snprintf(text, sizeof(text), "compa-%s stays", tail);
    char *out = NULL;
    uint32_t n = 99;
    ASSERT_TRUE(hyp_scrub_text(text, &out, &n));
    ASSERT_EQ(n, 0);
    ASSERT_STR_EQ(out, text);
    free(out);

    /* The positive control: the same prefix after a NON-word byte is a hit —
     * punctuation is exactly how a pasted key arrives, "(sk-...)". */
    char key[128];
    ASSERT_TRUE(make_fake_key("pa-", key, sizeof(key)));
    snprintf(text, sizeof(text), "(%s)", key);
    ASSERT_TRUE(hyp_scrub_text(text, &out, &n));
    ASSERT_EQ(n, 1);
    ASSERT_STR_EQ(out, "([REDACTED:pa-])");
    free(out);
    PASS();
}

TEST(scrub_counts_every_span) {
    char key_a[128], key_b[128], text[512];
    ASSERT_TRUE(make_fake_key(hyp_vendor_prefixes[0], key_a, sizeof(key_a)));
    ASSERT_TRUE(make_fake_key(hyp_vendor_prefixes[hyp_vendor_prefix_count - 1], key_b,
                              sizeof(key_b)));

    /* One at the very start, one at the very end — both edges of the scan. */
    snprintf(text, sizeof(text), "%s then some words then %s", key_a, key_b);
    char *out = NULL;
    uint32_t n = 0;
    ASSERT_TRUE(hyp_scrub_text(text, &out, &n));
    ASSERT_EQ(n, 2);
    ASSERT_NULL(strstr(out, DUMMY_BODY));
    ASSERT_NOT_NULL(strstr(out, " then some words then "));
    free(out);
    PASS();
}

TEST(scrub_refuses_null_and_touches_nothing) {
    char *out = (char *)"sentinel";
    uint32_t n = 99;
    ASSERT_FALSE(hyp_scrub_text(NULL, &out, &n));
    ASSERT_FALSE(hyp_scrub_text("x", NULL, &n));
    ASSERT_FALSE(hyp_scrub_text("x", &out, NULL));
    /* Outputs untouched on refusal. */
    ASSERT_STR_EQ(out, "sentinel");
    ASSERT_EQ(n, 99);
    PASS();
}

/* ── The ingest seam ────────────────────────────────────────────────────── */

static hyp_record_ingest_input_t base_ingest(void) {
    hyp_record_ingest_input_t in;
    memset(&in, 0, sizeof(in));
    in.kind = HYP_RECORD_MESSAGE;
    in.author = "agent:d2";
    in.timestamp_ms = FIXED_MS;
    in.content = "one turn of a transcript";
    in.anchor = NULL;
    in.origin = "feed:row-7";
    in.thread = "conversation-1";
    in.parent = NULL;
    return in;
}

/* The record.h input that the same fields WOULD produce, for derive_id
 * comparisons. `content` and `redactions` are the caller's to vary. */
static hyp_record_input_t as_record_input(const hyp_record_ingest_input_t *in,
                                          const char *content, uint32_t redactions) {
    hyp_record_input_t rin;
    memset(&rin, 0, sizeof(rin));
    rin.kind = in->kind;
    rin.author = in->author;
    rin.timestamp_ms = in->timestamp_ms;
    rin.content = content;
    rin.anchor = in->anchor;
    rin.origin = in->origin;
    rin.thread = in->thread;
    rin.parent = in->parent;
    rin.redactions = redactions;
    return rin;
}

TEST(ingest_without_a_scrubber_is_refused) {
    /* THE assertion of the "D2 before D1" row, at this end: no scrubber, no
     * record — not "scrub available", but "unscrubbed ingest is impossible".
     * D1's transcript store must make the SAME assertion at its own boundary,
     * through whatever surface it exposes. */
    hyp_record_ingest_input_t in = base_ingest();
    const hyp_record_t *rec = (const hyp_record_t *)0x1;
    ASSERT_EQ(hyp_record_ingest_scrubbed(&in, NULL, &rec), HYP_RECORD_ERR_NULL);
    ASSERT_NULL(rec);

    ASSERT_EQ(hyp_record_ingest_scrubbed(NULL, hyp_scrub_text, &rec), HYP_RECORD_ERR_NULL);
    ASSERT_EQ(hyp_record_ingest_scrubbed(&in, hyp_scrub_text, NULL), HYP_RECORD_ERR_NULL);
    PASS();
}

TEST(ingest_scrub_is_in_the_id) {
    /* The ordering guarantee, as a permanent test: the id is derived from the
     * SCRUBBED text, so the id an unscrubbed build would have produced is a
     * DIFFERENT id — which is exactly why a scrub after the build could never
     * repair a record, only mint a second one beside the leak. */
    char key[128], raw[256];
    ASSERT_TRUE(make_fake_key(hyp_vendor_prefixes[0], key, sizeof(key)));
    snprintf(raw, sizeof(raw), "here is my key %s please embed", key);

    hyp_record_ingest_input_t in = base_ingest();
    in.content = raw;

    const hyp_record_t *rec = NULL;
    ASSERT_EQ(hyp_record_ingest_scrubbed(&in, hyp_scrub_text, &rec), HYP_RECORD_OK);
    ASSERT_NOT_NULL(rec);
    ASSERT_EQ(rec->redactions, 1);
    ASSERT_NULL(strstr(rec->content, DUMMY_BODY));
    ASSERT_NOT_NULL(strstr(rec->content, "[REDACTED:"));

    /* What an unscrubbed construction would have produced. */
    char unscrubbed_id[HYP_RECORD_ID_LEN + 1];
    hyp_record_input_t rin = as_record_input(&in, raw, 0);
    ASSERT_EQ(hyp_record_derive_id(&rin, unscrubbed_id), HYP_RECORD_OK);
    ASSERT_TRUE(strcmp(rec->id, unscrubbed_id) != 0);

    /* Even claiming the count without doing the scrub is a different id: the
     * text itself is in the preimage, not just the number. */
    rin = as_record_input(&in, raw, 1);
    ASSERT_EQ(hyp_record_derive_id(&rin, unscrubbed_id), HYP_RECORD_OK);
    ASSERT_TRUE(strcmp(rec->id, unscrubbed_id) != 0);

    /* And the seam is EXACTLY scrub-then-build, nothing more: deriving over
     * the scrubbed text with the scrubber's count reproduces the id. */
    char scrubbed_id[HYP_RECORD_ID_LEN + 1];
    rin = as_record_input(&in, rec->content, rec->redactions);
    ASSERT_EQ(hyp_record_derive_id(&rin, scrubbed_id), HYP_RECORD_OK);
    ASSERT_STR_EQ(rec->id, scrubbed_id);

    hyp_record_free(rec);
    PASS();
}

TEST(ingest_clean_text_is_verbatim_with_zero_redactions) {
    /* Absent secrets mean an untouched record: same content, redactions 0,
     * and the id a direct hyp_record_build would have produced — the seam
     * adds nothing when there is nothing to remove. */
    hyp_record_ingest_input_t in = base_ingest();
    const hyp_record_t *rec = NULL;
    ASSERT_EQ(hyp_record_ingest_scrubbed(&in, hyp_scrub_text, &rec), HYP_RECORD_OK);
    ASSERT_NOT_NULL(rec);
    ASSERT_EQ(rec->redactions, 0);
    ASSERT_STR_EQ(rec->content, in.content);

    char direct_id[HYP_RECORD_ID_LEN + 1];
    hyp_record_input_t rin = as_record_input(&in, in.content, 0);
    ASSERT_EQ(hyp_record_derive_id(&rin, direct_id), HYP_RECORD_OK);
    ASSERT_STR_EQ(rec->id, direct_id);

    hyp_record_free(rec);
    PASS();
}

TEST(ingest_propagates_build_refusals) {
    /* The seam adds no leniency: everything hyp_record_build refuses is
     * refused through the seam with the same name. */
    const hyp_record_t *rec = NULL;

    hyp_record_ingest_input_t in = base_ingest();
    in.author = NULL;
    ASSERT_EQ(hyp_record_ingest_scrubbed(&in, hyp_scrub_text, &rec), HYP_RECORD_ERR_AUTHOR);
    ASSERT_NULL(rec);

    in = base_ingest();
    in.content = "";
    ASSERT_EQ(hyp_record_ingest_scrubbed(&in, hyp_scrub_text, &rec), HYP_RECORD_ERR_CONTENT);
    ASSERT_NULL(rec);

    in = base_ingest();
    in.content = NULL;
    ASSERT_EQ(hyp_record_ingest_scrubbed(&in, hyp_scrub_text, &rec), HYP_RECORD_ERR_CONTENT);
    ASSERT_NULL(rec);

    in = base_ingest();
    in.timestamp_ms = 0;
    ASSERT_EQ(hyp_record_ingest_scrubbed(&in, hyp_scrub_text, &rec), HYP_RECORD_ERR_TIMESTAMP);
    ASSERT_NULL(rec);
    PASS();
}

/* An injected scrubber, to prove the seam runs the one it is GIVEN — which is
 * what makes wiring (and the refusal when nothing is wired) real. */
static bool stub_scrub(const char *text, char **out_text, uint32_t *out_redactions) {
    (void)text;
    char *copy = malloc(6);
    if (!copy) {
        return false;
    }
    memcpy(copy, "CLEAN", 6);
    *out_text = copy;
    *out_redactions = 7;
    return true;
}

static bool failing_scrub(const char *text, char **out_text, uint32_t *out_redactions) {
    (void)text;
    (void)out_text;
    (void)out_redactions;
    return false;
}

TEST(ingest_uses_the_scrubber_it_is_given_and_fails_closed) {
    hyp_record_ingest_input_t in = base_ingest();
    const hyp_record_t *rec = NULL;
    ASSERT_EQ(hyp_record_ingest_scrubbed(&in, stub_scrub, &rec), HYP_RECORD_OK);
    ASSERT_NOT_NULL(rec);
    ASSERT_STR_EQ(rec->content, "CLEAN");
    ASSERT_EQ(rec->redactions, 7);
    hyp_record_free(rec);

    /* A scrubber that cannot scrub builds NOTHING. A record that slips past
     * the scrub is permanent, so this path must fail closed. */
    rec = (const hyp_record_t *)0x1;
    ASSERT_EQ(hyp_record_ingest_scrubbed(&in, failing_scrub, &rec), HYP_RECORD_ERR_ALLOC);
    ASSERT_NULL(rec);
    PASS();
}

/* ── Suite ──────────────────────────────────────────────────────────────── */

SUITE(scrub) {
    /* The scrub */
    RUN_TEST(scrub_scrubs_every_vendor_prefix);
    RUN_TEST(scrub_is_deterministic_and_idempotent);
    RUN_TEST(scrub_leaves_clean_text_verbatim);
    RUN_TEST(scrub_threshold_is_the_scanners);
    RUN_TEST(scrub_respects_word_boundaries);
    RUN_TEST(scrub_counts_every_span);
    RUN_TEST(scrub_refuses_null_and_touches_nothing);
    /* The seam */
    RUN_TEST(ingest_without_a_scrubber_is_refused);
    RUN_TEST(ingest_scrub_is_in_the_id);
    RUN_TEST(ingest_clean_text_is_verbatim_with_zero_redactions);
    RUN_TEST(ingest_propagates_build_refusals);
    RUN_TEST(ingest_uses_the_scrubber_it_is_given_and_fails_closed);
}
