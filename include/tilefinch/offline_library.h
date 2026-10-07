#ifndef TILEFINCH_OFFLINE_LIBRARY_H
#define TILEFINCH_OFFLINE_LIBRARY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"
#include "tilefinch/document.h"
#include "tilefinch/session.h"
#include "tilefinch/web_app_manifest.h"
#include "tilefinch/youtube_resolver.h"

#define OFFLINE_LIBRARY_ITEM_LIMIT 12u
#define OFFLINE_LIBRARY_DIRECTORY_LIMIT 768u
#define OFFLINE_LIBRARY_TITLE_LIMIT 128u
#define OFFLINE_LIBRARY_URL_LIMIT 1024u
#define OFFLINE_LIBRARY_FAILURE_LIMIT 128u
#define OFFLINE_LIBRARY_INDEX_LIMIT (32u * 1024u)
#define OFFLINE_LIBRARY_ARTICLE_LIMIT (1024u * 1024u)
#define OFFLINE_LIBRARY_APP_DOCUMENT_LIMIT (1024u * 1024u)
/* An installed game may carry about 1 MiB of classic script plus its
   markup-adjacent CSS, images and data (512 KiB), with up to 1 MiB of
   optional source-bound bytecode. The pack is streamed to and from the
   Memory Stick, so these bound disk and session-cache working set, not a
   second in-memory copy. */
#define OFFLINE_LIBRARY_APP_RESOURCE_LIMIT (1536u * 1024u)
#define OFFLINE_LIBRARY_APP_BYTECODE_LIMIT (1024u * 1024u)
#define OFFLINE_LIBRARY_APP_PACK_LIMIT \
    (OFFLINE_LIBRARY_APP_RESOURCE_LIMIT \
     + OFFLINE_LIBRARY_APP_BYTECODE_LIMIT + 160u * 1024u)
#define OFFLINE_LIBRARY_APP_ICON_EDGE 16u
#define OFFLINE_LIBRARY_APP_ICON_LIMIT \
    (OFFLINE_LIBRARY_APP_ICON_EDGE * OFFLINE_LIBRARY_APP_ICON_EDGE * 4u)

typedef enum {
    OFFLINE_ITEM_ARTICLE = 1,
    OFFLINE_ITEM_YOUTUBE = 2,
    OFFLINE_ITEM_WEB_APP = 3
} OfflineItemType;

typedef enum {
    OFFLINE_ITEM_QUEUED = 1,
    OFFLINE_ITEM_DOWNLOADING,
    OFFLINE_ITEM_PAUSED,
    OFFLINE_ITEM_READY,
    OFFLINE_ITEM_FAILED
} OfflineItemState;

typedef enum {
    OFFLINE_WEB_APP_INSTALL = 0,
    OFFLINE_WEB_APP_UPDATE,
    OFFLINE_WEB_APP_REINSTALL
} OfflineWebAppOperation;

typedef struct {
    uint64_t estimated_bytes;
    uint32_t document_hash;
    uint32_t resource_hash;
    uint32_t icon_hash;
    uint32_t resource_count;
    OfflineWebAppOperation operation;
} OfflineWebAppPreview;

typedef struct {
    uint32_t id;
    OfflineItemType type;
    OfflineItemState state;
    char title[OFFLINE_LIBRARY_TITLE_LIMIT];
    char source_url[OFFLINE_LIBRARY_URL_LIMIT];
    char video_id[YOUTUBE_VIDEO_ID_CAPACITY];
    char failure_reason[OFFLINE_LIBRARY_FAILURE_LIMIT];
    uint64_t content_bytes;
    uint64_t audio_bytes;
    uint64_t downloaded_bytes;
    uint64_t duration_ms;
    uint64_t saved_at_unix;
    uint32_t article_hash;
    uint32_t auxiliary_hash;
    uint32_t icon_hash;
    uint32_t resource_count;
    uint32_t icon_bytes;
    int width;
    int height;
    int itag;
    int audio_itag;
    bool split_streams;
    uint32_t app_theme_color;
    uint8_t app_theme_alpha;
    uint8_t app_display_mode;
    bool app_theme_color_valid;
} OfflineLibraryItem;

