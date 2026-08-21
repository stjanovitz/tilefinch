/* Ordinary-EBOOT qualification for the optional software decoder.
 *
 * This deliberately does not use PSPLink's module loader.  It proves the
 * shipping topology: a user-mode EBOOT loads the decoder PRX, passes the ABI
 * table through sceKernelStartModule(), then asks that component to load the
 * small resident kernel helper and restore the firmware Media Engine state.
 */

#include <pspdebug.h>
#include <pspkernel.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tilefinch/swdec_component.h"

PSP_MODULE_INFO("Tilefinch swdec probe", 0, 0, 1);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);

#define PROBE_PATH_LIMIT 512u

static int failures;

static void probe_log(const char *format, ...)
{
    char line[256];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    printf("tilefinch-swdec-probe: %s", line);
    pspDebugScreenPrintf("%s", line);
}

static int sibling_path(
    char *output, size_t capacity, const char *argv0, const char *leaf)
{
    if (output == NULL || capacity == 0 || leaf == NULL) return 0;
    const char *source = argv0 != NULL ? argv0 : "EBOOT.PBP";
    const char *slash = strrchr(source, '/');
    size_t directory_bytes = slash != NULL ? (size_t) (slash - source + 1) : 0;
    size_t leaf_bytes = strlen(leaf);
    if (directory_bytes > capacity - 1
        || leaf_bytes > capacity - directory_bytes - 1) return 0;
    memcpy(output, source, directory_bytes);
    memcpy(output + directory_bytes, leaf, leaf_bytes + 1);
    return 1;
}

int main(int argc, char *argv[])
{
    char component_path[PROBE_PATH_LIMIT];
    char helper_path[PROBE_PATH_LIMIT];
    TilefinchSwdecComponentApi api;
    memset(&api, 0, sizeof(api));
    TilefinchSwdecComponentStart start = {
        .magic = TILEFINCH_SWDEC_COMPONENT_MAGIC,
        .abi_version = TILEFINCH_SWDEC_COMPONENT_ABI_VERSION,
        .struct_size = sizeof(start),
        .api = &api
    };

    pspDebugScreenInit();
    pspDebugScreenPrintf("Tilefinch software decoder loader probe\n\n");

    const char *argv0 = argc > 0 ? argv[0] : NULL;
    if (!sibling_path(component_path, sizeof(component_path), argv0,
                      "tilefinch-swdec.prx")
        || !sibling_path(helper_path, sizeof(helper_path), argv0,
                         "swdec-meload.prx")) {
        probe_log("outcome=fail phase=paths\n");
        failures++;
        goto finished;
    }

    SceUID module = sceKernelLoadModule(component_path, 0, NULL);
    probe_log("phase=load-user result=0x%08x\n", (unsigned) module);
    if (module < 0) {
        failures++;
        goto finished;
    }

    int start_status = -1;
    int result = sceKernelStartModule(
        module, sizeof(start), &start, &start_status, NULL);
    probe_log("phase=start-user result=0x%08x status=%d api=0x%08x\n",
              (unsigned) result, start_status, (unsigned) api.magic);
    if (result < 0 || start_status != 0
        || api.magic != TILEFINCH_SWDEC_COMPONENT_MAGIC
        || api.abi_version != TILEFINCH_SWDEC_COMPONENT_ABI_VERSION
        || api.struct_size < sizeof(api)
        || api.attach_me == NULL || api.detach_me == NULL
        || api.restore_me == NULL || api.audio_shutdown == NULL) {
        failures++;
        goto finished;
    }

    result = api.attach_me(helper_path, probe_log);
    probe_log("phase=attach-me result=%d failed=%d\n",
              result, api.me_failed != NULL ? api.me_failed() : -1);
    if (result != 0) failures++;
    if (result == 0) {
        api.detach_me();
        result = api.restore_me();
        probe_log("phase=detach-restore-me result=%d\n", result);
        if (result != 0) failures++;
    }

finished:
    probe_log("outcome=%s failures=%d\n", failures == 0 ? "pass" : "fail",
              failures);
    pspDebugScreenPrintf("\nClosing automatically.\n");
    sceKernelDelayThread(2000000);
    sceKernelExitGame();
    return failures == 0 ? 0 : 1;
}
