/* Mutations confined to content that renders nothing (display:none) skip
   the relayout; anything that can reach rendered content still relayouts.
   Included by test_tilefinch.c and run from the web-runtime suite. */

static uint64_t hidden_mutation_layout_signature(const LayoutDocument *layout)
{
    uint64_t hash = UINT64_C(1469598103934665603);
#define HIDDEN_MUTATION_MIX(value) \
    (hash = (hash ^ (uint64_t) (uint32_t) (value)) * UINT64_C(1099511628211))
    HIDDEN_MUTATION_MIX(layout->count);
    HIDDEN_MUTATION_MIX(layout->height);
    for (size_t i = 0; i < layout->count; i++) {
        const DrawCommand *command = &layout->commands[i];
        HIDDEN_MUTATION_MIX(command->type);
        HIDDEN_MUTATION_MIX(command->x);
        HIDDEN_MUTATION_MIX(command->y);
        HIDDEN_MUTATION_MIX(command->width);
        HIDDEN_MUTATION_MIX(command->height);
        HIDDEN_MUTATION_MIX(command->color);
        for (uint32_t at = 0; command->text != NULL
                              && at < command->text_length; at++)
            HIDDEN_MUTATION_MIX((unsigned char) command->text[at]);
    }
#undef HIDDEN_MUTATION_MIX
    return hash;
}

/* Runs `script` as one author turn and applies the journal it left. */
static bool hidden_mutation_turn(NavigationSession *session,
                                 const char *script)
{
    return script_runtime_evaluate_diagnostic(
               session->page.runtime, script, "<hidden-mutation>",
               &session->page.script_result)
        && script_runtime_consume_relayout(session->page.runtime)
        && navigation_relayout(session);
}

