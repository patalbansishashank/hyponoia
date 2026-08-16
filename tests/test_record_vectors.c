/*
 * test_record_vectors.c — vectors for record content, keyed on the content
 * hash alone (§4 F1).
 *
 * The properties under test are the ones the unit exists for: two records
 * with byte-identical content and different ids produce exactly ONE vector
 * row (asserted as a ROW COUNT, not as the absence of an error); a shipment
 * carries no field that could hold an id, an author, an origin, a thread or
 * an anchor, and the receiver attributes by recomputing the content-hash
 * join; a vector matching no local record is HELD visibly, never dropped and
 * never attached; and a stamp mismatch refuses the whole shipment loudly.
 */

#include "test_framework.h"
#include "test_helpers.h"

#include "ask/record_vectors.h"
#include "foundation/identity.h"
#include "foundation/record.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RV_TEST_DIM 8

/* 2026-02-14T12:26:40.000Z — fixed, caller-supplied. No clock in this suite. */
#define RV_FIXED_MS INT64_C(1770985600000)

static void unit_axis(float *v, int dim, int axis) {
    for (int i = 0; i < dim; i++) {
        v[i] = 0.0F;
    }
    v[axis % dim] = 1.0F;
}

/* A record whose every identity-bearing field is caller-chosen, so two
 * builds can share content while agreeing on nothing else. */
static const hyp_record_t *make_record(const char *content, const char *author, const char *origin,
                                       const char *thread, int64_t timestamp_ms) {
    hyp_record_input_t in;
    memset(&in, 0, sizeof(in));
    in.kind = HYP_RECORD_SIGNAL;
    in.author = author;
    in.timestamp_ms = timestamp_ms;
    in.content = content;
    in.anchor = NULL;
    in.origin = origin;
    in.thread = thread;
    in.parent = NULL;
    in.redactions = 0;
    const hyp_record_t *rec = NULL;
    if (hyp_record_build(&in, &rec) != HYP_RECORD_OK) {
        return NULL;
    }
    return rec;
}

/* Byte search — the export file is binary, so strstr cannot be trusted with
 * it. */
static bool bytes_contain(const unsigned char *hay, size_t hay_len, const char *needle) {
    size_t n = strlen(needle);
    if (n == 0 || n > hay_len) {
        return false;
    }
    for (size_t i = 0; i + n <= hay_len; i++) {
        if (memcmp(hay + i, needle, n) == 0) {
            return true;
        }
    }
    return false;
}

static unsigned char *read_whole_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) {
        fclose(f);
        return NULL;
    }
    unsigned char *buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (got != (size_t)len) {
        free(buf);
        return NULL;
    }
    *out_len = got;
    return buf;
}

TEST(record_vectors_unstamped_store_refuses_rather_than_guesses) {
    char *dir = th_mktempdir("hyp-recvec");
    ASSERT_NOT_NULL(dir);
    hyp_record_vectors_t *v = hyp_record_vectors_open_path(TH_PATH(dir, "m.db"));
    ASSERT_NOT_NULL(v);

    /* "No stamp yet" and "a stamped store with no rows" are different answers. */
    hyp_record_vec_meta_t m;
    ASSERT_EQ(hyp_record_vectors_get_meta(v, &m), HYP_ASK_VEC_NOT_FOUND);
    ASSERT_EQ(hyp_record_vectors_count(v), 0);

    const hyp_record_t *rec = make_record("text", "agent:a", NULL, NULL, RV_FIXED_MS);
    ASSERT_NOT_NULL(rec);
    float vec[RV_TEST_DIM];
    unit_axis(vec, RV_TEST_DIM, 0);

    /* Writing before the space is recorded is refused, not defaulted. */
    ASSERT_EQ(hyp_record_vectors_put(v, rec, vec), HYP_ASK_VEC_ERR);
    /* So is shipping out of an unstamped store... */
    int64_t exported = -1;
    ASSERT_EQ(hyp_record_vectors_export(v, TH_PATH(dir, "out.rvec"), &exported), HYP_ASK_VEC_ERR);
    /* ...and receiving into one: "could not verify" refuses, it does not
     * adopt the shipper's word for the space. */
    ASSERT_EQ(th_write_file(TH_PATH(dir, "some.rvec"), "hyprvec1garbage"), 0);
    hyp_record_vec_import_report_t rep;
    ASSERT_EQ(hyp_record_vectors_import(v, TH_PATH(dir, "some.rvec"), NULL, 0, &rep),
              HYP_ASK_VEC_INCOMPATIBLE);
    ASSERT(strstr(hyp_record_vectors_error(v), "no encoder stamp") != NULL);

    hyp_record_free(rec);
    hyp_record_vectors_close(v);
    th_cleanup(dir);
    PASS();
}

