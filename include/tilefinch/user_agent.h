#ifndef TILEFINCH_USER_AGENT_H
#define TILEFINCH_USER_AGENT_H

/*
 * Keep network and JavaScript identity aligned.  The iPhone/WebKit/Safari
 * fields are compatibility tokens, as in mainstream browser user agents;
 * Tilefinch and the actual PSP platform remain explicit in the same product
 * comment.  navigator.platform and User-Agent Client Hints report the real
 * platform and capabilities rather than copying the compatibility profile.
 */
#define TILEFINCH_BROWSER_USER_AGENT                                      \
    "Mozilla/5.0 (iPhone; PlayStation Portable; Tilefinch/0.1) "         \
    "AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.4 "               \
    "Mobile/15E148 Safari/604.1"

#define TILEFINCH_BROWSER_BRAND "Tilefinch"
#define TILEFINCH_BROWSER_BRAND_VERSION "0.1"
#define TILEFINCH_BROWSER_FULL_VERSION "0.1.1"

#endif
