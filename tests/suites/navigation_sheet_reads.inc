/* Synchronous reads after a sheet change (CSSOM View 6; CSSOM 6.1.2).
   A script that adopts, replaces or edits a sheet, or inserts a <style>,
   and then reads a box in the same turn must see the new rules: the read
   forces style and layout with every sheet the document has at that
   moment. Each case reads twice: right after its change, inside the turn
   that made it, and again after the page has relaid out and then rebuilt
   its whole sheet. The two must agree, and match the expected values. The
   relayout applies every change in place, replaced constructed text too,
   and draws what the whole-sheet rebuild draws, whether or not a read
   installed the change first. */

typedef struct {
    const char *label;
    /* Run once at load; leaves globals for the change. */
    const char *setup;
    /* The sheet change, run in the same turn as the first read. */
    const char *change;
    /* What the read must report: "w|h|offsetW|offsetH|cs-width|color". */
    const char *expected;
    /* Read once before the change too, so a geometry snapshot already
       stands when the change arrives. */
    bool read_before;
    /* No read in the change's turn: the relayout alone applies it. */
    bool no_read;
    /* The relayout rebuilds the whole sheet: a <style> the sheet already
       holds changed, and only a rebuild takes its old rules out. Nothing
       can install that change before then, so a read in its turn still
       answers from the old rules; only the relayout's result is checked. */
    bool rebuilds;
    /* The journal cannot itemize the turn's resources (a node it names was
       reclaimed, as on archive.org): the relayout checks the resource
       fingerprint of the whole page. */
    bool conservative;
} SheetReadCase;

#define SHEET_READ_PROBE \
    "(()=>{const e=globalThis.probe();const r=e.getBoundingClientRect();" \
    "const c=getComputedStyle(e);return [r.width,r.height,e.offsetWidth," \
    "e.offsetHeight,c.width,c.color].join('|')})()"

static bool sheet_read_case(const SheetReadCase *test)
{
    char html[2048];
    int used = snprintf(html, sizeof(html),
        "<!doctype html><style>body{margin:0}"
        "#t{width:100px;height:10px;color:#000000}</style>"
        "<body><div id=t>Light</div><div id=h></div><script>"
        "globalThis.host=document.getElementById('h');"
        "globalThis.root=host.attachShadow({mode:'open'});"
        "root.innerHTML='<style>#s{width:100px;height:10px}</style>"
        "<div id=s>Shadow</div>';"
        "%s</script></body>", test->setup);
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool ready = installed && used > 0 && (size_t) used < sizeof(html)
        && navigation_init(&navigation, &budget, 4);
    if (ready) {
        /* These count whole-sheet rebuilds; a slow relayout must not
           change its kind. */
        navigation.relayout_preview_threshold_us = UINT64_MAX;
        navigation_enable_scripts(&navigation, 4 * MIB, 1000);
        navigation_enable_document_scripts(&navigation, 4, 32 * 1024,
                                           16 * 1024, 1000);
    }
    bool ok = ready && navigation_commit_html(&navigation,
        navigation_begin(&navigation), "https://sheet-read.test/", html,
        (size_t) used, 480, NULL, NULL, true);
    for (size_t i = 0; ok && i < 4; i++)
        ok = navigation_advance_runtime(&navigation, 16, 8);
    char script[1536];
    int script_used = snprintf(script, sizeof(script),
        "%s%s;globalThis.pocSummary=%s",
        test->read_before ? "void " SHEET_READ_PROBE ";" : "",
        test->change, test->no_read ? "''" : SHEET_READ_PROBE);
    ok = ok && script_used > 0 && (size_t) script_used < sizeof(script);
    ScriptResult first = {0};
    ok = ok && script_runtime_evaluate_diagnostic(navigation.page.runtime,
        script, "<sheet-read-change>", &first);
    char in_turn[128] = {0};
    if (ok) snprintf(in_turn, sizeof(in_turn), "%s", first.summary);
    if (ok && test->conservative) {
        ScriptMutationJournal *journal = (ScriptMutationJournal *)
            script_runtime_pending_journal(navigation.page.runtime);
        ok = journal != NULL;
        if (ok) journal->conservative_resource_scan = true;
    }
    /* Publish the change in place (no whole-sheet rebuild), then rebuild
       the whole sheet from the sources: both draw the same page. */
    size_t full = navigation.performance.full_relayouts;
    ok = ok && script_runtime_consume_relayout(navigation.page.runtime)
        && navigation_relayout(&navigation)
        && (test->rebuilds || navigation.performance.full_relayouts == full);
    full = navigation.performance.full_relayouts;
    uint64_t appended = append_order_layout_digest(&navigation.page.layout);
    if (ok) {
        navigation.style_rebuild_required = true;
        ok = navigation_relayout(&navigation)
            && navigation.performance.full_relayouts == full + 1
            && append_order_layout_digest(&navigation.page.layout)
                   == appended;
    }
    ScriptResult second = {0};
    ok = ok && script_runtime_evaluate_diagnostic(navigation.page.runtime,
        "globalThis.pocSummary=" SHEET_READ_PROBE, "<sheet-read-fresh>",
        &second);
    bool same = ok && (test->no_read || test->rebuilds
                       || strcmp(in_turn, second.summary) == 0)
        && strcmp(second.summary, test->expected) == 0;
    if (!same)
        fprintf(stderr, "sheet read %s: in-turn=%s fresh=%s expected=%s "
                "error=%s/%s nav=%s\n", test->label, in_turn,
                ok ? second.summary : "-", test->expected, first.error,
                second.error, navigation.last_error);
    if (ready) navigation_destroy(&navigation);
    bool clean = budget.current == 0;
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return same && clean;
}

