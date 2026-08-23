#include "tilefinch/session_persistence.h"

#include "tilefinch/url.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define PERSIST_HEADER_BYTES 36u
#define PERSIST_VERSION UINT32_C(1)
#define PERSIST_PATH_BYTES 1200u
#define PERSIST_SKIP_BUFFER_BYTES 256u
#define RECORD_CACHE UINT32_C(1)
#define RECORD_LOCAL_STORAGE UINT32_C(2)

_Static_assert(BROWSER_CACHE_ENTRIES <= 64,
               "persisted cache selection uses a 64-bit slot mask");
_Static_assert(BROWSER_CACHE_ENTRIES <= UINT8_MAX,
               "persisted cache order uses byte indices");

static const unsigned char persist_magic[8] = {
    'T', 'F', 'S', 'E', 'S', 'S', 1, 0
};

typedef struct {
    FILE *file;
    size_t bytes;
    uint32_t hash;
    bool failed;
} PersistWriter;

typedef struct {
    BrowserSessionPersistenceMask mask;
    size_t cache_count;
    size_t storage_count;
    size_t cache_bytes;
    uint64_t cache_slots;
} PersistPlan;

typedef enum {
    ASYNC_LOAD_HEADER = 0,
    ASYNC_LOAD_RECORD_TYPE,
    ASYNC_LOAD_RECORD_BYTES,
    ASYNC_LOAD_CACHE_URL_LENGTH,
    ASYNC_LOAD_CACHE_URL,
    ASYNC_LOAD_CACHE_BODY_LENGTH,
    ASYNC_LOAD_CACHE_BODY,
    ASYNC_LOAD_CACHE_STAMP,
    ASYNC_LOAD_CACHE_ETAG_LENGTH,
    ASYNC_LOAD_CACHE_ETAG,
    ASYNC_LOAD_CACHE_MODIFIED_LENGTH,
    ASYNC_LOAD_CACHE_MODIFIED,
    ASYNC_LOAD_CACHE_CONTENT_TYPE_LENGTH,
    ASYNC_LOAD_CACHE_CONTENT_TYPE,
    ASYNC_LOAD_CACHE_VARY_LENGTH,
    ASYNC_LOAD_CACHE_VARY,
    ASYNC_LOAD_CACHE_FLAGS,
    ASYNC_LOAD_CACHE_RESPONSE_LENGTH,
    ASYNC_LOAD_CACHE_RESPONSE,
    ASYNC_LOAD_CACHE_POLICY_LENGTH,
    ASYNC_LOAD_CACHE_POLICY,
    ASYNC_LOAD_STORAGE_ORIGIN_LENGTH,
    ASYNC_LOAD_STORAGE_ORIGIN,
    ASYNC_LOAD_STORAGE_KEY_LENGTH,
    ASYNC_LOAD_STORAGE_KEY,
    ASYNC_LOAD_STORAGE_VALUE_LENGTH,
    ASYNC_LOAD_STORAGE_VALUE,
    ASYNC_LOAD_TRAILING,
    ASYNC_LOAD_FINISHED
} AsyncLoadPhase;

struct BrowserSessionPersistenceLoad {
    BrowserSession *target;
    BrowserSession *staging;
    FILE *file;
    BrowserSessionPersistenceLimits limits;
    BrowserSessionPersistenceMask requested_mask;
    BrowserSessionPersistenceMask stored_mask;
    BrowserSessionPersistenceStatus status;
    BrowserSessionPersistenceStatus primary_status;
    AsyncLoadPhase phase;
    char primary[PERSIST_PATH_BYTES];
    char backup[PERSIST_PATH_BYTES];
    bool trying_backup;
    unsigned char header[PERSIST_HEADER_BYTES];
    size_t header_read;
    size_t payload_remaining;
    size_t record_remaining;
    size_t cache_bytes;
    size_t parsed_cache;
    size_t parsed_storage;
    size_t expected_cache;
    size_t expected_storage;
    uint32_t expected_hash;
    uint32_t hash;
    unsigned char scalar[8];
    size_t scalar_read;
    size_t scalar_size;
    size_t value_read;
    size_t value_size;
    uint32_t record_type;
    uint32_t record_bytes;
    char url[TILEFINCH_URL_SERIALIZED_LIMIT];
    char etag[sizeof(((BrowserCacheEntry *) 0)->etag)];
    char modified[sizeof(((BrowserCacheEntry *) 0)->last_modified)];
    char content_type[sizeof(((BrowserCacheEntry *) 0)->content_type)];
    char vary[sizeof(((BrowserCacheEntry *) 0)->vary)];
    char response[TILEFINCH_URL_SERIALIZED_LIMIT];
    char policy[BROWSER_REFERRER_POLICY_LIMIT];
    char origin[BROWSER_ORIGIN_LIMIT];
    char key[BROWSER_KEY_LIMIT];
    unsigned char *value;
    size_t body_length;
    uint64_t stamp;
    uint32_t flags;
    bool body_skipped;
};

static uint32_t hash_update(uint32_t hash, const void *data, size_t length)
{
    const unsigned char *bytes = data;
    for (size_t i = 0; i < length; i++) {
        hash = (hash ^ bytes[i]) * UINT32_C(16777619);
    }
    return hash;
}

static void put_u32(unsigned char output[4], uint32_t value)
{
    output[0] = (unsigned char) value;
    output[1] = (unsigned char) (value >> 8);
    output[2] = (unsigned char) (value >> 16);
    output[3] = (unsigned char) (value >> 24);
}

static void put_u64(unsigned char output[8], uint64_t value)
{
    put_u32(output, (uint32_t) value);
    put_u32(output + 4, (uint32_t) (value >> 32));
}

static uint32_t get_u32(const unsigned char input[4])
{
    return (uint32_t) input[0]
        | (uint32_t) input[1] << 8
        | (uint32_t) input[2] << 16
        | (uint32_t) input[3] << 24;
}

static uint64_t get_u64(const unsigned char input[8])
{
    return (uint64_t) get_u32(input)
        | (uint64_t) get_u32(input + 4) << 32;
}

static bool add_size(size_t *value, size_t addition)
{
    if (value == NULL || addition > SIZE_MAX - *value) return false;
    *value += addition;
    return true;
}

static bool bounded_length(const char *text, size_t capacity, size_t *length)
{
    if (text == NULL || capacity == 0) return false;
    const char *end = memchr(text, '\0', capacity);
    if (end == NULL) return false;
    if (length != NULL) *length = (size_t) (end - text);
    return true;
}

static bool field_size(const char *text, size_t capacity, size_t *size)
{
    size_t length = 0;
    return bounded_length(text, capacity, &length)
        && length <= UINT32_MAX && add_size(size, 4u)
        && add_size(size, length);
}

