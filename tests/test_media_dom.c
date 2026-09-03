#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int main(void)
{
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
                    "https://media.example.test/movie.mp4") == 0);
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
    document_destroy(&large_document);
    free(large_html);
    CHECK(budget.current == 0);
    puts("media-dom-tests: all checks passed");
    return 0;
}
