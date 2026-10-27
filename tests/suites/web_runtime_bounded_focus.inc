/* Bounded focus relayout (NavigationRelayoutPlan bounded_focus): a focus
   move on a page with a script realm lays out only the screens around the
   reader, and nothing may tell. Every frame equals the complete layout's,
   and script never observes the truncated layout (js_rt_bridge_layout):
   each geometry read completes it first. Included by test_tilefinch.c and
   run from the web-runtime suite. */

enum { BOUNDED_FOCUS_PARAGRAPHS = 150 };

static size_t bounded_focus_page(char *page, size_t capacity)
{
    size_t used = (size_t) snprintf(page, capacity,
        "<!doctype html><meta name=viewport content='width=device-width'>"
        "<style>body{margin:0}input{display:block;height:20px;margin:0}"
        /* A sibling focus rule: every move changes geometry and relayouts. */
        ".hint{display:none;height:17px}input:focus+.hint{display:block}"
        "p{margin:0;height:20px}#far{height:30px}"
        "#box{overflow:auto;height:40px}</style>"
        "<input id=a><div class=hint>A</div>"
        "<input id=b><div class=hint>B</div>"
        "<input id=c><div class=hint>C</div>");
    for (unsigned i = 0; i < BOUNDED_FOCUS_PARAGRAPHS && used < capacity; i++)
        used += (size_t) snprintf(page + used, capacity - used,
                                  "<p>Paragraph %u</p>", i);
    used += (size_t) snprintf(page + used, capacity - used,
        "<div id=far>far</div><div id=fresh style='height:44px'>fresh</div>"
        "<div id=box><div style='height:400px'>x</div>"
        "</div><input id=late>"
        "<script>var seen=[],io=null;</script>");
    return used < capacity ? used : 0;
}

static bool bounded_focus_frame(Budget *budget, NavigationSession *session,
                                uint16_t *pixels)
{
    const NavigationEntry *entry = navigation_current(session);
    TileCache cache = {0};
    bool ok = entry != NULL
        && tile_cache_init(&cache, budget, &session->page.layout, 8)
        && tile_cache_set_frame(&cache, pixels, 480u * 272u)
        && tile_cache_render_frame(&cache, entry->scroll_y, 480, 272, NULL);
    tile_cache_destroy(&cache);
    return ok;
}

static bool bounded_focus_same_frame(Budget *budget,
                                     NavigationSession *bounded,
                                     NavigationSession *reference,
                                     uint16_t *first, uint16_t *second)
{
    return navigation_current(bounded)->scroll_y
               == navigation_current(reference)->scroll_y
        && bounded->page.layout.height == reference->page.layout.height
        && bounded_focus_frame(budget, bounded, first)
        && bounded_focus_frame(budget, reference, second)
        && memcmp(first, second, 480u * 272u * sizeof(*first)) == 0;
}

static bool bounded_focus_probe(NavigationSession *session,
                                const char *source, char *answer,
                                size_t capacity)
{
    if (!script_runtime_evaluate_diagnostic(session->page.runtime, source,
            "<bounded-focus-probe>", &session->page.script_result))
        return false;
    snprintf(answer, capacity, "%s", session->page.script_result.summary);
    return true;
}

/* Runs the observers' pending passes, as the page's next task would. A
   pass still pending when a move dispatches its focus events would read
   geometry after the move's own mutation and build the complete layout,
   which the move then adopts instead of bounding. */
static bool bounded_focus_drain(NavigationSession *const sessions[2])
{
    char ignored[64];
    for (unsigned i = 0; i < 2; i++)
        if (!bounded_focus_probe(sessions[i], "pocSummary='drained'",
                                 ignored, sizeof(ignored))) return false;
    return true;
}

static bool bounded_focus_move(BrowserController *controller,
                               NavigationSession *session, const char *id)
{
    lxb_dom_node_t *root =
        lxb_dom_interface_node(session->page.document.html);
    return controller_focus_node(controller, find_id(root, id));
}

/* The same move on both sessions, after their pending tasks. */
static bool bounded_focus_move_both(BrowserController controllers[2],
                                    NavigationSession *const sessions[2],
                                    const char *id)
{
    if (!bounded_focus_drain(sessions)) return false;
    for (unsigned i = 0; i < 2; i++)
        if (!bounded_focus_move(&controllers[i], sessions[i], id))
            return false;
    return true;
}

static bool bounded_focus_cancel_layout(void *context, const char *phase,
                                        size_t completed_work_units)
{
    (void) context;
    (void) completed_work_units;
    return phase == NULL || strncmp(phase, "layout", 6) != 0;
}

