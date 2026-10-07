/*
 * Lazy webpack bundle records waiting for their digest (see session.h,
 * "Lazy webpack bundle records").
 *
 * A record is keyed by the SHA-256 of its bundle's exact bytes, and a first
 * visit never computes one (lookups digest only a candidate's bytes). The
 * load whose planner made the plan queues its record here with a reference
 * to the response body instead; idle turns digest the body in slices and then
 * store the record in the LAZY_BUNDLE table. The queue belongs to the
 * session, so a navigation does not lose it, and it is the first thing the
 * reclaims drop: its references may be all that keeps a body alive.
 */
#include "tilefinch/session.h"

#include "tilefinch/sha256.h"

#include <string.h>

typedef struct {
    /* NULL once digested: then source_digest holds the key's digest. */
    BrowserSharedBody *body;
    /* "name\0url\0partition\0" then the record payload, one allocation. */
    unsigned char *storage;
    size_t storage_bytes;
    const char *url;
    const char *partition;
    const unsigned char *record;
    size_t record_length;
    size_t source_length;
    size_t hashed;
    bool digested;
    uint8_t source_digest[TILEFINCH_SHA256_DIGEST_BYTES];
    uint32_t compile_flags;
    uint32_t epoch;
    uint32_t generation;
    TilefinchSha256 digest;
} BrowserLazyBundlePendingRecord;

struct BrowserLazyBundlePending {
    BrowserLazyBundlePendingRecord records[BROWSER_LAZY_BUNDLE_PENDING_LIMIT];
    size_t count;
    size_t body_bytes;
};

/* Takes entry `at` out of the queue without releasing it, keeping the rest
   in order (oldest first), and frees the queue once it is empty. */
static BrowserLazyBundlePendingRecord pending_detach(BrowserSession *session,
                                                     size_t at)
{
    struct BrowserLazyBundlePending *queue = session->lazy_bundle_pending;
    BrowserLazyBundlePendingRecord taken = queue->records[at];
    size_t length = taken.body == NULL ? 0 : taken.body->length;
    queue->body_bytes -= length <= queue->body_bytes
        ? length : queue->body_bytes;
    memmove(&queue->records[at], &queue->records[at + 1u],
            (queue->count - at - 1u) * sizeof(queue->records[0]));
    queue->count--;
    memset(&queue->records[queue->count], 0, sizeof(queue->records[0]));
    if (queue->count == 0) {
        budget_free(session->budget, queue);
        session->lazy_bundle_pending = NULL;
    }
    return taken;
}

static void pending_free(BrowserSession *session,
                         BrowserLazyBundlePendingRecord *record)
{
    browser_shared_body_release(record->body);
    budget_free(session->budget, record->storage);
    memset(record, 0, sizeof(*record));
}

static void pending_remove(BrowserSession *session, size_t at)
{
    BrowserLazyBundlePendingRecord taken = pending_detach(session, at);
    pending_free(session, &taken);
}

static bool pending_store(BrowserSession *session,
                          const BrowserScriptBytecodeKey *key,
                          uint32_t generation, const unsigned char *record,
                          size_t record_length)
{
    BrowserScriptBytecodeKey stored = *key;
    stored.source = NULL;
    return stored.digest_ready
        && browser_session_script_bytecode_put(
               session, BROWSER_SCRIPT_BYTECODE_LAZY_BUNDLE, &stored,
               generation, record, record_length);
}

bool browser_session_lazy_bundle_record_queue(
    BrowserSession *session, const BrowserScriptBytecodeKey *key,
    BrowserSharedBody *body, uint32_t generation,
    const unsigned char *record, size_t record_length)
{
    if (session == NULL || session->budget == NULL || key == NULL
        || key->module_name == NULL || key->response_url == NULL
        || key->partition_key == NULL || record == NULL
        || record_length == 0
        || session->maximum_lazy_bundle_record_bytes == 0
        || session->captive_portal_stash != NULL) return false;
    if (key->digest_ready)
        return pending_store(session, key, generation, record,
                             record_length);
    if (body == NULL || body->data == NULL
        || body->length != key->source_length || body->length == 0)
        return false;
    struct BrowserLazyBundlePending *queue = session->lazy_bundle_pending;
    size_t queued_bytes = queue == NULL ? 0 : queue->body_bytes;
    if ((queue != NULL && queue->count >= BROWSER_LAZY_BUNDLE_PENDING_LIMIT)
        || queued_bytes > BROWSER_LAZY_BUNDLE_PENDING_BYTES
        || body->length > BROWSER_LAZY_BUNDLE_PENDING_BYTES - queued_bytes)
        return false;
    /* One queued record per (name, URL, site): a newer load replaces it. */
    for (size_t i = 0; queue != NULL && i < queue->count; i++) {
        BrowserLazyBundlePendingRecord *entry = &queue->records[i];
        if (strcmp((const char *) entry->storage, key->module_name) == 0
            && strcmp(entry->url, key->response_url) == 0
            && strcmp(entry->partition, key->partition_key) == 0) {
            pending_remove(session, i);
            queue = session->lazy_bundle_pending;
            break;
        }
    }
    size_t name = strlen(key->module_name) + 1u;
    size_t url = strlen(key->response_url) + 1u;
    size_t partition = strlen(key->partition_key) + 1u;
    if (record_length > SIZE_MAX - name - url - partition) return false;
    size_t storage_bytes = name + url + partition + record_length;
    unsigned char *storage = budget_malloc_category(
        session->budget, BUDGET_CATEGORY_SESSION, storage_bytes);
    if (storage == NULL) return false;
    if (session->lazy_bundle_pending == NULL) {
        session->lazy_bundle_pending = budget_calloc_category(
            session->budget, BUDGET_CATEGORY_SESSION, 1,
            sizeof(*session->lazy_bundle_pending));
        if (session->lazy_bundle_pending == NULL) {
            budget_free(session->budget, storage);
            return false;
        }
    }
    queue = session->lazy_bundle_pending;
    memcpy(storage, key->module_name, name);
    memcpy(storage + name, key->response_url, url);
    memcpy(storage + name + url, key->partition_key, partition);
    memcpy(storage + name + url + partition, record, record_length);
    BrowserLazyBundlePendingRecord *entry = &queue->records[queue->count++];
    *entry = (BrowserLazyBundlePendingRecord) {
        .body = browser_shared_body_retain(body),
        .storage = storage,
        .storage_bytes = storage_bytes,
        .url = (const char *) storage + name,
        .partition = (const char *) storage + name + url,
        .record = storage + name + url + partition,
        .record_length = record_length,
        .source_length = body->length,
        .compile_flags = key->compile_flags,
        .epoch = session->script_bytecode_epoch,
        .generation = generation
    };
    tilefinch_sha256_init(&entry->digest);
    queue->body_bytes += body->length;
    return true;
}

