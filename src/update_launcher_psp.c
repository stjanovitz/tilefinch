#include <pspctrl.h>
#include <pspkernel.h>
#include <psploadexec.h>

#include "psp_systemctrl_stubs.h"

/*
 * Chrome design tokens. src/psp_ui_theme.h is the source of truth for every
 * colour on this surface; it is macro-only (stdint plus #defines), so the
 * frozen launcher can share it directly instead of carrying its own copy of
 * the hex. The accent is user-selectable in the browser, but the launcher
 * runs before any profile has been read, so it always draws the default.
 */
#include "psp_ui_theme.h"
#include "psp_boot_mark.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "tilefinch/build_version.h"
#include "tilefinch/install_paths.h"
#include "tilefinch/psp_display.h"
#include "tilefinch/psp_threads.h"
#include "tilefinch/update.h"

PSP_MODULE_INFO("Tilefinch Launcher", 0, 0, 1);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(2048);

#define LAUNCHER_WIDTH PSP_DISPLAY_SCREEN_WIDTH
#define LAUNCHER_HEIGHT PSP_DISPLAY_SCREEN_HEIGHT
#define LAUNCHER_STRIDE PSP_DISPLAY_STRIDE

static PspDisplay launcher_display;
static uint16_t *launcher_pixels;
static int launcher_text_x;
static int launcher_text_y;
static bool launcher_frame_visible;
static bool launcher_frame_prepared;

