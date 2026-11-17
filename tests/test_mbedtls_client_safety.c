/* Synthetic inputs through the pinned TLS library, not a model parser.
 * This executable performs no network I/O and handles no real credentials. */
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include <mbedtls/platform.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509.h>
#include <mbedtls/oid.h>
#include <mbedtls/psa_util.h>
#include <psa/crypto.h>
#include "ssl_misc.h"
#include "ssl_tls13_keys.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures;
static size_t owned, allocation_calls, fail_at;
#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "TLS safety check failed at %d: %s\n", __LINE__, #x); \
    failures++; \
} } while (0)

static void *test_calloc(size_t n, size_t size)
{
    if (fail_at != 0 && ++allocation_calls == fail_at) return NULL;
    void *p = calloc(n, size);
    if (p != NULL) owned++;
    return p;
}

static void test_free(void *p)
{
    if (p != NULL) {
        CHECK(owned != 0);
        owned--;
        free(p);
    }
}

typedef struct {
    unsigned char bytes[256];
    size_t size, used;
} TestIo;

static int test_send(void *ctx, const unsigned char *bytes, size_t size)
{
    (void) ctx; (void) bytes;
    return (int) size;
}

static int test_recv(void *ctx, unsigned char *bytes, size_t size)
{
    TestIo *io = ctx;
    if (io->used == io->size) return MBEDTLS_ERR_SSL_WANT_READ;
    if (size > io->size - io->used) size = io->size - io->used;
    memcpy(bytes, io->bytes + io->used, size);
    io->used += size;
    return (int) size;
}

static int test_rng(void *ctx, unsigned char *bytes, size_t size)
{
    (void) ctx;
    /* Only the early synthetic handshake uses this deterministic test RNG. */
    memset(bytes, 0x41, size);
    return 0;
}

static int prepare_client(mbedtls_ssl_context *ssl, mbedtls_ssl_config *conf,
                          TestIo *io)
{
    memset(io, 0, sizeof(*io));
    mbedtls_ssl_init(ssl);
    mbedtls_ssl_config_init(conf);
    int ret = mbedtls_ssl_config_defaults(conf, MBEDTLS_SSL_IS_CLIENT,
        MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) return ret;
    mbedtls_ssl_conf_rng(conf, test_rng, NULL);
    mbedtls_ssl_conf_min_tls_version(conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(conf, MBEDTLS_SSL_VERSION_TLS1_3);
    if ((ret = mbedtls_ssl_setup(ssl, conf)) != 0) return ret;
    mbedtls_ssl_set_bio(ssl, io, test_send, test_recv, NULL);
    for (unsigned i = 0; i < 8; i++) {
        ret = mbedtls_ssl_handshake_step(ssl);
        if (ret != 0) return ret == MBEDTLS_ERR_SSL_WANT_READ ? 0 : ret;
    }
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

static void record(TestIo *io, unsigned type, const unsigned char *body,
                   size_t size)
{
    CHECK(size <= sizeof(io->bytes) - 9);
    io->bytes[0] = MBEDTLS_SSL_MSG_HANDSHAKE;
    io->bytes[1] = 3; io->bytes[2] = 3;
    io->bytes[3] = (unsigned char) ((size + 4) >> 8);
    io->bytes[4] = (unsigned char) (size + 4);
    io->bytes[5] = (unsigned char) type;
    io->bytes[6] = 0; io->bytes[7] = 0;
    io->bytes[8] = (unsigned char) size;
    memcpy(io->bytes + 9, body, size);
    io->size = size + 9; io->used = 0;
}

static void hello_versions(void)
{
    const unsigned versions[] = {0x0303, 0x0304, 0x0305, 0x0301};
    for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); i++) {
        mbedtls_ssl_context ssl;
        mbedtls_ssl_config conf;
        TestIo io;
        int ret = prepare_client(&ssl, &conf, &io);
        CHECK(ret == 0);
        if (ret == 0) {
            unsigned char body[38] = {0};
            body[0] = (unsigned char) (versions[i] >> 8);
            body[1] = (unsigned char) versions[i];
            body[35] = 0xc0; body[36] = 0x2f;
            record(&io, MBEDTLS_SSL_HS_SERVER_HELLO, body, sizeof(body));
            mbedtls_ssl_protocol_version before = ssl.session_negotiate->tls_version;
            for (unsigned step = 0; step < 8; step++) {
                ret = mbedtls_ssl_handshake_step(&ssl);
                if (ret != 0 || ssl.state != MBEDTLS_SSL_SERVER_HELLO) break;
            }
            if (versions[i] == 0x0303) {
                CHECK(ret == 0 && ssl.state == MBEDTLS_SSL_SERVER_CERTIFICATE);
                CHECK(ssl.session_negotiate->tls_version == MBEDTLS_SSL_VERSION_TLS1_2);
            } else {
                CHECK(ret == MBEDTLS_ERR_SSL_BAD_PROTOCOL_VERSION);
                CHECK(ssl.tls_version == MBEDTLS_SSL_VERSION_TLS1_2);
                CHECK(ssl.session_negotiate->tls_version == before);
                CHECK(ssl.state == MBEDTLS_SSL_SERVER_HELLO);
            }
        }
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
    }
}

