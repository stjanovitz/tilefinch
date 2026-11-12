#ifndef TILEFINCH_PSP_HEAP_RESERVE_H
#define TILEFINCH_PSP_HEAP_RESERVE_H

#include <stdbool.h>

/* Whether boot found the optional software decoder installed, and the
   partition threshold (KiB outside the newlib heap) it chose for it
   (psp_heap_reserve.c, tilefinch_psp_heap_threshold_kb). */
bool psp_heap_reserve_swdec_found(void);
int psp_heap_reserve_threshold_kb(void);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
/* Logs the newlib heap's size, its arena and its free bytes. */
void psp_heap_reserve_report(void);
#endif

#endif
