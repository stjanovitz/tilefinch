#include "tilefinch/budget.h"
#include "tilefinch/content_blocker.h"
#include "tilefinch/document.h"
#include "tilefinch/fetch.h"
#include "tilefinch/font.h"
#include "tilefinch/layout.h"
#include "tilefinch/navigation.h"
#include "tilefinch/platform.h"
#include "tilefinch/request_context.h"
#include "tilefinch/resources.h"
#include "tilefinch/render.h"
#include "tilefinch/session.h"
#include "tilefinch/style.h"
#include "tilefinch/user_agent.h"
#include "../src/tilefinch_test_faults.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>
#ifdef TILEFINCH_TEST_HAVE_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#endif
#ifdef TILEFINCH_TEST_WOFF2
#include <brotli/encode.h>
#endif

#ifndef TILEFINCH_TEST_SOURCE_DIR
#define TILEFINCH_TEST_SOURCE_DIR "."
#endif

#define KIB (1024u)
#define MIB (1024u * KIB)
#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "web-font test failed at %s:%d: %s\n",           \
                __FILE__, __LINE__, #condition);                             \
        return false;                                                        \
    }                                                                        \
} while (0)

static lxb_dom_node_t *find_id(lxb_dom_node_t *node, const char *wanted)
{
    if (node == NULL) return NULL;
    if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        size_t length = 0;
        const char *id = document_attribute(node, "id", &length);
        if (id != NULL && length == strlen(wanted)
            && memcmp(id, wanted, length) == 0) return node;
    }
    for (lxb_dom_node_t *child = node->first_child; child != NULL;
         child = child->next) {
        lxb_dom_node_t *found = find_id(child, wanted);
        if (found != NULL) return found;
    }
    return NULL;
}

typedef struct {
    size_t stylesheet_parse_calls;
    size_t font_scan_calls;
    size_t font_fixup_calls;
} FontParsePhases;

static bool count_font_parse_phase(void *opaque, const char *phase,
                                   size_t completed_work_units)
{
    (void) completed_work_units;
    FontParsePhases *phases = opaque;
    if (phases == NULL || phase == NULL) return true;
    if (strcmp(phase, "stylesheet-parse") == 0) {
        phases->stylesheet_parse_calls++;
    } else if (strcmp(phase, "stylesheet-font-scan") == 0) {
        phases->font_scan_calls++;
    } else if (strcmp(phase, "stylesheet-font-fixup") == 0) {
        phases->font_fixup_calls++;
    }
    return true;
}

static bool test_inline_discovery_and_canonicalization(void)
{
    static const char html[] =
        "<!doctype html><style>"
        ".target{font-family:GDS\\20 Transport,serif;font-weight:700}"
        "/* @font-face{font-family:Comment Fake;src:url(comment.ttf)} */"
        ".noise::before{content:\"@font-face{font-family:String Fake;"
        "src:url(string.ttf)}\"}"
        "@keyframes fake{from{opacity:0}"
        "@font-face{font-family:Keyframe Fake;src:url(keyframe.ttf)}}"
        "@media (min-width:900px){@font-face{font-family:Media Fake;"
        "src:url(media.ttf)}}"
        "@supports (definitely-unsupported-property:value){"
        "@font-face{font-family:Supports Fake;src:url(supports.ttf)}}"
        "@container card (min-width:1px){"
        "@font-face{font-family:Container Fake;src:url(container.ttf)}}"
        "@media screen and (max-width:600px){"
        "@font-face{font-family:\"GDS Trans\\70 ort\";font-weight:400;"
        "font-style:normal;src:local(\"GDS Transport\"),"
        "url(regular.woff2) format(\"woff2\"),"
        "url(\"DATA:font/ttf;base64,AAAA\") format(\"truetype\"),"
        "url(../fonts/regular.woff) format(\"woff\"),"
        "url(./fallback.ttf) format(\"truetype\"),"
        "url(ignored.svg) format(\"svg\")}}"
        "@supports (display:flex){@layer fonts{"
        "@font-face{font-family:gds transport;font-weight:700;"
        "src:url(./bold.woff) format(woff)}}}"
        "</style><div id=target class=target>target</div>";
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 19));
    lxb_dom_node_t *target = find_id(
        lxb_dom_interface_node(document.html), "target");
    FontParsePhases phases = {0};
    TilefinchPlatformServices services = {
        .context = &phases,
        .cooperate = count_font_parse_phase
    };
    tilefinch_platform_set_services(&services);
    CHECK(target != NULL && stylesheet_build(&sheet, &budget, &document, 480));
    tilefinch_platform_set_services(NULL);
    CHECK(phases.stylesheet_parse_calls != 0);
    CHECK(phases.font_scan_calls == 0);
    CHECK(phases.font_fixup_calls != 0);

    StylesheetWebFontStats stats = {0};
    CHECK(stylesheet_web_font_stats(&sheet, &stats));
    CHECK(stats.declarations_discovered == 2);
    CHECK(stats.sources_selected == 3);
    CHECK(stats.unsupported_sources == 0);
    CHECK(stats.duplicate_sources == 0);
    CHECK(stats.skipped_family_limit == 0);
    CHECK(stats.skipped_source_limit == 0);
    CHECK(stylesheet_web_font_source_count(&sheet) == 3);

    StylesheetWebFontSource source = {0};
    CHECK(stylesheet_web_font_source(&sheet, 0, &source));
    CHECK(source.family_slot == 0 && !source.bold);
#ifdef TILEFINCH_TEST_WOFF2
    CHECK(strcmp(source.reference, "regular.woff2") == 0);
#else
    CHECK(strcmp(source.reference, "../fonts/regular.woff") == 0);
#endif
    CHECK(source.source_base_url == NULL);
    CHECK(source.source_referrer_policy == NULL);
    CHECK(stylesheet_web_font_source(&sheet, 1, &source));
    CHECK(source.family_slot == 0 && !source.bold);
#ifdef TILEFINCH_TEST_WOFF2
    CHECK(strcmp(source.reference, "../fonts/regular.woff") == 0);
#else
    CHECK(strcmp(source.reference, "./fallback.ttf") == 0);
#endif
    CHECK(stylesheet_web_font_source(&sheet, 2, &source));
    CHECK(source.family_slot == 0 && source.bold);
    CHECK(strcmp(source.reference, "./bold.woff") == 0);
    CHECK(!stylesheet_web_font_source(&sheet, 3, &source));

    ComputedStyle computed = style_for_node(&sheet, target, NULL);
    CHECK(font_family_is_web(computed.font_family));
    CHECK(font_family_web_slot(computed.font_family) == 0);
    CHECK(font_family_web_fallback(computed.font_family) == FONT_SERIF);
    CHECK(computed.font_bold && computed.font_weight == 700);

    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0);
    CHECK(budget_uninstall_lexbor(&budget));
    return true;
}

static bool test_external_base_and_source_order(void)
{
    static const char html[] =
        "<!doctype html><div id=target class=target>target</div>";
    static const char css[] =
        ".target{font-family:'aux sans',monospace}"
        "@font-face{font-family:\"AUX   Sans\";font-weight:normal;"
        "src:local(Aux),url(../fonts/aux.woff) format(woff),"
        "url(fallback.ttf) format(truetype)}";
    static const char base[] = "https://cdn.example/assets/css/site.css";
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 13));
    lxb_dom_node_t *target = find_id(
        lxb_dom_interface_node(document.html), "target");
    CHECK(target != NULL && stylesheet_build(&sheet, &budget, &document, 480));
    CHECK(stylesheet_add_css_from(&sheet, css, sizeof(css) - 1u, base));
    CHECK(stylesheet_add_css_from(&sheet, css, sizeof(css) - 1u, base));

    StylesheetWebFontStats stats = {0};
    CHECK(stylesheet_web_font_stats(&sheet, &stats));
    CHECK(stats.declarations_discovered == 2);
    CHECK(stats.sources_selected == 2);
    CHECK(stats.duplicate_sources == 2);
    CHECK(stylesheet_web_font_source_count(&sheet) == 2);
    StylesheetWebFontSource source = {0};
    CHECK(stylesheet_web_font_source(&sheet, 0, &source));
    CHECK(!source.bold && source.family_slot == 0);
    CHECK(strcmp(source.reference, "../fonts/aux.woff") == 0);
    CHECK(source.source_base_url != NULL
          && strcmp(source.source_base_url, base) == 0);
    CHECK(source.source_referrer_policy != NULL
          && source.source_referrer_policy[0] == '\0');
    CHECK(stylesheet_web_font_source(&sheet, 1, &source));
    CHECK(strcmp(source.reference, "fallback.ttf") == 0);
    CHECK(source.source_base_url != NULL
          && strcmp(source.source_base_url, base) == 0);
    CHECK(source.source_referrer_policy != NULL
          && source.source_referrer_policy[0] == '\0');

    ComputedStyle computed = style_for_node(&sheet, target, NULL);
    CHECK(font_family_is_web(computed.font_family));
    CHECK(font_family_web_slot(computed.font_family) == 0);
    CHECK(font_family_web_fallback(computed.font_family) == FONT_MONOSPACE);

    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0);
    CHECK(budget_uninstall_lexbor(&budget));
    return true;
}

static bool test_generated_icon_font_family(void)
{
    static const char html[] = "<!doctype html>"
        "<span id=target class='icon icon-search'></span>";
    static const char face_css[] =
        "@font-face{font-family:body-one;src:url(one.ttf)}"
        "@font-face{font-family:body-two;src:url(two.ttf)}"
        "@font-face{font-family:unused-one;src:url(three.ttf)}"
        "@font-face{font-family:unused-two;src:url(four.ttf)}"
        "@font-face{font-family:unused-three;src:url(five.ttf)}"
        "@font-face{font-family:icons;src:url(icons.woff2) format(woff2),"
        "url(icons.ttf) format(truetype),url(icons.woff) format(woff)}"
        "@font-face{font-family:body-three;src:url(seven.ttf)}"
        "@font-face{font-family:body-four;src:url(eight.ttf)}"
        "@font-face{font-family:refused-face;src:url(nine.ttf)}";
    static const char css[] =
        "span.icon:before,time.icon:before,a.icon:before{"
        "font-family:'icons' !important;font-style:normal;font-weight:normal;"
        "font-variant:normal;text-transform:none;line-height:1}"
        ".icon-search:before{content:'\\f002'}"
        "#target{font:16px icons;line-height:24px;font-weight:700}";
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 19));
    CHECK(stylesheet_build(&sheet, &budget, &document, 480));
    CHECK(stylesheet_add_css_from(&sheet, css, sizeof(css) - 1u,
                                 "https://example.test/site.css"));
    lxb_dom_node_t *node = find_id(lxb_dom_interface_node(document.html), "target");
    CHECK(node != NULL);
    ComputedStyle parent = style_for_node(&sheet, node, NULL);
    ComputedStyle pseudo = style_for_pseudo(&sheet, node, PSEUDO_BEFORE, &parent);
    CHECK(pseudo.generated_text_length == 3);
    CHECK(!font_family_is_web(pseudo.font_family));
    CHECK(parent.line_height == 24 && parent.font_weight == 700);
    CHECK(stylesheet_add_css_from(&sheet, face_css, sizeof(face_css) - 1u,
                                 "https://example.test/fonts.css"));
    parent = style_for_node(&sheet, node, NULL);
    CHECK(parent.line_height == 24 && parent.font_weight == 700);
    CHECK(font_family_is_web(parent.font_family));
    pseudo = style_for_pseudo(&sheet, node, PSEUDO_BEFORE, &parent);
    CHECK(font_family_is_web(pseudo.font_family));
    CHECK(font_family_web_slot(pseudo.font_family) == 5);
    CHECK(stylesheet_web_font_source_count(&sheet) == 9);
    StylesheetWebFontStats discovery = {0};
    CHECK(stylesheet_web_font_stats(&sheet, &discovery));
    CHECK(discovery.skipped_family_limit == 1);
    StylesheetWebFontSource last = {0};
    CHECK(stylesheet_web_font_source(&sheet, 8, &last)
          && last.family_slot == 7);
    CHECK(stylesheet_web_font_face(&sheet, 8, false) == NULL);
    FontFamily refused = FONT_SERIF;
    CHECK(!font_family_web(8, FONT_SANS, &refused)
          && refused == FONT_SERIF);
    LayoutDocument layout = {0};
    FontSet fallback_fonts = {0};
    CHECK(layout_build(&layout, &budget, &document, &sheet,
                       &fallback_fonts, NULL, 480));
    size_t generated_runs = 0;
    for (size_t at = 0; at < layout.count; at++) {
        const DrawCommand *command = &layout.commands[at];
        if (command->type != DRAW_TEXT || command->text_length != 3
            || memcmp(command->text, pseudo.generated_text, 3) != 0) continue;
        CHECK(draw_command_font_family(command) == pseudo.font_family);
        generated_runs++;
    }
    CHECK(generated_runs == 1);
    layout_destroy(&layout);
    /* Family and filter metadata must coexist, including copied shadows.
       Exercise every admitted family/fallback without another command word. */
    for (unsigned slot = 0; slot < TILEFINCH_WEB_FONT_FAMILY_LIMIT; slot++) {
        for (FontFamily fallback = FONT_SANS;
             fallback <= FONT_SYSTEM_SANS; fallback++) {
            FontFamily family = FONT_SANS;
            CHECK(font_family_web(slot, fallback, &family));
            for (unsigned filter = 0; filter < 8; filter++) {
                DrawCommand command = {.type = DRAW_TEXT,
                    .radius = LAYOUT_TEXT_KERNING_NONE
                              | LAYOUT_TEXT_BASELINE_ALIGNED};
                draw_command_set_filter_code(&command, filter);
                draw_command_set_text_font_family(&command, family);
                CHECK(draw_command_font_family(&command) == family);
                CHECK(draw_command_filter_code(&command) == filter);
                CHECK((command.radius & LAYOUT_TEXT_KERNING_NONE) != 0);
                CHECK((command.radius & LAYOUT_TEXT_BASELINE_ALIGNED) != 0);
                draw_command_set_filter_code(&command, 7 - filter);
                CHECK(draw_command_font_family(&command) == family);
                CHECK(draw_command_filter_code(&command) == 7 - filter);
                draw_command_set_text_font_family(&command, FONT_SERIF);
                CHECK(draw_command_font_family(&command) == FONT_SERIF);
                CHECK(draw_command_filter_code(&command) == 7 - filter);
                CHECK((command.radius & LAYOUT_TEXT_FONT_FAMILY_HIGH) == 0);
            }
        }
    }
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0);
    CHECK(budget_uninstall_lexbor(&budget));
    return true;
}

