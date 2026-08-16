#include "tilefinch/session.h"
#include "tilefinch/fetch.h"
#include "tilefinch/url.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "SESSION CHECK failed at %s:%d: %s\n",             \
                __FILE__, __LINE__, #condition);                             \
        return 1;                                                            \
    }                                                                        \
} while (0)

static bool contains_cookie(const char *header, const char *name)
{
    size_t length = strlen(name);
    const char *at = header;
    while ((at = strstr(at, name)) != NULL) {
        bool start = at == header
            || (at - header >= 2 && at[-1] == ' ' && at[-2] == ';');
        if (start && at[length] == '=') return true;
        at += length;
    }
    return false;
}

static int test_security_site_state(Budget *budget)
{
    size_t baseline = budget->current;
    BrowserSession session;
    char output[TILEFINCH_URL_SERIALIZED_LIMIT];
    CHECK(browser_session_init(&session, budget, 32u * 1024u));
    CHECK(!browser_session_mixed_content_site_allowed(
              &session, "https://page.test/")
          && browser_session_set_mixed_content_site_allowed(
              &session, "https://page.test/path", true)
          && browser_session_mixed_content_site_allowed(
              &session, "https://sub.page.test/other")
          && browser_session_set_mixed_content_site_allowed(
              &session, "https://page.test/", false)
          && !browser_session_mixed_content_site_allowed(
              &session, "https://page.test/"));

    static const char hsts[] =
        "strict-transport-security: max-age=3600; includeSubDomains\n";
    CHECK(browser_session_hsts_observe(
              &session, "https://secure.test/", hsts, sizeof(hsts) - 1u)
          && browser_session_hsts_upgrade_url(
              &session, "http://secure.test/a", output, sizeof(output))
          && strcmp(output, "https://secure.test/a") == 0
          && browser_session_hsts_upgrade_url(
              &session, "http://sub.secure.test/b", output, sizeof(output))
          && strcmp(output, "https://sub.secure.test/b") == 0);
    static const char remove[] =
        "strict-transport-security: max-age=0\n";
    CHECK(browser_session_hsts_observe(
              &session, "https://unrelated.test/", remove,
              sizeof(remove) - 1u)
          && browser_session_hsts_upgrade_url(
              &session, "http://secure.test/still-retained",
              output, sizeof(output))
          && strcmp(output,
                    "https://secure.test/still-retained") == 0);
    CHECK(browser_session_hsts_observe(
              &session, "https://secure.test/", remove,
              sizeof(remove) - 1u)
          && browser_session_hsts_upgrade_url(
              &session, "http://secure.test/a", output, sizeof(output))
          && strcmp(output, "http://secure.test/a") == 0);
    CHECK(!browser_session_hsts_observe(
        &session, "http://insecure.test/", hsts, sizeof(hsts) - 1u));

    /* RFC 6797 permits a quoted directive-value. The first STS field is
       authoritative; later fields must neither revoke nor repair it. */
    static const char quoted[] =
        "strict-transport-security: ; max-age=\"3600\"; "
        "includeSubDomains;\n";
    CHECK(browser_session_hsts_observe(
              &session, "https://quoted.test/", quoted,
              sizeof(quoted) - 1u)
          && browser_session_hsts_upgrade_url(
              &session, "http://child.quoted.test/a", output,
              sizeof(output))
          && strcmp(output, "https://child.quoted.test/a") == 0);
    static const char first_field_wins[] =
        "strict-transport-security: max-age=3600\n"
        "strict-transport-security: max-age=0\n";
    CHECK(browser_session_hsts_observe(
              &session, "https://first.test/", first_field_wins,
              sizeof(first_field_wins) - 1u)
          && browser_session_hsts_upgrade_url(
              &session, "http://first.test/a", output, sizeof(output))
          && strcmp(output, "https://first.test/a") == 0);

    /* Duplicate known directives and a valued includeSubDomains violate the
       field grammar. They must not create or modify state. */
    static const char duplicate_max_age[] =
        "strict-transport-security: max-age=3600; max-age=0\n";
    static const char valued_subdomains[] =
        "strict-transport-security: max-age=3600; includeSubDomains=yes\n";
    static const char duplicate_unknown[] =
        "strict-transport-security: max-age=3600; future=one; future=two\n";
    CHECK(!browser_session_hsts_observe(
              &session, "https://duplicate.test/", duplicate_max_age,
              sizeof(duplicate_max_age) - 1u)
          && browser_session_hsts_upgrade_url(
              &session, "http://duplicate.test/a", output, sizeof(output))
          && strcmp(output, "http://duplicate.test/a") == 0
          && !browser_session_hsts_observe(
              &session, "https://valued.test/", valued_subdomains,
              sizeof(valued_subdomains) - 1u)
          && browser_session_hsts_upgrade_url(
              &session, "http://valued.test/a", output, sizeof(output))
          && strcmp(output, "http://valued.test/a") == 0
          && !browser_session_hsts_observe(
              &session, "https://unknown.test/", duplicate_unknown,
              sizeof(duplicate_unknown) - 1u)
          && browser_session_hsts_upgrade_url(
              &session, "http://unknown.test/a", output, sizeof(output))
          && strcmp(output, "http://unknown.test/a") == 0);
    CHECK(browser_session_set_mixed_content_site_allowed(
        &session, "https://restart.test/", true));
    browser_session_destroy(&session);
    /* A new browser session is the restart boundary: compatibility authority
       must not leak across it even though unrelated profile data persists. */
    CHECK(browser_session_init(&session, budget, 32u * 1024u)
          && !browser_session_mixed_content_site_allowed(
                 &session, "https://restart.test/"));
    browser_session_destroy(&session);
    CHECK(budget->current == baseline);
    return 0;
}

static int test_redacted_cookie_seed(Budget *budget)
{
    size_t baseline = budget->current;
    BrowserCookieSeedEntry seed[] = {
        {
            .domain = "target.test", .path = "/",
            .name = "cf_clearance", .value_length = 12,
            .host_only = true, .secure = true, .http_only = true,
            .same_site = BROWSER_COOKIE_SAME_SITE_NONE,
            .creation_sequence = 4
        },
        {
            .domain = "target.test", .path = "/nested",
            .name = "empty", .value_length = 0,
            .secure = true, .same_site = BROWSER_COOKIE_SAME_SITE_NONE,
            .partitioned = true,
            .partition_key = "https://top-a.test",
            .creation_sequence = 9
        }
    };
    BrowserSession seeded;
    char header[256];
    TilefinchRequestContext context = {
        .target_url = "https://target.test/nested/page",
        .initiator_url = "https://top-a.test/start",
        .top_level_url = "https://top-a.test/start",
        .method = "GET", .mode = TILEFINCH_REQUEST_MODE_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_FETCH
    };
    CHECK(browser_session_init(&seeded, budget, 32u * 1024u)
          && browser_session_cookie_import_redacted_seed(
                 &seeded, seed, sizeof(seed) / sizeof(seed[0]))
          && seeded.cookie_clock == 9 && seeded.cookie_bytes == 12
          && seeded.cookies[0].expires_at == 0
          && seeded.cookies[0].value != NULL
          && strcmp(seeded.cookies[0].value, "xxxxxxxxxxxx") == 0
          && seeded.cookies[1].value != NULL
          && strcmp(seeded.cookies[1].value, "") == 0
          && browser_session_cookie_header_context(
                 &seeded, &context, header, sizeof(header))
          && strcmp(header, "empty=") == 0
          && browser_session_set_third_party_cookie_site_allowed(
                 &seeded, context.top_level_url, true)
          && browser_session_cookie_header_context(
                 &seeded, &context, header, sizeof(header))
          && strcmp(header, "empty=; cf_clearance=xxxxxxxxxxxx") == 0);
    /* Import is an empty-jar operation, including for a zero-entry seed. */
    CHECK(!browser_session_cookie_import_redacted_seed(&seeded, NULL, 0));
    browser_session_destroy(&seeded);
    CHECK(budget->current == baseline);

    BrowserSession empty;
    CHECK(browser_session_init(&empty, budget, 32u * 1024u)
          && browser_session_cookie_import_redacted_seed(&empty, NULL, 0)
          && empty.cookie_bytes == 0 && empty.cookie_clock == 0);
    browser_session_destroy(&empty);
    CHECK(budget->current == baseline);

    BrowserSession transactional;
    CHECK(browser_session_init(&transactional, budget, 32u * 1024u));
    size_t initialized = budget->current;
    budget_inject_failure_after(budget, 1);
    CHECK(!browser_session_cookie_import_redacted_seed(
        &transactional, seed, sizeof(seed) / sizeof(seed[0])));
    budget_clear_failure_injection(budget);
    CHECK(transactional.cookie_bytes == 0
          && transactional.cookie_clock == 0
          && budget->current == initialized);
    for (size_t i = 0; i < BROWSER_COOKIE_ENTRIES; i++) {
        CHECK(transactional.cookies[i].value == NULL);
    }
    BrowserCookieSeedEntry malformed[2];
    memcpy(malformed, seed, sizeof(malformed));
    malformed[1].creation_sequence = malformed[0].creation_sequence;
    CHECK(!browser_session_cookie_import_redacted_seed(
              &transactional, malformed, 2)
          && transactional.cookie_bytes == 0
          && transactional.cookie_clock == 0);
    memcpy(malformed, seed, sizeof(malformed));
    memset(malformed[0].domain, 'a', sizeof(malformed[0].domain));
    CHECK(!browser_session_cookie_import_redacted_seed(
              &transactional, malformed, 2)
          && transactional.cookie_bytes == 0
          && transactional.cookie_clock == 0);
    memcpy(malformed, seed, sizeof(malformed));
    malformed[1].creation_sequence = SIZE_MAX;
    CHECK(!browser_session_cookie_import_redacted_seed(
              &transactional, malformed, 2)
          && transactional.cookie_bytes == 0
          && transactional.cookie_clock == 0);
    browser_session_destroy(&transactional);
    CHECK(budget->current == baseline);

    BrowserCookieSeedEntry rollover_seed[2];
    memcpy(rollover_seed, seed, sizeof(rollover_seed));
    rollover_seed[0].creation_sequence = 1;
    rollover_seed[1].creation_sequence = SIZE_MAX - 1;
    BrowserSession rollover;
    CHECK(browser_session_init(&rollover, budget, 32u * 1024u)
          && browser_session_cookie_import_redacted_seed(
                 &rollover, rollover_seed, 2)
          && browser_session_cookie_set_http(
                 &rollover, "https://target.test/", "new-a=1; Path=/")
          && browser_session_cookie_set_http(
                 &rollover, "https://target.test/", "new-b=2; Path=/")
          && rollover.cookie_clock == 4);
    bool sequences[5] = {false};
    for (size_t i = 0; i < BROWSER_COOKIE_ENTRIES; i++) {
        if (rollover.cookies[i].value == NULL) continue;
        size_t sequence = rollover.cookies[i].creation_sequence;
        CHECK(sequence >= 1 && sequence <= 4 && !sequences[sequence]);
        sequences[sequence] = true;
    }
    CHECK(sequences[1] && sequences[2] && sequences[3] && sequences[4]);
    browser_session_destroy(&rollover);
    CHECK(budget->current == baseline);
    return 0;
}