static const uint8_t *launcher_glyph(char character)
{
    static const uint8_t blank[7] = {0};
    static const uint8_t unknown[7] = {31,17,2,4,4,0,4};
    static const uint8_t letters[26][7] = {
        {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30},
        {14,17,16,16,16,17,14}, {30,17,17,17,17,17,30},
        {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
        {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17},
        {14,4,4,4,4,4,14}, {7,2,2,2,2,18,12},
        {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
        {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17},
        {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
        {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
        {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4},
        {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4},
        {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
        {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31}
    };
    static const uint8_t digits[10][7] = {
        {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14},
        {14,17,1,2,4,8,31}, {30,1,1,14,1,1,30},
        {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
        {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8},
        {14,17,17,14,17,17,14}, {14,17,17,15,1,1,14}
    };
    static const uint8_t colon[7] = {0,6,6,0,6,6,0};
    static const uint8_t dash[7] = {0,0,0,31,0,0,0};
    static const uint8_t period[7] = {0,0,0,0,0,6,6};
    unsigned char value = (unsigned char) character;
    if (value >= 'a' && value <= 'z') value -= 'a' - 'A';
    if (value >= 'A' && value <= 'Z') return letters[value - 'A'];
    if (value >= '0' && value <= '9') return digits[value - '0'];
    if (value == ' ') return blank;
    if (value == ':') return colon;
    if (value == '-') return dash;
    if (value == '.') return period;
    return unknown;
}

static bool launcher_prepare_frame(void)
{
    if (launcher_frame_prepared) return launcher_pixels != NULL;
    uint16_t *back = psp_display_back_buffer(&launcher_display);
    if (back == NULL) return false;
    if (launcher_frame_visible) {
        uint16_t *front = psp_display_front_buffer(&launcher_display);
        if (front == NULL) return false;
        memcpy(
            back, front,
            PSP_DISPLAY_BUFFER_PIXELS * sizeof(*back));
    }
    launcher_pixels = back;
    launcher_frame_prepared = true;
    return true;
}

static void launcher_clear(void)
{
    if (!launcher_prepare_frame()) return;
    for (int y = 0; y < LAUNCHER_HEIGHT; y++) {
        uint16_t *row = launcher_pixels + (size_t) y * LAUNCHER_STRIDE;
        for (int x = 0; x < LAUNCHER_STRIDE; x++) row[x] = PSP_THEME_GROUND;
    }
    launcher_text_x = 20;
    launcher_text_y = 18;
}

static void launcher_draw_character(
    int x, int y, char character, uint16_t ink, int scale)
{
    const uint8_t *rows = launcher_glyph(character);
    for (int row = 0; row < 7; row++) {
        for (int column = 0; column < 5; column++) {
            if ((rows[row] & (1u << (4 - column))) == 0) continue;
            for (int dy = 0; dy < scale; dy++) {
                uint16_t *destination = launcher_pixels
                    + (size_t) (y + row * scale + dy) * LAUNCHER_STRIDE
                    + x + column * scale;
                for (int dx = 0; dx < scale; dx++) destination[dx] = ink;
            }
        }
    }
}

static void launcher_splash_rect(
    int x, int y, int width, int height, uint16_t color)
{
    /* Splash geometry is compile-time bounded inside 480x272. Keeping this
       private helper branch-free preserves the frozen launcher's size
       headroom; recovery text still uses the checked path above. */
    for (int row = y; row < y + height; row++) {
        uint16_t *destination =
            launcher_pixels + (size_t) row * LAUNCHER_STRIDE;
        for (int column = x; column < x + width; column++)
            destination[column] = color;
    }
}

/* Same corner test the browser's fill_round_rect uses, so the launcher's
   mark lands on exactly the silhouette the boot entrance draws. */
static void launcher_splash_round_rect(
    int x, int y, int width, int height, int radius, uint16_t color)
{
    for (int row = 0; row < height; row++) {
        int dy = 0;
        if (row < radius) dy = radius - row - 1;
        else if (row >= height - radius) dy = radius - (height - row);
        int inset = 0;
        while (inset < radius) {
            int dx = radius - inset - 1;
            if (dx * dx + dy * dy <= radius * radius) break;
            inset++;
        }
        uint16_t *destination =
            launcher_pixels + (size_t) (y + row) * LAUNCHER_STRIDE;
        for (int column = x + inset; column < x + width - inset; column++)
            destination[column] = color;
    }
}

static int launcher_splash_edge(
    int ax, int ay, int bx, int by, int x, int y)
{
    return (bx - ax) * (y - ay) - (by - ay) * (x - ax);
}

static void launcher_splash_triangle(
    int ax, int ay, int bx, int by, int cx, int cy, uint16_t color)
{
    int left = ax < bx ? ax : bx;
    if (cx < left) left = cx;
    int right = ax > bx ? ax : bx;
    if (cx > right) right = cx;
    int top = ay < by ? ay : by;
    if (cy < top) top = cy;
    int bottom = ay > by ? ay : by;
    if (cy > bottom) bottom = cy;
    for (int y = top; y <= bottom; y++) {
        uint16_t *destination =
            launcher_pixels + (size_t) y * LAUNCHER_STRIDE;
        for (int x = left; x <= right; x++) {
            int first = launcher_splash_edge(ax, ay, bx, by, x, y);
            int second = launcher_splash_edge(bx, by, cx, cy, x, y);
            int third = launcher_splash_edge(cx, cy, ax, ay, x, y);
            if ((first < 0 || second < 0 || third < 0)
                && (first > 0 || second > 0 || third > 0)) continue;
            destination[x] = color;
        }
    }
}

static void launcher_draw_text_at(
    int x, int y, const char *text, uint16_t ink, int scale)
{
    if (text == NULL || !launcher_prepare_frame()) return;
    for (; *text != '\0'; text++) {
        if (x + 5 * scale >= LAUNCHER_WIDTH) break;
        launcher_draw_character(x, y, *text, ink, scale);
        x += 6 * scale;
    }
}

static void launcher_publish_prepared(void)
{
    if (!launcher_frame_prepared) return;
    bool accepted = psp_display_publish(&launcher_display);
    if (accepted) launcher_frame_visible = true;
    launcher_frame_prepared = false;
    launcher_pixels = NULL;
}

static void launcher_puts(const char *text)
{
    if (text == NULL || !launcher_prepare_frame()) return;
    for (; *text != '\0'; text++) {
        if (*text == '\n') {
            launcher_text_x = 20;
            launcher_text_y += 18;
            continue;
        }
        if (launcher_text_x + 10 >= LAUNCHER_WIDTH) {
            launcher_text_x = 20;
            launcher_text_y += 18;
        }
        if (launcher_text_y + 14 < LAUNCHER_HEIGHT)
            launcher_draw_character(
                launcher_text_x, launcher_text_y, *text, PSP_THEME_TEXT, 2);
        launcher_text_x += 12;
    }
    launcher_publish_prepared();
}

/*
 * The mark uses the canonical primitive lists in psp_boot_mark.h and the same
 * screen position as the browser's boot entrance, so the LoadExec handoff is
 * a straight cut rather than a change of scene.
 */
#define LAUNCHER_MARK_SIZE TILEFINCH_BOOT_MARK_UNITS
#define LAUNCHER_MARK_LEFT ((LAUNCHER_WIDTH - LAUNCHER_MARK_SIZE) / 2)
#define LAUNCHER_MARK_TOP TILEFINCH_BOOT_MARK_TOP
#define LAUNCHER_MX(value) (LAUNCHER_MARK_LEFT + (value))
#define LAUNCHER_MY(value) (LAUNCHER_MARK_TOP + (value))

static void launcher_present_splash(void)
{
    launcher_clear();
    /* The mark's primitives are compile-time bounded but not NULL-checked, so
       they must not run when the display service refused a frame and
       launcher_clear left no buffer behind. main() already tolerates a failed
       screen init and boots on regardless. */
    if (launcher_pixels == NULL) return;
    uint16_t accent = PSP_THEME_ACCENT_EMBER;
    uint16_t body = PSP_THEME_ON_ACCENT;
    uint16_t eye = PSP_THEME_TEXT;
#define LAUNCHER_DRAW_MARK_ROUND(x, y, width, height, radius, color) \
    launcher_splash_round_rect(                                  \
        LAUNCHER_MX(x), LAUNCHER_MY(y), width, height, radius, color);
    TILEFINCH_BOOT_MARK_ROUND_RECTS(LAUNCHER_DRAW_MARK_ROUND)
#undef LAUNCHER_DRAW_MARK_ROUND
#define LAUNCHER_DRAW_MARK_TRIANGLE(ax, ay, bx, by, cx, cy, color) \
    launcher_splash_triangle(                                    \
        LAUNCHER_MX(ax), LAUNCHER_MY(ay),                         \
        LAUNCHER_MX(bx), LAUNCHER_MY(by),                         \
        LAUNCHER_MX(cx), LAUNCHER_MY(cy), color);
    TILEFINCH_BOOT_MARK_TRIANGLES(LAUNCHER_DRAW_MARK_TRIANGLE)
#undef LAUNCHER_DRAW_MARK_TRIANGLE
#define LAUNCHER_DRAW_MARK_RECT(x, y, width, height, color) \
    launcher_splash_rect(                                  \
        LAUNCHER_MX(x), LAUNCHER_MY(y), width, height, color);
    TILEFINCH_BOOT_MARK_RECTS(LAUNCHER_DRAW_MARK_RECT)
#undef LAUNCHER_DRAW_MARK_RECT
    /* The hold-L window is live here and only here -- the browser's own
       entrance deliberately carries no controls, because by then the window
       has closed. One muted line, on the launcher's embedded glyphs: the
       launcher must stay independent of the browser's fonts. */
    launcher_draw_text_at(
        177, 248, "HOLD L FOR SAFE START", PSP_THEME_TEXT_MUTED, 1);
    launcher_text_x = 20;
    launcher_text_y = 18;
    launcher_publish_prepared();
}

static bool launcher_screen_init(void)
{
    if (!psp_display_begin(
            &launcher_display, psp_display_system_backend())) {
        return false;
    }
    launcher_clear();
    return launcher_frame_prepared;
}

static int launcher_exit_callback(int arg1, int arg2, void *common)
{
    (void) arg1;
    (void) arg2;
    (void) common;
    sceKernelExitGame();
    return 0;
}

static int launcher_callback_thread(SceSize args, void *argp)
{
    (void) args;
    (void) argp;
    int callback = sceKernelCreateCallback(
        "tilefinch-launcher-exit", launcher_exit_callback, NULL);
    if (callback < 0) return callback;
    int registered = sceKernelRegisterExitCallback(callback);
    if (registered < 0) return registered;
    sceKernelSleepThreadCB();
    return 0;
}

static void launcher_setup_callbacks(void)
{
    int thread = sceKernelCreateThread(
        "tilefinch-launcher-callback", launcher_callback_thread,
        TILEFINCH_PSP_THREAD_PRIORITY_CALLBACK, 4096, 0, NULL);
    if (thread >= 0) sceKernelStartThread(thread, 0, NULL);
}

static bool launcher_slot_path(
    const TilefinchInstallPaths *paths, TilefinchUpdateSlot slot,
    char *output, size_t output_size)
{
    const char *name = slot == TILEFINCH_UPDATE_SLOT_A
        ? "slot-a" : slot == TILEFINCH_UPDATE_SLOT_B ? "slot-b" : NULL;
    if (paths == NULL || name == NULL) return false;
    size_t root_length = strlen(paths->install_root);
    size_t name_length = strlen(name);
    if (root_length + 1u + name_length >= output_size) return false;
    memcpy(output, paths->install_root, root_length);
    output[root_length] = '/';
    memcpy(output + root_length + 1u, name, name_length + 1u);
    return true;
}

static bool launcher_boot_slot(
    const TilefinchInstallPaths *paths, TilefinchUpdateSlot slot)
{
    char slot_dir[TILEFINCH_INSTALL_PATH_LIMIT];
    char eboot[TILEFINCH_INSTALL_PATH_LIMIT];
    if (!launcher_slot_path(paths, slot, slot_dir, sizeof(slot_dir))) {
        return false;
    }
    size_t directory_length = strlen(slot_dir);
    static const char suffix[] = "/EBOOT.PBP";
    if (directory_length + sizeof(suffix) > sizeof(eboot)) return false;
    memcpy(eboot, slot_dir, directory_length);
    memcpy(eboot + directory_length, suffix, sizeof(suffix));
    struct SceKernelLoadExecVSHParam cfw_parameters = {
        .size = sizeof(cfw_parameters),
        .args = strlen(eboot) + 1u,
        .argp = eboot,
        .key = "game",
        .unk5 = 0x10000
    };
    /*
     * The ordinary user-mode sceKernelLoadExec path cannot reliably start
     * another Memory Stick homebrew on post-1.xx firmware. Modern CFWs expose
     * this VSH memory-stick handoff specifically for games and homebrew.
     * A successful call does not return. Keep the ordinary call afterward
     * for PPSSPP and environments which implement only the standard API.
     */
    int cfw_result =
        sctrlKernelLoadExecVSHMs2(eboot, &cfw_parameters);
    struct SceKernelLoadExecParam fallback_parameters = {
        .size = sizeof(fallback_parameters),
        .args = strlen(eboot) + 1u,
        .argp = eboot,
        .key = "game"
    };
    int fallback_result =
        sceKernelLoadExec(eboot, &fallback_parameters);
    char diagnostic[80];
    snprintf(
        diagnostic, sizeof(diagnostic),
        "HANDOFF FAILED: CFW %08X STANDARD %08X\n",
        (unsigned) cfw_result, (unsigned) fallback_result);
    launcher_puts(diagnostic);
    return false;
}

static bool launcher_hash_is_nonzero(const uint8_t digest[32])
{
    uint8_t aggregate = 0;
    for (size_t index = 0; index < 32; index++)
        aggregate |= digest[index];
    return aggregate != 0;
}

static bool launcher_verify_pending(
    const TilefinchInstallPaths *paths, const TilefinchUpdateRoot *root,
    const TilefinchUpdateState *state, TilefinchUpdateStatus *status)
{
    char slot_dir[TILEFINCH_INSTALL_PATH_LIMIT];
    if (!launcher_slot_path(
            paths, state->pending_slot, slot_dir, sizeof(slot_dir))) {
        if (status != NULL) *status = TILEFINCH_UPDATE_BAD_PATH;
        return false;
    }
    time_t now = time(NULL);
    bool installed_pair_valid = state->installed_sequence != 0
        && launcher_hash_is_nonzero(state->installed_sha256);
    bool developer_unsigned = state->candidate_sequence
        == TILEFINCH_UPDATE_DEVELOPER_SEQUENCE;
    TilefinchUpdateSlotVerifyOptions options = {
        .embedded_root = developer_unsigned ? NULL : root,
        .now_unix = now > 0 ? (uint64_t) now : 0,
        .clock_valid = now > 0,
        .launcher_protocol = TILEFINCH_UPDATE_LAUNCHER_PROTOCOL,
        .installed_sequence = state->installed_sequence,
        .installed_package_sha256 = state->installed_sha256,
        .installed_sequence_valid = state->installed_sequence != 0,
        .installed_pair_valid = installed_pair_valid,
        .allow_downgrade = state->candidate_downgrade,
        .trust = developer_unsigned
            ? TILEFINCH_UPDATE_TRUST_DEVELOPER_UNSIGNED
            : TILEFINCH_UPDATE_TRUST_SIGNED
    };
    TilefinchUpdateVerifiedEnvelope verified;
    TilefinchUpdateStatus result = tilefinch_update_verify_slot(
        slot_dir, &options, &verified);
    if (result == TILEFINCH_UPDATE_OK
        && ((!developer_unsigned
             && verified.manifest.release_sequence
                    != state->candidate_sequence)
            || memcmp(
                   verified.manifest.package_sha256,
                   state->candidate_sha256, 32) != 0)) {
        result = TILEFINCH_UPDATE_PACKAGE_MISMATCH;
    }
    if (status != NULL) *status = result;
    return result == TILEFINCH_UPDATE_OK;
}

static uint32_t launcher_wait_pressed(unsigned milliseconds)
{
    SceCtrlData previous = {0};
    sceCtrlReadBufferPositive(&previous, 1);
    unsigned elapsed = 0;
    while (elapsed < milliseconds) {
        SceCtrlData current = {0};
        sceCtrlReadBufferPositive(&current, 1);
        uint32_t pressed = current.Buttons & ~previous.Buttons;
        if (pressed != 0) return pressed;
        previous = current;
        sceKernelDelayThread(16000);
        elapsed += 16;
    }
    return 0;
}

static bool launcher_wait_held(uint32_t button, unsigned milliseconds)
{
    unsigned elapsed = 0;
    do {
        SceCtrlData current = {0};
        sceCtrlPeekBufferPositive(&current, 1);
        if ((current.Buttons & button) != 0) return true;
        if (elapsed >= milliseconds) break;
        sceKernelDelayThread(16000);
        elapsed += 16;
    } while (true);
    return false;
}

static bool launcher_recover(
    const TilefinchInstallPaths *paths, TilefinchUpdateState *state)
{
    launcher_clear();
    /* The heading takes the accent the way a panel title does in the
       browser; the body stays on TEXT. Lines are kept inside the 38 columns
       the 2x glyphs give at this margin, so nothing hard-wraps mid-word. */
    launcher_draw_text_at(
        20, 24, "TILEFINCH SAFE START", PSP_THEME_ACCENT_EMBER, 2);
    launcher_text_x = 20;
    launcher_text_y = 60;
    launcher_puts(
        "The new version did not finish\n"
        "its first start. Your current\n"
        "version is still safe.\n\n"
        "X        TRY UPDATE AGAIN\n"
        "SQUARE   DISCARD UPDATE\n"
        "O        START CURRENT VERSION\n\n"
        "Starting the current version\n"
        "in 10 seconds...\n");
    /*
     * Never leave a user staring at a blind recovery timeout if the display
     * service rejected the mode or frame. In that case the safe default is
     * immediate handoff to the known-good slot.
     */
    unsigned wait_ms =
        launcher_frame_visible && psp_display_healthy(&launcher_display)
        ? 10000u : 0u;
    uint32_t pressed = launcher_wait_pressed(wait_ms);
    if (pressed & PSP_CTRL_CROSS) {
        if (tilefinch_update_state_retry_trial(state)
            && tilefinch_update_journal_store(
                   paths->data_dir, state, NULL, NULL)) {
            return true;
        }
    } else if (pressed & PSP_CTRL_SQUARE) {
        if (tilefinch_update_state_discard_trial(state)) {
            (void) tilefinch_update_journal_store(
                paths->data_dir, state, NULL, NULL);
        }
    }
    return false;
}

int main(int argc, char **argv)
{
    (void) argc;
    launcher_setup_callbacks();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    (void) launcher_screen_init();
    launcher_present_splash();
    bool recovery_button =
        launcher_wait_held(PSP_CTRL_LTRIGGER, 500);

    const char *argv0 = argv != NULL && argv[0] != NULL
        ? argv[0] : "EBOOT.PBP";
    TilefinchInstallPaths paths;
    if (!tilefinch_install_paths_derive_launcher(argv0, &paths)) {
        launcher_puts("COULD NOT LOCATE THE TILEFINCH INSTALL.\n");
        launcher_wait_pressed(30000);
        sceKernelExitGame();
        return 1;
    }
    (void) mkdir(paths.data_dir, 0777);

    TilefinchUpdateState state;
    if (!tilefinch_update_journal_load(paths.data_dir, &state, NULL)) {
        memset(&state, 0, sizeof(state));
        state.generation = 1;
        state.active_slot = TILEFINCH_UPDATE_SLOT_A;
        state.installed_sequence = TILEFINCH_RELEASE_SEQUENCE;
        (void) tilefinch_update_journal_store(
            paths.data_dir, &state, NULL, NULL);
    }

    if (recovery_button) {
        launcher_clear();
        launcher_puts(
            state.previous_slot == TILEFINCH_UPDATE_SLOT_NONE
                ? "SAFE START: NO PREVIOUS VERSION YET\n"
                : "SAFE START: PREVIOUS VERSION SELECTED\n");
    }
    TilefinchUpdateRoot root;
    bool have_root = tilefinch_update_embedded_root(&root);
    TilefinchUpdateStatus verify_status = TILEFINCH_UPDATE_BAD_KEY;
    bool pending_is_developer = state.candidate_sequence
        == TILEFINCH_UPDATE_DEVELOPER_SEQUENCE;
    bool pending_verified = state.trial == TILEFINCH_UPDATE_TRIAL_PENDING
        && (have_root || pending_is_developer)
        && launcher_verify_pending(
               &paths, &root, &state, &verify_status);
    TilefinchUpdateSlot slot = state.active_slot;
    TilefinchUpdateBootAction action = tilefinch_update_boot_decide(
        &state, recovery_button, pending_verified, &slot);

    if (action == TILEFINCH_UPDATE_BOOT_START_TRIAL) {
        launcher_puts(
            pending_is_developer
                ? "UNSIGNED DEVELOPER FILES: OK\nSTARTING TRIAL...\n"
                : "VERIFYING UPDATE: OK\nSTARTING TRIAL...\n");
        if (!tilefinch_update_state_start_trial(&state)
            || !tilefinch_update_journal_store(
                   paths.data_dir, &state, NULL, NULL)) {
            action = TILEFINCH_UPDATE_BOOT_RECOVERY;
        }
    } else if (state.trial == TILEFINCH_UPDATE_TRIAL_PENDING
               && !pending_verified) {
        launcher_puts("UPDATE REJECTED: ");
        launcher_puts(tilefinch_update_status_name(verify_status));
        launcher_puts("\nSTARTING CURRENT VERSION...\n");
        if (tilefinch_update_state_discard_trial(&state)) {
            (void) tilefinch_update_journal_store(
                paths.data_dir, &state, NULL, NULL);
        }
        slot = state.active_slot;
        action = TILEFINCH_UPDATE_BOOT_ACTIVE;
        sceKernelDelayThread(1500000);
    }
    if (action == TILEFINCH_UPDATE_BOOT_RECOVERY) {
        bool retry = launcher_recover(&paths, &state);
        slot = state.active_slot;
        if (retry) {
            TilefinchUpdateStatus retry_status = TILEFINCH_UPDATE_BAD_KEY;
            bool retry_is_developer = state.candidate_sequence
                == TILEFINCH_UPDATE_DEVELOPER_SEQUENCE;
            bool retry_verified = (have_root || retry_is_developer)
                && launcher_verify_pending(
                       &paths, &root, &state, &retry_status);
            bool retry_started = false;
            if (retry_verified) {
                retry_started =
                    tilefinch_update_state_start_trial(&state)
                    && tilefinch_update_journal_store(
                           paths.data_dir, &state, NULL, NULL);
                if (!retry_started) retry_status = TILEFINCH_UPDATE_IO;
            }
            if (retry_started) {
                launcher_puts(
                    retry_is_developer
                        ? "UNSIGNED DEVELOPER FILES: OK\n"
                          "STARTING TRIAL...\n"
                        : "VERIFYING UPDATE: OK\nSTARTING TRIAL...\n");
                slot = state.pending_slot;
            } else {
                launcher_puts("UPDATE RETRY REJECTED: ");
                launcher_puts(
                    tilefinch_update_status_name(retry_status));
                launcher_puts("\nSTARTING CURRENT VERSION...\n");
                if (!retry_verified
                    && state.trial == TILEFINCH_UPDATE_TRIAL_PENDING
                    && tilefinch_update_state_discard_trial(&state)) {
                    (void) tilefinch_update_journal_store(
                        paths.data_dir, &state, NULL, NULL);
                }
                slot = state.active_slot;
                sceKernelDelayThread(1500000);
            }
        }
    }
    if (!launcher_boot_slot(&paths, slot)) {
        launcher_puts(
            "\nCould not start this slot.\n"
            "Hold L during startup to try the previous version.\n"
            "Press HOME to exit.\n");
        sceKernelSleepThreadCB();
    }
    return 0;
}
