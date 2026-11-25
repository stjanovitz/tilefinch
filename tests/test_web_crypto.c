/* SubtleCrypto's secret-key primitives (src/web_crypto.c) against
   published vectors, then the bootstrap's crypto.subtle surface in a page
   and a dedicated worker.

   The same file builds twice: tilefinch-web-crypto-tests runs everything
   over the host's OpenSSL backend, and tilefinch-web-crypto-mbedtls-tests
   (TILEFINCH_WEB_CRYPTO_VECTORS_ONLY) compiles src/web_crypto.c against
   the pinned Mbed TLS the PSP links, so the device path runs the vectors
   too. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilefinch/web_crypto.h"

#if defined(TILEFINCH_WEB_CRYPTO_MBEDTLS)
#include <mbedtls/platform.h>
#endif

#if !defined(TILEFINCH_WEB_CRYPTO_VECTORS_ONLY)
#include "script_test_support.h"
#include "../src/js_runtime_internal.h"
#endif

static int failures;

#define CHECK(condition)                                                   \
    do {                                                                   \
        if (!(condition)) {                                                \
            fprintf(stderr, "CHECK failed: %s:%d: %s\n", __FILE__,         \
                    __LINE__, #condition);                                 \
            failures++;                                                    \
        }                                                                  \
    } while (0)

static size_t unhex(const char *text, uint8_t *output, size_t capacity)
{
    size_t length = strlen(text) / 2u;
    if (length > capacity) return 0;
    for (size_t i = 0; i < length; i++) {
        unsigned value = 0;
        sscanf(text + 2u * i, "%2x", &value);
        output[i] = (uint8_t) value;
    }
    return length;
}

static bool equals_hex(const uint8_t *bytes, size_t length, const char *hex)
{
    uint8_t wanted[256];
    size_t wanted_length = unhex(hex, wanted, sizeof(wanted));
    return wanted_length == length && memcmp(bytes, wanted, length) == 0;
}

#define TEXT(literal) (const uint8_t *) (literal), sizeof(literal) - 1u

#if defined(TILEFINCH_WEB_CRYPTO_MBEDTLS)
static size_t hash_allocations, hash_owned, hash_fail_at;

static void *test_hash_calloc(size_t count, size_t size)
{
    if (++hash_allocations == hash_fail_at) return NULL;
    void *p = calloc(count, size);
    if (p != NULL) hash_owned++;
    return p;
}

static void test_hash_free(void *p)
{
    if (p != NULL) {
        CHECK(hash_owned != 0);
        hash_owned--;
        free(p);
    }
}

static void test_hmac_setup_refusal(void)
{
    uint8_t output[64], zero[64] = {0};
    TilefinchWebPbkdf2State empty = {0};
    CHECK(mbedtls_platform_set_calloc_free(test_hash_calloc, test_hash_free)
          == 0);
    /* A short key exercises partial setup cleanly; no malformed inputs.
       HKDF's extract and expand share one MAC's three contexts. */
    for (hash_fail_at = 1; hash_fail_at <= 3; hash_fail_at++) {
        hash_allocations = 0;
        CHECK(!tilefinch_web_hmac(TILEFINCH_WEB_DIGEST_SHA256,
                TEXT("key"), TEXT("message"), output));
        CHECK(hash_allocations == hash_fail_at);
        CHECK(hash_owned == 0);

        TilefinchWebPbkdf2State state = {0};
        state.iterations = 1;
        state.u[0] = 0x5a;
        state.t[0] = 0x5a;
        memset(output, 0x5a, sizeof(output));
        hash_allocations = 0;
        CHECK(tilefinch_web_pbkdf2_slice(TILEFINCH_WEB_DIGEST_SHA256,
                TEXT("key"), TEXT("salt"), 2, output, sizeof(output),
                &state, NULL, NULL) == -1);
        CHECK(hash_allocations == hash_fail_at);
        CHECK(hash_owned == 0);
        CHECK(memcmp(output, zero, sizeof(output)) == 0);
        CHECK(memcmp(&state, &empty, sizeof(state)) == 0);
        /* A failed setup must not start extract. */
        memset(output, 0x5a, sizeof(output));
        hash_allocations = 0;
        CHECK(!tilefinch_web_hkdf(TILEFINCH_WEB_DIGEST_SHA256,
                TEXT("key"), TEXT("salt"), TEXT("info"),
                output, sizeof(output)));
        CHECK(hash_allocations == hash_fail_at);
        CHECK(hash_owned == 0);
        CHECK(memcmp(output, zero, sizeof(output)) == 0);
    }
    CHECK(mbedtls_platform_set_calloc_free(calloc, free) == 0);
}
#endif

static void test_hmac(void)
{
    uint8_t key[131], mac[64];
    memset(key, 0x0b, 20);
    /* RFC 4231 test case 1 and RFC 2202 test case 1. */
    CHECK(tilefinch_web_hmac(TILEFINCH_WEB_DIGEST_SHA256, key, 20,
                             TEXT("Hi There"), mac)
          && equals_hex(mac, 32, "b0344c61d8db38535ca8afceaf0bf12b"
                                 "881dc200c9833da726e9376c2e32cff7"));
    CHECK(tilefinch_web_hmac(TILEFINCH_WEB_DIGEST_SHA384, key, 20,
                             TEXT("Hi There"), mac)
          && equals_hex(mac, 48, "afd03944d84895626b0825f4ab46907f"
                                 "15f9dadbe4101ec682aa034c7cebc59c"
                                 "faea9ea9076ede7f4af152e8b2fa9cb6"));
    CHECK(tilefinch_web_hmac(TILEFINCH_WEB_DIGEST_SHA512, key, 20,
                             TEXT("Hi There"), mac)
          && equals_hex(mac, 64, "87aa7cdea5ef619d4ff0b4241a1d6cb0"
                                 "2379f4e2ce4ec2787ad0b30545e17cde"
                                 "daa833b7d6b8a702038b274eaea3f4e4"
                                 "be9d914eeb61f1702e696c203a126854"));
    CHECK(tilefinch_web_hmac(TILEFINCH_WEB_DIGEST_SHA1, key, 20,
                             TEXT("Hi There"), mac)
          && equals_hex(mac, 20, "b617318655057264e28bc0b6fb378c8ef146be00"));
    /* Test case 2: a key shorter than the hash. */
    CHECK(tilefinch_web_hmac(TILEFINCH_WEB_DIGEST_SHA256,
                             (const uint8_t *) "Jefe", 4,
                             TEXT("what do ya want for nothing?"), mac)
          && equals_hex(mac, 32, "5bdcc146bf60754e6a042426089575c7"
                                 "5a003f089d2739839dec58b964ec3843"));
    /* Test case 6: a key longer than the block is hashed first. */
    memset(key, 0xaa, sizeof(key));
    CHECK(tilefinch_web_hmac(
              TILEFINCH_WEB_DIGEST_SHA256, key, sizeof(key),
              TEXT("Test Using Larger Than Block-Size Key - Hash Key First"),
              mac)
          && equals_hex(mac, 32, "60e431591ee0b67f0d8a26aacbf5b77f"
                                 "8e0bc6213728c5140546040f0ee37f54"));
    CHECK(tilefinch_web_hmac(
              TILEFINCH_WEB_DIGEST_SHA512, key, sizeof(key),
              TEXT("Test Using Larger Than Block-Size Key - Hash Key First"),
              mac)
          && equals_hex(mac, 64, "80b24263c7c1a3ebb71493c1dd7be8b4"
                                 "9b46d1f41b4aeec1121b013783f8f352"
                                 "6b56d037e05f2598bd0fd2215d6a1e52"
                                 "95e64f73f63f0aec8b915a985d786598"));
    CHECK(!tilefinch_web_hmac((TilefinchWebDigest) 7, key, 20,
                              TEXT("x"), mac));
}

/* Stops a sliced derivation at each chance: one poll per slice. */
static bool pbkdf2_stop(void *context)
{
    (*(unsigned *) context)++;
    return false;
}

/* PBKDF2 run in one slice and in slices of 16 HMACs: both runs must
   succeed or fail together and give the same bytes. */
static bool tilefinch_web_pbkdf2(TilefinchWebDigest hash,
                                 const uint8_t *password,
                                 size_t password_length,
                                 const uint8_t *salt, size_t salt_length,
                                 uint32_t iterations, uint8_t *output,
                                 size_t output_length)
{
    TilefinchWebPbkdf2State state = {0};
    int whole = tilefinch_web_pbkdf2_slice(
        hash, password, password_length, salt, salt_length, iterations,
        output, output_length, &state, NULL, NULL);
    static uint8_t sliced[64 * 1024];
    if (output_length > sizeof(sliced)) return false;
    memset(&state, 0, sizeof(state));
    unsigned slices = 0;
    int status = 0;
    while (status == 0 && slices < 2000000u)
        status = tilefinch_web_pbkdf2_slice(
            hash, password, password_length, salt, salt_length, iterations,
            sliced, output_length, &state, pbkdf2_stop, &slices);
    if ((whole == 1) != (status == 1)) return false;
    if (whole != 1) return false;
    /* More than one slice whenever the work passes one slice. */
    size_t hash_length = tilefinch_web_hash_length(hash);
    size_t work = (output_length + hash_length - 1u) / hash_length
        * (size_t) iterations;
    if (work > 16u && slices < 2u) return false;
    return memcmp(output, sliced, output_length) == 0;
}

