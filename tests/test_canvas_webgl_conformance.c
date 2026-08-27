#include "tilefinch/budget.h"
#include "tilefinch/budget_quickjs.h"
#include "tilefinch/browser_engine.h"
#include "tilefinch/document.h"
#include "tilefinch/gamepad.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/offline_library.h"
#include "tilefinch/resources.h"
#include "tilefinch/viewport.h"
#include "tilefinch/web_app_manifest.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/js_runtime_internal.h"

#define MIB (1024u * 1024u)

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "check failed at %s:%d: %s\n",                     \
                __FILE__, __LINE__, #condition);                             \
        return false;                                                        \
    }                                                                        \
} while (0)

static char *read_source(const char *relative_path, size_t *length)
{
    char path[1024];
    int written = snprintf(
        path, sizeof(path), "%s/%s", TILEFINCH_TEST_SOURCE_DIR,
        relative_path);
    if (written < 0 || (size_t) written >= sizeof(path)) return NULL;
    FILE *file = fopen(path, "rb");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
        if (file != NULL) fclose(file);
        return NULL;
    }
    long end = ftell(file);
    if (end < 0 || end > 256 * 1024 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    char *source = malloc((size_t) end + 1u);
    if (source == NULL) {
        fclose(file);
        return NULL;
    }
    size_t read = fread(source, 1, (size_t) end, file);
    bool ok = read == (size_t) end && ferror(file) == 0;
    fclose(file);
    if (!ok) {
        free(source);
        return NULL;
    }
    source[read] = '\0';
    *length = read;
    return source;
}

static char *read_fixture(const char *name, size_t *length)
{
    char relative[256];
    int written = snprintf(
        relative, sizeof(relative),
        "tests/fixtures/canvas-webgl-games/%s", name);
    return written < 0 || (size_t) written >= sizeof(relative)
        ? NULL : read_source(relative, length);
}

static lxb_dom_node_t *find_element_id(
    lxb_dom_node_t *node, const char *wanted)
{
    for (lxb_dom_node_t *at = node; at != NULL; at = at->next) {
        size_t length = 0;
        const char *id = document_attribute(at, "id", &length);
        if (id != NULL && strlen(wanted) == length
            && memcmp(id, wanted, length) == 0) return at;
        lxb_dom_node_t *nested = find_element_id(at->first_child, wanted);
        if (nested != NULL) return nested;
    }
    return NULL;
}

static bool run_webgl_budget_refusal_ownership(void)
{
    static const char html[] =
        "<!doctype html><html><body><canvas id=probe></canvas></body></html>";
    Budget budget;
    budget_init(&budget, 4u * MIB);
    budget_install_lexbor(&budget);
    PocDocument document;
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    lxb_dom_node_t *canvas = find_element_id(
        lxb_dom_interface_node(document.html), "probe");
    CHECK(canvas != NULL);
    ImageResources images = {.budget = &budget};
    size_t baseline = budget.current;
    unsigned char *pixels = NULL;
    budget_inject_failure_after(&budget, 0);
    CHECK(images_prepare_canvas_surface(
              &images, &budget, canvas, 8, 8, &pixels)
              == IMAGE_CANVAS_COMMIT_REFUSED
          && pixels == NULL && budget.current == baseline
          && images.count == 0);
    budget_clear_failure_injection(&budget);
    CHECK(images_prepare_canvas_surface(
              &images, &budget, canvas, 8, 8, &pixels)
          == IMAGE_CANVAS_COMMIT_CREATED && pixels != NULL);
    size_t color_owned = budget.current;
    uint16_t *depth = NULL;
    budget_inject_failure_after(&budget, 0);
    CHECK(!images_prepare_canvas_depth(
              &images, &budget, canvas, 8, 8, &depth)
          && depth == NULL && budget.current == color_owned
          && images.canvas_depth[0].values == NULL
          && images.canvas_depth[1].values == NULL);
    budget_clear_failure_injection(&budget);
    CHECK(images_release_canvas(&images, &budget, canvas)
          && images.count == 0);
    images_destroy(&images);
    CHECK(budget.current == baseline);
    document_destroy(&document);
    CHECK(budget.current == 0
          && budget_active_allocations(&budget, NULL) == 0);
    return true;
}

static bool run_webgl_cache_epoch_admission(void)
{
    DomBridge realm_a = {0}, realm_b = {0};
    CHECK(js_webgl_realm_epoch_advance(&realm_a));
    CHECK(js_webgl_realm_epoch_advance(&realm_b));
    CHECK(realm_a.webgl_realm_epoch_high != realm_b.webgl_realm_epoch_high
          || realm_a.webgl_realm_epoch_low != realm_b.webgl_realm_epoch_low);

    ScriptWebglCacheAdmission cache = {0};
    bool reset = false;
    const int64_t repeated_canvas_handle = INT64_C(0x0000000100000001);
    uint32_t retained_pixel = 0;
    const uint32_t realm_a_pixel = UINT32_C(0xff112233);
    const uint32_t realm_b_pixel = UINT32_C(0xffaabbcc);
    CHECK(script_runtime_webgl_cache_admit(
        &cache, realm_a.webgl_realm_epoch_high,
        realm_a.webgl_realm_epoch_low, repeated_canvas_handle, &reset));
    CHECK(reset && cache.cached_entries == 0 && cache.cached_bytes == 0);
    retained_pixel = realm_a_pixel;
    cache.cached_entries = 1;
    cache.cached_bytes = sizeof(retained_pixel);

    CHECK(script_runtime_webgl_cache_admit(
        &cache, realm_a.webgl_realm_epoch_high,
        realm_a.webgl_realm_epoch_low, repeated_canvas_handle, &reset));
    CHECK(!reset && retained_pixel == realm_a_pixel);
    CHECK(script_runtime_webgl_cache_admit(
        &cache, realm_a.webgl_realm_epoch_high,
        realm_a.webgl_realm_epoch_low, repeated_canvas_handle + 1, &reset));
    CHECK(reset && cache.cached_entries == 0 && cache.cached_bytes == 0);
    CHECK(script_runtime_webgl_cache_admit(
        &cache, realm_a.webgl_realm_epoch_high,
        realm_a.webgl_realm_epoch_low, repeated_canvas_handle, &reset));
    CHECK(reset);
    cache.cached_entries = 1;
    cache.cached_bytes = sizeof(retained_pixel);

    /* Realm B deliberately reuses every page-controlled texture key.  The
       native epoch must force the upload before those keys are consulted. */
    CHECK(script_runtime_webgl_cache_admit(
        &cache, realm_b.webgl_realm_epoch_high,
        realm_b.webgl_realm_epoch_low, repeated_canvas_handle, &reset));
    CHECK(reset && cache.cached_entries == 0 && cache.cached_bytes == 0);
    retained_pixel = realm_b_pixel;
    cache.cached_entries = 1;
    cache.cached_bytes = sizeof(retained_pixel);
    CHECK(retained_pixel == realm_b_pixel);

    uint32_t prior_high = realm_b.webgl_realm_epoch_high;
    uint32_t prior_low = realm_b.webgl_realm_epoch_low;
    CHECK(js_webgl_realm_epoch_advance(&realm_b));
    CHECK(prior_high != realm_b.webgl_realm_epoch_high
          || prior_low != realm_b.webgl_realm_epoch_low);
    CHECK(script_runtime_webgl_cache_admit(
        &cache, realm_b.webgl_realm_epoch_high,
        realm_b.webgl_realm_epoch_low, repeated_canvas_handle, &reset));
    CHECK(reset);
    CHECK(!script_runtime_webgl_cache_admit(
        &cache, 0u, 0u, repeated_canvas_handle, &reset));
    return true;
}

