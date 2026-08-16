#include "memory/adr_records.h"

#include "foundation/constants.h"
#include "foundation/mem.h"
#include "foundation/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Days in each month of a year starting in March, the shift that makes the
 * leap day the last day of the cycle and removes every special case from the
 * conversion below. */
enum {
    ADR_DAYS_PER_ERA = 146097,
    ADR_DAYS_PER_4YEARS = 1460,
    ADR_YEARS_PER_ERA = 400,
    ADR_EPOCH_SHIFT_DAYS = 719468,
    ADR_SECONDS_PER_DAY = 86400,
    ADR_SECONDS_PER_HOUR = 3600,
    ADR_SECONDS_PER_MINUTE = 60,
    ADR_MS_PER_SECOND = 1000,
    ADR_MARCH = 3,
    ADR_FEBRUARY = 2
};

/* Days between the Unix epoch and a civil date, proleptic Gregorian. Pure
 * integer arithmetic: no timezone database, no locale, no libc call whose
 * answer depends on the environment. The migration's ids depend on this
 * function, so it has to give the same answer on every machine. */
static int64_t adr_days_from_civil(int64_t year, int64_t month, int64_t day) {
    year -= (month <= ADR_FEBRUARY) ? 1 : 0;
    const int64_t era = (year >= 0 ? year : year - (ADR_YEARS_PER_ERA - 1)) / ADR_YEARS_PER_ERA;
    const int64_t yoe = year - era * ADR_YEARS_PER_ERA;
    const int64_t mp = month + (month > ADR_FEBRUARY ? -ADR_MARCH : (12 - ADR_MARCH));
    const int64_t doy = (153 * mp + 2) / 5 + day - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * ADR_DAYS_PER_ERA + doe - ADR_EPOCH_SHIFT_DAYS;
}

/* Inverse of adr_days_from_civil. */
static void adr_civil_from_days(int64_t days, int64_t *year, int64_t *month, int64_t *day) {
    days += ADR_EPOCH_SHIFT_DAYS;
    const int64_t era =
        (days >= 0 ? days : days - (ADR_DAYS_PER_ERA - 1)) / ADR_DAYS_PER_ERA;
    const int64_t doe = days - era * ADR_DAYS_PER_ERA;
    const int64_t yoe =
        (doe - doe / ADR_DAYS_PER_4YEARS + doe / 36524 - doe / (ADR_DAYS_PER_ERA - 1)) / 365;
    const int64_t y = yoe + era * ADR_YEARS_PER_ERA;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    *day = doy - (153 * mp + 2) / 5 + 1;
    *month = mp + (mp < 10 ? ADR_MARCH : (ADR_MARCH - 12));
    *year = y + ((*month <= ADR_FEBRUARY) ? 1 : 0);
}

static bool adr_digits(const char *s, size_t n, int64_t *out) {
    int64_t value = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        value = value * 10 + (s[i] - '0');
    }
    *out = value;
    return true;
}

bool hyp_adr_timestamp_to_ms(const char *iso, int64_t *out_ms) {
    if (!iso || !out_ms) {
        return false;
    }
    if (strlen(iso) != (size_t)(HYP_ADR_TIMESTAMP_LEN - 1)) {
        return false;
    }
    if (iso[4] != '-' || iso[7] != '-' || iso[10] != 'T' || iso[13] != ':' || iso[16] != ':' ||
        iso[19] != 'Z') {
        return false;
    }
    int64_t year = 0;
    int64_t month = 0;
    int64_t day = 0;
    int64_t hour = 0;
    int64_t minute = 0;
    int64_t second = 0;
    if (!adr_digits(iso, 4, &year) || !adr_digits(iso + 5, 2, &month) ||
        !adr_digits(iso + 8, 2, &day) || !adr_digits(iso + 11, 2, &hour) ||
        !adr_digits(iso + 14, 2, &minute) || !adr_digits(iso + 17, 2, &second)) {
        return false;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 ||
        second > 60) {
        return false;
    }
    /* A leap second lands on the following instant rather than being refused:
     * refusing would drop a real document over one second of nomenclature. */
    if (second > 59) {
        second = 59;
    }
    const int64_t days = adr_days_from_civil(year, month, day);
    const int64_t seconds =
        days * ADR_SECONDS_PER_DAY + hour * ADR_SECONDS_PER_HOUR + minute * ADR_SECONDS_PER_MINUTE +
        second;
    const int64_t ms = seconds * ADR_MS_PER_SECOND;
    if (ms < HYP_RECORD_MIN_TIMESTAMP_MS || ms > HYP_RECORD_MAX_TIMESTAMP_MS) {
        return false;
    }
    *out_ms = ms;
    return true;
}

