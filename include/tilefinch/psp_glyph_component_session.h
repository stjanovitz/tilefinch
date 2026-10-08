#ifndef TILEFINCH_PSP_GLYPH_COMPONENT_SESSION_H
#define TILEFINCH_PSP_GLYPH_COMPONENT_SESSION_H

#include "tilefinch/browser_engine.h"
#include "tilefinch/browser_profile.h"
#include "tilefinch/glyph_component.h"
#include "tilefinch/glyph_component_store.h"
#include "tilefinch/glyph_pack_offer.h"

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
    uint16_t installed_mask;
    uint64_t installed_sequences[TILEFINCH_GLYPH_PACK_COUNT];
    uint16_t attached_mask;
    uint16_t lazy_attempted_mask;
    uint16_t lazy_processed_script_mask;
    uint8_t lazy_attached_count;
    bool root_ready;
    bool operation_initialized;
    bool auto_install;
    bool runtime_changed;
    /* The in-page language-pack offer: per-session site memory, and an
       install it started, whose outcome is reported on the page and whose
       pack is attached to it without a reload. */
    TilefinchGlyphOfferSession offer;
    TilefinchGlyphPack offer_install_pack;
    bool offer_install_pending;
    /* The offer's confirmation is waiting for the signed size. */
    bool offer_size_pending;
    /* An install completed and detached the runtime packs; see
       psp_glyph_component_session_reattach(). */
    bool reattach_pending;
} PspGlyphComponentSession;

typedef enum {
    PSP_GLYPH_COMPONENT_SIZE_NONE = 0,
    PSP_GLYPH_COMPONENT_SIZE_CHECKING,
    PSP_GLYPH_COMPONENT_SIZE_READY,
    PSP_GLYPH_COMPONENT_SIZE_FAILED
} PspGlyphComponentSizeState;

#ifdef TILEFINCH_GLYPH_SESSION_TEST_SEAM
/* Host tests only: replaces the signed-identity check of an installed
   generation. Never compiled into the browser. */
extern bool (*psp_glyph_component_session_test_identity)(
    const TilefinchInstallPaths *paths, TilefinchGlyphPack pack);
#endif

/* Boot attachment is deliberately inert for the embedded-only defaults. It
   verifies signed metadata before opening a selected pack and never writes. */
bool psp_glyph_component_session_attach_selected(
    PspGlyphComponentSession *session, Budget *budget,
    const TilefinchInstallPaths *paths, BrowserGlyphLanguage language,
    bool color_emoji);
void psp_glyph_component_session_destroy(PspGlyphComponentSession *session);
/* Detaches every runtime pack, as an install does before it promotes a new
   generation (the provider must not read a directory being rotated). */
void psp_glyph_component_session_detach_runtime(
    PspGlyphComponentSession *session, BrowserEngine *engine);

/* Hot-path work: at most one already-queued pack block, and therefore one
   bounded Memory Stick read, per call. There are no writes here. */
bool psp_glyph_component_session_pump_runtime(
    PspGlyphComponentSession *session, BrowserEngine *engine);
/* At most one signature/index probe and one attachment per call. The selected
   language and emoji keep provider priority; visible page-script hints may
   add no more than two other installed language packs. */
bool psp_glyph_component_session_attach_hinted(
    PspGlyphComponentSession *session,
    const TilefinchInstallPaths *paths, uint16_t script_mask,
    BrowserEngine *engine);

void psp_glyph_component_session_probe(
    PspGlyphComponentSession *session, const TilefinchInstallPaths *paths);
bool psp_glyph_component_session_installed(
    const PspGlyphComponentSession *session, TilefinchGlyphPack pack);
bool psp_glyph_component_session_installed_at_least(
    const PspGlyphComponentSession *session, TilefinchGlyphPack pack,
    uint64_t minimum_sequence);
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
/* After a completed install (menu or in-page offer): put back the selected
   language and emoji packs the install detached, then attach installed
   packs for script_mask (the page's scripts) through the lazy path, so the
   page can redraw without a restart. False when no install completed since
   the last call. */
bool psp_glyph_component_session_reattach(
    PspGlyphComponentSession *session, Budget *budget,
    const TilefinchInstallPaths *paths, BrowserGlyphLanguage language,
    bool color_emoji, uint16_t script_mask, BrowserEngine *engine);
/* The offer's confirmation: select the pack's operation without the menu's
   auto-install, so the client's signed-metadata check stops at AVAILABLE
   with the verified package size. The caller begins the check. */
bool psp_glyph_component_session_prepare_size_check(
    PspGlyphComponentSession *session, Budget *budget,
    const TilefinchInstallPaths *paths, TilefinchGlyphPack pack);
PspGlyphComponentSizeState psp_glyph_component_session_size(
    const PspGlyphComponentSession *session, TilefinchGlyphPack pack,
    uint64_t *bytes, const char **message);
bool psp_glyph_component_session_remove(
    PspGlyphComponentSession *session,
    const TilefinchInstallPaths *paths, TilefinchGlyphPack pack,
    BrowserEngine *engine);

#endif