static int test_bounded_focus_relayout(void)
{
    puts("test: bounded focus relayout matches the complete layout "
         "and hides its truncation from script");
    Budget budget;
    budget_init(&budget, 32 * MIB);
    budget_install_lexbor(&budget);
    static char page[16384];
    size_t used = bounded_focus_page(page, sizeof(page));
    CHECK(used != 0);
    /* `bounded` takes bounded focus relayouts; `reference` never does. */
    static NavigationSession bounded, reference;
    NavigationSession *const sessions[2] = {&bounded, &reference};
    BrowserController controllers[2];
    for (unsigned i = 0; i < 2; i++) {
        CHECK(navigation_init(sessions[i], &budget, 2));
        navigation_enable_scripts(sessions[i], 4 * MIB, 1000);
        CHECK(navigation_commit_html(sessions[i], navigation_begin(sessions[i]),
            "https://bounded.test/form", page, used, 480, NULL, NULL, true));
        CHECK(controller_init(&controllers[i], sessions[i]));
    }
    reference.page.bounded_focus_refused = true;
    CHECK(bounded_focus_drain(sessions));
    uint16_t *first = budget_malloc(&budget, 480u * 272u * sizeof(*first));
    uint16_t *second = budget_malloc(&budget, 480u * 272u * sizeof(*second));
    CHECK(first != NULL && second != NULL);

    /* Each read is the first geometry read after its own bounded move, so
       each entry point must complete the layout itself: the truncated one
       has no #far, #box or #late at all. */
    static const char *const probes[] = {
        "var r=document.getElementById('far').getBoundingClientRect();"
            "pocSummary=r.top+','+r.height+','+r.width",
        "pocSummary=String(document.getElementById('far').offsetTop)",
        "pocSummary=document.documentElement.scrollHeight+','"
            "+document.documentElement.clientHeight",
        "pocSummary=String(document.getElementById('box').scrollHeight)",
        "pocSummary=getComputedStyle(document.getElementById('far')).height",
        /* An explicit behavior skips scrollTo's getComputedStyle: the
           clamp against the page's extent is the first read. */
        "scrollTo({top:1000000,behavior:'instant'});"
            "pocSummary=String(scrollY);scrollTo({top:0,behavior:'instant'})",
        "var e=document.elementFromPoint(5,100);pocSummary=e?e.id:'none'",
        /* An observer's first pass runs after this program, as a
           microtask, and is the first to read geometry. */
        "(io=new IntersectionObserver(function(es){es.forEach(function(e){"
            "seen.push(e.target.id+':'+e.isIntersecting+':'"
            "+e.boundingClientRect.top);});},"
            "{rootMargin:'0px 0px 5000px 0px'}))"
            ".observe(document.getElementById('box'));pocSummary='observed'",
    };
    static const char *const focus_order[] = {"a", "b", "c"};
    size_t probe_count = sizeof(probes) / sizeof(probes[0]);
    size_t mismatches = 0;
    for (size_t probe = 0; probe < probe_count; probe++) {
        const char *target = focus_order[probe % 3u];
        CHECK(bounded_focus_move_both(controllers, sessions, target));
        /* The move took the bounded build; every frame after it is the
           complete layout's. */
        CHECK(navigation_bounded_focus_region_end(&bounded) != 0
              && bounded.page.layout.content_limit_y != 0
              && !reference.page.layout_provisional);
        CHECK(bounded_focus_same_frame(&budget, &bounded, &reference,
                                       first, second));
        bounded.page.bounded_focus_script_completions = 0;
        size_t completions =
            bounded.performance.bounded_focus_script_completions;
        char answers[2][256];
        for (unsigned i = 0; i < 2; i++)
            CHECK(bounded_focus_probe(sessions[i], probes[probe],
                                      answers[i], sizeof(answers[i])));
        /* The read completed the layout (the forced-geometry scratch);
           the page's own layout stays bounded until it adopts it. */
        bool completed = bounded.performance.bounded_focus_script_completions
                == completions + 1u && bounded.page.layout_provisional;
        if (strcmp(answers[0], answers[1]) != 0 || !completed) {
            fprintf(stderr, "bounded focus probe %zu: %s != %s%s\n",
                    probe, answers[0], answers[1],
                    completed ? "" : " (layout not completed first)");
            mismatches++;
        }
    }
    CHECK(mismatches == 0);
    char observed[2][256];
    for (unsigned i = 0; i < 2; i++)
        CHECK(bounded_focus_probe(sessions[i],
            "pocSummary=seen.join();io.disconnect()",
            observed[i], sizeof(observed[i])));
    CHECK(strcmp(observed[0], observed[1]) == 0
          && strncmp(observed[0], "box:true:", 9) == 0);
    bounded.page.bounded_focus_script_completions = 0;

    /* The completion adopts what script already built; the final frame is
       the complete layout's. */
    CHECK(navigation_complete_layout_sync(&bounded)
          && !bounded.page.layout_provisional
          && navigation_bounded_focus_region_end(&bounded) == 0
          && bounded.page.layout.count == reference.page.layout.count
          && bounded_focus_same_frame(&budget, &bounded, &reference,
                                      first, second));
    CHECK(bounded_focus_drain(sessions));

    /* Frontend: a focus move between nearby inputs is answered by the
       bounded layout; one that would wrap past the cut is not. */
    CHECK(bounded_focus_move_both(controllers, sessions, "a"));
    int region_end = navigation_bounded_focus_region_end(&bounded);
    CHECK(region_end != 0
          && controller_focus_move_within(&controllers[0], true, true,
                                          CONTROLLER_FOCUS_DOWN, region_end)
          && controller_focus_move_within(&controllers[0], false, false,
                                          CONTROLLER_FOCUS_DOWN, region_end));
    CHECK(bounded_focus_drain(sessions));
    for (unsigned i = 0; i < 2; i++)
        CHECK(controller_focus_next(&controllers[i]));
    CHECK(controller_focused_node(&controllers[0])
              == find_id(lxb_dom_interface_node(
                     bounded.page.document.html), "b")
          && bounded_focus_same_frame(&budget, &bounded, &reference,
                                      first, second));
    CHECK(bounded_focus_move_both(controllers, sessions, "c"));
    region_end = navigation_bounded_focus_region_end(&bounded);
    /* #late lies past the cut: the next control is not in the bounded
       layout at all, so the move must not be answered from it. */
    CHECK(region_end != 0
          && !controller_focus_move_within(&controllers[0], true, true,
                                           CONTROLLER_FOCUS_DOWN, region_end)
          && !controller_focus_move_within(&controllers[0], false, false,
                                           CONTROLLER_FOCUS_DOWN,
                                           region_end));

    /* A second move while the completion runs: input cancels it, the move
       takes another bounded build, and the next completion finishes. */
    TilefinchPlatformServices cancelling = {
        .cooperate = bounded_focus_cancel_layout };
    tilefinch_platform_set_services(&cancelling);
    NavigationLayoutCompletion cancelled = navigation_complete_layout(&bounded);
    tilefinch_platform_set_services(NULL);
    CHECK(cancelled == NAVIGATION_LAYOUT_COMPLETION_CANCELLED
          && bounded.page.layout_provisional);
    CHECK(bounded_focus_move_both(controllers, sessions, "b"));
    CHECK(navigation_bounded_focus_region_end(&bounded) != 0
          && bounded_focus_same_frame(&budget, &bounded, &reference,
                                      first, second));
    CHECK(navigation_complete_layout(&bounded)
              == NAVIGATION_LAYOUT_COMPLETION_ADOPTED
          && bounded_focus_same_frame(&budget, &bounded, &reference,
                                      first, second));
    CHECK(bounded_focus_drain(sessions));

    /* Memory pressure: without room for the complete build that must
       follow, a focus-only journal is planned as the complete build. */
    static ScriptMutationJournal journal;
    memset(&journal, 0, sizeof(journal));
    journal.count = 1;
    journal.records[0].kind = SCRIPT_MUTATION_ATTRIBUTE;
    journal.records[0].node = find_id(
        lxb_dom_interface_node(bounded.page.document.html), "a");
    snprintf(journal.records[0].attribute,
             sizeof(journal.records[0].attribute), "data-tilefinch-focus");
    CHECK(navigation_test_relayout_plan(&bounded, &journal, true)
              .bounded_focus);
    size_t limit = budget.limit;
    budget.limit = budget.current + 16u * 1024u;
    bool planned = navigation_test_relayout_plan(&bounded, &journal, true)
        .bounded_focus;
    budget.limit = limit;
    CHECK(!planned);
    /* Nor does a journal with anything but the focus markers. */
    journal.records[0].kind = SCRIPT_MUTATION_INLINE_STYLE;
    CHECK(!navigation_test_relayout_plan(&bounded, &journal, true)
               .bounded_focus);

    /* A page whose script completes every bounded layout gains nothing:
       after the second time it is relaid out completely again. */
    for (unsigned move = 0; move < 2; move++) {
        CHECK(bounded_focus_move_both(controllers, sessions,
                                      focus_order[move]));
        CHECK(navigation_bounded_focus_region_end(&bounded) != 0);
        char answer[256];
        CHECK(bounded_focus_probe(&bounded,
            "pocSummary=String(document.getElementById('far').offsetTop)",
            answer, sizeof(answer)));
    }
    CHECK(bounded.page.bounded_focus_refused);
    CHECK(bounded_focus_move_both(controllers, sessions, "c"));
    CHECK(!bounded.page.layout_provisional
          && bounded_focus_same_frame(&budget, &bounded, &reference,
                                      first, second));

    /* A complete build script cannot have (memory): the box the region
       proves is answered exactly; anything else is unknown, an empty box
       at the region's end, never at the top where every lazy loader would
       fire; the page root reads the extent the reader scrolls. The refusal
       is remembered (no build per read) and the page stops bounding. */
    bounded.page.bounded_focus_refused = false;
    bounded.page.bounded_focus_script_completions = 0;
    CHECK(bounded_focus_move(&controllers[0], &bounded, "a")
          && navigation_bounded_focus_region_end(&bounded) != 0);
    CHECK(bounded_focus_move(&controllers[1], &reference, "a"));
    tilefinch_test_faults()->refuse_bounded_geometry_builds = true;
    tilefinch_test_faults()->bounded_geometry_build_attempts = 0;
    static const char proven_probe[] =
        "var r=document.getElementById('b').getBoundingClientRect();"
        "pocSummary=r.top+','+r.left+','+r.width+','+r.height";
    char proven_answers[2][256];
    for (unsigned i = 0; i < 2; i++)
        CHECK(bounded_focus_probe(sessions[i], proven_probe,
                                  proven_answers[i],
                                  sizeof(proven_answers[i])));
    char refused[256];
    CHECK(bounded_focus_probe(&bounded,
        "var f=document.getElementById('fresh').getBoundingClientRect();"
        "pocSummary=(f.top>=innerHeight)+','+f.width+','+f.height+','"
        "+(document.documentElement.scrollHeight>innerHeight);"
        "(io=new IntersectionObserver(function(es){es.forEach(function(e){"
        "seen.push('fresh:'+e.isIntersecting);});}))"
        ".observe(document.getElementById('fresh'));seen=[]",
        refused, sizeof(refused)));
    char delivered[256];
    CHECK(bounded_focus_probe(&bounded, "pocSummary='drained'",
                              delivered, sizeof(delivered))
          && bounded_focus_probe(&bounded,
                 "pocSummary=seen.join();io.disconnect()",
                 delivered, sizeof(delivered)));
    unsigned attempts =
        tilefinch_test_faults()->bounded_geometry_build_attempts;
    tilefinch_test_faults()->refuse_bounded_geometry_builds = false;
    if (strcmp(proven_answers[0], proven_answers[1]) != 0
        || strcmp(refused, "true,0,0,true") != 0
        || strcmp(delivered, "fresh:false") != 0 || attempts != 1)
        fprintf(stderr, "bounded focus refused completion: proven %s vs "
                "%s; unproven %s; observer %s; builds tried %u\n",
                proven_answers[0], proven_answers[1], refused, delivered,
                attempts);
    CHECK(strcmp(proven_answers[0], proven_answers[1]) == 0
          && strcmp(refused, "true,0,0,true") == 0
          && strcmp(delivered, "fresh:false") == 0 && attempts == 1
          && bounded.page.layout_provisional
          && bounded.page.bounded_focus_refused);

    /* Navigating away while the completion is pending releases it. */
    CHECK(navigation_commit_html(&bounded, navigation_begin(&bounded),
        "https://bounded.test/next", "<!doctype html><p>next</p>",
        strlen("<!doctype html><p>next</p>"), 480, NULL, NULL, true)
          && !bounded.page.layout_provisional
          && navigation_bounded_focus_region_end(&bounded) == 0
          && !bounded.page.bounded_focus_refused);
    for (unsigned i = 0; i < 2; i++) navigation_destroy(sessions[i]);

    /* Content late in the document but drawn at its top (a header placed
       there by position:absolute, with a link) is not in a bounded layout:
       the region proof fails, the move takes the complete build, and the
       page stops bounding. */
    static char late_page[17000];
    size_t late_used = bounded_focus_page(late_page, sizeof(late_page) - 200);
    CHECK(late_used != 0);
    late_used += (size_t) snprintf(late_page + late_used,
        sizeof(late_page) - late_used,
        "<div style='position:absolute;top:0;right:0;width:80px'>"
        "<a href='#x'>Late link</a></div>");
    for (unsigned i = 0; i < 2; i++) {
        CHECK(navigation_init(sessions[i], &budget, 2));
        navigation_enable_scripts(sessions[i], 4 * MIB, 1000);
        CHECK(navigation_commit_html(sessions[i], navigation_begin(sessions[i]),
            "https://bounded.test/late", late_page, late_used, 480, NULL,
            NULL, true));
        CHECK(controller_init(&controllers[i], sessions[i]));
    }
    reference.page.bounded_focus_refused = true;
    CHECK(bounded_focus_move_both(controllers, sessions, "a"));
    CHECK(bounded.performance.bounded_focus_fallbacks == 1
          && bounded.page.bounded_focus_refused
          && !bounded.page.layout_provisional
          && bounded_focus_same_frame(&budget, &bounded, &reference,
                                      first, second));
    budget_free(&budget, first);
    budget_free(&budget, second);
    for (unsigned i = 0; i < 2; i++) navigation_destroy(sessions[i]);
    CHECK(budget.current == 0);
    return 0;
}