static void test_pbkdf2(void)
{
    uint8_t output[32];
    /* RFC 6070. */
    static const struct {
        uint32_t iterations;
        const char *hex;
    } sha1[] = {
        {1, "0c60c80f961f0e71f3a9b524af6012062fe037a6"},
        {2, "ea6c014dc72d6f8ccd1ed92ace1d41f0d8de8957"},
        {4096, "4b007901b765489abead49d926f721d065a429c1"},
    };
    for (size_t i = 0; i < sizeof(sha1) / sizeof(sha1[0]); i++) {
        CHECK(tilefinch_web_pbkdf2(TILEFINCH_WEB_DIGEST_SHA1,
                                   TEXT("password"), TEXT("salt"),
                                   sha1[i].iterations, output, 20)
              && equals_hex(output, 20, sha1[i].hex));
    }
    CHECK(tilefinch_web_pbkdf2(TILEFINCH_WEB_DIGEST_SHA1,
                               TEXT("passwordPASSWORDpassword"),
                               TEXT("saltSALTsaltSALTsaltSALTsaltSALTsalt"),
                               4096, output, 25)
          && equals_hex(output, 25, "3d2eec4fe41c849b80c8d83662c0e44a"
                                    "8b291a964cf2f07038"));
    CHECK(tilefinch_web_pbkdf2(TILEFINCH_WEB_DIGEST_SHA1,
                               TEXT("pass\0word"), TEXT("sa\0lt"), 4096,
                               output, 16)
          && equals_hex(output, 16, "56fa6aa75548099dcc37d7f03425e0c3"));
    CHECK(tilefinch_web_pbkdf2(TILEFINCH_WEB_DIGEST_SHA256,
                               TEXT("password"), TEXT("salt"), 4096,
                               output, 32)
          && equals_hex(output, 32, "c5e478d59288c841aa530db6845c4c8d"
                                    "962893a001ce4e11a4963873aa98134a"));
    CHECK(!tilefinch_web_pbkdf2(TILEFINCH_WEB_DIGEST_SHA256,
                                TEXT("password"), TEXT("salt"), 0,
                                output, 32));
    /* The HMAC count is bounded: the iteration loop cannot be
       interrupted. */
    CHECK(!tilefinch_web_pbkdf2(TILEFINCH_WEB_DIGEST_SHA256,
                                TEXT("password"), TEXT("salt"),
                                TILEFINCH_WEB_CRYPTO_PBKDF2_WORK_LIMIT + 1u,
                                output, 32));
}

static void test_hkdf(void)
{
    uint8_t ikm[22], salt[13], info[10], output[42];
    memset(ikm, 0x0b, sizeof(ikm));
    for (size_t i = 0; i < sizeof(salt); i++) salt[i] = (uint8_t) i;
    for (size_t i = 0; i < sizeof(info); i++) info[i] = (uint8_t) (0xf0 + i);
    /* RFC 5869 test cases 1, 3 and 4. */
    CHECK(tilefinch_web_hkdf(TILEFINCH_WEB_DIGEST_SHA256, ikm, 22, salt, 13,
                             info, 10, output, 42)
          && equals_hex(output, 42, "3cb25f25faacd57a90434f64d0362f2a"
                                    "2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
                                    "34007208d5b887185865"));
    CHECK(tilefinch_web_hkdf(TILEFINCH_WEB_DIGEST_SHA256, ikm, 22, NULL, 0,
                             NULL, 0, output, 42)
          && equals_hex(output, 42, "8da4e775a563c18f715f802a063c5a31"
                                    "b8a11f5c5ee1879ec3454e5f3c738d2d"
                                    "9d201395faa4b61a96c8"));
    CHECK(tilefinch_web_hkdf(TILEFINCH_WEB_DIGEST_SHA1, ikm, 11, salt, 13,
                             info, 10, output, 42)
          && equals_hex(output, 42, "085a01ea1b10f36933068b56efa5ad81"
                                    "a4f14b822f5b091568a9cdd4f155fda2"
                                    "c22e422478d305f3f896"));
    static uint8_t large[255u * 20u + 1u];
    CHECK(tilefinch_web_hkdf(TILEFINCH_WEB_DIGEST_SHA1, ikm, 22, NULL, 0,
                             NULL, 0, large, 255u * 20u));
    CHECK(!tilefinch_web_hkdf(TILEFINCH_WEB_DIGEST_SHA1, ikm, 22, NULL, 0,
                              NULL, 0, large, sizeof(large)));
    /* Test case 2: 80-byte inputs. */
    uint8_t ikm80[80], salt80[80], info80[80];
    for (size_t i = 0; i < 80; i++) {
        ikm80[i] = (uint8_t) i;
        salt80[i] = (uint8_t) (0x60 + i);
        info80[i] = (uint8_t) (0xb0 + i);
    }
    CHECK(tilefinch_web_hkdf(TILEFINCH_WEB_DIGEST_SHA256, ikm80, 80, salt80,
                             80, info80, 80, large, 82)
          && equals_hex(large, 82, "b11e398dc80327a1c8e7f78c596a4934"
                                   "4f012eda2d4efad8a050cc4c19afa97c"
                                   "59045a99cac7827271cb41c65e590e09"
                                   "da3275600c2f09b8367793a9aca3db71"
                                   "cc30c58179ec3e87c14c01d5c1f3434f"
                                   "1d87"));
}

/* HKDF's work limit, at its edges. With a 32-byte key and no salt,
   SHA-512 (its 128-byte blocks counting twice) admits all 255 output
   blocks with an info of up to 65,454 bytes, and one block with an info of
   up to 16,776,302 bytes. */
static void test_hkdf_work_limit(void)
{
    static uint8_t info[65455u];
    uint8_t key[32], one = 0;
    static uint8_t output[255u * 64u];
    uint8_t *large = malloc(16776303u);
    CHECK(large != NULL);
    if (large == NULL) return;
    memset(key, 0x0b, sizeof(key));
    memset(info, 0xf0, sizeof(info));
    memset(large, 0xf0, 16776303u);
    CHECK(tilefinch_web_hkdf(TILEFINCH_WEB_DIGEST_SHA512, key, 32, NULL, 0,
                             info, 65454u, output, sizeof(output)));
    CHECK(!tilefinch_web_hkdf(TILEFINCH_WEB_DIGEST_SHA512, key, 32, NULL, 0,
                              info, 65455u, output, sizeof(output)));
    CHECK(tilefinch_web_hkdf(TILEFINCH_WEB_DIGEST_SHA512, key, 32, NULL, 0,
                             large, 16776302u, output, 64));
    CHECK(!tilefinch_web_hkdf(TILEFINCH_WEB_DIGEST_SHA512, key, 32, NULL, 0,
                              large, 16776303u, output, 64));
    free(large);
    /* A refusal hashes nothing and writes nothing. */
    memset(output, 0x5a, sizeof(output));
    CHECK(!tilefinch_web_hkdf(TILEFINCH_WEB_DIGEST_SHA512, key, 32, NULL, 0,
                              info, 65455u, output, sizeof(output)));
    CHECK(output[0] == 0x5a && output[sizeof(output) - 1u] == 0x5a);
    /* Lengths at the ends of size_t are refused before any is read: the
       one-byte buffers below would be overrun otherwise. */
    const size_t extremes[] = {
        SIZE_MAX, SIZE_MAX - 1u, SIZE_MAX / 2u + 1u,
        TILEFINCH_WEB_CRYPTO_HKDF_WORK_LIMIT + 1u
    };
    for (size_t i = 0; i < sizeof(extremes) / sizeof(extremes[0]); i++) {
        size_t length = extremes[i];
        for (int hash = 0; hash < 4; hash++) {
            TilefinchWebDigest digest = (TilefinchWebDigest) hash;
            CHECK(!tilefinch_web_hkdf(digest, &one, length, NULL, 0, NULL, 0,
                                      output, 32));
            CHECK(!tilefinch_web_hkdf(digest, key, 32, &one, length, NULL, 0,
                                      output, 32));
            CHECK(!tilefinch_web_hkdf(digest, key, 32, NULL, 0, &one, length,
                                      output, 32));
        }
    }
}

static const char nist_plain[] =
    "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
    "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710";

static void test_aes(void)
{
    uint8_t key[16], iv[16], plain[64], sealed[96], opened[96];
    size_t length = 0;
    unhex("2b7e151628aed2a6abf7158809cf4f3c", key, sizeof(key));
    unhex(nist_plain, plain, sizeof(plain));
    /* SP 800-38A F.2.1, then the PKCS#7 block WebCrypto appends. */
    unhex("000102030405060708090a0b0c0d0e0f", iv, sizeof(iv));
    CHECK(tilefinch_web_aes_cbc(true, key, 16, iv, plain, 64, sealed,
                                &length)
          && equals_hex(sealed, length,
                        "7649abac8119b246cee98e9b12e9197d"
                        "5086cb9b507219ee95db113a917678b2"
                        "73bed6b8e3c1743b7116e69e22229516"
                        "3ff1caa1681fac09120eca307586e1a7"
                        "8cb82807230e1321d3fae00d18cc2012"));
    CHECK(tilefinch_web_aes_cbc(false, key, 16, iv, sealed, 80, opened,
                                &length)
          && length == 64 && memcmp(opened, plain, 64) == 0);
    CHECK(tilefinch_web_aes_cbc(true, key, 16, iv, plain, 5, sealed,
                                &length) && length == 16
          && tilefinch_web_aes_cbc(false, key, 16, iv, sealed, 16, opened,
                                   &length)
          && length == 5 && memcmp(opened, plain, 5) == 0);
    /* Without the padding block the last byte is not valid PKCS#7. */
    CHECK(tilefinch_web_aes_cbc(true, key, 16, iv, plain, 64, sealed,
                                &length));
    CHECK(!tilefinch_web_aes_cbc(false, key, 16, iv, sealed, 64, opened,
                                 &length));
    CHECK(!tilefinch_web_aes_cbc(false, key, 16, iv, sealed, 70, opened,
                                 &length));
    CHECK(!tilefinch_web_aes_cbc(true, key, 15, iv, plain, 16, sealed,
                                 &length));
    /* SP 800-38A F.5.1. */
    uint8_t counter[16];
    unhex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff", counter, sizeof(counter));
    CHECK(tilefinch_web_aes_ctr(key, 16, counter, 128, plain, 64, sealed)
          && equals_hex(sealed, 64,
                        "874d6191b620e3261bef6864990db6ce"
                        "9806f66b7970fdff8617187bb9fffdff"
                        "5ae4df3edbd5d35e5b4f09020db03eab"
                        "1e031dda2fbe03d1792170a0f3009cee"));
    /* An 8-bit counter wraps ff -> 00 without carrying. */
    uint8_t zeros[32] = {0}, wrapped[32], direct[16];
    uint8_t wrapped_counter[16];
    memcpy(wrapped_counter, counter, 16);
    wrapped_counter[15] = 0x00;
    CHECK(tilefinch_web_aes_ctr(key, 16, counter, 8, zeros, 32, wrapped)
          && tilefinch_web_aes_ctr(key, 16, wrapped_counter, 128, zeros, 16,
                                   direct)
          && memcmp(wrapped + 16, direct, 16) == 0);
    CHECK(tilefinch_web_aes_ctr(key, 16, counter, 1, zeros, 32, wrapped));
    CHECK(!tilefinch_web_aes_ctr(key, 16, counter, 1, plain, 33, sealed));
    CHECK(!tilefinch_web_aes_ctr(key, 16, counter, 0, plain, 16, sealed));
    CHECK(!tilefinch_web_aes_ctr(key, 16, counter, 129, plain, 16, sealed));
    /* The GCM specification's test cases 2 and 4. */
    uint8_t zero_key[16] = {0}, zero_iv[12] = {0}, gcm_key[16], gcm_iv[12],
        aad[20], gcm_plain[60];
    CHECK(tilefinch_web_aes_gcm(true, zero_key, 16, zero_iv, 12, NULL, 0,
                                zeros, 16, 16, sealed, &length)
          && equals_hex(sealed, length, "0388dace60b6a392f328c2b971b2fe78"
                                        "ab6e47d42cec13bdf53a67b21257bddf"));
    unhex("feffe9928665731c6d6a8f9467308308", gcm_key, sizeof(gcm_key));
    unhex("cafebabefacedbaddecaf888", gcm_iv, sizeof(gcm_iv));
    unhex("feedfacedeadbeeffeedfacedeadbeefabaddad2", aad, sizeof(aad));
    unhex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
          "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39",
          gcm_plain, sizeof(gcm_plain));
    static const char gcm_sealed[] =
        "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e"
        "21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091"
        "5bc94fbc3221a5db94fae95ae7121a47";
    CHECK(tilefinch_web_aes_gcm(true, gcm_key, 16, gcm_iv, 12, aad, 20,
                                gcm_plain, 60, 16, sealed, &length)
          && equals_hex(sealed, length, gcm_sealed));
    CHECK(tilefinch_web_aes_gcm(false, gcm_key, 16, gcm_iv, 12, aad, 20,
                                sealed, 76, 16, opened, &length)
          && length == 60 && memcmp(opened, gcm_plain, 60) == 0);
    /* A shorter tag is the full tag's prefix. */
    CHECK(tilefinch_web_aes_gcm(true, gcm_key, 16, gcm_iv, 12, aad, 20,
                                gcm_plain, 60, 12, opened, &length)
          && length == 72 && memcmp(opened, sealed, 72) == 0);
    sealed[70] ^= 1;
    CHECK(!tilefinch_web_aes_gcm(false, gcm_key, 16, gcm_iv, 12, aad, 20,
                                 sealed, 76, 16, opened, &length));
    CHECK(!tilefinch_web_aes_gcm(true, gcm_key, 16, gcm_iv, 0, aad, 20,
                                 gcm_plain, 60, 16, sealed, &length));
    CHECK(!tilefinch_web_aes_gcm(true, gcm_key, 16, gcm_iv, 12, aad, 20,
                                 gcm_plain, 60, 10, sealed, &length));
}

