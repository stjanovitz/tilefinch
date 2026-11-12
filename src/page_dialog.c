#include "tilefinch/page_dialog.h"

#include <string.h>

#include "tilefinch/frame_sandbox.h"
#include "tilefinch/url.h"
#include "tilefinch/utf8_encode.h"

PageDialogVerdict page_dialog_policy_admit(
    PageDialogPolicy *policy, uint32_t gesture_generation, uint64_t now_ms)
{
    if (policy == NULL) return PAGE_DIALOG_DENIED_REALM;
    if (policy->suppressed) return PAGE_DIALOG_DENIED_SUPPRESSED;
    if (policy->gesture_generation != gesture_generation) {
        policy->gesture_generation = gesture_generation;
        policy->since_gesture = 0;
    }
    if (policy->since_gesture >= PAGE_DIALOG_GESTURE_LIMIT)
        return PAGE_DIALOG_DENIED_GESTURE;
    /* The ring holds the last RATE_LIMIT openings; the next slot is the
       oldest. It must have left the window. A clock that stepped backwards
       counts as inside the window, the refusing side. */
    if (policy->shown >= PAGE_DIALOG_RATE_LIMIT) {
        uint64_t oldest = policy->opened_ms[policy->opened_next];
        if (now_ms < oldest || now_ms - oldest < PAGE_DIALOG_RATE_WINDOW_MS)
            return PAGE_DIALOG_DENIED_RATE;
    }
    return PAGE_DIALOG_ADMITTED;
}

void page_dialog_policy_opened(PageDialogPolicy *policy, uint64_t now_ms)
{
    if (policy == NULL) return;
    if (policy->shown != UINT32_MAX) policy->shown++;
    if (policy->since_gesture != UINT32_MAX) policy->since_gesture++;
    policy->opened_ms[policy->opened_next] = now_ms;
    policy->opened_next =
        (uint8_t) ((policy->opened_next + 1u) % PAGE_DIALOG_RATE_LIMIT);
}

void page_dialog_policy_answered(PageDialogPolicy *policy,
                                 PageDialogOutcome outcome)
{
    if (policy == NULL) return;
    if (outcome == PAGE_DIALOG_OUTCOME_SUPPRESS
        || outcome == PAGE_DIALOG_OUTCOME_LEAVE) policy->suppressed = true;
}

void page_dialog_policy_note_denied(PageDialogPolicy *policy,
                                    PageDialogVerdict verdict)
{
    if (policy == NULL || verdict <= PAGE_DIALOG_ADMITTED
        || verdict >= PAGE_DIALOG_VERDICT_COUNT) return;
    if (policy->denied[verdict] != UINT32_MAX) policy->denied[verdict]++;
}

/* Strict UTF-8: one scalar value, or U+FFFD for one malformed byte. */
static size_t dialog_utf8_next(const unsigned char *text, size_t remaining,
                               unsigned *codepoint)
{
    unsigned first = text[0];
    if (first < 0x80u) {
        *codepoint = first;
        return 1;
    }
    size_t count;
    unsigned value, minimum;
    if ((first & 0xe0u) == 0xc0u) {
        count = 2; value = first & 0x1fu; minimum = 0x80u;
    } else if ((first & 0xf0u) == 0xe0u) {
        count = 3; value = first & 0x0fu; minimum = 0x800u;
    } else if ((first & 0xf8u) == 0xf0u) {
        count = 4; value = first & 0x07u; minimum = 0x10000u;
    } else {
        *codepoint = 0xfffdu;
        return 1;
    }
    if (remaining < count) {
        *codepoint = 0xfffdu;
        return 1;
    }
    for (size_t at = 1; at < count; at++) {
        if ((text[at] & 0xc0u) != 0x80u) {
            *codepoint = 0xfffdu;
            return 1;
        }
        value = (value << 6) | (text[at] & 0x3fu);
    }
    /* Overlong forms, surrogates (WTF-8 lone halves included) and values
       past U+10FFFF all display as one replacement character. */
    if (value < minimum || value > 0x10ffffu
        || (value >= 0xd800u && value <= 0xdfffu))
        value = 0xfffdu;
    *codepoint = value;
    return count;
}

/* Characters a page could use to reorder, hide or forge the dialog text. */
static bool dialog_dropped_character(unsigned codepoint)
{
    if (codepoint < 0x20u || (codepoint >= 0x7fu && codepoint <= 0x9fu))
        return true;
    return codepoint == 0x061cu                         /* ALM */
        || codepoint == 0x200eu || codepoint == 0x200fu  /* LRM, RLM */
        || (codepoint >= 0x202au && codepoint <= 0x202eu) /* embed/override */
        || (codepoint >= 0x2066u && codepoint <= 0x2069u) /* isolates */
        || (codepoint >= 0x2060u && codepoint <= 0x2064u) /* invisible ops */
        || codepoint == 0xfeffu                          /* BOM / ZWNBSP */
        || (codepoint >= 0xfff9u && codepoint <= 0xfffbu) /* annotations */
        || (codepoint >= 0xe0000u && codepoint <= 0xe007fu); /* tags */
}

