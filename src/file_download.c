#include "tilefinch/file_download.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "tilefinch/platform.h"
#include "tilefinch/update.h"
#include "tilefinch/url.h"
#include "tilefinch/user_agent.h"

#define FILE_VALIDATOR_LIMIT 160u

struct FileDownloadManager {
    Budget *budget;
    BrowserSession *session;
    OfflineLibrary *library;
    FetchScheduler *scheduler;
    FetchResult *completion;
    FILE *output;
    uint32_t id;
    uint64_t request_id, offset, received, total, available;
    OfflineDownloadSpeed speed;
    bool headers_valid, length_known;
    /* A 416 confirmed the partial file already holds the whole entity. */
    bool resume_complete;
    char validator[FILE_VALIDATOR_LIMIT];
    char error[OFFLINE_LIBRARY_FAILURE_LIMIT];
};

bool file_download_url_supported(const char *url)
{
    TilefinchUrl parsed;
    return tilefinch_url_parse(url, &parsed)
        && parsed.scheme == TILEFINCH_URL_SCHEME_HTTPS
        && parsed.length < TILEFINCH_URL_SERIALIZED_LIMIT;
}

/* Response filenames are advisory. Use one conservative, portable basename,
   including on FAT. Non-ASCII bytes become '_' until the device file picker
   has a defined filename encoding; this never changes downloaded bytes. */
static void sanitize_filename(const char *input,
                               char output[FILE_DOWNLOAD_FILENAME_LIMIT])
{
    const char *base = input;
    for (const char *at = input; *at != '\0'; at++)
        if (*at == '/' || *at == '\\') base = at + 1;
    size_t used = 0;
    for (; *base != '\0' && used + 1 < FILE_DOWNLOAD_FILENAME_LIMIT; base++) {
        unsigned char c = (unsigned char) *base;
        output[used++] = c < 32 || c >= 127 || strchr("<>:\"|?*", c)
            ? '_' : (char) c;
    }
    while (used != 0 && (output[used - 1] == '.' || output[used - 1] == ' ')) used--;
    output[used] = '\0';
    if (used == 0 || output[0] == '.') {
        snprintf(output, FILE_DOWNLOAD_FILENAME_LIMIT, "download.bin");
        return;
    }
    size_t stem = strcspn(output, ".");
    bool reserved = (stem == 3 && (!strncasecmp(output, "CON", 3)
        || !strncasecmp(output, "PRN", 3) || !strncasecmp(output, "AUX", 3)
        || !strncasecmp(output, "NUL", 3)))
        || (stem == 4 && (!strncasecmp(output, "COM", 3)
            || !strncasecmp(output, "LPT", 3)) && output[3] >= '1' && output[3] <= '9');
    if (reserved) output[0] = '_';
}

bool file_download_filename_valid(const char *name)
{
    if (name == NULL || strnlen(name, FILE_DOWNLOAD_FILENAME_LIMIT)
            >= FILE_DOWNLOAD_FILENAME_LIMIT) return false;
    char safe[FILE_DOWNLOAD_FILENAME_LIMIT];
    sanitize_filename(name, safe);
    return strcmp(name, safe) == 0;
}

static bool disposition_name(const char *header, bool extended,
                              char output[1024])
{
    if (header == NULL || strlen(header) > 4096) return false;
    const char *at = header;
    while (*at != '\0') {
        if (*at++ != ';') continue;
        while (*at == ' ' || *at == '\t') at++;
        const char *name = at;
        while (isalnum((unsigned char) *at) || *at == '-' || *at == '*') at++;
        size_t count = (size_t) (at - name);
        bool match = count == (extended ? 9u : 8u)
            && strncasecmp(name, extended ? "filename*" : "filename", count) == 0;
        while (*at == ' ' || *at == '\t') at++;
        if (*at != '=') continue;
        at++;
        while (*at == ' ' || *at == '\t') at++;
        bool quoted = *at == '"';
        if (quoted) at++;
        size_t used = 0;
        while (*at != '\0' && (quoted ? *at != '"' : *at != ';')) {
            char c = *at++;
            if (quoted && c == '\\' && *at != '\0') c = *at++;
            if (used + 1 >= 1024 || c == '\r' || c == '\n') return false;
            output[used++] = c;
        }
        if (quoted && *at == '\0') return false;
        if (quoted) at++;
        while (used != 0 && (output[used - 1] == ' ' || output[used - 1] == '\t')) used--;
        output[used] = '\0';
        if (!match) continue;
        if (!extended) return used != 0;
        if (strncasecmp(output, "UTF-8'", 6) != 0) return false;
        char *value = strchr(output + 6, '\'');
        if (value == NULL) return false;
        memmove(output, value + 1, strlen(value + 1) + 1);
        return tilefinch_url_percent_decode(output) && output[0] != '\0';
    }
    return false;
}

