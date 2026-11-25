/* Relayouts of a large page keep the styles their changes cannot reach,
   and lay out exactly what a build from no cached styles lays out.
   Included by test_tilefinch.c and run from the web-runtime suite. */

/* The draw list with every command's geometry, color and text. */
static uint64_t relayout_reuse_signature(const LayoutDocument *layout)
{
    return hidden_mutation_layout_signature(layout);
}

/* The current tree laid out again from no cached styles. */
static uint64_t relayout_reuse_fresh_signature(NavigationSession *session)
{
    layout_reuse_cache_reset(session->page.layout_reuse);
    if (!navigation_relayout(session)) return 0;
    return relayout_reuse_signature(&session->page.layout);
}

/* How the miss bounds below were chosen. Style entries are placed by
   node address in an 8-slot probe window, so the evictions a walk causes
   depend on where the allocator put the nodes, which changes from run to
   run (ASLR). Over 48 runs, 24 of them in parallel, the checked relayouts
   missed at most 2.6% (insertion), 1.7% (append), 4.0% (removal) and
   12.5% (the sizing test's first relayout) of their lookups; the earlier
   2%/5%/10% bounds failed about one run in eight. A table that did not
   grow, or a cache reset, misses well over half, so these bounds still
   separate reuse from no reuse by several times. */
static int test_relayout_style_reuse(void)
{
    puts("test: relayouts of a large page keep unreachable styles");
    Budget budget;
    budget_init(&budget, 24 * MIB);
    budget_install_lexbor(&budget);
    /* More structural keys than StyleStructureKeys holds (every child
       list then restyles its parent's subtree), more :has() occurrences
       than the old 512-entry plan, a test counting from the end, and
       well over 1024 styled elements. */
    static char page[256 * 1024];
    size_t used = (size_t) snprintf(
        page, sizeof(page),
        "<!doctype html><html><head><style>"
        ".row{padding:1px}.row:last-child .mark{color:#c00}"
        ".mark{color:#00c}.esc\\:wide{width:1px}");
    for (int i = 0; i < 140; i++)
        used += (size_t) snprintf(page + used, sizeof(page) - used,
                                  ".k%d:first-child .x{margin-left:%dpx}",
                                  i, i % 3);
    for (int i = 0; i < 600; i++)
        used += (size_t) snprintf(page + used, sizeof(page) - used,
                                  ".h%d:has(.never%d){color:#0a0}", i, i);
    used += (size_t) snprintf(page + used, sizeof(page) - used,
                              "</style></head><body><div id=list>"
                              "<div class=row><b class=mark>ALPHA</b></div>"
                              "</div><div id=grid>");
    for (int i = 0; i < 400 && used + 64 < sizeof(page); i++)
        used += (size_t) snprintf(page + used, sizeof(page) - used,
                                  "<div class=\"k%d h%d\"><i class=x>%d</i>"
                                  "</div>", i % 20, i % 600, i % 10);
    used += (size_t) snprintf(page + used, sizeof(page) - used,
                              "</div></body></html>");
    CHECK(used + 1 < sizeof(page));
    static NavigationSession session;
    CHECK(navigation_init(&session, &budget, 2));
    navigation_enable_scripts(&session, 4 * MIB, 1000);
    uint64_t generation = navigation_begin(&session);
    CHECK(navigation_commit_html(&session, generation,
                                 "https://relayout-reuse.test/", page, used,
                                 480, NULL, NULL, true)
          && session.page.runtime != NULL
          && session.page.layout_reuse != NULL);

    /* Script grows the document well past the initial 1024 styles. The
       table grows to hold them: the second relayout after the insertion
       resolves almost nothing again. */
    CHECK(hidden_mutation_turn(&session,
              "(()=>{const g=document.createElement('div');let h='';"
              "for(let i=0;i<900;i++)h+='<b class=k'+(i%20)+'><i class=x>'"
              "+(i%10)+'</i></b>';g.innerHTML=h;"
              "document.body.appendChild(g)})()")
          && navigation_relayout(&session));
    size_t misses = session.performance.layout_reuse_style_misses;
    size_t hits = session.performance.layout_reuse_style_hits;
    CHECK(navigation_relayout(&session)
          && hits + 2000 < session.performance.layout_reuse_style_hits
          && (session.performance.layout_reuse_style_misses - misses) * 10
             < session.performance.layout_reuse_style_hits - hits);
    uint64_t steady = relayout_reuse_signature(&session.page.layout);

    /* An element appended to <body>: no sibling test sees it from the
       elements before it, so only its parent restyles, and the 600-rule
       :has() plan scopes the tree change instead of resetting. */
    size_t resets = session.performance.layout_reuse_full_resets;
    size_t fallbacks = session.performance.layout_reuse_has_fallbacks;
    misses = session.performance.layout_reuse_style_misses;
    hits = session.performance.layout_reuse_style_hits;
    CHECK(hidden_mutation_turn(&session,
              "const p=document.createElement('p');p.textContent='TAIL';"
              "document.body.appendChild(p)")
          && command_x(&session.page.layout, "TAIL") >= 0);
    CHECK(session.performance.layout_reuse_full_resets == resets
          && session.performance.layout_reuse_has_fallbacks == fallbacks
          && (session.performance.layout_reuse_style_misses - misses) * 10
             < session.performance.layout_reuse_style_hits - hits);
    uint64_t appended = relayout_reuse_signature(&session.page.layout);
    CHECK(appended != steady
          && relayout_reuse_fresh_signature(&session) == appended);

    /* And removed again in a later turn (an ad-block probe's shape): the
       elements before it keep their styles. */
    misses = session.performance.layout_reuse_style_misses;
    hits = session.performance.layout_reuse_style_hits;
    CHECK(hidden_mutation_turn(&session,
              "document.body.lastElementChild.remove()")
          && command_x(&session.page.layout, "TAIL") < 0);
    CHECK(session.performance.layout_reuse_full_resets == resets
          && (session.performance.layout_reuse_style_misses - misses) * 8
             < session.performance.layout_reuse_style_hits - hits
          && relayout_reuse_signature(&session.page.layout) == steady
          && relayout_reuse_fresh_signature(&session) == steady);
    CHECK(hidden_mutation_turn(&session,
              "const q=document.createElement('p');q.textContent='TAIL';"
              "document.body.appendChild(q)")
          && relayout_reuse_signature(&session.page.layout) == appended);

    /* A measurement probe: a span appended and removed in one turn, with
       a script block appended for good. The tree renders as before. */
    size_t skips = session.performance.hidden_relayout_skips;
    size_t relayouts = session.incremental_relayouts;
    bool measured_probe = hidden_mutation_turn(&session,
              "const s=document.createElement('span');s.textContent='WIDE';"
              "document.body.appendChild(s);globalThis.w=s.offsetWidth;"
              "s.remove();const j=document.createElement('script');"
              "j.type='application/json';j.textContent='{}';"
              "document.body.appendChild(j)");
    if (!measured_probe || session.performance.hidden_relayout_skips != skips + 1
        || session.incremental_relayouts != relayouts
        || relayout_reuse_signature(&session.page.layout) != appended)
        fprintf(stderr, "reuse probe ok=%d skips=%zu/%zu relayouts=%zu/%zu error=%s\n",
                measured_probe, session.performance.hidden_relayout_skips, skips,
                session.incremental_relayouts, relayouts, session.page.script_result.error);
    CHECK(measured_probe && session.performance.hidden_relayout_skips == skips + 1
          && session.incremental_relayouts == relayouts
          && relayout_reuse_signature(&session.page.layout) == appended);
    CHECK(relayout_reuse_fresh_signature(&session) == appended);

    /* A script block appended to <head>. The sheet's escaped selector
       stops the scan that would clear every head script, but the write's
       own :has() probe found no answer it can move. */
    size_t semantic = session.performance.semantic_relayout_skips;
    relayouts = session.incremental_relayouts;
    CHECK(hidden_mutation_turn(&session,
              "const h=document.createElement('script');"
              "h.type='application/json';document.head.appendChild(h)")
          && session.performance.semantic_relayout_skips == semantic + 1
          && session.incremental_relayouts == relayouts);

    /* A second .row: ALPHA's row stops being the last child, so its
       .mark loses the red color. The appended path must not keep it. */
    uint64_t before = relayout_reuse_signature(&session.page.layout);
    CHECK(hidden_mutation_turn(&session,
              "const r=document.createElement('div');r.className='row';"
              "r.innerHTML='<b class=mark>BETA</b>';"
              "document.getElementById('list').appendChild(r)"));
    uint64_t after = relayout_reuse_signature(&session.page.layout);
    CHECK(after != before
          && relayout_reuse_fresh_signature(&session) == after);
    /* BETA's row removed again: ALPHA's .mark is red once more. */
    CHECK(hidden_mutation_turn(&session,
              "document.getElementById('list').lastElementChild.remove()")
          && relayout_reuse_signature(&session.page.layout) == before
          && relayout_reuse_fresh_signature(&session) == before);
    CHECK(hidden_mutation_turn(&session,
              "const r2=document.createElement('div');r2.className='row';"
              "r2.innerHTML='<b class=mark>BETA</b>';"
              "document.getElementById('list').appendChild(r2)")
          && relayout_reuse_signature(&session.page.layout) == after);
    /* Appended in front of an existing sibling: positions move. */
    CHECK(hidden_mutation_turn(&session,
              "const g=document.getElementById('grid');"
              "const d=document.createElement('div');d.className='k3';"
              "d.innerHTML='<i class=x>FIRST</i>';"
              "g.insertBefore(d,g.firstChild)"));
    after = relayout_reuse_signature(&session.page.layout);
    CHECK(relayout_reuse_fresh_signature(&session) == after);

    navigation_destroy(&session);
    CHECK(budget.current == 0);
    return 0;
}

