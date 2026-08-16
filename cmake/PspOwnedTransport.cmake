include(ExternalProject)

set(TILEFINCH_PSP_TRANSPORT_MODE "OWNED" CACHE STRING
    "PSP live-network transport: OWNED or LEGACY")
set_property(CACHE TILEFINCH_PSP_TRANSPORT_MODE PROPERTY STRINGS OWNED LEGACY)
option(TILEFINCH_PSP_HTTP2
    "Enable nghttp2-backed HTTP/2 with HTTP/1.1 fallback on PSP" ON)
# Allegrex maddu multiply-accumulate core for mbed TLS's bignum inner loop
# (patches/mbedtls-3.6.6-psp-bnmul.patch). Mbed TLS 3.6.6 already ships a
# generic MIPS32 MULADDC block and that block is already active on this
# target; this option swaps it for an Allegrex-specific one. OFF until the
# crypto selftest EBOOT has run green under PPSSPP for the configuration
# being shipped -- a wrong carry here corrupts signatures silently, so the
# default may only flip with that gate passed. See
# docs/engineering/PSP_TRANSPORT.md.
option(TILEFINCH_PSP_ALLEGREX_BIGNUM_ASM
    "Use the Allegrex maddu MULADDC core in mbed TLS instead of the stock MIPS32 one"
    OFF)
# Project Everest's formally-verified Curve25519 for the x25519 key exchange
# (docs/engineering/PSP_TRANSPORT.md). Mbed TLS 3.6.6 always builds
# libeverest.a, but with MBEDTLS_ECDH_VARIANT_EVEREST_ENABLED off the everest
# translation units compile to empty objects and x25519 runs the generic ECP
# ladder. Defining the macro for the whole mbedTLS build (the same -D
# mechanism the bignum core uses) makes ecdh route Curve25519 through the
# HACL* scalar-mult. Unlike the hand-written bignum asm this is verified
# upstream code that computes the identical X25519 output, so it defaults ON;
# the crypto-selftest EBOOT (RFC 7748 known-answer vectors, Everest vs the
# generic path) is the gate, and a hardware run is the last confirmation.
option(TILEFINCH_PSP_EVEREST_X25519
    "Use Project Everest's verified Curve25519 for mbed TLS x25519 ECDH"
    ON)
set(TILEFINCH_PSP_TRANSPORT_CACHE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cache" CACHE PATH
    "Offline cache populated by scripts/fetch-psp-transport-deps.sh")

if(NOT TILEFINCH_PSP_TRANSPORT_MODE MATCHES "^(OWNED|LEGACY)$")
    message(FATAL_ERROR
        "TILEFINCH_PSP_TRANSPORT_MODE must be OWNED or LEGACY")
endif()

set(TILEFINCH_PSP_TRANSPORT_IS_OWNED OFF)
set(TILEFINCH_PSP_TRANSPORT_LIBRARIES
    curl mbedtls mbedx509 mbedcrypto z)
set(TILEFINCH_PSP_CRYPTO_LIBRARIES mbedcrypto)
set(TILEFINCH_PSP_TRANSPORT_DEPENDENCY "")

if(NOT PSP OR PSP_BROWSER_CURL_STUB
   OR NOT TILEFINCH_PSP_TRANSPORT_MODE STREQUAL "OWNED")
    if(PSP AND NOT PSP_BROWSER_CURL_STUB)
        message(WARNING
            "PSP transport: using legacy SDK curl/TLS by explicit request")
    endif()
    return()
endif()

