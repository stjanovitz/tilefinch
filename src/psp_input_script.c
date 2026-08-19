/*
 * Scripted controller source: parser, stepper, and the name tables the log
 * and the host trace share. See include/tilefinch/psp_input_script.h for the
 * design contract and docs/engineering/INPUT_SCRIPT_HARNESS.md for the format.
 *
 * Deliberately free of PSP SDK dependencies: the host replay test and the
 * validation EBOOT run this exact object, so a script that passes on the host
 * walks the same steps on the device.
 */
#include "tilefinch/psp_input_script.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The name tables below use switches without a default so that adding a
   PspUiAction, PspUiSettingId, or PspUiScreen is a -Wswitch warning here
   rather than an "unknown" in a golden nobody reads twice. */

const char *psp_input_script_action_name(PspUiAction action)
{
    switch (action) {
        case PSP_UI_ACTION_NONE: return "none";
        case PSP_UI_ACTION_FOCUS_PREVIOUS: return "focus-previous";
        case PSP_UI_ACTION_FOCUS_NEXT: return "focus-next";
        case PSP_UI_ACTION_FOCUS_UP: return "focus-up";
        case PSP_UI_ACTION_FOCUS_DOWN: return "focus-down";
        case PSP_UI_ACTION_FOCUS_LEFT: return "focus-left";
        case PSP_UI_ACTION_FOCUS_RIGHT: return "focus-right";
        case PSP_UI_ACTION_FOCUS_AT: return "focus-at";
        case PSP_UI_ACTION_ACTIVATE: return "activate";
        case PSP_UI_ACTION_SUBMIT_FOCUSED_TEXT:
            return "submit-focused-text";
        case PSP_UI_ACTION_BACK: return "back";
        case PSP_UI_ACTION_FORWARD: return "forward";
        case PSP_UI_ACTION_RELOAD: return "reload";
        case PSP_UI_ACTION_TOGGLE_READER: return "toggle-reader";
        case PSP_UI_ACTION_TOGGLE_READER_SITE: return "toggle-reader-site";
        case PSP_UI_ACTION_PAGE_UP: return "page-up";
        case PSP_UI_ACTION_PAGE_DOWN: return "page-down";
        case PSP_UI_ACTION_SCROLL_TOP: return "scroll-top";
        case PSP_UI_ACTION_SCROLL_BOTTOM: return "scroll-bottom";
        case PSP_UI_ACTION_OPEN_ADDRESS: return "open-address";
        case PSP_UI_ACTION_OPEN_VOICE_ADDRESS: return "open-voice-address";
        case PSP_UI_ACTION_OPEN_FIND: return "open-find";
        case PSP_UI_ACTION_FIND_PREVIOUS: return "find-previous";
        case PSP_UI_ACTION_FIND_NEXT: return "find-next";
        case PSP_UI_ACTION_FIND_EDIT: return "find-edit";
        case PSP_UI_ACTION_FIND_CLOSE: return "find-close";
        case PSP_UI_ACTION_VOICE_FOCUSED_TEXT: return "voice-focused-text";
        case PSP_UI_ACTION_HOME: return "home";
        case PSP_UI_ACTION_SAVE_FOR_LATER: return "save-for-later";
        case PSP_UI_ACTION_SHOW_OFFLINE: return "show-offline";
        case PSP_UI_ACTION_SHOW_SCREENSHOTS: return "show-screenshots";
        case PSP_UI_ACTION_TOGGLE_BOOKMARK: return "toggle-bookmark";
        case PSP_UI_ACTION_SWITCH_TAB: return "switch-tab";
        case PSP_UI_ACTION_NEW_TAB: return "new-tab";
        case PSP_UI_ACTION_CLOSE_TAB: return "close-tab";
        case PSP_UI_ACTION_SHOW_BOOKMARKS: return "show-bookmarks";
        case PSP_UI_ACTION_SHOW_HOMEPAGE: return "show-homepage";
        case PSP_UI_ACTION_SHOW_HISTORY: return "show-history";
        case PSP_UI_ACTION_SCREENSHOT: return "screenshot";
        case PSP_UI_ACTION_BUILD_DIAGNOSTIC_QR:
            return "build-diagnostic-qr";
        case PSP_UI_ACTION_DIAGNOSTIC_QR_PREVIOUS:
            return "diagnostic-qr-previous";
        case PSP_UI_ACTION_DIAGNOSTIC_QR_NEXT:
            return "diagnostic-qr-next";
        case PSP_UI_ACTION_DIAGNOSTIC_QR_PART_PREVIOUS:
            return "diagnostic-qr-part-previous";
        case PSP_UI_ACTION_DIAGNOSTIC_QR_PART_NEXT:
            return "diagnostic-qr-part-next";
        case PSP_UI_ACTION_CLOSE_DIAGNOSTIC_QR:
            return "close-diagnostic-qr";
        case PSP_UI_ACTION_POWER_TEST: return "power-test";
        case PSP_UI_ACTION_MEDIA_TEST: return "media-test";
        case PSP_UI_ACTION_EDIT_DEVELOPER_URL: return "edit-developer-url";
        case PSP_UI_ACTION_SET_VIDEO_DECODER: return "set-video-decoder";
        case PSP_UI_ACTION_SHOW_HOME: return "show-home";
        case PSP_UI_ACTION_HOME_ACTIVATE: return "home-activate";
        case PSP_UI_ACTION_COLLECTION_ACTIVATE: return "collection-activate";
        case PSP_UI_ACTION_COLLECTION_DELETE: return "collection-delete";
        case PSP_UI_ACTION_EXIT: return "exit";
    }
    return "unknown";
}

