#ifndef TILEFINCH_PSP_UI_H
#define TILEFINCH_PSP_UI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/psp_media_state.h"

#include "tilefinch/browser_profile.h"
#include "tilefinch/font.h"

#define PSP_UI_URL_CAPACITY 512
#define PSP_UI_TITLE_CAPACITY 128
#define PSP_UI_STATUS_CAPACITY 80
#define PSP_UI_MENU_ITEM_COUNT 14
#define PSP_UI_MEDIA_TITLE_CAPACITY 128
#define PSP_UI_TAB_LIMIT 5
#define PSP_UI_TAB_TITLE_CAPACITY 40
#define PSP_UI_TAB_DOMAIN_CAPACITY 48
#define PSP_UI_TAB_THUMBNAIL_WIDTH 60
#define PSP_UI_TAB_THUMBNAIL_HEIGHT 34
#define PSP_UI_TEXT_ENTRY_CAPACITY 512
#define PSP_UI_TEXT_SUGGESTION_LIMIT BROWSER_PROFILE_SUGGESTION_LIMIT
#define PSP_UI_FIND_QUERY_LIMIT 96u

/*
 * Native chrome surfaces (HOME and COLLECTIONS). Both read bounded views the
 * frontend refreshes when the underlying data changes; the frame path never
 * allocates and never reaches into the profile itself. Row strings are
 * borrowed pointers into profile/library storage, following the
 * BrowserProfileSuggestion convention: valid until that storage is mutated,
 * which is exactly when the frontend refreshes the view.
 */
#define PSP_UI_HOME_TILE_LIMIT 6
#define PSP_UI_HOME_CONTINUE_LIMIT 4
#define PSP_UI_HOME_LABEL_CAPACITY 24
#define PSP_UI_HOME_DETAIL_CAPACITY 40
/* The largest section is history, which the profile caps at 100. */
#define PSP_UI_COLLECTIONS_ROW_LIMIT BROWSER_PROFILE_HISTORY_LIMIT
#define PSP_UI_COLLECTIONS_TRAILING_CAPACITY 14

typedef struct {
    uint8_t count;
    uint8_t active_index;
    uint8_t hibernated_mask;
    bool can_create;
    char titles[PSP_UI_TAB_LIMIT][PSP_UI_TAB_TITLE_CAPACITY];
    char domains[PSP_UI_TAB_LIMIT][PSP_UI_TAB_DOMAIN_CAPACITY];
    uint8_t thumbnail_valid_mask;
    uint16_t thumbnails[PSP_UI_TAB_LIMIT]
                       [PSP_UI_TAB_THUMBNAIL_WIDTH
                        * PSP_UI_TAB_THUMBNAIL_HEIGHT];
} PspUiTabsView;

typedef struct {
    char label[PSP_UI_HOME_LABEL_CAPACITY];
    char detail[PSP_UI_HOME_DETAIL_CAPACITY];
} PspUiHomeEntry;

typedef struct {
    uint8_t tile_count;
    uint8_t continue_count;
    /* False until the engine finishes coming up behind the first frame; the
       surface stays fully interactive either way and says so in its hint. */
    bool engine_ready;
    PspUiHomeEntry tiles[PSP_UI_HOME_TILE_LIMIT];
    PspUiHomeEntry continues[PSP_UI_HOME_CONTINUE_LIMIT];
} PspUiHomeView;

typedef enum {
    PSP_UI_COLLECTION_OFFLINE = 0,
    PSP_UI_COLLECTION_BOOKMARKS,
    PSP_UI_COLLECTION_HISTORY
} PspUiCollectionSection;

#define PSP_UI_COLLECTION_SECTION_COUNT 3u

typedef struct {
    const char *title;
    const char *detail;
    char trailing[PSP_UI_COLLECTIONS_TRAILING_CAPACITY];
    bool deletable;
} PspUiCollectionsRow;

typedef struct {
    uint8_t section;
    uint16_t count;
    const char *empty_message;
    PspUiCollectionsRow rows[PSP_UI_COLLECTIONS_ROW_LIMIT];
} PspUiCollectionsView;

typedef enum {
    PSP_UI_BUTTON_UP       = 1u << 0,
    PSP_UI_BUTTON_DOWN     = 1u << 1,
    PSP_UI_BUTTON_LEFT     = 1u << 2,
    PSP_UI_BUTTON_RIGHT    = 1u << 3,
    PSP_UI_BUTTON_CONFIRM  = 1u << 4,
    PSP_UI_BUTTON_CANCEL   = 1u << 5,
    PSP_UI_BUTTON_TOOLBAR  = 1u << 6,
    PSP_UI_BUTTON_RELOAD   = 1u << 7,
    PSP_UI_BUTTON_PAGE_UP  = 1u << 8,
    PSP_UI_BUTTON_PAGE_DOWN = 1u << 9,
    PSP_UI_BUTTON_ADDRESS  = 1u << 10,
    PSP_UI_BUTTON_MENU     = 1u << 11
} PspUiButton;

