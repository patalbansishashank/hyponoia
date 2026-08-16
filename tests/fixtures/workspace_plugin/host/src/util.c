#include "util.h"

/* Resolves inside `host`. The workspace pass must never see this call: a
 * callee the member's own registry answered is not a candidate for anyone
 * else. */
int host_reserve_slot(void) {
    return 7;
}
