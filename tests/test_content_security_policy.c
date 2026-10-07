#include "tilefinch/content_security_policy.h"
#include "tilefinch/document.h"
#include "tilefinch/fetch.h"
#include "tilefinch/style.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "CSP CHECK failed at %s:%d: %s\n",                \
                __FILE__, __LINE__, #condition);                             \
        return 1;                                                            \
    }                                                                        \
} while (0)

static lxb_dom_node_t *find_element(lxb_dom_node_t *node, const char *wanted)
{
    for (; node != NULL; node = node->next) {
        size_t length = 0;
        const char *name = document_element_name(node, &length);
        if (name != NULL && strlen(wanted) == length
            && memcmp(name, wanted, length) == 0) return node;
        lxb_dom_node_t *child = find_element(node->first_child, wanted);
        if (child != NULL) return child;
    }
    return NULL;
}

static lxb_dom_node_t *find_element_by_id(lxb_dom_node_t *node,
                                          const char *wanted)
{
    for (; node != NULL; node = node->next) {
        size_t length = 0;
        const char *id = document_attribute(node, "id", &length);
        if (id != NULL && strlen(wanted) == length
            && memcmp(id, wanted, length) == 0) return node;
        lxb_dom_node_t *child = find_element_by_id(node->first_child, wanted);
        if (child != NULL) return child;
    }
    return NULL;
}

static int test_directives(void)
{
    static const char headers[] =
        "content-security-policy: default-src 'self'; "
        "script-src 'nonce-good' https://scripts.test; "
        "style-src 'sha256-bhHHL3z2vDgxUt0W3dWQOrprscmda2Y5pLsLg4GF+pI='; "
        "img-src https://img.test data:; font-src https://fonts.test; "
        "connect-src https:; frame-src https://frames.test; "
        "object-src 'none'; base-uri 'self'; "
        "form-action https://submit.test; frame-ancestors 'self'; "
        "media-src https://media.test\n";
    TilefinchContentSecurityPolicy csp;
    CHECK(tilefinch_csp_parse_response_headers(
        &csp, "https://page.test/article", headers, sizeof(headers) - 1,
        false));
    CHECK(csp.header_present && csp.valid && csp.policy_count == 1);
    CHECK(tilefinch_csp_allows_request(
        &csp, TILEFINCH_DESTINATION_SCRIPT,
        "https://scripts.test/app.js"));
    CHECK(!tilefinch_csp_allows_request(
        &csp, TILEFINCH_DESTINATION_SCRIPT,
        "https://page.test/app.js"));
    CHECK(tilefinch_csp_allows_request(
        &csp, TILEFINCH_DESTINATION_IMAGE,
        "https://img.test/photo.png"));
    CHECK(tilefinch_csp_allows_request(
        &csp, TILEFINCH_DESTINATION_IMAGE, "data:image/png;base64,AA=="));
    CHECK(!tilefinch_csp_allows_request(
        &csp, TILEFINCH_DESTINATION_IMAGE,
        "https://page.test/photo.png"));
    CHECK(tilefinch_csp_allows_request(
        &csp, TILEFINCH_DESTINATION_FONT,
        "https://fonts.test/font.woff"));
    CHECK(!tilefinch_csp_allows_request(
        &csp, TILEFINCH_DESTINATION_FONT,
        "https://page.test/font.woff"));
    CHECK(tilefinch_csp_allows_request(
        &csp, TILEFINCH_DESTINATION_FETCH,
        "https://api.other.test/data"));
    CHECK(!tilefinch_csp_allows_request(
        &csp, TILEFINCH_DESTINATION_FETCH,
        "http://api.other.test/data"));
    CHECK(tilefinch_csp_allows_request(
        &csp, TILEFINCH_DESTINATION_FRAME,
        "https://frames.test/embed"));
    CHECK(!tilefinch_csp_allows_request(
        &csp, TILEFINCH_DESTINATION_OTHER,
        "https://page.test/plugin"));
    CHECK(tilefinch_csp_allows_request(
        &csp, TILEFINCH_DESTINATION_MEDIA,
        "https://media.test/movie.mp4"));
    CHECK(!tilefinch_csp_allows_request(
        &csp, TILEFINCH_DESTINATION_MEDIA,
        "https://page.test/movie.mp4"));
    CHECK(tilefinch_csp_allows_base_uri(
        &csp, "https://page.test/base/"));
    CHECK(!tilefinch_csp_allows_base_uri(
        &csp, "https://other.test/base/"));
    CHECK(tilefinch_csp_allows_form_action(
        &csp, "https://submit.test/post"));
    CHECK(!tilefinch_csp_allows_form_action(
        &csp, "https://page.test/post"));
    CHECK(tilefinch_csp_has_frame_ancestors(&csp));
    CHECK(tilefinch_csp_allows_ancestor(&csp, "https://page.test/parent"));
    CHECK(!tilefinch_csp_allows_ancestor(
        &csp, "https://embedder.test/parent"));
    return 0;
}

