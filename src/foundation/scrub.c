/*
 * scrub.c — deterministic secret scrub over transcript text. See scrub.h for
 * the contract; the short version is that this must be a pure function of its
 * input and the table below, because its output is what a record id binds.
 */
#include "foundation/scrub.h"

#include <stdlib.h>
#include <string.h>

/*
 * THE vendor prefix table — the single source the config guard (cli.c), this
 * scrub, and the pre-push scanner all read. scripts/check-secret-history.sh
 * parses the definition below textually — it greps this file for the array
 * name followed by its initializer and reads the string literals out of that
 * ONE line — so keep the whole initializer on one line, and if the definition
 * moves or is renamed, point the script at the new location. It refuses to
 * invent a list of its own, and so must you.
 */
const char *const hyp_vendor_prefixes[] = {"sk-", "pa-", "jina_", "AIza", "sk_", "voy-"};
const size_t hyp_vendor_prefix_count = sizeof(hyp_vendor_prefixes) / sizeof(hyp_vendor_prefixes[0]);

/* The replacement marker, as one definition rather than a literal and a length
 * that must be kept in step. The span becomes MARKER_OPEN + prefix + ']'. */
#define SCRUB_MARKER_OPEN "[REDACTED:"
#define SCRUB_MARKER_OPEN_LEN (sizeof(SCRUB_MARKER_OPEN) - 1)
#define SCRUB_MARKER_CLOSE ']'

/* The single-allocation argument below depends on the marker never being longer
 * than the span it replaces: the marker is SCRUB_MARKER_OPEN_LEN + 1 + prefix
 * bytes, the span is HYP_SCRUB_MIN_BODY + prefix bytes. Lowering the threshold
 * far enough would turn that into a heap overflow, silently, in a constant
 * documented as tunable — so the compiler holds the invariant, not a comment. */
_Static_assert(HYP_SCRUB_MIN_BODY >= SCRUB_MARKER_OPEN_LEN + 1,
               "HYP_SCRUB_MIN_BODY must leave room for the marker; below this the scrub can "
               "grow the text past its one allocation");

/* Explicit byte classes, never <ctype.h> — ctype is locale-sensitive, and a
 * scrub whose output depends on the machine's locale produces two different
 * record ids from one transcript. */
static bool scrub_is_word_byte(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

static bool scrub_is_body_byte(char c) {
    return scrub_is_word_byte(c) || c == '-';
}

/* Length of the maximal run of key-body bytes starting at p. */
static size_t scrub_body_run(const char *p) {
    size_t n = 0;
    while (p[n] != '\0' && scrub_is_body_byte(p[n])) {
        n++;
    }
    return n;
}

bool hyp_scrub_text(const char *text, char **out_text, uint32_t *out_redactions) {
    if (!text || !out_text || !out_redactions) {
        return false;
    }

    size_t len = strlen(text);

    /* One allocation suffices: a marker is SCRUB_MARKER_OPEN_LEN + 1 + prefix
     * bytes, the span it replaces is at least HYP_SCRUB_MIN_BODY + prefix
     * bytes, and the _Static_assert above holds the first under the second, so
     * the output can never be longer than the input. */
    char *out = malloc(len + 1);
    if (!out) {
        return false;
    }

    size_t o = 0;
    uint32_t count = 0;
    size_t i = 0;
    while (i < len) {
        const char *hit = NULL;
        size_t hit_len = 0;
        size_t hit_run = 0;

        /* A key never starts mid-word: "compa-..." is a word, not a paste. */
        if (i == 0 || !scrub_is_word_byte(text[i - 1])) {
            for (size_t k = 0; k < hyp_vendor_prefix_count; k++) {
                const char *pfx = hyp_vendor_prefixes[k];
                size_t pfx_len = strlen(pfx);
                if (strncmp(text + i, pfx, pfx_len) != 0) {
                    continue;
                }
                size_t run = scrub_body_run(text + i + pfx_len);
                if (run < HYP_SCRUB_MIN_BODY) {
                    continue;
                }
                /* Longest qualifying prefix wins — one deterministic answer
                 * even if the table ever holds nested prefixes. */
                if (!hit || pfx_len > hit_len) {
                    hit = pfx;
                    hit_len = pfx_len;
                    hit_run = run;
                }
            }
        }

        if (hit) {
            memcpy(out + o, SCRUB_MARKER_OPEN, SCRUB_MARKER_OPEN_LEN);
            o += SCRUB_MARKER_OPEN_LEN;
            memcpy(out + o, hit, hit_len);
            o += hit_len;
            out[o++] = SCRUB_MARKER_CLOSE;
            count++;
            i += hit_len + hit_run;
        } else {
            out[o++] = text[i++];
        }
    }
    out[o] = '\0';

    *out_text = out;
    *out_redactions = count;
    return true;
}

hyp_record_status_t hyp_record_ingest_scrubbed(const hyp_record_ingest_input_t *in,
                                               hyp_scrub_fn scrub, const hyp_record_t **out) {
    if (out) {
        *out = NULL;
    }
    /* No scrubber wired means no record, full stop — the refusal D1 asserts. */
    if (!in || !scrub || !out) {
        return HYP_RECORD_ERR_NULL;
    }
    if (!in->content) {
        /* build would refuse this too, but the scrubber needs text to exist
         * before it can run, so the refusal happens here with the same name. */
        return HYP_RECORD_ERR_CONTENT;
    }

    char *clean = NULL;
    uint32_t redactions = 0;
    if (!scrub(in->content, &clean, &redactions)) {
        return HYP_RECORD_ERR_ALLOC;
    }

    hyp_record_input_t rin;
    memset(&rin, 0, sizeof(rin));
    rin.kind = in->kind;
    rin.author = in->author;
    rin.timestamp_ms = in->timestamp_ms;
    rin.content = clean;
    rin.anchor = in->anchor;
    rin.origin = in->origin;
    rin.thread = in->thread;
    rin.parent = in->parent;
    rin.redactions = redactions;

    hyp_record_status_t st = hyp_record_build(&rin, out);
    free(clean);
    return st;
}