static int test_hidden_mutation_relayouts(void)
{
    puts("test: mutations inside display:none content skip relayout");
    Budget budget;
    budget_init(&budget, 24 * MIB);
    budget_install_lexbor(&budget);
    static const char page[] =
        "<!doctype html><html><head><style>"
        ".row{padding:2px}.row:last-child{border-bottom:3px solid #c00}"
        ".wrap:has(.flag){background:#0a0}"
        ".slot:empty{display:none}"
        "</style></head><body>"
        "<div id=list><div class=row>ALPHA</div><div class=row>BETA</div>"
        "</div><div class=wrap id=wrap><span>WRAP</span></div>"
        "<p id=text>TEXT</p>"
        "<div id=closed style=display:none><span id=inner>x</span>"
        "<span id=other>y</span></div>"
        "<div class=slot id=slot></div>"
        "<ol id=ol><li>ONE</li><li>TWO</li></ol><div id=filler>";
    /* Pages under 256 nodes keep no layout reuse cache, and with it no
       selector facts: only a filler makes the page one that has them. */
    static char page_text[16 * 1024];
    size_t used = (size_t) snprintf(page_text, sizeof(page_text), "%s", page);
    for (int i = 0; i < 300 && used + 16 < sizeof(page_text); i++)
        used += (size_t) snprintf(page_text + used, sizeof(page_text) - used,
                                  "<b>%d</b>", i % 10);
    used += (size_t) snprintf(page_text + used, sizeof(page_text) - used,
                              "</div></body></html>");
    static NavigationSession session;
    CHECK(navigation_init(&session, &budget, 2));
    navigation_enable_scripts(&session, 4 * MIB, 1000);
    uint64_t generation = navigation_begin(&session);
    CHECK(navigation_commit_html(&session, generation,
                                 "https://hidden-mutation.test/", page_text,
                                 used, 480, NULL, NULL, true)
          && session.page.runtime != NULL);
    uint64_t committed = hidden_mutation_layout_signature(&session.page.layout);
    size_t relayouts = session.incremental_relayouts;

    /* A consent-management locator stub: a display:none <iframe> appended
       to <body> once, then found by name (window.frames) instead of being
       inserted again. */
    static const char locator_stub[] =
        "function tt(e){if(!window.frames[e]){if(document.body){"
        "const t=document.createElement('iframe');t.style.cssText="
        "'display:none';t.name=e;document.body.appendChild(t)}return true}"
        "return false}globalThis.pocSummary=String(tt('__tcfapiLocator'))";
    CHECK(hidden_mutation_turn(&session, locator_stub)
          && strcmp(session.page.script_result.summary, "true") == 0
          && session.performance.hidden_relayout_skips == 1
          && session.incremental_relayouts == relayouts);
    CHECK(script_runtime_evaluate_diagnostic(
              session.page.runtime,
              "globalThis.pocSummary=String(tt('__tcfapiLocator'))+':'+"
              "document.querySelectorAll('iframe').length",
              "<locator-found>", &session.page.script_result)
          && strcmp(session.page.script_result.summary, "false:1") == 0
          && !script_runtime_consume_relayout(session.page.runtime));

    /* A subtree inserted into, an attribute and inline style written
       inside, and a node removed from a display:none container. */
    CHECK(hidden_mutation_turn(&session,
              "const d=document.createElement('div');"
              "d.innerHTML='<b>deep</b><i>er</i>';"
              "document.getElementById('closed').appendChild(d)")
          && session.performance.hidden_relayout_skips == 2);
    CHECK(hidden_mutation_turn(&session,
              "const n=document.getElementById('inner');"
              "n.className='changed';n.setAttribute('data-k','1');"
              "n.style.color='red';n.textContent='z'")
          && session.performance.hidden_relayout_skips == 3);
    CHECK(hidden_mutation_turn(&session,
              "document.getElementById('other').remove()")
          && session.performance.hidden_relayout_skips == 4);
    /* Attributes and non-display inline properties of the element that is
       itself display:none. */
    CHECK(hidden_mutation_turn(&session,
              "const f=document.querySelector('iframe');f.title='consent';"
              "f.style.width='0px';f.setAttribute('aria-hidden','true')")
          && session.performance.hidden_relayout_skips == 5);
    /* A non-executing script block appended to <body>. */
    CHECK(hidden_mutation_turn(&session,
              "const s=document.createElement('script');"
              "s.type='application/json';s.textContent='{}';"
              "document.body.appendChild(s)")
          && session.performance.hidden_relayout_skips == 6
          && session.incremental_relayouts == relayouts);
    CHECK(hidden_mutation_layout_signature(&session.page.layout)
          == committed);

    /* A real relayout of the final tree draws exactly what was kept. */
    CHECK(hidden_mutation_turn(&session,
              "document.body.setAttribute('data-force','1')")
          && session.incremental_relayouts == relayouts + 1
          && hidden_mutation_layout_signature(&session.page.layout)
             == committed);

    /* Changes that can reach rendered content keep the relayout. A hidden
       last row ends BETA's .row:last-child border. */
    size_t skips = session.performance.hidden_relayout_skips;
    uint64_t before = hidden_mutation_layout_signature(&session.page.layout);
    CHECK(hidden_mutation_turn(&session,
              "const r=document.createElement('div');r.className='row';"
              "r.setAttribute('style','display:none');"
              "document.getElementById('list').appendChild(r)")
          && session.incremental_relayouts == relayouts + 2
          && hidden_mutation_layout_signature(&session.page.layout)
             != before);
    /* A hidden .flag turns on .wrap:has(.flag). */
    before = hidden_mutation_layout_signature(&session.page.layout);
    CHECK(hidden_mutation_turn(&session,
              "const g=document.createElement('i');g.className='flag';"
              "g.hidden=true;document.getElementById('wrap').appendChild(g)")
          && session.incremental_relayouts == relayouts + 3
          && hidden_mutation_layout_signature(&session.page.layout)
             != before);
    /* A hidden child defeats .slot:empty{display:none}. */
    CHECK(hidden_mutation_turn(&session,
              "const c=document.createElement('span');"
              "c.setAttribute('style','display:none');"
              "document.getElementById('slot').appendChild(c)")
          && session.incremental_relayouts == relayouts + 4);
    /* Hiding a rendered element. */
    CHECK(hidden_mutation_turn(&session,
              "document.getElementById('text').style.display='none'")
          && session.incremental_relayouts == relayouts + 5
          && command_x(&session.page.layout, "TEXT") < 0);
    /* Layout numbers list items whether or not they render. */
    CHECK(hidden_mutation_turn(&session,
              "const l=document.createElement('li');"
              "l.setAttribute('style','display:none');"
              "const o=document.getElementById('ol');"
              "o.insertBefore(l,o.firstChild)")
          && session.incremental_relayouts == relayouts + 6
          && session.performance.hidden_relayout_skips == skips);

    /* A logical argument can inspect an ancestor outside the :has()
       anchor. Inserting a hidden sibling of that ancestor changes its
       last-child result, and therefore the visible card's background. */
    CHECK(hidden_mutation_turn(&session,
              "(()=>{const s=document.createElement('style');s.textContent="
              "'.nestedCard:has(:is(.nestedRow:last-child .inside))'"
              "+'{background:#a00}';document.head.appendChild(s);"
              "const rows=document.createElement('div');rows.id='nestedRows';"
              "rows.innerHTML='<div class=nestedRow><div class=nestedCard>'"
              "+'<span class=inside>NESTED</span></div></div>';"
              "document.body.appendChild(rows)})()"));
    before = hidden_mutation_layout_signature(&session.page.layout);
    skips = session.performance.hidden_relayout_skips;
    CHECK(hidden_mutation_turn(&session,
              "(()=>{const r=document.createElement('div');r.style.display='none';"
              "document.getElementById('nestedRows').appendChild(r)})()")
          && session.performance.hidden_relayout_skips == skips
          && hidden_mutation_layout_signature(&session.page.layout) != before);

    navigation_destroy(&session);
    CHECK(budget.current == 0);
    return 0;
}
