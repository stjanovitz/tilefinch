#include "tilefinch/budget.h"
#include "tilefinch/media_failure_message.h"
#include "../src/media_backend_psp_policy.h"
#include "../src/media_backend_psp_allocation.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define KIB 1024u
typedef struct {
    Budget *budget;
    bool use_swdec;
    struct {
        bool (*make_room)(void *, size_t, unsigned *);
        void *memory_context;
    } platform;
} PspMediaSession;
typedef struct {
    const char *failure;
    unsigned attempts, rooms, succeed_after;
    bool refuse_room, stall_room;
    size_t needed;
} Fixture;
static size_t working_bytes;
static size_t media_psp_swdec_backend_last_working_set(void) { return working_bytes; }
static bool psp_media_create_playback_once(PspMediaSession *media,
    uint64_t audio_start_us, char *error, size_t size)
{
    Fixture *fixture = media->platform.memory_context;
    fixture->attempts++;
    if (audio_start_us != 123) return false;
    if (fixture->attempts > fixture->succeed_after) return true;
    if (error != NULL && size != 0) snprintf(error, size, "%s", fixture->failure);
    return false;
}
static bool make_room(void *opaque, size_t needed, unsigned *stage)
{
    Fixture *fixture = opaque;
    fixture->rooms++;
    fixture->needed = needed;
    if (fixture->refuse_room) return false;
    if (!fixture->stall_room) (*stage)++;
    return true;
}
#include "../src/psp_media_open_memory.inc"
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", name, __LINE__, #c); return false; \
} } while (0)
static bool run_case(const char *name)
{
    Budget budget;
    budget_init(&budget, 32u * 1024u * 1024u);
    Fixture fixture = {.failure = "PSP aligned codec allocation failed", .succeed_after = 1};
    PspMediaSession media = {.budget = &budget, .platform = {make_room, &fixture}};
    working_bytes = 0;
    char error[256] = {0};
    if (!strcmp(name, "aligned-retry")) {
        CHECK(psp_media_create_playback(&media, 123, error, sizeof(error)));
        CHECK(fixture.attempts == 2 && fixture.rooms == 1 && fixture.needed == 0);
    } else if (!strcmp(name, "working-set-retry")) {
        fixture.failure = "PSP 360p codec working set exceeded page budget";
        fixture.succeed_after = 2;
        CHECK(psp_media_create_playback(&media, 123, error, sizeof(error)));
        CHECK(fixture.attempts == 3 && fixture.rooms == 2);
    } else if (!strcmp(name, "state-retry")) {
        fixture.failure = "PSP backend state exceeded page budget";
        CHECK(psp_media_create_playback(&media, 123, error, sizeof(error)));
        CHECK(fixture.attempts == 2 && fixture.rooms == 1);
    } else if (!strcmp(name, "persistent-refusal")) {
        fixture.succeed_after = 99;
        CHECK(!psp_media_create_playback(&media, 123, error, sizeof(error)));
        CHECK(fixture.attempts == 3 && fixture.rooms == 2
            && !strcmp(error, fixture.failure));
    } else if (!strcmp(name, "unrelated-refusal")) {
        fixture.failure = "PSP AVC SPS geometry unsupported";
        CHECK(!psp_media_create_playback(&media, 123, error, sizeof(error)));
        CHECK(fixture.attempts == 1 && fixture.rooms == 0);
    } else if (!strcmp(name, "no-room")) {
        fixture.failure = "media packet exceeds budget";
        fixture.refuse_room = true;
        CHECK(!psp_media_create_playback(&media, 123, error, sizeof(error)));
        CHECK(fixture.attempts == 1 && fixture.rooms == 1);
    } else if (!strcmp(name, "stalled-room")) {
        fixture.failure = "media packet exceeds budget";
        fixture.stall_room = true;
        CHECK(!psp_media_create_playback(&media, 123, error, sizeof(error)));
        CHECK(fixture.attempts == 1 && fixture.rooms == 1);
    } else if (!strcmp(name, "software-headroom")) {
        fixture.failure = "swdec working set exceeds budget";
        media.use_swdec = true;
        working_bytes = 33u * 1024u * 1024u;
        CHECK(psp_media_create_playback(&media, 123, error, sizeof(error)));
        CHECK(fixture.needed == working_bytes + PSP_MEDIA_MAXIMUM_PACKET_BYTES + 512u * KIB);
    } else if (!strcmp(name, "software-physical-pressure")) {
        fixture.failure = "swdec working set exceeds budget";
        media.use_swdec = true;
        working_bytes = 8u * 1024u * 1024u;
        CHECK(psp_media_create_playback(&media, 123, error, sizeof(error)));
        CHECK(fixture.needed == 0);
    } else return false;
    CHECK(budget.current == 0);
    return true;
}

