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
    FILE *file;
    size_t remaining;
    size_t cache_bytes;
    uint32_t hash;
    BrowserSessionPersistenceStatus status;
} PersistReader;

typedef struct {
    BrowserSessionPersistenceMask mask;
    size_t cache_count;
    size_t storage_count;
    size_t cache_bytes;
    uint64_t cache_slots;
} PersistPlan;

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

static void reader_bytes(PersistReader *reader, void *data, size_t length)
{
    if (reader == NULL
        || reader->status != BROWSER_SESSION_PERSISTENCE_OK) return;
    if (length > reader->remaining) {
        reader->status = BROWSER_SESSION_PERSISTENCE_CORRUPT;
        return;
    }
    if (fread(data, 1, length, reader->file) != length) {
        reader->status = ferror(reader->file)
            ? BROWSER_SESSION_PERSISTENCE_IO_ERROR
            : BROWSER_SESSION_PERSISTENCE_CORRUPT;
        return;
    }
    reader->remaining -= length;
    reader->hash = hash_update(reader->hash, data, length);
}

static uint32_t reader_u32(PersistReader *reader)
{
    unsigned char encoded[4] = {0};
    reader_bytes(reader, encoded, sizeof(encoded));
    return get_u32(encoded);
}

static uint64_t reader_u64(PersistReader *reader)
{
    unsigned char encoded[8] = {0};
    reader_bytes(reader, encoded, sizeof(encoded));
    return get_u64(encoded);
}

