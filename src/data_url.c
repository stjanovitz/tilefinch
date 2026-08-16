#include "data_url.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

static int hex_digit(unsigned char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    value = (unsigned char) tolower(value);
    return value >= 'a' && value <= 'f' ? value - 'a' + 10 : -1;
}

static int base64_digit(unsigned char value)
{
    if (value >= 'A' && value <= 'Z') return value - 'A';
    if (value >= 'a' && value <= 'z') return value - 'a' + 26;
    if (value >= '0' && value <= '9') return value - '0' + 52;
    if (value == '+') return 62;
    return value == '/' ? 63 : -1;
}

static bool token_equals(const char *start, size_t length, const char *token)
{
    return strlen(token) == length && strncasecmp(start, token, length) == 0;
}

DataUrlDecodeResult data_url_decode(
    Budget *budget, const char *url, size_t url_length,
    size_t maximum_decoded_bytes, unsigned char **data, size_t *length,
    char *media_type, size_t media_type_size)
{
    if (data != NULL) *data = NULL;
    if (length != NULL) *length = 0;
    if (media_type != NULL && media_type_size != 0) media_type[0] = '\0';
    if (budget == NULL || url == NULL || data == NULL || length == NULL
        || media_type == NULL || media_type_size == 0) return DATA_URL_INVALID;
    if (url_length < 5 || strncasecmp(url, "data:", 5) != 0) {
        return DATA_URL_NOT_DATA;
    }
    const char *metadata = url + 5;
    const char *end = url + url_length;
    const char *comma = memchr(metadata, ',', (size_t) (end - metadata));
    if (comma == NULL) return DATA_URL_INVALID;

    bool base64 = false;
    const char *first_semicolon = memchr(
        metadata, ';', (size_t) (comma - metadata));
    const char *type_end = first_semicolon == NULL ? comma : first_semicolon;
    size_t type_length = (size_t) (type_end - metadata);
    static const char default_type[] = "text/plain";
    if (type_length == 0) {
        if (sizeof(default_type) > media_type_size) return DATA_URL_INVALID;
        memcpy(media_type, default_type, sizeof(default_type));
    } else {
        if (type_length >= media_type_size) return DATA_URL_INVALID;
        memcpy(media_type, metadata, type_length);
        media_type[type_length] = '\0';
    }
    for (const char *parameter = type_end; parameter < comma;) {
        if (*parameter != ';') return DATA_URL_INVALID;
        const char *next = memchr(
            parameter + 1, ';', (size_t) (comma - parameter - 1));
        if (next == NULL) next = comma;
        const char *start = parameter + 1;
        if (token_equals(start, (size_t) (next - start), "base64")) {
            if (base64) return DATA_URL_INVALID;
            base64 = true;
        }
        parameter = next;
    }

    const unsigned char *payload = (const unsigned char *) comma + 1;
    size_t payload_length = (size_t) (end - (const char *) payload);
    size_t decoded_length = 0;
    if (!base64) {
        for (size_t i = 0; i < payload_length; i++, decoded_length++) {
            if (payload[i] == '%') {
                if (i + 2 >= payload_length
                    || hex_digit(payload[i + 1]) < 0
                    || hex_digit(payload[i + 2]) < 0) {
                    return DATA_URL_INVALID;
                }
                i += 2;
            }
            if (decoded_length >= maximum_decoded_bytes) {
                return DATA_URL_TOO_LARGE;
            }
        }
    } else {
        size_t digits = 0, padding = 0;
        for (size_t i = 0; i < payload_length; i++) {
            unsigned char value = payload[i];
            if (isspace(value)) continue;
            if (value == '=') padding++;
            else {
                if (padding != 0 || base64_digit(value) < 0) {
                    return DATA_URL_INVALID;
                }
                digits++;
            }
        }
        if (padding > 2 || (digits + padding) % 4 != 0
            || digits % 4 == 1) return DATA_URL_INVALID;
        decoded_length = digits / 4 * 3;
        if (digits % 4 == 2) decoded_length++;
        else if (digits % 4 == 3) decoded_length += 2;
        if (decoded_length > maximum_decoded_bytes) {
            return DATA_URL_TOO_LARGE;
        }
    }

    unsigned char *decoded = budget_malloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, decoded_length + 1);
    if (decoded == NULL) return DATA_URL_NO_MEMORY;
    size_t output = 0;
    if (!base64) {
        for (size_t i = 0; i < payload_length;) {
            if (payload[i] == '%') {
                decoded[output++] = (unsigned char) (
                    hex_digit(payload[i + 1]) * 16
                    + hex_digit(payload[i + 2]));
                i += 3;
            } else {
                decoded[output++] = payload[i++];
            }
        }
    } else {
        unsigned accumulator = 0;
        unsigned bits = 0;
        for (size_t i = 0; i < payload_length; i++) {
            int digit = base64_digit(payload[i]);
            if (digit < 0) continue;
            accumulator = (accumulator << 6) | (unsigned) digit;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                decoded[output++] = (unsigned char) (accumulator >> bits);
                accumulator &= bits == 0 ? 0u : (1u << bits) - 1u;
            }
        }
    }
    decoded[output] = '\0';
    *data = decoded;
    *length = output;
    return DATA_URL_DECODED;
}