static const unsigned char dialog_ellipsis[] = {0xe2, 0x80, 0xa6};

size_t page_dialog_sanitize_text(
    const char *input, size_t length, bool single_line,
    size_t maximum_lines, char *output, size_t capacity, bool *truncated)
{
    if (truncated != NULL) *truncated = false;
    if (output == NULL || capacity == 0) return 0;
    output[0] = '\0';
    /* Room for the ellipsis and the terminator is kept back from the
       start, so truncation never needs to back up over written text. */
    if (capacity <= sizeof(dialog_ellipsis) + 1u) return 0;
    if (input == NULL) length = 0;
    if (maximum_lines == 0) maximum_lines = 1;
    const size_t limit = capacity - sizeof(dialog_ellipsis) - 1u;
    const unsigned char *bytes = (const unsigned char *) input;
    size_t written = 0;
    size_t lines = 1;
    /* Breaks seen but not yet written: a run of breaks becomes at most one
       blank line, and breaks before the first or after the last visible
       character are dropped. */
    size_t pending_breaks = 0;
    bool pending_space = false;
    bool cut = false;
    size_t at = 0;
    while (at < length) {
        unsigned codepoint;
        size_t used = dialog_utf8_next(bytes + at, length - at, &codepoint);
        at += used;
        bool line_break = codepoint == '\n' || codepoint == '\r'
            || codepoint == 0x85u || codepoint == 0x2028u
            || codepoint == 0x2029u;
        if (codepoint == '\r' && at < length && bytes[at] == '\n') at++;
        if (line_break && single_line) {
            line_break = false;
            codepoint = ' ';
        }
        if (line_break) {
            if (written != 0) pending_breaks++;
            pending_space = false;
            continue;
        }
        if (codepoint == '\t') codepoint = ' ';
        if (dialog_dropped_character(codepoint)) continue;
        if (codepoint == ' ') {
            /* Collapsed with its neighbours, and never leading a line. */
            if (written != 0 && pending_breaks == 0) pending_space = true;
            continue;
        }
        unsigned char encoded[4];
        size_t encoded_length = tilefinch_utf8_encode(codepoint, encoded);
        if (encoded_length == 0) continue;
        if (pending_breaks != 0) {
            size_t breaks = pending_breaks > 2u ? 2u : pending_breaks;
            if (lines + breaks > maximum_lines) {
                cut = true;
                break;
            }
            if (breaks > limit - written) {
                cut = true;
                break;
            }
            for (size_t b = 0; b < breaks; b++) output[written++] = '\n';
            lines += breaks;
            pending_breaks = 0;
        }
        size_t needed = encoded_length + (pending_space ? 1u : 0u);
        if (needed > limit - written) {
            cut = true;
            break;
        }
        if (pending_space) output[written++] = ' ';
        pending_space = false;
        memcpy(output + written, encoded, encoded_length);
        written += encoded_length;
    }
    if (cut) {
        memcpy(output + written, dialog_ellipsis, sizeof(dialog_ellipsis));
        written += sizeof(dialog_ellipsis);
        if (truncated != NULL) *truncated = true;
    }
    output[written] = '\0';
    return written;
}

/* Whole characters of INPUT that fit, without display's collapsing. Only what a
   string cannot carry here changes: malformed UTF-8, surrogates and NUL. For the
   keyboard, EDITABLE also makes the text one plain line. */
static size_t dialog_copy_text(const char *input, size_t length, bool editable,
                               char *output, size_t capacity)
{
    if (output == NULL || capacity == 0) return 0;
    output[0] = '\0';
    if (input == NULL) return 0;
    const unsigned char *bytes = (const unsigned char *) input;
    size_t written = 0;
    size_t at = 0;
    while (at < length) {
        unsigned codepoint;
        at += dialog_utf8_next(bytes + at, length - at, &codepoint);
        if (editable) {
            if (codepoint == '\r' && at < length && bytes[at] == '\n') at++;
            if (codepoint == '\n' || codepoint == '\r' || codepoint == '\t'
                || codepoint == 0x85u || codepoint == 0x2028u
                || codepoint == 0x2029u)
                codepoint = ' ';
            if (dialog_dropped_character(codepoint)) continue;
        } else if (codepoint == 0) {
            codepoint = 0xfffdu;
        }
        unsigned char encoded[4];
        size_t encoded_length = tilefinch_utf8_encode(codepoint, encoded);
        if (encoded_length == 0) continue;
        if (encoded_length >= capacity - written) break;
        memcpy(output + written, encoded, encoded_length);
        written += encoded_length;
    }
    output[written] = '\0';
    return written;
}

