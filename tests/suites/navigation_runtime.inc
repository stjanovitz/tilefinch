#define NAVIGATION_RUNTIME_JS_HEAP (2 * MIB + 256 * 1024u)

static void script_frontier_probe(void *opaque, size_t script_index,
                                  bool active)
{
    ScriptFrontierProbe *probe = opaque;
    if (probe == NULL) return;
    if (!active) {
        probe->final_calls++;
        probe->final_index = script_index;
        return;
    }
    if (probe->active_calls < sizeof(probe->indices)
                              / sizeof(probe->indices[0])) {
        probe->indices[probe->active_calls] = script_index;
    }
    probe->active_calls++;
}

static bool test_canvas_surface_survives_and_updates_without_relayout(void)
{
    static const char html[] =
        "<!doctype html><style>body{margin:0}</style>"
        "<canvas id=chart width=4 height=2>Fallback chart</canvas><script>"
        "const c=document.getElementById('chart').getContext('2d');"
        "c.fillStyle='#ff0000';c.fillRect(0,0,4,2);"
        "setTimeout(()=>{c.fillStyle='#0000ff';c.fillRect(2,0,2,2)},1)"
        "</script>";
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool ready = installed && navigation_init(&navigation, &budget, 4);
    if (ready) {
        /* This is a canvas publication test, not the minimum-heap boundary;
           allow the boot-captured Worker security intrinsics to coexist with
           the timer/canvas graph. Device defaults use a 5 MiB heap. */
        navigation_enable_scripts(
            &navigation, NAVIGATION_RUNTIME_JS_HEAP, 1000);
        navigation_enable_document_scripts(
            &navigation, 4, 32 * 1024, 16 * 1024, 1000);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool committed = ready && navigation_commit_html(
        &navigation, generation, "https://canvas.test/", html,
        sizeof(html) - 1u, 480, NULL, NULL, true);
    const ImageResource *canvas = NULL;
    bool painted = false;
    if (committed) {
        for (size_t i = 0; i < navigation.page.images.count; i++) {
            if (navigation.page.images.items[i].is_canvas) {
                canvas = &navigation.page.images.items[i];
                break;
            }
        }
        for (size_t i = 0; i < navigation.page.layout.count; i++) {
            if (navigation.page.layout.commands[i].type == DRAW_IMAGE
                && navigation.page.layout.commands[i].image == canvas) {
                painted = true;
                break;
            }
        }
    }
    bool initial = canvas != NULL && canvas->width == 4 && canvas->height == 2
        && canvas->pixels != NULL
        && canvas->pixels[0] == 255 && canvas->pixels[1] == 0
        && canvas->pixels[2] == 0 && canvas->pixels[3] == 255
        && canvas->pixels[8] == 255 && canvas->pixels[10] == 0;
    size_t relayouts = navigation.incremental_relayouts;
    size_t dom_mutations = navigation.page.script_result.dom_mutations;
    uint64_t content_generation = navigation.page.document.content_generation;
    bool advanced = initial && painted
        && navigation_advance_runtime(&navigation, 2, 4);
    canvas = navigation.page.images.count != 0
        ? &navigation.page.images.items[0] : NULL;
    bool updated = advanced && canvas != NULL && canvas->is_canvas
        && canvas->pixels[0] == 255 && canvas->pixels[2] == 0
        && canvas->pixels[8] == 0 && canvas->pixels[10] == 255
        && navigation.page.script_result.dom_mutations == dom_mutations
        && navigation.page.document.content_generation == content_generation
        && navigation.incremental_relayouts == relayouts + 1u
        && navigation.canvas_paint_damage_pending;
    bool ok = committed && initial && painted && updated;
    if (!ok) {
        lxb_dom_node_t *chart = test_find_id(
            lxb_dom_interface_node(navigation.page.document.html), "chart");
        size_t parent_name_length = 0;
        const char *parent_name = chart == NULL ? NULL
            : document_element_name(chart->parent, &parent_name_length);
        fprintf(stderr,
                "canvas bridge ready=%d committed=%d images=%zu painted=%d "
                "initial=%d advanced=%d updated=%d relayouts=%zu/%zu "
                "node=%p image-node=%p parent=%.*s connected=%d error=\"%s\"\n",
                ready, committed, navigation.page.images.count, painted,
                initial, advanced, updated, relayouts,
                navigation.incremental_relayouts, (void *) chart,
                canvas == NULL ? NULL : (void *) canvas->node,
                (int) parent_name_length,
                parent_name == NULL ? "" : parent_name,
                chart != NULL && chart->parent != NULL,
                navigation.last_error);
    }
    if (ready) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_canvas_fallback_text_mutation_remains_replaced(void)
{
    static const char html[] =
        "<!doctype html><canvas id=clock width=4 height=2>Fallback</canvas>"
        "<script>const c=document.getElementById('clock').getContext('2d');"
        "c.fillStyle='red';c.fillRect(0,0,4,2);setTimeout(()=>{"
        "c.canvas.innerText='Updated fallback';c.fillStyle='blue';"
        "c.fillRect(0,0,1,1)},1)</script>";
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool ready = installed && navigation_init(&navigation, &budget, 4);
    if (ready) {
        navigation_enable_scripts(
            &navigation, NAVIGATION_RUNTIME_JS_HEAP, 1000);
        navigation_enable_document_scripts(
            &navigation, 4, 32 * 1024, 16 * 1024, 1000);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool committed = ready && navigation_commit_html(
        &navigation, generation, "https://canvas.test/clock", html,
        sizeof(html) - 1u, 480, NULL, NULL, true);
    bool advanced = committed
        && navigation_advance_runtime(&navigation, 2, 4);
    lxb_dom_node_t *clock = committed ? test_find_id(
        lxb_dom_interface_node(navigation.page.document.html), "clock") : NULL;
    const ImageResource *image = clock == NULL ? NULL
        : images_find_node(&navigation.page.images, clock);
    bool painted = false;
    for (size_t i = 0; image != NULL
         && i < navigation.page.layout.count; i++) {
        if (navigation.page.layout.commands[i].type == DRAW_IMAGE
            && navigation.page.layout.commands[i].image == image) {
            painted = true;
            break;
        }
    }
    bool ok = advanced && image != NULL && image->is_canvas
        && image->pixels != NULL && image->pixels[2] == 255
        && painted && navigation.page.layout.height > 0
        && navigation.last_error[0] == '\0';
    if (ready) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_canvas_oversized_dimensions_are_bounded(void)
{
    static const char html[] =
        "<!doctype html><canvas id=large width=640 height=480></canvas>"
        "<script>const c=document.getElementById('large');"
        "const x=c.getContext('2d');x.fillStyle='#00ff00';"
        "x.fillRect(0,0,c.width,c.height)</script>";
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool ready = installed && navigation_init(&navigation, &budget, 4);
    if (ready) {
        navigation_enable_scripts(&navigation, 3 * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 4, 32 * 1024, 16 * 1024, 1000);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool committed = ready && navigation_commit_html(
        &navigation, generation, "https://canvas.test/bounded", html,
        sizeof(html) - 1u, 480, NULL, NULL, true);
    const ImageResource *image = committed
        && navigation.page.images.count == 1
        ? &navigation.page.images.items[0] : NULL;
    bool ok = image != NULL && image->is_canvas && image->pixels != NULL
        && image->width == 362 && image->height == 272
        && image->pixels[0] == 0 && image->pixels[1] == 255
        && image->pixels[2] == 0 && image->pixels[3] == 255
        && navigation.last_error[0] == '\0';
    if (ready) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_canvas_surface_count_is_bounded(void)
{
    static const char html[] =
        "<!doctype html><body>"
        "<canvas></canvas><canvas></canvas><canvas></canvas>"
        "<canvas></canvas><canvas></canvas><script>"
        "for(const canvas of document.querySelectorAll('canvas')){"
        "canvas.width=8;canvas.height=8;"
        "const context=canvas.getContext('2d');"
        "context.fillStyle='#123456';context.fillRect(0,0,8,8)}"
        "</script></body>";
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool ready = installed && navigation_init(&navigation, &budget, 4);
    if (ready) {
        /* This exercises the surface-count policy, not the minimum viable JS
           heap. Leave bounded headroom for the lazily installed Canvas API;
           backing arrays are still charged to the 16 MiB page budget. */
        navigation_enable_scripts(&navigation, 3 * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 4, 32 * 1024, 16 * 1024, 1000);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool committed = ready && navigation_commit_html(
        &navigation, generation, "https://canvas.test/quota", html,
        sizeof(html) - 1u, 480, NULL, NULL, true);
    size_t canvas_count = 0;
    for (size_t i = 0; committed && i < navigation.page.images.count; i++) {
        if (navigation.page.images.items[i].is_canvas) canvas_count++;
    }
    bool ok = committed && navigation.page.images.count == 4u
        && canvas_count == 4u
        && navigation.page.images.stats.decoded_bytes == 4u * 8u * 8u * 4u;
    if (!ok) {
        fprintf(stderr,
                "canvas quota ready=%d committed=%d images=%zu canvases=%zu "
                "bytes=%zu error=\"%s\"\n",
                ready, committed, navigation.page.images.count, canvas_count,
                navigation.page.images.stats.decoded_bytes,
                navigation.last_error);
    }
    if (ready) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_image_load_tasks_case(bool deferred)
{
    static const char format[] =
        "<!doctype html><body><img id=source %s class=loading width=1 height=1 "
        "src='%s' "
        "onload=\"document.getElementById('retired')?.remove();"
        "this.classList.remove('loading');globalThis.loads++;"
        "globalThis.valid=event.isTrusted&&!event.bubbles&&!event.cancelable"
        "&&this.complete&&this.naturalWidth>0\">"
        "<img id=retired width=1 height=1 src='%s' "
        "onload='globalThis.retiredLoads++'>"
        "<script>globalThis.loads=0;globalThis.retiredLoads=0;globalThis.valid=false;"
        "setInterval(()=>{},1000)</script></body>";
    char html[2048];
    const char *source = deferred
        ? "https://interactive-images.test/hero.svg"
        : "data:image/webp;base64,"
          "UklGRh4AAABXRUJQVlA4TBEAAAAvAUAAAAfQssI0uf+BiOh/AAA=";
    snprintf(html, sizeof(html), format, deferred ? "loading=lazy" : "",
             source, deferred ? "" : source);
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool ready = installed && navigation_init(&navigation, &budget, 4);
    char error[256] = {0};
    bool trace = deferred && fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR "/tests/fixtures/http-interactive-images",
        error, sizeof(error));
    if (ready) {
        navigation_enable_scripts(&navigation, 3 * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 4, 32 * 1024, 16 * 1024, 1000);
        if (!deferred) navigation_enable_external_resources(
            &navigation, 2, 32 * 1024, 16 * 1024,
            2, 32 * 1024, 16 * 1024, 64 * 1024, 1000);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool ok = ready && (!deferred || trace)
        && navigation_commit_html(&navigation, generation,
        "https://interactive-images.test/page", html, strlen(html),
        480, NULL, NULL, true);
    if (ok && deferred) {
        /* Start the actual deferred transport after the runtime and fixed
           placeholder exist; an eager commit must not satisfy this test. */
        navigation_enable_external_resources(
            &navigation, 2, 32 * 1024, 16 * 1024,
            2, 32 * 1024, 16 * 1024, 64 * 1024, 1000);
        navigation.page.deferred_image_targets = budget_calloc_category(
            &budget, BUDGET_CATEGORY_RESOURCE, 1, sizeof(ImagePriorityTarget));
        lxb_dom_node_t *source_node = test_find_id(
            lxb_dom_interface_node(navigation.page.document.html), "source");
        ok = navigation.page.deferred_image_targets != NULL && source_node != NULL;
        if (ok) {
            navigation.page.deferred_image_targets[0] = (ImagePriorityTarget) {
                .node = source_node, .kind = IMAGE_PRIORITY_KIND_DOCUMENT,
                .weak_handle = script_runtime_node_weak_handle(navigation.page.runtime, source_node)
            };
            navigation.page.deferred_image_count = 1;
            navigation.page.image_continuation_pending = true;
        }
        const DrawCommand *commands = navigation.page.layout.commands;
        size_t image_relayouts = navigation.performance.background_image_relayouts;
        ok = ok && navigation.page.images.stats.loaded == 0
            && navigation.page.image_continuation_pending
            && script_runtime_flush_geometry_layout(navigation.page.runtime,
                                                     &navigation.viewport);
        for (size_t i = 0; ok && i < 256
             && navigation.page.image_continuation_pending; i++)
            ok = navigation_run_deferred_image_work(&navigation);
        ok = ok && !navigation.page.image_continuation_pending
            && navigation.page.layout.commands == commands
            && navigation.performance.background_image_relayouts == image_relayouts;
        LayoutDocument stale = {0};
        bool adopted = script_runtime_take_geometry_layout(navigation.page.runtime,
            &navigation.viewport, &stale);
        layout_destroy(&stale);
        ok = ok && !adopted;
    }
    ScriptResult result = {0};
    if (ok) ok = script_runtime_evaluate_diagnostic(navigation.page.runtime,
        "globalThis.pocSummary=String(loads)", "<image-before-task>", &result)
        && strcmp(result.summary, "0") == 0;
    if (ok) ok = navigation_advance_runtime(&navigation, 0, 0)
        && script_runtime_evaluate_diagnostic(navigation.page.runtime,
            "globalThis.pocSummary=String(loads)", "<image-zero-budget>", &result)
        && strcmp(result.summary, "0") == 0;
    if (ok) ok = navigation_advance_runtime(&navigation, 0, 4)
        && script_runtime_evaluate_diagnostic(navigation.page.runtime,
            "globalThis.pocSummary=loads===1&&valid&&"
            "!document.getElementById('source').classList.contains('loading')"
            "?'loaded':'failed:'+loads+':'+valid", "<image-after-task>", &result)
        && strcmp(result.summary, "loaded") == 0;
    /* Reattaching the same pixels during a relayout must not re-fire load. */
    if (ok) ok = navigation_relayout(&navigation)
        && navigation_advance_runtime(&navigation, 0, 4)
        && script_runtime_evaluate_diagnostic(navigation.page.runtime,
            "globalThis.pocSummary=retiredLoads===0?String(loads):'retired-fired'",
            "<image-repeat>", &result)
        && strcmp(result.summary, "1") == 0;
    if (!ok) fprintf(stderr, "image load task: deferred=%d trace=%d loaded=%d "
        "pending=%d count=%zu images=%zu attempted=%zu failures=%zu "
        "summary=%s error=%s navigation=%s\n", deferred, trace,
        navigation.page.loaded, navigation.page.image_continuation_pending,
        navigation.page.deferred_image_count, navigation.page.images.count,
        navigation.page.images.stats.attempted, navigation.page.images.stats.failed,
        result.summary, result.error, navigation.last_error);
    if (ready) navigation_destroy(&navigation);
    if (trace) fetch_trace_end();
    bool clean = budget.current == 0 && budget_categories_reconcile(&budget);
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_candidate_ready_event_reads_used_geometry(void)
{
    static const char html[] =
        "<!doctype html><style>body{margin:0}.frame{width:240px;height:135px}"
        ".frame img{display:block;width:100%;height:100%}</style>"
        "<div class=frame><img id=poster></div><script>"
        "document.addEventListener('DOMContentLoaded',()=>{"
        "const image=document.getElementById('poster');"
        "globalThis.pocSummary=image.clientWidth+'x'+image.clientHeight"
        "})</script>";
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool ready = installed && navigation_init(&navigation, &budget, 2);
    if (ready) {
        navigation_enable_scripts(&navigation, 3 * MIB, 1000);
        navigation_enable_document_scripts(&navigation, 4, 32 * 1024, 16 * 1024, 1000);
    }
    bool ok = ready && navigation_commit_html(&navigation,
        navigation_begin(&navigation), "https://geometry.test/candidate", html,
        sizeof(html) - 1u, 480, NULL, NULL, true)
        && strcmp(navigation.page.script_result.summary, "240x135") == 0;
    ScriptResult result = {0};
    if (ok) ok = script_runtime_evaluate_diagnostic(navigation.page.runtime,
        "const frame=document.querySelector('.frame');frame.style.width='200px';"
        "globalThis.pocSummary=frame.clientWidth+'x'+document.getElementById('poster').clientWidth",
        "<committed-geometry>", &result) && strcmp(result.summary, "200x200") == 0;
    if (!ok) fprintf(stderr, "candidate geometry summary=%s error=%s\n",
        navigation.page.script_result.summary, navigation.page.script_result.error);
    if (ready) navigation_destroy(&navigation);
    bool clean = budget.current == 0 && budget_categories_reconcile(&budget);
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_image_load_tasks_follow_publication(void)
{
    return test_candidate_ready_event_reads_used_geometry()
        && test_image_load_tasks_case(false) && test_image_load_tasks_case(true);
}

static bool test_canvas_draw_image_accepts_loaded_html_image(void)
{
    static const char html[] =
        "<!doctype html><body><img id=source width=1 height=1 "
        "src='data:image/webp;base64,"
        "UklGRh4AAABXRUJQVlA4TBEAAAAvAUAAAAfQssI0uf+BiOh/AAA='>"
        "<canvas id=target width=1 height=1></canvas>"
        "<script>setInterval(()=>{},1000)</script></body>";
    static const char probe[] =
        "(()=>{const canvas=document.getElementById('target'),"
        "context=canvas.getContext('2d');"
        "context.drawImage(document.getElementById('source'),0,0);"
        "const pixel=context.getImageData(0,0,1,1).data;"
        "globalThis.pocSummary=pixel[3]===255?'canvas-image:ok':"
        "'canvas-image:fail:'+Array.from(pixel).join(',')})()";
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool ready = installed && navigation_init(&navigation, &budget, 4);
    if (ready) {
        /* Canvas plus a second author diagnostic evaluation intentionally
           exercises two independently admitted script surfaces; keep this
           compatibility test above the 2 MiB single-surface floor. */
        navigation_enable_scripts(&navigation, 3 * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 4, 32 * 1024, 16 * 1024, 1000);
        navigation_enable_external_resources(
            &navigation, 2, 32 * 1024, 16 * 1024,
            2, 32 * 1024, 16 * 1024, 64 * 1024, 1000);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool committed = ready && navigation_commit_html(
        &navigation, generation, "https://canvas.test/image", html,
        sizeof(html) - 1u, 480, NULL, NULL, true);
    ScriptResult result = {0};
    bool evaluated = committed && navigation.page.runtime != NULL
        && script_runtime_evaluate_diagnostic(
            navigation.page.runtime, probe, "<canvas-image>", &result);
    bool relaid = evaluated && navigation_relayout(&navigation);
    lxb_dom_node_t *target = committed ? test_find_id(
        lxb_dom_interface_node(navigation.page.document.html), "target")
        : NULL;
    const ImageResource *canvas = target == NULL ? NULL
        : images_find_node(&navigation.page.images, target);
    bool ok = evaluated && relaid
        && strcmp(result.summary, "canvas-image:ok") == 0
        && canvas != NULL && canvas->is_canvas && canvas->pixels != NULL
        && canvas->pixels[3] == 255;
    if (!ok) {
        fprintf(stderr,
                "canvas image ready=%d committed=%d runtime=%d eval=%d "
                "relayout=%d summary=\"%s\" error=\"%s\" images=%zu\n",
                ready, committed, navigation.page.runtime != NULL,
                evaluated, relaid, result.summary, result.error,
                navigation.page.images.count);
    }
    if (ready) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_detached_canvas_publishes_when_inserted(void)
{
    static const char html[] =
        "<!doctype html><body><script>"
        "const canvas=document.createElement('canvas');"
        "canvas.id='late-canvas';canvas.width=3;canvas.height=2;"
        "const context=canvas.getContext('2d');"
        "context.fillStyle='#abcdef';context.fillRect(0,0,3,2);"
        "setTimeout(()=>document.body.appendChild(canvas),1)"
        "</script></body>";
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool ready = installed && navigation_init(&navigation, &budget, 4);
    if (ready) {
        navigation_enable_scripts(&navigation, NAVIGATION_RUNTIME_JS_HEAP, 1000);
        navigation_enable_document_scripts(
            &navigation, 4, 32 * 1024, 16 * 1024, 1000);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool committed = ready && navigation_commit_html(
        &navigation, generation, "https://canvas.test/detached", html,
        sizeof(html) - 1u, 480, NULL, NULL, true);
    bool initially_absent = committed && navigation.page.images.count == 0;
    bool advanced = initially_absent
        && navigation_advance_runtime(&navigation, 2, 8);
    lxb_dom_node_t *canvas_node = advanced ? test_find_id(
        lxb_dom_interface_node(navigation.page.document.html), "late-canvas")
        : NULL;
    const ImageResource *canvas = canvas_node == NULL ? NULL
        : images_find_node(&navigation.page.images, canvas_node);
    bool ok = advanced && canvas != NULL && canvas->is_canvas
        && canvas->width == 3 && canvas->height == 2
        && canvas->pixels != NULL
        && canvas->pixels[0] == 0xabu && canvas->pixels[1] == 0xcdu
        && canvas->pixels[2] == 0xefu && canvas->pixels[3] == 255u;
    if (!ok) {
        fprintf(stderr,
                "detached canvas ready=%d committed=%d absent=%d advanced=%d "
                "images=%zu node=%p canvas=%p error=\"%s\"\n",
                ready, committed, initially_absent, advanced,
                navigation.page.images.count, (void *) canvas_node,
                (const void *) canvas, navigation.last_error);
    }
    if (ready) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_stream_teardown_after_realm_retirement(void)
{
    static const char html[] =
        "<!doctype html><title>Retained after OOM</title>"
        "<body><script>globalThis.started=true;</script>"
        "<main><h1>Readable content</h1><a href='/next'>Next</a></main>";
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool ready = installed && navigation_init(&navigation, &budget, 2);
    if (ready) {
        navigation_enable_scripts(&navigation, 4 * MIB, 1500);
        navigation_enable_document_scripts(
            &navigation, 4, 64 * 1024u, 64 * 1024u, 1500);
        navigation_set_stream_delivery(&navigation, 64, 0, 0, 0, 0, 0);
    }
    bool loaded = ready && navigation_commit_html(
        &navigation, navigation_begin(&navigation),
        "https://realm-retirement.test/document", html, sizeof(html)-1,
        480, NULL, NULL, true);
    bool had_runtime = loaded && navigation.page.runtime != NULL;
    if (had_runtime) navigation_test_retired_stream_teardown(&navigation);
    bool ok = loaded && navigation.page.loaded
        && had_runtime
        && navigation.page.runtime == NULL
        && strcmp(navigation.page.document.title, "Retained after OOM") == 0
        && navigation.page.layout.link_count != 0;
    if (!ok) fprintf(stderr, "retired staging ready=%d loaded=%d runtime=%p "
        "limited=%d scripts=%zu/%zu/%zu script-error=%s error=%s\n", ready, loaded,
        (void *)navigation.page.runtime, navigation.page.script_memory_limited,
        navigation.script_attempted, navigation.script_loaded, navigation.script_failed,
        navigation.page.script_result.error,
        navigation.last_error);
    if (installed) navigation_destroy(&navigation);
    bool clean = budget.current == 0 && budget_categories_reconcile(&budget);
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_navigation_keeps_server_dom_after_author_script_error(void)
{
    static const char html[] =
        "<!doctype html><title>Usable result</title><body>"
        "<main id=server-content><h1>Server-rendered page</h1>"
        "<p>This complete server response contains useful navigation and "
        "enough visible prose to remain preferable to an incomplete loading "
        "shell when optional hydration exhausts its bounded execution.</p>"
        "<input id=server-control value=server>"
        "<a href='/next'>Continue reading</a></main>"
        "<script>globalThis.hydrationRuns="
        "(globalThis.hydrationRuns||0)+1;"
        "globalThis.retainedServer=document.getElementById('server-content');"
        "globalThis.retainedControl=document.getElementById('server-control');"
        "globalThis.retainedControl.value='changed';"
        "document.body.innerHTML='<p>Loading</p>';"
        "throw new Error('missing optional dependency')</script>"
        "<script>globalThis.pocSummary='later-script-ran'</script>"
        "</body>";
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool ready = lexbor_installed
        && navigation_init(&navigation, &budget, 4);
    if (ready) {
        navigation_enable_scripts(&navigation, NAVIGATION_RUNTIME_JS_HEAP, 1000);
        navigation_enable_document_scripts(
            &navigation, 8, 64 * 1024, 32 * 1024, 1000);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool committed = ready && navigation_commit_html(
        &navigation, generation, "https://soft-script.test/", html,
        sizeof(html) - 1u, 480, NULL, NULL, true);
    lxb_dom_node_t *content = committed ? test_find_id(
        lxb_dom_interface_node(navigation.page.document.html),
        "server-content") : NULL;
    ScriptResult retained_probe = {0};
    bool retained_safe = committed
        && script_runtime_evaluate_diagnostic(
            navigation.page.runtime,
            "const current=document.getElementById('server-content'),"
            "currentControl=document.getElementById('server-control');"
            "globalThis.pocSummary=[hydrationRuns,retainedServer.isConnected,"
            "retainedServer.textContent.includes('Server-rendered page'),"
            "retainedControl.value,current!==retainedServer,"
            "currentControl.value].join(':')",
            "<hydration-retained-handle-probe>", &retained_probe)
        && strcmp(retained_probe.summary,
                  "1:false:true:changed:true:server") == 0;
    bool ok = committed && navigation.page.loaded && content != NULL
        && retained_safe
        && navigation.script_failed == 1u
        && navigation.page.script_result.scripts_evaluated == 0u
        && navigation.page.script_result.summary[0] == '\0'
        && strstr(navigation.page.script_result.error,
                  "missing optional dependency") != NULL;
    if (!ok) {
        fprintf(stderr,
                "soft script ready=%d committed=%d loaded=%d failed=%zu "
                "evaluated=%zu retained-safe=%d retained=\"%s\" "
                "summary=\"%s\" error=\"%s\" nav=\"%s\"\n",
                ready, committed, navigation.page.loaded,
                navigation.script_failed,
                navigation.page.script_result.scripts_evaluated,
                retained_safe, retained_probe.summary,
                navigation.page.script_result.summary,
                navigation.page.script_result.error,
                navigation.last_error);
    }
    if (ready) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_iframe_discovery_weak_handle_survives_reclaim(void)
{
    static const char html[] =
        "<!doctype html><body><div id=host></div></body>";
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool ready = installed && navigation_init(&navigation, &budget, 4);
    if (ready) navigation_enable_scripts(&navigation, 3 * MIB, 1000);
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool committed = ready && navigation_commit_html(
        &navigation, generation, "https://weak-frame.test/", html,
        sizeof(html) - 1u, 480, NULL, NULL, true);
    ScriptResult result = {0};
    bool inserted = committed && script_runtime_evaluate_diagnostic(
        navigation.page.runtime,
        "document.getElementById('host').innerHTML="
        "'<iframe id=late src=/child></iframe>'",
        "<weak-frame-insert>", &result);
    lxb_dom_node_t *late = inserted ? test_find_id(
        lxb_dom_interface_node(navigation.page.document.html), "late") : NULL;
    long weak = late == NULL ? 0 : script_runtime_node_weak_handle(
        navigation.page.runtime, late);
    bool reclaimed = weak != 0 && script_runtime_evaluate_diagnostic(
        navigation.page.runtime,
        "document.getElementById('host').textContent=''",
        "<weak-frame-reclaim>", &result);
    bool ok = reclaimed
        && script_runtime_node_handle_resolve(
               navigation.page.runtime, weak) == NULL
        && script_runtime_node_handle_resolve_connected(
               navigation.page.runtime, weak) == NULL;
    if (ready) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_document_script_refresh_failure_stops_following_scripts(void)
{
    /* Each script leaves a cookie in the browser session, which outlives
       the refused candidate; the first then mutates the DOM, whose refresh
       is refused. */
    static const char html[] =
        "<!doctype html><body><script>document.cookie='first=ran';"
        "document.body.appendChild(document.createElement('p'))"
        "</script><script>document.cookie='second=ran'</script></body>";
    static const char url[] = "https://refresh-boundary.test/";
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = lexbor_installed
        && browser_session_init(&browser, &budget, 128 * 1024);
    bool ready = browser_ready && navigation_init(&navigation, &budget, 4);
    if (ready) {
        navigation_attach_browser_session(&navigation, &browser);
        navigation_enable_scripts(&navigation, NAVIGATION_RUNTIME_JS_HEAP, 1000);
        navigation_enable_document_scripts(
            &navigation, 4, 32 * 1024, 16 * 1024, 1000);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    if (ready) script_runtime_test_refuse_next_document_refresh();
    bool committed = ready && navigation_commit_html(
        &navigation, generation, url, html, sizeof(html) - 1u, 480,
        NULL, NULL, true);
    char cookies[128] = {0};
    bool cookies_read = browser_ready && browser_session_cookie_get(
        &browser, url, cookies, sizeof(cookies));
    bool ok = ready && !committed && !navigation.page.loaded
        && strstr(navigation.last_error,
                  "could not refresh mutated candidate DOM") != NULL
        && cookies_read && strstr(cookies, "first=ran") != NULL
        && strstr(cookies, "second=ran") == NULL;
    if (!ok) {
        fprintf(stderr,
                "script refresh boundary committed=%d loaded=%d "
                "cookies=\"%s\" error=\"%s\"\n",
                committed, navigation.page.loaded, cookies,
                navigation.last_error);
    }
    if (ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_delayed_import_after_module_pipeline_failure(void)
{
    /* The root module installs the retained module loader during the
       commit's document-script pass, whose metrics live in that pass's
       stack frame; the replay-missing external script then fails the pass.
       A later import must use the loader context's own copy (ASan reports
       the stale frame otherwise). */
    static const char html[] =
        "<!doctype html><body>"
        "<script id=root type=module>"
        "globalThis.loadAfterFailure=()=>import('./late.js')"
        "</script><script src='/missing.js'></script></body>";

    Budget budget;
    budget_init(&budget, 24 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = lexbor_installed
        && browser_session_init(&browser, &budget, 128 * 1024);
    bool ready = browser_ready
        && navigation_init(&navigation, &budget, 4);
    if (ready) {
        navigation_attach_browser_session(&navigation, &browser);
        navigation_enable_scripts(&navigation, 4 * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 8, 64 * 1024, 32 * 1024, 1000);
    }
    bool replay_ready = ready && module_swap_replay_begin();
    uint64_t generation = replay_ready ? navigation_begin(&navigation) : 0;
    bool committed = replay_ready && navigation_commit_html(
        &navigation, generation, "https://module-swap.test/failure", html,
        sizeof(html) - 1, 480, NULL, NULL, true);
    bool failed_after_install = committed && navigation.script_modules == 1
        && navigation.script_failed == 1;
    ScriptResult diagnostic = {0};
    bool imported = failed_after_install
        && script_runtime_evaluate_diagnostic(
            navigation.page.runtime,
            "globalThis.pocSummary='module-failure:pending';"
            "loadAfterFailure().then(module=>{"
            "globalThis.pocSummary='module-failure:'+module.value"
            "}).catch(error=>{globalThis.pocSummary='module-failure:error:'"
            "+error})",
            "<module-failure-import>", &diagnostic);
    for (size_t i = 0; imported && i < 8
         && strcmp(diagnostic.summary, "module-failure:late-ok") != 0; i++) {
        imported = navigation_advance_runtime(&navigation, 0, 32);
        diagnostic = navigation.page.script_result;
    }
    if (replay_ready) fetch_trace_end();
    bool ok = imported
        && strcmp(diagnostic.summary, "module-failure:late-ok") == 0;
    if (!ok) {
        fprintf(stderr,
                "module failure ready=%d replay=%d committed=%d modules=%zu "
                "failed=%zu imported=%d summary=\"%s\" error=\"%s\" "
                "nav=\"%s\"\n",
                ready, replay_ready, committed, navigation.script_modules,
                navigation.script_failed, imported, diagnostic.summary,
                diagnostic.error, navigation.last_error);
    }
    if (ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

#undef NAVIGATION_RUNTIME_JS_HEAP
