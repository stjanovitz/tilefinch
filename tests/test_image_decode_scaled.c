/* Decoding at target size: a JPEG, PNG or WebP shown smaller than its
   source is decoded without a source-sized surface, and the Budget peak of
   the decode stays near the target plus a bounded working set. PNG output is
   identical to the previous decode-then-point-sample path; JPEG output,
   whose IDCT blocks are averaged, is closer than that path to an exact area
   average of the full decode. All images are generated here; none is a
   committed photo. */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "tilefinch/budget.h"
#include "tilefinch/resources.h"

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include <stb_image.h>

#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,          \
                    __LINE__, #condition);                                    \
            return 1;                                                         \
        }                                                                     \
    } while (0)

#define MIB (1024u * 1024u)

typedef struct {
    unsigned char *data;
    size_t length;
    size_t capacity;
} Bytes;

static void bytes_append(Bytes *bytes, const void *data, size_t length)
{
    if (bytes->length + length > bytes->capacity) {
        size_t capacity = bytes->capacity ? bytes->capacity * 2 : 4096;
        while (capacity < bytes->length + length) capacity *= 2;
        bytes->data = realloc(bytes->data, capacity);
        if (bytes->data == NULL) abort();
        bytes->capacity = capacity;
    }
    memcpy(bytes->data + bytes->length, data, length);
    bytes->length += length;
}

static void write_callback(void *context, void *data, int size)
{
    bytes_append(context, data, (size_t) size);
}

/* A deterministic test picture: smooth gradients, a fine checkerboard
   region, thin dark lines and a soft alpha ramp. */
static void synthetic_pixel(int x, int y, int width, int height,
                            unsigned char out[4])
{
    int r = x * 255 / (width - 1);
    int g = y * 255 / (height - 1);
    int b = ((x / 3 + y / 5) & 1) ? 230 : 40;
    if (x > width / 2) b = (x * 7 + y * 3) & 255;
    if ((x % 97) == 0 || (y % 61) == 0) r = g = b = 10;
    out[0] = (unsigned char) r;
    out[1] = (unsigned char) g;
    out[2] = (unsigned char) b;
    out[3] = (unsigned char) (x < width / 4 ? 255 : 255 - (y * 200 / height));
}

static unsigned char *synthetic_image(int width, int height, int channels)
{
    unsigned char *pixels = malloc((size_t) width * height * channels);
    if (pixels == NULL) abort();
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            unsigned char px[4];
            synthetic_pixel(x, y, width, height, px);
            memcpy(pixels + ((size_t) y * width + x) * channels, px,
                   (size_t) channels);
        }
    }
    return pixels;
}

/* An exact area average (fractional coverage, alpha-weighted colour) of an
   RGBA source, in double precision: the reference both decoders approach. */
static unsigned char *area_reference(const unsigned char *source, int width,
                                     int height, int target_width,
                                     int target_height)
{
    unsigned char *out = malloc((size_t) target_width * target_height * 4u);
    double sx = (double) width / target_width;
    double sy = (double) height / target_height;
    for (int ty = 0; ty < target_height; ty++) {
        double y0 = ty * sy, y1 = y0 + sy;
        for (int tx = 0; tx < target_width; tx++) {
            double x0 = tx * sx, x1 = x0 + sx;
            double sum[4] = {0, 0, 0, 0}, area = 0;
            for (int y = (int) y0; y < height && y < y1; y++) {
                double wy = fmin(y + 1, y1) - fmax(y, y0);
                for (int x = (int) x0; x < width && x < x1; x++) {
                    double w = (fmin(x + 1, x1) - fmax(x, x0)) * wy;
                    const unsigned char *p =
                        source + ((size_t) y * width + x) * 4u;
                    double a = p[3] / 255.0;
                    sum[0] += p[0] * a * w;
                    sum[1] += p[1] * a * w;
                    sum[2] += p[2] * a * w;
                    sum[3] += a * w;
                    area += w;
                }
            }
            unsigned char *o = out + ((size_t) ty * target_width + tx) * 4u;
            double alpha = sum[3] / area;
            for (int c = 0; c < 3; c++) {
                double v = sum[3] > 0 ? sum[c] / sum[3] : 0;
                o[c] = (unsigned char) fmin(255, floor(v + 0.5));
            }
            o[3] = (unsigned char) fmin(255, floor(alpha * 255 + 0.5));
        }
    }
    return out;
}

