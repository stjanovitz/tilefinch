#include "tilefinch/text_encoding.h"

#include <string.h>

#include "text_encoding_tables.inc"

_Static_assert(TILEFINCH_SINGLE_BYTE_ENCODING_COUNT
                   == TILEFINCH_ENCODING_X_MAC_CYRILLIC,
               "single-byte ids follow the Encoding Standard's order");

static bool encoding_is_space(unsigned char value)
{
    return value == 0x09 || value == 0x0a || value == 0x0c
        || value == 0x0d || value == 0x20;
}

static unsigned char encoding_lower(unsigned char value)
{
    return value >= 'A' && value <= 'Z' ? (unsigned char) (value + 32)
                                        : value;
}

TilefinchEncoding tilefinch_encoding_for_label(const char *label,
                                               size_t length)
{
    if (label == NULL) return TILEFINCH_ENCODING_NONE;
    while (length != 0 && encoding_is_space((unsigned char) label[0])) {
        label++;
        length--;
    }
    while (length != 0
           && encoding_is_space((unsigned char) label[length - 1])) {
        length--;
    }
    /* The longest label is 19 bytes ("iso_8859-1:1987" and friends are
       shorter); anything longer cannot match. */
    if (length == 0 || length > 32) return TILEFINCH_ENCODING_NONE;
    const char *entry = encoding_labels;
    for (size_t index = 0; index < sizeof(encoding_label_ids); index++) {
        size_t entry_length = strlen(entry);
        if (entry_length == length) {
            size_t at = 0;
            while (at < length
                   && encoding_lower((unsigned char) label[at])
                          == (unsigned char) entry[at]) {
                at++;
            }
            if (at == length) {
                return (TilefinchEncoding) encoding_label_ids[index];
            }
        }
        entry += entry_length + 1u;
    }
    return TILEFINCH_ENCODING_NONE;
}

const char *tilefinch_encoding_name(TilefinchEncoding encoding)
{
    if (encoding >= TILEFINCH_ENCODING_IBM866
        && encoding <= TILEFINCH_ENCODING_X_MAC_CYRILLIC) {
        return single_byte_names[encoding - TILEFINCH_ENCODING_IBM866];
    }
    switch (encoding) {
    case TILEFINCH_ENCODING_UTF8: return "UTF-8";
    case TILEFINCH_ENCODING_UTF16BE: return "UTF-16BE";
    case TILEFINCH_ENCODING_UTF16LE: return "UTF-16LE";
    case TILEFINCH_ENCODING_REPLACEMENT: return "replacement";
    case TILEFINCH_ENCODING_X_USER_DEFINED: return "x-user-defined";
    case TILEFINCH_ENCODING_UNSUPPORTED: return "unsupported";
    default: return NULL;
    }
}

bool tilefinch_encoding_decodable(TilefinchEncoding encoding)
{
    return encoding <= TILEFINCH_ENCODING_X_USER_DEFINED;
}

TilefinchEncoding tilefinch_encoding_from_content_type(const char *value)
{
    if (value == NULL) return TILEFINCH_ENCODING_NONE;
    /* Parameters follow the essence: ";" name "=" (token | quoted). The
       first charset parameter wins (MIME Sniffing, "parse a MIME type"). */
    const char *at = strchr(value, ';');
    while (at != NULL) {
        at++;
        while (encoding_is_space((unsigned char) *at)) at++;
        const char *name = at;
        while (*at != '\0' && *at != ';' && *at != '=') at++;
        size_t name_length = (size_t) (at - name);
        if (*at != '=') {
            at = *at == ';' ? at : NULL;
            continue;
        }
        at++;
        const char *parameter = at;
        size_t parameter_length = 0;
        if (*at == '"') {
            parameter = ++at;
            while (*at != '\0' && *at != '"') {
                if (*at == '\\' && at[1] != '\0') at++;
                at++;
            }
            parameter_length = (size_t) (at - parameter);
            if (*at == '"') at++;
            while (*at != '\0' && *at != ';') at++;
        } else {
            while (*at != '\0' && *at != ';') at++;
            parameter_length = (size_t) (at - parameter);
        }
        if (name_length == 7) {
            static const char charset[] = "charset";
            size_t i = 0;
            while (i < 7
                   && encoding_lower((unsigned char) name[i])
                          == (unsigned char) charset[i]) {
                i++;
            }
            if (i == 7) {
                return tilefinch_encoding_for_label(parameter,
                                                    parameter_length);
            }
        }
        if (*at != ';') break;
    }
    return TILEFINCH_ENCODING_NONE;
}

