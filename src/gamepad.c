#include "tilefinch/gamepad.h"

#include <limits.h>
#include <string.h>

_Static_assert(TILEFINCH_GAMEPAD_BUTTON_COUNT <= 32u,
               "gamepad buttons must fit the fixed button bitset");
_Static_assert(TILEFINCH_GAMEPAD_AXIS_COUNT == 4u,
               "the standard Gamepad mapping exposes four axes");
_Static_assert(TILEFINCH_GAMEPAD_FACE_MAPPING_COUNT <= 2,
               "gamepad face mapping must fit the profile/UI bit");

void tilefinch_gamepad_capture_init(TilefinchGamepadCapture *capture)
{
    if (capture != NULL) memset(capture, 0, sizeof(*capture));
}

TilefinchGamepadCaptureEvent tilefinch_gamepad_capture_request(
    TilefinchGamepadCapture *capture, bool page_available,
    uint64_t page_generation)
{
    if (capture == NULL) return TILEFINCH_GAMEPAD_CAPTURE_NO_CHANGE;
    if (!page_available) return TILEFINCH_GAMEPAD_CAPTURE_UNAVAILABLE;
    if (capture->active && capture->page_generation == page_generation)
        return TILEFINCH_GAMEPAD_CAPTURE_NO_CHANGE;
    capture->active = true;
    capture->page_generation = page_generation;
    capture->chord_hold_ms = 0;
    capture->chord_held = false;
    capture->chord_latched = false;
    capture->suppress_input_until_release = true;
    return TILEFINCH_GAMEPAD_CAPTURE_ENTERED;
}

TilefinchGamepadCaptureEvent tilefinch_gamepad_capture_step(
    TilefinchGamepadCapture *capture, bool page_available,
    uint64_t page_generation, bool chord_held, unsigned elapsed_ms)
{
    if (capture == NULL) return TILEFINCH_GAMEPAD_CAPTURE_NO_CHANGE;

    if (capture->active
        && (!page_available
            || capture->page_generation != page_generation)) {
        capture->active = false;
        capture->chord_latched = chord_held;
        capture->chord_hold_ms = 0;
        capture->chord_held = chord_held;
        capture->suppress_input_until_release = false;
        return TILEFINCH_GAMEPAD_CAPTURE_EXITED;
    }

    if (!chord_held) {
        capture->chord_held = false;
        capture->chord_latched = false;
        capture->chord_hold_ms = 0;
        return TILEFINCH_GAMEPAD_CAPTURE_NO_CHANGE;
    }

    if (!capture->chord_held) {
        capture->chord_held = true;
        capture->chord_hold_ms = 0;
    }
    if (capture->chord_latched)
        return TILEFINCH_GAMEPAD_CAPTURE_NO_CHANGE;

    unsigned remaining = TILEFINCH_GAMEPAD_CAPTURE_HOLD_MS
        - capture->chord_hold_ms;
    if (elapsed_ms < remaining) {
        capture->chord_hold_ms += elapsed_ms;
        return TILEFINCH_GAMEPAD_CAPTURE_NO_CHANGE;
    }

    capture->chord_hold_ms = TILEFINCH_GAMEPAD_CAPTURE_HOLD_MS;
    capture->chord_latched = true;
    if (capture->active) {
        capture->active = false;
        capture->suppress_input_until_release = false;
        return TILEFINCH_GAMEPAD_CAPTURE_EXITED;
    }
    if (!page_available)
        return TILEFINCH_GAMEPAD_CAPTURE_UNAVAILABLE;
    capture->active = true;
    capture->page_generation = page_generation;
    capture->suppress_input_until_release = false;
    return TILEFINCH_GAMEPAD_CAPTURE_ENTERED;
}

bool tilefinch_gamepad_capture_consumes_browser_input(
    const TilefinchGamepadCapture *capture)
{
    return capture != NULL && (capture->active || capture->chord_held);
}

int16_t tilefinch_gamepad_axis_from_u8(uint8_t value)
{
    enum { DEAD_ZONE = 12 };
    int centered = (int) value - 128;
    int magnitude = centered < 0 ? -centered : centered;
    if (magnitude <= DEAD_ZONE) return 0;
    int maximum = centered < 0 ? 128 : 127;
    int scaled = (magnitude - DEAD_ZONE) * INT16_MAX
        / (maximum - DEAD_ZONE);
    if (scaled > INT16_MAX) scaled = INT16_MAX;
    return (int16_t) (centered < 0 ? -scaled : scaled);
}

uint32_t tilefinch_gamepad_apply_face_mapping(
    uint32_t standard_buttons, TilefinchGamepadFaceMapping mapping)
{
    if (mapping != TILEFINCH_GAMEPAD_FACE_O_PRIMARY)
        return standard_buttons;
    const uint32_t primary_mask = (UINT32_C(1) << 0u)
        | (UINT32_C(1) << 1u);
    uint32_t primary = standard_buttons & primary_mask;
    uint32_t remapped = ((primary & (UINT32_C(1) << 0u)) << 1u)
        | ((primary & (UINT32_C(1) << 1u)) >> 1u);
    return (standard_buttons & ~primary_mask) | remapped;
}

bool tilefinch_gamepad_state_update(
    TilefinchGamepadState *state, bool connected, uint32_t buttons,
    int16_t axis_x, int16_t axis_y, uint64_t timestamp_ms)
{
    if (state == NULL) return false;
    if (!connected) {
        buttons = 0;
        axis_x = 0;
        axis_y = 0;
    }
    bool changed = state->connected != connected
        || state->buttons != buttons
        || state->axes[0] != axis_x || state->axes[1] != axis_y;
    if (!changed) return false;
    state->connected = connected;
    state->buttons = buttons;
    state->axes[0] = axis_x;
    state->axes[1] = axis_y;
    state->axes[2] = 0;
    state->axes[3] = 0;
    state->timestamp_ms = timestamp_ms;
    return true;
}
