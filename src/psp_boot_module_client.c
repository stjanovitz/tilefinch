/* Linked only into the relocatable browser. crt0 must park before newlib
   reserves the heap, not at the beginning of the application's main(). */
#include <pspkernel.h>
#include <pspkerror.h>
#include <pspmodulemgr.h>
#include <string.h>

#include "psp_boot_module.h"
#include "psp_thread_contract.h"
#ifdef TILEFINCH_PSP_VALIDATION_LOG
#include "tilefinch/psp_log.h"
#endif

extern void __real___libcglue_init(int, char **);
extern int __real_main(int, char **);
#ifdef TILEFINCH_PSP_BOOT_UNWRAPPED_SEMAPHORE
extern int __real_sceKernelWaitSema(SceUID, int, SceUInt *);
#define boot_wait_sema __real_sceKernelWaitSema
#else
#define boot_wait_sema sceKernelWaitSema
#endif

static bool module_handoff;
static PspBootModuleResult module_result;

static _Noreturn void decline_before_release(void)
{
    /* No libc heap exists yet. The launcher still owns the process and
       performs LoadExec after its bounded ready/retirement refusal. Starting
       another LoadExec here can race that handoff and reset the machine. */
    sceKernelExitThread(-1);
    for (;;) sceKernelSleepThread();
}

static int signal_ready(void *ctx, int32_t id)
{ (void) ctx; return sceKernelSignalSema(id, 1); }
static int wait_gate(void *ctx, int32_t id, uint32_t us)
{ (void) ctx; SceUInt timeout = us; return boot_wait_sema(id, 1, &timeout); }
static int join_thread(void *ctx, int32_t id, uint32_t us)
{ (void) ctx; return psp_thread_wait_end_bounded(id, us); }
static int delete_thread(void *ctx, int32_t id)
{ (void) ctx; return sceKernelDeleteThread(id); }
static int delete_gate(void *ctx, int32_t id)
{ (void) ctx; return sceKernelDeleteSema(id); }
static int stop_module(void *ctx, int32_t id)
{
    (void) ctx;
    int status = -1;
    /* A module with no stop entry leaves status untouched. The return value
       governs whether it stopped; the unload/query checks confirm retirement. */
    return sceKernelStopModule(id, 0, NULL, &status, NULL);
}
static int unload_module(void *ctx, int32_t id)
{ (void) ctx; return sceKernelUnloadModule(id); }
static int module_absent(void *ctx, int32_t id)
{
    (void) ctx;
    SceKernelModuleInfo info = {.size = sizeof(info)};
    int result = sceKernelQueryModuleInfo(id, &info);
    return result == (int) SCE_KERNEL_ERROR_UNKNOWN_MODULE ? 1
        : result < 0 ? -1 : 0;
}

static bool semaphore_valid(int id, const char *name)
{
    SceKernelSemaInfo info = {.size = sizeof(info)};
    return sceKernelReferSemaStatus(id, &info) >= 0
        && memcmp(info.name, name, strlen(name) + 1u) == 0
        && info.initCount == 0 && info.currentCount == 0 && info.maxCount == 1;
}

static bool parent_valid(const PspBootModuleArgument *arg)
{
    SceKernelModuleInfo module = {.size = sizeof(module)};
    SceKernelThreadInfo thread = {.size = sizeof(thread)};
    int self = sceKernelGetModuleIdByAddress((const void *) parent_valid);
    if (self < 0 || self == arg->module || sceKernelGetThreadId() == arg->thread
        || sceKernelQueryModuleInfo(arg->module, &module) < 0
        || memcmp(module.name, "Tilefinch Launcher", sizeof("Tilefinch Launcher")) != 0
        || psp_thread_snapshot(arg->thread, &thread) < 0
        || !semaphore_valid(arg->gate, PSP_BOOT_MODULE_GATE_NAME)
        || !semaphore_valid(arg->ready, PSP_BOOT_MODULE_READY_NAME)) return false;
    uintptr_t entry = (uintptr_t) thread.entry;
    for (unsigned at = 0; at < (unsigned char) module.nsegment && at < 4u; at++) {
        uintptr_t base = (uint32_t) module.segmentaddr[at];
        if (entry >= base && entry - base < (uint32_t) module.segmentsize[at]) return true;
    }
    return false;
}

void __wrap___libcglue_init(int argc, char **argv)
{
    if (argc > 1 && argv != NULL && argv[argc - 1] != NULL
        && strncmp(argv[argc - 1], PSP_BOOT_MODULE_PREFIX,
                   sizeof(PSP_BOOT_MODULE_PREFIX) - 1u) == 0) {
        PspBootModuleArgument argument;
        static const PspBootModuleOps ops = {
            NULL, signal_ready, wait_gate, join_thread, delete_thread,
            delete_gate, stop_module, unload_module, module_absent
        };
        /* Invalid arguments cannot authorize manipulating unrelated owners.
           Before the gate, only the parent may replace the process. */
        if (!psp_boot_module_decode(argv[argc - 1], &argument)
            || !parent_valid(&argument)) decline_before_release();
        module_result = psp_boot_module_receive(&argument, &ops);
        if (module_result != PSP_BOOT_MODULE_COMPLETE) {
            if (!psp_boot_module_receiver_owns_fallback(module_result))
                decline_before_release();
            psp_boot_module_exec_fallback(argv[0]);
        }
        module_handoff = true;
        argv[--argc] = NULL;
    }
    __real___libcglue_init(argc, argv);
}

int __wrap_main(int argc, char **argv)
{
    /* crt0 retains its original argc even though the pre-libc hook removed
       our private final argument from argv. Do not expose it to app code. */
    return __real_main(module_handoff ? argc - 1 : argc, argv);
}

void psp_boot_module_report(void)
{
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    if (module_handoff)
        psp_log_printf("tilefinch-boot-module: retired=1 result=%u\n", (unsigned) module_result);
#endif
}
