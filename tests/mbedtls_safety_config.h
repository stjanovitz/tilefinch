#ifndef TILEFINCH_TEST_MBEDTLS_SAFETY_CONFIG_H
#define TILEFINCH_TEST_MBEDTLS_SAFETY_CONFIG_H

/* Host-only fault injection; never used by the PSP transport build. */
#define MBEDTLS_PLATFORM_MEMORY

#endif
