/* Controller mapping, find/UI snapshot sync, screenshots, and small device
 * utilities shared by the browser EBOOT.
 */
#include "psp_app_internal.h"
#include "tilefinch/pixel_math.h"

void psp_text_input_present(
    void *user, const uint16_t *frame, const PspUiState *ui)
{
    (void) user;
    psp_present(frame, ui);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    /* Text entry owns its own controller loop, so the ordinary scripted
       frame-capture tap cannot observe it.  Keep one validation-only capture
       on host0: for real-device typography/layout checks without touching
       the Memory Stick.  A missing PSPLink host is a harmless failed fopen. */
    static bool text_entry_captured;
    if (!text_entry_captured && psp_input_script_running() && ui != NULL
        && ui->screen == PSP_UI_SCREEN_TEXT_ENTRY) {
        const uint16_t *presented = psp_display_front_buffer(&psp_display);
        text_entry_captured = presented != NULL
            && psp_dump_frame_strided_named(
                "host0:/psp-browser-script-dev.prx",
                "frame-danzeff-device.ppm", presented,
                PSP_DISPLAY_BUFFER_PIXELS, PSP_DISPLAY_STRIDE);
    }
#endif
}

_Static_assert(PSP_UI_FIND_QUERY_LIMIT == BROWSER_FIND_QUERY_LIMIT,
               "PSP find UI and engine query limits must agree");

void psp_find_view_update(PspUiFindView *view,
                                 const BrowserFindSnapshot *snapshot)
{
    if (view == NULL || snapshot == NULL) return;
    memset(view, 0, sizeof(*view));
    snprintf(view->query, sizeof(view->query), "%s", snapshot->query);
    view->match_count = snapshot->match_count;
    view->selected = snapshot->selected;
    view->truncated = snapshot->truncated;
    view->wrapped = snapshot->wrapped;
}

void psp_find_sync(BrowserEngine *engine, PspUiState *ui,
                          PspUiFindView *view)
{
    if (ui == NULL || ui->screen != PSP_UI_SCREEN_FIND) return;
    BrowserFindSnapshot snapshot = {0};
    if (!browser_engine_find_snapshot(engine, &snapshot)) {
        psp_ui_clear_find(ui);
        return;
    }
    psp_find_view_update(view, &snapshot);
    psp_ui_set_find(ui, view);
}


size_t psp_text_input_prepare_voice(void *user)
{
    PspVoicePrepareContext *context = user;
    BrowserEngine *engine = context == NULL ? NULL : context->engine;
    /* Closing the full-screen player normally retains a complete paused
       decoder for instant replay. Voice is the exceptional memory-pressure
       path: its fixed acoustic model needs that contiguous space more than a
       hidden player needs a fast replay. Never evict visible playback. */
    bool media_reclaimed = context != NULL
        && psp_media_reclaim_hidden_pipeline(context->media);
    /* Voice may be requested while deferred images, a page fetch, or the HOME
       preconnect still owns PSP socket descriptors. Cancel those handles
       before APCTL/Inet teardown: desktop sockets tolerate the old ordering,
       but real firmware can otherwise leave the network libraries only
       partially terminated and the voice allocation still fragmented. */
    size_t cancelled_network = browser_engine_cancel_network_work(
        engine, "voice input memory reclaim");
#ifdef TILEFINCH_PSP_LIVE_NETWORK
    fetch_preconnect_cancel("voice-memory-reclaim");
#endif
    BrowserOptionalMemoryReclaim reclaim = {0};
    if (!browser_engine_reclaim_optional_memory(engine, &reclaim)) return 0;
#ifdef TILEFINCH_PSP_LIVE_NETWORK
    /*
     * PSP networking modules retain roughly 640 KiB in the same kernel/user
     * partition from which sceKernelCreateThread must allocate its stack.
     * A real PSP-3000 run showed 80 KiB as the largest block after networking,
     * versus the voice worker's deliberately conservative 256 KiB stack.
     * Voice capture has no concurrent transfer, so unload the idle network
     * stack without touching the incumbent page. The next navigation follows
     * the normal bounded reconnect path, and navigation already evicts the
     * resident voice model before reconnecting.
     */
    if (context != NULL && context->network != NULL
        && psp_network_lifecycle_started(
               context->network_lifecycle)) {
        psp_network_lifecycle_request(
            context->network_lifecycle,
            PSP_NETWORK_REQUEST_VOICE_INHIBIT, true, 0,
            context->network, PSP_NETWORK_SUPERVISOR_STOPPING,
            "voice-off");
        bool network_unloaded =
            psp_shutdown_network_logged(context->network);
        printf(
            "tilefinch-voice-reclaim: network=%s "
            "requests-cancelled=%zu system-free=%u system-largest=%u\n",
            network_unloaded ? "unloaded" : "retained-busy",
            cancelled_network,
            sceKernelTotalFreeMemSize(), sceKernelMaxFreeMemSize());
    }
#endif
    printf(
        "tilefinch-voice-reclaim: total=%zu js=%zu http=%zu render=%zu "
        "media=%d "
        "network-cancelled=%zu budget_remaining=%zu\n",
        reclaim.total_bytes, reclaim.javascript_bytes,
        reclaim.session_cache_bytes, reclaim.render_cache_bytes,
        media_reclaimed ? 1 : 0,
        cancelled_network,
        budget_remaining(browser_engine_budget(engine)));
    return reclaim.total_bytes;
}

