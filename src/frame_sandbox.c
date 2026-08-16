#include "tilefinch/frame_sandbox.h"

#include <string.h>

static bool ascii_whitespace(unsigned char character)
{
    return character == '\t' || character == '\n' || character == '\f'
        || character == '\r' || character == ' ';
}

static unsigned char ascii_lower(unsigned char character)
{
    return character >= 'A' && character <= 'Z'
        ? (unsigned char) (character + ('a' - 'A')) : character;
}

static bool token_is(const char *token, size_t length, const char *wanted)
{
    if (strlen(wanted) != length) return false;
    for (size_t i = 0; i < length; i++) {
        if (ascii_lower((unsigned char) token[i])
            != (unsigned char) wanted[i]) return false;
    }
    return true;
}

uint32_t tilefinch_frame_sandbox_parse(const char *value, size_t length,
                                       bool present)
{
    if (!present) return 0;
    uint32_t flags = TILEFINCH_SANDBOX_PRESENT;
    /* An over-limit sandbox is treated as an empty sandbox. Never recover by
       granting the permissions which happened to fit in a prefix. */
    if (value == NULL || length > TILEFINCH_FRAME_SANDBOX_ATTRIBUTE_LIMIT) {
        return flags;
    }
    size_t at = 0;
    while (at < length) {
        while (at < length
               && ascii_whitespace((unsigned char) value[at])) at++;
        size_t start = at;
        while (at < length
               && !ascii_whitespace((unsigned char) value[at])) at++;
        size_t token_length = at - start;
        if (token_is(value + start, token_length, "allow-scripts")) {
            flags |= TILEFINCH_SANDBOX_ALLOW_SCRIPTS;
        } else if (token_is(value + start, token_length,
                            "allow-same-origin")) {
            flags |= TILEFINCH_SANDBOX_ALLOW_SAME_ORIGIN;
        } else if (token_is(value + start, token_length, "allow-forms")) {
            flags |= TILEFINCH_SANDBOX_ALLOW_FORMS;
        } else if (token_is(value + start, token_length, "allow-popups")) {
            flags |= TILEFINCH_SANDBOX_ALLOW_POPUPS;
        } else if (token_is(value + start, token_length,
                            "allow-top-navigation")) {
            flags |= TILEFINCH_SANDBOX_ALLOW_TOP_NAVIGATION;
        } else if (token_is(value + start, token_length,
                            "allow-top-navigation-by-user-activation")) {
            flags |= TILEFINCH_SANDBOX_ALLOW_TOP_NAVIGATION_BY_USER;
        }
    }
    return flags;
}

bool tilefinch_frame_sandbox_allows(uint32_t flags,
                                    TilefinchFrameSandboxFlag permission)
{
    return (flags & TILEFINCH_SANDBOX_PRESENT) == 0
        || (flags & (uint32_t) permission) != 0;
}
