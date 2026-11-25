#include "tilefinch/web_crypto.h"

#include <string.h>

/* The PSP links Mbed TLS for TLS, so every primitive below is code the
   transport already carries (the message-digest layer, AES and GCM); only
   the HMAC, PBKDF2, HKDF, CTR and padding glue here is new. The host uses
   OpenSSL, and a host test builds this file against Mbed TLS too
   (TILEFINCH_WEB_CRYPTO_MBEDTLS) so the PSP path runs the same vectors. */
#if defined(__PSP__) || defined(TILEFINCH_WEB_CRYPTO_MBEDTLS)
#define WEB_CRYPTO_MBEDTLS 1
#include <mbedtls/aes.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#else
#include <openssl/evp.h>
#endif

static void *(*const volatile web_crypto_memset)(void *, int, size_t) =
    memset;

void tilefinch_web_crypto_zeroize(void *bytes, size_t length)
{
    if (bytes != NULL && length != 0) web_crypto_memset(bytes, 0, length);
}

size_t tilefinch_web_hash_length(TilefinchWebDigest hash)
{
    switch (hash) {
    case TILEFINCH_WEB_DIGEST_SHA1: return 20u;
    case TILEFINCH_WEB_DIGEST_SHA256: return 32u;
    case TILEFINCH_WEB_DIGEST_SHA384: return 48u;
    case TILEFINCH_WEB_DIGEST_SHA512: return 64u;
    }
    return 0u;
}

static const uint8_t web_crypto_empty[1];

/* ---- hash backend: setup, begin, update, finish, copy, free ---- */

#if defined(WEB_CRYPTO_MBEDTLS)
typedef mbedtls_md_context_t WebHash;

static bool hash_setup(WebHash *hash, TilefinchWebDigest algorithm)
{
    static const mbedtls_md_type_t types[] = {
        MBEDTLS_MD_SHA1, MBEDTLS_MD_SHA256, MBEDTLS_MD_SHA384,
        MBEDTLS_MD_SHA512
    };
    mbedtls_md_init(hash);
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(
        types[algorithm]);
    return info != NULL && mbedtls_md_setup(hash, info, 0) == 0;
}

static bool hash_begin(WebHash *hash) { return mbedtls_md_starts(hash) == 0; }

static bool hash_update(WebHash *hash, const uint8_t *bytes, size_t length)
{
    return length == 0 || mbedtls_md_update(hash, bytes, length) == 0;
}

static bool hash_finish(WebHash *hash, uint8_t *output)
{
    return mbedtls_md_finish(hash, output) == 0;
}

static bool hash_copy(WebHash *target, const WebHash *source)
{
    return mbedtls_md_clone(target, source) == 0;
}

static void hash_free(WebHash *hash) { mbedtls_md_free(hash); }
#else
typedef struct {
    EVP_MD_CTX *context;
    const EVP_MD *md;
} WebHash;

static bool hash_setup(WebHash *hash, TilefinchWebDigest algorithm)
{
    hash->md = algorithm == TILEFINCH_WEB_DIGEST_SHA1 ? EVP_sha1()
        : algorithm == TILEFINCH_WEB_DIGEST_SHA256 ? EVP_sha256()
        : algorithm == TILEFINCH_WEB_DIGEST_SHA384 ? EVP_sha384()
        : EVP_sha512();
    hash->context = EVP_MD_CTX_new();
    return hash->context != NULL;
}

static bool hash_begin(WebHash *hash)
{
    return EVP_DigestInit_ex(hash->context, hash->md, NULL) == 1;
}

static bool hash_update(WebHash *hash, const uint8_t *bytes, size_t length)
{
    return length == 0
        || EVP_DigestUpdate(hash->context, bytes, length) == 1;
}

static bool hash_finish(WebHash *hash, uint8_t *output)
{
    return EVP_DigestFinal_ex(hash->context, output, NULL) == 1;
}

static bool hash_copy(WebHash *target, const WebHash *source)
{
    return EVP_MD_CTX_copy_ex(target->context, source->context) == 1;
}

static void hash_free(WebHash *hash) { EVP_MD_CTX_free(hash->context); }
#endif

/* ---- HMAC over the hash backend ----
   The inner and outer states are keyed once; each MAC copies them, so a
   PBKDF2 iteration costs two compressions instead of four. */

typedef struct {
    WebHash inner, outer, work;
    size_t length, block;
} WebHmac;

/* Unopened contexts are safe to free on either backend. */
static bool hmac_setup(WebHmac *hmac, TilefinchWebDigest algorithm)
{
    *hmac = (WebHmac) {
        .length = tilefinch_web_hash_length(algorithm),
        .block = algorithm >= TILEFINCH_WEB_DIGEST_SHA384 ? 128u : 64u
    };
    return hash_setup(&hmac->inner, algorithm)
        && hash_setup(&hmac->outer, algorithm)
        && hash_setup(&hmac->work, algorithm);
}