static void certificate_requests(void)
{
    static const unsigned char good[] = {1, 1, 0, 2, 4, 1, 0, 0};
    static const unsigned char odd[] = {1, 1, 0, 1, 4, 0, 0};
    static const unsigned char short_ca[] = {1, 1, 0, 2, 4, 1, 0xff};
    const unsigned char *bodies[] = {good, odd, short_ca, good, good};
    const size_t sizes[] = {sizeof(good), sizeof(odd), sizeof(short_ca), 2, 3};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        mbedtls_ssl_context ssl;
        mbedtls_ssl_config conf;
        TestIo io;
        int ret = prepare_client(&ssl, &conf, &io);
        CHECK(ret == 0);
        if (ret == 0) {
            ssl.tls_version = MBEDTLS_SSL_VERSION_TLS1_2;
            ssl.state = MBEDTLS_SSL_CERTIFICATE_REQUEST;
            ssl.handshake->ciphersuite_info = mbedtls_ssl_ciphersuite_from_id(0xc02f);
            record(&io, MBEDTLS_SSL_HS_CERTIFICATE_REQUEST, bodies[i], sizes[i]);
            ret = mbedtls_ssl_handshake_step(&ssl);
            CHECK(ret == (i == 0 ? 0 : MBEDTLS_ERR_SSL_DECODE_ERROR));
        }
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
    }
}

static void session_lengths(void)
{
    mbedtls_ssl_session session, restored;
    unsigned char bytes[4096];
    size_t size = 0, offset = 0;
    mbedtls_ssl_session_init(&session);
    session.tls_version = MBEDTLS_SSL_VERSION_TLS1_2;
    session.endpoint = MBEDTLS_SSL_IS_CLIENT;
    session.ciphersuite = 0xc02f;
    session.id_len = sizeof(session.id);
    memset(session.id, 0x5a, sizeof(session.id));
    CHECK(mbedtls_ssl_session_save(&session, bytes, sizeof(bytes), &size) == 0);
    for (size_t i = 1; i + sizeof(session.id) <= size; i++) {
        if (bytes[i - 1] == sizeof(session.id) &&
            memcmp(bytes + i, session.id, sizeof(session.id)) == 0) {
            CHECK(offset == 0);
            offset = i - 1;
        }
    }
    CHECK(offset != 0);
    if (offset != 0) {
        const unsigned lengths[] = {0, 32, 33, 255};
        for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
            mbedtls_ssl_session_init(&restored);
            bytes[offset] = (unsigned char) lengths[i];
            int ret = mbedtls_ssl_session_load(&restored, bytes, size);
            CHECK(ret == (lengths[i] <= 32 ? 0 : MBEDTLS_ERR_SSL_BAD_INPUT_DATA));
            if (ret == 0) CHECK(restored.id_len == lengths[i]);
            mbedtls_ssl_session_free(&restored);
        }
    }
    mbedtls_ssl_session_free(&session);
}

static void transform_failures(void)
{
    for (size_t fail = 0; fail <= 6; fail++) {
        mbedtls_ssl_transform transform;
        mbedtls_ssl_key_set keys;
        memset(&keys, 0, sizeof(keys));
        keys.key_len = 16; keys.iv_len = 12;
        mbedtls_ssl_transform_init(&transform);
        size_t before = owned;
        fail_at = fail; allocation_calls = 0;
        int ret = mbedtls_ssl_tls13_populate_transform(&transform,
            MBEDTLS_SSL_IS_CLIENT, MBEDTLS_TLS1_3_AES_128_GCM_SHA256,
            &keys, NULL);
        fail_at = 0;
        if (ret != 0) CHECK(owned == before);
        if (fail == 0) CHECK(ret == 0);
        mbedtls_ssl_transform_free(&transform);
        CHECK(owned == before);
    }
    mbedtls_ssl_transform transform;
    mbedtls_ssl_key_set keys;
    memset(&keys, 0, sizeof(keys));
    keys.key_len = 16; keys.iv_len = 12;
    mbedtls_ssl_transform_init(&transform);
    size_t before = owned;
    CHECK(mbedtls_ssl_tls13_populate_transform(&transform, 99,
        MBEDTLS_TLS1_3_AES_128_GCM_SHA256, &keys, NULL) != 0);
    CHECK(owned == before);
    mbedtls_ssl_transform_free(&transform);
    CHECK(owned == before);
}

static void distinguished_names(void)
{
    mbedtls_x509_name empty = {0}, normal = {0};
    char bytes[64];
    memset(bytes, 'X', sizeof(bytes));
    CHECK(mbedtls_x509_dn_gets(bytes, sizeof(bytes), &empty) == 0);
    CHECK(bytes[0] == 0 && bytes[1] == 'X');
    bytes[0] = 'X';
    CHECK(mbedtls_x509_dn_gets(bytes, 1, NULL) == 0 && bytes[0] == 0);
    bytes[0] = 'X';
    CHECK(mbedtls_x509_dn_gets(bytes, 0, &empty) == MBEDTLS_ERR_X509_BUFFER_TOO_SMALL);
    CHECK(bytes[0] == 'X');
    normal.oid.p = (unsigned char *) MBEDTLS_OID_AT_CN;
    normal.oid.len = MBEDTLS_OID_SIZE(MBEDTLS_OID_AT_CN);
    normal.val.tag = MBEDTLS_ASN1_UTF8_STRING;
    normal.val.p = (unsigned char *) "example";
    normal.val.len = 7;
    CHECK(mbedtls_x509_dn_gets(bytes, sizeof(bytes), &normal) == 10);
    CHECK(strcmp(bytes, "CN=example") == 0);
}

int main(void)
{
    CHECK(mbedtls_platform_set_calloc_free(test_calloc, test_free) == 0);
    CHECK(psa_crypto_init() == PSA_SUCCESS);
    hello_versions();
    certificate_requests();
    session_lengths();
    transform_failures();
    distinguished_names();
    mbedtls_psa_crypto_free();
    CHECK(owned == 0);
    if (failures != 0) return 1;
    puts("TLS client safety: malformed inputs rejected, cleanup and valid controls passed");
    return 0;
}
