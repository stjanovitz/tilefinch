/*
 * The persistent compiled-script tier: QuickJS bytecode of page scripts
 * (classic and module), and lazy webpack bundle records, kept on the
 * Memory Stick across browser restarts.
 * Off unless a directory is configured: the PSP application offers it as a
 * Settings option, boot.cfg's module_cache_dir is a developer override, and
 * the lab takes --script-cache-dir. See browser_session_script_disk_
 * configure in session.h for the contract and docs/STORAGE.md for the
 * on-card layout.
 *
 * Shape:
 *  - One pack file per record group: a table kind and the compile name,
 *    URL and top-level site of the RAM records it holds, so a MediaWiki
 *    ResourceLoader response and its ~70 segments are one file. A pack holds
 *    each record's ordinal, compile flags, source length and SHA-256 with
 *    its bytecode, and ends with a CRC-32 of everything before it.
 *  - The file name is this engine's fingerprint prefix and the group hash
 *    (which also covers the engine fingerprint, pointer width and bytecode
 *    ABI), so another build never looks a pack up and the sweep removes it.
 *  - An index file (index.tfsi) lists the packs with their size, site hash
 *    and last use. It is read once, on first use; a lookup the index does
 *    not name never touches the card.
 *  - Reads happen on a RAM miss, synchronously: the whole pack is read,
 *    verified and its records copied into the RAM table, with cancellable
 *    checkpoints every 16 KiB and between records (they will be used
 *    by the same load). Writes, evictions, the orphan sweep and index saves
 *    are idle work in bounded slices (browser_session_script_disk_
 *    maintenance); a pack is written from the RAM table's records, at most
 *    BROWSER_SCRIPT_DISK_WRITE_SLICE bytes per idle turn, to a temporary
 *    name that is renamed into place when complete.
 */
#include "tilefinch/session.h"
#include "tilefinch_test_faults.h"

#include "tilefinch/platform.h"
#include "tilefinch/sha256.h"
#include "tilefinch/url.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <zlib.h>

#ifndef TILEFINCH_QUICKJS_ENGINE_ID
#define TILEFINCH_QUICKJS_ENGINE_ID "unknown-engine"
#endif

#define SCRIPT_DISK_PACK_MAGIC "TFSC"
#define SCRIPT_DISK_INDEX_MAGIC "TFSI"
#define SCRIPT_DISK_VERSION 1u
#define SCRIPT_DISK_SUFFIX ".tfsc"
#define SCRIPT_DISK_INDEX_NAME "index.tfsi"
/* The retired per-module tier's files; this tier removes them. */
#define SCRIPT_DISK_LEGACY_SUFFIX ".tfmb"
#define SCRIPT_DISK_ENGINE_PREFIX 8u
/* magic, version, ABI, kind, group hash, record count */
#define SCRIPT_DISK_PACK_HEADER (4u + 4u + 4u + 4u + 32u + 4u)
/* ordinal, compile flags, source length, source SHA-256, bytecode length */
#define SCRIPT_DISK_RECORD_HEADER (4u + 4u + 4u + 32u + 4u)
/* CRC-32 of everything before it. The card is the browser's own storage
   (as trusted as the program on it): the checksum is there to catch a torn
   or damaged file, which a CRC does cheaply on the PSP; identity is the
   group hash and each record's source SHA-256. */
#define SCRIPT_DISK_TRAILER 4u
/* magic, version, ABI, engine fingerprint, entry count, clock */
#define SCRIPT_DISK_INDEX_HEADER (4u + 4u + 4u + 16u + 4u + 4u)
#define SCRIPT_DISK_INDEX_ENTRY (16u + 8u + 4u + 4u + 4u)
#define SCRIPT_DISK_PATH 224u
/* Idle calls before a failed sweep is retried, doubling per failure. */
#define SCRIPT_DISK_RETRY_FIRST 64u
#define SCRIPT_DISK_RETRY_MAX 4096u
/* A pack's records are written from the RAM table, which holds at most this
   many per kind. */
#define SCRIPT_DISK_JOB_RECORDS BROWSER_SCRIPT_BYTECODE_ENTRIES

enum {
    SCRIPT_DISK_ENTRY_SEEN = 1u,  /* found by this session's sweep */
    SCRIPT_DISK_ENTRY_REFUSED = 4u /* not read again this session */
};

typedef struct {
    uint8_t name[16];
    uint8_t site[8];
    uint32_t bytes;
    uint32_t used;
    uint8_t kind;
    uint8_t flags;
    /* The document realm (RAM-table generation) that last read it: a pack
       is read at most once per load, so a group whose source changed costs
       one read, not one per segment. Records evicted from RAM since can be
       read again by a later load. */
    uint32_t read_generation;
} ScriptDiskEntry;

typedef struct {
    bool active;
    FILE *file;
    char temporary[SCRIPT_DISK_PATH];
    uint8_t group[32];
    uint8_t site[8];
    BrowserScriptBytecodeKind kind;
    uint16_t count;
    uint16_t record;
    /* Bytes of the current record written, its header included (0: none). */
    size_t offset;
    size_t bytes;
    size_t expected_bytes;
    uint16_t slots[SCRIPT_DISK_JOB_RECORDS];
    uint32_t serials[SCRIPT_DISK_JOB_RECORDS];
    uint32_t crc;
} ScriptDiskJob;

struct BrowserScriptDisk {
    char directory[BROWSER_SCRIPT_DISK_DIRECTORY_LIMIT];
    bool write;
    bool directory_ready;
    bool suspended;
    size_t maximum_bytes;
    bool index_loaded;
    /* Structural changes (packs added or removed) are saved at the next
       idle turn; last-use stamps only ride along with them. */
    bool index_dirty;
    bool index_stamps_dirty;
    uint32_t clock;
    size_t count;
    uint64_t total;
    ScriptDiskEntry entries[BROWSER_SCRIPT_DISK_FILE_COUNT_LIMIT];
    bool swept;
    bool sweep_failed;
    void *sweep_cursor; /* DIR* */
    size_t sweep_visits;
    unsigned retry_wait;
    unsigned retry_backoff;
    ScriptDiskJob job;
    BrowserScriptDiskStats stats;
};

/* ---- small helpers ---- */

static void put_u32(unsigned char *out, uint32_t value)
{
    out[0] = (unsigned char) value;
    out[1] = (unsigned char) (value >> 8);
    out[2] = (unsigned char) (value >> 16);
    out[3] = (unsigned char) (value >> 24);
}

static uint32_t get_u32(const unsigned char *in)
{
    return (uint32_t) in[0] | ((uint32_t) in[1] << 8)
        | ((uint32_t) in[2] << 16) | ((uint32_t) in[3] << 24);
}

