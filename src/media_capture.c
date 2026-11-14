/* Page microphone capture: the session, the prompt's rules, frame policy
   and the sample ring (tilefinch/media_capture.h). */
#include "tilefinch/media_capture.h"

#include "tilefinch/platform.h"
#include "tilefinch/url.h"

#include <ctype.h>
#include <string.h>

const char *media_capture_verdict_error_name(MediaCaptureVerdict verdict)
{
    switch (verdict) {
    case MEDIA_CAPTURE_GRANTED: return "";
    case MEDIA_CAPTURE_DENIED_INSECURE: return "SecurityError";
    case MEDIA_CAPTURE_DENIED_LIMIT: return "AbortError";
    case MEDIA_CAPTURE_NO_DEVICE: return "NotFoundError";
    case MEDIA_CAPTURE_BUSY:
    case MEDIA_CAPTURE_FAILED: return "NotReadableError";
    default: return "NotAllowedError";
    }
}

const char *media_capture_verdict_message(MediaCaptureVerdict verdict)
{
    switch (verdict) {
    case MEDIA_CAPTURE_GRANTED: return "";
    case MEDIA_CAPTURE_DENIED_INSECURE:
        return "Microphone access requires a secure context";
    case MEDIA_CAPTURE_DENIED_POLICY:
        return "Microphone access is not allowed in this frame";
    case MEDIA_CAPTURE_DENIED_HIDDEN:
        return "Microphone access requires a visible page";
    case MEDIA_CAPTURE_DENIED_DISMISSED:
        return "Microphone permission was dismissed";
    case MEDIA_CAPTURE_DENIED_LIMIT:
        return "Too many live microphone tracks";
    case MEDIA_CAPTURE_NO_DEVICE: return "Requested device not found";
    case MEDIA_CAPTURE_BUSY:
        return "The microphone is in use by voice typing";
    case MEDIA_CAPTURE_FAILED: return "Could not start audio source";
    default: return "Permission denied";
    }
}

/* ---- The prompt ------------------------------------------------------- */

MediaCapturePromptAnswer media_capture_prompt_row_answer(unsigned row)
{
    static const MediaCapturePromptAnswer answers[
        MEDIA_CAPTURE_PROMPT_ROW_COUNT] = {
        MEDIA_CAPTURE_PROMPT_ALLOW_ONCE, MEDIA_CAPTURE_PROMPT_ALLOW_ALWAYS,
        MEDIA_CAPTURE_PROMPT_BLOCK
    };
    return row < MEDIA_CAPTURE_PROMPT_ROW_COUNT
        ? answers[row] : MEDIA_CAPTURE_PROMPT_DISMISSED;
}

void media_capture_prompt_modal_open(MediaCapturePromptModal *modal,
                                     bool can_remember, uint32_t held,
                                     uint64_t now_ms)
{
    if (modal == NULL) return;
    memset(modal, 0, sizeof(*modal));
    modal->can_remember = can_remember;
    modal->previous_buttons = held;
    modal->unreleased_buttons = held;
    modal->opened_ms = now_ms;
}

static uint8_t prompt_row_step(const MediaCapturePromptModal *modal,
                               int direction)
{
    unsigned row = modal->selection;
    /* Bounded: at most one skip per move. */
    for (unsigned tries = 0; tries < MEDIA_CAPTURE_PROMPT_ROW_COUNT; tries++) {
        row = (row + MEDIA_CAPTURE_PROMPT_ROW_COUNT + (unsigned) direction)
              % MEDIA_CAPTURE_PROMPT_ROW_COUNT;
        if (row != 1u || modal->can_remember) break;
    }
    return (uint8_t) row;
}

