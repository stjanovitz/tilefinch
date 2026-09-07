/*
 * Host reproduction of the fullscreen-player chrome flicker.
 *
 * Drives the real portable presentation stack -- psp_display rotation over
 * the three 16-bit page buffers, the psp_media_present skip/record contract,
 * and the real media chrome compositor -- through scripted playback
 * timelines, capturing every buffer exactly as scanout would latch it. A
 * published frame whose chrome disagrees with the state that was current at
 * its publish, or that differs from its neighbours while that state is
 * constant, is the flicker, made deterministic.
 *
 * The decoded picture is synthesized (each identity fills the planned video
 * rectangle with a distinct colour) because every reported symptom lives in
 * the chrome layer; the software scaler's own pixels are qualified elsewhere.
 *
 * Scenario S4 replays the cooperative supervisor's media present as
 * psp_present_supervisor_media performs it on the 16-bit surface: records
 * forgotten, the last complete scanout preserved, and the controls updated
 * from an immutable snapshot taken before the native unit began. The
 * assertions require what a viewer requires -- the picture and current
 * caption stay on the panel while the control acknowledgement is published.
 */

#include "tilefinch/psp_display.h"
#include "tilefinch/psp_media_present.h"
#include "tilefinch/psp_ui.h"
#include "../src/psp_ui_theme.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, \
                    #condition); \
            failures++; \
        } \
    } while (0)

enum { SCREEN_WIDTH = 480, SCREEN_HEIGHT = 272 };
enum { SOURCE_WIDTH = 640, SOURCE_HEIGHT = 360 };
enum { CONTROL_BAR_HEIGHT = 78, HINT_BAR_HEIGHT = 38 };
enum { CAPTURE_LIMIT = 512 };

/* ---------------------------------------------------------------- display */

static uint16_t edram[PSP_DISPLAY_BUFFER_PIXELS
                      * PSP_DISPLAY_PAGE_BUFFER_COUNT];

typedef struct {
    /* What scanout would show: a full copy of the buffer at latch time. */
    uint16_t pixels[PSP_DISPLAY_BUFFER_PIXELS];
    unsigned buffer_index;
    /* Live state stamped by the scenario at the moment of publish. */
    bool expect_bar;
    bool expect_caption;
    uint16_t expect_video;
    char kind;
} CapturedFrame;

static CapturedFrame captured[CAPTURE_LIMIT];
static size_t capture_count;
static unsigned pending_buffer_index;

static void *stub_compose_base(void) { return edram; }
static void stub_flush_range(void *address, size_t bytes)
{
    (void) address;
    (void) bytes;
}
static int stub_set_mode(int mode, int width, int height)
{
    (void) mode;
    (void) width;
    (void) height;
    return 0;
}
static void stub_wait_vblank(void) {}
static int stub_set_frame_buffer(void *address, int stride, int format,
                                 int sync)
{
    (void) stride;
    (void) format;
    (void) sync;
    if (capture_count < CAPTURE_LIMIT) {
        memcpy(captured[capture_count].pixels, address,
               PSP_DISPLAY_BUFFER_PIXELS * sizeof(uint16_t));
        captured[capture_count].buffer_index = pending_buffer_index;
    }
    capture_count++;
    return 0;
}

static const PspDisplayBackend stub_backend = {
    .compose_base = stub_compose_base,
    .flush_range = stub_flush_range,
    .set_mode = stub_set_mode,
    .set_frame_buffer = stub_set_frame_buffer,
    .wait_vblank = stub_wait_vblank
};

/* ------------------------------------------------- production glue mirror */

static PspDisplay display;
static PspMediaPresentRecord records[PSP_DISPLAY_PAGE_BUFFER_COUNT];
static uint16_t visible_video_color;

/* Mirrors psp_present_media_frame for a synthetic picture: plan, the skip
   contract against the per-buffer record, the full-rectangle blit, and the
   band fill, in production order. */
static void present_video_565(uint16_t *vram, uint64_t identity,
                              uint16_t video_color, bool chrome_paints)
{
    PspMediaPresentPlan plan;
    if (!psp_media_present_plan(&plan, SOURCE_WIDTH, SOURCE_HEIGHT,
                                SOURCE_WIDTH, SCREEN_WIDTH, SCREEN_HEIGHT)) {
        fprintf(stderr, "present plan refused\n");
        exit(2);
    }
    unsigned buffer_index =
        display.back_buffer % PSP_DISPLAY_PAGE_BUFFER_COUNT;
    PspMediaPresentRecord *record = &records[buffer_index];
    if (psp_media_present_skip_allowed(record, identity, 1u, &plan.video,
                                       buffer_index, chrome_paints)) {
        return;
    }
    for (int y = plan.video.y; y < plan.video.y + plan.video.height; y++) {
        uint16_t *row = vram + (size_t) y * PSP_DISPLAY_STRIDE;
        for (int x = plan.video.x; x < plan.video.x + plan.video.width; x++)
            row[x] = video_color;
    }
    for (size_t at = 0; at < plan.band_count; at++) {
        const PspMediaPresentRect *band = &plan.bands[at];
        for (int y = band->y; y < band->y + band->height; y++) {
            memset(vram + (size_t) y * PSP_DISPLAY_STRIDE + band->x, 0,
                   (size_t) band->width * sizeof(uint16_t));
        }
    }
    if (chrome_paints) record->valid = false;
    else psp_media_present_record(record, identity, 1u, &plan.video,
                                  buffer_index);
}