set(TILEFINCH_PSP_TRANSPORT_IS_OWNED ON)
set(_transport_prefix "${CMAKE_CURRENT_BINARY_DIR}/psp-transport/prefix")
set(_transport_lock
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/psp_transport/dependencies.lock")
set(_mbedtls_archive "${TILEFINCH_PSP_TRANSPORT_CACHE}/mbedtls-3.6.6.tar.bz2")
set(_curl_archive "${TILEFINCH_PSP_TRANSPORT_CACHE}/curl-8.21.0.tar.xz")
set(_nghttp2_archive
    "${TILEFINCH_PSP_TRANSPORT_CACHE}/nghttp2-1.69.0.tar.xz")

if(NOT EXISTS "${_transport_lock}")
    message(FATAL_ERROR "Missing PSP transport lock file: ${_transport_lock}")
endif()
file(READ "${_transport_lock}" _transport_lock_contents)
foreach(_locked_dependency IN ITEMS
        "curl|8.21.0|curl-8.21.0.tar.xz|aa1b66a70eace83dc624508745646c08ae561de512ab403adffb93ac87fc72e6|https://curl.se/download/curl-8.21.0.tar.xz"
        "mbedtls|3.6.6|mbedtls-3.6.6.tar.bz2|8fb65fae8dcae5840f793c0a334860a411f884cc537ea290ce1c52bb64ca007a|https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.6/mbedtls-3.6.6.tar.bz2"
        "nghttp2|1.69.0|nghttp2-1.69.0.tar.xz|1fb324b6ec2c56f6bde0658f4139ffd8209fa9e77ce98fd7a5f63af8d0e508ad|https://github.com/nghttp2/nghttp2/releases/download/v1.69.0/nghttp2-1.69.0.tar.xz")
    string(FIND "${_transport_lock_contents}" "${_locked_dependency}"
        _locked_dependency_at)
    if(_locked_dependency_at EQUAL -1)
        message(FATAL_ERROR
            "PSP transport lock and CMake dependency declarations disagree")
    endif()
endforeach()

foreach(_archive IN ITEMS "${_mbedtls_archive}" "${_curl_archive}")
    if(NOT EXISTS "${_archive}")
        message(FATAL_ERROR
            "Missing PSP transport source ${_archive}. Run scripts/fetch-psp-transport-deps.sh first.")
    endif()
endforeach()
if(TILEFINCH_PSP_HTTP2 AND NOT EXISTS "${_nghttp2_archive}")
    message(FATAL_ERROR
        "Missing PSP HTTP/2 source ${_nghttp2_archive}. Run scripts/fetch-psp-transport-deps.sh first.")
endif()

file(MAKE_DIRECTORY "${_transport_prefix}/include")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_transport_lock}"
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/mbedtls-3.6.6-psp.patch"
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/mbedtls-3.6.6-psp-bnmul.patch"
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/curl-8.21.0-psp.patch")

# The bignum patch is applied unconditionally so the extracted tree is the
# same whichever way the option is set; the block it adds is inert unless
# TILEFINCH_ALLEGREX_MULADDC is defined, and the stock MIPS32 block stays
# byte-for-byte upstream and takes over when it is not.
set(_mbedtls_bignum_flags "")
if(TILEFINCH_PSP_ALLEGREX_BIGNUM_ASM)
    set(_mbedtls_bignum_flags " -DTILEFINCH_ALLEGREX_MULADDC=1")
endif()

# Everest is selected by defining MBEDTLS_ECDH_VARIANT_EVEREST_ENABLED for the
# whole mbedTLS build; a command-line -D is seen by mbedtls_config.h and by the
# guarded everest translation units alike. It changes the mbedtls_ecdh_context
# layout, so consumers that touch that struct must be built with the same macro
# (the crypto-selftest EBOOT is, in cmake/TilefinchTargets.cmake); curl only
# uses the high-level SSL API and never sees the context.
set(_mbedtls_everest_flags "")
if(TILEFINCH_PSP_EVEREST_X25519)
    set(_mbedtls_everest_flags " -DMBEDTLS_ECDH_VARIANT_EVEREST_ENABLED=1")
endif()

ExternalProject_Add(tilefinch_psp_mbedtls
    URL "${_mbedtls_archive}"
    URL_HASH
        SHA256=8fb65fae8dcae5840f793c0a334860a411f884cc537ea290ce1c52bb64ca007a
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    PREFIX "${CMAKE_CURRENT_BINARY_DIR}/psp-transport/mbedtls"
    PATCH_COMMAND
        "${CMAKE_COMMAND}"
        "-DPATCH_SOURCE_DIR=<SOURCE_DIR>"
        "-DPATCH_FILE=${CMAKE_CURRENT_SOURCE_DIR}/patches/mbedtls-3.6.6-psp.patch"
        "-DPATCH_EXECUTABLE=${PATCH_EXECUTABLE}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/apply_patch.cmake"
        COMMAND
        "${CMAKE_COMMAND}"
        "-DPATCH_SOURCE_DIR=<SOURCE_DIR>"
        "-DPATCH_FILE=${CMAKE_CURRENT_SOURCE_DIR}/patches/mbedtls-3.6.6-psp-bnmul.patch"
        "-DPATCH_EXECUTABLE=${PATCH_EXECUTABLE}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/apply_patch.cmake"
    CMAKE_ARGS
        "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
        "-DCMAKE_INSTALL_PREFIX=${_transport_prefix}"
        "-DCMAKE_BUILD_TYPE=MinSizeRel"
        "-DCMAKE_C_FLAGS=-G0 -D_DEFAULT_SOURCE -ffunction-sections -fdata-sections${_mbedtls_bignum_flags}${_mbedtls_everest_flags}"
        "-DENABLE_PROGRAMS=OFF"
        "-DENABLE_TESTING=OFF"
        "-DMBEDTLS_USER_CONFIG_FILE=${CMAKE_CURRENT_SOURCE_DIR}/cmake/psp_transport/mbedtls_user_config.h"
    BUILD_BYPRODUCTS
        "${_transport_prefix}/lib/libmbedcrypto.a"
        "${_transport_prefix}/lib/libmbedtls.a"
        "${_transport_prefix}/lib/libmbedx509.a"
        "${_transport_prefix}/lib/libeverest.a"
        "${_transport_prefix}/lib/libp256m.a")

