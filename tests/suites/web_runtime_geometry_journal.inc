/* Geometry script forces part way through a turn, and the mutation
   journal that turn leaves: what the forced builds saw must not outlive
   content removed again, and builds within one turn apply each journal
   record once. Included by test_tilefinch.c and run from the web-runtime
   suite. */

/* Two boxes whose heights depend on which one is the first child. */
#define GEOMETRY_JOURNAL_STYLE \
    "body{margin:0}div.box{height:10px}div.box:first-child{height:50px}"

/* A measurement probe: a box inserted first, a box read, the probe
   removed, all in one turn. */
#define GEOMETRY_JOURNAL_PROBE \
    "{const p=document.createElement('div');p.className='box';" \
    "document.body.insertBefore(p,document.body.firstChild);" \
    "globalThis.during=document.getElementById('a').offsetHeight;" \
    "document.body.removeChild(p)}"

/* Script's reads in a later turn: the tree is as it was before the
   probe, so A is the first child again. */
#define GEOMETRY_JOURNAL_CHECK \
    "{const a=document.getElementById('a'),b=document.getElementById('b');" \
    "if(globalThis.during!==10||a.offsetHeight!==50" \
    "||b.getBoundingClientRect().top!==50)" \
    "throw Error('during='+globalThis.during+' A='+a.offsetHeight" \
    "+' B='+b.getBoundingClientRect().top)}"

static bool geometry_journal_commit(NavigationSession *session,
                                    Budget *budget, const char *page,
                                    size_t length)
{
    if (!navigation_init(session, budget, 2)) return false;
    navigation_enable_scripts(session, 4 * MIB, 1000);
    uint64_t generation = navigation_begin(session);
    return navigation_commit_html(session, generation,
                                  "https://geometry-journal.test/", page,
                                  length, 480, NULL, NULL, true)
        && session->page.runtime != NULL;
}

static bool geometry_journal_check(NavigationSession *session,
                                   const char *label)
{
    bool ok = script_runtime_evaluate_diagnostic(
        session->page.runtime, GEOMETRY_JOURNAL_CHECK, "<geometry-check>",
        &session->page.script_result);
    if (!ok)
        fprintf(stderr, "%s: %s\n", label, session->page.script_result.error);
    return ok;
}

/* A page with a layout reuse cache (one with enough nodes keeps one). */
static size_t geometry_journal_large_page(char *page, size_t size,
                                          const char *style)
{
    size_t used = (size_t) snprintf(
        page, size,
        "<!doctype html><html><head><style>%s</style></head><body>"
        "<div class=box id=a>A</div><div class=box id=b>B</div>"
        "<section id=filler>", style);
    for (int i = 0; i < 400 && used + 16 < size; i++)
        used += (size_t) snprintf(page + used, size - used, "<b>%d</b>",
                                  i % 10);
    used += (size_t) snprintf(page + used, size - used,
                              "</section></body></html>");
    return used;
}

/* The probe read A's height while the probe was the first child. The
   journal left only transient content, so the turn kept its published
   layout; script's later reads used to answer from the forced build of
   the probe's tree (A=10, B=60), and a page with a reuse cache kept A's
   probe-time style for its next relayout. */
static int test_geometry_after_transient_probe(void)
{
    puts("test: geometry read during a removed probe does not outlive it");
    Budget budget;
    budget_init(&budget, 24 * MIB);
    budget_install_lexbor(&budget);

    static const char small[] =
        "<!doctype html><html><head><style>" GEOMETRY_JOURNAL_STYLE
        "</style></head><body><div class=box id=a>A</div>"
        "<div class=box id=b>B</div></body></html>";
    static NavigationSession session;
    CHECK(geometry_journal_commit(&session, &budget, small,
                                  sizeof(small) - 1u));
    uint64_t committed = hidden_mutation_layout_signature(&session.page.layout);
    CHECK(hidden_mutation_turn(&session, GEOMETRY_JOURNAL_PROBE));
    CHECK(hidden_mutation_layout_signature(&session.page.layout) == committed);
    CHECK(geometry_journal_check(&session, "small page"));
    navigation_destroy(&session);

    /* With a reuse cache: the painted frame stays the tree's, and so does
       the next relayout (an unrelated append at the end). */
    static char page[16 * 1024];
    size_t used = geometry_journal_large_page(page, sizeof(page),
                                              GEOMETRY_JOURNAL_STYLE);
    CHECK(used + 1 < sizeof(page));
    CHECK(geometry_journal_commit(&session, &budget, page, used)
          && session.page.layout_reuse != NULL);
    committed = hidden_mutation_layout_signature(&session.page.layout);
    CHECK(hidden_mutation_turn(&session, GEOMETRY_JOURNAL_PROBE));
    CHECK(hidden_mutation_layout_signature(&session.page.layout) == committed);
    CHECK(geometry_journal_check(&session, "large page"));
    CHECK(hidden_mutation_turn(&session,
              "{const t=document.createElement('p');t.textContent='TAIL';"
              "document.body.appendChild(t)}"));
    uint64_t appended = hidden_mutation_layout_signature(&session.page.layout);
    CHECK(geometry_journal_check(&session, "large page after append"));
    CHECK(relayout_reuse_fresh_signature(&session) == appended);
    navigation_destroy(&session);
    CHECK(budget.current == 0);
    return 0;
}

