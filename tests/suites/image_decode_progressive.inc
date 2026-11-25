/* Progressive JPEGs at a reduced scale keep only each block's low-frequency
   coefficients (patches/stb-31c1ad37-jpeg-scaled-idct.patch). The writer
   below encodes one set of quantized coefficients as a baseline or a
   progressive stream (spectral selection, successive approximation, EOB
   runs, restart intervals, any sampling), so a reduced progressive decode
   can be compared exactly with a baseline decode of the same coefficients
   cut to that square. One flat Huffman table (every byte value has a code)
   serves every scan, so no symbol statistics are needed. */

typedef struct {
    int width, height, components, h[3], v[3], hmax, vmax, mcux, mcuy;
    int bw[3], bh[3];          /* blocks in each padded component plane */
    int cw[3], ch[3];          /* blocks a single-component scan covers */
    unsigned char quant[64];   /* natural order */
    short *coef[3];            /* bw * bh blocks of 64, natural order */
    int restart;               /* MCUs (blocks in one-component scans) */
} JpegCoefficients;

typedef struct {
    int count, component[3], ss, se, ah, al;
} JpegScanSpec;

typedef struct {
    Bytes *out;
    unsigned bits;
    int count;
    int predictor[3];
    unsigned eobrun;
    Bytes corrections;         /* refinement bits an EOB run carries */
} JpegWriter;

static void jw_bits(JpegWriter *w, unsigned value, int count)
{
    for (int i = count - 1; i >= 0; i--) {
        w->bits = (w->bits << 1) | ((value >> i) & 1u);
        if (++w->count == 8) {
            unsigned char byte = (unsigned char) w->bits;
            bytes_append(w->out, &byte, 1);
            if (byte == 0xff) {
                byte = 0;
                bytes_append(w->out, &byte, 1);
            }
            w->bits = 0;
            w->count = 0;
        }
    }
}

/* The flat table: symbols 0..254 are their own 8-bit code, 255 is 0x1fe. */
static void jw_symbol(JpegWriter *w, int symbol)
{
    if (symbol < 255) jw_bits(w, (unsigned) symbol, 8);
    else jw_bits(w, 0x1feu, 9);
}

static int jw_category(int value)
{
    int magnitude = value < 0 ? -value : value, bits = 0;
    while (magnitude != 0) {
        bits++;
        magnitude >>= 1;
    }
    return bits;
}

static void jw_value(JpegWriter *w, int value, int category)
{
    if (category == 0) return;
    unsigned bits = value < 0 ? (unsigned) (value - 1) : (unsigned) value;
    jw_bits(w, bits & ((1u << category) - 1u), category);
}

static void jw_flush_eobrun(JpegWriter *w)
{
    if (w->eobrun == 0) return;
    int bits = 0;
    while ((w->eobrun >> (bits + 1)) != 0) bits++;
    jw_symbol(w, bits << 4);
    jw_bits(w, w->eobrun - (1u << bits), bits);
    for (size_t i = 0; i < w->corrections.length; i++)
        jw_bits(w, w->corrections.data[i], 1);
    w->corrections.length = 0;
    w->eobrun = 0;
}

static void jw_segment(Bytes *out, int marker, const unsigned char *payload,
                       size_t length)
{
    unsigned char head[4] = {
        0xff, (unsigned char) marker, (unsigned char) ((length + 2u) >> 8),
        (unsigned char) (length + 2u)
    };
    bytes_append(out, head, 4);
    bytes_append(out, payload, length);
}

static short *jw_coef_block(const JpegCoefficients *j, int component,
                           int bx, int by)
{
    return j->coef[component] + 64 * ((size_t) by * j->bw[component] + bx);
}

static void jw_block_baseline(JpegWriter *w, int component,
                              const short *block)
{
    int diff = block[0] - w->predictor[component];
    w->predictor[component] = block[0];
    int category = jw_category(diff);
    jw_symbol(w, category);
    jw_value(w, diff, category);
    int run = 0;
    for (int k = 1; k < 64; k++) {
        int value = block[stbi__jpeg_dezigzag[k]];
        if (value == 0) {
            run++;
            continue;
        }
        for (; run > 15; run -= 16) jw_symbol(w, 0xf0);
        category = jw_category(value);
        jw_symbol(w, (run << 4) | category);
        jw_value(w, value, category);
        run = 0;
    }
    if (run > 0) jw_symbol(w, 0);
}

