#include "tilefinch/offline_library.h"

#include <errno.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "tilefinch/platform.h"
#include "tilefinch/update.h"

#define OFFLINE_INDEX_VERSION 5u
#define OFFLINE_INDEX_MINIMUM_VERSION 1u
#define OFFLINE_INDEX_HEADER_BYTES 20u
#define OFFLINE_ARTICLE_NODE_LIMIT 32768u
#define OFFLINE_ARTICLE_FREE_SPACE_RESERVE (256u * 1024u)
#define OFFLINE_ORPHAN_SCAN_LIMIT 128u

static void offline_error(
    char *error, size_t error_size, const char *format, ...)
{
    if (error == NULL || error_size == 0) return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

static uint64_t offline_now_unix(void)
{
    time_t now = time(NULL);
    return now > 0 ? (uint64_t) now : 0;
}

static uint32_t offline_hash(const unsigned char *data, size_t length)
{
    uint32_t hash = UINT32_C(2166136261);
    for (size_t index = 0; index < length; index++) {
        hash ^= data[index];
        hash *= UINT32_C(16777619);
    }
    return hash;
}

static bool put_u32(
    unsigned char *data, size_t capacity, size_t *used, uint32_t value)
{
    if (data == NULL || used == NULL || *used > capacity
        || capacity - *used < 4u) return false;
    for (unsigned byte = 0; byte < 4u; byte++)
        data[*used + byte] = (unsigned char) (value >> (byte * 8u));
    *used += 4u;
    return true;
}

static bool put_u64(
    unsigned char *data, size_t capacity, size_t *used, uint64_t value)
{
    return put_u32(data, capacity, used, (uint32_t) value)
        && put_u32(data, capacity, used, (uint32_t) (value >> 32));
}

static bool get_u32(
    const unsigned char *data, size_t length, size_t *used, uint32_t *value)
{
    if (data == NULL || used == NULL || value == NULL || *used > length
        || length - *used < 4u) return false;
    *value = (uint32_t) data[*used]
        | (uint32_t) data[*used + 1u] << 8
        | (uint32_t) data[*used + 2u] << 16
        | (uint32_t) data[*used + 3u] << 24;
    *used += 4u;
    return true;
}

static bool get_u64(
    const unsigned char *data, size_t length, size_t *used, uint64_t *value)
{
    uint32_t low = 0, high = 0;
    if (!get_u32(data, length, used, &low)
        || !get_u32(data, length, used, &high)) return false;
    *value = (uint64_t) low | (uint64_t) high << 32;
    return true;
}

static bool put_text(
    unsigned char *data, size_t capacity, size_t *used,
    const char *text, size_t limit)
{
    size_t length = text == NULL ? 0 : strnlen(text, limit);
    if (length >= limit || length > UINT32_MAX
        || !put_u32(data, capacity, used, (uint32_t) length)
        || *used > capacity || length > capacity - *used) return false;
    memcpy(data + *used, text, length);
    *used += length;
    return true;
}

static bool get_text(
    const unsigned char *data, size_t length, size_t *used,
    char *output, size_t capacity)
{
    uint32_t text_length = 0;
    if (!get_u32(data, length, used, &text_length)
        || text_length >= capacity || *used > length
        || text_length > length - *used) return false;
    memcpy(output, data + *used, text_length);
    output[text_length] = '\0';
    *used += text_length;
    return true;
}

static bool offline_directory_ready(const OfflineLibrary *library)
{
    if (library == NULL || library->directory[0] == '\0') return false;
    if (mkdir(library->directory, 0777) == 0) return true;
    return errno == EEXIST;
}

static uint64_t offline_existing_file_size(
    const OfflineLibrary *library, uint32_t id, const char *suffix)
{
    char path[OFFLINE_LIBRARY_DIRECTORY_LIMIT + 40u];
    struct stat status;
    return offline_library_item_path(
               library, id, suffix, path, sizeof(path))
            && stat(path, &status) == 0 && status.st_size >= 0
        ? (uint64_t) status.st_size : 0;
}

bool offline_library_item_path(
    const OfflineLibrary *library, uint32_t id, const char *suffix,
    char *output, size_t output_size)
{
    if (library == NULL || id == 0 || suffix == NULL || output == NULL
        || output_size == 0) return false;
    int written = snprintf(
        output, output_size, "%s/%08x%s",
        library->directory, (unsigned) id, suffix);
    return written > 0 && (size_t) written < output_size;
}

static bool offline_index_path(
    const OfflineLibrary *library, const char *suffix,
    char *output, size_t output_size)
{
    int written = library == NULL || suffix == NULL ? -1 : snprintf(
        output, output_size, "%s/library.bin%s",
        library->directory, suffix);
    return written > 0 && (size_t) written < output_size;
}

void offline_library_init(
    OfflineLibrary *library, Budget *budget, const char *directory)
{
    if (library == NULL) return;
    memset(library, 0, sizeof(*library));
    library->budget = budget;
    if (directory != NULL)
        snprintf(library->directory, sizeof(library->directory), "%s",
                 directory);
    library->next_id = 1;
}

static bool offline_item_valid(const OfflineLibraryItem *item)
{
    return item != NULL && item->id != 0
        && (item->type == OFFLINE_ITEM_ARTICLE
            || item->type == OFFLINE_ITEM_YOUTUBE
            || item->type == OFFLINE_ITEM_WEB_APP)
        && item->state >= OFFLINE_ITEM_QUEUED
        && item->state <= OFFLINE_ITEM_FAILED
        && item->content_bytes <= UINT64_MAX - item->audio_bytes
        && item->content_bytes + item->audio_bytes
               <= UINT64_MAX - item->icon_bytes
        && item->downloaded_bytes
               <= item->content_bytes + item->audio_bytes + item->icon_bytes
        && item->title[0] != '\0' && item->source_url[0] != '\0'
        && strnlen(item->title, sizeof(item->title)) < sizeof(item->title)
        && strnlen(item->source_url, sizeof(item->source_url))
               < sizeof(item->source_url)
        && (item->type != OFFLINE_ITEM_YOUTUBE
            || (item->video_id[0] != '\0'
                && strnlen(item->video_id, sizeof(item->video_id))
                       < sizeof(item->video_id)))
        && (item->type != OFFLINE_ITEM_WEB_APP
            || (item->state == OFFLINE_ITEM_READY
                && item->content_bytes <= OFFLINE_LIBRARY_APP_DOCUMENT_LIMIT
                && item->audio_bytes <= OFFLINE_LIBRARY_APP_PACK_LIMIT
                && item->icon_bytes <= OFFLINE_LIBRARY_APP_ICON_LIMIT
                && item->resource_count
                       <= BROWSER_OFFLINE_CACHE_ENTRY_LIMIT
                && item->app_display_mode
                       < TILEFINCH_WEB_APP_DISPLAY_COUNT));
}

static bool encode_item(
    unsigned char *data, size_t capacity, size_t *used,
    const OfflineLibraryItem *item)
{
    return offline_item_valid(item)
        && put_u32(data, capacity, used, item->id)
        && put_u32(data, capacity, used, (uint32_t) item->type)
        && put_u32(data, capacity, used, (uint32_t) item->state)
        && put_u64(data, capacity, used, item->content_bytes)
        && put_u64(data, capacity, used, item->audio_bytes)
        && put_u64(data, capacity, used, item->downloaded_bytes)
        && put_u64(data, capacity, used, item->duration_ms)
        && put_u64(data, capacity, used, item->saved_at_unix)
        && put_u32(data, capacity, used, item->article_hash)
        && put_u32(data, capacity, used, item->auxiliary_hash)
        && put_u32(data, capacity, used, item->icon_hash)
        && put_u32(data, capacity, used, item->resource_count)
        && put_u32(data, capacity, used, item->icon_bytes)
        && put_u32(data, capacity, used, (uint32_t) item->width)
        && put_u32(data, capacity, used, (uint32_t) item->height)
        && put_u32(data, capacity, used, (uint32_t) item->itag)
        && put_u32(data, capacity, used, (uint32_t) item->audio_itag)
        && put_u32(data, capacity, used, item->split_streams ? 1u : 0u)
        && put_u32(data, capacity, used, item->app_theme_color)
        && put_u32(data, capacity, used, item->app_theme_alpha)
        && put_u32(data, capacity, used, item->app_display_mode)
        && put_u32(data, capacity, used,
                   item->app_theme_color_valid ? 1u : 0u)
        && put_text(data, capacity, used, item->title, sizeof(item->title))
        && put_text(
               data, capacity, used, item->source_url,
               sizeof(item->source_url))
        && put_text(
               data, capacity, used, item->video_id,
               sizeof(item->video_id))
        && put_text(
               data, capacity, used, item->failure_reason,
               sizeof(item->failure_reason));
}

static bool decode_item(
    const unsigned char *data, size_t length, size_t *used,
    uint32_t version, OfflineLibraryItem *item)
{
    uint32_t type = 0, state = 0, width = 0, height = 0;
    uint32_t itag = 0, audio_itag = 0, split = 0;
    uint32_t app_theme_color = 0, app_theme_alpha = 0;
    uint32_t app_display_mode = 0, app_theme_color_valid = 0;
    OfflineLibraryItem staged = {0};
    if (!get_u32(data, length, used, &staged.id)
        || !get_u32(data, length, used, &type)
        || !get_u32(data, length, used, &state)
        || !get_u64(data, length, used, &staged.content_bytes)
        || !get_u64(data, length, used, &staged.audio_bytes)
        || !get_u64(data, length, used, &staged.downloaded_bytes)
        || !get_u64(data, length, used, &staged.duration_ms)
        || (version >= 2u
            && !get_u64(data, length, used, &staged.saved_at_unix))
        || !get_u32(data, length, used, &staged.article_hash)
        || (version >= 4u
            && (!get_u32(data, length, used, &staged.auxiliary_hash)
                || !get_u32(data, length, used, &staged.icon_hash)
                || !get_u32(data, length, used, &staged.resource_count)
                || !get_u32(data, length, used, &staged.icon_bytes)))
        || !get_u32(data, length, used, &width)
        || !get_u32(data, length, used, &height)
        || !get_u32(data, length, used, &itag)
        || !get_u32(data, length, used, &audio_itag)
        || !get_u32(data, length, used, &split)
        || (version >= 5u
            && (!get_u32(data, length, used, &app_theme_color)
                || !get_u32(data, length, used, &app_theme_alpha)
                || !get_u32(data, length, used, &app_display_mode)
                || !get_u32(data, length, used,
                            &app_theme_color_valid)))
        || !get_text(
               data, length, used, staged.title, sizeof(staged.title))
        || !get_text(
               data, length, used, staged.source_url,
               sizeof(staged.source_url))
        || !get_text(
               data, length, used, staged.video_id,
               sizeof(staged.video_id))
        || (version >= 3u
            && !get_text(
                   data, length, used, staged.failure_reason,
                   sizeof(staged.failure_reason)))) return false;
    staged.type = (OfflineItemType) type;
    staged.state = (OfflineItemState) state;
    staged.width = (int) (int32_t) width;
    staged.height = (int) (int32_t) height;
    staged.itag = (int) (int32_t) itag;
    staged.audio_itag = (int) (int32_t) audio_itag;
    staged.split_streams = split == 1u;
    staged.app_theme_color = app_theme_color;
    staged.app_theme_alpha = (uint8_t) app_theme_alpha;
    staged.app_display_mode = (uint8_t) app_display_mode;
    staged.app_theme_color_valid = app_theme_color_valid == 1u;
    if (split > 1u || app_theme_alpha > UINT8_MAX
        || app_display_mode >= TILEFINCH_WEB_APP_DISPLAY_COUNT
        || app_theme_color_valid > 1u || !offline_item_valid(&staged))
        return false;
    *item = staged;
    return true;
}

static bool offline_library_decode_index(
    const OfflineLibrary *library, const char *suffix,
    OfflineLibrary *decoded, bool *exists);
static bool offline_library_decode_index_with_buffer(
    const OfflineLibrary *library, const char *suffix,
    OfflineLibrary *decoded, bool *exists, unsigned char *data);

static bool offline_library_save_with_buffer(
    const OfflineLibrary *library, unsigned char *data)
{
    static const unsigned char magic[8] = {
        'T', 'F', 'O', 'F', 'F', '0', '1', 0
    };
    if (library == NULL || !library->loaded
        || library->count > OFFLINE_LIBRARY_ITEM_LIMIT
        || !offline_directory_ready(library)) return false;
    size_t used = OFFLINE_INDEX_HEADER_BYTES;
    for (size_t index = 0; index < library->count; index++)
        if (!encode_item(
                data, OFFLINE_LIBRARY_INDEX_LIMIT, &used,
                &library->items[index]))
            return false;
    memcpy(data, magic, sizeof(magic));
    size_t header_used = sizeof(magic);
    if (!put_u32(data, OFFLINE_INDEX_HEADER_BYTES, &header_used,
                 OFFLINE_INDEX_VERSION)
        || !put_u32(data, OFFLINE_INDEX_HEADER_BYTES, &header_used,
                    (uint32_t) library->count)
        || !put_u32(data, OFFLINE_INDEX_HEADER_BYTES, &header_used,
                    offline_hash(
                        data + OFFLINE_INDEX_HEADER_BYTES,
                        used - OFFLINE_INDEX_HEADER_BYTES))) return false;
    char path[OFFLINE_LIBRARY_DIRECTORY_LIMIT + 32u];
    char temporary[OFFLINE_LIBRARY_DIRECTORY_LIMIT + 32u];
    char backup[OFFLINE_LIBRARY_DIRECTORY_LIMIT + 32u];
    if (!offline_index_path(library, "", path, sizeof(path))
        || !offline_index_path(library, ".tmp", temporary,
                               sizeof(temporary))
        || !offline_index_path(library, ".bak", backup,
                               sizeof(backup))) return false;
    FILE *file = fopen(temporary, "wb");
    if (file == NULL) return false;
    bool okay = fwrite(data, 1, used, file) == used
        && fflush(file) == 0 && ferror(file) == 0;
    if (fclose(file) != 0) okay = false;
    if (!okay) {
        remove(temporary);
        return false;
    }
    bool primary_exists = false;
    bool primary_valid = offline_library_decode_index_with_buffer(
        library, "", NULL, &primary_exists, data);
    if (!primary_valid) {
        /* Recovery may have loaded .tmp or .bak because the primary is
           absent/corrupt. Preserve those known-good generations while
           publishing the normalized primary. If promotion fails, leave the
           complete .tmp for the next bounded recovery pass. */
        if (primary_exists && remove(path) != 0) return false;
        if (rename(temporary, path) != 0) return false;
        return true;
    }
    /* FAT does not replace an existing destination. Keep the previous
       complete index as a recovery generation instead of creating an
       unprotected remove-then-rename window. */
    if (remove(backup) != 0 && errno != ENOENT) {
        remove(temporary);
        return false;
    }
    bool backed_up = rename(path, backup) == 0;
    if (!backed_up && errno != ENOENT) {
        remove(temporary);
        return false;
    }
    if (rename(temporary, path) != 0) {
        if (backed_up) (void) rename(backup, path);
        remove(temporary);
        return false;
    }
    return true;
}

bool offline_library_save(const OfflineLibrary *library)
{
    if (library == NULL || library->budget == NULL) return false;
    unsigned char *data = budget_malloc_category(
        library->budget, BUDGET_CATEGORY_SESSION,
        OFFLINE_LIBRARY_INDEX_LIMIT);
    if (data == NULL) return false;
    bool saved = offline_library_save_with_buffer(library, data);
    budget_free(library->budget, data);
    return saved;
}

static bool offline_library_decode_index_with_buffer(
    const OfflineLibrary *library, const char *suffix,
    OfflineLibrary *decoded, bool *exists, unsigned char *data)
{
    static const unsigned char magic[8] = {
        'T', 'F', 'O', 'F', 'F', '0', '1', 0
    };
    if (exists != NULL) *exists = false;
    if (library == NULL || suffix == NULL
        || library->directory[0] == '\0') return false;
    char path[OFFLINE_LIBRARY_DIRECTORY_LIMIT + 32u];
    if (!offline_index_path(library, suffix, path, sizeof(path))) return false;
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        if (exists != NULL && errno != ENOENT) *exists = true;
        return false;
    }
    if (exists != NULL) *exists = true;
    size_t length = fread(data, 1, OFFLINE_LIBRARY_INDEX_LIMIT, file);
    bool okay = !ferror(file) && fgetc(file) == EOF;
    if (fclose(file) != 0) okay = false;
    if (!okay || length < OFFLINE_INDEX_HEADER_BYTES
        || memcmp(data, magic, sizeof(magic)) != 0) return false;
    size_t used = sizeof(magic);
    uint32_t version = 0, count = 0, checksum = 0;
    if (!get_u32(data, OFFLINE_INDEX_HEADER_BYTES, &used, &version)
        || !get_u32(data, OFFLINE_INDEX_HEADER_BYTES, &used, &count)
        || !get_u32(data, OFFLINE_INDEX_HEADER_BYTES, &used, &checksum)
        || version < OFFLINE_INDEX_MINIMUM_VERSION
        || version > OFFLINE_INDEX_VERSION
        || count > OFFLINE_LIBRARY_ITEM_LIMIT
        || checksum != offline_hash(
               data + OFFLINE_INDEX_HEADER_BYTES,
               length - OFFLINE_INDEX_HEADER_BYTES)) return false;
    if (decoded != NULL) {
        *decoded = *library;
        decoded->loaded = true;
        decoded->count = 0;
        decoded->next_id = 1;
    }
    uint32_t seen_ids[OFFLINE_LIBRARY_ITEM_LIMIT] = {0};
    uint32_t next_id = 1;
    used = OFFLINE_INDEX_HEADER_BYTES;
    for (size_t index = 0; index < count; index++) {
        OfflineLibraryItem item = {0};
        if (!decode_item(data, length, &used, version, &item)) return false;
        for (size_t prior = 0; prior < index; prior++)
            if (seen_ids[prior] == item.id) return false;
        seen_ids[index] = item.id;
        if (item.id >= next_id) {
            next_id = item.id == UINT32_MAX ? 1u : item.id + 1u;
        }
        if (decoded != NULL) {
            decoded->items[index] = item;
            decoded->count++;
            decoded->next_id = next_id;
        }
    }
    if (used != length) return false;
    return true;
}

