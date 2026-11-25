/* getComputedStyle keeps resolved styles across reads
   (src/js_computed_style.c). A change confined to a shadow host's subtree
   (an adoption list, the shadow tree's own content, a carrier becoming
   one) drops only the styles in that subtree; everything else stays.
   These cases check that every answer still equals what a cold cache
   resolves, for each way such a change crosses its scope: :host,
   ::slotted, inheritance and custom properties into light children, slot
   assignment, nested components, @property, keyframes and adoption
   order (::part and :host-context do not match in this cascade; their
   cases pin that).

   Each case warms the cache on every probed element (inside and outside
   the change's scope), makes its change and reads again, in the change's
   turn (the sheet settle installs it) or only after the relayout. Each
   answer is compared with the same probe after the cache was emptied.
   That comparison holds on the whole-clear code too: it is the invariant
   the scoped drops must keep. `scoped` cases also require that the change
   emptied no retained style wholesale, which fails on the whole-clear
   code. */

typedef struct {
    const char *label;
    /* Markup after <body>, including a setup script; it defines
       globalThis.targets() (the probed elements). */
    const char *body;
    /* The change, run in one turn. */
    const char *change;
    /* A substring the probe must show after the change (the change took
       effect at all), or NULL. */
    const char *expect;
    /* The change must not empty the retained styles wholesale. */
    bool scoped;
    /* The cascade does not match what the change touches (:host-context
       and ::part are not implemented): every answer must stay as it was.
       Pinned so that implementing them revisits the scope. */
    bool inert;
} StyleScopeCase;

#define STYLE_SCOPE_PROBE \
    "targets().map(e=>{const c=getComputedStyle(e);return [c.color," \
    "c.display,c.fontSize,c.marginLeft,c.getPropertyValue('--tone')]" \
    ".join(',')}).join(';')+'|'+document.body.innerText.replace(/\\s+/g,' ')"

/* Set by the refusal sweep: the budget refuses one allocation after this
   many minus one succeed, from the change's turn to its read (0: none).
   A probe the refusal broke is expected there, not a failure. */
static size_t style_scope_refuse_after;
static bool style_scope_quiet;
static size_t style_scope_refusals_taken, style_scope_refusals_read;

static bool style_scope_probe(NavigationSession *navigation,
                              const char *prefix, char *out, size_t size)
{
    char script[1024];
    int used = snprintf(script, sizeof(script), "%sglobalThis.pocSummary="
                        STYLE_SCOPE_PROBE, prefix == NULL ? "" : prefix);
    ScriptResult result = {0};
    bool ok = used > 0 && (size_t) used < sizeof(script)
        && script_runtime_evaluate_diagnostic(navigation->page.runtime,
               script, "<style-scope>", &result)
        && result.error[0] == '\0';
    /* A summary that filled the result was cut: comparing two cut
       summaries would hide a difference past the cut. */
    if (ok && strlen(result.summary) + 1u >= sizeof(result.summary)) {
        fprintf(stderr, "style scope probe: summary cut at %zu bytes\n",
                sizeof(result.summary));
        ok = false;
    } else if (!ok && !style_scope_quiet) {
        fprintf(stderr, "style scope probe error: %s\n", result.error);
    }
    snprintf(out, size, "%s", ok ? result.summary : "");
    return ok;
}

/* The probe again with every retained style gone: the reference. */
static bool style_scope_cold(NavigationSession *navigation, char *out,
                             size_t size)
{
    js_rt_bridge_computed_style_cache_free(&navigation->page.runtime->bridge);
    return style_scope_probe(navigation, NULL, out, size);
}

static size_t style_scope_full_clears(NavigationSession *navigation)
{
    return navigation->page.runtime->bridge.computed_style_cache.full_clears;
}

