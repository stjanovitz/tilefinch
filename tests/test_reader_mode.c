#include "tilefinch/reader_mode.h"
#include "tilefinch/platform.h"
#include "tilefinch/site_adapter.h"
#include "tilefinch/style.h"

#include <ctype.h>
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
         node != NULL && node != boundary && visited < 65536u; visited++) {
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

static lxb_dom_node_t *find_reader_root(PocDocument *document)
{
    lxb_dom_node_t *body = document_body_node(document);
    lxb_dom_node_t *boundary = body == NULL ? NULL : body->parent;
    lxb_dom_node_t *node = body;
    for (size_t visited = 0;
         node != NULL && node != boundary && visited < 65536u; visited++) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            size_t length = 0;
            if (document_attribute(
                    node, "data-tilefinch-reader-root", &length) != NULL)
                return node;
        }
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != NULL && node != boundary && node->next == NULL)
            node = node->parent;
        if (node != NULL && node != boundary) node = node->next;
    }
    return NULL;
}

static lxb_dom_node_t *find_named_within(lxb_dom_node_t *root,
                                         const char *name)
{
    lxb_dom_node_t *node = root;
    for (size_t visited = 0; node != NULL && visited < 65536u; visited++) {
        size_t length = 0;
        const char *actual = document_element_name(node, &length);
        if (actual != NULL && length == strlen(name)
            && strncasecmp(actual, name, length) == 0) return node;
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != NULL && node != root && node->next == NULL)
            node = node->parent;
        if (node == root) break;
        if (node != NULL) node = node->next;
    }
    return NULL;
}

static bool has_attribute(lxb_dom_node_t *node, const char *name)
{
    return node != NULL && node->type == LXB_DOM_NODE_TYPE_ELEMENT
        && lxb_dom_element_has_attribute(
               lxb_dom_interface_element(node),
               (const lxb_char_t *) name, strlen(name));
}

static size_t count_marked_elements(PocDocument *document,
                                    const char *attribute,
                                    bool require_clip_label);

static lxb_dom_node_t *find_direct_reader_root(PocDocument *document)
{
    lxb_dom_node_t *body = document_body_node(document);
    for (lxb_dom_node_t *node = body == NULL ? NULL : body->last_child;
         node != NULL; node = node->prev) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT
            && has_attribute(node, "data-tilefinch-reader-root")) return node;
    }
    return NULL;
}

static bool subtree_contains_text(lxb_dom_node_t *root, const char *needle)
{
    lxb_dom_node_t *node = root;
    size_t wanted = strlen(needle);
    for (size_t visited = 0; node != NULL && visited < 65536u; visited++) {
        if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
            size_t length = 0;
            const char *text = document_text_data(node, &length);
            if (text != NULL && length >= wanted) {
                for (size_t i = 0; i <= length - wanted; i++)
                    if (memcmp(text + i, needle, wanted) == 0) return true;
            }
        }
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != NULL && node != root && node->next == NULL)
            node = node->parent;
        if (node == root) break;
        if (node != NULL) node = node->next;
    }
    return false;
}

static size_t count_named_within(lxb_dom_node_t *root, const char *name)
{
    lxb_dom_node_t *node = root;
    size_t count = 0;
    for (size_t visited = 0; node != NULL && visited < 65536u; visited++) {
        size_t length = 0;
        const char *actual = document_element_name(node, &length);
        if (actual != NULL && length == strlen(name)
            && strncasecmp(actual, name, length) == 0) count++;
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != NULL && node != root && node->next == NULL)
            node = node->parent;
        if (node == root) break;
        if (node != NULL) node = node->next;
    }
    return count;
}

static lxb_dom_node_t *find_attribute_value_within(
    lxb_dom_node_t *root, const char *attribute, const char *wanted)
{
    lxb_dom_node_t *node = root;
    size_t wanted_length = strlen(wanted);
    for (size_t visited = 0; node != NULL && visited < 65536u; visited++) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            size_t length = 0;
            const char *value = document_attribute(node, attribute, &length);
            if (value != NULL && length == wanted_length
                && memcmp(value, wanted, wanted_length) == 0) return node;
        }
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != NULL && node != root && node->next == NULL)
            node = node->parent;
        if (node == root) break;
        if (node != NULL) node = node->next;
    }
    return NULL;
}

static size_t count_attribute_value_within(
    lxb_dom_node_t *root, const char *attribute, const char *wanted)
{
    lxb_dom_node_t *node = root;
    size_t wanted_length = wanted == NULL ? 0u : strlen(wanted);
    size_t count = 0u;
    for (size_t visited = 0; node != NULL && visited < 65536u; visited++) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            size_t length = 0;
            const char *value = document_attribute(node, attribute, &length);
            if ((wanted == NULL && has_attribute(node, attribute))
                || (wanted != NULL && value != NULL
                    && length == wanted_length
                    && memcmp(value, wanted, wanted_length) == 0))
                count++;
        }
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != NULL && node != root && node->next == NULL)
            node = node->parent;
        if (node == root) break;
        if (node != NULL) node = node->next;
    }
    return count;
}

static bool subtree_text_contains_words(lxb_dom_node_t *root,
                                        const char *words)
{
    char text[4096];
    size_t used = 0;
    bool pending_space = false;
    lxb_dom_node_t *node = root;
    for (size_t visited = 0; node != NULL && visited < 65536u; visited++) {
        if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
            size_t length = 0;
            const char *data = document_text_data(node, &length);
            for (size_t i = 0; data != NULL && i < length; i++) {
                unsigned char value = (unsigned char) data[i];
                if (isspace(value)) {
                    pending_space = used != 0;
                    continue;
                }
                if (pending_space && used < sizeof(text) - 1u)
                    text[used++] = ' ';
                pending_space = false;
                if (used < sizeof(text) - 1u) text[used++] = (char) value;
            }
        }
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != NULL && node != root && node->next == NULL)
            node = node->parent;
        if (node == root) break;
        if (node != NULL) node = node->next;
    }
    text[used] = '\0';
    return strstr(text, words) != NULL;
}

static bool test_reading_identity_and_semantic_selection(void)
{
    const char *paragraph = "<p>OPENING PROSE. This complete paragraph supplies ordinary "
        "article text with enough sentences to distinguish a genuine reading "
        "body from link-heavy recommendations, captions, or unrelated page chrome.</p>";
    char html[8192];
    size_t used = (size_t) snprintf(html, sizeof(html),
        "<!doctype html><body><main><article><figure><img src='/hero.png'>"
        "<figcaption>Lead caption</figcaption></figure><h1>READING TITLE</h1>"
        "<a rel='nofollow author' href='/author'>READING AUTHOR</a>%s%s%s%s"
        "<p>FINAL BODY SENTINEL.</p></article><section>",
        paragraph, paragraph, paragraph, paragraph);
    for (unsigned i = 0; i < 3; i++) {
        int n = snprintf(html + used, sizeof(html) - used,
            "<p>UNRELATED RECOMMENDATION. %s</p>", paragraph);
        CHECK(n > 0 && (size_t) n < sizeof(html) - used);
        used += (size_t) n;
    }
    CHECK(used + strlen("</section></main></body>") < sizeof(html));
    strcpy(html + used, "</section></main></body>");
    Budget budget;
    budget_init(&budget, 32u * 1024u * 1024u);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    ReaderDocumentAnalysis analysis = {0};
    CHECK(analyze(html, &analysis, &document, &budget));
    lxb_dom_node_t *root = find_reader_root(&document);
    CHECK(analysis.kind == READER_PAGE_ARTICLE && root != NULL
        && count_named_within(root, "h1") == 1u
        && count_named_within(root, "p") == 5u
        && !subtree_contains_text(root, "UNRELATED RECOMMENDATION")
        && subtree_contains_text(root, "FINAL BODY SENTINEL"));
    lxb_dom_node_t *article = find_named_within(root, "article");
    lxb_dom_node_t *first = article == NULL ? NULL : article->first_child;
    CHECK(first != NULL && subtree_contains_text(first, "READING TITLE")
        && first->next != NULL && subtree_contains_text(first->next, "READING AUTHOR")
        && first->next->next != NULL
        && subtree_contains_text(first->next->next, "OPENING PROSE"));
    document_destroy(&document);
    CHECK(budget_uninstall_lexbor(&budget) && budget.current == 0);
    return true;
}

static bool test_basic_layout_tables_and_embedded_images(void)
{
    const char html[] = "<!doctype html><body><table><tr><td><h1>Page heading</h1>"
        "<table><caption>Actual data</caption><tr><th scope=col>Key</th>"
        "<td>Value</td></tr></table></td><td><p>Side content</p></td></tr></table>"
        "<table role=presentation><tr><td><a href='/one'>First</a></td><td></td>"
        "<td><a href='/two'>Second</a></td></tr></table>"
        "<table><tr><td>10</td><td>20</td></tr></table>"
        "<table><tr><td><img src='/logo.png'></td><td><a href='/a'>A</a>"
        "<a href='/b'>B</a><a href='/c'>C</a><a href='/d'>D</a></td></tr></table>"
        "<img src='data:image/svg+xml,%3Csvg%20xmlns=%22http://www.w3.org/2000/svg%22%20width=%2220%22%20height=%2220%22/%3E'>"
        "<img src='data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///ywAAAAAAQABAAACAUwAOw==' data-src='/real.png'>"
        "</body>";
    Budget budget;
    budget_init(&budget, 32u * 1024u * 1024u);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    ReaderDocumentAnalysis analysis = {0};
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 113u)
        && reader_document_prepare_basic_with_stylesheet(&document, NULL, &analysis));
    lxb_dom_node_t *root = find_reader_root(&document);
    CHECK(root != NULL && analysis.kind == READER_PAGE_BASIC
        && count_named_within(root, "table") == 2u
        && count_named_within(root, "th") == 1u
        && count_named_within(root, "a") == 6u
        && count_named_within(root, "img") == 3u);
    lxb_dom_node_t *image = find_attribute_value_within(root, "src",
        "data:image/svg+xml,%3Csvg%20xmlns=%22http://www.w3.org/2000/svg%22%20width=%2220%22%20height=%2220%22/%3E");
    size_t length = 0;
    const char *source = document_attribute(image, "src", &length);
    CHECK(source != NULL && length > 19u && memcmp(source, "data:image/svg+xml,", 19u) == 0);
    CHECK(find_attribute_value_within(root, "src", "/real.png") != NULL);
    document_destroy(&document);
    CHECK(budget_uninstall_lexbor(&budget) && budget.current == 0);
    return true;
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

