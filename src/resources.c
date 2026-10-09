#include "tilefinch/resources.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tilefinch/fetch.h"
#include "tilefinch/content_blocker.h"
#include "tilefinch/platform.h"
#include "tilefinch/resource_integrity.h"
#include "tilefinch/url.h"

#define budget_malloc(b, s) budget_malloc_category((b), BUDGET_CATEGORY_RESOURCE, (s))
#define budget_calloc(b, n, s) budget_calloc_category((b), BUDGET_CATEGORY_RESOURCE, (n), (s))
#define budget_realloc(b, p, s) budget_realloc_category((b), BUDGET_CATEGORY_RESOURCE, (p), (s))

#define MAX_TRACKED_STYLESHEETS 32
#define STYLESHEET_FETCH_CONCURRENCY 4
#define STYLESHEET_FETCH_BATCH 2
/* Parsed-cost model for admission (stylesheet_source_cost_estimate). */
#define STYLESHEET_ESTIMATE_BLOCK_BYTES 512u
#define STYLESHEET_ESTIMATE_CUSTOM_PROPERTY_BYTES 176u
#define STYLESHEET_ESTIMATE_GROWTH_BYTES (128u * 1024u)
#define STYLESHEET_ESTIMATE_MINIMUM_BYTES (32u * 1024u)
#define STYLESHEET_LAYOUT_RESERVE (2u * 1024u * 1024u)
#define STYLESHEET_LARGE_SOURCE_BYTES (512u * 1024u)
#define STYLESHEET_LARGE_SOURCE_RULE_LIMIT 1572u
#define STYLESHEET_LARGE_SOURCE_HEAD_RULES 512u
#define STYLESHEET_LARGE_SOURCE_RELEVANT_RULES 768u
#define STYLESHEET_LARGE_SOURCE_SECONDARY_RULES 256u
#define STYLESHEET_LARGE_SOURCE_TAIL_PERCENT 95u
/* Memory-based admission of large stylesheets (stylesheet_response_bound):
   above a profile's per-file cap, when that cap is already large-sheet sized,
   one response may grow to this sanity ceiling if the page Budget can stage
   it. A response needs about its size for the body and five more for the
   large-source parse working set (stylesheet_source_cost_estimate measures
   4.9x on gitlab's and 4.4x on reuters' 768 KiB prefixes). */
#define STYLESHEET_FILE_CEILING_BYTES (2u * 1024u * 1024u)
#define STYLESHEET_STAGING_FACTOR 6u
/* Past its 768-rule allowance, a large sheet may keep up to this many more
   DOM-relevant rules while the Budget has STYLESHEET_ESTIMATE_BLOCK_BYTES per
   rule to spare beside the layout reserve. */
#define STYLESHEET_LARGE_SOURCE_EXTRA_RELEVANT_RULES 2304u
#define STYLESHEET_SELECTOR_TOKEN_BLOOM_WORDS 64u
#define STYLESHEET_PRIORITY_TOKEN_BLOOM_WORDS 128u
#define STYLESHEET_SELECTOR_PRIORITY_NODE_LIMIT 512u
/* Count text nodes too, but leave room for a hydrated document to add its
   first dynamic panels without immediately saturating the token census. */
#define STYLESHEET_SELECTOR_TOKEN_NODE_LIMIT 16384u

static uint32_t stylesheet_selector_token_hash(
    unsigned char kind, const char *text, size_t length)
{
    uint32_t hash = UINT32_C(2166136261) ^ kind;
    for (size_t i = 0; i < length; i++) {
        unsigned char value = (unsigned char) text[i];
        if (kind == 't') value = (unsigned char) tolower(value);
        hash ^= value;
        hash *= UINT32_C(16777619);
    }
    return hash;
}

static void stylesheet_selector_token_add_words(
    uint32_t *bloom, size_t bloom_words, unsigned char kind,
    const char *text, size_t length)
{
    if (bloom == NULL || bloom_words == 0 || text == NULL || length == 0) {
        return;
    }
    uint32_t hash = stylesheet_selector_token_hash(kind, text, length);
    uint32_t rotated = (hash << 13) | (hash >> 19);
    size_t bits = bloom_words * 32u;
    size_t first = (size_t) hash & (bits - 1u);
    size_t second = (size_t) (rotated ^ UINT32_C(0x9e3779b9))
        & (bits - 1u);
    bloom[first / 32u] |= UINT32_C(1) << (first & 31u);
    bloom[second / 32u] |= UINT32_C(1) << (second & 31u);
}

/* Returns true when the census visited every node under root, so the bloom
   answers "absent" only for tokens the document really lacks. */
static bool stylesheet_collect_selector_tokens(
    lxb_dom_node_t *root, uint32_t *bloom, size_t bloom_words,
    size_t node_limit, bool saturate_on_overflow)
{
    memset(bloom, 0, bloom_words * sizeof(*bloom));
    lxb_dom_node_t *node = root;
    size_t visited = 0;
    while (node != NULL && visited++ < node_limit) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            size_t name_length = 0;
            const char *name = document_element_name(node, &name_length);
            stylesheet_selector_token_add_words(
                bloom, bloom_words, 't', name, name_length);
            size_t id_length = 0;
            const char *id = document_attribute(node, "id", &id_length);
            stylesheet_selector_token_add_words(
                bloom, bloom_words, '#', id, id_length);
            size_t class_length = 0;
            const char *classes = document_attribute(
                node, "class", &class_length);
            for (size_t at = 0; classes != NULL && at < class_length;) {
                while (at < class_length
                       && isspace((unsigned char) classes[at])) at++;
                size_t begin = at;
                while (at < class_length
                       && !isspace((unsigned char) classes[at])) at++;
                stylesheet_selector_token_add_words(
                    bloom, bloom_words, '.', classes + begin, at - begin);
            }
        }
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != NULL && node != root && node->next == NULL) {
            node = node->parent;
        }
        node = node == NULL || node == root ? NULL : node->next;
    }
    if (node != NULL && saturate_on_overflow) {
        /* An incomplete census must not create false negatives. Saturating
           the fixed index converts the overflow case to ordinary bounded
           source order rather than pretending unseen tokens are absent. */
        memset(bloom, 0xff, bloom_words * sizeof(*bloom));
    }
    return node == NULL;
}

_Static_assert(STYLESHEET_REFERRER_POLICY_LIMIT
                   == FETCH_REFERRER_POLICY_LIMIT,
               "stylesheet and fetch policy storage must agree");

typedef struct {
    const char *response_url;
    char referrer_policy[STYLESHEET_REFERRER_POLICY_LIMIT];
    bool known;
} StylesheetResponseProvenance;

typedef struct {
    uint64_t request_id;
    char *url;
    char *retained_response_url;
    BrowserSharedBody *retained_body;
    char response_referrer_policy[STYLESHEET_REFERRER_POLICY_LIMIT];
    size_t maximum_bytes;
    bool owns_url;
    bool owns_response_url;
    bool response_provenance_known;
    bool retained_empty;
    bool apply_rules;
    bool preload;
    lxb_dom_node_t *element;
    bool cors;
    TilefinchCredentialsMode credentials;
    bool resource_grant_valid;
    TilefinchResourceGrant resource_grant;
    char content_type[128];
} PendingStylesheet;

typedef struct {
    Stylesheet *sheet;
    Budget *budget;
    const char *base_url;
    const char *document_url;
    const char *document_referrer_policy;
    const TilefinchContentSecurityPolicy *content_security_policy;
    size_t maximum_count;
    size_t maximum_total_bytes;
    size_t maximum_single_bytes;
    long timeout_ms;
    FetchScheduler *scheduler;
    BrowserSession *session;
    StylesheetDocumentResources *document_resources;
    uint32_t hashes[MAX_TRACKED_STYLESHEETS];
    bool hash_applied[MAX_TRACKED_STYLESHEETS];
    size_t hash_count;
    uint64_t alternate_theme_hashes[STYLESHEET_ALTERNATE_THEME_LIMIT];
    bool alternate_theme_active[STYLESHEET_ALTERNATE_THEME_LIMIT];
    size_t alternate_theme_count;
    bool alternate_theme_selection_valid;
    uint32_t selector_token_bloom[STYLESHEET_SELECTOR_TOKEN_BLOOM_WORDS];
    uint32_t priority_token_bloom[STYLESHEET_PRIORITY_TOKEN_BLOOM_WORDS];
    /* The whole-document census fit its node limit (no saturation). */
    bool selector_token_census_complete;
    ExternalStylesheetStats *stats;
    PendingStylesheet pending[STYLESHEET_FETCH_CONCURRENCY];
    size_t pending_count;
    size_t batch_limit;
    double started_ms;
    double deadline_ms;
    uint64_t slice_started_us;
    size_t slice_work_units;
    bool deadline_reached;
} ResourceContext;

static size_t stylesheet_estimate_add(size_t total, size_t amount)
{
    return amount > SIZE_MAX - total ? SIZE_MAX : total + amount;
}

static size_t stylesheet_estimate_scale(size_t count, size_t unit)
{
    return count > SIZE_MAX / unit ? SIZE_MAX : count * unit;
}

/* Rules, declarations and selector programs plus the rule index built on
   first use. Source bytes
   are a poor proxy (measured 1.0x to 5.6x across captured sites): a 2 MiB
   sheet of short rules costs about its own size, while a 50 KiB block of
   custom properties costs five times its size. The cost follows the number
   of rule blocks and custom-property declarations instead, which one
   allocation-free pass over the text counts (comments skipped; braces in
   strings over-count, which only makes the estimate more conservative).
   The coefficients bound every sheet of the calibration recorded in
   docs/engineering/MEMORY_EXPERIMENTS.md; a quarter is added for geometric
   array growth, plus a fixed allowance (ramped in for small sheets) for
   arrays and index tables that double on first use. */
size_t stylesheet_source_cost_estimate(const char *css, size_t length)
{
    if (css == NULL || length == 0) return 0;
    size_t blocks = 0;
    size_t custom_properties = 0;
    bool declaration_start = false;
    for (size_t at = 0; at < length; at++) {
        unsigned char value = (unsigned char) css[at];
        if (value == '/' && at + 1u < length && css[at + 1u] == '*') {
            at += 2u;
            while (at + 1u < length
                   && !(css[at] == '*' && css[at + 1u] == '/')) at++;
            at++;
            continue;
        }
        if (value == '{' || value == ';') {
            if (value == '{') blocks++;
            declaration_start = true;
            continue;
        }
        if (value == ' ' || value == '\t' || value == '\n'
            || value == '\r' || value == '\f') continue;
        if (declaration_start && value == '-' && at + 1u < length
            && css[at + 1u] == '-') custom_properties++;
        declaration_start = false;
    }
    size_t cost = stylesheet_estimate_add(
        stylesheet_estimate_scale(blocks, STYLESHEET_ESTIMATE_BLOCK_BYTES),
        stylesheet_estimate_scale(custom_properties,
                                  STYLESHEET_ESTIMATE_CUSTOM_PROPERTY_BYTES));
    cost = stylesheet_estimate_add(cost, cost / 4u);
    size_t growth = length < STYLESHEET_ESTIMATE_GROWTH_BYTES / 2u
        ? length * 2u : STYLESHEET_ESTIMATE_GROWTH_BYTES;
    cost = stylesheet_estimate_add(cost, growth);
    return cost < STYLESHEET_ESTIMATE_MINIMUM_BYTES
        ? STYLESHEET_ESTIMATE_MINIMUM_BYTES : cost;
}

/* A response cut at its byte cap ends wherever the cap fell: inside a
   declaration, a string, a comment, or an at-rule block. CSS parsing closes
   every open construct at end of input, so parsing the raw prefix would apply
   half a rule (or a whole @media block's worth of rules whose block was never
   closed). One allocation-free pass finds each top-level construct that ended
   on its own: a rule or at-rule block whose closing brace arrived, or a
   top-level statement such as @import ending in a ';' outside
   parentheses. Strings,
   comments, escapes and unquoted url() bodies are skipped the way the
   tokenizer consumes them, so braces and semicolons inside them never count,
   and input that ends inside any of them is unfinished. Constructed sheets
   split their text with the same scanner (js_css_statement_ends). */
bool stylesheet_next_statement(const char *css, size_t length,
                               size_t *offset, size_t *open_blocks)
{
    size_t depth = 0, parentheses = 0;
    if (css == NULL || offset == NULL) goto unfinished;
    for (size_t at = *offset; at < length; at++) {
        unsigned char value = (unsigned char) css[at];
        if (value == '/' && at + 1u < length && css[at + 1u] == '*') {
            size_t close = at + 2u;
            while (close + 1u < length
                   && !(css[close] == '*' && css[close + 1u] == '/')) {
                close++;
            }
            if (close + 1u >= length) goto unfinished;
            at = close + 1u;
            continue;
        }
        if (value == '\\') {
            /* An escape consumes the next code point; a trailing backslash
               is an incomplete token. */
            if (at + 1u >= length) goto unfinished;
            at++;
            continue;
        }
        if (value == '"' || value == '\'') {
            size_t close = at + 1u;
            bool closed = false;
            while (close < length) {
                unsigned char inner = (unsigned char) css[close];
                if (inner == '\\') {
                    if (close + 1u >= length) break;
                    close += 2u;
                    continue;
                }
                /* An unescaped newline ends a (bad) string token. */
                if (inner == value || inner == '\n' || inner == '\r'
                    || inner == '\f') {
                    closed = true;
                    break;
                }
                close++;
            }
            if (!closed) goto unfinished;
            at = close;
            continue;
        }
        if ((value == 'u' || value == 'U') && at + 3u < length
            && (css[at + 1u] == 'r' || css[at + 1u] == 'R')
            && (css[at + 2u] == 'l' || css[at + 2u] == 'L')
            && css[at + 3u] == '('
            && (at == 0
                || !(isalnum((unsigned char) css[at - 1u])
                     || css[at - 1u] == '-' || css[at - 1u] == '_'))) {
            size_t body = at + 4u;
            while (body < length
                   && (css[body] == ' ' || css[body] == '\t'
                       || css[body] == '\n' || css[body] == '\r'
                       || css[body] == '\f')) body++;
            if (body >= length) goto unfinished;
            if (css[body] != '"' && css[body] != '\'') {
                /* Unquoted url(): everything up to ')' is one token. */
                size_t close = body;
                while (close < length && css[close] != ')') {
                    if (css[close] == '\\') {
                        if (close + 1u >= length) goto unfinished;
                        close++;
                    }
                    close++;
                }
                if (close >= length) goto unfinished;
                at = close;
                continue;
            }
            /* A quoted url( is a function: the string is consumed above. */
            parentheses++;
            at += 3u;
            continue;
        }
        if (value == '(') {
            parentheses++;
        } else if (value == ')') {
            if (parentheses != 0) parentheses--;
        } else if (value == '{') {
            depth++;
        } else if (value == '}') {
            /* A stray top-level '}' joins the prelude of the rule it
               interrupts, as CSS Syntax consumes it: it ends nothing. */
            if (depth == 0) continue;
            if (--depth == 0) {
                *offset = at + 1u;
                return true;
            }
        } else if (value == ';' && depth == 0 && parentheses == 0) {
            *offset = at + 1u;
            return true;
        }
    }
    /* Even at depth zero the tail is unfinished: a selector without its
       block, or an @import whose media list may have been cut. */
unfinished:
    if (open_blocks != NULL) *open_blocks = depth;
    return false;
}

size_t stylesheet_complete_rules_prefix(const char *css, size_t length)
{
    size_t complete = 0;
    while (stylesheet_next_statement(css, length, &complete, NULL)) {}
    return complete;
}

/* Parsing a sheet must leave enough budget for authoritative layout and
   paint, so admission compares the estimate above plus that reserve with
   what remains, and omits this optional author source before the allocator
   can fail halfway through it. Earlier document-order sheets therefore win
   deterministically on constrained pages. Before refusing, the Budget's
   reclaim hook may release optional caches (module bytecode): an unstyled
   page is worse than a slower revisit. */
static bool stylesheet_parse_admitted(ResourceContext *context,
                                      size_t source_bytes, size_t working)
{
    if (context == NULL || context->budget == NULL || source_bytes == 0) {
        return true;
    }
    size_t required = stylesheet_estimate_add(
        working, STYLESHEET_LAYOUT_RESERVE);
    if (!budget_pressure_required(context->budget, working,
                                  STYLESHEET_LAYOUT_RESERVE)
        || (required != SIZE_MAX
            && budget_make_room(context->budget, required))) {
        return true;
    }
#ifndef TILEFINCH_NO_TRACE
    if (getenv("TILEFINCH_TRACE_STYLESHEETS") != NULL) {
        fprintf(stderr,
                "tilefinch-stylesheet-pressure source=%zu working=%zu "
                "remaining=%zu reserve=%u\n",
                source_bytes, working, budget_remaining(context->budget),
                (unsigned) STYLESHEET_LAYOUT_RESERVE);
    }
#endif
    context->stats->skipped_pressure++;
    budget_record_pressure(context->budget, BUDGET_PRESSURE_STYLESHEET,
                           working, 0);
    return false;
}

/* The largest response one stylesheet request may reserve: the profile's
   per-file cap, raised by memory when that cap is already large-sheet sized
   (the sheet's retained rules are then bounded by the large-source rule
   admission, so the byte cap only bounds staging). The extension is what the
   Budget can stage beside the layout reserve and the requests already in
   flight, up to STYLESHEET_FILE_CEILING_BYTES; small-memory profiles and a
   tight Budget keep the per-file cap, where the complete-rules prefix of a
   longer sheet still applies. `remaining` is the lane's aggregate room. */
static size_t stylesheet_response_bound(const ResourceContext *context,
                                        size_t remaining)
{
    size_t bound = context->maximum_single_bytes;
    if (bound >= STYLESHEET_LARGE_SOURCE_BYTES
        && bound < STYLESHEET_FILE_CEILING_BYTES
        && context->budget != NULL) {
        size_t reserved = STYLESHEET_LAYOUT_RESERVE;
        for (size_t i = 0; i < context->pending_count; i++) {
            reserved = stylesheet_estimate_add(
                reserved, stylesheet_estimate_scale(
                    context->pending[i].maximum_bytes,
                    STYLESHEET_STAGING_FACTOR));
        }
        size_t free_bytes = budget_remaining(context->budget);
        size_t affordable = free_bytes > reserved
            ? (free_bytes - reserved) / STYLESHEET_STAGING_FACTOR : 0;
        if (affordable > STYLESHEET_FILE_CEILING_BYTES)
            affordable = STYLESHEET_FILE_CEILING_BYTES;
        if (affordable > bound) bound = affordable;
    }
    return bound < remaining ? bound : remaining;
}

/* Style preloads are optional hints with a lane of their own: as many
   responses as active sheets but half the aggregate bytes. Active links are
   admitted against their own counters only, so no number of earlier preloads
   can refuse a page's CSS, while retained preload bodies stay bounded. */
static size_t style_preload_byte_limit(const ResourceContext *context)
{
    return context->maximum_total_bytes / 2u;
}

static StylesheetDocumentResource *document_resource_find(
    StylesheetDocumentResources *resources, const char *url)
{
    if (resources == NULL || url == NULL) return NULL;
    for (size_t i = 0; i < resources->count; i++) {
        if (resources->items[i].url != NULL
            && strcmp(resources->items[i].url, url) == 0) {
            return &resources->items[i];
        }
    }
    return NULL;
}