bool file_download_filename(const char *url, const char *disposition,
                            char output[FILE_DOWNLOAD_FILENAME_LIMIT])
{
    if (output == NULL || !file_download_url_supported(url)) return false;
    char candidate[1024];
    if (!disposition_name(disposition, true, candidate)
        && !disposition_name(disposition, false, candidate)) {
        TilefinchUrl parsed;
        if (!tilefinch_url_parse(url, &parsed)) return false;
        size_t first = parsed.path_offset, end = first + parsed.path_length;
        for (size_t i = first; i < end; i++) if (url[i] == '/') first = i + 1;
        size_t size = end - first;
        if (size >= sizeof(candidate)) return false;
        memcpy(candidate, url + first, size);
        candidate[size] = '\0';
        if (!tilefinch_url_percent_decode(candidate)) candidate[0] = '\0';
    }
    sanitize_filename(candidate, output);
    return true;
}

bool file_download_url_candidate(const char *url)
{
    char name[FILE_DOWNLOAD_FILENAME_LIMIT];
    if (!file_download_filename(url, NULL, name)) return false;
    const char *extension = strrchr(name, '.');
    static const char *const types[] = {
        ".zip", ".7z", ".rar", ".tar", ".gz", ".bz2", ".xz", ".zst",
        ".pbp", ".prx", ".elf", ".iso", ".cso", ".bin", ".pdf"
    };
    for (size_t i = 0; extension != NULL && i < sizeof(types) / sizeof(types[0]); i++)
        if (!strcasecmp(extension, types[i])) return true;
    return false;
}

static bool attachment(const FetchResult *response)
{
    char value[4097];
    if (!fetch_response_header_value(response, "content-disposition", value, sizeof(value))) return false;
    const char *at = value;
    while (*at == ' ' || *at == '\t') at++;
    return !strncasecmp(at, "attachment", 10)
        && (at[10] == '\0' || at[10] == ';' || at[10] == ' ' || at[10] == '\t');
}

bool file_download_response_candidate(const FetchResult *response)
{
    if (response == NULL || response->status_code != 200) return false;
    if (attachment(response)) return true;
    static const char *const types[] = {"application/octet-stream", "application/zip",
        "application/x-zip-compressed", "application/x-7z-compressed",
        "application/x-rar-compressed", "application/vnd.rar", "application/gzip",
        "application/x-gzip", "application/x-tar", "application/pdf",
        "application/x-psp-pbp", "application/x-executable"};
    size_t length = strcspn(response->content_type, "; \t");
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
        if (strlen(types[i]) == length && !strncasecmp(types[i], response->content_type, length)) return true;
    return false;
}

bool file_download_path(const OfflineLibrary *library, uint32_t id,
                        char *output, size_t capacity)
{
    const OfflineLibraryItem *item = offline_library_find(library, id);
    if (item == NULL || item->type != OFFLINE_ITEM_FILE
        || !file_download_filename_valid(item->title)) return false;
    int length = snprintf(output, capacity, "%s/%08x-%s", library->directory,
                          (unsigned) id, item->title);
    return length > 0 && (size_t) length < capacity;
}

