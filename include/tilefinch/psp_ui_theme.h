#ifndef TILEFINCH_PUBLIC_PSP_UI_THEME_H
#define TILEFINCH_PUBLIC_PSP_UI_THEME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/browser_profile.h"
#include "tilefinch/budget.h"

/* Color-only chrome palette. Geometry, typography and motion deliberately
   remain application-owned so a theme cannot invalidate the 480x272 layout. */
typedef struct {
    uint16_t ground;
    uint16_t ground_reader;
    uint16_t chrome_bar;
    uint16_t hint_bar;
    uint16_t panel;
    uint16_t surface;
    uint16_t surface_focus;
    uint16_t line;
    uint16_t text;
    uint16_t text_body;
    uint16_t text_muted;
    uint16_t text_faint;
    uint16_t accent;
    uint16_t accent_high;
    uint16_t on_accent;
    uint16_t ok;
    uint16_t warn;
} PspUiThemePalette;

enum {
    PSP_UI_THEME_FORMAT_VERSION = 1,
    PSP_UI_THEME_FILE_LIMIT = 2048,
    PSP_UI_THEME_LINE_LIMIT = 96,
    PSP_UI_THEME_FIELD_LIMIT = 24,
    PSP_UI_THEME_CATALOG_LIMIT = 12,
    PSP_UI_THEME_DIRECTORY_VISIT_LIMIT = 64,
    PSP_UI_THEME_LABEL_LIMIT = 32
};

typedef struct PspUiThemeCatalog PspUiThemeCatalog;

/* The UI is process-singleton state on PSP. Selection is performed only by
   the serialized presentation owner; painters read this pointer but never
   mutate a palette. */
extern const PspUiThemePalette *psp_ui_theme_active_palette;

const PspUiThemePalette *psp_ui_theme_palette(BrowserChromeTheme theme);
void psp_ui_theme_select(BrowserChromeTheme theme);
bool psp_ui_theme_load_custom_file(
    const char *path, char *error, size_t error_capacity);
bool psp_ui_theme_custom_available(void);

/* A catalog is short-lived settings state, charged to Budget only while the
   chooser is open. Scanning is bounded and validates each retained file;
   ordinary built-in-theme boot never calls this path. */
PspUiThemeCatalog *psp_ui_theme_catalog_create(
    Budget *budget, const char *directory, const char *selected_filename,
    char *error, size_t error_capacity);
void psp_ui_theme_catalog_destroy(PspUiThemeCatalog *catalog);
size_t psp_ui_theme_catalog_count(const PspUiThemeCatalog *catalog);
const char *psp_ui_theme_catalog_filename(
    const PspUiThemeCatalog *catalog, size_t index);
const char *psp_ui_theme_catalog_label(
    const PspUiThemeCatalog *catalog, size_t index);
uint16_t psp_ui_theme_catalog_accent(
    const PspUiThemeCatalog *catalog, size_t index);
size_t psp_ui_theme_catalog_selected(
    const PspUiThemeCatalog *catalog);
bool psp_ui_theme_catalog_truncated(const PspUiThemeCatalog *catalog);

/* The UI borrows this catalog only while its chooser is visible. Ownership
   remains with the app and must be unbound before destruction. */
void psp_ui_theme_catalog_bind(const PspUiThemeCatalog *catalog);
const PspUiThemeCatalog *psp_ui_theme_catalog_bound(void);
const char *psp_ui_theme_custom_label(void);

#endif