TilefinchEncoding tilefinch_encoding_sniff_bom(const unsigned char *data,
                                               size_t length,
                                               size_t *bom_length)
{
    if (bom_length != NULL) *bom_length = 0;
    if (data == NULL) return TILEFINCH_ENCODING_NONE;
    if (length >= 3 && data[0] == 0xef && data[1] == 0xbb
        && data[2] == 0xbf) {
        if (bom_length != NULL) *bom_length = 3;
        return TILEFINCH_ENCODING_UTF8;
    }
    if (length >= 2 && data[0] == 0xfe && data[1] == 0xff) {
        if (bom_length != NULL) *bom_length = 2;
        return TILEFINCH_ENCODING_UTF16BE;
    }
    if (length >= 2 && data[0] == 0xff && data[1] == 0xfe) {
        if (bom_length != NULL) *bom_length = 2;
        return TILEFINCH_ENCODING_UTF16LE;
    }
    return TILEFINCH_ENCODING_NONE;
}

/* --- HTML "prescan a byte stream to determine its encoding" --- */

typedef struct {
    const unsigned char *data;
    size_t length;
    size_t at;
} Prescan;

typedef struct {
    char name[16];
    size_t name_length;
    bool name_truncated;
    char value[128];
    size_t value_length;
    bool value_truncated;
} PrescanAttribute;

static void prescan_append(char *buffer, size_t capacity, size_t *length,
                           bool *truncated, unsigned char value)
{
    if (*length + 1u < capacity) buffer[(*length)++] = (char) value;
    else *truncated = true;
}

/* "Get an attribute". Returns false when none was found (at '>') or the
   bytes ran out (*exhausted). */
static bool prescan_attribute(Prescan *scan, PrescanAttribute *attribute,
                              bool *exhausted)
{
    memset(attribute, 0, sizeof(*attribute));
    const unsigned char *data = scan->data;
    size_t length = scan->length;
    while (scan->at < length
           && (encoding_is_space(data[scan->at]) || data[scan->at] == '/')) {
        scan->at++;
    }
    if (scan->at >= length) {
        *exhausted = true;
        return false;
    }
    if (data[scan->at] == '>') return false;
    /* Attribute name. */
    for (;;) {
        if (scan->at >= length) {
            *exhausted = true;
            return false;
        }
        unsigned char value = data[scan->at];
        if (value == '=' && attribute->name_length != 0) {
            scan->at++;
            goto attribute_value;
        }
        if (encoding_is_space(value)) break;
        if (value == '/' || value == '>') return true;
        prescan_append(attribute->name, sizeof(attribute->name),
                       &attribute->name_length, &attribute->name_truncated,
                       encoding_lower(value));
        scan->at++;
    }
    /* Spaces after the name. */
    while (scan->at < length && encoding_is_space(data[scan->at])) {
        scan->at++;
    }
    if (scan->at >= length) {
        *exhausted = true;
        return false;
    }
    if (data[scan->at] != '=') return true;
    scan->at++;
attribute_value:
    while (scan->at < length && encoding_is_space(data[scan->at])) {
        scan->at++;
    }
    if (scan->at >= length) {
        *exhausted = true;
        return false;
    }
    if (data[scan->at] == '"' || data[scan->at] == '\'') {
        unsigned char quote = data[scan->at++];
        for (;;) {
            if (scan->at >= length) {
                *exhausted = true;
                return false;
            }
            unsigned char value = data[scan->at++];
            if (value == quote) return true;
            prescan_append(attribute->value, sizeof(attribute->value),
                           &attribute->value_length,
                           &attribute->value_truncated,
                           encoding_lower(value));
        }
    }
    if (data[scan->at] == '>') return true;
    for (;;) {
        if (scan->at >= length) {
            *exhausted = true;
            return false;
        }
        unsigned char value = data[scan->at];
        if (encoding_is_space(value) || value == '>') return true;
        prescan_append(attribute->value, sizeof(attribute->value),
                       &attribute->value_length,
                       &attribute->value_truncated, encoding_lower(value));
        scan->at++;
    }
}

static bool attribute_is(const PrescanAttribute *attribute, const char *name)
{
    size_t length = strlen(name);
    return !attribute->name_truncated && attribute->name_length == length
        && memcmp(attribute->name, name, length) == 0;
}

