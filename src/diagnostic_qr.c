#include "tilefinch/diagnostic_qr.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "qrcodegen.h"

#define DIAGNOSTIC_BUNDLE_VERSION 2u
#define DIAGNOSTIC_CHUNK_BYTES 960u
#define DIAGNOSTIC_NAME_LIMIT 63u
#define DIAGNOSTIC_PATH_LIMIT 511u
#define DIAGNOSTIC_PAGE_TEXT_CAPACITY 1500u
#define DIAGNOSTIC_HEADER_BYTES 45u
#define DIAGNOSTIC_ENTRY_BYTES 20u
#define DIAGNOSTIC_QR_BUFFER_BYTES \
    qrcodegen_BUFFER_LEN_FOR_VERSION(TILEFINCH_DIAGNOSTIC_QR_VERSION)

struct TilefinchDiagnosticQrReport {
    TilefinchDiagnosticMetadata metadata;
    char app_version_storage[256];
    struct {
        char name[DIAGNOSTIC_NAME_LIMIT + 1u];
        char path[DIAGNOSTIC_PATH_LIMIT + 1u];
        uint32_t size;
        uint32_t part_count;
    } source[TILEFINCH_DIAGNOSTIC_QR_SOURCE_LIMIT];
    size_t source_count;
    uint32_t part_count;
    uint8_t *compressed;
    size_t compressed_size;
    uint32_t compressed_crc;
    uint32_t report_crc;
    uint8_t qr[DIAGNOSTIC_QR_BUFFER_BYTES];
    uint8_t temporary[DIAGNOSTIC_QR_BUFFER_BYTES];
    char page_text[DIAGNOSTIC_PAGE_TEXT_CAPACITY];
    TilefinchDiagnosticQrView view;
};

typedef struct {
    FILE *file;
    const char *name;
    const char *path;
    uint32_t size;
} DiagnosticOpenSource;

static void set_error(char *output, size_t capacity, const char *message)
{
    if (output == NULL || capacity == 0u) return;
    snprintf(output, capacity, "%s", message == NULL ? "" : message);
}

static void put_u32(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t) (value >> 24);
    output[1] = (uint8_t) (value >> 16);
    output[2] = (uint8_t) (value >> 8);
    output[3] = (uint8_t) value;
}

static void put_u64(uint8_t *output, uint64_t value)
{
    for (unsigned at = 0; at < 8u; at++)
        output[at] = (uint8_t) (value >> (56u - at * 8u));
}

static void close_sources(DiagnosticOpenSource *sources, size_t count)
{
    for (size_t at = 0; at < count; at++) {
        if (sources[at].file != NULL) fclose(sources[at].file);
        sources[at].file = NULL;
    }
}

static bool source_open(
    const TilefinchDiagnosticSource *source, DiagnosticOpenSource *opened)
{
    if (source == NULL || opened == NULL || source->name == NULL
        || source->path == NULL || source->name[0] == '\0'
        || source->path[0] == '\0') return false;
    size_t name_length = strlen(source->name);
    size_t path_length = strlen(source->path);
    if (name_length > DIAGNOSTIC_NAME_LIMIT
        || path_length > DIAGNOSTIC_PATH_LIMIT) return false;
    FILE *file = fopen(source->path, "rb");
    if (file == NULL) return errno == ENOENT;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    long length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    opened->file = file;
    opened->name = source->name;
#if LONG_MAX > 4294967295LL
    if ((uint64_t) length > UINT32_MAX) {
        fclose(file);
        return false;
    }
#endif
    opened->path = source->path;
    opened->size = (uint32_t) length;
    return true;
}

