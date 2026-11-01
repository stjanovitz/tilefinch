#include "tilefinch/psp_media_scrub.h"

#define media_playback_set_playing test_backend_set_playing
#define psp_media_session_dispatch_event test_preview_dispatch
#include "../src/psp_media_session_internal.h"
#undef media_playback_set_playing
#undef psp_media_session_dispatch_event

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do {                                              \
    if (!(condition)) {                                                    \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n",                    \
                __FILE__, __LINE__, #condition);                           \
        return 1;                                                          \
    }                                                                      \
} while (0)

static bool backend_playing;
static unsigned backend_updates, preview_events;

void test_backend_set_playing(MediaPlayback *playback, bool playing)
{
    if (playback != NULL) {
        backend_playing = playing;
        backend_updates++;
    }
}

void test_preview_dispatch(PspMediaSession *media, PspMediaEvent event,
                          const char *checkpoint)
{
    (void) checkpoint;
    if (event.type == PSP_MEDIA_EVENT_PREVIEW_STARTED) {
        media->machine.preview_active = true;
        media->ui.playing = false;
        preview_events++;
    }
}

static int test_preview_pauses_retained_backend(void)
{
    static PspMediaSession media;
    int backend_token;
    for (unsigned playing = 0; playing < 2; playing++) {
        memset(&media, 0, sizeof(media));
        media.playback = (MediaPlayback *) &backend_token;
        media.clock_us = 5000000u;
        media.ui.visible = true;
        media.ui.duration_us = 60000000u;
        media.ui.playing = backend_playing = playing != 0;
        backend_updates = preview_events = 0;
        psp_media_begin_preview(&media, 15000000u, playing != 0);
        CHECK(!backend_playing && backend_updates == 1u);
        CHECK(!media.ui.playing && preview_events == 1u);
        CHECK(media.scrub.was_playing == (playing != 0));
        CHECK(media.job_restore_us == 5000000u);
        CHECK(media.ui.seek_preview_time_us == 15000000u);
        media.clock_us = 6000000u;
        psp_media_begin_preview(&media, 25000000u, false);
        CHECK(!backend_playing && preview_events == 1u);
        CHECK(media.scrub.was_playing == (playing != 0));
        CHECK(media.job_restore_us == 5000000u);
        CHECK(media.ui.seek_preview_time_us == 25000000u);
        media.playback = NULL;
        psp_media_begin_preview(&media, 30000000u, false);
        CHECK(media.ui.seek_preview_time_us == 30000000u);
    }
    return 0;
}

int main(void)
{
    CHECK(test_preview_pauses_retained_backend() == 0);
    /* A scrub starts once and keeps the playing state it began from. */
    PspMediaScrub scrub = {0};
    CHECK(!psp_media_scrub_active(&scrub));
    CHECK(!psp_media_scrub_resume_playing(&scrub));
    CHECK(psp_media_scrub_begin(&scrub, true));
    CHECK(psp_media_scrub_active(&scrub) && !psp_media_scrub_cancelling(&scrub));
    CHECK(!psp_media_scrub_begin(&scrub, false));
    CHECK(psp_media_scrub_resume_playing(&scrub));

    /* Circle during a job asks for a cancel; the scrub stays active until
       the restore carries it out. A cancel with no scrub does nothing. */
    psp_media_scrub_request_cancel(&scrub);
    CHECK(psp_media_scrub_active(&scrub) && psp_media_scrub_cancelling(&scrub));
    CHECK(psp_media_scrub_resume_playing(&scrub));
    psp_media_scrub_end(&scrub);
    CHECK(!psp_media_scrub_active(&scrub) && !scrub.was_playing);
    psp_media_scrub_request_cancel(&scrub);
    CHECK(!psp_media_scrub_active(&scrub));

    /* A failed thumbnail keeps the target: retain drops a pending cancel
       and keeps the original playing state. */
    CHECK(psp_media_scrub_begin(&scrub, false));
    psp_media_scrub_request_cancel(&scrub);
    psp_media_scrub_retain(&scrub, false);
    CHECK(psp_media_scrub_active(&scrub) && !psp_media_scrub_cancelling(&scrub));
    CHECK(!psp_media_scrub_resume_playing(&scrub));

    /* The pending target: a reopen re-highlights the interrupted scrub
       target, or clears it when there was none. */
    PspMediaPendingTarget pending = {0};
    psp_media_target_after_reopen(&pending, true, 90000000u);
    CHECK(psp_media_target_highlight_pending(&pending)
          && pending.target_us == 90000000u);
    psp_media_target_after_reopen(&pending, false, 5u);
    CHECK(pending.intent == PSP_MEDIA_TARGET_NONE && pending.target_us == 0);

    /* Cross supersedes a highlight, and a later reopen never demotes the
       commit back to a thumbnail. */
    psp_media_target_after_reopen(&pending, true, 7u);
    psp_media_target_commit(&pending, 120000000u, true);
    CHECK(psp_media_target_commit_pending(&pending)
          && !psp_media_target_highlight_pending(&pending)
          && pending.target_us == 120000000u && pending.resume_playing);
    psp_media_target_after_reopen(&pending, true, 3u);
    psp_media_target_after_reopen(&pending, false, 0u);
    CHECK(psp_media_target_commit_pending(&pending)
          && pending.target_us == 120000000u && pending.resume_playing);
    psp_media_target_clear(&pending);
    CHECK(pending.intent == PSP_MEDIA_TARGET_NONE && !pending.resume_playing);

    /* A highlight chosen during a reopen: shown, replaced by a later one,
       dropped by Circle; a commit already made is not demoted. */
    psp_media_target_highlight(&pending, 10000000u);
    CHECK(psp_media_target_highlight_pending(&pending)
          && pending.target_us == 10000000u);
    psp_media_target_highlight(&pending, 20000000u);
    CHECK(pending.target_us == 20000000u);
    psp_media_target_cancel_highlight(&pending);
    CHECK(pending.intent == PSP_MEDIA_TARGET_NONE);
    psp_media_target_commit(&pending, 30000000u, false);
    psp_media_target_highlight(&pending, 40000000u);
    psp_media_target_cancel_highlight(&pending);
    CHECK(psp_media_target_commit_pending(&pending)
          && pending.target_us == 30000000u);
    psp_media_target_clear(&pending);

    /* The continuation: nothing continues at first, and nothing asks the
       open to autoplay. */
    PspMediaContinuation continuation = {0};
    CHECK(!psp_media_continuation_reopening(&continuation)
          && !psp_media_continuation_resuming(&continuation)
          && !psp_media_continuation_resume_playing(&continuation));

    /* A rewind rebuilds the pipeline: it reopens toward 0 s, playing. URL
       reuse is opt-in per reopen. */
    continuation.reuse_resolved = true;
    psp_media_continuation_reopen(&continuation, 0u, true);
    CHECK(psp_media_continuation_reopening(&continuation)
          && psp_media_continuation_resume_playing(&continuation)
          && continuation.position_us == 0u && !continuation.reuse_resolved);

    /* Cross during the reopen moves where it resumes. */
    psp_media_continuation_retarget(&continuation, 10000000u, false);
    CHECK(continuation.position_us == 10000000u
          && !psp_media_continuation_resume_playing(&continuation));

    /* The open finishes and seeks there; the destination now belongs to the
       seek job, so the continuation no longer asks for autoplay and can't
       be retargeted. */
    psp_media_continuation_begin_resume(&continuation);
    CHECK(psp_media_continuation_resuming(&continuation)
          && !psp_media_continuation_reopening(&continuation)
          && !psp_media_continuation_resume_playing(&continuation));
    psp_media_continuation_retarget(&continuation, 5u, true);
    CHECK(continuation.position_us == 0u && !continuation.playing);
    psp_media_continuation_end_resume(&continuation);
    CHECK(continuation.phase == PSP_MEDIA_CONTINUATION_NONE);

    /* A retry after the resume seek failed re-enters REOPENING, and the
       failure path's end_resume must not undo that. */
    psp_media_continuation_begin_resume(&continuation);
    psp_media_continuation_reopen(&continuation, 7000000u, true);
    psp_media_continuation_end_resume(&continuation);
    CHECK(psp_media_continuation_reopening(&continuation)
          && continuation.position_us == 7000000u);

    /* Retargeting with nothing continuing does nothing. */
    psp_media_continuation_clear(&continuation);
    psp_media_continuation_retarget(&continuation, 9u, true);
    CHECK(continuation.phase == PSP_MEDIA_CONTINUATION_NONE
          && continuation.position_us == 0u && !continuation.playing);

    /* Every reopen with a known duration ends in the resume seek, including
       a paused one to exactly 0:00 (an ended clip rewound past the 30 s
       reopen threshold). That case used to settle as a fresh passive open,
       whose poster frame is never decoded: the device sat with zero packets
       and failed "VIDEO FIRST FRAME TIMED OUT" five seconds later. */
    const uint64_t duration_us = 60000000u;
    psp_media_continuation_reopen(&continuation, 0u, false);
    CHECK(psp_media_continuation_needs_resume_seek(
        &continuation, duration_us));
    psp_media_continuation_reopen(&continuation, 0u, true);
    CHECK(psp_media_continuation_needs_resume_seek(
        &continuation, duration_us));
    psp_media_continuation_reopen(&continuation, 45000000u, false);
    CHECK(psp_media_continuation_needs_resume_seek(
        &continuation, duration_us));
    /* No duration (nothing to seek within), or nothing continuing: none. */
    CHECK(!psp_media_continuation_needs_resume_seek(&continuation, 0u));
    psp_media_continuation_begin_resume(&continuation);
    CHECK(!psp_media_continuation_needs_resume_seek(
        &continuation, duration_us));
    psp_media_continuation_clear(&continuation);
    CHECK(!psp_media_continuation_needs_resume_seek(
        &continuation, duration_us));

    puts("psp-media-scrub-tests status=PASS");
    return 0;
}