bool hyp_adr_timestamp_from_ms(int64_t ms, char *buf, size_t bufsz) {
    if (!buf || bufsz < (size_t)HYP_ADR_TIMESTAMP_LEN) {
        return false;
    }
    if (ms < HYP_RECORD_MIN_TIMESTAMP_MS || ms > HYP_RECORD_MAX_TIMESTAMP_MS) {
        return false;
    }
    int64_t seconds = ms / ADR_MS_PER_SECOND;
    int64_t days = seconds / ADR_SECONDS_PER_DAY;
    int64_t rem = seconds - days * ADR_SECONDS_PER_DAY;
    if (rem < 0) {
        days -= 1;
        rem += ADR_SECONDS_PER_DAY;
    }
    int64_t year = 0;
    int64_t month = 0;
    int64_t day = 0;
    adr_civil_from_days(days, &year, &month, &day);
    int written = snprintf(buf, bufsz, "%04lld-%02lld-%02lldT%02lld:%02lld:%02lldZ",
                          (long long)year, (long long)month, (long long)day,
                          (long long)(rem / ADR_SECONDS_PER_HOUR),
                          (long long)((rem % ADR_SECONDS_PER_HOUR) / ADR_SECONDS_PER_MINUTE),
                          (long long)(rem % ADR_SECONDS_PER_MINUTE));
    return written == HYP_ADR_TIMESTAMP_LEN - 1;
}

int64_t hyp_adr_floor_to_second_ms(int64_t ms) {
    int64_t rem = ms % ADR_MS_PER_SECOND;
    if (rem < 0) {
        rem += ADR_MS_PER_SECOND;
    }
    return ms - rem;
}

hyp_record_status_t hyp_adr_record_build(const char *project, const char *content,
                                         int64_t updated_at_ms, const hyp_record_t **out) {
    if (out) {
        *out = NULL;
    }
    if (!project || !project[0] || !content || !content[0] || !out) {
        return HYP_RECORD_ERR_NULL;
    }
    hyp_record_input_t in = {0};
    in.kind = HYP_RECORD_DECISION;
    in.author = HYP_ADR_RECORD_AUTHOR;
    in.timestamp_ms = updated_at_ms;
    in.content = content;
    in.anchor = NULL;  /* about a project, not about a span */
    in.origin = project;
    in.thread = NULL;
    in.parent = NULL;
    in.redactions = 0;
    return hyp_record_build(&in, out);
}

/* Fold one already-read document. Shared by the single-project and
 * whole-store entry points so there is exactly one place that decides what a
 * folded ADR looks like. */
static hyp_record_store_status_t adr_fold_document(hyp_record_store_t *records,
                                                   const char *project, const hyp_adr_t *adr,
                                                   hyp_adr_fold_result_t *out) {
    out->documents++;

    int64_t ms = 0;
    if (!adr->updated_at || !hyp_adr_timestamp_to_ms(adr->updated_at, &ms)) {
        /* A row whose stored instant cannot be read has no honest timestamp,
         * and inventing one would mint an id nobody else can reproduce. Refuse
         * this document and keep going: the row stays exactly where it is. */
        out->refused++;
        return HYP_RECORD_STORE_OK;
    }

    const hyp_record_t *rec = NULL;
    if (hyp_adr_record_build(project, adr->content, ms, &rec) != HYP_RECORD_OK) {
        out->refused++;
        return HYP_RECORD_STORE_OK;
    }

    bool added = false;
    hyp_record_store_status_t st = hyp_record_store_append(records, rec, &added);
    hyp_record_free(rec);
    if (st != HYP_RECORD_STORE_OK) {
        return st;
    }
    if (added) {
        out->added++;
    } else {
        out->present++;
    }
    return HYP_RECORD_STORE_OK;
}

hyp_record_store_status_t hyp_adr_fold_project(hyp_store_t *store, hyp_record_store_t *records,
                                               const char *project, hyp_adr_fold_result_t *out) {
    hyp_adr_fold_result_t local = {0};
    if (out) {
        memset(out, 0, sizeof(*out));
    } else {
        out = &local;
    }
    if (!store || !records || !project || !project[0]) {
        return HYP_RECORD_STORE_ERR_NULL;
    }

    hyp_adr_t adr;
    memset(&adr, 0, sizeof(adr));
    if (hyp_store_adr_get(store, project, &adr) != HYP_STORE_OK) {
        /* No document is not a failure: absent means there is nothing to
         * fold, which is a different answer from a fold that folded nothing. */
        return HYP_RECORD_STORE_OK;
    }
    hyp_record_store_status_t st = adr_fold_document(records, project, &adr, out);
    hyp_store_adr_free(&adr);
    return st;
}