static bool observe_canvas_game_frames(ScriptRuntime *runtime,
                                       ScriptResult *result,
                                       unsigned *frames)
{
    if (!script_runtime_evaluate_diagnostic(
            runtime,
            "globalThis.pocSummary=String("
            "globalThis.__canvasGameState?.frames"
            "??globalThis.__webglGameFrames??0)",
            "<canvas-game-frame-count>", result)) return false;
    char *end = NULL;
    unsigned long value = strtoul(result->summary, &end, 10);
    if (end == result->summary || *end != '\0' || value > 1000u) return false;
    *frames = (unsigned) value;
    return true;
}

static bool run_fixture(const char *name, const char *expected,
                        bool publish_gamepad, bool test_visibility)
{
    static const char html[] =
        "<!doctype html><html><body><canvas id=game></canvas></body></html>";
    Budget budget;
    budget_init(&budget, 24u * MIB);
    budget_install_lexbor(&budget);
    PocDocument document;
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    ViewportContext viewport;
    CHECK(viewport_context_init(&viewport, 480, 272, 480, 272));
    ScriptExecutionPolicy policy;
    CHECK(script_execution_policy_for_profile(
        SCRIPT_EXECUTION_PROFILE_PSP_REALISTIC, &policy));
    policy.slow_compile_threshold_us = UINT64_MAX;
    policy.slow_callback_threshold_us = UINT64_MAX;
    ScriptRuntimeOptions options = {
        .viewport = viewport,
        .execution_policy = policy,
        .defer_document_scripts = true
    };
    ScriptResult result;
    ScriptRuntime *runtime = script_runtime_create_configured(
        &document, &budget, 16u * MIB, 8000,
        "https://games.test/fixture", &options, &result);
    CHECK(runtime != NULL && result.success);
    ImageResources images = {.budget = &budget};
    script_runtime_set_images(runtime, &images);

    if (publish_gamepad) {
        TilefinchGamepadState gamepad = {
            .axes = {INT16_MAX, 0, 0, 0},
            .timestamp_ms = 1,
            .connected = true
        };
        CHECK(script_runtime_set_gamepad_state(runtime, &gamepad));
    }
    size_t source_length = 0;
    char *source = read_fixture(name, &source_length);
    CHECK(source != NULL && source_length != 0);
    bool evaluated = script_runtime_evaluate_diagnostic(
        runtime, source, name, &result);
    free(source);
    if (!evaluated)
        fprintf(stderr, "%s evaluation failed: %s\n", name, result.error);
    CHECK(evaluated);

    if (test_visibility) {
        unsigned initial_frames = 0;
        CHECK(observe_canvas_game_frames(runtime, &result, &initial_frames)
              && initial_frames == 1u);
        CHECK(script_runtime_advance(runtime, 16, 64, &result));
        CHECK(script_runtime_advance(runtime, 16, 64, &result));
        unsigned visible_frames = 0;
        CHECK(observe_canvas_game_frames(runtime, &result, &visible_frames)
              && visible_frames == initial_frames + 2u);
        CHECK(script_runtime_set_page_visibility(runtime, false));
        for (size_t tick = 0; tick < 4; tick++)
            CHECK(script_runtime_advance(runtime, 16, 64, &result));
        unsigned hidden_frames = 0;
        CHECK(observe_canvas_game_frames(runtime, &result, &hidden_frames)
              && hidden_frames == visible_frames);
        CHECK(script_runtime_set_page_visibility(runtime, true));
    }
    for (size_t tick = 0; tick < 24
         && strcmp(result.summary, expected) != 0; tick++) {
        CHECK(script_runtime_advance(runtime, 16, 64, &result));
    }
    bool result_read = script_runtime_evaluate_diagnostic(
        runtime,
        "globalThis.pocSummary=String(globalThis.pocSummary)",
        "<fixture-result>", &result);
    if (!result_read || strcmp(result.summary, expected) != 0)
        fprintf(stderr, "%s result: %s (%s)\n", name, result.summary,
                result.error);
    CHECK(result_read && strcmp(result.summary, expected) == 0);
    if (publish_gamepad) {
        CHECK(script_runtime_evaluate_diagnostic(
                  runtime,
                  "globalThis.pocSummary=__canvasGameState.inputObserved?"
                  "'GAMEPAD-OBSERVED':'GAMEPAD-MISSED'",
                  "<fixture-gamepad-result>", &result)
              && strcmp(result.summary, "GAMEPAD-OBSERVED") == 0);
    }

    script_runtime_set_images(runtime, NULL);
    script_runtime_destroy(runtime);
    images_destroy(&images);
    document_destroy(&document);
    CHECK(budget.current == 0
          && budget_active_allocations(&budget, NULL) == 0);
    return true;
}