const char *psp_input_script_setting_name(PspUiSettingId setting)
{
    switch (setting) {
        case PSP_UI_SETTING_NONE: return "none";
        case PSP_UI_SETTING_BROWSER_UI_SCALE: return "browser-ui-scale";
        case PSP_UI_SETTING_PAGE_FONT_PERCENT: return "page-font-percent";
        case PSP_UI_SETTING_READER_FONT: return "reader-font";
        case PSP_UI_SETTING_REMEMBER_READER_SITE_SCALE:
            return "remember-reader-site-scale";
        case PSP_UI_SETTING_CUSTOM_HOMEPAGE: return "custom-homepage";
        case PSP_UI_SETTING_HISTORY: return "history";
        case PSP_UI_SETTING_RESTORE_LAST_PAGE: return "restore-last-page";
        case PSP_UI_SETTING_TAB_HIBERNATION: return "tab-hibernation";
        case PSP_UI_SETTING_EXPERIMENTAL_VOICE: return "experimental-voice";
        case PSP_UI_SETTING_ADAPTIVE_VOICE_MEMORY:
            return "adaptive-voice-memory";
        case PSP_UI_SETTING_ANALOG_CURSOR: return "analog-cursor";
        case PSP_UI_SETTING_TEXT_ENTRY_MODE: return "text-entry-mode";
        case PSP_UI_SETTING_PERSISTENT_CACHE_MB: return "persistent-cache-mb";
        case PSP_UI_SETTING_LIVE_CACHE_KIB: return "live-cache-kib";
        case PSP_UI_SETTING_PERSIST_LOCAL_STORAGE:
            return "persist-local-storage";
        case PSP_UI_SETTING_SEARCH_ENGINE: return "search-engine";
        case PSP_UI_SETTING_COLOR_MODE: return "color-mode";
        case PSP_UI_SETTING_CHROME_THEME: return "chrome-theme";
        case PSP_UI_SETTING_GLYPH_LANGUAGE: return "glyph-language";
        case PSP_UI_SETTING_COLOR_EMOJI: return "color-emoji";
        case PSP_UI_SETTING_YOUTUBE_QUALITY: return "youtube-quality";
        case PSP_UI_SETTING_YOUTUBE_COMPACT_RESULTS:
            return "youtube-compact-results";
        case PSP_UI_SETTING_VIDEO_SCALING: return "video-scaling";
        case PSP_UI_SETTING_VIDEO_STARTUP_BUFFERING:
            return "video-startup-buffering";
        case PSP_UI_SETTING_RESUME_OFFLINE_DOWNLOADS:
            return "resume-offline-downloads";
        case PSP_UI_SETTING_CONTENT_BLOCKER_MODE:
            return "content-blocker-mode";
        case PSP_UI_SETTING_CONTENT_BLOCKER_SITE_ALLOWED:
            return "content-blocker-site-allowed";
        case PSP_UI_SETTING_CONTENT_BLOCKER_COSMETIC_HIDING:
            return "content-blocker-cosmetic-hiding";
        case PSP_UI_SETTING_COOKIE_BANNER_HIDDEN:
            return "cookie-banner-hidden";
        case PSP_UI_SETTING_UPDATE_CHECK: return "update-check";
        case PSP_UI_SETTING_WAVE_BACKGROUND: return "wave-background";
        case PSP_UI_SETTING_JAVASCRIPT: return "javascript";
        case PSP_UI_SETTING_SITE_JAVASCRIPT: return "site-javascript";
        case PSP_UI_SETTING_SITE_DATA_ALLOWED: return "site-data-allowed";
        case PSP_UI_SETTING_MIXED_CONTENT_SITE:
            return "mixed-content-site";
        case PSP_UI_SETTING_THIRD_PARTY_COOKIES_SITE:
            return "third-party-cookies-site";
        case PSP_UI_SETTING_TLS_SESSION_PERSISTENCE:
            return "tls-session-persistence";
        case PSP_UI_SETTING_NETWORK_PROFILE: return "network-profile";
        case PSP_UI_SETTING_UPDATE_CHANNEL: return "update-channel";
    }
    return "unknown";
}

