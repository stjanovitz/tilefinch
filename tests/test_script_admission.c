#include "tilefinch/browser_engine.h"
#include "tilefinch/script_admission.h"

#include <stdint.h>
#include <stdio.h>

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

#define KIB (1024u)
#define MIB (1024u * 1024u)

static void test_compile_model(void)
{
    /* The planning peak is the measured median: twice the source plus
       fixed parser state; the floor is the measured minimum. */
    CHECK(script_admission_compile_peak(0) == 256u * KIB);
    CHECK(script_admission_compile_peak(1u * MIB) == 2u * MIB + 256u * KIB);
    CHECK(script_admission_compile_floor(1u * MIB) == 1u * MIB + 16u * KIB);
    CHECK(script_admission_compile_floor(1u * MIB)
          < script_admission_compile_peak(1u * MIB));
    /* Saturating, never wrapping, on hostile sizes. */
    CHECK(script_admission_compile_peak(SIZE_MAX) == SIZE_MAX);
    CHECK(script_admission_compile_peak(SIZE_MAX / 2u + 1u) == SIZE_MAX);
    CHECK(script_admission_compile_floor(SIZE_MAX) == SIZE_MAX);
    /* The PSP app's per-script ceiling holds m.vk.com's 3.0 MiB
       application chunk. */
    CHECK((size_t) BROWSER_PSP_APP_SCRIPT_FILE_KB * KIB >= 3182206u);
}

static void test_reserves(void)
{
    /* A fixed floor plus a multiple of the source, at most the ceiling. */
    CHECK(script_admission_work_reserve(0, 4u, SIZE_MAX)
          == SCRIPT_ADMISSION_WORK_FLOOR_BYTES);
    CHECK(script_admission_work_reserve(
              100u * KIB, SCRIPT_ADMISSION_STORE_HEAP_MULTIPLIER, SIZE_MAX)
          == 464u * KIB);
    /* The dynamic reserve reaches the execution reserve at 56 KiB. */
    CHECK(script_admission_work_reserve(
              56u * KIB, SCRIPT_ADMISSION_DYNAMIC_HEAP_MULTIPLIER,
              SCRIPT_ADMISSION_EXECUTION_RESERVE_BYTES)
          == SCRIPT_ADMISSION_EXECUTION_RESERVE_BYTES);
    CHECK(script_admission_work_reserve(
              8u * KIB, SCRIPT_ADMISSION_DYNAMIC_HEAP_MULTIPLIER,
              SCRIPT_ADMISSION_EXECUTION_RESERVE_BYTES) == 128u * KIB);
    CHECK(script_admission_work_reserve(SIZE_MAX, 16u, SIZE_MAX) == SIZE_MAX);
    CHECK(script_admission_work_reserve(SIZE_MAX / 2u, 4u, 1u * MIB)
          == 1u * MIB);

    /* Affordable response bytes: what the Budget can stage beside the
       presentation reserve, never below the floor. */
    Budget budget;
    budget_init(&budget, 8u * MIB);
    CHECK(script_admission_affordable_bytes(&budget, 64u * KIB, false)
          == 8u * MIB - SCRIPT_ADMISSION_PRESENTATION_RESERVE_BYTES);
    void *held = budget_malloc_category(
        &budget, BUDGET_CATEGORY_RESOURCE, 6u * MIB);
    CHECK(held != NULL);
    CHECK(script_admission_affordable_bytes(&budget, 64u * KIB, false)
          == 64u * KIB);
    CHECK(script_admission_affordable_bytes(&budget, 1u * MIB, true)
          == 1u * MIB);
    budget_free(&budget, held);
    CHECK(budget.current == 0);
}

