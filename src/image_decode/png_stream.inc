/* Row-streaming PNG decoder, included by image_decode.c.

   stb_image inflates the whole IDAT stream into one buffer and unfilters it
   into a second, source-sized one before Tilefinch can scale it. This reads
   the same chunks but inflates one scanline at a time (zlib, charged to the
   decode's allocator), unfilters it against the previous scanline, converts
   it to RGBA exactly as stb does (only when the row sampler takes it), and
   writes the target directly. The working set is a bounded batch of
   scanlines, one prior scanline, one RGBA row, zlib's window and the
   sampler's column table; the source-sized image never exists, and the
   output equals stb's decode reduced by the same point sampling.

   Interlaced (Adam7) images and Apple's CgBI variant return "fallback" and
   take stb's path. Everything stb rejects is rejected here too; pixels
   match stb's conversion exactly, including 16-bit to 8-bit truncation,
   low-bit-depth gray scaling and tRNS colour keys. */

#include <zlib.h>

typedef enum {
    IMAGE_PNG_STREAM_DECODED = 0,
    IMAGE_PNG_STREAM_FAILED,
    IMAGE_PNG_STREAM_FALLBACK
} ImagePngStreamResult;

static voidpf image_png_zalloc(voidpf opaque, uInt items, uInt size)
{
    (void) opaque;
    if (size != 0u && items > UINT_MAX / size) return Z_NULL;
    return image_malloc((size_t) items * (size_t) size);
}

static void image_png_zfree(voidpf opaque, voidpf address)
{
    (void) opaque;
    image_free(address);
}

static uint32_t image_png_be32(const unsigned char *at)
{
    return ((uint32_t) at[0] << 24) | ((uint32_t) at[1] << 16)
        | ((uint32_t) at[2] << 8) | (uint32_t) at[3];
}

static unsigned char image_png_paeth(int a, int b, int c)
{
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return (unsigned char) a;
    if (pb <= pc) return (unsigned char) b;
    return (unsigned char) c;
}

static bool image_png_unfilter(unsigned char *row, const unsigned char *prior,
                               size_t length, size_t bpp, unsigned filter)
{
    size_t head = bpp < length ? bpp : length;
    switch (filter) {
    case 0:
        return true;
    case 1:
        for (size_t i = bpp; i < length; i++) row[i] += row[i - bpp];
        return true;
    case 2:
        for (size_t i = 0; i < length; i++) row[i] += prior[i];
        return true;
    case 3:
        for (size_t i = 0; i < head; i++) row[i] += prior[i] >> 1;
        for (size_t i = bpp; i < length; i++) {
            row[i] += (unsigned char) ((row[i - bpp] + prior[i]) >> 1);
        }
        return true;
    case 4:
        for (size_t i = 0; i < head; i++) row[i] += prior[i];
        for (size_t i = bpp; i < length; i++) {
            row[i] += image_png_paeth(row[i - bpp], prior[i],
                                      prior[i - bpp]);
        }
        return true;
    default:
        return false;
    }
}

typedef struct {
    int width;
    int height;
    int depth;
    int color;
    int channels;
    unsigned char palette[256 * 4];
    int palette_length;
    bool has_key;
    uint16_t key[3];
    bool has_alpha;
} ImagePngFormat;

/* Mirrors stbi__create_png_image_raw, stbi__compute_transparency[16],
   stbi__expand_png_palette and stbi__convert_16_to_8 for req_comp == 4. */