/* "Extracting a character encoding from a meta element" (content="..."). */
static TilefinchEncoding encoding_from_meta_content(const char *text,
                                                    size_t length)
{
    size_t at = 0;
    for (;;) {
        bool found = false;
        while (at + 7u <= length) {
            if (memcmp(text + at, "charset", 7) == 0) {
                found = true;
                break;
            }
            at++;
        }
        if (!found) return TILEFINCH_ENCODING_NONE;
        at += 7u;
        while (at < length && encoding_is_space((unsigned char) text[at])) {
            at++;
        }
        if (at >= length || text[at] != '=') continue;
        at++;
        while (at < length && encoding_is_space((unsigned char) text[at])) {
            at++;
        }
        if (at >= length) return TILEFINCH_ENCODING_NONE;
        if (text[at] == '"' || text[at] == '\'') {
            char quote = text[at++];
            size_t start = at;
            while (at < length && text[at] != quote) at++;
            if (at >= length) return TILEFINCH_ENCODING_NONE;
            return tilefinch_encoding_for_label(text + start, at - start);
        }
        size_t start = at;
        while (at < length && !encoding_is_space((unsigned char) text[at])
               && text[at] != ';') {
            at++;
        }
        return tilefinch_encoding_for_label(text + start, at - start);
    }
}

static bool prescan_starts(const Prescan *scan, const char *text)
{
    size_t length = strlen(text);
    if (scan->length - scan->at < length) return false;
    for (size_t i = 0; i < length; i++) {
        if (encoding_lower(scan->data[scan->at + i])
            != (unsigned char) text[i]) return false;
    }
    return true;
}

static bool prescan_is_letter(unsigned char value)
{
    value = encoding_lower(value);
    return value >= 'a' && value <= 'z';
}

TilefinchEncoding tilefinch_encoding_prescan(const unsigned char *data,
                                             size_t length)
{
    if (data == NULL) return TILEFINCH_ENCODING_NONE;
    if (length > TILEFINCH_ENCODING_PRESCAN_BYTES) {
        length = TILEFINCH_ENCODING_PRESCAN_BYTES;
    }
    Prescan scan = { data, length, 0 };
    while (scan.at < length) {
        if (prescan_starts(&scan, "<!--")) {
            /* The '>' must follow two dashes; those of "<!--" count. */
            size_t at = scan.at + 2u;
            while (at + 2u < length
                   && !(data[at] == '-' && data[at + 1] == '-'
                        && data[at + 2] == '>')) {
                at++;
            }
            if (at + 2u >= length) return TILEFINCH_ENCODING_NONE;
            scan.at = at + 2u;
        } else if (prescan_starts(&scan, "<meta")
                   && scan.at + 5u < length
                   && (encoding_is_space(data[scan.at + 5u])
                       || data[scan.at + 5u] == '/')) {
            scan.at += 6u;
            char seen[8][16];
            size_t seen_count = 0;
            bool got_pragma = false;
            int need_pragma = -1;
            TilefinchEncoding charset = TILEFINCH_ENCODING_NONE;
            bool have_charset = false;
            PrescanAttribute attribute;
            bool exhausted = false;
            while (prescan_attribute(&scan, &attribute, &exhausted)) {
                bool repeated = false;
                for (size_t i = 0; i < seen_count; i++) {
                    if (strcmp(seen[i], attribute.name) == 0) {
                        repeated = true;
                    }
                }
                if (repeated || attribute.name_truncated) continue;
                if (seen_count < 8) {
                    memcpy(seen[seen_count++], attribute.name,
                           attribute.name_length + 1u);
                }
                if (attribute_is(&attribute, "http-equiv")) {
                    if (!attribute.value_truncated
                        && attribute.value_length == 12
                        && memcmp(attribute.value, "content-type", 12) == 0) {
                        got_pragma = true;
                    }
                } else if (attribute_is(&attribute, "content")) {
                    TilefinchEncoding found = encoding_from_meta_content(
                        attribute.value, attribute.value_length);
                    if (found != TILEFINCH_ENCODING_NONE && !have_charset) {
                        charset = found;
                        have_charset = true;
                        need_pragma = 1;
                    }
                } else if (attribute_is(&attribute, "charset")) {
                    charset = attribute.value_truncated
                        ? TILEFINCH_ENCODING_NONE
                        : tilefinch_encoding_for_label(
                              attribute.value, attribute.value_length);
                    have_charset = true;
                    need_pragma = 0;
                }
            }
            if (exhausted) return TILEFINCH_ENCODING_NONE;
            if (need_pragma == -1 || (need_pragma == 1 && !got_pragma)
                || charset == TILEFINCH_ENCODING_NONE) {
                scan.at++;
                continue;
            }
            if (charset == TILEFINCH_ENCODING_UTF16BE
                || charset == TILEFINCH_ENCODING_UTF16LE) {
                return TILEFINCH_ENCODING_UTF8;
            }
            if (charset == TILEFINCH_ENCODING_X_USER_DEFINED) {
                return TILEFINCH_ENCODING_WINDOWS_1252;
            }
            return charset;
        } else if (scan.at + 1u < length && data[scan.at] == '<'
                   && (prescan_is_letter(data[scan.at + 1u])
                       || (data[scan.at + 1u] == '/'
                           && scan.at + 2u < length
                           && prescan_is_letter(data[scan.at + 2u])))) {
            while (scan.at < length && !encoding_is_space(data[scan.at])
                   && data[scan.at] != '>') {
                scan.at++;
            }
            PrescanAttribute attribute;
            bool exhausted = false;
            while (prescan_attribute(&scan, &attribute, &exhausted)) {
            }
            if (exhausted) return TILEFINCH_ENCODING_NONE;
        } else if (prescan_starts(&scan, "<!") || prescan_starts(&scan, "</")
                   || prescan_starts(&scan, "<?")) {
            while (scan.at < length && data[scan.at] != '>') scan.at++;
            if (scan.at >= length) return TILEFINCH_ENCODING_NONE;
        }
        scan.at++;
    }
    return TILEFINCH_ENCODING_NONE;
}

