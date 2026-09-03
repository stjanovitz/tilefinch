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

static bool test_script_frontier_snapshot_survives_author_mutation(void)
{
    static const char html[] =
        "<!doctype html><title>Frontier snapshot</title><body>"
        "<div id=mover><script id=first>"
        "globalThis.frontierOrder=['first'];"
        "document.body.appendChild(document.getElementById('mover'));"
        "</script></div>"
        "<script id=second>frontierOrder.push('second')</script>"
        "<script id=third>frontierOrder.push('third');"
        "globalThis.pocSummary=frontierOrder.join(',')</script>"
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
        navigation.defer_document_script_pipeline = true;
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool committed = ready && navigation_commit_html(
        &navigation, generation, "https://frontier.test/", html,
        sizeof(html) - 1, 480, NULL, NULL, true);
    ScriptFrontierProbe probe = {0};
    bool executed = committed
        && navigation_execute_document_scripts_streaming_from(
            &navigation, &navigation.page.document,
            script_frontier_probe, &probe);
    bool ok = executed
        && strcmp(navigation.page.script_result.summary,
                  "first,second,third") == 0
        && navigation.script_discovered == 3
        && probe.active_calls == 3 && probe.final_calls == 1
        && probe.indices[0] == 0 && probe.indices[1] == 1
        && probe.indices[2] == 2 && probe.final_index == 3;
    if (!ok) {
        fprintf(stderr,
                "frontier snapshot ready=%d committed=%d executed=%d "
                "active=%zu final=%zu/%zu discovered=%zu summary=\"%s\" "
                "error=\"%s\"\n",
                ready, committed, executed, probe.active_calls,
                probe.final_calls, probe.final_index,
                navigation.script_discovered,
                navigation.page.script_result.summary,
                navigation.last_error);
    }
    if (ready) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_script_frontier_skips_destroyed_snapshot_nodes(void)
{
    static const char html[] =
        "<!doctype html><title>Frontier destruction</title><body>"
        "<script>"
        "document.body.innerHTML='<main id=replacement>replacement</main>';"
        "globalThis.pocSummary='current-script-survived'"
        "</script>"
        "<script>globalThis.pocSummary='stale-script-ran'</script>"
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
        navigation.defer_document_script_pipeline = true;
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool committed = ready && navigation_commit_html(
        &navigation, generation, "https://frontier-destroy.test/", html,
        sizeof(html) - 1, 480, NULL, NULL, true);
    ScriptFrontierProbe probe = {0};
    bool executed = committed
        && navigation_execute_document_scripts_streaming_from(
            &navigation, &navigation.page.document,
            script_frontier_probe, &probe);
    bool ok = executed
        && strcmp(navigation.page.script_result.summary,
                  "current-script-survived") == 0
        && navigation.script_discovered == 1
        && navigation.script_parser_blocking == 1
        && probe.active_calls == 1 && probe.final_calls == 1
        && probe.indices[0] == 0 && probe.final_index == 1;
    if (!ok) {
        fprintf(stderr,
                "frontier destruction ready=%d committed=%d executed=%d "
                "active=%zu final=%zu/%zu discovered=%zu summary=\"%s\" "
                "error=\"%s\"\n",
                ready, committed, executed, probe.active_calls,
                probe.final_calls, probe.final_index,
                navigation.script_discovered,
                navigation.page.script_result.summary,
                navigation.last_error);
    }
    if (ready) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_script_frontier_snapshot_capacity_is_explicit(void)
{
    static const char prefix[] =
        "<!doctype html><title>Frontier capacity</title><body>";
    static const char script[] = "<script></script>";
    static const char suffix[] = "</body>";
    enum { SCRIPT_COUNT = SCRIPT_DOM_HANDLE_SLOT_CAPACITY + 1 };
    size_t length = sizeof(prefix) - 1
        + SCRIPT_COUNT * (sizeof(script) - 1) + sizeof(suffix) - 1;
    char *html = malloc(length + 1);
    if (html == NULL) return false;
    size_t at = 0;
    memcpy(html + at, prefix, sizeof(prefix) - 1);
    at += sizeof(prefix) - 1;
    for (size_t i = 0; i < SCRIPT_COUNT; i++) {
        memcpy(html + at, script, sizeof(script) - 1);
        at += sizeof(script) - 1;
    }
    memcpy(html + at, suffix, sizeof(suffix));

    Budget budget;
    budget_init(&budget, 64 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool ready = lexbor_installed
        && navigation_init(&navigation, &budget, 4);
    if (ready) {
        navigation_enable_scripts(&navigation, 4 * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 8, 64 * 1024, 32 * 1024, 1000);
        navigation.defer_document_script_pipeline = true;
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool committed = ready && navigation_commit_html(
        &navigation, generation, "https://frontier-cap.test/", html,
        length, 480, NULL, NULL, true);
    bool rejected = committed
        && !navigation_execute_document_scripts_streaming_from(
            &navigation, &navigation.page.document, NULL, NULL);
    bool ok = rejected
        && strstr(navigation.last_error, "script snapshot") != NULL
        && (strstr(navigation.last_error, "exceeded 2048 elements") != NULL
            || strstr(navigation.last_error,
                      "handle capacity exhausted") != NULL);
    if (ready) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) clean = budget_uninstall_lexbor(&budget) && clean;
    free(html);
    return ok && clean;
}

static bool test_preserved_runtime_retires_replaced_document(void)
{
    static const char old_html[] =
        "<!doctype html><title>Old realm</title><body>"
        "<div id=old><script>"
        "globalThis.rebindHeld=document.getElementById('old');"
        "globalThis.rebindCount=1"
        "</script></div></body>";
    static const char new_html[] =
        "<!doctype html><title>New realm</title><body>"
        "<div id=target>new</div></body>";
    static const char probe[] =
        "document.body.innerHTML='<main id=target><span>inner</span></main>';"
        "const target=document.getElementById('target');"
        "target.textContent='safe';"
        "globalThis.pocSummary=String(++globalThis.rebindCount)+':' +"
        "document.body.textContent+':' +"
        "String(globalThis.rebindHeld.textContent)+':' +"
        "String(globalThis.rebindHeld.isConnected)";

    Budget budget;
    budget_init(&budget, 24 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool ready = lexbor_installed
        && navigation_init(&navigation, &budget, 4);
    if (ready) navigation_enable_scripts(&navigation, 4 * MIB, 1000);
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool committed = ready && navigation_commit_html(
        &navigation, generation, "https://rebind.test/old", old_html,
        sizeof(old_html) - 1, 480, NULL, NULL, true);
    ScriptRuntime *runtime = committed ? navigation.page.runtime : NULL;
    lxb_dom_node_t *old_node = committed ? test_find_id(
        lxb_dom_interface_node(navigation.page.document.html), "old") : NULL;
    long old_handle = old_node == NULL ? 0
        : script_runtime_node_handle(runtime, old_node);
    generation = committed ? navigation_begin(&navigation) : 0;
    bool replaced = committed && old_handle != 0
        && navigation_commit_html_preserve_runtime(
            &navigation, generation, "https://rebind.test/new", new_html,
            sizeof(new_html) - 1, 480, NULL, NULL, false);
    ScriptResult diagnostic = {0};
    bool evaluated = replaced
        && script_runtime_evaluate_diagnostic(
            navigation.page.runtime, probe, "<rebind-retirement-probe>",
            &diagnostic);
    bool ok = evaluated
        && navigation.page.runtime == runtime
        && script_runtime_node_handle_resolve(runtime, old_handle) == NULL
        && strcmp(diagnostic.summary, "2:safe::false") == 0;
    if (!ok) {
        bool runtime_retained = navigation.page.runtime == runtime;
        bool old_live = runtime_retained
            && script_runtime_node_handle_resolve(runtime, old_handle) != NULL;
        fprintf(stderr,
                "rebind retirement ready=%d committed=%d replaced=%d "
                "evaluated=%d same-runtime=%d old-live=%d summary=\"%s\" "
                "error=\"%s\"\n",
                ready, committed, replaced, evaluated,
                runtime_retained, old_live,
                diagnostic.summary, diagnostic.error);
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

static bool test_transient_document_survives_active_rebind_until_detach(void)
{
    static const char old_html[] =
        "<!doctype html><title>Active old</title><body>"
        "<div id=old-live>old</div></body>";
    static const char new_html[] =
        "<!doctype html><title>Active new</title><body>"
        "<div id=live>live</div></body>";
    static const char corpus_html[] =
        "<!doctype html><body><script id=corpus-script>"
        "globalThis.corpusRan=(globalThis.corpusRan||0)+1"
        "</script></body>";

    Budget budget;
    budget_init(&budget, 24 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool ready = lexbor_installed
        && navigation_init(&navigation, &budget, 4);
    if (ready) {
        navigation_enable_scripts(&navigation, 4 * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 8, 64 * 1024, 32 * 1024, 1000);
        navigation.defer_document_script_pipeline = true;
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool committed = ready && navigation_commit_html(
        &navigation, generation, "https://owner.test/old", old_html,
        sizeof(old_html) - 1, 480, NULL, NULL, true);
    ScriptRuntime *runtime = committed ? navigation.page.runtime : NULL;
    lxb_dom_node_t *old_node = committed ? test_find_id(
        lxb_dom_interface_node(navigation.page.document.html), "old-live")
        : NULL;
    long old_handle = old_node == NULL ? 0
        : script_runtime_node_handle(runtime, old_node);

    PocDocument corpus = {0};
    bool corpus_parsed = committed && document_parse(
        &corpus, &budget, corpus_html, sizeof(corpus_html) - 1, 64);
    lxb_dom_node_t *corpus_node = corpus_parsed ? test_find_id(
        lxb_dom_interface_node(corpus.html), "corpus-script") : NULL;
    long corpus_handle = corpus_node == NULL ? 0
        : script_runtime_node_handle(runtime, corpus_node);
    bool corpus_registered = corpus_handle != 0
        && script_runtime_register_script_source_node(
            runtime, corpus_node, 7, 0);
    bool corpus_executed = corpus_registered
        && navigation_execute_document_scripts_streaming_from(
            &navigation, &corpus, NULL, NULL);

    generation = corpus_executed ? navigation_begin(&navigation) : 0;
    bool replaced = corpus_executed
        && navigation_commit_html_preserve_runtime(
            &navigation, generation, "https://owner.test/new", new_html,
            sizeof(new_html) - 1, 480, NULL, NULL, false);
    bool corpus_survived = replaced
        && navigation.page.runtime == runtime
        && script_runtime_node_handle_resolve(runtime, old_handle) == NULL
        && script_runtime_node_handle_resolve(runtime, corpus_handle)
             == corpus_node;

    ScriptRuntime *detach_runtime = navigation.page.runtime;
    if (corpus_parsed && detach_runtime != NULL) {
        script_runtime_detach_document(detach_runtime, &corpus);
        script_runtime_clear_script_source_nodes(detach_runtime);
    }
    bool corpus_retired = corpus_handle != 0 && detach_runtime != NULL
        && script_runtime_node_handle_resolve(
               detach_runtime, corpus_handle) == NULL;
    document_destroy(&corpus);

    static const char probe[] =
        "const live=document.getElementById('live');"
        "live.innerHTML='<b>inner</b>';live.textContent='safe';"
        "globalThis.pocSummary=String(globalThis.corpusRan)+':' +"
        "live.textContent";
    ScriptResult diagnostic = {0};
    bool evaluated = corpus_survived && corpus_retired
        && script_runtime_evaluate_diagnostic(
            detach_runtime, probe, "<transient-retirement-probe>",
            &diagnostic);
    bool ok = evaluated && strcmp(diagnostic.summary, "1:safe") == 0;
    if (!ok) {
        fprintf(stderr,
                "transient retirement ready=%d committed=%d parsed=%d "
                "registered=%d executed=%d replaced=%d survived=%d "
                "retired=%d evaluated=%d summary=\"%s\" error=\"%s\"\n",
                ready, committed, corpus_parsed, corpus_registered,
                corpus_executed, replaced, corpus_survived, corpus_retired,
                evaluated, diagnostic.summary, diagnostic.error);
    }
    if (ready) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_preserved_runtime_dynamic_import_owns_scheduler(void)
{
    static const char old_html[] =
        "<!doctype html><title>Module old</title><body>"
        "<script type=module>"
        "globalThis.loadAfterSwap=()=>import('./late.js');"
        "globalThis.pocSummary='module-swap:root'"
        "</script></body>";
    static const char new_html[] =
        "<!doctype html><title>Module new</title><body>new</body>";
    static const char start_import[] =
        "globalThis.pocSummary='module-swap:pending';"
        "loadAfterSwap().then(module=>{"
        "globalThis.pocSummary='module-swap:'+module.value"
        "}).catch(error=>{globalThis.pocSummary='module-swap:error:'+error})";

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
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool committed = ready && navigation_commit_html(
        &navigation, generation, "https://module-swap.test/page", old_html,
        sizeof(old_html) - 1, 480, NULL, NULL, true);
    ScriptRuntime *runtime = committed ? navigation.page.runtime : NULL;
    FetchScheduler *page_scheduler = committed
        ? navigation.page.resource_scheduler : NULL;
    FetchSchedulerDomain *domain = committed
        ? navigation.page.fetch_domain : NULL;
    FetchSchedulerDomainMetrics before_swap = {0};
    bool scheduler_still_lazy = page_scheduler != NULL
        && fetch_scheduler_domain_metrics(domain, &before_swap)
        && before_swap.active_views == 1;

    generation = scheduler_still_lazy ? navigation_begin(&navigation) : 0;
    bool replaced = scheduler_still_lazy
        && navigation_commit_html_preserve_runtime(
            &navigation, generation, "https://module-swap.test/page#next",
            new_html, sizeof(new_html) - 1, 480, NULL, NULL, false);
    FetchSchedulerDomainMetrics after_swap = {0};
    bool realm_retained = replaced && navigation.page.runtime == runtime
        && navigation.page.fetch_domain == domain
        && fetch_scheduler_domain_metrics(domain, &after_swap)
        && after_swap.active_views == 0;

    bool replay_ready = realm_retained && module_swap_replay_begin();
    ScriptResult diagnostic = {0};
    bool imported = replay_ready
        && script_runtime_evaluate_diagnostic(
            runtime, start_import, "<module-swap-import>", &diagnostic);
    FetchSchedulerDomainMetrics after_import = {0};
    bool realm_scheduler_created = imported
        && fetch_scheduler_domain_metrics(domain, &after_import)
        && after_import.active_views == 1;
    for (size_t i = 0; imported && i < 8
         && strcmp(diagnostic.summary, "module-swap:late-ok") != 0; i++) {
        imported = navigation_advance_runtime(&navigation, 0, 32);
        diagnostic = navigation.page.script_result;
    }
    if (replay_ready) fetch_trace_end();
    bool ok = imported && realm_scheduler_created
        && strcmp(diagnostic.summary, "module-swap:late-ok") == 0;
    if (!ok) {
        fprintf(stderr,
                "module swap ready=%d committed=%d lazy=%d views=%zu "
                "replaced=%d retained=%d views-after=%zu replay=%d "
                "imported=%d realm-views=%zu summary=\"%s\" error=\"%s\" "
                "nav=\"%s\"\n",
                ready, committed, scheduler_still_lazy,
                before_swap.active_views, replaced, realm_retained,
                after_swap.active_views, replay_ready, imported,
                after_import.active_views, diagnostic.summary, diagnostic.error,
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

static bool install_module_loader_then_fail(NavigationSession *navigation)
{
    ExternalScriptMetrics metrics = {0};
    ScriptResult result = {0};
    bool executed = document_scripts_execute(
        &navigation->page.document, navigation->page.runtime,
        navigation->budget, navigation->browser_session,
        navigation->page.resource_base_url,
        "https://module-swap.test/failure",
        navigation->page.referrer_policy, 8, 64 * 1024, 32 * 1024, 1000,
        navigation->page.resource_scheduler, &metrics, &result);
    /* The replay-missing external script fails after the retained module
       loader has been installed. Returning from this helper poisons the
       caller-owned metrics frame under ASan; a later import must use the
       context's copy. */
    /* The root module was evaluated explicitly above and the loader may now
       omit already-evaluated candidates from its executable accounting. The
       regression boundary is the failed pass after installing the retained
       module context; the later import verifies that context's lifetime. */
    return !executed;
}

static bool test_document_script_refresh_failure_stops_following_scripts(void)
{
    static const char html[] =
        "<!doctype html><body><script>"
        "document.body.setAttribute('data-mutated','yes')"
        "</script><script>globalThis.pocSummary='SECOND-RAN'</script></body>";
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
        navigation.defer_document_script_pipeline = true;
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool committed = ready && navigation_commit_html(
        &navigation, generation, "https://refresh-boundary.test/", html,
        sizeof(html) - 1u, 480, NULL, NULL, true);
    ExternalScriptMetrics metrics = {0};
    ScriptResult result = {0};
    script_runtime_test_refuse_next_document_refresh();
    bool executed = committed && document_scripts_execute(
        &navigation.page.document, navigation.page.runtime,
        navigation.budget, navigation.browser_session,
        navigation.page.resource_base_url,
        "https://refresh-boundary.test/", navigation.page.referrer_policy,
        4, 32 * 1024, 16 * 1024, 1000,
        navigation.page.resource_scheduler, &metrics, &result);
    bool ok = committed && !executed
        && script_runtime_document_refresh_failed(navigation.page.runtime)
        && strcmp(result.summary, "SECOND-RAN") != 0;
    if (!ok) {
        fprintf(stderr,
                "script refresh boundary committed=%d executed=%d "
                "sticky=%d summary=\"%s\"\n",
                committed, executed,
                committed && script_runtime_document_refresh_failed(
                    navigation.page.runtime),
                result.summary);
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
        navigation.defer_document_script_pipeline = true;
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool committed = ready && navigation_commit_html(
        &navigation, generation, "https://module-swap.test/failure", html,
        sizeof(html) - 1, 480, NULL, NULL, true);
    lxb_dom_node_t *root = committed ? test_find_id(
        lxb_dom_interface_node(navigation.page.document.html), "root") : NULL;
    ScriptResult module_result = {0};
    bool module_ready = root != NULL
        && script_runtime_evaluate_inline(
            navigation.page.runtime, root, true, &module_result);
    bool replay_ready = module_ready && module_swap_replay_begin();
    bool failed_after_install = replay_ready
        && install_module_loader_then_fail(&navigation);
    ScriptResult diagnostic = {0};
    bool imported = replay_ready
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
                "module failure ready=%d committed=%d module=%d failed=%d "
                "replay=%d imported=%d summary=\"%s\" error=\"%s\"\n",
                ready, committed, module_ready, failed_after_install,
                replay_ready, imported, diagnostic.summary, diagnostic.error);
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
