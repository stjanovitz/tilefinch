#include "tilefinch/content_blocker.h"
#include "tilefinch/fetch.h"
#include "tilefinch/document.h"
#include "tilefinch/layout.h"
#include "tilefinch/style.h"
#include "../src/style_internal.h"

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

static int test_prefix_scan_boundaries(void)
{
    static const char *const short_prefixes[] = {"q", "ab", "ad-slot-", "ad-banner-"};
    /* Six union branches must also fit the stylesheet's selector ceiling. */
    char long_prefix[17];
    memset(long_prefix, 'a', sizeof(long_prefix) - 1u);
    long_prefix[sizeof(long_prefix) - 1u] = '\0';
    for (size_t p = 0; p <= sizeof(short_prefixes) / sizeof(short_prefixes[0]); p++) {
        const char *prefix = p == sizeof(short_prefixes) / sizeof(short_prefixes[0])
            ? long_prefix : short_prefixes[p];
        size_t length = strlen(prefix);
        char css[2048];
        int written = snprintf(css, sizeof(css),
            ":is([class^=\"%s\"],[class*=\" %s\"],[class*=\"\\9 %s\"],"
            "[class*=\"\\a %s\"],[class*=\"\\c %s\"],[class*=\"\\d %s\"])"
            "{display:none}", prefix, prefix, prefix, prefix, prefix, prefix);
        CHECK(written > 0 && (size_t) written < sizeof(css));
        Budget budget;
        budget_init(&budget, 4u * 1024u * 1024u);
        CHECK(budget_install_lexbor(&budget));
        PocDocument document = {0};
        Stylesheet sheet = {0};
        static const char html[] = "<!doctype html><body><div></div></body>";
        CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 64u)
              && stylesheet_build(&sheet, &budget, &document, 480)
              && stylesheet_add_user_css(&sheet, css, (size_t) written));
        lxb_dom_node_t *node = document_body_node(&document)->first_child;
        CHECK(node != NULL);
        CHECK(sheet.count == 1u);
        (void) style_for_node(&sheet, node, NULL);
        CHECK(sheet.selector_program_ready
              && sheet.selector_program[0].opcode == STYLE_SELECTOR_CLASS_TOKEN_PREFIX);
        /* Borrowed spans deliberately vary without mutating the DOM. Disable
           resolution caches for this direct matcher test, then restore them
           before teardown. This tests bytes, not stale node-cache answers. */
        StyleResolveScratch *scratch = sheet.resolve_scratch;
        sheet.resolve_scratch = NULL;
        size_t owned_before_matching = budget.current;
        StyleMatchSubject subject;
        style_match_subject_prepare(node, &subject);
        unsigned char storage[512];
        for (size_t alignment = 0; alignment < 16u; alignment++) {
            char *classes = (char *) storage + alignment;
            subject.classes = classes;
            memcpy(classes, prefix, length);
            subject.classes_length = length;
            CHECK(style_rule_selector_matches_subject(&sheet, 0u, node, &subject));
            subject.classes_length = length - 1u;
            CHECK(!style_rule_selector_matches_subject(&sheet, 0u, node, &subject));
            for (unsigned separator = 0; separator <= 255u; separator++) {
                memset(classes, 'z', 65u);
                classes[64] = (char) separator;
                memcpy(classes + 65u, prefix, length);
                subject.classes_length = 65u + length;
                bool boundary = separator == ' ' || separator == '\t'
                    || separator == '\n' || separator == '\f' || separator == '\r';
                CHECK(style_rule_selector_matches_subject(&sheet, 0u, node, &subject)
                      == boundary);
                /* Valid prefix bytes beyond the supplied span must not count. */
                subject.classes_length--;
                CHECK(!style_rule_selector_matches_subject(&sheet, 0u, node, &subject));
            }
        }
        CHECK(budget.current == owned_before_matching);
        sheet.resolve_scratch = scratch;
        stylesheet_destroy(&sheet);
        document_destroy(&document);
        CHECK(budget.current == 0u && budget_uninstall_lexbor(&budget));
    }
    return 0;
}

