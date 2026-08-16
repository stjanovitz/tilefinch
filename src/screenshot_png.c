#include "tilefinch/screenshot_png.h"
#include "tilefinch/pixel_math.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t crc32_nibbles[16] = {
    UINT32_C(0x00000000), UINT32_C(0x1db71064),
    UINT32_C(0x3b6e20c8), UINT32_C(0x26d930ac),
    UINT32_C(0x76dc4190), UINT32_C(0x6b6b51f4),
    UINT32_C(0x4db26158), UINT32_C(0x5005713c),
    UINT32_C(0xedb88320), UINT32_C(0xf00f9344),
    UINT32_C(0xd6d6a3e8), UINT32_C(0xcb61b38c),
    UINT32_C(0x9b64c2b0), UINT32_C(0x86d3d2d4),
    UINT32_C(0xa00ae278), UINT32_C(0xbdbdf21c)
};

static uint32_t screenshot_crc32_update(
    uint32_t crc, const unsigned char *bytes, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        crc ^= bytes[i];
        crc = (crc >> 4) ^ crc32_nibbles[crc & 15u];
        crc = (crc >> 4) ^ crc32_nibbles[crc & 15u];
    }
    return crc;
}

static bool screenshot_write(
    ScreenshotPngWriter *writer, const void *data, size_t length)
{
    return length == 0
        || (writer != NULL && writer->file != NULL
            && fwrite(data, 1, length, writer->file) == length);
}

static bool screenshot_write_u32(
    ScreenshotPngWriter *writer, uint32_t value)
{
    unsigned char bytes[4] = {
        (unsigned char) (value >> 24),
        (unsigned char) (value >> 16),
        (unsigned char) (value >> 8),
        (unsigned char) value
    };
    return screenshot_write(writer, bytes, sizeof(bytes));
}

static bool screenshot_write_chunk(
    ScreenshotPngWriter *writer, const char type[4],
    const unsigned char *data, size_t length)
{
    if (length > UINT32_MAX
        || !screenshot_write_u32(writer, (uint32_t) length)
        || !screenshot_write(writer, type, 4)
        || !screenshot_write(writer, data, length)) return false;
    uint32_t crc = screenshot_crc32_update(
        UINT32_MAX, (const unsigned char *) type, 4);
    crc = screenshot_crc32_update(crc, data, length) ^ UINT32_MAX;
    return screenshot_write_u32(writer, crc);
}

static void screenshot_fail(
    ScreenshotPngWriter *writer, const char *message)
{
    if (writer == NULL) return;
    if (writer->file != NULL) {
        (void) fclose(writer->file);
        writer->file = NULL;
    }
    if (writer->temporary_path[0] != '\0')
        (void) remove(writer->temporary_path);
    writer->pixels = NULL;
    writer->status = SCREENSHOT_PNG_FAILED;
    snprintf(writer->error, sizeof(writer->error), "%s",
             message == NULL ? "screenshot write failed" : message);
}

static bool screenshot_idat_write(
    ScreenshotPngWriter *writer, const unsigned char *data, size_t length)
{
    if (!screenshot_write(writer, data, length)) return false;
    writer->idat_crc = screenshot_crc32_update(
        writer->idat_crc, data, length);
    return true;
}

static void screenshot_adler_update(
    ScreenshotPngWriter *writer, const unsigned char *data, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        writer->adler_s1 += data[i];
        if (writer->adler_s1 >= 65521u) writer->adler_s1 -= 65521u;
        writer->adler_s2 += writer->adler_s1;
        if (writer->adler_s2 >= 65521u) writer->adler_s2 -= 65521u;
    }
}

static bool screenshot_raw_write(
    ScreenshotPngWriter *writer, const unsigned char *data, size_t length)
{
    if (!screenshot_idat_write(writer, data, length)) return false;
    screenshot_adler_update(writer, data, length);
    return true;
}

