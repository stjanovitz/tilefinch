#include "tilefinch/psp_media_scrub.h"

#include <stdio.h>

#define CHECK(condition) do {                                              \
    if (!(condition)) {                                                    \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n",                    \
                __FILE__, __LINE__, #condition);                           \
        return 1;                                                          \
    }                                                                      \
} while (0)

int main(void)
{
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

    puts("psp-media-scrub-tests status=PASS");
    return 0;
}
