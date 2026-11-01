#ifndef TILEFINCH_SWDEC_BOUNDS_H
#define TILEFINCH_SWDEC_BOUNDS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Shared, allocation-free admission rules for data that crosses into the
 * optional decoder or its Media Engine helpers. Keep these checks at every
 * ownership boundary: a source parser, decoder, or component can fail
 * independently and none may authorize a write into the next owner's fixed
 * buffer. */
static inline bool swdec_dimensions_admitted(
    int width, int height, int max_width, int max_height)
{
    return width > 0 && height > 0 && max_width > 0 && max_height > 0
        && width <= max_width && height <= max_height;
}

static inline bool swdec_rgb565_destination_fits(
    int width, int height, int stride_pixels, size_t capacity_bytes)
{
    if (width <= 0 || height <= 0 || (width & 1) != 0
        || stride_pixels < width)
        return false;
    if ((size_t) stride_pixels > SIZE_MAX / sizeof(uint16_t))
        return false;
    size_t row_bytes = (size_t) stride_pixels * sizeof(uint16_t);
    return (size_t) height <= capacity_bytes / row_bytes;
}

static inline bool swdec_audio_channels_admitted(int channels)
{
    return channels == 1 || channels == 2;
}

/* Always service one packet, but do not keep the parser on a whole audio
   batch after the CPU has consumed its last published video row. Check
   again between packets: the CPU can catch up while AAC is decoding. */
static inline bool swdec_audio_service_continue(unsigned completed,
                                                 bool video_needs_row)
{
    return completed < 4u && (completed == 0u || !video_needs_row);
}

/* Absolute clock correction after selecting a late frame. Repeated visits to
 * the same timing error must not accumulate another copy of that error. */
static inline uint64_t swdec_clock_reanchor_slip(
    uint64_t presentation_clock_us, uint64_t selected_pts_us)
{
    return presentation_clock_us >= selected_pts_us
        ? presentation_clock_us - selected_pts_us : 0;
}

/* The frame to show at the audio clock: the latest decoded frame that is
 * already due (pts no more than half a frame ahead), or -1 if none is.
 * Audio never waits for video; when decoding falls behind, every due frame
 * older than the one returned is dropped instead of slowing the clock. */
static inline int swdec_pick_due_frame(
    const uint64_t *pts_us, const uint64_t *duration_us,
    const bool *eligible, unsigned count, uint64_t clock_us)
{
    int picked = -1;
    for (unsigned i = 0; i < count; i++) {
        if (!eligible[i]) continue;
        uint64_t half = duration_us[i] / 2u;
        bool due = pts_us[i] <= clock_us || pts_us[i] - clock_us <= half;
        if (due && (picked < 0 || pts_us[i] > pts_us[picked]))
            picked = (int) i;
    }
    return picked;
}

/* Discard only already-late non-reference pictures. A source-frame duration
 * is not a decode-cost estimate: cheap B pictures can still make their
 * deadline with less than one frame of lead. The presenter remains the
 * final due/supersession gate, and the bounded catch-up ladder is unchanged. */
static inline bool swdec_au_late(uint64_t pts_us, uint64_t clock_us)
{
    return pts_us <= clock_us;
}

typedef enum {
    SWDEC_NAL_NEXT = 0,
    SWDEC_NAL_END,
    SWDEC_NAL_BAD
} SwdecNalStep;

/* The next NAL unit of an H.264 access unit from *at: its payload offset
 * and length. nal_length_size 0 means Annex-B start codes; 1..4 means
 * length-prefixed (MP4), where a bad length is BAD. */
