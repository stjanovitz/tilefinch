/* Validation builds only (SWDEC_DIAGNOSTICS): where the software decoder's
 * decode thread spends its main-core time, reported per output picture when
 * a stream closes. See libavcodec/swdec_recon_stats.h.
 *
 * The PSP's user mode has no usable cycle counter, so everything is timed
 * with the 1 MHz system clock: every macroblock row's phases (five reads a
 * row), and the kernels of one macroblock in SWDEC_RT_SAMPLE, scaled up by
 * all macroblocks over sampled ones. One clock read's own cost, measured at
 * reset, is taken off each timed interval. The decode calls' wall time
 * against the thread's run time gives the time it spent off the CPU. */
#include "swdec_recon_diag.h"

#if SWDEC_DIAG_ENABLED
#include <pspkernel.h>
#include <stdint.h>
#include <string.h>

#include "libavcodec/swdec_recon_stats.h"
#include "swdec_chroma_replay.h"
#include "libavcodec/swdec_pipe.h"

_Static_assert(SWDEC_RT_COUNT == 11,
               "swdec_recon_diag_report lists 11 kernel buckets");

enum { RECON_PRE, RECON_RECON, RECON_BORDER, RECON_POST, RECON_TAIL,
       RECON_FLUSH, RECON_FLUSH_TAIL, RECON_SPIN, RECON_SPIN_TAIL,
       RECON_PHASES };
static struct {
    unsigned aus, marks, t_phase, t_tail, t_nested, in_tail;
    unsigned read_ns;                 /* one clock read */
    uint64_t wall_us, run_us;
    uint64_t phase_us[RECON_PHASES];
    uint64_t kernel_us[SWDEC_RT_COUNT], kernel_calls[SWDEC_RT_COUNT];
    uint64_t mbs, mbs_sampled;
    uint64_t qpel[3][16], qpel_avg;
} recon;

static unsigned recon_now(void) { return sceKernelGetSystemTimeLow(); }

unsigned swdec_recon_diag_run_us(void)
{
    SceKernelThreadRunStatus status;
    memset(&status, 0, sizeof(status));
    status.size = sizeof(status);
    if (sceKernelReferThreadRunStatus(sceKernelGetThreadId(), &status) < 0)
        return 0;
    return (unsigned) status.runClocks.low;
}

static void recon_mark(int mark)
{
    unsigned now = recon_now();
    recon.marks++;
    switch (mark) {
    case SWDEC_RMARK_ROW:
        swdec_chroma_replay_row_begin();   /* validation replay (its own time) */
        recon.t_phase = recon_now();
        break;
    case SWDEC_RMARK_RECON:
    case SWDEC_RMARK_BORDER:
    case SWDEC_RMARK_POST:
    case SWDEC_RMARK_ROW_END:
        /* each mark ends the phase the previous one began */
        recon.phase_us[mark == SWDEC_RMARK_RECON ? RECON_PRE
                       : mark == SWDEC_RMARK_BORDER ? RECON_RECON
                       : mark == SWDEC_RMARK_POST ? RECON_BORDER
                       : RECON_POST] += now - recon.t_phase;
        recon.t_phase = now;
        if (mark == SWDEC_RMARK_ROW_END) swdec_chroma_replay_row_end();
        break;
    case SWDEC_RMARK_FLUSH: case SWDEC_RMARK_SPIN:
        recon.t_nested = now;
        break;
    case SWDEC_RMARK_FLUSH_END:
        recon.phase_us[recon.in_tail ? RECON_FLUSH_TAIL : RECON_FLUSH] +=
            now - recon.t_nested;
        break;
    case SWDEC_RMARK_SPIN_END:
        recon.phase_us[recon.in_tail ? RECON_SPIN_TAIL : RECON_SPIN] +=
            now - recon.t_nested;
        break;
    case SWDEC_RMARK_TAIL: recon.t_tail = now; recon.in_tail = 1; break;
    case SWDEC_RMARK_TAIL_END:
        recon.phase_us[RECON_TAIL] += now - recon.t_tail;
        recon.in_tail = 0;
        break;
    }
}