/* PSNR over the colour of visible pixels (colour under alpha 0 is
   meaningless) and over alpha. */
static double psnr(const unsigned char *a, const unsigned char *b,
                   size_t pixels)
{
    double error = 0;
    size_t samples = 0;
    for (size_t i = 0; i < pixels; i++) {
        bool visible = a[i * 4 + 3] > 8 && b[i * 4 + 3] > 8;
        for (int c = 0; c < 4; c++) {
            if (c < 3 && !visible) continue;
            double d = (double) a[i * 4 + c] - b[i * 4 + c];
            error += d * d;
            samples++;
        }
    }
    if (error == 0) return 99.0;
    return 10.0 * log10(255.0 * 255.0 / (error / (double) samples));
}

typedef struct {
    size_t peak_delta;
    size_t output_bytes;
    ImageDecodeStatus status;
    unsigned char *pixels;
} DecodeRun;

static DecodeRun decode_at(Budget *budget, const Bytes *encoded,
                           int source_width, int source_height,
                           int target_width, int target_height)
{
    ImageResource image = {
        .encoded = encoded->data,
        .encoded_length = encoded->length,
        .source_width = source_width,
        .source_height = source_height,
        .width = target_width,
        .height = target_height
    };
    DecodeRun run = {0};
    size_t base = budget->current;
    budget->peak = base;
    run.status = image_resource_decode_checked(&image, budget, &run.pixels);
    run.peak_delta = budget->peak - base;
    run.output_bytes = run.pixels == NULL ? 0 : budget_usable_size(run.pixels);
    return run;
}

/* ---- PNG writer ------------------------------------------------------- */

