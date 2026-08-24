#include "tilefinch/update.h"

#include <string.h>
#include <sys/statvfs.h>

_Static_assert(sizeof(((struct statvfs *) 0)->f_bavail) <= sizeof(uint64_t),
               "statvfs available-block count must fit uint64_t");
_Static_assert(sizeof(((struct statvfs *) 0)->f_frsize) <= sizeof(uint64_t),
               "statvfs fragment size must fit uint64_t");
_Static_assert(sizeof(((struct statvfs *) 0)->f_bsize) <= sizeof(uint64_t),
               "statvfs block size must fit uint64_t");

static bool system_space_query(
    void *opaque, const char *path, uint64_t *blocks_available,
    uint64_t *fragment_size, uint64_t *block_size)
{
    (void) opaque;
    struct statvfs status;
    if (path == NULL || blocks_available == NULL || fragment_size == NULL
        || block_size == NULL || statvfs(path, &status) != 0) return false;
    *blocks_available = (uint64_t) status.f_bavail;
    *fragment_size = (uint64_t) status.f_frsize;
    *block_size = (uint64_t) status.f_bsize;
    return true;
}

bool tilefinch_update_query_free_space_with(
    const char *directory, uint64_t *available,
    TilefinchUpdateSpaceQuery query, void *opaque)
{
    if (directory == NULL || available == NULL || query == NULL) return false;
    const char *query_path = directory;
#if defined(__PSP__)
    /*
     * PSP newlib forwards this string directly to SCE_PR_GETDEV. The device
     * command accepts "ms0:", not a directory such as
     * "ms0:/PSP/GAME/TILEFINCH"; the latter is rejected by both PPSSPP and
     * the device service. Host statvfs(), by contrast, needs the full path.
     */
    char device[16];
    const char *colon = strchr(directory, ':');
    if (colon != NULL) {
        size_t length = (size_t) (colon - directory) + 1u;
        if (length >= sizeof(device)) return false;
        memcpy(device, directory, length);
        device[length] = '\0';
        query_path = device;
    }
#endif
    uint64_t blocks = 0, fragment = 0, filesystem_block = 0;
    if (!query(opaque, query_path, &blocks, &fragment, &filesystem_block))
        return false;
    uint64_t block = fragment != 0 ? fragment : filesystem_block;
    if (block == 0 || blocks > UINT64_MAX / block) return false;
    *available = blocks * block;
    return true;
}

bool tilefinch_update_query_free_space(
    const char *directory, uint64_t *available)
{
    return tilefinch_update_query_free_space_with(
        directory, available, system_space_query, NULL);
}
