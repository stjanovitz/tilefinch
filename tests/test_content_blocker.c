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
    static const struct { const char *attribute; bool hidden; } slots[] = {
        {"data-csa-c-slot-id=\"adplacements:detail:mobile\"", true},
        {"data-csa-c-slot-id=\"content:detail:mobile\"", false},
        {"data-csa-c-slot-id=\"not-adplacements:detail\"", false}
    };
    size_t class_count = sizeof(cases) / sizeof(cases[0]);
    for (size_t i = 0; i < class_count + sizeof(slots) / sizeof(slots[0]); i++) {
        const char *classes = i < class_count ? cases[i].classes : "";
        const char *attribute = i < class_count ? "" : slots[i - class_count].attribute;
        bool hidden = i < class_count ? cases[i].hidden : slots[i - class_count].hidden;
        char html[512];
        int written = snprintf(html, sizeof(html),
            "<!doctype html><style>body{margin:0}</style><body>"
            "<div class=\"%s\" %s style=\"display:block!important;"
            "height:250px;margin:35px\"></div>"
            "<div style=\"height:10px\"></div></body>", classes, attribute);
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
        CHECK(string_hidden == hidden);
        if ((style.display == DISPLAY_NONE) != hidden) {
            fprintf(stderr, "unexpected cosmetic match: %s %s\n", classes, attribute);
            return 1;
        }
        CHECK(layout_build(&layout, &budget, &document, &sheet,
                           NULL, NULL, 480));
        const LayoutNodeBox *following = layout_box_for_node(&layout, node->next);
        CHECK(following != NULL
              && (hidden ? following->y == 0 : following->y >= 250));
        layout_destroy(&layout);
        stylesheet_destroy(&sheet);
        document_destroy(&document);
        CHECK(budget.current == 0u && budget_uninstall_lexbor(&budget));
    }
    return 0;
}

/* The embed hop's player page: its own third-party requests also refuse
   the generic player-page list (analytics, consent, captcha SDKs...), on
   top of BASIC; no other page does, OFF blocks nothing, and the per-site
   allowlist still exempts the page. */
static int test_player_page_list(void)
{
    Budget budget;
    budget_init(&budget, 1024u * 1024u);
    ContentBlocker *blocker = content_blocker_create(&budget);
    CHECK(blocker != NULL
          && content_blocker_configure(blocker, CONTENT_BLOCKER_BASIC, NULL));
    const char *player = "https://player.example.test/embed/42";
    const char *analytics = "https://www.google-analytics.com/analytics.js";
    const char *consent = "https://cdn.cookielaw.org/consent.js";
    CHECK(!content_blocker_would_block(blocker, analytics, player, "script",
                                       "no-cors")
          && !content_blocker_player_page(blocker, player));
    content_blocker_set_player_page(blocker, player);
    CHECK(content_blocker_player_page(blocker, player)
          && content_blocker_player_page(
                 blocker, "https://www.example.test/other")
          && !content_blocker_player_page(blocker, "https://news.test/"));
    CHECK(content_blocker_would_block(blocker, analytics, player, "script",
                                      "no-cors")
          && content_blocker_would_block(blocker, consent, player, "", "cors")
          /* Ads stay blocked as BASIC blocks them. */
          && content_blocker_would_block(
                 blocker, "https://securepubads.doubleclick.net/x.js",
                 player, "script", "no-cors")
          /* First-party and the player's own CDN are untouched. */
          && !content_blocker_would_block(
                 blocker, "https://api.example.test/v1/play/42", player, "",
                 "cors")
          && !content_blocker_would_block(
                 blocker, "https://cdn.video.test/live/42/index.m3u8", player,
                 "", "cors")
          /* Another page keeps BASIC only. */
          && !content_blocker_would_block(blocker, analytics,
                                          "https://news.test/story", "script",
                                          "no-cors"));
    const char *const allowed[] = {"example.test"};
    CHECK(content_blocker_set_allowed_sites(blocker, allowed, 1u)
          && !content_blocker_would_block(blocker, analytics, player,
                                          "script", "no-cors")
          && content_blocker_set_allowed_sites(blocker, NULL, 0u));
    CHECK(content_blocker_configure(blocker, CONTENT_BLOCKER_OFF, NULL)
          && !content_blocker_would_block(blocker, analytics, player,
                                          "script", "no-cors")
          && content_blocker_configure(blocker, CONTENT_BLOCKER_BASIC, NULL));
    content_blocker_set_player_page(blocker, NULL);
    CHECK(!content_blocker_would_block(blocker, analytics, player, "script",
                                       "no-cors")
          && !content_blocker_player_page(blocker, player));
    content_blocker_destroy(blocker);
    CHECK(budget.current == 0u);
    return 0;
}