bool file_download_enqueue(OfflineLibrary *library, const char *url,
                           const char *suggested_name, uint32_t *id,
                           char *error, size_t error_size)
{
    char normalized[OFFLINE_LIBRARY_URL_LIMIT], filename[FILE_DOWNLOAD_FILENAME_LIMIT];
    if (library == NULL || !file_download_url_supported(url)
        || !tilefinch_url_request_key(url, normalized, sizeof(normalized))
        || !file_download_filename(normalized, NULL, filename)
        || !offline_library_load(library)) goto refuse;
    if (suggested_name != NULL && *suggested_name != '\0')
        sanitize_filename(suggested_name, filename);
    if (library->count >= OFFLINE_LIBRARY_ITEM_LIMIT) {
        if (error != NULL && error_size != 0)
            snprintf(error, error_size, "Downloads are full; delete a saved item");
        return false;
    }
    uint32_t previous_id = library->next_id;
    OfflineLibraryItem *item = &library->items[library->count++];
    *item = (OfflineLibraryItem) {.id = library->next_id++,
        .type = OFFLINE_ITEM_FILE, .state = OFFLINE_ITEM_QUEUED};
    if (item->id == 0) item->id = library->next_id++;
    snprintf(item->source_url, sizeof(item->source_url), "%s", normalized);
    snprintf(item->title, sizeof(item->title), "%s", filename);
    if (offline_library_save(library)) {
        if (id != NULL) *id = item->id;
        return true;
    }
    memset(item, 0, sizeof(*item));
    library->count--;
    library->next_id = previous_id;
refuse:
    if (error != NULL && error_size != 0)
        snprintf(error, error_size, "File download could not be queued");
    return false;
}