static size_t geometry_journal_misses(const NavigationSession *session)
{
    LayoutReuseStats stats = {0};
    layout_reuse_cache_stats(session->page.layout_reuse, &stats);
    return stats.style_misses;
}

static bool geometry_journal_step(NavigationSession *session,
                                  const char *script)
{
    bool ok = script_runtime_evaluate_diagnostic(
        session->page.runtime, script, "<geometry-step>",
        &session->page.script_result);
    if (!ok) fprintf(stderr, "step: %s\n", session->page.script_result.error);
    return ok;
}

/* Several geometry reads in one turn, each after a write. Every forced
   build used to apply the whole journal again, so a read after a small
   write restyled everything an earlier write had reached; each build now
   applies only the records new or changed since the last one. */
static int test_geometry_reads_apply_records_once(void)
{
    puts("test: geometry reads in one turn apply each journal record once");
    Budget budget;
    budget_init(&budget, 24 * MIB);
    budget_install_lexbor(&budget);
    static char page[16 * 1024];
    size_t used = geometry_journal_large_page(
        page, sizeof(page), GEOMETRY_JOURNAL_STYLE ".on b{color:#c00}");
    CHECK(used + 1 < sizeof(page));
    static NavigationSession session;
    CHECK(geometry_journal_commit(&session, &budget, page, used)
          && session.page.layout_reuse != NULL);

    /* A write reaching all 400 filler elements, then a read. */
    size_t misses = geometry_journal_misses(&session);
    CHECK(geometry_journal_step(&session,
        "{document.getElementById('filler').className='on';"
        "globalThis.h1=document.getElementById('a').offsetHeight}"));
    size_t first = geometry_journal_misses(&session) - misses;
    CHECK(first >= 400);
    /* A write to one box, then a read: the filler stays applied. */
    misses = geometry_journal_misses(&session);
    CHECK(geometry_journal_step(&session,
        "{document.getElementById('b').title='t';"
        "globalThis.h2=document.getElementById('b').offsetHeight}"));
    size_t second = geometry_journal_misses(&session) - misses;
    if (second * 8 >= first)
        fprintf(stderr, "second read restyled %zu (first %zu)\n", second,
                first);
    CHECK(second * 8 < first);
    /* A box inserted first and read, then removed again and read: the
       second read must see A as the first child again. */
    CHECK(geometry_journal_step(&session,
        "{const c=document.createElement('div');c.className='box';"
        "c.id='c';document.body.insertBefore(c,document.body.firstChild);"
        "globalThis.h3=document.getElementById('a').offsetHeight;"
        "c.remove();globalThis.h4=document.getElementById('a').offsetHeight;"
        "if(h1!==50||h2!==10||h3!==10||h4!==50)"
        "throw Error([h1,h2,h3,h4].join())}"));
    CHECK(script_runtime_consume_relayout(session.page.runtime)
          && navigation_relayout(&session));
    CHECK(relayout_reuse_fresh_signature(&session)
          == hidden_mutation_layout_signature(&session.page.layout));
    navigation_destroy(&session);
    CHECK(budget.current == 0);
    return 0;
}

/* A turn that builds a probe with innerHTML (which asks for a
   conservative resource scan), styles cells inside it, reads its width
   and removes it again left the tree as it was: no relayout, and the
   published layout stands. A probe that took an existing element with it
   did change the tree, and relays out. */
