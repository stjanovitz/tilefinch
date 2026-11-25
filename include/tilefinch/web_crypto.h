#ifndef TILEFINCH_WEB_CRYPTO_H
#define TILEFINCH_WEB_CRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/resource_integrity.h"

/* The symmetric SubtleCrypto primitives (WebCrypto's AES-GCM, AES-CBC,
   AES-CTR, HMAC, PBKDF2 and HKDF) over the TLS library: Mbed TLS on the
   PSP, OpenSSL on the host. Hashes are TilefinchWebDigest values. Every
   function returns false for what WebCrypto reports as an OperationError
   (a failed tag or padding check, a counter that would repeat, a refused
   parameter) and leaves no key-dependent state behind. The bindings check
   the WebCrypto parameter rules first; these functions recheck only what
   keeps them memory-safe. */

/* A message, salt, info, IV or derived output above this many bytes is
   refused, as SubtleCrypto.digest refuses its input. Decryption allows
   the ciphertext to include the corresponding tag or padding overhead. */
#define TILEFINCH_WEB_CRYPTO_INPUT_LIMIT (1024u * 1024u)
/* PBKDF2's HMAC count (iterations times output blocks) is bounded: a
   derivation runs in slices on the script thread, and this bounds how many
   it takes. */
#define TILEFINCH_WEB_CRYPTO_PBKDF2_WORK_LIMIT 10000000u
/* An operation's work, in units of about 0.14 us in PPSSPP at 333 MHz
   (a byte of SHA-256; SHA-384 and SHA-512 bytes count twice, AES-CBC and
   AES-CTR bytes twice, AES-GCM bytes 11 times). Up to this much runs in
   the call, about 8 ms, one PBKDF2 slice; more runs as a job in slices
   (docs/engineering/JS_RUNTIME.md). */
#define TILEFINCH_WEB_CRYPTO_SLICE_WORK 57344u
/* Each block of HKDF's output re-hashes info, so its work is bounded: the
   bytes its hashes compress, in whole blocks with padding and HMAC's key
   pads, in those units. With a salt of up to a hash block, this admits
   all 255 output blocks with an info of up to 131,490 bytes (SHA-1),
   131,478 (SHA-256), 65,470 (SHA-384) or 65,454 (SHA-512), and one
   SHA-512 block with an info of up to 16,776,302 bytes. */
#define TILEFINCH_WEB_CRYPTO_HKDF_WORK_LIMIT (32u * 1024u * 1024u)

/* The hash's output size in bytes (20, 32, 48 or 64); 0 when unknown. */
size_t tilefinch_web_hash_length(TilefinchWebDigest hash);

/* HMAC of data under key; writes tilefinch_web_hash_length(hash) bytes. */
bool tilefinch_web_hmac(TilefinchWebDigest hash,
                        const uint8_t *key, size_t key_length,
                        const uint8_t *data, size_t length,
                        uint8_t output[64]);
/* A PBKDF2 derivation's progress between slices: zeroed before the first.
   The size is fixed, as the bootstrap allocates it (subtle-crypto.js). */
typedef struct {
    uint8_t u[64], t[64];
    /* Output blocks finished, and iterations done in the next one. */
    uint32_t blocks, iterations;
} TilefinchWebPbkdf2State;
#define TILEFINCH_WEB_PBKDF2_STATE_BYTES 136u
/* Runs PBKDF2 from state until output is complete or `more` (asked every
   16 HMACs; NULL runs to the end) answers false. Returns 1 when output is
   complete (state zeroed), 0 when work remains in state, and -1 for an
   OperationError (output and state zeroed). Every call takes the same
   parameters. */
int tilefinch_web_pbkdf2_slice(TilefinchWebDigest hash,
                               const uint8_t *password,
                               size_t password_length,
                               const uint8_t *salt, size_t salt_length,
                               uint32_t iterations,
                               uint8_t *output, size_t output_length,
                               TilefinchWebPbkdf2State *state,
                               bool (*more)(void *), void *more_context);
/* RFC 5869 extract-then-expand; output_length <= 255 hash lengths, and
   false without hashing or writing when the work is over
   TILEFINCH_WEB_CRYPTO_HKDF_WORK_LIMIT. */
bool tilefinch_web_hkdf(TilefinchWebDigest hash,
                        const uint8_t *key, size_t key_length,
                        const uint8_t *salt, size_t salt_length,
                        const uint8_t *info, size_t info_length,
                        uint8_t *output, size_t output_length);