static inline SwdecNalStep swdec_au_next_nal(
    const unsigned char *au, size_t length, unsigned nal_length_size,
    size_t *at, size_t *nal_start, size_t *nal_bytes)
{
    if (*at >= length) return SWDEC_NAL_END;
    if (nal_length_size != 0) {
        if (nal_length_size > length - *at) return SWDEC_NAL_BAD;
        size_t bytes = 0;
        for (unsigned i = 0; i < nal_length_size; i++)
            bytes = (bytes << 8) | au[*at + i];
        *at += nal_length_size;
        if (bytes == 0 || bytes > length - *at) return SWDEC_NAL_BAD;
        *nal_start = *at;
        *nal_bytes = bytes;
        *at += bytes;
        return SWDEC_NAL_NEXT;
    }
    size_t prefix = 0;
    while (*at + 3u <= length) {
        if (au[*at] == 0 && au[*at + 1u] == 0 && au[*at + 2u] == 1u) {
            prefix = 3u;
            break;
        }
        (*at)++;
    }
    if (prefix == 0) return SWDEC_NAL_END;
    *nal_start = *at + prefix;
    size_t next = *nal_start;
    while (next + 3u <= length
           && !(au[next] == 0 && au[next + 1u] == 0
                && (au[next + 2u] == 1u
                    || (au[next + 2u] == 0 && next + 3u < length
                        && au[next + 3u] == 1u))))
        next++;
    if (next + 3u > length) next = length;
    *nal_bytes = next - *nal_start;
    *at = next;
    return SWDEC_NAL_NEXT;
}

/* Whether an H.264 access unit holds only non-reference pictures (every VCL
 * NAL unit, types 1 and 5, has nal_ref_idc 0), so nothing later decodes
 * against it and it may be dropped before decoding. Malformed or empty
 * input answers false: when in doubt, decode. */
static inline bool swdec_au_is_non_reference(
    const unsigned char *au, size_t length, unsigned nal_length_size)
{
    if (au == NULL || nal_length_size > 4u) return false;
    bool any_vcl = false;
    size_t at = 0, nal_start = 0, nal_bytes = 0;
    SwdecNalStep step;
    while ((step = swdec_au_next_nal(au, length, nal_length_size, &at,
                                     &nal_start, &nal_bytes))
           == SWDEC_NAL_NEXT) {
        if (nal_bytes == 0) continue;
        unsigned header = au[nal_start];
        unsigned type = header & 0x1fu;
        if (type == 1u || type == 5u) {
            any_vcl = true;
            if (type == 5u || (header & 0x60u) != 0) return false;
        }
    }
    return step == SWDEC_NAL_END && any_vcl;
}

/* The first VCL NAL unit's payload after its header byte (validation
 * traces read its slice type), or NULL. */
static inline const unsigned char *swdec_au_first_slice(
    const unsigned char *au, size_t length, unsigned nal_length_size,
    size_t *slice_bytes)
{
    if (au == NULL || nal_length_size > 4u) return NULL;
    size_t at = 0, nal_start = 0, nal_bytes = 0;
    while (swdec_au_next_nal(au, length, nal_length_size, &at, &nal_start,
                             &nal_bytes) == SWDEC_NAL_NEXT) {
        unsigned type = nal_bytes == 0 ? 0u : au[nal_start] & 0x1fu;
        if (nal_bytes > 1u && (type == 1u || type == 5u)) {
            *slice_bytes = nal_bytes - 1u;
            return au + nal_start + 1u;
        }
    }
    return NULL;
}

/* Whether the audio output may start: a full prebuffer of decoded frames,
 * or -- once every source has ended and the ME has decoded all it was
 * given -- whatever there is, so a stream shorter than the prebuffer (or a
 * seek into its last half second) still plays and its clock still moves. */
/* Pictures to gather before the first is shown: 20, or what a shorter
 * ring of `slots` pictures can hold with `in_flight` decodes still writing
 * (the ring stops at the slots the Media Engine can reach, which can be
 * fewer than 25). The target must be reachable: until it is met the first
 * picture is not taken, so no slot frees. */
#define SWDEC_STARTUP_READY_FRAMES 20u
static inline unsigned swdec_startup_ready_frames(unsigned slots,
                                                  unsigned in_flight)
{
    unsigned room = slots > in_flight ? slots - in_flight : 1u;
    return room < SWDEC_STARTUP_READY_FRAMES ? room
                                             : SWDEC_STARTUP_READY_FRAMES;
}