static int test_source_matching_and_intersection(void)
{
    static const char headers[] =
        "content-security-policy: default-src https://*.cdn.test/assets/; "
        "img-src https://img.test/exact.png\n"
        "content-security-policy: default-src https:; img-src https://img.test\n";
    TilefinchContentSecurityPolicy csp;
    CHECK(tilefinch_csp_parse_response_headers(
        &csp, "https://page.test/", headers, sizeof(headers) - 1, false));
    CHECK(csp.policy_count == 2);
    CHECK(tilefinch_csp_allows_request(
        &csp, TILEFINCH_DESTINATION_IMAGE,
        "https://img.test/exact.png"));
    CHECK(!tilefinch_csp_allows_request(
        &csp, TILEFINCH_DESTINATION_IMAGE,
        "https://img.test/exact.png.more"));
    CHECK(!tilefinch_csp_allows_request(
        &csp, TILEFINCH_DESTINATION_IMAGE,
        "https://other.test/exact.png"));
    CHECK(tilefinch_csp_allows_request(
        &csp, TILEFINCH_DESTINATION_SCRIPT,
        "https://sub.cdn.test/assets/app.js"));
    CHECK(!tilefinch_csp_allows_request(
        &csp, TILEFINCH_DESTINATION_SCRIPT,
        "https://cdn.test/assets/app.js"));
    CHECK(!tilefinch_csp_allows_request(
        &csp, TILEFINCH_DESTINATION_SCRIPT,
        "https://sub.cdn.test:8443/assets/app.js"));
    CHECK(!tilefinch_csp_allows_request(
        &csp, TILEFINCH_DESTINATION_SCRIPT,
        "https://sub.cdn.test/other/app.js"));

    static const char duplicate[] =
        "content-security-policy: img-src 'none'; img-src *\n";
    CHECK(tilefinch_csp_parse_response_headers(
        &csp, "https://page.test/", duplicate, sizeof(duplicate) - 1,
        false));
    CHECK(!tilefinch_csp_allows_request(
        &csp, TILEFINCH_DESTINATION_IMAGE, "https://img.test/a.png"));
    return 0;
}

/* CSP Level 3 element and attribute directives. weather.gov ships
   "script-src-elem 'self' ...; script-src 'unsafe-eval' 'unsafe-inline'":
   reading script-src for <script src> blocked every same-origin script. */
static int test_element_and_attribute_directives(void)
{
    static const char split[] =
        "content-security-policy: default-src 'none'; "
        "script-src-elem 'self' https://cdn.test 'unsafe-inline'; "
        "script-src 'unsafe-eval' 'unsafe-inline'; script-src-attr 'none'; "
        "style-src-elem 'self'; style-src 'none'; "
        "style-src-attr 'unsafe-inline'\n";
    static const char html[] =
        "<!doctype html><script>run()</script><style>p{}</style>";
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    budget_install_lexbor(&budget);
    PocDocument document = {0};
    CHECK(document_parse(
        &document, &budget, html, sizeof(html) - 1, sizeof(html)));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *script = find_element(root, "script");
    lxb_dom_node_t *style = find_element(root, "style");
    CHECK(script != NULL && style != NULL);
    TilefinchContentSecurityPolicy *csp = &document.content_security_policy;
    CHECK(tilefinch_csp_parse_response_headers(
        csp, "https://page.test/", split, sizeof(split) - 1u, false));
    CHECK(tilefinch_csp_allows_request(
        csp, TILEFINCH_DESTINATION_SCRIPT, "https://page.test/app.js"));
    CHECK(tilefinch_csp_allows_request(
        csp, TILEFINCH_DESTINATION_SCRIPT, "https://cdn.test/lib.js"));
    CHECK(!tilefinch_csp_allows_request(
        csp, TILEFINCH_DESTINATION_SCRIPT, "https://other.test/x.js"));
    CHECK(tilefinch_csp_allows_inline_script(csp, script));
    CHECK(!tilefinch_csp_allows_script_attribute(csp));
    /* eval is governed by script-src, never script-src-elem. */
    CHECK(tilefinch_csp_allows_dynamic_code(csp));
    CHECK(tilefinch_csp_allows_request(
        csp, TILEFINCH_DESTINATION_STYLE, "https://page.test/site.css"));
    CHECK(!tilefinch_csp_allows_inline_style(csp, style));
    CHECK(tilefinch_csp_allows_style_attribute(csp));
    /* Images still fall back to default-src 'none'. */
    CHECK(!tilefinch_csp_allows_request(
        csp, TILEFINCH_DESTINATION_IMAGE, "https://page.test/a.png"));

    /* Absent element/attribute directives fall back to script-src and
       style-src before default-src. */
    static const char family[] =
        "content-security-policy: default-src 'none'; "
        "script-src https://cdn.test 'unsafe-inline'; style-src 'self'\n";
    CHECK(tilefinch_csp_parse_response_headers(
        csp, "https://page.test/", family, sizeof(family) - 1u, false));
    CHECK(tilefinch_csp_allows_request(
        csp, TILEFINCH_DESTINATION_SCRIPT, "https://cdn.test/lib.js"));
    CHECK(!tilefinch_csp_allows_request(
        csp, TILEFINCH_DESTINATION_SCRIPT, "https://page.test/app.js"));
    CHECK(tilefinch_csp_allows_inline_script(csp, script));
    CHECK(tilefinch_csp_allows_script_attribute(csp));
    CHECK(tilefinch_csp_allows_request(
        csp, TILEFINCH_DESTINATION_STYLE, "https://page.test/site.css"));
    CHECK(!tilefinch_csp_allows_style_attribute(csp));
    document_destroy(&document);
    CHECK(budget.current == 0);
    return 0;
}