const char *psp_input_script_screen_name(PspUiScreen screen)
{
    switch (screen) {
        case PSP_UI_SCREEN_PAGE: return "page";
        case PSP_UI_SCREEN_MENU: return "menu";
        case PSP_UI_SCREEN_OPTIONS: return "options";
        case PSP_UI_SCREEN_OPTION_ITEMS: return "option-items";
        case PSP_UI_SCREEN_EXPERIMENTAL_OPTIONS: return "experimental";
        case PSP_UI_SCREEN_GLYPH_OPTIONS: return "glyph-options";
        case PSP_UI_SCREEN_UPDATE: return "update";
        case PSP_UI_SCREEN_DATA_OPTIONS: return "data-options";
        case PSP_UI_SCREEN_TABS: return "tabs";
        case PSP_UI_SCREEN_TEXT_ENTRY: return "text-entry";
        case PSP_UI_SCREEN_FIND: return "find";
        case PSP_UI_SCREEN_HOME: return "home";
        case PSP_UI_SCREEN_COLLECTIONS: return "collections";
        case PSP_UI_SCREEN_DIAGNOSTIC_QR: return "diagnostic-qr";
    }
    return "unknown";
}

const char *psp_input_script_button_name(PspUiButton button)
{
    switch (button) {
        case PSP_UI_BUTTON_UP: return "up";
        case PSP_UI_BUTTON_DOWN: return "down";
        case PSP_UI_BUTTON_LEFT: return "left";
        case PSP_UI_BUTTON_RIGHT: return "right";
        case PSP_UI_BUTTON_CONFIRM: return "cross";
        case PSP_UI_BUTTON_CANCEL: return "circle";
        case PSP_UI_BUTTON_TOOLBAR: return "triangle";
        case PSP_UI_BUTTON_RELOAD: return "square";
        case PSP_UI_BUTTON_PAGE_UP: return "ltrigger";
        case PSP_UI_BUTTON_PAGE_DOWN: return "rtrigger";
        case PSP_UI_BUTTON_ADDRESS: return "start";
        case PSP_UI_BUTTON_MENU: return "select";
    }
    return "unknown";
}

/* Scripts name the physical control, not the abstract one, because that is
   what a reviewer holding a PSP can follow. The mapping is the same table
   psp_ui_buttons() applies to the sampled pad. */
static uint32_t script_button_bit(const char *name)
{
    static const PspUiButton buttons[] = {
        PSP_UI_BUTTON_UP, PSP_UI_BUTTON_DOWN, PSP_UI_BUTTON_LEFT,
        PSP_UI_BUTTON_RIGHT, PSP_UI_BUTTON_CONFIRM, PSP_UI_BUTTON_CANCEL,
        PSP_UI_BUTTON_TOOLBAR, PSP_UI_BUTTON_RELOAD, PSP_UI_BUTTON_PAGE_UP,
        PSP_UI_BUTTON_PAGE_DOWN, PSP_UI_BUTTON_ADDRESS, PSP_UI_BUTTON_MENU
    };
    for (size_t at = 0; at < sizeof(buttons) / sizeof(buttons[0]); at++) {
        if (strcmp(psp_input_script_button_name(buttons[at]), name) == 0)
            return (uint32_t) buttons[at];
    }
    return 0;
}

