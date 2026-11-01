#ifndef SWDEC_CHROMA_REPLAY_H
#define SWDEC_CHROMA_REPLAY_H
#include "swdec_diag.h"
#if SWDEC_DIAG_ENABLED
/* Validation builds: the chroma-on-the-Media-Engine qualification replay
 * (swdec_chroma_replay.c). Reset at a stream's open (on_me: replay on the
 * Media Engine, else the scalar kernels run on the calling core), the row
 * marks drive it, report at close. */
extern int swdec_chroma_replay_enabled;   /* off unless asked for */
void swdec_chroma_replay_reset(int on_me);
void swdec_chroma_replay_install(void);   /* each decode call: the ME hook */
void swdec_chroma_replay_row_begin(void);
void swdec_chroma_replay_row_end(void);
void swdec_chroma_replay_report(void (*log)(const char *format, ...));
#endif
#endif