static void test_time_model(void)
{
    /* Device rates (PSP-3000, 2026-10-06): 10 s per MiB for a page to compile and run what it loads,
       7.1 s of that running, 50 ms per MiB to restore cached bytecode. */
    CHECK(script_admission_start_ms(1u * MIB) == 10000u);
    CHECK(script_admission_restore_ms(1u * MIB) == 50u);
    CHECK(script_admission_restore_ms(1) == 1u);
    CHECK(script_admission_restore_ms(SIZE_MAX) > 0u);
    CHECK(script_admission_start_ms(4u * MIB) == 40000u);
    CHECK(script_admission_run_ms(1u * MIB) == 7100u);
    /* The device check: xe.com's 2.3 MB and weather.com's 2.9 MB took
       15.5 s and 40.0 s of compile and run, m.vk.ru's first 1.9 MB 14.7 s
       (garbage collection excluded); the model says 22.7, 28.7 and 18.8. */
    CHECK(script_admission_start_ms(2383135u) == 22728u);
    /* 15 s is about 1.5 MiB of fresh script. */
    CHECK(script_admission_start_ms(3u * MIB / 2u) == SCRIPT_HEAVY_START_MS);
    CHECK(script_admission_disk_read_ms(1u * MIB) == 1000u);
    /* A cached bundle starts far faster than a fresh one, from RAM or
       from the Memory Stick tier. */
    CHECK(script_admission_restore_ms(1u * MIB)
              + script_admission_run_ms(1u * MIB)
          < script_admission_start_ms(1u * MIB));
    CHECK(script_admission_restore_ms(1u * MIB)
              + script_admission_disk_read_ms(1u * MIB)
              + script_admission_run_ms(1u * MIB)
          < script_admission_start_ms(1u * MIB));
}

static void test_classification(void)
{
    /* Light pages are not heavy, whatever their text. */
    CHECK(script_admission_classify(0, SCRIPT_HEAVY_START_MS - 1u, 0, 0, 0)
          == SCRIPT_HEAVY_CLASS_NONE);
    /* Heavy: an empty shell (b) or a page with content (a). */
    CHECK(script_admission_classify(
              SCRIPT_HEAVY_SHELL_TEXT_BYTES - 1u, SCRIPT_HEAVY_START_MS,
              0, 0, 0) == SCRIPT_HEAVY_CLASS_SHELL);
    CHECK(script_admission_classify(
              SCRIPT_HEAVY_SHELL_TEXT_BYTES, SCRIPT_HEAVY_START_MS,
              0, 0, 0) == SCRIPT_HEAVY_CLASS_CONTENT);
    /* Over the best case (c): waiting scripts whose floor or planned
       memory does not fit the heap the realm can reach, light or heavy. */
    CHECK(script_admission_classify(10000u, 1000u, 2u * MIB,
                                    3u * MIB, 2u * MIB)
          == SCRIPT_HEAVY_CLASS_OVER);
    CHECK(script_admission_classify(0, 1000u, 1u * MIB, 9u * MIB, 8u * MIB)
          == SCRIPT_HEAVY_CLASS_OVER);
    CHECK(script_admission_classify(0, 1000u, 1u * MIB, 3u * MIB, 8u * MIB)
          == SCRIPT_HEAVY_CLASS_NONE);
    /* m.vk.ru logged out: 5.3 MB of script, its 3.0 MiB chunk waiting,
       nothing visible, 8.4 MB of heap reachable. */
    size_t chunk = 3182206u;
    CHECK(script_admission_start_ms(5327443u) >= SCRIPT_HEAVY_START_MS);
    CHECK(script_admission_classify(
              0, script_admission_start_ms(5327443u), chunk,
              script_admission_run_memory(chunk), 8448179u)
          == SCRIPT_HEAVY_CLASS_OVER);
    /* The same page with room for it is an empty shell to ask about. */
    CHECK(script_admission_classify(
              0, script_admission_start_ms(5327443u), chunk,
              script_admission_run_memory(chunk), 16u * MIB)
          == SCRIPT_HEAVY_CLASS_SHELL);
    CHECK(script_admission_run_memory(SIZE_MAX) == SIZE_MAX);
}

int main(void)
{
    test_compile_model();
    test_reserves();
    test_time_model();
    test_classification();
    if (failures != 0) {
        fprintf(stderr, "script admission tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("script admission tests passed\n");
    return 0;
}
