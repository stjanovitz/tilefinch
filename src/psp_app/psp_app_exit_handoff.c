/* Hands the console back to whatever launched this EBOOT when it exits.
 *
 * A device run driven from a host over PSPLink has no operator standing at
 * the PSP: when the browser exits to the XMB the remote loop is over until
 * someone walks to the console and starts PSPLink again. `exit_to=` names the
 * EBOOT to load instead, and this TU is the load.
 *
 * Kept out of line because this cold CFW handoff does not belong in main's
 * growth-tripwire symbol.
 */
#include "psp_app_internal.h"

#include "psp_systemctrl_stubs.h"

bool psp_load_exec_eboot(const char *eboot_path,
                         int *cfw_result_out, int *fallback_result_out)
{
    if (eboot_path == NULL || eboot_path[0] == '\0') return false;
    /* The firmware parameter blocks take a mutable argument pointer, and the
       loaded program reads its own path back out of it. Copy rather than
       cast away the caller's const. */
    char target[TILEFINCH_INSTALL_PATH_LIMIT];
    int length = snprintf(target, sizeof(target), "%s", eboot_path);
    if (length <= 0 || (size_t) length >= sizeof(target)) return false;
    /*
     * The same recipe the stable launcher uses to start a slot (see
     * src/update_launcher_psp.c): user-mode sceKernelLoadExec cannot
     * reliably start another Memory Stick homebrew on post-1.xx firmware, so
     * the CFW VSH handoff goes first and the standard call remains for
     * PPSSPP and any environment that implements only the published API.
     * Neither returns on success.
     */
    struct SceKernelLoadExecVSHParam cfw_parameters = {
        .size = sizeof(cfw_parameters),
        .args = (unsigned) length + 1u,
        .argp = target,
        .key = "game",
        .unk5 = 0x10000
    };
    int cfw_result = sctrlKernelLoadExecVSHMs2(target, &cfw_parameters);
    struct SceKernelLoadExecParam fallback_parameters = {
        .size = sizeof(fallback_parameters),
        .args = (unsigned) length + 1u,
        .argp = target,
        .key = "game"
    };
    int fallback_result = sceKernelLoadExec(target, &fallback_parameters);
    if (cfw_result_out != NULL) *cfw_result_out = cfw_result;
    if (fallback_result_out != NULL) *fallback_result_out = fallback_result;
    return false;
}

#ifdef TILEFINCH_PSP_VALIDATION_LOG

bool psp_exit_handoff(const char *eboot_path)
{
    int cfw_result = 0;
    int fallback_result = 0;
    (void) psp_load_exec_eboot(
        eboot_path, &cfw_result, &fallback_result);
    /* Reaching here means both returned, which only happens on failure. The
       caller falls through to its ordinary exit, so this line is the whole
       report. */
    printf("tilefinch-exit-handoff: target=%s cfw=0x%08x standard=0x%08x\n",
           eboot_path, (unsigned) cfw_result, (unsigned) fallback_result);
    return false;
}

#endif