typedef enum {
    PSP_UI_ACTION_NONE = 0,
    PSP_UI_ACTION_FOCUS_PREVIOUS,
    PSP_UI_ACTION_FOCUS_NEXT,
    PSP_UI_ACTION_FOCUS_UP,
    PSP_UI_ACTION_FOCUS_DOWN,
    PSP_UI_ACTION_FOCUS_LEFT,
    PSP_UI_ACTION_FOCUS_RIGHT,
    /* Cursor handoff: focus the target nearest pointer_x/pointer_y. */
    PSP_UI_ACTION_FOCUS_AT,
    PSP_UI_ACTION_ACTIVATE,
    /* Contextual START on a focused single-line field: commit the HTML
       implicit-submit/default Enter action without reopening text entry. */
    PSP_UI_ACTION_SUBMIT_FOCUSED_TEXT,
    PSP_UI_ACTION_BACK,
    PSP_UI_ACTION_FORWARD,
    PSP_UI_ACTION_RELOAD,
    PSP_UI_ACTION_TOGGLE_READER,
    PSP_UI_ACTION_TOGGLE_READER_SITE,
    PSP_UI_ACTION_PAGE_UP,
    PSP_UI_ACTION_PAGE_DOWN,
    PSP_UI_ACTION_SCROLL_TOP,
    PSP_UI_ACTION_SCROLL_BOTTOM,
    PSP_UI_ACTION_OPEN_ADDRESS,
    PSP_UI_ACTION_OPEN_VOICE_ADDRESS,
    PSP_UI_ACTION_OPEN_FIND,
    PSP_UI_ACTION_FIND_PREVIOUS,
    PSP_UI_ACTION_FIND_NEXT,
    PSP_UI_ACTION_FIND_EDIT,
    PSP_UI_ACTION_FIND_CLOSE,
    PSP_UI_ACTION_VOICE_FOCUSED_TEXT,
    PSP_UI_ACTION_HOME,
    PSP_UI_ACTION_SAVE_FOR_LATER,
    PSP_UI_ACTION_SHOW_OFFLINE,
    PSP_UI_ACTION_SHOW_SCREENSHOTS,
    PSP_UI_ACTION_TOGGLE_BOOKMARK,
    PSP_UI_ACTION_SWITCH_TAB,
    PSP_UI_ACTION_NEW_TAB,
    PSP_UI_ACTION_CLOSE_TAB,
    PSP_UI_ACTION_SHOW_BOOKMARKS,
    PSP_UI_ACTION_SHOW_HOMEPAGE,
    PSP_UI_ACTION_SHOW_HISTORY,
    PSP_UI_ACTION_SCREENSHOT,
    PSP_UI_ACTION_POWER_TEST,
    PSP_UI_ACTION_MEDIA_TEST,
    /* Opens the native text-entry service for the local, explicitly
       untrusted Developer update endpoint. Page content cannot emit this. */
    PSP_UI_ACTION_EDIT_DEVELOPER_URL,
    /* Commits the Experimental screen's decoder-program selection to the
       boot override, and -- on the second press, once the restart prompt is
       up -- restarts to apply it. One emitter, two phases, because the
       receiver owns the prompt bit that tells them apart. Page content
       cannot emit this either. */
    PSP_UI_ACTION_SET_VIDEO_DECODER,
    /* Native surfaces. HOME_ACTIVATE and COLLECTION_ACTIVATE carry
       intent.list_index; COLLECTION_DELETE is only ever emitted after the
       row's own confirm step. */
    PSP_UI_ACTION_SHOW_HOME,
    PSP_UI_ACTION_HOME_ACTIVATE,
    PSP_UI_ACTION_COLLECTION_ACTIVATE,
    PSP_UI_ACTION_COLLECTION_DELETE,
    PSP_UI_ACTION_EXIT
} PspUiAction;

typedef struct {
    uint32_t held;
    uint32_t pressed;
    uint8_t analog_x;
    uint8_t analog_y;
    /* Actual time since the preceding UI sample. Zero selects the nominal
       16 ms cadence for deterministic host callers and old integrations. */
    unsigned elapsed_ms;
} PspUiInput;

typedef enum {
    PSP_UI_POINTER_NONE = 0,
    PSP_UI_POINTER_MOVE,
    PSP_UI_POINTER_DOWN,
    PSP_UI_POINTER_UP,
    PSP_UI_POINTER_CANCEL
} PspUiPointerPhase;

