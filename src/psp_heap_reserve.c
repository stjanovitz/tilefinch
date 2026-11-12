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

#include "tilefinch/psp_log.h"

/* The largest free partition block just before libc's init. _sbrk sizes the
   heap from the same call on the init's first allocation (pte_osInit's
   malloc; the init steps before it only create lightweight mutexes and fill
   the descriptor table), so the heap is this less the threshold. */
static size_t heap_reserve_largest_free_before_heap;

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