static bool offline_library_decode_index(
    const OfflineLibrary *library, const char *suffix,
    OfflineLibrary *decoded, bool *exists)
{
    if (library == NULL || library->budget == NULL) return false;
    unsigned char *data = budget_malloc_category(
        library->budget, BUDGET_CATEGORY_SESSION,
        OFFLINE_LIBRARY_INDEX_LIMIT);
    if (data == NULL) return false;
    bool decoded_ok = offline_library_decode_index_with_buffer(
        library, suffix, decoded, exists, data);
    budget_free(library->budget, data);
    return decoded_ok;
}

static bool offline_library_load_with_staged(
    OfflineLibrary *library, OfflineLibrary *staged)
{
    if (library == NULL || library->directory[0] == '\0') return false;
    if (library->loaded) return true;
    static const char *candidates[] = {"", ".tmp", ".bak"};
    memset(staged, 0, sizeof(*staged));
    bool any_index = false;
    bool recovered = false;
    for (size_t at = 0; at < sizeof(candidates) / sizeof(candidates[0]); at++) {
        bool exists = false;
        if (offline_library_decode_index(
                library, candidates[at], staged, &exists)) {
            recovered = at != 0;
            break;
        }
        any_index = any_index || exists;
    }
    if (!staged->loaded) {
        if (any_index) return false;
        *staged = *library;
        staged->loaded = true;
        staged->count = 0;
        staged->next_id = 1;
    }
    bool normalized = recovered;
    for (size_t index = 0; index < staged->count; index++) {
        OfflineLibraryItem *item = &staged->items[index];
        if (item->type != OFFLINE_ITEM_YOUTUBE) continue;
        if (item->state == OFFLINE_ITEM_DOWNLOADING) {
            item->state = OFFLINE_ITEM_PAUSED;
            normalized = true;
        }
        uint64_t video_part = offline_existing_file_size(
            staged, item->id, ".video.part");
        uint64_t audio_part = offline_existing_file_size(
            staged, item->id, ".audio.part");
        uint64_t video_final = offline_existing_file_size(
            staged, item->id, ".video.mp4");
        uint64_t audio_final = offline_existing_file_size(
            staged, item->id, ".audio.mp4");
        bool complete = video_final == item->content_bytes
            && (!item->split_streams
                || audio_final == item->audio_bytes);
        if (item->state == OFFLINE_ITEM_READY && !complete) {
            item->state = OFFLINE_ITEM_FAILED;
            normalized = true;
        }
        if (item->state != OFFLINE_ITEM_READY) {
            uint64_t recovered = video_part <= item->content_bytes
                    && audio_part <= item->audio_bytes
                    && (video_final == 0
                        || video_final == item->content_bytes)
                    && (audio_final == 0
                        || audio_final == item->audio_bytes)
                ? video_part + audio_part + video_final + audio_final : 0;
            uint64_t total = item->content_bytes + item->audio_bytes;
            if (recovered > total) recovered = 0;
            if (recovered != item->downloaded_bytes) {
                item->downloaded_bytes = recovered;
                normalized = true;
            }
        }
    }
    if (normalized && !offline_library_save(staged)) return false;
    *library = *staged;
    /* Files are named only by a fixed-width library id. A bounded lazy scan
       reclaims files left behind if power failed after file publication but
       before index promotion; unrelated files are never touched. */
    DIR *directory = opendir(library->directory);
    if (directory != NULL) {
        size_t scanned = 0;
        struct dirent *entry = NULL;
        while (scanned++ < OFFLINE_ORPHAN_SCAN_LIMIT
               && (entry = readdir(directory)) != NULL) {
            unsigned id = 0;
            char suffix[24] = {0};
            char tail = '\0';
            if (sscanf(entry->d_name, "%8x%23s%c", &id, suffix, &tail) != 2
                || id == 0 || offline_library_find(library, id) != NULL)
                continue;
            static const char *owned[] = {
                ".article.html", ".article.tmp", ".article.bak",
                ".app.html", ".app.pack", ".app.icon",
                ".app-html.tmp", ".app-pack.tmp", ".app-icon.tmp",
                ".video.part", ".video.mp4", ".audio.part", ".audio.mp4"
            };
            bool recognized = false;
            for (size_t at = 0; at < sizeof(owned) / sizeof(owned[0]); at++)
                if (strcmp(suffix, owned[at]) == 0) recognized = true;
            if (!recognized) continue;
            char path[OFFLINE_LIBRARY_DIRECTORY_LIMIT + 40u];
            int written = snprintf(
                path, sizeof(path), "%s/%s", library->directory,
                entry->d_name);
            if (written > 0 && (size_t) written < sizeof(path))
                (void) remove(path);
        }
        (void) closedir(directory);
    }
    return true;
}

