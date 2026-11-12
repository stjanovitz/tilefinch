#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tilefinch/budget.h"
#include "tilefinch/document.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/media_discovery.h"

#define MIB (1024u * 1024u)
#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #condition);                                                 \
        return 1;                                                            \
    }                                                                        \
} while (0)

static lxb_dom_node_t *find_element_by_id(lxb_dom_node_t *node,
                                          const char *wanted)
{
    size_t wanted_length = strlen(wanted);
    for (; node != NULL; node = node->next) {
        size_t length = 0;
        const char *id = document_attribute(node, "id", &length);
        if (id != NULL && length == wanted_length
            && memcmp(id, wanted, length) == 0) return node;
        lxb_dom_node_t *child = find_element_by_id(node->first_child, wanted);
        if (child != NULL) return child;
    }
    return NULL;
}

static int test_structured_scan_parity(void)
{
    Budget budget;
    budget_init(&budget, 4u * MIB);
    budget_install_lexbor(&budget);
    char html[8192];
    size_t length = (size_t) snprintf(html, sizeof(html),
        "<!doctype html><script type=application/ld+json>{</script>"
        "<script>window.data={broken}</script>");
    for (unsigned i = 0; i < 16u; i++) {
        int written = snprintf(html + length, sizeof(html) - length,
            "<div>ignored</div><script type=application/ld+json>["
            "{\"@type\":\"AudioObject\",\"contentUrl\":\"/audio-%u.m4a\"},"
            "{\"@type\":\"VideoObject\",\"contentUrl\":\"/video-%u.mp4\"}]"
            "</script>", i, i);
        CHECK(written >= 0 && (size_t) written < sizeof(html) - length);
        length += (size_t) written;
    }
    PocDocument document;
    CHECK(document_parse(&document, &budget, html, length, 42));
    size_t retained = budget.current;
    MediaStructuredAudioIndex audio;
    MediaStructuredVideoIndex video;
    CHECK(media_discover_structured_audio(&document, &audio)
          && media_discover_structured_video(&document, &video));
    CHECK(audio.candidate_count == MEDIA_STRUCTURED_AUDIO_CANDIDATE_LIMIT
          && video.candidate_count == MEDIA_STRUCTURED_VIDEO_CANDIDATE_LIMIT
          && audio.candidate_overflow == 4u && video.candidate_overflow == 8u
          && audio.inspected_bytes == video.inspected_bytes
          && audio.inspected_nodes == video.inspected_nodes
          && audio.malformed_scripts == 1u && video.malformed_scripts == 1u
          && audio.truncated_scripts == 0u && video.truncated_scripts == 0u);
    char url[64];
    CHECK(media_structured_audio_copy_url(&audio.candidates[11], url, sizeof(url))
          && strcmp(url, "/audio-11.m4a") == 0
          && media_structured_video_copy_url(&video.candidates[7], url, sizeof(url))
          && strcmp(url, "/video-7.mp4") == 0
          && budget.current == retained);
    /* Invalid documents clear either result rather than retaining a previous
       scan. Null result pointers must fail softly as before. */
    CHECK(!media_discover_structured_audio(NULL, &audio)
          && !media_discover_structured_video(NULL, &video)
          && audio.candidate_count == 0u && audio.inspected_bytes == 0u
          && video.candidate_count == 0u && video.inspected_bytes == 0u
          && !media_discover_structured_audio(&document, NULL)
          && !media_discover_structured_video(&document, NULL));
    document_destroy(&document);
    CHECK(budget.current == 0u);
    return 0;
}

/* Executable inline scripts are read for references only by the
   activation-time scan, and only as string literals: comments, regular
   expressions and relative names are not references. */
static int test_executable_script_references(void)
{
    Budget budget;
    budget_init(&budget, 4u * MIB);
    budget_install_lexbor(&budget);
    char url[256];
    char streaming[256];
    MediaDiscoveryResult result;

    /* The markup offers a direct MP4; the HLS master appears only in a
       player call, with its slashes escaped as JS string literals allow. */
    static const char player_call_html[] =
        "<!doctype html><body><video></video>"
        "<meta itemprop=contentUrl content='/clip/clip-480.mp4'>"
        "<script>var player=createPlayer('stage');"
        "player.setStream('https:\\/\\/video.test\\/clip\\/hls\\/"
        "master.m3u8?token=a1');player.start();</script></body>";
    PocDocument document;
    CHECK(document_parse(&document, &budget, player_call_html,
                         sizeof(player_call_html) - 1u, 51));
    CHECK(media_discover_document_candidates(
              &document, NULL, NULL, url, sizeof(url), streaming, sizeof(streaming),
              &result)
          && strcmp(url, "/clip/clip-480.mp4") == 0
          && result.kind == MEDIA_DISCOVERY_MP4
          && strcmp(streaming,
                    "https://video.test/clip/hls/master.m3u8?token=a1")
                 == 0);
    document_destroy(&document);

    /* No markup reference: a player configuration object lists MP4
       renditions under quality keys, larger first. */
    static const char quality_keys_html[] =
        "<!doctype html><body><video></video><script>"
        "var config = {poster: '/clip/poster.jpg',"
        " mp4_high: \"/clip/files/clip-a.mp4\","
        " mp4_low: '/clip/files/clip-b.mp4?sig=1&e=2',"
        " hls: \"/clip/hls/index.m3u8\"};"
        "setupPlayer(document.querySelector('video'), config);"
        "</script></body>";
    CHECK(document_parse(&document, &budget, quality_keys_html,
                         sizeof(quality_keys_html) - 1u, 52));
    CHECK(media_discover_document_candidates(
              &document, NULL, NULL, url, sizeof(url), streaming, sizeof(streaming),
              &result)
          && strcmp(url, "/clip/files/clip-b.mp4?sig=1&e=2") == 0
          && result.kind == MEDIA_DISCOVERY_MP4 && result.quality == 240u
          && strcmp(streaming, "/clip/hls/index.m3u8") == 0);
    /* Page-load declaration discovery keeps reading markup only. */
    CHECK(!media_discover_document_candidate(
        &document, url, sizeof(url), &result));
    document_destroy(&document);

    /* URL-looking text in comments and in a regular expression is not a
       reference (here each would outrank the real one), nor is a relative
       name; a division is not mistaken for a regular expression. */
    static const char lookalike_html[] =
        "<!doctype html><body><video></video><script>"
        "// player.load(\"/old/clip-240.mp4\");\n"
        "/* setStream('/old/live-240.m3u8') */\n"
        "var pattern = /src=\"\\/x\\/[a-z]+-240.mp4\"/;"
        "var name = 'clip-240.mp4', half = width / 2;"
        "player.load(\"/clip/clip-360.mp4\");"
        "</script></body>";
    CHECK(document_parse(&document, &budget, lookalike_html,
                         sizeof(lookalike_html) - 1u, 53));
    CHECK(media_discover_document_candidates(
              &document, NULL, NULL, url, sizeof(url), streaming, sizeof(streaming),
              &result)
          && strcmp(url, "/clip/clip-360.mp4") == 0
          && result.quality == 360u && streaming[0] == '\0');
    document_destroy(&document);

    /* Inert script types are not executable code. */
    static const char template_html[] =
        "<!doctype html><body><script type=text/template>"
        "player.load('/clip/template.mp4')</script></body>";
    CHECK(document_parse(&document, &budget, template_html,
                         sizeof(template_html) - 1u, 54));
    CHECK(!media_discover_document_candidates(
        &document, NULL, NULL, url, sizeof(url), streaming, sizeof(streaming),
        &result));
    document_destroy(&document);
    CHECK(budget.current == 0u);
    return 0;
}

/* The quality a player configuration's single MP4 key gives its
   reference, through the executable-script scan. */
static unsigned script_key_quality(Budget *budget, const char *key)
{
    char html[256];
    snprintf(html, sizeof(html),
             "<!doctype html><body><video></video><script>"
             "var config = {poster: '/clip/poster.jpg',"
             " %s: '/clip/files/clip.mp4'};"
             "setupPlayer(document.querySelector('video'), config);"
             "</script></body>", key);
    PocDocument document;
    char url[256];
    char streaming[256];
    MediaDiscoveryResult result;
    if (!document_parse(&document, budget, html, strlen(html), 55))
        return 9999u;
    bool found = media_discover_document_candidates(
        &document, NULL, NULL, url, sizeof(url), streaming,
        sizeof(streaming), &result);
    document_destroy(&document);
    return found && strcmp(url, "/clip/files/clip.mp4") == 0
        ? result.quality : 9999u;
}

/* Quality words in script keys are whole words: a key that merely contains
   "low", "med", "mid", "high" or "hd" names no quality. */
