#include "tilefinch/youtube_login.h"
#include "tilefinch/browser_profile.h"
#include "tilefinch/psp_ui.h"
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
    BrowserSession source, target;
    CHECK(browser_session_init(&source, &budget, 1));
    CHECK(browser_session_init(&target, &budget, 1));
    CHECK(!youtube_login_load(&target, path, 1700000000));
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
    CHECK(youtube_login_load(&target, path, 1700000000));
    char header[1024];
    CHECK(browser_session_cookie_header(&target, "https://m.youtube.com/watch", header, sizeof(header)));
    CHECK(strstr(header, "SAPISID=synthetic") && strstr(header, "local=host-only") && !strstr(header, "unsafe"));
    CHECK(browser_session_cookie_header(&target, "https://accounts.google.com/", header, sizeof(header)) && header[0] == '\0');
    CHECK(!youtube_login_load(&target, path, 1700000000)); /* Never clobber a live jar. */
    browser_session_cookie_clear(&target);
    /* A second save must install the new cookie on FAT, not retain the old one. */
    CHECK(browser_session_cookie_set_http(&source, "https://youtube.com/",
        "SAPISID=synthetic-updated; Secure; HttpOnly; SameSite=Lax; Path=/; Domain=youtube.com"));
    tilefinch_test_faults()->youtube_login_no_replace_rename = true;
    CHECK(youtube_login_save(&source, path, 1700000000));
    tilefinch_test_faults()->youtube_login_no_replace_rename = false;
    CHECK(youtube_login_load(&target, path, 1700000000));
    CHECK(browser_session_cookie_header(&target, "https://m.youtube.com/watch", header, sizeof(header))
        && strstr(header, "SAPISID=synthetic-updated"));
    browser_session_cookie_clear(&target);
    CHECK(browser_session_cookie_set_http(&source, "https://youtube.com/",
        "SAPISID=synthetic; Secure; HttpOnly; SameSite=Lax; Path=/; Domain=youtube.com"));
    CHECK(youtube_login_save(&source, path, 1700000000));
    size_t limit = budget.limit; budget.limit = budget.current;
    CHECK(!youtube_login_load(&target, path, 1700000000) && target.cookie_bytes == 0);
    CHECK(!youtube_login_save(&source, path, 1700000000)); budget.limit = limit;
    CHECK(youtube_login_load(&target, path, 1700000000));
    browser_session_cookie_clear(&target);
    FILE *file = fopen(path, "r+b"); CHECK(file && fseek(file, -1, SEEK_END) == 0);
    CHECK(fputc('!', file) != EOF && fclose(file) == 0);
    CHECK(!youtube_login_load(&target, path, 1700000000) && target.cookie_bytes == 0);
    CHECK(youtube_login_save(&source, path, 1700000000));
    CHECK(rename(path, victim) == 0 && symlink(victim, path) == 0);
    CHECK(!youtube_login_load(&target, path, 1700000000));
    CHECK(youtube_login_save(&source, path, 1700000000)); /* Replaces link, never target. */
    CHECK(lstat(path, &st) == 0 && S_ISREG(st.st_mode));
    CHECK(symlink(victim, temporary) == 0);
    CHECK(youtube_login_save(&source, path, 1700000000));
    CHECK(youtube_login_remove(path) && access(path, F_OK) != 0 && access(temporary, F_OK) != 0);
    CHECK(youtube_login_load(&target, victim, 1700000000)); /* Link targets stayed intact. */
    browser_session_cookie_clear(&target);
    for (size_t i = 0; i < BROWSER_COOKIE_ENTRIES; i++)
        if (source.cookies[i].value) source.cookies[i].expires_at = 1700000010;
    CHECK(youtube_login_save(&source, path, 1700000000));
    CHECK(youtube_login_load(&target, path, 1700000020) && target.cookie_bytes == 0);
    browser_session_set_site_data_allowed(&source, false);
    CHECK(!youtube_login_save(&source, path, 1700000000));
    browser_session_set_site_data_allowed(&target, false);
    CHECK(!youtube_login_load(&target, path, 1700000000));
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
    CHECK(budget.current == 0);
    CHECK(youtube_login_remove(path)); (void) remove(victim); (void) remove(profile_path);
    char backup[260]; snprintf(backup, sizeof(backup), "%s.bak", profile_path); (void) remove(backup);
    (void) rmdir(directory);
    puts("YouTube login: consent, persistence, rollback, and teardown passed"); return 0;
}
