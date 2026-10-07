#ifndef TILEFINCH_TEXT_ENCODING_H
#define TILEFINCH_TEXT_ENCODING_H

/* Decode-only support for the WHATWG Encoding Standard's UTF-8, UTF-16 and
   legacy single-byte encodings, and the HTML encoding-sniffing steps that
   choose one for a document (BOM, transport charset, <meta> prescan).
   Everything downstream of a decoder sees UTF-8. Encoders (form submission
   and URL query encoding in a legacy encoding) are not implemented. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Values 1..28 are the standard's single-byte encodings in its order; the
   generated tables (src/text_encoding_tables.inc) index by these ids. */
typedef enum {
    TILEFINCH_ENCODING_UTF8 = 0,
    TILEFINCH_ENCODING_IBM866,
    TILEFINCH_ENCODING_ISO_8859_2,
    TILEFINCH_ENCODING_ISO_8859_3,
    TILEFINCH_ENCODING_ISO_8859_4,
    TILEFINCH_ENCODING_ISO_8859_5,
    TILEFINCH_ENCODING_ISO_8859_6,
    TILEFINCH_ENCODING_ISO_8859_7,
    TILEFINCH_ENCODING_ISO_8859_8,
    TILEFINCH_ENCODING_ISO_8859_8_I,
    TILEFINCH_ENCODING_ISO_8859_10,
    TILEFINCH_ENCODING_ISO_8859_13,
    TILEFINCH_ENCODING_ISO_8859_14,
    TILEFINCH_ENCODING_ISO_8859_15,
    TILEFINCH_ENCODING_ISO_8859_16,
    TILEFINCH_ENCODING_KOI8_R,
    TILEFINCH_ENCODING_KOI8_U,
    TILEFINCH_ENCODING_MACINTOSH,
    TILEFINCH_ENCODING_WINDOWS_874,
    TILEFINCH_ENCODING_WINDOWS_1250,
    TILEFINCH_ENCODING_WINDOWS_1251,
    TILEFINCH_ENCODING_WINDOWS_1252,
    TILEFINCH_ENCODING_WINDOWS_1253,
    TILEFINCH_ENCODING_WINDOWS_1254,
    TILEFINCH_ENCODING_WINDOWS_1255,
    TILEFINCH_ENCODING_WINDOWS_1256,
    TILEFINCH_ENCODING_WINDOWS_1257,
    TILEFINCH_ENCODING_WINDOWS_1258,
    TILEFINCH_ENCODING_X_MAC_CYRILLIC,
    TILEFINCH_ENCODING_UTF16BE,
    TILEFINCH_ENCODING_UTF16LE,
    TILEFINCH_ENCODING_REPLACEMENT,
    TILEFINCH_ENCODING_X_USER_DEFINED,
    /* A real label (GBK, Shift_JIS, ...) whose decoder Tilefinch lacks. */
    TILEFINCH_ENCODING_UNSUPPORTED = 254,
    /* Not a label at all ("get an encoding" failure). */
    TILEFINCH_ENCODING_NONE = 255
} TilefinchEncoding;

/* "Get an encoding": trims ASCII whitespace and matches labels ASCII
   case-insensitively. */
TilefinchEncoding tilefinch_encoding_for_label(const char *label,
                                               size_t length);
/* The encoding's canonical name ("windows-1251"); NULL for NONE. */
const char *tilefinch_encoding_name(TilefinchEncoding encoding);
/* True for encodings this module can decode. */
bool tilefinch_encoding_decodable(TilefinchEncoding encoding);

/* The charset parameter of a Content-Type value, or NONE when it is absent
   or not a label. */
TilefinchEncoding tilefinch_encoding_from_content_type(const char *value);

/* BOM sniffing: UTF-8, UTF-16BE or UTF-16LE with *bom_length set, or NONE.
   Needs up to three bytes; fewer may give a premature NONE. */
TilefinchEncoding tilefinch_encoding_sniff_bom(const unsigned char *data,
                                               size_t length,
                                               size_t *bom_length);

/* HTML's "prescan a byte stream to determine its encoding" over at most the
   first 1024 bytes. Returns NONE when nothing usable was declared; UTF-16
   declarations become UTF-8 and x-user-defined becomes windows-1252, as the
   algorithm requires. */
#define TILEFINCH_ENCODING_PRESCAN_BYTES 1024u
TilefinchEncoding tilefinch_encoding_prescan(const unsigned char *data,
                                             size_t length);

/* Strict UTF-8 well-formedness (no overlongs, surrogates or values above
   U+10FFFF). */
bool tilefinch_utf8_valid(const unsigned char *data, size_t length);

/* A streaming decoder to UTF-8. Errors decode to U+FFFD. */
typedef struct {
    uint8_t encoding;
    bool has_lead_byte;
    uint8_t lead_byte;
    bool replacement_done;
    uint16_t lead_surrogate;
} TilefinchDecoder;

void tilefinch_decoder_init(TilefinchDecoder *decoder,
                            TilefinchEncoding encoding);
/* Decodes from *input, advancing it and *input_length, into output, and
   returns the bytes written. Stops when output may lack room for the next
   step (keep at least 8 bytes free; tilefinch_decoder_maximum_output sizes
   a whole-buffer decode). With flush, a pending partial UTF-16 unit is
   emitted as U+FFFD once all input is consumed. */
size_t tilefinch_decoder_decode(TilefinchDecoder *decoder,
                                const unsigned char **input,
                                size_t *input_length,
                                unsigned char *output, size_t capacity,
                                bool flush);
/* The most output tilefinch_decoder_decode can produce for length input
   bytes, saturating at SIZE_MAX. */
size_t tilefinch_decoder_maximum_output(TilefinchEncoding encoding,
                                        size_t length);

#endif
