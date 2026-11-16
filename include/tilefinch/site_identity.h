#ifndef TILEFINCH_SITE_IDENTITY_H
#define TILEFINCH_SITE_IDENTITY_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Per-site browser identity: the one place where a document may present a
 * User-Agent other than TILEFINCH_BROWSER_USER_AGENT. Two Google sites use
 * the honest TILEFINCH_GOOGLE_USER_AGENT instead:
 *
 * - accounts.google.com, always. With the shipping UA it serves its heavy
 *   sign-in page (about 3.9 MB of script, near the PSP's whole budget);
 *   with the honest one it serves a light page of inline scripts. The
 *   identity never changes there: when Google refuses the browser (its
 *   sign-in rejection page, a /sorry/ interstitial, or HTTP 429) the
 *   response commits like any page, and the browser says so in its own
 *   words (site_identity_accounts_refusal) instead of retrying.
 * - google.com and www.google.com, only after "Try Google anyway". With the
 *   shipping UA Google serves a WAP "Update your browser" page, so Google
 *   searches open a local compatibility page (site_adapter.c). Choosing
 *   "Try Google anyway" opts the browsing session in until the browser
 *   restarts.
 *
 * Scope is the top-level document, as in a real browser: the navigation
 * request (decided by its target), and every request a Google document makes
 * (subresources including third-party ones such as gstatic.com, iframes,
 * fetch/XHR, sendBeacon, workers) carry the same UA, and the document's
 * realms report it as navigator.userAgent/appVersion (workers mirror their
 * owner's navigator). Client Hints already name Tilefinch on PlayStation
 * Portable and need no change. Other top-level documents keep the shipping
 * UA. Each redirect hop of a top-level navigation takes the identity of its
 * own target, so every document gets the identity of the host that served
 * it (YouTube's sign-in hands off through accounts.youtube.com and
 * www.youtube.com, which keep the shipping UA).
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

/* http(s)://accounts.google.com on its default port. */
bool site_identity_accounts_host(const char *url);

/* Whether a realm whose document is at document_url gets the ARIA
   reflections: ARIAMixin's 44 string attributes (role included) and its
   eight element-reference attributes. For now only the sites listed in
   site_identity.c do; other realms keep the ~30 KiB of resident bootstrap
   they would cost. The match is on the URL's host, so it covers a child
   frame's realm (accounts.google.com's /_/bscframe) by the frame's own
   URL. */
bool site_identity_aria_reflection_site(const char *document_url);

/* The User-Agent a document at document_url presents, or NULL for the
   shipping identity. For a top-level navigation pass the target URL. */
const char *site_identity_user_agent_override(const char *document_url);
/* Whether user_agent is NULL or the honest identity: a value this module
   chose, which a redirect hop may replace with its target's identity. */
bool site_identity_user_agent_derived(const char *user_agent);

/* How Google answered a top-level navigation on accounts.google.com. */
typedef enum {
    SITE_IDENTITY_ACCOUNTS_ACCEPTED = 0,
    /* Its sign-in rejection page (/signin/rejected or /v3/signin/rejected):
       "Couldn't sign you in. This browser or app may not be secure." */
    SITE_IDENTITY_ACCOUNTS_REJECTED = 1,
    /* A /sorry/ interstitial on accounts.google.com, or Google's own after
       an accounts request (site_identity_accounts_request). */
    SITE_IDENTITY_ACCOUNTS_INTERSTITIAL = 2,
    /* HTTP 429 from accounts.google.com. */
    SITE_IDENTITY_ACCOUNTS_RATE_LIMITED = 3
} SiteIdentityAccountsRefusal;

/* Whether a navigation that ends on Google's /sorry/ interstitial counts as
   an accounts.google.com refusal: its request went to accounts.google.com,
   or an accounts.google.com page (source_url) opened the interstitial. */
bool site_identity_accounts_request(const char *request_url,
                                    const char *source_url);
/* Classify a top-level navigation's response by where it landed
   (effective_url) and its HTTP status. Pure: changes no state. reason, when
   not NULL, receives the rejection page's reason number (the digits of its
   rrk query value, at most four), or 0. */
SiteIdentityAccountsRefusal site_identity_accounts_refusal(
    bool accounts_request, const char *effective_url, long status,
    unsigned *reason);

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
/* Consumes the pending fallback navigation, if any: a Google search for
   the compatibility page. */
bool site_identity_take_google_fallback(char *url, size_t capacity);
void site_identity_clear_google_fallback(void);
/* Consumes the "Google didn't accept this search" note. */
bool site_identity_take_google_refusal_note(void);

/* Restores the browser-start state (tests). */
void site_identity_reset(void);

#endif