static void jw_block_dc(JpegWriter *w, const JpegScanSpec *scan,
                        int component, const short *block)
{
    int value = block[0] >> scan->al;
    if (scan->ah != 0) {
        jw_bits(w, (unsigned) value & 1u, 1);
        return;
    }
    int diff = value - w->predictor[component];
    w->predictor[component] = value;
    int category = jw_category(diff);
    jw_symbol(w, category);
    jw_value(w, diff, category);
}

static void jw_block_ac_first(JpegWriter *w, const JpegScanSpec *scan,
                              const short *block)
{
    int run = 0;
    for (int k = scan->ss; k <= scan->se; k++) {
        int value = block[stbi__jpeg_dezigzag[k]];
        int magnitude = (value < 0 ? -value : value) >> scan->al;
        if (magnitude == 0) {
            run++;
            continue;
        }
        jw_flush_eobrun(w);
        for (; run > 15; run -= 16) jw_symbol(w, 0xf0);
        int category = jw_category(magnitude);
        jw_symbol(w, (run << 4) | category);
        jw_value(w, value < 0 ? -magnitude : magnitude, category);
        run = 0;
    }
    if (run > 0 && ++w->eobrun == 0x7fffu) jw_flush_eobrun(w);
}

/* Mirrors the decoder: correction bits follow the symbol whose advance
   passes the coefficients they refine, and a ZRL consumes positions up to
   its sixteenth coefficient that is still zero. */
static void jw_block_ac_refine(JpegWriter *w, const JpegScanSpec *scan,
                               const short *block)
{
    int magnitude[64];
    for (int k = scan->ss; k <= scan->se; k++) {
        int value = block[stbi__jpeg_dezigzag[k]];
        magnitude[k] = (value < 0 ? -value : value) >> scan->al;
    }
    int start = scan->ss;
    for (int k = scan->ss; k <= scan->se; k++) {
        if (magnitude[k] != 1) continue;
        int zeros = 0;
        for (int p = start; p < k; p++) zeros += magnitude[p] == 0;
        jw_flush_eobrun(w);
        while (zeros > 15) {
            jw_symbol(w, 0xf0);
            int passed = 0, p = start;
            for (; passed < 16; p++) {
                if (magnitude[p] == 0) passed++;
                else jw_bits(w, (unsigned) magnitude[p] & 1u, 1);
            }
            start = p;
            zeros -= 16;
        }
        jw_symbol(w, (zeros << 4) | 1);
        jw_bits(w, block[stbi__jpeg_dezigzag[k]] > 0 ? 1u : 0u, 1);
        for (int p = start; p < k; p++)
            if (magnitude[p] != 0) jw_bits(w, (unsigned) magnitude[p] & 1u, 1);
        start = k + 1;
    }
    if (start > scan->se) return;
    for (int p = start; p <= scan->se; p++) {
        if (magnitude[p] == 0) continue;
        unsigned char bit = (unsigned char) (magnitude[p] & 1);
        bytes_append(&w->corrections, &bit, 1);
    }
    if (++w->eobrun == 0x7fffu) jw_flush_eobrun(w);
}

static void jw_block(JpegWriter *w, const JpegScanSpec *scan, bool progressive,
                     int component, const short *block)
{
    if (!progressive) jw_block_baseline(w, component, block);
    else if (scan->ss == 0) jw_block_dc(w, scan, component, block);
    else if (scan->ah == 0) jw_block_ac_first(w, scan, block);
    else jw_block_ac_refine(w, scan, block);
}

static void jw_interval_end(JpegWriter *w, int *marker, bool last)
{
    jw_flush_eobrun(w);
    while (w->count != 0) jw_bits(w, 1, 1);
    memset(w->predictor, 0, sizeof(w->predictor));
    if (last) return;
    unsigned char rst[2] = {0xff, (unsigned char) (0xd0 + (*marker & 7))};
    bytes_append(w->out, rst, 2);
    (*marker)++;
}

