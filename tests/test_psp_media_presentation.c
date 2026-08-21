#include "tilefinch/psp_media_state.h"
#include "tilefinch/psp_ui.h"

#include "../src/fetch/background_slot_policy.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "PSP MEDIA PRESENTATION CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition);                             \
        return false;                                                        \
    }                                                                        \
} while (0)

enum {
    WIDTH = 480,
    HEIGHT = 272,
    CONTROL_HEIGHT = 78,
    QUIET_MARGIN = 16
};

typedef struct {
    PspMediaMachine machine;
    PspUiMediaState ui;
    uint32_t frame[WIDTH * HEIGHT];
    uint16_t scratch[WIDTH * HEIGHT];
    uint32_t first_bottom[WIDTH * CONTROL_HEIGHT];
    uint32_t previous_bottom[WIDTH * CONTROL_HEIGHT];
    bool previous_bottom_valid;
} PresentationHarness;

typedef enum {
    JOURNEY_SLOT_FREE = 0,
    JOURNEY_SLOT_QUEUED,
    JOURNEY_SLOT_RUNNING,
    JOURNEY_SLOT_COMPLETE
} JourneySlotState;

typedef struct {
    JourneySlotState state;
    uint32_t generation;
    uint32_t route_generation;
    bool cancelled;
} JourneySlot;

typedef struct {
    JourneySlot slots[FETCH_BACKGROUND_REQUEST_LIMIT];
    uint32_t route_generation;
} JourneyTransport;

static uint64_t journey_claim(JourneyTransport *transport, bool foreground)
{
    unsigned limit = fetch_background_admission_slot_limit(foreground, true);
    for (unsigned at = 0; at < limit; at++) {
        JourneySlot *slot = &transport->slots[at];
        if (slot->state != JOURNEY_SLOT_FREE) continue;
        slot->generation = fetch_background_generation_next(slot->generation);
        slot->route_generation = transport->route_generation;
        slot->cancelled = false;
        slot->state = JOURNEY_SLOT_QUEUED;
        return fetch_background_request_id_make(at, slot->generation);
    }
    return 0;
}

static void journey_start(JourneyTransport *transport, uint64_t request)
{
    unsigned at = fetch_background_request_id_slot(request);
    if (at >= FETCH_BACKGROUND_REQUEST_LIMIT) return;
    JourneySlot *slot = &transport->slots[at];
    if (slot->generation == fetch_background_request_id_generation(request)
        && slot->state == JOURNEY_SLOT_QUEUED)
        slot->state = JOURNEY_SLOT_RUNNING;
}

static bool journey_complete(
    JourneyTransport *transport, uint64_t request)
{
    unsigned at = fetch_background_request_id_slot(request);
    if (at >= FETCH_BACKGROUND_REQUEST_LIMIT) return false;
    JourneySlot *slot = &transport->slots[at];
    if (slot->generation != fetch_background_request_id_generation(request)
        || slot->state != JOURNEY_SLOT_RUNNING) return false;
    bool publish = !slot->cancelled
        && slot->route_generation == transport->route_generation;
    slot->state = publish ? JOURNEY_SLOT_COMPLETE : JOURNEY_SLOT_FREE;
    return publish;
}

static void journey_consume(JourneyTransport *transport, uint64_t request)
{
    unsigned at = fetch_background_request_id_slot(request);
    if (at >= FETCH_BACKGROUND_REQUEST_LIMIT) return;
    JourneySlot *slot = &transport->slots[at];
    if (slot->generation == fetch_background_request_id_generation(request)
        && slot->state == JOURNEY_SLOT_COMPLETE)
        slot->state = JOURNEY_SLOT_FREE;
}

static void journey_change_route(JourneyTransport *transport)
{
    transport->route_generation = fetch_background_generation_next(
        transport->route_generation);
    for (unsigned at = 0; at < FETCH_BACKGROUND_REQUEST_LIMIT; at++) {
        JourneySlot *slot = &transport->slots[at];
        if (slot->state == JOURNEY_SLOT_QUEUED
            || slot->state == JOURNEY_SLOT_COMPLETE) {
            slot->state = JOURNEY_SLOT_FREE;
        } else if (slot->state == JOURNEY_SLOT_RUNNING) {
            slot->cancelled = true;
        }
    }
}

static uint32_t motion_pixel(int x, int y, unsigned phase)
{
    unsigned red = (unsigned) (x * 13 + y * 3 + phase * 47);
    unsigned green = (unsigned) (x * 5 + y * 17 + phase * 29);
    unsigned blue = (unsigned) (x * 19 + y * 7 + phase * 61);
    return UINT32_C(0xff000000)
        | (red & 0xffu) << 16
        | (green & 0xffu) << 8
        | (blue & 0xffu);
}

