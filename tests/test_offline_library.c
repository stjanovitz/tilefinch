#include "tilefinch/offline_library.h"
#include "tilefinch/media_file.h"
#include "tilefinch/psp_offline_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef TILEFINCH_TEST_SOURCE_DIR
#define TILEFINCH_TEST_SOURCE_DIR "."
#endif

#define CHECK(value) do { \
    if (!(value)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #value); \
        return 1; \
    } \
} while (0)

static uint32_t test_offline_hash(const unsigned char *data, size_t length)
{
    uint32_t hash = UINT32_C(2166136261);
    for (size_t index = 0; index < length; index++) {
        hash ^= data[index];
        hash *= UINT32_C(16777619);
    }
    return hash;
}

int main(void)
{
    char directory[160];
    snprintf(directory, sizeof(directory), "/tmp/tilefinch-offline-%ld",
             (long) getpid());
    CHECK(mkdir(directory, 0700) == 0);
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    budget_install_lexbor(&budget);
    PocDocument document = {0};
    static const char page[] =
        "<!doctype html><title>A &amp; B</title>"
        "<link rel=manifest href=/app.webmanifest>"
        "<nav>Do not save me</nav>"
        "<main><h1>Heading</h1><aside>Also do not save me</aside>"
        "<p>One &amp; two.</p><script>hidden()</script><p>Three.</p></main>";
    CHECK(document_parse(&document, &budget, page, sizeof(page) - 1u, 4096));
    size_t manifest_href_length = 0;
    const char *manifest_href = document_web_app_manifest_href(
        &document, &manifest_href_length);
    CHECK(manifest_href != NULL && manifest_href_length == 16u
          && memcmp(manifest_href, "/app.webmanifest", 16u) == 0);

    OfflineLibrary library;
    offline_library_init(&library, &budget, directory);
    CHECK(!library.loaded && offline_library_load(&library)
          && library.loaded && library.count == 0);
    uint32_t article_id = 0;
    char error[256] = {0};
    CHECK(offline_library_save_article(
              &library, &document, "https://example.test/article",
              &article_id, error, sizeof(error))
          && article_id != 0 && library.count == 1);
    char *html = NULL;
    size_t html_length = 0;
    CHECK(offline_library_read_article(
              &library, &budget, article_id, &html, &html_length,
              error, sizeof(error))
          && strstr(html, "A &amp; B") != NULL
          && strstr(html, "<h2>Heading</h2>") != NULL
          && strstr(html, "<p>One &amp; two.</p>") != NULL
          && strstr(html, "<pre>Heading") == NULL
          && strstr(html, "One &amp; two") != NULL
          && strstr(html, "hidden()") == NULL
          && strstr(html, "Do not save me") == NULL
          && strstr(html, "Also do not save me") == NULL);
    CHECK(offline_library_find(&library, article_id)->saved_at_unix != 0);
    budget_free(&budget, html);

    uint32_t video_id = 0;
    CHECK(offline_library_enqueue_youtube(
              &library, "https://www.youtube.com/watch?v=TFTEST00001",
              "Fixture video", &video_id, error, sizeof(error))
          && library.count == 2 && video_id != article_id);
    YoutubeStream stream = {
        .content_length = 17,
        .audio_content_length = 17,
        .duration_ms = 191000,
        .width = 640,
        .height = 360,
        .itag = 134,
        .audio_itag = 140,
        .split_streams = true
    };
    snprintf(stream.title, sizeof(stream.title), "Fixture video");
    CHECK(offline_library_apply_youtube_stream(&library, video_id, &stream));

    BrowserSession session;
    CHECK(browser_session_init(&session, &budget, 64u * 1024u));
    OfflineDownloadManager download;
    offline_download_manager_init(&download, &budget, &session, &library);
    CHECK(offline_download_manager_start(&download, video_id));
    snprintf(stream.media_url, sizeof(stream.media_url),
             "https://r1---sn-test.googlevideo.com/videoplayback?clen=17");
    snprintf(stream.audio_url, sizeof(stream.audio_url),
             "https://r1---sn-test.googlevideo.com/videoplayback?clen=17&audio=1");
    char trace_error[256] = {0};
    CHECK(fetch_trace_replay_begin(
              TILEFINCH_TEST_SOURCE_DIR "/fixtures/offline-download",
              trace_error, sizeof(trace_error))
          && offline_download_manager_adopt_resolved(&download, &stream));
    OfflineDownloadSnapshot snapshot = {0};
    CHECK(offline_download_manager_snapshot(
              &download, video_id, &snapshot)
          && snapshot.active && snapshot.total_bytes == 34
          && snapshot.available_bytes != 0);
    for (unsigned pump = 0; pump < 16
         && offline_download_manager_active(&download, NULL); pump++)
        (void) offline_download_manager_pump(&download);
    CHECK(!offline_download_manager_active(&download, NULL)
          && offline_library_find(&library, video_id)->state
                 == OFFLINE_ITEM_READY
          && offline_library_find(&library, video_id)->downloaded_bytes == 34);
    fetch_trace_end();
    offline_download_manager_destroy(&download);
    browser_session_destroy(&session);
    OfflineLibraryItem *failed_item = offline_library_find_mutable(
        &library, video_id);
    CHECK(failed_item != NULL);
    failed_item->state = OFFLINE_ITEM_PAUSED;
    snprintf(failed_item->failure_reason,
             sizeof(failed_item->failure_reason),
             "network interrupted after a resumable chunk");
    CHECK(offline_library_save(&library));

    char orphan_path[200];
    snprintf(orphan_path, sizeof(orphan_path),
             "%s/deadbeef.video.part", directory);
    FILE *orphan = fopen(orphan_path, "wb");
    CHECK(orphan != NULL && fwrite("orphan", 1, 6, orphan) == 6
          && fclose(orphan) == 0);
    OfflineLibrary loaded;
    offline_library_init(&loaded, &budget, directory);
    CHECK(offline_library_load(&loaded) && loaded.count == 2
          && offline_library_find(&loaded, article_id) != NULL
          && offline_library_find(&loaded, video_id)->content_bytes == 17
          && strstr(
                 offline_library_find(&loaded, video_id)->failure_reason,
                 "resumable chunk") != NULL
          && access(orphan_path, F_OK) != 0);
    OfflineLibraryItem *loaded_video = offline_library_find_mutable(
        &loaded, video_id);
    CHECK(loaded_video != NULL);
    loaded_video->state = OFFLINE_ITEM_READY;
    loaded_video->failure_reason[0] = '\0';

    char index_path[200], index_temporary[200], index_backup[200];
    snprintf(index_path, sizeof(index_path), "%s/library.bin", directory);
    snprintf(index_temporary, sizeof(index_temporary),
             "%s/library.bin.tmp", directory);
    snprintf(index_backup, sizeof(index_backup),
             "%s/library.bin.bak", directory);
    (void) unlink(index_temporary);
    CHECK(rename(index_path, index_temporary) == 0);
    OfflineLibrary recovered_temporary;
    offline_library_init(&recovered_temporary, &budget, directory);
    CHECK(offline_library_load(&recovered_temporary)
          && recovered_temporary.count == 2
          && offline_library_find(&recovered_temporary, article_id) != NULL);
    (void) unlink(index_backup);
    CHECK(rename(index_path, index_backup) == 0);
    OfflineLibrary recovered_backup;
    offline_library_init(&recovered_backup, &budget, directory);
    CHECK(offline_library_load(&recovered_backup)
          && recovered_backup.count == 2
          && offline_library_find(&recovered_backup, video_id) != NULL);
    FILE *corrupt_index = fopen(index_path, "wb");
    CHECK(corrupt_index != NULL
          && fwrite("broken", 1, 6, corrupt_index) == 6
          && fclose(corrupt_index) == 0);
    OfflineLibrary recovered_corrupt_primary;
    offline_library_init(&recovered_corrupt_primary, &budget, directory);
    CHECK(offline_library_load(&recovered_corrupt_primary)
          && recovered_corrupt_primary.count == 2
          && offline_library_find(
                 &recovered_corrupt_primary, article_id) != NULL);
    char *listing = NULL;
    size_t listing_length = 0;
    CHECK(offline_library_build_page(
              &loaded, &budget, &listing, &listing_length)
          && strstr(listing, "Saved offline") != NULL
          && strstr(listing, "Fixture video") != NULL
          && strstr(listing, "/offline/article?id=") != NULL
          && strstr(listing, "/offline/video?id=") != NULL);
    budget_free(&budget, listing);

    char video_path[200], audio_path[200];
    CHECK(offline_library_item_path(
              &loaded, video_id, ".video.mp4",
              video_path, sizeof(video_path))
          && offline_library_item_path(
              &loaded, video_id, ".audio.mp4",
              audio_path, sizeof(audio_path)));
    PspOfflineStore store = {.library = loaded};
    PspMediaOfflineSource offline_source = {0};
    char offline_url[128];
    snprintf(offline_url, sizeof(offline_url),
             "https://tilefinch.local/offline/video?id=%u",
             (unsigned) video_id);
    CHECK(psp_offline_store_resolve_media(
              &store, offline_url, &offline_source)
          && offline_source.stream.content_length == 17
          && offline_source.stream.audio_content_length == 17
          && strcmp(offline_source.video_path, video_path) == 0);
    char file_error[160] = {0};
    MediaFileRange *range = media_file_range_open(
        &budget, video_path, 17, file_error, sizeof(file_error));
    CHECK(range != NULL);
    MediaRangeReader reader = media_file_range_reader(range);
    unsigned char sample[5] = {0};
    CHECK(reader.read(reader.opaque, 11, sample, sizeof(sample))
          && memcmp(sample, "12345", 5) == 0
          && !reader.read(reader.opaque, 15, sample, sizeof(sample)));
    media_file_range_close(range);

    char article_path[200], article_backup[200];
    CHECK(offline_library_item_path(
              &loaded, article_id, ".article.html",
              article_path, sizeof(article_path))
          && offline_library_item_path(
              &loaded, article_id, ".article.bak",
              article_backup, sizeof(article_backup)));
    (void) unlink(article_backup);
    CHECK(rename(article_path, article_backup) == 0);
    html = NULL;
    html_length = 0;
    CHECK(offline_library_read_article(
              &loaded, &budget, article_id, &html, &html_length,
              error, sizeof(error))
          && strstr(html, "Heading") != NULL);
    budget_free(&budget, html);

    CHECK(offline_library_remove(&loaded, article_id)
          && loaded.count == 1
          && offline_library_find(&loaded, article_id) == NULL);

    static const char manifest_json[] =
        "{\"name\":\"Tiny Game\",\"short_name\":\"Tiny\","
        "\"start_url\":\"./play\",\"scope\":\"/\","
        "\"theme_color\":\"#123456\",\"display\":\"standalone\","
        "\"icons\":[{\"src\":\"icon.png\",\"sizes\":\"64x64\","
        "\"type\":\"image/png\"}]}";
    TilefinchWebAppManifest manifest = {0};
    CHECK(tilefinch_web_app_manifest_parse(
              manifest_json, sizeof(manifest_json) - 1u,
              "https://example.test/app.webmanifest",
              "https://example.test/game", &manifest,
              error, sizeof(error))
          && strcmp(manifest.short_name, "Tiny") == 0
          && strcmp(manifest.start_url, "https://example.test/play") == 0
          && manifest.theme_color_valid
          && manifest.theme_color == UINT32_C(0x123456)
          && manifest.theme_alpha == 255u
          && manifest.display_mode == TILEFINCH_WEB_APP_DISPLAY_STANDALONE
          && strcmp(tilefinch_web_app_display_mode_name(
                        manifest.display_mode), "Standalone") == 0
          && strcmp(manifest.icon_url,
                    "https://example.test/icon.png") == 0);
    CHECK(!tilefinch_web_app_manifest_parse(
              manifest_json, sizeof(manifest_json) - 1u,
              "https://other.test/app.webmanifest",
              "https://example.test/game", &manifest,
              error, sizeof(error)));
    TilefinchWebAppManifest empty_manifest = {0};
    CHECK(tilefinch_web_app_manifest_parse(
              "{}", 2u, "https://example.test/app.webmanifest",
              "https://example.test/game", &empty_manifest,
              error, sizeof(error))
          && empty_manifest.name[0] == '\0'
          && empty_manifest.start_url[0] == '\0'
          && empty_manifest.display_mode
                 == TILEFINCH_WEB_APP_DISPLAY_BROWSER);
    BrowserSession app_session;
    CHECK(browser_session_init(&app_session, &budget, 2u * 1024u * 1024u));
    static const unsigned char css[] = "canvas{width:100%}";
    CHECK(browser_session_cache_put_http(
        &app_session, "https://example.test/game.css", css,
        sizeof(css) - 1u, "", "", "text/css",
        "public,max-age=3600", "", 1u));
    static const unsigned char game_script[] =
        "globalThis.offlineGameReady=true";
    static const unsigned char compiled_game_script[] = {
        0x54, 0x46, 0x51, 0x4a, 0x53, 0x01
    };
    CHECK(browser_session_cache_put_http(
              &app_session, "https://example.test/game.js", game_script,
              sizeof(game_script) - 1u, "", "", "text/javascript",
              "public,max-age=3600", "", 1u)
          && browser_session_classic_script_bytecode_put(
              &app_session, "https://example.test/game.js", game_script,
              sizeof(game_script) - 1u, compiled_game_script,
              sizeof(compiled_game_script)));
    static const unsigned char install_compiled_script[] =
        "globalThis.installCompiled=true";
    unsigned char *install_script_data = budget_malloc(
        &budget, sizeof(install_compiled_script) - 1u);
    CHECK(install_script_data != NULL);
    memcpy(install_script_data, install_compiled_script,
           sizeof(install_compiled_script) - 1u);
    BrowserSharedBody *install_script_body = browser_shared_body_take(
        &budget, install_script_data, sizeof(install_compiled_script) - 1u);
    TilefinchRequestContext install_script_context = {
        .target_url = "https://example.test/install-compiled.js",
        .initiator_url = "https://example.test/game",
        .top_level_url = "https://example.test/game",
        .method = "GET", .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_SCRIPT
    };
    TilefinchResourceGrant install_script_grant = {
        .destination = TILEFINCH_DESTINATION_SCRIPT,
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .corp = TILEFINCH_CORP_UNSPECIFIED,
        .final_same_origin = true, .final_same_site = true,
        .mime_validated = true
    };
    CHECK(install_script_body != NULL
          && browser_session_cache_put_http_shared_classic_script(
              &app_session, install_script_context.target_url,
              install_script_body, "", "", "text/javascript",
              "public,max-age=3600", "", 1u, &install_script_context,
              &install_script_grant));
    browser_shared_body_release(install_script_body);
    static const unsigned char icon[OFFLINE_LIBRARY_APP_ICON_LIMIT] = {
        0x20, 0x40, 0x80, 0xff
    };
    OfflineWebAppPreview app_preview = {0};
    CHECK(offline_library_preview_web_app(
              &loaded, &document, &app_session,
              "https://example.test/game", &manifest,
              icon, sizeof(icon), &app_preview, error, sizeof(error))
          && app_preview.operation == OFFLINE_WEB_APP_INSTALL
          && app_preview.resource_count == 3u
          && app_preview.estimated_bytes > sizeof(css));
    uint32_t app_id = 0;
    CHECK(offline_library_save_web_app(
              &loaded, &document, &app_session,
              "https://example.test/game", &manifest,
              icon, sizeof(icon), &app_id, error, sizeof(error))
          && app_id != 0
          && offline_library_find(&loaded, app_id)->type
                 == OFFLINE_ITEM_WEB_APP
          && offline_library_find(&loaded, app_id)->app_theme_color_valid
          && offline_library_find(&loaded, app_id)->app_theme_color
                 == UINT32_C(0x123456)
          && offline_library_find(&loaded, app_id)->app_display_mode
                 == TILEFINCH_WEB_APP_DISPLAY_STANDALONE);
    CHECK(offline_library_preview_web_app(
              &loaded, &document, &app_session,
              "https://example.test/game", &manifest,
              icon, sizeof(icon), &app_preview, error, sizeof(error))
          && app_preview.operation == OFFLINE_WEB_APP_REINSTALL);
    TilefinchWebAppManifest changed_manifest = manifest;
    changed_manifest.theme_color = UINT32_C(0x654321);
    CHECK(offline_library_preview_web_app(
              &loaded, &document, &app_session,
              "https://example.test/game", &changed_manifest,
              icon, sizeof(icon), &app_preview, error, sizeof(error))
          && app_preview.operation == OFFLINE_WEB_APP_UPDATE);
    OfflineLibrary reloaded;
    offline_library_init(&reloaded, &budget, directory);
    CHECK(offline_library_load(&reloaded));
    const OfflineLibraryItem *reloaded_app = offline_library_find(
        &reloaded, app_id);
    CHECK(reloaded_app != NULL
          && reloaded_app->app_theme_color == UINT32_C(0x123456)
          && reloaded_app->app_theme_alpha == 255u
          && reloaded_app->app_display_mode
                 == TILEFINCH_WEB_APP_DISPLAY_STANDALONE);
    unsigned char restored_icon[OFFLINE_LIBRARY_APP_ICON_LIMIT];
    CHECK(offline_library_read_web_app_icon(
              &loaded, app_id, restored_icon)
          && memcmp(restored_icon, icon, sizeof(icon)) == 0);
    browser_session_cache_clear(&app_session);
    /* Installed apps larger than the ordinary live-cache preference remain
       launchable. Integrity checks are unchanged; only bounded admission is
       raised to the package's complete working set. */
    const OfflineLibraryItem *installed_app = offline_library_find(
        &loaded, app_id);
    CHECK(installed_app != NULL && installed_app->audio_bytes > 1u
          && browser_session_cache_set_maximum_bytes(
                 &app_session, (size_t) installed_app->audio_bytes - 1u));
    html = NULL;
    html_length = 0;
    CHECK(offline_library_read_web_app(
              &loaded, &budget, &app_session, app_id,
              &html, &html_length, error, sizeof(error))
          && strstr(html, "Heading") != NULL);
    /* A first-paint stylesheet artifact must not evict a deferred main game
       script from an otherwise package-sized cache. */
    TilefinchRequestContext css_context = {
        .target_url = "https://example.test/game.css",
        .initiator_url = "https://example.test/game",
        .top_level_url = "https://example.test/game",
        .method = "GET", .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_STYLE
    };
    size_t startup_artifact_length = 64u * 1024u;
    unsigned char *startup_artifact = budget_malloc(
        &budget, startup_artifact_length);
    CHECK(startup_artifact != NULL);
    memset(startup_artifact, 0x5a, startup_artifact_length);
    CHECK(browser_session_stylesheet_fragment_put_take(
        &app_session, css_context.target_url, &css_context,
        css, sizeof(css) - 1u, startup_artifact,
        startup_artifact_length));
    const BrowserCacheEntry *restored = NULL;
    CHECK(browser_session_cache_match_http(
              &app_session, "https://example.test/game.css",
              UINT64_C(2), &restored) == BROWSER_CACHE_FRESH
          && restored != NULL && restored->length == sizeof(css) - 1u);
    BrowserSharedBody *restored_bytecode =
        browser_session_classic_script_bytecode_acquire(
            &app_session, "https://example.test/game.js", game_script,
            sizeof(game_script) - 1u);
    CHECK(restored_bytecode != NULL
          && restored_bytecode->length == sizeof(compiled_game_script)
          && memcmp(restored_bytecode->data, compiled_game_script,
                    sizeof(compiled_game_script)) == 0
          && browser_session_classic_script_bytecode_acquire(
                 &app_session, "https://example.test/game.js",
                 (const unsigned char *) "different", 9u) == NULL);
    browser_shared_body_release(restored_bytecode);
    BrowserSharedBody *install_bytecode =
        browser_session_classic_script_bytecode_acquire(
            &app_session, install_script_context.target_url,
            install_compiled_script, sizeof(install_compiled_script) - 1u);
    CHECK(install_bytecode != NULL && install_bytecode->length != 0);
    browser_shared_body_release(install_bytecode);
    budget_free(&budget, html);

    /* A compiler-ABI change discards only the accelerator. The source body
       must still restore so an updated Tilefinch can recompile normally. */
    OfflineLibraryItem *mutable_app = offline_library_find_mutable(
        &loaded, app_id);
    char app_pack_path[OFFLINE_LIBRARY_DIRECTORY_LIMIT + 40u];
    CHECK(mutable_app != NULL && mutable_app->audio_bytes >= 32u
          && offline_library_item_path(
              &loaded, app_id, ".app.pack", app_pack_path,
              sizeof(app_pack_path)));
    size_t app_pack_length = (size_t) mutable_app->audio_bytes;
    unsigned char *app_pack = malloc(app_pack_length);
    FILE *app_pack_file = fopen(app_pack_path, "rb");
    CHECK(app_pack != NULL && app_pack_file != NULL
          && fread(app_pack, 1, app_pack_length, app_pack_file)
                 == app_pack_length
          && fclose(app_pack_file) == 0
          && app_pack[8] == 2u
          && (app_pack[24] != 0 || app_pack[25] != 0
              || app_pack[26] != 0 || app_pack[27] != 0));
    memset(app_pack + 28u, 0, 4u);
    app_pack_file = fopen(app_pack_path, "wb");
    CHECK(app_pack_file != NULL
          && fwrite(app_pack, 1, app_pack_length, app_pack_file)
                 == app_pack_length
          && fflush(app_pack_file) == 0
          && fclose(app_pack_file) == 0);
    mutable_app->auxiliary_hash = test_offline_hash(
        app_pack, app_pack_length);
    free(app_pack);
    browser_session_cache_clear(&app_session);
    html = NULL;
    html_length = 0;
    CHECK(offline_library_read_web_app(
              &loaded, &budget, &app_session, app_id,
              &html, &html_length, error, sizeof(error))
          && strstr(html, "Heading") != NULL
          && browser_session_classic_script_bytecode_acquire(
                 &app_session, install_script_context.target_url,
                 install_compiled_script,
                 sizeof(install_compiled_script) - 1u) == NULL);
    budget_free(&budget, html);
    browser_session_destroy(&app_session);
    listing = NULL;
    CHECK(offline_library_build_page(
              &loaded, &budget, &listing, &listing_length)
          && strstr(listing, "Offline web app") != NULL
          && strstr(listing, "/offline/app?id=") != NULL);
    budget_free(&budget, listing);
    CHECK(offline_library_remove(&loaded, app_id));

    char index[200], video[200], audio[200];
    snprintf(index, sizeof(index), "%s/library.bin", directory);
    CHECK(offline_library_item_path(
        &loaded, video_id, ".video.mp4", video, sizeof(video)));
    CHECK(offline_library_item_path(
        &loaded, video_id, ".audio.mp4", audio, sizeof(audio)));
    (void) unlink(video);
    (void) unlink(audio);
    (void) unlink(index);
    (void) unlink(index_temporary);
    (void) unlink(index_backup);
    CHECK(rmdir(directory) == 0);
    document_destroy(&document);
    CHECK(budget_uninstall_lexbor(&budget));
    CHECK(budget.current == 0);
    puts("offline-library-tests: ok");
    return 0;
}
