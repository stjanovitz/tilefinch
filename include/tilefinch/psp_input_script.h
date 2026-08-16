#ifndef TILEFINCH_PSP_INPUT_SCRIPT_H
#define TILEFINCH_PSP_INPUT_SCRIPT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/psp_ui.h"

/*
 * A scripted controller source for the interactive loop.
 *
 * The browser's action dispatch and settings receivers can only be entered by
 * a button press, so the PPSSPP smoke exercises the dispatch seam without ever
 * reaching a case body. This replaces the sampled pad with a reviewable text
 * script so the same run happens the same way on PPSSPP and on a PSP-3000.
 *
 * Two properties make it an oracle rather than a demo:
 *
 *   - It produces PspUiInput directly, in PSP_UI_BUTTON_* space, so the same
 *     object drives psp_ui_update() on the host. A host test replays a script
 *     and diffs the intent trace with no emulator in the loop.
 *   - It advances on browser readiness rather than on wall-clock ticks. A
 *     `wait 8` means eight idle frames, not eight frames of whatever the
 *     emulator managed while a navigation was still in flight. Emulator and
 *     device therefore walk the identical step sequence.
 *
 * Nothing here knows about the PSP SDK; it is compiled for the host tests and
 * for the validation-logging EBOOT only.
 */

#define PSP_INPUT_SCRIPT_STEP_LIMIT 256u
#define PSP_INPUT_SCRIPT_MARK_CAPACITY 20u

/* Frames the browser may stay busy before a stalled script gives up. At the
   nominal 16 ms cadence this is about 30 s, far longer than any hermetic
   navigation and far shorter than a hung emulator session. */
#define PSP_INPUT_SCRIPT_STALL_LIMIT 1800u

typedef enum {
    /* Idle frames: no buttons held. */
    PSP_INPUT_SCRIPT_STEP_WAIT = 0,
    /* Frames with `buttons` held; the first of them is the press edge. */
    PSP_INPUT_SCRIPT_STEP_HOLD,
    /* Alternating held/released frames: one press edge per pair. Menu
       navigation is mostly runs of the same press, and spelling each one as
       its own step would spend the step budget on `down`. */
    PSP_INPUT_SCRIPT_STEP_PRESS,
    /* One idle frame that also names a point in the log. */
    PSP_INPUT_SCRIPT_STEP_MARK,
    /* Frames with the analog stick held in a named cardinal direction. */
    PSP_INPUT_SCRIPT_STEP_ANALOG,
    /* Stop driving and leave through the ordinary report path. */
    PSP_INPUT_SCRIPT_STEP_END
} PspInputScriptStepKind;

typedef struct {
    uint8_t kind;
    bool advance_while_busy;
    uint16_t buttons;
    uint16_t ticks;
    uint8_t analog_x;
    uint8_t analog_y;
    char mark[PSP_INPUT_SCRIPT_MARK_CAPACITY];
} PspInputScriptStep;

typedef struct {
    PspInputScriptStep steps[PSP_INPUT_SCRIPT_STEP_LIMIT];
    uint16_t step_count;
    uint16_t step;
    uint16_t step_remaining;
    uint32_t ticks;
    uint32_t held_ticks;
    uint32_t busy_ticks;
    uint32_t busy_press_edges;
    uint32_t stall_limit;
    uint32_t stalled_ticks;
    bool armed;
    bool finished;
    bool stalled;
    bool reached_end;
    /* Borrowed from the current step for exactly the frame it fires on. */
    const char *mark;
} PspInputScript;

typedef void (*PspInputScriptWarning)(
    void *context, const char *path, size_t line_number, const char *reason);

void psp_input_script_reset(PspInputScript *script);

/*
 * Parses `text` (the whole script file) into `script`. Returns false and
 * leaves the script disarmed if any line is unusable, so a typo fails the run
 * instead of quietly skipping a step.
 */
bool psp_input_script_parse(
    PspInputScript *script, const char *text, const char *path,
    PspInputScriptWarning warning, void *warning_context);

bool psp_input_script_load(
    PspInputScript *script, const char *path,
    PspInputScriptWarning warning, void *warning_context);

bool psp_input_script_armed(const PspInputScript *script);

/* End a run without marking its scripted `end` step as reached. */
void psp_input_script_interrupt(PspInputScript *script);

/*
 * Overwrites `input` with this frame's scripted state and returns true while
 * the script is still driving. `ready` false holds the cursor where it is and
 * emits neutral input, which is how the script waits out a navigation without
 * spending ordinary steps on it. Explicit `-live` commands instead spend
 * their frames in that state so cancellation and feedback latency can be
 * tested. The neutral release half of the tap that entered busy work may
 * drain first; no ordinary held edge or later ordinary step can. Returns
 * false once the script ends, reaches `end`, or stalls; the caller then
 * leaves through the ordinary report path.
 */
bool psp_input_script_advance(
    PspInputScript *script, PspUiInput *input, uint32_t previous_buttons,
    bool ready);

/* Non-NULL only on the frame a `mark` step fires. */
const char *psp_input_script_mark(const PspInputScript *script);

/* Stable short names for the log and the host trace. Kept here rather than in
   psp_ui.c so the shipping EBOOT never links the string tables. */
const char *psp_input_script_action_name(PspUiAction action);
const char *psp_input_script_setting_name(PspUiSettingId setting);
const char *psp_input_script_screen_name(PspUiScreen screen);
const char *psp_input_script_button_name(PspUiButton button);

#endif
