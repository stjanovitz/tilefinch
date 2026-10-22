#ifndef TILEFINCH_PSP_UI_LANGUAGE_REPAIR_H
#define TILEFINCH_PSP_UI_LANGUAGE_REPAIR_H

#include "tilefinch/install_paths.h"
#include "tilefinch/psp_glyph_component_session.h"
#include "tilefinch/psp_ui.h"

/* The glyph pack an interface language's catalog needs; false for one that
   needs none (or an unknown language). */
bool psp_ui_language_pack(unsigned language, TilefinchGlyphPack *pack);
/* That pack is installed at the signed revision the catalog requires. */
bool psp_ui_language_pack_current(
    const PspGlyphComponentSession *session, unsigned language);

/* What the saved interface language is missing after boot. */
typedef enum {
    PSP_UI_LANGUAGE_REPAIR_NONE = 0,
    /* Its current-version catalog is absent, damaged or fails its pin. */
    PSP_UI_LANGUAGE_REPAIR_CATALOG,
    /* The catalog is fine; the glyph pack it needs is missing or older
       than the revision the catalog requires. */
    PSP_UI_LANGUAGE_REPAIR_FONT_PACK
} PspUiLanguageRepair;

/* Why a non-English saved interface language did not load at boot, using
   the language row's own checks: the catalog's pinned-bytes check, then
   the pack revision check over the boot attachment's verified state.
   NONE for English, a loaded catalog, an unslotted install, or a failure a
   download would not fix. Reads at most the one catalog file; call it once
   per boot, off the boot path. */
PspUiLanguageRepair psp_ui_language_repair_needed(
    const TilefinchInstallPaths *paths,
    const PspGlyphComponentSession *glyphs, unsigned language,
    bool catalog_loaded);

/* The once-per-boot offer. Until the UI is idle (psp_ui_language_repair_
   offer_ready) and the app is not `busy`, it waits, checking nothing.
   Then it checks once, sets *checked, and shows the offer if the language
   needs a download. Never again this session, answered or not. */
bool psp_ui_language_repair_offer_once(
    bool *checked, PspUiState *ui, const TilefinchInstallPaths *paths,
    const PspGlyphComponentSession *glyphs, unsigned language,
    bool catalog_loaded, bool busy);

#endif