static int test_worker_and_dynamic_code_policy(void)
{
    TilefinchContentSecurityPolicy csp;
    static const char blocked[] =
        "content-security-policy: default-src 'self'; "
        "script-src 'self'; worker-src https://workers.test\n";
    CHECK(tilefinch_csp_parse_response_headers(
        &csp, "https://page.test/", blocked, sizeof(blocked) - 1u, false));
    CHECK(!tilefinch_csp_allows_dynamic_code(&csp));
    CHECK(tilefinch_csp_allows_worker(
        &csp, "https://workers.test/task.js"));
    CHECK(!tilefinch_csp_allows_worker(
        &csp, "https://page.test/task.js"));

    static const char allowed[] =
        "content-security-policy: script-src 'self' 'unsafe-eval'; "
        "worker-src 'self' blob:\n";
    CHECK(tilefinch_csp_parse_response_headers(
        &csp, "https://page.test/", allowed, sizeof(allowed) - 1u, false));
    CHECK(tilefinch_csp_allows_dynamic_code(&csp));
    CHECK(tilefinch_csp_allows_worker(&csp, "https://page.test/task.js"));
    CHECK(tilefinch_csp_allows_worker(
        &csp, "blob:https://page.test/worker-1"));
    CHECK(!tilefinch_csp_allows_worker(&csp, "https://other.test/task.js"));

    static const char intersection[] =
        "content-security-policy: script-src 'self' 'unsafe-eval'\n"
        "content-security-policy: script-src 'self'\n";
    CHECK(tilefinch_csp_parse_response_headers(
        &csp, "https://page.test/", intersection,
        sizeof(intersection) - 1u, false));
    CHECK(!tilefinch_csp_allows_dynamic_code(&csp));
    return 0;
}

