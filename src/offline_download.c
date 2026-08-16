#include "tilefinch/offline_download.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "tilefinch/media_http.h"
#include "tilefinch/update.h"

#include "tilefinch/user_agent.h"
#define OFFLINE_DOWNLOAD_USER_AGENT TILEFINCH_BROWSER_USER_AGENT

static uint64_t offline_file_size(const char *path)
{
    struct stat status;
    return path != NULL && stat(path, &status) == 0 && status.st_size >= 0
        ? (uint64_t) status.st_size : 0;
}

static bool parse_u64(const char *text, uint64_t *value)
{
    if (text == NULL || text[0] == '\0' || value == NULL) return false;
    uint64_t parsed = 0;
    for (const unsigned char *at = (const unsigned char *) text;
         *at != 0; at++) {
        if (!isdigit(*at)) return false;
        unsigned digit = *at - '0';
        if (parsed > (UINT64_MAX - digit) / 10u) return false;
        parsed = parsed * 10u + digit;
    }
    *value = parsed;
    return true;
}

static bool parse_content_range(
    const char *text, uint64_t first, uint64_t last, uint64_t complete)
{
    unsigned long long parsed_first = 0, parsed_last = 0, parsed_total = 0;
    char tail = '\0';
    return text != NULL && sscanf(
        text, "bytes %llu-%llu/%llu%c",
        &parsed_first, &parsed_last, &parsed_total, &tail) == 3
        && parsed_first == first && parsed_last == last
        && parsed_total == complete;
}

static bool download_headers(void *opaque, const FetchResult *metadata)
{
    OfflineDownloadManager *manager = opaque;
    if (manager == NULL || metadata == NULL) return false;
    char content_length[64] = {0};
    uint64_t declared = 0;
    bool exact_length = fetch_response_header_value(
            metadata, "content-length", content_length,
            sizeof(content_length))
        && parse_u64(content_length, &declared)
        && declared == manager->request_length;
    bool valid = false;
    if (metadata->status_code == 206) {
        char content_range[128] = {0};
        valid = exact_length && fetch_response_header_value(
                metadata, "content-range", content_range,
                sizeof(content_range))
            && parse_content_range(
                content_range, manager->request_first,
                manager->request_first + manager->request_length - 1u,
                manager->part_total);
    } else if (metadata->status_code == 200) {
        /* The provider's range= query convention returns the exact requested
           slice with 200. Requiring its Content-Length before exposing a
           body prevents an ignored query from corrupting a resumed file. */
        valid = exact_length;
    }
    manager->headers_valid = valid;
    if (!valid)
        snprintf(manager->error, sizeof(manager->error),
                 "download range response was not exact (HTTP %ld)",
                 metadata->status_code);
    return valid;
}

static bool download_body(
    void *opaque, const unsigned char *data, size_t length)
{
    OfflineDownloadManager *manager = opaque;
    if (manager == NULL || !manager->headers_valid
        || manager->output == NULL || data == NULL
        || manager->request_written > manager->request_length
        || length > manager->request_length - manager->request_written
        || fwrite(data, 1, length, manager->output) != length) {
        if (manager != NULL)
            snprintf(manager->error, sizeof(manager->error),
                     "Memory Stick write failed");
        return false;
    }
    manager->request_written += length;
    return true;
}

void offline_download_manager_init(
    OfflineDownloadManager *manager, Budget *budget,
    BrowserSession *session, OfflineLibrary *library)
{
    if (manager == NULL) return;
    memset(manager, 0, sizeof(*manager));
    manager->budget = budget;
    manager->session = session;
    manager->library = library;
    manager->maximum_height = 360;
}

void offline_download_manager_set_maximum_height(
    OfflineDownloadManager *manager, int maximum_height)
{
    if (manager == NULL) return;
    manager->maximum_height = maximum_height <= 240 ? 240 : 360;
}

void offline_download_manager_set_cancel(
    OfflineDownloadManager *manager, YoutubeResolverCancelCallback cancel,
    void *opaque)
{
    if (manager == NULL) return;
    manager->cancel = cancel;
    manager->cancel_opaque = opaque;
}

