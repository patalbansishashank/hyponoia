/*
 * test_g2_retrieval.c — retrieval measured against decisions that were
 * actually taken (Track G, unit G2).
 *
 * THE GOLD IS NOT WRITTEN HERE, AND THAT IS THE WHOLE POINT. Every expected
 * answer is a paragraph lifted verbatim from a real commit message or from the
 * plan, located by a marker string; a derivation step outside this file fails
 * rather than paraphrasing when a marker stops matching. This file INGESTS
 * those paragraphs unchanged as decision records and then asks for them back.
 * Nothing in tests/fixtures/g2/retrieval_pairs.json was composed to be found.
 *
 * WHAT THIS MEASURES, SAID BEFORE THE NUMBER. The questions are the one
 * authored surface in the corpus and they share vocabulary with their golds:
 * a question asking why search_graph defaults to ten rows carries the words
 * the gold uses. So the lexical rates below answer "can the recorded decision
 * be found at all", NOT "can it be found when the words differ". A retriever
 * scored here is being handed the easier of the two problems, and a rate that
 * silently meant the easier thing would be worse than no rate.
 *
 * THE THREE LANES, AND WHY THEY ARE THREE. A client reaches memory through
 * search_memory. That tool refuses an anchor argument in this build, fail
 * closed, because no anchor resolver is wired into it — so the anchor lane
 * cannot be driven through the client surface and is driven against
 * record_store instead, and the refusal itself is asserted rather than worked
 * around. The free-text lane IS driven through the client surface, reading
 * what a client holds (structuredContent) rather than what the server emits.
 *
 * THE DISTRACTORS ARE REAL PROSE. Beside the decision records the store holds
 * the comment blocks this repository's own migration produces, attributed by
 * blame: real explanations of the same files the questions ask about. A
 * retrieval that cannot tell a decision from a neighbouring comment about the
 * same file has somewhere to fail.
 *
 * A DETECTOR THAT HAS NEVER SAID NO IS NOT A DETECTOR. Three controls carry
 * that weight: thousands of symbols whose why was never written down anywhere
 * must return nothing; the plausible-but-wrong answers each pair names must
 * return nothing; and a store holding the comments but none of the decisions
 * must return nothing for every anchor rather than the neighbour that shares
 * the file. Absent and empty stay distinguishable throughout — no store at all
 * withholds the records key, an empty answer supplies it.
 */
#include "test_framework.h"
#include "test_helpers.h"

#include <foundation/compat.h>
#include <foundation/constants.h>
#include <foundation/identity.h>
#include <foundation/log.h>
#include <foundation/record.h>
#include <mcp/mcp.h>
#include <memory/comment_migrate.h>
#include <store/record_store.h>
#include <yyjson/yyjson.h>

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define G2_PAIRS_PATH "tests/fixtures/g2/retrieval_pairs.json"
#define G2_NEVER_PATH "tests/fixtures/g2/never_recorded_names.txt"
#define G2_MANIFEST_PATH "tests/fixtures/g2/comment_blocks.manifest"

/* The corpus builds its addresses for a repository called hyponoia held in a
 * workspace of the same name. Both ends must agree or the anchor lane is
 * comparing two address spaces, so the migration is handed the same pair. */
#define G2_WORKSPACE "hyponoia"
#define G2_REPO "hyponoia"

#define G2_MAX_PAIRS 128
#define G2_MAX_TOKENS 24
#define G2_TOKEN_CAP 64

/* ── Reading the fixtures ───────────────────────────────────────────────── */

static char *g2_slurp(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)size + 1U);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got] = '\0';
    if (out_len) {
        *out_len = got;
    }
    return buf;
}

/* Days since the epoch for a civil date, so a record can carry the timestamp
 * of the commit that argued the decision instead of one this file invents. */
static int64_t g2_days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153U * (m + (m > 2 ? -3U : 9U)) + 2U) / 5U + d - 1U;
    const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

/* "YYYY-MM-DD" to epoch milliseconds. Zero when the text is not a date, which
 * the caller turns into a fixed stand-in rather than a clock reading: a record
 * id commits to its timestamp, so a clock here would mint a different corpus
 * on every run and nothing downstream could be compared. */
static int64_t g2_date_ms(const char *text) {
    int y = 0;
    int m = 0;
    int d = 0;
    if (!text || sscanf(text, "%4d-%2d-%2d", &y, &m, &d) != 3) {
        return 0;
    }
    if (m < 1 || m > 12 || d < 1 || d > 31) {
        return 0;
    }
    return g2_days_from_civil(y, (unsigned)m, (unsigned)d) * INT64_C(86400000);
}

/* ── Question tokens ────────────────────────────────────────────────────── */

/* Function words only. The list decides which words a question contributes to
 * the lexical lane, so it holds nothing a question could be about. */
static const char *const G2_STOPWORDS[] = {
    "the",   "and",  "for",    "from", "with",   "that",     "this",   "what",    "when",
    "why",   "how",  "does",   "did",  "was",    "were",     "are",    "its",     "not",
    "than",  "then", "into",   "over", "only",   "also",     "rather", "instead", "just",
    "still", "even", "ever",   "some", "any",    "all",      "more",   "most",    "less",
    "much",  "have", "has",    "had",  "been",   "being",    "would",  "should",  "could",
    "will",  "can",  "cannot", "must", "really", "actually", "which",  "who",     "there",
    "their", "them", "they",   "here", "about",  "come",     "comes",  "make",    "makes",
};