static int test_cosmetic_class_families(void)
{
    static const struct { const char *classes; bool hidden; } cases[] = {
        {"ad-slot-dynamic", true}, {"ad-slot-inview ad-wrapper", true},
        {"ad-banner-wrapper", true}, {"ad-wrapper", true},
        {"card ad-slot-mobile-incontent selected", true},
        {"card\tad-slot-header", true}, {"card\nad-slot-native", true},
        {"card\fad-slot-placeholder", true}, {"card\rad-slot-container", true},
        {"card&#9;ad-slot-dynamic", true},
        {"\tad-banner-wrapper card", true}, {"card\nad-banner-top", true},
        {"card\rad-banner-top", true}, {"card\fad-banner-top", true},
        {"card\tad-banner-top", true}, {"card ad-banner-top", true},
        {"ad-slot", true}, {"ad-banner", true},
        {"card-ad-slot-dynamic", false}, {"myad-banner-wrapper", false},
        {"ad-slotting", false}, {"ad-bannerish", false},
        {"ad-feedback__form", false}, {"ad-settings", false},
        {"ad-wrapper-content", false}, {"advertisement-settings", false},
        {"card&#11;ad-slot-header", false},
        {"AD-SLOT-header", false}, {"card xad-slot-header", false},
        {"ordinary", false}, {"", false}
    };
    char css[CONTENT_BLOCKER_COSMETIC_CSS_LIMIT];
    size_t css_length = 0;
    CHECK(content_blocker_cosmetic_css(css, sizeof(css), &css_length));
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char html[512];
        int written = snprintf(html, sizeof(html),
            "<!doctype html><style>body{margin:0}</style><body>"
            "<div class=\"%s\" style=\"display:block!important;"
            "height:250px;margin:35px\"></div>"
            "<div style=\"height:10px\"></div></body>", cases[i].classes);
        CHECK(written > 0 && (size_t) written < sizeof(html));
        Budget budget;
        budget_init(&budget, 4u * 1024u * 1024u);
        CHECK(budget_install_lexbor(&budget));
        PocDocument document = {0};
        Stylesheet sheet = {0};
        LayoutDocument layout = {0};
        CHECK(document_parse(&document, &budget, html, (size_t) written, 64u)
              && stylesheet_build(&sheet, &budget, &document, 480)
              && stylesheet_add_user_css(&sheet, css, css_length));
        lxb_dom_node_t *node = document_body_node(&document)->first_child;
        CHECK(node != NULL && node->next != NULL);
        ComputedStyle style = style_for_node(&sheet, node, NULL);
        CHECK(sheet.selector_program_ready
              && sheet.selector_program_rule_count == sheet.count);
        bool string_hidden = false;
        for (size_t rule = 0; rule < sheet.count; rule++) {
            /* Every rule except body's margin reset has the same hide
               declaration. Compare the optimized union with its ordinary
               CSS string matcher, including all whitespace boundaries. */
            if (style_selector_matches(node, sheet.rules[rule].selector,
                                       sheet.rules[rule].selector_length))
                string_hidden = true;
        }
        CHECK(string_hidden == cases[i].hidden);
        if ((style.display == DISPLAY_NONE) != cases[i].hidden) {
            fprintf(stderr, "unexpected cosmetic match: %s\n", cases[i].classes);
            return 1;
        }
        CHECK(layout_build(&layout, &budget, &document, &sheet,
                           NULL, NULL, 480));
        const LayoutNodeBox *following = layout_box_for_node(&layout, node->next);
        CHECK(following != NULL
              && (cases[i].hidden ? following->y == 0 : following->y >= 250));
        layout_destroy(&layout);
        stylesheet_destroy(&sheet);
        document_destroy(&document);
        CHECK(budget.current == 0u && budget_uninstall_lexbor(&budget));
    }
    return 0;
}

int main(void)
{
    CHECK(test_prefix_scan_boundaries() == 0);
    CHECK(test_cosmetic_class_families() == 0);
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
          && strstr(cookie_css, "dialog.cookie-policy") != NULL
          && strstr(cookie_css, "[data-cookie-policy]") != NULL
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

    /* $media applies to media fetches, and only to them. */
    CHECK(content_blocker_would_block(
        blocker, "https://media.example/clip.mp4",
        "https://publisher.example/", "video", "no-cors"));
    CHECK(!content_blocker_would_block(
        blocker, "https://media.example/poster.jpg",
        "https://publisher.example/", "image", "no-cors"));

    ContentBlockerMetrics metrics;
    CHECK(content_blocker_metrics(blocker, &metrics)
          && metrics.mode == CONTENT_BLOCKER_CUSTOM
          && metrics.rule_count == 5
          && metrics.allow_rule_count == 1
          && metrics.ignored_rule_count == 3
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
          && metrics.rule_count == 5);
    budget_inject_failure_after(&budget, 0);
    CHECK(!content_blocker_configure(
        blocker, CONTENT_BLOCKER_CUSTOM, custom_path));
    budget_clear_failure_injection(&budget);
    CHECK(content_blocker_metrics(blocker, &metrics)
          && metrics.mode == CONTENT_BLOCKER_CUSTOM
          && metrics.rule_count == 5);
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