bool tilefinch_utf8_valid(const unsigned char *data, size_t length)
{
    if (data == NULL) return length == 0;
    size_t at = 0;
    while (at < length) {
        unsigned char lead = data[at];
        if (lead < 0x80u) {
            at++;
            continue;
        }
        size_t need;
        unsigned char low = 0x80u, high = 0xbfu;
        if (lead >= 0xc2u && lead <= 0xdfu) need = 1;
        else if (lead >= 0xe0u && lead <= 0xefu) {
            need = 2;
            if (lead == 0xe0u) low = 0xa0u;
            if (lead == 0xedu) high = 0x9fu;
        } else if (lead >= 0xf0u && lead <= 0xf4u) {
            need = 3;
            if (lead == 0xf0u) low = 0x90u;
            if (lead == 0xf4u) high = 0x8fu;
        } else {
            return false;
        }
        if (length - at <= need) return false;
        if (data[at + 1] < low || data[at + 1] > high) return false;
        for (size_t i = 2; i <= need; i++) {
            if (data[at + i] < 0x80u || data[at + i] > 0xbfu) return false;
        }
        at += need + 1u;
    }
    return true;
}

/* --- Decoders --- */

void tilefinch_decoder_init(TilefinchDecoder *decoder,
                            TilefinchEncoding encoding)
{
    if (decoder == NULL) return;
    memset(decoder, 0, sizeof(*decoder));
    decoder->encoding = tilefinch_encoding_decodable(encoding)
        ? (uint8_t) encoding : (uint8_t) TILEFINCH_ENCODING_UTF8;
}

size_t tilefinch_decoder_maximum_output(TilefinchEncoding encoding,
                                        size_t length)
{
    if (!tilefinch_encoding_decodable(encoding)
        || encoding == TILEFINCH_ENCODING_UTF8) return length;
    if (encoding == TILEFINCH_ENCODING_REPLACEMENT) return 3u;
    /* Single-byte: one code point below U+10000 per byte. UTF-16: at most
       three bytes per two input bytes, plus a flushed partial unit. */
    if (length > (SIZE_MAX - 4u) / 3u) return SIZE_MAX;
    return 3u * length + 4u;
}

