#include "psp_boot_module.h"
#include "tilefinch/install_paths.h"

#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <psploadexec.h>
#include <pspmodulemgr.h>
#include <string.h>
#include "psp_systemctrl_stubs.h"

extern void __libcglue_deinit(void);

void psp_boot_module_exec_fallback(const char *path)
{
    char target[TILEFINCH_INSTALL_PATH_LIMIT];
    size_t n = 0;
    if (path != NULL) {
        for (; n + 1u < sizeof(target) && path[n] != '\0'; n++) target[n] = path[n];
        if (path[n] == '\0' && n != 0u) {
            target[n] = '\0';
            struct SceKernelLoadExecVSHParam cfw = {
                .size = sizeof(cfw), .args = n + 1u, .argp = target,
                .key = "game", .unk5 = 0x10000
            };
            (void) sctrlKernelLoadExecVSHMs2(target, &cfw);
            struct SceKernelLoadExecParam standard = {
                .size = sizeof(standard), .args = n + 1u, .argp = target, .key = "game"
            };
            (void) sceKernelLoadExec(target, &standard);
        }
    }
    sceKernelExitGame();
    for (;;) sceKernelSleepThread();
}

void psp_boot_module_start(const char *path, const void *arguments, size_t bytes,
                           bool (*retire_callbacks)(void *), void *context)
{
    if (path == NULL || arguments == NULL || bytes == 0u
        || bytes > TILEFINCH_INSTALL_PATH_LIMIT + 128u
        || retire_callbacks == NULL || ((const char *)arguments)[bytes - 1u] != '\0') return;
    /* Only the previously verified raw cache is a module candidate. The caller's
       argv[0] remains the authoritative PBP for every fallback. */
    uint8_t header[20];
    int file = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (file < 0) return;
    int got = sceIoRead(file, header, sizeof(header));
    int closed = sceIoClose(file);
    if (got != (int) sizeof(header) || closed < 0
        || !psp_boot_module_format(header, sizeof(header))) return;
    int gate = sceKernelCreateSema(PSP_BOOT_MODULE_GATE_NAME, 0, 0, 1, NULL);
    int ready = sceKernelCreateSema(PSP_BOOT_MODULE_READY_NAME, 0, 0, 1, NULL);
    int module = -1;
    if (gate < 0 || ready < 0) goto refused;
    module = sceKernelLoadModule(path, 0, NULL);
    if (module < 0) goto refused;
    char args[TILEFINCH_INSTALL_PATH_LIMIT + 128u + PSP_BOOT_MODULE_ARGUMENT_BYTES];
    memcpy(args, arguments, bytes);
    PspBootModuleArgument handoff = {
        gate, ready, sceKernelGetModuleIdByAddress((const void *) psp_boot_module_start),
        sceKernelGetThreadId()
    };
    if (!psp_boot_module_encode(args + bytes, sizeof(args) - bytes, &handoff)) {
        (void) sceKernelUnloadModule(module); /* Not started; no live owner. */
        goto refused;
    }
    int status = -1;
    int started = sceKernelStartModule(module,
        bytes + PSP_BOOT_MODULE_ARGUMENT_BYTES, args, &status, NULL);
    SceUInt timeout = 2000000u;
    if (started < 0 || status != 0 || sceKernelWaitSema(ready, 1, &timeout) < 0)
        /* An uncertain start must be cleaned by process replacement, not a
           forced unload. Its consumer is parked before libc initialization. */
        goto refused;
    if (!retire_callbacks(context) || sceKernelDeleteSema(ready) < 0) goto refused;
    __libcglue_deinit();
    /* No libc calls or heap-backed data access after this point. */
    if (sceKernelSignalSema(gate, 1) < 0) psp_boot_module_exec_fallback(arguments);
    /* Keep the UID/stack until the receiver joins and deletes the thread. */
    sceKernelExitThread(0);
    /* The gate transferred restart authority. If thread exit is refused,
       park: the consumer's bounded join owns process replacement now. Never
       race its fallback with another LoadExec from this thread. */
    for (;;) sceKernelSleepThread();
refused:
    /* LoadExec will also clear any parked consumer. Never free its gate or
       stop its code while ownership is uncertain. Before a start, semaphore
       cleanup is optional because the caller immediately replaces process. */
    (void) module;
    return;
}
