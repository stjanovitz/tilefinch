#ifndef TILEFINCH_SITE_IDENTITY_H
#define TILEFINCH_SITE_IDENTITY_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Per-site browser identity: the one place where a document may present a
 * User-Agent other than TILEFINCH_BROWSER_USER_AGENT.
 *
 * Google is the only such site. With the shipping compatibility UA it serves
 * a WAP "Update your browser" page, so Google searches open a local
 * compatibility page (site_adapter.c). Choosing "Try Google anyway" opts the
 * browsing session in: from then until the browser restarts, google.com and
 * www.google.com documents use the honest TILEFINCH_GOOGLE_USER_AGENT, for
 * which Google serves its scripted page and SearchGuard check.
 *
 * Scope is the top-level document, as in a real browser: the navigation
 * request (decided by its target), and every request a Google document makes
 * (subresources including third-party ones such as gstatic.com, iframes,
 * fetch/XHR, sendBeacon, workers) carry the same UA, and the document's
 * realms report it as navigator.userAgent/appVersion (workers mirror their
 * owner's navigator). Client Hints already name Tilefinch on PlayStation
 * Portable and need no change. Other top-level documents keep the shipping
 * UA. A redirect hop leaving Google keeps the UA its request started with.
 *
 * The opt-in is process state, never a URL parameter on the wire: the
 * compatibility form carries a tilefinch_raw=1 marker that the navigation
 * loader strips (site_identity_take_google_opt_in_marker) before any request
 * is built. A refused search (HTTP 429, a /sorry/ interstitial, the WAP page,
 * or a failed load) clears the opt-in and asks the browser to show the
 * compatibility page again with a short note.
 */
#define TILEFINCH_GOOGLE_USER_AGENT \
    "Mozilla/5.0 (PlayStation Portable) Tilefinch/0.1"
#define SITE_IDENTITY_GOOGLE_OPT_IN_MARKER "tilefinch_raw"

/* http(s)://google.com or www.google.com on its default port. */
bool site_identity_google_host(const char *url);
/* A Google host whose path is exactly /search. */
bool site_identity_google_search_url(const char *url);

bool site_identity_google_opted_in(void);
void site_identity_google_opt_in(void);

/* When url is a Google search URL carrying tilefinch_raw=1, writes it without
   every tilefinch_raw parameter to output, opts the session in, and returns
   true. Otherwise leaves the state alone and returns false. */
bool site_identity_take_google_opt_in_marker(
    const char *url, char *output, size_t capacity);

/* The User-Agent a document at document_url presents, or NULL for the
   shipping identity. For a top-level navigation pass the target URL. */
const char *site_identity_user_agent_override(const char *document_url);

/* Classify the outcome of a top-level navigation. True when the session is
   opted in and either request_url is Google's /sorry/ interstitial, or it is
   a Google search whose response was refused: a transport/load failure,
   HTTP 429, a redirect to /sorry/, or the XHTML-MP "Update your browser"
   page. Pure: changes no state. */
bool site_identity_google_refusal(
    const char *request_url, const char *effective_url, long status,
    const char *content_type, bool failed);
/* Clears the opt-in, arms the compatibility-page note, and records
   search_url (when it is a Google search) as the pending fallback. */
void site_identity_google_refused(const char *search_url);
/* Consumes the pending fallback navigation, if any. */
bool site_identity_take_google_fallback(char *url, size_t capacity);
void site_identity_clear_google_fallback(void);
/* Consumes the "Google didn't accept this search" note. */
bool site_identity_take_google_refusal_note(void);

/* Restores the browser-start state (tests). */
void site_identity_reset(void);

#endif
