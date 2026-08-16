#include "tilefinch/offline_library.h"
#include "tilefinch/media_file.h"
#include "tilefinch/psp_offline_store.h"

#include <stdio.h>
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
        "<!doctype html><title>A &amp; B</title><nav>Do not save me</nav>"
        "<main><h1>Heading</h1><aside>Also do not save me</aside>"
        "<p>One &amp; two.</p><script>hidden()</script><p>Three.</p></main>";
    CHECK(document_parse(&document, &budget, page, sizeof(page) - 1u, 4096));

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
          && access(orphan_path, F_OK) != 0);

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
