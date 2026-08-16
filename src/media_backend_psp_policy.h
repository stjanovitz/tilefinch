#ifndef TILEFINCH_MEDIA_BACKEND_PSP_POLICY_H
#define TILEFINCH_MEDIA_BACKEND_PSP_POLICY_H

/* Self-contained: every rule here is written against fixed-width types, and
   the header is now included by callers that establish neither. */
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * How far past the presentation clock a pump may submit.
 *
 * Fifty milliseconds was three and a half access units of runway: a browser
 * frame that overran had no way to make the deficit up, because the horizon
 * moves with the clock and never lends anything back. Device cycle verdict3
 * shows the consequence -- horizon exits on 71% of playing frames while the
 * Media Engine sat 88% idle (1,336 jobs averaging 3.5 ms across a 40-second
 * session). The engine was never the ceiling; eligibility was.
 *
 * The resource that actually grows with this number is the audio PCM queue,
 * and it is fixed: PSP_MEDIA_AUDIO_QUEUE_SLOTS blocks of
 * PSP_MEDIA_AUDIO_SAMPLES, which at the maximum supported rate is what the
 * static assert below bounds. Nothing is allocated per lead microsecond --
 * every admitted sample still passes through the single bounded packet buffer
 * and becomes the backend's, and the backend answers WOULD_BLOCK when its
 * queue or its one Media Engine job is full. So this only decides what a pump
 * may consider, never how much memory exists, and unit boundedness, the slice
 * and the cancellation checks are untouched.
 *
 * Half a second is the largest value the PCM queue admits and is the same
 * horizon the first-frame preroll already used, which is why that preroll now
 * needs no boost of its own.
 */
#define PSP_MEDIA_DECODE_LEAD_US 500000u
#define PSP_MEDIA_FIRST_FRAME_PREROLL_US 500000u
#define PSP_MEDIA_FIRST_FRAME_TIMEOUT_US 5000000u
#define PSP_MEDIA_FIRST_FRAME_ABSOLUTE_US 30000000u
#define PSP_MEDIA_FIRST_FRAME_NETWORK_PUMP_US 500000u
/* Rebuffering is deliberately hysteretic. Ordinary one-frame CDN jitter is
   invisible; a sustained empty source pauses presentation, then resumes only
   after both fixed HTTP windows again represent useful time. Repeated stalls
   ask for a longer reserve without allocating another byte. */
#define PSP_MEDIA_BUFFER_DEBOUNCE_US 350000u
#define PSP_MEDIA_BUFFER_TARGET_US 2000000u
#define PSP_MEDIA_BUFFER_STARTUP_TARGET_US 3000000u
#define PSP_MEDIA_BUFFER_REPEAT_TARGET_US 3000000u
#define PSP_MEDIA_BUFFER_STABLE_US 250000u
#define PSP_MEDIA_BUFFER_REPEAT_STABLE_US 600000u
#define PSP_MEDIA_BUFFER_DECODE_READY_US 400000u
#define PSP_MEDIA_BUFFER_SLOW_NOTICE_US 20000000u

/*
 * The buffering decision is deliberately pure. The PSP session owns the
 * transport, decoder and UI side effects, while this function owns the
 * hysteresis which decides when those effects happen. Keeping that split
 * makes the exact device policy available to the deterministic host network
 * simulator: a short Wi-Fi gap must remain invisible, a sustained starvation
 * may open the buffering surface once, and a refill must remain useful for a
 * stable interval before the surface closes again.
 */
typedef enum {
    PSP_MEDIA_BUFFER_KEEP = 0,
    PSP_MEDIA_BUFFER_BEGIN,
    PSP_MEDIA_BUFFER_END
} PspMediaBufferAction;

typedef struct {
    bool playing;
    bool pause_after_next_frame;
    bool job_active;
    bool buffering;
    bool startup;
    bool source_blocked;
    bool fill_pending;
    unsigned buffer_events;
    uint64_t now_us;
    uint64_t starved_since_us;
    uint64_t ready_since_us;
    uint64_t remaining_us;
    uint64_t decoded_ahead_us;
    uint64_t network_ahead_us;
} PspMediaBufferPolicyInput;

typedef struct {
    PspMediaBufferAction action;
    uint64_t starved_since_us;
    uint64_t ready_since_us;
    uint64_t target_us;
    uint64_t stable_us;
} PspMediaBufferPolicyDecision;

static inline PspMediaBufferPolicyDecision psp_media_buffer_policy(
    PspMediaBufferPolicyInput input)
{
    PspMediaBufferPolicyDecision decision = {
        .action = PSP_MEDIA_BUFFER_KEEP,
        .starved_since_us = input.starved_since_us,
        .ready_since_us = input.ready_since_us,
        .target_us = input.startup
            ? PSP_MEDIA_BUFFER_STARTUP_TARGET_US
            : (input.buffer_events >= 3u
                ? PSP_MEDIA_BUFFER_REPEAT_TARGET_US
                : PSP_MEDIA_BUFFER_TARGET_US),
        .stable_us = input.buffer_events >= 3u
            ? PSP_MEDIA_BUFFER_REPEAT_STABLE_US
            : PSP_MEDIA_BUFFER_STABLE_US
    };
    if (input.remaining_us != 0 && decision.target_us > input.remaining_us)
        decision.target_us = input.remaining_us;
    if (!input.playing || input.pause_after_next_frame || input.job_active) {
        decision.starved_since_us = 0;
        decision.ready_since_us = 0;
        if (input.buffering) decision.action = PSP_MEDIA_BUFFER_END;
        return decision;
    }
    if (!input.buffering) {
        if (input.source_blocked && input.fill_pending) {
            if (decision.starved_since_us == 0)
                decision.starved_since_us = input.now_us;
            if (input.now_us >= decision.starved_since_us
                && input.now_us - decision.starved_since_us
                       >= PSP_MEDIA_BUFFER_DEBOUNCE_US) {
                decision.action = PSP_MEDIA_BUFFER_BEGIN;
                decision.starved_since_us = 0;
                decision.ready_since_us = 0;
            }
        } else if (!input.fill_pending) {
            decision.starved_since_us = 0;
        }
        return decision;
    }

    bool ready = !input.source_blocked
        && input.network_ahead_us >= decision.target_us
        && (input.decoded_ahead_us >= PSP_MEDIA_BUFFER_DECODE_READY_US
            || input.remaining_us <= PSP_MEDIA_BUFFER_DECODE_READY_US);
    if (!ready) {
        decision.ready_since_us = 0;
        return decision;
    }
    if (decision.ready_since_us == 0) {
        decision.ready_since_us = input.now_us;
        return decision;
    }
    if (input.now_us >= decision.ready_since_us
        && input.now_us - decision.ready_since_us >= decision.stable_us) {
        decision.action = PSP_MEDIA_BUFFER_END;
        decision.starved_since_us = 0;
        decision.ready_since_us = 0;
    }
    return decision;
}

#define PSP_MEDIA_SEEK_TIMEOUT_US 5000000u
/* Long enough that an ordinary busy decoder never trips it, short enough that
   a wedge is named while the session is still on screen. */
#define PSP_MEDIA_STALL_REPORT_US 3000000u
/*
 * How far past the clock a source has to sit before it cannot have got there
 * by playing.
 *
 * The eligibility horizon only ever admits samples up to clock +
 * PSP_MEDIA_DECODE_LEAD_US, so an ordinary playing pipeline is buffered barely
 * beyond that -- one packet, tens of milliseconds. Anything further was put
 * there by a seek that repositioned the demuxer, and if the clock did not move
 * with it the horizon can never admit another sample: the pipeline exits on
 * eligibility forever while the source waits with content nothing will ask for.
 * Four decode leads is comfortably clear of the ordinary case and far below the
 * sixteen-second gap the device log recorded.
 */
#define PSP_MEDIA_CLOCK_DIVERGENCE_US (4u * PSP_MEDIA_DECODE_LEAD_US)
#define PSP_MEDIA_MODULE_PREPARE_TIMEOUT_US 20000000u
/*
 * The open transaction's own bounds, and why it needed any.
 *
 * Only module preparation was ever deadlined. Every other open phase relied on
 * the bound of whatever it called into, and those bounds compose: the demux
 * phase performs an unbounded number of blocking window reads and each one
 * re-armed its own fifteen-second window timeout, so "bounded" per read meant
 * nothing at all per phase. A device run sat on the OPENING VIDEO screen for
 * more than ten minutes after `tilefinch-media-modules: stage=ready` and
 * printed not one further line -- no deadline fired, and the phase it was in
 * could not even be named from the log.
 *
 * A healthy open measures twenty to twenty-five seconds on a PSP-3000 and the
 * resolver already caps itself at thirty, so a phase bound has to sit above
 * that to avoid pre-empting a bound that already exists. The total is what
 * makes a stuck open finite; the report interval is what makes it visible
 * while it is still stuck, which is the property whose absence cost the cycle.
 */
#define PSP_MEDIA_OPEN_PHASE_TIMEOUT_US 45000000u
#define PSP_MEDIA_OPEN_TOTAL_TIMEOUT_US 90000000u
#define PSP_MEDIA_OPEN_REPORT_INTERVAL_US 5000000u
/*
 * What the pump may spend while the graphics engine is drawing.
 *
 * The engine's wait touches no media memory, so the pump can have it -- but
 * only it. Sized generously at first, that budget became the frame: 23 units
 * and 15.1ms per present against an engine wait of 2.5ms, a 40ms slice living
 * inside a 33ms frame. The pump now stops when the engine stops, and these
 * are the ceiling for a frame where the poll misleads, not the target.
 *
 * Units are what these pumps then ran out of. A soak measured a 7.93ms engine
 * sync of which the pump covered 4.6ms before hitting six units, leaving
 * 2.66ms of pure wait with the decoder given nothing -- and the blocking
 * collect the ordinary advance path used to take has moved here, where the
 * frame does not pay for it. A unit that takes that collect can spend a whole
 * PSP_MEDIA_CODEC_COLLECT_WAIT_US, so the slice holds the measured sync plus
 * one such unit rather than the two it held before. still_busy() is still the
 * bound that matters; these only cover a frame where the poll misleads.
 */
#define PSP_MEDIA_PUMP_DRAW_SLICE_US 9000u
#define PSP_MEDIA_PUMP_DRAW_MAXIMUM_UNITS 12u
/* Fullscreen playback is paced by the presenter's vblank. Waiting a second
   vblank before inspecting PTS aliases a 24-fps stream against the browser's
   ~30-fps loop: one check is early and the next is late. A short sleep still
   yields the main CPU to the lower-priority codec worker without quantizing
   eligibility to the display period. Normal browsing retains its vblank
   throttle. */
#define PSP_MEDIA_FULLSCREEN_POLL_YIELD_US 2000u
/*
 * What an advance may afford, decided by where in the frame it runs.
 *
 * Two properties differ between the once-per-frame advance and the advances
 * the dead-time pumps make, and both were previously assumed.
 *
 * The first is the bounded collect wait. It exists to yield to the codec
 * worker. Even though that worker now outranks the browser, without this look
 * its completion is observed a whole main-loop iteration later. On a pump
 * that is filling
 * the engine's draw or the stage copy, that wait is free -- the thread had
 * nothing else to do. On the frame's own advance it is frame time: a device
 * soak measured 962 wait calls costing 2.65 seconds across 300 playing
 * frames, 8.8ms of every 46.4ms frame, which is most of the difference
 * between a 24fps stream playing at 0.9x and the same stream fitting two
 * vertical blanks.
 *
 * The second is the decoded surface. A borrowed video frame stays valid until
 * the next submit (see MediaVideoFrame in tilefinch/media_backend.h), and a
 * video submit can emit a picture the previous decode left pending -- a
 * colour conversion straight into that surface. While the DMA controller is
 * copying the surface into the texture, that is a write into the buffer the
 * copy is reading. The draw pump runs after the copy has been joined and is
 * unaffected; only the pump that overlaps the copy has to hold video back.
 * Audio owns none of that memory and keeps feeding.
 */
typedef enum {
    /* The frame's own advance: never block, the frame is the budget. */
    PSP_MEDIA_ADVANCE_FRAME = 0,
    /* Filling the graphics engine's draw: blocking is free, nothing of the
       decoder's is in flight. */
    PSP_MEDIA_ADVANCE_DRAW = 1,
    /* Filling the stage copy: blocking is free, but the copy is reading the
       decoded surface. */
    PSP_MEDIA_ADVANCE_STAGE_COPY = 2
} PspMediaAdvanceMode;

static inline bool psp_media_advance_may_wait(int mode)
{
    return mode == PSP_MEDIA_ADVANCE_DRAW
        || mode == PSP_MEDIA_ADVANCE_STAGE_COPY;
}

static inline bool psp_media_advance_may_submit_video(int mode)
{
    return mode != PSP_MEDIA_ADVANCE_STAGE_COPY;
}

/*
 * Where in the browser's frame a colour conversion finished.
 *
 * The worker converts whenever firmware returns, which is a different question
 * from where the browser thread was standing at the time -- and the second
 * question is the one that says whether a conversion lands where a take can
 * see it. A conversion completed during the vertical-blank block is one the
 * next frame's take collects immediately; one completed just after that take,
 * during the pump, waits a whole frame for the following one. Nothing in the
 * existing counters separates those two, and they have opposite fixes.
 *
 * Four coarse tags, sampled once per conversion, at one atomic load. PRESENT
 * is not published by the session at all: the present window is already
 * announced as an advance mode, and deriving it from that is one fewer pair of
 * setters to keep in step. VBLANK covers the pre-vblank feed, the block
 * itself, and the input poll that follows it -- the whole tail of the frame
 * after the session stops asking for work.
 */
