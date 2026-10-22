#include "tilefinch/youtube_login.h"
#include "tilefinch/youtube_lite.h"
#include "tilefinch/browser_profile.h"
#include "tilefinch/psp_ui.h"
#include "tilefinch/sha256.h"
#include "../src/tilefinch_test_faults.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "LOGIN CHECK %d: %s\n", __LINE__, #c); return 1; } } while (0)
static PspUiIntent press(PspUiState *ui, uint32_t button)
{
    PspUiInput input = {.pressed = button};
    return psp_ui_update(ui, &input);
}
static uint64_t identity_of(BrowserSession *session)
{
    return youtube_lite_session_login_identity(session);
}

/* The login identity follows every jar change at once, while repeated reads
   of an unchanged jar build no Cookie header (review 2026-10-21 M5). */
static int test_login_identity_follows_the_jar(Budget *budget)
{
    BrowserSession session, other;
    CHECK(browser_session_init(&session, budget, 1));
    CHECK(browser_session_init(&other, budget, 1));
    CHECK(identity_of(&session) == 0);
    CHECK(browser_session_cookie_set_http(&session, "https://youtube.com/",
        "SAPISID=first; Secure; Path=/; Domain=youtube.com"));
    uint64_t first = identity_of(&session);
    CHECK(first != 0);
    tilefinch_test_faults()->youtube_login_identity_builds = 0;
    for (unsigned read = 0; read < 100u; read++) CHECK(identity_of(&session) == first);
    /* One rebuild is allowed for the wall clock crossing a second. */
    CHECK(tilefinch_test_faults()->youtube_login_identity_builds <= 1u);
    CHECK(browser_session_cookie_set_http(&session, "https://youtube.com/",
        "PREF=f6=40000000; Secure; Path=/; Domain=youtube.com"));
    CHECK(identity_of(&session) == first);
    /* A same-length replacement is a different identity immediately. */
    CHECK(browser_session_cookie_set_http(&session, "https://youtube.com/",
        "SAPISID=other; Secure; Path=/; Domain=youtube.com"));
    uint64_t replaced = identity_of(&session);
    CHECK(replaced != 0 && replaced != first);
    CHECK(browser_session_cookie_set_http(&session, "https://youtube.com/",
        "SAPISID=first; Secure; Path=/; Domain=youtube.com"));
    CHECK(identity_of(&session) == first);
    /* The third-party copy stands in once SAPISID itself is gone. */
    CHECK(browser_session_cookie_set_http(&session, "https://youtube.com/",
        "__Secure-3PAPISID=third; Secure; Path=/; Domain=youtube.com"));
    CHECK(identity_of(&session) == first);
    CHECK(browser_session_cookie_set_http(&session, "https://youtube.com/",
        "SAPISID=gone; Secure; Path=/; Domain=youtube.com; Max-Age=0"));
    uint64_t third = identity_of(&session);
    CHECK(third != 0 && third != first);
    /* A cookie the request would not carry is not an identity. */
    CHECK(browser_session_cookie_set_http(&session, "https://youtube.com/",
        "__Secure-3PAPISID=third; Secure; Path=/; Domain=youtube.com; Max-Age=0"));
    CHECK(browser_session_cookie_set_http(&session, "https://youtube.com/",
        "SAPISID=deep; Secure; Path=/feed; Domain=youtube.com"));
    CHECK(identity_of(&session) == 0);
    CHECK(browser_session_cookie_set_http(&session, "https://youtube.com/",
        "SAPISID=first; Secure; Path=/; Domain=youtube.com"));
    CHECK(identity_of(&session) == first);
    browser_session_set_site_data_allowed(&session, false);
    CHECK(identity_of(&session) == 0);
    browser_session_set_site_data_allowed(&session, true);
    CHECK(identity_of(&session) == first);
    /* Each session is its own jar. */
    CHECK(identity_of(&other) == 0 && identity_of(&session) == first);
    browser_session_cookie_clear(&session);
    CHECK(identity_of(&session) == 0);
    browser_session_destroy(&other);
    browser_session_destroy(&session);
    return 0;
}

static void put32(unsigned char *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; i++) p[i] = (unsigned char) (v >> (8u * i));
}

/* A saved login whose one cookie (Secure, HttpOnly, Domain=youtube.com,
   Path=/, session lifetime) has a value of value_length bytes, written as an
   older build would have saved it. */
