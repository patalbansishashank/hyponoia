#ifndef HYP_SHA256_H
#define HYP_SHA256_H

/* In-process SHA-256 (FIPS 180-4). Used to verify the integrity of a
 * downloaded release before installing it, without shelling out to a
 * platform hashing tool (shasum / sha256sum / certutil) — those differ per
 * OS, may be absent, and mis-quote paths under cmd.exe. */

#include <stddef.h>
#include <stdint.h>

#define HYP_SHA256_DIGEST_LEN 32 /* raw digest bytes */
#define HYP_SHA256_HEX_LEN 64    /* lowercase hex chars (no NUL) */

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t buf[64];
    size_t buflen;
} hyp_sha256_ctx;

void hyp_sha256_init(hyp_sha256_ctx *c);
void hyp_sha256_update(hyp_sha256_ctx *c, const void *data, size_t len);
void hyp_sha256_final(hyp_sha256_ctx *c, uint8_t out[HYP_SHA256_DIGEST_LEN]);

/* One-shot hash of a buffer to lowercase hex. `out` must hold
 * HYP_SHA256_HEX_LEN + 1 bytes (hex chars + NUL). */
void hyp_sha256_hex(const void *data, size_t len, char out[HYP_SHA256_HEX_LEN + 1]);

/* RFC 2104 HMAC-SHA-256. The output is always HYP_SHA256_DIGEST_LEN bytes.
 * A NULL key/data pointer is accepted only when its corresponding length is
 * zero. */
void hyp_hmac_sha256(const void *key, size_t key_len, const void *data, size_t data_len,
                     uint8_t out[HYP_SHA256_DIGEST_LEN]);

#endif /* HYP_SHA256_H */
