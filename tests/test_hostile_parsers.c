#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "tilefinch/budget.h"
#include "tilefinch/document.h"
#include "tilefinch/fetch.h"
#include "tilefinch/public_suffix.h"
#include "tilefinch/section_router.h"
#include "tilefinch/section_store.h"
#include "tilefinch/session.h"
#include "tilefinch/style.h"
#include "tilefinch/url.h"

#define MIB (1024u * 1024u)
#define MUTATION_CAPACITY 4096u

/* NUL-terminated printable-safe-length copy for the string-oriented parsers.
   Raw bytes (including control characters and high bytes, but never an
   embedded NUL) are preserved so the parsers see genuinely hostile input. */
static void mutated_cstr(const unsigned char *data, size_t length,
                         char *output, size_t capacity)
{
    size_t copied = length < capacity - 1 ? length : capacity - 1;
    for (size_t i = 0; i < copied; i++) {
        output[i] = data[i] == '\0' ? '.' : (char) data[i];
    }
    output[copied] = '\0';
}

/* Exercise the WHATWG-subset URL parser and everything that consumes a parsed
   URL: normalization, resolution against a valid base, schemeful site keys,
   and same-origin comparison. */
static void exercise_url_parser(const unsigned char *data, size_t length)
{
    char text[2100];
    mutated_cstr(data, length, text, sizeof(text));

    TilefinchUrl url;
    (void) tilefinch_url_parse(text, &url);
    (void) tilefinch_url_is_secure(&url);
    (void) tilefinch_url_potentially_trustworthy(text);
    (void) tilefinch_url_is_downgrade(text, "https://base.test/a");
    (void) tilefinch_url_same_origin(text, "https://base.test/a");

    char scratch[TILEFINCH_URL_SERIALIZED_LIMIT];
    (void) tilefinch_url_normalize(text, scratch, sizeof(scratch));
    (void) tilefinch_url_request_key(text, scratch, sizeof(scratch));
    (void) tilefinch_url_origin(text, scratch, sizeof(scratch));
    (void) tilefinch_url_site_key(text, scratch, sizeof(scratch));
    (void) tilefinch_url_resolve("https://base.test/dir/page", text,
                              scratch, sizeof(scratch));
}

/* Exercise the hand-rolled public-suffix DAFSA decoder and the registrable
   domain derivation layered on top of it. */
static void exercise_public_suffix(const unsigned char *data, size_t length)
{
    char host[512];
    mutated_cstr(data, length, host, sizeof(host));
    bool is_public = false;
    (void) tilefinch_public_suffix_classify(host, &is_public);
    char registrable[256];
    (void) tilefinch_registrable_domain(host, registrable, sizeof(registrable));
}

/* Feed the whole mutated buffer as a raw Set-Cookie field so attribute
   parsing (Domain/Path/Secure/HttpOnly/SameSite/Partitioned/Max-Age/Expires,
   the __Host-/__Secure- rules, and the public-suffix interaction) sees
   hostile bytes, across a secure and an insecure origin. */
static bool exercise_set_cookie(Budget *budget, const unsigned char *data,
                                size_t length, size_t iteration)
{
    char field[600];
    mutated_cstr(data, length, field, sizeof(field));
    const char *origin = (iteration & 1) == 0
        ? "https://cookies.fuzz.test/path"
        : "http://cookies.fuzz.test/path";
    BrowserSession session = {0};
    if (!browser_session_init(&session, budget, 64 * 1024)) return true;
    (void) browser_session_cookie_set_http(&session, origin, field);
    browser_session_destroy(&session);
    return true;
}

/* Drive the HTTP request-header validators (extra-header list, per-value
   grammar) through the public fetch_request_validate entry point. */
static void exercise_request_headers(const unsigned char *data, size_t length)
{
    char text[2100];
    mutated_cstr(data, length, text, sizeof(text));
    FetchRequest request = {
        .method = "GET",
        .extra_headers = text,
        .content_type = text,
        .referer = text,
        .accept = text,
        .user_agent = text
    };
    FetchRequestValidationError error = FETCH_REQUEST_VALIDATION_OK;
    (void) fetch_request_validate(&request, &error);
}

/* Write the mutated buffer as a replay trace.meta and drive the trace-format
   metadata parser end to end without any network. */
static bool exercise_trace_metadata(const unsigned char *data, size_t length)
{
    char dir[] = "/tmp/tilefinch-hostile-trace-XXXXXX";
    if (mkdtemp(dir) == NULL) return true;
    char meta_path[320];
    int written = snprintf(meta_path, sizeof(meta_path), "%s/trace.meta", dir);
    bool ok = true;
    if (written > 0 && (size_t) written < sizeof(meta_path)) {
        FILE *meta = fopen(meta_path, "wb");
        if (meta != NULL) {
            if (length != 0) (void) fwrite(data, 1, length, meta);
            (void) fclose(meta);
            char error[160];
            if (fetch_trace_replay_begin(dir, error, sizeof(error))) {
                fetch_trace_end();
            }
        }
        (void) remove(meta_path);
    }
    (void) rmdir(dir);
    return ok;
}