static void fill_motion(uint32_t *pixels, unsigned phase)
{
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++)
            pixels[(size_t) y * WIDTH + x] = motion_pixel(x, y, phase);
    }
}

static bool row_is_declared(
    int y, const PspUiRowBand *bands, size_t count)
{
    for (size_t at = 0; at < count; at++) {
        if (y >= bands[at].top && y < bands[at].bottom) return true;
    }
    return false;
}

static bool render(PresentationHarness *harness, unsigned phase)
{
    fill_motion(harness->frame, phase);
    static uint32_t source[WIDTH * HEIGHT];
    memcpy(source, harness->frame, sizeof(source));
    PspUiRowBand bands[PSP_UI_MEDIA_OVERLAY_BAND_LIMIT];
    size_t count = psp_ui_media_overlay_bands(
        &harness->ui, WIDTH, HEIGHT, bands,
        PSP_UI_MEDIA_OVERLAY_BAND_LIMIT);
    psp_ui_media_composite_8888(
        &harness->ui, NULL, harness->frame,
        WIDTH, HEIGHT, WIDTH, harness->scratch);
    for (int y = 0; y < HEIGHT; y++) {
        if (row_is_declared(y, bands, count)) continue;
        CHECK(memcmp(
                  harness->frame + (size_t) y * WIDTH,
                  source + (size_t) y * WIDTH,
                  WIDTH * sizeof(*source)) == 0);
    }
    return true;
}

static bool quiet_ground_matches(
    const uint32_t *first, const uint32_t *second)
{
    for (int y = 0; y < CONTROL_HEIGHT; y++) {
        CHECK(memcmp(first + (size_t) y * WIDTH,
                     second + (size_t) y * WIDTH,
                     QUIET_MARGIN * sizeof(*first)) == 0);
        CHECK(memcmp(first + (size_t) y * WIDTH + WIDTH - QUIET_MARGIN,
                     second + (size_t) y * WIDTH + WIDTH - QUIET_MARGIN,
                     QUIET_MARGIN * sizeof(*first)) == 0);
    }
    return true;
}

static bool presentation_is_motion_stable(
    PresentationHarness *harness, unsigned phase)
{
    CHECK(render(harness, phase));
    const uint32_t *bottom = harness->frame
        + (size_t) (HEIGHT - CONTROL_HEIGHT) * WIDTH;
    memcpy(harness->first_bottom, bottom, sizeof(harness->first_bottom));
    CHECK(render(harness, phase + 1u));
    bottom = harness->frame
        + (size_t) (HEIGHT - CONTROL_HEIGHT) * WIDTH;
    if (harness->ui.visible && harness->ui.controls_visible) {
        CHECK(memcmp(harness->first_bottom, bottom,
                     sizeof(harness->first_bottom)) == 0);
        if (harness->previous_bottom_valid)
            CHECK(quiet_ground_matches(harness->previous_bottom, bottom));
        memcpy(harness->previous_bottom, bottom,
               sizeof(harness->previous_bottom));
        harness->previous_bottom_valid = true;
    } else {
        /* Hidden states are a true pass-through, including the footer. */
        for (int y = 0; y < CONTROL_HEIGHT; y++) {
            for (int x = 0; x < WIDTH; x++) {
                CHECK(bottom[(size_t) y * WIDTH + x]
                      == motion_pixel(
                          x, HEIGHT - CONTROL_HEIGHT + y, phase + 1u));
            }
        }
        harness->previous_bottom_valid = false;
    }
    return true;
}

static bool dispatch(PresentationHarness *harness, PspMediaEvent event)
{
    PspMediaDecision decision = psp_media_machine_transition(
        &harness->machine, &event);
    CHECK(decision.handled);
    CHECK(psp_media_machine_violations(&decision.next) == 0);
    harness->machine = decision.next;
    PspMediaUiProjection projection =
        psp_media_machine_project_ui(&harness->machine);
    psp_ui_media_apply_projection(&harness->ui, &projection);
    harness->ui.current_time_us = UINT64_C(5000000);
    harness->ui.duration_us = UINT64_C(120000000);
    return true;
}

static bool open_to_priming(PresentationHarness *harness)
{
    CHECK(dispatch(harness, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_OPEN,
        .autoplay = true,
        .has_separate_audio = true
    }));
    for (unsigned at = 0; at < 8u; at++) {
        CHECK(dispatch(harness, (PspMediaEvent) {
            .type = PSP_MEDIA_EVENT_OPEN_PHASE_COMPLETE,
            .has_separate_audio = true
        }));
    }
    CHECK(harness->machine.state == PSP_MEDIA_SESSION_PRIMING);
    return true;
}

