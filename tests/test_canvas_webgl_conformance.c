#include "tilefinch/budget.h"
#include "tilefinch/budget_quickjs.h"
#include "tilefinch/browser_engine.h"
#include "tilefinch/document.h"
#include "tilefinch/gamepad.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/offline_library.h"
#include "tilefinch/psp_offline_store.h"
#include "tilefinch/platform.h"
#include "tilefinch/resources.h"
#include "tilefinch/viewport.h"
#include "tilefinch/web_app_manifest.h"

#include <limits.h>
#include <math.h>
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
    if (end < 0 || end > 512 * 1024 || fseek(file, 0, SEEK_SET) != 0) {
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

static bool run_webgl_geometry_cache_admission(void)
{
    ScriptWebglGeometryCacheState cache = {0};
    ScriptWebglGeometryCacheSignature first = {{0}}, second = {{0}},
        third = {{0}};
    first.words[0] = 11u;
    second.words[0] = 12u;
    third.words[0] = 13u;
    size_t slot = SIZE_MAX;
    bool hit = true, reset = false;
    CHECK(script_runtime_webgl_geometry_cache_admit(
        &cache, 0u, 7u, 41, &first, 4096u,
        &slot, &hit, &reset));
    CHECK(reset && !hit && slot == 0u && cache.retained_bytes == 4096u);
    CHECK(script_runtime_webgl_geometry_cache_admit(
        &cache, 0u, 7u, 41, &first, 4096u,
        &slot, &hit, &reset));
    CHECK(!reset && hit && slot == 0u);
    CHECK(script_runtime_webgl_geometry_cache_admit(
        &cache, 0u, 7u, 41, &second, 8192u,
        &slot, &hit, &reset));
    CHECK(!reset && !hit && slot == 1u
          && cache.retained_bytes == 12288u);
    CHECK(script_runtime_webgl_geometry_cache_admit(
        &cache, 0u, 7u, 41, &third, 1024u,
        &slot, &hit, &reset));
    CHECK(!reset && !hit && slot == 2u
          && cache.retained_bytes == 13312u);
    ScriptWebglGeometryCacheSignature fourth = {{0}};
    fourth.words[0] = 4u;
    CHECK(script_runtime_webgl_geometry_cache_admit(
        &cache, 0u, 7u, 41, &fourth, 2048u,
        &slot, &hit, &reset));
    CHECK(!reset && !hit && slot == 3u
          && cache.retained_bytes == 15360u);
    /* Immutable scenery, mutable level geometry and two instanced base
       meshes must not evict one another merely by recurring in sequence. */
    CHECK(script_runtime_webgl_geometry_cache_admit(
        &cache, 0u, 7u, 41, &first, 4096u,
        &slot, &hit, &reset));
    CHECK(hit && slot == 0u);
    CHECK(script_runtime_webgl_geometry_cache_admit(
        &cache, 0u, 7u, 41, &second, 8192u,
        &slot, &hit, &reset));
    CHECK(hit && slot == 1u);
    CHECK(script_runtime_webgl_geometry_cache_admit(
        &cache, 0u, 7u, 41, &third, 1024u,
        &slot, &hit, &reset));
    CHECK(hit && slot == 2u);
    CHECK(script_runtime_webgl_geometry_cache_admit(
        &cache, 0u, 7u, 41, &fourth, 2048u,
        &slot, &hit, &reset));
    CHECK(hit && slot == 3u);
    first.words[1] = 99u; /* A buffer-generation change is a new key. */
    CHECK(script_runtime_webgl_geometry_cache_admit(
        &cache, 0u, 7u, 41, &first, 2048u,
        &slot, &hit, &reset));
    CHECK(!hit && slot == 0u && cache.retained_bytes == 13312u);
    CHECK(script_runtime_webgl_geometry_cache_admit(
        &cache, 0u, 8u, 41, &first, 2048u,
        &slot, &hit, &reset));
    CHECK(reset && !hit && slot == 0u && cache.retained_bytes == 2048u);
    CHECK(!script_runtime_webgl_geometry_cache_admit(
        &cache, 0u, 8u, 41, &first,
        SCRIPT_WEBGL_GEOMETRY_CACHE_BYTE_LIMIT + 1u,
        &slot, &hit, &reset));
    return true;
}

static bool run_webgl_temporal_matrix_admission(void)
{
    float previous[16] = {0}, current[16] = {0};
    previous[0] = previous[5] = previous[10] = previous[15] = 1.0f;
    memcpy(current, previous, sizeof(current));
    bool moved = true;
    CHECK(script_runtime_webgl_temporal_matrix_admit(
        previous, current, &moved) && !moved);
    current[12] = 0.08f;
    current[1] = 0.01f;
    CHECK(script_runtime_webgl_temporal_matrix_admit(
        previous, current, &moved) && moved);
    current[12] = 0.21f;
    CHECK(!script_runtime_webgl_temporal_matrix_admit(
        previous, current, &moved) && !moved);
    current[12] = 0.0f;
    current[3] = NAN;
    CHECK(!script_runtime_webgl_temporal_matrix_admit(
        previous, current, &moved) && !moved);
    CHECK(script_runtime_webgl_temporal_geometry_admit(false, false));
    CHECK(!script_runtime_webgl_temporal_geometry_admit(true, false));
    CHECK(!script_runtime_webgl_temporal_geometry_admit(false, true));
    return true;
}

static bool run_webgl_depth_readback_policy(void)
{
    CHECK(!script_runtime_webgl_depth_readback_required(
        false, UINT32_C(0x00000100)));
    CHECK(script_runtime_webgl_depth_readback_required(
        true, UINT32_C(0x00000100)));
    CHECK(script_runtime_webgl_depth_readback_required(
        false, UINT32_C(0x00004000)));
    CHECK(script_runtime_webgl_depth_readback_required(false, 0u));
    return true;
}

static bool run_webgl_antialias_edge_policy(void)
{
    CHECK(script_runtime_webgl_antialias_edges_admit(0u, 512u));
    CHECK(!script_runtime_webgl_antialias_edges_admit(0u, 513u));
    CHECK(!script_runtime_webgl_antialias_edges_admit(500u, 13u));
    CHECK(script_runtime_webgl_antialias_edges_admit(384u, 48u));
    /* An oversized earlier mesh consumes nothing, so a later compact draw
       remains eligible instead of losing AA with the large one. */
    size_t admitted = 0u;
    if (script_runtime_webgl_antialias_edges_admit(admitted, 3072u))
        admitted += 3072u;
    CHECK(admitted == 0u);
    CHECK(script_runtime_webgl_antialias_edges_admit(admitted, 48u));
    /* Tiny moving instances must not alternate between a filled primitive
       and a line-smoothed fringe around the old two-pixel boundary. The
       three-pixel threshold retains AA for the primary ball/control shapes. */
    CHECK(!script_runtime_webgl_antialias_radius_admit(3.999f));
    CHECK(!script_runtime_webgl_antialias_radius_admit(4.001f));
    CHECK(!script_runtime_webgl_antialias_radius_admit(8.999f));
    CHECK(script_runtime_webgl_antialias_radius_admit(9.0f));
    CHECK(script_runtime_webgl_antialias_radius_admit(16.0f));
    CHECK(!script_runtime_webgl_antialias_radius_admit(NAN));
    CHECK(!script_runtime_webgl_antialias_radius_admit(INFINITY));
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

    bool static_subdata_replayed = script_runtime_evaluate_diagnostic(
              runtime,
              "const staticRepeatExt=deferredGl.getExtension("
              "'TILEFINCH_repeated_frame_commands');"
              "const staticBuffer=deferredGl.createBuffer();"
              "deferredGl.bindBuffer(deferredGl.ARRAY_BUFFER,staticBuffer);"
              "deferredGl.bufferData(deferredGl.ARRAY_BUFFER,"
              "new Float32Array([-1,-1,1,-1,0,1]),deferredGl.STATIC_DRAW);"
              "deferredGl.vertexAttribPointer(position,2,deferredGl.FLOAT,"
              "false,0,0);deferredGl.enableVertexAttribArray(position);"
              /* Warm the ordinary wire-template/source-packet path before
                 capture, matching a game that begins retaining its stable
                 frame after initial admission. */
              "deferredGl.clearColor(0,0,0,1);"
              "deferredGl.clear(deferredGl.COLOR_BUFFER_BIT);"
              "deferredGl.drawArrays(deferredGl.TRIANGLES,0,3);"
              "deferredGl.finish();staticRepeatExt.begin();"
              "deferredGl.clear(deferredGl.COLOR_BUFFER_BIT);"
              "deferredGl.drawArrays(deferredGl.TRIANGLES,0,3);"
              "const staticList=staticRepeatExt.end(new Uint16Array([3,1]));"
              "deferredGl.finish();"
              "const staticStateRevision=deferredGl._repeatedCommandStateRevision;"
              "const staticStorageRevision=deferredGl._bufferStorageRevision;"
              "const staticSourceEpoch=deferredGl._sourcePacketEpoch;"
              "deferredGl.bufferSubData(deferredGl.ARRAY_BUFFER,0,"
              "new Float32Array([8,8,9,8,8,9]));"
              "const staticQueued=staticRepeatExt.execute("
              "staticList,new Uint16Array([3,1]));deferredGl.finish();"
              "const staticAfter=new Uint8Array(4);"
              "deferredGl.readPixels(2,1,1,1,deferredGl.RGBA,"
              "deferredGl.UNSIGNED_BYTE,staticAfter);"
              "globalThis.pocSummary=staticList&&staticQueued"
              "&&deferredGl._repeatedCommandStateRevision===staticStateRevision"
              "&&staticAfter[0]<20&&staticAfter[1]<20"
              "?'WEBGL-REPEAT-STATIC-SUBDATA':"
              "'WEBGL-REPEAT-STATIC-SUBDATA-FAIL:'"
              "+staticQueued+'|'+Array.from(staticAfter)+'|'"
              "+deferredGl._repeatedCommandStateRevision+'|'"
              "+staticStateRevision+'|'+deferredGl._bufferStorageRevision+'|'"
              "+staticStorageRevision+'|'+deferredGl._sourcePacketEpoch+'|'"
              "+staticSourceEpoch+'|'+deferredGl._sourcePacketRetained",
              "<webgl-repeat-static-subdata>", &result);
    if (!static_subdata_replayed
        || strcmp(result.summary, "WEBGL-REPEAT-STATIC-SUBDATA") != 0)
        fprintf(stderr, "static subdata replay failed: %s (%s)\n",
                result.summary, result.error);
    CHECK(static_subdata_replayed
          && strcmp(result.summary, "WEBGL-REPEAT-STATIC-SUBDATA") == 0);

    bool repeated_count_restored = script_runtime_evaluate_diagnostic(
              runtime,
              "const countExt=deferredGl.getExtension("
              "'TILEFINCH_repeated_frame_commands');"
              "const countIndices=deferredGl.createBuffer();"
              "deferredGl.bindBuffer(deferredGl.ELEMENT_ARRAY_BUFFER,countIndices);"
              "deferredGl.bufferData(deferredGl.ELEMENT_ARRAY_BUFFER,"
              "new Uint8Array([0,1,2,3,4,5]),deferredGl.STATIC_DRAW);"
              "const countVertices=new Float32Array(["
              "-1,-1,0,-1,-.5,1,0,-1,1,-1,.5,1]);"
              "const countBufferUp=deferredGl.createBuffer();"
              "deferredGl.bindBuffer(deferredGl.ARRAY_BUFFER,countBufferUp);"
              "deferredGl.bufferData(deferredGl.ARRAY_BUFFER,countVertices,"
              "deferredGl.STATIC_DRAW);"
              "deferredGl.vertexAttribPointer(position,2,deferredGl.FLOAT,"
              "false,0,0);deferredGl.enableVertexAttribArray(position);"
              "deferredGl.clearColor(0,0,0,1);"
              "deferredGl.clear(deferredGl.COLOR_BUFFER_BIT);"
              "deferredGl.drawElements(deferredGl.TRIANGLES,3,"
              "deferredGl.UNSIGNED_BYTE,0);deferredGl.finish();"
              /* Capture a three-index draw whose bounded replay may expand to
                 six. Replay patches the live wire count, then the ordinary
                 frame deliberately reuses the same plan and template slot. */
              "countExt.begin();deferredGl.clear(deferredGl.COLOR_BUFFER_BIT);"
              "deferredGl.drawElements(deferredGl.TRIANGLES,3,"
              "deferredGl.UNSIGNED_BYTE,0);"
              "const countListUp=countExt.end(new Uint16Array([6,1]));"
              "deferredGl.finish();"
              "const replayUp=countExt.execute(countListUp,"
              "new Uint16Array([6,1]));deferredGl.finish();"
              "const replayUpPixel=new Uint8Array(4);"
              "deferredGl.readPixels(3,1,1,1,deferredGl.RGBA,"
              "deferredGl.UNSIGNED_BYTE,replayUpPixel);"
              "const upHits=__tilefinchWebGLDiagnostics.commandSlotTemplateHits;"
              "deferredGl.clear(deferredGl.COLOR_BUFFER_BIT);"
              "deferredGl.drawElements(deferredGl.TRIANGLES,3,"
              "deferredGl.UNSIGNED_BYTE,0);deferredGl.finish();"
              "const ordinaryUpPixel=new Uint8Array(4);"
              "deferredGl.readPixels(3,1,1,1,deferredGl.RGBA,"
              "deferredGl.UNSIGNED_BYTE,ordinaryUpPixel);"
              "const upTemplateHit="
              "__tilefinchWebGLDiagnostics.commandSlotTemplateHits>upHits;"
              /* Use a fresh position buffer so the mirror case begins with a
                 newly admitted six-index template rather than inheriting the
                 first case's draw plan. */
              "const countBufferDown=deferredGl.createBuffer();"
              "deferredGl.bindBuffer(deferredGl.ARRAY_BUFFER,countBufferDown);"
              "deferredGl.bufferData(deferredGl.ARRAY_BUFFER,countVertices,"
              "deferredGl.STATIC_DRAW);"
              "deferredGl.vertexAttribPointer(position,2,deferredGl.FLOAT,"
              "false,0,0);deferredGl.enableVertexAttribArray(position);"
              "deferredGl.clear(deferredGl.COLOR_BUFFER_BIT);"
              "deferredGl.drawElements(deferredGl.TRIANGLES,6,"
              "deferredGl.UNSIGNED_BYTE,0);deferredGl.finish();"
              "countExt.begin();deferredGl.clear(deferredGl.COLOR_BUFFER_BIT);"
              "deferredGl.drawElements(deferredGl.TRIANGLES,6,"
              "deferredGl.UNSIGNED_BYTE,0);"
              "const countListDown=countExt.end(new Uint16Array([6,1]));"
              "deferredGl.finish();"
              "const replayDown=countExt.execute(countListDown,"
              "new Uint16Array([3,1]));deferredGl.finish();"
              "const replayDownPixel=new Uint8Array(4);"
              "deferredGl.readPixels(3,1,1,1,deferredGl.RGBA,"
              "deferredGl.UNSIGNED_BYTE,replayDownPixel);"
              "const downHits=__tilefinchWebGLDiagnostics.commandSlotTemplateHits;"
              "deferredGl.clear(deferredGl.COLOR_BUFFER_BIT);"
              "deferredGl.drawElements(deferredGl.TRIANGLES,6,"
              "deferredGl.UNSIGNED_BYTE,0);deferredGl.finish();"
              "const ordinaryDownPixel=new Uint8Array(4);"
              "deferredGl.readPixels(3,1,1,1,deferredGl.RGBA,"
              "deferredGl.UNSIGNED_BYTE,ordinaryDownPixel);"
              "const downTemplateHit="
              "__tilefinchWebGLDiagnostics.commandSlotTemplateHits>downHits;"
              "deferredGl.bindBuffer(deferredGl.ELEMENT_ARRAY_BUFFER,null);"
              "globalThis.pocSummary=countListUp&&countListDown"
              "&&replayUp&&replayDown&&upTemplateHit&&downTemplateHit"
              "&&replayUpPixel[0]>200&&ordinaryUpPixel[0]<20"
              "&&replayDownPixel[0]<20&&ordinaryDownPixel[0]>200"
              "?'WEBGL-REPEAT-COUNT-RESTORED':"
              "'WEBGL-REPEAT-COUNT-FAIL:'+replayUp+'|'"
              "+Array.from(replayUpPixel)+'|'+Array.from(ordinaryUpPixel)"
              "+'|'+replayDown+'|'+Array.from(replayDownPixel)+'|'"
              "+Array.from(ordinaryDownPixel)+'|'"
              "+upTemplateHit+'|'+downTemplateHit+'|'"
              "+deferredGl.getError()",
              "<webgl-repeat-count-restored>", &result);
    if (!repeated_count_restored
        || strcmp(result.summary, "WEBGL-REPEAT-COUNT-RESTORED") != 0)
        fprintf(stderr, "repeat count restore failed: %s (%s)\n",
                result.summary, result.error);
    CHECK(repeated_count_restored
          && strcmp(result.summary, "WEBGL-REPEAT-COUNT-RESTORED") == 0);

    bool repeated_state_hardened = script_runtime_evaluate_diagnostic(
              runtime,
              "const stateExt=deferredGl.getExtension("
              "'TILEFINCH_repeated_frame_commands');"
              "const stateQuad=deferredGl.createBuffer();"
              "deferredGl.bindBuffer(deferredGl.ELEMENT_ARRAY_BUFFER,null);"
              "deferredGl.bindBuffer(deferredGl.ARRAY_BUFFER,stateQuad);"
              "deferredGl.bufferData(deferredGl.ARRAY_BUFFER,new Float32Array(["
              "-1,-1,1,-1,-1,1,-1,1,1,-1,1,1]),deferredGl.STATIC_DRAW);"
              "deferredGl.useProgram(program);"
              "deferredGl.vertexAttribPointer(position,2,deferredGl.FLOAT,"
              "false,0,0);deferredGl.enableVertexAttribArray(position);"
              "deferredGl.uniform4f(deferredGl.getUniformLocation(program,'uColor'),"
              "1,0,0,1);deferredGl.viewport(0,0,4,4);"
              "deferredGl.clearColor(0,0,0,1);"
              "deferredGl.clear(deferredGl.COLOR_BUFFER_BIT);"
              "deferredGl.drawArrays(deferredGl.TRIANGLES,0,6);"
              "deferredGl.finish();"
              "stateExt.begin();deferredGl.clear(deferredGl.COLOR_BUFFER_BIT);"
              "deferredGl.drawArrays(deferredGl.TRIANGLES,0,6);"
              "const clearStateList=stateExt.end(new Uint16Array([6,1]));"
              "deferredGl.finish();"
              "const unchangedStateReplayed=!!clearStateList&&stateExt.execute("
              "clearStateList,new Uint16Array([6,1]));deferredGl.finish();"
              "deferredGl.clearColor(0,0,1,1);"
              "const changedClearRefused=!stateExt.execute(clearStateList,"
              "new Uint16Array([6,1]));deferredGl.finish();"
              "deferredGl.clear(deferredGl.COLOR_BUFFER_BIT);deferredGl.finish();"
              "const clearPixel=new Uint8Array(4);"
              "deferredGl.readPixels(0,0,1,1,deferredGl.RGBA,"
              "deferredGl.UNSIGNED_BYTE,clearPixel);"
              "deferredGl.clearColor(0,0,0,1);deferredGl.viewport(0,0,4,4);"
              "stateExt.begin();deferredGl.clear(deferredGl.COLOR_BUFFER_BIT);"
              "deferredGl.drawArrays(deferredGl.TRIANGLES,0,6);"
              "const viewportStateList=stateExt.end(new Uint16Array([6,1]));"
              "deferredGl.finish();deferredGl.viewport(0,0,2,4);"
              "const changedViewportRefused=!stateExt.execute(viewportStateList,"
              "new Uint16Array([6,1]));deferredGl.finish();"
              "deferredGl.clear(deferredGl.COLOR_BUFFER_BIT);"
              "deferredGl.drawArrays(deferredGl.TRIANGLES,0,6);"
              "deferredGl.finish();const viewportInside=new Uint8Array(4);"
              "const viewportOutside=new Uint8Array(4);"
              "deferredGl.readPixels(0,2,1,1,deferredGl.RGBA,"
              "deferredGl.UNSIGNED_BYTE,viewportInside);"
              "deferredGl.readPixels(3,2,1,1,deferredGl.RGBA,"
              "deferredGl.UNSIGNED_BYTE,viewportOutside);"
              "deferredGl.viewport(0,0,4,4);"
              "const matrixProgram=deferredGl.createProgram();"
              "deferredGl.attachShader(matrixProgram,shader(deferredGl.VERTEX_SHADER,"
              "'attribute vec2 aPosition;uniform mat4 uMatrix;void main(){'"
              "+'gl_Position=uMatrix*vec4(aPosition,0.,1.);}'));"
              "deferredGl.attachShader(matrixProgram,shader("
              "deferredGl.FRAGMENT_SHADER,"
              "'void main(){gl_FragColor=vec4(1.,0.,0.,1.);}'));"
              "deferredGl.linkProgram(matrixProgram);"
              "deferredGl.useProgram(matrixProgram);"
              "const matrixPosition=deferredGl.getAttribLocation("
              "matrixProgram,'aPosition');"
              "deferredGl.bindBuffer(deferredGl.ARRAY_BUFFER,stateQuad);"
              "deferredGl.vertexAttribPointer(matrixPosition,2,deferredGl.FLOAT,"
              "false,0,0);deferredGl.enableVertexAttribArray(matrixPosition);"
              "const matrixLocation=deferredGl.getUniformLocation("
              "matrixProgram,'uMatrix');const identity=new Float32Array(["
              "1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]);"
              "const translated=new Float32Array(identity);translated[12]=.25;"
              "deferredGl.uniformMatrix4fv(matrixLocation,false,identity);"
              "deferredGl.clear(deferredGl.COLOR_BUFFER_BIT);"
              "deferredGl.drawArrays(deferredGl.TRIANGLES,0,6);"
              "deferredGl.finish();stateExt.begin();"
              "deferredGl.clear(deferredGl.COLOR_BUFFER_BIT);"
              "deferredGl.drawArrays(deferredGl.TRIANGLES,0,6);"
              "deferredGl.uniformMatrix4fv(matrixLocation,false,translated);"
              "deferredGl.drawArrays(deferredGl.TRIANGLES,0,6);"
              "const variedUniformList=stateExt.end();deferredGl.finish();"
              "deferredGl.uniformMatrix4fv(matrixLocation,false,identity);"
              "stateExt.begin();deferredGl.clear(deferredGl.COLOR_BUFFER_BIT);"
              "deferredGl.drawArrays(deferredGl.TRIANGLES,0,6);"
              "deferredGl.drawArrays(deferredGl.TRIANGLES,0,6);"
              "const stableUniformList=stateExt.end();deferredGl.finish();"
              "const stableUniformReplayed=!!stableUniformList&&stateExt.execute("
              "stableUniformList,new Uint16Array([6,1,6,1]));"
              "deferredGl.finish();"
              "const tintProgram=deferredGl.createProgram();"
              "deferredGl.attachShader(tintProgram,shader(deferredGl.VERTEX_SHADER,"
              "'attribute vec2 aPosition;attribute vec4 aColor;varying vec4 vColor;'"
              "+'void main(){vColor=aColor;gl_Position=vec4(aPosition,0.,1.);}'));"
              "deferredGl.attachShader(tintProgram,shader("
              "deferredGl.FRAGMENT_SHADER,"
              "'uniform vec4 uColor;varying vec4 vColor;'"
              "+'void main(){gl_FragColor=uColor*vColor;}'));"
              "deferredGl.linkProgram(tintProgram);deferredGl.useProgram(tintProgram);"
              "const tintPosition=deferredGl.getAttribLocation("
              "tintProgram,'aPosition');const tintColor=deferredGl.getAttribLocation("
              "tintProgram,'aColor');deferredGl.bindBuffer("
              "deferredGl.ARRAY_BUFFER,stateQuad);"
              "deferredGl.vertexAttribPointer(tintPosition,2,deferredGl.FLOAT,"
              "false,0,0);deferredGl.enableVertexAttribArray(tintPosition);"
              "deferredGl.disableVertexAttribArray(tintColor);"
              "deferredGl.vertexAttrib4f(tintColor,.5,.25,1,1);"
              "deferredGl.uniform4f(deferredGl.getUniformLocation("
              "tintProgram,'uColor'),1,1,1,1);"
              "deferredGl.clear(deferredGl.COLOR_BUFFER_BIT);"
              "deferredGl.drawArrays(deferredGl.TRIANGLES,0,6);"
              "deferredGl.finish();stateExt.begin();"
              "deferredGl.clear(deferredGl.COLOR_BUFFER_BIT);"
              "deferredGl.drawArrays(deferredGl.TRIANGLES,0,6);"
              "const tintList=stateExt.end(new Uint16Array([6,1]));"
              "deferredGl.finish();const ordinaryTint=new Uint8Array(4);"
              "deferredGl.readPixels(2,2,1,1,deferredGl.RGBA,"
              "deferredGl.UNSIGNED_BYTE,ordinaryTint);"
              "const tintReplayed=!!tintList&&stateExt.execute("
              "tintList,new Uint16Array([6,1]));deferredGl.finish();"
              "const replayTint=new Uint8Array(4);"
              "deferredGl.readPixels(2,2,1,1,deferredGl.RGBA,"
              "deferredGl.UNSIGNED_BYTE,replayTint);"
              "const tintMatches=tintReplayed&&ordinaryTint.every("
              "(value,index)=>Math.abs(value-replayTint[index])<=1);"
              "deferredGl.useProgram(program);"
              "deferredGl.bindBuffer(deferredGl.ARRAY_BUFFER,buffer);"
              "deferredGl.vertexAttribPointer(position,2,deferredGl.FLOAT,"
              "false,0,0);deferredGl.enableVertexAttribArray(position);"
              "deferredGl.uniform4f(deferredGl.getUniformLocation(program,'uColor'),"
              "1,0,0,1);deferredGl.clearColor(0,0,0,1);"
              "globalThis.pocSummary=unchangedStateReplayed"
              "&&changedClearRefused&&clearPixel[2]>200"
              "&&changedViewportRefused&&viewportInside[0]>200"
              "&&viewportOutside[0]<20&&variedUniformList===null"
              "&&stableUniformReplayed&&ordinaryTint[0]>120"
              "&&ordinaryTint[1]>55&&ordinaryTint[2]>245&&tintMatches"
              "?'WEBGL-REPEAT-STATE-HARDENED':"
              "'WEBGL-REPEAT-STATE-FAIL:'+unchangedStateReplayed+'|'"
              "+changedClearRefused+'|'+Array.from(clearPixel)+'|'"
              "+changedViewportRefused+'|'+Array.from(viewportInside)+'|'"
              "+Array.from(viewportOutside)+'|'+(variedUniformList===null)+'|'"
              "+stableUniformReplayed+'|'+Array.from(ordinaryTint)+'|'"
              "+Array.from(replayTint)+'|'+tintMatches+'|'"
              "+deferredGl.getError()",
              "<webgl-repeat-state-hardened>", &result);
    if (!repeated_state_hardened
        || strcmp(result.summary, "WEBGL-REPEAT-STATE-HARDENED") != 0)
        fprintf(stderr, "repeat state hardening failed: %s (%s)\n",
                result.summary, result.error);
    CHECK(repeated_state_hardened
          && strcmp(result.summary, "WEBGL-REPEAT-STATE-HARDENED") == 0);

    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.deferredRepeatExt=deferredGl.getExtension("
              "'TILEFINCH_repeated_frame_commands');"
              "deferredGl.bindBuffer(deferredGl.ARRAY_BUFFER,buffer);"
              "deferredGl.bufferSubData(deferredGl.ARRAY_BUFFER,0,"
              "new Float32Array([-1,-1,1,-1,0,1]));"
              "deferredRepeatExt.begin();"
              "deferredGl.clear(deferredGl.COLOR_BUFFER_BIT);"
              "deferredGl.drawArrays(deferredGl.TRIANGLES,0,3);"
              "globalThis.deferredRepeatList=deferredRepeatExt.end();"
              "deferredGl.finish();"
              "globalThis.deferredRepeatCounts=new Uint16Array([3,1]);"
              "globalThis.deferredRepeatRevision="
              "deferredGl._bufferStorageRevision;"
              "globalThis.pocSummary=deferredRepeatList"
              "&&deferredRepeatList.drawCount===1"
              "?'WEBGL-REPEAT-CAPTURED':'WEBGL-REPEAT-CAPTURE-FAIL'",
              "<webgl-repeat-captured>", &result)
          && strcmp(result.summary, "WEBGL-REPEAT-CAPTURED") == 0);
    script_runtime_set_images(runtime, NULL);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "const queued=deferredRepeatExt.execute("
              "deferredRepeatList,deferredRepeatCounts);"
              "deferredGl.finish();"
              "deferredGl.bufferSubData(deferredGl.ARRAY_BUFFER,0,"
              "new Float32Array([8,8,9,8,8,9]));"
              "globalThis.pocSummary=queued"
              "&&deferredGl._bufferStorageRevision!==deferredRepeatRevision"
              "?'WEBGL-REPEAT-STORAGE-REPLACED':"
              "'WEBGL-REPEAT-STORAGE-FAIL'",
              "<webgl-repeat-storage-replaced>", &result)
          && strcmp(result.summary,
                    "WEBGL-REPEAT-STORAGE-REPLACED") == 0);
    script_runtime_set_images(runtime, &relocated_images);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "deferredGl.finish();"
              "globalThis.pocSummary=!deferredRepeatExt.execute("
              "deferredRepeatList,deferredRepeatCounts)"
              "?'WEBGL-REPEAT-STALE-REFUSED':"
              "'WEBGL-REPEAT-STALE-ACCEPTED'",
              "<webgl-repeat-stale-refused>", &result)
          && strcmp(result.summary,
                    "WEBGL-REPEAT-STALE-REFUSED") == 0);

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
    /* This bundled PSP game must remain legible before optional glyph packs
       or page fonts are available. Its authored UI labels therefore use the
       embedded ASCII vocabulary rather than rendering replacement tiles. */
    for (size_t at = 0; at < script_length; at++)
        CHECK((unsigned char) script[at] < 0x80u);

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
              "&&startSnapshot.brickInstances===startSnapshot.bricks"
              "&&startSnapshot.staticVertices===0"
              "&&startSnapshot.staticIndices<=4608"
              "&&startSnapshot.vertices<=3072"
              "&&startSnapshot.indices<=4608&&startSnapshot.meshDrops===0"
              "?'PRISM-STARTED':'PRISM-START-FAIL'",
              "<prism-start>", &result)
          && strcmp(result.summary, "PRISM-STARTED") == 0);

    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "const collisionBefore=__prismBreakDebug.snapshot();"
              "__prismBreakDebug.strikeBrick(0);"
              "const collisionAfter=__prismBreakDebug.snapshot();"
              "globalThis.pocSummary="
              "collisionAfter.bricks===collisionBefore.bricks-1"
              "&&collisionAfter.brickInstances===collisionBefore.brickInstances"
              "&&collisionAfter.brickInstanceUploads"
              "===collisionBefore.brickInstanceUploads+1"
              "&&collisionAfter.brickInstanceFloatsUploaded"
              "===collisionBefore.brickInstanceFloatsUploaded+16"
              "&&collisionAfter.staticVertices===0"
              "?'PRISM-BRICK-PATCHED':'PRISM-BRICK-REBUILT:'"
              "+JSON.stringify({collisionBefore,collisionAfter})",
              "<prism-brick-collision>", &result)
          && strcmp(result.summary, "PRISM-BRICK-PATCHED") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "__prismBreakDebug.clearLevel();__prismBreakDebug.step(1);"
              "const damageBefore=__prismBreakDebug.snapshot();"
              "__prismBreakDebug.strikeBrick(1);"
              "const damageAfter=__prismBreakDebug.snapshot();"
              "globalThis.pocSummary="
              "damageAfter.bricks===damageBefore.bricks"
              "&&damageAfter.brickInstanceUploads"
              "===damageBefore.brickInstanceUploads+1"
              "&&damageAfter.brickInstanceFloatsUploaded"
              "===damageBefore.brickInstanceFloatsUploaded+4"
              "?'PRISM-BRICK-TINT-PATCHED':'PRISM-BRICK-TINT-REBUILT:'"
              "+JSON.stringify({damageBefore,damageAfter})",
              "<prism-brick-damage>", &result)
          && strcmp(result.summary, "PRISM-BRICK-TINT-PATCHED") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "__prismBreakDebug.start();__prismBreakDebug.step(0);"
              "globalThis.pocSummary='PRISM-RESTARTED'",
              "<prism-restart-after-collision>", &result)
          && strcmp(result.summary, "PRISM-RESTARTED") == 0);

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
              "__prismBreakDebug.qualify('heavy');"
              "__prismBreakDebug.step(30);"
              "const heavySnapshot=__prismBreakDebug.snapshot();"
              "globalThis.pocSummary=heavySnapshot.mode==='playing'"
              "&&heavySnapshot.balls===3&&heavySnapshot.movingBalls===3"
              "&&heavySnapshot.particles>=32&&heavySnapshot.particles<=48"
              "&&heavySnapshot.vertices<=3072&&heavySnapshot.indices<=4608"
              "&&heavySnapshot.meshDrops===0"
              "&&__tilefinchWebGLDiagnostics.instancedDrawCalls>0"
              "&&__tilefinchWebGLDiagnostics.instances>0"
              "?'PRISM-HEAVY':'PRISM-HEAVY-FAIL:'"
              "+JSON.stringify(heavySnapshot)",
              "<prism-heavy-qualification>", &result)
          && strcmp(result.summary, "PRISM-HEAVY") == 0);
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