bool offline_library_load(OfflineLibrary *library)
{
    if (library == NULL || library->budget == NULL) return false;
    if (library->loaded) return true;
    OfflineLibrary *staged = budget_calloc_category(
        library->budget, BUDGET_CATEGORY_SESSION, 1, sizeof(*staged));
    if (staged == NULL) return false;
    bool loaded = offline_library_load_with_staged(library, staged);
    budget_free(library->budget, staged);
    return loaded;
}

const OfflineLibraryItem *offline_library_find(
    const OfflineLibrary *library, uint32_t id)
{
    if (library == NULL || !library->loaded || id == 0) return NULL;
    for (size_t index = 0; index < library->count; index++)
        if (library->items[index].id == id) return &library->items[index];
    return NULL;
}

OfflineLibraryItem *offline_library_find_mutable(
    OfflineLibrary *library, uint32_t id)
{
    return (OfflineLibraryItem *) offline_library_find(library, id);
}

static bool write_output(FILE *file, const unsigned char *data, size_t length,
                         uint32_t *hash, size_t *bytes)
{
    if (file == NULL || (data == NULL && length != 0)
        || hash == NULL || bytes == NULL
        || *bytes > OFFLINE_LIBRARY_ARTICLE_LIMIT
        || length > OFFLINE_LIBRARY_ARTICLE_LIMIT - *bytes) return false;
    size_t written = 0;
    while (written < length) {
        size_t chunk = length - written;
        if (chunk > 16u * 1024u) chunk = 16u * 1024u;
        if (fwrite(data + written, 1, chunk, file) != chunk) return false;
        for (size_t at = 0; at < chunk; at++) {
            *hash ^= data[written + at];
            *hash *= UINT32_C(16777619);
        }
        written += chunk;
        *bytes += chunk;
        if (written < length
            && !tilefinch_platform_cooperate(
                   "offline-article-write", *bytes)) return false;
    }
    return true;
}

static bool write_escaped(FILE *file, const char *text, bool attribute,
                          uint32_t *hash, size_t *bytes)
{
    if (file == NULL || text == NULL || hash == NULL || bytes == NULL)
        return false;
    const unsigned char *at = (const unsigned char *) text;
    const unsigned char *span = at;
    for (; *at != 0; at++) {
        const char *replacement = NULL;
        if (*at == '&') replacement = "&amp;";
        else if (*at == '<') replacement = "&lt;";
        else if (*at == '>') replacement = "&gt;";
        else if (attribute && *at == '"') replacement = "&quot;";
        if (replacement == NULL) continue;
        if (!write_output(file, span, (size_t) (at - span), hash, bytes)
            || !write_output(
                file, (const unsigned char *) replacement,
                strlen(replacement), hash, bytes)) return false;
        span = at + 1u;
    }
    return write_output(file, span, (size_t) (at - span), hash, bytes);
}

static bool write_escaped_bytes(
    FILE *file, const unsigned char *text, size_t text_length,
    uint32_t *hash, size_t *bytes, bool *have_content)
{
    if (file == NULL || (text == NULL && text_length != 0)
        || hash == NULL || bytes == NULL) return false;
    size_t span = 0;
    for (size_t at = 0; at < text_length; at++) {
        const char *replacement = NULL;
        if (text[at] == '&') replacement = "&amp;";
        else if (text[at] == '<') replacement = "&lt;";
        else if (text[at] == '>') replacement = "&gt;";
        if (replacement != NULL) {
            if (!write_output(file, text + span, at - span, hash, bytes)
                || !write_output(
                    file, (const unsigned char *) replacement,
                    strlen(replacement), hash, bytes)) return false;
            span = at + 1u;
        }
        if (have_content != NULL
            && text[at] != ' ' && text[at] != '\t'
            && text[at] != '\r' && text[at] != '\n')
            *have_content = true;
    }
    return write_output(
        file, text_length == span ? NULL : text + span,
        text_length - span, hash, bytes);
}

static bool offline_node_name_is(lxb_dom_node_t *node, const char *wanted)
{
    size_t length = 0;
    const char *name = document_element_name(node, &length);
    size_t wanted_length = strlen(wanted);
    return name != NULL && length == wanted_length
        && memcmp(name, wanted, length) == 0;
}

static bool offline_reader_hidden(lxb_dom_node_t *node)
{
    static const char *names[] = {
        "script", "style", "noscript", "nav", "aside", "footer", "form"
    };
    size_t length = 0;
    const char *name = document_element_name(node, &length);
    if (name == NULL) return false;
    for (size_t at = 0; at < sizeof(names) / sizeof(names[0]); at++)
        if (strlen(names[at]) == length
            && memcmp(name, names[at], length) == 0) return true;
    return false;
}

static const char *offline_reader_open_tag(lxb_dom_node_t *node)
{
    if (offline_node_name_is(node, "p")) return "<p>";
    if (offline_node_name_is(node, "h1")
        || offline_node_name_is(node, "h2")) return "<h2>";
    if (offline_node_name_is(node, "h3")
        || offline_node_name_is(node, "h4")
        || offline_node_name_is(node, "h5")
        || offline_node_name_is(node, "h6")) return "<h3>";
    if (offline_node_name_is(node, "blockquote")) return "<blockquote>";
    if (offline_node_name_is(node, "pre")) return "<pre>";
    if (offline_node_name_is(node, "ul")) return "<ul>";
    if (offline_node_name_is(node, "ol")) return "<ol>";
    if (offline_node_name_is(node, "li")) return "<li>";
    if (offline_node_name_is(node, "br")) return "<br>";
    return NULL;
}

