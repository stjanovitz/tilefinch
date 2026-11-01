#include "tilefinch/swdec_component.h"

#include <pspkernel.h>
#include <string.h>

#include "swdec.h"
#include "swdec_arena.h"
#include "swdec_diag.h"
#include "swdec_me.h"
#if SWDEC_DIAG_ENABLED
#include "libavcodec/swdec_pipe.h"
#include "swdec_recon_diag.h"
#include "swdec_chroma_replay.h"
#endif

PSP_MODULE_INFO("tilefinch_swdec", 0, 1, 0);

/* The PRX links without the PSPSDK C start-up. That start-up would run main()
 * on a new thread with the start block parsed as strings, leave through
 * sceKernelExitGame (ending the browser), and claim a second newlib heap from
 * the partition. module_start below reads the block directly instead.
 *
 * newlib still allocates a little for its own locks and conversions. The
 * decoder arenas are rebound per stream, so those long-lived allocations come
 * from a small fixed pool here and are never returned. */
extern void __locks_init(void), __locks_deinit(void);
extern void __init_mutex(void), __deinit_mutex(void), __fdman_init(void);

#define LIBC_POOL_BYTES (16u * 1024u)
static unsigned char libc_pool[LIBC_POOL_BYTES] __attribute__((aligned(64)));
static size_t libc_pool_used;

static void *libc_pool_alloc(size_t align, size_t bytes)
{
    if (align < 16u) align = 16u;
    if (align > 64u || bytes > LIBC_POOL_BYTES) return NULL;
    int interrupts = sceKernelCpuSuspendIntr();
    size_t start = (libc_pool_used + sizeof(size_t) + align - 1u)
        & ~(align - 1u);
    void *pointer = NULL;
    if (start <= LIBC_POOL_BYTES && bytes <= LIBC_POOL_BYTES - start) {
        memcpy(libc_pool + start - sizeof(size_t), &bytes, sizeof(size_t));
        pointer = libc_pool + start;
        libc_pool_used = start + bytes;
    }
    sceKernelCpuResumeIntr(interrupts);
    return pointer;
}

void *__wrap_malloc(size_t bytes);
void *__wrap_memalign(size_t align, size_t bytes);
void *__wrap_calloc(size_t count, size_t size);
void *__wrap_realloc(void *pointer, size_t bytes);
void __wrap_free(void *pointer);
struct _reent;
void *__wrap__malloc_r(struct _reent *r, size_t bytes);
void *__wrap__calloc_r(struct _reent *r, size_t count, size_t size);
void *__wrap__realloc_r(struct _reent *r, void *pointer, size_t bytes);
void __wrap__free_r(struct _reent *r, void *pointer);
void *__wrap__memalign_r(struct _reent *r, size_t align, size_t bytes);

void *__wrap_malloc(size_t bytes) { return libc_pool_alloc(16u, bytes); }
void *__wrap_memalign(size_t align, size_t bytes)
{
    return libc_pool_alloc(align, bytes);
}
void *__wrap_calloc(size_t count, size_t size)
{
    if (size != 0 && count > (size_t) -1 / size) return NULL;
    void *pointer = libc_pool_alloc(16u, count * size);
    if (pointer != NULL) memset(pointer, 0, count * size);
    return pointer;
}
void *__wrap_realloc(void *pointer, size_t bytes)
{
    if (pointer == NULL) return __wrap_malloc(bytes);
    size_t old;
    memcpy(&old, (unsigned char *) pointer - sizeof(size_t), sizeof(size_t));
    if (bytes <= old) return pointer;
    void *grown = __wrap_malloc(bytes);
    if (grown != NULL) memcpy(grown, pointer, old);
    return grown;
}
void __wrap_free(void *pointer) { (void) pointer; }
void *__wrap__malloc_r(struct _reent *r, size_t bytes)
{
    (void) r;
    return __wrap_malloc(bytes);
}
void *__wrap__calloc_r(struct _reent *r, size_t count, size_t size)
{
    (void) r;
    return __wrap_calloc(count, size);
}
void *__wrap__realloc_r(struct _reent *r, void *pointer, size_t bytes)
{
    (void) r;
    return __wrap_realloc(pointer, bytes);
}
void __wrap__free_r(struct _reent *r, void *pointer)
{
    (void) r;
    (void) pointer;
}
void *__wrap__memalign_r(struct _reent *r, size_t align, size_t bytes)
{
    (void) r;
    return __wrap_memalign(align, bytes);
}