static bool write_login_file(const char *path, const char *name, size_t value_length)
{
    static unsigned char file[48u + 24u + 64u + 4096u];
    size_t name_length = strlen(name), record = 24u + 11u + 1u + name_length + value_length;
    if (48u + record > sizeof(file)) return false;
    memset(file, 0, sizeof(file));
    memcpy(file, "TFYT\1\0\0\0", 8);
    put32(file + 8, (uint32_t) (48u + record)); put32(file + 12, 1u);
    unsigned char *at = file + 48;
    put32(at, 11u); put32(at + 4, 1u); put32(at + 8, (uint32_t) name_length);
    put32(at + 12, (uint32_t) value_length);
    at[3] = 2u; /* HttpOnly, domain cookie, SameSite unset */
    memcpy(at + 24, "youtube.com/", 12);
    memcpy(at + 36, name, name_length);
    memset(at + 36 + name_length, 'v', value_length);
    if (!tilefinch_sha256_digest(file + 48, record, file + 16)) return false;
    FILE *out = fopen(path, "wb");
    bool okay = out != NULL && fwrite(file, 1, 48u + record, out) == 48u + record;
    if (out != NULL && fclose(out) != 0) okay = false;
    return okay;
}

/* A cookie whose name=value cannot fit a Cookie header is never saved, and
   a file that holds one is refused with its own status (review 2026-10-21
   M4: older builds saved values of up to 4 KiB). */
static int test_unsendable_saved_cookie(Budget *budget, const char *path)
{
    BrowserSession source, target;
    YoutubeLoginLoadStatus status = YOUTUBE_LOGIN_NOT_LOADED;
    static char cookie[4200], header[BROWSER_COOKIE_HEADER_CAPACITY];
    CHECK(browser_session_init(&source, budget, 1) && browser_session_init(&target, budget, 1));
    source.maximum_cookie_bytes = target.maximum_cookie_bytes = 16u * 1024u;
    memcpy(cookie, "LOGIN_INFO=", 11);
    memset(cookie + 11, 'v', 4090);
    snprintf(cookie + 11 + 4090, sizeof(cookie) - 11 - 4090,
             "; Secure; HttpOnly; Path=/; Domain=youtube.com");
    CHECK(browser_session_cookie_set_http(&source, "https://youtube.com/", cookie));
    CHECK(browser_session_cookie_set_http(&source, "https://youtube.com/",
        "SAPISID=synthetic; Secure; Path=/; Domain=youtube.com"));
    CHECK(youtube_login_save(&source, path, 1700000000));
    CHECK(youtube_login_load(&target, path, 1700000000, &status) && status == YOUTUBE_LOGIN_LOADED);
    CHECK(browser_session_cookie_header(&target, "https://m.youtube.com/", header, sizeof(header))
        && strcmp(header, "SAPISID=synthetic") == 0 && target.cookie_bytes == 9u);
    browser_session_cookie_clear(&target);
    CHECK(write_login_file(path, "LOGIN_INFO", 4090u));
    status = YOUTUBE_LOGIN_LOADED;
    CHECK(!youtube_login_load(&target, path, 1700000000, &status));
    CHECK(status == YOUTUBE_LOGIN_COOKIE_TOO_LARGE && target.cookie_bytes == 0);
    /* The refused file is gone, so the next start does not report it again. */
    CHECK(access(path, F_OK) != 0);
    CHECK(!youtube_login_load(&target, path, 1700000000, &status)
        && status == YOUTUBE_LOGIN_NOT_LOADED);
    /* The same file with a value that fits loads. */
    CHECK(write_login_file(path, "LOGIN_INFO", 4000u));
    CHECK(youtube_login_load(&target, path, 1700000000, &status) && status == YOUTUBE_LOGIN_LOADED
        && target.cookie_bytes == 4000u);
    browser_session_destroy(&source); browser_session_destroy(&target);
    CHECK(youtube_login_remove(path));
    return 0;
}

