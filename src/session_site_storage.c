#include "session_site_storage.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "tilefinch/budget.h"
#include "tilefinch/url.h"
#include "tilefinch_test_faults.h"

#define budget_malloc(b, s) budget_malloc_category((b), BUDGET_CATEGORY_SESSION, (s))
#define budget_calloc(b, n, s) budget_calloc_category((b), BUDGET_CATEGORY_SESSION, (n), (s))
#define budget_realloc(b, p, s) budget_realloc_category((b), BUDGET_CATEGORY_SESSION, (p), (s))

/*
 * One store holds every origin's localStorage, sessionStorage and OPFS
 * entries as items in insertion order. A site record carries the origin's
 * tier and byte count:
 *
 *   MEMORY          values live in the item's heap block (key NUL value NUL)
 *   STICK_SESSION   values live in a log file named for the record slot,
 *                   deleted at exit (and at the next boot after a crash)
 *   STICK_ALWAYS    values live in a log file named for the origin's hash,
 *                   loaded on the site's first use each boot
 *
 * A Memory Stick file is a header followed by append-only records:
 *
 *   header  "TFSS" u8 version, u8 0, u16 origin length, origin
 *   record  u8 kind, u8 op, u16 key length, u32 value length,
 *           i64 modified ms, u32 value checksum, u32 header checksum,
 *           key, value
 *
 * The header checksum covers the first twenty bytes and the key, so the
 * index loads without reading values; a value is checked when read. A bad
 * or truncated record ends the load, and the next append overwrites it.
 * sessionStorage records are skipped when an always-kept file loads. Only
 * the index (keys and offsets) is resident for a stick site.
 */

#define STICK_VERSION 1u
#define STICK_HEADER_FIXED 8u
#define STICK_RECORD_HEADER 24u
#define STICK_OPEN_FILES 4u
#define STICK_PATH_BYTES (BROWSER_SITE_STORAGE_DIRECTORY_LIMIT + 32u)
/* Rewrite a log once its dead records are this large and at least as large
   as the site's live data. (Comparing with half the file never fires for a
   page that rewrites everything once: dead then falls short of half by the
   header bytes.) */
#define STICK_COMPACT_DEAD_BYTES (256u * 1024u)
#define STICK_LOAD_COMPACT_DEAD_BYTES (64u * 1024u)
#define STICK_SESSION_MARKER "session-files"
#define SCRATCH_RETAINED_BYTES (16u * 1024u)

enum {
    ITEM_LOCAL = 1,
    ITEM_SESSION = 2,
    ITEM_OPFS_FILE = 3,
    ITEM_OPFS_DIRECTORY = 4
};

enum {
    RECORD_SET = 1,
    RECORD_REMOVE = 2
};

typedef struct {
    char *key;              /* key NUL; in RAM followed by value NUL */
    size_t key_length;
    size_t value_length;
    size_t record_offset;   /* on the stick: the SET record's offset */
    uint32_t value_checksum;
    int64_t modified_ms;
    uint64_t generation;
    uint8_t site;
    uint8_t kind;
} SiteItem;

typedef struct {
    char origin[BROWSER_ORIGIN_LIMIT];
    BrowserSiteStorageTier tier;
    BrowserSiteStoragePolicy policy;
    size_t bytes;
    size_t allowance;       /* RAM tier */
    size_t item_count;
    size_t file_bytes;
    size_t dead_bytes;
    uint64_t file_stamp;
    FILE *file;
    bool used;
    bool loaded;            /* stick tier: the index is resident */
    bool offer_pending;     /* requested and not yet answered */
    bool declined;          /* the user said no this session */
} SiteRecord;

struct BrowserSiteStore {
    SiteRecord sites[BROWSER_SITE_STORAGE_SITES];
    SiteItem *items;
    size_t item_count;
    size_t item_capacity;
    size_t memory_bytes;    /* all RAM-tier sites */
    uint64_t file_clock;
    unsigned char *scratch;
    size_t scratch_capacity;
    char directory[BROWSER_SITE_STORAGE_DIRECTORY_LIMIT];
    bool configured;
    bool offers;
    bool session_marker;
    bool request_ready;
    BrowserSiteStorageRequest request;
};

static uint32_t fnv32(uint32_t hash, const void *data, size_t length)
{
    const unsigned char *bytes = data;
    for (size_t i = 0; i < length; i++) {
        hash ^= bytes[i];
        hash *= UINT32_C(16777619);
    }
    return hash;
}