static const char *offline_reader_close_tag(lxb_dom_node_t *node)
{
    if (offline_node_name_is(node, "p")) return "</p>";
    if (offline_node_name_is(node, "h1")
        || offline_node_name_is(node, "h2")) return "</h2>";
    if (offline_node_name_is(node, "h3")
        || offline_node_name_is(node, "h4")
        || offline_node_name_is(node, "h5")
        || offline_node_name_is(node, "h6")) return "</h3>";
    if (offline_node_name_is(node, "blockquote")) return "</blockquote>";
    if (offline_node_name_is(node, "pre")) return "</pre>";
    if (offline_node_name_is(node, "ul")) return "</ul>";
    if (offline_node_name_is(node, "ol")) return "</ol>";
    if (offline_node_name_is(node, "li")) return "</li>";
    return NULL;
}

static lxb_dom_node_t *offline_reader_root(PocDocument *document)
{
    lxb_dom_node_t *body = document_body_node(document);
    if (body == NULL) return NULL;
    lxb_dom_node_t *fallback_main = NULL;
    lxb_dom_node_t *node = body;
    lxb_dom_node_t *boundary = body->parent;
    size_t visited = 0;
    while (node != NULL && node != boundary) {
        if (++visited > OFFLINE_ARTICLE_NODE_LIMIT) return NULL;
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            if (offline_node_name_is(node, "article")) return node;
            if (fallback_main == NULL && offline_node_name_is(node, "main"))
                fallback_main = node;
        }
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != NULL && node != boundary && node->next == NULL)
            node = node->parent;
        if (node != NULL && node != boundary) node = node->next;
    }
    return fallback_main == NULL ? body : fallback_main;
}

static bool write_reader_subtree(
    FILE *file, lxb_dom_node_t *root, uint32_t *hash, size_t *bytes,
    bool *have_content)
{
    if (file == NULL || root == NULL) return false;
    lxb_dom_node_t *boundary = root->parent;
    lxb_dom_node_t *node = root;
    size_t hidden_depth = 0;
    size_t visited = 0;
    while (node != NULL && node != boundary) {
        if (++visited > OFFLINE_ARTICLE_NODE_LIMIT
            || ((visited & 127u) == 0
                && !tilefinch_platform_cooperate(
                       "offline-article", visited))) return false;
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            if (offline_reader_hidden(node)) hidden_depth++;
            const char *opening = hidden_depth == 0
                ? offline_reader_open_tag(node) : NULL;
            if (opening != NULL
                && !write_output(
                    file, (const unsigned char *) opening,
                    strlen(opening), hash, bytes)) return false;
        } else if (node->type == LXB_DOM_NODE_TYPE_TEXT
                   && hidden_depth == 0) {
            size_t length = 0;
            const char *text = document_text_data(node, &length);
            if (text != NULL && length != 0
                && !write_escaped_bytes(
                       file, (const unsigned char *) text, length,
                       hash, bytes, have_content)) return false;
        }
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        for (;;) {
            if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                bool hidden = offline_reader_hidden(node);
                const char *closing = hidden_depth == 0
                    ? offline_reader_close_tag(node) : NULL;
                if (closing != NULL
                    && !write_output(
                        file, (const unsigned char *) closing,
                        strlen(closing), hash, bytes)) return false;
                if (hidden) hidden_depth--;
            }
            if (node->next != NULL) {
                node = node->next;
                break;
            }
            node = node->parent;
            if (node == NULL || node == boundary) return true;
        }
    }
    return true;
}

static bool write_hashed(
    FILE *file, const char *text, uint32_t *hash, size_t *bytes)
{
    size_t length = strlen(text);
    if (*bytes > OFFLINE_LIBRARY_ARTICLE_LIMIT
        || length > OFFLINE_LIBRARY_ARTICLE_LIMIT - *bytes
        || fwrite(text, 1, length, file) != length) return false;
    *bytes += length;
    for (size_t index = 0; index < length; index++) {
        *hash ^= (unsigned char) text[index];
        *hash *= UINT32_C(16777619);
    }
    return true;
}

static bool offline_article_file_matches(
    const char *path, uint64_t expected_bytes, uint32_t expected_hash)
{
    if (path == NULL || expected_bytes == 0
        || expected_bytes > OFFLINE_LIBRARY_ARTICLE_LIMIT) return false;
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    unsigned char chunk[4096];
    uint64_t remaining = expected_bytes;
    uint32_t hash = UINT32_C(2166136261);
    bool okay = true;
    while (remaining != 0) {
        size_t wanted = remaining < sizeof(chunk)
            ? (size_t) remaining : sizeof(chunk);
        size_t read = fread(chunk, 1, wanted, file);
        if (read != wanted) {
            okay = false;
            break;
        }
        for (size_t at = 0; at < read; at++) {
            hash ^= chunk[at];
            hash *= UINT32_C(16777619);
        }
        remaining -= read;
    }
    if (okay) okay = fgetc(file) == EOF && !ferror(file)
        && hash == expected_hash;
    if (fclose(file) != 0) okay = false;
    return okay;
}

bool offline_library_save_article(
    OfflineLibrary *library, PocDocument *document, const char *source_url,
    uint32_t *saved_id, char *error, size_t error_size)
{
    if (error != NULL && error_size != 0) error[0] = '\0';
    if (library == NULL || document == NULL || source_url == NULL
        || source_url[0] == '\0' || strlen(source_url) >= OFFLINE_LIBRARY_URL_LIMIT
        || !offline_library_load(library)) {
        offline_error(error, error_size, "offline article input is invalid");
        return false;
    }
    const char *title = document->title == NULL || document->title[0] == '\0'
        ? "Saved article" : document->title;
    lxb_dom_node_t *reader_root = offline_reader_root(document);
    if (reader_root == NULL) {
        offline_error(error, error_size, "article structure exceeds the save bound");
        return false;
    }
    size_t slot = library->count;
    for (size_t index = 0; index < library->count; index++) {
        if (library->items[index].type == OFFLINE_ITEM_ARTICLE
            && strcmp(library->items[index].source_url, source_url) == 0) {
            slot = index;
            break;
        }
    }
    if (slot == library->count
        && library->count >= OFFLINE_LIBRARY_ITEM_LIMIT) {
        offline_error(
            error, error_size,
            "offline library is full; delete its oldest saved item");
        return false;
    }
    uint32_t id = slot < library->count
        ? library->items[slot].id : library->next_id++;
    if (id == 0) id = library->next_id++;
    bool replacing = slot < library->count;
    OfflineLibraryItem previous = {0};
    if (replacing) previous = library->items[slot];
    if (!offline_directory_ready(library)) {
        offline_error(error, error_size, "offline directory is unavailable");
        return false;
    }
    uint64_t free_bytes = 0;
    if (!tilefinch_update_query_free_space(
            library->directory, &free_bytes)) {
        offline_error(error, error_size,
                      "free Memory Stick space is unavailable");
        return false;
    }
    if (free_bytes < OFFLINE_LIBRARY_ARTICLE_LIMIT
                         + OFFLINE_ARTICLE_FREE_SPACE_RESERVE) {
        offline_error(error, error_size,
                      "not enough free Memory Stick space");
        return false;
    }
    char path[OFFLINE_LIBRARY_DIRECTORY_LIMIT + 40u];
    char temporary[OFFLINE_LIBRARY_DIRECTORY_LIMIT + 40u];
    char backup[OFFLINE_LIBRARY_DIRECTORY_LIMIT + 40u];
    if (!offline_library_item_path(
            library, id, ".article.html", path, sizeof(path))
        || !offline_library_item_path(
            library, id, ".article.tmp", temporary, sizeof(temporary))
        || !offline_library_item_path(
            library, id, ".article.bak", backup, sizeof(backup)))
        return false;
    FILE *file = fopen(temporary, "wb");
    if (file == NULL) {
        offline_error(error, error_size, "article file could not be opened");
        return false;
    }
    uint32_t hash = UINT32_C(2166136261);
    size_t bytes = 0;
    bool have_content = false;
    bool okay = write_hashed(
            file, "<!doctype html><meta name=viewport content=\"width=device-width\">"
                  "<style>body{font:17px/1.55 sans-serif;max-width:42em;margin:auto;"
                  "padding:14px;background:#faf8f2;color:#202020}h1{font-size:24px;"
                  "line-height:1.2}h2{font-size:21px;margin:1.2em 0 .35em}"
                  "h3{font-size:18px;margin:1em 0 .3em}p{margin:.65em 0}"
                  "blockquote{border-left:3px solid #aaa;margin:1em 0;padding-left:12px}"
                  "a{color:#0645ad}pre{white-space:pre-wrap;font:inherit}"
                  "li{margin:.25em 0}</style><main><h1>", &hash, &bytes)
        && write_escaped(file, title, false, &hash, &bytes)
        && write_hashed(file, "</h1><p><a href=\"", &hash, &bytes)
        && write_escaped(file, source_url, true, &hash, &bytes)
        && write_hashed(file, "\">Original article</a></p>", &hash, &bytes)
        && write_reader_subtree(
               file, reader_root, &hash, &bytes, &have_content)
        && write_hashed(file, "</main>", &hash, &bytes)
        && fflush(file) == 0 && ferror(file) == 0;
    if (fclose(file) != 0) okay = false;
    if (!okay || !have_content) {
        remove(temporary);
        offline_error(error, error_size, "article exceeded storage or write bound");
        return false;
    }
    bool prior_primary_valid = replacing && offline_article_file_matches(
        path, previous.content_bytes, previous.article_hash);
    bool backed_up = false;
    if (prior_primary_valid) {
        if (remove(backup) != 0 && errno != ENOENT) {
            remove(temporary);
            offline_error(
                error, error_size, "article backup could not be staged");
            return false;
        }
        backed_up = rename(path, backup) == 0;
        if (!backed_up) {
            remove(temporary);
            offline_error(
                error, error_size, "existing article could not be staged");
            return false;
        }
    } else if (replacing && remove(path) != 0 && errno != ENOENT) {
        remove(temporary);
        offline_error(error, error_size, "invalid article could not be replaced");
        return false;
    }
    if (rename(temporary, path) != 0) {
        if (backed_up) (void) rename(backup, path);
        remove(temporary);
        offline_error(error, error_size, "article could not be published");
        return false;
    }
    OfflineLibraryItem replacement = {
        .id = id,
        .type = OFFLINE_ITEM_ARTICLE,
        .state = OFFLINE_ITEM_READY,
        .content_bytes = bytes,
        .article_hash = hash,
        .saved_at_unix = offline_now_unix()
    };
    snprintf(replacement.title, sizeof(replacement.title), "%s", title);
    snprintf(replacement.source_url, sizeof(replacement.source_url), "%s",
             source_url);
    library->items[slot] = replacement;
    if (slot == library->count) library->count++;
    if (!offline_library_save(library)) {
        if (replacing) library->items[slot] = previous;
        else library->count--;
        remove(path);
        if (backed_up) (void) rename(backup, path);
        offline_error(error, error_size, "offline index could not be saved");
        return false;
    }
    /* Preserve the prior article beside the prior index generation. */
    if (saved_id != NULL) *saved_id = id;
    return true;
}