/* The decoder speed rung (0 full quality, 3 and 4 reduced) for `ready`
 * pictures waiting, with hysteresis. A ring of `slots` pictures holds at
 * most `slots` minus `in_flight` decodes and `held` pictures the presenter
 * still has (shown, and drawn by the graphics engine): that capacity sets
 * the rungs. Full quality recovers at 8 ready, or at the capacity when it
 * holds fewer. Quality reduces at a quarter of the capacity and most at a
 * sixth, at most 4 and 2: a full ring keeps 2/4/8. Fractions, because the
 * ring also drains when the decoder is fed nothing (a main loop iteration
 * of about 0.3 s empties seven pictures at 24 fps): on a shallow ring the
 * fixed 4 is reached by such a stall alone, with the decoder keeping up. */
static inline int swdec_speed_for_ready(int speed, unsigned ready,
                                        unsigned slots, unsigned in_flight,
                                        unsigned held)
{
    unsigned capacity = slots > in_flight + held
        ? slots - in_flight - held : 1u;
    unsigned recover = capacity < 8u ? capacity : 8u;
    unsigned reduce = capacity / 4u < 4u ? capacity / 4u : 4u;
    unsigned reduce_most = capacity / 6u < 2u ? capacity / 6u : 2u;
    if (ready <= reduce_most) return 3;
    if (ready <= reduce) return 4;
    if (ready >= recover) return 0;
    return speed;
}

/* The decoder speed a reduced rung asks for. Both reduced rungs (3 and 4)
 * used to drop non-reference pictures before decoding (one in two, one in
 * four) whether or not they were late. On a decode-bound stream that kept
 * the decoder only just level with the clock, with nothing ready, so every
 * expensive reference picture made the cheap pictures behind it late and
 * the presenter discarded them as superseded: on a 24 fps CABAC page 240 of
 * 604 decoded pictures in a 30 s window (12.5 fps shown), where fixed full
 * quality showed 19.6 fps and discarded 5. Dropping is left to lateness
 * (swdec_au_late), which sheds exactly the pictures that cannot make it;
 * a reduced rung now only skips deblocking on non-reference pictures
 * (speed 1), which costs no picture. */
static inline int swdec_rung_decoder_speed(int rung)
{
    return rung == 0 ? 0 : 1;
}

static inline bool swdec_audio_prebuffered(
    unsigned decoded, unsigned submitted, bool input_ended,
    unsigned prebuffer_frames)
{
    if (decoded >= prebuffer_frames) return true;
    return input_ended && decoded != 0 && decoded >= submitted;
}

/* A decoded AAC block the 1024-sample output cannot play as decoded: more
 * samples a channel than that, which is implicit SBR (HE-AAC announced only
 * by its output). The decoder reports a failed frame's error code (negative,
 * so huge unsigned) in the same field; that frame was written as silence
 * and plays. */
static inline bool swdec_audio_block_refused(unsigned samples)
{
    return samples > 1024u && samples <= 4096u;
}

/* What the browser does about a failed Media Engine: nothing while it is
 * fine, wait while the decode worker still holds a job (restarting it would
 * switch that job's slice between CPU and ME modes mid-row), restart it
 * once the worker is idle. */
typedef enum {
    SWDEC_ME_RECOVERY_NONE = 0,
    SWDEC_ME_RECOVERY_WAIT,
    SWDEC_ME_RECOVERY_RESTART
} SwdecMeRecovery;

static inline SwdecMeRecovery swdec_me_recovery_action(
    bool me_failed, unsigned jobs_in_flight)
{
    if (!me_failed) return SWDEC_ME_RECOVERY_NONE;
    return jobs_in_flight != 0 ? SWDEC_ME_RECOVERY_WAIT
                               : SWDEC_ME_RECOVERY_RESTART;
}