static size_t line_value(
    const uint8_t *bytes, size_t size, const char *key,
    char *output, size_t capacity)
{
    if (bytes == NULL || key == NULL || output == NULL || capacity == 0u)
        return 0u;
    output[0] = '\0';
    size_t key_length = strlen(key);
    for (size_t at = 0; at + key_length <= size;) {
        size_t line_end = at;
        while (line_end < size && bytes[line_end] != '\n') line_end++;
        if (line_end - at >= key_length
            && memcmp(bytes + at, key, key_length) == 0) {
            size_t length = line_end - at - key_length;
            if (length >= capacity) length = capacity - 1u;
            memcpy(output, bytes + at + key_length, length);
            output[length] = '\0';
            return length;
        }
        at = line_end < size ? line_end + 1u : size;
    }
    return 0u;
}

static void extract_error_summary(
    TilefinchDiagnosticQrView *view, const uint8_t *bytes, size_t size)
{
    char stage[24] = {0};
    char native[16] = {0};
    char http[12] = {0};
    char curl[12] = {0};
    char tls[16] = {0};
    char detail[32] = {0};
    char code_text[32] = {0};
    (void) line_value(bytes, size, "stage=", stage, sizeof(stage));
    (void) line_value(bytes, size, "native=", native, sizeof(native));
    (void) line_value(bytes, size, "http=", http, sizeof(http));
    (void) line_value(bytes, size, "curl-code=", curl, sizeof(curl));
    (void) line_value(bytes, size, "tls-verify=", tls, sizeof(tls));
    (void) line_value(bytes, size, "detail=", detail, sizeof(detail));
    const char *code = NULL;
    if (native[0] != '\0' && strcmp(native, "0x00000000") != 0) {
        code = native;
    } else if (tls[0] != '\0' && strcmp(tls, "0x00000000") != 0) {
        snprintf(code_text, sizeof(code_text), "TLS %.16s", tls);
        code = code_text;
    } else if (curl[0] != '\0' && strcmp(curl, "0") != 0) {
        snprintf(code_text, sizeof(code_text), "curl %.12s", curl);
        code = code_text;
    } else if (http[0] != '\0' && strcmp(http, "0") != 0
               && strcmp(http, "200") != 0) {
        snprintf(code_text, sizeof(code_text), "HTTP %.12s", http);
        code = code_text;
    } else {
        code = detail;
    }
    if (stage[0] != '\0' && code != NULL && code[0] != '\0')
        snprintf(view->error_summary, sizeof(view->error_summary),
                 "%.20s / %.22s", stage, code);
    else if (stage[0] != '\0')
        snprintf(view->error_summary, sizeof(view->error_summary), "%s", stage);
    else
        snprintf(view->error_summary, sizeof(view->error_summary), "See bundled log");
}

static void format_device(char *output, size_t capacity, uint32_t model)
{
    const char *name = NULL;
    switch (model) {
        case 0u: name = "PSP-1000"; break;
        case 1u: name = "PSP-2000"; break;
        case 2u: name = "PSP-3000"; break;
        case 3u: name = "PSP-N1000"; break;
        case 4u: name = "PSP-E1000"; break;
        default: break;
    }
    if (name != NULL) snprintf(output, capacity, "%s", name);
    else if (model == UINT32_MAX) snprintf(output, capacity, "Sony PSP");
    else snprintf(output, capacity, "PSP model %" PRIu32, model);
}

static void format_firmware(char *output, size_t capacity, uint32_t version)
{
    unsigned major = (version >> 24) & 0xffu;
    unsigned minor = (version >> 16) & 0xffu;
    unsigned revision = (version >> 8) & 0xffu;
    if (major <= 9u && minor <= 9u && revision <= 9u)
        snprintf(output, capacity, "%u.%u%u", major, minor, revision);
    else
        snprintf(output, capacity, "0x%08" PRIX32, version);
}

size_t tilefinch_base45_encoded_size(size_t byte_count)
{
    size_t pairs = byte_count / 2u;
    size_t tail = (byte_count % 2u) * 2u;
    if (pairs > (SIZE_MAX - tail) / 3u) return SIZE_MAX;
    return pairs * 3u + tail;
}

