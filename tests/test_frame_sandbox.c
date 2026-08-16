#include "tilefinch/frame_sandbox.h"

#include <stdio.h>
#include <string.h>

static int expect(bool condition, const char *message)
{
    if (condition) return 0;
    fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

int main(void)
{
    int failures = 0;
    uint32_t absent = tilefinch_frame_sandbox_parse(NULL, 0, false);
    failures += expect(
        tilefinch_frame_sandbox_allows(
            absent, TILEFINCH_SANDBOX_ALLOW_SCRIPTS),
        "an absent sandbox must not restrict scripts");

    uint32_t empty = tilefinch_frame_sandbox_parse("", 0, true);
    failures += expect(
        !tilefinch_frame_sandbox_allows(
            empty, TILEFINCH_SANDBOX_ALLOW_SCRIPTS)
        && !tilefinch_frame_sandbox_allows(
            empty, TILEFINCH_SANDBOX_ALLOW_SAME_ORIGIN),
        "an empty sandbox must deny scripts and origin inheritance");

    static const char mixed[] =
        "ALLOW-SCRIPTS\tallow-same-origin allow-forms ignored-token";
    uint32_t flags = tilefinch_frame_sandbox_parse(
        mixed, sizeof(mixed) - 1, true);
    failures += expect(
        tilefinch_frame_sandbox_allows(
            flags, TILEFINCH_SANDBOX_ALLOW_SCRIPTS)
        && tilefinch_frame_sandbox_allows(
            flags, TILEFINCH_SANDBOX_ALLOW_SAME_ORIGIN)
        && tilefinch_frame_sandbox_allows(
            flags, TILEFINCH_SANDBOX_ALLOW_FORMS)
        && !tilefinch_frame_sandbox_allows(
            flags, TILEFINCH_SANDBOX_ALLOW_POPUPS),
        "known tokens must be case-insensitive and unknown tokens ignored");

    char oversized[TILEFINCH_FRAME_SANDBOX_ATTRIBUTE_LIMIT + 2];
    memset(oversized, ' ', sizeof(oversized));
    memcpy(oversized, "allow-scripts", strlen("allow-scripts"));
    flags = tilefinch_frame_sandbox_parse(
        oversized, sizeof(oversized), true);
    failures += expect(
        !tilefinch_frame_sandbox_allows(
            flags, TILEFINCH_SANDBOX_ALLOW_SCRIPTS),
        "over-limit metadata must fail closed instead of accepting a prefix");

    if (failures == 0) puts("frame sandbox tests passed");
    return failures == 0 ? 0 : 1;
}
