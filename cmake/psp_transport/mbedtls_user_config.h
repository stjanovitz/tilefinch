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
/* Code-size trims that cannot change what goes on the wire or which peers
 * verify. Each module below is reachable only through generic dispatch
 * tables (md, cipher, PSA, PEM/PKCS key decryption), so --gc-sections keeps
 * it although no TLS client path selects it:
 * - RIPEMD-160, SHA-3 and NIST key wrap appear in no TLS 1.2/1.3 cipher
 *   suite or signature algorithm the client offers (src/fetch.c lists the
 *   groups and signature algorithms explicitly).
 * - DES only remains for encrypted PEM/PKCS#5/PKCS#12 private keys; Mbed TLS
 *   3.x has no 3DES cipher suites, and the browser loads no private keys.
 * - CRLs are never configured (no CURLOPT_CRLFILE); PKCS#7 needs them and is
 *   unused.
 * - Persistent PSA key storage is file-backed and never used: every key is
 *   a volatile TLS handshake key.
 * - DTLS is never used: libcurl runs TLS over TCP only, and the CID, cookie
 *   and anti-replay code paths are datagram-only.
 * - TLS 1.2 static-PSK key exchanges are filtered out of the ClientHello
 *   unless a PSK is configured, and none ever is. TLS 1.3 resumption PSK
 *   modes are separate and stay enabled.
 * - Only CBC, CTR and the AEAD modes are used by TLS; CFB, OFB and XTS are
 *   reachable only from the generic cipher table. */
#undef MBEDTLS_RIPEMD160_C
#undef MBEDTLS_SHA3_C
#undef MBEDTLS_NIST_KW_C
#undef MBEDTLS_DES_C
#undef MBEDTLS_PKCS5_C
#undef MBEDTLS_PKCS12_C
#undef MBEDTLS_PKCS7_C
#undef MBEDTLS_X509_CRL_PARSE_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_SSL_PROTO_DTLS
#undef MBEDTLS_SSL_DTLS_ANTI_REPLAY
#undef MBEDTLS_SSL_DTLS_HELLO_VERIFY
#undef MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE
#undef MBEDTLS_SSL_DTLS_CONNECTION_ID
#undef MBEDTLS_SSL_COOKIE_C
#undef MBEDTLS_KEY_EXCHANGE_PSK_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_DHE_PSK_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_ECDHE_PSK_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_RSA_PSK_ENABLED
#undef MBEDTLS_CIPHER_MODE_CFB
#undef MBEDTLS_CIPHER_MODE_OFB
#undef MBEDTLS_CIPHER_MODE_XTS
/* Dispatch-kept code with no client consumer:
 * - RSA key generation (MBEDTLS_GENPRIME): psa_generate_key is linked for
 *   TLS 1.3 ECDHE/FFDHE key shares and its dispatch kept the RSA key-pair
 *   case and the prime generator. TLS never generates an RSA key, and
 *   rsa_validate_params is only ever called without an RNG, which skips its
 *   primality test.
 * - Deterministic ECDSA (and with it HMAC_DRBG) is a signing-only nonce
 *   scheme; the client never signs (no own key or certificate is ever
 *   configured, the server role is compiled out).
 * - CMAC: TLS uses HMAC only; CMAC was reachable through psa_mac_* alone.
 * - Cipher paddings other than PKCS#7: TLS 1.2 CBC sets PADDING_NONE, and
 *   cipher_setup's default stays PKCS#7.
 * None of these appears in the ClientHello, the verification path or the
 * serialized-session header. */
#undef MBEDTLS_GENPRIME
#undef MBEDTLS_ECDSA_DETERMINISTIC
#undef MBEDTLS_CMAC_C
#undef MBEDTLS_CIPHER_PADDING_ONE_AND_ZEROS
#undef MBEDTLS_CIPHER_PADDING_ZEROS_AND_LEN
#undef MBEDTLS_CIPHER_PADDING_ZEROS
/* Client credentials, key pinning and file loading. curl never configures
 * a client certificate or key (no CURLOPT_SSLCERT/SSLKEY/KEYPASSWD or their
 * blobs; the curl patch's TILEFINCH_CURL_NO_CLIENT_CERT compiles the loaders
 * out), never pins a public key (CURLOPT_PINNEDPUBLICKEY is the only user of
 * pk_write in the link) and loads its CA bundle from memory with
 * CURLOPT_CAINFO_BLOB and a NULL CAPATH, so nothing reads keys or
 * certificates from files. The X.509 and PEM writers depend on pk_write
 * and were never linked. */
#undef MBEDTLS_PK_WRITE_C
#undef MBEDTLS_X509_CREATE_C
#undef MBEDTLS_X509_CRT_WRITE_C
#undef MBEDTLS_X509_CSR_WRITE_C
#undef MBEDTLS_PEM_WRITE_C
#undef MBEDTLS_FS_IO
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