static bool g2_is_stopword(const char *token) {
    size_t n = sizeof(G2_STOPWORDS) / sizeof(G2_STOPWORDS[0]);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(token, G2_STOPWORDS[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Lowercase runs of identifier characters, four or more, that are not function
 * words. Derived from the question alone and never from the gold — a rule that
 * peeked at the answer would be scoring itself. */
static size_t g2_tokenize(const char *question, char out[G2_MAX_TOKENS][G2_TOKEN_CAP]) {
    size_t count = 0;
    const char *p = question ? question : "";
    while (*p && count < G2_MAX_TOKENS) {
        while (*p && !isalnum((unsigned char)*p) && *p != '_') {
            p++;
        }
        const char *start = p;
        while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
            p++;
        }
        size_t len = (size_t)(p - start);
        if (len < 4U || len >= G2_TOKEN_CAP) {
            continue;
        }
        char token[G2_TOKEN_CAP];
        for (size_t i = 0; i < len; i++) {
            token[i] = (char)tolower((unsigned char)start[i]);
        }
        token[len] = '\0';
        if (g2_is_stopword(token)) {
            continue;
        }
        bool seen = false;
        for (size_t i = 0; i < count; i++) {
            if (strcmp(out[i], token) == 0) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            snprintf(out[count], G2_TOKEN_CAP, "%s", token);
            count++;
        }
    }
    return count;
}

/* ── The corpus under test ──────────────────────────────────────────────── */

typedef struct {
    char cache[HYP_PATH_MAX];
    char memdir[HYP_PATH_MAX];
    hyp_record_store_t *store;
    hyp_mcp_server_t *srv;
    yyjson_doc *doc;
    yyjson_val *pairs;
    size_t npairs;
    size_t nanchored;
    size_t ncomments;
    char gold_id[G2_MAX_PAIRS][HYP_RECORD_ID_LEN + 1];
    bool ready;
    bool attempted;
    char why[HYP_SZ_512];
} g2_corpus_t;

static g2_corpus_t g_c;
static char *g_saved_cache;
static char *g_saved_memdir;
static HYPLogLevel g_saved_log;

static const char *g2_pair_question(yyjson_val *pair) {
    return yyjson_get_str(yyjson_obj_get(pair, "question"));
}

static const char *g2_pair_gold(yyjson_val *pair) {
    yyjson_val *gold = yyjson_obj_get(pair, "gold");
    return yyjson_get_str(yyjson_obj_get(gold, "decision_verbatim"));
}

static const char *g2_pair_address(yyjson_val *pair) {
    yyjson_val *asked = yyjson_obj_get(pair, "asked_about");
    return asked ? yyjson_get_str(yyjson_obj_get(asked, "address")) : NULL;
}

static const char *g2_pair_symbol(yyjson_val *pair) {
    yyjson_val *asked = yyjson_obj_get(pair, "asked_about");
    return asked ? yyjson_get_str(yyjson_obj_get(asked, "symbol")) : NULL;
}

static const char *g2_pair_file(yyjson_val *pair) {
    yyjson_val *asked = yyjson_obj_get(pair, "asked_about");
    return asked ? yyjson_get_str(yyjson_obj_get(asked, "file_path")) : NULL;
}

/* Ingest one pair as the decision it records. The content is the lifted
 * paragraph and nothing else: adding the commit subject or the question would
 * hand the lexical lane words the corpus never recorded. */
static bool g2_ingest_pair(hyp_record_store_t *store, yyjson_val *pair, char *id_out) {
    yyjson_val *gold = yyjson_obj_get(pair, "gold");
    yyjson_val *source = yyjson_obj_get(gold, "source");
    const char *content = yyjson_get_str(yyjson_obj_get(gold, "decision_verbatim"));
    const char *author = yyjson_get_str(yyjson_obj_get(source, "author"));
    const char *commit = yyjson_get_str(yyjson_obj_get(source, "commit"));
    const char *date = yyjson_get_str(yyjson_obj_get(source, "date"));
    const char *file = yyjson_get_str(yyjson_obj_get(source, "file"));
    const char *address = g2_pair_address(pair);
    if (!content || !content[0]) {
        return false;
    }

    char origin[HYP_SZ_512];
    if (commit && commit[0]) {
        snprintf(origin, sizeof(origin), "g2-commit:%s", commit);
    } else if (file && file[0]) {
        snprintf(origin, sizeof(origin), "g2-plan:%lld",
                 (long long)yyjson_get_sint(yyjson_obj_get(source, "line")));
    } else {
        snprintf(origin, sizeof(origin), "g2-unsourced");
    }

    int64_t when = g2_date_ms(date);
    if (when <= 0) {
        /* The plan carries no commit date. One fixed instant keeps the ids
         * reproducible; nothing in this file asserts on time. */
        when = INT64_C(1700000000000);
    }

    hyp_record_input_t in;
    memset(&in, 0, sizeof(in));
    in.kind = HYP_RECORD_DECISION;
    in.author = (author && author[0]) ? author : "the plan";
    in.timestamp_ms = when;
    in.content = content;
    in.anchor = (address && address[0]) ? address : NULL;
    in.origin = origin;

    const hyp_record_t *rec = NULL;
    if (hyp_record_build(&in, &rec) != HYP_RECORD_OK || !rec) {
        return false;
    }
    hyp_record_store_status_t st = hyp_record_store_append(store, rec, NULL);
    if (st == HYP_RECORD_STORE_OK && id_out) {
        snprintf(id_out, HYP_RECORD_ID_LEN + 1, "%s", rec->id);
    }
    hyp_record_free(rec);
    return st == HYP_RECORD_STORE_OK;
}

static void g2_corpus_free(void) {
    if (g_c.srv) {
        hyp_mcp_server_free(g_c.srv);
        g_c.srv = NULL;
    }
    if (g_c.store) {
        hyp_record_store_close(g_c.store);
        g_c.store = NULL;
    }
    if (g_c.doc) {
        yyjson_doc_free(g_c.doc);
        g_c.doc = NULL;
    }
    if (g_c.cache[0]) {
        th_cleanup(g_c.cache);
        g_c.cache[0] = '\0';
    }
    if (g_saved_cache) {
        hyp_setenv("HYP_CACHE_DIR", g_saved_cache, 1);
        free(g_saved_cache);
        g_saved_cache = NULL;
    } else {
        hyp_unsetenv("HYP_CACHE_DIR");
    }
    if (g_saved_memdir) {
        hyp_setenv("HYP_MEMORY_DIR", g_saved_memdir, 1);
        free(g_saved_memdir);
        g_saved_memdir = NULL;
    } else {
        hyp_unsetenv("HYP_MEMORY_DIR");
    }
    if (g_c.attempted) {
        hyp_log_set_level(g_saved_log);
    }
    g_c.ready = false;
    g_c.attempted = false;
}

/* Build once. Every test asks for the same store, because populating it is
 * two hundred and seventy-odd digests and a manifest parse, and because the
 * lanes are supposed to be measured against ONE corpus. */
static bool g2_corpus_ensure(void) {
    if (g_c.attempted) {
        return g_c.ready;
    }
    g_c.attempted = true;
    snprintf(g_c.why, sizeof(g_c.why), "unset");

    /* Thousands of control queries at info level would bury the measurement in
     * one request line each. The level is restored when the suite ends. */
    g_saved_log = hyp_log_get_level();
    hyp_log_set_level(HYP_LOG_ERROR);

    const char *old_cache = getenv("HYP_CACHE_DIR");
    g_saved_cache = old_cache ? hyp_strdup(old_cache) : NULL;
    const char *old_mem = getenv("HYP_MEMORY_DIR");
    g_saved_memdir = old_mem ? hyp_strdup(old_mem) : NULL;

    char *base = th_mktempdir("hyp_g2");
    if (!base) {
        snprintf(g_c.why, sizeof(g_c.why), "no temp directory");
        return false;
    }
    snprintf(g_c.cache, sizeof(g_c.cache), "%s", base);
    snprintf(g_c.memdir, sizeof(g_c.memdir), "%s/memory", base);
    hyp_setenv("HYP_CACHE_DIR", g_c.cache, 1);
    hyp_setenv("HYP_MEMORY_DIR", g_c.memdir, 1);

    size_t len = 0;
    char *text = g2_slurp(G2_PAIRS_PATH, &len);
    if (!text) {
        snprintf(g_c.why, sizeof(g_c.why), "cannot read %s", G2_PAIRS_PATH);
        return false;
    }
    g_c.doc = yyjson_read(text, len, 0);
    free(text);
    if (!g_c.doc) {
        snprintf(g_c.why, sizeof(g_c.why), "%s is not JSON", G2_PAIRS_PATH);
        return false;
    }
    g_c.pairs = yyjson_obj_get(yyjson_doc_get_root(g_c.doc), "pairs");
    if (!g_c.pairs || !yyjson_is_arr(g_c.pairs)) {
        snprintf(g_c.why, sizeof(g_c.why), "%s carries no pairs array", G2_PAIRS_PATH);
        return false;
    }
    g_c.npairs = yyjson_arr_size(g_c.pairs);
    if (g_c.npairs == 0U || g_c.npairs > G2_MAX_PAIRS) {
        snprintf(g_c.why, sizeof(g_c.why), "%zu pairs is not a corpus this harness holds",
                 g_c.npairs);
        return false;
    }

    if (hyp_record_store_open(g_c.memdir, &g_c.store) != HYP_RECORD_STORE_OK || !g_c.store) {
        snprintf(g_c.why, sizeof(g_c.why), "the record store refused to open");
        return false;
    }

    size_t i = 0;
    size_t imax = 0;
    yyjson_val *pair = NULL;
    yyjson_arr_foreach(g_c.pairs, i, imax, pair) {
        if (!g2_ingest_pair(g_c.store, pair, g_c.gold_id[i])) {
            snprintf(g_c.why, sizeof(g_c.why), "pair %zu could not be recorded", i);
            return false;
        }
        const char *addr = g2_pair_address(pair);
        if (addr && addr[0]) {
            g_c.nanchored++;
        }
    }

    hyp_comment_migrate_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.workspace = G2_WORKSPACE;
    opts.repo = G2_REPO;
    hyp_comment_migrate_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    char err[HYP_SZ_512];
    err[0] = '\0';
    hyp_comment_migrate_status_t mst =
        hyp_comment_migrate_run(G2_MANIFEST_PATH, &opts, g_c.store, &stats, err, sizeof(err));
    if (mst != HYP_COMMENT_MIGRATE_OK) {
        snprintf(g_c.why, sizeof(g_c.why), "the comment corpus refused: %s (%s)",
                 hyp_comment_migrate_status_reason(mst), err);
        return false;
    }
    g_c.ncomments = stats.appended;

    g_c.srv = hyp_mcp_server_new(NULL);
    if (!g_c.srv) {
        snprintf(g_c.why, sizeof(g_c.why), "no MCP server");
        return false;
    }
    hyp_mcp_server_set_tool_profile(g_c.srv, HYP_MCP_TOOL_PROFILE_ALL);
    g_c.ready = true;
    return true;
}

/* ── Driving search_memory the way a client does ────────────────────────── */

static void g2_json_escape(const char *s, char *out, size_t cap) {
    size_t o = 0;
    for (const char *p = s ? s : ""; *p && o + 8U < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c == '\n') {
            out[o++] = '\\';
            out[o++] = 'n';
        } else if (c < 0x20) {
            o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c);
        } else {
            out[o++] = (char)c;
        }
    }
    out[o < cap ? o : cap - 1U] = '\0';
}

