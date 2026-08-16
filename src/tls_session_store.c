#include "tilefinch/tls_session_store.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * On-stick TLS session store. The file layout mirrors session_persistence.c:
 * an 8-byte magic, a store-format version, the header size, a crypto pin, the
 * entry count, the payload length and an FNV-1a hash over the payload, then a
 * flat sequence of entries. Any deviation on load is a plain miss.
 */

#define TLS_STORE_HEADER_BYTES 32u
#define TLS_STORE_VERSION UINT32_C(1)
#define TLS_STORE_PATH_BYTES 1200u

static const unsigned char tls_store_magic[8] = {
    'T', 'F', 'T', 'L', 'S', 'S', 1, 0
};

typedef struct {
    char *key;
    size_t key_length;
    unsigned char *blob;
    size_t blob_length;
    int64_t valid_until;
    int32_t ietf_tls_id;
    uint64_t last_use;
} TlsStoreEntry;

struct TlsSessionStore {
    TlsStoreEntry entries[TLS_SESSION_STORE_MAX_ENTRIES];
    size_t count;
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

TlsSessionStore *tls_session_store_create(void)
{
    return calloc(1, sizeof(TlsSessionStore));
}

static void entry_clear(TlsStoreEntry *entry)
{
    free(entry->key);
    free(entry->blob);
    memset(entry, 0, sizeof(*entry));
}

void tls_session_store_free(TlsSessionStore *store)
{
    if (store == NULL) return;
    for (size_t i = 0; i < store->count; i++) entry_clear(&store->entries[i]);
    free(store);
}

size_t tls_session_store_count(const TlsSessionStore *store)
{
    return store == NULL ? 0 : store->count;
}

/* A session is live when the clock is unknown (now <= 0, keep and let the
 * transport decide) or when it is unexpired and not implausibly far future. */
static int entry_live(int64_t valid_until, int64_t now)
{
    if (now <= 0) return 1;
    if (valid_until <= now) return 0;
    if (valid_until - now > TLS_SESSION_STORE_MAX_FUTURE_SECONDS) return 0;
    return 1;
}

static void store_remove_at(TlsSessionStore *store, size_t index)
{
    entry_clear(&store->entries[index]);
    for (size_t i = index + 1; i < store->count; i++) {
        store->entries[i - 1] = store->entries[i];
    }
    store->count--;
    memset(&store->entries[store->count], 0, sizeof(store->entries[0]));
}

static size_t distinct_hosts(const TlsSessionStore *store, const char *key,
                             size_t key_length, int *key_present)
{
    size_t hosts = 0;
    *key_present = 0;
    for (size_t i = 0; i < store->count; i++) {
        int seen = 0;
        for (size_t j = 0; j < i; j++) {
            if (store->entries[j].key_length == store->entries[i].key_length
                && memcmp(store->entries[j].key, store->entries[i].key,
                          store->entries[i].key_length) == 0) {
                seen = 1;
                break;
            }
        }
        if (!seen) hosts++;
        if (store->entries[i].key_length == key_length
            && memcmp(store->entries[i].key, key, key_length) == 0) {
            *key_present = 1;
        }
    }
    return hosts;
}

/* Least-recently-used entry index within a key group, or SIZE_MAX. */
static size_t lru_in_group(const TlsSessionStore *store, const char *key,
                           size_t key_length)
{
    size_t best = (size_t) -1;
    for (size_t i = 0; i < store->count; i++) {
        if (store->entries[i].key_length != key_length
            || memcmp(store->entries[i].key, key, key_length) != 0) continue;
        if (best == (size_t) -1
            || store->entries[i].last_use < store->entries[best].last_use) {
            best = i;
        }
    }
    return best;
}

/* Least-recently-used entry across the whole store, or SIZE_MAX. */
static size_t lru_global(const TlsSessionStore *store)
{
    size_t best = (size_t) -1;
    for (size_t i = 0; i < store->count; i++) {
        if (best == (size_t) -1
            || store->entries[i].last_use < store->entries[best].last_use) {
            best = i;
        }
    }
    return best;
}

/* Evict the least-recently-used host group entirely to free a key slot. */
static void evict_lru_host(TlsSessionStore *store)
{
    size_t victim = lru_global(store);
    if (victim == (size_t) -1) return;
    char key[TLS_SESSION_STORE_MAX_KEY_BYTES];
    size_t key_length = store->entries[victim].key_length;
    memcpy(key, store->entries[victim].key, key_length);
    for (size_t i = 0; i < store->count;) {
        if (store->entries[i].key_length == key_length
            && memcmp(store->entries[i].key, key, key_length) == 0) {
            store_remove_at(store, i);
        } else {
            i++;
        }
    }
}

static size_t entry_serialized_bytes(size_t key_length, size_t blob_length)
{
    /* key_len(4) + key + blob_len(4) + blob + valid_until(8) + tls_id(4)
     * + last_use(8) */
    return 4u + key_length + 4u + blob_length + 8u + 4u + 8u;
}

static size_t store_serialized_bytes(const TlsSessionStore *store)
{
    size_t total = TLS_STORE_HEADER_BYTES;
    for (size_t i = 0; i < store->count; i++) {
        total += entry_serialized_bytes(store->entries[i].key_length,
                                        store->entries[i].blob_length);
    }
    return total;
}

int tls_session_store_add(TlsSessionStore *store, const char *key,
                          const unsigned char *blob, size_t blob_length,
                          int64_t valid_until, int32_t ietf_tls_id,
                          int64_t now, uint64_t last_use)
{
    if (store == NULL || key == NULL || blob == NULL) return 0;
    size_t key_length = strlen(key);
    if (key_length == 0 || key_length > TLS_SESSION_STORE_MAX_KEY_BYTES
        || blob_length == 0 || blob_length > TLS_SESSION_STORE_MAX_ENTRY_BYTES) {
        return 0;
    }
    if (!entry_live(valid_until, now)) return 0;
    if (entry_serialized_bytes(key_length, blob_length)
        > TLS_SESSION_STORE_MAX_FILE_BYTES - TLS_STORE_HEADER_BYTES) {
        return 0;
    }

    int key_present = 0;
    size_t hosts = distinct_hosts(store, key, key_length, &key_present);

    if (key_present) {
        size_t group = 0;
        for (size_t i = 0; i < store->count; i++) {
            if (store->entries[i].key_length == key_length
                && memcmp(store->entries[i].key, key, key_length) == 0) {
                group++;
            }
        }
        if (group >= TLS_SESSION_STORE_MAX_PER_HOST) {
            size_t victim = lru_in_group(store, key, key_length);
            if (victim != (size_t) -1) store_remove_at(store, victim);
        }
    } else if (hosts >= TLS_SESSION_STORE_MAX_HOSTS) {
        evict_lru_host(store);
    }

    while (store->count >= TLS_SESSION_STORE_MAX_ENTRIES) {
        size_t victim = lru_global(store);
        if (victim == (size_t) -1) return 0;
        store_remove_at(store, victim);
    }
    while (store->count > 0
           && store_serialized_bytes(store)
                  + entry_serialized_bytes(key_length, blob_length)
              > TLS_SESSION_STORE_MAX_FILE_BYTES) {
        size_t victim = lru_global(store);
        if (victim == (size_t) -1) break;
        store_remove_at(store, victim);
    }

    TlsStoreEntry *slot = &store->entries[store->count];
    slot->key = malloc(key_length + 1);
    slot->blob = malloc(blob_length);
    if (slot->key == NULL || slot->blob == NULL) {
        free(slot->key);
        free(slot->blob);
        memset(slot, 0, sizeof(*slot));
        return 0;
    }
    memcpy(slot->key, key, key_length + 1);
    slot->key_length = key_length;
    memcpy(slot->blob, blob, blob_length);
    slot->blob_length = blob_length;
    slot->valid_until = valid_until;
    slot->ietf_tls_id = ietf_tls_id;
    slot->last_use = last_use;
    store->count++;
    return 1;
}

size_t tls_session_store_for_each(const TlsSessionStore *store, int64_t now,
                                  TlsSessionImportFn callback, void *context)
{
    if (store == NULL || callback == NULL) return 0;
    /* Visit oldest last_use first so a smaller live cache keeps the hottest
     * tail if the transport later evicts under its own share cap. */
    uint8_t order[TLS_SESSION_STORE_MAX_ENTRIES];
    size_t count = 0;
    for (size_t i = 0; i < store->count; i++) {
        if (!entry_live(store->entries[i].valid_until, now)) continue;
        size_t at = count;
        while (at != 0
               && store->entries[order[at - 1]].last_use
                      > store->entries[i].last_use) {
            order[at] = order[at - 1];
            at--;
        }
        order[at] = (uint8_t) i;
        count++;
    }
    for (size_t i = 0; i < count; i++) {
        const TlsStoreEntry *entry = &store->entries[order[i]];
        callback(context, entry->key, entry->blob, entry->blob_length,
                 entry->ietf_tls_id);
    }
    return count;
}

static int make_path(const char *path, const char *suffix,
                     char output[TLS_STORE_PATH_BYTES])
{
    if (path == NULL || path[0] == '\0') return 0;
    int written = snprintf(output, TLS_STORE_PATH_BYTES, "%s%s", path, suffix);
    return written > 0 && (size_t) written < TLS_STORE_PATH_BYTES;
}

static int install_temporary(const char *temporary, const char *path,
                             const char *backup)
{
    /* Mirror session_persistence.c: rotate the primary to backup, then rename
     * the closed temporary in. FAT may refuse a replacing rename, so retry
     * after removing only the older backup while the primary is still whole. */
    int had_previous = rename(path, backup) == 0;
    if (!had_previous) {
        if (errno == ENOENT) return rename(temporary, path) == 0;
        (void) remove(backup);
        had_previous = rename(path, backup) == 0;
        if (!had_previous && errno != ENOENT) return 0;
    }
    if (rename(temporary, path) == 0) return 1;
    if (had_previous) (void) rename(backup, path);
    return 0;
}

static void emit_bytes(FILE *file, uint32_t *hash, const void *data,
                       size_t length, int *ok)
{
    if (!*ok) return;
    if (file != NULL && fwrite(data, 1, length, file) != length) {
        *ok = 0;
        return;
    }
    *hash = hash_update(*hash, data, length);
}

static void emit_u32(FILE *file, uint32_t *hash, uint32_t value, int *ok)
{
    unsigned char encoded[4];
    put_u32(encoded, value);
    emit_bytes(file, hash, encoded, sizeof(encoded), ok);
}

static void emit_u64(FILE *file, uint32_t *hash, uint64_t value, int *ok)
{
    unsigned char encoded[8];
    put_u64(encoded, value);
    emit_bytes(file, hash, encoded, sizeof(encoded), ok);
}

static int emit_payload(FILE *file, const TlsSessionStore *store,
                        int64_t now, uint32_t *hash, size_t *bytes,
                        size_t *entry_count)
{
    int ok = 1;
    *hash = UINT32_C(2166136261);
    *bytes = 0;
    *entry_count = 0;
    for (size_t i = 0; i < store->count && ok; i++) {
        const TlsStoreEntry *entry = &store->entries[i];
        if (!entry_live(entry->valid_until, now)) continue;
        emit_u32(file, hash, (uint32_t) entry->key_length, &ok);
        emit_bytes(file, hash, entry->key, entry->key_length, &ok);
        emit_u32(file, hash, (uint32_t) entry->blob_length, &ok);
        emit_bytes(file, hash, entry->blob, entry->blob_length, &ok);
        emit_u64(file, hash, (uint64_t) entry->valid_until, &ok);
        emit_u32(file, hash, (uint32_t) entry->ietf_tls_id, &ok);
        emit_u64(file, hash, entry->last_use, &ok);
        *bytes += entry_serialized_bytes(entry->key_length, entry->blob_length);
        (*entry_count)++;
    }
    return ok;
}

int tls_session_store_save(const TlsSessionStore *store, const char *path,
                           uint32_t crypto_pin, int64_t now)
{
    char temporary[TLS_STORE_PATH_BYTES], backup[TLS_STORE_PATH_BYTES];
    if (store == NULL || !make_path(path, ".tmp", temporary)
        || !make_path(path, ".bak", backup)) {
        return -1;
    }
    uint32_t hash = 0;
    size_t payload_bytes = 0, entry_count = 0;
    if (!emit_payload(NULL, store, now, &hash, &payload_bytes, &entry_count)
        || payload_bytes > UINT32_MAX
        || TLS_STORE_HEADER_BYTES + payload_bytes
               > TLS_SESSION_STORE_MAX_FILE_BYTES) {
        return -1;
    }
    unsigned char header[TLS_STORE_HEADER_BYTES];
    memset(header, 0, sizeof(header));
    memcpy(header, tls_store_magic, sizeof(tls_store_magic));
    put_u32(header + 8, TLS_STORE_VERSION);
    put_u32(header + 12, TLS_STORE_HEADER_BYTES);
    put_u32(header + 16, crypto_pin);
    put_u32(header + 20, (uint32_t) entry_count);
    put_u32(header + 24, (uint32_t) payload_bytes);
    put_u32(header + 28, hash);

    FILE *file = fopen(temporary, "wb");
    if (file == NULL) return -1;
    int ok = fwrite(header, 1, sizeof(header), file) == sizeof(header);
    if (ok) {
        uint32_t verify_hash = 0;
        size_t verify_bytes = 0, verify_count = 0;
        ok = emit_payload(file, store, now, &verify_hash, &verify_bytes,
                          &verify_count)
             && verify_hash == hash && verify_bytes == payload_bytes
             && verify_count == entry_count;
    }
    ok = fclose(file) == 0 && ok;
    if (!ok) {
        (void) remove(temporary);
        return -1;
    }
    if (!install_temporary(temporary, path, backup)) {
        (void) remove(temporary);
        return -1;
    }
    return 0;
}

static TlsSessionStore *load_one(const char *path, uint32_t crypto_pin,
                                 int64_t now)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return NULL;
    unsigned char header[TLS_STORE_HEADER_BYTES];
    if (fread(header, 1, sizeof(header), file) != sizeof(header)
        || memcmp(header, tls_store_magic, sizeof(tls_store_magic)) != 0
        || get_u32(header + 8) != TLS_STORE_VERSION
        || get_u32(header + 12) != TLS_STORE_HEADER_BYTES
        || get_u32(header + 16) != crypto_pin) {
        fclose(file);
        return NULL;
    }
    uint32_t entry_count = get_u32(header + 20);
    uint32_t payload_bytes = get_u32(header + 24);
    uint32_t expected_hash = get_u32(header + 28);
    if (entry_count > TLS_SESSION_STORE_MAX_ENTRIES
        || payload_bytes
               > TLS_SESSION_STORE_MAX_FILE_BYTES - TLS_STORE_HEADER_BYTES) {
        fclose(file);
        return NULL;
    }
    unsigned char *payload = payload_bytes == 0 ? NULL : malloc(payload_bytes);
    if (payload_bytes != 0 && payload == NULL) {
        fclose(file);
        return NULL;
    }
    if (payload_bytes != 0
        && fread(payload, 1, payload_bytes, file) != payload_bytes) {
        free(payload);
        fclose(file);
        return NULL;
    }
    int trailing = fgetc(file);
    int stream_ok = !ferror(file);
    fclose(file);
    if (!stream_ok || trailing != EOF
        || hash_update(UINT32_C(2166136261), payload, payload_bytes)
               != expected_hash) {
        free(payload);
        return NULL;
    }