static bool run_deferred_webgl_flush(void)
{
    static const char html[] = "<!doctype html><html><body></body></html>";
    Budget budget;
    budget_init(&budget, 24u * MIB);
    budget_install_lexbor(&budget);
    PocDocument document;
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    ViewportContext viewport;
    CHECK(viewport_context_init(&viewport, 480, 272, 480, 272));
    ScriptExecutionPolicy policy;
    CHECK(script_execution_policy_for_profile(
        SCRIPT_EXECUTION_PROFILE_PSP_REALISTIC, &policy));
    policy.slow_compile_threshold_us = UINT64_MAX;
    policy.slow_callback_threshold_us = UINT64_MAX;
    ScriptRuntimeOptions options = {
        .viewport = viewport,
        .execution_policy = policy,
        .defer_document_scripts = true
    };
    ScriptResult result;
    ScriptRuntime *runtime = script_runtime_create_configured(
        &document, &budget, 16u * MIB, 8000,
        "https://games.test/deferred", &options, &result);
    CHECK(runtime != NULL && result.success);

    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "const canvas=document.createElement('canvas');"
              "canvas.width=4;canvas.height=4;document.body.appendChild(canvas);"
              "globalThis.deferredGl=canvas.getContext('webgl');"
              "const shader=(kind,source)=>{const value=deferredGl.createShader(kind);"
              "deferredGl.shaderSource(value,source);deferredGl.compileShader(value);"
              "return value};const program=deferredGl.createProgram();"
              "deferredGl.attachShader(program,shader(deferredGl.VERTEX_SHADER,"
              "'attribute vec2 aPosition;void main(){gl_Position=vec4(aPosition,0.,1.);}'));"
              "deferredGl.attachShader(program,shader(deferredGl.FRAGMENT_SHADER,"
              "'uniform vec4 uColor;void main(){gl_FragColor=uColor;}'));"
              "deferredGl.linkProgram(program);deferredGl.useProgram(program);"
              "deferredGl.uniform4f(deferredGl.getUniformLocation(program,'uColor'),"
              "1,0,0,1);"
              "const buffer=deferredGl.createBuffer();"
              "deferredGl.bindBuffer(deferredGl.ARRAY_BUFFER,buffer);"
              "deferredGl.bufferData(deferredGl.ARRAY_BUFFER,"
              "new Float32Array([-1,-1,1,-1,0,1]),deferredGl.DYNAMIC_DRAW);"
              "const position=deferredGl.getAttribLocation(program,'aPosition');"
              "deferredGl.vertexAttribPointer(position,2,deferredGl.FLOAT,false,0,0);"
              "deferredGl.enableVertexAttribArray(position);"
              "deferredGl.clearColor(0,0,0,1);"
              "deferredGl.clear(deferredGl.COLOR_BUFFER_BIT);"
              "deferredGl.drawArrays(deferredGl.TRIANGLES,0,3);"
              "deferredGl.finish();"
              "deferredGl.bufferSubData(deferredGl.ARRAY_BUFFER,0,"
              "new Float32Array([8,8,9,8,8,9]));"
              "globalThis.pocSummary=!deferredGl.isContextLost()"
              "&&deferredGl.getError()===deferredGl.NO_ERROR"
              "&&__tilefinchWebGLDiagnostics.deferredFlushes>0"
              "?'WEBGL-DEFERRED':'WEBGL-DEFERRED-FAIL'",
              "<webgl-deferred>", &result)
          && strcmp(result.summary, "WEBGL-DEFERRED") == 0);

    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "const inert={kind:'clear',mask:0,color:[0,0,0,0],depth:1,"
              "scissorEnabled:false,scissor:[0,0,4,4]};"
              "while(deferredGl._queuedSources.size<32)"
              "deferredGl._enqueueCommand(inert,[new Uint8Array(1)]);"
              "deferredGl._enqueueCommand(inert,[new Uint8Array(1)]);"
              "const headBefore=deferredGl._commands.length;"
              "deferredGl._enqueueCommand(inert);"
              "deferredGl.finish();"
              "globalThis.pocSummary=!deferredGl.isContextLost()"
              "&&deferredGl.getError()===deferredGl.NO_ERROR"
              "&&headBefore===deferredGl._commands.length"
              "&&deferredGl._tailCommands.length===2"
              "?'WEBGL-DEFERRED-CAP':'WEBGL-DEFERRED-CAP-FAIL:'"
              "+deferredGl._commands.length+'|'+deferredGl._tailCommands.length"
              "+'|'+deferredGl.getError()",
              "<webgl-deferred-cap>", &result)
          && strcmp(result.summary, "WEBGL-DEFERRED-CAP") == 0);

    ImageResources images = {.budget = &budget};
    script_runtime_set_images(runtime, &images);
    bool replayed = script_runtime_evaluate_diagnostic(
              runtime,
              "deferredGl.finish();const before=new Uint8Array(4);"
              "deferredGl.readPixels(2,2,1,1,deferredGl.RGBA,"
              "deferredGl.UNSIGNED_BYTE,before);"
              "deferredGl.clear(deferredGl.COLOR_BUFFER_BIT);"
              "deferredGl.drawArrays(deferredGl.TRIANGLES,0,3);"
              "deferredGl.finish();const after=new Uint8Array(4);"
              "deferredGl.readPixels(2,2,1,1,deferredGl.RGBA,"
              "deferredGl.UNSIGNED_BYTE,after);"
              "globalThis.pocSummary=!deferredGl.isContextLost()"
              "&&before[0]>200&&before[1]<20&&before[2]<20&&before[3]===255"
              "&&after[0]<20&&after[1]<20&&after[2]<20&&after[3]===255"
              "?'WEBGL-DEFERRED-REPLAYED':'WEBGL-DEFERRED-REPLAY-FAIL:'"
              "+Array.from(before)+'|'+Array.from(after)+'|'+deferredGl.getError()",
              "<webgl-deferred-replay>", &result);
    if (!replayed || strcmp(result.summary, "WEBGL-DEFERRED-REPLAYED") != 0)
        fprintf(stderr, "deferred replay failed: %s (%s)\n",
                result.summary, result.error);
    CHECK(replayed && strcmp(result.summary, "WEBGL-DEFERRED-REPLAYED") == 0);

    CHECK(images.count != 0 && images.items[0].pixels != NULL);
    unsigned char original_pixel[4];
    memcpy(original_pixel, images.items[0].pixels, sizeof(original_pixel));
    ImageResources relocated_images = {.budget = &budget};
    script_runtime_set_images(runtime, NULL);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "deferredGl.clearColor(0,0,1,1);"
              "deferredGl.clear(deferredGl.COLOR_BUFFER_BIT);"
              "deferredGl.finish();"
              "globalThis.pocSummary=!deferredGl.isContextLost()?"
              "'WEBGL-RELOCATE-PENDING':'WEBGL-RELOCATE-FAIL'",
              "<webgl-relocate-pending>", &result)
          && strcmp(result.summary, "WEBGL-RELOCATE-PENDING") == 0);
    script_runtime_set_images(runtime, &relocated_images);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "const relocatedPixel=new Uint8Array(4);"
              "deferredGl.readPixels(0,0,1,1,deferredGl.RGBA,"
              "deferredGl.UNSIGNED_BYTE,relocatedPixel);"
              "globalThis.pocSummary=relocatedPixel[2]>200"
              "&&relocatedPixel[3]===255?'WEBGL-RELOCATED':"
              "'WEBGL-RELOCATE-FAIL:'+Array.from(relocatedPixel)",
              "<webgl-relocated>", &result)
          && strcmp(result.summary, "WEBGL-RELOCATED") == 0);
    CHECK(images.items[0].pixels != NULL
          && memcmp(images.items[0].pixels, original_pixel,
                    sizeof(original_pixel)) == 0
          && relocated_images.count != 0);

    uint32_t restore_epoch_high = runtime->bridge.webgl_realm_epoch_high;
    uint32_t restore_epoch_low = runtime->bridge.webgl_realm_epoch_low;
    bool restore_pending = script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.restoreLost=0;globalThis.restoreDone=0;"
              "globalThis.restoreLostErrors=false;"
              "globalThis.reservationObserved=false;"
              "globalThis.restoreStayedLost=false;"
              "globalThis.oldRestoreBuffer=deferredGl.createBuffer();"
              "deferredGl.canvas.addEventListener('webglcontextlost',e=>{"
              "restoreLost++;e.preventDefault();"
              "restoreLostErrors="
              "deferredGl.getError()===deferredGl.CONTEXT_LOST_WEBGL"
              "&&deferredGl.getError()===deferredGl.NO_ERROR;"
              "const reservedContext=document.createElement('canvas')"
              ".getContext('webgl');"
              "const overLimitContext=document.createElement('canvas')"
              ".getContext('webgl');"
              "reservationObserved=!!reservedContext&&overLimitContext===null;"
              "restoreStayedLost=deferredGl.isContextLost()&&restoreDone===0;"
              "reservedContext?._lose('release reserve')});"
              "deferredGl.canvas.addEventListener('webglcontextrestored',()=>"
              "restoreDone++);deferredGl._lose('canceled restoration test');"
              "globalThis.pocSummary=deferredGl.isContextLost()"
              "&&restoreLost===1&&restoreDone===0&&restoreLostErrors"
              "&&reservationObserved&&restoreStayedLost?"
              "'WEBGL-RESTORE-PENDING':"
              "'WEBGL-RESTORE-PENDING-FAIL:'"
              "+deferredGl.isContextLost()+'|'+restoreLost+'|'"
              "+restoreDone+'|'+restoreLostErrors+'|'"
              "+reservationObserved+'|'+restoreStayedLost",
              "<webgl-restore-pending>", &result);
    if (!restore_pending
        || strcmp(result.summary, "WEBGL-RESTORE-PENDING") != 0)
        fprintf(stderr, "restore pending failed: %s (%s)\n",
                result.summary, result.error);
    CHECK(restore_pending
          && strcmp(result.summary, "WEBGL-RESTORE-PENDING") == 0);
    CHECK(restore_epoch_high != runtime->bridge.webgl_realm_epoch_high
          || restore_epoch_low != runtime->bridge.webgl_realm_epoch_low);
    CHECK(script_runtime_advance(runtime, 0, 64, &result));
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "const sameRestore=deferredGl.canvas.getContext('webgl');"
              "globalThis.pocSummary=sameRestore===deferredGl"
              "&&!deferredGl.isContextLost()&&restoreLost===1&&restoreDone===1"
              "&&oldRestoreBuffer._deleted&&oldRestoreBuffer._data.byteLength===0"
              "&&deferredGl.createBuffer()!==null"
              "?'WEBGL-RESTORED':'WEBGL-RESTORE-FAIL'",
              "<webgl-restored>", &result)
          && strcmp(result.summary, "WEBGL-RESTORED") == 0);

    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.invalidRestoreLost=0;"
              "globalThis.invalidRestoreDone=0;"
              "const invalidRestoreCanvas=document.createElement('canvas');"
              "globalThis.invalidRestoreGl="
              "invalidRestoreCanvas.getContext('webgl');"
              "invalidRestoreCanvas.addEventListener('webglcontextlost',e=>{"
              "invalidRestoreLost++;if(invalidRestoreLost===1){"
              "e.preventDefault();Object.defineProperty(invalidRestoreCanvas,"
              "'width',{configurable:true,value:NaN})}});"
              "invalidRestoreCanvas.addEventListener('webglcontextrestored',"
              "()=>invalidRestoreDone++);"
              "invalidRestoreGl._lose('invalid restored dimensions');"
              "globalThis.pocSummary=invalidRestoreGl.isContextLost()?"
              "'WEBGL-INVALID-RESTORE-PENDING':"
              "'WEBGL-INVALID-RESTORE-PENDING-FAIL'",
              "<webgl-invalid-restore-pending>", &result)
          && strcmp(result.summary, "WEBGL-INVALID-RESTORE-PENDING") == 0);
    CHECK(script_runtime_advance(runtime, 0, 64, &result));
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.pocSummary=invalidRestoreGl.isContextLost()"
              "&&invalidRestoreLost===2&&invalidRestoreDone===0?"
              "'WEBGL-INVALID-RESTORE-TERMINAL':"
              "'WEBGL-INVALID-RESTORE-FAIL:'"
              "+invalidRestoreLost+'|'+invalidRestoreDone+'|'"
              "+invalidRestoreGl.isContextLost()",
              "<webgl-invalid-restore-terminal>", &result)
          && strcmp(result.summary,
                    "WEBGL-INVALID-RESTORE-TERMINAL") == 0);

    script_runtime_set_images(runtime, NULL);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "deferredGl._lose('detached bridge restoration');"
              "globalThis.pocSummary=deferredGl.isContextLost()"
              "&&deferredGl._restorePending&&deferredGl._nativeReleasePending"
              "?'WEBGL-DETACHED-LOSS':'WEBGL-DETACHED-LOSS-FAIL'",
              "<webgl-detached-loss>", &result)
          && strcmp(result.summary, "WEBGL-DETACHED-LOSS") == 0);
    CHECK(script_runtime_advance(runtime, 0, 64, &result));
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.pocSummary=deferredGl.isContextLost()"
              "&&deferredGl._restorePending?'WEBGL-DETACHED-WAIT':"
              "'WEBGL-DETACHED-WAIT-FAIL'",
              "<webgl-detached-wait>", &result)
          && strcmp(result.summary, "WEBGL-DETACHED-WAIT") == 0);
    script_runtime_set_images(runtime, &relocated_images);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.pocSummary=!deferredGl.isContextLost()"
              "&&!deferredGl._restorePending&&!deferredGl._nativeReleasePending"
              "?'WEBGL-REBOUND-RESTORED':'WEBGL-REBOUND-RESTORE-FAIL'",
              "<webgl-rebound-restored>", &result)
          && strcmp(result.summary, "WEBGL-REBOUND-RESTORED") == 0);
    for (size_t cycle = 0; cycle < 3u; cycle++) {
        CHECK(script_runtime_evaluate_diagnostic(
                  runtime,
                  "deferredGl._lose('repeat restoration');"
                  "globalThis.pocSummary=deferredGl.isContextLost()?"
                  "'WEBGL-REPEAT-PENDING':'WEBGL-REPEAT-LOSS-FAIL'",
                  "<webgl-repeat-loss>", &result)
              && strcmp(result.summary, "WEBGL-REPEAT-PENDING") == 0);
        CHECK(script_runtime_advance(runtime, 0, 64, &result));
        CHECK(script_runtime_evaluate_diagnostic(
                  runtime,
                  "globalThis.pocSummary=!deferredGl.isContextLost()?"
                  "'WEBGL-REPEAT-RESTORED':'WEBGL-REPEAT-RESTORE-FAIL'",
                  "<webgl-repeat-restored>", &result)
              && strcmp(result.summary, "WEBGL-REPEAT-RESTORED") == 0);
    }
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.pocSummary=restoreLost===5&&restoreDone===5?"
              "'WEBGL-REPEAT-COUNT':'WEBGL-REPEAT-COUNT-FAIL:'"
              "+restoreLost+'|'+restoreDone",
              "<webgl-repeat-count>", &result)
          && strcmp(result.summary, "WEBGL-REPEAT-COUNT") == 0);

    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "(()=>{const abandoned=document.createElement('canvas');"
              "abandoned.getContext('webgl');})();"
              "globalThis.pocSummary=document.createElement('canvas')"
              ".getContext('webgl')===null?'WEBGL-CONTEXT-LIMIT':"
              "'WEBGL-CONTEXT-LIMIT-FAIL'",
              "<webgl-context-limit>", &result)
          && strcmp(result.summary, "WEBGL-CONTEXT-LIMIT") == 0);
    CHECK(script_runtime_advance(runtime, 0, 64, &result));
    (void) script_runtime_collect_and_trim(runtime);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.collectedReplacement=document.createElement('canvas')"
              ".getContext('webgl');globalThis.pocSummary=collectedReplacement"
              "?'WEBGL-CONTEXT-COLLECTED':'WEBGL-CONTEXT-STILL-HELD'",
              "<webgl-context-collection>", &result)
          && strcmp(result.summary, "WEBGL-CONTEXT-COLLECTED") == 0);

    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "const terminalCanvas=collectedReplacement.canvas;"
              "collectedReplacement._lose('terminal loss');"
              "const sameTerminal=terminalCanvas.getContext('webgl');"
              "globalThis.detachedCanvas=document.createElement('canvas');"
              "detachedCanvas.width=2;detachedCanvas.height=2;"
              "globalThis.detachedGl=detachedCanvas.getContext('webgl');"
              "globalThis.pocSummary=sameTerminal===collectedReplacement"
              "&&collectedReplacement.isContextLost()"
              "&&collectedReplacement.getError()"
              "===collectedReplacement.CONTEXT_LOST_WEBGL"
              "&&collectedReplacement.getError()===collectedReplacement.NO_ERROR"
              "&&detachedGl?'WEBGL-TERMINAL-RECLAIMED':"
              "'WEBGL-TERMINAL-RECLAIM-FAIL'",
              "<webgl-terminal-reclaim>", &result)
          && strcmp(result.summary, "WEBGL-TERMINAL-RECLAIMED") == 0);
    size_t mutations_before_detached = runtime->bridge.mutations.count;
    bool detached_evaluated = script_runtime_evaluate_diagnostic(
              runtime,
              "detachedGl.clearColor(.1,.8,.2,1);"
              "detachedGl.clear(detachedGl.COLOR_BUFFER_BIT);detachedGl.finish();"
              "const detachedPixel=new Uint8Array(4);"
              "detachedGl.readPixels(0,0,1,1,detachedGl.RGBA,"
              "detachedGl.UNSIGNED_BYTE,detachedPixel);"
              "const sampled=document.createElement('canvas');"
              "sampled.width=2;sampled.height=2;"
              "const sampledContext=sampled.getContext('2d');"
              "sampledContext.drawImage(detachedCanvas,0,0);"
              "const sampledPixel=sampledContext.getImageData(0,0,1,1).data;"
              "globalThis.pocSummary=detachedPixel[1]>150&&detachedPixel[3]===255"
              "&&sampledPixel[1]>150&&sampledPixel[3]===255"
              "?'WEBGL-DETACHED':'WEBGL-DETACHED-FAIL:'"
              "+Array.from(detachedPixel)+'|'+Array.from(sampledPixel)+'|'"
              "+detachedGl.isContextLost()+'|'+detachedGl.getError()+'|'"
              "+detachedGl._commands.length+'|'+detachedGl._surfaceReady",
              "<webgl-detached>", &result);
    if (!detached_evaluated || strcmp(result.summary, "WEBGL-DETACHED") != 0)
        fprintf(stderr, "detached WebGL result: %s (%s)\n",
                result.summary, result.error);
    CHECK(detached_evaluated && strcmp(result.summary, "WEBGL-DETACHED") == 0);
    CHECK(runtime->bridge.mutations.count == mutations_before_detached);

    script_runtime_set_images(runtime, NULL);
    script_runtime_destroy(runtime);
    images_destroy(&images);
    images_destroy(&relocated_images);
    document_destroy(&document);
    CHECK(budget.current == 0
          && budget_active_allocations(&budget, NULL) == 0);
    return true;
}