typedef struct {
    char *raw;
    yyjson_doc *doc;
    yyjson_val *result;
    yyjson_val *structured;
    const char *text;
    bool is_error;
} g2_reply_t;

static void g2_reply_free(g2_reply_t *r) {
    if (!r) {
        return;
    }
    if (r->doc) {
        yyjson_doc_free(r->doc);
    }
    free(r->raw);
    memset(r, 0, sizeof(*r));
}

/* One tools/call over the wire shape a client sends, with the reply read the
 * way a client reads it. structuredContent is what a client that trusts
 * structure holds; content[0].text is what one that reads prose holds. Both
 * are surfaced here so a test can assert the two ends agree. */
static bool g2_call(const char *args, g2_reply_t *out) {
    memset(out, 0, sizeof(*out));
    size_t cap = strlen(args) + 256U;
    char *line = (char *)malloc(cap);
    if (!line) {
        return false;
    }
    snprintf(line, cap,
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"search_memory\",\"arguments\":%s}}",
             args);
    out->raw = hyp_mcp_server_handle(g_c.srv, line);
    free(line);
    if (!out->raw) {
        return false;
    }
    out->doc = yyjson_read(out->raw, strlen(out->raw), 0);
    if (!out->doc) {
        return false;
    }
    out->result = yyjson_obj_get(yyjson_doc_get_root(out->doc), "result");
    if (!out->result) {
        return false;
    }
    out->is_error = yyjson_get_bool(yyjson_obj_get(out->result, "isError"));
    out->structured = yyjson_obj_get(out->result, "structuredContent");
    yyjson_val *content = yyjson_obj_get(out->result, "content");
    yyjson_val *first = content ? yyjson_arr_get(content, 0) : NULL;
    out->text = first ? yyjson_get_str(yyjson_obj_get(first, "text")) : NULL;
    return true;
}

/* A free-text search, as JSON, with the ceiling raised so a miss is a miss and
 * never a truncation. */
static bool g2_search(const char *query, g2_reply_t *out) {
    char escaped[HYP_SZ_1K];
    g2_json_escape(query, escaped, sizeof(escaped));
    char args[HYP_SZ_2K];
    snprintf(args, sizeof(args), "{\"query\":\"%s\",\"format\":\"json\",\"limit\":500}", escaped);
    return g2_call(args, out);
}

/* How many records the client is holding, and whether one of them is the id
 * this pair recorded. */
static size_t g2_rows(const g2_reply_t *r, const char *want_id, bool *found) {
    if (found) {
        *found = false;
    }
    yyjson_val *records = r->structured ? yyjson_obj_get(r->structured, "records") : NULL;
    if (!records || !yyjson_is_arr(records)) {
        return 0U;
    }
    size_t i = 0;
    size_t imax = 0;
    yyjson_val *row = NULL;
    yyjson_arr_foreach(records, i, imax, row) {
        const char *id = yyjson_get_str(yyjson_obj_get(row, "id"));
        if (found && want_id && id && strcmp(id, want_id) == 0) {
            *found = true;
        }
    }
    return yyjson_arr_size(records);
}