void psp_input_script_reset(PspInputScript *script)
{
    if (script == NULL) return;
    memset(script, 0, sizeof(*script));
    script->stall_limit = PSP_INPUT_SCRIPT_STALL_LIMIT;
}

static bool script_parse_buttons(const char *text, uint32_t *buttons)
{
    char token[24];
    size_t at = 0;
    *buttons = 0;
    while (*text != '\0') {
        if (*text == '+') {
            text++;
            continue;
        }
        at = 0;
        while (*text != '\0' && *text != '+') {
            if (at + 1u >= sizeof(token)) return false;
            token[at++] = *text++;
        }
        token[at] = '\0';
        uint32_t bit = script_button_bit(token);
        if (bit == 0) return false;
        *buttons |= bit;
    }
    return *buttons != 0;
}

static bool script_positive_count(const char *text, unsigned long *count)
{
    if (*text == '\0') return false;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (end == NULL || *end != '\0') return false;
    if (value == 0 || value > 0xFFFFul) return false;
    *count = value;
    return true;
}

static bool script_analog_direction(
    const char *name, uint8_t *x, uint8_t *y)
{
    if (name == NULL || x == NULL || y == NULL) return false;
    *x = 128u;
    *y = 128u;
    if (strcmp(name, "left") == 0) *x = 0u;
    else if (strcmp(name, "right") == 0) *x = 255u;
    else if (strcmp(name, "up") == 0) *y = 0u;
    else if (strcmp(name, "down") == 0) *y = 255u;
    else return false;
    return true;
}

static PspInputScriptStep *script_push(PspInputScript *script)
{
    if (script->step_count >= PSP_INPUT_SCRIPT_STEP_LIMIT) return NULL;
    PspInputScriptStep *step = &script->steps[script->step_count++];
    memset(step, 0, sizeof(*step));
    return step;
}

