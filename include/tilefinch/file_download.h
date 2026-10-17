#ifndef TILEFINCH_FILE_DOWNLOAD_H
#define TILEFINCH_FILE_DOWNLOAD_H

#include "tilefinch/offline_download.h"

#define FILE_DOWNLOAD_LIMIT (512u * 1024u * 1024u)
#define FILE_DOWNLOAD_FILENAME_LIMIT OFFLINE_LIBRARY_TITLE_LIMIT

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
bool file_download_snapshot(const FileDownloadManager *manager, uint32_t id,
                            OfflineDownloadSnapshot *snapshot);
void file_download_destroy(FileDownloadManager *manager);

#endif
