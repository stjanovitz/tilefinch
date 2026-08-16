#include "tilefinch/update.h"

#include <string.h>
#include <sys/statvfs.h>

bool tilefinch_update_query_free_space(
    const char *directory, uint64_t *available)
{
    if (directory == NULL || available == NULL) return false;
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
    struct statvfs status;
    if (statvfs(query_path, &status) != 0) return false;
    uint64_t block = status.f_frsize != 0
        ? status.f_frsize : status.f_bsize;
    if (block != 0 && (uint64_t) status.f_bavail > UINT64_MAX / block)
        *available = UINT64_MAX;
    else
        *available = (uint64_t) status.f_bavail * block;
    return block != 0;
}