/* Two bounded focus moves in a row: the second proves its region against
   the first's bounded layout, which is the complete layout's own only up
   to its own region. A late positioned link drawn just below that region
   (lost by the first cut, and so missing from both bounded layouts) must
   not end up inside the second region: every link the complete layout
   places in a bounded region is in that bounded layout's focus map. */
static size_t bounded_focus_late_link_page(char *page, size_t capacity,
                                           int late_top)
{
    size_t used = (size_t) snprintf(page, capacity,
        "<!doctype html><meta name=viewport content='width=device-width'>"
        "<style>body{margin:0}input{display:block;height:20px;margin:0}"
        ".hint{display:none;height:17px}input:focus+.hint{display:block}"
        "p{margin:0;height:20px}</style>"
        "<input id=a><div class=hint>A</div>");
    for (unsigned i = 0; i < 150 && used < capacity; i++)
        used += (size_t) snprintf(page + used, capacity - used,
            "%s<p>Paragraph %u</p>",
            i == 8 ? "<input id=d><div class=hint>D</div>" : "", i);
    if (used < capacity)
        used += (size_t) snprintf(page + used, capacity - used,
            "<div style='position:absolute;top:%dpx;right:0;width:80px'>"
            "<a id=late href='#x'>Late link</a></div>"
            "<script>var ready=1;</script>", late_top);
    return used < capacity ? used : 0;
}