static int test_script_key_quality_words(void)
{
    Budget budget;
    budget_init(&budget, 4u * MIB);
    budget_install_lexbor(&budget);
    static const struct {
        const char *key;
        unsigned quality;
    } cases[] = {
        {"low", 240u}, {"lowQuality", 240u}, {"url_low", 240u},
        {"mp4-low", 240u}, {"videoSmall", 240u}, {"mobileSrc", 240u},
        {"low2", 240u}, {"medium", 480u}, {"srcMid", 480u},
        {"hd", 720u}, {"hdUrl", 720u}, {"HDUrl", 720u}, {"videoHD", 720u},
        {"url_hq", 720u}, {"highQuality", 720u},
        {"'720p'", 720u}, {"mp4_360", 360u},
        /* Both named: the lower one, as before. */
        {"lowOrHigh", 240u},
        /* Substrings only. */
        {"download", 0u}, {"workflow", 0u}, {"slowMotion", 0u},
        {"media", 0u}, {"mediaUrl", 0u}, {"amid", 0u},
        {"highlights", 0u}, {"thigh", 0u}, {"shdw", 0u}, {"pathd", 0u}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        unsigned quality = script_key_quality(&budget, cases[i].key);
        if (quality != cases[i].quality) {
            fprintf(stderr, "key %s: quality %u, expected %u\n",
                    cases[i].key, quality, cases[i].quality);
            return 1;
        }
    }
    CHECK(budget.current == 0u);
    return 0;
}

/* A digit run longer than four digits is an ID, not a quality: a 3-4
   digit piece of it must not read as one. */
static int test_quality_hint_ignores_id_fragments(void)
{
    Budget budget;
    budget_init(&budget, 4u * MIB);
    budget_install_lexbor(&budget);
    char url[256];
    MediaDiscoveryResult result;
    static const char id_only_html[] =
        "<!doctype html><body>"
        "<div data-src='/v/202403/449472141/clip.mp4'></div></body>";
    PocDocument document;
    CHECK(document_parse(&document, &budget, id_only_html,
                         sizeof(id_only_html) - 1u, 61));
    CHECK(media_discover_document_candidate(
              &document, url, sizeof(url), &result)
          && result.quality == 0u);
    document_destroy(&document);
    static const char id_and_quality_html[] =
        "<!doctype html><body>"
        "<div data-src='/v/449472141/clip_360.mp4'></div></body>";
    CHECK(document_parse(&document, &budget, id_and_quality_html,
                         sizeof(id_and_quality_html) - 1u, 62));
    CHECK(media_discover_document_candidate(
              &document, url, sizeof(url), &result)
          && result.quality == 360u);
    document_destroy(&document);
    /* Nor is a year segment of the path ("/2026/10/") a quality. */
    static const char year_path_html[] =
        "<!doctype html><body>"
        "<div data-src='/v/2026/10/449472141_4500.mp4'></div></body>";
    CHECK(document_parse(&document, &budget, year_path_html,
                         sizeof(year_path_html) - 1u, 63));
    CHECK(media_discover_document_candidate(
              &document, url, sizeof(url), &result)
          && result.quality == 0u);
    document_destroy(&document);
    CHECK(budget.current == 0u);
    return 0;
}

/* The activation scan's own-video signals beyond the page address: an ID
   in the activated element's poster, the player's container, and small
   thumbnail frame sizes in a path. Page-load discovery has none of them. */
static int test_activation_affinity_signals(void)
{
    Budget budget;
    budget_init(&budget, 4u * MIB);
    budget_install_lexbor(&budget);
    char url[256];
    char streaming[256];
    MediaDiscoveryResult result;
    PocDocument document;

    static const char poster_html[] =
        "<!doctype html><body><div class=player><video id=main "
        "poster='https://img.test/p/202405/987654/poster.jpg'></video></div>"
        "<div data-src='https://media.test/a/202405/111111/x-360.mp4'></div>"
        "<div data-src='https://media.test/a/202405/987654/x-720.mp4'></div>"
        "</body>";
    CHECK(document_parse(&document, &budget, poster_html,
                         sizeof(poster_html) - 1u, 63));
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *video = find_element_by_id(root, "main");
    CHECK(video != NULL
          && media_discover_document_candidates(
                 &document, "https://video.test/watch", video, url,
                 sizeof(url), streaming, sizeof(streaming), &result)
          && strcmp(url, "https://media.test/a/202405/987654/x-720.mp4")
                 == 0);
    /* Without the element, the date-shaped run shared by both paths is
       not an identity: the quality policy decides. */
    CHECK(media_discover_document_candidates(
              &document, "https://video.test/watch", NULL, url, sizeof(url),
              streaming, sizeof(streaming), &result)
          && strcmp(url, "https://media.test/a/202405/111111/x-360.mp4")
                 == 0);
    CHECK(media_discover_document_candidate(
              &document, url, sizeof(url), &result)
          && strcmp(url, "https://media.test/a/202405/111111/x-360.mp4")
                 == 0);
    document_destroy(&document);

    static const char container_html[] =
        "<!doctype html><body>"
        "<div data-src='/strip/160x90/clip-360.mp4'></div>"
        "<section><div data-src='/clips/rail-360.mp4'></div></section>"
        "<div class=player><div class=stage><video id=main></video></div>"
        "<span data-src='/clips/main-720.mp4'></span></div></body>";
    CHECK(document_parse(&document, &budget, container_html,
                         sizeof(container_html) - 1u, 64));
    root = lxb_dom_interface_node(document.html);
    video = find_element_by_id(root, "main");
    CHECK(video != NULL
          && media_discover_document_candidates(
                 &document, NULL, video, url, sizeof(url), streaming,
                 sizeof(streaming), &result)
          && strcmp(url, "/clips/main-720.mp4") == 0);
    /* Without the element the thumbnail strip still ranks last. */
    CHECK(media_discover_document_candidates(
              &document, NULL, NULL, url, sizeof(url), streaming,
              sizeof(streaming), &result)
          && strcmp(url, "/clips/rail-360.mp4") == 0);
    document_destroy(&document);
    CHECK(budget.current == 0u);
    return 0;
}

/* The activation scan of one page, given its address, with no element. */
static bool discover_page(
    Budget *budget, const char *html, size_t length, const char *page_url,
    char *url, size_t capacity, MediaDiscoveryResult *result)
{
    PocDocument document;
    char streaming[512];
    if (!document_parse(&document, budget, html, length, 71)) {
        snprintf(url, capacity, "<parse failed>");
        return false;
    }
    bool found = media_discover_document_candidates(
        &document, page_url, NULL, url, capacity, streaming,
        sizeof(streaming), result);
    document_destroy(&document);
    return found;
}

/* A rehydration blob holds an app-promotion MP4 long before the page's own
   video, whose address has no media suffix. The promotion is never
   offered; the extensionless address is, under its strong video key, only
   beside the page's own ID, never under a sound's key, and never by
   page-load discovery. Better nothing than another video. */
static int test_promo_and_extensionless_video(void)
{
    Budget budget;
    budget_init(&budget, 4u * MIB);
    budget_install_lexbor(&budget);
    char url[512];
    MediaDiscoveryResult result;
    static const char page[] =
        "https://social.test/@someone/video/6718335390845095";
    static const char play_addr[] =
        "https://v.cdn.test/video/tos/a9d18e19b8504555/"
        "?a=1988&mime_type=video_mp4&sig=1";
#define PROMO_BLOB \
    "\"download_page_apk\":{\"title\":\"Get the app\",\"videos\":[" \
    "\"https:\\u002F\\u002Fstatic.test\\u002Fobj\\u002Fdownload" \
    "\\u002Fapk_new_8.mp4\"]}"
#define ITEM(id, extra) \
    "{\"id\":\"" id "\",\"desc\":\"clip\"," \
    "\"video\":{\"cover\":\"https:\\u002F\\u002Fimg.test\\u002Fc~tplv.image?x=1\"," \
    "\"thumbUrl\":\"https:\\u002F\\u002Fimg.test\\u002Fthumb\\u002Fq\"," \
    "\"playAddr\":\"https:\\u002F\\u002Fv.cdn.test\\u002Fvideo\\u002Ftos" \
    "\\u002Fa9d18e19b8504555\\u002F?a=1988\\u0026mime_type=video_mp4" \
    "\\u0026sig=1\"" extra "}," \
    "\"music\":{\"playUrl\":\"https:\\u002F\\u002Fm.cdn.test\\u002Fvideo" \
    "\\u002Ftos\\u002Fmusic77\\u002F?a=2\"}}"
    static const char rehydration_html[] =
        "<!doctype html><head><script type=application/json id=state>"
        "{\"app\":{\"abTest\":{" PROMO_BLOB "}},"
        "\"detail\":{\"itemInfo\":{\"itemStruct\":"
        ITEM("6718335390845095", "") "}}}"
        "</script></head><body></body>";
    CHECK(discover_page(&budget, rehydration_html,
                        sizeof(rehydration_html) - 1u, page, url,
                        sizeof(url), &result)
          && strcmp(url, play_addr) == 0
          && result.kind == MEDIA_DISCOVERY_MP4);
    /* Page-load discovery takes no extensionless address and drops the
       promotion: nothing. */
    PocDocument document;
    CHECK(document_parse(&document, &budget, rehydration_html,
                         sizeof(rehydration_html) - 1u, 72));
    CHECK(!media_discover_document_candidate(
        &document, url, sizeof(url), &result));
    document_destroy(&document);

    /* Another video's ID beside the address: nothing, not the promotion
       and not the other video. */
    static const char other_id_html[] =
        "<!doctype html><script type=application/json>"
        "{\"app\":{" PROMO_BLOB "},\"item\":" ITEM("5550001112223", "")
        "}</script>";
    CHECK(!discover_page(&budget, other_id_html, sizeof(other_id_html) - 1u,
                         page, url, sizeof(url), &result));

    /* A list of items, each with its own ID: the page's own item wins even
       after another one, and the earlier item's ID never reaches it. */
    static const char list_html[] =
        "<!doctype html><script type=application/json>"
        "{\"items\":[" ITEM("5550001112223", "") ","
        "{\"id\":\"6718335390845095\",\"video\":{\"playAddr\":"
        "\"https:\\/\\/v.cdn.test\\/video\\/tos\\/own\\/?a=7\"}}]}"
        "</script>";
    CHECK(discover_page(&budget, list_html, sizeof(list_html) - 1u, page,
                        url, sizeof(url), &result)
          && strcmp(url, "https://v.cdn.test/video/tos/own/?a=7") == 0);

    /* A sound's address under a strong key is no video, even beside the
       page's ID; nor is an image under one, nor an address under a key
       naming no video. */
    static const char not_video_html[] =
        "<!doctype html><script type=application/json>"
        "{\"id\":\"6718335390845095\","
        "\"music\":{\"playUrl\":\"https://m.cdn.test/a/77/?a=2\"},"
        "\"audioStreamUrl\":\"https://m.cdn.test/a/78/\","
        "\"videoUrl\":\"https://img.test/p/poster.jpeg?w=1\","
        "\"shareUrl\":\"https://social.test/s/6718335390845095/\"}"
        "</script>";
    CHECK(!discover_page(&budget, not_video_html, sizeof(not_video_html) - 1u,
                         page, url, sizeof(url), &result));

    /* Ad, promotion and app-download words in a key, an enclosing key or
       a path drop even a suffixed MP4 that carries the page's own ID; the
       page's real file under a plain key is kept, and "download" alone
       marks nothing. */
    static const char ads_html[] =
        "<!doctype html><script type=application/json>"
        "{\"adVideo\":\"/v/6718335390845095/ad-240.mp4\","
        "\"sponsored\":{\"items\":[{\"src\":\"/v/6718335390845095/s-240.mp4\"}]},"
        "\"hero\":{\"src\":\"/promo/6718335390845095/h-240.mp4\"},"
        "\"clip\":{\"src\":\"/v/6718335390845095/main-720.mp4\"}}"
        "</script>"
        "<div data-src='/ads/6718335390845095/banner-240.mp4'></div>";
    CHECK(discover_page(&budget, ads_html, sizeof(ads_html) - 1u, page, url,
                        sizeof(url), &result)
          && strcmp(url, "/v/6718335390845095/main-720.mp4") == 0);
    CHECK(document_parse(&document, &budget, ads_html,
                         sizeof(ads_html) - 1u, 73));
    CHECK(media_discover_document_candidate(
              &document, url, sizeof(url), &result)
          && strcmp(url, "/v/6718335390845095/main-720.mp4") == 0);
    document_destroy(&document);
    static const char download_html[] =
        "<!doctype html><script type=application/json>"
        "{\"download\":{\"url\":\"/files/download/clip-360.mp4\"}}</script>";
    CHECK(discover_page(&budget, download_html, sizeof(download_html) - 1u,
                        page, url, sizeof(url), &result)
          && strcmp(url, "/files/download/clip-360.mp4") == 0);
#undef ITEM
#undef PROMO_BLOB
    CHECK(budget.current == 0u);
    return 0;
}

