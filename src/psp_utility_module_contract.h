#ifndef TILEFINCH_PSP_UTILITY_MODULE_CONTRACT_H
#define TILEFINCH_PSP_UTILITY_MODULE_CONTRACT_H

#include <psputility_avmodules.h>
#include <psputility_netmodules.h>

#include "psp_module_policy.h"

/* Keep the raw status for the device log while forcing every production
   utility-module load through the exact already-resident classification. */
static inline PspUtilityModuleLoadDisposition
psp_utility_load_av_module(int module, int *native_status)
{
    int status = sceUtilityLoadAvModule(module);
    if (native_status != NULL) *native_status = status;
    return psp_utility_av_module_load_disposition(status);
}

static inline PspUtilityModuleLoadDisposition
psp_utility_load_net_module(int module, int *native_status)
{
    int status = sceUtilityLoadNetModule(module);
    if (native_status != NULL) *native_status = status;
    return psp_utility_net_module_load_disposition(status);
}

#endif
