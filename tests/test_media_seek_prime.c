/* Seeking into a long GOP: the committed-seek prime must step from the
   keyframe the demuxer resumes at to the target, and a target beyond its
   bounded scan must fall back to resuming from that keyframe. */
#include <stdio.h>
#include <string.h>

#include "tilefinch/budget.h"
#include "tilefinch/media_backend.h"
#include "tilefinch/media_source.h"
#include "../src/media_backend_psp_policy.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

/* A muxed MP4-like source: 30 fps H.264 with a keyframe every `keyint`
   frames, and 1024-sample AAC frames at 44.1 kHz, interleaved by time. The
   sample table is computed, so only next_sample/seek do any work. */
#define VIDEO_TIMESCALE 30u
#define AUDIO_TIMESCALE 44100u
typedef struct {
    unsigned keyint;
    unsigned video_samples;
    unsigned audio_samples;
    unsigned next_video;
    unsigned next_audio;
    unsigned steps;          /* next_sample calls since the last seek */
    unsigned peak_steps;     /* the most between two seeks */
    unsigned seeks;
} LongGop;

static uint64_t video_us(unsigned i)
{
    return (uint64_t) i * 1000000u / VIDEO_TIMESCALE;
}

static uint64_t audio_us(unsigned i)
{
    return (uint64_t) i * 1024u * 1000000u / AUDIO_TIMESCALE;
}

/* video_samples == 0: an audio-only source whose seek cannot use an index
   (it restarts from the beginning), so a prime has to step to the target. */
static size_t gop_track_count(const void *opaque)
{
    const LongGop *gop = opaque;
    return gop->video_samples == 0 ? 1 : 2;
}

static bool gop_track_info(const void *opaque, size_t index,
                           MediaMp4TrackInfo *info)
{
    const LongGop *gop = opaque;
    bool audio_only = gop->video_samples == 0;
    if (index >= (audio_only ? 1u : 2u) || info == NULL) return false;
    memset(info, 0, sizeof(*info));
    bool video = index == 0 && !audio_only;
    info->kind = video ? MEDIA_MP4_TRACK_VIDEO : MEDIA_MP4_TRACK_AUDIO;
    info->timescale = video ? VIDEO_TIMESCALE : AUDIO_TIMESCALE;
    info->largest_sample = 64;
    return true;
}

static bool gop_next_sample(void *opaque, MediaMp4Sample *sample)
{
    LongGop *gop = opaque;
    bool video_left = gop->next_video < gop->video_samples;
    bool audio_left = gop->next_audio < gop->audio_samples;
    if (!video_left && !audio_left) return false;
    bool take_video = video_left
        && (!audio_left
            || video_us(gop->next_video) <= audio_us(gop->next_audio));
    memset(sample, 0, sizeof(*sample));
    sample->size = 16;
    if (take_video) {
        unsigned i = gop->next_video++;
        sample->track_index = 0;
        sample->kind = MEDIA_MP4_TRACK_VIDEO;
        sample->dts = i;
        sample->pts = i;
        sample->duration = 1;
        sample->timescale = VIDEO_TIMESCALE;
        sample->keyframe = i % gop->keyint == 0;
        sample->offset = i;
    } else {
        unsigned i = gop->next_audio++;
        sample->track_index = 1;
        sample->kind = MEDIA_MP4_TRACK_AUDIO;
        sample->dts = (uint64_t) i * 1024u;
        sample->pts = (int64_t) i * 1024;
        sample->duration = 1024;
        sample->timescale = AUDIO_TIMESCALE;
        sample->keyframe = true;
        sample->offset = 0x100000u + i;
    }
    if (++gop->steps > gop->peak_steps) gop->peak_steps = gop->steps;
    return true;
}

static bool gop_last_error(const void *opaque, char *error, size_t size)
{
    (void) opaque;
    (void) error;
    (void) size;
    return false;
}

static bool gop_would_block(const void *opaque)
{
    (void) opaque;
    return false;
}

static bool gop_resident(const void *opaque, const MediaMp4Sample *sample)
{
    (void) opaque;
    (void) sample;
    return true;
}

static bool gop_read(void *opaque, const MediaMp4Sample *sample,
                     void *destination, size_t capacity)
{
    (void) opaque;
    if (sample->size > capacity) return false;
    memset(destination, 0, sample->size);
    return true;
}

/* Like the MP4 demuxer: video resumes at the keyframe at or before the
   target, audio at the first frame not before that keyframe. */