/* IS-19: one over-long line, or more rules than the matcher keeps, no
   longer rejects the whole custom list. */
static int test_long_lines_and_rule_limit(void)
{
    char path[128];
    snprintf(path, sizeof(path), "/tmp/tilefinch-adblock-long-XXXXXX");
    int descriptor = mkstemp(path);
    CHECK(descriptor >= 0);
    FILE *file = fdopen(descriptor, "wb");
    CHECK(file != NULL);
    bool ok = fputs("||first.example^\n", file) >= 0;
    /* A cosmetic rule of 5000 bytes, then one of exactly 2047 bytes plus
       its newline, then an unterminated short last line further down. */
    ok = ok && fputs("example.com##", file) >= 0;
    for (unsigned at = 0; ok && at < 5000u; at++) ok = fputc('a', file) != EOF;
    ok = ok && fputc('\n', file) != EOF;
    ok = ok && fputs("example.org##", file) >= 0;
    for (unsigned at = 13u; ok && at < 2047u; at++) ok = fputc('b', file) != EOF;
    ok = ok && fputs("\n||second.example^\n", file) >= 0;
    for (unsigned at = 0; ok && at < CONTENT_BLOCKER_RULE_LIMIT + 10u; at++)
        ok = fprintf(file, "||h%u.example^\n", at) > 0;
    ok = ok && fputs("||last.example^", file) >= 0;
    CHECK(fclose(file) == 0 && ok);

    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    ContentBlocker *blocker = content_blocker_create(&budget);
    CHECK(blocker != NULL
          && content_blocker_configure(blocker, CONTENT_BLOCKER_CUSTOM, path));
    ContentBlockerMetrics metrics;
    CHECK(content_blocker_metrics(blocker, &metrics)
          && metrics.rule_count == CONTENT_BLOCKER_RULE_LIMIT
          && metrics.truncated
          /* two long lines, 13 rules past the limit */
          && metrics.ignored_rule_count == 2u + 13u);
    CHECK(content_blocker_should_block(
              blocker, "https://first.example/a.js",
              "https://publisher.example/", "script", "no-cors")
          && content_blocker_should_block(
              blocker, "https://second.example/a.js",
              "https://publisher.example/", "script", "no-cors")
          && !content_blocker_should_block(
              blocker, "https://last.example/a.js",
              "https://publisher.example/", "script", "no-cors"));
    content_blocker_destroy(blocker);
    CHECK(unlink(path) == 0 && budget.current == 0);
    return 0;
}

/* Basic also refuses the ad-verification and ad-recovery hosts a page
   loads beside its (already blocked) auctions: nintendolife spent about
   1 MiB of script memory on them. Third-party requests only, subdomains
   included; the sites themselves, Off and a site exception are untouched. */
static int test_basic_ad_verification_hosts(void)
{
    Budget budget;
    budget_init(&budget, 1024u * 1024u);
    ContentBlocker *blocker = content_blocker_create(&budget);
    CHECK(blocker != NULL
          && content_blocker_configure(blocker, CONTENT_BLOCKER_BASIC, NULL));
    const char *page = "https://www.nintendolife.com/news";
    const char *const verification[] = {
        "https://cdn.confiant-integrations.net/x/gpt_and_prebid/config.js",
        "https://confiant-integrations.net/wrap.js",
        "https://nintendolife-com.videoplayerhub.btloader.com/tag.js",
        "https://btloader.com/tag?o=1&upapi=true",
    };
    for (size_t i = 0; i < sizeof(verification) / sizeof(verification[0]);
         i++) {
        CHECK(content_blocker_would_block(blocker, verification[i], page,
                                          "script", "no-cors"));
    }
    /* Their own pages, and look-alike hosts, are not refused. */
    CHECK(!content_blocker_would_block(
              blocker, "https://www.btloader.com/app.js",
              "https://www.btloader.com/", "script", "no-cors")
          && !content_blocker_would_block(
                 blocker, "https://notbtloader.com/tag.js", page, "script",
                 "no-cors")
          && !content_blocker_would_block(
                 blocker, "https://confiant-integrations.net.example/a.js",
                 page, "script", "no-cors"));
    const char *const allowed[] = {"nintendolife.com"};
    CHECK(content_blocker_set_allowed_sites(blocker, allowed, 1u)
          && !content_blocker_would_block(blocker, verification[1], page,
                                          "script", "no-cors")
          && content_blocker_set_allowed_sites(blocker, NULL, 0u));
    CHECK(content_blocker_configure(blocker, CONTENT_BLOCKER_OFF, NULL)
          && !content_blocker_would_block(blocker, verification[2], page,
                                          "script", "no-cors"));
    content_blocker_destroy(blocker);
    CHECK(budget.current == 0u);
    return 0;
}

