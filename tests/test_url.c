#include "tilefinch/url.h"
#include "tilefinch/request_context.h"
#include "tilefinch/public_suffix.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "URL CHECK failed at %s:%d: %s\n",                 \
                __FILE__, __LINE__, #condition);                             \
        return 1;                                                            \
    }                                                                        \
} while (0)

int main(void)
{
    TilefinchUrl url;
    char output[TILEFINCH_URL_SERIALIZED_LIMIT];
    CHECK(tilefinch_url_parse("HTTPS://Example.COM/a?b#c", &url));
    CHECK(url.scheme == TILEFINCH_URL_SCHEME_HTTPS && url.port == 443);
    CHECK(url.path_length == 2 && url.query_length == 1
          && url.fragment_length == 1);
    CHECK(tilefinch_url_same_origin("https://example.com/a",
                                 "HTTPS://EXAMPLE.COM:443/b"));
    CHECK(!tilefinch_url_same_origin("https://example.com/",
                                  "https://example.com:444/"));
    CHECK(!tilefinch_url_same_origin("http://example.com/",
                                  "https://example.com/"));
    CHECK(tilefinch_url_is_downgrade("https://example.com/",
                                  "http://example.com/"));
    CHECK(tilefinch_url_upgrade_to_https(
              "http://Example.COM:80/a?b#c", output, sizeof(output))
          && strcmp(output, "https://example.com/a?b#c") == 0);
    CHECK(tilefinch_url_upgrade_to_https(
              "http://example.com:8080/a", output, sizeof(output))
          && strcmp(output, "https://example.com:8080/a") == 0);
    CHECK(!tilefinch_url_upgrade_to_https(
        "https://example.com/already", output, sizeof(output)));

    CHECK(tilefinch_url_potentially_trustworthy(
        "https://ordinary.example/path"));
    CHECK(tilefinch_url_potentially_trustworthy(
        "https://192.168.1.20/private"));
    CHECK(tilefinch_url_potentially_trustworthy(
        "http://localhost:8080/path"));
    CHECK(tilefinch_url_potentially_trustworthy(
        "http://fixture.localhost/path"));
    CHECK(tilefinch_url_potentially_trustworthy(
        "http://127.0.0.1:8080/path"));
    CHECK(tilefinch_url_potentially_trustworthy(
        "http://[::1]:8080/path"));
    CHECK(!tilefinch_url_potentially_trustworthy(
        "http://ordinary.example/path"));
    CHECK(!tilefinch_url_potentially_trustworthy(
        "http://192.168.1.20/private"));
    CHECK(!tilefinch_url_potentially_trustworthy(
        "http://127.evil/path"));

    CHECK(!tilefinch_url_origin("https://User:Pass@Example.COM:443/a", output,
                             sizeof(output)));
    CHECK(tilefinch_url_origin("http://[2001:DB8::1]:8080/a", output,
                            sizeof(output)));
    CHECK(strcmp(output, "http://[2001:db8::1]:8080") == 0);
    CHECK(tilefinch_url_parse("https://[::]/", &url));
    CHECK(tilefinch_url_parse("https://[::ffff:192.0.2.1]/", &url));
    CHECK(tilefinch_url_parse("https://[2001:db8:0:1:2:3:4:5]/", &url));

    CHECK(tilefinch_url_normalize(
        "HTTPS://Example.COM:443/a/./b/../c\\d?q=1#x", output,
        sizeof(output)));
    CHECK(strcmp(output, "https://example.com/a/c/d?q=1#x") == 0);
    CHECK(tilefinch_url_request_key(
        "HTTPS://Example.COM:443/a/../b?q=1#private",
        output, sizeof(output)));
    CHECK(strcmp(output, "https://example.com/b?q=1") == 0);
    CHECK(tilefinch_url_normalize("https://example.com/a//b", output,
                               sizeof(output)));
    CHECK(strcmp(output, "https://example.com/a//b") == 0);
    CHECK(tilefinch_url_normalize("https://example.com/a/.", output,
                               sizeof(output)));
    CHECK(strcmp(output, "https://example.com/a/") == 0);
    CHECK(tilefinch_url_normalize("https://example.com/a/b/..", output,
                               sizeof(output)));
    CHECK(strcmp(output, "https://example.com/a/") == 0);
    CHECK(tilefinch_url_normalize("https://example.com/a/%2e%2E/c", output,
                               sizeof(output)));
    CHECK(strcmp(output, "https://example.com/c") == 0);
    CHECK(tilefinch_url_resolve("https://example.com/a/b/index.html?old#x",
                             "../image.png?new#f", output, sizeof(output)));
    CHECK(strcmp(output, "https://example.com/a/image.png?new#f") == 0);
    CHECK(tilefinch_url_resolve("https://example.com/a/b?old#x", "?", output,
                             sizeof(output)));
    CHECK(strcmp(output, "https://example.com/a/b?") == 0);
    CHECK(tilefinch_url_resolve("https://example.com/a/b?old#x", "#", output,
                             sizeof(output)));
    CHECK(strcmp(output, "https://example.com/a/b?old#") == 0);
    CHECK(tilefinch_url_resolve("https://example.com/a/b?old#x", "", output,
                             sizeof(output)));
    CHECK(strcmp(output, "https://example.com/a/b?old") == 0);
    CHECK(tilefinch_url_resolve("https://example.com/a/", "//Other.test/x",
                             output, sizeof(output)));
    CHECK(strcmp(output, "https://other.test/x") == 0);

    CHECK(!tilefinch_url_parse("javascript:alert(1)", &url));
    CHECK(!tilefinch_url_parse("https:///missing-host", &url));
    CHECK(!tilefinch_url_parse("https://example.com:0/", &url));
    CHECK(!tilefinch_url_parse("https://example.com:70000/", &url));
    CHECK(!tilefinch_url_parse(
        "https://example.com:18446744073709552059/", &url));
    CHECK(!tilefinch_url_parse(
        "https://example.com:4294967739/", &url));
    CHECK(!tilefinch_url_parse("https://example.com/raw space", &url));
    CHECK(!tilefinch_url_parse("https://ex\xC3\xA4mple.test/", &url));
    CHECK(!tilefinch_url_parse("https://user:secret@example.com/", &url));
    CHECK(!tilefinch_url_parse("https://[::::]/", &url));
    CHECK(!tilefinch_url_parse("https://[1::2::3]/", &url));
    CHECK(!tilefinch_url_parse("https://[1:2:3:4:5:6:7]/", &url));
    CHECK(!tilefinch_url_parse("https://[1:2:3:4:5:6:7:8:9]/", &url));
    CHECK(!tilefinch_url_parse("https://[1:2:3:4:5:6:7::8]/", &url));
    CHECK(!tilefinch_url_parse("https://[::ffff:1.2.3.999]/", &url));
    CHECK(!tilefinch_url_parse("https://[::ffff:1.2.3]/", &url));

    bool is_public_suffix = false;
    CHECK(tilefinch_public_suffix_classify("com", &is_public_suffix)
          && is_public_suffix);
    CHECK(tilefinch_public_suffix_classify("co.uk", &is_public_suffix)
          && is_public_suffix);
    CHECK(tilefinch_public_suffix_classify("github.io", &is_public_suffix)
          && is_public_suffix);
    CHECK(tilefinch_public_suffix_classify("example.com", &is_public_suffix)
          && !is_public_suffix);
    CHECK(tilefinch_public_suffix_classify("foo.ck", &is_public_suffix)
          && is_public_suffix);
    CHECK(tilefinch_public_suffix_classify("www.ck", &is_public_suffix)
          && !is_public_suffix);
    CHECK(!tilefinch_public_suffix_classify("127.0.0.1", &is_public_suffix));
    CHECK(tilefinch_registrable_domain("www.example.co.uk", output,
                                    sizeof(output))
          && strcmp(output, "example.co.uk") == 0);
    CHECK(tilefinch_registrable_domain("a.city.kawasaki.jp", output,
                                    sizeof(output))
          && strcmp(output, "city.kawasaki.jp") == 0);
    CHECK(tilefinch_registrable_domain("tenant.blogspot.com", output,
                                    sizeof(output))
          && strcmp(output, "tenant.blogspot.com") == 0);
    CHECK(!tilefinch_registrable_domain("co.uk", output, sizeof(output)));
    CHECK(tilefinch_url_site_key("https://WWW.Example.COM:8443/path", output,
                              sizeof(output))
          && strcmp(output, "https://example.com") == 0);
    CHECK(tilefinch_url_site_key("http://127.0.0.1:8080/path", output,
                              sizeof(output))
          && strcmp(output, "http://127.0.0.1") == 0);

    TilefinchRequestContext request = {
        .target_url = "https://api.example.test/data",
        .initiator_url = "https://app.example.test/page",
        .top_level_url = "https://app.example.test/page",
        .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_CORS,
        .credentials = TILEFINCH_CREDENTIALS_SAME_ORIGIN,
        .destination = TILEFINCH_DESTINATION_FETCH
    };
    CHECK(tilefinch_request_context_valid(&request));
    TilefinchRequestFacts facts;
    CHECK(tilefinch_request_context_analyze(&request, &facts));
    CHECK(facts.source == &request && facts.valid
          && !facts.same_origin && facts.same_site
          && facts.safe_method && !facts.sends_credentials
          && facts.allows_lax_cookie
          && facts.site == TILEFINCH_REQUEST_SITE_SAME_SITE
          && strcmp(tilefinch_request_facts_fetch_site(&facts),
                    tilefinch_request_fetch_site(&request)) == 0);
    CHECK(!tilefinch_request_same_origin(&request));
    CHECK(tilefinch_request_same_site(&request));
    CHECK(!tilefinch_request_sends_credentials(&request));
    CHECK(strcmp(tilefinch_request_fetch_site(&request), "same-site") == 0);
    request.target_url = "http://api.example.test/data";
    CHECK(!tilefinch_request_same_site(&request));
    request.target_url = "https://bob.github.io/data";
    request.top_level_url = "https://alice.github.io/page";
    CHECK(!tilefinch_request_same_site(&request));
    request.top_level_url = "https://app.example.test/page";
    request.target_url = "https://app.example.test/api";
    CHECK(tilefinch_request_same_origin(&request));
    CHECK(tilefinch_request_sends_credentials(&request));
    request.initiator_url = "HTTPS://APP.EXAMPLE.TEST/page";
    request.top_level_url = request.initiator_url;
    request.target_url = "https://app.example.test:443/default-port";
    CHECK(tilefinch_request_same_origin(&request));
    CHECK(tilefinch_request_sends_credentials(&request));
    request.target_url = "https://other.test/data";
    CHECK(!tilefinch_request_sends_credentials(&request));
    request.credentials = TILEFINCH_CREDENTIALS_INCLUDE;
    CHECK(tilefinch_request_sends_credentials(&request));
    request.credentials = TILEFINCH_CREDENTIALS_OMIT;
    CHECK(!tilefinch_request_sends_credentials(&request));
    request.credentials = (TilefinchCredentialsMode) 99;
    CHECK(!tilefinch_request_context_valid(&request));
    CHECK(!tilefinch_request_context_analyze(&request, &facts)
          && facts.source == &request && !facts.valid);
    CHECK(!tilefinch_request_sends_credentials(&request));
    request.credentials = TILEFINCH_CREDENTIALS_SAME_ORIGIN;
    request.target_url = "https://other.test/submit";
    request.mode = TILEFINCH_REQUEST_MODE_NAVIGATE;
    request.destination = TILEFINCH_DESTINATION_DOCUMENT;
    request.top_level_navigation = true;
    CHECK(tilefinch_request_allows_lax_cookie(&request));
    CHECK(tilefinch_request_context_analyze(&request, &facts)
          && facts.allows_lax_cookie && facts.safe_method);
    request.method = "POST";
    CHECK(!tilefinch_request_allows_lax_cookie(&request));
    CHECK(tilefinch_request_context_analyze(&request, &facts)
          && !facts.allows_lax_cookie && !facts.safe_method);
    puts("url-tests status=PASS");
    return 0;
}