/* True when `needle` appears in `hay` at identifier boundaries. The
 * never-recorded set claims no record NAMES the symbol; the tool's filter is a
 * raw case-folded substring, so a short name buried inside a longer identifier
 * comes back as a match without anyone having named anything. The two are
 * different claims and this is where they are told apart. */
static bool g2_names_identifier(const char *hay, const char *needle) {
    size_t n = needle ? strlen(needle) : 0U;
    if (!hay || n == 0U) {
        return false;
    }
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i != n) {
            continue;
        }
        char before = (p == hay) ? ' ' : p[-1];
        char after = p[n];
        bool lead = !(isalnum((unsigned char)before) || before == '_');
        bool trail = !(isalnum((unsigned char)after) || after == '_');
        if (lead && trail) {
            return true;
        }
    }
    return false;
}

/* ── The anchor lane, against the store the client cannot reach ─────────── */

static bool g2_anchor_holds(const char *anchor, const char *want_id, size_t *out_count) {
    hyp_record_store_query_t q;
    memset(&q, 0, sizeof(q));
    q.anchor = anchor;
    hyp_record_set_t *set = NULL;
    if (hyp_record_store_query(g_c.store, &q, &set) != HYP_RECORD_STORE_OK || !set) {
        if (out_count) {
            *out_count = 0U;
        }
        return false;
    }
    size_t n = hyp_record_set_count(set);
    bool found = hyp_record_set_get(set, want_id) != NULL;
    hyp_record_set_free(set);
    if (out_count) {
        *out_count = n;
    }
    return found;
}

/* ── Tests ──────────────────────────────────────────────────────────────── */

TEST(the_corpus_ingests_without_a_gold_being_written) {
    if (!g2_corpus_ensure()) {
        FAIL(g_c.why);
    }
    size_t total = 0;
    ASSERT_EQ(hyp_record_store_count(g_c.store, &total), HYP_RECORD_STORE_OK);
    ASSERT_EQ(total, g_c.npairs + g_c.ncomments);
    fprintf(stderr,
            "\n  G2 corpus: %zu decisions (%zu carrying a symbol address) + %zu migrated "
            "comment blocks = %zu records\n",
            g_c.npairs, g_c.nanchored, g_c.ncomments, total);

    /* The content is the paragraph and nothing beside it. A record whose text
     * had gained a title or a question would be scoring the harness. */
    size_t i = 0;
    size_t imax = 0;
    yyjson_val *pair = NULL;
    yyjson_arr_foreach(g_c.pairs, i, imax, pair) {
        const hyp_record_t *rec = NULL;
        hyp_record_store_status_t got = hyp_record_store_get(g_c.store, g_c.gold_id[i], &rec);
        const char *gold = g2_pair_gold(pair);
        bool same = got == HYP_RECORD_STORE_OK && rec && gold && strcmp(rec->content, gold) == 0;
        hyp_record_free(rec);
        if (!same) {
            FAIL("a recorded decision is not the paragraph the corpus lifted");
        }
    }
    PASS();
}

TEST(an_anchor_query_returns_the_decision_recorded_against_that_symbol) {
    if (!g2_corpus_ensure()) {
        FAIL(g_c.why);
    }
    size_t asked = 0;
    size_t hit = 0;
    size_t rows = 0;
    size_t i = 0;
    size_t imax = 0;
    yyjson_val *pair = NULL;
    yyjson_arr_foreach(g_c.pairs, i, imax, pair) {
        const char *addr = g2_pair_address(pair);
        if (!addr || !addr[0]) {
            continue;
        }
        asked++;
        size_t n = 0;
        if (g2_anchor_holds(addr, g_c.gold_id[i], &n)) {
            hit++;
        } else {
            fprintf(stderr, "\n  anchor miss: %s\n    %s\n",
                    yyjson_get_str(yyjson_obj_get(pair, "id")), addr);
        }
        rows += n;
    }
    fprintf(stderr, "\n  anchor lane: %zu/%zu, %.2f records returned per question\n", hit, asked,
            asked ? (double)rows / (double)asked : 0.0);
    ASSERT_EQ(hit, asked);
    ASSERT_EQ(asked, g_c.nanchored);
    PASS();
}

TEST(the_module_address_the_product_derives_is_the_one_the_corpus_assumed) {
    if (!g2_corpus_ensure()) {
        FAIL(g_c.why);
    }
    hyp_comment_migrate_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.workspace = G2_WORKSPACE;
    opts.repo = G2_REPO;

    size_t asked = 0;
    size_t agree = 0;
    size_t refused = 0;
    size_t i = 0;
    size_t imax = 0;
    yyjson_val *pair = NULL;
    yyjson_arr_foreach(g_c.pairs, i, imax, pair) {
        const char *addr = g2_pair_address(pair);
        const char *file = g2_pair_file(pair);
        if (!addr || !addr[0] || !file || !file[0]) {
            continue;
        }
        asked++;
        char module[HYP_ADDR_MAX + 1];
        module[0] = '\0';
        if (hyp_comment_migrate_anchor(&opts, file, module, sizeof(module)) !=
            HYP_COMMENT_MIGRATE_OK) {
            refused++;
            continue;
        }
        size_t mlen = strlen(module);
        if (strncmp(addr, module, mlen) == 0 && addr[mlen] == '.') {
            agree++;
        } else {
            fprintf(stderr, "\n  address disagreement for %s\n    corpus:  %s\n    product: %s\n",
                    file, addr, module);
        }
    }
    fprintf(stderr, "\n  module address: %zu/%zu agree, %zu paths the product refuses\n", agree,
            asked, refused);
    /* Every one, and the corpus cannot check this about itself: its addresses
     * were derived by hand from the documented naming rule, so agreement here
     * is the only evidence that the anchor lane above is not measuring one
     * address space against another. A shell script and a vendored assembler
     * blob are among the paths and the product names them too. */
    ASSERT_EQ(refused, 0U);
    ASSERT_EQ(agree, asked);
    PASS();
}

