#include "tilefinch/swdec_component_store.h"

#include <stdio.h>
#include <string.h>

bool tilefinch_swdec_component_path(
    const TilefinchInstallPaths *paths, const char *name,
    char *output, size_t output_size)
{
    if (name == NULL || name[0] == '\0' || strchr(name, '/') != NULL)
        return false;
    char relative[96];
    int written = snprintf(
        relative, sizeof(relative), "%s/%s",
        TILEFINCH_SWDEC_COMPONENT_DIRECTORY, name);
    return written > 0 && (size_t) written < sizeof(relative)
        && tilefinch_install_component_path(
            paths, relative, output, output_size);
}

bool tilefinch_swdec_component_prx_path_for_program(
    const char *argv0, char *output, size_t output_size)
{
    static const char suffix[] = "/components/"
        TILEFINCH_SWDEC_COMPONENT_DIRECTORY "/tilefinch-swdec.prx";
    TilefinchInstallPaths paths;
    if (output == NULL || output_size == 0
        || !tilefinch_install_paths_derive(argv0, &paths)) return false;
    size_t root = strlen(paths.install_root);
    if (root >= output_size || sizeof(suffix) > output_size - root)
        return false;
    memcpy(output, paths.install_root, root);
    memcpy(output + root, suffix, sizeof(suffix));
    return true;
}

TilefinchSwdecComponentInfoStatus tilefinch_swdec_component_info_parse(
    const char *bytes, size_t length, uint16_t *abi_version)
{
    static const char prefix[] =
        TILEFINCH_SWDEC_COMPONENT_INFO_MAGIC "\nabi=";
    if (abi_version != NULL) *abi_version = 0;
    if (bytes == NULL || length == 0)
        return TILEFINCH_SWDEC_COMPONENT_INFO_ABSENT;
    if (length <= sizeof(prefix) - 1u
        || memcmp(bytes, prefix, sizeof(prefix) - 1u) != 0)
        return TILEFINCH_SWDEC_COMPONENT_INFO_INVALID;
    size_t at = sizeof(prefix) - 1u;
    unsigned value = 0;
    size_t digits = 0;
    while (at < length && bytes[at] >= '0' && bytes[at] <= '9') {
        unsigned digit = (unsigned) (bytes[at] - '0');
        if (value > (UINT16_MAX - digit) / 10u)
            return TILEFINCH_SWDEC_COMPONENT_INFO_INVALID;
        value = value * 10u + digit;
        at++;
        digits++;
    }
    if (digits == 0 || value == 0
        || (at < length && bytes[at++] != '\n') || at != length)
        return TILEFINCH_SWDEC_COMPONENT_INFO_INVALID;
    if (abi_version != NULL) *abi_version = (uint16_t) value;
    return TILEFINCH_SWDEC_COMPONENT_INFO_VALID;
}

TilefinchSwdecComponentInfoStatus tilefinch_swdec_component_info_read(
    const TilefinchInstallPaths *paths, uint16_t *abi_version)
{
    char path[TILEFINCH_INSTALL_PATH_LIMIT];
    if (!tilefinch_swdec_component_path(
            paths, TILEFINCH_SWDEC_COMPONENT_INFO_NAME,
            path, sizeof(path)))
        return TILEFINCH_SWDEC_COMPONENT_INFO_ABSENT;
    FILE *file = fopen(path, "rb");
    if (file == NULL) return TILEFINCH_SWDEC_COMPONENT_INFO_ABSENT;
    char bytes[TILEFINCH_SWDEC_COMPONENT_INFO_LIMIT];
    size_t length = fread(bytes, 1u, sizeof(bytes), file);
    int trailing = length == sizeof(bytes) ? fgetc(file) : EOF;
    bool read_ok = !ferror(file) && fclose(file) == 0;
    if (!read_ok || trailing != EOF)
        return TILEFINCH_SWDEC_COMPONENT_INFO_INVALID;
    return tilefinch_swdec_component_info_parse(bytes, length, abi_version);
}

TilefinchSwdecFailure tilefinch_swdec_component_start_failure(
    int start_result, int module_status, bool api_valid)
{
    if (start_result < 0) return TILEFINCH_SWDEC_FAILURE_RUNTIME;
    if (module_status != 0 || !api_valid)
        return TILEFINCH_SWDEC_FAILURE_REBUILD;
    return TILEFINCH_SWDEC_FAILURE_NONE;
}

bool tilefinch_swdec_component_retry_allowed(
    TilefinchSwdecFailure failure, bool module_released, unsigned failures)
{
    return failure == TILEFINCH_SWDEC_FAILURE_RUNTIME && module_released
        && failures < TILEFINCH_SWDEC_COMPONENT_RUNTIME_ATTEMPTS;
}

void tilefinch_swdec_component_failure_message(
    TilefinchSwdecFailure failure, int native_error,
    char *output, size_t output_size)
{
    if (output == NULL || output_size == 0) return;
    switch (failure) {
    case TILEFINCH_SWDEC_FAILURE_MISSING:
        snprintf(output, output_size,
                 "OPTIONAL VIDEO DECODER NOT INSTALLED - SEE README");
        break;
    case TILEFINCH_SWDEC_FAILURE_REBUILD:
        snprintf(output, output_size,
                 "OPTIONAL VIDEO DECODER NEEDS REBUILD - SEE README");
        break;
    case TILEFINCH_SWDEC_FAILURE_RUNTIME:
    case TILEFINCH_SWDEC_FAILURE_NONE:
    default:
        snprintf(output, output_size,
                 "optional video decoder is unavailable (0x%08x)",
                 (unsigned) native_error);
        break;
    }
}