/* Move the decoder's sample counters into the stream totals. */
static void recon_fold(void)
{
    for (int i = 0; i < SWDEC_RT_COUNT; i++) {
        recon.kernel_us[i] += swdec_rt_us[i];
        recon.kernel_calls[i] += swdec_rt_calls[i];
    }
    recon.mbs += swdec_rt_mbs[0];
    recon.mbs_sampled += swdec_rt_mbs[1];
    for (int s = 0; s < 3; s++)
        for (int p = 0; p < 16; p++) recon.qpel[s][p] += swdec_qpel_calls[s][p];
    recon.qpel_avg += swdec_qpel_avg;
    memset(swdec_qpel_calls, 0, sizeof(swdec_qpel_calls));
    swdec_qpel_avg = 0;
    memset(swdec_rt_us, 0, sizeof(swdec_rt_us));
    memset(swdec_rt_calls, 0, sizeof(swdec_rt_calls));
    memset(swdec_rt_mbs, 0, sizeof(swdec_rt_mbs));
}

void swdec_recon_diag_reset(void)
{
    memset(&recon, 0, sizeof(recon));
    recon_fold();
    memset(&recon, 0, sizeof(recon));
    unsigned start = recon_now(), reads = 1024;
    for (unsigned i = 0; i < reads; i++) (void) recon_now();
    recon.read_ns = (unsigned) ((uint64_t) (recon_now() - start) * 1000u / reads);
    swdec_rt_now = recon_now;
    memset(swdec_chroma_stats, 0, sizeof(swdec_chroma_stats));
    swdec_recon_mark = recon_mark;
#ifdef SWDEC_WAIT_DIAG
    swdec_recon_mark = NULL;
#endif
}

void swdec_recon_diag_au(unsigned wall_us, unsigned run_us)
{
    recon.wall_us += wall_us;
    recon.run_us += run_us;
    recon.aus++;
    recon_fold();
}