/* Keys the inner and outer states; key_length is at most a block. */
static bool hmac_key(WebHmac *hmac, const uint8_t *key, size_t key_length)
{
    uint8_t pad[128];
    memset(pad, 0x36, hmac->block);
    for (size_t i = 0; i < key_length; i++) pad[i] ^= key[i];
    bool ok = hash_begin(&hmac->inner)
        && hash_update(&hmac->inner, pad, hmac->block);
    for (size_t i = 0; i < hmac->block; i++) pad[i] ^= 0x36 ^ 0x5c;
    ok = ok && hash_begin(&hmac->outer)
        && hash_update(&hmac->outer, pad, hmac->block);
    tilefinch_web_crypto_zeroize(pad, sizeof(pad));
    return ok;
}

static bool hmac_begin(WebHmac *hmac, TilefinchWebDigest algorithm,
                       const uint8_t *key, size_t key_length)
{
    uint8_t hashed[64];
    if (!hmac_setup(hmac, algorithm)) return false;
    bool ok = true;
    if (key_length > hmac->block) {
        ok = hash_begin(&hmac->work)
            && hash_update(&hmac->work, key, key_length)
            && hash_finish(&hmac->work, hashed);
        key = hashed;
        key_length = hmac->length;
    }
    ok = ok && hmac_key(hmac, key, key_length);
    tilefinch_web_crypto_zeroize(hashed, sizeof(hashed));
    return ok;
}

/* Ends the message hashed into work after a copy of the inner state:
   output = H(outer || H(inner || message)). output may alias it. */
static bool hmac_close(WebHmac *hmac, uint8_t *output)
{
    uint8_t inner[64];
    bool ok = hash_finish(&hmac->work, inner)
        && hash_copy(&hmac->work, &hmac->outer)
        && hash_update(&hmac->work, inner, hmac->length)
        && hash_finish(&hmac->work, output);
    tilefinch_web_crypto_zeroize(inner, sizeof(inner));
    return ok;
}

/* The MAC of a || b || c. output may alias a: it is written last. */
static bool hmac_run(WebHmac *hmac, const uint8_t *a, size_t a_length,
                     const uint8_t *b, size_t b_length,
                     const uint8_t *c, size_t c_length, uint8_t *output)
{
    return hash_copy(&hmac->work, &hmac->inner)
        && hash_update(&hmac->work, a, a_length)
        && hash_update(&hmac->work, b, b_length)
        && hash_update(&hmac->work, c, c_length)
        && hmac_close(hmac, output);
}

static void hmac_end(WebHmac *hmac)
{
    hash_free(&hmac->inner);
    hash_free(&hmac->outer);
    hash_free(&hmac->work);
}

#define WEB_BYTES(pointer) ((pointer) == NULL ? web_crypto_empty : (pointer))

_Static_assert(sizeof(TilefinchWebPbkdf2State)
               == TILEFINCH_WEB_PBKDF2_STATE_BYTES, "PBKDF2 state size");

int tilefinch_web_pbkdf2_slice(TilefinchWebDigest hash,
                               const uint8_t *password,
                               size_t password_length,
                               const uint8_t *salt, size_t salt_length,
                               uint32_t iterations,
                               uint8_t *output, size_t output_length,
                               TilefinchWebPbkdf2State *state,
                               bool (*more)(void *), void *more_context)
{
    size_t hash_length = tilefinch_web_hash_length(hash);
    if (hash_length == 0 || output == NULL || state == NULL
        || iterations == 0 || output_length == 0
        || output_length > TILEFINCH_WEB_CRYPTO_INPUT_LIMIT
        || (password == NULL && password_length != 0)
        || (salt == NULL && salt_length != 0)) return -1;
    size_t blocks = (output_length + hash_length - 1u) / hash_length;
    if (iterations > TILEFINCH_WEB_CRYPTO_PBKDF2_WORK_LIMIT / blocks
        || state->blocks >= blocks || state->iterations >= iterations)
        return -1;
    WebHmac hmac;
    bool ok = hmac_begin(&hmac, hash, WEB_BYTES(password), password_length);
    for (unsigned polls = 0; ok && state->blocks < blocks; ) {
        if (more != NULL && ++polls % 16u == 0 && !more(more_context))
            break;
        if (state->iterations == 0) {
            uint32_t block = state->blocks + 1u;
            uint8_t index[4] = {
                (uint8_t) (block >> 24), (uint8_t) (block >> 16),
                (uint8_t) (block >> 8), (uint8_t) block
            };
            ok = hmac_run(&hmac, WEB_BYTES(salt), salt_length, index, 4u,
                          NULL, 0, state->u);
            if (!ok) break;
            memcpy(state->t, state->u, hash_length);
        } else {
            ok = hmac_run(&hmac, state->u, hash_length, NULL, 0, NULL, 0,
                          state->u);
            if (!ok) break;
            for (size_t j = 0; j < hash_length; j++)
                state->t[j] ^= state->u[j];
        }
        if (++state->iterations == iterations) {
            size_t done = state->blocks * hash_length;
            size_t take = output_length - done < hash_length
                ? output_length - done : hash_length;
            memcpy(output + done, state->t, take);
            state->blocks++;
            state->iterations = 0;
        }
    }
    hmac_end(&hmac);
    bool complete = ok && state->blocks == blocks;
    if (!ok) tilefinch_web_crypto_zeroize(output, output_length);
    if (!ok || complete) tilefinch_web_crypto_zeroize(state, sizeof(*state));
    return !ok ? -1 : complete ? 1 : 0;
}