static int test_inline_nonce_and_hash(void)
{
    static const char html[] =
        "<!doctype html><script nonce='good'>alert(1)</script>"
        "<script id='empty'></script>"
        "<style>alert(1)</style>"
        "<p style='color:#123456'>styled</p>";
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    budget_install_lexbor(&budget);
    PocDocument document = {0};
    CHECK(document_parse(
        &document, &budget, html, sizeof(html) - 1, sizeof(html)));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *script = find_element(root, "script");
    lxb_dom_node_t *empty_script = find_element_by_id(root, "empty");
    lxb_dom_node_t *style = find_element(root, "style");
    lxb_dom_node_t *paragraph = find_element(root, "p");
    CHECK(script != NULL && empty_script != NULL && style != NULL
          && paragraph != NULL);

    static const char allowed[] =
        "content-security-policy: script-src 'nonce-good'; "
        "style-src 'sha256-bhHHL3z2vDgxUt0W3dWQOrprscmda2Y5pLsLg4GF+pI='\n";
    CHECK(tilefinch_csp_parse_response_headers(
        &document.content_security_policy, "https://page.test/", allowed,
        sizeof(allowed) - 1, false));
    /* Only a policy with a hash source digests the element's text. */
    size_t digests = tilefinch_csp_inline_digests();
    CHECK(tilefinch_csp_allows_inline_script(
        &document.content_security_policy, script));
    CHECK(tilefinch_csp_inline_digests() == digests);
    CHECK(!tilefinch_csp_allows_script_attribute(
        &document.content_security_policy));
    CHECK(tilefinch_csp_allows_inline_style(
        &document.content_security_policy, style));
    CHECK(tilefinch_csp_inline_digests() == digests + 1u);
    CHECK(!tilefinch_csp_allows_style_attribute(
        &document.content_security_policy));

    static const char blocked[] =
        "content-security-policy: script-src 'unsafe-inline' 'nonce-other'; "
        "style-src 'none'\n";
    CHECK(tilefinch_csp_parse_response_headers(
        &document.content_security_policy, "https://page.test/", blocked,
        sizeof(blocked) - 1, false));
    CHECK(!tilefinch_csp_allows_inline_script(
        &document.content_security_policy, script));
    CHECK(!tilefinch_csp_allows_script_attribute(
        &document.content_security_policy));
    CHECK(!tilefinch_csp_allows_inline_style(
        &document.content_security_policy, style));

    static const char empty_hash[] =
        "content-security-policy: script-src "
        "'sha256-47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU='\n";
    CHECK(tilefinch_csp_parse_response_headers(
        &document.content_security_policy, "https://page.test/", empty_hash,
        sizeof(empty_hash) - 1, false));
    CHECK(tilefinch_csp_allows_inline_script(
        &document.content_security_policy, empty_script));

    CHECK(tilefinch_csp_parse_response_headers(
        &document.content_security_policy, "https://page.test/", blocked,
        sizeof(blocked) - 1, false));
    Stylesheet sheet = {0};
    CHECK(stylesheet_build(&sheet, &budget, &document, 480));
    ComputedStyle computed = style_for_node(&sheet, paragraph, NULL);
    CHECK(sheet.block_inline_style_attributes
          && computed.color != 0x123456u);
    document_style_attribute_set_cssom_authorized(paragraph, true);
    CHECK(document_style_attribute_cssom_authorized(paragraph));
    computed = style_for_node(&sheet, paragraph, NULL);
    CHECK(computed.color == 0x123456u);
    document_style_attribute_set_cssom_authorized(paragraph, false);
    CHECK(!document_style_attribute_cssom_authorized(paragraph));
    stylesheet_destroy(&sheet);

    static const char unsafe_inline[] =
        "content-security-policy: style-src 'unsafe-inline'\n";
    CHECK(tilefinch_csp_parse_response_headers(
        &document.content_security_policy, "https://page.test/",
        unsafe_inline, sizeof(unsafe_inline) - 1, false));
    digests = tilefinch_csp_inline_digests();
    CHECK(tilefinch_csp_allows_inline_style(
        &document.content_security_policy, style)
          && tilefinch_csp_inline_digests() == digests);
    CHECK(tilefinch_csp_allows_style_attribute(
        &document.content_security_policy));
    CHECK(stylesheet_build(&sheet, &budget, &document, 480));
    computed = style_for_node(&sheet, paragraph, NULL);
    CHECK(!sheet.block_inline_style_attributes
          && computed.color == 0x123456u);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0);
    return 0;
}

/* CSP3 nonce, integrity and 'strict-dynamic' pre-request checks, the
   grant a request context carries across redirects, and the inline rules
   'strict-dynamic' changes. Reddit ships "script-src 'self'
   'strict-dynamic' 'nonce-...'" and loads everything through nonced and
   script-inserted scripts. */