static bool test_total_font_source_bound(void)
{
    char html[16384];
    size_t used = (size_t)snprintf(html, sizeof(html), "<!doctype html><style>");
    static const unsigned weights[] = {400,700,300,800,500,900};
    for (unsigned family = 0; family < TILEFINCH_WEB_FONT_FAMILY_LIMIT; family++)
        for (size_t w = 0; w < sizeof(weights)/sizeof(weights[0]); w++) {
            int added = snprintf(html + used, sizeof(html) - used,
                "@font-face{font-family:F%u;font-weight:%u;"
                "src:url(f%u-%u.woff),url(f%u-%u.ttf)}",
                family, weights[w], family, weights[w], family, weights[w]);
            CHECK(added > 0 && (size_t)added < sizeof(html) - used);
            used += (size_t)added;
        }
    CHECK(sizeof(html) - used > 30);
    used += (size_t)snprintf(html + used, sizeof(html) - used, "</style><p>bounded fonts</p>");
    Budget budget; budget_init(&budget, 8u*MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0}; Stylesheet sheet = {0};
    CHECK(document_parse(&document, &budget, html, used, 11)
          && stylesheet_build(&sheet, &budget, &document, 480));
    CHECK(stylesheet_web_font_source_count(&sheet) == STYLE_WEB_FONT_TOTAL_SOURCE_LIMIT);
    StylesheetWebFontStats stats = {0};
    CHECK(stylesheet_web_font_stats(&sheet, &stats)
          && stats.sources_selected == STYLE_WEB_FONT_TOTAL_SOURCE_LIMIT
          && stats.skipped_source_limit > 0);
    stylesheet_destroy(&sheet); document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return true;
}

static bool test_distinct_font_weights(void)
{
    static const char html[] = "<!doctype html><style>"
        "@font-face{font-family:Weighted;src:url(normal.woff);font-weight:400}"
        "@font-face{font-family:Weighted;src:url(bold.woff);font-weight:700}"
        "@font-face{font-family:Weighted;src:url(light.woff),url(light.ttf);font-weight:300}"
        "@font-face{font-family:Weighted;src:url(heavy.woff);font-weight:800}"
        "@font-face{font-family:Weighted;src:url(medium.woff);font-weight:500}"
        "@font-face{font-family:Weighted;src:url(extra.woff);font-weight:900}"
        "@font-face{font-family:Weighted;src:url(over-cap.woff);font-weight:200}"
        "</style><div>weights</div>";
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1, 19));
    CHECK(stylesheet_build(&sheet, &budget, &document, 480));
    CHECK(stylesheet_web_font_source_count(&sheet) == 7);
    WebFontSet selected = {0};
    unsigned weights_seen = 0;
    for (size_t i = 0; i < 7; i++) {
        StylesheetWebFontSource source = {0};
        CHECK(stylesheet_web_font_source(&sheet, i, &source));
        CHECK(source.family_slot == 0 && source.face_slot < TILEFINCH_WEB_FONT_WEIGHT_LIMIT);
        CHECK(source.weight == 300 || source.weight == 400
              || source.weight == 500 || source.weight == 700
              || source.weight == 800 || source.weight == 900);
        CHECK(source.bold == (source.weight >= 600));
        CHECK(stylesheet_web_font_face_at(&sheet, 0, source.face_slot) != NULL);
        selected.families[0].weights[source.face_slot] = source.weight;
        selected.families[0].faces[source.face_slot].loaded = true;
        selected.families[0].faces[source.face_slot].authored_weight = source.weight;
        weights_seen |= 1u << (source.weight / 100);
    }
    CHECK(weights_seen == ((1u << 3) | (1u << 4) | (1u << 5)
        | (1u << 7) | (1u << 8) | (1u << 9)));
    FontFamily family = FONT_SANS;
    CHECK(font_family_web(0, FONT_SERIF, &family));
    static const unsigned requested[] = {200,300,350,400,450,500,550,600,700,800};
    static const unsigned expected[] =  {300,300,300,400,500,500,700,700,700,800};
    for (size_t i = 0; i < sizeof(requested)/sizeof(requested[0]); i++) {
        const FontFace *face = font_context_face_at_weight(
            NULL, &selected, family, false, requested[i] >= 650, requested[i]);
        CHECK(face != NULL && face->authored_weight == expected[i]);
        CHECK(!font_face_needs_synthetic_bold(NULL, &selected, face, requested[i])
            || face->authored_weight < requested[i]);
    }
    StylesheetWebFontStats stats = {0};
    CHECK(stylesheet_web_font_stats(&sheet, &stats)
          && stats.skipped_source_limit == 1);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0);
    CHECK(budget_uninstall_lexbor(&budget));
    return true;
}

static bool test_single_pass_forward_reference_reinterns(void)
{
    static const char html[] =
        "<!doctype html><style>"
        ".a{font-family:Forward Face,sans-serif}"
        ".b{font-family:Forward Face,sans-serif}"
        "</style><style>"
        "@font-face{font-family:Forward Face;src:url(forward.ttf)}"
        "</style><div id=a class=a>a</div><div id=b class=b>b</div>";
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    CHECK(stylesheet_build(&sheet, &budget, &document, 480));
    CHECK(sheet.count == 2 && sheet.declaration_count == 1);
    CHECK(stylesheet_web_font_source_count(&sheet) == 1);
    lxb_dom_node_t *a = find_id(lxb_dom_interface_node(document.html), "a");
    lxb_dom_node_t *b = find_id(lxb_dom_interface_node(document.html), "b");
    ComputedStyle a_style = style_for_node(&sheet, a, NULL);
    ComputedStyle b_style = style_for_node(&sheet, b, NULL);
    CHECK(font_family_is_web(a_style.font_family)
          && a_style.font_family == b_style.font_family);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0);
    CHECK(budget_uninstall_lexbor(&budget));
    return true;
}

static bool test_external_referrer_policy_identity(void)
{
    static const char html[] = "<!doctype html><div>target</div>";
    static const char css[] =
        "@font-face{font-family:Policy Face;"
        "src:url(font.woff) format(woff)}";
    static const char allocation_failure_css[] =
        "@font-face{font-family:Policy Allocation Failure;"
        "src:url(failure.woff) format(woff)}";
    static const char base[] = "https://cdn.example/css/site.css";
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(STYLE_WEB_FONT_REFERRER_POLICY_CAPACITY == 40);
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 13));
    CHECK(stylesheet_build(&sheet, &budget, &document, 480));
    CHECK(stylesheet_add_css_from_context(
        &sheet, css, sizeof(css) - 1u, base, "No-Referrer"));
    CHECK(stylesheet_add_css_from_context(
        &sheet, css, sizeof(css) - 1u, base, "unsafe-url"));
    CHECK(stylesheet_add_css_from_context(
        &sheet, css, sizeof(css) - 1u, base, "NO-REFERRER"));

    StylesheetWebFontStats stats = {0};
    CHECK(stylesheet_web_font_stats(&sheet, &stats));
    CHECK(stats.declarations_discovered == 3);
    CHECK(stats.sources_selected == 2);
    CHECK(stats.duplicate_sources == 1);
    CHECK(stats.skipped_source_limit == 0);
    CHECK(stylesheet_web_font_source_count(&sheet) == 2);
    StylesheetWebFontSource source = {0};
    CHECK(stylesheet_web_font_source(&sheet, 0, &source));
    CHECK(source.source_base_url != NULL
          && strcmp(source.source_base_url, base) == 0);
    CHECK(source.source_referrer_policy != NULL
          && strcmp(source.source_referrer_policy, "no-referrer") == 0);
    CHECK(stylesheet_web_font_source(&sheet, 1, &source));
    CHECK(source.source_base_url != NULL
          && strcmp(source.source_base_url, base) == 0);
    CHECK(source.source_referrer_policy != NULL
          && strcmp(source.source_referrer_policy, "unsafe-url") == 0);

    /* With the fixed policy stored inline, this permits exactly the source
       reference allocation and then fails the external-base allocation.
       The partially retained reference must be rolled back and the source
       must not become reachable with document-relative semantics. */
    size_t before_failed_source = budget.current;
    budget_inject_failure_after(&budget, 1);
    CHECK(stylesheet_add_css_from_context(
        &sheet, allocation_failure_css, sizeof(allocation_failure_css) - 1u,
        base, "origin"));
    budget_clear_failure_injection(&budget);
    CHECK(budget.current == before_failed_source);
    CHECK(stylesheet_web_font_source_count(&sheet) == 2);
    CHECK(stylesheet_web_font_stats(&sheet, &stats));
    CHECK(stats.declarations_discovered == 4);
    CHECK(stats.sources_selected == 2);
    CHECK(stats.duplicate_sources == 1);
    CHECK(stats.skipped_source_limit == 1);

    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0);
    CHECK(budget_uninstall_lexbor(&budget));
    return true;
}

static void store_be32(unsigned char *destination, uint32_t value)
{
    destination[0] = (unsigned char) (value >> 24);
    destination[1] = (unsigned char) (value >> 16);
    destination[2] = (unsigned char) (value >> 8);
    destination[3] = (unsigned char) value;
}

static uint16_t read_be16(const unsigned char *source)
{
    return (uint16_t) ((uint16_t) source[0] << 8 | source[1]);
}

static uint32_t read_be32(const unsigned char *source)
{
    return (uint32_t) source[0] << 24 | (uint32_t) source[1] << 16
        | (uint32_t) source[2] << 8 | source[3];
}

static void store_be16(unsigned char *destination, uint16_t value)
{
    destination[0] = (unsigned char) (value >> 8);
    destination[1] = (unsigned char) value;
}

static bool aligned_four(size_t value, size_t *aligned)
{
    if (aligned == NULL || value > SIZE_MAX - 3u) return false;
    *aligned = (value + 3u) & ~(size_t) 3u;
    return true;
}

/* Build a standards-shaped, uncompressed WOFF 1.0 wrapper around the
   permissively licensed Tilefinch sfnt at runtime.  This exercises the exact
   page-font decoder without retaining another binary fixture in the repo. */
static bool build_uncompressed_woff(const unsigned char *sfnt,
                                    size_t sfnt_length,
                                    unsigned char **woff,
                                    size_t *woff_length)
{
    if (sfnt == NULL || woff == NULL || woff_length == NULL
        || sfnt_length < 12 || read_be32(sfnt) != UINT32_C(0x00010000)) {
        return false;
    }
    *woff = NULL;
    *woff_length = 0;
    uint16_t table_count = read_be16(sfnt + 4);
    if (table_count == 0 || table_count > 64
        || (size_t) table_count > (sfnt_length - 12u) / 16u) return false;
    size_t directory_bytes = (size_t) table_count * 20u;
    size_t output_size = 44u + directory_bytes;
    size_t reconstructed_size = 12u + (size_t) table_count * 16u;
    for (size_t table = 0; table < table_count; table++) {
        const unsigned char *record = sfnt + 12u + table * 16u;
        size_t source_offset = read_be32(record + 8);
        size_t source_length = read_be32(record + 12);
        size_t padded = 0;
        if (source_offset > sfnt_length
            || source_length > sfnt_length - source_offset
            || !aligned_four(source_length, &padded)
            || output_size > SIZE_MAX - padded
            || reconstructed_size > SIZE_MAX - padded) return false;
        output_size += padded;
        reconstructed_size += padded;
    }
    if (output_size > UINT32_MAX || reconstructed_size > UINT32_MAX
        || output_size > 4u * MIB) return false;
    unsigned char *output = calloc(1, output_size);
    if (output == NULL) return false;
    memcpy(output, "wOFF", 4);
    store_be32(output + 4, read_be32(sfnt));
    store_be32(output + 8, (uint32_t) output_size);
    store_be16(output + 12, table_count);
    store_be32(output + 16, (uint32_t) reconstructed_size);
    store_be16(output + 20, 1);
    size_t destination_offset = 44u + directory_bytes;
    for (size_t table = 0; table < table_count; table++) {
        const unsigned char *record = sfnt + 12u + table * 16u;
        unsigned char *destination = output + 44u + table * 20u;
        size_t source_offset = read_be32(record + 8);
        size_t source_length = read_be32(record + 12);
        size_t padded = 0;
        if (!aligned_four(source_length, &padded)) {
            free(output);
            return false;
        }
        store_be32(destination, read_be32(record));
        store_be32(destination + 4, (uint32_t) destination_offset);
        store_be32(destination + 8, (uint32_t) source_length);
        store_be32(destination + 12, (uint32_t) source_length);
        store_be32(destination + 16, read_be32(record + 4));
        memcpy(output + destination_offset, sfnt + source_offset,
               source_length);
        destination_offset += padded;
    }
    if (destination_offset != output_size) {
        free(output);
        return false;
    }
    *woff = output;
    *woff_length = output_size;
    return true;
}