static void reader_field(PersistReader *reader, char *output,
                         size_t capacity, bool allow_empty)
{
    uint32_t length = reader_u32(reader);
    if (reader->status != BROWSER_SESSION_PERSISTENCE_OK) return;
    if (capacity == 0 || length >= capacity
        || (!allow_empty && length == 0)) {
        reader->status = BROWSER_SESSION_PERSISTENCE_CORRUPT;
        return;
    }
    reader_bytes(reader, output, length);
    if (reader->status == BROWSER_SESSION_PERSISTENCE_OK
        && memchr(output, '\0', length) != NULL) {
        reader->status = BROWSER_SESSION_PERSISTENCE_CORRUPT;
    } else if (reader->status == BROWSER_SESSION_PERSISTENCE_OK) {
        output[length] = '\0';
    }
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

static unsigned char *reader_blob(PersistReader *reader, Budget *budget,
                                  size_t maximum, size_t allocation_maximum,
                                  size_t *length, bool allow_empty,
                                  bool *skipped)
{
    if (length != NULL) *length = 0;
    if (skipped != NULL) *skipped = false;
    uint32_t encoded_length = reader_u32(reader);
    if (reader->status != BROWSER_SESSION_PERSISTENCE_OK) return NULL;
    if (encoded_length > reader->remaining) {
        reader->status = BROWSER_SESSION_PERSISTENCE_CORRUPT;
        return NULL;
    }
    if (encoded_length > maximum || (!allow_empty && encoded_length == 0)
        || (size_t) encoded_length + 1u < encoded_length) {
        reader->status = BROWSER_SESSION_PERSISTENCE_LIMIT_EXCEEDED;
        return NULL;
    }
    if (length != NULL) *length = encoded_length;
    if (encoded_length > allocation_maximum) {
        unsigned char buffer[PERSIST_SKIP_BUFFER_BYTES];
        size_t remaining = encoded_length;
        while (remaining != 0
               && reader->status == BROWSER_SESSION_PERSISTENCE_OK) {
            size_t chunk = remaining < sizeof(buffer)
                ? remaining : sizeof(buffer);
            reader_bytes(reader, buffer, chunk);
            remaining -= chunk;
        }
        if (reader->status == BROWSER_SESSION_PERSISTENCE_OK
            && skipped != NULL) {
            *skipped = true;
        }
        return NULL;
    }
    unsigned char *value = budget_malloc_category(
        budget, BUDGET_CATEGORY_SESSION, (size_t) encoded_length + 1u);
    if (value == NULL) {
        reader->status = BROWSER_SESSION_PERSISTENCE_OUT_OF_MEMORY;
        return NULL;
    }
    reader_bytes(reader, value, encoded_length);
    if (reader->status != BROWSER_SESSION_PERSISTENCE_OK) {
        budget_free(budget, value);
        return NULL;
    }
    value[encoded_length] = 0;
    return value;
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

static void parse_cache(PersistReader *reader, BrowserSession *staging,
                        const BrowserSessionPersistenceLimits *limits)
{
    char url[TILEFINCH_URL_SERIALIZED_LIMIT];
    char etag[sizeof(staging->cache[0].etag)];
    char modified[sizeof(staging->cache[0].last_modified)];
    char content_type[sizeof(staging->cache[0].content_type)];
    char vary[sizeof(staging->cache[0].vary)];
    char response[TILEFINCH_URL_SERIALIZED_LIMIT];
    char policy[BROWSER_REFERRER_POLICY_LIMIT];
    reader_field(reader, url, sizeof(url), false);
    size_t body_length = 0;
    bool body_skipped = false;
    unsigned char *data = reader_blob(
        reader, staging->budget, limits->maximum_cache_bytes,
        staging->maximum_cache_bytes, &body_length, false,
        &body_skipped);
    uint64_t stamp = reader_u64(reader);
    reader_field(reader, etag, sizeof(etag), true);
    reader_field(reader, modified, sizeof(modified), true);
    reader_field(reader, content_type, sizeof(content_type), true);
    reader_field(reader, vary, sizeof(vary), true);
    uint32_t flags = reader_u32(reader);
    reader_field(reader, response, sizeof(response), true);
    reader_field(reader, policy, sizeof(policy), true);
    if (reader->status != BROWSER_SESSION_PERSISTENCE_OK) {
        budget_free(staging->budget, data);
        return;
    }
    if (body_length > limits->maximum_cache_bytes
        || reader->cache_bytes
               > limits->maximum_cache_bytes - body_length) {
        budget_free(staging->budget, data);
        reader->status = BROWSER_SESSION_PERSISTENCE_LIMIT_EXCEEDED;
        return;
    }
    reader->cache_bytes += body_length;
    char key[TILEFINCH_URL_SERIALIZED_LIMIT];
    if ((flags & ~UINT32_C(31)) != 0
        || ((flags & UINT32_C(16)) != 0
            && (flags & UINT32_C(8)) == 0)
        || stamp > SIZE_MAX
        || !http_field_value_valid(etag)
        || !http_field_value_valid(modified)
        || !http_field_value_valid(content_type)
        || !http_field_value_valid(vary)
        || !tilefinch_url_request_key(url, key, sizeof(key))
        || strcmp(url, key) != 0 || cache_entry_direct(staging, url) != NULL) {
        budget_free(staging->budget, data);
        reader->status = BROWSER_SESSION_PERSISTENCE_CORRUPT;
        return;
    }
    if (body_skipped) {
        /*
         * The disk snapshot may predate a lower live-memory preference.
         * Its bytes and metadata have still participated in bounds checks and
         * the whole-file hash, but do not make one oversized object consume
         * its disk-sized body in memory or defeat the transactional restore.
         */
        return;
    }
    BrowserSharedBody *body = browser_shared_body_take(
        staging->budget, data, body_length);
    if (body == NULL) {
        budget_free(staging->budget, data);
        reader->status = BROWSER_SESSION_PERSISTENCE_OUT_OF_MEMORY;
        return;
    }
    char cache_control[48] = {0};
    if ((flags & UINT32_C(1)) != 0) strcat(cache_control, "no-cache");
    if ((flags & UINT32_C(2)) != 0)
        strcat(cache_control, cache_control[0] == '\0'
                              ? "must-revalidate" : ",must-revalidate");
    if ((flags & UINT32_C(4)) != 0)
        strcat(cache_control, cache_control[0] == '\0'
                              ? "immutable" : ",immutable");
    bool stored = browser_session_cache_put_http_shared(
        staging, url, body, etag, modified, content_type,
        cache_control, vary, 0);
    browser_shared_body_release(body);
    if (!stored) {
        reader->status = BROWSER_SESSION_PERSISTENCE_CORRUPT;
        return;
    }
    BrowserCacheEntry *entry = cache_entry_direct(staging, url);
    if (entry == NULL) {
        reader->status = BROWSER_SESSION_PERSISTENCE_CORRUPT;
        return;
    }
    (void) stamp;
    /*
     * HTTP freshness timestamps use a per-process monotonic clock and cannot
     * be compared across a reboot. A restored body is always stale so ETag or
     * Last-Modified must revalidate it before reuse as a fresh response.
     */
    entry->stored_at_ns = 0;
    entry->fresh_until_ns = 0;
    entry->no_cache = true;
    entry->must_revalidate = (flags & UINT32_C(2)) != 0;
    entry->immutable = (flags & UINT32_C(4)) != 0;
    bool provenance_ok = true;
    if ((flags & UINT32_C(8)) != 0) {
        const char *final_url = response[0] == '\0' ? url : response;
        provenance_ok = (flags & UINT32_C(16)) != 0
            ? browser_session_cache_set_response_provenance(
                  staging, url, final_url, policy)
            : browser_session_cache_set_response_url(
                  staging, url, final_url);
    } else if (response[0] != '\0' || policy[0] != '\0') {
        provenance_ok = false;
    }
    if (!provenance_ok)
        reader->status = BROWSER_SESSION_PERSISTENCE_CORRUPT;
}

static void parse_storage(PersistReader *reader, BrowserSession *staging)
{
    char origin[BROWSER_ORIGIN_LIMIT], normalized[BROWSER_ORIGIN_LIMIT];
    char key[BROWSER_KEY_LIMIT];
    reader_field(reader, origin, sizeof(origin), false);
    reader_field(reader, key, sizeof(key), true);
    size_t value_length = 0;
    unsigned char *value = reader_blob(
        reader, staging->budget, staging->maximum_storage_bytes,
        staging->maximum_storage_bytes, &value_length, true, NULL);
    if (reader->status != BROWSER_SESSION_PERSISTENCE_OK) {
        budget_free(staging->budget, value);
        return;
    }
    const char *old_value = NULL;
    if (!tilefinch_url_origin(origin, normalized, sizeof(normalized))
        || strcmp(origin, normalized) != 0
        || browser_session_storage_get(
               staging, origin, true, key, &old_value, NULL)) {
        budget_free(staging->budget, value);
        reader->status = BROWSER_SESSION_PERSISTENCE_CORRUPT;
        return;
    }
    if (!browser_session_storage_set(
            staging, origin, true, key, (const char *) value,
            value_length)) {
        budget_free(staging->budget, value);
        reader->status = BROWSER_SESSION_PERSISTENCE_OUT_OF_MEMORY;
        return;
    }
    budget_free(staging->budget, value);
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

static BrowserSessionPersistenceStatus load_one(
    BrowserSession *session, const char *path,
    BrowserSessionPersistenceMask requested_mask,
    const BrowserSessionPersistenceLimits *limits)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return errno == ENOENT
        ? BROWSER_SESSION_PERSISTENCE_NOT_FOUND
        : BROWSER_SESSION_PERSISTENCE_IO_ERROR;
    unsigned char header[PERSIST_HEADER_BYTES];
    bool header_ok = fread(header, 1, sizeof(header), file) == sizeof(header);
    if (!header_ok) {
        BrowserSessionPersistenceStatus status = ferror(file)
            ? BROWSER_SESSION_PERSISTENCE_IO_ERROR
            : BROWSER_SESSION_PERSISTENCE_CORRUPT;
        fclose(file);
        return status;
    }
    uint32_t stored_mask = get_u32(header + 16);
    uint32_t payload_bytes = get_u32(header + 20);
    uint32_t expected_hash = get_u32(header + 24);
    uint32_t cache_count = get_u32(header + 28);
    uint32_t storage_count = get_u32(header + 32);
    if (memcmp(header, persist_magic, sizeof(persist_magic)) != 0
        || get_u32(header + 8) != PERSIST_VERSION
        || get_u32(header + 12) != PERSIST_HEADER_BYTES
        || stored_mask == 0
        || (stored_mask & ~BROWSER_SESSION_PERSIST_ALL) != 0
        || payload_bytes > limits->maximum_file_bytes - PERSIST_HEADER_BYTES
        || cache_count > limits->maximum_cache_entries
        || storage_count > limits->maximum_local_storage_entries) {
        fclose(file);
        return BROWSER_SESSION_PERSISTENCE_CORRUPT;
    }
    BrowserSession *staging = budget_calloc_category(
        session->budget, BUDGET_CATEGORY_SESSION, 1, sizeof(*staging));
    if (staging == NULL) {
        fclose(file);
        return BROWSER_SESSION_PERSISTENCE_OUT_OF_MEMORY;
    }
    size_t staging_cache_limit = session->maximum_cache_bytes;
    if (staging_cache_limit > limits->maximum_cache_bytes)
        staging_cache_limit = limits->maximum_cache_bytes;
    if (!browser_session_init(staging, session->budget,
                              staging_cache_limit)) {
        fclose(file);
        budget_free(session->budget, staging);
        return BROWSER_SESSION_PERSISTENCE_OUT_OF_MEMORY;
    }
    PersistReader reader = {
        .file = file, .remaining = payload_bytes,
        .hash = UINT32_C(2166136261),
        .status = BROWSER_SESSION_PERSISTENCE_OK
    };
    size_t parsed_cache = 0, parsed_storage = 0;
    while (reader.status == BROWSER_SESSION_PERSISTENCE_OK
           && reader.remaining != 0) {
        size_t before = reader.remaining;
        uint32_t type = reader_u32(&reader);
        uint32_t record_bytes = reader_u32(&reader);
        if (reader.status != BROWSER_SESSION_PERSISTENCE_OK) break;
        if (record_bytes > reader.remaining) {
            reader.status = BROWSER_SESSION_PERSISTENCE_CORRUPT;
            break;
        }
        if (type == RECORD_CACHE
            && (stored_mask & BROWSER_SESSION_PERSIST_CACHE) != 0) {
            parsed_cache++;
            parse_cache(&reader, staging, limits);
        } else if (type == RECORD_LOCAL_STORAGE
                   && (stored_mask
                       & BROWSER_SESSION_PERSIST_LOCAL_STORAGE) != 0) {
            parsed_storage++;
            parse_storage(&reader, staging);
        } else {
            reader.status = BROWSER_SESSION_PERSISTENCE_CORRUPT;
        }
        size_t consumed = before - reader.remaining;
        if (reader.status == BROWSER_SESSION_PERSISTENCE_OK
            && (consumed < 8u || consumed - 8u != record_bytes)) {
            reader.status = BROWSER_SESSION_PERSISTENCE_CORRUPT;
        }
    }
    int trailing = reader.status == BROWSER_SESSION_PERSISTENCE_OK
        ? fgetc(file) : EOF;
    if (reader.status == BROWSER_SESSION_PERSISTENCE_OK
        && (reader.remaining != 0 || trailing != EOF || ferror(file)
            || reader.hash != expected_hash
            || parsed_cache != cache_count
            || parsed_storage != storage_count)) {
        reader.status = BROWSER_SESSION_PERSISTENCE_CORRUPT;
    }
    fclose(file);
    BrowserSessionPersistenceMask commit_mask =
        requested_mask & stored_mask;
    if (reader.status == BROWSER_SESSION_PERSISTENCE_OK
        && (commit_mask & BROWSER_SESSION_PERSIST_LOCAL_STORAGE) != 0
        && !local_commit_fits(session, staging)) {
        reader.status = BROWSER_SESSION_PERSISTENCE_LIMIT_EXCEEDED;
    }
    if (reader.status == BROWSER_SESSION_PERSISTENCE_OK)
        commit_staging(session, staging, commit_mask);
    browser_session_destroy(staging);
    budget_free(session->budget, staging);
    return reader.status;
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
    BrowserSessionPersistenceStatus status =
        load_one(session, path, mask, &limits);
    if (status == BROWSER_SESSION_PERSISTENCE_NOT_FOUND
        || status == BROWSER_SESSION_PERSISTENCE_CORRUPT) {
        BrowserSessionPersistenceStatus backup_status =
            load_one(session, backup, mask, &limits);
        if (backup_status == BROWSER_SESSION_PERSISTENCE_OK) {
            /* Do not let a known-torn primary replace the recovered generation
               during the next save's rotation. */
            (void) remove(path);
            return backup_status;
        }
        if (status == BROWSER_SESSION_PERSISTENCE_NOT_FOUND
            || (backup_status != BROWSER_SESSION_PERSISTENCE_NOT_FOUND
                && backup_status != BROWSER_SESSION_PERSISTENCE_CORRUPT)) {
            return backup_status;
        }
    }
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
