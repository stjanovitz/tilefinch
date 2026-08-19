#ifndef TILEFINCH_DIAGNOSTIC_QR_H
#define TILEFINCH_DIAGNOSTIC_QR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Version 27 has 125 modules. Four quiet modules on every edge at two PSP
   pixels per module produce a 266-pixel square on the 480-by-272 display. */
#define TILEFINCH_DIAGNOSTIC_QR_VERSION 27u
#define TILEFINCH_DIAGNOSTIC_QR_MODULES 125u
#define TILEFINCH_DIAGNOSTIC_QR_QUIET_MODULES 4u
#define TILEFINCH_DIAGNOSTIC_QR_MODULE_PIXELS 2u
#define TILEFINCH_DIAGNOSTIC_QR_RENDER_PIXELS 266u
#define TILEFINCH_DIAGNOSTIC_QR_SOURCE_LIMIT 5u
#define TILEFINCH_DIAGNOSTIC_QR_RAW_LIMIT (128u * 1024u)
#define TILEFINCH_DIAGNOSTIC_QR_CAPTURE_LIMIT (56u * 1024u)
#define TILEFINCH_DIAGNOSTIC_QR_PAGE_LIMIT 64u

typedef struct TilefinchDiagnosticQrReport TilefinchDiagnosticQrReport;

typedef struct {
    const char *name;
    const char *path;
} TilefinchDiagnosticSource;

typedef struct {
    const char *app_version;
    uint64_t release_sequence;
    uint64_t created_unix_time;
    uint32_t psp_model;
    uint32_t psp_firmware;
} TilefinchDiagnosticMetadata;

/* Borrowed view into a report. `modules` is a row-major, LSB-first packed
   matrix without padding between rows. The representation is deliberately
   tiny and lets the UI render without linking the encoder into every
   executable which uses the shared chrome object. */
typedef struct {
    const uint8_t *modules;
    unsigned module_count;
    unsigned part_index;
    unsigned part_count;
    unsigned page_index;
    unsigned page_count;
    size_t raw_bytes;
    size_t compressed_bytes;
    char report_id[9];
    char app_version[16];
    char device[24];
    char firmware[16];
    char error_summary[48];
} TilefinchDiagnosticQrView;

/* Captures the size of every existing source and builds the first part of a
   complete, versioned report. Large files are divided into exact contiguous
   segments; source order defines part priority. Only one compressed part is
   retained in RAM. Missing files are skipped. The operation is explicitly
   user-triggered; no call belongs on a startup or frame path. */
TilefinchDiagnosticQrReport *tilefinch_diagnostic_qr_build(
    const TilefinchDiagnosticMetadata *metadata,
    const TilefinchDiagnosticSource *sources, size_t source_count,
    char *error, size_t error_capacity);

void tilefinch_diagnostic_qr_destroy(TilefinchDiagnosticQrReport *report);
const TilefinchDiagnosticQrView *tilefinch_diagnostic_qr_view(
    const TilefinchDiagnosticQrReport *report);
bool tilefinch_diagnostic_qr_select_page(
    TilefinchDiagnosticQrReport *report, unsigned page_index);
/* Rebuilds one report part from the unchanged source file after releasing the
   previous part's compressed payload. */
bool tilefinch_diagnostic_qr_select_part(
    TilefinchDiagnosticQrReport *report, unsigned part_index,
    char *error, size_t error_capacity);
const char *tilefinch_diagnostic_qr_page_text(
    const TilefinchDiagnosticQrReport *report);
static inline bool tilefinch_diagnostic_qr_module(
    const TilefinchDiagnosticQrView *view, unsigned x, unsigned y)
{
    if (view == NULL || view->modules == NULL
        || x >= view->module_count || y >= view->module_count) return false;
    size_t bit = (size_t) y * view->module_count + x;
    return ((view->modules[bit >> 3] >> (bit & 7u)) & 1u) != 0u;
}

/* Exposed for the host decoder/format tests. Base45 is RFC 9285 and uses the
   QR alphanumeric alphabet, avoiding byte-mode's lower capacity. */
size_t tilefinch_base45_encoded_size(size_t byte_count);
bool tilefinch_base45_encode(
    const uint8_t *bytes, size_t byte_count,
    char *output, size_t output_capacity, size_t *output_length);

#endif
