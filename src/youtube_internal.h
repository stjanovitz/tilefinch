#ifndef TILEFINCH_YOUTUBE_INTERNAL_H
#define TILEFINCH_YOUTUBE_INTERNAL_H

/* Small checks shared by the lite and full resolvers, which build requests
   from untrusted page and API values. */

#include "tilefinch/url.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* Printable ASCII only: safe to place in a request header field value. */
static inline bool youtube_header_value_safe(const char *value)
{
    if (value == NULL) return false;
    for (const unsigned char *at = (const unsigned char *) value;
         *at != '\0'; at++) {
        if (*at < 0x20u || *at >= 0x7fu) return false;
    }
    return true;
}

/* Non-empty [A-Za-z0-9_-]+ (plus '.' when allow_dot): API keys and client
   versions copied into URLs and headers. */
static inline bool youtube_api_token_safe(const char *value, bool allow_dot)
{
    if (value == NULL || value[0] == '\0') return false;
    for (const unsigned char *at = (const unsigned char *) value;
         *at != '\0'; at++) {
        if (!isalnum(*at) && *at != '-' && *at != '_'
            && (!allow_dot || *at != '.')) return false;
    }
    return true;
}

static inline bool youtube_host_is(const TilefinchUrl *url, const char *host)
{
    return strlen(host) == url->host_length
        && strncasecmp(url->value + url->host_offset,
                       host, url->host_length) == 0;
}

static inline void youtube_error(char *error, size_t error_size,
                                 const char *format, ...)
{
    if (error == NULL || error_size == 0) return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

#endif