static bool run_treadline_arena_game(void)
{
    static const char document_url[] =
        "https://games.test/examples/treadline-arena/index.html";
    size_t html_length = 0, script_length = 0, css_length = 0;
    size_t manifest_length = 0, icon_length = 0, web_multiplayer_length = 0;
    size_t arena_generator_length = 0;
    char *html = read_source(
        "examples/treadline-arena/index.html", &html_length);
    char *script = read_source(
        "examples/treadline-arena/game.js", &script_length);
    char *css = read_source(
        "examples/treadline-arena/game.css", &css_length);
    char *manifest_json = read_source(
        "examples/treadline-arena/manifest.webmanifest", &manifest_length);
    char *icon = read_source(
        "examples/treadline-arena/icon.svg", &icon_length);
    char *web_multiplayer = read_source(
        "examples/treadline-arena/multiplayer-web.js",
        &web_multiplayer_length);
    char *arena_generator = read_source(
        "examples/treadline-arena/arena-generator.js",
        &arena_generator_length);
    CHECK(html != NULL && script != NULL && css != NULL
          && manifest_json != NULL && icon != NULL && web_multiplayer != NULL
          && arena_generator != NULL
          && icon_length > 64u && strstr(icon, "<svg") != NULL
          && web_multiplayer_length > 4096u
          && web_multiplayer_length < 24u * 1024u
          && strstr(html, "multiplayer-web.js") != NULL
          && strstr(html, "arena-generator.js") != NULL
          /* The first authored menu must already have the initialized PSP
             copy and stable line boxes. A 700-weight face would be loaded by
             idle work and move the centered panel after its first paint. */
          && strstr(html, "Clear three arenas before your last redeploy.")
             != NULL
          && strstr(html, "Arcade: nub moves | face buttons aim") != NULL
          && strstr(html, "play.textContent = \"Starting...\"") != NULL
          && strstr(html,
                    "<script defer src=\"arena-generator.js\"></script>")
             != NULL
          && strstr(html, "<script defer src=\"game.js\"></script>")
             != NULL
          && strstr(html, "setTimeout(function ()") == NULL
          && strstr(html, "createElement(\"script\")") == NULL
          && strstr(html, "addEventListener(\"load\"") == NULL
          && strstr(css, "font-weight: bold") == NULL
          && strstr(css, "font: bold") == NULL
          && strstr(css, "font-weight: 600") != NULL
          && strstr(css, ".controls") != NULL
          && strstr(css, "min-height: 20px") != NULL
          && strstr(web_multiplayer, "manual-offer-answer") != NULL
          && strstr(web_multiplayer, "maxRetransmits: 0") != NULL
          && strstr(web_multiplayer, "TFW1J.") != NULL
          && strstr(web_multiplayer, "TFW1Z.") != NULL
          && strstr(script, "options.iceServers = []") != NULL
          && strstr(script, "buildArena(queuedDeploy)") != NULL
          /* The engine's real ceiling is the 1 MiB aggregate capture
             limit; this per-script bound only guards against runaway
             growth and keeps authoring headroom explicit. */
          && script_length < 384u * 1024u
          && arena_generator_length < 64u * 1024u
          && html_length + script_length + css_length + manifest_length
             + icon_length + web_multiplayer_length + arena_generator_length
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
              "https://games.test/examples/treadline-arena/manifest.webmanifest",
              document_url, &manifest, manifest_error,
              sizeof(manifest_error))
          && strcmp(manifest.name, "Treadline Arena") == 0
          && strcmp(manifest.short_name, "Treadline") == 0
          && strcmp(manifest.start_url, document_url) == 0
          && strcmp(manifest.scope,
                    "https://games.test/examples/treadline-arena/") == 0
          && strcmp(manifest.icon_url,
                    "https://games.test/examples/treadline-arena/icon.svg") == 0
          && manifest.icon_type[0] == '\0'
          && manifest.icon_size == 96u
          && manifest.display_mode == TILEFINCH_WEB_APP_DISPLAY_FULLSCREEN
          && manifest.theme_color_valid
          && manifest.theme_color == UINT32_C(0x102b38));

    ViewportContext viewport;
    CHECK(viewport_context_init(&viewport, 480, 272, 480, 272));
    ScriptExecutionPolicy policy;
    CHECK(script_execution_policy_for_profile(
        SCRIPT_EXECUTION_PROFILE_PSP_REALISTIC, &policy));
    /* This first-party offline game has its own bounded 384 KiB authored
       script envelope, checked above and admitted by the device package.
       Keep the isolated runtime aligned with that explicit game bound rather
       than the generic per-document compile default. */
    policy.maximum_host_compile_source_bytes = 384u * 1024u;
    policy.slow_compile_threshold_us = UINT64_MAX;
    policy.slow_callback_threshold_us = UINT64_MAX;
    ScriptRuntimeOptions options = {
        .viewport = viewport,
        .execution_policy = policy,
        .defer_document_scripts = true,
        .allow_test_network_primitive_overrides = true
    };
    ScriptResult result;
    char generated_online_fixture[2048] = {0};
    ScriptRuntime *runtime = script_runtime_create_configured(
        &document, &budget, 16u * MIB, 8000, document_url,
        &options, &result);
    CHECK(runtime != NULL && result.success);
    ImageResources images = {.budget = &budget};
    script_runtime_set_images(runtime, &images);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.__treadlineOnlineTest={sentCount:0,sentFirst:null,"
              "sentLast:null,accepted:false};"
              "globalThis.__treadlineEnableDebug=true;"
              "__tilefinchMultiplayerStart=(mode,game,name,code)=>"
              "(__treadlineOnlineTest.open=[mode,game,name,code],71);"
              "__tilefinchMultiplayerSend=(id,data,binary)=>{"
              "const copy=data.slice(0);"
              "if(!__treadlineOnlineTest.sentFirst)"
              "__treadlineOnlineTest.sentFirst=copy;"
              "__treadlineOnlineTest.sentLast=copy;"
              "__treadlineOnlineTest.sentCount++;"
              "return id===71&&binary;};"
              "__tilefinchMultiplayerAccept=(id,peer,accepted)=>"
              "(__treadlineOnlineTest.accepted=id===71&&peer===99&&accepted,true);"
              "__tilefinchMultiplayerAddRemoteCode=()=>true;"
              "__tilefinchMultiplayerClose=()=>true;"
              "globalThis.pocSummary='TREADLINE-ONLINE-SEAM';",
              "<treadline-online-seam>", &result)
          && strcmp(result.summary, "TREADLINE-ONLINE-SEAM") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, arena_generator, "treadline-arena/arena-generator.js",
              &result));
    bool treadline_loaded = script_runtime_evaluate_diagnostic(
        runtime, script, "treadline-arena/game.js", &result);
    if (!treadline_loaded || strcmp(result.summary, "TREADLINE-READY") != 0)
        fprintf(stderr, "treadline load summary=%s error=%s\n",
                result.summary, result.error);
    CHECK(treadline_loaded && strcmp(result.summary, "TREADLINE-READY") == 0);
    bool treadline_arena_generation_ok = script_runtime_evaluate_diagnostic(
              runtime,
              "const authoredMasks=["
              "__treadlineDebug.arenaValidationMask(0,0),"
              "__treadlineDebug.arenaValidationMask(1,1),"
              "__treadlineDebug.arenaValidationMask(2,2)];"
              "const arenaBytes=arena=>{const out=new Float32Array(104);let n=0;"
              "for(const list of [arena.obstacles,arena.barriers,arena.ramps,"
              "arena.gates])for(const item of list)"
              "for(let field=0;field<item.length;field++)out[n++]=item[field];"
              "return new Uint8Array(out.buffer)};"
              "const firstSeed=__treadlineDebug.generateArenaSeed(0x12345678);"
              "const firstWinner=__treadlineDebug.snapshot().arenaSeed;"
              "const firstBytes=arenaBytes(__treadlineArenaData.generatedArena);"
              "__treadlineDebug.beginArenaGeneration(0x12345678);"
              "let steppedResult=null;for(let slice=0;slice<140;slice++){"
              "steppedResult=__treadlineDebug.stepArenaGeneration(2);"
              "if(steppedResult.done)break;}steppedResult={...steppedResult};"
              "const steppedBytes=arenaBytes(__treadlineArenaData.generatedArena);"
              "const secondSeed=__treadlineDebug.generateArenaSeed(0x12345678);"
              "const secondWinner=__treadlineDebug.snapshot().arenaSeed;"
              "const secondBytes=arenaBytes(__treadlineArenaData.generatedArena);"
              "const clientArena={generated:true,"
              "obstacles:Array.from({length:16},()=>new Float32Array(4)),"
              "barriers:Array.from({length:6},()=>new Float32Array(4)),"
              "ramps:Array.from({length:2},()=>new Float32Array(6)),"
              "gates:[new Float32Array(4)],obstacleCount:0,barrierCount:0,"
              "rampCount:0,gateCount:1,enemies:5};"
              "const clientArenas=__treadlineArenaData.arenas.slice(0,3);"
              "clientArenas.push(clientArena);"
              "const clientGenerator=__treadlineCreateArenaGenerator("
              "clientArenas,clientArena);"
              "const clientChecksum=clientGenerator.materialize(firstWinner);"
              "const clientBytes=arenaBytes(clientArena);"
              "const shared=__treadlineDebug.generateArenaSeed(firstWinner);"
              "const sharedWinner=__treadlineDebug.snapshot().arenaSeed;"
              "const sharedBytes=arenaBytes(__treadlineArenaData.generatedArena);"
              "let byteEqual=true;"
              "for(let at=0;at<firstBytes.length;at++)"
              "if(firstBytes[at]!==secondBytes[at]"
              "||firstBytes[at]!==clientBytes[at]"
              "||firstBytes[at]!==steppedBytes[at]"
              "||firstBytes[at]!==sharedBytes[at]){byteEqual=false;break;}"
              "const fallbackGenerator=__treadlineCreateArenaGenerator("
              "__treadlineArenaData.arenas,__treadlineArenaData.generatedArena);"
              "const forcedFallback={...fallbackGenerator.generate(9,0)};"
              "fallbackGenerator.beginGeneration(9,0);"
              "const steppedFallback={...fallbackGenerator.stepGeneration(2)};"
              "const fallbackValid=fallbackGenerator.validate(3,3).accepted;"
              "const batch=__treadlineDebug.generatedArenaProbe(100);"
              "const variety=new Set();let rawAccepted=0;"
              "for(let seed=1;seed<=64;seed++){const generated="
              "fallbackGenerator.generate(Math.imul(seed,0x45d9f3b)>>>0);"
              "variety.add(generated.checksum);}"
              "for(let seed=1;seed<=192;seed++)"
              "if(fallbackGenerator.generate("
              "Math.imul(seed,0x27d4eb2d)>>>0,1).accepted)rawAccepted++;"
              "const elevation=__treadlineDebug.elevationBarrierProbe();"
              "const good=authoredMasks.every(mask=>mask===63)"
              "&&firstWinner===secondWinner&&firstWinner!==0"
              "&&steppedResult.done&&steppedResult.accepted===firstSeed.accepted"
              "&&steppedResult.attempts===firstSeed.attempts"
              "&&steppedResult.winningSeed===firstWinner"
              "&&steppedResult.checksum===firstSeed.checksum"
              "&&shared.accepted&&sharedWinner===firstWinner"
              "&&firstSeed.checksum===secondSeed.checksum"
              "&&clientChecksum===firstSeed.checksum&&byteEqual"
              "&&firstSeed.gameplayUnchanged&&secondSeed.gameplayUnchanged"
              "&&firstSeed.storageStable&&secondSeed.storageStable"
              "&&batch.accepted+batch.fallbacks===100"
              "&&batch.maximumAttempts<=20&&batch.obstacleCount<=16"
              "&&batch.barrierCount<=6&&batch.rampCount<=2"
              "&&variety.size>=32&&rawAccepted>=64"
              "&&batch.gameplayUnchanged&&batch.storageStable"
              "&&forcedFallback.fallback&&forcedFallback.attempts===0"
              "&&steppedFallback.done&&steppedFallback.fallback"
              "&&steppedFallback.attempts===0"
              "&&steppedFallback.checksum===forcedFallback.checksum"
              "&&fallbackValid&&elevation;"
              "globalThis.pocSummary=good?'TREADLINE-ARENAS'"
              ":'TREADLINE-ARENAS-FAIL:'+JSON.stringify({authoredMasks,"
              "firstSeed,secondSeed,steppedResult,byteEqual,forcedFallback,"
              "steppedFallback,fallbackValid,"
              "batch,variety:variety.size,rawAccepted,elevation,metrics:["
              "{...__treadlineDebug.arenaValidationMetrics(0,0)},"
              "{...__treadlineDebug.arenaValidationMetrics(1,1)},"
              "{...__treadlineDebug.arenaValidationMetrics(2,2)}]})",
              "<treadline-arena-generation>", &result);
    if (!treadline_arena_generation_ok
        || strcmp(result.summary, "TREADLINE-ARENAS") != 0)
        fprintf(stderr, "treadline arenas summary=%s error=%s\n",
                result.summary, result.error);
    CHECK(treadline_arena_generation_ok
          && strcmp(result.summary, "TREADLINE-ARENAS") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "__treadlineDebug.setMusicEnabled(true);"
              "const titleAudio=__treadlineDebug.snapshot();"
              "__treadlineDebug.start();__treadlineDebug.stepSimulation(2);"
              "const playingAudio=__treadlineDebug.snapshot();"
              "__treadlineDebug.setPaused(true);"
              "__treadlineDebug.stepSimulation(2);"
              "const pausedAudio=__treadlineDebug.snapshot();"
              "__treadlineDebug.setMusicEnabled(false);"
              "const mutedAudio=__treadlineDebug.snapshot();"
              "globalThis.pocSummary=titleAudio.audioRole==='melody'"
              "&&playingAudio.audioRole==='hum'"
              "&&pausedAudio.mode==='paused'"
              "&&pausedAudio.audioRole==='melody'"
              "&&mutedAudio.audioRole==='off'"
              "?'TREADLINE-AUDIO-ROLE':'TREADLINE-AUDIO-ROLE-FAIL:'"
              "+JSON.stringify({titleAudio,playingAudio,pausedAudio,mutedAudio})",
              "<treadline-audio-role>", &result)
          && strcmp(result.summary, "TREADLINE-AUDIO-ROLE") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "const forward=__treadlineDebug.longSoakCommand(20);"
              "const rightArc=__treadlineDebug.longSoakCommand(110);"
              "const leftArc=__treadlineDebug.longSoakCommand(260);"
              "const reverse=__treadlineDebug.longSoakCommand(330);"
              "globalThis.pocSummary=forward.left===1&&forward.right===1"
              "&&!forward.reverse&&rightArc.left<rightArc.right"
              "&&!rightArc.reverse&&leftArc.left>leftArc.right"
              "&&!leftArc.reverse&&reverse.left===1&&reverse.right===1"
              "&&reverse.reverse?'TREADLINE-SOAK-MOVEMENT'"
              ":'TREADLINE-SOAK-MOVEMENT-FAIL'",
              "<treadline-soak-movement>", &result)
          && strcmp(result.summary, "TREADLINE-SOAK-MOVEMENT") == 0);
    lxb_dom_node_t *deploy = find_element_id(
        lxb_dom_interface_node(document.html), "play");
    CHECK(deploy != NULL
          && script_runtime_dispatch_activation_node(runtime, deploy, &result)
          && script_runtime_page_fullscreen_active(runtime));
    bool treadline_started_ok = script_runtime_evaluate_diagnostic(
              runtime,
        "__treadlineDebug.step(8);"
              "const started=__treadlineDebug.snapshot();"
              "globalThis.pocSummary=started.mode==='playing'"
              "&&started.arena===0&&started.enemies===3"
              /* Keep the retained arena below the PSP translated-geometry
                 cache ceiling. Crossing it forces a second native flush and
                 defeats the steady-state cache on every game frame. */
              "&&started.staticIndices>0&&started.staticIndices<=2730"
              /* Large arena planes cross the PSP GE near plane as one unit
                 and can lose half their pixels. Pin the segmented geometry
                 rather than relying on a host rasterizer that clips it. */
              "&&started.arenaPlaneMaxSpan>0"
              "&&started.arenaPlaneMaxSpan<=4.5"
              "&&started.vertices<=4096&&started.indices<=6144"
              "&&started.boxInstances>0"
              "&&started.boxInstances<=started.instanceLimit"
              "&&started.staticIndices+started.boxInstances*36"
              "+started.hudPrimitives*6<=4096"
              "&&__tilefinchWebGLDiagnostics.instancedDrawCalls>0"
              "&&__tilefinchWebGLDiagnostics.drawPlanHits>0"
              "&&__tilefinchWebGLDiagnostics.commandTemplateHits>0"
              "&&__tilefinchWebGLDiagnostics.commandSlotTemplateHits>0"
              "&&__tilefinchWebGLDiagnostics.repeatedCommandListExecutions>0"
              /* Startup may capture once before the first retained HUD
                 publication and once afterward. Counts below that fixed
                 capacity must not create further lists. */
              "&&started.repeatedFrameCaptures<=2"
              "&&started.tankBarrels===started.blueTanks+started.redTanks"
              "&&document.getElementById('hud').hidden"
              "&&document.getElementById('objective-arrow').hidden"
              "&&started.meshDrops===0"
              "&&!started.commandEnabled"
              "?'TREADLINE-STARTED':'TREADLINE-START-FAIL:'"
              "+JSON.stringify(started)",
              "<treadline-start>", &result);
    if (!treadline_started_ok
        || strcmp(result.summary, "TREADLINE-STARTED") != 0)
      fprintf(stderr, "treadline start summary=%s error=%s\n",
              result.summary, result.error);
    CHECK(treadline_started_ok
          && strcmp(result.summary, "TREADLINE-STARTED") == 0);

    puts("test: Treadline Arcade and Classic controls translate into one packet shape");
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "localStorage.removeItem('treadline-settings-v1');"
              "__treadlineDebug.selectControls(1);"
              "__treadlineDebug.selectAimAssist(2);"
              "__treadlineDebug.savePreferences();"
              "const savedControls=JSON.parse(localStorage.getItem("
              "'treadline-settings-v1'));"
              "localStorage.setItem('treadline-settings-v1',"
              "JSON.stringify({controls:77,assist:-4}));"
              "__treadlineDebug.reloadPreferences();"
              "const clampedControls=__treadlineDebug.snapshot();"
              "globalThis.pocSummary=savedControls.controls===1"
              "&&savedControls.assist===2&&clampedControls.controls===0"
              "&&clampedControls.assist===1"
              "&&document.getElementById('controls-value').textContent"
              "==='ARCADE'&&document.getElementById('assist-value').textContent"
              "==='SNAP'?'TREADLINE-CONTROL-PREFERENCES'"
              ":'TREADLINE-CONTROL-PREFERENCES-FAIL:'"
              "+JSON.stringify({savedControls,clampedControls})",
              "<treadline-control-preferences>", &result)
          && strcmp(result.summary, "TREADLINE-CONTROL-PREFERENCES") == 0);

    TilefinchGamepadState treadline_gamepad = {
        .axes = {0, -INT16_MAX, 0, 0},
        .timestamp_ms = 101,
        .connected = true
    };
    CHECK(script_runtime_set_gamepad_state(runtime, &treadline_gamepad));
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "__treadlineDebug.selectControls(0);"
              "__treadlineDebug.selectAimAssist(0);"
              "__treadlineDebug.selectCamera(0);"
              "__treadlineDebug.setTankHeading(0,0);"
              "__treadlineDebug.resetInputProbe();"
              "globalThis.__arcadeForward=__treadlineDebug.pollInput();"
              "globalThis.pocSummary='TREADLINE-ARCADE-FORWARD'",
              "<treadline-arcade-forward>", &result)
          && strcmp(result.summary, "TREADLINE-ARCADE-FORWARD") == 0);
    treadline_gamepad.axes[0] = INT16_MAX;
    treadline_gamepad.axes[1] = 0;
    treadline_gamepad.timestamp_ms++;
    CHECK(script_runtime_set_gamepad_state(runtime, &treadline_gamepad)
          && script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.__arcadeRight=__treadlineDebug.pollInput();"
              "globalThis.pocSummary='TREADLINE-ARCADE-RIGHT'",
              "<treadline-arcade-right>", &result));
    treadline_gamepad.axes[0] = 0;
    treadline_gamepad.axes[1] = INT16_MAX;
    treadline_gamepad.timestamp_ms++;
    CHECK(script_runtime_set_gamepad_state(runtime, &treadline_gamepad)
          && script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.__arcadeReverse=__treadlineDebug.pollInput();"
              "globalThis.pocSummary='TREADLINE-ARCADE-REVERSE'",
              "<treadline-arcade-reverse>", &result));
    treadline_gamepad.axes[0] = 4096;
    treadline_gamepad.axes[1] = 0;
    treadline_gamepad.timestamp_ms++;
    CHECK(script_runtime_set_gamepad_state(runtime, &treadline_gamepad));
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "const arcadeDead=__treadlineDebug.pollInput();"
              "const arcadeForwardCheck=globalThis.__arcadeForward;"
              "const arcadeRightCheck=globalThis.__arcadeRight;"
              "const arcadeReverseCheck=globalThis.__arcadeReverse;"
              "globalThis.pocSummary=arcadeForwardCheck.left===1"
              "&&arcadeForwardCheck.right===1&&!arcadeForwardCheck.reverse"
              "&&arcadeRightCheck.left<arcadeRightCheck.right"
              "&&!arcadeRightCheck.reverse&&arcadeReverseCheck.left===1"
              "&&arcadeReverseCheck.right===1&&arcadeReverseCheck.reverse"
              "&&arcadeDead.left===0&&arcadeDead.right===0"
              "&&!arcadeDead.reverse"
              "?'TREADLINE-ARCADE-MOVE':'TREADLINE-ARCADE-MOVE-FAIL:'"
              "+JSON.stringify({arcadeForwardCheck,arcadeRightCheck,"
              "arcadeReverseCheck,arcadeDead})",
              "<treadline-arcade-move-check>", &result));
    if (strcmp(result.summary, "TREADLINE-ARCADE-MOVE") != 0)
        fprintf(stderr, "treadline arcade move summary=%s error=%s\n",
                result.summary, result.error);
    CHECK(strcmp(result.summary, "TREADLINE-ARCADE-MOVE") == 0);

    treadline_gamepad.axes[0] = treadline_gamepad.axes[1] = 0;
    treadline_gamepad.buttons = UINT32_C(1) << 3u;
    treadline_gamepad.timestamp_ms++;
    CHECK(script_runtime_set_gamepad_state(runtime, &treadline_gamepad)
          && script_runtime_evaluate_diagnostic(
              runtime,
              "__treadlineDebug.resetInputProbe();"
              "globalThis.__aimAway=__treadlineDebug.pollInput();"
              "globalThis.pocSummary='TREADLINE-AIM-AWAY'",
              "<treadline-aim-away>", &result));
    treadline_gamepad.buttons = UINT32_C(1) << 1u;
    treadline_gamepad.timestamp_ms++;
    CHECK(script_runtime_set_gamepad_state(runtime, &treadline_gamepad)
          && script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.__aimRoll=__treadlineDebug.pollInput();"
              "globalThis.pocSummary='TREADLINE-AIM-ROLL'",
              "<treadline-aim-roll>", &result));
    treadline_gamepad.buttons = UINT32_C(1) << 0u;
    treadline_gamepad.timestamp_ms++;
    CHECK(script_runtime_set_gamepad_state(runtime, &treadline_gamepad)
          && script_runtime_evaluate_diagnostic(
              runtime,
              "__treadlineDebug.resetInputProbe();"
              "globalThis.__aimToward=__treadlineDebug.pollInput();"
              "globalThis.pocSummary='TREADLINE-AIM-TOWARD'",
              "<treadline-aim-toward>", &result));
    treadline_gamepad.buttons = UINT32_C(1) << 2u;
    treadline_gamepad.timestamp_ms++;
    CHECK(script_runtime_set_gamepad_state(runtime, &treadline_gamepad)
          && script_runtime_evaluate_diagnostic(
              runtime,
              "__treadlineDebug.resetInputProbe();"
              "globalThis.__aimLeft=__treadlineDebug.pollInput();"
              "globalThis.pocSummary='TREADLINE-AIM-LEFT'",
              "<treadline-aim-left>", &result));
    treadline_gamepad.buttons = (UINT32_C(1) << 4u)
        | (UINT32_C(1) << 5u) | (UINT32_C(1) << 12u)
        | (UINT32_C(1) << 13u);
    treadline_gamepad.timestamp_ms++;
    CHECK(script_runtime_set_gamepad_state(runtime, &treadline_gamepad)
          && script_runtime_evaluate_diagnostic(
              runtime,
              "__treadlineDebug.resetInputProbe();"
              "const arcadeActionsCheck=__treadlineDebug.pollInput();"
              "const arcadeAwayCheck=globalThis.__aimAway;"
              "const arcadeRollCheck=globalThis.__aimRoll;"
              "const arcadeTowardCheck=globalThis.__aimToward;"
              "const arcadeAimLeftCheck=globalThis.__aimLeft;"
              "globalThis.pocSummary=arcadeAwayCheck.aimZ>.99"
              "&&Math.abs(arcadeAwayCheck.aimX)<.01"
              "&&arcadeRollCheck.aimX>.69&&arcadeRollCheck.aimZ>.69"
              "&&arcadeTowardCheck.aimZ<-.99"
              "&&arcadeAimLeftCheck.aimX<-.99&&arcadeActionsCheck.fire"
              "&&arcadeActionsCheck.secondary&&arcadeActionsCheck.gadget"
              "&&arcadeActionsCheck.ultimate"
              "?'TREADLINE-ARCADE-AIM':'TREADLINE-ARCADE-AIM-FAIL:'"
              "+JSON.stringify({arcadeAwayCheck,arcadeRollCheck,"
              "arcadeTowardCheck,arcadeAimLeftCheck,arcadeActionsCheck})",
              "<treadline-arcade-aim-check>", &result)
          && strcmp(result.summary, "TREADLINE-ARCADE-AIM") == 0);

    treadline_gamepad.buttons = (UINT32_C(1) << 0u)
        | (UINT32_C(1) << 1u) | (UINT32_C(1) << 2u)
        | (UINT32_C(1) << 4u)
        | (UINT32_C(1) << 5u);
    treadline_gamepad.axes[0] = INT16_MAX;
    treadline_gamepad.timestamp_ms++;
    CHECK(script_runtime_set_gamepad_state(runtime, &treadline_gamepad)
          && script_runtime_evaluate_diagnostic(
              runtime,
              "__treadlineDebug.selectControls(1);"
              "__treadlineDebug.selectAimAssist(0);"
              "__treadlineDebug.resetInputProbe();"
              "const classicControlsCheck=__treadlineDebug.pollInput();"
              "globalThis.pocSummary=classicControlsCheck.left===1"
              "&&classicControlsCheck.right===1&&classicControlsCheck.reverse"
              "&&classicControlsCheck.fire&&classicControlsCheck.secondary"
              "&&classicControlsCheck.aimX>.99?'TREADLINE-CLASSIC-CONTROLS'"
              ":'TREADLINE-CLASSIC-CONTROLS-FAIL:'"
              "+JSON.stringify(classicControlsCheck)",
              "<treadline-classic-controls>", &result)
          && strcmp(result.summary, "TREADLINE-CLASSIC-CONTROLS") == 0);

    treadline_gamepad.buttons = UINT32_C(1) << 3u;
    treadline_gamepad.axes[0] = treadline_gamepad.axes[1] = 0;
    treadline_gamepad.timestamp_ms++;
    CHECK(script_runtime_set_gamepad_state(runtime, &treadline_gamepad)
          && script_runtime_evaluate_diagnostic(
              runtime,
              "__treadlineDebug.start();__treadlineDebug.freezeBots(true);"
              "for(let id=2;id<6;id++)__treadlineDebug.setTankActive(id,false);"
              "__treadlineDebug.setTankPosition(0,0,0);"
              "__treadlineDebug.setTankHeading(0,0);"
              "__treadlineDebug.setTankPosition(1,Math.sin(Math.PI/9)*4,"
              "Math.cos(Math.PI/9)*4);"
              "__treadlineDebug.selectControls(0);"
              "__treadlineDebug.selectAimAssist(1);"
              "__treadlineDebug.resetInputProbe();"
              "globalThis.__snapInside=__treadlineDebug.pollInput();"
              "globalThis.pocSummary='TREADLINE-SNAP-INSIDE'",
              "<treadline-snap-inside>", &result));
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "__treadlineDebug.setTankPosition(1,Math.sin(Math.PI/4)*4,"
              "Math.cos(Math.PI/4)*4);"
              "__treadlineDebug.selectAimAssist(1);"
              "__treadlineDebug.resetInputProbe();"
              "const snapOutsideCheck=__treadlineDebug.pollInput();"
              "const snapInsideCheck=globalThis.__snapInside;"
              "globalThis.pocSummary=snapInsideCheck.aimX>.15"
              "&&snapInsideCheck.aimZ>.9"
              "&&Math.abs(snapOutsideCheck.aimX)<.01"
              "&&snapOutsideCheck.aimZ>.99"
              "?'TREADLINE-SNAP-ASSIST':'TREADLINE-SNAP-ASSIST-FAIL:'"
              "+JSON.stringify({snapInsideCheck,snapOutsideCheck})",
              "<treadline-snap-assist>", &result)
          && strcmp(result.summary, "TREADLINE-SNAP-ASSIST") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "__treadlineDebug.setTankPosition(1,Math.sin(Math.PI/9)*4,"
              "Math.cos(Math.PI/9)*4);"
              "__treadlineDebug.selectAimAssist(2);"
              "__treadlineDebug.resetInputProbe();"
              "const lockAcquireCheck=__treadlineDebug.pollInput();"
              "__treadlineDebug.setTankPosition(1,Math.sin(Math.PI/3)*4,"
              "Math.cos(Math.PI/3)*4);"
              "const lockTrackCheck=__treadlineDebug.pollInput();"
              "globalThis.pocSummary=lockAcquireCheck.lockTarget===1"
              "&&lockTrackCheck.lockTarget===1&&lockAcquireCheck.aimX>.3"
              "&&lockAcquireCheck.aimX<.4&&lockTrackCheck.aimX>.8"
              "?'TREADLINE-LOCK-ASSIST':'TREADLINE-LOCK-ASSIST-FAIL:'"
              "+JSON.stringify({lockAcquireCheck,lockTrackCheck})",
              "<treadline-lock-assist>", &result)
          && strcmp(result.summary, "TREADLINE-LOCK-ASSIST") == 0);
    treadline_gamepad.connected = false;
    treadline_gamepad.buttons = 0;
    treadline_gamepad.timestamp_ms++;
    CHECK(script_runtime_set_gamepad_state(runtime, &treadline_gamepad));
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "__treadlineDebug.selectControls(0);"
              "__treadlineDebug.selectAimAssist(0);"
              "__treadlineDebug.resetInputProbe();"
              "for(const init of [{key:'ArrowUp',code:'ArrowUp'},"
              "{key:'l',code:'KeyL'},{key:' ',code:'Space'},"
              "{key:'Shift',code:'ShiftLeft'}])dispatchEvent("
              "new KeyboardEvent('keydown',{...init,cancelable:true}));"
              "const arcadeKeyboardCheck=__treadlineDebug.pollInput();"
              "for(const init of [{key:'ArrowUp',code:'ArrowUp'},"
              "{key:'l',code:'KeyL'},{key:' ',code:'Space'},"
              "{key:'Shift',code:'ShiftLeft'}])dispatchEvent("
              "new KeyboardEvent('keyup',{...init,cancelable:true}));"
              "__treadlineDebug.selectControls(1);"
              "__treadlineDebug.resetInputProbe();"
              "for(const init of [{key:'w',code:'KeyW'},"
              "{key:'ArrowRight',code:'ArrowRight'},"
              "{key:' ',code:'Space'},{key:'e',code:'KeyE'}])"
              "dispatchEvent(new KeyboardEvent('keydown',"
              "{...init,cancelable:true}));"
              "const classicKeyboardCheck=__treadlineDebug.pollInput();"
              "for(const init of [{key:'w',code:'KeyW'},"
              "{key:'ArrowRight',code:'ArrowRight'},"
              "{key:' ',code:'Space'},{key:'e',code:'KeyE'}])"
              "dispatchEvent(new KeyboardEvent('keyup',"
              "{...init,cancelable:true}));"
              "globalThis.pocSummary=arcadeKeyboardCheck.left===1"
              "&&arcadeKeyboardCheck.right===1"
              "&&!arcadeKeyboardCheck.reverse&&arcadeKeyboardCheck.aimX>.99"
              "&&arcadeKeyboardCheck.fire&&arcadeKeyboardCheck.secondary"
              "&&classicKeyboardCheck.left===1"
              "&&classicKeyboardCheck.right===1"
              "&&!classicKeyboardCheck.reverse&&classicKeyboardCheck.aimX>.99"
              "&&classicKeyboardCheck.fire&&classicKeyboardCheck.secondary"
              "?'TREADLINE-KEYBOARD-SCHEMES'"
              ":'TREADLINE-KEYBOARD-SCHEMES-FAIL:'"
              "+JSON.stringify({arcadeKeyboardCheck,classicKeyboardCheck})",
              "<treadline-keyboard-schemes>", &result)
          && strcmp(result.summary, "TREADLINE-KEYBOARD-SCHEMES") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "__treadlineDebug.selectControls(0);"
              "__treadlineDebug.selectAimAssist(1);"
              "__treadlineDebug.savePreferences();"
              "__treadlineDebug.start();"
              "globalThis.pocSummary='TREADLINE-CONTROLS-RESET'",
              "<treadline-controls-reset>", &result));
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "const drawBefore=__tilefinchWebGLDiagnostics.drawCalls;"
              "__treadlineDebug.step(1);"
              "const drawAfter=__tilefinchWebGLDiagnostics.drawCalls;"
              "const indicator=__treadlineDebug.snapshot();"
              "const ix=indicator.objectiveIndicatorX-160;"
              "const iy=indicator.objectiveIndicatorY-90;"
              "globalThis.pocSummary=drawAfter-drawBefore===3"
              "&&ix*ix+iy*iy>=57*57"
              "?'TREADLINE-RETAINED-HUD-DRAW'"
              ":'TREADLINE-HUD-DRAW-FAIL:'"
              "+JSON.stringify([drawAfter-drawBefore,ix,iy])",
              "<treadline-retained-hud-draw>", &result)
          && strcmp(result.summary, "TREADLINE-RETAINED-HUD-DRAW") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "const delayed=__treadlineDebug.delayedFrame(.25);"
              "globalThis.pocSummary=Math.abs(delayed-1/30)<.0001"
              "?'TREADLINE-DELAY-BOUNDED':'TREADLINE-DELAY-REPLAYED:'+delayed",
              "<treadline-delayed-frame>", &result)
          && strcmp(result.summary, "TREADLINE-DELAY-BOUNDED") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "const authored=['score','arena','armor','gadget'].map(id=>"
              "document.getElementById(id).textContent).join('|');"
              "const transition=__treadlineDebug.completeArenaTransition();"
              "const after=['score','arena','armor','gadget'].map(id=>"
              "document.getElementById(id).textContent).join('|');"
              "globalThis.pocSummary=transition.transitioned"
              "&&transition.authoredWrites===0&&authored===after"
              "?'TREADLINE-ARENA-TRANSITION-DOM-FREE'"
              ":'TREADLINE-ARENA-TRANSITION-DOM-MUTATED:'"
              "+JSON.stringify([transition,authored,after])",
              "<treadline-arena-transition>", &result)
          && strcmp(result.summary,
                    "TREADLINE-ARENA-TRANSITION-DOM-FREE") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "__treadlineDebug.start();"
              "globalThis.pocSummary='TREADLINE-POST-TRANSITION-RESET'",
              "<treadline-post-transition-reset>", &result)
          && strcmp(result.summary,
                    "TREADLINE-POST-TRANSITION-RESET") == 0);
    bool repeated_instance_ok = script_runtime_evaluate_diagnostic(
              runtime,
              "const replayGl=document.querySelector('canvas').getContext('webgl');"
              "const replayA=new Uint8Array(320*180*4),"
              "replayB=new Uint8Array(320*180*4);"
              "__treadlineDebug.freezeBots(true);"
              "for(let id=2;id<6;id++)__treadlineDebug.setTankActive(id,false);"
              "__treadlineDebug.setTankPosition(0,0,-5.8);"
              "__treadlineDebug.setTankPosition(1,-1.5,-3.5);"
              "__treadlineDebug.step(1);"
              "const replayStateA=__treadlineDebug.snapshot();"
              "replayGl.readPixels(0,0,320,180,replayGl.RGBA,"
              "replayGl.UNSIGNED_BYTE,replayA);"
              "__treadlineDebug.setTankPosition(1,1.5,-3.5);"
              "__treadlineDebug.step(1);"
              "const replayStateB=__treadlineDebug.snapshot();"
              "replayGl.readPixels(0,0,320,180,replayGl.RGBA,"
              "replayGl.UNSIGNED_BYTE,replayB);"
              "let replayChanged=0;"
              "for(let at=0;at<replayA.length;at++)"
              "if(replayA[at]!==replayB[at])replayChanged++;"
              "globalThis.pocSummary=replayChanged>64"
              "?'TREADLINE-REPEATED-INSTANCE-LIVE'"
              ":'TREADLINE-REPEATED-INSTANCE-STALE:'"
              "+JSON.stringify([replayChanged,replayStateA,replayStateB])",
              "<treadline-repeated-instance>", &result);
    if (!repeated_instance_ok
        || strcmp(result.summary,
                  "TREADLINE-REPEATED-INSTANCE-LIVE") != 0)
        fprintf(stderr, "treadline repeated instance summary=%s error=%s\n",
                result.summary, result.error);
    CHECK(repeated_instance_ok
          && strcmp(result.summary,
                    "TREADLINE-REPEATED-INSTANCE-LIVE") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "__treadlineDebug.freezeBots(false);__treadlineDebug.start();"
              "__treadlineDebug.step(2);"
              "globalThis.pocSummary='TREADLINE-INSTANCE-RESET'",
              "<treadline-instance-reset>", &result)
          && strcmp(result.summary, "TREADLINE-INSTANCE-RESET") == 0);
    bool treadline_hud_alpha_ok = script_runtime_evaluate_diagnostic(
              runtime,
              "const hudGl=document.querySelector('canvas').getContext('webgl');"
              "__treadlineDebug.step(10);const hud=__treadlineDebug.snapshot();"
              "globalThis.pocSummary=hudGl.isEnabled(hudGl.BLEND)"
              "&&hud.hudCharacters>0&&hud.hudPrimitives>hud.hudCharacters"
              "&&hudGl._textureCount===0"
              "?'TREADLINE-HUD-INK':'TREADLINE-HUD-BLOCKS:'"
              "+JSON.stringify(hud)+'|'+hudGl._textureCount",
              "<treadline-hud-ink>", &result);
    if (!treadline_hud_alpha_ok
        || strcmp(result.summary, "TREADLINE-HUD-INK") != 0)
        fprintf(stderr, "treadline HUD summary=%s error=%s\n",
                result.summary, result.error);
    CHECK(treadline_hud_alpha_ok
          && strcmp(result.summary, "TREADLINE-HUD-INK") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "const hudUploadsBefore=__treadlineDebug.snapshot();"
              "__treadlineDebug.setCommandMeter(50);"
              "__treadlineDebug.step(1);"
              "const hudUploadsAfter=__treadlineDebug.snapshot();"
              "globalThis.pocSummary="
              "hudUploadsAfter.hudIndicatorUploads"
              ">hudUploadsBefore.hudIndicatorUploads"
              "&&hudUploadsAfter.hudTextUploads===hudUploadsBefore.hudTextUploads"
              "?'TREADLINE-HUD-SPLIT':'TREADLINE-HUD-SPLIT-FAIL:'"
              "+JSON.stringify([hudUploadsBefore,hudUploadsAfter])",
              "<treadline-hud-split>", &result)
          && strcmp(result.summary, "TREADLINE-HUD-SPLIT") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "const movementStart=__treadlineDebug.snapshot();"
              "__treadlineDebug.setKeyboard('w',true);"
              "__treadlineDebug.step(24);"
              "__treadlineDebug.setKeyboard('w',false);"
              "const movementEnd=__treadlineDebug.snapshot();"
              "globalThis.pocSummary=movementEnd.playerZ>movementStart.playerZ+.5"
              "&&movementEnd.inputSource==='keyboard'"
              "?'TREADLINE-KEYBOARD':'TREADLINE-KEYBOARD-FAIL:'"
              "+JSON.stringify(movementStart)+'|'+JSON.stringify(movementEnd)",
              "<treadline-keyboard>", &result)
          && strcmp(result.summary, "TREADLINE-KEYBOARD") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "__treadlineDebug.setKeyboard('fire',true);"
              "__treadlineDebug.step(1);"
              "__treadlineDebug.setKeyboard('fire',false);"
              "const fired=__treadlineDebug.snapshot();"
              "globalThis.pocSummary=fired.bullets>0"
              "&&fired.shots>0"
              "&&fired.bullets<=32&&fired.particles<=48"
              "&&fired.vertices<=4096&&fired.indices<=6144"
              "?'TREADLINE-FIRED':'TREADLINE-FIRE-FAIL:'"
              "+JSON.stringify(fired)",
              "<treadline-fire>", &result)
          && strcmp(result.summary, "TREADLINE-FIRED") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "__treadlineDebug.selectClass(0);__treadlineDebug.start();"
              "const classScout=__treadlineDebug.snapshot();"
              "__treadlineDebug.selectClass(1);__treadlineDebug.start();"
              "const classStriker=__treadlineDebug.snapshot();"
              "__treadlineDebug.selectClass(2);__treadlineDebug.start();"
              "const classBulwark=__treadlineDebug.snapshot();"
              "globalThis.pocSummary=classScout.className==='SCOUT'&&classScout.maxArmor===70"
              "&&classStriker.className==='STRIKER'&&classStriker.maxArmor===100"
              "&&classBulwark.className==='BULWARK'&&classBulwark.maxArmor===150"
              "&&document.getElementById('class-choice').textContent"
              ".includes('BULWARK')"
              "?'TREADLINE-CLASSES':'TREADLINE-CLASS-FAIL:'"
              "+[classScout,classStriker,classBulwark].map(JSON.stringify).join('|')",
              "<treadline-classes>", &result)
          && strcmp(result.summary, "TREADLINE-CLASSES") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "__treadlineDebug.selectClass(0);__treadlineDebug.start();"
              "__treadlineDebug.freezeBots(true);"
              "const scoutStart=__treadlineDebug.snapshot();"
              "__treadlineDebug.setKeyboard('w',true);"
              "__treadlineDebug.stepSimulation(30);"
              "__treadlineDebug.setKeyboard('w',false);"
              "const scoutEnd=__treadlineDebug.snapshot();"
              "__treadlineDebug.selectClass(2);__treadlineDebug.start();"
              "__treadlineDebug.freezeBots(true);"
              "const bulwarkStart=__treadlineDebug.snapshot();"
              "__treadlineDebug.setKeyboard('w',true);"
              "__treadlineDebug.stepSimulation(30);"
              "__treadlineDebug.setKeyboard('w',false);"
              "const bulwarkEnd=__treadlineDebug.snapshot();"
              "const scoutTravel=scoutEnd.playerZ-scoutStart.playerZ;"
              "const bulwarkTravel=bulwarkEnd.playerZ-bulwarkStart.playerZ;"
              "globalThis.pocSummary=scoutTravel>bulwarkTravel+.45"
              "&&bulwarkTravel>.7"
              "?'TREADLINE-CLASS-MOVEMENT':'TREADLINE-CLASS-MOVEMENT-FAIL:'"
              "+scoutTravel+'|'+bulwarkTravel",
              "<treadline-class-movement>", &result)
          && strcmp(result.summary, "TREADLINE-CLASS-MOVEMENT") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "__treadlineDebug.selectClass(0);__treadlineDebug.start();"
              "__treadlineDebug.freezeBots(true);__treadlineDebug.fireSecondary();"
              "const secondaryScout=__treadlineDebug.snapshot();"
              "__treadlineDebug.selectClass(1);__treadlineDebug.start();"
              "__treadlineDebug.freezeBots(true);__treadlineDebug.fireSecondary();"
              "const secondaryStriker=__treadlineDebug.snapshot();"
              "__treadlineDebug.selectClass(2);__treadlineDebug.start();"
              "__treadlineDebug.freezeBots(true);__treadlineDebug.fireSecondary();"
              "const secondaryBulwark=__treadlineDebug.snapshot();"
              "globalThis.pocSummary=secondaryScout.bullets===3"
              "&&secondaryScout.maxBulletDamage===11"
              "&&secondaryStriker.bullets===1&&secondaryStriker.piercingBullets===1"
              "&&secondaryStriker.bypassBullets===1&&secondaryStriker.maxBulletDamage===46"
              "&&secondaryBulwark.bullets===5&&secondaryBulwark.maxBulletDamage===10"
              "&&secondaryScout.bullets<=32&&secondaryBulwark.bullets<=32"
              "?'TREADLINE-SECONDARIES':'TREADLINE-SECONDARY-FAIL:'"
              "+[secondaryScout,secondaryStriker,secondaryBulwark]"
              ".map(JSON.stringify).join('|')",
              "<treadline-secondaries>", &result)
          && strcmp(result.summary, "TREADLINE-SECONDARIES") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "__treadlineDebug.selectClass(1);__treadlineDebug.start();"
              "__treadlineDebug.freezeBots(true);"
              "__treadlineDebug.damagePlayerFrom('FRONT',40);"
              "const front=__treadlineDebug.snapshot();"
              "__treadlineDebug.start();__treadlineDebug.freezeBots(true);"
              "__treadlineDebug.damagePlayerFrom('SIDE',40);"
              "const side=__treadlineDebug.snapshot();"
              "__treadlineDebug.start();__treadlineDebug.freezeBots(true);"
              "__treadlineDebug.damagePlayerFrom('REAR',40);"
              "const rear=__treadlineDebug.snapshot();"
              "globalThis.pocSummary=front.armor===74&&front.armorZone==='FRONT'"
              "&&side.armor===60&&side.armorZone==='SIDE'"
              "&&rear.armor===46&&rear.armorZone==='REAR'"
              "?'TREADLINE-ARMOR-ZONES':'TREADLINE-ARMOR-FAIL:'"
              "+[front,side,rear].map(JSON.stringify).join('|')",
              "<treadline-armor-zones>", &result)
          && strcmp(result.summary, "TREADLINE-ARMOR-ZONES") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "__treadlineDebug.selectClass(0);__treadlineDebug.start();"
              "__treadlineDebug.freezeBots(true);"
              "__treadlineDebug.setCommandEnabled(false);"
              "__treadlineDebug.setCommandMeter(100);"
              "__treadlineDebug.activateCommand();"
              "const disabled=__treadlineDebug.snapshot();"
              "__treadlineDebug.setCommandEnabled(true);"
              "__treadlineDebug.setCommandMeter(100);"
              "__treadlineDebug.activateCommand();"
              "const enabled=__treadlineDebug.snapshot();"
              "globalThis.pocSummary=!disabled.commandEnabled"
              "&&disabled.commandMeter===100&&disabled.commandBuff===0"
              "&&enabled.commandEnabled&&enabled.commandMeter===0"
              "&&enabled.commandBuff>0"
              "&&document.getElementById('command-choice').textContent"
              ".includes('On')"
              "?'TREADLINE-COMMAND':'TREADLINE-COMMAND-FAIL:'"
              "+JSON.stringify(disabled)+'|'+JSON.stringify(enabled)",
              "<treadline-command>", &result)
          && strcmp(result.summary, "TREADLINE-COMMAND") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "__treadlineDebug.selectClass(0);__treadlineDebug.start();"
              "__treadlineDebug.freezeBots(true);"
              "dispatchEvent(new KeyboardEvent('keydown',"
              "{key:'e',code:'KeyE',cancelable:true}));"
              "dispatchEvent(new KeyboardEvent('keyup',"
              "{key:'e',code:'KeyE',cancelable:true}));"
              "__treadlineDebug.step(1);"
              "const keyboardSecondary=__treadlineDebug.snapshot();"
              "__treadlineDebug.setCommandEnabled(true);"
              "__treadlineDebug.setCommandMeter(100);"
              "dispatchEvent(new KeyboardEvent('keydown',"
              "{key:'q',code:'KeyQ',cancelable:true}));"
              "dispatchEvent(new KeyboardEvent('keyup',"
              "{key:'q',code:'KeyQ',cancelable:true}));"
              "__treadlineDebug.step(1);"
              "const keyboardCommand=__treadlineDebug.snapshot();"
              "globalThis.pocSummary=keyboardSecondary.bullets===3"
              "&&keyboardSecondary.inputSource==='keyboard'"
              "&&keyboardCommand.commandBuff>0&&keyboardCommand.commandMeter===0"
              "?'TREADLINE-COMBAT-KEYS':'TREADLINE-COMBAT-KEYS-FAIL:'"
              "+JSON.stringify(keyboardSecondary)+'|'"
              "+JSON.stringify(keyboardCommand)",
              "<treadline-combat-keys>", &result)
          && strcmp(result.summary, "TREADLINE-COMBAT-KEYS") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "__treadlineDebug.probeRicochet();"
              "const firstImpact=__treadlineDebug.snapshot();"
              "__treadlineDebug.probeSecondImpact();"
              "const secondImpact=__treadlineDebug.snapshot();"
              "globalThis.pocSummary=firstImpact.ricochets===1"
              "&&firstImpact.bullets===1&&secondImpact.ricochets===1"
              "&&secondImpact.bullets===0"
              "?'TREADLINE-RICOCHET':'TREADLINE-RICOCHET-FAIL:'"
              "+JSON.stringify(firstImpact)+'|'+JSON.stringify(secondImpact)",
              "<treadline-ricochet>", &result)
          && strcmp(result.summary, "TREADLINE-RICOCHET") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "const barriersBefore=__treadlineDebug.snapshot().barriers;"
              "__treadlineDebug.probeBarrier();"
              "const barrierAfter=__treadlineDebug.snapshot();"
              "globalThis.pocSummary=barrierAfter.barriers===barriersBefore-1"
              "&&barrierAfter.barriersBroken===1"
              "&&barrierAfter.staticIndices>0"
              "?'TREADLINE-BARRIER':'TREADLINE-BARRIER-FAIL:'"
              "+JSON.stringify(barrierAfter)",
              "<treadline-barrier>", &result)
          && strcmp(result.summary, "TREADLINE-BARRIER") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "__treadlineDebug.clearGadgetEffects();"
              "__treadlineDebug.activateGadget('MINES');"
              "const mine=__treadlineDebug.snapshot();"
              "__treadlineDebug.clearGadgetEffects();"
              "__treadlineDebug.activateGadget('SMOKE');"
              "const smoke=__treadlineDebug.snapshot();"
              "__treadlineDebug.clearGadgetEffects();"
              "__treadlineDebug.activateGadget('SHIELD');"
              "const shield=__treadlineDebug.snapshot();"
              "__treadlineDebug.clearGadgetEffects();"
              "__treadlineDebug.activateGadget('REPAIR DRONE');"
              "const repair=__treadlineDebug.snapshot();"
              "__treadlineDebug.clearGadgetEffects();"
              "__treadlineDebug.activateGadget('BOOST TREADS');"
              "const boost=__treadlineDebug.snapshot();"
              "globalThis.pocSummary=mine.mines===1&&mine.mines<=12"
              "&&smoke.smoke===1&&smoke.smoke<=6&&shield.shield>0"
              "&&repair.repair>0&&boost.boost>0&&boost.gadgetCooldown>0"
              "&&boost.vertices<=4096&&boost.indices<=6144"
              "?'TREADLINE-GADGETS':'TREADLINE-GADGETS-FAIL:'"
              "+[mine,smoke,shield,repair,boost].map(JSON.stringify).join('|')",
              "<treadline-gadgets>", &result)
          && strcmp(result.summary, "TREADLINE-GADGETS") == 0);
    bool treadline_bot_roles_ok = script_runtime_evaluate_diagnostic(
              runtime,
              "__treadlineDebug.selectMode(1);"
              "__treadlineDebug.selectDifficulty(2);"
              "__treadlineDebug.start();__treadlineDebug.freezeBots(false);"
              "for(let id=2;id<5;id++)__treadlineDebug.setTankPosition(id,7,7-id);"
              "const allyBefore=__treadlineDebug.tankState(1);"
              "__treadlineDebug.stepSimulation(90);"
              "const allyAfter=__treadlineDebug.tankState(1);"
              "__treadlineDebug.selectMode(2);__treadlineDebug.start();"
              "const ambusher=__treadlineDebug.tankState(1);"
              "globalThis.pocSummary=allyBefore.role==='CAPTURE'"
              "&&allyAfter.x*allyAfter.x+allyAfter.z*allyAfter.z"
              "<allyBefore.x*allyBefore.x+allyBefore.z*allyBefore.z"
              "&&ambusher.role==='AMBUSH'"
              "&&document.getElementById('difficulty-choice').textContent"
              ".includes('ACE')"
              "?'TREADLINE-BOT-ROLES':'TREADLINE-BOT-ROLES-FAIL:'"
              "+JSON.stringify(allyBefore)+'|'+JSON.stringify(allyAfter)"
              "+'|'+JSON.stringify(ambusher)",
              "<treadline-bot-roles>", &result);
    if (!treadline_bot_roles_ok
        || strcmp(result.summary, "TREADLINE-BOT-ROLES") != 0)
        fprintf(stderr, "treadline roles summary=%s error=%s\n",
                result.summary, result.error);
    CHECK(treadline_bot_roles_ok
          && strcmp(result.summary, "TREADLINE-BOT-ROLES") == 0);
    bool treadline_bot_separation_ok = script_runtime_evaluate_diagnostic(
              runtime,
              "__treadlineDebug.selectMode(0);"
              "__treadlineDebug.selectDifficulty(0);"
              "__treadlineDebug.start();__treadlineDebug.freezeBots(false);"
              "for(let id=2;id<6;id++)__treadlineDebug.setTankActive(id,false);"
              "__treadlineDebug.setTankPosition(0,0,-5.8);"
              "__treadlineDebug.setTankPosition(1,.3,-5.8);"
              "let priorBot=__treadlineDebug.tankState(1),movingFrames=0;"
              "for(let frame=0;frame<90;frame++){"
              "__treadlineDebug.stepSimulation(1);"
              "const bot=__treadlineDebug.tankState(1);"
              "const dx=bot.x-priorBot.x,dz=bot.z-priorBot.z;"
              "if(dx*dx+dz*dz>.000001)movingFrames++;priorBot=bot;}"
              "const separatedBot=__treadlineDebug.tankState(1);"
              "const separatedPlayer=__treadlineDebug.tankState(0);"
              "const sx=separatedBot.x-separatedPlayer.x;"
              "const sz=separatedBot.z-separatedPlayer.z;"
              "const separation=Math.sqrt(sx*sx+sz*sz);"
              "globalThis.pocSummary=separatedBot.active&&separation>=2.05"
              "&&movingFrames>=55"
              "?'TREADLINE-BOT-SEPARATION':'TREADLINE-BOT-STUCK:'"
              "+JSON.stringify(separatedBot)+'|'+separation+'|'+movingFrames",
              "<treadline-bot-separation>", &result);
    if (!treadline_bot_separation_ok
        || strcmp(result.summary, "TREADLINE-BOT-SEPARATION") != 0)
        fprintf(stderr, "treadline separation summary=%s error=%s\n",
                result.summary, result.error);
    CHECK(treadline_bot_separation_ok
          && strcmp(result.summary, "TREADLINE-BOT-SEPARATION") == 0);
    bool treadline_tank_contact_ok = script_runtime_evaluate_diagnostic(
              runtime,
              "__treadlineDebug.start();__treadlineDebug.freezeBots(false);"
              "for(let id=2;id<6;id++)__treadlineDebug.setTankActive(id,false);"
              "__treadlineDebug.setTankPosition(0,0,-5.8);"
              "__treadlineDebug.setTankPosition(1,.3,-5.8);"
              "for(let frame=0;frame<24;frame++)"
              "__treadlineDebug.stepSimulation(1);"
              "const clearance=__treadlineDebug.tankPairClearance();"
              "globalThis.pocSummary=clearance>=-.001"
              "?'TREADLINE-TANK-CONTACT':'TREADLINE-TANK-OVERLAP:'"
              "+clearance",
              "<treadline-tank-contact>", &result);
    if (!treadline_tank_contact_ok
        || strcmp(result.summary, "TREADLINE-TANK-CONTACT") != 0)
        fprintf(stderr, "treadline contact summary=%s error=%s\n",
                result.summary, result.error);
    CHECK(treadline_tank_contact_ok
          && strcmp(result.summary, "TREADLINE-TANK-CONTACT") == 0);
    bool treadline_bot_standoff_ok = script_runtime_evaluate_diagnostic(
              runtime,
              "__treadlineDebug.selectMode(0);"
              "__treadlineDebug.selectDifficulty(0);"
              "__treadlineDebug.start();__treadlineDebug.freezeBots(false);"
              "for(let id=2;id<6;id++)__treadlineDebug.setTankActive(id,false);"
              "__treadlineDebug.setTankPosition(0,0,-5.8);"
              "__treadlineDebug.setTankPosition(1,.35,-5.8);"
              "let reached=false,backoffAfterClear=0,orbitFrames=0;"
              "let minimumDistance=99,maximumDistance=0;"
              "let firstAngle=0,lastAngle=0;"
              "for(let frame=0;frame<180;frame++){"
              "__treadlineDebug.stepSimulation(1);"
              "const player=__treadlineDebug.tankState(0);"
              "const bot=__treadlineDebug.tankState(1);"
              "const dx=bot.x-player.x,dz=bot.z-player.z;"
              "const distance=Math.sqrt(dx*dx+dz*dz);"
              "if(!reached&&bot.standoff&&!bot.backingOff&&distance>=2.55){"
              "reached=true;firstAngle=Math.atan2(dx,dz);}"
              "if(reached){if(bot.backingOff)backoffAfterClear++;"
              "minimumDistance=Math.min(minimumDistance,distance);"
              "maximumDistance=Math.max(maximumDistance,distance);"
              "if(bot.standoff){orbitFrames++;lastAngle=Math.atan2(dx,dz);}}}"
              "const finalPlayer=__treadlineDebug.tankState(0);"
              "const finalBot=__treadlineDebug.tankState(1);"
              "const finalDx=finalBot.x-finalPlayer.x;"
              "const finalDz=finalBot.z-finalPlayer.z;"
              "const finalDistance=Math.sqrt(finalDx*finalDx+finalDz*finalDz);"
              "let swept=Math.abs(lastAngle-firstAngle);"
              "if(swept>Math.PI)swept=Math.PI*2-swept;"
              "globalThis.pocSummary=reached&&backoffAfterClear<=2"
              "&&orbitFrames>=45&&swept>.18&&minimumDistance>=2.05"
              "&&maximumDistance<=3.75&&finalDistance>=2.25"
              "?'TREADLINE-BOT-STANDOFF':'TREADLINE-BOT-STANDOFF-FAIL:'"
              "+[reached,backoffAfterClear,orbitFrames,swept,minimumDistance,"
              "maximumDistance,finalDistance,"
              "JSON.stringify(finalBot)].join('|')",
              "<treadline-bot-standoff>", &result);
    if (!treadline_bot_standoff_ok
        || strcmp(result.summary, "TREADLINE-BOT-STANDOFF") != 0)
        fprintf(stderr, "treadline standoff summary=%s error=%s\n",
                result.summary, result.error);
    CHECK(treadline_bot_standoff_ok
          && strcmp(result.summary, "TREADLINE-BOT-STANDOFF") == 0);
    bool treadline_bot_obstacle_ok = script_runtime_evaluate_diagnostic(
              runtime,
              "__treadlineDebug.start();__treadlineDebug.freezeBots(false);"
              "for(let id=2;id<6;id++)__treadlineDebug.setTankActive(id,false);"
              "__treadlineDebug.setTankPosition(0,-3.65,-5.8);"
              "__treadlineDebug.setTankPosition(1,-3.65,-.6);"
              "__treadlineDebug.setTankHeading(1,Math.PI);"
              "let priorObstacleBot=__treadlineDebug.tankState(1),"
              "obstacleMovingFrames=0;"
              "for(let frame=0;frame<180;frame++){"
              "__treadlineDebug.stepSimulation(1);"
              "const bot=__treadlineDebug.tankState(1);"
              "const dx=bot.x-priorObstacleBot.x,dz=bot.z-priorObstacleBot.z;"
              "if(dx*dx+dz*dz>.000001)obstacleMovingFrames++;"
              "priorObstacleBot=bot;}"
              "const escapedBot=__treadlineDebug.tankState(1);"
              "globalThis.pocSummary=escapedBot.active&&escapedBot.z<-2.15"
              "&&escapedBot.blockedTime<.4&&obstacleMovingFrames>=105"
              "?'TREADLINE-BOT-OBSTACLE':'TREADLINE-BOT-OBSTACLE-STUCK:'"
              "+JSON.stringify(escapedBot)+'|'+obstacleMovingFrames",
              "<treadline-bot-obstacle>", &result);
    if (!treadline_bot_obstacle_ok
        || strcmp(result.summary, "TREADLINE-BOT-OBSTACLE") != 0)
        fprintf(stderr, "treadline obstacle summary=%s error=%s\n",
                result.summary, result.error);
    CHECK(treadline_bot_obstacle_ok
          && strcmp(result.summary, "TREADLINE-BOT-OBSTACLE") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "__treadlineDebug.selectMode(1);__treadlineDebug.start();"
              "__treadlineDebug.freezeBots(true);"
              "__treadlineDebug.setTankPosition(0,0,0);"
              "__treadlineDebug.setTankPosition(1,.5,0);"
              "for(let id=2;id<5;id++)__treadlineDebug.setTankPosition(id,7,7-id);"
              "__treadlineDebug.stepSimulation(60);"
              "const control=__treadlineDebug.snapshot();"
              "__treadlineDebug.selectMode(2);__treadlineDebug.start();"
              "__treadlineDebug.freezeBots(true);"
              "__treadlineDebug.setTankPosition(0,0,-6);"
              "for(let id=1;id<5;id++)__treadlineDebug.setTankPosition(id,7,7-id);"
              "__treadlineDebug.stepSimulation(60);"
              "const convoy=__treadlineDebug.snapshot();"
              "globalThis.pocSummary=control.gameMode===1"
              "&&control.blueTanks===2&&control.redTanks===3"
              "&&control.blueControl>5&&convoy.gameMode===2"
              "&&convoy.convoyActive&&convoy.convoyProgress>0"
              "&&convoy.blueTanks===1&&convoy.redTanks===4"
              "?'TREADLINE-MODES':'TREADLINE-MODES-FAIL:'"
              "+JSON.stringify(control)+'|'+JSON.stringify(convoy)",
              "<treadline-modes>", &result)
          && strcmp(result.summary, "TREADLINE-MODES") == 0);
    bool treadline_onslaught_ok = script_runtime_evaluate_diagnostic(
              runtime,
              "__treadlineDebug.selectMode(3);__treadlineDebug.start();"
              "const forgeStart=__treadlineDebug.snapshot();"
              "__treadlineDebug.step(29);"
              "const forgeHeld=__treadlineDebug.snapshot();"
              "__treadlineDebug.step(320);"
              "const forgeDone=__treadlineDebug.snapshot();"
              "__treadlineDebug.freezeBots(true);"
              "__treadlineDebug.step(8);"
              "const waveOne=__treadlineDebug.snapshot();"
              "const repeatBefore="
              "__tilefinchWebGLDiagnostics.repeatedCommandListExecutions;"
              "__treadlineDebug.destroyTank(1);"
              "const chained=__treadlineDebug.snapshot();"
              "__treadlineDebug.damagePlayerFrom('FRONT',8);"
              "const reset=__treadlineDebug.snapshot();"
              "let finalEnemy=0;for(let id=2;id<6;id++){"
              "if(__treadlineDebug.tankState(id)?.active)finalEnemy=id;}"
              "for(let id=2;id<6;id++){"
              "if(id!==finalEnemy&&__treadlineDebug.tankState(id)?.active)"
              "__treadlineDebug.destroyTank(id);}"
              "const beforeFinal=__treadlineDebug.snapshot();"
              "__treadlineDebug.destroyTank(finalEnemy);"
              "const beatStart=__treadlineDebug.snapshot();"
              "__treadlineDebug.stepSimulation(10);"
              "const beatMiddle=__treadlineDebug.snapshot();"
              "__treadlineDebug.stepSimulation(18);"
              "const beatEnd=__treadlineDebug.snapshot();"
              "__treadlineDebug.stepSimulation(90);"
              "__treadlineDebug.step(8);"
              "const waveTwo=__treadlineDebug.snapshot();"
              "const saved=JSON.parse(localStorage.getItem('treadline-settings-v1'));"
              "const slowedSim=beatMiddle.simTime-beatStart.simTime;"
              "const elapsedWall=beatMiddle.wallTime-beatStart.wallTime;"
              "globalThis.pocSummary=forgeStart.mode==='generating'"
              "&&forgeStart.arenaGenerationPhase===1"
              "&&forgeHeld.mode==='generating'&&forgeHeld.arenaGenerationAge<.5"
              "&&forgeDone.mode==='playing'&&forgeDone.arenaGenerationAge>=.5"
              "&&forgeDone.arenaGenerationPhase===0"
              "&&waveOne.gameMode===3&&waveOne.wave===1"
              "&&waveOne.multiplier===1&&waveOne.enemies>=3"
              "&&chained.multiplier===2&&chained.score>0"
              "&&reset.multiplier===1&&reset.damageTaken>0"
              "&&beforeFinal.enemies===1&&beatStart.mode==='playing'"
              "&&beatStart.pendingClear&&beatStart.killBeat>.3"
              "&&beatMiddle.mode==='playing'&&beatMiddle.pendingClear"
              "&&elapsedWall>.16&&slowedSim<elapsedWall*.35"
              "&&beatEnd.mode==='arena-clear'&&!beatEnd.pendingClear"
              "&&waveTwo.mode==='playing'&&waveTwo.wave===2"
              "&&waveTwo.multiplier>=1&&waveTwo.multiplier<=5"
              "&&waveTwo.enemies>=3&&waveTwo.hudIndicatorPrimitives<=12"
              "&&waveTwo.repeatedFrameCaptures<=waveOne.repeatedFrameCaptures+1"
              "&&__tilefinchWebGLDiagnostics.repeatedCommandListExecutions"
              ">repeatBefore"
              "&&saved.bestWave>=1&&saved.bestScore>=chained.score"
              "?'TREADLINE-ONSLAUGHT':'TREADLINE-ONSLAUGHT-FAIL:'"
              "+[JSON.stringify(forgeStart),JSON.stringify(forgeHeld),"
              "JSON.stringify(forgeDone),JSON.stringify(waveOne),JSON.stringify(chained),"
              "JSON.stringify(reset),JSON.stringify(beatStart),"
              "JSON.stringify(beatMiddle),JSON.stringify(beatEnd),"
              "JSON.stringify(waveTwo),JSON.stringify(saved)].join('|')",
              "<treadline-onslaught>", &result);
    if (!treadline_onslaught_ok
        || strcmp(result.summary, "TREADLINE-ONSLAUGHT") != 0)
        fprintf(stderr, "treadline onslaught summary=%s error=%s\n",
                result.summary, result.error);
    CHECK(treadline_onslaught_ok
          && strcmp(result.summary, "TREADLINE-ONSLAUGHT") == 0);
    bool treadline_telegraph_ok = script_runtime_evaluate_diagnostic(
              runtime,
              "__treadlineDebug.selectMode(0);"
              "__treadlineDebug.selectDifficulty(0);"
              "__treadlineDebug.start();__treadlineDebug.freezeBots(false);"
              "for(let id=3;id<6;id++)__treadlineDebug.setTankActive(id,false);"
              "__treadlineDebug.setTankPosition(0,0,-5.8);"
              "__treadlineDebug.setTankPosition(1,-1.4,-1);"
              "__treadlineDebug.setTankPosition(2,1.4,-1);"
              "const armedA=__treadlineDebug.armBotShot(1,false);"
              "const armedB=__treadlineDebug.armBotShot(2,false);"
              "__treadlineDebug.stepSimulation(12);"
              "const warning=__treadlineDebug.snapshot();"
              "const warningA=__treadlineDebug.tankState(1);"
              "const warningB=__treadlineDebug.tankState(2);"
              "__treadlineDebug.stepSimulation(72);"
              "const volley=__treadlineDebug.snapshot();"
              "globalThis.pocSummary=armedA&&armedB"
              "&&warning.botTelegraphs===2&&warning.botPlayerShots===0"
              "&&warningA.fireWindup>0&&warningB.fireWindup>0"
              "&&volley.botPlayerShots>=2"
              "&&volley.botTelegraphViolations===0"
              "&&volley.botVolleyMinimum>=.249"
              "?'TREADLINE-TELEGRAPH':'TREADLINE-TELEGRAPH-FAIL:'"
              "+JSON.stringify(warning)+'|'+JSON.stringify(warningA)"
              "+'|'+JSON.stringify(warningB)+'|'+JSON.stringify(volley)",
              "<treadline-telegraph>", &result);
    if (!treadline_telegraph_ok
        || strcmp(result.summary, "TREADLINE-TELEGRAPH") != 0)
        fprintf(stderr, "treadline telegraph summary=%s error=%s\n",
                result.summary, result.error);
    CHECK(treadline_telegraph_ok
          && strcmp(result.summary, "TREADLINE-TELEGRAPH") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "__treadlineDebug.start();__treadlineDebug.freezeBots(true);"
              "__treadlineDebug.damagePlayerFrom('SIDE',8);"
              "const flashed=__treadlineDebug.tankState(0);"
              "globalThis.pocSummary=flashed.hitFlash>0"
              "?'TREADLINE-HIT-FLASH':'TREADLINE-HIT-FLASH-FAIL:'"
              "+JSON.stringify(flashed)",
              "<treadline-hit-flash>", &result)
          && strcmp(result.summary, "TREADLINE-HIT-FLASH") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "__treadlineDebug.freezeBots(false);"
              "__treadlineDebug.selectMode(0);__treadlineDebug.start();"
              "__treadlineDebug.selectGadget(4);"
              "__treadlineDebug.selectDifficulty(2);"
              "globalThis.pocSummary="
              "document.getElementById('mode-choice').textContent"
              ".includes('SURVIVAL')"
              "&&document.getElementById('gadget-choice').textContent"
              ".includes('BOOST TREADS')"
              "&&document.getElementById('difficulty-choice').textContent"
              ".includes('ACE')"
              "&&document.getElementById('gadget-meter')!==null"
              "&&document.getElementById('objective-arrow')!==null"
              "?'TREADLINE-LOADOUT':'TREADLINE-LOADOUT-FAIL'",
              "<treadline-loadout>", &result)
          && strcmp(result.summary, "TREADLINE-LOADOUT") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "__treadlineDebug.freezeBots(true);"
              "__treadlineDebug.selectCamera(0);"
              "const safetyBefore=__treadlineDebug.snapshot().cameraSafetyUpdates;"
              "__treadlineDebug.setTankHeading(0,Math.PI/2);"
              "__treadlineDebug.stepCamera(30);"
              "const stable=__treadlineDebug.snapshot();"
              "const right=__treadlineDebug.mapScreenAim(1,0);"
              "const left=__treadlineDebug.mapScreenAim(-1,0);"
              "__treadlineDebug.selectCamera(1);"
              "__treadlineDebug.stepCamera(60);"
              "const follow=__treadlineDebug.snapshot();"
              "globalThis.pocSummary=Math.abs(stable.cameraYaw)<.001"
              "&&stable.cameraSafetyUpdates-safetyBefore===30"
              "&&right.x>.99&&left.x<-.99&&Math.abs(right.z)<.001"
              "&&follow.cameraYaw>.5&&follow.cameraYaw<.9"
              "&&__treadlineDebug.cameraProgramsAligned()"
              "&&document.getElementById('camera-choice').textContent"
              ".includes('FOLLOW')"
              "?'TREADLINE-CAMERA':'TREADLINE-CAMERA-FAIL:'"
              "+JSON.stringify(stable)+'|'+JSON.stringify(right)"
              "+'|'+JSON.stringify(follow)",
              "<treadline-camera>", &result)
          && strcmp(result.summary, "TREADLINE-CAMERA") == 0);
    bool treadline_camera_pixels_ok = script_runtime_evaluate_diagnostic(
              runtime,
              "const cameraGl=document.querySelector('canvas').getContext('webgl');"
              "const cameraA=new Uint8Array(320*180*4),"
              "cameraB=new Uint8Array(320*180*4);"
              "__treadlineDebug.start();__treadlineDebug.freezeBots(true);"
              "for(let id=2;id<6;id++)__treadlineDebug.setTankActive(id,false);"
              "__treadlineDebug.setTankPosition(0,0,-5.8);"
              "__treadlineDebug.setTankPosition(1,1.5,-3.2);"
              "__treadlineDebug.selectCamera(0);__treadlineDebug.step(1);"
              "cameraGl.readPixels(0,0,320,180,cameraGl.RGBA,"
              "cameraGl.UNSIGNED_BYTE,cameraA);"
              "__treadlineDebug.setTankHeading(0,Math.PI/2);"
              "__treadlineDebug.selectCamera(1);"
              "__treadlineDebug.stepCamera(60);__treadlineDebug.step(1);"
              "cameraGl.readPixels(0,0,320,180,cameraGl.RGBA,"
              "cameraGl.UNSIGNED_BYTE,cameraB);"
              "let changed=0,redAX=0,redAN=0,redBX=0,redBN=0;"
              "for(let pixel=0;pixel<320*180;pixel++){const at=pixel*4;"
              "if(cameraA[at]!==cameraB[at]||cameraA[at+1]!==cameraB[at+1]"
              "||cameraA[at+2]!==cameraB[at+2])changed++;"
              "const x=pixel%320,y=(pixel/320)|0;"
              "if(y>24&&y<156&&cameraA[at]>170&&cameraA[at+1]>40"
              "&&cameraA[at+1]<150&&cameraA[at+2]<100){redAX+=x;redAN++;}"
              "if(y>24&&y<156&&cameraB[at]>170&&cameraB[at+1]>40"
              "&&cameraB[at+1]<150&&cameraB[at+2]<100){redBX+=x;redBN++;}}"
              "const redShift=redAN&&redBN?Math.abs(redAX/redAN-redBX/redBN):0;"
              "globalThis.pocSummary=changed>2000&&redAN>12&&redBN>12"
              "&&redShift>2&&__treadlineDebug.cameraProgramsAligned()"
              "?'TREADLINE-CAMERA-PIXELS':'TREADLINE-CAMERA-PIXELS-FAIL:'"
              "+[changed,redAN,redBN,redShift].join('|')",
              "<treadline-camera-pixels>", &result);
    if (!treadline_camera_pixels_ok
        || strcmp(result.summary, "TREADLINE-CAMERA-PIXELS") != 0)
        fprintf(stderr, "treadline camera pixels summary=%s error=%s\n",
                result.summary, result.error);
    CHECK(treadline_camera_pixels_ok
          && strcmp(result.summary, "TREADLINE-CAMERA-PIXELS") == 0);
    bool treadline_collision_invariant_ok =
        script_runtime_evaluate_diagnostic(
              runtime,
              "__treadlineDebug.selectMode(0);"
              "__treadlineDebug.selectDifficulty(2);"
              "__treadlineDebug.start();__treadlineDebug.freezeBots(false);"
              "let invalidPlacement=-1,invalidTank=-1;"
              "for(let frame=0;frame<720&&invalidPlacement<0;frame++){"
              "__treadlineDebug.stepSimulation(1);"
              "for(let id=0;id<6;id++){const tank=__treadlineDebug.tankState(id);"
              "if(tank&&tank.active&&!tank.placementValid){"
              "invalidPlacement=frame;invalidTank=id;break;}}}"
              "globalThis.pocSummary=invalidPlacement<0"
              "?'TREADLINE-COLLISION-INVARIANT'"
              ":'TREADLINE-COLLISION-ESCAPE:'+invalidPlacement+'|'+invalidTank",
              "<treadline-collision-invariant>", &result);
    if (!treadline_collision_invariant_ok
        || strcmp(result.summary, "TREADLINE-COLLISION-INVARIANT") != 0)
        fprintf(stderr, "treadline collision invariant summary=%s error=%s\n",
                result.summary, result.error);
    CHECK(treadline_collision_invariant_ok
          && strcmp(result.summary,
                    "TREADLINE-COLLISION-INVARIANT") == 0);
    bool treadline_camera_edge_ok = script_runtime_evaluate_diagnostic(
              runtime,
              "__treadlineDebug.start();__treadlineDebug.freezeBots(true);"
              "__treadlineDebug.selectCamera(0);"
              "__treadlineDebug.setTankPosition(0,0,-7.6);"
              "__treadlineDebug.stepCamera(8);"
              "const edge=__treadlineDebug.snapshot();"
              "globalThis.pocSummary=edge.cameraSafeDistance>=1.19"
              "&&edge.cameraSafeDistance<=1.21"
              "&&edge.cameraDistance<=edge.cameraSafeDistance+.001"
              "?'TREADLINE-CAMERA-EDGE':'TREADLINE-CAMERA-EDGE-FAIL:'"
              "+JSON.stringify(edge)",
              "<treadline-camera-edge>", &result);
    if (!treadline_camera_edge_ok
        || strcmp(result.summary, "TREADLINE-CAMERA-EDGE") != 0)
        fprintf(stderr, "treadline camera edge summary=%s error=%s\n",
                result.summary, result.error);
    CHECK(treadline_camera_edge_ok
          && strcmp(result.summary, "TREADLINE-CAMERA-EDGE") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "dispatchEvent(new KeyboardEvent('keydown',"
              "{key:'p',code:'KeyP',cancelable:true}));"
              "dispatchEvent(new KeyboardEvent('keyup',"
              "{key:'p',code:'KeyP',cancelable:true}));"
              "__treadlineDebug.step(1);"
              "globalThis.pocSummary=__treadlineDebug.snapshot().mode==='paused'"
              "&&!document.getElementById('hud').hidden"
              "?'TREADLINE-PAUSED':'TREADLINE-PAUSE-FAIL'",
              "<treadline-pause>", &result)
          && strcmp(result.summary, "TREADLINE-PAUSED") == 0);
    bool treadline_online_ok = script_runtime_evaluate_diagnostic(
              runtime,
              "document.getElementById('online-host').click();"
              "__tilefinchDeliverMultiplayer(71,'invitecode',undefined,'internet',"
              "0,false,0,'123456789012','');"
              "__tilefinchDeliverMultiplayer(71,'peerrequest',undefined,'incoming',"
              "0,false,99,'','Guest');"
              "document.getElementById('online-accept').click();"
              "__tilefinchDeliverMultiplayer(71,'open',undefined,'',0,false,99,'','Guest');"
              "const beforeOnline=__treadlineDebug.snapshot();"
              "const inputPacket=new ArrayBuffer(12),inputView=new DataView(inputPacket);"
              "inputView.setUint8(0,1);inputView.setUint8(1,3);"
              "inputView.setUint16(2,1,true);inputView.setUint8(4,3);"
              "inputView.setInt16(8,32767,true);"
              "__tilefinchDeliverMultiplayer(71,'binary',inputPacket,'',0,false,99,'','');"
              "__treadlineDebug.stepSimulation(8);"
              "const afterOnline=__treadlineDebug.snapshot();"
              "const sent=__treadlineOnlineTest.sentFirst;"
              "globalThis.pocSummary=__treadlineOnlineTest.accepted"
              "&&__treadlineOnlineTest.open[0]==='host'"
              "&&__treadlineOnlineTest.open[1]==='treadline-arena-v2'"
              "&&afterOnline.onlineActive&&afterOnline.onlineRole==='host'"
              "&&afterOnline.onlineInputSequence===1"
              "&&afterOnline.remotePlayerZ>beforeOnline.remotePlayerZ"
              "&&sent instanceof ArrayBuffer&&sent.byteLength<=512"
              "&&new Uint8Array(sent)[0]===2"
              "?'TREADLINE-ONLINE':'TREADLINE-ONLINE-FAIL:'"
              "+JSON.stringify(afterOnline)+'|'+(sent&&sent.byteLength);",
              "<treadline-online>", &result);
    if (!treadline_online_ok
        || strcmp(result.summary, "TREADLINE-ONLINE") != 0)
        fprintf(stderr, "treadline online summary=%s error=%s\n",
                result.summary, result.error);
    CHECK(treadline_online_ok
          && strcmp(result.summary, "TREADLINE-ONLINE") == 0);
    bool treadline_generated_online_ok = script_runtime_evaluate_diagnostic(
              runtime,
              "for(let drain=0;drain<16;drain++)"
              "__tilefinchDeliverMultiplayer(71,'drain',undefined,'',"
              "0,false,99,'','');"
              "const generated=__treadlineDebug.onlineGeneratedSnapshot("
              "0x2468ace0);__treadlineDebug.setBarrierActive(0,false);"
              "__tilefinchDeliverMultiplayer(71,'drain',undefined,'',"
              "0,false,99,'','');"
              "__treadlineDebug.emitOnlineSnapshot();"
              "const packet=__treadlineOnlineTest.sentLast;"
              "const packetView=new DataView(packet);"
              "const wireOk=generated.accepted&&generated.seed!==0"
              "&&packetView.getUint8(1)===3&&packetView.getUint8(4)===255"
              "&&packetView.getUint32(24,true)===generated.seed"
              "&&packetView.getUint16(28,true)===generated.checksum"
              "&&packetView.getUint16(30,true)===14;"
              "const applied=__treadlineDebug.applyOnlinePacket(packet.slice(0));"
              "const pendingGeometry=__treadlineDebug.snapshot();"
              "__treadlineDebug.stepSimulation(1);"
              "const rebuilt=__treadlineDebug.snapshot();"
              "const bad=packet.slice(0),badView=new DataView(bad);"
              "badView.setUint16(2,(badView.getUint16(2,true)+1)&65535,true);"
              "badView.setUint16(28,badView.getUint16(28,true)^1,true);"
              "const mismatch=__treadlineDebug.applyOnlinePacket(bad);"
              "const fallback=__treadlineDebug.snapshot();"
              "const old=packet.slice(0),oldView=new DataView(old);"
              "oldView.setUint8(1,2);"
              "oldView.setUint16(2,(badView.getUint16(2,true)+1)&65535,true);"
              "const oldAccepted=__treadlineDebug.applyOnlinePacket(old);"
              "const afterOld=__treadlineDebug.snapshot();"
              "globalThis.pocSummary=wireOk&&applied&&rebuilt.arena===3"
              "&&pendingGeometry.onlineArenaGeometryPending"
              "&&!rebuilt.onlineArenaGeometryPending"
              "&&rebuilt.arenaSeed===generated.seed&&rebuilt.barriers===3"
              "&&mismatch&&fallback.arena===0&&fallback.barriers===3&&!oldAccepted"
              "&&afterOld.onlineSnapshotSequence===fallback.onlineSnapshotSequence"
              "?'TREADLINE-ONLINE-ARENA':'TREADLINE-ONLINE-ARENA-FAIL:'"
              "+JSON.stringify({generated,wireOk,applied,pendingGeometry,rebuilt,mismatch,"
              "fallback,oldAccepted,afterOld});",
              "<treadline-online-generated-arena>", &result);
    if (!treadline_generated_online_ok
        || strcmp(result.summary, "TREADLINE-ONLINE-ARENA") != 0)
        fprintf(stderr, "treadline generated online summary=%s error=%s\n",
                result.summary, result.error);
    CHECK(treadline_generated_online_ok
          && strcmp(result.summary, "TREADLINE-ONLINE-ARENA") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "const hex=bytes=>{let out='';for(const byte of bytes)"
              "out+=byte.toString(16).padStart(2,'0');return out};"
              "const arenaOut=new Float32Array(104);let arenaAt=0;"
              "for(const list of [__treadlineArenaData.generatedArena.obstacles,"
              "__treadlineArenaData.generatedArena.barriers,"
              "__treadlineArenaData.generatedArena.ramps,"
              "__treadlineArenaData.generatedArena.gates])"
              "for(const item of list)for(let field=0;field<item.length;field++)"
              "arenaOut[arenaAt++]=item[field];"
              "globalThis.pocSummary=hex(new Uint8Array("
              "__treadlineOnlineTest.sentLast))+'|'"
              "+hex(new Uint8Array(arenaOut.buffer));",
              "<treadline-online-fixture>", &result));
    CHECK(strlen(result.summary) < sizeof(generated_online_fixture));
    snprintf(generated_online_fixture, sizeof(generated_online_fixture),
             "%s", result.summary);
    bool treadline_online_soak_ok = script_runtime_evaluate_diagnostic(
              runtime,
              "__treadlineDebug.freezeBots(true);"
              "for(let id=2;id<5;id++)__treadlineDebug.setTankPosition(id,7,7-id);"
              "globalThis.__treadlineSoakSequence=2;"
              "globalThis.__treadlineSoakFrame=0;"
              "globalThis.pocSummary='TREADLINE-ONLINE-SOAK-READY';",
              "<treadline-online-soak-setup>", &result)
          && strcmp(result.summary, "TREADLINE-ONLINE-SOAK-READY") == 0;
    for (size_t chunk = 0; chunk < 5u && treadline_online_soak_ok; chunk++) {
        treadline_online_soak_ok = script_runtime_evaluate_diagnostic(
              runtime,
              "for(let count=0;count<60;count++){"
              "const frame=globalThis.__treadlineSoakFrame++;"
              "if(frame%3===0){"
              "const packet=new ArrayBuffer(12),view=new DataView(packet);"
              "view.setUint8(0,1);view.setUint8(1,3);"
              "view.setUint16(2,globalThis.__treadlineSoakSequence++,true);"
              "view.setUint8(4,(frame%120)<60?1:2);"
              "view.setInt16(8,32767,true);"
              "if(frame%33!==0)__tilefinchDeliverMultiplayer(71,'binary',packet,"
              "'',0,false,99,'','');"
              "if(frame%111===0)__tilefinchDeliverMultiplayer(71,'binary',packet,"
              "'',0,false,99,'','');"
              "}__treadlineDebug.stepSimulation(1);"
              "__tilefinchDeliverMultiplayer(71,'drain',undefined,'',"
              "0,false,99,'','');"
              "}"
              "globalThis.pocSummary='TREADLINE-ONLINE-SOAK-CHUNK';",
              "<treadline-online-soak-chunk>", &result)
          && strcmp(result.summary, "TREADLINE-ONLINE-SOAK-CHUNK") == 0;
    }
    if (treadline_online_soak_ok)
        treadline_online_soak_ok = script_runtime_evaluate_diagnostic(
              runtime,
              "const soaked=__treadlineDebug.snapshot();"
              "const last=__treadlineOnlineTest.sentLast;"
              "globalThis.pocSummary=soaked.onlineActive"
              "&&soaked.onlineRole==='host'&&soaked.onlineInputValid"
              "&&soaked.onlineInputSequence>90"
              "&&__treadlineOnlineTest.sentCount>=70"
              "&&__treadlineOnlineTest.sentCount<=85"
              "&&last instanceof ArrayBuffer&&last.byteLength<=320"
              "&&new Uint8Array(last)[0]===2"
              "&&soaked.vertices<=4096&&soaked.indices<=6144"
              "&&soaked.meshDrops===0"
              "?'TREADLINE-ONLINE-SOAK':'TREADLINE-ONLINE-SOAK-FAIL:'"
              "+JSON.stringify(soaked)+'|'"
              "+__treadlineOnlineTest.sentCount+'|'"
              "+(last&&last.byteLength);",
              "<treadline-online-soak-check>", &result)
          && strcmp(result.summary, "TREADLINE-ONLINE-SOAK") == 0;
    if (!treadline_online_soak_ok
        || strcmp(result.summary, "TREADLINE-ONLINE-SOAK") != 0)
        fprintf(stderr, "treadline online soak summary=%s error=%s\n",
                result.summary, result.error);
    CHECK(treadline_online_soak_ok
          && strcmp(result.summary, "TREADLINE-ONLINE-SOAK") == 0);
    bool treadline_visual_budget_ok = script_runtime_evaluate_diagnostic(
              runtime,
              "__treadlineDebug.selectMode(2);__treadlineDebug.start();"
              "__treadlineDebug.freezeBots(true);"
              "__treadlineDebug.saturateVisualLoad();"
              "const loaded=__treadlineDebug.snapshot();"
              "const staticCounts=__treadlineDebug.arenaStaticIndexCounts();"
              "const staticBounded=staticCounts.length===4"
              "&&staticCounts.every(value=>value>0"
              "&&value<=loaded.arenaMaximumStaticIndexCount"
              "&&value<=4096);"
              "const common=staticCounts[0]-(6*36+2*18);"
              "const wedgeBounded=staticCounts[3]-common"
              "===loaded.generatedStaticTailIndexLimit"
              "&&loaded.generatedStaticTailIndexLimit===8*36+2*18;"
              "const aggregate=loaded.staticIndices"
              "+loaded.boxInstances*36+loaded.hudPrimitives*6;"
              "globalThis.pocSummary=loaded.boxInstances<=64"
              "&&loaded.instanceLimit===64&&loaded.tankBarrels===6"
              "&&loaded.bullets===18&&loaded.renderedBulletInstances===18"
              "&&loaded.decals===12"
              "&&loaded.decalRecycles>=5&&loaded.instanceCapHitFrames>0"
              "&&staticBounded&&wedgeBounded&&aggregate<=4096"
              "?'TREADLINE-VISUAL-BUDGET':'TREADLINE-VISUAL-BUDGET-FAIL:'"
              "+JSON.stringify([loaded,staticCounts])",
              "<treadline-visual-budget>", &result);
    if (!treadline_visual_budget_ok
        || strcmp(result.summary, "TREADLINE-VISUAL-BUDGET") != 0)
        fprintf(stderr, "treadline visual budget summary=%s error=%s\n",
                result.summary, result.error);
    CHECK(treadline_visual_budget_ok
          && strcmp(result.summary, "TREADLINE-VISUAL-BUDGET") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
        runtime, "__treadlineDebug.stop();globalThis.pocSummary='STOPPED'",
        "<treadline-stop>", &result));

    script_runtime_set_images(runtime, NULL);
    script_runtime_destroy(runtime);
    images_destroy(&images);
    document_destroy(&document);
    PocDocument client_document;
    CHECK(document_parse(&client_document, &budget, html, html_length, 17));
    ScriptResult client_result;
    ScriptRuntime *client_runtime = script_runtime_create_configured(
        &client_document, &budget, 16u * MIB, 8000, document_url,
        &options, &client_result);
    CHECK(client_runtime != NULL && client_result.success);
    ImageResources client_images = {.budget = &budget};
    script_runtime_set_images(client_runtime, &client_images);
    CHECK(script_runtime_evaluate_diagnostic(
              client_runtime,
              "globalThis.__treadlineEnableDebug=true;",
              "<treadline-client-debug>", &client_result));
    CHECK(script_runtime_evaluate_diagnostic(
              client_runtime, arena_generator,
              "treadline-arena/arena-generator.js", &client_result));
    CHECK(script_runtime_evaluate_diagnostic(
              client_runtime, script, "treadline-arena/game.js",
              &client_result));
    char client_probe[4096];
    int client_probe_length = snprintf(
        client_probe, sizeof(client_probe),
        "const parts='%s'.split('|');"
        "const decode=hex=>{const out=new Uint8Array(hex.length/2);"
        "for(let at=0;at<out.length;at++)out[at]=parseInt("
        "hex.slice(at*2,at*2+2),16);return out};"
        "const packet=decode(parts[0]),expected=decode(parts[1]);"
        "const applied=__treadlineDebug.applyOnlinePacket(packet.buffer);"
        "const actual=new Float32Array(104);let n=0;"
        "for(const list of [__treadlineArenaData.generatedArena.obstacles,"
        "__treadlineArenaData.generatedArena.barriers,"
        "__treadlineArenaData.generatedArena.ramps,"
        "__treadlineArenaData.generatedArena.gates])"
        "for(const item of list)for(let f=0;f<item.length;f++)actual[n++]=item[f];"
        "const bytes=new Uint8Array(actual.buffer);let same=bytes.length===expected.length;"
        "for(let at=0;at<bytes.length&&same;at++)same=bytes[at]===expected[at];"
        "const state=__treadlineDebug.snapshot(),view=new DataView(packet.buffer);"
        "globalThis.pocSummary=applied&&same&&state.arena===3"
        "&&state.arenaSeed===view.getUint32(24,true)"
        "&&state.arenaGenerationChecksum===view.getUint16(28,true)"
        "&&state.barriers===3?'TREADLINE-FRESH-ONLINE-ARENA'"
        ":'TREADLINE-FRESH-ONLINE-ARENA-FAIL:'+JSON.stringify({applied,same,state});",
        generated_online_fixture);
    CHECK(client_probe_length > 0
          && (size_t)client_probe_length < sizeof(client_probe));
    CHECK(script_runtime_evaluate_diagnostic(
              client_runtime, client_probe,
              "<treadline-fresh-online-arena>", &client_result)
          && strcmp(client_result.summary,
                    "TREADLINE-FRESH-ONLINE-ARENA") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              client_runtime,
              "__treadlineDebug.stop();globalThis.pocSummary='STOPPED'",
              "<treadline-client-stop>", &client_result));
    script_runtime_set_images(client_runtime, NULL);
    script_runtime_destroy(client_runtime);
    images_destroy(&client_images);
    document_destroy(&client_document);
    free(html);
    free(script);
    free(css);
    free(manifest_json);
    free(icon);
    free(web_multiplayer);
    free(arena_generator);
    CHECK(budget.current == 0
          && budget_active_allocations(&budget, NULL) == 0);
    return true;
}