/* A news-video page declares its own clip (HLS) in JSON-LD. After load its
   script adds a related-clips list, first in document order, whose items
   carry 1080p MP4s, then (as a failed hydration did on a live page) drops
   the head's declarations, leaving the page's own clip only as one item of
   that list. The page's own video is picked every time: identity outranks
   kind, size and order, and the PSP-undecodable 1080p file of the page's
   own clip yields to its HLS master. */
static int test_late_related_clips(void)
{
    Budget budget;
    budget_init(&budget, 14u * MIB);
    budget_install_lexbor(&budget);
    static const char page[] =
        "https://news.test/show/video/own-story-slug-4410023351";
    static const char own_hls[] = "https://cdn.test/own/abs/index.m3u8";
    static const char html[] =
        "<!doctype html><html><head>"
        "<link rel=canonical "
        "href='https://news.test/show/video/own-story-slug-4410023351'>"
        "<script type=application/ld+json>{\"@context\":\"https://schema.org\","
        "\"@type\":\"VideoObject\",\"name\":\"Own story\","
        "\"url\":\"https://news.test/show/video/own-story-slug-4410023351\","
        "\"contentUrl\":\"https://cdn.test/own/abs/index.m3u8\"}</script>"
        "</head><body><div id=app></div></body></html>";
    PocDocument document;
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 81));
    ScriptResult result;
    ScriptRuntime *runtime = script_runtime_create(
        &document, &budget, 12u * MIB, 4000, page, &result);
    CHECK(runtime != NULL && result.success);
    char url[512];
    char streaming[512];
    MediaDiscoveryResult discovery;
    const MediaDeclaredVideo *declared = media_declared_video_cached(&document);
    CHECK(declared != NULL && strcmp(declared->media_url, own_hls) == 0);

    /* Related clips declared as VideoObjects too, ahead of the page's own:
       a 1080p MP4, and a 360p one the PSP could decode. */
    static const char related_ld[] =
        "const s=document.createElement('script');"
        "s.type='application/ld+json';"
        "s.textContent=JSON.stringify([{'@type':'VideoObject',"
        "url:'https://news.test/show/video/flood-story-9990001112',"
        "contentUrl:'https://cdn.test/v/2026/10/flood_story_1080.mp4'},"
        "{'@type':'VideoObject',"
        "url:'https://news.test/show/video/court-story-9990001113',"
        "contentUrl:'https://cdn.test/v/2026/10/court_story_360.mp4'}]);"
        "document.head.insertBefore(s,document.head.firstChild);"
        "globalThis.pocSummary='LD-ADDED'";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, related_ld, "<late-related-ld>", &result)
          && strcmp(result.summary, "LD-ADDED") == 0);
    declared = media_declared_video_cached(&document);
    CHECK(declared != NULL && strcmp(declared->media_url, own_hls) == 0);
    CHECK(media_discover_document_candidates(
              &document, page, NULL, url, sizeof(url), streaming,
              sizeof(streaming), &discovery)
          && strcmp(url, own_hls) == 0
          && discovery.kind == MEDIA_DISCOVERY_HLS);

    /* The head's declarations go; a state blob lists related clips with
       the page's own one second, each item naming its ID before its
       renditions: an HLS master and a 1080p MP4 under a year path. */
#define ASSETS(slug) \
    "videoAssets:[{format:'M3U',publicUrl:'https://cdn.test/" slug \
    "/abs/index.m3u8',height:1080,width:1920},{format:'MPEG4'," \
    "publicUrl:'https://cdn.test/v/2026/10/17931_" slug "_4500.mp4'," \
    "height:1080,width:1920}]"
    static const char hydrate[] =
        "for(const e of document.head.querySelectorAll("
        "'script[type=\"application/ld+json\"],link[rel=canonical]'))"
        "e.remove();"
        "const d=document.createElement('script');"
        "d.type='application/json';d.id='state';"
        "d.textContent=JSON.stringify({props:{video:{associatedPlaylists:"
        "[{videos:[{id:'mmvo9990001112',mpxMetadata:{id:'9990001112'},"
        "unibrow:{text:'News'}," ASSETS("flood") "},"
        "{id:'mmvo4410023351',mpxMetadata:{id:'4410023351'},"
        ASSETS("own") "}]}]}}});"
        "document.body.insertBefore(d,document.body.firstChild);"
        "globalThis.pocSummary=document.querySelectorAll("
        "'script[type=\"application/ld+json\"]').length===0"
        "?'HYDRATED':'HYDRATE-FAILED'";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, hydrate, "<late-hydrate>", &result)
          && strcmp(result.summary, "HYDRATED") == 0);
    /* The removed declaration is the kept one: the rescan finds none. */
    declared = media_declared_video_cached(&document);
    CHECK(declared != NULL && strcmp(declared->media_url, own_hls) == 0);
    CHECK(media_discover_document_candidates(
              &document, page, NULL, url, sizeof(url), streaming,
              sizeof(streaming), &discovery)
          && strcmp(url, own_hls) == 0
          && discovery.kind == MEDIA_DISCOVERY_HLS);
    script_runtime_destroy(runtime);
    document_destroy(&document);

    /* The same blob with numeric IDs; the page's own clip also offers a
       360p file, which the PSP decodes and so is preferred to its master.
       A related clip's ID never reaches the page's own item, nor the
       page's ID a later related one. */
    static const char numeric_html[] =
        "<!doctype html><body><script type=application/json>"
        "{\"videos\":[{\"id\":9990001112,\"videoAssets\":[{\"publicUrl\":"
        "\"https://cdn.test/v/flood_360.mp4\",\"height\":360}]},"
        "{\"id\":4410023351,\"videoAssets\":["
        "{\"publicUrl\":\"https://cdn.test/own/abs/index.m3u8\"},"
        "{\"publicUrl\":\"https://cdn.test/v/own_1080.mp4\",\"height\":1080},"
        "{\"publicUrl\":\"https://cdn.test/v/own_360.mp4\",\"height\":360}]},"
        "{\"id\":9990001113,\"videoAssets\":[{\"publicUrl\":"
        "\"https://cdn.test/v/court_240.mp4\",\"height\":240}]}]}"
        "</script></body>";
    CHECK(discover_page(&budget, numeric_html, sizeof(numeric_html) - 1u,
                        page, url, sizeof(url), &discovery)
          && strcmp(url, "https://cdn.test/v/own_360.mp4") == 0
          && discovery.kind == MEDIA_DISCOVERY_MP4);
