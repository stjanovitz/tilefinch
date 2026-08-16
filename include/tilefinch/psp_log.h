#ifndef TILEFINCH_PSP_LOG_H
#define TILEFINCH_PSP_LOG_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef enum {
    PSP_LOG_PHASE_BOOT = 0,
    PSP_LOG_PHASE_CONFIG,
    PSP_LOG_PHASE_ASSETS,
    PSP_LOG_PHASE_ENGINE,
    PSP_LOG_PHASE_NETWORK,
    PSP_LOG_PHASE_NAVIGATION,
    PSP_LOG_PHASE_REPORT,
    PSP_LOG_PHASE_RENDER,
    PSP_LOG_PHASE_INTERACTIVE,
    PSP_LOG_PHASE_INPUT,
    PSP_LOG_PHASE_VOICE,
    PSP_LOG_PHASE_MEDIA,
    PSP_LOG_PHASE_CLEANUP,
    PSP_LOG_PHASE_HALTED,
    PSP_LOG_PHASE_COUNT
} PspLogPhase;

bool psp_log_start(const char *argv0);
int psp_log_install_exception_handler(void);
bool psp_log_start_watchdog(uint32_t timeout_ms);
void psp_log_stop_watchdog(void);
void psp_log_heartbeat(void);
void psp_log_set_phase(PspLogPhase phase);
PspLogPhase psp_log_phase(void);
uint32_t psp_log_operation_begin(const char *action);
void psp_log_operation_end(uint32_t sequence, const char *action,
                           const char *result);
int psp_log_printf(const char *format, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
FILE *psp_log_file(void);
bool psp_log_flush(bool synchronize_device);
bool psp_log_healthy(void);
void psp_log_checkpoint(const char *name);
void psp_log_emergency(const char *state);
void psp_log_finish(const char *outcome);

#if !defined(TILEFINCH_PSP_VALIDATION_LOG) \
    && !defined(TILEFINCH_PSP_LOG_IMPLEMENTATION)
/*
 * Shipping builds compile logging out at the call site. Besides avoiding
 * Memory Stick traffic, this prevents construction of expensive diagnostic
 * arguments in boot, render, input, voice, and media hot paths. The
 * never-taken branch keeps every argument referenced and type-checked
 * (and psp_log_printf format-checked) while still constant-folding to the
 * stub value, so shipping builds compile the same call sites the
 * validation build does instead of a divergent argument-free variant.
 */
#define psp_log_start(argv0) (0 ? psp_log_start(argv0) : false)
#define psp_log_install_exception_handler() \
    (0 ? psp_log_install_exception_handler() : -1)
#define psp_log_start_watchdog(timeout_ms) \
    (0 ? psp_log_start_watchdog(timeout_ms) : false)
#define psp_log_stop_watchdog() (0 ? psp_log_stop_watchdog() : (void) 0)
#define psp_log_heartbeat() (0 ? psp_log_heartbeat() : (void) 0)
#define psp_log_set_phase(phase) (0 ? psp_log_set_phase(phase) : (void) 0)
#define psp_log_phase() (0 ? psp_log_phase() : PSP_LOG_PHASE_BOOT)
#define psp_log_operation_begin(action) \
    (0 ? psp_log_operation_begin(action) : UINT32_C(0))
#define psp_log_operation_end(sequence, action, result) \
    (0 ? psp_log_operation_end(sequence, action, result) : (void) 0)
#define psp_log_printf(...) \
    ((void) (0 ? psp_log_printf(__VA_ARGS__) : 0))
#define psp_log_file() (0 ? psp_log_file() : (FILE *) NULL)
#define psp_log_flush(synchronize_device) \
    (0 ? psp_log_flush(synchronize_device) : false)
#define psp_log_healthy() (0 ? psp_log_healthy() : false)
#define psp_log_checkpoint(name) (0 ? psp_log_checkpoint(name) : (void) 0)
#define psp_log_emergency(state) (0 ? psp_log_emergency(state) : (void) 0)
#define psp_log_finish(outcome) (0 ? psp_log_finish(outcome) : (void) 0)
#endif

#endif