static size_t encode_utf8(uint32_t code_point, unsigned char *output)
{
    if (code_point < 0x80u) {
        output[0] = (unsigned char) code_point;
        return 1;
    }
    if (code_point < 0x800u) {
        output[0] = (unsigned char) (0xc0u | (code_point >> 6));
        output[1] = (unsigned char) (0x80u | (code_point & 0x3fu));
        return 2;
    }
    if (code_point < 0x10000u) {
        output[0] = (unsigned char) (0xe0u | (code_point >> 12));
        output[1] = (unsigned char) (0x80u | ((code_point >> 6) & 0x3fu));
        output[2] = (unsigned char) (0x80u | (code_point & 0x3fu));
        return 3;
    }
    output[0] = (unsigned char) (0xf0u | (code_point >> 18));
    output[1] = (unsigned char) (0x80u | ((code_point >> 12) & 0x3fu));
    output[2] = (unsigned char) (0x80u | ((code_point >> 6) & 0x3fu));
    output[3] = (unsigned char) (0x80u | (code_point & 0x3fu));
    return 4;
}

size_t tilefinch_decoder_decode(TilefinchDecoder *decoder,
                                const unsigned char **input,
                                size_t *input_length,
                                unsigned char *output, size_t capacity,
                                bool flush)
{
    if (decoder == NULL || input == NULL || input_length == NULL
        || (*input == NULL && *input_length != 0) || output == NULL) {
        return 0;
    }
    const unsigned char *in = *input;
    size_t remaining = *input_length;
    size_t written = 0;
    uint8_t encoding = decoder->encoding;
    if (encoding == TILEFINCH_ENCODING_UTF8) {
        size_t copy = remaining < capacity ? remaining : capacity;
        if (copy != 0) memcpy(output, in, copy);
        in += copy;
        remaining -= copy;
        written = copy;
    } else if (encoding == TILEFINCH_ENCODING_REPLACEMENT) {
        /* The whole stream decodes to one U+FFFD. */
        if (remaining != 0 && !decoder->replacement_done
            && capacity >= 3u) {
            written = encode_utf8(0xfffdu, output);
            decoder->replacement_done = true;
        }
        if (decoder->replacement_done) {
            in += remaining;
            remaining = 0;
        }
    } else if (encoding == TILEFINCH_ENCODING_UTF16BE
               || encoding == TILEFINCH_ENCODING_UTF16LE) {
        bool big_endian = encoding == TILEFINCH_ENCODING_UTF16BE;
        /* Room for an unpaired lead's U+FFFD plus the unit after it. */
        while (remaining != 0 && capacity - written >= 7u) {
            unsigned char byte = *in++;
            remaining--;
            if (!decoder->has_lead_byte) {
                decoder->has_lead_byte = true;
                decoder->lead_byte = byte;
                continue;
            }
            decoder->has_lead_byte = false;
            uint32_t unit = big_endian
                ? ((uint32_t) decoder->lead_byte << 8) | byte
                : ((uint32_t) byte << 8) | decoder->lead_byte;
            if (decoder->lead_surrogate != 0) {
                uint32_t lead = decoder->lead_surrogate;
                decoder->lead_surrogate = 0;
                if (unit >= 0xdc00u && unit <= 0xdfffu) {
                    written += encode_utf8(
                        0x10000u + ((lead - 0xd800u) << 10)
                            + (unit - 0xdc00u),
                        output + written);
                    continue;
                }
                written += encode_utf8(0xfffdu, output + written);
            }
            if (unit >= 0xd800u && unit <= 0xdbffu) {
                decoder->lead_surrogate = (uint16_t) unit;
                continue;
            }
            if (unit >= 0xdc00u && unit <= 0xdfffu) unit = 0xfffdu;
            written += encode_utf8(unit, output + written);
        }
        if (flush && remaining == 0 && capacity - written >= 3u
            && (decoder->has_lead_byte || decoder->lead_surrogate != 0)) {
            written += encode_utf8(0xfffdu, output + written);
            decoder->has_lead_byte = false;
            decoder->lead_surrogate = 0;
        }
    } else {
        const uint16_t *index = encoding == TILEFINCH_ENCODING_X_USER_DEFINED
            ? NULL
            : single_byte_index[single_byte_table[
                  encoding - TILEFINCH_ENCODING_IBM866]];
        while (remaining != 0 && capacity - written >= 3u) {
            unsigned char byte = *in++;
            remaining--;
            if (byte < 0x80u) {
                output[written++] = byte;
                continue;
            }
            uint32_t code_point = index == NULL
                ? 0xf780u + (byte - 0x80u)
                : index[byte - 0x80u];
            if (code_point == 0) code_point = 0xfffdu;
            written += encode_utf8(code_point, output + written);
        }
    }
    *input = in;
    *input_length = remaining;
    return written;
}
