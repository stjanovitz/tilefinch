#include "tilefinch/psp_swdec_component.h"

#include <pspkernel.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tilefinch/psp_log.h"
#include "tilefinch/swdec_component_store.h"

#define printf psp_log_printf

/* Loading the component charges the page Budget nothing. The user PRX
   (1,970,644 bytes loaded for the release build, 2,036,356 with the
   validation diagnostics and the reference CABAC copy) and its start thread come from user-partition
   memory that boot leaves outside the newlib heap whenever the add-on is
   installed (psp_heap_reserve.c); the resident helper (about 3 KiB) loads
   into kernel memory. The component's own C library allocates from a 16 KiB
   pool inside its image (swdec_component_psp.c), and every decoder buffer
   is taken from the Budget when a stream opens (media_backend_psp_swdec.c).
   The 5 MiB this used to reserve predates both: it charged pages for memory
   the heap never gave up. */

#ifdef TILEFINCH_PSP_VALIDATION_LOG
/* boot.cfg validation_swdec_* flags for the component's start block,
   published at boot before anything can open a video. */
static uint32_t component_validation_flags;

void psp_swdec_component_set_validation_flags(uint32_t flags)
{
    component_validation_flags = flags;
}

static volatile uint32_t component_window_mark;

void psp_swdec_component_window_step(void)
{
    component_window_mark++;
}
#endif

