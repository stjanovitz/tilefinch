#include "psp_media_session_internal.h"

#include <stdio.h>

#include "media_backend_psp_policy.h"
#include "psp_media_pixels.h"
#include "tilefinch/psp_log.h"

#define printf psp_log_printf

static void psp_media_note_stage_signature(
    PspMediaSession *media, const MediaVideoFrame *frame,
    const void *staging, int texture_width, int rows)
{
    (void) media;
    if (frame == NULL || staging == NULL || frame->width <= 0
        || frame->height <= 0 || texture_width < frame->width
        || rows < frame->height) return;
    media_psp_backend_note_stage_signature(
        frame,
        psp_media_surface_signature(
            (const uint32_t *) staging, (unsigned) frame->width,
            (unsigned) frame->height, (unsigned) texture_width));
}

/*
 * The decoded-output slot this session is holding, or -1 for "none".
 *
 * Every claim and every release names a slot now, because with two of them an
 * unqualified release would either free the wrong one or free both -- and
 * freeing the wrong one hands the Media Engine a surface the presenter is
 * still reading. The session only ever holds the slot its retained frame came
 * from, so the frame is the answer.
 */
static int psp_media_claimed_slot(const PspMediaSession *media)
{
    if (media == NULL || !media->have_frame
        || media->frame.pixels == NULL) return -1;
    return media->frame.slot;
}

void psp_media_present_release_claimed_surface(PspMediaSession *media)
{
    int slot = psp_media_claimed_slot(media);
    if (slot >= 0) media_psp_backend_release_surface((unsigned) slot);
}

/*
 * The staged path's release, which also ends the claim.
 *
 * The stage copy is the last read this slot will ever get: every later present
 * of the same picture samples the staged texture instead --
 * psp_media_present_texture_for skips the staging branch entirely when the
 * identity has not moved -- so once the copy is joined the slot is finished
 * with. Handing it back here rather than at the next claim is what lets a
 * conversion start a whole browser frame earlier, and it is the difference
 * between the pipeline running at one picture per claim and running at the
 * stream's rate.
 *
 * Only the staged path may. The unstaged draw and the software scaler read the
 * slot on every redraw and use psp_media_present_release_claimed_surface above, which
 * ends the lease and leaves the claim standing.
 */
static void psp_media_finish_staged_surface(PspMediaSession *media)
{
    int slot = psp_media_claimed_slot(media);
    if (slot < 0) return;
    media_psp_backend_release_surface((unsigned) slot);
    (void) media_playback_release_video_slot(
        media->playback, (unsigned) slot, media->frame.generation);
}

/*
 * The surface has just gone from read to writable. Let the decoder publish a
 * picture that was waiting for exactly this.
 *
 * A decode returns up to four pictures and the surface holds one, so the rest
 * waited for a pump visit allowed to convert them -- and those visits are the
 * same submits and drains a read window holds off, so the picture waited on a
 * window its own predecessor was holding shut. A device run put that at 5281
 * turned-away visits and 25.1ms of mean emit-to-take inside a 51.9ms picture
 * period: the conversion was landing after the frame's take rather than before
 * it, and the next access unit could not go in until the take happened.
 *
 * Always called immediately after the release and never instead of it. The
 * backend decides whether anything is owed and the borrow protocol refuses
 * while any reader is still live, so this only says when to ask.
 */
void psp_media_present_emit_after_release(PspMediaSession *media)
{
    if (media == NULL || media->playback == NULL) return;
    (void) media_playback_emit_pending_video(media->playback);
}