/* The compressed-video queue between the demuxer and the decoder.
 *
 * With audio it decouples the two: a video unit waiting for the decoder must
 * not hold back the audio interleaved behind it, so it holds about eight of
 * the largest units (256 KiB to 1 MiB). Without audio nothing needs
 * decoupling (the demuxer may simply wait), so it only prefetches a couple
 * of units ahead of the decoder. Either way a smaller queue still plays
 * (more dropped or waiting units), so under memory pressure the allocation
 * steps down towards `floor`, the least that holds the largest unit. False
 * when no queue can hold the largest unit. */
#define SWDEC_AU_QUEUE_MIN_BYTES ((size_t) 256u * 1024u)
#define SWDEC_AU_QUEUE_MAX_BYTES ((size_t) 1024u * 1024u)
#define SWDEC_AU_QUEUE_VIDEO_ONLY_MIN_BYTES ((size_t) 64u * 1024u)
#define SWDEC_AU_QUEUE_FLOOR_MIN_BYTES ((size_t) 32u * 1024u)

static inline bool swdec_au_queue_plan(
    size_t largest, bool have_audio, size_t *preferred, size_t *floor)
{
    if (preferred == NULL || floor == NULL
        || largest >= SWDEC_AU_QUEUE_MAX_BYTES) return false;
    size_t units = have_audio ? 8u : 2u;
    size_t lower = have_audio ? SWDEC_AU_QUEUE_MIN_BYTES
                              : SWDEC_AU_QUEUE_VIDEO_ONLY_MIN_BYTES;
    size_t want = largest > SWDEC_AU_QUEUE_MAX_BYTES / units
        ? SWDEC_AU_QUEUE_MAX_BYTES : largest * units;
    if (want < lower) want = lower;
    if (want <= largest) want = largest + 1u;
    size_t least = largest + 1u;
    if (least < SWDEC_AU_QUEUE_FLOOR_MIN_BYTES)
        least = SWDEC_AU_QUEUE_FLOOR_MIN_BYTES;
    if (least > want) least = want;
    *preferred = want;
    *floor = least;
    return true;
}

/* The next smaller size to try after `bytes` was refused, or 0 when
   `bytes` was already the floor. */
static inline size_t swdec_au_queue_step_down(size_t bytes, size_t floor)
{
    if (bytes <= floor) return 0;
    size_t half = bytes / 2u;
    return half > floor ? half : floor;
}

/* The longest compressed sample the player hands a media backend:
 * media_playback refuses anything over PSP_MEDIA_MAXIMUM_PACKET_BYTES, and
 * the HLS transport-stream path assembles access units into a buffer of
 * SWDEC_TS_MAX_AU. Both are this (checked where the backend is built).
 *
 * A track's largest sample at open is not a bound: a fragmented MP4 (every
 * DASH download) has an empty moov, and at open the demuxer knows only the
 * first fragment's samples; a keyframe two seconds in can be twice as big.
 * Buffers sized from it grow on demand up to this bound instead. */
#define SWDEC_SAMPLE_MAX_BYTES ((size_t) 512u * 1024u)

/* A video queue of `capacity` bytes cannot take a `length`-byte unit even
 * when empty (swdec_au_queue_plan always leaves one byte over), so it must
 * grow first, to a plan made for `length`. */
static inline bool swdec_au_queue_needs_growth(size_t capacity, size_t length)
{
    return length >= capacity;
}

/* Bytes of decoder packet one `sample_bytes` sample can need: its NAL units
 * rewritten with four-byte start codes (nal_length_size 1-4; 0 for Annex-B,
 * which is copied as is) plus `parameter_set_bytes` of SPS/PPS put in front
 * of a keyframe. False on overflow or a bad prefix width. */
