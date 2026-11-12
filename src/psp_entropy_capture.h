#ifndef TILEFINCH_PSP_ENTROPY_CAPTURE_H
#define TILEFINCH_PSP_ENTROPY_CAPTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Validation-only capture of the raw timing samples the PSP entropy source
 * gathers (src/psp_entropy.c), so a host can look for structure the
 * per-batch estimator cannot see (tools/entropy_capture_report.py). Nothing
 * here is built into a shipping EBOOT: the hooks in psp_entropy.c exist only
 * under TILEFINCH_PSP_ENTROPY_CAPTURE, which cmake/PspOwnedTransport.cmake
 * sets for validation builds, and the recorder
 * (src/psp_app/psp_app_validation_entropy.c) is a validation unit.
 *
 * A capture boot's samples are that boot's pool input: anyone holding the
 * file and the seed file can narrow that boot's TLS keys. Validation builds
 * only, host0 only, and never a boot used for anything but measurement.
 *
 * File format "TFER" version 1, every integer little-endian:
 *   header, 40 bytes:
 *     0  "TFER"
 *     4  u16 version (1)          6  u16 header bytes (40)
 *     8  u16 record bytes (40)   10  u16 flags (bit 0: records dropped)
 *    12  u32 records written     16  u32 records dropped (buffer full)
 *    20  u32 work batch          24  u32 sleep batch
 *    28  u32 work touches        32  u32 sleep request (us)
 *    36  u32 clock (Hz, 1000000: the system timer every sample reads)
 *   then per record, 40 bytes followed by `count` u32 samples:
 *     0  u16 source (1 work, 2 sleep)
 *     2  u16 phase (PspEntropyCapturePhase)
 *     4  u32 record index (from 0, counting dropped ones)
 *     8  u32 count               12  u32 estimate (millibits per sample)
 *    16  u32 credited bits       20  u32 pool credited bits after
 *    24  u32 batch duration (us) 28  u32 reserved (0)
 *    32  u64 start (system time, us)
 *   An extra run (phase EXTRA) was never estimated or credited: its estimate
 *   and credited fields are 0xFFFFFFFF, and its samples are one contiguous
 *   run, not an estimator batch.
 */

#define PSP_ENTROPY_CAPTURE_VERSION 1u
#define PSP_ENTROPY_CAPTURE_HEADER_BYTES 40u
#define PSP_ENTROPY_CAPTURE_RECORD_BYTES 40u
#define PSP_ENTROPY_CAPTURE_NOT_ESTIMATED UINT32_MAX
#define PSP_ENTROPY_CAPTURE_FLAG_DROPPED 1u

typedef enum {
    PSP_ENTROPY_CAPTURE_SOURCE_WORK = 1,
    PSP_ENTROPY_CAPTURE_SOURCE_SLEEP = 2
} PspEntropyCaptureSource;

typedef enum {
    /* A batch of a foreground attempt (psp_entropy_fill). */
    PSP_ENTROPY_CAPTURE_PHASE_FILL = 1,
    /* A batch of the post-paint warm-up (psp_entropy_prepare_step). */
    PSP_ENTROPY_CAPTURE_PHASE_PREPARE = 2,
    /* A contiguous run taken only for the capture (psp_entropy_capture_run);
       it never reaches the pool. */
    PSP_ENTROPY_CAPTURE_PHASE_EXTRA = 3
} PspEntropyCapturePhase;

typedef struct {
    uint16_t source;
    uint16_t phase;
    uint32_t count;
    uint32_t millibits;
    uint32_t credited_bits;
    uint32_t pool_bits;
    uint32_t batch_us;
    uint64_t start_us;
} PspEntropyCaptureBatch;

typedef struct {
    uint32_t work_batch;
    uint32_t sleep_batch;
    uint32_t work_touches;
    uint32_t sleep_us;
} PspEntropyCaptureShape;

/* Called with the entropy lock held, after the pool took the batch, with
   the samples in the order they were measured (the pool's estimator sorts
   its own copy). It must only copy: no I/O, no entropy calls. */
typedef void (*PspEntropyCaptureSink)(const PspEntropyCaptureBatch *batch,
                                      const uint32_t *samples,
                                      void *context);

/* Defined in src/psp_entropy.c only under TILEFINCH_PSP_ENTROPY_CAPTURE;
   a shipping link that reached one would fail. */
void psp_entropy_capture_set_sink(PspEntropyCaptureSink sink, void *context);
void psp_entropy_capture_shape(PspEntropyCaptureShape *shape);
/* Measures `count` contiguous samples of one source into `samples`, with the
   sample functions and salt chain the pool's batches use, without touching
   the pool. Takes the entropy lock for the whole run. */
bool psp_entropy_capture_run(PspEntropyCaptureSource source,
                             uint32_t *samples, uint32_t count,
                             PspEntropyCaptureBatch *batch);

/* The in-memory file (pure; host-tested by tests/test_psp_entropy_capture.c). */
typedef struct {
    uint8_t *bytes;
    size_t capacity;
    size_t length;
    uint32_t records;
    uint32_t dropped;
    uint64_t samples[2];
} PspEntropyCaptureLog;

/* False when `capacity` cannot hold the header. */
bool psp_entropy_capture_log_init(PspEntropyCaptureLog *log, uint8_t *bytes,
                                  size_t capacity);
/* Appends one record; when it does not fit, counts it dropped instead. */
bool psp_entropy_capture_log_append(PspEntropyCaptureLog *log,
                                    const PspEntropyCaptureBatch *batch,
                                    const uint32_t *samples);
/* Writes the header; returns the file length. */
size_t psp_entropy_capture_log_finish(PspEntropyCaptureLog *log,
                                      const PspEntropyCaptureShape *shape);

/* entropy-capture.cfg, one key per line, each at most once, # comments:
 *   output=NAME        file written beside the program on host0:
 *                      ([A-Za-z0-9._-], 1..64, not starting with '.')
 *   extra_rounds=N     0..64 rounds of one work run and one sleep run
 *   work_run=N         256..16384 samples per extra work run
 *   sleep_run=N        64..4096 samples per extra sleep run
 *   buffer_kib=N       64..8192, the RAM the whole capture may use
 * output is required; the rest default to 16, 4096, 1024 and 1024. The
 * parser refuses a buffer_kib smaller than psp_entropy_capture_config_bytes. */
#define PSP_ENTROPY_CAPTURE_OUTPUT_LIMIT 64u

typedef struct {
    char output[PSP_ENTROPY_CAPTURE_OUTPUT_LIMIT + 1u];
    uint32_t extra_rounds;
    uint32_t work_run;
    uint32_t sleep_run;
    uint32_t buffer_kib;
} PspEntropyCaptureConfig;

/* The boot's own batches: two full 128-batch attempts of alternating work
   and sleep batches, with their record headers. */
#define PSP_ENTROPY_CAPTURE_BOOT_RESERVE (176u * 1024u)

/* What a config needs at most: header, boot reserve, every extra record,
   and the one run buffer the recorder samples into. */
uint64_t psp_entropy_capture_config_bytes(
    const PspEntropyCaptureConfig *config);

bool psp_entropy_capture_config_parse(const char *text, size_t length,
                                      PspEntropyCaptureConfig *config,
                                      char *why, size_t why_size);

#endif