typedef enum {
    PSP_MEDIA_LOOP_PHASE_OTHER = 0,
    PSP_MEDIA_LOOP_PHASE_PUMP = 1,
    PSP_MEDIA_LOOP_PHASE_PRESENT = 2,
    PSP_MEDIA_LOOP_PHASE_VBLANK = 3
} PspMediaLoopPhase;

#define PSP_MEDIA_LOOP_PHASES 4u

static inline int psp_media_loop_phase_resolve(
    int loop_phase, int advance_mode)
{
    if (advance_mode == PSP_MEDIA_ADVANCE_DRAW
        || advance_mode == PSP_MEDIA_ADVANCE_STAGE_COPY)
        return PSP_MEDIA_LOOP_PHASE_PRESENT;
    return loop_phase >= 0 && loop_phase < (int) PSP_MEDIA_LOOP_PHASES
        ? loop_phase : PSP_MEDIA_LOOP_PHASE_OTHER;
}

/*
 * A duration histogram, because a percentile needs the distribution and this
 * target cannot keep the samples.
 *
 * The one shared job slot serves AAC as well as AVC, so how long an audio job
 * holds it decides how many video submissions are refused for no reason to do
 * with video at all. A mean cannot answer that -- a slot held 3ms ninety
 * times a second and one held 40ms nine times a second have the same mean and
 * entirely different consequences -- and sorting several thousand samples on
 * device is not on. Fourteen fixed buckets cost 56 bytes and one compare
 * chain per job, and the percentile they yield is a bucket ceiling: an upper
 * bound on the true value, never a measurement of it, which is why the field
 * names say so.
 */
#define PSP_MEDIA_JOB_HISTOGRAM_BUCKETS 14u

static inline uint32_t psp_media_job_bucket_ceiling_us(unsigned bucket)
{
    static const uint32_t ceilings[PSP_MEDIA_JOB_HISTOGRAM_BUCKETS] = {
        500u, 1000u, 2000u, 3000u, 4000u, 6000u, 8000u, 12000u,
        16000u, 24000u, 32000u, 48000u, 64000u, UINT32_MAX
    };
    return bucket < PSP_MEDIA_JOB_HISTOGRAM_BUCKETS
        ? ceilings[bucket] : UINT32_MAX;
}

static inline unsigned psp_media_job_bucket(uint32_t duration_us)
{
    for (unsigned at = 0; at < PSP_MEDIA_JOB_HISTOGRAM_BUCKETS; at++)
        if (duration_us <= psp_media_job_bucket_ceiling_us(at)) return at;
    return PSP_MEDIA_JOB_HISTOGRAM_BUCKETS - 1u;
}

/*
 * The ceiling of the bucket the `permille`-th sample falls in, or zero for a
 * histogram nothing has been recorded into. Rounded up, so the median of a
 * single sample is that sample's own bucket rather than the one below it.
 */
static inline uint32_t psp_media_job_percentile_us(
    const uint32_t *buckets, unsigned count, unsigned permille)
{
    if (buckets == NULL || count == 0) return 0;
    uint64_t total = 0;
    for (unsigned at = 0; at < count; at++)
        total += (uint64_t) buckets[at];
    if (total == 0) return 0;
    uint64_t target =
        (total * (uint64_t) permille + UINT64_C(999)) / UINT64_C(1000);
    if (target == 0) target = 1;
    uint64_t seen = 0;
    for (unsigned at = 0; at < count; at++) {
        seen += (uint64_t) buckets[at];
        if (seen >= target) return psp_media_job_bucket_ceiling_us(at);
    }
    return psp_media_job_bucket_ceiling_us(count - 1u);
}
/*
 * And the same exclusion for the write the browser thread did not order.
 *
 * Holding video at the submit boundary is not the whole rule, because the
 * write is not made there. An access unit accepted before a read window opens
 * decodes on the codec worker, and the worker's own completion path converts
 * the picture into the surface -- so the write can land in the middle of a
 * stage copy that a submit gate, looking only at submissions, never saw. The
 * arithmetic said it would not: decode (>=4.2ms) plus conversion (>=5.5ms)
 * against a window that closes about 6ms after the frame is taken. That is a
 * four-millisecond margin guaranteed by nothing, and it narrows every time
 * the decoder gets faster or the present gets slower.
 *
 * So the two sides say so instead. A reader claims the surface and then waits
 * out any conversion already inside firmware; a conversion announces itself
 * and then waits out any claim. Each budget bounds the other side's longest
 * legitimate hold: one colour conversion for the reader, and for the writer
 * the longest read window there is -- the engine sampling the surface
 * directly for a whole draw, which is longer than the stage copy. Exceeding
 * either is not a delay, it is a defect, and both sides stop rather than
 * spin: the reader proceeds (its claim still fences every later write) and
 * the writer leaves the picture captured-pending for a later submit, which
 * the advance mode already fences.
 */
#define PSP_MEDIA_SURFACE_BORROW_WAIT_US 8000u
#define PSP_MEDIA_SURFACE_LEASE_WAIT_US 20000u
/*
 * Where the codec worker sits, and why it moved.
 *
 * It ran at the clock-worker priority, below the browser thread, which meant a
 * submitted job could not *start* until the browser thread next blocked.
 * Every one of the roughly 68 jobs a second paid a dispatch delay equal to
 * the browser's time-to-next-block, and the measured ceiling shows it: total
 * submissions stuck near 40 a second across two runs while the worker itself
 * was busy about a third of the time. A job slot that is idle and a queue
 * that is full at the same time is a scheduling result, not a capacity one.
 *
 * The worker's own CPU use is small -- it sleeps inside sceMpegAvcDecode,
 * sceMpegBaseCscAvc and sceAudiocodecDecode while the Media Engine works --
 * which is the same shape that makes the DMA worker safe above the browser.
 * It sits below DMA and audio output: their work is microseconds and must not
 * queue behind a codec job that is milliseconds inside firmware. The actual
 * values and compile-time ordering checks live together in psp_threads.h.
 *
 * The risk is the assumption. If those firmware calls spin on the CPU rather
 * than sleeping, this thread takes frame time away from the browser instead
 * of using time the browser was not using, and playing-steady-period on the
 * next soak says so immediately -- against 29017us measured at the old clock
 * priority. One named constant, and it reverts as cleanly as it lands.
 */
/*
 * How long a codec job may be in firmware before it is not coming back.
 *
 * Two soaks died at the same access unit -- nal-size=1707, a well-formed
 * non-IDR slice about five seconds past the seek's keyframe -- and in both the
 * worker logged event=avc-bridge-submit and then nothing. That line is
 * written on the worker immediately before sceMpegGetAvcNalAu, so the job was
 * picked up and firmware did not return from it. Everything downstream
 * followed: no completion, so no submission, so no byte was ever needed and
 * the transport went quiet, and two seconds later the frontend reported "NO
 * PACKET ACCEPTED" -- which names the watchdog rather than the wedge.
 *
 * The main thread cannot recover a thread that is inside firmware, but it can
 * say so. This sits above every job any device has completed (336ms was the
 * worst, and that measurement included the collect wait) and below the
 * frontend's no-progress ceiling, so the specific stage is named before the
 * generic ceiling fires. codec_native_stage says which firmware call it was.
 */
#define PSP_MEDIA_CODEC_JOB_WATCHDOG_US 1500000u

/*
 * Where the stability soak's forward seek lands, from a permille of duration.
 *
 * The fraction was two thirds and hardcoded. It is a knob because it is the
 * one variable that discriminates between the two stories about the hang five
 * seconds past that seek: at 667 the seek lands just before the offending
 * access unit and the decoder meets it on a freshly reprimed state, while at
 * 333 the same soak plays continuously into the very same content with the
 * state playback accumulated on its way there. Same bytes, different decoder.
 *
 * A stream too short to seek into meaningfully keeps the old midpoint
 * behaviour: below twelve seconds the fractions collapse into each other and
 * the soak is measuring startup either way.
 */
static inline uint64_t psp_media_stability_seek_target_us(
    uint64_t duration_us, long permille)
{
    if (duration_us <= UINT64_C(12000000)) return duration_us / 2u;
    if (permille <= 0 || permille >= 1000) permille = 667;
    /* Split the multiply so a 64-bit duration cannot overflow on the way. */
    uint64_t whole = (duration_us / 1000u) * (uint64_t) permille;
    uint64_t remainder = (duration_us % 1000u) * (uint64_t) permille / 1000u;
    return whole + remainder;
}
/*
 * How long a playing pipeline may accept no access unit before it is a defect
 * rather than a wait -- and why that is two numbers.
 *
 * Two seconds is right for a pipeline holding everything it needs: nothing
 * arriving from a decoder that has work and memory is a wedge, and the sooner
 * it is named the better. It is wrong for a pipeline whose source is still
 * fetching. PSP Wi-Fi delivers 150-250 KB/s and a cold position needs a window
 * on each of two sources, so the first access unit after a far seek or a
 * resume can legitimately be three to four seconds away -- plus a handshake if
 * the connection is new. A device run died exactly there: a resume at 168s of
 * a 252s stream, both windows cold, source-block=134 refill-block=134, killed
 * seven seconds in by the two-second ceiling with the refill still on the wire.
 *
 * So an outstanding refill buys the longer budget and nothing else does. Six
 * seconds is above the worst legitimate cold fetch this link produces and far
 * below a user's patience for a picture that is never coming; a source that
 * has nothing outstanding, or one whose window never lands, still fails on the
 * schedule it always did. Neither budget is reached while anything moves: a
 * frame, an access unit, or a byte is progress and resets the clock.
 */
#define PSP_MEDIA_DECODE_NO_PROGRESS_MS 2000u
#define PSP_MEDIA_DECODE_NO_PROGRESS_REFILL_MS 6000u

static inline unsigned psp_media_decode_no_progress_budget_ms(
    bool source_refilling)
{
    return source_refilling
        ? PSP_MEDIA_DECODE_NO_PROGRESS_REFILL_MS
        : PSP_MEDIA_DECODE_NO_PROGRESS_MS;
}
/*
 * And what the ordinary per-frame pump may spend while a video owns the whole
 * screen. There is no page behind it to owe the rest of the frame to, and the
 * device measured that pump unit-capped on 374 of 405 frames while using only
 * 1.2ms of its 12ms slice -- it wanted units, not time. Feeding belongs here,
 * in the frame, rather than inside a present where it was bounded by nothing
 * the frame knows about.
 *
 * That reading was of a pump whose units were worth spending because each one
 * could block for the codec worker. They no longer can -- the frame's own
 * advance stops at the first unit that submits nothing, since nothing on this
 * path will let the worker run before the next one -- so this ceiling now
 * bounds only a genuine catch-up run of successful submissions, and the
 * measured unit-cap should fall to nearly zero. Left where it is: it costs a
 * frame nothing to have room it does not use, and taking it away would cap a
 * pipeline that really is behind.
 */
#define PSP_MEDIA_PUMP_FULLSCREEN_MAXIMUM_UNITS 20u
/*
 * Steady-state playing-cadence measurement.
 *
 * The sustained frame-period is what the mandate is read against, and it must
 * not be polluted by two one-time costs. The first is warmup: the open, the
 * first decode and the first-connection handshake all land in the opening
 * second and each shows up as a long playing period. Skip that many playing
 * periods -- at a ~16.7ms cadence this is roughly the first second -- before a
 * period counts toward steady state. The second is a lone pathological stall: a
 * single ~500ms transport reconnect craters a mean it is averaged into. Exclude
 * any period longer than this ceiling, set well above any real per-frame
 * cadence (a doubled 33ms frame, even a quadrupled 66ms one) and well below a
 * connection stall, so genuine cadence variance stays in and the outlier does
 * not. Both excluded costs are still visible in the untrimmed total and max.
 */
#define PSP_MEDIA_PLAYING_STEADY_WARMUP_PERIODS 30u
#define PSP_MEDIA_PLAYING_STEADY_CEILING_US 200000u
/*
 * How many access units firmware may refuse before the stream is the problem.
 *
 * The device proved the bridge builds valid access units and firmware refuses
 * particular content: au-before carried esBuffer=0x09010000 with esSize=0x132,
 * exactly the 306-byte packet, and sceMpegAvcDecode still answered
 * 0x80628002. Two sessions died on 306-byte packets at unrelated stream
 * positions, so it is a recurring access-unit shape rather than a position.
 *
 * One such refusal must not end playback -- dropping a single access unit
 * costs one frame, ending the session costs the session. But a stream firmware
 * will not decode has to fail rather than run silently forever, so a short
 * consecutive run escalates immediately, and a stream that dribbles refusals
 * escalates on rate. One a second is already three percent of a 30fps stream
 * and far more than a healthy one produces.
 */
#define PSP_MEDIA_AVC_REFUSAL_CONSECUTIVE_LIMIT 5u
#define PSP_MEDIA_AVC_REFUSAL_WINDOW_US 60000000u
#define PSP_MEDIA_AVC_REFUSAL_WINDOW_LIMIT 60u

/*
 * Whether a refused access unit may be skipped, or the run of them has become
 * the stream's verdict. `consecutive` counts refusals since the last picture;
 * `window` counts them inside PSP_MEDIA_AVC_REFUSAL_WINDOW_US.
 */
static inline bool psp_media_avc_refusal_survivable(
    unsigned consecutive, unsigned window)
{
    return consecutive < PSP_MEDIA_AVC_REFUSAL_CONSECUTIVE_LIMIT
        && window < PSP_MEDIA_AVC_REFUSAL_WINDOW_LIMIT;
}