/* Repack the same bounded fixture with actual zlib-compressed tables, so
   WOFF tests exercise the inflater rather than only the sfnt wrapper. */
static bool build_compressed_woff(const unsigned char *sfnt,
                                  size_t sfnt_length,
                                  unsigned char **woff, size_t *woff_length)
{
    if (!build_uncompressed_woff(sfnt, sfnt_length, woff, woff_length)) {
        return false;
    }
    unsigned char *output = *woff;
    uint16_t tables = read_be16(sfnt + 4);
    size_t offset = 44u + (size_t) tables * 20u;
    bool compressed_any = false;
    for (size_t table = 0; table < tables; table++) {
        const unsigned char *source = sfnt + 12u + table * 16u;
        unsigned char *record = output + 44u + table * 20u;
        size_t length = read_be32(source + 12);
        const unsigned char *data = sfnt + read_be32(source + 8);
        uLongf compressed = (uLongf) length;
        size_t stored = length;
        if (compress2(output + offset, &compressed, data, (uLong) length,
                      Z_DEFAULT_COMPRESSION) == Z_OK && compressed < length) {
            stored = (size_t) compressed;
            compressed_any = true;
        } else {
            memcpy(output + offset, data, length);
        }
        store_be32(record + 4, (uint32_t) offset);
        store_be32(record + 8, (uint32_t) stored);
        size_t padded = 0;
        if (!aligned_four(stored, &padded)) {
            free(output);
            *woff = NULL;
            *woff_length = 0;
            return false;
        }
        memset(output + offset + stored, 0, padded - stored);
        offset += padded;
    }
    store_be32(output + 8, (uint32_t) offset);
    *woff_length = offset;
    if (!compressed_any) {
        free(output);
        *woff = NULL;
        *woff_length = 0;
        return false;
    }
    return true;
}

static bool read_file(const char *path, unsigned char **data, size_t *length)
{
    *data = NULL;
    *length = 0;
    FILE *file = fopen(path, "rb");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
        if (file != NULL) fclose(file);
        return false;
    }
    long size = ftell(file);
    if (size <= 0 || (unsigned long) size > 2u * MIB
        || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    unsigned char *bytes = malloc((size_t) size);
    if (bytes == NULL) {
        fclose(file);
        return false;
    }
    bool ok = fread(bytes, 1, (size_t) size, file) == (size_t) size;
    bool closed = fclose(file) == 0;
    if (!ok || !closed) {
        free(bytes);
        return false;
    }
    *data = bytes;
    *length = (size_t) size;
    return true;
}

static void remove_async_font_replay(const char *directory)
{
    char path[512];
    const char *files[] = {"0000.body", "0000.meta", "trace.meta"};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        if (snprintf(path, sizeof(path), "%s/%s", directory, files[i]) > 0) {
            (void) unlink(path);
        }
    }
    (void) rmdir(directory);
}

static bool test_embedded_font_loading(void)
{
    unsigned char *sfnt = NULL, *woff = NULL;
    size_t sfnt_length = 0, length = 0;
    CHECK(read_file(TILEFINCH_TEST_SOURCE_DIR "/fonts/TilefinchSans-Regular.ttf",
                    &sfnt, &sfnt_length)
          && build_compressed_woff(sfnt, sfnt_length, &woff, &length));
    free(sfnt);
    static const char prefix[] = "<!doctype html><style>@font-face{font-family:Embedded;"
        "src:url(\"DATA:font/woff;base64,";
    static const char suffix[] = "\") format(woff)}body{font-family:Embedded}"
        "</style><body>Embedded font</body>";
    static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t encoded_length = (length + 2u) / 3u * 4u;
    CHECK(encoded_length > 2048 && encoded_length < 128u * KIB);
    char *html = malloc(sizeof(prefix) - 1u + encoded_length + sizeof(suffix));
    CHECK(html != NULL);
    memcpy(html, prefix, sizeof(prefix) - 1u);
    char *out = html + sizeof(prefix) - 1u;
    for (size_t at = 0; at < length; at += 3u) {
        size_t left = length - at;
        uint32_t value = (uint32_t) woff[at] << 16;
        if (left > 1u) value |= (uint32_t) woff[at + 1u] << 8;
        if (left > 2u) value |= woff[at + 2u];
        *out++ = digits[value >> 18];
        *out++ = digits[(value >> 12) & 63u];
        *out++ = left > 1u ? digits[(value >> 6) & 63u] : '=';
        *out++ = left > 2u ? digits[value & 63u] : '=';
    }
    memcpy(out, suffix, sizeof(suffix));
    for (unsigned scenario = 0; scenario < 4; scenario++) {
        Budget budget;
        budget_init(&budget, 8u * MIB);
        CHECK(budget_install_lexbor(&budget));
        PocDocument document = {0};
        Stylesheet sheet = {0};
        CHECK(document_parse(&document, &budget, html, strlen(html), 17)
              && stylesheet_build(&sheet, &budget, &document, 480)
              && stylesheet_web_font_source_count(&sheet) == 1);
        TilefinchContentSecurityPolicy csp;
        tilefinch_csp_init(&csp);
        const char *policy = scenario == 1
            ? "content-security-policy: font-src 'self'\n"
            : "content-security-policy: font-src data:\n";
        CHECK(tilefinch_csp_parse_response_headers(&csp, "https://font.test/",
                    policy, strlen(policy), false));
        if (scenario == 3) budget_inject_failure_after(&budget, 0);
        ExternalFontStats stats = {0};
        bool completed = fonts_load_external(&sheet, &budget, "https://font.test/",
                  "https://font.test/", NULL, 2, 128u * KIB,
                  scenario == 2 ? length - 1u : 128u * KIB,
                  256u * KIB, 5000, NULL, NULL, &csp, &stats);
        CHECK(completed || scenario == 3);
        if (scenario == 3) {
            CHECK(budget.injected_failure_count != 0);
            budget_clear_failure_injection(&budget);
        }
        const FontFace *face = stylesheet_web_font_face(&sheet, 0, false);
        CHECK(face != NULL && face->loaded == (scenario == 0)
              && stats.loaded_faces == (scenario == 0 ? 1u : 0u));
        if (scenario == 0) CHECK(stats.encoded_bytes == length);
        stylesheet_destroy(&sheet);
        document_destroy(&document);
        CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    }
    free(html);
    free(woff);
    return true;
}

static uint64_t async_font_body_hash(const unsigned char *data,
                                     size_t length)
{
    uint64_t value = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < length; i++) {
        value ^= data[i];
        value *= UINT64_C(1099511628211);
    }
    return value;
}

static bool write_async_font_replay(
    const unsigned char *font, size_t font_length,
    char directory[128])
{
    if (font == NULL || font_length == 0 || directory == NULL) return false;
    snprintf(directory, 128, "%s", "/tmp/tilefinch-font-async-XXXXXX");
    if (mkdtemp(directory) == NULL) return false;
    char body_path[192], meta_path[192], trace_path[192];
    bool paths = snprintf(body_path, sizeof(body_path), "%s/0000.body",
                          directory) > 0
        && snprintf(meta_path, sizeof(meta_path), "%s/0000.meta",
                    directory) > 0
        && snprintf(trace_path, sizeof(trace_path), "%s/trace.meta",
                    directory) > 0;
    FILE *body = paths ? fopen(body_path, "wb") : NULL;
    bool body_written = body != NULL
        && fwrite(font, 1, font_length, body) == font_length;
    bool body_closed = body != NULL && fclose(body) == 0;
    FILE *meta = body_written && body_closed ? fopen(meta_path, "wb") : NULL;
    bool meta_written = meta != NULL && fprintf(
        meta,
        "psp-http-trace=8\n"
        "cookie-values=redacted\n"
        "method=GET\n"
        "url=https://font-async.test/font.ttf\n"
        "success=1\n"
        "async-delay-pumps=2\n"
        "external-cancel=0\n"
        "transport-timeout=0\n"
        "redirect-origin-tainted=0\n"
        "error=\n"
        "request-body-length=0\n"
        "request-body-hash=cbf29ce484222325\n"
        "request-content-type=\n"
        "request-cookie-bytes=0\n"
        "request-has-cf-clearance=0\n"
        "request-extra-header-bytes=0\n"
        "request-extra-header-shape=\n"
        "request-allow-http-errors=1\n"
        "request-enforce-cors=1\n"
        "request-redirect-same-origin-only=0\n"
        "request-cors-cached-response-validated=0\n"
        "request-if-none-match=\n"
        "request-if-modified-since=\n"
        "request-referer=\n"
        "request-origin=https://font-async.test\n"
        "request-accept=font/woff,application/font-woff;q=0.9,*/*;q=0.1\n"
        "request-sec-fetch-dest=font\n"
        "request-sec-fetch-mode=cors\n"
        "request-sec-fetch-site=same-origin\n"
        "request-send-client-hints=0\n"
        "request-client-hint-tokens=\n"
        "request-client-hint-origin=\n"
        "request-send-low-client-hints=0\n"
        "request-sec-fetch-user=0\n"
        "request-upgrade-insecure=0\n"
        "request-user-agent=" TILEFINCH_BROWSER_USER_AGENT "\n"
        "request-diagnostic-mobile-safari=0\n"
        "request-credentials=2\n"
        "request-credential-origin=https://font-async.test\n"
        "request-initiator-url=https://font-async.test/page\n"
        "request-referrer-source=https://font-async.test/page\n"
        "request-referrer-policy=no-referrer\n"
        "status=200\n"
        "length=%zu\n"
        "response-body-hash=%016llx\n"
        "effective-url=https://font-async.test/font.ttf\n"
        "content-type=font/ttf\n"
        "etag=\nlast-modified=\ncf-mitigated=\naccept-ch=\ncritical-ch=\n"
        "server=fixture\ncf-ray=\n"
        "response-header-count=2\nset-cookie-count=0\n"
        "response-header-0=content-type: font/ttf\n"
        "response-header-1=access-control-allow-origin: "
        "https://font-async.test\n",
        font_length,
        (unsigned long long) async_font_body_hash(font, font_length)) > 0;
    bool meta_closed = meta != NULL && fclose(meta) == 0;
    FILE *trace = meta_written && meta_closed
        ? fopen(trace_path, "wb") : NULL;
    bool trace_written = trace != NULL && fprintf(
        trace,
        "psp-http-trace-clock=1\n"
        "origin-ms=1000\n"
        "capture-complete=yes\n"
        "record-count=1\n") > 0;
    bool trace_closed = trace != NULL && fclose(trace) == 0;
    if (!trace_written || !trace_closed) {
        remove_async_font_replay(directory);
        return false;
    }
    return true;
}