static bool blob_size(size_t length, size_t *size)
{
    return length <= UINT32_MAX && add_size(size, 4u)
        && add_size(size, length);
}

static void writer_bytes(PersistWriter *writer, const void *data,
                         size_t length)
{
    if (writer == NULL || writer->failed) return;
    if (!add_size(&writer->bytes, length)
        || (writer->file != NULL
            && fwrite(data, 1, length, writer->file) != length)) {
        writer->failed = true;
        return;
    }
    writer->hash = hash_update(writer->hash, data, length);
}

static void writer_u32(PersistWriter *writer, uint32_t value)
{
    unsigned char encoded[4];
    put_u32(encoded, value);
    writer_bytes(writer, encoded, sizeof(encoded));
}

static void writer_u64(PersistWriter *writer, uint64_t value)
{
    unsigned char encoded[8];
    put_u64(encoded, value);
    writer_bytes(writer, encoded, sizeof(encoded));
}

static void writer_field(PersistWriter *writer, const char *text)
{
    size_t length = strlen(text);
    writer_u32(writer, (uint32_t) length);
    writer_bytes(writer, text, length);
}

static void writer_blob(PersistWriter *writer, const void *data,
                        size_t length)
{
    writer_u32(writer, (uint32_t) length);
    writer_bytes(writer, data, length);
}

static bool generic_cache_entry(const BrowserCacheEntry *entry)
{
    return entry != NULL && entry->data != NULL && entry->length != 0
        /* Version 1 has no fields for the resource partition or grant.
           Keeping these entries memory-only is safer than restoring them as
           URL-only data with their authority silently stripped. */
        && !entry->resource_grant_valid
        && !entry->classic_script_origin_variant
        && entry->module_request_fragment == NULL
        && entry->module_effective_url == NULL
        && entry->module_initiator_origin == NULL
        && !entry->module_cors_validated
        && !entry->module_cors_redirect_origin_tainted
        && !entry->module_javascript_mime_validated;
}

static bool cache_record_size(const BrowserCacheEntry *entry, size_t *size)
{
    size_t total = 8u + 4u;
    if (!generic_cache_entry(entry)
        || !field_size(entry->url, sizeof(entry->url), &total)
        || !blob_size(entry->length, &total)
        || !field_size(entry->etag, sizeof(entry->etag), &total)
        || !field_size(entry->last_modified,
                       sizeof(entry->last_modified), &total)
        || !field_size(entry->content_type,
                       sizeof(entry->content_type), &total)
        || !field_size(entry->vary, sizeof(entry->vary), &total)
        || !field_size(entry->response_referrer_policy,
                       sizeof(entry->response_referrer_policy), &total)) {
        return false;
    }
    const char *response = entry->response_url == NULL
        ? "" : entry->response_url;
    if (!field_size(response, TILEFINCH_URL_SERIALIZED_LIMIT, &total)
        || total > UINT32_MAX) return false;
    *size = total;
    return true;
}

static bool storage_record_size(const BrowserStorageEntry *entry,
                                size_t *size)
{
    size_t total = 0;
    if (entry == NULL || entry->value == NULL || !entry->local
        || !field_size(entry->origin, sizeof(entry->origin), &total)
        || !field_size(entry->key, sizeof(entry->key), &total)
        || !blob_size(entry->value_length, &total)
        || total > UINT32_MAX) {
        return false;
    }
    *size = total;
    return true;
}

static uint32_t cache_flags(const BrowserCacheEntry *entry)
{
    return (entry->no_cache ? UINT32_C(1) : 0)
        | (entry->must_revalidate ? UINT32_C(2) : 0)
        | (entry->immutable ? UINT32_C(4) : 0)
        | (entry->response_url_known ? UINT32_C(8) : 0)
        | (entry->response_referrer_policy_known ? UINT32_C(16) : 0);
}

static bool emit_payload(PersistWriter *writer,
                         const BrowserSession *session,
                         const PersistPlan *plan)
{
    if ((plan->mask & BROWSER_SESSION_PERSIST_CACHE) != 0) {
        /*
         * Oldest-first records let a smaller live cache restore the most
         * recently used tail without trusting persisted clock magnitudes.
         */
        uint8_t order[BROWSER_CACHE_ENTRIES];
        size_t count = 0;
        for (size_t i = 0; i < BROWSER_CACHE_ENTRIES; i++) {
            if ((plan->cache_slots & (UINT64_C(1) << i)) == 0) continue;
            size_t at = count;
            while (at != 0) {
                size_t previous = order[at - 1u];
                if (session->cache[previous].stamp
                        < session->cache[i].stamp
                    || (session->cache[previous].stamp
                            == session->cache[i].stamp
                        && previous < i)) {
                    break;
                }
                order[at] = order[at - 1u];
                at--;
            }
            order[at] = (uint8_t) i;
            count++;
        }
        for (size_t ordered = 0; ordered < count; ordered++) {
            const BrowserCacheEntry *entry =
                &session->cache[order[ordered]];
            if (!generic_cache_entry(entry)) continue;
            size_t size = 0;
            if (!cache_record_size(entry, &size)) return false;
            writer_u32(writer, RECORD_CACHE);
            writer_u32(writer, (uint32_t) size);
            writer_field(writer, entry->url);
            writer_blob(writer, entry->data, entry->length);
            writer_u64(writer, (uint64_t) ordered + 1u);
            writer_field(writer, entry->etag);
            writer_field(writer, entry->last_modified);
            writer_field(writer, entry->content_type);
            writer_field(writer, entry->vary);
            writer_u32(writer, cache_flags(entry));
            writer_field(writer, entry->response_url == NULL
                                  ? "" : entry->response_url);
            writer_field(writer, entry->response_referrer_policy);
        }
    }
    if ((plan->mask & BROWSER_SESSION_PERSIST_LOCAL_STORAGE) != 0) {
        for (size_t i = 0; i < BROWSER_STORAGE_ENTRIES; i++) {
            const BrowserStorageEntry *entry = &session->storage[i];
            if (entry->value == NULL || !entry->local) continue;
            size_t size = 0;
            if (!storage_record_size(entry, &size)) return false;
            writer_u32(writer, RECORD_LOCAL_STORAGE);
            writer_u32(writer, (uint32_t) size);
            writer_field(writer, entry->origin);
            writer_field(writer, entry->key);
            writer_blob(writer, entry->value, entry->value_length);
        }
    }
    return !writer->failed;
}

void browser_session_persistence_limits_default(
    BrowserSessionPersistenceLimits *limits)
{
    if (limits == NULL) return;
    *limits = (BrowserSessionPersistenceLimits) {
        .maximum_file_bytes = BROWSER_SESSION_PERSIST_MAX_FILE_BYTES,
        .maximum_cache_bytes = BROWSER_SESSION_PERSIST_MAX_CACHE_BYTES,
        .maximum_cache_entries = BROWSER_CACHE_ENTRIES,
        .maximum_local_storage_entries = BROWSER_STORAGE_ENTRIES
    };
}

