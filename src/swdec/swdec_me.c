/* swdec_me.c - PSP: run the H.264 parser on the Media Engine.
 *
 * CPU side: swdec_me_attach() loads the kernel helper (swdec-meload.prx),
 * boots the ME on swdec_me_worker() and installs the pipeline hooks; the
 * decoder's slice dispatcher then hands each CABAC slice to the ME through
 * swdec_me_run_slice_impl() while this core reconstructs and deblocks the
 * published rows (see libavcodec/h264_slice.c, swdec_pipe.h).
 * ME side: swdec_me_worker() runs ff_swdec_decode_slice() (plain C, no
 * syscalls, no VFPU) and writes its D-cache back at every row publish.
 * All RAM-only. Build with -DSWDEC_ME=1 (harness: SWDEC_ME=1).
 */
#include <pspkernel.h>
#include <pspsdk.h>
#include <psputils.h>
#include <string.h>
#include <stddef.h>
#include "swdec_me.h"
#include "psp_thread_contract.h"
#include "swdec.h"
#include "swdec_arena.h"
#include "swdec_bounds.h"
#include "libavcodec/h264dec.h"
#include "libavcodec/swdec_pipe.h"

int swdec_me_boot(unsigned entry, unsigned arg, unsigned stack_top);
int swdec_me_stop(void);
int swdec_me_fault(unsigned *cause, unsigned *epc);

typedef struct {
    volatile unsigned cmd, done, heartbeat, status;
    volatile unsigned avctx, sl, token;
    volatile unsigned pad[9];
} SwdecMeBox;
static SwdecMeBox me_box_storage __attribute__((aligned(64)));
static unsigned char me_stack[512 * 1024] __attribute__((aligned(64)));
static SceUID me_mod = -1;
static int me_attached;
static void (*me_log)(const char *fmt, ...);
static unsigned me_slices, me_wait_us;
static volatile unsigned swdec_me_beat;      /* bumped by the dispatcher; the watchdog watches it */
static volatile int me_dispatching;          /* watchdog is armed only while a slice is in flight */
static volatile unsigned watchdog_on;
static SceUID watchdog_th = -1;

#define UNC(p) ((void *) ((unsigned) (p) | 0x40000000u))   /* uncached alias, valid on both cores */

/* ------------------------------ Media Engine side ------------------------------ */
static void me_dcache_writeback_all(void)
{
    __asm__ volatile ("sync\n\t");
    /* 64 KB of index range: covers any plausible ME D-cache size (the proven
       whole-decoder spike used the same; extra indices wrap harmlessly) */
    for (unsigned a = 0; a < 65536; a += 64)
        __asm__ volatile ("cache 0x14, 0(%0)" : : "r"(0x80000000u + a));   /* index writeback-invalidate */
    __asm__ volatile ("sync\n\t");
}
static void me_pre_publish(void) { me_dcache_writeback_all(); }

static void me_csc_idle_one(void);      /* defined with the CSC block below */
static void me_csc_idle_one_ex(int wb);

/* The idle loop is read-only and the command is a magic word: if the CPU side
   ever dies and this memory is reused, an orphaned ME can neither corrupt it
   nor be tricked into running a job. */
#define SWDEC_ME_CMD_SLICE 0x5A11CE01u
static void __attribute__((noinline)) swdec_me_worker(SwdecMeBox *b)
{
    __asm__ volatile ("mtc0 %0, $12\n\tnop\n\tnop\n\t" : : "r"(0x60400000u));   /* CU2|CU1|BEV */
    /* A re-boot (recover/restore/attach cycle) starts with whatever the dcache
       held before the reset: valid-but-stale lines for arena addresses the CPU
       is about to reuse. Cold boot only works because the cache is empty. Sweep
       it now — this runs before the CPU writes any post-boot state, so writing
       back the few dirty idle-loop lines is harmless and stale clean lines just
       invalidate. */
    me_dcache_writeback_all();
    b->heartbeat = 1;                                  /* one-time hello */
    for (;;) {
        /* inter-AU idle: CSC first (the CPU is waiting to close this frame);
           audio second - its PCM ring holds ~743ms and can wait a millisecond */
        me_csc_idle_one_ex(1);
        {   /* the hook is installed after boot: read it through the uncached alias */
            void (*aux_)(void) = *(void (**)(void)) UNC(&swdec_me_aux);
            if (aux_) aux_();
        }
        if (b->cmd == SWDEC_ME_CMD_SLICE && b->token == ~b->avctx) {
            int r = ff_swdec_decode_slice((struct AVCodecContext *) b->avctx, (void *) b->sl);
            me_dcache_writeback_all();
            swdec_pipe_set_parse_done();
#ifdef SWDEC_ME_DEBLOCK
            {   /* drain: filter the tail rows the CPU publishes after parse
                   ends; the gaps between jobs are the ME's real idle window -
                   spend them converting final rows to RGB */
                const H264SliceContext *slc = (const H264SliceContext *) b->sl;
                while (!(swdec_pipe_tail_pushed() && !swdec_pipe_deblock_pending())) {
                    swdec_pipe_deblock_service(slc->h264);
                    if (!swdec_pipe_deblock_pending()) me_csc_idle_one();
                }
                me_csc_idle_one();   /* one more slice of idle before handing off */
                me_dcache_writeback_all();
            }
#endif
            b->status = (unsigned) r;
            b->cmd = 0; b->done = b->done + 1;
        }
    }
}
/* ------------------------------------------------------------------------------ */

