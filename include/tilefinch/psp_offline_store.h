#ifndef TILEFINCH_PSP_OFFLINE_STORE_H
#define TILEFINCH_PSP_OFFLINE_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/browser_engine.h"
#include "tilefinch/browser_profile.h"
#include "tilefinch/offline_download.h"
#include "tilefinch/psp_media_session.h"
#include "tilefinch/psp_ui.h"

typedef struct PspOfflineAppPreparation PspOfflineAppPreparation;

typedef enum {
    PSP_OFFLINE_ROUTE_NONE = 0,
    PSP_OFFLINE_ROUTE_PAGE,
    PSP_OFFLINE_ROUTE_STATE_CHANGED,
    PSP_OFFLINE_ROUTE_ERROR
} PspOfflineRouteResult;

typedef struct {
    OfflineLibrary library;
    OfflineDownloadManager download;
    BrowserSession *session;
    unsigned char *app_icon_cache;
    uint32_t app_icon_ids[OFFLINE_LIBRARY_ITEM_LIMIT];
    PspOfflineAppPreparation *app_preparation;
    uint32_t last_active_id;
    char status[80];
    /* Optional: shows progress text (install, preview, recompile) while
       that work runs, e.g. on the PSP's busy-work status line. */
    void (*show_progress)(void *context, const char *status);
    void *show_progress_context;
    /* Library row activation of an app with stale bytecode: the app the
       recompile offer is shown for, and one the user chose to open anyway. */
    uint32_t recompile_offer_id;
    uint32_t open_anyway_id;
    /* The installed app the current document was opened from: its source
       URL and the navigation generation that committed it. Installing that
       page again reuses the stored manifest metadata and icon. */
    char opened_app_url[OFFLINE_LIBRARY_URL_LIMIT];
    uint64_t opened_app_generation;
} PspOfflineStore;

void psp_offline_store_init(
    PspOfflineStore *store, Budget *budget, BrowserSession *session,
    const char *directory);
bool psp_offline_store_save_current(
    PspOfflineStore *store, BrowserEngine *engine);
bool psp_offline_store_install_current_app(
    PspOfflineStore *store, BrowserEngine *engine);
bool psp_offline_store_prepare_current_app(
    PspOfflineStore *store, BrowserEngine *engine);
void psp_offline_store_discard_app_preparation(PspOfflineStore *store);
const PspUiOfflineAppPreview *psp_offline_store_app_preview(
    const PspOfflineStore *store);
bool psp_offline_store_open_library(
    PspOfflineStore *store, BrowserEngine *engine, bool record_history);
PspOfflineRouteResult psp_offline_store_handle_url(
    PspOfflineStore *store, BrowserEngine *engine,
    const BrowserProfile *profile, const char *url, const char *source_title,
    bool record_history);
bool psp_offline_store_resolve_media(
    void *context, const char *url, PspMediaOfflineSource *source);
bool psp_offline_store_pump(PspOfflineStore *store);
void psp_offline_store_destroy(PspOfflineStore *store);
const char *psp_offline_store_status(const PspOfflineStore *store);
const unsigned char *psp_offline_store_app_icon(
    PspOfflineStore *store, uint32_t id);
/* Opening `id` from the Library: true when its bytecode is stale and the
   recompile offer (psp_offline_store_app_preview, operation
   PSP_UI_OFFLINE_APP_RECOMPILE) was prepared instead. False once the user
   chose to open it anyway. */
bool psp_offline_store_offer_recompile(PspOfflineStore *store, uint32_t id);
/* The offer's answers. Recompile publishes a new generation (new id). */
bool psp_offline_store_recompile_app(PspOfflineStore *store, uint32_t id,
                                     uint32_t *new_id);
uint32_t psp_offline_store_take_offer(PspOfflineStore *store,
                                      bool open_anyway);

#endif
