#ifndef TILEFINCH_PLATFORM_H
#define TILEFINCH_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "tilefinch/budget.h"
#include "tilefinch/media_capture.h"
#include "tilefinch/page_dialog.h"

typedef struct {
    uint32_t buttons;
    int16_t analog_x;
    int16_t analog_y;
} TilefinchPlatformInput;

typedef enum {
    TILEFINCH_DATE_FORMAT_YEAR_MONTH_DAY = 0,
    TILEFINCH_DATE_FORMAT_MONTH_DAY_YEAR,
    TILEFINCH_DATE_FORMAT_DAY_MONTH_YEAR
} TilefinchDateFormat;

enum {
    TILEFINCH_PLATFORM_BUTTON_UP = 1u << 0,
    TILEFINCH_PLATFORM_BUTTON_DOWN = 1u << 1,
    TILEFINCH_PLATFORM_BUTTON_LEFT = 1u << 2,
    TILEFINCH_PLATFORM_BUTTON_RIGHT = 1u << 3,
    TILEFINCH_PLATFORM_BUTTON_CROSS = 1u << 4,
    TILEFINCH_PLATFORM_BUTTON_CIRCLE = 1u << 5,
    TILEFINCH_PLATFORM_BUTTON_LTRIGGER = 1u << 6,
    TILEFINCH_PLATFORM_BUTTON_RTRIGGER = 1u << 7
};

/* What a cooperate checkpoint means to a frontend, beyond its phase name
   (which is for logs only). A checkpoint carries any combination. */
typedef enum {
    /* Resumable owner boundaries: a frontend may defer the next unit of
       work (return false) and keep completed work. */
    TILEFINCH_COOPERATE_PAGE_TASK = 1u << 0,        /* between author tasks */
    TILEFINCH_COOPERATE_RENDER_INPUT = 1u << 1,     /* between raster units */
    TILEFINCH_COOPERATE_IMAGE_ADMISSION = 1u << 2,  /* before image requests */
    /* A focus move's first safe point: native focus feedback may present. */
    TILEFINCH_COOPERATE_FOCUS_FEEDBACK = 1u << 3,
    /* Brackets (work units 1, then 0) a relayout beside a committed layout
       that page scrolling may keep serving. */
    TILEFINCH_COOPERATE_SCROLL_SERVABLE = 1u << 4,
    /* Brackets optional work that holds no author state (a font or image
       publication); a frontend may abandon it for input and it reruns. */
    TILEFINCH_COOPERATE_OPTIONAL_PUBLICATION = 1u << 5,
    /* With OPTIONAL_PUBLICATION: an image publication, which also yields to
       page input queued during the decode before it. */
    TILEFINCH_COOPERATE_IMAGE_PUBLICATION = 1u << 6,
    /* Brackets a provisional page's layout completion, whose checkpoints
       may serve page scrolling; EXTENSION_AWAITED when a reader waits at
       the bottom for the screens it adds. */
    TILEFINCH_COOPERATE_LAYOUT_COMPLETION = 1u << 7,
    TILEFINCH_COOPERATE_EXTENSION_AWAITED = 1u << 8,
    /* Brackets (1, then 0) composition writes during which the incumbent
       frame may not be sampled. */
    TILEFINCH_COOPERATE_RENDER_COMPOSE = 1u << 9,
    /* The document or render shell is borrowed by the work this checkpoint
       interrupts: a frontend may present immutable incumbent pixels but
       must not mutate or reenter it. */
    TILEFINCH_COOPERATE_RENDER_SHELL_BORROWED = 1u << 10
} TilefinchCooperateBoundary;