TEST(a_module_prefix_returns_the_decision_and_the_comments_about_that_file) {
    if (!g2_corpus_ensure()) {
        FAIL(g_c.why);
    }
    hyp_comment_migrate_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.workspace = G2_WORKSPACE;
    opts.repo = G2_REPO;

    size_t asked = 0;
    size_t withgold = 0;
    size_t withcomment = 0;
    size_t i = 0;
    size_t imax = 0;
    yyjson_val *pair = NULL;
    yyjson_arr_foreach(g_c.pairs, i, imax, pair) {
        const char *addr = g2_pair_address(pair);
        const char *file = g2_pair_file(pair);
        if (!addr || !addr[0] || !file || !file[0]) {
            continue;
        }
        char module[HYP_ADDR_MAX + 1];
        module[0] = '\0';
        if (hyp_comment_migrate_anchor(&opts, file, module, sizeof(module)) !=
            HYP_COMMENT_MIGRATE_OK) {
            continue;
        }
        size_t mlen = strlen(module);
        if (strncmp(addr, module, mlen) != 0) {
            continue;
        }
        asked++;
        hyp_record_store_query_t q;
        memset(&q, 0, sizeof(q));
        q.anchor_prefix = module;
        hyp_record_set_t *set = NULL;
        if (hyp_record_store_query(g_c.store, &q, &set) != HYP_RECORD_STORE_OK || !set) {
            continue;
        }
        if (hyp_record_set_get(set, g_c.gold_id[i])) {
            withgold++;
        }
        bool comment = false;
        for (size_t k = 0; k < hyp_record_set_count(set); k++) {
            const hyp_record_t *rec = hyp_record_set_at(set, k);
            if (rec && rec->origin && strncmp(rec->origin, "comment:", 8) == 0) {
                comment = true;
                break;
            }
        }
        if (comment) {
            withcomment++;
        }
        hyp_record_set_free(set);
    }
    fprintf(stderr,
            "\n  module prefix: %zu/%zu return the decision, %zu also return migrated prose "
            "about the same file\n",
            withgold, asked, withcomment);
    ASSERT_EQ(withgold, asked);
    ASSERT_GT(withcomment, 0U);
    PASS();
}

TEST(search_memory_answers_an_anchor_and_reports_what_it_resolved_to) {
    if (!g2_corpus_ensure()) {
        FAIL(g_c.why);
    }
    yyjson_val *pair = yyjson_arr_get(g_c.pairs, 0);
    const char *addr = g2_pair_address(pair);
    ASSERT_NOT_NULL(addr);
    char escaped[HYP_SZ_1K];
    g2_json_escape(addr, escaped, sizeof(escaped));
    char args[HYP_SZ_2K];
    snprintf(args, sizeof(args), "{\"anchor\":\"%s\",\"format\":\"json\"}", escaped);

    g2_reply_t r;
    if (!g2_call(args, &r)) {
        g2_reply_free(&r);
        FAIL("the anchor call produced no reply a client could read");
    }
    /* This used to assert a REFUSAL: the store could answer an anchored query
     * and the client surface would not, because reporting a status it could not
     * compute hands back a superset that reads like a match. The resolver is
     * wired now, so the surface answers — and the thing that makes the answer
     * safe is the same thing that made the refusal necessary: every anchored
     * row states what its anchor resolves to, so a match is never mistaken for
     * a live attachment.
     *
     * `records` PRESENT is the assertion, not `records` non-empty. An anchor
     * that resolves with nothing recorded against it is a real answer and an
     * empty list is the honest way to say it; the key being absent is what
     * would mean "there is nowhere this could have been recorded". */
    ASSERT_FALSE(r.is_error);
    ASSERT_NOT_NULL(r.structured);
    yyjson_val *rows = yyjson_obj_get(r.structured, "records");
    ASSERT_NOT_NULL(rows);
    size_t ri = 0;
    size_t rmax = 0;
    yyjson_val *row = NULL;
    yyjson_arr_foreach(rows, ri, rmax, row) {
        /* Anchored rows carry the status; the filter only admits anchored
         * rows, so every row here must. */
        ASSERT_NOT_NULL(yyjson_obj_get(row, "anchor_status"));
    }
    g2_reply_free(&r);
    PASS();
}

TEST(the_published_signature_and_the_handler_are_live_together) {
    if (!g2_corpus_ensure()) {
        FAIL(g_c.why);
    }
    /* THE CONVERGED STATE, pinned from both directions — and it has now
     * converged the other way.
     *
     * This test spent its first life pinning ABSENCE: the schema had offered
     * `anchor` and a `status` defaulting to "attached" while the handler
     * refused both, so a client built from the schema was refused for sending
     * the schema's own default. The refusals were the right end (a filter whose
     * state cannot be reported returns a superset that reads like a match), so
     * the arguments came out of the signature and the test asserted they stayed
     * out until a resolver landed.
     *
     * A resolver landed. src/memory/anchor.c is wired behind record_memory and
     * src/memory/orphan.c behind search_memory, in one commit with the schema,
     * which is the condition the old comment named. So the assertion inverts,
     * and it still fails in BOTH directions:
     *   (a) ADVERTISED — tools/list offers anchor on both tools and status on
     *       the reader. Read off tools/list, not the table it is built from,
     *       because the question is what a CLIENT holds.
     *   (b) ANSWERED — the handler accepts them rather than refusing, and an
     *       anchored answer carries anchor_status.
     * Un-advertise without unwiring and (a) fails; unwire without
     * un-advertising and (b) fails. Neither half can drift alone. */
    char *listed = hyp_mcp_server_handle(
        g_c.srv, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{}}");
    if (!listed) {
        FAIL("tools/list produced nothing a client could read");
    }
    yyjson_doc *ldoc = yyjson_read(listed, strlen(listed), 0);
    yyjson_val *tools =
        ldoc ? yyjson_obj_get(yyjson_obj_get(yyjson_doc_get_root(ldoc), "result"), "tools") : NULL;
    int seen = 0;
    bool offers_anchor = false;
    bool offers_status = false;
    bool teaches_anchor_status = false;
    bool writer_offers_anchor = false;
    size_t ti = 0;
    size_t tmax = 0;
    yyjson_val *tool = NULL;
    yyjson_arr_foreach(tools, ti, tmax, tool) {
        const char *name = yyjson_get_str(yyjson_obj_get(tool, "name"));
        if (!name) {
            continue;
        }
        bool reader = strcmp(name, "search_memory") == 0;
        bool writer = strcmp(name, "record_memory") == 0;
        if (!reader && !writer) {
            continue;
        }
        seen++;
        yyjson_val *props = yyjson_obj_get(yyjson_obj_get(tool, "inputSchema"), "properties");
        const char *desc = yyjson_get_str(yyjson_obj_get(tool, "description"));
        if (reader) {
            offers_anchor = props && yyjson_obj_get(props, "anchor") != NULL;
            offers_status = props && yyjson_obj_get(props, "status") != NULL;
            teaches_anchor_status = desc && strstr(desc, "anchor_status") != NULL;
        } else {
            writer_offers_anchor = props && yyjson_obj_get(props, "anchor") != NULL;
        }
    }
    yyjson_doc_free(ldoc);
    free(listed);

    /* The instrument, before anything that trusts it. */
    if (seen != 2) {
        FAIL("tools/list does not carry both memory tools; the walk is broken, not the surface");
    }
    if (!offers_anchor || !offers_status) {
        FAIL("search_memory no longer advertises anchor or status while the resolver is still "
             "wired behind them — an argument the handler answers and the schema hides is "
             "undiscoverable, which is the same divergence upside down");
    }
    if (!teaches_anchor_status) {
        FAIL("search_memory's description no longer teaches anchor_status, but anchored answers "
             "carry the key — a client is left to discover the field by accident");
    }
    if (!writer_offers_anchor) {
        FAIL("record_memory no longer advertises anchor while its handler resolves one — both "
             "memory tools carry it together or neither does");
    }

    /* (b) Answered, not refused. The exact call the schema's own contract
     * generates: the address of a real pair, and the status value the schema
     * declares as its default. */
    yyjson_val *pair = yyjson_arr_get(g_c.pairs, 0);
    const char *addr = g2_pair_address(pair);
    ASSERT_NOT_NULL(addr);
    char escaped[HYP_SZ_1K];
    g2_json_escape(addr, escaped, sizeof(escaped));
    char args[HYP_SZ_2K];
    snprintf(args, sizeof(args), "{\"anchor\":\"%s\",\"status\":\"any\",\"format\":\"json\"}",
             escaped);
    g2_reply_t anchored;
    if (!g2_call(args, &anchored)) {
        g2_reply_free(&anchored);
        FAIL("the anchor call produced no reply a client could read");
    }
    bool anchor_refused = anchored.is_error;
    g2_reply_free(&anchored);
    if (anchor_refused) {
        FAIL("the handler refuses an anchor it advertises — if the resolver was unwired, "
             "un-advertise anchor and status in this same commit");
    }

    g2_reply_t defaulted;
    if (!g2_call("{\"status\":\"attached\",\"format\":\"json\"}", &defaulted)) {
        g2_reply_free(&defaulted);
        FAIL("the status call produced no reply a client could read");
    }
    bool status_refused = defaulted.is_error;
    g2_reply_free(&defaulted);
    if (status_refused) {
        FAIL("the handler refuses status='attached', which the schema declares as its default "
             "when an anchor is supplied — a client obeying the contract it was handed is "
             "refused for it");
    }

    fprintf(stderr, "\n  signature against handler: both memory tools advertise anchor, "
                    "search_memory advertises status, and the handler answers all three\n");
    PASS();
}

