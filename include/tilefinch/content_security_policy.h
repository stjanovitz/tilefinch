#ifndef TILEFINCH_CONTENT_SECURITY_POLICY_H
#define TILEFINCH_CONTENT_SECURITY_POLICY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/request_context.h"
#include "tilefinch/url.h"

#define TILEFINCH_CSP_POLICY_LIMIT 4u
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
    TILEFINCH_CSP_DIRECTIVE_COUNT
} TilefinchCspDirective;

typedef struct {
    uint16_t offset;
    uint16_t length;
    bool present;
} TilefinchCspDirectiveValue;

typedef struct {
    TilefinchCspDirectiveValue directives[TILEFINCH_CSP_DIRECTIVE_COUNT];
} TilefinchCspPolicy;

typedef struct TilefinchContentSecurityPolicy {
    char document_origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    char storage[TILEFINCH_CSP_STORAGE_BYTES];
    TilefinchCspPolicy policies[TILEFINCH_CSP_POLICY_LIMIT];
    size_t storage_used;
    size_t policy_count;
    bool header_present;
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
bool tilefinch_csp_allows_request(
    const TilefinchContentSecurityPolicy *policy,
    TilefinchRequestDestination destination, const char *target_url);
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
bool tilefinch_csp_allows_base_uri(
    const TilefinchContentSecurityPolicy *policy, const char *target_url);
bool tilefinch_csp_allows_form_action(
    const TilefinchContentSecurityPolicy *policy, const char *target_url);
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
