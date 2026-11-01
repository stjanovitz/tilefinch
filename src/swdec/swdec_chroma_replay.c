/* Validation builds only (SWDEC_DIAGNOSTICS): qualification replay for
 * running chroma prediction on the Media Engine.
 *
 * One recorded macroblock row at a time (libavcodec/swdec_chroma_rec.h)
 * goes through
 *   - the main core's own kernels (the recorder's check, and their cost),
 *   - private scalar kernels on the Media Engine (no VFPU there): the row is
 *     posted through a one-line mailbox and run from the ME's idle hook
 *     (swdec_me_aux, wrapped here; its own work runs first), with the full
 *     data-cache sweep the row handoff uses before and after it,
 * and both are compared byte for byte with the prediction the decoder
 * produced. The main core then invalidates the ME's result and copies it
 * out at picture stride, as the integration would. Nothing here touches the
 * decoded pictures; it only reads references and its own buffers. Without
 * the ME (the bench) the scalar kernels run on the calling core. */
#include "swdec_chroma_replay.h"

#if SWDEC_DIAG_ENABLED
#include <pspkernel.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "libavcodec/swdec_chroma_rec.h"
#include "libavcodec/swdec_pipe.h"

#define UNCP(p) ((void *) ((uintptr_t) (p) | 0x40000000u))

/* --------------------------- scalar kernels --------------------------- */
/* Same arithmetic as FFmpeg's C references (h264chroma_template.c,
   h264dsp_template.c) for 8-bit pixels, on 8x8 blocks with stride 8. */
static inline int cr_clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline uint8_t cr_clip8(int v) { return (uint8_t) (v < 0 ? 0 : v > 255 ? 255 : v); }

static void cr_mc(uint8_t *d, const SwdecChromaOp *o, const uint8_t *ref,
                  int s, int pw, int ph)
{
    const int A = (8 - o->fx) * (8 - o->fy), B = o->fx * (8 - o->fy);
    const int C = (8 - o->fx) * o->fy, D = o->fx * o->fy;
    const int inside = o->cx >= 0 && o->cy >= 0
        && o->cx + o->w + 1 <= pw && o->cy + o->h + 1 <= ph;
    for (int j = 0; j < o->h; j++, d += 8) {
        if (inside) {
            const uint8_t *p = ref + (o->cy + j) * s + o->cx;
            for (int i = 0; i < o->w; i++) {
                int v = (A * p[i] + B * p[i + 1] + C * p[i + s] + D * p[i + s + 1] + 32) >> 6;
                d[i] = (uint8_t) (o->avg ? (d[i] + v + 1) >> 1 : v);
            }
        } else {
            const int y0 = cr_clamp(o->cy + j, 0, ph - 1), y1 = cr_clamp(o->cy + j + 1, 0, ph - 1);
            for (int i = 0; i < o->w; i++) {
                const int x0 = cr_clamp(o->cx + i, 0, pw - 1), x1 = cr_clamp(o->cx + i + 1, 0, pw - 1);
                int v = (A * ref[y0 * s + x0] + B * ref[y0 * s + x1]
                         + C * ref[y1 * s + x0] + D * ref[y1 * s + x1] + 32) >> 6;
                d[i] = (uint8_t) (o->avg ? (d[i] + v + 1) >> 1 : v);
            }
        }
    }
}

static void cr_weight(uint8_t *d, const SwdecChromaOp *o, int p)
{
    int offset = (int) ((unsigned) o->off[p] << o->log2);
    if (o->log2) offset += 1 << (o->log2 - 1);
    for (int j = 0; j < o->h; j++, d += 8)
        for (int i = 0; i < o->w; i++)
            d[i] = cr_clip8((d[i] * o->wa[p] + offset) >> o->log2);
}

static void cr_biweight(uint8_t *d, const uint8_t *src, const SwdecChromaOp *o, int p)
{
    const int offset = (int) ((unsigned) ((o->off[p] + 1) | 1) << o->log2);
    for (int j = 0; j < o->h; j++, d += 8, src += 8)
        for (int i = 0; i < o->w; i++)
            d[i] = cr_clip8((src[i] * o->wb[p] + d[i] * o->wa[p] + offset) >> (o->log2 + 1));
}