/* THE F1 ASSERTION ROW: two records, byte-identical content, different ids →
 * exactly ONE vector row. The row COUNT is asserted, not the absence of an
 * error — a store that stored two rows without complaining would pass the
 * weaker test. */
TEST(record_vectors_identical_content_different_ids_is_one_row) {
    char *dir = th_mktempdir("hyp-recvec");
    ASSERT_NOT_NULL(dir);
    hyp_record_vectors_t *v = hyp_record_vectors_open_path(TH_PATH(dir, "m.db"));
    ASSERT_NOT_NULL(v);
    ASSERT_EQ(hyp_record_vectors_begin(v, "m1", "c1", RV_TEST_DIM, 32768, false), HYP_ASK_VEC_OK);

    /* Same text, two sessions: different author, origin, thread AND time. */
    static const char *const SHARED = "the watcher fires the memory pull in the same pass";
    const hyp_record_t *a =
        make_record(SHARED, "agent:one", "sess-1:row-9", "thread-1", RV_FIXED_MS);
    const hyp_record_t *b =
        make_record(SHARED, "agent:two", "sess-2:row-4", "thread-2", RV_FIXED_MS + 86400000);
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);
    /* The premise, proven rather than assumed: the ids DIFFER. */
    ASSERT(strcmp(a->id, b->id) != 0);

    /* N records sharing content → ONE embed job. */
    const hyp_record_t *recs[2] = {a, b};
    int jobs[2] = {-1, -1};
    int job_count = -1;
    ASSERT_EQ(hyp_record_vectors_plan(v, recs, 2, jobs, &job_count), HYP_ASK_VEC_OK);
    ASSERT_EQ(job_count, 1);
    ASSERT_EQ(jobs[0], 0);

    /* And ONE row, even when both records are put explicitly. */
    float vec[RV_TEST_DIM];
    unit_axis(vec, RV_TEST_DIM, 2);
    ASSERT_EQ(hyp_record_vectors_put(v, a, vec), HYP_ASK_VEC_OK);
    ASSERT_EQ(hyp_record_vectors_put(v, b, vec), HYP_ASK_VEC_OK);
    ASSERT_EQ(hyp_record_vectors_count(v), 1);

    /* Both records resolve to that one row — the join answers for either. */
    float back[RV_TEST_DIM];
    ASSERT_EQ(hyp_record_vectors_get(v, a, back, RV_TEST_DIM), HYP_ASK_VEC_OK);
    ASSERT_EQ(hyp_record_vectors_get(v, b, back, RV_TEST_DIM), HYP_ASK_VEC_OK);
    ASSERT_MEM_EQ(back, vec, sizeof(vec));

    /* Nothing left to embed. */
    ASSERT_EQ(hyp_record_vectors_plan(v, recs, 2, jobs, &job_count), HYP_ASK_VEC_OK);
    ASSERT_EQ(job_count, 0);

    /* Control inside the test: DIFFERENT content is a second row. */
    const hyp_record_t *c =
        make_record("different text entirely", "agent:one", NULL, NULL, RV_FIXED_MS);
    ASSERT_NOT_NULL(c);
    float vec2[RV_TEST_DIM];
    unit_axis(vec2, RV_TEST_DIM, 3);
    ASSERT_EQ(hyp_record_vectors_put(v, c, vec2), HYP_ASK_VEC_OK);
    ASSERT_EQ(hyp_record_vectors_count(v), 2);

    hyp_record_free(a);
    hyp_record_free(b);
    hyp_record_free(c);
    hyp_record_vectors_close(v);
    th_cleanup(dir);
    PASS();
}