#undef ASSETS
    CHECK(budget.current == 0u);
    return 0;
}

/* A script that opens by assigning its state literal, with a short tail of
   code, is read past the 64 KiB per-script cap (within the shared text
   budget): its player's rendition list sits deep in the literal. Ordinary
   code keeps the cap. */
/* A data script built so that the forward scan reads each quote as real and
   the old embedUrl key check, walking back to the key's opening quote, read
   it as escaped: every literal walked to the start of the text, 3.1 billion
   steps for the 256 KiB the scan admits (1.4 s on a desktop, minutes on a
   PSP, on the main thread when the user presses Play). The check now looks
   one key length back. CPU time is the work counter here: the bound is
   two orders of magnitude above the fixed scan and several times below
   the old one on a host. */
static int test_hostile_key_quotes_scan_bounded(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    budget_install_lexbor(&budget);
    static const char head[] = "<!doctype html><script type=application/json>";
    static const char tail[] = "</script>";
    static const char unit[] = "\\\\\"k\\\\\":'v'";
    const size_t unit_length = sizeof(unit) - 1u;
    const size_t text_length = 256u * 1024u;
    CHECK(unit_length == 11u);
    size_t length = sizeof(head) - 1u + text_length + sizeof(tail) - 1u;
    char *html = malloc(length + 1u);
    CHECK(html != NULL);
    memcpy(html, head, sizeof(head) - 1u);
    for (size_t at = 0; at < text_length; at++)
        html[sizeof(head) - 1u + at] = unit[at % unit_length];
    memcpy(html + sizeof(head) - 1u + text_length, tail, sizeof(tail));
    PocDocument document;
    bool parsed = document_parse(&document, &budget, html, length, 73);
    free(html);
    CHECK(parsed);
    char url[512];
    char streaming[512];
    MediaDiscoveryResult result;
    clock_t started = clock();
    bool found = media_discover_document_candidates(
        &document, "https://site.test/watch", NULL, url, sizeof(url),
        streaming, sizeof(streaming), &result);
    double seconds = (double) (clock() - started) / CLOCKS_PER_SEC;
    document_destroy(&document);
    printf("hostile key-quote data script: %.3f s\n", seconds);
    CHECK(!found);
    CHECK(seconds < 0.25);

    /* The bounded check still recognises the key, quoted either way and in
       any case, and nothing else of its length. */
    static const char declared_html[] =
        "<!doctype html><script type=application/json>"
        "{'EMBEDURL':'/player/a.mp4',\"embedurl\" : \"/player/b.mp4\","
        "\"embedUrx\":\"/clip/c.mp4\"}</script>";
    found = discover_page(&budget, declared_html, sizeof(declared_html) - 1u,
                          "https://site.test/watch", url, sizeof(url),
                          &result);
    CHECK(found && strcmp(url, "/clip/c.mp4") == 0);
    return 0;
}

static int test_large_assignment_script(void)
{
    Budget budget;
    budget_init(&budget, 4u * MIB);
    budget_install_lexbor(&budget);
    char url[512];
    MediaDiscoveryResult result;
    const size_t padding = 100u * 1024u;
    static const char head[] =
        "<!doctype html><body><script>%s{\"common\":{\"pad\":\"";
    static const char tail[] =
        "\"},\"videoData\":{\"bvid\":\"BV1xY\",\"playUrlInfo\":[{\"order\":1,"
        "\"url\":\"https:\\u002F\\u002Fcdn.test\\u002Fupgc\\u002F43094904270"
        "\\u002F43094904270-1-16.mp4?e=ig8\\u0026os=akam\"}]}};"
        "(function(){var s=document.currentScript;"
        "s.parentNode.removeChild(s);}());</script></body>";
    static const char *const openers[] = {
        "window.__INITIAL_STATE__=", "var initialState = ",
        "setState(\"x\",", NULL
    };
    char *html = malloc(padding + 1024u);
    CHECK(html != NULL);
    for (size_t i = 0; i < sizeof(openers) / sizeof(openers[0]); i++) {
        const char *opener = openers[i] == NULL ? "if(a)b();x=" : openers[i];
        int written = snprintf(html, 512u, head, opener);
        CHECK(written > 0);
        size_t length = (size_t) written;
        memset(html + length, 'a', padding);
        length += padding;
        memcpy(html + length, tail, sizeof(tail));
        length += sizeof(tail) - 1u;
        bool found = discover_page(
            &budget, html, length, "https://site.test/video/BV1xY", url,
            sizeof(url), &result);
        if (i < 2u) {
            CHECK(found && strcmp(url,
                "https://cdn.test/upgc/43094904270/43094904270-1-16.mp4"
                "?e=ig8&os=akam") == 0);
        } else {
            /* A call or leading code is not a data assignment. */
            CHECK(!found);
        }
    }
    free(html);
    CHECK(budget.current == 0u);
    return 0;
}

/* One remembered-response scenario: a fresh document whose address is
   page_url, the responses offered in order, then the page's own candidate
   (empty when there is none). */
typedef struct {
    const char *url;
    const char *body;
    size_t length; /* 0: strlen(body) */
} StreamResponse;

static int stream_capture(const char *page_url, bool player_page,
                          const StreamResponse *responses, size_t count,
                          char *own, size_t own_capacity,
                          uint32_t *generation)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    budget_install_lexbor(&budget);
    static const char html[] =
        "<!doctype html><html><head><title>Player</title></head>"
        "<body><video></video></body></html>";
    PocDocument document;
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 7));
    for (size_t i = 0; i < count; i++)
        media_json_response_remember(
            &document, page_url, responses[i].url, 200, responses[i].body,
            responses[i].length != 0u ? responses[i].length
                                      : strlen(responses[i].body),
            player_page);
    *generation = media_json_response_generation(&document);
    MediaDiscoveryKind kind = MEDIA_DISCOVERY_NONE;
    if (!media_json_own_candidate(&document, 0u, own, own_capacity, &kind))
        own[0] = '\0';
    document_destroy(&document);
    CHECK(budget.current == 0u);
    return 0;
}

/* The embed hop's capture: cross-origin readable JSON, bodies past 64 KiB
   (read to 256 KiB), generic stream keys, #EXTM3U bodies; never a
   content type or binary body alone, never a related list, an ad or a
   stand-in clip; and the page's identity decides what is its own. */