/* A refusal invalidates the current dependent-frame chain. Recovery asks the
   demuxer for the next actual random-access sample, so this policy gate only
   has to establish that some stream duration remains. The demuxer—not a
   guessed time offset—decides whether a later keyframe really exists. */
static inline bool psp_media_refusal_resume_has_room(
    uint64_t position_us, uint64_t duration_us)
{
    return duration_us != 0 && position_us < duration_us;
}
#define PSP_MEDIA_AUDIO_SAMPLES 1024u
#define PSP_MEDIA_AUDIO_QUEUE_SLOTS 24u
#define PSP_MEDIA_AUDIO_MAXIMUM_RATE 48000u

/*
 * How many AAC access units one worker job may decode back to back, and why
 * the answer is two rather than "as many as are ready".
 *
 * The soak that motivated this measured the shared job slot directly: audio
 * owned 27.9 seconds of it against video's 24.5, over 3,174 audio jobs
 * averaging 8.8ms each -- of which the firmware call is only about 1.1ms.
 * The rest is the round trip: the dispatch to the worker and, overwhelmingly,
 * the wait for the browser thread to come back and collect (7.9ms mean across
 * all jobs). Audio was refusing video submissions 1,474 times to video's own
 * 314, and still delivering 26 blocks a second where 43 are needed. The work
 * is not the problem; the number of round trips is.
 *
 * Two halves the round trips. It does not reduce the AAC firmware work at all
 * -- two access units cost two decodes wherever they run -- so the win is
 * bounded by the overhead share, which is the honest reason not to reach for
 * a larger number and call it a bigger win.
 *
 * And two is the ceiling for a reason of its own. 2 x 1,024 samples is about
 * 46ms of media at 44.1kHz, which is one video frame interval. A job that
 * holds the one shared slot for longer than the video pipeline's own period
 * starts refusing video submissions for more than a frame at a time, which is
 * the very cost being removed. Do not raise it.
 */
#define PSP_MEDIA_AUDIO_BATCH_MAXIMUM 2u

/*
 * The per-access-unit staging ceiling, and so half the audio staging buffer.
 *
 * AAC-LC's own maximum is 768 bytes per channel -- 1,536 for the admitted
 * stereo configuration -- so this is a factor of 2.7 above anything the
 * format can produce. It is deliberately generous because it is a REFUSAL
 * threshold on a path that used to accept anything the shared packet buffer
 * could hold: a stream that plays today must not stop playing because a
 * batching optimisation introduced a tighter bound than the codec has.
 */
#define PSP_MEDIA_AUDIO_AU_BYTES 4096u
#define PSP_MEDIA_AUDIO_STAGING_BYTES \
    (PSP_MEDIA_AUDIO_AU_BYTES * PSP_MEDIA_AUDIO_BATCH_MAXIMUM)

/* Browser-owned compressed lookahead. Four access units are under 100ms at
   44.1kHz: enough to absorb one busy codec job and form the next two-unit
   batch, but too small to become a second unbounded playback pipeline. */
#define PSP_MEDIA_AUDIO_PENDING_SLOTS 4u
#define PSP_MEDIA_AUDIO_PENDING_BYTES \
    (PSP_MEDIA_AUDIO_AU_BYTES * PSP_MEDIA_AUDIO_PENDING_SLOTS)
#define PSP_MEDIA_AUDIO_PENDING_HOLD_US 30000u
#define PSP_MEDIA_AUDIO_PENDING_LOW_WATER 4u

/*
 * The worker's own bound on a batched job.
 *
 * Every guard on extending a job is checked by the browser thread before the
 * job is queued, on facts that were true then. This is the one the worker
 * checks for itself, between the two decodes, because it is the only party
 * that knows what the first one actually cost: a firmware call that ran long
 * -- the measured maximum was 3.2ms, but the job maximum was 342ms -- must
 * not be followed by a second one on the same slot. Past this the worker
 * stops and leaves the remaining access unit staged for the next job, which
 * is a state the flush path already handles.
 *
 * Twelve milliseconds is roughly a third of a browser frame and four times
 * the measured firmware cost of two AAC decodes.
 */
#define PSP_MEDIA_AUDIO_BATCH_BUDGET_US 12000u

/* A lone compressed unit may wait for its partner only while playback already
   has a safe PCM cushion. The count and age bounds keep end-of-stream and
   network stalls from retaining it indefinitely. */
static inline bool psp_media_audio_pending_should_wait(
    unsigned pending, unsigned queue_depth, bool playing, uint32_t age_us)
{
    return pending == 1u && playing
        && queue_depth >= PSP_MEDIA_AUDIO_PENDING_LOW_WATER
        && age_us < PSP_MEDIA_AUDIO_PENDING_HOLD_US;
}
#define PSP_MEDIA_DDR_BYTES (2u * 1024u * 1024u)
#define PSP_MEDIA_DDR_ALIGNMENT (4u * 1024u * 1024u)
#define PSP_MEDIA_MAXIMUM_PACKET_BYTES (512u * 1024u)
#define PSP_MEDIA_DEFAULT_ME_BOOT_TYPE 3
/* The shortest firmware SRC rate used by AAC is 8,000 Hz, whose 1,024-sample
   hardware block lasts 128 ms. Leave scheduler margin before declaring the
   blocking output call wedged; ordinary YouTube 44.1/48 kHz blocks still
   quiesce in roughly 21-23 ms. */
#define PSP_MEDIA_AUDIO_RESET_WAIT_US 200000u
#define PSP_MEDIA_VIDEO_TIMESTAMP_SLOTS 16u
/*
 * Keep the raw-NAL diagnostic no stricter than the timestamp queue which
 * already bounds the decoder's supported reorder depth. Frame rate and SPS
 * reference depth are not admission properties today: deriving a smaller
 * threshold from a nominal 24 fps floor rejected valid reordered streams,
 * while 23.976 fps streams did not even supply the derived packet count inside
 * the 500 ms horizon. A lower-rate stream may therefore reach the ordinary
 * bounded first-frame watchdog before this diagnostic; that is preferable to
 * falsely declaring a functioning firmware bridge unavailable.
 */
#define PSP_MEDIA_RAW_NAL_PROBE_PACKETS PSP_MEDIA_VIDEO_TIMESTAMP_SLOTS
#define PSP_MEDIA_VIDEO_DRAIN_CALLS 1u
#define PSP_MEDIA_VIDEO_DRAIN_SURFACE_POLLS 3u
#define PSP_MEDIA_240P_FRAME_STRIDE 512u
#define PSP_MEDIA_240P_FRAME_ROWS 272u
#define PSP_MEDIA_360P_MAX_WIDTH 640u
#define PSP_MEDIA_360P_MAX_HEIGHT 360u
#define PSP_MEDIA_360P_FRAME_STRIDE 768u
#define PSP_MEDIA_360P_FRAME_ROWS 368u
#define PSP_MEDIA_MPEG_DECODE_WIDTH 512u
#define PSP_MEDIA_AVC_PROFILE_BASELINE 0x42u
#define PSP_MEDIA_AVC_PROFILE_MAIN 0x4du
#define PSP_MEDIA_AVC_MAXIMUM_LEVEL 30u
/*
 * Decoded output surfaces the pipeline owns.
 *
 * Policy rather than pool bookkeeping, and defined here rather than beside the
 * carve, because it decides two things that must never drift apart: the memory
 * the pool reserves, and the working set the page Budget charges a stream for.
 * These values must not drift: if physical memory grows without the matching
 * engine charge, a page can be admitted against a working set a megabyte
 * smaller than the one it actually takes.
 */
#define PSP_MEDIA_SURFACE_SLOTS 2u
#define PSP_MEDIA_240P_SURFACE_BYTES \
    ((size_t) PSP_MEDIA_240P_FRAME_STRIDE * PSP_MEDIA_240P_FRAME_ROWS * 4u)
#define PSP_MEDIA_360P_SURFACE_BYTES \
    ((size_t) PSP_MEDIA_360P_FRAME_STRIDE * PSP_MEDIA_360P_FRAME_ROWS * 4u)
/*
 * What a stream is charged against the page budget. The historical values
 * counted exactly one surface along with the DDR arena, workspace, staging and
 * audio blocks; every slot beyond the first is added from the same geometry
 * the carve uses, so a change to the slot count moves the charge with it and
 * neither can be updated alone.
 */
#define PSP_MEDIA_240P_EXTERNAL_RESERVE                                      \
    ((size_t) (3584u * 1024u)                                                \
     + PSP_MEDIA_240P_SURFACE_BYTES * (PSP_MEDIA_SURFACE_SLOTS - 1u))
#define PSP_MEDIA_360P_EXTERNAL_RESERVE                                      \
    ((size_t) (4u * 1024u * 1024u)                                           \
     + PSP_MEDIA_360P_SURFACE_BYTES * (PSP_MEDIA_SURFACE_SLOTS - 1u))
#define PSP_MEDIA_SURFACE_CANARY_A UINT32_C(0x5aa5f00d)
#define PSP_MEDIA_SURFACE_CANARY_B UINT32_C(0xa55a0ff0)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "psp_module_policy.h"

/*
 * The one experimental decoder-program knob, spelled
 * `experimental_wide_video=` in boot.cfg. Its name is historical: it now
 * selects any decoder program the default build refuses to attempt, because
 * every such program is one hardware has already rejected.
 *
 * `wide` is the shipping default after its 360p presentation, lifecycle and
 * network-promotion gates passed on a PSP-3000. The absent spelling selects
 * it; explicit `off` remains the safe escape hatch and the process latch
 * still falls back to 240p after a first-access-unit rejection.
 *
 * `wide` restores the wide Main-profile program (sceMpeg create mode 5 /
 * Media Engine boot type 1) transcribed from PMPlayer, which a PSP-3000
 * rejects at the raw-NAL bridge -- sceMpegGetAvcNalAu returns 0xFFFFFFFF for
 * a structurally valid IDR. `wide-annexb` additionally rewrites the submitted
 * AVCC length prefixes to Annex-B start codes.
 *
 * `boot4` restores the historical Baseline program (create mode 4 / boot type
 * 4) for A/B against the boot-call-free default described at
 * psp_media_decoder_policy. It says nothing about wide pictures: those stay
 * clamped exactly as they are with the knob off, so `boot4` changes one
 * decision and only that one.
 *
 * `edram-real` and `no-boot` each restore one half of the pre-PMPlayer
 * behaviour the default no longer has:
 *
 * - `edram-real` calls the firmware's own sceAudiocodecGetEDRAM/ReleaseEDRAM
 *   pair for the AAC work buffer. The default is PMPlayer's substitute, which
 *   never calls either: cooleyesAudiocodecGetEDRAM allocates Buffer[4] bytes
 *   of ordinary 64-aligned storage and writes the pointer into Buffer[3].
 *   PMPlayer is the hardware-proven implementation and it bypasses the real
 *   grant deliberately, so the grant is the suspect, not the substitute.
 * - `no-boot` restores the assumption that the Media Engine is already
 *   running program type 3 when the process starts, so a first open that
 *   selects type 3 makes no sceMeBootStart call at all. The default makes one
 *   real call per process for whatever type the policy chose, because
 *   PMPlayer only ever short-circuits type 3 AFTER a real boot call has
 *   succeeded -- an assumption Tilefinch never earned.
 */
typedef enum {
    PSP_MEDIA_WIDE_PROGRAM_OFF = 0,
    PSP_MEDIA_WIDE_PROGRAM_WIDE = 1,
    PSP_MEDIA_WIDE_PROGRAM_WIDE_ANNEXB = 2,
    /* Not a wide selection. Named inside this enum because there is exactly
       one boot.cfg key for "attempt a program the default build will not". */
    PSP_MEDIA_WIDE_PROGRAM_BASELINE_BOOT4 = 3,
    /* Nor are these. Same key, same reason: one spelling, one act to flip. */
    PSP_MEDIA_WIDE_PROGRAM_EDRAM_REAL = 4,
    PSP_MEDIA_WIDE_PROGRAM_NO_BOOT = 5
} PspMediaWideProgram;

static inline PspMediaWideProgram psp_media_wide_program_from_name(
    const char *name)
{
    if (name == NULL) return PSP_MEDIA_WIDE_PROGRAM_OFF;
    if (strcmp(name, "wide") == 0) return PSP_MEDIA_WIDE_PROGRAM_WIDE;
    if (strcmp(name, "wide-annexb") == 0)
        return PSP_MEDIA_WIDE_PROGRAM_WIDE_ANNEXB;
    if (strcmp(name, "boot4") == 0)
        return PSP_MEDIA_WIDE_PROGRAM_BASELINE_BOOT4;
    if (strcmp(name, "edram-real") == 0)
        return PSP_MEDIA_WIDE_PROGRAM_EDRAM_REAL;
    if (strcmp(name, "no-boot") == 0)
        return PSP_MEDIA_WIDE_PROGRAM_NO_BOOT;
    return PSP_MEDIA_WIDE_PROGRAM_OFF;
}

/* Resolve the boot setting rather than merely parsing a spelling. Keeping
   this separate preserves `from_name`'s fail-closed answer for unknown input
   while making the absent, validated config select the promoted default. */
static inline PspMediaWideProgram psp_media_wide_program_configured(
    const char *name)
{
    return name == NULL || name[0] == '\0'
        ? PSP_MEDIA_WIDE_PROGRAM_WIDE
        : psp_media_wide_program_from_name(name);
}