static uint32_t checksum(uint32_t crc, const unsigned char *data,
                         size_t length)
{
    if (data == NULL) return (uint32_t) crc32(0L, Z_NULL, 0);
    while (length != 0) {
        uInt part = length > 0x40000000u ? 0x40000000u : (uInt) length;
        crc = (uint32_t) crc32(crc, data, part);
        data += part;
        length -= part;
    }
    return crc;
}

static bool trailer_valid(const unsigned char *data, size_t length)
{
    return length >= SCRIPT_DISK_TRAILER
        && checksum(checksum(0, NULL, 0), data, length - SCRIPT_DISK_TRAILER)
               == get_u32(data + length - SCRIPT_DISK_TRAILER);
}

static uint64_t disk_now_ns(void)
{
    return tilefinch_platform_monotonic_time_ns();
}

static void engine_prefix(char prefix[SCRIPT_DISK_ENGINE_PREFIX + 1u])
{
    static const char engine[] = TILEFINCH_QUICKJS_ENGINE_ID;
    for (size_t i = 0; i < SCRIPT_DISK_ENGINE_PREFIX; i++) {
        char c = i < sizeof(engine) - 1u ? engine[i] : '0';
        prefix[i] = isalnum((unsigned char) c) ? c : '0';
    }
    prefix[SCRIPT_DISK_ENGINE_PREFIX] = '\0';
}

static void engine_field(unsigned char field[16])
{
    static const char engine[] = TILEFINCH_QUICKJS_ENGINE_ID;
    memset(field, 0, 16);
    memcpy(field, engine, sizeof(engine) - 1u < 16u ? sizeof(engine) - 1u
                                                    : 16u);
}

static bool hash_text(TilefinchSha256 *context, const char *text)
{
    return tilefinch_sha256_update(context, (const unsigned char *) text,
                                   strlen(text) + 1u);
}

/* The group a record belongs to: this engine build, pointer width and
   bytecode ABI, then the table kind and the record's three key strings. */
static bool group_hash(BrowserScriptBytecodeKind kind, const char *name,
                       const char *url, const char *partition,
                       uint8_t hash[32])
{
    if (name == NULL || url == NULL || partition == NULL) return false;
    TilefinchSha256 context;
    tilefinch_sha256_init(&context);
    unsigned char numbers[12];
    put_u32(numbers, (uint32_t) sizeof(void *));
    put_u32(numbers + 4, TILEFINCH_QUICKJS_BYTECODE_ABI);
    put_u32(numbers + 8, (uint32_t) kind);
    return hash_text(&context, TILEFINCH_QUICKJS_ENGINE_ID)
        && tilefinch_sha256_update(&context, numbers, sizeof(numbers))
        && hash_text(&context, name) && hash_text(&context, url)
        && hash_text(&context, partition)
        && tilefinch_sha256_final(&context, hash);
}

static bool site_hash(const char *partition, uint8_t site[8])
{
    uint8_t digest[32];
    if (partition == NULL
        || !tilefinch_sha256_digest((const uint8_t *) partition,
                                    strlen(partition), digest)) return false;
    memcpy(site, digest, 8);
    return true;
}

static bool disk_path(const BrowserScriptDisk *disk, const uint8_t name[16],
                      const char *suffix, char *path, size_t capacity)
{
    static const char hex[] = "0123456789abcdef";
    char prefix[SCRIPT_DISK_ENGINE_PREFIX + 1u];
    engine_prefix(prefix);
    char text[33];
    for (size_t i = 0; i < 16; i++) {
        text[2 * i] = hex[name[i] >> 4];
        text[2 * i + 1] = hex[name[i] & 15u];
    }
    text[32] = '\0';
    int written = snprintf(path, capacity, "%s/%s-%s%s", disk->directory,
                           prefix, text, suffix);
    return written > 0 && (size_t) written < capacity;
}

static bool index_path(const BrowserScriptDisk *disk, const char *suffix,
                       char *path, size_t capacity)
{
    int written = snprintf(path, capacity, "%s/" SCRIPT_DISK_INDEX_NAME "%s",
                           disk->directory, suffix);
    return written > 0 && (size_t) written < capacity;
}

static void ensure_directory(BrowserScriptDisk *disk)
{
    if (disk->directory_ready) return;
    (void) mkdir(disk->directory, 0777);
    disk->directory_ready = true;
}

static bool session_writes(const BrowserSession *session)
{
    const BrowserScriptDisk *disk = session->script_disk;
    return disk != NULL && disk->write && !disk->suspended
        && session->site_data_allowed
        && session->captive_portal_stash == NULL;
}

/* ---- the index ---- */

static ScriptDiskEntry *index_find(BrowserScriptDisk *disk,
                                   const uint8_t name[16])
{
    for (size_t i = 0; i < disk->count; i++)
        if (memcmp(disk->entries[i].name, name, 16) == 0)
            return &disk->entries[i];
    return NULL;
}

static void index_remove_at(BrowserScriptDisk *disk, size_t at)
{
    uint32_t bytes = disk->entries[at].bytes;
    disk->total = bytes <= disk->total ? disk->total - bytes : 0;
    disk->entries[at] = disk->entries[disk->count - 1u];
    disk->count--;
    disk->index_dirty = true;
}

static void index_reset(BrowserScriptDisk *disk)
{
    disk->count = 0;
    disk->total = 0;
    disk->clock = 0;
}

/* Parses an index file image; false leaves the index empty. */
static bool index_parse(BrowserScriptDisk *disk, const unsigned char *data,
                        size_t length)
{
    unsigned char engine[16];
    engine_field(engine);
    if (length < SCRIPT_DISK_INDEX_HEADER + SCRIPT_DISK_TRAILER
        || memcmp(data, SCRIPT_DISK_INDEX_MAGIC, 4) != 0
        || get_u32(data + 4) != SCRIPT_DISK_VERSION
        || get_u32(data + 8) != TILEFINCH_QUICKJS_BYTECODE_ABI
        || memcmp(data + 12, engine, 16) != 0) return false;
    uint32_t count = get_u32(data + 28);
    if (count > BROWSER_SCRIPT_DISK_FILE_COUNT_LIMIT
        || length != SCRIPT_DISK_INDEX_HEADER
               + (size_t) count * SCRIPT_DISK_INDEX_ENTRY
               + SCRIPT_DISK_TRAILER) return false;
    if (!trailer_valid(data, length)) return false;
    index_reset(disk);
    disk->clock = get_u32(data + 32);
    const unsigned char *at = data + SCRIPT_DISK_INDEX_HEADER;
    for (uint32_t i = 0; i < count; i++, at += SCRIPT_DISK_INDEX_ENTRY) {
        ScriptDiskEntry entry = {0};
        memcpy(entry.name, at, 16);
        memcpy(entry.site, at + 16, 8);
        entry.bytes = get_u32(at + 24);
        entry.used = get_u32(at + 28);
        entry.kind = (uint8_t) get_u32(at + 32);
        if (entry.bytes == 0 || entry.bytes > BROWSER_SCRIPT_DISK_FILE_LIMIT
            || index_find(disk, entry.name) != NULL) continue;
        disk->entries[disk->count++] = entry;
        disk->total += entry.bytes;
    }
    return true;
}