static int test_stream_response_capture(void)
{
    static const char page[] =
        "https://player.example.test/play/embed/9ae8e8a6dc58bdad";
    char own[512];
    uint32_t generation = 0;
    /* A cross-origin {"data": ...} answer naming the page's ID. */
    StreamResponse data = {
        "https://api.other.test/v2/options",
        "{\"data\":\"https:\\/\\/cdn.test\\/live\\/9ae8e8a6dc58bdad\\/"
        "index.m3u8?token=a\\u0026e=1\"}"
    };
    CHECK(stream_capture(page, true, &data, 1, own, sizeof(own),
                         &generation) == 0
          && generation == 1u
          && strcmp(own, "https://cdn.test/live/9ae8e8a6dc58bdad/"
                         "index.m3u8?token=a&e=1") == 0);
    /* A 200 KiB body whose stream sits past 64 KiB, under m3u8_url; one
       whose stream sits past 256 KiB is not read that far. */
    static char large[300u * 1024u];
    for (size_t at = 0; at < 2u; at++) {
        size_t pad = at == 0u ? 150u * 1024u : 270u * 1024u;
        int used = snprintf(large, sizeof(large), "{\"pad\":\"");
        memset(large + used, 'x', pad);
        snprintf(large + used + pad, sizeof(large) - used - pad,
                 "\",\"video\":{\"id\":\"9ae8e8a6dc58bdad\",\"m3u8_url\":"
                 "\"https://cdn.test/v/master.m3u8\"}}");
        StreamResponse big = {"https://api.other.test/options", large};
        CHECK(stream_capture(page, true, &big, 1, own, sizeof(own),
                             &generation) == 0
              && (at == 0u
                  ? strcmp(own, "https://cdn.test/v/master.m3u8") == 0
                  : own[0] == '\0' && generation == 0u));
    }
    /* A playlist body: the playlist's own address. */
    StreamResponse playlist = {
        "https://cdn.test/hls/9ae8e8a6dc58bdad/master.m3u8",
        "\xef\xbb\xbf#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=800000,"
        "RESOLUTION=640x360\nlow/index.m3u8\n"
    };
    CHECK(stream_capture(page, true, &playlist, 1, own, sizeof(own),
                         &generation) == 0
          && strcmp(own, playlist.url) == 0);
    /* A bandwidth-test transport stream download: binary, whatever its
       type or address says. */
    static char transport[4096];
    memset(transport, 0, sizeof(transport));
    for (size_t at = 0; at < sizeof(transport); at += 188u)
        transport[at] = 0x47;
    memcpy(transport + 4, "m3u8 mp4", 8);
    StreamResponse measurement = {
        "https://cdn.test/measurements/9ae8e8a6dc58bdad-1555KB.ts",
        transport, sizeof(transport)
    };
    CHECK(stream_capture(page, true, &measurement, 1, own, sizeof(own),
                         &generation) == 0
          && generation == 0u && own[0] == '\0');
    /* Related videos: under a related key, at a related endpoint, or each
       with its own ID beside the page's. */
    StreamResponse related[] = {
        {"https://api.test/videos/9ae8e8a6dc58bdad/related",
         "[{\"id\":\"777777\",\"url\":\"https://cdn.test/777777.mp4\"}]"},
        {"https://api.test/feed",
         "{\"id\":\"9ae8e8a6dc58bdad\",\"recommended\":[{\"mp4\":"
         "\"https://cdn.test/a.mp4\"}],\"items\":[{\"id\":\"888888\","
         "\"file\":\"https://cdn.test/v/888888.mp4\"}]}"}
    };
    CHECK(stream_capture(page, true, related, 2, own, sizeof(own),
                         &generation) == 0
          && generation != 0u && own[0] == '\0');
    /* A list at the page's own endpoint whose items each name another
       video: not the page's, though the address names it. */
    StreamResponse next = {
        "https://api.test/v/9ae8e8a6dc58bdad/next",
        "{\"id\":\"1\",\"items\":[{\"id\":\"555555\",\"hls\":"
        "\"https://cdn.test/555555/index.m3u8\"}]}"
    };
    CHECK(stream_capture(page, true, &next, 1, own, sizeof(own),
                         &generation) == 0
          && generation == 1u && own[0] == '\0');
    /* An ad and an error clip, both beside the page's ID. */
    StreamResponse clips[] = {
        {"https://api.test/v/9ae8e8a6dc58bdad",
         "{\"ad\":{\"src\":\"https://cdn.test/spot.mp4\"},"
         "\"src\":\"https://cdn.test/ads/preroll-2.mp4\"}"},
        {"https://api.test/v/9ae8e8a6dc58bdad/state",
         "{\"src\":\"https://cdn.test/static/player/error.mp4\"}"}
    };
    CHECK(stream_capture(page, true, clips, 2, own, sizeof(own),
                         &generation) == 0
          && generation == 1u && own[0] == '\0');
    /* Identity: a channel page's short last word matches only as a whole
       word, and only for a player page; an object's slug member is
       enough. The own reference outranks an unrelated stream. */
    StreamResponse channels[] = {
        {"https://site.test/api/v2/channels/fixturefan/playback-url",
         "{\"data\":\"https://edge.test/fan.m3u8\"}"},
        {"https://site.test/api/v2/channels/fixture/playback-url",
         "{\"data\":\"https://edge.test/own.m3u8?token=t\"}"}
    };
    CHECK(stream_capture("https://player.site.test/fixture", true, channels, 2,
                         own, sizeof(own), &generation) == 0
          && strcmp(own, "https://edge.test/own.m3u8?token=t") == 0);
    CHECK(stream_capture("https://player.site.test/fixture", false, channels, 2,
                         own, sizeof(own), &generation) == 0
          && generation == 2u && own[0] == '\0');
    StreamResponse info = {
        "https://site.test/api/v2/channels/info",
        "{\"id\":668,\"slug\":\"fixture\",\"livestream\":{\"is_live\":true},"
        "\"playback_url\":\"https:\\/\\/edge.test\\/own.m3u8\"}"
    };
    CHECK(stream_capture("https://player.site.test/fixture", true, &info, 1,
                         own, sizeof(own), &generation) == 0
          && strcmp(own, "https://edge.test/own.m3u8") == 0);
    /* A playback answer for a query-named video: the https file first. */
    StreamResponse playback = {
        "https://edge.api.test/playback/v1/videos/6407513159112",
        "{\"id\":\"6407513159112\",\"sources\":["
        "{\"type\":\"application/x-mpegURL\","
        "\"src\":\"http://m.test/10s/master.m3u8?t=1\"},"
        "{\"height\":360,\"src\":\"http://f.test/main.mp4\"},"
        "{\"height\":360,\"src\":\"https://f.test/main.mp4\"},"
        "{\"src\":\"https://m.test/10s/preview.m3u8\"}]}"
    };
    CHECK(stream_capture(
              "https://players.test/665003/abc_default/index.html"
              "?videoId=6407513159112", true, &playback, 1, own,
              sizeof(own), &generation) == 0
          && strcmp(own, "https://f.test/main.mp4") == 0);
    return 0;
}

static int declared_embed(const char *head, char *embed, size_t capacity,
                          bool *media)
{
    Budget budget;
    budget_init(&budget, 4u * MIB);
    budget_install_lexbor(&budget);
    char html[2048];
    int length = snprintf(html, sizeof(html),
        "<!doctype html><html><head>%s</head><body><p>Story</p></body>"
        "</html>", head);
    CHECK(length > 0 && (size_t) length < sizeof(html));
    PocDocument document;
    CHECK(document_parse(&document, &budget, html, (size_t) length, 9));
    const MediaDeclaredVideo *declared =
        media_declared_video_or_embed_cached(&document);
    *media = media_declared_video_cached(&document) != NULL;
    snprintf(embed, capacity, "%s", declared == NULL ? "" : declared->embed_url);
    document_destroy(&document);
    return 0;
}

/* The declared player page: VideoObject embedUrl first, then an HTML
   og:video, then twitter:player; never direct media. A page declaring only
   a player page is still no declared video, as before. */
static int test_declared_embed_url(void)
{
    char embed[256];
    bool media = false;
    CHECK(declared_embed(
              "<script type=application/ld+json>{\"@type\":\"VideoObject\","
              "\"name\":\"Clip\",\"embedUrl\":\"https:\\/\\/players.test\\/"
              "v\\/index.html?videoId=6407513159112\"}</script>"
              "<meta name=twitter:player content=https://tw.test/p/1>",
              embed, sizeof(embed), &media) == 0
          && !media && strcmp(embed, "https://players.test/v/index.html"
                                     "?videoId=6407513159112") == 0);
    CHECK(declared_embed(
              "<meta property=og:video content=https://site.test/play/embed/9ae8>"
              "<meta property=og:video:type content=text/html>"
              "<meta name=twitter:player content=https://tw.test/p/1>",
              embed, sizeof(embed), &media) == 0
          && !media && strcmp(embed, "https://site.test/play/embed/9ae8") == 0);
    /* An extensionless og:video typed as a video is media, not a page. */
    CHECK(declared_embed(
              "<meta property=og:video content=https://cdn.test/stream/42>"
              "<meta property=og:video:type content=video/mp4>"
              "<meta name=twitter:player content=//tw.test/p/2>",
              embed, sizeof(embed), &media) == 0
          && strcmp(embed, "//tw.test/p/2") == 0);
    /* Direct media keeps the existing declaration, with the page beside it;
       other schemes and media files are never a player page. */
    CHECK(declared_embed(
              "<meta property=og:video content=https://cdn.test/v/clip.mp4>"
              "<meta name=twitter:player content=https://tw.test/p/3>",
              embed, sizeof(embed), &media) == 0
          && media && strcmp(embed, "https://tw.test/p/3") == 0);
    CHECK(declared_embed(
              "<meta name=twitter:player content=javascript:alert(1)>"
              "<meta property=og:video content=https://cdn.test/v/a.m3u8>",
              embed, sizeof(embed), &media) == 0
          && media && embed[0] == '\0');
    CHECK(declared_embed("<meta name=twitter:player content=ftp://x.test/p>",
                         embed, sizeof(embed), &media) == 0
          && !media && embed[0] == '\0');
    return 0;
}

/* Parses html as the server sent it, keeps its declarations as the parser
   does once the head is complete, and starts its realm at page. */
static ScriptRuntime *kept_page(Budget *budget, PocDocument *document,
                                const char *html, const char *page)
{
    if (!document_parse(document, budget, html, strlen(html), 81))
        return NULL;
    media_declared_video_keep_parsed(document);
    ScriptResult result;
    ScriptRuntime *runtime = script_runtime_create(
        document, budget, 12u * MIB, 4000, page, &result);
    if (runtime == NULL || !result.success) {
        if (runtime != NULL) script_runtime_destroy(runtime);
        document_destroy(document);
        return NULL;
    }
    return runtime;
}

static bool kept_run(ScriptRuntime *runtime, const char *source,
                     const char *expected)
{
    ScriptResult result;
    return script_runtime_evaluate_diagnostic(
               runtime, source, "<kept-declarations>", &result)
        && strcmp(result.summary, expected) == 0;
}

/* Client scripts that wipe the head (as hydration does) leave Play the
   server markup's declared video, its player page and the identity that
   ranks the page's own clip; a different declaration a script writes
   wins; moving to another URL forgets the kept one; a page that declares
   nothing keeps nothing. */