void psp_media_present_texture_for(
    PspMediaSession *media, const PspMediaPresentPlan *plan,
    const MediaVideoFrame *frame, void *staging, size_t staging_bytes,
    PspMediaPresentTexture *texture)
{
    if (texture == NULL) return;
    /* The layout that always works, and the answer whenever anything below
       declines. */
    texture->pixels = frame == NULL ? NULL : frame->pixels;
    texture->stride_pixels = frame == NULL ? 0 : frame->stride_pixels;
    texture->staged = false;
    /* Which memory the engine is about to sample, for the pump that feeds
       during the draw: its own copy, or the decoder's surface. */
    if (media != NULL) media->present_texture_staged = false;
    if (media == NULL || plan == NULL || frame == NULL
        || frame->pixels == NULL || staging == NULL
        /* A destination an abandoned transfer may still be writing is not a
           destination. Staging into it would be a second writer, and the
           picture it produced would be overwritten by the old transfer at a
           time nothing can predict -- after this picture's signature was
           taken, which is what makes the substitution undetectable. Fall
           through to the unstaged draw, which reads the decoded slot the
           quarantine is already keeping intact. */
        || psp_media_present_ge_stage_dma_quarantine_holds_staging(staging)
        || !psp_media_present_stage_fits(
               plan, frame->height, staging_bytes)) {
        /* Unstaged, so the engine will sample the decoder's surface for the
           whole draw. Claim it here, before the list is built, and let
           psp_media_present_texture_release end the claim once the engine has
           finished with the rows. */
        if (media != NULL && frame != NULL && frame->pixels != NULL
            && frame->slot >= 0) {
            (void) media_psp_backend_borrow_surface(
                (unsigned) frame->slot, frame->generation);
        }
        return;
    }
    int texture_width = plan->quads[0].texture_width;
    media->present_stage_async = false;
    if (media->present_stage_identity != frame->identity) {
        int rows = psp_media_present_stage_rows(frame->height);
        if (rows > plan->quads[0].texture_height)
            rows = plan->quads[0].texture_height;
        size_t stage_bytes =
            psp_media_present_stage_bytes(texture_width, rows);
        bool contiguous = frame->stride_pixels == texture_width;
        /*
         * Post the half-megabyte copy to the DMA worker when the source rows
         * are as wide as the texture -- the shipping 512 case -- so the DMA
         * controller runs it in parallel with the feed the present does next,
         * instead of blocking the interactive thread on it. The texture is not
         * sampled until psp_media_present_texture_finish has joined the copy. A
         * strided source, or a host with no worker, falls back to the
         * synchronous DMA and then the CPU stage, each of which completes the
         * copy before this returns and costs the thread the whole transfer.
         */
        /* Every branch below reads the decoded surface -- the controller in
           parallel, the CPU inline -- so the claim covers all of them. The
           asynchronous one hands it to psp_media_present_texture_finish,
           which drops it with the join. */
        if (frame->slot < 0
            || !media_psp_backend_borrow_surface(
                   (unsigned) frame->slot, frame->generation)) {
            /* The picture this present named is no longer in that slot, so
               there is nothing here to stage. Leave the texture pointing at
               the surface and let the caller draw the layout that always
               works rather than copying a successor under this identity. */
            texture->pixels = frame->pixels;
            texture->stride_pixels = frame->stride_pixels;
            texture->staged = false;
            media->present_texture_staged = false;
            return;
        }
        if (contiguous
            && psp_media_present_ge_stage_dma_submit(
                   staging, frame->pixels, stage_bytes,
                   (unsigned) frame->slot, frame->generation)) {
            media->present_stage_async = true;
            media->present_stage_pixels = staging;
            media->present_stage_width = texture_width;
            media->present_stage_rows = rows;
        } else {
            uint64_t started_us = psp_media_internal_now_us(media);
            if (!(contiguous
                  && psp_media_present_ge_stage_dma(
                         staging, frame->pixels, stage_bytes))) {
                psp_media_present_stage(
                    staging, frame->pixels, frame->stride_pixels,
                    texture_width, rows);
                psp_media_present_ge_stage_flush(staging, stage_bytes);
            }
            psp_media_note_stage_signature(media, frame, staging,
                                           texture_width, rows);
            media_psp_backend_note_frame_staged(frame);
            psp_media_finish_staged_surface(media);
            psp_media_present_emit_after_release(media);
            uint64_t elapsed_us = psp_media_internal_now_us(media) - started_us;
            media->present_stage_frames++;
            media->present_stage_total_us += elapsed_us;
            if (elapsed_us > media->present_stage_max_us)
                media->present_stage_max_us = elapsed_us;
        }
        media->present_stage_identity = frame->identity;
    }
    texture->pixels = staging;
    texture->stride_pixels = texture_width;
    texture->staged = true;
    media->present_texture_staged = true;
}