static void test_aes_message_limits(void)
{
    const size_t limit = TILEFINCH_WEB_CRYPTO_INPUT_LIMIT;
    const uint8_t key[16] = {0}, iv[16] = {0};
    uint8_t *plain = calloc(limit + 32u, 1u);
    uint8_t *sealed = calloc(limit + 32u, 1u);
    uint8_t *opened = malloc(limit + 32u);
    CHECK(plain != NULL && sealed != NULL && opened != NULL);
    if (plain == NULL || sealed == NULL || opened == NULL) goto done;
    plain[0] = 1;
    plain[limit - 1u] = 2;
    size_t length = 0;
    CHECK(tilefinch_web_aes_cbc(true, key, sizeof(key), iv, plain, limit,
                               sealed, &length) && length == limit + 16u);
    CHECK(tilefinch_web_aes_cbc(false, key, sizeof(key), iv, sealed,
                               limit + 16u, opened, &length)
          && length == limit && memcmp(plain, opened, limit) == 0);
    /* Change every last-block 0x10 to 0x0f via CBC's preceding block.
       The padding is now valid for a plaintext one byte over the limit. */
    for (size_t i = limit - 16u; i < limit; i++) sealed[i] ^= 0x1fu;
    CHECK(!tilefinch_web_aes_cbc(false, key, sizeof(key), iv, sealed,
                                limit + 16u, opened, &length));
    CHECK(!tilefinch_web_aes_cbc(false, key, sizeof(key), iv, sealed,
                                limit + 32u, opened, &length));
    CHECK(!tilefinch_web_aes_cbc(true, key, sizeof(key), iv, plain,
                                limit + 1u, sealed, &length));
    for (size_t tag = 4u; tag <= 16u; tag += 12u) {
        CHECK(tilefinch_web_aes_gcm(true, key, sizeof(key), iv, 12u, NULL,
                                   0, plain, limit, tag, sealed, &length)
              && length == limit + tag);
        CHECK(tilefinch_web_aes_gcm(false, key, sizeof(key), iv, 12u, NULL,
                                   0, sealed, limit + tag, tag, opened,
                                   &length)
              && length == limit && memcmp(plain, opened, limit) == 0);
        CHECK(!tilefinch_web_aes_gcm(false, key, sizeof(key), iv, 12u, NULL,
                                    0, sealed, limit + tag + 1u, tag, opened,
                                    &length));
        CHECK(!tilefinch_web_aes_gcm(true, key, sizeof(key), iv, 12u, NULL,
                                    0, plain, limit + 1u, tag, sealed,
                                    &length));
    }
    CHECK(!tilefinch_web_aes_gcm(false, key, sizeof(key), plain, limit + 1u,
                                NULL, 0, sealed, limit + 16u, 16u, opened,
                                &length));
    CHECK(!tilefinch_web_aes_gcm(false, key, sizeof(key), iv, 12u, plain,
                                limit + 1u, sealed, limit + 16u, 16u, opened,
                                &length));
    CHECK(!tilefinch_web_aes_ctr(key, sizeof(key), iv, 128u, plain,
                                limit + 1u, opened));
done:
    free(opened);
    free(sealed);
    free(plain);
}

/* Refuses every poll, so each run of a job feeds one chunk. */
static bool job_stop(void *context)
{
    (*(unsigned *) context)++;
    return false;
}

/* Runs params as a job to the end, one chunk a run when sliced; runs
   counts the runs it took. */
static bool run_job(const TilefinchWebJobParams *params, bool sliced,
                    uint8_t *output, size_t *written, unsigned *runs)
{
    TilefinchWebJob *job = malloc(tilefinch_web_job_size());
    if (job == NULL) return false;
    bool ok = tilefinch_web_job_begin(job, params);
    int status = 0;
    unsigned count = 0, polls = 0;
    while (ok && status == 0 && count < 1000000u) {
        status = tilefinch_web_job_run(job, output, sliced ? job_stop : NULL,
                                       &polls);
        count++;
    }
    ok = ok && status == 1;
    *written = ok ? tilefinch_web_job_length(job) : 0u;
    tilefinch_web_job_end(job);
    free(job);
    if (runs != NULL) *runs = count;
    return ok;
}

/* The SHA-256 of bytes, in hex, by a whole digest job. */
static void sha256_hex(const uint8_t *bytes, size_t length, char hex[65])
{
    TilefinchWebJobParams params = {
        .kind = TILEFINCH_WEB_JOB_DIGEST, .hash = TILEFINCH_WEB_DIGEST_SHA256,
        .a = bytes, .a_length = length
    };
    uint8_t digest[32];
    size_t written = 0;
    hex[0] = '\0';
    if (!run_job(&params, false, digest, &written, NULL) || written != 32)
        return;
    for (size_t i = 0; i < 32; i++)
        snprintf(hex + 2u * i, 3, "%02x", digest[i]);
}

/* One chunk a run gives the bytes one run gives, in more than one run
   whenever the input passes a chunk; wanted (when given) is the SHA-256 of
   the output that main's one-call code gave for the same input. */
static bool sliced_matches(const TilefinchWebJobParams *params,
                           const char *wanted, size_t chunk)
{
    size_t capacity = 0, work = 0, whole_length = 0, sliced_length = 0;
    if (!tilefinch_web_job_plan(params, &capacity, &work)) return false;
    uint8_t *whole = malloc(capacity + 1u), *sliced = malloc(capacity + 1u);
    unsigned runs = 0;
    bool ok = whole != NULL && sliced != NULL
        && run_job(params, false, whole, &whole_length, NULL)
        && run_job(params, true, sliced, &sliced_length, &runs)
        && whole_length == sliced_length
        && memcmp(whole, sliced, whole_length) == 0;
    size_t input = params->a_length + params->b_length + params->c_length
        + params->key_length;
    if (ok && input > 2u * chunk + 64u && runs < 2u) ok = false;
    if (ok && wanted != NULL) {
        char hex[65];
        sha256_hex(whole, whole_length, hex);
        ok = strcmp(hex, wanted) == 0;
        if (!ok) fprintf(stderr, "sliced kind=%d: %s\n", params->kind, hex);
    }
    free(whole);
    free(sliced);
    return ok;
}

/* Jobs whose input crosses many chunks, against main's one-call output
   (computed with the OpenSSL build of src/web_crypto.c at 0c0bcedb0),
   independent HKDF (OpenSSL's EVP_KDF) and the FIPS 180-2 million-'a'
   digests. Each also runs one chunk at a time. */
