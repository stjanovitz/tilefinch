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

int main(void)
{
    Budget budget;
    budget_init(&budget, 14u * MIB);
    budget_install_lexbor(&budget);
    static const char html[] =
        "<!doctype html><html><body><video id=media></video></body></html>";
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
        "for(const event of ['play','playing','loadedmetadata','timeupdate'])"
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
          && strcmp(request.source,
                    "https://media.example.test/movie.mp4") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.mediaHijacked=false;"
              "globalThis.__tilefinchMediaUpdate=()=>{"
              "globalThis.mediaHijacked=true;return true}",
              "<media-dom-host-hook-tamper>", &result));
    CHECK(script_runtime_update_media_state(
        runtime, request.node_handle, SCRIPT_MEDIA_STATE_PLAYING,
        2.5, 30.0));
    static const char verify[] =
        "globalThis.pocSummary=mediaProbe.readyState===2"
        "&&mediaProbe.networkState===1&&!mediaProbe.paused"
        "&&mediaProbe.currentTime===2.5&&mediaProbe.duration===30"
        "&&!globalThis.mediaHijacked"
        "&&mediaEvents.includes('loadedmetadata')"
        "&&mediaEvents.includes('playing')"
        "?'MEDIA-DOM-OK':'MEDIA-DOM-FAILED'";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, verify, "<media-dom-verify>", &result)
          && strcmp(result.summary, "MEDIA-DOM-OK") == 0);
    CHECK(script_runtime_update_media_state(
        runtime, request.node_handle, SCRIPT_MEDIA_STATE_ERROR, 0, 0));
    script_runtime_destroy(runtime);
    document_destroy(&document);
    CHECK(budget.current == 0);
    puts("media-dom-tests: all checks passed");
    return 0;
}
