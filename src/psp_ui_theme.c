#include "tilefinch/psp_ui_theme.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psp_ui_theme.h"

#define RGB(r, g, b) PSP_THEME_RGB((r), (g), (b))

_Static_assert(sizeof(PspUiThemePalette) == 17u * sizeof(uint16_t),
               "theme palette must not contain indeterminate padding");

static const PspUiThemePalette theme_midnight = {
    PSP_THEME_GROUND, PSP_THEME_GROUND_READER,
    PSP_THEME_CHROME_BAR, PSP_THEME_HINT_BAR, PSP_THEME_PANEL,
    PSP_THEME_SURFACE, PSP_THEME_SURFACE_FOCUS, PSP_THEME_LINE,
    PSP_THEME_TEXT, PSP_THEME_TEXT_BODY, PSP_THEME_TEXT_MUTED,
    PSP_THEME_TEXT_FAINT, PSP_THEME_ACCENT_MIDNIGHT,
    PSP_THEME_ACCENT_MIDNIGHT_HI, PSP_THEME_ON_ACCENT,
    PSP_THEME_OK, PSP_THEME_WARN
};

static const PspUiThemePalette theme_ocean = {
    PSP_THEME_GROUND, PSP_THEME_GROUND_READER,
    PSP_THEME_CHROME_BAR, PSP_THEME_HINT_BAR, PSP_THEME_PANEL,
    PSP_THEME_SURFACE, PSP_THEME_SURFACE_FOCUS, PSP_THEME_LINE,
    PSP_THEME_TEXT, PSP_THEME_TEXT_BODY, PSP_THEME_TEXT_MUTED,
    PSP_THEME_TEXT_FAINT, PSP_THEME_ACCENT_COBALT,
    PSP_THEME_ACCENT_COBALT_HI, PSP_THEME_ON_ACCENT,
    PSP_THEME_OK, PSP_THEME_WARN
};

static const PspUiThemePalette theme_slate = {
    PSP_THEME_GROUND, PSP_THEME_GROUND_READER,
    PSP_THEME_CHROME_BAR, PSP_THEME_HINT_BAR, PSP_THEME_PANEL,
    PSP_THEME_SURFACE, PSP_THEME_SURFACE_FOCUS, PSP_THEME_LINE,
    PSP_THEME_TEXT, PSP_THEME_TEXT_BODY, PSP_THEME_TEXT_MUTED,
    PSP_THEME_TEXT_FAINT, PSP_THEME_ACCENT_SLATE,
    PSP_THEME_ACCENT_SLATE_HI, PSP_THEME_ON_ACCENT,
    PSP_THEME_OK, PSP_THEME_WARN
};

static const PspUiThemePalette theme_ember = {
    PSP_THEME_GROUND, PSP_THEME_GROUND_READER,
    PSP_THEME_CHROME_BAR, PSP_THEME_HINT_BAR, PSP_THEME_PANEL,
    PSP_THEME_SURFACE, PSP_THEME_SURFACE_FOCUS, PSP_THEME_LINE,
    PSP_THEME_TEXT, PSP_THEME_TEXT_BODY, PSP_THEME_TEXT_MUTED,
    PSP_THEME_TEXT_FAINT, PSP_THEME_ACCENT_EMBER,
    PSP_THEME_ACCENT_EMBER_HI, PSP_THEME_ON_ACCENT,
    PSP_THEME_OK, PSP_THEME_WARN
};

/* Cool blue-grey surfaces complement the default Midnight accent while
   retaining enough separation on the PSP's low-contrast LCD. */