static StylesheetDocumentResource *document_resource_add(
    ResourceContext *context, const char *url, char **owned_url)
{
    StylesheetDocumentResources *resources = context->document_resources;
    if (resources == NULL || url == NULL
        || resources->count == STYLESHEET_DOCUMENT_RESOURCE_LIMIT) {
        return NULL;
    }
    char *copy = owned_url == NULL ? NULL : *owned_url;
    if (copy == NULL) {
        size_t length = strlen(url);
        copy = budget_malloc(context->budget, length + 1);
        if (copy == NULL) return NULL;
        memcpy(copy, url, length + 1);
    } else {
        *owned_url = NULL;
    }
    StylesheetDocumentResource *entry =
        &resources->items[resources->count++];
    memset(entry, 0, sizeof(*entry));
    entry->url = copy;
    return entry;
}

static StylesheetDocumentResource *document_resource_get_or_add(
    ResourceContext *context, const char *url, char **owned_url)
{
    StylesheetDocumentResource *entry = document_resource_find(
        context->document_resources, url);
    return entry != NULL ? entry
                         : document_resource_add(context, url, owned_url);
}

static bool stylesheet_referrer_policy_valid(const char *policy)
{
    return policy != NULL
        && tilefinch_referrer_policy_code(policy, strlen(policy), false)
               != TILEFINCH_REFERRER_POLICY_UNKNOWN;
}

static void stylesheet_referrer_policy_normalize_or_default(
    const char *policy, char output[STYLESHEET_REFERRER_POLICY_LIMIT])
{
    output[0] = '\0';
    if (policy == NULL) return;
    size_t length = strlen(policy);
    if (length == 0 || length >= STYLESHEET_REFERRER_POLICY_LIMIT) return;
    char normalized[STYLESHEET_REFERRER_POLICY_LIMIT];
    for (size_t i = 0; i < length; i++) {
        normalized[i] = (char) tolower((unsigned char) policy[i]);
    }
    normalized[length] = '\0';
    if (stylesheet_referrer_policy_valid(normalized)) {
        memcpy(output, normalized, length + 1u);
    }
}

static void stylesheet_referrer_policy_for_node(
    lxb_dom_node_t *node, const char *fallback,
    char output[STYLESHEET_REFERRER_POLICY_LIMIT])
{
    const char *selected = stylesheet_referrer_policy_valid(fallback)
        ? fallback : "";
    size_t length = 0;
    const char *attribute = document_attribute(
        node, "referrerpolicy", &length);
    char normalized[STYLESHEET_REFERRER_POLICY_LIMIT] = {0};
    if (attribute != NULL && length != 0 && length < sizeof(normalized)) {
        for (size_t i = 0; i < length; i++) {
            normalized[i] = (char) tolower((unsigned char) attribute[i]);
        }
        if (stylesheet_referrer_policy_valid(normalized)
            && normalized[0] != '\0') {
            selected = normalized;
        }
    }
    snprintf(output, STYLESHEET_REFERRER_POLICY_LIMIT, "%s", selected);
}

static bool stylesheet_response_provenance(
    const FetchResult *fetch, StylesheetResponseProvenance *provenance)
{
    if (fetch == NULL || provenance == NULL
        || fetch->effective_url[0] == '\0') return false;
    TilefinchUrl parsed;
    bool header_present = false;
    memset(provenance, 0, sizeof(*provenance));
    if (!tilefinch_url_parse(fetch->effective_url, &parsed)
        || !fetch_response_referrer_policy(
               fetch, &header_present, provenance->referrer_policy)) {
        return false;
    }
    /* For an external stylesheet, an absent or unknown-only response header
       leaves the sheet's stored policy empty.  Fetch resolves that to the UA
       default for descendant requests; it does not inherit the document or
       link request policy. */
    (void) header_present;
    provenance->response_url = fetch->effective_url;
    provenance->known = true;
    return true;
}

static bool stylesheet_revalidation_provenance(
    const FetchResult *fetch, const char *cached_response_url,
    const char *cached_referrer_policy, bool cached_known,
    StylesheetResponseProvenance *provenance)
{
    if (fetch == NULL || cached_response_url == NULL
        || cached_referrer_policy == NULL || !cached_known
        || provenance == NULL) return false;
    bool header_present = false;
    char response_policy[STYLESHEET_REFERRER_POLICY_LIMIT] = {0};
    if (!fetch_response_referrer_policy(fetch, &header_present,
                                        response_policy)) return false;
    memset(provenance, 0, sizeof(*provenance));
    provenance->response_url = fetch->effective_url[0] != '\0'
        ? fetch->effective_url : cached_response_url;
    const char *selected_policy = header_present
        ? response_policy : cached_referrer_policy;
    if (!stylesheet_referrer_policy_valid(selected_policy)) return false;
    snprintf(provenance->referrer_policy,
             sizeof(provenance->referrer_policy), "%s", selected_policy);
    provenance->known = true;
    return true;
}

static bool stylesheet_failure_is_terminal(const FetchResult *result)
{
    if (result == NULL) return false;
    if (result->status_code >= 400 && result->status_code <= 599) {
        return true;
    }
    return strstr(result->error, "response quota exceeded") != NULL
        || strstr(result->error, "unsupported") != NULL
        || strstr(result->error, "requested URL returned error:") != NULL
        || strstr(result->error, "redirect target is invalid") != NULL
        || strstr(result->error, "redirect limit") != NULL;
}

static bool stylesheet_http_status_is_error(const FetchResult *result)
{
    return result != NULL && result->status_code >= 400
        && result->status_code <= 599;
}

static void document_resource_record_failure(
    ResourceContext *context, const char *url, char **owned_url,
    const FetchResult *result, bool terminal)
{
    StylesheetDocumentResources *resources = context->document_resources;
    if (resources == NULL) return;
    StylesheetDocumentResource *entry = document_resource_get_or_add(
        context, url, owned_url);
    if (entry == NULL) return;
    if (entry->attempts != SIZE_MAX) entry->attempts++;
    if (terminal || stylesheet_failure_is_terminal(result)) {
        if (entry->state != STYLESHEET_DOCUMENT_RESOURCE_TERMINAL_FAILURE) {
            resources->terminal_failures++;
        }
        entry->state = STYLESHEET_DOCUMENT_RESOURCE_TERMINAL_FAILURE;
    } else {
        resources->transient_failures++;
        entry->state = STYLESHEET_DOCUMENT_RESOURCE_TRANSIENT_FAILURE;
    }
}

static void stylesheet_note_truncated(
    ResourceContext *context, const char *url, size_t received,
    size_t applied)
{
#ifndef TILEFINCH_NO_TRACE
    if (getenv("TILEFINCH_TRACE_STYLESHEETS") != NULL) {
        fprintf(stderr,
                "tilefinch-stylesheet-truncated received=%zu applied=%zu "
                "url=%s\n", received, applied, url == NULL ? "" : url);
    }
#endif
    StylesheetDocumentResources *resources = context->document_resources;
    StylesheetDocumentResource *entry =
        document_resource_find(resources, url);
    if (entry != NULL) {
        /* The page-lifetime ledger counts each response once, however many
           continuations reapply its retained prefix. */
        if (entry->truncated) return;
        entry->truncated = true;
    }
    size_t *count = resources != NULL ? &resources->truncated
                                      : &context->stats->truncated;
    size_t *received_total = resources != NULL
        ? &resources->truncated_received_bytes
        : &context->stats->truncated_received_bytes;
    size_t *applied_total = resources != NULL
        ? &resources->truncated_applied_bytes
        : &context->stats->truncated_applied_bytes;
    (*count)++;
    *received_total = stylesheet_estimate_add(*received_total, received);
    *applied_total = stylesheet_estimate_add(*applied_total, applied);
}

static void document_resource_record_loaded(
    ResourceContext *context, const char *url, char **owned_url,
    const StylesheetResponseProvenance *provenance,
    BrowserSharedBody *body, size_t length, bool rules_applied,
    bool cors_validated, TilefinchCredentialsMode credentials,
    const TilefinchResourceGrant *resource_grant)
{
    StylesheetDocumentResources *resources = context->document_resources;
    if (resources == NULL) return;
    StylesheetDocumentResource *entry = document_resource_get_or_add(
        context, url, owned_url);
    if (entry == NULL) return;
    if (entry->state != STYLESHEET_DOCUMENT_RESOURCE_LOADED
        && entry->attempts != SIZE_MAX) {
        entry->attempts++;
    }
    bool provenance_valid = provenance != NULL && provenance->known
        && provenance->response_url != NULL
        && provenance->response_url[0] != '\0'
        && strlen(provenance->response_url) < TILEFINCH_URL_SERIALIZED_LIMIT
        && stylesheet_referrer_policy_valid(provenance->referrer_policy);
    bool same_as_request = provenance_valid && url != NULL
        && strcmp(provenance->response_url, url) == 0;
    char *replacement = NULL;
    bool reuses_existing = false;
    if (provenance_valid && !same_as_request) {
        if (entry->response_url != NULL
            && strcmp(entry->response_url, provenance->response_url) == 0) {
            replacement = entry->response_url;
            reuses_existing = true;
        } else {
            size_t final_length = strlen(provenance->response_url);
            replacement = budget_malloc(
                context->budget, final_length + 1u);
            if (replacement != NULL) {
                memcpy(replacement, provenance->response_url,
                       final_length + 1u);
            } else {
                provenance_valid = false;
            }
        }
    }
    if (entry->body != body) {
        browser_shared_body_release(entry->body);
        entry->body = browser_shared_body_retain(body);
    }
    /* Body settlement and provenance retention are independent.  If a
       redirected URL cannot be copied, clear any older pair atomically; a
       later rebuild will refetch unconditionally instead of replaying the new
       body under stale URL/policy metadata. */
    if (!reuses_existing) {
        budget_free(context->budget, entry->response_url);
    }
    entry->response_url = provenance_valid ? replacement : NULL;
    entry->response_referrer_policy[0] = '\0';
    entry->response_provenance_known = provenance_valid;
    if (provenance_valid) {
        snprintf(entry->response_referrer_policy,
                 sizeof(entry->response_referrer_policy), "%s",
                 provenance->referrer_policy);
    } else {
        if (!reuses_existing) budget_free(context->budget, replacement);
    }
    entry->length = length;
    entry->rules_applied = entry->rules_applied || rules_applied;
    entry->cors_validated = cors_validated;
    entry->motion_source_readable = resource_grant != NULL
        && resource_grant->cors_validated;
    entry->credentials = credentials;
    /* Completion and retention are deliberately independent.  Under memory
       pressure the bounded body wrapper can fail after the transport has
       succeeded; the author-visible link must still settle.  A later style
       rebuild simply refetches when a non-empty body was not retained. */
    entry->state = STYLESHEET_DOCUMENT_RESOURCE_LOADED;
}

/* The first active link replaying a preloaded body takes over its charge. */
static void style_preload_charge_release(ResourceContext *context,
                                         StylesheetDocumentResource *entry)
{
    if (entry == NULL || !entry->preload_charged) return;
    ExternalStylesheetStats *stats = context->stats;
    if (stats->preload_attempted != 0) stats->preload_attempted--;
    stats->preload_bytes -= entry->preload_charge < stats->preload_bytes
        ? entry->preload_charge : stats->preload_bytes;
    entry->preload_charged = false;
    entry->preload_charge = 0;
}

static bool document_resource_suppresses(
    ResourceContext *context, StylesheetDocumentResource *entry)
{
    if (entry == NULL) return false;
    StylesheetDocumentResources *resources = context->document_resources;
    if (entry->state == STYLESHEET_DOCUMENT_RESOURCE_TERMINAL_FAILURE
        || (entry->state == STYLESHEET_DOCUMENT_RESOURCE_TRANSIENT_FAILURE
            && entry->attempts >= STYLESHEET_TRANSIENT_ATTEMPT_LIMIT)) {
        context->stats->failed++;
        if (resources != NULL) resources->retry_suppressed++;
        return true;
    }
    if (entry->state == STYLESHEET_DOCUMENT_RESOURCE_TRANSIENT_FAILURE
        && resources != NULL) {
        resources->transient_retries++;
    }
    return false;
}

static void pending_stylesheet_release(ResourceContext *context,
                                       PendingStylesheet *pending)
{
    if (pending == NULL) return;
    browser_shared_body_release(pending->retained_body);
    if (pending->owns_response_url) {
        budget_free(context->budget, pending->retained_response_url);
    }
    if (pending->owns_url) budget_free(context->budget, pending->url);
    memset(pending, 0, sizeof(*pending));
}

static bool pending_stylesheet_set_provenance(
    ResourceContext *context, PendingStylesheet *pending,
    const char *response_url, const char *response_referrer_policy)
{
    if (context == NULL || pending == NULL || pending->url == NULL) {
        return false;
    }
    if (pending->owns_response_url) {
        budget_free(context->budget, pending->retained_response_url);
    }
    pending->retained_response_url = NULL;
    pending->owns_response_url = false;
    pending->response_referrer_policy[0] = '\0';
    pending->response_provenance_known = false;
    TilefinchUrl parsed;
    if (response_url == NULL || response_url[0] == '\0'
        || strlen(response_url) >= TILEFINCH_URL_SERIALIZED_LIMIT
        || !tilefinch_url_parse(response_url, &parsed)
        || !stylesheet_referrer_policy_valid(response_referrer_policy)) {
        return false;
    }
    if (strcmp(response_url, pending->url) == 0) {
        pending->retained_response_url = pending->url;
    } else {
        size_t length = strlen(response_url);
        char *copy = budget_malloc(context->budget, length + 1u);
        if (copy == NULL) return false;
        memcpy(copy, response_url, length + 1u);
        pending->retained_response_url = copy;
        pending->owns_response_url = true;
    }
    snprintf(pending->response_referrer_policy,
             sizeof(pending->response_referrer_policy), "%s",
             response_referrer_policy);
    pending->response_provenance_known = true;
    return true;
}

static bool pending_stylesheet_retain_response(
    ResourceContext *context, PendingStylesheet *pending,
    BrowserSharedBody *body, size_t length, const char *response_url,
    const char *response_referrer_policy)
{
    if (context == NULL || pending == NULL
        || (length != 0
            && (body == NULL || body->length != length))) return false;
    BrowserSharedBody *retained = length == 0
        ? NULL : browser_shared_body_retain(body);
    if (length != 0 && retained == NULL) return false;
    if (!pending_stylesheet_set_provenance(
            context, pending, response_url, response_referrer_policy)) {
        browser_shared_body_release(retained);
        return false;
    }
    pending->retained_body = retained;
    pending->retained_empty = length == 0;
    return true;
}

static double resource_now_ms(void)
{
    return (double) tilefinch_platform_monotonic_time_us() * 0.001;
}


static void resource_finish_slice(ResourceContext *context)
{
    uint64_t finished = tilefinch_platform_monotonic_time_us();
    uint64_t elapsed_us = finished >= context->slice_started_us
        ? finished - context->slice_started_us : 0;
    if (elapsed_us > context->stats->max_slice_us) {
        context->stats->max_slice_us = elapsed_us;
        context->stats->max_slice_work_units = context->slice_work_units;
    }
    context->slice_started_us = finished;
    context->slice_work_units = 0;
}

static bool resource_cooperate(ResourceContext *context)
{
    resource_finish_slice(context);
    context->stats->cooperative_yields++;
    return tilefinch_platform_cooperate("resource",
                                     context->stats->work_units);
}

static bool resource_work(ResourceContext *context, size_t units,
                          bool force_yield)
{
    if (units > SIZE_MAX - context->stats->work_units) {
        context->stats->work_units = SIZE_MAX;
    } else {
        context->stats->work_units += units;
    }
    if (units > SIZE_MAX - context->slice_work_units) {
        context->slice_work_units = SIZE_MAX;
    } else {
        context->slice_work_units += units;
    }
    return (!force_yield && context->slice_work_units < 32)
           || resource_cooperate(context);
}

static bool cache_store_fetch(
    BrowserSession *session, const char *url, FetchResult *fetch,
    const StylesheetResponseProvenance *provenance,
    const TilefinchRequestContext *request_context,
    const TilefinchResourceGrant *resource_grant)
{
    char cache_control[256] = {0};
    char vary[128] = {0};
    (void) fetch_response_header_value(fetch, "cache-control",
                                       cache_control, sizeof(cache_control));
    (void) fetch_response_header_value(fetch, "vary", vary, sizeof(vary));
    (void) fetch_result_share_body(fetch);
    bool stored = false;
    if (fetch->shared_body != NULL && request_context != NULL
        && resource_grant != NULL) {
        stored = browser_session_cache_put_http_shared_resource(
            session, url, fetch->shared_body, fetch->etag,
            fetch->last_modified, fetch->content_type, cache_control, vary,
            tilefinch_platform_monotonic_time_ns(), request_context,
            resource_grant);
    }
    if (stored && provenance != NULL && provenance->known) {
        stored = browser_session_cache_set_resource_response_provenance(
            session, url, request_context, provenance->response_url,
            provenance->referrer_policy);
    }
    return stored;
}

static bool cache_revalidate_fetch(
    BrowserSession *session, const char *url, const FetchResult *fetch,
    const StylesheetResponseProvenance *provenance,
    const TilefinchRequestContext *request_context,
    const TilefinchResourceGrant *resource_grant)
{
    char cache_control[256] = {0};
    char vary[128] = {0};
    (void) fetch_response_header_value(fetch, "cache-control",
                                       cache_control, sizeof(cache_control));
    (void) fetch_response_header_value(fetch, "vary", vary, sizeof(vary));
    if (!browser_session_cache_revalidate_resource(
            session, url, cache_control, vary,
            tilefinch_platform_monotonic_time_ns(), request_context,
            resource_grant)) return false;
    return provenance != NULL && provenance->known
        && browser_session_cache_set_resource_response_provenance(
            session, url, request_context, provenance->response_url,
            provenance->referrer_policy);
}

static bool stylesheet_mime_matches(const char *content_type)
{
    if (content_type == NULL) return false;
    while (isspace((unsigned char) *content_type)) content_type++;
    static const char expected[] = "text/css";
    if (strncasecmp(content_type, expected, sizeof(expected) - 1u) != 0) {
        return false;
    }
    char after = content_type[sizeof(expected) - 1u];
    return after == '\0' || after == ';' || isspace((unsigned char) after);
}

static TilefinchRequestContext stylesheet_request_context(
    const ResourceContext *context, const char *url, bool cors,
    TilefinchCredentialsMode credentials)
{
    return (TilefinchRequestContext) {
        .target_url = url,
        .initiator_url = context == NULL ? NULL : context->document_url,
        .top_level_url = context == NULL ? NULL : context->document_url,
        .method = "GET",
        .mode = cors ? TILEFINCH_REQUEST_MODE_CORS
                     : TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = credentials,
        .destination = TILEFINCH_DESTINATION_STYLE
    };
}

static void resource_accept_response_cookies(
    ResourceContext *context, const TilefinchRequestContext *request_context,
    const char *fallback_url,
    const FetchResult *fetch)
{
    if (context == NULL || context->session == NULL || fetch == NULL
        || request_context == NULL
        || !tilefinch_request_sends_credentials(request_context)) return;
    TilefinchRequestContext request = *request_context;
    for (size_t i = 0; i < fetch->set_cookie_count; i++) {
        request.target_url = fetch_set_cookie_url(fetch, i, fallback_url);
        (void) browser_session_cookie_set_http_context(
            context->session, &request, fetch->set_cookies[i]);
    }
}

static bool stylesheet_stage_expired(ResourceContext *context)
{
    if (context->deadline_reached) return true;
    if (resource_now_ms() < context->deadline_ms) return false;
    context->deadline_reached = true;
    return true;
}

