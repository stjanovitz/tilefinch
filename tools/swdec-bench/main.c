/* swdec PPSSPP bench (see run-bench.sh). Reads bench.cfg beside the EBOOT:
 *   decode <file.aus> [stages] [refs=N] [mc=ref] [mb=generic] [edge=full]
 *          [job=full] [chroma=split] [prefetch=calls] [chromapred=parser]
 *          [chromapred=partial] [restart=N[,N...]]
 *                                decode, per-frame adler32, CSC A/B per
 *                                frame; the arena is sized for N held
 *                                pictures at 432x240 (default 16), as the
 *                                player sizes it, and its peak reported;
 *                                mc=ref uses the reference motion-
 *                                compensation kernels (swdec_mc_reference),
 *                                mb=generic the generic macroblock path
 *                                (swdec_mb_generic), edge=full the fixed
 *                                edge-emulation regions (swdec_edge_full),
 *                                job=full whole-cache jobs (swdec_job_full),
 *                                chroma=split one chroma plane per call
 *                                (swdec_chroma_split), prefetch=calls the
 *                                no-op prefetch calls (swdec_prefetch_calls),
 *                                chromapred=parser the parser predicts chroma
 *                                as the Media Engine does (swdec_chroma_force),
 *                                chromapred=partial it stops part-way through
 *                                rows and reconstruction predicts the rest
 *                                (swdec_chroma_partial);
 *                                restart=N,M closes the decoder after N
 *                                access units, mid-stream (a seek's or a
 *                                cancel's teardown), reopens it in the same
 *                                arena and decodes from the start again,
 *                                then after M more, and so on; each restart
 *                                prints a "restart" line and frames count
 *                                from 0 again (cmp_crc.py checks every
 *                                segment against the reference's start)
 *   cscx                         every (y, u, v) through both converters
 * and writes tilefinch-validation.txt (the file the PPSSPP runner collects);
 * its last line is the runner's clean-exit sentinel. PPSSPP's clock is an
 * instruction-count proxy (about one cycle per instruction), not device
 * time. */
#include <pspkernel.h>
#include <psputils.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <stdarg.h>

#include "swdec.h"
#include "swdec_csc.h"

PSP_MODULE_INFO("swdecbench", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);

unsigned swdec_stage_us[12];
int swdec_stage_on;
int swdec_probe_norecon;
/* main-core attribution (swdec_recon_diag.c), here on PPSSPP's clock */
#include "swdec_recon_diag.h"
#include "swdec_chroma_replay.h"
static void recon_line(const char *format, ...);
/* Weak: libraries prepared before the reference kernels existed still
   link (and refuse mc=ref). */
extern int swdec_mc_reference __attribute__((weak));
extern int swdec_mb_generic __attribute__((weak));
extern int swdec_edge_full __attribute__((weak));
extern int swdec_job_full __attribute__((weak));
extern int swdec_chroma_split __attribute__((weak));
extern int swdec_prefetch_calls __attribute__((weak));
extern int swdec_chroma_force __attribute__((weak));
extern int swdec_chroma_partial __attribute__((weak));
static const struct {
    const char *token, *name, *on, *off;
    int *flag;
} variants[] = {
    { "mc=ref", "mc", "ref", "new", &swdec_mc_reference },
    { "mb=generic", "mb", "generic", "simple", &swdec_mb_generic },
    { "edge=full", "edge", "full", "exact", &swdec_edge_full },
    { "job=full", "job", "full", "partial", &swdec_job_full },
    { "chroma=split", "chroma", "split", "paired", &swdec_chroma_split },
    { "prefetch=calls", "prefetch", "calls", "skipped", &swdec_prefetch_calls },
    { "chromapred=parser", "chromapred", "parser", "recon", &swdec_chroma_force },
    { "chromapred=partial", "chromapartial", "on", "off", &swdec_chroma_partial },
};
#define VARIANTS (sizeof variants / sizeof variants[0])
unsigned swdec_stage_now(void) { return sceKernelGetSystemTimeLow(); }