/* ---- AES backend: ECB encryption (CTR's key stream) and raw CBC ---- */

typedef enum {
    WEB_AES_ECB_ENCRYPT = 0,
    WEB_AES_CBC_ENCRYPT,
    WEB_AES_CBC_DECRYPT
} WebAesMode;

static bool web_aes_key_valid(const uint8_t *key, size_t key_length)
{
    return key != NULL
        && (key_length == 16u || key_length == 24u || key_length == 32u);
}

#if defined(WEB_CRYPTO_MBEDTLS)
typedef struct {
    mbedtls_aes_context aes;
    WebAesMode mode;
    uint8_t iv[16];
} WebAes;

static bool aes_begin(WebAes *aes, WebAesMode mode, const uint8_t *key,
                      size_t key_length, const uint8_t iv[16])
{
    mbedtls_aes_init(&aes->aes);
    aes->mode = mode;
    if (iv != NULL) memcpy(aes->iv, iv, 16u);
    unsigned bits = (unsigned) key_length * 8u;
    return (mode == WEB_AES_CBC_DECRYPT
            ? mbedtls_aes_setkey_dec(&aes->aes, key, bits)
            : mbedtls_aes_setkey_enc(&aes->aes, key, bits)) == 0;
}

/* length is a multiple of 16. */
static bool aes_update(WebAes *aes, const uint8_t *input, size_t length,
                       uint8_t *output)
{
    if (aes->mode != WEB_AES_ECB_ENCRYPT) {
        return mbedtls_aes_crypt_cbc(
            &aes->aes, aes->mode == WEB_AES_CBC_ENCRYPT
                ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT,
            length, aes->iv, input, output) == 0;
    }
    for (size_t at = 0; at < length; at += 16u) {
        if (mbedtls_aes_crypt_ecb(&aes->aes, MBEDTLS_AES_ENCRYPT,
                                  input + at, output + at) != 0)
            return false;
    }
    return true;
}

static void aes_end(WebAes *aes)
{
    mbedtls_aes_free(&aes->aes);
    tilefinch_web_crypto_zeroize(aes->iv, sizeof(aes->iv));
}
#else
typedef struct {
    EVP_CIPHER_CTX *context;
} WebAes;

static bool aes_begin(WebAes *aes, WebAesMode mode, const uint8_t *key,
                      size_t key_length, const uint8_t iv[16])
{
    const EVP_CIPHER *cipher = mode == WEB_AES_ECB_ENCRYPT
        ? (key_length == 16u ? EVP_aes_128_ecb()
           : key_length == 24u ? EVP_aes_192_ecb() : EVP_aes_256_ecb())
        : (key_length == 16u ? EVP_aes_128_cbc()
           : key_length == 24u ? EVP_aes_192_cbc() : EVP_aes_256_cbc());
    aes->context = EVP_CIPHER_CTX_new();
    return aes->context != NULL
        && EVP_CipherInit_ex(aes->context, cipher, NULL, key, iv,
                             mode == WEB_AES_CBC_DECRYPT ? 0 : 1) == 1
        && EVP_CIPHER_CTX_set_padding(aes->context, 0) == 1;
}

static bool aes_update(WebAes *aes, const uint8_t *input, size_t length,
                       uint8_t *output)
{
    int written = 0;
    return length <= (size_t) 0x7fffffff
        && EVP_CipherUpdate(aes->context, output, &written, input,
                            (int) length) == 1
        && (size_t) written == length;
}

static void aes_end(WebAes *aes) { EVP_CIPHER_CTX_free(aes->context); }
#endif


/* ---- jobs: an operation run in slices ----
   Every operation but PBKDF2 is a job: begin keys it, and each run feeds
   the next chunks of its input until the slice's time is up. A job hashes
   or ciphers its input in order exactly as one call would, so the bytes
   are the same however it is sliced. The one-call functions below run a
   job to the end. */

/* Input bytes fed between polls: about 1.2 ms of SHA-512, AES-CBC or
   AES-CTR in PPSSPP at 333 MHz, and 1.5 ms of AES-GCM. Both are whole AES
   blocks, so CBC and CTR chunks stay block-aligned. */
#define WEB_JOB_CHUNK 4096u
#define WEB_JOB_GCM_CHUNK 1024u
#define WEB_JOB_ENDED 0xffu

struct TilefinchWebJob {
    TilefinchWebJobParams p;
    size_t capacity, written;
    /* Bytes of the current input fed so far. */
    size_t at;
    bool (*more)(void *);
    void *more_context;
    uint8_t *output;
    /* Chunks begun in this run: the first is never refused, so every run
       makes progress. */
    unsigned chunks;
    /* The operation's stage, WEB_JOB_ENDED once it has settled. */
    uint8_t stage;
    /* Whether the contexts below are set up: on the first run, so a job
       waiting its turn holds no backend allocation. */
    bool opened;
    /* HKDF: the next output block's counter. */
    uint8_t counter;
    /* HMAC and HKDF; a digest hashes in hmac.work. */
    WebHmac hmac;
    WebAes aes;
    /* AES-CTR's counter block. */
    uint8_t block[16];
    /* A hashed HMAC key, then HKDF's pseudorandom key and T(i). */
    uint8_t t[64];
#if defined(WEB_CRYPTO_MBEDTLS)
    mbedtls_gcm_context gcm;
#else
    EVP_CIPHER_CTX *gcm;
#endif
};