static bool test_async_font_loader_is_fallback_first(void)
{
    static const char html[] =
        "<!doctype html><style>"
        "@font-face{font-family:AsyncFace;"
        "src:url(https://font-async.test/font.ttf) format(truetype)}"
        ".target{font-family:AsyncFace,sans-serif}"
        "</style><div class=target>fallback first</div>";
    unsigned char *font = NULL;
    size_t font_length = 0;
    CHECK(read_file(TILEFINCH_TEST_SOURCE_DIR
                    "/fonts/TilefinchSans-Regular.ttf",
                    &font, &font_length));
    char directory[128] = {0};
    CHECK(write_async_font_replay(font, font_length, directory));

    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    CHECK(stylesheet_build(&sheet, &budget, &document, 480));
    FontFace *face = stylesheet_web_font_face(&sheet, 0, false);
    CHECK(face != NULL && !face->loaded);

    char error[256] = {0};
    CHECK(fetch_trace_replay_begin_response_keyed(
        directory, error, sizeof(error)));
    FetchScheduler *scheduler = fetch_scheduler_create(
        &budget, 1, 256u * KIB);
    CHECK(scheduler != NULL);
    ExternalFontLoader loader = {0};
    ExternalFontStats stats = {0};
    CHECK(fonts_external_loader_begin(&loader, &sheet, 1000, &stats));
    /* Discovery is CPU-only: beginning the page font pipeline neither
       reserves a response body nor starts network work. */
    CHECK(fonts_external_loader_pending(&loader)
          && fetch_scheduler_pending(scheduler) == 0
          && stats.attempted == 0 && !face->loaded);

    bool loaded = false;
    CHECK(fonts_external_loader_step(
        &loader, &sheet, &budget, "https://font-async.test/page",
        "https://font-async.test/page", "no-referrer", 1,
        256u * KIB, 256u * KIB, 1u * MIB, scheduler, NULL, NULL, &stats, 0,
        &loaded));
    CHECK(!loaded && !face->loaded && stats.attempted == 1
          && fetch_scheduler_pending(scheduler) == 1);
    for (size_t slice = 0;
         slice < 8 && fonts_external_loader_pending(&loader); slice++) {
        CHECK(fonts_external_loader_step(
            &loader, &sheet, &budget, "https://font-async.test/page",
            "https://font-async.test/page", "no-referrer", 1,
            256u * KIB, 256u * KIB, 1u * MIB, scheduler, NULL, NULL, &stats, 0,
            &loaded));
    }
#ifdef TILEFINCH_TEST_HAVE_FREETYPE
    if (!loaded || !face->loaded || stats.loaded_faces != 1
        || stats.failed != 0) {
        FetchTraceReplayStats replay = {0};
        (void) fetch_trace_replay_stats(&replay);
        fprintf(stderr,
                "async-font loaded=%d face=%d attempted=%zu loaded-faces=%zu "
                "failed=%zu unsupported=%zu pending=%d scheduler=%zu "
                "replay matched=%zu rejected=%zu unmatched=%zu invalid=%zu\n",
                loaded, face->loaded, stats.attempted, stats.loaded_faces,
                stats.failed, stats.unsupported,
                fonts_external_loader_pending(&loader),
                fetch_scheduler_pending(scheduler),
                replay.matched_request_count, replay.rejected_request_count,
                replay.unmatched_request_count,
                replay.invalid_route_request_count);
    }
    CHECK(loaded && face->loaded && stats.loaded_faces == 1
          && stats.failed == 0);
#else
    CHECK(!loaded && !face->loaded && stats.loaded_faces == 0
          && stats.failed == 1 && stats.unsupported == 1);
#endif
    CHECK(!fonts_external_loader_pending(&loader)
          && fetch_scheduler_pending(scheduler) == 0);

    font_face_destroy(face);
    ExternalFontLoader cancelled_loader = {0};
    ExternalFontStats cancelled_stats = {0};
    CHECK(fonts_external_loader_begin(
        &cancelled_loader, &sheet, 1000, &cancelled_stats));
    loaded = false;
    CHECK(fonts_external_loader_step(
        &cancelled_loader, &sheet, &budget,
        "https://font-async.test/page",
        "https://font-async.test/page", "no-referrer", 1,
        256u * KIB, 256u * KIB, 1u * MIB, scheduler, NULL, NULL,
        &cancelled_stats, 0, &loaded));
    CHECK(fonts_external_loader_pending(&cancelled_loader)
          && cancelled_stats.attempted == 1 && !loaded);
    fonts_external_loader_cancel(
        &cancelled_loader, scheduler, &cancelled_stats);
    CHECK(!fonts_external_loader_pending(&cancelled_loader)
          && fetch_scheduler_pending(scheduler) == 0
          && cancelled_stats.deadline_cancelled == 0);

    fetch_scheduler_destroy(scheduler);
    fetch_trace_end();
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0);
    CHECK(budget_uninstall_lexbor(&budget));
    remove_async_font_replay(directory);
    free(font);
    return true;
}

static bool test_deferred_font_families_and_aliases(void)
{
#ifdef TILEFINCH_TEST_HAVE_FREETYPE
    static const char html[] = "<!doctype html><style>"
        "@font-face{font-family:Used;src:url(https://font-async.test/font.ttf)}"
        "@font-face{font-family:Later;src:url(https://font-async.test/font.ttf)}"
        "</style><p>text</p>";
    unsigned char *font = NULL;
    size_t length = 0;
    char directory[128] = {0}, error[256] = {0};
    CHECK(read_file(TILEFINCH_TEST_SOURCE_DIR "/fonts/TilefinchSans-Regular.ttf", &font, &length));
    CHECK(write_async_font_replay(font, length, directory));
    Budget budget; budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0}; Stylesheet sheet = {0};
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1, 17)
        && stylesheet_build(&sheet, &budget, &document, 480));
    CHECK(fetch_trace_replay_begin_response_keyed(directory, error, sizeof(error)));
    FetchScheduler *scheduler = fetch_scheduler_create(&budget, 1, 256u * KIB);
    ExternalFontLoader loader = {0}; ExternalFontStats stats = {0};
    CHECK(scheduler != NULL && fonts_external_loader_begin(&loader, &sheet, 1000, &stats));
    fonts_external_loader_set_usage(&loader, &sheet, 0);
    CHECK(!fonts_external_loader_pending(&loader) && stats.attempted == 0);
    fonts_external_loader_set_usage(&loader, &sheet, 1);
    for (unsigned turn = 0; turn < 32 && fonts_external_loader_pending(&loader); turn++) {
        bool loaded = false;
        CHECK(fonts_external_loader_step(&loader, &sheet, &budget,
            "https://font-async.test/page", "https://font-async.test/page", "no-referrer",
            4, 512u * KIB, 256u * KIB, MIB, scheduler, NULL, NULL, &stats, 0, &loaded));
    }
    FontFace *used = stylesheet_web_font_face(&sheet, 0, false);
    FontFace *later = stylesheet_web_font_face(&sheet, 1, false);
    CHECK(used != NULL && later != NULL && used->loaded && !later->loaded
        && stats.loaded_faces == 1 && !fonts_external_loader_pending(&loader));
    fonts_external_loader_set_usage(&loader, &sheet, 3);
    for (unsigned turn = 0; turn < 32 && fonts_external_loader_pending(&loader); turn++) {
        bool loaded = false;
        CHECK(fonts_external_loader_step(&loader, &sheet, &budget,
            "https://font-async.test/page", "https://font-async.test/page", "no-referrer",
            4, 512u * KIB, 256u * KIB, MIB, scheduler, NULL, NULL, &stats, 0, &loaded));
    }
    CHECK(later->loaded && stats.loaded_faces == 2 && stats.attempted == 2
        && used->implementation == later->implementation && used->data == later->data);
    int width = font_text_width(later, "AV test", 7, 17, false);
    CHECK(width > 0 && font_face_cached_ascii_advances(later) > 0);
    font_face_destroy(used);
    CHECK(font_text_width(later, "AV test", 7, 17, false) == width);
    fonts_external_loader_cancel(&loader, scheduler, &stats);
    fonts_external_loader_set_usage(&loader, &sheet, 1);
    CHECK(!fonts_external_loader_pending(&loader));
    fetch_scheduler_destroy(scheduler); fetch_trace_end();
    stylesheet_destroy(&sheet); document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    remove_async_font_replay(directory); free(font);
#endif
    return true;
}

static bool test_deferred_font_weights(bool prioritized)
{
#ifdef TILEFINCH_TEST_HAVE_FREETYPE
    static const char html[] = "<!doctype html><style>"
        "@font-face{font-family:Used;src:url(https://font-async.test/font.ttf);font-weight:400}"
        "@font-face{font-family:Used;src:url(https://font-async.test/font.ttf);font-weight:700}"
        "@font-face{font-family:Used;src:url(https://font-async.test/font.ttf);font-weight:500}"
        "</style><p>text</p>";
    unsigned char *font = NULL; size_t length = 0;
    char directory[128] = {0}, error[256] = {0};
    CHECK(read_file(TILEFINCH_TEST_SOURCE_DIR "/fonts/TilefinchSans-Regular.ttf", &font, &length)
          && write_async_font_replay(font, length, directory));
    Budget budget; budget_init(&budget, 8u*MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0}; Stylesheet sheet = {0};
    CHECK(document_parse(&document, &budget, html, sizeof(html)-1, 17)
          && stylesheet_build(&sheet, &budget, &document, 480));
    CHECK(fetch_trace_replay_begin_response_keyed(directory, error, sizeof(error)));
    FetchScheduler *scheduler = fetch_scheduler_create(&budget, 1, 256u*KIB);
    ExternalFontLoader loader = {0}; ExternalFontStats stats = {0};
    CHECK(scheduler != NULL && fonts_external_loader_begin(&loader, &sheet, 1000, &stats));
    if (prioritized)
        fonts_external_loader_prioritize_faces(&loader, &sheet, UINT64_C(5), UINT64_C(4));
    else fonts_external_loader_set_face_usage(&loader, &sheet, UINT64_C(5));
    for (unsigned turn = 0; turn < 32 && fonts_external_loader_pending(&loader); turn++) {
        bool loaded = false;
        CHECK(fonts_external_loader_step(&loader, &sheet, &budget,
            "https://font-async.test/page", "https://font-async.test/page", "no-referrer",
            4, 512u*KIB, 256u*KIB, MIB, scheduler, NULL, NULL, &stats, 0, &loaded));
    }
    CHECK(stats.loaded_faces == (prioritized ? 1u : 2u)
          && stats.attempted == (prioritized ? 1u : 2u)
          && stylesheet_web_font_face_at(&sheet, 0, 0)->loaded == !prioritized
          && !stylesheet_web_font_face_at(&sheet, 0, 1)->loaded
          && stylesheet_web_font_face_at(&sheet, 0, 2)->loaded);
    fonts_external_loader_set_face_usage(&loader, &sheet, UINT64_C(7));
    for (unsigned turn = 0; turn < 32 && fonts_external_loader_pending(&loader); turn++) {
        bool loaded = false;
        CHECK(fonts_external_loader_step(&loader, &sheet, &budget,
            "https://font-async.test/page", "https://font-async.test/page", "no-referrer",
            4, 512u*KIB, 256u*KIB, MIB, scheduler, NULL, NULL, &stats, 0, &loaded));
    }
    CHECK(stats.loaded_faces == 3 && stats.attempted == 3
          && stylesheet_web_font_face_at(&sheet, 0, 1)->loaded
          && stylesheet_web_font_face_at(&sheet, 0, 0)->implementation
              == stylesheet_web_font_face_at(&sheet, 0, 2)->implementation);
    fonts_external_loader_cancel(&loader, scheduler, &stats);
    fetch_scheduler_destroy(scheduler); fetch_trace_end();
    stylesheet_destroy(&sheet); document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    remove_async_font_replay(directory); free(font);
#endif
    return true;
}

static bool test_exact_cached_web_font_advances(void)
{
#ifdef TILEFINCH_TEST_HAVE_FREETYPE
    unsigned char *bytes = NULL;
    size_t length = 0;
    CHECK(read_file(TILEFINCH_TEST_SOURCE_DIR "/fonts/TilefinchSans-Regular.ttf", &bytes, &length));
    Budget budget; budget_init(&budget, 8u * MIB);
    FontFace face = {0}, alias = {0};
    CHECK(font_face_load_encoded(&face, &budget, bytes, length, MIB));
    char text[96];
    for (unsigned at = 0; at < 95; at++) text[at] = (char) (32 + at);
    text[95] = 0;
    const int sizes[] = {9 * 64, 17 * 64, 1191, 28 * 64, 17 * 64};
    for (unsigned at = 0; at < sizeof(sizes) / sizeof(sizes[0]); at++) {
        for (unsigned bold = 0; bold < 2; bold++) {
            for (unsigned kern = 0; kern < 2; kern++) {
                int reference = 0;
                for (unsigned c = 32; c < 127; c++) {
                    FontGlyph glyph = {0};
                    CHECK(font_glyph_load_at_size(&face, c, sizes[at], bold, &glyph));
                    reference += glyph.advance_fixed;
                    font_glyph_destroy(&face, &glyph);
                    if (kern && c > 32)
                        reference += font_kerning_at_size_fixed(&face, c - 1, c, sizes[at]);
                }
                CHECK(font_text_width_at_size_fixed_mode(&face, text, 95, sizes[at], bold, kern) == reference);
                CHECK(font_face_cached_ascii_advances(&face) == 95);
                for (unsigned repeat = 0; repeat < 8; repeat++)
                    CHECK(font_text_width_at_size_fixed_mode(&face, text, 95, sizes[at], bold, kern) == reference);
            }
        }
    }
    size_t retained = budget.current;
    CHECK(font_face_share_encoded(&alias, &face, &budget, bytes, length, MIB)
        && budget.current == retained);
    font_face_destroy(&alias); /* Alias first; source must remain usable. */
    FontFace refused = {0};
    CHECK(!font_face_share_encoded(&refused, &face, &budget, bytes, length, MIB - 1));
    bytes[0] ^= 1;
    CHECK(!font_face_share_encoded(&refused, &face, &budget, bytes, length, MIB));
    bytes[0] ^= 1;
    font_face_destroy(&face); free(bytes);
    CHECK(budget.current == 0);
#endif
    return true;
}

static bool cancel_font_fallback(void *opaque, const char *phase, size_t work)
{
    (void) work;
    if (strcmp(phase, "layout") != 0) return true;
    *(bool *) opaque = true;
    return false;
}

