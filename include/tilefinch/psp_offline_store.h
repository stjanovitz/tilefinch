#ifndef TILEFINCH_PSP_OFFLINE_STORE_H
#define TILEFINCH_PSP_OFFLINE_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/browser_engine.h"
#include "tilefinch/offline_download.h"
#include "tilefinch/psp_media_session.h"

typedef enum {
    PSP_OFFLINE_ROUTE_NONE = 0,
    PSP_OFFLINE_ROUTE_PAGE,
    PSP_OFFLINE_ROUTE_STATE_CHANGED,
    PSP_OFFLINE_ROUTE_ERROR
} PspOfflineRouteResult;

typedef struct {
    OfflineLibrary library;
    OfflineDownloadManager download;
    uint32_t last_active_id;
    char status[80];
} PspOfflineStore;

void psp_offline_store_init(
    PspOfflineStore *store, Budget *budget, BrowserSession *session,
    const char *directory);
bool psp_offline_store_save_current(
    PspOfflineStore *store, BrowserEngine *engine);
bool psp_offline_store_open_library(
    PspOfflineStore *store, BrowserEngine *engine, bool record_history);
PspOfflineRouteResult psp_offline_store_handle_url(
    PspOfflineStore *store, BrowserEngine *engine, const char *url,
    const char *source_title, bool record_history);
bool psp_offline_store_resolve_media(
    void *context, const char *url, PspMediaOfflineSource *source);
bool psp_offline_store_pump(PspOfflineStore *store);
void psp_offline_store_destroy(PspOfflineStore *store);
const char *psp_offline_store_status(const PspOfflineStore *store);

#endif
