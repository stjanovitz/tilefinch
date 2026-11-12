#ifndef TILEFINCH_UTF8_ENCODE_H
#define TILEFINCH_UTF8_ENCODE_H

#include <stddef.h>

/* Encode one Unicode scalar value as UTF-8 into bytes[4]. Returns the byte
   count, or 0 for a surrogate (U+D800..U+DFFF) or a value above U+10FFFF,
   neither of which has a UTF-8 encoding. */
static inline size_t tilefinch_utf8_encode(unsigned codepoint,
                                           unsigned char bytes[4])
{
    if (codepoint <= 0x7fu) {
        bytes[0] = (unsigned char) codepoint;
        return 1;
    }
    if (codepoint <= 0x7ffu) {
        bytes[0] = (unsigned char) (0xc0u | (codepoint >> 6));
        bytes[1] = (unsigned char) (0x80u | (codepoint & 0x3fu));
        return 2;
    }
    if (codepoint >= 0xd800u && codepoint <= 0xdfffu) return 0;
    if (codepoint <= 0xffffu) {
        bytes[0] = (unsigned char) (0xe0u | (codepoint >> 12));
        bytes[1] = (unsigned char) (0x80u | ((codepoint >> 6) & 0x3fu));
        bytes[2] = (unsigned char) (0x80u | (codepoint & 0x3fu));
        return 3;
    }
    if (codepoint > 0x10ffffu) return 0;
    bytes[0] = (unsigned char) (0xf0u | (codepoint >> 18));
    bytes[1] = (unsigned char) (0x80u | ((codepoint >> 12) & 0x3fu));
    bytes[2] = (unsigned char) (0x80u | ((codepoint >> 6) & 0x3fu));
    bytes[3] = (unsigned char) (0x80u | (codepoint & 0x3fu));
    return 4;
}

/* The longest prefix of text[0..length) that does not end inside a character,
   for cutting bounded text: a cut character would draw as a missing glyph. */
static inline size_t tilefinch_utf8_complete_prefix(const char *text,
                                                    size_t length)
{
    if (length == 0) return 0;
    size_t start = length - 1u;
    while (start > 0
           && ((unsigned char) text[start] & 0xc0u) == 0x80u) start--;
    unsigned char lead = (unsigned char) text[start];
    size_t expected = lead < 0x80u ? 1u
        : ((lead & 0xe0u) == 0xc0u ? 2u
        : ((lead & 0xf0u) == 0xe0u ? 3u
        : ((lead & 0xf8u) == 0xf0u ? 4u : 1u)));
    return length - start < expected ? start : length;
}

#endif
