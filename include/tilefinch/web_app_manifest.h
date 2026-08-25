#ifndef TILEFINCH_WEB_APP_MANIFEST_H
#define TILEFINCH_WEB_APP_MANIFEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TILEFINCH_WEB_APP_MANIFEST_LIMIT (64u * 1024u)
#define TILEFINCH_WEB_APP_NAME_LIMIT 128u
#define TILEFINCH_WEB_APP_URL_LIMIT 1024u

typedef enum {
    TILEFINCH_WEB_APP_DISPLAY_BROWSER = 0,
    TILEFINCH_WEB_APP_DISPLAY_MINIMAL_UI,
    TILEFINCH_WEB_APP_DISPLAY_STANDALONE,
    TILEFINCH_WEB_APP_DISPLAY_FULLSCREEN,
    TILEFINCH_WEB_APP_DISPLAY_COUNT
} TilefinchWebAppDisplayMode;

typedef struct {
    char name[TILEFINCH_WEB_APP_NAME_LIMIT];
    char short_name[TILEFINCH_WEB_APP_NAME_LIMIT];
    char start_url[TILEFINCH_WEB_APP_URL_LIMIT];
    char scope[TILEFINCH_WEB_APP_URL_LIMIT];
    char icon_url[TILEFINCH_WEB_APP_URL_LIMIT];
    char icon_type[64];
    unsigned icon_size;
    uint32_t theme_color;
    uint8_t theme_alpha;
    TilefinchWebAppDisplayMode display_mode;
    bool theme_color_valid;
} TilefinchWebAppManifest;

const char *tilefinch_web_app_display_mode_name(
    TilefinchWebAppDisplayMode mode);

/* Parses only the bounded install metadata Tilefinch consumes. Unknown JSON
   members are skipped with a fixed-depth stack; no object graph is built. */
bool tilefinch_web_app_manifest_parse(
    const char *json, size_t length, const char *manifest_url,
    const char *document_url, TilefinchWebAppManifest *manifest,
    char *error, size_t error_size);

#endif