/* The color of the text command reading `wanted`, UINT32_MAX if none. */
static uint32_t relayout_reuse_text_color(const LayoutDocument *layout,
                                          const char *wanted)
{
    size_t length = strlen(wanted);
    for (size_t i = 0; i < layout->count; i++) {
        const DrawCommand *command = &layout->commands[i];
        if (command->type == DRAW_TEXT && command->text_length == length
            && memcmp(command->text, wanted, length) == 0)
            return command->color;
    }
    return UINT32_MAX;
}

/* A child inserted in front of <body>'s first child moves every child's
   position. A tag-keyed sibling test that reaches descendants
   (`div:first-child em`) restyles only the elements its selector names,
   not the whole document, even with more such keys than the sheet once
   kept apart. */
static int test_relayout_structure_subjects(void)
{
    puts("test: a positional test reaching descendants restyles its subjects");
    Budget budget;
    budget_init(&budget, 24 * MIB);
    budget_install_lexbor(&budget);
    static char page[128 * 1024];
    size_t used = (size_t) snprintf(
        page, sizeof(page),
        "<!doctype html><html><head><style>"
        ".cell{padding:1px}em{color:#00c}div:first-child em{color:#c00}");
    for (int i = 0; i < 100; i++)
        used += (size_t) snprintf(page + used, sizeof(page) - used,
                                  ".k%d:first-child .x%d{margin-left:1px}",
                                  i, i);
    used += (size_t) snprintf(page + used, sizeof(page) - used,
                              "</style></head><body>"
                              "<div id=lead><em>LEAD</em></div><div id=grid>");
    for (int i = 0; i < 900 && used + 64 < sizeof(page); i++)
        used += (size_t) snprintf(page + used, sizeof(page) - used,
                                  "<p class=cell><i>%d</i><b>%d</b></p>",
                                  i % 10, i % 7);
    used += (size_t) snprintf(page + used, sizeof(page) - used,
                              "</div></body></html>");
    CHECK(used + 1 < sizeof(page));
    static NavigationSession session;
    CHECK(navigation_init(&session, &budget, 2));
    navigation_enable_scripts(&session, 4 * MIB, 1000);
    uint64_t generation = navigation_begin(&session);
    CHECK(navigation_commit_html(&session, generation,
                                 "https://relayout-subjects.test/", page,
                                 used, 480, NULL, NULL, true)
          && session.page.runtime != NULL
          && session.page.layout_reuse != NULL);
    CHECK(navigation_relayout(&session) && navigation_relayout(&session));
    CHECK(relayout_reuse_text_color(&session.page.layout, "LEAD")
          == 0xcc0000);

    size_t misses = session.performance.layout_reuse_style_misses;
    size_t hits = session.performance.layout_reuse_style_hits;
    CHECK(hidden_mutation_turn(&session,
              "const d=document.createElement('div');"
              "d.innerHTML='<em>FRONT</em>';"
              "document.body.insertBefore(d,document.body.firstChild)"));
    size_t missed = session.performance.layout_reuse_style_misses - misses;
    size_t hit = session.performance.layout_reuse_style_hits - hits;
    if (missed * 20 >= hit)
        fprintf(stderr, "structure subjects missed=%zu hit=%zu\n", missed,
                hit);
    CHECK(missed * 20 < hit);
    CHECK(relayout_reuse_text_color(&session.page.layout, "FRONT")
              == 0xcc0000
          && relayout_reuse_text_color(&session.page.layout, "LEAD")
              == 0x0000cc);
    uint64_t after = relayout_reuse_signature(&session.page.layout);
    CHECK(relayout_reuse_fresh_signature(&session) == after);

    /* Removed again: LEAD is its parent's first child once more. */
    CHECK(hidden_mutation_turn(&session,
              "document.body.firstElementChild.remove()")
          && relayout_reuse_text_color(&session.page.layout, "LEAD")
              == 0xcc0000);
    after = relayout_reuse_signature(&session.page.layout);
    CHECK(relayout_reuse_fresh_signature(&session) == after);

    navigation_destroy(&session);
    CHECK(budget.current == 0);
    return 0;
}

/* A [class] test off its subject reads every class but no id, and an [id]
   test every id but no class: a script that briefly sets an id on <body>
   restyles only the subjects of [id] tests, not the whole document, and
   class and id changes still reach the subjects of the tests they meet. */
