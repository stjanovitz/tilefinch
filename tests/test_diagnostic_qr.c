#include "tilefinch/diagnostic_qr.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <zlib.h>

static int base45_value(char character)
{
    static const char alphabet[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:";
    const char *found = strchr(alphabet, character);
    return found == NULL ? -1 : (int) (found - alphabet);
}

static size_t base45_decode(
    const char *text, uint8_t *output, size_t capacity)
{
    size_t length = strlen(text);
    size_t used = 0u;
    size_t at = 0u;
    while (at + 2u < length) {
        int a = base45_value(text[at]);
        int b = base45_value(text[at + 1u]);
        int c = base45_value(text[at + 2u]);
        assert(a >= 0 && b >= 0 && c >= 0);
        unsigned value = (unsigned) a + (unsigned) b * 45u
            + (unsigned) c * 45u * 45u;
        assert(value <= 65535u && used + 2u <= capacity);
        output[used++] = (uint8_t) (value >> 8);
        output[used++] = (uint8_t) value;
        at += 3u;
    }
    if (at < length) {
        assert(at + 1u < length);
        int a = base45_value(text[at]);
        int b = base45_value(text[at + 1u]);
        assert(a >= 0 && b >= 0);
        unsigned value = (unsigned) a + (unsigned) b * 45u;
        assert(value <= 255u && used < capacity);
        output[used++] = (uint8_t) value;
        at += 2u;
    }
    assert(at == length);
    return used;
}

static uint32_t read_u32(const uint8_t *bytes)
{
    return ((uint32_t) bytes[0] << 24)
        | ((uint32_t) bytes[1] << 16)
        | ((uint32_t) bytes[2] << 8)
        | bytes[3];
}

static char *make_file(const uint8_t *bytes, size_t size)
{
    char *path = malloc(64u);
    assert(path != NULL);
    snprintf(path, 64u, "/tmp/tilefinch-diagnostic-XXXXXX");
    int descriptor = mkstemp(path);
    assert(descriptor >= 0);
    size_t written = 0u;
    while (written < size) {
        ssize_t result = write(descriptor, bytes + written, size - written);
        assert(result > 0);
        written += (size_t) result;
    }
    assert(close(descriptor) == 0);
    return path;
}

static const char *page_data(const char *page)
{
    const char *at = page;
    unsigned total = strncmp(page, "TFD2:", 5u) == 0 ? 8u : 6u;
    for (unsigned separators = 0u; separators < total; separators++) {
        at = strchr(at, ':');
        assert(at != NULL);
        at++;
    }
    return at;
}

static uint8_t *decode_current_part(
    TilefinchDiagnosticQrReport *report, size_t *raw_length)
{
    const TilefinchDiagnosticQrView *view = tilefinch_diagnostic_qr_view(report);
    uint8_t *compressed = malloc(view->compressed_bytes);
    uint8_t *raw = malloc(view->raw_bytes);
    assert(compressed != NULL && raw != NULL);
    size_t compressed_used = 0u;
    for (unsigned page = 0u; page < view->page_count; page++) {
        assert(tilefinch_diagnostic_qr_select_page(report, page));
        const char *text = tilefinch_diagnostic_qr_page_text(report);
        assert(text != NULL && strncmp(text, "TFD2:", 5u) == 0);
        compressed_used += base45_decode(
            page_data(text), compressed + compressed_used,
            view->compressed_bytes - compressed_used);
    }
    uLongf size = (uLongf) view->raw_bytes;
    assert(uncompress(raw, &size, compressed, compressed_used) == Z_OK);
    assert(size == view->raw_bytes);
    free(compressed);
    *raw_length = (size_t) size;
    return raw;
}

static void test_base45_vectors(void)
{
    char output[64];
    size_t length = 0u;
    assert(tilefinch_base45_encode(
        (const uint8_t *) "AB", 2u, output, sizeof(output), &length));
    assert(length == 3u && strcmp(output, "BB8") == 0);
    assert(tilefinch_base45_encode(
        (const uint8_t *) "base-45", 7u, output, sizeof(output), &length));
    assert(strcmp(output, "UJCLQE7W581") == 0);
}

static void test_bundle_round_trip(void)
{
    static const char error_log[] =
        "tilefinch-device-error-v2\n"
        "version=0.1.4\n"
        "stage=navigation\n"
        "http=0\n"
        "native=0x80110601\n"
        "detail=check-profile\n";
    uint8_t extra[3500];
    uint32_t random = UINT32_C(0x51A7D10C);
    for (size_t at = 0; at < sizeof(extra); at++) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        extra[at] = (uint8_t) random;
    }
    char *error_path = make_file(
        (const uint8_t *) error_log, sizeof(error_log) - 1u);
    char *extra_path = make_file(extra, sizeof(extra));
    TilefinchDiagnosticSource sources[] = {
        {"tilefinch-last-error.txt", error_path},
        {"tilefinch-validation.txt", extra_path},
        {"missing.txt", "/tmp/tilefinch-diagnostic-does-not-exist"}
    };
    TilefinchDiagnosticMetadata metadata = {
        .app_version = "0.1.4",
        .release_sequence = 6u,
        .created_unix_time = UINT64_C(1787006559),
        .psp_model = 2u,
        .psp_firmware = UINT32_C(0x06060110)
    };
    char error[96];
    TilefinchDiagnosticQrReport *report = tilefinch_diagnostic_qr_build(
        &metadata, sources, sizeof(sources) / sizeof(sources[0]),
        error, sizeof(error));
    assert(report != NULL);
    assert(error[0] == '\0');
    const TilefinchDiagnosticQrView *view = tilefinch_diagnostic_qr_view(report);
    assert(view != NULL);
    assert(view->module_count == 125u);
    assert(view->part_count == 2u && view->part_index == 0u);
    assert(strcmp(view->device, "PSP-3000") == 0);
    assert(strcmp(view->firmware, "6.61") == 0);
    assert(strcmp(view->error_summary, "navigation / 0x80110601") == 0);
    assert(tilefinch_diagnostic_qr_module(view, 0u, 0u));
    assert(tilefinch_diagnostic_qr_module(view, 6u, 0u));
    assert(!tilefinch_diagnostic_qr_module(view, 7u, 0u));

    size_t raw_size = 0u;
    uint8_t *raw = decode_current_part(report, &raw_size);
    assert(memcmp(raw, "TFDG", 4u) == 0);
    assert(raw[4] == 2u && raw[5] == 1u && raw[6] == 0u);
    uint32_t report_id = read_u32(raw + 8u);
    char report_text[9];
    snprintf(report_text, sizeof(report_text), "%08" PRIX32, report_id);
    assert(strcmp(report_text, view->report_id) == 0);
    assert(read_u32(raw + 36u) == 0u && read_u32(raw + 40u) == 2u);
    size_t at = 45u + raw[44];
    assert(raw[at++] == strlen("tilefinch-last-error.txt"));
    assert(raw[at++] == 0u && raw[at++] == 0u && raw[at++] == 0u);
    uint32_t first_original = read_u32(raw + at); at += 4u;
    uint32_t first_offset = read_u32(raw + at); at += 4u;
    uint32_t first_size = read_u32(raw + at); at += 4u;
    uint32_t first_crc = read_u32(raw + at); at += 4u;
    assert(first_original == first_size && first_offset == 0u);
    assert(first_size == sizeof(error_log) - 1u);
    assert(memcmp(raw + at, "tilefinch-last-error.txt",
                  strlen("tilefinch-last-error.txt")) == 0);
    at += strlen("tilefinch-last-error.txt");
    assert(first_crc == (uint32_t) crc32(0L, raw + at, first_size));
    assert(memcmp(raw + at, error_log, first_size) == 0);
    at += first_size;
    assert(at == raw_size);
    free(raw);
    assert(tilefinch_diagnostic_qr_select_part(
        report, 1u, error, sizeof(error)));
    view = tilefinch_diagnostic_qr_view(report);
    assert(view->part_index == 1u && view->part_count == 2u);
    raw = decode_current_part(report, &raw_size);
    assert(read_u32(raw + 36u) == 1u && read_u32(raw + 40u) == 2u);
    at = 45u + raw[44];
    assert(raw[at++] == strlen("tilefinch-validation.txt"));
    assert(raw[at++] == 0u && raw[at++] == 0u && raw[at++] == 0u);
    uint32_t second_original = read_u32(raw + at); at += 4u;
    uint32_t second_offset = read_u32(raw + at); at += 4u;
    uint32_t second_size = read_u32(raw + at); at += 4u;
    uint32_t second_crc = read_u32(raw + at); at += 4u;
    assert(second_original == sizeof(extra) && second_size == sizeof(extra)
           && second_offset == 0u);
    at += strlen("tilefinch-validation.txt");
    assert(second_crc == (uint32_t) crc32(0L, raw + at, second_size));
    assert(memcmp(raw + at, extra, sizeof(extra)) == 0);
    at += sizeof(extra);
    assert(at == raw_size);
    free(raw);
    tilefinch_diagnostic_qr_destroy(report);
    assert(unlink(error_path) == 0);
    assert(unlink(extra_path) == 0);
    free(error_path);
    free(extra_path);
}