static bool run_prism_break_game(void)
{
    static const char document_url[] =
        "https://games.test/examples/prism-break-3d/index.html";
    size_t html_length = 0, script_length = 0, css_length = 0;
    size_t manifest_length = 0;
    char *html = read_source(
        "examples/prism-break-3d/index.html", &html_length);
    char *script = read_source(
        "examples/prism-break-3d/game.js", &script_length);
    char *css = read_source(
        "examples/prism-break-3d/game.css", &css_length);
    char *manifest_json = read_source(
        "examples/prism-break-3d/manifest.webmanifest", &manifest_length);
    CHECK(html != NULL && script != NULL && css != NULL
          && manifest_json != NULL
          && html_length + script_length + css_length + manifest_length
             < MIB);

    Budget budget;
    budget_init(&budget, 24u * MIB);
    budget_install_lexbor(&budget);
    PocDocument document;
    CHECK(document_parse(&document, &budget, html, html_length, 17));
    size_t href_length = 0;
    const char *href = document_web_app_manifest_href(
        &document, &href_length);
    CHECK(href != NULL && href_length == strlen("manifest.webmanifest")
          && memcmp(href, "manifest.webmanifest", href_length) == 0);
    TilefinchWebAppManifest manifest = {0};
    char manifest_error[160] = {0};
    CHECK(tilefinch_web_app_manifest_parse(
              manifest_json, manifest_length,
              "https://games.test/examples/prism-break-3d/manifest.webmanifest",
              document_url, &manifest, manifest_error,
              sizeof(manifest_error))
          && strcmp(manifest.name, "Prism Break 3D") == 0
          && strcmp(manifest.short_name, "Prism Break") == 0
          && strcmp(manifest.start_url, document_url) == 0
          && strcmp(manifest.scope,
                    "https://games.test/examples/prism-break-3d/") == 0
          && manifest.display_mode == TILEFINCH_WEB_APP_DISPLAY_FULLSCREEN
          && manifest.theme_color_valid
          && manifest.theme_color == UINT32_C(0x071229));

    ViewportContext viewport;
    CHECK(viewport_context_init(&viewport, 480, 272, 480, 272));
    ScriptExecutionPolicy policy;
    CHECK(script_execution_policy_for_profile(
        SCRIPT_EXECUTION_PROFILE_PSP_REALISTIC, &policy));
    policy.slow_compile_threshold_us = UINT64_MAX;
    policy.slow_callback_threshold_us = UINT64_MAX;
    ScriptRuntimeOptions options = {
        .viewport = viewport,
        .execution_policy = policy,
        .defer_document_scripts = true
    };
    ScriptResult result;
    ScriptRuntime *runtime = script_runtime_create_configured(
        &document, &budget, 16u * MIB, 8000, document_url,
        &options, &result);
    CHECK(runtime != NULL && result.success);
    ImageResources images = {.budget = &budget};
    script_runtime_set_images(runtime, &images);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, script, "prism-break-3d/game.js", &result)
          && strcmp(result.summary, "PRISM-BREAK-READY") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "const titleStart=__prismBreakDebug.snapshot();"
              "__prismBreakDebug.step(30);"
              "const titleMoved=__prismBreakDebug.snapshot();"
              "globalThis.pocSummary=titleStart.mode==='title'"
              "&&titleMoved.mode==='title'&&titleMoved.frames>titleStart.frames"
              "&&Math.abs(titleMoved.titleMotionX-titleStart.titleMotionX)>.01"
              "?'PRISM-TITLE-ANIMATED':'PRISM-TITLE-STATIC'",
              "<prism-title-animation>", &result)
          && strcmp(result.summary, "PRISM-TITLE-ANIMATED") == 0);
    const char *soak_frames = getenv("TILEFINCH_WEBGL_SOAK_FRAMES");
    if (soak_frames != NULL && strcmp(soak_frames, "600") == 0) {
        JS_RunGC(runtime->runtime);
        size_t heap_before = budget_quickjs_pool_js_malloc_current(
            runtime->quickjs_pool);
        size_t allocations_before = budget.allocation_count;
        CHECK(script_runtime_evaluate_diagnostic(
                  runtime,
                  "for(let frame=0;frame<600;frame++)"
                  "__prismBreakDebug.step(1);"
                  "globalThis.pocSummary='PRISM-SOAKED'",
                  "<prism-600-frame-soak>", &result)
              && strcmp(result.summary, "PRISM-SOAKED") == 0);
        JS_RunGC(runtime->runtime);
        size_t heap_after = budget_quickjs_pool_js_malloc_current(
            runtime->quickjs_pool);
        size_t heap_peak = budget_quickjs_pool_js_malloc_peak(
            runtime->quickjs_pool);
        size_t allocation_delta = budget.allocation_count - allocations_before;
        fprintf(stderr,
                "webgl-soak: frames=600 heap-before=%zu heap-after=%zu "
                "heap-peak=%zu budget-allocations=%zu\n",
                heap_before, heap_after, heap_peak, allocation_delta);
        CHECK(heap_after <= heap_before + 256u * 1024u);
    }
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "__prismBreakDebug.start();__prismBreakDebug.step(0);"
              "const startSnapshot=__prismBreakDebug.snapshot();"
              "globalThis.pocSummary=startSnapshot.mode==='playing'"
              "&&startSnapshot.bricks>=40"
              "&&startSnapshot.staticVertices>900"
              "&&startSnapshot.staticVertices<=3072"
              "&&startSnapshot.staticIndices<=4608"
              "&&startSnapshot.vertices<=3072"
              "&&startSnapshot.indices<=4608&&startSnapshot.meshDrops===0"
              "?'PRISM-STARTED':'PRISM-START-FAIL'",
              "<prism-start>", &result)
          && strcmp(result.summary, "PRISM-STARTED") == 0);

    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "const keyboardStart=__prismBreakDebug.snapshot().paddleX;"
              "dispatchEvent(new KeyboardEvent('keydown',"
              "{key:'ArrowRight',code:'ArrowRight',cancelable:true}));"
              "__prismBreakDebug.step(20);"
              "dispatchEvent(new KeyboardEvent('keyup',"
              "{key:'ArrowRight',code:'ArrowRight',cancelable:true}));"
              "const keyboardEnd=__prismBreakDebug.snapshot();"
              "globalThis.pocSummary=keyboardEnd.paddleX>keyboardStart+.5"
              "&&keyboardEnd.inputSource==='keyboard'"
              "?'PRISM-KEYBOARD':'PRISM-KEYBOARD-FAIL'",
              "<prism-keyboard>", &result)
          && strcmp(result.summary, "PRISM-KEYBOARD") == 0);

    TilefinchGamepadState gamepad = {
        .axes = {INT16_MAX, 0, 0, 0},
        .timestamp_ms = 1,
        .connected = true
    };
    CHECK(script_runtime_set_gamepad_state(runtime, &gamepad));
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "__prismBreakDebug.step(30);"
              "globalThis.pocSummary=__prismBreakDebug.snapshot().paddleX>1"
              "?'PRISM-GAMEPAD':'PRISM-GAMEPAD-FAIL'",
              "<prism-gamepad>", &result)
          && strcmp(result.summary, "PRISM-GAMEPAD") == 0);
    gamepad.axes[0] = 0;
    gamepad.timestamp_ms = 2;
    CHECK(script_runtime_set_gamepad_state(runtime, &gamepad));
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "const idlePadStart=__prismBreakDebug.snapshot().paddleX;"
              "dispatchEvent(new KeyboardEvent('keydown',"
              "{key:'ArrowLeft',code:'ArrowLeft',cancelable:true}));"
              "__prismBreakDebug.step(20);"
              "dispatchEvent(new KeyboardEvent('keyup',"
              "{key:'ArrowLeft',code:'ArrowLeft',cancelable:true}));"
              "const idlePadEnd=__prismBreakDebug.snapshot();"
              "globalThis.pocSummary=idlePadEnd.paddleX<idlePadStart-.5"
              "&&idlePadEnd.inputSource==='keyboard'"
              "&&idlePadEnd.gamepadConnected"
              "?'PRISM-IDLE-PAD-KEYBOARD':'PRISM-IDLE-PAD-BLOCKED'",
              "<prism-idle-gamepad-keyboard>", &result)
          && strcmp(result.summary, "PRISM-IDLE-PAD-KEYBOARD") == 0);
    bool physics_evaluated = script_runtime_evaluate_diagnostic(
              runtime,
              "__prismBreakDebug.start();__prismBreakDebug.launch();"
              "__prismBreakDebug.step(120);"
              "const playSnapshot=__prismBreakDebug.snapshot();"
              "globalThis.pocSummary=playSnapshot.score>0"
              "&&playSnapshot.balls===1&&playSnapshot.lives>=2"
              "&&playSnapshot.meshDrops===0"
              "?'PRISM-PHYSICS':'PRISM-PHYSICS-FAIL:'"
              "+JSON.stringify(playSnapshot)",
              "<prism-physics>", &result);
    if (!physics_evaluated || strcmp(result.summary, "PRISM-PHYSICS") != 0)
        fprintf(stderr, "Prism physics result: %s (%s)\n",
                result.summary, result.error);
    CHECK(physics_evaluated && strcmp(result.summary, "PRISM-PHYSICS") == 0);
    bool effects_evaluated = script_runtime_evaluate_diagnostic(
              runtime,
              "__prismBreakDebug.injectPower('multi');"
              "__prismBreakDebug.injectPower('wide');"
              "__prismBreakDebug.injectPower('shield');"
              "__prismBreakDebug.injectPower('laser');"
              "__prismBreakDebug.burst();__prismBreakDebug.step(2);"
              "const effectSnapshot=__prismBreakDebug.snapshot();"
              "globalThis.pocSummary=effectSnapshot.balls===3"
              "&&effectSnapshot.paddleWidth>2&&effectSnapshot.shield===1"
              "&&effectSnapshot.laser===12&&effectSnapshot.particles>0"
              "&&effectSnapshot.particles<=48&&effectSnapshot.vertices<=3072"
              "&&effectSnapshot.indices<=4608&&effectSnapshot.meshDrops===0"
              "?'PRISM-EFFECTS':'PRISM-EFFECTS-FAIL:'"
              "+JSON.stringify(effectSnapshot)",
              "<prism-effects>", &result);
    if (!effects_evaluated || strcmp(result.summary, "PRISM-EFFECTS") != 0)
        fprintf(stderr, "Prism effects result: %s (%s)\n",
                result.summary, result.error);
    CHECK(effects_evaluated && strcmp(result.summary, "PRISM-EFFECTS") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "for(let level=0;level<4;level++){"
              "__prismBreakDebug.clearLevel();__prismBreakDebug.step(1);}"
              "const levelSnapshot=__prismBreakDebug.snapshot();"
              "globalThis.pocSummary=levelSnapshot.mode==='victory'"
              "&&levelSnapshot.level===4"
              "?'PRISM-VICTORY':'PRISM-VICTORY-FAIL'",
              "<prism-levels>", &result)
          && strcmp(result.summary, "PRISM-VICTORY") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
        runtime, "__prismBreakDebug.stop();globalThis.pocSummary='STOPPED'",
        "<prism-stop>", &result));

    script_runtime_set_images(runtime, NULL);
    script_runtime_destroy(runtime);
    images_destroy(&images);
    document_destroy(&document);
    free(html);
    free(script);
    free(css);
    free(manifest_json);
    CHECK(budget.current == 0
          && budget_active_allocations(&budget, NULL) == 0);
    return true;
}

