#ifndef TILEFINCH_YOUTUBE_LOGIN_H
#define TILEFINCH_YOUTUBE_LOGIN_H
#include "tilefinch/session.h"
typedef enum {
    YOUTUBE_LOGIN_ASK = 0, YOUTUBE_LOGIN_ALWAYS, YOUTUBE_LOGIN_NEVER
} YoutubeLoginPolicy;
/* Native consent only. Unencrypted session credentials; never passwords.
   No shutdown save. Load is startup-only and refuses a nonempty cookie jar. */
bool youtube_login_save(const BrowserSession *, const char *path, int64_t now);
bool youtube_login_load(BrowserSession *, const char *path, int64_t now);
bool youtube_login_remove(const char *path);
#endif