static void cr_exec(const SwdecChromaRec *r, uint8_t (*out)[2][64])
{
    uint8_t buf[2][2][64];   /* [prediction / bi-prediction scratch][plane] */
    int k = 0;
    for (int mb = 0; mb < r->mbs; mb++) {
        for (unsigned i = 0; i < sizeof(buf); i++) ((uint8_t *) buf)[i] = 0;
        for (; k < r->ops && r->op[k].mb == mb; k++) {
            const SwdecChromaOp *o = &r->op[k];
            for (int p = 0; p < 2; p++) {
                uint8_t *d = buf[o->buf][p] + o->y * 8 + o->x;
                if (o->type == SWDEC_CR_MC)
                    cr_mc(d, o, o->ref[p], r->ref_stride, r->pic_w, r->pic_h);
                else if (o->type == SWDEC_CR_WEIGHT)
                    cr_weight(d, o, p);
                else
                    cr_biweight(d, buf[SWDEC_CR_TMP][p] + o->ty * 8 + o->tx, o, p);
            }
        }
        for (int i = 0; i < 64; i++) { out[mb][0][i] = buf[0][0][i]; out[mb][1][i] = buf[0][1][i]; }
    }
}

/* ------------------------------ mailbox ------------------------------- */
enum { BOX_IDLE, BOX_POSTED, BOX_RUNNING, BOX_DONE };
enum { JOB_ROW = 1, JOB_CALIBRATE = 2 };
typedef struct {
    volatile unsigned state, kind, stop, now;
    volatile unsigned sweep_in, exec, sweep_out;
    volatile unsigned pad[9];
} ReplayCtl;
_Static_assert(sizeof(ReplayCtl) == 64, "the mailbox owns one cache line");
static ReplayCtl ctl_storage __attribute__((aligned(64)));
#define CTL ((volatile ReplayCtl *) UNCP(&ctl_storage))
static void (*orig_aux_storage[16])(void) __attribute__((aligned(64)));   /* [0] used; own line */
#define ORIG_AUX (*(void (*volatile *)(void)) UNCP(&orig_aux_storage[0]))
static SwdecChromaRec box_rec __attribute__((aligned(64)));            /* main core writes */
static uint8_t box_out[SWDEC_CR_MAX_MBS][2][64] __attribute__((aligned(64)));   /* ME writes */
static uint8_t cpu_out[SWDEC_CR_MAX_MBS][2][64] __attribute__((aligned(64)));
static uint8_t copy_target[2][8 * 512] __attribute__((aligned(64)));

/* Media Engine side: called through swdec_me_aux, never on the main core */
static inline unsigned me_count(void)
{
    unsigned c;
    __asm__ volatile ("mfc0 %0, $9" : "=r" (c));
    return c;
}
static void me_sweep(void)
{
    /* the row handoff's own data-cache sweep (swdec_me.c) */
    __asm__ volatile ("sync\n\t");
    for (unsigned a = 0; a < 65536; a += 64)
        __asm__ volatile ("cache 0x14, 0(%0)" : : "r"(0x80000000u + a));
    __asm__ volatile ("sync\n\t");
}
static void replay_me_aux(void)
{
    void (*orig)(void) = ORIG_AUX;
    if (orig) orig();
    volatile ReplayCtl *c = CTL;
    if (c->state != BOX_POSTED) return;
    c->state = BOX_RUNNING;
    if (c->kind == JOB_CALIBRATE) {
        while (!c->stop) c->now = me_count();
        c->state = BOX_DONE;
        return;
    }
    unsigned t0 = me_count();
    me_sweep();                       /* nothing stale: the record and references */
    unsigned t1 = me_count();
    cr_exec(&box_rec, box_out);
    unsigned t2 = me_count();
    me_sweep();                       /* the results reach RAM */
    unsigned t3 = me_count();
    c->sweep_in = t1 - t0; c->exec = t2 - t1; c->sweep_out = t3 - t2;
    c->state = BOX_DONE;
}