static void jw_scan(Bytes *out, const JpegCoefficients *j,
                    const JpegScanSpec *scan, bool progressive)
{
    unsigned char header[10];
    size_t at = 0;
    header[at++] = (unsigned char) scan->count;
    for (int i = 0; i < scan->count; i++) {
        header[at++] = (unsigned char) (scan->component[i] + 1);
        header[at++] = 0;
    }
    header[at++] = (unsigned char) scan->ss;
    header[at++] = (unsigned char) scan->se;
    header[at++] = (unsigned char) ((scan->ah << 4) | scan->al);
    jw_segment(out, 0xda, header, at);
    JpegWriter w = {.out = out};
    int marker = 0, done = 0;
    if (scan->count == 1) {
        int c = scan->component[0];
        int total = j->cw[c] * j->ch[c];
        for (int by = 0; by < j->ch[c]; by++) {
            for (int bx = 0; bx < j->cw[c]; bx++) {
                jw_block(&w, scan, progressive, c,
                         jw_coef_block(j, c, bx, by));
                done++;
                if (j->restart != 0 && done % j->restart == 0 && done < total)
                    jw_interval_end(&w, &marker, false);
            }
        }
    } else {
        int total = j->mcux * j->mcuy;
        for (int my = 0; my < j->mcuy; my++) {
            for (int mx = 0; mx < j->mcux; mx++) {
                for (int i = 0; i < scan->count; i++) {
                    int c = scan->component[i];
                    for (int y = 0; y < j->v[c]; y++)
                        for (int x = 0; x < j->h[c]; x++)
                            jw_block(&w, scan, progressive, c,
                                jw_coef_block(j, c, mx * j->h[c] + x,
                                              my * j->v[c] + y));
                }
                done++;
                if (j->restart != 0 && done % j->restart == 0 && done < total)
                    jw_interval_end(&w, &marker, false);
            }
        }
    }
    jw_interval_end(&w, &marker, true);
    free(w.corrections.data);
}

/* The standard luminance table at quality 75, for every component. */
static const unsigned char jpeg_test_quant[64] = {
     8,  6,  5,  8, 12, 20, 26, 31,  6,  6,  7, 10, 13, 29, 30, 28,
     7,  7,  8, 12, 20, 29, 35, 28,  7,  9, 11, 15, 26, 44, 40, 31,
     9, 11, 19, 28, 34, 55, 52, 39, 12, 18, 28, 32, 41, 52, 57, 46,
    25, 32, 39, 44, 52, 61, 60, 51, 36, 46, 48, 49, 56, 50, 52, 50
};

/* Quantized coefficients of synthetic_image() in YCbCr; component 0 is
   sampled hy x vy, the others 1 x 1. */