static const PspUiThemePalette theme_light = {
    RGB(0xF2, 0xF5, 0xFA), RGB(0xF7, 0xF3, 0xEA),
    RGB(0xE4, 0xEA, 0xF3), RGB(0xDA, 0xE2, 0xEE),
    RGB(0xED, 0xF1, 0xF7), RGB(0xFF, 0xFF, 0xFF),
    RGB(0xD7, 0xE6, 0xFA), RGB(0xB7, 0xC4, 0xD4),
    RGB(0x10, 0x21, 0x3A), RGB(0x26, 0x3B, 0x56),
    RGB(0x59, 0x6C, 0x83), RGB(0x84, 0x92, 0xA5),
    RGB(0x24, 0x58, 0x8F), RGB(0x3F, 0x78, 0xB8),
    RGB(0xFF, 0xFF, 0xFF), RGB(0x26, 0x7A, 0x55),
    RGB(0xA9, 0x4F, 0x24)
};

static PspUiThemePalette theme_custom;
static bool theme_custom_valid;
static char theme_custom_label[PSP_UI_THEME_LABEL_LIMIT] = "Custom";

typedef struct {
    char filename[BROWSER_PROFILE_THEME_FILE_LIMIT];
    char label[PSP_UI_THEME_LABEL_LIMIT];
    uint16_t accent;
} PspUiThemeCatalogEntry;

struct PspUiThemeCatalog {
    Budget *budget;
    uint8_t count;
    uint8_t selected;
    bool selected_valid;
    bool truncated;
    PspUiThemeCatalogEntry entries[PSP_UI_THEME_CATALOG_LIMIT];
};

static const PspUiThemeCatalog *theme_catalog_bound;

const PspUiThemePalette *psp_ui_theme_active_palette = &theme_midnight;

const PspUiThemePalette *psp_ui_theme_palette(BrowserChromeTheme theme)
{
    switch (theme) {
        case BROWSER_CHROME_THEME_OCEAN: return &theme_ocean;
        case BROWSER_CHROME_THEME_PLUM: return &theme_slate;
        case BROWSER_CHROME_THEME_EMBER: return &theme_ember;
        case BROWSER_CHROME_THEME_LIGHT: return &theme_light;
        case BROWSER_CHROME_THEME_CUSTOM:
            return theme_custom_valid ? &theme_custom : &theme_midnight;
        case BROWSER_CHROME_THEME_FINCH:
        default: return &theme_midnight;
    }
}

void psp_ui_theme_select(BrowserChromeTheme theme)
{
    psp_ui_theme_active_palette = psp_ui_theme_palette(theme);
}

bool psp_ui_theme_custom_available(void)
{
    return theme_custom_valid;
}

const char *psp_ui_theme_custom_label(void)
{
    return theme_custom_label;
}

static void theme_error(char *output, size_t capacity, const char *message)
{
    if (output == NULL || capacity == 0u) return;
    snprintf(output, capacity, "%s", message);
}

static bool parse_hex_digit(char value, unsigned *digit)
{
    if (value >= '0' && value <= '9') *digit = (unsigned) (value - '0');
    else if (value >= 'a' && value <= 'f')
        *digit = (unsigned) (value - 'a' + 10);
    else if (value >= 'A' && value <= 'F')
        *digit = (unsigned) (value - 'A' + 10);
    else return false;
    return true;
}

static bool parse_color(const char *value, uint16_t *color)
{
    if (value == NULL || color == NULL || strlen(value) != 7u
        || value[0] != '#') return false;
    unsigned digits[6];
    for (size_t at = 0; at < 6u; at++)
        if (!parse_hex_digit(value[at + 1u], &digits[at])) return false;
    unsigned red = digits[0] * 16u + digits[1];
    unsigned green = digits[2] * 16u + digits[3];
    unsigned blue = digits[4] * 16u + digits[5];
    *color = tilefinch_rgb565_pack_u8(red, green, blue);
    return true;
}

typedef struct {
    const char *name;
    size_t offset;
} ThemeField;