int media_capture_prompt_modal_step(MediaCapturePromptModal *modal,
                                    uint32_t held, uint64_t now_ms)
{
    if (modal == NULL) return MEDIA_CAPTURE_PROMPT_DISMISSED;
    uint64_t open_ms = now_ms > modal->opened_ms
        ? now_ms - modal->opened_ms : 0;
    if (open_ms >= MEDIA_CAPTURE_PROMPT_MAXIMUM_OPEN_MS)
        return MEDIA_CAPTURE_PROMPT_DISMISSED;
    uint32_t pressed = held & ~modal->previous_buttons;
    modal->previous_buttons = held;
    modal->unreleased_buttons &= held;
    if (!modal->armed && modal->unreleased_buttons == 0
        && open_ms >= MEDIA_CAPTURE_PROMPT_ARM_MS)
        modal->armed = true;
    if (pressed & MEDIA_CAPTURE_BUTTON_CIRCLE)
        return MEDIA_CAPTURE_PROMPT_DISMISSED;
    if (pressed & MEDIA_CAPTURE_BUTTON_UP)
        modal->selection = prompt_row_step(modal, -1);
    else if (pressed & MEDIA_CAPTURE_BUTTON_DOWN)
        modal->selection = prompt_row_step(modal, 1);
    if (pressed & MEDIA_CAPTURE_BUTTON_CROSS) {
        MediaCapturePromptAnswer answer =
            media_capture_prompt_row_answer(modal->selection);
        /* Refusing acts at once; granting waits until the prompt is armed,
           so a button held or mashed through it cannot allow. */
        if (answer == MEDIA_CAPTURE_PROMPT_BLOCK || modal->armed)
            return (int) answer;
    }
    return MEDIA_CAPTURE_PROMPT_PENDING;
}

/* ---- Permissions Policy for frames ----------------------------------- */

static bool policy_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static bool policy_token_is(const char *token, size_t length,
                            const char *literal)
{
    size_t literal_length = strlen(literal);
    if (length != literal_length) return false;
    for (size_t at = 0; at < length; at++)
        if (tolower((unsigned char) token[at]) != literal[at]) return false;
    return true;
}

/* 'src': the child is still at the origin its container's src named. */
static bool policy_src_admits(const char *child_origin, const char *src_origin)
{
    return child_origin != NULL && src_origin != NULL
        && child_origin[0] != '\0' && strcmp(child_origin, "null") != 0
        && strcmp(child_origin, src_origin) == 0;
}