TEST(free_text_from_the_symbol_name_finds_the_decision_about_it) {
    if (!g2_corpus_ensure()) {
        FAIL(g_c.why);
    }
    size_t asked = 0;
    size_t hit = 0;
    size_t rows = 0;
    size_t i = 0;
    size_t imax = 0;
    yyjson_val *pair = NULL;
    yyjson_arr_foreach(g_c.pairs, i, imax, pair) {
        const char *sym = g2_pair_symbol(pair);
        if (!sym || !sym[0]) {
            continue;
        }
        asked++;
        g2_reply_t r;
        if (!g2_search(sym, &r)) {
            g2_reply_free(&r);
            FAIL("search_memory produced no reply a client could read");
        }
        bool found = false;
        rows += g2_rows(&r, g_c.gold_id[i], &found);
        if (found) {
            hit++;
        }
        g2_reply_free(&r);
    }
    fprintf(stderr,
            "\n  symbol-name lane (no vocabulary shared with the gold): %zu/%zu, %.2f records "
            "returned per question\n",
            hit, asked, asked ? (double)rows / (double)asked : 0.0);
    /* Pinned at what this corpus actually reaches, which is low, and the low
     * number is the finding rather than a fault in the pin: a decision
     * paragraph argues a design and hardly ever spells the identifier, so a
     * substring filter keyed on the name of the open symbol has almost nothing
     * to match. Raising this pin is what a semantic lane over records would
     * be for. */
    ASSERT_GTE(hit, 5U);
    PASS();
}

TEST(free_text_from_the_question_wording_finds_the_decision) {
    if (!g2_corpus_ensure()) {
        FAIL(g_c.why);
    }
    size_t oneshot = 0;
    size_t anyterm = 0;
    size_t rows = 0;
    size_t queries = 0;
    size_t i = 0;
    size_t imax = 0;
    yyjson_val *pair = NULL;
    yyjson_arr_foreach(g_c.pairs, i, imax, pair) {
        char tokens[G2_MAX_TOKENS][G2_TOKEN_CAP];
        size_t n = g2_tokenize(g2_pair_question(pair), tokens);
        size_t longest = 0;
        for (size_t t = 1; t < n; t++) {
            if (strlen(tokens[t]) > strlen(tokens[longest])) {
                longest = t;
            }
        }
        bool any = false;
        for (size_t t = 0; t < n; t++) {
            g2_reply_t r;
            if (!g2_search(tokens[t], &r)) {
                g2_reply_free(&r);
                FAIL("search_memory produced no reply a client could read");
            }
            bool found = false;
            rows += g2_rows(&r, g_c.gold_id[i], &found);
            queries++;
            g2_reply_free(&r);
            if (found) {
                any = true;
                if (t == longest) {
                    oneshot++;
                }
            }
        }
        if (any) {
            anyterm++;
        }
    }
    fprintf(stderr,
            "\n  question-wording lane (vocabulary IS shared with the gold, so this is the "
            "inflated one):\n    one shot, longest content word: %zu/%zu\n    agent retries every "
            "content word: %zu/%zu\n    %.1f records returned per query over %zu queries\n",
            oneshot, g_c.npairs, anyterm, g_c.npairs,
            queries ? (double)rows / (double)queries : 0.0, queries);
    /* Both pinned at the measured rate. The distance between this lane and the
     * symbol-name one above is shared vocabulary and nothing else, which is
     * why neither number is ever quoted without the other. */
    ASSERT_GTE(oneshot, 33U);
    ASSERT_GTE(anyterm, 55U);
    PASS();
}