static void test_oversized_logs_are_complete_across_bounded_parts(void)
{
    enum { LARGE_BYTES = 160 * 1024 };
    uint8_t *first = malloc(LARGE_BYTES);
    uint8_t *second = malloc(LARGE_BYTES);
    assert(first != NULL && second != NULL);
    uint32_t random = UINT32_C(0xA1B2C3D4);
    for (size_t at = 0; at < LARGE_BYTES; at++) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        first[at] = (uint8_t) random;
        second[at] = (uint8_t) (random ^ 0x5Au);
    }
    char *first_path = make_file(first, LARGE_BYTES);
    char *second_path = make_file(second, LARGE_BYTES);
    TilefinchDiagnosticSource sources[] = {
        {"tilefinch-last-error.txt", first_path},
        {"tilefinch-validation.txt", second_path}
    };
    TilefinchDiagnosticMetadata metadata = {.app_version = "test"};
    char error[96];
    TilefinchDiagnosticQrReport *report = tilefinch_diagnostic_qr_build(
        &metadata, sources, 2u, error, sizeof(error));
    assert(report != NULL && error[0] == '\0');
    const TilefinchDiagnosticQrView *view = tilefinch_diagnostic_qr_view(report);
    assert(view != NULL && view->raw_bytes <= TILEFINCH_DIAGNOSTIC_QR_CAPTURE_LIMIT);
    assert(view->part_count >= 6u);
    uint8_t *rebuilt[2] = {calloc(1u, LARGE_BYTES), calloc(1u, LARGE_BYTES)};
    size_t rebuilt_size[2] = {0u, 0u};
    assert(rebuilt[0] != NULL && rebuilt[1] != NULL);
    for (unsigned part = 0u; part < view->part_count; part++) {
        assert(tilefinch_diagnostic_qr_select_part(
            report, part, error, sizeof(error)));
        view = tilefinch_diagnostic_qr_view(report);
        assert(view->part_index == part
               && view->page_count <= TILEFINCH_DIAGNOSTIC_QR_PAGE_LIMIT);
        size_t raw_size = 0u;
        uint8_t *raw = decode_current_part(report, &raw_size);
        assert(raw[4] == 2u && raw[5] == 1u);
        assert(read_u32(raw + 36u) == part
               && read_u32(raw + 40u) == view->part_count);
        size_t at = 45u + raw[44];
        uint8_t name_length = raw[at++];
        assert((raw[at++] & 1u) != 0u);
        at += 2u;
        uint32_t original = read_u32(raw + at); at += 4u;
        uint32_t offset = read_u32(raw + at); at += 4u;
        uint32_t retained = read_u32(raw + at); at += 4u;
        uint32_t checksum = read_u32(raw + at); at += 4u;
        const char *first_name = "tilefinch-last-error.txt";
        const char *second_name = "tilefinch-validation.txt";
        size_t source = name_length == strlen(first_name)
            && memcmp(raw + at, first_name, name_length) == 0 ? 0u : 1u;
        assert(source == 0u || (name_length == strlen(second_name)
               && memcmp(raw + at, second_name, name_length) == 0));
        at += name_length;
        assert(original == LARGE_BYTES && offset == rebuilt_size[source]);
        const uint8_t *expected = source == 0u ? first : second;
        assert(memcmp(raw + at, expected + offset, retained) == 0);
        assert(checksum == (uint32_t) crc32(0L, raw + at, retained));
        memcpy(rebuilt[source] + offset, raw + at, retained);
        rebuilt_size[source] += retained;
        at += retained;
        assert(at == raw_size);
        free(raw);
    }
    assert(rebuilt_size[0] == LARGE_BYTES && rebuilt_size[1] == LARGE_BYTES);
    assert(memcmp(rebuilt[0], first, LARGE_BYTES) == 0);
    assert(memcmp(rebuilt[1], second, LARGE_BYTES) == 0);
    free(rebuilt[0]);
    free(rebuilt[1]);
    tilefinch_diagnostic_qr_destroy(report);
    assert(unlink(first_path) == 0 && unlink(second_path) == 0);
    free(first_path);
    free(second_path);
    free(first);
    free(second);
}