static FILE *out;
static void recon_line(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vfprintf(out, format, arguments);
    va_end(arguments);
}
static unsigned long long now_us(void) { return sceKernelGetSystemTimeWide(); }

static unsigned adler_update(unsigned a, const uint8_t *p, int n)
{
    unsigned s1 = a & 0xFFFF, s2 = a >> 16;
    for (int i = 0; i < n; i++) {
        s1 += p[i]; if (s1 >= 65521) s1 -= 65521;
        s2 += s1; if (s2 >= 65521) s2 -= 65521;
    }
    return s1 | (s2 << 16);
}

/* ffmpeg's framecrc: adler32 seeded with 0 over the packed cropped planes */
static unsigned picture_adler(const swdec_picture *pic)
{
    unsigned a = 0;
    for (int r = 0; r < pic->height; r++)
        a = adler_update(a, pic->plane[0] + r * pic->stride[0], pic->width);
    int cw = (pic->width + 1) / 2, ch = (pic->height + 1) / 2;
    for (int k = 1; k < 3; k++)
        for (int r = 0; r < ch; r++)
            a = adler_update(a, pic->plane[k] + r * pic->stride[k], cw);
    return a;
}

static uint16_t *csc_a, *csc_b;
static unsigned long long csc_c_us, csc_v_us;
static unsigned csc_frames, csc_bad;

static void csc_ab(const swdec_picture *pic)
{
    const int stride = 432;
    size_t bytes = (size_t) stride * pic->height * 2;
    memset(csc_a, 0, bytes);
    memset(csc_b, 0, bytes);
    SwdecCscPlanes p = {
        .y = pic->plane[0], .u = pic->plane[1], .v = pic->plane[2],
        .y_stride = pic->stride[0], .uv_stride = pic->stride[1],
        .width = pic->width, .dst = csc_a, .dst_stride = stride
    };
    unsigned long long t0 = now_us();
    swdec_csc_rows_c(&p, 0, pic->height);
    unsigned long long t1 = now_us();
    p.dst = csc_b;
    swdec_csc_rows_vfpu(&p, 0, pic->height);
    unsigned long long t2 = now_us();
    csc_c_us += t1 - t0;
    csc_v_us += t2 - t1;
    csc_frames++;
    if (memcmp(csc_a, csc_b, bytes) != 0) csc_bad++;
}

