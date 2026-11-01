/* swdec_csc.c - see swdec_csc.h. */
#include "swdec_csc.h"

#include <string.h>

static inline int csc_clamp(int v, int cap)
{
#if defined(__psp__) || defined(PSP)
    int r;
    __asm__ ("max %0, %1, $0\n\tmin %0, %0, %2" : "=r" (r) : "r" (v), "r" (cap));
    return r;
#else
    return v < 0 ? 0 : v > cap ? cap : v;
#endif
}

static inline uint16_t csc_565(int yy, int rc, int gc, int bc)
{
    int r = csc_clamp((yy + rc) >> 8, 255);
    int g = csc_clamp((yy + gc) >> 8, 255);
    int b = csc_clamp((yy + bc) >> 8, 255);
    return (uint16_t) (((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3));
}

/* Columns [x0, width) of the row pair starting at luma row j. */
static void csc_pair_c(const SwdecCscPlanes *p, int j, int x0)
{
    const uint8_t *y0 = p->y + j * p->y_stride;
    const uint8_t *y1 = y0 + p->y_stride;
    const uint8_t *u = p->u + (j >> 1) * p->uv_stride;
    const uint8_t *v = p->v + (j >> 1) * p->uv_stride;
    uint16_t *d0 = p->dst + j * p->dst_stride;
    uint16_t *d1 = d0 + p->dst_stride;
    for (int i = x0; i < p->width; i += 2) {
        int cu = u[i >> 1] - 128, cv = v[i >> 1] - 128;
        int rc = 409 * cv + 128 - 298 * 16;
        int gc = -100 * cu - 208 * cv + 128 - 298 * 16;
        int bc = 516 * cu + 128 - 298 * 16;
        d0[i]     = csc_565(298 * y0[i],     rc, gc, bc);
        d0[i + 1] = csc_565(298 * y0[i + 1], rc, gc, bc);
        d1[i]     = csc_565(298 * y1[i],     rc, gc, bc);
        d1[i + 1] = csc_565(298 * y1[i + 1], rc, gc, bc);
    }
}

void swdec_csc_rows_c(const SwdecCscPlanes *p, int row0, int row1)
{
    for (int j = row0; j + 1 < row1; j += 2)
        csc_pair_c(p, j, 0);
}

#if defined(__psp__) || defined(PSP)
/*
 * VFPU form of csc_565, sixteen pixels (two rows of eight, sharing four
 * chroma pairs) per step. Exactness follows the swdec kernel convention
 * (libavcodec/psp/vfpu_util.h): bytes enter as b - 128 through
 * xor/mtv/vc2i/vi2f 24, every sum is an integer below 2^24 so vscl/vadd are
 * exact, and vf2id 15 + vi2uc yields clip_uint8(floor(sum / 256)).
 *
 * With y' = y - 128, cu = u - 128, cv = v - 128 the portable sums are
 *   R = 298 y' + 409 cv              + 33504
 *   G = 298 y' - 100 cu - 208 cv     + 33504
 *   B = 298 y' + 516 cu              + 33504
 * (33504 = 298 * 128 + 128 - 298 * 16). Each four-pixel group fills a
 * matrix whose columns are R, G, B and y'; its rows are then the pixels as
 * R,G,B,x quads, vi2uc packs each into an 8888 word (R in the low byte) and
 * vt5650 packs four of those into the 565 layout csc_565 produces (R in the
 * low bits, alpha ignored).
 *
 * Registers: M000-M300 groups (C030 etc. hold y'), M400/C500/C510 chroma
 * terms, C700/C710 constants, C600/C620/C520/C720 packed 8888 pixels and
 * C610/C630/C530/C730 the 565 pairs.
 */
static const float csc_vfpu_k[8] __attribute__((aligned(16))) = {
    298.0f, 409.0f, -100.0f, -208.0f, 516.0f, 33504.0f, 0.0f, 0.0f
};

static inline void csc_vfpu_load_constants(void)
{
    __asm__ volatile (
        "lv.q C700, 0(%0)\n\t"
        "lv.q C710, 16(%0)\n\t"
        : : "r" (csc_vfpu_k) : "memory");
}

static inline void csc_vfpu_step16(
    const uint8_t *y0, const uint8_t *y1, const uint8_t *u,
    const uint8_t *v, uint16_t *d0, uint16_t *d1)
{
    unsigned t0, t1, t2, t3, t4, t5;
    __asm__ volatile (
        "lw    %[t0], 0(%[u])\n\t"
        "lw    %[t1], 0(%[v])\n\t"
        "lw    %[t2], 0(%[y0])\n\t"
        "lw    %[t3], 4(%[y0])\n\t"
        "lw    %[t4], 0(%[y1])\n\t"
        "lw    %[t5], 4(%[y1])\n\t"
        "xor   %[t0], %[t0], %[x]\n\t"
        "xor   %[t1], %[t1], %[x]\n\t"
        "xor   %[t2], %[t2], %[x]\n\t"
        "xor   %[t3], %[t3], %[x]\n\t"
        "xor   %[t4], %[t4], %[x]\n\t"
        "xor   %[t5], %[t5], %[x]\n\t"
        "mtv   %[t0], S400\n\t"
        "mtv   %[t1], S410\n\t"
        "mtv   %[t2], S030\n\t"
        "mtv   %[t3], S130\n\t"
        "mtv   %[t4], S230\n\t"
        "mtv   %[t5], S330\n\t"
        "vc2i.s C400, S400\n\t"
        "vc2i.s C410, S410\n\t"
        "vc2i.s C030, S030\n\t"
        "vc2i.s C130, S130\n\t"
        "vc2i.s C230, S230\n\t"
        "vc2i.s C330, S330\n\t"
        "vi2f.q C400, C400, 24\n\t"            /* cu x4 */
        "vi2f.q C410, C410, 24\n\t"            /* cv x4 */
        "vi2f.q C030, C030, 24\n\t"            /* y' row 0, pixels 0-3 */
        "vi2f.q C130, C130, 24\n\t"            /* y' row 0, pixels 4-7 */
        "vi2f.q C230, C230, 24\n\t"            /* y' row 1, pixels 0-3 */
        "vi2f.q C330, C330, 24\n\t"            /* y' row 1, pixels 4-7 */
        "vscl.q C420, C410, S701\n\t"          /* 409 cv */
        "vscl.q C500, C400, S710\n\t"          /* 516 cu */
        "vscl.q C430, C400, S702\n\t"          /* -100 cu */
        "vscl.q C510, C410, S703\n\t"          /* -208 cv */
        "vscl.q C030, C030, S700\n\t"          /* 298 y' */
        "vscl.q C130, C130, S700\n\t"
        "vscl.q C230, C230, S700\n\t"
        "vscl.q C330, C330, S700\n\t"
        "vadd.q C420, C420, C710[y,y,y,y]\n\t" /* rc + 33504 */
        "vadd.q C430, C430, C510\n\t"
        "vadd.q C500, C500, C710[y,y,y,y]\n\t" /* bc + 33504 */
        "vadd.q C430, C430, C710[y,y,y,y]\n\t" /* gc + 33504 */
        "vadd.q C000, C030, C420[x,x,y,y]\n\t" /* R */
        "vadd.q C100, C130, C420[z,z,w,w]\n\t"
        "vadd.q C200, C230, C420[x,x,y,y]\n\t"
        "vadd.q C300, C330, C420[z,z,w,w]\n\t"
        "vadd.q C010, C030, C430[x,x,y,y]\n\t" /* G */
        "vadd.q C110, C130, C430[z,z,w,w]\n\t"
        "vadd.q C210, C230, C430[x,x,y,y]\n\t"
        "vadd.q C310, C330, C430[z,z,w,w]\n\t"
        "vadd.q C020, C030, C500[x,x,y,y]\n\t" /* B */
        "vadd.q C120, C130, C500[z,z,w,w]\n\t"
        "vadd.q C220, C230, C500[x,x,y,y]\n\t"
        "vadd.q C320, C330, C500[z,z,w,w]\n\t"
        "vf2id.q C000, C000, 15\n\t"
        "vf2id.q C100, C100, 15\n\t"
        "vf2id.q C200, C200, 15\n\t"
        "vf2id.q C300, C300, 15\n\t"
        "vf2id.q C010, C010, 15\n\t"
        "vf2id.q C110, C110, 15\n\t"
        "vf2id.q C210, C210, 15\n\t"
        "vf2id.q C310, C310, 15\n\t"
        "vf2id.q C020, C020, 15\n\t"
        "vf2id.q C120, C120, 15\n\t"
        "vf2id.q C220, C220, 15\n\t"
        "vf2id.q C320, C320, 15\n\t"
        "vi2uc.q S600, R000\n\t"
        "vi2uc.q S620, R100\n\t"
        "vi2uc.q S520, R200\n\t"
        "vi2uc.q S720, R300\n\t"
        "vi2uc.q S601, R001\n\t"
        "vi2uc.q S621, R101\n\t"
        "vi2uc.q S521, R201\n\t"
        "vi2uc.q S721, R301\n\t"
        "vi2uc.q S602, R002\n\t"
        "vi2uc.q S622, R102\n\t"
        "vi2uc.q S522, R202\n\t"
        "vi2uc.q S722, R302\n\t"
        "vi2uc.q S603, R003\n\t"
        "vi2uc.q S623, R103\n\t"
        "vi2uc.q S523, R203\n\t"
        "vi2uc.q S723, R303\n\t"
        "vt5650.q C610, C600\n\t"
        "vt5650.q C630, C620\n\t"
        "vt5650.q C530, C520\n\t"
        "vt5650.q C730, C720\n\t"
        "sv.s  S610, 0(%[d0])\n\t"
        "sv.s  S611, 4(%[d0])\n\t"
        "sv.s  S630, 8(%[d0])\n\t"
        "sv.s  S631, 12(%[d0])\n\t"
        "sv.s  S530, 0(%[d1])\n\t"
        "sv.s  S531, 4(%[d1])\n\t"
        "sv.s  S730, 8(%[d1])\n\t"
        "sv.s  S731, 12(%[d1])\n\t"
        : [t0] "=&r" (t0), [t1] "=&r" (t1), [t2] "=&r" (t2),
          [t3] "=&r" (t3), [t4] "=&r" (t4), [t5] "=&r" (t5)
        : [y0] "r" (y0), [y1] "r" (y1), [u] "r" (u), [v] "r" (v),
          [d0] "r" (d0), [d1] "r" (d1), [x] "r" (0x80808080u)
        : "memory");
}

void swdec_csc_rows_vfpu(const SwdecCscPlanes *p, int row0, int row1)
{
    /* Word loads of the planes and word stores of the destination. */
    if ((((uintptr_t) p->y | (uintptr_t) p->u | (uintptr_t) p->v
          | (uintptr_t) p->dst) & 3u) != 0
        || ((p->y_stride | p->uv_stride | (p->dst_stride * 2)) & 3) != 0
        || (row0 & 1) != 0) {
        swdec_csc_rows_c(p, row0, row1);
        return;
    }
    const int vector_width = p->width & ~7;
    csc_vfpu_load_constants();
    for (int j = row0; j + 1 < row1; j += 2) {
        const uint8_t *y0 = p->y + j * p->y_stride;
        const uint8_t *y1 = y0 + p->y_stride;
        const uint8_t *u = p->u + (j >> 1) * p->uv_stride;
        const uint8_t *v = p->v + (j >> 1) * p->uv_stride;
        uint16_t *d0 = p->dst + j * p->dst_stride;
        uint16_t *d1 = d0 + p->dst_stride;
        for (int i = 0; i < vector_width; i += 8)
            csc_vfpu_step16(y0 + i, y1 + i, u + (i >> 1), v + (i >> 1),
                            d0 + i, d1 + i);
        if (vector_width < p->width)
            csc_pair_c(p, j, vector_width);
    }
}

/* 8 rows x 262 columns: every luma value on every row (in a different
   phase per row), 524 chroma pairs including both ends of each range, and
   a six-pixel scalar tail after the vector columns. */
#define CSC_CHECK_W 262
#define CSC_CHECK_STRIDE 264
#define CSC_CHECK_H 8
static int csc_check_state;   /* 0 unknown, 1 agrees, -1 disagrees */

int swdec_csc_vfpu_verified(void)
{
    if (csc_check_state != 0) return csc_check_state > 0;
    uint8_t y[CSC_CHECK_H * CSC_CHECK_STRIDE] __attribute__((aligned(16)));
    uint8_t u[CSC_CHECK_H / 2 * CSC_CHECK_STRIDE / 2] __attribute__((aligned(16)));
    uint8_t v[CSC_CHECK_H / 2 * CSC_CHECK_STRIDE / 2] __attribute__((aligned(16)));
    uint16_t a[CSC_CHECK_H * CSC_CHECK_STRIDE] __attribute__((aligned(16)));
    uint16_t b[CSC_CHECK_H * CSC_CHECK_STRIDE] __attribute__((aligned(16)));
    for (int r = 0; r < CSC_CHECK_H; r++)
        for (int c = 0; c < CSC_CHECK_STRIDE; c++)
            y[r * CSC_CHECK_STRIDE + c] = (uint8_t) (c + 37 * r);
    for (int r = 0; r < CSC_CHECK_H / 2; r++)
        for (int c = 0; c < CSC_CHECK_STRIDE / 2; c++) {
            u[r * CSC_CHECK_STRIDE / 2 + c] = (uint8_t) (2 * c + 67 * r);
            v[r * CSC_CHECK_STRIDE / 2 + c] = (uint8_t) (255 - 3 * c - 29 * r);
        }
    u[0] = 0; v[0] = 0; u[1] = 255; v[1] = 255; u[2] = 0; v[2] = 255;
    u[3] = 255; v[3] = 0;
    memset(a, 0x5A, sizeof a);
    memset(b, 0x5A, sizeof b);
    SwdecCscPlanes p = {
        .y = y, .u = u, .v = v,
        .y_stride = CSC_CHECK_STRIDE, .uv_stride = CSC_CHECK_STRIDE / 2,
        .width = CSC_CHECK_W, .dst = a, .dst_stride = CSC_CHECK_STRIDE
    };
    swdec_csc_rows_c(&p, 0, CSC_CHECK_H);
    p.dst = b;
    swdec_csc_rows_vfpu(&p, 0, CSC_CHECK_H);
    csc_check_state = memcmp(a, b, sizeof a) == 0 ? 1 : -1;
    return csc_check_state > 0;
}
#endif
