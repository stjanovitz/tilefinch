#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspkernel.h>
#include <psppower.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>

PSP_MODULE_INFO("PSPBrowserValidation", 0, 3, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);

#define SCREEN_WIDTH 480
#define SCREEN_HEIGHT 272
#define FRAME_STRIDE 512
#define TILE_SIZE 128
#define TILE_CAPACITY 16
#define VALIDATION_SCHEMA "psp-browser-validation-v3"
#define SUITE_SAMPLES 3
#define STRESS_SLOTS 96
#define STRESS_ROUNDS 16
#define FRAGMENT_SLOTS 48
#define IO_BYTES (256u * 1024u)
#define IO_CHUNK (32u * 1024u)
#define REQUIRED_CONTIGUOUS_BYTES (24u * 1024u * 1024u)
#define MAX_COMPOSE_US 200000u
#define MAX_SEQUENCE_US 350000u
#define EXPECTED_OVERLAY_CHECKSUM 0x2a9fbb9au

enum {
    FAILURE_CACHE_ALLOC = 1u << 0,
    FAILURE_MEMORY_HEADROOM = 1u << 1,
    FAILURE_ALLOCATOR_STRESS = 1u << 2,
    FAILURE_FRAGMENT_RECOVERY = 1u << 3,
    FAILURE_TILE_CHECKSUM = 1u << 4,
    FAILURE_TILE_TIMING = 1u << 5,
    FAILURE_PRIMITIVES = 1u << 6,
    FAILURE_STORAGE_IO = 1u << 7
};

typedef struct {
    int valid;
    int tile_x;
    int tile_y;
    uint32_t last_used;
    uint16_t pixels[TILE_SIZE * TILE_SIZE];
} ValidationTile;

typedef struct {
    ValidationTile *tiles;
    int capacity;
    uint32_t clock;
    uint32_t hits;
    uint32_t misses;
    uint32_t evictions;
} ValidationCache;

typedef struct {
    int capacity;
    int passed;
    uint32_t hits;
    uint32_t misses;
    uint32_t evictions;
    uint64_t total_us;
    uint64_t compose_us;
    uint64_t total_min_us;
    uint64_t total_max_us;
    uint64_t compose_min_us;
    uint64_t compose_max_us;
    unsigned samples;
    uint32_t checksum_top;
    uint32_t checksum_middle;
} SuiteResult;

typedef struct {
    int passed;
    unsigned rounds;
    unsigned allocations;
    size_t bytes_exercised;
    unsigned failures;
    uint64_t total_us;
} AllocatorStressResult;

typedef struct {
    int passed;
    unsigned blocks_allocated;
    size_t bytes_allocated;
    uint32_t free_before;
    uint32_t free_fragmented;
    uint32_t free_after;
    size_t largest_fragmented;
    size_t largest_recovered;
    uint64_t total_us;
} FragmentResult;

typedef struct {
    int passed;
    size_t bytes;
    size_t bytes_written;
    size_t bytes_read;
    uint32_t write_checksum;
    uint32_t read_checksum;
    uint64_t write_us;
    uint64_t read_us;
    int remove_result;
} StorageResult;

typedef struct {
    uint32_t devkit_version;
    int cpu_mhz;
    int bus_mhz;
    int power_online;
    int battery_present;
    int battery_charging;
    int battery_percent;
    int battery_minutes;
    int battery_temperature;
    int battery_voltage_mv;
} SystemResult;

typedef struct {
    int passed;
    unsigned scroll_samples;
    unsigned rounded_samples;
    unsigned rounded_failures;
    unsigned fixed_failures;
    uint32_t overlay_checksum_first;
    uint32_t overlay_checksum_middle;
    uint32_t overlay_checksum_last;
    uint16_t base_sample_first;
    uint16_t base_sample_middle;
    uint16_t base_sample_last;
    uint64_t base_us;
    uint64_t overlay_us;
} PrimitiveResult;

static int exit_callback(int arg1, int arg2, void *common)
{
    (void) arg1;
    (void) arg2;
    (void) common;
    sceKernelExitGame();
    return 0;
}

static int callback_thread(SceSize args, void *argp)
{
    (void) args;
    (void) argp;
    int callback = sceKernelCreateCallback("exit_callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(callback);
    sceKernelSleepThreadCB();
    return 0;
}

static void setup_callbacks(void)
{
    int thread = sceKernelCreateThread("callback_thread", callback_thread,
                                       0x11, 0xFA0, 0, NULL);
    if (thread >= 0) sceKernelStartThread(thread, 0, NULL);
}

static uint16_t rgb565(unsigned red, unsigned green, unsigned blue)
{
    return (uint16_t) (((red >> 3) << 11) | ((green >> 2) << 5)
                       | (blue >> 3));
}

static uint16_t synthetic_page_pixel(int x, int y)
{
    if (y < 48) return rgb565(35, 59, 100);
    int card = (y - 62) / 96;
    int within = (y - 62) % 96;
    if (y >= 62 && within >= 0 && within < 76 && x >= 14 && x < 466) {
        if (within < 7) return rgb565(47, 95 + (unsigned) (card % 3) * 20, 132);
        if ((within >= 20 && within < 25 && x >= 28 && x < 360)
            || (within >= 35 && within < 39 && x >= 28 && x < 440)
            || (within >= 48 && within < 52 && x >= 28 && x < 410)) {
            return rgb565(45, 59, 70);
        }
        return (card & 1) ? rgb565(239, 244, 248) : rgb565(255, 255, 255);
    }
    return rgb565(244, 241, 232);
}

static int rounded_contains(int x, int y, int left, int top,
                            int width, int height, int radius)
{
    int maximum_radius = width < height ? width / 2 : height / 2;
    if (radius > maximum_radius) radius = maximum_radius;
    if (radius <= 0) return x >= left && x < left + width
                            && y >= top && y < top + height;
    if (x < left || x >= left + width || y < top || y >= top + height) {
        return 0;
    }
    int center_x = x < left + radius
                   ? left + radius - 1
                   : (x >= left + width - radius
                      ? left + width - radius : x);
    int center_y = y < top + radius
                   ? top + radius - 1
                   : (y >= top + height - radius
                      ? top + height - radius : y);
    int dx = x - center_x;
    int dy = y - center_y;
    return dx * dx + dy * dy < radius * radius;
}

static void fill_rect(volatile uint16_t *frame, int left, int top,
                      int width, int height, uint16_t color)
{
    int right = left + width;
    int bottom = top + height;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > SCREEN_WIDTH) right = SCREEN_WIDTH;
    if (bottom > SCREEN_HEIGHT) bottom = SCREEN_HEIGHT;
    for (int y = top; y < bottom; y++) {
        for (int x = left; x < right; x++) {
            frame[y * FRAME_STRIDE + x] = color;
        }
    }
}