static int test_kept_declarations(void)
{
    Budget budget;
    budget_init(&budget, 16u * MIB);
    budget_install_lexbor(&budget);
    /* The page's address names no video; its canonical URL does. */
    static const char page[] = "https://news.test/watch";
    static const char own_hls[] = "https://cdn.test/own/abs/index.m3u8";
    static const char embed[] = "https://players.test/embed/4410023351";
    static const char html[] =
        "<!doctype html><html><head><title>Own story</title>"
        "<link rel=canonical "
        "href='https://news.test/watch/own-story-slug-4410023351'>"
        "<meta name=twitter:player "
        "content='https://players.test/embed/4410023351'>"
        "<script type=application/ld+json>{\"@context\":\"https://schema.org\","
        "\"@type\":\"VideoObject\",\"name\":\"Own story\","
        "\"duration\":\"PT1M5S\","
        "\"url\":\"https://news.test/watch/own-story-slug-4410023351\","
        "\"contentUrl\":\"https://cdn.test/own/abs/index.m3u8\"}</script>"
        "</head><body><div id=app></div></body></html>";
    /* Hydration empties the head and rebuilds it without declarations, and
       a state blob lists a related clip ahead of the page's own one. */
#define ASSETS(slug) \
    "videoAssets:[{format:'M3U',publicUrl:'https://cdn.test/" slug \
    "/abs/index.m3u8',height:1080,width:1920},{format:'MPEG4'," \
    "publicUrl:'https://cdn.test/v/2026/10/17931_" slug "_4500.mp4'," \
    "height:1080,width:1920}]"
    static const char wipe[] =
        "document.head.replaceChildren();"
        "const t=document.createElement('title');t.textContent='Rebuilt';"
        "document.head.appendChild(t);"
        "const m=document.createElement('meta');"
        "m.setAttribute('property','og:site_name');"
        "m.setAttribute('content','News');document.head.appendChild(m);"
        "const d=document.createElement('script');"
        "d.type='application/json';d.id='state';"
        "d.textContent=JSON.stringify({props:{video:{associatedPlaylists:"
        "[{videos:[{id:'mmvo9990001112',mpxMetadata:{id:'9990001112'},"
        ASSETS("flood") "},"
        "{id:'mmvo4410023351',mpxMetadata:{id:'4410023351'},"
        ASSETS("own") "}]}]}}});"
        "document.body.replaceChildren(d);"
        "globalThis.pocSummary='WIPED:'+document.head.children.length";
    char url[512];
    char streaming[512];
    MediaDiscoveryResult discovery;
    PocDocument document;
    ScriptRuntime *runtime = kept_page(&budget, &document, html, page);
    CHECK(runtime != NULL);
    /* Keeping is not a lookup. */
    CHECK(media_declared_video_discovery_count(&document) == 0u);
    CHECK(kept_run(runtime, wipe, "WIPED:2"));
    const MediaDeclaredVideo *declared = media_declared_video_cached(&document);
    CHECK(declared != NULL && strcmp(declared->media_url, own_hls) == 0
          && declared->structured && declared->kind == MEDIA_DISCOVERY_HLS
          && strcmp(declared->embed_url, embed) == 0
          && strcmp(declared->title, "Own story") == 0
          && declared->duration[0] != '\0');
    CHECK(media_declared_video_or_embed_cached(&document) == declared);
    CHECK(media_discover_document_candidates(
              &document, page, NULL, url, sizeof(url), streaming,
              sizeof(streaming), &discovery)
          && strcmp(url, own_hls) == 0);
    /* A same-URL replaceState (routers do it at startup) keeps it. */
    CHECK(kept_run(runtime,
                   "history.replaceState({r:1},'',location.href);"
                   "document.body.appendChild(document.createElement('p'));"
                   "globalThis.pocSummary='SAME'", "SAME"));
    declared = media_declared_video_cached(&document);
    CHECK(declared != NULL && strcmp(declared->media_url, own_hls) == 0);
    /* pushState to another article: neither the declaration nor the old
       identity is offered; the blob's first clip is no longer outranked. */
    CHECK(kept_run(runtime,
                   "history.pushState({r:2},'','/watch/next');"
                   "globalThis.pocSummary=location.pathname", "/watch/next"));
    CHECK(media_declared_video_cached(&document) == NULL
          && media_declared_video_or_embed_cached(&document) == NULL);
    CHECK(media_discover_document_candidates(
              &document, "https://news.test/watch/next", NULL, url,
              sizeof(url), streaming, sizeof(streaming), &discovery)
          && strcmp(url, own_hls) != 0);
    script_runtime_destroy(runtime);
    document_destroy(&document);
#undef ASSETS

    /* The head wiped by removing the element itself, before any lookup. */
    runtime = kept_page(&budget, &document, html, page);
    CHECK(runtime != NULL);
    CHECK(kept_run(runtime,
                   "document.head.remove();"
                   "globalThis.pocSummary=String(document.querySelectorAll("
                   "'link,meta,script').length)", "0"));
    declared = media_declared_video_cached(&document);
    CHECK(declared != NULL && strcmp(declared->media_url, own_hls) == 0);
    script_runtime_destroy(runtime);
    document_destroy(&document);

    /* A script replaces the declaration: the new one wins, and is then the
       kept one. */
    static const char other_hls[] = "https://cdn.test/other/abs/index.m3u8";
    runtime = kept_page(&budget, &document, html, page);
    CHECK(runtime != NULL);
    CHECK(kept_run(runtime,
                   "for(const e of document.head.querySelectorAll("
                   "'script,meta'))e.remove();"
                   "const s=document.createElement('script');"
                   "s.type='application/ld+json';"
                   "s.textContent=JSON.stringify({'@type':'VideoObject',"
                   "name:'Other story',"
                   "contentUrl:'https://cdn.test/other/abs/index.m3u8'});"
                   "document.head.appendChild(s);"
                   "globalThis.pocSummary='REPLACED'", "REPLACED"));
    declared = media_declared_video_cached(&document);
    CHECK(declared != NULL && strcmp(declared->media_url, other_hls) == 0
          && strcmp(declared->title, "Other story") == 0
          && declared->embed_url[0] == '\0');
    CHECK(kept_run(runtime,
                   "document.head.replaceChildren();"
                   "globalThis.pocSummary='GONE'", "GONE"));
    declared = media_declared_video_cached(&document);
    CHECK(declared != NULL && strcmp(declared->media_url, other_hls) == 0);
    script_runtime_destroy(runtime);
    document_destroy(&document);

    /* Only part removed (the VideoObject goes, the same player page stays):
       the kept declaration, which has both, still answers. */
    runtime = kept_page(&budget, &document, html, page);
    CHECK(runtime != NULL);
    CHECK(kept_run(runtime,
                   "document.head.querySelector('script').remove();"
                   "globalThis.pocSummary='PART'", "PART"));
    declared = media_declared_video_cached(&document);
    CHECK(declared != NULL && strcmp(declared->media_url, own_hls) == 0
          && strcmp(declared->embed_url, embed) == 0);
    script_runtime_destroy(runtime);
    document_destroy(&document);

    /* A page that declares only its player page keeps that. */
    static const char embed_only[] =
        "<!doctype html><html><head>"
        "<meta name=twitter:player "
        "content='https://players.test/embed/4410023351'>"
        "</head><body></body></html>";
    runtime = kept_page(&budget, &document, embed_only, page);
    CHECK(runtime != NULL);
    CHECK(kept_run(runtime, "document.head.replaceChildren();"
                   "globalThis.pocSummary='E'", "E"));
    CHECK(media_declared_video_cached(&document) == NULL);
    declared = media_declared_video_or_embed_cached(&document);
    CHECK(declared != NULL && strcmp(declared->embed_url, embed) == 0
          && declared->media_url[0] == '\0');
    script_runtime_destroy(runtime);
    document_destroy(&document);

    /* Nothing declared: nothing kept (no allocation), nothing offered. */
    static const char none[] =
        "<!doctype html><html><head><title>Plain</title>"
        "<link rel=canonical href='https://news.test/a/plain-story-4410023351'>"
        "<meta property=og:title content=Plain>"
        "<script type=application/ld+json>{\"@type\":\"NewsArticle\","
        "\"headline\":\"Plain\"}</script>"
        "</head><body><p>Text</p></body></html>";
    CHECK(document_parse(&document, &budget, none, sizeof(none) - 1u, 81));
    size_t before = budget.current;
    media_declared_video_keep_parsed(&document);
    CHECK(budget.current == before && document.declared_video_cache == NULL);
    ScriptResult result;
    runtime = script_runtime_create(
        &document, &budget, 12u * MIB, 4000, page, &result);
    CHECK(runtime != NULL && result.success);
    CHECK(kept_run(runtime, "document.head.replaceChildren();"
                   "globalThis.pocSummary='N'", "N"));
    CHECK(media_declared_video_cached(&document) == NULL
          && media_declared_video_or_embed_cached(&document) == NULL);
    script_runtime_destroy(runtime);
    document_destroy(&document);
    CHECK(budget.current == 0u);
    return 0;
}