static int test_nonce_and_strict_dynamic_requests(void)
{
    TilefinchContentSecurityPolicy csp;
    static const char nonce_only[] =
        "content-security-policy: default-src 'none'; "
        "script-src 'nonce-abc+/=' https://cdn.test; style-src 'nonce-abc+/='\n";
    CHECK(tilefinch_csp_parse_response_headers(
        &csp, "https://page.test/", nonce_only, sizeof(nonce_only) - 1u,
        false));
    uint8_t good = tilefinch_csp_request_grant(
        &csp, TILEFINCH_DESTINATION_SCRIPT, "abc+/=", 6, NULL, 0, true);
    uint8_t bad = tilefinch_csp_request_grant(
        &csp, TILEFINCH_DESTINATION_SCRIPT, "abc+/", 5, NULL, 0, true);
    uint8_t none = tilefinch_csp_request_grant(
        &csp, TILEFINCH_DESTINATION_SCRIPT, NULL, 0, NULL, 0, true);
    CHECK(tilefinch_csp_allows_request_granted(
        &csp, TILEFINCH_DESTINATION_SCRIPT, "https://other.test/a.js", good));
    CHECK(tilefinch_csp_allows_request_granted(
        &csp, TILEFINCH_DESTINATION_SCRIPT, "data:text/javascript,1", good));
    CHECK(!tilefinch_csp_allows_request_granted(
        &csp, TILEFINCH_DESTINATION_SCRIPT, "https://other.test/a.js", bad));
    CHECK(!tilefinch_csp_allows_request_granted(
        &csp, TILEFINCH_DESTINATION_SCRIPT, "https://other.test/a.js", none));
    /* Without 'strict-dynamic' the host list still applies to everyone. */
    CHECK(tilefinch_csp_allows_request_granted(
        &csp, TILEFINCH_DESTINATION_SCRIPT, "https://cdn.test/a.js", none));
    CHECK(tilefinch_csp_allows_request(
        &csp, TILEFINCH_DESTINATION_SCRIPT, "https://cdn.test/a.js"));
    CHECK(!tilefinch_csp_allows_request(
        &csp, TILEFINCH_DESTINATION_SCRIPT, "https://other.test/a.js"));
    /* Styles honor nonces; other destinations never take a grant. */
    uint8_t style = tilefinch_csp_request_grant(
        &csp, TILEFINCH_DESTINATION_STYLE, "abc+/=", 6, NULL, 0, true);
    CHECK(tilefinch_csp_allows_request_granted(
        &csp, TILEFINCH_DESTINATION_STYLE, "https://other.test/s.css",
        style));
    CHECK(tilefinch_csp_request_grant(
              &csp, TILEFINCH_DESTINATION_IMAGE, "abc+/=", 6, NULL, 0,
              false) == 0);

    static const char strict[] =
        "content-security-policy: script-src 'self' 'strict-dynamic' "
        "'nonce-R3dd1t' https://cdn.test 'unsafe-inline'\n";
    CHECK(tilefinch_csp_parse_response_headers(
        &csp, "https://page.test/", strict, sizeof(strict) - 1u, false));
    uint8_t parser_nonced = tilefinch_csp_request_grant(
        &csp, TILEFINCH_DESTINATION_SCRIPT, "R3dd1t", 6, NULL, 0, true);
    uint8_t parser_bare = tilefinch_csp_request_grant(
        &csp, TILEFINCH_DESTINATION_SCRIPT, NULL, 0, NULL, 0, true);
    uint8_t inserted = tilefinch_csp_request_grant(
        &csp, TILEFINCH_DESTINATION_SCRIPT, NULL, 0, NULL, 0, false);
    CHECK(parser_bare == TILEFINCH_CSP_GRANT_PARSER_INSERTED);
    CHECK(tilefinch_csp_allows_request_granted(
        &csp, TILEFINCH_DESTINATION_SCRIPT, "https://x.test/a.js",
        parser_nonced));
    /* 'self' and the host list are ignored for parser-inserted scripts. */
    CHECK(!tilefinch_csp_allows_request_granted(
        &csp, TILEFINCH_DESTINATION_SCRIPT, "https://page.test/a.js",
        parser_bare));
    CHECK(!tilefinch_csp_allows_request_granted(
        &csp, TILEFINCH_DESTINATION_SCRIPT, "https://cdn.test/a.js",
        parser_bare));
    /* A script-inserted (or imported) script needs nothing. */
    CHECK(tilefinch_csp_allows_request_granted(
        &csp, TILEFINCH_DESTINATION_SCRIPT, "https://x.test/a.js",
        inserted));
    CHECK(tilefinch_csp_allows_worker(&csp, "https://x.test/worker.js"));
    /* eval is unchanged: no 'unsafe-eval'. */
    CHECK(!tilefinch_csp_allows_dynamic_code(&csp));
    /* 'strict-dynamic' disables 'unsafe-inline' for script attributes. */
    CHECK(!tilefinch_csp_allows_script_attribute(&csp));

    /* Each enforced policy must admit the request on its own terms. */
    static const char two[] =
        "content-security-policy: script-src 'nonce-one'\n"
        "content-security-policy: script-src https://cdn.test\n";
    CHECK(tilefinch_csp_parse_response_headers(
        &csp, "https://page.test/", two, sizeof(two) - 1u, false));
    uint8_t one = tilefinch_csp_request_grant(
        &csp, TILEFINCH_DESTINATION_SCRIPT, "one", 3, NULL, 0, true);
    CHECK(one == (TILEFINCH_CSP_GRANT_PARSER_INSERTED | 1u));
    CHECK(tilefinch_csp_allows_request_granted(
        &csp, TILEFINCH_DESTINATION_SCRIPT, "https://cdn.test/a.js", one));
    CHECK(!tilefinch_csp_allows_request_granted(
        &csp, TILEFINCH_DESTINATION_SCRIPT, "https://x.test/a.js", one));

    /* The grant rides the prepared request to the transport, which applies
       it to the initial URL and to every redirect hop. */
    CHECK(tilefinch_csp_parse_response_headers(
        &csp, "https://page.test/", nonce_only, sizeof(nonce_only) - 1u,
        false));
    for (size_t pass = 0; pass < 2; pass++) {
        TilefinchRequestContext context = {
            .target_url = "https://other.test/a.js",
            .initiator_url = "https://page.test/",
            .top_level_url = "https://page.test/", .method = "GET",
            .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
            .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
            .destination = TILEFINCH_DESTINATION_SCRIPT,
            .csp_grant = pass == 0 ? good : none
        };
        FetchPreparedPageRequest prepared;
        CHECK(fetch_prepare_page_request_context(
            &context, "https://page.test/", "", NULL, &csp, NULL, NULL,
            &prepared, NULL));
        const FetchRequest *request = fetch_prepared_page_request(&prepared);
        CHECK(request != NULL);
        CHECK(fetch_request_security_allows_target(
                  request, "https://redirected.test/b.js") == (pass == 0));
    }
    return 0;
}