size_t tilefinch_web_job_size(void) { return sizeof(TilefinchWebJob); }

/* The bytes a hash compresses for a message of a + b bytes: the message,
   0x80 and the length field (an eighth of a block), in whole blocks. */
static size_t hash_padded(size_t a, size_t b, size_t block)
{
    return (a / block + b / block
            + (a % block + b % block + block / 8u + block) / block) * block;
}

/* HKDF's work, counted as the job hashes: every block of output re-hashes
   info (RFC 5869 section 2.3), and SHA-384 and SHA-512 bytes count twice.
   SIZE_MAX when it is over TILEFINCH_WEB_CRYPTO_HKDF_WORK_LIMIT. Each
   length is checked against the limit first, so no sum below can
   overflow. */
static size_t hkdf_work(size_t hash_length, size_t key_length,
                        size_t salt_length, size_t info_length,
                        size_t output_length)
{
    size_t block = hash_length > 32u ? 128u : 64u, weight = block / 64u,
        limit = TILEFINCH_WEB_CRYPTO_HKDF_WORK_LIMIT / weight;
    if (key_length > limit || salt_length > limit || info_length > limit)
        return SIZE_MAX;
    size_t blocks = (output_length + hash_length - 1u) / hash_length,
        /* Extract keys HMAC with the salt (hashed when over a block), then
           expand with the pseudorandom key; each keying hashes two pad
           blocks. */
        work = (salt_length > block ? hash_padded(salt_length, 0, block)
                                    : 0u)
            + 4u * block + hash_padded(key_length, 0, block)
            + hash_padded(hash_length, 0, block)
            + hash_padded(info_length, 1u, block),
        /* Blocks after the first also hash the previous block's output. */
        each = hash_padded(hash_length + 1u, info_length, block),
        outer = hash_padded(hash_length, 0, block);
    if (work > limit) return SIZE_MAX;
    size_t left = limit - work;
    if (outer > left) return SIZE_MAX;
    left -= outer;
    if (blocks - 1u > left / (each + outer)) return SIZE_MAX;
    return (work + outer + (blocks - 1u) * (each + outer)) * weight;
}

bool tilefinch_web_job_plan(const TilefinchWebJobParams *p,
                            size_t *capacity, size_t *work)
{
    const size_t limit = TILEFINCH_WEB_CRYPTO_INPUT_LIMIT;
    size_t hash_length = tilefinch_web_hash_length(p->hash), out = 0,
        cost = 0, tag = p->number;
    if ((p->key == NULL && p->key_length != 0)
        || (p->a == NULL && p->a_length != 0)
        || (p->b == NULL && p->b_length != 0)
        || (p->c == NULL && p->c_length != 0)) return false;
    bool aes = p->kind != TILEFINCH_WEB_JOB_DIGEST
        && p->kind != TILEFINCH_WEB_JOB_HMAC
        && p->kind != TILEFINCH_WEB_JOB_HKDF;
    if (aes ? !web_aes_key_valid(p->key, p->key_length) : hash_length == 0)
        return false;
    switch (p->kind) {
    case TILEFINCH_WEB_JOB_DIGEST:
    case TILEFINCH_WEB_JOB_HMAC:
        if (p->a_length > limit || p->key_length > limit) return false;
        out = hash_length;
        cost = (p->key_length + p->a_length + 512u)
            * (hash_length > 32u ? 2u : 1u);
        break;
    case TILEFINCH_WEB_JOB_HKDF:
        if (p->number == 0 || p->number > 255u * hash_length) return false;
        cost = hkdf_work(hash_length, p->key_length, p->a_length,
                         p->b_length, p->number);
        if (cost == SIZE_MAX) return false;
        out = p->number;
        break;
    case TILEFINCH_WEB_JOB_GCM_ENCRYPT:
    case TILEFINCH_WEB_JOB_GCM_DECRYPT: {
        bool encrypt = p->kind == TILEFINCH_WEB_JOB_GCM_ENCRYPT;
        if (p->a_length == 0 || p->a_length > limit || p->b_length > limit
            || tag < 4u || tag > 16u || (tag < 12u && tag != 4u && tag != 8u)
            || (!encrypt && p->c_length < tag)) return false;
        size_t text = encrypt ? p->c_length : p->c_length - tag;
        if (text > limit) return false;
        out = encrypt ? text + tag : text;
        /* GHASH is most of the cost; a 12-byte IV is not hashed. */
        cost = 11u * (p->b_length + text)
            + (p->a_length == 12u ? 0u : 9u * p->a_length);
        break;
    }
    case TILEFINCH_WEB_JOB_CBC_ENCRYPT:
    case TILEFINCH_WEB_JOB_CBC_DECRYPT:
        if (p->a_length != 16u) return false;
        if (p->kind == TILEFINCH_WEB_JOB_CBC_ENCRYPT) {
            if (p->b_length > limit) return false;
            out = p->b_length - p->b_length % 16u + 16u;
        } else {
            if (p->b_length == 0 || p->b_length % 16u != 0
                || p->b_length > limit + 16u) return false;
            out = p->b_length;
        }
        cost = 2u * p->b_length;
        break;
    case TILEFINCH_WEB_JOB_CTR: {
        if (p->a_length != 16u || p->number == 0 || p->number > 128u
            || p->b_length > limit) return false;
        /* Only the counter bits change, so more blocks than they can
           count would reuse a key stream block. */
        size_t blocks = (p->b_length + 15u) / 16u;
        if (p->number < 32u && blocks > ((size_t) 1u << p->number))
            return false;
        out = p->b_length;
        cost = 2u * p->b_length;
        break;
    }
    default:
        return false;
    }
    *capacity = out;
    *work = cost;
    return true;
}

