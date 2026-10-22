#ifndef TILEFINCH_FILE_DOWNLOAD_H
#define TILEFINCH_FILE_DOWNLOAD_H

#include "tilefinch/offline_download.h"

#define FILE_DOWNLOAD_LIMIT (512u * 1024u * 1024u)
#define FILE_DOWNLOAD_FILENAME_LIMIT OFFLINE_LIBRARY_TITLE_LIMIT
/* A file item's paths: "<directory>/<8-hex ID>-<name>" for the file and
   "<directory>/<8-hex ID><suffix>" for its part and metadata files. The 24
   bytes cover "/", the ID, "-" and the longest suffix, ".file.meta.tmp". */
#define FILE_DOWNLOAD_PATH_LIMIT \
    (OFFLINE_LIBRARY_DIRECTORY_LIMIT + FILE_DOWNLOAD_FILENAME_LIMIT + 24u)

/* Network files are inert bytes: this API never extracts, installs, or runs
   them. Only explicit browser UI can enqueue a transfer. */
typedef struct FileDownloadManager FileDownloadManager;

bool file_download_url_supported(const char *url);
bool file_download_url_candidate(const char *url);
bool file_download_response_candidate(const FetchResult *response);
bool file_download_filename(const char *url, const char *disposition,
                            char output[FILE_DOWNLOAD_FILENAME_LIMIT]);
bool file_download_filename_valid(const char *name);
bool file_download_path(const OfflineLibrary *library, uint32_t id,
                        char *output, size_t capacity);
bool file_download_enqueue(OfflineLibrary *library, const char *url,
                           const char *suggested_name, uint32_t *id,
                           char *error, size_t error_size);
FileDownloadManager *file_download_create(Budget *budget,
    BrowserSession *session, OfflineLibrary *library);
bool file_download_start(FileDownloadManager *manager, uint32_t id);
bool file_download_pump(FileDownloadManager *manager);
bool file_download_pause(FileDownloadManager *manager, uint32_t id);
bool file_download_active(const FileDownloadManager *manager, uint32_t *id);
/* Live figures of the active transfer ID, for the one snapshot producer
   (offline_download_manager_snapshot). False when ID is not active. */
bool file_download_progress(const FileDownloadManager *manager, uint32_t id,
                            uint64_t *total_bytes, uint64_t *available_bytes,
                            uint32_t *bytes_per_second);
void file_download_destroy(FileDownloadManager *manager);

#endif
