#include "psp_media_session_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "media_backend_psp_policy.h"
#include "psp_media_pixels.h"
#include "tilefinch/psp_log.h"

#define printf psp_log_printf
#define PSP_MEDIA_PREVIEW_WIDTH 128
#define PSP_MEDIA_PREVIEW_HEIGHT 72

static bool psp_media_reopen_backward_seek(
    PspMediaSession *media, uint64_t target_us)
{
    bool resume_playing = media->ui.playing || media->job_resume_playing;
    uint64_t from_us = media->clock_us;
    bool reuse_resolved_stream =
        psp_media_resolved_stream_reusable(media);
    psp_media_session_dispatch_event(media, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_CLOSE,
        .retain_pipeline = false
    }, "backward-seek-reopen-close");
    /* Keep the HTTP ranges and MP4 identity only conceptually: the ordinary
       open service re-resolves their expiring URLs, while destroy provides
       the proven firmware ordering (codec, audio DMA, EDRAM, MPEG). This is a
       rare >=30 s rewind, so a bounded rebuffer is preferable to carrying a
       reused AAC program across the discontinuity. */
    psp_media_pipeline_destroy(media);
    psp_media_finish_synchronous_quiesce(
        media, "backward-seek-reopen-released");
    if (media_psp_backend_quarantined()) {
        psp_media_job_failed(
            media, "backward seek",
            "VIDEO DECODER NEEDS APP RESTART");
        return true;
    }
    media->clock_us = target_us;
    media->reopen_resume_us = target_us;
    media->reopen_resume_playing = resume_playing;
    media->reopen_resume_pending = true;
    media->reopen_reuse_resolved_stream = reuse_resolved_stream;
    media->reopen_seek_completion_pending = true;
    media->open_service_pending = true;
    psp_media_session_dispatch_event(media, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_OPEN,
        .autoplay = resume_playing,
        .has_separate_audio = false
    }, "backward-seek-reopen-open");
    psp_ui_media_set_resolving(&media->ui, "Seeking video");
    psp_ui_media_set_resolving_progress(
        &media->ui, "Restarting decoder", 20u);
    printf(
        "tilefinch-media-job: backward-seek-reopen from=%lluus "
        "target=%lluus playing=%d reuse-resolved=%d threshold=%lluus\n",
        (unsigned long long) from_us,
        (unsigned long long) target_us, resume_playing ? 1 : 0,
        reuse_resolved_stream ? 1 : 0,
        (unsigned long long) PSP_MEDIA_BACKWARD_REOPEN_US);
    return true;
}

bool psp_media_request_seek(PspMediaSession *media,
                                   uint64_t target_us, bool preview)
{
    if (media == NULL || media->demux == NULL) return false;
    if (psp_media_seek_reopens_backend(
            media->clock_us, target_us, preview)) {
        if (media->last_resume_saved_us == UINT64_MAX)
            media->last_resume_saved_us = 0;
        return psp_media_reopen_backward_seek(media, target_us);
    }
    psp_media_session_dispatch_event(media, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_SEEK
    }, preview ? "preview-seek-request" : "seek-request");
    if (preview) {
        psp_media_session_dispatch_event(media, (PspMediaEvent) {
            .type = PSP_MEDIA_EVENT_PREVIEW_STARTED
        }, "preview-started");
    } else if (media->machine.preview_active) {
        psp_media_session_dispatch_event(media, (PspMediaEvent) {
            .type = PSP_MEDIA_EVENT_PREVIEW_ENDED
        }, "preview-committed");
    }
    psp_media_buffering_end(media, psp_media_internal_now_us(media));
    if (!preview && media->last_resume_saved_us == UINT64_MAX) {
        /* Replaying an ended stream is a new resume-recording interval. The
           terminal sentinel belongs to the completed pass, not to the retained
           decoder's entire lifetime. */
        media->last_resume_saved_us = 0;
    }
    if (preview && !media->seek_preview_started) {
        media->seek_preview_started = true;
        media->seek_preview_was_playing = media->ui.playing;
        media->seek_preview_cancel_pending = false;
        media->job_restore_us = media->clock_us;
    }
    media->job_target_us = target_us;
    psp_media_release_presentation_preroll(media, true);
    media->job_preview = preview;
    media->job_resume_playing = preview
        ? false
        : (media->seek_preview_started
            ? media->seek_preview_was_playing : media->ui.playing);
    media->ui.playing = false;
    if (media->playback != NULL)
        media_playback_set_playing(media->playback, false);
    media->job_phase = PSP_MEDIA_JOB_SEEK_PREPARE;
    media->job_started_us = psp_media_internal_now_us(media);
    media->job_phase_started_us = 0;
    media->job_units = 0;
    media->job_maximum_unit_us = 0;
    printf("tilefinch-media-job: seek-request preview=%d target=%lluus "
           "resume=%d preview-started=%d phase=%d\n",
           preview ? 1 : 0, (unsigned long long) target_us,
           media->job_resume_playing ? 1 : 0,
           media->seek_preview_started ? 1 : 0,
           (int) media->job_phase);
    psp_media_session_checkpoint(media, "seek-request");
    return true;
}

