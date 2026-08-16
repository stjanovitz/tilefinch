#include "tilefinch/diagnostics.h"

#include <stdio.h>
#include <string.h>

#include "tilefinch/platform.h"

void tilefinch_diagnostics_init(TilefinchDiagnostics *diagnostics,
                             TilefinchDiagnosticCallback callback,
                             void *opaque,
                             TilefinchDiagnosticSeverity minimum_severity)
{
    if (diagnostics == NULL) return;
    memset(diagnostics, 0, sizeof(*diagnostics));
    diagnostics->callback = callback;
    diagnostics->opaque = opaque;
    diagnostics->minimum_severity = minimum_severity;
    diagnostics->next_sequence = 1;
}

bool tilefinch_diagnostics_emit(TilefinchDiagnostics *diagnostics,
                             TilefinchDiagnosticSeverity severity,
                             TilefinchDiagnosticSubsystem subsystem,
                             TilefinchDiagnosticCode code,
                             const char *name, const char *detail,
                             uint64_t value, uint64_t auxiliary)
{
    if (diagnostics == NULL || diagnostics->callback == NULL) return false;
    if (severity < diagnostics->minimum_severity) {
        diagnostics->filtered++;
        return true;
    }
    TilefinchDiagnosticEvent event = {
        .sequence = diagnostics->next_sequence++,
        .timestamp_us = tilefinch_platform_monotonic_time_ns()
                        / UINT64_C(1000),
        .severity = severity,
        .subsystem = subsystem,
        .code = code,
        .value = value,
        .auxiliary = auxiliary
    };
    if (event.sequence == 0) event.sequence = diagnostics->next_sequence++;
    snprintf(event.name, sizeof(event.name), "%s", name == NULL ? "" : name);
    snprintf(event.detail, sizeof(event.detail), "%s",
             detail == NULL ? "" : detail);
    diagnostics->callback(diagnostics->opaque, &event);
    diagnostics->emitted++;
    return true;
}

const char *tilefinch_diagnostic_severity_name(TilefinchDiagnosticSeverity value)
{
    static const char *const names[] = {"debug", "info", "warning", "error"};
    return (unsigned) value <= TILEFINCH_DIAGNOSTIC_ERROR
        ? names[value] : "unknown";
}

const char *tilefinch_diagnostic_subsystem_name(TilefinchDiagnosticSubsystem value)
{
    static const char *const names[] = {
        "engine", "network", "parser", "script", "style", "layout",
        "render", "storage", "sections"
    };
    return (unsigned) value <= TILEFINCH_SUBSYSTEM_SECTIONS
        ? names[value] : "unknown";
}

const char *tilefinch_diagnostic_code_name(TilefinchDiagnosticCode value)
{
    static const char *const names[] = {
        "ok", "lifecycle", "invalid-input", "allocation-failed",
        "limit-reached", "cancelled", "timeout", "network-failed",
        "parse-failed", "script-failed", "layout-failed", "render-failed",
        "internal-failed"
    };
    return (unsigned) value <= TILEFINCH_DIAGNOSTIC_INTERNAL_FAILED
        ? names[value] : "unknown";
}
