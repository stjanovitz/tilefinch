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
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include "swdec_me.h"
#include "swdec_diag.h"
#include "psp_thread_contract.h"
#include "swdec.h"
#include "swdec_arena.h"
#include "swdec_bounds.h"
#include "swdec_csc.h"
#include "swdec_csc_claim.h"
#include "swdec_csc_mask.h"
#include "swdec_occupancy.h"
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
_Static_assert(sizeof(SwdecMeBox) == 64u, "the ME mailbox must own its cache line");
/* The ME runs its worker loop for as long as it is attached, so its stack has
   to live in the module image. The deepest ME-reachable frames measured from
   the linked component are a few KiB each (PS decorrelation 8.6 KiB, AAC
   output configuration 4.5 KiB, H.264 qpel 4.3 KiB, av_log 6 KiB + 5 KiB); 128
   KiB leaves several times the deepest plausible chain. Detach logs the
   measured high-water mark so device runs can confirm it. */
#ifndef SWDEC_ME_STACK_KB
#define SWDEC_ME_STACK_KB 128u
#endif
#define ME_STACK_BYTES (SWDEC_ME_STACK_KB * 1024u)
#define ME_STACK_FILL 0x57AC57ACu
static unsigned char me_stack[ME_STACK_BYTES] __attribute__((aligned(64)));
static SceUID me_mod = -1;
static int me_attached;
static void (*me_log)(const char *fmt, ...);
static unsigned me_slices, me_wait_us;
static volatile unsigned swdec_me_beat;      /* bumped by the dispatcher; the watchdog watches it */
static volatile int me_dispatching;          /* watchdog is armed only while a slice is in flight */
static volatile unsigned watchdog_on;
static SceUID watchdog_th = -1;

#define UNC(p) ((void *) ((unsigned) (p) | 0x40000000u))   /* uncached alias, valid on both cores */

#ifdef SWDEC_ME_DIAG2
#include <stdio.h>
#endif
#ifdef SWDEC_ME_DIAG2
/* Diagnostic build: where each core is, sampled by the watchdog thread and
   written straight to host0 (no browser log). The ME writes its fields only
   through the uncached alias; the struct owns whole cache lines. */
typedef struct {
    volatile unsigned idle, stage, cmd_seen, audio_frames;
    volatile unsigned pad[12];
} SwdecMeDiag;
static SwdecMeDiag me_diag_storage __attribute__((aligned(64)));
#define ME_DIAG_SET(field, value) \
    (((volatile SwdecMeDiag *) UNC(&me_diag_storage))->field = (value))
#define ME_DIAG_INC(field) do { \
        volatile SwdecMeDiag *me_diag_ = UNC(&me_diag_storage); \
        me_diag_->field = me_diag_->field + 1; \
    } while (0)
static volatile unsigned cpu_diag_stage, cpu_diag_spins;
static volatile SceUID cpu_diag_thread = -1;   /* the decode worker, recorded at dispatch */
#define CPU_DIAG_STAGE(value) (cpu_diag_stage = (value))
#else
#define ME_DIAG_SET(field, value) ((void) 0)
#define ME_DIAG_INC(field) ((void) 0)
#define CPU_DIAG_STAGE(value) ((void) 0)
#endif

#ifdef SWDEC_STAGE_DIAG
/* Validation component builds only (TILEFINCH_PSP_VALIDATION_LOG): where an
   access unit's time goes, reported once per stream at close. The main CPU
   keeps microseconds. The Media Engine cannot make system calls; it counts
   CP0 Count ticks once its boot probe (me_clock_probe) has seen Count run.
   Count stood still on device until written: the probe writes Count and
   Compare (interrupts stay off) when it finds Count stopped.

   ME time is accounted by exclusive phase: every Count tick belongs to the
   one phase the ME is in (the phases the occupancy sampler sees), so the
   buckets add up to the ME's running time. The totals are 64-bit (Count
   wraps every ~26 s at 166 MHz) and published under a sequence count. */
typedef struct {
    unsigned aus, au_wall, au_run, slices, slice_wall, sleeps, sleep_wall,
             tail, done_wait, csc, csc_wall, csc_run;
    unsigned closes, close_wall, close_run, claim_wait;
    unsigned row_us[4];          /* pre, recon, border, post (swdec_pipe.h) */
    unsigned row_last, reset_at;
} SwdecCpuStages;
static SwdecCpuStages cpu_stages;
enum { ME_CLOCK_NONE = 0, ME_CLOCK_COUNT = 1 };
typedef struct {                 /* the ME writes only through UNC() */
    volatile unsigned clock;     /* ME_CLOCK_*, chosen by the ME at boot */
    volatile unsigned probe_count, probe_count_dc, probe_count_written;
    volatile unsigned probe_raw; /* Count as first read at boot */
    volatile unsigned pad[11];
} SwdecMeStages;
_Static_assert(sizeof(SwdecMeStages) == 64u, "ME stages must own one cache line");
static SwdecMeStages me_stages_storage __attribute__((aligned(64)));
#define ME_STAGES ((volatile SwdecMeStages *) UNC(&me_stages_storage))
static inline unsigned me_count(void)
{
    unsigned c;
    __asm__ volatile ("mfc0 %0, $9" : "=r" (c));
    return c;
}
static void me_probe_spin(void)
{
    for (volatile unsigned i = 0; i < 20000u; i++) { }
}
static unsigned me_probe_delta(void)
{
    unsigned c0 = me_count();
    me_probe_spin();
    return me_count() - c0;
}
/* ME, at boot: Count as found; with Cause.DC (bit 27) cleared; after
   writing Count and Compare. The deltas over a ~0.2 ms spin are reported
   with the stages. */
static void me_clock_probe(void)
{
    volatile SwdecMeStages *m = ME_STAGES;
    m->probe_raw = me_count();
    m->probe_count = me_probe_delta();
    if (m->probe_count != 0) { m->clock = ME_CLOCK_COUNT; return; }
    unsigned cause;
    __asm__ volatile ("mfc0 %0, $13" : "=r" (cause));
    cause &= ~(1u << 27);
    __asm__ volatile ("mtc0 %0, $13\n\tsync\n\tnop\n\tnop\n\t" : : "r" (cause));
    m->probe_count_dc = me_probe_delta();
    if (m->probe_count_dc != 0) { m->clock = ME_CLOCK_COUNT; return; }
    __asm__ volatile ("mtc0 $0, $9\n\tsync\n\t"
                      "mtc0 %0, $11\n\tsync\n\tnop\n\tnop\n\t"
                      : : "r" (166500u));
    m->probe_count_written = me_probe_delta();
    m->clock = m->probe_count_written != 0 ? ME_CLOCK_COUNT : ME_CLOCK_NONE;
}
/* ME phase, sampled by the CPU (me_occupancy). The ME stores it through
   the uncached alias at each change; the line holds nothing else. */
enum {
    ME_PHASE_IDLE = 0, ME_PHASE_PARSE, ME_PHASE_DEBLOCK, ME_PHASE_WAIT,
    ME_PHASE_CSC, ME_PHASE_AUDIO, ME_PHASE_DRAIN, ME_PHASE_OFF,
    ME_PHASES
};
typedef struct {
    volatile unsigned phase;
    volatile unsigned pad[15];
} SwdecMePhase;
_Static_assert(sizeof(SwdecMePhase) == 64u, "ME phase must own one cache line");
static SwdecMePhase me_phase_storage __attribute__((aligned(64))) = { ME_PHASE_OFF, {0} };
#define ME_PHASE ((volatile SwdecMePhase *) UNC(&me_phase_storage))
/* Exclusive tick totals per phase, published by the ME (uncached): `seq`
   is odd while an update is under way. */
typedef struct {
    volatile unsigned seq;
    volatile unsigned lo[ME_PHASES], hi[ME_PHASES];
    volatile unsigned pad[15];
} SwdecMeTicks;
_Static_assert(sizeof(SwdecMeTicks) % 64u == 0, "ME ticks must own whole cache lines");
static SwdecMeTicks me_ticks_storage __attribute__((aligned(64)));
#define ME_TICKS ((volatile SwdecMeTicks *) UNC(&me_ticks_storage))
/* The ME's own bookkeeping, in its cache only (the CPU never touches it). */
static struct {
    unsigned phase, since, seq, counting;
    unsigned before_deblock, before_wait, ping_seen;
    unsigned long long ticks[ME_PHASES];
} me_acct __attribute__((aligned(64)));
/* Row-ready-to-resume timing. The CPU arms `armed` before it sleeps for a
   row; the ME's next row publish stamps its Count into `first_pub`. To put
   ME Count on the CPU's clock, the CPU pings the idle ME before each slice
   (`ping_req`); the ME answers with its Count, and a reply within 20 us
   fixes the offset to about 10 us. */
typedef struct {
    volatile unsigned armed, first_pub;
    volatile unsigned ping_req, ping_ans, ping_count;
    volatile unsigned pad[11];
} SwdecMeRowClock;
_Static_assert(sizeof(SwdecMeRowClock) == 64u, "row clock must own one cache line");
static SwdecMeRowClock me_rowclock_storage __attribute__((aligned(64)));
#define ME_ROWCLOCK ((volatile SwdecMeRowClock *) UNC(&me_rowclock_storage))
static void me_acct_fold(unsigned now)
{
    unsigned phase = me_acct.phase < ME_PHASES ? me_acct.phase : ME_PHASE_IDLE;
    unsigned long long total = me_acct.ticks[phase] + (now - me_acct.since);
    me_acct.ticks[phase] = total;
    me_acct.since = now;
    volatile SwdecMeTicks *t = ME_TICKS;
    t->seq = ++me_acct.seq;
    t->lo[phase] = (unsigned) total;
    t->hi[phase] = (unsigned) (total >> 32);
    t->seq = ++me_acct.seq;
}
/* ME: enter `next`, charging the ticks since the last change to the phase
   being left. */
static void me_phase_enter(unsigned next)
{
    if (me_acct.counting) me_acct_fold(me_count());
    if (next != me_acct.phase) {
        me_acct.phase = next;
        ME_PHASE->phase = next;
    }
}
/* ME idle loop: stay idle without a store per lap; fold before Count's
   32-bit difference could wrap. */
static void me_phase_idle(void)
{
    if (me_acct.phase != ME_PHASE_IDLE) { me_phase_enter(ME_PHASE_IDLE); return; }
    if (me_acct.counting) {
        unsigned now = me_count();
        if (now - me_acct.since >= 0x40000000u) me_acct_fold(now);
    }
}
/* ME, at boot (after the clock probe): resume from the published totals. */
static void me_acct_boot(void)
{
    volatile SwdecMeTicks *t = ME_TICKS;
    for (unsigned i = 0; i < ME_PHASES; i++)
        me_acct.ticks[i] = ((unsigned long long) t->hi[i] << 32) | t->lo[i];
    me_acct.seq = t->seq;
    if (me_acct.seq & 1u) t->seq = ++me_acct.seq;   /* a reset cut an update */
    me_acct.counting = ME_STAGES->clock == ME_CLOCK_COUNT;
    me_acct.since = me_count();
    me_acct.phase = ME_PHASE_IDLE;
    ME_PHASE->phase = ME_PHASE_IDLE;
}
static inline unsigned stage_now(void) { return sceKernelGetSystemTimeLow(); }
/* The decoding thread's run time, and its preemption counts (by
   interrupts and by other threads) at the last call. */
