#include "tilefinch/update.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#if defined(__PSP__)
#include <pspiofilemgr.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

static bool journal_path(
    char *output, size_t size, const char *directory,
    unsigned copy, bool temporary)
{
    static const char suffixes[2][2][20] = {
        {"update-state.0", "update-state.0.tmp"},
        {"update-state.1", "update-state.1.tmp"}
    };
    if (output == NULL || directory == NULL || copy > 1) return false;
    const char *suffix = suffixes[copy][temporary ? 1 : 0];
    size_t directory_length = strlen(directory);
    size_t suffix_length = strlen(suffix);
    if (directory_length + 1u + suffix_length >= size) return false;
    memcpy(output, directory, directory_length);
    output[directory_length] = '/';
    memcpy(output + directory_length + 1u, suffix, suffix_length + 1u);
    return true;
}

static size_t journal_read(
    const char *path, uint8_t output[TILEFINCH_UPDATE_STATE_BYTES])
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return 0;
    size_t length = fread(output, 1, TILEFINCH_UPDATE_STATE_BYTES, file);
    int extra = fgetc(file);
    if (fclose(file) != 0 || extra != EOF) return 0;
    return length;
}

bool tilefinch_update_journal_load(
    const char *data_dir, TilefinchUpdateState *state,
    unsigned *selected_copy)
{
    if (data_dir == NULL || state == NULL) return false;
    char paths[2][768];
    uint8_t records[2][TILEFINCH_UPDATE_STATE_BYTES];
    size_t lengths[2] = {0, 0};
    for (unsigned copy = 0; copy < 2; copy++) {
        if (!journal_path(
                paths[copy], sizeof(paths[copy]), data_dir, copy, false))
            return false;
        lengths[copy] = journal_read(paths[copy], records[copy]);
    }
    return tilefinch_update_state_select(
        records[0], lengths[0], records[1], lengths[1],
        state, selected_copy);
}

static bool journal_fault(
    TilefinchUpdateFaultHook fault, void *opaque, const char *operation)
{
    return fault != NULL && fault(opaque, operation);
}

static bool journal_sync_file(FILE *file)
{
    if (fflush(file) != 0) return false;
#if defined(__PSP__)
    return true;
#else
    return fsync(fileno(file)) == 0;
#endif
}

static bool journal_sync_directory(const char *directory)
{
#if defined(__PSP__)
    (void) directory;
    return sceIoSync("ms0:", 0) >= 0;
#else
    int descriptor = open(directory, O_RDONLY);
    if (descriptor < 0) return false;
    bool ok = fsync(descriptor) == 0;
    close(descriptor);
    return ok;
#endif
}

bool tilefinch_update_journal_store(
    const char *data_dir, const TilefinchUpdateState *state,
    TilefinchUpdateFaultHook fault, void *fault_opaque)
{
    if (data_dir == NULL || state == NULL) return false;
    uint8_t encoded[TILEFINCH_UPDATE_STATE_BYTES];
    if (tilefinch_update_state_encode(state, encoded) != TILEFINCH_UPDATE_OK)
        return false;
    TilefinchUpdateState current;
    unsigned selected = 1;
    bool have_current = tilefinch_update_journal_load(
        data_dir, &current, &selected);
    if (have_current && state->generation <= current.generation) return false;
    unsigned target = have_current ? selected ^ 1u : 0u;
    char path[768], temporary[768];
    if (!journal_path(path, sizeof(path), data_dir, target, false)
        || !journal_path(
               temporary, sizeof(temporary), data_dir, target, true))
        return false;
    if (journal_fault(fault, fault_opaque, "before-open")) return false;
    FILE *file = fopen(temporary, "wb");
    if (file == NULL) return false;
    bool ok = !journal_fault(fault, fault_opaque, "before-write")
        && fwrite(encoded, 1, sizeof(encoded), file) == sizeof(encoded)
        && !journal_fault(fault, fault_opaque, "before-file-sync")
        && journal_sync_file(file);
    if (fclose(file) != 0) ok = false;
    if (!ok) {
        remove(temporary);
        return false;
    }
    /* PSP FAT does not replace an existing destination with rename().  The
       target is deliberately the *unselected* copy, so removing that stale
       generation cannot destroy the currently selected healthy state.  This
       matters on the third and every later journal write: POSIX hosts replace
       copy 0/1 silently, while real firmware otherwise returns an error and
       leaves the updater unable to advance its trial state. */
    if (journal_fault(fault, fault_opaque, "before-target-remove")) {
        remove(temporary);
        return false;
    }
    if (remove(path) != 0 && errno != ENOENT) {
        remove(temporary);
        return false;
    }
    if (journal_fault(fault, fault_opaque, "after-target-remove")
        || journal_fault(fault, fault_opaque, "before-rename")
        || rename(temporary, path) != 0) {
        remove(temporary);
        return false;
    }
    if (journal_fault(fault, fault_opaque, "before-directory-sync")
        || !journal_sync_directory(data_dir)) return false;
    return true;
}
