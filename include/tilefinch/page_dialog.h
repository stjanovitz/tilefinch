#ifndef TILEFINCH_PAGE_DIALOG_H
#define TILEFINCH_PAGE_DIALOG_H

/*
 * HTML simple dialogs: window.alert(), confirm() and prompt().
 *
 * The engine owns who may open a dialog and how often (PageDialogPolicy),
 * what the user is shown (sanitized, bounded plain text titled with the
 * page's host), and what the page gets back. A frontend only draws the
 * request and reports the user's answer through the platform's page_dialog
 * service (tilefinch/platform.h); a frontend without that service, or one
 * that declines, is the HTML "cannot show simple dialogs" case and the page
 * receives the default-deny answer: nothing, false, or null.
 *
 * The modal's input rules are here too (PageDialogModal), so the PSP loop
 * and the host tests run the same state machine.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    PAGE_DIALOG_ALERT = 0,
    PAGE_DIALOG_CONFIRM,
    PAGE_DIALOG_PROMPT
} PageDialogKind;

/* UTF-16 code units read from each JavaScript argument. Longer strings are
   cut before they are converted, so a page cannot make the engine copy an
   arbitrarily large string. */
#define PAGE_DIALOG_SOURCE_UNITS 1024u
/* The message the user sees, in UTF-8 bytes including the terminator, and
   its line count. A longer message ends with an ellipsis. */
#define PAGE_DIALOG_MESSAGE_BYTES 640u
#define PAGE_DIALOG_MESSAGE_LINES 8u
/* prompt()'s default and answer, in UTF-8 bytes including the terminator. */
#define PAGE_DIALOG_PROMPT_BYTES 256u
/* "<host>" or "<host>:<port>" for the title: room for a 253-byte host,
   a five-digit port and the terminator, so no host is ever left out. The
   dialog shortens it on screen from the left (psp_ui_host_label). */
#define PAGE_DIALOG_HOST_BYTES 260u

/* Anti-abuse limits, per page load (one document's realm and its
   same-origin frames):
   - at most PAGE_DIALOG_GESTURE_LIMIT dialogs between two trusted user
     activations of the page (the dialog's own buttons are not one);
   - at most PAGE_DIALOG_RATE_LIMIT dialogs opened in any
     PAGE_DIALOG_RATE_WINDOW_MS, whatever the activations;
   - from the second dialog, the user may stop the page's dialogs for the
     rest of the load.
   Every refused call returns the default-deny answer at once, so a loop of
   alert() ends after a few dialogs instead of trapping the user. */
#define PAGE_DIALOG_GESTURE_LIMIT 3u
#define PAGE_DIALOG_RATE_LIMIT 5u
#define PAGE_DIALOG_RATE_WINDOW_MS 30000u
/* An answer that says yes (X, or Start in prompt) is taken only once the
   dialog has been on screen this long and every button held when it opened
   has been released, so a button held or mashed through a loop of dialogs
   cannot accept one the user never read. Circle, Triangle and Square act at
   once: each of them refuses. */
#define PAGE_DIALOG_ARM_MS 500u
/* A dialog nobody answers is dismissed (the default-deny answer) after this
   long, so the modal loop is bounded even when the user walks away. */
#define PAGE_DIALOG_MAXIMUM_OPEN_MS (10u * 60u * 1000u)

typedef enum {
    PAGE_DIALOG_ADMITTED = 0,
    PAGE_DIALOG_DENIED_REALM,       /* not a dialog-capable document realm */
    PAGE_DIALOG_DENIED_FRAME,       /* cross-origin or sandboxed frame */
    PAGE_DIALOG_DENIED_HIDDEN,      /* background, or becoming hidden */
    PAGE_DIALOG_DENIED_SUPPRESSED,  /* the user stopped this page's dialogs */
    PAGE_DIALOG_DENIED_GESTURE,     /* gesture limit reached */
    PAGE_DIALOG_DENIED_RATE,        /* rate window full */
    PAGE_DIALOG_DENIED_NESTED,      /* another dialog is already open */
    PAGE_DIALOG_DENIED_UNAVAILABLE, /* the frontend cannot show one now */
    PAGE_DIALOG_VERDICT_COUNT
} PageDialogVerdict;

typedef enum {
    /* Circle, or a frontend dismissal: alert returns, confirm is false,
       prompt is null. */
    PAGE_DIALOG_OUTCOME_DISMISSED = 0,
    /* X (alert, confirm) or the accepted text (prompt). */
    PAGE_DIALOG_OUTCOME_ACCEPTED,
    /* "Don't let this page show dialogs": dismissed, and every later call
       in this page load is refused. */
    PAGE_DIALOG_OUTCOME_SUPPRESS,
    /* Leave the page: dismissed and suppressed; the frontend then goes Back
       (or stops the load) once the page's script has returned. */
    PAGE_DIALOG_OUTCOME_LEAVE
} PageDialogOutcome;

typedef struct {
    PageDialogKind kind;
    /* Lower-case ASCII host, with ":port" when the URL names one; empty
       when the document has no host (an opaque origin, a data: URL), which
       a frontend titles "This page says". */
    const char *host;
    /* Sanitized plain text: at most PAGE_DIALOG_MESSAGE_LINES lines
       separated by '\n', no control or bidi-override characters. */
    const char *message;
    /* prompt() only: the sanitized single-line default value. */
    const char *default_value;
    /* Offer "Don't let this page show dialogs" (the second dialog on). */
    bool offer_suppress;
} TilefinchPageDialogRequest;