static int test_bounded_focus_consecutive_region(void)
{
    puts("test: a bounded focus move after another proves no more than "
         "its bounded incumbent's region, and keeps room for the "
         "complete build");
    Budget budget;
    budget_init(&budget, 32 * MIB);
    budget_install_lexbor(&budget);
    static char page[16384];
    static NavigationSession bounded, reference;
    NavigationSession *const sessions[2] = {&bounded, &reference};
    BrowserController controllers[2];
    /* First learn where a first move's region ends (the late link is
       positioned, so it moves no flow), then draw the link between that
       region's end and its build's limit. */
    int late_top = 0;
    for (unsigned pass = 0; pass < 2; pass++) {
        size_t used = bounded_focus_late_link_page(
            page, sizeof(page), pass == 0 ? 20000 : late_top);
        CHECK(used != 0);
        for (unsigned i = 0; i < 2; i++) {
            CHECK(navigation_init(sessions[i], &budget, 2));
            navigation_enable_scripts(sessions[i], 4 * MIB, 1000);
            CHECK(navigation_commit_html(sessions[i],
                navigation_begin(sessions[i]), "https://bounded.test/late",
                page, used, 480, NULL, NULL, true));
            CHECK(controller_init(&controllers[i], sessions[i]));
        }
        reference.page.bounded_focus_refused = true;
        CHECK(bounded_focus_move_both(controllers, sessions, "a"));
        int region = navigation_bounded_focus_region_end(&bounded);
        int limit = bounded.page.layout.content_limit_y;
        CHECK(region != 0 && limit > region);
        if (pass == 0) {
            late_top = region + (limit - region) / 2;
            for (unsigned i = 0; i < 2; i++) navigation_destroy(sessions[i]);
            continue;
        }
        /* The headroom a next bounded move keeps for the complete build
           it owes is that build's size, not the bounded incumbent's few
           screens: room for twice the latter but not the former is
           planned as the complete build. */
        size_t truncated = bounded.page.layout.count * sizeof(DrawCommand)
            + bounded.page.layout.node_box_count * sizeof(LayoutNodeBox);
        size_t whole = reference.page.layout.count * sizeof(DrawCommand)
            + reference.page.layout.node_box_count * sizeof(LayoutNodeBox);
        CHECK(whole > truncated);
        static ScriptMutationJournal journal;
        memset(&journal, 0, sizeof(journal));
        journal.count = 1;
        journal.records[0].kind = SCRIPT_MUTATION_ATTRIBUTE;
        journal.records[0].node = find_id(
            lxb_dom_interface_node(bounded.page.document.html), "d");
        snprintf(journal.records[0].attribute,
                 sizeof(journal.records[0].attribute),
                 "data-tilefinch-focus");
        CHECK(navigation_test_relayout_plan(&bounded, &journal, true)
                  .bounded_focus);
        size_t budget_limit = budget.limit;
        budget.limit = budget.current + truncated + whole;
        bool planned = navigation_test_relayout_plan(&bounded, &journal, true)
            .bounded_focus;
        budget.limit = budget_limit;
        if (planned)
            fprintf(stderr, "bounded headroom: planned with room for %zu "
                    "beside a %zu-byte complete layout\n",
                    truncated + whole, whole);
        CHECK(!planned);
        /* The second move, with no completion in between, lays out
           further than the first (its target sits lower). */
        CHECK(bounded_focus_move_both(controllers, sessions, "d"));
        region = navigation_bounded_focus_region_end(&bounded);
        CHECK(region != 0 && !reference.page.layout_provisional);
        const LayoutDocument *complete = &reference.page.layout;
        size_t missing = 0;
        for (size_t i = 0; i < complete->link_count; i++) {
            const LinkRegion *link = &complete->links[i];
            bool control = false;
            size_t index = 0;
            if ((int64_t) link->y + link->height > region) continue;
            if (!layout_focus_for_node(&bounded.page.layout, link->node,
                                       &control, &index)) {
                fprintf(stderr, "bounded consecutive: link at %d missing "
                        "inside region %d (late at %d)\n", link->y, region,
                        late_top);
                missing++;
            }
        }
        CHECK(missing == 0);
        for (unsigned i = 0; i < 2; i++) navigation_destroy(sessions[i]);
    }
    CHECK(budget.current == 0);
    return 0;
}

/* Script that reads only what a bounded layout proves, or whose reads the
   engine itself queued, does not complete it (review 10-26 J2): the
   observer pass a commit queues waits for the completion, a box inside
   the proven region is answered from it exactly, and neither counts
   toward the latch that stops bounding. */
