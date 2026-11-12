#ifndef TILEFINCH_SESSION_PERSISTENCE_H
#define TILEFINCH_SESSION_PERSISTENCE_H

#include <stddef.h>
#include <stdint.h>

#include "tilefinch/session.h"

/*
 * Persistence is an explicit frontend action. Nothing in BrowserSession
 * opens a file during initialization or destruction. There is intentionally
 * no sessionStorage bit: only localStorage is eligible for serialization.
 */
typedef uint32_t BrowserSessionPersistenceMask;

#define BROWSER_SESSION_PERSIST_CACHE UINT32_C(1)
#define BROWSER_SESSION_PERSIST_LOCAL_STORAGE UINT32_C(2)
#define BROWSER_SESSION_PERSIST_ALL                                      \
    (BROWSER_SESSION_PERSIST_CACHE | BROWSER_SESSION_PERSIST_LOCAL_STORAGE)

#define BROWSER_SESSION_PERSIST_MAX_FILE_BYTES (5u * 1024u * 1024u)
#define BROWSER_SESSION_PERSIST_MAX_CACHE_BYTES (4u * 1024u * 1024u)

typedef struct {
    size_t maximum_file_bytes;
    size_t maximum_cache_bytes;
    size_t maximum_cache_entries;
    size_t maximum_local_storage_entries;
} BrowserSessionPersistenceLimits;

typedef enum {
    BROWSER_SESSION_PERSISTENCE_OK = 0,
    BROWSER_SESSION_PERSISTENCE_INVALID_ARGUMENT,
    BROWSER_SESSION_PERSISTENCE_NOT_FOUND,
    BROWSER_SESSION_PERSISTENCE_IO_ERROR,
    BROWSER_SESSION_PERSISTENCE_LIMIT_EXCEEDED,
    BROWSER_SESSION_PERSISTENCE_CORRUPT,
    BROWSER_SESSION_PERSISTENCE_OUT_OF_MEMORY,
    /* A later release wrote the snapshot. Loads leave it and its backup
       alone, and saves refuse to rotate it away. */
    BROWSER_SESSION_PERSISTENCE_NEWER_FORMAT
} BrowserSessionPersistenceStatus;

typedef struct BrowserSessionPersistenceLoad BrowserSessionPersistenceLoad;

typedef enum {
    BROWSER_SESSION_PERSISTENCE_LOAD_PENDING = 0,
    BROWSER_SESSION_PERSISTENCE_LOAD_COMPLETE,
    BROWSER_SESSION_PERSISTENCE_LOAD_FAILED
} BrowserSessionPersistenceLoadProgress;

void browser_session_persistence_limits_default(
    BrowserSessionPersistenceLimits *limits);

/*
 * save() retains the newest reusable cache entries that fit the requested
 * snapshot ceiling, writes path.tmp completely, closes it, and then rotates
 * it over path while retaining the prior valid generation at path.bak.
 * load() stages every requested category under the session Budget and changes
 * the live session only after the complete file and checksum validate.
 */
BrowserSessionPersistenceStatus browser_session_persistence_save(
    const BrowserSession *session, const char *path,
    BrowserSessionPersistenceMask mask,
    const BrowserSessionPersistenceLimits *limits);
BrowserSessionPersistenceStatus browser_session_persistence_load(
    BrowserSession *session, const char *path,
    BrowserSessionPersistenceMask mask,
    const BrowserSessionPersistenceLimits *limits);

/*
 * Incremental counterpart to load(). The loader keeps one file open and
 * reads at most maximum_bytes per pump. Restored objects remain in a staging
 * session until the complete payload, record counts, and checksum validate;
 * cancellation or failure therefore leaves the live session unchanged.
 *
 * begin() may open the primary file and allocate the small loader/staging
 * records, but does not read payload bytes. A corrupt or missing primary is
 * retried from the rotated backup within later pump calls.
 * New loads are refused during captive sign-in. An already pending load
 * pauses without reading/publishing while that temporary partition is active,
 * and resumes into the ordinary session after sign-in closes. Terminal load
 * results remain terminal even if a portal subsequently opens.
 */
/* The size of the file a save of MASK would write now, header included,
   counted without writing or hashing (a free-space preflight). The save
   itself returns the same status for a plan that does not fit. */
BrowserSessionPersistenceStatus browser_session_persistence_planned_bytes(
    const BrowserSession *session, BrowserSessionPersistenceMask mask,
    const BrowserSessionPersistenceLimits *limits, uint64_t *bytes);
BrowserSessionPersistenceLoad *browser_session_persistence_load_begin(
    BrowserSession *session, const char *path,
    BrowserSessionPersistenceMask mask,
    const BrowserSessionPersistenceLimits *limits);
BrowserSessionPersistenceLoadProgress browser_session_persistence_load_pump(
    BrowserSessionPersistenceLoad *load, size_t maximum_bytes,
    BrowserSessionPersistenceStatus *status);
void browser_session_persistence_load_destroy(
    BrowserSessionPersistenceLoad *load);
BrowserSessionPersistenceStatus browser_session_persistence_clear(
    BrowserSession *session, BrowserSessionPersistenceMask mask);

/* Removes the complete backing file. Missing files are already clear. */
BrowserSessionPersistenceStatus browser_session_persistence_remove(
    const char *path);

/* True when a load that ended with STATUS may have left a snapshot on disk
   that the session never read (an I/O error, a memory or limit refusal, a
   newer format).
   Saving over it would rotate data the session does not have behind an
   empty or partial store, so callers skip that save. OK, NOT_FOUND (nothing
   to keep) and CORRUPT (both generations unreadable) return false. */
bool browser_session_persistence_load_left_unread(
    BrowserSessionPersistenceStatus status);

const char *browser_session_persistence_status_name(
    BrowserSessionPersistenceStatus status);

#endif