#if defined(WEB_CRYPTO_MBEDTLS)
static bool gcm_begin(TilefinchWebJob *job, bool encrypt)
{
    const TilefinchWebJobParams *p = &job->p;
    return mbedtls_gcm_setkey(&job->gcm, MBEDTLS_CIPHER_ID_AES, p->key,
                              (unsigned) p->key_length * 8u) == 0
        && mbedtls_gcm_starts(&job->gcm, encrypt ? MBEDTLS_GCM_ENCRYPT
                                                 : MBEDTLS_GCM_DECRYPT,
                              p->a, p->a_length) == 0;
}
#else
static bool gcm_begin(TilefinchWebJob *job, bool encrypt)
{
    const TilefinchWebJobParams *p = &job->p;
    const EVP_CIPHER *cipher = p->key_length == 16u ? EVP_aes_128_gcm()
        : p->key_length == 24u ? EVP_aes_192_gcm() : EVP_aes_256_gcm();
    job->gcm = EVP_CIPHER_CTX_new();
    return job->gcm != NULL && p->a_length <= (size_t) 0x7fffffff
        && EVP_CipherInit_ex(job->gcm, cipher, NULL, NULL, NULL,
                             encrypt ? 1 : 0) == 1
        && EVP_CIPHER_CTX_ctrl(job->gcm, EVP_CTRL_GCM_SET_IVLEN,
                               (int) p->a_length, NULL) == 1
        && EVP_CipherInit_ex(job->gcm, NULL, NULL, p->key, p->a, -1) == 1;
}
#endif

/* Sets up the job's contexts; Mbed TLS allocates its hash and GCM ones
   from the C heap, outside the script budget. */
static bool job_open(TilefinchWebJob *job)
{
    const TilefinchWebJobParams *p = &job->p;
    switch (p->kind) {
    case TILEFINCH_WEB_JOB_DIGEST:
        return hash_setup(&job->hmac.work, p->hash)
            && hash_begin(&job->hmac.work);
    case TILEFINCH_WEB_JOB_HMAC:
    case TILEFINCH_WEB_JOB_HKDF:
        /* The first stage hashes a key longer than a block. */
        return hmac_setup(&job->hmac, p->hash)
            && ((p->kind == TILEFINCH_WEB_JOB_HMAC ? p->key_length
                                                   : p->a_length)
                    <= job->hmac.block
                || hash_begin(&job->hmac.work));
    case TILEFINCH_WEB_JOB_GCM_ENCRYPT:
    case TILEFINCH_WEB_JOB_GCM_DECRYPT:
        return gcm_begin(job, p->kind == TILEFINCH_WEB_JOB_GCM_ENCRYPT);
    case TILEFINCH_WEB_JOB_CBC_ENCRYPT:
    case TILEFINCH_WEB_JOB_CBC_DECRYPT:
        return aes_begin(&job->aes, p->kind == TILEFINCH_WEB_JOB_CBC_ENCRYPT
                         ? WEB_AES_CBC_ENCRYPT : WEB_AES_CBC_DECRYPT,
                         p->key, p->key_length, p->a);
    case TILEFINCH_WEB_JOB_CTR:
        memcpy(job->block, p->a, 16u);
        return aes_begin(&job->aes, WEB_AES_ECB_ENCRYPT, p->key,
                         p->key_length, NULL);
    }
    return false;
}

bool tilefinch_web_job_begin(TilefinchWebJob *job,
                             const TilefinchWebJobParams *params)
{
    /* Zeroed contexts are unopened on both backends (Mbed TLS's init
       functions zero them), so end frees whatever the first run opened. */
    memset(job, 0, sizeof(*job));
    job->stage = WEB_JOB_ENDED;
    size_t work = 0;
    if (!tilefinch_web_job_plan(params, &job->capacity, &work)) return false;
    job->p = *params;
    job->counter = 1;
    job->stage = 0;
    return true;
}

static bool job_more(TilefinchWebJob *job)
{
    return job->chunks++ == 0 || job->more == NULL
        || job->more(job->more_context);
}

typedef bool (*WebJobStep)(TilefinchWebJob *job, const uint8_t *bytes,
                           size_t length, size_t at);

/* Feeds bytes [job->at, length) to step a chunk at a time while the slice
   lasts: 1 once all are fed (job->at is back to 0), 0 when the slice ended
   first, -1 for a failure. */
