/*
 * record_clock.c — the wall clock, kept OUT of record.c on purpose.
 *
 * Nothing in the record contract reads a clock. A captured timestamp would make
 * a record's id depend on the machine and the moment that built it, so the same
 * event ingested on two machines would get two ids, and a sync that is supposed
 * to be a union would instead double the store. That failure only becomes
 * visible after two machines have both ingested the same feed, which is an
 * expensive place to learn it.
 *
 * So the timestamp is always supplied by the caller, and the clock lives in its
 * own translation unit: tests/test_record_contract.sh asserts that record.c
 * contains no time.h, no time(), no clock_gettime and no GetSystemTime*. The
 * absence is then a property of the build rather than a habit of whoever edits
 * the file next.
 *
 * This exists at all so that "what time is it" has ONE answer on this path. The
 * obvious alternative — every caller reaching for its own clock — is how two
 * call sites end up with two epochs, and note that hyp_now_ms() in platform.h
 * is a MONOTONIC counter from an arbitrary origin, which would be silently
 * wrong here in a way that still produces plausible-looking numbers.
 */
#include "foundation/record.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

int64_t hyp_record_wall_clock_ms(void) {
#ifdef _WIN32
    /* FILETIME is 100-nanosecond ticks since 1601-01-01 UTC. */
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    /* Assembled by hand rather than through ULARGE_INTEGER: the union member
     * written is not the one read, which is correct here and indistinguishable
     * from a mistake to anything reading the code. */
    uint64_t ticks = ((uint64_t)ft.dwHighDateTime << 32) | (uint64_t)ft.dwLowDateTime;
    const uint64_t ticks_1601_to_1970 = 116444736000000000ULL;
    if (ticks < ticks_1601_to_1970) {
        return 0; /* before the Unix epoch: refused by the record validator */
    }
    return (int64_t)((ticks - ticks_1601_to_1970) / 10000ULL);
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        /* Fail closed: 0 is outside the accepted range, so a record built from
         * it is refused rather than stamped with a plausible-looking lie. */
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
#endif
}