static int test_bounded_focus_observer_pass_deferred(void)
{
    puts("test: a bounded focus layout's observer pass waits for its "
         "completion and proven reads do not complete it");
    Budget budget;
    budget_init(&budget, 32 * MIB);
    budget_install_lexbor(&budget);
    static char page[17000];
    size_t used = bounded_focus_page(page, sizeof(page) - 500);
    CHECK(used != 0);
    used += (size_t) snprintf(page + used, sizeof(page) - used,
        "<style>:root{--accent:#c03}</style>"
        "<script>var seen=[];new IntersectionObserver(function(es){"
        "es.forEach(function(e){seen.push(e.target.id+':'"
        "+e.isIntersecting);});}).observe(document.getElementById('far'));"
        "</script>");
    static NavigationSession bounded, reference;
    NavigationSession *const sessions[2] = {&bounded, &reference};
    BrowserController controllers[2];
    for (unsigned i = 0; i < 2; i++) {
        CHECK(navigation_init(sessions[i], &budget, 2));
        navigation_enable_scripts(sessions[i], 4 * MIB, 1000);
        CHECK(navigation_commit_html(sessions[i], navigation_begin(sessions[i]),
            "https://bounded.test/observed", page, used, 480, NULL, NULL,
            true));
        CHECK(controller_init(&controllers[i], sessions[i]));
    }
    reference.page.bounded_focus_refused = true;
    CHECK(bounded_focus_drain(sessions));
    static const char *const order[] = {"a", "b", "c", "b"};
    size_t builds = bounded.page.script_result.geometry_snapshot_builds;
    for (size_t move = 0; move < 4; move++) {
        CHECK(bounded_focus_move_both(controllers, sessions, order[move]));
        /* The page's next task: the commit's observer pass would run. */
        CHECK(bounded_focus_drain(sessions));
        char answers[2][128];
        for (unsigned i = 0; i < 2; i++)
            CHECK(bounded_focus_probe(sessions[i],
                "var r=document.getElementById('b').getBoundingClientRect();"
                "pocSummary=r.top+','+r.height+','+r.width",
                answers[i], sizeof(answers[i])));
        bool deferred = bounded.page.layout_provisional
            && navigation_bounded_focus_region_end(&bounded) != 0
            && bounded.performance.bounded_focus_script_completions == 0
            && bounded.page.script_result.geometry_snapshot_builds == builds;
        if (!deferred || strcmp(answers[0], answers[1]) != 0)
            fprintf(stderr, "bounded observer move %zu: provisional=%d "
                    "completions=%zu builds=%zu->%zu proven %s vs %s\n",
                    move, (int) bounded.page.layout_provisional,
                    bounded.performance.bounded_focus_script_completions,
                    builds, bounded.page.script_result.geometry_snapshot_builds,
                    answers[0], answers[1]);
        CHECK(deferred && strcmp(answers[0], answers[1]) == 0);
    }
    CHECK(!bounded.page.bounded_focus_refused);
    /* Computed style from the cascade alone (review 10-26 J3): a focus
       handler reading colours, display or a custom property leaves the
       layout bounded; a used size completes it. */
    char styles[2][256];
    for (unsigned i = 0; i < 2; i++)
        CHECK(bounded_focus_probe(sessions[i],
            "var s=getComputedStyle(document.getElementById('b'));"
            "pocSummary=s.color+';'+s.display+';'+s.visibility+';'"
            "+s.getPropertyValue('--accent')+';'"
            "+getComputedStyle(document.getElementById('far')).fontSize",
            styles[i], sizeof(styles[i])));
    bool cascade_only = bounded.page.layout_provisional
        && bounded.performance.bounded_focus_script_completions == 0;
    if (!cascade_only || strcmp(styles[0], styles[1]) != 0)
        fprintf(stderr, "bounded style reads: provisional=%d completions=%zu "
                "%s vs %s\n", (int) bounded.page.layout_provisional,
                bounded.performance.bounded_focus_script_completions,
                styles[0], styles[1]);
    CHECK(cascade_only && strcmp(styles[0], styles[1]) == 0);
    char used_height[2][64];
    for (unsigned i = 0; i < 2; i++)
        CHECK(bounded_focus_probe(sessions[i],
            "pocSummary=getComputedStyle(document.getElementById('far'))"
            ".height", used_height[i], sizeof(used_height[i])));
    CHECK(strcmp(used_height[0], used_height[1]) == 0
          && bounded.performance.bounded_focus_script_completions == 1);
    bounded.performance.bounded_focus_script_completions = 0;
    /* The completion publishes the complete layout and queues the pass;
       its entries match the page that never bounded. */
    CHECK(navigation_complete_layout_sync(&bounded)
          && !bounded.page.layout_provisional);
    CHECK(bounded_focus_drain(sessions));
    char seen[2][256];
    for (unsigned i = 0; i < 2; i++)
        CHECK(bounded_focus_probe(sessions[i], "pocSummary=seen.join()",
                                  seen[i], sizeof(seen[i])));
    CHECK(strcmp(seen[0], seen[1]) == 0
          && bounded.performance.bounded_focus_script_completions == 0);
    for (unsigned i = 0; i < 2; i++) navigation_destroy(sessions[i]);
    CHECK(budget.current == 0);
    return 0;
}

/* The image-reveal classifier is internal (script observes none of it):
   over a bounded focus layout it still sees the boxes that layout holds,
   so a class change above a formerly visible missing image is not
   classified as revealing one (review 10-26 J4). */
static int test_bounded_focus_image_reveal_classifier(void)
{
    puts("test: a bounded focus layout's boxes still classify image "
         "reveals");
    Budget budget;
    budget_init(&budget, 32 * MIB);
    budget_install_lexbor(&budget);
    static char page[17000];
    size_t used = (size_t) snprintf(page, sizeof(page),
        "<!doctype html><meta name=viewport content='width=device-width'>"
        "<style>body{margin:0}input{display:block;height:20px;margin:0}"
        ".hint{display:none;height:17px}input:focus+.hint{display:block}"
        "p{margin:0;height:20px}.dim img{display:none}</style>"
        "<input id=a><div class=hint>A</div><input id=b>"
        "<div class=hint>B</div>"
        "<div id=wrap><img id=pic alt=pic width=40 height=20></div>");
    for (unsigned i = 0; i < 150 && used < sizeof(page); i++)
        used += (size_t) snprintf(page + used, sizeof(page) - used,
                                  "<p>Paragraph %u</p>", i);
    used += (size_t) snprintf(page + used, sizeof(page) - used,
                              "<script>var ready=1;</script>");
    CHECK(used < sizeof(page));
    static NavigationSession session;
    BrowserController controller;
    CHECK(navigation_init(&session, &budget, 2));
    navigation_enable_scripts(&session, 4 * MIB, 1000);
    CHECK(navigation_commit_html(&session, navigation_begin(&session),
        "https://bounded.test/reveal", page, used, 480, NULL, NULL, true));
    CHECK(controller_init(&controller, &session));
    CHECK(bounded_focus_move(&controller, &session, "a")
          && bounded_focus_move(&controller, &session, "b")
          && navigation_bounded_focus_region_end(&session) != 0);
    char answer[64];
    CHECK(bounded_focus_probe(&session,
        "document.getElementById('wrap').className='dim';pocSummary='x'",
        answer, sizeof(answer)));
    static ScriptMutationJournal journal;
    CHECK(script_runtime_consume_mutations(session.page.runtime, &journal));
    if (journal.image_resource_refresh_required)
        fprintf(stderr, "bounded reveal: a visible missing image counted "
                "as revealed\n");
    CHECK(!journal.image_resource_refresh_required);
    (void) script_runtime_consume_relayout(session.page.runtime);
    navigation_destroy(&session);
    CHECK(budget.current == 0);
    return 0;
}