/* ----------------------------- main core ------------------------------ */
static struct {
    int on_me, recording, row_phase, cal_state;
    unsigned ticks_per_mhz;                   /* ME Count ticks per microsecond x1000 */
    unsigned rows, mbs, ops, unsupported, busy, cpu_bad_mbs, me_bad_mbs, me_rows;
    uint64_t cpu_us, scalar_us, collect_us, me_exec, me_sweep_in, me_sweep_out;
    int posted_mbs, posted_stride;
} rp;

static unsigned rp_now(void) { return sceKernelGetSystemTimeLow(); }

/* off unless asked for (validation_swdec_reference bit 128): it costs the
   main core time that would skew a device A/B */
int swdec_chroma_replay_enabled;

void swdec_chroma_replay_reset(int on_me)
{
    memset(&rp, 0, sizeof(rp));
    rp.on_me = on_me && swdec_chroma_replay_enabled;
    swdec_chroma_rec_on = 0;
    if (CTL->state == BOX_IDLE || CTL->state == BOX_DONE) CTL->state = BOX_IDLE;
}

void swdec_chroma_replay_install(void)
{
    if (!rp.on_me || !swdec_chroma_replay_enabled) return;
    void (**hook)(void) = (void (**)(void)) UNCP(&swdec_me_aux);
    void (*cur)(void) = *(void (*volatile *)(void)) hook;
    if (cur != replay_me_aux) {
        ORIG_AUX = cur;
        *(void (*volatile *)(void)) hook = replay_me_aux;
    }
}

static unsigned rp_mismatched_mbs(uint8_t (*a)[2][64], const SwdecChromaRec *r)
{
    unsigned bad = 0;
    for (int mb = 0; mb < r->mbs; mb++)
        if (memcmp(a[mb], r->expect[mb], sizeof(a[mb])) != 0) bad++;
    return bad;
}

/* the ME finished a posted row: take its results as the integration would */
static void rp_collect(void)
{
    volatile ReplayCtl *c = CTL;
    if (c->state != BOX_DONE) return;
    if (c->kind == JOB_ROW) {
        unsigned t0 = rp_now();
        sceKernelDcacheInvalidateRange(box_out, (unsigned) rp.posted_mbs * 128u);
        const int s = rp.posted_stride;
        for (int mb = 0; mb < rp.posted_mbs; mb++)
            for (int p = 0; p < 2; p++)
                for (int y = 0; y < 8; y++)
                    memcpy(copy_target[p] + y * s + (8 * mb) % s, box_out[mb][p] + 8 * y, 8);
        rp.collect_us += rp_now() - t0;
        rp.me_bad_mbs += rp_mismatched_mbs(box_out, &box_rec);
        rp.me_exec += c->exec; rp.me_sweep_in += c->sweep_in; rp.me_sweep_out += c->sweep_out;
        rp.me_rows++;
    }
    c->state = BOX_IDLE;
}

void swdec_chroma_replay_row_begin(void)
{
    if (!swdec_chroma_replay_enabled) return;
    volatile ReplayCtl *c = CTL;
    rp_collect();
    if (rp.on_me) {
        /* once per stream, calibrate the ME's Count against the system
           clock: the ME publishes Count in a spin while this core waits
           1 ms. Synchronous, so the ME never spins while this core waits
           on it for a row; `stop` is set on every exit, so a late pickup
           returns at once. */
        if (rp.cal_state == 0 && c->state == BOX_IDLE) {
            rp.cal_state = 1;
            c->stop = 0; c->kind = JOB_CALIBRATE; c->state = BOX_POSTED;
            unsigned t = rp_now();
            while (c->state == BOX_POSTED && rp_now() - t < 5000) { }
            if (c->state == BOX_RUNNING) {
                unsigned t0 = rp_now(), c0 = c->now;
                while (rp_now() - t0 < 1000) { }
                unsigned dt = rp_now() - t0, dc = c->now - c0;
                rp.ticks_per_mhz = (unsigned) ((uint64_t) dc * 1000u / dt);
            }
            c->stop = 1;
            t = rp_now();
            while (c->state == BOX_RUNNING && rp_now() - t < 5000) { }
            if (c->state == BOX_DONE) c->state = BOX_IDLE;
            return;
        }
        if (c->state != BOX_IDLE) { rp.busy++; return; }
    }
    /* one row in four while the mailbox is free */
    if ((rp.row_phase++ & 3) != 0) return;
    SwdecChromaRec *r = &swdec_chroma_rec;
    r->ops = r->mbs = r->overflow = 0;
    swdec_chroma_rec_on = 1;
    rp.recording = 1;
}