static bool script_parse_line(
    PspInputScript *script, char *line, const char **reason)
{
    /* command, then at most two operands; every form is fixed-arity. */
    char *cursor = line;
    char *fields[3] = {NULL, NULL, NULL};
    size_t field_count = 0;
    while (*cursor != '\0' && field_count < 3u) {
        while (*cursor == ' ' || *cursor == '\t') cursor++;
        if (*cursor == '\0') break;
        fields[field_count++] = cursor;
        while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t') cursor++;
        if (*cursor != '\0') *cursor++ = '\0';
    }
    while (*cursor == ' ' || *cursor == '\t') cursor++;
    if (*cursor != '\0') {
        *reason = "trailing text";
        return false;
    }
    if (field_count == 0) return true;

    char *command = fields[0];
    size_t command_length = strlen(command);
    bool advance_while_busy = command_length > 5u
        && strcmp(command + command_length - 5u, "-live") == 0;
    if (advance_while_busy) command[command_length - 5u] = '\0';
    unsigned long count = 0;
    uint32_t buttons = 0;
    PspInputScriptStep *step = NULL;

    if (strcmp(command, "wait") == 0) {
        if (field_count != 2u || !script_positive_count(fields[1], &count)) {
            *reason = "wait needs a positive frame count";
            return false;
        }
        step = script_push(script);
        if (step == NULL) { *reason = "too many steps"; return false; }
        step->kind = (uint8_t) PSP_INPUT_SCRIPT_STEP_WAIT;
        step->advance_while_busy = advance_while_busy;
        step->ticks = (uint16_t) count;
        return true;
    }
    if (strcmp(command, "tap") == 0 || strcmp(command, "hold") == 0) {
        bool is_hold = command[0] == 'h';
        if (is_hold) {
            if (field_count != 3u
                || !script_positive_count(fields[1], &count)
                || !script_parse_buttons(fields[2], &buttons)) {
                *reason = "hold needs a frame count and buttons";
                return false;
            }
        } else {
            if (field_count != 2u
                || !script_parse_buttons(fields[1], &buttons)) {
                *reason = "tap needs buttons";
                return false;
            }
            count = 1u;
        }
        step = script_push(script);
        if (step == NULL) { *reason = "too many steps"; return false; }
        step->kind = (uint8_t) (is_hold ? PSP_INPUT_SCRIPT_STEP_HOLD
                                       : PSP_INPUT_SCRIPT_STEP_PRESS);
        step->advance_while_busy = advance_while_busy;
        /* A tap includes its release frame.  Without it, adjacent taps of the
           same button collapse into one edge because previous_buttons stays
           held across the step boundary. */
        step->ticks = (uint16_t) (is_hold ? count : 2u);
        step->buttons = (uint16_t) buttons;
        return true;
    }
    if (strcmp(command, "press") == 0) {
        if (field_count != 3u
            || !script_positive_count(fields[1], &count)
            || count > 0x7FFFul
            || !script_parse_buttons(fields[2], &buttons)) {
            *reason = "press needs a press count and buttons";
            return false;
        }
        step = script_push(script);
        if (step == NULL) { *reason = "too many steps"; return false; }
        step->kind = (uint8_t) PSP_INPUT_SCRIPT_STEP_PRESS;
        step->advance_while_busy = advance_while_busy;
        /* Held frame then released frame, so every pair is one press edge. */
        step->ticks = (uint16_t) (count * 2ul);
        step->buttons = (uint16_t) buttons;
        return true;
    }
    if (strcmp(command, "stick") == 0) {
        uint8_t analog_x = 128u, analog_y = 128u;
        if (field_count != 3u
            || !script_positive_count(fields[1], &count)
            || !script_analog_direction(
                   fields[2], &analog_x, &analog_y)) {
            *reason = "stick needs a frame count and direction";
            return false;
        }
        step = script_push(script);
        if (step == NULL) { *reason = "too many steps"; return false; }
        step->kind = (uint8_t) PSP_INPUT_SCRIPT_STEP_ANALOG;
        step->advance_while_busy = advance_while_busy;
        step->ticks = (uint16_t) count;
        step->analog_x = analog_x;
        step->analog_y = analog_y;
        return true;
    }
    if (strcmp(command, "mark") == 0) {
        if (field_count != 2u
            || strlen(fields[1]) >= PSP_INPUT_SCRIPT_MARK_CAPACITY) {
            *reason = "mark needs a short name";
            return false;
        }
        step = script_push(script);
        if (step == NULL) { *reason = "too many steps"; return false; }
        step->kind = (uint8_t) PSP_INPUT_SCRIPT_STEP_MARK;
        step->advance_while_busy = advance_while_busy;
        step->ticks = 1u;
        snprintf(step->mark, sizeof(step->mark), "%s", fields[1]);
        return true;
    }
    if (strcmp(command, "end") == 0) {
        if (field_count != 1u || advance_while_busy) {
            *reason = "end takes no operands";
            return false;
        }
        step = script_push(script);
        if (step == NULL) { *reason = "too many steps"; return false; }
        step->kind = (uint8_t) PSP_INPUT_SCRIPT_STEP_END;
        step->ticks = 1u;
        return true;
    }
    *reason = "unknown command";
    return false;
}

bool psp_input_script_parse(
    PspInputScript *script, const char *text, const char *path,
    PspInputScriptWarning warning, void *warning_context)
{
    if (script == NULL || text == NULL) return false;
    psp_input_script_reset(script);
    char line[192];
    size_t line_number = 0;
    const char *cursor = text;
    bool ok = true;
    while (*cursor != '\0' && ok) {
        size_t length = 0;
        while (cursor[length] != '\0' && cursor[length] != '\n') length++;
        line_number++;
        size_t copy = length;
        while (copy > 0 && cursor[copy - 1u] == '\r') copy--;
        if (copy >= sizeof(line)) {
            if (warning != NULL)
                warning(warning_context, path, line_number, "line too long");
            ok = false;
            break;
        }
        memcpy(line, cursor, copy);
        line[copy] = '\0';
        cursor += length;
        if (*cursor == '\n') cursor++;
        char *content = line;
        while (*content == ' ' || *content == '\t') content++;
        if (*content == '\0' || *content == '#') continue;
        const char *reason = "malformed line";
        if (!script_parse_line(script, content, &reason)) {
            if (warning != NULL)
                warning(warning_context, path, line_number, reason);
            ok = false;
        }
    }
    if (!ok || script->step_count == 0) {
        if (ok && warning != NULL)
            warning(warning_context, path, line_number, "no steps");
        psp_input_script_reset(script);
        return false;
    }
    script->step = 0;
    script->step_remaining = script->steps[0].ticks;
    script->armed = true;
    return true;
}