int swdec_me_noinv = 0;   /* diagnostics: 1 = skip the per-row cache maintenance (expect mismatches) */
static int me_dead;

/* CPU: invalidate the cached copies of the tables the parser wrote for row mb_y */
static void cpu_pre_consume(const H264Context *h, int mb_y)
{
    if (swdec_me_noinv) return;
    const int mbs = h->mb_stride;
    const int mb0 = mb_y * mbs;
    sceKernelDcacheWritebackInvalidateRange(h->cur_pic.mb_type + mb0, (unsigned) mbs * sizeof(uint32_t));
    sceKernelDcacheWritebackInvalidateRange(h->cur_pic.qscale_table + mb0, (unsigned) mbs);
    sceKernelDcacheWritebackInvalidateRange(h->non_zero_count[mb0], (unsigned) mbs * 48);
    sceKernelDcacheWritebackInvalidateRange(h->cbp_table + mb0, (unsigned) mbs * sizeof(uint16_t));
    sceKernelDcacheWritebackInvalidateRange(h->chroma_pred_mode_table + mb0, (unsigned) mbs);
    sceKernelDcacheWritebackInvalidateRange(h->slice_table + mb0, (unsigned) mbs * sizeof(uint16_t));
    sceKernelDcacheWritebackInvalidateRange(h->list_counts + mb0, (unsigned) mbs);
    for (int list = 0; list < 2; list++) {
        if (h->cur_pic.motion_val[list]) {
            const int b0 = 4 * mb_y * h->b_stride;
            sceKernelDcacheWritebackInvalidateRange(h->cur_pic.motion_val[list] + b0, (unsigned) 4 * h->b_stride * sizeof(int16_t) * 2);
        }
        if (h->cur_pic.ref_index[list])
            sceKernelDcacheWritebackInvalidateRange(h->cur_pic.ref_index[list] + 4 * mb0, (unsigned) 4 * mbs);
    }
}
static void cpu_post_consume(void *p, size_t n) { if (!swdec_me_noinv) sceKernelDcacheWritebackInvalidateRange(p, (unsigned) n); }

#ifdef SWDEC_ME_DEBLOCK
/* CPU: before the ME filters row mb_y, write back + invalidate its pixels and
   the lines above it that filtering touches (the CPU wrote all of them) */
static void cpu_flush_row(const H264Context *h, int mb_y)
{
    AVFrame *f = h->cur_pic.f;
    int ls = f->linesize[0], uvls = f->linesize[1];
    int y0 = mb_y * 16 - 4; if (y0 < 0) y0 = 0;
    int y1 = (mb_y + 1) * 16; if (y1 > f->height) y1 = f->height;
    sceKernelDcacheWritebackInvalidateRange(f->data[0] + y0 * ls, (unsigned) ((y1 - y0) * ls));
    int c0 = mb_y * 8 - 2; if (c0 < 0) c0 = 0;
    int c1 = (mb_y + 1) * 8; if (c1 > f->height / 2) c1 = f->height / 2;
    sceKernelDcacheWritebackInvalidateRange(f->data[1] + c0 * uvls, (unsigned) ((c1 - c0) * uvls));
    sceKernelDcacheWritebackInvalidateRange(f->data[2] + c0 * uvls, (unsigned) ((c1 - c0) * uvls));
}
static void me_post_deblock(void) { me_dcache_writeback_all(); }
#endif

/* ---------------- CSC on the parser core (row-hot after deblock) ----------------
 * The CPU assigns an RGB565 destination slot before each AU; the ME converts
 * each 16-pixel row batch the moment the batch above it finishes deblocking
 * (its pixels are final and still in the ME dcache). The CPU finishes the
 * batches the ME didn't reach (normally the frame tail it deblocks itself)
 * at AU end via swdec_me_csc_close() - the done mask makes the split
 * self-balancing, same as the deblock work stealing. */
#define CSC_SLOTS SWDEC_ME_CSC_SLOTS
typedef struct {
    const uint8_t *y, *u, *v;       /* source planes (recorded by the ME) */
    int ys, uvs, w, h;              /* strides / cropped dims */
    uint16_t *dst;
    int dst_stride;
    size_t dst_capacity;
    volatile unsigned done;         /* bit n = row batch n converted */
    volatile unsigned recorded;     /* the ME saw this frame */
    volatile unsigned rejected;     /* decoded geometry exceeds dst */
    volatile int max_final;         /* highest row batch whose pixels are final */
} SwdecCscSlot;
static SwdecCscSlot csc_slots[CSC_SLOTS] __attribute__((aligned(64)));
static volatile int csc_active = -1;      /* slot for the AU being decoded, -1 = off */
static volatile unsigned csc_me_us, csc_me_batches, csc_cpu_batches;