static unsigned char png_paeth(int a, int b, int c)
{
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return (unsigned char) (pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

static void png_chunk(Bytes *out, const char *type, const unsigned char *data,
                      size_t length)
{
    unsigned char header[8] = {
        (unsigned char) (length >> 24), (unsigned char) (length >> 16),
        (unsigned char) (length >> 8), (unsigned char) length,
        (unsigned char) type[0], (unsigned char) type[1],
        (unsigned char) type[2], (unsigned char) type[3]
    };
    bytes_append(out, header, 8);
    if (length) bytes_append(out, data, length);
    uLong crc = crc32(0, header + 4, 4);
    if (length) crc = crc32(crc, data, (uInt) length);
    unsigned char tail[4] = {
        (unsigned char) (crc >> 24), (unsigned char) (crc >> 16),
        (unsigned char) (crc >> 8), (unsigned char) crc
    };
    bytes_append(out, tail, 4);
}

/* Encodes packed scanlines (row_bytes each) with the filter type cycling
   through all five so every unfilter path runs. */
static Bytes png_encode(int width, int height, int depth, int color,
                        int channels, const unsigned char *rows,
                        size_t row_bytes, const unsigned char *palette,
                        int palette_length, const unsigned char *trns,
                        size_t trns_length)
{
    Bytes out = {0};
    static const unsigned char signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    bytes_append(&out, signature, 8);
    unsigned char ihdr[13] = {
        (unsigned char) (width >> 24), (unsigned char) (width >> 16),
        (unsigned char) (width >> 8), (unsigned char) width,
        (unsigned char) (height >> 24), (unsigned char) (height >> 16),
        (unsigned char) (height >> 8), (unsigned char) height,
        (unsigned char) depth, (unsigned char) color, 0, 0, 0
    };
    png_chunk(&out, "IHDR", ihdr, 13);
    if (palette_length) png_chunk(&out, "PLTE", palette, (size_t) palette_length * 3u);
    if (trns_length) png_chunk(&out, "tRNS", trns, trns_length);
    size_t bpp = (size_t) channels * depth / 8u;
    if (bpp == 0) bpp = 1;
    size_t filtered_length = (row_bytes + 1u) * (size_t) height;
    unsigned char *filtered = malloc(filtered_length);
    for (int y = 0; y < height; y++) {
        const unsigned char *raw = rows + (size_t) y * row_bytes;
        const unsigned char *prior = y ? raw - row_bytes : NULL;
        unsigned char *f = filtered + (size_t) y * (row_bytes + 1u);
        int type = y % 5;
        f[0] = (unsigned char) type;
        for (size_t i = 0; i < row_bytes; i++) {
            int left = i >= bpp ? raw[i - bpp] : 0;
            int up = prior ? prior[i] : 0;
            int ul = prior && i >= bpp ? prior[i - bpp] : 0;
            int predictor = type == 1 ? left : type == 2 ? up
                : type == 3 ? (left + up) / 2
                : type == 4 ? png_paeth(left, up, ul) : 0;
            f[i + 1] = (unsigned char) (raw[i] - predictor);
        }
    }
    uLongf compressed_length = compressBound(filtered_length);
    unsigned char *compressed = malloc(compressed_length);
    if (compress2(compressed, &compressed_length, filtered,
                  filtered_length, 6) != Z_OK) abort();
    /* Split the stream over several IDAT chunks. */
    size_t at = 0;
    while (at < compressed_length) {
        size_t piece = compressed_length - at > 7000 ? 7000
            : compressed_length - at;
        png_chunk(&out, "IDAT", compressed + at, piece);
        at += piece;
    }
    png_chunk(&out, "IEND", NULL, 0);
    free(compressed);
    free(filtered);
    return out;
}

/* Packs synthetic samples for any PNG colour type and depth. */
static Bytes png_synthetic(int width, int height, int depth, int color,
                           bool with_trns)
{
    int channels = color == 0 || color == 3 ? 1 : color == 2 ? 3
        : color == 4 ? 2 : 4;
    size_t row_bytes = ((size_t) width * channels * depth + 7u) / 8u;
    unsigned char *rows = calloc((size_t) height, row_bytes);
    unsigned max = (1u << depth) - 1u;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            unsigned char px[4];
            synthetic_pixel(x, y, width, height, px);
            for (int c = 0; c < channels; c++) {
                unsigned value;
                if (color == 3) value = (unsigned) (x * 7 + y * 3) % 200u % (max + 1u);
                else if (color == 0 || color == 4) {
                    unsigned gray = (px[0] * 3u + px[1] * 5u + px[2]) / 9u;
                    value = c == 0 ? gray : px[3];
                    value = depth == 16 ? value * 257u + (unsigned) (x & 255)
                        : value * max / 255u;
                } else {
                    value = px[c];
                    value = depth == 16 ? value * 257u + (unsigned) (y & 255)
                        : value;
                }
                size_t bit = ((size_t) x * channels + c) * depth;
                unsigned char *row = rows + (size_t) y * row_bytes;
                if (depth == 16) {
                    row[bit / 8] = (unsigned char) (value >> 8);
                    row[bit / 8 + 1] = (unsigned char) value;
                } else if (depth == 8) {
                    row[bit / 8] = (unsigned char) value;
                } else {
                    row[bit / 8] |= (unsigned char) (value << (8 - depth - bit % 8));
                }
            }
        }
    }
    unsigned char palette[256 * 3], trns[256];
    size_t trns_length = 0;
    int palette_length = 0;
    if (color == 3) {
        palette_length = (int) (max + 1u < 200u ? max + 1u : 200u);
        for (int i = 0; i < palette_length; i++) {
            palette[i * 3] = (unsigned char) (i * 37);
            palette[i * 3 + 1] = (unsigned char) (255 - i);
            palette[i * 3 + 2] = (unsigned char) (i * 11);
            trns[i] = (unsigned char) (i * 53);
        }
        trns_length = with_trns ? (size_t) palette_length / 2u : 0;
    } else if (with_trns && (color == 0 || color == 2)) {
        /* Key the value of pixel (0, 0) so some pixels are transparent. */
        const unsigned char *first = rows;
        for (int c = 0; c < channels; c++) {
            unsigned value;
            if (depth == 16) value = (unsigned) ((first[c * 2] << 8) | first[c * 2 + 1]);
            else if (depth == 8) value = first[c];
            else value = (unsigned) (first[0] >> (8 - depth)) & max;
            trns[c * 2] = (unsigned char) (value >> 8);
            trns[c * 2 + 1] = (unsigned char) value;
        }
        trns_length = (size_t) channels * 2u;
    }
    Bytes encoded = png_encode(width, height, depth, color, channels, rows,
                               row_bytes, palette, palette_length, trns,
                               trns_length);
    free(rows);
    return encoded;
}

/* ---- WebP (lossless, solid colour) writer ------------------------------ */

