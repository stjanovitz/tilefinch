#ifndef TILEFINCH_JSON_SCAN_H
#define TILEFINCH_JSON_SCAN_H

/* One bounded scanner for untrusted JSON (provider responses, web app
   manifests). It never allocates and never recurses past the caller's depth
   bound; every loop is bounded by the input span.

   One policy for hostile input:
   - whitespace is JSON's own: space, tab, CR and LF;
   - strings reject raw control characters and unknown escapes, and need
     exactly four hex digits after \u; decoding joins a surrogate pair into
     one code point and turns a surrogate without its other half into
     U+FFFD (it is valid JSON; browsers display it that way). Decoding
     refuses \u0000, since a C string cannot hold U+0000: the string is
     still valid JSON (skipping accepts it), but no copy is cut short at
     the NUL and no key holding one aliases a shorter key;
   - scalars are exactly `true`, `false`, `null` or a number in JSON's
     grammar (-?(0|[1-9][0-9]*)(.[0-9]+)?([eE][+-]?[0-9]+)?), checked by
     its characters: nothing is converted, so no floating point is used;
   - a value may nest at most `depth` levels: a value at remaining depth 0
     is refused, and each container passes depth - 1 to its members. */

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const char *at;
    const char *end;
} JsonCursor;

/* Flags for json_scan_string_copy. */
enum {
    /* Copy the longest prefix that fits, cut on a UTF-8 boundary, instead
       of failing; the rest of the string is still validated. */
    JSON_SCAN_TRUNCATE = 1u
};

/* Skips JSON whitespace. */
void json_scan_space(JsonCursor *json);

/* The string at the cursor (after whitespace), decoded to UTF-8 into
   `output` (NUL-terminated). False when the string is malformed or holds
   \u0000, or does not
   fit `output_size` without JSON_SCAN_TRUNCATE; the cursor is then
   unspecified. On success the cursor is past the closing quote and
   *truncated (when not NULL) says whether a prefix was kept. */
bool json_scan_string_copy(JsonCursor *json, char *output, size_t output_size,
                           unsigned flags, bool *truncated);
/* json_scan_string_copy without truncation. */
bool json_scan_string(JsonCursor *json, char *output, size_t output_size);
/* Validates and steps over the string at the cursor (after whitespace). */
bool json_scan_skip_string(JsonCursor *json);

/* Validates and steps over one value at the cursor (after whitespace),
   nesting at most `depth` levels. */
bool json_scan_skip_value(JsonCursor *json, unsigned depth);

/* The bytes of the value at the cursor (after whitespace), from its first
   byte to just past its last, validated as json_scan_skip_value does; the
   cursor moves past it. */
bool json_scan_value_span(JsonCursor *json, unsigned depth,
                          JsonCursor *span);

/* An object member key followed by its colon: the key is decoded into `key`
   and the cursor is left at the value. */
bool json_scan_member_key(JsonCursor *json, char *key, size_t key_size);

/* The value of the immediate member `key` of the object at the start of
   `object` (after whitespace), as a cursor over the rest of the object
   starting at the value; `depth` bounds the members skipped on the way.
   Keys longer than 63 bytes, or holding \u0000, never match. A duplicate
   key yields its first member (JSON.parse keeps the last). At most
   `member_limit` members are examined. False when the object is malformed before the member is
   found, or has no such member. */
bool json_scan_member(JsonCursor object, const char *key, unsigned depth,
                      unsigned member_limit, JsonCursor *value);

/* The span of the object at the cursor (after whitespace): from its '{' to
   just past its '}', validated to `depth`. False for any other value. */
bool json_scan_object_span(JsonCursor *json, unsigned depth,
                           JsonCursor *span);

#endif
