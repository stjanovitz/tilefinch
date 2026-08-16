#ifndef TILEFINCH_OMNIBOX_H
#define TILEFINCH_OMNIBOX_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    BROWSER_SEARCH_GOOGLE = 0,
    BROWSER_SEARCH_BING,
    BROWSER_SEARCH_DUCKDUCKGO
} BrowserSearchEngine;

const char *browser_search_engine_name(BrowserSearchEngine engine);
bool browser_search_engine_valid(BrowserSearchEngine engine);

typedef enum {
    BROWSER_OMNIBOX_NAVIGATION = 0,
    BROWSER_OMNIBOX_SEARCH
} BrowserOmniboxResolutionKind;

/*
 * Resolve an address-bar entry without network or site-specific state.
 * Explicit HTTP(S) URLs and host-like entries become navigations; all other
 * text becomes an encoded query for the selected search provider.
 */
bool browser_omnibox_resolve(
    const char *input, BrowserSearchEngine engine,
    char *output, size_t output_capacity);
bool browser_omnibox_resolve_kind(
    const char *input, BrowserSearchEngine engine,
    char *output, size_t output_capacity,
    BrowserOmniboxResolutionKind *kind);

#endif