static int test_module_cache_provenance(void)
{
    Budget budget;
    budget_init(&budget, 256u * 1024u);
    BrowserSession session;
    CHECK(browser_session_init(&session, &budget, 16u * 1024u));
    static const unsigned char source[] = "export const value = 1;";
    const char *request_url = "https://cdn.test/root.js";
    const char *origin = "https://document.test";
    CHECK(browser_session_cache_put_http(
        &session, request_url, source, sizeof(source) - 1,
        "\"classic\"", NULL, "text/javascript", "max-age=60", NULL, 1));
    CHECK(browser_session_cache_match_module(
        &session, request_url, origin, origin, false,
        TILEFINCH_CREDENTIALS_SAME_ORIGIN,
        2, NULL) == BROWSER_CACHE_MISS);

    char effective_url[] = "https://cdn.test/final/root.js";
    char initiator_origin[] = "https://document.test";
    BrowserModuleCacheProvenance provenance = {
        .effective_url = effective_url,
        .initiator_origin = initiator_origin,
        .top_level_url = "https://document.test/page",
        .response_referrer_policy = "origin",
        .credentials = TILEFINCH_CREDENTIALS_SAME_ORIGIN,
        .cors_validated = true,
        .javascript_mime_validated = true,
        .referrer_policy_header_present = true
    };
    CHECK(browser_session_cache_put_http_module(
        &session, request_url, source, sizeof(source) - 1,
        "\"module\"", NULL, "Text/JavaScript; charset=utf-8",
        "max-age=60", " Origin, ACCEPT-ENCODING ", 10, &provenance));
    effective_url[8] = 'X';
    initiator_origin[8] = 'X';
    const BrowserCacheEntry *entry = NULL;
    CHECK(browser_session_cache_match_module(
        &session, request_url, origin, "https://document.test/page", false,
        TILEFINCH_CREDENTIALS_SAME_ORIGIN,
        11, &entry) == BROWSER_CACHE_FRESH
          && entry != NULL
          && strcmp(entry->module_effective_url,
                    "https://cdn.test/final/root.js") == 0
          && strcmp(entry->module_initiator_origin, origin) == 0
          && strcmp(entry->module_response_referrer_policy, "origin") == 0
          && !entry->module_cors_redirect_origin_tainted);
    CHECK(browser_session_cache_match_module(
        &session, request_url, origin, "https://other-top.test/page", false,
        TILEFINCH_CREDENTIALS_SAME_ORIGIN, 11, NULL)
        == BROWSER_CACHE_MISS);
    CHECK(browser_session_cache_match_module(
        &session, request_url, "null", "https://document.test/page", true,
        TILEFINCH_CREDENTIALS_SAME_ORIGIN, 11, NULL)
        == BROWSER_CACHE_MISS);
    BrowserModuleCacheProvenance opaque_provenance = provenance;
    opaque_provenance.initiator_origin = "null";
    opaque_provenance.initiator_opaque = true;
    CHECK(!browser_session_cache_put_http_module(
        &session, "https://cdn.test/opaque.js", source,
        sizeof(source) - 1, NULL, NULL, "text/javascript",
        "max-age=60", NULL, 11, &opaque_provenance));
    CHECK(browser_session_cache_match_module(
        &session, request_url, "https://other.test",
        "https://document.test/page", false,
        TILEFINCH_CREDENTIALS_SAME_ORIGIN, 11, NULL) == BROWSER_CACHE_MISS);
    CHECK(browser_session_cache_match_module(
        &session, request_url, origin, "https://document.test/page", false,
        TILEFINCH_CREDENTIALS_INCLUDE,
        11, NULL) == BROWSER_CACHE_MISS);

    BrowserModuleCacheProvenance revalidated = {
        .effective_url = "https://cdn.test/new-final/root.js",
        .initiator_origin = origin,
        .top_level_url = "https://document.test/page",
        .response_referrer_policy = "no-referrer",
        .credentials = TILEFINCH_CREDENTIALS_SAME_ORIGIN,
        .cors_validated = true,
        .cors_redirect_origin_tainted = true,
        .javascript_mime_validated = true,
        .referrer_policy_header_present = false
    };
    CHECK(browser_session_cache_revalidate_module(
        &session, request_url, "max-age=120",
        "accept-encoding, ORIGIN", 20, &revalidated));
    CHECK(browser_session_cache_match_module(
        &session, request_url, origin, "https://document.test/page", false,
        TILEFINCH_CREDENTIALS_SAME_ORIGIN,
        21, &entry) == BROWSER_CACHE_FRESH
          && entry != NULL
          && strcmp(entry->module_effective_url,
                    revalidated.effective_url) == 0
          && strcmp(entry->module_response_referrer_policy, "origin") == 0
          && entry->module_cors_redirect_origin_tainted);

    revalidated.referrer_policy_header_present = true;
    CHECK(browser_session_cache_revalidate_module(
        &session, request_url, NULL, NULL, 22, &revalidated));
    CHECK(browser_session_cache_match_module(
        &session, request_url, origin, "https://document.test/page", false,
        TILEFINCH_CREDENTIALS_SAME_ORIGIN,
        23, &entry) == BROWSER_CACHE_FRESH
          && strcmp(entry->module_response_referrer_policy,
                    "no-referrer") == 0);
    revalidated.response_referrer_policy = "";
    CHECK(browser_session_cache_revalidate_module(
        &session, request_url, NULL, NULL, 24, &revalidated));
    CHECK(browser_session_cache_match_module(
        &session, request_url, origin, "https://document.test/page", false,
        TILEFINCH_CREDENTIALS_SAME_ORIGIN,
        25, &entry) == BROWSER_CACHE_FRESH
          && entry->module_response_referrer_policy[0] == '\0');
    revalidated.response_referrer_policy = "invalid-policy";
    CHECK(!browser_session_cache_revalidate_module(
        &session, request_url, NULL, NULL, 26, &revalidated));

    CHECK(browser_session_cache_revalidate(
        &session, request_url, "max-age=120", NULL, 30));
    CHECK(browser_session_cache_match_module(
        &session, request_url, origin, "https://document.test/page", false,
        TILEFINCH_CREDENTIALS_SAME_ORIGIN,
        31, NULL) == BROWSER_CACHE_MISS);
    CHECK(!browser_session_cache_put_http_module(
        &session, request_url, source, sizeof(source) - 1,
        NULL, NULL, "text/javascript", "max-age=60",
        "Origin, X-Unsupported", 40, &revalidated));

    BrowserModuleCacheProvenance fragment_provenance = {
        .effective_url = "https://cdn.test/final/root.js#response-a",
        .initiator_origin = origin,
        .top_level_url = "https://document.test/page",
        .credentials = TILEFINCH_CREDENTIALS_SAME_ORIGIN,
        .cors_validated = true,
        .javascript_mime_validated = true
    };
    CHECK(browser_session_cache_put_http_module(
        &session, "https://cdn.test/root.js#a", source,
        sizeof(source) - 1, NULL, NULL, "text/javascript",
        "immutable", NULL, 50, &fragment_provenance));
    CHECK(browser_session_cache_match_module(
        &session, "https://cdn.test/root.js#a", origin,
        "https://document.test/page", false,
        TILEFINCH_CREDENTIALS_SAME_ORIGIN, 51,
        &entry) == BROWSER_CACHE_FRESH
          && entry != NULL
          && strcmp(entry->module_effective_url,
                    fragment_provenance.effective_url) == 0);
    CHECK(browser_session_cache_match_module(
        &session, "https://cdn.test/root.js#b", origin,
        "https://document.test/page", false,
        TILEFINCH_CREDENTIALS_SAME_ORIGIN, 51,
        NULL) == BROWSER_CACHE_MISS);
    browser_session_destroy(&session);
    CHECK(budget.current == 0 && budget.external_reserved == 0);
    return 0;
}

