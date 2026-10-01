#ifndef TILEFINCH_MBEDTLS_USER_CONFIG_H
#define TILEFINCH_MBEDTLS_USER_CONFIG_H

/*
 * libcurl owns sockets and timeouts. Keeping Mbed TLS's standalone networking
 * and alarm helpers would add unreachable PSP portability code.
 */
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C

/* Tilefinch is a TLS client. Keep both client protocol versions and all
 * verification support; only remove the server role. This configuration
 * must also reach every consumer of the public SSL structs. */
#undef MBEDTLS_SSL_SRV_C
/* No TLS debug callback is installed by the browser. Retain numeric version
 * reporting and verification diagnostics, but omit debug formatting and the
 * unused compile-time feature-name lookup table. */
#undef MBEDTLS_DEBUG_C
#undef MBEDTLS_VERSION_FEATURES
/* Entropy (docs/engineering/PSP_TRANSPORT.md, "Entropy"). The platform
 * source would call libcglue's getentropy(), which reseeds MT19937 from
 * time() on every call: every DRBG seed, ECDHE key and TLS random would be a
 * function of the wall-clock second. Drop it and register Tilefinch's
 * timing-jitter accumulator (src/psp_entropy_mbedtls.c) through the
 * hardware-poll hook instead. mbedtls_entropy_init() adds that hook for
 * every legacy entropy context and for the one inside PSA's RNG, so curl's
 * CTR_DRBG and psa_generate_random() both depend on it alone. */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#if defined(MBEDTLS_NO_DEFAULT_ENTROPY_SOURCES) || \
    defined(MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG) || \
    defined(MBEDTLS_ENTROPY_NV_SEED)
#error "Tilefinch's PSP entropy hook must be the RNG's only strong source"
#endif
#if !defined(MBEDTLS_SSL_CLI_C) || !defined(MBEDTLS_SSL_PROTO_TLS1_2) || \
    !defined(MBEDTLS_SSL_PROTO_TLS1_3)
#error "Tilefinch requires TLS 1.2 and TLS 1.3 client support"
#endif

#endif
