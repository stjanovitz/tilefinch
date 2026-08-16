/*
 * Host gate for the scripted-input harness.
 *
 * Two jobs, and it is worth being precise about the second one's limits:
 *
 *   1. The parser and stepper. Malformed scripts must be rejected rather than
 *      silently skipped, `press` must expand to one press edge per pair, and
 *      the readiness gate must hold the cursor rather than spend steps.
 *
 *   2. A replay of the checked-in scenario through psp_ui_update(), diffed
 *      against a golden trace. This proves the button sequence still reaches
 *      the menu rows and option rows it names -- a renumbered menu breaks it
 *      here, in a second, instead of in an emulator run.
 *
 * What it deliberately does NOT prove: that the action and settings receivers
 * did anything. Those live in the PSP-only TUs and need an engine. The device
 * golden under scripts/run-ppsspp-input-script.sh is the oracle for that, and
 * this trace is only the frontend half of the same run. The one piece of the
 * frontend the loop owns rather than psp_ui_update -- opening COLLECTIONS on
 * the three SHOW_* actions -- is mirrored below so the second half of the
 * scenario replays against a plausible surface instead of a stale page.
 */
#include "tilefinch/psp_input_script.h"
#include "tilefinch/psp_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return false; \
    } \
} while (0)

static size_t warning_count;
static char last_warning[128];

static void record_warning(
    void *context, const char *path, size_t line_number, const char *reason)
{
    (void) context;
    (void) path;
    warning_count++;
    snprintf(last_warning, sizeof(last_warning), "%zu:%s", line_number,
             reason == NULL ? "(null)" : reason);
}

static bool parse_rejects(const char *text)
{
    PspInputScript script;
    warning_count = 0;
    bool parsed = psp_input_script_parse(
        &script, text, "inline", record_warning, NULL);
    return !parsed && warning_count > 0 && !psp_input_script_armed(&script);
}

static bool test_parser(void)
{
    PspInputScript script;
    CHECK(psp_input_script_parse(
        &script, "# comment\n\n  wait 3\ntap select\n", "inline",
        record_warning, NULL));
    CHECK(script.step_count == 2);
    CHECK(script.steps[0].kind == PSP_INPUT_SCRIPT_STEP_WAIT
          && script.steps[0].ticks == 3);
    CHECK(script.steps[1].kind == PSP_INPUT_SCRIPT_STEP_PRESS
          && script.steps[1].ticks == 2
          && script.steps[1].buttons == PSP_UI_BUTTON_MENU);
    CHECK(psp_input_script_armed(&script));

    CHECK(psp_input_script_parse(
        &script, "tap cross+square\n", "inline", record_warning, NULL));
    CHECK(script.steps[0].buttons
          == (PSP_UI_BUTTON_CONFIRM | PSP_UI_BUTTON_RELOAD));

    /* A typo must fail the run, not quietly drop a press. */
    CHECK(parse_rejects("tap nosuchbutton\n"));
    CHECK(parse_rejects("wait\n"));
    CHECK(parse_rejects("wait 0\n"));
    CHECK(parse_rejects("wait 3 extra\n"));
    CHECK(parse_rejects("hold 2\n"));
    CHECK(parse_rejects("press cross\n"));
    CHECK(parse_rejects("stick 3\n"));
    CHECK(parse_rejects("stick 3 diagonal\n"));
    CHECK(parse_rejects("end now\n"));
    CHECK(parse_rejects("end-live\n"));
    CHECK(parse_rejects("jump 3\n"));
    CHECK(parse_rejects("# only comments\n"));
    CHECK(parse_rejects(""));
    return true;
}