static int test_transient_journal_skips_relayout(void)
{
    puts("test: a journal of content gone again within the turn skips relayout");
    Budget budget;
    budget_init(&budget, 24 * MIB);
    budget_install_lexbor(&budget);
    static char page[16 * 1024];
    size_t used = geometry_journal_large_page(page, sizeof(page),
                                              GEOMETRY_JOURNAL_STYLE);
    CHECK(used + 1 < sizeof(page));
    static NavigationSession session;
    CHECK(geometry_journal_commit(&session, &budget, page, used));
    uint64_t committed = hidden_mutation_layout_signature(&session.page.layout);
    size_t relayouts = session.performance.fast_relayouts
        + session.performance.full_relayouts;
    size_t skips = session.performance.transient_relayout_skips;
    CHECK(hidden_mutation_turn(&session,
        "{globalThis.during=10;const p=document.createElement('div');"
        "document.body.insertBefore(p,document.body.firstChild);"
        "p.innerHTML='<div><table><tr><td>x</td><td>y</td></tr></table></div>';"
        "const cells=p.getElementsByTagName('td');"
        "cells[0].style.display='none';cells[1].style.display='none';"
        "globalThis.w=p.offsetWidth;p.firstChild.innerHTML='';"
        "p.innerHTML='<i>z</i>';globalThis.w2=p.offsetHeight;"
        "document.body.removeChild(p)}"));
    CHECK(session.performance.fast_relayouts
          + session.performance.full_relayouts == relayouts);
    CHECK(session.performance.transient_relayout_skips == skips + 1);
    CHECK(hidden_mutation_layout_signature(&session.page.layout) == committed);
    CHECK(relayout_reuse_fresh_signature(&session) == committed);
    CHECK(geometry_journal_check(&session, "after skipped probe"));

    /* The probe took B along when it left. */
    CHECK(hidden_mutation_turn(&session,
        "{const p=document.createElement('div');"
        "document.body.appendChild(p);"
        "p.appendChild(document.getElementById('b'));"
        "globalThis.w=p.offsetWidth;document.body.removeChild(p)}"));
    CHECK(session.performance.transient_relayout_skips == skips + 1);
    CHECK(hidden_mutation_layout_signature(&session.page.layout) != committed);
    CHECK(relayout_reuse_fresh_signature(&session)
          == hidden_mutation_layout_signature(&session.page.layout));
    navigation_destroy(&session);
    CHECK(budget.current == 0);
    return 0;
}

/* A detached subtree's writes and the :has() rules of the sheet. A write
   to a div never attached records nothing. A write made inside a probe,
   whose subtree the probe's own innerHTML then discarded before the probe
   left, used to reach the layout reuse cache as a :has() note from a node
   with no connected ancestors: no anchor could be placed, so every
   :has()-dependent style was dropped. Only that subtree's entries go now,
   and the publication's styles match a fresh build. */
static int test_detached_record_keeps_has_styles(void)
{
    puts("test: writes inside discarded probe content keep :has() styles");
    Budget budget;
    budget_init(&budget, 24 * MIB);
    budget_install_lexbor(&budget);
    static char page[16 * 1024];
    size_t used = geometry_journal_large_page(page, sizeof(page),
        GEOMETRY_JOURNAL_STYLE
        "b:has(.mark){color:#c00}.box:has(.mark)~*{outline:1px solid}");
    CHECK(used + 1 < sizeof(page));
    static NavigationSession session;
    CHECK(geometry_journal_commit(&session, &budget, page, used)
          && session.page.layout_reuse != NULL);

    static ScriptMutationJournal journal;
    CHECK(script_runtime_evaluate_diagnostic(session.page.runtime,
        "{const d=document.createElement('div');"
        "d.innerHTML='<span class=mark>never attached</span>'}",
        "<detached-write>", &session.page.script_result));
    /* consume_mutations reports whether anything was recorded. */
    CHECK(!script_runtime_consume_mutations(session.page.runtime, &journal)
          || journal.count == 0);

    LayoutReuseStats before = {0}, after = {0};
    layout_reuse_cache_stats(session.page.layout_reuse, &before);
    CHECK(hidden_mutation_turn(&session,
        "{const p=document.createElement('div');"
        "p.innerHTML='<div><span>x</span></div>';"
        "document.body.appendChild(p);"
        "p.firstChild.innerHTML='<span class=mark>m</span>';"
        "p.innerHTML='';document.body.removeChild(p);"
        "document.getElementById('b').title='t'}"));
    layout_reuse_cache_stats(session.page.layout_reuse, &after);
    size_t misses = after.style_misses - before.style_misses;
    if (misses >= 40)
        fprintf(stderr, "restyled %zu, has-dropped %zu, fallbacks %zu\n",
                misses, after.has_dropped - before.has_dropped,
                after.has_fallbacks - before.has_fallbacks);
    CHECK(misses < 40);
    CHECK(relayout_reuse_fresh_signature(&session)
          == hidden_mutation_layout_signature(&session.page.layout));

    /* Content that left some other way keeps the ordinary invalidation:
       a probe still in the document whose innerHTML discarded the written
       subtree, then an existing section written into and removed. */
    CHECK(hidden_mutation_turn(&session,
        "{const p=document.createElement('div');"
        "p.innerHTML='<div><span>x</span></div>';"
        "document.body.insertBefore(p,document.getElementById('filler'));"
        "p.firstChild.innerHTML='<span class=mark>m</span>';"
        "p.innerHTML='<i>kept</i>'}"));
    CHECK(relayout_reuse_fresh_signature(&session)
          == hidden_mutation_layout_signature(&session.page.layout));
    CHECK(hidden_mutation_turn(&session,
        "{const f=document.getElementById('filler');"
        "f.firstChild.innerHTML='<span class=mark>m</span>';"
        "document.body.removeChild(f)}"));
    CHECK(relayout_reuse_fresh_signature(&session)
          == hidden_mutation_layout_signature(&session.page.layout));
    navigation_destroy(&session);
    CHECK(budget.current == 0);
    return 0;
}