TEST(a_question_whose_answer_was_never_recorded_returns_nothing) {
    if (!g2_corpus_ensure()) {
        FAIL(g_c.why);
    }
    size_t len = 0;
    char *names = g2_slurp(G2_NEVER_PATH, &len);
    if (!names) {
        FAIL("cannot read the never-recorded control set");
    }
    size_t asked = 0;
    size_t answered = 0;
    size_t named = 0;
    char *save = names;
    char *line = names;
    while (*line) {
        char *end = strchr(line, '\n');
        if (end) {
            *end = '\0';
        }
        if (line[0]) {
            asked++;
            g2_reply_t r;
            if (!g2_search(line, &r)) {
                g2_reply_free(&r);
                free(save);
                FAIL("search_memory produced no reply a client could read");
            }
            /* Empty, not absent: a store was read and it holds nothing about
             * this. The records key is present and carries zero rows. */
            yyjson_val *records = r.structured ? yyjson_obj_get(r.structured, "records") : NULL;
            if (!records || !yyjson_is_arr(records)) {
                g2_reply_free(&r);
                free(save);
                FAIL("a read store answered without a records key");
            }
            size_t n = yyjson_arr_size(records);
            if (n != 0U) {
                answered++;
                bool does_name = false;
                size_t ri = 0;
                size_t rmax = 0;
                yyjson_val *row = NULL;
                yyjson_arr_foreach(records, ri, rmax, row) {
                    if (g2_names_identifier(yyjson_get_str(yyjson_obj_get(row, "content")), line)) {
                        does_name = true;
                        break;
                    }
                }
                if (does_name) {
                    named++;
                }
                fprintf(stderr, "\n  never recorded, yet answered: %-28s %zu records, named=%s\n",
                        line, n, does_name ? "yes" : "no");
            }
            g2_reply_free(&r);
        }
        if (!end) {
            break;
        }
        line = end + 1;
    }
    free(save);
    fprintf(stderr,
            "\n  never-recorded control: %zu asked, %zu answered at all, %zu of those held a "
            "record that NAMES the symbol\n",
            asked, answered, named);
    ASSERT_GT(asked, 4000U);
    /* Two pins rather than a zero, because the two failure modes are not the
     * same thing and collapsing them would hide the interesting one.
     *
     * `answered` above its pin means the retrieval path started returning
     * records for questions with no recorded answer — the failure this control
     * exists for. The pinned few are a short name sitting inside a longer
     * identifier, which the tool's substring filter cannot help but match.
     *
     * `named` above its pin means the corpus's claim is wrong for one more
     * symbol: a record here does name it, so its why WAS written down. Those
     * are corpus findings and are printed by name rather than repaired, since
     * repairing a control by hand is how a gold gets authored. */
    ASSERT_LTE(answered, 8U);
    ASSERT_LTE(named, 3U);
    PASS();
}

TEST(a_plausible_wrong_answer_is_never_returned) {
    if (!g2_corpus_ensure()) {
        FAIL(g_c.why);
    }
    size_t asked = 0;
    size_t spoke = 0;
    size_t i = 0;
    size_t imax = 0;
    yyjson_val *pair = NULL;
    yyjson_arr_foreach(g_c.pairs, i, imax, pair) {
        yyjson_val *wrong = yyjson_obj_get(pair, "must_not_return");
        size_t j = 0;
        size_t jmax = 0;
        yyjson_val *phrase = NULL;
        yyjson_arr_foreach(wrong, j, jmax, phrase) {
            const char *needle = yyjson_get_str(phrase);
            if (!needle || !needle[0]) {
                continue;
            }
            asked++;
            g2_reply_t r;
            if (!g2_search(needle, &r)) {
                g2_reply_free(&r);
                FAIL("search_memory produced no reply a client could read");
            }
            size_t n = g2_rows(&r, NULL, NULL);
            if (n != 0U) {
                spoke++;
                fprintf(stderr, "\n  wrong answer returned for %s: '%s' -> %zu records\n",
                        yyjson_get_str(yyjson_obj_get(pair, "id")), needle, n);
            }
            g2_reply_free(&r);
        }
    }
    fprintf(stderr, "\n  named-wrong-answer control: %zu asked, %zu answered\n", asked, spoke);
    ASSERT_GT(asked, 0U);
    ASSERT_EQ(spoke, 0U);
    PASS();
}