static bool test_stepper(void)
{
    PspInputScript script;
    CHECK(psp_input_script_parse(
        &script, "press 2 down\nwait 1\nend\n", "inline",
        record_warning, NULL));
    PspUiInput input;
    uint32_t previous = 0;
    /* press 2 down: held, released, held, released. */
    const bool expected_held[4] = { true, false, true, false };
    for (size_t at = 0; at < 4u; at++) {
        CHECK(psp_input_script_advance(&script, &input, previous, true));
        CHECK((input.held == PSP_UI_BUTTON_DOWN) == expected_held[at]);
        /* Exactly one press edge per pair. */
        CHECK((input.pressed == PSP_UI_BUTTON_DOWN) == expected_held[at]);
        CHECK(input.analog_x == 128 && input.analog_y == 128);
        CHECK(input.elapsed_ms == 0);
        previous = input.held;
    }
    CHECK(psp_input_script_advance(&script, &input, previous, true));
    CHECK(input.held == 0);
    /* `end` stops the script and hands the exit back to the caller. */
    CHECK(!psp_input_script_advance(&script, &input, 0, true));
    CHECK(script.reached_end && script.finished && !script.stalled);
    CHECK(script.ticks == 5);

    CHECK(psp_input_script_parse(
        &script, "wait 10\nend\n", "inline", record_warning, NULL));
    psp_input_script_interrupt(&script);
    CHECK(script.finished && !script.reached_end && !script.stalled);
    CHECK(!psp_input_script_advance(&script, &input, 0, true));

    CHECK(psp_input_script_parse(
        &script, "stick 2 right\nstick 1 up\nend\n", "inline",
        record_warning, NULL));
    CHECK(psp_input_script_advance(&script, &input, 0, true)
          && input.analog_x == 255 && input.analog_y == 128
          && input.pressed == 0 && input.held == 0);
    CHECK(psp_input_script_advance(&script, &input, 0, true)
          && input.analog_x == 255 && input.analog_y == 128);
    CHECK(psp_input_script_advance(&script, &input, 0, true)
          && input.analog_x == 128 && input.analog_y == 0);
    CHECK(!psp_input_script_advance(&script, &input, 0, true));

    /* A not-ready frame emits neutral input and spends no step. */
    CHECK(psp_input_script_parse(
        &script, "tap cross\nend\n", "inline", record_warning, NULL));
    for (size_t at = 0; at < 50u; at++) {
        CHECK(psp_input_script_advance(&script, &input, 0, false));
        CHECK(input.held == 0 && input.pressed == 0);
    }
    CHECK(script.ticks == 0 && script.step == 0);
    CHECK(psp_input_script_advance(&script, &input, 0, true));
    CHECK(input.pressed == PSP_UI_BUTTON_CONFIRM);

    /* When that held edge synchronously starts work, only its neutral release
       may drain before the explicitly-live sequence. */
    CHECK(psp_input_script_parse(
        &script, "tap cross\nwait-live 1\ntap-live circle\nend\n",
        "inline", record_warning, NULL));
    previous = 0;
    CHECK(psp_input_script_advance(&script, &input, previous, true));
    CHECK(input.pressed == PSP_UI_BUTTON_CONFIRM);
    previous = input.held;
    CHECK(psp_input_script_advance(&script, &input, previous, false));
    CHECK(input.held == 0 && input.pressed == 0);
    previous = input.held;
    CHECK(psp_input_script_advance(&script, &input, previous, false));
    CHECK(input.held == 0 && input.pressed == 0);
    CHECK(psp_input_script_advance(&script, &input, previous, false));
    CHECK(input.pressed == PSP_UI_BUTTON_CANCEL);
    CHECK(script.busy_ticks == 3u && script.busy_press_edges == 1u);

    /* Live-suffixed commands are the explicit temporal exception: they spend
       frames and produce edges while the browser is busy, then an ordinary
       step waits at the next boundary. */
    CHECK(psp_input_script_parse(
        &script,
        "wait-live 2\ntap-live circle\nmark-live acknowledged\n"
        "tap cross\nend\n",
        "inline", record_warning, NULL));
    previous = 0;
    CHECK(psp_input_script_advance(&script, &input, previous, false));
    CHECK(input.held == 0 && script.ticks == 1u);
    CHECK(psp_input_script_advance(&script, &input, previous, false));
    CHECK(input.held == 0 && script.ticks == 2u);
    CHECK(psp_input_script_advance(&script, &input, previous, false));
    CHECK(input.pressed == PSP_UI_BUTTON_CANCEL);
    previous = input.held;
    CHECK(psp_input_script_advance(&script, &input, previous, false));
    previous = input.held;
    CHECK(psp_input_script_advance(&script, &input, previous, false));
    CHECK(psp_input_script_mark(&script) != NULL
          && strcmp(psp_input_script_mark(&script), "acknowledged") == 0);
    CHECK(script.busy_ticks == 5u && script.busy_press_edges == 1u);
    unsigned spent = script.ticks;
    CHECK(psp_input_script_advance(&script, &input, previous, false));
    CHECK(script.ticks == spent && input.pressed == 0);
    CHECK(psp_input_script_advance(&script, &input, previous, true));
    CHECK(input.pressed == PSP_UI_BUTTON_CONFIRM);

    /* Each adjacent tap owns a release frame, so identical taps cannot
       collapse into one edge at their shared boundary. */
    CHECK(psp_input_script_parse(
        &script, "tap cross\ntap cross\nend\n", "inline",
        record_warning, NULL));
    previous = 0;
    unsigned edges = 0;
    for (size_t at = 0; at < 4u; at++) {
        CHECK(psp_input_script_advance(&script, &input, previous, true));
        if (input.pressed == PSP_UI_BUTTON_CONFIRM) edges++;
        previous = input.held;
    }
    CHECK(edges == 2u);

    /* A browser that never becomes ready must end the run, not hang it. */
    CHECK(psp_input_script_parse(
        &script, "tap cross\nend\n", "inline", record_warning, NULL));
    script.stall_limit = 4;
    for (size_t at = 0; at < 3u; at++)
        CHECK(psp_input_script_advance(&script, &input, 0, false));
    CHECK(!psp_input_script_advance(&script, &input, 0, false));
    CHECK(script.stalled && !script.reached_end);

    /* Marks fire once, on the first frame of their step. */
    CHECK(psp_input_script_parse(
        &script, "mark here\nwait 2\n", "inline", record_warning, NULL));
    CHECK(psp_input_script_advance(&script, &input, 0, true));
    CHECK(psp_input_script_mark(&script) != NULL
          && strcmp(psp_input_script_mark(&script), "here") == 0);
    CHECK(psp_input_script_advance(&script, &input, 0, true));
    CHECK(psp_input_script_mark(&script) == NULL);
    return true;
}