static inline bool swdec_packet_bytes(
    size_t sample_bytes, unsigned nal_length_size,
    size_t parameter_set_bytes, size_t *capacity)
{
    if (capacity == NULL || nal_length_size > 4u) return false;
    size_t expansion = 0;
    if (nal_length_size != 0) {
        /* Each NAL unit takes at least its prefix and one byte. */
        size_t nal_count = sample_bytes / (nal_length_size + 1u);
        if (nal_count > SIZE_MAX / 4u) return false;
        expansion = nal_count * (4u - nal_length_size);
    }
    if (sample_bytes > SIZE_MAX - expansion
        || sample_bytes + expansion > SIZE_MAX - parameter_set_bytes)
        return false;
    *capacity = sample_bytes + expansion + parameter_set_bytes;
    return true;
}

/* The decoder packet's size once a unit needs `needed` bytes and it holds
 * `current`: unchanged when it fits; else half again as big (a stream's
 * keyframes grow over several fragments, so it is reallocated a few times,
 * not at each), at least `needed` and at most `ceiling`, what the largest
 * admitted sample can need. 0 when `needed` is past `ceiling`. */
static inline size_t swdec_packet_regrow(
    size_t current, size_t needed, size_t ceiling)
{
    if (needed <= current) return current;
    if (needed > ceiling) return 0;
    size_t step = current / 2u;
    size_t next = step > ceiling - current ? ceiling : current + step;
    return next < needed ? needed : next;
}

/* Bytes an AAC packet slot holds. The track's largest sample at open is no
 * bound for a fragmented file (see SWDEC_SAMPLE_MAX_BYTES; a sample download's
 * audio reads 571 bytes at open and 670 later), and the slots cannot grow:
 * the Media Engine reads them while later ones fill. AAC's own limit is:
 * a raw data block never exceeds the decoder's input buffer, 6144 bits a
 * channel (ISO/IEC 14496-3, minimum decoder input buffer), 1536 bytes for
 * the stereo the decoder plays. `channels` outside 1-2 counts as 2 (such
 * streams are refused later). */
#define SWDEC_AAC_CHANNEL_MAX_BYTES 768u
static inline size_t swdec_aac_sample_bound(
    uint32_t open_largest, unsigned channels)
{
    size_t bound = (size_t) SWDEC_AAC_CHANNEL_MAX_BYTES
        * (channels == 1u ? 1u : 2u);
    return open_largest > bound ? open_largest : bound;
}

/* The decoder arena for a stream of at most max_width x max_height whose
 * SPS asks for at most max_refs held pictures. Measured peaks (host decode
 * of this FFmpeg build with a counting allocator, 432x240): 1.15 MB plus
 * 226 KB per held picture, 4.77 MB at 16 references (High, B-pyramid);
 * device runs read 2-7% above the host (arena headers, first fit):
 * 2.43 MB for 416x234 at 5, 2.29-2.41 MB at 426x240 with 4 and
 * B-frames. This allows refs+2 pictures (with the per-picture padding),
 * 1 KiB per macroblock and 1 MiB of tables and slack: 3.08 MB at 416x234/5,
 * 2.97 MB at 426x240/4, 5.91 MB at 432x240/16, 15-30% over the device
 * peaks; the PPSSPP CRC gate (tools/swdec-bench/crc-gate.sh) checks every
 * clip's real peak against it. (It was refs+4 pictures and 1.5 MiB, which
 * left no room beside it in the Media Engine pool for the audio arena.)
 * The component's arena_bytes answers with exactly this. */
static inline size_t swdec_arena_estimate(
    int max_width, int max_height, int max_refs)
{
    if (max_width <= 0 || max_height <= 0 || max_refs < 0) return 0;
    size_t w = (size_t) ((max_width + 31) & ~31);
    size_t h = (size_t) ((max_height + 31) & ~31);
    size_t pic = (w + 64) * (h + 64) * 3 / 2;
    size_t pics = pic * (size_t) (max_refs + 2);
    size_t mbs = ((size_t) max_width / 16 + 1)
        * ((size_t) max_height / 16 + 1);
    return pics + mbs * 1024 + 1024 * 1024;
}

