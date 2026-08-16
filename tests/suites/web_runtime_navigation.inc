    puts("test: bounded navigation lifecycle");
    static const char nav_a[] =
        "<!doctype html><title>A</title><style>p{color:#112233}</style>"
        "<body><p>ALPHA PAGE</p></body>";
    static const char nav_b[] =
        "<!doctype html><title>B</title><style>p{color:#445566}</style>"
        "<body><p>BETA PAGE</p></body>";
    NavigationSession navigation;
    CHECK(navigation_init(&navigation, &budget, 8));
    uint64_t cancelled_generation = navigation_begin(&navigation);
    navigation_cancel(&navigation, cancelled_generation);
    CHECK(!navigation_commit_html(&navigation, cancelled_generation,
                                  "https://example.test/cancelled", nav_a,
                                  strlen(nav_a), 480, NULL, NULL, true));
    size_t stable_navigation_bytes = 0;
    for (unsigned i = 0; i < 100; i++) {
        bool alternate = (i & 1u) != 0;
        uint64_t generation = navigation_begin(&navigation);
        CHECK(navigation_commit_html(
            &navigation, generation,
            alternate ? "https://example.test/b" : "https://example.test/a",
            alternate ? nav_b : nav_a,
            alternate ? strlen(nav_b) : strlen(nav_a),
            480, NULL, NULL, true));
        const NavigationEntry *current = navigation_current(&navigation);
        CHECK(current != NULL && strcmp(current->title,
                                        alternate ? "B" : "A") == 0);
        if (i == 20) stable_navigation_bytes = budget.current;
    }
    CHECK(navigation.history_count == 8
          && navigation.loads_started == 101
          && navigation.loads_committed == 100
          && navigation.loads_cancelled == 1
          && navigation.page_destroys == 99
          && budget.current <= stable_navigation_bytes + 1024);
    const NavigationEntry *history_entry = NULL;
    CHECK(navigation_back(&navigation, &history_entry)
          && history_entry != NULL);
    navigation_set_scroll(&navigation, 123);
    CHECK(navigation_current(&navigation)->scroll_y == 123
          && navigation_forward(&navigation, &history_entry));
    navigation_set_scroll(&navigation, 321);
    CHECK(navigation_current(&navigation)->scroll_y == 321
          && navigation_back(&navigation, &history_entry)
          && navigation_current(&navigation)->scroll_y == 123
          && navigation_forward(&navigation, &history_entry)
          && navigation_current(&navigation)->scroll_y == 321);
    navigation_destroy(&navigation);
    CHECK(budget.current == 0);

    puts("test: section document replacement preserves the JavaScript realm");
    static const char realm_a[] =
        "<!doctype html><title>Realm A</title><body><p id=old>OLD</p>"
        "<script>globalThis.sectionState=41;globalThis.oldSectionNode="
        "document.getElementById('old');globalThis.hashEvents=[];"
        "addEventListener('hashchange',event=>hashEvents.push("
        "event.oldURL+'>'+event.newURL))</script></body>";
    static const char realm_b[] =
        "<!doctype html><title>Realm B</title><body><p id=new>NEW</p></body>";
    NavigationSession realm_navigation;
    CHECK(navigation_init(&realm_navigation, &budget, 4));
    navigation_enable_scripts(&realm_navigation, 4 * MIB, 1000);
    uint64_t realm_generation = navigation_begin(&realm_navigation);
    CHECK(navigation_commit_html(
        &realm_navigation, realm_generation, "https://example.test/realm",
        realm_a, strlen(realm_a), 480, NULL, NULL, true));
    ScriptRuntime *retained_realm = realm_navigation.page.runtime;
    realm_generation = navigation_begin(&realm_navigation);
    CHECK(navigation_commit_html_preserve_runtime(
        &realm_navigation, realm_generation, "https://example.test/realm",
        realm_b, strlen(realm_b), 480, NULL, NULL, false));
    CHECK(realm_navigation.page.runtime == retained_realm);
    size_t realm_swap_baseline = budget.current;
    for (size_t realm_swap = 0; realm_swap < 8; realm_swap++) {
        static const char wrapper_probe[] =
            "document.querySelectorAll('*');globalThis.sectionState++";
        CHECK(script_runtime_evaluate_diagnostic(
            realm_navigation.page.runtime, wrapper_probe,
            "<realm-wrapper-probe>", &realm_navigation.page.script_result));
        realm_generation = navigation_begin(&realm_navigation);
        CHECK(navigation_commit_html_preserve_runtime(
            &realm_navigation, realm_generation,
            "https://example.test/realm", realm_b, strlen(realm_b), 480,
            NULL, NULL, false));
        CHECK(realm_navigation.page.runtime == retained_realm);
    }
    /* The broader standards bootstrap moves QuickJS's allocator across one
       additional 32 KiB slab during this artificial eight-rebind wrapper
       stress. Measured growth is 287,296 bytes; this host-only ratchet does
       not change the runtime heap or wrapper caps. */
    if (budget.current > realm_swap_baseline + 320u * 1024u)
        fprintf(stderr,
                "realm swap retained=%zu baseline=%zu growth=%zu\n",
                budget.current, realm_swap_baseline,
                budget.current - realm_swap_baseline);
    CHECK(budget.current <= realm_swap_baseline + 320u * 1024u);
    CHECK(navigation_commit_same_document_url(
              &realm_navigation, "https://example.test/realm#new")
          && realm_navigation.history_count == 2
          && strcmp(navigation_current(&realm_navigation)->url,
                    "https://example.test/realm#new") == 0);
    static const char realm_probe[] =
        "globalThis.pocSummary=String(globalThis.sectionState)+':' +"
        "document.getElementById('new').textContent+':' +"
        "String(globalThis.oldSectionNode.textContent)+':' +"
        "document.body.textContent+':' +location.hash+':' +history.length+':' +"
        "globalThis.hashEvents.length";
    CHECK(script_runtime_evaluate_diagnostic(
              realm_navigation.page.runtime, realm_probe, "<realm-probe>",
              &realm_navigation.page.script_result)
          && strcmp(realm_navigation.page.script_result.summary,
                    "49:NEW::NEW:#new:2:1") == 0);
    navigation_destroy(&realm_navigation);
    CHECK(budget.current == 0);

    puts("test: persistent runtime and incremental relayout");
    static const char runtime_page[] =
        "<!doctype html><meta name=viewport content='width=device-width'>"
        "<title>Runtime</title><style>"
        "#status{display:block;color:#123456}"
        "#draw,#remove-on-down{display:block;width:120px;height:40px}"
        "[data-unrelated]{color:#654321}</style><body>"
        "<p id=status>WAITING</p><div id=draw tabindex=0></div>"
        "<div id=remove-on-down></div><a href='/next'>NEXT PAGE</a>"
        "<form action='/submit' method='post'><input id=edit name=q value='A'>"
        "<button id=go name=send value=Go>GO</button></form>"
        "<div id=prompt contenteditable=true data-placeholder='Ask anything'></div>"
        "<form id=search-form action='/search/?q='><input id=search name=q "
        "type=search value=PSP></form>"
        "<input id=password type=password value=secret>"
        "<input id=otp autocomplete='section-login one-time-code' value=123456>"
        "<script id=external src='/external.js'></script><script>"
        "localStorage.setItem('theme','psp');document.cookie='mode=reader';"
        "let ticks=0;const status=document.getElementById('status');"
        "status.addEventListener('load',()=>status.setAttribute('data-load','yes'));"
        "const edit=document.getElementById('edit');"
        "status.addEventListener('click',()=>status.setAttribute('data-pointer','yes'));"
        "const draw=document.getElementById('draw'),drawEvents=[];"
        "for(const type of ['pointermove','mousemove','pointerdown','mousedown','focus','pointerup','mouseup','click'])draw.addEventListener(type,event=>{drawEvents.push(type==='focus'?'focus':type+':'+event.clientX+','+event.clientY+':'+event.buttons+':'+event.offsetX+','+event.offsetY);status.setAttribute('data-draw',drawEvents.join('|'))});"
        "document.getElementById('remove-on-down').addEventListener('pointerdown',event=>event.currentTarget.remove());"
        "const editActivation=[];for(const type of ['pointerdown','mousedown','focus','pointerup','mouseup','click'])edit.addEventListener(type,()=>{editActivation.push(type);status.setAttribute('data-edit-activation',editActivation.join('|'))});"
        "edit.addEventListener('beforeinput',event=>{status.setAttribute('data-before',event.constructor.name+':'+event.inputType+':'+event.data+':'+edit.getAttribute('value')+':'+event.isTrusted);if(event.data==='X')event.preventDefault();});"
        "edit.addEventListener('input',event=>{status.setAttribute('data-value',edit.getAttribute('value'));status.setAttribute('data-input',event.constructor.name+':'+event.inputType+':'+event.data+':'+edit.getAttribute('value')+':'+event.isTrusted);});"
        "document.getElementById('prompt').addEventListener('input',()=>status.setAttribute('data-prompt','yes'));"
        "const go=document.getElementById('go'),activation=[];for(const type of ['pointerdown','mousedown','pointerup','mouseup','click'])go.addEventListener(type,event=>activation.push(type+':'+event.constructor.name+':'+event.isTrusted));go.addEventListener('click',()=>{status.textContent='CLICKED';status.setAttribute('data-activation',activation.join('|'));});edit.closest('form').addEventListener('submit',event=>status.setAttribute('data-submit',event.constructor.name+':'+(event.submitter===go)+':'+event.isTrusted));"
        "document.getElementById('search-form').addEventListener('submit',event=>status.setAttribute('data-search-submit',event.constructor.name+':'+(event.submitter===null)+':'+event.isTrusted));"
        "document.getElementById('external').addEventListener('load',()=>status.setAttribute('data-external','loaded'));"
        "setTimeout(()=>{ticks++;status.textContent='TIMER '+ticks;},10);"
        "</script></body>";
    NavigationSession interactive;
    BrowserSession browser_session;
    CHECK(browser_session_init(&browser_session, &budget, 256 * 1024));
    CHECK(navigation_init(&interactive, &budget, 4));
    navigation_attach_browser_session(&interactive, &browser_session);
    navigation_enable_scripts(&interactive, 4 * MIB, 1000);
    uint64_t runtime_generation = navigation_begin(&interactive);
    CHECK(navigation_commit_html(&interactive, runtime_generation,
                                 "https://example.test/runtime",
                                 runtime_page, strlen(runtime_page), 480,
                                 NULL, NULL, true));
    if (interactive.page.script_result.pending_tasks != 1
        || interactive.page.layout.control_count != 6) {
        lxb_dom_node_t *debug_prompt = find_id(
            lxb_dom_interface_node(interactive.page.document.html), "prompt");
        size_t debug_editable_length = 0;
        const char *debug_editable = document_attribute(
            debug_prompt, "contenteditable", &debug_editable_length);
        fprintf(stderr, "runtime pending=%zu controls=%zu error=%s\n",
                interactive.page.script_result.pending_tasks,
                interactive.page.layout.control_count,
                interactive.page.script_result.error);
        fprintf(stderr, "prompt=%p editable=%.*s length=%zu\n",
                (void *) debug_prompt, (int) debug_editable_length,
                debug_editable == NULL ? "" : debug_editable,
                debug_editable_length);
        for (size_t debug_i = 0;
             debug_i < interactive.page.layout.control_count; debug_i++) {
            fprintf(stderr, "control[%zu] type=%d node=%p\n", debug_i,
                    (int) interactive.page.layout.controls[debug_i].type,
                    (void *) interactive.page.layout.controls[debug_i].node);
        }
    }
    CHECK(interactive.page.script_result.pending_tasks == 1
          && command_x(&interactive.page.layout, "WAITING") >= 0
          && interactive.page.layout.control_count == 6
          && interactive.performance.resource_fingerprint_scans == 1);
    const char *stored_theme = NULL;
    size_t stored_theme_length = 0;
    char cookie_header[128];
    CHECK(browser_session_storage_get(
              &browser_session, "https://example.test/runtime", true,
              "theme", &stored_theme, &stored_theme_length)
          && stored_theme_length == 3
          && memcmp(stored_theme, "psp", 3) == 0
          && browser_session_cookie_get(
              &browser_session, "https://example.test/runtime",
              cookie_header, sizeof(cookie_header))
          && strcmp(cookie_header, "mode=reader") == 0);
    CHECK(browser_session_cookie_set_http(
              &browser_session, "https://chatgpt.com/cdn-cgi/challenge",
              "__cf_bm=opaque; Domain=chatgpt.com; Path=/; Secure; "
              "HttpOnly; SameSite=None")
          && browser_session_cookie_get(
              &browser_session, "https://chatgpt.com/", cookie_header,
              sizeof(cookie_header))
          && cookie_header[0] == '\0'
          && browser_session_cookie_header(
              &browser_session, "https://chatgpt.com/", cookie_header,
              sizeof(cookie_header))
          && strcmp(cookie_header, "__cf_bm=opaque") == 0
          && browser_session_cookie_header(
              &browser_session, "http://chatgpt.com/", cookie_header,
              sizeof(cookie_header))
          && cookie_header[0] == '\0'
          && browser_session_cookie_header(
              &browser_session, "https://example.com/", cookie_header,
              sizeof(cookie_header))
          && cookie_header[0] == '\0');
    bool retry_set = browser_session_cookie_set_http(
              &browser_session, "https://chatgpt.com/", "retry=1; Path=/")
          && browser_session_cookie_set_http(
              &browser_session, "https://chatgpt.com/",
              "retry=gone; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT");
    bool retry_header = browser_session_cookie_header(
              &browser_session, "https://chatgpt.com/", cookie_header,
              sizeof(cookie_header));
    CHECK(retry_set && retry_header
          && strcmp(cookie_header, "__cf_bm=opaque") == 0
          && browser_session_cookie_set_http(
              &browser_session, "https://chatgpt.com/", "short=1; Max-Age=0")
          && browser_session_cookie_header(
              &browser_session, "https://chatgpt.com/", cookie_header,
              sizeof(cookie_header))
          && strcmp(cookie_header, "__cf_bm=opaque") == 0);
    CHECK(navigation_advance_runtime(&interactive, 5, 4)
          && interactive.page.script_result.timer_callbacks_run == 0
          && interactive.incremental_relayouts == 0);
    CHECK(navigation_dispatch_event(&interactive, "#status", "load")
          && interactive.incremental_relayouts == 1
          && interactive.performance.fast_relayouts == 1
          && interactive.performance.full_relayouts == 0
          && interactive.performance.mutation_fast_relayouts == 1
          && interactive.performance.mutation_conservative_scans == 0
          && interactive.performance.resource_fingerprint_scans == 1);
    lxb_dom_node_t *runtime_status = find_id(
        lxb_dom_interface_node(interactive.page.document.html), "status");
    size_t runtime_load_length = 0;
    const char *runtime_load = document_attribute(
        runtime_status, "data-load", &runtime_load_length);
    CHECK(runtime_load != NULL && runtime_load_length == 3
          && memcmp(runtime_load, "yes", 3) == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              interactive.page.runtime,
              "document.getElementById('status').setAttribute("
              "'aria-label','ready')",
              "<semantic-attribute-mutation>",
              &interactive.page.script_result)
          && script_runtime_consume_relayout(interactive.page.runtime)
          && navigation_relayout(&interactive)
          && interactive.incremental_relayouts == 1
          && interactive.performance.fast_relayouts == 1
          && interactive.performance.semantic_relayout_skips == 1);
    CHECK(navigation_advance_runtime(&interactive, 5, 4)
          && interactive.page.script_result.timer_callbacks_run == 1
          && interactive.page.script_result.runtime_ticks == 2
          && interactive.page.script_result.pending_tasks == 0
          && interactive.incremental_relayouts == 2
          && interactive.performance.fast_relayouts == 2
          && interactive.performance.mutation_fast_relayouts == 2
          /* Ordinary text replacement is typed layout-only: the second
             relayout does not repeat the whole-document resource walk. */
          && interactive.performance.resource_fingerprint_scans == 1
          && interactive.performance.relayout_us > 0
          && interactive.relayout_damage_valid
          && command_x(&interactive.page.layout, "TIMER") >= 0);
    CHECK(script_runtime_evaluate_diagnostic(
              interactive.page.runtime,
              "(()=>{const node=document.getElementById('status');"
              "node.style.color='#654321';node.style.whiteSpace='nowrap';"
              "node.style.overflowX='hidden';"
              "node.style.textOverflow='ellipsis';"
              "node.style.outline='2px dashed #123456';"
              "node.style.outlineOffset='-3px';"
              "node.style.clipPath='inset(7% round 4px)';"
              "node.style.filter='brightness(.5)';"
              "node.style.borderCollapse='collapse'})()",
              "<typed-style-mutation>", &interactive.page.script_result)
          && script_runtime_consume_relayout(interactive.page.runtime)
          && navigation_relayout(&interactive)
          && interactive.performance.fast_relayouts == 3
          && interactive.performance.full_relayouts == 0
          && interactive.performance.mutation_resource_rebuilds == 0
          && interactive.performance.resource_fingerprint_scans == 1
          && command_x(&interactive.page.layout, "TIMER") >= 0);
    CHECK(script_runtime_evaluate_diagnostic(
              interactive.page.runtime,
              "(()=>{const style=getComputedStyle("
              "document.getElementById('status')),actual=["
              "style.textOverflow,style.overflowX,style.outlineWidth,"
              "style.outlineStyle,style.outlineColor,style.outlineOffset,"
              "style.clipPath,style.filter,style.borderCollapse].join('|');"
              "globalThis.pocSummary=actual})()",
              "<compact-visual-cssom-native>",
              &interactive.page.script_result)
          && strcmp(interactive.page.script_result.summary,
                    "ellipsis|hidden|2px|dashed|rgb(18, 52, 86)|-3px|"
                    "inset(7% round 4px)|brightness(0.75)|collapse") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              interactive.page.runtime,
              "document.getElementById('status').style.backgroundImage="
              "'url(/background.png)'",
              "<typed-style-resource-mutation>",
              &interactive.page.script_result)
          && script_runtime_consume_relayout(interactive.page.runtime)
          && navigation_relayout(&interactive)
          && interactive.performance.fast_relayouts == 3
          && interactive.performance.full_relayouts == 1
          && interactive.performance.mutation_resource_rebuilds == 1
          && interactive.performance.resource_fingerprint_scans == 2);
    CHECK(script_runtime_evaluate_diagnostic(
              interactive.page.runtime,
              "const script=document.createElement('script');"
              "script.type='application/json';script.src='/data.json';"
              "document.body.appendChild(script);"
              "const anchor=document.createElement('a');"
              "anchor.href='/next';document.body.appendChild(anchor)",
              "<non-render-resource-mutations>",
              &interactive.page.script_result)
          && script_runtime_consume_relayout(interactive.page.runtime)
          && navigation_relayout(&interactive)
          && interactive.performance.fast_relayouts == 4
          && interactive.performance.full_relayouts == 1
          && interactive.performance.mutation_resource_rebuilds == 1);
    bool adopted_evaluated = script_runtime_evaluate_diagnostic(
        interactive.page.runtime,
        "(()=>{const target=document.createElement('p');"
        "target.className='constructed-native';"
        "target.textContent='ADOPTED';document.body.appendChild(target);"
        "const sheet=new CSSStyleSheet();"
        "sheet.replaceSync('.constructed-native{color:#0a1b2c}');"
        "document.adoptedStyleSheets=[sheet]})()",
        "<constructed-stylesheet-native>",
        &interactive.page.script_result);
    bool adopted_relayout_pending = adopted_evaluated
        && script_runtime_consume_relayout(interactive.page.runtime);
    bool adopted_relayout = adopted_relayout_pending
        && navigation_relayout(&interactive);
    if (!adopted_relayout
        || interactive.performance.fast_relayouts != 4
        || interactive.performance.full_relayouts != 2
        || interactive.performance.mutation_resource_rebuilds != 2
        || interactive.performance.resource_fingerprint_scans != 3) {
        fprintf(stderr,
                "constructed stylesheet native probe: eval=%d pending=%d "
                "relayout=%d fast=%zu full=%zu resource=%zu scans=%zu "
                "error=%s\n",
                adopted_evaluated, adopted_relayout_pending, adopted_relayout,
                interactive.performance.fast_relayouts,
                interactive.performance.full_relayouts,
                interactive.performance.mutation_resource_rebuilds,
                interactive.performance.resource_fingerprint_scans,
                interactive.page.script_result.error);
    }
    CHECK(adopted_relayout
          && interactive.performance.fast_relayouts == 4
          && interactive.performance.full_relayouts == 2
          && interactive.performance.mutation_resource_rebuilds == 2
          && interactive.performance.resource_fingerprint_scans == 3);
    uint32_t adopted_text_color = UINT32_MAX;
    for (size_t command_at = 0;
         command_at < interactive.page.layout.count; command_at++) {
        const DrawCommand *command =
            &interactive.page.layout.commands[command_at];
        if (command->type == DRAW_TEXT && command->text_length == 7
            && memcmp(command->text, "ADOPTED", 7) == 0) {
            adopted_text_color = command->color;
            break;
        }
    }
    CHECK(adopted_text_color == 0x0a1b2c);
    CHECK(script_runtime_evaluate_diagnostic(
              interactive.page.runtime,
              "for(let i=0;i<40;i++)document.getElementById('status')."
              "textContent='JOURNAL '+i",
              "<mutation-journal-coalescing>",
              &interactive.page.script_result));
    CHECK(script_runtime_consume_relayout(interactive.page.runtime));
    CHECK(navigation_relayout(&interactive));
    CHECK(interactive.performance.mutation_journal_overflows == 0);
    CHECK(interactive.performance.mutation_conservative_scans == 0);
    CHECK(interactive.performance.resource_fingerprint_scans == 3);
    bool detached_image_evaluated = script_runtime_evaluate_diagnostic(
              interactive.page.runtime,
              "(()=>{const image=document.createElement('img');"
              "image.setAttribute('alt','DETACHED');image.className='card';"
              "image.setAttribute('loading','lazy');"
              "image.setAttribute('src','/first.png');"
              "image.setAttribute('width','80');"
              "image.setAttribute('height','40');"
              "document.body.appendChild(image)})()",
              "<detached-image-construction>",
              &interactive.page.script_result);
    bool detached_image_pending = detached_image_evaluated
        && script_runtime_consume_relayout(interactive.page.runtime);
    bool detached_image_relayout = detached_image_pending
        && navigation_relayout(&interactive);
    if (!detached_image_relayout
        || interactive.performance.fast_relayouts != 6
        || interactive.performance.full_relayouts != 2
        || interactive.performance.mutation_image_resource_scans != 0
        || interactive.performance.mutation_journal_overflows != 0
        || interactive.performance.resource_fingerprint_scans != 4) {
        fprintf(stderr,
                "detached image probe: eval=%d pending=%d relayout=%d "
                "fast=%zu full=%zu images=%zu overflow=%zu scans=%zu "
                "error=%s\n",
                detached_image_evaluated, detached_image_pending,
                detached_image_relayout,
                interactive.performance.fast_relayouts,
                interactive.performance.full_relayouts,
                interactive.performance.mutation_image_resource_scans,
                interactive.performance.mutation_journal_overflows,
                interactive.performance.resource_fingerprint_scans,
                interactive.page.script_result.error);
    }
    CHECK(detached_image_relayout
          && interactive.performance.fast_relayouts == 6
          && interactive.performance.full_relayouts == 2
          && interactive.performance.mutation_image_resource_scans == 0
          && interactive.performance.mutation_journal_overflows == 0
          && interactive.performance.resource_fingerprint_scans == 4);
    CHECK(script_runtime_evaluate_diagnostic(
              interactive.page.runtime,
              "(()=>{const image=document.querySelector('img.card');"
              "image.setAttribute('alt','CONNECTED');"
              "image.setAttribute('title','metadata');"
              "image.className='card ready'})()",
              "<non-resource-image-attributes>",
              &interactive.page.script_result)
          && script_runtime_consume_relayout(interactive.page.runtime)
          && navigation_relayout(&interactive)
          && interactive.performance.fast_relayouts == 7
          && interactive.performance.full_relayouts == 2
          && interactive.performance.mutation_image_resource_scans == 0
          && interactive.performance.resource_fingerprint_scans == 4);
    CHECK(script_runtime_evaluate_diagnostic(
              interactive.page.runtime,
              "document.querySelector('img.card').setAttribute("
              "'src','/second.png')",
              "<image-source-refresh>",
              &interactive.page.script_result)
          && script_runtime_consume_relayout(interactive.page.runtime)
          && navigation_relayout(&interactive)
          && interactive.performance.fast_relayouts == 8
          && interactive.performance.full_relayouts == 2
          && interactive.performance.mutation_resource_rebuilds == 2
          && interactive.performance.resource_fingerprint_scans == 5);
    CHECK(script_runtime_evaluate_diagnostic(
              interactive.page.runtime,
              "setTimeout(()=>{throw new Error('callback-probe')},0)",
              "https://example.test/callback-probe.js",
              &interactive.page.script_result)
          && navigation_advance_runtime(&interactive, 0, 4)
          && interactive.page.script_result.uncaught_callback_errors == 1
          && strstr(interactive.page.script_result.last_uncaught_callback_error,
                    "callback-probe") != NULL);
    CHECK(script_runtime_evaluate_diagnostic(
              interactive.page.runtime,
              "(()=>{const xhr=new XMLHttpRequest;let later=0;"
              "xhr.onreadystatechange=()=>{throw new Error('xhr-probe')};"
              "xhr.addEventListener('readystatechange',()=>later++);"
              "xhr.open('GET','/probe');globalThis.pocSummary="
              "'xhr-listener:'+later})()",
              "https://example.test/xhr-listener-probe.js",
              &interactive.page.script_result));
    CHECK(strcmp(interactive.page.script_result.summary,
                 "xhr-listener:1") == 0);
    CHECK(interactive.page.script_result.uncaught_callback_errors == 2);
    CHECK(strstr(interactive.page.script_result.last_uncaught_callback_error,
                 "XMLHttpRequest readystatechange") != NULL);

    puts("test: controller focus, editing, and activation");
    BrowserController controller;
    CHECK(controller_init(&controller, &interactive));
    const LayoutNodeBox *pointer_box = layout_box_for_node(
        &interactive.page.layout, runtime_status);
    ControllerAction pointer_action;
    CHECK(pointer_box != NULL
          && controller_focus_at(&controller, pointer_box->x + 1,
                                 pointer_box->y + 1)
          && controller.focus_kind == CONTROLLER_FOCUS_POINTER
          && controller_activate(&controller, &pointer_action)
          && pointer_action.type == CONTROLLER_ACTION_CONTROL
          && attribute_equals(runtime_status, "data-pointer", "yes"));

    lxb_dom_node_t *draw_node = find_id(
        lxb_dom_interface_node(interactive.page.document.html), "draw");
    const LayoutNodeBox *draw_box = layout_box_for_node(
        &interactive.page.layout, draw_node);
    CHECK(draw_box != NULL);
    int draw_x0 = draw_box->x + 4, draw_y0 = draw_box->y + 5;
    int draw_x1 = draw_box->x + 12, draw_y1 = draw_box->y + 15;
    bool pointer_activate = false;
    CHECK(controller_pointer_event(
              &controller, CONTROLLER_POINTER_MOVE,
              draw_x0, draw_y0, &pointer_activate)
          && !pointer_activate
          && controller_pointer_event(
              &controller, CONTROLLER_POINTER_DOWN,
              draw_x0, draw_y0, &pointer_activate)
          && !pointer_activate
          && controller_pointer_event(
              &controller, CONTROLLER_POINTER_MOVE,
              draw_x1, draw_y1, &pointer_activate)
          && !pointer_activate
          && controller_pointer_event(
              &controller, CONTROLLER_POINTER_UP,
              draw_x1, draw_y1, &pointer_activate)
          && pointer_activate);
    ControllerAction draw_action;
    CHECK(controller_activate(&controller, &draw_action)
          && draw_action.type == CONTROLLER_ACTION_CONTROL);
    char expected_draw[1024];
    snprintf(
        expected_draw, sizeof(expected_draw),
        "pointermove:%d,%d:0:4,5|mousemove:%d,%d:0:4,5|"
        "pointerdown:%d,%d:1:4,5|mousedown:%d,%d:1:4,5|focus|"
        "pointermove:%d,%d:1:12,15|mousemove:%d,%d:1:12,15|"
        "pointerup:%d,%d:0:12,15|mouseup:%d,%d:0:12,15|"
        "click:%d,%d:0:12,15",
        draw_x0, draw_y0, draw_x0, draw_y0,
        draw_x0, draw_y0, draw_x0, draw_y0,
        draw_x1, draw_y1, draw_x1, draw_y1,
        draw_x1, draw_y1, draw_x1, draw_y1,
        draw_x1, draw_y1);
    CHECK(attribute_equals(runtime_status, "data-draw", expected_draw));

    lxb_dom_node_t *remove_node = find_id(
        lxb_dom_interface_node(interactive.page.document.html),
        "remove-on-down");
    const LayoutNodeBox *remove_box = layout_box_for_node(
        &interactive.page.layout, remove_node);
    CHECK(remove_box != NULL);
    int remove_x = remove_box->x + 2, remove_y = remove_box->y + 2;
    size_t no_op_move_relayouts = interactive.incremental_relayouts;
    CHECK(controller_pointer_event(
              &controller, CONTROLLER_POINTER_MOVE,
              remove_x, remove_y, &pointer_activate)
          && interactive.incremental_relayouts == no_op_move_relayouts);
    CHECK(controller_pointer_event(
              &controller, CONTROLLER_POINTER_DOWN,
              remove_x, remove_y, &pointer_activate)
          && controller_pointer_event(
              &controller, CONTROLLER_POINTER_UP,
              remove_x, remove_y, &pointer_activate)
          && !pointer_activate);

    const ControlRegion *edit_region = &interactive.page.layout.controls[0];
    int edit_x = edit_region->x + 1, edit_y = edit_region->y + 1;
    CHECK(controller_pointer_event(
              &controller, CONTROLLER_POINTER_DOWN,
              edit_x, edit_y, &pointer_activate)
          && controller_pointer_event(
              &controller, CONTROLLER_POINTER_UP,
              edit_x, edit_y, &pointer_activate)
          && pointer_activate
          && controller_commit_pointer_click(&controller)
          && attribute_equals(
              runtime_status, "data-edit-activation",
              "pointerdown|mousedown|focus|pointerup|mouseup|click"));
    edit_region = &interactive.page.layout.controls[0];
    CHECK(controller_focus_at(&controller, edit_region->x, edit_region->y)
          && controller_insert_text(&controller, "BC", 2));
    lxb_dom_node_t *edit_node = find_id(
        lxb_dom_interface_node(interactive.page.document.html), "edit");
    size_t edit_value_length = 0;
    const char *edit_value = document_attribute(edit_node, "value",
                                                &edit_value_length);
    CHECK(edit_value != NULL && edit_value_length == 3
          && memcmp(edit_value, "ABC", 3) == 0
          && attribute_equals(runtime_status, "data-before",
                              "InputEvent:insertText:BC:A:true")
          && attribute_equals(runtime_status, "data-input",
                              "InputEvent:insertText:BC:ABC:true")
          && controller_backspace(&controller));
    edit_value = document_attribute(edit_node, "value", &edit_value_length);
    CHECK(edit_value != NULL && edit_value_length == 2
          && memcmp(edit_value, "AB", 2) == 0
          && attribute_equals(runtime_status, "data-before",
                              "InputEvent:deleteContentBackward:null:ABC:true")
          && attribute_equals(runtime_status, "data-input",
                              "InputEvent:deleteContentBackward:null:AB:true"));
    CHECK(controller_insert_text(&controller, "X", 1));
    edit_value = document_attribute(edit_node, "value", &edit_value_length);
    CHECK(edit_value != NULL && edit_value_length == 2
          && memcmp(edit_value, "AB", 2) == 0
          && attribute_equals(runtime_status, "data-before",
                              "InputEvent:insertText:X:AB:true")
          && attribute_equals(runtime_status, "data-input",
                              "InputEvent:deleteContentBackward:null:AB:true"));
    char controller_value[8] = {0};
    size_t controller_value_length = 0;
    CHECK(controller_text_value(
              &controller, controller_value, sizeof(controller_value),
              &controller_value_length)
          && controller_value_length == 2
          && strcmp(controller_value, "AB") == 0
          && controller_replace_text(&controller, "AB", 2));
    ControllerTextInputInfo text_input_info = {0};
    CHECK(controller_text_input_info(&controller, &text_input_info)
          && text_input_info.editable
          && text_input_info.voice_allowed
          && !text_input_info.keyboard_url_mode
          && !text_input_info.multiline);
    const ControlRegion *button_region = &interactive.page.layout.controls[1];
    ControllerAction controller_action;
    CHECK(controller_focus_at(&controller, button_region->x,
                              button_region->y));
    size_t activation_relayouts = interactive.incremental_relayouts;
    CHECK(controller_activate(&controller, &controller_action));
    CHECK(controller_action.type == CONTROLLER_ACTION_FORM_SUBMIT
          && strcmp(controller_action.method, "POST") == 0
          && strcmp(controller_action.url,
                    "https://example.test/submit") == 0
          && strcmp(controller_action.body, "q=AB&send=Go") == 0
          && attribute_equals(runtime_status, "data-submit",
                              "SubmitEvent:true:true")
          && attribute_equals(
              runtime_status, "data-activation",
              "pointerdown:PointerEvent:true|mousedown:MouseEvent:true|"
              "pointerup:PointerEvent:true|mouseup:MouseEvent:true|"
              "click:MouseEvent:true"));
    /* One relayout for the five-event activation transaction and one for the
       separately cancelable submit default action. */
    CHECK(interactive.incremental_relayouts == activation_relayouts + 2
          && command_x(&interactive.page.layout, "CLICKED") >= 0);
    lxb_dom_node_t *next_node = interactive.page.layout.links[0].node;
    CHECK(controller_focus_node(&controller, next_node));
    ScriptResult focus_relayout_result = {0};
    CHECK(script_runtime_evaluate_diagnostic(
              interactive.page.runtime,
              "(()=>{const link=document.createElement('a');"
              "link.href='/inserted';link.textContent='INSERTED';"
              "document.body.prepend(link)})()",
              "https://example.test/focus-relayout.js",
              &focus_relayout_result)
          && navigation_relayout(&interactive)
          && interactive.page.layout.links[0].node != next_node
          && controller_activate(&controller, &controller_action)
          && controller_action.type == CONTROLLER_ACTION_NAVIGATE
          && strcmp(controller_action.url,
                    "https://example.test/next") == 0);
    const ControlRegion *editable_region = &interactive.page.layout.controls[2];
    CHECK(editable_region->type == CONTROL_EDITABLE
          && controller_focus_at(&controller, editable_region->x,
                                 editable_region->y)
          && controller_insert_text(&controller, "What can you do?", 16));
    lxb_dom_node_t *prompt_node = find_id(
        lxb_dom_interface_node(interactive.page.document.html), "prompt");
    size_t prompt_length = 0;
    lxb_char_t *prompt_text = lxb_dom_node_text_content(prompt_node,
                                                        &prompt_length);
    size_t prompt_state_length = 0;
    const char *prompt_state = document_attribute(
        runtime_status, "data-prompt", &prompt_state_length);
    CHECK(prompt_text != NULL && prompt_length == 16
          && memcmp(prompt_text, "What can you do?", 16) == 0
          && prompt_state != NULL && prompt_state_length == 3
          && memcmp(prompt_state, "yes", 3) == 0);
    lxb_dom_document_destroy_text(prompt_node->owner_document, prompt_text);
    const ControlRegion *search_region = &interactive.page.layout.controls[3];
    CHECK(search_region->type == CONTROL_INPUT);
    CHECK(controller_focus_at(&controller, search_region->x,
                              search_region->y));
    CHECK(controller_activate(&controller, &controller_action));
    CHECK(controller_action.type == CONTROLLER_ACTION_FORM_SUBMIT);
    CHECK(strcmp(controller_action.method, "GET") == 0);
    CHECK(strcmp(controller_action.url,
                 "https://example.test/search/?q=&q=PSP") == 0);
    CHECK(controller_action.body_length == 0);
    CHECK(attribute_equals(runtime_status, "data-search-submit",
                           "SubmitEvent:true:true"));
    const ControlRegion *password_region =
        &interactive.page.layout.controls[4];
    CHECK(controller_focus_at(&controller, password_region->x,
                              password_region->y)
          && controller_text_input_info(&controller, &text_input_info)
          && text_input_info.editable
          && !text_input_info.voice_allowed);
    const ControlRegion *otp_region = &interactive.page.layout.controls[5];
    CHECK(controller_focus_at(&controller, otp_region->x, otp_region->y)
          && controller_text_input_info(&controller, &text_input_info)
          && text_input_info.editable
          && !text_input_info.voice_allowed);

    puts("test: ordered external script evaluation and load event");
    lxb_dom_node_t *external_node = find_id(
        lxb_dom_interface_node(interactive.page.document.html), "external");
    static const char external_source[] =
        "document.getElementById('status').textContent='EXTERNAL';";
    CHECK(external_node != NULL
          && navigation_evaluate_external_script(
              &interactive, external_node, external_source,
              strlen(external_source),
              "https://example.test/external.js")
          && interactive.page.script_result.external_scripts_loaded == 1
          && interactive.page.script_result.external_script_bytes
             == strlen(external_source)
          && command_x(&interactive.page.layout, "EXTERNAL") >= 0);
    size_t external_state_length = 0;
    const char *external_state = document_attribute(
        runtime_status, "data-external", &external_state_length);
    CHECK(external_state != NULL && external_state_length == 6
          && memcmp(external_state, "loaded", 6) == 0);

    puts("test: controller and JavaScript viewport scrolling");
    static const char scroll_page[] =
        "<!doctype html><meta name=viewport content='width=device-width'>"
        "<title>Scroll</title><style>"
        "#space{display:block;height:1200px}#fixed-focus{position:fixed;"
        "left:17px;top:19px;width:90px;height:30px}#nested{height:40px;"
        "overflow:auto}#nested-space{height:200px}</style><body>"
        "<button id=bottom type=button>Bottom</button>"
        "<button id=fixed-focus type=button>Fixed focus</button>"
        "<div id=nested><div id=nested-space>nested</div></div><div id=space></div>"
        "<p><a id=end href=/end>END OF PAGE</a></p><script>"
        "const scrollOrder=[];globalThis.pocSummary='scroll:pending';"
        "const recordScroll=name=>{scrollOrder.push(name+':'+scrollY);"
        "globalThis.pocSummary=scrollOrder.join(',');};"
        "addEventListener('scroll',()=>recordScroll('window'));"
        "const recordViewport=()=>recordScroll('viewport');"
        "visualViewport.addEventListener('scroll',recordViewport);"
        "document.getElementById('nested').addEventListener('scroll',"
        "()=>recordScroll('nested'));"
        "document.getElementById('bottom').addEventListener('click',"
        "()=>{const end=document.getElementById('end'),before=Math.round("
        "end.getBoundingClientRect().top);visualViewport.removeEventListener("
        "'scroll',recordViewport);visualViewport.addEventListener('scroll',"
        "()=>{scrollOrder.push('viewport-error:'+scrollY);throw new Error("
        "'expected viewport listener failure')});visualViewport."
        "addEventListener('scroll',recordViewport);scrollTo(0,32);"
        "scrollTo(0,10000);const after=Math.round(end."
        "getBoundingClientRect().top);globalThis.pocSummary=['sync',scrollY,"
        "visualViewport.pageTop,before-after].join(':');});</script></body>";
    uint64_t scroll_generation = navigation_begin(&interactive);
    CHECK(navigation_commit_html(
              &interactive, scroll_generation,
              "https://example.test/scroll", scroll_page,
              strlen(scroll_page), 480, NULL, NULL, true)
          && controller_init(&controller, &interactive)
          && interactive.page.layout.height > 272);
    int scroll_maximum = interactive.page.layout.height - 272;
    char synchronous_scroll_summary[96];
    snprintf(synchronous_scroll_summary, sizeof(synchronous_scroll_summary),
             "sync:%d:%d:%d", scroll_maximum, scroll_maximum,
             scroll_maximum);
    char dispatched_scroll_summary[256];
    snprintf(dispatched_scroll_summary, sizeof(dispatched_scroll_summary),
             "window:128,viewport:128,window:%d,viewport-error:%d,viewport:%d",
             scroll_maximum, scroll_maximum, scroll_maximum);
    lxb_dom_node_t *nested_scroll_node = find_id(
        lxb_dom_interface_node(interactive.page.document.html), "nested");
    const LayoutNodeBox *nested_scroll_box = layout_box_for_node(
        &interactive.page.layout, nested_scroll_node);
    size_t scroll_compile_attempts =
        interactive.page.script_result.host_compile_attempts;
    CHECK(nested_scroll_box != NULL && nested_scroll_box->scroll_y == 0
          && controller_scroll_by(&controller, 64, 272)
          && controller_scroll_by(&controller, 64, 272)
          && navigation_current(&interactive)->scroll_y == 128
          && nested_scroll_box->scroll_y == 0
          && interactive.page.script_result.host_compile_attempts
             == scroll_compile_attempts
          && strcmp(interactive.page.script_result.summary,
                    "scroll:pending") == 0
          && navigation_advance_runtime(&interactive, 0, 0)
          && strcmp(interactive.page.script_result.summary,
                    "window:128,viewport:128") == 0
          && nested_scroll_box->scroll_y == 0
          && interactive.page.script_result.host_compile_attempts
             == scroll_compile_attempts
          && controller_scroll_to_bottom(&controller, 272)
          && navigation_current(&interactive)->scroll_y
             == interactive.page.layout.height - 272
          && controller_scroll_by(&controller, -100000, 272)
          && navigation_current(&interactive)->scroll_y == 0
          && navigation_dispatch_event(&interactive, "#bottom", "click")
          && strcmp(interactive.page.script_result.summary,
                    synchronous_scroll_summary) == 0
          && navigation_current(&interactive)->scroll_y
             == scroll_maximum
          && navigation_advance_runtime(&interactive, 0, 0)
          && strcmp(interactive.page.script_result.summary,
                    dispatched_scroll_summary) == 0
          && interactive.page.script_result.uncaught_callback_errors == 1
          && strstr(interactive.page.script_result.last_uncaught_callback_error,
                    "visualViewport scroll") != NULL
          && strstr(interactive.page.script_result.last_uncaught_callback_error,
                    "expected viewport listener failure") != NULL);
    CHECK(controller_scroll_to_top(&controller, 272)
          && controller_scroll_step(&controller, 1, 0, 272)
          && navigation_current(&interactive)->scroll_y == 8
          && controller_scroll_step(&controller, 1, 60, 272)
          && navigation_current(&interactive)->scroll_y == 56
          && controller_scroll_page(&controller, 1, 272)
          && navigation_current(&interactive)->scroll_y == 294
          && controller_scroll_page(&controller, -1, 272)
          && navigation_current(&interactive)->scroll_y == 56);
    lxb_dom_node_t *fixed_focus_node = find_id(
        lxb_dom_interface_node(interactive.page.document.html),
        "fixed-focus");
    const ControlRegion *fixed_focus_region = NULL;
    int fixed_focus_x = 0;
    for (size_t i = 0; i < interactive.page.layout.control_count; i++) {
        if (interactive.page.layout.controls[i].node == fixed_focus_node) {
            fixed_focus_region = &interactive.page.layout.controls[i];
            fixed_focus_x = fixed_focus_region->x;
        }
    }
    CHECK(fixed_focus_node != NULL && fixed_focus_region != NULL
          && controller_focus_node(&controller, fixed_focus_node)
          && navigation_current(&interactive)->scroll_y == 56);
    int fixed_focus_y = 0;
    CHECK(controller_focused_rect(&controller, NULL, &fixed_focus_y,
                                  NULL, NULL)
          && fixed_focus_y == 56 + 19
          && controller_focus_at(&controller, fixed_focus_x + 1, 20)
          && navigation_current(&interactive)->scroll_y == 56);
    CHECK(script_runtime_evaluate_diagnostic(
              interactive.page.runtime,
              "globalThis.pocSummary=String(Math.round(document."
              "getElementById('fixed-focus').getBoundingClientRect().top))",
              "https://example.test/fixed-geometry.js",
              &interactive.page.script_result)
          && strcmp(interactive.page.script_result.summary, "19") == 0);
    CHECK(controller_scroll_to_bottom(&controller, 272));
    int tall_page_scroll = navigation_current(&interactive)->scroll_y;
    CHECK(script_runtime_evaluate_diagnostic(
              interactive.page.runtime,
              "document.getElementById('space').style.height='8px'",
              "https://example.test/shrink-scroll.js",
              &interactive.page.script_result)
          && script_runtime_consume_relayout(interactive.page.runtime)
          && navigation_relayout(&interactive));
    int shrunken_maximum = viewport_max_scroll_css(
        &interactive.viewport, interactive.page.layout.height);
    CHECK(tall_page_scroll > shrunken_maximum
          && navigation_current(&interactive)->scroll_y == shrunken_maximum);
    CHECK(script_runtime_evaluate_diagnostic(
              interactive.page.runtime,
              "document.getElementById('space').style.height='1200px'",
              "https://example.test/restore-scroll.js",
              &interactive.page.script_result)
          && script_runtime_consume_relayout(interactive.page.runtime)
          && navigation_relayout(&interactive));
    lxb_dom_node_t *end_node = find_id(
        lxb_dom_interface_node(interactive.page.document.html), "end");
    CHECK(controller_scroll_to_top(&controller, 272)
          && controller_focus_node(&controller, end_node)
          && navigation_current(&interactive)->scroll_y > 0);
    int focused_y = 0, focused_height = 0;
    CHECK(controller_focused_rect(&controller, NULL, &focused_y, NULL,
                                  &focused_height));
    int focused_scroll = navigation_current(&interactive)->scroll_y;
    CHECK(focused_y >= focused_scroll
          && focused_y + focused_height
             <= focused_scroll + controller.viewport_height);
    CHECK(controller_focus_at(&controller,
                              interactive.page.layout.links[0].x,
                              focused_y - focused_scroll));

    puts("test: pointer coordinates follow a scaled CSS viewport");
    static const char scaled_pointer_page[] =
        "<!doctype html><meta name=viewport content='width=320'>"
        "<style>#scaled{width:100px;height:50px}</style><body>"
        "<div id=scaled></div><p id=scaled-result></p><script>"
        "document.getElementById('scaled').addEventListener("
        "'pointermove',event=>"
        "document.getElementById('scaled-result').setAttribute('data-point',"
        "[event.clientX,event.clientY,event.offsetX,event.offsetY,"
        "event.buttons].join(',')))</script>";
    uint64_t scaled_pointer_generation = navigation_begin(&interactive);
    CHECK(navigation_commit_html(
              &interactive, scaled_pointer_generation,
              "https://example.test/scaled-pointer", scaled_pointer_page,
              strlen(scaled_pointer_page), 480, NULL, NULL, true)
          && interactive.viewport.css_width == 320
          && interactive.viewport.device_width == 480
          && controller_init(&controller, &interactive));
    lxb_dom_node_t *scaled_node = find_id(
        lxb_dom_interface_node(interactive.page.document.html), "scaled");
    lxb_dom_node_t *scaled_result = find_id(
        lxb_dom_interface_node(interactive.page.document.html),
        "scaled-result");
    const LayoutNodeBox *scaled_box = layout_box_for_node(
        &interactive.page.layout, scaled_node);
    CHECK(scaled_box != NULL);
    int scaled_box_x = scaled_box->x, scaled_box_y = scaled_box->y;
    int scaled_device_x = viewport_css_to_device(
        &interactive.viewport, scaled_box_x + 15);
    int scaled_device_y = viewport_css_to_device(
        &interactive.viewport, scaled_box_y + 10);
    int scaled_client_x = viewport_device_to_css(
        &interactive.viewport, scaled_device_x);
    int scaled_client_y = viewport_device_to_css(
        &interactive.viewport, scaled_device_y);
    CHECK(controller_pointer_event(
        &controller, CONTROLLER_POINTER_MOVE,
        scaled_device_x, scaled_device_y, &pointer_activate));
    char scaled_expected[64];
    snprintf(
        scaled_expected, sizeof(scaled_expected), "%d,%d,%d,%d,0",
        scaled_client_x, scaled_client_y,
        scaled_client_x - scaled_box_x,
        scaled_client_y - scaled_box_y);
    size_t scaled_actual_length = 0;
    const char *scaled_actual = document_attribute(
        scaled_result, "data-point", &scaled_actual_length);
    if (scaled_actual == NULL
        || strlen(scaled_expected) != scaled_actual_length
        || memcmp(scaled_actual, scaled_expected, scaled_actual_length) != 0) {
        fprintf(
            stderr, "scaled pointer expected=%s actual=%.*s error=%s\n",
            scaled_expected, (int) scaled_actual_length,
            scaled_actual == NULL ? "" : scaled_actual,
            interactive.page.script_result.error);
    }
    CHECK(attribute_equals(
        scaled_result, "data-point", scaled_expected));

    puts("test: bounded session persistence and cache");
    static const unsigned char cached_script[] = "cached-resource";
    CHECK(browser_session_cache_put(
              &browser_session, "https://example.test/external.js",
              cached_script, sizeof(cached_script) - 1));
    const unsigned char *cached_result = NULL;
    size_t cached_result_length = 0;
    CHECK(browser_session_cache_get(
              &browser_session, "https://example.test/external.js",
              &cached_result, &cached_result_length)
          && cached_result_length == sizeof(cached_script) - 1
          && memcmp(cached_result, cached_script,
                    cached_result_length) == 0);
    static const char persisted_page[] =
        "<!doctype html><title>Persisted</title><body>"
        "<p id=persist>MISS</p><script>"
        "if(localStorage.getItem('theme')==='psp'&&document.cookie.includes('mode=reader'))document.getElementById('persist').textContent='PERSISTED';"
        "</script></body>";
    uint64_t persisted_generation = navigation_begin(&interactive);
    CHECK(navigation_commit_html(
              &interactive, persisted_generation,
              "https://example.test/second", persisted_page,
              strlen(persisted_page), 480, NULL, NULL, true)
          && command_x(&interactive.page.layout, "PERSISTED") >= 0);
    navigation_destroy(&interactive);
    browser_session_destroy(&browser_session);
    CHECK(budget.current == 0);

    puts("test: inline SVG mutations refresh retained raster resources");
    static const char svg_refresh_page[] =
        "<!doctype html><style>html,body{margin:0}#icon{color:#ff0000}"
        "</style><body><svg id=icon width=8 height=8 viewBox='0 0 8 8' "
        "xmlns='http://www.w3.org/2000/svg'><path fill='currentColor' "
        "d='M0 0h8v8H0z'/></svg></body>";
    NavigationSession svg_refresh_navigation;
    CHECK(navigation_init(&svg_refresh_navigation, &budget, 2));
    navigation_enable_scripts(&svg_refresh_navigation, 3 * MIB, 1000);
    navigation_enable_external_resources(
        &svg_refresh_navigation, 2, 32 * 1024, 32 * 1024,
        4, 64 * 1024, 32 * 1024, 64 * 1024, 1000);
    uint64_t svg_refresh_generation =
        navigation_begin(&svg_refresh_navigation);
    CHECK(navigation_commit_html(
        &svg_refresh_navigation, svg_refresh_generation,
        "https://svg-refresh.test/", svg_refresh_page,
        strlen(svg_refresh_page), 480, NULL, NULL, true));
    lxb_dom_node_t *svg_refresh_icon = find_id(
        lxb_dom_interface_node(svg_refresh_navigation.page.document.html),
        "icon");
    const ImageResource *svg_refresh_before = images_find_node(
        &svg_refresh_navigation.page.images, svg_refresh_icon);
    CHECK(svg_refresh_before != NULL && svg_refresh_before->pixels != NULL
          && svg_refresh_before->pixels[0] > 240
          && svg_refresh_before->pixels[1] < 16
          && svg_refresh_before->pixels[2] < 16);
    CHECK(script_runtime_evaluate_diagnostic(
              svg_refresh_navigation.page.runtime,
              "document.querySelector('#icon path')."
              "setAttribute('fill','#0000ff')",
              "<inline-svg-resource-refresh>",
              &svg_refresh_navigation.page.script_result)
          && script_runtime_consume_relayout(
              svg_refresh_navigation.page.runtime)
          && navigation_relayout(&svg_refresh_navigation)
          && svg_refresh_navigation.performance
                 .mutation_image_resource_scans == 1);
    const ImageResource *svg_refresh_after = images_find_node(
        &svg_refresh_navigation.page.images, svg_refresh_icon);
    if (svg_refresh_after == NULL || svg_refresh_after->pixels == NULL
        || svg_refresh_after->pixels[0] >= 16
        || svg_refresh_after->pixels[1] >= 16
        || svg_refresh_after->pixels[2] <= 240) {
        fprintf(stderr,
                "inline SVG refresh before=%p after=%p rgba=%u,%u,%u,%u "
                "loaded=%zu failed=%zu error=%s\n",
                (void *) svg_refresh_before, (void *) svg_refresh_after,
                svg_refresh_after == NULL || svg_refresh_after->pixels == NULL
                    ? 0u : svg_refresh_after->pixels[0],
                svg_refresh_after == NULL || svg_refresh_after->pixels == NULL
                    ? 0u : svg_refresh_after->pixels[1],
                svg_refresh_after == NULL || svg_refresh_after->pixels == NULL
                    ? 0u : svg_refresh_after->pixels[2],
                svg_refresh_after == NULL || svg_refresh_after->pixels == NULL
                    ? 0u : svg_refresh_after->pixels[3],
                svg_refresh_navigation.page.images.stats.loaded,
                svg_refresh_navigation.page.images.stats.failed,
                svg_refresh_navigation.last_error);
    }
    CHECK(svg_refresh_after != NULL && svg_refresh_after->pixels != NULL
          && svg_refresh_after->pixels[0] < 16
          && svg_refresh_after->pixels[1] < 16
          && svg_refresh_after->pixels[2] > 240);
    navigation_destroy(&svg_refresh_navigation);
    CHECK(budget.current == 0);

    puts("test: revealing a hidden inline background discovers its image");
    static const char revealed_background_page[] =
        "<!doctype html><body><div id=hero style=\"display:none;opacity:0;"
        "background-image:url(data:image/png;base64,"
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk"
        "+A8AAQUBAScY42YAAAAASUVORK5CYII=)\"></div></body>";
    NavigationSession revealed_background_navigation;
    CHECK(navigation_init(&revealed_background_navigation, &budget, 2));
    navigation_enable_scripts(
        &revealed_background_navigation, 3 * MIB, 1000);
    navigation_enable_external_resources(
        &revealed_background_navigation, 2, 32 * 1024, 32 * 1024,
        4, 64 * 1024, 32 * 1024, 64 * 1024, 1000);
    uint64_t revealed_background_generation = navigation_begin(
        &revealed_background_navigation);
    CHECK(navigation_commit_html(
              &revealed_background_navigation,
              revealed_background_generation,
              "https://revealed-background.test/",
              revealed_background_page, strlen(revealed_background_page),
              480, NULL, NULL, true)
          && revealed_background_navigation.page.images.count == 0
          && script_runtime_evaluate_diagnostic(
              revealed_background_navigation.page.runtime,
              "const hero=document.getElementById('hero');"
              "hero.style.display='block';hero.style.opacity='1'",
              "<reveal-inline-background>",
              &revealed_background_navigation.page.script_result)
          && script_runtime_consume_relayout(
              revealed_background_navigation.page.runtime)
          && navigation_relayout(&revealed_background_navigation)
          && revealed_background_navigation.performance
                 .mutation_image_resource_scans == 1
          && revealed_background_navigation.page.images.count == 1
          && revealed_background_navigation.page.images.stats.loaded == 1
          && revealed_background_navigation.page.images.stats
                 .backgrounds_loaded == 1);
    navigation_destroy(&revealed_background_navigation);
    CHECK(budget.current == 0);

    puts("test: cooperative message channel, idle callback, and viewport");
    static const char cooperative_scheduler_page[] =
        "<!doctype html><meta name=viewport content='width=device-width'>"
        "<body><p id=result>WAIT</p><script>"
        "const channel=new MessageChannel,source={answer:42,nested:{ok:true}};"
        "let messageOk=false,idleOk=false;const finish=()=>{if(messageOk&&idleOk){"
        "document.getElementById('result').textContent='SCHEDULED';"
        "globalThis.pocSummary='scheduler:ok';channel.port1.close();"
        "channel.port2.close();}};channel.port1.onmessage=event=>{"
        "messageOk=event.isTrusted&&event.data.answer===42&&event.data.nested.ok;"
        "finish();};channel.port2.postMessage(source);source.answer=0;"
        "source.nested.ok=false;requestIdleCallback(deadline=>{idleOk="
        "typeof deadline.timeRemaining==='function'&&deadline.timeRemaining()>=0"
        "&&!deadline.didTimeout&&visualViewport.width===480"
        "&&visualViewport.height===272&&visualViewport.scale===1;finish();});"
        "</script></body>";
    NavigationSession cooperative_scheduler;
    CHECK(navigation_init(&cooperative_scheduler, &budget, 2));
    navigation_enable_scripts(&cooperative_scheduler, 3 * MIB, 1000);
    uint64_t cooperative_generation = navigation_begin(
        &cooperative_scheduler);
    CHECK(navigation_commit_html(
              &cooperative_scheduler, cooperative_generation,
              "https://scheduler.test/", cooperative_scheduler_page,
              strlen(cooperative_scheduler_page), 480, NULL, NULL, true)
          && cooperative_scheduler.page.script_result.pending_tasks == 2
          && navigation_advance_runtime(&cooperative_scheduler, 1, 4)
          && cooperative_scheduler.page.script_result.pending_tasks == 0
          && cooperative_scheduler.page.script_result.timer_callbacks_run == 2
          && strcmp(cooperative_scheduler.page.script_result.summary,
                    "scheduler:ok") == 0
          && command_x(&cooperative_scheduler.page.layout, "SCHEDULED") >= 0);
    navigation_destroy(&cooperative_scheduler);
    CHECK(budget.current == 0);

    puts("test: bounded element internals and broadcast channels");
    static const char modern_component_api_page[] =
        "<!doctype html><form id=form><label for=field>Answer</label>"
        "<x-field id=field name=answer></x-field></form><p id=result>WAIT</p>"
        "<script>class Field extends HTMLElement{static formAssociated=true;"
        "constructor(){super();this.i=this.attachInternals();"
        "this.i.setFormValue('42');this.i.role='textbox';"
        "this.i.states.add('--ready');this.disabledHistory=[];}"
        "formAssociatedCallback(form){"
        "this.associated=form===document.getElementById('form');}"
        "formDisabledCallback(disabled){this.disabledHistory.push(disabled);}"
        "formResetCallback(){this.resetSeen=true;}}"
        "customElements.define('x-field',Field);const field="
        "document.getElementById('field'),form=document.getElementById('form');"
        "let duplicate=false,ordinary=false;try{field.attachInternals();}"
        "catch(e){duplicate=e.name==='NotSupportedError';}"
        "try{document.createElement('div').attachInternals();}"
        "catch(e){ordinary=e.name==='NotSupportedError';}"
        "field.i.setValidity({valueMissing:true},'Required');"
        "const invalid=!form.checkValidity()&&field.i.validationMessage==='Required';"
        "field.i.setValidity({});field.setAttribute('disabled','');"
        "field.removeAttribute('disabled');form.reset();"
        "const data=new FormData(form),"
        "source=new BroadcastChannel('component-api'),"
        "target=new BroadcastChannel('component-api'),payload={value:7};"
        "target.onmessage=event=>{const ok=event.isTrusted&&event.data.value===7"
        "&&field.i.form===form&&field.i.labels.length===1&&field.associated"
        "&&field.i.states.has('--ready')&&field.i.role==='textbox'"
        "&&!field.hasAttribute('role')&&duplicate&&ordinary&&invalid"
        "&&field.disabledHistory.join(',')==='true,false'&&field.resetSeen"
        "&&form.checkValidity()&&data.get('answer')==='42'"
        "&&form.elements.includes(field);document.getElementById('result')."
        "textContent=ok?'MODERN-API':'FAILED';globalThis.pocSummary="
        "ok?'modern-api:ok':'modern-api:failed';source.close();target.close();};"
        "source.postMessage(payload);payload.value=0;</script>";
    NavigationSession modern_component_api;
    CHECK(navigation_init(&modern_component_api, &budget, 2));
    navigation_enable_scripts(&modern_component_api, 3 * MIB, 1000);
    uint64_t modern_component_generation = navigation_begin(
        &modern_component_api);
    CHECK(navigation_commit_html(
              &modern_component_api, modern_component_generation,
              "https://component-api.test/", modern_component_api_page,
              strlen(modern_component_api_page), 480, NULL, NULL, true)
          && navigation_advance_runtime(&modern_component_api, 1, 4)
          && strcmp(modern_component_api.page.script_result.summary,
                    "modern-api:ok") == 0
          && command_x(&modern_component_api.page.layout, "MODERN-API") >= 0);
    navigation_destroy(&modern_component_api);
    CHECK(budget.current == 0);

    puts("test: bounded document and window event propagation");
    static const char event_propagation_page[] =
        "<!doctype html><body><div id=outer tabindex=-1><button id=target>GO</button></div>"
        "<input id=field value=abcdef>"
        "<p id=result>WAIT</p><script>"
        "const order=[],target=document.getElementById('target'),outer="
        "document.getElementById('outer'),controller=new AbortController;"
        "addEventListener('tilefinch',event=>order.push('window-capture:'"
        "+event.eventPhase),true);document.addEventListener('tilefinch',event=>"
        "order.push('document-capture:'+event.eventPhase),true);"
        "outer.addEventListener('tilefinch',event=>order.push('outer-capture:'"
        "+event.eventPhase),true);target.addEventListener('tilefinch',event=>{"
        "order.push('target:'+event.eventPhase);globalThis.pathOk="
        "event.composedPath()[0]===target&&event.composedPath().at(-2)==="
        "document&&event.composedPath().at(-1)===window;});"
        "outer.addEventListener('tilefinch',event=>order.push('outer-bubble:'"
        "+event.eventPhase));document.addEventListener('tilefinch',event=>"
        "order.push('document-bubble:'+event.eventPhase),{once:true});"
        "addEventListener('tilefinch',event=>order.push('window-bubble:'"
        "+event.eventPhase));document.addEventListener('tilefinch',()=>"
        "order.push('aborted'),{signal:controller.signal});controller.abort();"
        "let targetAfterStop=false,ancestorAfterStop=false;target.addEventListener("
        "'halt',event=>event.stopPropagation(),true);target.addEventListener("
        "'halt',()=>targetAfterStop=true);outer.addEventListener('halt',()=>"
        "ancestorAfterStop=true);target.dispatchEvent(new Event('halt',"
        "{bubbles:true}));"
        "target.dispatchEvent(new Event('tilefinch',{bubbles:true,cancelable:true}));"
        "target.dispatchEvent(new Event('tilefinch',{bubbles:true}));"
        "const standalone=new EventTarget,standaloneOrder=[];standalone."
        "addEventListener('probe',event=>standaloneOrder.push(event.type),"
        "{once:true});standalone.dispatchEvent(new CustomEvent('probe',"
        "{detail:7}));standalone.dispatchEvent(new Event('probe'));"
        "const inputEvent=new InputEvent('beforeinput',{data:'x',inputType:"
        "'insertText',bubbles:true,cancelable:true}),keyboardEvent=new "
        "KeyboardEvent('keydown',{key:'Enter',code:'Enter',keyCode:13}),"
        "pointerEvent=new PointerEvent('pointerdown',{pointerId:7,pointerType:"
        "'pen',pressure:.5,clientX:9}),mouseEvent=new MouseEvent('click',"
        "{clientX:4,button:0}),focusEvent=new FocusEvent('focusin',"
        "{relatedTarget:outer});const constructorsOk=inputEvent instanceof "
        "UIEvent&&inputEvent instanceof Event&&inputEvent.data==='x'&&"
        "inputEvent.inputType==='insertText'&&keyboardEvent instanceof "
        "KeyboardEvent&&keyboardEvent.key==='Enter'&&keyboardEvent.code==="
        "'Enter'&&keyboardEvent.keyCode===13&&pointerEvent instanceof "
        "MouseEvent&&pointerEvent.pointerId===7&&pointerEvent.pointerType==="
        "'pen'&&pointerEvent.pressure===.5&&pointerEvent.clientX===9&&"
        "mouseEvent.clientX===4&&focusEvent.relatedTarget===outer&&"
        "Event.AT_TARGET===2&&KeyboardEvent.DOM_KEY_LOCATION_NUMPAD===3&&"
        "standaloneOrder.join(',')==='probe';"
        "const focusOrder=[],field=document.getElementById('field');target."
        "addEventListener('focus',event=>focusOrder.push('target-focus:'+"
        "(event.relatedTarget===document.body)+':'+event.isTrusted));target.addEventListener("
        "'blur',event=>focusOrder.push('target-blur:'+(event.relatedTarget==="
        "outer)));outer.addEventListener('focusin',event=>focusOrder.push("
        "'in:'+event.target.id));outer.addEventListener('focusout',event=>"
        "focusOrder.push('out:'+event.target.id));target.focus();outer.focus();"
        "const focusOk=document.activeElement===outer&&focusOrder.join('|')==="
        "'target-focus:true:true|in:target|target-blur:true|out:target|in:outer';"
        "let selectionChanges=0;document.addEventListener('selectionchange',"
        "()=>selectionChanges++);const range=document.createRange(),textNode="
        "target.firstChild;range.setStart(textNode,0);range.setEnd(textNode,2);"
        "const selection=getSelection();selection.removeAllRanges();selection."
        "addRange(range);field.setSelectionRange(2,5,'backward');const "
        "selectionOk=range instanceof Range&&range.toString()==='GO'&&"
        "selection.rangeCount===1&&selection.anchorNode===textNode&&"
        "selection.focusOffset===2&&selection.type==='Range'&&field."
        "selectionStart===2&&field.selectionEnd===5&&field."
        "selectionDirection==='backward'&&selectionChanges===3;"
        "const expected=['window-capture:1','document-capture:1',"
        "'outer-capture:1','target:2','outer-bubble:3','document-bubble:3',"
        "'window-bubble:3','window-capture:1','document-capture:1',"
        "'outer-capture:1','target:2','outer-bubble:3','window-bubble:3'];"
        "globalThis.pocSummary=pathOk&&constructorsOk&&focusOk&&selectionOk&&"
        "targetAfterStop&&"
        "!ancestorAfterStop&&"
        "order.join('|')===expected.join('|')"
        "?'events:ok':'events:flags='+[pathOk,constructorsOk,focusOk,"
        "selectionOk,targetAfterStop,!ancestorAfterStop,"
        "order.join('|')===expected.join('|')].join(',')+':focus=' + "
        "focusOrder.join('|')+':active='+(document.activeElement?.id||"
        "document.activeElement?.tagName||'none')+':'+"
        "order.join('|');document.getElementById('result')"
        ".textContent=pocSummary;</script></body>";
    NavigationSession event_propagation;
    CHECK(navigation_init(&event_propagation, &budget, 2));
    navigation_enable_scripts(&event_propagation, 3 * MIB, 1000);
    uint64_t event_generation = navigation_begin(&event_propagation);
    bool event_committed = navigation_commit_html(
        &event_propagation, event_generation,
        "https://events.test/", event_propagation_page,
        strlen(event_propagation_page), 480, NULL, NULL, true);
    if (!event_committed
        || strcmp(event_propagation.page.script_result.summary,
                  "events:ok") != 0) {
        fprintf(stderr,
                "event propagation committed=%s summary=\"%s\" error=\"%s\"\n",
                event_committed ? "yes" : "no",
                event_propagation.page.script_result.summary,
                event_propagation.page.script_result.error);
    }
    CHECK(event_committed
          && strcmp(event_propagation.page.script_result.summary,
                    "events:ok") == 0
          && command_x(&event_propagation.page.layout, "events:ok") >= 0);
    navigation_destroy(&event_propagation);
    CHECK(budget.current == 0);

    puts("test: directional focus follows retained layout geometry");
    static const char spatial_focus_page[] =
        "<!doctype html><meta name=viewport content='width=device-width'>"
        "<style>body{margin:0;position:relative;height:620px}"
        "button{position:absolute;width:72px;height:28px}"
        "#right{left:300px;top:80px}#down{left:190px;top:145px}"
        "#center{left:190px;top:80px}#up{left:190px;top:15px}"
        "#left{left:80px;top:80px}"
        "#zero{position:absolute;left:260px;top:500px;width:0;height:0;"
        "padding:0;border:0}</style><body>"
        "<button id=right>RIGHT</button><button id=down>DOWN</button>"
        "<button id=center>CENTER</button><button id=up>UP</button>"
        "<button id=left>LEFT</button><input id=zero type=text></body>";
    NavigationSession spatial_focus_navigation;
    CHECK(navigation_init(&spatial_focus_navigation, &budget, 2));
    uint64_t spatial_focus_generation =
        navigation_begin(&spatial_focus_navigation);
    CHECK(navigation_commit_html(
        &spatial_focus_navigation, spatial_focus_generation,
        "https://focus.test/spatial", spatial_focus_page,
        strlen(spatial_focus_page), 480, NULL, NULL, true));
    BrowserController spatial_focus_controller;
    CHECK(controller_init(
        &spatial_focus_controller, &spatial_focus_navigation));
    lxb_dom_node_t *spatial_center = find_id(
        lxb_dom_interface_node(spatial_focus_navigation.page.document.html),
        "center");
    lxb_dom_node_t *spatial_up = find_id(
        lxb_dom_interface_node(spatial_focus_navigation.page.document.html),
        "up");
    lxb_dom_node_t *spatial_down = find_id(
        lxb_dom_interface_node(spatial_focus_navigation.page.document.html),
        "down");
    lxb_dom_node_t *spatial_left = find_id(
        lxb_dom_interface_node(spatial_focus_navigation.page.document.html),
        "left");
    lxb_dom_node_t *spatial_right = find_id(
        lxb_dom_interface_node(spatial_focus_navigation.page.document.html),
        "right");
    lxb_dom_node_t *spatial_zero = find_id(
        lxb_dom_interface_node(spatial_focus_navigation.page.document.html),
        "zero");
    CHECK(spatial_center != NULL && spatial_up != NULL
          && spatial_down != NULL && spatial_left != NULL
          && spatial_right != NULL && spatial_zero != NULL
          && controller_focus_node(
                 &spatial_focus_controller, spatial_center)
          && controller_focus_direction(
                 &spatial_focus_controller, CONTROLLER_FOCUS_UP)
          && spatial_focus_controller.focus_node == spatial_up
          && controller_focus_node(
                 &spatial_focus_controller, spatial_center)
          && controller_focus_direction(
                 &spatial_focus_controller, CONTROLLER_FOCUS_DOWN)
          && spatial_focus_controller.focus_node == spatial_down
          && controller_focus_node(
                 &spatial_focus_controller, spatial_center)
          && controller_focus_direction(
                 &spatial_focus_controller, CONTROLLER_FOCUS_LEFT)
          && spatial_focus_controller.focus_node == spatial_left
          && controller_focus_node(
                 &spatial_focus_controller, spatial_center)
          && controller_focus_direction(
                 &spatial_focus_controller, CONTROLLER_FOCUS_RIGHT)
          && spatial_focus_controller.focus_node == spatial_right);
    int zero_width = -1, zero_height = -1;
    CHECK(controller_focus_node(
              &spatial_focus_controller, spatial_zero)
          && navigation_current(&spatial_focus_navigation)->scroll_y > 0
          && controller_focused_rect(
                 &spatial_focus_controller, NULL, NULL,
                 &zero_width, &zero_height)
          && (zero_width == 0 || zero_height == 0));
    navigation_destroy(&spatial_focus_navigation);
    CHECK(budget.current == 0);

    puts("test: sequential focus deduplicates inline link fragments");
    static const char fragment_focus_page[] =
        "<!doctype html><style>body{margin:0;width:90px}"
        "a{display:inline-block;margin-bottom:8px}</style><body>"
        "<a id=multi href=/same><span>ONE</span><span>TWO</span>"
        "<span>THREE</span></a><br>"
        "<a id=duplicate href=/same><span>NEXT</span></a>";
    NavigationSession fragment_focus_navigation;
    CHECK(navigation_init(&fragment_focus_navigation, &budget, 2));
    navigation_enable_scripts(&fragment_focus_navigation, 4 * MIB, 1000);
    uint64_t fragment_focus_generation =
        navigation_begin(&fragment_focus_navigation);
    CHECK(navigation_commit_html(
        &fragment_focus_navigation, fragment_focus_generation,
        "https://focus.test/fragments", fragment_focus_page,
        strlen(fragment_focus_page), 90, NULL, NULL, true));
    BrowserController fragment_focus_controller;
    lxb_dom_node_t *multi = find_id(
        lxb_dom_interface_node(
            fragment_focus_navigation.page.document.html), "multi");
    lxb_dom_node_t *duplicate = find_id(
        lxb_dom_interface_node(
            fragment_focus_navigation.page.document.html), "duplicate");
    CHECK(fragment_focus_navigation.page.layout.link_count >= 4
          && controller_init(
                 &fragment_focus_controller, &fragment_focus_navigation));
    size_t fragment_focus_relayouts =
        fragment_focus_navigation.incremental_relayouts;
    size_t fragment_focus_skips =
        fragment_focus_navigation.performance.focus_outline_relayout_skips;
    CHECK(controller_focus_node(&fragment_focus_controller, multi)
          && fragment_focus_navigation.incremental_relayouts
                 == fragment_focus_relayouts
          && fragment_focus_navigation.performance.focus_outline_relayout_skips
                 == ++fragment_focus_skips);
    const LinkRegion first_fragment =
        fragment_focus_navigation.page.layout.links[
            fragment_focus_controller.focus_index];
    int semantic_width = 0, semantic_height = 0;
    CHECK(controller_focused_rect(
              &fragment_focus_controller, NULL, NULL,
              &semantic_width, &semantic_height)
          && (semantic_width > first_fragment.width
              || semantic_height > first_fragment.height)
          && controller_focus_next(&fragment_focus_controller)
          && fragment_focus_controller.focus_node == duplicate);
    CHECK(controller_focus_previous(&fragment_focus_controller)
          && fragment_focus_controller.focus_node == multi);
    CHECK(fragment_focus_navigation.incremental_relayouts
              == fragment_focus_relayouts
          && fragment_focus_navigation.performance.focus_outline_relayout_skips
              == fragment_focus_skips + 2
          && controller_focus_next(&fragment_focus_controller)
          && fragment_focus_controller.focus_node == duplicate
          && fragment_focus_navigation.incremental_relayouts
                 == fragment_focus_relayouts
          && fragment_focus_navigation.performance.focus_outline_relayout_skips
                 == fragment_focus_skips + 3);
    navigation_destroy(&fragment_focus_navigation);
    CHECK(budget.current == 0);

    puts("test: focus outline skips layout but geometry and relations do not");
    static const char focus_invalidation_page[] =
        "<!doctype html><style>"
        "a{display:block;width:80px;height:20px}"
        "a:focus{outline:2px solid #123456;outline-offset:3px}"
        "button{display:block;width:60px;height:20px}"
        "button:focus{width:120px}"
        "input:focus+span{color:#ff0000}"
        ".paint{width:80px;border:1px solid #72777d;border-radius:2px}"
        ".paint:focus{width:80px;border-color:#3366cc;"
        "box-shadow:inset 0 0 0 1px #3366cc;outline:1px solid transparent}"
        "</style><body><a id=fast1 href=/1>ONE</a>"
        "<a id=fast2 href=/2>TWO</a><button id=wide>WIDE</button>"
        "<button class=paint id=paint>PAINT</button>"
        "<input id=related><span>STATE</span>";
    NavigationSession focus_invalidation_navigation;
    CHECK(navigation_init(&focus_invalidation_navigation, &budget, 2));
    navigation_enable_scripts(&focus_invalidation_navigation, 4 * MIB, 1000);
    uint64_t focus_invalidation_generation =
        navigation_begin(&focus_invalidation_navigation);
    CHECK(navigation_commit_html(
        &focus_invalidation_navigation, focus_invalidation_generation,
        "https://focus.test/invalidation", focus_invalidation_page,
        strlen(focus_invalidation_page), 480, NULL, NULL, true));
    BrowserController focus_invalidation_controller;
    CHECK(controller_init(
        &focus_invalidation_controller, &focus_invalidation_navigation));
    lxb_dom_node_t *fast1 = find_id(
        lxb_dom_interface_node(
            focus_invalidation_navigation.page.document.html), "fast1");
    lxb_dom_node_t *fast2 = find_id(
        lxb_dom_interface_node(
            focus_invalidation_navigation.page.document.html), "fast2");
    lxb_dom_node_t *wide = find_id(
        lxb_dom_interface_node(
            focus_invalidation_navigation.page.document.html), "wide");
    lxb_dom_node_t *related = find_id(
        lxb_dom_interface_node(
            focus_invalidation_navigation.page.document.html), "related");
    lxb_dom_node_t *paint = find_id(
        lxb_dom_interface_node(
            focus_invalidation_navigation.page.document.html), "paint");
    size_t focus_invalidation_relayouts =
        focus_invalidation_navigation.incremental_relayouts;
    CHECK(fast1 != NULL && fast2 != NULL && wide != NULL && related != NULL
          && paint != NULL);
    bool fast1_focused =
        controller_focus_node(&focus_invalidation_controller, fast1);
    if (!fast1_focused
        || focus_invalidation_navigation.incremental_relayouts
               != focus_invalidation_relayouts
        || focus_invalidation_navigation.performance
               .focus_outline_relayout_skips != 1) {
        fprintf(stderr, "focus fast1 moved=%d relayouts=%zu/%zu outline=%zu "
                "paint=%zu error=%s\n",
                fast1_focused,
                focus_invalidation_navigation.incremental_relayouts,
                focus_invalidation_relayouts,
                focus_invalidation_navigation.performance
                    .focus_outline_relayout_skips,
                focus_invalidation_navigation.performance
                    .focus_paint_relayout_skips,
                focus_invalidation_navigation.last_error);
    }
    CHECK(fast1_focused
          && focus_invalidation_navigation.incremental_relayouts
                 == focus_invalidation_relayouts
          && focus_invalidation_navigation.performance
                 .focus_outline_relayout_skips == 1);
    ControllerFocusOutline authored_focus = {0};
    CHECK(controller_focused_outline_style(
              &focus_invalidation_controller, &authored_focus)
          && authored_focus.width == 2
          && authored_focus.offset == 3
          && authored_focus.color == 0x123456
          && controller_focus_node(&focus_invalidation_controller, fast2)
          && focus_invalidation_navigation.incremental_relayouts
                 == focus_invalidation_relayouts
          && focus_invalidation_navigation.performance
                 .focus_outline_relayout_skips == 2);
    CHECK(controller_focus_node(&focus_invalidation_controller, paint)
          && focus_invalidation_navigation.incremental_relayouts
                 == focus_invalidation_relayouts
          && focus_invalidation_navigation.performance
                 .focus_paint_relayout_skips == 1
          && focus_invalidation_navigation.focus_paint_generation == 1
          && focus_invalidation_navigation.focus_paint_damage_valid);

    enum { FOCUS_FRAME_WIDTH = 480, FOCUS_FRAME_HEIGHT = 272 };
    size_t focus_frame_pixels =
        (size_t) FOCUS_FRAME_WIDTH * FOCUS_FRAME_HEIGHT;
    uint16_t *fast_focus_frame = budget_malloc(
        &budget, focus_frame_pixels * sizeof(*fast_focus_frame));
    uint16_t *full_focus_frame = budget_malloc(
        &budget, focus_frame_pixels * sizeof(*full_focus_frame));
    TileCache fast_focus_cache = {0}, full_focus_cache = {0};
    CHECK(fast_focus_frame != NULL && full_focus_frame != NULL
          && tile_cache_init(
              &fast_focus_cache, &budget,
              &focus_invalidation_navigation.page.layout, 8)
          && tile_cache_set_frame(
              &fast_focus_cache, fast_focus_frame, focus_frame_pixels)
          && tile_cache_render_frame(
              &fast_focus_cache, 0, FOCUS_FRAME_WIDTH,
              FOCUS_FRAME_HEIGHT, NULL));
    focus_invalidation_navigation.style_rebuild_required = true;
    CHECK(navigation_relayout(&focus_invalidation_navigation)
          && focus_invalidation_navigation.incremental_relayouts
                 == ++focus_invalidation_relayouts
          && tile_cache_init(
              &full_focus_cache, &budget,
              &focus_invalidation_navigation.page.layout, 8)
          && tile_cache_set_frame(
              &full_focus_cache, full_focus_frame, focus_frame_pixels)
          && tile_cache_render_frame(
              &full_focus_cache, 0, FOCUS_FRAME_WIDTH,
              FOCUS_FRAME_HEIGHT, NULL)
          && memcmp(
              fast_focus_frame, full_focus_frame,
              focus_frame_pixels * sizeof(*fast_focus_frame)) == 0);
    tile_cache_destroy(&full_focus_cache);
    tile_cache_destroy(&fast_focus_cache);
    budget_free(&budget, full_focus_frame);
    budget_free(&budget, fast_focus_frame);

    CHECK(controller_focus_node(&focus_invalidation_controller, wide)
          && focus_invalidation_navigation.incremental_relayouts
                 == ++focus_invalidation_relayouts);
    const LayoutNodeBox *wide_box = layout_box_for_node(
        &focus_invalidation_navigation.page.layout, wide);
    CHECK(wide_box != NULL && wide_box->width >= 120);
    CHECK(controller_focus_node(&focus_invalidation_controller, related)
          && focus_invalidation_navigation.incremental_relayouts
                 == ++focus_invalidation_relayouts);
    navigation_destroy(&focus_invalidation_navigation);
    CHECK(budget.current == 0);

    puts("test: relational and inherited focus dependencies force layout");
    static const char focus_dependency_page[] =
        "<!doctype html><style>"
        "body:has(.has-source:focus-visible) .has-target{color:#ff0000}"
        ".within:focus-within .within-target{color:#00ff00}"
        ".custom:focus{--descendant-focus:#ff0000}"
        ".custom span{color:var(--descendant-focus,#000000)}"
        "</style><body>"
        "<button class=has-source id=has-source>HAS</button>"
        "<span class=has-target>REMOTE</span>"
        "<div class=within><input id=within><span class=within-target>WITHIN</span></div>"
        "<button class=custom id=custom>CUSTOM<span>CHILD</span></button>";
    NavigationSession focus_dependency_navigation;
    CHECK(navigation_init(&focus_dependency_navigation, &budget, 2));
    navigation_enable_scripts(&focus_dependency_navigation, 4 * MIB, 1000);
    uint64_t focus_dependency_generation =
        navigation_begin(&focus_dependency_navigation);
    CHECK(navigation_commit_html(
        &focus_dependency_navigation, focus_dependency_generation,
        "https://focus.test/dependencies", focus_dependency_page,
        strlen(focus_dependency_page), 480, NULL, NULL, true));
    BrowserController focus_dependency_controller;
    CHECK(controller_init(
        &focus_dependency_controller, &focus_dependency_navigation));
    lxb_dom_node_t *has_source = find_id(
        lxb_dom_interface_node(
            focus_dependency_navigation.page.document.html), "has-source");
    lxb_dom_node_t *within = find_id(
        lxb_dom_interface_node(
            focus_dependency_navigation.page.document.html), "within");
    lxb_dom_node_t *custom = find_id(
        lxb_dom_interface_node(
            focus_dependency_navigation.page.document.html), "custom");
    size_t focus_dependency_relayouts =
        focus_dependency_navigation.incremental_relayouts;
    CHECK(has_source != NULL && within != NULL && custom != NULL
          && controller_focus_node(&focus_dependency_controller, has_source)
          && focus_dependency_navigation.incremental_relayouts
                 == ++focus_dependency_relayouts
          && controller_focus_node(&focus_dependency_controller, within)
          && focus_dependency_navigation.incremental_relayouts
                 == ++focus_dependency_relayouts
          && controller_focus_node(&focus_dependency_controller, custom)
          && focus_dependency_navigation.incremental_relayouts
                 == ++focus_dependency_relayouts
          && focus_dependency_navigation.performance
                 .focus_paint_relayout_skips == 0);
    navigation_destroy(&focus_dependency_navigation);
    CHECK(budget.current == 0);

    puts("test: repeated host focus stays bounded and reflects activeElement");
    char focus_page[32768];
    size_t focus_page_length = (size_t) snprintf(
        focus_page, sizeof(focus_page),
        "<!doctype html><style>a{display:block;padding:2px}</style><body>"
        "<script>var focusRealm=1</script><div><div><div><section><article>");
    for (size_t i = 0; i < 200 && focus_page_length < sizeof(focus_page); i++) {
        int written = snprintf(
            focus_page + focus_page_length,
            sizeof(focus_page) - focus_page_length,
            "<p><a id=focus-%zu href=/%zu>focus %zu</a></p>", i, i, i);
        CHECK(written > 0
              && (size_t) written
                     < sizeof(focus_page) - focus_page_length);
        focus_page_length += (size_t) written;
    }
    NavigationSession bounded_focus_navigation;
    CHECK(navigation_init(&bounded_focus_navigation, &budget, 2));
    navigation_enable_scripts(&bounded_focus_navigation, 4 * MIB, 1000);
    uint64_t bounded_focus_generation =
        navigation_begin(&bounded_focus_navigation);
    CHECK(navigation_commit_html(
        &bounded_focus_navigation, bounded_focus_generation,
        "https://focus.test/", focus_page, focus_page_length,
        480, NULL, NULL, true));
    BrowserController bounded_focus_controller;
    CHECK(controller_init(
        &bounded_focus_controller, &bounded_focus_navigation));
    size_t completed_focus_moves = 0;
    size_t failed_focus_dispatches = 0;
    for (; completed_focus_moves < 400; completed_focus_moves++) {
        if (!controller_focus_next(&bounded_focus_controller)) break;
        if (!bounded_focus_navigation.page.script_result.success)
            failed_focus_dispatches++;
    }
    size_t focused_id_length = 0;
    const char *focused_id = document_attribute(
        bounded_focus_controller.focus_node, "id", &focused_id_length);
    char active_focus_probe[256];
    snprintf(
        active_focus_probe, sizeof(active_focus_probe),
        "globalThis.pocSummary=document.activeElement?.id==='%.*s'"
        "?'bounded-focus:ok':'bounded-focus:'+(document.activeElement?.id||"
        "'none')",
        (int) focused_id_length, focused_id == NULL ? "" : focused_id);
    bool active_focus_evaluated = script_runtime_evaluate_diagnostic(
        bounded_focus_navigation.page.runtime, active_focus_probe,
        "<bounded-focus-probe>",
        &bounded_focus_navigation.page.script_result);
    if (completed_focus_moves != 400 || failed_focus_dispatches != 0
        || !active_focus_evaluated
        || strcmp(bounded_focus_navigation.page.script_result.summary,
                  "bounded-focus:ok") != 0) {
        fprintf(
            stderr,
            "bounded focus moves=%zu dispatch-failures=%zu index=%zu "
            "evaluated=%s summary=\"%s\" error=\"%s\"\n",
            completed_focus_moves, failed_focus_dispatches,
            bounded_focus_controller.focus_index,
            active_focus_evaluated ? "yes" : "no",
            bounded_focus_navigation.page.script_result.summary,
            bounded_focus_navigation.page.script_result.error);
    }
    CHECK(completed_focus_moves == 400 && failed_focus_dispatches == 0
          && active_focus_evaluated
          && bounded_focus_navigation.incremental_relayouts == 0
          && bounded_focus_navigation.performance
                 .focus_outline_relayout_skips == 400
          && strcmp(
              bounded_focus_navigation.page.script_result.summary,
              "bounded-focus:ok") == 0);
    navigation_destroy(&bounded_focus_navigation);
    CHECK(budget.current == 0);

    puts("test: bounded dialog and popover state");
    static const char modal_page[] =
        "<!doctype html><style>body{margin:0;padding:0}dialog:modal{width:180px;"
        "height:40px}#pop:popover-open{width:120px;height:30px}</style><body>"
        "<dialog id=modal><button id=inside>OK</button></dialog>"
        "<dialog id=closed>CLOSED</dialog><div id=pop popover>MENU</div>"
        "<p id=result>WAIT</p><script>const modal=document.getElementById('modal'),"
        "inside=document.getElementById('inside'),pop=document.getElementById('pop'),"
        "events=[];let blocked=true;modal.addEventListener('cancel',event=>{events."
        "push('cancel:'+event.isTrusted);if(blocked){blocked=false;event.preventDefault();}});"
        "modal.addEventListener('close',event=>events.push('close:'+event.isTrusted));"
        "modal.showModal();const opened=modal.open&&modal.matches(':modal')&&modal."
        "matches(':open')&&document.activeElement===inside;modal.requestClose('blocked');"
        "const stayed=modal.open&&modal.returnValue==='';modal.requestClose('accepted');"
        "const closed=!modal.open&&modal.returnValue==='accepted';modal.showModal();"
        "let first=true;pop.addEventListener('beforetoggle',event=>{events.push('before:'"
        "+event.oldState+'>'+event.newState+':'+event.isTrusted);if(first){first=false;"
        "event.preventDefault();}});pop.addEventListener('toggle',event=>events.push('toggle:'"
        "+event.oldState+'>'+event.newState+':'+event.isTrusted));pop.showPopover();"
        "const popBlocked=!pop.matches(':popover-open');pop.showPopover();pop.hidePopover();"
        "pop.togglePopover(true);const popOpen=pop.matches(':popover-open')&&pop.matches(':open');"
        "globalThis.pocSummary=opened&&stayed&&closed&&modal.matches(':modal')&&"
        "popBlocked&&popOpen&&events.filter(value=>value==='cancel:true').length===2&&"
        "events.includes('close:true')&&events.includes('toggle:closed>open:true')"
        "?'modal-popover:ok':'modal-popover:'+events.join('|');document.getElementById('result')."
        "textContent=pocSummary;</script></body>";
    NavigationSession modal_navigation;
    CHECK(navigation_init(&modal_navigation, &budget, 2));
    navigation_enable_scripts(&modal_navigation, 3 * MIB, 1000);
    uint64_t modal_generation = navigation_begin(&modal_navigation);
    CHECK(navigation_commit_html(
        &modal_navigation, modal_generation,
        "https://modal.test/", modal_page, strlen(modal_page),
        480, NULL, NULL, true));
    if (strcmp(modal_navigation.page.script_result.summary,
               "modal-popover:ok") != 0) {
        fprintf(stderr, "modal summary=%s error=%s\n",
                modal_navigation.page.script_result.summary,
                modal_navigation.page.script_result.error);
    }
    CHECK(strcmp(modal_navigation.page.script_result.summary,
                 "modal-popover:ok") == 0);
    lxb_dom_node_t *modal_root = lxb_dom_interface_node(
        modal_navigation.page.document.html);
    lxb_dom_node_t *modal_node = find_id(modal_root, "modal");
    lxb_dom_node_t *closed_node = find_id(modal_root, "closed");
    lxb_dom_node_t *popover_node = find_id(modal_root, "pop");
    ComputedStyle modal_style = style_for_node(
        &modal_navigation.page.stylesheet, modal_node, NULL);
    ComputedStyle closed_style = style_for_node(
        &modal_navigation.page.stylesheet, closed_node, NULL);
    ComputedStyle popover_style = style_for_node(
        &modal_navigation.page.stylesheet, popover_node, NULL);
    const LayoutNodeBox *modal_box = layout_box_for_node(
        &modal_navigation.page.layout, modal_node);
    const LayoutNodeBox *popover_box = layout_box_for_node(
        &modal_navigation.page.layout, popover_node);
    CHECK(modal_style.fixed_position && modal_style.has_z_index
          && modal_style.z_index == 10000 && modal_style.width == 180
          && closed_style.display == DISPLAY_NONE
          && popover_style.fixed_position && popover_style.has_z_index
          && popover_style.z_index == 10001 && popover_style.width == 120
          && modal_box != NULL && modal_box->width == 204
          && popover_box != NULL && popover_box->width == 120
          && modal_navigation.page.layout.fixed_count == 2);
    navigation_destroy(&modal_navigation);
    CHECK(budget.current == 0);

    puts("test: details disclosure activation");
    static const char details_page[] =
        "<!doctype html><body><details id=disclosure><summary id=summary>More"
        " information</summary><p id=secret>DISCLOSED CONTENT</p></details>"
        "<p id=result>WAIT</p><script>const details=document.getElementById("
        "'disclosure'),summary=document.getElementById('summary');details."
        "addEventListener('toggle',event=>details.setAttribute('data-toggle',"
        "event.oldState+'>'+event.newState+':'+event.isTrusted));summary.click();"
        "const opened=details.open&&details.matches(':open');summary.click();"
        "globalThis.pocSummary=opened&&!details.open&&summary instanceof "
        "HTMLSummaryElement&&details instanceof HTMLDetailsElement&&details."
        "getAttribute('data-toggle')==='open>closed:true'?'details:ok':'details:fail';"
        "document.getElementById('result').textContent=pocSummary;</script></body>";
    NavigationSession details_navigation;
    CHECK(navigation_init(&details_navigation, &budget, 2));
    navigation_enable_scripts(&details_navigation, 3 * MIB, 1000);
    uint64_t details_generation = navigation_begin(&details_navigation);
    CHECK(navigation_commit_html(
              &details_navigation, details_generation,
              "https://details.test/", details_page, strlen(details_page),
              480, NULL, NULL, true)
          && strcmp(details_navigation.page.script_result.summary,
                    "details:ok") == 0
          && command_x(&details_navigation.page.layout,
                       "DISCLOSED") < 0);
    lxb_dom_node_t *details_root = lxb_dom_interface_node(
        details_navigation.page.document.html);
    lxb_dom_node_t *details_node = find_id(details_root, "disclosure");
    lxb_dom_node_t *summary_node = find_id(details_root, "summary");
    BrowserController details_controller;
    ControllerAction details_action;
    CHECK(controller_init(&details_controller, &details_navigation)
          && controller_focus_node(&details_controller, summary_node)
          && controller_activate(&details_controller, &details_action)
          && details_action.type == CONTROLLER_ACTION_CONTROL
          && lxb_dom_element_has_attribute(
                 lxb_dom_interface_element(details_node),
                 (const lxb_char_t *) "open", 4)
          && attribute_equals(details_node, "data-toggle",
                              "closed>open:true")
          && command_x(&details_navigation.page.layout,
                       "DISCLOSED") >= 0);
    CHECK(controller_focus_node(&details_controller, summary_node)
          && controller_activate(&details_controller, &details_action)
          && !lxb_dom_element_has_attribute(
                  lxb_dom_interface_element(details_node),
                  (const lxb_char_t *) "open", 4)
          && attribute_equals(details_node, "data-toggle",
                              "open>closed:true")
          && command_x(&details_navigation.page.layout,
                       "DISCLOSED") < 0);
    navigation_destroy(&details_navigation);
    CHECK(budget.current == 0);

    puts("test: checkbox and radio activation defaults");
    static const char choice_page[] =
        "<!doctype html><style>#check:checked{width:77px}</style><body>"
        "<input id=check type=checkbox><input id=cancel type=checkbox>"
        "<input id=radio-a type=radio name=group checked>"
        "<input id=radio-b type=radio name=group><p id=result>WAIT</p><script>"
        "const check=document.getElementById('check'),cancel=document.getElementById("
        "'cancel'),a=document.getElementById('radio-a'),b=document.getElementById("
        "'radio-b'),events=[];for(const type of ['click','input','change'])check."
        "addEventListener(type,event=>events.push(type+':'+check.checked+':'"
        "+event.isTrusted));cancel.addEventListener('click',event=>{events.push("
        "'cancel-sees:'+cancel.checked);event.preventDefault();});b.addEventListener("
        "'click',event=>events.push('radio-click:'+a.checked+':'+b.checked));"
        "check.click();const checked=check.checked&&check.matches(':checked');"
        "const cancelled=!cancel.click()&&!cancel.checked;b.click();const radios="
        "!a.checked&&b.checked;globalThis.pocSummary=checked&&cancelled&&radios&&"
        "events.join('|')==='click:true:false|input:true:true|change:true:true|"
        "cancel-sees:true|radio-click:false:true'?'choices:ok':'choices:'+events."
        "join('|');document.getElementById('result').textContent=pocSummary;"
        "</script></body>";
    NavigationSession choice_navigation;
    CHECK(navigation_init(&choice_navigation, &budget, 2));
    navigation_enable_scripts(&choice_navigation, 3 * MIB, 1000);
    uint64_t choice_generation = navigation_begin(&choice_navigation);
    CHECK(navigation_commit_html(
              &choice_navigation, choice_generation,
              "https://choices.test/", choice_page, strlen(choice_page),
              480, NULL, NULL, true)
          && strcmp(choice_navigation.page.script_result.summary,
                    "choices:ok") == 0);
    lxb_dom_node_t *choice_root = lxb_dom_interface_node(
        choice_navigation.page.document.html);
    lxb_dom_node_t *check_node = find_id(choice_root, "check");
    lxb_dom_node_t *radio_a_node = find_id(choice_root, "radio-a");
    lxb_dom_node_t *radio_b_node = find_id(choice_root, "radio-b");
    ComputedStyle checked_choice_style = style_for_node(
        &choice_navigation.page.stylesheet, check_node, NULL);
    BrowserController choice_controller;
    ControllerAction choice_action;
    bool radio_a_checked = lxb_dom_element_has_attribute(
        lxb_dom_interface_element(radio_a_node),
        (const lxb_char_t *) "checked", 7);
    bool radio_b_checked = lxb_dom_element_has_attribute(
        lxb_dom_interface_element(radio_b_node),
        (const lxb_char_t *) "checked", 7);
    if (!checked_choice_style.has_width || checked_choice_style.width != 77
        || radio_a_checked || !radio_b_checked) {
        fprintf(stderr,
                "choice pre-controller style_width=%s:%d radio_a=%s "
                "radio_b=%s summary=\"%s\"\n",
                checked_choice_style.has_width ? "yes" : "no",
                checked_choice_style.width,
                radio_a_checked ? "checked" : "unchecked",
                radio_b_checked ? "checked" : "unchecked",
                choice_navigation.page.script_result.summary);
    }
    bool choice_controller_initialized = controller_init(
        &choice_controller, &choice_navigation);
    bool choice_focused = choice_controller_initialized
        && controller_focus_node(&choice_controller, check_node);
    bool choice_activated = choice_focused
        && controller_activate(&choice_controller, &choice_action);
    bool check_still_checked = lxb_dom_element_has_attribute(
        lxb_dom_interface_element(check_node),
        (const lxb_char_t *) "checked", 7);
    if (!choice_controller_initialized || !choice_focused || !choice_activated
        || choice_action.type != CONTROLLER_ACTION_CONTROL
        || check_still_checked) {
        fprintf(stderr,
                "choice controller init=%s focus=%s activate=%s action=%d "
                "checked_after=%s last_cancelled=%s\n",
                choice_controller_initialized ? "yes" : "no",
                choice_focused ? "yes" : "no",
                choice_activated ? "yes" : "no",
                (int) choice_action.type,
                check_still_checked ? "yes" : "no",
                choice_navigation.page.script_result.last_event_cancelled
                    ? "yes" : "no");
    }
    CHECK(checked_choice_style.has_width && checked_choice_style.width == 77
          && !radio_a_checked
          && radio_b_checked
          && choice_controller_initialized
          && choice_focused
          && choice_activated
          && choice_action.type == CONTROLLER_ACTION_CONTROL
          && !check_still_checked);
    ComputedStyle unchecked_choice_style = style_for_node(
        &choice_navigation.page.stylesheet, check_node, NULL);
    CHECK(!unchecked_choice_style.has_width
          && attribute_equals(check_node, "data-tilefinch-focus", ""));
    navigation_destroy(&choice_navigation);
    CHECK(budget.current == 0);

    puts("test: select and option state model");
    static const char select_page[] =
        "<!doctype html><body><form id=form><select id=single name=choice>"
        "<option id=alpha value=a>ALPHA OPTION</option><optgroup>"
        "<option id=beta value=b>BETA OPTION</option></optgroup></select>"
        "<select id=multi name=many multiple><option value=x selected>X</option>"
        "<option value=y selected>Y</option></select></form><p id=result>WAIT</p>"
        "<script>const single=document.getElementById('single'),alpha=document."
        "getElementById('alpha'),beta=document.getElementById('beta'),multi="
        "document.getElementById('multi');const initial=single.value==='a'&&"
        "single.selectedIndex===0&&alpha.selected&&single.options.item(1)===beta&&"
        "beta.index===1;single.value='b';const changed=single.value==='b'&&"
        "single.selectedIndex===1&&!alpha.selected&&beta.selected&&single."
        "selectedOptions.length===1;const multiple=multi.multiple&&multi."
        "selectedOptions.length===2&&new FormData(document.getElementById('form'))."
        "getAll('many').join(',')==='x,y';globalThis.pocSummary=initial&&changed&&"
        "multiple?'select:ok':'select:fail';document.getElementById('result')."
        "textContent=pocSummary;</script></body>";
    NavigationSession select_navigation;
    CHECK(navigation_init(&select_navigation, &budget, 2));
    navigation_enable_scripts(&select_navigation, 3 * MIB, 1000);
    uint64_t select_generation = navigation_begin(&select_navigation);
    bool select_committed = navigation_commit_html(
        &select_navigation, select_generation,
        "https://select.test/", select_page, strlen(select_page),
        480, NULL, NULL, true);
    if (!select_committed
        || strcmp(select_navigation.page.script_result.summary,
                  "select:ok") != 0
        || command_x(&select_navigation.page.layout, "BETA") < 0
        || command_x(&select_navigation.page.layout, "ALPHA") >= 0) {
        fprintf(stderr, "select committed=%d summary=%s error=%s beta=%d alpha=%d "
                "handles=%zu/%zu high=%zu exhaustions=%zu\n",
                select_committed,
                select_navigation.page.script_result.summary,
                select_navigation.page.script_result.error,
                command_x(&select_navigation.page.layout, "BETA"),
                command_x(&select_navigation.page.layout, "ALPHA"),
                select_navigation.page.script_result.dom_handle_slots_live,
                select_navigation.page.script_result.dom_handle_slots_peak,
                select_navigation.page.script_result.dom_handle_slots_high_water,
                select_navigation.page.script_result.dom_handle_exhaustions);
    }
    CHECK(select_committed
          && strcmp(select_navigation.page.script_result.summary,
                    "select:ok") == 0
          && command_x(&select_navigation.page.layout, "BETA") >= 0
          && command_x(&select_navigation.page.layout, "ALPHA") < 0);
    lxb_dom_node_t *select_root = lxb_dom_interface_node(
        select_navigation.page.document.html);
    lxb_dom_node_t *alpha_node = find_id(select_root, "alpha");
    lxb_dom_node_t *beta_node = find_id(select_root, "beta");
    CHECK(!lxb_dom_element_has_attribute(
              lxb_dom_interface_element(alpha_node),
              (const lxb_char_t *) "selected", 8)
          && !lxb_dom_element_has_attribute(
                 lxb_dom_interface_element(beta_node),
                 (const lxb_char_t *) "selected", 8));
    navigation_destroy(&select_navigation);
    CHECK(budget.current == 0);

    puts("test: bounded form constraint validation");
    static const char validation_page[] =
        "<!doctype html><body><form id=form><input id=field name=code required "
        "pattern='[0-9]{3}' minlength=3 maxlength=3><select id=kind required>"
        "<option value=''>Choose</option><option value=ok>Okay</option></select>"
        "<button id=submit>Send</button></form><p id=result>WAIT</p><script>"
        "const form=document.getElementById('form'),field=document.getElementById("
        "'field'),kind=document.getElementById('kind'),submit=document.getElementById("
        "'submit');let invalid=[];field.addEventListener('invalid',event=>invalid."
        "push('field:'+event.isTrusted+':'+event.bubbles));kind.addEventListener("
        "'invalid',event=>invalid.push('kind:'+event.isTrusted));const missing="
        "!form.checkValidity()&&field.validity.valueMissing&&kind.validity."
        "valueMissing;field.value='12';kind.value='ok';const pattern=!field."
        "checkValidity()&&field.validity.patternMismatch&&field.validity.tooShort;"
        "field.value='123';field.setCustomValidity('Nope');const custom=!field."
        "checkValidity()&&field.validity.customError&&field.validationMessage==="
        "'Nope';field.setCustomValidity('');let submitted=false;form.addEventListener("
        "'submit',event=>{submitted=event.submitter===submit;event.preventDefault();});"
        "form.requestSubmit(submit);globalThis.pocSummary=missing&&pattern&&custom&&"
        "field.validity.valid&&submitted&&field instanceof HTMLInputElement&&field."
        "validity instanceof ValidityState&&invalid.join('|')==='field:true:false|"
        "kind:true|field:true:false|field:true:false'?'validation:ok':'validation:'"
        "+invalid.join('|');document.getElementById('result').textContent=pocSummary;"
        "</script></body>";
    NavigationSession validation_navigation;
    CHECK(navigation_init(&validation_navigation, &budget, 2));
    navigation_enable_scripts(&validation_navigation, 3 * MIB, 1000);
    uint64_t validation_generation = navigation_begin(&validation_navigation);
    bool validation_committed = navigation_commit_html(
        &validation_navigation, validation_generation,
        "https://validation.test/", validation_page,
        strlen(validation_page), 480, NULL, NULL, true);
    if (!validation_committed
        || strcmp(validation_navigation.page.script_result.summary,
                  "validation:ok") != 0
        || command_x(&validation_navigation.page.layout,
                     "validation:ok") < 0) {
        fprintf(stderr, "validation committed=%d summary=%s error=%s x=%d\n",
                validation_committed,
                validation_navigation.page.script_result.summary,
                validation_navigation.page.script_result.error,
                command_x(&validation_navigation.page.layout,
                          "validation:ok"));
    }
    CHECK(validation_committed
          && strcmp(validation_navigation.page.script_result.summary,
                    "validation:ok") == 0
          && command_x(&validation_navigation.page.layout,
                       "validation:ok") >= 0);
    navigation_destroy(&validation_navigation);
    CHECK(budget.current == 0);

    puts("test: label control forwarding");
    static const char label_page[] =
        "<!doctype html><body><label id=label for=choice>Enable choice</label>"
        "<input id=choice type=checkbox><label id=blocked for=other>Blocked</label>"
        "<input id=other type=checkbox><p id=result>WAIT</p><script>const label="
        "document.getElementById('label'),choice=document.getElementById('choice'),"
        "blocked=document.getElementById('blocked'),other=document.getElementById("
        "'other'),events=[];for(const type of ['click','input','change'])choice."
        "addEventListener(type,event=>events.push(type+':'+choice.checked+':'"
        "+event.isTrusted));blocked.addEventListener('click',event=>event."
        "preventDefault());label.click();blocked.click();globalThis.pocSummary="
        "label.control===choice&&label.form===null&&choice.checked&&!other.checked&&"
        "events.join('|')==='click:true:false|input:true:true|change:true:true'"
        "?'label:ok':'label:'+events.join('|');document.getElementById('result')."
        "textContent=pocSummary;</script></body>";
    NavigationSession label_navigation;
    CHECK(navigation_init(&label_navigation, &budget, 2));
    navigation_enable_scripts(&label_navigation, 3 * MIB, 1000);
    uint64_t label_generation = navigation_begin(&label_navigation);
    CHECK(navigation_commit_html(
              &label_navigation, label_generation,
              "https://label.test/", label_page, strlen(label_page),
              480, NULL, NULL, true)
          && strcmp(label_navigation.page.script_result.summary,
                    "label:ok") == 0);
    lxb_dom_node_t *label_root = lxb_dom_interface_node(
        label_navigation.page.document.html);
    lxb_dom_node_t *label_node = find_id(label_root, "label");
    lxb_dom_node_t *labeled_choice_node = find_id(label_root, "choice");
    BrowserController label_controller;
    ControllerAction label_action;
    CHECK(controller_init(&label_controller, &label_navigation)
          && controller_focus_node(&label_controller, label_node)
          && controller_activate(&label_controller, &label_action)
          && label_action.type == CONTROLLER_ACTION_CONTROL
          && !lxb_dom_element_has_attribute(
                 lxb_dom_interface_element(labeled_choice_node),
                 (const lxb_char_t *) "checked", 7)
          && attribute_equals(labeled_choice_node,
                              "data-tilefinch-focus", ""));
    navigation_destroy(&label_navigation);
    CHECK(budget.current == 0);

    puts("test: stylesheet motion has a bounded relayout lifetime");
    static const char motion_page[] =
        "<!doctype html><style>@keyframes spin{from{transform:rotate(0deg)}"
        "to{transform:rotate(360deg)}}#spinner{width:16px;height:16px;"
        "animation:spin 1s linear infinite}</style><body>"
        "<div id=spinner></div><script>globalThis.motionReady=true</script>"
        "</body>";
    NavigationSession motion_navigation;
    CHECK(navigation_init(&motion_navigation, &budget, 2));
    navigation_enable_scripts(&motion_navigation, 4 * MIB, 1000);
    uint64_t motion_generation = navigation_begin(&motion_navigation);
    CHECK(navigation_commit_html(
        &motion_navigation, motion_generation, "https://motion.test/",
        motion_page, strlen(motion_page), 480, NULL, NULL, true)
          && motion_navigation.page.stylesheet.has_motion_keyframes);
    for (size_t frame = 0; frame < 260; frame++) {
        CHECK(navigation_advance_runtime(&motion_navigation, 16, 8));
    }
    CHECK(motion_navigation.incremental_relayouts >= 100
          && motion_navigation.incremental_relayouts <= 121
          && motion_navigation.performance.full_relayouts == 0
          && motion_navigation.page.script_result.pending_tasks == 0);
    navigation_destroy(&motion_navigation);
    CHECK(budget.current == 0);

    puts("test: sparse scroll CSSOM and bounded smooth scrolling");
    static const char smooth_scroll_page[] =
        "<!doctype html><style>"
        "#shell{display:block;width:120px;height:50px;overflow:auto;"
        "scroll-behavior:smooth;scroll-snap-type:y mandatory;"
        "scroll-padding-top:7px;overscroll-behavior:contain;"
        "scrollbar-width:none}"
        "#target{display:block;height:180px;scroll-snap-align:start;"
        "scroll-margin-top:3px;cursor:pointer}</style><body>"
        "<div id=shell><div id=target>TARGET</div></div><script>"
        "const shell=document.getElementById('shell'),"
        "target=document.getElementById('target'),"
        "s=getComputedStyle(shell),t=getComputedStyle(target);"
        "globalThis.pocSummary=[s.scrollBehavior,s.scrollSnapType,"
        "s.scrollPaddingTop,s.overscrollBehavior,s.scrollbarWidth,"
        "t.scrollSnapAlign,t.scrollMarginTop,t.cursor,"
        "CSS.supports('scroll-snap-type','y mandatory'),"
        "CSS.supports('scroll-padding','4px 8px'),"
        "CSS.supports('cursor','pointer')].join('|');"
        "shell.scrollTo({top:100,behavior:'smooth'});"
        "setTimeout(()=>{globalThis.pocSummary+='|'+shell.scrollTop},240)"
        "</script></body>";
    NavigationSession smooth_scroll_navigation;
    CHECK(navigation_init(&smooth_scroll_navigation, &budget, 2));
    navigation_enable_scripts(&smooth_scroll_navigation, 4 * MIB, 1000);
    uint64_t smooth_scroll_generation =
        navigation_begin(&smooth_scroll_navigation);
    CHECK(navigation_commit_html(
        &smooth_scroll_navigation, smooth_scroll_generation,
        "https://scroll.test/", smooth_scroll_page,
        strlen(smooth_scroll_page), 160, NULL, NULL, true));
    for (size_t frame = 0; frame < 20; frame++) {
        CHECK(navigation_advance_runtime(
            &smooth_scroll_navigation, 16, 8));
    }
    CHECK(strcmp(
              smooth_scroll_navigation.page.script_result.summary,
              "smooth|y mandatory|7px|contain|none|start|3px|pointer|"
              "true|true|true|100")
          == 0
          && smooth_scroll_navigation.page.layout.scroll_container_count == 1
          && smooth_scroll_navigation.page.layout.scroll_snap_candidate_count
             == 1);
    navigation_destroy(&smooth_scroll_navigation);
    CHECK(budget.current == 0);
