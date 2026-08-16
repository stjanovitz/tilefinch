#ifndef TILEFINCH_INSTALL_PATHS_H
#define TILEFINCH_INSTALL_PATHS_H

#include <stdbool.h>
#include <stddef.h>

#define TILEFINCH_INSTALL_PATH_LIMIT 768u

typedef struct {
    char program_dir[TILEFINCH_INSTALL_PATH_LIMIT];
    char install_root[TILEFINCH_INSTALL_PATH_LIMIT];
    char data_dir[TILEFINCH_INSTALL_PATH_LIMIT];
    char slot_name[7];
    bool slotted;
} TilefinchInstallPaths;

bool tilefinch_install_paths_derive(
    const char *argv0, TilefinchInstallPaths *paths);
/* The launcher lives at the root of an A/B install, while a legacy browser
   EBOOT can live at the same apparent path. Keep that role distinction
   explicit: launcher mutable state is always below root/data. */
bool tilefinch_install_paths_derive_launcher(
    const char *argv0, TilefinchInstallPaths *paths);
bool tilefinch_install_program_path(
    const TilefinchInstallPaths *paths, const char *relative,
    char *output, size_t output_size);
bool tilefinch_install_data_path(
    const TilefinchInstallPaths *paths, const char *relative,
    char *output, size_t output_size);
bool tilefinch_install_advisory_matches(
    const TilefinchInstallPaths *paths,
    const char *slot_name, const char *data_dir);

#endif
