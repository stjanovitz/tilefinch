#include "tilefinch/file_download.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* unicode_normalize; libunicode.h needs cutils.h first. */
#include "cutils.h"
#include "libunicode.h"

#include "tilefinch/font.h"
#include "tilefinch/platform.h"
#include "tilefinch/update.h"
#include "tilefinch/url.h"
#include "tilefinch/user_agent.h"
#include "tilefinch/utf8_encode.h"

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

/* A downloaded file has two names. Its title, kept in the library index and
   shown by every list and question, is the server's name as UTF-8: a
   basename, normalized to NFC, with control, bidi-override and FAT-reserved
   characters replaced by '_'. Its Memory Stick name is that title with each
   non-ASCII byte percent-encoded, cut to fit with the extension kept, so the
   firmware's FAT layer only ever sees printable ASCII; the item's ID prefix
   keeps every file's name unique. A title of printable ASCII, which is every
   title saved before titles kept other characters, is its own Memory Stick
   name. Neither name ever changes the downloaded bytes. */

#define FILE_NAME_POINT_LIMIT 256u
/* NFC of at most FILE_NAME_POINT_LIMIT code points, decomposed (at most a
   few code points each) in one growing buffer. */
#define FILE_NAME_NORMALIZE_ARENA 6144u

static size_t name_utf8_decode(const unsigned char *text, size_t available,
                               uint32_t *codepoint)
{
    unsigned char lead = text[0];
    if (lead < 0x80u) { *codepoint = lead; return 1; }
    size_t length;
    uint32_t value, minimum;
    if (lead >= 0xc2u && lead <= 0xdfu) { length = 2; value = lead & 0x1fu; minimum = 0x80u; }
    else if ((lead & 0xf0u) == 0xe0u) { length = 3; value = lead & 0x0fu; minimum = 0x800u; }
    else if (lead >= 0xf0u && lead <= 0xf4u) { length = 4; value = lead & 0x07u; minimum = 0x10000u; }
    else return 0;
    if (available < length) return 0;
    for (size_t at = 1; at < length; at++) {
        if ((text[at] & 0xc0u) != 0x80u) return 0;
        value = (value << 6) | (text[at] & 0x3fu);
    }
    if (value < minimum || value > 0x10ffffu
        || (value >= 0xd800u && value <= 0xdfffu)) return 0;
    *codepoint = value;
    return length;
}

/* Controls, FAT's reserved characters, and invisible characters that would
   let a name draw as another (an override turning "gpj.exe" into "exe.jpg"). */
static bool name_character_refused(uint32_t c)
{
    return c < 0x20u || (c >= 0x7fu && c <= 0x9fu)
        || (c < 0x80u && strchr("<>:\"|?*/\\", (int) c) != NULL)
        || c == 0x061cu || c == 0x200eu || c == 0x200fu
        || (c >= 0x202au && c <= 0x202eu) || (c >= 0x2066u && c <= 0x2069u)
        || c == 0x2028u || c == 0x2029u || c == 0xfeffu
        || (c >= 0xfdd0u && c <= 0xfdefu) || (c & 0xfffeu) == 0xfffeu;
}

/* Continues the character before it: a mark, joiner, variation selector,
   skin tone or tag. A cut never separates one from its base. */
static bool name_character_extends(uint32_t c)
{
    return font_codepoint_combining_mark(c) || c == 0x200du
        || (c >= 0xfe00u && c <= 0xfe0fu) || (c >= 0x1f3fbu && c <= 0x1f3ffu)
        || (c >= 0xe0020u && c <= 0xe007fu) || (c >= 0xe0100u && c <= 0xe01efu);
}

/* ".zip": the last dot and one to five ASCII letters or digits after it,
   the extension a cut keeps and the file-name row keeps in view. */
static size_t name_extension_length(const char *name, size_t length)
{
    for (size_t count = 1; count <= 6u && count < length; count++) {
        unsigned char c = (unsigned char) name[length - count];
        if (c == '.') return count > 1u ? count : 0;
        if (!((c >= '0' && c <= '9') || ((c | 0x20u) >= 'a' && (c | 0x20u) <= 'z')))
            return 0;
    }
    return 0;
}

typedef struct {
    unsigned char *data;
    size_t used, capacity;
    unsigned char *last;
} NameArena;

/* unicode_normalize's only buffer grows in place; anything else fails, and
   the name keeps its unnormalized form. */
