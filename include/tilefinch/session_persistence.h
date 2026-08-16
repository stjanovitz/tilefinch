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
    BROWSER_SESSION_PERSISTENCE_OUT_OF_MEMORY
} BrowserSessionPersistenceStatus;

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
BrowserSessionPersistenceStatus browser_session_persistence_clear(
    BrowserSession *session, BrowserSessionPersistenceMask mask);

/* Removes the complete backing file. Missing files are already clear. */
BrowserSessionPersistenceStatus browser_session_persistence_remove(
    const char *path);

const char *browser_session_persistence_status_name(
    BrowserSessionPersistenceStatus status);

#endif