static bool cache_prism_resource(
    BrowserEngine *engine, const char *document_url, const char *url,
    const char *content_type, TilefinchRequestDestination destination,
    TilefinchCredentialsMode credentials,
    const unsigned char *data, size_t length)
{
    Budget *budget = browser_engine_budget(engine);
    BrowserSession *session = browser_engine_session(engine);
    unsigned char *copy = budget_malloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, length + 1u);
    if (copy == NULL) return false;
    memcpy(copy, data, length);
    copy[length] = 0;
    BrowserSharedBody *body = browser_shared_body_take(
        budget, copy, length);
    if (body == NULL) {
        budget_free(budget, copy);
        return false;
    }
    TilefinchRequestContext context = {
        .target_url = url,
        .initiator_url = document_url,
        .top_level_url = document_url,
        .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = credentials,
        .destination = destination
    };
    TilefinchResourceGrant grant = {
        .destination = destination,
        .mode = context.mode,
        .credentials = credentials,
        .final_same_origin = true,
        .final_same_site = true,
        .mime_validated = true
    };
    bool stored = browser_session_cache_put_http_shared_resource(
        session, url, body, "", "", content_type,
        "public,max-age=3600", "", UINT64_C(1), &context, &grant);
    browser_shared_body_release(body);
    return stored;
}

