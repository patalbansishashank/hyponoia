/*
 * test_orphan_corpus.h — the Track G G1 anchor corpus, as C data.
 *
 * GENERATED. Do not hand-edit: every span text, hash and qualified name
 * here comes from this repository's own history via the corpus build, and is
 * re-extracted and re-hashed on the way into this file. The expectations are
 * the corpus's, not this suite's.
 *
 * THE PARSER CAVEAT, and how this file discharges it. The corpus parses
 * spans by brace-matching; the product parses them with tree-sitter. A
 * boundary disagreement would produce a hash mismatch that has nothing to
 * do with anchoring. So the span text is carried VERBATIM and the range is
 * pinned: a harness writes g1_text_t.text into a file and indexes exactly
 * those lines. No parser runs, and the hash the product computes is the
 * hash the corpus recorded, by construction.
 *
 * Line numbers in the after-tree are deliberately NOT carried. Resolution
 * never reads one, so a harness that reproduced them would be asserting
 * something the contract does not use.
 */
#ifndef HYP_TESTS_ORPHAN_CORPUS_H
#define HYP_TESTS_ORPHAN_CORPUS_H

/* One span's content, deduplicated: a copy-paste fixture's copies are
 * byte-identical, and their sharing one entry is the multiplicity being
 * data rather than a claim. */
typedef struct {
    const char *span_hash; /* what the corpus recorded for this text */
    const char *text;      /* verbatim, no trailing newline */
    int lines;
} g1_text_t;

/* One node of the live tree — where the content is NOW. */
typedef struct {
    const char *file_path;
    const char *name;
    const char *qualified_name; /* the corpus's, for the two-ends check */
    const char *address;
    int text_index;
    const char *fixture_id;
} g1_live_t;

/* One replayed lifecycle: the anchor as it was written, and what the
 * corpus says resolving it against the live tree must answer. */
typedef struct {
    const char *id;
    const char *category;
    const char *before_file_path;
    const char *before_name;
    const char *before_qn;
    const char *before_hash;
    const char *anchor; /* before_hash '@' before_address, as recorded */
    const char *expected_status;
    int expected_candidate_count;
    /* Addresses a resolver must never hand back for this anchor. The
     * corpus names the wrong answers; a suite that only checked the status
     * would pass by returning something plausible. NULL-terminated. */
    const char *const *must_not_attach;
    const char *note;
} g1_fixture_t;

/* A dead qualified name from one verified fixture, paired with a content
 * hash that the live tree really does hold at two and at three addresses.
 * NO COMMIT IN THIS HISTORY PRODUCED THIS STATE — two exhaustive searches
 * over the whole repository found zero instances, and the corpus refused to
 * fabricate one. Both operands are corpus facts and the expected count is
 * derived from the live tree rather than typed. */
typedef struct {
    const char *id;
    const char *dead_qn;
    const char *dead_from;   /* the fixture that proves the name is gone */
    const char *hash;
    const char *hash_from;   /* the fixture that supplies the duplication */
    int expected_candidate_count;
    const char *const *expected_addresses; /* sorted, NULL-terminated */
} g1_composed_t;

extern const g1_text_t g1_texts[];
extern const int g1_text_count;
extern const g1_live_t g1_live[];
extern const int g1_live_count;
extern const g1_fixture_t g1_fixtures[];
extern const int g1_fixture_count;
extern const g1_composed_t g1_composed[];
extern const int g1_composed_count;

/* The project the corpus's qualified names were built against. */
#define G1_PROJECT "hyponoia"

#endif /* HYP_TESTS_ORPHAN_CORPUS_H */
