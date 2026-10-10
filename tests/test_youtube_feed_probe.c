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
    YoutubeLiteFeedProbe result = {0};
    const char *signed_in = "{\"responseContext\":{\"mainAppWebResponseContext\":"
        "{\"loggedOut\":false}},\"contents\":[{\"videoRenderer\":{"
        "\"videoId\":\"abcdefghijk\",\"title\":{\"simpleText\":\"Mock video\"}}}]}";
    CHECK(youtube_lite_feed_probe_classify(&budget, signed_in, strlen(signed_in), &result));
    CHECK(result.api_json && result.api_login_known && !result.api_logged_out);
    CHECK(result.result_count == 1 && !result.api_error);
    CHECK(budget.current == 0);
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
    CHECK(youtube_lite_feed_probe_authorization(&session, &context, 1700000000,
        authorization, sizeof(authorization)));
    CHECK(strcmp(authorization, "SAPISIDHASH 1700000000_50415b7cc300dc567f070177e92d31db3da4ddea") == 0);
    CHECK(!youtube_lite_feed_probe_authorization(&session, &context, 1700000000,
        authorization, 4) && authorization[0] == '\0');
    context.initiator_url = "https://unrelated.test/";
    CHECK(!youtube_lite_feed_probe_authorization(&session, &context, 1700000000,
        authorization, sizeof(authorization)) && authorization[0] == '\0');
    context.initiator_url = "https://m.youtube.com/";
    context.credentials = TILEFINCH_CREDENTIALS_OMIT;
    CHECK(!youtube_lite_feed_probe_authorization(&session, &context, 1700000000,
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
    CHECK(youtube_lite_feed_probe_authorization(&session, &context, 1700000000,
        authorization, sizeof(authorization)));
    CHECK(strcmp(authorization, "SAPISIDHASH 1700000000_fa463aa826a789481717a032cfc472c309b2b703") == 0);
    browser_session_destroy(&session);
    CHECK(budget.current == 0);
    const char *signed_out = "{\"responseContext\":{\"mainAppWebResponseContext\":"
        "{\"loggedOut\":true}},\"error\":{\"code\":401}}";
    CHECK(youtube_lite_feed_probe_classify(&budget, signed_out, strlen(signed_out), &result));
    CHECK(result.api_error && result.api_logged_out && result.result_count == 0);
    CHECK(!youtube_lite_feed_probe_classify(&budget, "{} garbage", 10, &result));
    CHECK(!result.api_json);
    const char *nested = "{\"unrelated\":{\"error\":true,\"loggedOut\":true}}";
    CHECK(youtube_lite_feed_probe_classify(&budget, nested, strlen(nested), &result));
    CHECK(!result.api_error && !result.api_login_known);
    const char *empty_feed = "{\"responseContext\":{\"serviceTrackingParams\":["
        "{\"params\":[{\"key\":\"logged_in\",\"value\":\"1\"}]}]},"
        "\"contents\":{\"messageRenderer\":{\"text\":{\"simpleText\":\"Mock empty feed\"}}}}";
    CHECK(youtube_lite_feed_probe_classify(&budget, empty_feed, strlen(empty_feed), &result));
    CHECK(result.api_login_known && !result.api_logged_out && result.api_message
        && !result.api_signin_required && result.result_count == 0);
    const char *unrelated_login = "{\"contents\":{\"serviceTrackingParams\":["
        "{\"params\":[{\"key\":\"logged_in\",\"value\":\"1\"}]}]}}";
    CHECK(youtube_lite_feed_probe_classify(&budget, unrelated_login, strlen(unrelated_login), &result));
    CHECK(!result.api_login_known && !result.api_message);
    budget.limit = 1;
    CHECK(!youtube_lite_feed_probe_classify(&budget, signed_in, strlen(signed_in), &result));
    CHECK(budget.current == 0);
    return 0;
}