static bool effective_limits(
    const BrowserSessionPersistenceLimits *requested,
    BrowserSessionPersistenceLimits *limits)
{
    browser_session_persistence_limits_default(limits);
    if (requested != NULL) *limits = *requested;
    return limits->maximum_file_bytes >= PERSIST_HEADER_BYTES
        && limits->maximum_file_bytes
               <= BROWSER_SESSION_PERSIST_MAX_FILE_BYTES
        && limits->maximum_cache_bytes != 0
        && limits->maximum_cache_bytes
               <= BROWSER_SESSION_PERSIST_MAX_CACHE_BYTES
        && limits->maximum_cache_entries != 0
        && limits->maximum_cache_entries <= BROWSER_CACHE_ENTRIES
        && limits->maximum_local_storage_entries != 0
        && limits->maximum_local_storage_entries
               <= BROWSER_STORAGE_ENTRIES;
}

static bool make_path(const char *path, const char *suffix,
                      char output[PERSIST_PATH_BYTES])
{
    if (path == NULL || path[0] == '\0') return false;
    int written = snprintf(output, PERSIST_PATH_BYTES, "%s%s", path, suffix);
    return written > 0 && (size_t) written < PERSIST_PATH_BYTES;
}

static BrowserSessionPersistenceStatus build_plan(
    const BrowserSession *session, BrowserSessionPersistenceMask mask,
    const BrowserSessionPersistenceLimits *limits, PersistPlan *plan)
{
    *plan = (PersistPlan) {.mask = mask};
    if ((mask & BROWSER_SESSION_PERSIST_CACHE) != 0) {
        uint8_t order[BROWSER_CACHE_ENTRIES];
        size_t count = 0;
        for (size_t i = 0; i < BROWSER_CACHE_ENTRIES; i++) {
            const BrowserCacheEntry *entry = &session->cache[i];
            if (!generic_cache_entry(entry)) continue;
            size_t at = count;
            while (at != 0) {
                size_t previous = order[at - 1u];
                if (session->cache[previous].stamp
                        > entry->stamp
                    || (session->cache[previous].stamp
                            == entry->stamp
                        && previous > i)) {
                    break;
                }
                order[at] = order[at - 1u];
                at--;
            }
            order[at] = (uint8_t) i;
            count++;
        }
        /*
         * Cache persistence is an LRU snapshot, not durable application data.
         * When its independent disk ceiling is below the resident cache,
         * retain the newest entries that fit instead of failing the entire
         * transactional save. emit_payload() writes this selected set in the
         * opposite (oldest-first) order so a still-smaller restore keeps the
         * hottest tail.
         */
        for (size_t ordered = 0; ordered < count; ordered++) {
            size_t slot = order[ordered];
            const BrowserCacheEntry *entry = &session->cache[slot];
            if (plan->cache_count >= limits->maximum_cache_entries) break;
            if (entry->length > limits->maximum_cache_bytes
                || plan->cache_bytes
                       > limits->maximum_cache_bytes - entry->length) {
                continue;
            }
            plan->cache_slots |= UINT64_C(1) << slot;
            plan->cache_count++;
            plan->cache_bytes += entry->length;
        }
    }
    if ((mask & BROWSER_SESSION_PERSIST_LOCAL_STORAGE) != 0) {
        for (size_t i = 0; i < BROWSER_STORAGE_ENTRIES; i++) {
            const BrowserStorageEntry *entry = &session->storage[i];
            if (entry->value != NULL && entry->local
                && ++plan->storage_count
                       > limits->maximum_local_storage_entries) {
                return BROWSER_SESSION_PERSISTENCE_LIMIT_EXCEEDED;
            }
        }
    }
    return BROWSER_SESSION_PERSISTENCE_OK;
}

static void build_header(unsigned char header[PERSIST_HEADER_BYTES],
                         const PersistPlan *plan, size_t payload_bytes,
                         uint32_t hash)
{
    memset(header, 0, PERSIST_HEADER_BYTES);
    memcpy(header, persist_magic, sizeof(persist_magic));
    put_u32(header + 8, PERSIST_VERSION);
    put_u32(header + 12, PERSIST_HEADER_BYTES);
    put_u32(header + 16, plan->mask);
    put_u32(header + 20, (uint32_t) payload_bytes);
    put_u32(header + 24, hash);
    put_u32(header + 28, (uint32_t) plan->cache_count);
    put_u32(header + 32, (uint32_t) plan->storage_count);
}

static bool install_temporary(const char *temporary, const char *path,
                              const char *backup)
{
    /*
     * Rotate the primary through backup before installing the closed temporary
     * file. POSIX replaces an older backup atomically. FAT may refuse that
     * replacement, in which case remove only the older backup while the
     * primary is still intact and retry.
     */
    bool had_previous = rename(path, backup) == 0;
    if (!had_previous) {
        if (errno == ENOENT) return rename(temporary, path) == 0;
        (void) remove(backup);
        had_previous = rename(path, backup) == 0;
        if (!had_previous && errno != ENOENT) return false;
    }
    if (rename(temporary, path) == 0) return true;
    if (had_previous) (void) rename(backup, path);
    return false;
}

BrowserSessionPersistenceStatus browser_session_persistence_save(
    const BrowserSession *session, const char *path,
    BrowserSessionPersistenceMask mask,
    const BrowserSessionPersistenceLimits *requested_limits)
{
    BrowserSessionPersistenceLimits limits;
    char temporary[PERSIST_PATH_BYTES], backup[PERSIST_PATH_BYTES];
    if (session == NULL || session->budget == NULL
        || mask == 0 || (mask & ~BROWSER_SESSION_PERSIST_ALL) != 0
        || !effective_limits(requested_limits, &limits)
        || !make_path(path, ".tmp", temporary)
        || !make_path(path, ".bak", backup)) {
        return BROWSER_SESSION_PERSISTENCE_INVALID_ARGUMENT;
    }
    PersistPlan plan;
    BrowserSessionPersistenceStatus status =
        build_plan(session, mask, &limits, &plan);
    if (status != BROWSER_SESSION_PERSISTENCE_OK) return status;
    PersistWriter sizing = {.hash = UINT32_C(2166136261)};
    if (!emit_payload(&sizing, session, &plan) || sizing.failed
        || sizing.bytes > UINT32_MAX
        || sizing.bytes > limits.maximum_file_bytes - PERSIST_HEADER_BYTES) {
        return BROWSER_SESSION_PERSISTENCE_LIMIT_EXCEEDED;
    }
    unsigned char header[PERSIST_HEADER_BYTES];
    build_header(header, &plan, sizing.bytes, sizing.hash);
    FILE *file = fopen(temporary, "wb");
    if (file == NULL) return BROWSER_SESSION_PERSISTENCE_IO_ERROR;
    bool ok = fwrite(header, 1, sizeof(header), file) == sizeof(header);
    PersistWriter writer = {
        .file = file, .hash = UINT32_C(2166136261)
    };
    if (ok) ok = emit_payload(&writer, session, &plan)
        && writer.bytes == sizing.bytes && writer.hash == sizing.hash;
    ok = fclose(file) == 0 && ok;
    if (!ok) {
        (void) remove(temporary);
        return BROWSER_SESSION_PERSISTENCE_IO_ERROR;
    }
    if (!install_temporary(temporary, path, backup)) {
        (void) remove(temporary);
        return BROWSER_SESSION_PERSISTENCE_IO_ERROR;
    }
    return BROWSER_SESSION_PERSISTENCE_OK;
}

