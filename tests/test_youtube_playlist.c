#include "tilefinch/youtube_playlist.h"
#include "tilefinch/browser_engine.h"
#include "tilefinch/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); return 1; } } while (0)

static void make_page(YoutubePlaylistPage *p, size_t first, size_t count, bool more)
{
    memset(p, 0, sizeof(*p)); p->first = first; p->count = count;
    for (size_t i = 0; i < count; i++) {
        snprintf(p->entries[i].video_id, sizeof(p->entries[i].video_id), "TFTEST%05zu", first + i);
        snprintf(p->entries[i].title, sizeof(p->entries[i].title), "Entry %zu", first + i + 1);
    }
    if (more) snprintf(p->next_url, sizeof(p->next_url),
        "https://www.youtube.com/playlist?list=PLfixture&tilefinch_base=%zu", first + count);
}

int main(void)
{
    Budget budget; budget_init(&budget, 8u * 1024u * 1024u);
    BrowserSession session; CHECK(browser_session_init(&session, &budget, 256u * 1024u));
    const char *url = "https://www.youtube.com/playlist?list=PLfixture";
    size_t before_queue = budget.current;
    YoutubePlaylistQueue *q = youtube_playlist_begin(&budget, &session, url, 10, false, "TFTEST00010");
    CHECK(q && youtube_playlist_loading(q));
    youtube_playlist_set_initial_start(q, UINT64_C(42000000));
    CHECK(budget.current - before_queue <= 16u * 1024u);
    YoutubePlaylistPage page; make_page(&page, 0, 12, true);
    /* Repeated videos are distinct playlist positions, not deduplicated. */
    memcpy(page.entries[11].video_id, page.entries[10].video_id, sizeof(page.entries[11].video_id));
    CHECK(youtube_playlist_accept_page(q, &page));
    char video[128];
    CHECK(!youtube_playlist_take_video(q, NULL, sizeof(video)));
    CHECK(!youtube_playlist_take_video(q, video, 0));
    CHECK(youtube_playlist_view(q, NULL, 6) == 0);
    CHECK(youtube_playlist_take_video(q, video, sizeof(video)));
    CHECK(strstr(video, "TFTEST00010") && youtube_playlist_position(q) == 10);
    CHECK(strstr(video, "&t=42"));
    CHECK(!youtube_playlist_take_video(q, video, sizeof(video)));
    CHECK(youtube_playlist_step(q, 1) && youtube_playlist_take_video(q, video, sizeof(video)));
    CHECK(strstr(video, "TFTEST00010") && youtube_playlist_position(q) == 11);
    CHECK(!strstr(video, "&t="));
    CHECK(youtube_playlist_step(q, 1) && youtube_playlist_loading(q) && youtube_playlist_selecting(q));
    make_page(&page, 12, 12, true); CHECK(youtube_playlist_accept_page(q, &page));
    CHECK(youtube_playlist_take_video(q, video, sizeof(video)) && strstr(video, "TFTEST00012"));
    CHECK(!youtube_playlist_selecting(q));
    CHECK(youtube_playlist_step(q, -1) && youtube_playlist_take_video(q, video, sizeof(video)));
    CHECK(youtube_playlist_position(q) == 11);
    CHECK(youtube_playlist_select(q, 35));
    make_page(&page, 24, 12, true); CHECK(youtube_playlist_accept_page(q, &page));
    CHECK(youtube_playlist_take_video(q, video, sizeof(video)) && strstr(video, "TFTEST00035"));
    CHECK(youtube_playlist_step(q, 1));
    make_page(&page, 36, 12, true); CHECK(youtube_playlist_accept_page(q, &page));
    CHECK(youtube_playlist_take_video(q, video, sizeof(video)) && strstr(video, "TFTEST00036"));
    YoutubePlaylistEntry view[6]; CHECK(youtube_playlist_view(q, view, 6) == 6);
    CHECK(!strcmp(view[0].title, "Entry 37"));
    /* Rewind beyond the bounded window replays pages cooperatively. */
    CHECK(youtube_playlist_select(q, 1));
    make_page(&page, 0, 12, true); CHECK(youtube_playlist_accept_page(q, &page));
    CHECK(youtube_playlist_take_video(q, video, sizeof(video)) && strstr(video, "TFTEST00001"));
    CHECK(youtube_playlist_select(q, 12));
    make_page(&page, 12, 1, false); CHECK(youtube_playlist_accept_page(q, &page));
    CHECK(youtube_playlist_take_video(q, video, sizeof(video)));
    CHECK(!youtube_playlist_has_next(q) && !youtube_playlist_step(q, 1));
    CHECK(youtube_playlist_select(q, 0));
    make_page(&page, 13, 1, true);
    snprintf(page.next_url, sizeof(page.next_url), "https://unrelated.test/playlist?list=PLfixture");
    CHECK(!youtube_playlist_accept_page(q, &page) && youtube_playlist_failed(q));
    youtube_playlist_retry(q); CHECK(youtube_playlist_failed(q)
        && !youtube_playlist_can_retry(q) && !youtube_playlist_loading(q));
    youtube_playlist_destroy(q);
    /* Empty final continuation must remain a visible terminal failure. */
    q = youtube_playlist_begin(&budget, &session, url, 0, false, NULL);
    CHECK(q); make_page(&page, 0, 1, true);
    CHECK(youtube_playlist_accept_page(q, &page));
    CHECK(youtube_playlist_take_video(q, video, sizeof(video)));
    CHECK(youtube_playlist_step(q, 1));
    make_page(&page, 1, 0, false);
    CHECK(!youtube_playlist_accept_page(q, &page));
    youtube_playlist_retry(q);
    CHECK(youtube_playlist_failed(q) && !youtube_playlist_loading(q));
    CHECK(!youtube_playlist_pump(q, NULL));
    youtube_playlist_destroy(q);
    q = youtube_playlist_begin(&budget, &session,
        "https://www.youtube.com/playlist?list=PLfixture&tilefinch_base=24", 10, true, "TFTEST99999");
    CHECK(q); make_page(&page, 0, 12, false);
    CHECK(youtube_playlist_accept_page(q, &page) && youtube_playlist_take_video(q, video, sizeof(video)));
    CHECK(youtube_playlist_position(q) == 0 && strstr(video, "TFTEST00000"));
    CHECK(!strstr(video, "&t="));
    youtube_playlist_destroy(q);
    size_t owned = budget.current; size_t limit = budget.limit;
    budget.limit = budget.current;
    CHECK(!youtube_playlist_begin(&budget, &session, url, 0, true, NULL));
    CHECK(budget.current == owned); budget.limit = limit;
    CHECK(!youtube_playlist_begin(&budget, &session, "https://unrelated.test/playlist?list=PLfixture", 0, false, NULL));
    q = youtube_playlist_begin(&budget, &session, url, 0, false, "TFTEST99999");
    CHECK(q); make_page(&page, 0, 12, false);
    CHECK(!youtube_playlist_accept_page(q, &page) && youtube_playlist_failed(q));
    CHECK(!youtube_playlist_take_video(q, video, sizeof(video)));
    CHECK(youtube_playlist_can_retry(q));
    youtube_playlist_retry(q);
    CHECK(!youtube_playlist_failed(q) && youtube_playlist_loading(q));
    youtube_playlist_destroy(q);

    /* One service page with >12 entries must preserve its tail before using
       the server continuation, including duplicate IDs at adjacent positions. */
    char source[12000]; size_t used = (size_t) snprintf(source, sizeof(source), "<script>var ytInitialData='{\"metadata\":{\"playlistMetadataRenderer\":{\"title\":\"Weekend journeys\"}},\"contents\":[");
    for (unsigned i = 0; i < 15; i++) used += (size_t) snprintf(source + used, sizeof(source) - used,
        "%s{\"playlistVideoRenderer\":{\"videoId\":\"TFTEST%05u\",\"title\":{\"simpleText\":\"Entry %u\"}}}",
        i ? "," : "", i == 11 ? 10 : i, i + 1);
    used += (size_t) snprintf(source + used, sizeof(source) - used,
        ",{\"continuationItemRenderer\":{\"continuationEndpoint\":{\"continuationCommand\":{\"token\":\"fixture-next\"}}}}]}';</script>");
    YoutubeLiteDocument doc = {0}; char error[256];
    static const char lockups[] =
        "<script>var ytInitialData='{\"contents\":["
        "{\"lockupViewModel\":{\"contentType\":\"LOCKUP_CONTENT_TYPE_VIDEO\",\"contentId\":\"TFTEST00000\","
        "\"metadata\":{\"lockupMetadataViewModel\":{\"title\":{\"content\":\"New playlist card\"}}},"
        "\"rendererContext\":{\"commandContext\":{\"onTap\":{\"innertubeCommand\":{\"watchEndpoint\":{\"videoId\":\"TFTEST00000\",\"playlistId\":\"PLfixture\"}}}}}}},"
        "{\"lockupViewModel\":{\"contentType\":\"LOCKUP_CONTENT_TYPE_VIDEO\",\"contentId\":\"TFTEST00001\","
        "\"rendererContext\":{\"commandContext\":{\"onTap\":{\"innertubeCommand\":{\"watchEndpoint\":{\"videoId\":\"TFTEST00001\",\"playlistId\":\"PLother\"}}}}}}},"
        "{\"lockupViewModel\":{\"contentType\":\"LOCKUP_CONTENT_TYPE_VIDEO\",\"contentId\":\"TFTEST00002\","
        "\"rendererContext\":{\"commandContext\":{\"onTap\":{\"innertubeCommand\":{\"watchEndpoint\":{\"videoId\":\"TFTEST00003\",\"playlistId\":\"PLfixture\"}}}}}}},"
        "{\"lockupViewModel\":{\"contentType\":\"LOCKUP_CONTENT_TYPE_VIDEO\",\"contentId\":\"TFTEST00004\","
        "\"menu\":{\"watchEndpoint\":{\"videoId\":\"TFTEST00004\",\"playlistId\":\"PLfixture\"}}}}]}';</script>";
    CHECK(youtube_lite_build_document(&budget, url, lockups, sizeof(lockups) - 1u, &doc, error, sizeof(error)));
    CHECK(doc.playlist && doc.playlist->count == 1
        && !strcmp(doc.playlist->entries[0].video_id, "TFTEST00000")
        && !strcmp(doc.playlist->entries[0].title, "New playlist card"));
    youtube_lite_document_destroy(&doc);
    bool built = youtube_lite_build_document(&budget, url, source, used, &doc, error, sizeof(error));
    if (!built) fprintf(stderr, "build: %s\n", error);
    CHECK(built);
    CHECK(doc.playlist && doc.playlist->count == 12 && doc.playlist->first == 0);
    CHECK(!strcmp(doc.playlist->entries[10].video_id, doc.playlist->entries[11].video_id));
    CHECK(strstr(doc.html, "Play all") && strstr(doc.html, "Play from here"));
    CHECK(strstr(doc.html, "<h1>Weekend journeys</h1>"));
    CHECK(strstr(doc.html, "tilefinch_queue=0\" aria-label=\"Play from here\""));
    /* One native play target per card, not a second action row. */
    CHECK(!strstr(doc.html, ">Play from here</a>"));
    CHECK(strstr(doc.playlist->next_url, "tilefinch_offset=12") && !strstr(doc.playlist->next_url, "fixture-next"));
    BrowserConfig config; browser_config_init(&config, NULL);
    config.resources.enabled = false; config.javascript.enabled = false;
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine && browser_engine_commit_html(engine, url, doc.html, doc.html_length, true));
    NavigationSession *navigation = browser_engine_navigation(engine);
    const LinkRegion *first_card = NULL, *play_all = NULL;
    size_t card_count = 0;
    for (size_t i = 0; i < navigation->page.layout.link_count; i++) {
        const LinkRegion *link = &navigation->page.layout.links[i];
        if (strstr(link->url, "tilefinch_queue=all")) play_all = link;
        else if (strstr(link->url, "tilefinch_queue=")) {
            card_count++;
            if (strstr(link->url, "tilefinch_queue=0")) first_card = link;
        }
    }
    CHECK(card_count == 12 && first_card && play_all);
    CHECK(play_all->y < first_card->y && first_card->height > 0
        && first_card->y + first_card->height <= 272);
    lxb_dom_node_t *selected = NULL;
    for (size_t i = 0; i < navigation->page.layout.link_count; i++)
        if (strstr(navigation->page.layout.links[i].url, "tilefinch_queue=11"))
            selected = navigation->page.layout.links[i].node;
    CHECK(selected && browser_engine_focus_node(engine, selected));
    const BrowserController *controller = browser_engine_controller_view(engine);
    int scroll = navigation_current(navigation)->scroll_y;
    size_t focus = controller->focus_index; uint64_t generation = navigation->generation;
    ControllerAction action = {0}; CHECK(browser_engine_activate(engine, &action));
    CHECK(action.prefer_native_media && strstr(action.url, "tilefinch_queue=11"));
    CHECK(navigation->generation == generation && !strcmp(navigation_current(navigation)->url, url)
        && navigation_current(navigation)->scroll_y == scroll && controller->focus_index == focus);
    /* Playback is a native action, not another document/history entry. */
    CHECK(browser_engine_render_frame(engine, NULL));
    CHECK(navigation->generation == generation && navigation_current(navigation)->scroll_y == scroll && controller->focus_index == focus);
    Budget *engine_budget = browser_engine_budget(engine);
    CHECK(browser_engine_shutdown(engine) && engine_budget->current == 0);
    browser_engine_destroy(engine);
    char next[2048]; snprintf(next, sizeof(next), "%s", doc.playlist->next_url);
    youtube_lite_document_destroy(&doc);
    CHECK(youtube_lite_build_document(&budget, next, source, used, &doc, error, sizeof(error)));
    CHECK(doc.playlist && doc.playlist->first == 12 && doc.playlist->count == 3);
    CHECK(!strcmp(doc.playlist->entries[0].video_id, "TFTEST00012"));
    CHECK(strstr(doc.playlist->next_url, "tilefinch_base=15") && strstr(doc.playlist->next_url, "fixture-next"));
    youtube_lite_document_destroy(&doc);
    /* A refused document/facts allocation must leave nothing owned. */
    size_t retained = budget.current;
    for (size_t allowance = 0; allowance < 65536u; allowance += 1024u) {
        budget.limit = retained + allowance;
        bool okay = youtube_lite_build_document(&budget, url, source, used, &doc, error, sizeof(error));
        if (okay) youtube_lite_document_destroy(&doc);
        CHECK(budget.current == retained);
    }
    budget.limit = limit;
    /* Real cooperative loader -> authority-bound facts cache -> queue.
       Starting from an already displayed page must need no second fetch. */
    char directory[] = "/tmp/tilefinch-playlist-XXXXXX", path[256];
    CHECK(mkdtemp(directory));
    snprintf(path, sizeof(path), "%s/trace.meta", directory);
    FILE *file = fopen(path, "wb"); CHECK(file);
    CHECK(fprintf(file, "psp-http-trace-clock=1\norigin-ms=1700000000000\ncapture-complete=yes\nrecord-count=1\n") > 0 && fclose(file) == 0);
    snprintf(path, sizeof(path), "%s/0000.meta", directory);
    file = fopen(path, "wb"); CHECK(file);
    CHECK(fprintf(file, "psp-http-trace=1\nmethod=GET\nurl=https://m.youtube.com/playlist?list=PLfixture\n"
        "success=1\nstatus=200\nlength=%zu\neffective-url=https://m.youtube.com/playlist?list=PLfixture\n"
        "content-type=text/html; charset=utf-8\nset-cookie-count=0\n", used) > 0 && fclose(file) == 0);
    snprintf(path, sizeof(path), "%s/0000.body", directory);
    file = fopen(path, "wb"); CHECK(file);
    CHECK(fwrite(source, 1, used, file) == used && fclose(file) == 0);
    CHECK(fetch_trace_replay_begin(directory, error, sizeof(error)));
    CHECK(youtube_lite_load(&budget, &session, url, YOUTUBE_LITE_MAXIMUM_SOURCE_BYTES, 1000, &doc, error, sizeof(error)));
    youtube_lite_document_destroy(&doc);
    CHECK(youtube_lite_copy_playlist(&session, url, &page) && page.count == 12);
    q = youtube_playlist_begin(&budget, &session, url, 10, false, "TFTEST00010"); CHECK(q);
    CHECK(youtube_playlist_pump(q, NULL) && youtube_playlist_take_video(q, video, sizeof(video)));
    CHECK(strstr(video, "TFTEST00010") && !youtube_playlist_failed(q));
    youtube_playlist_destroy(q);
    /* A rejected cached page must not make Retry reuse those same stale
       facts or the matching cached HTML. Only this page is invalidated. */
    CHECK(youtube_lite_copy_playlist(&session, url, &page));
    strcpy(page.entries[10].video_id, "TFTEST99999");
    TilefinchRequestContext authority = {
        .target_url = "https://m.youtube.com/playlist?list=PLfixture",
        .top_level_url = url, .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_NAVIGATE,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_DOCUMENT,
        .top_level_navigation = true, .user_activated = true
    };
    CHECK(browser_session_site_adapter_document_cache_put(&session,
        "youtube-playlist-facts", url, 0, &authority, &page, sizeof(page),
        used, 12, 200, "", "", tilefinch_platform_monotonic_time_ns()));
    q = youtube_playlist_begin(&budget, &session, url, 10, false, "TFTEST00010");
    CHECK(q && youtube_playlist_pump(q, NULL) && youtube_playlist_failed(q));
    fetch_trace_end();
    CHECK(fetch_trace_replay_begin(directory, error, sizeof(error)));
    youtube_playlist_retry(q);
    CHECK(!youtube_lite_copy_playlist(&session, url, &page));
    for (unsigned turns = 0; turns < 4096u && youtube_playlist_loading(q); turns++)
        (void) youtube_playlist_pump(q, NULL);
    CHECK(!youtube_playlist_failed(q) && youtube_playlist_take_video(q, video, sizeof(video))
        && strstr(video, "TFTEST00010"));
    youtube_playlist_destroy(q); fetch_trace_end();
    const char *files[] = {"trace.meta", "0000.meta", "0000.body"};
    for (unsigned i = 0; i < 3; i++) { snprintf(path, sizeof(path), "%s/%s", directory, files[i]); CHECK(unlink(path) == 0); }
    CHECK(rmdir(directory) == 0);
    size_t index; bool all;
    CHECK(youtube_lite_playlist_selection("https://www.youtube.com/watch?v=TFTEST00001&tilefinch_queue=all", &index, &all) && all);
    CHECK(youtube_lite_playlist_selection("https://www.youtube.com/watch?v=TFTEST00001&tilefinch_queue=11", &index, &all) && !all && index == 11);
    CHECK(!youtube_lite_playlist_selection("https://www.youtube.com/watch?v=TFTEST00001&tilefinch_queue=12", &index, &all));
    browser_session_destroy(&session); CHECK(budget.current == 0);
    puts("youtube-playlist: order, boundaries, retry, refusal and teardown passed");
    return 0;
}