static void test_missing_logs_refused(void)
{
    TilefinchDiagnosticSource source = {
        "missing.txt", "/tmp/tilefinch-diagnostic-missing"
    };
    TilefinchDiagnosticMetadata metadata = {.app_version = "test"};
    char error[64];
    assert(tilefinch_diagnostic_qr_build(
        &metadata, &source, 1u, error, sizeof(error)) == NULL);
    assert(strcmp(error, "NO DIAGNOSTIC LOG FOUND") == 0);
}

static void test_error_summary_prefers_transport_diagnostics(void)
{
    static const char error_log[] =
        "tilefinch-device-error-v2\n"
        "stage=navigation\n"
        "http=200\n"
        "native=0x00000000\n"
        "curl-code=0\n"
        "tls-verify=0x00000008\n"
        "detail=certificate verification failed\n";
    char *path = make_file(
        (const uint8_t *) error_log, sizeof(error_log) - 1u);
    TilefinchDiagnosticSource source = {
        "tilefinch-last-error.txt", path
    };
    TilefinchDiagnosticMetadata metadata = {.app_version = "test"};
    char error[64];
    TilefinchDiagnosticQrReport *report = tilefinch_diagnostic_qr_build(
        &metadata, &source, 1u, error, sizeof(error));
    assert(report != NULL);
    assert(strcmp(
        tilefinch_diagnostic_qr_view(report)->error_summary,
        "navigation / TLS 0x00000008") == 0);
    tilefinch_diagnostic_qr_destroy(report);
    assert(unlink(path) == 0);
    free(path);
}

int main(void)
{
    test_base45_vectors();
    test_bundle_round_trip();
    test_oversized_logs_are_complete_across_bounded_parts();
    test_missing_logs_refused();
    test_error_summary_prefers_transport_diagnostics();
    puts("diagnostic QR tests passed");
    return 0;
}