static bool http_field_value_valid(const char *value)
{
    if (value == NULL) return false;
    for (const unsigned char *at = (const unsigned char *) value;
         *at != '\0'; at++) {
        if ((*at < 0x20u && *at != '\t') || *at == 0x7fu)
            return false;
    }
    return true;
}

static BrowserCacheEntry *cache_entry_direct(BrowserSession *session,
                                             const char *url)
{
    char key[TILEFINCH_URL_SERIALIZED_LIMIT];
    if (!tilefinch_url_request_key(url, key, sizeof(key))) return NULL;
    for (size_t i = 0; i < BROWSER_CACHE_ENTRIES; i++) {
        if (session->cache[i].data != NULL
            && strcmp(session->cache[i].url, key) == 0)
            return &session->cache[i];
    }
    return NULL;
}

static bool local_commit_fits(const BrowserSession *target,
                              const BrowserSession *staging)
{
    size_t session_count = 0, session_bytes = 0, local_count = 0;
    for (size_t i = 0; i < BROWSER_STORAGE_ENTRIES; i++) {
        const BrowserStorageEntry *entry = &target->storage[i];
        if (entry->value != NULL && !entry->local) {
            session_count++;
            if (entry->value_length > SIZE_MAX - session_bytes) return false;
            session_bytes += entry->value_length;
        }
        if (staging->storage[i].value != NULL
            && staging->storage[i].local) local_count++;
    }
    return session_bytes <= target->maximum_storage_bytes
        && local_count <= BROWSER_STORAGE_ENTRIES - session_count
        && staging->storage_bytes
               <= target->maximum_storage_bytes - session_bytes;
}

static void commit_staging(BrowserSession *target, BrowserSession *staging,
                           BrowserSessionPersistenceMask mask)
{
    if ((mask & BROWSER_SESSION_PERSIST_CACHE) != 0) {
        browser_session_cache_clear(target);
        memcpy(target->cache, staging->cache, sizeof(target->cache));
        memset(staging->cache, 0, sizeof(staging->cache));
        target->cache_bytes = staging->cache_bytes;
        staging->cache_bytes = 0;
        target->clock = staging->clock;
    }
    if ((mask & BROWSER_SESSION_PERSIST_LOCAL_STORAGE) != 0) {
        browser_session_storage_clear_all(target, true);
        for (size_t source = 0; source < BROWSER_STORAGE_ENTRIES; source++) {
            BrowserStorageEntry *entry = &staging->storage[source];
            if (entry->value == NULL || !entry->local) continue;
            for (size_t destination = 0;
                 destination < BROWSER_STORAGE_ENTRIES; destination++) {
                if (target->storage[destination].value == NULL) {
                    target->storage[destination] = *entry;
                    memset(entry, 0, sizeof(*entry));
                    break;
                }
            }
        }
        target->storage_bytes += staging->storage_bytes;
        staging->storage_bytes = 0;
    }
}

static void async_release_attempt(BrowserSessionPersistenceLoad *load)
{
    if (load->file != NULL) {
        fclose(load->file);
        load->file = NULL;
    }
    budget_free(load->target->budget, load->value);
    load->value = NULL;
    if (load->staging != NULL) {
        browser_session_destroy(load->staging);
        budget_free(load->target->budget, load->staging);
        load->staging = NULL;
    }
}

static void async_reset_fields(BrowserSessionPersistenceLoad *load)
{
    load->stored_mask = 0;
    load->phase = ASYNC_LOAD_HEADER;
    load->header_read = 0;
    load->payload_remaining = 0;
    load->record_remaining = 0;
    load->cache_bytes = 0;
    load->parsed_cache = 0;
    load->parsed_storage = 0;
    load->expected_cache = 0;
    load->expected_storage = 0;
    load->expected_hash = 0;
    load->hash = UINT32_C(2166136261);
    load->scalar_read = 0;
    load->scalar_size = 0;
    load->value_read = 0;
    load->value_size = 0;
    load->record_type = 0;
    load->record_bytes = 0;
    load->body_length = 0;
    load->stamp = 0;
    load->flags = 0;
    load->body_skipped = false;
    memset(load->header, 0, sizeof(load->header));
}

static BrowserSessionPersistenceStatus async_open_attempt(
    BrowserSessionPersistenceLoad *load, const char *path)
{
    async_release_attempt(load);
    async_reset_fields(load);
    load->file = fopen(path, "rb");
    if (load->file == NULL) {
        return errno == ENOENT ? BROWSER_SESSION_PERSISTENCE_NOT_FOUND
                               : BROWSER_SESSION_PERSISTENCE_IO_ERROR;
    }
    return BROWSER_SESSION_PERSISTENCE_OK;
}

static bool async_read(
    BrowserSessionPersistenceLoad *load, void *destination, size_t length,
    size_t *offset, size_t *budget, bool payload, bool record)
{
    if (*offset >= length) return true;
    if (*budget == 0 || load->status != BROWSER_SESSION_PERSISTENCE_OK)
        return false;
    size_t chunk = length - *offset;
    if (chunk > *budget) chunk = *budget;
    if (payload && chunk > load->payload_remaining) {
        load->status = BROWSER_SESSION_PERSISTENCE_CORRUPT;
        return false;
    }
    if (record && chunk > load->record_remaining) {
        load->status = BROWSER_SESSION_PERSISTENCE_CORRUPT;
        return false;
    }
    unsigned char *at = (unsigned char *) destination + *offset;
    size_t received = fread(at, 1, chunk, load->file);
    if (received != chunk) {
        load->status = ferror(load->file)
            ? BROWSER_SESSION_PERSISTENCE_IO_ERROR
            : BROWSER_SESSION_PERSISTENCE_CORRUPT;
        return false;
    }
    if (payload) {
        load->payload_remaining -= received;
        load->hash = hash_update(load->hash, at, received);
    }
    if (record) load->record_remaining -= received;
    *offset += received;
    *budget -= received;
    return *offset == length;
}

