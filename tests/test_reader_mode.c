#include "tilefinch/reader_mode.h"
#include "tilefinch/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static bool build_listing(char *html, size_t capacity, bool watch,
                          bool date_paths, bool marker_collision,
                          bool excluded_region)
{
    size_t used = 0;
    if (!append_text(html, capacity, &used,
            watch
                ? "<!doctype html><head><meta property='og:type' content='video.other'></head><body><div id=app><main itemscope itemtype='https://schema.org/VideoObject'><h1>Watch title</h1><video src='/movie.mp4'></video><p>Watch description.</p></main><section>"
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
        watch ? "</section></div></body>"
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
    CHECK(build_listing(html, sizeof(html), true, false, false, false)
          && analyze(html, &analysis, &document, &budget));
    CHECK(analysis.kind == READER_PAGE_WATCH
          && analysis.high_confidence && analysis.listing_entries == 10u);
    CHECK(count_marked_elements(
              &document, "data-tilefinch-reader-article", false) == 1u
          && count_marked_elements(
                 &document, "data-tilefinch-reader-list", false) == 1u);
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

int main(void)
{
    CHECK(test_article());
    CHECK(test_listing_and_watch());
    CHECK(test_large_page_bound());
    puts("reader-mode-tests: ok");
    return 0;
}