static bool psp_media_copy_preview(PspMediaSession *media)
{
    if (media == NULL || !media->have_frame
        || media->frame.pixels == NULL
        || media->frame.width <= 0 || media->frame.height <= 0
        || media->frame.stride_pixels < media->frame.width) return false;
    /*
     * A reader like any other. This one runs on the browser thread at a seek,
     * which is the moment the pipeline is most likely to be moving underneath
     * it, so the refusal below is not theoretical: a preview copied out of a
     * slot the reset had already recycled would be a thumbnail of the wrong
     * picture, kept and shown for the whole of the scrub.
     */
    if (media->frame.slot < 0
        || !media_psp_backend_borrow_surface(
               (unsigned) media->frame.slot, media->frame.generation)) {
        /*
         * The picture this session is holding is no longer in that slot.
         *
         * Reachable now that the staged path hands a slot back at its copy: a
         * scrub whose decode produced nothing new re-reads the frame it
         * already had, and that frame's slot may have been converted into
         * since. Copying anyway would put a different picture under this
         * scrub position -- the substitution the whole design exists to
         * prevent -- so the thumbnail simply stays as it was, which is what a
         * user dragging through a seek bar sees between updates regardless.
         * Only a scrub that has never managed one is a real failure.
         */
        return media->seek_preview_pixels != NULL;
    }
    if (media->seek_preview_pixels == NULL) {
        media->seek_preview_pixels = budget_calloc_category(
            media->budget, BUDGET_CATEGORY_RESOURCE,
            PSP_MEDIA_PREVIEW_WIDTH * PSP_MEDIA_PREVIEW_HEIGHT,
            sizeof(*media->seek_preview_pixels));
        if (media->seek_preview_pixels == NULL) {
            media_psp_backend_end_surface_read(
                (unsigned) media->frame.slot);
            return false;
        }
    }
    for (int y = 0; y < PSP_MEDIA_PREVIEW_HEIGHT; y++) {
        int source_y = y * media->frame.height / PSP_MEDIA_PREVIEW_HEIGHT;
        uint16_t *destination = media->seek_preview_pixels
            + (size_t) y * PSP_MEDIA_PREVIEW_WIDTH;
        if (media->frame.format == MEDIA_PIXEL_RGB565) {
            const uint16_t *source = media->frame.pixels;
            source += (size_t) source_y * media->frame.stride_pixels;
            for (int x = 0; x < PSP_MEDIA_PREVIEW_WIDTH; x++)
                destination[x] = source[
                    x * media->frame.width / PSP_MEDIA_PREVIEW_WIDTH];
        } else {
            const unsigned char *source = media->frame.pixels;
            source += (size_t) source_y
                * media->frame.stride_pixels * 4u;
            for (int x = 0; x < PSP_MEDIA_PREVIEW_WIDTH; x++) {
                destination[x] = psp_media_rgba565(
                    source + (size_t) (
                        x * media->frame.width / PSP_MEDIA_PREVIEW_WIDTH)
                        * 4u);
            }
        }
    }
    media_psp_backend_end_surface_read((unsigned) media->frame.slot);
    return true;
}