/* Late content in a bounded focus layout (review 10-26, bounded reveal):
   a page whose late section (after the cut) holds a panel that may paint
   into the region, and a top input whose focus changes geometry. The move
   must publish what the complete layout publishes there. */
typedef struct {
    const char *name;
    const char *rules;
    const char *panel;
    bool early; /* the section follows the input, before the cut */
    bool bounds; /* the move must still take the bounded build */
} BoundedRevealCase;

static size_t bounded_reveal_page(char *page, size_t capacity,
                                  const BoundedRevealCase *test)
{
    char section[256];
    snprintf(section, sizeof(section),
        "<div class=late><div style='%s;left:200px;width:120px;"
        "height:40px;background:#c00'><a id=panel href='#p'>Panel</a>"
        "</div></div>", test->panel);
    size_t used = (size_t) snprintf(page, capacity,
        "<!doctype html><meta name=viewport content='width=device-width'>"
        "<style>body{margin:0}input{display:block;height:20px;margin:0}"
        ".hint{display:none;height:17px}input:focus+.hint{display:block}"
        "p{margin:0;height:20px}%s</style><div id=wrap>"
        "<input id=a><div class=hint>A</div>%s", test->rules,
        test->early ? section : "");
    for (unsigned i = 0; i < 150 && used < capacity; i++)
        used += (size_t) snprintf(page + used, capacity - used,
                                  "<p>Paragraph %u</p>", i);
    if (used < capacity)
        used += (size_t) snprintf(page + used, capacity - used,
            "%s</div><script>var ready=1;</script>",
            test->early ? "" : section);
    return used < capacity ? used : 0;
}

/* The frames at the top, a screen down and at the region's last screen
   (the complete layout's when the move published it). */
static bool bounded_reveal_same_frames(Budget *budget,
                                       NavigationSession *bounded,
                                       NavigationSession *reference,
                                       int region, uint16_t *first,
                                       uint16_t *second)
{
    if (bounded->page.layout.height != reference->page.layout.height)
        return false;
    int end = region != 0 ? region : reference->page.layout.height;
    const int scrolls[] = {0, 272, end - 272};
    for (size_t i = 0; i < sizeof(scrolls) / sizeof(scrolls[0]); i++) {
        if (scrolls[i] < 0 || scrolls[i] + 272 > end) continue;
        TileCache caches[2] = {{0}};
        bool ok = true;
        for (unsigned side = 0; side < 2; side++) {
            NavigationSession *session = side == 0 ? bounded : reference;
            ok = ok && tile_cache_init(&caches[side], budget,
                                       &session->page.layout, 8)
                && tile_cache_set_frame(&caches[side],
                       side == 0 ? first : second, 480u * 272u)
                && tile_cache_render_frame(&caches[side], scrolls[i], 480,
                                           272, NULL);
        }
        for (unsigned side = 0; side < 2; side++)
            tile_cache_destroy(&caches[side]);
        if (!ok || memcmp(first, second, 480u * 272u * sizeof(*first)) != 0)
            return false;
    }
    return true;
}

/* The two sessions hold separate documents, so boxes and focus entries are
   matched by element name and rectangle. Every box the complete layout
   places wholly inside `region` (and every focus entry starting there) has
   its match in the bounded layout. */
static bool bounded_reveal_same_element(const lxb_dom_node_t *a,
                                        const lxb_dom_node_t *b)
{
    if (a == NULL || b == NULL) return a == b;
    return a->type == b->type && a->local_name == b->local_name;
}

static size_t bounded_reveal_missing(const LayoutDocument *bounded,
                                     const LayoutDocument *complete,
                                     int region)
{
    size_t missing = 0;
    for (size_t i = 0; i < complete->node_box_count; i++) {
        const LayoutNodeBox *box = &complete->node_boxes[i];
        if (box->node == NULL
            || (int64_t) box->y + box->height > region) continue;
        bool found = false;
        for (size_t j = 0; !found && j < bounded->node_box_count; j++) {
            const LayoutNodeBox *kept = &bounded->node_boxes[j];
            found = bounded_reveal_same_element(kept->node, box->node)
                && kept->x == box->x && kept->y == box->y
                && kept->width == box->width && kept->height == box->height;
        }
        if (!found) missing++;
    }
    for (size_t i = 0; i < complete->link_count; i++) {
        const LinkRegion *link = &complete->links[i];
        if (link->y >= region) continue;
        bool found = false;
        for (size_t j = 0; !found && j < bounded->link_count; j++)
            found = bounded_reveal_same_element(bounded->links[j].node,
                                                link->node)
                && bounded->links[j].x == link->x
                && bounded->links[j].y == link->y;
        if (!found) missing++;
    }
    for (size_t i = 0; i < complete->control_count; i++) {
        const ControlRegion *control = &complete->controls[i];
        if (control->y >= region) continue;
        bool found = false;
        for (size_t j = 0; !found && j < bounded->control_count; j++)
            found = bounded_reveal_same_element(bounded->controls[j].node,
                                                control->node)
                && bounded->controls[j].x == control->x
                && bounded->controls[j].y == control->y;
        if (!found) missing++;
    }
    return missing;
}

/* Each case's first focus move, on a page that bounds and on one that
   never does; failures are reported by case. */