static JpegCoefficients jpeg_coefficients(int width, int height,
                                          int components, int hy, int vy,
                                          int restart)
{
    JpegCoefficients j = {
        .width = width, .height = height, .components = components,
        .restart = restart, .hmax = components == 1 ? 1 : hy,
        .vmax = components == 1 ? 1 : vy
    };
    memcpy(j.quant, jpeg_test_quant, sizeof(j.quant));
    j.mcux = (width + 8 * j.hmax - 1) / (8 * j.hmax);
    j.mcuy = (height + 8 * j.vmax - 1) / (8 * j.vmax);
    unsigned char *rgb = synthetic_image(width, height, 3);
    double basis[8][8];
    for (int u = 0; u < 8; u++)
        for (int x = 0; x < 8; x++)
            basis[u][x] = (u == 0 ? sqrt(0.125) : 0.5)
                * cos((2 * x + 1) * u * M_PI / 16.0);
    for (int c = 0; c < components; c++) {
        j.h[c] = c == 0 ? j.hmax : 1;
        j.v[c] = c == 0 ? j.vmax : 1;
        j.bw[c] = j.mcux * j.h[c];
        j.bh[c] = j.mcuy * j.v[c];
        int samples_x = (width * j.h[c] + j.hmax - 1) / j.hmax;
        int samples_y = (height * j.v[c] + j.vmax - 1) / j.vmax;
        j.cw[c] = (samples_x + 7) / 8;
        j.ch[c] = (samples_y + 7) / 8;
        j.coef[c] = calloc((size_t) j.bw[c] * j.bh[c] * 64u, sizeof(short));
        if (j.coef[c] == NULL) abort();
        int sx = j.hmax / j.h[c], sy = j.vmax / j.v[c];
        for (int by = 0; by < j.bh[c]; by++) {
            for (int bx = 0; bx < j.bw[c]; bx++) {
                double block[8][8], rows[8][8];
                for (int y = 0; y < 8; y++) {
                    for (int x = 0; x < 8; x++) {
                        double sum = 0;
                        for (int dy = 0; dy < sy; dy++) {
                            for (int dx = 0; dx < sx; dx++) {
                                int px = ((bx * 8 + x) * sx + dx);
                                int py = ((by * 8 + y) * sy + dy);
                                if (px >= width) px = width - 1;
                                if (py >= height) py = height - 1;
                                const unsigned char *p =
                                    rgb + ((size_t) py * width + px) * 3u;
                                sum += c == 0
                                    ? 0.299 * p[0] + 0.587 * p[1]
                                        + 0.114 * p[2]
                                    : c == 1
                                    ? -0.168736 * p[0] - 0.331264 * p[1]
                                        + 0.5 * p[2] + 128.0
                                    : 0.5 * p[0] - 0.418688 * p[1]
                                        - 0.081312 * p[2] + 128.0;
                            }
                        }
                        block[y][x] = sum / (sx * sy) - 128.0;
                    }
                }
                for (int y = 0; y < 8; y++)
                    for (int u = 0; u < 8; u++) {
                        rows[y][u] = 0;
                        for (int x = 0; x < 8; x++)
                            rows[y][u] += basis[u][x] * block[y][x];
                    }
                short *out = jw_coef_block(&j, c, bx, by);
                for (int v = 0; v < 8; v++)
                    for (int u = 0; u < 8; u++) {
                        double f = 0;
                        for (int y = 0; y < 8; y++)
                            f += basis[v][y] * rows[y][u];
                        out[v * 8 + u] = (short) lround(
                            f / j.quant[v * 8 + u]);
                    }
            }
        }
    }
    free(rgb);
    return j;
}

/* The same coefficients with all but the size x size low-frequency square
   of each block cleared. */
static JpegCoefficients jpeg_coefficients_cut(const JpegCoefficients *j,
                                              int size)
{
    JpegCoefficients cut = *j;
    for (int c = 0; c < j->components; c++) {
        size_t count = (size_t) j->bw[c] * j->bh[c] * 64u;
        cut.coef[c] = malloc(count * sizeof(short));
        if (cut.coef[c] == NULL) abort();
        for (size_t i = 0; i < count; i++)
            cut.coef[c][i] = (short) ((int) (i % 8u) < size
                && (int) (i % 64u / 8u) < size ? j->coef[c][i] : 0);
    }
    return cut;
}

static void jpeg_coefficients_free(JpegCoefficients *j)
{
    for (int c = 0; c < j->components; c++) free(j->coef[c]);
}

/* Baseline, or progressive with successive approximation (libjpeg's
   standard script) or with spectral selection only (and one DC scan per
   component). */