bool offline_library_read_article(
    const OfflineLibrary *library, Budget *budget, uint32_t id,
    char **html, size_t *length, char *error, size_t error_size)
{
    if (html != NULL) *html = NULL;
    if (length != NULL) *length = 0;
    const OfflineLibraryItem *item = offline_library_find(library, id);
    if (item == NULL || item->type != OFFLINE_ITEM_ARTICLE
        || item->state != OFFLINE_ITEM_READY || budget == NULL
        || html == NULL || length == NULL
        || item->content_bytes == 0
        || item->content_bytes > OFFLINE_LIBRARY_ARTICLE_LIMIT
        || item->content_bytes > SIZE_MAX - 1u) {
        offline_error(error, error_size, "saved article is unavailable");
        return false;
    }
    char path[OFFLINE_LIBRARY_DIRECTORY_LIMIT + 40u];
    size_t size = (size_t) item->content_bytes;
    char *data = budget_malloc_category(
        budget, BUDGET_CATEGORY_SESSION, size + 1u);
    bool okay = false;
    static const char *suffixes[] = {".article.html", ".article.bak"};
    for (size_t at = 0; data != NULL
         && at < sizeof(suffixes) / sizeof(suffixes[0]); at++) {
        if (!offline_library_item_path(
                library, id, suffixes[at], path, sizeof(path))) continue;
        FILE *file = fopen(path, "rb");
        if (file == NULL) continue;
        okay = fread(data, 1, size, file) == size
            && fgetc(file) == EOF && !ferror(file);
        if (fclose(file) != 0) okay = false;
        if (okay) {
            data[size] = '\0';
            okay = offline_hash((const unsigned char *) data, size)
                == item->article_hash;
        }
        if (okay) break;
    }
    if (!okay) {
        budget_free(budget, data);
        offline_error(error, error_size, "saved article failed integrity checks");
        return false;
    }
    *html = data;
    *length = size;
    return true;
}

typedef struct {
    Budget *budget;
    unsigned char *data;
    size_t length;
    size_t capacity;
    size_t limit;
} OfflineBlob;

static bool offline_blob_reserve(OfflineBlob *blob, size_t extra)
{
    if (blob == NULL || extra > blob->limit - blob->length) return false;
    size_t needed = blob->length + extra;
    if (needed <= blob->capacity) return true;
    size_t capacity = blob->capacity == 0 ? 4096u : blob->capacity;
    while (capacity < needed) {
        size_t grown = capacity + capacity / 2u;
        if (grown <= capacity || grown > blob->limit) grown = blob->limit;
        capacity = grown;
        if (capacity < needed && capacity == blob->limit) return false;
    }
    unsigned char *resized = budget_realloc_category(
        blob->budget, BUDGET_CATEGORY_SESSION, blob->data, capacity);
    if (resized == NULL) return false;
    blob->data = resized;
    blob->capacity = capacity;
    return true;
}

static bool offline_blob_bytes(OfflineBlob *blob, const void *bytes,
                               size_t length)
{
    if ((bytes == NULL && length != 0) || !offline_blob_reserve(blob, length))
        return false;
    memcpy(blob->data + blob->length, bytes, length);
    blob->length += length;
    return true;
}

static bool offline_blob_u32(OfflineBlob *blob, uint32_t value)
{
    unsigned char bytes[4];
    size_t used = 0;
    return put_u32(bytes, sizeof(bytes), &used, value)
        && offline_blob_bytes(blob, bytes, sizeof(bytes));
}

static bool offline_blob_text(OfflineBlob *blob, const char *text,
                              size_t limit)
{
    size_t length = text == NULL ? 0 : strnlen(text, limit);
    return length < limit && length <= UINT32_MAX
        && offline_blob_u32(blob, (uint32_t) length)
        && offline_blob_bytes(blob, text, length);
}

static lxb_status_t offline_document_receive(
    const lxb_char_t *data, size_t length, void *opaque)
{
    return offline_blob_bytes(opaque, data, length)
        ? LXB_STATUS_OK : LXB_STATUS_ERROR_MEMORY_ALLOCATION;
}

static bool offline_pack_view(OfflineBlob *blob,
                              const BrowserOfflineCacheView *view)
{
    const TilefinchResourceGrant *grant = &view->resource_grant;
    return view->length <= UINT32_MAX
        && offline_blob_u32(blob, (uint32_t) view->kind)
        && offline_blob_u32(blob, (uint32_t) view->length)
        && offline_blob_text(blob, view->url, OFFLINE_LIBRARY_URL_LIMIT)
        && offline_blob_text(blob, view->content_type, 128u)
        && offline_blob_text(blob, view->response_url,
                             OFFLINE_LIBRARY_URL_LIMIT)
        && offline_blob_text(blob, view->response_referrer_policy, 128u)
        && offline_blob_u32(blob, (uint32_t) grant->destination)
        && offline_blob_u32(blob, (uint32_t) grant->mode)
        && offline_blob_u32(blob, (uint32_t) grant->credentials)
        && offline_blob_u32(blob, (uint32_t) grant->corp)
        && offline_blob_u32(blob, grant->initiator_opaque ? 1u : 0u)
        && offline_blob_u32(blob, grant->final_same_origin ? 1u : 0u)
        && offline_blob_u32(blob, grant->final_same_site ? 1u : 0u)
        && offline_blob_u32(blob, grant->cors_validated ? 1u : 0u)
        && offline_blob_u32(blob, grant->nosniff ? 1u : 0u)
        && offline_blob_u32(blob, grant->mime_validated ? 1u : 0u)
        && offline_blob_u32(blob, (uint32_t) view->module_credentials)
        && offline_blob_u32(blob, view->module_cors_validated ? 1u : 0u)
        && offline_blob_u32(blob,
                            view->module_redirect_origin_tainted ? 1u : 0u)
        && offline_blob_u32(blob,
                            view->module_javascript_mime_validated ? 1u : 0u)
        && offline_blob_bytes(blob, view->data, view->length);
}

static bool offline_write_blob(const OfflineLibrary *library, uint32_t id,
                               const char *suffix, const OfflineBlob *blob)
{
    char path[OFFLINE_LIBRARY_DIRECTORY_LIMIT + 40u];
    if (!offline_library_item_path(library, id, suffix, path, sizeof(path)))
        return false;
    FILE *file = fopen(path, "wb");
    bool okay = file != NULL
        && fwrite(blob->data, 1, blob->length, file) == blob->length
        && fflush(file) == 0 && ferror(file) == 0;
    if (file != NULL && fclose(file) != 0) okay = false;
    if (!okay) (void) remove(path);
    return okay;
}

static void offline_remove_app_payloads(const OfflineLibrary *library,
                                        uint32_t id)
{
    static const char *suffixes[] = {
        ".app.html", ".app.pack", ".app.icon",
        ".app-html.tmp", ".app-pack.tmp", ".app-icon.tmp"
    };
    char path[OFFLINE_LIBRARY_DIRECTORY_LIMIT + 40u];
    for (size_t at = 0; at < sizeof(suffixes) / sizeof(suffixes[0]); at++)
        if (offline_library_item_path(
                library, id, suffixes[at], path, sizeof(path)))
            (void) remove(path);
}

typedef struct {
    OfflineBlob html;
    OfflineBlob pack;
    size_t resource_count;
} OfflineAppSnapshot;

static void offline_app_snapshot_destroy(
    OfflineLibrary *library, OfflineAppSnapshot *snapshot)
{
    if (library == NULL || snapshot == NULL) return;
    budget_free(library->budget, snapshot->html.data);
    budget_free(library->budget, snapshot->pack.data);
    memset(snapshot, 0, sizeof(*snapshot));
}

