#ifndef TILEFINCH_SWDEC_COMPONENT_STORE_H
#define TILEFINCH_SWDEC_COMPONENT_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/install_paths.h"

#define TILEFINCH_SWDEC_COMPONENT_DIRECTORY "swdec"
#define TILEFINCH_SWDEC_COMPONENT_INFO_NAME "component-info.txt"
#define TILEFINCH_SWDEC_COMPONENT_INFO_MAGIC "tilefinch-swdec-component-v1"
#define TILEFINCH_SWDEC_COMPONENT_INFO_LIMIT 96u

typedef enum {
    TILEFINCH_SWDEC_COMPONENT_INFO_ABSENT = 0,
    TILEFINCH_SWDEC_COMPONENT_INFO_VALID,
    TILEFINCH_SWDEC_COMPONENT_INFO_INVALID
} TilefinchSwdecComponentInfoStatus;

bool tilefinch_swdec_component_path(
    const TilefinchInstallPaths *paths, const char *name,
    char *output, size_t output_size);
TilefinchSwdecComponentInfoStatus tilefinch_swdec_component_info_parse(
    const char *bytes, size_t length, uint16_t *abi_version);
TilefinchSwdecComponentInfoStatus tilefinch_swdec_component_info_read(
    const TilefinchInstallPaths *paths, uint16_t *abi_version);

#endif