static bool test_names(void)
{
    CHECK(strcmp(psp_input_script_action_name(PSP_UI_ACTION_EXIT),
                 "exit") == 0);
    CHECK(strcmp(psp_input_script_action_name(
                     PSP_UI_ACTION_EDIT_DEVELOPER_URL),
                 "edit-developer-url") == 0);
    CHECK(strcmp(psp_input_script_setting_name(
                     PSP_UI_SETTING_PAGE_FONT_PERCENT),
                 "page-font-percent") == 0);
    CHECK(strcmp(psp_input_script_setting_name(
                     PSP_UI_SETTING_NETWORK_PROFILE),
                 "network-profile") == 0);
    CHECK(strcmp(psp_input_script_screen_name(PSP_UI_SCREEN_OPTION_ITEMS),
                 "option-items") == 0);
    CHECK(strcmp(psp_input_script_button_name(PSP_UI_BUTTON_MENU),
                 "select") == 0);
    return true;
}

static bool test_live_media_scenario(const char *directory)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/youtube-autoplay-seek-live.txt",
             directory);
    PspInputScript script;
    warning_count = 0;
    CHECK(psp_input_script_load(
        &script, path, record_warning, NULL));
    CHECK(script.step_count == 16u);
    CHECK(script.steps[0].kind == PSP_INPUT_SCRIPT_STEP_WAIT
          && script.steps[0].advance_while_busy);
    CHECK(script.steps[2].buttons == PSP_UI_BUTTON_RIGHT
          && script.steps[4].buttons == PSP_UI_BUTTON_RIGHT
          && script.steps[6].buttons == PSP_UI_BUTTON_RIGHT);
    CHECK(strcmp(script.steps[1].mark, "autoplay") == 0
          && strcmp(script.steps[7].mark, "coalesced-seek") == 0
          && strcmp(script.steps[9].mark, "pre-commit") == 0
          && strcmp(script.steps[12].mark, "committed-seek") == 0);
    return true;
}

