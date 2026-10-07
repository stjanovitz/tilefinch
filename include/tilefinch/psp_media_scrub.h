#ifndef TILEFINCH_PSP_MEDIA_SCRUB_H
#define TILEFINCH_PSP_MEDIA_SCRUB_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Two small machines beside the player's lifecycle reducer
 * (psp_media_state.h). Neither is lifecycle state: they hold what the user
 * asked of the timeline, which has to survive the pipeline rebuilds that the
 * reducer drives (a quality or transport retry, a backward-seek reopen).
 *
 * The scrub is a tentative seek. Scrubbing moves only the highlighted time;
 * playback holds where the scrub began and remembers whether it was playing.
 * Circle while a decode transaction is in flight asks for a cancel, which
 * the next restore carries out instead of tearing into firmware work.
 *
 * The pending target is what becomes of a highlighted time at the next safe
 * point. HIGHLIGHT re-shows a scrub target that a reopen interrupted (or one
 * chosen while it ran); COMMIT is Cross pressed while a job was in flight.
 * They are exclusive, and a commit is never demoted back to a highlight.
 *
 * The continuation is a playback that outlives its pipeline: a backward-seek
 * reopen, a quality/transport/user retry, a track switch or a system resume
 * tears the backend down and must come back at a position, playing or not.
 * REOPENING carries that destination through the open; RESUMING is the seek
 * the finished open runs to reach it. The timeline is the same video
 * throughout, so it keeps accepting input (psp_media_session.c).
 */

typedef enum {
    PSP_MEDIA_SCRUB_NONE = 0,
    PSP_MEDIA_SCRUB_ACTIVE,
    PSP_MEDIA_SCRUB_CANCELLING
} PspMediaScrubPhase;

typedef struct {
    PspMediaScrubPhase phase;
    /* Meaningful only while a scrub is active. */
    bool was_playing;
} PspMediaScrub;

static inline bool psp_media_scrub_active(const PspMediaScrub *scrub)
{
    return scrub->phase != PSP_MEDIA_SCRUB_NONE;
}

static inline bool psp_media_scrub_cancelling(const PspMediaScrub *scrub)
{
    return scrub->phase == PSP_MEDIA_SCRUB_CANCELLING;
}

/* Whether playback resumes when the scrub ends without a commit. */
static inline bool psp_media_scrub_resume_playing(const PspMediaScrub *scrub)
{
    return psp_media_scrub_active(scrub) && scrub->was_playing;
}

/* A scrub starts once; further movement keeps its origin. True when this
   call started it. */
static inline bool psp_media_scrub_begin(PspMediaScrub *scrub,
                                         bool was_playing)
{
    if (psp_media_scrub_active(scrub)) return false;
    scrub->phase = PSP_MEDIA_SCRUB_ACTIVE;
    scrub->was_playing = was_playing;
    return true;
}

/* A failed thumbnail keeps the target highlighted: the scrub stays active
   (any cancel request is dropped) with its original playing state. */
static inline void psp_media_scrub_retain(PspMediaScrub *scrub,
                                          bool was_playing)
{
    scrub->phase = PSP_MEDIA_SCRUB_ACTIVE;
    scrub->was_playing = was_playing;
}

static inline void psp_media_scrub_request_cancel(PspMediaScrub *scrub)
{
    if (psp_media_scrub_active(scrub))
        scrub->phase = PSP_MEDIA_SCRUB_CANCELLING;
}

static inline void psp_media_scrub_end(PspMediaScrub *scrub)
{
    scrub->phase = PSP_MEDIA_SCRUB_NONE;
    scrub->was_playing = false;
}

typedef enum {
    PSP_MEDIA_TARGET_NONE = 0,
    PSP_MEDIA_TARGET_HIGHLIGHT,
    PSP_MEDIA_TARGET_COMMIT
} PspMediaTargetIntent;

typedef struct {
    PspMediaTargetIntent intent;
    uint64_t target_us;
    /* COMMIT only: the playing state after the committed seek. */
    bool resume_playing;
} PspMediaPendingTarget;

static inline bool psp_media_target_highlight_pending(
    const PspMediaPendingTarget *pending)
{
    return pending->intent == PSP_MEDIA_TARGET_HIGHLIGHT;
}

static inline bool psp_media_target_commit_pending(
    const PspMediaPendingTarget *pending)
{
    return pending->intent == PSP_MEDIA_TARGET_COMMIT;
}

static inline void psp_media_target_clear(PspMediaPendingTarget *pending)
{
    pending->intent = PSP_MEDIA_TARGET_NONE;
    pending->target_us = 0;
    pending->resume_playing = false;
}

/* Cross during a job: the latest committed time wins over anything else. */
static inline void psp_media_target_commit(PspMediaPendingTarget *pending,
                                           uint64_t target_us,
                                           bool resume_playing)
{
    pending->intent = PSP_MEDIA_TARGET_COMMIT;
    pending->target_us = target_us;
    pending->resume_playing = resume_playing;
}

/* A direction pressed while a reopen or its resume seek runs: shown now and
   previewed once the pipeline can decode, unless Cross already committed a
   time, which stands. */