static void publish(char kind, const PspUiMediaState *live,
                    uint16_t live_video_color)
{
    pending_buffer_index =
        display.back_buffer % PSP_DISPLAY_PAGE_BUFFER_COUNT;
    if (capture_count < CAPTURE_LIMIT) {
        CapturedFrame *frame = &captured[capture_count];
        frame->kind = kind;
        frame->expect_bar = live->controls_visible;
        frame->expect_caption = live->presentation != NULL
            && live->presentation->subtitle_text[0] != '\0';
        frame->expect_video = live_video_color;
    }
    if (!psp_display_publish(&display)) {
        fprintf(stderr, "publish refused\n");
        exit(2);
    }
}

/* Mirrors psp_present_internal's media leg on the 16-bit surface. */
static void ordinary_present(const PspUiMediaState *ui, uint64_t identity,
                             uint16_t video_color)
{
    uint16_t *vram = psp_display_back_buffer(&display);
    present_video_565(vram, identity, video_color,
                      psp_ui_media_overlay_paints(ui));
    psp_ui_media_composite_with_preview(ui, NULL, vram, SCREEN_WIDTH,
                                        SCREEN_HEIGHT, PSP_DISPLAY_STRIDE);
    publish('o', ui, video_color);
    visible_video_color = video_color;
}

/* Mirrors psp_present_supervisor_media's 16-bit branch: forget the records,
   preserve the last complete picture, update only the controls, publish. */
static void supervisor_present(const PspUiMediaState *snapshot,
                               const PspUiMediaState *live)
{
    psp_media_present_records_reset(records, PSP_DISPLAY_PAGE_BUFFER_COUNT);
    uint16_t *vram = psp_display_back_buffer(&display);
    const uint16_t *front = psp_display_front_buffer(&display);
    memcpy(vram, front,
           PSP_DISPLAY_BUFFER_PIXELS * sizeof(uint16_t));
    psp_ui_media_composite_supervisor_565(
        snapshot, vram, SCREEN_WIDTH, SCREEN_HEIGHT, PSP_DISPLAY_STRIDE);
    /* No decoder frame was acquired during this cooperative acknowledgement;
       the expected picture is the last one actually latched for scanout. */
    publish('s', live, visible_video_color);
}

/* ------------------------------------------------------------- classifiers */

static size_t count_color_span(const CapturedFrame *frame, int top,
                               int bottom, int left, int right,
                               uint16_t color)
{
    size_t count = 0;
    for (int y = top; y < bottom; y++) {
        const uint16_t *row = frame->pixels + (size_t) y * PSP_DISPLAY_STRIDE;
        for (int x = left; x < right; x++)
            if (row[x] == color) count++;
    }
    return count;
}

static bool frame_has_bar(const CapturedFrame *frame)
{
    /* The caption box never paints the hint-bar colour, so the legend ground
       alone identifies the control bar. */
    return count_color_span(frame, SCREEN_HEIGHT - HINT_BAR_HEIGHT,
                            SCREEN_HEIGHT, 0, SCREEN_WIDTH,
                            PSP_THEME_HINT_BAR)
        > (size_t) SCREEN_WIDTH * HINT_BAR_HEIGHT / 3u;
}

/* The box-background caption fills media_subtitle_rect with the chrome-bar
   colour: rows [204,254) with the controls hidden, [145,195) with them
   visible. Sample the left third of the box, clear of the centred badge. */
static bool frame_has_caption(const CapturedFrame *frame, bool controls)
{
    int bottom = controls ? SCREEN_HEIGHT - CONTROL_BAR_HEIGHT - 8
                          : SCREEN_HEIGHT - 18;
    return count_color_span(frame, bottom - 50, bottom, 60, 180,
                            PSP_THEME_CHROME_BAR) > 120u * 50u / 2u;
}

static bool frame_has_video(const CapturedFrame *frame, uint16_t color)
{
    /* Rows below the title bar and above every caption/badge position. */
    return count_color_span(frame, 40, 80, 0, SCREEN_WIDTH, color)
        > (size_t) SCREEN_WIDTH * 40u / 2u;
}

