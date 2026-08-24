#include "tilefinch/reader_mode.h"
#include "tilefinch/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define CHECK(value) do { \
    if (!(value)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #value); \
        return false; \
    } \
} while (0)

static bool analyze(const char *html, ReaderDocumentAnalysis *analysis,
                    PocDocument *document, Budget *budget)
{
    return document_parse(document, budget, html, strlen(html), 113u)
        && reader_document_prepare(document, analysis);
}

static size_t count_named_elements(PocDocument *document, const char *name)
{
    lxb_dom_node_t *body = document_body_node(document);
    lxb_dom_node_t *boundary = body == NULL ? NULL : body->parent;
    lxb_dom_node_t *node = body;
    size_t count = 0;
    for (size_t visited = 0;
         node != NULL && node != boundary && visited < 4096u; visited++) {
        size_t length = 0;
        const char *actual = document_element_name(node, &length);
        if (actual != NULL && length == strlen(name)
            && strncasecmp(actual, name, length) == 0) count++;
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != NULL && node != boundary && node->next == NULL)
            node = node->parent;
        if (node != NULL && node != boundary) node = node->next;
    }
    return count;
}

static bool test_article(void)
{
    static const char html[] =
        "<!doctype html><body><header><nav>Sections</nav></header>"
        "<main><article><h1>A bounded article</h1>"
        "<p>This is a deliberately substantial paragraph with enough ordinary "
        "prose to contribute to the density score. It has complete sentences, "
        "few links, and useful reading content repeated for classification.</p>"
        "<p>This second substantial paragraph describes a portable system and "
        "keeps the dominant subtree dense while unrelated navigation remains "
        "short. It is intentionally plain text rather than generated chrome.</p>"
        "<p>This third substantial paragraph makes the article candidate stable "
        "under the minimum paragraph and text thresholds. The exact words do "
        "not matter; their concentration in one subtree does.</p>"
        "<p>A final substantial paragraph supplies the remainder of the reader "
        "body and includes <a href='/next'>one useful link</a> without making "
        "the article link-dense or changing its role.</p>"
        "<img src='data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///ywAAAAAAQABAAACAUwAOw==' "
        "alt='Placeholder must not overlap the article'>"
        "<figure><img src='/diagram.png' alt='A useful diagram'>"
        "<figcaption>Diagram caption</figcaption></figure>"
        "<table><caption>Values</caption><tr><th>Name</th><td>One</td></tr>"
        "</table><pre><code>bounded_example();</code></pre>"
        "</article></main><aside>Promoted links</aside></body>";
    Budget budget;
    budget_init(&budget, 32u * 1024u * 1024u);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    ReaderDocumentAnalysis analysis = {0};
    CHECK(analyze(html, &analysis, &document, &budget));
    CHECK(analysis.prepared && analysis.high_confidence
          && analysis.kind == READER_PAGE_ARTICLE
          && analysis.visible_text_bytes >= 600u);
    size_t length = 0;
    const char *kind = document_attribute(
        document_body_node(&document), "data-tilefinch-reader-kind", &length);
    CHECK(kind != NULL && length == 7u && memcmp(kind, "article", 7u) == 0);
    /* Raw and extracted semantic elements coexist; Reader CSS chooses the
       hidden extracted root without destroying the source page. */
    CHECK(count_named_elements(&document, "article") == 2u
          && count_named_elements(&document, "a") == 2u
          && count_named_elements(&document, "img") == 3u
          && count_named_elements(&document, "table") == 2u
          && count_named_elements(&document, "pre") == 2u);
    document_destroy(&document);
    CHECK(budget_uninstall_lexbor(&budget) && budget.current == 0
          && budget_categories_reconcile(&budget));
    return true;
}

static bool append_text(char *output, size_t capacity, size_t *used,
                        const char *text)
{
    size_t length = strlen(text);
    if (*used >= capacity || length >= capacity - *used) return false;
    memcpy(output + *used, text, length);
    *used += length;
    output[*used] = '\0';
    return true;
}