typedef struct {
    uint64_t state;
} TestRandom;

static uint64_t test_random_next(TestRandom *random)
{
    uint64_t value = random->state;
    value ^= value >> 12;
    value ^= value << 25;
    value ^= value >> 27;
    random->state = value;
    return value * UINT64_C(2685821657736338717);
}

static size_t test_random_bounded(TestRandom *random, size_t bound)
{
    return bound == 0 ? 0 : (size_t) (test_random_next(random) % bound);
}

static size_t mutate_input(TestRandom *random, unsigned char *data,
                           size_t length)
{
    static const unsigned char syntax[] = {
        0, '<', '>', '/', '!', '-', '\'', '"', '=', ';', ':', '{', '}',
        '[', ']', '(', ')', '&', '#', '\r', '\n', 0x7f, 0x80, 0xff
    };
    size_t mutations = 1 + test_random_bounded(random, 16);
    for (size_t mutation = 0; mutation < mutations; mutation++) {
        size_t operation = test_random_bounded(random, 4);
        if (operation == 0 && length != 0) {
            data[test_random_bounded(random, length)] =
                syntax[test_random_bounded(
                    random, sizeof(syntax) / sizeof(syntax[0]))];
        } else if (operation == 1 && length < MUTATION_CAPACITY) {
            size_t position = test_random_bounded(random, length + 1);
            memmove(data + position + 1, data + position, length - position);
            data[position] = syntax[test_random_bounded(
                random, sizeof(syntax) / sizeof(syntax[0]))];
            length++;
        } else if (operation == 2 && length != 0) {
            size_t position = test_random_bounded(random, length);
            size_t removed = 1 + test_random_bounded(
                random, length - position);
            memmove(data + position, data + position + removed,
                    length - position - removed);
            length -= removed;
        } else if (length != 0 && length < MUTATION_CAPACITY) {
            size_t source = test_random_bounded(random, length);
            size_t copied = 1 + test_random_bounded(
                random, length - source);
            if (copied > MUTATION_CAPACITY - length)
                copied = MUTATION_CAPACITY - length;
            memcpy(data + length, data + source, copied);
            length += copied;
        }
    }
    return length;
}

static bool exercise_input(const unsigned char *data, size_t length,
                           size_t iteration, uint64_t seed)
{
    Budget budget;
    budget_init(&budget, 12 * MIB);
    budget_install_lexbor(&budget);

    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    size_t chunk_size = length == 0 ? 1 : 1 + (iteration % length);
    if (document_parse(&document, &budget, (const char *) data, length,
                       chunk_size)) {
        (void) stylesheet_build(&stylesheet, &budget, &document, 480);
    }
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);

    SectionRouteStream route = {
        .budget = &budget,
        .byte_threshold = section_route_stream_threshold(&budget)
    };
    size_t offset = 0;
    while (offset < length) {
        size_t chunk = 1 + ((offset + iteration) % 31);
        if (chunk > length - offset) chunk = length - offset;
        section_route_stream_scan(&route, data + offset, chunk);
        offset += chunk;
    }
    section_route_stream_abort(&route);

    CompressedSectionStore store = {0};
    (void) section_store_build(&store, &budget, (const char *) data, length,
                               4096, 8192);
    section_store_destroy(&store);

    BrowserSession session = {0};
    if (browser_session_init(&session, &budget, 64 * 1024)) {
        char cookie[512] = "fuzz=";
        size_t copied = length;
        if (copied > sizeof(cookie) - 6) copied = sizeof(cookie) - 6;
        for (size_t i = 0; i < copied; i++) {
            unsigned char byte = data[i];
            cookie[5 + i] = byte >= 0x20 && byte < 0x7f
                ? (char) byte : (char) ('a' + byte % 26);
        }
        cookie[5 + copied] = '\0';
        (void) browser_session_cookie_set_http(
            &session, "https://fuzz.test/path", cookie);
        browser_session_destroy(&session);
    }

    exercise_url_parser(data, length);
    exercise_public_suffix(data, length);
    (void) exercise_set_cookie(&budget, data, length, iteration);
    exercise_request_headers(data, length);
    /* The trace-metadata target touches the filesystem and process-global
       trace state; sample it rather than run it every iteration. */
    if (iteration % 16 == 0) (void) exercise_trace_metadata(data, length);

    if (budget.current != 0) {
        fprintf(stderr,
                "hostile-parser leak seed=%" PRIu64
                " iteration=%zu bytes=%zu live=%zu\n",
                seed, iteration, length, budget.current);
        budget_dump_active(&budget, stderr, 16);
        return false;
    }
    return true;
}

static bool parse_u64(const char *text, uint64_t *value)
{
    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0') return false;
    *value = (uint64_t) parsed;
    return true;
}

static void usage(const char *program)
{
    fprintf(stderr, "usage: %s [--seed N] [--iterations N]\n", program);
}

/* Guard against a target that silently rejects every input: assert the new
   parsers accept representative valid cases and produce the expected result,
   so a regression that breaks acceptance fails the harness immediately. */
