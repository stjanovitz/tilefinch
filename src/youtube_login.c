#include "tilefinch/youtube_login.h"
#include "tilefinch/sha256.h"
#include "tilefinch_test_faults.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#define LOGIN_CAP (64u * 1024u)
#define LOGIN_HEADER 48u
#define LOGIN_PATH 800u
#define LOGIN_VALUE 4096u

static void wipe(void *memory, size_t length)
{
    volatile unsigned char *p = memory;
    while (length-- != 0) *p++ = 0;
}
static bool domain_allowed(const char *domain)
{
    size_t n = strlen(domain);
    return strcmp(domain, "youtube.com") == 0
        || (n > 12u && strcmp(domain + n - 12u, ".youtube.com") == 0);
}
static bool eligible(const BrowserCookieEntry *c, int64_t now)
{
    return c->value != NULL && c->secure && !c->partitioned
        && domain_allowed(c->domain)
        && (c->expires_at == 0 || c->expires_at > now)
        && c->value_length <= LOGIN_VALUE
        && strlen(browser_cookie_entry_path(c)) < BROWSER_ORIGIN_LIMIT;
}
static void put32(unsigned char *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; i++) p[i] = (unsigned char) (v >> (8u * i));
}
static uint32_t get32(const unsigned char *p)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < 4; i++) v |= (uint32_t) p[i] << (8u * i);
    return v;
}
static FILE *open_file(const char *path, bool writing)
{
    int flags = writing ? O_WRONLY | O_CREAT | O_EXCL : O_RDONLY;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    /* PSP FAT has no symlinks; hosts must not follow credential-file links. */
    int fd = open(path, flags, 0600);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { close(fd); return NULL; }
    FILE *file = fdopen(fd, writing ? "wb" : "rb");
    if (file == NULL) close(fd);
    return file;
}
static bool temporary_path(const char *path, char temporary[LOGIN_PATH])
{
    if (path == NULL || path[0] == '\0') return false;
    int n = snprintf(temporary, LOGIN_PATH, "%s.tmp", path);
    return n > 0 && (size_t) n < LOGIN_PATH;
}
static int publish_rename(const char *temporary, const char *path)
{
#ifndef __PSP__
    if (tilefinch_test_faults()->youtube_login_no_replace_rename
        && access(path, F_OK) == 0) { errno = EEXIST; return -1; }
#endif
    return rename(temporary, path);
}
bool youtube_login_remove(const char *path)
{
    char temporary[LOGIN_PATH];
    if (!temporary_path(path, temporary)) return false;
    bool okay = remove(path) == 0 || errno == ENOENT;
    return (remove(temporary) == 0 || errno == ENOENT) && okay;
}
bool youtube_login_save(const BrowserSession *session, const char *path, int64_t now)
{
    char temporary[LOGIN_PATH];
    if (session == NULL || !session->site_data_allowed || now < 0
        || !temporary_path(path, temporary)) return false;
    size_t size = LOGIN_HEADER, count = 0;
    for (size_t i = 0; i < BROWSER_COOKIE_ENTRIES; i++) {
        const BrowserCookieEntry *c = &session->cookies[i];
        if (!eligible(c, now)) continue;
        size_t bytes = 24u + strlen(c->domain) + strlen(browser_cookie_entry_path(c))
            + strlen(c->name) + c->value_length;
        if (bytes > LOGIN_CAP - size) return false;
        size += bytes; count++;
    }
    if (count == 0) return false;
    unsigned char *buffer = budget_calloc(session->budget, 1, size);
    if (buffer == NULL) return false;
    memcpy(buffer, "TFYT\1\0\0\0", 8);
    put32(buffer + 8, (uint32_t) size); put32(buffer + 12, (uint32_t) count);
    size_t at = LOGIN_HEADER;
    for (size_t i = 0; i < BROWSER_COOKIE_ENTRIES; i++) {
        const BrowserCookieEntry *c = &session->cookies[i];
        if (!eligible(c, now)) continue;
        const char *parts[] = {c->domain, browser_cookie_entry_path(c), c->name, c->value};
        size_t lengths[] = {strlen(parts[0]), strlen(parts[1]), strlen(parts[2]), c->value_length};
        for (unsigned k = 0; k < 4; k++) put32(buffer + at + 4u * k, (uint32_t) lengths[k]);
        put32(buffer + at + 16, (uint32_t) c->expires_at);
        put32(buffer + at + 20, (uint32_t) ((uint64_t) c->expires_at >> 32));
        buffer[at + 3] = (unsigned char) ((c->host_only ? 1u : 0u)
            | (c->http_only ? 2u : 0u) | ((unsigned) c->same_site << 2));
        at += 24u;
        for (unsigned k = 0; k < 4; k++) {
            memcpy(buffer + at, parts[k], lengths[k]); at += lengths[k];
        }
    }
    bool okay = tilefinch_sha256_digest(buffer + LOGIN_HEADER, size - LOGIN_HEADER, buffer + 16);
    okay = okay && (remove(temporary) == 0 || errno == ENOENT);
    FILE *file = okay ? open_file(temporary, true) : NULL;
    okay = file != NULL && fwrite(buffer, 1, size, file) == size;
    if (file != NULL && fclose(file) != 0) okay = false;
    if (okay) {
        okay = publish_rename(temporary, path) == 0;
        /* FAT cannot replace a destination. Remove only after the complete
           temporary file is closed, and only for that refusal. No backup of
           credentials survives sign-out; a power loss here loses the saved
           login, never the live session. Other I/O failures keep the old file. */
        if (!okay && errno == EEXIST)
            okay = remove(path) == 0 && publish_rename(temporary, path) == 0;
    }
    if (file != NULL && !okay) (void) remove(temporary);
    wipe(buffer, size); budget_free(session->budget, buffer);
    return okay;
}
bool youtube_login_load(BrowserSession *session, const char *path, int64_t now)
{
    if (session == NULL || !session->site_data_allowed || now < 0 || path == NULL) return false;
    for (size_t i = 0; i < BROWSER_COOKIE_ENTRIES; i++)
        if (session->cookies[i].value != NULL) return false;
    FILE *file = open_file(path, false);
    if (file == NULL) return false;
    unsigned char header[LOGIN_HEADER], hash[32];
    bool okay = fread(header, 1, sizeof(header), file) == sizeof(header)
        && memcmp(header, "TFYT\1\0\0\0", 8) == 0;
    size_t size = okay ? get32(header + 8) : 0, count = okay ? get32(header + 12) : 0;
    okay = okay && size >= LOGIN_HEADER && size <= LOGIN_CAP && count > 0 && count <= BROWSER_COOKIE_ENTRIES;
    unsigned char *buffer = okay ? budget_malloc(session->budget, size) : NULL;
    BrowserSession *staged = buffer != NULL ? budget_calloc(session->budget, 1, sizeof(*staged)) : NULL;
    bool initialized = staged != NULL && browser_session_init(staged, session->budget, 1);
    okay = initialized && fread(buffer, 1, size - LOGIN_HEADER, file) == size - LOGIN_HEADER
        && fgetc(file) == EOF && !ferror(file)
        && tilefinch_sha256_digest(buffer, size - LOGIN_HEADER, hash)
        && memcmp(hash, header + 16, sizeof(hash)) == 0;
    if (fclose(file) != 0) okay = false;
    if (initialized) staged->maximum_cookie_bytes = session->maximum_cookie_bytes;
    size_t at = 0, payload = size >= LOGIN_HEADER ? size - LOGIN_HEADER : 0;
    for (size_t i = 0; okay && i < count; i++) {
        if (at > payload || 24u > payload - at) { okay = false; break; }
        unsigned flags = buffer[at + 3]; size_t lengths[4];
        lengths[0] = get32(buffer + at) & UINT32_C(0x00ffffff);
        for (unsigned k = 1; k < 4; k++) lengths[k] = get32(buffer + at + 4u * k);
        uint64_t expiry = get32(buffer + at + 16) | ((uint64_t) get32(buffer + at + 20) << 32);
        at += 24u; size_t total = 0;
        for (unsigned k = 0; k < 4; k++) {
            size_t cap = k == 2 ? BROWSER_KEY_LIMIT : k == 3 ? LOGIN_VALUE + 1u : BROWSER_ORIGIN_LIMIT;
            if (lengths[k] >= cap || (k != 3 && lengths[k] == 0)
                || lengths[k] > payload - at - total) { okay = false; break; }
            total += lengths[k];
        }
        if (!okay || (flags >> 2) > BROWSER_COOKIE_SAME_SITE_NONE || expiry > INT64_MAX) { okay = false; break; }
        char domain[BROWSER_ORIGIN_LIMIT], cookie[LOGIN_VALUE + 1024u], url[BROWSER_ORIGIN_LIMIT + 16u];
        memcpy(domain, buffer + at, lengths[0]); domain[lengths[0]] = '\0';
        if (memchr(buffer + at, 0, total) != NULL || memchr(buffer + at, ';', total) != NULL
            || memchr(buffer + at, '\r', total) != NULL || memchr(buffer + at, '\n', total) != NULL
            || !domain_allowed(domain)) { okay = false; break; }
        const unsigned char *path_part = buffer + at + lengths[0];
        const unsigned char *name = path_part + lengths[1], *value = name + lengths[2];
        int n = snprintf(cookie, sizeof(cookie), "%.*s=%.*s; Path=%.*s; Secure%s%s%.*s%s%s",
            (int) lengths[2], name, (int) lengths[3], value, (int) lengths[1], path_part,
            flags & 2u ? "; HttpOnly" : "", flags & 1u ? "" : "; Domain=",
            flags & 1u ? 0 : (int) lengths[0], domain,
            flags >> 2 ? "; SameSite=" : "",
            (flags >> 2) == BROWSER_COOKIE_SAME_SITE_STRICT ? "Strict"
                : (flags >> 2) == BROWSER_COOKIE_SAME_SITE_LAX ? "Lax"
                : (flags >> 2) == BROWSER_COOKIE_SAME_SITE_NONE ? "None" : "");
        snprintf(url, sizeof(url), "https://%s/", domain);
        bool live = expiry == 0 || expiry > (uint64_t) now;
        if (n < 0 || (size_t) n >= sizeof(cookie)
            || (live && !browser_session_cookie_set_http(staged, url, cookie))) okay = false;
        wipe(cookie, sizeof(cookie));
        if (okay && live) {
            BrowserCookieEntry *found = NULL;
            for (size_t j = 0; j < BROWSER_COOKIE_ENTRIES; j++) {
                BrowserCookieEntry *c = &staged->cookies[j];
                if (c->value != NULL && strlen(c->name) == lengths[2]
                    && memcmp(c->name, name, lengths[2]) == 0 && strcmp(c->domain, domain) == 0
                    && strlen(browser_cookie_entry_path(c)) == lengths[1]
                    && memcmp(browser_cookie_entry_path(c), path_part, lengths[1]) == 0
                    && c->value_length == lengths[3] && memcmp(c->value, value, lengths[3]) == 0
                    && c->host_only == ((flags & 1u) != 0) && c->http_only == ((flags & 2u) != 0)
                    && (unsigned) c->same_site == (flags >> 2)) found = c;
            }
            if (found == NULL) okay = false;
            else found->expires_at = (int64_t) expiry;
        }
        at += total;
    }
    okay = okay && at == payload;
    if (okay) {
        memcpy(session->cookies, staged->cookies, sizeof(session->cookies));
        session->cookie_bytes = staged->cookie_bytes;
        session->cookie_long_path_bytes = staged->cookie_long_path_bytes;
        session->cookie_clock = staged->cookie_clock;
        memset(staged->cookies, 0, sizeof(staged->cookies));
        staged->cookie_bytes = staged->cookie_long_path_bytes = 0;
    }
    if (initialized) browser_session_destroy(staged);
    budget_free(session->budget, staged);
    if (buffer != NULL) { wipe(buffer, size); budget_free(session->budget, buffer); }
    wipe(header, sizeof(header)); wipe(hash, sizeof(hash));
    return okay;
}
