#include "tilefinch/media_backend.h"

#include <pspaudio.h>
#include <pspkernel.h>

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "psp_thread_contract.h"
#include "swdec/swdec_bounds.h"
#include "tilefinch/psp_threads.h"

#define SWDEC_RING_SLOTS TILEFINCH_SWDEC_CSC_SLOT_COUNT
#define SWDEC_AU_MAP_SLOTS 64u
#define SWDEC_AUDIO_PACKET_SLOTS 48u
#define SWDEC_AUDIO_AHEAD_US UINT64_C(80000)
#define SWDEC_AUDIO_PREBUFFER_FRAMES 16u
#define SWDEC_AUX_ARENA_BYTES (2560u * 1024u)
#define SWDEC_WORKER_STACK_BYTES (512u * 1024u)
#define SWDEC_AUDIO_STACK_BYTES (16u * 1024u)
#define SWDEC_EVENT_WAKE 1u
#define SWDEC_MAX_WIDTH 432u
#define SWDEC_MAX_HEIGHT 240u
#define SWDEC_MAX_REFS 16
#define SWDEC_PARAMETER_SET_SLACK 1024u
#define SWDEC_CACHE_LINE_BYTES 64u

typedef enum {
    SWDEC_SLOT_FREE = 0,
    SWDEC_SLOT_WRITING,
    SWDEC_SLOT_READY,
    SWDEC_SLOT_READING
} SwdecSlotState;

typedef enum {
    SWDEC_JOB_IDLE = 0,
    SWDEC_JOB_READY,
    SWDEC_JOB_RUNNING,
    SWDEC_JOB_DONE
} SwdecJobState;

typedef struct {
    atomic_int state;
    atomic_uint readers;
    uint32_t generation;
    uint64_t identity;
    uint64_t pts_us;
    uint64_t duration_us;
    bool claimed;
    bool quarantined;
} SwdecVideoSlot;

typedef struct {
    uint64_t au;
    uint64_t pts_us;
    uint64_t duration_us;
    int slot;
} SwdecAuSlot;

typedef struct {
    Budget *budget;
    const TilefinchSwdecComponentApi *api;
    void *decoder;
    void *decoder_arena;
    size_t decoder_arena_bytes;
    void *aux_arena;
    uint16_t *rgb_ring;
    size_t rgb_slot_bytes;
    size_t rgb_ring_bytes;
    unsigned rgb_stride;
    unsigned width;
    unsigned height;
    unsigned nal_length_size;
    unsigned char *parameter_sets;
    size_t parameter_sets_bytes;
    unsigned char *video_packet;
    size_t video_packet_capacity;
    unsigned char *audio_packets;
    size_t audio_packet_stride;
    MediaAacStreamInfo audio_info;
    bool have_audio;
    bool audio_arena_bound;

    SceUID worker_event;
    SceUID worker_thread;
    SceUID audio_thread;
    bool worker_started;
    bool audio_started;
    atomic_int job_state;
    atomic_bool stop;
    atomic_bool playing;
    atomic_bool buffering;
    atomic_uint ready_count;
    atomic_uint displayed_count;
    atomic_uint audio_submitted;
    atomic_uint audio_output;
    atomic_uint audio_epoch;
    atomic_uint audio_cursor_sequence;
    uint64_t audio_cursor_us;
    atomic_uint audio_origin_sequence;
    uint64_t audio_origin_us;
    atomic_uint shown_sequence;
    uint64_t shown_end_us;

    size_t job_bytes;
    int job_slot;
    uint64_t job_au;
    uint64_t job_pts_us;
    uint64_t job_duration_us;
    bool job_flush;
    int job_result;
    atomic_int worker_error;
    bool drained;

    SwdecVideoSlot slots[SWDEC_RING_SLOTS];
    SwdecAuSlot au_slots[SWDEC_AU_MAP_SLOTS];
    int pending_csc_slot;
    uint64_t pending_csc_au;
    int last_claimed_slot;
    uint64_t next_au;
    uint64_t next_identity;
    uint64_t epoch;
    uint64_t presentation_clock_us;
    uint64_t clock_slip_us;
    int speed;
    MediaBackendStats stats;
    BudgetReservation worker_stack_reservation;
} PspSwdecBackend;

static bool swdec_quarantined;

static size_t swdec_cache_extent(size_t bytes)
{
    return (bytes + SWDEC_CACHE_LINE_BYTES - 1u)
        & ~(size_t) (SWDEC_CACHE_LINE_BYTES - 1u);
}

static uint16_t *swdec_rgb_slot(PspSwdecBackend *backend, int slot)
{
    return (uint16_t *) ((unsigned char *) backend->rgb_ring
        + (size_t) slot * backend->rgb_slot_bytes);
}

bool media_psp_swdec_backend_quarantined(void)
{
    return swdec_quarantined;
}