uint32_t psp_ui_buttons(uint32_t buttons)
{
    uint32_t mapped = 0;
    if (buttons & PSP_CTRL_UP) mapped |= PSP_UI_BUTTON_UP;
    if (buttons & PSP_CTRL_DOWN) mapped |= PSP_UI_BUTTON_DOWN;
    if (buttons & PSP_CTRL_LEFT) mapped |= PSP_UI_BUTTON_LEFT;
    if (buttons & PSP_CTRL_RIGHT) mapped |= PSP_UI_BUTTON_RIGHT;
    if (buttons & PSP_CTRL_CROSS) mapped |= PSP_UI_BUTTON_CONFIRM;
    if (buttons & PSP_CTRL_CIRCLE) mapped |= PSP_UI_BUTTON_CANCEL;
    if (buttons & PSP_CTRL_TRIANGLE) mapped |= PSP_UI_BUTTON_TOOLBAR;
    if (buttons & PSP_CTRL_SQUARE) mapped |= PSP_UI_BUTTON_RELOAD;
    if (buttons & PSP_CTRL_LTRIGGER) mapped |= PSP_UI_BUTTON_PAGE_UP;
    if (buttons & PSP_CTRL_RTRIGGER) mapped |= PSP_UI_BUTTON_PAGE_DOWN;
    if (buttons & PSP_CTRL_START) mapped |= PSP_UI_BUTTON_ADDRESS;
    if (buttons & PSP_CTRL_SELECT) mapped |= PSP_UI_BUTTON_MENU;
    return mapped;
}

PspUiInput psp_ui_input(const SceCtrlData *pad,
                               uint32_t previous_buttons,
                               unsigned elapsed_ms)
{
    uint32_t held = pad == NULL ? 0 : psp_ui_buttons(pad->Buttons);
    PspUiInput input = {
        .held = held,
        .pressed = held & ~previous_buttons,
        .analog_x = pad == NULL ? 128 : pad->Lx,
        .analog_y = pad == NULL ? 128 : pad->Ly,
        .elapsed_ms = elapsed_ms
    };
    return input;
}

/*
 * Names the operation journal and the crash record carry. Seven actions were
 * missing, so a reader-mode toggle or a HOME row opened as
 * `tilefinch-operation: action=none` and a crash during one named nothing;
 * the scripted-input harness surfaced it by cross-checking its own receiver
 * trace against this journal. The switch is now exhaustive with no default,
 * so the next action added to PspUiAction is a -Wswitch warning here rather
 * than another silent "none".
 */
const char *psp_ui_action_name(PspUiAction action)
{
    switch (action) {
        case PSP_UI_ACTION_NONE: return "none";
        case PSP_UI_ACTION_FOCUS_AT: return "focus-at";
        case PSP_UI_ACTION_TOGGLE_READER: return "toggle-reader";
        case PSP_UI_ACTION_TOGGLE_READER_SITE: return "toggle-reader-site";
        case PSP_UI_ACTION_SHOW_HOME: return "show-home";
        case PSP_UI_ACTION_HOME_ACTIVATE: return "home-activate";
        case PSP_UI_ACTION_COLLECTION_ACTIVATE: return "collection-activate";
        case PSP_UI_ACTION_COLLECTION_DELETE: return "collection-delete";
        case PSP_UI_ACTION_FOCUS_PREVIOUS: return "focus-previous";
        case PSP_UI_ACTION_FOCUS_NEXT: return "focus-next";
        case PSP_UI_ACTION_FOCUS_UP: return "focus-up";
        case PSP_UI_ACTION_FOCUS_DOWN: return "focus-down";
        case PSP_UI_ACTION_FOCUS_LEFT: return "focus-left";
        case PSP_UI_ACTION_FOCUS_RIGHT: return "focus-right";
        case PSP_UI_ACTION_ACTIVATE: return "activate";
        case PSP_UI_ACTION_SUBMIT_FOCUSED_TEXT:
            return "submit-focused-text";
        case PSP_UI_ACTION_BACK: return "back";
        case PSP_UI_ACTION_FORWARD: return "forward";
        case PSP_UI_ACTION_RELOAD: return "reload";
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
        case PSP_UI_ACTION_SHOW_DOWNLOADS: return "show-downloads";
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
        case PSP_UI_ACTION_EDIT_DEVELOPER_URL:
            return "edit-developer-url";
        case PSP_UI_ACTION_SET_VIDEO_DECODER:
            return "set-video-decoder";
        case PSP_UI_ACTION_EXIT: return "exit";
    }
    return "none";
}