#define THEME_FIELD(member) { #member, offsetof(PspUiThemePalette, member) }
static const ThemeField theme_fields[] = {
    THEME_FIELD(ground), THEME_FIELD(ground_reader),
    THEME_FIELD(chrome_bar), THEME_FIELD(hint_bar), THEME_FIELD(panel),
    THEME_FIELD(surface), THEME_FIELD(surface_focus), THEME_FIELD(line),
    THEME_FIELD(text), THEME_FIELD(text_body), THEME_FIELD(text_muted),
    THEME_FIELD(text_faint), THEME_FIELD(accent),
    THEME_FIELD(accent_high), THEME_FIELD(on_accent), THEME_FIELD(ok),
    THEME_FIELD(warn)
};
#undef THEME_FIELD

static bool theme_label_valid(const char *label)
{
    size_t length = label == NULL ? 0u : strlen(label);
    if (length == 0u || length >= PSP_UI_THEME_LABEL_LIMIT) return false;
    for (size_t at = 0; at < length; at++) {
        unsigned char byte = (unsigned char) label[at];
        if (byte < 0x20u || byte > 0x7eu || byte == '=' || byte == '\t')
            return false;
    }
    return true;
}

static void theme_label_from_filename(
    const char *filename, char label[PSP_UI_THEME_LABEL_LIMIT]);
static bool theme_filename_valid(const char *filename);

static bool theme_parse_file(
    const char *path, PspUiThemePalette *palette,
    char label[PSP_UI_THEME_LABEL_LIMIT],
    char *error, size_t error_capacity)
{
    if (error != NULL && error_capacity != 0u) error[0] = '\0';
    if (path == NULL) {
        theme_error(error, error_capacity, "THEME PATH UNAVAILABLE");
        return false;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        theme_error(error, error_capacity, "THEME FILE NOT FOUND");
        return false;
    }
    PspUiThemePalette candidate = theme_midnight;
    uint32_t seen = 0u;
    unsigned fields = 0u;
    unsigned lines = 0u;
    size_t total = 0u;
    bool header_seen = false;
    bool format_seen = false;
    bool name_seen = false;
    bool valid = true;
    char line[PSP_UI_THEME_LINE_LIMIT];
    while (fgets(line, sizeof(line), file) != NULL) {
        if (++lines > PSP_UI_THEME_FIELD_LIMIT) {
            valid = false;
            theme_error(error, error_capacity, "TOO MANY THEME LINES");
            break;
        }
        size_t length = strlen(line);
        if (total > PSP_UI_THEME_FILE_LIMIT - length) {
            valid = false;
            theme_error(error, error_capacity, "THEME EXCEEDS 2 KIB");
            break;
        }
        total += length;
        if (length != 0u && line[length - 1u] != '\n' && !feof(file)) {
            valid = false;
            theme_error(error, error_capacity, "THEME LINE TOO LONG");
            break;
        }
        while (length != 0u
               && (line[length - 1u] == '\n' || line[length - 1u] == '\r'))
            line[--length] = '\0';
        if (!header_seen) {
            if (strcmp(line, "tilefinch-theme") != 0) {
                valid = false;
                theme_error(error, error_capacity, "INVALID THEME HEADER");
                break;
            }
            header_seen = true;
            continue;
        }
        if (line[0] == '\0' || line[0] == '#') continue;
        char *equals = strchr(line, '=');
        if (equals == NULL || equals == line) {
            valid = false;
            theme_error(error, error_capacity, "INVALID THEME FIELD");
            break;
        }
        *equals++ = '\0';
        if (strcmp(line, "format") == 0) {
            if (format_seen) {
                valid = false;
                theme_error(error, error_capacity, "DUPLICATE THEME FORMAT");
            } else if (strcmp(equals, "1") != 0) {
                valid = false;
                theme_error(error, error_capacity, "UNSUPPORTED THEME FORMAT");
            } else {
                format_seen = true;
            }
            if (!valid) break;
            continue;
        }
        if (strcmp(line, "name") == 0) {
            if (name_seen) {
                valid = false;
                theme_error(error, error_capacity, "DUPLICATE THEME NAME");
            } else if (!theme_label_valid(equals)) {
                valid = false;
                theme_error(error, error_capacity, "INVALID THEME NAME");
            } else {
                snprintf(label, PSP_UI_THEME_LABEL_LIMIT, "%s", equals);
                name_seen = true;
            }
            if (!valid) break;
            continue;
        }
        bool matched = false;
        for (size_t at = 0; at < sizeof(theme_fields) / sizeof(theme_fields[0]);
             at++) {
            if (strcmp(line, theme_fields[at].name) != 0) continue;
            if ((seen & (UINT32_C(1) << at)) != 0u) {
                valid = false;
                theme_error(error, error_capacity, "DUPLICATE THEME FIELD");
                break;
            }
            uint16_t color;
            if (!parse_color(equals, &color)) {
                valid = false;
                theme_error(error, error_capacity, "INVALID THEME COLOR");
                break;
            }
            uint16_t *slot = (uint16_t *) ((unsigned char *) &candidate
                + theme_fields[at].offset);
            *slot = color;
            seen |= UINT32_C(1) << at;
            fields++;
            matched = true;
            break;
        }
        if (!valid) break;
        if (!matched) {
            valid = false;
            theme_error(error, error_capacity, "UNKNOWN THEME FIELD");
            break;
        }
    }
    if (ferror(file)) {
        valid = false;
        theme_error(error, error_capacity, "THEME READ FAILED");
    }
    if (valid && (!header_seen || !format_seen || fields == 0u)) {
        valid = false;
        theme_error(error, error_capacity,
                    !format_seen ? "THEME FORMAT MISSING"
                                 : "THEME HAS NO COLORS");
    }
    if (valid && total == PSP_UI_THEME_FILE_LIMIT) {
        int extra = fgetc(file);
        if (extra != EOF) {
            valid = false;
            theme_error(error, error_capacity, "THEME EXCEEDS 2 KIB");
        }
    }
    fclose(file);
    if (!valid) return false;
    *palette = candidate;
    return true;
}