static void test_sliced_jobs(void)
{
    const size_t n = 100003u;
    uint8_t *in = malloc(n + 64u), *sealed = malloc(n + 64u),
        *million = malloc(1000000u);
    CHECK(in != NULL && sealed != NULL && million != NULL);
    if (in == NULL || sealed == NULL || million == NULL) goto done;
    for (size_t i = 0; i < n + 64u; i++) in[i] = (uint8_t) (i * 7u + 3u);
    memset(million, 'a', 1000000u);
    uint8_t key[32], iv[16];
    for (int i = 0; i < 32; i++) key[i] = (uint8_t) (0xa0 + i);
    for (int i = 0; i < 16; i++) iv[i] = (uint8_t) (0x10 * i + 1);
    TilefinchWebJobParams gcm = {
        .kind = TILEFINCH_WEB_JOB_GCM_ENCRYPT, .number = 16u,
        .key = key, .key_length = 32u, .a = iv, .a_length = 12u,
        .b = in + 7, .b_length = 5003u, .c = in, .c_length = n
    };
    CHECK(sliced_matches(&gcm, "0308a932cfcdf9c979111cd69f170eb4"
                         "65757a9828fdb28606c671b10e9b38e3", 1024u));
    /* A 61-byte IV is hashed; no additional data. */
    TilefinchWebJobParams gcm_iv = {
        .kind = TILEFINCH_WEB_JOB_GCM_ENCRYPT, .number = 12u,
        .key = key, .key_length = 16u, .a = in, .a_length = 61u,
        .c = in, .c_length = n
    };
    CHECK(sliced_matches(&gcm_iv, "9370b5945af9a9d656d09b25193b6acc"
                         "3d51c0c73cf41b734fd4354b1e96edfd", 1024u));
    /* Decrypting the first opens it; a changed tag byte fails and zeroes
       the output. */
    size_t length = 0, opened_length = 0;
    CHECK(run_job(&gcm, true, sealed, &length, NULL) && length == n + 16u);
    TilefinchWebJobParams open = gcm;
    open.kind = TILEFINCH_WEB_JOB_GCM_DECRYPT;
    open.c = sealed;
    open.c_length = length;
    uint8_t *opened = malloc(n + 64u);
    CHECK(opened != NULL);
    if (opened != NULL) {
        CHECK(run_job(&open, true, opened, &opened_length, NULL)
              && opened_length == n && memcmp(opened, in, n) == 0);
        sealed[n + 15u] ^= 1u;
        memset(opened, 0x5a, n);
        CHECK(!run_job(&open, true, opened, &opened_length, NULL)
              && opened[0] == 0 && opened[n - 1u] == 0);
        sealed[n + 15u] ^= 1u;
    }
    TilefinchWebJobParams cbc = {
        .kind = TILEFINCH_WEB_JOB_CBC_ENCRYPT, .key = key, .key_length = 24u,
        .a = iv, .a_length = 16u, .b = in, .b_length = n
    };
    CHECK(sliced_matches(&cbc, "18be47160114b40b2d7291e643c7797b"
                         "a106624a6d075aa9ab56b498a67da3c9", 4096u));
    CHECK(run_job(&cbc, true, sealed, &length, NULL)
          && length == n - n % 16u + 16u);
    TilefinchWebJobParams unpad = cbc;
    unpad.kind = TILEFINCH_WEB_JOB_CBC_DECRYPT;
    unpad.b = sealed;
    unpad.b_length = length;
    if (opened != NULL)
        CHECK(run_job(&unpad, true, opened, &opened_length, NULL)
              && opened_length == n && memcmp(opened, in, n) == 0);
    free(opened);
    TilefinchWebJobParams ctr = {
        .kind = TILEFINCH_WEB_JOB_CTR, .number = 20u, .key = key,
        .key_length = 32u, .a = iv, .a_length = 16u, .b = in, .b_length = n
    };
    CHECK(sliced_matches(&ctr, "71b51898eb75781d1da9560a99d297a6"
                         "ac8171adc5197e0f3ac837e40a56de4e", 4096u));
    static const char *const macs[] = {
        "532be09159a2a84cfdeeee3c5be775b749b3127200af2feda1eaa015c06b5581",
        "db79687282b73e3bdc61ebea27be86b145cd862ab2e34fb306ea3ea97c552c16",
        "267bfcf504a66ee438d49ffa809a324e645611c0f4a86cd9a396959d20984d00",
        "8fe80da1e1b96f4039de1f941fe3ecc741e4fb7766c36c006c537ecdc0e59308",
    };
    /* RFC 5869 HKDF at its full 255 blocks: key, salt and info are the
       pattern's first 100, next 200 and next 1,000 bytes. */
    static const char *const hkdfs[] = {
        "370695d2cb9738393f07f1c711aa968ca7b53a82fd3d4c0b04637de6f174675d",
        "77490db16b52b52dfcb85a8d575796f43489803f3a3f18dab3ee78a4b5ae4fec",
        "2ac93d8d0c80e73c9ad177a0175448b2a01929ffa9182ed7eee67dac94dd3c7f",
        "98b1a5e1da87089fe3106387613c838495b399c3bec434f31a91b7f427dd975d",
    };
    static const char *const digests[] = {
        "34aa973cd4c4daa4f61eeb2bdbad27316534016f",
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
        "9d0e1809716474cb086e834e310a4a1ced149e9c00f248527972cec5704c2a5b"
        "07b8b3dc38ecc4ebae97ddd87f3d8985",
        "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973eb"
        "de0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b",
    };
    for (int h = 0; h < 4; h++) {
        TilefinchWebDigest hash = (TilefinchWebDigest) h;
        size_t hash_length = tilefinch_web_hash_length(hash);
        /* A 200-byte key is hashed first for SHA-1 and SHA-256. */
        TilefinchWebJobParams mac = {
            .kind = TILEFINCH_WEB_JOB_HMAC, .hash = hash, .key = in + 1,
            .key_length = 200u, .a = in, .a_length = n
        };
        CHECK(sliced_matches(&mac, macs[h], 4096u));
        TilefinchWebJobParams hkdf = {
            .kind = TILEFINCH_WEB_JOB_HKDF, .hash = hash,
            .number = 255u * hash_length, .key = in, .key_length = 100u,
            .a = in + 100, .a_length = 200u, .b = in + 300, .b_length = 1000u
        };
        CHECK(sliced_matches(&hkdf, hkdfs[h], 4096u));
        /* The one-call form agrees. */
        uint8_t *derived = malloc(255u * 64u);
        char hex[65];
        CHECK(derived != NULL && tilefinch_web_hkdf(hash, in, 100u, in + 100,
              200u, in + 300, 1000u, derived, 255u * hash_length));
        if (derived != NULL) {
            sha256_hex(derived, 255u * hash_length, hex);
            CHECK(strcmp(hex, hkdfs[h]) == 0);
        }
        free(derived);
        TilefinchWebJobParams digest = {
            .kind = TILEFINCH_WEB_JOB_DIGEST, .hash = hash, .a = million,
            .a_length = 1000000u
        };
        uint8_t value[64];
        size_t written = 0;
        unsigned runs = 0;
        CHECK(run_job(&digest, true, value, &written, &runs)
              && written == hash_length && runs > 200u
              && equals_hex(value, written, digests[h]));
    }
    /* Every kind, at sizes around the chunk and AES block edges. */
    static const size_t sizes[] = {
        0, 1, 15, 16, 17, 1023, 1024, 1025, 4095, 4096, 4097, 8193
    };
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        size_t size = sizes[i];
        TilefinchWebJobParams cases[] = {
            {.kind = TILEFINCH_WEB_JOB_DIGEST,
             .hash = TILEFINCH_WEB_DIGEST_SHA384, .a = in, .a_length = size},
            {.kind = TILEFINCH_WEB_JOB_HMAC, .hash = TILEFINCH_WEB_DIGEST_SHA1,
             .key = in, .key_length = size, .a = in, .a_length = size},
            {.kind = TILEFINCH_WEB_JOB_GCM_ENCRYPT, .number = 4u, .key = key,
             .key_length = 16u, .a = iv, .a_length = 12u, .b = in,
             .b_length = size, .c = in + 3, .c_length = size},
            {.kind = TILEFINCH_WEB_JOB_CBC_ENCRYPT, .key = key,
             .key_length = 32u, .a = iv, .a_length = 16u, .b = in,
             .b_length = size},
            {.kind = TILEFINCH_WEB_JOB_CTR, .number = 128u, .key = key,
             .key_length = 16u, .a = iv, .a_length = 16u, .b = in,
             .b_length = size},
            {.kind = TILEFINCH_WEB_JOB_HKDF,
             .hash = TILEFINCH_WEB_DIGEST_SHA256, .number = 255u * 32u,
             .key = in, .key_length = size, .a = in, .a_length = size,
             .b = in, .b_length = size},
        };
        for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
            if (!sliced_matches(&cases[c], NULL,
                                cases[c].kind == TILEFINCH_WEB_JOB_GCM_ENCRYPT
                                ? 1024u : 4096u)) {
                fprintf(stderr, "sliced size=%zu kind=%d differs\n", size,
                        cases[c].kind);
                failures++;
            }
        }
    }
done:
    free(million);
    free(sealed);
    free(in);
}

/* Work over a slice is what the bindings run as a job: the plans place
   the edges where the header's units say. */
static void test_job_plan(void)
{
    uint8_t key[16] = {0}, iv[16] = {0};
    static uint8_t data[65536];
    size_t capacity = 0, work = 0;
    TilefinchWebJobParams gcm = {
        .kind = TILEFINCH_WEB_JOB_GCM_ENCRYPT, .number = 16u, .key = key,
        .key_length = 16u, .a = iv, .a_length = 12u, .c = data,
        .c_length = TILEFINCH_WEB_CRYPTO_SLICE_WORK / 11u
    };
    CHECK(tilefinch_web_job_plan(&gcm, &capacity, &work)
          && work <= TILEFINCH_WEB_CRYPTO_SLICE_WORK
          && capacity == gcm.c_length + 16u);
    gcm.c_length++;
    CHECK(tilefinch_web_job_plan(&gcm, &capacity, &work)
          && work > TILEFINCH_WEB_CRYPTO_SLICE_WORK);
    TilefinchWebJobParams ctr = {
        .kind = TILEFINCH_WEB_JOB_CTR, .number = 128u, .key = key,
        .key_length = 16u, .a = iv, .a_length = 16u, .b = data,
        .b_length = TILEFINCH_WEB_CRYPTO_SLICE_WORK / 2u
    };
    CHECK(tilefinch_web_job_plan(&ctr, &capacity, &work)
          && work == TILEFINCH_WEB_CRYPTO_SLICE_WORK);
    /* Refusals: a bad tag length, a CBC IV that is not 16 bytes, a
       ciphertext shorter than its tag, an unknown hash. */
    gcm.number = 10u;
    CHECK(!tilefinch_web_job_plan(&gcm, &capacity, &work));
    TilefinchWebJobParams cbc = {
        .kind = TILEFINCH_WEB_JOB_CBC_ENCRYPT, .key = key, .key_length = 16u,
        .a = iv, .a_length = 12u, .b = data, .b_length = 16u
    };
    CHECK(!tilefinch_web_job_plan(&cbc, &capacity, &work));
    TilefinchWebJobParams open = {
        .kind = TILEFINCH_WEB_JOB_GCM_DECRYPT, .number = 16u, .key = key,
        .key_length = 16u, .a = iv, .a_length = 12u, .c = data,
        .c_length = 15u
    };
    CHECK(!tilefinch_web_job_plan(&open, &capacity, &work));
    TilefinchWebJobParams digest = {
        .kind = TILEFINCH_WEB_JOB_DIGEST, .hash = (TilefinchWebDigest) 4,
        .a = data, .a_length = 1u
    };
    CHECK(!tilefinch_web_job_plan(&digest, &capacity, &work));
}

#if defined(TILEFINCH_WEB_CRYPTO_MBEDTLS)
/* Mbed TLS takes a job's hash and GCM contexts from the C heap, outside
   the script budget, so a job opens them on its first run: any number of
   jobs waiting their turn hold none. A refusal there fails the run, and
   end frees what it opened. */
