#include "tilefinch/psp_ui.h"

/* Shared by the shipping operation journal and the validation input harness.
   Keep the switch exhaustive so a new PspUiAction produces a compiler warning
   instead of silently losing its diagnostic name. */
const char *psp_ui_action_name(PspUiAction action)
{
    switch (action) {
        case PSP_UI_ACTION_NONE: return "none";
        case PSP_UI_ACTION_FOCUS_PREVIOUS: return "focus-previous";
        case PSP_UI_ACTION_FOCUS_NEXT: return "focus-next";
        case PSP_UI_ACTION_FOCUS_UP: return "focus-up";
        case PSP_UI_ACTION_FOCUS_DOWN: return "focus-down";
        case PSP_UI_ACTION_FOCUS_LEFT: return "focus-left";
        case PSP_UI_ACTION_FOCUS_RIGHT: return "focus-right";
        case PSP_UI_ACTION_FOCUS_AT: return "focus-at";
        case PSP_UI_ACTION_ACTIVATE: return "activate";
        case PSP_UI_ACTION_SUBMIT_FOCUSED_TEXT:
            return "submit-focused-text";
        case PSP_UI_ACTION_BACK: return "back";
        case PSP_UI_ACTION_FORWARD: return "forward";
        case PSP_UI_ACTION_RELOAD: return "reload";
        case PSP_UI_ACTION_TOGGLE_READER: return "toggle-reader";
        case PSP_UI_ACTION_TOGGLE_BASIC: return "toggle-basic";
        case PSP_UI_ACTION_TOGGLE_READER_SITE: return "toggle-reader-site";
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
        case PSP_UI_ACTION_INSTALL_OFFLINE_APP: return "install-offline-app";
        case PSP_UI_ACTION_CONFIRM_OFFLINE_APP:
            return "confirm-offline-app";
        case PSP_UI_ACTION_CANCEL_OFFLINE_APP: return "cancel-offline-app";
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
        case PSP_UI_ACTION_CHECK_WIFI_SIGN_IN: return "check-wifi-sign-in";
        case PSP_UI_ACTION_BUILD_DIAGNOSTIC_QR: return "build-diagnostic-qr";
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
        case PSP_UI_ACTION_EDIT_DEVELOPER_URL: return "edit-developer-url";
        case PSP_UI_ACTION_SET_VIDEO_DECODER: return "set-video-decoder";
        case PSP_UI_ACTION_SHOW_HOME: return "show-home";
        case PSP_UI_ACTION_HOME_ACTIVATE: return "home-activate";
        case PSP_UI_ACTION_COLLECTION_ACTIVATE: return "collection-activate";
        case PSP_UI_ACTION_COLLECTION_DELETE: return "collection-delete";
        case PSP_UI_ACTION_RECOVERY_READER: return "recovery-reader";
        case PSP_UI_ACTION_RECOVERY_DISABLE_JAVASCRIPT:
            return "recovery-disable-javascript";
        case PSP_UI_ACTION_RECOVERY_AUDIO_ONLY: return "recovery-audio-only";
        case PSP_UI_ACTION_RECOVERY_LOWER_QUALITY:
            return "recovery-lower-quality";
        case PSP_UI_ACTION_RECOVERY_RETURN: return "recovery-return";
        case PSP_UI_ACTION_SHOW_SITE_STORAGE: return "show-site-storage";
        case PSP_UI_ACTION_SITE_STORAGE_DELETE: return "site-storage-delete";
        case PSP_UI_ACTION_STORAGE_OFFER_SESSION:
            return "storage-offer-session";
        case PSP_UI_ACTION_STORAGE_OFFER_ALWAYS:
            return "storage-offer-always";
        case PSP_UI_ACTION_STORAGE_OFFER_DECLINE:
            return "storage-offer-decline";
        case PSP_UI_ACTION_EXIT: return "exit";
    }
    return "unknown";
}