/* Nothing in the decoder exits; if newlib ever does, stop here rather than
   ending the browser. */
void _exit(int status)
{
    (void) status;
    __builtin_trap();
}

#if SWDEC_DIAG_ENABLED
/* The diagnostic FFmpeg build (SWDEC_STAGE_TIMING) has per-stage attribution
 * hooks, present but disabled until a runner switches them on. The release
 * build has none, and linking a release component against diagnostic
 * libraries fails on these symbols rather than shipping the hooks. */
unsigned swdec_stage_us[12];
int swdec_stage_on;
int swdec_probe_norecon;
unsigned swdec_stage_now(void) { return 0; }
#endif

_Static_assert(sizeof(TilefinchSwdecPicture) == sizeof(swdec_picture),
               "swdec picture ABI drift");
_Static_assert(sizeof(TilefinchSwdecStats) == sizeof(swdec_stats),
               "swdec statistics ABI drift");
_Static_assert(TILEFINCH_SWDEC_CSC_SLOT_COUNT == SWDEC_ME_CSC_SLOTS,
               "swdec CSC slot ABI drift");

/* The CPU-side reconstruction, motion compensation and tail deblocking use
 * VFPU kernels. The PSP raises a coprocessor-unusable exception for VFPU
 * instructions in a thread created without PSP_THREAD_ATTR_VFPU, and the
 * browser's decode worker is created without it: on a PSP-3000 that thread
 * stopped at its first macroblock while the ME waited for it. Grant the
 * attribute to whichever thread calls in; it is idempotent and cheap next to
 * an access unit. */
static int component_enable_vfpu(void)
{
    return sceKernelChangeCurrentThreadAttr(0, PSP_THREAD_ATTR_VFPU);
}

#if SWDEC_DIAG_ENABLED
/* Validation: the decode thread's main-core attribution (swdec_recon_diag.c),
 * logged through the logger the browser hands to attach_me. */
static void (*recon_log)(const char *format, ...);

static int component_attach_me(
    const char *helper_path, void (*log)(const char *format, ...))
{
    recon_log = log;
    return swdec_me_attach_path(helper_path, log);
}

/* The browser's steady window (TilefinchSwdecComponentValidationStart):
   when it opens, the stage figures restart; when it closes, they are
   reported between swdec-window start and end lines and restart again, so
   the report at close covers only what followed the window. Checked on
   the decode thread at each access unit. */
static const volatile uint32_t *window_mark;
static uint32_t window_seen;
static uint32_t window_frames;

static void component_window_check(const void *decoder)
{
    if (window_mark == NULL) return;
    uint32_t mark = *window_mark;
    if (mark == window_seen) return;
    window_seen = mark;
    swdec_stats stats;
    swdec_stats_get(decoder, &stats);
    if ((mark & 1u) == 0) {
        swdec_me_stage_report();
        swdec_recon_diag_report(stats.frames_out - window_frames, recon_log);
        if (recon_log != NULL)
            recon_log("swdec-window: when=end mark=%u\n", (unsigned) mark);
    } else if (recon_log != NULL) {
        recon_log("swdec-window: when=start mark=%u\n", (unsigned) mark);
    }
    window_frames = stats.frames_out;
    swdec_me_stage_reset();
    swdec_recon_diag_reset();
}
#endif