/* Basic refuses third-party tag managers and A/B testing clients, leaves
   paywall and subscription SDKs alone, and says which refused scripts a
   page may be waiting on. */
static int test_basic_sdk_hosts(void)
{
    Budget budget;
    budget_init(&budget, 1024u * 1024u);
    ContentBlocker *blocker = content_blocker_create(&budget);
    CHECK(blocker != NULL
          && content_blocker_configure(blocker, CONTENT_BLOCKER_BASIC, NULL));
    const char *page = "https://www.bbc.com/news";
    const char *const refused[] = {
        "https://www.googletagmanager.com/gtag/js?id=AW-1",
        "https://www.googletagmanager.com/gtm.js?id=GTM-1",
        "https://assets.adobedtm.com/launch-1.min.js",
        "https://tags.tiqcdn.com/utag/site/main/prod/utag.js",
        "https://nexus.ensighten.com/site/Bootstrap.js",
        "https://cdn.tagcommander.com/1/tc_site.js",
        "https://cdn.segment.com/analytics.js/v1/key/analytics.min.js",
        "https://api.segment.io/v1/t",
        "https://cdn.optimizely.com/public/1/s/bbcx_prod.js",
        "https://logx.optimizely.com/v1/events",
        "https://dev.visualwebsiteoptimizer.com/j.php?a=1",
        "https://try.abtasty.com/1.js",
        "https://www.googleoptimize.com/optimize.js?id=OPT-1",
        "https://abc.kameleoon.eu/kameleoon.js",
        "https://abc.kameleoon.io/engine.js",
        "https://cdn-4.convertexperiments.com/js/1.js",
        "https://site.tt.omtrdc.net/rest/v1/delivery",
    };
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        if (!content_blocker_would_block(blocker, refused[i], page, "script",
                                         "no-cors")) {
            fprintf(stderr, "not refused: %s\n", refused[i]);
            return 1;
        }
    }
    /* Every destination, as for the ad hosts. */
    CHECK(content_blocker_would_block(
              blocker, "https://www.googletagmanager.com/ns.html?id=GTM-1",
              page, "iframe", "navigate")
          && content_blocker_would_block(
                 blocker, "https://api.segment.io/v1/t", page, "", "cors"));
    /* Paywall and subscription SDKs load under Basic, by any destination:
       the opt-in level that would refuse them is not offered yet. */
    const char *const paywall[] = {
        "https://cdn.tinypass.com/api/tinypass.min.js",
        "https://cdn.piano.io/api/tinypass.min.js",
        "https://experience.piano.io/xbuilder/experience/load",
        "https://assets.poool.fr/audit.min.js",
        "https://js.pelcro.com/sdk/main.min.js",
        "https://connector.laterpay.net/1/laterpay.js",
    };
    for (size_t i = 0; i < sizeof(paywall) / sizeof(paywall[0]); i++) {
        if (content_blocker_would_block(blocker, paywall[i], page, "script",
                                        "no-cors")
            || content_blocker_refused_script_shims(blocker, paywall[i],
                                                    page) != 0u) {
            fprintf(stderr, "paywall SDK refused: %s\n", paywall[i]);
            return 1;
        }
    }
    CHECK(!content_blocker_would_block(
              blocker, "https://api.piano.io/api/v3/access", page, "",
              "cors")
          && !content_blocker_would_block(
                 blocker, "https://buy.tinypass.com/checkout/template",
                 page, "iframe", "navigate"));
    /* Look-alikes, Adobe's analytics host beside Target's, the vendors' own
       pages and first-party copies are not refused. */
    const char *const kept[] = {
        "https://notoptimizely.com/x.js",
        "https://optimizely.com.example/x.js",
        "https://site.sc.omtrdc.net/b/ss/x",
        "https://segment.io.example/sdk.js",
        "https://www.google-analytics.com/analytics.js",
        "https://static.files.bbci.co.uk/core/bundle-vendor-optimizely.js",
    };
    for (size_t i = 0; i < sizeof(kept) / sizeof(kept[0]); i++) {
        if (content_blocker_would_block(blocker, kept[i], page, "script",
                                        "no-cors")) {
            fprintf(stderr, "refused: %s\n", kept[i]);
            return 1;
        }
    }
    CHECK(!content_blocker_would_block(
              blocker, "https://cdn.optimizely.com/js/web.js",
              "https://www.optimizely.com/", "script", "no-cors"));
    ContentBlockerMetrics metrics;
    CHECK(content_blocker_metrics(blocker, &metrics)
          && metrics.mode == CONTENT_BLOCKER_BASIC
          && metrics.rule_count == 36u + 15u);

    /* What a page waits for from a refused script. */
    CHECK(content_blocker_refused_script_shims(
              blocker, refused[0], page) == CONTENT_BLOCKER_SHIM_GOOGLE_TAG
          && content_blocker_refused_script_shims(
                 blocker, refused[12], page)
                 == CONTENT_BLOCKER_SHIM_GOOGLE_TAG
          && content_blocker_refused_script_shims(blocker, refused[2], page)
                 == CONTENT_BLOCKER_SHIM_ADOBE_TARGET
          && content_blocker_refused_script_shims(blocker, refused[16], page)
                 == CONTENT_BLOCKER_SHIM_ADOBE_TARGET
          && content_blocker_refused_script_shims(blocker, refused[10], page)
                 == CONTENT_BLOCKER_SHIM_VWO
          /* Refused, nothing to answer. */
          && content_blocker_refused_script_shims(blocker, refused[8], page)
                 == 0u
          && content_blocker_refused_script_shims(
                 blocker, "https://securepubads.doubleclick.net/tag.js",
                 page) == 0u
          /* Not refused: a first-party container. */
          && content_blocker_refused_script_shims(
                 blocker, "https://www.googletagmanager.com/gtm.js",
                 "https://www.googletagmanager.com/") == 0u);
    const char *const allowed[] = {"bbc.com"};
    CHECK(content_blocker_set_allowed_sites(blocker, allowed, 1u)
          && !content_blocker_would_block(blocker, refused[0], page,
                                          "script", "no-cors")
          && content_blocker_refused_script_shims(blocker, refused[0], page)
                 == 0u
          && content_blocker_set_allowed_sites(blocker, NULL, 0u));
    CHECK(content_blocker_configure(blocker, CONTENT_BLOCKER_OFF, NULL));
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++)
        CHECK(!content_blocker_would_block(blocker, refused[i], page,
                                           "script", "no-cors")
              && content_blocker_refused_script_shims(
                     blocker, refused[i], page) == 0u);
    content_blocker_destroy(blocker);
    CHECK(budget.current == 0u);
    return 0;
}