typedef struct {
    PageDialogOutcome outcome;
    /* prompt(): the frontend writes the accepted text here, NUL-terminated,
       at most text_capacity bytes including the terminator. */
    char *text;
    size_t text_capacity;
} TilefinchPageDialogResponse;

/* Per page load. Zero-initialized storage is a fresh load. */
typedef struct {
    uint32_t shown;
    uint32_t since_gesture;
    uint32_t gesture_generation;
    uint64_t opened_ms[PAGE_DIALOG_RATE_LIMIT];
    uint8_t opened_next;
    bool suppressed;
    uint32_t denied[PAGE_DIALOG_VERDICT_COUNT];
} PageDialogPolicy;

/* The count-and-rate part of admission. GESTURE_GENERATION changes with
   every trusted user activation of the page; NOW_MS is monotonic. */
PageDialogVerdict page_dialog_policy_admit(
    PageDialogPolicy *policy, uint32_t gesture_generation, uint64_t now_ms);
/* Counts a dialog that is about to be shown (call after an ADMITTED
   verdict, before the frontend runs). */
void page_dialog_policy_opened(PageDialogPolicy *policy, uint64_t now_ms);
/* Applies the user's answer; SUPPRESS and LEAVE stop later dialogs. */
void page_dialog_policy_answered(PageDialogPolicy *policy,
                                 PageDialogOutcome outcome);
void page_dialog_policy_note_denied(PageDialogPolicy *policy,
                                    PageDialogVerdict verdict);

/* Copies INPUT (UTF-8, LENGTH bytes) as plain text for display into OUTPUT
   (CAPACITY bytes, always terminated): CR LF, CR, NEL, LS and PS become
   line breaks (or spaces when SINGLE_LINE), tabs become spaces, other C0
   and C1 controls, bidi embeddings, overrides, isolates and marks, and
   invisible format characters are dropped, malformed UTF-8 and surrogates
   become U+FFFD, and leading and trailing blank lines are trimmed. Text
   beyond MAXIMUM_LINES lines or the capacity ends with an ellipsis (U+2026)
   and sets *TRUNCATED. Returns the output length. */
size_t page_dialog_sanitize_text(
    const char *input, size_t length, bool single_line,
    size_t maximum_lines, char *output, size_t capacity, bool *truncated);

/* Copies a prompt() answer, the page's default or the user's text, into OUTPUT
   (CAPACITY bytes, always terminated): whole characters that fit, with spaces,
   line breaks and controls as they are. HTML returns the user's string, so this
   is what the page gets and what must not reach the screen. Only malformed
   UTF-8, surrogates and NUL become U+FFFD. Returns the output length. */
size_t page_dialog_answer_text(const char *input, size_t length,
                               char *output, size_t capacity);

/* The same answer as the keyboard's starting text: one line, with what
   page_dialog_sanitize_text drops left out, and spaces kept so that accepting
   it does not change them. */
size_t page_dialog_editable_text(const char *input, size_t length,
                                 char *output, size_t capacity);

/* The title host of URL: lower-case host and any explicit port of an
   http(s) URL whose host is printable ASCII. False (and an empty OUTPUT)
   otherwise. */
bool page_dialog_title_host(const char *url, char *output, size_t capacity);

/* Whether a child frame may open dialogs: its document is same-origin with
   its parent (and not sandboxed into an opaque origin), any sandbox it has
   includes allow-modals, and its parent may open them (the top-level
   document always may). Cross-origin frames never may, as in Chrome. */
bool page_dialog_frame_allowed(bool same_origin_with_parent,
                               uint32_t sandbox_flags,
                               bool parent_allowed);

/* ---- The modal's input rules ---------------------------------------- */

enum {
    PAGE_DIALOG_BUTTON_CROSS = 1u << 0,
    PAGE_DIALOG_BUTTON_CIRCLE = 1u << 1,
    PAGE_DIALOG_BUTTON_TRIANGLE = 1u << 2,
    PAGE_DIALOG_BUTTON_SQUARE = 1u << 3,
    PAGE_DIALOG_BUTTON_START = 1u << 4
};

typedef enum {
    PAGE_DIALOG_MODAL_WAIT = 0,
    /* prompt(): open the text entry with the current value. */
    PAGE_DIALOG_MODAL_EDIT,
    PAGE_DIALOG_MODAL_ACCEPT,
    PAGE_DIALOG_MODAL_DISMISS,
    PAGE_DIALOG_MODAL_SUPPRESS,
    PAGE_DIALOG_MODAL_LEAVE
} PageDialogModalStep;

typedef struct {
    PageDialogKind kind;
    bool offer_suppress;
    bool armed;
    uint32_t previous_buttons;
    uint32_t unreleased_buttons;
    uint64_t opened_ms;
} PageDialogModal;

/* HELD is the PAGE_DIALOG_BUTTON_* state when the dialog opened. */
void page_dialog_modal_open(PageDialogModal *modal, PageDialogKind kind,
                            bool offer_suppress, uint32_t held,
                            uint64_t now_ms);
/* One sample of the held buttons. Edges, not held state, answer. */
PageDialogModalStep page_dialog_modal_step(PageDialogModal *modal,
                                           uint32_t held, uint64_t now_ms);
/* After the text entry closes: its buttons are released before the
   dialog's own take effect again. */
void page_dialog_modal_resume(PageDialogModal *modal, uint32_t held);

#endif