static long stylesheet_remaining_ms(ResourceContext *context)
{
    double remaining = context->deadline_ms - resource_now_ms();
    if (remaining <= 1.0) return 1;
    if (remaining > (double) context->timeout_ms) {
        return context->timeout_ms;
    }
    return (long) remaining;
}

static bool name_is(lxb_dom_node_t *node, const char *wanted)
{
    size_t length = 0;
    const char *name = document_element_name(node, &length);
    return name != NULL && strlen(wanted) == length
           && memcmp(name, wanted, length) == 0;
}

static bool token_contains(const char *text, size_t length, const char *wanted)
{
    size_t wanted_length = strlen(wanted);
    for (size_t at = 0; at < length;) {
        while (at < length && isspace((unsigned char) text[at])) at++;
        size_t end = at;
        while (end < length && !isspace((unsigned char) text[end])) end++;
        if (end - at == wanted_length) {
            bool equal = true;
            for (size_t i = 0; i < wanted_length; i++) {
                if (tolower((unsigned char) text[at + i])
                    != tolower((unsigned char) wanted[i])) equal = false;
            }
            if (equal) return true;
        }
        at = end;
    }
    return false;
}

static uint32_t hash_url(const char *url)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; url[i] != '\0'; i++) {
        hash = (hash ^ (unsigned char) url[i]) * 16777619u;
    }
    return hash;
}

static uint64_t stylesheet_url_hash64(const char *url)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; url != NULL && url[i] != '\0'; i++) {
        hash = (hash ^ (unsigned char) url[i])
               * UINT64_C(1099511628211);
    }
    return hash;
}

/* Some applications describe their bounded theme set with inert
   data-color-theme/data-href links while also emitting a few immediately
   active href links for no-script startup. Fetching every mutually-exclusive
   variant wastes both the stylesheet quota and several network turns before
   later component CSS can be reached. Recognize the declaration rather than
   filenames or origins: suppression is enabled only when the document root
   selects light/dark explicitly and the registry contains that exact theme. */
static void stylesheet_collect_alternate_themes(
    ResourceContext *context, lxb_dom_node_t *root)
{
    /* Theme registries live in <head>, but framework-generated preload and
       metadata nodes can put them beyond the first 512 DOM nodes. This pass
       runs only for roots that explicitly declare a selected theme. */
    enum { MAXIMUM_WORK = 2048, MAXIMUM_DEPTH = 64 };
    if (context == NULL || root == NULL) return;
    lxb_dom_node_t *html = root;
    size_t html_work = 0;
    while (html != NULL && !name_is(html, "html") && html_work++ < 16) {
        html = html->first_child != NULL ? html->first_child : html->next;
    }
    if (html == NULL) return;
    size_t mode_length = 0;
    const char *mode = document_attribute(html, "data-color-mode",
                                          &mode_length);
    const char *theme_attribute = NULL;
    if (mode != NULL && mode_length == 5
        && strncasecmp(mode, "light", 5) == 0) {
        theme_attribute = "data-light-theme";
    } else if (mode != NULL && mode_length == 4
               && strncasecmp(mode, "dark", 4) == 0) {
        theme_attribute = "data-dark-theme";
    } else {
        return;
    }
    size_t selected_length = 0;
    const char *selected = document_attribute(
        html, theme_attribute, &selected_length);
    if (selected == NULL || selected_length == 0 || selected_length > 64) {
        return;
    }

    uint64_t discovered_hashes[STYLESHEET_ALTERNATE_THEME_LIMIT];
    bool discovered_active[STYLESHEET_ALTERNATE_THEME_LIMIT];
    size_t discovered_count = 0;
    lxb_dom_node_t *node = root;
    size_t work = 0, depth = 0;
    bool selected_found = false;
    for (;;) {
        if (++work > MAXIMUM_WORK) {
            /* A completed viewport-first registry is more authoritative than
               an incomplete later walk of a script-expanded document. */
            return;
        }
        if (name_is(node, "link")
            && discovered_count < STYLESHEET_ALTERNATE_THEME_LIMIT) {
            size_t theme_length = 0, href_length = 0;
            const char *theme = document_attribute(
                node, "data-color-theme", &theme_length);
            const char *href = document_attribute(
                node, "data-href", &href_length);
            if (theme != NULL && theme_length != 0 && theme_length <= 64
                && href != NULL && href_length != 0 && href_length < 2048) {
                char reference[2048], resolved[4096];
                memcpy(reference, href, href_length);
                reference[href_length] = '\0';
                if (fetch_resolve_url(context->base_url, reference, resolved,
                                      sizeof(resolved))) {
                    size_t at = discovered_count++;
                    discovered_hashes[at] =
                        stylesheet_url_hash64(resolved);
                    bool active = theme_length == selected_length
                        && strncasecmp(theme, selected, selected_length) == 0;
                    discovered_active[at] = active;
                    selected_found |= active;
                }
            }
        }
        if (node->first_child != NULL && depth < MAXIMUM_DEPTH) {
            node = node->first_child;
            depth++;
            continue;
        }
        while (node != root && node->next == NULL) {
            node = node->parent;
            if (depth != 0) depth--;
        }
        if (node == root) break;
        node = node->next;
    }
    context->alternate_theme_count = selected_found ? discovered_count : 0;
    context->alternate_theme_selection_valid = selected_found;
    if (selected_found) {
        memcpy(context->alternate_theme_hashes, discovered_hashes,
               discovered_count * sizeof(discovered_hashes[0]));
        memcpy(context->alternate_theme_active, discovered_active,
               discovered_count * sizeof(discovered_active[0]));
    }
#ifndef TILEFINCH_NO_TRACE
    if (getenv("TILEFINCH_TRACE_STYLESHEETS") != NULL) {
        fprintf(stderr,
                "tilefinch-stylesheet-themes mode=%.*s selected=%.*s "
                "entries=%zu active=%s\n",
                (int) mode_length, mode, (int) selected_length, selected,
                discovered_count,
                selected_found ? "yes" : "no");
    }
#endif
    if (context->document_resources != NULL) {
        StylesheetDocumentResources *resources = context->document_resources;
        resources->alternate_theme_count = context->alternate_theme_count;
        resources->alternate_theme_selection_valid = selected_found;
        memcpy(resources->alternate_theme_hashes,
               context->alternate_theme_hashes,
               context->alternate_theme_count
                   * sizeof(context->alternate_theme_hashes[0]));
        memcpy(resources->alternate_theme_active,
               context->alternate_theme_active,
               context->alternate_theme_count
                   * sizeof(context->alternate_theme_active[0]));
    }
}

static void stylesheet_restore_alternate_themes(ResourceContext *context)
{
    if (context == NULL || context->document_resources == NULL) return;
    const StylesheetDocumentResources *resources =
        context->document_resources;
    size_t count = resources->alternate_theme_count;
#ifndef TILEFINCH_NO_TRACE
    if (getenv("TILEFINCH_TRACE_STYLESHEETS") != NULL) {
        fprintf(stderr,
                "tilefinch-stylesheet-theme-restore valid=%s entries=%zu\n",
                resources->alternate_theme_selection_valid ? "yes" : "no",
                count);
    }
#endif
    if (!resources->alternate_theme_selection_valid
        || count > STYLESHEET_ALTERNATE_THEME_LIMIT) return;
    memcpy(context->alternate_theme_hashes,
           resources->alternate_theme_hashes,
           count * sizeof(context->alternate_theme_hashes[0]));
    memcpy(context->alternate_theme_active,
           resources->alternate_theme_active,
           count * sizeof(context->alternate_theme_active[0]));
    context->alternate_theme_count = count;
    context->alternate_theme_selection_valid = true;
}

static bool stylesheet_is_inactive_alternate_theme(
    const ResourceContext *context, const char *url)
{
    if (context == NULL || url == NULL
        || !context->alternate_theme_selection_valid) return false;
    uint64_t hash = stylesheet_url_hash64(url);
    for (size_t i = 0; i < context->alternate_theme_count; i++) {
        if (context->alternate_theme_hashes[i] == hash) {
            if (!context->alternate_theme_active[i]
#ifndef TILEFINCH_NO_TRACE
                && getenv("TILEFINCH_TRACE_STYLESHEETS") != NULL
#endif
                ) {
#ifndef TILEFINCH_NO_TRACE
                fprintf(stderr, "tilefinch-stylesheet-theme-skip %s\n",
                        url);
#endif
            }
            return !context->alternate_theme_active[i];
        }
    }
    return false;
}

typedef enum {
    STYLESHEET_REFERENCE_NEW = 0,
    STYLESHEET_REFERENCE_DUPLICATE,
    STYLESHEET_REFERENCE_PROMOTED
} StylesheetReferenceState;

/* A nonmatching-media link participates in loading but not in the cascade.
   Remember that distinction so a later matching reference to the same URL
   can promote the already queued/retained response instead of being hidden
   by URL deduplication. */
static StylesheetReferenceState stylesheet_reference_note(
    ResourceContext *context, uint32_t hash, bool apply_rules)
{
    for (size_t i = 0; i < context->hash_count; i++) {
        if (context->hashes[i] != hash) continue;
        if (context->hash_applied[i] || !apply_rules) {
            return STYLESHEET_REFERENCE_DUPLICATE;
        }
        context->hash_applied[i] = true;
        return STYLESHEET_REFERENCE_PROMOTED;
    }
    if (context->hash_count < MAX_TRACKED_STYLESHEETS) {
        context->hashes[context->hash_count] = hash;
        context->hash_applied[context->hash_count] = apply_rules;
        context->hash_count++;
    }
    return STYLESHEET_REFERENCE_NEW;
}

static void trim_span(const char **text, size_t *length)
{
    while (*length != 0 && isspace((unsigned char) **text)) {
        (*text)++; (*length)--;
    }
    while (*length != 0
           && isspace((unsigned char) (*text)[*length - 1])) (*length)--;
}

static bool ascii_starts(const char *text, size_t length, const char *wanted)
{
    size_t wanted_length = strlen(wanted);
    if (length < wanted_length) return false;
    for (size_t i = 0; i < wanted_length; i++) {
        if (tolower((unsigned char) text[i])
            != tolower((unsigned char) wanted[i])) return false;
    }
    return true;
}

static size_t matching_parenthesis(const char *text, size_t length,
                                   size_t open)
{
    int depth = 1;
    char quote = 0;
    for (size_t i = open + 1; i < length; i++) {
        if (quote != 0) {
            if (text[i] == '\\' && i + 1 < length) i++;
            else if (text[i] == quote) quote = 0;
            continue;
        }
        if (text[i] == '\'' || text[i] == '"') quote = text[i];
        else if (text[i] == '(') depth++;
        else if (text[i] == ')' && --depth == 0) return i;
    }
    return length;
}

static bool load_stylesheet_url(ResourceContext *context, const char *url,
                                unsigned depth,
                                const StylesheetResponseProvenance *initiator);
static bool load_stylesheet_imports(ResourceContext *context,
                                    const char *css, size_t length,
                                    const StylesheetResponseProvenance *sheet,
                                    unsigned depth);

