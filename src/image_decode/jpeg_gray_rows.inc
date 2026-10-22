/* A sequential, single-component JPEG has no chroma lookahead. Keep one
   eight-row IDCT band instead of its full plane. Other JPEG forms retain
   stb's existing path, including its restart/multiple-scan tolerance. */
static bool image_jpeg_gray_rows_eligible(const unsigned char *data, size_t size)
{
    if (size < 4 || data[0] != 0xff || data[1] != 0xd8) return false;
    size_t at = 2;
    unsigned component = 0;
    bool frame = false;
    while (at < size) {
        if (data[at++] != 0xff) return false;
        while (at < size && data[at] == 0xff) at++;
        if (size - at < 3) return false;
        unsigned marker = data[at++];
        size_t bytes = ((size_t) data[at] << 8) | data[at + 1];
        if (bytes < 2 || bytes > size - at) return false;
        const unsigned char *p = data + at + 2;
        if (marker == 0xc0 || marker == 0xc1) {
            if (frame || bytes != 11 || p[0] != 8 || p[5] != 1) return false;
            component = p[6];
            frame = true;
        } else if (marker == 0xda) {
            if (!frame || bytes != 8 || p[0] != 1 || p[1] != component
                || p[3] != 0 || p[4] != 63 || p[5] != 0) return false;
            at += bytes;
            while (at < size) {
                const unsigned char *next = memchr(data + at, 0xff, size - at);
                if (next == NULL) return true; /* stb tolerates missing EOI. */
                at = (size_t) (next - data) + 1;
                while (at < size && data[at] == 0xff) at++;
                if (at == size) return true;
                unsigned tail = data[at++];
                if (tail == 0) continue;
                /* Only the ordinary end marker is admitted here. */
                return tail == 0xd9;
            }
            return true;
        } else if (marker == 0xc2 || marker == 0xdd || marker == 0xd9
                   || marker == 0xd8 || marker == 0x01
                   || (marker >= 0xd0 && marker <= 0xd7)) return false;
        at += bytes;
    }
    return false;
}

static unsigned char *image_jpeg_gray_rows(stbi__jpeg *jpeg,
    int width, int height, int target_width, int target_height,
    unsigned char *external_output, bool compact, bool *interrupted)
{
    if (!stbi__decode_jpeg_header(jpeg, STBI__SCAN_header)
        || jpeg->progressive || jpeg->s->img_n != 1
        || jpeg->s->img_x != (unsigned) width
        || jpeg->s->img_y != (unsigned) height) return NULL;
    bool scan = false;
    while (!stbi__at_eof(jpeg->s)) {
        int marker = stbi__get_marker(jpeg);
        if (stbi__SOS(marker)) {
            scan = stbi__process_scan_header(jpeg) && jpeg->scan_n == 1;
            break;
        }
        if (!stbi__process_marker(jpeg, marker)) return NULL;
    }
    if (!scan || jpeg->restart_interval != 0) return NULL;
    int stride = (width + 7) & ~7;
    jpeg->img_comp[0].w2 = stride;
    jpeg->img_comp[0].raw_data = stbi__malloc_mad2(stride, 8, 15);
    if (jpeg->img_comp[0].raw_data == NULL) return NULL;
    jpeg->img_comp[0].data = (stbi_uc *)
        (((size_t) jpeg->img_comp[0].raw_data + 15u) & ~(size_t) 15u);
    size_t channels = compact ? 1u : 4u;
    size_t bytes = (size_t) target_width * (size_t) target_height * channels;
    unsigned char *output = external_output != NULL ? external_output
        : image_malloc(bytes);
    ImageRowSampler sampler = {0};
    bool ok = output != NULL && image_row_sampler_init(&sampler,
        width, height, target_width, target_height, output);
    stbi__jpeg_reset(jpeg);
    STBI_SIMD_ALIGN(short, block[64]);
    for (int y = 0; ok && y < height; y += 8) {
        for (int x = 0; x < width; x += 8) {
            int ac = jpeg->img_comp[0].ha;
            if (!stbi__jpeg_decode_block(jpeg, block,
                    jpeg->huff_dc + jpeg->img_comp[0].hd,
                    jpeg->huff_ac + ac, jpeg->fast_ac[ac], 0,
                    jpeg->dequant[jpeg->img_comp[0].tq])) {
                ok = false;
                break;
            }
            stbi__jpeg_idct_store(jpeg, 0, x, 0, block);
        }
        for (int row = 0; ok && row < 8 && y + row < height; row++) {
            if (image_row_sampler_wants(&sampler)) {
                const unsigned char *source = jpeg->img_comp[0].data
                    + (size_t) row * (size_t) stride;
                unsigned char *out = output + (size_t) sampler.target_y
                    * (size_t) target_width * channels;
                for (int x = 0; x < target_width; x++) {
                    unsigned char value = source[sampler.columns[x]];
                    if (compact) out[x] = value;
                    else {
                        out[(size_t) x * 4u] = value;
                        out[(size_t) x * 4u + 1] = value;
                        out[(size_t) x * 4u + 2] = value;
                        out[(size_t) x * 4u + 3] = 255;
                    }
                }
                sampler.target_y++;
            }
            sampler.source_y++;
        }
        if (ok && decode_arena == NULL && y + 8 < height
            && !tilefinch_platform_cooperate("image-jpeg-decode", (size_t) y)) {
            *interrupted = true;
            ok = false;
        }
    }
    ok = ok && image_row_sampler_finished(&sampler);
    image_row_sampler_release(&sampler);
    if (!ok) {
        if (external_output == NULL) image_free(output);
        return NULL;
    }
    return output;
}
