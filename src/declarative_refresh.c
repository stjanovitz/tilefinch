#include "tilefinch/declarative_refresh.h"

/* ASCII whitespace as the Infra standard defines it: TAB, LF, FF, CR,
   SPACE. Not isspace(), which also accepts VT. */
static bool refresh_ascii_whitespace(char character)
{
    return character == '\t' || character == '\n' || character == '\f'
        || character == '\r' || character == ' ';
}

static bool refresh_ascii_digit(char character)
{
    return character >= '0' && character <= '9';
}

static size_t refresh_skip_whitespace(const char *input, size_t length,
                                      size_t position)
{
    while (position < length && refresh_ascii_whitespace(input[position]))
        position++;
    return position;
}

static bool refresh_code_point_is(const char *input, size_t length,
                                  size_t position, char lower)
{
    if (position >= length) return false;
    char character = input[position];
    if (character >= 'A' && character <= 'Z')
        character = (char) (character - 'A' + 'a');
    return character == lower;
}

bool declarative_refresh_parse(const char *input, size_t length,
                               DeclarativeRefresh *refresh)
{
    if (refresh == NULL) return false;
    *refresh = (DeclarativeRefresh) {0};
    if (input == NULL) return false;
    /* Steps 2-3: position at the start, skip ASCII whitespace. */
    size_t position = refresh_skip_whitespace(input, length, 0);
    /* Steps 4-7: time is the leading run of ASCII digits, parsed as a
       non-negative integer; without one the next code point must be '.'. */
    uint32_t seconds = 0;
    size_t digits_start = position;
    while (position < length && refresh_ascii_digit(input[position])) {
        uint32_t digit = (uint32_t) (input[position] - '0');
        seconds = seconds > (UINT32_MAX - digit) / 10u
            ? UINT32_MAX : seconds * 10u + digit;
        position++;
    }
    if (position == digits_start
        && (position >= length || input[position] != '.')) return false;
    /* Step 8: ignore any further digits and full stops. */
    while (position < length
           && (refresh_ascii_digit(input[position])
               || input[position] == '.')) position++;
    /* Step 10: the time may be followed only by ';', ',' or whitespace,
       then optional whitespace, one ';' or ',', and whitespace. */
    if (position < length) {
        char separator = input[position];
        if (separator != ';' && separator != ','
            && !refresh_ascii_whitespace(separator)) return false;
        position = refresh_skip_whitespace(input, length, position);
        if (position < length
            && (input[position] == ';' || input[position] == ','))
            position++;
        position = refresh_skip_whitespace(input, length, position);
    }
    refresh->seconds = seconds;
    if (position >= length) return true;
    /* Step 11: the rest is the URL, optionally introduced by "url" [ws]
       "=" [ws] and optionally quoted. A partial "url" prefix leaves the
       whole remainder as the URL ("jump to the step labeled parse"). */
    size_t url_start = position;
    size_t url_end = length;
    bool skip_quotes = false;
    if (!refresh_code_point_is(input, length, position, 'u')) {
        skip_quotes = true;
    } else if (refresh_code_point_is(input, length, position + 1, 'r')
               && refresh_code_point_is(input, length, position + 2, 'l')) {
        size_t after = refresh_skip_whitespace(input, length, position + 3);
        if (after < length && input[after] == '=') {
            position = refresh_skip_whitespace(input, length, after + 1);
            skip_quotes = true;
        }
    }
    if (skip_quotes) {
        char quote = '\0';
        if (position < length
            && (input[position] == '\'' || input[position] == '"')) {
            quote = input[position];
            position++;
        }
        url_start = position;
        if (quote != '\0') {
            for (size_t at = position; at < length; at++) {
                if (input[at] == quote) {
                    url_end = at;
                    break;
                }
            }
        }
    }
    refresh->has_url = true;
    refresh->url_offset = url_start;
    refresh->url_length = url_end - url_start;
    return true;
}