static void test_job_contexts_on_first_run(void)
{
    enum { JOBS = 64 };
    static uint8_t data[20000], output[20000 + 16];
    uint8_t key[32] = {1}, iv[12] = {2};
    const TilefinchWebJobParams kinds[] = {
        {.kind = TILEFINCH_WEB_JOB_HKDF, .hash = TILEFINCH_WEB_DIGEST_SHA512,
         .number = 255u * 64u, .key = key, .key_length = 32u},
        {.kind = TILEFINCH_WEB_JOB_HMAC, .hash = TILEFINCH_WEB_DIGEST_SHA256,
         .key = data, .key_length = 200u, .a = data, .a_length = 20000u},
        {.kind = TILEFINCH_WEB_JOB_DIGEST, .hash = TILEFINCH_WEB_DIGEST_SHA1,
         .a = data, .a_length = 20000u},
        {.kind = TILEFINCH_WEB_JOB_GCM_ENCRYPT, .number = 16u, .key = key,
         .key_length = 32u, .a = iv, .a_length = 12u, .c = data,
         .c_length = 20000u},
    };
    TilefinchWebJob *jobs[JOBS] = {0};
    CHECK(mbedtls_platform_set_calloc_free(test_hash_calloc, test_hash_free)
          == 0);
    for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
        hash_allocations = hash_owned = hash_fail_at = 0;
        for (size_t i = 0; i < JOBS; i++) {
            jobs[i] = malloc(tilefinch_web_job_size());
            CHECK(jobs[i] != NULL
                  && tilefinch_web_job_begin(jobs[i], &kinds[k]));
        }
        CHECK(hash_allocations == 0 && hash_owned == 0);
        unsigned polls = 0;
        CHECK(tilefinch_web_job_run(jobs[0], output, job_stop, &polls) == 0);
        size_t opened = hash_owned;
        CHECK(opened != 0);
        tilefinch_web_job_end(jobs[0]);
        CHECK(hash_owned == 0);
        /* The first allocation refused: the run fails with the output
           zeroed, and end leaves nothing behind. */
        hash_allocations = 0;
        hash_fail_at = 1;
        memset(output, 0x5a, sizeof(output));
        CHECK(tilefinch_web_job_run(jobs[1], output, job_stop, &polls) == -1);
        CHECK(output[0] == 0 && hash_owned == 0);
        CHECK(tilefinch_web_job_run(jobs[1], output, NULL, NULL) == -1);
        hash_fail_at = 0;
        for (size_t i = 0; i < JOBS; i++) {
            if (jobs[i] != NULL) tilefinch_web_job_end(jobs[i]);
            free(jobs[i]);
            jobs[i] = NULL;
        }
        CHECK(hash_owned == 0);
    }
    CHECK(mbedtls_platform_set_calloc_free(calloc, free) == 0);
}
#endif

#if !defined(TILEFINCH_WEB_CRYPTO_VECTORS_ONLY)
static char *read_fixture(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return NULL;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    char *text = size > 0 ? malloc((size_t) size + 1u) : NULL;
    if (text != NULL && fread(text, 1, (size_t) size, file) != (size_t) size) {
        free(text);
        text = NULL;
    }
    if (text != NULL) text[size] = '\0';
    fclose(file);
    return text;
}

/* Runs source in a fresh page realm, advancing until pocSummary leaves
   "pending", and checks it against wanted. */
static void run_page(const char *name, const char *source,
                     const char *wanted, size_t heap_bytes)
{
    ScriptPageFixture fixture;
    CHECK(script_page_fixture_open(&fixture, "<script></script>", 0));
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_configured(
        &fixture.document, &fixture.budget, heap_bytes, 1000,
        "https://subtle-crypto.test/", &fixture.options, &result);
    CHECK(runtime != NULL);
    bool evaluated = runtime != NULL && script_runtime_evaluate_diagnostic(
        runtime, source, name, &result);
    CHECK(evaluated);
    for (size_t turn = 0; evaluated && turn < 16
         && strcmp(result.summary, "pending") == 0; turn++)
        CHECK(script_runtime_advance(runtime, turn, 16, &result));
    if (strcmp(result.summary, wanted) != 0) {
        fprintf(stderr, "%s: summary=%s error=%s\n", name, result.summary,
                result.error);
        failures++;
    }
    if (runtime != NULL) script_runtime_destroy(runtime);
    CHECK(script_page_fixture_close(&fixture));
    printf("%s: %s\n", name,
           strcmp(result.summary, wanted) == 0 ? "PASS" : "FAIL");
}

static void test_page_surface(void)
{
    char *source = read_fixture(TILEFINCH_TEST_SOURCE_DIR
                                "/tests/fixtures/web-crypto/subtle.js");
    CHECK(source != NULL);
    if (source == NULL) return;
    run_page("subtle-crypto-vectors-and-errors", source, "SUBTLE-OK",
             12u * 1024u * 1024u);
    free(source);
}

static void test_key_clone(void)
{
    char *source = read_fixture(TILEFINCH_TEST_SOURCE_DIR
                                "/tests/fixtures/web-crypto/key-clone.js");
    CHECK(source != NULL);
    if (source == NULL) return;
    run_page("subtle-crypto-key-clone", source, "KEY-CLONE-OK",
             8u * 1024u * 1024u);
    free(source);
}

static void test_page_aes_message_limits(void)
{
    run_page("subtle-aes-message-limits",
        "globalThis.pocSummary='pending';"
        "(async()=>{"
        "const s=crypto.subtle, limit=1024*1024;"
        "const bytes=new Uint8Array(limit+1);"
        "bytes[0]=19; bytes[limit-1]=23;"
        "const plain=bytes.subarray(0,limit);"
        "for(const [name,overhead,tagLength] of ["
        " ['AES-CBC',16],['AES-GCM',4,32],['AES-GCM',16,128]]) {"
        " const alg={name,iv:new Uint8Array(name==='AES-CBC'?16:12)};"
        " if(tagLength) alg.tagLength=tagLength;"
        " const key=await s.importKey('raw',new Uint8Array(16),name,false,"
        " ['encrypt','decrypt']);"
        " const encrypted=await s.encrypt(alg,key,plain);"
        " const opened=new Uint8Array(await s.decrypt(alg,key,encrypted));"
        " if(encrypted.byteLength!==limit+overhead || opened.length!==limit"
        " || opened[0]!==19 || opened[limit-1]!==23)"
        " throw Error(name+' round trip');"
        " try { await s.encrypt(alg,key,bytes); throw Error('oversize'); }"
        " catch(e) { if(e.name!=='OperationError') throw e; }"
        " const bad=new Uint8Array(limit+overhead+(tagLength?1:0));"
        " bad.set(new Uint8Array(encrypted));"
        " if(!tagLength)"
        " for(let i=limit-16;i<limit;i++) bad[i]^=31;"
        " try { await s.decrypt(alg,key,bad); throw Error('oversize'); }"
        " catch(e) { if(e.name!=='OperationError') throw e; }"
        "}"
        "pocSummary='AES-LIMITS-OK';"
        "})().catch(e=>pocSummary=e.name+': '+e.message);",
        "AES-LIMITS-OK", 12u * 1024u * 1024u);
}

static void test_pbkdf2_queue_bounds(void)
{
    char *source = read_fixture(TILEFINCH_TEST_SOURCE_DIR
                                "/tests/fixtures/web-crypto/pbkdf2-queue.js");
    CHECK(source != NULL);
    if (source == NULL) return;
    run_page("pbkdf2-queue-bytes", source, "OperationError",
             8u * 1024u * 1024u);
    free(source);
}

static void test_idle_cancellation_owner(void)
{
    char *source = read_fixture(
        TILEFINCH_TEST_SOURCE_DIR
        "/tests/fixtures/web-crypto/idle-cancellation.js");
    CHECK(source != NULL);
    if (source == NULL) return;
    run_page("idle-cancellation-owner", source, "timer,idle:undefined",
             4u * 1024u * 1024u);
    free(source);
}

/* A realm that never reads crypto.subtle does not load the module, and
   feature tests see the full shape before and after the first read. */
static void test_lazy_until_used(void)
{
    run_page("subtle-crypto-lazy",
             "pocSummary=typeof CryptoKey==='function'&&"
             "typeof crypto.subtle.importKey==='function'?'SHAPE':'NO-SHAPE'",
             "SHAPE", 4u * 1024u * 1024u);
    ScriptPageFixture fixture;
    CHECK(script_page_fixture_open(&fixture, "<script></script>", 0));
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_configured(
        &fixture.document, &fixture.budget, 4u * 1024u * 1024u, 1000,
        "https://subtle-crypto.test/", &fixture.options, &result);
    CHECK(runtime != NULL && script_runtime_evaluate_diagnostic(
        runtime, "pocSummary=typeof CryptoKey+':'+('subtle' in crypto)+':'"
        "+typeof Object.getOwnPropertyDescriptor(Crypto.prototype,'subtle')"
        ".get+':'+(()=>{try{new CryptoKey();return 'made'}"
        "catch(e){return e.name}})()",
        "unread", &result));
    size_t before = result.bootstrap_lazy_module_loads;
    CHECK(strcmp(result.summary, "function:true:function:TypeError") == 0);
    CHECK(runtime != NULL && script_runtime_evaluate_diagnostic(
        runtime, "pocSummary=(crypto.subtle instanceof SubtleCrypto)+':'"
        "+typeof crypto.subtle.importKey+':'+crypto.subtle.unwrapKey.length"
        "+':'+typeof Object.getOwnPropertyDescriptor(CryptoKey.prototype,"
        "'usages').get",
        "first-read", &result));
    CHECK(strcmp(result.summary, "true:function:7:function") == 0);
    CHECK(before == 0 && result.bootstrap_lazy_module_loads == 1
          && result.bootstrap_lazy_module_failures == 0);
    CHECK(runtime != NULL && script_runtime_evaluate_diagnostic(
        runtime, "crypto.subtle.generateKey({name:'AES-GCM',length:128},"
        "false,['encrypt'])", "first-operation", &result));
    CHECK(result.bootstrap_lazy_module_loads == 1);
    if (runtime != NULL) script_runtime_destroy(runtime);
    CHECK(script_page_fixture_close(&fixture));
}

/* PBKDF2 runs in slices: a derivation leaves input and other tasks their
   turns while it is pending, and gives the bytes a one-shot run gives. */