static Bytes jpeg_from_coefficients(const JpegCoefficients *j,
                                    bool progressive, bool approximate)
{
    Bytes out = {0};
    const unsigned char soi[2] = {0xff, 0xd8}, eoi[2] = {0xff, 0xd9};
    bytes_append(&out, soi, 2);
    unsigned char dqt[65] = {0};
    for (int k = 0; k < 64; k++)
        dqt[1 + k] = j->quant[stbi__jpeg_dezigzag[k]];
    jw_segment(&out, 0xdb, dqt, sizeof(dqt));
    unsigned char sof[15] = {
        8, (unsigned char) (j->height >> 8), (unsigned char) j->height,
        (unsigned char) (j->width >> 8), (unsigned char) j->width,
        (unsigned char) j->components
    };
    for (int c = 0; c < j->components; c++) {
        sof[6 + c * 3] = (unsigned char) (c + 1);
        sof[7 + c * 3] = (unsigned char) ((j->h[c] << 4) | j->v[c]);
    }
    jw_segment(&out, progressive ? 0xc2 : 0xc0, sof,
               6u + 3u * (size_t) j->components);
    unsigned char dht[2 * (1 + 16 + 256)] = {0};
    for (int table = 0; table < 2; table++) {
        unsigned char *at = dht + table * (1 + 16 + 256);
        at[0] = (unsigned char) (table << 4);
        at[8] = 255;
        at[9] = 1;
        for (int symbol = 0; symbol < 256; symbol++)
            at[17 + symbol] = (unsigned char) symbol;
    }
    jw_segment(&out, 0xc4, dht, sizeof(dht));
    if (j->restart != 0) {
        unsigned char dri[2] = {(unsigned char) (j->restart >> 8),
                                (unsigned char) j->restart};
        jw_segment(&out, 0xdd, dri, 2);
    }
    JpegScanSpec all = {.count = j->components,
                        .component = {0, 1, 2}, .se = 63};
    if (!progressive) {
        jw_scan(&out, j, &all, false);
    } else {
        JpegScanSpec scans[16];
        int count = 0;
        JpegScanSpec dc = all;
        dc.se = 0;
        dc.al = approximate ? 1 : 0;
        if (approximate) scans[count++] = dc;
        for (int c = 0; !approximate && c < j->components; c++)
            scans[count++] = (JpegScanSpec) {1, {c}, 0, 0, 0, 0};
        scans[count++] = (JpegScanSpec) {1, {0}, 1, 5, 0, approximate ? 2 : 0};
        for (int c = 1; c < j->components; c++)
            scans[count++] = (JpegScanSpec) {1, {c}, 1, 63, 0,
                                             approximate ? 1 : 0};
        scans[count++] = (JpegScanSpec) {1, {0}, 6, 63, 0, approximate ? 2 : 0};
        if (approximate) {
            scans[count++] = (JpegScanSpec) {1, {0}, 1, 63, 2, 1};
            dc.ah = 1;
            dc.al = 0;
            scans[count++] = dc;
            for (int c = 1; c < j->components; c++)
                scans[count++] = (JpegScanSpec) {1, {c}, 1, 63, 1, 0};
            scans[count++] = (JpegScanSpec) {1, {0}, 1, 63, 1, 0};
        }
        for (int i = 0; i < count; i++) jw_scan(&out, j, &scans[i], true);
    }
    bytes_append(&out, eoi, 2);
    return out;
}

static int jpeg_scaled_size(int size, int shift)
{
    return (size + (1 << shift) - 1) >> shift;
}

/* Decodes `encoded` on the background worker. */
static ImageDecodeWorkerResult jpeg_worker_decode(Budget *budget,
                                                  const Bytes *encoded,
                                                  int width, int height,
                                                  int target_width,
                                                  int target_height,
                                                  size_t *arena)
{
    ImageDecodeWorkerResult result = {0};
    unsigned char *copy = budget_malloc(budget, encoded->length);
    if (copy == NULL) return result;
    memcpy(copy, encoded->data, encoded->length);
    BrowserSharedBody *body = browser_shared_body_take(budget, copy,
                                                       encoded->length);
    if (body == NULL) return result;
    ImageResource image = {
        .encoded = body->data, .encoded_body = body,
        .encoded_length = encoded->length,
        .source_width = width, .source_height = height,
        .width = target_width, .height = target_height
    };
    size_t base = budget->current, refused = 0;
    budget->peak = base;
    uint32_t token = 0;
    if (image_decode_worker_submit(&image, budget, &token, &refused)
            == IMAGE_DECODE_SUBMIT_ACCEPTED) {
        if (arena != NULL) *arena = budget->peak - base;
        (void) image_decode_worker_collect(token, &result);
    }
    browser_shared_body_release(body);
    return result;
}

static const struct {
    int width, height, components, hy, vy, restart;
    bool approximate;
} jpeg_progressive_cases[] = {
    {128, 96, 3, 2, 2, 0, true},    /* 4:2:0 */
    {97, 67, 3, 1, 1, 3, true},     /* 4:4:4, odd size, restarts */
    {121, 45, 3, 2, 1, 5, true},    /* 4:2:2, odd size, restarts */
    {77, 53, 1, 1, 1, 0, true},     /* grayscale */
    {64, 64, 3, 2, 2, 2, false},    /* spectral selection only */
    {33, 17, 3, 2, 2, 1, true},     /* a restart after every MCU */
};

