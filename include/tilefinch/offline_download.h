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
/* The transfer rate is re-sampled at most this often. */
#define OFFLINE_DOWNLOAD_SPEED_SAMPLE_US UINT64_C(250000)

typedef enum {
    OFFLINE_DOWNLOAD_IDLE = 0,
    OFFLINE_DOWNLOAD_RESOLVE,
    OFFLINE_DOWNLOAD_VIDEO,
    OFFLINE_DOWNLOAD_AUDIO,
    OFFLINE_DOWNLOAD_FILE
} OfflineDownloadPhase;

typedef struct {
    uint32_t id;
    OfflineDownloadPhase phase;
    uint64_t downloaded_bytes;
    uint64_t total_bytes;
    uint64_t available_bytes;
    uint32_t bytes_per_second;
    bool active;
    char failure_reason[OFFLINE_LIBRARY_FAILURE_LIMIT];
} OfflineDownloadSnapshot;

/* Transfer-rate sampler shared by the video and file lanes. */
typedef struct {
    uint64_t time_us;
    uint64_t bytes;
    uint32_t bytes_per_second;
} OfflineDownloadSpeed;

typedef struct {
    Budget *budget;
    struct FileDownloadManager *file;
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
    uint64_t available_bytes;
    OfflineDownloadSpeed speed;
} OfflineDownloadManager;

/* Rules shared by every download lane (video, file and ZIP install), so a
   tuning change reaches all of them. */
/* Strict unsigned decimal: digits only, no sign or space, no overflow. */
bool offline_download_parse_decimal(const char *text, uint64_t *value);
/* "bytes FIRST-LAST/COMPLETE" and nothing after it. */
bool offline_download_parse_content_range(
    const char *text, uint64_t *first, uint64_t *last, uint64_t *complete);
/* True when BYTES more still leave the free-space reserve on the stick. */
bool offline_download_space_admits(uint64_t available, uint64_t bytes);
/* Starts a rate sample at BYTES already present. */
void offline_download_speed_reset(OfflineDownloadSpeed *speed,
                                  uint64_t bytes);
/* Records BYTES present now. A count below the sample base (a transfer that
   restarted from zero) starts a new sample instead of a rate. */
void offline_download_speed_sample(OfflineDownloadSpeed *speed,
                                   uint64_t bytes);
/* One bounded owner-thread pump of a download scheduler: at most two body
   callbacks, 32 KiB or 2 ms per call. */
void offline_download_pump_transfer(FetchScheduler *scheduler);

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
/* The only producer of download snapshots, for video and file items. */
bool offline_download_manager_snapshot(
    const OfflineDownloadManager *manager, uint32_t id,
    OfflineDownloadSnapshot *snapshot);
void offline_download_manager_destroy(OfflineDownloadManager *manager);

#endif