static bool async_read_scalar(
    BrowserSessionPersistenceLoad *load, size_t bytes, size_t *budget,
    bool record)
{
    if (load->scalar_size == 0) load->scalar_size = bytes;
    if (load->scalar_size != bytes) {
        load->status = BROWSER_SESSION_PERSISTENCE_CORRUPT;
        return false;
    }
    return async_read(
        load, load->scalar, bytes, &load->scalar_read, budget, true, record);
}

static uint32_t async_take_u32(BrowserSessionPersistenceLoad *load)
{
    uint32_t value = get_u32(load->scalar);
    load->scalar_read = 0;
    load->scalar_size = 0;
    return value;
}

static uint64_t async_take_u64(BrowserSessionPersistenceLoad *load)
{
    uint64_t value = get_u64(load->scalar);
    load->scalar_read = 0;
    load->scalar_size = 0;
    return value;
}

static bool async_field_length(
    BrowserSessionPersistenceLoad *load, size_t capacity, bool allow_empty,
    size_t *budget)
{
    if (!async_read_scalar(load, 4u, budget, true)) return false;
    uint32_t length = async_take_u32(load);
    if (capacity == 0 || length >= capacity
        || (!allow_empty && length == 0)) {
        load->status = BROWSER_SESSION_PERSISTENCE_CORRUPT;
        return false;
    }
    load->value_size = length;
    load->value_read = 0;
    return true;
}

static bool async_field_body(
    BrowserSessionPersistenceLoad *load, char *value, size_t capacity,
    size_t *budget)
{
    if (load->value_size >= capacity) {
        load->status = BROWSER_SESSION_PERSISTENCE_CORRUPT;
        return false;
    }
    if (!async_read(
            load, value, load->value_size, &load->value_read,
            budget, true, true)) return false;
    if (memchr(value, '\0', load->value_size) != NULL) {
        load->status = BROWSER_SESSION_PERSISTENCE_CORRUPT;
        return false;
    }
    value[load->value_size] = '\0';
    load->value_size = 0;
    load->value_read = 0;
    return true;
}

static bool async_commit_cache(BrowserSessionPersistenceLoad *load)
{
    BrowserSession *staging = load->staging;
    size_t body_length = load->body_length;
    if (body_length > load->limits.maximum_cache_bytes
        || load->cache_bytes
               > load->limits.maximum_cache_bytes - body_length) {
        load->status = BROWSER_SESSION_PERSISTENCE_LIMIT_EXCEEDED;
        return false;
    }
    load->cache_bytes += body_length;
    char request_key[TILEFINCH_URL_SERIALIZED_LIMIT];
    if ((load->flags & ~UINT32_C(31)) != 0
        || ((load->flags & UINT32_C(16)) != 0
            && (load->flags & UINT32_C(8)) == 0)
        || load->stamp > SIZE_MAX
        || !http_field_value_valid(load->etag)
        || !http_field_value_valid(load->modified)
        || !http_field_value_valid(load->content_type)
        || !http_field_value_valid(load->vary)
        || !tilefinch_url_request_key(
               load->url, request_key, sizeof(request_key))
        || strcmp(load->url, request_key) != 0
        || cache_entry_direct(staging, load->url) != NULL) {
        load->status = BROWSER_SESSION_PERSISTENCE_CORRUPT;
        return false;
    }
    if (load->body_skipped) return true;
    BrowserSharedBody *body = browser_shared_body_take(
        staging->budget, load->value, body_length);
    if (body == NULL) {
        load->status = BROWSER_SESSION_PERSISTENCE_OUT_OF_MEMORY;
        return false;
    }
    load->value = NULL;
    char cache_control[48] = {0};
    if ((load->flags & UINT32_C(1)) != 0)
        strcat(cache_control, "no-cache");
    if ((load->flags & UINT32_C(2)) != 0)
        strcat(cache_control, cache_control[0] == '\0'
                              ? "must-revalidate" : ",must-revalidate");
    if ((load->flags & UINT32_C(4)) != 0)
        strcat(cache_control, cache_control[0] == '\0'
                              ? "immutable" : ",immutable");
    bool stored = browser_session_cache_put_http_shared(
        staging, load->url, body, load->etag, load->modified,
        load->content_type, cache_control, load->vary, 0);
    browser_shared_body_release(body);
    BrowserCacheEntry *entry = stored
        ? cache_entry_direct(staging, load->url) : NULL;
    if (entry == NULL) {
        load->status = BROWSER_SESSION_PERSISTENCE_CORRUPT;
        return false;
    }
    entry->stored_at_ns = 0;
    entry->fresh_until_ns = 0;
    entry->no_cache = true;
    entry->must_revalidate = (load->flags & UINT32_C(2)) != 0;
    entry->immutable = (load->flags & UINT32_C(4)) != 0;
    bool provenance_ok = true;
    if ((load->flags & UINT32_C(8)) != 0) {
        const char *final_url = load->response[0] == '\0'
            ? load->url : load->response;
        provenance_ok = (load->flags & UINT32_C(16)) != 0
            ? browser_session_cache_set_response_provenance(
                  staging, load->url, final_url, load->policy)
            : browser_session_cache_set_response_url(
                  staging, load->url, final_url);
    } else if (load->response[0] != '\0' || load->policy[0] != '\0') {
        provenance_ok = false;
    }
    if (!provenance_ok) {
        load->status = BROWSER_SESSION_PERSISTENCE_CORRUPT;
        return false;
    }
    return true;
}

static bool async_commit_storage(BrowserSessionPersistenceLoad *load)
{
    char normalized[BROWSER_ORIGIN_LIMIT];
    const char *old_value = NULL;
    if (!tilefinch_url_origin(
            load->origin, normalized, sizeof(normalized))
        || strcmp(load->origin, normalized) != 0
        || browser_session_storage_get(
               load->staging, load->origin, true, load->key,
               &old_value, NULL)) {
        load->status = BROWSER_SESSION_PERSISTENCE_CORRUPT;
        return false;
    }
    if (!browser_session_storage_set(
            load->staging, load->origin, true, load->key,
            (const char *) load->value, load->body_length)) {
        load->status = BROWSER_SESSION_PERSISTENCE_OUT_OF_MEMORY;
        return false;
    }
    budget_free(load->target->budget, load->value);
    load->value = NULL;
    return true;
}

static bool async_prepare_staging(BrowserSessionPersistenceLoad *load)
{
    load->staging = budget_calloc_category(
        load->target->budget, BUDGET_CATEGORY_SESSION,
        1, sizeof(*load->staging));
    if (load->staging == NULL) return false;
    size_t cache_limit = load->target->maximum_cache_bytes;
    if (cache_limit > load->limits.maximum_cache_bytes)
        cache_limit = load->limits.maximum_cache_bytes;
    if (!browser_session_init(
            load->staging, load->target->budget, cache_limit)) {
        budget_free(load->target->budget, load->staging);
        load->staging = NULL;
        return false;
    }
    return true;
}