static void fill_rounded_rect(volatile uint16_t *frame, int left, int top,
                              int width, int height, int radius,
                              uint16_t color)
{
    for (int y = top; y < top + height; y++) {
        if (y < 0 || y >= SCREEN_HEIGHT) continue;
        for (int x = left; x < left + width; x++) {
            if (x < 0 || x >= SCREEN_WIDTH) continue;
            if (rounded_contains(x, y, left, top, width, height, radius)) {
                frame[y * FRAME_STRIDE + x] = color;
            }
        }
    }
}

static void draw_fixed_composer(volatile uint16_t *frame)
{
    uint16_t border = rgb565(217, 217, 217);
    uint16_t white = rgb565(255, 255, 255);
    uint16_t ink = rgb565(13, 13, 13);
    uint16_t quiet = rgb565(242, 242, 242);
    uint16_t muted = rgb565(138, 138, 138);

    /* ChatGPT-shaped fixed chrome using the same geometry as the lab. */
    fill_rounded_rect(frame, 222, 108, 36, 36, 18, quiet);
    fill_rect(frame, 239, 118, 2, 14, ink);
    fill_rounded_rect(frame, 12, 150, 456, 85, 26, border);
    fill_rounded_rect(frame, 13, 151, 454, 83, 25, white);
    fill_rect(frame, 31, 212, 15, 1, ink);
    fill_rect(frame, 38, 205, 1, 15, ink);
    fill_rect(frame, 351, 203, 3, 13, ink);
    fill_rect(frame, 349, 218, 11, 1, ink);
    fill_rounded_rect(frame, 377, 193, 82, 36, 18, quiet);
    fill_rect(frame, 390, 205, 2, 16, ink);
    fill_rect(frame, 396, 208, 2, 10, ink);
    fill_rect(frame, 0, 236, 480, 36, white);
    fill_rect(frame, 90, 249, 300, 1, muted);
}

static uint32_t fixed_overlay_checksum(const volatile uint16_t *frame)
{
    uint32_t hash = 2166136261u;
    for (int y = 150; y < 235; y++) {
        for (int x = 12; x < 468; x++) {
            if (!rounded_contains(x, y, 12, 150, 456, 85, 26)) continue;
            uint16_t value = frame[y * FRAME_STRIDE + x];
            hash = (hash ^ (value & 0xffu)) * 16777619u;
            hash = (hash ^ (value >> 8)) * 16777619u;
        }
    }
    return hash;
}

static void run_primitive_test(volatile uint16_t *frame,
                               PrimitiveResult *result)
{
    static const int scrolls[3] = {0, 136, 272};
    uint32_t checksums[3] = {0, 0, 0};
    uint16_t base_samples[3] = {0, 0, 0};
    memset(result, 0, sizeof(*result));
    result->scroll_samples = 3;
    result->rounded_samples = 6u * result->scroll_samples;

    for (unsigned sample = 0; sample < result->scroll_samples; sample++) {
        int scroll_y = scrolls[sample];
        uint64_t started = sceKernelGetSystemTimeWide();
        for (int y = 0; y < SCREEN_HEIGHT; y++) {
            for (int x = 0; x < SCREEN_WIDTH; x++) {
                frame[y * FRAME_STRIDE + x] =
                    synthetic_page_pixel(x, scroll_y + y);
            }
        }
        result->base_us += sceKernelGetSystemTimeWide() - started;
        base_samples[sample] = frame[100 * FRAME_STRIDE + 100];

        uint16_t corner_before = frame[150 * FRAME_STRIDE + 12];
        started = sceKernelGetSystemTimeWide();
        draw_fixed_composer(frame);
        result->overlay_us += sceKernelGetSystemTimeWide() - started;
        checksums[sample] = fixed_overlay_checksum(frame);

        if (frame[150 * FRAME_STRIDE + 12] != corner_before) {
            result->rounded_failures++;
        }
        if (frame[150 * FRAME_STRIDE + 240] != rgb565(217, 217, 217)) {
            result->rounded_failures++;
        }
        if (frame[151 * FRAME_STRIDE + 240] != rgb565(255, 255, 255)) {
            result->rounded_failures++;
        }
        if (frame[234 * FRAME_STRIDE + 240] != rgb565(217, 217, 217)) {
            result->rounded_failures++;
        }
        if (frame[192 * FRAME_STRIDE + 12] != rgb565(217, 217, 217)) {
            result->rounded_failures++;
        }
        if (frame[192 * FRAME_STRIDE + 13] != rgb565(255, 255, 255)) {
            result->rounded_failures++;
        }
    }

    result->overlay_checksum_first = checksums[0];
    result->overlay_checksum_middle = checksums[1];
    result->overlay_checksum_last = checksums[2];
    result->base_sample_first = base_samples[0];
    result->base_sample_middle = base_samples[1];
    result->base_sample_last = base_samples[2];
    if (checksums[0] != checksums[1] || checksums[1] != checksums[2]) {
        result->fixed_failures++;
    }
    if (checksums[0] != EXPECTED_OVERLAY_CHECKSUM) {
        result->fixed_failures++;
    }
    if (base_samples[0] == base_samples[1]
        && base_samples[1] == base_samples[2]) {
        result->fixed_failures++;
    }
    result->passed = result->rounded_failures == 0
                     && result->fixed_failures == 0;
    sceKernelDcacheWritebackInvalidateAll();
    sceDisplayWaitVblankStart();
}

