#ifndef TILEFINCH_SITE_STORAGE_H
#define TILEFINCH_SITE_STORAGE_H

/* Where a site's storage (localStorage, sessionStorage, OPFS) lives. */
typedef enum {
    BROWSER_SITE_STORAGE_MEMORY = 0,
    BROWSER_SITE_STORAGE_STICK_SESSION,  /* Memory Stick, deleted at exit */
    BROWSER_SITE_STORAGE_STICK_ALWAYS    /* Memory Stick, kept across boots */
} BrowserSiteStorageTier;

/* The user's standing choice for a site. */
typedef enum {
    BROWSER_SITE_STORAGE_ASK = 0,       /* offer the Memory Stick when full */
    BROWSER_SITE_STORAGE_STICK,         /* always keep it on the Memory Stick */
    BROWSER_SITE_STORAGE_MEMORY_ONLY    /* RAM only; never offer */
} BrowserSiteStoragePolicy;

#endif