void swdec_chroma_replay_row_end(void)
{
    if (!rp.recording) return;
    rp.recording = 0;
    swdec_chroma_rec_on = 0;
    const SwdecChromaRec *r = &swdec_chroma_rec;
    if (r->overflow) { rp.unsupported++; return; }
    if (r->mbs == 0) return;
    rp.rows++; rp.mbs += r->mbs; rp.ops += r->ops;
    unsigned t0 = rp_now();
    swdec_chroma_rec_cpu_replay(r, cpu_out);
    rp.cpu_us += rp_now() - t0;
    rp.cpu_bad_mbs += rp_mismatched_mbs(cpu_out, r);
    /* the record goes to the mailbox: header, the operations used, the
       expected predictions (compared on the main core only) */
    memcpy(&box_rec, r, offsetof(SwdecChromaRec, op));
    memcpy(box_rec.op, r->op, (size_t) r->ops * sizeof(r->op[0]));
    memcpy(box_rec.expect, r->expect, (size_t) r->mbs * sizeof(r->expect[0]));
    rp.posted_mbs = r->mbs;
    rp.posted_stride = r->ref_stride;
    if (rp.on_me) {
        sceKernelDcacheWritebackRange(&box_rec, sizeof(box_rec));   /* the ME reads RAM */
        CTL->kind = JOB_ROW;
        CTL->state = BOX_POSTED;
    } else {
        unsigned t1 = rp_now();
        cr_exec(&box_rec, box_out);
        rp.scalar_us += rp_now() - t1;
        rp.me_bad_mbs += rp_mismatched_mbs(box_out, &box_rec);
        rp.me_rows++;
    }
}

void swdec_chroma_replay_report(void (*log)(const char *format, ...))
{
    if (!swdec_chroma_replay_enabled) return;
    swdec_chroma_rec_on = 0;
    /* give a posted row a moment to finish; an unfinished one keeps its
       (static) buffers, nothing is reused */
    for (int i = 0; i < 20 && rp.on_me && CTL->state != BOX_IDLE
         && CTL->state != BOX_DONE; i++)
        sceKernelDelayThread(1000);
    rp_collect();
    CTL->stop = 1;
    if (log == NULL) return;
    const unsigned rows = rp.rows ? rp.rows : 1, me_rows = rp.me_rows ? rp.me_rows : 1;
    const unsigned tpm = rp.ticks_per_mhz;
#define ME_US(t) (tpm ? (unsigned) ((uint64_t) (t) * 1000u / tpm / me_rows) : 0u)
    log("swdec-chroma-replay: where=%s rows=%u me-rows=%u mbs=%u ops=%u "
        "unsupported=%u busy=%u cpu-mismatched-mbs=%u me-mismatched-mbs=%u "
        "per-row-us cpu-kernels=%u %s=%u me-sweep-in=%u me-sweep-out=%u "
        "cpu-collect=%u me-ticks-per-us-x1000=%u mbs-per-row=%u\n",
        rp.on_me ? "me" : "cpu", rp.rows, rp.me_rows, rp.mbs, rp.ops,
        rp.unsupported, rp.busy, rp.cpu_bad_mbs, rp.me_bad_mbs,
        (unsigned) (rp.cpu_us / rows), rp.on_me ? "me-exec" : "scalar-on-cpu",
        rp.on_me ? ME_US(rp.me_exec) : (unsigned) (rp.scalar_us / me_rows),
        ME_US(rp.me_sweep_in), ME_US(rp.me_sweep_out),
        (unsigned) (rp.collect_us / me_rows), tpm, rp.mbs / rows);
#undef ME_US
}
#endif