static void run_decode(const char *path, int stages, int refs, const char *line)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(out, "ERROR open %s\n", path); return; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t) n);
    if (!data || fread(data, 1, (size_t) n, f) != (size_t) n) {
        fprintf(out, "ERROR read %s\n", path); fclose(f); return;
    }
    fclose(f);
    char modes[224] = "";
    for (unsigned v = 0; v < VARIANTS; v++) {
        int on = strstr(line, variants[v].token) != NULL;
        if (variants[v].flag == NULL) {
            if (on) {
                fprintf(out, "ERROR %s: these libraries have no such switch\n",
                        variants[v].token);
                free(data); return;
            }
            continue;
        }
        *variants[v].flag = on;
        size_t used = strlen(modes);
        snprintf(modes + used, sizeof modes - used, " %s=%s", variants[v].name,
                 on ? variants[v].on : variants[v].off);
    }
    size_t arena_bytes = swdec_arena_bytes(432, 240, refs);
    void *arena = memalign(64, arena_bytes);
    swdec *d = arena ? swdec_open(arena, arena_bytes, 432, 240, refs) : NULL;
    if (!d) { fprintf(out, "ERROR open decoder\n"); free(data); free(arena); return; }
    unsigned restart_at[8], restarts = 0, next_restart = 0;
    const char *rs = strstr(line, "restart=");
    for (const char *q = rs ? rs + 8 : NULL; q && restarts < 8; ) {
        char *e;
        unsigned long v = strtoul(q, &e, 10);
        if (e == q || v == 0) break;
        restart_at[restarts++] = (unsigned) v;
        q = *e == ',' ? e + 1 : NULL;
    }
    if (restarts) {
        size_t used = strlen(modes);
        snprintf(modes + used, sizeof modes - used, " restart=");
        for (unsigned i = 0; i < restarts; i++) {
            used = strlen(modes);
            snprintf(modes + used, sizeof modes - used, "%s%u", i ? "," : "", restart_at[i]);
        }
    }
    size_t peak = 0;
    memset(swdec_stage_us, 0, sizeof swdec_stage_us);
    swdec_recon_diag_reset();
    swdec_chroma_replay_enabled = 1;
    swdec_chroma_replay_reset(0);   /* no ME here: the scalar kernels run on this core */
    swdec_stage_on = stages;
    csc_c_us = csc_v_us = 0; csc_frames = csc_bad = 0;
    unsigned long long decode_us = 0;
    unsigned frames = 0, aus = 0, segment_aus = 0, total_frames = 0;
    long at = 0;
    swdec_picture pic;
    fprintf(out, "clip %s stages=%d%s\n", path, stages, modes);
    while (at + 4 <= n) {
        if (next_restart < restarts && segment_aus == restart_at[next_restart]) {
            /* mid-stream teardown as a seek or a cancel does it: no drain */
            swdec_stats st;
            swdec_stats_get(d, &st);
            if (st.peak_bytes > peak) peak = st.peak_bytes;
            swdec_close(d);
            d = swdec_open(arena, arena_bytes, 432, 240, refs);
            if (!d) { fprintf(out, "ERROR reopen decoder\n"); free(data); free(arena); return; }
            fprintf(out, "restart after-aus=%u frames=%u\n", segment_aus, frames);
            next_restart++;
            total_frames += frames;
            frames = 0; segment_aus = 0; at = 0;
            continue;
        }
        unsigned len = data[at] | data[at + 1] << 8 | data[at + 2] << 16 | (unsigned) data[at + 3] << 24;
        at += 4;
        if (at + (long) len > n) break;
        unsigned long long t0 = now_us();
        unsigned run0 = swdec_recon_diag_run_us();
        int rc = swdec_decode(d, data + at, len, aus, &pic);
        decode_us += now_us() - t0;
        swdec_recon_diag_au((unsigned) (now_us() - t0), swdec_recon_diag_run_us() - run0);
        at += len; aus++; segment_aus++;
        if (rc == 1) {
            fprintf(out, "frame %u %08x\n", frames++, picture_adler(&pic));
            if (!stages) csc_ab(&pic);
        } else if (rc < 0) fprintf(out, "decode-error au=%u rc=%d\n", aus - 1, rc);
    }
    for (;;) {
        unsigned long long t0 = now_us();
        int rc = swdec_decode(d, NULL, 0, 0, &pic);
        decode_us += now_us() - t0;
        if (rc != 1) break;
        fprintf(out, "frame %u %08x\n", frames++, picture_adler(&pic));
        if (!stages) csc_ab(&pic);
    }
    swdec_stats st;
    swdec_stats_get(d, &st);
    if (st.peak_bytes > peak) peak = st.peak_bytes;
    total_frames += frames;
    fprintf(out, "summary aus=%u frames=%u decode-us=%llu per-frame-us=%llu "
            "refs=%d arena=%u peak=%u errors=%u\n",
            aus, total_frames, decode_us, total_frames ? decode_us / total_frames : 0, refs,
            (unsigned) st.arena_bytes, (unsigned) peak,
            (unsigned) st.errors);
    swdec_recon_diag_report(total_frames, recon_line);
    swdec_chroma_replay_report(recon_line);
    if (stages) {
        fprintf(out, "stages");
        for (int i = 0; i < 12; i++) fprintf(out, " s%d=%u", i, swdec_stage_us[i]);
        fprintf(out, "\n");
    } else {
        fprintf(out, "csc frames=%u c-us=%llu vfpu-us=%llu c-per-frame=%llu vfpu-per-frame=%llu mismatched-frames=%u\n",
                csc_frames, csc_c_us, csc_v_us,
                csc_frames ? csc_c_us / csc_frames : 0,
                csc_frames ? csc_v_us / csc_frames : 0, csc_bad);
    }
    swdec_close(d);
    free(arena);
    free(data);
    fflush(out);
}