static int test_response_cache_provenance(void)
{
    Budget budget;
    budget_init(&budget, 256u * 1024u);
    BrowserSession session;
    CHECK(browser_session_init(&session, &budget, 16u * 1024u));
    static const unsigned char body[] = "stylesheet-body";
    const char *request_url = "https://document.test/assets/site.css";
    CHECK(browser_session_cache_put_http(
        &session, request_url, body, sizeof(body) - 1u,
        "\"style-v1\"", NULL, "text/css", "max-age=60", NULL, 1));

    const BrowserCacheEntry *entry = browser_session_cache_lookup(
        &session, request_url);
    CHECK(entry != NULL && !entry->response_url_known
          && !entry->response_referrer_policy_known
          && entry->response_url == NULL
          && entry->response_referrer_policy[0] == '\0');
    unsigned char *retained_data = entry->data;
    BrowserSharedBody *retained_body = entry->body;

    /* The overwhelmingly common no-redirect case retains exact provenance
       without allocating, even when the next allocation is forced to fail. */
    budget_inject_failure_after(&budget, 0);
    CHECK(browser_session_cache_set_response_provenance(
        &session, request_url, request_url, ""));
    budget_clear_failure_injection(&budget);
    entry = browser_session_cache_lookup(&session, request_url);
    CHECK(entry != NULL && entry->response_url_known
          && entry->response_referrer_policy_known
          && entry->response_url == NULL
          && entry->response_referrer_policy[0] == '\0'
          && entry->data == retained_data && entry->body == retained_body);

    char first_redirect[] =
        "https://static.test/styles/first/site.css";
    CHECK(browser_session_cache_set_response_provenance(
        &session, request_url, first_redirect, "origin"));
    first_redirect[8] = 'X';
    entry = browser_session_cache_lookup(&session, request_url);
    CHECK(entry != NULL && entry->response_url_known
          && entry->response_referrer_policy_known
          && strcmp(entry->response_url,
                    "https://static.test/styles/first/site.css") == 0
          && strcmp(entry->response_referrer_policy, "origin") == 0);

    const char *second_redirect =
        "https://static.test/styles/second/site.css";
    CHECK(browser_session_cache_set_response_provenance(
        &session, request_url, second_redirect, "no-referrer"));
    entry = browser_session_cache_lookup(&session, request_url);
    CHECK(entry != NULL
          && strcmp(entry->response_url, second_redirect) == 0
          && strcmp(entry->response_referrer_policy, "no-referrer") == 0);

    /* Ordinary HTTP revalidation updates freshness but retains the response
       provenance for the combined representation. */
    CHECK(browser_session_cache_revalidate(
        &session, request_url, "max-age=120", NULL, 20));
    entry = browser_session_cache_lookup(&session, request_url);
    CHECK(entry != NULL && entry->response_url_known
          && entry->response_referrer_policy_known
          && strcmp(entry->response_url, second_redirect) == 0
          && strcmp(entry->response_referrer_policy, "no-referrer") == 0
          && entry->data == retained_data && entry->body == retained_body);

    /* The compatibility URL-only API can retain its historical URL evidence,
       but must explicitly make response-policy provenance unknown. */
    CHECK(browser_session_cache_set_response_url(
        &session, request_url, second_redirect));
    entry = browser_session_cache_lookup(&session, request_url);
    CHECK(entry != NULL && entry->response_url_known
          && !entry->response_referrer_policy_known
          && strcmp(entry->response_url, second_redirect) == 0
          && entry->response_referrer_policy[0] == '\0');

    CHECK(browser_session_cache_set_response_provenance(
        &session, request_url, second_redirect, "strict-origin"));
    CHECK(!browser_session_cache_set_response_provenance(
        &session, request_url, second_redirect, "STRICT-ORIGIN"));
    entry = browser_session_cache_lookup(&session, request_url);
    CHECK(entry != NULL && !entry->response_url_known
          && !entry->response_referrer_policy_known
          && entry->response_url == NULL
          && entry->response_referrer_policy[0] == '\0'
          && entry->data == retained_data && entry->body == retained_body);

    CHECK(browser_session_cache_set_response_provenance(
        &session, request_url, second_redirect, "same-origin"));
    CHECK(!browser_session_cache_set_response_provenance(
        &session, request_url, "data:text/css,body{}", "origin"));
    entry = browser_session_cache_lookup(&session, request_url);
    CHECK(entry != NULL && !entry->response_url_known
          && !entry->response_referrer_policy_known
          && entry->response_url == NULL
          && entry->response_referrer_policy[0] == '\0');

    /* A changed redirect requires one URL copy.  If it cannot be made, old
       URL and policy evidence are both discarded while the body survives. */
    CHECK(browser_session_cache_set_response_provenance(
        &session, request_url, second_redirect, "origin"));
    budget_inject_failure_after(&budget, 0);
    CHECK(!browser_session_cache_set_response_provenance(
        &session, request_url,
        "https://static.test/styles/third/site.css", "no-referrer"));
    budget_clear_failure_injection(&budget);
    entry = browser_session_cache_lookup(&session, request_url);
    CHECK(entry != NULL && !entry->response_url_known
          && !entry->response_referrer_policy_known
          && entry->response_url == NULL
          && entry->response_referrer_policy[0] == '\0'
          && entry->data == retained_data && entry->body == retained_body
          && entry->length == sizeof(body) - 1u
          && memcmp(entry->data, body, sizeof(body) - 1u) == 0);

    CHECK(browser_session_cache_set_response_provenance(
        &session, request_url, request_url,
        "strict-origin-when-cross-origin"));
    CHECK(browser_session_cache_clear_response_provenance(
        &session, request_url));
    entry = browser_session_cache_lookup(&session, request_url);
    CHECK(entry != NULL && !entry->response_url_known
          && !entry->response_referrer_policy_known
          && entry->response_url == NULL
          && entry->response_referrer_policy[0] == '\0'
          && !browser_session_cache_clear_response_provenance(
                 &session, "https://missing.test/style.css"));

    browser_session_destroy(&session);
    CHECK(budget.current == 0 && budget.external_reserved == 0);
    return 0;
}

static int test_bounded_site_adapter_state(Budget *budget)
{
    BrowserSession session;
    static const unsigned char payload[] = {
        0x01, 0x42, 0x00, 0x7f
    };
    unsigned char copy[sizeof(payload)] = {0};
    size_t length = 0;
    CHECK(browser_session_init(&session, budget, 16u * 1024u)
          && browser_session_site_adapter_state_put(
              &session, "fixture-adapter", payload, sizeof(payload), 100)
          && browser_session_site_adapter_state_get(
              &session, "fixture-adapter", copy, sizeof(copy), &length,
              125, 50)
          && length == sizeof(payload)
          && memcmp(copy, payload, sizeof(payload)) == 0);
    CHECK(browser_session_cookie_set_http(
              &session, "https://fixture.test/",
              "sid=changed; Path=/; Secure")
          && !browser_session_site_adapter_state_get(
              &session, "fixture-adapter", copy, sizeof(copy), &length,
              126, 50));
    CHECK(browser_session_site_adapter_state_put(
              &session, "fixture-adapter", payload, sizeof(payload), 200)
          && !browser_session_site_adapter_state_get(
              &session, "fixture-adapter", copy, sizeof(copy), &length,
              251, 50));
    browser_session_site_adapter_state_remove(
        &session, "fixture-adapter");
    CHECK(!browser_session_site_adapter_state_get(
        &session, "fixture-adapter", copy, sizeof(copy), &length,
        200, 0));
    browser_session_destroy(&session);
    return 0;
}

/* The HttpOnly and Secure overwrite guards run on the exact-key path only.
   Eviction re-points an entry at a victim chosen by age, so victim selection
   has to apply the same two rules; otherwise a caller that may not overwrite
   a protected cookie directly can simply push it out of the jar and then
   create it fresh.  Both halves below drive the per-domain limit, which is
   the cheaper of the two eviction paths to reach from script. */
static int test_cookie_eviction_guard(Budget *budget)
{
    /* Script (from_http == false) must not retire an HttpOnly cookie. */
    BrowserSession http_only_session;
    char header[512];
    char script_view[512];
    char cookie[96];
    static const char *const http_only_url = "https://evict.test/";
    CHECK(browser_session_init(&http_only_session, budget, 32u * 1024u));
    CHECK(browser_session_cookie_set_http(
        &http_only_session, http_only_url,
        "sid=secret; Path=/; Secure; HttpOnly"));
    /* Enough scripted writes to overrun the per-domain limit several times:
       one slot is already held by the protected cookie. */
    for (size_t i = 0; i < BROWSER_COOKIE_PER_DOMAIN_LIMIT + 4u; i++) {
        int written = snprintf(cookie, sizeof(cookie),
                               "pad%02zu=v; Path=/", i);
        CHECK(written > 0 && (size_t) written < sizeof(cookie));
        CHECK(browser_session_cookie_set(
            &http_only_session, http_only_url, cookie));
    }
    CHECK(browser_session_cookie_header(
        &http_only_session, http_only_url, header, sizeof(header)));
    /* The eviction path really ran: the oldest scripted cookies are gone and
       the newest survive, yet the HttpOnly entry is untouched. */
    CHECK(!contains_cookie(header, "pad00")
          && contains_cookie(header, "pad11")
          && contains_cookie(header, "sid")
          && strstr(header, "sid=secret") != NULL);
    CHECK(browser_session_cookie_get(
              &http_only_session, http_only_url, script_view,
              sizeof(script_view))
          && !contains_cookie(script_view, "sid"));
    /* With the cookie still present the exact-key guard applies again, so
       script cannot forge a replacement value. */
    CHECK(!browser_session_cookie_set(
              &http_only_session, http_only_url,
              "sid=forged; Path=/; Secure")
          && browser_session_cookie_header(
              &http_only_session, http_only_url, header, sizeof(header))
          && strstr(header, "sid=secret") != NULL
          && strstr(header, "forged") == NULL);
    browser_session_destroy(&http_only_session);

    /* An insecure origin must not retire a Secure cookie. */
    BrowserSession secure_session;
    static const char *const secure_url = "https://secure-evict.test/";
    static const char *const insecure_url = "http://secure-evict.test/";
    CHECK(browser_session_init(&secure_session, budget, 32u * 1024u));
    CHECK(browser_session_cookie_set_http(
        &secure_session, secure_url, "sid=locked; Path=/; Secure"));
    for (size_t i = 0; i < BROWSER_COOKIE_PER_DOMAIN_LIMIT + 4u; i++) {
        int written = snprintf(cookie, sizeof(cookie),
                               "pad%02zu=v; Path=/", i);
        CHECK(written > 0 && (size_t) written < sizeof(cookie));
        CHECK(browser_session_cookie_set(
            &secure_session, insecure_url, cookie));
    }
    CHECK(browser_session_cookie_header(
        &secure_session, secure_url, header, sizeof(header)));
    CHECK(!contains_cookie(header, "pad00")
          && contains_cookie(header, "pad11")
          && strstr(header, "sid=locked") != NULL);
    CHECK(!browser_session_cookie_set(
              &secure_session, insecure_url, "sid=forged; Path=/")
          && browser_session_cookie_header(
              &secure_session, secure_url, header, sizeof(header))
          && strstr(header, "sid=locked") != NULL
          && strstr(header, "forged") == NULL);
    browser_session_destroy(&secure_session);
    return 0;
}

