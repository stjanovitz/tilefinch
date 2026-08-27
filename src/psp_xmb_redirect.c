#include "tilefinch/xmb_redirect_policy.h"

#include <pspctrl.h>
#include <pspiofilemgr_kernel.h>
#include <pspkernel.h>
#include <psploadexec_kernel.h>
#include <systemctrl.h>

PSP_MODULE_INFO(
    "TilefinchXmbRedirect",
    PSP_MODULE_KERNEL | PSP_MODULE_NO_STOP
        | PSP_MODULE_SINGLE_LOAD | PSP_MODULE_SINGLE_START,
    1, 0);

#define TILEFINCH_XMB_REDIRECT_THREAD_PRIORITY 0x21
#define TILEFINCH_XMB_REDIRECT_THREAD_STACK 0x1000
#define TILEFINCH_XMB_REDIRECT_ACTIVATION_BUTTONS \
    (PSP_CTRL_CROSS | PSP_CTRL_CIRCLE)
#define TILEFINCH_XMB_REDIRECT_BYPASS_BUTTON PSP_CTRL_LTRIGGER
#define TILEFINCH_XMB_REDIRECT_MODULE_SETTLE_US 250000u
#define TILEFINCH_XMB_REDIRECT_EVENT UINT32_C(1)

static const char tilefinch_ms_launcher[] =
    "ms0:/PSP/GAME/TILEFINCH/EBOOT.PBP";
static const char tilefinch_ef_launcher[] =
    "ef0:/PSP/GAME/TILEFINCH/EBOOT.PBP";

static STMOD_HANDLER previous_start_handler;
static volatile int launch_armed;
static SceUID redirect_event = -1;

static bool installed_launcher(char target[sizeof(tilefinch_ms_launcher)])
{
    SceIoStat status = {0};
    const char *source = NULL;
    if (sceIoGetstat(tilefinch_ms_launcher, &status) >= 0) {
        source = tilefinch_ms_launcher;
    }
    if (source == NULL
        && sceIoGetstat(tilefinch_ef_launcher, &status) >= 0) {
        source = tilefinch_ef_launcher;
    }
    if (source == NULL) return false;
    for (unsigned int index = 0;
         index < sizeof(tilefinch_ms_launcher); index++) {
        target[index] = source[index];
        if (source[index] == '\0') return true;
    }
    return false;
}

static int redirect_thread(SceSize argument_size, void *argument)
{
    (void) argument_size;
    (void) argument;

    for (;;) {
        uint32_t observed = 0u;
        if (sceKernelWaitEventFlag(
                redirect_event,
                TILEFINCH_XMB_REDIRECT_EVENT,
                PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR,
                &observed,
                NULL) < 0) {
            launch_armed = 0;
            return 0;
        }

        /* The start-module hook is a pre-start callback. It records only the
           module identity and wakes this already-created worker; controller,
           file-system and LoadExec work never runs in the loader callback.
           A confirmation press distinguishes a real XMB activation from an
           incidental HTML-viewer load during VSH startup. */
        SceCtrlData controller = {0};
        int sampled = sceCtrlPeekBufferPositive(&controller, 1);
        if (!tilefinch_xmb_redirect_should_attempt(
                TILEFINCH_XMB_BROWSER_MODULE,
                sampled == 1,
                sampled == 1 ? controller.Buttons : 0u,
                TILEFINCH_XMB_REDIRECT_ACTIVATION_BUTTONS,
                TILEFINCH_XMB_REDIRECT_BYPASS_BUTTON,
                false)) {
            launch_armed = 0;
            continue;
        }

        /* Let the handler chain and Sony module_start complete before tearing
           down VSH. The former 20 ms handoff could race module initialization
           on hardware; a quarter second is bounded and still well below the
           stock browser's useful startup time. */
        if (sceKernelDelayThread(
                TILEFINCH_XMB_REDIRECT_MODULE_SETTLE_US) < 0) {
            launch_armed = 0;
            continue;
        }

        char launcher[sizeof(tilefinch_ms_launcher)];
        if (!installed_launcher(launcher)) {
            launch_armed = 0;
            continue;
        }

        SceKernelLoadExecVSHParam parameters = {
            .size = sizeof(parameters),
            .args = 0u,
            .argp = (void *) launcher,
            .key = "game",
            .unk5 = 0x10000u
        };
        while (launcher[parameters.args] != '\0') parameters.args++;
        parameters.args++;

        int result = launcher[0] == 'e'
            ? sctrlKernelLoadExecVSHEf2(launcher, &parameters)
            : sctrlKernelLoadExecVSHMs2(launcher, &parameters);

        /* A successful LoadExec never returns. Failure leaves Sony's browser
           live and rearms the worker for a later deliberate activation. */
        (void) result;
        launch_armed = 0;
    }
}

static int on_module_start(SceModule *module)
{
    /* Chain first. The previous owner may still be patching this module, and
       no Tilefinch thread may run until that handler has returned. */
    int previous_result = previous_start_handler != NULL
        ? previous_start_handler(module) : 0;

    if (tilefinch_xmb_redirect_module_matches(
            module != NULL ? module->modname : NULL,
            launch_armed != 0)) {
        launch_armed = 1;
        if (redirect_event < 0
            || sceKernelSetEventFlag(
                   redirect_event, TILEFINCH_XMB_REDIRECT_EVENT) < 0) {
            launch_armed = 0;
        }
    }

    return previous_result;
}

int module_start(SceSize argument_size, void *argument)
{
    (void) argument_size;
    (void) argument;
    launch_armed = 0;

    redirect_event = sceKernelCreateEventFlag(
        "TilefinchXmbRedirectEvent", 0, 0u, NULL);
    if (redirect_event < 0) return 0;
    SceUID thread = sceKernelCreateThread(
        "TilefinchXmbRedirect",
        redirect_thread,
        TILEFINCH_XMB_REDIRECT_THREAD_PRIORITY,
        TILEFINCH_XMB_REDIRECT_THREAD_STACK,
        0,
        NULL);
    if (thread < 0 || sceKernelStartThread(thread, 0, NULL) < 0) {
        if (thread >= 0) sceKernelDeleteThread(thread);
        sceKernelDeleteEventFlag(redirect_event);
        redirect_event = -1;
        return 0;
    }

    /* Register only after the worker is ready. From this point the loader
       callback merely chains and signals an event; it never allocates. */
    STMOD_HANDLER previous =
        sctrlHENSetStartModuleHandler(on_module_start);
    /* SINGLE_LOAD should make this impossible, but a warm VSH restart may
       retain a NO_STOP kernel plugin. Never turn an accidental second start
       into an infinitely recursive handler chain. A full reboot remains the
       supported way to replace this resident PRX. */
    previous_start_handler = previous == on_module_start ? NULL : previous;
    return 0;
}