static inline const char *psp_media_wide_program_name(int mode)
{
    if (mode == PSP_MEDIA_WIDE_PROGRAM_WIDE) return "wide";
    if (mode == PSP_MEDIA_WIDE_PROGRAM_WIDE_ANNEXB) return "wide-annexb";
    if (mode == PSP_MEDIA_WIDE_PROGRAM_BASELINE_BOOT4) return "boot4";
    if (mode == PSP_MEDIA_WIDE_PROGRAM_EDRAM_REAL) return "edram-real";
    if (mode == PSP_MEDIA_WIDE_PROGRAM_NO_BOOT) return "no-boot";
    return "off";
}

/* True for a boot.cfg spelling this build understands, including the absent
   and explicit-off spellings. Anything else is a typo the loader rejects
   rather than silently reading as "off". */
static inline bool psp_media_wide_program_name_valid(const char *name)
{
    return name != NULL
        && (name[0] == '\0' || strcmp(name, "off") == 0
            || strcmp(name, "wide") == 0
            || strcmp(name, "wide-annexb") == 0
            || strcmp(name, "boot4") == 0
            || strcmp(name, "edram-real") == 0
            || strcmp(name, "no-boot") == 0);
}

/*
 * The spellings a picker may offer, in the order it offers them. Index 0 is
 * the explicit compatibility state, and every entry is a spelling
 * psp_media_wide_program_name_valid already accepts, so no selection can
 * reach the boot-time config gate's halt.
 */
#define PSP_MEDIA_WIDE_PROGRAM_CHOICE_COUNT 6u

static inline const char *psp_media_wide_program_choice(unsigned index)
{
    switch (index) {
    case 0: return "off";
    case 1: return "wide";
    case 2: return "wide-annexb";
    case 3: return "boot4";
    case 4: return "edram-real";
    case 5: return "no-boot";
    default: return NULL;
    }
}

/* Where a stored spelling sits in that list, so a picker can open on the
   value the next boot will really use. Empty selects the promoted wide
   default; an unrecognized direct input fails closed to explicit `off`.
   Boot-config validation rejects that latter case before this helper. */
static inline unsigned psp_media_wide_program_choice_index(const char *name)
{
    if (name == NULL || name[0] == '\0')
        return (unsigned) PSP_MEDIA_WIDE_PROGRAM_WIDE;
    for (unsigned index = 0;
         index < PSP_MEDIA_WIDE_PROGRAM_CHOICE_COUNT; index++) {
        const char *choice = psp_media_wide_program_choice(index);
        if (choice != NULL && name != NULL && strcmp(name, choice) == 0)
            return index;
    }
    return 0;
}

static inline bool psp_media_wide_program_enabled(int mode)
{
    return mode == PSP_MEDIA_WIDE_PROGRAM_WIDE
        || mode == PSP_MEDIA_WIDE_PROGRAM_WIDE_ANNEXB;
}

static inline bool psp_media_wide_program_annexb(int mode)
{
    return mode == PSP_MEDIA_WIDE_PROGRAM_WIDE_ANNEXB;
}

/* The Baseline A/B: restore the Media Engine boot call the default build no
   longer makes. Only `boot4` asks for it; the wide spellings are about a
   different decision and must not drag this one along. */
static inline bool psp_media_baseline_boot_call_enabled(int mode)
{
    return mode == PSP_MEDIA_WIDE_PROGRAM_BASELINE_BOOT4;
}

/* The EDRAM A/B. False -- the default -- means the AAC work buffer comes from
   PMPlayer's substitute rather than the firmware grant, and no
   sceAudiocodecGetEDRAM or sceAudiocodecReleaseEDRAM call is made at all. */
static inline bool psp_media_real_edram_enabled(int mode)
{
    return mode == PSP_MEDIA_WIDE_PROGRAM_EDRAM_REAL;
}

/*
 * Whether the process's first media open makes a real sceMeBootStart call.
 *
 * True by default, including for the firmware's own type 3: PMPlayer's
 * lastMeBooterType starts at 3 too, but its first real-world stream was
 * always Baseline (type 4) or wide (type 1), so it always made a real call
 * through its kernel bridge before decoding anything and the type-3
 * short-circuit only ever ran after one had succeeded. Tilefinch's default
 * program IS type 3, so the same initializer meant the Media Engine may never
 * have been given the codec program at all. `no-boot` restores that.
 */
static inline bool psp_media_cold_boot_call_enabled(int mode)
{
    return mode != PSP_MEDIA_WIDE_PROGRAM_NO_BOOT;
}

/*
 * The single geometry predicate every wide-program decision reads. The proven
 * firmware program (create mode 4 / ME boot type 3) covers everything that
 * fits the PSP screen; anything larger needs the rejected wide program.
 */
static inline bool psp_media_wide_program_required(
    unsigned width, unsigned height)
{
    return width > 480u || height > 272u;
}

typedef struct {
    uint64_t pts_us;
    uint64_t duration_us;
} PspMediaVideoTimestamp;

typedef struct {
    PspMediaVideoTimestamp entries[PSP_MEDIA_VIDEO_TIMESTAMP_SLOTS];
    unsigned count;
} PspMediaVideoTimestampQueue;

typedef struct {
    unsigned stride_pixels;
    unsigned surface_rows;
    size_t surface_bytes;
    size_t external_reserve_bytes;
} PspMediaSurfacePolicy;

typedef struct {
    int mpeg_mode;
    int me_boot_type;
} PspMediaDecoderPolicy;

/*
 * Who owns a decoded-output slot, and the only four answers there are.
 *
 * The rule the whole machine exists to enforce: a slot that is claimed and a
 * slot that is writable are disjoint sets. The single-surface pipeline
 * enforced it by refusing to submit at all, which is what capped the rate; two
 * slots enforce it per slot instead, and every transition below is a statement
 * about exactly one of them.
 *
 *   FREE       nobody owns it. The only state a conversion may target.
 *   ME_WRITING the Media Engine's colour conversion is writing it. Reached
 *              only from FREE, and the generation is bumped on the way.
 *   READY      it holds a converted picture nobody has claimed.
 *   READING    the browser thread claimed it. Covers every reader there is --
 *              the DMA staging copy, a graphics-engine draw sampling the
 *              surface directly, the software scaler, the seek-preview copy
 *              and the staging re-copy after a failed join -- because they are
 *              one ownership question and answering it once is what keeps them
 *              from disagreeing.
 *
 * When READING ends is a property of the PATH, not of the moment, and getting
 * that wrong is worth a whole soak in either direction.
 *
 * On the STAGED path it ends at the join. The picture is copied once into a
 * texture the decoder cannot reach, and every later present of that same
 * picture samples the copy -- psp_media_present_texture_for skips the staging
 * branch entirely when the identity has not moved -- so after the copy is
 * joined nothing will ever read the slot again. Holding it longer bought
 * nothing and cost the rate: with the release deferred to the next claim, one
 * slot was freed per claim, so conversions could not outrun claims and the
 * pipeline ran at browser-frame rate rather than at the stream's.
 *
 * On the UNSTAGED paths -- the graphics-engine draw that samples the surface
 * directly, the software scaler, the seek-preview copy -- it still ends at the
 * next successful claim, and must. The browser loop redraws whatever it last
 * took whenever no newer picture is due, so a slot freed at the reader's
 * release could be converted into while a re-present still pointed at it:
 * not a tear, but the successor displayed under the claimed picture's
 * identity and timestamp, which is the silent substitution this design is
 * answerable for.
 *
 * And one case ends neither way. A staged copy whose join TIMED OUT was
 * abandoned rather than stopped, so the reader is still live with nothing
 * tracking it; that slot is quarantined and is refused to its writer by the
 * claim, by the release and by the reader quiesce until the presenter has
 * observed the transfer finish. See PspMediaPresentDmaJoin.
 */
typedef enum {
    PSP_MEDIA_SLOT_FREE = 0,
    PSP_MEDIA_SLOT_ME_WRITING = 1,
    PSP_MEDIA_SLOT_READY = 2,
    PSP_MEDIA_SLOT_READING = 3
} PspMediaSurfaceSlotState;

#define PSP_MEDIA_SLOT_STATES 4u

/*
 * How long each slot spent in each of those four states.
 *
 * Every rate question this pipeline has left is a question about occupancy,
 * and the counters that exist answer it only in events: how many conversions,
 * how many claims, how many refusals. Two slots that alternate perfectly and
 * two slots that spend nine tenths of the window READY while the decoder waits
 * for somewhere to put a picture produce the same conversion count. The
 * difference is entirely in the dwell, so measure the dwell.
 *
 * One 32-bit stamp and four 64-bit accumulators per slot, charged at each
 * transition and nowhere else -- about fifty charges a second between them.
 * The stamp is a sceKernelGetSystemTimeLow reading, so the subtraction is
 * modular and correct across the counter's 71-minute wrap for any interval
 * shorter than that; a slot held longer than 71 minutes is not a case this
 * pipeline can reach, and would be visible as a stopped rate long before.
 */
typedef struct {
    uint32_t since_us;
    uint64_t state_us[PSP_MEDIA_SLOT_STATES];
} PspMediaSlotDwell;

/* Charge the interval since the last charge to `state`, and re-arm. Called
   with the state being LEFT, immediately before the transition is written. */
static inline void psp_media_slot_dwell_charge(
    PspMediaSlotDwell *dwell, int state, uint32_t now_us)
{
    if (dwell == NULL) return;
    if (state >= 0 && state < (int) PSP_MEDIA_SLOT_STATES)
        dwell->state_us[state] += (uint64_t) (now_us - dwell->since_us);
    dwell->since_us = now_us;
}

/*
 * The session epoch: which seek/reset generation a piece of in-flight work
 * belongs to.
 *
 * Copied into every submitted job rather than shared as one atomic, because a
 * 64-bit load on this 32-bit target is two instructions and a reader can see
 * half of each of two values. A copy is read by exactly one thread and is
 * whole by construction.
 *
 * Zero is reserved and is never a live epoch, so a job structure that was
 * never stamped -- or one whose stamp was lost -- reads as invalid and its
 * completion is discarded rather than credited to the stream that replaced it.
 * The advance below therefore skips zero on wrap. The width mask exists so a
 * host test can force that wrap in a few iterations instead of the 2^64 a
 * device would need, and is UINT64_MAX everywhere else.
 */
#define PSP_MEDIA_EPOCH_INVALID UINT64_C(0)
#define PSP_MEDIA_EPOCH_FIRST UINT64_C(1)
#define PSP_MEDIA_EPOCH_WIDTH_MASK UINT64_C(0xFFFFFFFFFFFFFFFF)

static inline bool psp_media_epoch_valid(uint64_t epoch)
{
    return epoch != PSP_MEDIA_EPOCH_INVALID;
}

static inline uint64_t psp_media_epoch_advance(
    uint64_t epoch, uint64_t width_mask)
{
    /* A mask with no room to represent a live epoch cannot produce one. Answer
       the reserved value, which every consumer already refuses. */
    if (width_mask == 0) return PSP_MEDIA_EPOCH_INVALID;
    uint64_t next = (epoch + UINT64_C(1)) & width_mask;
    return next == PSP_MEDIA_EPOCH_INVALID ? PSP_MEDIA_EPOCH_FIRST : next;
}

/* Whether a completion carrying `carried` still belongs to the session. Both
   halves must be live: an unstamped job and a backend that has not opened an
   epoch are each a reason to discard, not to compare. */
static inline bool psp_media_epoch_current(uint64_t carried, uint64_t session)
{
    return psp_media_epoch_valid(carried) && psp_media_epoch_valid(session)
        && carried == session;
}

typedef struct {
    /*
     * The ownership word, and the ONLY field of this structure two threads
     * touch without a lock between them.
     *
     * Everything else here is written by whichever thread owns the slot and
     * read by whichever thread the state word has handed it to, so the state
     * word is what orders them. It is atomic for that reason and no other:
     * the four transitions split into two disjoint domains -- the codec
     * worker drives FREE -> ME_WRITING -> READY, the browser thread drives
     * READY -> READING -> FREE -- and each domain ends by publishing the
     * token the other one is waiting for.
     *
     * So each publish is a release store made AFTER every field it describes
     * is written, and each observation is an acquire load made BEFORE any of
     * those fields is read. psp_media_slot_publish and the accessors below
     * are the only permitted forms; a plain assignment to this field would
     * compile and would silently be the data race this exists to remove.
     *
     * Until the global take-vs-job gate came out, the ordering came from the
     * codec job handoff instead -- the worker release-stored DONE, the
     * browser acquire-loaded it, and a take was refused for the whole of any
     * running job. That gate refused about 13% of takes over a soak because
     * SOME job was running, including jobs writing the other slot. Per-slot
     * release/acquire is the same guarantee at the granularity the two-slot
     * pipeline actually needs.
     */
    _Atomic int state;
    /* Bumped every time the slot leaves FREE, so a reader that captured
       {slot, generation} can tell the picture it meant from the one now
       living at the same address. */
    uint32_t generation;
    uint64_t epoch;
    uint64_t identity;
    uint64_t pts_us;
    uint64_t duration_us;
    /* Monotonic decode order -- batch number then index within it -- which is
       what breaks a tie the presentation time cannot. */
    uint64_t sequence;
    uint32_t emitted_us;
    /* Validation builds: a hash of fixed sample points taken straight after
       the conversion, re-taken after staging. Right metadata over the wrong
       pixels is the one failure every counter in this file would call a
       success. */
    uint32_t signature;
    /* Per slot, because arming and checking have to name the same one. A
       canary armed on slot 0 and proven by a conversion into slot 1 proves
       nothing at all -- and would hide exactly the mis-slotted write the
       proof exists to catch. */
    bool extent_validated;
    bool canary_armed;
} PspMediaSurfaceSlot;