const char *psp_ui_action_acknowledgement(PspUiAction action)
{
    switch (action) {
        case PSP_UI_ACTION_ACTIVATE: return "ACTION RECEIVED - OPENING...";
        case PSP_UI_ACTION_SUBMIT_FOCUSED_TEXT:
            return "ENTER RECEIVED...";
        case PSP_UI_ACTION_BACK: return "BACK RECEIVED - OPENING...";
        case PSP_UI_ACTION_FORWARD:
            return "FORWARD RECEIVED - OPENING...";
        case PSP_UI_ACTION_RELOAD: return "RELOAD RECEIVED - STARTING...";
        case PSP_UI_ACTION_OPEN_ADDRESS: return "OPENING ADDRESS INPUT...";
        case PSP_UI_ACTION_OPEN_FIND:
        case PSP_UI_ACTION_FIND_EDIT:
        case PSP_UI_ACTION_FIND_PREVIOUS:
        case PSP_UI_ACTION_FIND_NEXT:
        case PSP_UI_ACTION_FIND_CLOSE:
            return NULL;
        case PSP_UI_ACTION_OPEN_VOICE_ADDRESS:
            return "STARTING EXPERIMENTAL VOICE INPUT...";
        case PSP_UI_ACTION_VOICE_FOCUSED_TEXT:
            return "STARTING EXPERIMENTAL VOICE INPUT...";
        case PSP_UI_ACTION_HOME: return "OPENING HOME...";
        case PSP_UI_ACTION_SAVE_FOR_LATER: return "SAVING ARTICLE...";
        case PSP_UI_ACTION_SHOW_OFFLINE: return "OPENING OFFLINE LIBRARY...";
        case PSP_UI_ACTION_SHOW_DOWNLOADS: return "OPENING DOWNLOADS...";
        case PSP_UI_ACTION_SHOW_SCREENSHOTS: return "OPENING SCREENSHOTS...";
        case PSP_UI_ACTION_TOGGLE_BOOKMARK:
            return "UPDATING BOOKMARKS...";
        case PSP_UI_ACTION_SWITCH_TAB: return "OPENING TAB...";
        case PSP_UI_ACTION_NEW_TAB: return "OPENING NEW TAB...";
        case PSP_UI_ACTION_CLOSE_TAB: return "CLOSING TAB...";
        case PSP_UI_ACTION_SHOW_BOOKMARKS:
            return "OPENING BOOKMARKS...";
        case PSP_UI_ACTION_SHOW_HOMEPAGE:
            return "OPENING MY HOMEPAGE...";
        case PSP_UI_ACTION_SHOW_HISTORY: return "OPENING HISTORY...";
        case PSP_UI_ACTION_SCREENSHOT: return NULL;
        case PSP_UI_ACTION_BUILD_DIAGNOSTIC_QR:
            return "BUILDING DIAGNOSTIC QR...";
        case PSP_UI_ACTION_DIAGNOSTIC_QR_PREVIOUS:
        case PSP_UI_ACTION_DIAGNOSTIC_QR_NEXT:
        case PSP_UI_ACTION_DIAGNOSTIC_QR_PART_PREVIOUS:
        case PSP_UI_ACTION_DIAGNOSTIC_QR_PART_NEXT:
        case PSP_UI_ACTION_CLOSE_DIAGNOSTIC_QR:
            return NULL;
        case PSP_UI_ACTION_POWER_TEST: return "POWER TEST CHANGING...";
        case PSP_UI_ACTION_MEDIA_TEST: return "VIDEO TEST STARTING...";
        case PSP_UI_ACTION_EDIT_DEVELOPER_URL:
            return "OPENING DEVELOPER URL INPUT...";
        case PSP_UI_ACTION_SET_VIDEO_DECODER: return NULL;
        case PSP_UI_ACTION_EXIT: return "EXIT RECEIVED...";
        case PSP_UI_ACTION_FOCUS_PREVIOUS:
        case PSP_UI_ACTION_FOCUS_NEXT:
        case PSP_UI_ACTION_FOCUS_UP:
        case PSP_UI_ACTION_FOCUS_DOWN:
        case PSP_UI_ACTION_FOCUS_LEFT:
        case PSP_UI_ACTION_FOCUS_RIGHT:
        case PSP_UI_ACTION_PAGE_UP:
        case PSP_UI_ACTION_PAGE_DOWN:
        case PSP_UI_ACTION_SCROLL_TOP:
        case PSP_UI_ACTION_SCROLL_BOTTOM:
        case PSP_UI_ACTION_NONE:
        default:
            return NULL;
    }
}