static bool apply_stylesheet_data(ResourceContext *context,
                                  const char *resolved,
                                  const StylesheetResponseProvenance *provenance,
                                  const unsigned char *css_data,
                                  size_t css_length, unsigned depth,
                                  FetchResult *fetched,
                                  bool use_cached,
                                  BrowserSharedBody *cached_body,
                                  const TilefinchRequestContext *request_context,
                                  const TilefinchResourceGrant *resource_grant,
                                  bool retained_motion_source_readable)
{
    if (provenance == NULL || !provenance->known
        || provenance->response_url == NULL
        || (css_data == NULL && css_length != 0)
        || !stylesheet_referrer_policy_valid(
               provenance->referrer_policy)) return false;
#ifndef TILEFINCH_NO_TRACE
    if (getenv("TILEFINCH_TRACE_STYLESHEETS") != NULL) {
        fprintf(stderr,
                "tilefinch-stylesheet-apply bytes=%zu/%zu source=%zu url=%s\n",
                context->stats->bytes, context->maximum_total_bytes,
                css_length, resolved);
    }
#endif
    if (css_length > context->maximum_total_bytes - context->stats->bytes) {
        context->stats->skipped_limit++;
        return true;
    }
    size_t parse_cost = stylesheet_source_cost_estimate(
        (const char *) css_data, css_length);
    if (css_length > context->maximum_single_bytes
        && budget_pressure_required(context->budget, parse_cost,
                                    STYLESHEET_LAYOUT_RESERVE)
        && !budget_make_room(context->budget, stylesheet_estimate_add(
               parse_cost, STYLESHEET_LAYOUT_RESERVE))) {
        /* Memory admitted this response past the per-file cap but no longer
           holds its parse: keep what the cap alone would have kept, its
           complete rules up to the cap, rather than refusing the sheet. */
        size_t kept = stylesheet_complete_rules_prefix(
            (const char *) css_data, context->maximum_single_bytes);
        if (kept == 0) {
            context->stats->skipped_pressure++;
            budget_record_pressure(context->budget,
                                   BUDGET_PRESSURE_STYLESHEET, parse_cost, 0);
            return true;
        }
        stylesheet_note_truncated(context, resolved, css_length, kept);
        css_length = kept;
        parse_cost = stylesheet_source_cost_estimate(
            (const char *) css_data, css_length);
    }
    if (!stylesheet_parse_admitted(context, css_length, parse_cost)) {
        return true;
    }
    /* Only retained author CSS consumes the aggregate source quota. A large
       sheet refused by the independent memory-pressure gate must not crowd
       out a later, small sheet that still fits and may carry critical mobile
       or accessibility rules. */
    context->stats->bytes += css_length;
    unsigned char *stable_cached = NULL;
    BrowserSharedBody *stable_body = use_cached && cached_body != NULL
        ? browser_shared_body_retain(cached_body) : NULL;
    if (use_cached && stable_body == NULL && css_length != 0) {
        stable_cached = budget_malloc(context->budget, css_length);
        if (stable_cached == NULL) return false;
        memcpy(stable_cached, css_data, css_length);
        css_data = stable_cached;
    }
    const char *selected_base = provenance->response_url;
    size_t source_base_length = strlen(selected_base);
    char *source_base = source_base_length >= TILEFINCH_URL_SERIALIZED_LIMIT
        ? NULL : budget_malloc(context->budget, source_base_length + 1u);
    if (source_base == NULL) {
        budget_free(context->budget, stable_cached);
        browser_shared_body_release(stable_body);
        return false;
    }
    memcpy(source_base, selected_base, source_base_length + 1u);
    StylesheetResponseProvenance stable_provenance = {
        .response_url = source_base,
        .known = true
    };
    snprintf(stable_provenance.referrer_policy,
             sizeof(stable_provenance.referrer_policy), "%s",
             provenance->referrer_policy);
    bool parsed = load_stylesheet_imports(
        context, (const char *) css_data, css_length, &stable_provenance,
        depth);
    size_t rules_before = context->sheet->count;
    size_t variables_before = context->sheet->variable_count;
    size_t previous_rule_limit = context->sheet->source_rule_limit_end;
    size_t previous_head_limit =
        context->sheet->source_rule_head_limit_end;
    size_t previous_secondary_limit =
        context->sheet->source_rule_secondary_limit_end;
    size_t previous_relevant_limit =
        context->sheet->source_rule_relevant_limit_end;
    const uint32_t *previous_priority_token_bloom =
        context->sheet->source_rule_priority_token_bloom;
    size_t previous_priority_token_bloom_words =
        context->sheet->source_rule_priority_token_bloom_words;
    const uint32_t *previous_token_bloom =
        context->sheet->source_rule_token_bloom;
    size_t previous_token_bloom_words =
        context->sheet->source_rule_token_bloom_words;
    const char *previous_tail_begin =
        context->sheet->source_rule_tail_begin;
    if (parsed && css_length >= STYLESHEET_LARGE_SOURCE_BYTES) {
        if (context->document_resources != NULL
            && !context->document_resources->selector_census_complete
            && !context->document_resources->final_resample_completed) {
            context->document_resources->final_resample_required = true;
        }
        context->sheet->source_rule_limit_end =
            rules_before > SIZE_MAX - STYLESHEET_LARGE_SOURCE_RULE_LIMIT
                ? SIZE_MAX
                : rules_before + STYLESHEET_LARGE_SOURCE_RULE_LIMIT;
        context->sheet->source_rule_head_limit_end =
            rules_before > SIZE_MAX - STYLESHEET_LARGE_SOURCE_HEAD_RULES
                ? SIZE_MAX
                : rules_before + STYLESHEET_LARGE_SOURCE_HEAD_RULES;
        /* DOM-relevant rules past the fixed allowance are admitted by
           memory: each needs about a rule block's estimate beside the
           sheet's parse working set and the layout reserve. Both the
           relevant allowance and the sheet's rule ceiling grow by them. */
        size_t extra_relevant = 0;
        size_t spare = budget_remaining(context->budget);
        size_t needed = stylesheet_estimate_add(
            parse_cost, STYLESHEET_LAYOUT_RESERVE);
        if (spare > needed) {
            extra_relevant = (spare - needed)
                             / STYLESHEET_ESTIMATE_BLOCK_BYTES;
            if (extra_relevant > STYLESHEET_LARGE_SOURCE_EXTRA_RELEVANT_RULES)
                extra_relevant = STYLESHEET_LARGE_SOURCE_EXTRA_RELEVANT_RULES;
        }
        context->sheet->source_rule_limit_end = stylesheet_estimate_add(
            context->sheet->source_rule_limit_end, extra_relevant);
        context->sheet->source_rule_relevant_limit_end =
            stylesheet_estimate_add(
                stylesheet_estimate_add(
                    context->sheet->source_rule_head_limit_end,
                    STYLESHEET_LARGE_SOURCE_RELEVANT_RULES),
                extra_relevant);
        /* A whole-document census that fit its node limit is as exact as
           the first-screen one, so rules it admits share the full relevant
           allowance. Only a saturated census (every token "present") keeps
           the small secondary allowance, which then degrades to plain
           source order. Sharing the allowance does not raise the sheet's
           rule ceiling (source_rule_limit_end); it stops DOM-relevant rules
           in the middle of a large sheet (icon sizes, display:none on
           mobile) from being dropped while the allowance sits unused. */
        context->sheet->source_rule_secondary_limit_end =
            context->selector_token_census_complete
                ? context->sheet->source_rule_relevant_limit_end
                : context->sheet->source_rule_head_limit_end
                        > SIZE_MAX - STYLESHEET_LARGE_SOURCE_SECONDARY_RULES
                    ? SIZE_MAX
                    : context->sheet->source_rule_head_limit_end
                        + STYLESHEET_LARGE_SOURCE_SECONDARY_RULES;
        context->sheet->source_rule_priority_token_bloom =
            context->priority_token_bloom;
        context->sheet->source_rule_priority_token_bloom_words =
            STYLESHEET_PRIORITY_TOKEN_BLOOM_WORDS;
        context->sheet->source_rule_token_bloom =
            context->selector_token_bloom;
        context->sheet->source_rule_token_bloom_words =
            STYLESHEET_SELECTOR_TOKEN_BLOOM_WORDS;
        context->sheet->source_rule_tail_begin =
            (const char *) css_data
            + (css_length / 100u) * STYLESHEET_LARGE_SOURCE_TAIL_PERCENT;
    }
    bool fragment_eligible = parsed && css_length != 0
        && context->session != NULL
        && tilefinch_url_same_origin(
               context->document_url, stable_provenance.response_url);
    if (parsed && css_length >= STYLESHEET_LARGE_SOURCE_BYTES
        && context->document_resources != NULL) {
        context->document_resources->sampled_large_source = true;
        for (size_t i = 0; i < STYLESHEET_SELECTOR_TOKEN_BLOOM_WORDS; i++) {
            context->document_resources->sampled_selector_tokens[i] |=
                context->selector_token_bloom[i];
        }
    }
#ifndef TILEFINCH_NO_TRACE
    fragment_eligible = fragment_eligible
        && getenv("TILEFINCH_DISABLE_STYLESHEET_FRAGMENT_CACHE") == NULL;
#endif
    /* Large sources deliberately retain bounded source-order bookends plus
       a DOM-relevant selector allowance. Their structural IR would otherwise
       replay every captured rule and silently bypass that degradation policy. */
    bool ir_eligible = fragment_eligible
        && css_length < STYLESHEET_LARGE_SOURCE_BYTES;
#ifndef TILEFINCH_NO_TRACE
    ir_eligible = ir_eligible
        && getenv("TILEFINCH_DISABLE_STYLESHEET_PARSED_IR") == NULL;
#endif
    BrowserSharedBody *compiled_fragment = NULL;
    BrowserSharedBody *parsed_ir = NULL;
    bool artifact_backing_available = false;
    if (fragment_eligible) {
        artifact_backing_available = browser_session_stylesheet_artifacts_acquire(
            context->session, resolved, request_context,
            css_data, css_length, &compiled_fragment,
            ir_eligible ? &parsed_ir : NULL);
    }
    unsigned char *new_ir = NULL;
    size_t new_ir_length = 0;
    size_t ir_operations_reused = 0;
    StyleParsedIrApplyResult ir_result = STYLE_PARSED_IR_REJECTED;
    /* The motion bootstrap runs in the page realm and invokes author
       methods with parsed selectors/keyframes. A no-CORS external response
       may style the page but must never enter that JS-visible side channel.
       Fresh imports have their own grant; retained responses keep the
       grant-derived readability bit. Missing provenance is non-readable. */
    bool previous_motion_source_blocked =
        context->sheet->motion_css_source_blocked;
    context->sheet->motion_css_source_blocked =
        !retained_motion_source_readable
        && (resource_grant == NULL || !resource_grant->cors_validated);
    if (parsed && parsed_ir != NULL) {
        ir_result = stylesheet_add_parsed_ir_from_context(
            context->sheet, parsed_ir->data, parsed_ir->length,
            source_base, stable_provenance.referrer_policy,
            &ir_operations_reused);
        if (ir_result == STYLE_PARSED_IR_APPLIED) {
            context->stats->parsed_ir_hits++;
            context->stats->parsed_ir_operations_reused +=
                ir_operations_reused;
            context->stats->parsed_ir_bytes += parsed_ir->length;
#ifndef TILEFINCH_NO_TRACE
            if (getenv("TILEFINCH_TRACE_STYLESHEETS") != NULL) {
                fprintf(stderr,
                        "tilefinch-stylesheet-ir action=hit "
                        "operations=%zu bytes=%zu url=%s\n",
                        ir_operations_reused, parsed_ir->length, resolved);
            }
#endif
        } else if (ir_result == STYLE_PARSED_IR_FAILED) {
            parsed = false;
        }
    }
    if (parsed && ir_result == STYLE_PARSED_IR_REJECTED) {
        if (ir_eligible) context->stats->parsed_ir_misses++;
        /* Retained document CSS can outlive its HTTP-cache entry. Unlike
           compiled selector fragments, IR does no useful first-parse work;
           do not allocate it when put_take cannot possibly find its owner.
           Fresh responses are stored below, so keep capturing those. */
        bool capture_ir = ir_eligible && (artifact_backing_available
            || (!use_cached && fetched != NULL));
        parsed = capture_ir
            ? stylesheet_add_css_from_context_capture_ir(
                  context->sheet, (const char *) css_data, css_length,
                  source_base, stable_provenance.referrer_policy,
                  &new_ir, &new_ir_length)
            : stylesheet_add_css_from_context(
                  context->sheet, (const char *) css_data, css_length,
                  source_base, stable_provenance.referrer_policy);
    }
    context->sheet->motion_css_source_blocked =
        previous_motion_source_blocked;
    fragment_eligible = fragment_eligible && parsed;
    unsigned char *new_fragment = NULL;
    size_t new_fragment_length = 0;
    size_t fragment_rules_reused = 0;
    if (compiled_fragment != NULL) {
        fragment_rules_reused = stylesheet_compiled_fragment_apply(
            context->sheet, rules_before, context->sheet->count,
            compiled_fragment->data, compiled_fragment->length);
        if (fragment_rules_reused != 0) {
            context->stats->compiled_fragment_hits++;
            context->stats->compiled_fragment_rules_reused +=
                fragment_rules_reused;
            context->stats->compiled_fragment_bytes +=
                compiled_fragment->length;
#ifndef TILEFINCH_NO_TRACE
            if (getenv("TILEFINCH_TRACE_STYLESHEETS") != NULL) {
                fprintf(stderr,
                        "tilefinch-stylesheet-fragment action=hit "
                        "rules=%zu bytes=%zu url=%s\n",
                        fragment_rules_reused, compiled_fragment->length,
                        resolved);
            }
#endif
        } else {
            context->stats->compiled_fragment_misses++;
        }
    } else if (fragment_eligible) {
        context->stats->compiled_fragment_misses++;
    }
    tilefinch_platform_trace_step("css-fragment-build");
    bool fragment_built = fragment_eligible && fragment_rules_reused == 0
        && stylesheet_compiled_fragment_build(
            context->sheet, rules_before, context->sheet->count,
            &new_fragment, &new_fragment_length);
    tilefinch_platform_trace_step("css-fragment-seed");
    if (fragment_built) {
        /* Seed this first parse too. The final whole-page selector program
           then consumes the same compiled instructions that are retained
           for a later same-origin navigation; first-load work is not
           duplicated merely to populate the cache. */
        (void) stylesheet_compiled_fragment_apply(
            context->sheet, rules_before, context->sheet->count,
            new_fragment, new_fragment_length);
    }
    context->sheet->source_rule_limit_end = previous_rule_limit;
    context->sheet->source_rule_head_limit_end = previous_head_limit;
    context->sheet->source_rule_secondary_limit_end =
        previous_secondary_limit;
    context->sheet->source_rule_relevant_limit_end = previous_relevant_limit;
    context->sheet->source_rule_priority_token_bloom =
        previous_priority_token_bloom;
    context->sheet->source_rule_priority_token_bloom_words =
        previous_priority_token_bloom_words;
    context->sheet->source_rule_token_bloom = previous_token_bloom;
    context->sheet->source_rule_token_bloom_words =
        previous_token_bloom_words;
    context->sheet->source_rule_tail_begin = previous_tail_begin;
    if (parsed) {
        context->stats->loaded++;
        context->stats->rules_added += context->sheet->count - rules_before;
        context->stats->variables_added += context->sheet->variable_count
                                           - variables_before;
        /* A response cut at its byte cap is not the resource; only this
           document's ledger keeps its complete-rule prefix. */
        if (!use_cached && context->session != NULL && fetched != NULL
            && !fetched->response_limit_exceeded) {
            (void) cache_store_fetch(
                context->session, resolved, fetched, &stable_provenance,
                request_context, resource_grant);
            /* Sharing trims the fetch buffer before transferring ownership,
               so it may move the bytes.  The artifact cache below must hash
               the post-transfer address, never the append buffer it replaced. */
            css_data = (const unsigned char *) fetched->data;
        }
        tilefinch_platform_trace_step("css-artifact-store");
        bool ir_stored = new_ir != NULL
            && browser_session_stylesheet_ir_put_take(
                   context->session, resolved, request_context,
                   css_data, css_length, new_ir, new_ir_length);
        if (ir_stored) {
            new_ir = NULL;
            context->stats->parsed_ir_stores++;
            context->stats->parsed_ir_bytes += new_ir_length;
#ifndef TILEFINCH_NO_TRACE
            if (getenv("TILEFINCH_TRACE_STYLESHEETS") != NULL) {
                fprintf(stderr,
                        "tilefinch-stylesheet-ir action=store "
                        "bytes=%zu url=%s\n", new_ir_length, resolved);
            }
#endif
        }
#ifndef TILEFINCH_NO_TRACE
        else if (new_ir != NULL
                 && getenv("TILEFINCH_TRACE_STYLESHEETS") != NULL) {
            fprintf(stderr,
                    "tilefinch-stylesheet-ir action=store-refused "
                    "bytes=%zu url=%s\n", new_ir_length, resolved);
        }
#endif
        bool fragment_stored = new_fragment != NULL
            && browser_session_stylesheet_fragment_put_take(
                   context->session, resolved, request_context,
                   css_data, css_length,
                   new_fragment, new_fragment_length);
        if (fragment_stored) {
            new_fragment = NULL;
            context->stats->compiled_fragment_stores++;
            context->stats->compiled_fragment_bytes += new_fragment_length;
#ifndef TILEFINCH_NO_TRACE
            if (getenv("TILEFINCH_TRACE_STYLESHEETS") != NULL) {
                fprintf(stderr,
                        "tilefinch-stylesheet-fragment action=store "
                        "rules=%zu bytes=%zu url=%s\n",
                        context->sheet->count - rules_before,
                        new_fragment_length, resolved);
            }
#endif
        }
#ifndef TILEFINCH_NO_TRACE
        else if (new_fragment != NULL
                 && getenv("TILEFINCH_TRACE_STYLESHEETS") != NULL) {
            fprintf(stderr,
                    "tilefinch-stylesheet-fragment action=store-refused "
                    "rules=%zu bytes=%zu url=%s\n",
                    context->sheet->count - rules_before,
                    new_fragment_length, resolved);
        }
#endif
    }
    browser_shared_body_release(compiled_fragment);
    browser_shared_body_release(parsed_ir);
    budget_free(context->budget, new_fragment);
    budget_free(context->budget, new_ir);
    budget_free(context->budget, source_base);
    budget_free(context->budget, stable_cached);
    browser_shared_body_release(stable_body);
    return parsed;
}

static bool settle_stylesheet_data(ResourceContext *context,
                                   const char *resolved,
                                   const StylesheetResponseProvenance *provenance,
                                   const unsigned char *css_data,
                                   size_t css_length, unsigned depth,
                                   FetchResult *fetched,
                                   bool use_cached,
                                   BrowserSharedBody *cached_body,
                                   bool apply_rules, bool preload,
                                   const TilefinchRequestContext *request_context,
                                   const TilefinchResourceGrant *resource_grant)
{
    if (apply_rules) {
        return apply_stylesheet_data(
            context, resolved, provenance, css_data, css_length,
            depth, fetched,
            use_cached, cached_body, request_context, resource_grant, false);
    }
    size_t *charged = preload ? &context->stats->preload_bytes
                              : &context->stats->bytes;
    size_t limit = preload ? style_preload_byte_limit(context)
                           : context->maximum_total_bytes;
    if (css_length > limit - *charged) {
        context->stats->skipped_limit++;
        return true;
    }
    *charged += css_length;
    context->stats->loaded++;
    /* Media affects whether CSS participates in the cascade, not whether the
       response is fetched, cached, or completes its link element. */
    if (!use_cached && context->session != NULL && fetched != NULL) {
        (void) cache_store_fetch(
            context->session, resolved, fetched, provenance,
            request_context, resource_grant);
    }
    return true;
}

static bool load_import_statement(ResourceContext *context,
                                  const char *statement, size_t length,
                                  const StylesheetResponseProvenance *sheet,
                                  unsigned depth)
{
    if (sheet == NULL || !sheet->known || sheet->response_url == NULL) {
        return false;
    }
    trim_span(&statement, &length);
    if (length == 0) return true;
    const char *reference = NULL;
    size_t reference_length = 0;
    size_t consumed = 0;
    if (ascii_starts(statement, length, "url(")) {
        size_t at = 4;
        while (at < length && isspace((unsigned char) statement[at])) at++;
        char quote = at < length
                     && (statement[at] == '\'' || statement[at] == '"')
                     ? statement[at++] : 0;
        size_t end = at;
        while (end < length
               && (quote != 0 ? statement[end] != quote
                              : statement[end] != ')')) end++;
        reference = statement + at;
        reference_length = end - at;
        if (quote != 0 && end < length) end++;
        while (end < length && statement[end] != ')') end++;
        consumed = end < length ? end + 1 : end;
    }
    else if (statement[0] == '\'' || statement[0] == '"') {
        char quote = statement[0];
        size_t end = 1;
        while (end < length && statement[end] != quote) {
            if (statement[end] == '\\' && end + 1 < length) end++;
            end++;
        }
        reference = statement + 1;
        reference_length = end - 1;
        consumed = end < length ? end + 1 : end;
    }
    trim_span(&reference, &reference_length);
    if (reference == NULL || reference_length == 0
        || reference_length >= 2048) {
        context->stats->failed++;
        return true;
    }
    const char *condition = statement + consumed;
    size_t condition_length = length - consumed;
    trim_span(&condition, &condition_length);
    for (;;) {
        if (ascii_starts(condition, condition_length, "layer")) {
            size_t used = 5;
            if (used < condition_length && condition[used] == '(') {
                size_t close = matching_parenthesis(condition,
                                                    condition_length, used);
                if (close == condition_length) {
                    context->stats->imports_skipped_conditions++;
                    return true;
                }
                used = close + 1;
            }
            condition += used; condition_length -= used;
            trim_span(&condition, &condition_length);
            continue;
        }
        if (ascii_starts(condition, condition_length, "supports(")) {
            size_t open = 8;
            size_t close = matching_parenthesis(condition, condition_length,
                                                open);
            if (close == condition_length
                || !stylesheet_supports_matches(
                       context->sheet, condition + open + 1,
                       close - open - 1)) {
                context->stats->imports_skipped_conditions++;
                return true;
            }
            condition += close + 1;
            condition_length -= close + 1;
            trim_span(&condition, &condition_length);
            continue;
        }
        break;
    }
    if (condition_length != 0
        && !stylesheet_media_matches(context->sheet, condition,
                                     condition_length)) {
        context->stats->imports_skipped_conditions++;
        return true;
    }
    if (depth >= 4) {
        context->stats->imports_skipped_depth++;
        return true;
    }
    char reference_copy[2048];
    memcpy(reference_copy, reference, reference_length);
    reference_copy[reference_length] = '\0';
    char resolved[4096];
    if (!fetch_resolve_url(sheet->response_url, reference_copy, resolved,
                           sizeof(resolved))) {
        context->stats->failed++;
        return true;
    }
    size_t loaded_before = context->stats->loaded;
    if (!load_stylesheet_url(context, resolved, depth + 1, sheet)) {
        return false;
    }
    if (context->stats->loaded > loaded_before) {
        context->stats->imports_loaded++;
    }
    return true;
}

static bool load_stylesheet_imports(ResourceContext *context,
                                    const char *css, size_t length,
                                    const StylesheetResponseProvenance *sheet,
                                    unsigned depth)
{
    int braces = 0;
    char quote = 0;
    for (size_t at = 0; at < length; at++) {
        if (quote != 0) {
            if (css[at] == '\\' && at + 1 < length) at++;
            else if (css[at] == quote) quote = 0;
            continue;
        }
        if (css[at] == '\'' || css[at] == '"') { quote = css[at]; continue; }
        if (css[at] == '/' && at + 1 < length && css[at + 1] == '*') {
            at += 2;
            while (at + 1 < length
                   && !(css[at] == '*' && css[at + 1] == '/')) at++;
            if (at + 1 < length) at++;
            continue;
        }
        if (css[at] == '{') { braces++; continue; }
        if (css[at] == '}' && braces > 0) { braces--; continue; }
        if (braces != 0 || at + 7 > length
            || !ascii_starts(css + at, length - at, "@import")) continue;
        unsigned char boundary = at + 7 < length
                                 ? (unsigned char) css[at + 7] : 0;
        if (isalnum(boundary) || boundary == '-' || boundary == '_') continue;
        size_t end = at + 7;
        int parentheses = 0;
        char statement_quote = 0;
        while (end < length) {
            char character = css[end];
            if (statement_quote != 0) {
                if (character == '\\' && end + 1 < length) end++;
                else if (character == statement_quote) statement_quote = 0;
            }
            else if (character == '\'' || character == '"') {
                statement_quote = character;
            }
            else if (character == '(') parentheses++;
            else if (character == ')' && parentheses > 0) parentheses--;
            else if (character == ';' && parentheses == 0) break;
            end++;
        }
        context->stats->discovered++;
        context->stats->imports_discovered++;
        if (!load_import_statement(context, css + at + 7,
                                   end - at - 7, sheet, depth)) {
            return false;
        }
        at = end;
    }
    return true;
}