typedef enum {
    PSP_UI_SETTING_NONE = 0,
    PSP_UI_SETTING_BROWSER_UI_SCALE,
    PSP_UI_SETTING_PAGE_FONT_PERCENT,
    PSP_UI_SETTING_READER_FONT,
    PSP_UI_SETTING_REMEMBER_READER_SITE_SCALE,
    PSP_UI_SETTING_CUSTOM_HOMEPAGE,
    PSP_UI_SETTING_HISTORY,
    PSP_UI_SETTING_RESTORE_LAST_PAGE,
    PSP_UI_SETTING_TAB_HIBERNATION,
    PSP_UI_SETTING_EXPERIMENTAL_VOICE,
    PSP_UI_SETTING_ADAPTIVE_VOICE_MEMORY,
    PSP_UI_SETTING_ANALOG_CURSOR,
    PSP_UI_SETTING_TEXT_ENTRY_MODE,
    PSP_UI_SETTING_PERSISTENT_CACHE_MB,
    PSP_UI_SETTING_LIVE_CACHE_KIB,
    PSP_UI_SETTING_PERSIST_LOCAL_STORAGE,
    PSP_UI_SETTING_SEARCH_ENGINE,
    PSP_UI_SETTING_COLOR_MODE,
    PSP_UI_SETTING_CHROME_THEME,
    PSP_UI_SETTING_GLYPH_LANGUAGE,
    PSP_UI_SETTING_COLOR_EMOJI,
    PSP_UI_SETTING_YOUTUBE_QUALITY,
    PSP_UI_SETTING_YOUTUBE_COMPACT_RESULTS,
    PSP_UI_SETTING_VIDEO_SCALING,
    PSP_UI_SETTING_VIDEO_STARTUP_BUFFERING,
    PSP_UI_SETTING_RESUME_OFFLINE_DOWNLOADS,
    PSP_UI_SETTING_CONTENT_BLOCKER_MODE,
    PSP_UI_SETTING_CONTENT_BLOCKER_SITE_ALLOWED,
    PSP_UI_SETTING_CONTENT_BLOCKER_COSMETIC_HIDING,
    PSP_UI_SETTING_COOKIE_BANNER_HIDDEN,
    PSP_UI_SETTING_UPDATE_CHECK,
    PSP_UI_SETTING_WAVE_BACKGROUND,
    PSP_UI_SETTING_JAVASCRIPT,
    PSP_UI_SETTING_SITE_JAVASCRIPT,
    PSP_UI_SETTING_SITE_DATA_ALLOWED,
    PSP_UI_SETTING_MIXED_CONTENT_SITE,
    PSP_UI_SETTING_THIRD_PARTY_COOKIES_SITE,
    PSP_UI_SETTING_TLS_SESSION_PERSISTENCE,
    PSP_UI_SETTING_NETWORK_PROFILE,
    PSP_UI_SETTING_UPDATE_CHANNEL
} PspUiSettingId;

typedef union {
    bool boolean;
    unsigned unsigned_value;
    BrowserSearchEngine search_engine;
    BrowserColorMode color_mode;
    BrowserChromeTheme chrome_theme;
    BrowserYoutubeQuality youtube_quality;
    BrowserVideoScaling video_scaling;
    BrowserTextEntryMode text_entry_mode;
    BrowserReaderFont reader_font;
    BrowserUpdateChannel update_channel;
    BrowserGlyphLanguage glyph_language;
    ContentBlockerMode content_blocker_mode;
} PspUiSettingValue;

typedef struct {
    PspUiSettingId id;
    PspUiSettingValue value;
} PspUiSettingChange;

typedef enum {
    PSP_UI_VOICE_COMPONENT_UNKNOWN = 0,
    PSP_UI_VOICE_COMPONENT_NOT_INSTALLED,
    PSP_UI_VOICE_COMPONENT_CHECKING,
    PSP_UI_VOICE_COMPONENT_DOWNLOADING,
    PSP_UI_VOICE_COMPONENT_INSTALLING,
    PSP_UI_VOICE_COMPONENT_READY,
    PSP_UI_VOICE_COMPONENT_ERROR,
    PSP_UI_VOICE_COMPONENT_LEGACY
} PspUiVoiceComponentPhase;

typedef enum {
    PSP_UI_GLYPH_COMPONENT_UNKNOWN = 0,
    PSP_UI_GLYPH_COMPONENT_NOT_INSTALLED,
    PSP_UI_GLYPH_COMPONENT_CHECKING,
    PSP_UI_GLYPH_COMPONENT_DOWNLOADING,
    PSP_UI_GLYPH_COMPONENT_INSTALLING,
    PSP_UI_GLYPH_COMPONENT_READY,
    PSP_UI_GLYPH_COMPONENT_ERROR
} PspUiGlyphComponentPhase;

typedef enum {
    PSP_UI_CURSOR_CROSSHAIR = 0,
    PSP_UI_CURSOR_POINTER,
    PSP_UI_CURSOR_TEXT,
    PSP_UI_CURSOR_MOVE,
    PSP_UI_CURSOR_WAIT,
    PSP_UI_CURSOR_NOT_ALLOWED,
    PSP_UI_CURSOR_RESIZE_HORIZONTAL,
    PSP_UI_CURSOR_RESIZE_VERTICAL,
    PSP_UI_CURSOR_HIDDEN
} PspUiCursorShape;

typedef struct {
    PspUiAction action;
    uint8_t tab_index;
    /* Row index inside the active native surface. */
    uint8_t list_index;
    int scroll_delta;
    PspUiPointerPhase pointer_phase;
    int pointer_x;
    int pointer_y;
    bool scroll_settle;
    bool visual_changed;
    PspUiSettingChange setting;
    bool clear_cache_requested;
    bool clear_cookies_requested;
    bool clear_local_storage_requested;
    bool clear_session_storage_requested;
    bool update_primary_requested;
    bool update_cancel_requested;
    bool load_content_blocker_allowlist_requested;
    bool voice_component_probe_requested;
    bool voice_component_primary_requested;
    bool voice_component_cancel_requested;
    bool voice_component_remove_requested;
    bool glyph_component_probe_requested;
    bool glyph_component_primary_requested;
    bool glyph_component_cancel_requested;
    bool glyph_component_remove_requested;
    uint8_t glyph_component_pack;
} PspUiIntent;