static void download_close_output(OfflineDownloadManager *manager)
{
    if (manager != NULL && manager->output != NULL) {
        (void) fflush(manager->output);
        (void) fclose(manager->output);
        manager->output = NULL;
    }
}

static void download_cancel_request(OfflineDownloadManager *manager)
{
    if (manager == NULL || manager->scheduler == NULL
        || manager->request_id == 0) return;
    (void) fetch_scheduler_cancel(
        manager->scheduler, manager->request_id, "offline download paused");
    (void) fetch_scheduler_discard(manager->scheduler, manager->request_id);
    manager->request_id = 0;
}

static void download_set_state(
    OfflineDownloadManager *manager, OfflineItemState state,
    const char *error)
{
    OfflineLibraryItem *item = manager == NULL ? NULL
        : offline_library_find_mutable(manager->library, manager->active_id);
    if (item != NULL) {
        item->state = state;
        (void) offline_library_save(manager->library);
    }
    if (manager != NULL && error != NULL && error != manager->error)
        snprintf(manager->error, sizeof(manager->error), "%s", error);
}

static void download_reset_active(OfflineDownloadManager *manager)
{
    if (manager == NULL) return;
    download_cancel_request(manager);
    download_close_output(manager);
    if (manager->completion != NULL) {
        fetch_result_destroy(manager->completion);
        budget_free(manager->budget, manager->completion);
        manager->completion = NULL;
    }
    manager->phase = OFFLINE_DOWNLOAD_IDLE;
    manager->active_id = 0;
    manager->part_offset = 0;
    manager->part_total = 0;
    manager->request_first = 0;
    manager->request_length = 0;
    manager->request_written = 0;
    manager->headers_valid = false;
    memset(&manager->stream, 0, sizeof(manager->stream));
}

bool offline_download_manager_start(
    OfflineDownloadManager *manager, uint32_t id)
{
    if (manager == NULL || manager->budget == NULL
        || manager->session == NULL || manager->library == NULL
        || !offline_library_load(manager->library)) return false;
    OfflineLibraryItem *item = offline_library_find_mutable(
        manager->library, id);
    if (item == NULL || item->type != OFFLINE_ITEM_YOUTUBE
        || item->state == OFFLINE_ITEM_READY) return false;
    if (manager->active_id != 0 && manager->active_id != id) return false;
    if (manager->scheduler == NULL) {
        manager->scheduler = fetch_scheduler_create(
            manager->budget, 1, OFFLINE_DOWNLOAD_CHUNK_BYTES);
        if (manager->scheduler == NULL) return false;
        /* Network production may block in PSP DNS/TLS even though the
           download state machine is cooperative. Keep validation, progress,
           cancellation, free-space policy and all file/index writes on this
           browser-thread pump; move only the authorized HTTP hop. */
        (void) fetch_scheduler_enable_background_transport(
            manager->scheduler, true);
    }
    if (manager->completion == NULL) {
        manager->completion = budget_calloc_category(
            manager->budget, BUDGET_CATEGORY_RESOURCE,
            1, sizeof(*manager->completion));
        if (manager->completion == NULL) return false;
        manager->completion->budget = manager->budget;
    }
    manager->active_id = id;
    manager->phase = OFFLINE_DOWNLOAD_RESOLVE;
    manager->error[0] = '\0';
    OfflineItemState previous_state = item->state;
    item->state = OFFLINE_ITEM_DOWNLOADING;
    if (offline_library_save(manager->library)) return true;
    item->state = previous_state;
    download_reset_active(manager);
    return false;
}

bool offline_download_manager_start_next_queued(
    OfflineDownloadManager *manager, uint32_t *started_id)
{
    if (started_id != NULL) *started_id = 0;
    if (manager == NULL || manager->active_id != 0
        || manager->library == NULL
        || !offline_library_load(manager->library)) return false;
    for (size_t index = 0; index < manager->library->count; index++) {
        OfflineLibraryItem *item = &manager->library->items[index];
        if (item->type != OFFLINE_ITEM_YOUTUBE
            || item->state != OFFLINE_ITEM_QUEUED) continue;
        if (!offline_download_manager_start(manager, item->id)) return false;
        if (started_id != NULL) *started_id = item->id;
        return true;
    }
    return false;
}