static bool test_presentation_font_source_identity(void)
{
#ifdef TILEFINCH_TEST_HAVE_FREETYPE
    static const char html[] = "<!doctype html><body>text";
    static const char source_css[] =
        "@font-face{font-family:Shared;src:url(face.ttf)}";
    static const char reordered_css[] =
        "@font-face{font-family:Other;src:url(other.ttf)}"
        "@font-face{font-family:Shared;src:url(face.ttf)}";
    static const char changed_css[] =
        "@font-face{font-family:Shared;src:url(different.ttf)}";
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet source = {0}, target = {0}, changed = {0};
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1, 13));
    CHECK(stylesheet_build(&source, &budget, &document, 480)
        && stylesheet_build(&target, &budget, &document, 480)
        && stylesheet_build(&changed, &budget, &document, 480));
    CHECK(stylesheet_add_css_from(&source, source_css, sizeof(source_css) - 1,
                                  "https://font.test/style.css")
        && stylesheet_add_css_from(&target, reordered_css, sizeof(reordered_css) - 1,
                                   "https://font.test/style.css")
        && stylesheet_add_css_from(&changed, changed_css, sizeof(changed_css) - 1,
                                   "https://font.test/style.css"));
    unsigned char *bytes = NULL;
    size_t length = 0;
    CHECK(read_file(TILEFINCH_TEST_SOURCE_DIR "/fonts/TilefinchSans-Regular.ttf",
                    &bytes, &length));
    FontFace *original = stylesheet_web_font_face_at(&source, 0, 0);
    CHECK(original && font_face_load_encoded(original, &budget, bytes, length, MIB));
    StyleWebFonts *owned = source.web_fonts;
    source.web_fonts = NULL;
    stylesheet_destroy(&source);
    size_t retained = budget.current;
    budget_inject_failure_after(&budget, 0);
    stylesheet_share_unchanged_web_fonts(&target, owned, MIB);
    stylesheet_share_unchanged_web_fonts(&changed, owned, MIB);
    budget_clear_failure_injection(&budget);
    FontFace *copy = stylesheet_web_font_face_at(&target, 1, 0);
    CHECK(copy && copy->loaded && copy->implementation == original->implementation
        && !stylesheet_web_font_face_at(&target, 0, 0)->loaded
        && !stylesheet_web_font_face_at(&changed, 0, 0)->loaded
        && budget.current == retained);
    stylesheet_release_web_fonts(&budget, owned);
    CHECK(font_text_width(copy, "Still available", 15, 16, false) > 0);
    stylesheet_destroy(&target);
    stylesheet_destroy(&changed);
    document_destroy(&document);
    free(bytes);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
#endif
    return true;
}

typedef enum {
    EXTERNAL_FONT_STANDARD,
    EXTERNAL_FONT_PRESENTATION,
    EXTERNAL_FONT_ICON_PRESSURE
} ExternalFontScenario;

static bool test_external_font_publication(bool refuse, bool smart,
                                            ExternalFontScenario scenario)
{
    static const char prefix[] =
        "<!doctype html><style>"
        "@font-face{font-family:AsyncFace;"
        "src:url(https://font-async.test/font.ttf) format(truetype)}"
        "#metric{font-family:AsyncFace,sans-serif;font-size:20px;width:20ch;height:8px}"
        "</style><body><div id=metric></div>";
    char html[16384];
    size_t html_length = (size_t) snprintf(html, sizeof(html), "%s", prefix);
    if (scenario == EXTERNAL_FONT_ICON_PRESSURE) {
        html_length += (size_t) snprintf(html + html_length, sizeof(html) - html_length,
            "<style>#metric::before{content:'\\e000'}</style>");
    }
    for (unsigned row = 0; row < 300; row++) {
        int appended = snprintf(html + html_length, sizeof(html) - html_length,
            "<p>font-independent row %u</p>", row);
        CHECK(appended > 0 && (size_t) appended < sizeof(html) - html_length);
        html_length += (size_t) appended;
    }
    unsigned char *font = NULL;
    size_t font_length = 0;
    CHECK(read_file(TILEFINCH_TEST_SOURCE_DIR
                    "/fonts/TilefinchSans-Regular.ttf",
                    &font, &font_length));
    char directory[128] = {0};
    CHECK(write_async_font_replay(font, font_length, directory));
    char replay_error[256] = {0};
    CHECK(fetch_trace_replay_begin_response_keyed(
        directory, replay_error, sizeof(replay_error)));

    Budget budget;
    budget_init(&budget, 16u * MIB);
    CHECK(budget_install_lexbor(&budget));
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    CHECK(browser_session_init(&browser, &budget, 128u * KIB)
          && navigation_init(&navigation, &budget, 4));
    navigation_attach_browser_session(&navigation, &browser);
    navigation_enable_web_fonts(
        &navigation, 1, 256u * KIB, 256u * KIB, 1u * MIB, 1000);
    uint64_t generation = navigation_begin(&navigation);
    CHECK(navigation_commit_html(
        &navigation, generation, "https://font-async.test/page", html,
        html_length, 480, NULL, NULL, true));
    lxb_dom_node_t *metric = find_id(
        lxb_dom_interface_node(navigation.page.document.html), "metric");
    const LayoutNodeBox *metric_box = layout_box_for_node(&navigation.page.layout, metric);
    CHECK(metric_box != NULL && navigation.page.stylesheet.metric_web_font_families == 1);
    CHECK(navigation.page.stylesheet.metric_web_font_faces == UINT64_C(1));
    CHECK(navigation.page.runtime == NULL
          && navigation.page.external_font_loader.required_faces == UINT64_C(1));
    int old_width = metric_box->width;
    LayoutReuseStats before = {0}, after = {0};
    CHECK(navigation.page.layout_reuse != NULL);
    layout_reuse_cache_stats(navigation.page.layout_reuse, &before);
    if (refuse) navigation_test_refuse_next_background_font_relayout();
    NavigationBackgroundWorkOutcome outcome =
        NAVIGATION_BACKGROUND_WORK_SUCCESS;
    for (size_t slice = 0; slice < 16u; slice++) {
        outcome = navigation_run_background_resources(&navigation);
        if (outcome != NAVIGATION_BACKGROUND_WORK_SUCCESS) break;
        if (navigation.performance.background_fonts_loaded != 0) break;
    }
    layout_reuse_cache_stats(navigation.page.layout_reuse, &after);
    CHECK(outcome == (refuse ? NAVIGATION_BACKGROUND_WORK_SOFT_REFUSAL
                            : NAVIGATION_BACKGROUND_WORK_SUCCESS)
          && navigation.page.loaded
          && navigation.last_error[0] == '\0'
          && navigation.performance.background_fonts_loaded == 1u
          && navigation.performance.background_font_failures == (refuse ? 1u : 0u)
          && navigation.performance.background_font_relayouts == (refuse ? 0u : 1u));
    if (!refuse) {
        metric_box = layout_box_for_node(&navigation.page.layout, metric);
        CHECK(after.style_hits > before.style_hits && metric_box != NULL
              && metric_box->width != old_width);
        FontFace *face = stylesheet_web_font_face_at(&navigation.page.stylesheet, 0, 0);
        CHECK(face != NULL && face->loaded);
        if (smart) {
            CHECK(navigation_set_web_font_mode(&navigation, WEB_FONTS_SMART));
            size_t remaining = budget_remaining(&budget);
            CHECK(remaining > 1536u * KIB);
            void *pressure = budget_malloc(&budget, remaining - 1536u * KIB);
            CHECK(pressure != NULL);
            size_t old_memory = budget.current;
            CHECK(navigation_run_background_resources(&navigation)
                  == NAVIGATION_BACKGROUND_WORK_SUCCESS);
            CHECK(navigation.page.web_font_pressure_fallback && navigation.page.loaded);
            if (scenario == EXTERNAL_FONT_ICON_PRESSURE) {
                CHECK(face->loaded && budget.current <= old_memory);
            } else CHECK(!face->loaded && budget.current < old_memory);
            budget_free(&budget, pressure);
            CHECK(!navigation_background_resources_pending(&navigation));
            if (scenario == EXTERNAL_FONT_ICON_PRESSURE) {
                static const char css[] = "body{color:#123456}";
                NavigationUserCssTransaction *transaction = navigation_apply_user_css_begin(
                    &navigation, css, sizeof(css) - 1);
                CHECK(transaction != NULL);
                navigation_apply_user_css_commit(&navigation, transaction);
                face = stylesheet_web_font_face_at(&navigation.page.stylesheet, 0, 0);
                CHECK(face && face->loaded);
                size_t full_before = navigation.performance.full_relayouts;
                navigation.style_rebuild_required = true;
                CHECK(navigation_relayout(&navigation)
                    && navigation.performance.full_relayouts == full_before + 1);
                face = stylesheet_web_font_face_at(&navigation.page.stylesheet, 0, 0);
                CHECK(face && face->loaded);
                CHECK(navigation_set_web_font_mode(&navigation, WEB_FONTS_OFF));
                CHECK(navigation_run_background_resources(&navigation)
                      == NAVIGATION_BACKGROUND_WORK_SUCCESS && !face->loaded);
            }
        } else {
            if (scenario == EXTERNAL_FONT_PRESENTATION) {
                static const char presentation[] = "body{color:#123456}";
                NavigationUserCssTransaction *transaction = navigation_apply_user_css_begin(
                    &navigation, presentation, sizeof(presentation) - 1);
                CHECK(transaction != NULL);
                FontFace *replacement = stylesheet_web_font_face_at(
                    &navigation.page.stylesheet, 0, 0);
                CHECK(replacement && replacement->loaded
                      && replacement->implementation == face->implementation);
                CHECK(navigation_apply_user_css_rollback(&navigation, transaction));
                face = stylesheet_web_font_face_at(&navigation.page.stylesheet, 0, 0);
                CHECK(face && face->loaded);
                transaction = navigation_apply_user_css_begin(
                    &navigation, presentation, sizeof(presentation) - 1);
                CHECK(transaction != NULL);
                navigation_apply_user_css_commit(&navigation, transaction);
                face = stylesheet_web_font_face_at(&navigation.page.stylesheet, 0, 0);
                CHECK(face && face->loaded);
                goto teardown;
            }
            CHECK(navigation_set_web_font_mode(&navigation, WEB_FONTS_OFF));
            budget_inject_failure_after(&budget, 0);
            CHECK(navigation_run_background_resources(&navigation) == NAVIGATION_BACKGROUND_WORK_SOFT_REFUSAL
                  && navigation.page.loaded && face->loaded);
            budget_clear_failure_injection(&budget);
            CHECK(!navigation_background_resources_pending(&navigation));
            CHECK(navigation_set_web_font_mode(&navigation, WEB_FONTS_ON)
                  && navigation_set_web_font_mode(&navigation, WEB_FONTS_OFF));
            bool cancelled = false;
            TilefinchPlatformServices services = {.context = &cancelled, .cooperate = cancel_font_fallback};
            tilefinch_platform_set_services(&services);
            NavigationBackgroundWorkOutcome interrupted = navigation_run_background_resources(&navigation);
            tilefinch_platform_set_services(NULL);
            CHECK(interrupted == NAVIGATION_BACKGROUND_WORK_SOFT_REFUSAL && cancelled
                  && navigation.page.loaded && face->loaded
                  && navigation_background_resources_pending(&navigation));
            size_t old_memory = budget.current;
            CHECK(navigation_run_background_resources(&navigation) == NAVIGATION_BACKGROUND_WORK_SUCCESS
                  && navigation.page.loaded && !face->loaded && budget.current < old_memory);
            CHECK(!navigation_background_resources_pending(&navigation));
        }
        CHECK(navigation.relayout_damage_valid && navigation.relayout_damage_top == 0
              && navigation.relayout_damage_bottom >= navigation.page.layout.height);
    }
teardown:
    navigation_destroy(&navigation);
    browser_session_destroy(&browser);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    fetch_trace_end();
    remove_async_font_replay(directory);
    free(font);
    return true;
}

static bool test_web_font_modes(void)
{
    static const char html[] = "<!doctype html><style>"
        "@font-face{font-family:Demand;src:url(https://font-async.test/font.ttf)}"
        "p{font-family:Demand,sans-serif}</style><p>Readable fallback</p>";
    unsigned char *font = NULL; size_t length = 0;
    char directory[128] = {0}, error[256] = {0};
    CHECK(read_file(TILEFINCH_TEST_SOURCE_DIR "/fonts/TilefinchSans-Regular.ttf", &font, &length)
          && write_async_font_replay(font, length, directory));
    for (WebFontMode mode = WEB_FONTS_ON; mode < WEB_FONTS_MODE_COUNT; mode++) {
        Budget budget; budget_init(&budget, 16u * MIB);
        CHECK(budget_install_lexbor(&budget));
        NavigationSession navigation = {0};
        CHECK(navigation_init(&navigation, &budget, 4)
              && navigation_set_web_font_mode(&navigation, mode)
              && !navigation_set_web_font_mode(&navigation, WEB_FONTS_MODE_COUNT));
        navigation_enable_web_fonts(&navigation, 4, 256u * KIB, 256u * KIB, MIB, 1000);
        CHECK(fetch_trace_replay_begin_response_keyed(directory, error, sizeof(error))
              && navigation_commit_html(&navigation, navigation_begin(&navigation),
                   "https://font-async.test/page", html, sizeof(html) - 1, 480, NULL, NULL, true));
        size_t original_limit = budget.limit;
        budget.limit = budget.current + 256u * KIB;
        CHECK(navigation_run_background_resources(&navigation) == NAVIGATION_BACKGROUND_WORK_SUCCESS);
        if (mode == WEB_FONTS_ON) {
            CHECK(navigation.page.external_fonts.attempted == 1
                  && !navigation.page.web_font_pressure_fallback);
        } else {
            CHECK(navigation.page.external_fonts.attempted == 0
                  && !fonts_external_loader_pending(&navigation.page.external_font_loader));
            if (mode == WEB_FONTS_SMART) CHECK(navigation.page.web_font_pressure_fallback);
        }
        budget.limit = original_limit;
        if (mode != WEB_FONTS_ON) CHECK(!navigation_background_resources_pending(&navigation));
        navigation_destroy(&navigation); fetch_trace_end();
        CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    }
    remove_async_font_replay(directory); free(font);
    return true;
}