/* pass 0: read in the change's turn; pass 1: read after the relayout. */
static bool style_scope_case(const StyleScopeCase *test, int pass)
{
    char html[4096];
    int used = snprintf(html, sizeof(html),
        "<!doctype html><style>body{margin:0}.out{color:rgb(5,5,5)}"
        "</style><body>%s</body>", test->body);
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool ready = installed && used > 0 && (size_t) used < sizeof(html)
        && navigation_init(&navigation, &budget, 4);
    if (ready) {
        navigation.relayout_preview_threshold_us = UINT64_MAX;
        navigation_enable_scripts(&navigation, 4 * MIB, 1000);
        navigation_enable_document_scripts(&navigation, 4, 32 * 1024,
                                           16 * 1024, 1000);
    }
    bool ok = ready && navigation_commit_html(&navigation,
        navigation_begin(&navigation), "https://style-scope.test/", html,
        (size_t) used, 480, NULL, NULL, true);
    for (size_t i = 0; ok && i < 4; i++)
        ok = navigation_advance_runtime(&navigation, 16, 8);
    /* Settle the setup's own changes, then warm every probed style. */
    if (ok && script_runtime_consume_relayout(navigation.page.runtime))
        ok = navigation_relayout(&navigation);
    char before[2048] = {0}, warm[2048] = {0};
    ok = ok && style_scope_probe(&navigation, NULL, before, sizeof(before))
        && style_scope_probe(&navigation, NULL, warm, sizeof(warm))
        && strcmp(before, warm) == 0;
    /* Full clears outside the reference probes (which start cold). */
    size_t unscoped = 0, mark = ok ? style_scope_full_clears(&navigation) : 0;
    char kept[2048] = {0}, cold[2048] = {0};
    char prefix[1024];
    used = snprintf(prefix, sizeof(prefix), "%s;", test->change);
    ok = ok && used > 0 && (size_t) used < sizeof(prefix);
    bool refused = false;
    if (pass == 0) {
        /* The reference at the same moment, before the relayout: a read
           the change's turn could not install answers from the cascade as
           it stands, retained or not. */
        size_t injected = budget.injected_failure_count;
        if (ok && style_scope_refuse_after != 0)
            budget_inject_failure_after(&budget, style_scope_refuse_after - 1u);
        bool read = ok && style_scope_probe(&navigation, prefix, kept,
                                            sizeof(kept));
        budget_clear_failure_injection(&budget);
        refused = budget.injected_failure_count != injected;
        if (refused) {
            style_scope_refusals_taken++;
            if (read) style_scope_refusals_read++;
        }
        /* A refusal that broke the change's own script leaves nothing to
           compare; the session must still tear down clean. */
        if (ok && !read && refused) {
            if (ready) navigation_destroy(&navigation);
            bool clean = budget.current == 0;
            if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
            if (!clean)
                fprintf(stderr, "style scope %s: refusal %zu leaked\n",
                        test->label, style_scope_refuse_after);
            return clean;
        }
        ok = read;
        if (ok) unscoped += style_scope_full_clears(&navigation) - mark;
        ok = ok && style_scope_cold(&navigation, cold, sizeof(cold));
        if (ok) mark = style_scope_full_clears(&navigation);
    } else {
        ScriptResult result = {0};
        ok = ok && script_runtime_evaluate_diagnostic(
                navigation.page.runtime, prefix, "<style-scope-change>",
                &result) && result.error[0] == '\0';
    }
    ok = ok && script_runtime_consume_relayout(navigation.page.runtime)
        && navigation_relayout(&navigation);
    if (pass == 1) {
        ok = ok && style_scope_probe(&navigation, NULL, kept, sizeof(kept));
        if (ok) unscoped += style_scope_full_clears(&navigation) - mark;
        ok = ok && style_scope_cold(&navigation, cold, sizeof(cold));
        if (ok) mark = style_scope_full_clears(&navigation);
    }
    /* After the relayout, retained (from the reference's fill) and cold
       again. */
    char after[2048] = {0}, after_cold[2048] = {0};
    ok = ok && style_scope_probe(&navigation, NULL, after, sizeof(after));
    if (ok) unscoped += style_scope_full_clears(&navigation) - mark;
    ok = ok && style_scope_cold(&navigation, after_cold, sizeof(after_cold));
    bool scoped = ok && unscoped == 0;
    bool same = ok && strcmp(kept, cold) == 0 && strcmp(after, after_cold) == 0
        && (test->expect == NULL || strstr(after_cold, test->expect) != NULL)
        && (strcmp(after_cold, before) != 0) != test->inert;
    /* Under a refusal the change may not take effect until a later
       install; equality with the cold cache is what must hold. */
    if (refused && ok && strcmp(kept, cold) == 0
        && strcmp(after, after_cold) == 0) same = true;
    bool pass_ok = same && (!test->scoped || scoped || refused);
    if (!pass_ok)
        fprintf(stderr, "style scope %s (pass %d): ok=%d same=%d scoped=%d "
                "full-clears=%zu\n  before=%s\n  kept=%s\n  cold=%s\n"
                "  after=%s\n  after-cold=%s\n  nav=%s\n", test->label,
                pass, (int) ok, (int) same, (int) scoped, unscoped,
                before, kept, cold, after, after_cold, navigation.last_error);
    if (ready) navigation_destroy(&navigation);
    bool clean = budget.current == 0;
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return pass_ok && clean;
}

