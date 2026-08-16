#ifndef TILEFINCH_OFFLINE_DOWNLOAD_H
#define TILEFINCH_OFFLINE_DOWNLOAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "tilefinch/fetch.h"
#include "tilefinch/offline_library.h"
#include "tilefinch/session.h"

#define OFFLINE_DOWNLOAD_CHUNK_BYTES (256u * 1024u)
#define OFFLINE_DOWNLOAD_STREAM_LIMIT (512u * 1024u * 1024u)
#define OFFLINE_DOWNLOAD_FREE_SPACE_RESERVE (8u * 1024u * 1024u)

typedef enum {
    OFFLINE_DOWNLOAD_IDLE = 0,
    OFFLINE_DOWNLOAD_RESOLVE,
    OFFLINE_DOWNLOAD_VIDEO,
    OFFLINE_DOWNLOAD_AUDIO
} OfflineDownloadPhase;

typedef struct {
    Budget *budget;
    BrowserSession *session;
    OfflineLibrary *library;
    FetchScheduler *scheduler;
    FetchResult *completion;
    YoutubeStream stream;
    OfflineDownloadPhase phase;
    uint32_t active_id;
    uint64_t request_id;
    uint64_t part_offset;
    uint64_t part_total;
    uint64_t request_first;
    uint64_t request_length;
    size_t request_written;
    FILE *output;
    bool headers_valid;
    char request_url[YOUTUBE_MEDIA_URL_CAPACITY + 80u];
    char error[256];
    YoutubeResolverCancelCallback cancel;
    void *cancel_opaque;
    int maximum_height;
} OfflineDownloadManager;

void offline_download_manager_init(
    OfflineDownloadManager *manager, Budget *budget,
    BrowserSession *session, OfflineLibrary *library);
void offline_download_manager_set_cancel(
    OfflineDownloadManager *manager, YoutubeResolverCancelCallback cancel,
    void *opaque);
void offline_download_manager_set_maximum_height(
    OfflineDownloadManager *manager, int maximum_height);
bool offline_download_manager_start(
    OfflineDownloadManager *manager, uint32_t id);
/* Starts the oldest queued item. Paused/failed items require an explicit
   user retry, so a bad item cannot make the queue spin forever. */
bool offline_download_manager_start_next_queued(
    OfflineDownloadManager *manager, uint32_t *started_id);
/* Testable post-resolver boundary. Production reaches the same function
   after a fresh provider resolution; callers must supply a provider-validated
   direct YouTube stream. */
bool offline_download_manager_adopt_resolved(
    OfflineDownloadManager *manager, const YoutubeStream *stream);
bool offline_download_manager_pump(OfflineDownloadManager *manager);
bool offline_download_manager_pause(
    OfflineDownloadManager *manager, uint32_t id);
bool offline_download_manager_active(
    const OfflineDownloadManager *manager, uint32_t *id);
void offline_download_manager_destroy(OfflineDownloadManager *manager);

#endif
