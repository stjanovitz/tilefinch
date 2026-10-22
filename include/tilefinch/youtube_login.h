#ifndef TILEFINCH_YOUTUBE_LOGIN_H
#define TILEFINCH_YOUTUBE_LOGIN_H
#include "tilefinch/session.h"
typedef enum {
    YOUTUBE_LOGIN_ASK = 0, YOUTUBE_LOGIN_ALWAYS, YOUTUBE_LOGIN_NEVER
} YoutubeLoginPolicy;
typedef enum {
    YOUTUBE_LOGIN_LOADED = 0,
    /* No file, a nonempty jar, or a damaged or refused file. */
    YOUTUBE_LOGIN_NOT_LOADED,
    /* The file holds a cookie whose name=value cannot fit a Cookie header
       (BROWSER_COOKIE_HEADER_CAPACITY), so it could never be sent; the
       whole login is refused, the file is removed (so the refusal is
       reported once), and the user must sign in again. */
    YOUTUBE_LOGIN_COOKIE_TOO_LARGE
} YoutubeLoginLoadStatus;
/* Native consent only. Unencrypted session credentials; never passwords.
   No shutdown save. Load is startup-only and refuses a nonempty cookie jar.
   Save leaves out cookies that could never be sent. status may be NULL. */
bool youtube_login_save(const BrowserSession *, const char *path, int64_t now);
bool youtube_login_load(BrowserSession *, const char *path, int64_t now,
                        YoutubeLoginLoadStatus *status);
bool youtube_login_remove(const char *path);
#endif
