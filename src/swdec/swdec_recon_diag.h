#ifndef SWDEC_RECON_DIAG_H
#define SWDEC_RECON_DIAG_H
#include "swdec_diag.h"
#if SWDEC_DIAG_ENABLED
/* Validation builds: main-core attribution of the decode thread's time
 * (see swdec_recon_diag.c and libavcodec/swdec_recon_stats.h). Reset at a
 * stream's open, account each decode call, report at close. */
void swdec_recon_diag_reset(void);
unsigned swdec_recon_diag_run_us(void);   /* this thread's run time */
void swdec_recon_diag_au(unsigned wall_us, unsigned run_us);
void swdec_recon_diag_report(unsigned pictures,
                             void (*log)(const char *format, ...));
#endif
#endif