static uint32_t frame_region_hash(const CapturedFrame *frame, int top,
                                  int bottom)
{
    uint32_t hash = 2166136261u;
    for (int y = top; y < bottom; y++) {
        const uint16_t *row = frame->pixels + (size_t) y * PSP_DISPLAY_STRIDE;
        for (int x = 0; x < SCREEN_WIDTH; x++) {
            hash ^= row[x];
            hash *= 16777619u;
        }
    }
    return hash;
}

static void dump_timeline(const char *scenario, size_t begin)
{
    fprintf(stderr, "-- %s timeline --\n", scenario);
    for (size_t at = begin; at < capture_count && at < CAPTURE_LIMIT; at++) {
        const CapturedFrame *frame = &captured[at];
        fprintf(stderr,
                "  present=%zu kind=%c buffer=%u bar=%d/%d caption=%d/%d "
                "video=%d below-timeline=%08x\n",
                at, frame->kind, frame->buffer_index,
                frame_has_bar(frame), frame->expect_bar,
                frame_has_caption(frame, frame->expect_bar),
                frame->expect_caption,
                frame_has_video(frame, frame->expect_video),
                (unsigned) frame_region_hash(
                    frame, SCREEN_HEIGHT - CONTROL_BAR_HEIGHT,
                    SCREEN_HEIGHT));
    }
}

/* One failure counter for the whole run. */
static int failures;

/* Assert chrome and picture agree with the live state for every capture in
   [begin, capture_count). The clock is allowed to advance within otherwise
   identical control states, so byte identity is diagnostic rather than an
   oracle here. */
static void check_window(const char *scenario, size_t begin)
{
    bool window_failed = false;
    for (size_t at = begin; at < capture_count && at < CAPTURE_LIMIT; at++) {
        const CapturedFrame *frame = &captured[at];
        if (frame_has_bar(frame) != frame->expect_bar) window_failed = true;
        if (frame_has_caption(frame, frame->expect_bar)
                != frame->expect_caption)
            window_failed = true;
        if (!frame_has_video(frame, frame->expect_video))
            window_failed = true;
    }
    if (window_failed) {
        dump_timeline(scenario, begin);
        fprintf(stderr, "FAILED scenario %s\n", scenario);
        failures++;
    } else {
        fprintf(stderr, "ok scenario %s (%zu presents)\n", scenario,
                capture_count - begin);
    }
}

/* ---------------------------------------------------------------- states */

static void make_playing_state(PspUiMediaState *media,
                               PspUiMediaPresentation *presentation,
                               uint64_t time_us, bool controls,
                               const char *caption)
{
    psp_ui_media_init(media);
    psp_ui_media_bind_presentation(media, presentation);
    psp_ui_media_set(media, true, true, false, time_us,
                     UINT64_C(120000000), "Stability scenario");
    psp_ui_media_set_subtitle_style(media, BROWSER_SUBTITLE_SIZE_STANDARD,
                                    BROWSER_SUBTITLE_BACKGROUND_BOX);
    psp_ui_media_set_subtitle(media, caption == NULL ? "" : caption);
    if (controls) psp_ui_media_show_controls(media);
    else media->controls_visible = false;
}

