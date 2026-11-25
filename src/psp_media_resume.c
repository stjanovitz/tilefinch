#include "psp_media_session_internal.h"

static void psp_media_persist_profile_change(PspMediaSession *media)
{
    if (media == NULL) return;
    if (media->platform.profile_changed != NULL) {
        media->platform.profile_changed(
            media->platform.profile_context, psp_media_internal_now_us(media));
    } else if (media->profile_path != NULL) {
        /* Portable callers without the PSP profile-store seam retain the
           original immediate persistence contract. */
        (void) browser_profile_save(media->profile, media->profile_path);
    }
}

static bool psp_media_finish_resume_update(
    PspMediaSession *media, bool changed, bool persist)
{
    if (media == NULL) return false;
    bool dirty = changed || media->resume_profile_dirty;
    if (!persist) {
        /* Updating the bounded in-memory resume table must stay off the Memory
           Stick hot path, but a later close/suspend must still flush it even
           when the clock has not advanced since this update. */
        media->resume_profile_dirty = dirty;
        return dirty;
    }
    if (dirty) psp_media_persist_profile_change(media);
    media->resume_profile_dirty = false;
    return dirty;
}

uint64_t psp_media_duration_us(const PspMediaSession *media)
{
    if (media == NULL) return 0;
    return media->stream.duration_ms > UINT64_MAX / UINT64_C(1000)
        ? UINT64_MAX
        : media->stream.duration_ms * UINT64_C(1000);
}

void psp_media_note_resume_output(PspMediaSession *media, uint64_t pts_us)
{
    if (media->resume_output_blocked && pts_us == media->resume_output_us)
        return;
    media->resume_output_us = pts_us;
    media->resume_output_observed = true;
    media->resume_output_blocked = false;
}

void psp_media_capture_resume_audio(PspMediaSession *media)
{
    uint64_t audio_us;
    if (media->audio_only
        && media_playback_audio_cursor_us(media->playback, &audio_us))
        psp_media_note_resume_output(media, audio_us);
}

void psp_media_reset_resume_output(PspMediaSession *media)
{
    media->last_resume_saved_us = 0;
    media->resume_output_us = 0;
    media->resume_output_observed = false;
    media->resume_output_blocked = false;
}

void psp_media_clear_resume_evidence(PspMediaSession *media)
{
    psp_media_capture_resume_audio(media);
    if (media->resume_output_observed) {
        /* The same paused cursor or a redraw of the same picture is not
           new playback. Allow the next distinct output to record again. */
        media->last_resume_saved_us = media->resume_output_us;
        media->resume_output_observed = false;
        media->resume_output_blocked = true;
    }
    media->resume_profile_dirty = false;
}

bool psp_media_record_resume(PspMediaSession *media, bool persist)
{
    if (media == NULL || media->profile == NULL
        || !browser_profile_save_playback_positions(media->profile))
        return false;
    char video_id[YOUTUBE_VIDEO_ID_CAPACITY] = {0};
    uint64_t duration_us = psp_media_duration_us(media);
    psp_media_capture_resume_audio(media);
    /* Resolvers, seek jobs and recovery may project a new clock long before
       any output reaches the viewer. Retain the last observed PTS for this
       route even if the decoder carrying it has already been retired. */
    if (!media->resume_output_observed || duration_us == 0
        || !youtube_watch_url_video_id(media->source, video_id))
        return psp_media_finish_resume_update(media, false, persist);
    uint64_t observed_us = media->resume_output_us;
    /* Only a played rewind or ending clears the saved position. */
    bool finished = duration_us <= UINT64_C(5000000)
        || observed_us >= duration_us - UINT64_C(5000000);
    uint64_t position = finished || observed_us < UINT64_C(5000000)
        ? 0 : observed_us;
    bool changed = browser_profile_record_resume(
        media->profile, video_id, position, duration_us);
    media->last_resume_saved_us = observed_us;
    return psp_media_finish_resume_update(media, changed, persist);
}