static bool build_listing(char *html, size_t capacity, unsigned media_mode,
                          bool date_paths, bool marker_collision,
                          bool excluded_region)
{
    size_t used = 0;
    if (!append_text(html, capacity, &used,
            media_mode == 1u
                ? "<!doctype html><head><meta property='og:type' content='video.other'></head><body><div id=app><main itemscope itemtype='https://schema.org/VideoObject'><h1>Watch title</h1><video src='/movie.mp4'></video><p>Watch description.</p></main><section>"
                : media_mode == 2u
                    ? "<!doctype html><head><meta property='og:type' content='video.other'><script type='application/ld+json'>{\"@type\":\"VideoObject\",\"contentUrl\":\"/promo.mp4\"}</script></head><body><header>Browse</header><section>"
                : excluded_region
                    ? "<!doctype html><body><header>Browse</header><nav>"
                    : "<!doctype html><body><header>Browse</header><section>"))
        return false;
    for (unsigned i = 0; i < 10u; i++) {
        char entry[512];
        int written = date_paths
            ? snprintf(
                  entry, sizeof(entry),
                  "<article><a href='/archive/2026/08/slug-%02u'><img data-src='/thumb-%u.jpg' alt='Archive'></a><a href='/archive/2026/08/slug-%02u' title='Archive %u'><span class='quality-badge'>HD</span></a><span>1:2%u 10 views</span></article>",
                  i + 1u, i, i + 1u, i, i)
            : snprintf(
                  entry, sizeof(entry),
                  "<article><a href='/video-%u'><img src='data:image/gif;base64,AAAA' data-thumb='/thumb-%u.jpg' alt='Clip %u'></a><a href='/video-%u' title='Clip %u'%s><span class='quality-badge'>1080p</span></a><span>1:2%u 10 views</span></article>",
                  i, i, i, i, i,
                  marker_collision && i == 5u
                      ? " data-tilefinch-reader-title='foreign'" : "", i);
        if (written < 0 || (size_t) written >= sizeof(entry)
            || !append_text(html, capacity, &used, entry)) return false;
    }
    return append_text(
        html, capacity, &used,
        media_mode == 1u ? "</section></div></body>"
              : excluded_region ? "</nav></body>" : "</section></body>");
}

static size_t count_marked_elements(PocDocument *document,
                                    const char *attribute,
                                    bool require_clip_label)
{
    lxb_dom_node_t *body = document_body_node(document);
    lxb_dom_node_t *boundary = body == NULL ? NULL : body->parent;
    lxb_dom_node_t *node = body;
    size_t count = 0;
    size_t visited = 0;
    while (node != NULL && node != boundary && visited++ < 4096u) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            size_t length = 0;
            const char *value = document_attribute(node, attribute, &length);
            if (value != NULL) {
                if (require_clip_label
                    && !(length >= 5u && memcmp(value, "Clip ", 5u) == 0))
                    return SIZE_MAX;
                count++;
            }
        }
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        for (;;) {
            if (node->next != NULL) {
                node = node->next;
                break;
            }
            node = node->parent;
            if (node == NULL || node == boundary) break;
        }
    }
    return count;
}

static bool test_listing_and_watch(void)
{
    char html[8192];
    Budget budget;
    budget_init(&budget, 32u * 1024u * 1024u);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    ReaderDocumentAnalysis analysis = {0};
    CHECK(build_listing(html, sizeof(html), false, false, false, false)
          && analyze(html, &analysis, &document, &budget));
    CHECK(analysis.kind == READER_PAGE_LISTING
          && analysis.high_confidence && analysis.listing_entries == 10u);
    CHECK(count_marked_elements(
              &document, "data-tilefinch-reader-entry", false) == 10u
          && count_marked_elements(
                 &document, "data-tilefinch-reader-thumb", false) == 10u
          && count_marked_elements(
                 &document, "data-tilefinch-reader-label", true) == 10u);
    document_destroy(&document);
    CHECK(budget.current == 0);

    memset(&analysis, 0, sizeof(analysis));
    CHECK(build_listing(html, sizeof(html), 2u, false, false, false)
          && analyze(html, &analysis, &document, &budget));
    /* Head-only promotional/watch metadata cannot prove a watch root, but it
       does veto listing auto-engagement: a script-injected player may share
       the page with a recommendations cluster. */
    CHECK(analysis.kind != READER_PAGE_LISTING
          && !analysis.high_confidence && analysis.listing_entries == 0u);
    document_destroy(&document);
    CHECK(budget.current == 0);

    memset(&analysis, 0, sizeof(analysis));
    CHECK(build_listing(html, sizeof(html), true, false, false, false)
          && analyze(html, &analysis, &document, &budget));
    CHECK(analysis.kind == READER_PAGE_WATCH
          && analysis.high_confidence && analysis.listing_entries == 10u);
    CHECK(count_marked_elements(
              &document, "data-tilefinch-reader-article", false) == 1u
          && count_marked_elements(
                 &document, "data-tilefinch-reader-root", false) == 1u);
    document_destroy(&document);
    CHECK(budget.current == 0);

    memset(&analysis, 0, sizeof(analysis));
    CHECK(build_listing(html, sizeof(html), false, true, false, false)
          && analyze(html, &analysis, &document, &budget));
    CHECK(analysis.kind == READER_PAGE_RAW
          && !analysis.high_confidence && analysis.listing_entries == 0u);
    document_destroy(&document);
    CHECK(budget.current == 0);

    memset(&analysis, 0, sizeof(analysis));
    CHECK(build_listing(html, sizeof(html), false, false, true, false)
          && analyze(html, &analysis, &document, &budget));
    size_t kind_length = 0;
    CHECK(analysis.kind == READER_PAGE_RAW && analysis.bounded_out
          && document_attribute(
                 document_body_node(&document),
                 "data-tilefinch-reader-kind", &kind_length) == NULL
          && count_marked_elements(
                 &document, "data-tilefinch-reader-entry", false) == 0u
          && count_marked_elements(
                 &document, "data-tilefinch-reader-list", false) == 0u
          && count_marked_elements(
                 &document, "data-tilefinch-reader-title", false) == 1u);
    document_destroy(&document);
    CHECK(budget.current == 0);

    memset(&analysis, 0, sizeof(analysis));
    CHECK(build_listing(html, sizeof(html), false, false, false, true)
          && analyze(html, &analysis, &document, &budget));
    CHECK(analysis.kind == READER_PAGE_RAW
          && analysis.listing_entries == 0u
          && count_marked_elements(
                 &document, "data-tilefinch-reader-entry", false) == 0u);
    document_destroy(&document);
    CHECK(budget_uninstall_lexbor(&budget) && budget.current == 0
          && budget_categories_reconcile(&budget));
    return true;
}

