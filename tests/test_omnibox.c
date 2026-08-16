#include "tilefinch/omnibox.h"

#include <stdio.h>
#include <string.h>

#define CHECK(value) do { \
    if (!(value)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #value); \
        return 1; \
    } \
} while (0)

static bool resolves(
    const char *input, BrowserSearchEngine engine, const char *expected)
{
    char output[512];
    return browser_omnibox_resolve(
               input, engine, output, sizeof(output))
        && strcmp(output, expected) == 0;
}

static bool resolves_kind(
    const char *input, BrowserSearchEngine engine, const char *expected,
    BrowserOmniboxResolutionKind expected_kind)
{
    char output[512];
    BrowserOmniboxResolutionKind kind = BROWSER_OMNIBOX_NAVIGATION;
    return browser_omnibox_resolve_kind(
               input, engine, output, sizeof(output), &kind)
        && strcmp(output, expected) == 0 && kind == expected_kind;
}

int main(void)
{
    CHECK(resolves(
        " https://example.test/a?q=one ", BROWSER_SEARCH_GOOGLE,
        "https://example.test/a?q=one"));
    CHECK(resolves(
        "en.wikipedia.org/wiki/PSP", BROWSER_SEARCH_GOOGLE,
        "https://en.wikipedia.org/wiki/PSP"));
    CHECK(resolves(
        "localhost:8080/test", BROWSER_SEARCH_GOOGLE,
        "https://localhost:8080/test"));
    CHECK(resolves_kind(
        "[2001:db8::1]:8080/test", BROWSER_SEARCH_GOOGLE,
        "https://[2001:db8::1]:8080/test",
        BROWSER_OMNIBOX_NAVIGATION));
    CHECK(resolves_kind(
        "127.0.0.1:8080/test", BROWSER_SEARCH_GOOGLE,
        "https://127.0.0.1:8080/test",
        BROWSER_OMNIBOX_NAVIGATION));
    CHECK(resolves_kind(
        "xn--bcher-kva.example/a", BROWSER_SEARCH_GOOGLE,
        "https://xn--bcher-kva.example/a",
        BROWSER_OMNIBOX_NAVIGATION));
    CHECK(resolves_kind(
        "HTTPS://Example.Test:443/a/../b", BROWSER_SEARCH_GOOGLE,
        "https://example.test/b",
        BROWSER_OMNIBOX_NAVIGATION));
    CHECK(resolves(
        "portable web browser", BROWSER_SEARCH_GOOGLE,
        "https://www.google.com/search?q=portable%20web%20browser"));
    CHECK(resolves(
        "portable & fast", BROWSER_SEARCH_BING,
        "https://www.bing.com/search?q=portable%20%26%20fast"));
    CHECK(resolves(
        "PSP homebrew", BROWSER_SEARCH_DUCKDUCKGO,
        "https://duckduckgo.com/?q=PSP%20homebrew"));
    CHECK(resolves_kind(
        "http://bad host/path", BROWSER_SEARCH_GOOGLE,
        "https://www.google.com/search?q=http%3A%2F%2Fbad%20host%2Fpath",
        BROWSER_OMNIBOX_SEARCH));
    CHECK(resolves_kind(
        "ftp://example.test/file", BROWSER_SEARCH_GOOGLE,
        "https://www.google.com/search?q=ftp%3A%2F%2Fexample.test%2Ffile",
        BROWSER_OMNIBOX_SEARCH));
    CHECK(resolves_kind(
        "example.test.:443", BROWSER_SEARCH_GOOGLE,
        "https://www.google.com/search?q=example.test.%3A443",
        BROWSER_OMNIBOX_SEARCH));
    CHECK(resolves_kind(
        "example.test:99999", BROWSER_SEARCH_GOOGLE,
        "https://www.google.com/search?q=example.test%3A99999",
        BROWSER_OMNIBOX_SEARCH));
    CHECK(resolves_kind(
        "bücher.example", BROWSER_SEARCH_GOOGLE,
        "https://www.google.com/search?q=b%C3%BCcher.example",
        BROWSER_OMNIBOX_SEARCH));
    CHECK(strcmp(
        browser_search_engine_name(BROWSER_SEARCH_DUCKDUCKGO),
        "DUCKDUCKGO") == 0);

    char tiny[8] = "dirty";
    CHECK(!browser_omnibox_resolve(
        "this will not fit", BROWSER_SEARCH_GOOGLE, tiny, sizeof(tiny)));
    CHECK(tiny[0] == '\0');
    char empty[8] = "dirty";
    CHECK(!browser_omnibox_resolve(
        "   ", BROWSER_SEARCH_GOOGLE, empty, sizeof(empty)));
    CHECK(empty[0] == '\0');
    puts("omnibox-tests: ok");
    return 0;
}