bool screenshot_png_begin(
    ScreenshotPngWriter *writer, const char *final_path,
    const uint16_t *pixels, int width, int height, int stride)
{
    if (writer == NULL) return false;
    screenshot_png_cancel(writer);
    memset(writer, 0, sizeof(*writer));
    size_t path_length = final_path == NULL
        ? 0 : strnlen(final_path, SCREENSHOT_PNG_PATH_CAPACITY);
    if (path_length == 0 || path_length >= SCREENSHOT_PNG_PATH_CAPACITY
        || pixels == NULL || width <= 0 || height <= 0
        || width > SCREENSHOT_PNG_MAX_WIDTH
        || height > SCREENSHOT_PNG_MAX_HEIGHT || stride < width) {
        screenshot_fail(writer, "invalid screenshot input");
        return false;
    }
    size_t row_bytes = 1u + (size_t) width * 3u;
    if (row_bytes > UINT16_MAX
        || (size_t) height > (UINT32_MAX - 6u) / (row_bytes + 5u)) {
        screenshot_fail(writer, "screenshot geometry exceeds PNG bounds");
        return false;
    }
    snprintf(writer->final_path, sizeof(writer->final_path), "%s",
             final_path);
    int temporary_length = snprintf(
        writer->temporary_path, sizeof(writer->temporary_path),
        "%s.tmp", final_path);
    if (temporary_length <= 0
        || (size_t) temporary_length >= sizeof(writer->temporary_path)) {
        screenshot_fail(writer, "screenshot path is too long");
        return false;
    }
    writer->file = fopen(writer->temporary_path, "wb");
    if (writer->file == NULL) {
        screenshot_fail(writer, "could not create screenshot file");
        return false;
    }
    static const unsigned char signature[8] = {
        137, 80, 78, 71, 13, 10, 26, 10
    };
    unsigned char ihdr[13] = {
        (unsigned char) ((unsigned) width >> 24),
        (unsigned char) ((unsigned) width >> 16),
        (unsigned char) ((unsigned) width >> 8),
        (unsigned char) width,
        (unsigned char) ((unsigned) height >> 24),
        (unsigned char) ((unsigned) height >> 16),
        (unsigned char) ((unsigned) height >> 8),
        (unsigned char) height,
        8, 2, 0, 0, 0
    };
    uint32_t idat_length = 2u
        + (uint32_t) height * ((uint32_t) row_bytes + 5u) + 4u;
    if (!screenshot_write(writer, signature, sizeof(signature))
        || !screenshot_write_chunk(writer, "IHDR", ihdr, sizeof(ihdr))
        || !screenshot_write_u32(writer, idat_length)
        || !screenshot_write(writer, "IDAT", 4)) {
        screenshot_fail(writer, "could not write screenshot header");
        return false;
    }
    writer->idat_crc = screenshot_crc32_update(
        UINT32_MAX, (const unsigned char *) "IDAT", 4);
    static const unsigned char zlib_header[2] = {0x78, 0x01};
    if (!screenshot_idat_write(writer, zlib_header, sizeof(zlib_header))) {
        screenshot_fail(writer, "could not start screenshot image data");
        return false;
    }
    writer->pixels = pixels;
    writer->width = width;
    writer->height = height;
    writer->stride = stride;
    writer->adler_s1 = 1;
    writer->status = SCREENSHOT_PNG_PENDING;
    return true;
}