static int job_feed(TilefinchWebJob *job, const uint8_t *bytes,
                    size_t length, size_t chunk, WebJobStep step)
{
    while (job->at < length) {
        if (!job_more(job)) return 0;
        size_t take = length - job->at < chunk ? length - job->at : chunk;
        if (!step(job, bytes + job->at, take, job->at)) return -1;
        job->at += take;
    }
    job->at = 0;
    return 1;
}

static bool step_hash(TilefinchWebJob *job, const uint8_t *bytes,
                      size_t length, size_t at)
{
    (void) at;
    return hash_update(&job->hmac.work, bytes, length);
}

/* CBC either way; length is a multiple of 16. */
static bool step_aes(TilefinchWebJob *job, const uint8_t *bytes,
                     size_t length, size_t at)
{
    return aes_update(&job->aes, bytes, length, job->output + at);
}

static bool step_ctr(TilefinchWebJob *job, const uint8_t *bytes,
                     size_t length, size_t at)
{
    uint8_t stream[16];
    bool ok = true;
    for (size_t i = 0; ok && i < length; i += 16u) {
        ok = aes_update(&job->aes, job->block, 16u, stream);
        size_t take = length - i < 16u ? length - i : 16u;
        for (size_t j = 0; j < take; j++)
            job->output[at + i + j] = bytes[i + j] ^ stream[j];
        /* Increment the rightmost counter bits, wrapping within them. */
        unsigned bits = (unsigned) job->p.number;
        for (int k = 15; bits != 0u && k >= 0; k--) {
            unsigned take_bits = bits < 8u ? bits : 8u;
            unsigned mask = (1u << take_bits) - 1u;
            unsigned value = (job->block[k] & mask) + 1u;
            job->block[k] = (uint8_t) ((job->block[k] & ~mask)
                                       | (value & mask));
            bits -= take_bits;
            if (value <= mask) break;
        }
    }
    tilefinch_web_crypto_zeroize(stream, sizeof(stream));
    return ok;
}

static bool step_gcm_aad(TilefinchWebJob *job, const uint8_t *bytes,
                         size_t length, size_t at)
{
    (void) at;
#if defined(WEB_CRYPTO_MBEDTLS)
    return mbedtls_gcm_update_ad(&job->gcm, bytes, length) == 0;
#else
    int written = 0;
    return EVP_CipherUpdate(job->gcm, NULL, &written, bytes,
                            (int) length) == 1;
#endif
}

static bool step_gcm(TilefinchWebJob *job, const uint8_t *bytes,
                     size_t length, size_t at)
{
#if defined(WEB_CRYPTO_MBEDTLS)
    size_t written = 0;
    return mbedtls_gcm_update(&job->gcm, bytes, length, job->output + at,
                              length, &written) == 0 && written == length;
#else
    int written = 0;
    return EVP_CipherUpdate(job->gcm, job->output + at, &written, bytes,
                            (int) length) == 1
        && (size_t) written == length;
#endif
}

/* Writes the tag after the ciphertext, or checks the one after it. */
static bool gcm_finish(TilefinchWebJob *job, size_t text)
{
    const TilefinchWebJobParams *p = &job->p;
    bool encrypt = p->kind == TILEFINCH_WEB_JOB_GCM_ENCRYPT;
    size_t tag = p->number;
    job->written = encrypt ? text + tag : text;
#if defined(WEB_CRYPTO_MBEDTLS)
    uint8_t check[16];
    size_t written = 0;
    bool ok = mbedtls_gcm_finish(&job->gcm, NULL, 0, &written, check,
                                 tag) == 0 && written == 0;
    if (encrypt) memcpy(job->output + text, check, tag);
    else {
        /* Every tag byte is compared whatever the earlier ones held. */
        unsigned difference = 0;
        for (size_t i = 0; i < tag; i++)
            difference |= check[i] ^ p->c[text + i];
        ok = ok && difference == 0;
    }
    tilefinch_web_crypto_zeroize(check, sizeof(check));
    return ok;
#else
    uint8_t final[16];
    int written = 0;
    return (encrypt
            || EVP_CIPHER_CTX_ctrl(job->gcm, EVP_CTRL_GCM_SET_TAG, (int) tag,
                                   (void *) (p->c + text)) == 1)
        && EVP_CipherFinal_ex(job->gcm, final, &written) == 1
        && (!encrypt
            || EVP_CIPHER_CTX_ctrl(job->gcm, EVP_CTRL_GCM_GET_TAG, (int) tag,
                                   job->output + text) == 1);
#endif
}

/* PKCS#7: the last byte names 1..16 bytes that all hold it. The check
   reads every candidate byte whatever it finds. */
static bool cbc_unpad(TilefinchWebJob *job)
{
    const uint8_t *output = job->output;
    size_t length = job->p.b_length;
    unsigned pad = output[length - 1u], bad = pad - 1u >= 16u;
    for (unsigned i = 1; i <= 16u; i++) {
        unsigned in_pad = i <= pad;
        bad |= in_pad & (output[length - i] != pad);
    }
    job->written = bad ? 0u : length - pad;
    return !bad && length - pad <= TILEFINCH_WEB_CRYPTO_INPUT_LIMIT;
}