static void image_png_row_to_rgba(const ImagePngFormat *format,
                                  const unsigned char *raw,
                                  unsigned char *out)
{
    static const unsigned char gray_scale[9] = {
        0, 0xff, 0x55, 0, 0x11, 0, 0, 0, 0x01
    };
    int width = format->width;
    if (format->depth < 8) {
        int depth = format->depth;
        int mask = (1 << depth) - 1;
        int scale = format->color == 0 ? gray_scale[depth] : 1;
        for (int x = 0; x < width; x++, out += 4) {
            int bit = x * depth;
            int value = (raw[bit >> 3] >> (8 - depth - (bit & 7))) & mask;
            if (format->color == 3) {
                memcpy(out, format->palette + value * 4, 4);
                continue;
            }
            unsigned char gray = (unsigned char) (value * scale);
            out[0] = out[1] = out[2] = gray;
            out[3] = format->has_key && gray == format->key[0] ? 0 : 255;
        }
        return;
    }
    if (format->depth == 8 && format->color == 6) {
        memcpy(out, raw, (size_t) width * 4u);
        return;
    }
    if (format->depth == 8 && format->color == 3) {
        for (int x = 0; x < width; x++, out += 4) {
            memcpy(out, format->palette + raw[x] * 4, 4);
        }
        return;
    }
    if (format->depth == 8 && format->color == 2 && !format->has_key) {
        for (int x = 0; x < width; x++, out += 4, raw += 3) {
            out[0] = raw[0];
            out[1] = raw[1];
            out[2] = raw[2];
            out[3] = 255;
        }
        return;
    }
    if (format->depth == 8) {
        for (int x = 0; x < width; x++, out += 4) {
            const unsigned char *in = raw + (size_t) x * format->channels;
            switch (format->color) {
            case 0:
                out[0] = out[1] = out[2] = in[0];
                out[3] = format->has_key && in[0] == format->key[0]
                    ? 0 : 255;
                break;
            case 2:
                out[0] = in[0];
                out[1] = in[1];
                out[2] = in[2];
                out[3] = format->has_key && in[0] == format->key[0]
                         && in[1] == format->key[1]
                         && in[2] == format->key[2] ? 0 : 255;
                break;
            case 3:
                memcpy(out, format->palette + in[0] * 4, 4);
                break;
            case 4:
                out[0] = out[1] = out[2] = in[0];
                out[3] = in[1];
                break;
            default:
                memcpy(out, in, 4);
                break;
            }
        }
        return;
    }
    for (int x = 0; x < width; x++, out += 4) {
        const unsigned char *in = raw + (size_t) x * format->channels * 2u;
        uint16_t sample[4];
        for (int c = 0; c < format->channels; c++) {
            sample[c] = (uint16_t) ((in[c * 2] << 8) | in[c * 2 + 1]);
        }
        switch (format->color) {
        case 0:
            out[0] = out[1] = out[2] = (unsigned char) (sample[0] >> 8);
            out[3] = format->has_key && sample[0] == format->key[0]
                ? 0 : 255;
            break;
        case 2:
            out[0] = (unsigned char) (sample[0] >> 8);
            out[1] = (unsigned char) (sample[1] >> 8);
            out[2] = (unsigned char) (sample[2] >> 8);
            out[3] = format->has_key && sample[0] == format->key[0]
                     && sample[1] == format->key[1]
                     && sample[2] == format->key[2] ? 0 : 255;
            break;
        case 4:
            out[0] = out[1] = out[2] = (unsigned char) (sample[0] >> 8);
            out[3] = (unsigned char) (sample[1] >> 8);
            break;
        default:
            for (int c = 0; c < 4; c++) {
                out[c] = (unsigned char) (sample[c] >> 8);
            }
            break;
        }
    }
}

static bool image_png_header(const unsigned char *data, uint32_t length,
                             ImagePngFormat *format)
{
    if (length != 13u) return false;
    uint32_t width = image_png_be32(data);
    uint32_t height = image_png_be32(data + 4);
    int depth = data[8], color = data[9];
    if (width == 0 || height == 0 || width > STBI_MAX_DIMENSIONS
        || height > STBI_MAX_DIMENSIONS || data[10] != 0 || data[11] != 0
        || data[12] > 1) return false;
    if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16)
        return false;
    switch (color) {
    case 0: format->channels = 1; break;
    case 2: format->channels = 3; break;
    case 3: format->channels = 1; break;
    case 4: format->channels = 2; break;
    case 6: format->channels = 4; break;
    default: return false;
    }
    if ((color == 3 && depth == 16) || (color != 0 && color != 3 && depth < 8))
        return false;
    format->width = (int) width;
    format->height = (int) height;
    format->depth = depth;
    format->color = color;
    format->has_alpha = color == 4 || color == 6;
    return true;
}