typedef struct {
    unsigned char bytes[64];
    size_t bit;
} BitWriter;

static void put_bits(BitWriter *writer, unsigned value, int count)
{
    for (int i = 0; i < count; i++, writer->bit++) {
        if ((value >> i) & 1u)
            writer->bytes[writer->bit / 8] |= (unsigned char) (1u << (writer->bit % 8));
    }
}

/* A VP8L stream whose five prefix codes each hold one symbol: every pixel is
   (r, g, b) and costs zero bits, whatever the size. */
static Bytes webp_solid(int width, int height, unsigned r, unsigned g,
                        unsigned b)
{
    BitWriter writer = {{0}, 0};
    put_bits(&writer, 0x2f, 8);
    put_bits(&writer, (unsigned) width - 1u, 14);
    put_bits(&writer, (unsigned) height - 1u, 14);
    put_bits(&writer, 0, 1);  /* alpha unused */
    put_bits(&writer, 0, 3);  /* version */
    put_bits(&writer, 0, 1);  /* no transform */
    put_bits(&writer, 0, 1);  /* no colour cache */
    put_bits(&writer, 0, 1);  /* no meta prefix codes */
    unsigned symbols[5] = {g, r, b, 255, 0};
    for (int i = 0; i < 5; i++) {
        put_bits(&writer, 1, 1);  /* simple code */
        put_bits(&writer, 0, 1);  /* one symbol */
        if (i == 4) {
            put_bits(&writer, 0, 1);
            put_bits(&writer, 0, 1);
        } else {
            put_bits(&writer, 1, 1);
            put_bits(&writer, symbols[i], 8);
        }
    }
    size_t payload = (writer.bit + 7u) / 8u;
    size_t padded = payload + (payload & 1u);
    Bytes out = {0};
    unsigned riff = (unsigned) (4u + 8u + padded);
    unsigned char header[20] = {
        'R', 'I', 'F', 'F', (unsigned char) riff, (unsigned char) (riff >> 8),
        (unsigned char) (riff >> 16), (unsigned char) (riff >> 24),
        'W', 'E', 'B', 'P', 'V', 'P', '8', 'L',
        (unsigned char) payload, (unsigned char) (payload >> 8),
        (unsigned char) (payload >> 16), (unsigned char) (payload >> 24)
    };
    bytes_append(&out, header, 20);
    bytes_append(&out, writer.bytes, padded);
    return out;
}

/* The previous reduction: one point sample per target pixel. */
static unsigned char *nearest_reference(const unsigned char *source,
                                        int width, int height,
                                        int target_width, int target_height)
{
    unsigned char *out = malloc((size_t) target_width * target_height * 4u);
    for (int y = 0; y < target_height; y++) {
        int sy = (int) ((int64_t) y * height / target_height);
        for (int x = 0; x < target_width; x++) {
            int sx = (int) ((int64_t) x * width / target_width);
            memcpy(out + ((size_t) y * target_width + x) * 4u,
                   source + ((size_t) sy * width + sx) * 4u, 4u);
        }
    }
    return out;
}

/* ---- tests ------------------------------------------------------------- */

/* Every non-interlaced PNG format decodes bit-exactly as stb does at full
   size, and reduced, exactly as stb's decode point-sampled (the previous
   path). */
static int test_png_formats(Budget *budget)
{
    static const int formats[][3] = {
        /* depth, color, tRNS */
        {1, 0, 1}, {2, 0, 0}, {4, 0, 1}, {8, 0, 1}, {16, 0, 1},
        {8, 2, 0}, {8, 2, 1}, {16, 2, 1},
        {1, 3, 0}, {2, 3, 1}, {4, 3, 1}, {8, 3, 1},
        {8, 4, 0}, {16, 4, 0}, {8, 6, 0}, {16, 6, 0}
    };
    const int width = 157, height = 93;
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        Bytes png = png_synthetic(width, height, formats[i][0],
                                  formats[i][1], formats[i][2] != 0);
        int w = 0, h = 0, n = 0;
        unsigned char *expected = stbi_load_from_memory(
            png.data, (int) png.length, &w, &h, &n, 4);
        CHECK(expected != NULL && w == width && h == height);
        DecodeRun full = decode_at(budget, &png, width, height, width, height);
        if (full.status != IMAGE_DECODE_SUCCEEDED
            || memcmp(full.pixels, expected, (size_t) width * height * 4u) != 0) {
            fprintf(stderr, "png depth=%d color=%d trns=%d not stb-exact\n",
                    formats[i][0], formats[i][1], formats[i][2]);
            return 1;
        }
        DecodeRun reduced = decode_at(budget, &png, width, height, 61, 37);
        unsigned char *reference = nearest_reference(expected, width, height,
                                                     61, 37);
        if (reduced.status != IMAGE_DECODE_SUCCEEDED
            || reduced.output_bytes != 61u * 37u * 4u
            || memcmp(reduced.pixels, reference, 61u * 37u * 4u) != 0) {
            fprintf(stderr, "png depth=%d color=%d trns=%d reduced differs\n",
                    formats[i][0], formats[i][1], formats[i][2]);
            return 1;
        }
        image_resource_free_decoded(budget, full.pixels);
        image_resource_free_decoded(budget, reduced.pixels);
        free(reference);
        stbi_image_free(expected);
        free(png.data);
    }
    CHECK(budget->current == 0);
    return 0;
}

