/* tfexec -- a two-page PRX that LoadExecs the installed Tilefinch browser.
 *
 * It exists for one job: closing the remote device loop when the browser is
 * run as an EBOOT rather than as a PRX. `exit_to=` sends the browser back to
 * PSPLink's EBOOT when it exits (src/psp_app/psp_app_exit_handoff.c); this is
 * the other half, the hop PSPLink takes to start the browser again. Together
 * they make an unattended watch/search cycle possible from the host:
 *
 *     pspsh> ld host0:/tfexec.prx      # browser runs, exits back to PSPLink
 *     pspsh> ld host0:/tfexec.prx      # ... and again, with a new boot.cfg
 *
 * PSPLink loads relocatable modules but rejects a static PSP ELF with
 * 0x80020148, so this is a PRX. It is deliberately not part of the CMake
 * build: it links no engine code, changes on the order of once a year, and
 * building it from the SDK's own build.mak keeps it honest about the PRX
 * recipe the browser's psp-browser-script-dev-prx target reproduces.
 *
 *     PSPDEV=/path/to/pspdev PATH=$PSPDEV/bin:$PATH make -C tools/psplink-loop
 *
 * See docs/engineering/INPUT_SCRIPT_HARNESS.md, "PSPLink live loop".
 */
#include <pspsdk.h>
#include <pspkernel.h>
#include <psploadexec.h>
#include <psploadexec_kernel.h>
#include <stdio.h>

PSP_MODULE_INFO("tfexec", 0, 1, 1);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER);

/* Mirrors the stable launcher's proven handoff (src/update_launcher_psp.c):
   sctrlKernelLoadExecVSHMs2 with key="game"/unk5=0x10000 first, plain
   sceKernelLoadExec as the fallback for PPSSPP and any environment that
   implements only the published API. Neither returns on success. The import
   stub is the project's own (src/systemctrl_user_imports.S), so no GPL-3.0
   psp-cfw-sdk archive reaches this binary. */
int sctrlKernelLoadExecVSHMs2(const char *file,
                              struct SceKernelLoadExecVSHParam *param);

/* Lowercase slot-a to match the launcher's own argv0 shape exactly: the
   browser derives its data directory from argv[0], and a case-mismatched
   path would send its profile and validation log somewhere else. */
static const char tf_eboot[] = "ms0:/PSP/GAME/TILEFINCH/slot-a/EBOOT.PBP";

int main(int argc, char *argv[])
{
    (void) argc;
    (void) argv;
    struct SceKernelLoadExecVSHParam cfw_parameters = {
        .size = sizeof(cfw_parameters),
        .args = sizeof(tf_eboot),
        .argp = (void *) tf_eboot,
        .key = "game",
        .unk5 = 0x10000
    };
    int cfw_result = sctrlKernelLoadExecVSHMs2(tf_eboot, &cfw_parameters);
    struct SceKernelLoadExecParam fallback_parameters = {
        .size = sizeof(fallback_parameters),
        .args = sizeof(tf_eboot),
        .argp = (void *) tf_eboot,
        .key = "game"
    };
    int fallback_result = sceKernelLoadExec(tf_eboot, &fallback_parameters);
    /* Reaching here means both calls returned, which only happens on
       failure. The pspsh shell is the only reader, so this line is the whole
       report. */
    printf("tfexec: HANDOFF FAILED cfw=0x%08X standard=0x%08X\n",
           (unsigned) cfw_result, (unsigned) fallback_result);
    return 0;
}