int main(void)
{
    if (!psp_display_begin(&display, &stub_backend)) {
        fprintf(stderr, "display begin failed\n");
        return 2;
    }

    PspUiMediaState ui;
    PspUiMediaPresentation presentation;

    /* S1: controls visible, then hidden, over a picture that advances every
       fourth present. No published frame may disagree with the live state. */
    psp_media_present_records_reset(records, PSP_DISPLAY_PAGE_BUFFER_COUNT);
    size_t begin = capture_count;
    uint64_t identity = 1;
    for (int at = 0; at < 24; at++) {
        if (at != 0 && at % 4 == 0) identity++;
        make_playing_state(&ui, &presentation,
                           UINT64_C(1000000) * (uint64_t) at, true, NULL);
        ordinary_present(&ui, identity, (uint16_t) (0x2000u + identity));
    }
    for (int at = 0; at < 24; at++) {
        if (at % 4 == 0) identity++;
        make_playing_state(&ui, &presentation,
                           UINT64_C(1000000) * (uint64_t) (24 + at), false,
                           NULL);
        ordinary_present(&ui, identity, (uint16_t) (0x2000u + identity));
    }
    check_window("S1-controls-fade", begin);

    /* S2: caption transitions with no controls: two-line cue, bridged gap,
       one-line cue. The box must exist exactly when text does. */
    psp_media_present_records_reset(records, PSP_DISPLAY_PAGE_BUFFER_COUNT);
    begin = capture_count;
    for (int at = 0; at < 16; at++) {
        if (at % 4 == 0) identity++;
        make_playing_state(&ui, &presentation, 0, false,
                           "FIRST CAPTION LINE THAT WRAPS ONTO A SECOND");
        ordinary_present(&ui, identity, (uint16_t) (0x2000u + identity));
    }
    for (int at = 0; at < 3; at++) {
        make_playing_state(&ui, &presentation, 0, false, "");
        ordinary_present(&ui, identity, (uint16_t) (0x2000u + identity));
    }
    for (int at = 0; at < 16; at++) {
        if (at % 4 == 0) identity++;
        make_playing_state(&ui, &presentation, 0, false, "SECOND");
        ordinary_present(&ui, identity, (uint16_t) (0x2000u + identity));
    }
    check_window("S2-caption-transition", begin);

    /* S3: frozen picture while chrome cycles: pause-like identity hold. */
    psp_media_present_records_reset(records, PSP_DISPLAY_PAGE_BUFFER_COUNT);
    begin = capture_count;
    identity++;
    for (int cycle = 0; cycle < 3; cycle++) {
        for (int at = 0; at < 9; at++) {
            make_playing_state(&ui, &presentation, 0, cycle % 2 == 0, NULL);
            ordinary_present(&ui, identity, (uint16_t) (0x2000u + identity));
        }
    }
    check_window("S3-frozen-picture-chrome-cycle", begin);

    /* S4: the cooperative supervisor interleave during a long native unit.
       The live state advances (time moves, the caption changes) while the
       supervisor republishes an immutable snapshot taken before the unit
       began, exactly as psp_present_supervisor_media does on this surface. */
    psp_media_present_records_reset(records, PSP_DISPLAY_PAGE_BUFFER_COUNT);
    begin = capture_count;
    identity++;
    PspUiMediaState snapshot;
    PspUiMediaPresentation snapshot_presentation;
    make_playing_state(&snapshot, &snapshot_presentation,
                       UINT64_C(30000000), true, "OLD CAPTION");
    for (int at = 0; at < 30; at++) {
        if (at % 4 == 0) identity++;
        make_playing_state(&ui, &presentation,
                           UINT64_C(30000000)
                               + UINT64_C(700000) * (uint64_t) at,
                           true, "NEW CAPTION AFTER THE TRANSITION");
        if (at % 5 == 4)
            supervisor_present(&snapshot, &ui);
        else
            ordinary_present(&ui, identity, (uint16_t) (0x2000u + identity));
    }
    check_window("S4-supervisor-snapshot-interleave", begin);

    /* An open is pumped under supervisor ownership: copying the initial
       loading frame and repainting only the footer leaves its central 0%
       unchanged throughout resolution. Compare against the normal painter. */
    static uint16_t loading[PSP_DISPLAY_BUFFER_PIXELS];
    static uint16_t expected_loading[PSP_DISPLAY_BUFFER_PIXELS];
    memset(loading, 0, sizeof(loading));
    memset(expected_loading, 0, sizeof(expected_loading));
    psp_ui_media_set_resolving(&ui, "Synthetic video");
    psp_ui_media_composite(&ui, loading, SCREEN_WIDTH, SCREEN_HEIGHT,
                           PSP_DISPLAY_STRIDE);
    psp_ui_media_set_resolving_progress(&ui, "Loading...", 530u);
    psp_ui_media_composite(&ui, expected_loading, SCREEN_WIDTH, SCREEN_HEIGHT,
                           PSP_DISPLAY_STRIDE);
    psp_ui_media_composite_supervisor_565(
        &ui, loading, SCREEN_WIDTH, SCREEN_HEIGHT, PSP_DISPLAY_STRIDE);
    if (memcmp(loading, expected_loading, sizeof(loading)) != 0) {
        fprintf(stderr, "S5-supervisor-loading: progress panel stayed stale\n");
        failures++;
    }
    /* Seeking must still update only the footer, preserving every picture
       and caption pixel exactly as the flicker fix requires. */
    memset(loading, 0x65, sizeof(loading));
    memcpy(expected_loading, loading, sizeof(loading));
    ui.seek_in_progress = true;
    psp_ui_media_composite_controls(&ui, expected_loading,
        SCREEN_WIDTH, SCREEN_HEIGHT, PSP_DISPLAY_STRIDE);
    psp_ui_media_composite_supervisor_565(&ui, loading,
        SCREEN_WIDTH, SCREEN_HEIGHT, PSP_DISPLAY_STRIDE);
    if (memcmp(loading, expected_loading, sizeof(loading)) != 0) {
        fprintf(stderr, "S6-supervisor-seek: frozen picture changed\n");
        failures++;
    }

    if (capture_count > CAPTURE_LIMIT) {
        fprintf(stderr, "capture overflow: %zu\n", capture_count);
        failures++;
    }
    if (failures != 0) {
        fprintf(stderr, "%d scenario failure(s)\n", failures);
        return 1;
    }
    printf("present chrome stability: all scenarios stable\n");
    return 0;
}
