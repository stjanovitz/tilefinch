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
        "form-action https://submit.test; frame-ancestors 'self'\n";
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
    CHECK(tilefinch_csp_allows_inline_script(
        &document.content_security_policy, script));
    CHECK(!tilefinch_csp_allows_script_attribute(
        &document.content_security_policy));
    CHECK(tilefinch_csp_allows_inline_style(
        &document.content_security_policy, style));
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
    CHECK(test_worker_and_dynamic_code_policy() == 0);
    CHECK(test_inline_nonce_and_hash() == 0);
    CHECK(test_bounded_failure_and_framing() == 0);
    puts("content security policy tests passed");
    return 0;
}