/* Shared setup: a held constructed sheet `s` (adopted by host #a), a
   second host #b with light children not yet a shadow host, and light
   elements outside both. */
#define STYLE_SCOPE_HELD \
    "<div class=out id=o1>o<i id=o2>i</i></div>" \
    "<div id=a><span id=al>al<b id=alb>b</b></span></div>" \
    "<div id=b><span id=bl>bl<b id=blb>b</b></span></div>" \
    "<div class=out id=o3 style='color:var(--tone,rgb(6,6,6))'>o3</div>"

#define STYLE_SCOPE_SHEET \
    "globalThis.s=new CSSStyleSheet();s.replaceSync(" \
    "':host{color:rgb(1,2,3);--tone:rgb(7,7,7)}" \
    "::slotted(span){font-size:21px;margin-left:3px}" \
    ".in{margin-left:9px;color:var(--tone)}');"

#define STYLE_SCOPE_TARGETS(extra) \
    "globalThis.targets=()=>[o1,o2,a,al,alb,b,bl,blb,o3" extra "]" \
    ".map(x=>typeof x==='string'?document.getElementById(x):x);"

static bool test_style_scope_matches_cold_cache(void)
{
    static const char held_setup[] =
        STYLE_SCOPE_HELD "<script>" STYLE_SCOPE_SHEET
        "globalThis.ra=a.attachShadow({mode:'open'});"
        "ra.innerHTML='<slot></slot><div class=in id=ain>x</div>';"
        "ra.adoptedStyleSheets=[s];"
        "for(const id of ['o1','o2','a','al','alb','b','bl','blb','o3'])"
        "globalThis[id]=document.getElementById(id);"
        STYLE_SCOPE_TARGETS(",ra.getElementById('ain')") "</script>";
    static const char shadow_b_setup[] =
        STYLE_SCOPE_HELD "<script>" STYLE_SCOPE_SHEET
        "for(const id of ['o1','o2','a','al','alb','b','bl','blb','o3'])"
        "globalThis[id]=document.getElementById(id);"
        "globalThis.ra=a.attachShadow({mode:'open'});"
        "ra.innerHTML='<slot></slot><div class=in id=ain>x</div>';"
        "ra.adoptedStyleSheets=[s];"
        "globalThis.rb=b.attachShadow({mode:'open'});"
        "rb.innerHTML='<div id=wrap><slot id=bs></slot></div>"
        "<div class=in id=bin>y</div>';rb.adoptedStyleSheets=[s];"
        STYLE_SCOPE_TARGETS(",ra.getElementById('ain'),"
                            "rb.getElementById('bin')") "</script>";
    /* An outer component whose shadow tree holds an inner component; the
       inner one's light child lives in the outer shadow tree. */
    static const char nested_setup[] =
        STYLE_SCOPE_HELD "<script>" STYLE_SCOPE_SHEET
        "for(const id of ['o1','o2','a','al','alb','b','bl','blb','o3'])"
        "globalThis[id]=document.getElementById(id);"
        "globalThis.ra=a.attachShadow({mode:'open'});"
        "ra.innerHTML='<slot></slot><div id=inner><span id=il>il</span></div>';"
        "globalThis.t=new CSSStyleSheet();"
        "t.replaceSync('#inner::part(p){color:rgb(4,4,4)}"
        "::slotted(span){font-size:19px}');ra.adoptedStyleSheets=[t];"
        "globalThis.inner=ra.getElementById('inner');"
        "globalThis.ri=inner.attachShadow({mode:'open'});"
        "ri.innerHTML='<slot id=is></slot><p id=ip>p</p>';"
        "ri.adoptedStyleSheets=[s];"
        STYLE_SCOPE_TARGETS(",inner,ra.getElementById('il'),"
                            "ri.getElementById('ip')") "</script>";
    static const char order_setup[] =
        STYLE_SCOPE_HELD "<script>"
        "for(const id of ['o1','o2','a','al','alb','b','bl','blb','o3'])"
        "globalThis[id]=document.getElementById(id);"
        "globalThis.x=new CSSStyleSheet();x.replaceSync("
        "':host{color:rgb(10,0,0)}');"
        "globalThis.y=new CSSStyleSheet();y.replaceSync("
        "':host{color:rgb(0,10,0)}');"
        "globalThis.ra=a.attachShadow({mode:'open'});ra.innerHTML='<slot></slot>';"
        "ra.adoptedStyleSheets=[x,y];"
        "globalThis.rb=b.attachShadow({mode:'open'});rb.innerHTML='<slot></slot>';"
        "rb.adoptedStyleSheets=[x,y];"
        STYLE_SCOPE_TARGETS("") "</script>";
    static const char registered_setup[] =
        STYLE_SCOPE_HELD "<script>"
        "for(const id of ['o1','o2','a','al','alb','b','bl','blb','o3'])"
        "globalThis[id]=document.getElementById(id);"
        "globalThis.s=new CSSStyleSheet();s.replaceSync("
        "'@property --tone{syntax:\"<color>\";inherits:false;"
        "initial-value:rgb(8,8,8)}"
        "@keyframes spin{from{opacity:0}to{opacity:1}}"
        ":host{--tone:rgb(1,1,1);animation-name:spin}"
        "::slotted(*){color:var(--tone)}');"
        "globalThis.ra=a.attachShadow({mode:'open'});ra.innerHTML='<slot></slot>';"
        "ra.adoptedStyleSheets=[s];"
        STYLE_SCOPE_TARGETS("") "</script>";
    static const char context_setup[] =
        STYLE_SCOPE_HELD "<script>"
        "for(const id of ['o1','o2','a','al','alb','b','bl','blb','o3'])"
        "globalThis[id]=document.getElementById(id);"
        "document.body.className='dark';"
        "globalThis.s=new CSSStyleSheet();s.replaceSync("
        "':host-context(.dark){color:rgb(2,2,2);margin-left:5px}');"
        "globalThis.ra=a.attachShadow({mode:'open'});ra.innerHTML='<slot></slot>';"
        "ra.adoptedStyleSheets=[s];"
        STYLE_SCOPE_TARGETS("") "</script>";
    /* Components that keep a <style> in each instance's root (no adopted
       sheets): the custom rule index groups those rules by tree, so a
       change to one tree's rules or classes must still reach exactly the
       elements the full cascade gives it. */
    static const char shadow_style_setup[] =
        "<style>.warm{--tone:rgb(9,9,0)}</style>" STYLE_SCOPE_HELD "<script>"
        "for(const id of ['o1','o2','a','al','alb','b','bl','blb','o3'])"
        "globalThis[id]=document.getElementById(id);"
        "const css='<style>:host{--tone:rgb(7,0,0)}.in{color:var(--tone)}"
        "::slotted(span){--tone:rgb(0,7,0)}.hot{--tone:rgb(0,9,0)}</style>';"
        "globalThis.ra=a.attachShadow({mode:'open'});"
        "ra.innerHTML=css+'<slot></slot><div class=in id=ain>x</div>';"
        "globalThis.rb=b.attachShadow({mode:'open'});"
        "rb.innerHTML=css+'<slot></slot><div class=in id=bin>y</div>';"
        STYLE_SCOPE_TARGETS(",ra.getElementById('ain'),"
                            "rb.getElementById('bin')") "</script>";
    const StyleScopeCase cases[] = {
        {"a shadow tree gains a <style> declaring a custom property",
         shadow_style_setup,
         "const st=document.createElement('style');"
         "st.textContent='.in{--tone:rgb(0,0,9)}';rb.appendChild(st)",
         "rgb(0, 0, 9)", false},
        {"a shadow tree's <style> is removed", shadow_style_setup,
         "rb.querySelector('style').remove()", NULL, false},
        {"a shadow tree's <style> text changes", shadow_style_setup,
         "rb.querySelector('style').textContent="
         "':host{--tone:rgb(0,0,8)}.in{color:var(--tone)}'",
         "rgb(0, 0, 8)", false},
        {"a class inside one shadow tree changes", shadow_style_setup,
         "rb.getElementById('bin').className='in hot'", "rgb(0, 9, 0)",
         false},
        {"a host's class changes", shadow_style_setup,
         "b.className='warm'", "rgb(9, 9, 0)", false},
        /* archive.org's results grid: a component upgrades an element the
           page already read, attaching a shadow root that adopts the
           component's held sheet. */
        {"a host attaches a root that adopts a held sheet", held_setup,
         "const r=b.attachShadow({mode:'open'});"
         "r.innerHTML='<slot></slot><div class=in>y</div>';"
         "r.adoptedStyleSheets=[s]", "rgb(1, 2, 3)", true},
        {"a root drops a held sheet after a read installed it again",
         shadow_b_setup,
         "rb.adoptedStyleSheets=[];void bl.offsetWidth;"
         "rb.adoptedStyleSheets=[s];void bl.offsetWidth;"
         "rb.adoptedStyleSheets=[]", NULL, true},
        {"a root drops a sheet another root holds", shadow_b_setup,
         "rb.adoptedStyleSheets=[]", NULL, true},
        {"a shadow tree loses its slot", shadow_b_setup,
         "rb.getElementById('bs').remove()", NULL, true},
        {"a slot is renamed", shadow_b_setup,
         "rb.getElementById('bs').setAttribute('name','other')", NULL, true},
        {"a slot's ancestor is hidden", shadow_b_setup,
         "rb.getElementById('wrap').setAttribute('hidden','')", NULL, true},
        /* innerText reads the same retained styles: a light child left
           without a slot renders nothing. */
        {"a host attaches a root without a slot", held_setup,
         "b.attachShadow({mode:'open'})", NULL, true},
        /* The read cannot install the change (a <link> waits for the
           relayout), so it answers from the cascade as it stands and the
           relayout's in-place install must drop what it reaches. */
        {"a root drops a held sheet beside a <link>", shadow_b_setup,
         "const l=document.createElement('link');l.rel='stylesheet';"
         "l.href='data:text/css,';document.head.appendChild(l);"
         "rb.adoptedStyleSheets=[];void bl.offsetWidth", NULL, false},
        /* A light-tree change, scoped on the whole-clear code too. */
        {"a light child changes slot", shadow_b_setup,
         "bl.setAttribute('slot','other')", NULL, true},
        {"an inner component loses its slot", nested_setup,
         "ri.getElementById('is').remove()", NULL, true},
        {"an inner shadow element gains a part", nested_setup,
         "ri.getElementById('ip').setAttribute('part','p')",
         NULL, true, true},
        {"a root adopts held sheets in another order", order_setup,
         "rb.adoptedStyleSheets=[y,x]", "rgb(10, 0, 0)", true},
        {"a new root adopts a sheet with @property and keyframes",
         registered_setup,
         "const r=b.attachShadow({mode:'open'});r.innerHTML='<slot></slot>';"
         "r.adoptedStyleSheets=[s]", NULL, true},
        {"a new root adopts a sheet with :host-context", context_setup,
         "const r=b.attachShadow({mode:'open'});r.innerHTML='<slot></slot>';"
         "r.adoptedStyleSheets=[s]", NULL, true, true},
        /* Changes that reach outside one host keep the whole clear. */
        {"an ancestor class moves :host-context", context_setup,
         "document.body.className=''", NULL, false, true},
        {"the document adopts the held sheet", held_setup,
         "const d=new CSSStyleSheet();d.replaceSync("
         "'.out{margin-left:11px}');document.adoptedStyleSheets=[d]",
         "11px", false},
        {"the held sheet's text is replaced", held_setup,
         "s.replaceSync(':host{color:rgb(3,2,1)}')", "rgb(3, 2, 1)", false},
    };
    bool ok = true;
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            if (!style_scope_case(&cases[i], pass)) {
                fprintf(stderr, "style scope failed: %s (pass %d)\n",
                        cases[i].label, pass);
                ok = false;
            }
        }
    }
    return ok;
}