static void swdec_error(
    char *error, size_t error_size, const char *format, ...)
{
    if (error == NULL || error_size == 0) return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

static uint64_t swdec_sample_time_us(const MediaMp4Sample *sample)
{
    if (sample == NULL || sample->timescale == 0) return 0;
    uint64_t value = sample->pts >= 0
        ? (uint64_t) sample->pts : sample->dts;
    if (sample->timescale == 90000u && value <= UINT32_MAX) {
        uint32_t ticks = (uint32_t) value;
        uint32_t seconds = ticks / 90000u;
        uint32_t remainder = ticks % 90000u;
        return (uint64_t) seconds * UINT64_C(1000000)
            + (uint32_t) (remainder * 100u / 9u);
    }
    uint64_t whole = value / sample->timescale;
    uint64_t remainder = value % sample->timescale;
    if (whole > UINT64_MAX / UINT64_C(1000000)) return UINT64_MAX;
    uint64_t base = whole * UINT64_C(1000000);
    uint64_t fraction = remainder * UINT64_C(1000000) / sample->timescale;
    return fraction > UINT64_MAX - base ? UINT64_MAX : base + fraction;
}

static uint64_t swdec_sample_duration_us(const MediaMp4Sample *sample)
{
    if (sample == NULL || sample->timescale == 0) return 0;
    if (sample->timescale == 90000u
        && sample->duration <= UINT32_MAX / 100u)
        return (uint32_t) (sample->duration * 100u / 9u);
    return (uint64_t) sample->duration * UINT64_C(1000000)
        / sample->timescale;
}

static bool swdec_append_start_code(
    unsigned char *output, size_t capacity, size_t *used,
    const unsigned char *nal, size_t bytes)
{
    if (output == NULL || used == NULL || nal == NULL
        || *used > capacity || bytes > capacity - *used
        || capacity - *used - bytes < 4u) return false;
    output[(*used)++] = 0;
    output[(*used)++] = 0;
    output[(*used)++] = 0;
    output[(*used)++] = 1;
    memcpy(output + *used, nal, bytes);
    *used += bytes;
    return true;
}

static bool swdec_build_parameter_sets(
    const unsigned char *config, size_t length,
    unsigned char *output, size_t capacity, size_t *written,
    unsigned *nal_length_size)
{
    if (written != NULL) *written = 0;
    if (nal_length_size != NULL) *nal_length_size = 0;
    if (config == NULL || length < 7u || output == NULL || written == NULL
        || nal_length_size == NULL || config[0] != 1u) return false;
    unsigned nls = (config[4] & 3u) + 1u;
    if (nls < 1u || nls > 4u) return false;
    size_t at = 6u;
    unsigned sps_count = config[5] & 31u;
    if (sps_count == 0) return false;
    for (unsigned i = 0; i < sps_count; i++) {
        if (at > length || length - at < 2u) return false;
        size_t bytes = ((size_t) config[at] << 8) | config[at + 1u];
        at += 2u;
        if (bytes == 0 || bytes > length - at
            || !swdec_append_start_code(
                   output, capacity, written, config + at, bytes)) return false;
        at += bytes;
    }
    if (at >= length) return false;
    unsigned pps_count = config[at++];
    if (pps_count == 0) return false;
    for (unsigned i = 0; i < pps_count; i++) {
        if (at > length || length - at < 2u) return false;
        size_t bytes = ((size_t) config[at] << 8) | config[at + 1u];
        at += 2u;
        if (bytes == 0 || bytes > length - at
            || !swdec_append_start_code(
                   output, capacity, written, config + at, bytes)) return false;
        at += bytes;
    }
    *nal_length_size = nls;
    return true;
}

static bool swdec_convert_avcc(
    PspSwdecBackend *backend, const unsigned char *payload, size_t length,
    bool keyframe, size_t *written)
{
    if (written != NULL) *written = 0;
    if (backend == NULL || payload == NULL || written == NULL) return false;
    size_t used = 0;
    if (keyframe && backend->parameter_sets_bytes != 0) {
        if (backend->parameter_sets_bytes > backend->video_packet_capacity)
            return false;
        memcpy(backend->video_packet, backend->parameter_sets,
               backend->parameter_sets_bytes);
        used = backend->parameter_sets_bytes;
    }
    size_t at = 0;
    while (at < length) {
        if (backend->nal_length_size > length - at) return false;
        size_t bytes = 0;
        for (unsigned i = 0; i < backend->nal_length_size; i++)
            bytes = (bytes << 8) | payload[at + i];
        at += backend->nal_length_size;
        if (bytes == 0 || bytes > length - at
            || !swdec_append_start_code(
                   backend->video_packet, backend->video_packet_capacity,
                   &used, payload + at, bytes)) return false;
        at += bytes;
    }
    *written = used;
    return true;
}

static bool swdec_packet_capacity(
    size_t sample_bytes, unsigned nal_length_size,
    size_t parameter_set_bytes, size_t *capacity)
{
    if (capacity == NULL || nal_length_size < 1u || nal_length_size > 4u)
        return false;
    size_t nal_count = sample_bytes / (nal_length_size + 1u);
    size_t expansion = nal_count * (4u - nal_length_size);
    if (sample_bytes > SIZE_MAX - expansion
        || sample_bytes + expansion > SIZE_MAX - parameter_set_bytes)
        return false;
    *capacity = sample_bytes + expansion + parameter_set_bytes;
    return true;
}

static bool swdec_annexb_has_nal(
    const unsigned char *bytes, size_t length, unsigned wanted)
{
    if (bytes == NULL) return false;
    for (size_t i = 0; i + 4u < length; i++) {
        size_t prefix = bytes[i] == 0 && bytes[i + 1u] == 0
            && bytes[i + 2u] == 1u ? 3u
            : bytes[i] == 0 && bytes[i + 1u] == 0
              && bytes[i + 2u] == 0 && bytes[i + 3u] == 1u ? 4u : 0u;
        if (prefix != 0 && (bytes[i + prefix] & 0x1fu) == wanted)
            return true;
    }
    return false;
}

static int swdec_find_free_slot(PspSwdecBackend *backend)
{
    for (unsigned i = 0; i < SWDEC_RING_SLOTS; i++) {
        int expected = SWDEC_SLOT_FREE;
        if (!backend->slots[i].quarantined
            && atomic_compare_exchange_strong_explicit(
                &backend->slots[i].state, &expected, SWDEC_SLOT_WRITING,
                memory_order_acq_rel, memory_order_acquire)) return (int) i;
    }
    return -1;
}

static void swdec_free_slot(PspSwdecBackend *backend, int slot)
{
    if (backend == NULL || slot < 0 || slot >= (int) SWDEC_RING_SLOTS)
        return;
    SwdecVideoSlot *state = &backend->slots[slot];
    state->claimed = false;
    if (!state->quarantined) {
        atomic_store_explicit(
            &state->state, SWDEC_SLOT_FREE, memory_order_release);
    }
}

static void swdec_close_pending_csc(PspSwdecBackend *backend)
{
    if (backend->pending_csc_slot < 0) return;
    int slot = backend->pending_csc_slot;
    uint64_t au = backend->pending_csc_au;
    if (!backend->api->csc_close()) {
        SwdecAuSlot *mapping = &backend->au_slots[au & 63u];
        if (mapping->au == au && mapping->slot == slot) mapping->slot = -1;
        swdec_free_slot(backend, slot);
    }
    backend->pending_csc_slot = -1;
}

static void swdec_publish_picture(
    PspSwdecBackend *backend, const TilefinchSwdecPicture *picture)
{
    uint64_t source_au = picture->pts;
    SwdecAuSlot *mapping = &backend->au_slots[source_au & 63u];
    if (mapping->au != source_au || mapping->slot < 0
        || mapping->slot >= (int) SWDEC_RING_SLOTS) {
        atomic_store_explicit(
            &backend->worker_error, -20, memory_order_release);
        return;
    }
    int slot = mapping->slot;
    if (picture->width != (int) backend->width
        || picture->height != (int) backend->height
        || !swdec_rgb565_destination_fits(
            picture->width, picture->height, (int) backend->rgb_stride,
            backend->rgb_slot_bytes)) {
        mapping->slot = -1;
        backend->api->csc_off();
        backend->pending_csc_slot = -1;
        swdec_free_slot(backend, slot);
        atomic_store_explicit(
            &backend->worker_error, -23, memory_order_release);
        return;
    }
    uint64_t pts_us = mapping->pts_us;
    uint64_t duration_us = mapping->duration_us;
    mapping->slot = -1;
    if (backend->pending_csc_slot == slot) {
        if (!backend->api->csc_close()) {
            backend->pending_csc_slot = -1;
            swdec_free_slot(backend, slot);
            atomic_store_explicit(
                &backend->worker_error, -21, memory_order_release);
            return;
        }
        backend->pending_csc_slot = -1;
    }
    /* The ME and the bounded CPU tail have both written this isolated
       surface and published their writes before csc_close returns. Discard
       any stale CPU cache lines before the browser stages the frame. */
    sceKernelDcacheInvalidateRange(
        swdec_rgb_slot(backend, slot), (unsigned) backend->rgb_slot_bytes);
    SwdecVideoSlot *state = &backend->slots[slot];
    state->generation++;
    if (state->generation == 0) state->generation = 1;
    state->identity = ++backend->next_identity;
    state->pts_us = pts_us;
    state->duration_us = duration_us;
    state->claimed = false;
    atomic_fetch_add_explicit(
        &backend->ready_count, 1u, memory_order_relaxed);
    atomic_store_explicit(
        &state->state, SWDEC_SLOT_READY, memory_order_release);
    backend->stats.decoded_video_frames++;
}

static void swdec_select_speed(PspSwdecBackend *backend)
{
    unsigned ready = atomic_load_explicit(
        &backend->ready_count, memory_order_relaxed);
    unsigned displayed = atomic_load_explicit(
        &backend->displayed_count, memory_order_relaxed);
    int speed = backend->speed;
    if (displayed == 0) speed = 0;
    else if (ready <= 2u) speed = 3;
    else if (ready <= 4u) speed = 4;
    else if (ready >= 8u) speed = 0;
    if (speed != backend->speed) {
        backend->api->set_speed(backend->decoder, speed);
        backend->speed = speed;
    }
}

static void swdec_decode_job(PspSwdecBackend *backend)
{
    swdec_close_pending_csc(backend);
    if (backend->job_slot >= 0) {
        SwdecAuSlot *mapping =
            &backend->au_slots[backend->job_au & 63u];
        if (mapping->slot >= 0) {
            atomic_store_explicit(
                &backend->worker_error, -22, memory_order_release);
            backend->job_result = -1;
            return;
        }
        mapping->au = backend->job_au;
        mapping->pts_us = backend->job_pts_us;
        mapping->duration_us = backend->job_duration_us;
        mapping->slot = backend->job_slot;
        backend->api->csc_begin(
            backend->job_slot, swdec_rgb_slot(backend, backend->job_slot),
            (int) backend->rgb_stride, backend->rgb_slot_bytes);
    } else {
        backend->api->csc_off();
    }
    swdec_select_speed(backend);
    TilefinchSwdecPicture picture = {0};
    int result = backend->api->decode(
        backend->decoder,
        backend->job_flush ? NULL : backend->video_packet,
        backend->job_flush ? 0 : backend->job_bytes,
        backend->job_au, &picture);
    backend->job_result = result;
    if (backend->job_slot >= 0) {
        backend->pending_csc_slot = backend->job_slot;
        backend->pending_csc_au = backend->job_au;
    }
    if (result == 1) swdec_publish_picture(backend, &picture);
    else if (result < 0) atomic_store_explicit(
        &backend->worker_error, result, memory_order_release);
    if (backend->job_flush && result == 0) backend->drained = true;
}

static int swdec_worker_main(SceSize arguments, void *argument)
{
    (void) arguments;
    PspSwdecBackend *backend = *(PspSwdecBackend **) argument;
    while (!atomic_load_explicit(&backend->stop, memory_order_acquire)) {
        u32 bits = 0;
        (void) sceKernelWaitEventFlag(
            backend->worker_event, SWDEC_EVENT_WAKE,
            PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR, &bits, NULL);
        if (atomic_load_explicit(&backend->stop, memory_order_acquire)) break;
        int expected = SWDEC_JOB_READY;
        if (!atomic_compare_exchange_strong_explicit(
                &backend->job_state, &expected, SWDEC_JOB_RUNNING,
                memory_order_acq_rel, memory_order_acquire)) continue;
        swdec_decode_job(backend);
        atomic_store_explicit(
            &backend->job_state, SWDEC_JOB_DONE, memory_order_release);
    }
    return 0;
}

static void swdec_publish_u64(
    atomic_uint *sequence, uint64_t *destination, uint64_t value)
{
    atomic_fetch_add_explicit(sequence, 1u, memory_order_acq_rel);
    *destination = value;
    atomic_fetch_add_explicit(sequence, 1u, memory_order_release);
}

static uint64_t swdec_read_u64(
    const atomic_uint *sequence, const uint64_t *source)
{
    for (;;) {
        unsigned before = atomic_load_explicit(sequence, memory_order_acquire);
        if ((before & 1u) != 0) continue;
        uint64_t value = *source;
        atomic_thread_fence(memory_order_acquire);
        unsigned after = atomic_load_explicit(sequence, memory_order_relaxed);
        if (before == after) return value;
    }
}

static int swdec_audio_main(SceSize arguments, void *argument)
{
    (void) arguments;
    PspSwdecBackend *backend = *(PspSwdecBackend **) argument;
    static short silence[2048];
    int channel = sceAudioChReserve(
        PSP_AUDIO_NEXT_CHANNEL, 1024, PSP_AUDIO_FORMAT_STEREO);
    if (channel < 0) {
        atomic_store_explicit(
            &backend->worker_error, channel, memory_order_release);
        return 0;
    }
    unsigned output = 0;
    unsigned audio_epoch = atomic_load_explicit(
        &backend->audio_epoch, memory_order_acquire);
    bool started = false;
    uint64_t cursor = 0;
    uint32_t numerator = UINT32_C(1024000000);
    uint32_t step = numerator / backend->audio_info.sample_rate;
    uint32_t remainder_step = numerator % backend->audio_info.sample_rate;
    uint32_t remainder = 0;
    while (!atomic_load_explicit(&backend->stop, memory_order_acquire)) {
        unsigned current_epoch = atomic_load_explicit(
            &backend->audio_epoch, memory_order_acquire);
        if (current_epoch != audio_epoch) {
            audio_epoch = current_epoch;
            output = 0;
            started = false;
            cursor = 0;
            remainder = 0;
        }
        unsigned done = backend->api->audio_done();
        bool running = atomic_load_explicit(
            &backend->playing, memory_order_acquire)
            && !atomic_load_explicit(
                &backend->buffering, memory_order_acquire);
        uint64_t shown_end = swdec_read_u64(
            &backend->shown_sequence, &backend->shown_end_us);
        bool audio_only = backend->decoder == NULL;
        bool gated = !audio_only && started
            && cursor > shown_end + SWDEC_AUDIO_AHEAD_US;
        if (!started && done >= SWDEC_AUDIO_PREBUFFER_FRAMES
            && (audio_only || shown_end != 0)) {
            cursor = swdec_read_u64(
                &backend->audio_origin_sequence, &backend->audio_origin_us);
            started = true;
        }
        if (running && started && !gated && done > output) {
            (void) sceAudioOutputBlocking(
                channel, PSP_AUDIO_VOLUME_MAX,
                (void *) backend->api->audio_pcm(output));
            output++;
            cursor += step;
            remainder += remainder_step;
            if (remainder >= backend->audio_info.sample_rate) {
                cursor++;
                remainder -= backend->audio_info.sample_rate;
            }
            atomic_store_explicit(
                &backend->audio_output, output, memory_order_release);
            swdec_publish_u64(
                &backend->audio_cursor_sequence,
                &backend->audio_cursor_us, cursor);
        } else {
            (void) sceAudioOutputBlocking(
                channel, PSP_AUDIO_VOLUME_MAX, silence);
        }
    }
    sceAudioChRelease(channel);
    return 0;
}

static MediaBackendResult swdec_collect(
    PspSwdecBackend *backend, char *error, size_t error_size)
{
    int state = atomic_load_explicit(&backend->job_state, memory_order_acquire);
    if (state == SWDEC_JOB_READY || state == SWDEC_JOB_RUNNING)
        return MEDIA_BACKEND_WOULD_BLOCK;
    if (state != SWDEC_JOB_DONE) return MEDIA_BACKEND_ACCEPTED;
    int result = backend->job_result;
    int worker_error = atomic_load_explicit(
        &backend->worker_error, memory_order_acquire);
    atomic_store_explicit(
        &backend->job_state, SWDEC_JOB_IDLE, memory_order_release);
    if (result < 0 || worker_error != 0) {
        backend->stats.last_native_error = worker_error != 0
            ? worker_error : result;
        swdec_error(error, error_size,
                    "software H.264 decode failed (%d/%d)",
                    result, worker_error);
        return MEDIA_BACKEND_ERROR;
    }
    return MEDIA_BACKEND_ACCEPTED;
}

static bool swdec_adts_header(
    const MediaAacStreamInfo *info, size_t payload_bytes,
    unsigned char output[7])
{
    static const unsigned rates[] = {
        96000, 88200, 64000, 48000, 44100, 32000, 24000,
        22050, 16000, 12000, 11025, 8000, 7350
    };
    unsigned rate_index = 13u;
    for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); i++)
        if (rates[i] == info->sample_rate) { rate_index = i; break; }
    size_t frame_bytes = payload_bytes + 7u;
    if (rate_index >= 13u || info->channels == 0 || info->channels > 2
        || frame_bytes > 0x1fffu) return false;
    unsigned channels = info->channels;
    output[0] = 0xffu;
    output[1] = 0xf1u;
    output[2] = (unsigned char) ((1u << 6) | (rate_index << 2)
                                 | (channels >> 2));
    output[3] = (unsigned char) (((channels & 3u) << 6)
                                 | (frame_bytes >> 11));
    output[4] = (unsigned char) (frame_bytes >> 3);
    output[5] = (unsigned char) ((frame_bytes << 5) | 0x1fu);
    output[6] = 0xfcu;
    return true;
}