static bool load_stylesheet_url(
    ResourceContext *context, const char *resolved, unsigned depth,
    const StylesheetResponseProvenance *initiator)
{
    if (initiator == NULL || !initiator->known
        || initiator->response_url == NULL
        || !stylesheet_referrer_policy_valid(
               initiator->referrer_policy)) return false;
    if (!tilefinch_csp_allows_request(
            context->content_security_policy,
            TILEFINCH_DESTINATION_STYLE, resolved)) {
        context->stats->failed++;
        return true;
    }
    /* This entry point also serves retained/rebuilt inputs and @imports,
       which do not necessarily pass through queue_stylesheet_link(). */
    if (stylesheet_is_inactive_alternate_theme(context, resolved)) {
        context->stats->skipped_alternate_theme++;
        return true;
    }
    if (stylesheet_stage_expired(context)) {
        context->stats->deadline_cancelled++;
        return true;
    }
    uint32_t hash = hash_url(resolved);
    if (stylesheet_reference_note(context, hash, true)
            == STYLESHEET_REFERENCE_DUPLICATE) {
        context->stats->duplicate++;
        return true;
    }
    StylesheetDocumentResource *document_resource = document_resource_find(
        context->document_resources, resolved);
    TilefinchRequestContext request_context = stylesheet_request_context(
        context, resolved, false, TILEFINCH_CREDENTIALS_INCLUDE);
    if (document_resource_suppresses(context, document_resource)) {
        return true;
    }
    if (document_resource != NULL
        && document_resource->state
               == STYLESHEET_DOCUMENT_RESOURCE_LOADED
        && document_resource->response_provenance_known
        && (document_resource->body != NULL
            || document_resource->length == 0)) {
        StylesheetResponseProvenance retained = {
            .response_url = document_resource->response_url != NULL
                ? document_resource->response_url : document_resource->url,
            .known = true
        };
        snprintf(retained.referrer_policy, sizeof(retained.referrer_policy),
                 "%s", document_resource->response_referrer_policy);
        context->stats->attempted++;
        context->stats->cache_hits++;
        context->document_resources->retained_body_hits++;
        return apply_stylesheet_data(
            context, resolved, &retained,
            document_resource->body == NULL
                ? NULL : document_resource->body->data,
            document_resource->length, depth, NULL, true,
            document_resource->body, &request_context, NULL,
            document_resource->motion_source_readable);
    }
    if (context->stats->attempted >= context->maximum_count
        || context->stats->bytes >= context->maximum_total_bytes) {
        context->stats->skipped_limit++;
        if (document_resource == NULL
            || document_resource->state
                   != STYLESHEET_DOCUMENT_RESOURCE_LOADED) {
            document_resource_record_failure(
                context, resolved, NULL, NULL, true);
        }
        return true;
    }
    size_t remaining = context->maximum_total_bytes - context->stats->bytes;
    size_t maximum = stylesheet_response_bound(context, remaining);
    if (maximum == 0) {
        context->stats->skipped_limit++;
        document_resource_record_failure(
            context, resolved, NULL, NULL, true);
        return true;
    }
    context->stats->attempted++;
    const BrowserCacheEntry *cached = NULL;
    BrowserCacheStatus cache_status = context->session == NULL
        || content_blocker_would_block(
               context->session->content_blocker, resolved,
               context->document_url, "style", "no-cors")
        ? BROWSER_CACHE_MISS : browser_session_cache_match_resource(
            context->session, resolved, &request_context,
            tilefinch_platform_monotonic_time_ns(), &cached);
    PendingStylesheet cached_snapshot = {
        .url = (char *) resolved
    };
    size_t cached_length = cached == NULL ? 0 : cached->length;
    bool cached_provenance_known = cached != NULL
        && cached->response_url_known
        && cached->response_referrer_policy_known
        && cached->length <= maximum
        && (cached->length == 0 || cached->body != NULL)
        && pending_stylesheet_retain_response(
            context, &cached_snapshot, cached->body, cached->length,
            browser_cache_entry_response_url(cached),
            cached->response_referrer_policy);
    if (cached_provenance_known) {
        cached_snapshot.resource_grant_valid = cached->resource_grant_valid;
        cached_snapshot.resource_grant = cached->resource_grant;
        snprintf(cached_snapshot.content_type,
                 sizeof(cached_snapshot.content_type), "%s",
                 cached->content_type);
    }
    if (!cached_provenance_known) {
        cached = NULL;
        cache_status = BROWSER_CACHE_MISS;
    }
    bool use_cached = cache_status == BROWSER_CACHE_FRESH && cached != NULL;
    FetchResult *fetched = NULL;
    const unsigned char *css_data = NULL;
    size_t css_length = 0;
    bool revalidated = false;
    StylesheetResponseProvenance response_provenance = {0};
    TilefinchResourceGrant resource_grant = {0};
    if (use_cached) {
        css_data = cached_snapshot.retained_body == NULL
            ? NULL : cached_snapshot.retained_body->data;
        css_length = cached_length;
        response_provenance.response_url =
            cached_snapshot.retained_response_url;
        snprintf(response_provenance.referrer_policy,
                 sizeof(response_provenance.referrer_policy), "%s",
                 cached_snapshot.response_referrer_policy);
        response_provenance.known = true;
        resource_grant = cached->resource_grant;
        context->stats->cache_hits++;
    } else {
        fetched = fetch_result_create(context->budget);
        if (fetched == NULL) {
            pending_stylesheet_release(context, &cached_snapshot);
            return false;
        }
        FetchRequest transport = {
            .accept = "text/css,*/*;q=0.1",
            .if_none_match = cached == NULL ? NULL : cached->etag,
            .if_modified_since = cached == NULL
                                 ? NULL : cached->last_modified,
        };
        FetchPreparedPageRequest prepared;
        if (!fetch_prepare_page_request_context(
                &request_context, initiator->response_url,
                initiator->referrer_policy, context->session,
                context->content_security_policy, NULL, &transport,
                &prepared, NULL)) {
            fetch_result_free(fetched);
            pending_stylesheet_release(context, &cached_snapshot);
            return false;
        }
        const FetchRequest *request = fetch_prepared_page_request(&prepared);
        if (request == NULL || !fetch_scheduler_request(
                                     context->scheduler, resolved, request,
                                     maximum,
                                     stylesheet_remaining_ms(context),
                                     fetched)) {
            if (fetched->timed_out) context->deadline_reached = true;
            context->stats->failed++;
            document_resource_record_failure(
                context, resolved, NULL, fetched, false);
            fetch_result_free(fetched);
            pending_stylesheet_release(context, &cached_snapshot);
            return true;
        }
        resource_accept_response_cookies(
            context, &request_context, resolved, fetched);
        if (stylesheet_http_status_is_error(fetched)) {
            context->stats->failed++;
            document_resource_record_failure(
                context, resolved, NULL, fetched, true);
            fetch_result_free(fetched);
            pending_stylesheet_release(context, &cached_snapshot);
            return true;
        }
        if (fetched->status_code == 304 && cached != NULL
            && stylesheet_revalidation_provenance(
                   fetched, cached_snapshot.retained_response_url,
                   cached_snapshot.response_referrer_policy,
                   cached_provenance_known, &response_provenance)) {
            if (fetched->effective_url[0] == '\0') {
                snprintf(fetched->effective_url,
                         sizeof(fetched->effective_url), "%s",
                         response_provenance.response_url);
            }
            bool mime_valid = stylesheet_mime_matches(
                    cached_snapshot.content_type)
                && (fetched->content_type[0] == '\0'
                    || stylesheet_mime_matches(fetched->content_type));
            if (!cached_snapshot.resource_grant_valid
                || !fetch_resource_grant_revalidate_304(
                    fetched, &request_context,
                    &cached_snapshot.resource_grant,
                    false, mime_valid, false, &resource_grant, NULL)) {
                context->stats->failed++;
                document_resource_record_failure(
                    context, resolved, NULL, fetched, true);
                fetch_result_free(fetched);
                pending_stylesheet_release(context, &cached_snapshot);
                return true;
            }
            css_data = cached_snapshot.retained_body == NULL
                ? NULL : cached_snapshot.retained_body->data;
            css_length = cached_length;
            context->stats->cache_hits++;
            revalidated = true;
            use_cached = true;
        } else if (fetched->status_code >= 200
                   && fetched->status_code <= 299
                   && stylesheet_response_provenance(
                          fetched, &response_provenance)
                   && fetch_resource_grant_create(
                          fetched, &request_context, false,
                          stylesheet_mime_matches(fetched->content_type),
                          false, &resource_grant, NULL)) {
            css_data = (const unsigned char *) fetched->data;
            css_length = fetched->length;
        } else {
            context->stats->failed++;
            document_resource_record_failure(
                context, resolved, NULL, fetched, true);
            fetch_result_free(fetched);
            pending_stylesheet_release(context, &cached_snapshot);
            return true;
        }
    }
    bool parsed = apply_stylesheet_data(context, resolved,
                                        &response_provenance, css_data,
                                        css_length, depth, fetched,
                                        use_cached,
                                        use_cached
                                            ? cached_snapshot.retained_body
                                            : NULL,
                                        &request_context, &resource_grant,
                                        false);
    if (parsed) {
        BrowserSharedBody *body = use_cached
            ? cached_snapshot.retained_body
            : fetched == NULL ? NULL : fetched->shared_body;
        if (body == NULL && css_length != 0 && !use_cached
            && fetched != NULL && context->document_resources != NULL) {
            (void) fetch_result_share_body(fetched);
            body = fetched->shared_body;
        }
        document_resource_record_loaded(
            context, resolved, NULL, &response_provenance, body, css_length,
            true, false, TILEFINCH_CREDENTIALS_INCLUDE, &resource_grant);
    }
    if (revalidated) {
        (void) cache_revalidate_fetch(
            context->session, resolved, fetched, &response_provenance,
            &request_context, &resource_grant);
    }
    fetch_result_free(fetched);
    pending_stylesheet_release(context, &cached_snapshot);
    return parsed;
}

static void abandon_stylesheet_batch(ResourceContext *context,
                                     const char *reason)
{
    for (size_t i = 0; i < context->pending_count; i++) {
        PendingStylesheet *pending = &context->pending[i];
        if (pending->request_id != 0) {
            (void) fetch_scheduler_cancel(context->scheduler,
                                          pending->request_id, reason);
        }
        pending_stylesheet_release(context, pending);
    }
    context->pending_count = 0;
}

static void style_preload_settlement(ResourceContext *context,
                                     lxb_dom_node_t *element,
                                     StylesheetDocumentResourceState state)
{
    StylesheetDocumentResources *resources = context->document_resources;
    if (resources == NULL || element == NULL) return;
    size_t at = 0;
    while (at < resources->style_preload_count
           && resources->style_preloads[at].element != element) at++;
    if (at == STYLESHEET_DOCUMENT_RESOURCE_LIMIT) return;
    if (at == resources->style_preload_count) resources->style_preload_count++;
    resources->style_preloads[at].element = element;
    resources->style_preloads[at].state = state;
}

static bool flush_stylesheet_batch(ResourceContext *context)
{
    if (context->pending_count == 0) return true;
    context->stats->batches++;
    bool parsed = true;
    for (size_t i = 0; i < context->pending_count; i++) {
        PendingStylesheet *pending = &context->pending[i];
        /* A preload promoted by a matching active link is that link now. */
        bool preload_only = pending->preload && !pending->apply_rules;
        FetchResult *fetched = fetch_result_create(context->budget);
        if (fetched == NULL) {
            if (preload_only) {
                if (pending->request_id != 0) {
                    (void) fetch_scheduler_cancel(context->scheduler,
                        pending->request_id, "preload allocation refused");
                    (void) fetch_scheduler_discard(context->scheduler,
                                                   pending->request_id);
                }
                context->stats->failed++;
                pending_stylesheet_release(context, pending);
                continue;
            }
            for (size_t j = i; j < context->pending_count; j++) {
                PendingStylesheet *remaining = &context->pending[j];
                if (remaining->request_id != 0) {
                    (void) fetch_scheduler_cancel(
                        context->scheduler, remaining->request_id,
                        "stylesheet result allocation failed");
                }
                pending_stylesheet_release(context, remaining);
            }
            context->pending_count = 0;
            return false;
        }
        bool success = pending->request_id == 0
            && pending->response_provenance_known
            && (pending->retained_body != NULL || pending->retained_empty);
        if (!success) {
            for (;;) {
                if (fetch_scheduler_take(context->scheduler,
                                         pending->request_id,
                                         &success, fetched)) break;
                if (stylesheet_stage_expired(context)) {
                    for (size_t j = i; j < context->pending_count; j++) {
                        if (context->pending[j].request_id != 0) {
                            (void) fetch_scheduler_cancel(
                                context->scheduler,
                                context->pending[j].request_id,
                                "stylesheet stage deadline exceeded");
                        }
                    }
                    context->stats->deadline_cancelled +=
                        context->pending_count - i;
                }
                (void) fetch_scheduler_pump(
                    context->scheduler, STYLESHEET_FETCH_CONCURRENCY, 10);
                if (!resource_work(context, 1, true)) {
                    fetch_result_free(fetched);
                    abandon_stylesheet_batch(
                        context, "stylesheet pipeline cancelled");
                    return false;
                }
            }
        }
        /* A sheet over its byte cap still carries the received prefix. Its
           complete rules are applied in place (the cut only shortens the
           buffer already held); the unfinished tail is dropped. */
        bool truncated = false;
        size_t truncated_received = 0;
        if (!success && pending->apply_rules && pending->request_id != 0
            && fetched->response_limit_exceeded
            && fetched->status_code >= 200 && fetched->status_code <= 299
            && fetched->data != NULL && fetched->length != 0
            && fetched->shared_body == NULL) {
            size_t kept = stylesheet_complete_rules_prefix(
                fetched->data, fetched->length);
            if (kept != 0) {
                truncated_received = fetched->length;
                fetched->length = kept;
                fetched->data[kept] = '\0';
                truncated = true;
                success = true;
            }
        }
        if (context->stats->batches == 1 && success) {
            context->stats->first_batch_loaded++;
        }
        if (!success) {
            if (fetched->timed_out) context->deadline_reached = true;
            context->stats->failed++;
            /* A preload's quota, mode or transport refusal is its own; only
               an HTTP error status says the URL itself failed. */
            if (!preload_only || stylesheet_http_status_is_error(fetched)) {
                document_resource_record_failure(
                    context, pending->url,
                    pending->owns_url ? &pending->url : NULL,
                    fetched, false);
                if (pending->url == NULL) pending->owns_url = false;
            }
        } else {
            TilefinchRequestContext request_context =
                stylesheet_request_context(
                    context, pending->url, pending->cors,
                    pending->credentials);
            if (pending->request_id != 0) {
                resource_accept_response_cookies(
                    context, &request_context, pending->url, fetched);
            }
            if (pending->request_id != 0
                && stylesheet_http_status_is_error(fetched)) {
                context->stats->failed++;
                document_resource_record_failure(
                    context, pending->url,
                    pending->owns_url ? &pending->url : NULL,
                    fetched, true);
                if (pending->url == NULL) pending->owns_url = false;
                fetch_result_free(fetched);
                pending_stylesheet_release(context, pending);
                continue;
            }
            bool revalidated = false;
            bool retained_data = false;
            StylesheetResponseProvenance provenance = {0};
            TilefinchResourceGrant resource_grant =
                pending->resource_grant;
            bool resource_grant_valid = pending->resource_grant_valid;
            if (pending->request_id == 0) {
                provenance.response_url = pending->retained_response_url;
                snprintf(provenance.referrer_policy,
                         sizeof(provenance.referrer_policy), "%s",
                         pending->response_referrer_policy);
                provenance.known = pending->response_provenance_known;
                retained_data = true;
            } else if (fetched->status_code == 304) {
                revalidated = stylesheet_revalidation_provenance(
                    fetched, pending->retained_response_url,
                    pending->response_referrer_policy,
                    pending->response_provenance_known, &provenance);
                retained_data = revalidated
                    && (pending->retained_body != NULL
                        || pending->retained_empty);
                if (revalidated && provenance.response_url != NULL
                    && fetched->effective_url[0] == '\0') {
                    snprintf(fetched->effective_url,
                             sizeof(fetched->effective_url), "%s",
                             provenance.response_url);
                }
                bool mime_valid = stylesheet_mime_matches(
                        pending->content_type)
                    && (fetched->content_type[0] == '\0'
                        || stylesheet_mime_matches(fetched->content_type));
                success = revalidated && retained_data
                    && resource_grant_valid
                    && fetch_resource_grant_revalidate_304(
                        fetched, &request_context, &pending->resource_grant,
                        pending->cors, mime_valid, false,
                        &resource_grant, NULL);
                resource_grant_valid = success;
            } else if (fetched->status_code >= 200
                       && fetched->status_code <= 299) {
                success = stylesheet_response_provenance(
                    fetched, &provenance)
                    && fetch_resource_grant_create(
                        fetched, &request_context, pending->cors,
                        stylesheet_mime_matches(fetched->content_type),
                        false, &resource_grant, NULL);
                resource_grant_valid = success;
            } else {
                success = false;
            }
            if (!success || !provenance.known
                || provenance.response_url == NULL) {
                context->stats->failed++;
                if (!preload_only) {
                    document_resource_record_failure(
                        context, pending->url,
                        pending->owns_url ? &pending->url : NULL,
                        fetched, true);
                    if (pending->url == NULL) pending->owns_url = false;
                }
                fetch_result_free(fetched);
                pending_stylesheet_release(context, pending);
                continue;
            }
            const unsigned char *css_data = retained_data
                ? pending->retained_body == NULL
                    ? NULL : pending->retained_body->data
                : (const unsigned char *) fetched->data;
            size_t css_length = retained_data
                ? pending->retained_body == NULL
                    ? 0 : pending->retained_body->length
                : fetched->length;
            size_t integrity_length = 0;
            const char *integrity = pending->element == NULL ? NULL
                : document_attribute(
                    pending->element, "integrity", &integrity_length);
            bool integrity_present = integrity != NULL
                && integrity_length != 0;
            if (integrity_present && !pending->cors
                && !tilefinch_url_same_origin(
                       context->document_url, provenance.response_url)) {
                context->stats->failed++;
                fetch_result_free(fetched);
                pending_stylesheet_release(context, pending);
                continue;
            }
            TilefinchIntegrityResult integrity_result =
                tilefinch_resource_integrity_verify(
                    integrity, integrity_length, css_data, css_length);
            if (integrity_result == TILEFINCH_INTEGRITY_MISMATCH
                || integrity_result == TILEFINCH_INTEGRITY_INVALID) {
                context->stats->failed++;
                /* Integrity belongs to this link element, not to the URL.
                   A later link with different metadata must still settle. */
                fetch_result_free(fetched);
                pending_stylesheet_release(context, pending);
                continue;
            }
            if (revalidated) {
                context->stats->cache_hits++;
            }
            tilefinch_platform_trace_step("css-settle");
            /* The link's rules start here in the cascade (a promoted
               preload's element stands for the link that promoted it). */
            if (pending->apply_rules && pending->element != NULL) {
                stylesheet_note_style_source(context->sheet,
                                             pending->element);
            }
            size_t refused_before = context->stats->skipped_limit;
            if (!settle_stylesheet_data(
                    context, pending->url, &provenance, css_data,
                    css_length, 0,
                    retained_data ? NULL : fetched,
                    retained_data,
                    retained_data ? pending->retained_body : NULL,
                    pending->apply_rules, preload_only,
                    &request_context,
                    resource_grant_valid ? &resource_grant : NULL)) {
                parsed = false;
            } else if (preload_only
                       && context->stats->skipped_limit != refused_before) {
                /* Over the preload byte lane: the element errors and nothing
                   is retained or charged. */
            } else {
                if (pending->preload)
                    style_preload_settlement(context, pending->element,
                        STYLESHEET_DOCUMENT_RESOURCE_LOADED);
                if (revalidated && resource_grant_valid) {
                    (void) cache_revalidate_fetch(
                        context->session, pending->url, fetched, &provenance,
                        &request_context, &resource_grant);
                }
                BrowserSharedBody *body = retained_data
                    ? pending->retained_body
                    : fetched->shared_body;
                if (body == NULL && css_length != 0
                    && !retained_data
                    && context->document_resources != NULL) {
                    (void) fetch_result_share_body(fetched);
                    body = fetched->shared_body;
                }
                /* Preloads never take the ledger rows active links need. */
                StylesheetDocumentResources *resources =
                    context->document_resources;
                size_t reserved =
                    context->maximum_count < STYLESHEET_DOCUMENT_RESOURCE_LIMIT
                    ? context->maximum_count
                    : STYLESHEET_DOCUMENT_RESOURCE_LIMIT;
                const char *url = pending->url;
                bool retain = !preload_only || (resources != NULL
                    && (document_resource_find(resources, url) != NULL
                        || resources->count
                               < STYLESHEET_DOCUMENT_RESOURCE_LIMIT
                                 - reserved));
                if (retain) {
                    document_resource_record_loaded(
                        context, pending->url,
                        pending->owns_url ? &pending->url : NULL,
                        &provenance, body, css_length, pending->apply_rules,
                        pending->cors, pending->credentials,
                        resource_grant_valid ? &resource_grant : NULL);
                    if (pending->url == NULL) pending->owns_url = false;
                }
                if (truncated) {
                    stylesheet_note_truncated(
                        context, url, truncated_received, css_length);
                }
                StylesheetDocumentResource *entry = preload_only && retain
                    ? document_resource_find(resources, url) : NULL;
                if (entry != NULL) {
                    entry->preload_charged = true;
                    entry->preload_charge = css_length;
                }
            }
        }
        fetch_result_free(fetched);
        pending_stylesheet_release(context, pending);
        if (!parsed) {
            for (size_t j = i + 1; j < context->pending_count; j++) {
                PendingStylesheet *remaining = &context->pending[j];
                if (remaining->request_id != 0) {
                    (void) fetch_scheduler_cancel(
                        context->scheduler, remaining->request_id,
                        "stylesheet pipeline aborted");
                    bool ignored_success = false;
                    FetchResult *ignored = fetch_result_create(
                        context->budget);
                    if (ignored != NULL) {
                        (void) fetch_scheduler_take(context->scheduler,
                                                    remaining->request_id,
                                                    &ignored_success, ignored);
                    }
                    fetch_result_free(ignored);
                }
                pending_stylesheet_release(context, remaining);
            }
            break;
        }
    }
    context->pending_count = 0;
    return parsed;
}