/* THE OTHER F1 ASSERTION ROW: the ship/receive round trip attributes via the
 * content-hash join, and a vector matching no local record is held VISIBLY —
 * enumerable and counted, not dropped, not attached. */
TEST(record_vectors_ship_roundtrip_joins_and_holds) {
    char *dir = th_mktempdir("hyp-recvec");
    ASSERT_NOT_NULL(dir);
    char ship[512];
    snprintf(ship, sizeof(ship), "%s", TH_PATH(dir, "ship.rvec"));

    const hyp_record_t *r1 =
        make_record("first shared text", "agent:sender", "s1", NULL, RV_FIXED_MS);
    const hyp_record_t *r2 =
        make_record("second shared text", "agent:sender", "s2", NULL, RV_FIXED_MS);
    ASSERT_NOT_NULL(r1);
    ASSERT_NOT_NULL(r2);
    float v1[RV_TEST_DIM];
    float v2[RV_TEST_DIM];
    unit_axis(v1, RV_TEST_DIM, 1);
    unit_axis(v2, RV_TEST_DIM, 5);

    /* The sender embeds once, centrally, and ships. */
    hyp_record_vectors_t *sender = hyp_record_vectors_open_path(TH_PATH(dir, "a.db"));
    ASSERT_NOT_NULL(sender);
    ASSERT_EQ(hyp_record_vectors_begin(sender, "m1", "c1", RV_TEST_DIM, 32768, false),
              HYP_ASK_VEC_OK);
    ASSERT_EQ(hyp_record_vectors_put(sender, r1, v1), HYP_ASK_VEC_OK);
    ASSERT_EQ(hyp_record_vectors_put(sender, r2, v2), HYP_ASK_VEC_OK);
    int64_t exported = 0;
    ASSERT_EQ(hyp_record_vectors_export(sender, ship, &exported), HYP_ASK_VEC_OK);
    ASSERT_EQ(exported, 2);
    hyp_record_vectors_close(sender);

    /* The receiver holds r1's record but has never seen r2's. Its local view
     * is content hashes it computed ITSELF, through the one hash. */
    char h1[HYP_ADDR_SPAN_HASH_LEN + 1];
    char h2[HYP_ADDR_SPAN_HASH_LEN + 1];
    hyp_addr_span_hash(r1->content, h1);
    hyp_addr_span_hash(r2->content, h2);
    const char *local[1] = {h1};

    hyp_record_vectors_t *recv = hyp_record_vectors_open_path(TH_PATH(dir, "b.db"));
    ASSERT_NOT_NULL(recv);
    ASSERT_EQ(hyp_record_vectors_begin(recv, "m1", "c1", RV_TEST_DIM, 32768, false),
              HYP_ASK_VEC_OK);
    hyp_record_vec_import_report_t rep;
    ASSERT_EQ(hyp_record_vectors_import(recv, ship, local, 1, &rep), HYP_ASK_VEC_OK);
    ASSERT_EQ(rep.attached, 1);
    ASSERT_EQ(rep.held, 1);
    ASSERT_EQ(rep.already, 0);

    /* Attribution came back through the join: the receiver never received an
     * id, an author or an origin, yet r1's record finds its vector. */
    float back[RV_TEST_DIM];
    ASSERT_EQ(hyp_record_vectors_get(recv, r1, back, RV_TEST_DIM), HYP_ASK_VEC_OK);
    ASSERT_MEM_EQ(back, v1, sizeof(v1));

    /* The unmatched vector is HELD: not attached (get says not found)... */
    ASSERT_EQ(hyp_record_vectors_get(recv, r2, back, RV_TEST_DIM), HYP_ASK_VEC_NOT_FOUND);
    /* ...and VISIBLE — counted and named, not dropped. */
    ASSERT_EQ(hyp_record_vectors_pending_count(recv), 1);
    char **held = NULL;
    int held_count = 0;
    ASSERT_EQ(hyp_record_vectors_pending_hashes(recv, &held, &held_count), HYP_ASK_VEC_OK);
    ASSERT_EQ(held_count, 1);
    ASSERT_STR_EQ(held[0], h2);
    hyp_record_vectors_hashes_free(held, held_count);
    hyp_record_vec_meta_t meta;
    ASSERT_EQ(hyp_record_vectors_get_meta(recv, &meta), HYP_ASK_VEC_OK);
    ASSERT_EQ(meta.row_count, 1);
    ASSERT_EQ(meta.pending_count, 1);
    hyp_record_vec_meta_free(&meta);

    /* r2's record arrives later (partial sync resolving). The claim IS the
     * join, run against real content — and only then does held become
     * attached. */
    const hyp_record_t *arrivals[1] = {r2};
    int64_t claimed = 0;
    ASSERT_EQ(hyp_record_vectors_claim(recv, arrivals, 1, &claimed), HYP_ASK_VEC_OK);
    ASSERT_EQ(claimed, 1);
    ASSERT_EQ(hyp_record_vectors_pending_count(recv), 0);
    ASSERT_EQ(hyp_record_vectors_count(recv), 2);
    ASSERT_EQ(hyp_record_vectors_get(recv, r2, back, RV_TEST_DIM), HYP_ASK_VEC_OK);
    ASSERT_MEM_EQ(back, v2, sizeof(v2));

    /* Importing the same shipment again is the union absorbing duplicates. */
    ASSERT_EQ(hyp_record_vectors_import(recv, ship, local, 1, &rep), HYP_ASK_VEC_OK);
    ASSERT_EQ(rep.attached, 0);
    ASSERT_EQ(rep.held, 0);
    ASSERT_EQ(rep.already, 2);
    ASSERT_EQ(hyp_record_vectors_count(recv), 2);

    hyp_record_free(r1);
    hyp_record_free(r2);
    hyp_record_vectors_close(recv);
    th_cleanup(dir);
    PASS();
}

