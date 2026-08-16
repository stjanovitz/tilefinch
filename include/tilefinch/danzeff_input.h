#ifndef TILEFINCH_DANZEFF_INPUT_H
#define TILEFINCH_DANZEFF_INPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    DANZEFF_INPUT_LETTERS = 0,
    DANZEFF_INPUT_NUMBERS
} DanzeffInputMode;

typedef enum {
    DANZEFF_INPUT_TRIANGLE = 0,
    DANZEFF_INPUT_SQUARE,
    DANZEFF_INPUT_CROSS,
    DANZEFF_INPUT_CIRCLE
} DanzeffInputFace;

typedef enum {
    DANZEFF_INPUT_CONTINUE = 0,
    DANZEFF_INPUT_CANCEL,
    DANZEFF_INPUT_DONE,
    DANZEFF_INPUT_SUBMIT
} DanzeffInputFinish;

/* Returns a row-major 0..8 cell using the established Danzeff dead zones. */
unsigned danzeff_input_cell(uint8_t analog_x, uint8_t analog_y);

/* Returns an ASCII byte, '\b' for backspace, or zero for an empty position. */
char danzeff_input_character(
    DanzeffInputMode mode, bool shifted, unsigned cell,
    DanzeffInputFace face);

/* Host-testable interpretation of the two global keyboard commands. */
DanzeffInputFinish danzeff_input_finish(
    bool start_pressed, bool select_pressed,
    bool submit_allowed, bool shift_held);

/* Bounded UTF-8-aware editing for the ASCII characters produced above. */
bool danzeff_input_insert(
    char *text, size_t capacity, size_t *cursor, char character,
    bool *replace_all);
bool danzeff_input_backspace(
    char *text, size_t *cursor, bool *replace_all);
bool danzeff_input_move_cursor(
    const char *text, size_t *cursor, int direction, bool *replace_all);

#endif