size_t page_dialog_answer_text(const char *input, size_t length,
                               char *output, size_t capacity)
{
    return dialog_copy_text(input, length, false, output, capacity);
}

size_t page_dialog_editable_text(const char *input, size_t length,
                                 char *output, size_t capacity)
{
    return dialog_copy_text(input, length, true, output, capacity);
}

bool page_dialog_title_host(const char *url, char *output, size_t capacity)
{
    if (output == NULL || capacity == 0) return false;
    output[0] = '\0';
    TilefinchUrl parsed;
    if (url == NULL || !tilefinch_url_parse(url, &parsed)
        || parsed.host_length == 0) return false;
    const char *host = parsed.value + parsed.host_offset;
    size_t host_length = parsed.host_length;
    char port[8] = {0};
    size_t port_length = 0;
    if (parsed.explicit_port) {
        unsigned value = parsed.port;
        char digits[6];
        size_t count = 0;
        do {
            digits[count++] = (char) ('0' + value % 10u);
            value /= 10u;
        } while (value != 0 && count < sizeof(digits));
        port[port_length++] = ':';
        while (count != 0) port[port_length++] = digits[--count];
    }
    if (host_length > capacity - 1u
        || port_length > capacity - 1u - host_length) return false;
    for (size_t i = 0; i < host_length; i++) {
        unsigned char c = (unsigned char) host[i];
        /* A host is ASCII after URL parsing; anything else is not shown. */
        if (c <= 0x20u || c >= 0x7fu) {
            output[0] = '\0';
            return false;
        }
        output[i] = (char) (c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    }
    memcpy(output + host_length, port, port_length);
    output[host_length + port_length] = '\0';
    return true;
}

bool page_dialog_frame_allowed(bool same_origin_with_parent,
                               uint32_t sandbox_flags,
                               bool parent_allowed)
{
    return parent_allowed && same_origin_with_parent
        && tilefinch_frame_sandbox_allows(
               sandbox_flags, TILEFINCH_SANDBOX_ALLOW_MODALS);
}

void page_dialog_modal_open(PageDialogModal *modal, PageDialogKind kind,
                            bool offer_suppress, uint32_t held,
                            uint64_t now_ms)
{
    if (modal == NULL) return;
    memset(modal, 0, sizeof(*modal));
    modal->kind = kind;
    modal->offer_suppress = offer_suppress;
    modal->previous_buttons = held;
    modal->unreleased_buttons = held;
    modal->opened_ms = now_ms;
}

void page_dialog_modal_resume(PageDialogModal *modal, uint32_t held)
{
    if (modal == NULL) return;
    modal->previous_buttons = held;
    modal->unreleased_buttons |= held;
}

PageDialogModalStep page_dialog_modal_step(PageDialogModal *modal,
                                           uint32_t held, uint64_t now_ms)
{
    if (modal == NULL) return PAGE_DIALOG_MODAL_DISMISS;
    uint32_t pressed = held & ~modal->previous_buttons;
    modal->previous_buttons = held;
    modal->unreleased_buttons &= held;
    if (!modal->armed && modal->unreleased_buttons == 0
        && now_ms >= modal->opened_ms
        && now_ms - modal->opened_ms >= PAGE_DIALOG_ARM_MS)
        modal->armed = true;
    if (now_ms >= modal->opened_ms
        && now_ms - modal->opened_ms >= PAGE_DIALOG_MAXIMUM_OPEN_MS)
        return PAGE_DIALOG_MODAL_DISMISS;
    /* Refusals first: one sample with several edges never accepts. */
    if (pressed & PAGE_DIALOG_BUTTON_CIRCLE) return PAGE_DIALOG_MODAL_DISMISS;
    if (pressed & PAGE_DIALOG_BUTTON_TRIANGLE) return PAGE_DIALOG_MODAL_LEAVE;
    if ((pressed & PAGE_DIALOG_BUTTON_SQUARE) && modal->offer_suppress)
        return PAGE_DIALOG_MODAL_SUPPRESS;
    if (!modal->armed) return PAGE_DIALOG_MODAL_WAIT;
    if (modal->kind == PAGE_DIALOG_PROMPT) {
        if (pressed & PAGE_DIALOG_BUTTON_START) return PAGE_DIALOG_MODAL_ACCEPT;
        if (pressed & PAGE_DIALOG_BUTTON_CROSS) return PAGE_DIALOG_MODAL_EDIT;
        return PAGE_DIALOG_MODAL_WAIT;
    }
    if (pressed & PAGE_DIALOG_BUTTON_CROSS) return PAGE_DIALOG_MODAL_ACCEPT;
    return PAGE_DIALOG_MODAL_WAIT;
}