static int test_active_mixed_content_block(Budget *budget)
{
    size_t baseline = budget->current;
    FetchRequest request = {
        .method = "GET",
        .sec_fetch_dest = "script",
        .sec_fetch_mode = "no-cors",
        .sec_fetch_site = "cross-site",
        .credentials = FETCH_CREDENTIALS_OMIT,
        .initiator_url = "https://secure.test/page"
    };
    FetchResult result = {0};
    CHECK(!fetch_request_cancelable(
              budget, "http://insecure.test/app.js", &request,
              1024, 1000, NULL, NULL, &result)
          && strcmp(result.error, "blocked active mixed content") == 0);
    fetch_result_destroy(&result);
    request.sec_fetch_dest = "document";
    memset(&result, 0, sizeof(result));
    CHECK(!fetch_request_cancelable(
              budget, "http://insecure.test/forged-document", &request,
              1024, 1000, NULL, NULL, &result)
          && strcmp(result.error, "blocked active mixed content") == 0);
    fetch_result_destroy(&result);
    CHECK(budget->current == baseline);
    return 0;
}

static int test_global_site_data_policy(Budget *budget)
{
    BrowserSession session;
    char cookies[128];
    const char *value = NULL;
    size_t value_length = 0;
    static const char *url = "https://policy.test/page";
    CHECK(browser_session_init(&session, budget, 32u * 1024u)
          && browser_session_site_data_allowed(&session)
          && browser_session_cookie_set_http(
                 &session, url, "sid=retained; Path=/; Secure")
          && browser_session_storage_set(
                 &session, url, true, "local", "kept", 4)
          && browser_session_storage_set(
                 &session, url, false, "session", "kept", 4));
    browser_session_set_site_data_allowed(&session, false);
    BrowserCookieOverlay *blocked_overlay =
        browser_session_cookie_overlay_create(budget, &session);
    TilefinchRequestContext request_context = {
        .target_url = url, .initiator_url = url, .top_level_url = url,
        .method = "GET", .mode = TILEFINCH_REQUEST_MODE_SAME_ORIGIN,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_OTHER
    };
    CHECK(!browser_session_site_data_allowed(&session)
          && browser_session_cookie_header(
                 &session, url, cookies, sizeof(cookies))
          && cookies[0] == '\0'
          && !browser_session_cookie_set_http(
                 &session, url, "blocked=1; Path=/; Secure")
          && browser_session_storage_length(&session, url, true) == 0
          && browser_session_storage_length(&session, url, false) == 0
          && !browser_session_storage_get(
                 &session, url, true, "local", &value, &value_length)
          && !browser_session_storage_set(
                 &session, url, true, "new", "blocked", 7)
          && blocked_overlay != NULL
          && browser_cookie_overlay_header_context(
                 blocked_overlay, &request_context,
                 cookies, sizeof(cookies))
          && cookies[0] == '\0'
          && !browser_cookie_overlay_set_http_context(
                 blocked_overlay, &request_context,
                 "redirect=blocked; Path=/; Secure"));
    browser_cookie_overlay_destroy(blocked_overlay);
    browser_session_set_site_data_allowed(&session, true);
    CHECK(browser_session_cookie_header(
              &session, url, cookies, sizeof(cookies))
          && strcmp(cookies, "sid=retained") == 0
          && browser_session_storage_get(
                 &session, url, true, "local", &value, &value_length)
          && value_length == 4 && memcmp(value, "kept", 4) == 0
          && browser_session_storage_get(
                 &session, url, false, "session", &value, &value_length));
    browser_session_destroy(&session);
    return 0;
}

static int test_typed_resource_grants(void)
{
    TilefinchRequestContext context = {
        .target_url = "https://cdn.example.test/app.js",
        .initiator_url = "https://www.example.test/page",
        .top_level_url = "https://www.example.test/page",
        .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_SCRIPT
    };
    FetchResult result = {0};
    snprintf(result.effective_url, sizeof(result.effective_url), "%s",
             context.target_url);
    TilefinchResourceGrant grant;
    TilefinchResourceDeniedReason denied;
    CHECK(fetch_resource_grant_create(
              &result, &context, false, false, false, &grant, &denied)
          && denied == TILEFINCH_RESOURCE_DENIED_NONE
          && grant.final_same_site && !grant.final_same_origin
          && !grant.nosniff && !grant.mime_validated);

    snprintf(result.response_headers, sizeof(result.response_headers), "%s",
             "x-content-type-options: nosniff\n");
    result.response_headers_length = strlen(result.response_headers);
    CHECK(!fetch_resource_grant_create(
              &result, &context, false, false, false, &grant, &denied)
          && denied == TILEFINCH_RESOURCE_DENIED_MIME);
    CHECK(fetch_resource_grant_create(
              &result, &context, false, true, false, &grant, &denied)
          && grant.nosniff && grant.mime_validated);

    snprintf(result.response_headers, sizeof(result.response_headers), "%s",
             "cross-origin-resource-policy: same-origin\n");
    result.response_headers_length = strlen(result.response_headers);
    CHECK(!fetch_resource_grant_create(
              &result, &context, false, true, false, &grant, &denied)
          && denied == TILEFINCH_RESOURCE_DENIED_CORP);
    snprintf(result.response_headers, sizeof(result.response_headers), "%s",
             "cross-origin-resource-policy: same-site\n");
    result.response_headers_length = strlen(result.response_headers);
    CHECK(fetch_resource_grant_create(
              &result, &context, false, true, false, &grant, &denied)
          && grant.corp == TILEFINCH_CORP_SAME_SITE);

    context.mode = TILEFINCH_REQUEST_MODE_CORS;
    CHECK(fetch_resource_grant_create(
              &result, &context, true, true, false, &grant, &denied)
          && grant.cors_validated);
    context.mode = TILEFINCH_REQUEST_MODE_NO_CORS;
    context.initiator_opaque = true;
    CHECK(!fetch_resource_grant_create(
              &result, &context, false, true, false, &grant, &denied)
          && denied == TILEFINCH_RESOURCE_DENIED_CORP);
    context.initiator_opaque = false;

    snprintf(result.response_headers, sizeof(result.response_headers), "%s",
             "x-content-type-options: nosniff\n"
             "x-content-type-options: nosniff\n");
    result.response_headers_length = strlen(result.response_headers);
    CHECK(!fetch_resource_grant_create(
              &result, &context, false, true, false, &grant, &denied)
          && denied
                 == TILEFINCH_RESOURCE_DENIED_MALFORMED_SECURITY_HEADER);
    result.response_security_headers_truncated = true;
    CHECK(!fetch_resource_grant_create(
              &result, &context, false, true, false, &grant, &denied)
          && denied
                 == TILEFINCH_RESOURCE_DENIED_TRUNCATED_SECURITY_HEADER);

    result.response_security_headers_truncated = false;
    snprintf(result.response_headers, sizeof(result.response_headers), "%s",
             "cross-origin-resource-policy: sometimes\n");
    result.response_headers_length = strlen(result.response_headers);
    CHECK(!fetch_resource_grant_create(
              &result, &context, false, true, false, &grant, &denied)
          && denied
                 == TILEFINCH_RESOURCE_DENIED_MALFORMED_SECURITY_HEADER);
    snprintf(result.response_headers, sizeof(result.response_headers), "%s",
             "x-content-type-options: sniff-if-convenient\n");
    result.response_headers_length = strlen(result.response_headers);
    CHECK(!fetch_resource_grant_create(
              &result, &context, false, true, false, &grant, &denied)
          && denied
                 == TILEFINCH_RESOURCE_DENIED_MALFORMED_SECURITY_HEADER);
    snprintf(result.response_headers, sizeof(result.response_headers),
             "cross-origin-resource-policy: %040d\n", 1);
    result.response_headers_length = strlen(result.response_headers);
    CHECK(!fetch_resource_grant_create(
              &result, &context, false, true, false, &grant, &denied)
          && denied
                 == TILEFINCH_RESOURCE_DENIED_MALFORMED_SECURITY_HEADER);

    TilefinchResourceGrant cached = {
        .destination = TILEFINCH_DESTINATION_SCRIPT,
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .corp = TILEFINCH_CORP_CROSS_ORIGIN,
        .final_same_site = true
    };
    FetchResult revalidated = {.status_code = 304};
    snprintf(revalidated.effective_url, sizeof(revalidated.effective_url),
             "%s", context.target_url);
    CHECK(fetch_resource_grant_revalidate_304(
              &revalidated, &context, &cached, false, true, false,
              &grant, &denied)
          && denied == TILEFINCH_RESOURCE_DENIED_NONE
          && grant.corp == TILEFINCH_CORP_CROSS_ORIGIN);
    snprintf(revalidated.response_headers,
             sizeof(revalidated.response_headers), "%s",
             "cross-origin-resource-policy: same-origin\n");
    revalidated.response_headers_length = strlen(
        revalidated.response_headers);
    CHECK(!fetch_resource_grant_revalidate_304(
              &revalidated, &context, &cached, false, true, false,
              &grant, &denied)
          && denied == TILEFINCH_RESOURCE_DENIED_CORP);
    snprintf(revalidated.response_headers,
             sizeof(revalidated.response_headers), "%s",
             "x-content-type-options: nosniff\n");
    revalidated.response_headers_length = strlen(
        revalidated.response_headers);
    CHECK(!fetch_resource_grant_revalidate_304(
              &revalidated, &context, &cached, false, false, false,
              &grant, &denied)
          && denied == TILEFINCH_RESOURCE_DENIED_MIME);
    revalidated.response_security_headers_truncated = true;
    CHECK(!fetch_resource_grant_revalidate_304(
              &revalidated, &context, &cached, false, true, false,
              &grant, &denied)
          && denied
                 == TILEFINCH_RESOURCE_DENIED_TRUNCATED_SECURITY_HEADER);

    FetchResult cors = {0};
    snprintf(cors.response_headers, sizeof(cors.response_headers), "%s",
             "access-control-allow-origin: https://www.example.test\n");
    cors.response_headers_length = strlen(cors.response_headers);
    CHECK(fetch_cors_response_allows(
              &cors, "https://www.example.test", false));
    cors.response_security_headers_truncated = true;
    CHECK(!fetch_cors_response_allows(
        &cors, "https://www.example.test", false));
    return 0;
}