bool tilefinch_base45_encode(
    const uint8_t *bytes, size_t byte_count,
    char *output, size_t output_capacity, size_t *output_length)
{
    static const char alphabet[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:";
    if ((bytes == NULL && byte_count != 0u) || output == NULL) return false;
    size_t needed = tilefinch_base45_encoded_size(byte_count);
    if (needed == SIZE_MAX || needed >= output_capacity) return false;
    size_t used = 0u;
    size_t at = 0u;
    while (at + 1u < byte_count) {
        unsigned value = (unsigned) bytes[at] * 256u + bytes[at + 1u];
        output[used++] = alphabet[value % 45u];
        output[used++] = alphabet[(value / 45u) % 45u];
        output[used++] = alphabet[value / (45u * 45u)];
        at += 2u;
    }
    if (at < byte_count) {
        unsigned value = bytes[at];
        output[used++] = alphabet[value % 45u];
        output[used++] = alphabet[value / 45u];
    }
    output[used] = '\0';
    if (output_length != NULL) *output_length = used;
    return true;
}

static bool report_render_page(
    TilefinchDiagnosticQrReport *report, unsigned page_index)
{
    if (report == NULL || page_index >= report->view.page_count) return false;
    size_t offset = (size_t) page_index * DIAGNOSTIC_CHUNK_BYTES;
    size_t remaining = report->compressed_size - offset;
    size_t chunk_size = remaining < DIAGNOSTIC_CHUNK_BYTES
        ? remaining : DIAGNOSTIC_CHUNK_BYTES;
    uint32_t chunk_crc = (uint32_t) crc32(
        0L, report->compressed + offset, (uInt) chunk_size);
    int prefix = snprintf(
        report->page_text, sizeof(report->page_text),
        "TFD2:%08" PRIX32 ":%08X:%08X:%02X:%02X:%08" PRIX32
        ":%08" PRIX32 ":",
        report->report_crc,
        report->view.part_index + 1u, report->view.part_count,
        page_index + 1u, report->view.page_count,
        report->compressed_crc, chunk_crc);
    if (prefix <= 0 || (size_t) prefix >= sizeof(report->page_text))
        return false;
    size_t encoded = 0u;
    if (!tilefinch_base45_encode(
            report->compressed + offset, chunk_size,
            report->page_text + (size_t) prefix,
            sizeof(report->page_text) - (size_t) prefix, &encoded))
        return false;
    (void) encoded;
    if (!qrcodegen_encodeText(
            report->page_text, report->temporary, report->qr,
            qrcodegen_Ecc_MEDIUM,
            TILEFINCH_DIAGNOSTIC_QR_VERSION,
            TILEFINCH_DIAGNOSTIC_QR_VERSION,
            qrcodegen_Mask_AUTO, false)) return false;
    if (qrcodegen_getSize(report->qr)
            != (int) TILEFINCH_DIAGNOSTIC_QR_MODULES) return false;
    report->view.modules = report->qr + 1u;
    report->view.module_count = TILEFINCH_DIAGNOSTIC_QR_MODULES;
    report->view.page_index = page_index;
    return true;
}

static size_t part_payload_capacity(size_t version_length, size_t name_length)
{
    size_t overhead = DIAGNOSTIC_HEADER_BYTES + version_length
        + DIAGNOSTIC_ENTRY_BYTES + name_length;
    return overhead < TILEFINCH_DIAGNOSTIC_QR_CAPTURE_LIMIT
        ? TILEFINCH_DIAGNOSTIC_QR_CAPTURE_LIMIT - overhead : 0u;
}

static uint32_t source_part_count(uint32_t size, size_t payload)
{
    if (size == 0u) return 1u;
    return (uint32_t) (((uint64_t) size + payload - 1u) / payload);
}

static uint32_t report_identity(const TilefinchDiagnosticQrReport *report)
{
    uint8_t numbers[32];
    put_u64(numbers, report->metadata.release_sequence);
    put_u64(numbers + 8u, report->metadata.created_unix_time);
    put_u32(numbers + 16u, report->metadata.psp_model);
    put_u32(numbers + 20u, report->metadata.psp_firmware);
    put_u32(numbers + 24u, report->part_count);
    put_u32(numbers + 28u, (uint32_t) report->source_count);
    uLong crc = crc32(0L, (const Bytef *) "TFDG2", 5u);
    crc = crc32(crc, numbers, sizeof(numbers));
    crc = crc32(crc, (const Bytef *) report->app_version_storage,
                (uInt) strlen(report->app_version_storage));
    for (size_t at = 0u; at < report->source_count; at++) {
        crc = crc32(crc, (const Bytef *) report->source[at].name,
                    (uInt) strlen(report->source[at].name));
        uint8_t size_bytes[4];
        put_u32(size_bytes, report->source[at].size);
        crc = crc32(crc, size_bytes, sizeof(size_bytes));
    }
    return (uint32_t) crc;
}

static bool locate_part(
    const TilefinchDiagnosticQrReport *report, uint32_t part_index,
    size_t *source_index, uint32_t *source_part_index)
{
    if (report == NULL || part_index >= report->part_count) return false;
    for (size_t source = 0u; source < report->source_count; source++) {
        if (part_index < report->source[source].part_count) {
            *source_index = source;
            *source_part_index = part_index;
            return true;
        }
        part_index -= report->source[source].part_count;
    }
    return false;
}

static void report_release_part(TilefinchDiagnosticQrReport *report)
{
    if (report == NULL) return;
    free(report->compressed);
    report->compressed = NULL;
    report->compressed_size = 0u;
    report->view.modules = NULL;
    report->view.raw_bytes = 0u;
    report->view.compressed_bytes = 0u;
    report->view.page_count = 0u;
    report->page_text[0] = '\0';
}

static bool report_build_part(
    TilefinchDiagnosticQrReport *report, uint32_t part_index,
    char *error, size_t error_capacity)
{
    set_error(error, error_capacity, "");
    size_t source_index = 0u;
    uint32_t source_part = 0u;
    if (!locate_part(report, part_index, &source_index, &source_part)) {
        set_error(error, error_capacity, "DIAGNOSTIC PART INVALID");
        return false;
    }
    report_release_part(report);
    size_t version_length = strlen(report->app_version_storage);
    size_t name_length = strlen(report->source[source_index].name);
    size_t payload = part_payload_capacity(version_length, name_length);
    uint64_t offset64 = (uint64_t) source_part * payload;
    uint32_t offset = (uint32_t) offset64;
    uint32_t remaining = report->source[source_index].size - offset;
    uint32_t retained = remaining < payload ? remaining : (uint32_t) payload;
    size_t raw_size = DIAGNOSTIC_HEADER_BYTES + version_length
        + DIAGNOSTIC_ENTRY_BYTES + name_length + retained;
    uint8_t *raw = malloc(raw_size);
    if (raw == NULL) {
        set_error(error, error_capacity, "NOT ENOUGH MEMORY FOR DIAGNOSTICS");
        return false;
    }
    FILE *file = fopen(report->source[source_index].path, "rb");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
        if (file != NULL) fclose(file);
        free(raw);
        set_error(error, error_capacity, "A DIAGNOSTIC LOG COULD NOT BE READ");
        return false;
    }
    long current_length = ftell(file);
    if (current_length < 0 || (uint64_t) (unsigned long) current_length
            < report->source[source_index].size
        || offset > (uint32_t) LONG_MAX
        || fseek(file, (long) offset, SEEK_SET) != 0) {
        fclose(file);
        free(raw);
        set_error(error, error_capacity, "A DIAGNOSTIC LOG CHANGED WHILE READING");
        return false;
    }

    size_t used = 0u;
    memcpy(raw + used, "TFDG", 4u); used += 4u;
    raw[used++] = DIAGNOSTIC_BUNDLE_VERSION;
    raw[used++] = 1u;
    raw[used++] = 0u;
    raw[used++] = 0u;
    put_u32(raw + used, report->report_crc); used += 4u;
    put_u64(raw + used, report->metadata.release_sequence); used += 8u;
    put_u64(raw + used, report->metadata.created_unix_time); used += 8u;
    put_u32(raw + used, report->metadata.psp_model); used += 4u;
    put_u32(raw + used, report->metadata.psp_firmware); used += 4u;
    put_u32(raw + used, part_index); used += 4u;
    put_u32(raw + used, report->part_count); used += 4u;
    raw[used++] = (uint8_t) version_length;
    memcpy(raw + used, report->app_version_storage, version_length);
    used += version_length;
    raw[used++] = (uint8_t) name_length;
    bool segmented = offset != 0u || retained != report->source[source_index].size;
    raw[used++] = segmented ? 1u : 0u;
    raw[used++] = 0u;
    raw[used++] = 0u;
    put_u32(raw + used, report->source[source_index].size); used += 4u;
    put_u32(raw + used, offset); used += 4u;
    put_u32(raw + used, retained); used += 4u;
    size_t crc_offset = used;
    used += 4u;
    memcpy(raw + used, report->source[source_index].name, name_length);
    used += name_length;
    size_t data_offset = used;
    size_t got = fread(raw + used, 1u, retained, file);
    fclose(file);
    if (got != retained) {
        free(raw);
        set_error(error, error_capacity, "A DIAGNOSTIC LOG CHANGED WHILE READING");
        return false;
    }
    used += got;
    put_u32(raw + crc_offset,
            (uint32_t) crc32(0L, raw + data_offset, (uInt) got));
    if (part_index == 0u
        && strcmp(report->source[source_index].name,
                  "tilefinch-last-error.txt") == 0)
        extract_error_summary(&report->view, raw + data_offset, got);

    uLongf compressed_capacity = compressBound((uLong) used);
    report->compressed = malloc((size_t) compressed_capacity);
    if (report->compressed == NULL
        || compress2(report->compressed, &compressed_capacity,
                     raw, (uLong) used, Z_BEST_SPEED) != Z_OK) {
        free(raw);
        report_release_part(report);
        set_error(error, error_capacity, "DIAGNOSTIC COMPRESSION FAILED");
        return false;
    }
    free(raw);
    report->compressed_size = (size_t) compressed_capacity;
    report->compressed_crc = (uint32_t) crc32(
        0L, report->compressed, (uInt) report->compressed_size);
    size_t page_count = (report->compressed_size + DIAGNOSTIC_CHUNK_BYTES - 1u)
        / DIAGNOSTIC_CHUNK_BYTES;
    if (page_count == 0u) page_count = 1u;
    if (page_count > TILEFINCH_DIAGNOSTIC_QR_PAGE_LIMIT) {
        report_release_part(report);
        set_error(error, error_capacity, "DIAGNOSTIC PART EXCEEDED QR BOUND");
        return false;
    }
    report->view.part_index = part_index;
    report->view.part_count = report->part_count;
    report->view.raw_bytes = used;
    report->view.compressed_bytes = report->compressed_size;
    report->view.page_count = (unsigned) page_count;
    if (!report_render_page(report, 0u)) {
        report_release_part(report);
        set_error(error, error_capacity, "QR ENCODING FAILED");
        return false;
    }
    return true;
}

