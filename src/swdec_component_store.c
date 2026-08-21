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
