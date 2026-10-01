#ifndef TILEFINCH_PSP_ENTROPY_H
#define TILEFINCH_PSP_ENTROPY_H

#include <stdbool.h>
#include <stddef.h>

/*
 * The PSP's entropy source (src/psp_entropy.c, pure half in
 * src/psp_entropy_pool.h; docs/engineering/PSP_TRANSPORT.md "Entropy").
 * The owned mbed TLS build registers it as the only strong entropy source
 * through MBEDTLS_ENTROPY_HARDWARE_ALT, so legacy entropy contexts and PSA's
 * RNG (curl's psa_generate_random) both seed from it.
 */

typedef enum {
    /* No output until PSP_ENTROPY_TARGET_BITS have been credited from
       timing jitter. A bounded attempt that falls short fails the request;
       credit is kept, so the next request continues where it stopped. */
    PSP_ENTROPY_REQUIRE_CREDIT = 0,
    /* After one bounded attempt, release output even if the credit fell
       short, and report it. Only for processes that never create secrets
       with it (the launcher verifies signatures) or that exist to be run
       under emulation (validation builds, the crypto selftest). */
    PSP_ENTROPY_PERMIT_UNCREDITED
} PspEntropyPolicy;

/* Receives one diagnostic line without a trailing newline. */
typedef void (*PspEntropyReport)(const char *line);

/*
 * Call once from the main thread before any other thread can reach TLS or
 * DNS. `seed_path` names the persisted seed file (NULL for none); it is read
 * and rewritten when the pool is first seeded, not here, so configuring
 * costs no Memory Stick I/O. A NULL `report` prints to stdout. An
 * unconfigured process gets REQUIRE_CREDIT, no seed file, and stdout.
 */
void psp_entropy_configure(const char *seed_path, PspEntropyPolicy policy,
                           PspEntropyReport report);

/* Fill with seeded output, gathering first if needed: at most one bounded
   attempt (0.75 s plus one batch) per call until the pool is released,
   then only hashing. False when the policy refuses. */
bool psp_entropy_fill(void *output, size_t length);

/* Fill from the pool without gathering or waiting, whatever its credit.
   For values that need only be unguessable off-path (DNS query IDs),
   never for key material. */
void psp_entropy_fill_best_effort(void *output, size_t length);

#endif