#define PSP_SCREENSHOT_LIST_LIMIT 32u
#define PSP_SCREENSHOT_NAME_LIMIT 80u
#define PSP_SCREENSHOT_SWEEP_ENTRY_LIMIT 128u
/* One 480x272 uncompressed PNG is about 385 KB and the temporary is the only
   copy in flight; 1 MB leaves room for the capture plus the directory work. */
#define PSP_SCREENSHOT_FREE_SPACE_BYTES (1024u * 1024u)

/*
 * True only when the free-space query succeeds AND reports less than one
 * capture's worth of room. A query that is unavailable is not treated as a
 * full stick: the capture goes ahead and any real failure is reported by the
 * writer instead of refusing on a guess.
 */
bool psp_screenshot_space_short(const char *directory)
{
    uint64_t available = 0;
    return directory != NULL
        && tilefinch_update_query_free_space(directory, &available)
        && available < PSP_SCREENSHOT_FREE_SPACE_BYTES;
}

static bool psp_screenshot_temporary_name(const char *name)
{
    if (name == NULL || strncmp(name, "tilefinch-", 10u) != 0) return false;
    size_t length = strnlen(name, PSP_SCREENSHOT_NAME_LIMIT);
    if (length < 19u || length >= PSP_SCREENSHOT_NAME_LIMIT
        || strcmp(name + length - 8u, ".png.tmp") != 0) return false;
    for (size_t at = 0; at < length; at++) {
        unsigned char ch = (unsigned char) name[at];
        if (!((ch >= 'a' && ch <= 'z')
              || (ch >= 'A' && ch <= 'Z')
              || (ch >= '0' && ch <= '9')
              || ch == '-' || ch == '.')) return false;
    }
    return true;
}

/*
 * Capture names are unique, so a crash mid-capture strands its `.png.tmp`
 * with nothing to overwrite it. Sweep those before the next capture. Only
 * names this writer could have produced are removed, and the walk is bounded
 * like the offline library's orphan sweep.
 */
static void psp_screenshot_sweep_temporaries(const char *directory)
{
    DIR *folder = opendir(directory);
    if (folder == NULL) return;
    size_t visited = 0;
    struct dirent *entry;
    while ((entry = readdir(folder)) != NULL) {
        if (++visited > PSP_SCREENSHOT_SWEEP_ENTRY_LIMIT) break;
        if (!psp_screenshot_temporary_name(entry->d_name)) continue;
        char path[SCREENSHOT_PNG_PATH_CAPACITY];
        int length = snprintf(
            path, sizeof(path), "%s/%s", directory, entry->d_name);
        if (length <= 0 || (size_t) length >= sizeof(path)) continue;
        (void) remove(path);
    }
    closedir(folder);
}


PspScreenshotDestination psp_screenshot_destination(
    const char *data_directory,
    char output[SCREENSHOT_PNG_PATH_CAPACITY])
{
    if (data_directory == NULL || data_directory[0] == '\0'
        || output == NULL) return PSP_SCREENSHOT_DESTINATION_UNAVAILABLE;
    char directory[SCREENSHOT_PNG_PATH_CAPACITY];
    int directory_length = snprintf(
        directory, sizeof(directory), "%s/screenshots", data_directory);
    if (directory_length <= 0
        || (size_t) directory_length >= sizeof(directory)
        || (mkdir(directory, 0777) != 0 && errno != EEXIST)) {
        return PSP_SCREENSHOT_DESTINATION_UNAVAILABLE;
    }
    /* Reclaim first, then ask how much room is left. */
    psp_screenshot_sweep_temporaries(directory);
    if (psp_screenshot_space_short(directory))
        return PSP_SCREENSHOT_DESTINATION_FULL;
    ScePspDateTime clock = {0};
    int clock_result = sceRtcGetCurrentClockLocalTime(&clock);
    unsigned tick = (unsigned) sceKernelGetSystemTimeLow();
    int length;
    if (clock_result >= 0) {
        length = snprintf(
            output, SCREENSHOT_PNG_PATH_CAPACITY,
            "%s/tilefinch-%04u%02u%02u-%02u%02u%02u-%08x.png",
            directory, clock.year, clock.month, clock.day,
            clock.hour, clock.minute, clock.second, tick);
    } else {
        length = snprintf(
            output, SCREENSHOT_PNG_PATH_CAPACITY,
            "%s/tilefinch-%08x.png", directory, tick);
    }
    return length > 0 && (size_t) length < SCREENSHOT_PNG_PATH_CAPACITY
        ? PSP_SCREENSHOT_DESTINATION_OK
        : PSP_SCREENSHOT_DESTINATION_UNAVAILABLE;
}

