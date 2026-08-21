    puts("test: inherited fieldset disabled state");
    static const char disabled_page[] =
        "<!doctype html><style>input:disabled{width:61px}input:enabled{height:31px}"
        "</style><body><form id=form><fieldset id=set disabled><legend>Legend "
        "<input id=legend-input name=legend value=yes required></legend>"
        "<input id=blocked-input name=blocked value=no required></fieldset>"
        "<input id=own-disabled name=own value=no disabled><select id=select "
        "name=choice><optgroup disabled><option id=disabled-option value=no "
        "selected>No</option></optgroup><option value=yes>Yes</option></select>"
        "</form><p id=result>WAIT</p><script>const form=document.getElementById("
        "'form'),legend=document.getElementById('legend-input'),blocked=document."
        "getElementById('blocked-input'),own=document.getElementById('own-disabled'),"
        "option=document.getElementById('disabled-option'),set=document.getElementById("
        "'set'),data=new FormData(form);globalThis.pocSummary=set instanceof "
        "HTMLFieldSetElement&&set.elements.length===2&&!legend.disabled&&legend."
        "matches(':enabled')&&blocked.disabled&&blocked.matches(':disabled')&&"
        "own.disabled&&option.disabled&&data.get('legend')==='yes'&&!data.has("
        "'blocked')&&!data.has('own')&&!data.has('choice')&&form.checkValidity()?"
        "'disabled:ok':"
        "'disabled:fail';document.getElementById('result').textContent=pocSummary;"
        "</script></body>";
    NavigationSession disabled_navigation;
    CHECK(navigation_init(&disabled_navigation, &budget, 2));
    navigation_enable_scripts(&disabled_navigation, 3 * MIB, 1000);
    uint64_t disabled_generation = navigation_begin(&disabled_navigation);
    CHECK(navigation_commit_html(
              &disabled_navigation, disabled_generation,
              "https://disabled.test/", disabled_page,
              strlen(disabled_page), 480, NULL, NULL, true)
          && strcmp(disabled_navigation.page.script_result.summary,
                    "disabled:ok") == 0);
    lxb_dom_node_t *disabled_root = lxb_dom_interface_node(
        disabled_navigation.page.document.html);
    lxb_dom_node_t *legend_input_node = find_id(disabled_root, "legend-input");
    lxb_dom_node_t *blocked_input_node = find_id(disabled_root, "blocked-input");
    BrowserController disabled_controller;
    CHECK(controller_init(&disabled_controller, &disabled_navigation)
          && controller_focus_node(&disabled_controller, legend_input_node)
          && !controller_focus_node(&disabled_controller, blocked_input_node));
    navigation_destroy(&disabled_navigation);
    CHECK(budget.current == 0);

    puts("test: WPT-derived form and selector compatibility");
    static const char wpt_compatibility_page[] =
        "<!doctype html><body><div id=css-text-host></div><script>"
        "const textarea=document.createElement('textarea');"
        "textarea.textContent='before\\r\\nafter';"
        "const textareaDefaults=textarea.defaultValue==='before\\r\\nafter'"
        "&&textarea.value==='before\\nafter';textarea.value='dirty';"
        "textarea.textContent='new default';"
        "const textareaDirty=textarea.defaultValue==='new default'"
        "&&textarea.value==='dirty';"
        "const input=document.createElement('input');input.type='text';"
        "input.value='a\\r\\nb';const inputText=input.value==='ab';"
        "input.type='checkbox';input.removeAttribute('value');"
        "const checkboxDefault=input.value==='on'&&!input.indeterminate;"
        "const select=document.createElement('select'),first=new Option(' one '),"
        "second=new Option('two');select.append(first,second);"
        "const selected=select.selectedOptions,liveBefore=selected.length===1"
        "&&selected[0]===first&&first.value==='one';second.selected=true;"
        "const liveAfter=selected.length===1&&selected[0]===second"
        "&&!second.hasAttribute('selected');"
        "const selectorStyle=document.createElement('style');"
        "document.head.append(selectorStyle);const selectorSheet=selectorStyle.sheet;"
        "selectorSheet.insertRule(':is(main, article) > p{}');"
        "const selectorSupports=CSS.supports("
        "'selector(:where(main, article) > p)'),selectorSerialized="
        "selectorSheet.cssRules[0].selectorText===':is(main, article) > p',"
        "selectors=selectorSupports&&selectorSerialized;"
        "const text=document.createTextNode('🌠test');"
        "text.insertData(2,'-');text.replaceData(3,2,'OK');"
        "const characterData=text.data==='🌠-OKst'&&text.length===7"
        "&&text.substringData(0,2)==='🌠';"
        "let characterDataException=false;try{text.deleteData(8,1)}"
        "catch(error){characterDataException=error instanceof DOMException"
        "&&error.name==='IndexSizeError'}"
        "const adjacentHost=document.createElement('div'),"
        "adjacentChild=document.createElement('span');"
        "adjacentHost.insertAdjacentElement('afterbegin',adjacentChild);"
        "adjacentChild.insertAdjacentText('afterend','tail');"
        "const adjacent=adjacentHost.firstChild===adjacentChild"
        "&&adjacentHost.lastChild.textContent==='tail';"
        "const cssTextHost=document.getElementById('css-text-host');"
        "cssTextHost.style.cssText='--space:pre;white-space:var(--space)';"
        "const cssTextValue=cssTextHost.style.cssText.includes('--space:pre'),"
        "emptyStyle=document.createElement('div');"
        "emptyStyle.style.cssText='color:red';"
        "emptyStyle.style.removeProperty('color');"
        "const emptyStyleAfterRemove=emptyStyle.hasAttribute('style')"
        "&&emptyStyle.getAttribute('style')===''&&emptyStyle.matches('[style=\"\"]');"
        "emptyStyle.style.cssText='color:blue';emptyStyle.style.cssText='';"
        "const emptyStyleAfterCssText=emptyStyle.hasAttribute('style')"
        "&&emptyStyle.getAttribute('style')==='';"
        "emptyStyle.removeAttribute('style');"
        "const explicitStyleRemoval=!emptyStyle.hasAttribute('style'),"
        "reservedCustomProperty=!CSS.supports('--','initial'),"
        "supportsGrid=CSS.supports('display','grid'),"
        "supportsSubgrid=!CSS.supports('display','subgrid'),"
        "supportsQuery=CSS.supports("
        "'(display:grid) and (not (display:subgrid))'),"
        "supportsValues=supportsGrid&&supportsSubgrid&&supportsQuery,"
        "cssText=cssTextValue&&reservedCustomProperty&&supportsValues;"
        "globalThis.pocSummary=textareaDefaults&&textareaDirty&&inputText"
        "&&checkboxDefault&&liveBefore&&liveAfter&&selectors&&characterData"
        "&&characterDataException&&adjacent&&cssText"
        "&&emptyStyleAfterRemove&&emptyStyleAfterCssText&&explicitStyleRemoval"
        "&&DOMException.SYNTAX_ERR===12?'wpt-compat:ok':'wpt-compat:'"
        "+[textareaDefaults,textareaDirty,inputText,checkboxDefault,"
        "liveBefore,liveAfter,selectors,characterData,"
        "characterDataException,adjacent,cssText,"
        "emptyStyleAfterRemove,emptyStyleAfterCssText,explicitStyleRemoval,"
        "DOMException.SYNTAX_ERR===12,"
        "selectorSupports,selectorSerialized,cssTextValue,supportsValues,"
        "supportsGrid,supportsSubgrid,supportsQuery,"
        "reservedCustomProperty]."
        "map(Number).join('');"
        "</script></body>";
    NavigationSession wpt_compatibility_navigation;
    CHECK(navigation_init(&wpt_compatibility_navigation, &budget, 2));
    navigation_enable_scripts(&wpt_compatibility_navigation, 4 * MIB, 1000);
    uint64_t wpt_compatibility_generation =
        navigation_begin(&wpt_compatibility_navigation);
    bool wpt_compatibility_committed = navigation_commit_html(
        &wpt_compatibility_navigation, wpt_compatibility_generation,
        "https://wpt-compatibility.test/", wpt_compatibility_page,
        strlen(wpt_compatibility_page), 480, NULL, NULL, true);
    if (!wpt_compatibility_committed
        || strcmp(wpt_compatibility_navigation.page.script_result.summary,
                  "wpt-compat:ok") != 0) {
        fprintf(stderr, "wpt compatibility committed=%d summary=%s error=%s\n",
                wpt_compatibility_committed,
                wpt_compatibility_navigation.page.script_result.summary,
                wpt_compatibility_navigation.page.script_result.error);
    }
    CHECK(wpt_compatibility_committed
          && strcmp(
                 wpt_compatibility_navigation.page.script_result.summary,
                 "wpt-compat:ok") == 0);
    navigation_destroy(&wpt_compatibility_navigation);
    CHECK(budget.current == 0);

    puts("test: bounded file, body, beacon, and stream APIs");
    static const char browser_io_page[] =
        "<!doctype html><body><script>"
        "const file=new File(new Set(['tile','finch']),'bird/name.txt',"
        "{type:'TEXT/PLAIN',lastModified:123}),slice=file.slice(4,9),"
        "fileInput=document.createElement('input');fileInput.type='file';"
        "const fileShape=file instanceof Blob&&file.name==='bird:name.txt'"
        "&&file.type==='text/plain'&&file.lastModified===123"
        "&&slice.size===5&&fileInput.files instanceof FileList"
        "&&fileInput.files.length===0&&fileInput.files.item(0)===null;"
        "const readerResult=new Promise(resolve=>{const reader=new FileReader;"
        "let events='';for(const name of ['loadstart','progress','load','loadend'])"
        "reader.addEventListener(name,()=>events+=name+',');"
        "reader.onload=()=>resolve(reader.result==='tilefinch'"
        "&&events==='loadstart,progress,load,');reader.readAsText(file);});"
        "const streamResult=(async()=>{"
        "const source=new ReadableStream({start(c){c.enqueue('one');"
        "c.enqueue('two');c.close();}}),branches=source.tee(),"
        "left=branches[0].getReader(),right=branches[1].getReader();"
        "const l1=await left.read(),r1=await right.read();"
        "const transformed=new TransformStream({transform(value,c){"
        "c.enqueue(String(value).toUpperCase());}}),writer=transformed.writable."
        "getWriter(),output=transformed.readable.getReader();"
        "await writer.write('psp');const transformedValue=await output.read();"
        "await writer.close();const transformedEnd=await output.read();"
        "const response=new Response(new ReadableStream({start(c){"
        "c.enqueue(new TextEncoder().encode('a=1&b=two'));c.close();}}),"
        "{headers:{'content-type':'application/x-www-form-urlencoded'}}),"
        "data=await response.formData();"
        "return l1.value==='one'&&r1.value==='one'"
        "&&transformedValue.value==='PSP'&&transformedEnd.done"
        "&&data.get('a')==='1'&&data.get('b')==='two';})();"
        "const request=new Request('/submit',{method:'POST',"
        "body:new URLSearchParams({q:'bird'}),cache:'no-store',"
        "redirect:'manual',keepalive:true}),clone=request.clone(),"
        "response=Response.json({ok:true},{status:201}),"
        "redirect=Response.redirect('/next',303),"
        "bodyShape=request.body instanceof ReadableStream"
        "&&request.cache==='no-store'&&request.redirect==='manual'"
        "&&request.keepalive&&clone.method==='POST'"
        "&&response.status===201&&response.headers.get('content-type')"
        "==='application/json'&&redirect.status===303"
        "&&redirect.headers.get('location').endsWith('/next');"
        "const beaconShape=typeof navigator.sendBeacon==='function'"
        "&&!navigator.sendBeacon('/too-large',new Blob([new Uint8Array(65537)]));"
        "Promise.all([readerResult,streamResult,response.json(),"
        "clone.text()]).then(values=>{globalThis.pocSummary="
        "fileShape&&bodyShape&&beaconShape&&values[0]&&values[1]"
        "&&values[2].ok&&values[3]==='q=bird'?'browser-io:ok':"
        "'browser-io:fail';}).catch(error=>globalThis.pocSummary="
        "'browser-io:error:'+error);"
        "</script></body>";
    NavigationSession browser_io_navigation;
    CHECK(navigation_init(&browser_io_navigation, &budget, 2));
    navigation_enable_scripts(&browser_io_navigation, 5 * MIB, 1000);
    uint64_t browser_io_generation =
        navigation_begin(&browser_io_navigation);
    CHECK(navigation_commit_html(
        &browser_io_navigation, browser_io_generation,
        "https://browser-io.test/", browser_io_page,
        strlen(browser_io_page), 480, NULL, NULL, true));
    for (size_t tick = 0;
         tick < 16
         && strcmp(browser_io_navigation.page.script_result.summary,
                   "browser-io:ok") != 0;
         tick++)
        CHECK(navigation_advance_runtime(&browser_io_navigation, 1, 8));
    if (strcmp(browser_io_navigation.page.script_result.summary,
               "browser-io:ok") != 0)
        fprintf(stderr, "browser io summary=%s error=%s\n",
                browser_io_navigation.page.script_result.summary,
                browser_io_navigation.page.script_result.error);
    CHECK(strcmp(browser_io_navigation.page.script_result.summary,
                 "browser-io:ok") == 0);
    navigation_destroy(&browser_io_navigation);
    CHECK(budget.current == 0);

    puts("test: bounded Canvas 2D paths, transforms, pixels, and PNG");
    static const char canvas_page[] =
        "<!doctype html><body><script>"
        "const canvas=document.createElement('canvas');canvas.width=32;"
        "canvas.height=16;const context=canvas.getContext('2d');"
        "context.fillStyle='#ff0000';context.fillRect(0,0,8,8);"
        "context.save();context.translate(8,0);context.fillStyle='#0000ff';"
        "context.fillRect(0,0,8,8);context.restore();"
        "const path=new Path2D;path.moveTo(16,0);path.lineTo(24,8);"
        "path.lineTo(16,8);path.closePath();context.fillStyle='#00ff00';"
        "context.fill(path);context.strokeStyle='#ffffff';context.lineWidth=1;"
        "context.stroke(path);context.setLineDash([2,1]);"
        "const gradient=context.createLinearGradient(0,8,8,8);"
        "gradient.addColorStop(0,'#000000');gradient.addColorStop(1,'#ffffff');"
        "context.fillStyle=gradient;context.fillRect(0,8,8,8);"
        "context.beginPath();context.moveTo(8,12);"
        "context.quadraticCurveTo(12,8,16,12);"
        "context.bezierCurveTo(18,14,20,10,22,12);context.stroke();"
        "context.beginPath();context.ellipse(27,12,3,2,0,0,Math.PI*2);"
        "context.stroke();"
        "const matrix=context.getTransform(),red=context.getImageData(1,1,1,1),"
        "blue=context.getImageData(9,1,1,1),inside=context.isPointInPath(path,18,4),"
        "gradientLeft=context.getImageData(0,9,1,1),"
        "gradientRight=context.getImageData(7,9,1,1),"
        "metrics=context.measureText('PSP'),copy=document.createElement('canvas');"
        "copy.width=32;copy.height=16;copy.getContext('2d').drawImage(canvas,0,0);"
        "const copied=copy.getContext('2d').getImageData(1,1,1,1),"
        "dataURL=canvas.toDataURL(),png=atob(dataURL.split(',')[1]),"
        "syncOk=context.canvas===canvas&&matrix.a===1&&matrix.e===0"
        "&&new DOMMatrix({a:2,b:0,c:0,d:3,e:4,f:5}).f===5"
        "&&context.getLineDash().join(',')==='2,1'&&inside&&metrics.width>0"
        "&&gradient instanceof CanvasGradient"
        "&&gradientRight.data[0]>gradientLeft.data[0]"
        "&&red.data.join(',')==='255,0,0,255'"
        "&&blue.data.join(',')==='0,0,255,255'"
        "&&copied.data.join(',')==='255,0,0,255'"
        "&&dataURL.startsWith('data:image/png;base64,')"
        "&&png.charCodeAt(0)===137&&png.slice(1,4)==='PNG';"
        "canvas.toBlob(blob=>blob.arrayBuffer().then(buffer=>{"
        "const bytes=new Uint8Array(buffer);globalThis.pocSummary="
        "syncOk&&blob.type==='image/png'&&bytes[0]===137?"
        "'canvas:ok':'canvas:fail';}));"
        "</script></body>";
    NavigationSession canvas_navigation;
    CHECK(navigation_init(&canvas_navigation, &budget, 2));
    navigation_enable_scripts(&canvas_navigation, 6 * MIB, 1000);
    uint64_t canvas_generation = navigation_begin(&canvas_navigation);
    CHECK(navigation_commit_html(
        &canvas_navigation, canvas_generation,
        "https://canvas.test/", canvas_page, strlen(canvas_page),
        480, NULL, NULL, true));
    for (size_t tick = 0;
         tick < 16
         && strcmp(canvas_navigation.page.script_result.summary,
                   "canvas:ok") != 0;
         tick++)
        CHECK(navigation_advance_runtime(&canvas_navigation, 1, 8));
    if (strcmp(canvas_navigation.page.script_result.summary,
               "canvas:ok") != 0)
        fprintf(stderr, "canvas summary=%s error=%s\n",
                canvas_navigation.page.script_result.summary,
                canvas_navigation.page.script_result.error);
    CHECK(strcmp(canvas_navigation.page.script_result.summary,
                 "canvas:ok") == 0
          && canvas_navigation.page.script_result
                 .bootstrap_lazy_module_loads == 1
          && canvas_navigation.page.script_result
                 .bootstrap_lazy_module_failures == 0);
    navigation_destroy(&canvas_navigation);
    CHECK(budget.current == 0);

    puts("test: bounded native input widgets and controller defaults");
    static const char native_controls_page[] =
        "<!doctype html><body><form id=form action=/submit method=post>"
        "<input id=check name=check type=checkbox>"
        "<input id=radio name=mode type=radio value=a>"
        "<input id=slider name=slider type=range min=10 max=20 step=2 value=14>"
        "<input id=count name=count type=number min=1 max=9 step=2 value=1>"
        "<select id=choice name=choice><option value=a selected>A</option>"
        "<option value=b>B</option></select>"
        "<input id=send name=send type=submit value=SEND>"
        "<input id=reset type=reset value=RESET>"
        "<input id=plain value=text></form><script>"
        "const count=document.getElementById('count'),"
        "slider=document.getElementById('slider'),"
        "reset=document.getElementById('reset'),"
        "plain=document.getElementById('plain');"
        "count.stepUp(2);const up=count.value==='5';"
        "count.stepDown();const down=count.value==='3';"
        "slider.stepUp();slider.stepDown();"
        "const fractional=document.createElement('input');"
        "fractional.type='number';fractional.value='0.1';fractional.step='0.1';"
        "fractional.stepUp(2);const exact=fractional.value==='0.3';"
        "const offStep=document.createElement('input');offStep.type='number';"
        "offStep.min='0';offStep.step='2';offStep.value='3';offStep.stepUp();"
        "const snapped=offStep.value==='4';"
        "let invalid=false;try{plain.stepUp()}catch(error){invalid="
        "error instanceof DOMException&&error.name==='InvalidStateError'}"
        "count.value='7';reset.click();const resetOk=count.value==='1';"
        "globalThis.pocSummary=up&&down&&slider.value==='14'&&exact&&snapped"
        "&&invalid&&resetOk"
        "?'native-controls:ok':'native-controls:fail';"
        "</script></body>";
    NavigationSession native_controls_navigation;
    CHECK(navigation_init(&native_controls_navigation, &budget, 2));
    navigation_enable_scripts(&native_controls_navigation, 4 * MIB, 1000);
    uint64_t native_controls_generation =
        navigation_begin(&native_controls_navigation);
    CHECK(navigation_commit_html(
              &native_controls_navigation, native_controls_generation,
              "https://native-controls.test/", native_controls_page,
              strlen(native_controls_page), 480, NULL, NULL, true)
          && strcmp(native_controls_navigation.page.script_result.summary,
                    "native-controls:ok") == 0);
    lxb_dom_node_t *native_controls_root = lxb_dom_interface_node(
        native_controls_navigation.page.document.html);
    lxb_dom_node_t *native_check = find_id(native_controls_root, "check");
    lxb_dom_node_t *native_slider = find_id(native_controls_root, "slider");
    lxb_dom_node_t *native_count = find_id(native_controls_root, "count");
    lxb_dom_node_t *native_choice = find_id(native_controls_root, "choice");
    lxb_dom_node_t *native_send = find_id(native_controls_root, "send");
    lxb_dom_node_t *native_reset = find_id(native_controls_root, "reset");
    const ControlRegion *check_region = NULL, *slider_region = NULL;
    const ControlRegion *count_region = NULL, *choice_region = NULL;
    const ControlRegion *send_region = NULL, *reset_region = NULL;
    for (size_t i = 0;
         i < native_controls_navigation.page.layout.control_count; i++) {
        const ControlRegion *region =
            &native_controls_navigation.page.layout.controls[i];
        if (region->node == native_check) check_region = region;
        else if (region->node == native_slider) slider_region = region;
        else if (region->node == native_count) count_region = region;
        else if (region->node == native_choice) choice_region = region;
        else if (region->node == native_send) send_region = region;
        else if (region->node == native_reset) reset_region = region;
    }
    if (check_region == NULL || slider_region == NULL || count_region == NULL
        || choice_region == NULL || send_region == NULL
        || reset_region == NULL
        || check_region->type != CONTROL_TOGGLE
        || slider_region->type != CONTROL_RANGE
        || count_region->type != CONTROL_INPUT
        || choice_region->type != CONTROL_SELECT
        || send_region->type != CONTROL_BUTTON
        || reset_region->type != CONTROL_BUTTON
        || check_region->width < 18 || check_region->width > 32
        || check_region->height < 18 || check_region->height > 32
        || slider_region->width != 160
        || slider_region->height < 20 || slider_region->height > 32
        || send_region->width != 44) {
        fprintf(
            stderr,
            "native regions check=%p:%d:%dx%d slider=%p:%d:%dx%d "
            "count=%p:%d choice=%p:%d send=%p:%d:%dx%d reset=%p:%d\n",
            (void *) check_region,
            check_region == NULL ? -1 : (int) check_region->type,
            check_region == NULL ? -1 : check_region->width,
            check_region == NULL ? -1 : check_region->height,
            (void *) slider_region,
            slider_region == NULL ? -1 : (int) slider_region->type,
            slider_region == NULL ? -1 : slider_region->width,
            slider_region == NULL ? -1 : slider_region->height,
            (void *) count_region,
            count_region == NULL ? -1 : (int) count_region->type,
            (void *) choice_region,
            choice_region == NULL ? -1 : (int) choice_region->type,
            (void *) send_region,
            send_region == NULL ? -1 : (int) send_region->type,
            send_region == NULL ? -1 : send_region->width,
            send_region == NULL ? -1 : send_region->height,
            (void *) reset_region,
            reset_region == NULL ? -1 : (int) reset_region->type);
    }
    CHECK(check_region != NULL && check_region->type == CONTROL_TOGGLE
          && check_region->width >= 18 && check_region->width <= 32
          && check_region->height >= 18 && check_region->height <= 32
          && slider_region != NULL && slider_region->type == CONTROL_RANGE
          && slider_region->width == 160
          && slider_region->height >= 20 && slider_region->height <= 32
          && count_region != NULL && count_region->type == CONTROL_INPUT
          && choice_region != NULL && choice_region->type == CONTROL_SELECT
          && send_region != NULL && send_region->type == CONTROL_BUTTON
          && send_region->width == 44
          && reset_region != NULL && reset_region->type == CONTROL_BUTTON);
    bool toggle_stroke = false;
    for (size_t i = 0;
         i < native_controls_navigation.page.layout.count; i++) {
        const DrawCommand *command =
            &native_controls_navigation.page.layout.commands[i];
        if (command->type == DRAW_STROKE_RECT
            && command->x >= check_region->x
            && command->x + command->width
                   <= check_region->x + check_region->width
            && command->y >= check_region->y
            && command->y + command->height
                   <= check_region->y + check_region->height) {
            toggle_stroke = true;
            break;
        }
    }
    CHECK(toggle_stroke);
    BrowserController native_controls_controller;
    ControllerAction native_controls_action;
    ControllerTextInputInfo native_controls_text = {0};
    CHECK(controller_init(
              &native_controls_controller, &native_controls_navigation)
          && controller_focus_node(
              &native_controls_controller, native_check)
          && !controller_text_input_info(
              &native_controls_controller, &native_controls_text)
          && controller_activate(
              &native_controls_controller, &native_controls_action)
          && native_controls_action.type == CONTROLLER_ACTION_CONTROL);
    CHECK(lxb_dom_element_has_attribute(
              lxb_dom_interface_element(native_check),
              (const lxb_char_t *) "checked", 7));
    CHECK(controller_focus_node(
              &native_controls_controller, native_slider)
          && !controller_text_input_info(
              &native_controls_controller, &native_controls_text)
          && controller_activate(
              &native_controls_controller, &native_controls_action));
    size_t native_value_length = 0;
    const char *native_value =
        document_control_value(native_slider, &native_value_length);
    CHECK(native_value != NULL && native_value_length == 2
          && memcmp(native_value, "16", 2) == 0);
    CHECK(controller_focus_node(
              &native_controls_controller, native_choice)
          && controller_activate(
              &native_controls_controller, &native_controls_action));
    ScriptResult native_controls_result = {0};
    CHECK(script_runtime_evaluate_diagnostic(
              native_controls_navigation.page.runtime,
              "globalThis.pocSummary=document.getElementById('choice').value",
              "<native-select-value>", &native_controls_result)
          && strcmp(native_controls_result.summary, "b") == 0);
    bool native_send_focused = controller_focus_node(
        &native_controls_controller, native_send);
    bool native_send_editable = controller_text_input_info(
        &native_controls_controller, &native_controls_text);
    bool native_send_activated = native_send_focused
        && controller_activate(
            &native_controls_controller, &native_controls_action);
    if (!native_send_focused || native_send_editable
        || !native_send_activated
        || native_controls_action.type != CONTROLLER_ACTION_FORM_SUBMIT
        || strcmp(native_controls_action.body,
                  "check=on&slider=16&count=1&choice=b&send=SEND") != 0) {
        fprintf(
            stderr,
            "native submit focus=%d editable=%d activate=%d type=%d body=%s\n",
            native_send_focused, native_send_editable,
            native_send_activated, (int) native_controls_action.type,
            native_controls_action.body);
    }
    CHECK(native_send_focused && !native_send_editable
          && native_send_activated
          && native_controls_action.type == CONTROLLER_ACTION_FORM_SUBMIT
          && strcmp(native_controls_action.body,
                    "check=on&slider=16&count=1&choice=b&send=SEND") == 0);
    CHECK(controller_focus_node(
              &native_controls_controller, native_reset)
          && controller_activate(
              &native_controls_controller, &native_controls_action)
          && native_controls_action.type == CONTROLLER_ACTION_CONTROL);
    native_value = document_control_value(
        native_slider, &native_value_length);
    if (native_value == NULL || native_value_length != 2
        || memcmp(native_value, "14", 2) != 0
        || lxb_dom_element_has_attribute(
            lxb_dom_interface_element(native_check),
            (const lxb_char_t *) "checked", 7)) {
        fprintf(
            stderr, "native reset value=%.*s length=%zu checked=%d\n",
            (int) native_value_length,
            native_value == NULL ? "" : native_value,
            native_value_length,
            lxb_dom_element_has_attribute(
                lxb_dom_interface_element(native_check),
                (const lxb_char_t *) "checked", 7));
    }
    CHECK(native_value != NULL && native_value_length == 2
          && memcmp(native_value, "14", 2) == 0
          && !lxb_dom_element_has_attribute(
              lxb_dom_interface_element(native_check),
              (const lxb_char_t *) "checked", 7));
    navigation_destroy(&native_controls_navigation);
    CHECK(budget.current == 0);

    puts("test: parser blocking, defer, module, and DOMContentLoaded order");
    static const char ordered_scripts_page[] =
        "<!doctype html><title>Order</title><link rel=stylesheet href=/theme.css integrity='sha256-JYeLaMI2g2JUyAMRFmomzPdWGigJn1ucaCO79KhxyU8='><body><p id=probe>ORDER</p>"
        "<script>globalThis.order=['blocking-1'];globalThis.computedOk=getComputedStyle(document.getElementById('probe')).display==='grid';</script>"
        "<script type=module>globalThis.order.push('module');</script>"
        "<script>order.push('blocking-2');const dynamic=document.createElement('script');dynamic.textContent=\"order.push('dynamic')\";document.head.appendChild(dynamic);const dynamicExternal=document.createElement('script');dynamicExternal.id='integrity-probe';dynamicExternal.src='/dynamic.js';dynamicExternal.integrity='sha256-COznQ5ZviU9B5EACPJMioIittuLAymlK0EjIp6rJV1g=';dynamicExternal.addEventListener('load',()=>{order.push('dynamic-load');globalThis.pocSummary=order.join(',')+':'+computedOk});document.head.appendChild(dynamicExternal);const dynamicStyle=document.createElement('style');dynamicStyle.textContent='#probe{color:#123456}';document.head.appendChild(dynamicStyle);const dynamicLink=document.createElement('link');dynamicLink.rel='stylesheet';dynamicLink.href='/dynamic.css';dynamicLink.integrity='sha256-W127z64XoV2/VrojvPV5g6AvZ+0tpirAWF2hhaS65ac=';dynamicLink.addEventListener('load',()=>{order.push('dynamic-style-load');globalThis.pocSummary=order.join(',')+':'+computedOk});document.head.appendChild(dynamicLink);const delayedFrame=document.createElement('iframe');setTimeout(()=>document.body.appendChild(delayedFrame),0);document.addEventListener('DOMContentLoaded',()=>globalThis.pocSummary=order.join(',')+':'+computedOk);</script>"
        "</body>";
    static const unsigned char ordered_css[] = "#probe{display:grid}";
    static const unsigned char ordered_dynamic_css[] =
        "#probe{font-weight:700}";
    static const unsigned char ordered_dynamic_script[] =
        "order.push('dynamic-external')";
    BrowserSession ordered_browser_session;
    CHECK(browser_session_init(&ordered_browser_session, &budget,
                               128 * 1024)
          && test_cache_put_stylesheet(
              &ordered_browser_session, "https://order.test/theme.css",
              ordered_css, sizeof(ordered_css) - 1, "ordered-v1")
          && test_cache_put_stylesheet(
              &ordered_browser_session, "https://order.test/dynamic.css",
              ordered_dynamic_css, sizeof(ordered_dynamic_css) - 1,
              "ordered-dynamic-v1")
          && test_cache_put_classic_script(
              &ordered_browser_session, "https://order.test/",
              "https://order.test/dynamic.js", ordered_dynamic_script,
              sizeof(ordered_dynamic_script) - 1));
    NavigationSession ordered_navigation;
    CHECK(navigation_init(&ordered_navigation, &budget, 4));
    navigation_attach_browser_session(&ordered_navigation,
                                      &ordered_browser_session);
    navigation_enable_scripts(&ordered_navigation, 4 * MIB, 1000);
    navigation_enable_document_scripts(&ordered_navigation, 8, 64 * 1024,
                                       32 * 1024, 1000);
    navigation_enable_external_resources(
        &ordered_navigation, 2, 64 * 1024, 32 * 1024,
        2, 64 * 1024, 32 * 1024, 128 * 1024, 1000);
    uint64_t ordered_generation = navigation_begin(&ordered_navigation);
    bool ordered_committed = navigation_commit_html(
        &ordered_navigation, ordered_generation,
        "https://order.test/", ordered_scripts_page,
        strlen(ordered_scripts_page), 480, NULL, NULL, true);
    CHECK(ordered_committed
          && ordered_navigation.script_discovered == 5
          && ordered_navigation.script_parser_blocking == 2
          && ordered_navigation.script_deferred == 1
          && ordered_navigation.script_asynchronous == 0
          && ordered_navigation.script_modules == 1
          && ordered_navigation.page.script_result.scripts_evaluated == 4
          && ordered_navigation.page.script_result.dynamic_scripts_queued == 1
          && ordered_navigation.page.script_result.dynamic_scripts_started == 0
          && ordered_navigation.page.script_result.dynamic_scripts_completed == 0
          && ordered_navigation.page.script_result.pending_tasks == 2
          && ordered_navigation.page.script_result.dom_content_loaded_dispatched
          && ordered_navigation.page.external_stylesheets.loaded == 2
          && ordered_navigation.page.external_stylesheets.cache_hits == 2
          && ordered_navigation.page.external_stylesheets.retained_body_hits
                 == 1
          && ordered_browser_session.cache_hits == 2
          && strcmp(ordered_navigation.page.script_result.summary,
                    "blocking-1,blocking-2,dynamic,module,"
                    "dynamic-style-load:true") == 0);
    lxb_dom_node_t *ordered_probe = find_id(
        lxb_dom_interface_node(ordered_navigation.page.document.html),
        "probe");
    ComputedStyle ordered_probe_style = style_for_node(
        &ordered_navigation.page.stylesheet, ordered_probe, NULL);
    CHECK(ordered_probe != NULL && ordered_probe_style.color == 0x123456);
    CHECK(ordered_navigation.frames_discovered == 0
          && navigation_advance_runtime(&ordered_navigation, 1, 4)
          && ordered_navigation.page.script_result.dynamic_scripts_started == 1
          && ordered_navigation.page.script_result.dynamic_scripts_completed == 1
          && ordered_navigation.page.script_result.dynamic_scripts_cache_hits == 1
          && ordered_navigation.page.script_result.pending_tasks == 0
          && ordered_navigation.page.script_result.runtime_ticks == 1
          && ordered_navigation.page.script_result.timer_callbacks_run == 1
          && ordered_browser_session.cache_hits == 3
          && strcmp(ordered_navigation.page.script_result.summary,
                    "blocking-1,blocking-2,dynamic,module,"
                    "dynamic-style-load,dynamic-external,dynamic-load:true")
                 == 0
          /* A missing-src iframe is the local about:blank document, not a
             failed network frame. */
          && ordered_navigation.frames_discovered == 0
          && ordered_navigation.frames_failed == 0
          && ordered_navigation.page.frame_count == 0);
    lxb_dom_node_t *integrity_probe = find_id(
        lxb_dom_interface_node(ordered_navigation.page.document.html),
        "integrity-probe");
    static const char invalid_integrity[] =
        "sha256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";
    CHECK(integrity_probe != NULL
          && lxb_dom_element_set_attribute(
                 lxb_dom_interface_element(integrity_probe),
                 (const lxb_char_t *) "integrity", 9,
                 (const lxb_char_t *) invalid_integrity,
                 sizeof(invalid_integrity) - 1) != NULL
          && !navigation_evaluate_external_script(
                 &ordered_navigation, integrity_probe,
                 "globalThis.sriPoison=true", 25,
                 "https://order.test/integrity-probe.js")
          && script_runtime_evaluate_diagnostic(
                 ordered_navigation.page.runtime,
                 "globalThis.pocSummary=typeof sriPoison",
                 "https://order.test/integrity-result.js",
                 &ordered_navigation.page.script_result)
          && strcmp(ordered_navigation.page.script_result.summary,
                    "undefined") == 0);
    navigation_destroy(&ordered_navigation);
    browser_session_destroy(&ordered_browser_session);
    CHECK(budget.current == 0);

    puts("test: detached iframe retires runtime, fetch, and messages");
    static const char owned_frame_page[] =
        "<!doctype html><body><iframe id='owned' src='/child.html'>"
        "</iframe></body>";
    static const char owned_child_page[] =
        "<!doctype html><body><script>globalThis.pocSummary='child-ready'"
        "</script></body>";
    BrowserSession frame_owner_session;
    NavigationSession frame_owner_navigation;
    CHECK(browser_session_init(&frame_owner_session, &budget, 64 * 1024)
          && navigation_init(&frame_owner_navigation, &budget, 2));
    navigation_attach_browser_session(&frame_owner_navigation,
                                      &frame_owner_session);
    navigation_enable_scripts(&frame_owner_navigation, 4 * MIB, 1000);
    navigation_enable_document_scripts(&frame_owner_navigation, 4,
                                       64 * 1024, 64 * 1024, 1000);
    uint64_t frame_owner_generation =
        navigation_begin(&frame_owner_navigation);
    CHECK(navigation_commit_html(
        &frame_owner_navigation, frame_owner_generation,
        "https://frame-owner.test/", owned_frame_page,
        strlen(owned_frame_page), 480, NULL, NULL, true));
    for (size_t tick = 0;
         tick < 8 && frame_owner_navigation.frames_loaded == 0; tick++) {
        CHECK(navigation_advance_runtime(&frame_owner_navigation, 1, 8));
    }
    /* The unit fetch backend deliberately has no network response path for
       frames. Install a real child realm in the discovered ownership slot so
       this test stays hermetic while exercising navigation's teardown seam. */
    CHECK(frame_owner_navigation.frames_discovered == 1
          && frame_owner_navigation.frames_failed == 1
          && frame_owner_navigation.page.frame_count == 1);
    NavigationFrame *owned_frame = &frame_owner_navigation.page.frames[0];
    MessageOpaqueProbe owned_frame_probe = {0};
    CHECK(document_parse(&owned_frame->document, &budget,
                         owned_child_page, sizeof(owned_child_page) - 1, 16));
    owned_frame->runtime = script_runtime_create_deferred(
        &owned_frame->document, &budget, 2 * MIB, 1000,
        "https://frame-owner.test/child.html", NULL,
        &owned_frame->script_result);
    CHECK(owned_frame->runtime != NULL
          && script_runtime_configure_messaging(
              owned_frame->runtime, true, true, false, count_test_message,
              &owned_frame_probe));
    CHECK(script_runtime_evaluate_diagnostic(
              owned_frame->runtime,
              "const original=parent.postMessage;parent.postMessage=()=>{};"
              "globalThis.pocSummary=parent.postMessage===original?"
              "'same-origin-blocked':'same-origin-writable'",
              "https://frame-owner.test/same-origin-window.js",
              &owned_frame->script_result)
          && strcmp(owned_frame->script_result.summary,
                    "same-origin-writable") == 0);
    owned_frame->loaded = true;
    static const char failed_child_json[] = "{\"event\":\"failed-child\"}";
    static const char queued_origin[] = "*";
    CHECK(script_runtime_evaluate_diagnostic(
        owned_frame->runtime,
        "globalThis.__tilefinchReceiveMessage=()=>{throw new Error('failed realm')}",
        "https://frame-owner.test/fail-message.js",
        &owned_frame->script_result));
    NavigationMessage *failed_child_message =
        &frame_owner_navigation.page.messages[0];
    failed_child_message->json = budget_malloc_category(
        &budget, BUDGET_CATEGORY_NAVIGATION, sizeof(failed_child_json));
    failed_child_message->target_origin = budget_malloc_category(
        &budget, BUDGET_CATEGORY_NAVIGATION, sizeof(queued_origin));
    CHECK(failed_child_message->json != NULL
          && failed_child_message->target_origin != NULL);
    memcpy(failed_child_message->json, failed_child_json,
           sizeof(failed_child_json));
    memcpy(failed_child_message->target_origin, queued_origin,
           sizeof(queued_origin));
    failed_child_message->source = frame_owner_navigation.page.runtime;
    failed_child_message->target_frame_handle = owned_frame->parent_handle;
    failed_child_message->target_generation =
        owned_frame->lifecycle_generation;
    snprintf(failed_child_message->source_origin,
             sizeof(failed_child_message->source_origin),
             "https://frame-owner.test");
    failed_child_message->sequence = 1;
    frame_owner_navigation.page.message_count = 1;
    /* Author replacement of a compatibility global must not intercept the
       native scheduler's retained bootstrap callback. */
    CHECK(navigation_advance_runtime(&frame_owner_navigation, 1, 8)
          && frame_owner_navigation.frames_failed == 1
          && frame_owner_navigation.frame_messages_dropped == 0
          && frame_owner_navigation.page.message_count == 0
          && frame_owner_navigation.page.runtime != NULL
          && owned_frame->runtime != NULL
          && owned_frame->document.html != NULL);
    script_runtime_destroy(owned_frame->runtime);
    owned_frame->runtime = NULL;
    document_destroy(&owned_frame->document);
    owned_frame->loaded = false;
    memset(&owned_frame->script_result, 0, sizeof(owned_frame->script_result));
    CHECK(document_parse(&owned_frame->document, &budget,
                         owned_child_page, sizeof(owned_child_page) - 1, 16));
    owned_frame->runtime = script_runtime_create_deferred(
        &owned_frame->document, &budget, 2 * MIB, 1000,
        "https://frame-owner.test/replacement-child.html", NULL,
        &owned_frame->script_result);
    CHECK(owned_frame->runtime != NULL
          && script_runtime_configure_messaging(
              owned_frame->runtime, true, true, false, count_test_message,
              &owned_frame_probe));
    owned_frame->loaded = true;
    static const char queued_json[] = "{\"event\":\"queued-before-detach\"}";
    NavigationMessage *queued = &frame_owner_navigation.page.messages[0];
    queued->json = budget_malloc_category(
        &budget, BUDGET_CATEGORY_NAVIGATION, sizeof(queued_json));
    queued->target_origin = budget_malloc_category(
        &budget, BUDGET_CATEGORY_NAVIGATION, sizeof(queued_origin));
    CHECK(queued->json != NULL && queued->target_origin != NULL);
    memcpy(queued->json, queued_json, sizeof(queued_json));
    memcpy(queued->target_origin, queued_origin, sizeof(queued_origin));
    queued->source = owned_frame->runtime;
    queued->source_frame_handle = owned_frame->parent_handle;
    queued->source_generation = owned_frame->lifecycle_generation;
    snprintf(queued->source_origin, sizeof(queued->source_origin),
             "https://frame-owner.test");
    queued->sequence = 2;
    frame_owner_navigation.page.message_count = 1;
    CHECK(frame_owner_navigation.page.message_count == 1
          && script_runtime_evaluate_diagnostic(
              frame_owner_navigation.page.runtime,
              "globalThis.ownedFrame=document.getElementById('owned');"
              "globalThis.savedOwnedWindow=ownedFrame.contentWindow;"
              "ownedFrame.remove()",
              "https://frame-owner.test/remove-frame.js",
              &frame_owner_navigation.page.script_result)
          && navigation_advance_runtime(&frame_owner_navigation, 1, 8)
          && frame_owner_navigation.frames_detached == 1
          && frame_owner_navigation.frame_messages_dropped == 1
          && frame_owner_navigation.page.message_count == 0
          && owned_frame->retired && owned_frame->runtime == NULL
          && owned_frame->document.html == NULL
          && script_runtime_evaluate_diagnostic(
              frame_owner_navigation.page.runtime,
              "globalThis.pocSummary=savedOwnedWindow.closed?"
              "'FRAME-DETACHED-OK':'FRAME-DETACHED-FAILED'",
              "https://frame-owner.test/check-frame.js",
              &frame_owner_navigation.page.script_result)
          && strcmp(frame_owner_navigation.page.script_result.summary,
                    "FRAME-DETACHED-OK") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              frame_owner_navigation.page.runtime,
              "document.body.appendChild(ownedFrame)",
              "https://frame-owner.test/reattach-frame.js",
              &frame_owner_navigation.page.script_result)
          && navigation_advance_runtime(&frame_owner_navigation, 1, 8)
          && frame_owner_navigation.frames_discovered == 2
          && frame_owner_navigation.page.frame_count == 1
          && !owned_frame->retired
          && script_runtime_evaluate_diagnostic(
              frame_owner_navigation.page.runtime,
              "globalThis.pocSummary=ownedFrame.contentWindow"
              "===savedOwnedWindow&&!savedOwnedWindow.closed?"
              "'FRAME-REATTACHED-OK':'FRAME-REATTACHED-FAILED'",
              "https://frame-owner.test/check-reattach.js",
              &frame_owner_navigation.page.script_result)
          && strcmp(frame_owner_navigation.page.script_result.summary,
                    "FRAME-REATTACHED-OK") == 0);
    navigation_destroy(&frame_owner_navigation);
    browser_session_destroy(&frame_owner_session);
    CHECK(budget.current == 0);

    puts("test: script pressure admission skips before allocator failure");
    static const char pressure_script_page[] =
        "<!doctype html><title>Pressure</title><body><p>READY</p>"
        "<script>globalThis.inlinePressureScriptRan=true</script>"
        "<script src='/pressure.js' defer></script></body>";
    static const unsigned char pressure_script_source[] =
        "globalThis.pocSummary='pressure-script-ran'";
    BrowserSession pressure_browser_session;
    NavigationSession pressure_navigation;
    BudgetReservation pressure_reservation = {0};
    CHECK(browser_session_init(&pressure_browser_session, &budget,
                               64 * 1024)
          && test_cache_put_classic_script(
              &pressure_browser_session, "https://pressure.test/",
              "https://pressure.test/pressure.js", pressure_script_source,
              sizeof(pressure_script_source) - 1)
          && navigation_init(&pressure_navigation, &budget, 2));
    navigation_attach_browser_session(&pressure_navigation,
                                      &pressure_browser_session);
    navigation_enable_scripts(&pressure_navigation, 3 * MIB, 1000);
    navigation_enable_document_scripts(&pressure_navigation, 2,
                                       64 * 1024, 32 * 1024, 1000);
    pressure_navigation.defer_document_script_pipeline = true;
    uint64_t pressure_generation = navigation_begin(&pressure_navigation);
    CHECK(navigation_commit_html(
              &pressure_navigation, pressure_generation,
              "https://pressure.test/", pressure_script_page,
              strlen(pressure_script_page), 480, NULL, NULL, true)
          && pressure_navigation.script_discovered == 0);
    (void) script_runtime_collect_and_trim(pressure_navigation.page.runtime);
    size_t pressure_remaining = budget_remaining(&budget);
    CHECK(pressure_remaining > 2 * MIB
          && budget_reservation_acquire(
              &pressure_reservation, &budget, BUDGET_CATEGORY_RESOURCE,
              pressure_remaining - 256 * 1024));
    size_t pressure_failures_before = budget.failure_count;
    size_t pressure_decisions_before =
        budget.pressure[BUDGET_PRESSURE_JAVASCRIPT].decisions;
    bool pressure_executed = navigation_execute_document_scripts_from(
        &pressure_navigation, &pressure_navigation.page.document);
    bool pressure_diagnostic = pressure_executed
        && script_runtime_evaluate_diagnostic(
            pressure_navigation.page.runtime,
            "globalThis.pocSummary="
            "globalThis.inlinePressureScriptRan?"
            "'inline-pressure-ran':'inline-pressure-missing'",
            "https://pressure.test/verify-inline.js",
            &pressure_navigation.page.script_result);
    CHECK(pressure_diagnostic
          && pressure_navigation.script_discovered == 2
          && pressure_navigation.script_attempted == 0
          && pressure_navigation.script_loaded == 0
          && pressure_navigation.script_failed == 0
          && pressure_navigation.script_skipped_quota == 0
          && pressure_navigation.script_skipped_pressure == 2
          && pressure_navigation.script_pressure_collections == 1
          && budget.failure_count == pressure_failures_before
          && budget.pressure[BUDGET_PRESSURE_JAVASCRIPT].decisions
                 == pressure_decisions_before + 2
          && budget.pressure[BUDGET_PRESSURE_JAVASCRIPT].avoided_bytes
                 != 0
          && strcmp(pressure_navigation.page.script_result.summary,
                    "inline-pressure-missing") == 0);
    budget_reservation_release(&pressure_reservation);
    navigation_destroy(&pressure_navigation);
    browser_session_destroy(&pressure_browser_session);
    CHECK(budget.current == 0);

    puts("test: cached nested stylesheet imports and range media");
    static const char import_page[] =
        "<!doctype html><link rel=stylesheet href=/styles/root.css>"
        "<body><div id=import-card>IMPORT</div></body>";
    static const unsigned char import_root_css[] =
        "@import url('/import.css') screen and (400px <= width < 600px);"
        "@import '/print.css' print;"
        "@import '/never.css' supports(display: subgrid);"
        "#import-card{background:#dce9f7}";
    static const unsigned char import_child_css[] =
        "@import \"nested.css\";"
        "#import-card{border-left:3px solid #315780}";
    static const unsigned char import_nested_css[] =
        "@media (400px <= width < 600px) and (height >= 272px)"
        "{#import-card{color:#246813;background:#112233}}"
        "@media (width >= 700px){#import-card{display:none}}";
    PocDocument import_document;
    Stylesheet import_sheet;
    BrowserSession import_session;
    ExternalStylesheetStats import_stats;
    CHECK(document_parse(&import_document, &budget, import_page,
                         strlen(import_page), 17)
          && stylesheet_build(&import_sheet, &budget, &import_document, 480)
          && browser_session_init(&import_session, &budget, 64 * 1024)
          && test_cache_put_stylesheet(
              &import_session, "https://imports.test/styles/root.css",
              import_root_css, sizeof(import_root_css) - 1, NULL)
          && test_cache_put_stylesheet(
              &import_session, "https://imports.test/import.css",
              import_child_css, sizeof(import_child_css) - 1, NULL)
          && test_cache_put_stylesheet(
              &import_session, "https://imports.test/nested.css",
              import_nested_css, sizeof(import_nested_css) - 1, NULL)
          && stylesheets_load_external(
              &import_document, &import_sheet, &budget,
              "https://imports.test/page", 8, 64 * 1024, 16 * 1024,
              1000, NULL, &import_session, &import_stats));
    lxb_dom_node_t *import_card = find_id(
        lxb_dom_interface_node(import_document.html), "import-card");
    ComputedStyle import_card_style = style_for_node(
        &import_sheet, import_card, NULL);
    CHECK(import_card != NULL && import_stats.discovered == 5
          && import_stats.attempted == 3 && import_stats.loaded == 3
          && import_stats.cache_hits == 3
          && import_stats.imports_discovered == 4
          && import_stats.imports_loaded == 2
          && import_stats.imports_skipped_conditions == 2
          && import_stats.imports_skipped_depth == 0
          && import_card_style.display == DISPLAY_BLOCK
          && import_card_style.color == 0x246813
          && import_card_style.has_background
          && import_card_style.background == 0xdce9f7
          && import_card_style.border.left == 3
          && stylesheet_border_color(
                 &import_sheet, &import_card_style,
                 STYLE_BORDER_LEFT, NULL) == 0x315780);
    static const char repeated_minimum_pass[] =
        "(min-width:20em) and (min-width:29em)";
    static const char repeated_minimum_fail[] =
        "(min-width:20em) and (min-width:40.0625em)";
    static const char repeated_maximum_pass[] =
        "(max-width:40em) and (max-width:30em)";
    static const char repeated_maximum_fail[] =
        "(max-width:40em) and (max-width:20em)";
    static const char repeated_height_fail[] =
        "(min-height:10em) and (min-height:20em)";
    static const char responsive_geometry_pass[] =
        "(max-device-width:480px) and (orientation:landscape) and "
        "(min-aspect-ratio:16/10)";
    static const char responsive_geometry_fail[] =
        "(max-aspect-ratio:4/3)";
    static const char tailwind_desktop_breakpoint[] =
        "(min-width:64rem)";
    CHECK(stylesheet_media_matches(
              &import_sheet, repeated_minimum_pass,
              strlen(repeated_minimum_pass))
          && !stylesheet_media_matches(
              &import_sheet, repeated_minimum_fail,
              strlen(repeated_minimum_fail))
          && stylesheet_media_matches(
              &import_sheet, repeated_maximum_pass,
              strlen(repeated_maximum_pass))
          && !stylesheet_media_matches(
              &import_sheet, repeated_maximum_fail,
              strlen(repeated_maximum_fail))
          && !stylesheet_media_matches(
              &import_sheet, repeated_height_fail,
              strlen(repeated_height_fail))
          && stylesheet_media_matches(
              &import_sheet, responsive_geometry_pass,
              strlen(responsive_geometry_pass))
          && !stylesheet_media_matches(
              &import_sheet, responsive_geometry_fail,
              strlen(responsive_geometry_fail))
          && !stylesheet_media_matches(
              &import_sheet, tailwind_desktop_breakpoint,
              strlen(tailwind_desktop_breakpoint))
          && style_parse_length(&import_sheet, "10vmin", 6, -1, NULL) == 27
          && style_parse_length(&import_sheet, "10vmax", 6, -1, NULL) == 48
          && style_parse_length(&import_sheet, "10dvw", 5, -1, NULL) == 48
          && stylesheet_media_matches(
              &import_sheet, "(device-width:480px)",
              sizeof("(device-width:480px)") - 1)
          && !stylesheet_media_matches(
              &import_sheet, "(width:479px)", 13));
    stylesheet_destroy(&import_sheet);
    document_destroy(&import_document);
    browser_session_destroy(&import_session);
    CHECK(budget.current == 0);

    puts("test: layered utility breakpoints preserve mobile display");
    PocDocument utility_document;
    Stylesheet utility_sheet;
    CHECK(document_parse(
              &utility_document, &budget,
              "<!doctype html><div id=mobile-wordmark "
              "class='hidden lg:flex'>WORDMARK</div>",
              strlen("<!doctype html><div id=mobile-wordmark "
                     "class='hidden lg:flex'>WORDMARK</div>"), 17)
          && stylesheet_build(&utility_sheet, &budget,
                              &utility_document, 480));
    lxb_dom_node_t *mobile_wordmark = find_id(
        lxb_dom_interface_node(utility_document.html), "mobile-wordmark");
    CHECK(mobile_wordmark != NULL
          && stylesheet_add_css(
              &utility_sheet,
              "@layer utilities{.hidden{display:none}"
              "@media(min-width:64rem){.lg\\:flex{display:flex}}",
              strlen("@layer utilities{.hidden{display:none}"
                     "@media(min-width:64rem){.lg\\:flex{display:flex}}")));
    ComputedStyle mobile_wordmark_style = style_for_node(
        &utility_sheet, mobile_wordmark, NULL);
    CHECK(mobile_wordmark_style.display == DISPLAY_NONE);
    stylesheet_destroy(&utility_sheet);
    document_destroy(&utility_document);
    CHECK(budget.current == 0);

    puts("test: large generated stylesheets retain bounded sampled rules");
    static const char split_css[] =
        ".head{padding-top:1px}.unused{padding-left:4px}"
        ":where(.absent-helper){margin-top:9px}"
        ".middle\\:wide{padding-right:2px}"
        ":root{--late-pad:6px}h1{padding-left:var(--late-pad)}"
        ".tail{padding-bottom:3px}";
    PocDocument split_document;
    Stylesheet split_sheet;
    CHECK(document_parse(
              &split_document, &budget,
              "<!doctype html><h1 id=heading>Heading</h1>"
              "<div id=split class='head middle:wide tail'></div>",
              strlen("<!doctype html><h1 id=heading>Heading</h1>"
                     "<div id=split class='head middle:wide tail'></div>"), 17)
          && stylesheet_build(&split_sheet, &budget, &split_document, 480));
    size_t split_start = split_sheet.count;
    uint32_t split_bloom[64] = {0};
    static const unsigned char middle_token[] = "middle:wide";
    uint32_t middle_hash = UINT32_C(2166136261) ^ '.';
    for (size_t i = 0; i < sizeof(middle_token) - 1u; i++) {
        middle_hash ^= middle_token[i];
        middle_hash *= UINT32_C(16777619);
    }
    uint32_t middle_rotated = (middle_hash << 13) | (middle_hash >> 19);
    size_t middle_first = (size_t) middle_hash & (64u * 32u - 1u);
    size_t middle_second = (size_t) (
        middle_rotated ^ UINT32_C(0x9e3779b9))
        & (64u * 32u - 1u);
    split_bloom[middle_first / 32u] |=
        UINT32_C(1) << (middle_first & 31u);
    split_bloom[middle_second / 32u] |=
        UINT32_C(1) << (middle_second & 31u);
    static const unsigned char heading_token[] = "h1";
    uint32_t heading_hash = UINT32_C(2166136261) ^ 't';
    for (size_t i = 0; i < sizeof(heading_token) - 1u; i++) {
        heading_hash ^= heading_token[i];
        heading_hash *= UINT32_C(16777619);
    }
    uint32_t heading_rotated = (heading_hash << 13) | (heading_hash >> 19);
    size_t heading_first = (size_t) heading_hash & (64u * 32u - 1u);
    size_t heading_second = (size_t) (
        heading_rotated ^ UINT32_C(0x9e3779b9))
        & (64u * 32u - 1u);
    split_bloom[heading_first / 32u] |=
        UINT32_C(1) << (heading_first & 31u);
    split_bloom[heading_second / 32u] |=
        UINT32_C(1) << (heading_second & 31u);
    split_sheet.source_rule_head_limit_end = split_start + 1u;
    split_sheet.source_rule_relevant_limit_end = split_start + 4u;
    split_sheet.source_rule_limit_end = split_start + 5u;
    split_sheet.source_rule_token_bloom = split_bloom;
    split_sheet.source_rule_token_bloom_words = 64u;
    split_sheet.source_rule_tail_begin = strstr(split_css, ".tail");
    CHECK(split_sheet.source_rule_tail_begin != NULL
          && stylesheet_add_css(&split_sheet, split_css,
                                sizeof(split_css) - 1u));
    split_sheet.source_rule_limit_end = 0;
    split_sheet.source_rule_head_limit_end = 0;
    split_sheet.source_rule_relevant_limit_end = 0;
    split_sheet.source_rule_token_bloom = NULL;
    split_sheet.source_rule_token_bloom_words = 0;
    split_sheet.source_rule_tail_begin = NULL;
    lxb_dom_node_t *split = find_id(
        lxb_dom_interface_node(split_document.html), "split");
    lxb_dom_node_t *heading = find_id(
        lxb_dom_interface_node(split_document.html), "heading");
    ComputedStyle split_style = style_for_node(&split_sheet, split, NULL);
    ComputedStyle heading_style = style_for_node(
        &split_sheet, heading, NULL);
    CHECK(split != NULL && heading != NULL);
    CHECK(split_style.padding.top == 1
          && split_style.padding.left == 0
          && split_style.padding.right == 2
          && split_style.padding.bottom == 3);
    CHECK(heading_style.padding.left == 6);
    CHECK(split_sheet.count == split_start + 4u
          && split_sheet.diagnostic_selector_drops >= 1u);
    stylesheet_destroy(&split_sheet);
    document_destroy(&split_document);
    CHECK(budget.current == 0);

    puts("test: exhausted author realm reveals unresolved component light DOM");
    PocDocument component_document;
    Stylesheet component_sheet;
    CHECK(document_parse(
              &component_document, &budget,
              "<!doctype html><site-header><site-brand id=brand>Brand"
              "</site-brand><app-menu id=menu>Navigation"
              "</app-menu></site-header><app-shell id=shell>Readable fallback"
              "</app-shell>",
              strlen("<!doctype html><site-header><site-brand id=brand>Brand"
                     "</site-brand><app-menu id=menu>Navigation"
                     "</app-menu></site-header><app-shell id=shell>"
                     "Readable fallback</app-shell>"), 17)
          && stylesheet_build(&component_sheet, &budget,
                              &component_document, 480)
          && stylesheet_add_css(
              &component_sheet,
              "app-shell:not(:defined){visibility:hidden;opacity:0}"
              "app-shell:defined{visibility:visible;opacity:1}"
              "site-brand:not(:defined){visibility:hidden}"
              "app-menu:not(:defined){visibility:hidden}",
              strlen("app-shell:not(:defined){visibility:hidden;opacity:0}"
                     "app-shell:defined{visibility:visible;opacity:1}"
                     "site-brand:not(:defined){visibility:hidden}"
                     "app-menu:not(:defined){visibility:hidden}")));
    lxb_dom_node_t *component_shell = find_id(
        lxb_dom_interface_node(component_document.html), "shell");
    lxb_dom_node_t *component_menu = find_id(
        lxb_dom_interface_node(component_document.html), "menu");
    lxb_dom_node_t *component_brand = find_id(
        lxb_dom_interface_node(component_document.html), "brand");
    ComputedStyle unresolved_component = style_for_node(
        &component_sheet, component_shell, NULL);
    CHECK(component_shell != NULL && unresolved_component.visibility_hidden
          && unresolved_component.opacity == 0);
    uint64_t component_generation = component_sheet.build_generation;
    stylesheet_enable_static_custom_element_fallback(&component_sheet);
    ComputedStyle fallback_component = style_for_node(
        &component_sheet, component_shell, NULL);
    ComputedStyle fallback_menu = style_for_node(
        &component_sheet, component_menu, NULL);
    ComputedStyle fallback_brand = style_for_node(
        &component_sheet, component_brand, NULL);
    CHECK(component_sheet.static_custom_element_fallback
          && component_sheet.build_generation != component_generation
          && !fallback_component.visibility_hidden
          && fallback_component.opacity == 255
          && !fallback_brand.visibility_hidden
          && fallback_menu.visibility_hidden);
    stylesheet_destroy(&component_sheet);
    document_destroy(&component_document);
    CHECK(budget.current == 0);

    puts("test: exhausted author realm reveals only the staged first frame");
    static const char staged_content_page[] =
        "<!doctype html><style>.opacity-0{opacity:0}.slide{position:absolute;"
        "opacity:0}img{opacity:0}#modal{position:fixed;z-index:4}</style>"
        "<body><h1 id=heading class=opacity-0>Server title</h1>"
        "<div id=intro style='position:absolute;opacity:0'>"
        "<h2 class=opacity-0>Intro title</h2></div>"
        "<div id=deck><section id=first class=slide><img id=poster></section>"
        "<section id=later class=slide>Later</section></div>"
        "<div id=modal class=opacity-0>Dialog</div>"
        "<p id=hidden class=opacity-0 aria-hidden=true>Hidden</p></body>";
    PocDocument staged_content_document;
    Stylesheet staged_content_sheet;
    CHECK(document_parse(&staged_content_document, &budget,
                         staged_content_page,
                         strlen(staged_content_page), 18)
          && stylesheet_build(&staged_content_sheet, &budget,
                              &staged_content_document, 480));
    lxb_dom_node_t *staged_heading = find_id(
        lxb_dom_interface_node(staged_content_document.html), "heading");
    lxb_dom_node_t *staged_first = find_id(
        lxb_dom_interface_node(staged_content_document.html), "first");
    lxb_dom_node_t *staged_intro = find_id(
        lxb_dom_interface_node(staged_content_document.html), "intro");
    lxb_dom_node_t *staged_later = find_id(
        lxb_dom_interface_node(staged_content_document.html), "later");
    lxb_dom_node_t *staged_poster = find_id(
        lxb_dom_interface_node(staged_content_document.html), "poster");
    lxb_dom_node_t *staged_modal = find_id(
        lxb_dom_interface_node(staged_content_document.html), "modal");
    lxb_dom_node_t *staged_hidden = find_id(
        lxb_dom_interface_node(staged_content_document.html), "hidden");
    stylesheet_enable_static_custom_element_fallback(&staged_content_sheet);
    ComputedStyle revealed_heading = style_for_node(
        &staged_content_sheet, staged_heading, NULL);
    ComputedStyle revealed_first = style_for_node(
        &staged_content_sheet, staged_first, NULL);
    ComputedStyle revealed_intro = style_for_node(
        &staged_content_sheet, staged_intro, NULL);
    ComputedStyle retained_later = style_for_node(
        &staged_content_sheet, staged_later, NULL);
    ComputedStyle revealed_poster = style_for_node(
        &staged_content_sheet, staged_poster, &revealed_first);
    ComputedStyle retained_modal = style_for_node(
        &staged_content_sheet, staged_modal, NULL);
    ComputedStyle retained_hidden = style_for_node(
        &staged_content_sheet, staged_hidden, NULL);
    CHECK(revealed_heading.opacity == 255
          && revealed_intro.opacity == 255
          && revealed_first.opacity == 0
          && revealed_poster.opacity == 0
          && retained_later.opacity == 0
          && retained_modal.opacity == 0
          && retained_hidden.opacity == 0);
    stylesheet_destroy(&staged_content_sheet);
    document_destroy(&staged_content_document);
    CHECK(budget.current == 0);

    puts("test: escaped CSS identifiers match literal DOM tokens");
    static const char escaped_identifier_page[] =
        "<!doctype html><style>div{padding-top:19px}"
        ".govuk-\\!-padding-top-0{padding-top:0}"
        "#escaped\\!id{margin-left:7px}"
        ".bg-\\[url\\(\\'https\\:\\/\\/example\\.com\\/a\\.png\\'\\)\\]"
        "{background-image:url(https://example.com/a.png)}"
        ".py-1\\.5{padding-top:.375rem;padding-bottom:.375rem}"
        ".rounded-\\[100px\\]{border-radius:100px}"
        ".outline{outline-style:solid}"
        ".outline-2{outline-width:2px}"
        ".outline-\\[rgb\\(70_31_140\\/60\\%\\)\\]"
        "{outline-color:rgb(70 31 140/60%)}"
        ".w-\\[clamp\\(min\\(250px\\,100\\%\\)\\,62svw\\,440px\\)\\]"
        "{width:clamp(min(250px,100%),62svw,440px)}"
        ".escaped-content:before{--tw-content:\\\"\\\";"
        "content:var(--tw-content)}"
        ".suffix-survives{height:19px}</style><body>"
        "<div id='escaped!id' class='govuk-!-padding-top-0'>ESCAPED</div>"
        "<div id='utility-pill' class='py-1.5 rounded-[100px] outline "
        "outline-2 outline-[rgb(70_31_140/60%)]'>SEARCH</div>"
        "<div id='escaped-comma' "
        "class='w-[clamp(min(250px,100%),62svw,440px)]'></div>"
        "<div id='escaped-suffix' class='suffix-survives'></div>"
        "</body>";
    PocDocument escaped_identifier_document;
    Stylesheet escaped_identifier_sheet;
    CHECK(document_parse(&escaped_identifier_document, &budget,
                         escaped_identifier_page,
                         strlen(escaped_identifier_page), 17)
          && stylesheet_build(&escaped_identifier_sheet, &budget,
                              &escaped_identifier_document, 480));
    lxb_dom_node_t *escaped_identifier = find_id(
        lxb_dom_interface_node(escaped_identifier_document.html),
        "escaped!id");
    lxb_dom_node_t *utility_pill = find_id(
        lxb_dom_interface_node(escaped_identifier_document.html),
        "utility-pill");
    lxb_dom_node_t *escaped_comma = find_id(
        lxb_dom_interface_node(escaped_identifier_document.html),
        "escaped-comma");
    lxb_dom_node_t *escaped_suffix = find_id(
        lxb_dom_interface_node(escaped_identifier_document.html),
        "escaped-suffix");
    ComputedStyle escaped_identifier_style = style_for_node(
        &escaped_identifier_sheet, escaped_identifier, NULL);
    ComputedStyle utility_pill_style = style_for_node(
        &escaped_identifier_sheet, utility_pill, NULL);
    ComputedStyle escaped_comma_style = style_for_node(
        &escaped_identifier_sheet, escaped_comma, NULL);
    ComputedStyle escaped_suffix_style = style_for_node(
        &escaped_identifier_sheet, escaped_suffix, NULL);
    int escaped_comma_width = 0;
    CHECK(escaped_identifier != NULL
          && escaped_identifier_style.padding.top == 0
          && escaped_identifier_style.margin.left == 7);
    CHECK(utility_pill != NULL);
    CHECK(utility_pill_style.padding.top == 6);
    CHECK(utility_pill_style.padding.bottom == 6);
    CHECK(style_border_radius_corner(
              stylesheet_border_radius_code(
                  &escaped_identifier_sheet, &utility_pill_style), 0) == 100);
    CHECK(computed_style_outline_width(&utility_pill_style) == 2);
    CHECK(computed_style_outline_style(&utility_pill_style)
          == STYLE_OUTLINE_SOLID);
    CHECK(utility_pill_style.outline_color == 0x461f8c);
    CHECK(utility_pill_style.outline_alpha == 153);
    CHECK(escaped_comma != NULL && escaped_comma_style.has_width
          && style_length_resolve(&escaped_identifier_sheet,
                                  escaped_comma_style.width, 480,
                                  &escaped_comma_width)
          && escaped_comma_width == 298);
    CHECK(escaped_suffix != NULL && escaped_suffix_style.has_height
          && escaped_suffix_style.height == 19);
    stylesheet_destroy(&escaped_identifier_sheet);
    document_destroy(&escaped_identifier_document);
    CHECK(budget.current == 0);

    puts("test: live DOM bridge and batched relayout");
    static const char mutation_page[] =
        "<!doctype html><title>Mutation</title>"
        "<style>#root{place-items:center end;place-content:end center;flex-direction:row-reverse;flex-wrap:wrap-reverse;margin:auto}#added{display:block;color:#123456;align-self:baseline;align-content:space-around;order:-7;min-width:20%;max-width:80%;min-height:25%;max-height:75%}</style>"
        "<body><div id=root class=outer>OLD</div><script>"
        "const stream=new ReadableStream({start(controller){controller.close();}});"
        "stream.pipeThrough(new TextEncoderStream());"
        "const streamOk=typeof ReadableStream.prototype.pipeTo==='function';"
        "const lockStream=new ReadableStream({start(controller){controller.close();}}),lockReader=lockStream.getReader();let secondReaderError='';try{lockStream.getReader();}catch(error){secondReaderError=error.name;}lockReader.releaseLock();const cancelResponse=new Response('cancel');cancelResponse.body.cancel();const streamStateOk=secondReaderError==='TypeError'&&!lockStream.locked&&cancelResponse.bodyUsed;"
        "localStorage.setItem('theme','dark');"
        "localStorage.alpha='one';localStorage.setItem('beta','two');"
        "const storageKeys=[];for(let i=0;i<localStorage.length;i++)"
        "storageKeys.push(localStorage.key(i));"
        "const storageEnumerable=Object.keys(localStorage),storageOk="
        "localStorage.alpha==='one'&&localStorage.getItem('alpha')==='one'"
        "&&localStorage.length===3&&storageKeys.includes('theme')"
        "&&storageKeys.includes('alpha')&&storageKeys.includes('beta')"
        "&&storageEnumerable.includes('alpha')"
        "&&storageEnumerable.includes('beta')"
        "&&!storageEnumerable.includes('getItem')"
        "&&localStorage instanceof Storage;delete localStorage.alpha;"
        "localStorage.removeItem('beta');"
        "const startupOk=matchMedia('(max-width:480px)').matches&&matchMedia('(orientation:landscape) and (hover:none) and (pointer:coarse)').matches&&!matchMedia('(min-width:481px)').matches&&screen.orientation.type==='landscape-primary'&&screen.orientation.angle===90&&'oninput' in document&&'onclick' in document.createElement('button')&&document.hasFocus()===true;"
        "const randomBytes=crypto.getRandomValues(new Uint8Array(32));"
        "performance.mark('startup');const perfOk=performance.getEntriesByName('startup').length===1&&PerformanceObserver.supportedEntryTypes.includes('resource');"
        "const svg=document.createElementNS('http://www.w3.org/2000/svg','svg');"
        "const blob=new Blob(['PSP'],{type:'text/plain'});const blobURL=URL.createObjectURL(blob);const workerBlob=new Blob(['self.ready=true;'],{type:'text/javascript'});const workerURL=URL.createObjectURL(workerBlob);const worker=new Worker(workerURL);const blobOk=blob.size===3&&blob.type==='text/plain'&&blobURL.startsWith('blob:https://')&&worker instanceof Worker;worker.terminate();URL.revokeObjectURL(workerURL);URL.revokeObjectURL(blobURL);"
        "const interfaceHost=document.createElement('div');document.body.appendChild(interfaceHost);const shadow=interfaceHost.attachShadow({mode:'closed'});const frame=document.createElement('iframe');frame.id='shadow-frame';shadow.appendChild(frame);const iterator=document.createNodeIterator(interfaceHost,NodeFilter.SHOW_ELEMENT|NodeFilter.SHOW_TEXT);const nativeSource=Function.prototype.toString;const prototypeQueriesOk=[Document.prototype,DocumentFragment.prototype,Element.prototype].every(proto=>typeof proto.querySelector==='function'&&typeof proto.querySelectorAll==='function'&&nativeSource.call(proto.querySelector).includes('[native code]')&&nativeSource.call(proto.querySelectorAll).includes('[native code]'));const frameEval=frame.contentWindow.eval,staleFrame=document.createElement('iframe'),staleWindow=staleFrame.contentWindow;let stalePostOk=true;try{staleWindow.postMessage({probe:true},'*');}catch(_){stalePostOk=false;}const interfaceBits=[interfaceHost instanceof HTMLDivElement,interfaceHost.nodeType===Node.ELEMENT_NODE,frame instanceof HTMLIFrameElement,shadow instanceof ShadowRoot,shadow.getElementById('shadow-frame')===frame,frame.isConnected,frame.contentDocument instanceof Document&&frame.contentDocument!==document,staleFrame.contentDocument===null,frameEval('21*2')===42,stalePostOk,nativeSource.call(frameEval).includes('[native code]'),prototypeQueriesOk,interfaceHost.shadowRoot===null,iterator.nextNode()!==null,interfaceHost.isConnected,interfaceHost.getBoundingClientRect().width>0,document.scripts.length>0,document.styleSheets.length>0],interfaceOk=interfaceBits.every(Boolean);"
        "const positionFirst=document.createElement('i'),positionSecond=document.createElement('i'),positionDetached=document.createElement('i');interfaceHost.append(positionFirst,positionSecond);const disconnectedForward=positionFirst.compareDocumentPosition(positionDetached),disconnectedReverse=positionDetached.compareDocumentPosition(positionFirst);let invalidPositionThrew=false;try{positionFirst.compareDocumentPosition({});}catch(error){invalidPositionThrew=error instanceof TypeError;}const positionOk=Node.DOCUMENT_POSITION_DISCONNECTED===1&&Node.prototype.DOCUMENT_POSITION_CONTAINED_BY===16&&positionFirst.compareDocumentPosition(positionFirst)===0&&interfaceHost.compareDocumentPosition(positionFirst)===(Node.DOCUMENT_POSITION_FOLLOWING|Node.DOCUMENT_POSITION_CONTAINED_BY)&&positionFirst.compareDocumentPosition(interfaceHost)===(Node.DOCUMENT_POSITION_PRECEDING|Node.DOCUMENT_POSITION_CONTAINS)&&positionFirst.compareDocumentPosition(positionSecond)===Node.DOCUMENT_POSITION_FOLLOWING&&positionSecond.compareDocumentPosition(positionFirst)===Node.DOCUMENT_POSITION_PRECEDING&&(disconnectedForward&(Node.DOCUMENT_POSITION_DISCONNECTED|Node.DOCUMENT_POSITION_IMPLEMENTATION_SPECIFIC))===(Node.DOCUMENT_POSITION_DISCONNECTED|Node.DOCUMENT_POSITION_IMPLEMENTATION_SPECIFIC)&&(disconnectedForward&6)!==0&&(disconnectedReverse&6)!==0&&(disconnectedForward&6)!==(disconnectedReverse&6)&&invalidPositionThrew;"
        "interfaceHost.style.setProperty('--CaseSensitive','kept');interfaceHost.style.cssFloat='left';interfaceHost.style.placeItems='center end';interfaceHost.style.placeSelf='start end';interfaceHost.style.placeContent='end center';const cssomNameOk=interfaceHost.style.getPropertyValue('--CaseSensitive')==='kept'&&interfaceHost.style.getPropertyValue('float')==='left'&&interfaceHost.style.alignItems==='center'&&interfaceHost.style.justifyItems==='end'&&interfaceHost.style.placeItems==='center end'&&interfaceHost.style.alignSelf==='start'&&interfaceHost.style.justifySelf==='end'&&interfaceHost.style.placeSelf==='start end'&&interfaceHost.style.alignContent==='end'&&interfaceHost.style.justifyContent==='center'&&interfaceHost.style.placeContent==='end center';"
        "const capabilitiesOk=navigator.gpu===undefined&&typeof GPU==='undefined'&&typeof RTCPeerConnection==='undefined'&&typeof RTCDataChannel==='undefined';"
        "const encoder=new TextEncoder,encodedReplacement=encoder.encode(String.fromCharCode(0xd800)),intoTarget=new Uint8Array(4),intoResult=encoder.encodeInto('A€B',intoTarget);const streamingDecoder=new TextDecoder,streamFirst=streamingDecoder.decode(new Uint8Array([0xe2,0x82]),{stream:true}),streamSecond=streamingDecoder.decode(new Uint8Array([0xac]));let fatalEncoding='';try{new TextDecoder('utf-8',{fatal:true}).decode(new Uint8Array([0xc0]));}catch(error){fatalEncoding=error.name;}const encodingOk=encodedReplacement.join(',')==='239,191,189'&&intoResult.read===2&&intoResult.written===4&&intoTarget.join(',')==='65,226,130,172'&&streamFirst===''&&streamSecond==='€'&&new TextDecoder().decode(new Uint8Array([0xef,0xbb,0xbf,65]))==='A'&&new TextDecoder('utf8',{ignoreBOM:true}).decode(new Uint8Array([0xef,0xbb,0xbf,65]))==='﻿A'&&new TextDecoder().decode(new Uint8Array([0xc0,0xaf])).length===2&&fatalEncoding==='TypeError';"
        "let invalidBase64Name='';try{atob('!');}catch(error){invalidBase64Name=error.name;}const base64Ok=atob(btoa('PSP\\0'))==='PSP\\0'&&atob('UFM')==='PS'&&atob('UFM=')==='PS'&&invalidBase64Name==='InvalidCharacterError';"
        "const abortController=new AbortController,abortSignal=abortController.signal;let abortSeen=false;abortSignal.addEventListener('abort',()=>abortSeen=true,{once:true});abortController.abort();const abortOk=abortSignal.aborted&&abortSeen&&abortSignal.reason instanceof DOMException&&abortSignal.reason.name==='AbortError';"
        "const template=document.createElement('template');template.innerHTML='<template for=target><b>update</b></template><template data-assistant-presentation-flush></template>';const templateClone=template.cloneNode(true),clonedTarget=templateClone.content.querySelector('template[for=target]'),flushTemplate=template.content.querySelector('template[data-assistant-presentation-flush]'),fragmentProbe=document.createDocumentFragment(),fragmentText=document.createTextNode('fragment');fragmentProbe.appendChild(fragmentText);const fragmentHost=document.createElement('div');fragmentHost.appendChild(fragmentProbe);const equalProbe=document.createElement('p');equalProbe.textContent='equal';const templateOk=template.children.length===0&&template.content.querySelector('template[for=target]')!==null&&template.content.querySelectorAll('template[for],template[data-assistant-presentation-flush]').length===2&&flushTemplate.matches('template[data-assistant-presentation-flush]')&&clonedTarget.content.querySelector('b').textContent==='update'&&equalProbe.isEqualNode(equalProbe.cloneNode(true))&&fragmentProbe.childNodes.length===0&&fragmentHost.textContent==='fragment';"
        "const parsedDocument=new DOMParser().parseFromString('<p id=parsed>One &amp; <b>two</b></p>','text/html'),xmlDocument=new DOMParser().parseFromString('<x/>','application/xml'),parsedElement=parsedDocument.getElementById('parsed'),domParserBits=[parsedDocument instanceof Document,parsedDocument.nodeType===Node.DOCUMENT_NODE,parsedDocument.defaultView===null,parsedDocument.contentType==='text/html',parsedDocument.documentElement.textContent==='One & two',parsedElement?.querySelector('b').textContent==='two',xmlDocument instanceof Document&&xmlDocument.documentElement?.localName==='x'],domParserOk=domParserBits.every(Boolean);"
        "const workDocument=document.implementation.createHTMLDocument('');workDocument.documentElement.innerHTML='<style id=\"adopted-style\">#adopted-probe{color:rgb(1,2,3)}</style>';const adoptedStyle=workDocument.getElementById('adopted-style');document.head.appendChild(adoptedStyle);const createdDocumentOk=adoptedStyle instanceof HTMLStyleElement&&adoptedStyle.ownerDocument===document&&adoptedStyle.parentElement===document.head&&adoptedStyle.textContent.includes('#adopted-probe');"
        "const moveA=document.createElement('div'),moveB=document.createElement('div'),moveOne=document.createElement('i'),moveTwo=document.createElement('i'),moveThree=document.createElement('i');moveOne.id='move-one';moveTwo.id='move-two';moveThree.id='move-three';moveA.appendChild(moveOne);moveA.appendChild(moveTwo);moveB.appendChild(moveOne);const crossAppendOk=moveA.firstChild===moveTwo&&moveB.firstChild===moveOne;moveA.insertBefore(moveTwo,moveTwo);moveA.appendChild(moveThree);moveA.appendChild(moveTwo);const reorderOk=moveA.firstChild===moveThree&&moveA.lastChild===moveTwo;const orphanReference=document.createElement('i');let invalidInsertThrew=false;try{moveA.insertBefore(moveOne,orphanReference);}catch(_){invalidInsertThrew=true;}const invalidInsertPreserved=moveOne.parentElement===moveB&&moveA.firstChild===moveThree;moveA.insertBefore(moveOne,moveTwo);const crossInsertOk=moveB.childNodes.length===0&&moveA.childNodes.length===3&&moveA.childNodes[0]===moveThree&&moveA.childNodes[1]===moveOne&&moveA.childNodes[2]===moveTwo;const preinsertOk=crossAppendOk&&reorderOk&&invalidInsertThrew&&invalidInsertPreserved&&crossInsertOk;"
        "const platformOk=streamOk&&streamStateOk&&storageOk&&localStorage.length===1&&capabilitiesOk&&cssomNameOk&&encodingOk&&base64Ok&&abortOk&&templateOk&&domParserOk&&createdDocumentOk&&preinsertOk&&positionOk&&randomBytes.some(value=>value!==0)&&crypto.randomUUID().length===36&&svg.namespaceURI.includes('svg')&&perfOk&&blobOk&&interfaceOk&&top===window&&parent===window;"
        "const url=new URL('../next?q=one+two#part','https://example.test/a/b');url.searchParams.set('extra','yes');"
        "const recordParams=new URLSearchParams({conversationState:'{\\\"messages\\\":[]}',count:0});const sequenceParams=new URLSearchParams([['first','one'],['first','two']]);"
        "const urlOk=url.pathname==='/next'&&url.searchParams.get('q')==='one two'&&url.search.includes('extra=yes')&&String(new URL('.',url))==='https://example.test/'&&recordParams.toString()==='conversationState=%7B%22messages%22%3A%5B%5D%7D&count=0'&&sequenceParams.getAll('first').join(',')==='one,two';"
        "const submitForm=document.createElement('form'),submitInput=document.createElement('input'),uncheckedInput=document.createElement('input'),submitButton=document.createElement('button');submitForm.action='/submit';submitInput.name='message';submitInput.value='hello';uncheckedInput.name='ignored';uncheckedInput.type='checkbox';submitButton.type='submit';submitForm.appendChild(submitInput);submitForm.appendChild(uncheckedInput);submitForm.appendChild(submitButton);document.body.appendChild(submitForm);const formData=new FormData(submitForm);formData.set('message','updated');formData.append('extra','yes');const formDataOk=submitForm.action.endsWith('/submit')&&submitForm.elements.namedItem('message')===submitInput&&formData.get('message')==='updated'&&!formData.has('ignored')&&[...formData].length===2;let submitterOk=false;submitForm.addEventListener('submit',event=>{submitterOk=event.submitter===submitButton;event.preventDefault();});submitButton.click();"
        "const documentObserver=new MutationObserver(records=>{if(records.some(record=>record.addedNodes.some(node=>node.id==='document-observer-probe')))document.body.setAttribute('data-document-observer','yes');});documentObserver.observe(document,{childList:true,subtree:true});const documentObserverProbe=document.createElement('i');documentObserverProbe.id='document-observer-probe';document.body.appendChild(documentObserverProbe);"
        "const root=document.getElementById('root');"
        "root.addEventListener('ping',()=>root.setAttribute('data-hit','yes'));"
        "root.dispatchEvent(new Event('ping'));"
        "document.addEventListener('DOMContentLoaded',()=>{"
        " const added=document.createElement('p'); added.id='added'; added.className='item'; added.setAttribute('data-kind','live');"
        " added.textContent='LIVE DOM'; root.appendChild(added);"
        " const fragment=document.createElement('div');fragment.innerHTML='<b id=fragment>FRAGMENT</b>';root.appendChild(fragment);"
        " added.style.color='#123456';added.style.setProperty('--probe','kept');"
        " added.style.setProperty('--shadow','rgb(1, 2, 3) 4px 5px 6px');"
        " added.style.textShadow='var(--shadow)';"
        " const addedStyle=getComputedStyle(added);const selectorOk=document.querySelectorAll('#root > .item[data-kind=live]').length===1&&added.closest('.outer')===root&&added.parentElement===root&&addedStyle.color==='#123456'&&fragment.querySelector('#fragment').textContent==='FRAGMENT'&&fragment.innerHTML.includes('<b id=\"fragment\">');"
        " globalThis.pocSummary=added.textContent+':'+root.getAttribute('data-hit')+':'+(startupOk&&urlOk&&platformOk&&formDataOk&&submitterOk?'ok':'bad-'+[startupOk,urlOk,platformOk,formDataOk,submitterOk].map(Number).join('')+'-platform='+[streamOk,streamStateOk,capabilitiesOk,cssomNameOk,encodingOk,base64Ok,abortOk,templateOk,domParserOk,preinsertOk,positionOk,randomBytes.some(value=>value!==0),svg.namespaceURI.includes('svg'),perfOk,blobOk,interfaceOk,top===window,parent===window].map(Number).join('')+'-storage='+[localStorage.alpha,localStorage.length,storageKeys.join(','),storageEnumerable.join(','),localStorage instanceof Storage].join('/')+'-parser='+domParserBits.map(Number).join('')+'-interface='+interfaceBits.map(Number).join(''))+':'+(selectorOk?'selectors':'bad');"
        "});</script></body>";
    PocDocument mutated;
    CHECK(document_parse(&mutated, &budget, mutation_page,
                         strlen(mutation_page), 29));
    ScriptResult mutation_scripts;
    bool mutation_ok = scripts_run_document(&mutated, &budget, 4 * MIB,
                                            1000, &mutation_scripts);
    if (!mutation_ok) {
        fprintf(stderr, "mutation runtime failed: %s\n",
                mutation_scripts.error);
    }
    if (strcmp(mutation_scripts.summary,
               "LIVE DOM:yes:ok:selectors") != 0) {
        fprintf(stderr,
                "mutation summary: \"%s\" success=%s scripts=%zu "
                "events=%zu mutations=%zu relayout=%s error=\"%s\"\n",
                mutation_scripts.summary,
                mutation_scripts.success ? "yes" : "no",
                mutation_scripts.scripts_evaluated,
                mutation_scripts.events_dispatched,
                mutation_scripts.dom_mutations,
                mutation_scripts.relayout_required ? "yes" : "no",
                mutation_scripts.error);
    }
    CHECK(mutation_ok);
    CHECK(mutation_scripts.success && mutation_scripts.relayout_required
          && mutation_scripts.dom_mutations >= 4
          && mutation_scripts.events_dispatched >= 2
          && mutation_scripts.performance_now_calls >= 1
          && mutation_scripts.bootstrap_lazy_module_loads == 1
          && mutation_scripts.bootstrap_lazy_module_failures == 0
          && mutated.body_text == NULL
          && strcmp(mutation_scripts.summary,
                    "LIVE DOM:yes:ok:selectors") == 0
          && strstr(document_body_text(&mutated), "LIVE DOM") != NULL);
    lxb_dom_node_t *mutated_root = find_id(
        lxb_dom_interface_node(mutated.html), "root");
    size_t hit_length = 0;
    const char *hit = document_attribute(mutated_root, "data-hit", &hit_length);
    CHECK(hit != NULL && hit_length == 3 && memcmp(hit, "yes", 3) == 0);
    size_t observer_length = 0;
    const char *observer_seen = document_attribute(
        document_body_node(&mutated), "data-document-observer",
        &observer_length);
    CHECK(observer_seen != NULL && observer_length == 3
          && memcmp(observer_seen, "yes", 3) == 0);
    Stylesheet mutation_sheet;
    LayoutDocument mutation_layout;
    CHECK(stylesheet_build(&mutation_sheet, &budget, &mutated, 480)
          && layout_build(&mutation_layout, &budget, &mutated,
                          &mutation_sheet, NULL, NULL, 480)
          && command_x(&mutation_layout, "LIVE") >= 0
          && command_x(&mutation_layout, "DOM") >
             command_x(&mutation_layout, "LIVE"));
    ScriptResult computed_style_result;
    ScriptRuntime *computed_style_runtime = script_runtime_create_deferred(
        &mutated, &budget, 2 * MIB, 1000, "https://example.test/",
        NULL, &computed_style_result);
    CHECK(computed_style_runtime != NULL);
    script_runtime_set_stylesheet(computed_style_runtime, &mutation_sheet);
    CHECK(script_runtime_evaluate_diagnostic(
              computed_style_runtime,
              "const value=getComputedStyle(document.getElementById('added'));"
              "globalThis.pocSummary=value.alignSelf+':'+value.alignContent+':'+value.order+"
              "':'+value.minWidth+':'+value.maxWidth+':'+"
              "value.minHeight+':'+value.maxHeight+':'+"
              "value.getPropertyValue('--probe')+':'+value.textShadow+':'+"
              "getComputedStyle(document.getElementById('root')).alignItems+"
              "':'+getComputedStyle(document.getElementById('root')).justifyItems+"
              "':'+getComputedStyle(document.getElementById('root')).placeItems+"
              "':'+getComputedStyle(document.getElementById('root')).placeContent+"
              "':'+getComputedStyle(document.getElementById('root')).flexDirection+"
              "':'+getComputedStyle(document.getElementById('root')).flexWrap+"
              "':'+getComputedStyle(document.getElementById('root')).marginTop+"
              "':'+getComputedStyle(document.getElementById('root')).marginRight+"
              "':'+getComputedStyle(document.getElementById('root')).marginBottom+"
              "':'+getComputedStyle(document.getElementById('root')).marginLeft",
              "https://example.test/computed-style.js",
              &computed_style_result)
          && strcmp(computed_style_result.summary,
                    "baseline:space-around:-7:20%:80%:25%:75%:kept:"
                    "rgb(1, 2, 3) 4px 5px 6px:center:end:center end:end center:"
                    "row-reverse:wrap-reverse:auto:auto:auto:auto") == 0);
    script_runtime_destroy(computed_style_runtime);
    layout_destroy(&mutation_layout);
    stylesheet_destroy(&mutation_sheet);
    document_destroy(&mutated);
    CHECK(budget.current == 0);

    puts("test: isolated frame messaging and trusted delivery");
    static const char parent_message_page[] =
        "<!doctype html><body><script>window.addEventListener('message',"
        "event=>globalThis.pocSummary=String(event.isTrusted)+':' +"
        "event.origin+':' +event.data.event);</script></body>";
    static const char child_message_page[] =
        "<!doctype html><body><script>parent.postMessage({event:'hello'},"
        "'*');</script></body>";
    PocDocument parent_message_document, child_message_document;
    ScriptResult parent_message_result, child_message_result;
    CHECK(document_parse(&parent_message_document, &budget,
                         parent_message_page,
                         strlen(parent_message_page), 16)
          && document_parse(&child_message_document, &budget,
                            child_message_page,
                            strlen(child_message_page), 16));
    ScriptRuntime *parent_message_runtime = script_runtime_create(
        &parent_message_document, &budget, 2 * MIB, 1000,
        "https://parent.test/", &parent_message_result);
    ScriptRuntime *child_message_runtime = script_runtime_create_deferred(
        &child_message_document, &budget, 2 * MIB, 1000,
        "https://child.test/", NULL, &child_message_result);
    MessageTestTarget message_target = {
        parent_message_runtime, &parent_message_result
    };
    TestRemoteSelectorProbe legacy_child_backing_probe = {0};
    ScriptRemoteDocumentBinding legacy_child_binding = {
        .selector_lookup = test_bound_remote_selector_lookup,
        .selector_opaque = &legacy_child_backing_probe
    };
    char legacy_remote_identifier[129] = {0};
    size_t legacy_remote_section = SIZE_MAX;
    CHECK(parent_message_runtime != NULL && child_message_runtime != NULL
          /* Convenience constructors begin top-level for compatibility. A
             later child messaging configuration must irreversibly sanitize
             that legacy realm and its already-queued backing work. */
          && script_runtime_rebind_remote_document(
                 child_message_runtime, &legacy_child_binding)
          && script_runtime_evaluate_diagnostic(
                 child_message_runtime,
                 "globalThis.pocSummary=document.querySelector('aside')"
                 "?'legacy-bound':'legacy-missing'",
                 "<legacy-child-before-scope>", &child_message_result)
          && strcmp(child_message_result.summary, "legacy-bound") == 0
          && script_runtime_take_remote_element_request(
                 child_message_runtime, legacy_remote_identifier,
                 sizeof(legacy_remote_identifier), &legacy_remote_section)
          && legacy_remote_section == 1
          && script_runtime_evaluate_diagnostic(
                 child_message_runtime, "document.querySelector('aside')",
                 "<legacy-child-pending-scope>", &child_message_result)
          && script_runtime_configure_messaging(
              child_message_runtime, true, false, false,
              forward_test_message,
              &message_target)
          && !script_runtime_take_remote_element_request(
                 child_message_runtime, legacy_remote_identifier,
                 sizeof(legacy_remote_identifier), &legacy_remote_section)
          && !script_runtime_rebind_remote_document(
                 child_message_runtime, &legacy_child_binding)
          && !script_runtime_set_section_identity(
                 child_message_runtime, 2));
    size_t legacy_child_calls = legacy_child_backing_probe.calls;
    CHECK(script_runtime_evaluate_diagnostic(
              child_message_runtime,
              "globalThis.pocSummary=document.querySelector('aside')"
              "===null?'child-isolated':'child-leaked'",
              "<legacy-child-after-scope>", &child_message_result)
          && strcmp(child_message_result.summary, "child-isolated") == 0
          && legacy_child_backing_probe.calls == legacy_child_calls
          && !script_runtime_configure_messaging(
                 child_message_runtime, false, true, false,
                 forward_test_message,
                 &message_target));
    CHECK(script_runtime_evaluate_diagnostic(
              child_message_runtime,
              "let blocked=false;const send=parent.postMessage;"
              "try{parent.postMessage=()=>{}}catch(error){blocked="
              "error instanceof DOMException&&error.name==='SecurityError'}"
              "send({event:'hello'},'*');globalThis.pocSummary=blocked&&"
              "parent.postMessage===send?'cross-origin-blocked':"
              "'cross-origin-writable'",
              "https://child.test/message.js", &child_message_result)
          && strcmp(child_message_result.summary,
                    "cross-origin-blocked") == 0
          && strcmp(parent_message_result.summary,
                    "true:https://child.test:hello") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              child_message_runtime,
              "setTimeout(()=>{throw new Error('child-task-probe')},0)",
              "https://child.test/task.js", &child_message_result)
          && script_runtime_advance(child_message_runtime, 0, 4,
                                    &child_message_result)
          && strstr(child_message_result.last_uncaught_callback_error,
                    "child-task-probe") != NULL
          && strstr(child_message_result.last_uncaught_callback_task,
                    "realm=child task=timer:timeout:id=") != NULL);
    script_runtime_destroy(child_message_runtime);
    script_runtime_destroy(parent_message_runtime);

    puts("test: sandbox opaque child origin and parent messaging");
    MessageOpaqueProbe opaque_message_probe = {0};
    ScriptResult opaque_message_result = {0};
    BrowserSession opaque_message_session;
    static const char opaque_url[] = "https://opaque-child.test/";
    TilefinchRequestContext opaque_seed_context = {
        .target_url = opaque_url, .initiator_url = opaque_url,
        .top_level_url = opaque_url, .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_SAME_ORIGIN,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_OTHER
    };
    CHECK(browser_session_init(&opaque_message_session, &budget,
                               64 * 1024)
          && browser_session_storage_set(
                 &opaque_message_session, opaque_url, true, "secret",
                 "STORAGE_SECRET", strlen("STORAGE_SECRET"))
          && browser_session_cookie_set_context(
                 &opaque_message_session, &opaque_seed_context,
                 "secret=COOKIE_SECRET; Secure; SameSite=None"));
    ScriptRuntime *opaque_message_runtime = script_runtime_create_deferred(
        &child_message_document, &budget, 2 * MIB, 1000,
        opaque_url, &opaque_message_session, &opaque_message_result);
    CHECK(opaque_message_runtime != NULL
          && script_runtime_configure_messaging(
              opaque_message_runtime, true, false, true,
              count_test_message, &opaque_message_probe)
          && script_runtime_evaluate_diagnostic(
              opaque_message_runtime,
              "let domainBlocked=false;try{document.domain='example.test'}"
              "catch(error){domainBlocked=error instanceof DOMException&&"
              "error.name==='SecurityError'}parent.postMessage({event:"
              "'opaque'},'*');globalThis.pocSummary=location.origin==='null'"
              "&&origin==='null'&&opener===null&&open()===null&&domainBlocked"
              "&&(()=>{try{void localStorage;return false}catch(error){return "
              "error.name==='SecurityError'}})()"
              "&&(()=>{try{void sessionStorage;return false}catch(error){return "
              "error.name==='SecurityError'}})()"
              "&&document.cookie===''"
              "?'OPAQUE-ORIGIN-OK':'OPAQUE-ORIGIN-FAILED'",
              "https://opaque-child.test/probe.js",
              &opaque_message_result)
          && strcmp(opaque_message_result.summary,
                    "OPAQUE-ORIGIN-OK") == 0
          && opaque_message_probe.calls == 1);
    script_runtime_destroy(opaque_message_runtime);
    const char *retained_secret = NULL;
    size_t retained_secret_length = 0;
    CHECK(browser_session_storage_get(
              &opaque_message_session, opaque_url, true, "secret",
              &retained_secret, &retained_secret_length)
          && retained_secret_length == strlen("STORAGE_SECRET")
          && memcmp(retained_secret, "STORAGE_SECRET",
                    retained_secret_length) == 0);
    browser_session_destroy(&opaque_message_session);

    /* A failed child-global setup still completes the irreversible scope
       transition, but must not publish the proposed callback opaque. */
    ScriptResult failed_message_result = {0};
    ScriptRuntime *failed_message_runtime = script_runtime_create_deferred(
        &child_message_document, &budget, 2 * MIB, 1000,
        "https://failed-child.test/", NULL, &failed_message_result);
    MessageOpaqueProbe *failed_message_opaque =
        malloc(sizeof(*failed_message_opaque));
    CHECK(failed_message_runtime != NULL && failed_message_opaque != NULL);
    *failed_message_opaque = (MessageOpaqueProbe) {0};
    script_runtime_bound_allocator_cache(failed_message_runtime, 0, 0);
    (void) script_runtime_collect_and_trim(failed_message_runtime);
    budget_inject_failure_after(&budget, 0);
    bool failed_message_configured = script_runtime_configure_messaging(
        failed_message_runtime, true, false, false, count_test_message,
        failed_message_opaque);
    budget_clear_failure_injection(&budget);
    CHECK(!failed_message_configured);
    free(failed_message_opaque);
    failed_message_opaque = NULL;
    CHECK(!script_runtime_rebind_remote_document(
              failed_message_runtime, &legacy_child_binding)
          && !script_runtime_set_section_identity(
                 failed_message_runtime, 2)
          && !script_runtime_configure_messaging(
                 failed_message_runtime, false, true, false,
                 count_test_message, NULL)
          && script_runtime_evaluate_diagnostic(
                 failed_message_runtime,
                 "__tilefinchPostMessage(0,'{}','*');"
                 "globalThis.pocSummary='failed-child-safe'",
                 "<failed-child-post-message>", &failed_message_result)
          && strcmp(failed_message_result.summary,
                    "failed-child-safe") == 0);
    script_runtime_destroy(failed_message_runtime);
    document_destroy(&child_message_document);
    document_destroy(&parent_message_document);
    CHECK(budget.current == 0);

    return 0;
}