typedef enum {
    PSP_UI_SCREEN_PAGE = 0,
    PSP_UI_SCREEN_MENU,
    PSP_UI_SCREEN_OPTIONS,
    PSP_UI_SCREEN_OPTION_ITEMS,
    PSP_UI_SCREEN_EXPERIMENTAL_OPTIONS,
    PSP_UI_SCREEN_GLYPH_OPTIONS,
    PSP_UI_SCREEN_UPDATE,
    PSP_UI_SCREEN_DATA_OPTIONS,
    PSP_UI_SCREEN_TABS,
    PSP_UI_SCREEN_TEXT_ENTRY,
    PSP_UI_SCREEN_FIND,
    /*
     * Native chrome surfaces. Unlike every screen above they are full
     * surfaces rather than overlays over a page: they own the whole panel,
     * keep the analog cursor live, and are never captured as tab state.
     */
    PSP_UI_SCREEN_HOME,
    PSP_UI_SCREEN_COLLECTIONS
} PspUiScreen;

/* True for the native surfaces above, which draw instead of the page. */
bool psp_ui_screen_is_native_surface(PspUiScreen screen);

typedef struct {
    const char *description;
    const char *text;
    const BrowserProfileSuggestion *suggestions;
    size_t cursor;
    size_t suggestion_count;
    int suggestion_selection;
    unsigned cell;
    bool shifted;
    bool numbers;
    bool replace_all;
    bool allow_submit;
    bool navigation;
} PspUiTextEntryView;

typedef struct {
    char query[PSP_UI_FIND_QUERY_LIMIT + 1u];
    size_t match_count;
    size_t selected;
    bool truncated;
    bool wrapped;
} PspUiFindView;