static unsigned stage_preempt_intr, stage_preempt_thread;
static unsigned stage_run_us(void)
{
    SceKernelThreadRunStatus rs;
    memset(&rs, 0, sizeof rs);
    rs.size = sizeof rs;
    if (sceKernelReferThreadRunStatus(sceKernelGetThreadId(), &rs) < 0)
        return 0;
    stage_preempt_intr = rs.intrPreemptCount;
    stage_preempt_thread = rs.threadPreemptCount;
    return (unsigned) rs.runClocks.low;
}
/* CPU: ME Count on the CPU clock, and the resume delays measured with it. */
#define ROWC_BUCKETS 7u            /* <50, <100, <200, <400, <800, <1600 us, more */
static struct {
    unsigned seq, tried, ok, last_c, t_mid, base_t, rate_q16;
    unsigned long long c64, base_c64;
    int have;
    unsigned wakes, productive, empty, early, negative, delay_max;
    unsigned long long delay_sum;
    unsigned hist[ROWC_BUCKETS];
    unsigned intr0, thread0, preempt_intr, preempt_thread;
} rowc;
/* Keep the largest off-CPU decodes, not a log write inside each row wait.
   These are diagnostic samples; sleep and resume time overlap, as do
   tail-completion and sleep time. They must not be added as siblings. */
#define WAIT_TRACE_MAX 32u
typedef struct {
    uint64_t pts;
    unsigned wall, run, sleep, sleeps, done, resume, productive, empty;
    unsigned intr, thread, census;
    unsigned phase_us[ME_PHASES], phase_valid;
} SwdecWaitTrace;
static SwdecWaitTrace wait_trace[WAIT_TRACE_MAX], wait_before;
static unsigned wait_trace_count, wait_trace_seen;
static unsigned long long wait_ticks_before[ME_PHASES];
static int wait_ticks_valid;
static void me_rowclock_ping(void)
{
    if (ME_STAGES->clock != ME_CLOCK_COUNT) return;
    volatile SwdecMeRowClock *rc = ME_ROWCLOCK;
    unsigned seq = ++rowc.seq;
    rowc.tried++;
    unsigned t1 = stage_now(), t2;
    rc->ping_req = seq;
    do {
        t2 = stage_now();
        if (rc->ping_ans == seq) break;
    } while (t2 - t1 < 40u);
    if (rc->ping_ans != seq || t2 - t1 > 20u) return;
    unsigned c = rc->ping_count, mid = t1 + (t2 - t1) / 2u;
    if (!rowc.have) {
        rowc.have = 1;
        rowc.c64 = rowc.base_c64 = c;
        rowc.base_t = mid;
        rowc.rate_q16 = 166u * 65536u + 32768u;    /* 166.5 until measured */
    } else {
        rowc.c64 += (unsigned) (c - rowc.last_c);
        unsigned span = mid - rowc.base_t;
        if (span >= 1000000u)
            rowc.rate_q16 = (unsigned) ((rowc.c64 - rowc.base_c64) * 65536u
                                        / span);
    }
    rowc.last_c = c;
    rowc.t_mid = mid;
    rowc.ok++;
}
static void me_rowclock_wake(int published, unsigned first_pub,
                             unsigned resumed)
{
    rowc.wakes++;
    if (!published) { rowc.empty++; return; }
    if (first_pub == 0 || !rowc.have || rowc.rate_q16 == 0) {
        rowc.early++;                 /* published before the CPU armed */
        return;
    }
    long long ticks = (int) (first_pub - rowc.last_c);
    long long at = (long long) rowc.t_mid
        + ticks * 65536 / (long long) rowc.rate_q16;
    long long delay = (long long) (int) (resumed - (unsigned) at);
    if (delay < 0) { rowc.negative++; return; }
    rowc.productive++;
    rowc.delay_sum += (unsigned long long) delay;
    if (delay > rowc.delay_max) rowc.delay_max = (unsigned) delay;
    unsigned bucket = 0;
    for (unsigned limit = 50u; bucket + 1u < ROWC_BUCKETS
                               && (unsigned long long) delay >= limit;
         limit *= 2u) bucket++;
    rowc.hist[bucket]++;
}
/* CPU: other threads' run time while an access unit decodes, on every
   eighth unit: run clocks of every thread before and after. */
#define CENSUS_THREADS 40
/* The wall interval brackets every clock read of the sample: wall starts
   before the first thread's reading and ends after the last one, so no
   thread's interval extends past it. A thread whose clock could not be read
   at either end is left out of that sample (counted invalid), never
   treated as 0. */
static struct {
    SceUID id[CENSUS_THREADS];
    unsigned run0[CENSUS_THREADS];
    unsigned char ok0[CENSUS_THREADS];
    int n, active;
    unsigned t0, counter, aus, wall, invalid;
    struct {
        SceUID id;
        char name[24];
        unsigned samples;
        unsigned long long us, us2;  /* sum and sum of squares per sample */
    } acc[CENSUS_THREADS];
    int acc_n;
} census;
static int census_run(SceUID id, unsigned *us)
{
    SceKernelThreadRunStatus rs;
    memset(&rs, 0, sizeof rs);
    rs.size = sizeof rs;
    if (sceKernelReferThreadRunStatus(id, &rs) < 0) return 0;
    *us = (unsigned) rs.runClocks.low;
    return 1;
}
static void census_begin(void)
{
    census.active = 0;
    if ((census.counter++ & 7u) != 0) return;
    int count = 0;
    if (sceKernelGetThreadmanIdList(SCE_KERNEL_TMID_Thread, census.id,
                                    CENSUS_THREADS, &count) < 0) return;
    census.n = count > CENSUS_THREADS ? CENSUS_THREADS : count;
    census.t0 = stage_now();
    for (int i = 0; i < census.n; i++)
        census.ok0[i] = (unsigned char) census_run(census.id[i],
                                                   &census.run0[i]);
    census.active = 1;
}
static void census_end(void)
{
    if (!census.active) return;
    census.active = 0;
    unsigned run1[CENSUS_THREADS];
    unsigned char ok1[CENSUS_THREADS];
    for (int i = 0; i < census.n; i++)
        ok1[i] = (unsigned char) census_run(census.id[i], &run1[i]);
    census.wall += stage_now() - census.t0;
    census.aus++;
    for (int i = 0; i < census.n; i++) {
        if (!census.ok0[i] || !ok1[i]) { census.invalid++; continue; }
        unsigned delta = run1[i] - census.run0[i];
        int k = 0;
        while (k < census.acc_n && census.acc[k].id != census.id[i]) k++;
        if (k == census.acc_n) {
            if (k == CENSUS_THREADS) continue;
            census.acc_n++;
            memset(&census.acc[k], 0, sizeof census.acc[k]);
            census.acc[k].id = census.id[i];
            SceKernelThreadInfo info;
            if (psp_thread_snapshot(census.id[i], &info) < 0)
                info.name[0] = '\0';
            memcpy(census.acc[k].name, info.name, sizeof census.acc[k].name - 1u);
            census.acc[k].name[sizeof census.acc[k].name - 1u] = '\0';
        }
        census.acc[k].samples++;
        census.acc[k].us += delta;
        census.acc[k].us2 += (unsigned long long) delta * delta;
    }
}
static void stage_mark(int mark)
{
    switch (mark) {
    case SWDEC_MARK_ME_DEBLOCK:
        me_acct.before_deblock = me_acct.phase;
        me_phase_enter(ME_PHASE_DEBLOCK);
        break;
    case SWDEC_MARK_ME_DEBLOCK_END:
        me_phase_enter(me_acct.before_deblock);
        break;
    case SWDEC_MARK_ME_WAIT:
        me_acct.before_wait = me_acct.phase;
        me_phase_enter(ME_PHASE_WAIT);
        break;
    case SWDEC_MARK_ME_WAIT_END:
        me_phase_enter(me_acct.before_wait);
        break;
    default: {                   /* CPU row marks arrive in order */
        unsigned now = stage_now();
        if (mark > SWDEC_MARK_CPU_PRE && mark <= SWDEC_MARK_CPU_ROW_END)
            cpu_stages.row_us[mark - 1] += now - cpu_stages.row_last;
        cpu_stages.row_last = now;
        break;
    }
    }
}
#endif
#ifdef SWDEC_STAGE_DIAG
#define STAGE_ME_PROBE_CLOCK() (me_clock_probe(), me_acct_boot())
#define STAGE_ME_PHASE(value) me_phase_enter(value)
#define STAGE_ME_PHASE_SAVE(v) unsigned v = me_acct.phase
#else
#define STAGE_ME_PROBE_CLOCK() ((void) 0)
#define STAGE_ME_PHASE(value) ((void) 0)
#define STAGE_ME_PHASE_SAVE(v) ((void) 0)
#endif

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
static void me_pre_publish(void)
{
    me_dcache_writeback_all();
#ifdef SWDEC_STAGE_DIAG
    volatile SwdecMeRowClock *rc = ME_ROWCLOCK;
    if (rc->armed) {
        rc->first_pub = me_count();
        rc->armed = 0;
    }
#endif
}

static void me_csc_idle_one(void);      /* defined with the CSC block below */
static void me_csc_idle_one_ex(int wb);

/* The idle loop is read-only and the command is a magic word: if the CPU side
   ever dies and this memory is reused, an orphaned ME can neither corrupt it
   nor be tricked into running a job. */
