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
    /* The HTTPS-first attempt keeps every explicit port but HTTP's own:
       a station at :8000 is tried at https://host:8000 (as on run 7's
       SHOUTcast server), and :80 becomes HTTPS's default. */
    static const char *const upgrades[][2] = {
        {"http://shoutcast.example:8000/", "https://shoutcast.example:8000/"},
        {"http://shoutcast.example:8000?fromyp=true",
         "https://shoutcast.example:8000/?fromyp=true"},
        {"http://radio.example:80/;", "https://radio.example/;"},
        {"http://radio.example/live", "https://radio.example/live"},
        {"http://radio.example:443/live", "https://radio.example/live"},
        {"http://radio.example:8443/live", "https://radio.example:8443/live"},
        {"http://[2001:db8::1]:8000/a", "https://[2001:db8::1]:8000/a"}
    };
    for (size_t i = 0; i < sizeof(upgrades) / sizeof(upgrades[0]); i++) {
        CHECK(tilefinch_url_upgrade_to_https(
                  upgrades[i][0], output, sizeof(output))
              && strcmp(output, upgrades[i][1]) == 0);
    }

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
    /* Loopback spelled the ways the resolver accepts is still loopback. */
    CHECK(tilefinch_url_potentially_trustworthy("http://2130706433/"));
    CHECK(tilefinch_url_potentially_trustworthy("http://0x7f.1/"));
    CHECK(tilefinch_url_potentially_trustworthy("http://127.1/"));
    CHECK(!tilefinch_url_potentially_trustworthy("http://134744072/"));
    CHECK(!tilefinch_url_potentially_trustworthy("http://127.0.0.256/"));

    /* IPv4 hosts as inet_aton, curl and the URL Standard read them. */
    static const struct {
        const char *host;
        TilefinchHostIpv4 kind;
        unsigned char address[4];
    } ipv4_hosts[] = {
        {"8.8.8.8", TILEFINCH_HOST_IPV4_ADDRESS, {8, 8, 8, 8}},
        {"134744072", TILEFINCH_HOST_IPV4_ADDRESS, {8, 8, 8, 8}},
        {"0x08080808", TILEFINCH_HOST_IPV4_ADDRESS, {8, 8, 8, 8}},
        {"0X8080808", TILEFINCH_HOST_IPV4_ADDRESS, {8, 8, 8, 8}},
        {"8.526344", TILEFINCH_HOST_IPV4_ADDRESS, {8, 8, 8, 8}},
        {"8.8.2056", TILEFINCH_HOST_IPV4_ADDRESS, {8, 8, 8, 8}},
        {"010.010.010.010", TILEFINCH_HOST_IPV4_ADDRESS, {8, 8, 8, 8}},
        {"0xc0.0250.1.1", TILEFINCH_HOST_IPV4_ADDRESS, {192, 168, 1, 1}},
        {"4294967295", TILEFINCH_HOST_IPV4_ADDRESS, {255, 255, 255, 255}},
        {"0", TILEFINCH_HOST_IPV4_ADDRESS, {0, 0, 0, 0}},
        {"0x", TILEFINCH_HOST_IPV4_ADDRESS, {0, 0, 0, 0}},
        {"1.2.3.4.", TILEFINCH_HOST_IPV4_ADDRESS, {1, 2, 3, 4}},
        {"4294967296", TILEFINCH_HOST_IPV4_INVALID, {0}},
        {"99999999999999999999", TILEFINCH_HOST_IPV4_INVALID, {0}},
        {"08", TILEFINCH_HOST_IPV4_INVALID, {0}},
        {"1.2.3.4.5", TILEFINCH_HOST_IPV4_INVALID, {0}},
        {"256.1.1.1", TILEFINCH_HOST_IPV4_INVALID, {0}},
        {"1.2.3.256", TILEFINCH_HOST_IPV4_INVALID, {0}},
        {"1.16777216", TILEFINCH_HOST_IPV4_INVALID, {0}},
        {"example.123", TILEFINCH_HOST_IPV4_INVALID, {0}},
        {"1.0xg", TILEFINCH_HOST_IPV4_NAME, {0}},
        {"example.com", TILEFINCH_HOST_IPV4_NAME, {0}},
        {"intranet", TILEFINCH_HOST_IPV4_NAME, {0}},
        {"127.evil", TILEFINCH_HOST_IPV4_NAME, {0}},
        {"1e5", TILEFINCH_HOST_IPV4_NAME, {0}},
        {"0x1g", TILEFINCH_HOST_IPV4_NAME, {0}}
    };
    for (size_t i = 0; i < sizeof(ipv4_hosts) / sizeof(ipv4_hosts[0]); i++) {
        unsigned char address[4] = {0};
        TilefinchHostIpv4 kind = tilefinch_host_ipv4(
            ipv4_hosts[i].host, strlen(ipv4_hosts[i].host), address);
        if (kind != ipv4_hosts[i].kind
            || (kind == TILEFINCH_HOST_IPV4_ADDRESS
                && memcmp(address, ipv4_hosts[i].address, 4) != 0)) {
            fprintf(stderr, "IPv4 host %s: kind %d address %u.%u.%u.%u\n",
                    ipv4_hosts[i].host, (int) kind, address[0], address[1],
                    address[2], address[3]);
            return 1;
        }
    }

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
    /* An absolute reference ignores the base, even one that cannot be a
       base; a relative reference against such a base fails. */
    CHECK(tilefinch_url_resolve("blob:https://example.com/uuid",
                             "https://example.com/a.js", output,
                             sizeof(output)));
    CHECK(strcmp(output, "https://example.com/a.js") == 0);
    CHECK(tilefinch_url_resolve("about:blank", "http://example.com/b",
                             output, sizeof(output)));
    CHECK(strcmp(output, "http://example.com/b") == 0);
    CHECK(!tilefinch_url_resolve("blob:https://example.com/uuid", "a.js",
                              output, sizeof(output)));
    CHECK(!tilefinch_url_resolve("about:blank", "", output, sizeof(output)));

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

    /* libpsl's decoder needs only the bytes an offset's tag names: a graph
       may end in a one-byte offset list and a return value. Graph for the
       single key "a" (value 0): root list, end-char 'a', child list, value. */
    static const unsigned char short_tail[] = {0x81, 0xe1, 0x81, 0x80};
    CHECK(tilefinch_public_suffix_test_lookup(
              short_tail, sizeof(short_tail), "a") == 0
          && tilefinch_public_suffix_test_lookup(
              short_tail, sizeof(short_tail), "b") == -1);
    /* A two-byte offset needs both bytes; a truncated one is refused. */
    static const unsigned char truncated[] = {0x81, 0xe1, 0x40};
    CHECK(tilefinch_public_suffix_test_lookup(
              truncated, sizeof(truncated), "a") == -1);
    bool is_public_suffix = false;
    CHECK(tilefinch_public_suffix_classify("com", &is_public_suffix)
          && is_public_suffix);
    CHECK(tilefinch_public_suffix_classify("co.uk", &is_public_suffix)
          && is_public_suffix);
    CHECK(tilefinch_public_suffix_classify("github.io", &is_public_suffix)
          && is_public_suffix);
    CHECK(tilefinch_public_suffix_classify("gov.de", &is_public_suffix)
          && is_public_suffix);
    CHECK(tilefinch_public_suffix_classify("cloudzyro.app", &is_public_suffix)
          && is_public_suffix);
    CHECK(tilefinch_public_suffix_classify("newtenant.replit.dev", &is_public_suffix)
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
    request.top_level_url = "https://chatgpt.com/";
    request.initiator_url =
        "https://challenges.cloudflare.com/challenge-frame";
    request.target_url =
        "https://brunhild.challenges.cloudflare.com/probe";
    request.credentials = TILEFINCH_CREDENTIALS_INCLUDE;
    CHECK(tilefinch_request_context_analyze(&request, &facts)
          && !facts.same_origin && !facts.same_site
          && !facts.allows_lax_cookie
          && facts.site == TILEFINCH_REQUEST_SITE_SAME_SITE
          && strcmp(tilefinch_request_facts_fetch_site(&facts),
                    "same-site") == 0);
    CHECK(!tilefinch_request_same_site(&request));
    request.initiator_url = "https://app.example.test/page";
    request.top_level_url = "https://app.example.test/page";
    request.credentials = TILEFINCH_CREDENTIALS_SAME_ORIGIN;
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
    /* Host-within: equal, or a subdomain at a '.' boundary, any case. */
    CHECK(tilefinch_host_within("example.com", 11, "example.com", 11));
    CHECK(tilefinch_host_within("a.Example.COM", 13, "example.com", 11));
    CHECK(!tilefinch_host_within("badexample.com", 14, "example.com", 11));
    CHECK(!tilefinch_host_within("example.com", 11, "a.example.com", 13));
    CHECK(tilefinch_host_within("a.example.com:80", 13, "example.com", 11));
    CHECK(!tilefinch_host_within(NULL, 0, "example.com", 11));
    /* Referrer-Policy codes are stored, so their order is pinned. */
    CHECK(tilefinch_referrer_policy_code("", 0, false) == 0);
    CHECK(strcmp(tilefinch_referrer_policy_name(0), "") == 0);
    CHECK(tilefinch_referrer_policy_code("no-referrer", 11, false) == 1);
    CHECK(tilefinch_referrer_policy_code("unsafe-url", 10, false)
          == TILEFINCH_REFERRER_POLICY_COUNT);
    CHECK(tilefinch_referrer_policy_code("Strict-Origin", 13, false)
          == TILEFINCH_REFERRER_POLICY_UNKNOWN);
    CHECK(tilefinch_referrer_policy_code("Strict-Origin", 13, true) == 6);
    /* Exact spans only: a prefix of a longer token is not that token. */
    CHECK(tilefinch_referrer_policy_code("origin-when", 6, false) == 3);
    CHECK(tilefinch_referrer_policy_code("origin", 5, false)
          == TILEFINCH_REFERRER_POLICY_UNKNOWN);
    for (uint8_t code = 0; code <= TILEFINCH_REFERRER_POLICY_COUNT; code++) {
        const char *name = tilefinch_referrer_policy_name(code);
        CHECK(name != NULL
              && tilefinch_referrer_policy_code(name, strlen(name), false)
                     == code);
    }
    CHECK(tilefinch_referrer_policy_name(TILEFINCH_REFERRER_POLICY_COUNT + 1u)
          == NULL);
    /* NW-08: an IPv4 host is serialized as its dotted quad however it is
       spelled, so origin, site key and cookie scope are per address. Two
       machines whose spellings share "0x1.1" are different sites, and
       127.1 is the origin 127.0.0.1. */
    static const char *const canonical_hosts[][2] = {
        {"http://127.1/a", "http://127.0.0.1/a"},
        {"http://0x7F.0.0.1:8080/x?y#z", "http://127.0.0.1:8080/x?y#z"},
        {"http://2130706433/", "http://127.0.0.1/"},
        {"http://192.168.0x1.1/", "http://192.168.1.1/"},
        {"https://010.010.010.010/", "https://8.8.8.8/"},
        {"http://8.8.8.8/", "http://8.8.8.8/"},
        {"http://Example.123/", "http://example.123/"},
        {"http://1.0xg/", "http://1.0xg/"},
        {"http://[2001:db8::1]/", "http://[2001:db8::1]/"}
    };
    for (size_t i = 0;
         i < sizeof(canonical_hosts) / sizeof(canonical_hosts[0]); i++) {
        if (!tilefinch_url_normalize(canonical_hosts[i][0], output,
                                     sizeof(output))
            || strcmp(output, canonical_hosts[i][1]) != 0)
            fprintf(stderr, "normalize %s -> %s\n", canonical_hosts[i][0],
                    output);
        CHECK(tilefinch_url_normalize(canonical_hosts[i][0], output,
                                      sizeof(output))
              && strcmp(output, canonical_hosts[i][1]) == 0);
    }
    CHECK(tilefinch_url_origin("http://127.1:8080/x", output, sizeof(output))
          && strcmp(output, "http://127.0.0.1:8080") == 0);
    CHECK(tilefinch_url_same_origin("http://127.1/", "http://127.0.0.1/"));
    CHECK(tilefinch_url_same_origin("http://0x7f000001/a", "http://127.1/b"));
    CHECK(!tilefinch_url_same_origin("http://127.1/", "http://127.0.0.2/"));
    char other_site[TILEFINCH_URL_SERIALIZED_LIMIT];
    CHECK(tilefinch_url_site_key("http://192.168.0x1.1/", output,
                                 sizeof(output))
          && tilefinch_url_site_key("http://10.9.0x1.1/", other_site,
                                    sizeof(other_site))
          && strcmp(output, "http://192.168.1.1") == 0
          && strcmp(other_site, "http://10.9.1.1") == 0);

    puts("url-tests status=PASS");
    return 0;
}