/* Address-free is a PROPERTY of the format, tested by construction: the file
 * is header + count x (hash, vector), to the byte — there is no field an id,
 * an author, an origin, a thread, an anchor or the content itself could
 * travel in, and none of their bytes appear. */
TEST(record_vectors_shipment_is_address_free_by_construction) {
    char *dir = th_mktempdir("hyp-recvec");
    ASSERT_NOT_NULL(dir);
    char ship[512];
    snprintf(ship, sizeof(ship), "%s", TH_PATH(dir, "ship.rvec"));

    /* Every identity-bearing field carries a distinctive marker. */
    const hyp_record_t *rec =
        make_record("CONTENTMARK the text itself must not ship either", "AUTHORMARK-agent",
                    "ORIGINMARK-row-7", "THREADMARK-conv", RV_FIXED_MS);
    ASSERT_NOT_NULL(rec);

    static const char MODEL[] = "m1";
    static const char CONTRACT[] = "c1";
    hyp_record_vectors_t *v = hyp_record_vectors_open_path(TH_PATH(dir, "a.db"));
    ASSERT_NOT_NULL(v);
    ASSERT_EQ(hyp_record_vectors_begin(v, MODEL, CONTRACT, RV_TEST_DIM, 32768, false),
              HYP_ASK_VEC_OK);
    float vec[RV_TEST_DIM];
    unit_axis(vec, RV_TEST_DIM, 4);
    ASSERT_EQ(hyp_record_vectors_put(v, rec, vec), HYP_ASK_VEC_OK);
    int64_t exported = 0;
    ASSERT_EQ(hyp_record_vectors_export(v, ship, &exported), HYP_ASK_VEC_OK);
    ASSERT_EQ(exported, 1);
    hyp_record_vectors_close(v);

    size_t len = 0;
    unsigned char *bytes = read_whole_file(ship, &len);
    ASSERT_NOT_NULL(bytes);

    /* The arithmetic: magic(8) + ship_format(4) + dim(4) + window(4)
     * + model(4+len) + contract(4+len) + count(8) + 1 x (hash 32 + dim x 4).
     * An exact size means NO ROOM for any field beyond these — absence of an
     * address is structural, not a matter of nobody having spotted one. */
    size_t expect = 8 + 4 + 4 + 4 + (4 + (sizeof(MODEL) - 1)) + (4 + (sizeof(CONTRACT) - 1)) + 8 +
                    (size_t)1 * (HYP_ADDR_SPAN_HASH_LEN + RV_TEST_DIM * sizeof(float));
    ASSERT_EQ(len, expect);

    /* And none of the identity bytes appear — not the id, not any field. */
    ASSERT_FALSE(bytes_contain(bytes, len, rec->id));
    ASSERT_FALSE(bytes_contain(bytes, len, "AUTHORMARK"));
    ASSERT_FALSE(bytes_contain(bytes, len, "ORIGINMARK"));
    ASSERT_FALSE(bytes_contain(bytes, len, "THREADMARK"));
    ASSERT_FALSE(bytes_contain(bytes, len, "CONTENTMARK"));
    /* What DOES appear is exactly the join key: the content hash. */
    char h[HYP_ADDR_SPAN_HASH_LEN + 1];
    hyp_addr_span_hash(rec->content, h);
    ASSERT_TRUE(bytes_contain(bytes, len, h));

    free(bytes);
    hyp_record_free(rec);
    th_cleanup(dir);
    PASS();
}