/* At each reduced scale a progressive decode is exactly the baseline decode
   of the same coefficients cut to the low-frequency square the reduced
   block keeps; at full scale it is the baseline decode of them all. The
   background worker's arena holds the same decode. */
static int test_progressive_matches_cut_baseline(Budget *budget)
{
    for (size_t i = 0; i < sizeof(jpeg_progressive_cases)
                               / sizeof(jpeg_progressive_cases[0]); i++) {
        int w = jpeg_progressive_cases[i].width;
        int h = jpeg_progressive_cases[i].height;
        JpegCoefficients j = jpeg_coefficients(
            w, h, jpeg_progressive_cases[i].components,
            jpeg_progressive_cases[i].hy, jpeg_progressive_cases[i].vy,
            jpeg_progressive_cases[i].restart);
        Bytes progressive = jpeg_from_coefficients(
            &j, true, jpeg_progressive_cases[i].approximate);
        for (int shift = 0; shift <= 3; shift++) {
            int tw = jpeg_scaled_size(w, shift), th = jpeg_scaled_size(h, shift);
            JpegCoefficients cut = jpeg_coefficients_cut(&j, 8 >> shift);
            Bytes baseline = jpeg_from_coefficients(&cut, false, false);
            DecodeRun expected = decode_at(budget, &baseline, w, h, tw, th);
            DecodeRun run = decode_at(budget, &progressive, w, h, tw, th);
            size_t bytes = (size_t) tw * th * 4u;
            CHECK(expected.status == IMAGE_DECODE_SUCCEEDED
                  && run.status == IMAGE_DECODE_SUCCEEDED);
            if (memcmp(run.pixels, expected.pixels, bytes) != 0) {
                fprintf(stderr, "progressive case %zu shift %d differs\n",
                        i, shift);
                return 1;
            }
            if (shift != 0) {
                ImageDecodeWorkerResult worker = jpeg_worker_decode(
                    budget, &progressive, w, h, tw, th, NULL);
                CHECK(worker.status == IMAGE_DECODE_SUCCEEDED
                      && worker.pixels != NULL
                      && memcmp(worker.pixels, run.pixels, bytes) == 0);
                budget_free(budget, worker.pixels);
            }
            image_resource_free_decoded(budget, run.pixels);
            image_resource_free_decoded(budget, expected.pixels);
            free(baseline.data);
            jpeg_coefficients_free(&cut);
        }
        free(progressive.data);
        jpeg_coefficients_free(&j);
    }
    CHECK(images_decode_worker_shutdown(budget) && budget->current == 0);
    return 0;
}

/* A 1000x1000 4:2:0 progressive photograph shown at half, a quarter or an
   eighth of its size holds (8 >> shift)^2 coefficients and a 64-bit map
   per block until its last scan, not all 64 coefficients: at half size its
   worker arena falls from about 4.9 MB to about 2.6 MB. */
static int test_progressive_scaled_memory(Budget *budget)
{
    const int w = 1000, h = 1000;
    JpegCoefficients j = jpeg_coefficients(w, h, 3, 2, 2, 0);
    Bytes progressive = jpeg_from_coefficients(&j, true, true);
    for (int shift = 1; shift <= 3; shift++) {
        int tw = jpeg_scaled_size(w, shift), th = jpeg_scaled_size(h, shift);
        size_t size = (size_t) (8 >> shift), planes = 0, kept = 0, full = 0;
        for (int c = 0; c < 3; c++) {
            size_t blocks = (size_t) j.bw[c] * j.bh[c];
            planes += blocks * 64u >> (2 * shift);
            kept += blocks * (size * size + 4u) * 2u;
            full += blocks * 128u;
        }
        size_t target = (size_t) tw * th * 4u;
        DecodeRun run = decode_at(budget, &progressive, w, h, tw, th);
        size_t arena = 0;
        ImageDecodeWorkerResult worker = jpeg_worker_decode(
            budget, &progressive, w, h, tw, th, &arena);
        fprintf(stderr, "progressive 1000x1000 shift %d: peak=%zu "
                "arena=%zu coefficients=%zu (all 64: %zu)\n", shift,
                run.peak_delta, arena, kept, full);
        CHECK(run.status == IMAGE_DECODE_SUCCEEDED
              && worker.status == IMAGE_DECODE_SUCCEEDED);
        CHECK(memcmp(run.pixels, worker.pixels, target) == 0);
        size_t bound = target + planes + kept + 256u * 1024u;
        CHECK(run.peak_delta <= bound && arena <= bound + progressive.length);
        image_resource_free_decoded(budget, run.pixels);
        budget_free(budget, worker.pixels);
    }
    free(progressive.data);
    jpeg_coefficients_free(&j);
    CHECK(images_decode_worker_shutdown(budget) && budget->current == 0);
    return 0;
}

