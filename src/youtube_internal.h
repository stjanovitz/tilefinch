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

/* Buffer sizes, including the terminator, for the account-scoped IDs the
   provider copies out of pages and URLs. A playlist ID is bounded only by
   this buffer; a channel ID is exactly "UC" plus 22 characters. */
#define YOUTUBE_ACCOUNT_ID_CAPACITY 128u
#define YOUTUBE_CHANNEL_ID_CAPACITY 25u

/* One '&'-separated field of a URL query or fragment. `value` is NULL when
   the field has no '='. */
typedef struct {
    const char *name;
    size_t name_length;
    const char *value;
    const char *value_end;
} YoutubeQueryField;

/* The one walker over query and fragment fields: reads the field at
   *cursor and advances past it. */
static inline bool youtube_query_next(const char **cursor, const char *end,
                                      YoutubeQueryField *field)
{
    const char *at = *cursor;
    if (at >= end) return false;
    const char *field_end = memchr(at, '&', (size_t) (end - at));
    if (field_end == NULL) field_end = end;
    const char *equals = memchr(at, '=', (size_t) (field_end - at));
    field->name = at;
    field->name_length = (size_t) ((equals != NULL ? equals : field_end) - at);
    field->value = equals != NULL ? equals + 1 : NULL;
    field->value_end = field_end;
    *cursor = field_end < end ? field_end + 1 : end;
    return true;
}

/* A `name=...` field, with or without a value. */
static inline bool youtube_query_field_is(const YoutubeQueryField *field,
                                          const char *name)
{
    size_t length = strlen(name);
    return field->value != NULL && field->name_length == length
        && memcmp(field->name, name, length) == 0;
}

/* The latest start time a link or a renderer may ask for: one week. */
#define YOUTUBE_START_MAXIMUM_SECONDS 604800u

/* Parses a start time, bare seconds ("754") or the share-link unit form
   ("754s", "12m34s", "1h2m3s"), into whole seconds. False for anything
   else, including a time past YOUTUBE_START_MAXIMUM_SECONDS. Implemented
   in youtube_resolver.c. */
bool youtube_start_seconds_parse(const char *value, const char *end,
                                 unsigned *seconds);

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