/* The loop-owned transitions the scenario depends on.  Mirroring all of them
   matters: after HOME, continuing to drive the stale PAGE surface used to let
   the host golden pass a sequence the device interpreted differently. */
static void apply_surface_transition(
    PspUiState *ui, PspUiTabsView *tabs, uint8_t *document_tabs,
    const PspUiIntent *intent)
{
    switch (intent->action) {
        case PSP_UI_ACTION_HOME:
            psp_ui_show_home(ui);
            break;
        case PSP_UI_ACTION_NEW_TAB:
            if (tabs->count < PSP_UI_TAB_LIMIT) {
                tabs->active_index = tabs->count++;
                tabs->can_create = tabs->count < PSP_UI_TAB_LIMIT;
                psp_ui_set_tabs(ui, tabs);
            }
            psp_ui_show_home(ui);
            break;
        case PSP_UI_ACTION_SWITCH_TAB:
            if (intent->tab_index < tabs->count) {
                tabs->active_index = (uint8_t) intent->tab_index;
                psp_ui_set_tabs(ui, tabs);
            }
            if ((*document_tabs & (uint8_t) (1u << tabs->active_index)) != 0)
                psp_ui_leave_native_surface(ui);
            else
                psp_ui_show_home(ui);
            break;
        case PSP_UI_ACTION_SHOW_SCREENSHOTS:
            *document_tabs |= (uint8_t) (1u << tabs->active_index);
            psp_ui_leave_native_surface(ui);
            break;
        case PSP_UI_ACTION_SHOW_BOOKMARKS:
            psp_ui_show_collections(ui, PSP_UI_COLLECTION_BOOKMARKS);
            break;
        case PSP_UI_ACTION_SHOW_HISTORY:
            psp_ui_show_collections(ui, PSP_UI_COLLECTION_HISTORY);
            break;
        case PSP_UI_ACTION_SHOW_OFFLINE:
            psp_ui_show_collections(ui, PSP_UI_COLLECTION_OFFLINE);
            break;
        default:
            break;
    }
}

static bool replay(const char *script_path, FILE *out)
{
    PspInputScript script;
    warning_count = 0;
    if (!psp_input_script_load(
            &script, script_path, record_warning, NULL)) {
        fprintf(stderr, "FAIL cannot load %s (%s)\n", script_path,
                warning_count == 0 ? "no warning" : last_warning);
        return false;
    }
    PspUiState ui;
    psp_ui_init(&ui);
    PspUiHomeView home = { .tile_count = 2, .continue_count = 0 };
    psp_ui_set_home(&ui, &home);
    /* The device runner leaves url= empty, so the same script starts on the
       ordinary native HOME rather than the retired engine-first test page. */
    psp_ui_show_home(&ui);
    PspUiTabsView tabs = { .count = 1, .active_index = 0, .can_create = true };
    uint8_t document_tabs = 0;
    psp_ui_set_tabs(&ui, &tabs);
    PspUiCollectionsView collections = {
        .section = PSP_UI_COLLECTION_BOOKMARKS,
        .count = 0,
        .empty_message = "empty"
    };
    psp_ui_set_collections(&ui, &collections);

    /* Leaf name only: the golden is checked in and must not carry the
       absolute path of whichever tree produced it. */
    const char *leaf = strrchr(script_path, '/');
    fprintf(out, "script %s steps=%u\n",
            leaf == NULL ? script_path : leaf + 1,
            (unsigned) script.step_count);
    PspUiInput input;
    uint32_t previous = 0;
    while (psp_input_script_advance(&script, &input, previous, true)) {
        previous = input.held;
        const char *mark = psp_input_script_mark(&script);
        if (mark != NULL)
            fprintf(out, "mark %s screen=%s\n", mark,
                    psp_input_script_screen_name(ui.screen));
        PspUiIntent intent = psp_ui_update(&ui, &input);
        apply_surface_transition(&ui, &tabs, &document_tabs, &intent);
        if (intent.action == PSP_UI_ACTION_NONE
            && intent.setting.id == PSP_UI_SETTING_NONE)
            continue;
        fprintf(out,
                "step=%u action=%s setting=%s screen=%s menu=%u option=%u "
                "group=%u tab=%u\n",
                (unsigned) script.step,
                psp_input_script_action_name(intent.action),
                psp_input_script_setting_name(intent.setting.id),
                psp_input_script_screen_name(ui.screen),
                (unsigned) ui.menu_selection, (unsigned) ui.options_selection,
                (unsigned) ui.options_group_selection,
                (unsigned) intent.tab_index);
    }
    fprintf(out, "outcome %s frames=%u steps=%u/%u\n",
            script.stalled ? "stalled"
                           : (script.reached_end ? "complete" : "interrupted"),
            (unsigned) script.ticks, (unsigned) script.step,
            (unsigned) script.step_count);
    return true;
}