/* Called before each classic script an install, preview or recompile
   compiles: `done` of `total` (1-based). Work then reaches a
   tilefinch_platform_cooperate() checkpoint, whose refusal stops it with
   nothing published. */
typedef void (*OfflineLibraryProgress)(void *context, unsigned done,
                                       unsigned total);
typedef struct OfflineAppStagedBytecode OfflineAppStagedBytecode;

typedef struct {
    Budget *budget;
    char directory[OFFLINE_LIBRARY_DIRECTORY_LIMIT];
    OfflineLibraryItem items[OFFLINE_LIBRARY_ITEM_LIMIT];
    size_t count;
    uint32_t next_id;
    bool loaded;
    OfflineLibraryProgress progress;
    void *progress_context;
    /* Bytecode a preview compiled, kept for the confirming install. */
    OfflineAppStagedBytecode *staged;
    /* Classic scripts compiled by install/preview/recompile (telemetry). */
    size_t script_compiles;
} OfflineLibrary;

void offline_library_init(
    OfflineLibrary *library, Budget *budget, const char *directory);
bool offline_library_load(OfflineLibrary *library);
bool offline_library_save(const OfflineLibrary *library);
const OfflineLibraryItem *offline_library_find(
    const OfflineLibrary *library, uint32_t id);
OfflineLibraryItem *offline_library_find_mutable(
    OfflineLibrary *library, uint32_t id);

bool offline_library_save_article(
    OfflineLibrary *library, PocDocument *document, const char *source_url,
    uint32_t *saved_id, char *error, size_t error_size);
bool offline_library_read_article(
    const OfflineLibrary *library, Budget *budget, uint32_t id,
    char **html, size_t *length, char *error, size_t error_size);
bool offline_library_save_web_app(
    OfflineLibrary *library, PocDocument *document, BrowserSession *session,
    const char *source_url, const TilefinchWebAppManifest *manifest,
    const unsigned char *icon, size_t icon_length,
    uint32_t *saved_id, char *error, size_t error_size);
bool offline_library_preview_web_app(
    OfflineLibrary *library, PocDocument *document, BrowserSession *session,
    const char *source_url, const TilefinchWebAppManifest *manifest,
    const unsigned char *icon, size_t icon_length,
    OfflineWebAppPreview *preview, char *error, size_t error_size);
/* Release bytecode a preview kept for its install (install does this
   itself; call it when a preview is abandoned). */
void offline_library_discard_staged(OfflineLibrary *library);
bool offline_library_read_web_app(
    const OfflineLibrary *library, Budget *budget, BrowserSession *session,
    uint32_t id, char **html, size_t *length,
    char *error, size_t error_size);
/* True when an installed app's stored bytecode came from another compiler
   ABI (an engine update), so its next launch would compile every script.
   `scripts` receives how many scripts carry bytecode. Reads the fingerprint
   kept in the index; an entry saved by an older build is probed once from
   its pack's record headers (no bodies are read). */
bool offline_library_app_needs_recompile(
    OfflineLibrary *library, uint32_t id, unsigned *scripts);
/* Recompile an installed app's classic scripts from its stored source and
   publish a new generation (new id) with fresh bytecode, using the same
   temporary-file, index-first transaction as installation. The old pack
   must authenticate completely; any failure leaves the old generation and
   index untouched. Reports progress and stops at a refused cooperate
   checkpoint like installation. */
bool offline_library_recompile_web_app(
    OfflineLibrary *library, uint32_t id, uint32_t *new_id,
    char *error, size_t error_size);
bool offline_library_read_web_app_icon(
    const OfflineLibrary *library, uint32_t id,
    unsigned char output[OFFLINE_LIBRARY_APP_ICON_LIMIT]);

bool offline_library_enqueue_youtube(
    OfflineLibrary *library, const char *watch_url, const char *title,
    uint32_t *queued_id, char *error, size_t error_size);
bool offline_library_apply_youtube_stream(
    OfflineLibrary *library, uint32_t id, const YoutubeStream *stream);
bool offline_library_remove(OfflineLibrary *library, uint32_t id);

bool offline_library_build_page(
    const OfflineLibrary *library, Budget *budget,
    char **html, size_t *length);
bool offline_library_item_path(
    const OfflineLibrary *library, uint32_t id, const char *suffix,
    char *output, size_t output_size);

#endif
