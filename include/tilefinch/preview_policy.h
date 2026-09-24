#ifndef TILEFINCH_PREVIEW_POLICY_H
#define TILEFINCH_PREVIEW_POLICY_H

#include <stdbool.h>
#include <stddef.h>

/*
 * When a streaming document's load preview is (re)built, and how deep.
 *
 * Pure state machine: the document stream feeds it what has arrived and
 * where the reader is, asks for a decision at each parser checkpoint, and
 * reports how each attempt ended. The stream keeps the DOM and stylesheet
 * gates (a usable prefix, a current stylesheet, visible pixels, memory);
 * this decides only whether a layout is worth spending.
 *
 *   NONE -------- painted --------> FIRST_SCREEN  (first screen not filled)
 *     |                                  | painted and filled, or out of
 *     |                                  v refreshes
 *     +--------- painted, filled ---> SETTLED
 *   FIRST_SCREEN, SETTLED --- reader scrolled ---> FOLLOWING
 *   any ------------------- given up (memory) ---> ABANDONED
 *
 * A failed attempt leaves the phase unchanged.
 */

/* Rebuilds of a first screen that is still filling in. */
#define PREVIEW_POLICY_REFRESH_LIMIT 3u
/* Body growth that makes such a rebuild worthwhile. */
#define PREVIEW_POLICY_REFRESH_BYTES (16u * 1024u)
/* Previews built to follow a reader, per navigation: each lays the document
   out from the top again. */
#define PREVIEW_POLICY_EXTENSION_LIMIT 24u
/* A preview that already covers everything parsed follows the reader only
   once this much more body has been parsed and at least the current
   element requirement (initially the minimum below) of rendering elements
   has closed. Bytes alone are not content: inline <style> and hidden menus
   can run for tens of kilobytes. */
#define PREVIEW_POLICY_EXTENSION_BYTES (4u * 1024u)
#define PREVIEW_POLICY_EXTENSION_ELEMENTS 32u
#define PREVIEW_POLICY_EXTENSION_ELEMENTS_LIMIT 4096u
/* Admitted checks (past the stream's cheap gates) without new input. */
#define PREVIEW_POLICY_CHECK_LIMIT 4u

typedef enum {
    PREVIEW_PHASE_NONE = 0,
    PREVIEW_PHASE_FIRST_SCREEN,
    PREVIEW_PHASE_SETTLED,
    PREVIEW_PHASE_FOLLOWING,
    PREVIEW_PHASE_ABANDONED
} PreviewPhase;

typedef enum {
    PREVIEW_ACTION_NONE = 0,
    /* The first preview. */
    PREVIEW_ACTION_FIRST,
    /* The first screen again, from more of the document. */
    PREVIEW_ACTION_REFRESH,
    /* Deeper, to follow the reader. */
    PREVIEW_ACTION_EXTEND
} PreviewAction;

typedef struct {
    PreviewPhase phase;
    /* An attempt is in progress (between started and painted/failed). */
    bool building;
    unsigned refreshes;
    unsigned extensions;
    unsigned checks;
    /* Parsed bytes when the last attempt started. */
    size_t attempt_bytes;
    /* Rendering elements the next extension needs, once the live preview
       covers everything parsed: doubled when one laid out no more content,
       reset when one did. */
    size_t extension_elements;
    /* The stream's gates refused the last extension (the stylesheet could
       not follow, say); the next waits for more body, at these bytes. */
    bool refused;
    size_t refused_bytes;
} PreviewPolicy;

typedef struct {
    size_t parsed_bytes;
    /* Closed elements that can draw (not style, script, link, meta,
       template). */
    size_t closed_content_elements;
    int reader_y;
    int viewport_height;
    /* Depth of a preview that is not following anyone: the viewport and
       the configured lookahead. */
    int base_limit_y;
    /* The retained preview on screen, if any. */
    bool live;
    bool live_truncated;
    int live_content_end_y;
    size_t live_closed_elements;
} PreviewPolicyInput;

typedef struct {
    PreviewAction action;
    /* Layout depth for the attempt. */
    int limit_y;
    /* Fresh input since the last admitted check (reopens the check cap). */
    bool fresh;
    /* Why nothing is built (for validation logs); NULL when building. */
    const char *reason;
} PreviewDecision;

typedef struct {
    bool retained;
    int content_end_y;
    bool truncated;
    bool first_screen_filled;
} PreviewPaintResult;

void preview_policy_init(PreviewPolicy *policy);
/* Input-driven transitions: a reader who scrolled is being followed. */
void preview_policy_observe(PreviewPolicy *policy,
                            const PreviewPolicyInput *input);
PreviewDecision preview_policy_decide(const PreviewPolicy *policy,
                                      const PreviewPolicyInput *input);
/* The stream's cheap gates passed for this decision. */
void preview_policy_note_check(PreviewPolicy *policy,
                               const PreviewDecision *decision);
/* The stream's costly gates refused this decision: following the reader
   again waits for more body rather than repeating them on every feed. */
void preview_policy_refused(PreviewPolicy *policy,
                            const PreviewDecision *decision,
                            size_t parsed_bytes);
/* A structural checkpoint (head or body closed): checks may run again. */
void preview_policy_reopen_checks(PreviewPolicy *policy);
void preview_policy_attempt_started(PreviewPolicy *policy,
                                    const PreviewDecision *decision,
                                    size_t parsed_bytes);
/* previous_content_end_y: the live preview's, before this one (0 if none). */
void preview_policy_painted(PreviewPolicy *policy,
                            const PreviewDecision *decision,
                            const PreviewPaintResult *result,
                            int previous_content_end_y);
void preview_policy_failed(PreviewPolicy *policy);
void preview_policy_abandon(PreviewPolicy *policy);
/* Nothing has been painted or attempted, and more may still be. */
bool preview_policy_awaiting_first(const PreviewPolicy *policy);

#endif