static ImagePngStreamResult image_decode_png_stream(
    const unsigned char *encoded, size_t encoded_length,
    int target_width, int target_height, int *source_width,
    int *source_height, unsigned char **pixels, bool *interrupted)
{
    static const unsigned char signature[8] = {
        137, 80, 78, 71, 13, 10, 26, 10
    };
    *pixels = NULL;
    *interrupted = false;
    if (encoded == NULL || encoded_length < 8u
        || memcmp(encoded, signature, 8) != 0) return IMAGE_PNG_STREAM_FAILED;
    ImagePngFormat *format = image_malloc(sizeof(*format));
    if (format == NULL) return IMAGE_PNG_STREAM_FAILED;
    memset(format, 0, sizeof(*format));
    for (int i = 0; i < 256; i++) format->palette[i * 4 + 3] = 255;

    ImagePngStreamResult result = IMAGE_PNG_STREAM_FAILED;
    bool header_seen = false, inflating = false;
    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    unsigned char *rows = NULL, *prior = NULL, *rgba = NULL, *output = NULL;
    size_t row_bytes = 0, filled = 0, bpp = 1, stride = 0, batch = 0;
    int y = 0;
    bool scaled = false;
    ImageRowSampler sampler;
    memset(&sampler, 0, sizeof(sampler));

    size_t at = 8u;
    while (at <= encoded_length && encoded_length - at >= 12u) {
        uint32_t length = image_png_be32(encoded + at);
        const unsigned char *type = encoded + at + 4u;
        if (length > encoded_length - at - 12u) break;
        const unsigned char *data = encoded + at + 8u;
        at += 12u + length;
        if (!header_seen) {
            if (memcmp(type, "CgBI", 4) == 0) {
                result = IMAGE_PNG_STREAM_FALLBACK;
                goto done;
            }
            if (memcmp(type, "IHDR", 4) != 0
                || !image_png_header(data, length, format)) goto done;
            header_seen = true;
            if (data[12] != 0) {
                result = IMAGE_PNG_STREAM_FALLBACK;
                goto done;
            }
            continue;
        }
        if (memcmp(type, "IHDR", 4) == 0) goto done;
        if (memcmp(type, "PLTE", 4) == 0) {
            if (inflating || length > 768u || length % 3u != 0u) goto done;
            format->palette_length = (int) (length / 3u);
            for (int i = 0; i < format->palette_length; i++) {
                format->palette[i * 4] = data[i * 3];
                format->palette[i * 4 + 1] = data[i * 3 + 1];
                format->palette[i * 4 + 2] = data[i * 3 + 2];
                format->palette[i * 4 + 3] = 255;
            }
            continue;
        }
        if (memcmp(type, "tRNS", 4) == 0) {
            if (inflating || format->has_alpha) goto done;
            if (format->color == 3) {
                if (format->palette_length == 0
                    || length > (uint32_t) format->palette_length) goto done;
                for (uint32_t i = 0; i < length; i++) {
                    format->palette[i * 4 + 3] = data[i];
                }
                format->has_alpha = true;
            } else {
                if (length != (uint32_t) format->channels * 2u) goto done;
                static const unsigned char gray_scale[9] = {
                    0, 0xff, 0x55, 0, 0x11, 0, 0, 0, 0x01
                };
                for (int c = 0; c < format->channels; c++) {
                    uint16_t value = (uint16_t) ((data[c * 2] << 8)
                                                 | data[c * 2 + 1]);
                    format->key[c] = format->depth == 16 ? value
                        : (uint16_t) ((value & 255u)
                                      * gray_scale[format->depth]);
                }
                format->has_key = true;
                format->has_alpha = true;
            }
            continue;
        }
        if (memcmp(type, "IDAT", 4) == 0) {
            if (format->color == 3 && format->palette_length == 0) goto done;
            if (!inflating) {
                size_t bits = (size_t) format->width
                    * (size_t) format->channels * (size_t) format->depth;
                row_bytes = (bits + 7u) / 8u;
                bpp = (size_t) format->channels * (size_t) format->depth / 8u;
                if (bpp == 0) bpp = 1;
                if (target_width <= 0 || target_height <= 0
                    || target_width > format->width
                    || target_height > format->height
                    || (size_t) target_width
                           > SIZE_MAX / 4u / (size_t) target_height
                    || row_bytes > SIZE_MAX / 2u - 2u
                    || (size_t) format->width > SIZE_MAX / 4u) goto done;
                scaled = target_width != format->width
                    || target_height != format->height;
                /* zlib inflates several scanlines (filter byte + row) per
                   call into one bounded batch, about 16 KiB, so its fast
                   path runs; the last row of each batch is kept as the next
                   batch's prior row. */
                stride = row_bytes + 1u;
                size_t batch_rows = 16384u / stride;
                if (batch_rows == 0) batch_rows = 1;
                if (batch_rows > (size_t) format->height)
                    batch_rows = (size_t) format->height;
                batch = batch_rows * stride;
                rows = image_malloc(batch);
                prior = image_malloc(row_bytes);
                output = image_malloc((size_t) target_width
                                      * (size_t) target_height * 4u);
                rgba = scaled ? image_malloc((size_t) format->width * 4u)
                              : NULL;
                if (rows == NULL || prior == NULL || output == NULL
                    || (scaled && rgba == NULL)) goto done;
                memset(prior, 0, row_bytes);
                if (scaled && !image_row_sampler_init(
                        &sampler, format->width, format->height,
                        target_width, target_height, output)) goto done;
                stream.zalloc = image_png_zalloc;
                stream.zfree = image_png_zfree;
                if (inflateInit(&stream) != Z_OK) goto done;
                inflating = true;
            }
            stream.next_in = (Bytef *) data;
            stream.avail_in = length;
            while (stream.avail_in != 0 && y < format->height) {
                stream.next_out = rows + filled;
                stream.avail_out = (uInt) (batch - filled);
                int status = inflate(&stream, Z_NO_FLUSH);
                filled = batch - stream.avail_out;
                if (status != Z_OK && status != Z_STREAM_END
                    && status != Z_BUF_ERROR) goto done;
                size_t used = 0;
                const unsigned char *above = prior;
                while (filled - used >= stride && y < format->height) {
                    unsigned char *current = rows + used + 1u;
                    if (!image_png_unfilter(current, above, row_bytes, bpp,
                                            rows[used])) goto done;
                    if (!scaled) {
                        image_png_row_to_rgba(
                            format, current,
                            output + (size_t) y * (size_t) format->width * 4u);
                    } else {
                        bool wanted = image_row_sampler_wants(&sampler);
                        if (wanted)
                            image_png_row_to_rgba(format, current, rgba);
                        if (!image_row_sampler_push(
                                &sampler, wanted ? rgba : NULL)) goto done;
                    }
                    above = current;
                    used += stride;
                    y++;
                }
                if (used != 0) {
                    memcpy(prior, above, row_bytes);
                    memmove(rows, rows + used, filled - used);
                    filled -= used;
                    if (y < format->height
                        && !tilefinch_platform_cooperate(
                               "image-png-decode", (size_t) y)) {
                        *interrupted = true;
                        goto done;
                    }
                }
                if (status == Z_STREAM_END) break;
            }
            if (y == format->height) break;
            continue;
        }
        if (memcmp(type, "IEND", 4) == 0) break;
        /* An unknown critical chunk (upper-case first letter) is an error,
           as in stb; ancillary chunks are skipped. */
        if ((type[0] & 0x20u) == 0) goto done;
    }
    if (header_seen && inflating && y == format->height
        && (!scaled || image_row_sampler_finished(&sampler))) {
        result = IMAGE_PNG_STREAM_DECODED;
        *pixels = output;
        output = NULL;
        *source_width = format->width;
        *source_height = format->height;
    }
done:
    if (inflating) inflateEnd(&stream);
    image_row_sampler_release(&sampler);
    image_free(rgba);
    image_free(prior);
    image_free(rows);
    image_free(output);
    image_free(format);
    return result;
}
