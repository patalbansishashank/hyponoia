/* Internal seams shared by platform implementations and focused tests. */
#ifndef HYP_PLATFORM_INTERNAL_H
#define HYP_PLATFORM_INTERNAL_H

#include <stdint.h>

/* Convert a monotonic counter to nanoseconds using its ticks-per-second
 * frequency. Kept outside the Windows guard so arithmetic edge cases can be
 * verified on every supported build host. */
uint64_t hyp_platform_scale_counter_ns(uint64_t counter, uint64_t frequency);

#endif /* HYP_PLATFORM_INTERNAL_H */