typedef struct {
    void *context;
    uint64_t (*wall_time_ns)(void *context);
    uint64_t (*monotonic_time_ns)(void *context);
    /* Native microsecond clock for 32-bit devices whose kernel already
       exposes this unit. Hot scheduling checkpoints should use this rather
       than manufacturing nanoseconds and dividing them back down. */
    uint64_t (*monotonic_time_us)(void *context);
    /* Optional stable BCP-47 language tag. Implementations may query the
       device lazily on first use; the returned storage must remain valid for
       the installed service lifetime. */
    const char *(*preferred_language)(void *context);
    /* Optional numeric date order. It is consulted only when a provider did
       not supply localized display text, so implementations may query the
       device lazily without adding work to startup. */
    TilefinchDateFormat (*preferred_date_format)(void *context);
    bool (*secure_random)(void *context, void *data, size_t length);
    bool (*read_asset)(void *context, Budget *budget, const char *path,
                       size_t maximum_bytes, unsigned char **data,
                       size_t *length);
    bool (*poll_input)(void *context, TilefinchPlatformInput *input);
    bool (*present_rgb565)(void *context, const uint16_t *pixels,
                           size_t width, size_t height, size_t stride_pixels);
    /* Optional synchronous, bit-exact snapshot copy. False asks the engine
       to use memcpy. No format conversion or publication takes place. */
    bool (*copy_pixel_rows)(void *context, unsigned char *destination,
                            size_t destination_stride,
                            const unsigned char *source, size_t source_stride,
                            size_t row_bytes, size_t rows);
    /* Optional: pixels once presented are about to be freed. A frontend
       that repaints the last presented frame from another thread stops
       using them before this returns. */
    void (*retire_frame)(void *context, const uint16_t *pixels);
    /* Called at deterministic engine work boundaries. Returning false asks
       the current operation to cancel without committing partial state.
       At a PAGE_TASK, RENDER_INPUT or IMAGE_ADMISSION boundary it instead
       defers the next resumable unit, retaining completed work. A
       RENDER_SHELL_BORROWED checkpoint may present immutable incumbent
       pixels but must not mutate/reenter the borrowed document/render
       shell; RENDER_COMPOSE brackets writes during which that incumbent may
       not be sampled (TilefinchCooperateBoundary). A frontend that needs
       the flags installs cooperate_at. */
    bool (*cooperate)(void *context, const char *phase,
                      size_t completed_work_units);
    /* Optional: the same checkpoint with its TilefinchCooperateBoundary
       flags. When installed it is called instead of cooperate. */
    bool (*cooperate_at)(void *context, const char *phase,
                         size_t completed_work_units, unsigned boundary);
    void (*log_message)(void *context, const char *message);
    /* Optional: show a page's alert/confirm/prompt and block until the user
       answers (tilefinch/page_dialog.h). Called on the engine's owner
       thread from inside the page's script, which stays suspended: the
       frontend must not reenter the engine, and only presents immutable
       incumbent pixels. False means no dialog could be shown, and the page
       gets the default-deny answer. */
    bool (*page_dialog)(void *context,
                        const TilefinchPageDialogRequest *request,
                        TilefinchPageDialogResponse *response);
    /* Optional: the microphone for page capture (tilefinch/media_capture.h),
       all called on the engine's owner thread. Without media_capture_start
       there is no microphone and every request is NotFoundError.
       - available: whether this device has an input at all;
       - permission: the decision remembered for ORIGIN, and whether an
         "Always allow" for it could be remembered (*can_remember);
       - prompt: show the browser-owned prompt and block until it is
         answered, under the same rules as page_dialog; false means no
         prompt could be shown (the request is refused, nothing kept);
       - remember: store GRANTED or DENIED for ORIGIN (false when full);
       - start: open the input, filling *format; returns GRANTED, NO_DEVICE,
         BUSY or FAILED;
       - stop: close the input and clear every sample it held. */
    bool (*media_capture_available)(void *context);
    MediaCapturePermission (*media_capture_permission)(
        void *context, const char *origin, bool *can_remember);
    bool (*media_capture_prompt)(void *context,
                                 const TilefinchMediaCapturePrompt *request,
                                 MediaCapturePromptAnswer *answer);
    bool (*media_capture_remember)(void *context, const char *origin,
                                   MediaCapturePermission permission);
    MediaCaptureVerdict (*media_capture_start)(void *context,
                                               MediaCaptureFormat *format);
    void (*media_capture_stop)(void *context);
    /* The samples since *cursor (media_capture_ring_since). */
    uint32_t (*media_capture_read_since)(void *context, float *output,
                                         uint32_t count, uint32_t *cursor);
    /* Optional: the input started and has since failed or stalled; the
       engine then ends capture (MEDIA_CAPTURE_END_DEVICE). */
    bool (*media_capture_failed)(void *context);
    /* Optional: with page capture stopped, whether the input is free for
       the frontend's own capture (voice typing): no worker left and no
       transfer that may still write. Answered at once. Without it, true. */
    bool (*media_capture_input_free)(void *context);
    /* Optional: whether audio output goes to headphones rather than a
       speaker the microphone can hear. Page microphone audio is heard only
       while this is true; without it, never. */
    bool (*audio_output_private)(void *context);
    /* Native user navigation only. No page text, draft or credentials cross
       this boundary. Missing service or dismissal means stay on the page. */
    bool (*confirm_draft_loss)(void *context);
} TilefinchPlatformServices;

bool tilefinch_platform_confirm_draft_loss(void);

void tilefinch_platform_set_services(const TilefinchPlatformServices *services);

uint64_t tilefinch_platform_wall_time_ns(void);
uint64_t tilefinch_platform_monotonic_time_ns(void);
uint64_t tilefinch_platform_monotonic_time_us(void);
const char *tilefinch_platform_preferred_language(void);
TilefinchDateFormat tilefinch_platform_preferred_date_format(void);
bool tilefinch_platform_secure_random(void *data, size_t length);
bool tilefinch_platform_read_asset(Budget *budget, const char *path,
                                size_t maximum_bytes, unsigned char **data,
                                size_t *length);
bool tilefinch_platform_poll_input(TilefinchPlatformInput *input);
bool tilefinch_platform_present_rgb565(const uint16_t *pixels, size_t width,
                                    size_t height, size_t stride_pixels);