/* AES keys are 16, 24 or 32 bytes. */
/* Encryption writes length + tag_length bytes (ciphertext, then tag);
   decryption reads the tag from the end of input and writes
   length - tag_length bytes. tag_length is 4, 8 or 12..16. */
bool tilefinch_web_aes_gcm(bool encrypt, const uint8_t *key,
                           size_t key_length, const uint8_t *iv,
                           size_t iv_length, const uint8_t *aad,
                           size_t aad_length, const uint8_t *input,
                           size_t length, size_t tag_length,
                           uint8_t *output, size_t *output_length);
/* PKCS#7 padded: encryption writes (length / 16 + 1) * 16 bytes into an
   output that large; decryption needs length bytes of scratch output
   and returns the plaintext length after checking the padding. */
bool tilefinch_web_aes_cbc(bool encrypt, const uint8_t *key,
                           size_t key_length, const uint8_t iv[16],
                           const uint8_t *input, size_t length,
                           uint8_t *output, size_t *output_length);
/* The rightmost counter_bits (1..128) of the 16-byte counter block count
   blocks and wrap within themselves; false when the message would reuse a
   counter value. Writes length bytes. */
bool tilefinch_web_aes_ctr(const uint8_t *key, size_t key_length,
                           const uint8_t counter[16], unsigned counter_bits,
                           const uint8_t *input, size_t length,
                           uint8_t *output);

/* An operation run in slices: every one above but PBKDF2, and digests.
   The values match the bootstrap's operation numbers. */
typedef enum {
    TILEFINCH_WEB_JOB_HMAC = 0,
    TILEFINCH_WEB_JOB_GCM_ENCRYPT = 1,
    TILEFINCH_WEB_JOB_GCM_DECRYPT = 2,
    TILEFINCH_WEB_JOB_CBC_ENCRYPT = 3,
    TILEFINCH_WEB_JOB_CBC_DECRYPT = 4,
    TILEFINCH_WEB_JOB_CTR = 5,
    TILEFINCH_WEB_JOB_HKDF = 7,
    TILEFINCH_WEB_JOB_DIGEST = 8
} TilefinchWebJobKind;
/* A job's parameters, as the functions above take them. The bytes must
   stay unchanged until the job ends.
     digest: a = data;  HMAC: key, a = data;
     HKDF: key, a = salt, b = info, number = output bytes;
     AES-GCM: key, a = iv, b = additional data, c = data (with the tag
       when decrypting), number = tag bytes;
     AES-CBC: key, a = iv (16 bytes), b = data;
     AES-CTR: key, a = counter block (16 bytes), b = data,
       number = counter bits. */
typedef struct {
    TilefinchWebJobKind kind;
    TilefinchWebDigest hash;
    size_t number;
    const uint8_t *key, *a, *b, *c;
    size_t key_length, a_length, b_length, c_length;
} TilefinchWebJobParams;
typedef struct TilefinchWebJob TilefinchWebJob;
/* The bytes a TilefinchWebJob takes; the caller allocates it. */
size_t tilefinch_web_job_size(void);
/* Checks the parameters as the functions above do: false for an
   OperationError. Otherwise gives the output bytes the job needs (it may
   write fewer) and its work in TILEFINCH_WEB_CRYPTO_SLICE_WORK's units. */
bool tilefinch_web_job_plan(const TilefinchWebJobParams *params,
                            size_t *capacity, size_t *work);
/* Plans a job; false for an OperationError. The job keys itself and
   allocates its backend contexts in its first run, so one that has not
   run holds nothing to free. Every begin, true or false, is followed by
   one tilefinch_web_job_end. */
bool tilefinch_web_job_begin(TilefinchWebJob *job,
                             const TilefinchWebJobParams *params);
/* Runs the job until it is complete or `more` (asked before each chunk
   after the first, a few KiB of input; NULL runs to the end) answers
   false. Every run takes the same output, at least the planned capacity.
   Returns 1 when the output is complete, 0 when work remains, and -1 for
   an OperationError (output zeroed). */
int tilefinch_web_job_run(TilefinchWebJob *job, uint8_t *output,
                          bool (*more)(void *), void *more_context);
/* The output bytes written once the job is complete. */
size_t tilefinch_web_job_length(const TilefinchWebJob *job);
/* Frees what the job holds and zeroes it. */
void tilefinch_web_job_end(TilefinchWebJob *job);

/* Zeroes bytes in a way the compiler cannot drop. */
void tilefinch_web_crypto_zeroize(void *bytes, size_t length);

#endif