static bool offline_app_snapshot_build(
    OfflineLibrary *library, PocDocument *document, BrowserSession *session,
    const char *source_url, OfflineAppSnapshot *snapshot,
    char *error, size_t error_size)
{
    if (library == NULL || document == NULL || session == NULL
        || source_url == NULL || snapshot == NULL) return false;
    *snapshot = (OfflineAppSnapshot) {
        .html = {
            .budget = library->budget,
            .limit = OFFLINE_LIBRARY_APP_DOCUMENT_LIMIT
        },
        .pack = {
            .budget = library->budget,
            .limit = OFFLINE_LIBRARY_APP_PACK_LIMIT
        }
    };
    BrowserOfflineCacheView views[BROWSER_OFFLINE_CACHE_ENTRY_LIMIT];
    size_t resource_bytes = 0;
    bool complete = false;
    size_t cache_limit = session->maximum_cache_bytes
        < OFFLINE_LIBRARY_APP_RESOURCE_LIMIT
        ? session->maximum_cache_bytes : OFFLINE_LIBRARY_APP_RESOURCE_LIMIT;
    snapshot->resource_count =
        browser_session_cache_collect_offline_same_origin(
            session, source_url, views, BROWSER_OFFLINE_CACHE_ENTRY_LIMIT,
            cache_limit, &resource_bytes, &complete);
    if (!complete) {
        offline_error(error, error_size,
                      "app resources exceed the offline package bound");
        return false;
    }
    static const unsigned char magic[8] = {'T','F','A','P','P','0','1',0};
    bool okay = lxb_html_serialize_tree_cb(
            lxb_dom_interface_node(document->html),
            offline_document_receive, &snapshot->html) == LXB_STATUS_OK
        && snapshot->html.length != 0
        && offline_blob_bytes(&snapshot->pack, magic, sizeof(magic))
        && offline_blob_u32(&snapshot->pack, 1u)
        && offline_blob_u32(
               &snapshot->pack, (uint32_t) snapshot->resource_count);
    for (size_t at = 0; okay && at < snapshot->resource_count; at++)
        okay = offline_pack_view(&snapshot->pack, &views[at]);
    if (!okay) {
        offline_error(error, error_size,
                      "offline app exceeded snapshot bounds");
        offline_app_snapshot_destroy(library, snapshot);
    }
    return okay;
}

bool offline_library_preview_web_app(
    OfflineLibrary *library, PocDocument *document, BrowserSession *session,
    const char *source_url, const TilefinchWebAppManifest *manifest,
    const unsigned char *icon, size_t icon_length,
    OfflineWebAppPreview *preview, char *error, size_t error_size)
{
    if (preview != NULL) *preview = (OfflineWebAppPreview) {0};
    if (error != NULL && error_size != 0) error[0] = '\0';
    if (library == NULL || document == NULL || session == NULL
        || source_url == NULL || manifest == NULL || preview == NULL
        || strlen(source_url) >= OFFLINE_LIBRARY_URL_LIMIT
        || icon_length > OFFLINE_LIBRARY_APP_ICON_LIMIT
        || !offline_library_load(library)) {
        offline_error(error, error_size, "offline app input is invalid");
        return false;
    }
    OfflineAppSnapshot snapshot = {0};
    if (!offline_app_snapshot_build(
            library, document, session, source_url, &snapshot,
            error, error_size)) return false;
    preview->estimated_bytes = (uint64_t) snapshot.html.length
        + snapshot.pack.length + icon_length;
    preview->document_hash = offline_hash(
        snapshot.html.data, snapshot.html.length);
    preview->resource_hash = offline_hash(
        snapshot.pack.data, snapshot.pack.length);
    preview->icon_hash = icon_length == 0 ? 0 : offline_hash(icon, icon_length);
    preview->resource_count = (uint32_t) snapshot.resource_count;
    preview->operation = OFFLINE_WEB_APP_INSTALL;
    for (size_t at = 0; at < library->count; at++) {
        const OfflineLibraryItem *item = &library->items[at];
        if (item->type != OFFLINE_ITEM_WEB_APP
            || strcmp(item->source_url, source_url) != 0) continue;
        bool unchanged = item->article_hash == preview->document_hash
            && item->auxiliary_hash == preview->resource_hash
            && item->icon_hash == preview->icon_hash
            && item->app_theme_color == manifest->theme_color
            && item->app_theme_alpha == manifest->theme_alpha
            && item->app_display_mode == (uint8_t) manifest->display_mode
            && item->app_theme_color_valid == manifest->theme_color_valid;
        preview->operation = unchanged
            ? OFFLINE_WEB_APP_REINSTALL : OFFLINE_WEB_APP_UPDATE;
        break;
    }
    offline_app_snapshot_destroy(library, &snapshot);
    return true;
}

bool offline_library_save_web_app(
    OfflineLibrary *library, PocDocument *document, BrowserSession *session,
    const char *source_url, const TilefinchWebAppManifest *manifest,
    const unsigned char *icon, size_t icon_length,
    uint32_t *saved_id, char *error, size_t error_size)
{
    if (error != NULL && error_size != 0) error[0] = '\0';
    if (library == NULL || document == NULL || session == NULL
        || source_url == NULL || manifest == NULL
        || strlen(source_url) >= OFFLINE_LIBRARY_URL_LIMIT
        || icon_length > OFFLINE_LIBRARY_APP_ICON_LIMIT
        || !offline_library_load(library) || !offline_directory_ready(library)) {
        offline_error(error, error_size, "offline app input is invalid");
        return false;
    }
    OfflineAppSnapshot snapshot = {0};
    if (!offline_app_snapshot_build(
            library, document, session, source_url, &snapshot,
            error, error_size)) return false;
    OfflineBlob *html = &snapshot.html;
    OfflineBlob *pack = &snapshot.pack;
    size_t resource_count = snapshot.resource_count;
    bool okay = true;
    uint64_t free_bytes = 0;
    uint64_t required = (uint64_t) html->length + pack->length + icon_length
        + OFFLINE_ARTICLE_FREE_SPACE_RESERVE;
    if (!tilefinch_update_query_free_space(library->directory, &free_bytes)
        || free_bytes < required) {
        offline_error(error, error_size,
                      "not enough free Memory Stick space");
        okay = false;
    }
    size_t slot = library->count;
    for (size_t at = 0; at < library->count; at++)
        if (library->items[at].type == OFFLINE_ITEM_WEB_APP
            && strcmp(library->items[at].source_url, source_url) == 0) {
            slot = at;
            break;
        }
    if (slot == library->count && library->count >= OFFLINE_LIBRARY_ITEM_LIMIT)
        okay = false;
    bool replacing = slot < library->count;
    OfflineLibraryItem previous = {0};
    if (replacing) previous = library->items[slot];
    uint32_t previous_next_id = library->next_id;
    uint32_t id = library->next_id++;
    if (id == 0) id = library->next_id++;
    OfflineBlob icon_blob = {
        .budget = library->budget, .data = (unsigned char *) icon,
        .length = icon_length, .capacity = icon_length,
        .limit = OFFLINE_LIBRARY_APP_ICON_LIMIT
    };
    if (!okay
        || !offline_write_blob(library, id, ".app-html.tmp", html)
        || !offline_write_blob(library, id, ".app-pack.tmp", pack)
        || (icon_length != 0
            && !offline_write_blob(library, id, ".app-icon.tmp", &icon_blob))) {
        offline_error(error, error_size,
                      "offline app exceeded storage or write bounds");
        goto fail;
    }
    char temporary[OFFLINE_LIBRARY_DIRECTORY_LIMIT + 40u];
    char final[OFFLINE_LIBRARY_DIRECTORY_LIMIT + 40u];
    const char *pairs[][2] = {
        {".app-html.tmp", ".app.html"}, {".app-pack.tmp", ".app.pack"},
        {".app-icon.tmp", ".app.icon"}
    };
    for (size_t at = 0; at < (icon_length == 0 ? 2u : 3u); at++) {
        if (!offline_library_item_path(library, id, pairs[at][0], temporary,
                                       sizeof(temporary))
            || !offline_library_item_path(library, id, pairs[at][1], final,
                                           sizeof(final))) goto fail;
        (void) remove(final);
        if (rename(temporary, final) != 0) goto fail;
    }
    OfflineLibraryItem item = {
        .id = id, .type = OFFLINE_ITEM_WEB_APP, .state = OFFLINE_ITEM_READY,
        .content_bytes = html->length, .audio_bytes = pack->length,
        .downloaded_bytes = html->length + pack->length + icon_length,
        .saved_at_unix = offline_now_unix(),
        .article_hash = offline_hash(html->data, html->length),
        .auxiliary_hash = offline_hash(pack->data, pack->length),
        .icon_hash = icon_length == 0 ? 0 : offline_hash(icon, icon_length),
        .resource_count = (uint32_t) resource_count,
        .icon_bytes = (uint32_t) icon_length,
        .app_theme_color = manifest->theme_color,
        .app_theme_alpha = manifest->theme_alpha,
        .app_display_mode = (uint8_t) manifest->display_mode,
        .app_theme_color_valid = manifest->theme_color_valid
    };
    const char *name = manifest->short_name[0] != '\0'
        ? manifest->short_name : manifest->name;
    snprintf(item.title, sizeof(item.title), "%s", name[0] == '\0'
             ? (document->title == NULL ? "Offline app" : document->title)
             : name);
    snprintf(item.source_url, sizeof(item.source_url), "%s", source_url);
    library->items[slot] = item;
    if (!replacing) library->count++;
    if (!offline_library_save(library)) {
        if (replacing) library->items[slot] = previous;
        else library->count--;
        library->next_id = previous_next_id;
        goto fail;
    }
    if (replacing) offline_remove_app_payloads(library, previous.id);
    if (saved_id != NULL) *saved_id = id;
    offline_app_snapshot_destroy(library, &snapshot);
    return true;
fail:
    offline_remove_app_payloads(library, id);
    if (library->next_id != previous_next_id
        && (slot >= library->count || library->items[slot].id != id))
        library->next_id = previous_next_id;
    offline_app_snapshot_destroy(library, &snapshot);
    return false;
}

