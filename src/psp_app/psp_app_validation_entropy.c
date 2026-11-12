/* Raw entropy-sample capture (validation builds only; src/psp_entropy_capture.h).
 *
 * The entropy pool's estimator sees one batch at a time and only lags up to
 * 64; a beat between two clocks, or between two such beats, can look like
 * jitter at that scale. This records the samples themselves, in measured
 * order, for tools/entropy_capture_report.py.
 *
 * It arms only when the program runs from host0: (PSPLink memory mode) and
 * host0:/entropy-capture.cfg exists (keys in psp_entropy_capture.h), and it
 * writes only beside that program on host0:, so it cannot write the Memory
 * Stick. The boot's own batches are kept from the moment of arming; at the
 * preseed point the boot seeds (a trace boot does so anyway), the extra runs
 * follow, and the file is written once, after sampling, so host0: latency
 * never falls between samples.
 */
#include "psp_app_validation.h"
#include "tilefinch_compiler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef TILEFINCH_PSP_LIVE_NETWORK
#include "psp_entropy_capture.h"
#include "psp_entropy_pool.h"
#include "tilefinch/psp_entropy.h"

#define ENTROPY_CAPTURE_CONFIG "entropy-capture.cfg"
#define ENTROPY_CAPTURE_CONFIG_LIMIT 1024u

static struct {
    bool armed;
    PspEntropyCaptureConfig config;
    PspEntropyCaptureLog log;
    uint8_t *bytes;
    uint32_t *run;
    char output_path[TILEFINCH_INSTALL_PATH_LIMIT];
} entropy_capture;

static void entropy_capture_sink(const PspEntropyCaptureBatch *batch,
                                 const uint32_t *samples, void *context)
{
    (void) context;
    (void) psp_entropy_capture_log_append(&entropy_capture.log, batch,
                                          samples);
}

static void entropy_capture_release(void)
{
    /* The samples were this boot's pool input. */
    if (entropy_capture.bytes != NULL)
        psp_entropy_zeroize(entropy_capture.bytes,
                            entropy_capture.log.capacity);
    if (entropy_capture.run != NULL)
        psp_entropy_zeroize(entropy_capture.run,
                            4u * (entropy_capture.config.work_run
                                  > entropy_capture.config.sleep_run
                                      ? entropy_capture.config.work_run
                                      : entropy_capture.config.sleep_run));
    free(entropy_capture.bytes);
    free(entropy_capture.run);
    entropy_capture.bytes = NULL;
    entropy_capture.run = NULL;
    entropy_capture.armed = false;
}

