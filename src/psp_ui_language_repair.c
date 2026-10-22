#include "tilefinch/psp_ui_language_repair.h"
#include "tilefinch/ui_language.h"

bool psp_ui_language_pack(unsigned language, TilefinchGlyphPack *pack)
{
    const TilefinchUiLanguageSpec *spec = tilefinch_ui_language_spec(language);
    return spec && tilefinch_glyph_pack_for_language(spec->glyphs, pack);
}

bool psp_ui_language_pack_current(
    const PspGlyphComponentSession *session, unsigned language)
{
    const TilefinchUiLanguageSpec *spec = tilefinch_ui_language_spec(language);
    TilefinchGlyphPack pack;
    return session != NULL && psp_ui_language_pack(language, &pack)
        && psp_glyph_component_session_installed_at_least(
            session, pack, spec->minimum_glyph_sequence);
}

PspUiLanguageRepair psp_ui_language_repair_needed(
    const TilefinchInstallPaths *paths,
    const PspGlyphComponentSession *glyphs, unsigned language,
    bool catalog_loaded)
{
    if (catalog_loaded || language == TILEFINCH_UI_LANGUAGE_ENGLISH
        || tilefinch_ui_language_spec(language) == NULL
        || paths == NULL || !paths->slotted)
        return PSP_UI_LANGUAGE_REPAIR_NONE;
    /* The download fetches a missing catalog and then any pack it needs,
       so a missing catalog is the whole story even when both are. */
    if (!tilefinch_ui_translation_installed(paths, language))
        return PSP_UI_LANGUAGE_REPAIR_CATALOG;
    TilefinchGlyphPack pack;
    if (psp_ui_language_pack(language, &pack)
        && !psp_ui_language_pack_current(glyphs, language))
        return PSP_UI_LANGUAGE_REPAIR_FONT_PACK;
    return PSP_UI_LANGUAGE_REPAIR_NONE;
}

bool psp_ui_language_repair_offer_once(
    bool *checked, PspUiState *ui, const TilefinchInstallPaths *paths,
    const PspGlyphComponentSession *glyphs, unsigned language,
    bool catalog_loaded, bool busy)
{
    if (checked == NULL || *checked || busy
        || !psp_ui_language_repair_offer_ready(ui)) return false;
    *checked = true;
    PspUiLanguageRepair repair = psp_ui_language_repair_needed(
        paths, glyphs, language, catalog_loaded);
    return repair != PSP_UI_LANGUAGE_REPAIR_NONE
        && psp_ui_show_language_repair_offer(
            ui, language, repair == PSP_UI_LANGUAGE_REPAIR_FONT_PACK);
}