static bool offline_read_file(const OfflineLibrary *library, Budget *budget,
                              uint32_t id, const char *suffix, size_t size,
                              uint32_t hash, unsigned char **output)
{
    if (size == 0 || size > SIZE_MAX - 1u || output == NULL) return false;
    char path[OFFLINE_LIBRARY_DIRECTORY_LIMIT + 40u];
    if (!offline_library_item_path(library, id, suffix, path, sizeof(path)))
        return false;
    unsigned char *data = budget_malloc_category(
        budget, BUDGET_CATEGORY_SESSION, size + 1u);
    FILE *file = data == NULL ? NULL : fopen(path, "rb");
    bool okay = file != NULL && fread(data, 1, size, file) == size
        && fgetc(file) == EOF && !ferror(file);
    if (file != NULL && fclose(file) != 0) okay = false;
    if (okay) okay = offline_hash(data, size) == hash;
    if (!okay) {
        budget_free(budget, data);
        return false;
    }
    data[size] = 0;
    *output = data;
    return true;
}

static bool offline_pack_text(const unsigned char *data, size_t length,
                              size_t *used, const char **text)
{
    uint32_t size = 0;
    if (!get_u32(data, length, used, &size) || *used > length
        || size > length - *used) return false;
    *text = (const char *) data + *used;
    *used += size;
    return true;
}

bool offline_library_read_web_app(
    const OfflineLibrary *library, Budget *budget, BrowserSession *session,
    uint32_t id, char **html, size_t *length,
    char *error, size_t error_size)
{
    if (html != NULL) *html = NULL;
    if (length != NULL) *length = 0;
    const OfflineLibraryItem *item = offline_library_find(library, id);
    if (item == NULL || item->type != OFFLINE_ITEM_WEB_APP
        || item->state != OFFLINE_ITEM_READY || budget == NULL
        || session == NULL || html == NULL || length == NULL
        || item->content_bytes > OFFLINE_LIBRARY_APP_DOCUMENT_LIMIT
        || item->audio_bytes > OFFLINE_LIBRARY_APP_PACK_LIMIT) {
        offline_error(error, error_size, "offline app is unavailable");
        return false;
    }
    unsigned char *document = NULL, *pack = NULL;
    if (!offline_read_file(library, budget, id, ".app.html",
                           (size_t) item->content_bytes, item->article_hash,
                           &document)
        || !offline_read_file(library, budget, id, ".app.pack",
                              (size_t) item->audio_bytes,
                              item->auxiliary_hash, &pack)) goto fail;
    static const unsigned char magic[8] = {'T','F','A','P','P','0','1',0};
    size_t used = 0;
    uint32_t version = 0, count = 0;
    if (item->audio_bytes < sizeof(magic)
        || memcmp(pack, magic, sizeof(magic)) != 0) goto fail;
    used = sizeof(magic);
    if (!get_u32(pack, (size_t) item->audio_bytes, &used, &version)
        || !get_u32(pack, (size_t) item->audio_bytes, &used, &count)
        || version != 1u || count != item->resource_count
        || count > BROWSER_OFFLINE_CACHE_ENTRY_LIMIT) goto fail;
    for (uint32_t at = 0; at < count; at++) {
        uint32_t kind = 0, body_length = 0, fields[14] = {0};
        const char *texts[4] = {0};
        size_t starts[4] = {0}, sizes[4] = {0};
        if (!get_u32(pack, (size_t) item->audio_bytes, &used, &kind)
            || !get_u32(pack, (size_t) item->audio_bytes, &used, &body_length))
            goto fail;
        for (size_t field = 0; field < 4u; field++) {
            starts[field] = used;
            if (!offline_pack_text(pack, (size_t) item->audio_bytes,
                                   &used, &texts[field])) goto fail;
            sizes[field] = used - starts[field] - 4u;
        }
        for (size_t field = 0; field < 14u; field++)
            if (!get_u32(pack, (size_t) item->audio_bytes, &used,
                         &fields[field])) goto fail;
        if (used > item->audio_bytes || body_length > item->audio_bytes - used
            || kind > BROWSER_OFFLINE_CACHE_MODULE) goto fail;
        char url[OFFLINE_LIBRARY_URL_LIMIT];
        char content_type[128];
        char response_url[OFFLINE_LIBRARY_URL_LIMIT];
        char referrer_policy[BROWSER_REFERRER_POLICY_LIMIT];
        char *strings[4] = {
            url, content_type, response_url, referrer_policy
        };
        const size_t capacities[4] = {
            sizeof(url), sizeof(content_type), sizeof(response_url),
            sizeof(referrer_policy)
        };
        for (size_t field = 0; field < 4u; field++) {
            if (sizes[field] >= capacities[field]) goto fail;
            memcpy(strings[field], texts[field], sizes[field]);
            strings[field][sizes[field]] = '\0';
        }
        BrowserOfflineCacheView view = {
            .kind = (BrowserOfflineCacheKind) kind,
            .url = strings[0], .content_type = strings[1],
            .response_url = strings[2],
            .response_referrer_policy = strings[3],
            .data = pack + used, .length = body_length,
            .resource_grant = {
                .destination = (TilefinchRequestDestination) fields[0],
                .mode = (TilefinchRequestMode) fields[1],
                .credentials = (TilefinchCredentialsMode) fields[2],
                .corp = (TilefinchCrossOriginResourcePolicy) fields[3],
                .initiator_opaque = fields[4] != 0,
                .final_same_origin = fields[5] != 0,
                .final_same_site = fields[6] != 0,
                .cors_validated = fields[7] != 0,
                .nosniff = fields[8] != 0,
                .mime_validated = fields[9] != 0
            },
            .module_credentials = (TilefinchCredentialsMode) fields[10],
            .module_cors_validated = fields[11] != 0,
            .module_redirect_origin_tainted = fields[12] != 0,
            .module_javascript_mime_validated = fields[13] != 0
        };
        if (!browser_session_cache_restore_offline(
                session, item->source_url, &view)) goto fail;
        used += body_length;
    }
    if (used != item->audio_bytes) goto fail;
    budget_free(budget, pack);
    *html = (char *) document;
    *length = (size_t) item->content_bytes;
    return true;
fail:
    budget_free(budget, document);
    budget_free(budget, pack);
    offline_error(error, error_size, "offline app failed integrity checks");
    return false;
}

bool offline_library_read_web_app_icon(
    const OfflineLibrary *library, uint32_t id,
    unsigned char output[OFFLINE_LIBRARY_APP_ICON_LIMIT])
{
    const OfflineLibraryItem *item = offline_library_find(library, id);
    if (item == NULL || item->type != OFFLINE_ITEM_WEB_APP
        || item->icon_bytes != OFFLINE_LIBRARY_APP_ICON_LIMIT
        || output == NULL) return false;
    char path[OFFLINE_LIBRARY_DIRECTORY_LIMIT + 40u];
    if (!offline_library_item_path(
            library, id, ".app.icon", path, sizeof(path))) return false;
    FILE *file = fopen(path, "rb");
    bool okay = file != NULL
        && fread(output, 1, OFFLINE_LIBRARY_APP_ICON_LIMIT, file)
               == OFFLINE_LIBRARY_APP_ICON_LIMIT
        && fgetc(file) == EOF && !ferror(file);
    if (file != NULL && fclose(file) != 0) okay = false;
    return okay && offline_hash(output, OFFLINE_LIBRARY_APP_ICON_LIMIT)
                       == item->icon_hash;
}

bool offline_library_enqueue_youtube(
    OfflineLibrary *library, const char *watch_url, const char *title,
    uint32_t *queued_id, char *error, size_t error_size)
{
    if (library == NULL || !youtube_watch_url_supported(watch_url)
        || strlen(watch_url) >= OFFLINE_LIBRARY_URL_LIMIT
        || !offline_library_load(library)) {
        offline_error(error, error_size, "YouTube save route is invalid");
        return false;
    }
    char video_id[YOUTUBE_VIDEO_ID_CAPACITY] = {0};
    if (!youtube_watch_url_video_id(watch_url, video_id)) return false;
    for (size_t index = 0; index < library->count; index++) {
        if (library->items[index].type == OFFLINE_ITEM_YOUTUBE
            && strcmp(library->items[index].video_id, video_id) == 0) {
            if (queued_id != NULL) *queued_id = library->items[index].id;
            return true;
        }
    }
    if (library->count >= OFFLINE_LIBRARY_ITEM_LIMIT) {
        offline_error(
            error, error_size,
            "offline library is full; delete its oldest saved item");
        return false;
    }
    OfflineLibraryItem *item = &library->items[library->count++];
    *item = (OfflineLibraryItem) {
        .id = library->next_id++,
        .type = OFFLINE_ITEM_YOUTUBE,
        .state = OFFLINE_ITEM_QUEUED,
        .saved_at_unix = offline_now_unix()
    };
    if (item->id == 0) item->id = library->next_id++;
    snprintf(item->title, sizeof(item->title), "%s",
             title == NULL || title[0] == '\0' ? "YouTube video" : title);
    snprintf(item->source_url, sizeof(item->source_url), "%s", watch_url);
    snprintf(item->video_id, sizeof(item->video_id), "%s", video_id);
    if (!offline_library_save(library)) {
        memset(item, 0, sizeof(*item));
        library->count--;
        offline_error(error, error_size, "download queue could not be saved");
        return false;
    }
    if (queued_id != NULL) *queued_id = item->id;
    return true;
}

bool offline_library_apply_youtube_stream(
    OfflineLibrary *library, uint32_t id, const YoutubeStream *stream)
{
    OfflineLibraryItem *item = offline_library_find_mutable(library, id);
    if (item == NULL || item->type != OFFLINE_ITEM_YOUTUBE || stream == NULL
        || stream->content_length == 0
        || (stream->split_streams && stream->audio_content_length == 0))
        return false;
    OfflineLibraryItem previous = *item;
    item->content_bytes = stream->content_length;
    item->audio_bytes = stream->audio_content_length;
    item->duration_ms = stream->duration_ms;
    item->width = stream->width;
    item->height = stream->height;
    item->itag = stream->itag;
    item->audio_itag = stream->audio_itag;
    item->split_streams = stream->split_streams;
    if (stream->title[0] != '\0')
        snprintf(item->title, sizeof(item->title), "%.*s",
                 (int) sizeof(item->title) - 1, stream->title);
    if (offline_library_save(library)) return true;
    *item = previous;
    return false;
}

