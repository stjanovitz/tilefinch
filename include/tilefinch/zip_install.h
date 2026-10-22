#ifndef TILEFINCH_ZIP_INSTALL_H
#define TILEFINCH_ZIP_INSTALL_H

#include "tilefinch/budget.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ZIP_INSTALL_ENTRY_LIMIT 512u
#define ZIP_INSTALL_UNPACKED_LIMIT (256u * 1024u * 1024u)
#define ZIP_INSTALL_FILE_LIMIT (128u * 1024u * 1024u)
#define ZIP_INSTALL_NAME_LIMIT 192u
/* An installed app's GAME folder name, terminator included. */
#define ZIP_INSTALL_FOLDER_NAME_LIMIT 64u
/* The folder name as lowercase hex (two digits per byte), terminated. */
#define ZIP_INSTALL_FOLDER_HEX_LIMIT (2u * ZIP_INSTALL_FOLDER_NAME_LIMIT - 1u)
/* Installations the manager keeps reachable, and GAME directory entries it
   examines to find them. */
#define ZIP_INSTALL_MANAGED_LIMIT 64u
#define ZIP_INSTALL_GAME_SCAN_LIMIT 512u

typedef struct ZipInstall ZipInstall;
typedef struct {
    char name[ZIP_INSTALL_FOLDER_NAME_LIMIT];
    char destination[512];
    uint32_t unpacked_bytes;
    uint16_t files, skipped_files;
    bool removing;
} ZipInstallPreview;
typedef enum {
    ZIP_INSTALL_PREVIEW, ZIP_INSTALL_PENDING, ZIP_INSTALL_DONE,
    ZIP_INSTALL_FAILED, ZIP_INSTALL_CANCELLED
} ZipInstallStatus;

/* A bounded signature probe for the Details action, not a trust decision. */
bool zip_install_path_candidate(const char *path);
/* Only folders bearing Tilefinch's checked installation receipt are listed
   or accepted. The trusted frontend supplies GAME, never a page URL. With
   NAMES NULL only *COUNT is reported (CAPACITY still bounds it). */
bool zip_install_list(const char *game_directory,
    char names[][ZIP_INSTALL_FOLDER_NAME_LIMIT], size_t capacity,
    size_t *count);
/* The removal route carries a folder name as lowercase hex. Decoding
   refuses anything the encoder cannot have produced. */
void zip_install_encode_folder_name(const char *name,
    char output[ZIP_INSTALL_FOLDER_HEX_LIMIT]);
bool zip_install_decode_folder_name(const char *hex,
    char output[ZIP_INSTALL_FOLDER_NAME_LIMIT]);
ZipInstall *zip_install_prepare_removal(Budget *budget, const char *game_directory,
    const char *name, char *error, size_t error_capacity);

/* Preparation is read-only and cooperatively cancellable. Only begin creates
   a private staging directory; a complete CRC-checked app is renamed once. */
ZipInstall *zip_install_prepare(Budget *budget, const char *archive,
    const char *game_directory, const char *suggested_name, uint32_t id,
    char *error, size_t error_capacity);
const ZipInstallPreview *zip_install_preview(const ZipInstall *install);
bool zip_install_begin(ZipInstall *install);
ZipInstallStatus zip_install_pump(ZipInstall *install);
void zip_install_cancel(ZipInstall *install);
const char *zip_install_error(const ZipInstall *install);
void zip_install_destroy(ZipInstall *install);

#endif
