#include "tilefinch/browser_tabs.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CHECK(value) do { \
    if (!(value)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #value); \
        return 1; \
    } \
} while (0)

static bool commit_page(
    NavigationSession *navigation, unsigned index)
{
    static const char html[] =
        "<!doctype html><title>Page</title><p>tab history</p>";
    char url[64];
    snprintf(url, sizeof(url), "https://tabs.test/%u", index);
    uint64_t generation = navigation_begin(navigation);
    return navigation_commit_html(
        navigation, generation, url, html, sizeof(html) - 1u,
        480, NULL, NULL, true);
}

int main(void)
{
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    budget_install_lexbor(&budget);
    NavigationSession navigation;
    CHECK(navigation_init(&navigation, &budget, 8));
    for (unsigned index = 0; index < 6; index++)
        CHECK(commit_page(&navigation, index));
    CHECK(navigation.history_count == 6
          && navigation.history_index == 5);
    navigation.history[5].scroll_y = 73;
    navigation.history[5].focus_kind = 2;
    navigation.history[5].focus_index = 4;

    BrowserTabs *tabs = browser_tabs_create(&budget, &navigation);
    CHECK(tabs != NULL
          && browser_tabs_count(tabs) == 1
          && browser_tabs_active_index(tabs) == 0
          && browser_tabs_owned_bytes() <= 48u * 1024u);
    const BrowserTabSnapshot *first = browser_tabs_active(tabs);
    CHECK(first != NULL
          && first->history_count == BROWSER_TAB_HISTORY_LIMIT
          && first->history_index == 3
          && strcmp(first->history[0].url, "https://tabs.test/2") == 0
          && strcmp(first->history[3].url, "https://tabs.test/5") == 0
          && first->history[3].scroll_y == 73
          && first->history[3].focus_kind == 2
          && first->history[3].focus_index == 4);
    CHECK(strcmp(browser_tabs_active_find_query(tabs), "") == 0
          && browser_tabs_set_active_find_query(tabs, "portable console")
          && strcmp(browser_tabs_active_find_query(tabs),
                    "portable console") == 0);

    size_t created = 0;
    CHECK(browser_tabs_add(
              tabs, "https://tilefinch.local/home", "New Tab", &created)
          && created == 1
          && browser_tabs_active_index(tabs) == 1
          && strcmp(browser_tabs_active_find_query(tabs), "") == 0);
    CHECK(browser_tabs_add(
              tabs, "https://example.test/", "Example", &created)
          && created == 2
          && browser_tabs_add(
                 tabs, "https://third.test/", "Third", &created)
          && created == 3
          && browser_tabs_add(
                 tabs, "https://fourth.test/", "Fourth", &created)
          && created == 4
          && !browser_tabs_add(
                 tabs, "https://overflow.test/", "Too many", NULL));
    CHECK(browser_tabs_select(tabs, 0)
          && browser_tabs_remove(tabs, 1)
          && browser_tabs_count(tabs) == 4
          && browser_tabs_active_index(tabs) == 0);
    char hibernation_path[160];
    snprintf(
        hibernation_path, sizeof(hibernation_path),
        "/tmp/tilefinch-tab-%ld.bin", (long) getpid());
    char session_path[160];
    snprintf(
        session_path, sizeof(session_path),
        "/tmp/tilefinch-tabs-session-%ld.bin", (long) getpid());
    size_t resident_before = browser_tabs_resident_bytes(tabs);
    CHECK(!browser_tabs_hibernate(
              tabs, 1, "/missing/tilefinch/tab.bin")
          && browser_tabs_tab(tabs, 1) != NULL
          && browser_tabs_hibernate(tabs, 1, hibernation_path)
          && browser_tabs_hibernated(tabs, 1)
          && browser_tabs_tab(tabs, 1) == NULL
          && strcmp(browser_tabs_title(tabs, 1), "Example") == 0
          && browser_tabs_resident_bytes(tabs)
                 + sizeof(BrowserTabSnapshot) == resident_before
          && !browser_tabs_select(tabs, 1)
          && !browser_tabs_hibernate(tabs, 2, hibernation_path));
    CHECK(browser_tabs_save_session(
        tabs, session_path, hibernation_path));
    BrowserTabs *restored = browser_tabs_create(&budget, &navigation);
    CHECK(restored != NULL
          && browser_tabs_restore_session(restored, session_path)
          && browser_tabs_count(restored) == 4
          && browser_tabs_active_index(restored) == 0
          && strcmp(browser_tabs_active_find_query(restored),
                    "portable console") == 0
          && !browser_tabs_hibernated(restored, 1)
          && strcmp(browser_tabs_tab(restored, 1)->history[0].url,
                    "https://example.test/") == 0);
    browser_tabs_destroy(restored);
    budget_inject_failure_after(&budget, 0);
    CHECK(!browser_tabs_rehydrate(tabs, 1, hibernation_path)
          && browser_tabs_hibernated(tabs, 1));
    budget_clear_failure_injection(&budget);
    CHECK(browser_tabs_rehydrate(tabs, 1, hibernation_path)
          && !browser_tabs_hibernated(tabs, 1)
          && browser_tabs_tab(tabs, 1) != NULL
          && strcmp(browser_tabs_tab(tabs, 1)->history[0].url,
                    "https://example.test/") == 0
          && browser_tabs_resident_bytes(tabs) == resident_before);
    CHECK(browser_tabs_hibernate(tabs, 1, hibernation_path));
    FILE *corrupt = fopen(hibernation_path, "r+b");
    CHECK(corrupt != NULL && fseek(corrupt, -1, SEEK_END) == 0);
    int last_byte = fgetc(corrupt);
    CHECK(last_byte != EOF && fseek(corrupt, -1, SEEK_END) == 0
          && fputc(last_byte ^ 0x5a, corrupt) != EOF
          && fclose(corrupt) == 0
          && !browser_tabs_rehydrate(tabs, 1, hibernation_path)
          && browser_tabs_hibernated(tabs, 1));
    CHECK(browser_tabs_remove(tabs, 1)
          && browser_tabs_count(tabs) == 3
          && browser_tabs_active_index(tabs) == 0);
    for (size_t index = 0; index < browser_tabs_count(tabs); index++)
        CHECK(!browser_tabs_hibernated(tabs, index));
    char long_title[400];
    memset(long_title, 'T', sizeof(long_title));
    long_title[sizeof(long_title) - 1u] = '\0';
    CHECK(browser_tabs_update_active_document(
              tabs, "https://tabs.test/5", long_title)
          && browser_tabs_active(tabs)
                 ->history[browser_tabs_active(tabs)->history_index]
                 .title[NAVIGATION_TITLE_LIMIT - 1u] == '\0');

    NavigationHistoryRecord records[BROWSER_TAB_HISTORY_LIMIT];
    size_t count = 0;
    size_t current = 0;
    CHECK(browser_tab_history_records(
              browser_tabs_active(tabs), records, &count, &current)
          && count == 4 && current == 3);
    const NavigationEntry *before = navigation_current(&navigation);
    CHECK(before != NULL);
    char before_url[NAVIGATION_URL_LIMIT];
    snprintf(before_url, sizeof(before_url), "%s", before->url);
    budget_inject_failure_after(&budget, 0);
    CHECK(!navigation_replace_history(
        &navigation, records, count, current));
    budget_clear_failure_injection(&budget);
    CHECK(navigation.history_count == 6
          && strcmp(navigation_current(&navigation)->url, before_url) == 0);
    char oversized_url[NAVIGATION_URL_LIMIT + 1u];
    char oversized_title[NAVIGATION_TITLE_LIMIT + 1u];
    memset(oversized_url, 'u', sizeof(oversized_url));
    memset(oversized_title, 't', sizeof(oversized_title));
    oversized_url[sizeof(oversized_url) - 1u] = '\0';
    oversized_title[sizeof(oversized_title) - 1u] = '\0';
    NavigationHistoryRecord oversized = {
        .url = oversized_url,
        .title = "title"
    };
    CHECK(!navigation_replace_history(&navigation, &oversized, 1, 0)
          && navigation.history_count == 6
          && strcmp(navigation_current(&navigation)->url, before_url) == 0);
    oversized.url = "https://tabs.test/oversized-title";
    oversized.title = oversized_title;
    CHECK(!navigation_replace_history(&navigation, &oversized, 1, 0)
          && navigation.history_count == 6
          && strcmp(navigation_current(&navigation)->url, before_url) == 0);
    CHECK(navigation_replace_history(
              &navigation, records, count, current)
          && navigation.history_count == 4
          && navigation.history_index == 3
          && strcmp(
                 navigation_current(&navigation)->url,
                 "https://tabs.test/5") == 0);

    browser_tabs_destroy(tabs);
    unlink(hibernation_path);
    unlink(session_path);
    navigation_destroy(&navigation);
    CHECK(budget_uninstall_lexbor(&budget));
    CHECK(budget.current == 0
          && budget_active_allocations(&budget, NULL) == 0);
    puts("browser-tabs-tests: ok");
    return 0;
}