/* Seventeen light hosts (one more than a scoped drop names) with a light
   child each, beside the shared elements; `s` is held, adopted by none. */
#define STYLE_SCOPE_MANY \
    "<div id=many></div>" STYLE_SCOPE_HELD "<script>" STYLE_SCOPE_SHEET \
    "for(const id of ['o1','o2','a','al','alb','b','bl','blb','o3'])" \
    "globalThis[id]=document.getElementById(id);" \
    "globalThis.H=[];const m=document.getElementById('many');" \
    "for(let i=0;i<17;i++){const h=document.createElement('div');" \
    "const l=document.createElement('span');l.textContent='l'+i;" \
    "h.appendChild(l);m.appendChild(h);H.push(h)}" \
    "globalThis.targets=()=>[o1,o2,a,al,alb,b,bl,blb,o3]" \
    ".concat(H,H.map(h=>h.firstChild));"

/* The ways a turn's changes overflow or outlive the scopes the drops
   name: more hosts than a drop names, more mutated subtrees than the
   queue holds, a queued host gone or replaced before the read, and shadow
   and document-wide changes together. Every answer must equal the cold
   cache's; none may keep a stale style. */
static bool test_style_scope_overflow_and_mixed_turns(void)
{
    static const char many_setup[] = STYLE_SCOPE_MANY "</script>";
    static const char held_setup[] =
        STYLE_SCOPE_HELD "<script>" STYLE_SCOPE_SHEET
        "for(const id of ['o1','o2','a','al','alb','b','bl','blb','o3'])"
        "globalThis[id]=document.getElementById(id);"
        "globalThis.ra=a.attachShadow({mode:'open'});"
        "ra.innerHTML='<slot></slot><div class=in id=ain>x</div>';"
        "ra.adoptedStyleSheets=[s];"
        STYLE_SCOPE_TARGETS(",ra.getElementById('ain')") "</script>";
    const StyleScopeCase cases[] = {
        {"seventeen hosts attach roots that adopt a held sheet", many_setup,
         "for(const h of H){const r=h.attachShadow({mode:'open'});"
         "r.innerHTML='<slot></slot>';r.adoptedStyleSheets=[s]}",
         "rgb(1, 2, 3)", false},
        {"seventeen light subtrees change in one turn", many_setup,
         "for(const h of H)h.firstChild.className='out'", NULL, false},
        {"a host is removed after it adopts", held_setup,
         "const r=b.attachShadow({mode:'open'});r.innerHTML='<slot></slot>';"
         "r.adoptedStyleSheets=[s];b.remove()", NULL, false},
        {"a host is replaced after it adopts", held_setup,
         "const r=b.attachShadow({mode:'open'});r.innerHTML='<slot></slot>';"
         "r.adoptedStyleSheets=[s];const n=document.createElement('div');"
         "n.append(...b.childNodes);b.replaceWith(n)", NULL, false},
        {"a root's host is replaced after the root drops its sheet",
         held_setup,
         "ra.adoptedStyleSheets=[];const n=document.createElement('div');"
         "n.append(...a.childNodes);a.replaceWith(n)", NULL, false},
        {"a root adopts while the document adopts too", held_setup,
         "const r=b.attachShadow({mode:'open'});r.innerHTML='<slot></slot>';"
         "r.adoptedStyleSheets=[s];const d=new CSSStyleSheet();"
         "d.replaceSync('.out{margin-left:11px}');"
         "document.adoptedStyleSheets=[d]", "11px", false},
        {"a root adopts while the body's inherited style changes",
         held_setup,
         "const r=b.attachShadow({mode:'open'});r.innerHTML='<slot></slot>';"
         "r.adoptedStyleSheets=[s];document.body.style.fontSize='13px'",
         "13px", false},
        {"a root adopts while a light rule's sheet text changes", held_setup,
         "const r=b.attachShadow({mode:'open'});r.innerHTML='<slot></slot>';"
         "r.adoptedStyleSheets=[s];document.querySelector('style')"
         ".textContent+='.out{font-size:17px}'", "17px", false},
    };
    bool ok = true;
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            if (!style_scope_case(&cases[i], pass)) {
                fprintf(stderr, "style scope failed: %s (pass %d)\n",
                        cases[i].label, pass);
                ok = false;
            }
        }
    }
    return ok;
}