static int test_relayout_identity_attribute_kinds(void)
{
    puts("test: [class]/[id] tests off the subject reach only their kind");
    Budget budget;
    budget_init(&budget, 24 * MIB);
    budget_install_lexbor(&budget);
    static char page[128 * 1024];
    size_t used = (size_t) snprintf(
        page, sizeof(page),
        "<!doctype html><html><head><style>"
        ".cell{padding:1px}em{color:#00c}"
        "[class*=wrap] [class*=hot]{color:#c00}[id^=on] .flag{color:#0c0}"
        "</style></head><body>"
        "<div id=lead><em class=flag>LEAD</em></div>"
        "<div id=grid><span class=hotspot>HOT</span>");
    for (int i = 0; i < 900 && used + 64 < sizeof(page); i++)
        used += (size_t) snprintf(page + used, sizeof(page) - used,
                                  "<p class=cell><i>%d</i><b>%d</b></p>",
                                  i % 10, i % 7);
    used += (size_t) snprintf(page + used, sizeof(page) - used,
                              "</div></body></html>");
    CHECK(used + 1 < sizeof(page));
    static NavigationSession session;
    CHECK(navigation_init(&session, &budget, 2));
    navigation_enable_scripts(&session, 4 * MIB, 1000);
    uint64_t generation = navigation_begin(&session);
    CHECK(navigation_commit_html(&session, generation,
                                 "https://relayout-identity.test/", page,
                                 used, 480, NULL, NULL, true)
          && session.page.runtime != NULL
          && session.page.layout_reuse != NULL);
    CHECK(navigation_relayout(&session) && navigation_relayout(&session));
    CHECK(relayout_reuse_text_color(&session.page.layout, "LEAD")
          == 0x0000cc);

    const char *turns[] = {
        "document.body.id='tmp';document.body.removeAttribute('id')",
        "document.body.id='on1'",
    };
    for (size_t t = 0; t < sizeof(turns) / sizeof(turns[0]); t++) {
        size_t misses = session.performance.layout_reuse_style_misses;
        size_t hits = session.performance.layout_reuse_style_hits;
        CHECK(hidden_mutation_turn(&session, turns[t]));
        size_t missed = session.performance.layout_reuse_style_misses
            - misses;
        size_t hit = session.performance.layout_reuse_style_hits - hits;
        /* Over 24 runs the turns missed at most 6.6% (evictions, see the
           bounds note above); with the identity scan unbounded the first
           turn misses three times as many as it hits. */
        if (missed * 8 >= hit)
            fprintf(stderr, "identity kinds turn %zu missed=%zu hit=%zu\n",
                    t, missed, hit);
        CHECK(missed * 8 < hit);
        uint64_t after = relayout_reuse_signature(&session.page.layout);
        CHECK(relayout_reuse_fresh_signature(&session) == after);
    }
    CHECK(relayout_reuse_text_color(&session.page.layout, "LEAD")
          == 0x00cc00);

    CHECK(hidden_mutation_turn(&session,
              "document.getElementById('grid').className='wrapper'")
          && relayout_reuse_text_color(&session.page.layout, "HOT")
              == 0xcc0000);
    uint64_t after = relayout_reuse_signature(&session.page.layout);
    CHECK(relayout_reuse_fresh_signature(&session) == after);

    navigation_destroy(&session);
    CHECK(budget.current == 0);
    return 0;
}

/* Class tests that read the element itself, inside :is()/:not() in the
   subject (`:is([class^=ad-])`, `x :not([class~=off])`), restyle only the
   changed element, as the subject's own compound does; `[class~=name]`
   elsewhere reads `name` as `.name` would. Read as any class, both left a
   rule with no subject key reading every class change, so a class change
   on <html> restyled the whole document. */
static int test_relayout_identity_subject_arguments(void)
{
    puts("test: class tests on the subject inside :is()/:not() stay local");
    Budget budget;
    budget_init(&budget, 24 * MIB);
    budget_install_lexbor(&budget);
    static char page[128 * 1024];
    size_t used = (size_t) snprintf(
        page, sizeof(page),
        "<!doctype html><html><head><style>"
        ".cell{padding:1px}em{color:#00c}"
        ":is([class^=\"ad-x-\"],[class*=\" ad-x-\"]){display:none}"
        ".wrap [class~=price] :not([class~=off]){color:#0a0}"
        ".on .flag{color:#c00}"
        "</style></head><body>"
        "<div class=wrap><span id=p><em>PRICE</em></span></div>"
        "<div id=lead><em class=flag>LEAD</em></div><div id=grid>");
    for (int i = 0; i < 900 && used + 64 < sizeof(page); i++)
        used += (size_t) snprintf(page + used, sizeof(page) - used,
                                  "<p class=cell><i>%d</i><b>%d</b></p>",
                                  i % 10, i % 7);
    used += (size_t) snprintf(page + used, sizeof(page) - used,
                              "</div></body></html>");
    CHECK(used + 1 < sizeof(page));
    static NavigationSession session;
    CHECK(navigation_init(&session, &budget, 2));
    navigation_enable_scripts(&session, 4 * MIB, 1000);
    uint64_t generation = navigation_begin(&session);
    CHECK(navigation_commit_html(&session, generation,
                                 "https://relayout-subject-args.test/", page,
                                 used, 480, NULL, NULL, true)
          && session.page.runtime != NULL
          && session.page.layout_reuse != NULL);
    CHECK(navigation_relayout(&session) && navigation_relayout(&session));
    struct {
        const char *script, *text;
        uint32_t color;
        bool scoped;
    } turns[] = {
        /* <html>: only the rules keyed by .flag reach past it. */
        {"document.documentElement.className='on'", "LEAD", 0xcc0000, true},
        {"document.getElementById('p').className='price'", "PRICE",
         0x00aa00, false},
        {"document.getElementById('p').className='price ad-x-1'", "PRICE",
         UINT32_MAX, false},
        {"document.documentElement.className=''", "LEAD", 0x0000cc, true},
    };
    for (size_t t = 0; t < sizeof(turns) / sizeof(turns[0]); t++) {
        size_t misses = session.performance.layout_reuse_style_misses;
        size_t hits = session.performance.layout_reuse_style_hits;
        CHECK(hidden_mutation_turn(&session, turns[t].script));
        size_t missed = session.performance.layout_reuse_style_misses
            - misses;
        size_t hit = session.performance.layout_reuse_style_hits - hits;
        uint32_t color = relayout_reuse_text_color(&session.page.layout,
                                                   turns[t].text);
        bool scoped = missed * 8 < hit;
        if (color != turns[t].color || (turns[t].scoped && !scoped))
            fprintf(stderr, "subject arguments turn %zu missed=%zu hit=%zu "
                    "color=%06x\n", t, missed, hit, (unsigned) color);
        CHECK(color == turns[t].color && (!turns[t].scoped || scoped));
        uint64_t after = relayout_reuse_signature(&session.page.layout);
        CHECK(relayout_reuse_fresh_signature(&session) == after);
    }
    navigation_destroy(&session);
    CHECK(budget.current == 0);
    return 0;
}

/* A <style> a script adds joins the sheet in place. When each of its
   rules' subjects carries a key (a class, id, tag or attribute) and it
   adds no at-rule besides @media and @supports, only the elements
   carrying those keys restyle; a rule whose subject carries no key, or
   keyframes, still restyle the whole document. Every result is a fresh
   build's. */
