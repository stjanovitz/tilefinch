#ifndef TILEFINCH_SCREENSHOT_PNG_H
#define TILEFINCH_SCREENSHOT_PNG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define SCREENSHOT_PNG_MAX_WIDTH 480
#define SCREENSHOT_PNG_MAX_HEIGHT 272
#define SCREENSHOT_PNG_PATH_CAPACITY 768
#define SCREENSHOT_PNG_ERROR_CAPACITY 80

typedef enum {
    SCREENSHOT_PNG_IDLE = 0,
    SCREENSHOT_PNG_PENDING,
    SCREENSHOT_PNG_COMPLETE,
    SCREENSHOT_PNG_FAILED
} ScreenshotPngStatus;

/*
 * A streaming, allocation-free PNG writer for an immutable RGB565 snapshot.
 * Each scanline is one uncompressed DEFLATE block. This costs about 1.3 KiB
 * more than a single stored-block stream at 480x272, while making each pump a
 * natural bounded checkpoint and avoiding a second RGB framebuffer.
 * Zero-initialize the writer before its first use. A pending writer must be
 * completed or cancelled before it is reused.
 */
typedef struct {
    FILE *file;
    const uint16_t *pixels;
    int width;
    int height;
    int stride;
    size_t row;
    uint32_t idat_crc;
    uint32_t adler_s1;
    uint32_t adler_s2;
    ScreenshotPngStatus status;
    char final_path[SCREENSHOT_PNG_PATH_CAPACITY];
    char temporary_path[SCREENSHOT_PNG_PATH_CAPACITY + 5];
    char error[SCREENSHOT_PNG_ERROR_CAPACITY];
} ScreenshotPngWriter;

bool screenshot_png_begin(
    ScreenshotPngWriter *writer, const char *final_path,
    const uint16_t *pixels, int width, int height, int stride);
ScreenshotPngStatus screenshot_png_pump(
    ScreenshotPngWriter *writer, size_t maximum_rows);
void screenshot_png_cancel(ScreenshotPngWriter *writer);
unsigned screenshot_png_progress_per_mille(const ScreenshotPngWriter *writer);
const char *screenshot_png_error(const ScreenshotPngWriter *writer);

#endif