/* CSP3 hash-source for external scripts: every recognized SRI hash of the
   element must be listed; unknown algorithms and options are ignored. */
static int test_integrity_hash_requests(void)
{
    TilefinchContentSecurityPolicy csp;
    static const char hashes[] =
        "content-security-policy: script-src 'sha256-AAAA' 'SHA384-BBBB'\n";
    CHECK(tilefinch_csp_parse_response_headers(
        &csp, "https://page.test/", hashes, sizeof(hashes) - 1u, false));
    static const char *const admitted[] = {
        "sha256-AAAA", "sha384-BBBB?ct=application/javascript",
        "sha256-AAAA md5-ZZZZ", " sha256-AAAA  sha384-BBBB "
    };
    static const char *const refused[] = {
        "sha256-AAAB", "sha256-AAAA sha512-CCCC", "md5-ZZZZ", "",
        "sha256-AAAAA"
    };
    for (size_t i = 0; i < sizeof(admitted) / sizeof(admitted[0]); i++) {
        uint8_t grant = tilefinch_csp_request_grant(
            &csp, TILEFINCH_DESTINATION_SCRIPT, NULL, 0, admitted[i],
            strlen(admitted[i]), true);
        CHECK(tilefinch_csp_allows_request_granted(
            &csp, TILEFINCH_DESTINATION_SCRIPT, "https://x.test/a.js",
            grant));
    }
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        uint8_t grant = tilefinch_csp_request_grant(
            &csp, TILEFINCH_DESTINATION_SCRIPT, NULL, 0, refused[i],
            strlen(refused[i]), true);
        CHECK(!tilefinch_csp_allows_request_granted(
            &csp, TILEFINCH_DESTINATION_SCRIPT, "https://x.test/a.js",
            grant));
    }
    /* Integrity never admits a stylesheet. */
    CHECK(tilefinch_csp_request_grant(
              &csp, TILEFINCH_DESTINATION_STYLE, NULL, 0, "sha256-AAAA", 11,
              true) == 0);
    return 0;
}

/* The element nonce is the hidden [[CryptographicNonce]] slot, and an
   element whose attributes carry "<script" (dangling markup that swallowed
   a real nonce) is not nonceable. 'strict-dynamic' and every hash
   algorithm disable 'unsafe-inline'. */