static int test_resource_cache_partitioning(Budget *budget)
{
    size_t baseline = budget->current;
    BrowserSession session;
    CHECK(browser_session_init(&session, budget, 32u * 1024u));
    static const unsigned char source[] = "globalThis.partitioned=1";
    unsigned char *copy = budget_malloc(
        budget, sizeof(source) - 1u);
    CHECK(copy != NULL);
    memcpy(copy, source, sizeof(source) - 1u);
    BrowserSharedBody *body = browser_shared_body_take(
        budget, copy, sizeof(source) - 1u);
    CHECK(body != NULL);
    TilefinchRequestContext first = {
        .target_url = "https://cdn.test/shared.js",
        .initiator_url = "https://page-a.test/article",
        .top_level_url = "https://page-a.test/article",
        .method = "GET", .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_SCRIPT
    };
    TilefinchRequestContext second = first;
    second.initiator_url = "https://page-b.test/article";
    second.top_level_url = second.initiator_url;
    TilefinchResourceGrant first_grant = {
        .destination = TILEFINCH_DESTINATION_SCRIPT,
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .corp = TILEFINCH_CORP_CROSS_ORIGIN
    };
    CHECK(browser_session_cache_put_http_shared_resource(
        &session, first.target_url, body, NULL, NULL,
        "application/javascript", "max-age=60", NULL, 1,
        &first, &first_grant));
    const BrowserCacheEntry *matched = NULL;
    CHECK(browser_session_cache_match_resource(
              &session, first.target_url, &first, 2, &matched)
              == BROWSER_CACHE_FRESH
          && matched != NULL
          && browser_session_cache_match_resource(
              &session, second.target_url, &second, 2, &matched)
              == BROWSER_CACHE_MISS
          && matched == NULL);
    TilefinchResourceGrant second_grant = first_grant;
    CHECK(browser_session_cache_put_http_shared_resource(
              &session, second.target_url, body, NULL, NULL,
              "application/javascript", "max-age=60", NULL, 2,
              &second, &second_grant)
          && browser_session_cache_match_resource(
              &session, first.target_url, &first, 3, &matched)
              == BROWSER_CACHE_FRESH
          && browser_session_cache_match_resource(
              &session, second.target_url, &second, 3, &matched)
              == BROWSER_CACHE_FRESH);
    TilefinchRequestContext opaque = first;
    opaque.target_url = "https://cdn.test/opaque.js";
    opaque.initiator_url = "https://page-a.test/sandboxed-frame";
    opaque.initiator_opaque = true;
    TilefinchResourceGrant opaque_grant = first_grant;
    opaque_grant.initiator_opaque = true;
    CHECK(!browser_session_cache_put_http_shared_resource(
              &session, opaque.target_url, body, NULL, NULL,
              "application/javascript", "max-age=60", NULL, 3,
              &opaque, &opaque_grant)
          && browser_session_cache_match_resource(
                 &session, opaque.target_url, &opaque, 4, &matched)
                 == BROWSER_CACHE_MISS
          && matched == NULL);
    browser_shared_body_release(body);
    browser_session_destroy(&session);
    CHECK(budget->current == baseline);
    return 0;
}

static int test_stylesheet_cache_signature_tracks_ram_authority(Budget *budget)
{
    size_t baseline = budget->current;
    BrowserSession session;
    CHECK(browser_session_init(&session, budget, 32u * 1024u));
    uint64_t empty = browser_session_stylesheet_cache_signature(&session, 1);
    static const unsigned char generic[] = "not a stylesheet";
    CHECK(browser_session_cache_put_http(
              &session, "https://page.test/data", generic,
              sizeof(generic) - 1u, NULL, NULL, "text/plain",
              "max-age=60", NULL, 1)
          && browser_session_stylesheet_cache_signature(&session, 1)
                 == empty);

    TilefinchRequestContext context = {
        .target_url = "https://cdn.test/site.css",
        .initiator_url = "https://page.test/article",
        .top_level_url = "https://page.test/article",
        .method = "GET", .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_STYLE
    };
    TilefinchResourceGrant grant = {
        .destination = TILEFINCH_DESTINATION_STYLE,
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .corp = TILEFINCH_CORP_CROSS_ORIGIN
    };
    static const unsigned char first_css[] = "body{color:#123}";
    static const unsigned char second_css[] = "body{color:#456}";
    unsigned char *first_copy = budget_malloc(
        budget, sizeof(first_css) - 1u);
    unsigned char *second_copy = budget_malloc(
        budget, sizeof(second_css) - 1u);
    CHECK(first_copy != NULL && second_copy != NULL);
    memcpy(first_copy, first_css, sizeof(first_css) - 1u);
    memcpy(second_copy, second_css, sizeof(second_css) - 1u);
    BrowserSharedBody *first_body = browser_shared_body_take(
        budget, first_copy, sizeof(first_css) - 1u);
    BrowserSharedBody *second_body = browser_shared_body_take(
        budget, second_copy, sizeof(second_css) - 1u);
    CHECK(first_body != NULL && second_body != NULL);
    CHECK(browser_session_cache_put_http_shared_resource(
        &session, context.target_url, first_body, NULL, NULL, "text/css",
        "max-age=60", NULL, 2, &context, &grant));
    uint64_t first_signature =
        browser_session_stylesheet_cache_signature(&session, 2);
    CHECK(first_signature != empty);
    CHECK(browser_session_cache_put_http_shared_resource(
        &session, context.target_url, second_body, NULL, NULL, "text/css",
        "max-age=60", NULL, 3, &context, &grant));
    uint64_t second_signature =
        browser_session_stylesheet_cache_signature(&session, 3);
    CHECK(second_signature != empty && second_signature != first_signature);
    CHECK(browser_session_stylesheet_cache_signature(&session, UINT64_MAX)
          == 0);
    browser_shared_body_release(first_body);
    browser_shared_body_release(second_body);
    browser_session_destroy(&session);
    CHECK(budget->current == baseline);
    return 0;
}

static int test_typed_response_security_and_request_authority(Budget *budget)
{
    FetchResponseSecurityMetadata metadata;
    fetch_response_security_metadata_reset(&metadata);
    CHECK(fetch_response_security_metadata_collect(
              &metadata, "access-control-allow-origin", 27,
              "https://page.test", 17, false)
          && fetch_response_security_metadata_collect(
              &metadata, "access-control-allow-credentials", 32,
              "true", 4, false)
          && fetch_response_security_metadata_collect(
              &metadata, "cross-origin-resource-policy", 28,
              "same-origin", 11, false)
          && fetch_response_security_metadata_collect(
              &metadata, "x-content-type-options", 22,
              "nosniff", 7, false)
          && fetch_response_security_metadata_collect(
              &metadata, "strict-transport-security", 25,
              "max-age=3600; includeSubDomains", 31, false));
    CHECK(metadata.allow_origin_state == FETCH_SECURITY_FIELD_VALID
          && strcmp(metadata.allow_origin, "https://page.test") == 0
          && metadata.allow_credentials_state == FETCH_SECURITY_FIELD_VALID
          && metadata.allow_credentials
          && metadata.corp_state == FETCH_SECURITY_FIELD_VALID
          && metadata.corp == TILEFINCH_CORP_SAME_ORIGIN
          && metadata.nosniff_state == FETCH_SECURITY_FIELD_VALID
          && metadata.nosniff
          && metadata.hsts_state == FETCH_SECURITY_FIELD_VALID
          && metadata.hsts_max_age == 3600
          && metadata.hsts_include_subdomains);
    CHECK(fetch_response_security_metadata_collect(
              &metadata, "access-control-allow-origin", 27,
              "https://other.test", 18, true)
          && metadata.allow_origin_state == FETCH_SECURITY_FIELD_DUPLICATE
          && metadata.allow_origin[0] == '\0');

    BrowserSession session;
    CHECK(browser_session_init(&session, budget, 32u * 1024u));
    TilefinchRequestContext cookie_context = {
        .target_url = "https://page.test/article",
        .initiator_url = "https://page.test/",
        .top_level_url = "https://page.test/",
        .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_SAME_ORIGIN,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_FETCH
    };
    CHECK(browser_session_cookie_set_http(
        &session, cookie_context.target_url,
        "builder=shared; Path=/; Secure; SameSite=Lax"));
    TilefinchRequestFacts cookie_facts;
    char regular_cookie[128], analyzed_cookie[128];
    CHECK(tilefinch_request_context_analyze(
              &cookie_context, &cookie_facts)
          && browser_session_cookie_header_context(
              &session, &cookie_context,
              regular_cookie, sizeof(regular_cookie))
          && browser_session_cookie_header_request_facts(
              &session, &cookie_context, &cookie_facts,
              analyzed_cookie, sizeof(analyzed_cookie))
          && strcmp(regular_cookie, analyzed_cookie) == 0
          && strcmp(regular_cookie, "builder=shared") == 0);
    fetch_response_security_metadata_reset(&metadata);
    CHECK(fetch_response_security_metadata_collect(
              &metadata, "strict-transport-security", 25,
              "max-age=60", 10, false)
          && browser_session_hsts_observe_metadata(
              &session, "https://typed.test/", &metadata));
    char upgraded[TILEFINCH_URL_SERIALIZED_LIMIT];
    CHECK(browser_session_hsts_upgrade_url(
              &session, "http://typed.test/a", upgraded, sizeof(upgraded))
          && strcmp(upgraded, "https://typed.test/a") == 0);

    TilefinchRequestContext context = {
        .target_url = "https://api.other.test/data",
        .initiator_url = "https://page.test/article",
        .top_level_url = "https://page.test/",
        .method = "POST",
        .mode = TILEFINCH_REQUEST_MODE_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_FETCH
    };
    FetchRequest transport = {
        .body = "x", .body_length = 1,
        .content_type = "text/plain", .accept = "application/json"
    };
    FetchPreparedPageRequest prepared;
    CHECK(fetch_prepare_page_request_context(
        &context, context.initiator_url, "strict-origin-when-cross-origin",
        &session, NULL, NULL, &transport, &prepared, NULL));
    const FetchRequest *request = fetch_prepared_page_request(&prepared);
    CHECK(request != NULL && request->page_context != NULL
          && request->prepared_page_version
                 == FETCH_PREPARED_PAGE_REQUEST_VERSION
          && strcmp(request->origin, "https://page.test") == 0
          && strcmp(request->referer, "https://page.test/") == 0
          && strcmp(request->sec_fetch_dest, "empty") == 0
          && strcmp(request->sec_fetch_mode, "cors") == 0
          && strcmp(request->sec_fetch_site, "cross-site") == 0
          && request->enforce_cors
          && fetch_request_validate(request, NULL));

    FetchRequest forged = *request;
    forged.prepared_page_version = 0;
    CHECK(!fetch_request_validate(&forged, NULL));
    transport.origin = "https://attacker.test";
    CHECK(!fetch_prepare_page_request_context(
        &context, context.initiator_url, NULL, &session, NULL, NULL,
        &transport, &prepared, NULL));
    /* A failed rebuild must invalidate an envelope that previously held a
       valid request; callers must never observe stale authority. */
    CHECK(fetch_prepared_page_request(&prepared) == NULL);
    TilefinchRequestContext impossible = context;
    impossible.mode = TILEFINCH_REQUEST_MODE_NAVIGATE;
    CHECK(!tilefinch_request_context_valid(&impossible));

    browser_session_destroy(&session);
    return 0;
}