/* Digests up to `limit` more bytes of a queued record's body; once the
   whole body is digested the record keeps only the digest and lets the
   body go. */
static void pending_digest(BrowserSession *session,
                           BrowserLazyBundlePendingRecord *entry,
                           size_t limit)
{
    if (entry->digested || entry->body == NULL) return;
    size_t slice = entry->source_length - entry->hashed;
    if (slice > limit) slice = limit;
    if (slice != 0
        && !tilefinch_sha256_update(&entry->digest,
                                    entry->body->data + entry->hashed,
                                    slice)) return;
    entry->hashed += slice;
    if (entry->hashed < entry->source_length
        || !tilefinch_sha256_final(&entry->digest, entry->source_digest))
        return;
    struct BrowserLazyBundlePending *queue = session->lazy_bundle_pending;
    queue->body_bytes -= entry->source_length <= queue->body_bytes
        ? entry->source_length : queue->body_bytes;
    browser_shared_body_release(entry->body);
    entry->body = NULL;
    entry->digested = true;
}

bool browser_session_lazy_bundle_record_maintenance(BrowserSession *session)
{
    struct BrowserLazyBundlePending *queue =
        session == NULL ? NULL : session->lazy_bundle_pending;
    if (queue == NULL || queue->count == 0) return false;
    BrowserLazyBundlePendingRecord *entry = &queue->records[0];
    if (entry->epoch != session->script_bytecode_epoch
        || session->captive_portal_stash != NULL) {
        pending_remove(session, 0);
        return true;
    }
    if (!entry->digested) {
        size_t hashed = entry->hashed;
        pending_digest(session, entry, BROWSER_LAZY_BUNDLE_HASH_SLICE);
        if (!entry->digested && entry->hashed == hashed) {
            pending_remove(session, 0);
            return true;
        }
        if (!entry->digested) return true;
    }
    /* Out of the queue before storing: the store allocates, and the Budget
       reclaim hook it may run works on the queue. */
    BrowserLazyBundlePendingRecord done = pending_detach(session, 0);
    BrowserScriptBytecodeKey key = {
        .module_name = (const char *) done.storage,
        .response_url = done.url,
        .partition_key = done.partition,
        .source_length = done.source_length,
        .compile_flags = done.compile_flags,
        .ordinal = 0,
        .digest_ready = true
    };
    memcpy(key.source_digest, done.source_digest, sizeof(key.source_digest));
    (void) pending_store(session, &key, done.generation, done.record,
                        done.record_length);
    pending_free(session, &done);
    return true;
}

size_t browser_session_lazy_bundle_pending_count(
    const BrowserSession *session)
{
    return session == NULL || session->lazy_bundle_pending == NULL
        ? 0 : session->lazy_bundle_pending->count;
}

void browser_session_lazy_bundle_pending_clear(BrowserSession *session,
                                               const char *partition)
{
    if (session == NULL) return;
    for (size_t i = browser_session_lazy_bundle_pending_count(session);
         i-- > 0;) {
        if (partition == NULL
            || strcmp(session->lazy_bundle_pending->records[i].partition,
                      partition) == 0)
            pending_remove(session, i);
    }
}

/* A queued record keeps a body alive only when it holds the body's last
   reference. Rather than lose the record, the reclaim finishes its digest
   there and then (CPU only: nothing is allocated) and releases the body;
   the record, a few KiB, is stored at the next idle turn. Records whose
   body something else still holds release nothing and are left alone. */
size_t browser_session_lazy_bundle_pending_reclaim(BrowserSession *session,
                                                   size_t target_bytes)
{
    size_t released = 0;
    for (size_t i = 0; released < target_bytes
             && i < browser_session_lazy_bundle_pending_count(session); i++) {
        BrowserLazyBundlePendingRecord *entry =
            &session->lazy_bundle_pending->records[i];
        if (entry->body == NULL || entry->body->references != 1u) continue;
        size_t length = entry->source_length;
        pending_digest(session, entry, SIZE_MAX);
        if (!entry->digested) {
            pending_remove(session, i);
            i--;
        }
        released = length > SIZE_MAX - released ? SIZE_MAX
                                                : released + length;
    }
    return released;
}