static void component_error(
    char *error, size_t error_size, const char *format, ...)
{
    if (error == NULL || error_size == 0) return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

static void component_log(const char *format, ...)
{
    /* The validation component's stage report runs to about 330 bytes; a
       256-byte line cut its last fields and its newline. */
    char line[512];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    printf("tilefinch-swdec: %s", line);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    /* Component lines are rare and often the last words before a Media
       Engine stall takes the device down; push each one to the log file. */
    (void) psp_log_flush(true);
#endif
}

static bool component_file_exists(const char *path)
{
    FILE *file = path == NULL ? NULL : fopen(path, "rb");
    if (file == NULL) return false;
    fclose(file);
    return true;
}

/* Release the user module only while it cannot own the Media Engine. A stop or
   unload failure keeps the module id: firmware still owns its memory, so the
   component must not be loaded again on top of it. */
static bool component_unload_module(PspSwdecComponent *component)
{
    if (component == NULL || component->module_id < 0 || component->took_me)
        return component != NULL && component->module_id < 0;
    if (component->module_started) {
        int module_status = 0;
        int stopped = sceKernelStopModule(
            component->module_id, 0, NULL, &module_status, NULL);
        if (stopped < 0 || module_status != 0) {
            component->last_native_error = stopped < 0
                ? stopped : module_status;
            return false;
        }
        component->module_started = false;
    }
    int unloaded = sceKernelUnloadModule(component->module_id);
    if (unloaded < 0) {
        component->last_native_error = unloaded;
        return false;
    }
    component->module_id = -1;
    memset(&component->api, 0, sizeof(component->api));
    return true;
}

static void component_failed_message(
    const PspSwdecComponent *component,
    char *error, size_t error_size)
{
    tilefinch_swdec_component_failure_message(
        component == NULL ? TILEFINCH_SWDEC_FAILURE_RUNTIME
                          : component->failure,
        component == NULL ? 0 : component->last_native_error,
        error, error_size);
}

static bool component_program_path(
    PspSwdecComponent *component, const char *name,
    char *path, size_t path_size)
{
    return component != NULL && (component->shared_install
        ? tilefinch_swdec_component_path(
              component->install_paths, name, path, path_size)
        : tilefinch_install_program_path(
              component->install_paths, name, path, path_size));
}

void psp_swdec_component_init(
    PspSwdecComponent *component, Budget *budget,
    const TilefinchInstallPaths *install_paths)
{
    if (component == NULL) return;
    memset(component, 0, sizeof(*component));
    component->budget = budget;
    component->install_paths = install_paths;
    component->module_id = -1;
    component->state = PSP_SWDEC_COMPONENT_COLD;
}

static bool psp_swdec_component_load(
    PspSwdecComponent *component, char *error, size_t error_size)
{
    if (component == NULL) return false;
    if (component->state == PSP_SWDEC_COMPONENT_LOADED
        || component->state == PSP_SWDEC_COMPONENT_ATTACHED
        || component->state == PSP_SWDEC_COMPONENT_RESTORED) return true;
    if (component->state == PSP_SWDEC_COMPONENT_FAILED) {
        component_failed_message(component, error, error_size);
        return false;
    }
    char path[TILEFINCH_INSTALL_PATH_LIMIT];
    component->shared_install = tilefinch_swdec_component_path(
        component->install_paths, "tilefinch-swdec.prx",
        path, sizeof(path)) && component_file_exists(path);
    if (component->shared_install) {
        uint16_t installed_abi = 0;
        TilefinchSwdecComponentInfoStatus info =
            tilefinch_swdec_component_info_read(
                component->install_paths, &installed_abi);
        if (info != TILEFINCH_SWDEC_COMPONENT_INFO_VALID
            || installed_abi != TILEFINCH_SWDEC_COMPONENT_ABI_VERSION) {
            component->failure = PSP_SWDEC_COMPONENT_FAILURE_REBUILD;
            component->state = PSP_SWDEC_COMPONENT_FAILED;
            component_failed_message(component, error, error_size);
            return false;
        }
    } else if (!tilefinch_install_program_path(
                   component->install_paths, "tilefinch-swdec.prx",
                   path, sizeof(path)) || !component_file_exists(path)) {
        component->failure = PSP_SWDEC_COMPONENT_FAILURE_MISSING;
        component->state = PSP_SWDEC_COMPONENT_FAILED;
        component_failed_message(component, error, error_size);
        return false;
    }
    char helper[TILEFINCH_INSTALL_PATH_LIMIT];
    if (!component_program_path(
            component, "swdec-meload.prx", helper, sizeof(helper))
        || !component_file_exists(helper)) {
        component->failure = PSP_SWDEC_COMPONENT_FAILURE_REBUILD;
        component->state = PSP_SWDEC_COMPONENT_FAILED;
        component_error(error, error_size,
                        "OPTIONAL VIDEO DECODER IS INCOMPLETE - SEE README");
        return false;
    }
    if (!component_program_path(
            component, "tilefinch-swdec.prx", path, sizeof(path))) {
        component->state = PSP_SWDEC_COMPONENT_FAILED;
        component->failure = PSP_SWDEC_COMPONENT_FAILURE_RUNTIME;
        component_error(error, error_size,
                        "software decoder component path is unavailable");
        return false;
    }
    int module = sceKernelLoadModule(path, 0, NULL);
    if (module < 0) {
        component->last_native_error = module;
        component->state = PSP_SWDEC_COMPONENT_FAILED;
        component->failure = PSP_SWDEC_COMPONENT_FAILURE_RUNTIME;
        component_error(error, error_size,
                        "software decoder load failed (0x%08x)",
                        (unsigned) module);
        return false;
    }
    component->module_id = module;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    /* The longer start block carries the validation flags (same ABI). */
    TilefinchSwdecComponentValidationStart start = {
        .start = {
            .magic = TILEFINCH_SWDEC_COMPONENT_MAGIC,
            .abi_version = TILEFINCH_SWDEC_COMPONENT_ABI_VERSION,
            .struct_size = sizeof(start),
            .api = &component->api
        },
        .validation_flags = component_validation_flags,
        .window_mark = &component_window_mark
    };
    if (component_validation_flags != 0)
        component_log("validation flags 0x%x\n",
                      (unsigned) component_validation_flags);
#else
    TilefinchSwdecComponentStart start = {
        .magic = TILEFINCH_SWDEC_COMPONENT_MAGIC,
        .abi_version = TILEFINCH_SWDEC_COMPONENT_ABI_VERSION,
        .struct_size = sizeof(start),
        .api = &component->api
    };
#endif
    int status = -1;
    int result = sceKernelStartModule(
        module, sizeof(start), &start, &status, NULL);
    component->module_started = result >= 0;
    bool api_valid = result >= 0 && status == 0
        && component->api.magic == TILEFINCH_SWDEC_COMPONENT_MAGIC
        && component->api.abi_version
             == TILEFINCH_SWDEC_COMPONENT_ABI_VERSION
        && component->api.struct_size >= sizeof(component->api)
        && component->api.attach_me != NULL
        && component->api.detach_me != NULL
        && component->api.restore_me != NULL
        && component->api.arena_bytes != NULL
        && component->api.open != NULL
        && component->api.decode != NULL
        && component->api.set_speed != NULL
        && component->api.close != NULL
        && component->api.bind_aux_arena != NULL
        && component->api.recover_me != NULL
        && component->api.me_failed != NULL
        && component->api.audio_setup != NULL
        && component->api.audio_submit != NULL
        && component->api.audio_done != NULL
        && component->api.audio_pcm != NULL
        && component->api.audio_reset != NULL
        && component->api.audio_shutdown != NULL
        && component->api.csc_begin != NULL
        && component->api.csc_picture != NULL
        && component->api.csc_close != NULL
        && component->api.csc_off != NULL;
    TilefinchSwdecFailure start_failure =
        tilefinch_swdec_component_start_failure(result, status, api_valid);
    if (start_failure != TILEFINCH_SWDEC_FAILURE_NONE) {
        /* A firmware refusal to start the module (memory for its start
           thread, most often) is not a sign of a stale component: only
           module_start's own refusal or a foreign API table are. */
        component->state = PSP_SWDEC_COMPONENT_FAILED;
        component->failure = start_failure;
        memset(&component->api, 0, sizeof(component->api));
        (void) component_unload_module(component);
        component->last_native_error = result < 0 ? result : status;
        if (start_failure == TILEFINCH_SWDEC_FAILURE_RUNTIME)
            component_error(error, error_size,
                            "software decoder start failed (0x%08x)",
                            (unsigned) result);
        else
            component_failed_message(component, error, error_size);
        return false;
    }
    component->state = PSP_SWDEC_COMPONENT_LOADED;
    printf("tilefinch-swdec: event=component-loaded module=0x%08x\n",
           (unsigned) module);
    return true;
}

bool psp_swdec_component_prepare(
    PspSwdecComponent *component, char *error, size_t error_size)
{
    if (component == NULL) return false;
    if (component->state == PSP_SWDEC_COMPONENT_ATTACHED) return true;
    if (!psp_swdec_component_load(component, error, error_size)) return false;
    char helper[TILEFINCH_INSTALL_PATH_LIMIT];
    if (!component_program_path(
            component, "swdec-meload.prx", helper, sizeof(helper))
        || !component_file_exists(helper)) {
        component_error(error, error_size,
                        "OPTIONAL VIDEO DECODER IS INCOMPLETE - SEE README");
        component->state = PSP_SWDEC_COMPONENT_FAILED;
        component->failure = PSP_SWDEC_COMPONENT_FAILURE_REBUILD;
        (void) component_unload_module(component);
        return false;
    }
    /* From the first attach call onward, assume the component may own ME state
       until restore proves otherwise. This is conservative for an early
       failure and prevents failure cleanup from unloading code after a
       partial takeover. */
    component->took_me = true;
    int result = component->api.attach_me(helper, component_log);
    if (result != 0) {
        component->last_native_error = result;
        /* Attach can fail after the helper patched the firmware preamble.
           Restore unconditionally before declaring this route unavailable. */
        component->api.detach_me();
        int restored = component->api.restore_me();
        if (restored == 0 || restored == 1) {
            component->took_me = false;
            (void) component_unload_module(component);
        }
        component->state = PSP_SWDEC_COMPONENT_FAILED;
        component->failure = PSP_SWDEC_COMPONENT_FAILURE_RUNTIME;
        component_error(error, error_size,
                        "software decoder ME attach failed (%d)", result);
        return false;
    }
    component->state = PSP_SWDEC_COMPONENT_ATTACHED;
    return true;
}

bool psp_swdec_component_suspend(
    PspSwdecComponent *component, char *error, size_t error_size)
{
    if (component == NULL || !component->took_me) return true;
    if (component->state == PSP_SWDEC_COMPONENT_ATTACHED)
        component->api.detach_me();
    int result = component->api.restore_me != NULL
        ? component->api.restore_me() : -1;
    if (result != 0 && result != 1) {
        component->last_native_error = result;
        component->state = PSP_SWDEC_COMPONENT_FAILED;
        component_error(error, error_size,
                        "software decoder ME restore failed (%d)", result);
        return false;
    }
    component->state = PSP_SWDEC_COMPONENT_RESTORED;
    return true;
}

bool psp_swdec_component_resume(
    PspSwdecComponent *component, char *error, size_t error_size)
{
    if (component == NULL || !component->took_me) return true;
    return psp_swdec_component_prepare(component, error, error_size);
}

void psp_swdec_component_shutdown(PspSwdecComponent *component)
{
    if (component == NULL) return;
    if (component->took_me
        && component->state == PSP_SWDEC_COMPONENT_ATTACHED)
        component->api.detach_me();
    bool restored = !component->took_me;
    if (component->took_me && component->api.restore_me != NULL) {
        int result = component->api.restore_me();
        restored = result == 0 || result == 1;
        if (!restored) component->last_native_error = result;
    }
    if (restored) {
        component->took_me = false;
        component->state = PSP_SWDEC_COMPONENT_RESTORED;
        (void) component_unload_module(component);
    }
}

bool psp_swdec_component_owns_me(const PspSwdecComponent *component)
{
    return component != NULL && component->took_me;
}

bool psp_swdec_component_attached(const PspSwdecComponent *component)
{
    return component != NULL
        && component->state == PSP_SWDEC_COMPONENT_ATTACHED;
}

const TilefinchSwdecComponentApi *psp_swdec_component_api(
    const PspSwdecComponent *component)
{
    return psp_swdec_component_attached(component)
        ? &component->api : NULL;
}