static bool run_prism_break_offline_reopen(void)
{
    static const char document_url[] =
        "https://games.test/examples/prism-break-3d/index.html";
    static const char css_url[] =
        "https://games.test/examples/prism-break-3d/game.css";
    static const char script_url[] =
        "https://games.test/examples/prism-break-3d/game.js";
    size_t html_length = 0, script_length = 0, css_length = 0;
    size_t manifest_length = 0;
    char *html = read_source(
        "examples/prism-break-3d/index.html", &html_length);
    char *script = read_source(
        "examples/prism-break-3d/game.js", &script_length);
    char *css = read_source(
        "examples/prism-break-3d/game.css", &css_length);
    char *manifest_json = read_source(
        "examples/prism-break-3d/manifest.webmanifest", &manifest_length);
    CHECK(html != NULL && script != NULL && css != NULL
          && manifest_json != NULL);

    /* Model installation while the running game has hidden its panel. The
       installed app must reconstruct a usable entry state when its script
       starts again. */
    static const char panel[] = "<section id=\"panel\">";
    char *panel_at = strstr(html, panel);
    CHECK(panel_at != NULL);
    size_t prefix = (size_t) (panel_at - html);
    static const char hidden_panel[] = "<section id=\"panel\" hidden>";
    size_t grown_length = html_length
        + sizeof(hidden_panel) - sizeof(panel);
    char *snapshot_html = malloc(grown_length + 1u);
    CHECK(snapshot_html != NULL);
    memcpy(snapshot_html, html, prefix);
    memcpy(snapshot_html + prefix, hidden_panel, sizeof(hidden_panel) - 1u);
    size_t suffix_start = prefix + sizeof(panel) - 1u;
    memcpy(snapshot_html + prefix + sizeof(hidden_panel) - 1u,
           html + suffix_start, html_length - suffix_start + 1u);

    BrowserConfig config;
    browser_config_init(&config, NULL);
    config.memory_limit = 32u * MIB;
    config.javascript.enabled = true;
    config.javascript.document_scripts_enabled = true;
    config.resources.enabled = true;
    char engine_error[256] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, engine_error, sizeof(engine_error));
    CHECK(engine != NULL);
    CHECK(cache_prism_resource(
              engine, document_url, css_url, "text/css",
              TILEFINCH_DESTINATION_STYLE, TILEFINCH_CREDENTIALS_INCLUDE,
              (const unsigned char *) css, css_length)
          && cache_prism_resource(
              engine, document_url, script_url, "text/javascript",
              TILEFINCH_DESTINATION_SCRIPT,
              TILEFINCH_CREDENTIALS_INCLUDE,
              (const unsigned char *) script, script_length));

    PocDocument snapshot;
    CHECK(document_parse(
        &snapshot, browser_engine_budget(engine), snapshot_html,
        grown_length, 512));
    TilefinchWebAppManifest manifest = {0};
    char error[256] = {0};
    CHECK(tilefinch_web_app_manifest_parse(
        manifest_json, manifest_length,
        "https://games.test/examples/prism-break-3d/manifest.webmanifest",
        document_url, &manifest, error, sizeof(error)));
    char directory[] = "/tmp/tilefinch-prism-offline-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    OfflineLibrary library;
    offline_library_init(
        &library, browser_engine_budget(engine), directory);
    uint32_t id = 0;
    CHECK(offline_library_save_web_app(
        &library, &snapshot, browser_engine_session(engine), document_url,
        &manifest, NULL, 0, &id, error, sizeof(error)));
    document_destroy(&snapshot);

    browser_session_cache_clear(browser_engine_session(engine));
    char *restored_html = NULL;
    size_t restored_length = 0;
    CHECK(offline_library_read_web_app(
        &library, browser_engine_budget(engine), browser_engine_session(engine),
        id, &restored_html, &restored_length, error, sizeof(error)));
    TilefinchRequestContext restored_script_context = {
        .target_url = script_url,
        .initiator_url = document_url,
        .top_level_url = document_url,
        .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_SCRIPT
    };
    const BrowserCacheEntry *restored_script = NULL;
    BrowserCacheStatus restored_script_status =
        browser_session_cache_match_classic_script(
            browser_engine_session(engine), script_url,
            &restored_script_context, UINT64_C(2), &restored_script);
    if (restored_script_status != BROWSER_CACHE_FRESH) {
        fprintf(stderr, "offline Prism: restored script cache status=%d\n",
                (int) restored_script_status);
    }
    CHECK(browser_engine_commit_html(
        engine, document_url, restored_html, restored_length, true));
    budget_free(browser_engine_budget(engine), restored_html);
    NavigationSession *navigation = browser_engine_navigation(engine);
    lxb_dom_node_t *restored_panel = find_element_id(
        lxb_dom_interface_node(navigation->page.document.html), "panel");
    size_t hidden_length = 0;
    bool panel_visible = restored_panel != NULL
        && document_attribute(restored_panel, "hidden", &hidden_length) == NULL;
    bool gamepad_available = browser_engine_page_gamepad_available(engine);
    if (!panel_visible || navigation->script_loaded != 1u
        || navigation->page.runtime == NULL || !gamepad_available) {
        fprintf(stderr,
                "offline Prism: panel=%d scripts=%zu/%zu attempted=%zu "
                "failed=%zu cache-hits=%zu bytes=%zu runtime=%d gamepad=%d "
                "script-success=%d script-error=%s navigation-error=%s\n",
                panel_visible, navigation->script_loaded,
                navigation->script_discovered, navigation->script_attempted,
                navigation->script_failed, navigation->script_cache_hits,
                navigation->script_bytes,
                navigation->page.runtime != NULL, gamepad_available,
                navigation->page.script_result.success,
                navigation->page.script_result.error,
                navigation->last_error);
    }
    CHECK(panel_visible && navigation->script_loaded == 1u
          && navigation->page.runtime != NULL && gamepad_available);

    CHECK(offline_library_remove(&library, id));
    char index_path[256], backup_path[256], temporary_path[256];
    snprintf(index_path, sizeof(index_path), "%s/library.bin", directory);
    snprintf(backup_path, sizeof(backup_path), "%s/library.bin.bak", directory);
    snprintf(temporary_path, sizeof(temporary_path), "%s/library.bin.tmp",
             directory);
    (void) unlink(index_path);
    (void) unlink(backup_path);
    (void) unlink(temporary_path);
    browser_engine_destroy(engine);
    CHECK(rmdir(directory) == 0);
    free(snapshot_html);
    free(html);
    free(script);
    free(css);
    free(manifest_json);
    return true;
}

