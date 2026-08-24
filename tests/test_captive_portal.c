#include "tilefinch/captive_portal.h"
#include "tilefinch/content_security_policy.h"
#include "tilefinch/fetch.h"
#include "tilefinch/session.h"
#include "tilefinch/session_persistence.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

int main(void)
{
    TilefinchCaptiveProbeResponse response = {
        .status_code = 204, .transport_succeeded = true
    };
    CHECK(tilefinch_captive_probe_classify(&response)
          == TILEFINCH_CAPTIVE_PROBE_INTERNET);

    response.status_code = 302;
    response.location = "http://192.168.4.1/login";
    CHECK(tilefinch_captive_probe_classify(&response)
          == TILEFINCH_CAPTIVE_PROBE_PORTAL);
    char url[TILEFINCH_URL_SERIALIZED_LIMIT];
    CHECK(tilefinch_captive_probe_portal_url(
        TILEFINCH_CAPTIVE_PORTAL_PROBE_URL, response.location, url));
    CHECK(strcmp(url, "http://192.168.4.1/login") == 0);

    static const unsigned char html[] =
        " \n<!doctype html><title>Guest sign-in</title>";
    response = (TilefinchCaptiveProbeResponse) {
        .status_code = 200,
        .content_type = "text/html; charset=utf-8",
        .body = html,
        .body_length = sizeof(html) - 1u,
        .transport_succeeded = true
    };
    CHECK(tilefinch_captive_probe_classify(&response)
          == TILEFINCH_CAPTIVE_PROBE_PORTAL);
    CHECK(tilefinch_captive_probe_portal_url(
        TILEFINCH_CAPTIVE_PORTAL_PROBE_URL, NULL, url));
    CHECK(strcmp(url, TILEFINCH_CAPTIVE_PORTAL_PROBE_URL) == 0);

    static const unsigned char unexpected[] = "sign in required";
    response = (TilefinchCaptiveProbeResponse) {
        .status_code = 200,
        .content_type = "text/plain",
        .body = unexpected,
        .body_length = sizeof(unexpected) - 1u,
        .transport_succeeded = true
    };
    CHECK(tilefinch_captive_probe_classify(&response)
          == TILEFINCH_CAPTIVE_PROBE_PORTAL);

    response = (TilefinchCaptiveProbeResponse) {
        .timed_out = true
    };
    CHECK(tilefinch_captive_probe_classify(&response)
          == TILEFINCH_CAPTIVE_PROBE_RETRYABLE);
    CHECK(!tilefinch_captive_probe_portal_url(
        TILEFINCH_CAPTIVE_PORTAL_PROBE_URL, "file:///tmp/no", url));

    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    BrowserSession session;
    CHECK(browser_session_init(&session, &budget, 128u * 1024u));
    CHECK(browser_session_storage_set(
        &session, "https://ordinary.example/", true,
        "name", "ordinary", 8u));
    CHECK(browser_session_cookie_set(
        &session, "https://ordinary.example/", "sid=ordinary; Secure"));
    char cookies[128];
    CHECK(browser_session_cookie_header(
        &session, "https://ordinary.example/", cookies, sizeof(cookies))
        && strstr(cookies, "sid=ordinary") != NULL);
    static const unsigned char cached[] = "ordinary body";
    CHECK(browser_session_cache_put(
        &session, "https://ordinary.example/page",
        cached, sizeof(cached) - 1u));

    CHECK(browser_session_captive_portal_begin(
        &session, "http://192.168.4.1/login"));
    static const char csp_header[] =
        "content-security-policy: default-src 'none'\n";
    TilefinchContentSecurityPolicy csp;
    CHECK(tilefinch_csp_parse_response_headers(
        &csp, "http://192.168.4.1/login", csp_header,
        sizeof(csp_header) - 1u, false));
    TilefinchRequestContext portal_context = {
        .target_url = "http://192.168.4.1/step",
        .initiator_url = "http://192.168.4.1/login",
        .top_level_url = "http://192.168.4.1/login",
        .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_NAVIGATE,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        /* Keep the transport's portal-navigation envelope while exercising
           a destination which CSP controls. The old wrapper returned before
           consulting destination policy at all. */
        .destination = TILEFINCH_DESTINATION_SCRIPT,
        .top_level_navigation = true,
        .user_activated = true
    };
    FetchRequest portal_request = {
        .cookie_session = &session,
        .page_context = &portal_context,
        .content_security_policy = &csp,
        .sec_fetch_dest = "document",
        .sec_fetch_mode = "navigate"
    };
    CHECK(!fetch_request_security_allows_target(
        &portal_request, portal_context.target_url));
    CHECK(browser_session_persistence_save(
              &session, "/unused-portal-session",
              BROWSER_SESSION_PERSIST_ALL, NULL)
          == BROWSER_SESSION_PERSISTENCE_INVALID_ARGUMENT);
    const char *stored = NULL;
    size_t stored_length = 0;
    CHECK(!browser_session_storage_get(
        &session, "https://ordinary.example/", true,
        "name", &stored, &stored_length));
    CHECK(browser_session_cookie_header(
        &session, "https://ordinary.example/", cookies, sizeof(cookies))
        && cookies[0] == '\0');
    CHECK(browser_session_captive_portal_url_allowed(
        &session, "http://192.168.4.1/step"));
    CHECK(!browser_session_captive_portal_url_allowed(
        &session, "http://192.168.5.1/"));
    CHECK(browser_session_captive_portal_authorize_navigation(
        &session, "http://192.168.4.1/step",
        "https://login.example/continue"));
    CHECK(browser_session_captive_portal_url_allowed(
        &session, "https://login.example/done"));
    CHECK(browser_session_captive_portal_authorize_navigation(
        &session, "https://login.example/continue",
        "https://identity.example/step"));
    CHECK(browser_session_captive_portal_authorize_navigation(
        &session, "https://identity.example/step",
        "http://terms.example/accept"));
    CHECK(!browser_session_captive_portal_authorize_navigation(
        &session, "http://terms.example/accept",
        "https://fifth.example/too-many"));
    CHECK(!browser_session_captive_portal_authorize_navigation(
        &session, "https://untrusted.example/",
        "https://also-untrusted.example/"));
    CHECK(!browser_session_captive_portal_authorize_navigation(
        &session, "http://192.168.4.1/", "file:///not-authorized"));
    CHECK(browser_session_cache_lookup(
        &session, "https://ordinary.example/page") == NULL);
    CHECK(!browser_session_cache_put(
        &session, "http://192.168.4.1/cached",
        cached, sizeof(cached) - 1u));
    CHECK(browser_session_storage_set(
        &session, "http://192.168.4.1/", false,
        "portal", "temporary", 9u));
    CHECK(browser_session_cookie_set(
        &session, "http://192.168.4.1/", "portal=temporary"));
    CHECK(browser_session_cookie_header(
        &session, "http://192.168.4.1/", cookies, sizeof(cookies))
        && strstr(cookies, "portal=temporary") != NULL);

    browser_session_captive_portal_end(&session);
    CHECK(!browser_session_captive_portal_active(&session));
    CHECK(browser_session_storage_get(
        &session, "https://ordinary.example/", true,
        "name", &stored, &stored_length));
    CHECK(stored_length == 8u && memcmp(stored, "ordinary", 8u) == 0);
    CHECK(!browser_session_storage_get(
        &session, "http://192.168.4.1/", false,
        "portal", &stored, &stored_length));
    CHECK(browser_session_cookie_header(
        &session, "https://ordinary.example/", cookies, sizeof(cookies))
        && strstr(cookies, "sid=ordinary") != NULL);
    CHECK(browser_session_cookie_header(
        &session, "http://192.168.4.1/", cookies, sizeof(cookies))
        && cookies[0] == '\0');
    CHECK(browser_session_cache_lookup(
        &session, "https://ordinary.example/page") != NULL);

    /* Destruction is also a portal-exit boundary: temporary data is dropped,
       the ordinary tables are restored, and then all owned bytes are freed. */
    CHECK(browser_session_captive_portal_begin(
        &session, "http://192.168.4.1/login"));
    CHECK(browser_session_storage_set(
        &session, "http://192.168.4.1/", false,
        "portal", "temporary", 9u));
    browser_session_destroy(&session);
    CHECK(budget.current == 0u);
    puts("captive portal tests passed");
    return 0;
}