static bool test_blocked_font_settles_without_backpressure_retry(void)
{
    static const char html[] =
        "<!doctype html><style>"
        "@font-face{font-family:BlockedFace;"
        "src:url(https://doubleclick.net/font.ttf) format(truetype)}"
        ".target{font-family:BlockedFace,sans-serif}"
        "</style><div class=target>fallback</div>";
    static const char document_url[] = "https://publisher.example/page";
    Budget budget;
    budget_init(&budget, 4u * MIB);
    CHECK(budget_install_lexbor(&budget));
    BrowserSession session = {0};
    CHECK(browser_session_init(&session, &budget, 256u * KIB));
    ContentBlocker *blocker = content_blocker_create(&budget);
    CHECK(blocker != NULL
          && content_blocker_configure(
                 blocker, CONTENT_BLOCKER_BASIC, NULL));
    session.content_blocker = blocker;
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    CHECK(stylesheet_build(&sheet, &budget, &document, 480));
    FetchScheduler *scheduler = fetch_scheduler_create(
        &budget, 1, 256u * KIB);
    CHECK(scheduler != NULL);
    ExternalFontLoader loader = {0};
    ExternalFontStats stats = {0};
    bool loaded = false;
    CHECK(fonts_external_loader_begin(&loader, &sheet, 1000, &stats));
    CHECK(fonts_external_loader_step(
        &loader, &sheet, &budget, document_url, document_url,
        "no-referrer", 1, 256u * KIB, 256u * KIB, 1u * MIB,
        scheduler, &session, NULL, &stats, 0, &loaded));
    ContentBlockerMetrics metrics = {0};
    CHECK(!loaded && !fonts_external_loader_pending(&loader)
          && fetch_scheduler_pending(scheduler) == 0
          && stats.sources_discovered == 1 && stats.attempted == 1
          && stats.failed == 1 && stats.loaded_faces == 0
          && content_blocker_metrics(blocker, &metrics)
          && metrics.requests_blocked == 1);
    fetch_scheduler_destroy(scheduler);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    session.content_blocker = NULL;
    content_blocker_destroy(blocker);
    browser_session_destroy(&session);
    CHECK(budget.current == 0);
    CHECK(budget_uninstall_lexbor(&budget));
    return true;
}

static bool rejected_without_retention(Budget *budget,
                                       const unsigned char *data,
                                       size_t length,
                                       size_t maximum_backend_bytes)
{
    size_t baseline = budget->current;
    FontFace face = {0};
    bool rejected = !font_face_load_encoded(
        &face, budget, data, length, maximum_backend_bytes);
    bool empty = !face.loaded && face.data == NULL
                 && face.implementation == NULL
                 && face.backend == FONT_FACE_BACKEND_NONE;
    font_face_destroy(&face);
    return rejected && empty && budget->current == baseline;
}

static bool rejected_woff_prefix(Budget *budget, unsigned char *woff,
                                 size_t woff_length, size_t prefix_length,
                                 size_t maximum_backend_bytes)
{
    if (prefix_length >= woff_length || prefix_length > UINT32_MAX) {
        return false;
    }
    uint32_t declared = read_be32(woff + 8);
    if (prefix_length >= 44) {
        store_be32(woff + 8, (uint32_t) prefix_length);
    }
    bool rejected = rejected_without_retention(
        budget, woff, prefix_length, maximum_backend_bytes);
    store_be32(woff + 8, declared);
    return rejected;
}

static bool test_woff_truncation_boundaries(Budget *budget,
                                            unsigned char *woff,
                                            size_t woff_length,
                                            size_t maximum_backend_bytes)
{
    CHECK(woff_length >= 44 && read_be32(woff) == UINT32_C(0x774f4646));
    uint16_t table_count = read_be16(woff + 12);
    CHECK(table_count != 0
          && 44u + (size_t) table_count * 20u < woff_length);
    for (size_t prefix = 0; prefix < 44; prefix++) {
        CHECK(rejected_woff_prefix(budget, woff, woff_length, prefix,
                                   maximum_backend_bytes));
    }
    /* Both sides of every directory-record boundary, including the first
       byte of table storage after the complete directory. */
    for (size_t entry = 0; entry <= table_count; entry++) {
        size_t boundary = 44u + entry * 20u;
        if (boundary != 0 && boundary - 1u < woff_length) {
            CHECK(rejected_woff_prefix(
                budget, woff, woff_length, boundary - 1u,
                maximum_backend_bytes));
        }
        if (boundary < woff_length) {
            CHECK(rejected_woff_prefix(
                budget, woff, woff_length, boundary,
                maximum_backend_bytes));
        }
    }
    /* Give FreeType a truthful shortened container length at the start and
       just before the end of each table, not merely a stale full-size WOFF
       header that the outer loader can reject by itself. */
    for (size_t table = 0; table < table_count; table++) {
        const unsigned char *record = woff + 44u + table * 20u;
        size_t offset = read_be32(record + 4);
        size_t compressed_length = read_be32(record + 8);
        CHECK(offset < woff_length
              && compressed_length <= woff_length - offset);
        size_t boundaries[3] = {
            offset,
            offset + (compressed_length != 0),
            offset + compressed_length - (compressed_length != 0)
        };
        for (size_t candidate = 0; candidate < 3; candidate++) {
            size_t prefix = boundaries[candidate];
            if (prefix >= 44 && prefix < woff_length) {
                CHECK(rejected_woff_prefix(
                    budget, woff, woff_length, prefix,
                    maximum_backend_bytes));
            }
        }
    }
    CHECK(rejected_woff_prefix(budget, woff, woff_length, woff_length - 1u,
                               maximum_backend_bytes));
    return true;
}

#ifdef TILEFINCH_TEST_HAVE_FREETYPE
static bool test_web_glyph_coverage(const FontFace *face,
                                     const unsigned char *bytes, size_t length)
{
    FT_Library library = NULL;
    FT_Face reference = NULL;
    CHECK(FT_Init_FreeType(&library) == 0
          && FT_New_Memory_Face(library, bytes, (FT_Long) length,
                                0, &reference) == 0);
    static const unsigned codepoints[] = {'I', 'l', '|', 'A'};
    for (int size = 12 * 64; size <= 20 * 64; size += 17) {
        CHECK(FT_Set_Char_Size(reference, 0, size, 72, 72) == 0);
        for (size_t at = 0; at < sizeof(codepoints) / sizeof(codepoints[0]); at++) {
            FT_UInt index = FT_Get_Char_Index(reference, codepoints[at]);
            CHECK(FT_Load_Glyph(reference, index, FT_LOAD_NO_HINTING
                      | FT_LOAD_NO_AUTOHINT | FT_LOAD_NO_BITMAP | FT_LOAD_NO_SVG) == 0
                  && FT_Render_Glyph(reference->glyph, FT_RENDER_MODE_NORMAL) == 0);
            FontGlyph glyph = {0};
            CHECK(font_glyph_load_at_size(face, codepoints[at], size, false, &glyph));
            FT_GlyphSlot slot = reference->glyph;
            bool same = glyph.width == (int) slot->bitmap.width
                && glyph.height == (int) slot->bitmap.rows
                && glyph.x_offset == slot->bitmap_left
                && glyph.y_offset == -slot->bitmap_top
                && glyph.advance_fixed == slot->advance.x;
            for (int y = 0; same && y < glyph.height; y++) {
                int pitch = slot->bitmap.pitch;
                int row = pitch < 0 ? glyph.height - y - 1 : y;
                const unsigned char *pixels = slot->bitmap.buffer
                    + (size_t) row * (size_t) (pitch < 0 ? -pitch : pitch);
                same = memcmp(glyph.pixels + (size_t) y * glyph.width,
                              pixels, (size_t) glyph.width) == 0;
            }
            if (!same) fprintf(stderr, "glyph coverage U+%04x size=%d native=%dx%d@%d,%d reference=%ux%u@%d,%d\n",
                               codepoints[at], size, glyph.width, glyph.height,
                               glyph.x_offset, glyph.y_offset, slot->bitmap.width,
                               slot->bitmap.rows, slot->bitmap_left, -slot->bitmap_top);
            font_glyph_destroy(face, &glyph);
            CHECK(same);
        }
    }
    FT_Done_Face(reference);
    FT_Done_FreeType(library);
    return true;
}

typedef struct {
    int width;
    int metric_height;
    int line_height;
    int ascent;
    int glyph_width;
    int glyph_height;
    int glyph_advance;
    uint64_t glyph_hash;
} FontProbe;

static bool probe_web_font(Budget *budget, const unsigned char *encoded,
                           size_t encoded_length,
                           size_t maximum_backend_bytes, FontProbe *probe)
{
    FontFace face = {0};
    FontGlyph glyph = {0};
    FontProbe measured = {0};
    bool loaded = font_face_load_encoded(
        &face, budget, encoded, encoded_length, maximum_backend_bytes);
    bool ok = loaded && face.loaded
        && face.backend == FONT_FACE_BACKEND_FREETYPE_WEB;
    if (ok) CHECK(font_face_backend_bytes(&face) > 0
        && font_face_backend_bytes(&face) <= budget->current);
    if (ok) {
        measured.width = font_text_width(
            &face, "Tilefinch AV", 12, 17, false);
        measured.metric_height = font_metric_height(&face, 17);
        measured.line_height = font_line_height(&face, 17);
        measured.ascent = font_ascent(&face, 17);
        ok = measured.width > 0 && measured.metric_height > 0
             && measured.line_height >= measured.metric_height
             && measured.ascent > 0
             && font_glyph_load(&face, 'A', 17, false, &glyph)
             && glyph.pixels != NULL && glyph.budget == budget
             && glyph.width > 0 && glyph.height > 0;
    }
    if (ok) {
        measured.glyph_width = glyph.width;
        measured.glyph_height = glyph.height;
        measured.glyph_advance = glyph.advance_fixed;
        measured.glyph_hash = UINT64_C(1469598103934665603);
        size_t pixels = (size_t) glyph.width * (size_t) glyph.height;
        for (size_t i = 0; i < pixels; i++) {
            measured.glyph_hash ^= glyph.pixels[i];
            measured.glyph_hash *= UINT64_C(1099511628211);
        }
    }
    font_glyph_destroy(&face, &glyph);
    font_face_destroy(&face);
    CHECK(font_face_backend_bytes(&face) == 0 && font_face_backend_bytes(NULL) == 0);
    if (ok && probe != NULL) *probe = measured;
    return ok;
}

static bool test_web_font_raster_retention(Budget *budget,
    const unsigned char *encoded, size_t encoded_length,
    size_t maximum_backend_bytes)
{
    /* Keep identical web-face glyphs across layout replacement, but reject
       a newly loaded face in the very same family slot/address. */
    WebFontSet cached_fonts = {0};
    FontSet cached_fallback = {0};
    CHECK(font_face_load_encoded(&cached_fonts.families[0].regular,
        budget, encoded, encoded_length, maximum_backend_bytes));
    FontFamily cached_family;
    CHECK(font_family_web(0, FONT_SANS, &cached_family));
    DrawCommand cached_text = {.type = DRAW_TEXT, .text = "AAA",
        .text_length = 3, .x = 0, .y = 0, .width = 48, .height = 24,
        .font_size = 16, .opacity_scale = 256};
    draw_command_set_text_font_family(&cached_text, cached_family);
    LayoutDocument cached_layout = {.budget = budget,
        .commands = &cached_text, .count = 1, .width = 48, .height = 32,
        .fonts = &cached_fallback, .web_fonts = &cached_fonts,
        .page_background = 0xffffff};
    TileCache cached_tiles;
    uint16_t cached_pixels[48 * 32], expected_pixels[48 * 32];
    CHECK(tile_cache_init(&cached_tiles, budget, &cached_layout, 1)
          && tile_cache_set_frame(&cached_tiles, cached_pixels, 48 * 32)
          && tile_cache_render_frame(&cached_tiles, 0, 48, 32, NULL));
    CHECK(cached_tiles.glyph_cache_misses != 0);
    size_t cached_misses = cached_tiles.glyph_cache_misses;
    memcpy(expected_pixels, cached_pixels, sizeof(cached_pixels));
    CHECK(tile_cache_replace_layout(&cached_tiles, &cached_layout)
          && tile_cache_render_frame(&cached_tiles, 0, 48, 32, NULL)
          && cached_tiles.glyph_cache_misses == cached_misses
          && memcmp(cached_pixels, expected_pixels, sizeof(cached_pixels)) == 0);
    uint64_t previous_serial = cached_fonts.families[0].regular.serial;
    web_font_set_destroy(&cached_fonts);
    CHECK(font_face_load_encoded(&cached_fonts.families[0].regular,
        budget, encoded, encoded_length, maximum_backend_bytes)
        && cached_fonts.families[0].regular.serial != previous_serial);
    CHECK(tile_cache_replace_layout(&cached_tiles, &cached_layout)
          && tile_cache_render_frame(&cached_tiles, 0, 48, 32, NULL)
          && cached_tiles.glyph_cache_misses > cached_misses
          && memcmp(cached_pixels, expected_pixels, sizeof(cached_pixels)) == 0);
    tile_cache_destroy(&cached_tiles);
    web_font_set_destroy(&cached_fonts);
    return true;
}

