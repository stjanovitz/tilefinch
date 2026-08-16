#include "tilefinch/danzeff_input.h"

#include <string.h>

/*
 * Character ordering and controls follow the Danzeff OSK created by Danzel
 * and Jeff Chen. This is a clean, texture-free implementation for Tilefinch's
 * bounded native UI; the upstream BSD-3-Clause notice is distributed with
 * the application.
 */
static const char danzeff_layout[4][9][4] = {
    {
        {',', 'a', 'b', 'c'}, {'.', 'd', 'e', 'f'},
        {'!', 'g', 'h', 'i'}, {'-', 'j', 'k', 'l'},
        {'\b', 'm', ' ', 'n'}, {'?', 'o', 'p', 'q'},
        {'(', 'r', 's', 't'}, {':', 'u', 'v', 'w'},
        {')', 'x', 'y', 'z'}
    },
    {
        {'^', 'A', 'B', 'C'}, {'@', 'D', 'E', 'F'},
        {'*', 'G', 'H', 'I'}, {'_', 'J', 'K', 'L'},
        {'\b', 'M', ' ', 'N'}, {'"', 'O', 'P', 'Q'},
        {'=', 'R', 'S', 'T'}, {';', 'U', 'V', 'W'},
        {'/', 'X', 'Y', 'Z'}
    },
    {
        {0, 0, 0, '1'}, {0, 0, 0, '2'}, {0, 0, 0, '3'},
        {0, 0, 0, '4'}, {'\b', 0, ' ', '5'}, {0, 0, 0, '6'},
        {0, 0, 0, '7'}, {0, 0, 0, '8'}, {0, 0, '0', '9'}
    },
    {
        {',', '(', '.', ')'}, {'"', '<', '\'', '>'},
        {'-', '[', '_', ']'}, {'!', '{', '?', '}'},
        {'\b', 0, ' ', 0}, {'+', '\\', '=', '/'},
        {':', '@', ';', '#'}, {'~', '$', '`', '%'},
        {'*', '^', '|', '&'}
    }
};

unsigned danzeff_input_cell(uint8_t analog_x, uint8_t analog_y)
{
    unsigned x = analog_x < 85u ? 0u : (analog_x > 170u ? 2u : 1u);
    unsigned y = analog_y < 85u ? 0u : (analog_y > 170u ? 2u : 1u);
    return y * 3u + x;
}

char danzeff_input_character(
    DanzeffInputMode mode, bool shifted, unsigned cell,
    DanzeffInputFace face)
{
    if (mode > DANZEFF_INPUT_NUMBERS || cell >= 9u
        || face > DANZEFF_INPUT_CIRCLE) return 0;
    unsigned page = (unsigned) mode * 2u + (shifted ? 1u : 0u);
    return danzeff_layout[page][cell][face];
}

DanzeffInputFinish danzeff_input_finish(
    bool start_pressed, bool select_pressed,
    bool submit_allowed, bool shift_held)
{
    if (select_pressed) return DANZEFF_INPUT_CANCEL;
    if (!start_pressed) return DANZEFF_INPUT_CONTINUE;
    return submit_allowed && shift_held
        ? DANZEFF_INPUT_SUBMIT : DANZEFF_INPUT_DONE;
}

static size_t utf8_previous(const char *text, size_t cursor)
{
    if (text == NULL || cursor == 0) return 0;
    size_t previous = cursor - 1u;
    while (previous > 0
           && ((unsigned char) text[previous] & 0xc0u) == 0x80u)
        previous--;
    return previous;
}

static size_t utf8_next(const char *text, size_t cursor)
{
    if (text == NULL || text[cursor] == '\0') return cursor;
    size_t next = cursor + 1u;
    while (text[next] != '\0'
           && ((unsigned char) text[next] & 0xc0u) == 0x80u)
        next++;
    return next;
}

bool danzeff_input_insert(
    char *text, size_t capacity, size_t *cursor, char character,
    bool *replace_all)
{
    if (text == NULL || capacity == 0 || cursor == NULL
        || character == '\0' || character == '\b') return false;
    size_t length = strnlen(text, capacity);
    if (length >= capacity || *cursor > length) return false;
    if (replace_all != NULL && *replace_all) {
        text[0] = '\0';
        length = 0;
        *cursor = 0;
        *replace_all = false;
    }
    if (length + 1u >= capacity) return false;
    memmove(text + *cursor + 1u, text + *cursor, length - *cursor + 1u);
    text[(*cursor)++] = character;
    return true;
}

bool danzeff_input_backspace(
    char *text, size_t *cursor, bool *replace_all)
{
    if (text == NULL || cursor == NULL) return false;
    size_t length = strlen(text);
    if (*cursor > length) return false;
    if (replace_all != NULL && *replace_all) {
        bool changed = length != 0;
        text[0] = '\0';
        *cursor = 0;
        *replace_all = false;
        return changed;
    }
    if (*cursor == 0) return false;
    size_t previous = utf8_previous(text, *cursor);
    memmove(text + previous, text + *cursor, length - *cursor + 1u);
    *cursor = previous;
    return true;
}

bool danzeff_input_move_cursor(
    const char *text, size_t *cursor, int direction, bool *replace_all)
{
    if (text == NULL || cursor == NULL || direction == 0) return false;
    size_t length = strlen(text);
    if (*cursor > length) return false;
    size_t moved = direction < 0
        ? utf8_previous(text, *cursor) : utf8_next(text, *cursor);
    bool selection_changed = replace_all != NULL && *replace_all;
    if (replace_all != NULL) *replace_all = false;
    if (moved == *cursor) return selection_changed;
    *cursor = moved;
    return true;
}
