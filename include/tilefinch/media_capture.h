#ifndef TILEFINCH_MEDIA_CAPTURE_H
#define TILEFINCH_MEDIA_CAPTURE_H

/*
 * Page microphone capture: navigator.mediaDevices.getUserMedia({audio}).
 *
 * The engine owns who may capture (secure context, Permissions Policy
 * delegation to frames, visibility, the user's stored decision and the
 * prompt's anti-abuse limits), the one process-wide capture session that
 * every live track of the page shares, and the bounded sample ring pages
 * read through Web Audio. A frontend supplies the hardware and the
 * browser-owned prompt through the platform's media_capture services
 * (tilefinch/platform.h): a frontend without them has no microphone, and
 * every request is NotFoundError.
 *
 * Nothing here stores audio beyond the ring: no file, no Memory Stick, no
 * network. The ring holds the newest MEDIA_CAPTURE_READ_SAMPLES a reader
 * may take and is zeroed when capture stops. The prompt's modal input rules are here too,
 * so the PSP loop and the host tests run the same state machine.
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One mono 16-bit stream at the AudioContext rate, so a
   MediaStreamAudioSourceNode needs no resampler. */
#define MEDIA_CAPTURE_SAMPLE_RATE 44100u
#define MEDIA_CAPTURE_CHANNELS 1u
/* The hardware transfer, about 23 ms. */
#define MEDIA_CAPTURE_BLOCK_SAMPLES 1024u
/* The most samples the one reader (the Web Audio pump) takes per read;
   analysers keep their own history in the engine. */
#define MEDIA_CAPTURE_READ_SAMPLES 4096u
/* What one read may take plus the blocks a writer may be filling
   meanwhile: 8,192 samples, 16 KiB while the microphone is live. */
#define MEDIA_CAPTURE_RING_SAMPLES \
    (MEDIA_CAPTURE_READ_SAMPLES + 4u * MEDIA_CAPTURE_BLOCK_SAMPLES)
/* Live tracks one realm may hold (each getUserMedia() or clone() adds one). */
#define MEDIA_CAPTURE_TRACK_LIMIT 8u
/* Prompt anti-abuse, per page load: after a dismissal the page may ask
   again only after a new user activation, and never after this many. */
#define MEDIA_CAPTURE_DISMISS_LIMIT 3u
/* Allow answers are taken only once the prompt has been on screen this long
   and every button held when it opened is released (as page dialogs). */
#define MEDIA_CAPTURE_PROMPT_ARM_MS 500u
/* An unanswered prompt is dismissed after this long. */
#define MEDIA_CAPTURE_PROMPT_MAXIMUM_OPEN_MS (2u * 60u * 1000u)
/* "<host>" or "<host>:<port>", as page dialogs title themselves. */
#define MEDIA_CAPTURE_HOST_BYTES 260u

/* A site's microphone permission state (Permissions API names). */
typedef enum {
    MEDIA_CAPTURE_PERMISSION_PROMPT = 0,
    MEDIA_CAPTURE_PERMISSION_GRANTED,
    MEDIA_CAPTURE_PERMISSION_DENIED
} MediaCapturePermission;

/* The outcome of one getUserMedia() request. */
typedef enum {
    MEDIA_CAPTURE_GRANTED = 0,
    MEDIA_CAPTURE_DENIED_REALM,      /* not a document realm */
    MEDIA_CAPTURE_DENIED_INSECURE,   /* not a secure context */
    MEDIA_CAPTURE_DENIED_POLICY,     /* frame without "microphone" */
    MEDIA_CAPTURE_DENIED_HIDDEN,     /* page not visible: never prompts */
    MEDIA_CAPTURE_DENIED_BLOCKED,    /* the user's stored Block */
    MEDIA_CAPTURE_DENIED_DISMISSED,  /* the prompt was closed unanswered */
    MEDIA_CAPTURE_DENIED_EMBARGO,    /* dismissal limit, or no new gesture */
    MEDIA_CAPTURE_DENIED_UNAVAILABLE,/* the frontend cannot prompt now */
    MEDIA_CAPTURE_DENIED_LIMIT,      /* MEDIA_CAPTURE_TRACK_LIMIT reached */
    MEDIA_CAPTURE_NO_DEVICE,         /* no microphone on this PSP */
    MEDIA_CAPTURE_BUSY,              /* voice typing holds the input */
    MEDIA_CAPTURE_FAILED,            /* the hardware refused or stalled */
    MEDIA_CAPTURE_VERDICT_COUNT
} MediaCaptureVerdict;

/* The DOMException name a page sees for VERDICT ("" for GRANTED). */
const char *media_capture_verdict_error_name(MediaCaptureVerdict verdict);
/* A short, fixed message for VERDICT (no page or user data). */
const char *media_capture_verdict_message(MediaCaptureVerdict verdict);