static bool queue_stylesheet_link(ResourceContext *context,
                                  lxb_dom_node_t *node)
{
    size_t rel_length = 0;
    const char *rel = document_attribute(node, "rel", &rel_length);
    if (rel == NULL) return true;
    bool stylesheet = token_contains(rel, rel_length, "stylesheet");
    if (!stylesheet) {
        size_t as_length = 0;
        const char *as = document_attribute(node, "as", &as_length);
        if (!token_contains(rel, rel_length, "preload")
            || as == NULL || as_length != 5u
            || strncasecmp(as, "style", 5u) != 0) return true;
    }
    if (!stylesheet)
        style_preload_settlement(context, node,
            STYLESHEET_DOCUMENT_RESOURCE_TERMINAL_FAILURE);
    context->stats->discovered++;
    size_t media_length = 0;
    const char *media = document_attribute(node, "media", &media_length);
    bool media_matches = media == NULL || media_length == 0
        || stylesheet_media_matches(context->sheet, media, media_length);
    if (!media_matches) {
        context->stats->skipped_media++;
        if (!stylesheet) {
            style_preload_settlement(context, node,
                STYLESHEET_DOCUMENT_RESOURCE_EMPTY);
            return true;
        }
    }
    bool apply_rules = stylesheet && media_matches;
    if (stylesheet_stage_expired(context)) {
        context->stats->deadline_cancelled++;
        return true;
    }
    size_t href_length = 0;
    const char *href = document_attribute(node, "href", &href_length);
    if (href == NULL || href_length >= 2048) {
        size_t theme_length = 0, alternate_length = 0;
        const char *theme = document_attribute(
            node, "data-color-theme", &theme_length);
        const char *alternate = document_attribute(
            node, "data-href", &alternate_length);
        if (theme != NULL && theme_length != 0
            && alternate != NULL && alternate_length != 0) {
            context->stats->skipped_alternate_theme++;
        } else {
            context->stats->failed++;
        }
        return true;
    }
    char reference[2048];
    memcpy(reference, href, href_length);
    reference[href_length] = '\0';
    char resolved[4096];
    if (!fetch_resolve_url(context->base_url, reference, resolved,
                           sizeof(resolved))) {
        context->stats->failed++;
        return true;
    }
    if (stylesheet_is_inactive_alternate_theme(context, resolved)) {
        context->stats->skipped_alternate_theme++;
        return true;
    }
    /* A matching nonce admits the link whatever its URL (style-src). */
    uint8_t csp_grant = tilefinch_csp_element_grant(
        context->content_security_policy, TILEFINCH_DESTINATION_STYLE, node,
        true);
    if (!tilefinch_csp_allows_request_granted(
            context->content_security_policy,
            TILEFINCH_DESTINATION_STYLE, resolved, csp_grant)) {
        context->stats->failed++;
        return true;
    }
    size_t integrity_length = 0, crossorigin_length = 0;
    const char *integrity = document_attribute(
        node, "integrity", &integrity_length);
    const char *crossorigin = document_attribute(
        node, "crossorigin", &crossorigin_length);
    bool integrity_present = integrity != NULL && integrity_length != 0;
    bool cors = crossorigin != NULL;
    TilefinchCredentialsMode credentials = cors
        && crossorigin_length == strlen("use-credentials")
        && strncasecmp(crossorigin, "use-credentials",
                       crossorigin_length) == 0
            ? TILEFINCH_CREDENTIALS_INCLUDE
            : cors ? TILEFINCH_CREDENTIALS_SAME_ORIGIN
                   : TILEFINCH_CREDENTIALS_INCLUDE;
    uint32_t hash = hash_url(resolved);
    /* SRI is an element-scoped assertion. Do not allow a failed or differently
       annotated link to suppress a later link to the same URL. Ordinary
       unannotated references retain the bounded URL-level dedup fast path. */
    StylesheetReferenceState reference_state = integrity_present
        ? STYLESHEET_REFERENCE_NEW
        : stylesheet_reference_note(context, hash, apply_rules);
    /* Record the URL so a later active stylesheet can promote this queued
       preload without spending a second request slot. Each preload still
       verifies its own metadata before receiving its completion event. */
    if (!stylesheet) reference_state = STYLESHEET_REFERENCE_NEW;
    if (reference_state == STYLESHEET_REFERENCE_DUPLICATE) {
        context->stats->duplicate++;
        return true;
    }
    ExternalStylesheetStats *stats = context->stats;
    if (reference_state == STYLESHEET_REFERENCE_PROMOTED) {
        context->stats->duplicate++;
        size_t remaining = stats->bytes < context->maximum_total_bytes
            ? context->maximum_total_bytes - stats->bytes : 0;
        size_t wanted = stylesheet_response_bound(context, remaining);
        for (size_t i = 0; i < context->pending_count; i++) {
            PendingStylesheet *queued = &context->pending[i];
            size_t queued_integrity_length = 0;
            if (queued->element != NULL)
                (void) document_attribute(queued->element, "integrity",
                                           &queued_integrity_length);
            if (queued->url != NULL && strcmp(queued->url, resolved) == 0
                && queued->cors == cors
                && queued->credentials == credentials
                && queued_integrity_length == 0) {
                if (queued->preload && !queued->apply_rules) {
                    /* Promotion moves the response into the active lane:
                       it needs an active slot, and a request capped by the
                       smaller preload allowance must not cap this link. */
                    if (stats->attempted >= context->maximum_count
                        || queued->maximum_bytes < wanted) break;
                    if (stats->preload_attempted != 0)
                        stats->preload_attempted--;
                    stats->attempted++;
                }
                queued->apply_rules = true;
                return true;
            }
        }
    }
    StylesheetDocumentResource *document_resource = document_resource_find(
        context->document_resources, resolved);
    if (document_resource_suppresses(context, document_resource)) {
        if (!stylesheet)
            style_preload_settlement(context, node, document_resource->state);
        return true;
    }
    /* Each lane is admitted against its own charges (see
       style_preload_byte_limit). A preload refused here only errors its own
       element; it never records the URL as failed for active links. */
    size_t *attempted = stylesheet ? &stats->attempted
                                   : &stats->preload_attempted;
    size_t charged = stylesheet ? stats->bytes : stats->preload_bytes;
    size_t byte_limit = stylesheet ? context->maximum_total_bytes
                                   : style_preload_byte_limit(context);
    if (*attempted >= context->maximum_count || charged >= byte_limit) {
        context->stats->skipped_limit++;
        if (stylesheet && (document_resource == NULL
            || document_resource->state
                   != STYLESHEET_DOCUMENT_RESOURCE_LOADED)) {
            document_resource_record_failure(
                context, resolved, NULL, NULL, true);
        }
        return true;
    }
    if (context->pending_count == context->batch_limit
        && !flush_stylesheet_batch(context)) return false;
    /* The flush settles charges; read this lane's again. */
    charged = stylesheet ? stats->bytes : stats->preload_bytes;
    size_t remaining = charged < byte_limit ? byte_limit - charged : 0;
    size_t maximum = stylesheet_response_bound(context, remaining);
    if (maximum == 0) {
        context->stats->skipped_limit++;
        if (stylesheet)
            document_resource_record_failure(
                context, resolved, NULL, NULL, true);
        return true;
    }
    size_t url_length = strlen(resolved);
    char *url = budget_malloc(context->budget, url_length + 1);
    if (url == NULL) {
        if (!stylesheet) context->stats->failed++;
        return !stylesheet;
    }
    memcpy(url, resolved, url_length + 1);
    PendingStylesheet *pending =
        &context->pending[context->pending_count];
    pending->url = url;
    pending->owns_url = true;
    pending->maximum_bytes = maximum;
    pending->apply_rules = apply_rules;
    pending->preload = !stylesheet;
    pending->element = node;
    pending->cors = cors;
    pending->credentials = credentials;
    TilefinchRequestContext request_context = stylesheet_request_context(
        context, resolved, pending->cors, pending->credentials);
    request_context.csp_grant = csp_grant;
    if (integrity_present
        && !tilefinch_url_same_origin(context->document_url, resolved)
        && !pending->cors) {
        context->stats->failed++;
        pending_stylesheet_release(context, pending);
        return true;
    }
    if (document_resource != NULL
        && document_resource->state
               == STYLESHEET_DOCUMENT_RESOURCE_LOADED
        && document_resource->cors_validated == pending->cors
        && (!pending->cors
            || document_resource->credentials == pending->credentials)
        && document_resource->response_provenance_known
        && (document_resource->body != NULL
            || document_resource->length == 0)
        && pending_stylesheet_retain_response(
            context, pending, document_resource->body,
            document_resource->length,
            document_resource->response_url != NULL
                ? document_resource->response_url : document_resource->url,
            document_resource->response_referrer_policy)) {
        context->stats->cache_hits++;
        context->document_resources->retained_body_hits++;
        if (stylesheet)
            style_preload_charge_release(context, document_resource);
        (*attempted)++;
        context->pending_count++;
        return true;
    }
    const BrowserCacheEntry *cached = NULL;
    BrowserCacheStatus cache_status = context->session == NULL
        || content_blocker_would_block(
               context->session->content_blocker, resolved,
               context->document_url, "style",
               pending->cors ? "cors" : "no-cors")
        ? BROWSER_CACHE_MISS : browser_session_cache_match_resource(
            context->session, resolved, &request_context,
            tilefinch_platform_monotonic_time_ns(), &cached);
    bool cache_retained = cached != NULL
        && cached->response_url_known
        && cached->response_referrer_policy_known
        && cached->length <= maximum
        && (cached->length == 0 || cached->body != NULL)
        && pending_stylesheet_retain_response(
            context, pending, cached->body, cached->length,
            browser_cache_entry_response_url(cached),
            cached->response_referrer_policy);
    if (!cache_retained) {
        cached = NULL;
        cache_status = BROWSER_CACHE_MISS;
    } else {
        pending->resource_grant_valid = cached->resource_grant_valid;
        pending->resource_grant = cached->resource_grant;
        snprintf(pending->content_type, sizeof(pending->content_type), "%s",
                 cached->content_type);
    }
    if (cache_status == BROWSER_CACHE_FRESH) {
        context->stats->cache_hits++;
        (*attempted)++;
        context->pending_count++;
        return true;
    } else {
        char request_referrer_policy[STYLESHEET_REFERRER_POLICY_LIMIT];
        stylesheet_referrer_policy_for_node(
            node, context->document_referrer_policy,
            request_referrer_policy);
        FetchRequest transport = {
            .accept = "text/css,*/*;q=0.1",
            .if_none_match = cached == NULL ? NULL : cached->etag,
            .if_modified_since = cached == NULL
                                 ? NULL : cached->last_modified,
        };
        FetchPreparedPageRequest prepared;
        if (!fetch_prepare_page_request_context(
                &request_context, context->document_url,
                request_referrer_policy, context->session,
                context->content_security_policy, NULL,
                &transport, &prepared, NULL)) {
            pending_stylesheet_release(context, pending);
            if (!stylesheet) context->stats->failed++;
            return !stylesheet;
        }
        const FetchRequest *request = fetch_prepared_page_request(&prepared);
        pending->request_id = fetch_scheduler_enqueue(
            context->scheduler, resolved, request, maximum,
            stylesheet_remaining_ms(context));
        if (pending->request_id == 0) {
            (*attempted)++;
            context->stats->failed++;
            /* Scheduler admission failure is transient.  Do not materialize a
               full FetchResult merely to classify this known outcome: that
               object would add roughly 12 KiB to this PSP stack frame. */
            document_resource_record_failure(
                context, pending->url, &pending->url, NULL, false);
            pending->owns_url = pending->url != NULL;
            pending_stylesheet_release(context, pending);
            return true;
        }
    }
    (*attempted)++;
    context->pending_count++;
    return true;
}

static bool process_stylesheet_node(
    ResourceContext *context, lxb_dom_node_t *node)
{
    if (node == NULL || node->type != LXB_DOM_NODE_TYPE_ELEMENT) return true;
    if (name_is(node, "style")) {
        if (!tilefinch_csp_allows_inline_style(
                context->content_security_policy, node)) return true;
        size_t override_length = 0;
        const char *override = document_cssom_sheet_text(node, &override_length);
        size_t inline_bytes = 0;
        size_t inline_working = 0;
        if (override != NULL) {
            inline_bytes = override_length;
            inline_working = stylesheet_source_cost_estimate(override, override_length);
        }
        for (lxb_dom_node_t *child = override == NULL ? node->first_child : NULL;
             child != NULL; child = child->next) {
            size_t css_length = 0;
            const char *css = document_text_data(child, &css_length);
            inline_bytes = stylesheet_estimate_add(inline_bytes, css_length);
            inline_working = stylesheet_estimate_add(
                inline_working,
                stylesheet_source_cost_estimate(css, css_length));
        }
        if (!stylesheet_parse_admitted(context, inline_bytes,
                                       inline_working)) return true;
        /* A queued external sheet precedes this inline block in the
           document and must be parsed first. Consecutive links remain
           batched, preserving parallel fetch without changing the author
           cascade. */
        if (!flush_stylesheet_batch(context)) return false;
        StylesheetResponseProvenance inline_sheet = {
            .response_url = context->base_url,
            .known = true
        };
        snprintf(inline_sheet.referrer_policy,
                 sizeof(inline_sheet.referrer_policy), "%s",
                 context->document_referrer_policy);
        if (override != NULL && !load_stylesheet_imports(
                context, override, override_length, &inline_sheet, 0)) return false;
        for (lxb_dom_node_t *child = override == NULL ? node->first_child : NULL;
             child != NULL; child = child->next) {
            size_t css_length = 0;
            const char *css = document_text_data(child, &css_length);
            if (css != NULL
                && !load_stylesheet_imports(
                    context, css, css_length, &inline_sheet, 0)) {
                return false;
            }
        }
        return stylesheet_add_style_element(
            context->sheet, node, context->content_security_policy);
    }
    return !name_is(node, "link")
        || queue_stylesheet_link(context, node);
}

/* The document's adopted constructed sheets, once each, after every <style>
   and <link> source (document_adopted_sheets_active). They are admitted
   like inline blocks; no style-src check applies to script-built sheets,
   and they cannot @import. */
static bool process_adopted_sheets(ResourceContext *context,
                                   const PocDocument *document)
{
    lxb_dom_node_t *nodes[DOCUMENT_ADOPTED_TIER_LIMIT];
    uint32_t revisions[DOCUMENT_ADOPTED_TIER_LIMIT];
    size_t count = document_adopted_sheets_active(
        document, nodes, revisions, DOCUMENT_ADOPTED_TIER_LIMIT);
    if (count == SIZE_MAX) count = DOCUMENT_ADOPTED_TIER_LIMIT;
    for (size_t i = 0; i < count; i++) {
        if (!resource_work(context, 1, false)) return false;
        size_t bytes = 0, working = 0;
        size_t override_length = 0;
        const char *override = document_cssom_sheet_text(nodes[i], &override_length);
        if (override != NULL) {
            bytes = override_length;
            working = stylesheet_source_cost_estimate(override, override_length);
        }
        for (lxb_dom_node_t *child = override == NULL ? nodes[i]->first_child : NULL;
             child != NULL; child = child->next) {
            size_t css_length = 0;
            const char *css = document_text_data(child, &css_length);
            bytes = stylesheet_estimate_add(bytes, css_length);
            working = stylesheet_estimate_add(
                working, stylesheet_source_cost_estimate(css, css_length));
        }
        if (!stylesheet_parse_admitted(context, bytes, working)) continue;
        if (!flush_stylesheet_batch(context)
            || !stylesheet_add_adopted_sheet(
                   context->sheet, document, nodes[i], revisions[i]))
            return false;
    }
    (void) stylesheet_set_adopted_scopes(context->sheet, document, NULL, 0,
                                         NULL, NULL);
    return true;
}

static bool walk(ResourceContext *context, lxb_dom_node_t *node)
{
    if (node == NULL) return true;
    lxb_dom_node_t *const boundary_parent = node->parent;
    for (;;) {
        if (!resource_work(context, 1, false)) return false;
        if (!process_stylesheet_node(context, node)) return false;
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node->next == NULL) {
            if (node->parent == boundary_parent) return true;
            node = node->parent;
        }
        node = node->next;
    }
}

static bool stylesheets_append_ordered_suffix_impl(
    Stylesheet *sheet, Budget *budget, lxb_dom_node_t *const *nodes,
    size_t node_count, const char *base_url, const char *document_url,
    const char *document_referrer_policy,
    const TilefinchContentSecurityPolicy *content_security_policy,
    size_t maximum_count,
    size_t maximum_total_bytes, size_t maximum_single_bytes,
    long timeout_ms, FetchScheduler *scheduler, BrowserSession *session,
    StylesheetDocumentResources *resources,
    ExternalStylesheetStats *stats, bool rule_batch);

bool stylesheets_append_ordered_suffix_with_context(
    Stylesheet *sheet, Budget *budget, lxb_dom_node_t *const *nodes,
    size_t node_count, const char *base_url, const char *document_url,
    const char *document_referrer_policy,
    const TilefinchContentSecurityPolicy *content_security_policy,
    size_t maximum_count,
    size_t maximum_total_bytes, size_t maximum_single_bytes,
    long timeout_ms, FetchScheduler *scheduler, BrowserSession *session,
    StylesheetDocumentResources *resources,
    ExternalStylesheetStats *stats)
{
    return stylesheets_append_ordered_suffix_impl(
        sheet, budget, nodes, node_count, base_url, document_url,
        document_referrer_policy, content_security_policy, maximum_count,
        maximum_total_bytes, maximum_single_bytes, timeout_ms, scheduler,
        session, resources, stats, true);
}

bool stylesheets_settle_style_preloads_with_context(
    Stylesheet *sheet, Budget *budget, lxb_dom_node_t *const *nodes,
    size_t node_count, const char *base_url, const char *document_url,
    const char *document_referrer_policy,
    const TilefinchContentSecurityPolicy *content_security_policy,
    size_t maximum_count,
    size_t maximum_total_bytes, size_t maximum_single_bytes,
    long timeout_ms, FetchScheduler *scheduler, BrowserSession *session,
    StylesheetDocumentResources *resources,
    ExternalStylesheetStats *stats)
{
    for (size_t i = 0; nodes != NULL && i < node_count; i++) {
        size_t rel_length = 0, as_length = 0;
        const char *rel = document_attribute(nodes[i], "rel", &rel_length);
        const char *as = document_attribute(nodes[i], "as", &as_length);
        if (!name_is(nodes[i], "link") || rel == NULL
            || token_contains(rel, rel_length, "stylesheet")
            || !token_contains(rel, rel_length, "preload")
            || as == NULL || as_length != 5u
            || strncasecmp(as, "style", 5u) != 0) return false;
    }
    return stylesheets_append_ordered_suffix_impl(
        sheet, budget, nodes, node_count, base_url, document_url,
        document_referrer_policy, content_security_policy, maximum_count,
        maximum_total_bytes, maximum_single_bytes, timeout_ms, scheduler,
        session, resources, stats, false);
}

