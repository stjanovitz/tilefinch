/* The looping span helpers of the style subsystem, defined once (see
   style_internal.h). */

#include "style_values_internal.h"

#include <ctype.h>
#include <string.h>

void style_trim(const char **text, size_t *length)
{
    while (*length != 0 && isspace((unsigned char) **text)) {
        (*text)++;
        (*length)--;
    }
    while (*length != 0 && isspace((unsigned char) (*text)[*length - 1])) {
        (*length)--;
    }
}

bool style_span_case_equal(const char *text, size_t length,
                           const char *wanted)
{
    size_t wanted_length = strlen(wanted);
    if (wanted_length != length) return false;
    for (size_t i = 0; i < length; i++) {
        if (tolower((unsigned char) text[i])
            != tolower((unsigned char) wanted[i])) return false;
    }
    return true;
}

bool style_span_contains_text(const char *text, size_t length,
                              const char *wanted)
{
    size_t wanted_length = strlen(wanted);
    if (text == NULL || wanted_length == 0 || wanted_length > length)
        return false;
    for (size_t at = 0; at + wanted_length <= length; at++) {
        if (text[at] == wanted[0]
            && memcmp(text + at, wanted, wanted_length) == 0) return true;
    }
    return false;
}