/* A machine on a different encoder REFUSES shipped vectors — model, contract,
 * dim and window each refuse on their own, nothing is imported, and the
 * error names both sides. The gate is hyp_ask_stamp_check, the same function
 * the code-span store refuses with. */
TEST(record_vectors_import_refuses_a_different_stamp) {
    char *dir = th_mktempdir("hyp-recvec");
    ASSERT_NOT_NULL(dir);
    char ship[512];
    snprintf(ship, sizeof(ship), "%s", TH_PATH(dir, "ship.rvec"));

    const hyp_record_t *rec = make_record("shared text", "agent:a", NULL, NULL, RV_FIXED_MS);
    ASSERT_NOT_NULL(rec);
    float vec[RV_TEST_DIM];
    unit_axis(vec, RV_TEST_DIM, 0);

    hyp_record_vectors_t *sender = hyp_record_vectors_open_path(TH_PATH(dir, "a.db"));
    ASSERT_NOT_NULL(sender);
    ASSERT_EQ(hyp_record_vectors_begin(sender, "m1", "c1", RV_TEST_DIM, 32768, false),
              HYP_ASK_VEC_OK);
    ASSERT_EQ(hyp_record_vectors_put(sender, rec, vec), HYP_ASK_VEC_OK);
    ASSERT_EQ(hyp_record_vectors_export(sender, ship, NULL), HYP_ASK_VEC_OK);
    hyp_record_vectors_close(sender);

    char h[HYP_ADDR_SPAN_HASH_LEN + 1];
    hyp_addr_span_hash(rec->content, h);
    const char *local[1] = {h};
    hyp_record_vec_import_report_t rep;

    /* A different MODEL refuses, naming both sides. */
    hyp_record_vectors_t *other = hyp_record_vectors_open_path(TH_PATH(dir, "b.db"));
    ASSERT_NOT_NULL(other);
    ASSERT_EQ(hyp_record_vectors_begin(other, "m2", "c1", RV_TEST_DIM, 32768, false),
              HYP_ASK_VEC_OK);
    ASSERT_EQ(hyp_record_vectors_import(other, ship, local, 1, &rep), HYP_ASK_VEC_INCOMPATIBLE);
    ASSERT(strstr(hyp_record_vectors_error(other), "m1") != NULL);
    ASSERT(strstr(hyp_record_vectors_error(other), "m2") != NULL);
    /* Refused means NOTHING landed — not even into pending. */
    ASSERT_EQ(hyp_record_vectors_count(other), 0);
    ASSERT_EQ(hyp_record_vectors_pending_count(other), 0);
    hyp_record_vectors_close(other);

    /* A different PREFIX CONTRACT refuses: same weights, different text fed
     * to them, a different space that model_id alone cannot catch. */
    other = hyp_record_vectors_open_path(TH_PATH(dir, "c.db"));
    ASSERT_NOT_NULL(other);
    ASSERT_EQ(hyp_record_vectors_begin(other, "m1", "c2", RV_TEST_DIM, 32768, false),
              HYP_ASK_VEC_OK);
    ASSERT_EQ(hyp_record_vectors_import(other, ship, local, 1, &rep), HYP_ASK_VEC_INCOMPATIBLE);
    ASSERT_EQ(hyp_record_vectors_pending_count(other), 0);
    hyp_record_vectors_close(other);

    /* A different WINDOW refuses. */
    other = hyp_record_vectors_open_path(TH_PATH(dir, "d.db"));
    ASSERT_NOT_NULL(other);
    ASSERT_EQ(hyp_record_vectors_begin(other, "m1", "c1", RV_TEST_DIM, 4096, false),
              HYP_ASK_VEC_OK);
    ASSERT_EQ(hyp_record_vectors_import(other, ship, local, 1, &rep), HYP_ASK_VEC_INCOMPATIBLE);
    hyp_record_vectors_close(other);

    /* A matching stamp is the positive control: the same file imports. */
    other = hyp_record_vectors_open_path(TH_PATH(dir, "e.db"));
    ASSERT_NOT_NULL(other);
    ASSERT_EQ(hyp_record_vectors_begin(other, "m1", "c1", RV_TEST_DIM, 32768, false),
              HYP_ASK_VEC_OK);
    ASSERT_EQ(hyp_record_vectors_import(other, ship, local, 1, &rep), HYP_ASK_VEC_OK);
    ASSERT_EQ(rep.attached, 1);
    hyp_record_vectors_close(other);

    hyp_record_free(rec);
    th_cleanup(dir);
    PASS();
}