static bool download_begin_part(
    OfflineDownloadManager *manager, const char *remote_url,
    uint64_t total, const char *part_suffix)
{
    if (manager == NULL || remote_url == NULL || total == 0
        || total > OFFLINE_DOWNLOAD_STREAM_LIMIT) return false;
    char path[OFFLINE_LIBRARY_DIRECTORY_LIMIT + 40u];
    if (!offline_library_item_path(
            manager->library, manager->active_id, part_suffix,
            path, sizeof(path))) return false;
    uint64_t offset = offline_file_size(path);
    if (offset > total) {
        (void) remove(path);
        offset = 0;
    }
    manager->output = fopen(path, offset == 0 ? "wb" : "ab");
    if (manager->output == NULL) return false;
    manager->part_offset = offset;
    manager->part_total = total;
    snprintf(manager->request_url, sizeof(manager->request_url), "%s",
             remote_url);
    return true;
}

static bool download_resolve(OfflineDownloadManager *manager)
{
    OfflineLibraryItem *item = offline_library_find_mutable(
        manager->library, manager->active_id);
    if (item == NULL) return false;
    char error[256] = {0};
    memset(&manager->stream, 0, sizeof(manager->stream));
    bool resolved = youtube_resolve_progressive_mp4_cancelable(
        manager->budget, manager->session, item->source_url,
        manager->maximum_height, 30000,
        manager->cancel, manager->cancel_opaque,
        &manager->stream, error, sizeof(error));
    if (!resolved || manager->stream.content_length == 0
        || manager->stream.content_length > OFFLINE_DOWNLOAD_STREAM_LIMIT
        || (manager->stream.split_streams
            && (manager->stream.audio_content_length == 0
                || manager->stream.audio_content_length
                       > OFFLINE_DOWNLOAD_STREAM_LIMIT))) {
        download_set_state(
            manager, OFFLINE_ITEM_FAILED,
            error[0] == '\0' ? "video exceeds the offline size bound" : error);
        download_reset_active(manager);
        return false;
    }
    if (offline_download_manager_adopt_resolved(
            manager, &manager->stream))
        return true;
    download_set_state(manager, OFFLINE_ITEM_FAILED,
                       "resolved video failed download policy");
    download_reset_active(manager);
    return false;
}