static int test_relayout_style_append_keyed(void)
{
    puts("test: an appended style restyles only its subjects' elements");
    Budget budget;
    budget_init(&budget, 24 * MIB);
    budget_install_lexbor(&budget);
    static char page[128 * 1024];
    size_t used = (size_t) snprintf(
        page, sizeof(page),
        "<!doctype html><html><head><style>"
        ".cell{padding:1px}span{color:#00c}"
        "</style></head><body>"
        "<p class=tgt><span>TGT</span></p><div id=box><span>BOX</span></div>"
        "<div id=grid>");
    for (int i = 0; i < 900 && used + 64 < sizeof(page); i++)
        used += (size_t) snprintf(page + used, sizeof(page) - used,
                                  "<p class=cell><i>%d</i><b>%d</b></p>",
                                  i % 10, i % 7);
    used += (size_t) snprintf(page + used, sizeof(page) - used,
                              "</div></body></html>");
    CHECK(used + 1 < sizeof(page));
    static NavigationSession session;
    CHECK(navigation_init(&session, &budget, 2));
    navigation_enable_scripts(&session, 4 * MIB, 1000);
    uint64_t generation = navigation_begin(&session);
    CHECK(navigation_commit_html(&session, generation,
                                 "https://style-append-keyed.test/", page,
                                 used, 480, NULL, NULL, true)
          && session.page.runtime != NULL
          && session.page.layout_reuse != NULL);
    CHECK(navigation_relayout(&session) && navigation_relayout(&session));
    struct {
        const char *css, *text;
        uint32_t color;
        bool scoped;
    } turns[] = {
        {".tgt span{color:#c00}", "TGT", 0xcc0000, true},
        {"@media all{.tgt>span{color:#0c0}}", "TGT", 0x00cc00, true},
        {"#box{display:none}", "BOX", UINT32_MAX, true},
        {"[data-k]{color:#cc0}p.tgt span{color:#0cc}", "TGT", 0x00cccc,
         true},
        /* No key on the subject: anything may match. */
        {"body p.tgt *{color:#00c}", "TGT", 0x0000cc, false},
        /* An element no new selector names may run these. */
        {"@keyframes k{from{color:#c00}}body p.tgt span{color:#c0c}",
         "TGT",
         0xcc00cc, false},
    };
    for (size_t t = 0; t < sizeof(turns) / sizeof(turns[0]); t++) {
        char script[256];
        snprintf(script, sizeof(script),
                 "{const s=document.createElement('style');"
                 "s.textContent='%s';document.body.append(s)}",
                 turns[t].css);
        size_t appends = session.performance.mutation_style_appends;
        size_t misses = session.performance.layout_reuse_style_misses;
        size_t hits = session.performance.layout_reuse_style_hits;
        CHECK(hidden_mutation_turn(&session, script));
        size_t appended = session.performance.mutation_style_appends
            - appends;
        size_t missed = session.performance.layout_reuse_style_misses
            - misses;
        size_t hit = session.performance.layout_reuse_style_hits - hits;
        uint32_t color = relayout_reuse_text_color(&session.page.layout,
                                                   turns[t].text);
        bool scoped = missed * 8 < hit;
        if (appended != 1 || color != turns[t].color
            || scoped != turns[t].scoped)
            fprintf(stderr, "keyed append turn %zu appended=%zu missed=%zu "
                    "hit=%zu color=%06x\n", t, appended, missed, hit,
                    (unsigned) color);
        CHECK(appended == 1 && color == turns[t].color
              && scoped == turns[t].scoped);
        uint64_t after = relayout_reuse_signature(&session.page.layout);
        CHECK(relayout_reuse_fresh_signature(&session) == after);
    }
    navigation_destroy(&session);
    CHECK(budget.current == 0);
    return 0;
}

/* A <style> a script adds that changes nothing a style reads (no rules:
   empty, comments in several text nodes, an at-rule the sheet ignores)
   keeps every computed style. One that adds no rule but a custom property
   or keyframes still restyles the page. Every result is a fresh build's. */
static int test_relayout_style_append_unchanged(void)
{
    puts("test: an appended style that changes nothing keeps every style");
    Budget budget;
    budget_init(&budget, 24 * MIB);
    budget_install_lexbor(&budget);
    static char page[128 * 1024];
    size_t used = (size_t) snprintf(
        page, sizeof(page),
        "<!doctype html><html><head><style>"
        ":root{--c:#00c}.cell{padding:1px}.tgt span{color:var(--c)}"
        "</style></head><body><p class=tgt><span>TGT</span></p>"
        "<div id=grid>");
    for (int i = 0; i < 900 && used + 64 < sizeof(page); i++)
        used += (size_t) snprintf(page + used, sizeof(page) - used,
                                  "<p class=cell><i>%d</i><b>%d</b></p>",
                                  i % 10, i % 7);
    used += (size_t) snprintf(page + used, sizeof(page) - used,
                              "</div></body></html>");
    CHECK(used + 1 < sizeof(page));
    static NavigationSession session;
    CHECK(navigation_init(&session, &budget, 2));
    navigation_enable_scripts(&session, 4 * MIB, 1000);
    uint64_t generation = navigation_begin(&session);
    CHECK(navigation_commit_html(&session, generation,
                                 "https://style-append-unchanged.test/", page,
                                 used, 480, NULL, NULL, true)
          && session.page.runtime != NULL
          && session.page.layout_reuse != NULL);
    CHECK(navigation_relayout(&session) && navigation_relayout(&session));
    struct {
        const char *script;
        uint32_t color;
        bool scoped;
    } turns[] = {
        {"document.body.append(document.createElement('style'))", 0x0000cc,
         true},
        {"{const s=document.createElement('style');"
         "s.append('/* a */','/* b */');document.body.append(s)}",
         0x0000cc, true},
        {"{const s=document.createElement('style');"
         "s.textContent='@page{margin:0}';document.body.append(s)}",
         0x0000cc, true},
        {"{const s=document.createElement('style');"
         "s.textContent=':root{--c:#c00}';document.body.append(s)}",
         0xcc0000, false},
        {"{const s=document.createElement('style');"
         "s.textContent='@keyframes k{from{color:#0c0}}';"
         "document.body.append(s)}", 0xcc0000, false},
    };
    for (size_t t = 0; t < sizeof(turns) / sizeof(turns[0]); t++) {
        size_t appends = session.performance.mutation_style_appends;
        size_t misses = session.performance.layout_reuse_style_misses;
        size_t hits = session.performance.layout_reuse_style_hits;
        CHECK(hidden_mutation_turn(&session, turns[t].script));
        size_t appended = session.performance.mutation_style_appends
            - appends;
        size_t missed = session.performance.layout_reuse_style_misses
            - misses;
        size_t hit = session.performance.layout_reuse_style_hits - hits;
        uint32_t color = relayout_reuse_text_color(&session.page.layout,
                                                   "TGT");
        bool scoped = missed * 8 < hit;
        if (appended != 1 || color != turns[t].color
            || scoped != turns[t].scoped)
            fprintf(stderr, "unchanged append turn %zu appended=%zu "
                    "missed=%zu hit=%zu color=%06x\n", t, appended, missed,
                    hit, (unsigned) color);
        CHECK(appended == 1 && color == turns[t].color
              && scoped == turns[t].scoped);
        uint64_t after = relayout_reuse_signature(&session.page.layout);
        CHECK(relayout_reuse_fresh_signature(&session) == after);
    }
    navigation_destroy(&session);
    CHECK(budget.current == 0);
    return 0;
}

/* A page with more style sources than the sheet records. A <style> a
   script adds after every other source, or in front of a source the sheet
   recorded, joins the sheet in place; one it adds in front of any other
   source, and new text in a source the sheet already parsed, still
   rebuild the sheet, so the cascade is a fresh load's. */