typedef struct {
    bool chrome_visible;
    PspUiScreen screen;
    bool history_enabled;
    bool restore_last_page;
    bool tab_hibernation_enabled;
    bool resume_offline_downloads;
    bool experimental_voice_input;
    bool adaptive_voice_memory;
    bool analog_cursor_enabled;
    bool danzeff_text_input;
    bool cursor_visible;
    bool cursor_pointer_down;
    uint8_t cursor_shape;
    uint8_t page_scrollbar_width;
    unsigned persistent_cache_mb;
    unsigned live_cache_kib;
    bool persist_local_storage;
    bool update_primary_enabled;
    bool update_cancel_enabled;
    uint8_t validation_power_test_phase;
    uint8_t validation_media_test_phase;
    unsigned youtube_240p : 1;
    unsigned youtube_compact_results : 1;
    /* Nearest neighbour drawn by the CPU rather than the graphics chip's
       bilinear. Clear is Smooth, which is the default. */
    unsigned video_scaling_sharp : 1;
    unsigned video_startup_buffering : 1;
    unsigned content_blocker_mode : 2;
    unsigned content_blocker_site_allowed : 1;
    unsigned reader_mode : 1;
    unsigned reader_font_serif : 1;
    unsigned remember_reader_site_scale : 1;
    unsigned content_blocker_cosmetic_hiding : 1;
    unsigned cookie_banner_hidden : 1;
    unsigned chrome_theme : 2;
    unsigned update_check_enabled : 1;
    /* A completed background check found a newer signed release. */
    unsigned update_release_available : 1;
    unsigned developer_update_available : 1;
    unsigned update_channel : 2;
    /* Reserved legacy profile bit. CPU ambient motion is not rendered. */
    unsigned wave_enabled : 1;
    /* The frontend stepped the clock down or has work pending, so ambient
       motion must stop until it says otherwise. */
    unsigned motion_suppressed : 1;
    unsigned javascript_enabled : 1;
    unsigned site_javascript_enabled : 1;
    unsigned site_data_allowed : 1;
    unsigned tls_session_persistence : 1;
    unsigned mixed_content_site_allowed : 1;
    unsigned third_party_cookie_site_allowed : 1;
    unsigned collections_section : 2;
    /* The experimental decoder-program knob the Experimental screen shows, as
       an index into psp_media_wide_program_choice, and whether a saved
       selection is currently asking to restart. Seeded from the parsed boot
       configuration so the picker opens on the spelling the next boot will
       really use, and never holding one the config gate would reject. Both
       ride in this word's spare bits so the state stays at its ratchet. */
    unsigned experimental_decoder_choice : 3;
    unsigned experimental_decoder_restart_prompt : 1;
    /* How far in the cursor has faded, on the theme's focus-settle budget:
       0 is gone, PSP_THEME_MOTION_FOCUS_FRAMES is fully present. It rides
       in the spare bits of this word so the state stays at its ratchet. */
    unsigned cursor_fade : 2;
    /* Root panels rise into place. Nested panels enter from the right and
       parents return from the left; packed here to preserve the state-size
       ratchet. */
    unsigned overlay_motion : 2;
    /* Optional component presentation is compact persistent chrome state,
       not a second controller. Three bits cover every phase and one bit the
       destructive confirmation while preserving the 1 KiB UI ratchet. */
    unsigned voice_component_phase : 3;
    unsigned voice_component_remove_confirmation : 1;
    /* Encodes -1..1000 as 0..1001 in the remaining bitfield word instead of
       growing this per-frame state beyond its 1 KiB ratchet. */
    unsigned voice_component_progress_plus_one : 10;
    unsigned glyph_language : 3;
    unsigned color_emoji : 1;
    unsigned glyph_component_phase : 3;
    unsigned glyph_component_remove_confirmation : 1;
    /* Same -1..1000 encoding as the voice component above. */
    unsigned glyph_component_progress_plus_one : 10;
    /* Selected PSP Settings > Network Settings slot (1..100). It is a boot
       override rather than profile data, but lives here so Options can show
       the value that the next connection will request. */
    unsigned network_profile : 7;
    unsigned network_profile_label_valid : 1;
    int update_progress_per_mille;
    BrowserSearchEngine search_engine;
    BrowserColorMode color_mode;
    bool page_dark;
    bool loading;
    bool secure;
    bool can_go_back;
    bool can_go_forward;
    bool has_focus;
    bool focus_editable;
    bool custom_homepage_enabled;
    /* All three are frame counters bounded below 1024 by their producers. */
    uint16_t activity_frames;
    uint16_t toast_frames;
    uint16_t loading_phase;
    uint8_t overlay_animation_frames;
    uint8_t toast_entry_frames;
    /* Focus-settle budget from the theme sheet; drives the focus ring only. */
    uint8_t focus_settle_frames;
    /* Every menu is bounded below 16 entries; byte indices avoid spending
       pointer-width state on persistent frontend selections. */
    uint8_t menu_selection;
    uint8_t options_selection;
    uint8_t options_group_selection;
    uint8_t data_options_selection;
    /* Zero, or the destructive Site Data row plus one awaiting confirmation. */
    uint8_t data_clear_confirmation;
    uint8_t tab_selection;
    uint8_t experimental_options_selection;
    uint8_t glyph_options_selection;
    uint8_t glyph_installed_mask;
    uint8_t glyph_operation_pack;
    /* Native surfaces. HOME indexes tiles then CONTINUE rows in one space;
       COLLECTIONS keeps a row selection, its first visible row, and zero or
       the row awaiting a delete confirmation plus one. */
    /* The surface an overlay returns to: PAGE, or a native surface while one
       is showing. Overlays never dismiss straight to a page the user is not
       actually looking at. */
    uint8_t base_screen;
    uint8_t home_selection;
    uint8_t collections_selection;
    uint8_t collections_first_row;
    uint8_t collections_delete_confirmation;
    const PspUiTabsView *tabs;
    /* Exactly one native surface is showing at a time, so their views share
       a slot the way the text-entry and find views already do. */
    union {
        const PspUiHomeView *home;
        const PspUiCollectionsView *collections;
    };
    unsigned browser_ui_scale;
    unsigned page_font_percent;
    int progress_per_mille;
    uint32_t page_requests_blocked;
    /* Presentation saturates; the persisted profile keeps the 64-bit count. */
    uint32_t total_requests_blocked;
    int scroll_y;
    int maximum_scroll_y;
    int focus_x;
    int focus_y;
    int focus_width;
    int focus_height;
    /*
     * Millisecond accumulators, all clamped well below 65 s by the constants
     * that drive them (analog hold 1.5 s, scroll remainder < 1 ms of travel,
     * focus repeat 360 ms, cursor hide 2 s). Sixteen bits each keeps the
     * frontend state inside its size ratchet with room for new surfaces.
     */
    uint16_t analog_hold_ms;
    uint16_t analog_scroll_remainder;
    uint16_t focus_repeat_elapsed_ms;
    uint16_t cursor_idle_ms;
    /* One of the four direction bits, or zero. */
    uint16_t focus_repeat_direction;
    int8_t analog_scroll_direction;
    int cursor_x_milli;
    int cursor_y_milli;
    char url[PSP_UI_URL_CAPACITY];
    char title[PSP_UI_TITLE_CAPACITY];
    char status[PSP_UI_STATUS_CAPACITY];
    /* Update and Options are mutually exclusive overlays. Sharing their
       bounded label storage keeps the per-frame UI state at its 1 KiB
       ratchet instead of charging every browsing frame for an SSID. */
    union {
        struct {
            char update_version[16];
            char update_status[64];
            /* The recovery screen deliberately shows only a compact
               verified note. */
            char update_notes[24];
            char update_primary_label[24];
        };
        char network_profile_label[128];
    };
    /* Borrowed only while the synchronous native text-entry loop is active;
       psp_ui_clear_text_entry() runs before that stack frame can unwind. */
    union {
        const PspUiTextEntryView *text_entry;
        const PspUiFindView *find_view;
    };
} PspUiState;

typedef enum {
    PSP_UI_MEDIA_ACTION_NONE = 0,
    PSP_UI_MEDIA_ACTION_PLAY_PAUSE,
    PSP_UI_MEDIA_ACTION_PREVIEW_SEEK,
    PSP_UI_MEDIA_ACTION_CANCEL_SEEK_PREVIEW,
    PSP_UI_MEDIA_ACTION_SEEK,
    PSP_UI_MEDIA_ACTION_RETRY,
    PSP_UI_MEDIA_ACTION_CLOSE
} PspUiMediaAction;