static bool test_sheet_changes_reach_synchronous_reads(void)
{
    static const char light[] =
        "globalThis.probe=()=>document.getElementById('t');";
    static const char shadow[] =
        "globalThis.probe=()=>root.getElementById('s');";
    static const char wide[] = "200|30|200|30|200px|rgb(0, 128, 0)";
    static const char held[] = "150|30|150|30|150px|rgb(0, 128, 0)";
    char setup[8][256];
    snprintf(setup[0], sizeof(setup[0]), "%s", light);
    snprintf(setup[1], sizeof(setup[1]), "%s"
        "globalThis.a=new CSSStyleSheet();a.replaceSync('#t{width:150px}');"
        "document.adoptedStyleSheets=[a];", light);
    snprintf(setup[2], sizeof(setup[2]), "%s", shadow);
    snprintf(setup[3], sizeof(setup[3]), "%s"
        "globalThis.a=new CSSStyleSheet();a.replaceSync('#s{width:150px}');"
        "root.adoptedStyleSheets=[a];", shadow);
    snprintf(setup[4], sizeof(setup[4]), "%s"
        "globalThis.a=new CSSStyleSheet();"
        "a.replaceSync('#s{width:150px;height:30px;color:#008000}');"
        "root.adoptedStyleSheets=[a];", shadow);
    const SheetReadCase cases[] = {
        {"document adopts a sheet", setup[0],
         "const b=new CSSStyleSheet();b.replaceSync("
         "'#t{width:200px;height:30px;color:#008000}');"
         "document.adoptedStyleSheets=[b]", wide, false},
        {"document replaces its adopted list", setup[1],
         "const b=new CSSStyleSheet();b.replaceSync("
         "'#t{width:200px;height:30px;color:#008000}');"
         "document.adoptedStyleSheets=[b]", wide, false},
        {"document's adopted sheet is replaced in place", setup[1],
         "a.replaceSync('#t{width:200px;height:30px;color:#008000}')",
         wide, false},
        {"document gains a <style>", setup[0],
         "const s=document.createElement('style');"
         "s.textContent='#t{width:200px;height:30px;color:#008000}';"
         "document.body.appendChild(s)", wide, false},
        {"shadow root adopts a sheet", setup[2],
         "const b=new CSSStyleSheet();b.replaceSync("
         "'#s{width:200px;height:30px;color:#008000}');"
         "root.adoptedStyleSheets=[b]", wide, false},
        {"shadow root replaces its adopted list", setup[3],
         "const b=new CSSStyleSheet();b.replaceSync("
         "'#s{width:200px;height:30px;color:#008000}');"
         "root.adoptedStyleSheets=[b]", wide, false},
        {"shadow root's adopted sheet is replaced in place", setup[3],
         "a.replaceSync('#s{width:200px;height:30px;color:#008000}')",
         wide, false},
        {"shadow root gains a <style>", setup[2],
         "const s=document.createElement('style');"
         "s.textContent='#s{width:200px;height:30px;color:#008000}';"
         "root.appendChild(s)", wide, false},
        /* A second change after a read installed the first: the relayout
           appends only what the read did not install. */
        {"document adopts again after a read", setup[0],
         "const b=new CSSStyleSheet();b.replaceSync('#t{width:150px}');"
         "document.adoptedStyleSheets=[b];void probe().offsetWidth;"
         "const c=new CSSStyleSheet();c.replaceSync("
         "'#t{width:200px;height:30px;color:#008000}');"
         "document.adoptedStyleSheets=[b,c]", wide, false},
        {"document gains a second <style> after a read", setup[0],
         "const s=document.createElement('style');"
         "s.textContent='#t{width:150px}';document.body.appendChild(s);"
         "void probe().offsetWidth;const u=document.createElement('style');"
         "u.textContent='#t{width:200px;height:30px;color:#008000}';"
         "document.body.appendChild(u)", wide, false},
        {"a <style> a read installed is edited", setup[0],
         "const s=document.createElement('style');"
         "s.textContent='#t{width:150px}';document.body.appendChild(s);"
         "void probe().offsetWidth;"
         "s.textContent='#t{width:200px;height:30px;color:#008000}'",
         wide, false, false, true},
        /* Nothing new to parse: the held sheet's rules now reach one more
           tree (the scope table changes). */
        {"a new shadow root adopts a held sheet", setup[4],
         "const h=document.createElement('div');document.body.appendChild(h);"
         "const r=h.attachShadow({mode:'open'});"
         "r.innerHTML='<div id=s>Second</div>';r.adoptedStyleSheets=[a];"
         "globalThis.probe=()=>r.getElementById('s')", held, false},
        /* After the read the turn adds a <link> that holds no rules, and
           the journal cannot itemize the turn's resources (archive.org's
           search does both): the relayout keeps the sheet the read
           installed instead of rebuilding it. */
        {"a <link> follows a read that installed a <style>", setup[0],
         "const s=document.createElement('style');"
         "s.textContent='#t{width:200px;height:30px;color:#008000}';"
         "document.body.appendChild(s);void probe().offsetWidth;"
         "const l=document.createElement('link');l.rel='icon';"
         "l.href='data:,';document.head.appendChild(l)",
         wide, false, false, false, true},
        {"shadow root gains a second <style> after a read", setup[2],
         "const s=document.createElement('style');"
         "s.textContent='#s{width:150px}';root.appendChild(s);"
         "void probe().offsetWidth;const u=document.createElement('style');"
         "u.textContent='#s{width:200px;height:30px;color:#008000}';"
         "root.appendChild(u)", wide, false},
    };
    bool ok = true;
    for (int pass = 0; pass < 3; pass++) {
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            SheetReadCase test = cases[i];
            test.read_before = pass == 1;
            test.no_read = pass == 2;
            if (!sheet_read_case(&test)) {
                fprintf(stderr, "sheet read failed: %s (pass %d: %s)\n",
                        test.label, pass, pass == 0 ? "read after"
                        : pass == 1 ? "read before and after" : "no read");
                ok = false;
            }
        }
    }
    return ok;
}