/* Cutting coefficients instead of averaging all of them changes a reduced
   picture slightly: it drops the high frequencies a box average folds in,
   as libjpeg's scaled IDCT does. The area average of the full decode is
   that box average, so it favours the baseline decode; the cut stays
   within a few levels of it and above the scaled-decode floor. */
static int test_progressive_scaled_quality(Budget *budget)
{
    for (size_t i = 0; i < 3; i++) {
        int w = i == 0 ? 640 : i == 1 ? 333 : 400;
        int h = i == 0 ? 480 : i == 1 ? 251 : 300;
        JpegCoefficients j = jpeg_coefficients(w, h, 3, i == 2 ? 1 : 2,
                                               i == 2 ? 1 : 2, 0);
        Bytes progressive = jpeg_from_coefficients(&j, true, true);
        Bytes baseline = jpeg_from_coefficients(&j, false, false);
        DecodeRun full = decode_at(budget, &baseline, w, h, w, h);
        CHECK(full.status == IMAGE_DECODE_SUCCEEDED);
        for (int shift = 1; shift <= 3; shift++) {
            int tw = jpeg_scaled_size(w, shift), th = jpeg_scaled_size(h, shift);
            unsigned char *reference = area_reference(full.pixels, w, h,
                                                      tw, th);
            DecodeRun cut = decode_at(budget, &progressive, w, h, tw, th);
            DecodeRun averaged = decode_at(budget, &baseline, w, h, tw, th);
            CHECK(cut.status == IMAGE_DECODE_SUCCEEDED
                  && averaged.status == IMAGE_DECODE_SUCCEEDED);
            double cut_db = psnr(cut.pixels, reference, (size_t) tw * th);
            double averaged_db = psnr(averaged.pixels, reference,
                                      (size_t) tw * th);
            double between = psnr(cut.pixels, averaged.pixels,
                                  (size_t) tw * th);
            fprintf(stderr, "progressive %dx%d shift %d: cut %.2f dB, "
                    "averaged %.2f dB, between %.2f dB\n", w, h, shift,
                    cut_db, averaged_db, between);
            CHECK(cut_db >= 24.0 && between >= 30.0);
            image_resource_free_decoded(budget, cut.pixels);
            image_resource_free_decoded(budget, averaged.pixels);
            free(reference);
        }
        image_resource_free_decoded(budget, full.pixels);
        free(progressive.data);
        free(baseline.data);
        jpeg_coefficients_free(&j);
    }
    CHECK(budget->current == 0);
    return 0;
}

/* Corrupt progressive streams (cut short, damaged scan data, impossible
   scan parameters) either decode or fail at every scale, on both paths,
   and return all their memory. */