typedef struct {
    char name[PSP_SCREENSHOT_NAME_LIMIT];
    uint64_t bytes;
} PspScreenshotListEntry;

static bool psp_screenshot_name_valid(const char *name)
{
    if (name == NULL || strncmp(name, "tilefinch-", 10u) != 0) return false;
    size_t length = strnlen(name, PSP_SCREENSHOT_NAME_LIMIT);
    if (length < 15u || length >= PSP_SCREENSHOT_NAME_LIMIT
        || strcmp(name + length - 4u, ".png") != 0) return false;
    for (size_t at = 0; at < length; at++) {
        unsigned char ch = (unsigned char) name[at];
        if (!((ch >= 'a' && ch <= 'z')
              || (ch >= 'A' && ch <= 'Z')
              || (ch >= '0' && ch <= '9')
              || ch == '-' || ch == '.')) return false;
    }
    return true;
}

static bool psp_screenshot_page_append(
    char *html, size_t capacity, size_t *used, const char *format, ...)
{
    if (html == NULL || used == NULL || *used >= capacity) return false;
    va_list arguments;
    va_start(arguments, format);
    int written = vsnprintf(
        html + *used, capacity - *used, format, arguments);
    va_end(arguments);
    if (written < 0 || (size_t) written >= capacity - *used) return false;
    *used += (size_t) written;
    return true;
}

bool psp_open_screenshot_list(
    BrowserEngine *engine, const char *data_directory, bool push_history)
{
    if (engine == NULL || data_directory == NULL) return false;
    char directory[SCREENSHOT_PNG_PATH_CAPACITY];
    int directory_length = snprintf(
        directory, sizeof(directory), "%s/screenshots", data_directory);
    if (directory_length <= 0
        || (size_t) directory_length >= sizeof(directory)) return false;
    PspScreenshotListEntry entries[PSP_SCREENSHOT_LIST_LIMIT] = {{{0}, 0}};
    size_t count = 0;
    DIR *folder = opendir(directory);
    if (folder != NULL) {
        size_t visited = 0;
        struct dirent *entry;
        while (visited++ < 128u && count < PSP_SCREENSHOT_LIST_LIMIT
               && (entry = readdir(folder)) != NULL) {
            if (!psp_screenshot_name_valid(entry->d_name)) continue;
            PspScreenshotListEntry *retained = &entries[count];
            snprintf(retained->name, sizeof(retained->name), "%.*s",
                     (int) sizeof(retained->name) - 1, entry->d_name);
            char path[SCREENSHOT_PNG_PATH_CAPACITY];
            struct stat status;
            int path_length = snprintf(
                path, sizeof(path), "%s/%s", directory, entry->d_name);
            if (path_length <= 0 || (size_t) path_length >= sizeof(path)
                || stat(path, &status) != 0 || status.st_size < 0) continue;
            retained->bytes = (uint64_t) status.st_size;
            count++;
        }
        closedir(folder);
    }
    for (size_t at = 1; at < count; at++) {
        PspScreenshotListEntry moving = entries[at];
        size_t before = at;
        while (before != 0
               && strcmp(entries[before - 1u].name, moving.name) < 0) {
            entries[before] = entries[before - 1u];
            before--;
        }
        entries[before] = moving;
    }
    const size_t capacity = 24u * 1024u;
    Budget *budget = browser_engine_budget(engine);
    char *html = budget_malloc_category(
        budget, BUDGET_CATEGORY_SESSION, capacity);
    if (html == NULL) return false;
    size_t used = 0;
    bool okay = psp_screenshot_page_append(
        html, capacity, &used,
        "<!doctype html><meta name=viewport content=\"width=device-width\">"
        "<title>Screenshots</title><style>body{font:16px/1.4 sans-serif;"
        "margin:0;padding:14px;background:#f6f4ee;color:#202020}h1{font-size:"
        "22px}.shot{background:#fff;border:1px solid #ccc;border-radius:8px;"
        "padding:9px;margin:8px 0}.meta{color:#666;font-size:13px}</style>"
        "<h1>Screenshots</h1><p>Newest captures are listed first. Copy PNG "
        "files from <strong>data/screenshots</strong> to view or share them.</p>");
    if (okay && count == 0)
        okay = psp_screenshot_page_append(
            html, capacity, &used, "<p>No screenshots saved yet.</p>");
    for (size_t at = 0; okay && at < count; at++) {
        okay = psp_screenshot_page_append(
            html, capacity, &used,
            "<section class=shot><strong>%s</strong><div class=meta>"
            "%llu KB</div></section>", entries[at].name,
            (unsigned long long) ((entries[at].bytes + 1023u) / 1024u));
    }
    if (okay) {
        (void) browser_engine_set_user_css(engine, "", 0);
        okay = browser_engine_commit_html(
                engine, "https://tilefinch.local/screenshots",
                html, used, push_history)
            && browser_engine_refresh_shell(engine)
            && browser_engine_render_frame(engine, NULL);
    }
    budget_free(budget, html);
    return okay;
}

