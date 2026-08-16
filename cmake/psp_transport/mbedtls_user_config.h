#ifndef TILEFINCH_MBEDTLS_USER_CONFIG_H
#define TILEFINCH_MBEDTLS_USER_CONFIG_H

/*
 * libcurl owns sockets and timeouts. Keeping Mbed TLS's standalone networking
 * and alarm helpers would add unreachable PSP portability code.
 */
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C

#endif