int main(void)
{
    Budget budget;
    budget_init(&budget, 512u * 1024u);
    size_t empty_budget = budget.current;
    TilefinchRequestContext opaque_context = {
        .target_url = "https://opaque.test/api",
        .initiator_url = "https://opaque.test/frame",
        .top_level_url = "https://opaque.test/frame",
        .method = "GET", .mode = TILEFINCH_REQUEST_MODE_CORS,
        .credentials = TILEFINCH_CREDENTIALS_SAME_ORIGIN,
        .destination = TILEFINCH_DESTINATION_FETCH,
        .initiator_opaque = true
    };
    CHECK(tilefinch_request_context_valid(&opaque_context)
          && !tilefinch_request_same_origin(&opaque_context)
          && !tilefinch_request_same_site(&opaque_context)
          && !tilefinch_request_sends_credentials(&opaque_context)
          && strcmp(tilefinch_request_fetch_site(&opaque_context),
                    "cross-site") == 0);
    CHECK(test_bounded_site_adapter_state(&budget) == 0);
    CHECK(test_global_site_data_policy(&budget) == 0);
    CHECK(test_cookie_eviction_guard(&budget) == 0);
    CHECK(test_redacted_cookie_seed(&budget) == 0);
    CHECK(test_security_site_state(&budget) == 0);
    CHECK(test_active_mixed_content_block(&budget) == 0);
    CHECK(test_typed_resource_grants() == 0);
    CHECK(test_resource_cache_partitioning(&budget) == 0);
    CHECK(test_stylesheet_cache_signature_tracks_ram_authority(&budget) == 0);
    CHECK(test_module_cache_provenance() == 0);
    CHECK(test_response_cache_provenance() == 0);
    CHECK(test_typed_response_security_and_request_authority(&budget) == 0);
    BrowserSession expiry_session;
    char expiry_cookie[160];
    char expiry_header[128];
    CHECK(browser_session_init(
        &expiry_session, &budget, 32u * 1024u)
          && expiry_session.accounting_bytes
                 == sizeof(expiry_session.storage)
                    + sizeof(expiry_session.cookies)
                    + sizeof(expiry_session.cache)
                    + sizeof(expiry_session.site_adapter_state)
          && budget.external_reserved == expiry_session.accounting_bytes
          && budget_categories_reconcile(&budget));
    for (size_t i = 0; i < BROWSER_COOKIE_ENTRIES; i++) {
        char expiry_url[96];
        int url_written = snprintf(
            expiry_url, sizeof(expiry_url),
            "https://expiry-%zu.test/", i / BROWSER_COOKIE_PER_DOMAIN_LIMIT);
        int written = snprintf(
            expiry_cookie, sizeof(expiry_cookie),
            "stale-%02zu=old; Path=/; Secure; Max-Age=3600", i);
        CHECK(url_written > 0
              && (size_t) url_written < sizeof(expiry_url)
              && written > 0 && (size_t) written < sizeof(expiry_cookie));
        CHECK(browser_session_cookie_set_http(
            &expiry_session, expiry_url, expiry_cookie));
    }
    CHECK(expiry_session.cookie_bytes == BROWSER_COOKIE_ENTRIES * 3u);
    for (size_t i = 0; i < BROWSER_COOKIE_ENTRIES; i++) {
        CHECK(expiry_session.cookies[i].value != NULL);
        expiry_session.cookies[i].expires_at = 1;
    }
    CHECK(browser_session_cookie_set_http(
        &expiry_session, "https://expiry.test/",
        "fresh-name=fresh; Path=/; Secure; Max-Age=3600"));
    size_t occupied = 0;
    for (size_t i = 0; i < BROWSER_COOKIE_ENTRIES; i++) {
        if (expiry_session.cookies[i].value != NULL) occupied++;
    }
    CHECK(occupied == 1 && expiry_session.cookie_bytes == 5
          && browser_session_cookie_header(
              &expiry_session, "https://expiry.test/", expiry_header,
              sizeof(expiry_header))
          && strcmp(expiry_header, "fresh-name=fresh") == 0);
    browser_session_destroy(&expiry_session);
    CHECK(budget.current == empty_budget && budget.external_reserved == 0
          && budget_categories_reconcile(&budget));

    puts("test: long cookie paths are on-demand and per-domain slots evict");
    CHECK(browser_session_init(
        &expiry_session, &budget, 32u * 1024u));
    char long_cookie_url[768] = "https://long.test/";
    size_t long_prefix = strlen(long_cookie_url);
    memset(long_cookie_url + long_prefix, 'a', 420);
    memcpy(long_cookie_url + long_prefix + 420, "/leaf", 6);
    CHECK(browser_session_cookie_set_http(
              &expiry_session, long_cookie_url,
              "long-path=ok; Secure")
          && expiry_session.cookie_long_path_bytes > BROWSER_ORIGIN_LIMIT
          && browser_session_cookie_header(
              &expiry_session, long_cookie_url, expiry_header,
              sizeof(expiry_header))
          && strcmp(expiry_header, "long-path=ok") == 0);
    for (size_t i = 0; i < BROWSER_COOKIE_PER_DOMAIN_LIMIT + 1u; i++) {
        int written = snprintf(
            expiry_cookie, sizeof(expiry_cookie),
            "bounded-%02zu=v; Path=/; Secure", i);
        CHECK(written > 0 && (size_t) written < sizeof(expiry_cookie)
              && browser_session_cookie_set_http(
                  &expiry_session, "https://bounded.test/",
                  expiry_cookie));
    }
    CHECK(browser_session_cookie_set_http(
              &expiry_session, "https://other.test/",
              "other=kept; Path=/; Secure")
          && browser_session_cookie_header(
              &expiry_session, "https://bounded.test/", expiry_header,
              sizeof(expiry_header))
          && strstr(expiry_header, "bounded-00=") == NULL
          && strstr(expiry_header, "bounded-01=v") != NULL
          && browser_session_cookie_header(
              &expiry_session, "https://other.test/", expiry_header,
              sizeof(expiry_header))
          && strcmp(expiry_header, "other=kept") == 0);
    browser_session_destroy(&expiry_session);
    CHECK(budget.current == empty_budget && budget.external_reserved == 0
          && budget_categories_reconcile(&budget));

    BrowserSession overlay_session;
    char overlay_header[512];
    CHECK(browser_session_init(
        &overlay_session, &budget, 32u * 1024u));
    CHECK(browser_session_cookie_set_http(
        &overlay_session, "https://target.test/",
        "sid=secret; Path=/; Secure; HttpOnly"));
    /* A deeper insecure cookie would serialize before the root Secure cookie
       and shadow it at the server. Reject creation and deletion attempts. */
    CHECK(!browser_session_cookie_set_http(
              &overlay_session, "http://target.test/login/index",
              "sid=attacker; Path=/login")
          && !browser_session_cookie_set_http(
              &overlay_session, "http://target.test/login/index",
              "sid=; Path=/login; Max-Age=0")
          && browser_session_cookie_header(
              &overlay_session, "https://target.test/login/index",
              overlay_header, sizeof(overlay_header))
          && strcmp(overlay_header, "sid=secret") == 0);
    /* Compatible parent/subdomain scopes receive the same protection. */
    CHECK(browser_session_cookie_set_http(
              &overlay_session, "https://target.test/",
              "domain-sid=secret; Domain=target.test; Path=/; Secure")
          && !browser_session_cookie_set_http(
              &overlay_session, "http://sub.target.test/login/index",
              "domain-sid=attacker; Path=/login"));
    /* Different names and disjoint paths cannot shadow the Secure entry. */
    CHECK(browser_session_cookie_set_http(
              &overlay_session, "http://target.test/login/index",
              "other-sid=allowed; Path=/login")
          && browser_session_cookie_set_http(
              &overlay_session, "https://target.test/admin/index",
              "area-sid=secret; Path=/admin; Secure")
          && browser_session_cookie_set_http(
              &overlay_session, "http://target.test/login/index",
              "area-sid=allowed; Path=/login"));
    /* Partitioned and unpartitioned cookies can co-serialize for a matching
       top-level site, so the conservative overlay guard crosses both jars. */
    TilefinchRequestContext partitioned_overlay = {
        .target_url = "https://target.test/",
        .initiator_url = "https://top-a.test/",
        .top_level_url = "https://top-a.test/",
        .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_FETCH
    };
    CHECK(browser_session_cookie_set_http_context(
        &overlay_session, &partitioned_overlay,
        "partition-sid=secret; Path=/; Secure; SameSite=None; Partitioned"));
    TilefinchRequestContext unpartitioned_overlay = partitioned_overlay;
    unpartitioned_overlay.target_url = "http://target.test/login/index";
    /* Ordinary cross-site cookies are ignored without exposing a storage
       oracle. Once the site grant exists, the overlay guard still prevents
       the insecure cookie from shadowing its Partitioned Secure peer. */
    CHECK(browser_session_cookie_set_http_context(
        &overlay_session, &unpartitioned_overlay,
        "partition-sid=allowed; Path=/login"));
    CHECK(browser_session_set_third_party_cookie_site_allowed(
        &overlay_session, "https://top-a.test/", true));
    CHECK(!browser_session_cookie_set_http_context(
        &overlay_session, &unpartitioned_overlay,
        "partition-sid=allowed; Path=/login"));

    /* Redirect following needs a private cookie jar, but must not duplicate
       the storage and cache tables from BrowserSession. This budget is large
       enough for the fixed cookie table and cloned values, yet too small for
       the historical full-session overlay. */
    Budget redirect_budget;
    budget_init(&redirect_budget, 48u * 1024u);
    BrowserCookieOverlay *redirect_overlay =
        browser_session_cookie_overlay_create(
            &redirect_budget, &overlay_session);
    CHECK(redirect_overlay != NULL);
    TilefinchRequestContext redirect_context = {
        .target_url = "https://target.test/login/index",
        .initiator_url = "https://target.test/start",
        .top_level_url = "https://target.test/start",
        .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_NAVIGATE,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_DOCUMENT,
        .top_level_navigation = true
    };
    size_t source_cookie_bytes = overlay_session.cookie_bytes;
    CHECK(browser_cookie_overlay_header_context(
              redirect_overlay, &redirect_context,
              overlay_header, sizeof(overlay_header))
          && contains_cookie(overlay_header, "sid")
          && browser_cookie_overlay_set_http_context(
              redirect_overlay, &redirect_context,
              "redirect-only=staged; Path=/; Secure; HttpOnly")
          && browser_cookie_overlay_header_context(
              redirect_overlay, &redirect_context,
              overlay_header, sizeof(overlay_header))
          && contains_cookie(overlay_header, "redirect-only")
          && overlay_session.cookie_bytes == source_cookie_bytes
          && browser_session_cookie_header_context(
              &overlay_session, &redirect_context,
              overlay_header, sizeof(overlay_header))
          && !contains_cookie(overlay_header, "redirect-only"));
    browser_cookie_overlay_destroy(redirect_overlay);
    CHECK(redirect_budget.current == 0);
    Budget redirect_failure_budget;
    budget_init(&redirect_failure_budget, 48u * 1024u);
    budget_inject_failure_after(&redirect_failure_budget, 2);
    CHECK(browser_session_cookie_overlay_create(
              &redirect_failure_budget, &overlay_session) == NULL
          && redirect_failure_budget.current == 0);
    browser_session_destroy(&overlay_session);
    CHECK(budget.current == empty_budget);

    BrowserSession domain_session;
    char domain_header[128];
    CHECK(browser_session_init(
        &domain_session, &budget, 32u * 1024u));
    /* Parent-domain cookies use the bundled PSL: valid registrable scopes
       reach siblings, while ICANN and PRIVATE suffixes remain isolated. */
    CHECK(browser_session_cookie_set_http(
              &domain_session, "https://www.example.com/",
              "parent=wide; Domain=.ExAmPlE.CoM; Path=/; Secure")
          && browser_session_cookie_set_http(
              &domain_session, "https://www.example.com/",
              "host=only; Path=/; Secure")
          && browser_session_cookie_header(
              &domain_session, "https://api.example.com/", domain_header,
              sizeof(domain_header))
          && contains_cookie(domain_header, "parent")
          && !contains_cookie(domain_header, "host")
          && !browser_session_cookie_set_http(
              &domain_session, "https://www.example.com/",
              "suffix=bad; Domain=com; Path=/; Secure")
          && !browser_session_cookie_set_http(
              &domain_session, "https://shop.example.co.uk/",
              "suffix=bad; Domain=co.uk; Path=/; Secure")
          && !browser_session_cookie_set_http(
              &domain_session, "https://alice.github.io/",
              "suffix=bad; Domain=github.io; Path=/; Secure")
          && !browser_session_cookie_set_http(
              &domain_session, "https://www.example.com/",
              "unrelated=bad; Domain=example.net; Path=/; Secure")
          && !browser_session_cookie_set_http(
              &domain_session, "https://127.0.0.1/",
              "numeric=bad; Domain=0.0.1; Path=/; Secure")
          && !browser_session_cookie_set_http(
              &domain_session, "https://www.example.com/",
              "__Host-invalid=value; Domain=example.com; Path=/; Secure"));
    /* An exact public-suffix host follows RFC 6265's safe exception and stays
       host-only; the Domain attribute is still visible to __Host- checks. */
    CHECK(browser_session_cookie_set_http(
              &domain_session, "https://test/",
              "suffix=exact; Domain=test; Path=/; Secure")
          && browser_session_cookie_header(
              &domain_session, "https://test/", domain_header,
              sizeof(domain_header))
          && strcmp(domain_header, "suffix=exact") == 0
          && browser_session_cookie_header(
              &domain_session, "https://sub.test/", domain_header,
              sizeof(domain_header))
          && domain_header[0] == '\0'
          && !browser_session_cookie_set_http(
              &domain_session, "https://test/",
              "__Host-invalid=value; Domain=test; Path=/; Secure"));
    browser_session_destroy(&domain_session);
    CHECK(budget.current == empty_budget);

    BrowserSession session;
    char header[1024];
    CHECK(browser_session_init(&session, &budget, 32u * 1024u));
    CHECK(!browser_session_storage_set(
        &session, "https://target.test/", true, "oversized", "x",
        SIZE_MAX));

    CHECK(browser_session_cookie_set_http(
        &session, "https://target.test/path/index",
        "strict=s; Path=/; Secure; SameSite=Strict"));
    CHECK(browser_session_cookie_set_http(
        &session, "https://target.test/path/index",
        "lax=l; Path=/; Secure; SameSite=Lax"));
    CHECK(browser_session_cookie_set_http(
        &session, "https://target.test/path/index",
        "none=n; Path=/; Secure; SameSite=None"));
    CHECK(!browser_session_cookie_set_http(
        &session, "https://target.test/",
        "bad-none=x; Path=/; SameSite=None"));
    CHECK(!browser_session_cookie_set_http(
        &session, "https://target.test/",
        "bad-domain=x; Domain=test; Path=/; Secure"));
    CHECK(!browser_session_cookie_set_http(
        &session, "https://target.test/",
        "__Host-bad=x; Domain=target.test; Path=/; Secure"));
    CHECK(!browser_session_cookie_set_http(
        &session, "https://target.test/", "bad name=x; Path=/; Secure"));
    CHECK(!browser_session_cookie_set_http(
        &session, "https://target.test/", "bad:name=x; Path=/; Secure"));
    CHECK(!browser_session_cookie_set_http(
        &session, "https://target.test/",
        "injected=x\r\nSet-Cookie: second=y; Path=/; Secure"));
    CHECK(!browser_session_cookie_set_http(
        &session, "https://target.test/", "nonascii=\xC3\xA9; Path=/; Secure"));
    CHECK(browser_session_cookie_set_http(
        &session, "https://target.test/",
        "server-only=secret; Path=/; Secure; HttpOnly"));
    CHECK(browser_session_cookie_set_http(
              &session, "https://target.test/", "ordered=root; Path=/; Secure")
          && browser_session_cookie_set_http(
              &session, "https://target.test/path/index",
              "ordered=deep; Path=/path; Secure")
          && browser_session_cookie_header(
              &session, "https://target.test/path/index", header,
              sizeof(header)));
    const char *deep_cookie = strstr(header, "ordered=deep");
    const char *root_cookie = strstr(header, "ordered=root");
    CHECK(deep_cookie != NULL && root_cookie != NULL
          && deep_cookie < root_cookie);
    char undersized_header[8] = "stale";
    CHECK(!browser_session_cookie_header(
              &session, "https://target.test/path/index",
              undersized_header, sizeof(undersized_header))
          && undersized_header[0] == '\0');
    CHECK(!browser_session_cookie_set(
        &session, "https://target.test/",
        "server-only=script; Path=/; Secure"));
    CHECK(!browser_session_cookie_set(
        &session, "https://target.test/",
        "server-only=; Path=/; Secure; Max-Age=0"));
    CHECK(browser_session_cookie_header(
        &session, "https://target.test/", header, sizeof(header))
          && strstr(header, "server-only=secret") != NULL);
    CHECK(!browser_session_cookie_set_http(
        &session, "http://target.test/",
        "server-only=insecure; Path=/"));

    TilefinchRequestContext request = {
        .target_url = "https://target.test/data",
        .initiator_url = "https://other.test/page",
        .top_level_url = "https://other.test/page",
        .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_FETCH
    };
    CHECK(browser_session_cookie_header_context(
        &session, &request, header, sizeof(header)));
    CHECK(!contains_cookie(header, "strict")
          && !contains_cookie(header, "lax")
          && !contains_cookie(header, "none"));
    CHECK(browser_session_set_third_party_cookie_site_allowed(
              &session, request.top_level_url, true)
          && browser_session_cookie_header_context(
              &session, &request, header, sizeof(header))
          && contains_cookie(header, "none"));

    /* A Critical-CH restart is still the same frame request context. A
       default-SameSite response cookie is eligible for a same-site frame
       retry, but must remain withheld from an otherwise identical cross-site
       frame. */
    TilefinchRequestContext frame_retry = {
        .target_url = "https://frame.test/child",
        .initiator_url = "https://frame.test/parent",
        .top_level_url = "https://frame.test/parent",
        .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_NAVIGATE,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_FRAME
    };
    CHECK(browser_session_cookie_set_http_context(
              &session, &frame_retry,
              "frame-retry=seen; Path=/; Secure")
          && browser_session_cookie_header_context(
              &session, &frame_retry, header, sizeof(header))
          && contains_cookie(header, "frame-retry"));
    TilefinchRequestContext cross_site_frame_retry = frame_retry;
    cross_site_frame_retry.initiator_url = "https://other.test/parent";
    cross_site_frame_retry.top_level_url = "https://other.test/parent";
    CHECK(browser_session_cookie_header_context(
              &session, &cross_site_frame_retry, header, sizeof(header))
          && !contains_cookie(header, "frame-retry")
          && browser_session_cookie_header_context(
              &session, &frame_retry, header, sizeof(header))
          && contains_cookie(header, "frame-retry"));

    request.mode = TILEFINCH_REQUEST_MODE_NAVIGATE;
    request.destination = TILEFINCH_DESTINATION_DOCUMENT;
    request.top_level_navigation = true;
    CHECK(browser_session_cookie_header_context(
        &session, &request, header, sizeof(header)));
    CHECK(!contains_cookie(header, "strict")
          && contains_cookie(header, "lax")
          && contains_cookie(header, "none"));
    request.method = "POST";
    CHECK(browser_session_cookie_header_context(
        &session, &request, header, sizeof(header)));
    CHECK(!contains_cookie(header, "strict")
          && !contains_cookie(header, "lax")
          && contains_cookie(header, "none"));

    TilefinchRequestContext partition_set = {
        .target_url = "https://target.test/",
        .initiator_url = "https://top.test/",
        .top_level_url = "https://top.test/",
        .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_FETCH
    };
    CHECK(browser_session_cookie_set_http_context(
        &session, &partition_set,
        "part=p; Path=/; Secure; SameSite=None; Partitioned"));
    CHECK(!browser_session_cookie_set_http_context(
        &session, &partition_set,
        "bad-part=p; Path=/; SameSite=None; Partitioned"));
    CHECK(browser_session_cookie_header_context(
        &session, &partition_set, header, sizeof(header)));
    CHECK(contains_cookie(header, "part"));
    partition_set.top_level_url = "https://different.test/";
    CHECK(browser_session_cookie_header_context(
        &session, &partition_set, header, sizeof(header)));
    CHECK(!contains_cookie(header, "part"));

    request.credentials = TILEFINCH_CREDENTIALS_OMIT;
    CHECK(browser_session_cookie_header_context(
        &session, &request, header, sizeof(header)) && header[0] == '\0');
    CHECK(!browser_session_cookie_header_context(
              NULL, &request, header, sizeof(header))
          && header[0] == '\0');
    CHECK(!browser_session_cookie_set_http_context(
        &session, &request, "omitted=must-not-store; Path=/; Secure"));
    request.credentials = (TilefinchCredentialsMode) 99;
    CHECK(!browser_session_cookie_header_context(
              &session, &request, header, sizeof(header))
          && header[0] == '\0');
    request.credentials = TILEFINCH_CREDENTIALS_INCLUDE;
    CHECK(browser_session_cookie_header_context(
        &session, &request, header, sizeof(header))
          && !contains_cookie(header, "omitted"));

    TilefinchRequestContext default_port = {
        .target_url = "https://target.test:443/data",
        .initiator_url = "HTTPS://TARGET.TEST/page",
        .top_level_url = "https://target.test/",
        .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_CORS,
        .credentials = TILEFINCH_CREDENTIALS_SAME_ORIGIN,
        .destination = TILEFINCH_DESTINATION_FETCH
    };
    CHECK(browser_session_cookie_header_context(
        &session, &default_port, header, sizeof(header))
          && contains_cookie(header, "strict"));
    default_port.target_url = "https://other.test/data";
    CHECK(browser_session_cookie_header_context(
        &session, &default_port, header, sizeof(header))
          && header[0] == '\0');
    CHECK(!browser_session_cookie_set_http_context(
        &session, &default_port,
        "cross-origin-same-origin=must-not-store; Path=/; Secure"));
    default_port.credentials = TILEFINCH_CREDENTIALS_INCLUDE;
    CHECK(browser_session_cookie_header_context(
        &session, &default_port, header, sizeof(header))
          && !contains_cookie(header, "cross-origin-same-origin"));

    static const unsigned char cached[] = "canonical-cache";
    CHECK(browser_session_cache_put_response(
        &session, "HTTPS://Target.Test:443/a/../asset#one",
        cached, sizeof(cached) - 1, "\"v1\"", NULL, "text/plain"));
    CHECK(!browser_session_cache_put_response(
        &session, "https://user:secret@target.test/private",
        cached, sizeof(cached) - 1, "\"v1\"", NULL, "text/plain"));
    const BrowserCacheEntry *entry = browser_session_cache_lookup(
        &session, "https://target.test/asset#two");
    CHECK(entry != NULL
          && strcmp(entry->url, "https://target.test/asset") == 0
          && entry->length == sizeof(cached) - 1
          && memcmp(entry->data, cached, sizeof(cached) - 1) == 0);
    CHECK(browser_session_cache_put_http(
        &session, "https://target.test/invalid-age", cached,
        sizeof(cached) - 1, NULL, NULL, "text/plain",
        "max-age=5junk", NULL, 100));
    CHECK(browser_session_cache_match_http(
        &session, "https://target.test/invalid-age", 100, NULL)
          == BROWSER_CACHE_STALE);
    CHECK(browser_session_cache_put_http(
        &session, "https://target.test/negative-age", cached,
        sizeof(cached) - 1, NULL, NULL, "text/plain",
        "max-age=-1", NULL, 100));
    CHECK(browser_session_cache_match_http(
        &session, "https://target.test/negative-age", 100, NULL)
          == BROWSER_CACHE_STALE);
    CHECK(browser_session_cache_put_http(
        &session, "https://target.test/valid-age", cached,
        sizeof(cached) - 1, NULL, NULL, "text/plain",
        "max-age=5", NULL, 100));
    CHECK(browser_session_cache_match_http(
        &session, "https://target.test/valid-age", 104, NULL)
          == BROWSER_CACHE_FRESH);

    /* Shared ownership accepts only a complete allocation from its declared
       Budget, and a session never imports accounting from another envelope. */
    Budget foreign_budget;
    budget_init(&foreign_budget, 4096);
    unsigned char *foreign_data = budget_malloc(&foreign_budget, 16);
    unsigned char *short_data = budget_malloc(&budget, 4);
    CHECK(foreign_data != NULL && short_data != NULL
          && browser_shared_body_take(&budget, foreign_data, 16) == NULL
          && browser_shared_body_take(&budget, short_data, 5) == NULL);
    budget_free(&budget, short_data);
    BrowserSharedBody *foreign_body = browser_shared_body_take(
        &foreign_budget, foreign_data, 16);
    CHECK(foreign_body != NULL && foreign_body->references == 1
          && !browser_session_cache_put_http_shared(
              &session, "https://target.test/foreign", foreign_body,
              NULL, NULL, "application/octet-stream", "max-age=60", NULL,
              100)
          && foreign_body->references == 1
          && browser_session_cache_lookup(
              &session, "https://target.test/foreign") == NULL);
    browser_shared_body_release(foreign_body);
    CHECK(foreign_budget.current == 0);

    static const unsigned char fetched_payload[] = "fetch-result-shared";
    char *fetched_data = budget_malloc_category(
        &budget, BUDGET_CATEGORY_RESOURCE, sizeof(fetched_payload) - 1u);
    CHECK(fetched_data != NULL);
    memcpy(fetched_data, fetched_payload, sizeof(fetched_payload) - 1u);
    FetchResult fetched = {
        .budget = &budget,
        .data = fetched_data,
        .length = sizeof(fetched_payload) - 1u,
        .capacity = sizeof(fetched_payload) - 1u
    };
    CHECK(fetch_result_share_body(&fetched)
          && fetched.data == fetched_data && fetched.capacity == 0
          && fetched.shared_body != NULL
          && fetched.shared_body->data == (unsigned char *) fetched_data
          && fetched.shared_body->references == 1
          && fetch_result_share_body(&fetched)
          && fetched.shared_body->references == 1
          && browser_session_cache_put_http_shared(
              &session, "https://target.test/fetched-shared",
              fetched.shared_body, NULL, NULL, "text/javascript",
              "max-age=60", NULL, 100)
          && fetched.shared_body->references == 2);
    fetch_result_destroy(&fetched);
    entry = browser_session_cache_lookup(
        &session, "https://target.test/fetched-shared");
    CHECK(entry != NULL && entry->data == (unsigned char *) fetched_data
          && entry->body != NULL && entry->body->references == 1);

    static const unsigned char shared_payload[] = "shared-cache-body";
    unsigned char *owned_payload = budget_malloc_category(
        &budget, BUDGET_CATEGORY_RESOURCE, sizeof(shared_payload) - 1u);
    CHECK(owned_payload != NULL);
    memcpy(owned_payload, shared_payload, sizeof(shared_payload) - 1u);
    BrowserSharedBody *shared = browser_shared_body_take(
        &budget, owned_payload, sizeof(shared_payload) - 1u);
    CHECK(shared != NULL && shared->references == 1);
    CHECK(browser_session_cache_put_http_shared(
        &session, "https://target.test/shared", shared, NULL, NULL,
        "application/octet-stream", "max-age=60", NULL, 100));
    CHECK(shared->references == 2);
    browser_shared_body_release(shared);

    entry = browser_session_cache_lookup(
        &session, "https://target.test/shared");
    CHECK(entry != NULL && entry->body != NULL
          && entry->data == owned_payload
          && entry->body->references == 1
          && memcmp(entry->data, shared_payload,
                    sizeof(shared_payload) - 1u) == 0);

    size_t cache_bytes_before_reclaim = session.cache_bytes;
    size_t cache_evictions_before_reclaim = session.cache_evictions;
    size_t physically_reclaimed = browser_session_cache_reclaim(&session, 1);
    CHECK(physically_reclaimed > 0
          && session.cache_bytes < cache_bytes_before_reclaim
          && session.cache_evictions == cache_evictions_before_reclaim + 1);

    /* Prospective cache-size accounting must not wrap at the 32-bit/size_t
       boundary and admit an entry that is actually over quota. */
    BrowserSession overflow_session;
    CHECK(browser_session_init(&overflow_session, &budget, SIZE_MAX));
    unsigned char *overflow_data = budget_malloc(&budget, 8);
    CHECK(overflow_data != NULL);
    BrowserSharedBody *overflow_body = browser_shared_body_take(
        &budget, overflow_data, 8);
    CHECK(overflow_body != NULL);
    overflow_session.cache_bytes = SIZE_MAX - 4;
    CHECK(!browser_session_cache_put_http_shared(
        &overflow_session, "https://example.test/overflow", overflow_body,
        NULL, NULL, "application/octet-stream", "max-age=60", NULL, 1));
    CHECK(overflow_session.cache_bytes == SIZE_MAX - 4);
    overflow_session.cache_bytes = 0;
    browser_shared_body_release(overflow_body);
    browser_session_destroy(&overflow_session);

    browser_session_destroy(&session);
    CHECK(budget.current == 0);
    puts("session-security-tests status=PASS");
    return 0;
}
