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
/* Optional components are installation-wide siblings of the A/B slots.
   Browser updates replace only a program slot and therefore never overwrite
   or remove a component the user built or installed separately. */
bool tilefinch_install_component_path(
    const TilefinchInstallPaths *paths, const char *relative,
    char *output, size_t output_size);
bool tilefinch_install_advisory_matches(
    const TilefinchInstallPaths *paths,
    const char *slot_name, const char *data_dir);

/* Flushes the Memory Stick file system: sceIoSync on "ms0:", then on
   "msfat0:" when the first name is refused. Some real firmware and
   storage-driver combinations reject both even though every checked write,
   close and rename before the call succeeded (PPSSPP accepts them), so
   callers must not treat REJECTED as the failure of work that already
   completed. A host build has no device to flush and reports SKIPPED. */
typedef enum {
    TILEFINCH_MEMORY_STICK_SYNCED = 0,
    TILEFINCH_MEMORY_STICK_SYNC_REJECTED,
    TILEFINCH_MEMORY_STICK_SYNC_SKIPPED
} TilefinchMemoryStickSync;

TilefinchMemoryStickSync tilefinch_memory_stick_sync(void);
#ifndef __PSP__
/* Host tests only: the result every later tilefinch_memory_stick_sync()
   reports, so callers' handling of a rejecting device can run off-device. */
void tilefinch_memory_stick_sync_set_host_result(
    TilefinchMemoryStickSync result);
#endif

#endif