TEST(record_vectors_import_refuses_garbage_and_truncation_atomically) {
    char *dir = th_mktempdir("hyp-recvec");
    ASSERT_NOT_NULL(dir);
    char ship[512];
    snprintf(ship, sizeof(ship), "%s", TH_PATH(dir, "ship.rvec"));

    const hyp_record_t *r1 = make_record("one", "agent:a", NULL, NULL, RV_FIXED_MS);
    const hyp_record_t *r2 = make_record("two", "agent:a", NULL, NULL, RV_FIXED_MS);
    ASSERT_NOT_NULL(r1);
    ASSERT_NOT_NULL(r2);
    float vec[RV_TEST_DIM];
    unit_axis(vec, RV_TEST_DIM, 0);

    hyp_record_vectors_t *sender = hyp_record_vectors_open_path(TH_PATH(dir, "a.db"));
    ASSERT_NOT_NULL(sender);
    ASSERT_EQ(hyp_record_vectors_begin(sender, "m1", "c1", RV_TEST_DIM, 32768, false),
              HYP_ASK_VEC_OK);
    ASSERT_EQ(hyp_record_vectors_put(sender, r1, vec), HYP_ASK_VEC_OK);
    ASSERT_EQ(hyp_record_vectors_put(sender, r2, vec), HYP_ASK_VEC_OK); /* contents differ */
    ASSERT_EQ(hyp_record_vectors_export(sender, ship, NULL), HYP_ASK_VEC_OK);
    hyp_record_vectors_close(sender);

    hyp_record_vectors_t *recv = hyp_record_vectors_open_path(TH_PATH(dir, "b.db"));
    ASSERT_NOT_NULL(recv);
    ASSERT_EQ(hyp_record_vectors_begin(recv, "m1", "c1", RV_TEST_DIM, 32768, false),
              HYP_ASK_VEC_OK);
    hyp_record_vec_import_report_t rep;

    /* Not a shipment at all. */
    ASSERT_EQ(th_write_file(TH_PATH(dir, "noise.bin"), "this is not a shipment"), 0);
    ASSERT_EQ(hyp_record_vectors_import(recv, TH_PATH(dir, "noise.bin"), NULL, 0, &rep),
              HYP_ASK_VEC_ERR);

    /* A shipment truncated mid-row: REFUSED WHOLE — the rows before the cut
     * do not land, because a partial import is a store whose contents depend
     * on where the file happened to end. */
    size_t len = 0;
    unsigned char *bytes = read_whole_file(ship, &len);
    ASSERT_NOT_NULL(bytes);
    char cutpath[512];
    snprintf(cutpath, sizeof(cutpath), "%s", TH_PATH(dir, "cut.rvec"));
    FILE *f = fopen(cutpath, "wb");
    ASSERT_NOT_NULL(f);
    ASSERT_EQ(fwrite(bytes, 1, len - 7, f), len - 7);
    fclose(f);
    free(bytes);
    ASSERT_EQ(hyp_record_vectors_import(recv, cutpath, NULL, 0, &rep), HYP_ASK_VEC_ERR);
    ASSERT_EQ(hyp_record_vectors_count(recv), 0);
    ASSERT_EQ(hyp_record_vectors_pending_count(recv), 0);

    hyp_record_free(r1);
    hyp_record_free(r2);
    hyp_record_vectors_close(recv);
    th_cleanup(dir);
    PASS();
}