/* A 2400x1600 RGBA PNG shown at 480x320 never has a source-sized surface:
   the decode's Budget peak is the 600 KiB target plus zlib's window, a
   16 KiB scanline batch, one RGBA row and the column table, and the pixels
   are the previous path's exactly. That path held stb's inflated stream and
   the 15 MiB RGBA source. */
static int test_large_png_peak(Budget *budget)
{
    const int width = 2400, height = 1600, tw = 480, th = 320;
    unsigned char *source = synthetic_image(width, height, 4);
    Bytes png = png_encode(width, height, 8, 6, 4, source,
                           (size_t) width * 4u, NULL, 0, NULL, 0);
    DecodeRun run = decode_at(budget, &png, width, height, tw, th);
    size_t target_bytes = (size_t) tw * th * 4u;
    fprintf(stderr, "large png: peak=%zu target=%zu working=%zu\n",
            run.peak_delta, target_bytes, run.peak_delta - target_bytes);
    CHECK(run.status == IMAGE_DECODE_SUCCEEDED
          && run.output_bytes == target_bytes);
    CHECK(run.peak_delta <= target_bytes + 192u * 1024u);
    unsigned char *reference = nearest_reference(source, width, height,
                                                 tw, th);
    CHECK(memcmp(run.pixels, reference, target_bytes) == 0);
    image_resource_free_decoded(budget, run.pixels);
    free(reference);
    free(png.data);
    free(source);
    CHECK(budget->current == 0);
    return 0;
}

static Bytes jpeg_synthetic(int width, int height, int channels, int quality)
{
    unsigned char *source = synthetic_image(width, height, channels);
    Bytes jpeg = {0};
    if (stbi_write_jpg_to_func(write_callback, &jpeg, width, height,
                               channels, source, quality) == 0) abort();
    free(source);
    return jpeg;
}

/* A JPEG is decoded at 1/2, 1/4 or 1/8 in the IDCT and finished by the
   row sampler: correct size, and a Budget peak of the target plus reduced
   planes instead of full-resolution ones (4:2:0 at 2400x1600: 5.6 MiB of
   planes before). Sampling averaged blocks is closer to an exact area
   average of the full decode than the previous point sampling of that decode
   was; that, plus a floor, is the quality bar. These synthetic pictures are
   adversarial (one-pixel lines, saturated chroma checkerboards). */
