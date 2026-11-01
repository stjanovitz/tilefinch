#ifndef TILEFINCH_SWDEC_COMPONENT_STORE_H
#define TILEFINCH_SWDEC_COMPONENT_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/install_paths.h"

#define TILEFINCH_SWDEC_COMPONENT_DIRECTORY "swdec"
#define TILEFINCH_SWDEC_COMPONENT_INFO_NAME "component-info.txt"
#define TILEFINCH_SWDEC_COMPONENT_INFO_MAGIC "tilefinch-swdec-component-v1"
#define TILEFINCH_SWDEC_COMPONENT_INFO_LIMIT 96u

typedef enum {
    TILEFINCH_SWDEC_COMPONENT_INFO_ABSENT = 0,
    TILEFINCH_SWDEC_COMPONENT_INFO_VALID,
    TILEFINCH_SWDEC_COMPONENT_INFO_INVALID
} TilefinchSwdecComponentInfoStatus;

bool tilefinch_swdec_component_path(
    const TilefinchInstallPaths *paths, const char *name,
    char *output, size_t output_size);
/*
 * Partition memory left outside the newlib heap (PSP_HEAP_THRESHOLD_SIZE_KB).
 * Every browser keeps 2 MiB for the firmware modules it loads after boot
 * (network, AV codecs). When the optional decoder is installed it keeps
 * another 2 MiB for that PRX: 1.90 MiB loaded with the validation
 * diagnostics, 1.88 MiB without, plus its start thread -- the configuration
 * device runs have played with. Without the add-on that room stays heap.
 * The choice is made from the add-on's presence before newlib claims the
 * heap (psp_heap_reserve.c).
 */
#define TILEFINCH_PSP_HEAP_THRESHOLD_KB 2048
#define TILEFINCH_PSP_HEAP_THRESHOLD_SWDEC_KB 4096
static inline int tilefinch_psp_heap_threshold_kb(bool swdec_installed)
{
    return swdec_installed ? TILEFINCH_PSP_HEAP_THRESHOLD_SWDEC_KB
                           : TILEFINCH_PSP_HEAP_THRESHOLD_KB;
}
/* The installed component PRX for the program at `argv0`, as
   tilefinch_swdec_component_path finds it, without allocating or
   formatting: this runs before the C library is initialised. */
bool tilefinch_swdec_component_prx_path_for_program(
    const char *argv0, char *output, size_t output_size);

TilefinchSwdecComponentInfoStatus tilefinch_swdec_component_info_parse(
    const char *bytes, size_t length, uint16_t *abi_version);
TilefinchSwdecComponentInfoStatus tilefinch_swdec_component_info_read(
    const TilefinchInstallPaths *paths, uint16_t *abi_version);

/*
 * Why an installed component cannot be used. REBUILD is only for a
 * component that is not this browser's ABI (its info file, the start block
 * its module_start refused, or the table it returned); RUNTIME is the
 * firmware refusing to load or start a component that may well be current
 * (memory, most often), and carries the firmware's code.
 */
typedef enum {
    TILEFINCH_SWDEC_FAILURE_NONE = 0,
    TILEFINCH_SWDEC_FAILURE_MISSING,
    TILEFINCH_SWDEC_FAILURE_REBUILD,
    TILEFINCH_SWDEC_FAILURE_RUNTIME
} TilefinchSwdecFailure;

/* sceKernelStartModule's result, module_start's status and whether the API
   table it filled matches this ABI. */
TilefinchSwdecFailure tilefinch_swdec_component_start_failure(
    int start_result, int module_status, bool api_valid);
/* The message for a failure, with `native_error` for a runtime one. */
void tilefinch_swdec_component_failure_message(
    TilefinchSwdecFailure failure, int native_error,
    char *output, size_t output_size);

#endif