static int test_inline_slots_and_strict_dynamic(void)
{
    static const char html[] =
        "<!doctype html><script nonce='n1' id=a>run()</script>"
        "<script id=b>run()</script>"
        "<script nonce='n1' id=c data-x='<script src=x>'>run()</script>"
        "<style nonce='n1' id=d>p{}</style>";
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    budget_install_lexbor(&budget);
    PocDocument document = {0};
    CHECK(document_parse(
        &document, &budget, html, sizeof(html) - 1, sizeof(html)));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *a = find_element_by_id(root, "a");
    lxb_dom_node_t *b = find_element_by_id(root, "b");
    lxb_dom_node_t *c = find_element_by_id(root, "c");
    lxb_dom_node_t *d = find_element_by_id(root, "d");
    CHECK(a != NULL && b != NULL && c != NULL && d != NULL);
    TilefinchContentSecurityPolicy *csp = &document.content_security_policy;
    static const char strict[] =
        "content-security-policy: script-src 'nonce-n1' 'strict-dynamic' "
        "'unsafe-inline'; style-src 'nonce-n1'\n";
    CHECK(tilefinch_csp_parse_response_headers(
        csp, "https://page.test/", strict, sizeof(strict) - 1u, false));
    /* A policy that arrives after parsing (a frame) hides what exists. */
    CHECK(document_nonce_hiding_enable(&document));
    size_t length = 0;
    const char *attribute = document_attribute(a, "nonce", &length);
    CHECK(attribute != NULL && length == 0);
    const char *slot = document_element_nonce(a, &length);
    CHECK(slot != NULL && length == 2 && memcmp(slot, "n1", 2) == 0);
    CHECK(tilefinch_csp_allows_inline_script(csp, a));
    CHECK(tilefinch_csp_allows_inline_style(csp, d));
    /* 'unsafe-inline' is ignored next to 'strict-dynamic'. */
    CHECK(!tilefinch_csp_allows_inline_script(csp, b));
    CHECK(!tilefinch_csp_allows_inline_script(csp, c));
    CHECK(tilefinch_csp_element_grant(
              csp, TILEFINCH_DESTINATION_SCRIPT, c, true)
          == TILEFINCH_CSP_GRANT_PARSER_INSERTED);
    CHECK(tilefinch_csp_element_grant(
              csp, TILEFINCH_DESTINATION_SCRIPT, a, true)
          == (TILEFINCH_CSP_GRANT_PARSER_INSERTED | 1u));
    /* An author write to the attribute replaces the slot. */
    CHECK(lxb_dom_element_set_attribute(
              lxb_dom_interface_element(a), (const lxb_char_t *) "nonce", 5,
              (const lxb_char_t *) "n2", 2) != NULL);
    slot = document_element_nonce(a, &length);
    CHECK(slot != NULL && length == 2 && memcmp(slot, "n2", 2) == 0);
    CHECK(!tilefinch_csp_allows_inline_script(csp, a));
    /* The IDL setter writes the slot only. */
    CHECK(document_element_set_nonce(&document, b, "n1", 2));
    CHECK(document_attribute(b, "nonce", &length) == NULL);
    CHECK(tilefinch_csp_allows_inline_script(csp, b));

    static const char sha384[] =
        "content-security-policy: script-src 'unsafe-inline' "
        "'sha384-AAAA'\n";
    CHECK(tilefinch_csp_parse_response_headers(
        csp, "https://page.test/", sha384, sizeof(sha384) - 1u, false));
    CHECK(!tilefinch_csp_allows_inline_script(csp, c));
    document_destroy(&document);
    CHECK(budget.current == 0);
    return 0;
}

/* A form control keeps its native value state beside a nonce slot, in
   either order: hiding a nonce must not make the field uneditable, and an
   edited field's nonce is still hidden. */
static int test_control_value_beside_nonce(void)
{
    static const char html[] =
        "<!doctype html><body><input id=a nonce='n1' value='abc'>"
        "<input id=b value='abc'></body>";
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    budget_install_lexbor(&budget);
    PocDocument document = {0};
    CHECK(document_parse(
        &document, &budget, html, sizeof(html) - 1, sizeof(html)));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *a = find_element_by_id(root, "a");
    lxb_dom_node_t *b = find_element_by_id(root, "b");
    CHECK(a != NULL && b != NULL);
    /* b: edited first, then given a nonce. */
    CHECK(document_control_value_set(&document, b, "first", 5));
    CHECK(lxb_dom_element_set_attribute(
              lxb_dom_interface_element(b), (const lxb_char_t *) "nonce", 5,
              (const lxb_char_t *) "n1", 2) != NULL);
    static const char policy[] =
        "content-security-policy: script-src 'nonce-n1'\n";
    CHECK(tilefinch_csp_parse_response_headers(
        &document.content_security_policy, "https://page.test/", policy,
        sizeof(policy) - 1u, false));
    CHECK(document_nonce_hiding_enable(&document));
    size_t length = 0;
    const char *nonce = document_element_nonce(a, &length);
    CHECK(nonce != NULL && length == 2 && memcmp(nonce, "n1", 2) == 0);
    CHECK(document_attribute(a, "nonce", &length) != NULL && length == 0);
    /* a: hidden first, then edited. */
    CHECK(document_control_value_set(&document, a, "zzz", 3));
    const char *value = document_control_value(a, &length);
    CHECK(value != NULL && length == 3 && memcmp(value, "zzz", 3) == 0);
    nonce = document_element_nonce(a, &length);
    CHECK(nonce != NULL && length == 2 && memcmp(nonce, "n1", 2) == 0);
    value = document_control_value(b, &length);
    CHECK(value != NULL && length == 5 && memcmp(value, "first", 5) == 0);
    nonce = document_element_nonce(b, &length);
    CHECK(nonce != NULL && length == 2 && memcmp(nonce, "n1", 2) == 0);
    CHECK(document_attribute(b, "nonce", &length) != NULL && length == 0);
    document_destroy(&document);
    CHECK(budget.current == 0);
    return 0;
}

