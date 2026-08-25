#include <stdio.h>
#include <string.h>

#include <zlib.h>

#include "psp_hls_gzip.h"

static int failures;
#define CHECK(value) do {                                                    \
    if (!(value)) {                                                          \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #value);   \
        failures++;                                                          \
    }                                                                        \
} while (0)

static size_t make_gzip(
    const unsigned char *source, size_t source_length,
    unsigned char *output, size_t output_capacity)
{
    z_stream stream = {0};
    if (deflateInit2(&stream, Z_BEST_SPEED, Z_DEFLATED, MAX_WBITS + 16,
                     8, Z_DEFAULT_STRATEGY) != Z_OK) return 0;
    stream.next_in = (Bytef *) source;
    stream.avail_in = (uInt) source_length;
    stream.next_out = output;
    stream.avail_out = (uInt) output_capacity;
    int status = deflate(&stream, Z_FINISH);
    size_t length = status == Z_STREAM_END
        ? output_capacity - stream.avail_out : 0;
    (void) deflateEnd(&stream);
    return length;
}

static void test_incremental_gzip(void)
{
    static const unsigned char source[] =
        "#EXTM3U\n#EXT-X-MEDIA-SEQUENCE:900\n"
        "#EXTINF:2.0,\nsegment-900.ts\n"
        "#EXTINF:2.0,\nsegment-901.ts\n";
    unsigned char compressed[256];
    size_t compressed_length = make_gzip(
        source, sizeof(source) - 1u, compressed, sizeof(compressed));
    CHECK(compressed_length != 0);

    Budget budget;
    budget_init(&budget, 256u * 1024u);
    PspHlsGzip gzip = {0};
    CHECK(psp_hls_gzip_begin(&gzip, &budget));
    unsigned char decoded[sizeof(source) + 16u] = {0};
    size_t input_at = 0;
    size_t output_at = 0;
    unsigned iterations = 0;
    while (!gzip.finished && iterations++ < 256u) {
        size_t offered = compressed_length - input_at;
        if (offered > 7u) offered = 7u;
        size_t room = sizeof(decoded) - output_at;
        if (room > 11u) room = 11u;
        size_t consumed = 0;
        size_t produced = 0;
        int detail = 0;
        PspHlsGzipStatus status = psp_hls_gzip_pump(
            &gzip, compressed + input_at, offered,
            decoded + output_at, room, &consumed, &produced, &detail);
        CHECK(status == PSP_HLS_GZIP_PROGRESS
              || status == PSP_HLS_GZIP_FINISHED);
        CHECK(consumed != 0 || produced != 0);
        input_at += consumed;
        output_at += produced;
    }
    CHECK(gzip.finished);
    CHECK(input_at == compressed_length);
    CHECK(output_at == sizeof(source) - 1u);
    CHECK(memcmp(decoded, source, sizeof(source) - 1u) == 0);
    psp_hls_gzip_reset(&gzip);
    CHECK(budget.current == 0);
}

static void test_refusal_and_malformed_input(void)
{
    Budget tiny;
    budget_init(&tiny, 1u);
    PspHlsGzip gzip = {0};
    CHECK(!psp_hls_gzip_begin(&gzip, &tiny));
    CHECK(tiny.current == 0);

    Budget budget;
    budget_init(&budget, 256u * 1024u);
    CHECK(psp_hls_gzip_begin(&gzip, &budget));
    static const unsigned char malformed[] = "not gzip";
    unsigned char output[32];
    size_t consumed = 0;
    size_t produced = 0;
    int detail = 0;
    CHECK(psp_hls_gzip_pump(
              &gzip, malformed, sizeof(malformed) - 1u,
              output, sizeof(output), &consumed, &produced, &detail)
          == PSP_HLS_GZIP_MALFORMED);
    CHECK(detail == Z_DATA_ERROR);
    psp_hls_gzip_reset(&gzip);
    CHECK(budget.current == 0);
}

int main(void)
{
    test_incremental_gzip();
    test_refusal_and_malformed_input();
    if (failures != 0) return 1;
    puts("PSP HLS gzip tests passed");
    return 0;
}