typedef enum {
    SMALL_FILE_OK,
    SMALL_FILE_IO_ERROR,
    SMALL_FILE_REFUSED,
    SMALL_FILE_CANCELLED
} SmallFileRead;

static SmallFileRead read_small_file(Budget *budget, const char *path,
                            size_t limit, unsigned char **data,
                            size_t *length, bool paced)
{
    *data = NULL;
    *length = 0;
    FILE *file = fopen(path, "rb");
    if (file == NULL) return SMALL_FILE_IO_ERROR;
#ifndef __PSP__
    if (paced) {
        tilefinch_test_faults()->script_cache_read_attempts++;
        if (tilefinch_test_faults()->fail_next_script_cache_read) {
            tilefinch_test_faults()->fail_next_script_cache_read = false;
            fclose(file);
            return SMALL_FILE_IO_ERROR;
        }
    }
#endif
    bool ok = fseek(file, 0, SEEK_END) == 0;
    long size = ok ? ftell(file) : -1;
    ok = ok && size > 0 && (size_t) size <= limit
        && fseek(file, 0, SEEK_SET) == 0;
    unsigned char *buffer = ok ? budget_malloc_category(
        budget, BUDGET_CATEGORY_SESSION, (size_t) size) : NULL;
    SmallFileRead result = ok && buffer == NULL
        ? SMALL_FILE_REFUSED : SMALL_FILE_IO_ERROR;
    ok = buffer != NULL;
    for (size_t at = 0; ok && at < (size_t) size;) {
        size_t part = (size_t) size - at;
        if (part > BROWSER_SCRIPT_DISK_WRITE_SLICE)
            part = BROWSER_SCRIPT_DISK_WRITE_SLICE;
        ok = fread(buffer + at, 1, part, file) == part;
        at += part;
        if (ok && paced && !tilefinch_platform_cooperate("script-cache-read", at)) {
            result = SMALL_FILE_CANCELLED;
            ok = false;
        }
    }
    fclose(file);
    if (!ok) {
        budget_free(budget, buffer);
        return result;
    }
    *data = buffer;
    *length = (size_t) size;
    return SMALL_FILE_OK;
}

/* One read of index.tfsi (or the temporary copy a save left complete). A
   missing, damaged or foreign index is an empty one: the sweep then removes
   the packs it no longer names. */
static void index_load(BrowserSession *session)
{
    BrowserScriptDisk *disk = session->script_disk;
    if (disk->index_loaded) return;
    disk->index_loaded = true;
    index_reset(disk);
    const size_t limit = SCRIPT_DISK_INDEX_HEADER + SCRIPT_DISK_TRAILER
        + (size_t) BROWSER_SCRIPT_DISK_FILE_COUNT_LIMIT
              * SCRIPT_DISK_INDEX_ENTRY;
    const char *suffixes[2] = {"", ".tmp"};
    bool refused = false;
    for (size_t i = 0; i < 2; i++) {
        char path[SCRIPT_DISK_PATH];
        unsigned char *data = NULL;
        size_t length = 0;
        SmallFileRead read = index_path(disk, suffixes[i], path, sizeof(path))
            ? read_small_file(session->budget, path, limit, &data, &length, false)
            : SMALL_FILE_IO_ERROR;
        if (read != SMALL_FILE_OK) {
            refused |= read == SMALL_FILE_REFUSED;
            continue;
        }
        disk->stats.index_reads++;
        bool parsed = index_parse(disk, data, length);
        budget_free(session->budget, data);
        if (parsed) {
            /* Recovered from the temporary copy: publish it again. */
            if (i != 0) disk->index_dirty = true;
            return;
        }
        /* Damaged or foreign: empty, rewritten with the next pack. */
        index_reset(disk);
    }
    /* A refused read is not an empty index. Do not sweep its valid packs
       away, and retry when the Budget has room again. */
    if (refused) disk->index_loaded = false;
}

/* tmp + remove + rename (FAT does not rename over an existing file). */
static bool index_save(BrowserSession *session)
{
    BrowserScriptDisk *disk = session->script_disk;
    char path[SCRIPT_DISK_PATH], temporary[SCRIPT_DISK_PATH];
    if (!index_path(disk, "", path, sizeof(path))
        || !index_path(disk, ".tmp", temporary, sizeof(temporary)))
        return false;
    ensure_directory(disk);
    size_t length = SCRIPT_DISK_INDEX_HEADER
        + disk->count * SCRIPT_DISK_INDEX_ENTRY + SCRIPT_DISK_TRAILER;
    unsigned char *data = budget_malloc_category(
        session->budget, BUDGET_CATEGORY_SESSION, length);
    if (data == NULL) return false;
    memcpy(data, SCRIPT_DISK_INDEX_MAGIC, 4);
    put_u32(data + 4, SCRIPT_DISK_VERSION);
    put_u32(data + 8, TILEFINCH_QUICKJS_BYTECODE_ABI);
    engine_field(data + 12);
    put_u32(data + 28, (uint32_t) disk->count);
    put_u32(data + 32, disk->clock);
    unsigned char *at = data + SCRIPT_DISK_INDEX_HEADER;
    for (size_t i = 0; i < disk->count; i++, at += SCRIPT_DISK_INDEX_ENTRY) {
        const ScriptDiskEntry *entry = &disk->entries[i];
        memcpy(at, entry->name, 16);
        memcpy(at + 16, entry->site, 8);
        put_u32(at + 24, entry->bytes);
        put_u32(at + 28, entry->used);
        put_u32(at + 32, entry->kind);
    }
    put_u32(data + length - SCRIPT_DISK_TRAILER,
            checksum(0, data, length - SCRIPT_DISK_TRAILER));
    FILE *file = fopen(temporary, "wb");
    bool ok = file != NULL;
    ok = ok && fwrite(data, 1, length, file) == length;
    if (file != NULL) ok = fclose(file) == 0 && ok;
    budget_free(session->budget, data);
    if (ok) {
        (void) remove(path);
        ok = rename(temporary, path) == 0;
    }
    if (!ok) {
        (void) remove(temporary);
        return false;
    }
    disk->index_dirty = false;
    disk->index_stamps_dirty = false;
    disk->stats.index_writes++;
    disk->stats.written_bytes += length;
    return true;
}

