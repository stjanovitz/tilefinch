#ifndef TILEFINCH_TLS_SESSION_STORE_H
#define TILEFINCH_TLS_SESSION_STORE_H

#include <stddef.h>
#include <stdint.h>

/*
 * Cross-boot TLS session resumption store
 * (docs/engineering/PSP_TRANSPORT.md).
 *
 * A bounded, versioned, checksummed, backup-rotated on-stick record of the
 * serialized TLS sessions libcurl exports through curl_easy_ssls_export().
 * The blobs are resumption secrets (bearer-ish ticket material): they live
 * only on the user's stick, are cleared by CLEAR HTTP CACHES, and are noted
 * in the README privacy section.
 *
 * This module is pure data plus file I/O -- it never links libcurl or Mbed
 * TLS so it can be exercised host-side. The fetch layer owns the glue that
 * feeds curl's export callback into tls_session_store_add() and replays
 * tls_session_store_for_each() back through curl_easy_ssls_import().
 *
 * The stored blob is only meaningful to the exact Mbed TLS build that packed
 * it, so a caller-supplied crypto pin (MBEDTLS_VERSION_NUMBER on the owned
 * transport) is written into the header and re-checked on load; a mismatch is
 * a plain miss. Timestamps are absolute epoch seconds captured from the
 * device clock at export; a `now` of 0 (RTC unavailable) disables expiry and
 * far-future pruning so a wrong clock degrades transparently to a full
 * handshake rather than importing bad state.
 */

/* Distinct peer keys (hosts + TLS config); under curl's 25-peer share cap. */
#define TLS_SESSION_STORE_MAX_HOSTS 16u
/* Sessions retained per peer key. */
#define TLS_SESSION_STORE_MAX_PER_HOST 2u
#define TLS_SESSION_STORE_MAX_ENTRIES \
    (TLS_SESSION_STORE_MAX_HOSTS * TLS_SESSION_STORE_MAX_PER_HOST)
/* Serialized session blob ceiling; larger entries are skipped. */
#define TLS_SESSION_STORE_MAX_ENTRY_BYTES 4096u
/* Peer key string ceiling. */
#define TLS_SESSION_STORE_MAX_KEY_BYTES 512u
/* Whole-file ceiling including header. */
#define TLS_SESSION_STORE_MAX_FILE_BYTES (64u * 1024u)
/* An entry whose valid_until is more than this far in the future is refused:
 * a legitimate ticket cannot outlive RFC 8446's 7-day cap, so a larger value
 * is a wrong (far-future) RTC at export time. */
#define TLS_SESSION_STORE_MAX_FUTURE_SECONDS (8 * 86400)

typedef struct TlsSessionStore TlsSessionStore;

/* An empty in-memory store, or NULL on allocation failure. */
TlsSessionStore *tls_session_store_create(void);
void tls_session_store_free(TlsSessionStore *store);

size_t tls_session_store_count(const TlsSessionStore *store);

/*
 * Add one exported session. `key` is curl's peer_key, stored verbatim.
 * Enforces the blob-size, per-host, host-count, entry-count and total-bytes
 * bounds, evicting the least-recently-used entry (by `last_use`) when a cap is
 * hit. When now > 0, an already-expired or implausibly-far-future entry is
 * dropped. `last_use` orders LRU eviction. Returns true when the entry was
 * retained, false when it was skipped or evicted.
 */
int tls_session_store_add(TlsSessionStore *store, const char *key,
                          const unsigned char *blob, size_t blob_length,
                          int64_t valid_until, int32_t ietf_tls_id,
                          int64_t now, uint64_t last_use);

/* Replays live entries oldest-last_use first. When now > 0, expired and
 * far-future entries are skipped. Returns the number of entries visited. */
typedef void (*TlsSessionImportFn)(void *context, const char *key,
                                   const unsigned char *blob,
                                   size_t blob_length, int32_t ietf_tls_id);
size_t tls_session_store_for_each(const TlsSessionStore *store, int64_t now,
                                  TlsSessionImportFn callback, void *context);

/*
 * Load the store from `path`, falling back to `path`.bak, mirroring the
 * session-persistence backup discipline. Returns NULL on any miss: absent
 * file, torn/corrupt payload, store-format-version mismatch, or crypto-pin
 * mismatch. `crypto_pin` is the current Mbed TLS build tag; `now` prunes
 * non-live entries when positive.
 */
TlsSessionStore *tls_session_store_load(const char *path, uint32_t crypto_pin,
                                        int64_t now);

/*
 * Persist `store` to `path` (+`.bak`) via tmp + backup-rotation + rename with
 * the FAT remove+rename fallback. Non-live entries are pruned first when
 * now > 0. Returns 0 on success, non-zero on an I/O or bounds failure.
 */
int tls_session_store_save(const TlsSessionStore *store, const char *path,
                           uint32_t crypto_pin, int64_t now);

/* Remove the store and its .tmp/.bak generations. Missing files are ignored.
 * Returns 0 when nothing readable remains. */
int tls_session_store_remove(const char *path);

#endif /* TILEFINCH_TLS_SESSION_STORE_H */
