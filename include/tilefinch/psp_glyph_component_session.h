#ifndef TILEFINCH_PSP_GLYPH_COMPONENT_SESSION_H
#define TILEFINCH_PSP_GLYPH_COMPONENT_SESSION_H

#include "tilefinch/browser_engine.h"
#include "tilefinch/browser_profile.h"
#include "tilefinch/glyph_component.h"
#include "tilefinch/glyph_component_store.h"

typedef enum {
    PSP_GLYPH_COMPONENT_PRIMARY_NONE = 0,
    PSP_GLYPH_COMPONENT_PRIMARY_CHECK_REQUIRED
} PspGlyphComponentPrimaryResult;

typedef struct {
    Budget *budget;
    TilefinchUpdateRoot root;
    TilefinchGlyphProvider *provider;
    TilefinchUpdateClient *client;
    TilefinchGlyphComponentInstall *installer;
    TilefinchUpdateClientSnapshot client_snapshot;
    TilefinchUpdateInstallSnapshot install_snapshot;
    char package_path[TILEFINCH_INSTALL_PATH_LIMIT];
    TilefinchGlyphPack operation_pack;
    uint8_t installed_mask;
    uint8_t attached_mask;
    bool root_ready;
    bool operation_initialized;
    bool auto_install;
    bool runtime_changed;
} PspGlyphComponentSession;

/* Boot attachment is deliberately inert for the embedded-only defaults. It
   verifies signed metadata before opening a selected pack and never writes. */
bool psp_glyph_component_session_attach_selected(
    PspGlyphComponentSession *session, Budget *budget,
    const TilefinchInstallPaths *paths, BrowserGlyphLanguage language,
    bool color_emoji);
void psp_glyph_component_session_destroy(PspGlyphComponentSession *session);

/* Hot-path work: at most one already-queued pack block, and therefore one
   bounded Memory Stick read, per call. There are no writes here. */
bool psp_glyph_component_session_pump_runtime(
    PspGlyphComponentSession *session, BrowserEngine *engine);

void psp_glyph_component_session_probe(
    PspGlyphComponentSession *session, const TilefinchInstallPaths *paths);
bool psp_glyph_component_session_installed(
    const PspGlyphComponentSession *session, TilefinchGlyphPack pack);
bool psp_glyph_component_session_metadata_url(
    TilefinchGlyphPack pack, char *output, size_t capacity);
bool psp_glyph_component_session_select_operation(
    PspGlyphComponentSession *session, Budget *budget,
    const TilefinchInstallPaths *paths, TilefinchGlyphPack pack);
PspGlyphComponentPrimaryResult psp_glyph_component_session_primary(
    PspGlyphComponentSession *session,
    const TilefinchInstallPaths *paths, BrowserEngine *engine);
bool psp_glyph_component_session_begin_check(
    PspGlyphComponentSession *session, uint64_t now_unix, bool clock_valid);
bool psp_glyph_component_session_cancel(PspGlyphComponentSession *session);
bool psp_glyph_component_session_pump_operation(
    PspGlyphComponentSession *session,
    const TilefinchInstallPaths *paths, BrowserEngine *engine);
bool psp_glyph_component_session_active(
    const PspGlyphComponentSession *session);
bool psp_glyph_component_session_remove(
    PspGlyphComponentSession *session,
    const TilefinchInstallPaths *paths, TilefinchGlyphPack pack,
    BrowserEngine *engine);

#endif
