#ifndef TILEFINCH_GAMEPAD_H
#define TILEFINCH_GAMEPAD_H

#include <stdbool.h>
#include <stdint.h>

#define TILEFINCH_GAMEPAD_BUTTON_COUNT 17u
#define TILEFINCH_GAMEPAD_AXIS_COUNT 4u
#define TILEFINCH_GAMEPAD_CAPTURE_HOLD_MS 700u

/* W3C standard-gamepad button positions. Missing PSP controls remain
   present and unpressed so game code can use the ordinary mapping table. */
typedef enum {
    TILEFINCH_GAMEPAD_BUTTON_PRIMARY = 0,
    TILEFINCH_GAMEPAD_BUTTON_SECONDARY = 1,
    TILEFINCH_GAMEPAD_BUTTON_TERTIARY = 2,
    TILEFINCH_GAMEPAD_BUTTON_QUATERNARY = 3,
    TILEFINCH_GAMEPAD_BUTTON_LEFT_SHOULDER = 4,
    TILEFINCH_GAMEPAD_BUTTON_RIGHT_SHOULDER = 5,
    TILEFINCH_GAMEPAD_BUTTON_LEFT_TRIGGER = 6,
    TILEFINCH_GAMEPAD_BUTTON_RIGHT_TRIGGER = 7,
    TILEFINCH_GAMEPAD_BUTTON_SELECT = 8,
    TILEFINCH_GAMEPAD_BUTTON_START = 9,
    TILEFINCH_GAMEPAD_BUTTON_LEFT_STICK = 10,
    TILEFINCH_GAMEPAD_BUTTON_RIGHT_STICK = 11,
    TILEFINCH_GAMEPAD_BUTTON_DPAD_UP = 12,
    TILEFINCH_GAMEPAD_BUTTON_DPAD_DOWN = 13,
    TILEFINCH_GAMEPAD_BUTTON_DPAD_LEFT = 14,
    TILEFINCH_GAMEPAD_BUTTON_DPAD_RIGHT = 15,
    TILEFINCH_GAMEPAD_BUTTON_HOME = 16
} TilefinchGamepadButton;

/* Face-button preferences are serialized in BrowserProfile. Append values;
   never reorder them. The PSP's physical button names stay out of the web
   Gamepad object: this only chooses which face button occupies each standard
   mapping slot while page controls are active. */
typedef enum {
    TILEFINCH_GAMEPAD_FACE_X_PRIMARY = 0,
    TILEFINCH_GAMEPAD_FACE_O_PRIMARY,
    TILEFINCH_GAMEPAD_FACE_MAPPING_COUNT
} TilefinchGamepadFaceMapping;

typedef struct {
    uint32_t buttons;
    int16_t axes[TILEFINCH_GAMEPAD_AXIS_COUNT];
    uint64_t timestamp_ms;
    bool connected;
} TilefinchGamepadState;

typedef struct {
    uint64_t page_generation;
    unsigned chord_hold_ms;
    bool active;
    bool chord_held;
    bool chord_latched;
} TilefinchGamepadCapture;

typedef enum {
    TILEFINCH_GAMEPAD_CAPTURE_NO_CHANGE = 0,
    TILEFINCH_GAMEPAD_CAPTURE_ENTERED,
    TILEFINCH_GAMEPAD_CAPTURE_EXITED,
    TILEFINCH_GAMEPAD_CAPTURE_UNAVAILABLE
} TilefinchGamepadCaptureEvent;

void tilefinch_gamepad_capture_init(TilefinchGamepadCapture *capture);
TilefinchGamepadCaptureEvent tilefinch_gamepad_capture_step(
    TilefinchGamepadCapture *capture, bool page_available,
    uint64_t page_generation, bool chord_held, unsigned elapsed_ms);
bool tilefinch_gamepad_capture_consumes_browser_input(
    const TilefinchGamepadCapture *capture);

/* Normalizes the PSP's unsigned analog sample to the Gamepad API's -1..1
   domain represented as signed Q15. A small center dead zone prevents an
   untouched aging nub from making games drift. */
int16_t tilefinch_gamepad_axis_from_u8(uint8_t value);
uint32_t tilefinch_gamepad_apply_face_mapping(
    uint32_t standard_buttons, TilefinchGamepadFaceMapping mapping);
bool tilefinch_gamepad_state_update(
    TilefinchGamepadState *state, bool connected, uint32_t buttons,
    int16_t axis_x, int16_t axis_y, uint64_t timestamp_ms);

#endif