/*
 * The three permitted forms of touching that word.
 *
 * `observe` is for a caller about to read the fields the state describes: it
 * acquires, so every metadata store the publishing thread made before its
 * release store is visible afterwards. `peek` is for a caller that only wants
 * to count states or pick a target it will re-check -- occupancy telemetry and
 * admission heuristics -- and pays no barrier for an answer it does not act on
 * field-by-field. `publish` is the release store that hands the slot over.
 *
 * The const on `observe`/`peek` is deliberate: the helpers below take a const
 * slot pointer because reading a state must never be mistaken for owning one.
 */
static inline int psp_media_slot_observe(const PspMediaSurfaceSlot *slot)
{
    return slot == NULL ? PSP_MEDIA_SLOT_FREE
        : atomic_load_explicit(&slot->state, memory_order_acquire);
}

static inline int psp_media_slot_peek(const PspMediaSurfaceSlot *slot)
{
    return slot == NULL ? PSP_MEDIA_SLOT_FREE
        : atomic_load_explicit(&slot->state, memory_order_relaxed);
}

/*
 * Hand the slot to the other domain.
 *
 * Every caller must have finished writing the fields this state describes --
 * for READY that is the whole picture, for FREE that is the erasure of the one
 * before it. A publish that runs before those stores would let the receiving
 * thread act on a slot whose metadata is still moving, which is the one
 * substitution the pixel signatures exist to catch and the one this ordering
 * exists to prevent.
 */
static inline void psp_media_slot_publish(PspMediaSurfaceSlot *slot, int state)
{
    if (slot == NULL) return;
    atomic_store_explicit(&slot->state, state, memory_order_release);
}

/*
 * Which of two READY pictures is earlier.
 *
 * Presentation time decides. A tie falls back to decode order, and so does a
 * timestamp of zero: zero is the timestamp queue's unset value rather than a
 * position in the stream, and sorting on it would put an unnamed picture ahead
 * of every named one. Decode order is monotonic and never absent, which is the
 * whole reason it is the fallback.
 */
static inline bool psp_media_slot_precedes(
    const PspMediaSurfaceSlot *a, const PspMediaSurfaceSlot *b)
{
    if (a == NULL || b == NULL) return false;
    if (a->pts_us == 0 || b->pts_us == 0 || a->pts_us == b->pts_us)
        return a->sequence < b->sequence;
    return a->pts_us < b->pts_us;
}

/*
 * A picture this reader may claim. The acquire is what makes the epoch read
 * below -- and every other field the caller goes on to read -- the values the
 * converting thread published, rather than whatever happened to be in the
 * structure while it was still writing them.
 */
static inline bool psp_media_slot_holds_picture(
    const PspMediaSurfaceSlot *slot, uint64_t epoch)
{
    return slot != NULL
        && psp_media_slot_observe(slot) == PSP_MEDIA_SLOT_READY
        && psp_media_epoch_current(slot->epoch, epoch);
}

/*
 * The picture the browser thread must claim next, or -1.
 *
 * Earliest first, unconditionally. The caller may still refuse what this
 * returns -- a picture whose time has not come is waited for -- but it must
 * never answer that refusal by claiming a later one instead: displaying
 * out of order is worse than displaying late, and skipping the earliest is
 * how a pipeline silently reorders a stream.
 */
static inline int psp_media_slot_take_index(
    const PspMediaSurfaceSlot *slots, unsigned count, uint64_t epoch)
{
    int best = -1;
    int writing = -1;
    if (slots == NULL) return -1;
    /* Discover, then validate. If the discovery pass sees no READY picture,
       wait for the next browser visit; accepting a candidate that appeared
       only behind that scan is what permitted a newer slot to leapfrog an
       older one. Once discovery sees a candidate it remains READY until this
       sole consumer returns, so the validation pass has a stable incumbent;
       only the other slot can begin behind it, with a later sequence. Four
       acquire loads is fixed work for the two-slot pool. */
    for (unsigned pass = 0; pass < 2u; pass++) {
        int pass_best = -1;
        int pass_writing = -1;
        for (unsigned at = 0; at < count; at++) {
            int state = psp_media_slot_observe(&slots[at]);
            if (!psp_media_epoch_current(slots[at].epoch, epoch)) continue;
            if (state == PSP_MEDIA_SLOT_READY) {
                if (pass_best < 0
                    || psp_media_slot_precedes(
                           &slots[at], &slots[pass_best]))
                    pass_best = (int) at;
            } else if (state == PSP_MEDIA_SLOT_ME_WRITING) {
                /* Ordering metadata is published before ME_WRITING. */
                if (pass_writing < 0
                    || psp_media_slot_precedes(
                           &slots[at], &slots[pass_writing]))
                    pass_writing = (int) at;
            }
        }
        if (pass == 0u && pass_best < 0) return -1;
        best = pass_best;
        writing = pass_writing;
    }
    /* The producer can publish an older conversion after its slot was scanned
       but before a newer READY slot was selected. Waiting for that specific
       predecessor prevents an out-of-order claim without restoring the old
       global rule that every conversion blocks every take. */
    if (best >= 0 && writing >= 0
        && psp_media_slot_precedes(&slots[writing], &slots[best]))
        return -1;
    return best;
}

/* A/V catch-up is deliberately a one-picture decision. A READY successor is
   the proof that dropping the late head will not expose a hole, and requiring
   that successor to be due prevents a timestamp discontinuity from discarding
   the last useful picture. One browser frame can therefore retire at most one
   stale picture, while stream order remains earliest-first. */
#define PSP_MEDIA_VIDEO_LATE_DROP_US 100000u
/* A cold presenter briefly has less throughput while its first GE texture,
   DMA staging, and firmware batches settle. Give it half a second to establish
   the two-surface cadence before applying the normal 100 ms late threshold.
   The former 72-frame minimum imposed at least three seconds on 24 fps
   content, and its circular skew guard kept the relaxed mode active until
   frame 120 on device; audio reached 675 ms ahead before visibly accelerated
   video caught up. Twelve frames preserves the genuinely cold setup allowance
   without turning startup into a multi-second alternate timing mode. */
#define PSP_MEDIA_VIDEO_STARTUP_CATCHUP_FRAMES 12u
#define PSP_MEDIA_VIDEO_STARTUP_LATE_DROP_US 500000u

static inline bool psp_media_startup_catchup_settled(
    size_t displayed_frames)
{
    /* Do not require the relaxed 500 ms skew threshold to be satisfied here.
       That made the startup mode circular: the normal 100 ms correction could
       not engage until the 30 Hz presenter had already raced several seconds
       of video back into sync. The cold-path allowance is a bounded number of
       successfully displayed frames, not an A/V convergence policy. */
    return displayed_frames >= PSP_MEDIA_VIDEO_STARTUP_CATCHUP_FRAMES;
}

/* Startup has one already-presented still plus this two-slot decoded queue.
   Releasing audio any earlier recreates the race where its cursor establishes
   the master clock before ordinary video cadence exists. */
#define PSP_MEDIA_STARTUP_READY_FRAMES PSP_MEDIA_SURFACE_SLOTS

static inline bool psp_media_startup_preroll_ready(unsigned ready_frames)
{
    return ready_frames >= PSP_MEDIA_STARTUP_READY_FRAMES;
}

/* Advance audible time by elapsed device time, never by how quickly firmware
   accepted its startup queue. Submitted audio remains the hard upper bound;
   retaining the clamped value prevents a jump when supply resumes. */
static inline uint64_t psp_media_audio_cursor_advance_us(
    uint64_t elapsed_us, uint32_t delta_us, bool running,
    uint64_t accepted_us)
{
    if (running)
        elapsed_us = delta_us > UINT64_MAX - elapsed_us
            ? UINT64_MAX : elapsed_us + delta_us;
    return elapsed_us > accepted_us ? accepted_us : elapsed_us;
}

static inline bool psp_media_video_should_drop_late(
    uint64_t clock_us,
    const PspMediaSurfaceSlot *current,
    const PspMediaSurfaceSlot *successor,
    bool startup_catchup)
{
    uint64_t late_limit_us =
        startup_catchup
        ? PSP_MEDIA_VIDEO_STARTUP_LATE_DROP_US
        : PSP_MEDIA_VIDEO_LATE_DROP_US;
    if (current == NULL || successor == NULL
        || current->pts_us == 0 || successor->pts_us == 0
        || !psp_media_slot_precedes(current, successor)
        || clock_us <= current->pts_us
        || clock_us - current->pts_us <= late_limit_us)
        return false;
    if (successor->pts_us <= clock_us) return true;
    return successor->duration_us != 0
        && successor->pts_us - clock_us <= successor->duration_us / 2u;
}

/* The slot the next conversion may target, or -1 for "the pipeline is full".
   Lowest index first, so a two-slot pool alternates predictably and a trace
   reader can follow it.

   Acquired, because this is the receiving half of the browser thread's
   hand-back: the caller is about to write the slot, and it must first see the
   erasure of the picture that was there. A relaxed answer could let a
   conversion's identity store be overwritten by the release's trailing
   clear. */
static inline int psp_media_slot_free_index(
    const PspMediaSurfaceSlot *slots, unsigned count)
{
    if (slots == NULL) return -1;
    for (unsigned at = 0; at < count; at++)
        if (psp_media_slot_observe(&slots[at]) == PSP_MEDIA_SLOT_FREE)
            return (int) at;
    return -1;
}

/* Occupancy, not ownership: nothing acts on a field of the slots it counts,
   so these peek. */
static inline unsigned psp_media_slot_free_count(
    const PspMediaSurfaceSlot *slots, unsigned count)
{
    unsigned free_slots = 0;
    if (slots == NULL) return 0;
    for (unsigned at = 0; at < count; at++)
        if (psp_media_slot_peek(&slots[at]) == PSP_MEDIA_SLOT_FREE)
            free_slots++;
    return free_slots;
}

/* Slots holding a converted picture nobody has claimed, of this epoch or any
   other. The drain policy asks this rather than the single frame_ready flag it
   used to, and a stale-epoch picture is still occupancy. */
static inline unsigned psp_media_slot_ready_count(
    const PspMediaSurfaceSlot *slots, unsigned count)
{
    unsigned ready = 0;
    if (slots == NULL) return 0;
    for (unsigned at = 0; at < count; at++)
        if (psp_media_slot_peek(&slots[at]) == PSP_MEDIA_SLOT_READY) ready++;
    return ready;
}

/*
 * A picture's pixels, in one word.
 *
 * Every other check in this pipeline is on metadata: identities match, slots
 * match, generations match, counts balance. All of them pass for a picture
 * whose bytes are the wrong picture's -- which is precisely the defect a
 * two-slot pipeline can introduce and a one-slot one could not. So sample the
 * pixels themselves at a fixed, tiny set of points, once when the conversion
 * finishes and again when the staging copy that will actually be drawn has
 * landed, and compare.
 *
 * Twelve points rather than a full hash because this runs inside the frame it
 * is measuring: twelve reads cost nothing and a wrong picture differs at
 * almost every pixel, so the miss rate against a real substitution is
 * negligible. Locations are derived from the display size alone, so the two
 * ends agree without sharing anything but the geometry -- the source is read
 * at the surface stride, the staged copy at the texture stride, and the points
 * are the same points.
 */
#define PSP_MEDIA_SIGNATURE_SAMPLES 12u

static inline void psp_media_signature_point(
    unsigned index, unsigned width, unsigned height,
    unsigned *x, unsigned *y)
{
    /* Odd multiples of half a cell keep every point strictly inside the
       rectangle, and the coprime stride on the vertical axis stops the twelve
       from landing on one diagonal. */
    unsigned column = (2u * index + 1u);
    unsigned row = (2u * ((5u * index + 2u) % PSP_MEDIA_SIGNATURE_SAMPLES)
                    + 1u);
    *x = (width * column) / (2u * PSP_MEDIA_SIGNATURE_SAMPLES);
    *y = (height * row) / (2u * PSP_MEDIA_SIGNATURE_SAMPLES);
    if (*x >= width) *x = width - 1u;
    if (*y >= height) *y = height - 1u;
}

/* Zero means "not taken": a signature is only ever compared against another
   signature of the same picture, and a geometry too small to sample is one
   this check has nothing to say about. */
static inline uint32_t psp_media_surface_signature(
    const uint32_t *pixels, unsigned width, unsigned height,
    unsigned stride_pixels)
{
    if (pixels == NULL || width == 0 || height == 0
        || stride_pixels < width) return 0;
    uint32_t hash = UINT32_C(2166136261);
    for (unsigned index = 0; index < PSP_MEDIA_SIGNATURE_SAMPLES; index++) {
        unsigned x = 0;
        unsigned y = 0;
        psp_media_signature_point(index, width, height, &x, &y);
        uint32_t pixel = pixels[(size_t) y * stride_pixels + x];
        for (unsigned byte = 0; byte < 4u; byte++) {
            hash ^= (pixel >> (byte * 8u)) & 0xffu;
            hash *= UINT32_C(16777619);
        }
    }
    /* Fold the reserved "absent" value onto a neighbour rather than letting a
       legitimate all-zero sample read as no measurement at all. */
    return hash == 0 ? UINT32_C(1) : hash;
}

typedef enum {
    PSP_MEDIA_AUDIO_RESET_READY = 0,
    PSP_MEDIA_AUDIO_RESET_WAIT = 1,
    PSP_MEDIA_AUDIO_RESET_TIMEOUT = 2
} PspMediaAudioResetAction;