/* How many held pictures to size the arena for. A stream's SPS declares
 * them (max_num_ref_frames, or max_dec_frame_buffering when larger); the
 * arena is sized for that, never below SWDEC_ARENA_MIN_REFS (the depth the
 * bench measured; smaller streams save little) and never above H.264's 16.
 * Without a readable SPS at open the arena keeps the full 16. A later SPS
 * asking for more than the arena was sized for is refused
 * (swdec_sps_frames_admitted) rather than left to exhaust the arena. */
#define SWDEC_ARENA_MIN_REFS 4
#define SWDEC_ARENA_MAX_REFS 16
static inline int swdec_arena_refs(bool known, unsigned frames)
{
    if (!known) return SWDEC_ARENA_MAX_REFS;
    if (frames < (unsigned) SWDEC_ARENA_MIN_REFS) return SWDEC_ARENA_MIN_REFS;
    if (frames > (unsigned) SWDEC_ARENA_MAX_REFS) return SWDEC_ARENA_MAX_REFS;
    return (int) frames;
}

static inline bool swdec_sps_frames_admitted(unsigned frames, int arena_refs)
{
    return arena_refs > 0 && frames <= (unsigned) arena_refs;
}

/* Validation restricts the usable reservoir after normal allocation, so
 * it cannot change admission/recovery or exceed the admitted capacity. */
static inline unsigned swdec_picture_slot_limit(unsigned capacity, bool fixed_nine)
{
    return fixed_nine && capacity > 9u ? 9u : capacity;
}

/* How many whole picture slots fit in `claimable` bytes of pool (each slot
 * a 64-byte multiple, claimed back to back), at most `slots`. */
static inline unsigned swdec_pool_slot_count(
    size_t claimable, size_t slot_bytes, unsigned slots)
{
    if (slot_bytes == 0) return 0;
    size_t fit = claimable / slot_bytes;
    return fit < (size_t) slots ? (unsigned) fit : slots;
}

/* Media Engine reach: the extra RAM bank (0x0A000000 up) is invisible to it,
 * through either cached or uncached alias. */
#define SWDEC_ME_VISIBLE_LIMIT_ADDRESS UINT32_C(0x0A000000)
static inline bool swdec_me_address_visible(uintptr_t address, size_t bytes)
{
    uintptr_t physical = address & (uintptr_t) UINT32_C(0x1FFFFFFF);
    return address != 0 && physical < SWDEC_ME_VISIBLE_LIMIT_ADDRESS
        && bytes <= SWDEC_ME_VISIBLE_LIMIT_ADDRESS - physical;
}

/* Allocations the Media Engine can reach. Blocks it cannot reach are held
 * aside (so the allocator offers another) up to `max_held` at a time; one
 * search can serve several allocations (a ring of picture slots), keeping
 * the rejected blocks across them so each allocation does not walk past the
 * same high blocks again, and releases them once at its end. */
typedef void *(*SwdecAllocate)(void *context, size_t bytes);
typedef void (*SwdecRelease)(void *context, void *block);
#define SWDEC_LOW_SEARCH_HELD 32u
typedef struct {
    void *held[SWDEC_LOW_SEARCH_HELD];
    unsigned count;
    unsigned max_held;
    unsigned passed_over;     /* blocks held aside, in all */
    unsigned attempts;        /* calls to the allocator, in all */
} SwdecLowSearch;

static inline void swdec_low_search_begin(SwdecLowSearch *search,
                                          unsigned max_held)
{
    search->count = 0;
    search->max_held = max_held > SWDEC_LOW_SEARCH_HELD
        ? SWDEC_LOW_SEARCH_HELD : max_held;
    search->passed_over = 0;
    search->attempts = 0;
}

/* A reachable block, or NULL when none is found while at most `max_held`
 * blocks are held; with `accept_high` a held unreachable block is returned
 * instead of NULL. */