static MediaBackendResult swdec_submit(
    void *opaque, const MediaMp4Sample *sample,
    const unsigned char *payload, size_t length,
    char *error, size_t error_size)
{
    PspSwdecBackend *backend = opaque;
    MediaBackendResult collected = swdec_collect(backend, error, error_size);
    if (collected == MEDIA_BACKEND_ERROR) return collected;
    if (sample->kind == MEDIA_MP4_TRACK_AUDIO) {
        if (!backend->have_audio) return MEDIA_BACKEND_ACCEPTED;
        unsigned submitted = atomic_load_explicit(
            &backend->audio_submitted, memory_order_acquire);
        unsigned done = backend->api->audio_done();
        if (submitted - done >= SWDEC_AUDIO_PACKET_SLOTS)
            return MEDIA_BACKEND_WOULD_BLOCK;
        bool adts = sample->packet_format == MEDIA_PACKET_FORMAT_AAC_ADTS;
        size_t header_bytes = adts ? 0u : 7u;
        if (length > backend->audio_packet_stride
            || header_bytes > backend->audio_packet_stride - length) {
            swdec_error(error, error_size, "AAC packet exceeds swdec bound");
            return MEDIA_BACKEND_ERROR;
        }
        unsigned char *packet = backend->audio_packets
            + (size_t) (submitted % SWDEC_AUDIO_PACKET_SLOTS)
              * backend->audio_packet_stride;
        if (!adts
            && !swdec_adts_header(&backend->audio_info, length, packet)) {
            swdec_error(error, error_size, "AAC config cannot form ADTS");
            return MEDIA_BACKEND_ERROR;
        }
        memcpy(packet + header_bytes, payload, length);
        /* The audio-only path never enters the video slice bridge whose
           whole-cache maintenance used to mask this missing handoff. Every
           packet is line-isolated, so a range writeback cannot touch Budget
           metadata or a neighbouring in-flight packet. */
        sceKernelDcacheWritebackRange(
            packet, (unsigned) swdec_cache_extent(length + header_bytes));
        if (submitted == 0) swdec_publish_u64(
            &backend->audio_origin_sequence, &backend->audio_origin_us,
            swdec_sample_time_us(sample));
        if (backend->api->audio_submit(
                packet, (unsigned) (length + header_bytes)) != 0)
            return MEDIA_BACKEND_WOULD_BLOCK;
        atomic_store_explicit(
            &backend->audio_submitted, submitted + 1u,
            memory_order_release);
        backend->stats.submitted_audio_packets++;
        return MEDIA_BACKEND_ACCEPTED;
    }
    if (sample->kind != MEDIA_MP4_TRACK_VIDEO)
        return MEDIA_BACKEND_ACCEPTED;
    if (atomic_load_explicit(&backend->job_state, memory_order_acquire)
        != SWDEC_JOB_IDLE) return MEDIA_BACKEND_WOULD_BLOCK;
    int slot = swdec_find_free_slot(backend);
    if (slot < 0) return MEDIA_BACKEND_WOULD_BLOCK;
    size_t written = 0;
    if (sample->packet_format == MEDIA_PACKET_FORMAT_H264_ANNEX_B) {
        bool prepend = sample->keyframe && backend->parameter_sets_bytes != 0
            && !swdec_annexb_has_nal(payload, length, 7u);
        size_t prefix = prepend ? backend->parameter_sets_bytes : 0u;
        if (length > backend->video_packet_capacity
            || prefix > backend->video_packet_capacity - length) {
            swdec_free_slot(backend, slot);
            swdec_error(error, error_size, "Annex-B H.264 AU exceeds bound");
            return MEDIA_BACKEND_ERROR;
        }
        if (prefix != 0)
            memcpy(backend->video_packet, backend->parameter_sets, prefix);
        memcpy(backend->video_packet + prefix, payload, length);
        written = prefix + length;
    } else if (!swdec_convert_avcc(
                   backend, payload, length, sample->keyframe, &written)) {
        swdec_free_slot(backend, slot);
        swdec_error(error, error_size, "invalid length-prefixed H.264 AU");
        return MEDIA_BACKEND_ERROR;
    }
    if (!media_h264_annexb_sample_is_admitted(
            backend->video_packet, written,
            (uint16_t) backend->width, (uint16_t) backend->height)) {
        swdec_free_slot(backend, slot);
        swdec_error(error, error_size,
                    "H.264 access unit changed decoded geometry");
        return MEDIA_BACKEND_ERROR;
    }
    backend->job_bytes = written;
    backend->job_slot = slot;
    backend->job_au = backend->next_au++;
    backend->job_pts_us = swdec_sample_time_us(sample);
    backend->job_duration_us = swdec_sample_duration_us(sample);
    backend->job_flush = false;
    backend->job_result = 0;
    atomic_store_explicit(
        &backend->worker_error, 0, memory_order_release);
    atomic_store_explicit(
        &backend->job_state, SWDEC_JOB_READY, memory_order_release);
    if (sceKernelSetEventFlag(backend->worker_event, SWDEC_EVENT_WAKE) < 0) {
        atomic_store_explicit(
            &backend->job_state, SWDEC_JOB_IDLE, memory_order_release);
        swdec_free_slot(backend, slot);
        swdec_error(error, error_size, "swdec worker wake failed");
        return MEDIA_BACKEND_ERROR;
    }
    backend->stats.submitted_video_packets++;
    return MEDIA_BACKEND_QUEUED;
}