static inline bool psp_media_audio_prefer_standard_channel(
    uint32_t sample_rate)
{
    /* The ordinary PSP audio channels run at 44.1 kHz. The common target AAC
       track already has that rate, so prefer one of the eight
       independently reservable channels instead of depending on the single
       process-global SRC channel. Other admitted rates still require SRC. */
    return sample_rate == 44100u;
}

/*
 * Keep the ordinary playback horizon tight, but let a newly opened H.264
 * stream submit a bounded prefix before audio presentation starts. Main
 * profile commonly delays its first picture until later reference frames
 * arrive. A 50 ms horizon could therefore stop before the first picture,
 * while its three decoded AAC blocks filled the paused queue and prevented
 * any later video packet from being selected. The caller still submits at
 * most two packets per pump; this only moves the bounded horizon.
 */
static inline uint64_t psp_media_decode_clock_us(
    uint64_t clock_us, bool awaiting_first_frame)
{
    uint64_t preroll_clock =
        PSP_MEDIA_FIRST_FRAME_PREROLL_US > PSP_MEDIA_DECODE_LEAD_US
            ? PSP_MEDIA_FIRST_FRAME_PREROLL_US
                - PSP_MEDIA_DECODE_LEAD_US
            : 0;
    return awaiting_first_frame && clock_us < preroll_clock
        ? preroll_clock : clock_us;
}

/* A seek floor freezes the public clock at an arbitrary authored time. If the
   first retained frame starts just over half a frame later, the ordinary due
   test would reject it forever and both decoded slots would fill. Admit only
   the already-decoded head, only while a floor is active; the caller releases
   the floor as soon as that frame is claimed. */
static inline uint64_t psp_media_seek_take_clock_us(
    uint64_t clock_us, uint64_t floor_us,
    bool have_ready_frame, uint64_t ready_frame_us)
{
    return floor_us != 0 && have_ready_frame && ready_frame_us > clock_us
        ? ready_frame_us : clock_us;
}

static inline bool psp_media_deadline_reached(
    uint64_t started_us, uint64_t now_us, uint64_t timeout_us)
{
    return started_us != 0 && timeout_us != 0
        && now_us >= started_us
        && now_us - started_us >= timeout_us;
}

static inline bool psp_media_retry_should_resume(
    bool ui_playing, bool pending_job_resume)
{
    return ui_playing || pending_job_resume;
}

static inline bool psp_media_retry_preview_should_resume(
    bool ui_playing, bool pending_job_resume,
    bool preview_started, bool preview_was_playing)
{
    return psp_media_retry_should_resume(
               ui_playing, pending_job_resume)
        || (preview_started && preview_was_playing);
}

/*
 * A large committed rewind crosses an AAC decoder lifetime boundary.
 *
 * Real PSP firmware survived ordinary no-touch seeks, but a 420-second
 * rewind after sustained 360p playback left sceAudiocodecDecode inside the
 * Media Engine indefinitely. Recreating only MPEG state does not cover the
 * AAC program, while re-running sceAudiocodecInit against the same EDRAM has
 * itself wedged on hardware. Reopen the complete codec backend instead. The
 * 30-second threshold keeps small skips and preview scrubbing on the fast
 * reset path; previews are tentative and may issue many positions before one
 * is committed.
 */
#define PSP_MEDIA_BACKWARD_REOPEN_US UINT64_C(30000000)

static inline bool psp_media_seek_reopens_backend(
    uint64_t current_us, uint64_t target_us, bool preview)
{
    return !preview && current_us > target_us
        && current_us - target_us >= PSP_MEDIA_BACKWARD_REOPEN_US;
}

/*
 * Where a failed seek left the demuxer, which is where the presentation clock
 * has to be put so that the two describe the same position.
 *
 * A seek repositions the source first and reaches a picture second. When the
 * second step fails -- a timeout, an unavailable frame, a decode error -- the
 * source stays where the seek put it while `clock_us` still describes where
 * playback was. The device log recorded exactly that: clock 2.9 s against a
 * source buffered to 19.8 s, after which every pump exited on the eligibility
 * horizon, no packet was ever submitted again, and the session was dead until
 * the user closed it.
 *
 * The answer is the seek's own target, not the position the user should be
 * restored to. Those differ for a preview scrub -- `psp_media_recovery_position_us`
 * keeps the pre-preview position because the move is tentative until CROSS
 * confirms it -- and this is deliberately the other one: it is a statement
 * about the source, and the source moved whether or not the user meant to keep
 * it. A restore leg is the same rule read the other way; its seek pointed the
 * demuxer back at the position it is restoring.
 *
 * A prepare-phase failure leaves the demuxer's position genuinely unknown.
 * Adopting the target is still the right guess: too high a clock opens the
 * horizon and the first decoded picture pulls it back down (the resync in
 * psp_media_advance), while too low a clock is the freeze this exists to
 * prevent.
 */
static inline uint64_t psp_media_seek_failure_clock_us(
    bool restoring_preview, uint64_t target_us, uint64_t restore_us)
{
    return restoring_preview ? restore_us : target_us;
}

typedef enum {
    /* The source moved past the clock, so the horizon can never admit
       anything. Named first because it also produces the symptoms of the two
       below. */
    PSP_MEDIA_STALL_SUSPECT_CLOCK_DIVERGENCE = 0,
    PSP_MEDIA_STALL_SUSPECT_SOURCE_WINDOW,
    PSP_MEDIA_STALL_SUSPECT_DECODER_STAGING,
    PSP_MEDIA_STALL_SUSPECT_ELIGIBILITY_HORIZON
} PspMediaStallSuspect;

/*
 * Name what a pipeline that has stopped submitting is stuck on.
 *
 * The window test used to come first, and that is what made the recorded
 * clock-divergence freeze report `suspect=source-window`: a diverged clock
 * leaves the seek's own window pending forever, because nothing ever asks the
 * demuxer for the bytes that would retire it, so the window is the
 * consequence and not the cause. Test the clock against the source first --
 * that state alone is sufficient for the freeze, whatever else is also true --
 * and only then ask whether the transport or the decoder is holding things up.
 */
static inline PspMediaStallSuspect psp_media_stall_suspect(
    uint64_t clock_us, uint64_t buffered_us, size_t horizon_breaks,
    size_t submit_block_calls, bool window_pending,
    bool decode_job_pending)
{
    if (horizon_breaks != 0 && buffered_us > clock_us
        && buffered_us - clock_us > PSP_MEDIA_CLOCK_DIVERGENCE_US) {
        return PSP_MEDIA_STALL_SUSPECT_CLOCK_DIVERGENCE;
    }
    if (window_pending) return PSP_MEDIA_STALL_SUSPECT_SOURCE_WINDOW;
    return submit_block_calls != 0 && !decode_job_pending
        ? PSP_MEDIA_STALL_SUSPECT_DECODER_STAGING
        : PSP_MEDIA_STALL_SUSPECT_ELIGIBILITY_HORIZON;
}

static inline const char *psp_media_stall_suspect_name(
    PspMediaStallSuspect suspect)
{
    switch (suspect) {
        case PSP_MEDIA_STALL_SUSPECT_CLOCK_DIVERGENCE:
            return "clock-divergence";
        case PSP_MEDIA_STALL_SUSPECT_SOURCE_WINDOW: return "source-window";
        case PSP_MEDIA_STALL_SUSPECT_DECODER_STAGING:
            return "decoder-staging";
        case PSP_MEDIA_STALL_SUSPECT_ELIGIBILITY_HORIZON:
        default: return "eligibility-horizon";
    }
}

typedef enum {
    PSP_MEDIA_OPEN_WATCH_CONTINUE = 0,
    PSP_MEDIA_OPEN_WATCH_CANCELLED,
    PSP_MEDIA_OPEN_WATCH_PHASE_TIMEOUT,
    PSP_MEDIA_OPEN_WATCH_TOTAL_TIMEOUT
} PspMediaOpenWatchVerdict;

/*
 * Whether an open transaction may still run.
 *
 * Asked at the top of every open unit and, separately, on every frame in which
 * the pump does not run at all -- a page navigation owning the cooperate scope
 * suppresses the pump, and an open nobody is pumping still has to have a
 * deadline and still has to answer CIRCLE. Cancellation outranks both clocks
 * because the user asking to stop is not a failure to diagnose.
 *
 * `phase_started_us` of zero means the phase clock has not been armed yet,
 * which is the state between a phase transition and the next unit; only the
 * total applies then.
 */
static inline PspMediaOpenWatchVerdict psp_media_open_watch(
    bool cancelled, uint64_t started_us, uint64_t phase_started_us,
    uint64_t now_us)
{
    if (cancelled) return PSP_MEDIA_OPEN_WATCH_CANCELLED;
    if (psp_media_deadline_reached(
            started_us, now_us, PSP_MEDIA_OPEN_TOTAL_TIMEOUT_US)) {
        return PSP_MEDIA_OPEN_WATCH_TOTAL_TIMEOUT;
    }
    return psp_media_deadline_reached(
               phase_started_us, now_us, PSP_MEDIA_OPEN_PHASE_TIMEOUT_US)
        ? PSP_MEDIA_OPEN_WATCH_PHASE_TIMEOUT
        : PSP_MEDIA_OPEN_WATCH_CONTINUE;
}

/*
 * What a blocking read inside an open phase may still spend.
 *
 * The number handed to the range source, so that the reads a demux performs
 * share one budget instead of each re-arming the per-window timeout. Zero
 * means "no budget left", which the source turns into an immediate failure
 * rather than one more fifteen-second wait.
 */
static inline uint64_t psp_media_open_wait_budget_us(
    uint64_t started_us, uint64_t phase_started_us, uint64_t now_us)
{
    uint64_t total_left = PSP_MEDIA_OPEN_TOTAL_TIMEOUT_US;
    if (started_us != 0) {
        uint64_t spent = now_us > started_us ? now_us - started_us : 0;
        total_left = spent >= PSP_MEDIA_OPEN_TOTAL_TIMEOUT_US
            ? 0 : PSP_MEDIA_OPEN_TOTAL_TIMEOUT_US - spent;
    }
    uint64_t phase_left = PSP_MEDIA_OPEN_PHASE_TIMEOUT_US;
    if (phase_started_us != 0) {
        uint64_t spent = now_us > phase_started_us
            ? now_us - phase_started_us : 0;
        phase_left = spent >= PSP_MEDIA_OPEN_PHASE_TIMEOUT_US
            ? 0 : PSP_MEDIA_OPEN_PHASE_TIMEOUT_US - spent;
    }
    return phase_left < total_left ? phase_left : total_left;
}

/*
 * And what a blocking read inside a seek may spend.
 *
 * A seek used to spend this budget decoding forward to an exact frame, and a
 * cold window at a far offset made that the whole five seconds and then a
 * failure. It now spends it warming the connection the position it landed on
 * will read from -- best effort, so an expired budget costs the first playing
 * frames a fetch rather than costing the viewer the seek. The open's much
 * larger budget stays the open's: a viewer waiting on a scrub is not a
 * loading screen.
 */
static inline uint64_t psp_media_seek_wait_budget_us(
    uint64_t started_us, uint64_t now_us)
{
    if (started_us == 0) return PSP_MEDIA_SEEK_TIMEOUT_US;
    uint64_t spent = now_us > started_us ? now_us - started_us : 0;
    return spent >= PSP_MEDIA_SEEK_TIMEOUT_US
        ? 0 : PSP_MEDIA_SEEK_TIMEOUT_US - spent;
}

typedef enum {
    PSP_MEDIA_FIRST_FRAME_WAITING = 0,
    /* The pipeline spent the stalled window inside its bounded pump, which on
       this path can only mean an HTTP range read. */
    PSP_MEDIA_FIRST_FRAME_NETWORK_STALLED,
    PSP_MEDIA_FIRST_FRAME_DECODER_STALLED
} PspMediaFirstFrameVerdict;

/*
 * The first-frame watchdog is a progress watchdog, not a wall clock. Its
 * samples arrive through HTTP range reads over PSP Wi-Fi, where multi-second
 * stalls are ordinary; the original fixed five-second budget therefore expired
 * on healthy pipelines that were merely waiting for bytes, and then spent the
 * single 240p retry re-downloading over the same slow link. Any genuine
 * progress -- a packet submitted, a decoded frame taken, or bytes landing in a
 * range cache -- rearms `progress_us`, so the window only expires after five
 * seconds during which the pipeline produced nothing at all. `opened_us` still
 * caps the whole attempt so a wedged pipeline cannot wait forever.
 *
 * `pump_us` is the time spent inside the bounded decode pump since the last
 * progress. The codec runs on its own worker, so the only thing that can hold
 * the main thread in that pump for a substantial fraction of a stalled window
 * is a range read: that is a starved network, not a stalled decoder, and it
 * must not burn the one-shot quality fallback.
 */
static inline PspMediaFirstFrameVerdict psp_media_first_frame_verdict(
    uint64_t opened_us, uint64_t progress_us, uint64_t pump_us,
    uint64_t now_us, bool progressed)
{
    if (opened_us == 0) return PSP_MEDIA_FIRST_FRAME_WAITING;
    bool stalled = !progressed
        && psp_media_deadline_reached(
               progress_us, now_us, PSP_MEDIA_FIRST_FRAME_TIMEOUT_US);
    bool capped = psp_media_deadline_reached(
        opened_us, now_us, PSP_MEDIA_FIRST_FRAME_ABSOLUTE_US);
    if (!stalled && !capped) return PSP_MEDIA_FIRST_FRAME_WAITING;
    uint64_t window = now_us > progress_us ? now_us - progress_us : 0;
    uint64_t inside = pump_us < window ? pump_us : window;
    return inside >= PSP_MEDIA_FIRST_FRAME_NETWORK_PUMP_US
            && inside * 2u >= window
        ? PSP_MEDIA_FIRST_FRAME_NETWORK_STALLED
        : PSP_MEDIA_FIRST_FRAME_DECODER_STALLED;
}

