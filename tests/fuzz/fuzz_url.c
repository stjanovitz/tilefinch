/* libFuzzer target: URL parsing/normalization/resolution and data: URLs.
   Input layout: byte 0 selects a base URL, the remaining bytes are the URL
   text under test (also used as a base and as a data: URL payload). */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "data_url.h"
#include "tilefinch/budget.h"
#include "tilefinch/public_suffix.h"
#include "tilefinch/url.h"

static const char *const bases[] = {
    "https://base.test/dir/page",
    "http://example.com",
    "https://a.b.example.co.uk:8443/x/y/z/?q=1#frag",
    "http://[::1]:8080/a/b",
    "https://127.0.0.1/%2e%2e/./a",
    "http://h/?",
    "https://h/#",
    "https://localhost/a\\b\\c",
};

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0 || size > 4096) return 0;
    const char *base = bases[data[0] % (sizeof(bases) / sizeof(bases[0]))];
    data++;
    size--;

    /* Exact-size heap copy so any read past the end trips ASan. */
    char *text = malloc(size + 1);
    if (text == NULL) return 0;
    memcpy(text, data, size);
    text[size] = '\0';

    TilefinchUrl url;
    if (tilefinch_url_parse(text, &url)) {
        (void) tilefinch_url_is_secure(&url);
        if (url.host_length != 0)
            (void) tilefinch_host_within(text + url.host_offset,
                                         url.host_length, "example.com", 11);
    }
    (void) tilefinch_url_potentially_trustworthy(text);
    (void) tilefinch_url_is_downgrade(text, base);
    (void) tilefinch_url_is_downgrade(base, text);
    (void) tilefinch_url_same_origin(text, base);

    char out[TILEFINCH_URL_SERIALIZED_LIMIT];
    char small[24];
    (void) tilefinch_url_normalize(text, out, sizeof(out));
    (void) tilefinch_url_normalize(text, small, sizeof(small));
    (void) tilefinch_url_request_key(text, out, sizeof(out));
    (void) tilefinch_url_origin(text, out, sizeof(out));
    (void) tilefinch_url_origin(text, small, sizeof(small));
    (void) tilefinch_url_site_key(text, out, sizeof(out));
    (void) tilefinch_url_upgrade_to_https(text, out, sizeof(out));
    (void) tilefinch_url_resolve(base, text, out, sizeof(out));
    (void) tilefinch_url_resolve(base, text, small, sizeof(small));
    (void) tilefinch_url_resolve(text, "../x/./y?z#w", out, sizeof(out));
    (void) tilefinch_url_resolve(text, "", out, sizeof(out));
    (void) tilefinch_url_resolve(text, "#f", out, sizeof(out));
    (void) tilefinch_url_resolve(text, "?q", out, sizeof(out));
    (void) tilefinch_url_resolve(text, "//other.test/p", out, sizeof(out));
    (void) tilefinch_referrer_policy_code(text, size, true);
    (void) tilefinch_referrer_policy_code(text, size, false);

    bool is_public = false;
    (void) tilefinch_public_suffix_classify(text, &is_public);
    char registrable[256];
    (void) tilefinch_registrable_domain(text, registrable, sizeof(registrable));
    (void) tilefinch_registrable_domain(text, small, sizeof(small));

    /* data: URL decoding sees the raw bytes (embedded NULs allowed) with
       no terminator. */
    char *raw = malloc(size + 5);
    if (raw != NULL) {
        memcpy(raw, "data:", 5);
        memcpy(raw + 5, data, size);
        Budget budget;
        budget_init(&budget, 1024 * 1024);
        unsigned char *decoded = NULL;
        size_t decoded_length = 0;
        char media_type[64];
        size_t limits[2] = {size, 16};
        for (int i = 0; i < 2; i++) {
            DataUrlDecodeResult result = data_url_decode(
                &budget, raw, size + 5, limits[i], &decoded,
                &decoded_length, media_type, sizeof(media_type));
            if (result == DATA_URL_DECODED) {
                if (decoded_length > limits[i]) abort();
                budget_free(&budget, decoded);
            }
            /* The input may already carry its own "data:" prefix. */
            result = data_url_decode(&budget, (const char *) data, size,
                                     limits[i], &decoded, &decoded_length,
                                     media_type, sizeof(media_type));
            if (result == DATA_URL_DECODED) {
                if (decoded_length > limits[i]) abort();
                budget_free(&budget, decoded);
            }
        }
        if (budget.current != 0) abort();
        free(raw);
    }
    free(text);
    return 0;
}