bool offline_download_manager_adopt_resolved(
    OfflineDownloadManager *manager, const YoutubeStream *resolved)
{
    OfflineLibraryItem *item = manager == NULL ? NULL
        : offline_library_find_mutable(
              manager->library, manager->active_id);
    if (item == NULL || resolved == NULL
        || manager->phase != OFFLINE_DOWNLOAD_RESOLVE
        || !youtube_media_url_supported(resolved->media_url)
        || (resolved->split_streams
            && !youtube_media_url_supported(resolved->audio_url))
        || resolved->content_length == 0
        || resolved->content_length > OFFLINE_DOWNLOAD_STREAM_LIMIT
        || (resolved->split_streams
            && (resolved->audio_content_length == 0
                || resolved->audio_content_length
                       > OFFLINE_DOWNLOAD_STREAM_LIMIT))) {
        return false;
    }
    manager->stream = *resolved;
    const YoutubeStream *stream = &manager->stream;
    uint64_t required = stream->content_length;
    if (stream->split_streams) {
        if (required > UINT64_MAX - stream->audio_content_length) {
            download_set_state(manager, OFFLINE_ITEM_FAILED,
                               "video size is invalid");
            download_reset_active(manager);
            return false;
        }
        required += stream->audio_content_length;
    }
    char video_part[OFFLINE_LIBRARY_DIRECTORY_LIMIT + 40u] = {0};
    char audio_part[OFFLINE_LIBRARY_DIRECTORY_LIMIT + 40u] = {0};
    uint64_t existing_video = offline_library_item_path(
            manager->library, manager->active_id, ".video.part",
            video_part, sizeof(video_part))
        ? offline_file_size(video_part) : 0;
    uint64_t existing_audio = offline_library_item_path(
            manager->library, manager->active_id, ".audio.part",
            audio_part, sizeof(audio_part))
        ? offline_file_size(audio_part) : 0;
    char video_final[OFFLINE_LIBRARY_DIRECTORY_LIMIT + 40u] = {0};
    char audio_final[OFFLINE_LIBRARY_DIRECTORY_LIMIT + 40u] = {0};
    uint64_t final_video = offline_library_item_path(
            manager->library, manager->active_id, ".video.mp4",
            video_final, sizeof(video_final))
        ? offline_file_size(video_final) : 0;
    uint64_t final_audio = offline_library_item_path(
            manager->library, manager->active_id, ".audio.mp4",
            audio_final, sizeof(audio_final))
        ? offline_file_size(audio_final) : 0;
    if ((item->content_bytes != 0
         && item->content_bytes != stream->content_length)
        || (item->audio_bytes != 0
            && item->audio_bytes != stream->audio_content_length)
        || existing_video > stream->content_length
        || existing_audio > stream->audio_content_length
        || (final_video != 0 && final_video != stream->content_length)
        || (final_audio != 0
            && final_audio != stream->audio_content_length)) {
        if (video_part[0] != '\0') (void) remove(video_part);
        if (audio_part[0] != '\0') (void) remove(audio_part);
        if (video_final[0] != '\0') (void) remove(video_final);
        if (audio_final[0] != '\0') (void) remove(audio_final);
        existing_video = 0;
        existing_audio = 0;
        final_video = 0;
        final_audio = 0;
        item->downloaded_bytes = 0;
    } else {
        if (final_video == stream->content_length && existing_video != 0) {
            (void) remove(video_part);
            existing_video = 0;
        }
        if (final_audio == stream->audio_content_length
            && existing_audio != 0) {
            (void) remove(audio_part);
            existing_audio = 0;
        }
        item->downloaded_bytes = final_video + final_audio
            + existing_video + existing_audio;
    }
    uint64_t retained = item->downloaded_bytes;
    uint64_t available = 0;
    if (!tilefinch_update_query_free_space(
            manager->library->directory, &available)) {
        download_set_state(manager, OFFLINE_ITEM_PAUSED,
                           "free Memory Stick space is unavailable");
        download_reset_active(manager);
        return false;
    }
    uint64_t remaining = required >= retained ? required - retained : required;
    if (available < OFFLINE_DOWNLOAD_FREE_SPACE_RESERVE
        || remaining > available - OFFLINE_DOWNLOAD_FREE_SPACE_RESERVE) {
        download_set_state(manager, OFFLINE_ITEM_PAUSED,
                           "not enough free Memory Stick space");
        download_reset_active(manager);
        return false;
    }
    if (!offline_library_apply_youtube_stream(
            manager->library, manager->active_id, stream)) {
        download_set_state(manager, OFFLINE_ITEM_PAUSED,
                           "download metadata could not be saved");
        download_reset_active(manager);
        return false;
    }
    item = offline_library_find_mutable(manager->library, manager->active_id);
    if (final_video == stream->content_length
        && (!stream->split_streams
            || final_audio == stream->audio_content_length)) {
        item->downloaded_bytes = required;
        item->state = OFFLINE_ITEM_READY;
        (void) offline_library_save(manager->library);
        download_reset_active(manager);
        return true;
    }
    if (final_video == stream->content_length && stream->split_streams) {
        if (!download_begin_part(
                manager, stream->audio_url, stream->audio_content_length,
                ".audio.part")) {
            download_set_state(manager, OFFLINE_ITEM_PAUSED,
                               "audio download file could not be opened");
            download_reset_active(manager);
            return false;
        }
        manager->phase = OFFLINE_DOWNLOAD_AUDIO;
        return true;
    }
    if (!download_begin_part(
            manager, stream->media_url, stream->content_length,
            ".video.part")) {
        download_set_state(manager, OFFLINE_ITEM_PAUSED,
                           "video download file could not be opened");
        download_reset_active(manager);
        return false;
    }
    manager->phase = OFFLINE_DOWNLOAD_VIDEO;
    return true;
}