typedef struct {
    PspUiMediaAction action;
    uint64_t seek_time_us;
    bool visual_changed;
} PspUiMediaIntent;

/*
 * Native media controls are intentionally independent of the page chrome.
 * A decoder/backend publishes only bounded state here; neither the UI nor
 * the controller needs to know which site supplied the media.
 */
typedef struct {
    bool visible;
    bool controls_visible;
    bool resolving;
    bool failed;
    /*
     * A failure the process cannot retry: the firmware decoder was
     * quarantined, so every later open is refused by the backend anyway.
     * The failed panel then names the restart instead of drawing a Retry
     * affordance that can only fail again. psp_ui_media_set_error clears it,
     * so a caller which knows nothing about quarantine keeps the retry.
     */
    bool retry_unavailable;
    bool playing;
    bool ended;
    bool buffering;
    bool seek_preview_active;
    /* Authoritative capabilities projected by the media state machine. */
    bool controls_enabled;
    bool play_pause_enabled;
    bool seek_enabled;
    int8_t analog_seek_direction;
    unsigned controls_remaining_ms;
    unsigned resolving_progress_per_mille;
    uint64_t current_time_us;
    uint64_t duration_us;
    uint64_t seek_preview_time_us;
    uint64_t buffered_until_us;
    const FontFace *title_font;
    char title[PSP_UI_MEDIA_TITLE_CAPACITY];
    char status[PSP_UI_STATUS_CAPACITY];
} PspUiMediaState;

typedef struct {
    const uint16_t *pixels;
    int width;
    int height;
    int stride;
} PspUiMediaPreview;

typedef enum {
    PSP_UI_STARTUP_SPLASH = 0,
    PSP_UI_STARTUP_HOMEPAGE
} PspUiStartupView;

/*
 * The boot entrance: the surface that becomes HOME rather than being
 * replaced by it. The whole choreography is a pure function of `frame`, so
 * the frontend simply presents whichever frame its init has reached -- a
 * fast boot skips ahead and a slow one holds, and neither needs a clock.
 *
 * Frame 0 is the mark alone. Frames 1..9 hold the static mark while startup
 * advances. Frames 10..19 glide it to HOME while the tiles rise, with the
 * CONTINUE list, status line and hint settling last.
 */
#define PSP_UI_BOOT_ENTRANCE_FRAMES 20u
#define PSP_UI_BOOT_ENTRANCE_GLIDE_FRAME 10u

typedef struct {
    unsigned frame;
    /* Retained for source/fixture compatibility; currently ignored. */
    bool wave;
    /*
     * The slow branch. When boot takes a long path the mark stays centred
     * and this one line -- the branch's own existing status string -- shows
     * beneath it in TEXT_MUTED. No other chrome, and never more than one.
     * NULL is the ordinary path.
     */
    const char *branch_status;
} PspUiBootEntranceView;

/*
 * `home` supplies the tiles that rise and the furniture that settles; it may
 * be NULL for the frames that run before a profile exists, in which case the
 * entrance draws only the canonical mark. Safe-start input and its hint belong
 * to the stable launcher, before this process exists.
 */
void psp_ui_boot_entrance_composite(
    const PspUiBootEntranceView *view, const PspUiState *home,
    uint16_t *pixels, int width, int height, int stride);

void psp_ui_init(PspUiState *ui);
/*
 * The device chrome uses the already-loaded application sans face after the
 * engine becomes available.  Glyphs are retained in one bounded process-wide
 * cache so a frame never allocates per character.  Startup rendering keeps
 * using the tiny built-in face until this is installed.
 */
void psp_ui_set_chrome_fonts(
    const FontFace *regular, const FontFace *bold);
void psp_ui_clear_chrome_font(void);
/*
 * `twelve_hour` mirrors PSP_SYSTEMPARAM_ID_INT_TIME_FORMAT: the status
 * clock renders "1:42 PM" when set and "13:42" when clear. A
 * `battery_percent` above 100 is clamped (some packs report over 100
 * transiently); an out-of-range hour, minute, or negative percentage
 * still invalidates the whole status line. `wifi_bars` is the wifi signal
 * as 0..4 filled bars, or a negative value when there is no signal to show
 * (link not READY, or a build without live networking) -- the bars are
 * then absent rather than drawn empty.
 */
void psp_ui_set_device_status(
    unsigned hour, unsigned minute, int battery_percent, bool charging,
    bool twelve_hour, int wifi_bars);
void psp_ui_set_page(PspUiState *ui, const char *title, const char *url,
                     bool secure);
void psp_ui_set_network_profile(
    PspUiState *ui, unsigned profile, const char *ssid);
/* Adopt a user-accepted destination before association/fetch begins while
   the incumbent page pixels remain on screen. */
void psp_ui_set_navigation_target(PspUiState *ui, const char *url);
void psp_ui_set_history(PspUiState *ui, bool can_go_back,
                        bool can_go_forward);
void psp_ui_set_loading(PspUiState *ui, bool loading,
                        int progress_per_mille);