/* Every (y, u, v): one picture per u value, chroma rows walk v, each 2x2
   block of a chroma row takes four consecutive luma values. */
static void run_cscx(void)
{
    const int w = 128, h = 512, cw = 64, ch = 256;
    uint8_t *y = memalign(64, (size_t) w * h);
    uint8_t *u = memalign(64, (size_t) cw * ch);
    uint8_t *v = memalign(64, (size_t) cw * ch);
    for (int r = 0; r < ch; r++)
        for (int c = 0; c < cw; c++) {
            v[r * cw + c] = (uint8_t) r;
            y[(2 * r) * w + 2 * c] = (uint8_t) (4 * c);
            y[(2 * r) * w + 2 * c + 1] = (uint8_t) (4 * c + 1);
            y[(2 * r + 1) * w + 2 * c] = (uint8_t) (4 * c + 2);
            y[(2 * r + 1) * w + 2 * c + 1] = (uint8_t) (4 * c + 3);
        }
    unsigned bad = 0;
    unsigned long long tc = 0, tv = 0;
    for (int uu = 0; uu < 256; uu++) {
        memset(u, uu, (size_t) cw * ch);
        SwdecCscPlanes p = { .y = y, .u = u, .v = v, .y_stride = w,
            .uv_stride = cw, .width = w, .dst = csc_a, .dst_stride = w };
        unsigned long long t0 = now_us();
        swdec_csc_rows_c(&p, 0, h);
        unsigned long long t1 = now_us();
        p.dst = csc_b;
        swdec_csc_rows_vfpu(&p, 0, h);
        tc += t1 - t0; tv += now_us() - t1;
        if (memcmp(csc_a, csc_b, (size_t) w * h * 2) != 0) {
            bad++;
            if (bad < 4)
                for (int i = 0; i < w * h; i++)
                    if (csc_a[i] != csc_b[i]) {
                        fprintf(out, "cscx mismatch u=%d at=%d c=%04x v=%04x\n", uu, i, csc_a[i], csc_b[i]);
                        break;
                    }
        }
    }
    fprintf(out, "cscx pixels=%u mismatched-u=%u c-us=%llu vfpu-us=%llu self-check=%d\n",
            256u * w * h, bad, tc, tv, swdec_csc_vfpu_verified());
    free(y); free(u); free(v);
    fflush(out);
}

int main(int argc, char **argv)
{
    (void) argc; (void) argv;
    out = fopen("tilefinch-validation.txt", "w");
    if (!out) sceKernelExitGame();
    csc_a = memalign(64, 432 * 512 * 2);
    csc_b = memalign(64, 432 * 512 * 2);
    FILE *cfg = fopen("bench.cfg", "r");
    char line[256];
    while (cfg && fgets(line, sizeof line, cfg)) {
        char a[128] = "", b[128] = "";
        int k = sscanf(line, "%127s %127s", a, b);
        if (k >= 1 && strcmp(a, "cscx") == 0) run_cscx();
        else if (k >= 2 && strcmp(a, "decode") == 0) {
            const char *r = strstr(line, "refs=");
            int refs = r != NULL ? atoi(r + 5) : 16;
            if (refs < 1 || refs > 16) refs = 16;
            run_decode(b, strstr(line, "stages") != NULL, refs, line);
        }
    }
    if (cfg) fclose(cfg);
    fprintf(out, "DONE\ntilefinch-validation: outcome=clean-exit\n");
    fclose(out);
    sceKernelExitGame();
    return 0;
}