static void *component_open(
    void *arena, size_t arena_bytes, int max_width, int max_height,
    int max_refs)
{
    if (component_enable_vfpu() < 0) return NULL;
    swdec_me_stage_reset();
#if SWDEC_DIAG_ENABLED
    swdec_recon_diag_reset();
    swdec_chroma_replay_reset(swdec_me_enabled);
#endif
    return swdec_open(arena, arena_bytes, max_width, max_height, max_refs);
}

static int component_decode(
    void *decoder, const uint8_t *access_unit, size_t access_unit_bytes,
    uint64_t pts, TilefinchSwdecPicture *picture)
{
    if (component_enable_vfpu() < 0) return -6;
#if SWDEC_DIAG_ENABLED
    component_window_check(decoder);
#endif
    unsigned wall, run;
    swdec_me_stage_au_begin(&wall, &run);
#if SWDEC_DIAG_ENABLED
    swdec_chroma_replay_install();
    unsigned recon_wall = sceKernelGetSystemTimeLow(),
             recon_run = swdec_recon_diag_run_us();
#endif
    swdec_me_stage_decode_census(0);
    int result = swdec_decode(
        decoder, access_unit, access_unit_bytes, pts,
        (swdec_picture *) picture);
    swdec_me_stage_decode_census(1);
#if SWDEC_DIAG_ENABLED
    swdec_recon_diag_au(sceKernelGetSystemTimeLow() - recon_wall,
                        swdec_recon_diag_run_us() - recon_run);
#endif
    swdec_me_stage_au_end(SWDEC_ME_STAGE_DECODE, wall, run);
    swdec_me_stage_decode_trace(pts);
    return result;
}

static void component_stats(
    const void *decoder, TilefinchSwdecStats *stats)
{
    swdec_stats_get(decoder, (swdec_stats *) stats);
}

static int component_me_failed(void)
{
    return swdec_me_failed;
}

static int component_csc_picture(
    int slot, void *rgb565, int stride_pixels, size_t capacity_bytes,
    const TilefinchSwdecPicture *picture)
{
    if (picture == NULL || component_enable_vfpu() < 0) return 0;
    unsigned wall, run;
    swdec_me_stage_au_begin(&wall, &run);
    int result = swdec_me_csc_picture(
        slot, rgb565, stride_pixels, capacity_bytes,
        picture->plane[0], picture->plane[1], picture->plane[2],
        picture->stride[0], picture->stride[1],
        picture->width, picture->height);
    swdec_me_stage_au_end(SWDEC_ME_STAGE_CSC_PICTURE, wall, run);
    return result;
}

static void component_close(void *decoder)
{
    swdec_me_stage_report();
#if SWDEC_DIAG_ENABLED
    {
        swdec_stats stats;
        swdec_stats_get(decoder, &stats);
        swdec_recon_diag_report(stats.frames_out, recon_log);
        swdec_chroma_replay_report(recon_log);
    }
#endif
    swdec_close(decoder);
}

static int component_csc_close(void)
{
    if (component_enable_vfpu() < 0) return 0;
    unsigned wall, run;
    swdec_me_stage_au_begin(&wall, &run);
    int result = swdec_me_csc_close();
    swdec_me_stage_au_end(SWDEC_ME_STAGE_CSC_CLOSE, wall, run);
    return result;
}

static int component_audio_setup(void)
{
    return swdec_me_audio_setup(NULL);
}

static void component_bind_aux_arena(void *arena, size_t arena_bytes)
{
    swdec_arena_bind_aux(arena, arena_bytes);
}

int module_start(SceSize argument_size, void *arguments);
int module_stop(SceSize argument_size, void *arguments);