/* Scale a bounded 64-bit progress value onto a small UI extent without
   pulling software 64-bit division into a PSP frame. The result is clamped
   to [0, extent]; shifting both operands retains sub-pixel precision for
   ordinary media durations and multi-gigabyte downloads. */
unsigned psp_ui_ratio_extent_u64(
    uint64_t value, uint64_t total, unsigned extent);
void psp_ui_set_scroll(PspUiState *ui, int scroll_y, int maximum_scroll_y);
void psp_ui_set_page_interaction(PspUiState *ui, PspUiCursorShape cursor,
                                 unsigned scrollbar_width);
void psp_ui_set_focus(PspUiState *ui, bool visible, int x, int y,
                      int width, int height);
void psp_ui_show_status(PspUiState *ui, const char *status,
                        unsigned duration_frames);
void psp_ui_set_update(
    PspUiState *ui, const char *version, const char *status,
    const char *notes, int progress_per_mille, const char *primary_label,
    bool primary_enabled, bool cancel_enabled);
void psp_ui_set_voice_component(
    PspUiState *ui, PspUiVoiceComponentPhase phase,
    int progress_per_mille);
void psp_ui_set_glyph_component(
    PspUiState *ui, uint8_t installed_mask, uint8_t operation_pack,
    PspUiGlyphComponentPhase phase, int progress_per_mille);
void psp_ui_set_tabs(PspUiState *ui, const PspUiTabsView *tabs);
/*
 * Native surfaces. The views are borrowed and must outlive the screen; the
 * setters clamp the surface's selection to what the new view actually holds
 * so a refresh can never leave focus pointing past the end of a list.
 */
void psp_ui_set_home(PspUiState *ui, const PspUiHomeView *home);
void psp_ui_set_collections(
    PspUiState *ui, const PspUiCollectionsView *collections);
/* Opens COLLECTIONS on a section, resetting scroll and any confirmation. */
void psp_ui_show_collections(
    PspUiState *ui, PspUiCollectionSection section);
/*
 * Classifies a navigation target as one of the three legacy
 * tilefinch.local collection URLs a pre-upgrade session may still hold
 * (/bookmarks, /history, /offline, each with an optional trailing slash).
 * Returns true and writes the matching section when it is one; leaves
 * `section` untouched and returns false otherwise. Item routes under
 * /offline (e.g. /offline/video?id=) are deliberately not matched. Pure,
 * so the navigation dispatch can decide to open COLLECTIONS instead of the
 * retired HTML generator, and host tests can exercise the mapping.
 */
bool psp_ui_legacy_collection_url(
    const char *url, PspUiCollectionSection *section);
/*
 * Classifies a navigation target as the built-in start page
 * (https://tilefinch.local/home, with an optional trailing slash).
 *
 * The engine's start-page generator was deliberately removed: nothing
 * serves that address any more, and no site adapter claims the host. Any
 * navigation site that can produce the URL -- a typed address, a restored
 * tab, a history entry, a bookmark row, a reload, a tab switch -- must
 * therefore route it to the native HOME surface instead of starting a page
 * load, which would fetch a host that does not exist and land on an error
 * page. This is the single classifier those sites share; keeping it pure
 * and here lets host tests pin the accepted spellings.
 */
bool psp_ui_native_home_url(const char *url);
/* True for the reserved HTTPS origin itself and every path below it.  This
 * is the final guard before the frontend considers a network navigation:
 * an internal URL which no native/generated page recognizes must fail
 * locally, never leak into DNS as a request for tilefinch.local. */
bool psp_ui_internal_url(const char *url);
void psp_ui_show_home(PspUiState *ui);
/* Hands the panel back to the page; call when a navigation is committed. */
void psp_ui_leave_native_surface(PspUiState *ui);
void psp_ui_set_text_entry(
    PspUiState *ui, const PspUiTextEntryView *view);
void psp_ui_clear_text_entry(PspUiState *ui);
void psp_ui_set_find(PspUiState *ui, const PspUiFindView *view);
void psp_ui_clear_find(PspUiState *ui);
/* Abandon page-directed held/analog input while a candidate navigation owns
   the foreground. The cursor position is retained for the next analog move. */
void psp_ui_suspend_page_input(PspUiState *ui);
PspUiIntent psp_ui_update(PspUiState *ui, const PspUiInput *input);
/* Short-lived visual motion which benefits from entering the presenter
 * without a redundant pre-vblank wait.  False in the steady state, so this
 * never changes the idle-reading cadence or power policy. */
bool psp_ui_motion_pending(const PspUiState *ui);
/*
 * Returns true only when psp_ui_update() has already changed pixels which
 * are useful to publish before the action is dispatched. Page focus and
 * scroll actions change their visible state in the controller, so presenting
 * them before dispatch would publish one stale frame and then publish again.
 */
bool psp_ui_intent_has_predispatch_visual(const PspUiIntent *intent);
/*
 * True when the pre-dispatch presentation is also the final presentation
 * required for this input. Pure overlay movement/open/close changes only
 * PspUiState; settings and page actions still need a later presentation
 * after their side effects and status text have been applied.
 */