set(_nghttp2_dependency "")
set(_nghttp2_library "")
if(TILEFINCH_PSP_HTTP2)
    ExternalProject_Add(tilefinch_psp_nghttp2
        URL "${_nghttp2_archive}"
        URL_HASH
            SHA256=1fb324b6ec2c56f6bde0658f4139ffd8209fa9e77ce98fd7a5f63af8d0e508ad
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        PREFIX "${CMAKE_CURRENT_BINARY_DIR}/psp-transport/nghttp2"
        CMAKE_ARGS
            "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
            "-DCMAKE_INSTALL_PREFIX=${_transport_prefix}"
            "-DCMAKE_BUILD_TYPE=MinSizeRel"
            "-DCMAKE_C_FLAGS=-G0 -ffunction-sections -fdata-sections"
            "-DBUILD_SHARED_LIBS=OFF"
            "-DBUILD_STATIC_LIBS=ON"
            "-DENABLE_LIB_ONLY=ON"
            "-DENABLE_THREADS=OFF"
            "-DENABLE_APP=OFF"
            "-DENABLE_EXAMPLES=OFF"
            "-DENABLE_HPACK_TOOLS=OFF"
            "-DENABLE_HTTP3=OFF"
        BUILD_BYPRODUCTS "${_transport_prefix}/lib/libnghttp2.a")
    set(_nghttp2_dependency tilefinch_psp_nghttp2)
    set(_nghttp2_library "${_transport_prefix}/lib/libnghttp2.a")
endif()

set(_curl_dependencies tilefinch_psp_mbedtls)
set(_curl_nghttp2_args "-DUSE_NGHTTP2=OFF")
if(_nghttp2_dependency)
    list(APPEND _curl_dependencies "${_nghttp2_dependency}")
    set(_curl_nghttp2_args
        "-DUSE_NGHTTP2=ON"
        "-DNGHTTP2_INCLUDE_DIR=${_transport_prefix}/include"
        "-DNGHTTP2_LIBRARY=${_nghttp2_library}")
