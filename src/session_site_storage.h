#ifndef TILEFINCH_SESSION_SITE_STORAGE_H
#define TILEFINCH_SESSION_SITE_STORAGE_H

#include <stdbool.h>
#include <stddef.h>

#include "tilefinch/session.h"

/* Session-internal hooks into the site store (session_site_storage.c). */

/* Frees the store. This-session-only Memory Stick files are deleted. */
void site_storage_destroy(BrowserSession *session);
/* Captive sign-in: the portal runs on an empty, RAM-only store. */
struct BrowserSiteStore *site_storage_detach(BrowserSession *session);
void site_storage_attach(BrowserSession *session,
                         struct BrowserSiteStore *store);
void site_storage_usage(const BrowserSession *session, const char *origin,
                        BrowserSiteDataUsage *usage);
/* Deletes the origin's data; its tier and standing choice remain. */
void site_storage_clear_origin(BrowserSession *session, const char *origin);

/* The "Save local data" snapshot covers localStorage kept in RAM; a site
   on the Memory Stick already has its own file. */
typedef struct {
    const char *origin;
    const char *key;
    const char *value;
    size_t value_length;
} SiteStorageLocalView;
bool site_storage_next_local(const BrowserSession *session, size_t *cursor,
                             SiteStorageLocalView *view);
size_t site_storage_local_count(const BrowserSession *session);
/* Replacing the RAM localStorage with a loaded snapshot: fits() checks
   limits and reserves everything adopt() needs, so adopt() cannot fail.
   Origins kept on the Memory Stick in target are skipped. */
bool site_storage_local_fits(BrowserSession *target,
                             const BrowserSession *staging);
void site_storage_adopt_local(BrowserSession *target,
                              BrowserSession *staging);

#endif