void psp_media_cancel_decode(PspMediaSession *media)
{
    if (media == NULL) return;
    psp_media_buffering_end(media, psp_media_internal_now_us(media));
    psp_media_release_presentation_preroll(media, true);
    if (media->playback != NULL)
        media_playback_set_playing(media->playback, false);
    media->ui.playing = false;
    media->decode_job_pending = false;
    media->pause_boundary_pending = false;
    media->first_frame_started_us = 0;
    media->first_frame_opened_us = 0;
    media->first_frame_pump_us = 0;
    media->job_phase = PSP_MEDIA_JOB_NONE;
    media->job_preview = false;
    media->job_resume_playing = false;
    media->job_resume_open = false;
    media->job_started_us = 0;
    media->job_phase_started_us = 0;
    media->job_units = 0;
    media->job_maximum_unit_us = 0;
    media->seek_preview_started = false;
    media->seek_preview_was_playing = false;
    media->seek_preview_cancel_pending = false;
    psp_ui_media_cancel_seek_preview(&media->ui);
    psp_ui_media_set_buffering(
        &media->ui, false,
        media->playback == NULL ? 0
            : media_playback_buffered_until_us(media->playback));
    printf("tilefinch-media-job: decoder cancellation accepted\n");
}

void psp_media_interrupt_decode(PspMediaSession *media)
{
    if (media == NULL) return;
    /* Cancellation can arrive after a seek reset or one or more native
       packet submissions. Preserve the transaction's user-visible recovery
       position before cancel_decode clears its phase, and forbid close from
       retaining that partially advanced native pipeline as a replay cache. */
    media->clock_us = psp_media_recovery_position_us(media);
    media->recovery_service_active = true;
    psp_media_cancel_decode(media);
}