bool offline_library_remove(OfflineLibrary *library, uint32_t id)
{
    if (library == NULL || !library->loaded) return false;
    size_t index = 0;
    while (index < library->count && library->items[index].id != id) index++;
    if (index == library->count) return false;
    OfflineLibrary *previous = budget_malloc_category(
        library->budget, BUDGET_CATEGORY_SESSION, sizeof(*previous));
    if (previous == NULL) return false;
    *previous = *library;
    if (index + 1u < library->count)
        memmove(&library->items[index], &library->items[index + 1u],
                (library->count - index - 1u) * sizeof(library->items[0]));
    memset(&library->items[library->count - 1u], 0,
           sizeof(library->items[0]));
    library->count--;
    if (!offline_library_save(library)) {
        *library = *previous;
        budget_free(library->budget, previous);
        return false;
    }
    budget_free(library->budget, previous);
    const char *suffixes[] = {
        ".article.html", ".article.bak", ".article.tmp",
        ".app.html", ".app.pack", ".app.icon",
        ".app-html.tmp", ".app-pack.tmp", ".app-icon.tmp",
        ".video.mp4", ".audio.mp4",
        ".video.part", ".audio.part"
    };
    char path[OFFLINE_LIBRARY_DIRECTORY_LIMIT + 40u];
    for (size_t at = 0; at < sizeof(suffixes) / sizeof(suffixes[0]); at++)
        if (offline_library_item_path(
                library, id, suffixes[at], path, sizeof(path)))
            (void) remove(path);
    return true;
}

typedef struct {
    Budget *budget;
    char *data;
    size_t length;
    size_t capacity;
    bool failed;
} OfflineHtml;

static bool html_append(OfflineHtml *html, const char *format, ...)
{
    if (html == NULL || html->failed) return false;
    va_list arguments;
    va_start(arguments, format);
    int wanted = vsnprintf(NULL, 0, format, arguments);
    va_end(arguments);
    if (wanted < 0 || (size_t) wanted > 65536u) {
        html->failed = true;
        return false;
    }
    size_t need = (size_t) wanted;
    if (html->length > 64u * 1024u || need > 64u * 1024u - html->length) {
        html->failed = true;
        return false;
    }
    if (html->length + need + 1u > html->capacity) {
        size_t capacity = html->capacity == 0 ? 4096u : html->capacity;
        while (capacity < html->length + need + 1u) capacity *= 2u;
        char *grown = budget_realloc_category(
            html->budget, BUDGET_CATEGORY_SESSION, html->data, capacity);
        if (grown == NULL) {
            html->failed = true;
            return false;
        }
        html->data = grown;
        html->capacity = capacity;
    }
    va_start(arguments, format);
    vsnprintf(html->data + html->length, need + 1u, format, arguments);
    va_end(arguments);
    html->length += need;
    return true;
}

static bool html_escape(OfflineHtml *html, const char *text)
{
    if (text == NULL) return true;
    for (const unsigned char *at = (const unsigned char *) text;
         *at != 0; at++) {
        if (*at == '&') { if (!html_append(html, "&amp;")) return false; }
        else if (*at == '<') { if (!html_append(html, "&lt;")) return false; }
        else if (*at == '>') { if (!html_append(html, "&gt;")) return false; }
        else if (*at == '"') { if (!html_append(html, "&quot;")) return false; }
        else if (!html_append(html, "%c", *at)) return false;
    }
    return true;
}

static bool offline_saved_date(
    uint64_t timestamp, char output[11])
{
    if (timestamp == 0 || output == NULL) return false;
    time_t value = (time_t) timestamp;
    if (value < 0 || (uint64_t) value != timestamp) return false;
    struct tm *parts = gmtime(&value);
    return parts != NULL
        && strftime(output, 11, "%Y-%m-%d", parts) == 10;
}

bool offline_library_build_page(
    const OfflineLibrary *library, Budget *budget,
    char **output, size_t *length)
{
    if (output != NULL) *output = NULL;
    if (length != NULL) *length = 0;
    if (library == NULL || !library->loaded || budget == NULL
        || output == NULL || length == NULL) return false;
    OfflineHtml html = {.budget = budget};
    uint64_t free_bytes = 0;
    bool have_free_space = tilefinch_update_query_free_space(
        library->directory, &free_bytes);
    bool okay = html_append(
        &html, "<!doctype html><meta name=viewport content=\"width=device-width\">"
        "<style>body{font:16px/1.4 sans-serif;margin:0;padding:14px;background:#f6f4ee;"
        "color:#202020}h1{font-size:22px}.item{background:white;border:1px solid #ccc;"
        "border-radius:8px;padding:10px;margin:9px 0}.meta{color:#666;font-size:13px}"
        "a{color:#0645ad;margin-right:14px}</style><h1>Saved offline</h1>"
        "<p>Reader articles, offline web apps, and YouTube downloads are stored on the Memory Stick.</p>");
    if (okay && have_free_space)
        okay = html_append(
            &html, "<p class=meta>Free space: %llu.%llu MB</p>",
            (unsigned long long) (free_bytes / UINT64_C(1048576)),
            (unsigned long long)
                ((free_bytes % UINT64_C(1048576)) * 10u
                 / UINT64_C(1048576)));
    bool have_queued = false;
    for (size_t index = 0; index < library->count; index++)
        if (library->items[index].type == OFFLINE_ITEM_YOUTUBE
            && library->items[index].state == OFFLINE_ITEM_QUEUED)
            have_queued = true;
    if (okay && have_queued)
        okay = html_append(
            &html,
            "<p><a href=\"https://tilefinch.local/offline/start-queue\">Start queued downloads</a></p>");
    if (okay && library->count == 0)
        okay = html_append(&html, "<p>Nothing saved yet.</p>");
    size_t oldest = 0;
    for (size_t index = 1; index < library->count; index++) {
        uint64_t candidate = library->items[index].saved_at_unix;
        uint64_t retained = library->items[oldest].saved_at_unix;
        if ((candidate != 0 && retained == 0)
            || (candidate != 0 && candidate < retained)) oldest = index;
    }
    if (okay && library->count == OFFLINE_LIBRARY_ITEM_LIMIT)
        okay = html_append(
            &html, "<p class=meta>Library full. The oldest item is marked "
                   "below as a deletion suggestion.</p>");
    for (size_t index = 0; okay && index < library->count; index++) {
        const OfflineLibraryItem *item = &library->items[index];
        const char *state = item->state == OFFLINE_ITEM_READY ? "ready"
            : item->state == OFFLINE_ITEM_DOWNLOADING ? "downloading"
            : item->state == OFFLINE_ITEM_PAUSED ? "paused"
            : item->state == OFFLINE_ITEM_FAILED ? "failed" : "queued";
        unsigned percent = 0;
        uint64_t total = item->content_bytes + item->audio_bytes;
        if (total != 0)
            percent = (unsigned) (item->downloaded_bytes * 100u / total);
        okay = html_append(&html, "<section class=item><strong>")
            && html_escape(&html, item->title)
            && html_append(
                &html, "</strong><div class=meta>%s &middot; %s",
                item->type == OFFLINE_ITEM_ARTICLE ? "Reader article"
                    : item->type == OFFLINE_ITEM_WEB_APP ? "Offline web app"
                                                        : "YouTube video",
                state);
        if (okay && total != 0)
            okay = html_append(
                &html, " &middot; %llu.%llu MB%s",
                (unsigned long long) (total / UINT64_C(1048576)),
                (unsigned long long)
                    ((total % UINT64_C(1048576)) * 10u
                     / UINT64_C(1048576)),
                item->type == OFFLINE_ITEM_YOUTUBE ? "" : " saved");
        if (okay && item->type == OFFLINE_ITEM_YOUTUBE && total != 0)
            okay = html_append(&html, " &middot; %u%%", percent);
        char saved_date[11];
        if (okay && offline_saved_date(item->saved_at_unix, saved_date))
            okay = html_append(
                &html, " &middot; saved %s", saved_date);
        if (okay && library->count == OFFLINE_LIBRARY_ITEM_LIMIT
            && index == oldest)
            okay = html_append(&html, " &middot; oldest");
        if (okay) okay = html_append(&html, "</div>");
        if (okay && item->state == OFFLINE_ITEM_READY)
            okay = html_append(
                &html, item->type == OFFLINE_ITEM_ARTICLE
                    ? "<a href=\"https://tilefinch.local/offline/article?id=%u\">Open</a>"
                    : item->type == OFFLINE_ITEM_WEB_APP
                        ? "<a href=\"https://tilefinch.local/offline/app?id=%u\">Open</a>"
                        : "<a href=\"https://tilefinch.local/offline/video?id=%u\">Play</a>",
                (unsigned) item->id);
        if (okay && item->type == OFFLINE_ITEM_YOUTUBE
            && item->state != OFFLINE_ITEM_READY)
            okay = html_append(
                &html, "<a href=\"https://tilefinch.local/offline/resume?id=%u\">%s</a>",
                (unsigned) item->id,
                item->state == OFFLINE_ITEM_DOWNLOADING ? "Pause" : "Resume");
        if (okay)
            okay = html_append(
                &html, "<a href=\"https://tilefinch.local/offline/delete?id=%u\">Delete</a></section>",
                (unsigned) item->id);
    }
    if (!okay || html.failed || html.data == NULL) {
        budget_free(budget, html.data);
        return false;
    }
    *output = html.data;
    *length = html.length;
    return true;
}