/* Removes the pack named by index entry `at` and the entry. */
static bool pack_remove_at(BrowserScriptDisk *disk, size_t at)
{
    char path[SCRIPT_DISK_PATH];
    if (!disk_path(disk, disk->entries[at].name, SCRIPT_DISK_SUFFIX, path,
                   sizeof(path))) return false;
    if (remove(path) != 0 && errno != ENOENT) return false;
    disk->stats.removed++;
    index_remove_at(disk, at);
    return true;
}

/* ---- configuration ---- */

static void job_abort(BrowserScriptDisk *disk)
{
    if (!disk->job.active) return;
    if (disk->job.file != NULL) fclose(disk->job.file);
    (void) remove(disk->job.temporary);
    memset(&disk->job, 0, sizeof(disk->job));
}

static void sweep_close(BrowserScriptDisk *disk)
{
    if (disk->sweep_cursor != NULL) closedir(disk->sweep_cursor);
    disk->sweep_cursor = NULL;
}

static void disk_release(BrowserSession *session)
{
    BrowserScriptDisk *disk = session->script_disk;
    if (disk == NULL) return;
    job_abort(disk);
    sweep_close(disk);
    /* Last-use stamps of a read-only session are not worth a write. */
    if (disk->write && disk->index_loaded
        && (disk->index_dirty || disk->index_stamps_dirty))
        (void) index_save(session);
    budget_free(session->budget, disk);
    session->script_disk = NULL;
}

/* A host build never touches a PSP device path, whatever it is given: the
   test and lab tiers live in host directories only. */
static bool directory_allowed(const char *directory)
{
#if !defined(__PSP__)
    const char *colon = strchr(directory, ':');
    const char *slash = strchr(directory, '/');
    if (colon != NULL && (slash == NULL || colon < slash)) return false;
#else
    (void) directory;
#endif
    return true;
}

bool browser_session_script_disk_configure(BrowserSession *session,
                                           const char *directory, bool write,
                                           size_t maximum_bytes)
{
    if (session == NULL || session->budget == NULL) return false;
    disk_release(session);
    if (directory == NULL || directory[0] == '\0') return true;
    if (strlen(directory) >= BROWSER_SCRIPT_DISK_DIRECTORY_LIMIT
        || !directory_allowed(directory)) return false;
    BrowserScriptDisk *disk = budget_calloc_category(
        session->budget, BUDGET_CATEGORY_SESSION, 1, sizeof(*disk));
    if (disk == NULL) return false;
    snprintf(disk->directory, sizeof(disk->directory), "%s", directory);
    disk->write = write;
    disk->maximum_bytes = maximum_bytes == 0
        ? BROWSER_SCRIPT_DISK_DEFAULT_BYTES : maximum_bytes;
    session->script_disk = disk;
    return true;
}

void browser_session_script_disk_release(BrowserSession *session)
{
    if (session != NULL) disk_release(session);
}

bool browser_session_script_disk_enabled(const BrowserSession *session)
{
    return session != NULL && session->script_disk != NULL
        && !session->script_disk->suspended
        && session->captive_portal_stash == NULL;
}

bool browser_session_script_disk_writable(const BrowserSession *session)
{
    return session != NULL && session_writes(session);
}

void browser_session_script_disk_stats(const BrowserSession *session,
                                       BrowserScriptDiskStats *stats)
{
    if (stats == NULL) return;
    memset(stats, 0, sizeof(*stats));
    if (session == NULL || session->script_disk == NULL) return;
    *stats = session->script_disk->stats;
    stats->files = session->script_disk->count;
    stats->bytes = session->script_disk->total;
}

bool browser_session_script_disk_usage(BrowserSession *session,
                                       uint64_t *bytes, size_t *files)
{
    if (bytes != NULL) *bytes = 0;
    if (files != NULL) *files = 0;
    if (session == NULL || session->script_disk == NULL) return false;
    index_load(session);
    if (!session->script_disk->index_loaded) return false;
    if (bytes != NULL) *bytes = session->script_disk->total;
    if (files != NULL) *files = session->script_disk->count;
    return true;
}

static BrowserScriptBytecodeTable *table_of(BrowserSession *session,
                                            BrowserScriptBytecodeKind kind)
{
    if (kind == BROWSER_SCRIPT_BYTECODE_LAZY_BUNDLE)
        return session->lazy_bundle_records;
    return kind == BROWSER_SCRIPT_BYTECODE_CLASSIC
        ? session->classic_bytecode : session->module_bytecode;
}

/* ---- reading ---- */

static ScriptDiskEntry *lookup_entry(BrowserSession *session,
                                     BrowserScriptBytecodeKind kind,
                                     const BrowserScriptBytecodeKey *key,
                                     uint8_t group[32])
{
    BrowserScriptDisk *disk = session->script_disk;
    if (!group_hash(kind, key->module_name, key->response_url,
                    key->partition_key, group)) return NULL;
    index_load(session);
    return index_find(disk, group);
}

/* A pack that failed verification or restore: remove it (writes on) or
   stop reading it this session. */
static void pack_refuse(BrowserSession *session, ScriptDiskEntry *entry)
{
    BrowserScriptDisk *disk = session->script_disk;
    disk->stats.rejects++;
    entry->flags |= SCRIPT_DISK_ENTRY_REFUSED;
    if (disk->write && session_writes(session))
        (void) pack_remove_at(disk, (size_t) (entry - disk->entries));
}

static BrowserScriptBytecodeEntry *table_record(
    BrowserSession *session, BrowserScriptBytecodeKind kind,
    const char *name, const char *url, const char *partition,
    uint32_t ordinal)
{
    BrowserScriptBytecodeTable *table = table_of(session, kind);
    for (size_t i = 0; table != NULL && i < BROWSER_SCRIPT_BYTECODE_ENTRIES;
         i++) {
        BrowserScriptBytecodeEntry *entry = &table->entries[i];
        if (entry->bytecode != NULL && entry->ordinal == ordinal
            && strcmp(entry->module_name, name) == 0
            && strcmp(entry->response_url, url) == 0
            && strcmp(entry->partition_key, partition) == 0) return entry;
    }
    return NULL;
}

