#include "tilefinch/psp_entropy.h"

#include <mbedtls/entropy.h>

#include <stddef.h>

#if !defined(MBEDTLS_ENTROPY_HARDWARE_ALT) \
    || !defined(MBEDTLS_NO_PLATFORM_ENTROPY)
#error "the owned mbed TLS configuration must route entropy through this hook"
#endif

/*
 * mbedtls_entropy_init() registers this as a strong source
 * (cmake/psp_transport/mbedtls_user_config.h), for every legacy entropy
 * context and for the one inside PSA's RNG. Every returned byte is counted
 * as entropy by mbed TLS, which is sound only because psp_entropy_fill
 * releases nothing until the pool holds PSP_ENTROPY_TARGET_BITS of credited
 * timing jitter; after that its output is a hash-based generator keyed by
 * that pool, as getrandom() is after the kernel's pool is initialized.
 * (A process configured PSP_ENTROPY_PERMIT_UNCREDITED is released after one
 * short attempt instead, and says so in its report line.)
 * The prototype lives in mbed TLS's private library/entropy_poll.h.
 */
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t length,
                          size_t *output_length);

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t length,
                          size_t *output_length)
{
    (void) data;
    *output_length = 0;
    if (!psp_entropy_fill(output, length))
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    *output_length = length;
    return 0;
}