static void rasterize_tile(ValidationTile *tile)
{
    int origin_x = tile->tile_x * TILE_SIZE;
    int origin_y = tile->tile_y * TILE_SIZE;
    for (int y = 0; y < TILE_SIZE; y++) {
        for (int x = 0; x < TILE_SIZE; x++) {
            tile->pixels[y * TILE_SIZE + x] =
                synthetic_page_pixel(origin_x + x, origin_y + y);
        }
    }
}

static ValidationTile *ensure_tile(ValidationCache *cache, int tile_x, int tile_y)
{
    for (int i = 0; i < cache->capacity; i++) {
        ValidationTile *tile = &cache->tiles[i];
        if (tile->valid && tile->tile_x == tile_x && tile->tile_y == tile_y) {
            cache->hits++;
            tile->last_used = ++cache->clock;
            return tile;
        }
    }
    cache->misses++;
    ValidationTile *victim = NULL;
    for (int i = 0; i < cache->capacity; i++) {
        ValidationTile *tile = &cache->tiles[i];
        if (!tile->valid) {
            victim = tile;
            break;
        }
        if (victim == NULL || tile->last_used < victim->last_used) victim = tile;
    }
    if (victim == NULL) return NULL;
    if (victim->valid) cache->evictions++;
    victim->valid = 1;
    victim->tile_x = tile_x;
    victim->tile_y = tile_y;
    victim->last_used = ++cache->clock;
    rasterize_tile(victim);
    return victim;
}

static int render_frame(ValidationCache *cache, int scroll_y,
                        volatile uint16_t *frame, uint64_t *compose_us)
{
    uint64_t compose_started = sceKernelGetSystemTimeWide();
    for (int tile_y = scroll_y / TILE_SIZE;
         tile_y <= (scroll_y + SCREEN_HEIGHT - 1) / TILE_SIZE; tile_y++) {
        for (int tile_x = 0; tile_x <= (SCREEN_WIDTH - 1) / TILE_SIZE; tile_x++) {
            ValidationTile *tile = ensure_tile(cache, tile_x, tile_y);
            if (tile == NULL) return 0;
            int world_left = tile_x * TILE_SIZE;
            int world_top = tile_y * TILE_SIZE;
            int x0 = world_left;
            int x1 = world_left + TILE_SIZE;
            int y0 = world_top;
            int y1 = world_top + TILE_SIZE;
            if (x0 < 0) x0 = 0;
            if (x1 > SCREEN_WIDTH) x1 = SCREEN_WIDTH;
            if (y0 < scroll_y) y0 = scroll_y;
            if (y1 > scroll_y + SCREEN_HEIGHT) y1 = scroll_y + SCREEN_HEIGHT;
            for (int world_y = y0; world_y < y1; world_y++) {
                int screen_y = world_y - scroll_y;
                int local_y = world_y - world_top;
                int local_x = x0 - world_left;
                for (int x = x0; x < x1; x++) {
                    frame[screen_y * FRAME_STRIDE + x] =
                        tile->pixels[local_y * TILE_SIZE + local_x++];
                }
            }
        }
    }
    sceKernelDcacheWritebackInvalidateAll();
    *compose_us += sceKernelGetSystemTimeWide() - compose_started;
    sceDisplayWaitVblankStart();
    return 1;
}

static uint32_t frame_checksum(const volatile uint16_t *frame)
{
    uint32_t hash = 2166136261u;
    for (int y = 0; y < SCREEN_HEIGHT; y++) {
        for (int x = 0; x < SCREEN_WIDTH; x++) {
            uint16_t value = frame[y * FRAME_STRIDE + x];
            hash = (hash ^ (value & 0xffu)) * 16777619u;
            hash = (hash ^ (value >> 8)) * 16777619u;
        }
    }
    return hash;
}

static size_t probe_largest_malloc(void)
{
    const size_t granularity = 64u * 1024u;
    size_t low = 0;
    size_t high = 896;
    while (low < high) {
        size_t middle = low + (high - low + 1) / 2;
        void *probe = malloc(middle * granularity);
        if (probe != NULL) {
            volatile uint8_t *bytes = (volatile uint8_t *) probe;
            size_t length = middle * granularity;
            for (size_t offset = 0; offset < length; offset += 4096) {
                bytes[offset] = (uint8_t) (offset >> 12);
            }
            bytes[length - 1] = 0x5a;
            free(probe);
            low = middle;
        } else {
            high = middle - 1;
        }
    }
    return low * granularity;
}