static uint64_t fnv64(const char *text)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char *at = (const unsigned char *) text;
         *at != '\0'; at++) {
        hash ^= *at;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void put_u16(unsigned char *out, uint16_t value)
{
    out[0] = (unsigned char) value;
    out[1] = (unsigned char) (value >> 8);
}

static void put_u32(unsigned char *out, uint32_t value)
{
    for (int i = 0; i < 4; i++) out[i] = (unsigned char) (value >> (8 * i));
}

static void put_u64(unsigned char *out, uint64_t value)
{
    for (int i = 0; i < 8; i++) out[i] = (unsigned char) (value >> (8 * i));
}

static uint16_t get_u16(const unsigned char *in)
{
    return (uint16_t) (in[0] | (in[1] << 8));
}

static uint32_t get_u32(const unsigned char *in)
{
    uint32_t value = 0;
    for (int i = 3; i >= 0; i--) value = (value << 8) | in[i];
    return value;
}

static uint64_t get_u64(const unsigned char *in)
{
    uint64_t value = 0;
    for (int i = 7; i >= 0; i--) value = (value << 8) | in[i];
    return value;
}

static bool same_class(uint8_t a, uint8_t b)
{
    if (a >= ITEM_OPFS_FILE) return b >= ITEM_OPFS_FILE;
    return a == b;
}

static size_t item_bytes(const SiteItem *item)
{
    return item->key_length + item->value_length;
}

static size_t record_bytes(size_t key_length, size_t value_length)
{
    return STICK_RECORD_HEADER + key_length + value_length;
}

static bool stick_tier(const SiteRecord *site)
{
    return site->tier != BROWSER_SITE_STORAGE_MEMORY;
}

static int64_t modified_now(void)
{
    int64_t seconds = (int64_t) time(NULL);
    if (seconds <= 0 || seconds > INT64_MAX / 1000) return 0;
    return seconds * 1000;
}

static uint64_t next_generation(BrowserSession *session)
{
    if (++session->site_storage_generation == 0)
        session->site_storage_generation = 1;
    return session->site_storage_generation;
}

/* ---- store and site records ------------------------------------------ */

static struct BrowserSiteStore *store_create(BrowserSession *session)
{
    if (session->site_store == NULL) {
        session->site_store = budget_calloc(
            session->budget, 1, sizeof(*session->site_store));
        if (session->site_store != NULL) session->site_store->offers = true;
    }
    return session->site_store;
}

static void site_reset(SiteRecord *site)
{
    memset(site, 0, sizeof(*site));
}

static void scratch_trim(BrowserSession *session,
                         struct BrowserSiteStore *store)
{
    if (store->scratch_capacity <= SCRATCH_RETAINED_BYTES) return;
    budget_free(session->budget, store->scratch);
    store->scratch = NULL;
    store->scratch_capacity = 0;
}

/* An unconfigured store with nothing in it is released, so a session
   whose pages stop using storage returns to paying one pointer. */
static void store_trim(BrowserSession *session)
{
    struct BrowserSiteStore *store = session->site_store;
    if (store == NULL) return;
    scratch_trim(session, store);
    if (store->item_count == 0 && store->items != NULL) {
        budget_free(session->budget, store->items);
        store->items = NULL;
        store->item_capacity = 0;
    }
    if (store->configured || store->item_count != 0) return;
    for (size_t i = 0; i < BROWSER_SITE_STORAGE_SITES; i++)
        if (store->sites[i].used) return;
    budget_free(session->budget, store->scratch);
    budget_free(session->budget, store);
    session->site_store = NULL;
}

static int site_find(const struct BrowserSiteStore *store,
                     const char *origin)
{
    if (store == NULL) return -1;
    for (size_t i = 0; i < BROWSER_SITE_STORAGE_SITES; i++) {
        if (store->sites[i].used
            && strcmp(store->sites[i].origin, origin) == 0) return (int) i;
    }
    return -1;
}

static int site_acquire(struct BrowserSiteStore *store, const char *origin)
{
    int found = site_find(store, origin);
    if (found >= 0) return found;
    for (size_t i = 0; i < BROWSER_SITE_STORAGE_SITES; i++) {
        SiteRecord *site = &store->sites[i];
        if (site->used) continue;
        site_reset(site);
        snprintf(site->origin, sizeof(site->origin), "%s", origin);
        site->used = true;
        site->loaded = true;
        site->allowance = BROWSER_SITE_STORAGE_INITIAL_BYTES;
        return (int) i;
    }
    return -1;
}

static void site_release_if_idle(struct BrowserSiteStore *store,
                                 size_t index)
{
    SiteRecord *site = &store->sites[index];
    if (site->used && site->tier == BROWSER_SITE_STORAGE_MEMORY
        && site->policy == BROWSER_SITE_STORAGE_ASK
        && site->item_count == 0 && site->file == NULL
        && !site->offer_pending && !site->declined) site_reset(site);
}

static void site_bytes_change(struct BrowserSiteStore *store, size_t index,
                              size_t old_bytes, size_t new_bytes)
{
    SiteRecord *site = &store->sites[index];
    site->bytes = site->bytes - old_bytes + new_bytes;
    if (site->tier == BROWSER_SITE_STORAGE_MEMORY)
        store->memory_bytes = store->memory_bytes - old_bytes + new_bytes;
}

static unsigned char *scratch_reserve(BrowserSession *session,
                                      struct BrowserSiteStore *store,
                                      size_t length)
{
    if (length == SIZE_MAX) return NULL;
    if (store->scratch_capacity < length + 1u) {
        unsigned char *grown = budget_realloc(
            session->budget, store->scratch, length + 1u);
        if (grown == NULL) return NULL;
        store->scratch = grown;
        store->scratch_capacity = length + 1u;
    }
    return store->scratch;
}

/* ---- items ------------------------------------------------------------ */

static bool items_reserve(BrowserSession *session,
                          struct BrowserSiteStore *store, size_t extra)
{
    if (store->item_count + extra <= store->item_capacity) return true;
    if (store->item_count + extra > BROWSER_SITE_STORAGE_ITEMS) return false;
    size_t capacity = store->item_capacity == 0 ? 16u
                                                : store->item_capacity * 2u;
    while (capacity < store->item_count + extra) capacity *= 2u;
    if (capacity > BROWSER_SITE_STORAGE_ITEMS)
        capacity = BROWSER_SITE_STORAGE_ITEMS;
    SiteItem *grown = budget_realloc(
        session->budget, store->items, capacity * sizeof(*grown));
    if (grown == NULL) return false;
    store->items = grown;
    store->item_capacity = capacity;
    return true;
}

static int item_find(const struct BrowserSiteStore *store, size_t site,
                     uint8_t kind, const char *key)
{
    size_t length = strlen(key);
    for (size_t i = 0; i < store->item_count; i++) {
        const SiteItem *item = &store->items[i];
        if (item->site == site && same_class(item->kind, kind)
            && item->key_length == length
            && memcmp(item->key, key, length) == 0) return (int) i;
    }
    return -1;
}

/* Drops the item from the index; the caller has logged any removal. */
static void item_drop(BrowserSession *session,
                      struct BrowserSiteStore *store, size_t at)
{
    SiteItem *item = &store->items[at];
    size_t site = item->site;
    site_bytes_change(store, site, item_bytes(item), 0);
    store->sites[site].item_count--;
    budget_free(session->budget, item->key);
    memmove(item, item + 1,
            (store->item_count - at - 1u) * sizeof(*item));
    store->item_count--;
}

static char *memory_block(BrowserSession *session, const char *key,
                          size_t key_length, const unsigned char *value,
                          size_t value_length, bool with_value)
{
    size_t size = key_length + 1u;
    if (with_value) size += value_length + 1u;
    char *block = budget_malloc(session->budget, size);
    if (block == NULL) return NULL;
    memcpy(block, key, key_length);
    block[key_length] = '\0';
    if (with_value) {
        if (value_length != 0)
            memcpy(block + key_length + 1u, value, value_length);
        block[key_length + 1u + value_length] = '\0';
    }
    return block;
}

/* ---- Memory Stick files ------------------------------------------------ */

static bool stick_path(const struct BrowserSiteStore *store, size_t index,
                       BrowserSiteStorageTier tier,
                       char path[STICK_PATH_BYTES])
{
    int written = tier == BROWSER_SITE_STORAGE_STICK_ALWAYS
        ? snprintf(path, STICK_PATH_BYTES, "%s/p-%016llx", store->directory,
                   (unsigned long long) fnv64(store->sites[index].origin))
        : snprintf(path, STICK_PATH_BYTES, "%s/s-%02u", store->directory,
                   (unsigned) index);
    return written > 0 && (size_t) written < STICK_PATH_BYTES;
}

static bool temporary_path(const char *path, char out[STICK_PATH_BYTES])
{
    int written = snprintf(out, STICK_PATH_BYTES, "%s.tmp", path);
    return written > 0 && (size_t) written < STICK_PATH_BYTES;
}

/* The original log while a compacted one is put in its place. */
static bool backup_path(const char *path, char out[STICK_PATH_BYTES])
{
    int written = snprintf(out, STICK_PATH_BYTES, "%s.bak", path);
    return written > 0 && (size_t) written < STICK_PATH_BYTES;
}

static void stick_close(SiteRecord *site)
{
    if (site->file != NULL) fclose(site->file);
    site->file = NULL;
}

static void stick_make_room(struct BrowserSiteStore *store, size_t keep)
{
    for (;;) {
        size_t open = 0, oldest = BROWSER_SITE_STORAGE_SITES;
        for (size_t i = 0; i < BROWSER_SITE_STORAGE_SITES; i++) {
            if (store->sites[i].file == NULL || i == keep) continue;
            open++;
            if (oldest == BROWSER_SITE_STORAGE_SITES
                || store->sites[i].file_stamp
                       < store->sites[oldest].file_stamp) oldest = i;
        }
        if (open < STICK_OPEN_FILES) return;
        stick_close(&store->sites[oldest]);
    }
}

static void stick_mark_session_files(struct BrowserSiteStore *store)
{
    if (store->session_marker) return;
    char path[STICK_PATH_BYTES];
    int written = snprintf(path, sizeof(path), "%s/%s", store->directory,
                           STICK_SESSION_MARKER);
    if (written <= 0 || (size_t) written >= sizeof(path)) return;
    FILE *marker = fopen(path, "wb");
    if (marker != NULL) fclose(marker);
    store->session_marker = true;
}

static bool stick_write_header(FILE *file, const char *origin,
                               size_t *written)
{
    size_t length = strlen(origin);
    unsigned char header[STICK_HEADER_FIXED] = {'T', 'F', 'S', 'S'};
    header[4] = STICK_VERSION;
    put_u16(header + 6, (uint16_t) length);
    if (fwrite(header, 1, sizeof(header), file) != sizeof(header)
        || fwrite(origin, 1, length, file) != length) return false;
    *written = sizeof(header) + length;
    return true;
}

/* Creates an empty file for the site at its current tier's name. */
static bool stick_create(struct BrowserSiteStore *store, size_t index)
{
    SiteRecord *site = &store->sites[index];
    char path[STICK_PATH_BYTES];
    if (!stick_path(store, index, site->tier, path)) return false;
    stick_close(site);
    stick_make_room(store, index);
    if (site->tier == BROWSER_SITE_STORAGE_STICK_SESSION)
        stick_mark_session_files(store);
    FILE *file = fopen(path, "w+b");
    if (file == NULL) return false;
    size_t written = 0;
    if (!stick_write_header(file, site->origin, &written)
        || fflush(file) != 0) {
        fclose(file);
        remove(path);
        return false;
    }
    site->file = file;
    site->file_bytes = written;
    site->dead_bytes = 0;
    site->file_stamp = ++store->file_clock;
    return true;
}

static FILE *stick_handle(struct BrowserSiteStore *store, size_t index)
{
    SiteRecord *site = &store->sites[index];
    if (site->file == NULL) {
        if (site->file_bytes == 0) {
            if (!stick_create(store, index)) return NULL;
        } else {
            char path[STICK_PATH_BYTES];
            if (!stick_path(store, index, site->tier, path)) return NULL;
            stick_make_room(store, index);
            site->file = fopen(path, "r+b");
        }
    }
    if (site->file != NULL) site->file_stamp = ++store->file_clock;
    return site->file;
}

static bool stick_append(struct BrowserSiteStore *store, size_t index,
                         uint8_t kind, uint8_t op, const char *key,
                         size_t key_length, const unsigned char *value,
                         size_t value_length, int64_t modified_ms,
                         size_t *record_offset, uint32_t *value_checksum)
{
    SiteRecord *site = &store->sites[index];
#ifndef __PSP__
    if (tilefinch_test_faults()->fail_site_storage_appends != 0) {
        tilefinch_test_faults()->fail_site_storage_appends--;
        return false;
    }
#endif
    FILE *file = stick_handle(store, index);
    if (file == NULL || key_length > UINT16_MAX
        || value_length > UINT32_MAX) return false;
    unsigned char header[STICK_RECORD_HEADER];
    uint32_t checksum = fnv32(UINT32_C(2166136261), value, value_length);
    header[0] = kind;
    header[1] = op;
    put_u16(header + 2, (uint16_t) key_length);
    put_u32(header + 4, (uint32_t) value_length);
    put_u64(header + 8, (uint64_t) modified_ms);
    put_u32(header + 16, checksum);
    put_u32(header + 20, fnv32(fnv32(UINT32_C(2166136261), header, 20),
                               key, key_length));
    if (fseek(file, (long) site->file_bytes, SEEK_SET) != 0
        || fwrite(header, 1, sizeof(header), file) != sizeof(header)
        || fwrite(key, 1, key_length, file) != key_length
        || (value_length != 0
            && fwrite(value, 1, value_length, file) != value_length)) {
        clearerr(file);
        return false;
    }
    if (record_offset != NULL) *record_offset = site->file_bytes;
    if (value_checksum != NULL) *value_checksum = checksum;
    site->file_bytes += record_bytes(key_length, value_length);
    return true;
}

static const unsigned char *stick_read(BrowserSession *session,
                                       struct BrowserSiteStore *store,
                                       const SiteItem *item)
{
    FILE *file = stick_handle(store, item->site);
    unsigned char *buffer = scratch_reserve(session, store,
                                            item->value_length);
    if (file == NULL || buffer == NULL) return NULL;
    long offset = (long) (item->record_offset + STICK_RECORD_HEADER
                          + item->key_length);
    if (fseek(file, offset, SEEK_SET) != 0
        || fread(buffer, 1, item->value_length, file)
               != item->value_length
        || fnv32(UINT32_C(2166136261), buffer, item->value_length)
               != item->value_checksum) {
        clearerr(file);
        return NULL;
    }
    buffer[item->value_length] = 0;
    return buffer;
}

static const unsigned char *item_value(BrowserSession *session,
                                       struct BrowserSiteStore *store,
                                       const SiteItem *item)
{
    if (!stick_tier(&store->sites[item->site]))
        return (const unsigned char *) item->key + item->key_length + 1u;
    if (item->value_length == 0) return (const unsigned char *) "";
    return stick_read(session, store, item);
}

/* Rewrites the site's log with only live records. On failure the old file
   is kept and nothing changes.

   A rename cannot replace an existing file on the PSP, so the swap moves
   the original aside first: path -> path.bak, path.tmp -> path, then the
   backup is removed. At every point a complete log exists under one of
   the three names, and stick_load looks for them in that order (a crash
   before the swap leaves path; during it, .tmp or .bak, both complete). */
static bool stick_compact(BrowserSession *session,
                          struct BrowserSiteStore *store, size_t index)
{
    SiteRecord *site = &store->sites[index];
    char path[STICK_PATH_BYTES], temporary[STICK_PATH_BYTES];
    char backup[STICK_PATH_BYTES];
    if (!stick_path(store, index, site->tier, path)
        || !temporary_path(path, temporary)
        || !backup_path(path, backup)) return false;
    FILE *source = stick_handle(store, index);
    if (source == NULL) return false;
    size_t *offsets = NULL;
    if (site->item_count != 0) {
        offsets = budget_malloc(session->budget,
                                site->item_count * sizeof(*offsets));
        if (offsets == NULL) return false;
    }
    FILE *output = fopen(temporary, "w+b");
    size_t written = 0;
    bool ok = output != NULL
        && stick_write_header(output, site->origin, &written);
    unsigned char chunk[1024];
    size_t ordinal = 0;
    for (size_t i = 0; ok && i < store->item_count; i++) {
        const SiteItem *item = &store->items[i];
        if (item->site != index) continue;
        size_t length = record_bytes(item->key_length, item->value_length);
        offsets[ordinal++] = written;
        if (fseek(source, (long) item->record_offset, SEEK_SET) != 0) {
            ok = false;
            break;
        }
        for (size_t copied = 0; ok && copied < length;) {
            size_t part = length - copied < sizeof(chunk)
                ? length - copied : sizeof(chunk);
            ok = fread(chunk, 1, part, source) == part
                && fwrite(chunk, 1, part, output) == part;
            copied += part;
        }
        written += length;
    }
    if (output != NULL && fclose(output) != 0) ok = false;
    clearerr(source);
    if (!ok) {
        remove(temporary);
        budget_free(session->budget, offsets);
        return false;
    }
    stick_close(site);
    remove(backup);
    if (rename(path, backup) != 0) {
        remove(temporary);
        budget_free(session->budget, offsets);
        return false;
    }
#ifndef __PSP__
    if (tilefinch_test_faults()->crash_next_site_storage_compact) {
        tilefinch_test_faults()->crash_next_site_storage_compact = false;
        budget_free(session->budget, offsets);
        return false;
    }
    bool refuse_rename =
        tilefinch_test_faults()->fail_next_site_storage_compact_rename;
    tilefinch_test_faults()->fail_next_site_storage_compact_rename = false;
#else
    const bool refuse_rename = false;
#endif
    if (refuse_rename || rename(temporary, path) != 0) {
        /* Put the original back: this session's offsets describe it. If
           even that fails, the next load finds .tmp or .bak. */
        (void) rename(backup, path);
        remove(temporary);
        budget_free(session->budget, offsets);
        return false;
    }
    remove(backup);
    ordinal = 0;
    for (size_t i = 0; i < store->item_count; i++)
        if (store->items[i].site == index)
            store->items[i].record_offset = offsets[ordinal++];
    budget_free(session->budget, offsets);
    site->file_bytes = written;
    site->dead_bytes = 0;
    return true;
}

static void stick_drop_site_items(BrowserSession *session,
                                  struct BrowserSiteStore *store,
                                  size_t index)
{
    for (size_t i = store->item_count; i-- > 0;)
        if (store->items[i].site == index) item_drop(session, store, i);
}

typedef enum {
    STICK_LOG_MISSING = 0,   /* could not be opened */
    STICK_LOG_INVALID,       /* opened, but not this site's log */
    STICK_LOG_VALID
} StickLogState;

/* Opens name as the site's log when its header is intact, leaving the
   position after the header. */
static StickLogState stick_open_log(const char *name, const char *site_origin,
                                    FILE **opened, long *end,
                                    size_t *origin_length)
{
    *opened = NULL;
    FILE *file = fopen(name, "r+b");
    if (file == NULL) return STICK_LOG_MISSING;
    *end = -1;
    if (fseek(file, 0, SEEK_END) == 0) *end = ftell(file);
    unsigned char header[STICK_HEADER_FIXED];
    char origin[BROWSER_ORIGIN_LIMIT];
    bool valid = *end >= (long) STICK_HEADER_FIXED
        && fseek(file, 0, SEEK_SET) == 0
        && fread(header, 1, sizeof(header), file) == sizeof(header)
        && memcmp(header, "TFSS", 4) == 0 && header[4] == STICK_VERSION
        && (*origin_length = get_u16(header + 6)) < sizeof(origin)
        && fread(origin, 1, *origin_length, file) == *origin_length;
    if (valid) {
        origin[*origin_length] = '\0';
        valid = strcmp(origin, site_origin) == 0;
    }
    if (!valid) {
        fclose(file);
        return STICK_LOG_INVALID;
    }
    *opened = file;
    return STICK_LOG_VALID;
}

/* True when name is the site's log and every record in it, value
   included, checks out to the end of the file: what a complete rewrite
   looks like, as opposed to a torn or damaged one. */
static bool stick_log_intact(const char *name, const char *site_origin)
{
    FILE *file = NULL;
    long end = -1;
    size_t origin_length = 0;
    if (stick_open_log(name, site_origin, &file, &end, &origin_length)
        != STICK_LOG_VALID) return false;
    size_t offset = STICK_HEADER_FIXED + origin_length;
    char key[BROWSER_OPFS_PATH_LIMIT];
    unsigned char chunk[1024];
    bool intact = true;
    while (intact && (long) offset < end) {
        unsigned char record[STICK_RECORD_HEADER];
        size_t key_length = 0, value_length = 0;
        intact = fread(record, 1, sizeof(record), file) == sizeof(record)
            && record[0] >= ITEM_LOCAL && record[0] <= ITEM_OPFS_DIRECTORY
            && (record[1] == RECORD_SET || record[1] == RECORD_REMOVE)
            && (key_length = get_u16(record + 2)) != 0
            && key_length < sizeof(key)
            && (value_length = get_u32(record + 4))
                   <= BROWSER_SITE_STORAGE_STICK_VALUE_BYTES
            && (long) (offset + record_bytes(key_length, value_length)) <= end
            && fread(key, 1, key_length, file) == key_length
            && fnv32(fnv32(UINT32_C(2166136261), record, 20), key,
                     key_length) == get_u32(record + 20);
        uint32_t checksum = UINT32_C(2166136261);
        for (size_t read = 0; intact && read < value_length;) {
            size_t part = value_length - read < sizeof(chunk)
                ? value_length - read : sizeof(chunk);
            intact = fread(chunk, 1, part, file) == part;
            checksum = fnv32(checksum, chunk, part);
            read += part;
        }
        intact = intact && checksum == get_u32(record + 16);
        offset += record_bytes(key_length, value_length);
    }
    fclose(file);
    return intact && (long) offset == end;
}

/* Loads an always-kept site's index from its file on first use. */
static bool stick_load(BrowserSession *session,
                       struct BrowserSiteStore *store, size_t index)
{
    SiteRecord *site = &store->sites[index];
    char path[STICK_PATH_BYTES], temporary[STICK_PATH_BYTES];
    char backup[STICK_PATH_BYTES];
    if (!stick_path(store, index, site->tier, path)
        || !temporary_path(path, temporary)
        || !backup_path(path, backup)) return false;
    stick_make_room(store, index);
    site->bytes = 0;
    site->item_count = 0;
    site->file_bytes = 0;
    site->dead_bytes = 0;
    /* Which copy is the log follows from how far a compaction's swap got
       (see stick_compact). Without a .bak the swap never began: the log
       is the original and a .tmp may be a torn rewrite. With one, the
       original is the .bak and the rewritten copy (the log, or .tmp if
       the log was not renamed in yet) was complete when the swap began;
       that copy wins only if every record and value still checks out,
       since choosing it discards the original. Nothing is removed until
       the choice is made. */
    const char *candidates[3] = { path, temporary, backup };
    StickLogState states[3];
    for (size_t i = 0; i < 3; i++) {
        FILE *probe = NULL;
        long probe_end = -1;
        size_t probe_origin = 0;
        states[i] = stick_open_log(candidates[i], site->origin, &probe,
                                   &probe_end, &probe_origin);
        if (probe != NULL) fclose(probe);
    }
    size_t chosen = 3;
    if (states[2] == STICK_LOG_VALID) {
        size_t rewritten = states[0] == STICK_LOG_VALID ? 0
            : states[1] == STICK_LOG_VALID ? 1 : 3;
        chosen = rewritten != 3
                && stick_log_intact(candidates[rewritten], site->origin)
            ? rewritten : 2;
    } else if (states[0] == STICK_LOG_VALID) {
        chosen = 0;
    } else if (states[1] == STICK_LOG_VALID) {
        chosen = 1;
    }
    FILE *file = NULL;
    long end = -1;
    size_t origin_length = 0;
    /* Into the log's name; a rename cannot replace a file on the PSP, so
       whatever holds the name and was not chosen goes first. */
    if (chosen != 3 && chosen != 0 && states[0] != STICK_LOG_MISSING)
        remove(path);
    if (chosen != 3 && (chosen == 0 || rename(candidates[chosen], path) == 0))
        (void) stick_open_log(path, site->origin, &file, &end,
                              &origin_length);
    if (file != NULL) {
        /* With the log in place, a leftover copy is redundant (or a torn
           or damaged rewrite): do not keep it on the stick. */
        remove(temporary);
        remove(backup);
    } else {
        /* Nothing usable: drop what is not this site's log, but never a
           file that merely failed to open. */
        for (size_t i = 0; i < 3; i++)
            if (states[i] == STICK_LOG_INVALID) remove(candidates[i]);
        site->loaded = true;   /* nothing stored yet */
        return true;
    }
    size_t offset = STICK_HEADER_FIXED + origin_length;
    bool failed = false;
    char key[BROWSER_OPFS_PATH_LIMIT];
    for (;;) {
        unsigned char record[STICK_RECORD_HEADER];
        if (fread(record, 1, sizeof(record), file) != sizeof(record)) break;
        uint8_t kind = record[0], op = record[1];
        size_t key_length = get_u16(record + 2);
        size_t value_length = get_u32(record + 4);
        if (kind < ITEM_LOCAL || kind > ITEM_OPFS_DIRECTORY
            || (op != RECORD_SET && op != RECORD_REMOVE)
            || key_length == 0 || key_length >= sizeof(key)
            || value_length > BROWSER_SITE_STORAGE_STICK_VALUE_BYTES
            || fread(key, 1, key_length, file) != key_length
            || fnv32(fnv32(UINT32_C(2166136261), record, 20), key,
                     key_length) != get_u32(record + 20)) break;
        key[key_length] = '\0';
        size_t length = record_bytes(key_length, value_length);
        if ((long) (offset + length) > end
            || fseek(file, (long) (offset + length), SEEK_SET) != 0) break;
        int existing = item_find(store, index, kind, key);
        if (kind == ITEM_SESSION) {
            site->dead_bytes += length;
        } else if (op == RECORD_REMOVE) {
            site->dead_bytes += length;
            if (existing >= 0) {
                const SiteItem *old = &store->items[existing];
                site->dead_bytes +=
                    record_bytes(old->key_length, old->value_length);
                item_drop(session, store, (size_t) existing);
            }
        } else if (existing >= 0) {
            SiteItem *item = &store->items[existing];
            site->dead_bytes +=
                record_bytes(item->key_length, item->value_length);
            if (site->bytes - item_bytes(item) + key_length + value_length
                > BROWSER_SITE_STORAGE_STICK_SITE_BYTES) break;
            site_bytes_change(store, index, item_bytes(item),
                              key_length + value_length);
            item->kind = kind;
            item->value_length = value_length;
            item->record_offset = offset;
            item->value_checksum = get_u32(record + 16);
            item->modified_ms = (int64_t) get_u64(record + 8);
            item->generation = next_generation(session);
        } else {
            if (site->item_count >= BROWSER_SITE_STORAGE_SITE_ITEMS
                || site->bytes + key_length + value_length
                       > BROWSER_SITE_STORAGE_STICK_SITE_BYTES) break;
            char *block = NULL;
            if (!items_reserve(session, store, 1)
                || (block = memory_block(session, key, key_length, NULL, 0,
                                         false)) == NULL) {
                failed = true;
                break;
            }
            store->items[store->item_count++] = (SiteItem) {
                .key = block,
                .key_length = key_length,
                .value_length = value_length,
                .record_offset = offset,
                .value_checksum = get_u32(record + 16),
                .modified_ms = (int64_t) get_u64(record + 8),
                .generation = next_generation(session),
                .site = (uint8_t) index,
                .kind = kind
            };
            site->item_count++;
            site_bytes_change(store, index, 0, key_length + value_length);
        }
        offset += length;
    }
    if (failed) {
        fclose(file);
        stick_drop_site_items(session, store, index);
        return false;
    }
    clearerr(file);
    site->file = file;
    site->file_bytes = offset;
    site->file_stamp = ++store->file_clock;
    site->loaded = true;
    /* A torn tail needs no repair: appends start at the last good record
       and overwrite it, and a load always stops at the first bad one. */
    if (site->dead_bytes > STICK_LOAD_COMPACT_DEAD_BYTES
        && site->dead_bytes >= site->bytes)
        (void) stick_compact(session, store, index);
    return true;
}

static bool site_ready(BrowserSession *session,
                       struct BrowserSiteStore *store, size_t index)
{
    SiteRecord *site = &store->sites[index];
    if (site->loaded) return true;
    return store->configured && stick_load(session, store, index);
}

/* Moves a RAM site's items into a new log for tier. On failure nothing
   changes and the partial file is removed. */
static bool site_move_to_stick(BrowserSession *session,
                               struct BrowserSiteStore *store, size_t index,
                               BrowserSiteStorageTier tier)
{
    SiteRecord *site = &store->sites[index];
    size_t *offsets = NULL;
    uint32_t *checksums = NULL;
    if (site->item_count != 0) {
        offsets = budget_malloc(session->budget,
                                site->item_count * sizeof(*offsets));
        checksums = budget_malloc(session->budget,
                                  site->item_count * sizeof(*checksums));
        if (offsets == NULL || checksums == NULL) {
            budget_free(session->budget, offsets);
            budget_free(session->budget, checksums);
            return false;
        }
    }
    site->tier = tier;
    bool ok = stick_create(store, index);
    size_t ordinal = 0;
    for (size_t i = 0; ok && i < store->item_count; i++) {
        const SiteItem *item = &store->items[i];
        if (item->site != index) continue;
        ok = stick_append(
            store, index, item->kind, RECORD_SET, item->key,
            item->key_length,
            (const unsigned char *) item->key + item->key_length + 1u,
            item->value_length, item->modified_ms, &offsets[ordinal],
            &checksums[ordinal]);
        ordinal++;
    }
    if (ok && fflush(site->file) != 0) ok = false;
    if (!ok) {
        char path[STICK_PATH_BYTES];
        stick_close(site);
        if (stick_path(store, index, tier, path)) remove(path);
        site->tier = BROWSER_SITE_STORAGE_MEMORY;
        site->file_bytes = 0;
        budget_free(session->budget, offsets);
        budget_free(session->budget, checksums);
        return false;
    }
    store->memory_bytes -= site->bytes;
    ordinal = 0;
    for (size_t i = 0; i < store->item_count; i++) {
        SiteItem *item = &store->items[i];
        if (item->site != index) continue;
        item->record_offset = offsets[ordinal];
        item->value_checksum = checksums[ordinal++];
        char *shrunk = budget_realloc(session->budget, item->key,
                                      item->key_length + 1u);
        if (shrunk != NULL) item->key = shrunk;
    }
    budget_free(session->budget, offsets);
    budget_free(session->budget, checksums);
    site->loaded = true;
    site->offer_pending = false;
    site->declined = false;
    return true;
}

/* Renames a site's log between the session and always names. */
static bool site_retier(struct BrowserSiteStore *store, size_t index,
                        BrowserSiteStorageTier tier)
{
    SiteRecord *site = &store->sites[index];
    char from[STICK_PATH_BYTES], to[STICK_PATH_BYTES];
    if (!stick_path(store, index, site->tier, from)
        || !stick_path(store, index, tier, to)) return false;
    stick_close(site);
    if (tier == BROWSER_SITE_STORAGE_STICK_SESSION)
        stick_mark_session_files(store);
    if (site->file_bytes != 0) {
        remove(to);
        if (rename(from, to) != 0) return false;
    }
    site->tier = tier;
    return true;
}

static void site_remove_file(struct BrowserSiteStore *store, size_t index)
{
    SiteRecord *site = &store->sites[index];
    char path[STICK_PATH_BYTES], other[STICK_PATH_BYTES];
    stick_close(site);
    if (stick_path(store, index, site->tier, path)) {
        remove(path);
        /* A load would bring an interrupted compaction's copy back. */
        if (temporary_path(path, other)) remove(other);
        if (backup_path(path, other)) remove(other);
    }
    site->file_bytes = 0;
    site->dead_bytes = 0;
}

/* ---- admission -------------------------------------------------------- */

static void site_offer(struct BrowserSiteStore *store, size_t index,
                       size_t needed)
{
    SiteRecord *site = &store->sites[index];
    if (!store->configured || !store->offers || store->request_ready
        || site->policy == BROWSER_SITE_STORAGE_MEMORY_ONLY
        || site->declined || site->offer_pending
        || needed > BROWSER_SITE_STORAGE_STICK_SITE_BYTES) return;
    store->request = (BrowserSiteStorageRequest) {
        .current_bytes = site->bytes,
        .needed_bytes = needed,
        .stick_limit_bytes = BROWSER_SITE_STORAGE_STICK_SITE_BYTES
    };
    snprintf(store->request.origin, sizeof(store->request.origin), "%s",
             site->origin);
    store->request_ready = true;
    site->offer_pending = true;
}

/* Grows a RAM site's allowance by doubling, only while the budget keeps
   the growth reserve free. */
static bool site_grow(BrowserSession *session,
                      struct BrowserSiteStore *store, SiteRecord *site,
                      size_t needed, size_t added)
{
    if (needed > BROWSER_SITE_STORAGE_MEMORY_SITE_BYTES
        || store->memory_bytes
               > BROWSER_SITE_STORAGE_MEMORY_TOTAL_BYTES - added) return false;
    size_t allowance = site->allowance != 0
        ? site->allowance : BROWSER_SITE_STORAGE_INITIAL_BYTES;
    while (allowance < needed) allowance *= 2u;
    if (allowance > BROWSER_SITE_STORAGE_MEMORY_SITE_BYTES)
        allowance = BROWSER_SITE_STORAGE_MEMORY_SITE_BYTES;
    if (allowance > site->allowance
        && budget_pressure_required(
               session->budget, allowance - site->allowance,
               BROWSER_SITE_STORAGE_GROWTH_RESERVE_BYTES)) return false;
    site->allowance = allowance;
    return true;
}

/* Whether a write taking the site from old_bytes to new_bytes for this
   item fits. A RAM site that cannot grow may raise the Memory Stick offer;
   the write itself still fails. */
static bool site_admit(BrowserSession *session,
                       struct BrowserSiteStore *store, size_t index,
                       size_t old_bytes, size_t new_bytes, bool new_item)
{
    SiteRecord *site = &store->sites[index];
    if (new_item
        && (site->item_count >= BROWSER_SITE_STORAGE_SITE_ITEMS
            || store->item_count >= BROWSER_SITE_STORAGE_ITEMS)) return false;
    if (new_bytes <= old_bytes) return true;
    size_t added = new_bytes - old_bytes;
    if (site->bytes > SIZE_MAX - added) return false;
    size_t needed = site->bytes + added;
    if (stick_tier(site))
        return needed <= BROWSER_SITE_STORAGE_STICK_SITE_BYTES;
    if (needed <= site->allowance
        && store->memory_bytes
               <= BROWSER_SITE_STORAGE_MEMORY_TOTAL_BYTES - added)
        return true;
    if (site_grow(session, store, site, needed, added)) return true;
    site_offer(store, index, needed);
    return false;
}

/* ---- common write path ------------------------------------------------- */

static bool origin_of(const char *url, char origin[BROWSER_ORIGIN_LIMIT])
{
    return url != NULL
        && tilefinch_url_origin(url, origin, BROWSER_ORIGIN_LIMIT);
}

/* The site for url, loaded, or -1. create acquires a record. */
static int site_for(BrowserSession *session, const char *url, bool create)
{
    char origin[BROWSER_ORIGIN_LIMIT];
    if (!origin_of(url, origin)) return -1;
    struct BrowserSiteStore *store = create ? store_create(session)
                                            : session->site_store;
    if (store == NULL) return -1;
    int index = create ? site_acquire(store, origin)
                       : site_find(store, origin);
    if (index < 0) return -1;
    if (!site_ready(session, store, (size_t) index)) return -1;
    return index;
}

/* Sets (or creates) an item. The value is copied into RAM or appended to
   the site's log. */
static bool item_set(BrowserSession *session, size_t index, uint8_t kind,
                     const char *key, const unsigned char *value,
                     size_t value_length, bool admit)
{
    struct BrowserSiteStore *store = session->site_store;
    SiteRecord *site = &store->sites[index];
    size_t key_length = strlen(key);
    /* No tier holds a larger value; this also keeps the byte sums below
       from overflowing. */
    if (value_length > BROWSER_SITE_STORAGE_STICK_VALUE_BYTES) return false;
    int existing = item_find(store, index, kind, key);
    size_t old_bytes = existing >= 0
        ? item_bytes(&store->items[existing]) : 0;
    if (admit && !site_admit(session, store, index, old_bytes,
                             key_length + value_length, existing < 0))
        return false;
    if (existing < 0 && !items_reserve(session, store, 1)) return false;
    int64_t modified = kind >= ITEM_OPFS_FILE ? modified_now() : 0;
    size_t offset = 0;
    uint32_t checksum = 0;
    char *block = NULL;
    if (stick_tier(site)) {
        if (existing < 0
            && (block = memory_block(session, key, key_length, NULL, 0,
                                     false)) == NULL) return false;
        if (!stick_append(store, index, kind, RECORD_SET, key, key_length,
                          value, value_length, modified, &offset,
                          &checksum)) {
            budget_free(session->budget, block);
            return false;
        }
        if (existing >= 0) {
            const SiteItem *old = &store->items[existing];
            site->dead_bytes +=
                record_bytes(old->key_length, old->value_length);
        }
    } else {
        block = memory_block(session, key, key_length, value, value_length,
                             true);
        if (block == NULL) return false;
    }
    SiteItem *item;
    if (existing >= 0) {
        item = &store->items[existing];
        if (block != NULL) {
            budget_free(session->budget, item->key);
            item->key = block;
        }
    } else {
        item = &store->items[store->item_count++];
        *item = (SiteItem) {
            .key = block, .key_length = key_length,
            .site = (uint8_t) index
        };
        site->item_count++;
    }
    site_bytes_change(store, index, old_bytes, key_length + value_length);
    item->kind = kind;
    item->value_length = value_length;
    item->record_offset = offset;
    item->value_checksum = checksum;
    item->modified_ms = modified;
    item->generation = next_generation(session);
    /* After the write, so a pass that rewrites everything ends compacted. */
    if (stick_tier(site) && site->dead_bytes > STICK_COMPACT_DEAD_BYTES
        && site->dead_bytes >= site->bytes)
        (void) stick_compact(session, store, index);
    return true;
}

/* Removes an item, logging the removal for a stick site so it stays gone
   after a reload (sessionStorage is never reloaded). When the removal
   cannot be logged the item stays, as it would reappear at the next
   load: false. */
static bool item_remove(BrowserSession *session, size_t at)
{
    struct BrowserSiteStore *store = session->site_store;
    SiteItem *item = &store->items[at];
    SiteRecord *site = &store->sites[item->site];
    if (stick_tier(site)) {
        size_t dead = record_bytes(item->key_length, item->value_length);
        if (item->kind != ITEM_SESSION) {
            if (!stick_append(store, item->site, item->kind, RECORD_REMOVE,
                              item->key, item->key_length, NULL, 0, 0, NULL,
                              NULL)) return false;
            dead += record_bytes(item->key_length, 0);
        }
        site->dead_bytes += dead;
    }
    item_drop(session, store, at);
    return true;
}

static void site_idle(BrowserSession *session, size_t index)
{
    site_release_if_idle(session->site_store, index);
    store_trim(session);
}

/* ---- localStorage and sessionStorage ----------------------------------- */

static uint8_t storage_kind(bool local)
{
    return local ? ITEM_LOCAL : ITEM_SESSION;
}

bool browser_session_storage_get(const BrowserSession *session,
                                 const char *url, bool local,
                                 const char *key, const char **value,
                                 size_t *value_length)
{
    if (session == NULL || !session->site_data_allowed || key == NULL)
        return false;
    BrowserSession *mutable_session = (BrowserSession *) session;
    int index = site_for(mutable_session, url, false);
    if (index < 0) return false;
    struct BrowserSiteStore *store = session->site_store;
    int at = item_find(store, (size_t) index, storage_kind(local), key);
    if (at < 0 || store->items[at].kind != storage_kind(local)) return false;
    const unsigned char *data = item_value(mutable_session, store,
                                           &store->items[at]);
    if (data == NULL) return false;
    if (value != NULL) *value = (const char *) data;
    if (value_length != NULL) *value_length = store->items[at].value_length;
    return true;
}

size_t browser_session_storage_length(
    const BrowserSession *session, const char *url, bool local)
{
    if (session == NULL || !session->site_data_allowed) return 0;
    int index = site_for((BrowserSession *) session, url, false);
    if (index < 0) return 0;
    const struct BrowserSiteStore *store = session->site_store;
    size_t count = 0;
    for (size_t i = 0; i < store->item_count; i++)
        if (store->items[i].site == (size_t) index
            && store->items[i].kind == storage_kind(local)) count++;
    return count;
}

bool browser_session_storage_key(
    const BrowserSession *session, const char *url, bool local,
    size_t index, const char **key)
{
    if (session == NULL || !session->site_data_allowed) return false;
    int site = site_for((BrowserSession *) session, url, false);
    if (site < 0) return false;
    const struct BrowserSiteStore *store = session->site_store;
    size_t seen = 0;
    for (size_t i = 0; i < store->item_count; i++) {
        const SiteItem *item = &store->items[i];
        if (item->site != (size_t) site
            || item->kind != storage_kind(local)) continue;
        if (seen++ != index) continue;
        if (key != NULL) *key = item->key;
        return true;
    }
    return false;
}

bool browser_session_storage_set(BrowserSession *session, const char *url,
                                 bool local, const char *key,
                                 const char *value, size_t value_length)
{
    if (session == NULL || !session->site_data_allowed
        || key == NULL || value == NULL || key[0] == '\0'
        || strlen(key) >= BROWSER_KEY_LIMIT) return false;
    int index = site_for(session, url, true);
    if (index < 0) {
        store_trim(session);
        return false;
    }
    SiteRecord *site = &session->site_store->sites[index];
    if (stick_tier(site)
        && value_length > BROWSER_SITE_STORAGE_STICK_VALUE_BYTES) return false;
    bool stored = item_set(session, (size_t) index, storage_kind(local), key,
                           (const unsigned char *) value, value_length,
                           true);
    if (!stored) site_idle(session, (size_t) index);
    return stored;
}

bool browser_session_storage_remove(BrowserSession *session, const char *url,
                                    bool local, const char *key)
{
    if (session == NULL || !session->site_data_allowed || key == NULL)
        return false;
    int index = site_for(session, url, false);
    if (index < 0) return true;
    int at = item_find(session->site_store, (size_t) index,
                       storage_kind(local), key);
    if (at < 0) return true;
    bool removed = item_remove(session, (size_t) at);
    site_idle(session, (size_t) index);
    return removed;
}

bool browser_session_storage_clear(BrowserSession *session, const char *url,
                                   bool local)
{
    if (session == NULL || !session->site_data_allowed) return false;
    int index = site_for(session, url, false);
    if (index < 0) return true;
    struct BrowserSiteStore *store = session->site_store;
    bool cleared = true;
    for (size_t i = store->item_count; i-- > 0;)
        if (store->items[i].site == (size_t) index
            && store->items[i].kind == storage_kind(local)
            && !item_remove(session, i)) cleared = false;
    site_idle(session, (size_t) index);
    return cleared;
}

bool browser_session_storage_clear_all(BrowserSession *session, bool local)
{
    if (session == NULL || session->budget == NULL
        || session->site_store == NULL) return true;
    struct BrowserSiteStore *store = session->site_store;
    bool cleared = true;
    /* An always-kept site not used yet this boot still has localStorage in
       its file: load its index so the removals below reach it. One that
       cannot be loaded keeps its data, so the clear did not happen. */
    if (local) {
        for (size_t i = 0; i < BROWSER_SITE_STORAGE_SITES; i++)
            if (store->sites[i].used && !site_ready(session, store, i))
                cleared = false;
    }
    for (size_t i = store->item_count; i-- > 0;)
        if (store->items[i].kind == storage_kind(local)
            && !item_remove(session, i)) cleared = false;
    for (size_t i = 0; i < BROWSER_SITE_STORAGE_SITES; i++)
        site_release_if_idle(store, i);
    store_trim(session);
    return cleared;
}

/* ---- OPFS ------------------------------------------------------------- */

static bool opfs_path_valid(const char *path)
{
    if (path == NULL || path[0] != '/') return false;
    size_t length = strlen(path);
    if (length == 0 || length >= BROWSER_OPFS_PATH_LIMIT) return false;
    if (length > 1 && path[length - 1] == '/') return false;
    const char *component = path + 1;
    while (*component != '\0') {
        const char *separator = strchr(component, '/');
        size_t component_length = separator == NULL
            ? strlen(component) : (size_t) (separator - component);
        if (component_length == 0
            || (component_length == 1 && component[0] == '.')
            || (component_length == 2 && component[0] == '.'
                && component[1] == '.')) return false;
        component = separator == NULL ? component + component_length
                                      : separator + 1;
    }
    return true;
}

static BrowserOpfsResult opfs_authority(const BrowserSession *session,
                                        const char *url)
{
    char origin[BROWSER_ORIGIN_LIMIT];
    if (session == NULL || session->budget == NULL
        || !session->site_data_allowed
        || session->captive_portal_stash != NULL
        || !origin_of(url, origin)) {
        return BROWSER_OPFS_UNAVAILABLE;
    }
    return BROWSER_OPFS_OK;
}

static bool opfs_parent_path(
    const char *path, char parent[BROWSER_OPFS_PATH_LIMIT])
{
    if (!opfs_path_valid(path) || strcmp(path, "/") == 0) return false;
    const char *separator = strrchr(path, '/');
    size_t length = separator == path ? 1u : (size_t) (separator - path);
    memcpy(parent, path, length);
    parent[length] = '\0';
    return true;
}

static BrowserOpfsKind opfs_kind(const SiteItem *item)
{
    return item->kind == ITEM_OPFS_DIRECTORY ? BROWSER_OPFS_DIRECTORY
                                             : BROWSER_OPFS_FILE;
}

static void opfs_fill_view(BrowserSession *session, const SiteItem *item,
                           bool with_data, BrowserOpfsView *view)
{
    struct BrowserSiteStore *store = session->site_store;
    view->kind = opfs_kind(item);
    view->data = NULL;
    view->data_length = item->value_length;
    view->last_modified_ms = item->modified_ms;
    view->generation = item->generation;
    if (item->kind == ITEM_OPFS_FILE
        && (with_data || !stick_tier(&store->sites[item->site])))
        view->data = item_value(session, store, item);
}

/* The item for path in url's site, or -1. */
static int opfs_find(BrowserSession *session, const char *url,
                     const char *path, int *site)
{
    *site = site_for(session, url, false);
    if (*site < 0) return -1;
    return item_find(session->site_store, (size_t) *site, ITEM_OPFS_FILE,
                     path);
}

static BrowserOpfsResult opfs_lookup(const BrowserSession *session,
                                     const char *url, const char *path,
                                     bool with_data, BrowserOpfsView *view)
{
    if (view != NULL) memset(view, 0, sizeof(*view));
    BrowserOpfsResult authority = opfs_authority(session, url);
    if (authority != BROWSER_OPFS_OK) return authority;
    if (!opfs_path_valid(path)) return BROWSER_OPFS_INVALID_PATH;
    if (strcmp(path, "/") == 0) {
        if (view != NULL) view->kind = BROWSER_OPFS_DIRECTORY;
        return BROWSER_OPFS_OK;
    }
    BrowserSession *mutable_session = (BrowserSession *) session;
    int site = -1;
    int at = opfs_find(mutable_session, url, path, &site);
    if (at < 0) return BROWSER_OPFS_NOT_FOUND;
    if (view != NULL) {
        opfs_fill_view(mutable_session,
                       &session->site_store->items[at], with_data, view);
        if (with_data && view->kind == BROWSER_OPFS_FILE
            && view->data == NULL) return BROWSER_OPFS_UNAVAILABLE;
    }
    return BROWSER_OPFS_OK;
}

BrowserOpfsResult browser_session_opfs_stat(
    const BrowserSession *session, const char *url, const char *path,
    BrowserOpfsView *view)
{
    return opfs_lookup(session, url, path, false, view);
}

BrowserOpfsResult browser_session_opfs_read(
    const BrowserSession *session, const char *url, const char *path,
    BrowserOpfsView *view)
{
    return opfs_lookup(session, url, path, true, view);
}

BrowserOpfsResult browser_session_opfs_create(
    BrowserSession *session, const char *url, const char *path,
    BrowserOpfsKind kind)
{
    BrowserOpfsResult authority = opfs_authority(session, url);
    if (authority != BROWSER_OPFS_OK) return authority;
    if (!opfs_path_valid(path) || strcmp(path, "/") == 0
        || (kind != BROWSER_OPFS_FILE
            && kind != BROWSER_OPFS_DIRECTORY)) {
        return BROWSER_OPFS_INVALID_PATH;
    }
    int site = -1;
    int existing = opfs_find(session, url, path, &site);
    if (existing >= 0) {
        return opfs_kind(&session->site_store->items[existing]) == kind
            ? BROWSER_OPFS_OK : BROWSER_OPFS_TYPE_MISMATCH;
    }
    char parent[BROWSER_OPFS_PATH_LIMIT];
    if (!opfs_parent_path(path, parent)) return BROWSER_OPFS_INVALID_PATH;
    if (strcmp(parent, "/") != 0) {
        int parent_at = site < 0 ? -1
            : item_find(session->site_store, (size_t) site, ITEM_OPFS_FILE,
                        parent);
        if (parent_at < 0) return BROWSER_OPFS_NOT_FOUND;
        if (session->site_store->items[parent_at].kind
            != ITEM_OPFS_DIRECTORY) return BROWSER_OPFS_TYPE_MISMATCH;
    }
    site = site_for(session, url, true);
    if (site < 0) {
        store_trim(session);
        return BROWSER_OPFS_QUOTA_EXCEEDED;
    }
    if (!item_set(session, (size_t) site,
                  kind == BROWSER_OPFS_DIRECTORY ? ITEM_OPFS_DIRECTORY
                                                 : ITEM_OPFS_FILE,
                  path, NULL, 0, true)) {
        site_idle(session, (size_t) site);
        return BROWSER_OPFS_QUOTA_EXCEEDED;
    }
    return BROWSER_OPFS_OK;
}

BrowserOpfsResult browser_session_opfs_write(
    BrowserSession *session, const char *url, const char *path,
    const unsigned char *data, size_t data_length)
{
    BrowserOpfsResult authority = opfs_authority(session, url);
    if (authority != BROWSER_OPFS_OK) return authority;
    if (!opfs_path_valid(path) || strcmp(path, "/") == 0
        || (data == NULL && data_length != 0)) {
        return BROWSER_OPFS_INVALID_PATH;
    }
    int site = -1;
    int at = opfs_find(session, url, path, &site);
    if (at < 0) return BROWSER_OPFS_NOT_FOUND;
    struct BrowserSiteStore *store = session->site_store;
    if (store->items[at].kind != ITEM_OPFS_FILE)
        return BROWSER_OPFS_TYPE_MISMATCH;
    SiteRecord *record = &store->sites[site];
    size_t limit = stick_tier(record) ? BROWSER_OPFS_STICK_FILE_BYTE_LIMIT
                                      : BROWSER_OPFS_FILE_BYTE_LIMIT;
    if (data_length > limit) {
        if (!stick_tier(record)
            && data_length <= BROWSER_OPFS_STICK_FILE_BYTE_LIMIT)
            site_offer(store, (size_t) site,
                       record->bytes - store->items[at].value_length
                           + data_length);
        return BROWSER_OPFS_QUOTA_EXCEEDED;
    }
    return item_set(session, (size_t) site, ITEM_OPFS_FILE, path, data,
                    data_length, true)
        ? BROWSER_OPFS_OK : BROWSER_OPFS_QUOTA_EXCEEDED;
}

static bool opfs_descendant(const char *parent, const char *candidate,
                            const char **relative)
{
    if (strcmp(parent, "/") == 0) {
        if (candidate[0] != '/' || candidate[1] == '\0') return false;
        if (relative != NULL) *relative = candidate + 1;
        return true;
    }
    size_t length = strlen(parent);
    if (strncmp(candidate, parent, length) != 0
        || candidate[length] != '/' || candidate[length + 1] == '\0') {
        return false;
    }
    if (relative != NULL) *relative = candidate + length + 1;
    return true;
}

/* Session-scoped OPFS only: a site kept on the Memory Stick always keeps
   its files. */
bool browser_session_opfs_clear_all(BrowserSession *session)
{
    if (session == NULL || session->site_store == NULL) return true;
    struct BrowserSiteStore *store = session->site_store;
    bool cleared = true;
    for (size_t i = store->item_count; i-- > 0;)
        if (store->items[i].kind >= ITEM_OPFS_FILE
            && store->sites[store->items[i].site].tier
                   != BROWSER_SITE_STORAGE_STICK_ALWAYS
            && !item_remove(session, i)) cleared = false;
    for (size_t i = 0; i < BROWSER_SITE_STORAGE_SITES; i++)
        site_release_if_idle(store, i);
    store_trim(session);
    return cleared;
}

BrowserOpfsResult browser_session_opfs_remove(
    BrowserSession *session, const char *url, const char *path,
    bool recursive)
{
    BrowserOpfsResult authority = opfs_authority(session, url);
    if (authority != BROWSER_OPFS_OK) return authority;
    if (!opfs_path_valid(path) || strcmp(path, "/") == 0)
        return BROWSER_OPFS_INVALID_PATH;
    int site = -1;
    int at = opfs_find(session, url, path, &site);
    if (at < 0) return BROWSER_OPFS_NOT_FOUND;
    struct BrowserSiteStore *store = session->site_store;
    if (store->items[at].kind == ITEM_OPFS_DIRECTORY) {
        for (size_t i = store->item_count; i-- > 0;) {
            const SiteItem *child = &store->items[i];
            if (child->site != (size_t) site
                || child->kind < ITEM_OPFS_FILE
                || !opfs_descendant(path, child->key, NULL)) continue;
            if (!recursive) return BROWSER_OPFS_NOT_EMPTY;
            /* A removal the Memory Stick refused stays; so does the
               directory holding it. */
            if (!item_remove(session, i)) {
                site_idle(session, (size_t) site);
                return BROWSER_OPFS_UNAVAILABLE;
            }
        }
        at = item_find(store, (size_t) site, ITEM_OPFS_FILE, path);
    }
    bool removed = at >= 0 && item_remove(session, (size_t) at);
    site_idle(session, (size_t) site);
    return removed ? BROWSER_OPFS_OK : BROWSER_OPFS_UNAVAILABLE;
}

BrowserOpfsResult browser_session_opfs_child(
    const BrowserSession *session, const char *url, const char *parent,
    size_t index, const char **name, BrowserOpfsView *view)
{
    if (name != NULL) *name = NULL;
    if (view != NULL) memset(view, 0, sizeof(*view));
    BrowserOpfsView parent_view;
    BrowserOpfsResult parent_result = browser_session_opfs_stat(
        session, url, parent, &parent_view);
    if (parent_result != BROWSER_OPFS_OK) return parent_result;
    if (parent_view.kind != BROWSER_OPFS_DIRECTORY)
        return BROWSER_OPFS_TYPE_MISMATCH;
    BrowserSession *mutable_session = (BrowserSession *) session;
    int site = site_for(mutable_session, url, false);
    if (site < 0) return BROWSER_OPFS_NOT_FOUND;
    const struct BrowserSiteStore *store = session->site_store;
    size_t seen = 0;
    for (size_t i = 0; i < store->item_count; i++) {
        const SiteItem *item = &store->items[i];
        const char *relative = NULL;
        if (item->site != (size_t) site || item->kind < ITEM_OPFS_FILE
            || !opfs_descendant(parent, item->key, &relative)
            || strchr(relative, '/') != NULL) continue;
        if (seen++ != index) continue;
        if (name != NULL) *name = relative;
        if (view != NULL) opfs_fill_view(mutable_session, item, false, view);
        return BROWSER_OPFS_OK;
    }
    return BROWSER_OPFS_NOT_FOUND;
}

/* ---- the Memory Stick tier ---------------------------------------------- */

bool browser_session_site_storage_configure(BrowserSession *session,
                                            const char *directory)
{
    if (session == NULL || session->budget == NULL || directory == NULL
        || directory[0] == '\0'
        || strlen(directory) >= BROWSER_SITE_STORAGE_DIRECTORY_LIMIT)
        return false;
    struct BrowserSiteStore *store = store_create(session);
    if (store == NULL) return false;
    snprintf(store->directory, sizeof(store->directory), "%s", directory);
    store->configured = true;
    return true;
}

void browser_session_site_storage_set_offers(BrowserSession *session,
                                             bool enabled)
{
    if (session == NULL) return;
    struct BrowserSiteStore *store = store_create(session);
    if (store == NULL) return;
    store->offers = enabled;
    if (!enabled && store->request_ready) {
        int index = site_find(store, store->request.origin);
        if (index >= 0) store->sites[index].offer_pending = false;
        store->request_ready = false;
    }
    store_trim(session);
}

bool browser_session_site_storage_set_policy(
    BrowserSession *session, const char *url,
    BrowserSiteStoragePolicy policy)
{
    char origin[BROWSER_ORIGIN_LIMIT];
    if (session == NULL || session->budget == NULL
        || !origin_of(url, origin)) return false;
    struct BrowserSiteStore *store = store_create(session);
    if (store == NULL) return false;
    int found = site_acquire(store, origin);
    if (found < 0) return false;
    size_t index = (size_t) found;
    SiteRecord *site = &store->sites[index];
    bool ok = true;
    if (policy == BROWSER_SITE_STORAGE_STICK) {
        if (!store->configured) {
            ok = false;
        } else if (site->tier == BROWSER_SITE_STORAGE_MEMORY) {
            if (site->item_count == 0) {
                /* Registered at boot, or nothing stored yet: the file
                   loads (or is created) on first use. */
                site->tier = BROWSER_SITE_STORAGE_STICK_ALWAYS;
                site->loaded = false;
                site->offer_pending = false;
                site->declined = false;
            } else {
                ok = site_move_to_stick(
                    session, store, index,
                    BROWSER_SITE_STORAGE_STICK_ALWAYS);
            }
        } else if (site->tier == BROWSER_SITE_STORAGE_STICK_SESSION) {
            ok = site_retier(store, index,
                             BROWSER_SITE_STORAGE_STICK_ALWAYS);
        }
    } else if (site->tier == BROWSER_SITE_STORAGE_STICK_ALWAYS) {
        ok = site_ready(session, store, index)
            && site_retier(store, index,
                           BROWSER_SITE_STORAGE_STICK_SESSION);
    }
    if (ok) site->policy = policy;
    site_idle(session, index);
    return ok;
}

bool browser_session_site_storage_take_request(
    BrowserSession *session, BrowserSiteStorageRequest *request)
{
    if (session == NULL || session->site_store == NULL) return false;
    struct BrowserSiteStore *store = session->site_store;
    scratch_trim(session, store);
    if (!store->request_ready) return false;
    if (request != NULL) *request = store->request;
    store->request_ready = false;
    return true;
}

bool browser_session_site_storage_grant(BrowserSession *session,
                                        const char *url, bool always)
{
    if (always) {
        return browser_session_site_storage_set_policy(
            session, url, BROWSER_SITE_STORAGE_STICK);
    }
    char origin[BROWSER_ORIGIN_LIMIT];
    if (session == NULL || session->site_store == NULL
        || !session->site_store->configured || !origin_of(url, origin))
        return false;
    struct BrowserSiteStore *store = session->site_store;
    int index = site_find(store, origin);
    if (index < 0) return false;
    SiteRecord *site = &store->sites[index];
    site->offer_pending = false;
    if (site->tier != BROWSER_SITE_STORAGE_MEMORY) return true;
    bool moved = site_move_to_stick(session, store, (size_t) index,
                                    BROWSER_SITE_STORAGE_STICK_SESSION);
    site_idle(session, (size_t) index);
    return moved;
}

void browser_session_site_storage_decline(BrowserSession *session,
                                          const char *url)
{
    char origin[BROWSER_ORIGIN_LIMIT];
    if (session == NULL || !origin_of(url, origin)) return;
    int index = site_find(session->site_store, origin);
    if (index < 0) return;
    SiteRecord *site = &session->site_store->sites[index];
    site->offer_pending = false;
    site->declined = true;
}

static void site_clear_data(BrowserSession *session, size_t index)
{
    struct BrowserSiteStore *store = session->site_store;
    for (size_t i = store->item_count; i-- > 0;)
        if (store->items[i].site == index) item_drop(session, store, i);
    if (stick_tier(&store->sites[index])) {
        site_remove_file(store, index);
        store->sites[index].loaded = true;
    }
}

bool browser_session_site_storage_forget(BrowserSession *session,
                                         const char *url)
{
    char origin[BROWSER_ORIGIN_LIMIT];
    if (session == NULL || !origin_of(url, origin)) return false;
    int index = site_find(session->site_store, origin);
    if (index < 0) return true;
    struct BrowserSiteStore *store = session->site_store;
    SiteRecord *site = &store->sites[index];
    site_clear_data(session, (size_t) index);
    site_reset(site);
    store_trim(session);
    return true;
}

static bool info_listed(const SiteRecord *site)
{
    return site->used
        && (site->tier != BROWSER_SITE_STORAGE_MEMORY
            || site->item_count != 0
            || site->policy != BROWSER_SITE_STORAGE_ASK);
}

static void info_fill(const struct BrowserSiteStore *store, size_t index,
                      BrowserSiteStorageInfo *info)
{
    const SiteRecord *site = &store->sites[index];
    *info = (BrowserSiteStorageInfo) {
        .tier = site->tier,
        .policy = site->policy,
        .bytes = site->bytes,
        .limit_bytes = stick_tier(site)
            ? BROWSER_SITE_STORAGE_STICK_SITE_BYTES : site->allowance,
        .item_count = site->item_count,
        .file_bytes = site->file_bytes,
        .loaded = site->loaded
    };
    snprintf(info->origin, sizeof(info->origin), "%s", site->origin);
    if (!site->loaded && store->configured) {
        char path[STICK_PATH_BYTES];
        FILE *file = stick_path(store, index, site->tier, path)
            ? fopen(path, "rb") : NULL;
        if (file != NULL) {
            if (fseek(file, 0, SEEK_END) == 0) {
                long end = ftell(file);
                if (end > 0) info->file_bytes = (size_t) end;
            }
            fclose(file);
        }
    }
}

BrowserSiteStorageTier browser_session_site_storage_tier(
    const BrowserSession *session, const char *url)
{
    char origin[BROWSER_ORIGIN_LIMIT];
    if (session == NULL || !origin_of(url, origin))
        return BROWSER_SITE_STORAGE_MEMORY;
    int index = site_find(session->site_store, origin);
    return index < 0 ? BROWSER_SITE_STORAGE_MEMORY
                     : session->site_store->sites[index].tier;
}

size_t browser_session_site_storage_count(const BrowserSession *session)
{
    if (session == NULL || session->site_store == NULL) return 0;
    size_t count = 0;
    for (size_t i = 0; i < BROWSER_SITE_STORAGE_SITES; i++)
        if (info_listed(&session->site_store->sites[i])) count++;
    return count;
}

bool browser_session_site_storage_info(const BrowserSession *session,
                                       size_t index,
                                       BrowserSiteStorageInfo *info)
{
    if (session == NULL || session->site_store == NULL || info == NULL)
        return false;
    size_t seen = 0;
    for (size_t i = 0; i < BROWSER_SITE_STORAGE_SITES; i++) {
        if (!info_listed(&session->site_store->sites[i])) continue;
        if (seen++ != index) continue;
        info_fill(session->site_store, i, info);
        return true;
    }
    return false;
}

bool browser_session_site_storage_info_for(const BrowserSession *session,
                                           const char *url,
                                           BrowserSiteStorageInfo *info)
{
    char origin[BROWSER_ORIGIN_LIMIT];
    if (session == NULL || info == NULL || !origin_of(url, origin))
        return false;
    int index = site_find(session->site_store, origin);
    if (index < 0) {
        *info = (BrowserSiteStorageInfo) {
            .limit_bytes = BROWSER_SITE_STORAGE_INITIAL_BYTES,
            .loaded = true
        };
        snprintf(info->origin, sizeof(info->origin), "%s", origin);
        return true;
    }
    info_fill(session->site_store, (size_t) index, info);
    return true;
}

void browser_site_storage_remove_session_files(const char *directory)
{
    char path[STICK_PATH_BYTES];
    if (directory == NULL) return;
    int written = snprintf(path, sizeof(path), "%s/%s", directory,
                           STICK_SESSION_MARKER);
    if (written <= 0 || (size_t) written >= sizeof(path)) return;
    FILE *marker = fopen(path, "rb");
    if (marker == NULL) return;
    fclose(marker);
    for (unsigned slot = 0; slot < BROWSER_SITE_STORAGE_SITES; slot++) {
        snprintf(path, sizeof(path), "%s/s-%02u", directory, slot);
        remove(path);
        snprintf(path, sizeof(path), "%s/s-%02u.tmp", directory, slot);
        remove(path);
        snprintf(path, sizeof(path), "%s/s-%02u.bak", directory, slot);
        remove(path);
    }
    snprintf(path, sizeof(path), "%s/%s", directory, STICK_SESSION_MARKER);
    remove(path);
}

/* ---- session-internal hooks ------------------------------------------- */

void site_storage_destroy(BrowserSession *session)
{
    struct BrowserSiteStore *store = session->site_store;
    if (store == NULL) return;
    for (size_t i = 0; i < BROWSER_SITE_STORAGE_SITES; i++) {
        SiteRecord *site = &store->sites[i];
        stick_close(site);
        if (site->used && site->tier == BROWSER_SITE_STORAGE_STICK_SESSION)
            site_remove_file(store, i);
    }
    if (store->session_marker)
        browser_site_storage_remove_session_files(store->directory);
    for (size_t i = 0; i < store->item_count; i++)
        budget_free(session->budget, store->items[i].key);
    budget_free(session->budget, store->items);
    budget_free(session->budget, store->scratch);
    budget_free(session->budget, store);
    session->site_store = NULL;
}

struct BrowserSiteStore *site_storage_detach(BrowserSession *session)
{
    struct BrowserSiteStore *store = session->site_store;
    session->site_store = NULL;
    return store;
}

void site_storage_attach(BrowserSession *session,
                         struct BrowserSiteStore *store)
{
    site_storage_destroy(session);
    session->site_store = store;
}

void site_storage_usage(const BrowserSession *session, const char *origin,
                        BrowserSiteDataUsage *usage)
{
    int index = site_find(session->site_store, origin);
    if (index < 0
        || !site_ready((BrowserSession *) session, session->site_store,
                       (size_t) index)) return;
    const struct BrowserSiteStore *store = session->site_store;
    for (size_t i = 0; i < store->item_count; i++) {
        const SiteItem *item = &store->items[i];
        if (item->site != (size_t) index) continue;
        if (item->kind >= ITEM_OPFS_FILE) {
            usage->opfs_entry_count++;
            usage->opfs_bytes += item->value_length;
        } else {
            if (item->kind == ITEM_LOCAL) usage->local_storage_count++;
            else usage->session_storage_count++;
            usage->storage_bytes += item_bytes(item);
        }
    }
}

void site_storage_clear_origin(BrowserSession *session, const char *origin)
{
    int index = site_find(session->site_store, origin);
    if (index < 0) return;
    SiteRecord *site = &session->site_store->sites[index];
    site_clear_data(session, (size_t) index);
    site->bytes = 0;
    site->allowance = BROWSER_SITE_STORAGE_INITIAL_BYTES;
    site_idle(session, (size_t) index);
}

bool site_storage_next_local(const BrowserSession *session, size_t *cursor,
                             SiteStorageLocalView *view)
{
    const struct BrowserSiteStore *store = session->site_store;
    if (store == NULL) return false;
    while (*cursor < store->item_count) {
        const SiteItem *item = &store->items[(*cursor)++];
        const SiteRecord *site = &store->sites[item->site];
        if (item->kind != ITEM_LOCAL || stick_tier(site)) continue;
        *view = (SiteStorageLocalView) {
            .origin = site->origin,
            .key = item->key,
            .value = item->key + item->key_length + 1u,
            .value_length = item->value_length
        };
        return true;
    }
    return false;
}

size_t site_storage_local_count(const BrowserSession *session)
{
    size_t cursor = 0, count = 0;
    SiteStorageLocalView view;
    while (site_storage_next_local(session, &cursor, &view)) count++;
    return count;
}

/* Target sites the staging snapshot would land in: RAM sites only. */
static bool adopt_target(const struct BrowserSiteStore *target,
                         const char *origin, int *index)
{
    *index = site_find(target, origin);
    return *index < 0
        || !stick_tier(&target->sites[*index]);
}

bool site_storage_local_fits(BrowserSession *target,
                             const BrowserSession *staging)
{
    const struct BrowserSiteStore *source = staging->site_store;
    if (source == NULL) return true;
    size_t site_bytes[BROWSER_SITE_STORAGE_SITES] = {0};
    size_t site_items[BROWSER_SITE_STORAGE_SITES] = {0};
    size_t incoming_items = 0, incoming_bytes = 0, new_sites = 0;
    const struct BrowserSiteStore *store = target->site_store;
    for (size_t s = 0; s < BROWSER_SITE_STORAGE_SITES; s++) {
        const SiteRecord *site = &source->sites[s];
        if (!site->used) continue;
        int index = -1;
        if (!adopt_target(store, site->origin, &index)) continue;
        for (size_t i = 0; i < source->item_count; i++) {
            const SiteItem *item = &source->items[i];
            if (item->site != s || item->kind != ITEM_LOCAL) continue;
            site_bytes[s] += item_bytes(item);
            site_items[s]++;
        }
        if (site_items[s] == 0) continue;
        if (index < 0) new_sites++;
        incoming_items += site_items[s];
        incoming_bytes += site_bytes[s];
    }
    if (incoming_items == 0) return true;
    /* What remains in target after its RAM localStorage is replaced. */
    size_t kept_items = 0, kept_bytes = 0, free_sites = 0;
    if (store != NULL) {
        for (size_t i = 0; i < store->item_count; i++) {
            const SiteItem *item = &store->items[i];
            if (item->kind == ITEM_LOCAL
                && !stick_tier(&store->sites[item->site])) continue;
            kept_items++;
            if (!stick_tier(&store->sites[item->site]))
                kept_bytes += item_bytes(item);
        }
        for (size_t s = 0; s < BROWSER_SITE_STORAGE_SITES; s++)
            if (!store->sites[s].used) free_sites++;
    } else {
        free_sites = BROWSER_SITE_STORAGE_SITES;
    }
    if (new_sites > free_sites
        || kept_items + incoming_items > BROWSER_SITE_STORAGE_ITEMS
        || kept_bytes + incoming_bytes
               > BROWSER_SITE_STORAGE_MEMORY_TOTAL_BYTES) return false;
    for (size_t s = 0; s < BROWSER_SITE_STORAGE_SITES; s++) {
        if (site_items[s] == 0) continue;
        size_t bytes = site_bytes[s], items = site_items[s];
        int index = site_find(store, source->sites[s].origin);
        if (index >= 0) {
            for (size_t i = 0; i < store->item_count; i++) {
                const SiteItem *item = &store->items[i];
                if (item->site != (size_t) index || item->kind == ITEM_LOCAL)
                    continue;
                bytes += item_bytes(item);
                items++;
            }
        }
        if (bytes > BROWSER_SITE_STORAGE_MEMORY_SITE_BYTES
            || items > BROWSER_SITE_STORAGE_SITE_ITEMS) return false;
    }
    struct BrowserSiteStore *reserved = store_create(target);
    return reserved != NULL
        && items_reserve(target, reserved,
                         kept_items + incoming_items > reserved->item_count
                             ? kept_items + incoming_items
                                   - reserved->item_count
                             : 0);
}

void site_storage_adopt_local(BrowserSession *target,
                              BrowserSession *staging)
{
    struct BrowserSiteStore *store = target->site_store;
    struct BrowserSiteStore *source = staging->site_store;
    if (store != NULL) {
        for (size_t i = store->item_count; i-- > 0;)
            if (store->items[i].kind == ITEM_LOCAL
                && !stick_tier(&store->sites[store->items[i].site]))
                item_drop(target, store, i);
    }
    if (source == NULL || store == NULL) {
        store_trim(target);
        return;
    }
    for (size_t i = 0; i < source->item_count; i++) {
        SiteItem *item = &source->items[i];
        if (item->kind != ITEM_LOCAL || item->key == NULL) continue;
        int index = -1;
        if (!adopt_target(store, source->sites[item->site].origin, &index))
            continue;
        if (index < 0) index = site_acquire(store,
                                            source->sites[item->site].origin);
        if (index < 0) continue;   /* fits() reserved a slot */
        SiteRecord *site = &store->sites[index];
        store->items[store->item_count++] = (SiteItem) {
            .key = item->key,
            .key_length = item->key_length,
            .value_length = item->value_length,
            .generation = next_generation(target),
            .site = (uint8_t) index,
            .kind = ITEM_LOCAL
        };
        site->item_count++;
        site_bytes_change(store, (size_t) index, 0, item_bytes(item));
        if (site->allowance == 0)
            site->allowance = BROWSER_SITE_STORAGE_INITIAL_BYTES;
        while (site->allowance < site->bytes
               && site->allowance < BROWSER_SITE_STORAGE_MEMORY_SITE_BYTES)
            site->allowance *= 2u;
        item->key = NULL;
    }
    for (size_t s = 0; s < BROWSER_SITE_STORAGE_SITES; s++)
        site_release_if_idle(store, s);
    store_trim(target);
}