static MediaBackendResult swdec_drain(
    void *opaque, char *error, size_t error_size)
{
    PspSwdecBackend *backend = opaque;
    MediaBackendResult collected = swdec_collect(backend, error, error_size);
    if (collected == MEDIA_BACKEND_ERROR) return collected;
    if (collected == MEDIA_BACKEND_WOULD_BLOCK) return collected;
    if (backend->decoder == NULL) {
        unsigned submitted = atomic_load_explicit(
            &backend->audio_submitted, memory_order_acquire);
        return backend->api->audio_done() < submitted
            ? MEDIA_BACKEND_WOULD_BLOCK : MEDIA_BACKEND_END;
    }
    if (backend->drained) return MEDIA_BACKEND_END;
    backend->job_bytes = 0;
    backend->job_slot = -1;
    backend->job_au = backend->next_au++;
    backend->job_pts_us += backend->job_duration_us;
    backend->job_flush = true;
    backend->job_result = 0;
    atomic_store_explicit(
        &backend->worker_error, 0, memory_order_release);
    atomic_store_explicit(
        &backend->job_state, SWDEC_JOB_READY, memory_order_release);
    if (sceKernelSetEventFlag(backend->worker_event, SWDEC_EVENT_WAKE) < 0) {
        atomic_store_explicit(
            &backend->job_state, SWDEC_JOB_IDLE, memory_order_release);
        swdec_error(error, error_size, "swdec drain wake failed");
        return MEDIA_BACKEND_ERROR;
    }
    return MEDIA_BACKEND_QUEUED;
}

static bool swdec_advance(
    void *opaque, uint64_t clock_us, char *error, size_t error_size)
{
    PspSwdecBackend *backend = opaque;
    backend->presentation_clock_us = clock_us;
    MediaBackendResult result = swdec_collect(backend, error, error_size);
    if (result == MEDIA_BACKEND_ERROR) return false;
    if (backend->api->me_failed != NULL && backend->api->me_failed()) {
        if (backend->api->recover_me() != 0
            || (backend->have_audio && backend->api->audio_setup() != 0)) {
            swdec_error(error, error_size,
                        "software decoder ME recovery failed");
            return false;
        }
    }
    return true;
}

static void swdec_release_prior_claim(PspSwdecBackend *backend, int keep)
{
    int prior = backend->last_claimed_slot;
    if (prior < 0 || prior == keep) return;
    SwdecVideoSlot *state = &backend->slots[prior];
    if (atomic_load_explicit(&state->readers, memory_order_acquire) == 0)
        swdec_free_slot(backend, prior);
    backend->last_claimed_slot = -1;
}