static bool known_answer_checks(void)
{
    TilefinchUrl url;
    if (!tilefinch_url_parse("https://a.example.com/p?q=1", &url)) return false;
    if (tilefinch_url_parse("http://exa mple.com/", &url)) return false;

    char site[TILEFINCH_URL_SERIALIZED_LIMIT];
    if (!tilefinch_url_site_key("https://www.example.com/x", site, sizeof(site))
        || strcmp(site, "https://example.com") != 0) {
        return false;
    }

    bool is_public = false;
    if (!tilefinch_public_suffix_classify("com", &is_public) || !is_public) {
        return false;
    }
    if (!tilefinch_public_suffix_classify("example.com", &is_public)
        || is_public) {
        return false;
    }
    char registrable[256];
    if (!tilefinch_registrable_domain("www.example.com", registrable,
                                   sizeof(registrable))
        || strcmp(registrable, "example.com") != 0) {
        return false;
    }

    Budget budget;
    budget_init(&budget, MIB);
    BrowserSession session = {0};
    bool cookies_ok = false;
    if (browser_session_init(&session, &budget, 64 * 1024)) {
        /* A plain cookie is accepted; a __Host- cookie with a Domain is a
           policy violation that must be rejected. */
        bool accepted = browser_session_cookie_set_http(
            &session, "https://ka.test/path", "id=1; Path=/");
        bool rejected = browser_session_cookie_set_http(
            &session, "https://ka.test/path",
            "__Host-x=1; Domain=ka.test; Path=/");
        cookies_ok = accepted && !rejected;
        browser_session_destroy(&session);
    }
    bool balanced = budget.current == 0;
    return cookies_ok && balanced;
}

int main(int argc, char **argv)
{
    if (!known_answer_checks()) {
        fprintf(stderr, "hostile-parser known-answer check failed\n");
        return 1;
    }
    static const char *const corpus[] = {
        "<!doctype html><style>@media(max-width:600px){.x{display:flex;"
        "--tone:#123456;color:var(--tone)}}</style><body><div class=x>ok",
        "<script>const x='</not-script>';if(a < b){x.replace(/<|>/g,'')}</script>"
        "<!--unterminated--><textarea>&amp;<fake></textarea>",
        "<table>text<tr><td colspan=999999>A<td>B</table>"
        "<svg><foreignObject><p style='width:calc(100% - 2px)'>x",
        "a=b; Domain=.example.test; Path=/; SameSite=None; Secure; Partitioned",
        "<html><head><style>@layer a,b;@supports(display:grid){*{display:grid}}"
        "</style></head><body><template><select><option>one<option>two",
        "<!doctype html><style>:root{--w:8197;--h:4611}.a{"
        "width:clamp(130px,39.4%,159px);"
        "flex:0 0 calc((100% - (3 * 16px))/3.5);"
        "padding-bottom:calc(100%*(var(--h)/var(--w)));"
        "height:calc(100% - 155px)}</style><div class=a></div>",
        "<!doctype html><style>.a{width:calc(calc(calc(calc(calc(calc("
        "calc(calc(calc(calc(calc(calc(1px))))))))))));"
        "height:min(1px,2px,3px,4px,5px,6px,7px,8px,9px,10px,11px,12px);"
        "max-width:calc(2px * 3px);min-width:calc(20px/0);"
        "padding:calc(1px + 2qu);flex:1 1 calc(2px*3px)}"
        "</style><div class=a></div>",
        "<!doctype html><style>:root{--a:var(--b);--b:var(--a)}.x{"
        "width:calc(var(--a) + 1px);height:calc(var(--missing)*2);"
        "padding:min(1px,2px,3px,4px,5px,6px,7px,8px,9px)}"
        "</style><div class=x></div>"
    };
    uint64_t seed = UINT64_C(0x535441474531);
    uint64_t iterations_u64 = 256;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            if (!parse_u64(argv[++i], &seed)) {
                usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
            if (!parse_u64(argv[++i], &iterations_u64)
                || iterations_u64 == 0 || iterations_u64 > 1000000) {
                usage(argv[0]);
                return 2;
            }
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    TestRandom random = {.state = seed != 0 ? seed : UINT64_C(1)};
    /* Always exercise every seed intact before mutation. This makes the
       bounded CSS-math and variable-cycle teardown cases deterministic. */
    for (size_t i = 0; i < sizeof(corpus) / sizeof(corpus[0]); i++) {
        if (!exercise_input((const unsigned char *) corpus[i],
                            strlen(corpus[i]), i, seed)) return 1;
    }
    for (size_t iteration = 0; iteration < (size_t) iterations_u64;
         iteration++) {
        const char *source = corpus[test_random_bounded(
            &random, sizeof(corpus) / sizeof(corpus[0]))];
        size_t length = strlen(source);
        unsigned char data[MUTATION_CAPACITY];
        memcpy(data, source, length);
        length = mutate_input(&random, data, length);
        if (!exercise_input(data, length, iteration, seed)) return 1;
    }

    printf("hostile-parser-harness: passed seed=%" PRIu64
           " iterations=%" PRIu64 "\n",
           seed, iterations_u64);
    return 0;
}
