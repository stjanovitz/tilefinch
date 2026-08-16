#include "tilefinch/update.h"

#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/version.h>
#if MBEDTLS_VERSION_MAJOR >= 3
#include <psa/crypto.h>
#endif

static bool mbedtls_verify(
    void *opaque, const uint8_t public_point[65],
    const uint8_t digest[32], const uint8_t signature[64])
{
    (void) opaque;
#if MBEDTLS_VERSION_MAJOR >= 3
    if (psa_crypto_init() != PSA_SUCCESS) return false;
#endif
    mbedtls_ecp_group group;
    mbedtls_ecp_point point;
    mbedtls_mpi r, s;
    mbedtls_ecp_group_init(&group);
    mbedtls_ecp_point_init(&point);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    int result = mbedtls_ecp_group_load(
        &group, MBEDTLS_ECP_DP_SECP256R1);
    if (result == 0)
        result = mbedtls_ecp_point_read_binary(
            &group, &point, public_point, 65);
    if (result == 0) result = mbedtls_mpi_read_binary(&r, signature, 32);
    if (result == 0)
        result = mbedtls_mpi_read_binary(&s, signature + 32, 32);
    if (result == 0)
        result = mbedtls_ecdsa_verify(
            &group, digest, 32, &point, &r, &s);
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecp_point_free(&point);
    mbedtls_ecp_group_free(&group);
    return result == 0;
}

TilefinchUpdateCrypto tilefinch_update_default_crypto(void)
{
    return (TilefinchUpdateCrypto) {.verify = mbedtls_verify};
}