static bool cache_offline_resource(
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

static bool stage_treadline_offline_fixture(BrowserEngine *engine)
{
    const char *directory = getenv("TILEFINCH_TREADLINE_STAGE_DIR");
    if (directory == NULL || directory[0] == '\0') return true;
    static const char base[] =
        "https://games.test/examples/treadline-arena/";
    static const struct {
        const char *path;
        const char *type;
        TilefinchRequestDestination destination;
    } resources[] = {
        {"game.css", "text/css", TILEFINCH_DESTINATION_STYLE},
        {"multiplayer-web.js", "text/javascript", TILEFINCH_DESTINATION_SCRIPT},
        {"arena-generator.js", "text/javascript", TILEFINCH_DESTINATION_SCRIPT},
        {"game.js", "text/javascript", TILEFINCH_DESTINATION_SCRIPT},
        {"manifest.webmanifest", "application/manifest+json",
         TILEFINCH_DESTINATION_FETCH},
        {"icon.svg", "image/svg+xml", TILEFINCH_DESTINATION_IMAGE},
    };
    char *html = NULL, *manifest_json = NULL, *icon = NULL;
    size_t html_length = 0, manifest_length = 0, icon_length = 0;
    html = read_source("examples/treadline-arena/index.html", &html_length);
    manifest_json = read_source(
        "examples/treadline-arena/manifest.webmanifest", &manifest_length);
    icon = read_source("examples/treadline-arena/icon.svg", &icon_length);
    CHECK(html != NULL && manifest_json != NULL && icon != NULL);

    browser_session_cache_clear(browser_engine_session(engine));
    for (size_t at = 0; at < sizeof(resources) / sizeof(resources[0]); at++) {
        char source_path[160], url[192];
        int source_written = snprintf(
            source_path, sizeof(source_path), "examples/treadline-arena/%s",
            resources[at].path);
        int url_written = snprintf(
            url, sizeof(url), "%s%s", base, resources[at].path);
        size_t length = 0;
        char *data = source_written > 0
                && (size_t) source_written < sizeof(source_path)
            ? read_source(source_path, &length) : NULL;
        CHECK(data != NULL && url_written > 0
              && (size_t) url_written < sizeof(url)
              && cache_offline_resource(
                  engine, "https://games.test/examples/treadline-arena/index.html",
                  url, resources[at].type, resources[at].destination,
                  TILEFINCH_CREDENTIALS_INCLUDE,
                  (const unsigned char *) data, length));
        free(data);
    }

    PocDocument document;
    CHECK(document_parse(
        &document, browser_engine_budget(engine), html, html_length, 512));
    TilefinchWebAppManifest manifest = {0};
    char error[256] = {0};
    CHECK(tilefinch_web_app_manifest_parse(
        manifest_json, manifest_length,
        "https://games.test/examples/treadline-arena/manifest.webmanifest",
        "https://games.test/examples/treadline-arena/index.html",
        &manifest, error, sizeof(error)));
    OfflineLibrary library;
    offline_library_init(
        &library, browser_engine_budget(engine), directory);
    uint32_t id = 0;
    CHECK(offline_library_save_web_app(
        &library, &document, browser_engine_session(engine),
        "https://games.test/examples/treadline-arena/index.html", &manifest,
        (const unsigned char *) icon, icon_length, &id,
        error, sizeof(error)));
    fprintf(stderr, "staged Treadline offline app id=%u at %s\n",
            (unsigned) id, directory);
    document_destroy(&document);
    free(html);
    free(manifest_json);
    free(icon);
    return true;
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
    CHECK(strstr(css, "#hud") != NULL
          && strstr(css, "#power") != NULL
          && strstr(css, "text-shadow") == NULL);

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
    char sans[512], serif[512], italic[512], bold[512];
    char serif_bold[512], metric[512], metric_bold[512];
    snprintf(sans, sizeof(sans), "%s/fonts/DejaVuSans-Latin.ttf",
             TILEFINCH_TEST_SOURCE_DIR);
    snprintf(serif, sizeof(serif), "%s/fonts/DejaVuSerif-Latin.ttf",
             TILEFINCH_TEST_SOURCE_DIR);
    snprintf(italic, sizeof(italic),
             "%s/fonts/DejaVuSans-Oblique-Latin.ttf",
             TILEFINCH_TEST_SOURCE_DIR);
    snprintf(bold, sizeof(bold), "%s/fonts/DejaVuSans-Bold-Latin.ttf",
             TILEFINCH_TEST_SOURCE_DIR);
    snprintf(serif_bold, sizeof(serif_bold),
             "%s/fonts/DejaVuSerif-Bold-Latin.ttf",
             TILEFINCH_TEST_SOURCE_DIR);
    snprintf(metric, sizeof(metric),
             "%s/fonts/TilefinchSans-Regular.ttf",
             TILEFINCH_TEST_SOURCE_DIR);
    snprintf(metric_bold, sizeof(metric_bold),
             "%s/fonts/TilefinchSans-Bold.ttf",
             TILEFINCH_TEST_SOURCE_DIR);
    CHECK(browser_config_set_font_paths(
              &config, sans, serif, italic, bold, serif_bold,
              metric, metric_bold, 1536u * 1024u));
    browser_config_set_staged_font_loading(&config, true);
    char engine_error[256] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, engine_error, sizeof(engine_error));
    CHECK(engine != NULL
          && browser_engine_prepare_native_home_font(engine)
          && !browser_engine_baseline_fonts_ready(engine));
    CHECK(cache_offline_resource(
              engine, document_url, css_url, "text/css",
              TILEFINCH_DESTINATION_STYLE, TILEFINCH_CREDENTIALS_INCLUDE,
              (const unsigned char *) css, css_length)
          && cache_offline_resource(
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
    const char *stage_directory = getenv("TILEFINCH_PRISM_STAGE_DIR");
    bool keep_staged_app = stage_directory != NULL
        && stage_directory[0] != '\0';
    char directory[OFFLINE_LIBRARY_DIRECTORY_LIMIT];
    if (keep_staged_app) {
        int directory_length = snprintf(
            directory, sizeof(directory), "%s", stage_directory);
        CHECK(directory_length > 0
              && (size_t) directory_length < sizeof(directory));
    } else {
        int directory_length = snprintf(
            directory, sizeof(directory),
            "/tmp/tilefinch-prism-offline-XXXXXX");
        CHECK(directory_length > 0
              && (size_t) directory_length < sizeof(directory));
        CHECK(mkdtemp(directory) != NULL);
    }
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
    budget_free(browser_engine_budget(engine), restored_html);
    PspOfflineStore route_store;
    psp_offline_store_init(
        &route_store, browser_engine_budget(engine),
        browser_engine_session(engine), directory);
    char route_url[128];
    snprintf(route_url, sizeof(route_url),
             "https://tilefinch.local/offline/app?id=%u", (unsigned) id);
    BrowserProfile *profile = browser_profile_create(
        browser_engine_budget(engine));
    CHECK(profile != NULL
          && browser_engine_set_javascript_enabled(engine, false)
          && psp_offline_store_handle_url(
              &route_store, engine, profile, route_url, NULL, true)
              == PSP_OFFLINE_ROUTE_PAGE
          && browser_engine_baseline_fonts_ready(engine)
          && browser_engine_font_face(engine, FONT_SANS) != NULL
          && browser_engine_font_face(engine, FONT_METRIC_SANS) != NULL);
    BrowserViewSnapshot offline_view = {0};
    CHECK(browser_engine_view_snapshot(engine, &offline_view)
          && offline_view.has_focus
          && offline_view.focus_has_authored_outline);
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
          && navigation->page.runtime != NULL && gamepad_available
          && navigation->page.script_result
                 .external_script_bytecode_cache_hits == 1u
          && navigation->page.script_result
                 .external_script_bytecode_cache_misses == 0u);

    ScriptResult game_result;
    bool visible_layout_changed = false;
    CHECK(script_runtime_evaluate_diagnostic(
              navigation->page.runtime,
              "__prismBreakDebug.start();__prismBreakDebug.step(1);"
              "globalThis.pocSummary='PRISM-OVERLAY-STARTED'",
              "<prism-overlay-start>", &game_result)
          && strcmp(game_result.summary, "PRISM-OVERLAY-STARTED") == 0
          && browser_engine_advance_runtime(
              engine, 16, 8, &visible_layout_changed)
          && browser_engine_render_frame(engine, NULL));
    for (unsigned frame = 0; frame < 3; frame++) {
        CHECK(browser_engine_advance_runtime(
                  engine, 16, 8, &visible_layout_changed)
              && browser_engine_render_frame(engine, NULL));
    }
    const TileCache *render = browser_engine_render_metrics_view(engine);
    if (render == NULL || !render->canvas_overlay_ready) {
        fprintf(stderr,
                "offline Prism overlay: ready=%d builds=%zu "
                "fast=%zu regions=%zu pixels=%zu\n",
                render != NULL && render->canvas_overlay_ready,
                render == NULL ? 0u : render->canvas_overlay_builds,
                render == NULL ? 0u : render->canvas_fast_frames,
                render == NULL ? 0u : render->canvas_overlay_region_count,
                render == NULL ? 0u : render->canvas_overlay_pixel_count);
        if (render != NULL && render->layout != NULL) {
            fprintf(stderr, "offline Prism layout: commands=%zu sticky=%zu "
                    "fixed=%zu late=%zu overflow=%zu\n",
                    render->layout->count, render->layout->sticky_count,
                    render->layout->fixed_count,
                    render->layout->late_positioned_order_count,
                    render->layout->overflow_order_count);
            for (size_t order = 0; order < render->layout->count; order++) {
                size_t index = render->layout->paint_order[order];
                const DrawCommand *command = &render->layout->commands[index];
                uint8_t flags = render->layout->command_flags == NULL
                    ? 0 : render->layout->command_flags[index];
                fprintf(stderr,
                        "offline Prism command: order=%zu index=%zu type=%u "
                        "flags=%02x rect=%d,%d,%d,%d blend=%u canvas=%d\n",
                        order, index, command->type, flags, command->x,
                        command->y, command->width, command->height,
                        draw_command_blend_mode(command),
                        command->type == DRAW_IMAGE && command->image != NULL
                            && command->image->is_canvas);
            }
        }
    }
    CHECK(render != NULL && render->canvas_overlay_ready
          && render->canvas_overlay_builds == 1
          && render->canvas_overlay_region_count
                 <= TILEFINCH_CANVAS_OVERLAY_REGION_LIMIT
          && render->canvas_overlay_pixel_count <= 32768u);
    uint64_t steady_started = tilefinch_platform_monotonic_time_us();
    for (unsigned frame = 0; frame < 8; frame++) {
        CHECK(browser_engine_advance_runtime(
                  engine, 16, 8, &visible_layout_changed)
              && browser_engine_render_frame(engine, NULL));
    }
    uint64_t steady_elapsed =
        tilefinch_platform_monotonic_time_us() - steady_started;
    size_t patches_before = render->canvas_overlay_patches;
    size_t patch_regions_before = render->canvas_overlay_patch_regions;
    uint64_t score_started = tilefinch_platform_monotonic_time_us();
    CHECK(script_runtime_evaluate_diagnostic(
              navigation->page.runtime,
              "__prismBreakDebug.setScore(100);"
              "globalThis.pocSummary=__prismBreakDebug.snapshot()"
              ".hudGlyphs>=6?'PRISM-SCORE-CHANGED':'PRISM-SCORE-EMPTY'",
              "<prism-score-change>", &game_result)
          && strcmp(game_result.summary, "PRISM-SCORE-CHANGED") == 0
          && browser_engine_advance_runtime(
              engine, 16, 8, &visible_layout_changed)
          && browser_engine_render_frame(engine, NULL));
    uint64_t score_elapsed =
        tilefinch_platform_monotonic_time_us() - score_started;
    uint64_t power_started = tilefinch_platform_monotonic_time_us();
    CHECK(script_runtime_evaluate_diagnostic(
              navigation->page.runtime,
              "__prismBreakDebug.start();__prismBreakDebug.launch();"
              "__prismBreakDebug.injectPower('multi');"
              "__prismBreakDebug.step(1);"
              "globalThis.pocSummary=__prismBreakDebug.snapshot().balls===3"
              "?'PRISM-POWER-CHANGED':'PRISM-POWER-FAILED'",
              "<prism-power-change>", &game_result)
          && strcmp(game_result.summary, "PRISM-POWER-CHANGED") == 0
          && browser_engine_advance_runtime(
              engine, 16, 8, &visible_layout_changed)
          && browser_engine_render_frame(engine, NULL));
    uint64_t power_elapsed =
        tilefinch_platform_monotonic_time_us() - power_started;
    uint64_t laser_started = tilefinch_platform_monotonic_time_us();
    CHECK(script_runtime_evaluate_diagnostic(
              navigation->page.runtime,
              "__prismBreakDebug.injectPower('laser');"
              "__prismBreakDebug.launch();__prismBreakDebug.step(1);"
              "globalThis.pocSummary=__prismBreakDebug.snapshot().shots===2"
              "?'PRISM-LASER-FIRED':'PRISM-LASER-FAILED'",
              "<prism-laser-fire>", &game_result)
          && strcmp(game_result.summary, "PRISM-LASER-FIRED") == 0
          && browser_engine_advance_runtime(
              engine, 16, 8, &visible_layout_changed)
          && browser_engine_render_frame(engine, NULL));
    uint64_t laser_elapsed =
        tilefinch_platform_monotonic_time_us() - laser_started;
    render = browser_engine_render_metrics_view(engine);
    fprintf(stderr,
            "prism-overlay-host: steady-average-us=%llu "
            "score-change-us=%llu power-change-us=%llu laser-fire-us=%llu "
            "patches=%zu patch-regions=%zu "
            "regions=%zu pixels=%zu\n",
            (unsigned long long) (steady_elapsed / 8u),
            (unsigned long long) score_elapsed,
            (unsigned long long) power_elapsed,
            (unsigned long long) laser_elapsed,
            render->canvas_overlay_patches - patches_before,
            render->canvas_overlay_patch_regions - patch_regions_before,
            render->canvas_overlay_region_count,
            render->canvas_overlay_pixel_count);
    CHECK(render->canvas_overlay_builds == 1
          && render->canvas_overlay_patches == patches_before
          && render->canvas_overlay_patch_regions == patch_regions_before);
    psp_offline_store_destroy(&route_store);
    browser_profile_destroy(profile);

    if (!keep_staged_app) {
        CHECK(offline_library_remove(&library, id));
        char index_path[OFFLINE_LIBRARY_DIRECTORY_LIMIT + 32u];
        char backup_path[OFFLINE_LIBRARY_DIRECTORY_LIMIT + 32u];
        char temporary_path[OFFLINE_LIBRARY_DIRECTORY_LIMIT + 32u];
        snprintf(index_path, sizeof(index_path), "%s/library.bin", directory);
        snprintf(backup_path, sizeof(backup_path), "%s/library.bin.bak",
                 directory);
        snprintf(temporary_path, sizeof(temporary_path), "%s/library.bin.tmp",
                 directory);
        (void) unlink(index_path);
        (void) unlink(backup_path);
        (void) unlink(temporary_path);
    }
    CHECK(stage_treadline_offline_fixture(engine));
    browser_engine_destroy(engine);
    if (!keep_staged_app) CHECK(rmdir(directory) == 0);
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
    puts("test: translated WebGL geometry cache is exact and bounded");
    if (!run_webgl_geometry_cache_admission()) return 1;
    puts("test: WebGL temporal AA admits only bounded camera motion");
    if (!run_webgl_temporal_matrix_admission()) return 1;
    puts("test: WebGL skips only disposable fully-cleared depth readback");
    if (!run_webgl_depth_readback_policy()) return 1;
    puts("test: WebGL AA work skips oversized meshes without consuming quota");
    if (!run_webgl_antialias_edge_policy()) return 1;
    puts("test: refused WebGL color/depth preparation leaves Budget ownership");
    if (!run_webgl_budget_refusal_ownership()) return 1;
    puts("test: representative Canvas game advances, responds, and pauses");
    if (!run_fixture("canvas-game.js", "CANVAS-GAME-PASS", true, true))
        return 1;
    puts("test: representative indexed WebGL game renders repeated frames");
    if (!run_fixture("webgl-game.js", "WEBGL-GAME-PASS", false, true))
        return 1;
    puts("test: bounded WebGL instancing retains geometry and transforms copies");
    if (!run_fixture(
            "webgl-instancing.js", "WEBGL-INSTANCING-PASS", false, false))
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
    puts("test: Treadline Arena exercises modes, gadgets, and tank combat");
    if (!run_treadline_arena_game()) return 1;
    puts("test: installed Prism Break reopens from its offline resource pack");
    if (!run_prism_break_offline_reopen()) return 1;
    puts("tilefinch-canvas-webgl-conformance-tests: all checks passed");
    return 0;
}
