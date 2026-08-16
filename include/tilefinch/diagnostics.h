#ifndef TILEFINCH_DIAGNOSTICS_H
#define TILEFINCH_DIAGNOSTICS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    TILEFINCH_DIAGNOSTIC_DEBUG = 0,
    TILEFINCH_DIAGNOSTIC_INFO,
    TILEFINCH_DIAGNOSTIC_WARNING,
    TILEFINCH_DIAGNOSTIC_ERROR
} TilefinchDiagnosticSeverity;

typedef enum {
    TILEFINCH_SUBSYSTEM_ENGINE = 0,
    TILEFINCH_SUBSYSTEM_NETWORK,
    TILEFINCH_SUBSYSTEM_PARSER,
    TILEFINCH_SUBSYSTEM_SCRIPT,
    TILEFINCH_SUBSYSTEM_STYLE,
    TILEFINCH_SUBSYSTEM_LAYOUT,
    TILEFINCH_SUBSYSTEM_RENDER,
    TILEFINCH_SUBSYSTEM_STORAGE,
    TILEFINCH_SUBSYSTEM_SECTIONS
} TilefinchDiagnosticSubsystem;

typedef enum {
    TILEFINCH_DIAGNOSTIC_OK = 0,
    TILEFINCH_DIAGNOSTIC_LIFECYCLE,
    TILEFINCH_DIAGNOSTIC_INVALID_INPUT,
    TILEFINCH_DIAGNOSTIC_ALLOCATION_FAILED,
    TILEFINCH_DIAGNOSTIC_LIMIT_REACHED,
    TILEFINCH_DIAGNOSTIC_CANCELLED,
    TILEFINCH_DIAGNOSTIC_TIMEOUT,
    TILEFINCH_DIAGNOSTIC_NETWORK_FAILED,
    TILEFINCH_DIAGNOSTIC_PARSE_FAILED,
    TILEFINCH_DIAGNOSTIC_SCRIPT_FAILED,
    TILEFINCH_DIAGNOSTIC_LAYOUT_FAILED,
    TILEFINCH_DIAGNOSTIC_RENDER_FAILED,
    TILEFINCH_DIAGNOSTIC_INTERNAL_FAILED
} TilefinchDiagnosticCode;

#define TILEFINCH_DIAGNOSTIC_NAME_LIMIT 40
#define TILEFINCH_DIAGNOSTIC_DETAIL_LIMIT 192

typedef struct {
    uint64_t sequence;
    uint64_t timestamp_us;
    TilefinchDiagnosticSeverity severity;
    TilefinchDiagnosticSubsystem subsystem;
    TilefinchDiagnosticCode code;
    uint64_t value;
    uint64_t auxiliary;
    char name[TILEFINCH_DIAGNOSTIC_NAME_LIMIT];
    char detail[TILEFINCH_DIAGNOSTIC_DETAIL_LIMIT];
} TilefinchDiagnosticEvent;

typedef void (*TilefinchDiagnosticCallback)(
    void *opaque, const TilefinchDiagnosticEvent *event);

typedef struct {
    TilefinchDiagnosticCallback callback;
    void *opaque;
    TilefinchDiagnosticSeverity minimum_severity;
    uint64_t next_sequence;
    size_t emitted;
    size_t filtered;
} TilefinchDiagnostics;

void tilefinch_diagnostics_init(TilefinchDiagnostics *diagnostics,
                             TilefinchDiagnosticCallback callback,
                             void *opaque,
                             TilefinchDiagnosticSeverity minimum_severity);
bool tilefinch_diagnostics_emit(TilefinchDiagnostics *diagnostics,
                             TilefinchDiagnosticSeverity severity,
                             TilefinchDiagnosticSubsystem subsystem,
                             TilefinchDiagnosticCode code,
                             const char *name, const char *detail,
                             uint64_t value, uint64_t auxiliary);
const char *tilefinch_diagnostic_severity_name(TilefinchDiagnosticSeverity value);
const char *tilefinch_diagnostic_subsystem_name(TilefinchDiagnosticSubsystem value);
const char *tilefinch_diagnostic_code_name(TilefinchDiagnosticCode value);

#endif
