#include "tilefinch/diagnostics.h"

#include <ctype.h>
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

static bool diagnostic_contains_ascii_folded(
    const char *text, const char *needle)
{
    if (text == NULL || needle == NULL || needle[0] == '\0') return false;
    for (; *text != '\0'; text++) {
        const char *left = text;
        const char *right = needle;
        while (*left != '\0' && *right != '\0'
               && tolower((unsigned char) *left)
                      == tolower((unsigned char) *right)) {
            left++;
            right++;
        }
        if (*right == '\0') return true;
    }
    return false;
}

bool tilefinch_error_is_certificate_verification_failure(
    const char *detail)
{
    if (detail == NULL || detail[0] == '\0') return false;
    bool certificate = diagnostic_contains_ascii_folded(
                           detail, "certificate")
        || diagnostic_contains_ascii_folded(detail, "x.509")
        || diagnostic_contains_ascii_folded(detail, "x509");
    bool verification = diagnostic_contains_ascii_folded(detail, "mbedtls")
        || diagnostic_contains_ascii_folded(detail, "ssl")
        || diagnostic_contains_ascii_folded(detail, "tls")
        || diagnostic_contains_ascii_folded(detail, "trusted ca")
        || diagnostic_contains_ascii_folded(detail, "local issuer")
        || diagnostic_contains_ascii_folded(detail, "not correctly signed")
        || diagnostic_contains_ascii_folded(detail, "peer verification")
        || diagnostic_contains_ascii_folded(detail, "verification failed")
        || diagnostic_contains_ascii_folded(detail, "verify failed")
        || diagnostic_contains_ascii_folded(detail, "not yet valid")
        || diagnostic_contains_ascii_folded(detail, "expired");
    return certificate && verification;
}

TilefinchTlsGuidance tilefinch_tls_verification_guidance(
    uint32_t flags, bool flags_available, PspTimeStatus clock_status)
{
    if (clock_status != PSP_TIME_OK) {
        return TILEFINCH_TLS_GUIDANCE_TIME;
    }
    if (!flags_available) return TILEFINCH_TLS_GUIDANCE_DETAILS;
    if ((flags & (TILEFINCH_TLS_VERIFY_EXPIRED
                  | TILEFINCH_TLS_VERIFY_FUTURE)) != 0)
        return TILEFINCH_TLS_GUIDANCE_TIME;
    if ((flags & TILEFINCH_TLS_VERIFY_HOSTNAME) != 0)
        return TILEFINCH_TLS_GUIDANCE_REDIRECTED;
    if ((flags & TILEFINCH_TLS_VERIFY_NOT_TRUSTED) != 0)
        return TILEFINCH_TLS_GUIDANCE_UNTRUSTED;
    if ((flags & (TILEFINCH_TLS_VERIFY_BAD_MD
                  | TILEFINCH_TLS_VERIFY_BAD_PK
                  | TILEFINCH_TLS_VERIFY_BAD_KEY)) != 0) {
        return TILEFINCH_TLS_GUIDANCE_UNSUPPORTED;
    }
    return TILEFINCH_TLS_GUIDANCE_DETAILS;
}