static bool test_audio_page_precedes_listing(void)
{
    char html[8192];
    size_t used = 0;
    CHECK(append_text(
        html, sizeof(html), &used,
        "<!doctype html><head><script type='application/ld+json'>"
        "{\"@type\":\"MusicRecording\",\"name\":\"Episode\","
        "\"audio\":{\"@type\":\"AudioObject\","
        "\"contentUrl\":\"/episode.m4a\"}}</script></head>"
        "<body><div id=app><main><h1>Episode title</h1>"
        "<button>Play episode</button><p>Episode notes remain primary.</p>"
        "</main><section>"));
    for (unsigned i = 0; i < 10u; i++) {
        char entry[384];
        int written = snprintf(
            entry, sizeof(entry),
            "<article><a href='/video-%u'><img src='/thumb-%u.jpg' "
            "alt='Related %u'></a><a href='/video-%u' title='Related %u'>"
            "Related %u</a><span>1:2%u 10 views</span></article>",
            i, i, i, i, i, i, i);
        CHECK(written > 0 && (size_t) written < sizeof(entry)
              && append_text(html, sizeof(html), &used, entry));
    }
    CHECK(append_text(html, sizeof(html), &used,
                      "</section></div></body>"));

    Budget budget;
    budget_init(&budget, 32u * 1024u * 1024u);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    ReaderDocumentAnalysis analysis = {0};
    CHECK(analyze(html, &analysis, &document, &budget));
    /* Structured media metadata is a discovery hint, not proof that the
       page's primary visible content is a player. */
    CHECK(analysis.kind == READER_PAGE_LISTING
          && analysis.high_confidence && analysis.listing_entries == 10u);
    CHECK(count_marked_elements(
              &document, "data-tilefinch-reader-article", false) == 0u
          && count_marked_elements(
                 &document, "data-tilefinch-reader-list", false) == 1u);
    document_destroy(&document);
    CHECK(budget.current == 0);

    used = 0;
    CHECK(append_text(
        html, sizeof(html), &used,
        "<!doctype html><body><div id=app><main><h1>Authored audio</h1>"
        "<audio controls src='/episode.m4a'></audio><p>Episode notes.</p>"
        "</main><section>"));
    for (unsigned i = 0; i < 8u; i++) {
        char entry[384];
        int written = snprintf(
            entry, sizeof(entry),
            "<article><a href='/video-%u'><img src='/thumb-%u.jpg' "
            "alt='Related %u'></a><span>1:2%u 10 views</span></article>",
            i, i, i, i);
        CHECK(written > 0 && (size_t) written < sizeof(entry)
              && append_text(html, sizeof(html), &used, entry));
    }
    CHECK(append_text(html, sizeof(html), &used,
                      "</section></div></body>")
          && analyze(html, &analysis, &document, &budget));
    CHECK(analysis.kind == READER_PAGE_WATCH
          && analysis.high_confidence && analysis.listing_entries == 8u
          && count_marked_elements(
                 &document, "data-tilefinch-reader-article", false) == 1u);
    document_destroy(&document);
    CHECK(budget_uninstall_lexbor(&budget) && budget.current == 0
          && budget_categories_reconcile(&budget));
    return true;
}