int main(void)
{
    CHECK(test_basic_sdk_hosts() == 0);
    CHECK(test_basic_ad_verification_hosts() == 0);
    CHECK(test_player_page_list() == 0);
    CHECK(test_long_lines_and_rule_limit() == 0);
    CHECK(test_prefix_scan_boundaries() == 0);
    CHECK(test_cosmetic_class_families() == 0);
    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    ContentBlocker *blocker = content_blocker_create(&budget);
    CHECK(blocker != NULL);
    static const char inline_tracker[] = "\nwindow.ue_ibe = (window.ue_ibe || 0) + 1;"
        "/* n.RXVM=function rx.ex64( /rd/uedata */ window.shouldNotRun=1;";
    CHECK(content_blocker_configure(blocker, CONTENT_BLOCKER_BASIC, NULL));
    CHECK(content_blocker_inline_rule(blocker, "https://www.amazon.com/", inline_tracker,
        sizeof(inline_tracker)-1) != NULL);
    CHECK(!content_blocker_inline_rule(blocker, "https://amazon.com.example.test/", inline_tracker,
        sizeof(inline_tracker)-1));
    CHECK(!content_blocker_inline_rule(blocker, "https://www.amazon.com/", inline_tracker,
        sizeof(inline_tracker)-40));
    CHECK(!content_blocker_inline_rule(blocker, "https://www.amazon.com/", inline_tracker,
        CONTENT_BLOCKER_INLINE_BYTES_LIMIT+1u));
    const char *inline_allowed[] = {"amazon.com"};
    CHECK(content_blocker_set_allowed_sites(blocker, inline_allowed, 1));
    CHECK(!content_blocker_inline_rule(blocker, "https://www.amazon.com/", inline_tracker,
        sizeof(inline_tracker)-1));
    CHECK(content_blocker_set_allowed_sites(blocker, NULL, 0));
    CHECK(content_blocker_configure(blocker, CONTENT_BLOCKER_OFF, NULL));
    CHECK(!content_blocker_inline_rule(blocker, "https://www.amazon.com/", inline_tracker,
        sizeof(inline_tracker)-1));
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
    /* Always the registrable site: a subdomain never survives, so a caller
       compares the site itself (clear-site-data's YouTube login check). */
    CHECK(content_blocker_site_from_url("https://m.youtube.com/watch?v=x", site)
          && strcmp(site, "youtube.com") == 0);
    CHECK(!content_blocker_site_from_url("not a URL", site));

    unlink(custom_path);
    content_blocker_destroy(blocker);
    CHECK(budget.current == 0 && budget_categories_reconcile(&budget));
    puts("content-blocker-tests: ok");
    return 0;
}