/*
 * Recover a refused access unit by resetting the decoder instead of skipping
 * it -- on the browser thread, at the point the refusal reaches it.
 *
 * Which thread does what is the whole of this design. The refusal is detected
 * inside psp_media_decode_staged_video, which runs on the codec worker, and the
 * worker keeps doing exactly what it did before: it counts the refusal, logs
 * it, retires the unit and returns. It resets nothing, waits on nothing, and
 * reads no session state -- a worker that reached into a seek would be a second
 * writer of the demuxer, the clock and the surface. All it publishes is a
 * number, the backend's running refusal total, and this runs after an advance
 * has collected the job that incremented it. A total above the one already
 * accounted for is a refusal this frame owes a recovery to.
 *
 * The recovery rebuilds at a later random-access point. Device experiments
 * proved that continuing, replaying the refused AU, or omitting it while
 * replaying dependent pictures can all hang firmware. This conservative path
 * can visibly skip content, but it remains only as a backstop: the ordinary
 * decode path now completes the firmware's Detail2 transaction after every
 * successful AU and should prevent the refusal state from arising.
 *
 * Returns true when it acted, which ends the caller's frame: the reset
 * invalidated the borrowed surface, so no picture is left for the rest of the
 * advance to present. `decision_ready` is distinct: false means the shared
 * codec slot was still running and the refusal total could not be observed.
 * That is not "no refusal"; the worker's video hold must stay latched until a
 * later frame can make the decision from a stable snapshot.
 */
static size_t psp_media_pump_present(
    PspMediaSession *media, bool (*still_busy)(void), int mode)
{
    if (media == NULL || media->playback == NULL || !media->ui.visible
        || !media->ui.playing
        || media->machine.state == PSP_MEDIA_SESSION_OPENING
        || media->job_phase != PSP_MEDIA_JOB_NONE
        || media->pause_boundary_pending) return 0;
    /* The whole difference between here and the frame's own advance: this
       thread is waiting on hardware, so a unit may block for the codec worker
       at no cost to the frame -- and, while the copy is the hardware being
       waited on, may not hand the decoder a picture to write over it. */
    media_psp_backend_set_advance_mode(mode);
    uint64_t started_us = psp_media_internal_now_us(media);
    uint64_t decode_clock_us =
        psp_media_session_decode_clock_us(media, false);
    char error[256] = {0};
    MediaPlaybackJobStats before = {0};
    media_playback_job_stats(media->playback, &before);
    size_t units = 0;
    while (units < PSP_MEDIA_PUMP_DRAW_MAXIMUM_UNITS) {
        uint64_t now_us = psp_media_internal_now_us(media);
        uint64_t spent_us = now_us - started_us;
        if (spent_us >= PSP_MEDIA_PUMP_DRAW_SLICE_US) break;
        if (!still_busy()) break;
        /* A unit may sleep for the codec worker, but never past the window
           that made sleeping free. The device measured one 4ms wait consuming
           a 4.45ms draw and returning 0.6 submissions a frame; what this pump
           needs from its window is several looks at the decoder, not one. */
        media_psp_backend_set_wait_limit_us(
            (unsigned) (PSP_MEDIA_PUMP_DRAW_SLICE_US - spent_us));
        MediaPlaybackAdvanceResult advance =
            media_playback_advance_bounded_cancelable(
                media->playback, decode_clock_us,
                PSP_MEDIA_JOB_MAXIMUM_PACKETS, NULL,
                error, sizeof(error));
        units++;
        /* An error here is the ordinary advance's to diagnose: it has the
           retry ladder, the panel and the stats. Stop and let the next frame
           reach it rather than reporting a failure from inside a present. */
        if (advance != MEDIA_PLAYBACK_ADVANCE_PENDING) break;
    }
    MediaPlaybackJobStats after = {0};
    media_playback_job_stats(media->playback, &after);
    media_psp_backend_set_advance_mode(PSP_MEDIA_ADVANCE_FRAME);
    media_psp_backend_set_wait_limit_us(0u);
    media->pump_draw_frames++;
    media->pump_draw_units += units;
    media->pump_draw_submitted +=
        after.packets_submitted - before.packets_submitted;
    media->pump_draw_us += psp_media_internal_now_us(media) - started_us;
    return units;
}