/* Why the capture session ended; a track ended by anything but the page's
   own stop() fires "ended". */
typedef enum {
    MEDIA_CAPTURE_END_NONE = 0,  /* still live */
    MEDIA_CAPTURE_END_PAGE,      /* the page stopped its last track */
    MEDIA_CAPTURE_END_USER,      /* the browser's Stop microphone */
    MEDIA_CAPTURE_END_NAVIGATION,/* the page went away */
    MEDIA_CAPTURE_END_SUSPEND,   /* PSP sleep or HOME exit */
    MEDIA_CAPTURE_END_VOICE,     /* voice typing needed the input */
    MEDIA_CAPTURE_END_MEDIA,     /* a native media session took over */
    MEDIA_CAPTURE_END_PERMISSION,/* the site's permission was revoked */
    MEDIA_CAPTURE_END_DEVICE,    /* the hardware stopped delivering */
    MEDIA_CAPTURE_END_SYSTEM_SCREEN /* a firmware screen (the system
                                       keyboard) would hide the indicator */
} MediaCaptureEndReason;

/* ---- The prompt ------------------------------------------------------- */

typedef enum {
    /* Circle, HOME, a timeout, or a frontend dismissal: NotAllowedError,
       and nothing is remembered. */
    MEDIA_CAPTURE_PROMPT_DISMISSED = 0,
    /* "Allow this time": until the page is navigated away or closed. */
    MEDIA_CAPTURE_PROMPT_ALLOW_ONCE,
    /* "Always allow on this site": the origin is remembered. */
    MEDIA_CAPTURE_PROMPT_ALLOW_ALWAYS,
    /* "Block": the origin is remembered as denied. */
    MEDIA_CAPTURE_PROMPT_BLOCK
} MediaCapturePromptAnswer;

/* The prompt's rows, top to bottom. */
#define MEDIA_CAPTURE_PROMPT_ROW_COUNT 3u

typedef struct {
    /* The top-level page's lower-case ASCII host (with ":port" when the URL
       names one), which the prompt names; never the page's own text. */
    const char *host;
    /* The top-level origin the decision is remembered under. */
    const char *origin;
    /* The request came from a frame the top-level page delegated to. */
    bool from_frame;
    /* "Always allow" can be remembered (the site table has room). */
    bool can_remember;
} TilefinchMediaCapturePrompt;

enum {
    MEDIA_CAPTURE_BUTTON_UP = 1u << 0,
    MEDIA_CAPTURE_BUTTON_DOWN = 1u << 1,
    MEDIA_CAPTURE_BUTTON_CROSS = 1u << 2,
    MEDIA_CAPTURE_BUTTON_CIRCLE = 1u << 3
};

/* The modal is still waiting for an answer. */
#define MEDIA_CAPTURE_PROMPT_PENDING (-1)

typedef struct {
    uint8_t selection;
    bool can_remember;
    bool armed;
    uint32_t previous_buttons;
    uint32_t unreleased_buttons;
    uint64_t opened_ms;
} MediaCapturePromptModal;

/* HELD is the MEDIA_CAPTURE_BUTTON_* state when the prompt opened. The
   selection starts on "Allow this time". */
void media_capture_prompt_modal_open(MediaCapturePromptModal *modal,
                                     bool can_remember, uint32_t held,
                                     uint64_t now_ms);
/* One sample of the held buttons; edges answer. Returns a
   MediaCapturePromptAnswer, or MEDIA_CAPTURE_PROMPT_PENDING. Up/Down move
   the selection (skipping "Always" when it cannot be remembered); X takes
   the selected row (an allow only once armed); Circle dismisses at once;
   MEDIA_CAPTURE_PROMPT_MAXIMUM_OPEN_MS dismisses. */
int media_capture_prompt_modal_step(MediaCapturePromptModal *modal,
                                    uint32_t held, uint64_t now_ms);
/* The answer row ROW stands for. */
MediaCapturePromptAnswer media_capture_prompt_row_answer(unsigned row);

/* ---- Permissions Policy for frames ----------------------------------- */

/* Whether a child frame may use the microphone. The feature's default
   allowlist is 'self': a same-origin child of an allowed parent may, a
   cross-origin one only when its container's allow attribute (ALLOW,
   LENGTH bytes, possibly NULL) delegates "microphone" to it: '*'; no
   allowlist or 'src' while the child (CHILD_ORIGIN) is still at
   SRC_ORIGIN, the origin of the container's src when the frame was made;
   'self' for a same-origin child; or its exact origin. 'none' refuses. A
   frame with an opaque origin never may. */
bool media_capture_frame_allowed(const char *allow, size_t length,
                                 bool same_origin_with_parent,
                                 bool opaque_origin,
                                 const char *child_origin,
                                 const char *src_origin,
                                 bool parent_allowed);

