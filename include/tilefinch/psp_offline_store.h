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

#endif