/* HMAC, or HKDF: stage 0 keys the MAC (hashing a long key first), stage 1
   hashes the message, and HKDF's stage 2 makes one output block a turn. */
static int job_mac(TilefinchWebJob *job)
{
    const TilefinchWebJobParams *p = &job->p;
    WebHmac *hmac = &job->hmac;
    bool hkdf = p->kind == TILEFINCH_WEB_JOB_HKDF;
    int status = 1;
    if (job->stage == 0) {
        /* HMAC's key, or HKDF-Extract's salt. An empty salt is HashLen
           zero bytes: HMAC pads both to a block. */
        const uint8_t *key = WEB_BYTES(hkdf ? p->a : p->key);
        size_t key_length = hkdf ? p->a_length : p->key_length;
        if (key_length > hmac->block) {
            status = job_feed(job, key, key_length, WEB_JOB_CHUNK,
                              step_hash);
            if (status <= 0) return status;
            if (!hash_finish(&hmac->work, job->t)) return -1;
            key = job->t;
            key_length = hmac->length;
        }
        if (!hmac_key(hmac, key, key_length)
            || !hash_copy(&hmac->work, &hmac->inner)) return -1;
        job->stage = 1;
    }
    if (job->stage == 1) {
        /* HMAC's data, or HKDF-Extract's input keying material. */
        status = job_feed(job, WEB_BYTES(hkdf ? p->key : p->a),
                          hkdf ? p->key_length : p->a_length, WEB_JOB_CHUNK,
                          step_hash);
        if (status <= 0) return status;
        if (!hkdf) {
            job->written = hmac->length;
            return hmac_close(hmac, job->output) ? 1 : -1;
        }
        /* The pseudorandom key keys HKDF-Expand; T(0) is empty. */
        if (!hmac_close(hmac, job->t) || !hmac_key(hmac, job->t, hmac->length)
            || !hash_copy(&hmac->work, &hmac->inner)) return -1;
        job->stage = 2;
    }
    /* T(i) = HMAC(PRK, T(i - 1) || info || i). Each block counts as a
       chunk, so a short info is polled too. */
    for (;;) {
        status = job_feed(job, WEB_BYTES(p->b), p->b_length, WEB_JOB_CHUNK,
                          step_hash);
        if (status <= 0) return status;
        uint8_t counter = job->counter;
        if (!hash_update(&hmac->work, &counter, 1u)
            || !hmac_close(hmac, job->t)) return -1;
        size_t left = p->number - job->written,
            take = left < hmac->length ? left : hmac->length;
        memcpy(job->output + job->written, job->t, take);
        job->written += take;
        if (job->written == p->number) return 1;
        job->counter++;
        if (!hash_copy(&hmac->work, &hmac->inner)
            || !hash_update(&hmac->work, job->t, hmac->length)) return -1;
        if (!job_more(job)) return 0;
    }
}

int tilefinch_web_job_run(TilefinchWebJob *job, uint8_t *output,
                          bool (*more)(void *), void *more_context)
{
    if (job->stage == WEB_JOB_ENDED || output == NULL) return -1;
    const TilefinchWebJobParams *p = &job->p;
    job->output = output;
    job->more = more;
    job->more_context = more_context;
    job->chunks = 0;
    int status = -1;
    if (!job->opened) job->opened = job_open(job);
    if (job->opened) switch (p->kind) {
    case TILEFINCH_WEB_JOB_DIGEST:
        status = job_feed(job, WEB_BYTES(p->a), p->a_length, WEB_JOB_CHUNK,
                          step_hash);
        job->written = job->capacity;
        if (status > 0 && !hash_finish(&job->hmac.work, output)) status = -1;
        break;
    case TILEFINCH_WEB_JOB_HMAC:
    case TILEFINCH_WEB_JOB_HKDF:
        status = job_mac(job);
        break;
    case TILEFINCH_WEB_JOB_GCM_ENCRYPT:
    case TILEFINCH_WEB_JOB_GCM_DECRYPT: {
        /* Stage 0 authenticates the additional data, stage 1 the text. */
        size_t text = p->kind == TILEFINCH_WEB_JOB_GCM_ENCRYPT
            ? p->c_length : p->c_length - p->number;
        status = 1;
        if (job->stage == 0) {
            status = job_feed(job, WEB_BYTES(p->b), p->b_length,
                              WEB_JOB_GCM_CHUNK, step_gcm_aad);
            if (status > 0) job->stage = 1;
        }
        if (status > 0)
            status = job_feed(job, WEB_BYTES(p->c), text, WEB_JOB_GCM_CHUNK,
                              step_gcm);
        if (status > 0 && !gcm_finish(job, text)) status = -1;
        break;
    }
    case TILEFINCH_WEB_JOB_CBC_ENCRYPT: {
        size_t whole = p->b_length - p->b_length % 16u;
        status = job_feed(job, WEB_BYTES(p->b), whole, WEB_JOB_CHUNK,
                          step_aes);
        if (status > 0) {
            /* PKCS#7: the last block is padded with its pad's length. */
            uint8_t last[16];
            size_t pad = 16u - p->b_length % 16u;
            if (pad != 16u) memcpy(last, p->b + whole, 16u - pad);
            memset(last + 16u - pad, (int) pad, pad);
            if (!aes_update(&job->aes, last, 16u, output + whole))
                status = -1;
            tilefinch_web_crypto_zeroize(last, sizeof(last));
            job->written = whole + 16u;
        }
        break;
    }
    case TILEFINCH_WEB_JOB_CBC_DECRYPT:
        status = job_feed(job, p->b, p->b_length, WEB_JOB_CHUNK, step_aes);
        if (status > 0 && !cbc_unpad(job)) status = -1;
        break;
    case TILEFINCH_WEB_JOB_CTR:
        status = job_feed(job, WEB_BYTES(p->b), p->b_length, WEB_JOB_CHUNK,
                          step_ctr);
        job->written = p->b_length;
        break;
    }
    if (status < 0) {
        tilefinch_web_crypto_zeroize(output, job->capacity);
        job->written = 0;
    }
    if (status != 0) job->stage = WEB_JOB_ENDED;
    return status;
}