static void run_suite_once(ValidationCache *cache, int capacity,
                           volatile uint16_t *frame, SuiteResult *result)
{
    static const int scrolls[7] = {0, 136, 272, 408, 544, 272, 0};
    memset(cache->tiles, 0, sizeof(ValidationTile) * TILE_CAPACITY);
    cache->capacity = capacity;
    cache->clock = 0;
    cache->hits = 0;
    cache->misses = 0;
    cache->evictions = 0;
    memset(result, 0, sizeof(*result));
    result->capacity = capacity;
    result->passed = 1;

    uint32_t checksums[7] = {0};
    uint64_t started = sceKernelGetSystemTimeWide();
    for (int i = 0; i < 7; i++) {
        if (!render_frame(cache, scrolls[i], frame, &result->compose_us)) {
            result->passed = 0;
            break;
        }
        checksums[i] = frame_checksum(frame);
    }
    result->total_us = sceKernelGetSystemTimeWide() - started;
    if (checksums[0] != checksums[6] || checksums[2] != checksums[5]) {
        result->passed = 0;
    }
    result->hits = cache->hits;
    result->misses = cache->misses;
    result->evictions = cache->evictions;
    result->checksum_top = checksums[0];
    result->checksum_middle = checksums[2];
}

static uint64_t median3(uint64_t a, uint64_t b, uint64_t c)
{
    if (a > b) { uint64_t swap = a; a = b; b = swap; }
    if (b > c) { uint64_t swap = b; b = c; c = swap; }
    if (a > b) { uint64_t swap = a; a = b; b = swap; }
    return b;
}

static void run_suite(ValidationCache *cache, int capacity,
                      volatile uint16_t *frame, SuiteResult *result)
{
    SuiteResult samples[SUITE_SAMPLES];
    memset(samples, 0, sizeof(samples));
    for (unsigned i = 0; i < SUITE_SAMPLES; i++) {
        run_suite_once(cache, capacity, frame, &samples[i]);
    }
    *result = samples[0];
    result->samples = SUITE_SAMPLES;
    result->passed = samples[0].passed && samples[1].passed && samples[2].passed;
    for (unsigned i = 1; i < SUITE_SAMPLES; i++) {
        if (samples[i].hits != result->hits || samples[i].misses != result->misses
            || samples[i].evictions != result->evictions
            || samples[i].checksum_top != result->checksum_top
            || samples[i].checksum_middle != result->checksum_middle) {
            result->passed = 0;
        }
    }
    result->total_us = median3(samples[0].total_us, samples[1].total_us,
                               samples[2].total_us);
    result->compose_us = median3(samples[0].compose_us, samples[1].compose_us,
                                 samples[2].compose_us);
    result->total_min_us = samples[0].total_us;
    result->total_max_us = samples[0].total_us;
    result->compose_min_us = samples[0].compose_us;
    result->compose_max_us = samples[0].compose_us;
    for (unsigned i = 1; i < SUITE_SAMPLES; i++) {
        if (samples[i].total_us < result->total_min_us) result->total_min_us = samples[i].total_us;
        if (samples[i].total_us > result->total_max_us) result->total_max_us = samples[i].total_us;
        if (samples[i].compose_us < result->compose_min_us) result->compose_min_us = samples[i].compose_us;
        if (samples[i].compose_us > result->compose_max_us) result->compose_max_us = samples[i].compose_us;
    }
}

static void run_allocator_stress(AllocatorStressResult *result)
{
    void *blocks[STRESS_SLOTS];
    size_t sizes[STRESS_SLOTS];
    memset(result, 0, sizeof(*result));
    result->passed = 1;
    result->rounds = STRESS_ROUNDS;
    uint64_t started = sceKernelGetSystemTimeWide();
    for (unsigned round = 0; round < STRESS_ROUNDS; round++) {
        memset(blocks, 0, sizeof(blocks));
        for (unsigned i = 0; i < STRESS_SLOTS; i++) {
            sizes[i] = 17u + ((i * 977u + round * 313u) % 8176u);
            blocks[i] = malloc(sizes[i]);
            if (blocks[i] == NULL) {
                result->passed = 0;
                result->failures++;
                break;
            }
            uint8_t pattern = (uint8_t) (i * 37u + round * 19u);
            memset(blocks[i], pattern, sizes[i]);
            result->allocations++;
            result->bytes_exercised += sizes[i];
        }
        for (unsigned i = 0; i < STRESS_SLOTS && blocks[i] != NULL; i++) {
            uint8_t pattern = (uint8_t) (i * 37u + round * 19u);
            const uint8_t *bytes = blocks[i];
            if (bytes[0] != pattern || bytes[sizes[i] / 2] != pattern
                || bytes[sizes[i] - 1] != pattern) {
                result->passed = 0;
                result->failures++;
            }
        }
        for (unsigned parity = 1; parity < 3; parity++) {
            for (unsigned i = parity & 1u; i < STRESS_SLOTS; i += 2) {
                if (blocks[i] != NULL) { free(blocks[i]); blocks[i] = NULL; }
            }
        }
        if (!result->passed) break;
    }
    result->total_us = sceKernelGetSystemTimeWide() - started;
}

