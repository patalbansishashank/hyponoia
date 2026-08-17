/*
 * record.c — the append-only record. See record.h for the contract: why the id
 * is a digest over every field, why merging is a union, and why the timestamp
 * is supplied rather than captured.
 *
 * TWO PROPERTIES OF THIS FILE ARE ENFORCED BY tests/test_record_contract.sh:
 *
 *   1. It reads NO CLOCK. There is no <time.h>, no time(), no clock_gettime().
 *      A captured timestamp would make an id depend on the machine that built
 *      it, which turns sync into a duplicate storm — a failure that only shows
 *      up after two machines have both ingested the same feed, which is far too
 *      late to learn it. The wall clock lives in record_clock.c, a separate
 *      translation unit, so the absence is structural rather than a habit.
 *   2. It PARSES NOTHING. No strchr, strstr, strtok, sscanf or strpbrk. anchor,
 *      origin and thread are opaque byte strings owned by whoever wrote them;
 *      the moment this file looked inside one it would inherit that producer's
 *      vocabulary and the next adapter would have to translate into it. The
 *      encoding is length-prefixed precisely so nothing here needs to.
 */
#include "foundation/record.h"

#include "foundation/constants.h" /* HYP_ALLOC_ONE */
#include "foundation/sha256.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* ── Kinds and statuses ─────────────────────────────────────────────────── */

/*
 * Both of these are switches with no default on purpose. Adding a kind or a
 * status without naming it here is then a -Wswitch error under -Werror, so the
 * enumeration cannot drift from the thing it enumerates. A lookup table indexed
 * by the enum value would have compiled fine and returned garbage.
 */

const char *hyp_record_kind_name(hyp_record_kind_t kind) {
    switch (kind) {
    case HYP_RECORD_DECISION:
        return "decision";
    case HYP_RECORD_VERDICT:
        return "verdict";
    case HYP_RECORD_SUMMARY:
        return "summary";
    case HYP_RECORD_SIGNAL:
        return "signal";
    case HYP_RECORD_MESSAGE:
        return "message";
    case HYP_RECORD_KIND_COUNT:
        break;
    }
    return NULL;
}

bool hyp_record_kind_from_name(const char *name, hyp_record_kind_t *out) {
    if (!name || !out) {
        return false;
    }
    /* Derived from the switch above rather than a second list of spellings. */
    for (int i = 0; i < (int)HYP_RECORD_KIND_COUNT; i++) {
        const char *candidate = hyp_record_kind_name((hyp_record_kind_t)i);
        if (candidate && strcmp(candidate, name) == 0) {
            *out = (hyp_record_kind_t)i;
            return true;
        }
    }
    return false;
}

const char *hyp_record_status_reason(hyp_record_status_t status) {
    switch (status) {
    case HYP_RECORD_OK:
        return "ok";
    case HYP_RECORD_ERR_NULL:
        return "a required argument was NULL";
    case HYP_RECORD_ERR_KIND:
        return "kind is not one of the five record kinds";
    case HYP_RECORD_ERR_AUTHOR:
        return "author must be present, non-empty and within the length cap";
    case HYP_RECORD_ERR_TIMESTAMP:
        return "timestamp must be supplied in UTC milliseconds since the Unix epoch";
    case HYP_RECORD_ERR_CONTENT:
        return "content must be present, non-empty and within the length cap";
    case HYP_RECORD_ERR_ANCHOR:
        return "anchor must be absent or a non-empty string within the length cap";
    case HYP_RECORD_ERR_ORIGIN:
        return "origin must be absent or a non-empty string within the length cap";
    case HYP_RECORD_ERR_THREAD:
        return "thread must be absent or a non-empty string within the length cap";
    case HYP_RECORD_ERR_PARENT:
        return "parent must be absent or the id of another record";
    case HYP_RECORD_ERR_TAMPERED:
        return "the record's id does not match its fields — it was altered after it was built";
    case HYP_RECORD_ERR_COLLISION:
        return "two different records claim the same id — refused, never merged";
    case HYP_RECORD_ERR_ALLOC:
        return "out of memory";
    }
    return "unknown record status";
}

/* ── The one field list ─────────────────────────────────────────────────── */

/*
 * Every field of a record appears here EXACTLY ONCE, with its offset in both
 * the input struct and the finished record. Three things are driven from this
 * table — the id preimage, the projection of a record back to an input, and
 * field equality — so a field cannot be in the digest but missing from the
 * comparison, or vice versa. Three hand-written lists in their place would
 * let one drift from the others.
 *
 * The preimage's field names come from the member names via the preprocessor,
 * so renaming a member renames it in the digest too. That is deliberate: it
 * makes a rename a visible, total change of every id rather than a silent one.
 */