static inline PspMediaAudioResetAction psp_media_audio_reset_action(
    bool output_in_flight, uint32_t elapsed_us)
{
    if (!output_in_flight) return PSP_MEDIA_AUDIO_RESET_READY;
    return elapsed_us < PSP_MEDIA_AUDIO_RESET_WAIT_US
        ? PSP_MEDIA_AUDIO_RESET_WAIT : PSP_MEDIA_AUDIO_RESET_TIMEOUT;
}

static inline bool psp_media_runtime_reset_after_failure(
    int last_native_error, bool runtime_initialized)
{
    return runtime_initialized && last_native_error != 0;
}

static inline bool psp_media_audio_stream_admitted(
    uint32_t track_sample_rate, uint16_t track_channels,
    uint32_t config_sample_rate, uint16_t config_channels,
    uint16_t config_samples_per_frame)
{
    /* sceAudioSRCChReserve does not accept an arbitrary AAC sampling rate.
       Real firmware and PPSSPP accept this discrete family (PSPSDK's 11050
       spelling is a header-documentation typo; the firmware value is 11025).
       Reject 7350 Hz and explicit nonstandard rates before codec and output
       setup fail at a device-only boundary. */
    bool output_rate = track_sample_rate == 48000u
        || track_sample_rate == 44100u
        || track_sample_rate == 32000u
        || track_sample_rate == 24000u
        || track_sample_rate == 22050u
        || track_sample_rate == 16000u
        || track_sample_rate == 12000u
        || track_sample_rate == 11025u
        || track_sample_rate == 8000u;
    return output_rate
        && track_channels == 2u
        && config_sample_rate == track_sample_rate
        && config_channels == track_channels
        && config_samples_per_frame == PSP_MEDIA_AUDIO_SAMPLES;
}

#define PSP_MEDIA_ERROR_BUSY UINT32_C(0x80000021)
#define PSP_MEDIA_ERROR_OUT_OF_MEMORY UINT32_C(0x80000022)
#define PSP_MEDIA_ERROR_ERRNO_ENOMEM UINT32_C(0x8001000c)
#define PSP_MEDIA_ERROR_MEMBLOCK_ALLOC_FAILED UINT32_C(0x800200d9)
#define PSP_MEDIA_ERROR_HEAP_ALLOC_FAILED UINT32_C(0x800200dd)
#define PSP_MEDIA_ERROR_MODULE_MGR_BUSY UINT32_C(0x80020143)
#define PSP_MEDIA_ERROR_KERNEL_NO_MEMORY UINT32_C(0x80020190)
#define PSP_MEDIA_ERROR_KERNEL_NOMEM UINT32_C(0x800203ea)
static inline bool psp_media_module_failure_retryable(int status)
{
    uint32_t code = (uint32_t) status;
    return code == PSP_MEDIA_ERROR_BUSY
        || code == PSP_MEDIA_ERROR_OUT_OF_MEMORY
        || code == PSP_MEDIA_ERROR_ERRNO_ENOMEM
        || code == PSP_MEDIA_ERROR_MEMBLOCK_ALLOC_FAILED
        || code == PSP_MEDIA_ERROR_HEAP_ALLOC_FAILED
        || code == PSP_MEDIA_ERROR_MODULE_MGR_BUSY
        || code == PSP_MEDIA_ERROR_KERNEL_NO_MEMORY
        || code == PSP_MEDIA_ERROR_KERNEL_NOMEM
        /* A suspend can kill the asynchronous module worker between utility
           calls. Its stepwise flags/retained module UID make restarting the
           worker safe; latching this process-wide would otherwise disable
           every later video after one unlucky sleep boundary. */
        || code == PSP_MODULE_ERROR_THREAD_TERMINATED;
}

/*
 * The raw-MP4 firmware path has two independent widths. sceMpegCreate and
 * sceMpegAvcDecode retain the proven 512-pixel decoder argument in both
 * modes. Only the subsequent CSC destination grows to a 768-pixel stride for
 * pictures wider than 480 pixels. Mature PSP players also select a Media
 * Engine program before creating the decoder: type 4 for baseline, type 3
 * for ordinary main-profile video, and type 1 for wide main-profile video.
 * That ladder is a faithful transcription of PMPlayer Advance -- but PMPlayer
 * selects the program from its own kernel-resident module, and Tilefinch is
 * user-mode homebrew that has to reach sceMeBootStart through
 * sctrlHENFindFunction plus kuKernelCall.
 *
 * Three PSP-3000 6.60 device runs now say the same thing about that
 * difference. EVERY configuration attempted so far failed at the raw-NAL
 * bridge with 0xFFFFFFFF for a structurally valid IDR, and sceAudiocodec --
 * also on the Media Engine -- errored in the same sessions: wide Main on
 * type 1, 240p Baseline on type 4, and then type 3, which at the time made no
 * boot call at all because the cached program was assumed to be 3 already.
 *
 * Baseline therefore defaults to type 3 as well. The target 240p stream is
 * Constrained Baseline (avcC profile byte 0x42 with constraint_set1 set),
 * which is by definition decodable by any conforming Main-profile decoder.
 * Routing it to the Main-profile program is legitimate, not merely expedient. Type 3
 * is now also really booted rather than assumed: see
 * psp_media_cold_boot_call_enabled for why that assumption was never earned.
 *
 * Repeated physical-device qualification later promoted wide type 1 to the
 * absent-config default. The policy still receives an
 * explicit `wide_program_enabled` guard so compatibility mode and the
 * process-local first-AU rejection latch can refuse it before create/prime.
 * `baseline_boot_call` remains the boot4 diagnostic for historical A/B work.
 */
static inline bool psp_media_decoder_policy(
    unsigned profile, unsigned level, unsigned width, unsigned height,
    bool wide_program_enabled, bool baseline_boot_call,
    PspMediaDecoderPolicy *policy)
{
    if (policy == NULL || width == 0 || height == 0
        || level == 0 || level > PSP_MEDIA_AVC_MAXIMUM_LEVEL)
        return false;
    bool wide = psp_media_wide_program_required(width, height);
    if (wide && !wide_program_enabled) return false;
    if (profile == PSP_MEDIA_AVC_PROFILE_MAIN) {
        policy->mpeg_mode = wide ? 5 : 4;
        policy->me_boot_type = wide ? 1 : PSP_MEDIA_DEFAULT_ME_BOOT_TYPE;
        return true;
    }
    if (profile == PSP_MEDIA_AVC_PROFILE_BASELINE && !wide) {
        policy->mpeg_mode = 4;
        policy->me_boot_type = baseline_boot_call
            ? 4 : PSP_MEDIA_DEFAULT_ME_BOOT_TYPE;
        return true;
    }
    return false;
}

/*
 * Early hardware runs rejected the wide Main-profile program (create mode 5 /
 * ME boot type 1) before later bridge and lifecycle fixes qualified it. Keep
 * the narrow rejection latch as a compatibility escape for devices or
 * firmware that still reject the first access unit: recognize the wide
 * program failing at AVC admission before anything primed it, and nothing
 * else. A failure after priming remains an ordinary per-stream failure and
 * must not disable 360p for the rest of the process.
 *
 * Create mode is part of the shape on purpose. Neither Baseline program --
 * the default mode 4 / type 3 nor the mode 4 / type 4 the `boot4` knob
 * restores -- can trip this latch, because 240p rejecting an access unit says
 * nothing about whether the wide program would decode 360p.
 */
static inline bool psp_media_wide_program_rejected_by(
    int mpeg_mode, int me_boot_type, bool avc_admission_stage,
    bool decoder_primed)
{
    return mpeg_mode == 5 && me_boot_type == 1
        && avc_admission_stage && !decoder_primed;
}

/*
 * The quality a fresh open may ask the resolver for. Two independent reasons
 * cap it at the compatibility program's ceiling: an explicit override turned
 * the wide program off, or the promoted program has already been latched off
 * after rejecting its first access unit on this device. Quality is the
 * source's vertical pixel count, and the compatibility program covers
 * everything at or below 240p. The rejection latch is memory-only, so a
 * restart re-tests the promoted program.
 *
 * `wide_program_enabled` is false for the `boot4` spelling of the knob, so a
 * Baseline A/B keeps the wide clamp exactly where the default has it.
 */
static inline unsigned psp_media_admitted_quality(
    unsigned requested_quality, bool wide_program_rejected,
    bool wide_program_enabled)
{
    return (wide_program_rejected || !wide_program_enabled)
            && requested_quality > 240u
        ? 240u : requested_quality;
}

/*
 * A signed media URL may expire during a long pause or range reads may
 * start returning 403 after edge/IP rebinding. Re-resolution is deliberately
 * one-shot until the replacement stream proves stable; cancellation always
 * wins. Thirty seconds is enough to avoid opening a new demux over a URL
 * already at its boundary.
 */
static inline bool psp_media_transport_refresh_policy(
    bool already_attempted, bool cancelled,
    long video_http_status, long audio_http_status,
    uint64_t expires_unix, uint64_t now_unix)
{
    if (already_attempted || cancelled) return false;
    if (video_http_status == 403 || audio_http_status == 403) return true;
    return expires_unix != 0
        && expires_unix <= now_unix + UINT64_C(30);
}

/* Do not let a bad replacement URL create an immediate resolve loop. A
   recovered stream earns another future refresh only after it has produced a
   frame and advanced five seconds beyond the recovery point. */
static inline bool psp_media_transport_recovery_stable(
    bool attempted, uint64_t rearm_at_us, uint64_t clock_us,
    bool received_frame)
{
    return attempted && received_frame && rearm_at_us != 0
        && clock_us >= rearm_at_us;
}

/*
 * H.264 codes whole 16-pixel macroblocks. A 426x240 stream is therefore coded
 * as 432x240 and carries a crop rectangle that trims the eight columns the
 * encoder had to invent. Everything derived from the SPS in this backend --
 * decoded_width/decoded_height, the admission check, the frame handed to the
 * presenter -- is the cropped DISPLAY size, because that is the only size a
 * viewer should ever see. Firmware speaks the other language: sceMpegAvcDecode
 * reports the CODED size, and sceMpegBaseCscAvc is fed macroblock counts and
 * writes whole macroblock rows. This is the one conversion between them.
 */
static inline unsigned psp_media_macroblock_align(unsigned pixels)
{
    /* Callers pass SPS-derived dimensions, which the demux bounds to
       UINT16_MAX. Refuse anything whose padded size would not fit that same
       bound, so this never answers with a rounded-up value the rest of the
       backend could not represent. */
    if (pixels > 0xfff0u) return 0;
    return (pixels + 15u) & ~15u;
}

/*
 * Keep the 240p path genuinely smaller so it remains useful as a pressure
 * fallback. The firmware AVC/CSC path uses a 512-pixel stride through 480
 * authored pixels and a 768-pixel stride above that boundary.
 * sceMpegBaseCscAvc consumes width/height in 16-pixel blocks, so nominal
 * 640x360 output writes 640x368 pixels. The firmware CSC output is
 * 32-bit ABGR (RGBA byte order on little-endian PSP), so 360p gets a complete
 * 768x368x4 protected surface. The PSP presenter scales only the authored
 * 640x360 portion and converts it to the display's RGB565 format.
 */
static inline bool psp_media_surface_policy(
    unsigned width, unsigned height, PspMediaSurfacePolicy *policy)
{
    if (policy == NULL || width == 0 || height == 0
        || width > PSP_MEDIA_360P_MAX_WIDTH
        || height > PSP_MEDIA_360P_MAX_HEIGHT) return false;
    bool high = width > 480u || height > 272u;
    policy->stride_pixels = high
        ? PSP_MEDIA_360P_FRAME_STRIDE : PSP_MEDIA_240P_FRAME_STRIDE;
    policy->surface_rows = high
        ? PSP_MEDIA_360P_FRAME_ROWS : PSP_MEDIA_240P_FRAME_ROWS;
    policy->surface_bytes =
        (size_t) policy->stride_pixels * policy->surface_rows
        * sizeof(uint32_t);
    policy->external_reserve_bytes = high
        ? PSP_MEDIA_360P_EXTERNAL_RESERVE
        : PSP_MEDIA_240P_EXTERNAL_RESERVE;
    return true;
}

/*
 * The CSC destination is macroblock-padded on both axes, so the protected
 * surface has to hold the PADDED source, not the display size. Both bands
 * already cover themselves: the 512-pixel stride serves display widths through
 * 480, which pads to 480, and the 768-pixel stride serves widths through 640,
 * which pads to 640; likewise 272 rows hold a 272-padded 240p picture and 368
 * rows hold a 368-padded 360p one. Stating that as a checked invariant rather
 * than a comment means a future stride or clamp change cannot quietly let the
 * Media Engine write past the surface.
 */
static inline bool psp_media_surface_covers_decoded(
    const PspMediaSurfacePolicy *policy, unsigned width, unsigned height)
{
    if (policy == NULL) return false;
    unsigned padded_width = psp_media_macroblock_align(width);
    unsigned padded_height = psp_media_macroblock_align(height);
    return padded_width != 0 && padded_height != 0
        && padded_width <= policy->stride_pixels
        && padded_height <= policy->surface_rows;
}

