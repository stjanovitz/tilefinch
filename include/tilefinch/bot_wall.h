#ifndef TILEFINCH_BOT_WALL_H
#define TILEFINCH_BOT_WALL_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Recognizes a top-level response that is a bot-protection wall rather than
 * the site: a CAPTCHA or "you have been blocked" page that Tilefinch cannot
 * (and does not try to) get past. The browser then tells the reader the site
 * may not work on Tilefinch instead of leaving a silent blank page.
 *
 * Generic and cheap: a refusal status (403, 405, 429 or 503, plus the
 * 202 that AWS WAF challenges answer with) together with a vendor's own
 * response signature -- a header or Server value DataDome, Cloudflare,
 * Imperva, AWS WAF, Kasada and HUMAN (PerimeterX) set on their walls, or one
 * of their fixed block-page titles. No site is named; nothing is fetched.
 */

#define TILEFINCH_BOT_WALL_VENDOR_LIMIT 24u
#define TILEFINCH_BOT_WALL_SITE_LIMIT 64u

/* The vendor ("DataDome", ...) whose wall the response headers identify,
   or NULL. `headers` is the "name: value\n" snapshot a FetchResult keeps;
   server and cf_mitigated are its parsed Server and cf-mitigated values. */
const char *tilefinch_bot_wall_header_vendor(
    long status, const char *server, const char *cf_mitigated,
    const char *headers, size_t headers_length);

/* The vendor whose block page carries this document title, or NULL. Only
   consulted for refusal statuses. */
const char *tilefinch_bot_wall_title_vendor(long status, const char *title);

/* Whether a status is one bot walls refuse with. */
bool tilefinch_bot_wall_status(long status);

/* The display name of a URL's host without a leading "www." ("nytimes.com"
   for https://www.nytimes.com/...); false when there is no host or it does
   not fit. */
bool tilefinch_bot_wall_site(const char *url, char *site, size_t capacity);

/* The two-line notice the chrome shows, e.g.
   "nytimes.com blocked Tilefinch\nBot protection: this site may not work".
   Fits PSP_UI_STATUS_CAPACITY (80); false if it does not fit `capacity`. */
bool tilefinch_bot_wall_notice(const char *site, char *output,
                               size_t capacity);

#endif