static bool font_probes_equal(const FontProbe *left, const FontProbe *right)
{
    return left->width == right->width
        && left->metric_height == right->metric_height
        && left->line_height == right->line_height
        && left->ascent == right->ascent
        && left->glyph_width == right->glyph_width
        && left->glyph_height == right->glyph_height
        && left->glyph_advance == right->glyph_advance
        && left->glyph_hash == right->glyph_hash;
}

static bool sweep_font_allocation_failures(const unsigned char *encoded,
                                           size_t encoded_length,
                                           size_t maximum_backend_bytes)
{
    Budget clean;
    budget_init(&clean, 4u * MIB);
    size_t allocations_before = clean.allocation_count;
    FontProbe probe = {0};
    CHECK(probe_web_font(&clean, encoded, encoded_length,
                         maximum_backend_bytes, &probe));
    size_t successful_allocations = clean.allocation_count
                                    - allocations_before;
    size_t largest = 0;
    CHECK(clean.current == 0
          && budget_active_allocations(&clean, &largest) == 0
          && successful_allocations != 0);

    size_t sweep_limit = successful_allocations + 32u;
    if (sweep_limit > 256u) sweep_limit = 256u;
    bool reached_uninjected_success = false;
    for (size_t failure_at = 0; failure_at <= sweep_limit; failure_at++) {
        Budget fault;
        budget_init(&fault, 4u * MIB);
        budget_inject_failure_after(&fault, failure_at);
        FontProbe ignored = {0};
        bool completed = probe_web_font(
            &fault, encoded, encoded_length, maximum_backend_bytes, &ignored);
        bool injected = fault.injected_failure_count != 0;
        budget_clear_failure_injection(&fault);
        largest = 0;
        CHECK(fault.current == 0
              && budget_active_allocations(&fault, &largest) == 0);
        if (!injected) {
            CHECK(completed);
            reached_uninjected_success = true;
            break;
        }
    }
    CHECK(reached_uninjected_success);
    return true;
}
#endif

#if defined(TILEFINCH_TEST_WOFF2) && defined(TILEFINCH_TEST_HAVE_FREETYPE)
static bool build_null_transform_woff2(const unsigned char *sfnt, size_t length,
    unsigned char **output, size_t *output_length)
{
    CHECK(length >= 12);
    unsigned tables = read_be16(sfnt + 4);
    CHECK(tables > 0 && tables <= 256 && tables <= (length - 12u) / 16u);
    unsigned char directory[256u * 9u];
    size_t directory_length = 0, payload_length = 0;
    unsigned char *payload = malloc(length);
    CHECK(payload != NULL);
    for (unsigned i = 0; i < tables; i++) {
        const unsigned char *entry = sfnt + 12u + i * 16u;
        uint32_t tag = read_be32(entry), offset = read_be32(entry + 8);
        uint32_t bytes = read_be32(entry + 12);
        CHECK(offset <= length && bytes <= length - offset
              && bytes <= length - payload_length);
        directory[directory_length++] = (tag == UINT32_C(0x676c7966)
            || tag == UINT32_C(0x6c6f6361)) ? 255u : 63u;
        memcpy(directory + directory_length, entry, 4);
        directory_length += 4;
        unsigned char encoded[5];size_t count = 0;
        do { encoded[count++] = (unsigned char) (bytes & 127u); bytes >>= 7; } while (bytes);
        while (count) {
            unsigned char byte = encoded[--count];
            directory[directory_length++] = count ? byte | 128u : byte;
        }
        bytes = read_be32(entry + 12);
        memcpy(payload + payload_length, sfnt + offset, bytes);
        payload_length += bytes;
    }
    size_t compressed = BrotliEncoderMaxCompressedSize(payload_length);
    unsigned char *result = calloc(1, 48u + directory_length + compressed + 3u);
    CHECK(result != NULL && BrotliEncoderCompress(4, BROTLI_DEFAULT_WINDOW,
        BROTLI_MODE_FONT, payload_length, payload, &compressed,
        result + 48u + directory_length));
    free(payload);
    store_be32(result, UINT32_C(0x774f4632));
    store_be32(result + 4, UINT32_C(0x00010000));
    *output_length = (48u + directory_length + compressed + 3u) & ~(size_t) 3u;
    store_be32(result + 8, (uint32_t) *output_length);
    result[12] = (unsigned char) (tables >> 8);result[13] = (unsigned char) tables;
    store_be32(result + 16, (uint32_t) length);
    store_be32(result + 20, (uint32_t) compressed);
    memcpy(result + 48u, directory, directory_length);
    *output = result;
    return true;
}

typedef struct {
    unsigned calls;
    unsigned refuse_at;
} Woff2Cooperation;

static bool cancel_woff2(void *opaque, const char *phase, size_t work)
{
    (void) work;
    if (strcmp(phase, "font-woff2") != 0 && strcmp(phase, "font-sfnt") != 0) return true;
    Woff2Cooperation *cooperation = opaque;
    cooperation->calls++;
    return cooperation->refuse_at == 0
        || cooperation->calls < cooperation->refuse_at;
}

static bool test_bounded_woff2(void)
{
    unsigned char *sfnt = NULL, *woff2 = NULL;
    size_t length = 0, encoded_length = 0;
    CHECK(read_file(TILEFINCH_TEST_SOURCE_DIR "/fonts/TilefinchSans-Regular.ttf", &sfnt, &length)
        && build_null_transform_woff2(sfnt, length, &woff2, &encoded_length));
    Budget budget;budget_init(&budget, 8u * MIB);
    FontProbe raw = {0}, encoded = {0};
    bool raw_ok = probe_web_font(&budget, sfnt, length, 4u * MIB, &raw);
    bool encoded_ok = probe_web_font(&budget, woff2, encoded_length, 4u * MIB, &encoded);
    CHECK(raw_ok && encoded_ok && font_probes_equal(&raw, &encoded));
    /* Decoder workspace must not be mistaken for retained face storage. */
    CHECK(probe_web_font(&budget, woff2, encoded_length, 256u*KIB, &encoded)
          && font_probes_equal(&raw, &encoded) && budget.current == 0);
    Woff2Cooperation cooperation = {0};
    TilefinchPlatformServices services = {
        .context = &cooperation, .cooperate = cancel_woff2
    };
    tilefinch_platform_set_services(&services);
    CHECK(probe_web_font(&budget, woff2, encoded_length, 4u * MIB, &encoded));
    unsigned checkpoints = cooperation.calls;
    CHECK(checkpoints > 1);
    /* Refuse at every checkpoint, including reconstruction after realloc.
       Each cancellation must roll back, and a later load must still work. */
    for (unsigned point = 1; point <= checkpoints; point++) {
        cooperation.calls = 0;
        cooperation.refuse_at = point;
        CHECK(rejected_without_retention(&budget, woff2, encoded_length, 4u * MIB));
        CHECK(cooperation.calls >= point && budget.current == 0);
    }
    tilefinch_platform_set_services(NULL);
    CHECK(budget.current == 0
        && probe_web_font(&budget, woff2, encoded_length, 4u * MIB, &encoded));
    CHECK(rejected_without_retention(&budget, woff2, encoded_length, length - 1u));
    for (size_t prefix = 0; prefix < 48; prefix++)
        CHECK(rejected_without_retention(&budget, woff2, prefix, MIB));
    CHECK(rejected_without_retention(&budget, woff2, encoded_length - 1u, MIB));
    uint32_t declared = read_be32(woff2 + 8);
    store_be32(woff2 + 8, (uint32_t) encoded_length - 1u);
    CHECK(rejected_without_retention(&budget, woff2, encoded_length - 1u, MIB));
    store_be32(woff2 + 8, declared);
    CHECK(sweep_font_allocation_failures(woff2, encoded_length, 4u * MIB));
    free(woff2);free(sfnt);
    CHECK(budget.current == 0);
    return true;
}
#endif

static bool test_bounded_font_loader(void)
{
    unsigned char *bytes = NULL;
    size_t length = 0;
    CHECK(read_file(TILEFINCH_TEST_SOURCE_DIR "/fonts/TilefinchSans-Regular.ttf",
                    &bytes, &length));
    CHECK(length > 64 && bytes[0] == 0 && bytes[1] == 1
          && bytes[2] == 0 && bytes[3] == 0);
    unsigned char *woff = NULL;
    size_t woff_length = 0;
    CHECK(build_uncompressed_woff(bytes, length, &woff, &woff_length));
    CHECK(woff != NULL && woff_length >= 44
          && read_be32(woff) == UINT32_C(0x774f4646)
          && read_be32(woff + 4) == UINT32_C(0x00010000)
          && read_be32(woff + 8) == woff_length
          && read_be32(woff + 16) <= 1u * MIB);
    Budget budget;
    budget_init(&budget, 4u * MIB);
    const size_t maximum_backend_bytes = 1u * MIB;

    for (size_t truncated = 0; truncated < 64; truncated++) {
        CHECK(rejected_without_retention(
            &budget, bytes, truncated, maximum_backend_bytes));
    }
    CHECK(rejected_without_retention(&budget, bytes, length, length - 1u));
    CHECK(test_woff_truncation_boundaries(
        &budget, woff, woff_length, maximum_backend_bytes));

    unsigned char malformed[64] = {0};
    memcpy(malformed, "wOFF", 4);
    store_be32(malformed + 4, UINT32_C(0x00010000));
    store_be32(malformed + 8, 44);
    store_be32(malformed + 16, 64);
    CHECK(rejected_without_retention(&budget, malformed, 44,
                                     maximum_backend_bytes));
    store_be32(malformed + 8, 45);
    CHECK(rejected_without_retention(&budget, malformed, 44,
                                     maximum_backend_bytes));
    store_be32(malformed + 8, 44);
    store_be32(malformed + 16, (uint32_t) maximum_backend_bytes + 1u);
    CHECK(rejected_without_retention(&budget, malformed, 44,
                                     maximum_backend_bytes));
    memcpy(malformed, "wOF2", 4);
    CHECK(rejected_without_retention(&budget, malformed, sizeof(malformed),
                                     maximum_backend_bytes));
    memcpy(malformed, "OTTO", 4);
    CHECK(rejected_without_retention(&budget, malformed, sizeof(malformed),
                                     maximum_backend_bytes));
    memcpy(malformed, "ttcf", 4);
    CHECK(rejected_without_retention(&budget, malformed, sizeof(malformed),
                                     maximum_backend_bytes));
    store_be32(malformed, UINT32_C(0x00010000));
    CHECK(rejected_without_retention(&budget, malformed, sizeof(malformed),
                                     maximum_backend_bytes));

    budget_inject_failure_after(&budget, 0);
    CHECK(rejected_without_retention(
        &budget, bytes, length, maximum_backend_bytes));
    budget_clear_failure_injection(&budget);

    size_t baseline = budget.current;
    FontFace face = {0};
#ifdef TILEFINCH_TEST_HAVE_FREETYPE
    CHECK(font_face_load_encoded(&face, &budget, bytes, length,
                                 maximum_backend_bytes));
    CHECK(face.loaded && face.backend == FONT_FACE_BACKEND_FREETYPE_WEB);
    CHECK(face.data != NULL && face.data != bytes
          && face.data_length == length && face.implementation != NULL);
    unsigned char *retained_data = face.data;
    void *retained_implementation = face.implementation;
    CHECK(!font_face_load_encoded(&face, &budget, bytes, length,
                                  maximum_backend_bytes));
    CHECK(face.data == retained_data
          && face.implementation == retained_implementation);
    CHECK(font_text_width(&face, "Tilefinch", 9, 16, false) > 0
          && font_metric_height(&face, 19) == 22
          /* Page faces use their design metric, not fallback minimum leading. */
          && font_line_height(&face, 19) == 22
          && font_line_baseline(&face, 40, 43)
                 == font_ascent(&face, 40) - 2);
    FontGlyph glyph = {0};
    CHECK(font_glyph_load(&face, 'A', 16, false, &glyph));
    CHECK(glyph.pixels != NULL && glyph.budget == &budget
          && glyph.width > 0 && glyph.height > 0);
    font_glyph_destroy(&face, &glyph);
    CHECK(test_web_glyph_coverage(&face, bytes, length));
    CHECK(test_web_font_raster_retention(&budget, bytes, length,
        maximum_backend_bytes));
    font_face_destroy(&face);
    font_face_destroy(&face);
    CHECK(budget.current == baseline);
    FontProbe raw_probe = {0}, woff_probe = {0};
    CHECK(probe_web_font(&budget, bytes, length, maximum_backend_bytes,
                         &raw_probe));
    CHECK(probe_web_font(&budget, woff, woff_length, maximum_backend_bytes,
                         &woff_probe));
    CHECK(font_probes_equal(&raw_probe, &woff_probe));

    unsigned char *compressed_woff = NULL;
    size_t compressed_length = 0;
    FontProbe compressed_probe = {0};
    CHECK(build_compressed_woff(bytes, length,
                                &compressed_woff, &compressed_length));
    CHECK(compressed_length < woff_length);
    CHECK(probe_web_font(&budget, compressed_woff, compressed_length,
                         maximum_backend_bytes, &compressed_probe));
    CHECK(font_probes_equal(&raw_probe, &compressed_probe));
    CHECK(budget.current == baseline);
    CHECK(test_woff_truncation_boundaries(
        &budget, compressed_woff, compressed_length, maximum_backend_bytes));
    uint16_t compressed_tables = read_be16(compressed_woff + 12);
    bool checked_bad_stream = false;
    for (size_t table = 0; table < compressed_tables; table++) {
        const unsigned char *record = compressed_woff + 44u + table * 20u;
        if (read_be32(record + 8) >= read_be32(record + 12)) continue;
        size_t offset = read_be32(record + 4);
        unsigned char saved = compressed_woff[offset];
        compressed_woff[offset] ^= 0xffu;
        CHECK(rejected_without_retention(
            &budget, compressed_woff, compressed_length, maximum_backend_bytes));
        compressed_woff[offset] = saved;
        checked_bad_stream = true;
        break;
    }
    CHECK(checked_bad_stream);
    CHECK(sweep_font_allocation_failures(
        compressed_woff, compressed_length, maximum_backend_bytes));
    free(compressed_woff);

    unsigned char *fractional_metric_bytes = NULL;
    size_t fractional_metric_length = 0;
    FontFace fractional_metric_face = {0};
    CHECK(read_file(TILEFINCH_TEST_SOURCE_DIR
                    "/fonts/DejaVuSans-Oblique-Latin.ttf",
                    &fractional_metric_bytes, &fractional_metric_length)
          && font_face_load_encoded(
              &fractional_metric_face, &budget, fractional_metric_bytes,
              fractional_metric_length, maximum_backend_bytes)
          /* The 2384-unit hhea box at 19px is 22.1171875px.  CSS layout
             needs the scalable ceiling (23), not FreeType's grid-fitted
             nearest-pixel size metric (22). */
          && font_metric_height(&fractional_metric_face, 19) == 23);
    font_face_destroy(&fractional_metric_face);
    free(fractional_metric_bytes);
    CHECK(budget.current == baseline);

    CHECK(sweep_font_allocation_failures(
        bytes, length, maximum_backend_bytes));
    CHECK(sweep_font_allocation_failures(
        woff, woff_length, maximum_backend_bytes));
#else
    CHECK(!font_face_load_encoded(&face, &budget, bytes, length,
                                  maximum_backend_bytes));
    CHECK(!font_face_load_encoded(&face, &budget, woff, woff_length,
                                  maximum_backend_bytes));
#endif
    font_face_destroy(&face);
    font_face_destroy(&face);
    CHECK(budget.current == baseline);
    free(woff);
    free(bytes);
    return true;
}