static bool download_enqueue_chunk(OfflineDownloadManager *manager)
{
    if (manager == NULL || manager->request_id != 0
        || manager->output == NULL || manager->part_offset >= manager->part_total)
        return false;
    uint64_t remaining = manager->part_total - manager->part_offset;
    size_t wanted = remaining < OFFLINE_DOWNLOAD_CHUNK_BYTES
        ? (size_t) remaining : OFFLINE_DOWNLOAD_CHUNK_BYTES;
    const char *media_url = manager->phase == OFFLINE_DOWNLOAD_AUDIO
        ? manager->stream.audio_url : manager->stream.media_url;
    if (!media_http_build_range_url(
            media_url, manager->part_offset,
            manager->part_offset + wanted - 1u,
            manager->request_url, sizeof(manager->request_url)))
        return false;
    FetchRequest request = {
        .method = "GET",
        .allow_http_errors = true,
        .accept = "video/mp4,video/*;q=0.9,*/*;q=0.5",
        .sec_fetch_dest = "video",
        .sec_fetch_mode = "no-cors",
        .sec_fetch_site = "cross-site",
        .user_agent = OFFLINE_DOWNLOAD_USER_AGENT,
        .credentials = FETCH_CREDENTIALS_OMIT,
        .redirect_url_validator = youtube_media_url_supported
    };
    /* The request object is deep-copied by the scheduler. Use the selected
       item's watch URL as the referrer rather than any array position. */
    OfflineLibraryItem *item = offline_library_find_mutable(
        manager->library, manager->active_id);
    request.referer = item == NULL ? NULL : item->source_url;
    request.initiator_url = request.referer;
    request.referrer_source = request.referer;
    manager->request_first = manager->part_offset;
    manager->request_length = wanted;
    manager->request_written = 0;
    manager->headers_valid = false;
    FetchStreamOptions stream = {
        .on_headers = download_headers,
        .on_body = download_body,
        .opaque = manager,
        .chunk_bytes = 16u * 1024u
    };
    manager->request_id = fetch_scheduler_enqueue_stream(
        manager->scheduler, manager->request_url, &request,
        wanted, 30000, &stream);
    return manager->request_id != 0;
}

static bool download_publish_part(
    OfflineDownloadManager *manager, const char *part_suffix,
    const char *final_suffix)
{
    char part[OFFLINE_LIBRARY_DIRECTORY_LIMIT + 40u];
    char final[OFFLINE_LIBRARY_DIRECTORY_LIMIT + 40u];
    if (!offline_library_item_path(
            manager->library, manager->active_id, part_suffix,
            part, sizeof(part))
        || !offline_library_item_path(
            manager->library, manager->active_id, final_suffix,
            final, sizeof(final))) return false;
    if (rename(part, final) == 0) return true;
    return remove(final) == 0 && rename(part, final) == 0;
}

static bool download_advance_part(OfflineDownloadManager *manager)
{
    download_close_output(manager);
    if (manager->phase == OFFLINE_DOWNLOAD_VIDEO) {
        if (!download_publish_part(
                manager, ".video.part", ".video.mp4")) {
            download_set_state(manager, OFFLINE_ITEM_PAUSED,
                               "video file could not be published");
            download_reset_active(manager);
            return false;
        }
        if (manager->stream.split_streams) {
            if (!download_begin_part(
                    manager, manager->stream.audio_url,
                    manager->stream.audio_content_length,
                    ".audio.part")) {
                download_set_state(manager, OFFLINE_ITEM_PAUSED,
                                   "audio download file could not be opened");
                download_reset_active(manager);
                return false;
            }
            manager->phase = OFFLINE_DOWNLOAD_AUDIO;
            return true;
        }
    } else if (manager->phase == OFFLINE_DOWNLOAD_AUDIO
               && !download_publish_part(
                      manager, ".audio.part", ".audio.mp4")) {
        download_set_state(manager, OFFLINE_ITEM_PAUSED,
                           "audio file could not be published");
        download_reset_active(manager);
        return false;
    }
    OfflineLibraryItem *item = offline_library_find_mutable(
        manager->library, manager->active_id);
    if (item != NULL) {
        item->downloaded_bytes = item->content_bytes + item->audio_bytes;
        item->state = OFFLINE_ITEM_READY;
        if (!offline_library_save(manager->library)) {
            item->state = OFFLINE_ITEM_PAUSED;
            snprintf(manager->error, sizeof(manager->error),
                     "completed download metadata could not be saved");
        }
    }
    download_reset_active(manager);
    return true;
}