void swdec_recon_diag_report(unsigned pictures,
                             void (*log)(const char *format, ...))
{
    swdec_recon_mark = NULL;
    swdec_rt_on = 0;
    recon_fold();
    if (log == NULL || pictures == 0 || recon.aus == 0) return;
    const uint64_t *p = recon.phase_us;
#define PER_PICTURE(v) ((unsigned) ((uint64_t) (v) / pictures))
    uint64_t rows_us = p[RECON_PRE] + p[RECON_RECON] + p[RECON_BORDER]
        + p[RECON_POST] + p[RECON_TAIL];
    log("swdec-recon-cpu: pictures=%u aus=%u clock-read-ns=%u "
        "per-picture-us wall=%u cpu=%u off-cpu=%u rows=%u cache-pre=%u "
        "recon=%u border=%u cache-post=%u flush=%u spin=%u tail-deblock=%u "
        "wall-not-in-rows=%d cpu-minus-rows=%d\n",
        pictures, recon.aus, recon.read_ns,
        PER_PICTURE(recon.wall_us), PER_PICTURE(recon.run_us),
        PER_PICTURE(recon.wall_us - recon.run_us), PER_PICTURE(rows_us),
        PER_PICTURE(p[RECON_PRE]), PER_PICTURE(p[RECON_RECON]),
        PER_PICTURE(p[RECON_BORDER] - p[RECON_FLUSH] - p[RECON_SPIN]),
        PER_PICTURE(p[RECON_POST]),
        PER_PICTURE(p[RECON_FLUSH] + p[RECON_FLUSH_TAIL]),
        PER_PICTURE(p[RECON_SPIN] + p[RECON_SPIN_TAIL]),
        PER_PICTURE(p[RECON_TAIL] - p[RECON_FLUSH_TAIL] - p[RECON_SPIN_TAIL]),
        /* signed and unclamped, so a shortfall shows: the rows are wall
           time, so CPU minus rows goes negative when rows were interrupted */
        (int) (((int64_t) recon.wall_us - (int64_t) rows_us) / (int64_t) pictures),
        (int) (((int64_t) recon.run_us - (int64_t) rows_us) / (int64_t) pictures));
    /* inter macroblocks whose chroma the parser (Media Engine) predicted,
       ones the main core predicted, macroblocks it waited for part-way and
       its wait loop iterations in all, and rows it claimed the rest of */
    log("swdec-chroma-me: mbs-from-parser=%u mbs-on-cpu=%u mbs-waited=%u wait-spins=%u rows-claimed=%u\n",
        swdec_chroma_stats[0], swdec_chroma_stats[1], swdec_chroma_stats[2],
        swdec_chroma_stats[3], swdec_chroma_stats[4]);
    if (recon.mbs_sampled == 0) return;
    /* Sampled kernel time less one clock read per interval, scaled to all
       macroblocks; a macroblock's own interval also holds its kernels'
       reads (two each). */
    /* Each interval also holds the clock reads of the intervals nested in
       it (two each): MOTION holds the MC/weight/edge ones, MB all others. */
    const uint64_t *calls = recon.kernel_calls;
    uint64_t in_motion = calls[SWDEC_RT_LUMA_FULL] + calls[SWDEC_RT_LUMA_1D]
        + calls[SWDEC_RT_LUMA_2D] + calls[SWDEC_RT_CHROMA]
        + calls[SWDEC_RT_WEIGHT] + calls[SWDEC_RT_EDGE];
    uint64_t in_mb = in_motion + calls[SWDEC_RT_IDCT] + calls[SWDEC_RT_INTRA]
        + calls[SWDEC_RT_MOTION] + calls[SWDEC_RT_RESTORE];
    unsigned v[SWDEC_RT_COUNT];
    for (int i = 0; i < SWDEC_RT_COUNT; i++) {
        uint64_t reads = calls[i] + (i == SWDEC_RT_MOTION ? 2 * in_motion
                                     : i == SWDEC_RT_MB ? 2 * in_mb : 0);
        uint64_t ns = recon.kernel_us[i] * 1000u;
        uint64_t bias = reads * recon.read_ns;
        ns = ns > bias ? ns - bias : 0;
        v[i] = (unsigned) (ns * recon.mbs / recon.mbs_sampled / 1000u / pictures);
    }
    unsigned mc = v[SWDEC_RT_LUMA_FULL] + v[SWDEC_RT_LUMA_1D]
        + v[SWDEC_RT_LUMA_2D] + v[SWDEC_RT_CHROMA] + v[SWDEC_RT_WEIGHT]
        + v[SWDEC_RT_EDGE];
    unsigned parts = v[SWDEC_RT_MOTION] + v[SWDEC_RT_IDCT]
        + v[SWDEC_RT_INTRA] + v[SWDEC_RT_RESTORE];
    log("swdec-recon-kernels: sampled-mbs=%u/%u per-picture-us mb=%u "
        "luma-full=%u luma-1d=%u luma-2d=%u chroma=%u weight=%u edge=%u "
        "motion-other=%d idct-add=%u intra=%u restore=%u mb-other=%d "
        "sampling-cost=%u\n",
        (unsigned) recon.mbs_sampled, (unsigned) recon.mbs, v[SWDEC_RT_MB],
        v[SWDEC_RT_LUMA_FULL], v[SWDEC_RT_LUMA_1D], v[SWDEC_RT_LUMA_2D],
        v[SWDEC_RT_CHROMA], v[SWDEC_RT_WEIGHT], v[SWDEC_RT_EDGE],
        (int) v[SWDEC_RT_MOTION] - (int) mc,
        v[SWDEC_RT_IDCT], v[SWDEC_RT_INTRA], v[SWDEC_RT_RESTORE],
        (int) v[SWDEC_RT_MB] - (int) parts,
        PER_PICTURE((uint64_t) (recon.marks + 2 * (in_mb
            + calls[SWDEC_RT_MB])) * recon.read_ns / 1000u));
    /* luma MC calls per 100 pictures, by size and position x + 4y */
    for (int size = 0; size < 3; size++) {
        unsigned c[16];
        for (int i = 0; i < 16; i++)
            c[i] = (unsigned) (recon.qpel[size][i] * 100u / pictures);
        log("swdec-recon-qpel: size=%d per-100-pictures avg-all=%u "
            "%u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u\n",
            16 >> size, (unsigned) (recon.qpel_avg * 100u / pictures),
            c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7],
            c[8], c[9], c[10], c[11], c[12], c[13], c[14], c[15]);
    }
#undef PER_PICTURE
}
#endif
