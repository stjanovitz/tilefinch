#include "tilefinch/psp_entropy.h"
#include "psp_entropy_pool.h"

#include <pspkernel.h>
#include <pspsysmem.h>
#include <pspthreadman.h>
#include <psputils.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/*
 * Sample sources. Both are measured with the 1 MHz system timer, the finest
 * clock user mode can read (CP0 Count and the hardware timer registers are
 * kernel-only, and every read is a syscall).
 *
 * Work: a fixed run of read-modify-writes through the uncached alias of a
 * small buffer, so every access goes to DDR and its duration moves with bus
 * arbitration against the Media Engine, GE and DMA, with interrupt entry,
 * and with the phase of the timer tick against the CPU clock.
 * Sleep: the observed length of the shortest sceKernelDelayThread, which is
 * the timer interrupt's latency plus whatever other threads and interrupt
 * handlers ran before this thread was dispatched again.
 *
 * Batches alternate so a regime that silences one source (an idle bus, an
 * empty ready queue) still leaves the other. The attempt bounds keep a
 * source that has gone quiet from stalling the caller: the browser thread
 * performs the first seeding inside curl's global initialization.
 */
#define PSP_ENTROPY_WORK_BATCH 256u
#define PSP_ENTROPY_SLEEP_BATCH 64u
#define PSP_ENTROPY_WORK_TOUCHES 32u
#define PSP_ENTROPY_WORK_WORDS 1024u
#define PSP_ENTROPY_SLEEP_US 1u
#define PSP_ENTROPY_ATTEMPT_US UINT64_C(750000)
#define PSP_ENTROPY_ATTEMPT_BATCHES 128u
#define PSP_ENTROPY_UNCACHED UINT32_C(0x40000000)
#define PSP_ENTROPY_REPORT_BYTES 320u
#define PSP_ENTROPY_PATH_BYTES 768u

_Static_assert(PSP_ENTROPY_WORK_BATCH <= PSP_ENTROPY_MAX_BATCH
                   && PSP_ENTROPY_SLEEP_BATCH >= PSP_ENTROPY_MIN_BATCH,
               "sample batches must be estimable");
_Static_assert((PSP_ENTROPY_WORK_WORDS & (PSP_ENTROPY_WORK_WORDS - 1u)) == 0,
               "the work index is masked");

typedef struct {
    uint32_t batches;
    uint32_t samples;
    uint32_t minimum_millibits;
    uint32_t maximum_millibits;
    uint64_t sample_us;
} PspEntropySourceStats;

typedef struct {
    PspEntropyPool pool;
    PspEntropyPolicy policy;
    PspEntropyReport report;
    SceUID lock;
    bool boot_mixed;
    bool released;
    bool released_credited;
    uint32_t attempts;
    uint64_t gather_us;
    PspEntropySourceStats work;
    PspEntropySourceStats sleep;
    const char *seed_status;
    char seed_path[PSP_ENTROPY_PATH_BYTES];
} PspEntropyState;

static PspEntropyState psp_entropy = {
    .lock = -1,
    .seed_status = "none",
};
static uint32_t psp_entropy_samples[PSP_ENTROPY_MAX_BATCH];
static uint32_t psp_entropy_work_area[PSP_ENTROPY_WORK_WORDS]
    __attribute__((aligned(64)));
static bool psp_entropy_work_area_flushed;

static void psp_entropy_emit(const char *line)
{
    if (psp_entropy.report != NULL) {
        psp_entropy.report(line);
        return;
    }
    fputs(line, stdout);
    fputc('\n', stdout);
}

void psp_entropy_configure(const char *seed_path, PspEntropyPolicy policy,
                           PspEntropyReport report)
{
    psp_entropy.policy = policy;
    psp_entropy.report = report;
    psp_entropy.seed_path[0] = '\0';
    if (seed_path != NULL) {
        size_t length = strlen(seed_path);
        if (length < sizeof(psp_entropy.seed_path))
            memcpy(psp_entropy.seed_path, seed_path, length + 1u);
    }
    if (psp_entropy.lock < 0)
        psp_entropy.lock = sceKernelCreateSema(
            "tilefinch-entropy", 0, 1, 1, NULL);
}