static bool test_refresh_refusal_rolls_back_extracted_reader(void)
{
    static const char html[] =
        "<!doctype html><title>Transactional Reader</title><body>"
        "<main><article><h1>Transactional Reader</h1>"
        "<p>This substantial first paragraph supplies ordinary article prose "
        "and establishes a deterministic semantic Reader candidate.</p>"
        "<p>This second substantial paragraph keeps the selected article "
        "dense while the transaction test targets only final metadata refresh."
        "</p><p>This third substantial paragraph ensures the semantic root "
        "remains well above the useful-content threshold for extraction.</p>"
        "<p>This fourth substantial paragraph completes the article shape and "
        "must remain exactly as authored after an injected refresh refusal.</p>"
        "<p>This fifth substantial paragraph adds enough concentrated prose "
        "to exceed the article density threshold without relying on links, "
        "navigation, generated chrome, or any hidden duplicate content.</p>"
        "<p>This final substantial paragraph makes classification independent "
        "of the allocation experiment and gives the selected article a clear "
        "dominant share of all visible words in the document.</p>"
        "</article></main></body>";
    Budget budget;
    budget_init(&budget, 32u * 1024u * 1024u);
    CHECK(budget_install_lexbor(&budget));

    /* Measure the deterministic prepare allocation sequence so the second
       run refuses document_refresh's final replacement-title allocation,
       after the clone and all Reader markers have been installed. */
    PocDocument probe = {0};
    ReaderDocumentAnalysis probe_analysis = {0};
    CHECK(document_parse(
              &probe, &budget, html, sizeof(html) - 1u, 113u));
    size_t prepare_allocation_start = budget.allocation_count;
    CHECK(reader_document_prepare(&probe, &probe_analysis)
          && probe_analysis.kind == READER_PAGE_ARTICLE
          && find_direct_reader_root(&probe) != NULL);
    size_t prepare_allocations =
        budget.allocation_count - prepare_allocation_start;
    CHECK(prepare_allocations != 0u);
    document_destroy(&probe);
    CHECK(budget.current == 0u);

    PocDocument document = {0};
    ReaderDocumentAnalysis analysis = {0};
    CHECK(document_parse(
              &document, &budget, html, sizeof(html) - 1u, 113u));
    size_t original_nodes = document.node_count;
    size_t original_elements = document.element_count;
    size_t original_attributes = document.attribute_count;
    uint64_t original_generation = document.content_generation;
    size_t injected_before = budget.injected_failure_count;
    budget_inject_failure_after(&budget, prepare_allocations - 1u);
    bool prepared = reader_document_prepare(&document, &analysis);
    budget_clear_failure_injection(&budget);
    lxb_dom_node_t *body = document_body_node(&document);
    CHECK(prepared && analysis.prepared
          && analysis.kind == READER_PAGE_RAW && analysis.bounded_out
          && budget.injected_failure_count == injected_before + 1u
          && find_direct_reader_root(&document) == NULL
          && body != NULL
          && !has_attribute(body, "data-tilefinch-reader-kind")
          && count_marked_elements(
                 &document, "data-tilefinch-reader-article", false) == 0u
          && count_marked_elements(
                 &document, "data-tilefinch-reader-path", false) == 0u
          && document.node_count == original_nodes
          && document.element_count == original_elements
          && document.attribute_count == original_attributes
          && document.content_generation == original_generation
          && document.title != NULL
          && strcmp(document.title, "Transactional Reader") == 0);
    document_destroy(&document);
    CHECK(budget_uninstall_lexbor(&budget) && budget.current == 0u
          && budget_categories_reconcile(&budget));
    return true;
}