static int test_jpeg_scaled(Budget *budget)
{
    static const struct {
        int width, height, channels, quality, tw, th;
        double floor_db;
    } cases[] = {
        {2400, 1600, 3, 85, 480, 320, 22.0},   /* 4:2:0, shift 2 */
        {2400, 1600, 3, 95, 300, 200, 50.0},   /* 4:4:4, shift 3 */
        {333, 251, 3, 85, 100, 76, 22.0},      /* odd sizes, shift 1 */
        {333, 251, 1, 90, 160, 120, 25.0},     /* grayscale, shift 1 */
        /* One dimension, shift 0: the previous path's samples exactly. */
        {640, 480, 3, 90, 640, 200, 23.0},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int width = cases[i].width, height = cases[i].height;
        int tw = cases[i].tw, th = cases[i].th;
        Bytes jpeg = jpeg_synthetic(width, height, cases[i].channels,
                                    cases[i].quality);
        CHECK(jpeg.length > 0);
        DecodeRun full = decode_at(budget, &jpeg, width, height, width, height);
        CHECK(full.status == IMAGE_DECODE_SUCCEEDED);
        size_t full_peak = full.peak_delta;
        unsigned char *reference = area_reference(full.pixels, width, height,
                                                  tw, th);
        unsigned char *previous = nearest_reference(full.pixels, width,
                                                    height, tw, th);
        double previous_quality = psnr(previous, reference,
                                       (size_t) tw * th);
        free(previous);
        image_resource_free_decoded(budget, full.pixels);
        DecodeRun run = decode_at(budget, &jpeg, width, height, tw, th);
        size_t target_bytes = (size_t) tw * th * 4u;
        double quality = run.pixels == NULL ? 0
            : psnr(run.pixels, reference, (size_t) tw * th);
        fprintf(stderr, "jpeg %dx%d c=%d q=%d -> %dx%d: peak=%zu "
                "target=%zu full-decode-peak=%zu psnr=%.2f "
                "(point sampling %.2f)\n",
                width, height, cases[i].channels, cases[i].quality, tw, th,
                run.peak_delta, target_bytes, full_peak, quality,
                previous_quality);
        CHECK(run.status == IMAGE_DECODE_SUCCEEDED
              && run.output_bytes == target_bytes);
        CHECK(quality >= cases[i].floor_db && quality >= previous_quality);
        /* Target, plus reduced planes (under 2x the target per dimension,
           at most three bytes per reduced pixel) and decoder state. */
        CHECK(run.peak_delta <= target_bytes + target_bytes * 3u
                                    + 160u * 1024u);
        if (width >= 2000) CHECK(run.peak_delta * 4u < full_peak);
        image_resource_free_decoded(budget, run.pixels);
        free(reference);
        free(jpeg.data);
    }
    CHECK(budget->current == 0);
    return 0;
}

/* libwebp's own scratch is charged to the decode's Budget as it allocates,
   rather than pre-reserved at eight bytes per source pixel. */
static int test_webp_accounting(Budget *budget)
{
    const int width = 1600, height = 1000, tw = 400, th = 250;
    Bytes webp = webp_solid(width, height, 200, 120, 40);
    DecodeRun run = decode_at(budget, &webp, width, height, tw, th);
    size_t target_bytes = (size_t) tw * th * 4u;
    size_t old_reservation = (size_t) width * height * 8u + webp.length;
    fprintf(stderr, "webp lossless %dx%d -> %dx%d: peak=%zu target=%zu "
            "previous reservation=%zu\n", width, height, tw, th,
            run.peak_delta, target_bytes, old_reservation);
    CHECK(run.status == IMAGE_DECODE_SUCCEEDED
          && run.output_bytes == target_bytes);
    CHECK(run.pixels[0] == 200 && run.pixels[1] == 120
          && run.pixels[2] == 40 && run.pixels[3] == 255
          && memcmp(run.pixels, run.pixels + target_bytes - 4u, 4u) == 0);
    /* libwebp's lossless decoder keeps a source-sized ARGB plane: it is
       visible in the peak, which still stays well under the reservation. */
    CHECK(run.peak_delta > target_bytes + (size_t) width * height * 4u);
    CHECK(run.peak_delta < old_reservation);
    image_resource_free_decoded(budget, run.pixels);
    free(webp.data);
    CHECK(budget->current == 0);
    return 0;
}

#include "suites/image_decode_webp_alpha.inc"

int main(int argc, char **argv)
{
    Budget budget;
    budget_init(&budget, 96u * MIB);
    const char *only = argc == 3 && strcmp(argv[1], "--only") == 0
        ? argv[2] : NULL;
    static const struct {
        const char *name;
        int (*run)(Budget *);
    } tests[] = {
        {"png-formats", test_png_formats},
        {"large-png", test_large_png_peak},
        {"jpeg", test_jpeg_scaled},
        {"webp", test_webp_accounting},
        {"webp-alpha", test_webp_alpha_byte_history},
        {"webp-alpha-filters", test_webp_alpha_filters},
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        if (only != NULL && strcmp(only, tests[i].name) != 0) continue;
        if (tests[i].run(&budget) != 0) return 1;
    }
    puts("image decode scaled tests passed");
    return 0;
}
