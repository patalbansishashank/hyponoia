/*
 * profile.c — Activatable profiling implementation.
 */
#include "foundation/profile.h"
#include "foundation/log.h"
#include "foundation/compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

enum {
    PROF_BUF_LEN = 32,
    PROF_NS_PER_US = 1000,
    PROF_US_PER_MS = 1000,
    PROF_US_PER_SEC = 1000000,
};
#define PROF_US_PER_SEC_D 1000000.0

bool hyp_profile_active = false;

void hyp_profile_init(void) {
    const char *env = getenv("HYP_PROFILE");
    if (env && env[0] != '\0' && env[0] != '0') {
        hyp_profile_active = true;
    }
}

void hyp_profile_enable(void) {
    hyp_profile_active = true;
}

void hyp_profile_now(struct timespec *ts) {
    hyp_clock_gettime(CLOCK_MONOTONIC, ts);
}

void hyp_profile_log_elapsed(const char *phase, const char *sub, const struct timespec *start,
                             long items) {
    struct timespec now;
    hyp_clock_gettime(CLOCK_MONOTONIC, &now);

    long us = ((long)(now.tv_sec - start->tv_sec) * PROF_US_PER_SEC) +
              ((now.tv_nsec - start->tv_nsec) / PROF_NS_PER_US);
    long ms = us / PROF_US_PER_MS;

    char ms_buf[PROF_BUF_LEN];
    char us_buf[PROF_BUF_LEN];
    char items_buf[PROF_BUF_LEN];
    snprintf(ms_buf, sizeof(ms_buf), "%ld", ms);
    snprintf(us_buf, sizeof(us_buf), "%ld", us);

    if (items > 0 && us > 0) {
        long rate = (long)((double)items * PROF_US_PER_SEC_D / (double)us);
        char rate_buf[PROF_BUF_LEN];
        snprintf(items_buf, sizeof(items_buf), "%ld", items);
        snprintf(rate_buf, sizeof(rate_buf), "%ld", rate);
        hyp_log_info("prof", "phase", phase, "sub", sub, "ms", ms_buf, "us", us_buf, "items",
                     items_buf, "rate_per_s", rate_buf);
    } else if (items > 0) {
        snprintf(items_buf, sizeof(items_buf), "%ld", items);
        hyp_log_info("prof", "phase", phase, "sub", sub, "ms", ms_buf, "us", us_buf, "items",
                     items_buf);
    } else {
        hyp_log_info("prof", "phase", phase, "sub", sub, "ms", ms_buf, "us", us_buf);
    }
}