static bool gop_seek(void *opaque, uint64_t target_us, uint64_t *actual_us)
{
    LongGop *gop = opaque;
    gop->seeks++;
    if (gop->video_samples == 0) {
        gop->next_audio = 0;
        gop->steps = 0;
        if (actual_us != NULL) *actual_us = 0;
        return true;
    }
    unsigned frame = (unsigned) (target_us * VIDEO_TIMESCALE / 1000000u);
    if (frame >= gop->video_samples) frame = gop->video_samples - 1u;
    unsigned key = frame - frame % gop->keyint;
    gop->next_video = key;
    gop->next_audio = 0;
    while (gop->next_audio < gop->audio_samples
           && audio_us(gop->next_audio) < video_us(key)) gop->next_audio++;
    gop->steps = 0;
    if (actual_us != NULL) *actual_us = video_us(key);
    return true;
}

static const MediaSampleSourceOps gop_ops = {
    .track_count = gop_track_count,
    .track_info = gop_track_info,
    .next_sample = gop_next_sample,
    .last_error = gop_last_error,
    .would_block = gop_would_block,
    .sample_resident = gop_resident,
    .read_sample_waiting = gop_read,
    .read_sample = gop_read,
    .seek_us = gop_seek,
    .seek_after_us = gop_seek
};

/* The first audio sample the backend was handed since the last reset. */
static uint64_t first_audio_us = UINT64_MAX;

static MediaBackendResult backend_submit(
    void *opaque, const MediaMp4Sample *sample, const unsigned char *payload,
    size_t length, char *error, size_t error_size)
{
    (void) opaque; (void) payload; (void) length;
    (void) error; (void) error_size;
    if (sample->kind == MEDIA_MP4_TRACK_AUDIO && first_audio_us == UINT64_MAX)
        first_audio_us = media_ticks_to_us(sample->dts, sample->timescale);
    return MEDIA_BACKEND_ACCEPTED;
}

static bool backend_advance(void *opaque, uint64_t clock_us,
                            char *error, size_t error_size)
{
    (void) opaque; (void) clock_us; (void) error; (void) error_size;
    return true;
}

static bool backend_reset(void *opaque, char *error, size_t error_size)
{
    (void) error; (void) error_size;
    first_audio_us = UINT64_MAX;
    (*(unsigned *) opaque)++;
    return true;
}

static void backend_destroy(void *opaque) { (void) opaque; }