static bool stylesheets_append_ordered_suffix_impl(
    Stylesheet *sheet, Budget *budget, lxb_dom_node_t *const *nodes,
    size_t node_count, const char *base_url, const char *document_url,
    const char *document_referrer_policy,
    const TilefinchContentSecurityPolicy *content_security_policy,
    size_t maximum_count,
    size_t maximum_total_bytes, size_t maximum_single_bytes,
    long timeout_ms, FetchScheduler *scheduler, BrowserSession *session,
    StylesheetDocumentResources *resources,
    ExternalStylesheetStats *stats, bool rule_batch)
{
    char normalized_document_policy[STYLESHEET_REFERRER_POLICY_LIMIT];
    stylesheet_referrer_policy_normalize_or_default(
        document_referrer_policy, normalized_document_policy);
    TilefinchUrl parsed_document_url;
    if (sheet == NULL || budget == NULL || nodes == NULL || node_count == 0
        || base_url == NULL || document_url == NULL
        || !tilefinch_url_parse(document_url, &parsed_document_url)
        || maximum_count == 0 || maximum_count > MAX_TRACKED_STYLESHEETS
        || maximum_total_bytes == 0 || maximum_single_bytes == 0
        || timeout_ms <= 0 || scheduler == NULL || stats == NULL
        || (resources != NULL && resources->budget != budget)) return false;
    if (rule_batch && !stylesheet_begin_rule_batch(sheet)) return false;
    ResourceContext context = {
        .sheet = sheet,
        .budget = budget,
        .base_url = base_url,
        .document_url = document_url,
        .document_referrer_policy = normalized_document_policy,
        .content_security_policy = content_security_policy,
        .maximum_count = maximum_count,
        .maximum_total_bytes = maximum_total_bytes,
        .maximum_single_bytes = maximum_single_bytes,
        .timeout_ms = timeout_ms,
        .scheduler = scheduler,
        .session = session,
        .document_resources = resources,
        .stats = stats,
        .batch_limit = STYLESHEET_FETCH_BATCH,
        .started_ms = resource_now_ms(),
        .slice_started_us = tilefinch_platform_monotonic_time_us()
    };
    tilefinch_platform_trace_step("css-prelude");
    lxb_dom_node_t *selector_root = nodes[0];
    while (selector_root != NULL && selector_root->parent != NULL) {
        selector_root = selector_root->parent;
    }
    stylesheet_collect_selector_tokens(
        selector_root, context.priority_token_bloom,
        STYLESHEET_PRIORITY_TOKEN_BLOOM_WORDS,
        STYLESHEET_SELECTOR_PRIORITY_NODE_LIMIT, false);
    context.selector_token_census_complete =
        stylesheet_collect_selector_tokens(
            selector_root, context.selector_token_bloom,
            STYLESHEET_SELECTOR_TOKEN_BLOOM_WORDS,
            STYLESHEET_SELECTOR_TOKEN_NODE_LIMIT, true);
    context.deadline_ms = context.started_ms + (double) timeout_ms;
    stylesheet_restore_alternate_themes(&context);
    /* The full ordered loader suppresses URLs already encountered earlier
       in the document, including imports. Seed the suffix context from the
       document resource ledger so continuation preserves that behavior. */
    for (size_t i = 0; resources != NULL && i < resources->count
         && context.hash_count < MAX_TRACKED_STYLESHEETS; i++) {
        const StylesheetDocumentResource *resource = &resources->items[i];
        if (resource->url == NULL) continue;
        context.hashes[context.hash_count] = hash_url(resource->url);
        context.hash_applied[context.hash_count] = resource->rules_applied;
        context.hash_count++;
    }
    bool ok = true;
    for (size_t i = 0; ok && i < node_count; i++) {
        ok = resource_work(&context, 1, false)
            && process_stylesheet_node(&context, nodes[i]);
    }
    tilefinch_platform_trace_step("css-flush");
    if (ok) ok = flush_stylesheet_batch(&context);
    else abandon_stylesheet_batch(
        &context, "stylesheet suffix continuation cancelled");
    if (rule_batch && !stylesheet_end_rule_batch(sheet)) ok = false;
    resource_finish_slice(&context);
    double elapsed = resource_now_ms() - context.started_ms;
    if (elapsed < 0.0) elapsed = 0.0;
    uint64_t elapsed_ms = (uint64_t) elapsed;
    stats->elapsed_ms = elapsed_ms > UINT64_MAX - stats->elapsed_ms
        ? UINT64_MAX : stats->elapsed_ms + elapsed_ms;
    stats->deadline_exceeded = stats->deadline_exceeded
        || context.deadline_reached
        || resource_now_ms() >= context.deadline_ms;
    if (resources != NULL) {
        stats->retained_body_hits = resources->retained_body_hits;
        stats->transient_retries = resources->transient_retries;
        stats->transient_failures = resources->transient_failures;
        stats->terminal_failures = resources->terminal_failures;
        stats->retry_suppressed = resources->retry_suppressed;
        stats->final_retry_grants = resources->final_retry_grants;
        stats->pressure_serializations = resources->pressure_serializations;
        stats->truncated = resources->truncated;
        stats->truncated_received_bytes =
            resources->truncated_received_bytes;
        stats->truncated_applied_bytes = resources->truncated_applied_bytes;
    }
    return ok;
}

bool stylesheets_load_external_tracked_with_context(
    const PocDocument *document, Stylesheet *sheet, Budget *budget,
    const char *base_url, const char *document_url,
    const char *document_referrer_policy, size_t maximum_count,
    size_t maximum_total_bytes, size_t maximum_single_bytes,
    long timeout_ms, FetchScheduler *scheduler, BrowserSession *session,
    StylesheetDocumentResources *resources,
    ExternalStylesheetStats *stats)
{
    char normalized_document_policy[STYLESHEET_REFERRER_POLICY_LIMIT];
    stylesheet_referrer_policy_normalize_or_default(
        document_referrer_policy, normalized_document_policy);
    TilefinchUrl parsed_document_url;
    if (document == NULL || document->html == NULL || sheet == NULL
        || budget == NULL || base_url == NULL || document_url == NULL
        || !tilefinch_url_parse(document_url, &parsed_document_url)
        || stats == NULL
        || maximum_count == 0 || maximum_count > MAX_TRACKED_STYLESHEETS
        || maximum_total_bytes == 0 || maximum_single_bytes == 0
        || timeout_ms <= 0
        || (resources != NULL && resources->budget != NULL
            && resources->budget != budget)) return false;
    memset(stats, 0, sizeof(*stats));
    if (resources != NULL && resources->budget == NULL) {
        resources->budget = budget;
    }
    /* This pass walks the current document. Discard old element identities;
       suffix continuation keeps earlier settlements until the next rebuild. */
    if (resources != NULL) resources->style_preload_count = 0;
    /* Preload charges belong to the stats this pass just cleared. */
    for (size_t i = 0; resources != NULL && i < resources->count; i++) {
        resources->items[i].preload_charged = false;
        resources->items[i].preload_charge = 0;
    }
    bool owns_scheduler = scheduler == NULL;
    if (owns_scheduler) {
        size_t reserved = maximum_single_bytes;
        if (reserved <= SIZE_MAX / STYLESHEET_FETCH_CONCURRENCY) {
            reserved *= STYLESHEET_FETCH_CONCURRENCY;
        } else {
            reserved = SIZE_MAX;
        }
        scheduler = fetch_scheduler_create(
            budget, STYLESHEET_FETCH_CONCURRENCY, reserved);
        if (scheduler == NULL) return false;
    }
    /* stylesheet_build_context handles inline-only pages. Once external
       resources participate, reconstruct the author sheet in true DOM input
       order so <style>, <link>, and imported rules share one cascade timeline. */
    if (!sheet->document_rules_deferred
        && !stylesheet_reset_document_rules(sheet)) {
        if (owns_scheduler) fetch_scheduler_destroy(scheduler);
        return false;
    }
    bool batch_rules = true;
#ifndef TILEFINCH_NO_TRACE
    batch_rules = getenv("TILEFINCH_DISABLE_STYLESHEET_RULE_BATCH") == NULL;
#endif
    if (batch_rules && !stylesheet_begin_rule_batch(sheet)) {
        if (owns_scheduler) fetch_scheduler_destroy(scheduler);
        return false;
    }
    ResourceContext context = {
        .sheet = sheet,
        .budget = budget,
        .base_url = base_url,
        .document_url = document_url,
        .document_referrer_policy = normalized_document_policy,
        .content_security_policy = &document->content_security_policy,
        .maximum_count = maximum_count,
        .maximum_total_bytes = maximum_total_bytes,
        .maximum_single_bytes = maximum_single_bytes,
        .timeout_ms = timeout_ms,
        .scheduler = scheduler,
        .session = session,
        .document_resources = resources,
        .stats = stats,
        .batch_limit = STYLESHEET_FETCH_BATCH,
        .started_ms = resource_now_ms(),
        .slice_started_us = tilefinch_platform_monotonic_time_us()
    };
    tilefinch_platform_trace_step("css-prelude");
    stylesheet_collect_selector_tokens(
        lxb_dom_interface_node(document->html), context.priority_token_bloom,
        STYLESHEET_PRIORITY_TOKEN_BLOOM_WORDS,
        STYLESHEET_SELECTOR_PRIORITY_NODE_LIMIT, false);
    context.selector_token_census_complete =
        stylesheet_collect_selector_tokens(
            lxb_dom_interface_node(document->html),
            context.selector_token_bloom,
            STYLESHEET_SELECTOR_TOKEN_BLOOM_WORDS,
            STYLESHEET_SELECTOR_TOKEN_NODE_LIMIT, true);
    size_t parallel_working = maximum_single_bytes;
    if (parallel_working <= SIZE_MAX / STYLESHEET_FETCH_BATCH) {
        parallel_working *= STYLESHEET_FETCH_BATCH;
    } else {
        parallel_working = SIZE_MAX;
    }
    if (budget_pressure_required(budget, parallel_working,
                                 2u * 1024u * 1024u)) {
        context.batch_limit = 1;
        if (resources != NULL) resources->pressure_serializations++;
        size_t avoided = maximum_single_bytes
            <= SIZE_MAX / (STYLESHEET_FETCH_BATCH - 1)
            ? maximum_single_bytes * (STYLESHEET_FETCH_BATCH - 1)
            : SIZE_MAX;
        budget_record_pressure(budget, BUDGET_PRESSURE_STYLESHEET,
                               avoided, 0);
    }
    context.deadline_ms = context.started_ms + (double) timeout_ms;
    /* Script frameworks commonly normalize the root's theme attributes after
       the first viewport pass. Preserve the already validated page choice
       when that later DOM no longer carries a complete registry; a newly
       discoverable explicit registry below still replaces it. */
    stylesheet_restore_alternate_themes(&context);
    stylesheet_collect_alternate_themes(
        &context, lxb_dom_interface_node(document->html));
    tilefinch_platform_trace_step("css-walk");
    bool ok = walk(&context, lxb_dom_interface_node(document->html));
    tilefinch_platform_trace_step("css-flush");
    if (ok) ok = flush_stylesheet_batch(&context);
    else abandon_stylesheet_batch(&context,
                                   "stylesheet discovery cancelled");
    if (ok) ok = process_adopted_sheets(&context, document);
    if (batch_rules && !stylesheet_end_rule_batch(sheet)) ok = false;
    resource_finish_slice(&context);
    if (ok) sheet->document_rules_deferred = false;
    double elapsed = resource_now_ms() - context.started_ms;
    if (elapsed < 0.0) elapsed = 0.0;
    stats->deadline_exceeded = context.deadline_reached
        || resource_now_ms() >= context.deadline_ms;
    stats->elapsed_ms = (uint64_t) elapsed;
    if (resources != NULL) {
        stats->retained_body_hits = resources->retained_body_hits;
        stats->transient_retries = resources->transient_retries;
        stats->transient_failures = resources->transient_failures;
        stats->terminal_failures = resources->terminal_failures;
        stats->retry_suppressed = resources->retry_suppressed;
        stats->final_retry_grants = resources->final_retry_grants;
        stats->pressure_serializations = resources->pressure_serializations;
        stats->truncated = resources->truncated;
        stats->truncated_received_bytes =
            resources->truncated_received_bytes;
        stats->truncated_applied_bytes = resources->truncated_applied_bytes;
    }
    if (owns_scheduler) fetch_scheduler_destroy(scheduler);
    return ok;
}

bool stylesheets_load_external_tracked(
    const PocDocument *document, Stylesheet *sheet, Budget *budget,
    const char *base_url, size_t maximum_count,
    size_t maximum_total_bytes, size_t maximum_single_bytes,
    long timeout_ms, FetchScheduler *scheduler, BrowserSession *session,
    StylesheetDocumentResources *resources,
    ExternalStylesheetStats *stats)
{
    return stylesheets_load_external_tracked_with_context(
        document, sheet, budget, base_url, base_url, "", maximum_count,
        maximum_total_bytes, maximum_single_bytes, timeout_ms, scheduler,
        session, resources, stats);
}

bool stylesheets_load_external_with_context(
    const PocDocument *document, Stylesheet *sheet, Budget *budget,
    const char *base_url, const char *document_url,
    const char *document_referrer_policy, size_t maximum_count,
    size_t maximum_total_bytes, size_t maximum_single_bytes,
    long timeout_ms, FetchScheduler *scheduler, BrowserSession *session,
    ExternalStylesheetStats *stats)
{
    return stylesheets_load_external_tracked_with_context(
        document, sheet, budget, base_url, document_url,
        document_referrer_policy, maximum_count, maximum_total_bytes,
        maximum_single_bytes, timeout_ms, scheduler, session, NULL, stats);
}

bool stylesheets_load_external(const PocDocument *document, Stylesheet *sheet,
                               Budget *budget, const char *base_url,
                               size_t maximum_count,
                               size_t maximum_total_bytes,
                               size_t maximum_single_bytes,
                               long timeout_ms,
                               FetchScheduler *scheduler,
                               BrowserSession *session,
                               ExternalStylesheetStats *stats)
{
    return stylesheets_load_external_with_context(
        document, sheet, budget, base_url, base_url, "", maximum_count,
        maximum_total_bytes, maximum_single_bytes, timeout_ms, scheduler,
        session, stats);
}

static void font_accept_response_cookies(
    BrowserSession *session, TilefinchRequestContext *context,
    const char *fallback_url, const FetchResult *fetch)
{
    if (session == NULL || context == NULL || fetch == NULL) return;
    for (size_t i = 0; i < fetch->set_cookie_count; i++) {
        context->target_url = fetch_set_cookie_url(fetch, i, fallback_url);
        (void) browser_session_cookie_set_http_context(
            session, context, fetch->set_cookies[i]);
    }
    context->target_url = fallback_url;
}

static size_t font_charge_encoded_bytes(ExternalFontStats *stats,
                                        size_t bytes, size_t maximum)
{
    if (stats == NULL || bytes == 0 || stats->encoded_bytes >= maximum) {
        return 0;
    }
    size_t remaining = maximum - stats->encoded_bytes;
    size_t charged = bytes < remaining ? bytes : remaining;
    stats->encoded_bytes += charged;
    if (charged != bytes) stats->skipped_limit++;
    return charged;
}

static bool font_loader_arguments_valid(
    Stylesheet *sheet, Budget *budget, const char *document_base_url,
    const char *document_url, size_t maximum_attempts,
    size_t maximum_total_encoded_bytes,
    size_t maximum_single_encoded_bytes,
    size_t maximum_face_backend_bytes, FetchScheduler *scheduler,
    ExternalFontStats *stats)
{
    if (sheet == NULL || budget == NULL || document_base_url == NULL
        || document_url == NULL || stats == NULL || maximum_attempts == 0
        || maximum_attempts > TILEFINCH_WEB_FONT_FAMILY_LIMIT * 2u
                               * STYLE_WEB_FONT_SOURCE_LIMIT
        || maximum_total_encoded_bytes == 0
        || maximum_single_encoded_bytes == 0
        || maximum_face_backend_bytes == 0 || scheduler == NULL) return false;
    return true;
}

static void font_loader_update_elapsed(ExternalFontLoader *loader,
                                       ExternalFontStats *stats)
{
    if (loader == NULL || stats == NULL || loader->started_ms <= 0.0) return;
    double elapsed = resource_now_ms() - loader->started_ms;
    if (elapsed < 0.0) elapsed = 0.0;
    stats->elapsed_ms = (uint64_t) elapsed;
}

bool fonts_external_loader_begin(ExternalFontLoader *loader,
                                 Stylesheet *sheet, long timeout_ms,
                                 ExternalFontStats *stats)
{
    if (loader == NULL || sheet == NULL || stats == NULL
        || timeout_ms <= 0) return false;
    memset(loader, 0, sizeof(*loader));
    memset(stats, 0, sizeof(*stats));
    StylesheetWebFontStats discovery = {0};
    (void) stylesheet_web_font_stats(sheet, &discovery);
    stats->declarations_discovered = discovery.declarations_discovered;
    stats->sources_discovered = stylesheet_web_font_source_count(sheet);
    stats->unsupported = discovery.unsupported_sources;
    stats->duplicate_sources = discovery.duplicate_sources;
    stats->skipped_limit = discovery.skipped_family_limit
                           + discovery.skipped_source_limit;
    loader->started_ms = resource_now_ms();
    loader->deadline_ms = loader->started_ms + (double) timeout_ms;
    loader->active = stats->sources_discovered != 0;
    return true;
}

bool fonts_external_loader_pending(const ExternalFontLoader *loader)
{
    return loader != NULL && loader->active;
}

void fonts_external_loader_cancel(ExternalFontLoader *loader,
                                  FetchScheduler *scheduler,
                                  ExternalFontStats *stats)
{
    if (loader == NULL) return;
    if (loader->request_id != 0 && scheduler != NULL) {
        (void) fetch_scheduler_cancel(
            scheduler, loader->request_id, "font continuation cancelled");
        (void) fetch_scheduler_discard(scheduler, loader->request_id);
    }
    loader->request_id = 0;
    loader->active = false;
    font_loader_update_elapsed(loader, stats);
}

static void font_loader_expire(ExternalFontLoader *loader,
                               FetchScheduler *scheduler,
                               ExternalFontStats *stats)
{
    if (loader == NULL || stats == NULL) return;
    stats->deadline_exceeded = true;
    if (loader->active
        && loader->next_source < stats->sources_discovered) {
        stats->deadline_cancelled +=
            stats->sources_discovered - loader->next_source;
    }
    fonts_external_loader_cancel(loader, scheduler, stats);
}

static bool font_loader_resolve_source(
    Stylesheet *sheet, size_t index, const char *document_base_url,
    const char *document_url, const char *referrer_policy,
    StylesheetWebFontSource *source, FontFace **destination,
    const char **referrer_source, const char **source_referrer_policy,
    char resolved[4096])
{
    if (!stylesheet_web_font_source(sheet, index, source)) return false;
    *destination = stylesheet_web_font_face(
        sheet, source->family_slot, source->bold);
    if (*destination == NULL) return false;
    const char *source_base = source->source_base_url != NULL
        ? source->source_base_url : document_base_url;
    *referrer_source = source->source_base_url != NULL
        ? source->source_base_url : document_url;
    *source_referrer_policy = source->source_referrer_policy != NULL
        ? source->source_referrer_policy : referrer_policy;
    return source->reference != NULL
        && fetch_resolve_url(
            source_base, source->reference, resolved, 4096);
}