TEST(record_vectors_stamp_gate_wipe_and_persistence) {
    char *dir = th_mktempdir("hyp-recvec");
    ASSERT_NOT_NULL(dir);
    char kept[512];
    snprintf(kept, sizeof(kept), "%s", TH_PATH(dir, "m.db"));

    const hyp_record_t *rec = make_record("some text", "agent:a", NULL, NULL, RV_FIXED_MS);
    ASSERT_NOT_NULL(rec);
    float vec[RV_TEST_DIM];
    unit_axis(vec, RV_TEST_DIM, 0);

    hyp_record_vectors_t *v = hyp_record_vectors_open_path(kept);
    ASSERT_NOT_NULL(v);
    ASSERT_EQ(hyp_record_vectors_begin(v, "m1", "c1", RV_TEST_DIM, 32768, false), HYP_ASK_VEC_OK);
    ASSERT_EQ(hyp_record_vectors_put(v, rec, vec), HYP_ASK_VEC_OK);

    /* A non-unit vector never reaches disk. */
    float bad[RV_TEST_DIM];
    for (int i = 0; i < RV_TEST_DIM; i++) {
        bad[i] = 1.0F;
    }
    ASSERT_EQ(hyp_record_vectors_put(v, rec, bad), HYP_ASK_VEC_ERR);

    /* Re-stamping with a different encoder refuses unless told to wipe... */
    ASSERT_EQ(hyp_record_vectors_begin(v, "m2", "c1", RV_TEST_DIM, 32768, false),
              HYP_ASK_VEC_INCOMPATIBLE);
    ASSERT_EQ(hyp_record_vectors_count(v), 1);
    /* ...and the SAME stamp is not a mismatch. */
    ASSERT_EQ(hyp_record_vectors_begin(v, "m1", "c1", RV_TEST_DIM, 32768, false), HYP_ASK_VEC_OK);
    hyp_record_vectors_close(v);

    /* The stamp survives a close and reopen, or the gate only ever worked
     * within the process that wrote it. */
    v = hyp_record_vectors_open_path(kept);
    ASSERT_NOT_NULL(v);
    hyp_record_vec_meta_t m;
    ASSERT_EQ(hyp_record_vectors_get_meta(v, &m), HYP_ASK_VEC_OK);
    ASSERT_EQ(m.format, HYP_RECORD_VEC_FORMAT);
    ASSERT_STR_EQ(m.model_id, "m1");
    ASSERT_STR_EQ(m.prefix_contract, "c1");
    ASSERT_EQ(m.dim, RV_TEST_DIM);
    ASSERT_EQ(m.window_tokens, 32768);
    ASSERT_EQ(m.row_count, 1);
    hyp_record_vec_meta_free(&m);

    /* Told to wipe: attached rows go, because a mixed matrix is undetectable
     * downstream. */
    ASSERT_EQ(hyp_record_vectors_begin(v, "m2", "c9", RV_TEST_DIM, 32768, true), HYP_ASK_VEC_OK);
    ASSERT_EQ(hyp_record_vectors_count(v), 0);
    ASSERT_EQ(hyp_record_vectors_pending_count(v), 0);

    hyp_record_free(rec);
    hyp_record_vectors_close(v);
    th_cleanup(dir);
    PASS();
}