/* One allocation refused at each point from a change's turn to its read
   (the sheet settle installs the change in that read): the install either
   lands whole, keeps the old cascade or drops every retained style, and
   the read must equal the cold cache's either way, with nothing leaked.
   The sweep stops once refusals no longer reach the turn. */
static bool test_style_scope_install_refusals(void)
{
    static const char held_setup[] =
        STYLE_SCOPE_HELD "<script>" STYLE_SCOPE_SHEET
        "for(const id of ['o1','o2','a','al','alb','b','bl','blb','o3'])"
        "globalThis[id]=document.getElementById(id);"
        "globalThis.ra=a.attachShadow({mode:'open'});"
        "ra.innerHTML='<slot></slot><div class=in id=ain>x</div>';"
        "ra.adoptedStyleSheets=[s];globalThis.t=new CSSStyleSheet();"
        "t.replaceSync(':host{margin-left:4px}::slotted(b){color:rgb(9,1,1)}');"
        STYLE_SCOPE_TARGETS(",ra.getElementById('ain')") "</script>";
    const StyleScopeCase cases[] = {
        /* Scoped without a refusal: the sheet is already installed. */
        {"a host attaches a root that adopts a held sheet", held_setup,
         "const r=b.attachShadow({mode:'open'});"
         "r.innerHTML='<slot></slot><div class=in>y</div>';"
         "r.adoptedStyleSheets=[s]", "rgb(1, 2, 3)", true},
        /* A sheet no root held yet: its rules are installed. */
        {"a root adopts a second sheet", held_setup,
         "ra.adoptedStyleSheets=[s,t]", NULL, false},
        {"a host attaches a root that adopts two sheets", held_setup,
         "const r=b.attachShadow({mode:'open'});r.innerHTML='<slot></slot>';"
         "r.adoptedStyleSheets=[t,s]", NULL, false},
    };
    bool ok = true;
    style_scope_quiet = true;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        style_scope_refusals_taken = style_scope_refusals_read = 0;
        size_t points = 0;
        /* TILEFINCH_STYLE_SCOPE_REFUSAL_AT=N runs only refusal point N. */
        const char *only = getenv("TILEFINCH_STYLE_SCOPE_REFUSAL_AT");
        size_t first = only == NULL ? 1u : (size_t) strtoul(only, NULL, 10) + 1u;
        for (size_t after = first; after <= (only == NULL ? 20000u : first);
             after++) {
            size_t taken = style_scope_refusals_taken;
            style_scope_refuse_after = after;
            if (!style_scope_case(&cases[i], 0)) {
                fprintf(stderr, "style scope refusal failed: %s (refusal "
                        "after %zu allocations)\n", cases[i].label,
                        after - 1u);
                ok = false;
                break;
            }
            if (style_scope_refusals_taken == taken) break;
            points++;
        }
        if (getenv("TILEFINCH_STYLE_SCOPE_REFUSAL_REPORT") != NULL)
            fprintf(stderr, "style scope refusal %s: points=%zu read=%zu\n",
                    cases[i].label, points, style_scope_refusals_read);
        /* The sweep must have refused inside the turn and still read. */
        if (points == 0 || style_scope_refusals_read == 0) {
            fprintf(stderr, "style scope refusal %s: points=%zu read=%zu\n",
                    cases[i].label, points, style_scope_refusals_read);
            ok = false;
        }
    }
    style_scope_refuse_after = 0;
    style_scope_quiet = false;
    return ok;
}