typedef enum { RF_KIND, RF_STR, RF_I64, RF_U32 } rf_type_t;

typedef struct {
    const char *name;
    rf_type_t type;
    size_t in_off;  /* offset within hyp_record_input_t */
    size_t rec_off; /* offset within hyp_record_t */
} rf_desc_t;

#define RF_FIELD(member, kind_tag) \
    {#member, (kind_tag), offsetof(hyp_record_input_t, member), offsetof(hyp_record_t, member)}

static const rf_desc_t RECORD_FIELDS[] = {
    RF_FIELD(kind, RF_KIND),   RF_FIELD(author, RF_STR), RF_FIELD(timestamp_ms, RF_I64),
    RF_FIELD(content, RF_STR), RF_FIELD(anchor, RF_STR), RF_FIELD(origin, RF_STR),
    RF_FIELD(thread, RF_STR),  RF_FIELD(parent, RF_STR), RF_FIELD(redactions, RF_U32),
};

#define RECORD_FIELD_COUNT (sizeof(RECORD_FIELDS) / sizeof(RECORD_FIELDS[0]))

/* ── Canonical encoding ─────────────────────────────────────────────────── */

/*
 * Length-prefixed and self-describing. Every value is preceded by a tag byte
 * and, for strings, by its byte length, so no value can impersonate a
 * separator and no field can bleed into its neighbour. 'N' (absent) and a
 * zero-length 'S' (present but empty) are different bytes, because absent and
 * empty are different answers even where the validator currently refuses one of
 * them.
 */

static void enc_u64(hyp_sha256_ctx *ctx, uint64_t value) {
    uint8_t bytes[8];
    for (int i = 7; i >= 0; i--) {
        bytes[i] = (uint8_t)(value & 0xFFu);
        value >>= 8;
    }
    hyp_sha256_update(ctx, bytes, sizeof(bytes));
}

static void enc_str(hyp_sha256_ctx *ctx, const char *s) {
    if (!s) {
        hyp_sha256_update(ctx, "N", 1);
        return;
    }
    size_t len = strlen(s);
    hyp_sha256_update(ctx, "S", 1);
    enc_u64(ctx, (uint64_t)len);
    hyp_sha256_update(ctx, s, len);
}

static void hex_of(const uint8_t *digest, char out[HYP_RECORD_ID_LEN + 1]) {
    static const char hexdigits[] = "0123456789abcdef";
    for (size_t i = 0; i < HYP_SHA256_DIGEST_LEN; i++) {
        out[i * 2] = hexdigits[digest[i] >> 4];
        out[i * 2 + 1] = hexdigits[digest[i] & 0x0Fu];
    }
    out[HYP_RECORD_ID_LEN] = '\0';
}

static void record_digest(const hyp_record_input_t *in, char out[HYP_RECORD_ID_LEN + 1]) {
    hyp_sha256_ctx ctx;
    hyp_sha256_init(&ctx);

    /* Domain tag first: a future record shape is a disjoint id space. */
    enc_str(&ctx, HYP_RECORD_CONTRACT);

    /* Byte-wise base pointers: field offsets are byte offsets, and a char*
     * base would make every memcpy below the same pointer type as its size. */
    const unsigned char *base = (const unsigned char *)in;
    for (size_t i = 0; i < RECORD_FIELD_COUNT; i++) {
        const rf_desc_t *f = &RECORD_FIELDS[i];
        enc_str(&ctx, f->name);
        switch (f->type) {
        case RF_KIND: {
            hyp_record_kind_t kind;
            memcpy(&kind, base + f->in_off, sizeof(kind));
            /* By NAME, never by ordinal — reordering the enum must not silently
             * rewrite every id in every store. */
            enc_str(&ctx, hyp_record_kind_name(kind));
            break;
        }
        case RF_STR: {
            const char *value;
            memcpy(&value, base + f->in_off, sizeof(value));
            enc_str(&ctx, value);
            break;
        }
        case RF_I64: {
            int64_t value;
            memcpy(&value, base + f->in_off, sizeof(value));
            hyp_sha256_update(&ctx, "I", 1);
            enc_u64(&ctx, (uint64_t)value);
            break;
        }
        case RF_U32: {
            uint32_t value;
            memcpy(&value, base + f->in_off, sizeof(value));
            hyp_sha256_update(&ctx, "U", 1);
            enc_u64(&ctx, (uint64_t)value);
            break;
        }
        }
    }

    uint8_t digest[HYP_SHA256_DIGEST_LEN];
    hyp_sha256_final(&ctx, digest);
    hex_of(digest, out);
}

/* ── Validation ─────────────────────────────────────────────────────────── */

static hyp_record_status_t check_required(const char *s, size_t cap, hyp_record_status_t on_bad) {
    if (!s || s[0] == '\0' || strlen(s) > cap) {
        return on_bad;
    }
    return HYP_RECORD_OK;
}

static hyp_record_status_t check_optional(const char *s, size_t cap, hyp_record_status_t on_bad) {
    if (!s) {
        return HYP_RECORD_OK; /* absent: "there is nothing to look at here" */
    }
    if (s[0] == '\0' || strlen(s) > cap) {
        return on_bad; /* present-but-empty is what a half-filled struct produces */
    }
    return HYP_RECORD_OK;
}

bool hyp_record_id_is_valid(const char *s) {
    if (!s) {
        return false;
    }
    size_t i = 0;
    for (; i < (size_t)HYP_RECORD_ID_LEN; i++) {
        char c = s[i];
        bool is_digit = (c >= '0' && c <= '9');
        bool is_lower_hex = (c >= 'a' && c <= 'f');
        if (!is_digit && !is_lower_hex) {
            return false;
        }
    }
    return s[i] == '\0';
}

static hyp_record_status_t record_validate(const hyp_record_input_t *in) {
    if (!in) {
        return HYP_RECORD_ERR_NULL;
    }
    if (!hyp_record_kind_name(in->kind)) {
        return HYP_RECORD_ERR_KIND;
    }

    hyp_record_status_t st =
        check_required(in->author, HYP_RECORD_MAX_AUTHOR, HYP_RECORD_ERR_AUTHOR);
    if (st != HYP_RECORD_OK) {
        return st;
    }
    if (in->timestamp_ms < HYP_RECORD_MIN_TIMESTAMP_MS ||
        in->timestamp_ms > HYP_RECORD_MAX_TIMESTAMP_MS) {
        return HYP_RECORD_ERR_TIMESTAMP;
    }
    st = check_required(in->content, HYP_RECORD_MAX_CONTENT, HYP_RECORD_ERR_CONTENT);
    if (st != HYP_RECORD_OK) {
        return st;
    }
    st = check_optional(in->anchor, HYP_RECORD_MAX_ANCHOR, HYP_RECORD_ERR_ANCHOR);
    if (st != HYP_RECORD_OK) {
        return st;
    }
    st = check_optional(in->origin, HYP_RECORD_MAX_ORIGIN, HYP_RECORD_ERR_ORIGIN);
    if (st != HYP_RECORD_OK) {
        return st;
    }
    st = check_optional(in->thread, HYP_RECORD_MAX_THREAD, HYP_RECORD_ERR_THREAD);
    if (st != HYP_RECORD_OK) {
        return st;
    }
    /* parent is OURS, so it is checked rather than trusted: an identifier from
     * somewhere else in this field is a confident error now instead of a
     * reference that dangles forever. */
    if (in->parent && !hyp_record_id_is_valid(in->parent)) {
        return HYP_RECORD_ERR_PARENT;
    }
    return HYP_RECORD_OK;
}

/* ── Build, free, verify ────────────────────────────────────────────────── */

hyp_record_status_t hyp_record_derive_id(const hyp_record_input_t *in,
                                         char out[HYP_RECORD_ID_LEN + 1]) {
    if (!out) {
        return HYP_RECORD_ERR_NULL;
    }
    out[0] = '\0';
    hyp_record_status_t st = record_validate(in);
    if (st != HYP_RECORD_OK) {
        return st;
    }
    record_digest(in, out);
    return HYP_RECORD_OK;
}

static size_t tail_size(const char *s) {
    return s ? strlen(s) + 1 : 0;
}

static const char *tail_copy(char **tail, const char *s) {
    if (!s) {
        return NULL;
    }
    size_t n = strlen(s) + 1;
    char *dst = *tail;
    memcpy(dst, s, n);
    *tail += n;
    return dst;
}

hyp_record_status_t hyp_record_build(const hyp_record_input_t *in, const hyp_record_t **out) {
    if (!out) {
        return HYP_RECORD_ERR_NULL;
    }
    *out = NULL;
    hyp_record_status_t st = record_validate(in);
    if (st != HYP_RECORD_OK) {
        return st;
    }

    char id[HYP_RECORD_ID_LEN + 1];
    record_digest(in, id);

    size_t bytes = sizeof(hyp_record_t) + sizeof(id) + tail_size(in->author) +
                   tail_size(in->content) + tail_size(in->anchor) + tail_size(in->origin) +
                   tail_size(in->thread) + tail_size(in->parent);

    /* One allocation for the struct and every string it owns: one malloc, one
     * free, and no way to free half a record. */
    void *block = calloc(HYP_ALLOC_ONE, bytes);
    if (!block) {
        return HYP_RECORD_ERR_ALLOC;
    }
    char *tail = (char *)block + sizeof(hyp_record_t);

    /* The members are const, so the record cannot be assembled field by field.
     * It is INITIALISED, once, and the finished value is copied into the
     * allocation — which is the whole point: there is no moment at which a
     * half-built mutable record exists for anything to hold on to. */
    const hyp_record_t finished = {.id = tail_copy(&tail, id),
                                   .kind = in->kind,
                                   .author = tail_copy(&tail, in->author),
                                   .timestamp_ms = in->timestamp_ms,
                                   .content = tail_copy(&tail, in->content),
                                   .anchor = tail_copy(&tail, in->anchor),
                                   .origin = tail_copy(&tail, in->origin),
                                   .thread = tail_copy(&tail, in->thread),
                                   .parent = tail_copy(&tail, in->parent),
                                   .redactions = in->redactions};
    memcpy(block, &finished, sizeof(finished));

    *out = (const hyp_record_t *)block;
    return HYP_RECORD_OK;
}

void hyp_record_free(const hyp_record_t *rec) {
    if (!rec) {
        return;
    }
    /* The pointer is copied rather than cast so that dropping the const —
     * unavoidable, since free() takes void* — does not read as a licence to
     * write through it anywhere else in this file. */
    void *block;
    memcpy(&block, &rec, sizeof(block));
    free(block);
}

/* Project a finished record back to the input that would rebuild it. Driven by
 * the same field table, so verification exercises the identical encoding path
 * that construction used — there is one end here, not two that could agree to
 * be wrong together. */
static void record_to_input(const hyp_record_t *rec, hyp_record_input_t *out) {
    memset(out, 0, sizeof(*out));
    const unsigned char *src = (const unsigned char *)rec;
    unsigned char *dst = (unsigned char *)out;
    for (size_t i = 0; i < RECORD_FIELD_COUNT; i++) {
        const rf_desc_t *f = &RECORD_FIELDS[i];
        /* Read into a typed local and write it back out: copying byte-to-byte
         * between two offset expressions makes the size look like the size of a
         * pointer rather than of the value, which is a real bug most of the time
         * and reads like one here. */
        switch (f->type) {
        case RF_KIND: {
            hyp_record_kind_t value;
            memcpy(&value, src + f->rec_off, sizeof(value));
            memcpy(dst + f->in_off, &value, sizeof(value));
            break;
        }
        case RF_STR: {
            const char *value;
            memcpy(&value, src + f->rec_off, sizeof(value));
            memcpy(dst + f->in_off, &value, sizeof(value));
            break;
        }
        case RF_I64: {
            int64_t value;
            memcpy(&value, src + f->rec_off, sizeof(value));
            memcpy(dst + f->in_off, &value, sizeof(value));
            break;
        }
        case RF_U32: {
            uint32_t value;
            memcpy(&value, src + f->rec_off, sizeof(value));
            memcpy(dst + f->in_off, &value, sizeof(value));
            break;
        }
        }
    }
}

bool hyp_record_verify(const hyp_record_t *rec) {
    if (!rec || !rec->id) {
        return false;
    }
    hyp_record_input_t in;
    record_to_input(rec, &in);
    if (record_validate(&in) != HYP_RECORD_OK) {
        return false;
    }
    char id[HYP_RECORD_ID_LEN + 1];
    record_digest(&in, id);
    return strcmp(id, rec->id) == 0;
}

static bool str_equal(const char *a, const char *b) {
    if (a == b) {
        return true;
    }
    if (!a || !b) {
        return false; /* absent and empty are not the same answer */
    }
    return strcmp(a, b) == 0;
}

bool hyp_record_equal(const hyp_record_t *a, const hyp_record_t *b) {
    if (a == b) {
        return true;
    }
    if (!a || !b) {
        return false;
    }
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    for (size_t i = 0; i < RECORD_FIELD_COUNT; i++) {
        const rf_desc_t *f = &RECORD_FIELDS[i];
        switch (f->type) {
        case RF_KIND: {
            hyp_record_kind_t ka, kb;
            memcpy(&ka, pa + f->rec_off, sizeof(ka));
            memcpy(&kb, pb + f->rec_off, sizeof(kb));
            if (ka != kb) {
                return false;
            }
            break;
        }
        case RF_STR: {
            const char *sa, *sb;
            memcpy(&sa, pa + f->rec_off, sizeof(sa));
            memcpy(&sb, pb + f->rec_off, sizeof(sb));
            if (!str_equal(sa, sb)) {
                return false;
            }
            break;
        }
        case RF_I64: {
            int64_t va, vb;
            memcpy(&va, pa + f->rec_off, sizeof(va));
            memcpy(&vb, pb + f->rec_off, sizeof(vb));
            if (va != vb) {
                return false;
            }
            break;
        }
        case RF_U32: {
            uint32_t va, vb;
            memcpy(&va, pa + f->rec_off, sizeof(va));
            memcpy(&vb, pb + f->rec_off, sizeof(vb));
            if (va != vb) {
                return false;
            }
            break;
        }
        }
    }
    return true;
}

/* ── The set, and the union ─────────────────────────────────────────────── */

struct hyp_record_set {
    const hyp_record_t **items; /* sorted ascending by id — the canonical order */
    size_t count;
    size_t cap;
};

hyp_record_set_t *hyp_record_set_create(void) {
    return (hyp_record_set_t *)calloc(HYP_ALLOC_ONE, sizeof(hyp_record_set_t));
}

void hyp_record_set_free(hyp_record_set_t *set) {
    if (!set) {
        return;
    }
    for (size_t i = 0; i < set->count; i++) {
        hyp_record_free(set->items[i]);
    }
    free(set->items);
    free(set);
}

/* Binary search. On a miss, *pos is where the id would be inserted, which is
 * what keeps the array sorted and therefore keeps enumeration canonical. */
static bool set_find(const hyp_record_set_t *set, const char *id, size_t *pos) {
    size_t lo = 0;
    size_t hi = set->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = strcmp(set->items[mid]->id, id);
        if (cmp == 0) {
            *pos = mid;
            return true;
        }
        if (cmp < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    *pos = lo;
    return false;
}

static bool set_reserve(hyp_record_set_t *set, size_t needed) {
    if (needed <= set->cap) {
        return true;
    }
    size_t cap = set->cap ? set->cap : 8;
    while (cap < needed) {
        cap *= 2;
    }
    const hyp_record_t **grown =
        (const hyp_record_t **)realloc((void *)set->items, cap * sizeof(*grown));
    if (!grown) {
        return false;
    }
    set->items = grown;
    set->cap = cap;
    return true;
}

/* Capacity must already be reserved: an insert that cannot fail is what lets
 * merge be atomic. */
static void set_insert_at(hyp_record_set_t *set, size_t pos, const hyp_record_t *rec) {
    memmove(&set->items[pos + 1], &set->items[pos], (set->count - pos) * sizeof(*set->items));
    set->items[pos] = rec;
    set->count++;
}

/* Copy a record into storage this set owns, rebuilding it from its own fields.
 * The rebuild IS the verification: a record whose id does not match what its
 * fields hash to cannot get in. */
static hyp_record_status_t set_copy_verified(const hyp_record_t *rec, const hyp_record_t **out) {
    *out = NULL;
    if (!rec || !rec->id) {
        return HYP_RECORD_ERR_NULL;
    }
    hyp_record_input_t in;
    record_to_input(rec, &in);
    hyp_record_status_t st = hyp_record_build(&in, out);
    if (st != HYP_RECORD_OK) {
        return st;
    }
    if (strcmp((*out)->id, rec->id) != 0) {
        hyp_record_free(*out);
        *out = NULL;
        return HYP_RECORD_ERR_TAMPERED;
    }
    return HYP_RECORD_OK;
}

hyp_record_status_t hyp_record_set_add(hyp_record_set_t *set, const hyp_record_t *rec,
                                       bool *out_added) {
    if (out_added) {
        *out_added = false;
    }
    if (!set || !rec) {
        return HYP_RECORD_ERR_NULL;
    }

    const hyp_record_t *copy = NULL;
    hyp_record_status_t st = set_copy_verified(rec, &copy);
    if (st != HYP_RECORD_OK) {
        return st;
    }

    size_t pos = 0;
    if (set_find(set, copy->id, &pos)) {
        /* Same id. Either the same record — the union absorbing a duplicate,
         * which is the normal case and not an error — or two different records
         * claiming one id, which is refused. Refusing is not resolving: there is
         * no rule here for picking a winner, deliberately. */
        bool identical = hyp_record_equal(set->items[pos], copy);
        hyp_record_free(copy);
        return identical ? HYP_RECORD_OK : HYP_RECORD_ERR_COLLISION;
    }

    if (!set_reserve(set, set->count + 1)) {
        hyp_record_free(copy);
        return HYP_RECORD_ERR_ALLOC;
    }
    set_insert_at(set, pos, copy);
    if (out_added) {
        *out_added = true;
    }
    return HYP_RECORD_OK;
}

hyp_record_status_t hyp_record_set_merge(hyp_record_set_t *dst, const hyp_record_set_t *src) {
    if (!dst || !src) {
        return HYP_RECORD_ERR_NULL;
    }
    if (src->count == 0) {
        return HYP_RECORD_OK;
    }

    /* Two passes so the merge is atomic: everything is verified and copied
     * before anything is inserted. A merge that gave up halfway would leave a
     * store whose contents depend on where the error happened, and "what is in
     * my store" would stop being a function of what was written to it.
     *
     * merge(A, A) needs no special case: every id is already present and equal,
     * so nothing is pending and dst is never touched while src is being read. */
    const hyp_record_t **pending = (const hyp_record_t **)calloc(src->count, sizeof(*pending));
    if (!pending) {
        return HYP_RECORD_ERR_ALLOC;
    }

    size_t pending_count = 0;
    hyp_record_status_t st = HYP_RECORD_OK;
    for (size_t i = 0; i < src->count; i++) {
        const hyp_record_t *copy = NULL;
        st = set_copy_verified(src->items[i], &copy);
        if (st != HYP_RECORD_OK) {
            break;
        }
        size_t pos = 0;
        if (set_find(dst, copy->id, &pos)) {
            bool identical = hyp_record_equal(dst->items[pos], copy);
            hyp_record_free(copy);
            if (!identical) {
                st = HYP_RECORD_ERR_COLLISION;
                break;
            }
            continue;
        }
        pending[pending_count++] = copy;
    }

    if (st == HYP_RECORD_OK && !set_reserve(dst, dst->count + pending_count)) {
        st = HYP_RECORD_ERR_ALLOC;
    }
    if (st != HYP_RECORD_OK) {
        for (size_t i = 0; i < pending_count; i++) {
            hyp_record_free(pending[i]);
        }
        free((void *)pending);
        return st;
    }

    for (size_t i = 0; i < pending_count; i++) {
        size_t pos = 0;
        (void)set_find(dst, pending[i]->id, &pos);
        set_insert_at(dst, pos, pending[i]);
    }
    free((void *)pending);
    return HYP_RECORD_OK;
}

size_t hyp_record_set_count(const hyp_record_set_t *set) {
    return set ? set->count : 0;
}

const hyp_record_t *hyp_record_set_get(const hyp_record_set_t *set, const char *id) {
    if (!set || !id) {
        return NULL;
    }
    size_t pos = 0;
    return set_find(set, id, &pos) ? set->items[pos] : NULL;
}

const hyp_record_t *hyp_record_set_at(const hyp_record_set_t *set, size_t index) {
    if (!set || index >= set->count) {
        return NULL;
    }
    return set->items[index];
}

/* Its own domain tag: a set digest and a record id must never be confusable,
 * and a one-record set must not digest to that record's id. */
#define HYP_RECORD_SET_CONTRACT "hyp-record-set-v1"

void hyp_record_set_digest(const hyp_record_set_t *set, char out[HYP_RECORD_ID_LEN + 1]) {
    if (!out) {
        return;
    }
    hyp_sha256_ctx ctx;
    hyp_sha256_init(&ctx);
    enc_str(&ctx, HYP_RECORD_SET_CONTRACT);
    size_t count = set ? set->count : 0;
    enc_u64(&ctx, (uint64_t)count);
    for (size_t i = 0; i < count; i++) {
        enc_str(&ctx, set->items[i]->id);
    }
    uint8_t digest[HYP_SHA256_DIGEST_LEN];
    hyp_sha256_final(&ctx, digest);
    hex_of(digest, out);
}