static void *name_arena_realloc(void *opaque, void *pointer, size_t size)
{
    NameArena *arena = opaque;
    if (size == 0) return NULL;
    if (pointer == NULL) {
        size_t start = (arena->used + 7u) & ~(size_t) 7u;
        if (start > arena->capacity || size > arena->capacity - start) return NULL;
        arena->last = arena->data + start;
        arena->used = start + size;
        return arena->last;
    }
    if (pointer != arena->last) return NULL;
    size_t start = (size_t) (arena->last - arena->data);
    if (size > arena->capacity - start) return NULL;
    arena->used = start + size;
    return pointer;
}

static bool name_ascii_valid(const char *name)
{
    size_t length = strnlen(name, FILE_DOWNLOAD_FILENAME_LIMIT);
    if (length == 0 || length >= FILE_DOWNLOAD_FILENAME_LIMIT || name[0] == '.'
        || name[length - 1] == '.' || name[length - 1] == ' ') return false;
    for (size_t at = 0; at < length; at++) {
        unsigned char c = (unsigned char) name[at];
        if (c < 32 || c >= 127 || strchr("<>:\"|?*/\\", c)) return false;
    }
    size_t stem = strcspn(name, ".");
    return !((stem == 3 && (!strncasecmp(name, "CON", 3)
        || !strncasecmp(name, "PRN", 3) || !strncasecmp(name, "AUX", 3)
        || !strncasecmp(name, "NUL", 3)))
        || (stem == 4 && (!strncasecmp(name, "COM", 3)
            || !strncasecmp(name, "LPT", 3)) && name[3] >= '1' && name[3] <= '9'));
}

