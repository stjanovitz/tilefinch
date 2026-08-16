#include "tilefinch/screenshot_png.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static uint32_t read_u32(const unsigned char *bytes)
{
    return (uint32_t) bytes[0] << 24
        | (uint32_t) bytes[1] << 16
        | (uint32_t) bytes[2] << 8
        | bytes[3];
}

static uint32_t crc32_slow(const unsigned char *bytes, size_t length)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < length; i++) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; bit++) {
            uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & mask);
        }
    }
    return crc ^ UINT32_MAX;
}

static uint32_t adler32_slow(const unsigned char *bytes, size_t length)
{
    uint32_t first = 1;
    uint32_t second = 0;
    for (size_t i = 0; i < length; i++) {
        first = (first + bytes[i]) % 65521u;
        second = (second + first) % 65521u;
    }
    return (second << 16) | first;
}

int main(void)
{
    static const char path[] = "/tmp/tilefinch-screenshot-test.png";
    static const char cancelled[] =
        "/tmp/tilefinch-screenshot-cancelled.png";
    (void) remove(path);
    (void) remove("/tmp/tilefinch-screenshot-test.png.tmp");
    (void) remove(cancelled);
    (void) remove("/tmp/tilefinch-screenshot-cancelled.png.tmp");

    const uint16_t pixels[6] = {
        0xf800u, 0x07e0u, 0x1234u,
        0x001fu, 0xffffu, 0x5678u
    };
    ScreenshotPngWriter writer = {0};
    CHECK(screenshot_png_begin(&writer, path, pixels, 2, 2, 3)
          && writer.status == SCREENSHOT_PNG_PENDING
          && screenshot_png_progress_per_mille(&writer) == 0);
    CHECK(screenshot_png_pump(&writer, 1) == SCREENSHOT_PNG_PENDING
          && screenshot_png_progress_per_mille(&writer) == 500);
    CHECK(screenshot_png_pump(&writer, 1) == SCREENSHOT_PNG_COMPLETE
          && screenshot_png_progress_per_mille(&writer) == 1000);

    FILE *file = fopen(path, "rb");
    CHECK(file != NULL && fseek(file, 0, SEEK_END) == 0);
    long file_size = ftell(file);
    CHECK(file_size > 0 && file_size < 256);
    CHECK(fseek(file, 0, SEEK_SET) == 0);
    unsigned char png[256];
    CHECK(fread(png, 1, (size_t) file_size, file) == (size_t) file_size
          && fclose(file) == 0);
    static const unsigned char signature[8] = {
        137, 80, 78, 71, 13, 10, 26, 10
    };
    CHECK(memcmp(png, signature, sizeof(signature)) == 0);

    const unsigned char *idat = NULL;
    size_t idat_length = 0;
    bool saw_ihdr = false;
    bool saw_iend = false;
    size_t at = sizeof(signature);
    while (at + 12u <= (size_t) file_size) {
        size_t length = read_u32(png + at);
        CHECK(length <= (size_t) file_size - at - 12u);
        const unsigned char *type = png + at + 4u;
        const unsigned char *data = type + 4u;
        uint32_t expected_crc = read_u32(data + length);
        CHECK(crc32_slow(type, length + 4u) == expected_crc);
        if (memcmp(type, "IHDR", 4) == 0) {
            CHECK(length == 13 && read_u32(data) == 2
                  && read_u32(data + 4) == 2
                  && data[8] == 8 && data[9] == 2);
            saw_ihdr = true;
        } else if (memcmp(type, "IDAT", 4) == 0) {
            idat = data;
            idat_length = length;
        } else if (memcmp(type, "IEND", 4) == 0) {
            CHECK(length == 0);
            saw_iend = true;
        }
        at += length + 12u;
    }
    CHECK(at == (size_t) file_size && saw_ihdr && saw_iend
          && idat != NULL && idat_length == 30);
    CHECK(idat[0] == 0x78 && idat[1] == 0x01);
    unsigned char raw[14];
    size_t input = 2;
    size_t output = 0;
    for (unsigned row = 0; row < 2; row++) {
        CHECK(input + 5u <= idat_length
              && idat[input] == (row == 1 ? 1 : 0));
        uint16_t length = (uint16_t) idat[input + 1u]
            | (uint16_t) idat[input + 2u] << 8;
        uint16_t inverse = (uint16_t) idat[input + 3u]
            | (uint16_t) idat[input + 4u] << 8;
        CHECK(length == 7 && (uint16_t) ~length == inverse);
        input += 5u;
        CHECK(input + length <= idat_length
              && output + length <= sizeof(raw));
        memcpy(raw + output, idat + input, length);
        input += length;
        output += length;
    }
    static const unsigned char expected_raw[14] = {
        0, 255, 0, 0, 0, 255, 0,
        0, 0, 0, 255, 255, 255, 255
    };
    CHECK(output == sizeof(expected_raw)
          && memcmp(raw, expected_raw, sizeof(raw)) == 0
          && input + 4u == idat_length
          && read_u32(idat + input)
               == adler32_slow(raw, sizeof(raw)));

    ScreenshotPngWriter cancelled_writer = {0};
    CHECK(screenshot_png_begin(
              &cancelled_writer, cancelled, pixels, 2, 2, 3)
          && screenshot_png_pump(&cancelled_writer, 1)
               == SCREENSHOT_PNG_PENDING);
    screenshot_png_cancel(&cancelled_writer);
    CHECK(cancelled_writer.status == SCREENSHOT_PNG_IDLE
          && fopen(cancelled, "rb") == NULL
          && fopen("/tmp/tilefinch-screenshot-cancelled.png.tmp", "rb")
               == NULL);

    ScreenshotPngWriter invalid = {0};
    CHECK(!screenshot_png_begin(&invalid, path, pixels, 481, 2, 481)
          && invalid.status == SCREENSHOT_PNG_FAILED
          && strstr(screenshot_png_error(&invalid), "invalid") != NULL);
    screenshot_png_cancel(&invalid);
    (void) remove(path);

    static uint16_t full_pixels[
        SCREENSHOT_PNG_MAX_WIDTH * SCREENSHOT_PNG_MAX_HEIGHT];
    for (size_t i = 0;
         i < sizeof(full_pixels) / sizeof(full_pixels[0]); i++) {
        full_pixels[i] = (uint16_t) (i * 73u);
    }
    static const char full_path[] =
        "/tmp/tilefinch-screenshot-full.png";
    (void) remove(full_path);
    (void) remove("/tmp/tilefinch-screenshot-full.png.tmp");
    ScreenshotPngWriter full = {0};
    CHECK(screenshot_png_begin(
        &full, full_path, full_pixels,
        SCREENSHOT_PNG_MAX_WIDTH, SCREENSHOT_PNG_MAX_HEIGHT,
        SCREENSHOT_PNG_MAX_WIDTH));
    unsigned pumps = 0;
    while (full.status == SCREENSHOT_PNG_PENDING && pumps < 100u) {
        (void) screenshot_png_pump(&full, 4);
        pumps++;
    }
    CHECK(full.status == SCREENSHOT_PNG_COMPLETE && pumps == 68u);
    file = fopen(full_path, "rb");
    CHECK(file != NULL && fseek(file, 0, SEEK_END) == 0
          && ftell(file) == 393375L
          && fclose(file) == 0);
    screenshot_png_cancel(&full);
    (void) remove(full_path);
    puts("screenshot-png-tests: ok");
    return 0;
}