static void run_fragmentation_test(FragmentResult *result)
{
    static const size_t sizes[4] = {
        64u * 1024u, 128u * 1024u, 256u * 1024u, 512u * 1024u
    };
    void *blocks[FRAGMENT_SLOTS];
    memset(blocks, 0, sizeof(blocks));
    memset(result, 0, sizeof(*result));
    result->free_before = sceKernelTotalFreeMemSize();
    uint64_t started = sceKernelGetSystemTimeWide();
    for (unsigned i = 0; i < FRAGMENT_SLOTS; i++) {
        size_t length = sizes[i % 4u];
        blocks[i] = memalign(64, length);
        if (blocks[i] == NULL) break;
        volatile uint8_t *bytes = blocks[i];
        for (size_t offset = 0; offset < length; offset += 4096) {
            bytes[offset] = (uint8_t) (i ^ (unsigned) (offset >> 12));
        }
        bytes[length - 1] = (uint8_t) (0xa5u ^ i);
        result->blocks_allocated++;
        result->bytes_allocated += length;
    }
    for (unsigned i = 1; i < result->blocks_allocated; i += 2) {
        free(blocks[i]);
        blocks[i] = NULL;
    }
    result->free_fragmented = sceKernelTotalFreeMemSize();
    result->largest_fragmented = probe_largest_malloc();
    for (unsigned i = 0; i < result->blocks_allocated; i += 2) {
        free(blocks[i]);
        blocks[i] = NULL;
    }
    result->largest_recovered = probe_largest_malloc();
    result->free_after = sceKernelTotalFreeMemSize();
    result->total_us = sceKernelGetSystemTimeWide() - started;
    result->passed = result->blocks_allocated >= 16
                     && result->largest_recovered >= result->largest_fragmented;
}

static uint8_t storage_pattern(size_t offset)
{
    uint32_t value = (uint32_t) offset * 1664525u + 1013904223u;
    return (uint8_t) (value ^ (value >> 11) ^ (value >> 19));
}

static uint32_t checksum_bytes(uint32_t hash, const uint8_t *data, size_t length)
{
    for (size_t i = 0; i < length; i++) hash = (hash ^ data[i]) * 16777619u;
    return hash;
}

static void run_storage_test(StorageResult *result)
{
    uint8_t buffer[IO_CHUNK];
    memset(result, 0, sizeof(*result));
    result->bytes = IO_BYTES;
    result->write_checksum = 2166136261u;
    result->read_checksum = 2166136261u;
    FILE *file = fopen("validation_io.tmp", "wb");
    if (file == NULL) return;
    uint64_t started = sceKernelGetSystemTimeWide();
    for (size_t offset = 0; offset < IO_BYTES; offset += IO_CHUNK) {
        size_t length = IO_BYTES - offset;
        if (length > IO_CHUNK) length = IO_CHUNK;
        for (size_t i = 0; i < length; i++) buffer[i] = storage_pattern(offset + i);
        result->write_checksum = checksum_bytes(result->write_checksum, buffer, length);
        size_t written = fwrite(buffer, 1, length, file);
        result->bytes_written += written;
        if (written != length) break;
    }
    if (fflush(file) != 0) result->bytes_written = 0;
    if (fclose(file) != 0) result->bytes_written = 0;
    result->write_us = sceKernelGetSystemTimeWide() - started;

    file = fopen("validation_io.tmp", "rb");
    if (file != NULL) {
        started = sceKernelGetSystemTimeWide();
        size_t offset = 0;
        while (offset < IO_BYTES) {
            size_t length = IO_BYTES - offset;
            if (length > IO_CHUNK) length = IO_CHUNK;
            size_t received = fread(buffer, 1, length, file);
            result->read_checksum = checksum_bytes(result->read_checksum,
                                                    buffer, received);
            result->bytes_read += received;
            offset += received;
            if (received != length) break;
        }
        if (fclose(file) != 0) result->bytes_read = 0;
        result->read_us = sceKernelGetSystemTimeWide() - started;
    }
    result->remove_result = remove("validation_io.tmp");
    result->passed = result->bytes_written == IO_BYTES
                     && result->bytes_read == IO_BYTES
                     && result->write_checksum == result->read_checksum
                     && result->remove_result == 0;
}