static bool test_font_request_destination(void)
{
    CHECK(TILEFINCH_DESTINATION_OTHER == 6);
    CHECK(TILEFINCH_DESTINATION_FONT == 7);
    TilefinchRequestContext request = {
        .target_url = "https://fonts.example/font.woff",
        .initiator_url = "https://document.example/page",
        .top_level_url = "https://document.example/page",
        .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_CORS,
        .credentials = TILEFINCH_CREDENTIALS_SAME_ORIGIN,
        .destination = TILEFINCH_DESTINATION_FONT
    };
    CHECK(tilefinch_request_context_valid(&request));
    CHECK(strcmp(tilefinch_request_fetch_destination(&request), "font") == 0);
    CHECK(strcmp(tilefinch_request_fetch_mode(&request), "cors") == 0);
    CHECK(strcmp(tilefinch_request_fetch_site(&request), "cross-site") == 0);
    CHECK(!tilefinch_request_sends_credentials(&request));
    request.target_url = "https://document.example/font.woff";
    CHECK(tilefinch_request_same_origin(&request));
    CHECK(tilefinch_request_sends_credentials(&request));
    CHECK(strcmp(tilefinch_request_fetch_destination(&request), "font") == 0);
    return true;
}

static bool test_cache_response_url_provenance(void)
{
    static const char request_url[] = "https://document.example/font.woff";
    static const unsigned char payload[] = {0, 1, 2, 3};
    Budget budget;
    budget_init(&budget, 512u * KIB);
    BrowserSession session;
    CHECK(browser_session_init(&session, &budget, 64u * KIB));
    CHECK(browser_session_cache_put_http(
        &session, request_url, payload, sizeof(payload), NULL, NULL,
        "font/woff", "max-age=60", NULL, 100));
    const BrowserCacheEntry *entry = browser_session_cache_lookup(
        &session, request_url);
    CHECK(entry != NULL && !entry->response_url_known
          && entry->response_url == NULL
          && strcmp(browser_cache_entry_response_url(entry), request_url)
                 == 0);
    CHECK(!browser_session_cache_set_response_url(
        &session, request_url, "data:font/woff;base64,AAAA"));
    entry = browser_session_cache_lookup(&session, request_url);
    CHECK(entry != NULL && !entry->response_url_known);

    /* Even a no-redirect response must affirmatively mark the provenance;
       the request-key fallback alone is not CORS evidence. */
    CHECK(browser_session_cache_set_response_url(
        &session, request_url, request_url));
    entry = browser_session_cache_lookup(&session, request_url);
    CHECK(entry != NULL && entry->response_url_known
          && entry->response_url == NULL
          && strcmp(browser_cache_entry_response_url(entry), request_url)
                 == 0);

    char redirected[] = "https://static.document.example/fonts/final.woff";
    CHECK(browser_session_cache_set_response_url(
        &session, request_url, redirected));
    redirected[8] = 'X';
    entry = browser_session_cache_lookup(&session, request_url);
    CHECK(entry != NULL && entry->response_url_known
          && entry->response_url != NULL
          && strcmp(browser_cache_entry_response_url(entry),
                    "https://static.document.example/fonts/final.woff") == 0);
    CHECK(browser_session_cache_revalidate(
        &session, request_url, "max-age=120", NULL, 200));
    entry = browser_session_cache_lookup(&session, request_url);
    CHECK(entry != NULL && entry->response_url_known
          && strcmp(browser_cache_entry_response_url(entry),
                    "https://static.document.example/fonts/final.woff") == 0);

    browser_session_destroy(&session);
    CHECK(budget.current == 0);
    return true;
}

static bool test_external_fonts_bypass_generic_cache(void)
{
    static const char html[] =
        "<!doctype html><style>"
        "@font-face{font-family:CachedFace;"
        "src:url(https://document.example/cached.ttf) format(truetype)}"
        ".target{font-family:CachedFace,sans-serif}"
        "</style><div class=target>cached</div>";
    static const char document_url[] = "https://document.example/page";
    static const char font_url[] = "https://document.example/cached.ttf";
    unsigned char *bytes = NULL;
    size_t length = 0;
    CHECK(read_file(TILEFINCH_TEST_SOURCE_DIR "/fonts/TilefinchSans-Regular.ttf",
                    &bytes, &length));

    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(budget_install_lexbor(&budget));
    BrowserSession session;
    CHECK(browser_session_init(&session, &budget, 512u * KIB));
    uint64_t now = tilefinch_platform_monotonic_time_ns();
    CHECK(browser_session_cache_put_http(
        &session, font_url, bytes, length, NULL, NULL, "font/ttf",
        "max-age=60", NULL, now));
    CHECK(browser_session_cache_set_response_url(
        &session, font_url, font_url));
    size_t cache_hits = session.cache_hits;
    size_t cache_misses = session.cache_misses;

    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 7));
    CHECK(stylesheet_build(&sheet, &budget, &document, 480));
    CHECK(stylesheet_web_font_source_count(&sheet) == 1);

    /* If fonts consulted the generic cache, this valid cached TTF would load
       and the injected transport failure would remain unconsumed. */
    fetch_inject_failure_once(FETCH_INJECT_TLS);
    ExternalFontStats stats = {0};
    CHECK(fonts_load_external(
        &sheet, &budget, document_url, document_url, "no-referrer",
        1, 256u * KIB, 128u * KIB, 1u * MIB, 1000, NULL, &session, NULL,
        &stats));
    fetch_inject_failure_once(FETCH_INJECT_NONE);
    CHECK(stats.sources_discovered == 1 && stats.attempted == 1
          && stats.loaded_faces == 0 && stats.failed == 1
          && stats.cache_hits == 0 && stats.encoded_bytes == 0);
    CHECK(session.cache_hits == cache_hits
          && session.cache_misses == cache_misses);
    FontFace *face = stylesheet_web_font_face(&sheet, 0, false);
    CHECK(face != NULL && !face->loaded && face->data == NULL
          && face->implementation == NULL);

    stylesheet_destroy(&sheet);
    document_destroy(&document);
    browser_session_destroy(&session);
    free(bytes);
    CHECK(budget.current == 0);
    CHECK(budget_uninstall_lexbor(&budget));
    return true;
}


#include "suites/font_gpos.inc"
#include "suites/font_variations.inc"

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--embedded-font-only") == 0)
        return test_embedded_font_loading() ? 0 : 1;
    if (argc == 2 && strcmp(argv[1], "--variations-only") == 0)
        return test_variable_font_descriptors() && test_variable_font_weights()
            && test_variable_weight_refusals() && test_discard_unconsumed_font_tables() ? 0 : 1;
    if (argc == 2 && strcmp(argv[1], "--opentype-kerning-only") == 0)
        return test_opentype_pair_kerning() ? 0 : 1;
    if (argc == 2 && strcmp(argv[1], "--font-transaction-only") == 0)
        return test_external_font_publication(false, false, EXTERNAL_FONT_PRESENTATION) ? 0 : 1;
    if (argc == 2 && strcmp(argv[1], "--font-presentation-only") == 0)
        return test_presentation_font_source_identity()
            && test_external_font_publication(false, false, EXTERNAL_FONT_PRESENTATION)
            && test_external_font_publication(false, true, EXTERNAL_FONT_ICON_PRESSURE) ? 0 : 1;
    if (argc == 2 && strcmp(argv[1], "--font-icon-pressure-only") == 0)
        return test_external_font_publication(false, true, EXTERNAL_FONT_ICON_PRESSURE) ? 0 : 1;
    if ((argc == 2 || argc == 3) && (strcmp(argv[1], "--font-perf-only") == 0
                     || strcmp(argv[1], "--legacy-font-perf-only") == 0)) {
        tilefinch_test_faults()->legacy_web_font_work =
            strcmp(argv[1], "--legacy-font-perf-only") == 0;
        if (argc == 3 && strcmp(argv[2], "aliases") == 0)
            return test_deferred_font_families_and_aliases() ? 0 : 1;
        if (argc == 3 && strcmp(argv[2], "advances") == 0)
            return test_exact_cached_web_font_advances() ? 0 : 1;
        return test_external_font_publication(false, false, EXTERNAL_FONT_STANDARD)
            && test_deferred_font_families_and_aliases()
            && test_exact_cached_web_font_advances() ? 0 : 1;
    }
#if defined(TILEFINCH_TEST_WOFF2) && defined(TILEFINCH_TEST_HAVE_FREETYPE)
    if (!test_bounded_woff2()) return 1;
#endif
    if (!test_embedded_font_loading()
        || !test_variable_font_descriptors() || !test_variable_font_weights()
        || !test_variable_weight_refusals()
        || !test_discard_unconsumed_font_tables()
        || !test_opentype_pair_kerning()
        || !test_total_font_source_bound()
        || !test_distinct_font_weights()
        || !test_bounded_font_loader()
        || !test_async_font_loader_is_fallback_first()
        || !test_deferred_font_families_and_aliases()
        || !test_deferred_font_weights(false)
        || !test_deferred_font_weights(true)
        || !test_exact_cached_web_font_advances()
        || !test_external_font_publication(false, false, EXTERNAL_FONT_STANDARD)
        || !test_external_font_publication(true, false, EXTERNAL_FONT_STANDARD)
        || !test_external_font_publication(false, true, EXTERNAL_FONT_STANDARD)
        || !test_external_font_publication(false, false, EXTERNAL_FONT_PRESENTATION)
        || !test_external_font_publication(false, true, EXTERNAL_FONT_ICON_PRESSURE)
        || !test_presentation_font_source_identity()
        || !test_web_font_modes()
        || !test_blocked_font_settles_without_backpressure_retry()
        || !test_inline_discovery_and_canonicalization()
        || !test_single_pass_forward_reference_reinterns()
        || !test_external_base_and_source_order()
        || !test_generated_icon_font_family()
        || !test_external_referrer_policy_identity()
        || !test_font_request_destination()
        || !test_cache_response_url_provenance()
        || !test_external_fonts_bypass_generic_cache()) return 1;
    puts("web-font-tests status=PASS");
    return 0;
}
