#ifndef TILEFINCH_VOICE_COMPONENT_H
#define TILEFINCH_VOICE_COMPONENT_H

#include <stdbool.h>
#include <stddef.h>

#include "tilefinch/install_paths.h"
#include "tilefinch/update.h"

#define TILEFINCH_VOICE_COMPONENT_ID "voice-en-us"
#define TILEFINCH_VOICE_COMPONENT_ABI 1u
#define TILEFINCH_VOICE_COMPONENT_REMOVED_MARKER "UNINSTALLED"

typedef enum {
    TILEFINCH_VOICE_COMPONENT_NONE = 0,
    TILEFINCH_VOICE_COMPONENT_SHARED,
    TILEFINCH_VOICE_COMPONENT_LEGACY
} TilefinchVoiceComponentSource;

/*
 * Resolve lazily: callers invoke this only when the user opens the Voice
 * submenu or starts recognition. Ordinary boot merely constructs the
 * deterministic shared path and performs no Memory Stick read.
 */
bool tilefinch_voice_component_path(
    const TilefinchInstallPaths *paths, char *output, size_t output_size);
TilefinchVoiceComponentSource tilefinch_voice_component_resolve(
    const TilefinchInstallPaths *paths, char *output, size_t output_size);

/* Recover the signed installed floor from active/previous metadata. Expiry is
   intentionally ignored after installation; signature, package identity and
   the READY digest still have to match. This avoids a separate mutable
   anti-rollback database and remains recoverable after interrupted promote. */
bool tilefinch_voice_component_installed_identity(
    Budget *budget, const TilefinchInstallPaths *paths,
    const TilefinchUpdateRoot *root, uint64_t *sequence,
    uint8_t package_sha256[32]);

typedef struct TilefinchVoiceComponentInstall TilefinchVoiceComponentInstall;

typedef struct {
    const char *package_path;
    const uint8_t *envelope;
    size_t envelope_length;
    const TilefinchUpdateManifest *manifest;
    const uint8_t *manifest_digest;
    const char *install_root;
} TilefinchVoiceComponentInstallOptions;

TilefinchVoiceComponentInstall *tilefinch_voice_component_install_create(
    Budget *budget, const TilefinchVoiceComponentInstallOptions *options);
void tilefinch_voice_component_install_destroy(
    TilefinchVoiceComponentInstall *job);
bool tilefinch_voice_component_install_cancel(
    TilefinchVoiceComponentInstall *job);
bool tilefinch_voice_component_install_pump(
    TilefinchVoiceComponentInstall *job, size_t maximum_bytes);
bool tilefinch_voice_component_install_snapshot(
    const TilefinchVoiceComponentInstall *job,
    TilefinchUpdateInstallSnapshot *snapshot);

/* Removes only the shared optional component. Legacy slot-local models are
   immutable slot resources and disappear naturally with older slots. */
bool tilefinch_voice_component_remove(const TilefinchInstallPaths *paths);

#endif