bool psp_media_seek_decode_pump(
    PspMediaSession *media,
    const TilefinchCancellation *cancellation)
{
    if (media == NULL
        || (media->job_phase != PSP_MEDIA_JOB_SEEK_PREPARE
            && media->job_phase != PSP_MEDIA_JOB_SEEK_DECODE
            && media->job_phase
                 != PSP_MEDIA_JOB_PREVIEW_RESTORE_PREPARE
            && media->job_phase
                 != PSP_MEDIA_JOB_PREVIEW_RESTORE_DECODE)) return false;
    /*
     * The seek's transport step, and the reason a resume seek deadlocked.
     *
     * A read that misses its window pumps the scheduler once on its way to
     * WOULD_BLOCK, so a loop that reads every iteration keeps its own transfer
     * moving. A seek does not read every iteration: the eligibility horizon
     * turns most of them away before the demuxer is asked for anything, and
     * the device cycle counted 5,261 horizon exits against 6 source-blocked
     * reads. For those 5,261 iterations nothing in this job touched the
     * transport at all, so the window the seek had already asked for sat
     * un-advanced until the five-second deadline killed the job -- with the
     * whole 256 KiB eventually received and never installed
     * (inflight=262144 window-pending=1, suspect=source-window).
     *
     * psp_media_advance pumps both ranges every frame before it advances
     * playback, and returns here before reaching that call. Take the same
     * bounded step, on the same quota, so a pending window completes during a
     * seek exactly as it does during playback. Every other loop that waits on
     * a window either does this or delegates to a blocking read that owns its
     * own bounded pump; this was the one that did neither.
     */
    psp_media_pump_ranges(media);
    uint64_t unit_started =
        psp_media_internal_now_us(media);
    char error[256] = {0};
    bool restore =
        media->job_phase == PSP_MEDIA_JOB_PREVIEW_RESTORE_PREPARE
        || media->job_phase == PSP_MEDIA_JOB_PREVIEW_RESTORE_DECODE;
    bool cancelled_preview = restore
        && media->seek_preview_cancel_pending;
    uint64_t target_us =
        restore ? media->job_restore_us : media->job_target_us;
    if (media->job_phase == PSP_MEDIA_JOB_SEEK_PREPARE
        || media->job_phase
             == PSP_MEDIA_JOB_PREVIEW_RESTORE_PREPARE) {
        media->have_frame = false;
        memset(&media->frame, 0, sizeof(media->frame));
        uint64_t actual_us = 0;
        if (!media_playback_seek(
                media->playback, target_us, &actual_us,
                error, sizeof(error))) {
            if (psp_media_retry_delivery_failure(
                    media, "seek-reset", error, false)) return true;
            if (psp_media_retry_240p(
                    media, "seek-reset", error, false)) return true;
            psp_media_job_failed(
                media, "seek",
                error[0] == '\0' ? "media seek failed" : error);
            return true;
        }
        /*
         * The decode leg, retired for every seek that owes nobody a picture at
         * an exact timestamp.
         *
         * A scrub preview owes one: it is a still, and the still is the whole
         * point, so a preview capture and the restore that follows it keep
         * decoding forward from the keyframe. Nothing else does. An open's
         * resume owes playback from where the viewer left off; a confirmed
         * seek owes playback from where they scrubbed to -- and the reset
         * above has already put the demuxer there, one keyframe early, which
         * is where playback would have started anyway.
         *
         * Decoding forward to the exact frame first cost the device a
         * five-second timeout on every resume (22.5s target, cold window), and
         * the two-minute stability soak died outright on the far forward seek
         * it makes at fifteen seconds: `event=seek direction=forward
         * target=168174000us` then `reason="SEEK FRAME UNAVAILABLE"`, the leg
         * reporting a caught-up pipeline before one picture existed. A seek
         * that cannot fail is worth more than a seek that lands on the exact
         * requested frame: after the reconcile below, ordinary playback
         * reaches that frame within a keyframe interval, with its own
         * watchdogs, buffering surface and clock resync doing the work.
         *
         * The clock adopts the position the demuxer now holds, which is the
         * same consistent pair psp_media_seek_failure_clock_us restores after
         * a failure.
         */
        bool decodes_to_the_target = media->job_preview || restore;
        printf("tilefinch-media-job: seek-prepared preview=%d restore=%d "
               "decode-target=%d target=%lluus actual=%lluus resume=%d\n",
               media->job_preview ? 1 : 0, restore ? 1 : 0,
               decodes_to_the_target ? 1 : 0,
               (unsigned long long) target_us,
               (unsigned long long) actual_us,
               media->job_resume_playing ? 1 : 0);
        if (!decodes_to_the_target) {
            bool resume_open = media->job_resume_open;
            media->job_resume_open = false;
            /*
             * Pay the video connection's handshake here, not on the first
             * playing frame. A long think-time between open and the play press
             * closes the connection the open warmed, so the first video window
             * fetch re-handshakes -- a device cycle measured that at 497ms,
             * landing as one 484ms playing hitch, while audio (whose window was
             * already in flight) stayed at zero. Warm video the same way,
             * under this job's wait budget and its loading UI, so the fetch the
             * first playing frame makes hits a warm connection and a cached
             * window. Best effort: if it would block or the sample is too
             * large, playback makes the connection as it did before -- which
             * is also what makes this safe on a seek, where the budget is the
             * five seconds the decode leg used to spend failing.
             */
            if (resume_open) psp_media_open_arm_wait_budget(media);
            else psp_media_seek_arm_wait_budget(media);
            bool video_warmed = media_playback_warm_video(
                media->playback, target_us, error, sizeof(error));
            /* And audio, at the position the reset left it -- the keyframe,
               not the request. Warming only video left the audio window to be
               fetched at a far offset on a connection with nothing left to
               burst with: 16 KiB in six seconds, video held back to keep the
               interleave, and the session gone. Audio is a tenth of the
               bitrate and the first thing playback runs out of. */
            bool audio_warmed = media_playback_warm_audio(
                media->playback, actual_us, error, sizeof(error));
            psp_media_open_clear_wait_budget(media);
            media->clock_us = target_us;
            media->presentation_floor_us = target_us;
            media->presentation_preroll_startup = false;
            media->job_phase = PSP_MEDIA_JOB_NONE;
            media->job_preview = false;
            media->seek_preview_started = false;
            bool playing = media->job_resume_playing;
            media->ui.playing = playing;
            media_playback_set_playing(media->playback, playing);
            psp_ui_media_set(
                &media->ui, true, playing, false, target_us,
                psp_media_duration_us(media), media->stream.title);
            if (!resume_open || media->reopen_seek_completion_pending) {
                media->seek_completions++;
                media->reopen_seek_completion_pending = false;
            }
            printf("tilefinch-media-job: %s at=%lluus actual=%lluus "
                   "playing=%d decode-leg=skipped video-warmed=%d "
                   "audio-warmed=%d audio-held=%d elapsed=%lluus\n",
                   resume_open ? "resume-open" : "seek-complete",
                   (unsigned long long) target_us,
                   (unsigned long long) actual_us, playing ? 1 : 0,
                   video_warmed ? 1 : 0, audio_warmed ? 1 : 0,
                   media->presentation_preroll_audio_held ? 1 : 0,
                   (unsigned long long) (
                       psp_media_internal_now_us(media) - media->job_started_us));
            media->job_started_us = 0;
            media->job_units = 0;
            media->job_maximum_unit_us = 0;
            return true;
        }
        /*
         * A preview is paused from the UI's point of view, but it still has
         * to feed the decoder until a target picture exists.  The generic
         * playback pump deliberately does no demux work while `playing` is
         * false; leaving the backend paused here therefore made the first
         * preview pump report COMPLETE with no frame and fail immediately as
         * SEEK FRAME UNAVAILABLE.  Run only the decode pipeline, with split
         * audio held so seeking cannot leak sound.  Completion below restores
         * the user's real play/pause state and the normal post-seek preroll
         * releases audio at a synchronized boundary.
         */
        media->presentation_preroll_startup = false;
        media_playback_set_playing(media->playback, true);
        media->job_phase = restore
            ? PSP_MEDIA_JOB_PREVIEW_RESTORE_DECODE
            : PSP_MEDIA_JOB_SEEK_DECODE;
    } else {
        /*
         * The seek horizon has to move, or the seek cannot finish.
         *
         * Passing the target as the clock makes the eligibility horizon a
         * fixed target+lead for the whole job: the pump submits that much
         * content once and then reports "caught up" forever, however long the
         * decoder needs to reach a picture at the target. Device cycle
         * realclock shows it exactly -- a resume seek submitted 41 packets,
         * almost precisely one decode lead of content, then took 5,061
         * horizon exits in five seconds and died on the seek deadline.
         *
         * Advance it with the samples already accepted instead. Each pump then
         * admits another lead beyond what is buffered, so the seek always has
         * somewhere to go, while the job's own deadline and the backend's
         * WOULD_BLOCK remain the bounds -- unit boundedness is untouched.
         */
        uint64_t buffered_us =
            media_playback_buffered_until_us(media->playback);
        uint64_t seek_clock_us =
            buffered_us > target_us ? buffered_us : target_us;
        MediaPlaybackAdvanceResult result =
            media_playback_advance_bounded_cancelable(
                media->playback, seek_clock_us,
                PSP_MEDIA_JOB_MAXIMUM_PACKETS,
                cancellation,
                error, sizeof(error));
        if (result == MEDIA_PLAYBACK_ADVANCE_CANCELLED) {
            psp_media_interrupt_decode(media);
            return true;
        }
        if (result == MEDIA_PLAYBACK_ADVANCE_ERROR) {
            if (psp_media_retry_delivery_failure(
                    media, "seek-decode", error, false)) return true;
            if (psp_media_retry_240p(
                    media, "seek-decode", error, false)) return true;
            psp_media_job_failed(media, "seek decode", error);
            return true;
        }
        MediaVideoFrame frame = {0};
        /* The public clock is intentionally frozen while the seek preview is
           built.  Give the backend the preview target for due-frame gating;
           otherwise READY slots just beyond the old playback position remain
           "early", fill both surfaces, and the decoder can never reach the
           requested picture. */
        media_playback_set_presentation_clock_us(
            media->playback, target_us);
        if (media_playback_take_video_frame(
                media->playback, &frame)) {
            media->frame = frame;
        }
        MediaVideoSeekDecision decision = media_video_seek_decide(
            &media->frame, target_us,
            result == MEDIA_PLAYBACK_ADVANCE_PENDING
                || !media_playback_ended(media->playback));
        if (decision == MEDIA_VIDEO_SEEK_WAIT) {
            uint64_t now_us = psp_media_internal_now_us(media);
            if (psp_media_deadline_reached(
                    media->job_started_us, now_us,
                    PSP_MEDIA_SEEK_TIMEOUT_US)) {
                if (psp_media_retry_240p(
                        media, "seek-timeout",
                        "VIDEO SEEK TIMED OUT", false)) return true;
                psp_media_job_failed(
                    media, "seek decode", "VIDEO SEEK TIMED OUT");
            }
            return true;
        }
        if (decision == MEDIA_VIDEO_SEEK_UNAVAILABLE) {
            psp_media_job_failed(
                media, "seek decode", "SEEK FRAME UNAVAILABLE");
            return true;
        }
        media->have_frame = true;
        if (decision == MEDIA_VIDEO_SEEK_FINAL_FALLBACK)
            printf("tilefinch-media-job: seek used final frame\n");
        media->clock_us = target_us;
        if (media->job_preview && !restore) {
            if (!media->have_frame || !psp_media_copy_preview(media)) {
                psp_media_job_failed(
                    media, "seek preview",
                    "could not produce bounded preview frame");
                return true;
            }
            /* Direction input can move the highlighted target while this
               bounded preview is decoding. Never overwrite that newer UI
               target with the picture which just completed; the session
               will coalesce directly to the latest request after restore. */
            if (!media->ui.seek_preview_active
                || media->ui.seek_preview_time_us
                       == media->job_target_us) {
                psp_ui_media_set_seek_preview(
                    &media->ui, media->job_target_us);
            }
            media->job_phase =
                PSP_MEDIA_JOB_PREVIEW_RESTORE_PREPARE;
        } else {
            bool playing = restore
                ? (cancelled_preview
                    && media->seek_preview_was_playing)
                : media->job_resume_playing;
            media->ui.playing = playing;
            media_playback_set_playing(media->playback, playing);
            psp_ui_media_set(
                &media->ui, true, playing, false,
                target_us,
                psp_media_duration_us(media),
                media->stream.title);
            media->job_phase = PSP_MEDIA_JOB_NONE;
            if (!restore) media->seek_preview_started = false;
            if (cancelled_preview) {
                media->seek_preview_started = false;
                media->seek_preview_cancel_pending = false;
                psp_ui_media_cancel_seek_preview(&media->ui);
                psp_media_session_dispatch_event(media, (PspMediaEvent) {
                    .type = PSP_MEDIA_EVENT_PREVIEW_ENDED
                }, "preview-cancelled");
            }
            if (!restore) media->seek_completions++;
            printf("tilefinch-media-job: seek-complete preview=%d "
                   "units=%zu max-unit=%lluus elapsed=%lluus\n",
                   media->job_preview ? 1 : 0, media->job_units,
                   (unsigned long long) media->job_maximum_unit_us,
                   (unsigned long long) (
                       psp_media_internal_now_us(media)
                       - media->job_started_us));
            media->job_started_us = 0;
            media->job_preview = false;
        }
    }
    uint64_t unit_us =
        psp_media_internal_now_us(media) - unit_started;
    media->job_units++;
    if (unit_us > media->job_maximum_unit_us)
        media->job_maximum_unit_us = unit_us;
    if (media->job_phase == PSP_MEDIA_JOB_NONE
        && media->seek_preview_started) {
        media->seek_preview_started = false;
    }
    if (media->job_phase == PSP_MEDIA_JOB_NONE
        && !media->job_preview) {
        media->job_units = 0;
        media->job_maximum_unit_us = 0;
    }
    if (media->job_phase == PSP_MEDIA_JOB_NONE
        && restore && !cancelled_preview) {
        /* The preview remains visible in the UI while playback has been
           restored to its original position and remains paused. */
        media->seek_preview_started = true;
    }
    if (media->job_phase == PSP_MEDIA_JOB_NONE
        && restore && media->playback == NULL) {
        media->seek_preview_started = false;
        psp_ui_media_cancel_seek_preview(&media->ui);
        psp_media_session_dispatch_event(media, (PspMediaEvent) {
            .type = PSP_MEDIA_EVENT_PREVIEW_ENDED
        }, "preview-restore-failed");
        return false;
    }
    return true;
}