/* ---- The sample ring --------------------------------------------------- */

/* Single producer (the capture worker), single consumer (the browser
   thread). WRITTEN counts every sample ever published, modulo 2^32. */
typedef struct {
    int16_t *samples;
    uint32_t capacity;
    _Atomic uint32_t written;
} MediaCaptureRing;

void media_capture_ring_init(MediaCaptureRing *ring, int16_t *storage,
                             uint32_t capacity);
/* Producer: append COUNT samples (at most capacity/4 at a time). */
void media_capture_ring_publish(MediaCaptureRing *ring,
                                const int16_t *samples, uint32_t count);
/* How many samples the ring holds (saturating at its capacity). */
uint32_t media_capture_ring_written(const MediaCaptureRing *ring);
/* Consumer: the samples published since *CURSOR (a ring position this
   function returned; UINT32_MAX starts at the newest sample and reads
   none), at most COUNT of them, the newest when more are waiting, as
   floats in [-1, 1). Moves *CURSOR past them and returns how many. A
   reader that falls a whole ring behind sees only part of the gap. */
uint32_t media_capture_ring_since(const MediaCaptureRing *ring,
                                  uint32_t *cursor, float *output,
                                  uint32_t count);

/* Level statistics of one block, for logs: peak magnitude, and the mean
   square in [0, 32768^2]. */
void media_capture_block_levels(const int16_t *samples, size_t count,
                                unsigned *peak, uint64_t *mean_square);

/* ---- The process's capture session ------------------------------------ */

/*
 * One microphone, shared by every live track of the visible page. The
 * engine opens the session for each granted request and releases it as
 * tracks stop; the device starts with the first track and stops with the
 * last. A frontend ends it from outside the page (Stop microphone, sleep,
 * voice typing, native media) with tilefinch_media_capture_stop(); the
 * page's tracks then end at its next task boundary, with "ended" events.
 * Browser thread only.
 */
typedef struct {
    uint32_t sample_rate;
    uint32_t channels;
    /* What the frontend measured or knows of its input path, in seconds. */
    double latency;
} MediaCaptureFormat;

/* Frontends: stop capture now. No-op when nothing is live. */
void tilefinch_media_capture_stop(MediaCaptureEndReason reason);
/* Frontends, before capturing themselves (voice typing): stop page capture
   with REASON and answer whether the input is free now. False means a
   transfer of the stopped capture may still be running: the frontend must
   not capture, and says the microphone is busy. Never waits. */
bool tilefinch_media_capture_hand_over(MediaCaptureEndReason reason);
/* Frontends: whether the microphone is live (the indicator). */
bool tilefinch_media_capture_live(void);
/* Whether the "microphone on" indicator shows: true from the device's
   start to its stop. Safe from any thread. */
bool tilefinch_media_capture_indicator(void);
/* The live session's generation (0 when none). */
uint32_t tilefinch_media_capture_generation(void);
/* Frontends: a remembered decision changed outside the page (a menu row,
   Reset permissions). Pages re-read permissions.query() states and every
   "Allow this time" is forgotten; a frontend that revoked a grant also
   stops a live capture with MEDIA_CAPTURE_END_PERMISSION. */
void tilefinch_media_capture_permissions_changed(void);
/* Bumped by every decision change; pages compare it per task boundary. */
uint32_t tilefinch_media_capture_permission_epoch(void);
/* Bumped only by tilefinch_media_capture_permissions_changed(): a change
   from outside the page, which also forgets every "Allow this time". */
uint32_t media_capture_revoke_epoch(void);
/* Engine: a page's own prompt answer changed a decision. */
void media_capture_permission_noted(void);

/* Engine: add one track to the session, starting the device when it is the
   first. On GRANTED, *GENERATION names the session and *FORMAT describes
   it. */
MediaCaptureVerdict media_capture_session_open(uint32_t *generation,
                                               MediaCaptureFormat *format);
/* Engine: TRACKS tracks of GENERATION stopped; the last one stops the
   device. A stale generation is ignored. */
void media_capture_session_release(uint32_t generation, unsigned tracks);
/* Engine: why GENERATION ended, or MEDIA_CAPTURE_END_NONE while live. A
   live session whose input the frontend reports failed
   (media_capture_failed) ends first, with MEDIA_CAPTURE_END_DEVICE; so do
   tilefinch_media_capture_live() and the reads. */
MediaCaptureEndReason media_capture_session_end_reason(uint32_t generation);
/* Engine: the live session's samples since *CURSOR, as
   media_capture_ring_since (0 and *CURSOR unchanged when none is live). */
uint32_t media_capture_session_read_since(float *output, uint32_t count,
                                          uint32_t *cursor);

#endif
