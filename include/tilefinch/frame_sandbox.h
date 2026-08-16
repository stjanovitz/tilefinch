#ifndef TILEFINCH_FRAME_SANDBOX_H
#define TILEFINCH_FRAME_SANDBOX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TILEFINCH_FRAME_SANDBOX_ATTRIBUTE_LIMIT 1024u

typedef enum {
    TILEFINCH_SANDBOX_PRESENT = 1u << 0,
    TILEFINCH_SANDBOX_ALLOW_SCRIPTS = 1u << 1,
    TILEFINCH_SANDBOX_ALLOW_SAME_ORIGIN = 1u << 2,
    TILEFINCH_SANDBOX_ALLOW_FORMS = 1u << 3,
    TILEFINCH_SANDBOX_ALLOW_POPUPS = 1u << 4,
    TILEFINCH_SANDBOX_ALLOW_TOP_NAVIGATION = 1u << 5,
    TILEFINCH_SANDBOX_ALLOW_TOP_NAVIGATION_BY_USER = 1u << 6
} TilefinchFrameSandboxFlag;

uint32_t tilefinch_frame_sandbox_parse(const char *value, size_t length,
                                       bool present);
bool tilefinch_frame_sandbox_allows(uint32_t flags,
                                    TilefinchFrameSandboxFlag permission);

#endif
