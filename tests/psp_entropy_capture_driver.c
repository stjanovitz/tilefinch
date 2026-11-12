/* src/psp_entropy.c on the host, built with its validation capture hooks
 * (TILEFINCH_PSP_ENTROPY_CAPTURE), over a modelled system timer.
 *
 *   tilefinch-psp-entropy-capture-driver beat|jitter OUT.tfer EXPECTED.bin
 *
 * beat:   every work sample lasts exactly 7.2 us and every sleep 230.37 us,
 *         with fixed gaps, read on a 1 us clock: two clocks beating, no
 *         entropy at all.
 * jitter: the same sources with independent random durations and gaps.
 *
 * Runs three warm-up batches (psp_entropy_prepare_step), one foreground
 * attempt (psp_entropy_fill), then three rounds of extra runs
 * (psp_entropy_capture_run), recording through the same sink and encoder the
 * PSP recorder uses (src/psp_app/psp_app_validation_entropy.c). Writes the
 * capture to OUT.tfer and, independently, every sample the modelled clock
 * produced, in order, to EXPECTED.bin (u32 LE). Prints a summary line;
 * tests/test_entropy_capture_report.py checks both files.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pspkernel.h"

static double clock_us = 1000.0;
static bool jitter_mode;
static uint64_t random_state = UINT64_C(0x9e3779b97f4a7c15);
static bool reading_open;
static bool delayed;
static uint32_t opened_at;
static uint32_t *expected;
static size_t expected_count;
static size_t expected_capacity;

static double model_uniform(void)
{
    /* splitmix64 */
    uint64_t z = (random_state += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    z ^= z >> 31;
    return (double) (z >> 11) / 9007199254740992.0;
}

SceUID sceKernelCreateSema(const char *name, int attributes, int initial,
                           int maximum, void *options)
{
    (void) name; (void) attributes; (void) initial; (void) maximum;
    (void) options;
    return 1;
}
int sceKernelWaitSema(SceUID id, int count, unsigned *timeout)
{
    (void) id; (void) count; (void) timeout;
    return 0;
}
int sceKernelPollSema(SceUID id, int count)
{
    (void) id; (void) count;
    return 0;
}
int sceKernelSignalSema(SceUID id, int count)
{
    (void) id; (void) count;
    return 0;
}
int64_t sceKernelGetSystemTimeWide(void)
{
    return (int64_t) floor(clock_us);
}

/* psp_entropy.c reads this only around one sample: an opening read, the
   work or the sleep, a closing read. A closing read with no sleep between
   is a work sample. */
unsigned sceKernelGetSystemTimeLow(void)
{
    if (!reading_open) {
        reading_open = true;
        delayed = false;
        opened_at = (uint32_t) floor(clock_us);
        return opened_at;
    }
    if (!delayed)
        clock_us += jitter_mode ? 6.0 + 3.0 * model_uniform() : 7.2;
    uint32_t closed_at = (uint32_t) floor(clock_us);
    if (expected_count == expected_capacity) {
        expected_capacity = expected_capacity == 0 ? 65536u
                                                   : 2u * expected_capacity;
        expected = realloc(expected, expected_capacity * sizeof(*expected));
        if (expected == NULL) abort();
    }
    expected[expected_count++] = closed_at - opened_at;
    clock_us += jitter_mode ? 5.0 * model_uniform() : 0.53;
    reading_open = false;
    return closed_at;
}
int sceKernelDelayThread(unsigned microseconds)
{
    (void) microseconds;
    delayed = true;
    clock_us += jitter_mode ? 200.0 + 60.0 * model_uniform() : 230.37;
    return 0;
}
int sceKernelGetThreadId(void)
{
    return 1;
}
size_t sceKernelTotalFreeMemSize(void)
{
    return 1024u * 1024u;
}
void sceKernelDcacheWritebackInvalidateRange(const void *address,
                                             unsigned size)
{
    (void) address; (void) size;
}

#include "psp_entropy.c"

static void driver_sink(const PspEntropyCaptureBatch *batch,
                        const uint32_t *samples, void *context)
{
    (void) psp_entropy_capture_log_append(context, batch, samples);
}

static void driver_quiet(const char *line)
{
    (void) line;
}

static bool write_file(const char *path, const void *bytes, size_t length)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) return false;
    bool written = fwrite(bytes, 1, length, file) == length;
    return fclose(file) == 0 && written;
}

int main(int argc, char **argv)
{
    if (argc != 4 || (strcmp(argv[1], "beat") != 0
                      && strcmp(argv[1], "jitter") != 0)) {
        fprintf(stderr, "usage: %s beat|jitter OUT.tfer EXPECTED.bin\n",
                argv[0]);
        return 2;
    }
    jitter_mode = strcmp(argv[1], "jitter") == 0;
    size_t capacity = 8u * 1024u * 1024u;
    uint8_t *bytes = malloc(capacity);
    uint32_t *run = malloc(8192u * sizeof(uint32_t));
    PspEntropyCaptureLog log;
    if (bytes == NULL || run == NULL
        || !psp_entropy_capture_log_init(&log, bytes, capacity))
        return 1;

    psp_entropy_configure(NULL, PSP_ENTROPY_PERMIT_UNCREDITED, driver_quiet);
    psp_entropy_capture_set_sink(driver_sink, &log);
    /* The first step only mixes boot context; three more gather. */
    for (unsigned step = 0; step < 4u; step++)
        if (psp_entropy_prepare_step() != PSP_ENTROPY_PREPARE_PENDING)
            return 1;
    if (!psp_entropy_fill(NULL, 0u)) return 1;

    PspEntropyPool before = psp_entropy.pool;
    for (unsigned round = 0; round < 3u; round++) {
        PspEntropyCaptureBatch batch;
        if (!psp_entropy_capture_run(PSP_ENTROPY_CAPTURE_SOURCE_WORK, run,
                                     8192u, &batch))
            return 1;
        (void) psp_entropy_capture_log_append(&log, &batch, run);
        if (!psp_entropy_capture_run(PSP_ENTROPY_CAPTURE_SOURCE_SLEEP, run,
                                     1024u, &batch))
            return 1;
        (void) psp_entropy_capture_log_append(&log, &batch, run);
    }
    bool pool_unchanged =
        memcmp(&before, &psp_entropy.pool, sizeof(before)) == 0;
    psp_entropy_capture_set_sink(NULL, NULL);

    PspEntropyCaptureShape shape;
    psp_entropy_capture_shape(&shape);
    size_t length = psp_entropy_capture_log_finish(&log, &shape);
    if (!write_file(argv[2], bytes, length)
        || !write_file(argv[3], expected, expected_count * sizeof(uint32_t)))
        return 1;
    printf("entropy-capture-driver: mode=%s records=%u dropped=%u "
           "expected=%zu pool-credited=%u released-credited=%d "
           "pool-unchanged-by-extras=%d\n", argv[1], (unsigned) log.records,
           (unsigned) log.dropped, expected_count,
           (unsigned) psp_entropy.pool.credited_bits,
           psp_entropy.released_credited ? 1 : 0, pool_unchanged ? 1 : 0);
    free(expected);
    free(run);
    free(bytes);
    return 0;
}