static bool finish_quiescing(PresentationHarness *harness)
{
    CHECK(dispatch(harness, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_ADMISSION_STOPPED
    }));
    CHECK(dispatch(harness, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_TRANSPORT_CANCELLED
    }));
    CHECK(dispatch(harness, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_BACKEND_QUIESCED
    }));
    return true;
}

static bool test_youtube_navigation_media_journey(void)
{
    static PresentationHarness harness;
    JourneyTransport transport = {0};
    transport.route_generation = 1u;
    memset(&harness, 0, sizeof(harness));
    harness.machine = psp_media_machine_initial();
    psp_ui_media_init(&harness.ui);

    /* Home -> search results. Optional thumbnail work fills the normal lane,
       including one request that has already entered the worker. */
    journey_change_route(&transport);
    uint64_t document = journey_claim(&transport, true);
    CHECK(document != 0);
    journey_start(&transport, document);
    CHECK(journey_complete(&transport, document));
    journey_consume(&transport, document);
    uint64_t old_thumbnail = journey_claim(&transport, false);
    CHECK(old_thumbnail != 0);
    journey_start(&transport, old_thumbnail);
    while (journey_claim(&transport, false) != 0) {}

    /* Results -> video A. Cancellation is immediate for queued thumbnails;
       the running thumbnail retires late. The reserved foreground lane still
       admits the media route, and the stale thumbnail cannot publish. */
    journey_change_route(&transport);
    uint64_t video_range = journey_claim(&transport, true);
    CHECK(video_range != 0);
    journey_start(&transport, video_range);
    CHECK(!journey_complete(&transport, old_thumbnail));
    CHECK(journey_complete(&transport, video_range));
    journey_consume(&transport, video_range);

    psp_ui_media_set_resolving(&harness.ui, "Journey video A");
    CHECK(open_to_priming(&harness));
    CHECK(dispatch(&harness, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_PRIME_READY
    }));
    CHECK(harness.machine.state == PSP_MEDIA_SESSION_PLAYING);
    CHECK(presentation_is_motion_stable(&harness, 31u));

    /* A short read becomes a debounced buffering episode. Source recovery
       alone does not hide the overlay; a stable refill does. */
    CHECK(dispatch(&harness, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_SOURCE_STARVED
    }));
    CHECK(harness.machine.state == PSP_MEDIA_SESSION_BUFFERING);
    CHECK(dispatch(&harness, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_SOURCE_AVAILABLE
    }));
    CHECK(harness.machine.state == PSP_MEDIA_SESSION_BUFFERING);
    CHECK(presentation_is_motion_stable(&harness, 33u));
    CHECK(dispatch(&harness, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_BUFFER_STABLE,
        .readiness = PSP_MEDIA_PRESENTATION_READY
    }));
    CHECK(harness.machine.state == PSP_MEDIA_SESSION_PLAYING);

    CHECK(dispatch(&harness, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_SEEK
    }));
    CHECK(dispatch(&harness, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_PREVIEW_STARTED
    }));
    CHECK(presentation_is_motion_stable(&harness, 35u));
    CHECK(dispatch(&harness, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_PREVIEW_ENDED
    }));
    CHECK(dispatch(&harness, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_SEEK_COMPLETE
    }));
    CHECK(dispatch(&harness, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_PRIME_READY
    }));
    CHECK(dispatch(&harness, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_PAUSE
    }));
    CHECK(dispatch(&harness, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_PLAY
    }));

    /* Back to results tears down video A before video B is admitted. A late
       range completion from A cannot satisfy B because both request and route
       generations must match. */
    uint64_t late_range = journey_claim(&transport, true);
    CHECK(late_range != 0);
    journey_start(&transport, late_range);
    journey_change_route(&transport);
    CHECK(dispatch(&harness, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_CLOSE
    }));
    CHECK(finish_quiescing(&harness));
    CHECK(harness.machine.state == PSP_MEDIA_SESSION_IDLE);
    CHECK(!journey_complete(&transport, late_range));

    journey_change_route(&transport);
    uint64_t second_range = journey_claim(&transport, true);
    CHECK(second_range != 0);
    journey_start(&transport, second_range);
    CHECK(journey_complete(&transport, second_range));
    journey_consume(&transport, second_range);
    psp_ui_media_set_resolving(&harness.ui, "Journey video B");
    CHECK(open_to_priming(&harness));

    /* A terminal failure is bounded: it reaches Failed only after the same
       ordered teardown used by Back, never an endless opening/buffering UI. */
    CHECK(dispatch(&harness, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_PLAYBACK_FAILED
    }));
    CHECK(finish_quiescing(&harness));
    CHECK(harness.machine.state == PSP_MEDIA_SESSION_FAILED);
    CHECK(psp_media_machine_project_ui(&harness.machine).retry_available);
    return true;
}

