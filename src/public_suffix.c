#include "tilefinch/public_suffix.h"

#include <ctype.h>
#include <stdint.h>
#include <string.h>

/* This is the fixed-set decoder used by libpsl, reduced to the ASCII mode
   required by Tilefinch's URL parser. The graph is immutable, allocation-free,
   and bounded by the compiled snapshot. */

#define PSL_FLAG_EXCEPTION (1u << 0)
#define PSL_FLAG_WILDCARD  (1u << 1)

#include "public_suffix_dafsa.inc"

static bool next_offset(const unsigned char **position,
                        const unsigned char *end,
                        const unsigned char **offset)
{
    if (*position >= end || (size_t) (end - *position) < 3) return false;
    const unsigned char *at = *position;
    size_t distance;
    size_t consumed;
    switch (*at & 0x60u) {
        case 0x60u:
            distance = ((size_t) (*at & 0x1fu) << 16)
                | ((size_t) at[1] << 8) | at[2];
            consumed = 3;
            break;
        case 0x40u:
            distance = ((size_t) (*at & 0x1fu) << 8) | at[1];
            consumed = 2;
            break;
        default:
            distance = *at & 0x3fu;
            consumed = 1;
            break;
    }
    if (distance > (size_t) (end - *offset)) return false;
    *offset += distance;
    *position = (*at & 0x80u) != 0 ? end : at + consumed;
    return true;
}

static int fixed_set_lookup(const char *key, size_t key_length)
{
    const unsigned char *position = tilefinch_psl_dafsa;
    const unsigned char *end = tilefinch_psl_dafsa
        + sizeof(tilefinch_psl_dafsa);
    const unsigned char *offset = position;
    const unsigned char *key_at = (const unsigned char *) key;
    const unsigned char *key_end = key_at + key_length;

    while (next_offset(&position, end, &offset)) {
        bool consumed = false;
        if (offset >= end) return -1;
        if (key_at != key_end && (*offset & 0x80u) == 0) {
            if (*offset != *key_at) continue;
            consumed = true;
            offset++;
            key_at++;
            while (offset < end && (*offset & 0x80u) == 0
                   && key_at != key_end) {
                if (*offset != *key_at) return -1;
                offset++;
                key_at++;
            }
            if (offset >= end) return -1;
        }
        if (key_at == key_end) {
            if ((*offset & 0xe0u) == 0x80u) return *offset & 0x0fu;
            if (consumed) return -1;
            continue;
        }
        if ((*offset ^ 0x80u) != *key_at) {
            if (consumed) return -1;
            continue;
        }
        offset++;
        key_at++;
        position = offset;
    }
    return -1;
}

static bool dns_name(const char *input, char output[254])
{
    if (input == NULL) return false;
    size_t length = strlen(input);
    if (length == 0 || length > 253 || input[0] == '.'
        || input[length - 1] == '.') return false;
    bool only_digits_and_dots = true;
    size_t label_length = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char byte = (unsigned char) input[i];
        if (byte == '.') {
            if (label_length == 0 || label_length > 63
                || output[i - 1] == '-') return false;
            output[i] = '.';
            label_length = 0;
            continue;
        }
        if (!isalnum(byte) && byte != '-') return false;
        if (label_length == 0 && byte == '-') return false;
        output[i] = (char) tolower(byte);
        label_length++;
        if (!isdigit(byte)) only_digits_and_dots = false;
    }
    if (label_length == 0 || label_length > 63
        || output[length - 1] == '-' || only_digits_and_dots) return false;
    output[length] = '\0';
    return true;
}

static bool classified_public_suffix(const char *domain)
{
    if (strchr(domain, '.') == NULL) return true;
    int result = fixed_set_lookup(domain, strlen(domain));
    if (result >= 0) return ((unsigned) result & PSL_FLAG_EXCEPTION) == 0;
    const char *parent = strchr(domain, '.');
    if (parent == NULL || parent[1] == '\0') return false;
    parent++;
    result = fixed_set_lookup(parent, strlen(parent));
    return result >= 0
        && ((unsigned) result & PSL_FLAG_WILDCARD) != 0;
}

bool tilefinch_public_suffix_classify(const char *domain,
                                   bool *is_public_suffix)
{
    if (is_public_suffix == NULL) return false;
    char canonical[254];
    if (!dns_name(domain, canonical)) return false;
    *is_public_suffix = classified_public_suffix(canonical);
    return true;
}

bool tilefinch_registrable_domain(const char *host, char *output,
                               size_t output_size)
{
    if (output == NULL || output_size == 0) return false;
    output[0] = '\0';
    char canonical[254];
    if (!dns_name(host, canonical)) return false;
    const char *candidate = canonical;
    const char *previous = NULL;
    while (true) {
        if (classified_public_suffix(candidate)) {
            if (previous == NULL) return false;
            size_t length = strlen(previous);
            if (length + 1 > output_size) return false;
            memcpy(output, previous, length + 1);
            return true;
        }
        const char *dot = strchr(candidate, '.');
        if (dot == NULL || dot[1] == '\0') return false;
        previous = candidate;
        candidate = dot + 1;
    }
}
