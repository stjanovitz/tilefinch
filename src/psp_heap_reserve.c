/* Decides, before newlib claims its heap, how much of the user partition to
 * leave outside it.
 *
 * PSPSDK's _sbrk sizes the heap on its first call: the largest free block
 * less sce_newlib_heap_threshold_kb_size KiB (PSP_HEAP_SIZE_KB(-1) with
 * PSP_HEAP_THRESHOLD_SIZE_KB in psp_script_main.c). The first call comes
 * from inside __libcglue_init (pthread and TZ setup allocate), so this
 * wraps that function (--wrap=__libcglue_init) and raises the threshold
 * when the optional software decoder is installed, before anything has
 * allocated. A browser without the add-on keeps the whole heap. */
#include <pspiofilemgr.h>

#include "psp_boot_module.h"
#include "psp_heap_reserve.h"
#include "tilefinch/swdec_component_store.h"

extern int sce_newlib_heap_threshold_kb_size;
extern void __real___libcglue_init(int, char **);

static bool heap_reserve_swdec_found;

#ifdef TILEFINCH_PSP_VALIDATION_LOG
#include <malloc.h>
#include <pspsysmem.h>
#include <stdint.h>
#include <unistd.h>

#include "tilefinch/budget.h"
#include "tilefinch/psp_log.h"

/* The largest free partition block just before libc's init. _sbrk sizes the
   heap from the same call on the init's first allocation (pte_osInit's
   malloc; the init steps before it only create lightweight mutexes and fill
   the descriptor table), so the heap is this less the threshold. */
static size_t heap_reserve_largest_free_before_heap;

/* Defined by both PSP link scripts (the EBOOT's and linkfile.prx). */
extern char _ftext[];
extern char _end[];

void psp_heap_reserve_report(void)
{
    size_t heap = tilefinch_psp_newlib_heap_bytes(
        heap_reserve_largest_free_before_heap,
        sce_newlib_heap_threshold_kb_size);
    struct mallinfo arena = mallinfo();
    /* psp_log_printf, not printf: plain stdout never reaches the
       validation log. */
    psp_log_printf("tilefinch-psp-script: heap bytes=%u arena=%u free=%u\n",
                   (unsigned) heap, (unsigned) arena.arena,
                   (unsigned) tilefinch_psp_heap_free_bytes(
                       heap, (size_t) arena.arena, (size_t) arena.fordblks));
    /* Where the loader put the image and the heap. The I-cache and D-cache
       are 2-way with 8 KiB per way, so an address modulo 8 KiB decides its
       cache set; two runs compare their timings only if these match
       (docs/engineering/CODE_LAYOUT.md). newlib's heap is one contiguous
       sbrk region, so its base is the break less the arena. */
    uintptr_t text = (uintptr_t) _ftext;
    uintptr_t heap_base = (uintptr_t) sbrk(0) - (uintptr_t) arena.arena;
    psp_log_printf("tilefinch-code-layout: text=0x%08x text-set=%u "
                   "end=0x%08x heap=0x%08x heap-set=%u\n",
                   (unsigned) text, (unsigned) (text & 8191u),
                   (unsigned) (uintptr_t) _end, (unsigned) heap_base,
                   (unsigned) (heap_base & 8191u));
}

typedef struct {
    bool valid;
    size_t heap;
    size_t arena;
    size_t in_use;
    size_t arena_free;
    size_t budget_current;
    size_t budget_peak;
    int system_free;
} HeapCommitSample;

static HeapCommitSample heap_commit_peak;

static void heap_commit_sample(HeapCommitSample *sample, const Budget *budget)
{
    struct mallinfo arena = mallinfo();
    sample->valid = true;
    sample->heap = tilefinch_psp_newlib_heap_bytes(
        heap_reserve_largest_free_before_heap,
        sce_newlib_heap_threshold_kb_size);
    sample->arena = (size_t) arena.arena;
    sample->in_use = (size_t) arena.uordblks;
    sample->arena_free = (size_t) arena.fordblks;
    sample->budget_current = budget != NULL ? budget->current : 0u;
    sample->budget_peak = budget != NULL ? budget->peak : 0u;
    sample->system_free = sceKernelTotalFreeMemSize();
}

static void heap_commit_keep(const HeapCommitSample *sample)
{
    if (!heap_commit_peak.valid || sample->in_use > heap_commit_peak.in_use)
        heap_commit_peak = *sample;
}

static void heap_commit_observe(void *opaque, const Budget *budget)
{
    (void) opaque;
    HeapCommitSample sample;
    heap_commit_sample(&sample, budget);
    heap_commit_keep(&sample);
}

void psp_heap_commit_watch(const Budget *budget)
{
    budget_set_peak_observer(budget, heap_commit_observe, NULL,
                             256u * 1024u);
}

static void heap_commit_print(const char *phase,
                              const HeapCommitSample *sample)
{
    size_t uncommitted =
        sample->heap > sample->arena ? sample->heap - sample->arena : 0u;
    /* in-use counts every malloc chunk (with newlib's chunk overhead);
       the Budget counts its own allocations with their headers. What is
       left is libc, PSPSDK, thread stacks and other unbudgeted mallocs. */
    long non_budget = (long) sample->in_use - (long) sample->budget_current;
    psp_log_printf(
        "tilefinch-heap-commit: phase=%s heap=%u arena=%u in-use=%u "
        "arena-free=%u uncommitted=%u real-free=%u budget-current=%u "
        "budget-peak=%u non-budget=%ld system-free=%d\n",
        phase, (unsigned) sample->heap, (unsigned) sample->arena,
        (unsigned) sample->in_use, (unsigned) sample->arena_free,
        (unsigned) uncommitted,
        (unsigned) (uncommitted + sample->arena_free),
        (unsigned) sample->budget_current, (unsigned) sample->budget_peak,
        non_budget, sample->system_free);
}

void psp_heap_commit_report(const char *phase, const Budget *budget,
                            bool with_peak)
{
    HeapCommitSample sample;
    heap_commit_sample(&sample, budget);
    heap_commit_keep(&sample);
    heap_commit_print(phase != NULL ? phase : "unknown", &sample);
    if (with_peak) heap_commit_print("peak", &heap_commit_peak);
}
#endif

bool psp_heap_reserve_swdec_found(void)
{
    return heap_reserve_swdec_found;
}

int psp_heap_reserve_threshold_kb(void)
{
    return sce_newlib_heap_threshold_kb_size;
}

void __wrap___libcglue_init(int argc, char **argv)
{
    /* The launcher handoff first: it returns once the launcher is gone,
       and may take its private argument off argv. */
    if (psp_boot_module_before_libc != NULL)
        psp_boot_module_before_libc(&argc, argv);
    char path[TILEFINCH_INSTALL_PATH_LIMIT];
    SceIoStat stat;
    heap_reserve_swdec_found = argc > 0 && argv != NULL
        && tilefinch_swdec_component_prx_path_for_program(
               argv[0], path, sizeof(path))
        && sceIoGetstat(path, &stat) >= 0
        && !FIO_S_ISDIR(stat.st_mode);
    if (heap_reserve_swdec_found
        && sce_newlib_heap_threshold_kb_size
               < tilefinch_psp_heap_threshold_kb(true))
        sce_newlib_heap_threshold_kb_size =
            tilefinch_psp_heap_threshold_kb(true);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    heap_reserve_largest_free_before_heap = sceKernelMaxFreeMemSize();
#endif
    __real___libcglue_init(argc, argv);
}