int main(void)
{
    char directory[] = "/tmp/tilefinch-login-XXXXXX";
    CHECK(mkdtemp(directory));
    char path[256], profile_path[256], temporary[260], victim[256];
    snprintf(path, sizeof(path), "%s/login", directory);
    snprintf(profile_path, sizeof(profile_path), "%s/profile", directory);
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    snprintf(victim, sizeof(victim), "%s/victim", directory);
    Budget budget; budget_init(&budget, 4u * 1024u * 1024u);
    if (test_login_identity_follows_the_jar(&budget)) return 1;
    if (test_unsendable_saved_cookie(&budget, path)) return 1;
    BrowserSession source, target;
    CHECK(browser_session_init(&source, &budget, 1));
    CHECK(browser_session_init(&target, &budget, 1));
    CHECK(!youtube_login_load(&target, path, 1700000000, NULL));
    CHECK(browser_session_cookie_set_http(&source, "https://youtube.com/",
        "SAPISID=synthetic; Secure; HttpOnly; SameSite=Lax; Path=/; Domain=youtube.com"));
    CHECK(browser_session_cookie_set_http(&source, "https://m.youtube.com/",
        "local=host-only; Secure; SameSite=Strict; Path=/watch"));
    CHECK(browser_session_cookie_set_http(&source, "https://accounts.google.com/",
        "SID=never-save-google; Secure; HttpOnly; Path=/"));
    CHECK(browser_session_cookie_set_http(&source, "https://notyoutube.com/",
        "SID=never-save-other; Secure; Path=/"));
    CHECK(browser_session_cookie_set_http(&source, "https://youtube.com/",
        "unsafe=non-secure; Path=/"));
    CHECK(access(path, F_OK) != 0); /* Creating/signing in is not consent. */
    CHECK(youtube_login_save(&source, path, 1700000000));
    struct stat st; CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
    CHECK(youtube_login_load(&target, path, 1700000000, NULL));
    char header[1024];
    CHECK(browser_session_cookie_header(&target, "https://m.youtube.com/watch", header, sizeof(header)));
    CHECK(strstr(header, "SAPISID=synthetic") && strstr(header, "local=host-only") && !strstr(header, "unsafe"));
    CHECK(browser_session_cookie_header(&target, "https://accounts.google.com/", header, sizeof(header)) && header[0] == '\0');
    CHECK(!youtube_login_load(&target, path, 1700000000, NULL)); /* Never clobber a live jar. */
    browser_session_cookie_clear(&target);
    /* A second save must install the new cookie on FAT, not retain the old one. */
    CHECK(browser_session_cookie_set_http(&source, "https://youtube.com/",
        "SAPISID=synthetic-updated; Secure; HttpOnly; SameSite=Lax; Path=/; Domain=youtube.com"));
    tilefinch_test_faults()->replace_file_no_replace = true;
    CHECK(youtube_login_save(&source, path, 1700000000));
    tilefinch_test_faults()->replace_file_no_replace = false;
    CHECK(youtube_login_load(&target, path, 1700000000, NULL));
    CHECK(browser_session_cookie_header(&target, "https://m.youtube.com/watch", header, sizeof(header))
        && strstr(header, "SAPISID=synthetic-updated"));
    browser_session_cookie_clear(&target);
    CHECK(browser_session_cookie_set_http(&source, "https://youtube.com/",
        "SAPISID=synthetic; Secure; HttpOnly; SameSite=Lax; Path=/; Domain=youtube.com"));
    CHECK(youtube_login_save(&source, path, 1700000000));
    size_t limit = budget.limit; budget.limit = budget.current;
    CHECK(!youtube_login_load(&target, path, 1700000000, NULL) && target.cookie_bytes == 0);
    CHECK(!youtube_login_save(&source, path, 1700000000)); budget.limit = limit;
    CHECK(youtube_login_load(&target, path, 1700000000, NULL));
    browser_session_cookie_clear(&target);
    FILE *file = fopen(path, "r+b"); CHECK(file && fseek(file, -1, SEEK_END) == 0);
    CHECK(fputc('!', file) != EOF && fclose(file) == 0);
    CHECK(!youtube_login_load(&target, path, 1700000000, NULL) && target.cookie_bytes == 0);
    CHECK(youtube_login_save(&source, path, 1700000000));
    CHECK(rename(path, victim) == 0 && symlink(victim, path) == 0);
    CHECK(!youtube_login_load(&target, path, 1700000000, NULL));
    CHECK(youtube_login_save(&source, path, 1700000000)); /* Replaces link, never target. */
    CHECK(lstat(path, &st) == 0 && S_ISREG(st.st_mode));
    CHECK(symlink(victim, temporary) == 0);
    CHECK(youtube_login_save(&source, path, 1700000000));
    CHECK(youtube_login_remove(path) && access(path, F_OK) != 0 && access(temporary, F_OK) != 0);
    CHECK(youtube_login_load(&target, victim, 1700000000, NULL)); /* Link targets stayed intact. */
    browser_session_cookie_clear(&target);
    for (size_t i = 0; i < BROWSER_COOKIE_ENTRIES; i++)
        if (source.cookies[i].value) source.cookies[i].expires_at = 1700000010;
    CHECK(youtube_login_save(&source, path, 1700000000));
    CHECK(youtube_login_load(&target, path, 1700000020, NULL) && target.cookie_bytes == 0);
    browser_session_set_site_data_allowed(&source, false);
    CHECK(!youtube_login_save(&source, path, 1700000000));
    browser_session_set_site_data_allowed(&target, false);
    CHECK(!youtube_login_load(&target, path, 1700000000, NULL));
    browser_session_destroy(&source); browser_session_destroy(&target);

    BrowserProfile *profile = browser_profile_create(&budget); CHECK(profile);
    CHECK(browser_profile_youtube_login_policy(profile) == YOUTUBE_LOGIN_ASK);
    for (unsigned policy = 0; policy < 3u; policy++) {
        browser_profile_set_youtube_login_policy(profile, policy);
        CHECK(browser_profile_save(profile, profile_path));
        browser_profile_set_youtube_login_policy(profile, 0);
        CHECK(browser_profile_load(profile, profile_path));
        CHECK(browser_profile_youtube_login_policy(profile) == policy);
    }
    browser_profile_set_youtube_login_policy(profile, 999);
    CHECK(browser_profile_youtube_login_policy(profile) == YOUTUBE_LOGIN_NEVER);
    browser_profile_destroy(profile);
    PspUiState ui; psp_ui_init(&ui); ui.screen = PSP_UI_SCREEN_PAGE;
    static uint16_t pixels[480u * 272u];
    static const PspUiAction actions[] = {PSP_UI_ACTION_YOUTUBE_LOGIN_SAVE,
        PSP_UI_ACTION_YOUTUBE_LOGIN_ALWAYS, PSP_UI_ACTION_YOUTUBE_LOGIN_NOT_NOW, PSP_UI_ACTION_YOUTUBE_LOGIN_NEVER};
    for (unsigned choice = 0; choice < 4u; choice++) {
        psp_ui_show_youtube_login_offer(&ui);
        CHECK(ui.screen == PSP_UI_SCREEN_YOUTUBE_LOGIN && ui.menu_selection == 2u);
        for (unsigned tick = 0; tick < PSP_UI_STORAGE_OFFER_ARMING_FRAMES; tick++)
            CHECK(press(&ui, PSP_UI_BUTTON_CONFIRM).action == PSP_UI_ACTION_NONE);
        ui.menu_selection = (uint8_t) choice;
        psp_ui_composite(&ui, pixels, 480, 272, 480);
        CHECK(press(&ui, PSP_UI_BUTTON_CONFIRM).action == actions[choice]);
    }
    psp_ui_show_youtube_login_offer(&ui); ui.heavy_offer_arming = 0;
    CHECK(press(&ui, PSP_UI_BUTTON_CANCEL).action == PSP_UI_ACTION_YOUTUBE_LOGIN_NOT_NOW);
    psp_ui_show_youtube_login_offer(&ui); ui.heavy_offer_arming = 0;
    CHECK(press(&ui, PSP_UI_BUTTON_MENU).action == PSP_UI_ACTION_YOUTUBE_LOGIN_NOT_NOW);
    psp_ui_show_youtube_login_offer(&ui);
    psp_ui_dismiss_youtube_login_offer(&ui);
    CHECK(ui.screen == PSP_UI_SCREEN_PAGE);
    ui.screen = PSP_UI_SCREEN_MENU;
    psp_ui_dismiss_youtube_login_offer(&ui);
    CHECK(ui.screen == PSP_UI_SCREEN_MENU);
    CHECK(budget.current == 0);
    CHECK(youtube_login_remove(path)); (void) remove(victim); (void) remove(profile_path);
    char backup[260]; snprintf(backup, sizeof(backup), "%s.bak", profile_path); (void) remove(backup);
    (void) rmdir(directory);
    puts("YouTube login: consent, persistence, rollback, and teardown passed"); return 0;
}