bool psp_ui_theme_load_custom_file(
    const char *path, char *error, size_t error_capacity)
{
    PspUiThemePalette candidate;
    char label[PSP_UI_THEME_LABEL_LIMIT] = "Custom";
    if (!theme_parse_file(path, &candidate, label, error, error_capacity))
        return false;
    if (strcmp(label, "Custom") == 0) {
        const char *filename = strrchr(path, '/');
        filename = filename == NULL ? path : filename + 1;
        if (strcmp(filename, "theme.tfth") != 0
            && theme_filename_valid(filename))
            theme_label_from_filename(filename, label);
    }
    theme_custom = candidate;
    theme_custom_valid = true;
    snprintf(theme_custom_label, sizeof(theme_custom_label), "%s", label);
    return true;
}

static bool theme_filename_valid(const char *filename)
{
    if (filename == NULL) return false;
    size_t length = strlen(filename);
    if (length <= 5u || length >= BROWSER_PROFILE_THEME_FILE_LIMIT
        || strcmp(filename + length - 5u, ".tfth") != 0
        || filename[0] == '.') return false;
    for (size_t at = 0; at < length; at++) {
        unsigned char byte = (unsigned char) filename[at];
        if (!(isalnum(byte) || byte == '-' || byte == '_' || byte == '.'))
            return false;
    }
    return true;
}

static void theme_label_from_filename(
    const char *filename, char label[PSP_UI_THEME_LABEL_LIMIT])
{
    size_t length = strlen(filename);
    if (length > 5u) length -= 5u;
    if (length >= PSP_UI_THEME_LABEL_LIMIT)
        length = PSP_UI_THEME_LABEL_LIMIT - 1u;
    bool capitalize = true;
    for (size_t at = 0; at < length; at++) {
        char byte = filename[at];
        if (byte == '-' || byte == '_') {
            label[at] = ' ';
            capitalize = true;
        } else {
            label[at] = capitalize
                ? (char) toupper((unsigned char) byte) : byte;
            capitalize = false;
        }
    }
    label[length] = '\0';
}