void tilefinch_platform_retire_frame(const uint16_t *pixels);
/* The caller supplies nonoverlapping, valid row spans. Completes all rows
   synchronously, including when the optional platform accelerator declines. */
void tilefinch_platform_copy_pixel_rows(
    unsigned char *destination, size_t destination_stride,
    const unsigned char *source, size_t source_stride,
    size_t row_bytes, size_t rows);
/* A checkpoint with no boundary meaning: work a frontend may only pace. */
bool tilefinch_platform_cooperate(const char *phase,
                               size_t completed_work_units);
/* A checkpoint whose meaning is BOUNDARY (TilefinchCooperateBoundary
   flags); PHASE names it for logs. */
bool tilefinch_platform_cooperate_at(const char *phase,
                                     size_t completed_work_units,
                                     unsigned boundary);
/* The render shell brackets its own raster and composition with these.
   Every checkpoint reached inside -- an image decoder's or a font's, under
   whatever name -- then carries TILEFINCH_COOPERATE_RENDER_SHELL_BORROWED.
   Brackets nest; the engine's owner thread only. */
void tilefinch_platform_render_shell_borrow_begin(void);
void tilefinch_platform_render_shell_borrow_end(void);
void tilefinch_platform_log_message(const char *message);
/* False when no frontend dialog service is installed or it declined. */
bool tilefinch_platform_page_dialog(const TilefinchPageDialogRequest *request,
                                    TilefinchPageDialogResponse *response);
/* The media_capture services; each is the "no microphone" answer when the
   frontend installed none (unavailable, PROMPT, no prompt, NO_DEVICE). */
bool tilefinch_platform_media_capture_available(void);
MediaCapturePermission tilefinch_platform_media_capture_permission(
    const char *origin, bool *can_remember);
bool tilefinch_platform_media_capture_prompt(
    const TilefinchMediaCapturePrompt *request,
    MediaCapturePromptAnswer *answer);
bool tilefinch_platform_media_capture_remember(
    const char *origin, MediaCapturePermission permission);
MediaCaptureVerdict tilefinch_platform_media_capture_start(
    MediaCaptureFormat *format);
void tilefinch_platform_media_capture_stop(void);
bool tilefinch_platform_audio_output_private(void);
uint32_t tilefinch_platform_media_capture_read_since(float *output,
                                                    uint32_t count,
                                                    uint32_t *cursor);
bool tilefinch_platform_media_capture_failed(void);
bool tilefinch_platform_media_capture_input_free(void);
/* Publishes the closed file TEMPORARY under PATH on a filesystem whose
   rename may refuse to replace an existing file (the PSP's FAT).

   With BACKUP, the previous PATH is first rotated to BACKUP (an older
   BACKUP is removed only if the rename refuses to replace it, while PATH is
   still whole) and moved back if TEMPORARY cannot be installed, so a
   complete generation exists under PATH or BACKUP at every instant.
   Without BACKUP, an atomic replace is tried first; only on a destination
   collision (and when TEMPORARY and PATH are files) is PATH moved to PATH.old and the rename
   retried. A failed retry moves PATH back; a power loss in that window
   leaves PATH.old (removed by the next replace) and TEMPORARY.

   TEMPORARY is never removed: on failure the caller decides whether it is
   discarded or kept for recovery. */
bool tilefinch_platform_replace_file(const char *temporary, const char *path,
                                     const char *backup);
/* Publish without ever creating an aside or backup. If the filesystem
   cannot replace an existing name, retire it before installing the stage.
   The old file may be lost on failure; the caller owns stage cleanup. */
bool tilefinch_platform_replace_file_no_copy(const char *temporary,
                                             const char *path);
/* Finish a private, disposable staging file. Takes ownership of the open
   stream. Only a successful writer and close can publish; all other paths
   discard the stage. Use replace_file directly for recoverable stages. */
bool tilefinch_platform_finish_file(FILE *file, bool complete,
                                   const char *temporary, const char *path,
                                   const char *backup);
/* fprintf for short records: formats with vsnprintf into a 1 KiB stack
   buffer and writes the text with one fwrite. The shipping PSP image
   formats only through snprintf, so writing files this way keeps newlib's
   stream printf core (_vfprintf_r, about 13.5 KB of .text) out of it.
   Returns the bytes written, or -1 when the text does not fit the buffer
   or the write fails; longer values go out with fputs or fwrite. */
int tilefinch_platform_file_printf(FILE *file, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
/* Formats a message into a caller's error buffer (truncating to fit); does
   nothing when ERROR is NULL or ERROR_SIZE is 0. Modules alias it under
   their own short name (`#define hls_error tilefinch_format_error`). */
void tilefinch_format_error(char *error, size_t error_size,
                            const char *format, ...)
    __attribute__((format(printf, 3, 4)));
/* Validation builds: name a step of engine work so that a visible gap
   between two cooperate checkpoints can be attributed to the steps that ran
   inside it (`label` must be a string literal). No-op otherwise. */
void tilefinch_platform_trace_step(const char *label);

#endif