static inline int csc_clamp(int v, int cap)
{
    int r;
    __asm__ ("max %0, %1, $0\n\tmin %0, %0, %2" : "=r" (r) : "r" (v), "r" (cap));
    return r;
}
static inline uint16_t csc_565(int yy, int rc, int gc, int bc)
{
    int r = csc_clamp((yy + rc) >> 8, 255);
    int g = csc_clamp((yy + gc) >> 8, 255);
    int b = csc_clamp((yy + bc) >> 8, 255);
    return (uint16_t) (((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3));
}
/* convert luma rows [unit*8, unit*8+8) of slot s (runs on either core);
   8-row units pack into the ME's sub-millisecond idle gaps without making a
   queued deblock job wait behind a long conversion */
static void csc_batch(SwdecCscSlot *s, int unit)
{
    if (!swdec_rgb565_destination_fits(
            s->w, s->h, s->dst_stride, s->dst_capacity)) return;
    int w = s->w;
    int j1 = (unit + 1) * 8; if (j1 > s->h) j1 = s->h;
    for (int j = unit * 8; j + 1 < j1; j += 2) {
        const uint8_t *y0 = s->y + j * s->ys;
        const uint8_t *y1 = y0 + s->ys;
        const uint8_t *u = s->u + (j >> 1) * s->uvs;
        const uint8_t *v = s->v + (j >> 1) * s->uvs;
        uint16_t *d0 = s->dst + j * s->dst_stride;
        uint16_t *d1 = d0 + s->dst_stride;
        for (int i = 0; i < w; i += 2) {
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
}
/* ME side: the deblock service reports row `batch` final. Recording only -
   converting here would sit on the critical path (it delays the next row
   parse and the queue drain the CPU waits on). The conversion itself runs
   from the drain-phase idle loop below. */
static void me_csc_row(const H264Context *h, int batch)
{
    int slot = *(volatile int *) UNC(&csc_active);
    if (slot < 0) return;
    SwdecCscSlot *s = (SwdecCscSlot *) UNC(&csc_slots[slot]);
    if (!s->recorded) {
        AVFrame *f = h->cur_pic.f;
        if (f == NULL || !swdec_rgb565_destination_fits(
                f->width, f->height, s->dst_stride, s->dst_capacity)) {
            s->rejected = 1;
            return;
        }
        s->y = f->data[0]; s->u = f->data[1]; s->v = f->data[2];
        s->ys = f->linesize[0]; s->uvs = f->linesize[1];
        s->w = f->width; s->h = f->height;
        s->recorded = 1;
    }
    /* row batch `batch` (16 luma rows) is final: its two 8-row units are ready */
    if (2 * batch + 1 > s->max_final) s->max_final = 2 * batch + 1;
}
/* ME idle work: convert ONE pending final 8-row unit (bounded: ~0.45ms, so a
   fresh deblock job is never kept waiting long). wb: write the unit back to
   RAM immediately - required for conversions in the inter-AU idle loop, where
   no slice-end writeback follows before the presenter can read the slot. */
static void me_csc_idle_one_ex(int wb)
{
    int slot = *(volatile int *) UNC(&csc_active);
    if (slot < 0) return;
    SwdecCscSlot *s = (SwdecCscSlot *) UNC(&csc_slots[slot]);
    if (!s->recorded || s->rejected
        || !swdec_rgb565_destination_fits(
            s->w, s->h, s->dst_stride, s->dst_capacity)) return;
    int mf = s->max_final;
    unsigned did = 0;
    int n = 0;
    for (int b = 0; b <= mf && n < (wb ? 3 : 1); b++) {
        if ((s->done | did) & (1u << b)) continue;
        csc_batch(s, b);
        did |= 1u << b;
        n++;
        if (!wb) break;
    }
    if (!did) return;
    /* inter-AU: write the pixels back BEFORE publishing the bits, so the CPU
       can never see done=1 for rows that are still only in the ME dcache
       (one sweep per batch of units, not per unit - the sweep also evicts the
       working set, so batching it matters) */
    if (wb) me_dcache_writeback_all();
    s->done = s->done | did;
    *(volatile unsigned *) UNC(&csc_me_batches) += (unsigned) n;
}
static void me_csc_idle_one(void) { me_csc_idle_one_ex(0); }

/* CPU side */
void swdec_me_csc_begin(int slot, void *dst_rgb565, int stride_pixels,
                        size_t capacity_bytes)
{
    if (slot < 0 || slot >= CSC_SLOTS || dst_rgb565 == NULL
        || stride_pixels <= 0 || capacity_bytes == 0) {
        *(volatile int *) UNC(&csc_active) = -1;
        return;
    }
    SwdecCscSlot *s = (SwdecCscSlot *) UNC(&csc_slots[slot]);
    s->dst = dst_rgb565;
    s->dst_stride = stride_pixels;
    s->dst_capacity = capacity_bytes;
    s->done = 0; s->recorded = 0; s->rejected = 0; s->max_final = -1;
    s->y = s->u = s->v = NULL;
    __asm__ volatile ("sync" ::: "memory");
    *(volatile int *) UNC(&csc_active) = slot;
}
void swdec_me_csc_off(void) { *(volatile int *) UNC(&csc_active) = -1; }
/* AU done: finish the batches the ME didn't reach; returns 1 if the slot holds
   a frame (writes back the CPU-converted regions for the GE), 0 if untouched */
int swdec_me_csc_close(void)
{
    int slot = *(volatile int *) UNC(&csc_active);
    if (slot < 0) return 0;
    *(volatile int *) UNC(&csc_active) = -1;
    SwdecCscSlot *s = (SwdecCscSlot *) UNC(&csc_slots[slot]);
    if (!s->recorded || s->rejected
        || !swdec_rgb565_destination_fits(
            s->w, s->h, s->dst_stride, s->dst_capacity)) return 0;
    int nb = (s->h + 7) / 8;
    if (nb > 32) nb = 32;
    int did_cpu = 0;
    for (int b = 0; b < nb; b++) {
        if (s->done & (1u << b)) continue;
        /* the CPU wrote these pixels last (tail deblock): its cache is current */
        csc_batch(s, b);
        s->done = s->done | (1u << b);
        csc_cpu_batches++;
        int j0 = b * 8, j1 = (b + 1) * 8; if (j1 > s->h) j1 = s->h;
        sceKernelDcacheWritebackRange(
            s->dst + j0 * s->dst_stride,
            (unsigned) (j1 - j0) * (unsigned) s->dst_stride * 2u);
        did_cpu = 1;
    }
    (void) did_cpu;
    return 1;
}
unsigned swdec_me_csc_stats(unsigned *cpu_batches)
{
    if (cpu_batches) *cpu_batches = csc_cpu_batches;
    return *(volatile unsigned *) UNC(&csc_me_batches);
}
void swdec_me_csc_stats_reset(void)
{
    csc_cpu_batches = 0;
    *(volatile unsigned *) UNC(&csc_me_batches) = 0;
}

int swdec_me_failed = 0;      /* set on any ME failure; the harness stops the run (product code may fall back) */
static void me_fail(const char *why)
{
    me_dispatching = 0;
    if (me_log) me_log("swdec-me: %s - stopping the ME\n", why);
    swdec_me_stop();            /* hold the ME in reset before anything else can go wrong */
    swdec_me_enabled = 0;       /* later slices (if any) decode on the CPU, sequential pipeline */
    swdec_me_run_slice = NULL;  /* and without any ME hooks */
    swdec_me_pre_publish = NULL; swdec_cpu_pre_consume = NULL; swdec_cpu_post_consume = NULL;
    swdec_me_deblock = 0; swdec_cpu_flush_row = NULL; swdec_me_post_deblock = NULL;
    swdec_me_csc_row = NULL; *(volatile int *) UNC(&csc_active) = -1;
    me_dead = 1;
    swdec_me_failed = 1;
}

/* CPU dispatcher: hand the slice to the ME, consume rows while it parses */
static int swdec_me_run_slice_impl(struct AVCodecContext *avctx, void *slv)
{
    H264SliceContext *sl = slv;
    const H264Context *h = sl->h264;
    SwdecMeBox *b = UNC(&me_box_storage);
    volatile int *pp = (volatile int *) UNC(swdec_pipe_get());
    if (me_dead) return AVERROR_EXTERNAL;
    if (me_slices < 8 && me_log) me_log("swdec-me: slice %u start (au state: prod=%d cons=%d)\n", me_slices, pp[0], pp[1]);
    swdec_pipe_slice_begin();
    me_dispatching = 1;
    unsigned want = b->done + 1;
    sceKernelDcacheWritebackInvalidateAll();      /* everything the ME must see (slice header state, ring memset, ...) */
    b->avctx = (unsigned) avctx; b->sl = (unsigned) sl; b->token = ~(unsigned) avctx;
    __asm__ volatile ("sync" ::: "memory");
    b->cmd = SWDEC_ME_CMD_SLICE;
    unsigned long long t0 = (unsigned long long) sceKernelGetSystemTimeWide();
    unsigned spins = 0;
    int tail_done = 0;
    for (;;) {
        swdec_me_beat++;
        swdec_pipe_consume_all((void *) h, sl);
#ifdef SWDEC_ME_DEBLOCK
        if (!tail_done && swdec_pipe_parse_done()) {   /* all rows parsed: recon the rest, push the held-back row */
            swdec_pipe_consume_all((void *) h, sl);
            swdec_pipe_deblock_flush((const void *) h);
            swdec_pipe_set_tail_pushed();
            tail_done = 1;
        }
#else
        (void) tail_done;
#endif
        if (swdec_pipe_bad) {
            if (me_log) {
                me_log("swdec-me: BAD kind=%d at slice %u: info={%d %d %d %d %d %d} prod=%d cons=%d\n", swdec_pipe_bad, me_slices,
                       swdec_pipe_bad_info[0], swdec_pipe_bad_info[1], swdec_pipe_bad_info[2], swdec_pipe_bad_info[3],
                       swdec_pipe_bad_info[4], swdec_pipe_bad_info[5], pp[0], pp[1]);
                me_log("swdec-me: rec0={%d %d %d %d %d} rec1={%d %d %d %d %d}\n", pp[3], pp[4], pp[5], pp[6], pp[7], pp[8], pp[9], pp[10], pp[11], pp[12]);
            }
            me_fail("inconsistent row record/job");
            return AVERROR_EXTERNAL;
        }
        if (b->done == want) break;
        if ((++spins & 1023u) == 0) {
            unsigned c, e;
            unsigned el = (unsigned) ((unsigned long long) sceKernelGetSystemTimeWide() - t0);
            if (swdec_me_fault(&c, &e)) {
                if (me_log) me_log("swdec-me: ME FAULT cause=%08x epc=%08x at slice %u (prod=%d cons=%d)\n", c, e, me_slices, pp[0], pp[1]);
                me_fail("ME exception");
                return AVERROR_EXTERNAL;
            }
            if ((spins & (65536u - 1u)) == 0 && me_log && me_slices < 2)
                me_log("swdec-me: waiting: slice %u t=%u us prod=%d cons=%d done=%u want=%u hb=%u\n", me_slices, el, pp[0], pp[1], b->done, want, b->heartbeat);
            if (el > 2000000u) {
                if (me_log) me_log("swdec-me: ME timeout at slice %u: prod=%d cons=%d done=%u want=%u hb=%u\n", me_slices, pp[0], pp[1], b->done, want, b->heartbeat);
                me_fail("ME timeout");
                return AVERROR_EXTERNAL;
            }
        }
    }
    swdec_pipe_consume_all((void *) h, sl);
    unsigned el = (unsigned) ((unsigned long long) sceKernelGetSystemTimeWide() - t0);
    me_wait_us += el;
    me_slices++;
    if (me_slices <= 3 && me_log) me_log("swdec-me: slice %u done in %u us, status %d, prod=%d cons=%d\n", me_slices, el, (int) b->status, pp[0], pp[1]);
    /* the ME modified sl (parser state) and h (tables, ER, ...); this core holds no dirty
       lines of them (it only wrote pixels, rsl and its own stack meanwhile) */
    sceKernelDcacheWritebackInvalidateRange(sl, sizeof *sl);
    sceKernelDcacheWritebackInvalidateRange((void *) h, sizeof *h);
    me_dispatching = 0;
    return (int) b->status;
}

/* Watchdog: an independent thread that keeps liveness on this core. If the
   dispatcher's heartbeat stalls for > 6 s the watchdog logs the shared state
   and puts the ME into reset - whatever else is stuck, the device stays
   recoverable over PSPLink (thterm + modunld, no power cycle). */
static int watchdog_thread(SceSize args, void *argp)
{
    (void) args; (void) argp;
    unsigned last = 0, still = 0;
    while (watchdog_on) {
        sceKernelDelayThread(500 * 1000);
        unsigned now = swdec_me_beat;
        if (!watchdog_on) break;
        still = (now == last) ? still + 1 : 0;
        last = now;
        {
            static unsigned tick;
            if ((++tick % 20u) == 0 && me_log)   /* liveness every ~10 s: silence is never ambiguous */
                me_log("swdec-live: beat=%u slices=%u\n", now, me_slices);
        }
        if (still == 12 && me_attached && !me_dead && me_dispatching) {   /* ~6 s without progress during a dispatch */
            SwdecMeBox *b = UNC(&me_box_storage);
            volatile int *pp = (volatile int *) UNC(swdec_pipe_get());
            unsigned cse, epc;
            int f = swdec_me_fault(&cse, &epc);
            if (me_log) me_log("swdec-me: WATCHDOG: stalled at slice %u: box cmd=%08x done=%u status=%d hb=%u pipe prod=%d cons=%d fault=%d cause=%08x epc=%08x\n",
                               me_slices, b->cmd, b->done, (int) b->status, b->heartbeat, pp[0], pp[1], f, cse, epc);
            me_fail("watchdog stall");
        }
    }
    return 0;
}

/* ---------------- audio on the parser core (aux idle work) ---------------- */
/* The CPU enqueues complete ADTS frames; the ME decodes them with aac_fixed
   between video rows and reports a CRC32 of the output planes per frame. */
#define AQ 128
typedef struct {
    volatile unsigned aprod, acons;
    struct { volatile unsigned ptr, len; } in[AQ];
    struct { volatile unsigned crc, samples; } out[AQ];
    volatile unsigned decode_us;                  /* accumulated ME time in the audio decoder */
    volatile unsigned errors;
} SwdecAudioQ;
static SwdecAudioQ audioq_storage __attribute__((aligned(64)));
static swdec_audio *me_audio;                     /* opened by the CPU, used only by the ME */
static unsigned me_acrc_tab[256];
/* playback ring: interleaved S16 stereo, one AAC frame per slot; the ME writes
   through the uncached alias so the CPU audio thread can read a slot the moment
   acons covers it (no writeback ordering to wait for) */
static short me_pcm_ring[AQ][2048] __attribute__((aligned(64)));
static volatile int me_poison_flag __attribute__((aligned(64)));

static unsigned me_acrc(unsigned crc, const unsigned char *p, unsigned n)
{
    crc = ~crc;
    while (n--) crc = me_acrc_tab[(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

/* runs on the ME from the pipeline's idle hooks */
static void me_audio_service(void)
{
    SwdecAudioQ *q = UNC(&audioq_storage);
    int decoded_any = 0;
    {   /* fault-injection test: fault here, with audio state live */
        volatile int *poison = UNC(&me_poison_flag);
        if (*poison) { *poison = 0; *(volatile int *) 5 = 0x0DEAD; }
    }
    while (q->acons < q->aprod) {
        decoded_any = 1;
        unsigned i = q->acons & (AQ - 1);
        swdec_audio_frame out;
        int r = swdec_audio_decode(me_audio, (const unsigned char *) q->in[i].ptr, q->in[i].len, &out);
        if (r == 1 && !swdec_audio_channels_admitted(out.channels)) r = -5;
        if (r == 1) {
            unsigned crc = 0;
            for (int ch = 0; ch < out.channels; ch++)
                crc = me_acrc(crc, (const unsigned char *) out.plane[ch], (unsigned) out.nb_samples * 4);
            {   /* interleave S16 stereo into the uncached playback ring (mono duplicated) */
                unsigned *dst = (unsigned *) UNC(me_pcm_ring[i]);
                const int32_t *l = out.plane[0];
                const int32_t *r32 = out.channels > 1 ? out.plane[1] : out.plane[0];
                int ns = out.nb_samples > 1024 ? 1024 : out.nb_samples;
                for (int s = 0; s < ns; s++) {
                    unsigned lo = (unsigned) (l[s] >> 16) & 0xFFFFu;
                    unsigned hi = (unsigned) (r32[s] >> 16) & 0xFFFFu;
                    dst[s] = lo | (hi << 16);
                }
                for (int s = ns; s < 1024; s++) dst[s] = 0;
            }
            q->out[i].crc = crc; q->out[i].samples = (unsigned) out.nb_samples;
        } else {
            q->out[i].crc = 0xFFFFFFFFu; q->out[i].samples = (unsigned) r;   /* error code for diagnosis */
            q->errors = q->errors + 1;
        }
        __asm__ volatile ("sync" ::: "memory");
        q->acons = q->acons + 1;
    }
    /* the decoder's internal state (arena metadata, frame refs) must reach RAM:
       a reset discards dirty ME lines, and a rebooted ME would otherwise resume
       from a half-old aux arena and unref stale buffers (AdEL on first decode) */
    if (decoded_any) me_dcache_writeback_all();
}

/* CPU side */
int swdec_me_audio_setup(void *arena_unused)
{
    (void) arena_unused;
    if (me_audio) {
        *(void (**)(void)) UNC(&swdec_me_aux) = me_audio_service;
        return 0;   /* reinstall the hook (cleared by detach/me_fail) */
    }
    for (unsigned i = 0; i < 256; i++) {
        unsigned cc = i;
        for (int k = 0; k < 8; k++) cc = (cc & 1u) ? 0xEDB88320u ^ (cc >> 1) : cc >> 1;
        me_acrc_tab[i] = cc;
    }
    /* the audio decoder lives in its own arena: the main one is not
       cross-core-safe (the ME allocates per audio frame while the CPU
       allocates for video) */
    swdec_arena_aux_sp_range(me_stack, me_stack + sizeof me_stack);
    swdec_arena_route_aux(1);                    /* setup runs on the CPU: route its allocations to the aux arena */
    me_audio = swdec_audio_open(NULL, 0);
    swdec_arena_route_aux(0);
    if (!me_audio) return -1;
    SwdecAudioQ *q = UNC(&audioq_storage);
    memset((void *) q, 0, sizeof *q);
    sceKernelDcacheWritebackInvalidateRange(
        swdec_arena_aux_base(), (unsigned) swdec_arena_aux_bytes());
    *(void (**)(void)) UNC(&swdec_me_aux) = me_audio_service;
    return 0;
}
/* diagnosis: decode one frame on the CPU through a second context in the same
   aux arena (route_aux), to separate ME-execution effects from table state */
static swdec_audio *cpu_audio;
int swdec_me_audio_cpu_decode(const void *adts, unsigned len, unsigned *crc)
{
    swdec_arena_route_aux(1);
    if (!cpu_audio) cpu_audio = swdec_audio_open(NULL, 0);
    swdec_audio_frame out;
    int r = cpu_audio ? swdec_audio_decode(cpu_audio, adts, len, &out) : -1;
    swdec_arena_route_aux(0);
    if (r == 1 && !swdec_audio_channels_admitted(out.channels)) r = -5;
    if (r == 1) {
        unsigned cc = 0;
        for (int ch = 0; ch < out.channels; ch++)
            cc = me_acrc(cc, (const unsigned char *) out.plane[ch], (unsigned) out.nb_samples * 4);
        *crc = cc;
    }
    return r;
}

/* diagnosis: fresh context, decode up to the given frame, return samples 256..263 of it */
int swdec_me_audio_cpu_samples(const void *adts, unsigned len, int *out, int n);
int swdec_me_audio_cpu_samples(const void *adts, unsigned len, int *out, int n)
{
    swdec_audio_frame f;
    int r;
    swdec_arena_route_aux(1);
    r = swdec_audio_decode(cpu_audio, adts, len, &f);
    swdec_arena_route_aux(0);
    if (r == 1)
        for (int i = 0; i < n; i++) out[i] = f.plane[0][256 + i];
    return r;
}

int swdec_me_audio_submit(const void *adts, unsigned len)
{
    SwdecAudioQ *q = UNC(&audioq_storage);
    if (q->aprod - q->acons >= AQ) return -1;    /* queue full */
    unsigned i = q->aprod & (AQ - 1);
    q->in[i].ptr = (unsigned) adts; q->in[i].len = len;
    __asm__ volatile ("sync" ::: "memory");
    q->aprod = q->aprod + 1;
    return 0;
}
int swdec_me_audio_poll(unsigned upto, unsigned *crc, unsigned *samples)
{
    SwdecAudioQ *q = UNC(&audioq_storage);
    if (q->acons <= upto) return 0;              /* not decoded yet */
    unsigned i = upto & (AQ - 1);
    *crc = q->out[i].crc; *samples = q->out[i].samples;
    return 1;
}
unsigned swdec_me_audio_done(void) { return ((SwdecAudioQ *) UNC(&audioq_storage))->acons; }
static int swdec_me_audio_wait_idle(unsigned timeout_us)
{
    SwdecAudioQ *q = UNC(&audioq_storage);
    unsigned started = sceKernelGetSystemTimeLow();
    while (q->acons != q->aprod) {
        if ((unsigned) (sceKernelGetSystemTimeLow() - started) >= timeout_us)
            return -1;
        sceKernelDelayThread(1000);
    }
    return 0;
}

int swdec_me_audio_reset(unsigned timeout_us)
{
    SwdecAudioQ *q = UNC(&audioq_storage);
    if (swdec_me_audio_wait_idle(timeout_us) != 0) return -1;
    q->aprod = 0; q->acons = 0; q->errors = 0;
    return 0;
}

int swdec_me_audio_shutdown(unsigned timeout_us)
{
    SwdecAudioQ *q = UNC(&audioq_storage);
    if (me_audio == NULL) {
        *(void (**)(void)) UNC(&swdec_me_aux) = NULL;
        memset((void *) q, 0, sizeof *q);
        swdec_arena_unbind_aux();
        return 0;
    }
    if (swdec_me_audio_wait_idle(timeout_us) != 0) return -1;
    *(void (**)(void)) UNC(&swdec_me_aux) = NULL;
    /* The ME publishes queue completion before its final whole-cache
       writeback. Allow that writeback to finish, then invalidate the CPU's
       view before walking the decoder structures during close. */
    sceKernelDelayThread(5000);
    sceKernelDcacheInvalidateRange(
        swdec_arena_aux_base(), (unsigned) swdec_arena_aux_bytes());
    swdec_arena_route_aux(1);
    swdec_audio_close(me_audio);
    me_audio = NULL;
    swdec_arena_route_aux(0);
    memset((void *) q, 0, sizeof *q);
    swdec_arena_unbind_aux();
    return 0;
}
unsigned swdec_me_audio_us(void) { return ((SwdecAudioQ *) UNC(&audioq_storage))->decode_us; }
unsigned swdec_me_audio_errors(void) { return ((SwdecAudioQ *) UNC(&audioq_storage))->errors; }
const short *swdec_me_audio_pcm(unsigned idx) { return (const short *) UNC(me_pcm_ring[idx & (AQ - 1)]); }
void swdec_me_poison_arm(void) { *(volatile int *) UNC(&me_poison_flag) = 1; }
void swdec_me_mark_dead(void) { me_dead = 1; swdec_me_enabled = 0; }

int swdec_me_attach_path(
    const char *helper_path,
    void (*log)(const char *fmt, ...))
{
    me_log = log;
    if (me_attached) return 0;
    if (helper_path == NULL || helper_path[0] == '\0') return -4;
    {
        unsigned mine = (unsigned) sizeof(H264SliceContext) * 65536u + (unsigned) offsetof(H264SliceContext, swdec_ring) + (unsigned) sizeof(H264Context) % 65536u;
        if (mine != swdec_layout_key()) { if (log) log("swdec-me: struct layout mismatch with the library (%08x vs %08x) - refusing\n", mine, swdec_layout_key()); return -3; }
    }
    me_mod = pspSdkLoadStartModule(
        helper_path, PSP_MEMORY_PARTITION_KERNEL);
    if (me_mod < 0) {
        /* likely already resident from a previous run (the helper is kept loaded:
           kernel-module unload cycling corrupts kernel lists under PSPLink) */
        if (log) log("swdec-me: meload load returned %08x (assuming resident)\n", (unsigned) me_mod);
        me_mod = -1;
    }
    SwdecMeBox *b = UNC(&me_box_storage);
    memset((void *) b, 0, sizeof *b);
    swdec_pipe_set(UNC(swdec_pipe_get()));
    swdec_pipe_reset();
    swdec_me_pre_publish   = me_pre_publish;
    swdec_cpu_pre_consume  = cpu_pre_consume;
    swdec_cpu_post_consume = cpu_post_consume;
    swdec_me_run_slice     = swdec_me_run_slice_impl;
#ifdef SWDEC_ME_DEBLOCK
    swdec_me_deblock       = 1;
    swdec_cpu_flush_row    = cpu_flush_row;
    swdec_me_post_deblock  = me_post_deblock;
    swdec_me_csc_row       = me_csc_row;
#endif
    unsigned stack_top = (((unsigned) me_stack + sizeof me_stack) & 0x1FFFFFFFu) | 0x80000000u;
    sceKernelDcacheWritebackInvalidateAll();
    int r = swdec_me_boot((unsigned) swdec_me_worker, (unsigned) b, stack_top);
    sceKernelDelayThread(20000);
    if (r != 0 || b->heartbeat == 0) { if (log) log("swdec-me: ME boot failed r=%d heartbeat=%u\n", r, b->heartbeat); return -2; }
    swdec_me_enabled = 1;
    me_attached = 1;
    watchdog_on = 1;
    watchdog_th = sceKernelCreateThread("swdec_me_watchdog", watchdog_thread, 0x10, 0x4000, 0, NULL);   /* higher prio than main */
    if (watchdog_th >= 0) sceKernelStartThread(watchdog_th, 0, NULL);
    if (log) log("swdec-me: attached, ME heartbeat %u, worker %08x, deblock-on-ME %d\n", b->heartbeat, (unsigned) swdec_me_worker, swdec_me_deblock);
    return 0;
}

int swdec_me_attach(void (*log)(const char *fmt, ...))
{
    return swdec_me_attach_path("host0:/swdec-meload.prx", log);
}

/* Resident runner: bring a failed ME split back to life without reloading the
   module. The kernel helper stays resident; we re-boot the ME and reinstall
   the hooks. */
int swdec_me_recover(void)
{
    if (!me_attached) return swdec_me_attach(me_log);
    if (!me_dead && swdec_me_enabled) return 0;
    SwdecMeBox *b = UNC(&me_box_storage);
    memset((void *) b, 0, sizeof *b);
    swdec_pipe_set(UNC(swdec_pipe_get()));
    swdec_pipe_reset();
    swdec_me_pre_publish   = me_pre_publish;
    swdec_cpu_pre_consume  = cpu_pre_consume;
    swdec_cpu_post_consume = cpu_post_consume;
    swdec_me_run_slice     = swdec_me_run_slice_impl;
#ifdef SWDEC_ME_DEBLOCK
    swdec_me_deblock       = 1;
    swdec_cpu_flush_row    = cpu_flush_row;
    swdec_me_post_deblock  = me_post_deblock;
    swdec_me_csc_row       = me_csc_row;
#endif
    unsigned stack_top = (((unsigned) me_stack + sizeof me_stack) & 0x1FFFFFFFu) | 0x80000000u;
    sceKernelDcacheWritebackInvalidateAll();
    int r = swdec_me_boot((unsigned) swdec_me_worker, (unsigned) b, stack_top);
    sceKernelDelayThread(20000);
    if (r != 0 || b->heartbeat == 0) { if (me_log) me_log("swdec-me: recover boot failed r=%d hb=%u\n", r, b->heartbeat); return -1; }
    me_dead = 0;
    swdec_me_failed = 0;
    swdec_me_enabled = 1;
    if (me_log) me_log("swdec-me: recovered (ME rebooted, hooks reinstalled)\n");
    return 0;
}

void swdec_me_detach(void)
{
    if (!me_attached) return;
    watchdog_on = 0;
    if (watchdog_th >= 0) {           /* the thread must be gone before this module unloads */
        (void) psp_thread_wait_end_bounded(watchdog_th, 2000000u);
        sceKernelDeleteThread(watchdog_th);
        watchdog_th = -1;
    }
    swdec_me_enabled = 0;
    swdec_me_run_slice = NULL;
    swdec_me_pre_publish = NULL; swdec_cpu_pre_consume = NULL; swdec_cpu_post_consume = NULL;
    swdec_me_deblock = 0; swdec_cpu_flush_row = NULL; swdec_me_post_deblock = NULL;
    swdec_me_csc_row = NULL; *(volatile int *) UNC(&csc_active) = -1;
    {   /* quiesce audio before the reset: wait out any in-flight decode, then
           give the ME time to finish its post-batch cache writeback */
        SwdecAudioQ *q = UNC(&audioq_storage);
        unsigned spins = 0;
        while (q->acons != q->aprod && spins++ < 3000) sceKernelDelayThread(1000);
        sceKernelDelayThread(5000);
    }
    swdec_pipe_set(NULL);
    swdec_me_stop();
    if (me_log) me_log("swdec-me: detached after %u slices, %u ms waited\n", me_slices, me_wait_us / 1000u);
    /* the kernel helper stays resident deliberately (3 KB): unload cycling
       under PSPLink corrupts kernel lists; it is idempotent across runs */
    me_mod = -1;
    me_attached = 0;
}