bool browser_session_script_disk_promote(BrowserSession *session,
                                         BrowserScriptBytecodeKind kind,
                                         const BrowserScriptBytecodeKey *key,
                                         uint32_t generation)
{
    if (!browser_session_script_disk_enabled(session) || key == NULL
        || key->module_name == NULL || key->response_url == NULL
        || key->partition_key == NULL) return false;
    BrowserScriptDisk *disk = session->script_disk;
    uint8_t group[32];
    ScriptDiskEntry *entry = lookup_entry(session, kind, key, group);
    if (entry == NULL) {
        disk->stats.index_misses++;
        return false;
    }
    if ((entry->flags & SCRIPT_DISK_ENTRY_REFUSED) != 0
        || (generation != 0 && entry->read_generation == generation))
        return false;
    char path[SCRIPT_DISK_PATH];
    if (!disk_path(disk, group, SCRIPT_DISK_SUFFIX, path, sizeof(path)))
        return false;
    uint64_t read_started = disk_now_ns();
    unsigned char *data = NULL;
    size_t length = 0;
    SmallFileRead read = read_small_file(session->budget, path,
                                BROWSER_SCRIPT_DISK_FILE_LIMIT, &data,
                                &length, true);
    uint64_t verify_started = disk_now_ns();
    disk->stats.read_ns += verify_started - read_started;
    if (read != SMALL_FILE_OK) {
        /* Retry cancellations and temporary memory pressure, but do not
           hammer an unreadable pack for every factory in this navigation. */
        if (read == SMALL_FILE_IO_ERROR) entry->read_generation = generation;
        /* Gone (or unreadable): the index was stale; a Budget refusal says
           nothing about the file, so only a missing file drops the entry. */
        FILE *probe = fopen(path, "rb");
        if (probe == NULL) {
            if (session_writes(session))
                index_remove_at(disk, (size_t) (entry - disk->entries));
        } else {
            fclose(probe);
        }
        return false;
    }
    disk->stats.reads++;
    disk->stats.read_bytes += length;
    /* Verify the whole file before believing any field of it. */
    uint32_t crc = checksum(0, NULL, 0);
    bool cancelled = false;
    size_t payload = length >= SCRIPT_DISK_TRAILER
        ? length - SCRIPT_DISK_TRAILER : 0;
    for (size_t at = 0; at < payload;) {
        size_t part = payload - at;
        if (part > BROWSER_SCRIPT_DISK_WRITE_SLICE)
            part = BROWSER_SCRIPT_DISK_WRITE_SLICE;
        crc = checksum(crc, data + at, part);
        at += part;
        if (!tilefinch_platform_cooperate("script-cache-verify", at)) {
            cancelled = true;
            break;
        }
    }
    if (cancelled) {
        budget_free(session->budget, data);
        return false; /* Retry is allowed in this same navigation. */
    }
    bool ok = length >= SCRIPT_DISK_PACK_HEADER + SCRIPT_DISK_TRAILER
        && crc == get_u32(data + payload)
        && memcmp(data, SCRIPT_DISK_PACK_MAGIC, 4) == 0
        && get_u32(data + 4) == SCRIPT_DISK_VERSION
        && get_u32(data + 8) == TILEFINCH_QUICKJS_BYTECODE_ABI
        && get_u32(data + 12) == (uint32_t) kind
        && memcmp(data + 16, group, 32) == 0;
    uint32_t count = ok ? get_u32(data + 48) : 0;
    ok = ok && count != 0 && count <= SCRIPT_DISK_JOB_RECORDS;
    /* Every record must fit before any is used. */
    size_t end = length - SCRIPT_DISK_TRAILER;
    size_t at = SCRIPT_DISK_PACK_HEADER;
    for (uint32_t i = 0; ok && i < count; i++) {
        if (end - at < SCRIPT_DISK_RECORD_HEADER) {
            ok = false;
            break;
        }
        uint32_t bytes = get_u32(data + at + 44);
        at += SCRIPT_DISK_RECORD_HEADER;
        ok = bytes != 0 && bytes <= end - at;
        at += ok ? bytes : 0;
    }
    ok = ok && at == end;
    disk->stats.verify_ns += disk_now_ns() - verify_started;
    if (!ok) {
        budget_free(session->budget, data);
        pack_refuse(session, entry);
        return false;
    }
    entry->used = ++disk->clock;
    disk->index_stamps_dirty = true;
    /* The requested record first (it may be all the table has room for),
       then the rest of the pack. */
    bool promoted_requested = false;
    bool retry = false;
    for (int pass = 0; pass < 2; pass++) {
        at = SCRIPT_DISK_PACK_HEADER;
        for (uint32_t i = 0; i < count; i++) {
            const unsigned char *record = data + at;
            uint32_t bytes = get_u32(record + 44);
            at += SCRIPT_DISK_RECORD_HEADER + bytes;
            uint32_t ordinal = get_u32(record);
            if ((ordinal == key->ordinal) != (pass == 0)) continue;
            if (!tilefinch_platform_cooperate("script-cache-promote", i)) {
                budget_free(session->budget, data);
                return promoted_requested;
            }
            BrowserScriptBytecodeKey stored = {
                .module_name = key->module_name,
                .response_url = key->response_url,
                .partition_key = key->partition_key,
                .source_length = get_u32(record + 8),
                .compile_flags = get_u32(record + 4),
                .ordinal = ordinal,
                .digest_ready = true
            };
            memcpy(stored.source_digest, record + 12, 32);
            /* A record already in RAM (this load stored or restored it)
               stays as it is. */
            if (table_record(session, kind, stored.module_name,
                             stored.response_url, stored.partition_key,
                             ordinal) != NULL) continue;
            bool may_fit = browser_session_script_bytecode_may_fit(
                session, kind, &stored, bytes, generation);
            if (!browser_session_script_bytecode_put(session, kind, &stored,
                    generation, record + SCRIPT_DISK_RECORD_HEADER, bytes)) {
                retry |= may_fit;
                continue;
            }
            BrowserScriptBytecodeEntry *copy = table_record(
                session, kind, stored.module_name, stored.response_url,
                stored.partition_key, ordinal);
            if (copy != NULL) copy->disk_state = BROWSER_SCRIPT_DISK_CLEAN;
            disk->stats.promoted++;
            if (pass == 0) promoted_requested = true;
        }
    }
    /* Only a completed restore suppresses another group read. In particular,
       Budget refusal and input cancellation never latch a navigation miss. */
    if (!retry) entry->read_generation = generation;
    budget_free(session->budget, data);
    return promoted_requested;
}

void browser_session_script_disk_discard(BrowserSession *session,
                                         BrowserScriptBytecodeKind kind,
                                         const BrowserScriptBytecodeKey *key)
{
    if (!browser_session_script_disk_enabled(session) || key == NULL)
        return;
    uint8_t group[32];
    ScriptDiskEntry *entry = lookup_entry(session, kind, key, group);
    if (entry != NULL) pack_refuse(session, entry);
}

/* ---- the orphan sweep ---- */

