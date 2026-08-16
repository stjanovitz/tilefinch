#ifndef TILEFINCH_PSP_VOICE_COMPONENT_SESSION_H
#define TILEFINCH_PSP_VOICE_COMPONENT_SESSION_H

#include "tilefinch/budget.h"
#include "tilefinch/install_paths.h"
#include "tilefinch/psp_ui.h"
#include "tilefinch/update.h"
#include "tilefinch/voice_component.h"

typedef enum {
    PSP_VOICE_COMPONENT_PRIMARY_NONE = 0,
    PSP_VOICE_COMPONENT_PRIMARY_CHECK_REQUIRED
} PspVoiceComponentPrimaryResult;

typedef struct {
    Budget *budget;
    TilefinchUpdateRoot root;
    TilefinchUpdateClient *client;
    TilefinchVoiceComponentInstall *installer;
    TilefinchUpdateClientSnapshot client_snapshot;
    TilefinchUpdateInstallSnapshot install_snapshot;
    char package_path[TILEFINCH_INSTALL_PATH_LIMIT];
    char model_path[TILEFINCH_INSTALL_PATH_LIMIT];
    bool initialized;
    bool available;
    bool installed;
    bool auto_install;
    TilefinchVoiceComponentSource source;
} PspVoiceComponentSession;

void psp_voice_component_session_probe(
    PspVoiceComponentSession *session, const TilefinchInstallPaths *paths);
bool psp_voice_component_session_initialize(
    PspVoiceComponentSession *session, Budget *budget,
    const TilefinchInstallPaths *paths);
bool psp_voice_component_session_metadata_url(
    char *output, size_t capacity);
void psp_voice_component_session_destroy(PspVoiceComponentSession *session);
PspVoiceComponentPrimaryResult psp_voice_component_session_primary(
    PspVoiceComponentSession *session,
    const TilefinchInstallPaths *paths);
bool psp_voice_component_session_begin_check(
    PspVoiceComponentSession *session, uint64_t now_unix, bool clock_valid);
bool psp_voice_component_session_cancel(PspVoiceComponentSession *session);
bool psp_voice_component_session_pump(
    PspVoiceComponentSession *session,
    const TilefinchInstallPaths *paths);
bool psp_voice_component_session_active(
    const PspVoiceComponentSession *session);
bool psp_voice_component_session_remove(
    PspVoiceComponentSession *session,
    const TilefinchInstallPaths *paths);
void psp_voice_component_session_refresh_ui(
    const PspVoiceComponentSession *session, PspUiState *ui);

#endif
