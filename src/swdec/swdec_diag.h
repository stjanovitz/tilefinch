#ifndef SWDEC_DIAG_H
#define SWDEC_DIAG_H

/*
 * The software-decoder component's single diagnostics switch.
 *
 * SWDEC_DIAGNOSTICS=1 compiles in everything that exists to investigate the
 * component rather than to play video: progress and failure logging, the
 * Media Engine liveness and stack reports, fault injection, the CPU audio
 * cross-checks, the cache-maintenance bypass, the arena census, and the
 * finer flags below. Validation trees set it (cmake/PspSwdec.cmake, with
 * TILEFINCH_PSP_VALIDATION_LOG); release component builds never do, and a
 * finer flag without it is a build error, so none of it can reach a
 * shipping component by accident.
 *
 * Finer flags, each only with SWDEC_DIAGNOSTICS:
 *   SWDEC_STAGE_DIAG   per-access-unit stage clock (validation default)
 *   SWDEC_WAIT_DIAG    omit fine reconstruction probes and audio CRCs
 *                     while retaining stage/row-wait timing
 *   SWDEC_ME_DIAG      watchdog reports instead of stopping the ME
 *   SWDEC_ME_DIAG2     host0 state samples from the watchdog
 *   SWDEC_SPIN_DIAG    report pipeline spin-waits over 2 s
 *   SWDEC_ME_OFF       never boot the ME (video on the CPU alone)
 *   SWDEC_POISON       fill the decoder arena with 0xAA at open
 */
#if defined(SWDEC_DIAGNOSTICS) && SWDEC_DIAGNOSTICS
#define SWDEC_DIAG_ENABLED 1
#else
#define SWDEC_DIAG_ENABLED 0
#if defined(SWDEC_STAGE_DIAG) || defined(SWDEC_ME_DIAG) \
    || defined(SWDEC_ME_DIAG2) || defined(SWDEC_SPIN_DIAG) \
    || defined(SWDEC_ME_OFF) || defined(SWDEC_POISON) || defined(SWDEC_WAIT_DIAG) \
    || (defined(SWDEC_STAGE_TIMING) && SWDEC_STAGE_TIMING)
#error "swdec diagnostic flags require SWDEC_DIAGNOSTICS=1"
#endif
#endif

/* A log line through an optional logger. Without diagnostics the call and
   its format string are compiled out, while the arguments stay referenced
   and type-checked. */
#if SWDEC_DIAG_ENABLED
#define SWDEC_LOG_TO(logger, ...) \
    do { if (logger) (logger)(__VA_ARGS__); } while (0)
#else
#define SWDEC_LOG_TO(logger, ...) \
    ((void) (0 ? ((logger)(__VA_ARGS__), 0) : 0))
#endif

#endif