static void sanitize_filename(const char *input,
                               char output[FILE_DOWNLOAD_FILENAME_LIMIT])
{
    const char *base = input;
    for (const char *at = input; *at != '\0'; at++)
        if (*at == '/' || *at == '\\') base = at + 1;
    size_t length = strlen(base);
    size_t extension = name_extension_length(base, length);
    size_t stem_bytes = length - extension;
    uint32_t points[FILE_NAME_POINT_LIMIT];
    size_t count = 0;
    for (size_t at = 0; at < stem_bytes && count < FILE_NAME_POINT_LIMIT;) {
        uint32_t c = '_';
        size_t step = name_utf8_decode((const unsigned char *) base + at,
                                       stem_bytes - at, &c);
        if (step == 0) { c = '_'; step = 1; }
        else if (name_character_refused(c)) c = '_';
        points[count++] = c;
        at += step;
    }
    bool truncated = count == FILE_NAME_POINT_LIMIT;
    unsigned char arena_bytes[FILE_NAME_NORMALIZE_ARENA];
    NameArena arena = {.data = arena_bytes, .capacity = sizeof(arena_bytes)};
    uint32_t *normal = NULL;
    int normal_count = count == 0 ? -1 : unicode_normalize(&normal, points,
        (int) count, UNICODE_NFC, &arena, name_arena_realloc);
    const uint32_t *stem = normal_count >= 0 && (size_t) normal_count <= count
        ? normal : points;
    if (stem == normal) count = (size_t) normal_count;
    /* Fit the stem in front of the extension, then back the cut up to a
       character that starts a cluster. */
    size_t room = FILE_DOWNLOAD_FILENAME_LIMIT - 1u - extension, used = 0, kept = 0;
    unsigned char bytes[4];
    while (kept < count) {
        size_t width = tilefinch_utf8_encode(stem[kept], bytes);
        if (width == 0 || width > room - used) break;
        used += width;
        kept++;
    }
    if (kept < count || truncated) {
        while (kept > 0 && kept < count && name_character_extends(stem[kept])) kept--;
        /* A joiner left at the end gives up the emoji it joined. */
        while (kept > 0 && stem[kept - 1u] == 0x200du) {
            kept--;
            while (kept > 0 && name_character_extends(stem[kept - 1u])) kept--;
            if (kept > 0) kept--;
        }
        size_t flags = 0;
        while (flags < kept && stem[kept - 1u - flags] >= 0x1f1e6u
               && stem[kept - 1u - flags] <= 0x1f1ffu) flags++;
        if (flags % 2u != 0 && kept < count && stem[kept] >= 0x1f1e6u
            && stem[kept] <= 0x1f1ffu) kept--;
    }
    used = 0;
    for (size_t at = 0; at < kept; at++) {
        size_t width = tilefinch_utf8_encode(stem[at], bytes);
        memcpy(output + used, bytes, width);
        used += width;
    }
    memcpy(output + used, base + stem_bytes, extension);
    used += extension;
    while (used != 0 && (output[used - 1] == '.' || output[used - 1] == ' ')) used--;
    output[used] = '\0';
    if (used == 0 || output[0] == '.') {
        snprintf(output, FILE_DOWNLOAD_FILENAME_LIMIT, "download.bin");
        return;
    }
    size_t stem_length = strcspn(output, ".");
    bool reserved = (stem_length == 3 && (!strncasecmp(output, "CON", 3)
        || !strncasecmp(output, "PRN", 3) || !strncasecmp(output, "AUX", 3)
        || !strncasecmp(output, "NUL", 3)))
        || (stem_length == 4 && (!strncasecmp(output, "COM", 3)
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

bool file_download_stored_name_valid(const char *name)
{
    return name != NULL && name_ascii_valid(name);
}

bool file_download_stored_name(const char *name,
                               char output[FILE_DOWNLOAD_FILENAME_LIMIT])
{
    if (output == NULL || !file_download_filename_valid(name)) return false;
    static const char hex[] = "0123456789ABCDEF";
    size_t length = strlen(name);
    size_t extension = name_extension_length(name, length);
    size_t stem = length - extension;
    size_t room = FILE_DOWNLOAD_FILENAME_LIMIT - 1u - extension, used = 0;
    bool cut = false;
    for (size_t at = 0; at < stem;) {
        size_t step = 1;
        while (at + step < stem && ((unsigned char) name[at + step] & 0xc0u) == 0x80u)
            step++;
        bool ascii = (unsigned char) name[at] < 0x80u;
        size_t width = ascii ? 1u : 3u * step;
        if (width > room - used) { cut = true; break; }
        for (size_t byte = 0; byte < step; byte++) {
            unsigned char c = (unsigned char) name[at + byte];
            if (ascii) { output[used++] = (char) c; continue; }
            output[used++] = '%';
            output[used++] = hex[c >> 4];
            output[used++] = hex[c & 15u];
        }
        at += step;
    }
    while (cut && used != 0 && (output[used - 1] == '.' || output[used - 1] == ' '))
        used--;
    memcpy(output + used, name + stem, extension);
    used += extension;
    output[used] = '\0';
    return name_ascii_valid(output);
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

/* The decoded last path segment, empty for a path ending in '/'. */
static bool url_path_basename(const char *url, char output[1024])
{
    TilefinchUrl parsed;
    if (!tilefinch_url_parse(url, &parsed)) return false;
    size_t first = parsed.path_offset, end = first + parsed.path_length;
    for (size_t i = first; i < end; i++) if (url[i] == '/') first = i + 1;
    size_t size = end - first;
    if (size >= 1024) return false;
    memcpy(output, url + first, size);
    output[size] = '\0';
    if (!tilefinch_url_percent_decode(output)) output[0] = '\0';
    return true;
}

bool file_download_filename(const char *url, const char *disposition,
                            char output[FILE_DOWNLOAD_FILENAME_LIMIT])
{
    if (output == NULL || !file_download_url_supported(url)) return false;
    char candidate[1024];
    if (!disposition_name(disposition, true, candidate)
        && !disposition_name(disposition, false, candidate)
        && !url_path_basename(url, candidate)) return false;
    sanitize_filename(candidate, output);
    return true;
}

bool file_download_url_candidate(const char *url)
{
    /* Judge the URL's own last segment. The saved-file name falls back to
       download.bin for a path such as /settings/, and that fallback is not
       evidence of a file: every such link would offer a download. */
    char name[1024];
    if (!file_download_url_supported(url) || !url_path_basename(url, name))
        return false;
    const char *extension = strrchr(name, '.');
    if (extension == name) return false; /* ".bin" is no file name */
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
    char stored[FILE_DOWNLOAD_FILENAME_LIMIT];
    if (item == NULL || item->type != OFFLINE_ITEM_FILE
        || !file_download_stored_name(item->title, stored)) return false;
    int length = snprintf(output, capacity, "%s/%08x-%s", library->directory,
                          (unsigned) id, stored);
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
    bool okay = item != NULL && fputs("TFDL01\n", file) >= 0
        && fputs(item->source_url, file) >= 0 && fputc('\n', file) != EOF
        && fputs(manager->validator, file) >= 0 && fputc('\n', file) != EOF;
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
        if (have && file_download_filename(item->source_url, disposition, filename)) {
            /* The Downloads row shows this title. */
            bool held = offline_library_fence_acquire(manager->library);
            snprintf(item->title, sizeof(item->title), "%s", filename);
            offline_library_fence_release(manager->library, held);
        }
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