bool psp_ui_intent_predispatch_is_complete(const PspUiIntent *intent);
bool psp_ui_color_mode_is_dark(BrowserColorMode mode, unsigned local_hour);
void psp_ui_apply_page_dark_rgb565(
    uint16_t *pixels, int width, int height, int stride);
void psp_ui_composite(const PspUiState *ui, uint16_t *pixels,
                      int width, int height, int stride);
/*
 * Immediate, allocation-free startup presentation. The splash requires no
 * filesystem or engine state; the homepage view mirrors the compiled-in
 * start page closely enough that handing off to the real page does not flash
 * through an unrelated desktop-like layout.
 */
void psp_ui_startup_composite(
    PspUiStartupView view, const char *status, int progress_per_mille,
    uint16_t *pixels, int width, int height, int stride);
size_t psp_ui_state_bytes(void);
void psp_ui_media_init(PspUiMediaState *media);
void psp_ui_media_set_title_font(PspUiMediaState *media,
                                 const FontFace *font);
void psp_ui_media_set(PspUiMediaState *media, bool visible, bool playing,
                      bool ended, uint64_t current_time_us,
                      uint64_t duration_us, const char *title);
void psp_ui_media_set_resolving(PspUiMediaState *media, const char *title);
void psp_ui_media_set_resolving_progress(
    PspUiMediaState *media, const char *status,
    unsigned progress_per_mille);
void psp_ui_media_set_error(PspUiMediaState *media, const char *message);
/*
 * As above, plus a muted second line naming the underlying cause. Both lines
 * share the bounded `status` field; `reason` is dropped when it does not fit.
 */
void psp_ui_media_set_error_reason(PspUiMediaState *media,
                                   const char *message, const char *reason);
void psp_ui_media_set_buffering(PspUiMediaState *media, bool buffering,
                                uint64_t buffered_until_us);
void psp_ui_media_apply_projection(
    PspUiMediaState *media, const PspMediaUiProjection *projection);
void psp_ui_media_set_seek_preview(PspUiMediaState *media,
                                   uint64_t target_time_us);
void psp_ui_media_cancel_seek_preview(PspUiMediaState *media);
void psp_ui_media_show_controls(PspUiMediaState *media);
void psp_ui_media_tick(PspUiMediaState *media, unsigned elapsed_ms);
PspUiMediaIntent psp_ui_media_update(PspUiMediaState *media,
                                     const PspUiInput *input);
/* True only when psp_ui_media_update() has already changed pixels worth
 * publishing before the session consumes the intent. */
bool psp_ui_media_intent_has_predispatch_visual(
    const PspUiMediaIntent *intent);
PspUiMediaIntent psp_ui_media_activate_at(PspUiMediaState *media,
                                          int x, int y,
                                          int width, int height);
void psp_ui_media_composite(const PspUiMediaState *media, uint16_t *pixels,
                            int width, int height, int stride);
void psp_ui_media_composite_with_preview(
    const PspUiMediaState *media, const PspUiMediaPreview *preview,
    uint16_t *pixels, int width, int height, int stride);

/*
 * The rows psp_ui_media_composite_with_preview may write, as half-open
 * [top, bottom) ranges, sorted and merged.
 *
 * The player's overlay is composed in 16-bit colour and, during fullscreen
 * video, has to reach a 32-bit buffer (see src/psp_ui_media_8888.c). The
 * conversion is per row, so what the wrapper needs is exactly this: which rows
 * the composite reads as a blend backdrop and writes as a result. Everything
 * else in the frame is the decoder's bytes and must not be round-tripped
 * through 16 bits at all.
 *
 * Returns the number of bands written, 0 when the composite would draw
 * nothing. tests/test_psp_ui.c holds the two in step by compositing into a
 * poisoned surface and requiring that no pixel outside the declared bands
 * moved.
 */
typedef struct {
    int top;
    int bottom;
} PspUiRowBand;

#define PSP_UI_MEDIA_OVERLAY_BAND_LIMIT 3u

size_t psp_ui_media_overlay_bands(
    const PspUiMediaState *media, int width, int height,
    PspUiRowBand *bands, size_t capacity);

/* Repaint only the opaque scrubber/legend band. This is used by the
 * cooperative seek supervisor while the last complete video frame remains
 * frozen on the 32-bit scanout surface. */
void psp_ui_media_composite_controls(
    const PspUiMediaState *media, uint16_t *pixels,
    int width, int height, int stride);

/*
 * Composite the same overlay over a 32-bit video buffer, using `scratch` --
 * a 16-bit surface of the same stride and height -- as the working surface.
 * Only the rows psp_ui_media_overlay_bands reports are converted in either
 * direction; the picture keeps the decoder's own bytes. See
 * src/psp_ui_media_8888.c for the channel mapping and where it was measured.
 */
void psp_ui_media_composite_8888(
    const PspUiMediaState *media, const PspUiMediaPreview *preview,
    uint32_t *pixels, int width, int height, int stride,
    uint16_t *scratch);

void psp_ui_media_composite_controls_8888(
    const PspUiMediaState *media, uint32_t *pixels,
    int width, int height, int stride, uint16_t *scratch);

size_t psp_ui_media_state_bytes(void);

#endif