static bool psp_entropy_lock(void)
{
    /* An unconfigured process (the launcher before configure, a probe) is
       single-threaded when it first gets here. */
    if (psp_entropy.lock < 0)
        psp_entropy.lock = sceKernelCreateSema(
            "tilefinch-entropy", 0, 1, 1, NULL);
    return psp_entropy.lock >= 0
        && sceKernelWaitSema(psp_entropy.lock, 1, NULL) >= 0;
}

static void psp_entropy_unlock(void)
{
    (void) sceKernelSignalSema(psp_entropy.lock, 1);
}

static uint32_t psp_entropy_sample_work(uint32_t salt)
{
    if (!psp_entropy_work_area_flushed) {
        /* The loader cleared this buffer through the cache; write those
           lines back once so the uncached walk owns the memory outright. */
        sceKernelDcacheWritebackInvalidateRange(
            psp_entropy_work_area, sizeof(psp_entropy_work_area));
        psp_entropy_work_area_flushed = true;
    }
    volatile uint32_t *words = (volatile uint32_t *) (
        (uintptr_t) psp_entropy_work_area | PSP_ENTROPY_UNCACHED);
    uint32_t state = salt | 1u;
    uint32_t started = sceKernelGetSystemTimeLow();
    for (uint32_t touch = 0; touch < PSP_ENTROPY_WORK_TOUCHES; touch++) {
        state = state * UINT32_C(1664525) + UINT32_C(1013904223);
        words[(state >> 16) & (PSP_ENTROPY_WORK_WORDS - 1u)] += state;
    }
    return sceKernelGetSystemTimeLow() - started;
}

static uint32_t psp_entropy_sample_sleep(void)
{
    uint32_t started = sceKernelGetSystemTimeLow();
    (void) sceKernelDelayThread(PSP_ENTROPY_SLEEP_US);
    return sceKernelGetSystemTimeLow() - started;
}

static void psp_entropy_note(PspEntropySourceStats *stats, uint32_t count,
                             uint32_t millibits, uint64_t elapsed_us)
{
    if (stats->batches == 0u || millibits < stats->minimum_millibits)
        stats->minimum_millibits = millibits;
    if (millibits > stats->maximum_millibits)
        stats->maximum_millibits = millibits;
    stats->batches++;
    stats->samples += count;
    stats->sample_us += elapsed_us;
}

/* Context that differs across boots and devices but that nobody should
   count on: it keeps two unseeded pools apart, nothing more. */
static void psp_entropy_mix_boot_context(void)
{
    struct {
        uint64_t system_us;
        int64_t wall_seconds;
        uint32_t free_bytes;
        int32_t thread;
        uintptr_t stack;
    } context;
    memset(&context, 0, sizeof(context));
    context.system_us = (uint64_t) sceKernelGetSystemTimeWide();
    context.wall_seconds = (int64_t) time(NULL);
    context.free_bytes = (uint32_t) sceKernelTotalFreeMemSize();
    context.thread = (int32_t) sceKernelGetThreadId();
    context.stack = (uintptr_t) &context;
    psp_entropy_pool_mix(&psp_entropy.pool, PSP_ENTROPY_DOMAIN_BOOT_CONTEXT,
                         &context, sizeof(context));
}

static void psp_entropy_mix_seed_file(void)
{
    if (psp_entropy.seed_path[0] == '\0') return;
    uint8_t file[PSP_ENTROPY_SEED_FILE_BYTES + 1u];
    size_t length = 0;
    FILE *stream = fopen(psp_entropy.seed_path, "rb");
    if (stream != NULL) {
        length = fread(file, 1, sizeof(file), stream);
        fclose(stream);
    }
    uint8_t seed[PSP_ENTROPY_SEED_BYTES];
    if (stream == NULL) {
        psp_entropy.seed_status = "absent";
    } else if (psp_entropy_seed_file_decode(file, length, seed)) {
        psp_entropy_pool_mix(&psp_entropy.pool, PSP_ENTROPY_DOMAIN_SEED_FILE,
                             seed, sizeof(seed));
        psp_entropy.seed_status = "loaded";
    } else {
        psp_entropy.seed_status = "invalid";
    }
    psp_entropy_zeroize(seed, sizeof(seed));
    psp_entropy_zeroize(file, sizeof(file));
}