static int test_relayout_tail_style_past_source_bound(void)
{
    puts("test: a style added after every source of a large sheet appends");
    Budget budget;
    budget_init(&budget, 24 * MIB);
    budget_install_lexbor(&budget);
    static char page[32 * 1024];
    size_t used = (size_t) snprintf(page, sizeof(page),
                                    "<!doctype html><html><head>"
                                    "<style>.w{color:#c00}</style>");
    for (unsigned i = 0; i < STYLE_SOURCE_NODE_LIMIT + 8u; i++)
        used += (size_t) snprintf(page + used, sizeof(page) - used,
                                  "<style>.s%u{margin-left:1px}</style>", i);
    used += (size_t) snprintf(page + used, sizeof(page) - used,
        "</head><body><p class=t>TAIL</p><p class=m>MID</p><p class=w>WIN</p>"
        "<style>.m{color:#00c}</style></body></html>");
    CHECK(used + 1 < sizeof(page));
    static NavigationSession session;
    CHECK(navigation_init(&session, &budget, 2));
    navigation_enable_scripts(&session, 4 * MIB, 1000);
    uint64_t generation = navigation_begin(&session);
    CHECK(navigation_commit_html(&session, generation,
                                 "https://tail-style.test/", page, used, 480,
                                 NULL, NULL, true)
          && session.page.runtime != NULL);
    CHECK(navigation_relayout(&session)
          && session.page.stylesheet.style_sources_bounded_out);
    struct {
        const char *script, *text;
        uint32_t color;
        bool appended;
    } turns[] = {
        {"{const s=document.createElement('style');"
         "s.textContent='.t{color:#c00}';document.body.append(s)}",
         "TAIL", 0xcc0000, true},
        {"{const s=document.createElement('style');"
         "s.textContent='.m{color:#0c0}';document.head.append(s)}",
         "MID", 0x0000cc, false},
        {"document.querySelectorAll('head style')[100].textContent="
         "'.t{color:#00c}'", "TAIL", 0xcc0000, false},
        {"{const s=document.createElement('style');"
         "s.textContent='.t{color:#0c0}';document.body.append(s)}",
         "TAIL", 0x00cc00, true},
        /* The last source, now in the sheet, loses its rule. */
        {"{const all=document.querySelectorAll('style');"
         "all[all.length-1].textContent='.u{color:#00c}'}",
         "TAIL", 0xcc0000, false},
        /* In front of a recorded head source: it beats the first head rule
           and loses to the later body rule. */
        {"{const s=document.createElement('style');s.id='n5';"
         "s.textContent='.w{color:#0c0}.m{color:#0c0}';"
         "document.querySelectorAll('head style')[51].before(s)}",
         "WIN", 0x00cc00, true},
        /* After that new (unrecorded) source, before the recorded one. */
        {"{const s=document.createElement('style');s.id='n6';"
         "s.textContent='.w{color:#00c}';"
         "document.getElementById('n5').after(s)}",
         "WIN", 0x0000cc, true},
        /* In front of the new source: its place is not recorded. */
        {"{const s=document.createElement('style');"
         "s.textContent='.w{color:#cc0}';"
         "document.getElementById('n5').before(s)}",
         "WIN", 0x0000cc, false},
    };
    for (size_t t = 0; t < sizeof(turns) / sizeof(turns[0]); t++) {
        size_t appends = session.performance.mutation_style_appends;
        CHECK(hidden_mutation_turn(&session, turns[t].script));
        size_t appended = session.performance.mutation_style_appends
            - appends;
        uint32_t color = relayout_reuse_text_color(&session.page.layout,
                                                   turns[t].text);
        /* The later body rule keeps MID blue throughout. */
        CHECK(relayout_reuse_text_color(&session.page.layout, "MID")
              == 0x0000cc);
        if (appended != (turns[t].appended ? 1u : 0u)
            || color != turns[t].color)
            fprintf(stderr, "tail style turn %zu appended=%zu color=%06x\n",
                    t, appended, (unsigned) color);
        CHECK(appended == (turns[t].appended ? 1u : 0u)
              && color == turns[t].color);
        uint64_t after = relayout_reuse_signature(&session.page.layout);
        CHECK(relayout_reuse_fresh_signature(&session) == after);
    }

    navigation_destroy(&session);
    CHECK(budget.current == 0);
    return 0;
}

/* A committed page larger than the initial table: its first layout is
   sized for the document's elements before it walks them, so the first
   relayout after the commit finds them all. */
static int test_relayout_style_table_sizing(void)
{
    puts("test: a large page's first relayout reuses the commit's styles");
    Budget budget;
    budget_init(&budget, 24 * MIB);
    budget_install_lexbor(&budget);
    static char page[96 * 1024];
    size_t used = (size_t) snprintf(
        page, sizeof(page),
        "<!doctype html><html><head><style>.c{padding:1px}.c i{color:#00c}"
        "</style></head><body>");
    for (int i = 0; i < 1400 && used + 32 < sizeof(page); i++)
        used += (size_t) snprintf(page + used, sizeof(page) - used,
                                  "<p class=c><i>%d</i></p>", i % 10);
    used += (size_t) snprintf(page + used, sizeof(page) - used,
                              "</body></html>");
    CHECK(used + 1 < sizeof(page));
    static NavigationSession session;
    CHECK(navigation_init(&session, &budget, 2));
    navigation_enable_scripts(&session, 4 * MIB, 1000);
    uint64_t generation = navigation_begin(&session);
    CHECK(navigation_commit_html(&session, generation,
                                 "https://relayout-sizing.test/", page, used,
                                 480, NULL, NULL, true)
          && session.page.layout_reuse != NULL);
    size_t misses = session.performance.layout_reuse_style_misses;
    size_t hits = session.performance.layout_reuse_style_hits;
    CHECK(navigation_relayout(&session)
          && hits + 2000 < session.performance.layout_reuse_style_hits
          && (session.performance.layout_reuse_style_misses - misses) * 3
             < session.performance.layout_reuse_style_hits - hits);
    navigation_destroy(&session);
    CHECK(budget.current == 0);
    return 0;
}

/* One discovered image: the element, which source, and what kind. */
typedef struct {
    const lxb_dom_node_t *node;
    uint64_t source_hash;
    bool is_background;
    bool is_mask;
    int pseudo;
} DiscoveredImage;

static size_t discovered_images(const ImageResources *images,
                                DiscoveredImage *out, size_t capacity)
{
    size_t count = 0;
    for (size_t i = 0; images != NULL && i < images->count; i++) {
        if (count == capacity) return SIZE_MAX;
        out[count++] = (DiscoveredImage) {
            .node = images->items[i].node,
            .source_hash = images->items[i].source_hash,
            .is_background = images->items[i].is_background,
            .is_mask = images->items[i].is_mask,
            .pseudo = (int) images->items[i].pseudo
        };
    }
    return count;
}

/* A full rebuild's image discovery resolves the document's styles into
   the layout reuse cache; the layout that follows must take them from
   there. The user stylesheet used to be appended between the two, which
   moved the sheet's generation, so layout dropped every style discovery
   had just resolved and resolved the document again (and discovery had
   fetched images under elements the user sheet hides). Rebuilds must
   discover exactly the commit's images, in document order, and resolve
   each element's style about once. */