int module_start(SceSize argument_size, void *arguments)
{
    if (arguments == NULL
        || argument_size < sizeof(TilefinchSwdecComponentStart))
        return -1;
    TilefinchSwdecComponentStart *start = arguments;
    if (start->magic != TILEFINCH_SWDEC_COMPONENT_MAGIC
        || start->abi_version != TILEFINCH_SWDEC_COMPONENT_ABI_VERSION
        || start->struct_size < sizeof(*start)
        || start->api == NULL) return -2;
#if SWDEC_DIAG_ENABLED
    /* A validation browser's longer start block (same ABI): its flags
       choose the decoder variants for this load. The Media Engine has not
       been booted yet; write the line back so its first read sees it. */
    /* Each field is read only when both sizes cover it: a browser from
       before window_mark passes the shorter block. */
    const size_t flags_end =
        offsetof(TilefinchSwdecComponentValidationStart, validation_flags)
        + sizeof(uint32_t);
    const size_t mark_end = sizeof(TilefinchSwdecComponentValidationStart);
    const TilefinchSwdecComponentValidationStart *validation = arguments;
    uint32_t validation_flags = 0;
    if (argument_size >= flags_end && start->struct_size >= flags_end)
        validation_flags = validation->validation_flags;
    if (argument_size >= mark_end && start->struct_size >= mark_end)
        window_mark = validation->window_mark;
    swdec_mc_reference =
        (validation_flags & TILEFINCH_SWDEC_VALIDATION_REFERENCE) != 0;
    swdec_mb_generic =
        (validation_flags & TILEFINCH_SWDEC_VALIDATION_MB_GENERIC) != 0;
    swdec_edge_full =
        (validation_flags & TILEFINCH_SWDEC_VALIDATION_EDGE_FULL) != 0;
    /* read by the Media Engine's parser too (it saves the jobs): its
       slice dispatches write the whole data cache back first */
    swdec_job_full =
        (validation_flags & TILEFINCH_SWDEC_VALIDATION_JOB_FULL) != 0;
    swdec_chroma_split =
        (validation_flags & TILEFINCH_SWDEC_VALIDATION_CHROMA_SPLIT) != 0;
    swdec_prefetch_calls =
        (validation_flags & TILEFINCH_SWDEC_VALIDATION_PREFETCH) != 0;
    /* read by the Media Engine's parser: dispatch writes the cache back */
    swdec_chroma_cpu =
        (validation_flags & TILEFINCH_SWDEC_VALIDATION_CHROMA_CPU) != 0;
    swdec_chroma_replay_enabled =
        (validation_flags & TILEFINCH_SWDEC_VALIDATION_CHROMA_REPLAY) != 0;
#endif

    TilefinchSwdecComponentApi api = {
        .magic = TILEFINCH_SWDEC_COMPONENT_MAGIC,
        .abi_version = TILEFINCH_SWDEC_COMPONENT_ABI_VERSION,
        .struct_size = sizeof(api),
        .arena_bytes = swdec_arena_bytes,
        .open = component_open,
        .decode = component_decode,
        .stats = component_stats,
        .set_speed = (void (*)(void *, int)) swdec_set_speed,
        .close = component_close,
        .bind_aux_arena = component_bind_aux_arena,
#if SWDEC_DIAG_ENABLED
        .attach_me = component_attach_me,
#else
        .attach_me = swdec_me_attach_path,
#endif
        .recover_me = swdec_me_recover,
        .detach_me = swdec_me_detach,
        .restore_me = swdec_me_restore,
        .me_failed = component_me_failed,
        .audio_setup = component_audio_setup,
        .audio_submit = swdec_me_audio_submit,
        .audio_poll = swdec_me_audio_poll,
        .audio_done = swdec_me_audio_done,
        .audio_pcm = swdec_me_audio_pcm,
        .audio_reset = swdec_me_audio_reset,
        .audio_shutdown = swdec_me_audio_shutdown,
        .csc_begin = swdec_me_csc_begin,
        .csc_picture = component_csc_picture,
        .csc_close = component_csc_close,
        .csc_off = swdec_me_csc_off
    };
    __locks_init();
    __init_mutex();
    __fdman_init();
    memcpy(start->api, &api, sizeof(api));
    return 0;
}

/* The browser stops the component only after restoring the Media Engine and
   closing every decoder. */
int module_stop(SceSize argument_size, void *arguments)
{
    (void) argument_size;
    (void) arguments;
    __deinit_mutex();
    __locks_deinit();
    return 0;
}
