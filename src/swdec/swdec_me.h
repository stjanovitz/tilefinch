#ifndef SWDEC_ME_H
#define SWDEC_ME_H
#include <stddef.h>
/* PSP only: parse on the Media Engine (see swdec_me.c). */
int  swdec_me_attach_path(
    const char *helper_path,
    void (*log)(const char *fmt, ...));
int  swdec_me_attach(void (*log)(const char *fmt, ...));
void swdec_me_detach(void);
int  swdec_me_restore(void);
int  swdec_me_recover(void);   /* re-boot the ME + hooks after a failure (resident runner) */
extern int swdec_me_failed;   /* 1 after any ME failure (ME already stopped, hooks removed) */
/* audio on the ME (call setup after swdec_open + swdec_me_attach) */
int swdec_me_audio_setup(void *arena_unused);
int swdec_me_audio_submit(const void *adts, unsigned len);
int swdec_me_audio_poll(unsigned upto, unsigned *crc, unsigned *samples);
unsigned swdec_me_audio_done(void);
unsigned swdec_me_audio_us(void);
unsigned swdec_me_audio_errors(void);
int swdec_me_audio_cpu_decode(const void *adts, unsigned len, unsigned *crc);
int swdec_me_audio_reset(unsigned timeout_us);
int swdec_me_audio_shutdown(unsigned timeout_us);
/* playback: interleaved S16 stereo PCM for decoded frame `idx` (1024 samples/ch),
   valid once swdec_me_audio_done() > idx and until idx+32 frames were submitted */
const short *swdec_me_audio_pcm(unsigned idx);
/* fault-injection test support */
void swdec_me_poison_arm(void);   /* next audio service on the ME faults deliberately */
void swdec_me_mark_dead(void);    /* force the recover path when the watchdog didn't run */
/* CSC on the ME: RGB565 conversion of each row batch right after it is
   deblocked (rows are ME-cache-hot). Assign a destination before each AU;
   close at AU end (the CPU converts whatever the ME did not reach). */
#define SWDEC_ME_CSC_SLOTS 25
void swdec_me_csc_begin(int slot, void *dst_rgb565, int stride_pixels,
                        size_t capacity_bytes);
void swdec_me_csc_off(void);
int  swdec_me_csc_close(void);                          /* 1 = slot holds a frame */
unsigned swdec_me_csc_stats(unsigned *cpu_batches);     /* returns ME batches */
void swdec_me_csc_stats_reset(void);
#endif
