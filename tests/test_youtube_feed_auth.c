#include "tilefinch/youtube_lite.h"
#include "tilefinch/request_context.h"
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", \
    __FILE__, __LINE__, #x); return 1; } } while (0)

int main(void)
{
    Budget budget;
    budget_init(&budget, 512u * 1024u);
    /* Sign-in state as the feed job's own classifier reads it. */
    const char *signed_in = "{\"responseContext\":{\"mainAppWebResponseContext\":"
        "{\"loggedOut\":false}},\"contents\":[{\"videoRenderer\":{"
        "\"videoId\":\"abcdefghijk\",\"title\":{\"simpleText\":\"Mock video\"}}}]}";
    CHECK(youtube_lite_test_feed_authentication(signed_in, strlen(signed_in)) == 1);
    BrowserSession session;
    CHECK(browser_session_init(&session, &budget, 1024));
    CHECK(browser_session_cookie_set_http(&session, "https://m.youtube.com/",
        "SAPISID=mock-session; Secure; HttpOnly; Path=/"));
    CHECK(youtube_lite_rating_begin(NULL, &session, "abcdefghijk", true, 1000) == NULL);
    CHECK(youtube_lite_rating_begin(&budget, NULL, "abcdefghijk", false, 1000) == NULL);
    CHECK(youtube_lite_rating_begin(&budget, &session, "invalid/id", true, 1000) == NULL);
    size_t saved_limit = budget.limit;
    budget.limit = budget.current;
    CHECK(youtube_lite_rating_begin(&budget, &session, "abcdefghijk", false, 1000) == NULL);
    budget.limit = saved_limit;
    TilefinchRequestContext context = {
        .target_url = "https://m.youtube.com/youtubei/v1/browse",
        .initiator_url = "https://m.youtube.com/", .top_level_url = "https://m.youtube.com/",
        .method = "POST", .mode = TILEFINCH_REQUEST_MODE_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_FETCH
    };
    char authorization[96];
    CHECK(youtube_lite_test_feed_authorization(&session, &context, 1700000000,
        authorization, sizeof(authorization)));
    CHECK(strcmp(authorization, "SAPISIDHASH 1700000000_50415b7cc300dc567f070177e92d31db3da4ddea") == 0);
    CHECK(!youtube_lite_test_feed_authorization(&session, &context, 1700000000,
        authorization, 4) && authorization[0] == '\0');
    context.initiator_url = "https://unrelated.test/";
    CHECK(!youtube_lite_test_feed_authorization(&session, &context, 1700000000,
        authorization, sizeof(authorization)) && authorization[0] == '\0');
    context.initiator_url = "https://m.youtube.com/";
    context.credentials = TILEFINCH_CREDENTIALS_OMIT;
    CHECK(!youtube_lite_test_feed_authorization(&session, &context, 1700000000,
        authorization, sizeof(authorization)) && authorization[0] == '\0');
    context.credentials = TILEFINCH_CREDENTIALS_INCLUDE;
    CHECK(browser_session_cookie_set_http(&session, "https://m.youtube.com/",
        "SAPISID=; Secure; HttpOnly; Path=/; Max-Age=0"));
    char fallback[192];
    memcpy(fallback, "__Secure-3PAPISID=", 18);
    memset(fallback + 18, 'x', 120);
    static const char attributes[] = "; Secure; HttpOnly; Path=/";
    memcpy(fallback + 138, attributes, sizeof(attributes));
    CHECK(strlen(fallback) == 138u + sizeof(attributes) - 1u);
    CHECK(browser_session_cookie_set_http(&session, "https://m.youtube.com/", fallback));
    CHECK(youtube_lite_test_feed_authorization(&session, &context, 1700000000,
        authorization, sizeof(authorization)));
    CHECK(strcmp(authorization, "SAPISIDHASH 1700000000_fa463aa826a789481717a032cfc472c309b2b703") == 0);
    /* The signed-in identity reads the same cookie the signature used. */
    CHECK(youtube_lite_session_signed_in(&session));
    browser_session_destroy(&session);
    CHECK(budget.current == 0);
    /* A signed-in jar whose m.youtube.com Cookie header outgrows
       BROWSER_COOKIE_HEADER_CAPACITY still reads as signed in, still signs,
       and its provider requests still carry the account cookie, even
       behind 27 older HttpOnly cookies that alone fill the header
       (review 2026-10-21 M4). */
    BrowserSession large;
    CHECK(browser_session_init(&large, &budget, 1024));
    youtube_lite_session_register_credentials(&large);
    for (unsigned i = 0; i < 27u; ++i) {
        char filler[256];
        snprintf(filler, sizeof(filler),
            "filler-cookie-with-a-long-name-number-%02u=%0114u; Secure; HttpOnly; Path=/",
            i, i);
        CHECK(browser_session_cookie_set_http(&large, "https://m.youtube.com/", filler));
    }
    CHECK(browser_session_cookie_set_http(&large, "https://m.youtube.com/",
        "SAPISID=mock-session; Secure; Path=/"));
    CHECK(youtube_lite_session_signed_in(&large));
    CHECK(youtube_lite_test_feed_authorization(&large, &context, 1700000000,
        authorization, sizeof(authorization)));
    CHECK(strcmp(authorization, "SAPISIDHASH 1700000000_50415b7cc300dc567f070177e92d31db3da4ddea") == 0);
    static char header[BROWSER_COOKIE_HEADER_CAPACITY];
    CHECK(browser_session_cookie_header_context(&large, &context, header, sizeof(header))
        && strstr(header, "SAPISID=mock-session") != NULL
        && strstr(header, "filler-cookie-with-a-long-name-number-00=") == header);
    browser_session_destroy(&large);
    CHECK(budget.current == 0);
    const char *signed_out = "{\"responseContext\":{\"mainAppWebResponseContext\":"
        "{\"loggedOut\":true}},\"error\":{\"code\":401}}";
    CHECK(youtube_lite_test_feed_authentication(signed_out, strlen(signed_out)) == -1);
    CHECK(youtube_lite_test_feed_authentication("{} garbage", 10) == 0);
    /* Only the top-level responseContext speaks for the account. */
    const char *nested = "{\"unrelated\":{\"error\":true,\"loggedOut\":true}}";
    CHECK(youtube_lite_test_feed_authentication(nested, strlen(nested)) == 0);
    /* MWEB also reports authentication through its tracking parameters. */
    const char *tracked_in = "{\"responseContext\":{\"serviceTrackingParams\":["
        "{\"params\":[{\"key\":\"logged_in\",\"value\":\"1\"}]}]},"
        "\"contents\":{\"messageRenderer\":{\"text\":{\"simpleText\":\"Mock empty feed\"}}}}";
    CHECK(youtube_lite_test_feed_authentication(tracked_in, strlen(tracked_in)) == 1);
    const char *tracked_out = "{\"responseContext\":{\"serviceTrackingParams\":["
        "{\"service\":\"GFEEDBACK\"},"
        "{\"params\":[{\"key\":\"e\",\"value\":\"1\"},"
        "{\"key\":\"logged_in\",\"value\":\"0\"}]}]}}";
    CHECK(youtube_lite_test_feed_authentication(tracked_out, strlen(tracked_out)) == -1);
    /* The explicit loggedOut flag wins over tracking parameters. */
    const char *both = "{\"responseContext\":{\"mainAppWebResponseContext\":"
        "{\"loggedOut\":true},\"serviceTrackingParams\":["
        "{\"params\":[{\"key\":\"logged_in\",\"value\":\"1\"}]}]}}";
    CHECK(youtube_lite_test_feed_authentication(both, strlen(both)) == -1);
    const char *unrelated_login = "{\"contents\":{\"serviceTrackingParams\":["
        "{\"params\":[{\"key\":\"logged_in\",\"value\":\"1\"}]}]}}";
    CHECK(youtube_lite_test_feed_authentication(unrelated_login, strlen(unrelated_login)) == 0);
    return 0;
}