static int write_result(uint32_t failure_mask, const SystemResult *system,
                        uint64_t diagnostic_us,
                        uint32_t free_before, uint32_t max_before,
                        uint32_t free_after, uint32_t max_after,
                        const struct mallinfo *heap_before,
                        const struct mallinfo *heap_with_cache,
                        const struct mallinfo *heap_after,
                        size_t largest_with_cache,
                        const AllocatorStressResult *stress,
                        const FragmentResult *fragment,
                        const StorageResult *storage,
                        const PrimitiveResult *primitive,
                        int ppm_written, const SuiteResult *suites,
                        size_t suite_count)
{
    FILE *file = fopen("validation.txt", "w");
    if (file == NULL) return 0;
    fprintf(file, "schema=%s\n", VALIDATION_SCHEMA);
    fprintf(file, "run_complete=yes\n");
    fprintf(file, "status=%s\n", failure_mask == 0 ? "PASS" : "FAIL");
    fprintf(file, "failure_mask=%08x\n", (unsigned) failure_mask);
    fprintf(file, "failure_bit_00000001=cache_allocation\n");
    fprintf(file, "failure_bit_00000002=contiguous_memory\n");
    fprintf(file, "failure_bit_00000004=allocator_stress\n");
    fprintf(file, "failure_bit_00000008=fragment_recovery\n");
    fprintf(file, "failure_bit_00000010=tile_checksum\n");
    fprintf(file, "failure_bit_00000020=tile_timing\n");
    fprintf(file, "failure_bit_00000040=rounded_fixed_primitives\n");
    fprintf(file, "failure_bit_00000080=storage_io\n");
    fprintf(file, "diagnostic_us=%llu\n", (unsigned long long) diagnostic_us);
    fprintf(file, "devkit_version=%08x\n", (unsigned) system->devkit_version);
    fprintf(file, "cpu_mhz=%d\nbus_mhz=%d\n", system->cpu_mhz, system->bus_mhz);
    fprintf(file, "power_online=%d\nbattery_present=%d\nbattery_charging=%d\n",
            system->power_online, system->battery_present,
            system->battery_charging);
    fprintf(file, "battery_percent=%d\nbattery_minutes=%d\n",
            system->battery_percent, system->battery_minutes);
    fprintf(file, "battery_temperature_c=%d\nbattery_voltage_mv=%d\n",
            system->battery_temperature, system->battery_voltage_mv);
    fprintf(file, "battery_temperature_available=%s\nbattery_voltage_available=%s\n",
            system->battery_temperature >= 0 ? "yes" : "no",
            system->battery_voltage_mv >= 0 ? "yes" : "no");
    fprintf(file, "free_before=%u\nmax_before=%u\n",
            (unsigned) free_before, (unsigned) max_before);
    fprintf(file, "free_after=%u\nmax_after=%u\n",
            (unsigned) free_after, (unsigned) max_after);
    fprintf(file, "heap_arena_before=%u\nheap_used_before=%u\nheap_free_before=%u\n",
            (unsigned) heap_before->arena, (unsigned) heap_before->uordblks,
            (unsigned) heap_before->fordblks);
    fprintf(file, "heap_used_with_cache=%u\nheap_free_with_cache=%u\n",
            (unsigned) heap_with_cache->uordblks,
            (unsigned) heap_with_cache->fordblks);
    fprintf(file, "largest_malloc_with_cache=%u\n", (unsigned) largest_with_cache);
    fprintf(file, "largest_probe_capped=%s\n",
            largest_with_cache == 896u * 64u * 1024u ? "yes" : "no");
    fprintf(file, "required_contiguous_bytes=%u\n",
            (unsigned) REQUIRED_CONTIGUOUS_BYTES);
    fprintf(file, "memory_headroom_status=%s\n",
            largest_with_cache >= REQUIRED_CONTIGUOUS_BYTES ? "PASS" : "FAIL");
    fprintf(file, "heap_used_after=%u\nheap_free_after=%u\n",
            (unsigned) heap_after->uordblks, (unsigned) heap_after->fordblks);
    fprintf(file, "allocator_stress_status=%s\n",
            stress->passed ? "PASS" : "FAIL");
    fprintf(file, "allocator_stress_rounds=%u\nallocator_stress_allocations=%u\n",
            stress->rounds, stress->allocations);
    fprintf(file, "allocator_stress_bytes=%u\nallocator_stress_failures=%u\n",
            (unsigned) stress->bytes_exercised, stress->failures);
    fprintf(file, "allocator_stress_total_us=%llu\n",
            (unsigned long long) stress->total_us);
    fprintf(file, "fragment_status=%s\n", fragment->passed ? "PASS" : "FAIL");
    fprintf(file, "fragment_blocks=%u\nfragment_bytes=%u\n",
            fragment->blocks_allocated, (unsigned) fragment->bytes_allocated);
    fprintf(file, "fragment_free_before=%u\nfragment_free_fragmented=%u\nfragment_free_after=%u\n",
            (unsigned) fragment->free_before, (unsigned) fragment->free_fragmented,
            (unsigned) fragment->free_after);
    fprintf(file, "fragment_largest_fragmented=%u\nfragment_largest_recovered=%u\n",
            (unsigned) fragment->largest_fragmented,
            (unsigned) fragment->largest_recovered);
    fprintf(file, "fragment_total_us=%llu\n",
            (unsigned long long) fragment->total_us);
    fprintf(file, "storage_status=%s\nstorage_bytes=%u\n",
            storage->passed ? "PASS" : "FAIL", (unsigned) storage->bytes);
    fprintf(file, "storage_bytes_written=%u\nstorage_bytes_read=%u\n",
            (unsigned) storage->bytes_written, (unsigned) storage->bytes_read);
    fprintf(file, "storage_write_checksum=%08x\nstorage_read_checksum=%08x\n",
            (unsigned) storage->write_checksum, (unsigned) storage->read_checksum);
    fprintf(file, "storage_write_us=%llu\nstorage_read_us=%llu\n",
            (unsigned long long) storage->write_us,
            (unsigned long long) storage->read_us);
    fprintf(file, "storage_remove_result=%d\n", storage->remove_result);
    fprintf(file, "primitive_status=%s\n",
            primitive->passed ? "PASS" : "FAIL");
    fprintf(file, "primitive_scroll_samples=%u\nprimitive_rounded_samples=%u\n",
            primitive->scroll_samples, primitive->rounded_samples);
    fprintf(file, "primitive_rounded_failures=%u\nprimitive_fixed_failures=%u\n",
            primitive->rounded_failures, primitive->fixed_failures);
    fprintf(file, "primitive_overlay_checksum_first=%08x\n",
            (unsigned) primitive->overlay_checksum_first);
    fprintf(file, "primitive_overlay_checksum_middle=%08x\n",
            (unsigned) primitive->overlay_checksum_middle);
    fprintf(file, "primitive_overlay_checksum_last=%08x\n",
            (unsigned) primitive->overlay_checksum_last);
    fprintf(file, "primitive_overlay_checksum_expected=%08x\n",
            (unsigned) EXPECTED_OVERLAY_CHECKSUM);
    fprintf(file, "primitive_base_sample_first=%04x\n",
            (unsigned) primitive->base_sample_first);
    fprintf(file, "primitive_base_sample_middle=%04x\n",
            (unsigned) primitive->base_sample_middle);
    fprintf(file, "primitive_base_sample_last=%04x\n",
            (unsigned) primitive->base_sample_last);
    fprintf(file, "primitive_base_us=%llu\nprimitive_overlay_us=%llu\n",
            (unsigned long long) primitive->base_us,
            (unsigned long long) primitive->overlay_us);
    fprintf(file, "operating_target_mhz=333\nclock_comparison=disabled\n");
    fprintf(file, "ppm_written=%s\n", ppm_written ? "yes" : "no");
    fprintf(file, "tile_bytes=%u\n", (unsigned) (sizeof(ValidationTile) * TILE_CAPACITY));
    for (size_t i = 0; i < suite_count; i++) {
        const SuiteResult *suite = &suites[i];
        fprintf(file, "suite_%d_status=%s\n", suite->capacity,
                suite->passed ? "PASS" : "FAIL");
        fprintf(file, "suite_%d_hits=%u\nsuite_%d_misses=%u\nsuite_%d_evictions=%u\n",
                suite->capacity, (unsigned) suite->hits,
                suite->capacity, (unsigned) suite->misses,
                suite->capacity, (unsigned) suite->evictions);
        fprintf(file, "suite_%d_total_us=%llu\nsuite_%d_compose_us=%llu\n",
                suite->capacity, (unsigned long long) suite->total_us,
                suite->capacity, (unsigned long long) suite->compose_us);
        fprintf(file, "suite_%d_samples=%u\n", suite->capacity, suite->samples);
        fprintf(file, "suite_%d_total_min_us=%llu\nsuite_%d_total_max_us=%llu\n",
                suite->capacity, (unsigned long long) suite->total_min_us,
                suite->capacity, (unsigned long long) suite->total_max_us);
        fprintf(file, "suite_%d_compose_min_us=%llu\nsuite_%d_compose_max_us=%llu\n",
                suite->capacity, (unsigned long long) suite->compose_min_us,
                suite->capacity, (unsigned long long) suite->compose_max_us);
        fprintf(file, "suite_%d_checksum_top=%08x\nsuite_%d_checksum_middle=%08x\n",
                suite->capacity, (unsigned) suite->checksum_top,
                suite->capacity, (unsigned) suite->checksum_middle);
    }
    fprintf(file, "criteria_max_compose_us=%u\ncriteria_max_sequence_us=%u\n",
            (unsigned) MAX_COMPOSE_US, (unsigned) MAX_SEQUENCE_US);
    fprintf(file, "run_end=1\n");
    return fclose(file) == 0;
}