static bool screenshot_write_row(ScreenshotPngWriter *writer)
{
    size_t raw_length = 1u + (size_t) writer->width * 3u;
    uint16_t length = (uint16_t) raw_length;
    unsigned char header[5] = {
        (unsigned char) (writer->row + 1u == (size_t) writer->height),
        (unsigned char) length,
        (unsigned char) (length >> 8),
        (unsigned char) ~length,
        (unsigned char) (~length >> 8)
    };
    if (!screenshot_idat_write(writer, header, sizeof(header))) return false;
    static const unsigned char filter = 0;
    if (!screenshot_raw_write(writer, &filter, 1)) return false;
    const uint16_t *source = writer->pixels
        + writer->row * (size_t) writer->stride;
    enum { CONVERSION_PIXELS = 128 };
    unsigned char rgb[CONVERSION_PIXELS * 3];
    for (size_t x = 0; x < (size_t) writer->width;) {
        size_t count = (size_t) writer->width - x;
        if (count > CONVERSION_PIXELS) count = CONVERSION_PIXELS;
        for (size_t i = 0; i < count; i++) {
            uint16_t pixel = source[x + i];
            unsigned red = tilefinch_rgb565_red_code(pixel);
            unsigned green = tilefinch_rgb565_green_code(pixel);
            unsigned blue = tilefinch_rgb565_blue_code(pixel);
            rgb[i * 3u] = (unsigned char) ((red << 3) | (red >> 2));
            rgb[i * 3u + 1u] =
                (unsigned char) ((green << 2) | (green >> 4));
            rgb[i * 3u + 2u] =
                (unsigned char) ((blue << 3) | (blue >> 2));
        }
        if (!screenshot_raw_write(writer, rgb, count * 3u)) return false;
        x += count;
    }
    writer->row++;
    return true;
}

static ScreenshotPngStatus screenshot_finish(ScreenshotPngWriter *writer)
{
    uint32_t adler = (writer->adler_s2 << 16) | writer->adler_s1;
    unsigned char adler_bytes[4] = {
        (unsigned char) (adler >> 24), (unsigned char) (adler >> 16),
        (unsigned char) (adler >> 8), (unsigned char) adler
    };
    if (!screenshot_idat_write(writer, adler_bytes, sizeof(adler_bytes))
        || !screenshot_write_u32(
               writer, writer->idat_crc ^ UINT32_MAX)
        || !screenshot_write_chunk(writer, "IEND", NULL, 0)) {
        screenshot_fail(writer, "could not finish screenshot image data");
        return writer->status;
    }
    if (fclose(writer->file) != 0) {
        writer->file = NULL;
        screenshot_fail(writer, "could not flush screenshot file");
        return writer->status;
    }
    writer->file = NULL;
    if (rename(writer->temporary_path, writer->final_path) != 0) {
        screenshot_fail(writer, "could not publish screenshot file");
        return writer->status;
    }
    writer->temporary_path[0] = '\0';
    writer->pixels = NULL;
    writer->status = SCREENSHOT_PNG_COMPLETE;
    return writer->status;
}

ScreenshotPngStatus screenshot_png_pump(
    ScreenshotPngWriter *writer, size_t maximum_rows)
{
    if (writer == NULL) return SCREENSHOT_PNG_FAILED;
    if (writer->status != SCREENSHOT_PNG_PENDING || maximum_rows == 0)
        return writer->status;
    size_t stop = writer->row + maximum_rows;
    if (stop < writer->row || stop > (size_t) writer->height)
        stop = (size_t) writer->height;
    while (writer->row < stop) {
        if (!screenshot_write_row(writer)) {
            screenshot_fail(writer, "could not write screenshot pixels");
            return writer->status;
        }
    }
    return writer->row == (size_t) writer->height
        ? screenshot_finish(writer) : writer->status;
}

void screenshot_png_cancel(ScreenshotPngWriter *writer)
{
    if (writer == NULL) return;
    if (writer->file != NULL) (void) fclose(writer->file);
    if (writer->temporary_path[0] != '\0')
        (void) remove(writer->temporary_path);
    memset(writer, 0, sizeof(*writer));
}

unsigned screenshot_png_progress_per_mille(const ScreenshotPngWriter *writer)
{
    if (writer == NULL || writer->height <= 0) return 0;
    if (writer->status == SCREENSHOT_PNG_COMPLETE) return 1000;
    size_t row = writer->row > (size_t) writer->height
        ? (size_t) writer->height : writer->row;
    return (unsigned) (row * 1000u / (size_t) writer->height);
}

const char *screenshot_png_error(const ScreenshotPngWriter *writer)
{
    return writer == NULL || writer->error[0] == '\0'
        ? "screenshot unavailable" : writer->error;
}
