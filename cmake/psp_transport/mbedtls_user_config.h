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
#if !defined(MBEDTLS_SSL_CLI_C) || !defined(MBEDTLS_SSL_PROTO_TLS1_2) || \
    !defined(MBEDTLS_SSL_PROTO_TLS1_3)
#error "Tilefinch requires TLS 1.2 and TLS 1.3 client support"
#endif

#endif