int main(void)
{
    CHECK(test_kept_declarations() == 0);
    CHECK(test_declared_embed_url() == 0);
    CHECK(test_stream_response_capture() == 0);
    CHECK(test_promo_and_extensionless_video() == 0);
    CHECK(test_late_related_clips() == 0);
    CHECK(test_large_assignment_script() == 0);
    CHECK(test_hostile_key_quotes_scan_bounded() == 0);
    CHECK(test_quality_hint_ignores_id_fragments() == 0);
    CHECK(test_activation_affinity_signals() == 0);
    CHECK(test_structured_scan_parity() == 0);
    CHECK(test_executable_script_references() == 0);
    CHECK(test_script_key_quality_words() == 0);
    Budget budget;
    budget_init(&budget, 14u * MIB);
    budget_install_lexbor(&budget);
    static const char html[] =
        "<!doctype html><html><body>"
        "<video id=media crossorigin=anonymous></video></body></html>";
    PocDocument document;
    CHECK(document_parse(
        &document, &budget, html, sizeof(html) - 1u, 17));
    ScriptResult result;
    ScriptRuntime *runtime = script_runtime_create(
        &document, &budget, 12u * MIB, 4000,
        "https://example.test/watch", &result);
    CHECK(runtime != NULL && result.success);
    static const char start[] =
        "const media=document.querySelector('#media'),events=[];"
        "for(const event of ['play','playing','pause','waiting',"
        "'loadedmetadata','timeupdate','seeked','ended','error'])"
        "media.addEventListener(event,()=>events.push(event));"
        "media.src='https://media.example.test/movie.mp4';"
        "media.setAttribute('title','Night radio \\u00e9');"
        "globalThis.mediaProbe=media;globalThis.mediaEvents=events;"
        "media.play();globalThis.pocSummary=media.paused"
        "?'MEDIA-DOM-FAILED':'MEDIA-DOM-QUEUED'";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, start, "<media-dom-start>", &result)
          && strcmp(result.summary, "MEDIA-DOM-QUEUED") == 0);
    ScriptMediaRequest request;
    CHECK(script_runtime_consume_media_request(runtime, &request)
          && request.command == SCRIPT_MEDIA_COMMAND_PLAY
          && request.mode == TILEFINCH_REQUEST_MODE_CORS
          && request.credentials == TILEFINCH_CREDENTIALS_SAME_ORIGIN
          && strcmp(request.source,
                    "https://media.example.test/movie.mp4") == 0
          /* The element's own title names what plays. */
          && strcmp(request.title, "Night radio \xc3\xa9") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.mediaHijacked=false;"
              "globalThis.__tilefinchMediaUpdate=()=>{"
              "globalThis.mediaHijacked=true;return true}",
              "<media-dom-host-hook-tamper>", &result));
    CHECK(script_runtime_update_media_state(
        runtime, request.node_handle, SCRIPT_MEDIA_STATE_LOADING,
        0, 30.0));
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.pocSummary=!mediaProbe.paused"
              "&&mediaEvents.filter(x=>x==='pause').length===0"
              "?'MEDIA-LOADING-OK':'MEDIA-LOADING-FAILED'",
              "<media-dom-loading-verify>", &result)
          && strcmp(result.summary, "MEDIA-LOADING-OK") == 0);
    CHECK(script_runtime_update_media_state(
        runtime, request.node_handle, SCRIPT_MEDIA_STATE_PLAYING,
        2.5, 30.0));
    CHECK(script_runtime_update_media_state(
        runtime, request.node_handle, SCRIPT_MEDIA_STATE_PLAYING,
        2.75, 30.0));
    static const char verify[] =
        "globalThis.pocSummary=mediaProbe.readyState===2"
        "&&mediaProbe.networkState===1&&!mediaProbe.paused"
        "&&mediaProbe.currentTime===2.75&&mediaProbe.duration===30"
        "&&!globalThis.mediaHijacked"
        "&&mediaEvents.includes('loadedmetadata')"
        "&&mediaEvents.filter(x=>x==='playing').length===1"
        "&&mediaEvents.filter(x=>x==='seeked').length===0"
        "?'MEDIA-DOM-OK':'MEDIA-DOM-FAILED'";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, verify, "<media-dom-verify>", &result)
          && strcmp(result.summary, "MEDIA-DOM-OK") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, "mediaProbe.currentTime=3",
              "<media-dom-seek>", &result));
    CHECK(script_runtime_consume_media_request(runtime, &request)
          && request.command == SCRIPT_MEDIA_COMMAND_SEEK);
    CHECK(script_runtime_update_media_state(
        runtime, request.node_handle, SCRIPT_MEDIA_STATE_PLAYING, 3, 30));
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.pocSummary=mediaEvents.filter(x=>x==='seeked')"
              ".length===1?'MEDIA-SEEK-OK':'MEDIA-SEEK-FAILED'",
              "<media-dom-seek-verify>", &result)
          && strcmp(result.summary, "MEDIA-SEEK-OK") == 0);
    CHECK(script_runtime_update_media_state(
        runtime, request.node_handle, SCRIPT_MEDIA_STATE_LOADING, 3, 30));
    CHECK(script_runtime_update_media_state(
        runtime, request.node_handle, SCRIPT_MEDIA_STATE_PLAYING, 3.25, 30));
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.pocSummary=mediaEvents.filter(x=>x==='waiting')"
              ".length===2&&mediaEvents.filter(x=>x==='playing').length===2"
              "?'MEDIA-BUFFER-OK':'MEDIA-BUFFER-FAILED'",
              "<media-dom-buffer-verify>", &result)
          && strcmp(result.summary, "MEDIA-BUFFER-OK") == 0);
    CHECK(script_runtime_update_media_state(
        runtime, request.node_handle, SCRIPT_MEDIA_STATE_ENDED, 30, 30));
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.pocSummary=mediaEvents.filter(x=>x==='ended')"
              ".length===1?'MEDIA-ENDED-OK':'MEDIA-ENDED-FAILED'",
              "<media-dom-ended-verify>", &result)
          && strcmp(result.summary, "MEDIA-ENDED-OK") == 0);
    script_runtime_destroy(runtime);
    document_destroy(&document);

    static const char audio_html[] =
        "<!doctype html><html><body>"
        "<audio id=audio><source src='/episode.m4a' type='audio/mp4'>"
        "</audio></body></html>";
    PocDocument audio_document;
    CHECK(document_parse(
        &audio_document, &budget,
        audio_html, sizeof(audio_html) - 1u, 19));
    ScriptRuntime *audio_runtime = script_runtime_create(
        &audio_document, &budget, 12u * MIB, 4000,
        "https://example.test/listen", &result);
    CHECK(audio_runtime != NULL && result.success);
    CHECK(script_runtime_evaluate_diagnostic(
              audio_runtime,
              "const a=document.querySelector('#audio');a.play();"
              "globalThis.pocSummary=a.canPlayType('audio/mp4')==='maybe'"
              "&&a.currentSrc==='https://example.test/episode.m4a'"
              "?'AUDIO-DOM-OK':'AUDIO-DOM-FAILED'",
              "<audio-dom-start>", &result)
          && strcmp(result.summary, "AUDIO-DOM-OK") == 0);
    CHECK(script_runtime_consume_media_request(audio_runtime, &request)
          && request.command == SCRIPT_MEDIA_COMMAND_PLAY
          && request.audio_only
          && strcmp(request.source,
                    "https://example.test/episode.m4a") == 0);
    CHECK(script_runtime_update_media_state(
        audio_runtime, request.node_handle,
        SCRIPT_MEDIA_STATE_PLAYING, 1.5, 30.0));
    CHECK(script_runtime_evaluate_diagnostic(
              audio_runtime,
              "globalThis.pocSummary=!document.querySelector('#audio').paused"
              "&&document.querySelector('#audio').currentTime===1.5"
              "&&document.querySelector('#audio').duration===30"
              "?'AUDIO-PLAYING-OK':'AUDIO-PLAYING-FAILED'",
              "<audio-dom-playing>", &result)
          && strcmp(result.summary, "AUDIO-PLAYING-OK") == 0);
    script_runtime_destroy(audio_runtime);
    document_destroy(&audio_document);

    /* Native control activation does not call HTMLMediaElement.play(). Its
       first state report must nevertheless attach to the same private media
       state record, and the temporary bootstrap bridge must not be exposed
       to page script. */
    static const char native_html[] =
        "<!doctype html><html><body>"
        "<video id=native src='https://media.example.test/native.mp4'>"
        "</video></body></html>";
    PocDocument native_document;
    CHECK(document_parse(
        &native_document, &budget, native_html, sizeof(native_html) - 1u, 18));
    ScriptRuntime *native_runtime = script_runtime_create(
        &native_document, &budget, 12u * MIB, 4000,
        "https://example.test/native", &result);
    CHECK(native_runtime != NULL && result.success);
    lxb_dom_node_t *native_node = find_element_by_id(
        lxb_dom_interface_node(native_document.html), "native");
    long native_handle = script_runtime_node_handle(
        native_runtime, native_node);
    CHECK(native_handle > 0);
    CHECK(script_runtime_update_media_state(
        native_runtime, native_handle, SCRIPT_MEDIA_STATE_LOADING, 0, 0));
    CHECK(script_runtime_update_media_state(
        native_runtime, native_handle, SCRIPT_MEDIA_STATE_PLAYING, 1.25, 9));
    CHECK(script_runtime_evaluate_diagnostic(
              native_runtime,
              "const n=document.querySelector('#native');"
              "globalThis.pocSummary=n.paused===false&&n.currentTime===1.25"
              "&&n.duration===9"
              "&&typeof globalThis.__tilefinchMediaStateFor==='undefined'"
              "?'MEDIA-NATIVE-OK':'MEDIA-NATIVE-FAILED'",
              "<media-native-verify>", &result)
          && strcmp(result.summary, "MEDIA-NATIVE-OK") == 0);
    CHECK(script_runtime_update_media_state(
        native_runtime, native_handle, SCRIPT_MEDIA_STATE_ENDED, 9, 9));
    script_runtime_destroy(native_runtime);
    document_destroy(&native_document);

    char bounded_html[8192];
    size_t used = (size_t) snprintf(
        bounded_html, sizeof(bounded_html),
        "<!doctype html><script type=application/ld+json>[");
    for (size_t i = 0; i < 13u; i++) {
        int written = snprintf(
            bounded_html + used, sizeof(bounded_html) - used,
            "%s{\"@type\":\"AudioObject\",\"name\":\"Track %zu\","
            "\"contentUrl\":\"/audio/%zu.m4a\"}",
            i == 0 ? "" : ",", i, i);
        CHECK(written > 0 && (size_t) written < sizeof(bounded_html) - used);
        used += (size_t) written;
    }
    int tail = snprintf(
        bounded_html + used, sizeof(bounded_html) - used,
        "]</script><script type=application/ld+json>"
        "{\"@type\":\"AudioObject\",broken</script>");
    CHECK(tail > 0 && (size_t) tail < sizeof(bounded_html) - used);
    used += (size_t) tail;
    PocDocument bounded_document;
    CHECK(document_parse(
        &bounded_document, &budget, bounded_html, used, 29));
    MediaStructuredAudioIndex structured = {0};
    CHECK(media_discover_structured_audio(&bounded_document, &structured)
          && structured.candidate_count
                 == MEDIA_STRUCTURED_AUDIO_CANDIDATE_LIMIT
          && structured.candidate_overflow == 1u
          && structured.malformed_scripts == 1u);
    char copied[128];
    CHECK(media_structured_audio_copy_url(
              &structured.candidates[0], copied, sizeof(copied))
          && strcmp(copied, "/audio/0.m4a") == 0);
    document_destroy(&bounded_document);

    static const char video_declaration_html[] =
        "<!doctype html><html><head>"
        "<meta property=og:video content='/loose-360.mp4'>"
        "<script type=application/ld+json>"
        "{\"@context\":\"https://schema.org\","
        "\"@type\":\"VideoObject\",\"name\":\"Fixture movie\","
        "\"thumbnailUrl\":\"/poster.jpg?exact=1\","
        "\"duration\":\"PT1M30S\","
        "\"embedUrl\":\"/player/embed.mp4\","
        "\"contentUrl\":\"/structured-240.mp4\"}"
        "</script></head><body>Watch</body></html>";
    PocDocument video_declaration_document;
    CHECK(document_parse(
        &video_declaration_document, &budget,
        video_declaration_html, sizeof(video_declaration_html) - 1u, 35));
    MediaStructuredVideoIndex videos = {0};
    CHECK(media_discover_structured_video(
              &video_declaration_document, &videos)
          && videos.candidate_count == 1u);
    CHECK(media_structured_video_copy_url(
              &videos.candidates[0], copied, sizeof(copied))
          && strcmp(copied, "/structured-240.mp4") == 0);
    CHECK(media_structured_video_copy_name(
              &videos.candidates[0], copied, sizeof(copied))
          && strcmp(copied, "Fixture movie") == 0);
    CHECK(media_structured_video_copy_thumbnail(
              &videos.candidates[0], copied, sizeof(copied))
          && strcmp(copied, "/poster.jpg?exact=1") == 0);
    CHECK(media_structured_video_copy_duration(
              &videos.candidates[0], copied, sizeof(copied))
          && strcmp(copied, "PT1M30S") == 0);
    MediaDeclaredVideo declared = {0};
    CHECK(media_discover_declared_video(
              &video_declaration_document, &declared)
          && declared.structured
          && strcmp(declared.media_url, "/structured-240.mp4") == 0
          && strcmp(declared.thumbnail_url,
                    "/poster.jpg?exact=1") == 0
          && strcmp(declared.title, "Fixture movie") == 0);
    MediaDiscoveryResult discovery = {0};
    CHECK(media_discover_document_candidate(
              &video_declaration_document, copied, sizeof(copied),
              &discovery)
          && strcmp(copied, "/structured-240.mp4") == 0);
    document_destroy(&video_declaration_document);

    static const char embed_only_html[] =
        "<!doctype html><script type=application/ld+json>"
        "{\"@context\":\"https://schema.org\","
        "\"@type\":\"VideoObject\","
        "\"embedUrl\":\"/player/embed.mp4\"}</script>";
    PocDocument embed_only_document;
    CHECK(document_parse(
        &embed_only_document, &budget,
        embed_only_html, sizeof(embed_only_html) - 1u, 37));
    memset(&videos, 0, sizeof(videos));
    CHECK(!media_discover_structured_video(&embed_only_document, &videos)
          && videos.candidate_count == 0u);
    memset(&declared, 0, sizeof(declared));
    CHECK(!media_discover_declared_video(&embed_only_document, &declared));
    CHECK(!media_discover_document_candidate(
        &embed_only_document, copied, sizeof(copied), &discovery));
    document_destroy(&embed_only_document);

    static const char video_quality_html[] =
        "<!doctype html><script type=application/ld+json>["
        "{\"@type\":\"VideoObject\","
        "\"contentUrl\":\"/feature-1080.mp4\"},"
        "{\"@type\":\"VideoObject\","
        "\"contentUrl\":\"/feature-240.mp4\"},"
        "{\"@type\":\"VideoObject\","
        "\"contentUrl\":\"/feature-live.m3u8\"}]</script>";
    PocDocument video_quality_document;
    CHECK(document_parse(
              &video_quality_document, &budget,
              video_quality_html, sizeof(video_quality_html) - 1u, 39)
          && media_discover_declared_video(
              &video_quality_document, &declared)
          && declared.kind == MEDIA_DISCOVERY_MP4
          && declared.quality == 240u
          && strcmp(declared.media_url, "/feature-240.mp4") == 0);
    document_destroy(&video_quality_document);

    static const char hls_declaration_html[] =
        "<!doctype html><meta property=og:video "
        "content='https://media.example.test/live.m3u8'>";
    PocDocument hls_declaration_document;
    CHECK(document_parse(
              &hls_declaration_document, &budget,
              hls_declaration_html, sizeof(hls_declaration_html) - 1u, 41)
          && media_discover_declared_video(
              &hls_declaration_document, &declared)
          && declared.kind == MEDIA_DISCOVERY_HLS
          && strcmp(declared.media_url,
                    "https://media.example.test/live.m3u8") == 0);
    document_destroy(&hls_declaration_document);

    static const char data_attribute_html[] =
        "<!doctype html><body><div data-src='/lazy-240.mp4'></div></body>";
    PocDocument data_attribute_document;
    CHECK(document_parse(
              &data_attribute_document, &budget,
              data_attribute_html, sizeof(data_attribute_html) - 1u, 42)
          && !media_discover_declared_video(
              &data_attribute_document, &declared)
          && media_discover_document_candidate(
              &data_attribute_document, copied, sizeof(copied), &discovery)
          && strcmp(copied, "/lazy-240.mp4") == 0);
    document_destroy(&data_attribute_document);

    static const char transactional_candidate_html[] =
        "<!doctype html><script type=application/ld+json>["
        "{\"@type\":\"VideoObject\","
        "\"contentUrl\":\"/retained-1080.mp4\"},"
        "{\"@type\":\"VideoObject\","
        "\"contentUrl\":\"/corrupt-\\u0041-240.mp4\"}]</script>";
    PocDocument transactional_candidate_document;
    CHECK(document_parse(
              &transactional_candidate_document, &budget,
              transactional_candidate_html,
              sizeof(transactional_candidate_html) - 1u, 42)
          && media_discover_declared_video(
              &transactional_candidate_document, &declared)
          && strcmp(declared.media_url, "/retained-1080.mp4") == 0
          && media_discover_document_candidate(
              &transactional_candidate_document,
              copied, sizeof(copied), &discovery)
          && strcmp(copied, "/retained-1080.mp4") == 0);
    document_destroy(&transactional_candidate_document);

    static const char false_og_type_html[] =
        "<!doctype html><meta property=og:type content=videobad>"
        "<meta name=download content='/not-a-declared-watch-240.mp4'>";
    PocDocument false_og_type_document;
    CHECK(document_parse(
              &false_og_type_document, &budget,
              false_og_type_html, sizeof(false_og_type_html) - 1u, 43)
          && !media_discover_declared_video(
              &false_og_type_document, &declared));
    document_destroy(&false_og_type_document);

    static const char large_prefix[] =
        "<!doctype html><script type=application/ld+json>";
    static const char large_suffix[] =
        "{\"@type\":\"AudioObject\","
        "\"contentUrl\":\"/past-limit.m4a\"}</script>";
    size_t large_padding = 256u * 1024u;
    size_t large_length = sizeof(large_prefix) - 1u + large_padding
        + sizeof(large_suffix) - 1u;
    char *large_html = malloc(large_length);
    CHECK(large_html != NULL);
    memcpy(large_html, large_prefix, sizeof(large_prefix) - 1u);
    memset(large_html + sizeof(large_prefix) - 1u, ' ', large_padding);
    memcpy(large_html + sizeof(large_prefix) - 1u + large_padding,
           large_suffix, sizeof(large_suffix) - 1u);
    PocDocument large_document;
    CHECK(document_parse(
        &large_document, &budget, large_html, large_length, 31));
    memset(&structured, 0, sizeof(structured));
    CHECK(!media_discover_structured_audio(&large_document, &structured)
          && structured.inspected_bytes == 256u * 1024u
          && structured.truncated_scripts == 1u
          && structured.malformed_scripts == 0u
          && structured.candidate_count == 0u);
    CHECK(!media_discover_structured_video(&large_document, &videos)
          && videos.inspected_bytes == structured.inspected_bytes
          && videos.truncated_scripts == structured.truncated_scripts
          && videos.malformed_scripts == structured.malformed_scripts
          && videos.candidate_count == 0u);
    document_destroy(&large_document);
    free(large_html);
    CHECK(budget.current == 0);
    puts("media-dom-tests: all checks passed");
    return 0;
}
