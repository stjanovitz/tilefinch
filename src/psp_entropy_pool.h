#ifndef TILEFINCH_PSP_ENTROPY_POOL_H
#define TILEFINCH_PSP_ENTROPY_POOL_H

#include "tilefinch/sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The platform-free half of the PSP entropy source (psp_entropy.c owns the
 * clock, the lock and the Memory Stick): the hash pool, the per-batch
 * min-entropy estimate and credit accounting, and the seed-file format.
 * docs/engineering/PSP_TRANSPORT.md ("Entropy") explains the design.
 *
 * Why an accumulator at all: the PSP offers no hardware random number
 * generator to a user-mode EBOOT. KIRK's PRNG (command 0xE through
 * sceUtilsBufferCopyWithRange) is exported only by the kernel "semaphore"
 * library, and libcglue's getentropy() reseeds MT19937 from time() on every
 * call, so its output is a function of the wall-clock second. What user mode
 * can observe is the microsecond system timer, and the jitter in how long
 * fixed work and short sleeps take is the one input a remote observer cannot
 * reconstruct.
 *
 * Credit is deliberately pessimistic. Each batch of timing samples is
 * charged the worst of several predictors (most-common value, repeat at lags
 * 1..PSP_ENTROPY_LAG_MAX, and a linear-trend guess), using the 99% upper
 * confidence bound on the predictor's hit rate in the style of NIST SP
 * 800-90B. The resulting per-sample min-entropy is capped at one bit and
 * then divided by PSP_ENTROPY_SAFETY_DIVISOR, because timing samples are not
 * independent and a predictor that we did not think of could do better. A
 * deterministic clock (PPSSPP's emulated one advances with executed
 * instructions) yields constant or cyclic samples and earns little or no
 * credit.
 *
 * Every byte offered is hashed into the pool whether or not it is credited;
 * the persisted seed file and boot context are mixed in but never credited,
 * since a copied or restored Memory Stick would replay them.
 */

/* Credited bits the pool must hold before it releases output as a seed. */
#define PSP_ENTROPY_TARGET_BITS 256u
/* Batch bounds for one estimate: large enough for a meaningful confidence
   bound, small enough to estimate in place with an insertion sort. */
#define PSP_ENTROPY_MIN_BATCH 32u
#define PSP_ENTROPY_MAX_BATCH 256u
#define PSP_ENTROPY_LAG_MAX 8u
/* Per-sample estimate ceiling (thousandths of a bit) before the divisor. */
#define PSP_ENTROPY_SAMPLE_CAP_MILLIBITS 1000u
#define PSP_ENTROPY_SAFETY_DIVISOR 4u

#define PSP_ENTROPY_SEED_BYTES 32u
/* "TFES", version, seed, then 8 bytes of SHA-256 over what precedes them.
   The check only detects a torn or foreign file; the seed is never credited,
   so a forged one can at worst contribute nothing. */
#define PSP_ENTROPY_SEED_FILE_VERSION 1u
#define PSP_ENTROPY_SEED_FILE_BYTES (4u + 4u + PSP_ENTROPY_SEED_BYTES + 8u)

/* Mixing domains keep the same bytes from different sources distinct. */
typedef enum {
    PSP_ENTROPY_DOMAIN_KEY = 0,
    PSP_ENTROPY_DOMAIN_WORK_TIMING,
    PSP_ENTROPY_DOMAIN_SLEEP_TIMING,
    PSP_ENTROPY_DOMAIN_SEED_FILE,
    PSP_ENTROPY_DOMAIN_BOOT_CONTEXT,
    PSP_ENTROPY_DOMAIN_POLL_CONTEXT
} PspEntropyDomain;

typedef struct {
    /* Always primed with the current key, so everything mixed since the last
       output chains into the next one. */
    TilefinchSha256 absorb;
    uint8_t key[TILEFINCH_SHA256_DIGEST_BYTES];
    uint64_t generation;
    uint32_t credited_bits;
} PspEntropyPool;

void psp_entropy_pool_init(PspEntropyPool *pool);

/* Hash `length` bytes into the pool without credit. */
void psp_entropy_pool_mix(PspEntropyPool *pool, PspEntropyDomain domain,
                          const void *data, size_t length);

/*
 * Estimated min-entropy per sample, in thousandths of a bit, of `count`
 * timing samples in collection order. Returns 0 when count is outside
 * [PSP_ENTROPY_MIN_BATCH, PSP_ENTROPY_MAX_BATCH]. The estimate sorts
 * `samples` in place once the order-dependent predictors have run, so mix
 * the batch before estimating it.
 */
uint32_t psp_entropy_estimate_millibits(uint32_t *samples, size_t count);

/* Bits credited for `count` samples at `millibits` each: capped at
   PSP_ENTROPY_SAMPLE_CAP_MILLIBITS, divided by PSP_ENTROPY_SAFETY_DIVISOR,
   rounded down. */
uint32_t psp_entropy_credit_bits(uint32_t millibits, size_t count);

/*
 * Mix a batch of timing samples (serialized little-endian, so the pool
 * state is identical on host and PSP), estimate it, and add the credit.
 * Returns the per-sample estimate; `credited` receives the bits added.
 * `samples` is reordered as described above.
 */
uint32_t psp_entropy_pool_add_batch(PspEntropyPool *pool,
                                    PspEntropyDomain domain,
                                    uint32_t *samples, size_t count,
                                    uint32_t *credited);

bool psp_entropy_pool_ready(const PspEntropyPool *pool);

/*
 * Produce `length` bytes and ratchet the key through a one-way step, so a
 * later compromise of the pool does not reveal earlier output. The caller
 * decides whether the pool is ready enough for the purpose.
 */
void psp_entropy_pool_generate(PspEntropyPool *pool, void *output,
                               size_t length);

void psp_entropy_seed_file_encode(
    const uint8_t seed[PSP_ENTROPY_SEED_BYTES],
    uint8_t file[PSP_ENTROPY_SEED_FILE_BYTES]);
/* False for any length, magic, version or check mismatch, or an all-zero
   seed (a zero-filled file is not a seed that was ever written). */
bool psp_entropy_seed_file_decode(const uint8_t *file, size_t length,
                                  uint8_t seed[PSP_ENTROPY_SEED_BYTES]);

/* Overwrite secrets the compiler must not elide. */
void psp_entropy_zeroize(void *data, size_t length);

#endif