static int test_bounded_failure_and_framing(void)
{
    TilefinchContentSecurityPolicy csp;
    CHECK(!tilefinch_csp_parse_response_headers(
        &csp, "https://child.test/", NULL, 0, true));
    CHECK(!csp.valid);

    /* A live large-site policy crossed 4 KiB in 2026. Preserve the bounded
       fail-closed design without regressing ordinary large-site headers. */
    char large[6000];
    size_t used = (size_t) snprintf(
        large, sizeof(large), "content-security-policy: default-src ");
    static const char source[] = "https://assets.example ";
    while (used + sizeof(source) + 2u < 4700u) {
        memcpy(large + used, source, sizeof(source) - 1u);
        used += sizeof(source) - 1u;
    }
    large[used++] = '\n';
    large[used] = '\0';
    CHECK(used > 4096u && used < FETCH_RESPONSE_HEADERS_LIMIT);
    CHECK(tilefinch_csp_parse_response_headers(
        &csp, "https://child.test/", large, used, false));
    CHECK(csp.valid && csp.header_present && csp.policy_count == 1u);

    static const char too_many[] =
        "content-security-policy: default-src 'self'\n"
        "content-security-policy: default-src 'self'\n"
        "content-security-policy: default-src 'self'\n"
        "content-security-policy: default-src 'self'\n"
        "content-security-policy: default-src 'self'\n";
    CHECK(!tilefinch_csp_parse_response_headers(
        &csp, "https://child.test/", too_many, sizeof(too_many) - 1,
        false));
    CHECK(!csp.valid);

    static const char xfo_deny[] = "x-frame-options: DENY\n";
    CHECK(tilefinch_csp_parse_response_headers(
        &csp, "https://child.test/", xfo_deny, sizeof(xfo_deny) - 1,
        false));
    CHECK(!tilefinch_frame_embedding_allowed(
        &csp, "https://child.test/", "https://child.test/parent",
        xfo_deny, sizeof(xfo_deny) - 1));

    static const char xfo_same[] = "x-frame-options: SAMEORIGIN\n";
    CHECK(tilefinch_csp_parse_response_headers(
        &csp, "https://child.test/", xfo_same, sizeof(xfo_same) - 1,
        false));
    CHECK(tilefinch_frame_embedding_allowed(
        &csp, "https://child.test/", "https://child.test/parent",
        xfo_same, sizeof(xfo_same) - 1));
    CHECK(!tilefinch_frame_embedding_allowed(
        &csp, "https://child.test/", "https://parent.test/",
        xfo_same, sizeof(xfo_same) - 1));

    static const char csp_overrides_xfo[] =
        "content-security-policy: frame-ancestors https://parent.test\n"
        "x-frame-options: DENY\n";
    CHECK(tilefinch_csp_parse_response_headers(
        &csp, "https://child.test/", csp_overrides_xfo,
        sizeof(csp_overrides_xfo) - 1, false));
    CHECK(tilefinch_frame_embedding_allowed(
        &csp, "https://child.test/", "https://parent.test/path",
        csp_overrides_xfo, sizeof(csp_overrides_xfo) - 1));
    CHECK(!tilefinch_frame_embedding_allowed(
        &csp, "https://child.test/", "https://other.test/path",
        csp_overrides_xfo, sizeof(csp_overrides_xfo) - 1));
    return 0;
}

int main(void)
{
    CHECK(test_directives() == 0);
    CHECK(test_source_matching_and_intersection() == 0);
    CHECK(test_element_and_attribute_directives() == 0);
    CHECK(test_worker_and_dynamic_code_policy() == 0);
    CHECK(test_inline_nonce_and_hash() == 0);
    CHECK(test_nonce_and_strict_dynamic_requests() == 0);
    CHECK(test_integrity_hash_requests() == 0);
    CHECK(test_inline_slots_and_strict_dynamic() == 0);
    CHECK(test_control_value_beside_nonce() == 0);
    CHECK(test_bounded_failure_and_framing() == 0);
    puts("content security policy tests passed");
    return 0;
}