bool fonts_external_loader_step(
    ExternalFontLoader *loader, Stylesheet *sheet, Budget *budget,
    const char *document_base_url, const char *document_url,
    const char *referrer_policy, size_t maximum_attempts,
    size_t maximum_total_encoded_bytes,
    size_t maximum_single_encoded_bytes,
    size_t maximum_face_backend_bytes, FetchScheduler *scheduler,
    BrowserSession *session, const TilefinchContentSecurityPolicy *csp,
    ExternalFontStats *stats,
    unsigned maximum_wait_ms, bool *face_loaded)
{
    if (loader == NULL || face_loaded == NULL
        || !font_loader_arguments_valid(
            sheet, budget, document_base_url, document_url,
            maximum_attempts, maximum_total_encoded_bytes,
            maximum_single_encoded_bytes, maximum_face_backend_bytes,
            scheduler, stats)) return false;
    *face_loaded = false;
    if (!loader->active) return true;
    if (resource_now_ms() >= loader->deadline_ms) {
        font_loader_expire(loader, scheduler, stats);
        return true;
    }

    char origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    if (!tilefinch_url_origin(document_url, origin, sizeof(origin))) {
        stats->failed += stats->sources_discovered - loader->next_source;
        loader->active = false;
        font_loader_update_elapsed(loader, stats);
        return true;
    }

    if (loader->request_id != 0) {
        FetchPumpQuota quota = {
            .maximum_body_callbacks = 2,
            .maximum_body_bytes = 32u * 1024u,
            .maximum_time_us = 2000
        };
        FetchPumpMetrics metrics = {0};
        (void) fetch_scheduler_pump_bounded(
            scheduler, 1, maximum_wait_ms > 10 ? 10 : maximum_wait_ms,
            &quota, &metrics);
        FetchRequestProgress progress = {0};
        if (!fetch_scheduler_request_progress(
                scheduler, loader->request_id, &progress)
            || !progress.complete) {
            if (resource_now_ms() >= loader->deadline_ms) {
                font_loader_expire(loader, scheduler, stats);
            } else {
                font_loader_update_elapsed(loader, stats);
            }
            return true;
        }
        FetchResult *fetched = fetch_result_create(budget);
        if (fetched == NULL) {
            (void) fetch_scheduler_discard(
                scheduler, loader->request_id);
            loader->request_id = 0;
            loader->next_source = loader->pending_source + 1u;
            loader->active =
                loader->next_source < stats->sources_discovered;
            stats->failed++;
            font_loader_update_elapsed(loader, stats);
            return true;
        }
        bool success = false;
        if (!fetch_scheduler_take(
                scheduler, loader->request_id, &success, fetched)) {
            fetch_result_free(fetched);
            if (resource_now_ms() >= loader->deadline_ms) {
                font_loader_expire(loader, scheduler, stats);
            } else {
                font_loader_update_elapsed(loader, stats);
            }
            return true;
        }
        size_t settled_index = loader->pending_source;
        loader->request_id = 0;
        loader->next_source = settled_index + 1u;
        loader->active =
            loader->next_source < stats->sources_discovered;
        StylesheetWebFontSource source = {0};
        FontFace *destination = NULL;
        const char *referrer_source = NULL;
        const char *source_referrer_policy = NULL;
        char resolved[4096];
        if (!font_loader_resolve_source(
                sheet, settled_index, document_base_url, document_url,
                referrer_policy, &source, &destination, &referrer_source,
                &source_referrer_policy, resolved)) {
            stats->failed++;
            fetch_result_free(fetched);
            font_loader_update_elapsed(loader, stats);
            return true;
        }
        size_t source_bytes_charged = font_charge_encoded_bytes(
            stats, fetched->received_body_bytes,
            maximum_total_encoded_bytes);
        if (!success) {
            if (fetched->timed_out) stats->deadline_exceeded = true;
            stats->failed++;
            fetch_result_free(fetched);
            font_loader_update_elapsed(loader, stats);
            return true;
        }
        TilefinchRequestContext request_context = {
            .target_url = resolved,
            .initiator_url = document_url,
            .top_level_url = document_url,
            .method = "GET",
            .mode = TILEFINCH_REQUEST_MODE_CORS,
            .credentials = TILEFINCH_CREDENTIALS_SAME_ORIGIN,
            .destination = TILEFINCH_DESTINATION_FONT
        };
        font_accept_response_cookies(
            session, &request_context, resolved, fetched);
        if (fetched->status_code < 200 || fetched->status_code > 299) {
            stats->failed++;
            fetch_result_free(fetched);
            font_loader_update_elapsed(loader, stats);
            return true;
        }
        size_t additional_bytes =
            fetched->length > source_bytes_charged
                ? fetched->length - source_bytes_charged : 0;
        size_t request_maximum =
            maximum_total_encoded_bytes >=
                    stats->encoded_bytes - source_bytes_charged
                ? maximum_total_encoded_bytes
                    - (stats->encoded_bytes - source_bytes_charged)
                : 0;
        if (request_maximum > maximum_single_encoded_bytes) {
            request_maximum = maximum_single_encoded_bytes;
        }
        if (fetched->length > request_maximum
            || additional_bytes > maximum_total_encoded_bytes
                                   - stats->encoded_bytes) {
            stats->skipped_limit++;
            fetch_result_free(fetched);
            font_loader_update_elapsed(loader, stats);
            return true;
        }
        if (additional_bytes != 0) {
            (void) font_charge_encoded_bytes(
                stats, additional_bytes, maximum_total_encoded_bytes);
        }
        if (font_face_load_encoded(
                destination, budget, (const unsigned char *) fetched->data,
                fetched->length, maximum_face_backend_bytes)) {
            stats->loaded_faces++;
            stats->retained_encoded_bytes += destination->data_length;
            *face_loaded = true;
        } else {
            stats->failed++;
            stats->unsupported++;
        }
        fetch_result_free(fetched);
        font_loader_update_elapsed(loader, stats);
        return true;
    }

    while (loader->next_source < stats->sources_discovered) {
        if (resource_now_ms() >= loader->deadline_ms) {
            font_loader_expire(loader, scheduler, stats);
            return true;
        }
        size_t index = loader->next_source;
        StylesheetWebFontSource source = {0};
        FontFace *destination = NULL;
        const char *referrer_source = NULL;
        const char *source_referrer_policy = NULL;
        char resolved[4096];
        if (!font_loader_resolve_source(
                sheet, index, document_base_url, document_url,
                referrer_policy, &source, &destination, &referrer_source,
                &source_referrer_policy, resolved)) {
            stats->failed++;
            loader->next_source++;
            continue;
        }
        if (!tilefinch_csp_allows_request(
                csp, TILEFINCH_DESTINATION_FONT, resolved)) {
            stats->failed++;
            loader->next_source++;
            continue;
        }
        if (destination->loaded) {
            stats->duplicate_sources++;
            loader->next_source++;
            continue;
        }
        if (stats->attempted >= maximum_attempts
            || stats->encoded_bytes >= maximum_total_encoded_bytes) {
            stats->skipped_limit++;
            loader->next_source++;
            continue;
        }
        size_t remaining_bytes = maximum_total_encoded_bytes
                                 - stats->encoded_bytes;
        size_t maximum = maximum_single_encoded_bytes < remaining_bytes
                         ? maximum_single_encoded_bytes : remaining_bytes;
        if (maximum == 0) {
            stats->skipped_limit++;
            loader->next_source++;
            continue;
        }
        /* A full scheduler reports both capacity pressure and policy refusal
           as a zero request id.  Settle blocker refusals here so the font
           continuation does not mistake a permanent decision for transient
           backpressure and retry it until the stage deadline. */
        if (session != NULL
            && content_blocker_would_block(
                   session->content_blocker, resolved, document_url,
                   "font", "cors")) {
            (void) content_blocker_should_block(
                session->content_blocker, resolved, document_url,
                "font", "cors");
            stats->attempted++;
            stats->failed++;
            loader->next_source++;
            continue;
        }
        /* The shared HTTP cache is intentionally not consulted here.  Its
           generic entries do not yet retain the request origin, CORS result,
           credentials mode, and redirect-origin taint needed to prove that a
           cached representation is reusable by this font request.  In
           particular, a same-origin cache key may hold a no-CORS response
           reached through a cross-origin redirect.  Page-local decoded faces
           remain retained by the stylesheet, so repeated glyph use is still
           allocation- and network-free. */
        TilefinchRequestContext request_context = {
            .target_url = resolved,
            .initiator_url = document_url,
            .top_level_url = document_url,
            .method = "GET",
            .mode = TILEFINCH_REQUEST_MODE_CORS,
            .credentials = TILEFINCH_CREDENTIALS_SAME_ORIGIN,
            .destination = TILEFINCH_DESTINATION_FONT
        };
        FetchRequest transport = {
            .allow_http_errors = true,
            .accept = "font/woff,application/font-woff;q=0.9,*/*;q=0.1",
        };
        FetchPreparedPageRequest *prepared = budget_malloc_category(
            budget, BUDGET_CATEGORY_RESOURCE, sizeof(*prepared));
        if (prepared == NULL) {
            font_loader_update_elapsed(loader, stats);
            return true;
        }
        if (!fetch_prepare_page_request_context(
                &request_context, referrer_source, source_referrer_policy,
                session, csp, NULL, &transport, prepared, NULL)) {
            budget_free(budget, prepared);
            stats->failed++;
            loader->next_source++;
            continue;
        }
        const FetchRequest *request = fetch_prepared_page_request(prepared);
        double remaining_ms = loader->deadline_ms - resource_now_ms();
        long request_timeout = remaining_ms < 1.0
            ? 1 : (long) remaining_ms;
        uint64_t request_id = fetch_scheduler_enqueue(
            scheduler, resolved, request, maximum, request_timeout);
        budget_free(budget, prepared);
        if (request_id == 0) {
            /* A shared page scheduler can be temporarily full. Preserve
               source order and retry this source during the next idle slice. */
            font_loader_update_elapsed(loader, stats);
            return true;
        }
        stats->attempted++;
        loader->pending_source = index;
        loader->request_id = request_id;
        font_loader_update_elapsed(loader, stats);
        return true;
    }
    loader->active = false;
    font_loader_update_elapsed(loader, stats);
    return true;
}

bool fonts_load_external(Stylesheet *sheet, Budget *budget,
                         const char *document_base_url,
                         const char *document_url,
                         const char *referrer_policy,
                         size_t maximum_attempts,
                         size_t maximum_total_encoded_bytes,
                         size_t maximum_single_encoded_bytes,
                         size_t maximum_face_backend_bytes,
                         long timeout_ms, FetchScheduler *scheduler,
                         BrowserSession *session,
                         const TilefinchContentSecurityPolicy *csp,
                         ExternalFontStats *stats)
{
    if (timeout_ms <= 0 || stats == NULL || budget == NULL) return false;
    bool owns_scheduler = scheduler == NULL;
    if (owns_scheduler) {
        scheduler = fetch_scheduler_create(
            budget, 1, maximum_single_encoded_bytes);
        if (scheduler == NULL) return false;
    }
    if (!font_loader_arguments_valid(
            sheet, budget, document_base_url, document_url,
            maximum_attempts, maximum_total_encoded_bytes,
            maximum_single_encoded_bytes, maximum_face_backend_bytes,
            scheduler, stats)) {
        if (owns_scheduler) fetch_scheduler_destroy(scheduler);
        return false;
    }
    ExternalFontLoader loader = {0};
    bool ok = fonts_external_loader_begin(
        &loader, sheet, timeout_ms, stats);
    while (ok && fonts_external_loader_pending(&loader)) {
        bool face_loaded = false;
        ok = fonts_external_loader_step(
            &loader, sheet, budget, document_base_url, document_url,
            referrer_policy, maximum_attempts,
            maximum_total_encoded_bytes, maximum_single_encoded_bytes,
            maximum_face_backend_bytes, scheduler, session, csp, stats, 10,
            &face_loaded);
    }
    if (!ok) {
        fonts_external_loader_cancel(&loader, scheduler, stats);
    }
    if (owns_scheduler) fetch_scheduler_destroy(scheduler);
    return ok;
}

void stylesheet_document_resources_destroy(
    StylesheetDocumentResources *resources)
{
    if (resources == NULL) return;
    for (size_t i = 0; i < resources->count; i++) {
        browser_shared_body_release(resources->items[i].body);
        budget_free(resources->budget, resources->items[i].response_url);
        budget_free(resources->budget, resources->items[i].url);
    }
    memset(resources, 0, sizeof(*resources));
}

bool stylesheet_document_resources_copy_for_rebuild(
    StylesheetDocumentResources *destination,
    const StylesheetDocumentResources *source)
{
    if (destination == NULL || source == NULL || destination == source
        || destination->budget != NULL || destination->count != 0u
        || source->count > STYLESHEET_DOCUMENT_RESOURCE_LIMIT
        || (source->count != 0u && source->budget == NULL)) return false;
    *destination = *source;
    destination->count = 0u;
    for (size_t i = 0; i < source->count; i++) {
        const StylesheetDocumentResource *original = &source->items[i];
        StylesheetDocumentResource *copy = &destination->items[i];
        *copy = *original;
        copy->url = NULL;
        copy->response_url = NULL;
        copy->body = NULL;
        copy->rules_applied = false;
        destination->count++;
        if (original->url != NULL) {
            size_t length = strlen(original->url) + 1u;
            copy->url = budget_malloc_category(
                source->budget, BUDGET_CATEGORY_RESOURCE, length);
            if (copy->url == NULL) goto refused;
            memcpy(copy->url, original->url, length);
        }
        if (original->response_url != NULL) {
            size_t length = strlen(original->response_url) + 1u;
            copy->response_url = budget_malloc_category(
                source->budget, BUDGET_CATEGORY_RESOURCE, length);
            if (copy->response_url == NULL) goto refused;
            memcpy(copy->response_url, original->response_url, length);
        }
        copy->body = browser_shared_body_retain(original->body);
    }
    return true;

refused:
    stylesheet_document_resources_destroy(destination);
    return false;
}

bool stylesheet_document_resources_retain(
    StylesheetDocumentResources *resources, const char *request_url,
    const char *response_url, const char *response_referrer_policy,
    BrowserSharedBody *body, size_t length, bool cors_validated,
    TilefinchCredentialsMode credentials,
    const TilefinchResourceGrant *resource_grant)
{
    if (resources == NULL || resources->budget == NULL
        || request_url == NULL || response_url == NULL
        || response_referrer_policy == NULL
        || request_url[0] == '\0' || response_url[0] == '\0'
        || (body == NULL && length != 0)
        || !stylesheet_referrer_policy_valid(response_referrer_policy)) {
        return false;
    }
    ResourceContext context = {
        .budget = resources->budget,
        .document_resources = resources
    };
    StylesheetResponseProvenance provenance = {
        .response_url = response_url,
        .known = true
    };
    snprintf(provenance.referrer_policy, sizeof(provenance.referrer_policy),
             "%s", response_referrer_policy);
    document_resource_record_loaded(
        &context, request_url, NULL, &provenance, body, length, false,
        cors_validated, credentials, resource_grant);
    StylesheetDocumentResource *entry = document_resource_find(
        resources, request_url);
    return entry != NULL
        && entry->state == STYLESHEET_DOCUMENT_RESOURCE_LOADED
        && entry->response_provenance_known
        && (length == 0 || entry->body != NULL);
}

void stylesheet_document_resources_open_final_retry(
    StylesheetDocumentResources *resources)
{
    if (resources == NULL) return;
    for (size_t i = 0; i < resources->count; i++) {
        StylesheetDocumentResource *entry = &resources->items[i];
        if (entry->state
                != STYLESHEET_DOCUMENT_RESOURCE_TRANSIENT_FAILURE
            || entry->attempts < STYLESHEET_TRANSIENT_ATTEMPT_LIMIT
            || entry->final_retry_granted) {
            continue;
        }
        entry->attempts = STYLESHEET_TRANSIENT_ATTEMPT_LIMIT - 1;
        entry->final_retry_granted = true;
        resources->final_retry_grants++;
    }
}

bool stylesheet_document_resources_prepare_complete_census(
    StylesheetDocumentResources *resources)
{
    if (resources == NULL) return false;
    resources->selector_census_complete = true;
    if (!resources->final_resample_required
        || resources->final_resample_completed) return false;
    for (size_t i = 0; i < resources->count; i++) {
        StylesheetDocumentResource *entry = &resources->items[i];
        if (entry->state == STYLESHEET_DOCUMENT_RESOURCE_LOADED
            && entry->body != NULL && entry->response_provenance_known) {
            entry->rules_applied = false;
        }
    }
    resources->final_resample_required = false;
    resources->final_resample_completed = true;
    return true;
}

bool stylesheet_document_resources_note_dynamic_tokens(
    StylesheetDocumentResources *resources, lxb_dom_node_t *root)
{
    if (resources == NULL || root == NULL
        || !resources->sampled_large_source
        || resources->dynamic_selector_resamples >= 8u) return false;
    uint32_t tokens[STYLESHEET_SELECTOR_TOKEN_BLOOM_WORDS];
    bool complete = stylesheet_collect_selector_tokens(
        root, tokens, STYLESHEET_SELECTOR_TOKEN_BLOOM_WORDS, 512u, false);
    /* A bounded partial census is still useful: absence is never used here,
       only the positively observed tokens. */
    (void) complete;
    bool changed = false;
    for (size_t i = 0; i < STYLESHEET_SELECTOR_TOKEN_BLOOM_WORDS; i++) {
        changed |= (tokens[i] & ~resources->sampled_selector_tokens[i]) != 0;
        resources->sampled_selector_tokens[i] |= tokens[i];
    }
    if (changed) resources->dynamic_selector_resamples++;
    return changed;
}

bool stylesheet_document_resources_link_applied(
    const StylesheetDocumentResources *resources, const char *base_url,
    const char *href, size_t href_length)
{
    if (resources == NULL || href == NULL || base_url == NULL
        || href_length >= 2048) return false;
    char reference[2048];
    memcpy(reference, href, href_length);
    reference[href_length] = '\0';
    char resolved[4096];
    if (!fetch_resolve_url(base_url, reference, resolved,
                           sizeof(resolved))) return false;
    for (size_t i = 0; i < resources->count; i++) {
        const StylesheetDocumentResource *entry = &resources->items[i];
        if (entry->url != NULL && strcmp(entry->url, resolved) == 0) {
            return entry->rules_applied;
        }
    }
    return false;
}

StylesheetDocumentResourceState stylesheet_document_resources_link_state(
    const StylesheetDocumentResources *resources, const char *base_url,
    const char *href, size_t href_length)
{
    if (href == NULL) return STYLESHEET_DOCUMENT_RESOURCE_EMPTY;
    if (base_url == NULL || href_length >= 2048) {
        return STYLESHEET_DOCUMENT_RESOURCE_TERMINAL_FAILURE;
    }
    char reference[2048];
    memcpy(reference, href, href_length);
    reference[href_length] = '\0';
    char resolved[4096];
    if (!fetch_resolve_url(base_url, reference, resolved,
                           sizeof(resolved))) {
        return STYLESHEET_DOCUMENT_RESOURCE_TERMINAL_FAILURE;
    }
    if (resources == NULL) return STYLESHEET_DOCUMENT_RESOURCE_EMPTY;
    for (size_t i = 0; i < resources->count; i++) {
        const StylesheetDocumentResource *entry = &resources->items[i];
        if (entry->url != NULL && strcmp(entry->url, resolved) == 0) {
            return entry->state;
        }
    }
    return STYLESHEET_DOCUMENT_RESOURCE_EMPTY;
}