/* A held vector is superseded when the content is embedded locally: the hash
 * ends up attached exactly once, in whichever way it arrived. */
TEST(record_vectors_local_put_supersedes_a_held_vector) {
    char *dir = th_mktempdir("hyp-recvec");
    ASSERT_NOT_NULL(dir);
    char ship[512];
    snprintf(ship, sizeof(ship), "%s", TH_PATH(dir, "ship.rvec"));

    const hyp_record_t *rec = make_record("late-arriving text", "agent:a", NULL, NULL, RV_FIXED_MS);
    ASSERT_NOT_NULL(rec);
    float vec[RV_TEST_DIM];
    unit_axis(vec, RV_TEST_DIM, 6);

    hyp_record_vectors_t *sender = hyp_record_vectors_open_path(TH_PATH(dir, "a.db"));
    ASSERT_NOT_NULL(sender);
    ASSERT_EQ(hyp_record_vectors_begin(sender, "m1", "c1", RV_TEST_DIM, 32768, false),
              HYP_ASK_VEC_OK);
    ASSERT_EQ(hyp_record_vectors_put(sender, rec, vec), HYP_ASK_VEC_OK);
    ASSERT_EQ(hyp_record_vectors_export(sender, ship, NULL), HYP_ASK_VEC_OK);
    hyp_record_vectors_close(sender);

    /* Receiver with NO matching local record: the vector is held. */
    hyp_record_vectors_t *recv = hyp_record_vectors_open_path(TH_PATH(dir, "b.db"));
    ASSERT_NOT_NULL(recv);
    ASSERT_EQ(hyp_record_vectors_begin(recv, "m1", "c1", RV_TEST_DIM, 32768, false),
              HYP_ASK_VEC_OK);
    hyp_record_vec_import_report_t rep;
    ASSERT_EQ(hyp_record_vectors_import(recv, ship, NULL, 0, &rep), HYP_ASK_VEC_OK);
    ASSERT_EQ(rep.held, 1);
    ASSERT_EQ(hyp_record_vectors_pending_count(recv), 1);

    /* The record arrives AND this machine embeds it itself: one attached row,
     * zero pending — never two copies of one hash. */
    ASSERT_EQ(hyp_record_vectors_put(recv, rec, vec), HYP_ASK_VEC_OK);
    ASSERT_EQ(hyp_record_vectors_count(recv), 1);
    ASSERT_EQ(hyp_record_vectors_pending_count(recv), 0);

    hyp_record_free(rec);
    hyp_record_vectors_close(recv);
    th_cleanup(dir);
    PASS();
}

SUITE(record_vectors) {
    RUN_TEST(record_vectors_unstamped_store_refuses_rather_than_guesses);
    RUN_TEST(record_vectors_identical_content_different_ids_is_one_row);
    RUN_TEST(record_vectors_ship_roundtrip_joins_and_holds);
    RUN_TEST(record_vectors_shipment_is_address_free_by_construction);
    RUN_TEST(record_vectors_import_refuses_a_different_stamp);
    RUN_TEST(record_vectors_import_refuses_garbage_and_truncation_atomically);
    RUN_TEST(record_vectors_stamp_gate_wipe_and_persistence);
    RUN_TEST(record_vectors_local_put_supersedes_a_held_vector);
}