TEST(without_the_decision_an_anchor_returns_nothing_rather_than_a_neighbour) {
    if (!g2_corpus_ensure()) {
        FAIL(g_c.why);
    }
    /* A second store holding the migrated comments and none of the decisions.
     * Every question's anchor is then answerable only by a neighbour, and the
     * right answer is silence. */
    char *base = th_mktempdir("hyp_g2n");
    if (!base) {
        FAIL("no temp directory");
    }
    char dir[HYP_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", base);
    hyp_record_store_t *bare = NULL;
    if (hyp_record_store_open(dir, &bare) != HYP_RECORD_STORE_OK || !bare) {
        th_cleanup(dir);
        FAIL("the control store refused to open");
    }
    hyp_comment_migrate_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.workspace = G2_WORKSPACE;
    opts.repo = G2_REPO;
    hyp_comment_migrate_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    char err[HYP_SZ_512];
    err[0] = '\0';
    if (hyp_comment_migrate_run(G2_MANIFEST_PATH, &opts, bare, &stats, err, sizeof(err)) !=
        HYP_COMMENT_MIGRATE_OK) {
        hyp_record_store_close(bare);
        th_cleanup(dir);
        FAIL("the control corpus refused");
    }

    size_t asked = 0;
    size_t spoke = 0;
    size_t neighbours = 0;
    size_t i = 0;
    size_t imax = 0;
    yyjson_val *pair = NULL;
    yyjson_arr_foreach(g_c.pairs, i, imax, pair) {
        const char *addr = g2_pair_address(pair);
        if (!addr || !addr[0]) {
            continue;
        }
        asked++;
        hyp_record_store_query_t q;
        memset(&q, 0, sizeof(q));
        q.anchor = addr;
        hyp_record_set_t *set = NULL;
        hyp_record_store_status_t st = hyp_record_store_query(bare, &q, &set);
        if (st != HYP_RECORD_STORE_OK) {
            hyp_record_set_free(set);
            hyp_record_store_close(bare);
            th_cleanup(dir);
            FAIL("a coherent query was refused instead of answered empty");
        }
        if (set && hyp_record_set_count(set) != 0U) {
            spoke++;
            fprintf(stderr, "\n  a neighbour answered for %s\n", addr);
        }
        hyp_record_set_free(set);

        /* And the store is not merely empty: the file's own prose is there,
         * one address up, which is exactly the neighbour a resolver looking
         * for something to say would reach for. */
        const char *file = g2_pair_file(pair);
        char module[HYP_ADDR_MAX + 1];
        module[0] = '\0';
        if (file && file[0] &&
            hyp_comment_migrate_anchor(&opts, file, module, sizeof(module)) ==
                HYP_COMMENT_MIGRATE_OK) {
            hyp_record_store_query_t p;
            memset(&p, 0, sizeof(p));
            p.anchor_prefix = module;
            hyp_record_set_t *near = NULL;
            if (hyp_record_store_query(bare, &p, &near) == HYP_RECORD_STORE_OK && near &&
                hyp_record_set_count(near) != 0U) {
                neighbours++;
            }
            hyp_record_set_free(near);
        }
    }
    fprintf(stderr,
            "\n  withheld-decision control: %zu anchors asked, %zu answered, %zu of them have "
            "real prose one address up that was not substituted\n",
            asked, spoke, neighbours);
    hyp_record_store_close(bare);
    th_cleanup(dir);
    ASSERT_GT(asked, 0U);
    ASSERT_EQ(spoke, 0U);
    ASSERT_GT(neighbours, 0U);
    PASS();
}

TEST(a_missing_answer_and_a_missing_store_do_not_look_alike) {
    if (!g2_corpus_ensure()) {
        FAIL(g_c.why);
    }
    /* Empty: the store was read, and there is nothing. */
    hyp_record_store_query_t q;
    memset(&q, 0, sizeof(q));
    q.anchor = "hyp1:hyponoia/hyponoia#hyponoia.src.nowhere.no_such_symbol";
    hyp_record_set_t *set = NULL;
    ASSERT_EQ(hyp_record_store_query(g_c.store, &q, &set), HYP_RECORD_STORE_OK);
    ASSERT_NOT_NULL(set);
    ASSERT_EQ(hyp_record_set_count(set), 0U);
    hyp_record_set_free(set);

    /* Absent: a query that contradicts itself is refused, never answered with
     * a plausible empty set. */
    hyp_record_store_query_t bad;
    memset(&bad, 0, sizeof(bad));
    bad.anchor = "hyp1:hyponoia/hyponoia#hyponoia.src.foundation.record";
    bad.unanchored_only = true;
    hyp_record_set_t *none = NULL;
    ASSERT_NEQ(hyp_record_store_query(g_c.store, &bad, &none), HYP_RECORD_STORE_OK);
    hyp_record_set_free(none);

    /* And on the client surface the same distinction is a key that is there or
     * is not. With no store on this machine, records is withheld and a reason
     * is named; the never-recorded control above holds the other half.
     *
     * The directory stays switched across the call, not merely across the
     * server's construction: the tool resolves the memory directory when it
     * answers, so restoring too early would ask the populated store and
     * quietly assert nothing. */
    char empty_dir[HYP_PATH_MAX];
    snprintf(empty_dir, sizeof(empty_dir), "%s/no-memory-here", g_c.cache);
    hyp_setenv("HYP_MEMORY_DIR", empty_dir, 1);
    hyp_mcp_server_t *cold = hyp_mcp_server_new(NULL);
    if (!cold) {
        hyp_setenv("HYP_MEMORY_DIR", g_c.memdir, 1);
        FAIL("no MCP server");
    }
    hyp_mcp_server_set_tool_profile(cold, HYP_MCP_TOOL_PROFILE_ALL);
    hyp_mcp_server_t *warm = g_c.srv;
    g_c.srv = cold;
    g2_reply_t r;
    bool ok = g2_search("record", &r);
    g_c.srv = warm;
    hyp_mcp_server_free(cold);
    hyp_setenv("HYP_MEMORY_DIR", g_c.memdir, 1);
    if (!ok) {
        g2_reply_free(&r);
        FAIL("the cold server produced no reply a client could read");
    }
    bool withheld = r.structured && yyjson_obj_get(r.structured, "records") == NULL;
    bool explained = r.structured && yyjson_obj_get(r.structured, "memory") != NULL;
    g2_reply_free(&r);
    if (!withheld) {
        FAIL("a machine with no store answered with a records key, which reads as 'nothing was "
             "ever recorded'");
    }
    if (!explained) {
        FAIL("a machine with no store withheld the records key and named no reason");
    }
    PASS();
}

TEST(what_the_client_holds_is_what_the_server_said) {
    if (!g2_corpus_ensure()) {
        FAIL(g_c.why);
    }
    /* The two ends of one answer: the structure a client prefers and the text
     * a client falls back to must be the same answer, not two. */
    g2_reply_t r;
    if (!g2_search("append-only", &r)) {
        g2_reply_free(&r);
        FAIL("search_memory produced no reply a client could read");
    }
    yyjson_doc *from_text = r.text ? yyjson_read(r.text, strlen(r.text), 0) : NULL;
    yyjson_val *troot = from_text ? yyjson_doc_get_root(from_text) : NULL;
    bool clean = !r.is_error && r.text && r.structured && troot;
    uint64_t in_text = clean ? yyjson_get_uint(yyjson_obj_get(troot, "matched")) : 0U;
    uint64_t in_structure = clean ? yyjson_get_uint(yyjson_obj_get(r.structured, "matched")) : 0U;
    size_t rows_text = clean ? yyjson_arr_size(yyjson_obj_get(troot, "records")) : 0U;
    size_t rows_structure = clean ? yyjson_arr_size(yyjson_obj_get(r.structured, "records")) : 0U;
    yyjson_doc_free(from_text);
    g2_reply_free(&r);
    if (!clean) {
        FAIL("the reply carried neither a readable text nor a structure");
    }
    if (in_text != in_structure || rows_text != rows_structure) {
        FAIL("the two ends of one answer disagree");
    }
    ASSERT_GT(in_text, 0U);
    PASS();
}

/* ── Suite ──────────────────────────────────────────────────────────────── */

SUITE(g2_retrieval) {
    RUN_TEST(the_corpus_ingests_without_a_gold_being_written);
    /* the lanes */
    RUN_TEST(an_anchor_query_returns_the_decision_recorded_against_that_symbol);
    RUN_TEST(the_module_address_the_product_derives_is_the_one_the_corpus_assumed);
    RUN_TEST(a_module_prefix_returns_the_decision_and_the_comments_about_that_file);
    RUN_TEST(search_memory_answers_an_anchor_and_reports_what_it_resolved_to);
    RUN_TEST(the_published_signature_and_the_handler_are_live_together);
    RUN_TEST(free_text_from_the_symbol_name_finds_the_decision_about_it);
    RUN_TEST(free_text_from_the_question_wording_finds_the_decision);
    /* the controls, without which none of the above is believed */
    RUN_TEST(a_question_whose_answer_was_never_recorded_returns_nothing);
    RUN_TEST(a_plausible_wrong_answer_is_never_returned);
    RUN_TEST(without_the_decision_an_anchor_returns_nothing_rather_than_a_neighbour);
    RUN_TEST(a_missing_answer_and_a_missing_store_do_not_look_alike);
    RUN_TEST(what_the_client_holds_is_what_the_server_said);
    g2_corpus_free();
}