static void psp_entropy_gather_locked(void)
{
    uint64_t started = (uint64_t) sceKernelGetSystemTimeWide();
    uint32_t salt = (uint32_t) started;
    for (uint32_t batch = 0;
         batch < PSP_ENTROPY_ATTEMPT_BATCHES
             && !psp_entropy_pool_ready(&psp_entropy.pool);
         batch++) {
        uint64_t batch_started = (uint64_t) sceKernelGetSystemTimeWide();
        if (batch_started - started >= PSP_ENTROPY_ATTEMPT_US) break;
        bool sleep = (batch & 1u) != 0u;
        uint32_t count = sleep ? PSP_ENTROPY_SLEEP_BATCH
                               : PSP_ENTROPY_WORK_BATCH;
        for (uint32_t index = 0; index < count; index++) {
            uint32_t sample = sleep ? psp_entropy_sample_sleep()
                                    : psp_entropy_sample_work(salt);
            psp_entropy_samples[index] = sample;
            salt = salt * UINT32_C(2654435761) + sample;
        }
        uint64_t batch_us =
            (uint64_t) sceKernelGetSystemTimeWide() - batch_started;
        uint32_t credited = 0;
        uint32_t millibits = psp_entropy_pool_add_batch(
            &psp_entropy.pool,
            sleep ? PSP_ENTROPY_DOMAIN_SLEEP_TIMING
                  : PSP_ENTROPY_DOMAIN_WORK_TIMING,
            psp_entropy_samples, count, &credited);
        psp_entropy_note(sleep ? &psp_entropy.sleep : &psp_entropy.work,
                         count, millibits, batch_us);
    }
    psp_entropy_zeroize(psp_entropy_samples, sizeof(psp_entropy_samples));
    psp_entropy.attempts++;
    psp_entropy.gather_us +=
        (uint64_t) sceKernelGetSystemTimeWide() - started;
}

static void psp_entropy_report_state(const char *event)
{
    char line[PSP_ENTROPY_REPORT_BYTES];
    const PspEntropySourceStats *work = &psp_entropy.work;
    const PspEntropySourceStats *sleep = &psp_entropy.sleep;
    snprintf(line, sizeof(line),
             "tilefinch-entropy: event=%s credited-bits=%u target=%u "
             "attempts=%u elapsed-us=%llu seed-file=%s "
             "work-batches=%u work-mbits=%u..%u work-sample-us=%llu "
             "sleep-batches=%u sleep-mbits=%u..%u sleep-sample-us=%llu",
             event, (unsigned) psp_entropy.pool.credited_bits,
             (unsigned) PSP_ENTROPY_TARGET_BITS,
             (unsigned) psp_entropy.attempts,
             (unsigned long long) psp_entropy.gather_us,
             psp_entropy.seed_status,
             (unsigned) work->batches, (unsigned) work->minimum_millibits,
             (unsigned) work->maximum_millibits,
             (unsigned long long) (work->samples == 0u
                 ? 0u : work->sample_us / work->samples),
             (unsigned) sleep->batches, (unsigned) sleep->minimum_millibits,
             (unsigned) sleep->maximum_millibits,
             (unsigned long long) (sleep->samples == 0u
                 ? 0u : sleep->sample_us / sleep->samples));
    psp_entropy_emit(line);
}

