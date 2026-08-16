#ifndef TILEFINCH_URL_H
#define TILEFINCH_URL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TILEFINCH_URL_SERIALIZED_LIMIT 2048
#define TILEFINCH_ORIGIN_SERIALIZED_LIMIT 320

typedef enum {
    TILEFINCH_URL_SCHEME_INVALID = 0,
    TILEFINCH_URL_SCHEME_HTTP,
    TILEFINCH_URL_SCHEME_HTTPS
} TilefinchUrlScheme;

/* A non-owning view over an absolute HTTP(S) URL. Offsets refer to value and
   remain valid only while value remains alive. The effective port is always
   populated, including when the source omitted a port. */
typedef struct {
    const char *value;
    size_t length;
    TilefinchUrlScheme scheme;
    size_t scheme_length;
    size_t authority_offset;
    size_t authority_length;
    size_t host_offset;
    size_t host_length;
    size_t path_offset;
    size_t path_length;
    size_t query_offset;
    size_t query_length;
    size_t fragment_offset;
    size_t fragment_length;
    uint16_t port;
    bool explicit_port;
    bool ipv6_literal;
    bool has_query;
    bool has_fragment;
} TilefinchUrl;

bool tilefinch_url_parse(const char *value, TilefinchUrl *url);
bool tilefinch_url_is_secure(const TilefinchUrl *url);
/* Secure-context transport gate used for fingerprinting-sensitive browser
   metadata. HTTPS is trustworthy; HTTP is accepted only for localhost and
   loopback fixture/development origins. */
bool tilefinch_url_potentially_trustworthy(const char *value);
bool tilefinch_url_same_origin(const char *left, const char *right);
bool tilefinch_url_is_downgrade(const char *source, const char *target);
bool tilefinch_url_origin(const char *value, char *output, size_t output_size);
bool tilefinch_url_normalize(const char *value, char *output,
                          size_t output_size);
/* Canonical network/cache identity: normalized HTTP(S) URL without a
   fragment, since fragments are never part of an HTTP request target. */
bool tilefinch_url_request_key(const char *value, char *output,
                            size_t output_size);
bool tilefinch_url_resolve(const char *base, const char *reference,
                        char *output, size_t output_size);
/* Rewrites an absolute HTTP URL to HTTPS without a downgrade fallback.
   An explicit :80 is mapped to :443; other explicit ports are retained. */
bool tilefinch_url_upgrade_to_https(const char *value, char *output,
                                    size_t output_size);

/* Schemeful site identity: scheme plus the PSL-derived registrable domain.
   Ports do not split a site. IP literals, single-label hosts, and inputs with
   no registrable domain conservatively retain their canonical host. */
bool tilefinch_url_site_key(const char *value, char *output, size_t output_size);

#endif