int main(void)
{
    puts("test: native WebGL cache admission separates realm incarnations");
    if (!run_webgl_cache_epoch_admission()) return 1;
    puts("test: refused WebGL color/depth preparation leaves Budget ownership");
    if (!run_webgl_budget_refusal_ownership()) return 1;
    puts("test: representative Canvas game advances, responds, and pauses");
    if (!run_fixture("canvas-game.js", "CANVAS-GAME-PASS", true, true))
        return 1;
    puts("test: representative indexed WebGL game renders repeated frames");
    if (!run_fixture("webgl-game.js", "WEBGL-GAME-PASS", false, true))
        return 1;
    puts("test: curated Canvas drawing and bounds conformance");
    if (!run_fixture(
            "canvas-conformance.js", "CANVAS-CONFORMANCE-PASS", false, false))
        return 1;
    puts("test: curated WebGL object, error, draw, and bounds conformance");
    if (!run_fixture(
            "webgl-conformance.js", "WEBGL-CONFORMANCE-PASS", false, false))
        return 1;
    puts("test: detached WebGL flush retains commands until reattachment");
    if (!run_deferred_webgl_flush()) return 1;
    puts("test: installable Prism Break exercises levels, input, and effects");
    if (!run_prism_break_game()) return 1;
    puts("test: installed Prism Break reopens from its offline resource pack");
    if (!run_prism_break_offline_reopen()) return 1;
    puts("tilefinch-canvas-webgl-conformance-tests: all checks passed");
    return 0;
}