static bool file_size(const char *path, uint64_t *size)
{
    struct stat status;
    if (stat(path, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0)
        return false;
    *size = (uint64_t) status.st_size;
    return true;
}

static bool safe_validator(const char *value)
{
    size_t length = strnlen(value, FILE_VALIDATOR_LIMIT);
    if (length < 3 || length >= FILE_VALIDATOR_LIMIT) return false;
    for (size_t i = 0; i < length; i++)
        if ((unsigned char) value[i] < 32 || (unsigned char) value[i] >= 127)
            return false;
    return (value[0] == 'E' && value[1] == ':' && value[2] == '"'
            && value[length - 1] == '"') || (value[0] == 'M' && value[1] == ':');
}

static bool save_validator(FileDownloadManager *manager)
{
    char path[FILE_DOWNLOAD_PATH_LIMIT], temporary[FILE_DOWNLOAD_PATH_LIMIT];
    if (!offline_library_item_path(manager->library, manager->id, ".file.meta", path, sizeof(path))
        || !offline_library_item_path(manager->library, manager->id, ".file.meta.tmp", temporary, sizeof(temporary))) return false;
    FILE *file = fopen(temporary, "wb");
    if (file == NULL) return false;
    const OfflineLibraryItem *item = offline_library_find(manager->library, manager->id);
    bool okay = item != NULL && fprintf(file, "TFDL01\n%s\n%s\n", item->source_url,
                                        manager->validator) > 0;
    if (fflush(file) != 0 || ferror(file)) okay = false;
    if (fclose(file) != 0) okay = false;
    if (!okay || !tilefinch_platform_replace_file(temporary, path, NULL)) {
        remove(temporary);
        return false;
    }
    return true;
}

static void load_validator(FileDownloadManager *manager)
{
    manager->validator[0] = '\0';
    char path[FILE_DOWNLOAD_PATH_LIMIT];
    if (!offline_library_item_path(manager->library, manager->id, ".file.meta", path, sizeof(path))) return;
    FILE *file = fopen(path, "rb");
    if (file == NULL) return;
    char magic[16] = {0}, url[OFFLINE_LIBRARY_URL_LIMIT + 2u] = {0}, validator[FILE_VALIDATOR_LIMIT + 2u] = {0};
    bool okay = fgets(magic, sizeof(magic), file) != NULL && !strcmp(magic, "TFDL01\n")
        && fgets(url, sizeof(url), file) != NULL
        && fgets(validator, sizeof(validator), file) != NULL
        && fgetc(file) == EOF && !ferror(file);
    fclose(file);
    const OfflineLibraryItem *item = offline_library_find(manager->library, manager->id);
    size_t u = strlen(url), v = strlen(validator);
    if (!okay || item == NULL || u == 0 || v == 0 || url[u - 1] != '\n' || validator[v - 1] != '\n') return;
    url[u - 1] = validator[v - 1] = '\0';
    if (!strcmp(item->source_url, url) && safe_validator(validator))
        memcpy(manager->validator, validator, strlen(validator) + 1u);
}

static void close_output(FileDownloadManager *manager)
{
    if (manager->output != NULL) {
        if (fclose(manager->output) != 0 && manager->error[0] == '\0')
            snprintf(manager->error, sizeof(manager->error), "Memory Stick write failed");
        manager->output = NULL;
    }
}

static void stop(FileDownloadManager *manager, OfflineItemState state, const char *message)
{
    if (manager->request_id != 0) {
        fetch_scheduler_cancel(manager->scheduler, manager->request_id, "file download stopped");
        fetch_scheduler_discard(manager->scheduler, manager->request_id);
        manager->request_id = 0;
    }
    close_output(manager);
    OfflineLibraryItem *item = offline_library_find_mutable(manager->library, manager->id);
    if (message != NULL && message != manager->error)
        snprintf(manager->error, sizeof(manager->error), "%s", message);
    /* A completed download is already persisted as READY; skip a second,
       identical index write. */
    if (item != NULL && (item->state != state
                         || strcmp(item->failure_reason, manager->error) != 0)) {
        item->state = state;
        snprintf(item->failure_reason, sizeof(item->failure_reason), "%s", manager->error);
        offline_library_save(manager->library);
    }
    manager->id = 0;
}

/* A resume of a partial file that already holds every byte is answered
   416. Finish it only when the server names exactly that length for the
   same strong validator; otherwise forget the validator so the next try
   starts from zero. The 416 body is never file data. */
static bool range_not_satisfiable(FileDownloadManager *manager, const FetchResult *response)
{
    char range[128];
    unsigned long long complete = 0;
    char tail;
    const char *validator = manager->validator[0] == 'E' ? response->etag : response->last_modified;
    if (manager->offset != 0 && safe_validator(manager->validator)
        && !strcmp(validator, manager->validator + 2)
        && fetch_response_header_value(response, "content-range", range, sizeof(range))
        && sscanf(range, "bytes */%llu%c", &complete, &tail) == 1
        && complete == manager->offset) {
        manager->resume_complete = true;
        manager->length_known = true;
        manager->total = manager->received = manager->offset;
        return false;
    }
    manager->validator[0] = '\0';
    save_validator(manager);
    snprintf(manager->error, sizeof(manager->error), "Resume could not be verified; retry to restart");
    return false;
}

static bool headers(void *opaque, const FetchResult *response)
{
    FileDownloadManager *manager = opaque;
    OfflineLibraryItem *item = offline_library_find_mutable(manager->library, manager->id);
    char length[64], range[128], encoding[64];
    uint64_t declared = 0, total = 0;
    if (item != NULL && response->status_code == 416 && manager->offset != 0)
        return range_not_satisfiable(manager, response);
    bool known = fetch_response_header_value(response, "content-length", length, sizeof(length));
    if (item == NULL || (response->status_code != 200 && response->status_code != 206)
        || (known && !offline_download_parse_decimal(length, &declared))
        || (fetch_response_header_value(response, "content-encoding", encoding, sizeof(encoding))
            && strcasecmp(encoding, "identity"))
        || (!strncasecmp(response->content_type, "text/html", 9) && !attachment(response))) {
        snprintf(manager->error, sizeof(manager->error), "Server returned no downloadable file (HTTP %ld)", response->status_code);
        return false;
    }
    if (response->status_code == 206) {
        uint64_t first = 0, last = 0, complete = 0;
        const char *validator = manager->validator[0] == 'E' ? response->etag : response->last_modified;
        if (manager->offset == 0 || !safe_validator(manager->validator)
            || strcmp(validator, manager->validator + 2)
            || !fetch_response_header_value(response, "content-range", range, sizeof(range))
            || !offline_download_parse_content_range(range, &first, &last, &complete)
            || first != manager->offset || last < first || complete == 0
            || complete > FILE_DOWNLOAD_LIMIT || last != complete - 1u
            || (known && declared != complete - first)) {
            manager->validator[0] = '\0';
            save_validator(manager);
            snprintf(manager->error, sizeof(manager->error), "Resume could not be verified; retry to restart");
            return false;
        }
        total = complete;
        known = true;
    } else {
        /* An ignored Range or changed If-Range is a fresh complete response,
           never bytes to append to the existing partial file. */
        manager->offset = manager->received = 0;
        total = declared;
        manager->validator[0] = '\0';
        if (response->etag[0] == '"' && strlen(response->etag) + 2 < sizeof(manager->validator))
        {
            memcpy(manager->validator, "E:", 2);
            memcpy(manager->validator + 2, response->etag, strlen(response->etag) + 1u);
        }
        else if (response->last_modified[0] != '\0'
                 && strlen(response->last_modified) + 2 < sizeof(manager->validator))
        {
            memcpy(manager->validator, "M:", 2);
            memcpy(manager->validator + 2, response->last_modified, strlen(response->last_modified) + 1u);
        }
        if (!safe_validator(manager->validator)) manager->validator[0] = '\0';
        char disposition[4097], filename[FILE_DOWNLOAD_FILENAME_LIMIT];
        bool have = fetch_response_header_value(response, "content-disposition", disposition, sizeof(disposition));
        if (have && file_download_filename(item->source_url, disposition, filename))
            snprintf(item->title, sizeof(item->title), "%s", filename);
    }
    if ((known && total > FILE_DOWNLOAD_LIMIT)
        || !tilefinch_update_query_free_space(manager->library->directory, &manager->available)
        || !offline_download_space_admits(manager->available, known ? total - manager->offset : 0)) {
        snprintf(manager->error, sizeof(manager->error), "File is too large or Memory Stick space is insufficient");
        return false;
    }
    manager->length_known = known;
    manager->total = total;
    item->content_bytes = known ? total : FILE_DOWNLOAD_LIMIT;
    item->downloaded_bytes = manager->offset;
    char path[FILE_DOWNLOAD_PATH_LIMIT], final[FILE_DOWNLOAD_PATH_LIMIT];
    struct stat exists;
    if (!file_download_path(manager->library, manager->id, final, sizeof(final))
        || stat(final, &exists) == 0
        || !offline_library_item_path(manager->library, manager->id, ".file.part", path, sizeof(path))
        || !offline_library_save(manager->library)) {
        snprintf(manager->error, sizeof(manager->error), "Download metadata or destination is unavailable");
        return false;
    }
    /* Truncate a stale partial file before persisting a new validator: a
       validator must never describe bytes of a different entity. */
    manager->output = fopen(path, manager->offset != 0 ? "ab" : "wb");
    if (manager->output == NULL) {
        snprintf(manager->error, sizeof(manager->error), "Download file could not be opened on the Memory Stick");
        return false;
    }
    if (!save_validator(manager)) {
        close_output(manager);
        snprintf(manager->error, sizeof(manager->error), "Download metadata or destination is unavailable");
        return false;
    }
    manager->headers_valid = true;
    return true;
}

static bool body(void *opaque, const unsigned char *data, size_t length)
{
    FileDownloadManager *manager = opaque;
    if (!manager->headers_valid || manager->output == NULL
        || manager->received > FILE_DOWNLOAD_LIMIT || length > FILE_DOWNLOAD_LIMIT - manager->received
        || (manager->length_known && (manager->received > manager->total || length > manager->total - manager->received))
        || !offline_download_space_admits(manager->available, length)
        || fwrite(data, 1, length, manager->output) != length) {
        snprintf(manager->error, sizeof(manager->error), "Memory Stick write failed or file exceeded its limit");
        return false;
    }
    manager->received += length;
    manager->available -= length;
    OfflineLibraryItem *item = offline_library_find_mutable(manager->library, manager->id);
    if (item != NULL) item->downloaded_bytes = manager->received;
    offline_download_speed_sample(&manager->speed, manager->received);
    return true;
}

FileDownloadManager *file_download_create(Budget *budget, BrowserSession *session, OfflineLibrary *library)
{
    if (budget == NULL || session == NULL || library == NULL) return NULL;
    FileDownloadManager *manager = budget_calloc_category(budget, BUDGET_CATEGORY_SESSION, 1, sizeof(*manager));
    if (manager != NULL) {
        manager->budget = budget; manager->session = session; manager->library = library;
    }
    return manager;
}

bool file_download_start(FileDownloadManager *manager, uint32_t id)
{
    if (manager == NULL || manager->id != 0) return false;
    OfflineLibraryItem *item = offline_library_find_mutable(manager->library, id);
    if (item == NULL || item->type != OFFLINE_ITEM_FILE || item->state == OFFLINE_ITEM_READY
        || !file_download_url_supported(item->source_url)) return false;
    if (manager->scheduler == NULL) {
        manager->scheduler = fetch_scheduler_create(manager->budget, 1, FILE_DOWNLOAD_LIMIT);
        if (manager->scheduler == NULL) return false;
        fetch_scheduler_enable_background_transport(manager->scheduler, true);
    }
    if (manager->completion == NULL) {
        manager->completion = budget_calloc_category(manager->budget, BUDGET_CATEGORY_RESOURCE, 1, sizeof(FetchResult));
        if (manager->completion == NULL) return false;
        manager->completion->budget = manager->budget;
    }
    manager->id = id;
    manager->error[0] = '\0';
    manager->offset = 0;
    manager->headers_valid = false;
    manager->length_known = false;
    manager->resume_complete = false;
    manager->total = manager->available = 0;
    char path[FILE_DOWNLOAD_PATH_LIMIT];
    load_validator(manager);
    if (safe_validator(manager->validator)
        && offline_library_item_path(manager->library, id, ".file.part", path, sizeof(path)))
        file_size(path, &manager->offset);
    if (manager->offset >= FILE_DOWNLOAD_LIMIT) manager->offset = 0;
    manager->received = manager->offset;
    offline_download_speed_reset(&manager->speed, manager->offset);
    char extra[256] = {0};
    if (manager->offset != 0)
        snprintf(extra, sizeof(extra), "Range: bytes=%llu-\nIf-Range: %s",
                 (unsigned long long) manager->offset, manager->validator + 2);
    FetchRequest transport = {.method = "GET", .allow_http_errors = true,
        .identity_encoding = true, .accept = "*/*", .extra_headers = extra[0] ? extra : NULL,
        .stall_abort_seconds = 15, .user_agent = TILEFINCH_BROWSER_USER_AGENT,
        .redirect_url_validator = file_download_url_supported,
        .credentials = FETCH_CREDENTIALS_INCLUDE,
        .sec_fetch_dest = "document", .sec_fetch_mode = "navigate",
        .sec_fetch_site = "none", .sec_fetch_user = true,
        .credential_origin = item->source_url};
    TilefinchRequestContext context = {.target_url = item->source_url, .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_NAVIGATE, .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_DOCUMENT, .top_level_navigation = true, .user_activated = true};
    /* This is an explicitly confirmed native service, not a page fetch.
       Keep its HTTPS trust boundary on every hop and use the existing
       context-aware cookie snapshot, which never forwards origin credentials
       to a different host. No cookie is written into download metadata. */
    char cookies[BROWSER_COOKIE_HEADER_CAPACITY] = {0};
    if (!browser_session_cookie_header_context(manager->session, &context, cookies, sizeof(cookies))) {
        stop(manager, OFFLINE_ITEM_PAUSED, "Download request failed its security policy");
        return false;
    }
    transport.cookie = cookies;
    transport.cookie_session = manager->session;
    transport.cookie_context = &context;
    FetchStreamOptions stream = {.on_headers = headers, .on_body = body, .opaque = manager, .chunk_bytes = 16384};
    manager->request_id = fetch_scheduler_enqueue_stream(manager->scheduler, item->source_url,
        &transport, FILE_DOWNLOAD_LIMIT, 600000, &stream);
    if (manager->request_id == 0) {
        stop(manager, OFFLINE_ITEM_PAUSED, "Download could not start");
        return false;
    }
    item->state = OFFLINE_ITEM_DOWNLOADING;
    if (!offline_library_save(manager->library)) {
        stop(manager, OFFLINE_ITEM_PAUSED, "Download state could not be saved");
        return false;
    }
    return true;
}

bool file_download_pump(FileDownloadManager *manager)
{
    if (manager == NULL || manager->id == 0) return false;
    if (offline_library_find(manager->library, manager->id) == NULL) {
        stop(manager, OFFLINE_ITEM_PAUSED, "Download was removed");
        return true;
    }
    offline_download_pump_transfer(manager->scheduler);
    bool success = false;
    FetchStreamMetrics metrics;
    if (!fetch_scheduler_take_stream(manager->scheduler, manager->request_id, &success, &metrics, manager->completion)) return false;
    manager->request_id = 0;
    bool okay = manager->resume_complete
        || (success && manager->headers_valid && manager->output != NULL
            && (!manager->length_known || manager->received == manager->total)
            && metrics.bytes_received == manager->received - manager->offset);
    if (manager->output != NULL && fflush(manager->output) != 0) okay = false;
    close_output(manager);
    if (manager->error[0] != '\0') okay = false;
    if (!okay && manager->error[0] == '\0')
        /* Transport errors may embed signed URLs. Keep persisted/user-facing
           failure reasons fixed and credential-free. */
        snprintf(manager->error, sizeof(manager->error), "Download was interrupted; retry or cancel");
    fetch_result_destroy(manager->completion);
    manager->completion->budget = manager->budget;
    if (okay) {
        /* Persist the completed length before publishing. Recovery can finish
           this rename after power loss; a failed final index write must never
           strand a completed file behind a downloading record. */
        OfflineLibraryItem *item = offline_library_find_mutable(manager->library, manager->id);
        item->state = OFFLINE_ITEM_READY;
        item->failure_reason[0] = '\0';
        item->content_bytes = item->downloaded_bytes = manager->received;
        okay = offline_library_save(manager->library);
        if (!okay) snprintf(manager->error, sizeof(manager->error), "Completed download state could not be saved");
    }
    if (okay) {
        char part[FILE_DOWNLOAD_PATH_LIMIT], final[FILE_DOWNLOAD_PATH_LIMIT];
        struct stat exists;
        okay = offline_library_item_path(manager->library, manager->id, ".file.part", part, sizeof(part))
            && file_download_path(manager->library, manager->id, final, sizeof(final))
            && stat(final, &exists) != 0 && errno == ENOENT && rename(part, final) == 0;
        if (!okay) snprintf(manager->error, sizeof(manager->error), "File could not be published without overwriting an existing file");
    }
    stop(manager, okay ? OFFLINE_ITEM_READY : OFFLINE_ITEM_PAUSED, NULL);
    return true;
}

bool file_download_pause(FileDownloadManager *manager, uint32_t id)
{
    if (manager == NULL || id == 0 || manager->id != id) return false;
    stop(manager, OFFLINE_ITEM_PAUSED, NULL);
    return true;
}

bool file_download_active(const FileDownloadManager *manager, uint32_t *id)
{
    if (manager == NULL || manager->id == 0) return false;
    if (id != NULL) *id = manager->id;
    return true;
}

bool file_download_progress(const FileDownloadManager *manager, uint32_t id,
                            uint64_t *total_bytes, uint64_t *available_bytes,
                            uint32_t *bytes_per_second)
{
    if (manager == NULL || id == 0 || manager->id != id) return false;
    *total_bytes = manager->length_known ? manager->total : 0;
    *available_bytes = manager->available;
    *bytes_per_second = manager->speed.bytes_per_second;
    return true;
}

void file_download_destroy(FileDownloadManager *manager)
{
    if (manager == NULL) return;
    if (manager->id != 0) file_download_pause(manager, manager->id);
    if (manager->scheduler != NULL) fetch_scheduler_destroy(manager->scheduler);
    if (manager->completion != NULL) {
        fetch_result_destroy(manager->completion);
        budget_free(manager->budget, manager->completion);
    }
    budget_free(manager->budget, manager);
}