static void sweep_fail(BrowserScriptDisk *disk)
{
    unsigned wait = disk->retry_backoff;
    if (wait < SCRIPT_DISK_RETRY_FIRST) wait = SCRIPT_DISK_RETRY_FIRST;
    disk->sweep_failed = true;
    disk->retry_wait = wait;
    disk->retry_backoff = wait < SCRIPT_DISK_RETRY_MAX
        ? wait * 2u : SCRIPT_DISK_RETRY_MAX;
    sweep_close(disk);
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Classifies a directory entry as one of this tier's names: a pack or a
   temporary pack of some engine build, a retired per-module record, or the
   index's temporary or backup copy. */
typedef enum {
    SCRIPT_DISK_NAME_FOREIGN = 0,
    SCRIPT_DISK_NAME_PACK,
    SCRIPT_DISK_NAME_TEMPORARY,
    SCRIPT_DISK_NAME_LEGACY,
    SCRIPT_DISK_NAME_INDEX_SPARE
} ScriptDiskName;

static ScriptDiskName classify_name(const char *name, bool *this_build,
                                    uint8_t parsed[16])
{
    *this_build = false;
    size_t length = strlen(name);
    if (strcmp(name, SCRIPT_DISK_INDEX_NAME ".tmp") == 0)
        return SCRIPT_DISK_NAME_INDEX_SPARE;
    const size_t stem = SCRIPT_DISK_ENGINE_PREFIX + 1u + 32u;
    if (length < stem || name[SCRIPT_DISK_ENGINE_PREFIX] != '-')
        return SCRIPT_DISK_NAME_FOREIGN;
    for (size_t i = 0; i < SCRIPT_DISK_ENGINE_PREFIX; i++)
        if (!isalnum((unsigned char) name[i]))
            return SCRIPT_DISK_NAME_FOREIGN;
    for (size_t i = 0; i < 16; i++) {
        int high = hex_value(name[SCRIPT_DISK_ENGINE_PREFIX + 1u + 2u * i]);
        int low = hex_value(name[SCRIPT_DISK_ENGINE_PREFIX + 2u + 2u * i]);
        if (high < 0 || low < 0) return SCRIPT_DISK_NAME_FOREIGN;
        parsed[i] = (uint8_t) (high << 4 | low);
    }
    const char *suffix = name + stem;
    char prefix[SCRIPT_DISK_ENGINE_PREFIX + 1u];
    engine_prefix(prefix);
    *this_build = memcmp(name, prefix, SCRIPT_DISK_ENGINE_PREFIX) == 0;
    if (strcmp(suffix, SCRIPT_DISK_SUFFIX) == 0) return SCRIPT_DISK_NAME_PACK;
    if (strcmp(suffix, SCRIPT_DISK_SUFFIX ".tmp") == 0
        || strcmp(suffix, ".tmp") == 0) return SCRIPT_DISK_NAME_TEMPORARY;
    if (strcmp(suffix, SCRIPT_DISK_LEGACY_SUFFIX) == 0)
        return SCRIPT_DISK_NAME_LEGACY;
    return SCRIPT_DISK_NAME_FOREIGN;
}

/* One bounded slice of the sweep (or, with `remove_all`, of a clear): this
   tier's names only. With writes on it removes other builds' packs,
   retired records, stray temporary files and packs the index does not
   name; at its end, index entries whose pack is missing are dropped. */
static bool sweep_step(BrowserSession *session, bool remove_all)
{
    BrowserScriptDisk *disk = session->script_disk;
    if (disk->swept || disk->sweep_failed) return false;
    DIR *directory = disk->sweep_cursor;
    if (directory == NULL) {
        directory = opendir(disk->directory);
        if (directory == NULL) {
            if (errno == ENOENT) disk->swept = true;
            else sweep_fail(disk);
            return true;
        }
        disk->sweep_cursor = directory;
        disk->sweep_visits = 0;
        for (size_t i = 0; i < disk->count; i++)
            disk->entries[i].flags &= (uint8_t) ~SCRIPT_DISK_ENTRY_SEEN;
    }
    uint64_t started = disk_now_ns();
    char path[SCRIPT_DISK_PATH];
    for (unsigned i = 0; i < BROWSER_SCRIPT_DISK_SCAN_SLICE; i++) {
        if (disk->sweep_visits >= BROWSER_SCRIPT_DISK_SCAN_LIMIT) {
            sweep_fail(disk);
            return true;
        }
        if (!remove_all && i != 0 && disk_now_ns() - started >= 2000000u)
            break;
        errno = 0;
        struct dirent *dirent = readdir(directory);
        if (dirent == NULL) {
            if (errno != 0) {
                sweep_fail(disk);
                return true;
            }
            disk->swept = true;
            break;
        }
        disk->sweep_visits++;
        bool this_build = false;
        uint8_t name[16];
        ScriptDiskName kind = classify_name(dirent->d_name, &this_build,
                                            name);
        if (kind == SCRIPT_DISK_NAME_FOREIGN) continue;
        int written = snprintf(path, sizeof(path), "%s/%s", disk->directory,
                               dirent->d_name);
        if (written <= 0 || (size_t) written >= sizeof(path)) continue;
        if (!remove_all && disk->job.active
            && strcmp(path, disk->job.temporary) == 0) continue;
        ScriptDiskEntry *entry = kind == SCRIPT_DISK_NAME_PACK && this_build
            ? index_find(disk, name) : NULL;
        if (!remove_all && entry != NULL) {
            entry->flags |= SCRIPT_DISK_ENTRY_SEEN;
            continue;
        }
        if (remove(path) == 0) disk->stats.removed++;
        else if (errno != ENOENT) {
            sweep_fail(disk);
            return true;
        }
    }
    if (disk->swept) {
        sweep_close(disk);
        disk->retry_backoff = 0;
        if (!remove_all) {
            for (size_t i = disk->count; i-- > 0;)
                if ((disk->entries[i].flags & SCRIPT_DISK_ENTRY_SEEN) == 0)
                    index_remove_at(disk, i);
        }
    }
    return true;
}

/* ---- writing ---- */

/* The first RAM record still to be written, or NULL. */
static BrowserScriptBytecodeEntry *dirty_record(
    BrowserSession *session, BrowserScriptBytecodeKind *kind)
{
    for (int k = 0; k < (int) BROWSER_SCRIPT_BYTECODE_KINDS; k++) {
        BrowserScriptBytecodeTable *table =
            table_of(session, (BrowserScriptBytecodeKind) k);
        for (size_t i = 0;
             table != NULL && i < BROWSER_SCRIPT_BYTECODE_ENTRIES; i++) {
            BrowserScriptBytecodeEntry *entry = &table->entries[i];
            if (entry->bytecode != NULL
                && entry->disk_state == BROWSER_SCRIPT_DISK_DIRTY) {
                *kind = (BrowserScriptBytecodeKind) k;
                return entry;
            }
        }
    }
    return NULL;
}

static bool same_group(const BrowserScriptBytecodeEntry *a,
                       const BrowserScriptBytecodeEntry *b)
{
    return strcmp(a->module_name, b->module_name) == 0
        && strcmp(a->response_url, b->response_url) == 0
        && strcmp(a->partition_key, b->partition_key) == 0;
}

/* Marks a group's RAM records written (or not worth writing). */
static void group_mark(BrowserSession *session,
                       BrowserScriptBytecodeKind kind,
                       const BrowserScriptBytecodeEntry *member,
                       uint8_t state)
{
    BrowserScriptBytecodeTable *table = table_of(session, kind);
    for (size_t i = 0; table != NULL && i < BROWSER_SCRIPT_BYTECODE_ENTRIES;
         i++) {
        BrowserScriptBytecodeEntry *entry = &table->entries[i];
        if (entry->bytecode != NULL && entry != member
            && same_group(entry, member)) entry->disk_state = state;
    }
    ((BrowserScriptBytecodeEntry *) member)->disk_state = state;
}

/* Drops the least recently used pack: one removal per idle turn. */
static bool evict_one(BrowserScriptDisk *disk)
{
    if (disk->count == 0) return false;
    size_t victim = 0;
    for (size_t i = 1; i < disk->count; i++)
        if (disk->entries[i].used < disk->entries[victim].used) victim = i;
    if (!pack_remove_at(disk, victim)) return false;
    disk->stats.evictions++;
    return true;
}

static bool job_write(ScriptDiskJob *job, const void *data, size_t length)
{
    if (length == 0) return true;
    if (fwrite(data, 1, length, job->file) != length) return false;
    job->bytes += length;
    job->crc = checksum(job->crc, data, length);
    return true;
}

/* Chooses a group with records still to write and opens its pack, after
   making room; or marks a group that cannot be written as handled. */
static bool job_start(BrowserSession *session)
{
    BrowserScriptDisk *disk = session->script_disk;
    BrowserScriptBytecodeKind kind = BROWSER_SCRIPT_BYTECODE_MODULE;
    BrowserScriptBytecodeEntry *member = dirty_record(session, &kind);
    if (member == NULL) return false;
    ScriptDiskJob *job = &disk->job;
    memset(job, 0, sizeof(*job));
    BrowserScriptBytecodeTable *table = table_of(session, kind);
    size_t bytes = SCRIPT_DISK_PACK_HEADER + SCRIPT_DISK_TRAILER;
    for (size_t i = 0; i < BROWSER_SCRIPT_BYTECODE_ENTRIES; i++) {
        BrowserScriptBytecodeEntry *entry = &table->entries[i];
        if (entry->bytecode == NULL || !same_group(entry, member)) continue;
        if (entry->source_length > UINT32_MAX
            || entry->bytecode->length > BROWSER_SCRIPT_DISK_FILE_LIMIT
                                            - SCRIPT_DISK_RECORD_HEADER) {
            bytes = SIZE_MAX;
            break;
        }
        size_t record = SCRIPT_DISK_RECORD_HEADER + entry->bytecode->length;
        if (record > BROWSER_SCRIPT_DISK_FILE_LIMIT - bytes) {
            bytes = SIZE_MAX;
            break;
        }
        bytes += record;
        job->slots[job->count] = (uint16_t) i;
        job->serials[job->count] = entry->serial;
        job->count++;
    }
    if (bytes == SIZE_MAX || bytes > disk->maximum_bytes
        || !group_hash(kind, member->module_name, member->response_url,
                       member->partition_key, job->group)
        || !site_hash(member->partition_key, job->site)) {
        /* Larger than a pack may be: kept in RAM only. */
        disk->stats.skipped++;
        group_mark(session, kind, member, BROWSER_SCRIPT_DISK_NONE);
        return true;
    }
    /* Make room first, one removal per turn. The pack being replaced
       counts as free. */
    ScriptDiskEntry *existing = index_find(disk, job->group);
    uint64_t replaced = existing != NULL ? existing->bytes : 0;
    uint64_t total = disk->total - replaced;
    if (total > disk->maximum_bytes - bytes
        || (existing == NULL
            && disk->count >= BROWSER_SCRIPT_DISK_FILE_COUNT_LIMIT)) {
        if (!evict_one(disk)) {
            group_mark(session, kind, member, BROWSER_SCRIPT_DISK_NONE);
            disk->stats.skipped++;
        }
        return true;
    }
    if (!disk_path(disk, job->group, SCRIPT_DISK_SUFFIX ".tmp",
                   job->temporary, sizeof(job->temporary))) return false;
    ensure_directory(disk);
    job->file = fopen(job->temporary, "wb");
    if (job->file == NULL) {
        disk->stats.write_failures++;
        group_mark(session, kind, member, BROWSER_SCRIPT_DISK_NONE);
        return true;
    }
    job->active = true;
    job->kind = kind;
    job->expected_bytes = bytes;
    job->crc = checksum(0, NULL, 0);
    unsigned char header[SCRIPT_DISK_PACK_HEADER];
    memcpy(header, SCRIPT_DISK_PACK_MAGIC, 4);
    put_u32(header + 4, SCRIPT_DISK_VERSION);
    put_u32(header + 8, TILEFINCH_QUICKJS_BYTECODE_ABI);
    put_u32(header + 12, (uint32_t) kind);
    memcpy(header + 16, job->group, 32);
    put_u32(header + 48, job->count);
    if (!job_write(job, header, sizeof(header))) {
        disk->stats.write_failures++;
        job_abort(disk);
        group_mark(session, kind, member, BROWSER_SCRIPT_DISK_NONE);
    }
    return true;
}

/* The job's record `at`, if the RAM table still holds the same one. */
static BrowserScriptBytecodeEntry *job_record(BrowserSession *session,
                                              size_t at)
{
    ScriptDiskJob *job = &session->script_disk->job;
    BrowserScriptBytecodeTable *table = table_of(session, job->kind);
    if (table == NULL) return NULL;
    BrowserScriptBytecodeEntry *entry = &table->entries[job->slots[at]];
    return entry->bytecode != NULL && entry->serial == job->serials[at]
        ? entry : NULL;
}

/* Writes up to one slice of the open pack; publishes it when complete. */
static bool job_step(BrowserSession *session)
{
    BrowserScriptDisk *disk = session->script_disk;
    ScriptDiskJob *job = &disk->job;
    uint64_t started = disk_now_ns();
    size_t budget = BROWSER_SCRIPT_DISK_WRITE_SLICE;
    while (job->record < job->count && budget != 0) {
        BrowserScriptBytecodeEntry *entry = job_record(session, job->record);
        if (entry == NULL) {
            /* The table dropped a record meanwhile: start again later with
               what it holds now. */
            disk->stats.write_restarts++;
            job_abort(disk);
            return true;
        }
        if (job->offset == 0) {
            /* A record's header is written whole. */
            unsigned char header[SCRIPT_DISK_RECORD_HEADER];
            put_u32(header, entry->ordinal);
            put_u32(header + 4, entry->compile_flags);
            put_u32(header + 8, (uint32_t) entry->source_length);
            memcpy(header + 12, entry->source_digest, 32);
            put_u32(header + 44, (uint32_t) entry->bytecode->length);
            if (!job_write(job, header, sizeof(header))) goto failed;
            job->offset = SCRIPT_DISK_RECORD_HEADER;
            budget = budget > sizeof(header) ? budget - sizeof(header) : 0;
        }
        size_t done = job->offset - SCRIPT_DISK_RECORD_HEADER;
        size_t remaining = entry->bytecode->length - done;
        size_t chunk = remaining < budget ? remaining : budget;
        if (!job_write(job, entry->bytecode->data + done, chunk))
            goto failed;
        job->offset += chunk;
        budget -= chunk;
        if (done + chunk == entry->bytecode->length) {
            job->record++;
            job->offset = 0;
        }
    }
    disk->stats.write_ns += disk_now_ns() - started;
    if (job->record < job->count) return true;
    unsigned char trailer[SCRIPT_DISK_TRAILER];
    put_u32(trailer, job->crc);
    if (fwrite(trailer, 1, sizeof(trailer), job->file) != sizeof(trailer))
        goto failed;
    job->bytes += sizeof(trailer);
    int closed = fclose(job->file);
    job->file = NULL;
    char path[SCRIPT_DISK_PATH];
    if (closed != 0 || job->bytes != job->expected_bytes
        || !disk_path(disk, job->group, SCRIPT_DISK_SUFFIX, path,
                      sizeof(path))) goto failed;
    /* FAT does not rename over an existing file. */
    (void) remove(path);
    if (rename(job->temporary, path) != 0) goto failed;
    ScriptDiskEntry *existing = index_find(disk, job->group);
    if (existing != NULL) {
        disk->total -= existing->bytes <= disk->total ? existing->bytes
                                                      : disk->total;
    } else {
        existing = &disk->entries[disk->count++];
    }
    memset(existing, 0, sizeof(*existing));
    memcpy(existing->name, job->group, 16);
    memcpy(existing->site, job->site, 8);
    existing->bytes = (uint32_t) job->bytes;
    existing->used = ++disk->clock;
    existing->kind = (uint8_t) job->kind;
    existing->flags = SCRIPT_DISK_ENTRY_SEEN;
    disk->total += job->bytes;
    disk->index_dirty = true;
    disk->stats.writes++;
    disk->stats.written_bytes += job->bytes;
    for (size_t i = 0; i < job->count; i++) {
        BrowserScriptBytecodeEntry *entry = job_record(session, i);
        if (entry != NULL) entry->disk_state = BROWSER_SCRIPT_DISK_CLEAN;
    }
    memset(job, 0, sizeof(*job));
    return true;
failed:
    disk->stats.write_failures++;
    {
        BrowserScriptBytecodeEntry *member = job_record(session, 0);
        BrowserScriptBytecodeKind kind = job->kind;
        job_abort(disk);
        /* Not retried this session: the card refused it. */
        if (member != NULL)
            group_mark(session, kind, member, BROWSER_SCRIPT_DISK_NONE);
    }
    return true;
}

bool browser_session_script_disk_maintenance(BrowserSession *session)
{
    if (!browser_session_script_disk_enabled(session)) return false;
    BrowserScriptDisk *disk = session->script_disk;
    if (!disk->index_loaded) {
        index_load(session);
        return true;
    }
    if (!disk->write) return false;
    if (disk->job.active) {
        if (!session_writes(session)) {
            job_abort(disk);
            return true;
        }
        return job_step(session);
    }
    if (disk->sweep_failed) {
        if (disk->retry_wait > 1u) {
            disk->retry_wait--;
        } else {
            disk->sweep_failed = false;
            disk->swept = false;
        }
    }
    if (!disk->swept && !disk->sweep_failed)
        return sweep_step(session, false);
    /* Packs first; the index is saved once nothing is left to write, so a
       page's packs cost one index write (a pack missing from it after a
       crash is an orphan the next sweep removes). */
    if (session_writes(session) && job_start(session)) return true;
    if (disk->index_dirty) {
        if (!index_save(session)) disk->stats.write_failures++;
        disk->index_dirty = false;
        return true;
    }
    return false;
}

bool browser_session_script_disk_clear(BrowserSession *session)
{
    if (session == NULL || session->script_disk == NULL) return true;
    BrowserScriptDisk *disk = session->script_disk;
    job_abort(disk);
    sweep_close(disk);
    if (!disk->write) {
        /* A read-only tier is never written: stop reading it instead. */
        disk->suspended = true;
        return true;
    }
    disk->swept = false;
    disk->sweep_failed = false;
    while (sweep_step(session, true)) {
        if (disk->swept || disk->sweep_failed) break;
    }
    char path[SCRIPT_DISK_PATH];
    bool index_gone = !index_path(disk, "", path, sizeof(path))
        || remove(path) == 0 || errno == ENOENT;
    index_reset(disk);
    disk->index_loaded = true;
    disk->index_dirty = false;
    disk->index_stamps_dirty = false;
    disk->swept = !disk->sweep_failed;
    disk->stats.clears++;
    /* What RAM still holds is no longer on the card. */
    for (int k = 0; k < (int) BROWSER_SCRIPT_BYTECODE_KINDS; k++) {
        BrowserScriptBytecodeTable *table =
            table_of(session, (BrowserScriptBytecodeKind) k);
        for (size_t i = 0;
             table != NULL && i < BROWSER_SCRIPT_BYTECODE_ENTRIES; i++)
            if (table->entries[i].disk_state == BROWSER_SCRIPT_DISK_CLEAN)
                table->entries[i].disk_state = BROWSER_SCRIPT_DISK_NONE;
    }
    if (disk->sweep_failed) disk->suspended = true;
    return !disk->sweep_failed && index_gone;
}

bool browser_session_script_disk_clear_site(BrowserSession *session,
                                            const char *partition)
{
    if (session == NULL || session->script_disk == NULL || partition == NULL)
        return true;
    BrowserScriptDisk *disk = session->script_disk;
    uint8_t site[8];
    if (!site_hash(partition, site)) return false;
    if (!disk->write) {
        /* A read-only tier keeps the site's packs: stop reading it so
           they are not promoted again. */
        disk->suspended = true;
        return true;
    }
    index_load(session);
    if (disk->job.active && memcmp(disk->job.site, site, 8) == 0)
        job_abort(disk);
    if (!disk->index_loaded) {
        disk->suspended = true;
        return false;
    }
    bool ok = true, changed = false;
    for (size_t i = disk->count; i-- > 0;) {
        if (memcmp(disk->entries[i].site, site, 8) != 0) continue;
        if (pack_remove_at(disk, i)) changed = true;
        else ok = false;
    }
    if (changed) ok = index_save(session) && ok;
    return ok;
}