static bool compare(const char *produced_path, const char *golden_path)
{
    FILE *produced = fopen(produced_path, "rb");
    FILE *golden = fopen(golden_path, "rb");
    if (produced == NULL || golden == NULL) {
        fprintf(stderr, "FAIL cannot open trace or golden\n");
        if (produced != NULL) (void) fclose(produced);
        if (golden != NULL) (void) fclose(golden);
        return false;
    }
    char left[512];
    char right[512];
    size_t line = 0;
    bool same = true;
    while (same) {
        char *a = fgets(left, sizeof(left), produced);
        char *b = fgets(right, sizeof(right), golden);
        line++;
        if (a == NULL && b == NULL) break;
        if (a == NULL || b == NULL || strcmp(left, right) != 0) {
            fprintf(stderr,
                    "FAIL trace differs at line %zu\n  got:  %s"
                    "  want: %s",
                    line, a == NULL ? "(eof)\n" : left,
                    b == NULL ? "(eof)\n" : right);
            same = false;
        }
    }
    (void) fclose(produced);
    (void) fclose(golden);
    return same;
}

int main(int argc, char **argv)
{
    const char *directory = TILEFINCH_INPUT_SCRIPT_DIR;
    bool update = argc > 1 && strcmp(argv[1], "--update") == 0;
    char script_path[512];
    char golden_path[512];
    char trace_path[512];
    snprintf(script_path, sizeof(script_path), "%s/menu-tour.txt", directory);
    snprintf(golden_path, sizeof(golden_path),
             "%s/menu-tour.host-trace.txt", directory);
    snprintf(trace_path, sizeof(trace_path), "menu-tour.host-trace.produced");

    if (!test_parser() || !test_stepper() || !test_names()
        || !test_live_media_scenario(directory)) return 1;

    if (update) {
        FILE *out = fopen(golden_path, "wb");
        if (out == NULL || !replay(script_path, out)) {
            if (out != NULL) (void) fclose(out);
            fprintf(stderr, "FAIL cannot write golden\n");
            return 1;
        }
        if (fclose(out) != 0) return 1;
        printf("updated %s\n", golden_path);
        return 0;
    }
    /*
     * Replayed twice, from a fresh script and a fresh UI each time. The
     * stepper keeps no state the caller cannot see, so two passes that differ
     * would mean the determinism the device golden rests on is not there.
     */
    for (int pass = 0; pass < 2; pass++) {
        FILE *out = fopen(trace_path, "wb");
        if (out == NULL) {
            fprintf(stderr, "FAIL cannot write trace\n");
            return 1;
        }
        bool ok = replay(script_path, out);
        if (fclose(out) != 0) ok = false;
        if (!ok || !compare(trace_path, golden_path)) {
            fprintf(stderr, "FAIL replay pass %d\n", pass + 1);
            return 1;
        }
    }
    (void) remove(trace_path);
    printf("tilefinch-input-script-tests: PASS\n");
    return 0;
}