static int bounded_reveal_run(const BoundedRevealCase *cases, size_t count)
{
    Budget budget;
    budget_init(&budget, 32 * MIB);
    budget_install_lexbor(&budget);
    static char page[16384];
    static NavigationSession bounded, reference;
    NavigationSession *const sessions[2] = {&bounded, &reference};
    BrowserController controllers[2];
    uint16_t *first = budget_malloc(&budget, 480u * 272u * sizeof(*first));
    uint16_t *second = budget_malloc(&budget, 480u * 272u * sizeof(*second));
    CHECK(first != NULL && second != NULL);
    size_t failures = 0;
    for (size_t at = 0; at < count; at++) {
        const BoundedRevealCase *test = &cases[at];
        size_t used = bounded_reveal_page(page, sizeof(page), test);
        CHECK(used != 0);
        for (unsigned i = 0; i < 2; i++) {
            CHECK(navigation_init(sessions[i], &budget, 2));
            navigation_enable_scripts(sessions[i], 4 * MIB, 1000);
            CHECK(navigation_commit_html(sessions[i],
                navigation_begin(sessions[i]), "https://bounded.test/reveal",
                page, used, 480, NULL, NULL, true));
            CHECK(controller_init(&controllers[i], sessions[i]));
        }
        reference.page.bounded_focus_refused = true;
        CHECK(bounded_focus_move_both(controllers, sessions, "a"));
        CHECK(!reference.page.layout_provisional);
        int region = navigation_bounded_focus_region_end(&bounded);
        /* A published complete build is compared everywhere. */
        size_t missing = bounded_reveal_missing(&bounded.page.layout,
            &reference.page.layout, region != 0 ? region : INT_MAX);
        bool same = bounded_reveal_same_frames(&budget, &bounded,
            &reference, region, first, second);
        if (missing != 0 || !same || (test->bounds && region == 0)) {
            fprintf(stderr, "bounded late content %s: region %d, %zu boxes "
                    "or focus entries missing, frames %s%s\n", test->name,
                    region, missing, same ? "same" : "differ",
                    test->bounds && region == 0 ? ", did not bound" : "");
            failures++;
        }
        for (unsigned i = 0; i < 2; i++) navigation_destroy(sessions[i]);
    }
    budget_free(&budget, first);
    budget_free(&budget, second);
    CHECK(failures == 0);
    CHECK(budget.current == 0);
    return 0;
}

/* Late content that always paints into the region (the incumbent half of
   the proof): a viewport-fixed panel is recorded at its static position,
   far below the region, but paints in every viewport. */
static int test_bounded_focus_late_paint(void)
{
    puts("test: a bounded focus move keeps late content that paints into "
         "its region");
    static const BoundedRevealCase cases[] = {
        {"static-fixed", "", "position:fixed;top:30px", false, false},
        {"static-absolute", "#wrap{position:relative}",
         "position:absolute;top:30px", false, false},
        {"static-transform", "", "transform:translateY(-2950px)",
         false, false},
        {"static-relative", "", "position:relative;top:-2950px",
         false, false},
        {"static-margin", "", "margin-top:-2950px", false, false},
        /* Control: nothing late paints into the region. */
        {"static-sticky", "", "position:sticky;bottom:0", false, true},
    };
    return bounded_reveal_run(cases, sizeof(cases) / sizeof(cases[0]));
}

/* A focus move that reveals a late section: the top input's focus rule
   shows a hidden section placed after the cut, whose out-of-flow content
   paints inside the region. The cut never traverses the section, so
   nothing of it is recorded after the cut, and the incumbent drew nothing
   of it there: neither half of the region proof sees it. */
static int test_bounded_focus_reveal(void)
{
    puts("test: a bounded focus move that reveals a late section publishes "
         "the complete layout's region");
    static const BoundedRevealCase cases[] = {
        {"fixed", ".late{display:none}#a:focus~.late{display:block}",
         "position:fixed;top:30px", false, false},
        {"absolute", "#wrap{position:relative}.late{display:none}"
         "#a:focus~.late{display:block}", "position:absolute;top:30px",
         false, false},
        {"early-absolute", "#wrap{position:relative}.late{display:none}"
         "#a:focus~.late{display:block}", "position:absolute;top:30px",
         true, false},
        {"content-visibility", ".late{content-visibility:hidden}"
         "#a:focus~.late{content-visibility:visible}",
         "position:fixed;top:30px", false, false},
        {"visibility", ".late{visibility:hidden}"
         "#a:focus~.late{visibility:visible}", "position:fixed;top:30px",
         false, false},
        {"focus-within", ".late{display:none}"
         "#wrap:focus-within .late{display:block}",
         "position:fixed;top:30px", false, false},
        {"custom-property", ".late{display:var(--show,none)}"
         "#a:focus~.late{--show:block}", "position:fixed;top:30px",
         false, false},
        {"sticky", ".late{display:none}#a:focus~.late{display:block}",
         "position:sticky;bottom:0", false, false},
        /* Control: the focus rules reach no late section. */
        {"self-only", ".late{display:none}", "position:fixed;top:30px",
         false, true},
    };
    return bounded_reveal_run(cases, sizeof(cases) / sizeof(cases[0]));
}

/* Late content that the change moves up into the region: a focus leaving
   an input hides its hint, the page above the cut shrinks, and a late box
   anchored to the bottom of an ancestor the cut ran through rises with it.
   The incumbent drew it just below the region. */
static size_t bounded_shift_page(char *page, size_t capacity, int bottom)
{
    size_t used = (size_t) snprintf(page, capacity,
        "<!doctype html><meta name=viewport content='width=device-width'>"
        "<style>body{margin:0}input{display:block;height:20px;margin:0}"
        ".hint{display:none;height:17px}input:focus+.hint{display:block}"
        "p{margin:0;height:20px}#wrap{position:relative}</style>"
        "<div id=wrap><input id=a><div class=hint>A</div><input id=b>");
    for (unsigned i = 0; i < 150 && used < capacity; i++)
        used += (size_t) snprintf(page + used, capacity - used,
                                  "<p>Paragraph %u</p>", i);
    if (used < capacity)
        used += (size_t) snprintf(page + used, capacity - used,
            "<div style='position:absolute;left:200px;width:120px;"
            "height:10px;bottom:%dpx;background:#c00'></div></div>"
            "<script>var ready=1;</script>", bottom);
    return used < capacity ? used : 0;
}