static void async_select_backup_failure(
    BrowserSessionPersistenceLoad *load,
    BrowserSessionPersistenceStatus backup_status)
{
    if (load->primary_status == BROWSER_SESSION_PERSISTENCE_NOT_FOUND
        || (backup_status != BROWSER_SESSION_PERSISTENCE_NOT_FOUND
            && backup_status != BROWSER_SESSION_PERSISTENCE_CORRUPT)) {
        load->status = backup_status;
    } else {
        load->status = load->primary_status;
    }
    load->phase = ASYNC_LOAD_FINISHED;
}

static bool async_handle_failure(BrowserSessionPersistenceLoad *load)
{
    if (load->status == BROWSER_SESSION_PERSISTENCE_OK) return false;
    if (!load->trying_backup
        && (load->status == BROWSER_SESSION_PERSISTENCE_NOT_FOUND
            || load->status == BROWSER_SESSION_PERSISTENCE_CORRUPT)) {
        load->primary_status = load->status;
        load->trying_backup = true;
        BrowserSessionPersistenceStatus backup_status =
            async_open_attempt(load, load->backup);
        load->status = backup_status;
        if (backup_status != BROWSER_SESSION_PERSISTENCE_OK)
            async_select_backup_failure(load, backup_status);
        return true;
    }
    if (load->trying_backup)
        async_select_backup_failure(load, load->status);
    else
        load->phase = ASYNC_LOAD_FINISHED;
    return true;
}

BrowserSessionPersistenceLoad *browser_session_persistence_load_begin(
    BrowserSession *session, const char *path,
    BrowserSessionPersistenceMask mask,
    const BrowserSessionPersistenceLimits *requested_limits)
{
    BrowserSessionPersistenceLimits limits;
    char backup[PERSIST_PATH_BYTES];
    if (session == NULL || session->budget == NULL
        || mask == 0 || (mask & ~BROWSER_SESSION_PERSIST_ALL) != 0
        || !effective_limits(requested_limits, &limits)
        || !make_path(path, ".bak", backup)) return NULL;
    BrowserSessionPersistenceLoad *load = budget_calloc_category(
        session->budget, BUDGET_CATEGORY_SESSION, 1, sizeof(*load));
    if (load == NULL) return NULL;
    load->target = session;
    load->limits = limits;
    load->requested_mask = mask;
    load->status = BROWSER_SESSION_PERSISTENCE_OK;
    load->primary_status = BROWSER_SESSION_PERSISTENCE_OK;
    snprintf(load->primary, sizeof(load->primary), "%s", path);
    snprintf(load->backup, sizeof(load->backup), "%s", backup);
    BrowserSessionPersistenceStatus open_status =
        async_open_attempt(load, load->primary);
    load->status = open_status;
    if (open_status != BROWSER_SESSION_PERSISTENCE_OK)
        (void) async_handle_failure(load);
    return load;
}

static void async_finish_record(BrowserSessionPersistenceLoad *load)
{
    if (load->record_remaining != 0) {
        load->status = BROWSER_SESSION_PERSISTENCE_CORRUPT;
        return;
    }
    if (load->record_type == RECORD_CACHE) {
        if (!async_commit_cache(load)) return;
        load->parsed_cache++;
    } else {
        if (!async_commit_storage(load)) return;
        load->parsed_storage++;
    }
    load->phase = load->payload_remaining == 0
        ? ASYNC_LOAD_TRAILING : ASYNC_LOAD_RECORD_TYPE;
}