static void test_pbkdf2_yields(void)
{
    ScriptPageFixture fixture;
    CHECK(script_page_fixture_open(&fixture, "<script></script>", 0));
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_configured(
        &fixture.document, &fixture.budget, 6u * 1024u * 1024u, 1000,
        "https://subtle-crypto.test/", &fixture.options, &result);
    CHECK(runtime != NULL);
    bool ok = runtime != NULL && script_runtime_evaluate_diagnostic(
        runtime,
        "globalThis.order=[];pocSummary='pending';"
        "document.body.addEventListener('click',()=>order.push('input'));"
        "const e=new TextEncoder();crypto.subtle.importKey('raw',"
        "e.encode('password'),'PBKDF2',false,['deriveBits']).then(k=>{"
        "const p=crypto.subtle.deriveBits({name:'PBKDF2',hash:'SHA-256',"
        "salt:e.encode('salt'),iterations:200000},k,256);"
        "setTimeout(()=>order.push('task'),0);return p}).then(b=>{"
        "order.push('derived');pocSummary=order.join()+':'+[...new Uint8Array(b)]"
        ".map(v=>v.toString(16).padStart(2,'0')).join('')},"
        "e=>pocSummary='ERR-'+e.name)",
        "pbkdf2-yields", &result);
    CHECK(ok);
    CHECK(ok && script_runtime_dispatch(runtime, "body", "click", &result));
    for (size_t turn = 0; ok && turn < 4096
         && strcmp(result.summary, "pending") == 0; turn++)
        ok = script_runtime_advance(runtime, 1, 16, &result);
    const char *wanted = "input,task,derived:ca64cfe28ca5559c62fba4afcb19f268"
                         "89a67d5b135e571bffb087647e01becd";
    bool passed = ok && strcmp(result.summary, wanted) == 0;
    if (!passed) {
        fprintf(stderr, "pbkdf2-yields: summary=%s error=%s\n",
                result.summary, result.error);
        failures++;
    }
    if (runtime != NULL) script_runtime_destroy(runtime);
    CHECK(script_page_fixture_close(&fixture));
    printf("subtle-crypto-pbkdf2-yields: %s\n", passed ? "PASS" : "FAIL");
}

/* A page that has filled its task table still gets its turns while
   derivations run: they continue in slices from the browser's reserved
   slot, one slice per task between them, rather than finishing in the
   call. Each gives its own bytes, and once the page clears its timers no
   task is left behind. */
static void test_pbkdf2_full_task_table(void)
{
    ScriptPageFixture fixture;
    CHECK(script_page_fixture_open(&fixture, "<script></script>", 0));
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_configured(
        &fixture.document, &fixture.budget, 6u * 1024u * 1024u, 1000,
        "https://subtle-crypto.test/", &fixture.options, &result);
    CHECK(runtime != NULL);
    bool ok = runtime != NULL && script_runtime_evaluate_diagnostic(
        runtime,
        "globalThis.order=[];pocSummary='pending';let n=0,id;"
        "const ids=[setInterval(()=>n++,1)];"
        "while((id=setTimeout(()=>{},1e9)))ids.push(id);"
        "document.body.addEventListener('click',()=>order.push('input'));"
        "const e=new TextEncoder(),hex=b=>[...new Uint8Array(b)].map(v=>"
        "v.toString(16).padStart(2,'0')).join('');"
        "crypto.subtle.importKey('raw',e.encode('password'),'PBKDF2',false,"
        "['deriveBits']).then(k=>{const all=[];for(let i=0;i<6;i++)"
        "all.push(crypto.subtle.deriveBits({name:'PBKDF2',hash:'SHA-256',"
        "salt:e.encode('salt'+i),iterations:500000},k,128).then(b=>{"
        "if(!order.includes('derived'))order.push('derived',n?'ticked':"
        "'unticked');return hex(b)}));return Promise.all(all)}).then(h=>{"
        "for(const i of ids)clearTimeout(i);pocSummary=order.join()+':'+"
        "ids.length+':'+h.join()},e=>pocSummary='ERR-'+e.name)",
        "pbkdf2-full-table", &result);
    CHECK(ok);
    CHECK(ok && script_runtime_dispatch(runtime, "body", "click", &result));
    for (size_t turn = 0; ok && turn < 4096
         && strcmp(result.summary, "pending") == 0; turn++)
        ok = script_runtime_advance(runtime, 1, 16, &result);
    const char *wanted = "input,derived,ticked:128:"
                         "2aee1190515007f7854df247fd2863ec,"
                         "46ba568d1dbf8884b1eed87d3eb522bc,"
                         "38122b7c5a00d1685ec417063ca5532f,"
                         "85fc8244ef25ecc11c23481309f26076,"
                         "478471338a7a3aa8b4d76a9f853e770f,"
                         "c8f19d85bdcbc17496052bd018bb7aeb";
    bool passed = ok && strcmp(result.summary, wanted) == 0;
    ScriptRunnableState runnable = {0};
    bool drained = runtime != NULL
        && script_runtime_runnable_state(runtime, &runnable)
        && runnable.timers == 0;
    if (!passed || !drained) {
        fprintf(stderr, "pbkdf2-full-table: summary=%s error=%s timers=%zu\n",
                result.summary, result.error, runnable.timers);
        failures++;
    }
    if (runtime != NULL) script_runtime_destroy(runtime);
    CHECK(script_page_fixture_close(&fixture));
    printf("subtle-crypto-pbkdf2-full-table: %s\n",
           passed && drained ? "PASS" : "FAIL");
}

/* With the browser's reserved slots taken too, a derivation that needs
   another slice rejects with an OperationError rather than finishing in
   the call, and leaves no task behind. */
static void test_pbkdf2_slot_refused(void)
{
    ScriptPageFixture fixture;
    CHECK(script_page_fixture_open(&fixture, "<script></script>", 0));
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_configured(
        &fixture.document, &fixture.budget, 6u * 1024u * 1024u, 1000,
        "https://subtle-crypto.test/", &fixture.options, &result);
    CHECK(runtime != NULL);
    bool ok = runtime != NULL && script_runtime_evaluate_diagnostic(
        runtime,
        "pocSummary='pending';const ids=[],timers=__tilefinchTestDomShared;"
        "let id;while((id=setTimeout(()=>{},1e9)))ids.push(id);"
        "while((id=timers.scheduleReservedTask(()=>{},1e9)))ids.push(id);"
        "const e=new TextEncoder(),done=r=>{for(const i of ids)"
        "timers.cancelTimer(i);pocSummary=r};"
        "crypto.subtle.importKey('raw',e.encode('password'),'PBKDF2',false,"
        "['deriveBits']).then(k=>crypto.subtle.deriveBits({name:'PBKDF2',"
        "hash:'SHA-256',salt:e.encode('salt'),iterations:9000000},k,256))"
        ".then(()=>done('derived'),e=>done(e.name))",
        "pbkdf2-slot-refused", &result);
    for (size_t turn = 0; ok && turn < 64
         && strcmp(result.summary, "pending") == 0; turn++)
        ok = script_runtime_advance(runtime, 1, 16, &result);
    bool passed = ok && strcmp(result.summary, "OperationError") == 0;
    ScriptRunnableState runnable = {0};
    bool drained = runtime != NULL
        && script_runtime_runnable_state(runtime, &runnable)
        && runnable.timers == 0;
    if (!passed || !drained) {
        fprintf(stderr, "pbkdf2-slot-refused: summary=%s error=%s "
                "timers=%zu\n", result.summary, result.error,
                runnable.timers);
        failures++;
    }
    if (runtime != NULL) script_runtime_destroy(runtime);
    CHECK(script_page_fixture_close(&fixture));
    printf("subtle-crypto-pbkdf2-slot-refused: %s\n",
           passed && drained ? "PASS" : "FAIL");
}

/* A long derivation stays pending across turns, and leaving the page (its
   realm destroyed) stops it and frees what it held. */
static void test_pbkdf2_navigation_stops(void)
{
    ScriptPageFixture fixture;
    CHECK(script_page_fixture_open(&fixture, "<script></script>", 0));
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_configured(
        &fixture.document, &fixture.budget, 6u * 1024u * 1024u, 1000,
        "https://subtle-crypto.test/", &fixture.options, &result);
    CHECK(runtime != NULL);
    bool ok = runtime != NULL && script_runtime_evaluate_diagnostic(
        runtime,
        "pocSummary='pending';const e=new TextEncoder();"
        "crypto.subtle.importKey('raw',e.encode('password'),'PBKDF2',false,"
        "['deriveBits']).then(k=>crypto.subtle.deriveBits({name:'PBKDF2',"
        "hash:'SHA-512',salt:e.encode('salt'),iterations:9000000},k,512))"
        ".then(()=>pocSummary='derived',e=>pocSummary='ERR-'+e.name)",
        "pbkdf2-navigation", &result);
    for (size_t turn = 0; ok && turn < 8; turn++)
        ok = script_runtime_advance(runtime, 1, 16, &result);
    bool pending = ok && strcmp(result.summary, "pending") == 0;
    if (!pending) {
        fprintf(stderr, "pbkdf2-navigation: summary=%s error=%s\n",
                result.summary, result.error);
        failures++;
    }
    if (runtime != NULL) script_runtime_destroy(runtime);
    CHECK(script_page_fixture_close(&fixture));
    printf("subtle-crypto-pbkdf2-navigation: %s\n", pending ? "PASS" : "FAIL");
}

/* Runs one case of pbkdf2-scope.js until it settles. A frame:<handle>
   summary asks for the host's part: "frame-left" commits a cross-origin
   document in that frame, "frame-closed" closes it, as navigation does.
   Then, with no task left behind, the largest PBKDF2 request
   (pbkdf2-capacity.js), which is charged the whole pending-derivation
   ceiling, must be admitted: no job of a stopped scope or a finished one
   still holds a charge. */