static bool test_authoritative_lifecycle_presentation_trace(void)
{
    static PresentationHarness harness;
    memset(&harness, 0, sizeof(harness));
    harness.machine = psp_media_machine_initial();
    psp_ui_media_init(&harness.ui);
    psp_ui_media_set_resolving(&harness.ui, "Lifecycle trace");
    psp_ui_media_set_resolving_progress(
        &harness.ui, "Loading...", 920u);

    CHECK(open_to_priming(&harness));
    CHECK(harness.ui.resolving && harness.ui.playing);
    psp_ui_media_tick(&harness.ui, 10000u);
    CHECK(harness.ui.controls_visible);
    CHECK(presentation_is_motion_stable(&harness, 1u));

    CHECK(dispatch(&harness, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_PRIME_READY
    }));
    CHECK(harness.machine.state == PSP_MEDIA_SESSION_PLAYING);
    CHECK(presentation_is_motion_stable(&harness, 3u));

    CHECK(dispatch(&harness, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_PAUSE
    }));
    CHECK(harness.machine.state == PSP_MEDIA_SESSION_PAUSED);
    CHECK(presentation_is_motion_stable(&harness, 5u));
    CHECK(dispatch(&harness, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_PLAY
    }));
    CHECK(harness.machine.state == PSP_MEDIA_SESSION_PLAYING);
    CHECK(presentation_is_motion_stable(&harness, 7u));

    CHECK(dispatch(&harness, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_SOURCE_STARVED
    }));
    CHECK(harness.machine.state == PSP_MEDIA_SESSION_BUFFERING);
    CHECK(harness.ui.buffering);
    CHECK(presentation_is_motion_stable(&harness, 9u));
    CHECK(dispatch(&harness, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_BUFFER_STABLE,
        .readiness = PSP_MEDIA_PRESENTATION_READY
    }));
    CHECK(harness.machine.state == PSP_MEDIA_SESSION_PLAYING);
    CHECK(presentation_is_motion_stable(&harness, 11u));

    CHECK(dispatch(&harness, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_SEEK
    }));
    CHECK(harness.machine.state == PSP_MEDIA_SESSION_SEEKING);
    CHECK(presentation_is_motion_stable(&harness, 13u));
    CHECK(dispatch(&harness, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_PREVIEW_STARTED
    }));
    CHECK(harness.machine.preview_active && harness.ui.seek_preview_active);
    CHECK(presentation_is_motion_stable(&harness, 15u));
    CHECK(dispatch(&harness, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_PREVIEW_ENDED
    }));
    CHECK(!harness.ui.seek_preview_active);
    CHECK(dispatch(&harness, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_SEEK_COMPLETE
    }));
    CHECK(harness.machine.state == PSP_MEDIA_SESSION_PRIMING);
    CHECK(presentation_is_motion_stable(&harness, 17u));
    CHECK(dispatch(&harness, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_PRIME_READY
    }));

    CHECK(dispatch(&harness, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_DECODER_REFUSED
    }));
    CHECK(harness.machine.state == PSP_MEDIA_SESSION_RECOVERING);
    CHECK(presentation_is_motion_stable(&harness, 19u));
    CHECK(dispatch(&harness, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_RECOVERY_COMPLETE
    }));
    CHECK(harness.machine.state == PSP_MEDIA_SESSION_PRIMING);
    CHECK(presentation_is_motion_stable(&harness, 21u));
    CHECK(dispatch(&harness, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_PRIME_READY
    }));
    CHECK(harness.machine.state == PSP_MEDIA_SESSION_PLAYING);

    CHECK(dispatch(&harness, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_CLOSE,
        .retain_pipeline = true
    }));
    CHECK(harness.machine.state == PSP_MEDIA_SESSION_DORMANT);
    CHECK(!harness.ui.visible);
    CHECK(presentation_is_motion_stable(&harness, 23u));
    return true;
}

int main(void)
{
    if (!test_authoritative_lifecycle_presentation_trace()) return 1;
    if (!test_youtube_navigation_media_journey()) return 1;
    puts("tilefinch-psp-media-presentation-tests: all checks passed");
    return 0;
}