static int theme_entry_compare(
    const PspUiThemeCatalogEntry *left,
    const PspUiThemeCatalogEntry *right)
{
    int label_order = strcmp(left->label, right->label);
    return label_order != 0
        ? label_order : strcmp(left->filename, right->filename);
}

static void theme_catalog_sort(PspUiThemeCatalog *catalog)
{
    if (catalog == NULL) return;
    for (size_t at = 1u; at < catalog->count; at++) {
        PspUiThemeCatalogEntry entry = catalog->entries[at];
        size_t insert = at;
        while (insert != 0u
               && theme_entry_compare(
                      &catalog->entries[insert - 1u], &entry) > 0) {
            catalog->entries[insert] = catalog->entries[insert - 1u];
            insert--;
        }
        catalog->entries[insert] = entry;
    }
}

PspUiThemeCatalog *psp_ui_theme_catalog_create(
    Budget *budget, const char *directory, const char *selected_filename,
    char *error, size_t error_capacity)
{
    if (error != NULL && error_capacity != 0u) error[0] = '\0';
    if (budget == NULL || directory == NULL || directory[0] == '\0') {
        theme_error(error, error_capacity, "THEME FOLDER UNAVAILABLE");
        return NULL;
    }
    PspUiThemeCatalog *catalog = budget_calloc_category(
        budget, BUDGET_CATEGORY_SESSION, 1u, sizeof(*catalog));
    if (catalog == NULL) {
        theme_error(error, error_capacity, "THEME LIST OUT OF MEMORY");
        return NULL;
    }
    catalog->budget = budget;
    DIR *folder = opendir(directory);
    if (folder == NULL) {
        /* A missing folder is an empty catalog, not a broken settings page. */
        if (errno != ENOENT)
            theme_error(error, error_capacity, "THEME FOLDER UNREADABLE");
        return catalog;
    }
    size_t visits = 0u;
    struct dirent *entry = NULL;
    PspUiThemeCatalogEntry selected_entry;
    bool selected_entry_valid = false;
    while (visits++ < PSP_UI_THEME_DIRECTORY_VISIT_LIMIT
           && (entry = readdir(folder)) != NULL) {
        if (!theme_filename_valid(entry->d_name)) continue;
        char path[1200];
        int path_length = snprintf(
            path, sizeof(path), "%s/%s", directory, entry->d_name);
        if (path_length < 0 || (size_t) path_length >= sizeof(path)) continue;
        PspUiThemePalette palette;
        char label[PSP_UI_THEME_LABEL_LIMIT] = "";
        char ignored[1];
        if (!theme_parse_file(path, &palette, label, ignored, 0u)) continue;
        if (label[0] == '\0') theme_label_from_filename(entry->d_name, label);

        PspUiThemeCatalogEntry candidate;
        size_t filename_length = strlen(entry->d_name);
        memcpy(candidate.filename, entry->d_name, filename_length + 1u);
        snprintf(candidate.label, sizeof(candidate.label), "%s", label);
        candidate.accent = palette.accent;
        if (selected_filename != NULL
            && strcmp(candidate.filename, selected_filename) == 0) {
            selected_entry = candidate;
            selected_entry_valid = true;
        }

        size_t insert = 0u;
        while (insert < catalog->count
               && theme_entry_compare(&catalog->entries[insert], &candidate)
                      <= 0)
            insert++;
        if (catalog->count < PSP_UI_THEME_CATALOG_LIMIT) {
            memmove(&catalog->entries[insert + 1u],
                    &catalog->entries[insert],
                    (catalog->count - insert) * sizeof(catalog->entries[0]));
            catalog->entries[insert] = candidate;
            catalog->count++;
        } else if (insert < PSP_UI_THEME_CATALOG_LIMIT) {
            memmove(&catalog->entries[insert + 1u],
                    &catalog->entries[insert],
                    (PSP_UI_THEME_CATALOG_LIMIT - insert - 1u)
                        * sizeof(catalog->entries[0]));
            catalog->entries[insert] = candidate;
            catalog->truncated = true;
        } else {
            catalog->truncated = true;
        }
    }
    if (entry != NULL) catalog->truncated = true;
    closedir(folder);

    /* A selected file may live beyond the bounded directory prefix. Probe
       that one known-safe basename directly so the active choice is still
       visible without extending the scan. */
    if (!selected_entry_valid && theme_filename_valid(selected_filename)) {
        char path[1200];
        int path_length = snprintf(
            path, sizeof(path), "%s/%s", directory, selected_filename);
        PspUiThemePalette palette;
        char label[PSP_UI_THEME_LABEL_LIMIT] = "";
        char ignored[1];
        if (path_length >= 0 && (size_t) path_length < sizeof(path)
            && theme_parse_file(path, &palette, label, ignored, 0u)) {
            if (label[0] == '\0')
                theme_label_from_filename(selected_filename, label);
            size_t filename_length = strlen(selected_filename);
            memcpy(selected_entry.filename, selected_filename,
                   filename_length + 1u);
            snprintf(selected_entry.label, sizeof(selected_entry.label),
                     "%s", label);
            selected_entry.accent = palette.accent;
            selected_entry_valid = true;
        }
    }

    /* The active theme must remain reachable even when it sorts outside the
       retained prefix. This displaces one inactive entry without growing the
       catalog or weakening the directory-visit bound. */
    bool selected_retained = false;
    for (size_t at = 0; at < catalog->count; at++) {
        if (selected_filename != NULL
            && strcmp(catalog->entries[at].filename, selected_filename) == 0) {
            selected_retained = true;
            break;
        }
    }
    if (selected_entry_valid && !selected_retained) {
        if (catalog->count < PSP_UI_THEME_CATALOG_LIMIT)
            catalog->entries[catalog->count++] = selected_entry;
        else
            catalog->entries[catalog->count - 1u] = selected_entry;
        catalog->truncated = true;
        theme_catalog_sort(catalog);
    }
    for (size_t at = 0; at < catalog->count; at++) {
        if (selected_filename != NULL
            && strcmp(catalog->entries[at].filename, selected_filename) == 0) {
            catalog->selected = (uint8_t) at;
            catalog->selected_valid = true;
            break;
        }
    }
    return catalog;
}