bool offline_download_manager_pump(OfflineDownloadManager *manager)
{
    if (manager == NULL || manager->active_id == 0) return false;
    if (manager->phase == OFFLINE_DOWNLOAD_RESOLVE)
        return download_resolve(manager);
    if (manager->request_id == 0) {
        if (manager->part_offset >= manager->part_total)
            return download_advance_part(manager);
        if (!download_enqueue_chunk(manager)) {
            download_set_state(manager, OFFLINE_ITEM_PAUSED,
                               "download request could not start");
            download_reset_active(manager);
            return false;
        }
        return true;
    }
    FetchPumpQuota quota = {
        .maximum_body_callbacks = 2,
        .maximum_body_bytes = 32u * 1024u,
        .maximum_time_us = 2000
    };
    (void) fetch_scheduler_pump_bounded(
        manager->scheduler, 1, 0, &quota, NULL);
    bool success = false;
    FetchStreamMetrics metrics = {0};
    if (manager->completion == NULL) {
        download_set_state(manager, OFFLINE_ITEM_PAUSED,
                           "download completion buffer is unavailable");
        download_reset_active(manager);
        return false;
    }
    FetchResult *result = manager->completion;
    if (!fetch_scheduler_take_stream(
            manager->scheduler, manager->request_id,
            &success, &metrics, result)) return false;
    manager->request_id = 0;
    bool exact = success && manager->headers_valid
        && manager->request_written == manager->request_length
        && metrics.bytes_received == manager->request_length
        && fflush(manager->output) == 0;
    if (!exact) {
        snprintf(manager->error, sizeof(manager->error), "%s",
                 result->error[0] == '\0'
                     ? "download chunk was incomplete" : result->error);
        fetch_result_destroy(result);
        result->budget = manager->budget;
        download_set_state(manager, OFFLINE_ITEM_PAUSED, manager->error);
        download_reset_active(manager);
        return false;
    }
    fetch_result_destroy(result);
    result->budget = manager->budget;
    manager->part_offset += manager->request_length;
    OfflineLibraryItem *item = offline_library_find_mutable(
        manager->library, manager->active_id);
    if (item != NULL) {
        uint64_t video_done = manager->phase == OFFLINE_DOWNLOAD_AUDIO
            ? item->content_bytes : 0;
        item->downloaded_bytes = video_done + manager->part_offset;
    }
    return true;
}

bool offline_download_manager_pause(
    OfflineDownloadManager *manager, uint32_t id)
{
    if (manager == NULL || id == 0) return false;
    OfflineLibraryItem *item = offline_library_find_mutable(
        manager->library, id);
    if (item == NULL || item->type != OFFLINE_ITEM_YOUTUBE
        || item->state == OFFLINE_ITEM_READY) return false;
    if (manager->active_id == id) {
        download_cancel_request(manager);
        download_close_output(manager);
        if (manager->completion != NULL) {
            fetch_result_destroy(manager->completion);
            budget_free(manager->budget, manager->completion);
            manager->completion = NULL;
        }
        manager->phase = OFFLINE_DOWNLOAD_IDLE;
        manager->active_id = 0;
    }
    item->state = OFFLINE_ITEM_PAUSED;
    return offline_library_save(manager->library);
}

bool offline_download_manager_active(
    const OfflineDownloadManager *manager, uint32_t *id)
{
    if (manager == NULL || manager->active_id == 0) return false;
    if (id != NULL) *id = manager->active_id;
    return true;
}

void offline_download_manager_destroy(OfflineDownloadManager *manager)
{
    if (manager == NULL) return;
    if (manager->active_id != 0)
        (void) offline_download_manager_pause(
            manager, manager->active_id);
    if (manager->completion != NULL) {
        fetch_result_destroy(manager->completion);
        budget_free(manager->budget, manager->completion);
        manager->completion = NULL;
    }
    if (manager->scheduler != NULL)
        fetch_scheduler_destroy(manager->scheduler);
    manager->scheduler = NULL;
    manager->active_id = 0;
}
