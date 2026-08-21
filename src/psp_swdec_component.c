#include "tilefinch/psp_swdec_component.h"

#include <pspkernel.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tilefinch/psp_log.h"
#include "tilefinch/swdec_component_store.h"

#define printf psp_log_printf

/* psp-size currently reports 4,751,096 bytes for the user component and
   2,717 bytes for the resident helper. Keep modest loader/alignment headroom
   and fail admission before asking the firmware for untracked memory. */
#define PSP_SWDEC_COMPONENT_RESIDENT_BYTES (5u * 1024u * 1024u)

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
    char line[256];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    printf("tilefinch-swdec: %s", line);
}

static bool component_file_exists(const char *path)
{
    FILE *file = path == NULL ? NULL : fopen(path, "rb");
    if (file == NULL) return false;
    fclose(file);
    return true;
}

static void component_failed_message(
    const PspSwdecComponent *component,
    char *error, size_t error_size)
{
    if (component != NULL
        && component->failure == PSP_SWDEC_COMPONENT_FAILURE_MISSING) {
        component_error(error, error_size,
                        "OPTIONAL VIDEO DECODER NOT INSTALLED - SEE README");
    } else if (component != NULL
               && component->failure == PSP_SWDEC_COMPONENT_FAILURE_REBUILD) {
        component_error(error, error_size,
                        "OPTIONAL VIDEO DECODER NEEDS REBUILD - SEE README");
    } else {
        component_error(error, error_size,
                        "optional video decoder is unavailable (0x%08x)",
                        component == NULL ? 0u
                            : (unsigned) component->last_native_error);
    }
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
    if (!budget_reservation_acquire(
            &component->resident_reservation, component->budget,
            BUDGET_CATEGORY_RESOURCE,
            PSP_SWDEC_COMPONENT_RESIDENT_BYTES)) {
        component->state = PSP_SWDEC_COMPONENT_FAILED;
        component->failure = PSP_SWDEC_COMPONENT_FAILURE_RUNTIME;
        component_error(error, error_size,
                        "software decoder component exceeds memory budget");
        return false;
    }
    if (!component_program_path(
            component, "tilefinch-swdec.prx", path, sizeof(path))) {
        component->state = PSP_SWDEC_COMPONENT_FAILED;
        component->failure = PSP_SWDEC_COMPONENT_FAILURE_RUNTIME;
        budget_reservation_release(&component->resident_reservation);
        component_error(error, error_size,
                        "software decoder component path is unavailable");
        return false;
    }
    int module = sceKernelLoadModule(path, 0, NULL);
    if (module < 0) {
        component->last_native_error = module;
        component->state = PSP_SWDEC_COMPONENT_FAILED;
        component->failure = PSP_SWDEC_COMPONENT_FAILURE_RUNTIME;
        budget_reservation_release(&component->resident_reservation);
        component_error(error, error_size,
                        "software decoder load failed (0x%08x)",
                        (unsigned) module);
        return false;
    }
    TilefinchSwdecComponentStart start = {
        .magic = TILEFINCH_SWDEC_COMPONENT_MAGIC,
        .abi_version = TILEFINCH_SWDEC_COMPONENT_ABI_VERSION,
        .struct_size = sizeof(start),
        .api = &component->api
    };
    int status = -1;
    int result = sceKernelStartModule(
        module, sizeof(start), &start, &status, NULL);
    if (result < 0 || status != 0
        || component->api.magic != TILEFINCH_SWDEC_COMPONENT_MAGIC
        || component->api.abi_version
             != TILEFINCH_SWDEC_COMPONENT_ABI_VERSION
        || component->api.struct_size < sizeof(component->api)
        || component->api.attach_me == NULL
        || component->api.detach_me == NULL
        || component->api.restore_me == NULL
        || component->api.arena_bytes == NULL
        || component->api.open == NULL
        || component->api.decode == NULL
        || component->api.set_speed == NULL
        || component->api.close == NULL
        || component->api.bind_aux_arena == NULL
        || component->api.recover_me == NULL
        || component->api.me_failed == NULL
        || component->api.audio_setup == NULL
        || component->api.audio_submit == NULL
        || component->api.audio_done == NULL
        || component->api.audio_pcm == NULL
        || component->api.audio_reset == NULL
        || component->api.audio_shutdown == NULL
        || component->api.csc_begin == NULL
        || component->api.csc_close == NULL
        || component->api.csc_off == NULL) {
        component->last_native_error = result < 0 ? result : status;
        component->state = PSP_SWDEC_COMPONENT_FAILED;
        component->failure = PSP_SWDEC_COMPONENT_FAILURE_REBUILD;
        memset(&component->api, 0, sizeof(component->api));
        component_failed_message(component, error, error_size);
        return false;
    }
    component->module_id = module;
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
        return false;
    }
    int result = component->api.attach_me(helper, component_log);
    if (result != 0) {
        component->last_native_error = result;
        /* Attach can fail after the helper patched the firmware preamble.
           Restore unconditionally before declaring this route unavailable. */
        component->api.detach_me();
        (void) component->api.restore_me();
        component->state = PSP_SWDEC_COMPONENT_FAILED;
        component->failure = PSP_SWDEC_COMPONENT_FAILURE_RUNTIME;
        component_error(error, error_size,
                        "software decoder ME attach failed (%d)", result);
        return false;
    }
    component->took_me = true;
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
    if (component->took_me && component->api.restore_me != NULL)
        (void) component->api.restore_me();
    if (component->took_me)
        component->state = PSP_SWDEC_COMPONENT_RESTORED;
    budget_reservation_release(&component->resident_reservation);
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
