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
    for (int i = 0; i < 20; i++)
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
    CHECK(hidden_mutation_turn(&session,
              "const s=document.createElement('span');s.textContent='WIDE';"
              "document.body.appendChild(s);globalThis.w=s.offsetWidth;"
              "s.remove();const j=document.createElement('script');"
              "j.type='application/json';j.textContent='{}';"
              "document.body.appendChild(j)")
          && session.performance.hidden_relayout_skips == skips + 1
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
