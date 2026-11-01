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
    __real___libcglue_init(argc, argv);
}