TilefinchDiagnosticQrReport *tilefinch_diagnostic_qr_build(
    const TilefinchDiagnosticMetadata *metadata,
    const TilefinchDiagnosticSource *sources, size_t source_count,
    char *error, size_t error_capacity)
{
    set_error(error, error_capacity, "");
    if (metadata == NULL || sources == NULL || source_count == 0u
        || source_count > TILEFINCH_DIAGNOSTIC_QR_SOURCE_LIMIT) {
        set_error(error, error_capacity, "DIAGNOSTIC SOURCES INVALID");
        return NULL;
    }
    DiagnosticOpenSource opened[TILEFINCH_DIAGNOSTIC_QR_SOURCE_LIMIT] = {0};
    size_t opened_count = 0u;
    for (size_t at = 0u; at < source_count; at++) {
        DiagnosticOpenSource candidate = {0};
        errno = 0;
        if (!source_open(&sources[at], &candidate)) {
            close_sources(opened, opened_count);
            set_error(error, error_capacity, "A DIAGNOSTIC LOG COULD NOT BE READ");
            return NULL;
        }
        if (candidate.file != NULL) opened[opened_count++] = candidate;
    }
    if (opened_count == 0u) {
        set_error(error, error_capacity, "NO DIAGNOSTIC LOG FOUND");
        return NULL;
    }
    TilefinchDiagnosticQrReport *report = calloc(1u, sizeof(*report));
    if (report == NULL) {
        close_sources(opened, opened_count);
        set_error(error, error_capacity, "NOT ENOUGH MEMORY FOR DIAGNOSTICS");
        return NULL;
    }
    report->metadata = *metadata;
    snprintf(report->app_version_storage, sizeof(report->app_version_storage),
             "%s", metadata->app_version == NULL ? "unknown" : metadata->app_version);
    report->metadata.app_version = report->app_version_storage;
    report->source_count = opened_count;
    uint64_t total_parts = 0u;
    size_t version_length = strlen(report->app_version_storage);
    for (size_t at = 0u; at < opened_count; at++) {
        snprintf(report->source[at].name, sizeof(report->source[at].name),
                 "%s", opened[at].name);
        snprintf(report->source[at].path, sizeof(report->source[at].path),
                 "%s", opened[at].path);
        report->source[at].size = opened[at].size;
        size_t payload = part_payload_capacity(
            version_length, strlen(report->source[at].name));
        if (payload == 0u) total_parts = UINT64_MAX;
        else {
            report->source[at].part_count = source_part_count(
                report->source[at].size, payload);
            total_parts += report->source[at].part_count;
        }
    }
    close_sources(opened, opened_count);
    if (total_parts == 0u || total_parts > UINT32_MAX) {
        tilefinch_diagnostic_qr_destroy(report);
        set_error(error, error_capacity, "DIAGNOSTIC PART COUNT INVALID");
        return NULL;
    }
    report->part_count = (uint32_t) total_parts;
    report->report_crc = report_identity(report);
    snprintf(report->view.report_id, sizeof(report->view.report_id),
             "%08" PRIX32, report->report_crc);
    snprintf(report->view.app_version, sizeof(report->view.app_version), "%.15s",
             report->app_version_storage);
    format_device(report->view.device, sizeof(report->view.device),
                  metadata->psp_model);
    format_firmware(report->view.firmware, sizeof(report->view.firmware),
                    metadata->psp_firmware);
    snprintf(report->view.error_summary, sizeof(report->view.error_summary),
             "See bundled log");
    if (!report_build_part(report, 0u, error, error_capacity)) {
        tilefinch_diagnostic_qr_destroy(report);
        return NULL;
    }
    return report;
}

void tilefinch_diagnostic_qr_destroy(TilefinchDiagnosticQrReport *report)
{
    if (report == NULL) return;
    report_release_part(report);
    free(report);
}

const TilefinchDiagnosticQrView *tilefinch_diagnostic_qr_view(
    const TilefinchDiagnosticQrReport *report)
{
    return report == NULL ? NULL : &report->view;
}

bool tilefinch_diagnostic_qr_select_page(
    TilefinchDiagnosticQrReport *report, unsigned page_index)
{
    return report_render_page(report, page_index);
}

bool tilefinch_diagnostic_qr_select_part(
    TilefinchDiagnosticQrReport *report, unsigned part_index,
    char *error, size_t error_capacity)
{
    return report_build_part(
        report, (uint32_t) part_index, error, error_capacity);
}

const char *tilefinch_diagnostic_qr_page_text(
    const TilefinchDiagnosticQrReport *report)
{
    return report == NULL ? NULL : report->page_text;
}
