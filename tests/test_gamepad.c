#include "tilefinch/gamepad.h"

#include <limits.h>
#include <stdio.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

int main(void)
{
    TilefinchGamepadCapture capture;
    tilefinch_gamepad_capture_init(&capture);
    CHECK(!capture.active
          && !tilefinch_gamepad_capture_consumes_browser_input(&capture));

    CHECK(tilefinch_gamepad_capture_step(
              &capture, true, 7, true,
              TILEFINCH_GAMEPAD_CAPTURE_HOLD_MS - 1)
              == TILEFINCH_GAMEPAD_CAPTURE_NO_CHANGE
          && !capture.active
          && tilefinch_gamepad_capture_consumes_browser_input(&capture));
    CHECK(tilefinch_gamepad_capture_step(&capture, true, 7, true, 1)
              == TILEFINCH_GAMEPAD_CAPTURE_ENTERED
          && capture.active && capture.page_generation == 7);
    CHECK(tilefinch_gamepad_capture_step(&capture, true, 7, true, 1000)
              == TILEFINCH_GAMEPAD_CAPTURE_NO_CHANGE
          && capture.active);
    CHECK(tilefinch_gamepad_capture_step(&capture, true, 7, false, 16)
              == TILEFINCH_GAMEPAD_CAPTURE_NO_CHANGE);
    CHECK(tilefinch_gamepad_capture_step(
              &capture, true, 7, true,
              TILEFINCH_GAMEPAD_CAPTURE_HOLD_MS)
              == TILEFINCH_GAMEPAD_CAPTURE_EXITED
          && !capture.active
          && tilefinch_gamepad_capture_consumes_browser_input(&capture));
    CHECK(tilefinch_gamepad_capture_step(&capture, true, 7, false, 16)
              == TILEFINCH_GAMEPAD_CAPTURE_NO_CHANGE
          && !tilefinch_gamepad_capture_consumes_browser_input(&capture));

    CHECK(tilefinch_gamepad_capture_step(
              &capture, false, 8, true,
              TILEFINCH_GAMEPAD_CAPTURE_HOLD_MS)
              == TILEFINCH_GAMEPAD_CAPTURE_UNAVAILABLE
          && !capture.active);
    CHECK(tilefinch_gamepad_capture_step(&capture, false, 8, false, 16)
              == TILEFINCH_GAMEPAD_CAPTURE_NO_CHANGE);
    CHECK(tilefinch_gamepad_capture_step(
              &capture, true, 8, true,
              TILEFINCH_GAMEPAD_CAPTURE_HOLD_MS)
              == TILEFINCH_GAMEPAD_CAPTURE_ENTERED);
    CHECK(tilefinch_gamepad_capture_step(&capture, true, 9, false, 16)
              == TILEFINCH_GAMEPAD_CAPTURE_EXITED
          && !capture.active);

    CHECK(tilefinch_gamepad_axis_from_u8(128) == 0
          && tilefinch_gamepad_axis_from_u8(120) == 0
          && tilefinch_gamepad_axis_from_u8(136) == 0
          && tilefinch_gamepad_axis_from_u8(0) == -INT16_MAX
          && tilefinch_gamepad_axis_from_u8(255) == INT16_MAX
          && tilefinch_gamepad_axis_from_u8(64) < 0
          && tilefinch_gamepad_axis_from_u8(192) > 0);

    const uint32_t face_and_shoulder = (UINT32_C(1) << 0u)
        | (UINT32_C(1) << 2u)
        | (UINT32_C(1) << TILEFINCH_GAMEPAD_BUTTON_LEFT_SHOULDER);
    CHECK(tilefinch_gamepad_apply_face_mapping(
              face_and_shoulder, TILEFINCH_GAMEPAD_FACE_X_PRIMARY)
              == face_and_shoulder);
    CHECK(tilefinch_gamepad_apply_face_mapping(
              face_and_shoulder, TILEFINCH_GAMEPAD_FACE_O_PRIMARY)
              == ((UINT32_C(1) << 1u) | (UINT32_C(1) << 2u)
                  | (UINT32_C(1)
                     << TILEFINCH_GAMEPAD_BUTTON_LEFT_SHOULDER)));
    CHECK(tilefinch_gamepad_apply_face_mapping(
              UINT32_C(1) << TILEFINCH_GAMEPAD_BUTTON_DPAD_RIGHT,
              TILEFINCH_GAMEPAD_FACE_O_PRIMARY)
              == (UINT32_C(1) << TILEFINCH_GAMEPAD_BUTTON_DPAD_RIGHT));

    TilefinchGamepadState state = {0};
    CHECK(tilefinch_gamepad_state_update(
              &state, true, UINT32_C(1) << 3, 100, -200, 42)
          && state.connected && state.timestamp_ms == 42
          && state.buttons == (UINT32_C(1) << 3)
          && state.axes[0] == 100 && state.axes[1] == -200);
    CHECK(!tilefinch_gamepad_state_update(
              &state, true, UINT32_C(1) << 3, 100, -200, 99)
          && state.timestamp_ms == 42);
    CHECK(tilefinch_gamepad_state_update(
              &state, false, UINT32_MAX, 100, 100, 100)
          && !state.connected && state.buttons == 0
          && state.axes[0] == 0 && state.axes[1] == 0
          && state.timestamp_ms == 100);

    puts("tilefinch-gamepad-tests: all checks passed");
    return 0;
}