/*
 * What sceMpegAvcDecodeDetail2 must report for a picture of the stream we
 * opened. Firmware answers in CODED pixels, so a 426x240 source is expected to
 * come back as 432x240 -- padding it to macroblocks is agreement, not drift.
 * Comparing the reported size against the display size instead rejected every
 * picture of any stream whose width was not already a multiple of sixteen.
 *
 * A genuine mid-stream geometry change still has to fail here, and does: it
 * changes the coded size too, so a 432x240 picture arriving for a 400x240
 * source (coded 400x240) is refused exactly as before.
 */
static inline bool psp_media_decoded_geometry_admitted(
    int reported_width, int reported_height,
    unsigned display_width, unsigned display_height)
{
    if (reported_width <= 0 || reported_height <= 0) return false;
    unsigned expected_width = psp_media_macroblock_align(display_width);
    unsigned expected_height = psp_media_macroblock_align(display_height);
    return expected_width != 0 && expected_height != 0
        && (unsigned) reported_width == expected_width
        && (unsigned) reported_height == expected_height;
}

/*
 * Raw-NAL decoding is not implemented by PPSSPP, so it falls back to its PMP
 * compatibility path. That path currently fixes its internal image at
 * 480x272 even when the submitted SPS is 640x360, while still reporting a
 * successful picture. Poison the last CSC block row before the first picture
 * and require the decoder to overwrite it. This is also a useful firmware
 * contract check: a successful 360p conversion must cover all 368 output
 * rows.
 */
static inline void psp_media_surface_canary_fill(
    uint32_t *row, unsigned width)
{
    if (row == NULL) return;
    for (unsigned x = 0; x < width; x++)
        row[x] = (x & 1u) != 0
            ? PSP_MEDIA_SURFACE_CANARY_B
            : PSP_MEDIA_SURFACE_CANARY_A;
}

static inline bool psp_media_surface_canary_was_overwritten(
    const uint32_t *row, unsigned width)
{
    if (row == NULL || width == 0) return false;
    for (unsigned x = 0; x < width; x++) {
        uint32_t canary = (x & 1u) != 0
            ? PSP_MEDIA_SURFACE_CANARY_B
            : PSP_MEDIA_SURFACE_CANARY_A;
        if (row[x] == canary) return false;
    }
    return true;
}

/*
 * AVCC to Annex-B, in place, for the wide-program experiment.
 *
 * The demux hands the bridge exactly what the MP4 sample contained: NAL units
 * each preceded by a big-endian length of `prefix_size` bytes. The hardware
 * rejection logged a submitted head of 0000014125b84005, which reads as an
 * AVCC length of 321 followed by an IDR header, and equally as an Annex-B
 * three-byte start code followed by a non-IDR header. PMPlayer-era streams
 * were Annex-B, so the framing is the leading hypothesis for why firmware
 * refuses the access unit -- but it is a hypothesis, which is why this only
 * runs behind the experimental knob.
 *
 * Only a four-byte prefix can be rewritten: 00 00 00 01 is exactly four bytes,
 * so the buffer keeps its length and no per-NAL allocation is needed. Any
 * other prefix width, a length that leaves the buffer, or trailing bytes that
 * cannot start another unit are refused rather than half-converted; the caller
 * logs the refusal and fails the submission.
 *
 * Parameter sets are not touched. The bridge takes SPS and PPS as separate
 * pointers with explicit sizes, so nothing in this buffer carries them and no
 * in-band injection is required.
 */
static inline bool psp_media_annexb_rewrite(
    unsigned char *buffer, size_t bytes, unsigned prefix_size,
    unsigned *nal_count)
{
    if (nal_count != NULL) *nal_count = 0;
    if (buffer == NULL || bytes == 0 || prefix_size != 4u) return false;
    size_t at = 0;
    unsigned count = 0;
    while (at < bytes) {
        if (bytes - at < 4u) return false;
        size_t length = ((size_t) buffer[at] << 24)
            | ((size_t) buffer[at + 1u] << 16)
            | ((size_t) buffer[at + 2u] << 8)
            | (size_t) buffer[at + 3u];
        if (length == 0 || length > bytes - at - 4u) return false;
        buffer[at] = 0u;
        buffer[at + 1u] = 0u;
        buffer[at + 2u] = 0u;
        buffer[at + 3u] = 1u;
        at += 4u + length;
        count++;
    }
    if (count == 0) return false;
    if (nal_count != NULL) *nal_count = count;
    return true;
}

/*
 * An undocumented raw-NAL bridge result is not judged from iAuSize. Bound how
 * long the actual decoder may retain timestamps without producing a picture;
 * the limit deliberately fits the existing reorder queue so it cannot stall
 * at a full queue first.
 */
static inline bool psp_media_raw_nal_probe_exhausted(
    unsigned submitted_packets, bool picture_ready)
{
    return !picture_ready
        && submitted_packets >= PSP_MEDIA_RAW_NAL_PROBE_PACKETS;
}

/*
 * sceMpegAvcDecode reports whether an output picture exists, but not which
 * submitted access unit it belongs to. Retain presentation timestamps in
 * sorted order so delayed/reordered output receives the earliest outstanding
 * PTS. Sixteen matches the mature PSP MP4 implementations and bounds state.
 */
static inline bool psp_media_timestamp_push(
    PspMediaVideoTimestampQueue *queue, uint64_t pts_us,
    uint64_t duration_us)
{
    if (queue == NULL
        || queue->count >= PSP_MEDIA_VIDEO_TIMESTAMP_SLOTS) return false;
    unsigned at = queue->count;
    while (at != 0 && pts_us < queue->entries[at - 1u].pts_us) {
        queue->entries[at] = queue->entries[at - 1u];
        at--;
    }
    queue->entries[at] = (PspMediaVideoTimestamp) {
        .pts_us = pts_us, .duration_us = duration_us
    };
    queue->count++;
    return true;
}

static inline bool psp_media_timestamp_pop(
    PspMediaVideoTimestampQueue *queue,
    PspMediaVideoTimestamp *timestamp)
{
    if (queue == NULL || timestamp == NULL || queue->count == 0)
        return false;
    *timestamp = queue->entries[0];
    queue->count--;
    for (unsigned at = 0; at < queue->count; at++)
        queue->entries[at] = queue->entries[at + 1u];
    return true;
}

/*
 * Mature raw-MP4 players treat the decoder status as a picture count and
 * walk parallel Detail2 info/YUV arrays when it exceeds one. Bound that
 * firmware-provided count before indexing either array, and require one
 * retained timestamp for every picture in the batch.
 */
static inline bool psp_media_output_batch_admitted(
    int output_count, unsigned timestamp_count)
{
    return output_count > 0
        && (unsigned) output_count <= PSP_MEDIA_VIDEO_TIMESTAMP_SLOTS
        && (unsigned) output_count <= timestamp_count;
}

static inline bool psp_media_output_count_sane(int output_count)
{
    return output_count >= 0
        && (unsigned) output_count <= PSP_MEDIA_VIDEO_TIMESTAMP_SLOTS;
}

/*
 * What a reposition does to the decoder. See validation_media_reset_mode.
 */
#define PSP_MEDIA_RESET_MODE_IN_PLACE 0
#define PSP_MEDIA_RESET_MODE_RECREATE 1
#define PSP_MEDIA_RESET_MODE_NO_TOUCH 2

/*
 * Pictures at the head of a batch that belong to where the stream used to be,
 * and why a no-touch reposition cannot simply drop our queues and hope.
 *
 * Modes 0 and 1 hand firmware a flush or a new object, so nothing survives the
 * reposition and every picture that arrives afterwards belongs to the new
 * position. Mode 2 deliberately tells firmware nothing, which means the Media
 * Engine's reorder pipeline is still holding whatever it had not released --
 * the device log shows output arriving in batches of two and three, so units
 * in between are being held. Those pictures come out after the reposition.
 *
 * Our own queues are cleared, because a pre-seek timestamp must never pair
 * with a post-seek picture. That leaves the opposite hazard, and it is the one
 * that would end the run rather than blemish it: psp_media_output_batch_admitted
 * requires one retained timestamp per picture, so a batch of three arriving
 * against the single timestamp the first post-reposition access unit pushed is
 * refused outright and the session fails with "invalid picture batch". The
 * experiment would then measure the admission rule instead of the wedge.
 *
 * So while the drain window is open, a batch bigger than the timestamps held
 * is read as "the excess is stale" and those pictures are skipped rather than
 * presented. They are at the head because a batch is in output order and the
 * held pictures are the older ones. The window is bounded by the timestamp
 * queue, which is already this decoder's stated bound on supported reorder
 * depth, so a decoder that never returns to a paired batch cannot keep
 * skipping forever.
 *
 * What this does not fix, and does not pretend to: a single held picture that
 * fits inside the timestamps already queued is paired with the earliest of
 * them, so it is presented as one frame of pre-seek content and every later
 * picture is shifted by one entry. That is a bounded A/V skew of a few tens of
 * milliseconds, not a failure. PMPlayer avoids it by never clearing its
 * timestamp queue at all, letting held pictures pair with their own old
 * timestamps; we cannot copy that directly, because our presentation clock
 * re-anchors to a decoded picture's timestamp and a pre-seek timestamp would
 * drag the clock back across the seek.
 */
static inline unsigned psp_media_unpaired_pictures(
    int output_count, unsigned timestamp_count)
{
    if (output_count <= 0) return 0u;
    if ((unsigned) output_count > PSP_MEDIA_VIDEO_TIMESTAMP_SLOTS) return 0u;
    if ((unsigned) output_count <= timestamp_count) return 0u;
    return (unsigned) output_count - timestamp_count;
}

static inline unsigned psp_media_reposition_stale_pictures(
    unsigned drain_units, int output_count, unsigned timestamp_count)
{
    if (drain_units == 0u) return 0u;
    return psp_media_unpaired_pictures(output_count, timestamp_count);
}

typedef enum {
    PSP_MEDIA_VIDEO_DRAIN_COMPLETE = 0,
    PSP_MEDIA_VIDEO_DRAIN_WAIT_FOR_SURFACE,
    PSP_MEDIA_VIDEO_DRAIN_DROP_SURFACE,
    PSP_MEDIA_VIDEO_DRAIN_EMIT_PENDING,
    PSP_MEDIA_VIDEO_DRAIN_CALL_NATIVE
} PspMediaVideoDrainAction;

/*
 * sceMpegAvcDecodeStop is a terminal operation that can expose a bounded
 * batch of delayed pictures. Do not call it while the sole RGB surface is
 * still owned by the presenter, and never call it more than once for a
 * stream. The backend drains any Detail2 batch before consulting this policy
 * again. A frontend which abandons a surface gets three later pumps to accept
 * it; after that the backend drops the unobservable tail and retires instead
 * of staying pending for the lifetime of the page.
 */
static inline PspMediaVideoDrainAction psp_media_video_drain_action(
    unsigned timestamp_count, unsigned pending_pictures,
    unsigned free_slots, unsigned native_calls,
    unsigned surface_waits)
{
    /* A drain publishes into a slot like anything else, so the question is
       whether one is free rather than whether the single surface had been
       claimed. With two slots this is false far less often, and when it is
       true it is because both are legitimately occupied -- one claimed, one
       converted and waiting -- which is the shape the pipeline is meant to
       reach rather than the stall it used to describe. */
    if (free_slots == 0) {
        return surface_waits < PSP_MEDIA_VIDEO_DRAIN_SURFACE_POLLS
            ? PSP_MEDIA_VIDEO_DRAIN_WAIT_FOR_SURFACE
            : PSP_MEDIA_VIDEO_DRAIN_DROP_SURFACE;
    }
    if (pending_pictures != 0)
        return PSP_MEDIA_VIDEO_DRAIN_EMIT_PENDING;
    if (timestamp_count == 0) return PSP_MEDIA_VIDEO_DRAIN_COMPLETE;
    return native_calls < PSP_MEDIA_VIDEO_DRAIN_CALLS
        ? PSP_MEDIA_VIDEO_DRAIN_CALL_NATIVE
        : PSP_MEDIA_VIDEO_DRAIN_COMPLETE;
}

/*
 * An AAC access unit begins at zero, so a closed decode horizon includes one
 * more unit than the number of complete durations it spans. The bounded
 * first-frame preroll is the largest paused-audio horizon used by the
 * frontend; ordinary playback retains only its much smaller decode lead.
 */
_Static_assert(
    1u + ((uint64_t) PSP_MEDIA_FIRST_FRAME_PREROLL_US
          * PSP_MEDIA_AUDIO_MAXIMUM_RATE)
         / (PSP_MEDIA_AUDIO_SAMPLES * UINT64_C(1000000))
        <= PSP_MEDIA_AUDIO_QUEUE_SLOTS,
    "PSP audio read-ahead must fit the paused PCM queue");
/* Ordinary playback now uses the same horizon, so it is bound by the same
   queue rather than by the argument that it was much smaller. */
_Static_assert(
    1u + ((uint64_t) PSP_MEDIA_DECODE_LEAD_US
          * PSP_MEDIA_AUDIO_MAXIMUM_RATE)
         / (PSP_MEDIA_AUDIO_SAMPLES * UINT64_C(1000000))
        <= PSP_MEDIA_AUDIO_QUEUE_SLOTS,
    "PSP decode lead must fit the PCM queue it fills");

/* The probe must reach its verdict before the reorder queue blocks
   submission, or a full queue -- not the probe -- decides the outcome. */
_Static_assert(
    PSP_MEDIA_RAW_NAL_PROBE_PACKETS <= PSP_MEDIA_VIDEO_TIMESTAMP_SLOTS,
    "PSP raw-NAL probe must conclude before the reorder queue fills");

#endif