endif()
ExternalProject_Add(tilefinch_psp_curl
    URL "${_curl_archive}"
    URL_HASH
        SHA256=aa1b66a70eace83dc624508745646c08ae561de512ab403adffb93ac87fc72e6
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    PREFIX "${CMAKE_CURRENT_BINARY_DIR}/psp-transport/curl"
    DEPENDS ${_curl_dependencies}
    PATCH_COMMAND
        "${CMAKE_COMMAND}"
        "-DPATCH_SOURCE_DIR=<SOURCE_DIR>"
        "-DPATCH_FILE=${CMAKE_CURRENT_SOURCE_DIR}/patches/curl-8.21.0-psp.patch"
        "-DPATCH_EXECUTABLE=${PATCH_EXECUTABLE}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/apply_patch.cmake"
    CMAKE_ARGS
        "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
        "-DCMAKE_INSTALL_PREFIX=${_transport_prefix}"
        "-DCMAKE_BUILD_TYPE=MinSizeRel"
        "-DCMAKE_C_FLAGS=-G0 -D_DEFAULT_SOURCE -ffunction-sections -fdata-sections"
        "-DCMAKE_PREFIX_PATH=${_transport_prefix}"
        "-DCURL_USE_CMAKECONFIG=OFF"
        "-DCURL_USE_PKGCONFIG=OFF"
        "-DMBEDTLS_INCLUDE_DIR=${_transport_prefix}/include"
        "-DMBEDTLS_LIBRARY=${_transport_prefix}/lib/libmbedtls.a"
        "-DMBEDX509_LIBRARY=${_transport_prefix}/lib/libmbedx509.a"
        "-DMBEDCRYPTO_LIBRARY=${_transport_prefix}/lib/libmbedcrypto.a"
        "-DBUILD_SHARED_LIBS=OFF"
        "-DBUILD_STATIC_LIBS=ON"
        "-DBUILD_CURL_EXE=OFF"
        "-DBUILD_EXAMPLES=OFF"
        "-DBUILD_LIBCURL_DOCS=OFF"
        "-DBUILD_MISC_DOCS=OFF"
        "-DBUILD_TESTING=OFF"
        "-DPICKY_COMPILER=OFF"
        "-DCURL_USE_MBEDTLS=ON"
        "-DCURL_USE_OPENSSL=OFF"
        # Compile in curl_easy_ssls_export/import (OFF upstream) so Tilefinch
        # can serialize TLS sessions to the stick and resume across boots.
        # See docs/engineering/PSP_TRANSPORT.md.
        "-DUSE_SSLS_EXPORT=ON"
        "-DCURL_USE_LIBPSL=OFF"
        "-DCURL_BROTLI=OFF"
        "-DCURL_ZSTD=OFF"
        "-DCURL_ZLIB=ON"
        "-DHTTP_ONLY=ON"
        "-DENABLE_IPV6=OFF"
        "-DENABLE_THREADED_RESOLVER=OFF"
        "-DENABLE_UNIX_SOCKETS=OFF"
        "-DCURL_DISABLE_COOKIES=ON"
        "-DCURL_DISABLE_HSTS=ON"
        "-DCURL_DISABLE_ALTSVC=ON"
        "-DCURL_DISABLE_NETRC=ON"
        "-DCURL_DISABLE_DOH=ON"
        "-DCURL_DISABLE_SOCKETPAIR=ON"
        ${_curl_nghttp2_args}
    BUILD_BYPRODUCTS "${_transport_prefix}/lib/libcurl.a")

add_library(tilefinch_psp_transport INTERFACE)
add_dependencies(tilefinch_psp_transport tilefinch_psp_curl)
target_include_directories(tilefinch_psp_transport INTERFACE
    "${_transport_prefix}/include")
target_compile_definitions(tilefinch_psp_transport INTERFACE
    TILEFINCH_PSP_OWNED_TRANSPORT=1
    TILEFINCH_PSP_CURL_VERSION="8.21.0"
    TILEFINCH_PSP_MBEDTLS_VERSION="3.6.6"
    TILEFINCH_PSP_NGHTTP2_VERSION="1.69.0")
target_link_libraries(tilefinch_psp_transport INTERFACE
    "${_transport_prefix}/lib/libcurl.a")
if(TILEFINCH_PSP_HTTP2)
    target_link_libraries(tilefinch_psp_transport INTERFACE
        "${_transport_prefix}/lib/libnghttp2.a")
    target_compile_definitions(tilefinch_psp_transport INTERFACE
        TILEFINCH_PSP_HTTP2=1)
endif()
target_link_libraries(tilefinch_psp_transport INTERFACE
    "${_transport_prefix}/lib/libmbedtls.a"
    "${_transport_prefix}/lib/libmbedx509.a"
    "${_transport_prefix}/lib/libmbedcrypto.a"
    "${_transport_prefix}/lib/libeverest.a"
    "${_transport_prefix}/lib/libp256m.a"
    z)

add_library(tilefinch_psp_crypto INTERFACE)
add_dependencies(tilefinch_psp_crypto tilefinch_psp_mbedtls)
target_include_directories(tilefinch_psp_crypto INTERFACE
    "${_transport_prefix}/include")
target_link_libraries(tilefinch_psp_crypto INTERFACE
    "${_transport_prefix}/lib/libmbedcrypto.a"
    "${_transport_prefix}/lib/libeverest.a"
    "${_transport_prefix}/lib/libp256m.a")

set(TILEFINCH_PSP_TRANSPORT_LIBRARIES tilefinch_psp_transport)
set(TILEFINCH_PSP_CRYPTO_LIBRARIES tilefinch_psp_crypto)
set(TILEFINCH_PSP_TRANSPORT_DEPENDENCY tilefinch_psp_curl)
message(STATUS
    "PSP transport: project-owned curl 8.21.0 + Mbed TLS 3.6.6, HTTP/2=${TILEFINCH_PSP_HTTP2}")