const char *psp_media_action_name(PspUiMediaAction action)
{
    switch (action) {
        case PSP_UI_MEDIA_ACTION_PLAY_PAUSE: return "media-play-pause";
        case PSP_UI_MEDIA_ACTION_PREVIEW_SEEK:
            return "media-preview-seek";
        case PSP_UI_MEDIA_ACTION_CANCEL_SEEK_PREVIEW:
            return "media-cancel-seek-preview";
        case PSP_UI_MEDIA_ACTION_SEEK: return "media-seek";
        case PSP_UI_MEDIA_ACTION_RETRY: return "media-retry";
        case PSP_UI_MEDIA_ACTION_CLOSE: return "media-close";
        case PSP_UI_MEDIA_ACTION_NONE:
        default: return "media-none";
    }
}

void psp_app_capture_supervisor_media_intent(
    PspInteractiveState *interactive)
{
    if (interactive == NULL) return;
    PspUiMediaIntent intent = {0};
    if (!psp_navigation_cooperate_take_media_intent(&intent)) return;
    interactive->deferred_media_intent = intent;
    interactive->deferred_media_intent_pending = true;
}

bool psp_app_dispatch_deferred_media_intent(
    PspMediaSession *media, PspInteractiveState *interactive)
{
    if (media == NULL || interactive == NULL
        || !interactive->deferred_media_intent_pending) return false;
    if (!media->ui.visible) {
        interactive->deferred_media_intent_pending = false;
        interactive->deferred_media_intent = (PspUiMediaIntent) {0};
        return false;
    }
    /* A quality/transport retry may have torn down the old pipeline after the
       callback accepted a direction edge. Keep the one latest target until
       the replacement pipeline is usable; never ask request_seek to consume
       it against a NULL demuxer. */
    if (interactive->deferred_media_intent.action
            == PSP_UI_MEDIA_ACTION_PREVIEW_SEEK
        && media->playback == NULL) return false;

    PspUiMediaIntent intent = interactive->deferred_media_intent;
    interactive->deferred_media_intent_pending = false;
    interactive->deferred_media_intent = (PspUiMediaIntent) {0};
    const char *action = psp_media_action_name(intent.action);
    uint32_t operation = psp_log_operation_begin(action);
    psp_media_execute_intent(media, intent);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    psp_input_script_observe_media(&intent, &media->ui);
#endif
    psp_log_operation_end(operation, action, "deferred");
    return intent.visual_changed
        || intent.action != PSP_UI_MEDIA_ACTION_NONE;
}

/* Files ship beside the EBOOT; derive their directory from argv[0]. */
void psp_sibling_path(char *output, size_t size, const char *argv0,
                             const char *name)
{
    const char *slash = argv0 == NULL ? NULL : strrchr(argv0, '/');
    if (slash == NULL) {
        snprintf(output, size, "%s", name);
        return;
    }
    snprintf(output, size, "%.*s/%s", (int) (slash - argv0), argv0, name);
}

