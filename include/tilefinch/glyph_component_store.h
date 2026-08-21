#ifndef TILEFINCH_GLYPH_COMPONENT_STORE_H
#define TILEFINCH_GLYPH_COMPONENT_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"
#include "tilefinch/install_paths.h"
#include "tilefinch/update.h"

typedef enum {
    TILEFINCH_GLYPH_PACK_JAPANESE = 0,
    TILEFINCH_GLYPH_PACK_CHINESE_SIMPLIFIED,
    TILEFINCH_GLYPH_PACK_CHINESE_TRADITIONAL,
    TILEFINCH_GLYPH_PACK_KOREAN,
    TILEFINCH_GLYPH_PACK_COLOR_EMOJI,
    TILEFINCH_GLYPH_PACK_CYRILLIC,
    TILEFINCH_GLYPH_PACK_LATIN_EXTENDED,
    TILEFINCH_GLYPH_PACK_COUNT
} TilefinchGlyphPack;

_Static_assert(TILEFINCH_GLYPH_PACK_COUNT <= 8,
               "glyph pack mask must fit in uint8_t");

typedef struct {
    const char *id;
    const char *label;
    const char *metadata_asset;
    const char *pack_asset;
} TilefinchGlyphPackSpec;

const TilefinchGlyphPackSpec *tilefinch_glyph_pack_spec(
    TilefinchGlyphPack pack);

/* Resolver checks active then the last signed previous generation. A durable
   uninstall marker suppresses both after an interrupted removal. */
bool tilefinch_glyph_component_resolve(
    const TilefinchInstallPaths *paths, TilefinchGlyphPack pack,
    char *output, size_t output_size);
bool tilefinch_glyph_component_installed_identity(
    Budget *budget, const TilefinchInstallPaths *paths,
    TilefinchGlyphPack pack, const TilefinchUpdateRoot *root,
    uint64_t *sequence, uint8_t package_sha256[32]);

typedef struct TilefinchGlyphComponentInstall
    TilefinchGlyphComponentInstall;

typedef struct {
    const char *package_path;
    const uint8_t *envelope;
    size_t envelope_length;
    const TilefinchUpdateManifest *manifest;
    const uint8_t *manifest_digest;
    const char *install_root;
    TilefinchGlyphPack pack;
} TilefinchGlyphComponentInstallOptions;

TilefinchGlyphComponentInstall *tilefinch_glyph_component_install_create(
    Budget *budget, const TilefinchGlyphComponentInstallOptions *options);
void tilefinch_glyph_component_install_destroy(
    TilefinchGlyphComponentInstall *job);
bool tilefinch_glyph_component_install_cancel(
    TilefinchGlyphComponentInstall *job);
bool tilefinch_glyph_component_install_pump(
    TilefinchGlyphComponentInstall *job, size_t maximum_bytes);
bool tilefinch_glyph_component_install_snapshot(
    const TilefinchGlyphComponentInstall *job,
    TilefinchUpdateInstallSnapshot *snapshot);
bool tilefinch_glyph_component_remove(
    const TilefinchInstallPaths *paths, TilefinchGlyphPack pack);

#endif