static void run_pbkdf2_scope(const char *name, const char *scope_case,
                             const char *wanted)
{
    char *fixture = read_fixture(TILEFINCH_TEST_SOURCE_DIR
                                 "/tests/fixtures/web-crypto/pbkdf2-scope.js");
    char *probe = read_fixture(TILEFINCH_TEST_SOURCE_DIR
                               "/tests/fixtures/web-crypto/pbkdf2-capacity.js");
    CHECK(fixture != NULL && probe != NULL);
    size_t capacity = fixture == NULL ? 0u : strlen(fixture) + 64u;
    char *script = fixture == NULL ? NULL : malloc(capacity);
    CHECK(script != NULL);
    ScriptPageFixture page;
    CHECK(script_page_fixture_open(&page, "<script></script>", 0));
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_configured(
        &page.document, &page.budget, 16u * 1024u * 1024u, 1000,
        "https://subtle-crypto.test/", &page.options, &result);
    CHECK(runtime != NULL);
    bool ok = runtime != NULL && script != NULL && probe != NULL;
    if (ok) {
        snprintf(script, capacity, "const scopeCase = '%s';\n%s",
                 scope_case, fixture);
        ok = script_runtime_evaluate_diagnostic(runtime, script, name,
                                                &result);
    }
    for (size_t turn = 0; ok && turn < 8192; turn++) {
        long frame = 0;
        if (sscanf(result.summary, "frame:%ld", &frame) == 1) {
            bool left = strcmp(scope_case, "frame-left") == 0;
            ok = script_runtime_set_frame_window_state(
                     runtime, frame, left, false, false,
                     left ? "https://other.test/" : NULL, left, &result)
                && script_runtime_evaluate_diagnostic(
                     runtime, "pocSummary='pending'", name, &result);
        } else if (strcmp(result.summary, "pending") != 0) {
            break;
        }
        if (ok) ok = script_runtime_advance(runtime, 1, 16, &result);
    }
    bool passed = ok && strcmp(result.summary, wanted) == 0;
    ScriptRunnableState runnable = {0};
    /* Stopped jobs leave their scope's turn at the next slice task. */
    for (size_t turn = 0; ok && turn < 8
         && script_runtime_runnable_state(runtime, &runnable)
         && runnable.timers != 0; turn++)
        ok = script_runtime_advance(runtime, 1, 16, &result);
    bool drained = ok && script_runtime_runnable_state(runtime, &runnable)
        && runnable.timers == 0;
    char summary[sizeof(result.summary)];
    snprintf(summary, sizeof(summary), "%s", result.summary);
    ok = ok && script_runtime_evaluate_diagnostic(runtime, probe,
                                                  "pbkdf2-capacity", &result);
    for (size_t turn = 0; ok && turn < 64
         && strcmp(result.summary, "pending") == 0; turn++)
        ok = script_runtime_advance(runtime, 1, 16, &result);
    bool released = ok && strcmp(result.summary, "admitted") == 0;
    if (!passed || !drained || !released) {
        fprintf(stderr, "%s: summary=%s timers=%zu capacity=%s error=%s\n",
                name, summary, runnable.timers, result.summary,
                result.error);
        failures++;
    }
    if (runtime != NULL) script_runtime_destroy(runtime);
    CHECK(script_page_fixture_close(&page));
    free(script);
    free(probe);
    free(fixture);
    printf("%s: %s\n", name,
           passed && drained && released ? "PASS" : "FAIL");
}

/* A worker's derivations are its own: terminating it stops them, so they
   no longer keep the page's from running. */
static void test_pbkdf2_terminated_worker(void)
{
    run_pbkdf2_scope("pbkdf2-terminated-worker", "terminated",
                     "started:fbcaa0d5cb053807a2d69a4476190c1b"
                     "0348452f5187431df95ebbf0132dfb92");
}

/* A worker holding more long derivations than a scope runs at once does
   not hold up the page's: each scope gets its turn of the slices. */
static void test_pbkdf2_busy_worker(void)
{
    run_pbkdf2_scope("pbkdf2-busy-worker", "busy",
                     "started:fbcaa0d5cb053807a2d69a4476190c1b"
                     "0348452f5187431df95ebbf0132dfb92");
}

#define PBKDF2_PAGE_BYTES "fbcaa0d5cb053807a2d69a4476190c1b" \
                          "0348452f5187431df95ebbf0132dfb92"

/* A frame window has its own Crypto, and its derivations, running and
   waiting, stop with its document, unsettled, as its realm is gone. */
static void test_pbkdf2_frame_navigation(void)
{
    run_pbkdf2_scope("pbkdf2-frame-navigation", "frame",
                     "own-crypto:started:0::" PBKDF2_PAGE_BYTES);
}

/* A removed frame keeps its realm, so its derivations reject; they give
   up their turns and charges, and the page's own derivation runs. */
static void test_pbkdf2_frame_removed(void)
{
    run_pbkdf2_scope("pbkdf2-frame-removed", "frame-removed",
                     "own-crypto:started:20:OperationError:"
                     PBKDF2_PAGE_BYTES);
}

/* A cross-origin document committed in the frame keeps the old realm
   behind the WindowProxy; the old document's derivations reject. */
static void test_pbkdf2_frame_left(void)
{
    run_pbkdf2_scope("pbkdf2-frame-left", "frame-left",
                     "own-crypto:started:20:OperationError:"
                     PBKDF2_PAGE_BYTES);
}

/* A frame the host closes (detached or failed) rejects its derivations
   while its realm stays. */
static void test_pbkdf2_frame_closed(void)
{
    run_pbkdf2_scope("pbkdf2-frame-closed", "frame-closed",
                     "own-crypto:started:20:OperationError:"
                     PBKDF2_PAGE_BYTES);
}

/* Twenty derivations started at once all finish with their own bytes:
   those over the 16 a scope runs at once wait their turn instead of being
   refused, and the page's timers keep running between slices. */
static void test_pbkdf2_waits_in_turn(void)
{
    run_pbkdf2_scope("pbkdf2-waits-in-turn", "parallel",
                     "ticked:20:"
                     "89eee2e55cccdceefa385c7b0976048488b69bc120fb7d8167d327be44ea08b5,"
                     "f927796a519de5c7c5cb257773b914bf43e001bb61e6a049d0adb4969b9f89ee,"
                     "8aced4a8de798d666fa81fedc10080a6fa10aa5b4245d6162f6cf9a0c3162814,"
                     "55215f3167fe111e3585bc43a39505b08c640d4f616d18b7f29b296bbb88c4c4");
}

/* Runs one case of sliced.js with each slice one chunk long, until it
   settles and no task is left behind. */
static void run_sliced(const char *name, const char *scope_case,
                       const char *wanted)
{
    char *fixture = read_fixture(TILEFINCH_TEST_SOURCE_DIR
                                 "/tests/fixtures/web-crypto/sliced.js");
    CHECK(fixture != NULL);
    size_t capacity = fixture == NULL ? 0u : strlen(fixture) + 64u;
    char *script = fixture == NULL ? NULL : malloc(capacity);
    CHECK(script != NULL);
    ScriptPageFixture page;
    CHECK(script_page_fixture_open(&page, "<script></script>", 0));
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_configured(
        &page.document, &page.budget, 24u * 1024u * 1024u, 1000,
        "https://subtle-crypto.test/", &page.options, &result);
    CHECK(runtime != NULL);
    bool ok = runtime != NULL && script != NULL;
    size_t turns = 0;
    if (ok) {
        runtime->crypto_slice_ns = 1u;
        snprintf(script, capacity, "const scopeCase = '%s';\n%s",
                 scope_case, fixture);
        ok = script_runtime_evaluate_diagnostic(runtime, script, name,
                                                &result);
    }
    for (; ok && turns < 20000
         && strcmp(result.summary, "pending") == 0; turns++)
        ok = script_runtime_advance(runtime, 1, 16, &result);
    bool passed = ok && strcmp(result.summary, wanted) == 0;
    ScriptRunnableState runnable = {0};
    for (size_t turn = 0; ok && turn < 8
         && script_runtime_runnable_state(runtime, &runnable)
         && runnable.timers != 0; turn++)
        ok = script_runtime_advance(runtime, 1, 16, &result);
    bool drained = ok && script_runtime_runnable_state(runtime, &runnable)
        && runnable.timers == 0;
    if (!passed || !drained) {
        fprintf(stderr, "%s: summary=%s timers=%zu turns=%zu error=%s\n",
                name, result.summary, runnable.timers, turns, result.error);
        failures++;
    }
    if (runtime != NULL) script_runtime_destroy(runtime);
    CHECK(script_page_fixture_close(&page));
    free(script);
    free(fixture);
    printf("%s: %s (%zu turns)\n", name, passed && drained ? "PASS" : "FAIL",
           turns);
}

/* Long and short operations started together settle in the order they
   were asked for, with the bytes of the one-call code, while the page's
   timers run between slices. */
static void test_sliced_order(void)
{
    run_sliced("subtle-sliced-order", "order", "task,A,B,C,D,E,F,G:ok");
}

/* A worker's sliced operations and the page's share the slices, each
   scope settling in its own order. */
static void test_sliced_worker(void)
{
    run_sliced("subtle-sliced-worker", "worker",
               "page:P,Q:ok;worker:X,Y,Z:ok");
}

/* Terminating a worker stops its sliced operations; the page's run. */
static void test_sliced_terminated_worker(void)
{
    run_sliced("subtle-sliced-terminated", "terminated", "started:ok");
}

/* Short operations queued behind a long one settle after it in the order
   asked, failures too, and in the task the long one ends in. */
static void test_sliced_failure_order(void)
{
    run_sliced("subtle-sliced-failure-order", "failure",
               "A,B-OperationError,C,D-OperationError,E");
}

/* A frame's sliced operations stop with its document, unsettled. */
static void test_sliced_frame_navigation(void)
{
    run_sliced("subtle-sliced-frame", "frame", "started:ok");
}

/* A removed frame's sliced operations reject, and its SubtleCrypto stays
   stopped: what the page later asks of it rejects at once instead of
   running until the next frame change. Put back, the frame gets a working
   Crypto; the page's own job runs. */
static void test_sliced_frame_removed(void)
{
    run_sliced("subtle-sliced-frame-removed", "frame-removed",
               "started,OperationError,OperationError,OperationError,"
               "OperationError:OperationError,OperationError,OperationError"
               ":ok:ok");
}

/* __testRefuse(n) refuses the nth JavaScript heap allocation from then
   on (JS_TestRefuseAllocation); __testRefusalPending() is whether that
   refusal has yet to happen. */
static JSValue crypto_test_refuse(JSContext *context, JSValueConst this_value,
                                  int argc, JSValueConst *argv)
{
    (void) this_value;
    uint32_t nth = 0;
    if (argc > 0 && JS_ToUint32(context, &nth, argv[0]) < 0)
        return JS_EXCEPTION;
    JS_TestRefuseAllocation(nth);
    return JS_UNDEFINED;
}

static JSValue crypto_test_refusal_pending(JSContext *context,
                                           JSValueConst this_value,
                                           int argc, JSValueConst *argv)
{
    (void) this_value; (void) argc; (void) argv;
    return JS_NewBool(context, JS_TestRefusalPending());
}

/* The first read of crypto.subtle with its nth allocation refused, then a
   second read: "LEFT" when the refusal never came, otherwise how many of
   the 11 operations and 4 CryptoKey attributes each read left installed,
   and whether a key round trip works after the second. */