static bool swdec_take_video_frame(void *opaque, MediaVideoFrame *frame)
{
    PspSwdecBackend *backend = opaque;
    int selected = -1;
    uint64_t selected_pts = UINT64_MAX;
    uint64_t effective_clock = backend->presentation_clock_us
        >= backend->clock_slip_us
        ? backend->presentation_clock_us - backend->clock_slip_us : 0;
    for (unsigned i = 0; i < SWDEC_RING_SLOTS; i++) {
        SwdecVideoSlot *state = &backend->slots[i];
        if (atomic_load_explicit(&state->state, memory_order_acquire)
              != SWDEC_SLOT_READY
            || state->claimed || state->pts_us >= selected_pts)
            continue;
        selected = (int) i;
        selected_pts = state->pts_us;
    }
    if (selected < 0) return false;
    SwdecVideoSlot *state = &backend->slots[selected];
    if (selected_pts > effective_clock
        && selected_pts - effective_clock > state->duration_us / 2u)
        return false;
    if (effective_clock > selected_pts + 8000u) {
        uint64_t late = effective_clock - selected_pts;
        backend->clock_slip_us = late > UINT64_MAX - backend->clock_slip_us
            ? UINT64_MAX : backend->clock_slip_us + late;
    }
    swdec_release_prior_claim(backend, selected);
    state->claimed = true;
    backend->last_claimed_slot = selected;
    atomic_fetch_sub_explicit(
        &backend->ready_count, 1u, memory_order_relaxed);
    *frame = (MediaVideoFrame) {
        .pixels = swdec_rgb_slot(backend, selected),
        .width = (int) backend->width,
        .height = (int) backend->height,
        .stride_pixels = (int) backend->rgb_stride,
        .format = MEDIA_PIXEL_RGB565,
        .pts_us = state->pts_us,
        .duration_us = state->duration_us,
        .identity = state->identity,
        .slot = selected,
        .generation = state->generation,
        .epoch = backend->epoch
    };
    return true;
}

static size_t swdec_discard_video_before(void *opaque, uint64_t floor_us)
{
    PspSwdecBackend *backend = opaque;
    size_t discarded = 0;
    for (unsigned i = 0; i < SWDEC_RING_SLOTS; i++) {
        SwdecVideoSlot *state = &backend->slots[i];
        if (atomic_load_explicit(&state->state, memory_order_acquire)
              != SWDEC_SLOT_READY
            || state->claimed || state->pts_us >= floor_us) continue;
        atomic_fetch_sub_explicit(
            &backend->ready_count, 1u, memory_order_relaxed);
        swdec_free_slot(backend, (int) i);
        discarded++;
    }
    backend->stats.discarded_seek_video_frames += discarded;
    return discarded;
}

static bool swdec_stats(const void *opaque, MediaBackendStats *stats)
{
    const PspSwdecBackend *backend = opaque;
    if (backend == NULL || stats == NULL) return false;
    *stats = backend->stats;
    stats->audio_output_blocks = atomic_load_explicit(
        &backend->audio_output, memory_order_acquire);
    return true;
}

static bool swdec_audio_cursor(const void *opaque, uint64_t *cursor_us)
{
    const PspSwdecBackend *backend = opaque;
    if (backend == NULL || cursor_us == NULL || !backend->have_audio)
        return false;
    *cursor_us = swdec_read_u64(
        &backend->audio_cursor_sequence, &backend->audio_cursor_us);
    return true;
}

static unsigned swdec_ready_frames(const void *opaque)
{
    const PspSwdecBackend *backend = opaque;
    return backend == NULL ? 0 : atomic_load_explicit(
        &backend->ready_count, memory_order_acquire);
}

static bool swdec_ready_start(const void *opaque, uint64_t *start_us)
{
    const PspSwdecBackend *backend = opaque;
    if (backend == NULL || start_us == NULL) return false;
    uint64_t earliest = UINT64_MAX;
    for (unsigned i = 0; i < SWDEC_RING_SLOTS; i++) {
        const SwdecVideoSlot *slot = &backend->slots[i];
        if (atomic_load_explicit(&slot->state, memory_order_acquire)
              == SWDEC_SLOT_READY
            && !slot->claimed && slot->pts_us < earliest)
            earliest = slot->pts_us;
    }
    if (earliest == UINT64_MAX) return false;
    *start_us = earliest;
    return true;
}

static size_t swdec_displayed_frames(const void *opaque)
{
    const PspSwdecBackend *backend = opaque;
    return backend == NULL ? 0 : atomic_load_explicit(
        &backend->displayed_count, memory_order_acquire);
}

static void swdec_set_clock(void *opaque, uint64_t clock_us)
{
    ((PspSwdecBackend *) opaque)->presentation_clock_us = clock_us;
}

static void swdec_set_playing(void *opaque, bool playing)
{
    atomic_store_explicit(
        &((PspSwdecBackend *) opaque)->playing, playing,
        memory_order_release);
}

static void swdec_set_buffering(void *opaque, bool buffering)
{
    atomic_store_explicit(
        &((PspSwdecBackend *) opaque)->buffering, buffering,
        memory_order_release);
}

static bool swdec_borrow(void *opaque, unsigned slot, uint32_t generation)
{
    PspSwdecBackend *backend = opaque;
    if (backend == NULL || slot >= SWDEC_RING_SLOTS) return false;
    SwdecVideoSlot *state = &backend->slots[slot];
    int slot_state = atomic_load_explicit(&state->state, memory_order_acquire);
    if ((slot_state != SWDEC_SLOT_READY && slot_state != SWDEC_SLOT_READING)
        || state->generation != generation || state->quarantined) return false;
    atomic_fetch_add_explicit(&state->readers, 1u, memory_order_acq_rel);
    atomic_store_explicit(
        &state->state, SWDEC_SLOT_READING, memory_order_release);
    return true;
}

static void swdec_release(void *opaque, unsigned slot)
{
    PspSwdecBackend *backend = opaque;
    if (backend == NULL || slot >= SWDEC_RING_SLOTS) return;
    SwdecVideoSlot *state = &backend->slots[slot];
    unsigned readers = atomic_load_explicit(
        &state->readers, memory_order_acquire);
    if (readers == 0) return;
    readers = atomic_fetch_sub_explicit(
        &state->readers, 1u, memory_order_acq_rel) - 1u;
    if (readers == 0 && !state->quarantined)
        atomic_store_explicit(
            &state->state, SWDEC_SLOT_READY, memory_order_release);
}

static void swdec_quarantine(void *opaque, unsigned slot)
{
    PspSwdecBackend *backend = opaque;
    if (backend == NULL || slot >= SWDEC_RING_SLOTS) return;
    backend->slots[slot].quarantined = true;
    swdec_quarantined = true;
}

static void swdec_release_quarantine(void *opaque, unsigned slot)
{
    (void) opaque;
    (void) slot;
    /* A timed-out DMA may still read this memory. Process-lifetime only. */
}

static bool swdec_is_quarantined(const void *opaque, unsigned slot)
{
    const PspSwdecBackend *backend = opaque;
    return backend == NULL || slot >= SWDEC_RING_SLOTS
        || backend->slots[slot].quarantined;
}

static void swdec_note_displayed(
    void *opaque, const MediaVideoFrame *frame, int present_path)
{
    (void) present_path;
    PspSwdecBackend *backend = opaque;
    if (backend == NULL || frame == NULL) return;
    uint64_t end = frame->pts_us > UINT64_MAX - frame->duration_us
        ? UINT64_MAX : frame->pts_us + frame->duration_us;
    swdec_publish_u64(&backend->shown_sequence, &backend->shown_end_us, end);
    atomic_fetch_add_explicit(
        &backend->displayed_count, 1u, memory_order_relaxed);
}

static void swdec_note_quiesced(
    void *opaque, const MediaVideoFrame *frame)
{
    PspSwdecBackend *backend = opaque;
    if (backend == NULL || frame == NULL || frame->slot < 0
        || frame->slot >= (int) SWDEC_RING_SLOTS) return;
    swdec_free_slot(backend, frame->slot);
    if (backend->last_claimed_slot == frame->slot)
        backend->last_claimed_slot = -1;
}

static bool swdec_release_slot(
    void *opaque, unsigned slot, uint32_t generation)
{
    PspSwdecBackend *backend = opaque;
    if (backend == NULL || slot >= SWDEC_RING_SLOTS) return false;
    SwdecVideoSlot *state = &backend->slots[slot];
    if (state->generation != generation
        || atomic_load_explicit(&state->readers, memory_order_acquire) != 0)
        return false;
    swdec_free_slot(backend, (int) slot);
    if (backend->last_claimed_slot == (int) slot)
        backend->last_claimed_slot = -1;
    return true;
}

static const MediaBackendPresentationOps swdec_presentation_ops = {
    .borrow = swdec_borrow,
    .release = swdec_release,
    .end_auxiliary_read = swdec_release,
    .quarantine = swdec_quarantine,
    .release_quarantine = swdec_release_quarantine,
    .is_quarantined = swdec_is_quarantined,
    .note_displayed = swdec_note_displayed,
    .note_quiesced = swdec_note_quiesced
};

