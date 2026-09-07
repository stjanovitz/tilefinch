#include "stt_component_loader.h"
#include <pspkernel.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

TilefinchSttApi tilefinch_stt_api;
static char component_path[768];
static SceUID component_module = -1;
static bool component_started;
static bool component_quarantined;
static BudgetReservation component_memory;

void psp_voice_component_configure(const char *directory)
{
    if (component_module >= 0) return;
    component_path[0] = '\0';
    if (!directory || !directory[0]) return;
    /* libcglue has a 256-byte cwd. Require an absolute PSP device path,
     * independent of whichever page/model was most recently opened. */
    const char *device = strchr(directory, ':');
    if (!device || device == directory || device[1] != '/'
        || strlen(directory) >= 256u) return;
    int n = snprintf(component_path, sizeof(component_path),
                     "%s/tilefinch-voice.prx", directory);
    if (n < 0 || (size_t)n >= sizeof(component_path)) component_path[0] = '\0';
}

void psp_voice_component_unload(void)
{
    if (component_quarantined) return;
    if (component_module >= 0) {
        if (component_started) {
            int status = -1;
            if (sceKernelStopModule(component_module, 0, NULL, &status, NULL) < 0
                || status != 0) return;
            component_started = false;
        }
        memset(&tilefinch_stt_api, 0, sizeof(tilefinch_stt_api));
        if (sceKernelUnloadModule(component_module) < 0) return;
        component_module = -1;
    }
    budget_reservation_release(&component_memory);
}

void psp_voice_component_quarantine(void)
{
    component_quarantined = true;
    memset(&tilefinch_stt_api, 0, sizeof(tilefinch_stt_api));
}

bool psp_voice_component_is_quarantined(void)
{
    return component_quarantined;
}

bool psp_voice_component_load(Budget *budget)
{
    if (component_quarantined) return false;
    if (component_started && tilefinch_stt_api_valid(&tilefinch_stt_api))
        return true;
    /* Retry failed cleanup before attempting a second module. */
    psp_voice_component_unload();
    if (component_module >= 0 || !budget || !component_path[0]) return false;
    if (!budget_reservation_acquire(&component_memory, budget,
            BUDGET_CATEGORY_RESOURCE, TILEFINCH_STT_RESIDENT_LIMIT)) return false;
    component_module = sceKernelLoadModule(component_path, 0, NULL);
    if (component_module < 0) goto failed;
    SceKernelModuleInfo info = {0};
    info.size = sizeof(info);
    if (sceKernelQueryModuleInfo(component_module, &info) < 0) goto failed;
    size_t resident = 0;
    if (info.nsegment > 4) goto failed;
    for (unsigned i = 0; i < info.nsegment; ++i) {
        if (info.segmentsize[i] > TILEFINCH_STT_RESIDENT_LIMIT - resident)
            goto failed;
        resident += info.segmentsize[i];
    }
    TilefinchSttStart start = {
        .magic = TILEFINCH_STT_MAGIC, .abi = TILEFINCH_STT_ABI,
        .size = sizeof(start), .api = &tilefinch_stt_api,
        .allocate = malloc, .allocate_zero = calloc, .resize = realloc,
        .release = free, .align = memalign, .terminate = _exit
    };
    char directory[sizeof(component_path)];
    memcpy(directory, component_path, sizeof(directory));
    char *separator = strrchr(directory, '/');
    if (!separator) goto failed;
    *separator = '\0';
    start.directory = directory;
    int status = -1;
    int result = sceKernelStartModule(component_module, sizeof(start),
                                      &start, &status, NULL);
    component_started = result >= 0 && status == 0;
    if (!component_started || !tilefinch_stt_api_valid(&tilefinch_stt_api))
        goto failed;
    return true;
failed:
    psp_voice_component_unload();
    return false;
}