size_t tilefinch_web_job_length(const TilefinchWebJob *job)
{
    return job->written;
}

void tilefinch_web_job_end(TilefinchWebJob *job)
{
    hmac_end(&job->hmac);
    aes_end(&job->aes);
#if defined(WEB_CRYPTO_MBEDTLS)
    mbedtls_gcm_free(&job->gcm);
#else
    EVP_CIPHER_CTX_free(job->gcm);
#endif
    tilefinch_web_crypto_zeroize(job, sizeof(*job));
}

/* ---- the one-call forms ---- */

/* Runs a job to the end. A refused parameter writes nothing; any later
   failure zeroes the output. */
static bool job_once(const TilefinchWebJobParams *params, uint8_t *output,
                     size_t *written)
{
    TilefinchWebJob job;
    bool ok = tilefinch_web_job_begin(&job, params)
        && tilefinch_web_job_run(&job, output, NULL, NULL) == 1;
    if (!ok) tilefinch_web_crypto_zeroize(output, job.capacity);
    if (written != NULL) *written = ok ? job.written : 0u;
    tilefinch_web_job_end(&job);
    return ok;
}

bool tilefinch_web_hmac(TilefinchWebDigest hash,
                        const uint8_t *key, size_t key_length,
                        const uint8_t *data, size_t length,
                        uint8_t output[64])
{
    if (output == NULL) return false;
    TilefinchWebJobParams params = {
        .kind = TILEFINCH_WEB_JOB_HMAC, .hash = hash,
        .key = key, .key_length = key_length, .a = data, .a_length = length
    };
    return job_once(&params, output, NULL);
}

bool tilefinch_web_hkdf(TilefinchWebDigest hash,
                        const uint8_t *key, size_t key_length,
                        const uint8_t *salt, size_t salt_length,
                        const uint8_t *info, size_t info_length,
                        uint8_t *output, size_t output_length)
{
    if (output == NULL) return false;
    TilefinchWebJobParams params = {
        .kind = TILEFINCH_WEB_JOB_HKDF, .hash = hash,
        .number = output_length, .key = key, .key_length = key_length,
        .a = salt, .a_length = salt_length, .b = info, .b_length = info_length
    };
    return job_once(&params, output, NULL);
}

bool tilefinch_web_aes_cbc(bool encrypt, const uint8_t *key,
                           size_t key_length, const uint8_t iv[16],
                           const uint8_t *input, size_t length,
                           uint8_t *output, size_t *output_length)
{
    if (output == NULL || output_length == NULL || iv == NULL) return false;
    TilefinchWebJobParams params = {
        .kind = encrypt ? TILEFINCH_WEB_JOB_CBC_ENCRYPT
                        : TILEFINCH_WEB_JOB_CBC_DECRYPT,
        .key = key, .key_length = key_length, .a = iv, .a_length = 16u,
        .b = input, .b_length = length
    };
    return job_once(&params, output, output_length);
}

bool tilefinch_web_aes_ctr(const uint8_t *key, size_t key_length,
                           const uint8_t counter[16], unsigned counter_bits,
                           const uint8_t *input, size_t length,
                           uint8_t *output)
{
    if (output == NULL || counter == NULL) return false;
    TilefinchWebJobParams params = {
        .kind = TILEFINCH_WEB_JOB_CTR, .number = counter_bits,
        .key = key, .key_length = key_length, .a = counter, .a_length = 16u,
        .b = input, .b_length = length
    };
    return job_once(&params, output, NULL);
}

bool tilefinch_web_aes_gcm(bool encrypt, const uint8_t *key,
                           size_t key_length, const uint8_t *iv,
                           size_t iv_length, const uint8_t *aad,
                           size_t aad_length, const uint8_t *input,
                           size_t length, size_t tag_length,
                           uint8_t *output, size_t *output_length)
{
    if (output == NULL || output_length == NULL) return false;
    TilefinchWebJobParams params = {
        .kind = encrypt ? TILEFINCH_WEB_JOB_GCM_ENCRYPT
                        : TILEFINCH_WEB_JOB_GCM_DECRYPT,
        .number = tag_length, .key = key, .key_length = key_length,
        .a = iv, .a_length = iv_length, .b = aad, .b_length = aad_length,
        .c = input, .c_length = length
    };
    return job_once(&params, output, output_length);
}