size_t psp_media_pump_while_drawing(PspMediaSession *media)
{
    /* The engine samples the staged copy in the display's own memory, which
       the decoder never touches, so this pump may feed anything. When staging
       declined -- a geometry it does not fit, a buffer it cannot have -- the
       engine is reading the decoded surface directly and video waits exactly
       as it does during the copy. */
    bool reads_the_surface =
        media != NULL && !media->present_texture_staged;
    return psp_media_pump_present(
        media, psp_media_present_ge_drawing,
        reads_the_surface
            ? PSP_MEDIA_ADVANCE_STAGE_COPY : PSP_MEDIA_ADVANCE_DRAW);
}

void psp_media_present_texture_finish(PspMediaSession *media)
{
    if (media == NULL || !media->present_stage_async) return;
    /* Feed the decoder while the DMA worker copies this picture, then collect
       it before the caller starts the list. The stage cost recorded here is the
       residual join wait after that feeding -- what the overlap could not hide,
       near zero when the copy finished inside the feed window. */
    (void) psp_media_pump_present(
        media, psp_media_present_ge_stage_dma_busy,
        PSP_MEDIA_ADVANCE_STAGE_COPY);
    uint64_t started_us = psp_media_internal_now_us(media);
    PspMediaPresentDmaJoin outcome = psp_media_present_ge_stage_dma_join();
    bool joined = outcome == PSP_MEDIA_DMA_JOIN_SUCCESS;
    bool still_live = outcome == PSP_MEDIA_DMA_JOIN_TIMED_OUT_STILL_LIVE;
    if (still_live) {
        /*
         * Nobody stopped this transfer, so both of its addresses are somebody
         * else's until it is seen to end.
         *
         * Everything the completed-failure branch does below is forbidden
         * here, and this is why the two used to be one branch and must not
         * be. Repeating the copy would put a second writer into a destination
         * a live controller is writing. Sampling the pixel signature would
         * hash bytes that are still moving. Publishing the staged identity
         * would tell the next present that this picture is already staged, so
         * the frame after would draw a texture the abandoned transfer is
         * still landing in. Forget the identity, quarantine the source slot
         * so the decoder cannot be handed it, and remember which picture is
         * owed a release when the transfer is finally observed to finish.
         */
        media->present_stage_identity = 0;
        if (media->frame.slot >= 0) {
            media_psp_backend_quarantine_surface(
                (unsigned) media->frame.slot);
            media->dma_quarantine_slot = media->frame.slot;
            media->dma_quarantine_generation = media->frame.generation;
        }
        printf("tilefinch-media-present: event=dma-quarantine slot=%d "
               "generation=%u identity=%llu\n",
               media->frame.slot, (unsigned) media->frame.generation,
               (unsigned long long) media->frame.identity);
    } else if (!joined) {
        /*
         * The worker answered and the controller is provably finished with
         * both addresses; it simply refused the copy. The texture holds
         * whatever it left, and the caller is about to draw from it, so
         * repeat the copy here -- same bytes, same source, same destination
         * -- and if even that is impossible, forget the identity so the next
         * present stages the picture again rather than trusting this one
         * forever.
         */
        if (!psp_media_present_ge_stage_dma_recover(
                media->frame.slot, media->frame.generation))
            media->present_stage_identity = 0;
    }
    if (!still_live) {
        psp_media_note_stage_signature(
            media, &media->frame, media->present_stage_pixels,
            media->present_stage_width, media->present_stage_rows);
        media_psp_backend_note_frame_staged(&media->frame);
    }
    if (joined) {
        /* The copy is the last reader of the surface on this path: every later
           present of this picture samples the staged texture, which the
           decoder cannot reach. Give the slot back to the codec worker now
           rather than at the next claim, so a conversion waiting on it waits
           only for the copy -- which is the whole of the rate this second
           surface was reserved to buy. */
        psp_media_finish_staged_surface(media);
    } else {
        /*
         * Neither failure ends the claim early.
         *
         * A completed failure could -- the controller is finished with the
         * slot -- but the picture was not staged, so later presents still
         * read the slot and the claim has to outlive this frame exactly as
         * the unstaged path's does. A timed-out join must not, and for a
         * stronger reason: the transfer is still live. Dropping the lease is
         * safe in that case only because the quarantine set above has already
         * taken over the job the lease was doing, and it is the quarantine,
         * not the lease, that the claim, the release and the reset quiesce
         * now consult.
         */
        psp_media_present_release_claimed_surface(media);
    }
    psp_media_present_emit_after_release(media);
    uint64_t elapsed_us = psp_media_internal_now_us(media) - started_us;
    media->present_stage_async = false;
    media->present_stage_frames++;
    media->present_stage_total_us += elapsed_us;
    if (elapsed_us > media->present_stage_max_us)
        media->present_stage_max_us = elapsed_us;
}