static int test_progressive_scaled_hostile(Budget *budget)
{
    for (size_t i = 0; i < sizeof(jpeg_progressive_cases)
                               / sizeof(jpeg_progressive_cases[0]); i++) {
        int w = jpeg_progressive_cases[i].width;
        int h = jpeg_progressive_cases[i].height;
        JpegCoefficients j = jpeg_coefficients(
            w, h, jpeg_progressive_cases[i].components,
            jpeg_progressive_cases[i].hy, jpeg_progressive_cases[i].vy,
            jpeg_progressive_cases[i].restart);
        Bytes good = jpeg_from_coefficients(
            &j, true, jpeg_progressive_cases[i].approximate);
        size_t first_scan = 0;
        for (size_t at = 2; at + 1 < good.length && first_scan == 0; at++)
            if (good.data[at] == 0xff && good.data[at + 1] == 0xda)
                first_scan = at;
        CHECK(first_scan != 0);
        for (int variant = 0; variant < 12; variant++) {
            Bytes bad = {0};
            bytes_append(&bad, good.data, good.length);
            size_t data = first_scan + 2u + good.data[first_scan + 3];
            if (variant < 3) {
                bad.length = data + (good.length - data) * (size_t) (variant + 1) / 4u;
            } else if (variant < 9) {
                for (size_t at = data + (size_t) variant * 7u;
                     at < bad.length - 2u; at += 97u + (size_t) variant * 13u)
                    bad.data[at] ^= (unsigned char) (0x5a + variant);
            } else {
                /* Ss, Se and Ah/Al of the first scan. */
                unsigned char *tail = good.data + data - 3u;
                size_t offset = (size_t) (tail - good.data);
                if (variant == 9) bad.data[offset + 1] = 64;
                if (variant == 10) bad.data[offset] = 9, bad.data[offset + 1] = 3;
                if (variant == 11) bad.data[offset + 2] = 0x0e;
            }
            for (int shift = 0; shift <= 3; shift++) {
                int tw = jpeg_scaled_size(w, shift);
                int th = jpeg_scaled_size(h, shift);
                DecodeRun run = decode_at(budget, &bad, w, h, tw, th);
                if (run.pixels != NULL)
                    image_resource_free_decoded(budget, run.pixels);
                if (shift != 0) {
                    ImageDecodeWorkerResult worker = jpeg_worker_decode(
                        budget, &bad, w, h, tw, th, NULL);
                    budget_free(budget, worker.pixels);
                }
                CHECK(images_decode_worker_shutdown(budget)
                      && budget->current == 0);
            }
            free(bad.data);
        }
        free(good.data);
        jpeg_coefficients_free(&j);
    }
    return 0;
}

/* Components no scan of a progressive stream cut short reaches decode as
   neutral (here: no colour), at every scale, and never show what the heap
   held before; freed memory is dirtied first so a reused block would. */
static int test_progressive_cut_short_is_neutral(Budget *budget)
{
    const int w = 64, h = 64;
    JpegCoefficients j = jpeg_coefficients(w, h, 3, 1, 1, 0);
    Bytes stream = jpeg_from_coefficients(&j, true, false);
    size_t scans[2] = {0, 0};
    for (size_t at = 2, found = 0; at + 1 < stream.length && found < 2; at++)
        if (stream.data[at] == 0xff && stream.data[at + 1] == 0xda)
            scans[found++] = at;
    CHECK(scans[1] != 0);
    /* Only the luma DC scan, then the end of the image. */
    stream.length = scans[1];
    const unsigned char eoi[2] = {0xff, 0xd9};
    bytes_append(&stream, eoi, 2);
    for (int shift = 0; shift <= 3; shift++) {
        int tw = jpeg_scaled_size(w, shift), th = jpeg_scaled_size(h, shift);
        void *dirty[16];
        for (int i = 0; i < 16; i++) {
            dirty[i] = budget_malloc(budget, (size_t) w * h * 2u + 15u);
            CHECK(dirty[i] != NULL);
            memset(dirty[i], 0x5a, (size_t) w * h * 2u + 15u);
        }
        for (int i = 0; i < 16; i++) budget_free(budget, dirty[i]);
        DecodeRun run = decode_at(budget, &stream, w, h, tw, th);
        CHECK(run.status == IMAGE_DECODE_SUCCEEDED);
        for (size_t i = 0; i < (size_t) tw * th; i++) {
            const unsigned char *p = run.pixels + i * 4u;
            CHECK(p[0] == p[1] && p[1] == p[2] && p[3] == 255);
        }
        image_resource_free_decoded(budget, run.pixels);
    }
    free(stream.data);
    jpeg_coefficients_free(&j);
    CHECK(budget->current == 0);
    return 0;
}