#ifdef TILEFINCH_PSP_VALIDATION_LOG
bool psp_probe_file(
    const char *path, size_t maximum_bytes, size_t *size_out,
    uint8_t digest[TILEFINCH_SHA256_DIGEST_BYTES])
{
    if (path == NULL || size_out == NULL || digest == NULL) return false;
    *size_out = 0;
    FILE *file = fopen(path, "rb");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
        if (file != NULL) fclose(file);
        return false;
    }
    long length = ftell(file);
    if (length < 0 || (size_t) length > maximum_bytes
        || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    size_t size = (size_t) length;
    uint8_t *contents = malloc(size == 0 ? 1 : size);
    bool ok = contents != NULL
        && (size == 0 || fread(contents, 1, size, file) == size)
        && tilefinch_sha256_digest(contents, size, digest);
    free(contents);
    fclose(file);
    if (ok) *size_out = size;
    return ok;
}
#endif

void psp_config_warning(
    void *context, const char *path, size_t line_number, const char *key)
{
    (void) context;
    printf("tilefinch-config: ignored-key path=\"%s\" line=%zu "
           "key=\"%.*s\"\n",
           path, line_number, 80, key);
}

uint64_t psp_frame_hash(const uint16_t *frame, size_t pixels)
{
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < pixels; i++) {
        hash ^= frame[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

bool psp_dump_frame_strided_named(
    const char *argv0, const char *name, const uint16_t *frame,
    size_t pixels, size_t stride_pixels)
{
    if (name == NULL || frame == NULL
        || stride_pixels < PSP_SCREEN_WIDTH
        || stride_pixels > SIZE_MAX / (size_t) PSP_SCREEN_HEIGHT
        || pixels < stride_pixels * (size_t) PSP_SCREEN_HEIGHT)
        return false;
    char dump_path[768];
    psp_sibling_path(dump_path, sizeof(dump_path), argv0, name);
    FILE *dump = fopen(dump_path, "wb");
    if (dump == NULL) return false;
    bool okay = fprintf(
        dump, "P6\n%d %d\n255\n", PSP_SCREEN_WIDTH,
        PSP_SCREEN_HEIGHT) > 0;
    unsigned char row[PSP_SCREEN_WIDTH * 3];
    for (int y = 0; okay && y < PSP_SCREEN_HEIGHT; y++) {
        psp_log_heartbeat();
        for (int x = 0; x < PSP_SCREEN_WIDTH; x++) {
            uint16_t pixel = frame[
                (size_t) y * stride_pixels + (size_t) x];
            row[x * 3] = (unsigned char) (
                tilefinch_rgb565_red_code(pixel) * 255u / 31u);
            row[x * 3 + 1] = (unsigned char) (
                tilefinch_rgb565_green_code(pixel) * 255u / 63u);
            row[x * 3 + 2] = (unsigned char) (
                tilefinch_rgb565_blue_code(pixel) * 255u / 31u);
        }
        okay = fwrite(row, 1, sizeof(row), dump) == sizeof(row);
    }
    if (fflush(dump) != 0 || ferror(dump)) okay = false;
    if (fclose(dump) != 0) okay = false;
    return okay;
}

bool psp_dump_frame_named(
    const char *argv0, const char *name, const uint16_t *frame,
    size_t pixels)
{
    return psp_dump_frame_strided_named(
        argv0, name, frame, pixels, PSP_SCREEN_WIDTH);
}

bool psp_content_blocker_apply_allowed_sites(
    BrowserEngine *engine, const BrowserProfile *profile)
{
    const char *sites[CONTENT_BLOCKER_ALLOW_SITE_LIMIT];
    size_t count = browser_profile_content_blocker_allowed_site_count(profile);
    if (count > CONTENT_BLOCKER_ALLOW_SITE_LIMIT) return false;
    for (size_t i = 0; i < count; i++) {
        sites[i] = browser_profile_content_blocker_allowed_site(profile, i);
        if (sites[i] == NULL) return false;
    }
    return browser_engine_content_blocker_set_allowed_sites(
        engine, count == 0 ? NULL : sites, count);
}

void psp_content_blocker_restore_allowed_site_count(
    BrowserProfile *profile, size_t count)
{
    while (browser_profile_content_blocker_allowed_site_count(profile)
           > count) {
        size_t last = browser_profile_content_blocker_allowed_site_count(
                          profile) - 1u;
        const char *site = browser_profile_content_blocker_allowed_site(
            profile, last);
        char url[CONTENT_BLOCKER_HOST_LIMIT + 16u];
        if (site == NULL
            || snprintf(url, sizeof(url), "https://%s/", site)
                   >= (int) sizeof(url)
            || !browser_profile_set_content_blocker_site_allowed(
                   profile, url, false)) break;
    }
}

void psp_sync_ui(PspUiState *ui, const BrowserEngine *engine,
                        const BrowserProfile *profile)
{
    BrowserViewSnapshot view;
    if (ui == NULL || !browser_engine_view_snapshot(engine, &view)) return;
    psp_ui_set_page(ui, view.title, view.url, view.secure);
    psp_ui_set_history(ui, view.can_go_back, view.can_go_forward);
    psp_ui_set_scroll(ui, view.scroll_y, view.maximum_scroll_y);
    if (profile != NULL) {
        const BrowserSession *security_session =
            browser_engine_session_view(engine);
        ui->site_javascript_enabled =
            browser_profile_site_javascript_enabled(profile, view.url);
        ui->mixed_content_site_allowed =
            browser_session_mixed_content_site_allowed(
                security_session, view.url);
        ui->third_party_cookie_site_allowed =
            browser_profile_third_party_cookie_site_allowed(
                profile, view.url);
        ui->reader_site_always =
            browser_profile_reader_site_always(profile, view.url);
        ContentBlockerMetrics blocker_metrics = {0};
        bool have_blocker_metrics = browser_engine_content_blocker_metrics(
            engine, &blocker_metrics);
        ui->content_blocker_mode = (uint8_t) (have_blocker_metrics
            ? blocker_metrics.mode
            : browser_profile_content_blocker_mode(profile));
        ui->content_blocker_cosmetic_hiding =
            browser_profile_content_blocker_cosmetic_hiding(profile);
        ui->cookie_banner_hidden =
            browser_profile_cookie_banner_hidden(profile, view.url);
        uint64_t total_blocked =
            browser_profile_content_blocker_total_blocked(profile);
        ui->total_requests_blocked = total_blocked > UINT32_MAX
            ? UINT32_MAX : (uint32_t) total_blocked;
        if (strncmp(view.url, "https://tilefinch.local/", 25u) == 0)
            ui->page_requests_blocked = 0;
        bool allowlist_applied = !have_blocker_metrics
            || blocker_metrics.allowed_site_count
                   == browser_profile_content_blocker_allowed_site_count(
                          profile);
        ui->content_blocker_site_allowed =
            ui->content_blocker_mode != CONTENT_BLOCKER_OFF
            && allowlist_applied
            && browser_profile_content_blocker_site_allowed(
                   profile, view.url);
    }
    LayoutCursor cursor = browser_engine_pointer_cursor(engine);
    PspUiCursorShape shape = PSP_UI_CURSOR_CROSSHAIR;
    switch (cursor) {
        case LAYOUT_CURSOR_POINTER:
            shape = PSP_UI_CURSOR_POINTER;
            break;
        case LAYOUT_CURSOR_TEXT:
            shape = PSP_UI_CURSOR_TEXT;
            break;
        case LAYOUT_CURSOR_MOVE:
            shape = PSP_UI_CURSOR_MOVE;
            break;
        case LAYOUT_CURSOR_WAIT:
            shape = PSP_UI_CURSOR_WAIT;
            break;
        case LAYOUT_CURSOR_NOT_ALLOWED:
            shape = PSP_UI_CURSOR_NOT_ALLOWED;
            break;
        case LAYOUT_CURSOR_RESIZE_HORIZONTAL:
            shape = PSP_UI_CURSOR_RESIZE_HORIZONTAL;
            break;
        case LAYOUT_CURSOR_RESIZE_VERTICAL:
            shape = PSP_UI_CURSOR_RESIZE_VERTICAL;
            break;
        case LAYOUT_CURSOR_HIDDEN:
            shape = PSP_UI_CURSOR_HIDDEN;
            break;
        case LAYOUT_CURSOR_AUTO:
        case LAYOUT_CURSOR_CROSSHAIR:
        default:
            break;
    }
    psp_ui_set_page_interaction(
        ui, shape, (unsigned) browser_engine_root_scrollbar_width(engine));
    psp_ui_set_focus(
        ui, view.has_focus, view.focus_x, view.focus_y,
        view.focus_width, view.focus_height);
    ControllerTextInputInfo text_info = {0};
    ui->focus_editable = view.has_focus
        && browser_engine_text_input_info(engine, &text_info)
        && text_info.editable && !text_info.multiline;
}

bool psp_engine_views_refresh(
    PspEngineViews *views, const BrowserEngine *engine)
{
    if (views == NULL) return false;
    BrowserFrontendSnapshot snapshot = {0};
    bool available = browser_engine_frontend_snapshot(engine, &snapshot);
    views->frame = snapshot.frame.pixels;
    views->frame_pixels = snapshot.frame.pixel_count;
    views->navigation = snapshot.navigation;
    views->controller = snapshot.controller;
    return available;
}