BrowserSessionPersistenceLoadProgress browser_session_persistence_load_pump(
    BrowserSessionPersistenceLoad *load, size_t maximum_bytes,
    BrowserSessionPersistenceStatus *reported_status)
{
    if (reported_status != NULL)
        *reported_status = BROWSER_SESSION_PERSISTENCE_INVALID_ARGUMENT;
    if (load == NULL || maximum_bytes == 0)
        return BROWSER_SESSION_PERSISTENCE_LOAD_FAILED;
    size_t budget = maximum_bytes;
    unsigned transitions = 0;
    while (load->phase != ASYNC_LOAD_FINISHED
           && load->status == BROWSER_SESSION_PERSISTENCE_OK
           && budget != 0 && transitions++ < 64u) {
        switch (load->phase) {
        case ASYNC_LOAD_HEADER:
            if (!async_read(
                    load, load->header, sizeof(load->header),
                    &load->header_read, &budget, false, false)) break;
            load->stored_mask = get_u32(load->header + 16);
            load->payload_remaining = get_u32(load->header + 20);
            load->expected_hash = get_u32(load->header + 24);
            load->expected_cache = get_u32(load->header + 28);
            load->expected_storage = get_u32(load->header + 32);
            if (memcmp(
                    load->header, persist_magic, sizeof(persist_magic)) != 0
                || get_u32(load->header + 8) != PERSIST_VERSION
                || get_u32(load->header + 12) != PERSIST_HEADER_BYTES
                || load->stored_mask == 0
                || (load->stored_mask & ~BROWSER_SESSION_PERSIST_ALL) != 0
                || load->payload_remaining
                       > load->limits.maximum_file_bytes
                             - PERSIST_HEADER_BYTES
                || load->expected_cache
                       > load->limits.maximum_cache_entries
                || load->expected_storage
                       > load->limits.maximum_local_storage_entries) {
                load->status = BROWSER_SESSION_PERSISTENCE_CORRUPT;
                break;
            }
            if (!async_prepare_staging(load)) {
                load->status = BROWSER_SESSION_PERSISTENCE_OUT_OF_MEMORY;
                break;
            }
            load->phase = load->payload_remaining == 0
                ? ASYNC_LOAD_TRAILING : ASYNC_LOAD_RECORD_TYPE;
            break;
        case ASYNC_LOAD_RECORD_TYPE:
            if (!async_read_scalar(load, 4u, &budget, false)) break;
            load->record_type = async_take_u32(load);
            load->phase = ASYNC_LOAD_RECORD_BYTES;
            break;
        case ASYNC_LOAD_RECORD_BYTES:
            if (!async_read_scalar(load, 4u, &budget, false)) break;
            load->record_bytes = async_take_u32(load);
            if (load->record_bytes > load->payload_remaining
                || (load->record_type != RECORD_CACHE
                    && load->record_type != RECORD_LOCAL_STORAGE)
                || (load->record_type == RECORD_CACHE
                    && (load->stored_mask
                        & BROWSER_SESSION_PERSIST_CACHE) == 0)
                || (load->record_type == RECORD_LOCAL_STORAGE
                    && (load->stored_mask
                        & BROWSER_SESSION_PERSIST_LOCAL_STORAGE) == 0)) {
                load->status = BROWSER_SESSION_PERSISTENCE_CORRUPT;
                break;
            }
            load->record_remaining = load->record_bytes;
            load->phase = load->record_type == RECORD_CACHE
                ? ASYNC_LOAD_CACHE_URL_LENGTH
                : ASYNC_LOAD_STORAGE_ORIGIN_LENGTH;
            break;
#define ASYNC_FIELD_LENGTH_CASE(phase_name, next_phase, member, allow) \
        case phase_name: \
            if (async_field_length( \
                    load, sizeof(load->member), allow, &budget)) \
                load->phase = next_phase; \
            break
#define ASYNC_FIELD_BODY_CASE(phase_name, next_phase, member) \
        case phase_name: \
            if (async_field_body( \
                    load, load->member, sizeof(load->member), &budget)) \
                load->phase = next_phase; \
            break
        ASYNC_FIELD_LENGTH_CASE(
            ASYNC_LOAD_CACHE_URL_LENGTH, ASYNC_LOAD_CACHE_URL, url, false);
        ASYNC_FIELD_BODY_CASE(
            ASYNC_LOAD_CACHE_URL, ASYNC_LOAD_CACHE_BODY_LENGTH, url);
        case ASYNC_LOAD_CACHE_BODY_LENGTH:
            if (!async_read_scalar(load, 4u, &budget, true)) break;
            load->body_length = async_take_u32(load);
            if (load->body_length > load->record_remaining) {
                load->status = BROWSER_SESSION_PERSISTENCE_CORRUPT;
                break;
            }
            if (load->body_length > load->limits.maximum_cache_bytes
                || load->body_length == 0
                || load->body_length + 1u < load->body_length) {
                load->status = BROWSER_SESSION_PERSISTENCE_LIMIT_EXCEEDED;
                break;
            }
            load->body_skipped =
                load->body_length > load->staging->maximum_cache_bytes;
            if (!load->body_skipped) {
                load->value = budget_malloc_category(
                    load->target->budget, BUDGET_CATEGORY_SESSION,
                    load->body_length + 1u);
                if (load->value == NULL) {
                    load->status = BROWSER_SESSION_PERSISTENCE_OUT_OF_MEMORY;
                    break;
                }
            }
            load->value_read = 0;
            load->phase = ASYNC_LOAD_CACHE_BODY;
            break;
        case ASYNC_LOAD_CACHE_BODY: {
            unsigned char skipped[PERSIST_SKIP_BUFFER_BYTES];
            size_t target = load->body_skipped
                ? load->body_length - load->value_read
                : load->body_length;
            if (load->body_skipped) {
                size_t chunk = target < sizeof(skipped)
                    ? target : sizeof(skipped);
                if (chunk > budget) chunk = budget;
                size_t at = 0;
                if (!async_read(
                        load, skipped, chunk, &at, &budget, true, true))
                    break;
                load->value_read += chunk;
                if (load->value_read != load->body_length) break;
            } else if (!async_read(
                           load, load->value, target, &load->value_read,
                           &budget, true, true)) break;
            if (load->value != NULL)
                load->value[load->body_length] = 0;
            load->value_read = 0;
            load->phase = ASYNC_LOAD_CACHE_STAMP;
            break;
        }
        case ASYNC_LOAD_CACHE_STAMP:
            if (!async_read_scalar(load, 8u, &budget, true)) break;
            load->stamp = async_take_u64(load);
            load->phase = ASYNC_LOAD_CACHE_ETAG_LENGTH;
            break;
        ASYNC_FIELD_LENGTH_CASE(
            ASYNC_LOAD_CACHE_ETAG_LENGTH, ASYNC_LOAD_CACHE_ETAG,
            etag, true);
        ASYNC_FIELD_BODY_CASE(
            ASYNC_LOAD_CACHE_ETAG, ASYNC_LOAD_CACHE_MODIFIED_LENGTH, etag);
        ASYNC_FIELD_LENGTH_CASE(
            ASYNC_LOAD_CACHE_MODIFIED_LENGTH, ASYNC_LOAD_CACHE_MODIFIED,
            modified, true);
        ASYNC_FIELD_BODY_CASE(
            ASYNC_LOAD_CACHE_MODIFIED,
            ASYNC_LOAD_CACHE_CONTENT_TYPE_LENGTH, modified);
        ASYNC_FIELD_LENGTH_CASE(
            ASYNC_LOAD_CACHE_CONTENT_TYPE_LENGTH,
            ASYNC_LOAD_CACHE_CONTENT_TYPE, content_type, true);
        ASYNC_FIELD_BODY_CASE(
            ASYNC_LOAD_CACHE_CONTENT_TYPE, ASYNC_LOAD_CACHE_VARY_LENGTH,
            content_type);
        ASYNC_FIELD_LENGTH_CASE(
            ASYNC_LOAD_CACHE_VARY_LENGTH, ASYNC_LOAD_CACHE_VARY,
            vary, true);
        ASYNC_FIELD_BODY_CASE(
            ASYNC_LOAD_CACHE_VARY, ASYNC_LOAD_CACHE_FLAGS, vary);
        case ASYNC_LOAD_CACHE_FLAGS:
            if (!async_read_scalar(load, 4u, &budget, true)) break;
            load->flags = async_take_u32(load);
            load->phase = ASYNC_LOAD_CACHE_RESPONSE_LENGTH;
            break;
        ASYNC_FIELD_LENGTH_CASE(
            ASYNC_LOAD_CACHE_RESPONSE_LENGTH, ASYNC_LOAD_CACHE_RESPONSE,
            response, true);
        ASYNC_FIELD_BODY_CASE(
            ASYNC_LOAD_CACHE_RESPONSE, ASYNC_LOAD_CACHE_POLICY_LENGTH,
            response);
        ASYNC_FIELD_LENGTH_CASE(
            ASYNC_LOAD_CACHE_POLICY_LENGTH, ASYNC_LOAD_CACHE_POLICY,
            policy, true);
        case ASYNC_LOAD_CACHE_POLICY:
            if (async_field_body(
                    load, load->policy, sizeof(load->policy), &budget))
                async_finish_record(load);
            break;
        ASYNC_FIELD_LENGTH_CASE(
            ASYNC_LOAD_STORAGE_ORIGIN_LENGTH, ASYNC_LOAD_STORAGE_ORIGIN,
            origin, false);
        ASYNC_FIELD_BODY_CASE(
            ASYNC_LOAD_STORAGE_ORIGIN, ASYNC_LOAD_STORAGE_KEY_LENGTH,
            origin);
        ASYNC_FIELD_LENGTH_CASE(
            ASYNC_LOAD_STORAGE_KEY_LENGTH, ASYNC_LOAD_STORAGE_KEY,
            key, true);
        ASYNC_FIELD_BODY_CASE(
            ASYNC_LOAD_STORAGE_KEY, ASYNC_LOAD_STORAGE_VALUE_LENGTH, key);
        case ASYNC_LOAD_STORAGE_VALUE_LENGTH:
            if (!async_read_scalar(load, 4u, &budget, true)) break;
            load->body_length = async_take_u32(load);
            if (load->body_length > load->record_remaining) {
                load->status = BROWSER_SESSION_PERSISTENCE_CORRUPT;
                break;
            }
            if (load->body_length
                    > load->staging->maximum_storage_bytes
                || load->body_length + 1u < load->body_length) {
                load->status = BROWSER_SESSION_PERSISTENCE_LIMIT_EXCEEDED;
                break;
            }
            load->value = budget_malloc_category(
                load->target->budget, BUDGET_CATEGORY_SESSION,
                load->body_length + 1u);
            if (load->value == NULL) {
                load->status = BROWSER_SESSION_PERSISTENCE_OUT_OF_MEMORY;
                break;
            }
            load->value_read = 0;
            load->phase = ASYNC_LOAD_STORAGE_VALUE;
            break;
        case ASYNC_LOAD_STORAGE_VALUE:
            if (!async_read(
                    load, load->value, load->body_length,
                    &load->value_read, &budget, true, true)) break;
            load->value[load->body_length] = 0;
            async_finish_record(load);
            break;
        case ASYNC_LOAD_TRAILING: {
            int trailing = fgetc(load->file);
            budget--;
            if (trailing != EOF || ferror(load->file)
                || load->payload_remaining != 0
                || load->hash != load->expected_hash
                || load->parsed_cache != load->expected_cache
                || load->parsed_storage != load->expected_storage) {
                load->status = BROWSER_SESSION_PERSISTENCE_CORRUPT;
                break;
            }
            BrowserSessionPersistenceMask commit_mask =
                load->requested_mask & load->stored_mask;
            if ((commit_mask & BROWSER_SESSION_PERSIST_LOCAL_STORAGE) != 0
                && !local_commit_fits(load->target, load->staging)) {
                load->status = BROWSER_SESSION_PERSISTENCE_LIMIT_EXCEEDED;
                break;
            }
            commit_staging(load->target, load->staging, commit_mask);
            if (load->trying_backup) {
                (void) remove(load->primary);
            }
            load->phase = ASYNC_LOAD_FINISHED;
            break;
        }
        case ASYNC_LOAD_FINISHED:
            break;
        }
#undef ASYNC_FIELD_LENGTH_CASE
#undef ASYNC_FIELD_BODY_CASE
        if (load->status != BROWSER_SESSION_PERSISTENCE_OK)
            (void) async_handle_failure(load);
    }
    if (load->status != BROWSER_SESSION_PERSISTENCE_OK)
        (void) async_handle_failure(load);
    if (reported_status != NULL) *reported_status = load->status;
    if (load->phase != ASYNC_LOAD_FINISHED)
        return BROWSER_SESSION_PERSISTENCE_LOAD_PENDING;
    return load->status == BROWSER_SESSION_PERSISTENCE_OK
        ? BROWSER_SESSION_PERSISTENCE_LOAD_COMPLETE
        : BROWSER_SESSION_PERSISTENCE_LOAD_FAILED;
}