static void psp_entropy_write_seed(const uint8_t seed[PSP_ENTROPY_SEED_BYTES])
{
    uint8_t file[PSP_ENTROPY_SEED_FILE_BYTES];
    psp_entropy_seed_file_encode(seed, file);
    FILE *stream = fopen(psp_entropy.seed_path, "wb");
    bool written = stream != NULL
        && fwrite(file, 1, sizeof(file), stream) == sizeof(file);
    if (stream != NULL && fclose(stream) != 0) written = false;
    psp_entropy_zeroize(file, sizeof(file));
    psp_entropy_emit(written ? "tilefinch-entropy: seed-file=written"
                             : "tilefinch-entropy: seed-file=write-failed");
}

static void psp_entropy_mix_poll_context(void)
{
    uint64_t now = (uint64_t) sceKernelGetSystemTimeWide();
    psp_entropy_pool_mix(&psp_entropy.pool, PSP_ENTROPY_DOMAIN_POLL_CONTEXT,
                         &now, sizeof(now));
}

bool psp_entropy_fill(void *output, size_t length)
{
    if (output == NULL && length != 0) return false;
    if (!psp_entropy_lock()) return false;
    bool write_seed = false;
    uint8_t next_seed[PSP_ENTROPY_SEED_BYTES];
    if (!psp_entropy.released) {
        if (!psp_entropy.boot_mixed) {
            psp_entropy_mix_boot_context();
            psp_entropy_mix_seed_file();
            psp_entropy.boot_mixed = true;
        }
        psp_entropy_gather_locked();
        if (psp_entropy_pool_ready(&psp_entropy.pool)) {
            psp_entropy.released = true;
            psp_entropy.released_credited = true;
            psp_entropy_report_state("seeded mode=credited");
        } else if (psp_entropy.policy == PSP_ENTROPY_PERMIT_UNCREDITED) {
            psp_entropy.released = true;
            psp_entropy_report_state("seeded mode=uncredited");
        } else {
            psp_entropy_report_state("unseeded");
            psp_entropy_unlock();
            return false;
        }
        /* The successor is drawn before any caller output, so the file never
           equals anything this boot hands out; it carries this boot's
           entropy forward without ever being credited next time. */
        if (psp_entropy.seed_path[0] != '\0') {
            psp_entropy_pool_generate(&psp_entropy.pool, next_seed,
                                      sizeof(next_seed));
            write_seed = true;
        }
    }
    psp_entropy_mix_poll_context();
    psp_entropy_pool_generate(&psp_entropy.pool, output, length);
    psp_entropy_unlock();
    if (write_seed) {
        psp_entropy_write_seed(next_seed);
        psp_entropy_zeroize(next_seed, sizeof(next_seed));
    }
    return true;
}

void psp_entropy_fill_best_effort(void *output, size_t length)
{
    if (output == NULL || length == 0) return;
    if (!psp_entropy_lock()) {
        /* Only when the semaphore could not be created: a private pool
           keyed by the clock still varies per call. */
        PspEntropyPool fallback;
        psp_entropy_pool_init(&fallback);
        uint64_t now = (uint64_t) sceKernelGetSystemTimeWide();
        psp_entropy_pool_mix(&fallback, PSP_ENTROPY_DOMAIN_POLL_CONTEXT,
                             &now, sizeof(now));
        psp_entropy_pool_generate(&fallback, output, length);
        psp_entropy_zeroize(&fallback, sizeof(fallback));
        return;
    }
    if (!psp_entropy.boot_mixed) {
        psp_entropy_mix_boot_context();
        psp_entropy_mix_seed_file();
        psp_entropy.boot_mixed = true;
    }
    if (!psp_entropy.released) {
        /* One timing sample per call: cheap, and it makes successive
           pre-seed outputs depend on more than the call count. */
        uint32_t sample = psp_entropy_sample_work(
            (uint32_t) sceKernelGetSystemTimeWide());
        psp_entropy_pool_mix(&psp_entropy.pool,
                             PSP_ENTROPY_DOMAIN_WORK_TIMING,
                             &sample, sizeof(sample));
    }
    psp_entropy_mix_poll_context();
    psp_entropy_pool_generate(&psp_entropy.pool, output, length);
    psp_entropy_unlock();
}