/* Whether one allowlist token admits the child. */
static bool policy_token_admits(const char *token, size_t length,
                                bool same_origin_with_parent,
                                const char *child_origin,
                                const char *src_origin)
{
    if (policy_token_is(token, length, "*")) return true;
    if (policy_token_is(token, length, "'src'"))
        return policy_src_admits(child_origin, src_origin);
    if (policy_token_is(token, length, "'self'"))
        return same_origin_with_parent;
    if (child_origin == NULL || child_origin[0] == '\0' || length == 0
        || length >= TILEFINCH_ORIGIN_SERIALIZED_LIMIT)
        return false;
    char url[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    char origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    memcpy(url, token, length);
    url[length] = '\0';
    return tilefinch_url_origin(url, origin, sizeof(origin))
        && strcmp(origin, "null") != 0
        && strcmp(origin, child_origin) == 0;
}

bool media_capture_frame_allowed(const char *allow, size_t length,
                                 bool same_origin_with_parent,
                                 bool opaque_origin,
                                 const char *child_origin,
                                 const char *src_origin,
                                 bool parent_allowed)
{
    if (!parent_allowed || opaque_origin) return false;
    if (allow == NULL) length = 0;
    /* The attribute is page text: the walk is bounded by its length, and
       at most one pass over each byte. */
    size_t at = 0;
    while (at < length) {
        size_t end = at;
        while (end < length && allow[end] != ';') end++;
        size_t cursor = at;
        while (cursor < end && policy_space(allow[cursor])) cursor++;
        size_t name = cursor;
        while (cursor < end && !policy_space(allow[cursor])) cursor++;
        if (policy_token_is(allow + name, cursor - name, "microphone")) {
            /* The first "microphone" directive decides. An empty allowlist
               is 'src': the origin of the container's src. */
            bool any = false, admitted = false, none = false;
            while (cursor < end) {
                while (cursor < end && policy_space(allow[cursor])) cursor++;
                size_t token = cursor;
                while (cursor < end && !policy_space(allow[cursor])) cursor++;
                if (cursor == token) break;
                any = true;
                if (policy_token_is(allow + token, cursor - token, "'none'"))
                    none = true;
                else if (policy_token_admits(allow + token, cursor - token,
                                             same_origin_with_parent,
                                             child_origin, src_origin))
                    admitted = true;
            }
            if (!any) return policy_src_admits(child_origin, src_origin);
            return admitted && !none;
        }
        at = end + 1u;
    }
    return same_origin_with_parent;
}

/* ---- The sample ring --------------------------------------------------- */

/* The ring's state is one word, so the reader sees a consistent pair:
   the next write position in the low half, how many samples are valid
   (saturating at the capacity) in the high half. */
#define RING_HEAD(state) ((state) & 0xffffu)
#define RING_FILLED(state) ((state) >> 16)

void media_capture_ring_init(MediaCaptureRing *ring, int16_t *storage,
                             uint32_t capacity)
{
    if (ring == NULL) return;
    ring->samples = storage;
    ring->capacity = storage == NULL || capacity > 0xffffu ? 0 : capacity;
    atomic_store_explicit(&ring->written, 0u, memory_order_release);
}

void media_capture_ring_publish(MediaCaptureRing *ring,
                                const int16_t *samples, uint32_t count)
{
    if (ring == NULL || ring->capacity == 0 || samples == NULL) return;
    if (count > ring->capacity / 4u) count = ring->capacity / 4u;
    uint32_t state = atomic_load_explicit(&ring->written,
                                          memory_order_relaxed);
    uint32_t head = RING_HEAD(state), filled = RING_FILLED(state);
    for (uint32_t at = 0; at < count; at++) {
        ring->samples[head] = samples[at];
        head = head + 1u == ring->capacity ? 0u : head + 1u;
    }
    filled = count > ring->capacity - filled ? ring->capacity
                                             : filled + count;
    atomic_store_explicit(&ring->written, (filled << 16) | head,
                          memory_order_release);
}

uint32_t media_capture_ring_since(const MediaCaptureRing *ring,
                                  uint32_t *cursor, float *output,
                                  uint32_t count)
{
    if (ring == NULL || cursor == NULL || output == NULL
        || ring->capacity == 0) return 0;
    uint32_t state = atomic_load_explicit(
        (_Atomic uint32_t *) &ring->written, memory_order_acquire);
    uint32_t capacity = ring->capacity, head = RING_HEAD(state);
    if (*cursor >= capacity) {
        *cursor = head;
        return 0;
    }
    uint32_t waiting = head >= *cursor ? head - *cursor
                                       : head + capacity - *cursor;
    /* What a writer may be filling meanwhile is never read. */
    uint32_t readable = capacity > 4u * MEDIA_CAPTURE_BLOCK_SAMPLES
        ? capacity - 4u * MEDIA_CAPTURE_BLOCK_SAMPLES : 0u;
    if (waiting > readable) waiting = readable;
    if (waiting > RING_FILLED(state)) waiting = RING_FILLED(state);
    if (waiting > count) waiting = count;
    uint32_t index = head >= waiting ? head - waiting
                                     : head + capacity - waiting;
    for (uint32_t at = 0; at < waiting; at++) {
        output[at] = (float) ring->samples[index] / 32768.0f;
        index = index + 1u == capacity ? 0u : index + 1u;
    }
    *cursor = head;
    return waiting;
}

uint32_t media_capture_ring_written(const MediaCaptureRing *ring)
{
    return ring == NULL ? 0u
        : RING_FILLED(atomic_load_explicit(
              (_Atomic uint32_t *) &ring->written, memory_order_acquire));
}

void media_capture_block_levels(const int16_t *samples, size_t count,
                                unsigned *peak, uint64_t *mean_square)
{
    unsigned largest = 0;
    uint64_t sum = 0;
    for (size_t at = 0; samples != NULL && at < count; at++) {
        int value = samples[at];
        unsigned magnitude = (unsigned) (value < 0 ? -value : value);
        if (magnitude > largest) largest = magnitude;
        sum += (uint64_t) ((int64_t) value * value);
    }
    if (peak != NULL) *peak = largest;
    if (mean_square != NULL) *mean_square = count == 0 ? 0 : sum / count;
}

/* ---- The process's capture session ------------------------------------ */

enum { CAPTURE_ENDED_HISTORY = 8 };

static struct {
    uint32_t generation;
    uint32_t last_generation;
    unsigned tracks;
    uint32_t ended_generation[CAPTURE_ENDED_HISTORY];
    uint8_t ended_reason[CAPTURE_ENDED_HISTORY];
    uint8_t ended_next;
    uint32_t permission_epoch;
    uint32_t revoke_epoch;
} capture;

/* The indicator's copy of "live", for presentation on any thread (the
   busy-screen supervisor draws from the callback thread). */
static atomic_bool capture_indicator;

bool tilefinch_media_capture_indicator(void)
{
    return atomic_load_explicit(&capture_indicator, memory_order_acquire);
}

void tilefinch_media_capture_stop(MediaCaptureEndReason reason)
{
    if (capture.generation == 0) return;
    if (reason == MEDIA_CAPTURE_END_NONE) reason = MEDIA_CAPTURE_END_USER;
    capture.ended_generation[capture.ended_next] = capture.generation;
    capture.ended_reason[capture.ended_next] = (uint8_t) reason;
    capture.ended_next = (uint8_t) ((capture.ended_next + 1u)
                                    % CAPTURE_ENDED_HISTORY);
    capture.generation = 0;
    capture.tracks = 0;
    tilefinch_platform_media_capture_stop();
    atomic_store_explicit(&capture_indicator, false, memory_order_release);
}

/* A live session whose input failed or stalled since it started ends
   here, with MEDIA_CAPTURE_END_DEVICE, at the next question about it: the
   page's tracks then end at its next task boundary. */
static void media_capture_check_device(void)
{
    if (capture.generation != 0 && tilefinch_platform_media_capture_failed())
        tilefinch_media_capture_stop(MEDIA_CAPTURE_END_DEVICE);
}

bool tilefinch_media_capture_hand_over(MediaCaptureEndReason reason)
{
    tilefinch_media_capture_stop(reason);
    return tilefinch_platform_media_capture_input_free();
}

bool tilefinch_media_capture_live(void)
{
    media_capture_check_device();
    return capture.generation != 0;
}

uint32_t tilefinch_media_capture_generation(void)
{
    return capture.generation;
}

void tilefinch_media_capture_permissions_changed(void)
{
    capture.permission_epoch++;
    capture.revoke_epoch++;
}

void media_capture_permission_noted(void)
{
    capture.permission_epoch++;
}

uint32_t media_capture_revoke_epoch(void)
{
    return capture.revoke_epoch;
}

uint32_t tilefinch_media_capture_permission_epoch(void)
{
    return capture.permission_epoch;
}

MediaCaptureVerdict media_capture_session_open(uint32_t *generation,
                                               MediaCaptureFormat *format)
{
    MediaCaptureFormat shared = {
        .sample_rate = MEDIA_CAPTURE_SAMPLE_RATE,
        .channels = MEDIA_CAPTURE_CHANNELS,
        .latency = (double) MEDIA_CAPTURE_BLOCK_SAMPLES
                   / (double) MEDIA_CAPTURE_SAMPLE_RATE
    };
    if (capture.generation == 0) {
        MediaCaptureVerdict verdict =
            tilefinch_platform_media_capture_start(&shared);
        if (verdict != MEDIA_CAPTURE_GRANTED) return verdict;
        uint32_t next = capture.last_generation + 1u;
        if (next == 0) next = 1u;
        capture.generation = capture.last_generation = next;
        capture.tracks = 0;
        /* On before the page can hear anything: the device started and
           its first block is in the ring. */
        atomic_store_explicit(&capture_indicator, true,
                              memory_order_release);
    }
    capture.tracks++;
    if (generation != NULL) *generation = capture.generation;
    if (format != NULL) *format = shared;
    return MEDIA_CAPTURE_GRANTED;
}

void media_capture_session_release(uint32_t generation, unsigned tracks)
{
    if (generation == 0 || generation != capture.generation) return;
    capture.tracks = tracks >= capture.tracks ? 0 : capture.tracks - tracks;
    if (capture.tracks == 0) tilefinch_media_capture_stop(MEDIA_CAPTURE_END_PAGE);
}

MediaCaptureEndReason media_capture_session_end_reason(uint32_t generation)
{
    media_capture_check_device();
    if (generation != 0 && generation == capture.generation)
        return MEDIA_CAPTURE_END_NONE;
    for (unsigned at = 0; at < CAPTURE_ENDED_HISTORY; at++)
        if (generation != 0 && capture.ended_generation[at] == generation)
            return (MediaCaptureEndReason) capture.ended_reason[at];
    return MEDIA_CAPTURE_END_NAVIGATION;
}

uint32_t media_capture_session_read_since(float *output, uint32_t count,
                                          uint32_t *cursor)
{
    media_capture_check_device();
    if (output == NULL || cursor == NULL || capture.generation == 0)
        return 0;
    return tilefinch_platform_media_capture_read_since(output, count,
                                                       cursor);
}