void browser_session_persistence_load_destroy(
    BrowserSessionPersistenceLoad *load)
{
    if (load == NULL || load->target == NULL) return;
    Budget *budget = load->target->budget;
    async_release_attempt(load);
    budget_free(budget, load);
}

BrowserSessionPersistenceStatus browser_session_persistence_load(
    BrowserSession *session, const char *path,
    BrowserSessionPersistenceMask mask,
    const BrowserSessionPersistenceLimits *requested_limits)
{
    BrowserSessionPersistenceLimits limits;
    char backup[PERSIST_PATH_BYTES];
    if (session == NULL || session->budget == NULL
        || mask == 0 || (mask & ~BROWSER_SESSION_PERSIST_ALL) != 0
        || !effective_limits(requested_limits, &limits)
        || !make_path(path, ".bak", backup)) {
        return BROWSER_SESSION_PERSISTENCE_INVALID_ARGUMENT;
    }
    BrowserSessionPersistenceLoad *load =
        browser_session_persistence_load_begin(
            session, path, mask, &limits);
    if (load == NULL)
        return BROWSER_SESSION_PERSISTENCE_OUT_OF_MEMORY;
    BrowserSessionPersistenceStatus status =
        BROWSER_SESSION_PERSISTENCE_OK;
    BrowserSessionPersistenceLoadProgress progress =
        BROWSER_SESSION_PERSISTENCE_LOAD_PENDING;
    while (progress == BROWSER_SESSION_PERSISTENCE_LOAD_PENDING) {
        progress = browser_session_persistence_load_pump(
            load, 64u * 1024u, &status);
    }
    browser_session_persistence_load_destroy(load);
    return status;
}

BrowserSessionPersistenceStatus browser_session_persistence_clear(
    BrowserSession *session, BrowserSessionPersistenceMask mask)
{
    if (session == NULL || session->budget == NULL || mask == 0
        || (mask & ~BROWSER_SESSION_PERSIST_ALL) != 0) {
        return BROWSER_SESSION_PERSISTENCE_INVALID_ARGUMENT;
    }
    if ((mask & BROWSER_SESSION_PERSIST_CACHE) != 0)
        browser_session_cache_clear(session);
    if ((mask & BROWSER_SESSION_PERSIST_LOCAL_STORAGE) != 0)
        browser_session_storage_clear_all(session, true);
    return BROWSER_SESSION_PERSISTENCE_OK;
}

BrowserSessionPersistenceStatus browser_session_persistence_remove(
    const char *path)
{
    char temporary[PERSIST_PATH_BYTES], backup[PERSIST_PATH_BYTES];
    if (!make_path(path, ".tmp", temporary)
        || !make_path(path, ".bak", backup)) {
        return BROWSER_SESSION_PERSISTENCE_INVALID_ARGUMENT;
    }
    const char *paths[] = {temporary, backup, path};
    bool removed = true;
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        if (remove(paths[i]) != 0 && errno != ENOENT)
            removed = false;
    }
    return removed ? BROWSER_SESSION_PERSISTENCE_OK
                   : BROWSER_SESSION_PERSISTENCE_IO_ERROR;
}

const char *browser_session_persistence_status_name(
    BrowserSessionPersistenceStatus status)
{
    static const char *const names[] = {
        "ok", "invalid-argument", "not-found", "io-error",
        "limit-exceeded", "corrupt", "out-of-memory"
    };
    return (unsigned) status < sizeof(names) / sizeof(names[0])
        ? names[status] : "unknown";
}