void psp_ui_theme_catalog_destroy(PspUiThemeCatalog *catalog)
{
    if (catalog == NULL) return;
    if (theme_catalog_bound == catalog) theme_catalog_bound = NULL;
    budget_free(catalog->budget, catalog);
}

size_t psp_ui_theme_catalog_count(const PspUiThemeCatalog *catalog)
{
    return catalog == NULL ? 0u : catalog->count;
}

const char *psp_ui_theme_catalog_filename(
    const PspUiThemeCatalog *catalog, size_t index)
{
    return catalog != NULL && index < catalog->count
        ? catalog->entries[index].filename : NULL;
}

const char *psp_ui_theme_catalog_label(
    const PspUiThemeCatalog *catalog, size_t index)
{
    return catalog != NULL && index < catalog->count
        ? catalog->entries[index].label : NULL;
}

uint16_t psp_ui_theme_catalog_accent(
    const PspUiThemeCatalog *catalog, size_t index)
{
    return catalog != NULL && index < catalog->count
        ? catalog->entries[index].accent : theme_midnight.accent;
}

size_t psp_ui_theme_catalog_selected(const PspUiThemeCatalog *catalog)
{
    return catalog != NULL && catalog->selected_valid
        ? catalog->selected : SIZE_MAX;
}

bool psp_ui_theme_catalog_truncated(const PspUiThemeCatalog *catalog)
{
    return catalog != NULL && catalog->truncated;
}

void psp_ui_theme_catalog_bind(const PspUiThemeCatalog *catalog)
{
    theme_catalog_bound = catalog;
}

const PspUiThemeCatalog *psp_ui_theme_catalog_bound(void)
{
    return theme_catalog_bound;
}

#undef RGB
