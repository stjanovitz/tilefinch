#ifndef TILEFINCH_CONTENT_SECURITY_POLICY_H
#define TILEFINCH_CONTENT_SECURITY_POLICY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/request_context.h"
#include "tilefinch/url.h"

/* Policies a response's header fields may deliver, and the total once
   <meta http-equiv="Content-Security-Policy"> policies join them. */
#define TILEFINCH_CSP_HEADER_POLICY_LIMIT 4u
#define TILEFINCH_CSP_POLICY_LIMIT 7u
/* Bounded independently from the response snapshot. Large production CSPs
   commonly exceed 4 KiB even after unrelated response fields are discarded. */
#define TILEFINCH_CSP_STORAGE_BYTES 8192u

typedef enum {
    TILEFINCH_CSP_DEFAULT_SRC = 0,
    TILEFINCH_CSP_SCRIPT_SRC,
    TILEFINCH_CSP_STYLE_SRC,
    TILEFINCH_CSP_IMG_SRC,
    TILEFINCH_CSP_FONT_SRC,
    TILEFINCH_CSP_CONNECT_SRC,
    TILEFINCH_CSP_FRAME_SRC,
    TILEFINCH_CSP_OBJECT_SRC,
    TILEFINCH_CSP_BASE_URI,
    TILEFINCH_CSP_FORM_ACTION,
    TILEFINCH_CSP_FRAME_ANCESTORS,
    TILEFINCH_CSP_WORKER_SRC,
    TILEFINCH_CSP_MEDIA_SRC,
    /* CSP Level 3 element/attribute directives. Absent, each falls back to
       its family (script-src or style-src) and then default-src. Keep the
       two script entries first: the fallback is chosen by position. */
    TILEFINCH_CSP_SCRIPT_SRC_ELEM,
    TILEFINCH_CSP_SCRIPT_SRC_ATTR,
    TILEFINCH_CSP_STYLE_SRC_ELEM,
    TILEFINCH_CSP_STYLE_SRC_ATTR,
    TILEFINCH_CSP_DIRECTIVE_COUNT
} TilefinchCspDirective;

typedef struct {
    uint16_t offset;
    uint16_t length;
    bool present;
} TilefinchCspDirectiveValue;

typedef struct {
    TilefinchCspDirectiveValue directives[TILEFINCH_CSP_DIRECTIVE_COUNT];
    /* The policy's serialized text in `storage`. */
    uint16_t text_offset;
    uint16_t text_length;
    /* Delivered by a <meta> element rather than a header field. */
    bool meta;
} TilefinchCspPolicy;

typedef struct TilefinchContentSecurityPolicy {
    char document_origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    char storage[TILEFINCH_CSP_STORAGE_BYTES];
    TilefinchCspPolicy policies[TILEFINCH_CSP_POLICY_LIMIT];
    size_t storage_used;
    size_t policy_count;
    /* A header field delivered a policy (nonce hiding follows only this). */
    bool header_present;
    /* False is fail-closed: every check refuses. */
    bool valid;
} TilefinchContentSecurityPolicy;

struct lxb_dom_node;

void tilefinch_csp_init(TilefinchContentSecurityPolicy *policy);
/* Parses every CSP field from the normalized newline-delimited response
   snapshot. A security-header truncation is a hard failure, never a reason
   to accept an incomplete policy. */
bool tilefinch_csp_parse_response_headers(
    TilefinchContentSecurityPolicy *policy, const char *document_url,
    const char *headers, size_t headers_length,
    bool security_headers_truncated);
/* Whether any policy is enforced: one from a header field or a <meta>. */
bool tilefinch_csp_enforced(const TilefinchContentSecurityPolicy *policy);
/* CSP3 with HTML's http-equiv "Content-Security-Policy" state: the content
   of a <meta> in the document's head, appended as enforced policies (a
   comma separates policies, as in a header field) beside any header
   policies, all of which must allow. frame-ancestors is ignored in a meta
   policy (report-uri and sandbox are not implemented at all). A policy
   with the same text as an earlier meta policy changes nothing and is not
   stored again. Policies only accumulate, so this can only restrict; a
   policy that does not fit the bounded storage makes the policy
   fail-closed. Returns whether the enforced policy changed. */
bool tilefinch_csp_add_meta_policy(
    TilefinchContentSecurityPolicy *policy, const char *content,
    size_t content_length);
/* Records `document_url`'s origin for 'self' when no response header parse
   did (a document committed from memory). */
void tilefinch_csp_bind_document_origin(
    TilefinchContentSecurityPolicy *policy, const char *document_url);
bool tilefinch_csp_allows_request(
    const TilefinchContentSecurityPolicy *policy,
    TilefinchRequestDestination destination, const char *target_url);