static bool swdec_reset(void *opaque, char *error, size_t error_size)
{
    PspSwdecBackend *backend = opaque;
    if (backend == NULL) return false;
    if (atomic_load_explicit(&backend->job_state, memory_order_acquire)
          != SWDEC_JOB_IDLE) {
        swdec_error(error, error_size,
                    "software decoder is still finishing an access unit");
        return false;
    }
    for (unsigned i = 0; i < SWDEC_RING_SLOTS; i++) {
        if (atomic_load_explicit(
                &backend->slots[i].readers, memory_order_acquire) != 0) {
            swdec_error(error, error_size,
                        "software decoder surface is still borrowed");
            return false;
        }
    }
    if (backend->decoder != NULL) {
        swdec_close_pending_csc(backend);
        backend->api->csc_off();
    }
    if (backend->have_audio
        && backend->api->audio_shutdown(2000000u) != 0) {
        swdec_error(error, error_size,
                    "software decoder audio queue did not quiesce");
        return false;
    }
    if (backend->have_audio) {
        backend->audio_arena_bound = false;
        backend->api->bind_aux_arena(
            backend->aux_arena, SWDEC_AUX_ARENA_BYTES);
        backend->audio_arena_bound = true;
        if (backend->api->audio_setup() != 0) {
            swdec_error(error, error_size,
                        "software decoder audio reopen failed at seek");
            return false;
        }
    }
    if (backend->decoder != NULL) {
        backend->api->close(backend->decoder);
        backend->decoder = NULL;
        backend->decoder = backend->api->open(
            backend->decoder_arena, backend->decoder_arena_bytes,
            (int) backend->width, (int) backend->height, SWDEC_MAX_REFS);
        if (backend->decoder == NULL) {
            swdec_error(error, error_size,
                        "software decoder reopen failed at seek");
            return false;
        }
    }
    for (unsigned i = 0; i < SWDEC_AU_MAP_SLOTS; i++)
        backend->au_slots[i].slot = -1;
    for (unsigned i = 0; i < SWDEC_RING_SLOTS; i++) {
        SwdecVideoSlot *slot = &backend->slots[i];
        slot->claimed = false;
        slot->quarantined = false;
        atomic_store_explicit(
            &slot->state, SWDEC_SLOT_FREE, memory_order_release);
    }
    backend->pending_csc_slot = -1;
    backend->last_claimed_slot = -1;
    backend->drained = false;
    backend->clock_slip_us = 0;
    backend->speed = -1;
    backend->epoch++;
    if (backend->epoch == 0) backend->epoch = 1;
    atomic_store_explicit(&backend->ready_count, 0u, memory_order_release);
    atomic_store_explicit(
        &backend->audio_submitted, 0u, memory_order_release);
    atomic_store_explicit(&backend->audio_output, 0u, memory_order_release);
    atomic_fetch_add_explicit(
        &backend->audio_epoch, 1u, memory_order_acq_rel);
    swdec_publish_u64(
        &backend->audio_cursor_sequence, &backend->audio_cursor_us, 0);
    swdec_publish_u64(
        &backend->audio_origin_sequence, &backend->audio_origin_us, 0);
    atomic_store_explicit(
        &backend->worker_error, 0, memory_order_release);
    return true;
}

static void swdec_destroy(void *opaque)
{
    PspSwdecBackend *backend = opaque;
    if (backend == NULL) return;
    atomic_store_explicit(&backend->stop, true, memory_order_release);
    if (backend->worker_started && backend->worker_event >= 0)
        (void) sceKernelSetEventFlag(backend->worker_event, SWDEC_EVENT_WAKE);
    bool worker_stopped = !backend->worker_started
        || psp_thread_wait_end_bounded(
               backend->worker_thread, 2000000u) >= 0;
    bool audio_stopped = !backend->audio_started
        || psp_thread_wait_end_bounded(
               backend->audio_thread, 2000000u) >= 0;
    if (!worker_stopped || !audio_stopped) {
        swdec_quarantined = true;
        return;
    }
    if (backend->worker_thread >= 0)
        sceKernelDeleteThread(backend->worker_thread);
    if (backend->audio_thread >= 0)
        sceKernelDeleteThread(backend->audio_thread);
    if (backend->worker_event >= 0)
        sceKernelDeleteEventFlag(backend->worker_event);
    if (backend->decoder != NULL) {
        swdec_close_pending_csc(backend);
        backend->api->csc_off();
    }
    if (backend->audio_arena_bound
        && backend->api->audio_shutdown(2000000u) != 0) {
        swdec_quarantined = true;
        return;
    }
    if (backend->decoder != NULL) backend->api->close(backend->decoder);
    budget_reservation_release(&backend->worker_stack_reservation);
    budget_free(backend->budget, backend->audio_packets);
    budget_free(backend->budget, backend->video_packet);
    budget_free(backend->budget, backend->parameter_sets);
    budget_free(backend->budget, backend->rgb_ring);
    budget_free(backend->budget, backend->aux_arena);
    budget_free(backend->budget, backend->decoder_arena);
    budget_free(backend->budget, backend);
}