size_t psp_media_feed_before_blocking(PspMediaSession *media)
{
    /* The frame stops here whether or not this feed does anything, so the
       phase is published before the guard rather than after it: a conversion
       landing during the block is in the block's window even when there was
       nothing left to submit into it. Telemetry only. */
    media_psp_backend_set_loop_phase(PSP_MEDIA_LOOP_PHASE_VBLANK);
    /*
     * The codec worker now outranks this thread, but a pre-vblank feed still
     * matters: it gives the worker a ready unit before the frame's longest
     * block instead of leaving the Media Engine idle during that wait. Whatever
     * the present left queued
     * was all the worker had for it. A submission attempt costs a staging
     * memcpy and an event flag, takes no wait of any kind, and hands the
     * worker several milliseconds of work it would otherwise have spent idle.
     *
     * Bounded to one unit. This is not a pump; it is the last thing the frame
     * does before it stops, and its whole job is that the worker does not.
     */
    if (media == NULL || media->playback == NULL || !media->ui.visible
        || !media->ui.playing
        || media->machine.state == PSP_MEDIA_SESSION_OPENING
        || media->job_phase != PSP_MEDIA_JOB_NONE
        || media->pause_boundary_pending) return 0;
    MediaPlaybackJobStats before = {0};
    MediaPlaybackJobStats after = {0};
    media_playback_job_stats(media->playback, &before);
    char error[256] = {0};
    (void) media_playback_advance_bounded_cancelable(
        media->playback,
        psp_media_session_decode_clock_us(media, false),
        PSP_MEDIA_JOB_MAXIMUM_PACKETS, NULL, error, sizeof(error));
    media_playback_job_stats(media->playback, &after);
    size_t submitted =
        after.packets_submitted - before.packets_submitted;
    media->prevblank_frames++;
    media->prevblank_submitted += submitted;
    return submitted;
}

void psp_media_present_texture_release(PspMediaSession *media)
{
    if (media == NULL) return;
    psp_media_present_release_claimed_surface(media);
    psp_media_present_emit_after_release(media);
}

/*
 * The end of a quarantine, one frame at a time.
 *
 * A transfer nobody could join cannot be cancelled, so there are exactly two
 * ways out: it is seen to finish, or it never is. The first is polled here at
 * the cost of one event-flag peek per frame -- and because the timed-out join
 * also latched the DMA worker dead, no further copy is ever posted and that
 * event can only belong to the abandoned one. That is what makes the poll
 * proof rather than inference.
 *
 * On the observation the slot is released through the ordinary staged-release
 * path, naming the picture the copy actually read: the quarantine kept the
 * slot READING at that generation, so the release is the same one the join
 * would have made had it landed, merely late.
 *
 * The second way out has no safe continuation. A controller that has not
 * finished a quarter-megabyte transfer in two seconds is not going to, and
 * every frame spent waiting is a frame with one slot fewer and a picture on
 * the panel that cannot be replaced. End the session through the same surface
 * every other unrecoverable media failure uses, and let the retry ladder
 * rebuild the whole stack -- worker included.
 *
 * Returns true when it ended the session, which ends the caller's frame.
 */