static int test_full_rebuild_shares_discovery_styles(void)
{
    puts("test: a full rebuild's layout reuses image discovery's styles");
    Budget budget;
    budget_init(&budget, 32 * MIB);
    budget_install_lexbor(&budget);
    static const char dot[] =
        "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAf"
        "FcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==";
    static char page[256 * 1024];
    size_t used = (size_t) snprintf(
        page, sizeof(page),
        "<!doctype html><html><head><style>"
        ".row{padding:1px}.w-\\[3px\\]{width:3px}.md\\:p-2{padding:2px}"
        ".bg-\\[url\\]{background-image:url('%s')}"
        ".ico::before{content:url('%s')}"
        ".none{display:none}"
        "</style></head><body>"
        "<div class=banner><img id=hidden src='%s'></div>"
        "<img id=first src='%s'>", dot, dot, dot, dot);
    for (int i = 0; i < 1200 && used + 160 < sizeof(page); i++) {
        const char *extra = i % 300 == 7 ? " bg-[url]"
            : i % 300 == 150 ? " ico" : i % 97 == 5 ? " none" : "";
        used += (size_t) snprintf(
            page + used, sizeof(page) - used,
            "<div class='row w-[3px] md:p-2%s'><span>%d</span>%s</div>",
            extra, i % 10, i % 400 == 200 ? "<img src='data:,x'>" : "");
    }
    used += (size_t) snprintf(page + used, sizeof(page) - used,
                              "<img id=last src='%s'></body></html>", dot);
    CHECK(used + 1 < sizeof(page));
    static const char user_css[] = ".banner{display:none}";
    static NavigationSession session;
    CHECK(navigation_init(&session, &budget, 2));
    navigation_enable_external_resources(
        &session, 2, 32 * 1024, 16 * 1024,
        64, 256 * 1024, 16 * 1024, 4 * MIB, 1000);
    CHECK(navigation_set_user_css(&session, user_css, sizeof(user_css) - 1));
    uint64_t generation = navigation_begin(&session);
    CHECK(navigation_commit_html(&session, generation,
                                 "https://rebuild-discovery.test/", page,
                                 used, 480, NULL, NULL, true)
          && session.page.layout_reuse != NULL);
    static DiscoveredImage committed[64], rebuilt[64], first_rebuild[64];
    size_t committed_count = discovered_images(&session.page.images,
                                               committed, 64);
    /* The two data <img> and four backgrounds; the banner's image is
       hidden by the user sheet. */
    CHECK(committed_count == 6);
    lxb_dom_node_t *hidden = find_id(
        lxb_dom_interface_node(session.page.document.html), "hidden");
    CHECK(hidden != NULL);
    for (size_t i = 0; i < committed_count; i++)
        CHECK(committed[i].node != hidden);
    for (int round = 0; round < 2; round++) {
        size_t full = session.performance.full_relayouts;
        size_t misses = session.performance.layout_reuse_style_misses;
        LayoutReuseStats cache_before = {0}, cache_after = {0};
        layout_reuse_cache_stats(session.page.layout_reuse, &cache_before);
        CHECK(navigation_set_user_css(&session, user_css,
                                      sizeof(user_css) - 1)
              && navigation_relayout(&session)
              && session.performance.full_relayouts == full + 1);
        size_t rebuilt_count = discovered_images(&session.page.images,
                                                 rebuilt, 64);
        /* The commit stages the markup images it found first; a rebuild
           discovers in document order. Same images either way, and the
           same order on every rebuild. */
        CHECK(rebuilt_count == committed_count);
        for (size_t i = 0; i < committed_count; i++) {
            bool found = false;
            for (size_t k = 0; !found && k < rebuilt_count; k++)
                found = memcmp(&committed[i], &rebuilt[k],
                               sizeof(committed[i])) == 0;
            if (!found || rebuilt[i].node == hidden) {
                printf("rebuild %d image %zu differs\n", round, i);
                CHECK(false);
            }
            if (round == 1)
                CHECK(memcmp(&rebuilt[i], &first_rebuild[i],
                             sizeof(rebuilt[i])) == 0);
        }
        if (round == 0) memcpy(first_rebuild, rebuilt, sizeof(rebuilt));
        /* Discovery resolves every displayed element against the rebuilt
           sheet (about 2,400); the layout after it must resolve almost
           none again (it resolved all 2,400 again before). */
        size_t discovery = session.page.images.stats.node_style_cache_misses;
        size_t layout = session.performance.layout_reuse_style_misses
            - misses;
        layout_reuse_cache_stats(session.page.layout_reuse, &cache_after);
        size_t evictions = cache_after.style_evictions - cache_before.style_evictions;
        /* Node addresses determine probe-window collisions. Account for
           observed evictions, not a looser arbitrary miss percentage. A
           generation reset still loses the whole document without recording
           those losses as evictions, so it fails this check. */
        if (discovery < 2000 || (layout > discovery / 20
            && layout - discovery / 20 > evictions)) {
            printf("rebuild %d: discovery missed %zu styles, layout %zu, evictions %zu\n",
                   round, discovery, layout, evictions);
            CHECK(false);
        }
    }
    navigation_destroy(&session);
    CHECK(budget.current == 0);
    return 0;
}

static bool relayout_reuse_has_text(const LayoutDocument *layout,
                                    const char *text)
{
    size_t length = strlen(text);
    for (size_t i = 0; i < layout->count; i++) {
        const DrawCommand *command = &layout->commands[i];
        if (command->type == DRAW_TEXT && command->text_length == length
            && memcmp(command->text, text, length) == 0) return true;
    }
    return false;
}

/* The user stylesheet ends a built sheet, after the adopted sheets. A
   sheet a shadow root adopts later joins the sheet in place: numbered
   after the user rules, the adopted order range took them in, so they
   applied only inside shadow trees and a user rule hiding an element in
   the document stopped hiding it. */
static int test_relayout_adopted_append_keeps_user_rules(void)
{
    puts("test: a sheet adopted in place leaves user rules on the document");
    Budget budget;
    budget_init(&budget, 24 * MIB);
    budget_install_lexbor(&budget);
    static char page[16 * 1024];
    size_t used = (size_t) snprintf(page, sizeof(page),
        "<!doctype html><html><head><style>.in{color:#00c}</style></head>"
        "<body><p class=hide>GONE</p><p>KEEP</p><div id=a></div>"
        "<div id=b></div><div>");
    /* Pages under 256 nodes keep no layout reuse cache. */
    for (int i = 0; i < 300; i++)
        used += (size_t) snprintf(page + used, sizeof(page) - used,
                                  "<i>%d</i>", i % 10);
    used += (size_t) snprintf(page + used, sizeof(page) - used,
        "</div><script>{const r=document.getElementById('a')"
        ".attachShadow({mode:'open'});r.innerHTML='<span class=in>AIN</span>';"
        "const s=new CSSStyleSheet();s.replaceSync('.in{color:#0c0}');"
        "r.adoptedStyleSheets=[s]}</script></body></html>");
    CHECK(used + 1 < sizeof(page));
    static const char user_css[] = ".hide{display:none!important}";
    static NavigationSession session;
    CHECK(navigation_init(&session, &budget, 2));
    navigation_enable_scripts(&session, 4 * MIB, 1000);
    CHECK(navigation_set_user_css(&session, user_css, sizeof(user_css) - 1));
    uint64_t generation = navigation_begin(&session);
    CHECK(navigation_commit_html(&session, generation,
                                 "https://adopted-user.test/", page, used,
                                 480, NULL, NULL, true)
          && session.page.runtime != NULL);
    CHECK(navigation_relayout(&session));
    /* A rebuild puts the adopted sheet in front of the user rules. */
    CHECK(navigation_set_user_css(&session, user_css, sizeof(user_css) - 1)
          && navigation_relayout(&session));
    CHECK(session.page.stylesheet.scopes.adopted_source_count == 1);
    CHECK(relayout_reuse_has_text(&session.page.layout, "AIN")
          && !relayout_reuse_has_text(&session.page.layout, "GONE"));
    size_t appends = session.performance.mutation_style_appends;
    CHECK(hidden_mutation_turn(&session,
        "{const r=document.getElementById('b').attachShadow({mode:'open'});"
        "r.innerHTML='<span class=in>BIN</span>';"
        "const t=new CSSStyleSheet();t.replaceSync('.in{color:#c0c}');"
        "r.adoptedStyleSheets=[t]}"));
    /* Taken in place, as a second adopted source. */
    CHECK(session.performance.mutation_style_appends == appends + 1u);
    CHECK(session.page.stylesheet.scopes.adopted_source_count == 2);
    uint64_t in_place = relayout_reuse_signature(&session.page.layout);
    /* Laid out again from no cached styles, on the same sheet. */
    CHECK(relayout_reuse_fresh_signature(&session) == in_place);
    bool hidden = !relayout_reuse_has_text(&session.page.layout, "GONE");
    if (!hidden) fprintf(stderr, "adopted append: user rule lost\n");
    CHECK(hidden && relayout_reuse_has_text(&session.page.layout, "KEEP")
          && relayout_reuse_has_text(&session.page.layout, "BIN"));
    /* The same tree from a rebuilt sheet. */
    CHECK(navigation_set_user_css(&session, user_css, sizeof(user_css) - 1)
          && navigation_relayout(&session));
    CHECK(relayout_reuse_signature(&session.page.layout) == in_place);
    navigation_destroy(&session);
    CHECK(budget.current == 0);
    return 0;
}

