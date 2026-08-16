#include "tilefinch/content_blocker.h"
#include "tilefinch/fetch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(value) do { \
    if (!(value)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #value); \
        return 1; \
    } \
} while (0)

static bool write_custom_list(char path[128])
{
    snprintf(path, 128, "/tmp/tilefinch-adblock-XXXXXX");
    int descriptor = mkstemp(path);
    if (descriptor < 0) return false;
    FILE *file = fdopen(descriptor, "wb");
    if (file == NULL) {
        close(descriptor);
        unlink(path);
        return false;
    }
    static const char list[] =
        "! Tilefinch parser regression\n"
        "||ads.example^$third-party,script\n"
        "@@||allow.ads.example^$script\n"
        "||images.example^$image\n"
        "0.0.0.0 tracker.example\n"
        "example.com##.advert\n"
        "||scoped.example^$domain=publisher.example\n"
        "||media.example^$media\n"
        "||socket.example^$websocket\n";
    bool ok = fwrite(list, 1, sizeof(list) - 1u, file) == sizeof(list) - 1u;
    ok = fclose(file) == 0 && ok;
    if (!ok) unlink(path);
    return ok;
}

int main(void)
{
    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    ContentBlocker *blocker = content_blocker_create(&budget);
    CHECK(blocker != NULL);
    char cosmetic[CONTENT_BLOCKER_COSMETIC_CSS_LIMIT];
    size_t cosmetic_length = 0;
    CHECK(content_blocker_cosmetic_css(
              cosmetic, sizeof(cosmetic), &cosmetic_length)
          && cosmetic_length == strlen(cosmetic)
          && strstr(cosmetic, "ins.adsbygoogle") != NULL
          && strstr(cosmetic, "[data-ad-slot]") != NULL
          && strstr(cosmetic, "display:none!important") != NULL);
    CHECK(!content_blocker_cosmetic_css(
        cosmetic, 8, &cosmetic_length));
    char cookie_css[CONTENT_BLOCKER_COOKIE_CSS_LIMIT];
    size_t cookie_length = 0;
    CHECK(content_blocker_cookie_banner_css(
              cookie_css, sizeof(cookie_css), &cookie_length)
          && cookie_length == strlen(cookie_css)
          && strstr(cookie_css, "#onetrust-banner-sdk") != NULL
          && strstr(cookie_css, "sp_message_container_") != NULL
          && strstr(cookie_css, ":is([data-testid=") != NULL
          && strstr(cookie_css, "overflow:auto!important") != NULL
          && strstr(cookie_css, "accept") == NULL);
    CHECK(!content_blocker_cookie_banner_css(
        cookie_css, 8, &cookie_length));
    CHECK(content_blocker_configure(blocker, CONTENT_BLOCKER_BASIC, NULL));
    CHECK(content_blocker_should_block(
        blocker, "https://pagead2.googlesyndication.com/ad.js",
        "https://news.example/article", "script", "no-cors"));
    CHECK(!content_blocker_should_block(
        blocker, "https://doubleclick.net/", "https://doubleclick.net/",
        "document", "navigate"));
    CHECK(!content_blocker_should_block(
        blocker, "https://cdn.news.example/app.js",
        "https://news.example/article", "script", "no-cors"));

    const char *allowed[] = {"news.example"};
    CHECK(content_blocker_set_allowed_sites(blocker, allowed, 1));
    CHECK(!content_blocker_should_block(
        blocker, "https://doubleclick.net/ad.js",
        "https://news.example/article", "script", "no-cors"));
    CHECK(content_blocker_set_allowed_sites(blocker, NULL, 0));

    char custom_path[128];
    CHECK(write_custom_list(custom_path));
    CHECK(content_blocker_configure(
        blocker, CONTENT_BLOCKER_CUSTOM, custom_path));
    CHECK(content_blocker_should_block(
        blocker, "https://ads.example/banner.js",
        "https://publisher.example/", "script", "no-cors"));
    CHECK(!content_blocker_should_block(
        blocker, "https://ads.example/banner.png",
        "https://publisher.example/", "image", "no-cors"));
    CHECK(!content_blocker_should_block(
        blocker, "https://ads.example/first.js",
        "https://ads.example/", "script", "no-cors"));
    CHECK(!content_blocker_should_block(
        blocker, "https://allow.ads.example/banner.js",
        "https://publisher.example/", "script", "no-cors"));
    CHECK(content_blocker_should_block(
        blocker, "https://images.example/banner.png",
        "https://publisher.example/", "image", "no-cors"));
    CHECK(!content_blocker_should_block(
        blocker, "https://images.example/app.js",
        "https://publisher.example/", "script", "no-cors"));
    CHECK(content_blocker_should_block(
        blocker, "https://sub.tracker.example/pixel",
        "https://publisher.example/", "image", "no-cors"));
    CHECK(!content_blocker_should_block(
        blocker, "https://scoped.example/ad.js",
        "https://publisher.example/", "script", "no-cors"));

    ContentBlockerMetrics metrics;
    CHECK(content_blocker_metrics(blocker, &metrics)
          && metrics.mode == CONTENT_BLOCKER_CUSTOM
          && metrics.rule_count == 4
          && metrics.allow_rule_count == 1
          && metrics.ignored_rule_count == 4
          && metrics.requests_blocked == 3
          && metrics.retained_bytes < 192u * 1024u
          && !metrics.truncated);

    FetchRequest blocked_request = {
        .method = "GET",
        .accept = "application/javascript",
        .initiator_url = "https://publisher.example/",
        .sec_fetch_dest = "script",
        .sec_fetch_mode = "no-cors",
        .content_blocker = blocker
    };
    FetchResult blocked_result = {0};
    CHECK(!fetch_request_cancelable(
              &budget, "https://ads.example/network.js",
              &blocked_request, 4096u, 50, NULL, NULL, &blocked_result)
          && strcmp(blocked_result.error, "blocked by content blocker") == 0
          && blocked_result.data == NULL);
    fetch_result_destroy(&blocked_result);
    FetchScheduler *scheduler = fetch_scheduler_create(
        &budget, 1u, 4096u);
    CHECK(scheduler != NULL);
    CHECK(fetch_scheduler_enqueue(
              scheduler, "https://ads.example/scheduled.js",
              &blocked_request, 4096u, 50) == 0);
    fetch_scheduler_destroy(scheduler);
    CHECK(!content_blocker_configure(
        blocker, CONTENT_BLOCKER_CUSTOM, "/tmp/missing-tilefinch-list"));
    CHECK(content_blocker_metrics(blocker, &metrics)
          && metrics.mode == CONTENT_BLOCKER_CUSTOM
          && metrics.rule_count == 4);
    budget_inject_failure_after(&budget, 0);
    CHECK(!content_blocker_configure(
        blocker, CONTENT_BLOCKER_CUSTOM, custom_path));
    budget_clear_failure_injection(&budget);
    CHECK(content_blocker_metrics(blocker, &metrics)
          && metrics.mode == CONTENT_BLOCKER_CUSTOM
          && metrics.rule_count == 4);
    CHECK(content_blocker_configure(blocker, CONTENT_BLOCKER_OFF, NULL));
    CHECK(!content_blocker_should_block(
        blocker, "https://ads.example/banner.js",
        "https://publisher.example/", "script", "no-cors"));

    char site[CONTENT_BLOCKER_HOST_LIMIT];
    CHECK(content_blocker_site_from_url(
              "https://en.wikipedia.org/wiki/PSP", site)
          && strcmp(site, "wikipedia.org") == 0);
    CHECK(!content_blocker_site_from_url("not a URL", site));

    unlink(custom_path);
    content_blocker_destroy(blocker);
    CHECK(budget.current == 0 && budget_categories_reconcile(&budget));
    puts("content-blocker-tests: ok");
    return 0;
}