static bool diagnostics(void)
{
    const char *name = "allocation-diagnostics";
    const unsigned char present = 1;
    const void *buffers[PSP_MEDIA_ALLOC_COUNT];
    CHECK(PSP_MEDIA_ALLOC_COUNT == 12);
    /* Exhaustively preserve the old required-buffer predicate for every
       presence combination and video/audio form, without firmware calls. */
    for (unsigned absent = 0; absent < (1u << PSP_MEDIA_ALLOC_COUNT); absent++) {
        for (unsigned i = 0; i < PSP_MEDIA_ALLOC_COUNT; i++)
            buffers[i] = (absent & (1u << i)) ? NULL : &present;
        for (unsigned video = 0; video < 2; video++) {
            for (unsigned audio = 0; audio < 2; audio++) {
                unsigned required = 0x10u | (video ? 0x7fu : 0u) | (audio ? 0xf80u : 0u);
                CHECK(psp_media_allocation_missing(video != 0, audio != 0, buffers)
                    == (absent & required));
            }
        }
    }
    PspMediaAllocationFailure failure = {
        .missing_mask = 2, .request_bytes = 2u * 1024u * 1024u,
        .request_alignment = 4u * 1024u * 1024u, .allocation_errno = 12,
        .heap_lower_bound = 30u * 1024u * 1024u,
        .heap_arena = 40u * 1024u * 1024u, .heap_free = 1024u * 1024u,
        .heap_top = 4096, .pool_flags = PSP_MEDIA_ALLOC_POOL_ABSENT,
        .budget_remaining = 16u * 1024u * 1024u,
        .width = 640, .height = 360, .devkit = 0x06060110u,
        .system_free = 2326528, .system_largest = 1048576
    };
    char output[512];
    CHECK(psp_media_allocation_format(&failure, output, sizeof(output)));
    CHECK(strstr(output, "missing=0x002 request=2097152 alignment=4194304 errno=12\n")
        && strstr(output, "lower-bound=31457280")
        && strstr(output, "flags=0x1\n")
        && strstr(output, "devkit=0x06060110 size=640x360"));
    for (const unsigned char *p = (unsigned char *) output; *p; p++)
        CHECK(isalnum(*p) || *p == ' ' || *p == '\n' || *p == '=' || *p == '-');
    /* Every possible 32-bit PSP quantity still fits the release report's
       fixed stack buffer. No partial or unterminated record on refusal. */
    failure.request_bytes = failure.request_alignment = UINT32_MAX;
    failure.heap_lower_bound = failure.heap_arena = failure.heap_free = UINT32_MAX;
    failure.heap_top = failure.pool_bytes = failure.pool_used = UINT32_MAX;
    failure.budget_remaining = UINT32_MAX;
    failure.width = failure.height = failure.devkit = failure.pool_flags = UINT32_MAX;
    CHECK(psp_media_allocation_format(&failure, output, sizeof(output)));
    char small[9] = "12345678";
    CHECK(!psp_media_allocation_format(&failure, small, 8)
        && small[0] == '\0' && small[8] == '\0');
    CHECK(!psp_media_allocation_format(&failure, NULL, 512));
    CHECK(!psp_media_allocation_format(&failure, small, 0));
    failure.missing_mask = 0;
    CHECK(!psp_media_allocation_format(&failure, output, sizeof(output)) && output[0] == '\0');
    puts("allocation-diagnostics: PASS");
    return true;
}
int main(void)
{
    const char *cases[] = {"aligned-retry", "working-set-retry", "state-retry",
        "persistent-refusal", "unrelated-refusal", "no-room", "stalled-room",
        "software-headroom", "software-physical-pressure"};
    bool ok = diagnostics();
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        bool passed = run_case(cases[i]);
        printf("%s: %s\n", cases[i], passed ? "PASS" : "FAIL");
        ok = passed && ok;
    }
    return ok ? 0 : 1;
}