static bool test_hidden_or_peripheral_media_is_not_primary(void)
{
    static const char html[] =
        "<!doctype html><body><main><h1>Product landing page</h1>"
        "<p>This is a substantial server-rendered introduction with useful "
        "content, complete sentences, and no primary playback experience.</p>"
        "<p>A second paragraph keeps the body readable and article-shaped "
        "without turning its promotional media into a watch page.</p>"
        "<p>A third paragraph makes the semantic content unambiguous.</p>"
        "</main><aside><video controls src='/promotion.mp4'></video></aside>"
        "<section hidden><h2>Hidden player</h2>"
        "<video controls src='/hidden.mp4'></video></section></body>";
    Budget budget;
    budget_init(&budget, 32u * 1024u * 1024u);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    ReaderDocumentAnalysis analysis = {0};
    CHECK(analyze(html, &analysis, &document, &budget));
    CHECK(analysis.kind != READER_PAGE_WATCH);
    document_destroy(&document);
    CHECK(budget_uninstall_lexbor(&budget) && budget.current == 0
          && budget_categories_reconcile(&budget));
    return true;
}

static bool test_large_page_bound(void)
{
    const size_t capacity = 1280u * 1024u;
    char *html = malloc(capacity);
    CHECK(html != NULL);
    size_t used = 0;
    CHECK(append_text(html, capacity, &used,
                      "<!doctype html><body><main><article><h1>Large page</h1>"));
    static const char paragraph[] =
        "<p>Dense readable text stays inside one bounded subtree while the "
        "classifier visits each node once and releases its scratch table. "
        "The content is intentionally repetitive because this fixture measures "
        "the traversal and storage bound rather than linguistic quality. It also "
        "keeps links and navigation out of the winning text region so scoring "
        "remains deterministic across optimized host builds.</p>";
    for (size_t i = 0; i < 3000u; i++)
        CHECK(append_text(html, capacity, &used, paragraph));
    CHECK(append_text(html, capacity, &used, "</article></main></body>"));
    Budget budget;
    budget_init(&budget, 32u * 1024u * 1024u);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    CHECK(document_parse(&document, &budget, html, used, 4096u));
    ReaderDocumentAnalysis analysis = {0};
    uint64_t started = tilefinch_platform_monotonic_time_ns();
    CHECK(reader_document_prepare(&document, &analysis));
    uint64_t elapsed_us =
        (tilefinch_platform_monotonic_time_ns() - started) / 1000u;
    fprintf(stderr, "reader-large: html=%zu nodes=%u elapsed-us=%llu\n",
            used, analysis.visited_nodes,
            (unsigned long long) elapsed_us);
    CHECK(!analysis.bounded_out && analysis.kind == READER_PAGE_ARTICLE
          && analysis.visited_nodes < 8192u
          && elapsed_us < UINT64_C(500000));
    document_destroy(&document);
    free(html);
    CHECK(budget_uninstall_lexbor(&budget) && budget.current == 0
          && budget_categories_reconcile(&budget));
    return true;
}

static bool test_bounded_page_keeps_manual_article(void)
{
    const size_t capacity = 2u * 1024u * 1024u;
    char *html = malloc(capacity);
    CHECK(html != NULL);
    size_t used = 0;
    CHECK(append_text(html, capacity, &used,
                      "<!doctype html><body><main><article>"
                      "<h1>Bounded article</h1>"));
    static const char paragraph[] =
        "<p>This readable paragraph belongs to the primary article and "
        "remains useful when a very large document reaches the analyzer's "
        "fixed traversal ceiling. Links, headings, and semantic structure "
        "must remain available to an explicit Reader request.</p>";
    for (size_t i = 0; i < 5000u; i++)
        CHECK(append_text(html, capacity, &used, paragraph));
    CHECK(append_text(html, capacity, &used, "</article></main></body>"));

    Budget budget;
    budget_init(&budget, 32u * 1024u * 1024u);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    CHECK(document_parse(&document, &budget, html, used, 4096u));
    ReaderDocumentAnalysis analysis = {0};
    CHECK(reader_document_prepare(&document, &analysis));
    CHECK(analysis.bounded_out && analysis.kind == READER_PAGE_ARTICLE
          && !analysis.high_confidence
          && analysis.visited_nodes == 8193u
          && count_marked_elements(
                 &document, "data-tilefinch-reader-root", false) == 1u);
    document_destroy(&document);
    free(html);
    CHECK(budget_uninstall_lexbor(&budget) && budget.current == 0
          && budget_categories_reconcile(&budget));
    return true;
}

int main(void)
{
    CHECK(test_article());
    CHECK(test_listing_and_watch());
    CHECK(test_audio_page_precedes_listing());
    CHECK(test_hidden_or_peripheral_media_is_not_primary());
    CHECK(test_large_page_bound());
    CHECK(test_bounded_page_keeps_manual_article());
    puts("reader-mode-tests: ok");
    return 0;
}