/* CSP3 pre-request metadata (§6.7.1.1, "script directives pre-request
   check"), reduced to one byte a request context carries through every
   redirect hop. Bit i is set when enforced policy i matched the initiating
   element's nonce (scripts and styles) or integrity metadata (scripts),
   which admits the request whatever its URL. PARSER_INSERTED marks a
   parser-inserted script: 'strict-dynamic' refuses it unless matched.
   Zero is a script-initiated request without element metadata (a script-
   inserted element, module import, worker), which 'strict-dynamic' admits
   and which otherwise must match by URL. */
#define TILEFINCH_CSP_GRANT_PARSER_INSERTED 0x80u
_Static_assert(TILEFINCH_CSP_POLICY_LIMIT <= 7u,
               "policy bits must stay below the parser-inserted flag");
uint8_t tilefinch_csp_request_grant(
    const TilefinchContentSecurityPolicy *policy,
    TilefinchRequestDestination destination,
    const char *nonce, size_t nonce_length,
    const char *integrity, size_t integrity_length, bool parser_inserted);
/* The grant for an element: its [[CryptographicNonce]] when the element is
   nonceable, and for scripts its integrity attribute. */
uint8_t tilefinch_csp_element_grant(
    const TilefinchContentSecurityPolicy *policy,
    TilefinchRequestDestination destination,
    struct lxb_dom_node *element, bool parser_inserted);
/* Every nonce-source value of every enforced policy, for the document's
   nonce slots. */
void tilefinch_csp_visit_nonce_sources(
    const TilefinchContentSecurityPolicy *policy,
    void (*visit)(void *opaque, const char *value, size_t length),
    void *opaque);
/* CSP3 "Is element nonceable?": an attribute name or value containing
   "<script" or "<style" (ASCII case-insensitive) means dangling markup may
   have swallowed a legitimate nonce. */
bool tilefinch_csp_text_has_markup(const char *text, size_t length);
/* The grant a module script's descendants inherit (HTML "descendant script
   fetch options"): the root's nonce and parser metadata, never its
   integrity, which belongs to the root's own response. */
uint8_t tilefinch_csp_element_descendant_grant(
    const TilefinchContentSecurityPolicy *policy,
    struct lxb_dom_node *element, bool parser_inserted);
/* tilefinch_csp_allows_request() is this with a zero grant. */
bool tilefinch_csp_allows_request_granted(
    const TilefinchContentSecurityPolicy *policy,
    TilefinchRequestDestination destination, const char *target_url,
    uint8_t grant);
bool tilefinch_csp_allows_inline_script(
    const TilefinchContentSecurityPolicy *policy,
    struct lxb_dom_node *element);
bool tilefinch_csp_allows_script_attribute(
    const TilefinchContentSecurityPolicy *policy);
bool tilefinch_csp_allows_inline_style(
    const TilefinchContentSecurityPolicy *policy,
    struct lxb_dom_node *element);
bool tilefinch_csp_allows_style_attribute(
    const TilefinchContentSecurityPolicy *policy);
/* Inline element digests computed so far (process-wide, monotonic): an
   element is hashed only when a policy lists a 'sha256-' source. Counted
   only in builds with tracing or the validation log (a test probe). */
size_t tilefinch_csp_inline_digests(void);
bool tilefinch_csp_allows_base_uri(
    const TilefinchContentSecurityPolicy *policy, const char *target_url);
bool tilefinch_csp_allows_form_action(
    const TilefinchContentSecurityPolicy *policy, const char *target_url);
/* connect-src for a WebSocket. `request_url` is the socket's request URL,
   its http(s) spelling (ws as http, wss as https, after any HSTS upgrade),
   and sources match it as the ws(s) URL it stands for: `wss://host` and
   `ws:` name sockets. */
bool tilefinch_csp_allows_websocket(
    const TilefinchContentSecurityPolicy *policy, const char *request_url,
    uint8_t grant);
bool tilefinch_csp_allows_worker(
    const TilefinchContentSecurityPolicy *policy, const char *target_url);
/* Dynamic compilation (eval/Function and their aliases) is allowed only
   when every enforced script policy explicitly contains 'unsafe-eval'. */
bool tilefinch_csp_allows_dynamic_code(
    const TilefinchContentSecurityPolicy *policy);
bool tilefinch_csp_has_frame_ancestors(
    const TilefinchContentSecurityPolicy *policy);
bool tilefinch_csp_allows_ancestor(
    const TilefinchContentSecurityPolicy *policy, const char *ancestor_url);
/* CSP frame-ancestors takes precedence over X-Frame-Options. Without that
   directive, DENY and SAMEORIGIN are enforced from every bounded response
   header field. Truncated security metadata is rejected by the CSP parser
   before this function is called. */
bool tilefinch_frame_embedding_allowed(
    const TilefinchContentSecurityPolicy *policy,
    const char *response_url, const char *ancestor_url,
    const char *headers, size_t headers_length);

#endif