static inline void *swdec_low_search_take(
    SwdecLowSearch *search, SwdecAllocate allocate, SwdecRelease release,
    void *context, size_t bytes, bool accept_high)
{
    for (;;) {
        search->attempts++;
        void *block = allocate(context, bytes);
        if (block == NULL) break;
        if (swdec_me_address_visible((uintptr_t) block, bytes)) return block;
        if (search->count == search->max_held) {
            release(context, block);
            break;
        }
        search->held[search->count++] = block;
        search->passed_over++;
    }
    return accept_high && search->count != 0
        ? search->held[--search->count] : NULL;
}

static inline void swdec_low_search_end(
    SwdecLowSearch *search, SwdecRelease release, void *context)
{
    while (search->count != 0) release(context, search->held[--search->count]);
}

/* One allocation, searched on its own. */
static inline void *swdec_alloc_me_visible(
    SwdecAllocate allocate, SwdecRelease release, void *context,
    size_t bytes, unsigned max_held, bool accept_high, unsigned *held_out)
{
    SwdecLowSearch search;
    swdec_low_search_begin(&search, max_held);
    void *found = swdec_low_search_take(
        &search, allocate, release, context, bytes, accept_high);
    if (held_out != NULL) *held_out = search.passed_over;
    swdec_low_search_end(&search, release, context);
    return found;
}

/* The audio decoder's failures, block by block (the Media Engine reports
 * an error code in place of a sample count). A run of SWDEC_AUDIO_RESET_RUN
 * failures is when the component flushes its decoder; SWDEC_AUDIO_FAIL_RUN
 * (about 2.2 s at 44.1 kHz) gives the track up: video keeps playing on a
 * silent clock rather than on a decoder that will not recover. */
#define SWDEC_AUDIO_RESET_RUN 8u
#define SWDEC_AUDIO_FAIL_RUN 96u
typedef struct {
    unsigned decoded;
    unsigned errors;
    unsigned run;
    unsigned longest_run;
    int first_error;
    bool failed;
    unsigned seek_retries;    /* fresh chances a seek has given */
} SwdecAudioErrors;

/* `samples` as the component reports it; true when this block makes the
 * track fail. */
static inline bool swdec_audio_errors_note(SwdecAudioErrors *state,
                                           unsigned samples)
{
    int code = (int) samples;
    if (code >= 0) {
        state->decoded++;
        state->run = 0;
        return false;
    }
    if (state->errors++ == 0) state->first_error = code;
    state->run++;
    if (state->run > state->longest_run) state->longest_run = state->run;
    if (!state->failed && state->run >= SWDEC_AUDIO_FAIL_RUN) {
        state->failed = true;
        return true;
    }
    return false;
}

/* A seek replaces the audio decoder: a given-up track gets a fresh chance
 * there (its failure run counted again from zero), at most
 * SWDEC_AUDIO_SEEK_RETRIES times per stream so a track that never decodes
 * does not cycle through give-ups. True when the track plays after the
 * seek; false when it stays given up (the clock stays on silence). */
#define SWDEC_AUDIO_SEEK_RETRIES 2u
static inline bool swdec_audio_errors_seek(SwdecAudioErrors *state)
{
    state->run = 0;
    if (!state->failed) return true;
    if (state->seek_retries >= SWDEC_AUDIO_SEEK_RETRIES) return false;
    state->seek_retries++;
    state->failed = false;
    return true;
}

/* Where a given-up track's silent clock starts after a seek: at the first
 * audio packet's time once one has arrived (`origin_ready`), with video
 * once the first picture of the new position is shown, as sound would.
 * With no audio left to arrive it starts at the end of that picture. False
 * while neither is known: run from zero, the clock would hold video until
 * it had counted up to the seek position. */
static inline bool swdec_silent_clock_start(
    bool origin_ready, uint64_t origin_us, bool input_ended,
    uint64_t shown_end_us, bool audio_only, uint64_t *cursor_us)
{
    if (origin_ready && (audio_only || shown_end_us != 0)) {
        *cursor_us = origin_us;
        return true;
    }
    if (input_ended && (audio_only || shown_end_us != 0)) {
        *cursor_us = shown_end_us;
        return true;
    }
    return false;
}

#endif