int main(void)
{
    Budget budget;
    budget_init(&budget, 4u * 1024u * 1024u);
    unsigned resets = 0;
    MediaBackend backend = {
        .opaque = &resets, .submit = backend_submit,
        .advance = backend_advance, .reset = backend_reset,
        .destroy = backend_destroy
    };
    char error[256] = {0};

    puts("test: a muxed seek 6 s past a keyint-250 keyframe primes");
    /* 60 s of 30 fps video, keyframes every 250 frames (8.33 s). */
    LongGop gop = { .keyint = 250, .video_samples = 1800,
                    .audio_samples = 2584 };
    MediaSampleSource source = { .opaque = &gop, .ops = &gop_ops };
    MediaPlayback *playback = media_playback_create_sources(
        &budget, &source, NULL, &backend, NULL, error, sizeof(error));
    CHECK(playback != NULL);
    uint64_t keyframe_us = video_us(250);              /* 8,333,333 us */
    uint64_t target_us = keyframe_us + UINT64_C(6000000);
    uint64_t actual_us = 0;
    CHECK(media_playback_seek(playback, target_us, &actual_us,
                              error, sizeof(error)));
    CHECK(actual_us == keyframe_us);
    uint64_t primed_us = 0;
    CHECK(media_playback_prime_video_source(
              playback, target_us, NULL, error, sizeof(error))
          == MEDIA_PLAYBACK_SOURCE_PRIME_READY);
    MediaPlaybackSourcePrimeStatus audio = media_playback_prime_audio_source(
        playback, target_us, &primed_us, error, sizeof(error));
    if (audio != MEDIA_PLAYBACK_SOURCE_PRIME_READY)
        fprintf(stderr, "audio prime: %d %s\n", (int) audio, error);
    CHECK(audio == MEDIA_PLAYBACK_SOURCE_PRIME_READY);
    CHECK(primed_us == target_us);
    /* The shared source stays where the video prime left it: no scan ahead
       to the target and no second window (the firmware livelock). */
    CHECK(gop.peak_steps <= 1u);
    media_playback_destroy(playback);

    puts("test: a target beyond the bounded scan falls back to the keyframe");
    /* A separate audio source that can only restart from the beginning:
       a target 200 s in is far past what a prime may step over. */
    LongGop video_gop = { .keyint = 7200, .video_samples = 7200 };
    LongGop audio_gop = { .audio_samples = 10336 };
    MediaSampleSource audio_source = { .opaque = &audio_gop, .ops = &gop_ops };
    source.opaque = &video_gop;
    error[0] = '\0';
    playback = media_playback_create_sources(
        &budget, &source, &audio_source, &backend, NULL, error,
        sizeof(error));
    CHECK(playback != NULL);
    target_us = UINT64_C(200000000);
    CHECK(media_playback_seek(playback, target_us, &actual_us,
                              error, sizeof(error)));
    CHECK(actual_us == 0);
    CHECK(media_playback_prime_audio_source(
              playback, target_us, &primed_us, error, sizeof(error))
          == MEDIA_PLAYBACK_SOURCE_PRIME_FAILED);
    CHECK(strstr(error, "prime scan exceeded") != NULL);
    uint64_t resume_us = UINT64_MAX;
    CHECK(psp_media_seek_prime_fallback(false, target_us, actual_us,
                                        &resume_us));
    CHECK(resume_us == actual_us);
    /* What the session does next: seek to the keyframe and prime there. */
    CHECK(media_playback_seek(playback, resume_us, &actual_us,
                              error, sizeof(error)));
    CHECK(media_playback_prime_video_source(
              playback, resume_us, NULL, error, sizeof(error))
          == MEDIA_PLAYBACK_SOURCE_PRIME_READY
          && media_playback_prime_audio_source(
              playback, resume_us, &primed_us, error, sizeof(error))
          == MEDIA_PLAYBACK_SOURCE_PRIME_READY);
    CHECK(primed_us >= resume_us && primed_us - resume_us < 30000u);
    CHECK(audio_gop.peak_steps > 64u);    /* the scan crosses the old cap */
    media_playback_destroy(playback);

    puts("test: a keyframe seek resumes sound at the keyframe");
    gop = (LongGop) { .keyint = 250, .video_samples = 1800,
                      .audio_samples = 2584 };
    source.opaque = &gop;
    playback = media_playback_create_sources(
        &budget, &source, NULL, &backend, NULL, error, sizeof(error));
    CHECK(playback != NULL);
    target_us = keyframe_us + UINT64_C(6000000);
    /* Exact: audio begins at the target, 6 s of video ahead of it. */
    CHECK(media_playback_seek(playback, target_us, &actual_us,
                              error, sizeof(error)));
    media_playback_set_playing(playback, true);
    for (unsigned i = 0; i < 64u && first_audio_us == UINT64_MAX; i++)
        (void) media_playback_advance_bounded(
            playback, target_us + UINT64_C(1000000), 64,
            error, sizeof(error));
    CHECK(first_audio_us >= target_us);
    /* Keyframe: audio begins with the keyframe's picture. */
    CHECK(media_playback_seek_keyframe(playback, target_us, &actual_us,
                                       error, sizeof(error)));
    CHECK(actual_us == keyframe_us);
    for (unsigned i = 0; i < 64u && first_audio_us == UINT64_MAX; i++)
        (void) media_playback_advance_bounded(
            playback, actual_us + UINT64_C(1000000), 64,
            error, sizeof(error));
    CHECK(first_audio_us >= keyframe_us
          && first_audio_us < keyframe_us + UINT64_C(30000));
    media_playback_destroy(playback);
    CHECK(psp_media_seek_lands_on_keyframe(true, false, false));
    CHECK(!psp_media_seek_lands_on_keyframe(false, false, false));
    CHECK(!psp_media_seek_lands_on_keyframe(true, true, false));
    CHECK(!psp_media_seek_lands_on_keyframe(true, false, true));

    puts("test: the fallback is taken once and never at the keyframe");
    CHECK(!psp_media_seek_prime_fallback(true, target_us, 0, &resume_us));
    CHECK(!psp_media_seek_prime_fallback(false, 5000000u, 5000000u,
                                         &resume_us));
    CHECK(!psp_media_seek_prime_fallback(false, 5000000u, 6000000u,
                                         &resume_us));
    CHECK(resets == 5u);
    CHECK(budget.current == 0);
    puts("media seek prime tests passed");
    return 0;
}