bool media_psp_swdec_backend_create_sources(
    Budget *budget, const MediaSampleSource *video_source,
    const MediaSampleSource *audio_source,
    const TilefinchSwdecComponentApi *component,
    MediaBackend *backend_out, char *error, size_t error_size)
{
    if (error != NULL && error_size != 0) error[0] = '\0';
    if (budget == NULL || video_source == NULL || video_source->ops == NULL
        || component == NULL
        || backend_out == NULL || swdec_quarantined) {
        swdec_error(error, error_size, "software decoder unavailable");
        return false;
    }
    MediaMp4TrackInfo video = {0};
    MediaMp4TrackInfo audio = {0};
    bool have_video = false, have_audio = false;
    const MediaSampleSource *sources[2] = {video_source, audio_source};
    size_t source_count = audio_source == NULL ? 1u : 2u;
    for (size_t source = 0; source < source_count; source++) {
        if (sources[source] == NULL || sources[source]->ops == NULL) continue;
        for (size_t i = 0;
             i < sources[source]->ops->track_count(sources[source]->opaque);
             i++) {
            MediaMp4TrackInfo info;
            if (!sources[source]->ops->track_info(
                    sources[source]->opaque, i, &info)) continue;
            if (!have_video && info.kind == MEDIA_MP4_TRACK_VIDEO
                && info.codec == MEDIA_MP4_FOURCC('a','v','c','1')) {
                video = info;
                have_video = true;
            } else if (!have_audio && info.kind == MEDIA_MP4_TRACK_AUDIO
                       && info.codec == MEDIA_MP4_FOURCC('m','p','4','a')) {
                audio = info;
                have_audio = true;
            }
        }
    }
    uint8_t profile = 0;
    uint16_t width = 0, height = 0;
    uint8_t nal_length = 0;
    bool annex_b = video.packet_format == MEDIA_PACKET_FORMAT_H264_ANNEX_B;
    if (annex_b) {
        width = video.width;
        height = video.height;
        for (size_t i = 0; i + 4u < video.codec_config_length; i++) {
            size_t prefix = video.codec_config[i] == 0
                && video.codec_config[i + 1u] == 0
                && video.codec_config[i + 2u] == 1u ? 3u
                : video.codec_config[i] == 0
                  && video.codec_config[i + 1u] == 0
                  && video.codec_config[i + 2u] == 0
                  && video.codec_config[i + 3u] == 1u ? 4u : 0u;
            if (prefix != 0
                && (video.codec_config[i + prefix] & 0x1fu) == 7u
                && i + prefix + 1u < video.codec_config_length) {
                profile = video.codec_config[i + prefix + 1u];
                break;
            }
        }
    }
    if (!have_video
        || (!annex_b
            && (media_h264_avcc_decoder_route(
                    video.codec_config, video.codec_config_length, &profile)
                  == MEDIA_H264_DECODER_ROUTE_UNSUPPORTED
                || !media_h264_avcc_dimensions(
                    video.codec_config, video.codec_config_length,
                    &width, &height, &nal_length)))
        || width == 0 || height == 0
        || width > SWDEC_MAX_WIDTH || height > SWDEC_MAX_HEIGHT) {
        swdec_error(error, error_size,
                    "software H.264 is limited to 432x240");
        return false;
    }
    (void) profile;
    PspSwdecBackend *backend = budget_calloc_category(
        budget, BUDGET_CATEGORY_NAVIGATION, 1, sizeof(*backend));
    if (backend == NULL) {
        swdec_error(error, error_size, "swdec state exceeds budget");
        return false;
    }
    backend->budget = budget;
    backend->api = component;
    backend->worker_event = -1;
    backend->worker_thread = -1;
    backend->audio_thread = -1;
    backend->pending_csc_slot = -1;
    backend->last_claimed_slot = -1;
    backend->width = width;
    backend->height = height;
    backend->rgb_stride = (width + 15u) & ~15u;
    backend->nal_length_size = nal_length;
    backend->speed = -1;
    backend->epoch = 1;
    for (unsigned i = 0; i < SWDEC_AU_MAP_SLOTS; i++)
        backend->au_slots[i].slot = -1;
    for (unsigned i = 0; i < SWDEC_RING_SLOTS; i++) {
        atomic_init(&backend->slots[i].state, SWDEC_SLOT_FREE);
        atomic_init(&backend->slots[i].readers, 0u);
    }
    atomic_init(&backend->job_state, SWDEC_JOB_IDLE);
    atomic_init(&backend->stop, false);
    atomic_init(&backend->playing, false);
    atomic_init(&backend->buffering, false);
    atomic_init(&backend->ready_count, 0u);
    atomic_init(&backend->displayed_count, 0u);
    atomic_init(&backend->audio_submitted, 0u);
    atomic_init(&backend->audio_output, 0u);
    atomic_init(&backend->audio_epoch, 1u);
    atomic_init(&backend->audio_cursor_sequence, 0u);
    atomic_init(&backend->audio_origin_sequence, 0u);
    atomic_init(&backend->shown_sequence, 0u);
    atomic_init(&backend->worker_error, 0);

    backend->decoder_arena_bytes = component->arena_bytes(
        (int) width, (int) height, SWDEC_MAX_REFS);
    backend->rgb_slot_bytes = swdec_cache_extent(
        (size_t) backend->rgb_stride * height * sizeof(uint16_t));
    backend->rgb_ring_bytes = backend->rgb_slot_bytes * SWDEC_RING_SLOTS;
    size_t parameter_capacity = video.codec_config_length
        + SWDEC_PARAMETER_SET_SLACK;
    backend->parameter_sets = budget_malloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, parameter_capacity);
    backend->decoder_arena = budget_malloc_cacheline_category(
        budget, BUDGET_CATEGORY_RESOURCE, backend->decoder_arena_bytes);
    if (have_audio) backend->aux_arena = budget_malloc_cacheline_category(
        budget, BUDGET_CATEGORY_RESOURCE, SWDEC_AUX_ARENA_BYTES);
    backend->rgb_ring = budget_malloc_cacheline_category(
        budget, BUDGET_CATEGORY_RENDER, backend->rgb_ring_bytes);
    bool packet_capacity_ok = annex_b
        ? video.largest_sample <= SIZE_MAX - parameter_capacity
        : swdec_packet_capacity(
              video.largest_sample, backend->nal_length_size,
              parameter_capacity, &backend->video_packet_capacity);
    if (annex_b)
        backend->video_packet_capacity = video.largest_sample
            + parameter_capacity;
    if (!packet_capacity_ok) {
        swdec_error(error, error_size, "swdec packet bound overflow");
        swdec_destroy(backend);
        return false;
    }
    backend->video_packet = budget_malloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, backend->video_packet_capacity);
    if (backend->parameter_sets == NULL || backend->decoder_arena == NULL
        || (have_audio && backend->aux_arena == NULL)
        || backend->rgb_ring == NULL
        || backend->video_packet == NULL
        || !budget_reservation_acquire(
            &backend->worker_stack_reservation, budget,
            BUDGET_CATEGORY_RESOURCE,
            SWDEC_WORKER_STACK_BYTES + SWDEC_AUDIO_STACK_BYTES)) {
        swdec_error(error, error_size, "swdec working set exceeds budget");
        swdec_destroy(backend);
        return false;
    }
    bool parameter_sets_ok;
    if (annex_b) {
        parameter_sets_ok = video.codec_config != NULL
            && video.codec_config_length != 0
            && video.codec_config_length <= parameter_capacity;
        if (parameter_sets_ok) {
            memcpy(backend->parameter_sets, video.codec_config,
                   video.codec_config_length);
            backend->parameter_sets_bytes = video.codec_config_length;
            backend->nal_length_size = 0;
        }
    } else {
        parameter_sets_ok = swdec_build_parameter_sets(
            video.codec_config, video.codec_config_length,
            backend->parameter_sets, parameter_capacity,
            &backend->parameter_sets_bytes, &backend->nal_length_size);
    }
    if (!parameter_sets_ok) {
        swdec_error(error, error_size, "invalid AVC decoder configuration");
        swdec_destroy(backend);
        return false;
    }
    backend->decoder = component->open(
        backend->decoder_arena, backend->decoder_arena_bytes,
        (int) width, (int) height, SWDEC_MAX_REFS);
    if (backend->decoder == NULL) {
        swdec_error(error, error_size, "software H.264 decoder open failed");
        swdec_destroy(backend);
        return false;
    }
    if (have_audio) {
        bool adts = audio.packet_format == MEDIA_PACKET_FORMAT_AAC_ADTS;
        bool audio_info_ok = adts
            ? audio.sample_rate != 0 && audio.channels != 0
            : media_aac_esds_stream_info(
                  audio.codec_config, audio.codec_config_length,
                  &backend->audio_info);
        if (adts) backend->audio_info = (MediaAacStreamInfo) {
            .sample_rate = audio.sample_rate,
            .channels = audio.channels,
            .samples_per_frame = 1024u
        };
        if (!audio_info_ok
            || backend->audio_info.channels == 0
            || backend->audio_info.channels > 2
            || backend->audio_info.sample_rate == 0
            || audio.largest_sample > 16384u) {
            swdec_error(error, error_size, "swdec AAC stream unsupported");
            swdec_destroy(backend);
            return false;
        }
        backend->audio_packet_stride = swdec_cache_extent(
            (size_t) audio.largest_sample + (adts ? 0u : 7u));
        backend->audio_packets = budget_malloc_cacheline_category(
            budget, BUDGET_CATEGORY_RESOURCE,
            backend->audio_packet_stride * SWDEC_AUDIO_PACKET_SLOTS);
        component->bind_aux_arena(
            backend->aux_arena, SWDEC_AUX_ARENA_BYTES);
        backend->audio_arena_bound = true;
        if (backend->audio_packets == NULL || component->audio_setup() != 0) {
            swdec_error(error, error_size, "swdec AAC setup failed");
            swdec_destroy(backend);
            return false;
        }
        backend->have_audio = true;
        backend->stats.audio_sample_rate = backend->audio_info.sample_rate;
    }
    backend->worker_event = sceKernelCreateEventFlag(
        "TilefinchSwdec", 0, 0, NULL);
    backend->worker_thread = sceKernelCreateThread(
        "TilefinchSwdec", swdec_worker_main,
        TILEFINCH_PSP_THREAD_PRIORITY_SWDEC,
        SWDEC_WORKER_STACK_BYTES, PSP_THREAD_ATTR_USER, NULL);
    if (backend->worker_event < 0 || backend->worker_thread < 0
        || sceKernelStartThread(
               backend->worker_thread, sizeof(backend), &backend) < 0) {
        swdec_error(error, error_size, "swdec worker start failed");
        swdec_destroy(backend);
        return false;
    }
    backend->worker_started = true;
    if (backend->have_audio) {
        backend->audio_thread = sceKernelCreateThread(
            "TilefinchSwdecAudio", swdec_audio_main,
            TILEFINCH_PSP_THREAD_PRIORITY_AUDIO,
            SWDEC_AUDIO_STACK_BYTES, PSP_THREAD_ATTR_USER, NULL);
        if (backend->audio_thread < 0
            || sceKernelStartThread(
                   backend->audio_thread, sizeof(backend), &backend) < 0) {
            swdec_error(error, error_size, "swdec audio worker start failed");
            swdec_destroy(backend);
            return false;
        }
        backend->audio_started = true;
    }
    backend->stats.external_bytes =
        SWDEC_WORKER_STACK_BYTES + SWDEC_AUDIO_STACK_BYTES;
    *backend_out = (MediaBackend) {
        .opaque = backend,
        .presentation = &swdec_presentation_ops,
        .startup_ready_frames = 20u,
        .preferred_decode_lead_us = 1000000u,
        .submit = swdec_submit,
        .drain = swdec_drain,
        .advance = swdec_advance,
        .take_video_frame = swdec_take_video_frame,
        .discard_video_before = swdec_discard_video_before,
        .stats = swdec_stats,
        .audio_cursor_us = swdec_audio_cursor,
        .ready_video_frames = swdec_ready_frames,
        .ready_video_start_us = swdec_ready_start,
        .displayed_video_frames = swdec_displayed_frames,
        .set_presentation_clock_us = swdec_set_clock,
        .set_playing = swdec_set_playing,
        .set_buffering = swdec_set_buffering,
        .reset = swdec_reset,
        .destroy = swdec_destroy,
        .release_video_slot = swdec_release_slot
    };
    return true;
}

