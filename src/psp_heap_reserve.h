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

#include "tilefinch/budget.h"

/* Samples the heap (mallinfo) each time `budget`'s global peak rises by
   256 KiB and keeps the sample with the most bytes in use, for the
   phase=peak line of psp_heap_commit_report. */
void psp_heap_commit_watch(const Budget *budget);
/* Logs one tilefinch-heap-commit line for `phase` (the newlib heap's
   committed arena against the Budget's charge); with `with_peak` it then
   logs the kept high-water sample as phase=peak. */
void psp_heap_commit_report(const char *phase, const Budget *budget,
                            bool with_peak);
#endif

#endif