static inline void psp_media_target_highlight(
    PspMediaPendingTarget *pending, uint64_t target_us)
{
    if (psp_media_target_commit_pending(pending)) return;
    pending->intent = PSP_MEDIA_TARGET_HIGHLIGHT;
    pending->target_us = target_us;
    pending->resume_playing = false;
}

/* Circle over a highlight chosen during a reopen. */
static inline void psp_media_target_cancel_highlight(
    PspMediaPendingTarget *pending)
{
    if (psp_media_target_highlight_pending(pending))
        psp_media_target_clear(pending);
}

/* A reopen re-highlights the scrub target it interrupted (or nothing), but
   a commit already made stands. */
static inline void psp_media_target_after_reopen(
    PspMediaPendingTarget *pending, bool highlight, uint64_t target_us)
{
    if (psp_media_target_commit_pending(pending)) return;
    if (!highlight) {
        psp_media_target_clear(pending);
        return;
    }
    pending->intent = PSP_MEDIA_TARGET_HIGHLIGHT;
    pending->target_us = target_us;
    pending->resume_playing = false;
}

typedef enum {
    PSP_MEDIA_CONTINUATION_NONE = 0,
    PSP_MEDIA_CONTINUATION_REOPENING,
    PSP_MEDIA_CONTINUATION_RESUMING
} PspMediaContinuationPhase;

typedef struct {
    PspMediaContinuationPhase phase;
    /* REOPENING only: where the replacement pipeline resumes, and whether it
       plays there. RESUMING hands both to the seek job it started. */
    uint64_t position_us;
    bool playing;
    /* REOPENING only: a large rewind may reopen from the already-authorized
       direct URLs while they remain valid. Consumed by the open's resolve. */
    bool reuse_resolved;
} PspMediaContinuation;

static inline bool psp_media_continuation_reopening(
    const PspMediaContinuation *continuation)
{
    return continuation->phase == PSP_MEDIA_CONTINUATION_REOPENING;
}

static inline bool psp_media_continuation_resuming(
    const PspMediaContinuation *continuation)
{
    return continuation->phase == PSP_MEDIA_CONTINUATION_RESUMING;
}

/* The autoplay the replacement open asks for; false when nothing continues. */
static inline bool psp_media_continuation_resume_playing(
    const PspMediaContinuation *continuation)
{
    return psp_media_continuation_reopening(continuation)
        && continuation->playing;
}

/* The pipeline is about to be rebuilt; the next open resumes here. A retry
   that follows a failed resume seek re-enters REOPENING from RESUMING. URL
   reuse is off unless the caller (the backward-seek reopen) opts in. */
static inline void psp_media_continuation_reopen(
    PspMediaContinuation *continuation, uint64_t position_us, bool playing)
{
    continuation->phase = PSP_MEDIA_CONTINUATION_REOPENING;
    continuation->position_us = position_us;
    continuation->playing = playing;
    continuation->reuse_resolved = false;
}

/* Cross during the reopen: the restart's destination is the latest
   committed time. */
static inline void psp_media_continuation_retarget(
    PspMediaContinuation *continuation, uint64_t position_us, bool playing)
{
    if (!psp_media_continuation_reopening(continuation)) return;
    continuation->position_us = position_us;
    continuation->playing = playing;
}

/* At the end of the replacement open: whether it runs a resume seek to the
   continuation's position (true), or settles as a fresh open (false).

   Every reopen with a known duration resumes through the seek, a paused one
   to 0:00 included. That case used to skip the seek as "already at its
   target", but settling as a fresh open made it a passive open: one armed to
   pause after its poster frame while the decoder was left paused, so no
   packet was ever fed and the first-frame watchdog failed it five seconds
   later ("VIDEO FIRST FRAME TIMED OUT"; PSP-3000, an ended 60 s clip looped
   by the stability harness, 2026-10-06). The resume seek is the path every
   other reopen takes and leaves the player paused at the target. */
static inline bool psp_media_continuation_needs_resume_seek(
    const PspMediaContinuation *continuation, uint64_t duration_us)
{
    return psp_media_continuation_reopening(continuation)
        && duration_us != 0;
}

/* The open finished and started its resume seek. */
static inline void psp_media_continuation_begin_resume(
    PspMediaContinuation *continuation)
{
    continuation->phase = PSP_MEDIA_CONTINUATION_RESUMING;
    continuation->position_us = 0;
    continuation->playing = false;
    continuation->reuse_resolved = false;
}

/* The resume seek finished or was abandoned. A REOPENING entered since (a
   retry after a failed resume) stands. */
static inline void psp_media_continuation_end_resume(
    PspMediaContinuation *continuation)
{
    if (psp_media_continuation_resuming(continuation))
        continuation->phase = PSP_MEDIA_CONTINUATION_NONE;
}

static inline void psp_media_continuation_clear(
    PspMediaContinuation *continuation)
{
    continuation->phase = PSP_MEDIA_CONTINUATION_NONE;
    continuation->position_us = 0;
    continuation->playing = false;
    continuation->reuse_resolved = false;
}

#endif