bool media_psp_swdec_backend_create_split(
    Budget *budget, const MediaMp4Demux *video_demux,
    const MediaMp4Demux *audio_demux,
    const TilefinchSwdecComponentApi *component,
    MediaBackend *backend_out, char *error, size_t error_size)
{
    MediaSampleSource video = {0}, audio = {0};
    if (!media_sample_source_from_mp4(
            (MediaMp4Demux *) video_demux, &video)) return false;
    const MediaSampleSource *audio_pointer = NULL;
    if (audio_demux != NULL) {
        if (!media_sample_source_from_mp4(
                (MediaMp4Demux *) audio_demux, &audio)) return false;
        audio_pointer = &audio;
    }
    return media_psp_swdec_backend_create_sources(
        budget, &video, audio_pointer, component,
        backend_out, error, error_size);
}

bool media_psp_swdec_backend_create_audio(
    Budget *budget, const MediaMp4Demux *audio_demux,
    const TilefinchSwdecComponentApi *component,
    MediaBackend *backend_out, char *error, size_t error_size)
{
    if (error != NULL && error_size != 0) error[0] = '\0';
    if (budget == NULL || audio_demux == NULL || component == NULL
        || backend_out == NULL || swdec_quarantined) {
        swdec_error(error, error_size, "software AAC decoder unavailable");
        return false;
    }
    MediaMp4TrackInfo audio = {0};
    bool have_audio = false;
    for (size_t i = 0; i < media_mp4_track_count(audio_demux); i++) {
        MediaMp4TrackInfo info;
        if (media_mp4_track_info(audio_demux, i, &info)
            && info.kind == MEDIA_MP4_TRACK_AUDIO
            && info.codec == MEDIA_MP4_FOURCC('m','p','4','a')) {
            audio = info;
            have_audio = true;
            break;
        }
    }
    MediaAacStreamInfo audio_info = {0};
    if (!have_audio
        || !media_aac_esds_stream_info(
            audio.codec_config, audio.codec_config_length, &audio_info)
        || audio_info.channels == 0 || audio_info.channels > 2
        || audio_info.sample_rate == 0 || audio.largest_sample > 16384u) {
        swdec_error(error, error_size, "software AAC stream unsupported");
        return false;
    }
    PspSwdecBackend *backend = budget_calloc_category(
        budget, BUDGET_CATEGORY_NAVIGATION, 1, sizeof(*backend));
    if (backend == NULL) {
        swdec_error(error, error_size, "swdec audio state exceeds budget");
        return false;
    }
    backend->budget = budget;
    backend->api = component;
    backend->worker_event = -1;
    backend->worker_thread = -1;
    backend->audio_thread = -1;
    backend->pending_csc_slot = -1;
    backend->last_claimed_slot = -1;
    backend->speed = -1;
    backend->epoch = 1;
    backend->have_audio = true;
    backend->audio_info = audio_info;
    backend->audio_packet_stride = swdec_cache_extent(
        (size_t) audio.largest_sample + 7u);
    for (unsigned i = 0; i < SWDEC_AU_MAP_SLOTS; i++)
        backend->au_slots[i].slot = -1;
    for (unsigned i = 0; i < SWDEC_RING_SLOTS; i++) {
        atomic_init(&backend->slots[i].state, SWDEC_SLOT_FREE);
        atomic_init(&backend->slots[i].readers, 0u);
    }
    atomic_init(&backend->job_state, SWDEC_JOB_IDLE);
    atomic_init(&backend->stop, false);
    atomic_init(&backend->playing, false);
    atomic_init(&backend->buffering, false);
    atomic_init(&backend->ready_count, 0u);
    atomic_init(&backend->displayed_count, 0u);
    atomic_init(&backend->audio_submitted, 0u);
    atomic_init(&backend->audio_output, 0u);
    atomic_init(&backend->audio_epoch, 1u);
    atomic_init(&backend->audio_cursor_sequence, 0u);
    atomic_init(&backend->audio_origin_sequence, 0u);
    atomic_init(&backend->shown_sequence, 0u);
    atomic_init(&backend->worker_error, 0);

    backend->aux_arena = budget_malloc_cacheline_category(
        budget, BUDGET_CATEGORY_RESOURCE, SWDEC_AUX_ARENA_BYTES);
    backend->audio_packets = budget_malloc_cacheline_category(
        budget, BUDGET_CATEGORY_RESOURCE,
        backend->audio_packet_stride * SWDEC_AUDIO_PACKET_SLOTS);
    if (backend->aux_arena == NULL || backend->audio_packets == NULL
        || !budget_reservation_acquire(
            &backend->worker_stack_reservation, budget,
            BUDGET_CATEGORY_RESOURCE, SWDEC_AUDIO_STACK_BYTES)) {
        swdec_error(error, error_size,
                    "swdec audio working set exceeds budget");
        swdec_destroy(backend);
        return false;
    }
    component->bind_aux_arena(
        backend->aux_arena, SWDEC_AUX_ARENA_BYTES);
    backend->audio_arena_bound = true;
    if (component->audio_setup() != 0) {
        swdec_error(error, error_size, "swdec AAC setup failed");
        swdec_destroy(backend);
        return false;
    }
    backend->audio_thread = sceKernelCreateThread(
        "TilefinchSwdecAudio", swdec_audio_main,
        TILEFINCH_PSP_THREAD_PRIORITY_AUDIO,
        SWDEC_AUDIO_STACK_BYTES, PSP_THREAD_ATTR_USER, NULL);
    if (backend->audio_thread < 0
        || sceKernelStartThread(
               backend->audio_thread, sizeof(backend), &backend) < 0) {
        swdec_error(error, error_size, "swdec audio worker start failed");
        swdec_destroy(backend);
        return false;
    }
    backend->audio_started = true;
    backend->stats.audio_sample_rate = audio_info.sample_rate;
    backend->stats.external_bytes = SWDEC_AUDIO_STACK_BYTES;
    *backend_out = (MediaBackend) {
        .opaque = backend,
        .preferred_decode_lead_us = 1000000u,
        .submit = swdec_submit,
        .drain = swdec_drain,
        .advance = swdec_advance,
        .stats = swdec_stats,
        .audio_cursor_us = swdec_audio_cursor,
        .set_presentation_clock_us = swdec_set_clock,
        .set_playing = swdec_set_playing,
        .set_buffering = swdec_set_buffering,
        .reset = swdec_reset,
        .destroy = swdec_destroy
    };
    return true;
}
