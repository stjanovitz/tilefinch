#ifndef TILEFINCH_COMPONENT_STORE_H
#define TILEFINCH_COMPONENT_STORE_H

/* Memory Stick file steps shared by the installers that write below the
   install root: language packs (glyph_component_store.c), the voice model
   (voice_component.c) and browser updates (update_installer.c). Paths are
   bounded by TILEFINCH_INSTALL_PATH_LIMIT.

   Renames stay within one directory. The PSP firmware's rename() reports
   success for a target in another directory but leaves the file in the
   source directory under the target's name, and PPSSPP drops the target's
   directory the same way, so tilefinch_component_store_rename refuses any
   pair of paths whose parent directories differ. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"

/* DIRECTORY "/" RELATIVE into OUTPUT; false when an argument is missing or
   the result does not fit. */
bool tilefinch_component_store_join(
    const char *directory, const char *relative,
    char *output, size_t output_size);

bool tilefinch_component_store_exists(const char *path);

/* Creates every ancestor directory of PATH, not PATH itself. The leading
   "/" and a device root such as "ms0:/" are skipped, and directories that
   already exist are fine. PATH is restored before returning. */
bool tilefinch_component_store_make_parents(char *path);

/* Creates PATH and its ancestors; an existing directory is fine. */
bool tilefinch_component_store_make_directory(const char *path);

/* Removes PATH and everything below it, refusing to go deeper than
   DEPTH_LIMIT levels or to visit more than ENTRY_LIMIT entries. A missing
   PATH counts as removed. Symbolic links are unlinked, never followed. */
bool tilefinch_component_store_remove_tree(
    const char *path, unsigned depth_limit, size_t entry_limit);

/* Writes LENGTH bytes to PATH, replacing it, then flushes; on the host the
   file is also fsynced. The Memory Stick sync after a checked write is a
   durability hint and never fails it (tilefinch_memory_stick_sync). */
bool tilefinch_component_store_write_file(
    const char *path, const void *bytes, size_t length);

/* True when PATH holds exactly the LENGTH bytes at EXPECTED. */
bool tilefinch_component_store_file_matches(
    const char *path, const void *expected, size_t length);

/* Reads the whole of PATH into a session buffer charged to BUDGET. NULL
   when the file is missing, empty, longer than LIMIT, unreadable, or the
   budget refuses it. The caller frees the buffer with budget_free. */
uint8_t *tilefinch_component_store_read_file(
    Budget *budget, const char *path, size_t limit, size_t *length);

/* rename(FROM, TO) when both name entries of the same directory. Otherwise
   it does nothing, sets errno to EXDEV and returns false. When rename()
   itself fails, errno is what rename() set. */
bool tilefinch_component_store_rename(const char *from, const char *to);

#endif