hyp_record_store_status_t hyp_adr_fold_store(hyp_store_t *store, hyp_record_store_t *records,
                                             hyp_adr_fold_result_t *out) {
    hyp_adr_fold_result_t local = {0};
    if (out) {
        memset(out, 0, sizeof(*out));
    } else {
        out = &local;
    }
    if (!store || !records) {
        return HYP_RECORD_STORE_ERR_NULL;
    }

    char **projects = NULL;
    int count = 0;
    if (hyp_store_adr_list_projects(store, &projects, &count) != HYP_STORE_OK) {
        return HYP_RECORD_STORE_ERR_IO;
    }

    hyp_record_store_status_t st = HYP_RECORD_STORE_OK;
    for (int i = 0; i < count && st == HYP_RECORD_STORE_OK; i++) {
        hyp_adr_fold_result_t one = {0};
        st = hyp_adr_fold_project(store, records, projects[i], &one);
        out->documents += one.documents;
        out->added += one.added;
        out->present += one.present;
        out->refused += one.refused;
    }
    hyp_store_adr_free_project_list(projects, count);
    return st;
}

hyp_record_store_status_t hyp_adr_records_open(const char *dir, hyp_record_store_t **out) {
    if (!out) {
        return HYP_RECORD_STORE_ERR_NULL;
    }
    *out = NULL;
    if (dir && dir[0]) {
        return hyp_record_store_open(dir, out);
    }

    const char *cache = hyp_resolve_cache_dir();
    if (!cache || !cache[0]) {
        return HYP_RECORD_STORE_ERR_OPEN;
    }
    char path[HYP_SZ_1K];
    int written = snprintf(path, sizeof(path), "%s/memory", cache);
    if (written <= 0 || written >= (int)sizeof(path)) {
        return HYP_RECORD_STORE_ERR_OPEN;
    }
    return hyp_record_store_open(path, out);
}

int hyp_adr_write(hyp_store_t *store, hyp_record_store_t *records, const char *project,
                  const char *content, int64_t event_ms) {
    if (!store || !records || !project || !project[0] || !content || !content[0]) {
        return HYP_STORE_ERR;
    }

    /* Fold what the row already holds BEFORE superseding it. Without this
     * step the text about to be replaced would be the last version the mutable
     * store ever destroyed, on the very write that retires the mutable store. */
    hyp_adr_fold_result_t folded = {0};
    if (hyp_adr_fold_project(store, records, project, &folded) != HYP_RECORD_STORE_OK) {
        return HYP_STORE_ERR;
    }

    const int64_t ms = hyp_adr_floor_to_second_ms(event_ms);
    char stamp[HYP_ADR_TIMESTAMP_LEN];
    if (!hyp_adr_timestamp_from_ms(ms, stamp, sizeof(stamp))) {
        return HYP_STORE_ERR;
    }

    const hyp_record_t *rec = NULL;
    if (hyp_adr_record_build(project, content, ms, &rec) != HYP_RECORD_OK) {
        return HYP_STORE_ERR;
    }
    hyp_record_store_status_t st = hyp_record_store_append(records, rec, NULL);
    hyp_record_free(rec);
    if (st != HYP_RECORD_STORE_OK) {
        /* The append is the write. A projection refreshed past a failed append
         * would be a document with no record behind it, which is the mutable
         * store wearing a new name. */
        return HYP_STORE_ERR;
    }

    return hyp_store_adr_store_at(store, project, content, stamp);
}

int hyp_adr_write_via_records(hyp_store_t *store, const char *project, const char *content) {
    hyp_record_store_t *records = NULL;
    if (hyp_adr_records_open(NULL, &records) != HYP_RECORD_STORE_OK) {
        return HYP_STORE_ERR;
    }
    /* One clock reading, passed in. The record contract reads no clock itself,
     * so that there is exactly one answer to what time it is on this path. */
    int rc = hyp_adr_write(store, records, project, content, hyp_record_wall_clock_ms());
    hyp_record_store_close(records);
    return rc;
}