#define SWDEC_ME_CMD_SLICE 0x5A11CE01u
/* CPU sleep between polls while the ME has no new row for the dispatcher. */
#define SWDEC_ME_WAIT_SLEEP_US 100u

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
    STAGE_ME_PROBE_CLOCK();
    b->heartbeat = 1;                                  /* one-time hello */
    for (;;) {
#ifdef SWDEC_STAGE_DIAG
        me_phase_idle();
        {   /* answer the CPU's clock ping (one uncached read a lap) */
            volatile SwdecMeRowClock *rc = ME_ROWCLOCK;
            unsigned request = rc->ping_req;
            if (request != me_acct.ping_seen) {
                rc->ping_count = me_count();
                me_acct.ping_seen = request;
                rc->ping_ans = request;
            }
        }
#endif
        ME_DIAG_INC(idle);
        ME_DIAG_SET(stage, 1u);
        /* inter-AU idle: CSC first (the CPU is waiting to close this frame);
           audio second - its PCM ring holds ~743ms and can wait a millisecond */
        me_csc_idle_one_ex(1);
        ME_DIAG_SET(stage, 2u);
        {   /* the hook is installed after boot: read it through the uncached alias */
            void (*aux_)(void) = *(void (**)(void)) UNC(&swdec_me_aux);
            if (aux_) aux_();
        }
        ME_DIAG_SET(stage, 3u);
        if (b->cmd == SWDEC_ME_CMD_SLICE && b->token == ~b->avctx) {
            ME_DIAG_INC(cmd_seen);
            ME_DIAG_SET(stage, 4u);
            STAGE_ME_PHASE(ME_PHASE_PARSE);
            int r = ff_swdec_decode_slice((struct AVCodecContext *) b->avctx, (void *) b->sl);
            STAGE_ME_PHASE(ME_PHASE_DRAIN);
            ME_DIAG_SET(stage, 5u);
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

#if SWDEC_DIAG_ENABLED
int swdec_me_noinv = 0;   /* diagnostics: 1 = skip the per-row cache maintenance (expect mismatches) */
#else
#define swdec_me_noinv 0
#endif
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
 * Once receive_frame identifies the picture produced by an AU (or by the EOF
 * flush), the CPU binds that picture to an RGB565 destination slot. The ME and
 * CPU then share its final eight-row units; the two-word done mask makes the
 * split self-balancing and covers the full 272-pixel PSP display without a
 * tear-prone shared uint64_t. */
#define CSC_SLOTS SWDEC_ME_CSC_SLOTS
typedef struct {
    const uint8_t *y, *u, *v;       /* source planes (recorded by the ME) */
    int ys, uvs, w, h;              /* strides / cropped dims */
    uint16_t *dst;
    int dst_stride;
    size_t dst_capacity;
    SwdecCscMask done;              /* bit n = eight-row unit n converted */
    volatile unsigned recorded;     /* the ME saw this frame */
    volatile unsigned rejected;     /* decoded geometry exceeds dst */
    volatile int max_final;         /* highest row batch whose pixels are final */
} SwdecCscSlot;
/* State both cores share through the uncached alias must own whole cache
   lines. The main CPU writes its own neighbouring variables through the
   cache; when such a dirty line is written back (by eviction, or by the
   whole-cache writeback before a conversion) it carries the CPU's stale
   copy of the shared bytes with it and overwrites what the uncached stores
   put in RAM. The slot offer sharing a line with the decoder's per-frame
   swdec_ring_dirty, and the last conversion slot sharing one with the
   dispatcher's per-iteration beat counter, lost a published slot that way:
   the conversion then found it off and refused the picture. */
#define SWDEC_SHARED_LINES(bytes) (((bytes) + 63u) & ~(size_t) 63u)
static union {
    SwdecCscSlot slot[CSC_SLOTS];
    unsigned char lines[SWDEC_SHARED_LINES(sizeof(SwdecCscSlot) * CSC_SLOTS)];
} csc_slots_box __attribute__((aligned(64)));
#define csc_slots (csc_slots_box.slot)
/* Which slot the ME may convert, and whether it is converting one now
   (swdec_csc_claim.h): the CPU withdraws the offer and waits for the ME's
   batch to end before it finishes, publishes, rebinds or releases a slot. */
static SwdecCscClaim csc_claim_box __attribute__((aligned(64))) = { -1, -1, 0, 0, {0} };
#define CSC_CLAIM ((SwdecCscClaim *) UNC(&csc_claim_box))
_Static_assert(sizeof(csc_slots_box) % 64u == 0, "csc slots must fill whole cache lines");
static volatile unsigned csc_me_batches, csc_cpu_batches;
/* Main-CPU conversions use the VFPU once swdec_csc_vfpu_verified() agreed
   with the portable converter on this machine (checked at the first
   CPU-side conversion, on the decoding thread, which holds the VFPU). */
static int csc_cpu_vfpu, csc_cpu_checked;

/* convert luma rows [unit*8, unit*8+8) of slot s; 8-row units pack into the
   ME's sub-millisecond idle gaps without making a queued deblock job wait
   behind a long conversion. The Media Engine has no VFPU, so it runs the
   portable converter; the main CPU runs the VFPU one once it has proved
   itself against the portable one (see swdec_csc.h). */
static void csc_batch(SwdecCscSlot *s, int unit, int on_cpu)
{
    if (!swdec_rgb565_destination_fits(
            s->w, s->h, s->dst_stride, s->dst_capacity)) return;
    int j1 = (unit + 1) * 8; if (j1 > s->h) j1 = s->h;
    SwdecCscPlanes planes = {
        .y = s->y, .u = s->u, .v = s->v,
        .y_stride = s->ys, .uv_stride = s->uvs,
        .width = s->w, .dst = s->dst, .dst_stride = s->dst_stride
    };
    if (on_cpu && csc_cpu_vfpu)
        swdec_csc_rows_vfpu(&planes, unit * 8, j1);
    else
        swdec_csc_rows_c(&planes, unit * 8, j1);
}
/* ME side: the deblock service reports row `batch` final. Recording only -
   converting here would sit on the critical path (it delays the next row
   parse and the queue drain the CPU waits on). The conversion itself runs
   from the drain-phase idle loop below. */
static void me_csc_row(const H264Context *h, int batch)
{
    int slot = swdec_csc_claim_take(CSC_CLAIM);
    if (slot < 0) return;
    SwdecCscSlot *s = (SwdecCscSlot *) UNC(&csc_slots[slot]);
    if (!s->recorded) {
        /* The picture being decoded has the coded size (416x240 for a
           416x234 stream); the output picture is that cropped. Convert the
           cropped picture: its rows are the coded picture's first ones when
           the SPS crops only right and bottom, as encoders do. */
        AVFrame *f = h->cur_pic.f;
        const SPS *sps = h->ps.sps;
        int w = h->avctx->width, ht = h->avctx->height;
        if (f == NULL || sps == NULL || sps->crop_left != 0
            || sps->crop_top != 0 || w <= 0 || ht <= 0
            || w > f->width || ht > f->height
            || !swdec_rgb565_destination_fits(
                w, ht, s->dst_stride, s->dst_capacity)) {
            s->rejected = 1;
            swdec_csc_claim_release(CSC_CLAIM);
            return;
        }
        s->y = f->data[0]; s->u = f->data[1]; s->v = f->data[2];
        s->ys = f->linesize[0]; s->uvs = f->linesize[1];
        s->w = w; s->h = ht;
        s->recorded = 1;
    }
    /* row batch `batch` (16 luma rows) is final: its two 8-row units are ready */
    if (2 * batch + 1 > s->max_final) s->max_final = 2 * batch + 1;
    swdec_csc_claim_release(CSC_CLAIM);
}
/* ME idle work: convert ONE pending final 8-row unit (bounded: ~0.45ms, so a
   fresh deblock job is never kept waiting long). wb: write the unit back to
   RAM immediately - required for conversions in the inter-AU idle loop, where
   no slice-end writeback follows before the presenter can read the slot.
   Without wb (the drain inside a slice) the pixels reach RAM at the slice's
   final writeback, which precedes the slice's completion; the CPU closes a
   conversion only after that. */
static void me_csc_idle_one_ex(int wb)
{
    SwdecCscClaim *claim = CSC_CLAIM;
    int slot = swdec_csc_claim_take(claim);
    if (slot < 0) return;
    SwdecCscSlot *s = (SwdecCscSlot *) UNC(&csc_slots[slot]);
    if (!s->recorded || s->rejected
        || !swdec_rgb565_destination_fits(
            s->w, s->h, s->dst_stride, s->dst_capacity)) {
        swdec_csc_claim_release(claim);
        return;
    }
    int mf = s->max_final;
    SwdecCscMask did = {{0, 0}};
    int n = 0;
    if (mf >= (int) SWDEC_CSC_MASK_UNITS)
        mf = (int) SWDEC_CSC_MASK_UNITS - 1;
    STAGE_ME_PHASE_SAVE(stage_phase);
    STAGE_ME_PHASE(ME_PHASE_CSC);
    for (int b = 0; b <= mf && n < (wb ? 3 : 1); b++) {
        if (swdec_csc_mask_test(&s->done, (unsigned) b)
            || swdec_csc_mask_test(&did, (unsigned) b)) continue;
        csc_batch(s, b, 0);
        swdec_csc_mask_set(&did, (unsigned) b);
        n++;
        if (!wb) break;
    }
    if (did.words[0] == 0 && did.words[1] == 0) {
        STAGE_ME_PHASE(stage_phase);
        swdec_csc_claim_release(claim);
        return;
    }
    /* inter-AU: write the pixels back BEFORE publishing the bits, so the CPU
       can never see done=1 for rows that are still only in the ME dcache
       (one sweep per batch of units, not per unit - the sweep also evicts the
       working set, so batching it matters) */
    if (wb) me_dcache_writeback_all();
    s->done.words[0] = s->done.words[0] | did.words[0];
    s->done.words[1] = s->done.words[1] | did.words[1];
    *(volatile unsigned *) UNC(&csc_me_batches) += (unsigned) n;
    STAGE_ME_PHASE(stage_phase);
    swdec_csc_claim_release(claim);
}
static void me_csc_idle_one(void) { me_csc_idle_one_ex(0); }

/* CPU side */
static int swdec_me_visible(const void *pointer, size_t bytes);
/* Bounded wait for the ME's batch on a withdrawn slot (see
   swdec_csc_claim_reclaim): a batch still announced after the limit means
   the ME is wedged, and it is stopped through the ordinary ME failure path
   before the CPU touches the slot. */
#define SWDEC_CSC_CLAIM_TIMEOUT_US 20000u
#define SWDEC_CSC_CLAIM_SPIN_US 200u
static unsigned csc_claim_waits, csc_claim_timeouts;
/* Slots an ME that could not be stopped may still write: never finished,
   published or offered again until a reclaim finds no batch announced. */
static unsigned csc_quarantine;
static void me_fail(const char *why);
static unsigned csc_reclaim_now(void *context)
{
    (void) context;
    return sceKernelGetSystemTimeLow();
}
static void csc_reclaim_pause(void *context, unsigned waited_us)
{
    (void) context;
    if (waited_us >= SWDEC_CSC_CLAIM_SPIN_US) sceKernelDelayThread(50);
}
static bool csc_reclaim_stop_me(void *context)
{
    (void) context;
    me_fail("colour conversion batch did not end");
    return me_dead != 0;
}
static int csc_quarantined(int slot)
{
    return slot >= 0 && slot < CSC_SLOTS
        && (csc_quarantine & (1u << (unsigned) slot)) != 0;
}
/* Take the conversion slot back from the ME: withdraw the offer and wait
   until no ME batch can touch it. 0 when the CPU owns every slot again
   (the ME let go, or was stopped); -1 when a wedged ME could not be
   stopped and its slot is quarantined. */
static int csc_claim_reclaim(void)
{
    SwdecCscClaim *claim = CSC_CLAIM;
    if (me_dead) {                    /* the ME is held in reset */
        swdec_csc_claim_init(claim);
        csc_quarantine = 0;
        return 0;
    }
    if (claim->active < 0 && claim->busy < 0) {
        csc_quarantine = 0;
        return 0;
    }
#ifdef SWDEC_STAGE_DIAG
    unsigned stage_t0 = stage_now();
#endif
    csc_claim_waits++;
    static const SwdecCscReclaimOps ops = {
        csc_reclaim_now, csc_reclaim_pause, csc_reclaim_stop_me, NULL
    };
    int held = -1;
    SwdecCscReclaim result = swdec_csc_claim_reclaim(
        claim, SWDEC_CSC_CLAIM_TIMEOUT_US, &ops, &held);
#ifdef SWDEC_STAGE_DIAG
    cpu_stages.claim_wait += stage_now() - stage_t0;
#endif
    if (result == SWDEC_CSC_RECLAIM_OK) {
        csc_quarantine = 0;
        return 0;
    }
    if (csc_claim_timeouts++ < 3u)
        SWDEC_LOG_TO(me_log, "swdec-me: colour conversion batch still "
                     "running after %u us; %s\n", SWDEC_CSC_CLAIM_TIMEOUT_US,
                     result == SWDEC_CSC_RECLAIM_STOPPED
                         ? "Media Engine stopped, converting on the main CPU"
                         : "slot quarantined, picture dropped");
    if (result == SWDEC_CSC_RECLAIM_STOPPED) {
        csc_quarantine = 0;
        return 0;
    }
    if (held >= 0 && held < CSC_SLOTS) csc_quarantine |= 1u << (unsigned) held;
    return -1;
}
void swdec_me_csc_begin(int slot, void *dst_rgb565, int stride_pixels,
                        size_t capacity_bytes)
{
    /* The previous binding may still be the ME's: take it back first, so
       a late batch can never land in the slot's new picture. */
    (void) csc_claim_reclaim();
    /* A slot the Media Engine cannot reach is never bound to it: the close
       then reports nothing converted and the browser converts the picture
       on the CPU (swdec_me_csc_picture). Neither is a quarantined one. */
    if (slot < 0 || slot >= CSC_SLOTS || csc_quarantined(slot)
        || dst_rgb565 == NULL
        || stride_pixels <= 0 || capacity_bytes == 0
        || !swdec_me_visible(dst_rgb565, capacity_bytes))
        return;
    SwdecCscSlot *s = (SwdecCscSlot *) UNC(&csc_slots[slot]);
    s->dst = dst_rgb565;
    s->dst_stride = stride_pixels;
    s->dst_capacity = capacity_bytes;
    swdec_csc_mask_clear(&s->done);
    s->recorded = 0; s->rejected = 0; s->max_final = -1;
    s->y = s->u = s->v = NULL;
    swdec_csc_claim_offer(CSC_CLAIM, slot);
}
/* The Media Engine reaches only the stock 24 MiB of user RAM: a picture slot
   the browser had to place in the extra bank (0x0A000000 up) is converted
   on the main CPU alone. */
#define SWDEC_ME_VISIBLE_LIMIT 0x0A000000u
static int swdec_me_visible(const void *pointer, size_t bytes)
{
    uintptr_t physical = (uintptr_t) pointer & 0x1FFFFFFFu;
    return physical < SWDEC_ME_VISIBLE_LIMIT
        && bytes <= SWDEC_ME_VISIBLE_LIMIT - physical;
}
static unsigned csc_high_slots;
static int csc_finish_on_cpu(SwdecCscSlot *s, unsigned completed_cpu);
static int csc_close_slot(int slot);
static void csc_cpu_init(void);
int swdec_me_csc_picture(int slot, void *dst_rgb565, int stride_pixels,
                         size_t capacity_bytes,
                         const uint8_t *y, const uint8_t *u,
                         const uint8_t *v, int y_stride, int uv_stride,
                         int width, int height)
{
    unsigned units = swdec_csc_unit_count(height);
    if (dst_rgb565 == NULL || y == NULL || u == NULL || v == NULL
        || units == 0u
        || !swdec_rgb565_destination_fits(
            width, height, stride_pixels, capacity_bytes)) return 0;
    if (slot >= 0 && slot < CSC_SLOTS
        && !swdec_me_visible(dst_rgb565, capacity_bytes)) {
        /* Never offered to the ME; take back any earlier offer first. */
        (void) csc_claim_reclaim();
        if (csc_quarantined(slot)) return 0;
        SwdecCscSlot *cpu = (SwdecCscSlot *) UNC(&csc_slots[slot]);
        cpu->dst = dst_rgb565;
        cpu->dst_stride = stride_pixels;
        cpu->dst_capacity = capacity_bytes;
        swdec_csc_mask_clear(&cpu->done);
        cpu->rejected = 0;
        cpu->y = y; cpu->u = u; cpu->v = v;
        cpu->ys = y_stride; cpu->uvs = uv_stride;
        cpu->w = width; cpu->h = height;
        cpu->max_final = (height + 7) / 8 - 1;
        cpu->recorded = 1;
        /* The ME deblocked most rows in RAM; drop any stale lines of them
           (writing the CPU's own tail rows back first). */
        sceKernelDcacheWritebackInvalidateAll();
        if (csc_high_slots++ == 0)
            SWDEC_LOG_TO(me_log, "swdec-me: picture slot at %08x is outside "
                         "the Media Engine's reach; converting it on the "
                         "main CPU\n", (unsigned) (uintptr_t) dst_rgb565);
        return csc_finish_on_cpu(cpu, 0);
    }
    swdec_me_csc_begin(
        slot, dst_rgb565, stride_pixels, capacity_bytes);
    if (slot < 0 || slot >= CSC_SLOTS || csc_quarantined(slot)
        || CSC_CLAIM->active != slot) return 0;
    SwdecCscSlot *s = (SwdecCscSlot *) UNC(&csc_slots[slot]);
    s->y = y; s->u = u; s->v = v;
    s->ys = y_stride; s->uvs = uv_stride;
    s->w = width; s->h = height;
    unsigned split = swdec_csc_cpu_first(
        units, dst_rgb565, (unsigned) stride_pixels, !me_dead);
    s->max_final = (int) split - 1;
    /* Snapshot the binding before publishing recorded. The ME alone writes
       its completion mask while both cores convert disjoint row ranges. */
    SwdecCscSlot local = *s;
    if (split < units) csc_cpu_init();
    /* The ME reads the planes from RAM. This core may still hold dirty
       lines of them: the tail rows it reconstructed and deblocked itself
       at the end of the slice are written back only by later eviction.
       Push them out (the whole 16 KiB D-cache costs less than walking the
       picture's ~150 KiB) before the ME may convert a row. */
    sceKernelDcacheWritebackAll();
    __asm__ volatile ("sync" ::: "memory");
    s->recorded = 1;
    /* This path binds conversion after receive_frame has identified the
       actual output picture. It is used for reordered and EOF-delayed
       pictures, where preselecting a destination from the submitted AU is
       incorrect. */
    if (split < units) {
        for (unsigned b = split; b < units; b++) {
            csc_batch(&local, (int) b, 1);
            unsigned end = (b + 1u) * 8u;
            if (end > (unsigned) height) end = (unsigned) height;
            sceKernelDcacheWritebackRange(
                local.dst + b * 8u * (unsigned) local.dst_stride,
                (end - b * 8u) * (unsigned) local.dst_stride * 2u);
        }
        /* Join before merging mask bits, finishing a slow/unstarted ME
           prefix, publishing, or releasing any pixels. A failed join keeps
           the existing quarantine and never exposes a partial picture. */
        if (csc_claim_reclaim() != 0 && csc_quarantined(slot)) return 0;
        for (unsigned b = split; b < units; b++)
            swdec_csc_mask_set(&s->done, b);
        csc_cpu_batches += units - split;
        return csc_finish_on_cpu(s, units - split);
    }
    return csc_close_slot(slot);
}
void swdec_me_csc_off(void) { (void) csc_claim_reclaim(); }
/* AU done: take the slot back from the ME, finish the batches it didn't
   reach; returns 1 if the slot holds a frame (writes back the CPU-converted
   regions for the GE), 0 if untouched */
int swdec_me_csc_close(void)
{
    int slot = CSC_CLAIM->active;
    if (slot < 0) return 0;
    return csc_close_slot(slot);
}
static int csc_close_slot(int slot)
{
    /* A slot the ME may still write is neither finished nor published:
       the browser drops the picture. */
    if (csc_claim_reclaim() != 0 && csc_quarantined(slot)) return 0;
    SwdecCscSlot *s = (SwdecCscSlot *) UNC(&csc_slots[slot]);
    int converted = csc_finish_on_cpu(s, 0);
#if SWDEC_DIAG_ENABLED
    {   /* why a bound conversion held nothing (first few) */
        static unsigned reported;
        if (!converted && reported < 3u) {
            reported++;
            SWDEC_LOG_TO(me_log, "swdec-me: csc close empty slot=%d recorded=%d "
                         "rejected=%d size=%dx%d final=%d\n", slot,
                         (int) s->recorded, (int) s->rejected, s->w, s->h,
                         s->max_final);
        }
    }
#endif
    return converted;
}
#ifdef SWDEC_STAGE_DIAG
/* Per converted picture: eight-row units the ME had done when the CPU took
   the slot back, units the CPU converted, and the ME's share. */
static unsigned csc_split_pictures, csc_split_me_units, csc_split_cpu_units;
static unsigned csc_split_share[SWDEC_CSC_SHARE_BUCKETS];
#endif
static void csc_cpu_init(void)
{
    if (!csc_cpu_checked) {
        csc_cpu_checked = 1;
        csc_cpu_vfpu = swdec_csc_vfpu_verified();
        SWDEC_LOG_TO(me_log, "swdec-me: colour conversion on the main CPU: %s\n",
                     csc_cpu_vfpu ? "vfpu (agrees with the portable form)"
                                  : "portable (vfpu check failed)");
    }
}
/* Convert every batch the ME did not reach; 1 if the slot holds a frame.
   The slot is the CPU's (never offered, or taken back). */
static int csc_finish_on_cpu(SwdecCscSlot *s, unsigned completed_cpu)
{
    if (!s->recorded || s->rejected
        || !swdec_rgb565_destination_fits(
            s->w, s->h, s->dst_stride, s->dst_capacity)) return 0;
    int nb = (int) swdec_csc_unit_count(s->h);
    if (nb == 0) return 0;
    csc_cpu_init();
    unsigned cpu_units = completed_cpu;
    for (int b = 0; b < nb; b++) {
        if (swdec_csc_mask_test(&s->done, (unsigned) b)) continue;
        /* the CPU wrote these pixels last (tail deblock): its cache is current */
        csc_batch(s, b, 1);
        swdec_csc_mask_set(&s->done, (unsigned) b);
        csc_cpu_batches++;
        cpu_units++;
        int j0 = b * 8, j1 = (b + 1) * 8; if (j1 > s->h) j1 = s->h;
        sceKernelDcacheWritebackRange(
            s->dst + j0 * s->dst_stride,
            (unsigned) (j1 - j0) * (unsigned) s->dst_stride * 2u);
    }
#ifdef SWDEC_STAGE_DIAG
    unsigned me_units = (unsigned) nb - cpu_units;
    csc_split_pictures++;
    csc_split_me_units += me_units;
    csc_split_cpu_units += cpu_units;
    csc_split_share[swdec_csc_share_bucket(me_units, (unsigned) nb)]++;
#endif
    return 1;
}
#if SWDEC_DIAG_ENABLED
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
#endif

#if SWDEC_DIAG_ENABLED
/* me_log is the browser's logger. A thread this module creates (the
   watchdog) starts with this module's $gp, while the browser's C library
   reaches _impure_ptr through the browser's $gp: logging from the watchdog
   took the browser's log lock and then wrote through a wrong FILE pointer,
   leaving every later browser log call blocked. The watchdog therefore only
   formats its line here; a browser thread (decode, audio poll, detach)
   prints it. */
static char me_deferred_text[320];
static volatile int me_deferred_ready;
static int me_on_watchdog_thread(void)
{
    return watchdog_th >= 0 && sceKernelGetThreadId() == watchdog_th;
}
static void me_say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void me_say(const char *fmt, ...)
{
    if (!me_log) return;
    va_list args;
    if (me_on_watchdog_thread()) {
        if (me_deferred_ready) return;            /* keep the first unread line */
        va_start(args, fmt);
        vsnprintf(me_deferred_text, sizeof me_deferred_text, fmt, args);
        va_end(args);
        me_deferred_ready = 1;
        return;
    }
    char line[320];
    va_start(args, fmt);
    vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    me_log("%s", line);
}
static void me_flush_deferred(void)
{
    if (!me_deferred_ready || !me_log || me_on_watchdog_thread()) return;
    char line[sizeof me_deferred_text];
    int interrupts = sceKernelCpuSuspendIntr();
    int ready = me_deferred_ready;
    if (ready) {
        memcpy(line, me_deferred_text, sizeof line);
        me_deferred_ready = 0;
    }
    sceKernelCpuResumeIntr(interrupts);
    if (ready) me_log("%s", line);
}
#else
/* Without diagnostics nothing is logged, so nothing is deferred. */
#define me_say(...) SWDEC_LOG_TO(me_log, __VA_ARGS__)
static void me_flush_deferred(void) { }
#endif

int swdec_me_failed = 0;      /* set on any ME failure; the harness stops the run (product code may fall back) */
static void me_fail(const char *why)
{
    me_dispatching = 0;
    me_say("swdec-me: %s - stopping the ME\n", why);
    swdec_me_stop();            /* hold the ME in reset before anything else can go wrong */
    swdec_me_enabled = 0;       /* later slices (if any) decode on the CPU, sequential pipeline */
    swdec_me_run_slice = NULL;  /* and without any ME hooks */
    swdec_me_pre_publish = NULL; swdec_cpu_pre_consume = NULL; swdec_cpu_post_consume = NULL;
    swdec_me_deblock = 0; swdec_cpu_flush_row = NULL; swdec_me_post_deblock = NULL;
    swdec_me_csc_row = NULL; swdec_csc_claim_init(CSC_CLAIM);   /* the ME is held in reset */
    csc_quarantine = 0;
#ifdef SWDEC_STAGE_DIAG
    swdec_stage_mark = NULL;
    ME_PHASE->phase = ME_PHASE_OFF;   /* held in reset: samples count it off */
#endif
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
    me_flush_deferred();
    if (me_dead) return AVERROR_EXTERNAL;
    if (me_slices < 8)
        SWDEC_LOG_TO(me_log, "swdec-me: slice %u start (au state: prod=%d cons=%d)\n", me_slices, pp[0], pp[1]);
    CPU_DIAG_STAGE(10u);
#ifdef SWDEC_ME_DIAG2
    cpu_diag_thread = sceKernelGetThreadId();
#endif
    swdec_pipe_slice_begin();
    me_dispatching = 1;
    unsigned want = b->done + 1;
    CPU_DIAG_STAGE(11u);
    sceKernelDcacheWritebackInvalidateAll();      /* everything the ME must see (slice header state, ring memset, ...) */
    CPU_DIAG_STAGE(12u);
    b->avctx = (unsigned) avctx; b->sl = (unsigned) sl; b->token = ~(unsigned) avctx;
#ifdef SWDEC_STAGE_DIAG
    me_rowclock_ping();
#endif
    __asm__ volatile ("sync" ::: "memory");
    b->cmd = SWDEC_ME_CMD_SLICE;
    CPU_DIAG_STAGE(13u);
    unsigned long long t0 = (unsigned long long) sceKernelGetSystemTimeWide();
    unsigned spins = 0;
    int tail_done = 0;
#ifdef SWDEC_STAGE_DIAG
    unsigned stage_tail_end = 0;
#endif
    for (;;) {
        swdec_me_beat++;
#ifdef SWDEC_ME_DIAG2
        cpu_diag_spins = spins;
        CPU_DIAG_STAGE(14u);
#endif
        int consumed_before = pp[1];
        swdec_pipe_consume_all((void *) h, sl);
        CPU_DIAG_STAGE(15u);
        int progressed = pp[1] != consumed_before;
#ifdef SWDEC_ME_DEBLOCK
        if (!tail_done && swdec_pipe_parse_done()) {
            progressed = 1;   /* all rows parsed: recon the rest, push the held-back row */
#ifdef SWDEC_STAGE_DIAG
            unsigned stage_tail_t0 = stage_now();
#endif
            swdec_pipe_consume_all((void *) h, sl);
            swdec_pipe_deblock_flush((const void *) h);
            swdec_pipe_set_tail_pushed();
            tail_done = 1;
#ifdef SWDEC_STAGE_DIAG
            stage_tail_end = stage_now();
            cpu_stages.tail += stage_tail_end - stage_tail_t0;
#endif
        }
#else
        (void) tail_done;
#endif
        if (swdec_pipe_bad) {
            SWDEC_LOG_TO(me_log, "swdec-me: BAD kind=%d at slice %u: info={%d %d %d %d %d %d} prod=%d cons=%d\n", swdec_pipe_bad, me_slices,
                         swdec_pipe_bad_info[0], swdec_pipe_bad_info[1], swdec_pipe_bad_info[2], swdec_pipe_bad_info[3],
                         swdec_pipe_bad_info[4], swdec_pipe_bad_info[5], pp[0], pp[1]);
            SWDEC_LOG_TO(me_log, "swdec-me: rec0={%d %d %d %d %d} rec1={%d %d %d %d %d}\n", pp[3], pp[4], pp[5], pp[6], pp[7], pp[8], pp[9], pp[10], pp[11], pp[12]);
            me_fail("inconsistent row record/job");
            return AVERROR_EXTERNAL;
        }
        if (b->done == want) break;
        /* Nothing to reconstruct yet: sleep instead of spinning. A hot spin
           keeps every lower-priority thread (the network service among
           them) off the CPU for most of each frame; on a PSP-3000 the
           stream's HTTP windows slowed to about 27 KB/s meanwhile. */
        if (!progressed) {
#ifdef SWDEC_STAGE_DIAG
            volatile SwdecMeRowClock *rc = ME_ROWCLOCK;
            int produced = pp[0];
            rc->first_pub = 0;
            rc->armed = 1;
            unsigned stage_sleep_t0 = stage_now();
            sceKernelDelayThread(SWDEC_ME_WAIT_SLEEP_US);
            unsigned stage_resumed = stage_now();
            rc->armed = 0;
            me_rowclock_wake(pp[0] != produced, rc->first_pub, stage_resumed);
            cpu_stages.sleep_wall += stage_resumed - stage_sleep_t0;
            cpu_stages.sleeps++;
#else
            sceKernelDelayThread(SWDEC_ME_WAIT_SLEEP_US);
#endif
        }
        if ((++spins & 1023u) == 0) {
            unsigned c, e;
            unsigned el = (unsigned) ((unsigned long long) sceKernelGetSystemTimeWide() - t0);
            if (swdec_me_fault(&c, &e)) {
                SWDEC_LOG_TO(me_log, "swdec-me: ME FAULT cause=%08x epc=%08x at slice %u (prod=%d cons=%d)\n", c, e, me_slices, pp[0], pp[1]);
                me_fail("ME exception");
                return AVERROR_EXTERNAL;
            }
            if ((spins & (65536u - 1u)) == 0 && me_slices < 2)
                SWDEC_LOG_TO(me_log, "swdec-me: waiting: slice %u t=%u us prod=%d cons=%d done=%u want=%u hb=%u\n", me_slices, el, pp[0], pp[1], b->done, want, b->heartbeat);
            if (el > 2000000u) {
                SWDEC_LOG_TO(me_log, "swdec-me: ME timeout at slice %u: prod=%d cons=%d done=%u want=%u hb=%u\n", me_slices, pp[0], pp[1], b->done, want, b->heartbeat);
                me_fail("ME timeout");
                return AVERROR_EXTERNAL;
            }
        }
    }
    swdec_pipe_consume_all((void *) h, sl);
    unsigned el = (unsigned) ((unsigned long long) sceKernelGetSystemTimeWide() - t0);
    me_wait_us += el;
#ifdef SWDEC_STAGE_DIAG
    cpu_stages.slices++;
    cpu_stages.slice_wall += el;
    if (stage_tail_end != 0) cpu_stages.done_wait += stage_now() - stage_tail_end;
#endif
    me_slices++;
    if (me_slices <= 3)
        SWDEC_LOG_TO(me_log, "swdec-me: slice %u done in %u us, status %d, prod=%d cons=%d\n", me_slices, el, (int) b->status, pp[0], pp[1]);
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
#if defined(SWDEC_ME_DIAG) || defined(SWDEC_ME_DIAG2)
static void me_diag_audio_state(unsigned *aprod, unsigned *acons);
#endif
#ifdef SWDEC_SPIN_DIAG
/* Called from the CPU decode worker when one of the pipeline's spin-waits on
   the ME has lasted over 2 s (diagnostic builds only; once per site). */
static unsigned spin_now_us(void) { return sceKernelGetSystemTimeLow(); }
static void spin_report(int site, int prod, int cons, int dprod, int dcons)
{
    unsigned cse = 0, epc = 0;
    int f = swdec_me_fault(&cse, &epc);
    SwdecMeBox *b = UNC(&me_box_storage);
    SWDEC_LOG_TO(me_log, "swdec-me: SPIN site=%s >2s at slice %u: prod=%d cons=%d dprod=%d dcons=%d box done=%u fault=%d cause=%08x epc=%08x\n",
                       site == 1 ? "deblock-publish" : site == 2 ? "deblock-flush" : "other",
                       me_slices, prod, cons, dprod, dcons, b->done, f, cse, epc);
}
unsigned (*swdec_spin_now_us)(void) = spin_now_us;
void (*swdec_spin_report)(int site, int prod, int cons, int dprod, int dcons) = spin_report;
#endif
#ifdef SWDEC_ME_DIAG2
/* One state sample, appended straight to host0:/swdec-diag.txt with
   open/write/close: no browser log, no shared semaphore, nothing buffered. */
static void me_diag2_sample(const char *tag)
{
    char line[640];
    SwdecMeBox *b = UNC(&me_box_storage);
    volatile SwdecMeDiag *d = UNC(&me_diag_storage);
    volatile int *pp = (volatile int *) UNC(swdec_pipe_get());
    unsigned cause = 0, epc = 0, aprod = 0, acons = 0;
    int fault = me_attached ? swdec_me_fault(&cause, &epc) : 0;
    me_diag_audio_state(&aprod, &acons);
    int n = snprintf(line, sizeof line,
        "%s t=%u beat=%u cpu-stage=%u spins=%u slices=%u dispatching=%d "
        "box cmd=%08x done=%u hb=%u me idle=%u stage=%u cmd-seen=%u "
        "audio-frames=%u audio=%u/%u pipe prod=%d cons=%d dprod=%d dcons=%d "
        "parse-done=%d fault=%d cause=%08x epc=%08x\n",
        tag, (unsigned) sceKernelGetSystemTimeLow(), swdec_me_beat,
        cpu_diag_stage, cpu_diag_spins, me_slices, me_dispatching,
        b->cmd, b->done, b->heartbeat, d->idle, d->stage, d->cmd_seen,
        d->audio_frames, aprod, acons, pp[0], pp[1], pp[15], pp[16], pp[13],
        fault, cause, epc);
    if (n <= 0) return;
    if ((unsigned) n >= sizeof line) n = (int) sizeof line - 1;
    if (cpu_diag_thread >= 0 && (unsigned) n < sizeof line - 1) {
        SceKernelThreadRunStatus rs;
        memset(&rs, 0, sizeof rs);
        rs.size = sizeof rs;
        int got = sceKernelReferThreadRunStatus(cpu_diag_thread, &rs);
        int m = snprintf(line + n - 1, sizeof line - (size_t) n + 1,
            " worker status=%d(%08x) prio=%d wait-type=%d wait-id=%08x run-clocks=%u preempts=%u\n",
            rs.status, (unsigned) got, rs.currentPriority, rs.waitType,
            (unsigned) rs.waitId, (unsigned) rs.runClocks.low,
            (unsigned) rs.threadPreemptCount);
        if (m > 0) n = n - 1 + m;
        if ((unsigned) n >= sizeof line) n = (int) sizeof line - 1;
    }
    SceUID fd = sceIoOpen("host0:/swdec-diag.txt",
                          PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0644);
    if (fd < 0) return;
    (void) sceIoWrite(fd, line, (SceSize) n);
    (void) sceIoClose(fd);
}
#endif

static int watchdog_thread(SceSize args, void *argp)
{
    (void) args; (void) argp;
    unsigned last = 0, still = 0;
#ifdef SWDEC_ME_DIAG2
    unsigned samples = 0;
    me_diag2_sample("swdec-diag: watchdog-start");
#endif
    while (watchdog_on) {
        sceKernelDelayThread(500 * 1000);
        unsigned now = swdec_me_beat;
        if (!watchdog_on) break;
#ifdef SWDEC_ME_DIAG2
        /* every 500 ms for the first 40 samples, then every 10 s; this build
           never logs through the browser and never resets the ME */
        ++samples;
        if (samples <= 40u || samples % 20u == 0)
            me_diag2_sample("swdec-diag: sample");
        continue;
#endif
        still = (now == last) ? still + 1 : 0;
        last = now;
#if SWDEC_DIAG_ENABLED
        {
            static unsigned tick;
            if ((++tick % 20u) == 0)   /* liveness every ~10 s: silence is never ambiguous */
                me_say("swdec-live: beat=%u slices=%u\n", now, me_slices);
        }
#endif
        if (still == 12 && me_attached && !me_dead && me_dispatching) {   /* ~6 s without progress during a dispatch */
#if SWDEC_DIAG_ENABLED
            SwdecMeBox *b = UNC(&me_box_storage);
            volatile int *pp = (volatile int *) UNC(swdec_pipe_get());
            unsigned cse, epc;
            int f = swdec_me_fault(&cse, &epc);
            me_say("swdec-me: WATCHDOG: stalled at slice %u: box cmd=%08x done=%u status=%d hb=%u pipe prod=%d cons=%d fault=%d cause=%08x epc=%08x\n",
                               me_slices, b->cmd, b->done, (int) b->status, b->heartbeat, pp[0], pp[1], f, cse, epc);
#endif
#ifdef SWDEC_ME_DIAG
            /* Diagnostic build: report, give the log time to reach host0, and
               leave the ME running. The CPU worker stays parked in its wait;
               resetting the ME here is the suspect for the device lock-up. */
            if (me_log) {
                unsigned aprod, acons, untouched = 0;
                const volatile unsigned *words = UNC(me_stack);
                me_diag_audio_state(&aprod, &acons);
                while (untouched < ME_STACK_BYTES / sizeof(unsigned)
                       && words[untouched] == ME_STACK_FILL) untouched++;
                me_say("swdec-me: WATCHDOG-DIAG: audio aprod=%u acons=%u stack high-water %u/%u bytes - not stopping the ME\n",
                       aprod, acons,
                       ME_STACK_BYTES - untouched * (unsigned) sizeof(unsigned), ME_STACK_BYTES);
            }
            sceKernelDelayThread(1000 * 1000);
            me_dispatching = 0;   /* report once */
#else
            me_fail("watchdog stall");
#endif
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
static union {
    SwdecAudioQ q;
    unsigned char lines[SWDEC_SHARED_LINES(sizeof(SwdecAudioQ))];
} audioq_box __attribute__((aligned(64)));
#define audioq_storage (audioq_box.q)
_Static_assert(sizeof(audioq_box) % 64u == 0, "audio queue must fill whole cache lines");
static swdec_audio *me_audio;                     /* opened by the CPU, used only by the ME */
#if SWDEC_DIAG_ENABLED
/* Validation: a CRC32 of every decoded block, compared offline with a host
   decode. 4 bytes a sample per channel through a table walk on the ME:
   ~353 KB/s of 44.1 kHz stereo, so release builds leave it out. */
static unsigned me_acrc_tab[256];
#endif
/* playback ring: interleaved S16 stereo, one AAC frame per slot; the ME writes
   through the uncached alias so the CPU audio thread can read a slot the moment
   acons covers it (no writeback ordering to wait for). It lives in the aux
   arena the browser binds for audio (512 KiB) rather than in the module image,
   so it exists exactly from audio setup to audio shutdown. */
typedef short SwdecPcmSlot[2048];
/* Both cores read the ring pointer, so it sits alone in a cache line and is
   only ever accessed through the uncached alias. */
static struct { SwdecPcmSlot *volatile ring; unsigned pad[15]; }
    me_pcm_ring_box __attribute__((aligned(64)));
#define ME_PCM_RING (((volatile typeof(me_pcm_ring_box) *) UNC(&me_pcm_ring_box))->ring)
static const short me_pcm_silence[2048] __attribute__((aligned(64)));
#if SWDEC_DIAG_ENABLED
static volatile int me_poison_flag __attribute__((aligned(64)));
#endif

#if SWDEC_DIAG_ENABLED
static unsigned me_acrc(unsigned crc, const unsigned char *p, unsigned n)
{
    crc = ~crc;
    while (n--) crc = me_acrc_tab[(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}
#endif

/* runs on the ME from the pipeline's idle hooks */
/* ME-only: consecutive failed frames, and decoder flushes made for them. */
static unsigned me_audio_error_run, me_audio_flushes;
static void me_audio_service(void)
{
    SwdecAudioQ *q = UNC(&audioq_storage);
    int decoded_any = 0;
#if SWDEC_DIAG_ENABLED
    {   /* fault-injection test: fault here, with audio state live. The
           target is read through a volatile pointer so the compiler neither
           folds nor warns about the deliberately invalid store. */
        volatile int *poison = UNC(&me_poison_flag);
        if (*poison) {
            volatile int *volatile target = (volatile int *) 5;
            *poison = 0;
            *target = 0x0DEAD;
        }
    }
#endif
#ifdef SWDEC_STAGE_DIAG
    unsigned stage_phase = me_acct.phase;
    if (q->acons < q->aprod) me_phase_enter(ME_PHASE_AUDIO);
#endif
    /* A bounded batch per visit: after a seek or a network burst dozens of
       AAC frames can be queued, and draining them all at one idle point
       held the next video row (and the CPU waiting on it) for tens of
       milliseconds. Recheck after every packet: even four packets can
       delay video once the CPU catches up. Every visit still completes
       at least one packet, including every published row, so audio keeps
       progressing even when video is continuously waiting. */
    volatile SwdecMeBox *box = UNC(&me_box_storage);
    volatile int *pipe = UNC(swdec_pipe_get());
    for (unsigned batch = 0; q->acons < q->aprod; batch++) {
        if (!swdec_audio_service_continue(batch,
                box->cmd == SWDEC_ME_CMD_SLICE && pipe[0] <= pipe[1]))
            break;
        decoded_any = 1;
        unsigned i = q->acons & (AQ - 1);
        swdec_audio_frame out;
        ME_DIAG_SET(stage, 6u);
        int r = swdec_audio_decode(me_audio, (const unsigned char *) q->in[i].ptr, q->in[i].len, &out);
        ME_DIAG_INC(audio_frames);
        ME_DIAG_SET(stage, 7u);
        if (r == 1 && !swdec_audio_channels_admitted(out.channels)) r = -5;
        if (r == 1) {
            unsigned crc = 0;   /* 0 in release builds: nothing reads it */
#if SWDEC_DIAG_ENABLED && !defined(SWDEC_WAIT_DIAG)
            for (int ch = 0; ch < out.channels; ch++)
                crc = me_acrc(crc, (const unsigned char *) out.plane[ch], (unsigned) out.nb_samples * 4);
#endif
            {   /* interleave S16 stereo into the uncached playback ring (mono duplicated) */
                unsigned *dst = (unsigned *) UNC(ME_PCM_RING[i]);
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
            me_audio_error_run = 0;
        } else {
            /* A frame that failed to decode plays as silence, not as
               whatever this slot held from an earlier lap of the ring. */
            unsigned *dst = (unsigned *) UNC(ME_PCM_RING[i]);
            for (int sample = 0; sample < 1024; sample++) dst[sample] = 0;
            q->out[i].crc = 0xFFFFFFFFu; q->out[i].samples = (unsigned) r;   /* error code: the backend plays the silence */
            q->errors = q->errors + 1;
            /* A run of failures: start the decoder afresh (ADTS frames
               carry their own configuration, so the next one resyncs). */
            if (++me_audio_error_run == SWDEC_AUDIO_RESET_RUN) {
                swdec_audio_flush(me_audio);
                me_audio_error_run = 0;
                me_audio_flushes++;
            }
        }
        __asm__ volatile ("sync" ::: "memory");
        q->acons = q->acons + 1;
    }
    /* the decoder's internal state (arena metadata, frame refs) must reach RAM:
       a reset discards dirty ME lines, and a rebooted ME would otherwise resume
       from a half-old aux arena and unref stale buffers (AdEL on first decode) */
    if (decoded_any) {
        me_dcache_writeback_all();
    }
#ifdef SWDEC_STAGE_DIAG
    if (stage_phase != me_acct.phase) me_phase_enter(stage_phase);
#endif
}

/* CPU side */
int swdec_me_audio_setup(void *arena_unused)
{
    (void) arena_unused;
    if (me_audio) {
        *(void (**)(void)) UNC(&swdec_me_aux) = me_audio_service;
        return 0;   /* reinstall the hook (audio_shutdown clears it) */
    }
#if SWDEC_DIAG_ENABLED
    for (unsigned i = 0; i < 256; i++) {
        unsigned cc = i;
        for (int k = 0; k < 8; k++) cc = (cc & 1u) ? 0xEDB88320u ^ (cc >> 1) : cc >> 1;
        me_acrc_tab[i] = cc;
    }
#endif
    /* the audio decoder lives in its own arena: the main one is not
       cross-core-safe (the ME allocates per audio frame while the CPU
       allocates for video) */
    swdec_arena_aux_sp_range(me_stack, me_stack + sizeof me_stack);
    swdec_arena_route_aux(1);                    /* setup runs on the CPU: route its allocations to the aux arena */
    SwdecPcmSlot *ring = swdec_memalign(64, sizeof(SwdecPcmSlot) * AQ);
    if (ring != NULL) me_audio = swdec_audio_open(NULL, 0);
    if (me_audio == NULL) swdec_free(ring);
    else ME_PCM_RING = ring;
    swdec_arena_route_aux(0);
    me_say("swdec-me: aux arena setup %u/%u bytes%s\n",
           (unsigned) swdec_arena_aux_peak(),
           (unsigned) swdec_arena_aux_bytes(),
           me_audio ? "" : " (decoder open failed)");
    if (!me_audio) return -1;
    SwdecAudioQ *q = UNC(&audioq_storage);
    memset((void *) q, 0, sizeof *q);
    sceKernelDcacheWritebackInvalidateRange(
        swdec_arena_aux_base(), (unsigned) swdec_arena_aux_bytes());
    *(void (**)(void)) UNC(&swdec_me_aux) = me_audio_service;
    return 0;
}
#if SWDEC_DIAG_ENABLED
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
#endif

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
unsigned swdec_me_audio_done(void)
{
    me_flush_deferred();   /* polled by the browser's audio thread */
    return ((SwdecAudioQ *) UNC(&audioq_storage))->acons;
}
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
    unsigned audio_errors = q->errors;
    memset((void *) q, 0, sizeof *q);
    {
        SwdecPcmSlot *ring = ME_PCM_RING;
        ME_PCM_RING = NULL;
        swdec_free(ring);
    }
    /* What the audio arena really needed (the browser sizes it). */
    me_say("swdec-me: aux arena peak %u/%u bytes, audio errors %u, decoder flushes %u\n",
           (unsigned) swdec_arena_aux_peak(),
           (unsigned) swdec_arena_aux_bytes(),
           audio_errors, *(volatile unsigned *) UNC(&me_audio_flushes));
    (void) audio_errors;
    /* ME-written: through the uncached alias (idle now, written back). */
    *(volatile unsigned *) UNC(&me_audio_error_run) = 0;
    *(volatile unsigned *) UNC(&me_audio_flushes) = 0;
    swdec_arena_unbind_aux();
    return 0;
}
#if defined(SWDEC_ME_DIAG) || defined(SWDEC_ME_DIAG2)
static void me_diag_audio_state(unsigned *aprod, unsigned *acons)
{
    SwdecAudioQ *q = UNC(&audioq_storage);
    *aprod = q->aprod;
    *acons = q->acons;
}
#endif
#if SWDEC_DIAG_ENABLED
unsigned swdec_me_audio_us(void) { return ((SwdecAudioQ *) UNC(&audioq_storage))->decode_us; }
unsigned swdec_me_audio_errors(void) { return ((SwdecAudioQ *) UNC(&audioq_storage))->errors; }
#endif
const short *swdec_me_audio_pcm(unsigned idx)
{
    SwdecPcmSlot *ring = ME_PCM_RING;
    /* a late reader racing audio shutdown gets silence, never a stale block */
    if (ring == NULL) return me_pcm_silence;
    return (const short *) UNC(ring[idx & (AQ - 1)]);
}
#if SWDEC_DIAG_ENABLED
void swdec_me_poison_arm(void) { *(volatile int *) UNC(&me_poison_flag) = 1; }
void swdec_me_mark_dead(void) { me_dead = 1; swdec_me_enabled = 0; }
#endif

int swdec_me_attach_path(
    const char *helper_path,
    void (*log)(const char *fmt, ...))
{
    me_log = log;
    if (me_attached) return 0;
    if (helper_path == NULL || helper_path[0] == '\0') return -4;
    {
        unsigned mine = (unsigned) sizeof(H264SliceContext) * 65536u + (unsigned) offsetof(H264SliceContext, swdec_ring) + (unsigned) sizeof(H264Context) % 65536u;
        if (mine != swdec_layout_key()) { SWDEC_LOG_TO(log, "swdec-me: struct layout mismatch with the library (%08x vs %08x) - refusing\n", mine, swdec_layout_key()); return -3; }
    }
#if SWDEC_DIAG_ENABLED
    for (unsigned i = 0; i < ME_STACK_BYTES / sizeof(unsigned); i++)
        ((unsigned *) me_stack)[i] = ME_STACK_FILL;   /* written back below, before boot */
#endif
    me_mod = pspSdkLoadStartModule(
        helper_path, PSP_MEMORY_PARTITION_KERNEL);
    if (me_mod < 0) {
        /* likely already resident from a previous run (the helper is kept loaded:
           kernel-module unload cycling corrupts kernel lists under PSPLink) */
        SWDEC_LOG_TO(log, "swdec-me: meload load returned %08x (assuming resident)\n", (unsigned) me_mod);
        me_mod = -1;
    }
#ifdef SWDEC_ME_OFF
    /* Diagnostic build: never boot the ME. Video decodes on the CPU alone
       (no ME hooks); AAC, which only runs on the ME, is never serviced, so
       use a clip without audio. The helper stays loaded so restore works. */
    me_attached = 1;
    SWDEC_LOG_TO(log, "swdec-me: ME left off (diagnostic build): video decodes on the CPU only\n");
    return 0;
#endif
    SwdecMeBox *b = UNC(&me_box_storage);
    memset((void *) b, 0, sizeof *b);
    swdec_csc_claim_init(CSC_CLAIM);   /* nothing offered to the booting ME */
    csc_quarantine = 0;
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
#ifdef SWDEC_STAGE_DIAG
    swdec_stage_mark       = stage_mark;
#endif
    unsigned stack_top = (((unsigned) me_stack + sizeof me_stack) & 0x1FFFFFFFu) | 0x80000000u;
    sceKernelDcacheWritebackInvalidateAll();
    int r = swdec_me_boot((unsigned) swdec_me_worker, (unsigned) b, stack_top);
    sceKernelDelayThread(20000);
    if (r != 0 || b->heartbeat == 0) { SWDEC_LOG_TO(log, "swdec-me: ME boot failed r=%d heartbeat=%u\n", r, b->heartbeat); return -2; }
    swdec_me_enabled = 1;
    me_attached = 1;
    watchdog_on = 1;
    watchdog_th = sceKernelCreateThread("swdec_me_watchdog", watchdog_thread, 0x10, 0x4000, 0, NULL);   /* higher prio than main */
    if (watchdog_th >= 0) sceKernelStartThread(watchdog_th, 0, NULL);
    SWDEC_LOG_TO(log, "swdec-me: attached, ME heartbeat %u, worker %08x, deblock-on-ME %d\n", b->heartbeat, (unsigned) swdec_me_worker, swdec_me_deblock);
    return 0;
}

#if SWDEC_DIAG_ENABLED
/* Development runner: the helper beside a PSPLink host. */
int swdec_me_attach(void (*log)(const char *fmt, ...))
{
    return swdec_me_attach_path("host0:/swdec-meload.prx", log);
}
#endif

/* Resident runner: bring a failed ME split back to life without reloading the
   module. The kernel helper stays resident; we re-boot the ME and reinstall
   the hooks. */
int swdec_me_recover(void)
{
#if SWDEC_DIAG_ENABLED
    if (!me_attached) return swdec_me_attach(me_log);
#else
    if (!me_attached) return -1;   /* the browser attaches before any decode */
#endif
    if (!me_dead && swdec_me_enabled) return 0;
    SwdecMeBox *b = UNC(&me_box_storage);
    memset((void *) b, 0, sizeof *b);
    swdec_csc_claim_init(CSC_CLAIM);   /* nothing offered to the booting ME */
    csc_quarantine = 0;
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
#ifdef SWDEC_STAGE_DIAG
    swdec_stage_mark       = stage_mark;
#endif
    unsigned stack_top = (((unsigned) me_stack + sizeof me_stack) & 0x1FFFFFFFu) | 0x80000000u;
    sceKernelDcacheWritebackInvalidateAll();
    int r = swdec_me_boot((unsigned) swdec_me_worker, (unsigned) b, stack_top);
    sceKernelDelayThread(20000);
    if (r != 0 || b->heartbeat == 0) { SWDEC_LOG_TO(me_log, "swdec-me: recover boot failed r=%d hb=%u\n", r, b->heartbeat); return -1; }
    me_dead = 0;
    swdec_me_failed = 0;
    swdec_me_enabled = 1;
    SWDEC_LOG_TO(me_log, "swdec-me: recovered (ME rebooted, hooks reinstalled)\n");
    return 0;
}

#ifdef SWDEC_STAGE_DIAG
static void me_occ_stop(void);
#endif
void swdec_me_detach(void)
{
    if (!me_attached) return;
#ifdef SWDEC_STAGE_DIAG
    me_occ_stop();                    /* no alarm may outlive the module */
#endif
    watchdog_on = 0;
    if (watchdog_th >= 0) {           /* the thread must be gone before this module unloads */
        (void) psp_thread_wait_end_bounded(watchdog_th, 2000000u);
        sceKernelDeleteThread(watchdog_th);
        watchdog_th = -1;
    }
    me_flush_deferred();   /* the watchdog is gone: print what it left */
    swdec_me_enabled = 0;
    swdec_me_run_slice = NULL;
    swdec_me_pre_publish = NULL; swdec_cpu_pre_consume = NULL; swdec_cpu_post_consume = NULL;
    swdec_me_deblock = 0; swdec_cpu_flush_row = NULL; swdec_me_post_deblock = NULL;
    swdec_me_csc_row = NULL;
    (void) csc_claim_reclaim();   /* the ME still runs: let its batch end */
#ifdef SWDEC_STAGE_DIAG
    swdec_stage_mark = NULL;
#endif
    {   /* quiesce audio before the reset: wait out any in-flight decode, then
           give the ME time to finish its post-batch cache writeback */
        SwdecAudioQ *q = UNC(&audioq_storage);
        unsigned spins = 0;
        while (q->acons != q->aprod && spins++ < 3000) sceKernelDelayThread(1000);
        sceKernelDelayThread(5000);
    }
    swdec_pipe_set(NULL);
    swdec_me_stop();
#if SWDEC_DIAG_ENABLED
    if (me_log) {
        const volatile unsigned *words = UNC(me_stack);
        unsigned untouched = 0;
        while (untouched < ME_STACK_BYTES / sizeof(unsigned)
               && words[untouched] == ME_STACK_FILL) untouched++;
        me_log("swdec-me: detached after %u slices, %u ms waited, stack high-water %u/%u bytes\n",
               me_slices, me_wait_us / 1000u,
               ME_STACK_BYTES - untouched * (unsigned) sizeof(unsigned), ME_STACK_BYTES);
    }
#endif
    /* the kernel helper stays resident deliberately (3 KB): unload cycling
       under PSPLink corrupts kernel lists; it is idempotent across runs */
    me_mod = -1;
    me_attached = 0;
}

#ifdef SWDEC_STAGE_DIAG
/* ME occupancy by sampling: a kernel alarm on the main CPU reads the ME's
   published phase at jittered intervals (uniform 0.5-2.5 ms, so no
   periodic work in either core aliases with it) while a stream is open.
   Each share is samples/total with a 95 % interval of
   1.96 * sqrt(p (1 - p) / n). The alarm handler runs in interrupt context
   and only reads, counts and draws the next interval. */
#define ME_OCC_MIN_US 500u
#define ME_OCC_SPREAD_US 2001u
static struct {
    SceUID alarm;
    volatile int running;
    unsigned seed;
    unsigned samples[ME_PHASES];
    unsigned total;
    unsigned handler_us;
    unsigned started_us;
} me_occ = { -1, 0, 0, {0}, 0, 0, 0 };
static unsigned me_occ_next(void)
{
    me_occ.seed = me_occ.seed * 1664525u + 1013904223u;
    return ME_OCC_MIN_US + (me_occ.seed >> 8) % ME_OCC_SPREAD_US;
}
static SceUInt me_occ_alarm(void *common)
{
    (void) common;
    unsigned t0 = sceKernelGetSystemTimeLow();
    unsigned phase = ME_PHASE->phase;
    me_occ.samples[phase < ME_PHASES ? phase : ME_PHASE_OFF]++;
    me_occ.total++;
    SceUInt next = me_occ.running ? me_occ_next() : 0u;
    me_occ.handler_us += sceKernelGetSystemTimeLow() - t0;
    return next;
}
static void me_occ_stop(void)
{
    me_occ.running = 0;
    if (me_occ.alarm >= 0) {
        (void) sceKernelCancelAlarm(me_occ.alarm);
        me_occ.alarm = -1;
    }
}
static void me_occ_start(void)
{
    me_occ_stop();
    memset(me_occ.samples, 0, sizeof me_occ.samples);
    me_occ.total = 0;
    me_occ.handler_us = 0;
    me_occ.seed ^= sceKernelGetSystemTimeLow();
    me_occ.started_us = sceKernelGetSystemTimeLow();
    me_occ.running = 1;
    me_occ.alarm = sceKernelSetAlarm(me_occ_next(), me_occ_alarm, NULL);
    if (me_occ.alarm < 0) me_occ.running = 0;
}
/* CPU: a consistent copy of the ME's exclusive tick totals. */
static unsigned long long me_ticks_base[ME_PHASES];
static int me_ticks_base_valid;
static int me_ticks_read(unsigned long long out[ME_PHASES])
{
    volatile SwdecMeTicks *t = ME_TICKS;
    for (unsigned attempt = 0; attempt < 1000u; attempt++) {
        unsigned before = t->seq;
        if (before & 1u) continue;
        for (unsigned i = 0; i < ME_PHASES; i++)
            out[i] = ((unsigned long long) t->hi[i] << 32) | t->lo[i];
        if (t->seq == before) return 0;
    }
    return -1;
}
static const char *const me_phase_names[ME_PHASES] = {
    "idle", "parse", "deblock", "wait", "csc", "audio", "drain", "off"
};
static void me_occ_report(void)
{
    unsigned span = sceKernelGetSystemTimeLow() - me_occ.started_us;
    me_occ_stop();
    unsigned n = me_occ.total;
    if (!me_log) return;
    char line[480];
    int used;
    if (n != 0) {
        used = snprintf(line, sizeof line, "swdec-stages-occupancy: samples=%u", n);
        for (unsigned i = 0; i < ME_PHASES && used > 0
                             && (size_t) used < sizeof line; i++) {
            unsigned share, ci;     /* tenths of a percent */
            swdec_occupancy_share(me_occ.samples[i], n, &share, &ci);
            used += snprintf(line + used, sizeof line - (size_t) used,
                             " %s=%u.%u+-%u.%u%%", me_phase_names[i],
                             share / 10u, share % 10u, ci / 10u, ci % 10u);
        }
        me_log("%s handler=%uus (%u ns/sample, %u ppm of %uus)\n", line,
               me_occ.handler_us,
               (unsigned) ((unsigned long long) me_occ.handler_us * 1000u / n),
               span ? (unsigned) ((unsigned long long) me_occ.handler_us
                                  * 1000000u / span) : 0u, span);
    }
    /* Exclusive ME ticks over the same span, against the sampler: shares
       of the ticks, and of the samples taken while the ME ran (not off). */
    volatile SwdecMeStages *m = ME_STAGES;
    unsigned long long now[ME_PHASES], total = 0;
    if (!me_ticks_base_valid || me_ticks_read(now) != 0) {
        /* No consistent pair of readings: no shares from this span. */
        me_log("swdec-stages-me: clock=%s probe=%u/%u/%u raw=%u span=%uus "
               "ticks discarded (%s)\n",
               m->clock == ME_CLOCK_COUNT ? "count" : "none",
               m->probe_count, m->probe_count_dc, m->probe_count_written,
               m->probe_raw, span,
               me_ticks_base_valid ? "torn read at close"
                                   : "torn read at open");
        return;
    }
    for (unsigned i = 0; i < ME_PHASES; i++) {
        now[i] -= me_ticks_base[i];
        total += now[i];
    }
    unsigned on = n - me_occ.samples[ME_PHASE_OFF];
    used = snprintf(line, sizeof line,
                    "swdec-stages-me: clock=%s probe=%u/%u/%u raw=%u "
                    "span=%uus ticks=%llu ticks-per-us=%u.%u",
                    m->clock == ME_CLOCK_COUNT ? "count" : "none",
                    m->probe_count, m->probe_count_dc,
                    m->probe_count_written, m->probe_raw, span, total,
                    span ? (unsigned) (total / span) : 0u,
                    span ? (unsigned) (total * 10u / span % 10u) : 0u);
    unsigned agree = 0, compared = 0;
    for (unsigned i = 0; i < ME_PHASE_OFF && total != 0 && used > 0
                         && (size_t) used < sizeof line; i++) {
        unsigned tick_share = (unsigned) (now[i] * 1000u / total);
        unsigned share = 0, ci = 0;
        swdec_occupancy_share(me_occ.samples[i], on, &share, &ci);
        unsigned diff = tick_share > share ? tick_share - share
                                           : share - tick_share;
        if (on != 0) {
            compared++;
            if (diff <= ci + 1u) agree++;  /* within the interval (+rounding) */
        }
        used += snprintf(line + used, sizeof line - (size_t) used,
                         " %s=%u.%u%%(%u.%u)", me_phase_names[i],
                         tick_share / 10u, tick_share % 10u,
                         share / 10u, share % 10u);
    }
    /* Not counted: the phase in progress when the stream opened and when
       this report was taken (at most one phase each), and any time the ME
       was held in reset (Count stops). */
    me_log("%s agree=%u/%u\n", line, agree, compared);
}

void swdec_me_stage_reset(void)
{
    me_occ_start();
    memset(&cpu_stages, 0, sizeof cpu_stages);
    cpu_stages.reset_at = stage_now();
    me_ticks_base_valid = me_ticks_read(me_ticks_base) == 0;
    csc_split_pictures = csc_split_me_units = csc_split_cpu_units = 0;
    memset(csc_split_share, 0, sizeof csc_split_share);
    memset(&rowc, 0, sizeof rowc);
    census.acc_n = 0;
    census.aus = census.wall = census.counter = 0;
    wait_trace_count = wait_trace_seen = 0;
}
void swdec_me_stage_au_begin(unsigned *wall, unsigned *run)
{
    *wall = stage_now();
    *run = stage_run_us();
    rowc.intr0 = stage_preempt_intr;
    rowc.thread0 = stage_preempt_thread;
}
void swdec_me_stage_decode_census(int end)
{
    if (end) census_end();
    else {
        wait_before = (SwdecWaitTrace) {
            .wall = cpu_stages.au_wall, .run = cpu_stages.au_run,
            .sleep = cpu_stages.sleep_wall, .sleeps = cpu_stages.sleeps,
            .done = cpu_stages.done_wait, .resume = (unsigned) rowc.delay_sum,
            .productive = rowc.productive, .empty = rowc.empty,
            .intr = rowc.preempt_intr, .thread = rowc.preempt_thread,
            .census = (census.counter & 7u) == 0
        };
        wait_ticks_valid = me_ticks_read(wait_ticks_before) == 0;
        census_begin();
    }
}
void swdec_me_stage_decode_trace(uint64_t pts)
{
    SwdecWaitTrace sample = {
        .pts = pts,
        .wall = cpu_stages.au_wall - wait_before.wall,
        .run = cpu_stages.au_run - wait_before.run,
        .sleep = cpu_stages.sleep_wall - wait_before.sleep,
        .sleeps = cpu_stages.sleeps - wait_before.sleeps,
        .done = cpu_stages.done_wait - wait_before.done,
        .resume = (unsigned) rowc.delay_sum - wait_before.resume,
        .productive = rowc.productive - wait_before.productive,
        .empty = rowc.empty - wait_before.empty,
        .intr = rowc.preempt_intr - wait_before.intr,
        .thread = rowc.preempt_thread - wait_before.thread,
        .census = wait_before.census
    };
    unsigned long long ticks[ME_PHASES];
    /* Ignore the initial nominal Count rate until a full second has
       calibrated it. Completed phase deltas can straddle the AU boundary;
       they identify concurrent ME work, not additive CPU wall buckets. */
    if (wait_ticks_valid && rowc.t_mid - rowc.base_t >= 1000000u
        && rowc.rate_q16 != 0 && me_ticks_read(ticks) == 0) {
        sample.phase_valid = 1;
        for (unsigned i = 0; i < ME_PHASES; i++)
            sample.phase_us[i] = (unsigned) ((ticks[i] - wait_ticks_before[i])
                * 65536u / rowc.rate_q16);
    }
    wait_trace_seen++;
    if (sample.run > sample.wall) return;
    unsigned slot = wait_trace_count;
    if (slot == WAIT_TRACE_MAX) {
        slot = 0;
        for (unsigned i = 1; i < WAIT_TRACE_MAX; i++)
            if (wait_trace[i].wall - wait_trace[i].run <
                wait_trace[slot].wall - wait_trace[slot].run) slot = i;
        if (sample.wall - sample.run <=
            wait_trace[slot].wall - wait_trace[slot].run) return;
    } else wait_trace_count++;
    wait_trace[slot] = sample;
}
void swdec_me_stage_au_end(int kind, unsigned wall, unsigned run)
{
    unsigned dw = stage_now() - wall, dr = stage_run_us() - run;
    if (kind == SWDEC_ME_STAGE_CSC_PICTURE) {
        cpu_stages.csc++; cpu_stages.csc_wall += dw; cpu_stages.csc_run += dr;
    } else if (kind == SWDEC_ME_STAGE_CSC_CLOSE) {
        cpu_stages.closes++; cpu_stages.close_wall += dw; cpu_stages.close_run += dr;
    } else {
        cpu_stages.aus++; cpu_stages.au_wall += dw; cpu_stages.au_run += dr;
        rowc.preempt_intr += stage_preempt_intr - rowc.intr0;
        rowc.preempt_thread += stage_preempt_thread - rowc.thread0;
    }
}
static void me_offcpu_report(void)
{
    const SwdecCpuStages *c = &cpu_stages;
    unsigned aus = c->aus ? c->aus : 1u;
    me_log("swdec-stages-wait: sleep-us=%u sleeps=%u wakes=%u productive=%u "
           "empty=%u early=%u negative=%u resume-mean=%uus resume-max=%uus "
           "resume<50/100/200/400/800/1600/more=%u/%u/%u/%u/%u/%u/%u "
           "pings=%u/%u ticks-per-us=%u.%u preempt-intr=%u preempt-thread=%u "
           "(per decode %u.%u/%u.%u)\n",
           SWDEC_ME_WAIT_SLEEP_US, c->sleeps, rowc.wakes, rowc.productive,
           rowc.empty, rowc.early, rowc.negative,
           rowc.productive ? (unsigned) (rowc.delay_sum / rowc.productive) : 0u,
           rowc.delay_max, rowc.hist[0], rowc.hist[1], rowc.hist[2],
           rowc.hist[3], rowc.hist[4], rowc.hist[5], rowc.hist[6],
           rowc.ok, rowc.tried, rowc.rate_q16 >> 16,
           (unsigned) (((rowc.rate_q16 & 0xFFFFu) * 10u) >> 16),
           rowc.preempt_intr, rowc.preempt_thread,
           rowc.preempt_intr / aus, rowc.preempt_intr * 10u / aus % 10u,
           rowc.preempt_thread / aus, rowc.preempt_thread * 10u / aus % 10u);
    if (census.aus == 0) return;
    /* The busiest threads: mean microseconds per sampled decode with a 95 %
       interval over the samples (1.96 * standard error). */
    char line[480];
    int used = snprintf(line, sizeof line,
                        "swdec-stages-threads: sampled=%u invalid=%u "
                        "wall-per-decode=%uus",
                        census.aus, census.invalid, census.wall / census.aus);
    unsigned long long per_sum = 0;
    for (int i = 0; i < census.acc_n; i++)
        if (census.acc[i].samples != 0)
            per_sum += census.acc[i].us / census.acc[i].samples;
    unsigned char shown_mask[CENSUS_THREADS] = {0};
    for (int shown = 0; shown < 8 && used > 0 && (size_t) used < sizeof line;
         shown++) {
        int best = -1;
        unsigned long long best_mean = 0;
        for (int i = 0; i < census.acc_n; i++) {
            if (shown_mask[i] || census.acc[i].samples == 0) continue;
            unsigned long long mean = census.acc[i].us / census.acc[i].samples;
            if (best < 0 || mean > best_mean) { best = i; best_mean = mean; }
        }
        if (best < 0 || best_mean == 0) break;
        shown_mask[best] = 1;
        unsigned long long n = census.acc[best].samples;
        unsigned long long mean2 = census.acc[best].us2 / n;
        unsigned long long sq = best_mean * best_mean;
        unsigned long long var = mean2 > sq ? mean2 - sq : 0;
        unsigned ci = n > 1
            ? (unsigned) (196ull * swdec_occupancy_isqrt64(var / (n - 1)) / 100ull)
            : 0u;
        used += snprintf(line + used, sizeof line - (size_t) used,
                         " %s=%u+-%u",
                         census.acc[best].name[0] ? census.acc[best].name : "?",
                         (unsigned) best_mean, ci);
    }
    unsigned wall = census.wall / census.aus;
    me_log("%s threads=%d unattributed=%uus\n", line, census.acc_n,
           wall > per_sum ? wall - (unsigned) per_sum : 0u);
}
void swdec_me_stage_report(void)
{
    me_occ_report();
    if (!me_log || cpu_stages.aus == 0) return;
    me_offcpu_report();
    for (unsigned i = 0; i < wait_trace_count; i++) {
        const SwdecWaitTrace *s = &wait_trace[i];
        me_log("swdec-wait-au: au=%llu wall=%u cpu=%u sleep=%u sleeps=%u "
               "tail-wait=%u ready-resume=%u productive=%u empty=%u "
               "intr=%u thread=%u census=%u selected=%u/%u "
               "me-valid=%u parse=%u filter=%u wait=%u csc=%u audio=%u drain=%u\n",
               (unsigned long long) s->pts, s->wall, s->run, s->sleep,
               s->sleeps, s->done, s->resume, s->productive, s->empty,
               s->intr, s->thread, s->census, wait_trace_count, wait_trace_seen,
               s->phase_valid, s->phase_us[ME_PHASE_PARSE],
               s->phase_us[ME_PHASE_DEBLOCK], s->phase_us[ME_PHASE_WAIT],
               s->phase_us[ME_PHASE_CSC], s->phase_us[ME_PHASE_AUDIO],
               s->phase_us[ME_PHASE_DRAIN]);
    }
    const SwdecCpuStages *c = &cpu_stages;
    me_log("swdec-stages: aus=%u au-wall=%uus au-run=%uus slices=%u "
           "slice-wall=%uus sleeps=%u sleep-wall=%uus row-pre=%uus "
           "row-recon=%uus row-border=%uus row-post=%uus tail=%uus "
           "done-wait=%uus csc=%u csc-wall=%uus csc-run=%uus "
           "csc-close=%u close-wall=%uus close-run=%uus claim-wait=%uus\n",
           c->aus, c->au_wall, c->au_run, c->slices, c->slice_wall,
           c->sleeps, c->sleep_wall, c->row_us[0], c->row_us[1],
           c->row_us[2], c->row_us[3], c->tail, c->done_wait, c->csc,
           c->csc_wall, c->csc_run, c->closes, c->close_wall, c->close_run,
           c->claim_wait);
    /* Eight-row units per converted picture: done by the ME before the CPU
       took the slot back, and converted by the CPU; share histogram of the
       ME's part (none, up to 1/4, 1/2, 3/4, less than all, all). */
    unsigned units = csc_split_me_units + csc_split_cpu_units;
    me_log("swdec-stages-csc: pictures=%u me-units=%u cpu-units=%u "
           "me-share=%u%% share=%u/%u/%u/%u/%u/%u claim-waits=%u "
           "claim-timeouts=%u me-batches=%u me-backoffs=%u\n",
           csc_split_pictures, csc_split_me_units, csc_split_cpu_units,
           units ? (unsigned) ((unsigned long long) csc_split_me_units * 100u
                               / units) : 0u,
           csc_split_share[0], csc_split_share[1], csc_split_share[2],
           csc_split_share[3], csc_split_share[4], csc_split_share[5],
           csc_claim_waits, csc_claim_timeouts, CSC_CLAIM->taken,
           CSC_CLAIM->backed);
}
#endif
