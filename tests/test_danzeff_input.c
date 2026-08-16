#include "tilefinch/danzeff_input.h"

#include <stdio.h>
#include <string.h>

#define CHECK(value) do { \
    if (!(value)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #value); \
        return 1; \
    } \
} while (0)

int main(void)
{
    CHECK(danzeff_input_cell(0, 0) == 0
          && danzeff_input_cell(84, 171) == 6
          && danzeff_input_cell(85, 85) == 4
          && danzeff_input_cell(170, 170) == 4
          && danzeff_input_cell(171, 171) == 8);
    CHECK(danzeff_input_character(
              DANZEFF_INPUT_LETTERS, false, 0,
              DANZEFF_INPUT_TRIANGLE) == ',');
    CHECK(danzeff_input_character(
              DANZEFF_INPUT_LETTERS, false, 0,
              DANZEFF_INPUT_CROSS) == 'b');
    CHECK(danzeff_input_character(
              DANZEFF_INPUT_LETTERS, true, 8,
              DANZEFF_INPUT_CIRCLE) == 'Z');
    CHECK(danzeff_input_character(
              DANZEFF_INPUT_NUMBERS, false, 4,
              DANZEFF_INPUT_TRIANGLE) == '\b');
    CHECK(danzeff_input_character(
              DANZEFF_INPUT_NUMBERS, false, 4,
              DANZEFF_INPUT_CIRCLE) == '5');
    CHECK(danzeff_input_character(
              DANZEFF_INPUT_NUMBERS, false, 8,
              DANZEFF_INPUT_CROSS) == '0');
    CHECK(danzeff_input_character(
              DANZEFF_INPUT_NUMBERS, true, 7,
              DANZEFF_INPUT_SQUARE) == '$');
    CHECK(danzeff_input_finish(false, false, true, true)
              == DANZEFF_INPUT_CONTINUE
          && danzeff_input_finish(true, false, true, false)
              == DANZEFF_INPUT_DONE
          && danzeff_input_finish(true, false, true, true)
              == DANZEFF_INPUT_SUBMIT
          && danzeff_input_finish(true, false, false, true)
              == DANZEFF_INPUT_DONE
          && danzeff_input_finish(true, true, true, true)
              == DANZEFF_INPUT_CANCEL);

    char text[16] = "https://old";
    size_t cursor = strlen(text);
    bool replace_all = true;
    CHECK(danzeff_input_insert(
              text, sizeof(text), &cursor, 'w', &replace_all)
          && strcmp(text, "w") == 0 && cursor == 1 && !replace_all);
    CHECK(danzeff_input_insert(
              text, sizeof(text), &cursor, 'i', &replace_all)
          && strcmp(text, "wi") == 0 && cursor == 2);
    CHECK(danzeff_input_move_cursor(
              text, &cursor, -1, &replace_all)
          && cursor == 1);
    CHECK(danzeff_input_insert(
              text, sizeof(text), &cursor, 'k', &replace_all)
          && strcmp(text, "wki") == 0 && cursor == 2);
    CHECK(danzeff_input_backspace(text, &cursor, &replace_all)
          && strcmp(text, "wi") == 0 && cursor == 1);

    snprintf(text, sizeof(text), "A\xc3\xa9Z");
    cursor = strlen(text);
    CHECK(danzeff_input_move_cursor(text, &cursor, -1, NULL)
          && cursor == 3);
    CHECK(danzeff_input_backspace(text, &cursor, NULL)
          && strcmp(text, "AZ") == 0 && cursor == 1);

    char full[4] = "abc";
    cursor = 3;
    CHECK(!danzeff_input_insert(
        full, sizeof(full), &cursor, 'd', NULL));
    cursor = strlen(full);
    replace_all = true;
    CHECK(danzeff_input_move_cursor(
              full, &cursor, 1, &replace_all)
          && cursor == strlen(full) && !replace_all);
    puts("danzeff-input-tests: ok");
    return 0;
}