static int test_bounded_focus_shift(void)
{
    puts("test: a bounded focus move publishes the complete layout's region "
         "when late content rises into it");
    Budget budget;
    budget_init(&budget, 32 * MIB);
    budget_install_lexbor(&budget);
    static char page[16384];
    static NavigationSession bounded, reference;
    NavigationSession *const sessions[2] = {&bounded, &reference};
    BrowserController controllers[2];
    uint16_t *first = budget_malloc(&budget, 480u * 272u * sizeof(*first));
    uint16_t *second = budget_malloc(&budget, 480u * 272u * sizeof(*second));
    CHECK(first != NULL && second != NULL);
    /* First learn the wrapper's height with #a focused and where the move
       to #b bounds its region, then anchor the box just below it. */
    int bottom = -100000, wrap_height = 0, region = 0;
    bool same = false;
    for (unsigned pass = 0; pass < 2; pass++) {
        size_t used = bounded_shift_page(page, sizeof(page), bottom);
        CHECK(used != 0);
        for (unsigned i = 0; i < 2; i++) {
            CHECK(navigation_init(sessions[i], &budget, 2));
            navigation_enable_scripts(sessions[i], 4 * MIB, 1000);
            CHECK(navigation_commit_html(sessions[i],
                navigation_begin(sessions[i]), "https://bounded.test/shift",
                page, used, 480, NULL, NULL, true));
            CHECK(controller_init(&controllers[i], sessions[i]));
        }
        reference.page.bounded_focus_refused = true;
        CHECK(bounded_focus_move_both(controllers, sessions, "a"));
        CHECK(navigation_complete_layout_sync(&bounded)
              && !bounded.page.layout_provisional);
        const LayoutNodeBox *wrap = layout_box_for_node(
            &reference.page.layout,
            find_id(lxb_dom_interface_node(reference.page.document.html),
                    "wrap"));
        CHECK(wrap != NULL);
        wrap_height = wrap->height;
        CHECK(bounded_focus_move_both(controllers, sessions, "b"));
        region = navigation_bounded_focus_region_end(&bounded);
        if (pass == 0) {
            CHECK(region != 0);
            bottom = wrap_height - region - 15;
            for (unsigned i = 0; i < 2; i++) navigation_destroy(sessions[i]);
            continue;
        }
        same = bounded_reveal_same_frames(&budget, &bounded, &reference,
                                          region, first, second)
            && bounded_reveal_missing(&bounded.page.layout,
                   &reference.page.layout,
                   region != 0 ? region : INT_MAX) == 0;
        if (!same)
            fprintf(stderr, "bounded shift: region %d, box anchored %dpx "
                    "above a %dpx wrapper: frames differ\n", region, bottom,
                    wrap_height);
        for (unsigned i = 0; i < 2; i++) navigation_destroy(sessions[i]);
    }
    budget_free(&budget, first);
    budget_free(&budget, second);
    CHECK(same);
    CHECK(budget.current == 0);
    return 0;
}

/* Late lines whose columns a completed box sets: a float that starts above
   the cut and runs past it, which the focus narrows, or the cut-through
   wrapper's shrink-to-fit width, which the focused input widens. Each late
   row then takes a line fewer, so a late box anchored to the bottom of the
   wrapper rises much further than any completed box did. */
static size_t bounded_float_page(char *page, size_t capacity, int bottom,
                                 bool ancestor)
{
    size_t used = (size_t) snprintf(page, capacity,
        "<!doctype html><meta name=viewport content='width=device-width'>"
        "<style>body{margin:0}input{display:block;height:20px;margin:0}"
        "p{margin:0;height:20px}#wrap{position:relative}"
        ".row i{display:inline-block;width:150px;height:20px}%s</style>"
        "<div id=wrap><input id=a>",
        ancestor
        ? "#wrap{width:min-content}#a{width:400px}#a:focus{width:460px}"
          ".row i{width:140px}"
        : ".side{float:right;width:200px;height:3000px}"
          "#a:focus~.side{width:100px}");
    for (unsigned i = 0; i < 34 && used < capacity; i++)
        used += (size_t) snprintf(page + used, capacity - used,
                                  "<p>Paragraph %u</p>", i);
    if (used < capacity && !ancestor)
        used += (size_t) snprintf(page + used, capacity - used,
                                  "<div class=side></div>");
    for (unsigned i = 0; i < 100 && used < capacity; i++)
        used += (size_t) snprintf(page + used, capacity - used,
            "<div class=row><i></i><i></i>%s</div>",
            ancestor ? "<i></i>" : "");
    if (used < capacity)
        used += (size_t) snprintf(page + used, capacity - used,
            "<div style='position:absolute;left:0;width:120px;"
            "height:10px;bottom:%dpx;background:#c00'></div></div>"
            "<script>var ready=1;</script>", bottom);
    return used < capacity ? used : 0;
}

static int test_bounded_focus_float_columns(void)
{
    puts("test: a bounded focus move publishes the complete layout's region "
         "when a completed float or the cut-through wrapper reshapes the "
         "late lines");
    Budget budget;
    budget_init(&budget, 32 * MIB);
    budget_install_lexbor(&budget);
    static char page[16384];
    static NavigationSession bounded, reference;
    NavigationSession *const sessions[2] = {&bounded, &reference};
    BrowserController controllers[2];
    uint16_t *first = budget_malloc(&budget, 480u * 272u * sizeof(*first));
    uint16_t *second = budget_malloc(&budget, 480u * 272u * sizeof(*second));
    CHECK(first != NULL && second != NULL);
    /* First learn the wrapper's height after the move, then anchor the
       box to rise into the first screen (from far below the region). */
    size_t failures = 0;
    int bottom = 0;
    for (unsigned variant = 0; variant < 4; variant++) {
        bool ancestor = variant >= 2;
        unsigned pass = variant % 2u;
        if (pass == 0) bottom = -100000;
        size_t used = bounded_float_page(page, sizeof(page), bottom,
                                         ancestor);
        CHECK(used != 0);
        for (unsigned i = 0; i < 2; i++) {
            CHECK(navigation_init(sessions[i], &budget, 2));
            navigation_enable_scripts(sessions[i], 4 * MIB, 1000);
            CHECK(navigation_commit_html(sessions[i],
                navigation_begin(sessions[i]), "https://bounded.test/float",
                page, used, 480, NULL, NULL, true));
            CHECK(controller_init(&controllers[i], sessions[i]));
        }
        reference.page.bounded_focus_refused = true;
        int before = reference.page.layout.height;
        CHECK(bounded_focus_move_both(controllers, sessions, "a"));
        int region = navigation_bounded_focus_region_end(&bounded);
        if (pass == 0) {
            const LayoutNodeBox *wrap = layout_box_for_node(
                &reference.page.layout,
                find_id(lxb_dom_interface_node(
                    reference.page.document.html), "wrap"));
            CHECK(wrap != NULL
                  && reference.page.layout.height + 1000 < before);
            bottom = wrap->height - 210;
            for (unsigned i = 0; i < 2; i++) navigation_destroy(sessions[i]);
            continue;
        }
        bool same = bounded_reveal_same_frames(&budget, &bounded, &reference,
                                               region, first, second)
            && bounded_reveal_missing(&bounded.page.layout,
                   &reference.page.layout,
                   region != 0 ? region : INT_MAX) == 0;
        if (!same) {
            fprintf(stderr, "bounded %s: region %d, box anchored %dpx "
                    "above the wrapper's bottom: frames differ\n",
                    ancestor ? "wrapper" : "float", region, bottom);
            failures++;
        }
        for (unsigned i = 0; i < 2; i++) navigation_destroy(sessions[i]);
    }
    budget_free(&budget, first);
    budget_free(&budget, second);
    CHECK(failures == 0);
    CHECK(budget.current == 0);
    return 0;
}