static const char subtle_refusal_script[] =
    "const names=['encrypt','decrypt','sign','verify','generateKey',"
    "'deriveKey','deriveBits','importKey','exportKey','wrapKey','unwrapKey'],"
    "attributes=['type','extractable','algorithm','usages'],"
    "shape=()=>names.filter(n=>typeof SubtleCrypto.prototype[n]==='function')"
    ".length+attributes.filter(n=>typeof Object.getOwnPropertyDescriptor("
    "CryptoKey.prototype,n)?.get==='function').length;"
    "__testRefuse(N);try{crypto.subtle}catch{}"
    "const left=__testRefusalPending();__testRefuse(0);"
    "const first=shape();let second=-1;try{crypto.subtle;second=shape()}catch{}"
    "pocSummary=left?'LEFT':'pending';"
    "if(!left){const s=crypto.subtle,iv=new Uint8Array(12);"
    "Promise.resolve().then(()=>s.generateKey({name:'AES-GCM',length:128},"
    "true,['encrypt','decrypt']))"
    ".then(k=>s.encrypt({name:'AES-GCM',iv},k,new Uint8Array(4)).then(c=>"
    "s.decrypt({name:'AES-GCM',iv},k,c)).then(p=>s.exportKey('raw',k).then("
    "r=>p.byteLength+'/'+r.byteLength+'/'+k.algorithm.name)))"
    ".then(v=>v,e=>e.name).then(v=>pocSummary=first+':'+second+':'+v)}";

/* A first read of crypto.subtle that runs out of memory part way through
   adding the operations leaves the API whole once a later read succeeds,
   wherever the refusal falls: in the module load (then digest alone stays,
   as the loader marks the module failed), in building the operations, or
   between the members it adds. */
static void test_subtle_refused_install_retries(void)
{
    unsigned installs_refused = 0, loads_refused = 0, wrong = 0;
    for (unsigned nth = 1; nth < 100000u; nth++) {
        ScriptPageFixture fixture;
        CHECK(script_page_fixture_open(&fixture, "<script></script>", 0));
        ScriptResult result = {0};
        ScriptRuntime *runtime = script_runtime_create_configured(
            &fixture.document, &fixture.budget, 6u * 1024u * 1024u, 1000,
            "https://subtle-crypto.test/", &fixture.options, &result);
        CHECK(runtime != NULL);
        if (runtime == NULL) break;
        JSContext *context = runtime->context;
        JSValue global = JS_GetGlobalObject(context);
        JS_SetPropertyStr(context, global, "__testRefuse",
                          JS_NewCFunction(context, crypto_test_refuse,
                                          "__testRefuse", 1));
        JS_SetPropertyStr(context, global, "__testRefusalPending",
                          JS_NewCFunction(context, crypto_test_refusal_pending,
                                          "__testRefusalPending", 0));
        JS_FreeValue(context, global);
        char script[sizeof(subtle_refusal_script) + 32];
        snprintf(script, sizeof(script), "const N=%u;%s", nth,
                 subtle_refusal_script);
        bool evaluated = script_runtime_evaluate_diagnostic(
            runtime, script, "subtle-refused-install", &result);
        JS_TestRefuseAllocation(0);
        for (size_t turn = 0; evaluated && turn < 16
             && strcmp(result.summary, "pending") == 0; turn++)
            evaluated = script_runtime_advance(runtime, turn, 16, &result);
        bool left = strcmp(result.summary, "LEFT") == 0;
        bool load_failed = result.bootstrap_lazy_module_failures != 0;
        int first = -1, second = -1;
        char tail[64] = "";
        sscanf(result.summary, "%d:%d:%63s", &first, &second, tail);
        bool whole = second == 15 && strcmp(tail, "4/16/AES-GCM") == 0;
        bool digest_only = load_failed && second == 0
            && strcmp(tail, "TypeError") == 0;
        if (!left && !whole && !digest_only) {
            if (wrong++ < 6)
                fprintf(stderr, "subtle refused install nth=%u summary=%s "
                        "load-failed=%d error=%s\n", nth, result.summary,
                        load_failed, result.error);
        }
        if (!left && !load_failed && first != 15) installs_refused++;
        if (load_failed) loads_refused++;
        script_runtime_destroy(runtime);
        CHECK(script_page_fixture_close(&fixture));
        if (left) break;
    }
    printf("subtle-refused-install: install refusals=%u load refusals=%u "
           "wrong=%u: %s\n", installs_refused, loads_refused, wrong,
           wrong == 0 && installs_refused != 0 ? "PASS" : "FAIL");
    CHECK(wrong == 0);
    /* The sweep reached the installation, not only the module load. */
    CHECK(installs_refused != 0);
}

/* Key data is released with its CryptoKey: 64 dropped 256 KiB keys fit a
   4 MiB realm only when each one's secret is freed. */
static void test_key_data_released(void)
{
    run_page("subtle-crypto-key-release",
             "(()=>{pocSummary='pending';const data=new Uint8Array(262144);"
             "let chain=Promise.resolve(),n=0;for(let i=0;i<64;i++)"
             "chain=chain.then(()=>crypto.subtle.importKey('raw',data,"
             "{name:'HMAC',hash:'SHA-256'},false,['sign'])).then(k=>"
             "crypto.subtle.sign('HMAC',k,data)).then(()=>{n++});"
             "chain.then(()=>pocSummary='RELEASED-'+n,"
             "e=>pocSummary='RELEASE-'+e.name)})()",
             "RELEASED-64", 4u * 1024u * 1024u);
}

static void test_window_key_message(void)
{
    run_page("subtle-crypto-window-message",
             "(async()=>{pocSummary='pending';const subtle=crypto.subtle,"
             "key=await subtle.importKey('raw',new Uint8Array([1,2,3]),"
             "{name:'HMAC',hash:'SHA-256'},false,['sign']);"
             "const input=new Uint8Array([9]),want=String(new Uint8Array("
             "await subtle.sign('HMAC',key,input)));"
             "key.algorithm.hash.name='SHA-1';key.usages[0]='verify';"
             "let count=0;addEventListener('message',async e=>{count++;"
             "try{const [first,again]=e.data,signed=String(new Uint8Array("
             "await subtle.sign('HMAC',first,input))),"
             "exported=await subtle.exportKey('raw',first).then("
             "()=>'exported',e=>e.name);"
             "pocSummary=count===1&&e.source===window&&first instanceof CryptoKey"
             "&&e.data.length===2&&first!==key&&first===again&&first.usages.join()==='sign'"
             "&&first.algorithm.hash.name==='SHA-256'&&!first.extractable"
             "&&signed===want&&exported==='InvalidAccessError'"
             "?'WINDOW-KEY-OK':'WINDOW-KEY-BAD'}"
             "catch(error){pocSummary=error.name+':'+error.message}});"
             "postMessage([key,key,'wrong-origin'],'https://wrong-origin.test');"
             "postMessage([key,key],'*')})()"
             ".catch(e=>pocSummary=e.name+':'+e.message)",
             "WINDOW-KEY-OK", 4u * 1024u * 1024u);
}

/* A dedicated worker's crypto.subtle runs the same operations, PBKDF2's
   slices included; buffers cross from the worker realm and results come
   back as its own. */
static void test_worker(void)
{
    run_page("subtle-crypto-worker",
             "(()=>{pocSummary='pending';const source=\""
             "const s=crypto.subtle,iv=new Uint8Array(12),"
             "data=new TextEncoder().encode('worker');"
             "s.generateKey({name:'AES-GCM',length:128},false,"
             "['encrypt','decrypt']).then(k=>s.encrypt({name:'AES-GCM',iv},"
             "k,data).then(sealed=>s.decrypt({name:'AES-GCM',iv},k,sealed)"
             ".then(opened=>s.exportKey('raw',k).then(()=>'exported',"
             "e=>e.name).then(exported=>s.importKey('raw',data,'PBKDF2',false,"
             "['deriveBits']).then(p=>s.deriveBits({name:'PBKDF2',"
             "hash:'SHA-256',salt:new TextEncoder().encode('salt'),"
             "iterations:100000},p,64)).then(bits=>postMessage([k instanceof "
             "CryptoKey,sealed instanceof ArrayBuffer,new TextDecoder()"
             ".decode(opened),exported,bits instanceof ArrayBuffer,[...new "
             "Uint8Array(bits)].map(v=>v.toString(16).padStart(2,'0')).join('')"
             "].join()))))))"
             ".catch(e=>postMessage('ERR-'+e.name+'-'+e.message))\","
             "url=URL.createObjectURL(new Blob([source])),"
             "worker=new Worker(url);worker.onmessage=e=>{pocSummary=e.data;"
             "worker.terminate();URL.revokeObjectURL(url)}})()",
             "true,true,worker,InvalidAccessError,true,16496a931fcaa985",
             6u * 1024u * 1024u);
}
#endif

int main(void)
{
#if defined(TILEFINCH_WEB_CRYPTO_MBEDTLS)
    test_hmac_setup_refusal();
#endif
    test_hmac();
    test_pbkdf2();
    test_hkdf();
    test_hkdf_work_limit();
    test_aes();
    test_aes_message_limits();
    test_job_plan();
    test_sliced_jobs();
#if defined(TILEFINCH_WEB_CRYPTO_MBEDTLS)
    test_job_contexts_on_first_run();
#endif
#if !defined(TILEFINCH_WEB_CRYPTO_VECTORS_ONLY)
    test_page_surface();
    test_key_clone();
    test_page_aes_message_limits();
    test_lazy_until_used();
    test_subtle_refused_install_retries();
    test_pbkdf2_yields();
    test_pbkdf2_full_task_table();
    test_pbkdf2_slot_refused();
    test_pbkdf2_queue_bounds();
    test_pbkdf2_waits_in_turn();
    test_pbkdf2_terminated_worker();
    test_pbkdf2_busy_worker();
    test_pbkdf2_frame_navigation();
    test_pbkdf2_frame_removed();
    test_pbkdf2_frame_left();
    test_pbkdf2_frame_closed();
    test_pbkdf2_navigation_stops();
    test_sliced_order();
    test_sliced_worker();
    test_sliced_terminated_worker();
    test_sliced_frame_navigation();
    test_sliced_failure_order();
    test_sliced_frame_removed();
    test_idle_cancellation_owner();
    test_key_data_released();
    test_window_key_message();
    test_worker();
#endif
    if (failures != 0) {
        fprintf(stderr, "web crypto: %d failure(s)\n", failures);
        return 1;
    }
    puts("web crypto: all checks passed");
    return 0;
}