bool psp_input_script_load(
    PspInputScript *script, const char *path,
    PspInputScriptWarning warning, void *warning_context)
{
    if (script != NULL) psp_input_script_reset(script);
    if (script == NULL || path == NULL) return false;
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        if (warning != NULL) warning(warning_context, path, 0, "not found");
        return false;
    }
    /* Scripts are a few hundred bytes of text; refuse anything that is not,
       rather than sizing a buffer from a file the device just found. */
    char text[8192];
    size_t read = fread(text, 1u, sizeof(text) - 1u, file);
    bool overflowed = !feof(file) || ferror(file);
    (void) fclose(file);
    if (overflowed) {
        if (warning != NULL) warning(warning_context, path, 0, "too large");
        return false;
    }
    text[read] = '\0';
    return psp_input_script_parse(
        script, text, path, warning, warning_context);
}

bool psp_input_script_armed(const PspInputScript *script)
{
    return script != NULL && script->armed;
}

void psp_input_script_interrupt(PspInputScript *script)
{
    if (script == NULL || !script->armed) return;
    script->finished = true;
    script->mark = NULL;
}

const char *psp_input_script_mark(const PspInputScript *script)
{
    return script == NULL ? NULL : script->mark;
}

bool psp_input_script_advance(
    PspInputScript *script, PspUiInput *input, uint32_t previous_buttons,
    bool ready)
{
    if (input != NULL) {
        input->held = 0;
        input->pressed = 0;
        input->analog_x = 128;
        input->analog_y = 128;
        /* Zero selects psp_ui_update()'s nominal 16 ms cadence, so overlay
           animation, focus repeat, and toast expiry advance by frame count
           rather than by however long the host took to get here. */
        input->elapsed_ms = 0;
    }
    if (script == NULL) return false;
    script->mark = NULL;
    if (!script->armed || script->finished) return false;
    while (script->step < script->step_count && script->step_remaining == 0u) {
        script->step++;
        if (script->step < script->step_count)
            script->step_remaining = script->steps[script->step].ticks;
    }
    if (script->step >= script->step_count) {
        script->reached_end = true;
        script->finished = true;
        return false;
    }
    const PspInputScriptStep *step = &script->steps[script->step];
    /* A tap can synchronously enter a long operation on its held half. Its
       following half is a neutral release, not a second user action. Drain
       only that release while busy so an explicit `-live` step after the tap
       can reach the cooperative supervisor. Never drain an ordinary held
       frame or another ordinary step. */
    bool pending_tap_release =
        !ready && !step->advance_while_busy
        && step->kind == (uint8_t) PSP_INPUT_SCRIPT_STEP_PRESS
        && (script->step_remaining % 2u) != 0u;
    if (!ready && !step->advance_while_busy && !pending_tap_release) {
        script->stalled_ticks++;
        if (script->stall_limit != 0
            && script->stalled_ticks >= script->stall_limit) {
            script->stalled = true;
            script->finished = true;
            return false;
        }
        return true;
    }
    script->stalled_ticks = 0;
    if (step->kind == (uint8_t) PSP_INPUT_SCRIPT_STEP_END) {
        script->reached_end = true;
        script->finished = true;
        return false;
    }
    if (step->kind == (uint8_t) PSP_INPUT_SCRIPT_STEP_MARK
        && script->step_remaining == step->ticks)
        script->mark = step->mark;
    bool holding = step->kind == (uint8_t) PSP_INPUT_SCRIPT_STEP_HOLD;
    if (step->kind == (uint8_t) PSP_INPUT_SCRIPT_STEP_PRESS) {
        /* Frames run down from an even total, so an even remainder is the
           held half and the following odd remainder releases it. */
        holding = (script->step_remaining % 2u) == 0u;
    }
    if (holding && input != NULL) {
        input->held = step->buttons;
        input->pressed = step->buttons & ~previous_buttons;
        script->held_ticks++;
        if (!ready && input->pressed != 0) script->busy_press_edges++;
    }
    if (step->kind == (uint8_t) PSP_INPUT_SCRIPT_STEP_ANALOG
        && input != NULL) {
        input->analog_x = step->analog_x;
        input->analog_y = step->analog_y;
    }
    if (!ready) script->busy_ticks++;
    script->step_remaining--;
    script->ticks++;
    return true;
}
