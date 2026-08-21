#include <stdio.h>
#include <string.h>

#include "tilefinch/budget.h"
#include "tilefinch/document.h"
#include "tilefinch/js_runtime.h"

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
    CHECK(budget.current == 0);
    puts("media-dom-tests: all checks passed");
    return 0;
}