static int write_frame_ppm(const volatile uint16_t *frame)
{
    FILE *file = fopen("validation.ppm", "wb");
    if (file == NULL) return 0;
    fprintf(file, "P6\n%d %d\n255\n", SCREEN_WIDTH, SCREEN_HEIGHT);
    for (int y = 0; y < SCREEN_HEIGHT; y++) {
        for (int x = 0; x < SCREEN_WIDTH; x++) {
            uint16_t value = frame[y * FRAME_STRIDE + x];
            fputc((int) (((value >> 11) & 31u) * 255u / 31u), file);
            fputc((int) (((value >> 5) & 63u) * 255u / 63u), file);
            fputc((int) ((value & 31u) * 255u / 31u), file);
        }
    }
    return fclose(file) == 0;
}

int main(int argc, char **argv)
{
    (void) argc;
    (void) argv;
    setup_callbacks();
    uint64_t diagnostic_started = sceKernelGetSystemTimeWide();
    scePowerSetClockFrequency(333, 333, 166);
    pspDebugScreenInitEx(NULL, PSP_DISPLAY_PIXEL_FORMAT_565, 1);
    pspDebugScreenSetXY(1, 1);
    pspDebugScreenPrintf("PSP Browser Validation v3\nRunning all diagnostics; please wait...\n");
    volatile uint16_t *frame = (volatile uint16_t *) 0x44000000;

    SystemResult system;
    memset(&system, 0, sizeof(system));
    system.devkit_version = (uint32_t) sceKernelDevkitVersion();
    system.cpu_mhz = scePowerGetCpuClockFrequencyInt();
    system.bus_mhz = scePowerGetBusClockFrequencyInt();
    system.power_online = scePowerIsPowerOnline();
    system.battery_present = scePowerIsBatteryExist();
    system.battery_charging = scePowerIsBatteryCharging();
    system.battery_percent = scePowerGetBatteryLifePercent();
    system.battery_minutes = scePowerGetBatteryLifeTime();
    system.battery_temperature = scePowerGetBatteryTemp();
    system.battery_voltage_mv = scePowerGetBatteryVolt();

    uint32_t free_before = sceKernelTotalFreeMemSize();
    uint32_t max_before = sceKernelMaxFreeMemSize();
    struct mallinfo heap_before = mallinfo();
    ValidationCache cache;
    memset(&cache, 0, sizeof(cache));
    cache.tiles = memalign(64, sizeof(ValidationTile) * TILE_CAPACITY);
    struct mallinfo heap_with_cache = mallinfo();
    size_t largest_with_cache = cache.tiles != NULL ? probe_largest_malloc() : 0;

    AllocatorStressResult stress;
    FragmentResult fragment;
    StorageResult storage;
    run_allocator_stress(&stress);
    run_fragmentation_test(&fragment);

    SuiteResult suites[4];
    memset(suites, 0, sizeof(suites));
    if (cache.tiles != NULL) {
        static const int capacities[4] = {4, 8, 12, 16};
        for (int i = 0; i < 4; i++) run_suite(&cache, capacities[i], frame, &suites[i]);
    }

    PrimitiveResult primitive;
    run_primitive_test(frame, &primitive);

    run_storage_test(&storage);

    uint32_t failure_mask = 0;
    if (cache.tiles == NULL) failure_mask |= FAILURE_CACHE_ALLOC;
    if (largest_with_cache < REQUIRED_CONTIGUOUS_BYTES) failure_mask |= FAILURE_MEMORY_HEADROOM;
    if (!stress.passed) failure_mask |= FAILURE_ALLOCATOR_STRESS;
    if (!fragment.passed) failure_mask |= FAILURE_FRAGMENT_RECOVERY;
    for (int i = 0; i < 4; i++) {
        if (!suites[i].passed || suites[i].checksum_top != 0x1a8d617du
            || suites[i].checksum_middle != 0x761ad0bdu) {
            failure_mask |= FAILURE_TILE_CHECKSUM;
        }
    }
    if (suites[3].compose_us > MAX_COMPOSE_US
        || suites[3].total_us > MAX_SEQUENCE_US) failure_mask |= FAILURE_TILE_TIMING;
    if (!primitive.passed) failure_mask |= FAILURE_PRIMITIVES;
    if (!storage.passed) failure_mask |= FAILURE_STORAGE_IO;

    if (cache.tiles != NULL) free(cache.tiles);
    struct mallinfo heap_after = mallinfo();
    uint32_t free_after = sceKernelTotalFreeMemSize();
    uint32_t max_after = sceKernelMaxFreeMemSize();
    int ppm_written = write_frame_ppm(frame);
    uint64_t diagnostic_us = sceKernelGetSystemTimeWide() - diagnostic_started;
    int log_written = write_result(failure_mask, &system, diagnostic_us,
                                   free_before, max_before,
                                   free_after, max_after, &heap_before,
                                   &heap_with_cache, &heap_after,
                                   largest_with_cache, &stress, &fragment,
                                   &storage, &primitive, ppm_written, suites, 4);

    pspDebugScreenClear();
    pspDebugScreenSetXY(1, 1);
    pspDebugScreenSetTextColor(failure_mask == 0 ? 0x0000ff00 : 0x000000ff);
    pspDebugScreenPrintf("PSP BROWSER VALIDATION v3: %s\n",
                         failure_mask == 0 ? "PASS" : "FAIL");
    pspDebugScreenSetTextColor(0x00ffffff);
    pspDebugScreenPrintf("failure mask %08x  log %s\n",
                         (unsigned) failure_mask, log_written ? "written" : "FAILED");
    pspDebugScreenPrintf("clock %d/%d MHz  battery %d%%\n",
                         system.cpu_mhz, system.bus_mhz, system.battery_percent);
    pspDebugScreenPrintf("free before %u  max %u\n",
                         (unsigned) free_before, (unsigned) max_before);
    pspDebugScreenPrintf("free after  %u  max %u\n",
                         (unsigned) free_after, (unsigned) max_after);
    pspDebugScreenPrintf("tile bytes %u\n", (unsigned) (sizeof(ValidationTile) * TILE_CAPACITY));
    pspDebugScreenPrintf("heap free with cache %u\n",
                         (unsigned) heap_with_cache.fordblks);
    pspDebugScreenPrintf("largest malloc %u\n", (unsigned) largest_with_cache);
    pspDebugScreenPrintf("allocator %s  fragment %s  storage %s\n",
                         stress.passed ? "PASS" : "FAIL",
                         fragment.passed ? "PASS" : "FAIL",
                         storage.passed ? "PASS" : "FAIL");
    for (int i = 0; i < 4; i++) {
        pspDebugScreenPrintf("%2d tiles: %u/%u/%u med %llu/%llu us\n",
            suites[i].capacity, (unsigned) suites[i].hits,
            (unsigned) suites[i].misses, (unsigned) suites[i].evictions,
            (unsigned long long) suites[i].total_us,
            (unsigned long long) suites[i].compose_us);
    }
    pspDebugScreenPrintf("round-trip %08x / %08x\n",
                         (unsigned) suites[3].checksum_top,
                         (unsigned) suites[3].checksum_middle);
    pspDebugScreenPrintf("rounded/fixed %s  base/overlay %llu/%llu us\n",
                         primitive.passed ? "PASS" : "FAIL",
                         (unsigned long long) primitive.base_us,
                         (unsigned long long) primitive.overlay_us);
    pspDebugScreenPrintf("Return validation.txt from PSPBVAL.\n");

    sceKernelDelayThread(5000000);
    sceKernelExitGame();
    return failure_mask == 0 && log_written ? 0 : 1;
}