static bool test_static_fallback_respects_bare_hidden_body(void)
{
    static const char html[] =
        "<!doctype html><body hidden><main>"
        "<h1>Explicitly hidden server document</h1><p>"
        "This complete text-rich server document deliberately uses the bare "
        "boolean hidden attribute, which static degradation must preserve even "
        "when optional hydration can no longer run.</p></main></body>";
    Budget budget;
    budget_init(&budget, 32u * 1024u * 1024u);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    CHECK(document_parse(
              &document, &budget, html, sizeof(html) - 1u, 113u)
          && stylesheet_build(&stylesheet, &budget, &document, 480));
    lxb_dom_node_t *body = document_body_node(&document);
    CHECK(body != NULL && has_attribute(body, "hidden"));
    stylesheet_enable_static_custom_element_fallback(&stylesheet);
    ComputedStyle style = style_for_node(&stylesheet, body, NULL);
    CHECK(style.display == DISPLAY_NONE);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    CHECK(budget_uninstall_lexbor(&budget) && budget.current == 0u
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

typedef struct {
    size_t analyze_text_calls;
    size_t extract_calls;
    size_t extract_node_calls;
    size_t analyze_last;
    size_t extract_last;
    size_t extract_node_last;
    size_t analyze_max_gap;
    size_t extract_max_gap;
    size_t extract_node_max_gap;
} ReaderCooperateProbe;

static bool reader_cooperate_probe(void *context, const char *phase,
                                   size_t completed_work_units)
{
    ReaderCooperateProbe *probe = context;
    if (probe == NULL || phase == NULL) return true;
    size_t *calls = NULL;
    size_t *last = NULL;
    size_t *max_gap = NULL;
    if (strcmp(phase, "reader-analyze-text") == 0) {
        calls = &probe->analyze_text_calls;
        last = &probe->analyze_last;
        max_gap = &probe->analyze_max_gap;
    } else if (strcmp(phase, "reader-extract") == 0) {
        calls = &probe->extract_calls;
        last = &probe->extract_last;
        max_gap = &probe->extract_max_gap;
    } else if (strcmp(phase, "reader-extract-nodes") == 0) {
        calls = &probe->extract_node_calls;
        last = &probe->extract_node_last;
        max_gap = &probe->extract_node_max_gap;
    } else {
        return true;
    }
    size_t gap = completed_work_units >= *last
        ? completed_work_units - *last : SIZE_MAX;
    if (gap > *max_gap) *max_gap = gap;
    *last = completed_work_units;
    (*calls)++;
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
                      ? " data-tilefinch-reader-title" : "", i);
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
            if (has_attribute(node, attribute)) {
                if (require_clip_label
                    && !(value != NULL && length >= 5u
                         && memcmp(value, "Clip ", 5u) == 0))
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

static bool test_empty_listing_extraction_falls_back_to_raw(void)
{
    char html[8192];
    size_t used = 0;
    CHECK(append_text(
        html, sizeof(html), &used,
        "<!doctype html><body><main><h1>Video index</h1><section>"));
    for (unsigned i = 0; i < 8u; i++) {
        char entry[512];
        int written = snprintf(
            entry, sizeof(entry),
            "<article><a href='/video-%u' title='Clip %u'>"
            "<img src='data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///ywAAAAAAQABAAACAUwAOw=='></a></article>",
            i, i);
        CHECK(written > 0 && (size_t) written < sizeof(entry)
              && append_text(html, sizeof(html), &used, entry));
    }
    CHECK(append_text(html, sizeof(html), &used,
                      "</section></main></body>"));

    Budget budget;
    budget_init(&budget, 32u * 1024u * 1024u);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    ReaderDocumentAnalysis analysis = {0};
    CHECK(analyze(html, &analysis, &document, &budget));
    /* Placeholder-only cards can satisfy the bounded listing classifier,
       but they produce no visible semantic clone. Keep the source DOM usable
       through conservative raw reflow instead of installing an empty root. */
    CHECK(analysis.kind == READER_PAGE_RAW && !analysis.high_confidence
          && analysis.listing_entries == 0u
          && find_reader_root(&document) == NULL);
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

static bool test_primary_media_requires_visible_authored_source(void)
{
    static const char controls_only[] =
        "<!doctype html><body><main><article><h1>Episode notes</h1>"
        "<video controls poster='/cover.jpg'></video>"
        "<p>This article describes an episode whose player is injected later, "
        "so controls and a poster alone must not turn it into a watch page.</p>"
        "<p>The useful server-rendered prose remains eligible for Reader.</p>"
        "<p>A third paragraph makes the authored article shape explicit.</p>"
        "</article></main></body>";
    static const char stylesheet_hidden[] =
        "<!doctype html><head><style>.deferred-player{display:none}</style>"
        "</head><body><main><article><h1>Written feature</h1>"
        "<video class='deferred-player' controls src='/promo.mp4'></video>"
        "<p>This titled feature has a stylesheet-hidden promotional player "
        "which is not a visible primary playback experience.</p>"
        "<p>The second paragraph preserves its readable article shape.</p>"
        "<p>The third paragraph keeps the classification deterministic.</p>"
        "</article></main></body>";
    static const char playable[] =
        "<!doctype html><body><main><h1>Playable episode</h1>"
        "<video controls poster='/cover.jpg'><source src='/episode.mp4'>"
        "</video><p>Authored playback and notes.</p></main></body>";
    const char *fixtures[] = {controls_only, stylesheet_hidden, playable};
    Budget budget;
    budget_init(&budget, 32u * 1024u * 1024u);
    CHECK(budget_install_lexbor(&budget));
    for (size_t i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); i++) {
        PocDocument document = {0};
        Stylesheet stylesheet = {0};
        ReaderDocumentAnalysis analysis = {0};
        CHECK(document_parse(
                  &document, &budget, fixtures[i], strlen(fixtures[i]), 113u)
              && stylesheet_build(&stylesheet, &budget, &document, 480)
              && reader_document_prepare_with_stylesheet(
                     &document, &stylesheet, &analysis));
        CHECK(i == 2u ? analysis.kind == READER_PAGE_WATCH
                      : analysis.kind != READER_PAGE_WATCH);
        stylesheet_destroy(&stylesheet);
        document_destroy(&document);
        CHECK(budget.current == 0);
    }
    CHECK(budget_uninstall_lexbor(&budget)
          && budget_categories_reconcile(&budget));
    return true;
}

static bool test_auto_article_requires_semantic_root(void)
{
    static const char marketing[] =
        "<!doctype html><body><main><h1>Products for every team</h1>"
        "<section><h2>Move faster</h2><p>This substantial landing-page section "
        "describes a product benefit in complete sentences and provides enough "
        "prose to remain useful when Reader is requested manually.</p></section>"
        "<section><h2>Work together</h2><p>This second promotional section "
        "explains collaboration features with ordinary readable language while "
        "remaining part of a broad marketing page rather than an article.</p></section>"
        "<section><h2>Stay secure</h2><p>This third section outlines security "
        "features and deployment choices with enough words for the bounded "
        "content extractor to produce a useful manual presentation.</p></section>"
        "<section><h2>Get started</h2><p>This final call-to-action section "
        "contains more readable detail, customer guidance, and supporting "
        "explanation without claiming authored article semantics.</p></section>"
        "</main></body>";
    static const char article[] =
        "<!doctype html><body><main><article><h1>An authored report</h1>"
        "<p>This substantial first paragraph introduces a focused report in "
        "complete sentences and establishes its coherent subject for readers.</p>"
        "<p>This second paragraph develops the report with enough ordinary prose "
        "to keep the selected semantic subtree comfortably above its threshold.</p>"
        "<p>This third paragraph adds evidence and explanation while preserving "
        "a low link density and a predictable long-form reading structure.</p>"
        "<p>This fourth paragraph concludes the authored report and makes its "
        "article semantics unambiguous to the bounded classifier. The focused "
        "narrative remains useful and substantial through its final sentence.</p>"
        "</article></main></body>";
    Budget budget;
    budget_init(&budget, 32u * 1024u * 1024u);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    ReaderDocumentAnalysis analysis = {0};
    CHECK(analyze(marketing, &analysis, &document, &budget));
    CHECK(analysis.kind == READER_PAGE_ARTICLE
          && !analysis.high_confidence
          && find_reader_root(&document) != NULL);
    document_destroy(&document);
    CHECK(budget.current == 0);

    memset(&analysis, 0, sizeof(analysis));
    CHECK(analyze(article, &analysis, &document, &budget));
    CHECK(analysis.kind == READER_PAGE_ARTICLE
          && analysis.high_confidence
          && find_reader_root(&document) != NULL);
    document_destroy(&document);
    CHECK(budget_uninstall_lexbor(&budget) && budget.current == 0
          && budget_categories_reconcile(&budget));
    return true;
}

/* Automatic Reader shape rules (October 2026 site census). Each fixture is
   synthetic; the comments name the census page shape it stands for. */
static const char clear_prose_paragraph[] =
    "<p>This paragraph of a long report carries its own sentences, more than "
    "one hundred and twenty bytes of ordinary prose with only a single "
    "<a href='/related'>link</a> in it.</p>";

static bool analyze_fixture(const char *html, size_t length,
                            ReaderDocumentAnalysis *analysis,
                            bool expect_root)
{
    Budget budget;
    budget_init(&budget, 32u * 1024u * 1024u);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    *analysis = (ReaderDocumentAnalysis) {0};
    CHECK(document_parse(&document, &budget, html, length, 113u)
          && reader_document_prepare(&document, analysis));
    CHECK((find_reader_root(&document) != NULL) == expect_root);
    document_destroy(&document);
    CHECK(budget_uninstall_lexbor(&budget) && budget.current == 0u
          && budget_categories_reconcile(&budget));
    return true;
}

static bool append_paragraphs(char *html, size_t capacity, size_t *used,
                              const char *paragraph, size_t count)
{
    for (size_t i = 0; i < count; i++)
        CHECK(append_text(html, capacity, used, paragraph));
    return true;
}

/* A wiki or documentation article without an authored <article> element:
   one h1 and long paragraphs in a plain wrapper (the Guardian, MDN). */
static bool test_auto_reader_accepts_unmarked_clear_article(void)
{
    char html[8192];
    size_t used = 0;
    CHECK(append_text(html, sizeof(html), &used,
                      "<!doctype html><body><nav><a href='/'>Home</a></nav>"
                      "<div id=content><h1>A plain report</h1>"
                      "<h2 id=background><a href='#background'>Background"
                      "</a></h2>"));
    CHECK(append_paragraphs(html, sizeof(html), &used,
                            clear_prose_paragraph, 3u));
    CHECK(append_text(html, sizeof(html), &used,
                      "<h2 id=example><a href='#example'>Example</a></h2>"
                      "<pre>const doubled = values.map((value) => value * 2);"
                      "</pre>"));
    CHECK(append_paragraphs(html, sizeof(html), &used,
                            clear_prose_paragraph, 3u));
    CHECK(append_text(html, sizeof(html), &used,
                      "<p>Short note.</p></div><footer>Footer</footer>"
                      "</body>"));
    ReaderDocumentAnalysis analysis = {0};
    CHECK(analyze_fixture(html, used, &analysis, true));
    CHECK(analysis.kind == READER_PAGE_ARTICLE && analysis.high_confidence);
    return true;
}

/* A news front page: the same prose density, but every story is a headline
   link to another page (npr.org: 44 of them). */
static bool test_auto_reader_refuses_front_page_teasers(void)
{
    char html[16384];
    size_t used = 0;
    CHECK(append_text(html, sizeof(html), &used,
                      "<!doctype html><body><main><h1>News</h1>"));
    for (size_t i = 0; i < 8u; i++) {
        char story[512];
        int written = snprintf(
            story, sizeof(story),
            "<section><h3><a href='/story/%zu'>Headline number %zu</a></h3>"
            "%s</section>", i, i, clear_prose_paragraph);
        CHECK(written > 0 && (size_t) written < sizeof(story)
              && append_text(html, sizeof(html), &used, story));
    }
    CHECK(append_text(html, sizeof(html), &used, "</main></body>"));
    ReaderDocumentAnalysis analysis = {0};
    CHECK(analyze_fixture(html, used, &analysis, true));
    CHECK(analysis.kind == READER_PAGE_ARTICLE && !analysis.high_confidence);
    return true;
}

/* A converter or product page: a title and a few explanatory paragraphs
   among many short label paragraphs (xe.com), or no page title in the dense
   region at all (store and forecast pages). */
static bool test_auto_reader_refuses_label_pages_and_untitled_regions(void)
{
    char html[16384];
    size_t used = 0;
    CHECK(append_text(html, sizeof(html), &used,
                      "<!doctype html><body><main><h1>Convert units</h1>"));
    CHECK(append_paragraphs(html, sizeof(html), &used,
                            clear_prose_paragraph, 5u));
    CHECK(append_paragraphs(html, sizeof(html), &used,
                            "<p>1 unit = 0.88 other units</p>", 12u));
    CHECK(append_text(html, sizeof(html), &used, "</main></body>"));
    ReaderDocumentAnalysis analysis = {0};
    CHECK(analyze_fixture(html, used, &analysis, true));
    CHECK(analysis.kind == READER_PAGE_ARTICLE && !analysis.high_confidence);

    used = 0;
    CHECK(append_text(html, sizeof(html), &used,
                      "<!doctype html><body><main><h2>Store item</h2>"));
    CHECK(append_paragraphs(html, sizeof(html), &used,
                            clear_prose_paragraph, 8u));
    CHECK(append_text(html, sizeof(html), &used, "</main></body>"));
    CHECK(analyze_fixture(html, used, &analysis, true));
    CHECK(analysis.kind == READER_PAGE_ARTICLE && !analysis.high_confidence);
    return true;
}

/* Paragraphs inside excluded chrome used to count toward every wrapper
   around it, so an outer <div> outscored the page's own <article> (bbc.co.uk
   article pages: 50 counted paragraphs against the article's 22). */
static bool test_excluded_paragraphs_do_not_favor_wrappers(void)
{
    char html[16384];
    size_t used = 0;
    CHECK(append_text(html, sizeof(html), &used,
                      "<!doctype html><body><div id=root><nav>"));
    CHECK(append_paragraphs(html, sizeof(html), &used,
                            "<p>Navigation label</p>", 40u));
    CHECK(append_text(html, sizeof(html), &used,
                      "</nav><article><h1>The report</h1>"));
    CHECK(append_paragraphs(html, sizeof(html), &used,
                            clear_prose_paragraph, 5u));
    CHECK(append_text(html, sizeof(html), &used,
                      "</article></div></body>"));
    Budget budget;
    budget_init(&budget, 32u * 1024u * 1024u);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    ReaderDocumentAnalysis analysis = {0};
    CHECK(document_parse(&document, &budget, html, used, 113u)
          && reader_document_prepare(&document, &analysis));
    lxb_dom_node_t *article = find_named_within(
        document_body_node(&document), "article");
    CHECK(analysis.kind == READER_PAGE_ARTICLE && analysis.high_confidence
          && article != NULL
          && has_attribute(article, "data-tilefinch-reader-article"));
    document_destroy(&document);
    CHECK(budget_uninstall_lexbor(&budget) && budget.current == 0u
          && budget_categories_reconcile(&budget));
    return true;
}

/* Collapsed disclosure sections (MediaWiki's mobile article layout): the
   heading's button names the hidden section in aria-controls with
   aria-expanded="false". Reader and Basic retire scripts, so they keep the
   section; content hidden any other way stays out. */
static bool test_collapsed_disclosure_sections_are_kept(void)
{
    char html[16384];
    size_t used = 0;
    CHECK(append_text(html, sizeof(html), &used,
                      "<!doctype html><body><main><h1>Encyclopedia entry</h1>"
                      "<p>The lead paragraph is shown before any section and "
                      "summarizes the subject in more than one hundred and "
                      "twenty bytes of prose.</p>"
                      "<div class=heading><h2>History</h2>"
                      "<button aria-controls='history-content' "
                      "aria-expanded='false'>Toggle</button></div>"
                      "<div id=history-content hidden>"));
    CHECK(append_paragraphs(html, sizeof(html), &used,
                            clear_prose_paragraph, 5u));
    CHECK(append_text(html, sizeof(html), &used,
                      "<p>COLLAPSED SECTION TEXT</p></div>"
                      "<div class=heading><h2>Search</h2></div>"
                      "<div id=found hidden=until-found><p>UNTIL FOUND TEXT"
                      "</p></div>"
                      "<div id=plain hidden><p>PLAIN HIDDEN TEXT</p></div>"
                      "<button aria-controls='other' aria-expanded='false'>"
                      "Menu</button><div id=other hidden><p>UNNAMED REGION"
                      "</p></div><div id=menu hidden><p>CONTROLLED ELSEWHERE"
                      "</p></div></main></body>"));
    Budget budget;
    budget_init(&budget, 32u * 1024u * 1024u);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    ReaderDocumentAnalysis analysis = {0};
    CHECK(document_parse(&document, &budget, html, used, 113u)
          && reader_document_prepare(&document, &analysis));
    lxb_dom_node_t *root = find_reader_root(&document);
    CHECK(analysis.kind == READER_PAGE_ARTICLE && analysis.high_confidence
          && root != NULL
          && subtree_contains_text(root, "COLLAPSED SECTION TEXT")
          && subtree_contains_text(root, "UNTIL FOUND TEXT")
          && !subtree_contains_text(root, "PLAIN HIDDEN TEXT")
          && subtree_contains_text(root, "UNNAMED REGION")
          && !subtree_contains_text(root, "CONTROLLED ELSEWHERE"));
    document_destroy(&document);

    Stylesheet stylesheet = {0};
    document = (PocDocument) {0};
    CHECK(document_parse(&document, &budget, html, used, 113u)
          && stylesheet_build(&stylesheet, &budget, &document, 480)
          && reader_document_prepare_basic_with_stylesheet(
                 &document, &stylesheet, &analysis));
    root = find_direct_reader_root(&document);
    CHECK(analysis.kind == READER_PAGE_BASIC && root != NULL
          && subtree_contains_text(root, "COLLAPSED SECTION TEXT")
          && !subtree_contains_text(root, "PLAIN HIDDEN TEXT"));
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    CHECK(budget_uninstall_lexbor(&budget) && budget.current == 0u
          && budget_categories_reconcile(&budget));
    return true;
}

/* Text nodes no longer exhaust the classifier walk: a 4,000-element article
   with about 10,000 nodes is classified whole and can engage automatically
   (en.wikipedia.org articles have about two nodes per element). */
static bool test_text_nodes_do_not_bound_the_classifier(void)
{
    const size_t capacity = 512u * 1024u;
    char *html = malloc(capacity);
    CHECK(html != NULL);
    size_t used = 0;
    CHECK(append_text(html, capacity, &used,
                      "<!doctype html><body><main><h1>Long article</h1>"));
    CHECK(append_paragraphs(
        html, capacity, &used,
        "<p>Readable prose with an <b>emphasized</b> phrase continues the long "
        "article with well over one hundred and twenty bytes of its own "
        "text.</p>",
        2000u));
    CHECK(append_text(html, capacity, &used,
                      "<p>FINAL PARAGRAPH</p></main></body>"));
    ReaderDocumentAnalysis analysis = {0};
    CHECK(analyze_fixture(html, used, &analysis, true));
    free(html);
    if (!(analysis.kind == READER_PAGE_ARTICLE && !analysis.bounded_out
          && !analysis.extraction_truncated && analysis.visited_nodes > 8192u
          && analysis.high_confidence)) {
        fprintf(stderr, "text-heavy article: kind=%d bounded=%d truncated=%d "
                "visited=%u high=%d nodes=%u\n", (int) analysis.kind,
                (int) analysis.bounded_out,
                (int) analysis.extraction_truncated,
                (unsigned) analysis.visited_nodes,
                (int) analysis.high_confidence,
                (unsigned) analysis.extracted_nodes);
        return false;
    }
    return true;
}

static bool test_reader_heading_sizes_are_bounded(void)
{
    char css[SITE_ADAPTER_READER_CSS_LIMIT];
    char adapter[32];
    CHECK(site_adapter_reader_css(
              "https://example.test/report",
              SITE_ADAPTER_READER_FONT_SERIF, 100u,
              css, sizeof(css), adapter, sizeof(adapter))
          && strstr(css, "h1{font-size:1.55rem!important}") != NULL
          && strstr(css, "h2{font-size:1.3rem!important}") != NULL
          && strstr(css, "h3{font-size:1.15rem!important}") != NULL
          && strstr(css, "h6{font-size:1.05rem!important}") != NULL);
    return true;
}

static bool test_excluded_text_does_not_dilute_article(void)
{
    char html[8192];
    size_t used = 0;
    CHECK(append_text(html, sizeof(html), &used,
                      "<!doctype html><body><nav>"));
    for (size_t i = 0; i < 80u; i++)
        CHECK(append_text(html, sizeof(html), &used,
                          "Peripheral navigation label and link. "));
    CHECK(append_text(
        html, sizeof(html), &used,
        "</nav><main><article><h1>The useful article</h1>"
        "<p>This substantial first paragraph explains the actual subject in "
        "complete sentences and belongs to the readable primary content.</p>"
        "<p>This second paragraph continues the explanation with enough prose "
        "to establish a dense semantic article rather than page chrome.</p>"
        "<p>This third paragraph provides further useful details for a reader "
        "and keeps the primary subtree comfortably above its threshold.</p>"
        "<p>This fourth paragraph completes the article and ensures excluded "
        "navigation cannot dilute the classification score. Its concluding "
        "details remain focused on the same readable subject and provide a "
        "clear ending for the report.</p>"
        "</article></main></body>"));
    Budget budget;
    budget_init(&budget, 32u * 1024u * 1024u);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    ReaderDocumentAnalysis analysis = {0};
    CHECK(analyze(html, &analysis, &document, &budget));
    CHECK(analysis.kind == READER_PAGE_ARTICLE
          && analysis.high_confidence
          && analysis.visible_text_bytes < 1200u);
    document_destroy(&document);
    CHECK(budget_uninstall_lexbor(&budget) && budget.current == 0
          && budget_categories_reconcile(&budget));
    return true;
}

static bool test_aria_and_header_chrome_are_excluded(void)
{
    static const char html[] =
        "<!doctype html><body><div><header>SITE HEADER MUST GO</header></div>"
        "<div role='banner'>BANNER MUST GO</div>"
        "<div role='navigation'>NAVIGATION MUST GO</div>"
        "<section role='search'>SEARCH MUST GO</section>"
        "<div role='complementary'>COMPLEMENTARY MUST GO</div>"
        "<div role='contentinfo'>CONTENT INFO MUST GO</div>"
        "<main><article><header><h1>ARTICLE HEADER MUST STAY</h1></header>"
        "<p>This substantial first paragraph contains useful article prose "
        "and establishes the primary readable content without page chrome.</p>"
        "<p>This second paragraph continues with enough ordinary text for a "
        "stable article score and a predictable extracted presentation.</p>"
        "<p>This third paragraph contributes more detailed reading material "
        "while semantic navigation regions remain outside the score.</p>"
        "<p>This fourth paragraph completes a comfortably dominant article "
        "whose own semantic header must remain available to the reader. The "
        "closing explanation adds useful context without introducing any "
        "page-level controls or unrelated navigation.</p>"
        "</article></main></body>";
    Budget budget;
    budget_init(&budget, 32u * 1024u * 1024u);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    ReaderDocumentAnalysis analysis = {0};
    CHECK(analyze(html, &analysis, &document, &budget));
    lxb_dom_node_t *root = find_reader_root(&document);
    CHECK(analysis.kind == READER_PAGE_ARTICLE && analysis.high_confidence
          && analysis.visible_text_bytes < 1000u && root != NULL
          && count_named_within(root, "header") == 2u
          && subtree_contains_text(root, "ARTICLE HEADER MUST STAY")
          && !subtree_contains_text(root, "SITE HEADER MUST GO")
          && !subtree_contains_text(root, "BANNER MUST GO")
          && !subtree_contains_text(root, "NAVIGATION MUST GO")
          && !subtree_contains_text(root, "SEARCH MUST GO")
          && !subtree_contains_text(root, "COMPLEMENTARY MUST GO")
          && !subtree_contains_text(root, "CONTENT INFO MUST GO"));
    document_destroy(&document);
    CHECK(budget_uninstall_lexbor(&budget) && budget.current == 0
          && budget_categories_reconcile(&budget));
    return true;
}

static bool test_flattened_blocks_and_empty_semantics(void)
{
    static const char html[] =
        "<!doctype html><body><main><article><h1>Extraction boundaries</h1>"
        "<p>This substantial first paragraph gives the article enough useful "
        "prose to qualify for a deterministic Reader extraction.</p>"
        "<p>This second substantial paragraph keeps the selected content dense "
        "and separate from the intentionally discarded semantic chrome.</p>"
        "<p>This third substantial paragraph completes the minimum article "
        "shape while leaving the boundary fixture easy to inspect.</p>"
        "<p>This fourth substantial paragraph adds enough focused prose for "
        "the article threshold while preserving the test's purpose: adjacent "
        "flattened blocks need distinct readable word boundaries.</p>"
        "<div>ALPHA BLOCK</div><div>BETA BLOCK</div>"
        "<ul><li><nav>EMPTY NAV ITEM</nav></li>"
        "<li><span hidden>EMPTY HIDDEN ITEM</span></li>"
        "<li>VISIBLE ITEM</li></ul></article></main></body>";
    Budget budget;
    budget_init(&budget, 32u * 1024u * 1024u);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    ReaderDocumentAnalysis analysis = {0};
    CHECK(analyze(html, &analysis, &document, &budget));
    lxb_dom_node_t *root = find_reader_root(&document);
    CHECK(analysis.kind == READER_PAGE_ARTICLE && root != NULL
          && subtree_text_contains_words(root, "ALPHA BLOCK BETA BLOCK")
          && count_named_within(root, "ul") == 1u
          && count_named_within(root, "li") == 1u
          && subtree_contains_text(root, "VISIBLE ITEM")
          && !subtree_contains_text(root, "EMPTY NAV ITEM")
          && !subtree_contains_text(root, "EMPTY HIDDEN ITEM"));
    document_destroy(&document);
    CHECK(budget_uninstall_lexbor(&budget) && budget.current == 0
          && budget_categories_reconcile(&budget));
    return true;
}

static bool test_hidden_inline_and_modal_content_is_excluded(void)
{
    static const char html[] =
        "<!doctype html><body style='display:none'><main><article>"
        "<h1>Visible report</h1>"
        "<p>This substantial first paragraph establishes the useful report "
        "and gives the semantic article enough visible prose for Reader.</p>"
        "<p>This second paragraph continues the report with ordinary words "
        "that must survive extraction without hidden responsive copies.</p>"
        "<p>This third paragraph provides more focused article content and "
        "keeps the selected subtree comfortably above its threshold.</p>"
        "<p>This fourth paragraph completes the useful report while modal "
        "and inline-hidden descendants remain absent from Reader. The visible "
        "presentation still carries a complete and coherent conclusion.</p>"
        "<dialog>CLOSED DIALOG DUPLICATE</dialog>"
        "<div inert>INERT DUPLICATE</div>"
        "<div style='display: none ! important'>DISPLAY DUPLICATE</div>"
        "<div style='visibility: collapse'>VISIBILITY DUPLICATE</div>"
        "<div style='opacity: -0.000'>OPACITY DUPLICATE</div>"
        "<div style='display:none!important; display:block'>"
        "IMPORTANT DUPLICATE</div>"
        "<div style='display:none; display:garbage'>"
        "INVALID OVERRIDE DUPLICATE</div>"
        "<div style='display:none; display:block'>LAST VALUE VISIBLE</div>"
        "<div style='--description:&quot;display:none&quot;; color:red'>"
        "QUOTED TOKEN VISIBLE</div>"
        "<dialog open>OPEN DIALOG VISIBLE</dialog>"
        "</article></main></body>";
    Budget budget;
    budget_init(&budget, 32u * 1024u * 1024u);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    ReaderDocumentAnalysis analysis = {0};
    CHECK(analyze(html, &analysis, &document, &budget));
    lxb_dom_node_t *root = find_reader_root(&document);
    CHECK(analysis.kind == READER_PAGE_ARTICLE && root != NULL
          && subtree_contains_text(root, "Visible report")
          && subtree_contains_text(root, "LAST VALUE VISIBLE")
          && subtree_contains_text(root, "QUOTED TOKEN VISIBLE")
          && subtree_contains_text(root, "OPEN DIALOG VISIBLE")
          && !subtree_contains_text(root, "CLOSED DIALOG DUPLICATE")
          && !subtree_contains_text(root, "INERT DUPLICATE")
          && !subtree_contains_text(root, "DISPLAY DUPLICATE")
          && !subtree_contains_text(root, "VISIBILITY DUPLICATE")
          && !subtree_contains_text(root, "OPACITY DUPLICATE")
          && !subtree_contains_text(root, "IMPORTANT DUPLICATE")
          && !subtree_contains_text(root, "INVALID OVERRIDE DUPLICATE"));
    document_destroy(&document);
    CHECK(budget_uninstall_lexbor(&budget) && budget.current == 0
          && budget_categories_reconcile(&budget));
    return true;
}

static bool test_large_text_node_is_cooperative(void)
{
    const size_t capacity = 160u * 1024u;
    char *html = malloc(capacity);
    CHECK(html != NULL);
    size_t used = 0;
    CHECK(append_text(html, capacity, &used,
                      "<!doctype html><body><main><article>"
                      "<h1>Cooperative extraction</h1><p>"));
    static const char run[] =
        "Ordinary unescaped article text is copied in a bounded run while "
        "the platform receives regular opportunities to service input. ";
    for (size_t i = 0; i < 700u; i++)
        CHECK(append_text(html, capacity, &used, run));
    CHECK(append_text(
        html, capacity, &used,
        "LARGE TEXT SENTINEL</p>"
        "<p>A second paragraph preserves the semantic article shape.</p>"
        "<p>A third paragraph keeps classification deterministic.</p>"
        "<p>A fourth paragraph completes the readable report.</p>"
        "</article></main></body>"));

    Budget budget;
    budget_init(&budget, 32u * 1024u * 1024u);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    CHECK(document_parse(&document, &budget, html, used, 4096u));
    ReaderCooperateProbe probe = {0};
    TilefinchPlatformServices services = {
        .context = &probe,
        .cooperate = reader_cooperate_probe
    };
    ReaderDocumentAnalysis analysis = {0};
    tilefinch_platform_set_services(&services);
    bool prepared = reader_document_prepare(&document, &analysis);
    tilefinch_platform_set_services(NULL);
    lxb_dom_node_t *root = find_reader_root(&document);
    CHECK(prepared && analysis.kind == READER_PAGE_ARTICLE
          && root != NULL
          && subtree_contains_text(root, "LARGE TEXT SENTINEL")
          && probe.analyze_text_calls >= 16u
          && probe.extract_calls >= 16u
          && probe.analyze_max_gap <= 4096u
          && probe.extract_max_gap <= 4096u);
    document_destroy(&document);
    free(html);
    CHECK(budget_uninstall_lexbor(&budget) && budget.current == 0
          && budget_categories_reconcile(&budget));
    return true;
}

static bool test_semantic_attributes_survive_extraction(void)
{
    static const char html[] =
        "<!doctype html><body><main lang='en-US' dir='ltr'>"
        "<h1>Semantic episode</h1><audio controls src='/episode.m4a'></audio>"
        "<p>This episode has enough descriptive text to keep its semantic "
        "content useful in the extracted reader presentation.</p>"
        "<ol start='3' reversed><li value='7'>Seventh item</li></ol>"
        "<time datetime='2026-08-29'>August 29</time>"
        "<details open><summary>Notes</summary><p>Expanded notes.</p></details>"
        "<picture><source srcset='/small.jpg 1x, /large.jpg 2x' "
        "media='(min-width: 400px)'><img src='/small.jpg' "
        "srcset='/small.jpg 1x, /large.jpg 2x' sizes='100vw' alt='Cover'>"
        "</picture><table><tr><th scope='col' headers='group'>Name</th>"
        "<td headers='group'>Value</td></tr></table></main></body>";
    Budget budget;
    budget_init(&budget, 32u * 1024u * 1024u);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    ReaderDocumentAnalysis analysis = {0};
    CHECK(analyze(html, &analysis, &document, &budget));
    CHECK(analysis.kind == READER_PAGE_WATCH && analysis.high_confidence);
    lxb_dom_node_t *root = find_reader_root(&document);
    CHECK(root != NULL);
    size_t length = 0;
    lxb_dom_node_t *audio = find_named_within(root, "audio");
    CHECK(audio != NULL && has_attribute(audio, "controls"));
    lxb_dom_node_t *ordered = find_named_within(root, "ol");
    CHECK(ordered != NULL
          && has_attribute(ordered, "reversed")
          && document_attribute(ordered, "start", &length) != NULL
          && length == 1u);
    lxb_dom_node_t *time = find_named_within(root, "time");
    CHECK(time != NULL
          && document_attribute(time, "datetime", &length) != NULL
          && length == 10u);
    lxb_dom_node_t *source = find_named_within(root, "source");
    CHECK(source != NULL
          && document_attribute(source, "srcset", &length) != NULL
          && length != 0);
    lxb_dom_node_t *image = find_named_within(root, "img");
    CHECK(image != NULL
          && document_attribute(image, "sizes", &length) != NULL
          && length == 5u);
    lxb_dom_node_t *details = find_named_within(root, "details");
    CHECK(details != NULL && has_attribute(details, "open"));
    lxb_dom_node_t *heading = find_named_within(root, "h1");
    CHECK(heading != NULL);
    document_destroy(&document);
    CHECK(budget_uninstall_lexbor(&budget) && budget.current == 0
          && budget_categories_reconcile(&budget));
    return true;
}

static bool test_unknown_wrappers_do_not_consume_semantic_quota(void)
{
    char html[32768];
    size_t used = 0;
    CHECK(append_text(html, sizeof(html), &used,
                      "<!doctype html><body><main><article>"
                      "<h1>Wrapped article</h1>"));
    for (size_t i = 0; i < 520u; i++)
        CHECK(append_text(html, sizeof(html), &used, "<div></div>"));
    static const char paragraph[] =
        "<p>This substantial semantic paragraph remains visible after many "
        "non-semantic wrappers because wrappers do not spend the bounded "
        "Reader tree's scarce emitted-node allowance. BOTTOM SENTINEL.</p>";
    for (size_t i = 0; i < 4u; i++)
        CHECK(append_text(html, sizeof(html), &used, paragraph));
    CHECK(append_text(html, sizeof(html), &used,
                      "</article></main></body>"));
    Budget budget;
    budget_init(&budget, 32u * 1024u * 1024u);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    ReaderDocumentAnalysis analysis = {0};
    CHECK(analyze(html, &analysis, &document, &budget));
    lxb_dom_node_t *root = find_reader_root(&document);
    CHECK(analysis.kind == READER_PAGE_ARTICLE
          && !analysis.extraction_truncated
          && analysis.extracted_nodes < 16u
          && root != NULL
          && subtree_contains_text(root, "BOTTOM SENTINEL"));
    document_destroy(&document);
    CHECK(budget_uninstall_lexbor(&budget) && budget.current == 0
          && budget_categories_reconcile(&budget));
    return true;
}

static bool test_extraction_truncation_is_explicit(void)
{
    const size_t capacity = 1024u * 1024u;
    char *html = malloc(capacity);
    CHECK(html != NULL);
    size_t used = 0;
    CHECK(append_text(html, capacity, &used,
                      "<!doctype html><body><main><article>"
                      "<h1>Long article</h1>"));
    static const char paragraph[] =
        "<p>Bounded readable prose fills the article while the extracted tree "
        "retains a balanced prefix and explicitly reports its omitted suffix.</p>";
    /* More paragraphs than the 4,096-node ceiling an ample budget grants. */
    for (size_t i = 0; i < 4200u; i++)
        CHECK(append_text(html, capacity, &used, paragraph));
    CHECK(append_text(html, capacity, &used,
                      "<p>BOTTOM MUST BE OMITTED</p></article></main></body>"));
    Budget budget;
    budget_init(&budget, 32u * 1024u * 1024u);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    ReaderDocumentAnalysis analysis = {0};
    CHECK(document_parse(&document, &budget, html, used, 4096u)
          && reader_document_prepare(&document, &analysis));
    lxb_dom_node_t *root = find_reader_root(&document);
    size_t marker_length = 0;
    CHECK(analysis.kind == READER_PAGE_ARTICLE
          && analysis.extraction_truncated && analysis.bounded_out
          && !analysis.high_confidence
          && analysis.extracted_nodes == 4096u
          && root != NULL
          && document_attribute(
                 root, "data-tilefinch-reader-truncated",
                 &marker_length) != NULL
          && subtree_contains_text(
                 root, "Reader view shortened to fit this device.")
          && !subtree_contains_text(root, "BOTTOM MUST BE OMITTED"));
    document_destroy(&document);
    free(html);
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
    CHECK(analysis.bounded_out && analysis.extraction_truncated
          && !analysis.high_confidence
          && analysis.kind == READER_PAGE_ARTICLE
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
    const size_t capacity = 3u * 1024u * 1024u;
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
    /* More elements than the classifier's 8,192 scratch records. */
    for (size_t i = 0; i < 9000u; i++)
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
          && analysis.visited_nodes > 8192u
          && find_direct_reader_root(&document) != NULL);
    document_destroy(&document);
    free(html);
    CHECK(budget_uninstall_lexbor(&budget) && budget.current == 0
          && budget_categories_reconcile(&budget));
    return true;
}

static bool test_complete_bounded_walk_is_transactional(void)
{
    const size_t capacity = 512u * 1024u;
    char *html = malloc(capacity);
    CHECK(html != NULL);
    size_t used = 0u;
    CHECK(append_text(
        html, capacity, &used,
        "<!doctype html><body><main><article><h1>Complete article</h1>"
        "<p>This meaningful server-rendered article has enough readable "
        "prose to remain the preferred Reader candidate. Strict automatic "
        "recovery must nevertheless stay raw when the later document walk "
        "reaches its fixed node ceiling.</p>"));
    static const char supporting_paragraph[] =
        "<p>The extracted article itself remains compact and serializes "
        "cleanly. Semantic paragraphs, headings, and readable prose make "
        "this an unambiguous article while the unrelated suffix exercises "
        "only the classifier traversal bound.</p>";
    for (size_t i = 0u; i < 12u; i++)
        CHECK(append_text(html, capacity, &used, supporting_paragraph));
    CHECK(append_text(html, capacity, &used, "</article></main>"));
    for (size_t i = 0u; i < 8300u; i++)
        CHECK(append_text(html, capacity, &used, "<script></script>"));
    CHECK(append_text(html, capacity, &used, "</body>"));

    Budget budget;
    budget_init(&budget, 32u * 1024u * 1024u);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    CHECK(document_parse(&document, &budget, html, used, 4096u));
    size_t nodes_before = document.node_count;
    size_t attributes_before = document.attribute_count;
    lxb_dom_node_t *body = document_body_node(&document);
    ReaderDocumentAnalysis analysis = {0};
    bool prepared = reader_document_prepare_complete_with_stylesheet(
        &document, NULL, &analysis);
    lxb_dom_node_t *root = find_direct_reader_root(&document);
    bool marked = has_attribute(body, "data-tilefinch-reader-kind");
    bool okay = prepared && analysis.kind == READER_PAGE_ARTICLE
        && analysis.bounded_out && !analysis.extraction_truncated
        && root == NULL && !marked
        && document.node_count == nodes_before
        && document.attribute_count == attributes_before;
    if (!okay) {
        fprintf(stderr,
                "reader-complete-bound prepared=%d kind=%d bounded=%d "
                "truncated=%d visited=%u root=%p marker=%d nodes=%zu/%zu "
                "attributes=%zu/%zu\n",
                prepared ? 1 : 0, (int) analysis.kind,
                analysis.bounded_out ? 1 : 0,
                analysis.extraction_truncated ? 1 : 0,
                analysis.visited_nodes, (void *) root, marked ? 1 : 0,
                document.node_count, nodes_before,
                document.attribute_count, attributes_before);
    }
    CHECK(okay);
    document_destroy(&document);
    free(html);
    CHECK(budget_uninstall_lexbor(&budget) && budget.current == 0u
          && budget_categories_reconcile(&budget));
    return true;
}

static bool test_basic_navigation_and_link_boundaries(void)
{
    static const char html[] =
        "<!doctype html><body><nav aria-label='Browse &amp; explore'>"
        "<a href='/one'>One</a><a href='/two'>Two</a></nav>"
        "<div role='navigation'><a href='/three'>Three</a></div>"
        "<main><p><span>word</span><span>piece</span> "
        "<a href='/four'>Four</a><a href='/five'>Five</a> "
        "<b><a href='/six'>Six</a></b><a href='/seven'>Seven</a></p>"
        "<select><option>UNUSABLE OPTION</option></select>"
        "<form role='search' action='/search'><input name='q'><select name='kind'>"
        "<option value='all'>All</option></select></form></main></body>";
    Budget budget;
    budget_init(&budget, 32u * 1024u * 1024u);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    ReaderDocumentAnalysis analysis = {0};
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 113u)
          && reader_document_prepare_basic_complete_with_stylesheet(
                 &document, NULL, &analysis));
    lxb_dom_node_t *root = find_direct_reader_root(&document);
    CHECK(root != NULL && analysis.kind == READER_PAGE_BASIC
          && !analysis.bounded_out && !analysis.extraction_truncated
          && count_named_within(root, "details") == 2u
          && count_named_within(root, "summary") == 2u
          && !has_attribute(find_named_within(root, "details"), "open")
          && subtree_contains_text(root, "Browse & explore")
          && subtree_text_contains_words(root, "One Two")
          && subtree_text_contains_words(root, "wordpiece Four Five")
          && subtree_text_contains_words(root, "Six Seven")
          && !subtree_contains_text(root, "UNUSABLE OPTION")
          && count_named_within(root, "select") == 1u
          && count_named_within(root, "option") == 1u
          && find_attribute_value_within(root, "href", "/three") != NULL
          && analysis.retained_forms == 1u);
    document_destroy(&document);
    CHECK(budget_uninstall_lexbor(&budget) && budget.current == 0u
          && budget_categories_reconcile(&budget));
    return true;
}

static bool test_basic_view_preserves_actions_without_guessing(void)
{
    static const char html[] =
        "<!doctype html><body><header><h1 id='top'>Catalog</h1>"
        "<a href='#details'>Skip to details</a></header>"
        "<nav><a href='/categories'>Categories</a></nav>"
        "<main><p>Server-rendered catalog introduction.</p>"
        "<form role='search' action='/find' method='GET'>"
        "<label for='query'>Search catalog</label>"
        "<input id='query' name='q' type='search' placeholder='Keywords'>"
        "<textarea name='note' readonly>Fixed note</textarea>"
        "<select name='scope'><optgroup disabled>"
        "<option value='blocked'>Blocked scope</option></optgroup>"
        "<option value='all'>All items</option></select>"
        "<fieldset disabled><input name='blocked-query'>"
        "<select name='blocked-select'><option value='bad'>Bad</option></select>"
        "<textarea name='blocked-note'>Bad note</textarea>"
        "<button type='submit'>Disabled submit</button></fieldset>"
        "<button type='submit'>Search</button>"
        "<button type='button'>Client-only chooser</button></form>"
        "<form role='search' action='/disabled-only'><fieldset disabled>"
        "<label>Disabled query<input name='disabled-query'></label>"
        "</fieldset></form>"
        "<form method='post' action='/account'>"
        "<label>Password <input name='password'></label>"
        "<button type='submit'>Sign in</button></form>"
        "<section id='details'><h2>Details</h2><figure>"
        "<img src='/item.png' alt='Item'><figcaption>Item caption</figcaption>"
        "</figure><ul><li>One</li><li>Two</li></ul></section></main></body>";
    Budget budget;
    budget_init(&budget, 32u * 1024u * 1024u);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    ReaderDocumentAnalysis analysis = {0};
    CHECK(document_parse(
              &document, &budget, html, sizeof(html) - 1u, 113u)
          && stylesheet_build(&stylesheet, &budget, &document, 480)
          && reader_document_prepare_basic_complete_with_stylesheet(
                 &document, &stylesheet, &analysis));
    lxb_dom_node_t *root = find_direct_reader_root(&document);
    CHECK(analysis.prepared && analysis.kind == READER_PAGE_BASIC
          && !analysis.bounded_out && !analysis.extraction_truncated
          && analysis.retained_forms == 1u && analysis.mapped_anchors >= 3u
          && root != NULL
          && count_named_within(root, "form") == 1u
          && count_named_within(root, "input") == 1u
          && count_named_within(root, "textarea") == 1u
          && count_named_within(root, "select") == 1u
          && count_named_within(root, "option") == 1u
          && count_named_within(root, "button") == 1u
          && count_named_within(root, "nav") == 1u
          && count_named_within(root, "img") == 1u
          && count_named_within(root, "figcaption") == 1u
          && count_named_within(root, "ul") == 1u
          && subtree_contains_text(root, "Server-rendered catalog")
          && subtree_contains_text(root, "Client-only chooser")
          && subtree_contains_text(root, "Sign in")
          && find_attribute_value_within(
                 root, "href", "#details") != NULL
          && find_attribute_value_within(
                 root, "href", "#tilefinch-extracted-details") == NULL
          && find_attribute_value_within(
                 root, "id", "tilefinch-extracted-details") != NULL
          && find_attribute_value_within(
                 root, "for", "tilefinch-extracted-query") != NULL
          && find_attribute_value_within(root, "action", "/find") != NULL
          && find_attribute_value_within(
                 root, "action", "/disabled-only") == NULL
          && find_attribute_value_within(root, "action", "/account") == NULL
          && has_attribute(find_named_within(root, "textarea"), "readonly"));
    /* Source controls remain connected beside the hidden clone. */
    CHECK(count_named_elements(&document, "form") == 4u
          && count_named_elements(&document, "button") == 5u);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    CHECK(budget_uninstall_lexbor(&budget) && budget.current == 0u
          && budget_categories_reconcile(&budget));
    return true;
}

static bool test_extracted_fragment_markers_preserve_empty_targets(void)
{
    static const char html[] =
        "<!doctype html><body><main><article><h1>Fragment map</h1>"
        "<p>This substantial first paragraph gives the extracted article a "
        "coherent source while fragment targets remain at their exact source "
        "positions for navigation.</p>"
        "<p>This second paragraph supplies more ordinary prose so automatic "
        "article classification is stable and unrelated to the empty targets "
        "under test.</p>"
        "<p>This third paragraph keeps the selected subtree dense enough for "
        "the ordinary Reader path while preserving both duplicate identifiers "
        "in source order.</p>"
        "<p>This fourth paragraph completes the useful article shape and makes "
        "the same extractor contract observable through Reader and Basic.</p>"
        "<div id='details'><p>First destination.</p></div>"
        "<div id='details'><p>Duplicate destination.</p></div>"
        "<a name='legacy'></a><p>Content after the legacy anchor.</p>"
        "</article></main></body>";
    Budget budget;
    budget_init(&budget, 32u * 1024u * 1024u);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    ReaderDocumentAnalysis analysis = {0};
    CHECK(document_parse(
              &document, &budget, html, sizeof(html) - 1u, 113u)
          && stylesheet_build(&stylesheet, &budget, &document, 480)
          && reader_document_prepare_basic_complete_with_stylesheet(
                 &document, &stylesheet, &analysis));
    lxb_dom_node_t *root = find_direct_reader_root(&document);
    lxb_dom_node_t *legacy = root == NULL ? NULL
        : find_attribute_value_within(
              root, "name", "tilefinch-extracted-legacy");
    CHECK(analysis.prepared && analysis.kind == READER_PAGE_BASIC
          && !analysis.bounded_out && !analysis.extraction_truncated
          && analysis.mapped_anchors == 3u && root != NULL
          /* Duplicate source ids remain in source order. That matches HTML's
             first-target behavior without an unbounded deduplication table. */
          && count_attribute_value_within(
                 root, "id", "tilefinch-extracted-details") == 2u
          && count_attribute_value_within(
                 root, "data-tilefinch-reader-anchor", NULL) == 3u
          && legacy != NULL && find_named_within(legacy, "a") == legacy
          && count_attribute_value_within(
                 root, "name", "tilefinch-extracted-legacy") == 1u
          && subtree_contains_text(root, "First destination")
          && subtree_contains_text(root, "Duplicate destination")
          && subtree_contains_text(root, "Content after the legacy anchor"));
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    CHECK(budget_uninstall_lexbor(&budget) && budget.current == 0u
          && budget_categories_reconcile(&budget));

    budget_init(&budget, 32u * 1024u * 1024u);
    CHECK(budget_install_lexbor(&budget));
    document = (PocDocument) {0};
    analysis = (ReaderDocumentAnalysis) {0};
    CHECK(analyze(html, &analysis, &document, &budget));
    root = find_reader_root(&document);
    legacy = root == NULL ? NULL : find_attribute_value_within(
        root, "name", "tilefinch-extracted-legacy");
    CHECK(analysis.prepared && analysis.kind == READER_PAGE_ARTICLE
          && !analysis.bounded_out && !analysis.extraction_truncated
          && analysis.mapped_anchors == 3u && root != NULL
          && count_attribute_value_within(
                 root, "id", "tilefinch-extracted-details") == 2u
          && count_attribute_value_within(
                 root, "data-tilefinch-reader-anchor", NULL) == 3u
          && legacy != NULL && find_named_within(legacy, "a") == legacy
          && subtree_contains_text(root, "First destination")
          && subtree_contains_text(root, "Duplicate destination")
          && subtree_contains_text(root, "Content after the legacy anchor"));
    document_destroy(&document);
    CHECK(budget_uninstall_lexbor(&budget) && budget.current == 0u
          && budget_categories_reconcile(&budget));
    return true;
}

static bool test_basic_anchor_bounds_are_transactional(void)
{
    /* The extractor admits at most 192 source bytes in an anchor value. */
    char long_id[194u];
    memset(long_id, 'x', sizeof(long_id) - 1u);
    long_id[sizeof(long_id) - 1u] = '\0';
    char html[1024];
    int written = snprintf(
        html, sizeof(html),
        "<!doctype html><body><h1>Long anchor</h1>"
        "<div id='%s'><p>Useful destination.</p></div></body>", long_id);
    CHECK(written > 0 && (size_t) written < sizeof(html));

    Budget budget;
    budget_init(&budget, 32u * 1024u * 1024u);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    ReaderDocumentAnalysis analysis = {0};
    CHECK(document_parse(
              &document, &budget, html, (size_t) written, 113u)
          && stylesheet_build(&stylesheet, &budget, &document, 480));
    size_t nodes_before = document.node_count;
    size_t attributes_before = document.attribute_count;
    lxb_dom_node_t *body = document_body_node(&document);
    CHECK(reader_document_prepare_basic_complete_with_stylesheet(
              &document, &stylesheet, &analysis)
          && analysis.prepared && analysis.kind == READER_PAGE_BASIC
          && analysis.bounded_out && !analysis.extraction_truncated
          && find_direct_reader_root(&document) == NULL
          && !has_attribute(body, "data-tilefinch-reader-kind")
          && document.node_count == nodes_before
          && document.attribute_count == attributes_before);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    CHECK(budget_uninstall_lexbor(&budget) && budget.current == 0u
          && budget_categories_reconcile(&budget));

    const size_t many_capacity = 160u * 1024u;
    char *many = malloc(many_capacity);
    CHECK(many != NULL);
    size_t used = 0u;
    CHECK(append_text(
        many, many_capacity, &used,
        "<!doctype html><body><h1>Many anchors</h1>"));
    for (size_t i = 0; i <= 4096u; i++) {
        char marker[48];
        written = snprintf(marker, sizeof(marker), "<x id='a%zu'></x>", i);
        CHECK(written > 0 && (size_t) written < sizeof(marker)
              && append_text(many, many_capacity, &used, marker));
    }
    CHECK(append_text(many, many_capacity, &used, "</body>"));
    budget_init(&budget, 32u * 1024u * 1024u);
    CHECK(budget_install_lexbor(&budget));
    document = (PocDocument) {0};
    stylesheet = (Stylesheet) {0};
    analysis = (ReaderDocumentAnalysis) {0};
    CHECK(document_parse(&document, &budget, many, used, 113u)
          && stylesheet_build(&stylesheet, &budget, &document, 480));
    body = document_body_node(&document);
    /* More fragment markers than the emitted-node bound is a node bound,
       not an omitted action: the view is admitted as a labeled prefix. */
    CHECK(reader_document_prepare_basic_complete_with_stylesheet(
              &document, &stylesheet, &analysis)
          && analysis.prepared && analysis.kind == READER_PAGE_BASIC
          && !analysis.bounded_out && analysis.extraction_truncated
          && analysis.extracted_nodes == 4096u
          && find_direct_reader_root(&document) != NULL
          && has_attribute(body, "data-tilefinch-reader-kind")
          && subtree_contains_text(
                 find_direct_reader_root(&document),
                 "Basic view shortened to fit this device."));
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    free(many);
    CHECK(budget_uninstall_lexbor(&budget) && budget.current == 0u
          && budget_categories_reconcile(&budget));
    return true;
}

/* A news front page puts thousands of wrapper nodes ahead of its first
   story. Basic view is bounded by what it emits, so the content after them
   is extracted; a form the emitted-node bound cuts through is withdrawn
   whole rather than offered with some of its controls. */
static bool test_basic_view_bounds_emitted_not_inspected_nodes(void)
{
    size_t capacity = 160u * 1024u;
    char *html = malloc(capacity);
    CHECK(html != NULL);
    size_t used = 0u;
    CHECK(append_text(html, capacity, &used,
                      "<!doctype html><body><nav>"));
    for (size_t i = 0; i < 5000u; i++)
        CHECK(append_text(html, capacity, &used, "<div></div>"));
    CHECK(append_text(
        html, capacity, &used,
        "</nav><h1>Front page</h1><p>The first story survives the "
        "wrappers.</p><form role='search' action='/find'>"
        "<label>Query<input name='q'></label>"));
    /* More filler than the 4,096-node ceiling an ample budget grants. */
    for (size_t i = 0; i < 4200u; i++)
        CHECK(append_text(html, capacity, &used, "<p>filler</p>"));
    CHECK(append_text(html, capacity, &used,
                      "<input name='late'></form></body>"));
    Budget budget;
    budget_init(&budget, 32u * 1024u * 1024u);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    ReaderDocumentAnalysis analysis = {0};
    CHECK(document_parse(&document, &budget, html, used, 113u)
          && stylesheet_build(&stylesheet, &budget, &document, 480));
    CHECK(reader_document_prepare_basic_complete_with_stylesheet(
              &document, &stylesheet, &analysis));
    lxb_dom_node_t *root = find_direct_reader_root(&document);
    bool ok = analysis.kind == READER_PAGE_BASIC && !analysis.bounded_out
        && analysis.extraction_truncated && analysis.visited_nodes > 4096u
        && analysis.retained_forms == 0u && root != NULL
        && subtree_contains_text(root, "The first story survives")
        && subtree_contains_text(root, "Basic view shortened")
        && count_named_within(root, "form") == 0u
        && count_named_within(root, "input") == 0u;
    if (!ok) {
        fprintf(stderr, "basic large page: kind=%d bounded=%d truncated=%d "
                "visited=%u forms=%u nodes=%u root=%d\n",
                (int) analysis.kind, (int) analysis.bounded_out,
                (int) analysis.extraction_truncated,
                (unsigned) analysis.visited_nodes,
                (unsigned) analysis.retained_forms,
                (unsigned) analysis.extracted_nodes, root != NULL);
    }
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    free(html);
    CHECK(budget_uninstall_lexbor(&budget) && budget.current == 0u
          && budget_categories_reconcile(&budget));
    return ok;
}

static bool test_basic_view_complete_bound_is_transactional(void)
{
    char html[16384];
    size_t used = 0u;
    CHECK(append_text(
        html, sizeof(html), &used,
        "<!doctype html><body><h1>Bounded forms</h1><p>Useful body.</p>"));
    for (size_t i = 0; i < 9u; i++) {
        char form[256];
        int written = snprintf(
            form, sizeof(form),
            "<form role='search' action='/find/%zu'><label>Query %zu"
            "<input name='q%zu'></label></form>", i, i, i);
        CHECK(written > 0 && (size_t) written < sizeof(form)
              && append_text(html, sizeof(html), &used, form));
    }
    CHECK(append_text(html, sizeof(html), &used, "</body>"));
    Budget budget;
    budget_init(&budget, 32u * 1024u * 1024u);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    ReaderDocumentAnalysis analysis = {0};
    CHECK(document_parse(&document, &budget, html, used, 113u)
          && stylesheet_build(&stylesheet, &budget, &document, 480));
    size_t nodes_before = document.node_count;
    size_t attributes_before = document.attribute_count;
    lxb_dom_node_t *body = document_body_node(&document);
    CHECK(reader_document_prepare_basic_complete_with_stylesheet(
              &document, &stylesheet, &analysis)
          && analysis.prepared && analysis.kind == READER_PAGE_BASIC
          && analysis.bounded_out && analysis.retained_forms == 8u
          && find_direct_reader_root(&document) == NULL
          && !has_attribute(body, "data-tilefinch-reader-kind")
          && document.node_count == nodes_before
          && document.attribute_count == attributes_before);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    CHECK(budget_uninstall_lexbor(&budget) && budget.current == 0u
          && budget_categories_reconcile(&budget));
    return true;
}

static bool test_basic_view_form_scan_bound_is_transactional(void)
{
    char html[16384];
    size_t used = 0u;
    CHECK(append_text(
        html, sizeof(html), &used,
        "<!doctype html><body><h1>Bounded search</h1>"
        "<p>Useful server-rendered body.</p>"
        "<form role='search' action='/find'>"));
    /* Put the query control beyond the independent per-form scan ceiling.
       Complete Basic admission must refuse the whole extracted tree rather
       than flattening this form into a partially actionable page. */
    for (size_t i = 0; i < 193u; i++)
        CHECK(append_text(html, sizeof(html), &used, "<span></span>"));
    CHECK(append_text(
        html, sizeof(html), &used,
        "<label>Query<input name='q' type='search'></label>"
        "</form></body>"));

    Budget budget;
    budget_init(&budget, 32u * 1024u * 1024u);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    ReaderDocumentAnalysis analysis = {0};
    CHECK(document_parse(&document, &budget, html, used, 113u)
          && stylesheet_build(&stylesheet, &budget, &document, 480));
    size_t nodes_before = document.node_count;
    size_t attributes_before = document.attribute_count;
    lxb_dom_node_t *body = document_body_node(&document);
    CHECK(reader_document_prepare_basic_complete_with_stylesheet(
              &document, &stylesheet, &analysis)
          && analysis.prepared && analysis.kind == READER_PAGE_BASIC
          && analysis.bounded_out && analysis.retained_forms == 0u
          && find_direct_reader_root(&document) == NULL
          && !has_attribute(body, "data-tilefinch-reader-kind")
          && document.node_count == nodes_before
          && document.attribute_count == attributes_before);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    CHECK(budget_uninstall_lexbor(&budget) && budget.current == 0u
          && budget_categories_reconcile(&budget));
    return true;
}

static bool build_basic_select_fixture(char *html, size_t capacity,
                                       size_t option_count, size_t *used)
{
    *used = 0u;
    if (!append_text(
            html, capacity, used,
            "<!doctype html><body><h1>Bounded choices</h1>"
            "<p>Useful server-rendered choices.</p>"
            "<form role='search' action='/find'>"
            "<label>Query<input name='q' type='search'></label>"
            "<label>Scope<select name='scope'>")) return false;
    for (size_t i = 0; i < option_count; i++) {
        char option[80];
        int written = snprintf(
            option, sizeof(option),
            "<option value='v%zu'>Choice %zu</option>", i, i);
        if (written <= 0 || (size_t) written >= sizeof(option)
            || !append_text(html, capacity, used, option)) return false;
    }
    return append_text(
        html, capacity, used,
        "</select></label><button type='submit'>Search</button>"
        "</form></body>");
}

static bool test_basic_select_option_bound_is_transactional(void)
{
    char html[16384];
    size_t used = 0u;
    CHECK(build_basic_select_fixture(
        html, sizeof(html), 128u, &used));
    Budget budget;
    budget_init(&budget, 32u * 1024u * 1024u);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    ReaderDocumentAnalysis analysis = {0};
    CHECK(document_parse(&document, &budget, html, used, 113u)
          && stylesheet_build(&stylesheet, &budget, &document, 480)
          && reader_document_prepare_basic_complete_with_stylesheet(
                 &document, &stylesheet, &analysis));
    lxb_dom_node_t *root = find_direct_reader_root(&document);
    CHECK(analysis.kind == READER_PAGE_BASIC && !analysis.bounded_out
          && root != NULL && count_named_within(root, "select") == 1u
          && count_named_within(root, "option") == 128u);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    CHECK(budget_uninstall_lexbor(&budget) && budget.current == 0u
          && budget_categories_reconcile(&budget));

    CHECK(build_basic_select_fixture(
        html, sizeof(html), 129u, &used));
    budget_init(&budget, 32u * 1024u * 1024u);
    CHECK(budget_install_lexbor(&budget));
    document = (PocDocument) {0};
    stylesheet = (Stylesheet) {0};
    analysis = (ReaderDocumentAnalysis) {0};
    CHECK(document_parse(&document, &budget, html, used, 113u)
          && stylesheet_build(&stylesheet, &budget, &document, 480));
    size_t nodes_before = document.node_count;
    size_t attributes_before = document.attribute_count;
    lxb_dom_node_t *body = document_body_node(&document);
    CHECK(reader_document_prepare_basic_complete_with_stylesheet(
              &document, &stylesheet, &analysis)
          && analysis.kind == READER_PAGE_BASIC && analysis.bounded_out
          && find_direct_reader_root(&document) == NULL
          && !has_attribute(body, "data-tilefinch-reader-kind")
          && document.node_count == nodes_before
          && document.attribute_count == attributes_before);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    CHECK(budget_uninstall_lexbor(&budget) && budget.current == 0u
          && budget_categories_reconcile(&budget));
    return true;
}

static bool test_basic_structural_extraction_is_cooperative_once(void)
{
    char html[32768];
    size_t used = 0u;
    CHECK(append_text(
        html, sizeof(html), &used,
        "<!doctype html><body><h1>Cooperative Basic view</h1>"));
    for (size_t i = 0; i < 180u; i++) {
        CHECK(append_text(
            html, sizeof(html), &used,
            "<div><span>Bounded server item.</span></div>"));
    }
    CHECK(append_text(html, sizeof(html), &used, "</body>"));

    Budget budget;
    budget_init(&budget, 32u * 1024u * 1024u);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    CHECK(document_parse(&document, &budget, html, used, 113u)
          && stylesheet_build(&stylesheet, &budget, &document, 480));
    ReaderCooperateProbe probe = {0};
    TilefinchPlatformServices services = {
        .context = &probe,
        .cooperate = reader_cooperate_probe
    };
    ReaderDocumentAnalysis analysis = {0};
    tilefinch_platform_set_services(&services);
    bool prepared =
        reader_document_prepare_basic_complete_with_stylesheet(
            &document, &stylesheet, &analysis);
    tilefinch_platform_set_services(NULL);
    CHECK(prepared && analysis.kind == READER_PAGE_BASIC
          && !analysis.bounded_out
          && find_direct_reader_root(&document) != NULL
          && probe.extract_node_calls >= 4u
          && probe.extract_node_max_gap <= 128u);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    CHECK(budget_uninstall_lexbor(&budget) && budget.current == 0u
          && budget_categories_reconcile(&budget));
    return true;
}

static bool refuse_reader_table(void *context, const char *phase, size_t work)
{
    size_t *calls = context;
    if (strcmp(phase, "reader-table") != 0) return true;
    (*calls)++;
    return work < 128u;
}

static bool test_basic_table_classification_cancellation(void)
{
    char html[8192];
    size_t used = 0;
    CHECK(append_text(html, sizeof(html), &used, "<body><table><tr><td>"));
    for (size_t i = 0; i < 180u; i++)
        CHECK(append_text(html, sizeof(html), &used, "<span>cell</span>"));
    CHECK(append_text(html, sizeof(html), &used, "</td></tr></table></body>"));
    Budget budget;
    budget_init(&budget, 32u * 1024u * 1024u);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    CHECK(document_parse(&document, &budget, html, used, 113u));
    size_t nodes_before = document.node_count, calls = 0;
    TilefinchPlatformServices services = {
        .context = &calls, .cooperate = refuse_reader_table
    };
    ReaderDocumentAnalysis analysis = {0};
    tilefinch_platform_set_services(&services);
    bool prepared = reader_document_prepare_basic_complete_with_stylesheet(
        &document, NULL, &analysis);
    tilefinch_platform_set_services(NULL);
    CHECK(!prepared && calls == 1u && analysis.bounded_out
        && find_direct_reader_root(&document) == NULL
        && document.node_count == nodes_before
        && !has_attribute(document_body_node(&document), "data-tilefinch-reader-kind"));
    document_destroy(&document);
    CHECK(budget_uninstall_lexbor(&budget) && budget.current == 0u);
    return true;
}

/* The emitted bound scales with the budget's free room: an ample budget
   extracts a long page whole, while a budget with less than the 4 MiB
   reserve free still gets the 512-node floor, as before scaling. The markup
   buffer grows past its initial 64 KiB on the way. */
static bool basic_view_nodes_under_budget(const char *html, size_t length,
                                          size_t limit,
                                          ReaderDocumentAnalysis *analysis)
{
    Budget budget;
    budget_init(&budget, limit);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    *analysis = (ReaderDocumentAnalysis) {0};
    CHECK(document_parse(&document, &budget, html, length, 113u)
          && stylesheet_build(&stylesheet, &budget, &document, 480));
    CHECK(reader_document_prepare_basic_complete_with_stylesheet(
              &document, &stylesheet, analysis));
    lxb_dom_node_t *root = find_direct_reader_root(&document);
    CHECK(root != NULL && subtree_contains_text(root, "Paragraph 0 "));
    CHECK(analysis->extraction_truncated
          == subtree_contains_text(root, "Basic view shortened"));
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    CHECK(budget_uninstall_lexbor(&budget) && budget.current == 0u
          && budget_categories_reconcile(&budget));
    return true;
}

static bool test_basic_view_bound_scales_with_budget(void)
{
    const size_t capacity = 256u * 1024u;
    char *html = malloc(capacity);
    CHECK(html != NULL);
    size_t used = 0u;
    CHECK(append_text(html, capacity, &used,
                      "<!doctype html><body><h1>Long listing</h1>"));
    for (size_t i = 0; i < 1500u; i++) {
        char paragraph[160];
        int written = snprintf(
            paragraph, sizeof(paragraph),
            "<p>Paragraph %zu of a long server-rendered page whose second "
            "half used to be cut.</p>", i);
        CHECK(written > 0 && (size_t) written < sizeof(paragraph)
              && append_text(html, capacity, &used, paragraph));
    }
    CHECK(append_text(html, capacity, &used,
                      "<p>LAST PARAGRAPH</p></body>"));
    ReaderDocumentAnalysis ample = {0};
    CHECK(basic_view_nodes_under_budget(
        html, used, 32u * 1024u * 1024u, &ample));
    CHECK(ample.kind == READER_PAGE_BASIC && !ample.extraction_truncated
          && !ample.bounded_out && ample.extracted_nodes == 1502u
          && ample.extracted_bytes > 64u * 1024u);
    ReaderDocumentAnalysis tight = {0};
    CHECK(basic_view_nodes_under_budget(
        html, used, 2u * 1024u * 1024u, &tight));
    if (!(tight.kind == READER_PAGE_BASIC && tight.extraction_truncated
          && tight.extracted_nodes == 512u)) {
        fprintf(stderr, "basic budget floor: kind=%d truncated=%d nodes=%u\n",
                (int) tight.kind, (int) tight.extraction_truncated,
                (unsigned) tight.extracted_nodes);
        return false;
    }
    free(html);
    return true;
}

static bool test_declared_video_synthesizes_watch_surface(void)
{
    static const char html[] =
        "<!doctype html><head><script type=application/ld+json>"
        "{\"@type\":\"VideoObject\",\"name\":\"Declared feature\","
        "\"thumbnailUrl\":\"/poster.jpg\","
        "\"contentUrl\":\"/feature-240.mp4\"}</script></head><body>"
        "<div data-tilefinch-declared-media-card=synthetic>"
        "Engine card must not be copied</div><article>"
        "<h1>Declared feature</h1>"
        "<p>This complete paragraph introduces the declared feature and its "
        "important context for a compact reader presentation.</p>"
        "<p>The second paragraph preserves useful server rendered details "
        "without depending on author hydration.</p>"
        "<p>The third paragraph gives the viewer enough information to decide "
        "whether to play the media.</p>"
        "<p>The fourth paragraph retains meaningful article evidence in the "
        "original source order.</p>"
        "<p>The fifth paragraph keeps this fixture above the bounded article "
        "confidence threshold.</p>"
        "<p>The final paragraph closes the description with a useful next "
        "step for the viewer.</p></article></body>";
    Budget budget;
    budget_init(&budget, 32u * 1024u * 1024u);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    CHECK(document_parse(
              &document, &budget, html, sizeof(html) - 1u, 113u)
          && stylesheet_build(&stylesheet, &budget, &document, 480));
    /* This fixture represents the exact native recovery node. A matching
       authored attribute alone is deliberately not trusted. */
    document.declared_video_card_node =
        document_body_node(&document)->first_child;
    ReaderDocumentAnalysis analysis = {0};
    CHECK(reader_document_prepare_with_stylesheet(
              &document, &stylesheet, &analysis)
          && analysis.kind == READER_PAGE_WATCH);
    lxb_dom_node_t *root = find_direct_reader_root(&document);
    lxb_dom_node_t *video = find_named_within(root, "video");
    CHECK(root != NULL && video != NULL
          && has_attribute(video, "controls")
          && has_attribute(video, "data-tilefinch-declared-media-card")
          && !subtree_contains_text(root, "Engine card must not be copied"));
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    CHECK(budget_uninstall_lexbor(&budget) && budget.current == 0u
          && budget_categories_reconcile(&budget));

    budget_init(&budget, 32u * 1024u * 1024u);
    CHECK(budget_install_lexbor(&budget));
    memset(&document, 0, sizeof(document));
    memset(&stylesheet, 0, sizeof(stylesheet));
    CHECK(document_parse(
              &document, &budget, html, sizeof(html) - 1u, 115u)
          && stylesheet_build(&stylesheet, &budget, &document, 480));
    document.declared_video_card_node =
        document_body_node(&document)->first_child;
    analysis = (ReaderDocumentAnalysis) {0};
    CHECK(reader_document_prepare_basic_complete_with_stylesheet(
              &document, &stylesheet, &analysis)
          && analysis.kind == READER_PAGE_BASIC);
    root = find_direct_reader_root(&document);
    video = find_named_within(root, "video");
    CHECK(video != NULL
          && has_attribute(video, "data-tilefinch-declared-media-card")
          && !subtree_contains_text(root, "Engine card must not be copied"));
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    CHECK(budget_uninstall_lexbor(&budget) && budget.current == 0u
          && budget_categories_reconcile(&budget));
    return true;
}

int main(void)
{
    if (!test_article()
        || !test_refresh_refusal_rolls_back_extracted_reader()
        || !test_static_fallback_respects_bare_hidden_body()
        || !test_listing_and_watch()
        || !test_empty_listing_extraction_falls_back_to_raw()
        || !test_audio_page_precedes_listing()
        || !test_hidden_or_peripheral_media_is_not_primary()
        || !test_primary_media_requires_visible_authored_source()
        || !test_auto_article_requires_semantic_root()
        || !test_reader_heading_sizes_are_bounded()
        || !test_excluded_text_does_not_dilute_article()
        || !test_aria_and_header_chrome_are_excluded()
        || !test_flattened_blocks_and_empty_semantics()
        || !test_hidden_inline_and_modal_content_is_excluded()
        || !test_large_text_node_is_cooperative()
        || !test_semantic_attributes_survive_extraction()
        || !test_unknown_wrappers_do_not_consume_semantic_quota()
        || !test_extraction_truncation_is_explicit()
        || !test_large_page_bound()
        || !test_bounded_page_keeps_manual_article()
        || !test_complete_bounded_walk_is_transactional()
        || !test_reading_identity_and_semantic_selection()
        || !test_basic_layout_tables_and_embedded_images()
        || !test_basic_navigation_and_link_boundaries()
        || !test_basic_view_preserves_actions_without_guessing()
        || !test_extracted_fragment_markers_preserve_empty_targets()
        || !test_basic_anchor_bounds_are_transactional()
        || !test_basic_view_bounds_emitted_not_inspected_nodes()
        || !test_basic_view_complete_bound_is_transactional()
        || !test_basic_view_form_scan_bound_is_transactional()
        || !test_basic_select_option_bound_is_transactional()
        || !test_basic_structural_extraction_is_cooperative_once()
        || !test_basic_table_classification_cancellation()
        || !test_basic_view_bound_scales_with_budget()
        || !test_auto_reader_accepts_unmarked_clear_article()
        || !test_auto_reader_refuses_front_page_teasers()
        || !test_auto_reader_refuses_label_pages_and_untitled_regions()
        || !test_excluded_paragraphs_do_not_favor_wrappers()
        || !test_collapsed_disclosure_sections_are_kept()
        || !test_text_nodes_do_not_bound_the_classifier()
        || !test_declared_video_synthesizes_watch_surface())
        return EXIT_FAILURE;
    puts("reader-mode-tests: ok");
    return EXIT_SUCCESS;
}