/* An adopted sheet with no rules still joins the sheet in place, at the
   user rules, and still ends the adopted range there: ended at the tail,
   the range took the user rules in for the empty source's shadow tree.
   The next adoption goes in front of the user rules too, after the empty
   source; numbered past it, the empty source's first order overtook it,
   and taking an adopted sheet out then refused and rebuilt the sheet. */
static int test_relayout_empty_adopted_append_stays_in_order(void)
{
    puts("test: an empty adopted sheet leaves later appends in place");
    Budget budget;
    budget_init(&budget, 24 * MIB);
    budget_install_lexbor(&budget);
    static char page[16 * 1024];
    size_t used = (size_t) snprintf(page, sizeof(page),
        "<!doctype html><html><head><style>.in{color:#00c}</style></head>"
        "<body><p class=hide>GONE</p><p>KEEP</p><div id=a></div>"
        "<div id=b></div><div id=c></div><div>");
    /* Pages under 256 nodes keep no layout reuse cache. */
    for (int i = 0; i < 300; i++)
        used += (size_t) snprintf(page + used, sizeof(page) - used,
                                  "<i>%d</i>", i % 10);
    used += (size_t) snprintf(page + used, sizeof(page) - used,
        "</div><script>{const r=document.getElementById('a')"
        ".attachShadow({mode:'open'});r.innerHTML='<span class=in>AIN</span>';"
        "const s=new CSSStyleSheet();s.replaceSync('.in{color:#0c0}');"
        "r.adoptedStyleSheets=[s]}</script></body></html>");
    CHECK(used + 1 < sizeof(page));
    static const char user_css[] = ".hide{display:none!important}";
    static NavigationSession session;
    CHECK(navigation_init(&session, &budget, 2));
    navigation_enable_scripts(&session, 4 * MIB, 1000);
    CHECK(navigation_set_user_css(&session, user_css, sizeof(user_css) - 1));
    uint64_t generation = navigation_begin(&session);
    CHECK(navigation_commit_html(&session, generation,
                                 "https://adopted-empty.test/", page, used,
                                 480, NULL, NULL, true)
          && session.page.runtime != NULL);
    CHECK(navigation_relayout(&session));
    /* A rebuild puts the adopted sheet in front of the user rules. */
    CHECK(navigation_set_user_css(&session, user_css, sizeof(user_css) - 1)
          && navigation_relayout(&session));
    CHECK(session.page.stylesheet.scopes.adopted_source_count == 1);
    CHECK(!relayout_reuse_has_text(&session.page.layout, "GONE"));
    size_t appends = session.performance.mutation_style_appends;
    size_t fallbacks = session.performance.mutation_style_append_fallbacks;
    CHECK(hidden_mutation_turn(&session,
        "{const r=document.getElementById('b').attachShadow({mode:'open'});"
        "r.innerHTML='<span class=in>BIN</span>';"
        "r.adoptedStyleSheets=[new CSSStyleSheet()]}"));
    CHECK(session.performance.mutation_style_appends == appends + 1u);
    CHECK(session.page.stylesheet.scopes.adopted_source_count == 2);
    /* The append changed no rule, so the relayout kept every style: a
       layout from no cached styles reads the range. */
    uint64_t empty = relayout_reuse_signature(&session.page.layout);
    bool same = relayout_reuse_fresh_signature(&session) == empty;
    bool hidden = !relayout_reuse_has_text(&session.page.layout, "GONE");
    if (!same || !hidden)
        fprintf(stderr, "empty adopted append: user rule lost (fresh "
                "layout %s)\n", same ? "same" : "differs");
    CHECK(same);
    CHECK(hidden && relayout_reuse_has_text(&session.page.layout, "BIN"));
    CHECK(hidden_mutation_turn(&session,
        "{const r=document.getElementById('c').attachShadow({mode:'open'});"
        "r.innerHTML='<span class=in>CIN</span>';"
        "const u=new CSSStyleSheet();u.replaceSync('.in{color:#c0c}');"
        "r.adoptedStyleSheets=[u]}"));
    CHECK(session.performance.mutation_style_appends == appends + 2u);
    CHECK(session.page.stylesheet.scopes.adopted_source_count == 3);
    CHECK(!relayout_reuse_has_text(&session.page.layout, "GONE")
          && relayout_reuse_has_text(&session.page.layout, "CIN"));
    /* The first adopted sheet leaves: taken out in place, not rebuilt. */
    CHECK(hidden_mutation_turn(&session,
        "document.getElementById('a').shadowRoot.adoptedStyleSheets=[]"));
    bool in_place = session.performance.mutation_style_appends
            == appends + 3u
        && session.performance.mutation_style_append_fallbacks == fallbacks;
    if (!in_place)
        fprintf(stderr, "empty adopted append: removal rebuilt (appends=%zu "
                "fallbacks=%zu)\n",
                session.performance.mutation_style_appends - appends,
                session.performance.mutation_style_append_fallbacks
                    - fallbacks);
    CHECK(in_place);
    CHECK(session.page.stylesheet.scopes.adopted_source_count == 2);
    uint64_t kept = relayout_reuse_signature(&session.page.layout);
    CHECK(!relayout_reuse_has_text(&session.page.layout, "GONE")
          && relayout_reuse_has_text(&session.page.layout, "KEEP"));
    /* Laid out again from no cached styles, on the same sheet. */
    CHECK(relayout_reuse_fresh_signature(&session) == kept);
    /* The same tree from a rebuilt sheet. */
    CHECK(navigation_set_user_css(&session, user_css, sizeof(user_css) - 1)
          && navigation_relayout(&session));
    CHECK(relayout_reuse_signature(&session.page.layout) == kept);
    navigation_destroy(&session);
    CHECK(budget.current == 0);
    return 0;
}

/* One turn of `turn` (after `setup`, its own turn) over a page whose
   leaf is red only while `rule`'s :has() argument matches inside #p.
   The relayout must lay out what a fresh build does: blue. 1 when not. */