TILEFINCH_COLD_PATH void psp_validation_entropy_capture_arm(
    const TilefinchInstallPaths *paths)
{
    if (paths == NULL || paths->slotted
        || strncmp(paths->program_dir, "host0:", 6) != 0)
        return;
    char config_path[TILEFINCH_INSTALL_PATH_LIMIT];
    if (!tilefinch_install_program_path(paths, ENTROPY_CAPTURE_CONFIG,
                                        config_path, sizeof(config_path)))
        return;
    FILE *file = fopen(config_path, "rb");
    if (file == NULL) return;
    char text[ENTROPY_CAPTURE_CONFIG_LIMIT + 1u];
    size_t length = fread(text, 1, sizeof(text), file);
    fclose(file);
    char why[64] = "";
    PspEntropyCaptureConfig config;
    if (length > ENTROPY_CAPTURE_CONFIG_LIMIT) {
        printf("tilefinch-entropy-capture: refused reason=config-too-long\n");
        return;
    }
    if (!psp_entropy_capture_config_parse(text, length, &config, why,
                                          sizeof(why))) {
        printf("tilefinch-entropy-capture: refused reason=\"%s\"\n", why);
        return;
    }
    if (!tilefinch_install_program_path(paths, config.output,
                                        entropy_capture.output_path,
                                        sizeof(entropy_capture.output_path))
        || strncmp(entropy_capture.output_path, "host0:", 6) != 0) {
        printf("tilefinch-entropy-capture: refused reason=output-path\n");
        return;
    }
    uint32_t run = config.work_run > config.sleep_run
        ? config.work_run : config.sleep_run;
    size_t log_bytes = (size_t) config.buffer_kib * 1024u
        - 4u * (size_t) run;
    entropy_capture.config = config;
    entropy_capture.bytes = malloc(log_bytes);
    entropy_capture.run = malloc(4u * (size_t) run);
    if (entropy_capture.bytes == NULL || entropy_capture.run == NULL
        || !psp_entropy_capture_log_init(&entropy_capture.log,
                                         entropy_capture.bytes, log_bytes)) {
        printf("tilefinch-entropy-capture: refused reason=no-memory "
               "buffer-kib=%u\n", (unsigned) config.buffer_kib);
        entropy_capture_release();
        return;
    }
    entropy_capture.armed = true;
    psp_entropy_capture_set_sink(entropy_capture_sink, NULL);
    printf("tilefinch-entropy-capture: armed output=%s extra-rounds=%u "
           "work-run=%u sleep-run=%u buffer-kib=%u\n",
           entropy_capture.output_path, (unsigned) config.extra_rounds,
           (unsigned) config.work_run, (unsigned) config.sleep_run,
           (unsigned) config.buffer_kib);
}

static void entropy_capture_extra(PspEntropyCaptureSource source,
                                  uint32_t count)
{
    PspEntropyCaptureBatch batch;
    if (psp_entropy_capture_run(source, entropy_capture.run, count, &batch))
        (void) psp_entropy_capture_log_append(&entropy_capture.log, &batch,
                                              entropy_capture.run);
}

TILEFINCH_COLD_PATH void psp_validation_entropy_capture_finish(void)
{
    if (!entropy_capture.armed) return;
    /* A trace boot has just seeded; any other boot seeds now, so the file
       holds the boot's own batches (a no-op once the pool is released). */
    (void) psp_entropy_fill(NULL, 0u);
    uint64_t started = (uint64_t) sceKernelGetSystemTimeWide();
    for (uint32_t round = 0; round < entropy_capture.config.extra_rounds;
         round++) {
        entropy_capture_extra(PSP_ENTROPY_CAPTURE_SOURCE_WORK,
                              entropy_capture.config.work_run);
        entropy_capture_extra(PSP_ENTROPY_CAPTURE_SOURCE_SLEEP,
                              entropy_capture.config.sleep_run);
    }
    psp_entropy_capture_set_sink(NULL, NULL);
    uint64_t sampled_us = (uint64_t) sceKernelGetSystemTimeWide() - started;
    PspEntropyCaptureShape shape;
    psp_entropy_capture_shape(&shape);
    size_t length = psp_entropy_capture_log_finish(&entropy_capture.log,
                                                   &shape);
    FILE *file = fopen(entropy_capture.output_path, "wb");
    bool written = file != NULL
        && fwrite(entropy_capture.bytes, 1, length, file) == length;
    if (file != NULL && fclose(file) != 0) written = false;
    printf("tilefinch-entropy-capture: %s path=%s records=%u dropped=%u "
           "work-samples=%llu sleep-samples=%llu bytes=%zu extra-us=%llu\n",
           written ? "written" : "write-failed",
           entropy_capture.output_path,
           (unsigned) entropy_capture.log.records,
           (unsigned) entropy_capture.log.dropped,
           (unsigned long long) entropy_capture.log.samples[0],
           (unsigned long long) entropy_capture.log.samples[1], length,
           (unsigned long long) sampled_us);
    entropy_capture_release();
}

#else

void psp_validation_entropy_capture_arm(const TilefinchInstallPaths *paths)
{
    (void) paths;
}

void psp_validation_entropy_capture_finish(void)
{
}

#endif
