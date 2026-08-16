#ifndef TILEFINCH_PSP_SYSTEMCTRL_STUBS_H
#define TILEFINCH_PSP_SYSTEMCTRL_STUBS_H

#include <stdint.h>

#include <psploadexec_kernel.h>

/*
 * Prototypes for the SystemCtrlForUser imports declared in
 * src/systemctrl_user_imports.S. They are written against the installed
 * custom firmware's published ABI so that neither the GPL-3.0 psp-cfw-sdk
 * headers nor its stub archive become part of the build. Both imports are
 * weak: firmware without a CFW SystemCtrl module leaves them unresolved and
 * the calls return errors instead of preventing module load.
 */

/* Resolve an exported or HEN-patched function address by module, library,
   and NID. CFW implementations document zero when unavailable, but the weak
   unresolved import may surface a negative PSP status word; callers must
   validate the kernel-code address rather than testing only for nonzero. */
uint32_t sctrlHENFindFunction(const char *module_name,
                              const char *library_name, uint32_t nid);

/* CFW VSH Memory Stick handoff used to start another homebrew EBOOT. A
   successful call does not return; on environments without SystemCtrl the
   call fails and the standard sceKernelLoadExec path remains the
   fallback. */
int sctrlKernelLoadExecVSHMs2(const char *path,
                              struct SceKernelLoadExecVSHParam *param);

/* KUBridge argument block for kuKernelCall: twelve 32-bit argument slots
   followed by two 32-bit return words, matching the firmware library's
   published ABI. */
typedef struct KernelCallArg {
    uint32_t arg1;
    uint32_t arg2;
    uint32_t arg3;
    uint32_t arg4;
    uint32_t arg5;
    uint32_t arg6;
    uint32_t arg7;
    uint32_t arg8;
    uint32_t arg9;
    uint32_t arg10;
    uint32_t arg11;
    uint32_t arg12;
    uint32_t ret1;
    uint32_t ret2;
} KernelCallArg;

/* Load a module through the CFW bridge and call a kernel-mode function from
   user mode. Both are unavailable without a CFW KUBridge module. */
SceUID kuKernelLoadModule(const char *path, int flags,
                          SceKernelLMOption *option);
int kuKernelCall(void *function, struct KernelCallArg *arguments);

#endif