static int relayout_stale_children_case(const char *label, const char *rule,
                                        const char *children,
                                        const char *setup, const char *turn)
{
    Budget budget;
    budget_init(&budget, 24 * MIB);
    budget_install_lexbor(&budget);
    static char page[16 * 1024];
    size_t used = (size_t) snprintf(page, sizeof(page),
        "<!doctype html><html><head><style>.leaf{color:#00c}"
        "%s{color:#c00}</style></head><body><div class=a id=a>"
        "<div id=p>%s</div><p class=leaf>LEAF</p></div><div id=other>"
        "</div><div>", rule, children);
    /* Pages under 256 nodes keep no layout reuse cache. */
    for (int i = 0; i < 300; i++)
        used += (size_t) snprintf(page + used, sizeof(page) - used,
                                  "<i>%d</i>", i % 10);
    used += (size_t) snprintf(page + used, sizeof(page) - used,
                              "</div></body></html>");
    static NavigationSession session;
    int failed = 1;
    if (used + 1 < sizeof(page) && navigation_init(&session, &budget, 2)) {
        navigation_enable_scripts(&session, 4 * MIB, 1000);
        uint64_t generation = navigation_begin(&session);
        bool ready = navigation_commit_html(
                         &session, generation, "https://stale-children.test/",
                         page, used, 480, NULL, NULL, true)
            && session.page.runtime != NULL
            && session.page.layout_reuse != NULL
            && navigation_relayout(&session)
            && (setup == NULL || hidden_mutation_turn(&session, setup));
        uint32_t before = ready
            ? relayout_reuse_text_color(&session.page.layout, "LEAF")
            : UINT32_MAX;
        bool turned = ready && before == 0xcc0000
            && hidden_mutation_turn(&session, turn);
        uint32_t reused = turned
            ? relayout_reuse_text_color(&session.page.layout, "LEAF")
            : UINT32_MAX;
        uint64_t signature = relayout_reuse_signature(&session.page.layout);
        bool fresh = turned
            && relayout_reuse_fresh_signature(&session) == signature;
        failed = !(turned && reused == 0x0000cc && fresh);
        if (failed)
            fprintf(stderr, "stale children %s: ready=%d before=%06x "
                    "reused=%06x fresh=%d error=%s\n", label, ready,
                    (unsigned) before, (unsigned) reused, fresh,
                    session.page.script_result.error);
        navigation_destroy(&session);
    }
    if (budget.current != 0) failed = 1;
    return failed;
}

/* A write earlier in the turn takes a :has() argument off an old child
   that the turn then removes (innerHTML, removal or a move): the record
   of that write cannot reach the anchor once the child is gone, so the
   tree change's own record must (fails while its probe saw the children
   only as they were by then). */
static int test_relayout_stale_children_reach_has(void)
{
    puts("test: a tree change reaches :has() answers earlier writes moved");
    static const char x_rule[] = ".a:has(.x) .leaf";
    static const char x_child[] = "<span id=c class=x>C</span>";
    static const char replace[] =
        "document.getElementById('p').innerHTML='<b>N</b>'";
    static const struct {
        const char *label, *rule, *children, *setup, *turn;
    } cases[] = {
        {"innerHTML, child freed", x_rule, x_child, NULL,
         "document.getElementById('c').className='';"
         "document.getElementById('p').innerHTML='<b>N</b>'"},
        {"innerHTML, child kept", x_rule, x_child, NULL,
         "globalThis.k=document.getElementById('c');k.className='';"
         "document.getElementById('p').innerHTML='<b>N</b>'"},
        {"innerHTML, attribute removed", ".a:has([data-x]) .leaf",
         "<span id=c data-x>C</span>", NULL,
         "document.getElementById('c').removeAttribute('data-x');"
         "document.getElementById('p').innerHTML='<b>N</b>'"},
        {"innerHTML, descendant removed first", x_rule,
         "<i><span id=c class=x>C</span></i>", NULL,
         "document.getElementById('c').remove();"
         "document.getElementById('p').innerHTML='<b>N</b>'"},
        {"innerHTML, focused child", ".a:has(:focus) .leaf",
         "<input id=c>", "document.getElementById('c').focus()", replace},
        {"innerHTML, focus within", ".a:focus-within .leaf",
         "<input id=c>", "document.getElementById('c').focus()", replace},
        {"removal", x_rule, x_child, NULL,
         "document.getElementById('c').className='';"
         "document.getElementById('p').remove()"},
        {"move", x_rule, x_child, NULL,
         "document.getElementById('c').className='';"
         "document.getElementById('other').appendChild("
         "document.getElementById('p'))"},
    };
    int failed = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        failed |= relayout_stale_children_case(
            cases[i].label, cases[i].rule, cases[i].children,
            cases[i].setup, cases[i].turn);
    CHECK(failed == 0);
    return 0;
}

/* A class write while the sheet holds more partial [class] tests than the
   identity table keeps carries only the empty-name token. A sheet removed
   later in the turn brings the count back under the bound: the table must
   not then register the document's own [class*=x] test under a token the
   pending write never carried (fails while incompleteness lasted only for
   the sheet generation). */
static int test_relayout_identity_bound_survives_removal(void)
{
    puts("test: a pending class write reaches tests a removed sheet unbounded");
    Budget budget;
    budget_init(&budget, 24 * MIB);
    budget_install_lexbor(&budget);
    static char page[32 * 1024];
    size_t used = (size_t) snprintf(page, sizeof(page),
        "<!doctype html><html><head><style>.leaf{color:#00c}"
        "[class*=x] .leaf{color:#c00}</style></head><body>"
        "<div id=a class=a><p class=leaf>LEAF</p></div><div id=h></div><div>");
    for (int i = 0; i < 300; i++)
        used += (size_t) snprintf(page + used, sizeof(page) - used,
                                  "<i>%d</i>", i % 10);
    used += (size_t) snprintf(page + used, sizeof(page) - used,
        "</div><script>{let css='';for(let i=0;i<300;i++)"
        "css+='[class*=k'+i+'] b{margin-left:1px}';"
        "const s=new CSSStyleSheet();s.replaceSync(css);"
        "document.getElementById('h').attachShadow({mode:'open'})"
        ".adoptedStyleSheets=[s]}</script></body></html>");
    CHECK(used + 1 < sizeof(page));
    static NavigationSession session;
    CHECK(navigation_init(&session, &budget, 2));
    navigation_enable_scripts(&session, 4 * MIB, 1000);
    uint64_t generation = navigation_begin(&session);
    CHECK(navigation_commit_html(&session, generation,
                                 "https://identity-bound.test/", page, used,
                                 480, NULL, NULL, true)
          && session.page.runtime != NULL
          && session.page.layout_reuse != NULL);
    CHECK(navigation_relayout(&session) && navigation_relayout(&session));
    CHECK(session.page.stylesheet.scopes.adopted_source_count == 1
          && relayout_reuse_text_color(&session.page.layout, "LEAF")
                 == 0x0000cc);
    CHECK(hidden_mutation_turn(&session,
              "document.getElementById('a').className='ax';"
              "document.getElementById('h').shadowRoot"
              ".adoptedStyleSheets=[]"));
    uint32_t color = relayout_reuse_text_color(&session.page.layout, "LEAF");
    uint64_t reused = relayout_reuse_signature(&session.page.layout);
    bool fresh = relayout_reuse_fresh_signature(&session) == reused;
    if (color != 0xcc0000 || !fresh)
        fprintf(stderr, "identity bound: color=%06x fresh=%d adopted=%u\n",
                (unsigned) color, fresh,
                (unsigned) session.page.stylesheet.scopes.adopted_source_count);
    CHECK(color == 0xcc0000 && fresh);
    navigation_destroy(&session);
    CHECK(budget.current == 0);
    return 0;
}