    TlsSessionStore *store = tls_session_store_create();
    if (store == NULL) {
        free(payload);
        return NULL;
    }
    size_t offset = 0;
    size_t parsed = 0;
    int ok = 1;
    while (ok && offset < payload_bytes) {
        if (payload_bytes - offset < 4u) { ok = 0; break; }
        uint32_t key_length = get_u32(payload + offset);
        offset += 4u;
        if (key_length == 0 || key_length > TLS_SESSION_STORE_MAX_KEY_BYTES
            || payload_bytes - offset < key_length) { ok = 0; break; }
        const char *key = (const char *) payload + offset;
        if (memchr(key, '\0', key_length) != NULL) { ok = 0; break; }
        offset += key_length;
        if (payload_bytes - offset < 4u) { ok = 0; break; }
        uint32_t blob_length = get_u32(payload + offset);
        offset += 4u;
        if (blob_length == 0
            || blob_length > TLS_SESSION_STORE_MAX_ENTRY_BYTES
            || payload_bytes - offset < blob_length) { ok = 0; break; }
        const unsigned char *blob = payload + offset;
        offset += blob_length;
        if (payload_bytes - offset < 8u + 4u + 8u) { ok = 0; break; }
        int64_t valid_until = (int64_t) get_u64(payload + offset);
        offset += 8u;
        int32_t ietf_tls_id = (int32_t) get_u32(payload + offset);
        offset += 4u;
        uint64_t last_use = get_u64(payload + offset);
        offset += 8u;
        parsed++;
        char key_copy[TLS_SESSION_STORE_MAX_KEY_BYTES + 1];
        memcpy(key_copy, key, key_length);
        key_copy[key_length] = '\0';
        (void) tls_session_store_add(store, key_copy, blob, blob_length,
                                     valid_until, ietf_tls_id, now, last_use);
    }
    free(payload);
    if (!ok || offset != payload_bytes || parsed != entry_count) {
        tls_session_store_free(store);
        return NULL;
    }
    return store;
}

TlsSessionStore *tls_session_store_load(const char *path, uint32_t crypto_pin,
                                        int64_t now)
{
    char backup[TLS_STORE_PATH_BYTES];
    if (!make_path(path, ".bak", backup)) return NULL;
    TlsSessionStore *store = load_one(path, crypto_pin, now);
    if (store != NULL) return store;
    store = load_one(backup, crypto_pin, now);
    if (store != NULL) {
        /* Do not let a known-torn primary replace the recovered generation on
         * the next save's rotation. */
        (void) remove(path);
    }
    return store;
}

int tls_session_store_remove(const char *path)
{
    char temporary[TLS_STORE_PATH_BYTES], backup[TLS_STORE_PATH_BYTES];
    if (!make_path(path, ".tmp", temporary) || !make_path(path, ".bak", backup)) {
        return -1;
    }
    const char *paths[] = {temporary, backup, path};
    int removed = 0;
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        if (remove(paths[i]) != 0 && errno != ENOENT) removed = -1;
    }
    return removed;
}
