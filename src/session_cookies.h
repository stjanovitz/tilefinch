#ifndef TILEFINCH_SESSION_COOKIES_H
#define TILEFINCH_SESSION_COOKIES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/session.h"

/* Session-internal hooks between session.c and the cookie jar
   (session_cookies.c). */

/* The cookie clock in epoch seconds: the replay clock when a trace replay
   has advanced it, otherwise the wall clock. HSTS shares it. */
int64_t session_cookie_now(void);
/* Adds the live cookies the site at url would receive to usage. False when
   url is not a cookie URL; usage is then untouched. */
bool session_cookie_site_usage(const BrowserSession *session, const char *url,
                               BrowserSiteDataUsage *usage);
/* Clears every cookie the site at url would receive. False when url is not
   a cookie URL. */
bool session_cookie_clear_site(BrowserSession *session, const char *url);

/* Defined in session.c: whether url's site is in a security allow list
   (the mixed-content and third-party-cookie lists). */
bool session_security_site_list_contains(
    const char (*sites)[BROWSER_ORIGIN_LIMIT], size_t count,
    const char *url);

#endif
