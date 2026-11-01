/* swdec_csc.h - YUV 4:2:0 (BT.601 limited range) to RGB565 for the swdec
 * presenter surface.
 *
 * Both converters write rows [row0, row1) of the destination, two luma rows
 * at a time (a trailing odd row is left alone, as it always was). The
 * portable one runs on either core; the VFPU one only on the main CPU, and
 * only after swdec_csc_vfpu_verified() said it agrees with the portable one
 * on this machine.
 */
#ifndef SWDEC_CSC_H
#define SWDEC_CSC_H

#include <stdint.h>

typedef struct {
    const uint8_t *y, *u, *v;
    int y_stride, uv_stride;
    int width;                 /* even */
    uint16_t *dst;
    int dst_stride;            /* pixels */
} SwdecCscPlanes;

void swdec_csc_rows_c(const SwdecCscPlanes *p, int row0, int row1);

#if defined(__psp__) || defined(PSP)
/* Main CPU only (the Media Engine has no VFPU). The caller's thread must
   have the VFPU attribute. Falls back to the portable converter for any
   geometry the vector loop cannot address word-aligned. */
void swdec_csc_rows_vfpu(const SwdecCscPlanes *p, int row0, int row1);
/* Converts a synthetic block that walks every luma value against a spread
   of chroma pairs (and the extremes) through both converters, once; 1 when
   they agree byte for byte. Main CPU, VFPU thread. */
int swdec_csc_vfpu_verified(void);
#endif

#endif
