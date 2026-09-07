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
    static NavigationSession disabled_navigation;
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
    static NavigationSession wpt_compatibility_navigation;
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
        "const readAll=async stream=>{const reader=stream.getReader(),chunks=[];"
        "let total=0;for(;;){const item=await reader.read();if(item.done)break;"
        "chunks.push(item.value);total+=item.value.byteLength;}const joined=new "
        "Uint8Array(total);let at=0;for(const chunk of chunks){joined.set(chunk,"
        "at);at+=chunk.byteLength;}return joined;};"
        "const compressionResult=(async()=>{let invalidFormat=false,malformed=false;"
        "try{new CompressionStream('br');}catch(error){invalidFormat=error instanceof "
        "TypeError;}const source=new Uint8Array(270000);for(let i=0;i<source.length;"
        "i++)source[i]=(i*29+7)&255;for(const format of ['deflate','deflate-raw',"
        "'gzip']){const compressor=new CompressionStream(format),writer=compressor."
        "writable.getWriter();await writer.write(source.subarray(0,90000));await "
        "writer.write(source.subarray(90000,180000));await writer.write(source."
        "subarray(180000));await writer.close();const packed=await readAll(compressor."
        "readable),decompressor=new DecompressionStream(format),inflateWriter="
        "decompressor.writable.getWriter();await inflateWriter.write(packed);await "
        "inflateWriter.close();const restored=await readAll(decompressor.readable);"
        "if(restored.length!==source.length) return false;for(let i=0;i<source.length;"
        "i+=997)if(restored[i]!==source[i])return false;}try{const broken=new "
        "DecompressionStream('gzip'),writer=broken.writable.getWriter();await writer."
        "write(new Uint8Array([1,2,3]));await writer.close();}catch(error){malformed="
        "error instanceof TypeError;}return invalidFormat&&malformed&&Object.prototype."
        "toString.call(new CompressionStream('gzip'))==='[object CompressionStream]'"
        "&&Object.prototype.toString.call(new DecompressionStream('gzip'))==="
        "'[object DecompressionStream]'&&Function.prototype.toString.call("
        "CompressionStream).includes('[native code]');})();"
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
        "Promise.all([readerResult,streamResult,compressionResult,response.json(),"
        "clone.text()]).then(values=>{globalThis.pocSummary="
        "fileShape&&bodyShape&&beaconShape&&values[0]&&values[1]"
        "&&values[2]&&values[3].ok&&values[4]==='q=bird'?'browser-io:ok':"
        "'browser-io:fail';}).catch(error=>globalThis.pocSummary="
        "'browser-io:error:'+error);"
        "</script></body>";
    static NavigationSession browser_io_navigation;
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
    FontSet canvas_fonts = {0};
    CHECK(font_set_load(
        &canvas_fonts, &budget, TILEFINCH_TEST_SANS_FONT,
        TILEFINCH_TEST_SERIF_FONT, TILEFINCH_TEST_SANS_ITALIC_FONT,
        TILEFINCH_TEST_SANS_BOLD_FONT, TILEFINCH_TEST_SERIF_BOLD_FONT,
        TILEFINCH_TEST_METRIC_SANS_FONT,
        TILEFINCH_TEST_METRIC_SANS_BOLD_FONT, 2 * MIB));
    static const char canvas_page[] =
        "<!doctype html><body><script>"
        "const canvas=document.createElement('canvas');canvas.width=32;"
        "canvas.height=16;document.body.appendChild(canvas);"
        "const context=canvas.getContext('2d');"
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
        "&&(()=>{const css=new DOMMatrixReadOnly('matrix(1, 0, 0, 1, 12, -4)'),"
        "list=new DOMMatrixReadOnly('translate(8px, -3px) scale(2)'),"
        "three=new DOMMatrixReadOnly('matrix3d(1,0,0,0,0,1,0,0,0,0,1,0,7,9,0,1)');"
        "let invalid=false;try{new DOMMatrixReadOnly('translate(20%)')}catch(error)"
        "{invalid=error.name==='SyntaxError'}return css.e===12&&css.f===-4"
        "&&list.a===2&&list.d===2&&list.e===8&&list.f===-3&&three.e===7"
        "&&three.f===9&&css.is2D&&Object.prototype.toString.call(css)==="
        "'[object DOMMatrixReadOnly]'&&invalid})()"
        "&&context.getLineDash().join(',')==='2,1'&&inside&&metrics.width>0"
        "&&gradient instanceof CanvasGradient"
        "&&gradientRight.data[0]>gradientLeft.data[0]"
        "&&red.data.join(',')==='255,0,0,255'"
        "&&blue.data.join(',')==='0,0,255,255'"
        "&&copied.data.join(',')==='255,0,0,255'"
        "&&dataURL.startsWith('data:image/png;base64,')"
        "&&png.charCodeAt(0)===137&&png.slice(1,4)==='PNG';"
        "const advanced=document.createElement('canvas');advanced.width=64;"
        "advanced.height=40;document.body.appendChild(advanced);"
        "const draw=advanced.getContext('2d');"
        "draw.font='bold 16px sans-serif';const narrow=draw.measureText('III'),"
        "wide=draw.measureText('WWW');draw.fillStyle='white';"
        "draw.fillText('W',1,17);let glyphInk=0;for(const value of "
        "draw.getImageData(0,0,20,20).data.filter((_,i)=>i%4===3))"
        "if(value)glyphInk++;draw.save();draw.beginPath();draw.roundRect(24,1,16,14,4);"
        "draw.clip();draw.fillStyle='red';draw.beginPath();draw.rect(20,0,24,20);"
        "draw.fill();"
        "const clippedOut=draw.getImageData(23,1,1,1).data[3],"
        "clippedIn=draw.getImageData(28,5,1,1).data[3];draw.restore();"
        "const boundedClip=document.createElement('canvas');boundedClip.width=8;"
        "boundedClip.height=8;const bounded=boundedClip.getContext('2d');"
        "for(let i=0;i<5;i++){bounded.beginPath();bounded.moveTo(0,0);"
        "bounded.lineTo(8,0);bounded.lineTo(4,8);bounded.closePath();bounded.clip();}"
        "bounded.fillRect(0,0,8,8);const fifthClipEmpty="
        "bounded.getImageData(4,2,1,1).data[3]===0;"
        "const sprite=document.createElement('canvas');sprite.width=2;"
        "sprite.height=1;const spriteContext=sprite.getContext('2d');"
        "spriteContext.fillStyle='red';spriteContext.fillRect(0,0,1,1);"
        "spriteContext.fillStyle='blue';spriteContext.fillRect(1,0,1,1);"
        "draw.imageSmoothingEnabled=true;draw.drawImage(sprite,0,0,2,1,0,24,3,1);"
        "const smooth=draw.getImageData(1,24,1,1).data;"
        "draw.imageSmoothingEnabled=false;draw.drawImage(sprite,0,0,2,1,0,26,4,1);"
        "const nearestLeft=draw.getImageData(0,26,1,1).data,"
        "nearestRight=draw.getImageData(3,26,1,1).data;"
        "draw.beginPath();draw.moveTo(46,4);draw.arcTo(58,4,58,16,5);"
        "draw.lineTo(58,16);draw.lineWidth=4;draw.lineCap='round';"
        "draw.lineJoin='round';draw.miterLimit=8;draw.setLineDash([3,2]);"
        "draw.strokeStyle='lime';draw.stroke();const strokeHit="
        "draw.isPointInStroke(47,4),dashProbe=new Path2D;"
        "dashProbe.moveTo(0,38);dashProbe.lineTo(20,38);draw.setLineDash([4,4]);"
        "const dashOn=draw.isPointInStroke(dashProbe,2,38),"
        "dashOff=draw.isPointInStroke(dashProbe,6,38);let captured=0,capturedMoves=0;"
        "const captureOther=document.createElement('div');"
        "document.body.appendChild(captureOther);advanced.addEventListener("
        "'gotpointercapture',()=>captured++);advanced.addEventListener("
        "'lostpointercapture',()=>captured++);advanced.addEventListener("
        "'pointermove',event=>{if(event.pointerId===1&&Number.isFinite("
        "event.offsetX)&&Number.isFinite(event.offsetY))capturedMoves++;});"
        "advanced.setPointerCapture(1);__tilefinchDispatchActivationHandle("
        "captureOther.__handle,1,10,12,0,0,1);"
        "advanced.releasePointerCapture(1);const advancedOk=wide.width>narrow.width"
        "&&wide.actualBoundingBoxAscent>0&&glyphInk>8&&glyphInk<300"
        "&&clippedOut===0&&clippedIn===255&&fifthClipEmpty"
        "&&smooth[0]>0&&smooth[2]>0"
        "&&nearestLeft[0]===255&&nearestLeft[2]===0"
        "&&nearestRight[0]===0&&nearestRight[2]===255"
        "&&strokeHit&&dashOn&&!dashOff&&draw.getLineDash().join(',')==='4,4'"
        "&&draw.miterLimit===8&&captured===2&&capturedMoves===1;"
        "const diagnosticBefore="
        "{...__tilefinchCanvasDiagnostics};for(let i=0;i<130;i++)"
        "draw.fillRect(i%64,30+(i%8),1,1);draw.getImageData(0,30,1,1);"
        "const rectangleBatchOk=__tilefinchCanvasDiagnostics.rectangleCommands"
        "-diagnosticBefore.rectangleCommands===130&&"
        "__tilefinchCanvasDiagnostics.rectangleBatches"
        "-diagnosticBefore.rectangleBatches===5;const imageBefore="
        "{...__tilefinchCanvasDiagnostics};for(let i=0;i<18;i++)"
        "draw.drawImage(sprite,40,30,2,1);draw.getImageData(40,30,1,1);"
        "const imageBatchOk=__tilefinchCanvasDiagnostics.imageCommands"
        "-imageBefore.imageCommands===18&&__tilefinchCanvasDiagnostics.imageBatches"
        "-imageBefore.imageBatches===2;"
        "const paintCanvas=document.createElement('canvas');paintCanvas.width=64;"
        "paintCanvas.height=24;const paint=paintCanvas.getContext('2d'),paintBefore="
        "{...__tilefinchCanvasDiagnostics};paint.beginPath();paint.rect(1,1,8,8);"
        "paint.fill();paint.font='10px sans-serif';paint.fillText('A',12,10);"
        "paint.beginPath();paint.moveTo(24,2);paint.lineTo(32,10);paint.stroke();"
        "paint.strokeText('B',36,10);paint.getImageData(0,0,1,1);"
        "const paintBatchOk=__tilefinchCanvasDiagnostics.paintCommands"
        "-paintBefore.paintCommands===4&&__tilefinchCanvasDiagnostics.paintBatches"
        "-paintBefore.paintBatches===1&&__tilefinchCanvasDiagnostics.pathRasters"
        "-paintBefore.pathRasters===2&&__tilefinchCanvasDiagnostics.textRasters"
        "-paintBefore.textRasters===2;"
        "const svgCanvas=document.createElement('canvas');svgCanvas.width=40;"
        "svgCanvas.height=16;const svg=svgCanvas.getContext('2d'),"
        "svgPath=new Path2D('M1 1h8v8h-8z'),movedPath=new Path2D;"
        "movedPath.addPath(svgPath,new DOMMatrix({a:1,b:0,c:0,d:1,e:12,f:0}));"
        "svg.fillStyle='rgb(0,255,0)';svg.fill(svgPath);svg.fill(movedPath);"
        "const svgFirst=svg.getImageData(4,4,1,1).data,"
        "svgMoved=svg.getImageData(16,4,1,1).data,"
        "svgPathOk=svgFirst[1]===255&&svgMoved[1]===255"
        "&&svg.isPointInPath(svgPath,4,4)&&svg.isPointInPath(movedPath,16,4);"
        "const curveTextCanvas=document.createElement('canvas'),"
        "curveApiCanvas=document.createElement('canvas');curveTextCanvas.width="
        "curveApiCanvas.width=48;curveTextCanvas.height=curveApiCanvas.height=24;"
        "const curveTextContext=curveTextCanvas.getContext('2d'),"
        "curveApiContext=curveApiCanvas.getContext('2d'),curveText=new Path2D("
        "'M2 12Q6 2 10 12T18 12C20 2 24 2 26 12S32 22 34 12M36 12A4 4 0 0 1 44 12'),"
        "curveApi=new Path2D;curveApi.moveTo(2,12);"
        "curveApi.quadraticCurveTo(6,2,10,12);"
        "curveApi.quadraticCurveTo(14,22,18,12);"
        "curveApi.bezierCurveTo(20,2,24,2,26,12);"
        "curveApi.bezierCurveTo(28,22,32,22,34,12);curveApi.moveTo(36,12);"
        "curveApi.ellipse(40,12,4,4,0,Math.PI,Math.PI*2);"
        "curveTextContext.strokeStyle=curveApiContext.strokeStyle='white';"
        "curveTextContext.stroke(curveText);curveApiContext.stroke(curveApi);"
        "const curveTextPixels=curveTextContext.getImageData(0,0,48,24).data,"
        "curveApiPixels=curveApiContext.getImageData(0,0,48,24).data;"
        "let svgCurveOk=true;for(let i=0;i<curveTextPixels.length;i++)"
        "if(curveTextPixels[i]!==curveApiPixels[i]){svgCurveOk=false;break;}"
        "const shadowCanvas=document.createElement('canvas');shadowCanvas.width=24;"
        "shadowCanvas.height=12;const shadow=shadowCanvas.getContext('2d'),"
        "shadowBefore={...__tilefinchCanvasDiagnostics};shadow.fillStyle='red';"
        "shadow.shadowColor='rgb(0,255,0)';shadow.shadowOffsetX=8;"
        "shadow.fillRect(1,1,4,4);const shadowSource="
        "shadow.getImageData(2,2,1,1).data,shadowPixel="
        "shadow.getImageData(10,2,1,1).data,shadowOk=shadowSource[0]===255"
        "&&shadowSource[1]===0&&shadowPixel[0]===0&&shadowPixel[1]===255"
        "&&__tilefinchCanvasDiagnostics.shadowCommands"
        "-shadowBefore.shadowCommands===1;"
        "canvas.toBlob(blob=>blob.arrayBuffer().then(buffer=>{"
        "const bytes=new Uint8Array(buffer);globalThis.pocSummary="
        "syncOk&&advancedOk&&rectangleBatchOk&&imageBatchOk&&paintBatchOk"
        "&&svgPathOk&&svgCurveOk&&shadowOk&&blob.type==='image/png'"
        "&&bytes[0]===137?"
        "'canvas:ok':'canvas:fail:'+"
        "[syncOk,advancedOk,rectangleBatchOk,imageBatchOk,paintBatchOk,svgPathOk,"
        "svgCurveOk,shadowOk,blob.type,bytes[0],"
        "strokeHit,dashOn,dashOff,draw.getLineDash().join(',')].join('|');}));"
        "</script></body>";
    static NavigationSession canvas_navigation;
    CHECK(navigation_init(&canvas_navigation, &budget, 2));
    navigation_enable_scripts(&canvas_navigation, 6 * MIB, 1000);
    uint64_t canvas_generation = navigation_begin(&canvas_navigation);
    CHECK(navigation_commit_html(
        &canvas_navigation, canvas_generation,
        "https://canvas.test/", canvas_page, strlen(canvas_page),
        480, &canvas_fonts, NULL, true));
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
    font_set_destroy(&canvas_fonts);
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
    static NavigationSession native_controls_navigation;
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
        "const selfTiming=performance.getEntriesByName("
        "'https://order.test/dynamic.js','resource')[0];"
        "order.push(selfTiming&&Object.prototype.toString.call(selfTiming)==="
        "'[object PerformanceResourceTiming]'&&Object.keys(selfTiming).length===0"
        "&&selfTiming.initiatorType==='script'&&selfTiming.decodedBodySize>0"
        "?'dynamic-external':'dynamic-timing-missing')";
    static BrowserSession ordered_browser_session;
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
    static NavigationSession ordered_navigation;
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
        "<!doctype html><body><input id='frame-check' type='checkbox'>"
        "<div id='frame-pointer' style='position:absolute;left:100px;top:50px;"
        "width:80px;height:30px'>"
        "<span id='frame-pointer-ink'>POINTER</span></div>"
        "<script>globalThis.pocSummary='child-ready';"
        "document.getElementById('frame-check').addEventListener('click',"
        "()=>globalThis.pocSummary='frame-clicked');"
        "document.getElementById('frame-pointer').addEventListener('click',"
        "()=>globalThis.pocSummary='frame-pointer-clicked')</script></body>";
    static BrowserSession frame_owner_session;
    static NavigationSession frame_owner_navigation;
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
    size_t early_message_drops =
        frame_owner_navigation.frame_messages_dropped;
    CHECK(owned_frame->runtime == NULL && !owned_frame->loaded
          && !owned_frame->retired && !owned_frame->fetch_pending
          && script_runtime_evaluate_diagnostic(
              frame_owner_navigation.page.runtime,
              "document.getElementById('owned').contentWindow.postMessage("
              "{early:true},'*')",
              "https://frame-owner.test/early-message-send.js",
              &frame_owner_navigation.page.script_result)
          && frame_owner_navigation.page.message_count == 1u);
    FetchScheduler *early_scheduler =
        frame_owner_navigation.page.resource_scheduler;
    frame_owner_navigation.page.resource_scheduler = NULL;
    CHECK(navigation_advance_runtime(&frame_owner_navigation, 0, 4)
          && frame_owner_navigation.page.message_count == 0u
          && frame_owner_navigation.frame_messages_dropped
                 == early_message_drops + 1u);
    /* Reuse the bounded slot as a successful child. A failed child must not
       poison delivery for a later unrelated browsing context. */
    early_message_drops = frame_owner_navigation.frame_messages_dropped;
    CHECK(script_runtime_set_frame_window_state(
              frame_owner_navigation.page.runtime,
              owned_frame->parent_handle, true, true, false,
              "https://frame-owner.test/child.html",
              &frame_owner_navigation.page.script_result));
    CHECK(document_parse(&owned_frame->document, &budget,
                         owned_child_page, sizeof(owned_child_page) - 1, 16));
    owned_frame->runtime = script_runtime_create_deferred(
        &owned_frame->document, &budget, 4 * MIB, 1000,
        "https://frame-owner.test/child.html", NULL,
        &owned_frame->script_result);
    CHECK(owned_frame->runtime != NULL
          && navigation_test_configure_frame_messaging(
              &frame_owner_navigation, owned_frame));
    CHECK(script_runtime_evaluate_diagnostic(
              owned_frame->runtime,
              "addEventListener('message',event=>{if(event.data.early)"
              "globalThis.pocSummary='early-message-delivered'})",
              "https://frame-owner.test/early-message.js",
              &owned_frame->script_result)
          && script_runtime_evaluate_diagnostic(
              frame_owner_navigation.page.runtime,
              "document.getElementById('owned').contentWindow.postMessage("
              "{early:true},'*')",
              "https://frame-owner.test/replacement-message-send.js",
              &frame_owner_navigation.page.script_result)
          && frame_owner_navigation.page.message_count == 1u);
    owned_frame->loaded = true;
    frame_owner_navigation.page.resource_scheduler = NULL;
    CHECK(navigation_advance_runtime(&frame_owner_navigation, 0, 4)
          && frame_owner_navigation.page.message_count == 0u
          && frame_owner_navigation.frame_messages_dropped
                 == early_message_drops
          && strcmp(owned_frame->script_result.summary,
                    "early-message-delivered") == 0);
    frame_owner_navigation.page.resource_scheduler = early_scheduler;
    CHECK(script_runtime_evaluate_diagnostic(
              owned_frame->runtime,
              "const original=parent.postMessage;parent.postMessage=()=>{};"
              "globalThis.pocSummary=parent.postMessage===original?"
              "'same-origin-blocked':'same-origin-writable'",
              "https://frame-owner.test/same-origin-window.js",
              &owned_frame->script_result)
          && strcmp(owned_frame->script_result.summary,
                    "same-origin-writable") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              owned_frame->runtime,
              "document.getElementById('frame-check').addEventListener("
              "'click',()=>globalThis.pocSummary='frame-clicked')",
              "https://frame-owner.test/frame-activation.js",
              &owned_frame->script_result));
    frame_owner_navigation.images = &frame_owner_navigation.page.images;
    CHECK(navigation_test_refresh_frame_presentation(
        &frame_owner_navigation, owned_frame));
    CHECK(owned_frame->presentation != NULL);
    CHECK(images_find_node(
        &frame_owner_navigation.page.images,
        owned_frame->element) != NULL);
    CHECK(navigation_activate_frame(
        &frame_owner_navigation, owned_frame->element));
    CHECK(strcmp(owned_frame->script_result.summary,
                 "frame-clicked") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
        owned_frame->runtime,
        "document.getElementById('frame-pointer-ink').addEventListener('click',()=>"
        "globalThis.pocSummary='frame-pointer-clicked')",
        "https://frame-owner.test/frame-pointer-activation.js",
        &owned_frame->script_result));
    CHECK(navigation_activate_frame_at(
        &frame_owner_navigation, owned_frame->element, 110, 60));
    CHECK(strcmp(owned_frame->script_result.summary,
                 "frame-pointer-clicked") == 0);
    /* Queue one complete legacy-capacity burst before delivery.  The first
       child task replies three times while fifteen messages remain queued.
       postMessage must queue all three replies rather than exposing the
       bounded native queue as an author-visible InternalError. */
    size_t posted_before = frame_owner_navigation.frame_messages_posted;
    size_t delivered_before = frame_owner_navigation.frame_messages_delivered;
    size_t dropped_burst_before = frame_owner_navigation.frame_messages_dropped;
    CHECK(navigation_test_configure_frame_messaging(
              &frame_owner_navigation, owned_frame)
          && script_runtime_evaluate_diagnostic(
              owned_frame->runtime,
              "addEventListener('message',event=>{if(event.data.trigger){"
              "parent.postMessage({reply:1},'*');"
              "parent.postMessage({reply:2},'*');"
              "parent.postMessage({reply:3},'*')}})",
              "https://frame-owner.test/reply-burst.js",
              &owned_frame->script_result)
          && script_runtime_evaluate_diagnostic(
              frame_owner_navigation.page.runtime,
              "globalThis.replyBurst=0;"
              "addEventListener('message',event=>{if(event.data.reply)"
              "replyBurst++;globalThis.pocSummary=String(replyBurst)});"
              "const target=document.getElementById('owned').contentWindow;"
              "target.postMessage({trigger:true},'*');"
              "for(let i=0;i<15;i++)target.postMessage({filler:i},'*')",
              "https://frame-owner.test/queue-burst.js",
              &frame_owner_navigation.page.script_result)
          && frame_owner_navigation.page.message_count == 16u
          && navigation_advance_runtime(&frame_owner_navigation, 1, 64)
          && frame_owner_navigation.page.message_count == 0u
          && frame_owner_navigation.frame_messages_posted
                 == posted_before + 19u
          && frame_owner_navigation.frame_messages_delivered
                 == delivered_before + 19u
          && frame_owner_navigation.frame_messages_dropped
                 == dropped_burst_before
          && owned_frame->script_result.uncaught_callback_errors == 0u
          && strcmp(frame_owner_navigation.page.script_result.summary,
                    "3") == 0);
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
          && frame_owner_navigation.frame_messages_dropped == 1
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
          && navigation_test_configure_frame_messaging(
              &frame_owner_navigation, owned_frame));
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
          && frame_owner_navigation.frame_messages_dropped
                 == early_message_drops + 1u
          && frame_owner_navigation.page.message_count == 0
          && owned_frame->retired && owned_frame->runtime == NULL
          && owned_frame->document.html == NULL
          && script_runtime_evaluate_diagnostic(
              frame_owner_navigation.page.runtime,
              "savedOwnedWindow.postMessage({late:true},'*');"
              "globalThis.pocSummary=savedOwnedWindow.closed?"
              "'FRAME-DETACHED-OK':'FRAME-DETACHED-FAILED'",
              "https://frame-owner.test/check-frame.js",
              &frame_owner_navigation.page.script_result)
          && frame_owner_navigation.page.message_count == 0
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

    /* A child realm mutates its owned document before the bounded refresh
       fails.  The parent page must be retired too: its frame tables and
       controller/layout state otherwise retain pointers into that poisoned
       child graph. */
    script_runtime_destroy(owned_frame->runtime);
    owned_frame->runtime = NULL;
    document_destroy(&owned_frame->document);
    CHECK(document_parse(&owned_frame->document, &budget,
                         owned_child_page, sizeof(owned_child_page) - 1, 16));
    owned_frame->runtime = script_runtime_create_deferred(
        &owned_frame->document, &budget, 2 * MIB, 1000,
        "https://frame-owner.test/refresh-child.html", NULL,
        &owned_frame->script_result);
    CHECK(owned_frame->runtime != NULL
          && script_runtime_evaluate_diagnostic(
              owned_frame->runtime,
              "setTimeout(()=>document.body.innerHTML='<p>changed</p>',0)",
              "https://frame-owner.test/refresh-failure.js",
              &owned_frame->script_result));
    owned_frame->loaded = true;
    script_runtime_test_refuse_next_document_refresh();
    CHECK(!navigation_advance_runtime(&frame_owner_navigation, 1, 8)
          && !frame_owner_navigation.page.loaded
          && frame_owner_navigation.page.document.html == NULL
          && frame_owner_navigation.page.runtime == NULL
          && frame_owner_navigation.page.layout.count == 0u
          && strstr(frame_owner_navigation.last_error,
                    "refresh mutated DOM") != NULL);
    navigation_destroy(&frame_owner_navigation);
    browser_session_destroy(&frame_owner_session);
    CHECK(budget.current == 0);

    puts("test: top navigation outranks outgoing child failure");
    static const char navigation_wins_page[] =
        "<!doctype html><body><iframe src='/outgoing.html'></iframe></body>";
    static const char navigation_wins_child[] =
        "<!doctype html><body>outgoing child</body>";
    static BrowserSession navigation_wins_session;
    static NavigationSession navigation_wins;
    CHECK(browser_session_init(&navigation_wins_session, &budget, 64 * 1024)
          && navigation_init(&navigation_wins, &budget, 2));
    navigation_attach_browser_session(&navigation_wins,
                                      &navigation_wins_session);
    navigation_enable_scripts(&navigation_wins, 4 * MIB, 1000);
    navigation_enable_document_scripts(&navigation_wins, 4,
                                       64 * 1024, 64 * 1024, 1000);
    uint64_t navigation_wins_generation = navigation_begin(&navigation_wins);
    CHECK(navigation_commit_html(
        &navigation_wins, navigation_wins_generation,
        "https://navigation-wins.test/", navigation_wins_page,
        sizeof(navigation_wins_page) - 1, 480, NULL, NULL, true));
    for (size_t tick = 0;
         tick < 8 && navigation_wins.frames_failed == 0; tick++) {
        CHECK(navigation_advance_runtime(&navigation_wins, 1, 8));
    }
    CHECK(navigation_wins.page.frame_count == 1
          && navigation_wins.frames_failed == 1);
    NavigationFrame *navigation_wins_frame = &navigation_wins.page.frames[0];
    CHECK(document_parse(&navigation_wins_frame->document, &budget,
                         navigation_wins_child,
                         sizeof(navigation_wins_child) - 1, 16));
    navigation_wins_frame->runtime = script_runtime_create_deferred(
        &navigation_wins_frame->document, &budget, 2 * MIB, 1000,
        "https://navigation-wins.test/outgoing.html", NULL,
        &navigation_wins_frame->script_result);
    CHECK(navigation_wins_frame->runtime != NULL);
    navigation_wins_frame->loaded = true;
    CHECK(script_runtime_evaluate_diagnostic(
              navigation_wins.page.runtime,
              "setTimeout(()=>location.replace("
              "'https://stream.test/document'),0)",
              "https://navigation-wins.test/request-replacement.js",
              &navigation_wins.page.script_result)
          && script_runtime_evaluate_diagnostic(
              navigation_wins_frame->runtime,
              "setTimeout(()=>{for(;;){}},0)",
              "https://navigation-wins.test/stale-child.js",
              &navigation_wins_frame->script_result));
    char navigation_wins_error[256] = {0};
    bool navigation_wins_replay = fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-stream",
        navigation_wins_error, sizeof(navigation_wins_error));
    uint64_t navigation_wins_before = navigation_wins.generation;
    bool navigation_wins_advanced = navigation_wins_replay
        && navigation_advance_runtime(&navigation_wins, 1, 8);
    /* The replay intentionally lacks the contextual Referer-bearing request,
       so replacement transport fails.  Dispatch itself must nevertheless win
       the tick: the generation advances and the stale child error is never
       surfaced.  Before the fix, the child timeout fires first, leaves the
       generation unchanged, and reports "frame scheduler failed". */
    CHECK(navigation_wins_replay && !navigation_wins_advanced
          && navigation_wins.generation == navigation_wins_before + 1u
          && navigation_wins.page.loaded
          && strstr(navigation_wins.last_error,
                    "frame scheduler failed") == NULL
          && strstr(navigation_wins.last_error,
                    "does not match any unclaimed record") != NULL);
    if (navigation_wins_replay) fetch_trace_end();
    navigation_destroy(&navigation_wins);
    browser_session_destroy(&navigation_wins_session);
    CHECK(budget.current == 0);

    puts("test: nested iframe presentation and activation recurse by realm");
    static const char nested_frame_page[] =
        "<!doctype html><body><iframe id='outer' src='/outer.html'>"
        "</iframe></body>";
    static const char nested_outer_page[] =
        "<!doctype html><body><iframe id='inner' src='/inner.html'>"
        "</iframe></body>";
    static const char nested_inner_page[] =
        "<!doctype html><body><input id='nested-check' type='checkbox'>"
        "<script>document.getElementById('nested-check').addEventListener("
        "'click',()=>globalThis.pocSummary='nested-clicked')</script></body>";
    static BrowserSession nested_frame_session;
    static NavigationSession nested_frame_navigation;
    MessageOpaqueProbe nested_frame_probe = {0};
    CHECK(browser_session_init(&nested_frame_session, &budget, 64 * 1024)
          && navigation_init(&nested_frame_navigation, &budget, 4));
    navigation_attach_browser_session(&nested_frame_navigation,
                                      &nested_frame_session);
    navigation_enable_scripts(&nested_frame_navigation, 6 * MIB, 1000);
    navigation_enable_document_scripts(&nested_frame_navigation, 8,
                                       128 * 1024, 128 * 1024, 1000);
    uint64_t nested_frame_generation = navigation_begin(
        &nested_frame_navigation);
    CHECK(navigation_commit_html(
              &nested_frame_navigation, nested_frame_generation,
              "https://nested-frame.test/", nested_frame_page,
              strlen(nested_frame_page), 480, NULL, NULL, true)
          && navigation_advance_runtime(&nested_frame_navigation, 1, 8)
          && nested_frame_navigation.page.frame_count == 1);
    NavigationFrame *outer_frame =
        &nested_frame_navigation.page.frames[0];
    if (outer_frame->fetch_pending) {
        CHECK(fetch_scheduler_cancel(
                  nested_frame_navigation.page.resource_scheduler,
                  outer_frame->fetch_request_id, "test supplies child")
              && fetch_scheduler_discard(
                  nested_frame_navigation.page.resource_scheduler,
                  outer_frame->fetch_request_id));
        outer_frame->fetch_pending = false;
    }
    if (outer_frame->retired) {
        outer_frame->retired = false;
        CHECK(script_runtime_set_frame_window_state(
            nested_frame_navigation.page.runtime,
            outer_frame->parent_handle, true, true, false,
            "https://nested-frame.test/outer.html",
            &nested_frame_navigation.page.script_result));
    }
    CHECK(outer_frame->parent_runtime
              == nested_frame_navigation.page.runtime
          && outer_frame->parent_frame_slot_plus_one == 0
          && document_parse(&outer_frame->document, &budget,
                            nested_outer_page,
                            sizeof(nested_outer_page) - 1, 16));
    outer_frame->runtime = script_runtime_create_deferred(
        &outer_frame->document, &budget, 2 * MIB, 1000,
        "https://nested-frame.test/outer.html", NULL,
        &outer_frame->script_result);
    CHECK(outer_frame->runtime != NULL
          && script_runtime_configure_messaging(
              outer_frame->runtime, true, true, false, true,
              count_test_message, &nested_frame_probe));
    script_runtime_set_images(outer_frame->runtime, &outer_frame->images);
    outer_frame->loaded = true;
    nested_frame_navigation.images = &nested_frame_navigation.page.images;
    bool outer_presented = navigation_test_refresh_frame_presentation(
        &nested_frame_navigation, outer_frame);
    bool outer_advanced = outer_presented
        && navigation_advance_runtime(&nested_frame_navigation, 1, 8);
    if (!outer_presented || !outer_advanced
        || nested_frame_navigation.page.frame_count != 2) {
        fprintf(stderr,
                "nested discover presented=%d advanced=%d count=%zu "
                "failed=%zu scheduler=%p outer-retired=%d outer-runtime=%p\n",
                outer_presented, outer_advanced,
                nested_frame_navigation.page.frame_count,
                nested_frame_navigation.frames_failed,
                (void *) nested_frame_navigation.page.resource_scheduler,
                outer_frame->retired, (void *) outer_frame->runtime);
    }
    CHECK(outer_presented && outer_advanced
          && nested_frame_navigation.page.frame_count == 2);
    NavigationFrame *inner_frame =
        &nested_frame_navigation.page.frames[1];
    if (inner_frame->fetch_pending) {
        CHECK(fetch_scheduler_cancel(
                  nested_frame_navigation.page.resource_scheduler,
                  inner_frame->fetch_request_id, "test supplies nested child")
              && fetch_scheduler_discard(
                  nested_frame_navigation.page.resource_scheduler,
                  inner_frame->fetch_request_id));
        inner_frame->fetch_pending = false;
    }
    if (inner_frame->retired) {
        inner_frame->retired = false;
        CHECK(script_runtime_set_frame_window_state(
            outer_frame->runtime, inner_frame->parent_handle,
            true, true, false,
            "https://nested-frame.test/inner.html",
            &outer_frame->script_result));
    }
    CHECK(inner_frame->parent_runtime == outer_frame->runtime
          && inner_frame->parent_frame_slot_plus_one == 1
          && document_parse(&inner_frame->document, &budget,
                            nested_inner_page,
                            sizeof(nested_inner_page) - 1, 16));
    inner_frame->runtime = script_runtime_create_deferred(
        &inner_frame->document, &budget, 2 * MIB, 1000,
        "https://nested-frame.test/inner.html", NULL,
        &inner_frame->script_result);
    CHECK(inner_frame->runtime != NULL
          && script_runtime_configure_messaging(
              inner_frame->runtime, true, true, false, false,
              count_test_message, &nested_frame_probe)
          && script_runtime_evaluate_diagnostic(
              inner_frame->runtime,
              "top.postMessage({event:'nested-top'},'*');"
              "globalThis.pocSummary=top!==parent?'nested-top-distinct':"
              "'nested-top-aliased'",
              "https://nested-frame.test/nested-top.js",
              &inner_frame->script_result)
          && strcmp(inner_frame->script_result.summary,
                    "nested-top-distinct") == 0
          && nested_frame_probe.calls == 1
          && nested_frame_probe.last_target == -1
          && script_runtime_evaluate_diagnostic(
              inner_frame->runtime,
              "document.getElementById('nested-check').addEventListener("
              "'click',()=>globalThis.pocSummary='nested-clicked')",
              "https://nested-frame.test/nested-activation.js",
              &inner_frame->script_result));
    script_runtime_set_images(inner_frame->runtime, &inner_frame->images);
    inner_frame->loaded = true;
    bool inner_presented = navigation_test_refresh_frame_presentation(
        &nested_frame_navigation, inner_frame);
    bool inner_image = images_find_node(
        &outer_frame->images, inner_frame->element) != NULL;
    bool outer_image = images_find_node(
        &nested_frame_navigation.page.images, outer_frame->element) != NULL;
    bool nested_activated = navigation_activate_frame(
        &nested_frame_navigation, outer_frame->element);
    if (!inner_presented || !inner_image || !outer_image || !nested_activated
        || strcmp(inner_frame->script_result.summary, "nested-clicked") != 0) {
        fprintf(stderr,
                "nested activation presented=%d inner-image=%d outer-image=%d "
                "activated=%d summary=%s\n",
                inner_presented, inner_image, outer_image, nested_activated,
                inner_frame->script_result.summary);
    }
    CHECK(inner_presented && inner_image && outer_image && nested_activated
          && strcmp(inner_frame->script_result.summary,
                    "nested-clicked") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              nested_frame_navigation.page.runtime,
              "document.getElementById('outer').remove()",
              "https://nested-frame.test/remove-ancestor.js",
              &nested_frame_navigation.page.script_result)
          && navigation_advance_runtime(&nested_frame_navigation, 1, 8)
          && outer_frame->retired && outer_frame->runtime == NULL
          && outer_frame->document.html == NULL
          && inner_frame->retired && inner_frame->runtime == NULL
          && inner_frame->document.html == NULL);
    navigation_destroy(&nested_frame_navigation);
    browser_session_destroy(&nested_frame_session);
    CHECK(budget.current == 0);

    puts("test: script pressure admission skips before allocator failure");
    static const char pressure_script_page[] =
        "<!doctype html><title>Pressure</title><body><p>READY</p>"
        "<script>globalThis.inlinePressureScriptRan=true</script>"
        "<script src='/pressure.js' defer></script></body>";
    static const unsigned char pressure_script_source[] =
        "globalThis.pocSummary='pressure-script-ran'";
    static BrowserSession pressure_browser_session;
    static NavigationSession pressure_navigation;
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
    static BrowserSession import_session;
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

    puts("test: bounded selector admission retains attribute-only rules");
    PocDocument attribute_document;
    Stylesheet attribute_sheet;
    CHECK(document_parse(
              &attribute_document, &budget,
              "<!doctype html><div id=attribute data-probe></div>",
              strlen("<!doctype html><div id=attribute data-probe></div>"),
              17)
          && stylesheet_build(
                 &attribute_sheet, &budget, &attribute_document, 480));
    size_t attribute_start = attribute_sheet.count;
    uint32_t empty_bloom[2] = {0};
    attribute_sheet.source_rule_head_limit_end = attribute_start;
    attribute_sheet.source_rule_relevant_limit_end = attribute_start + 1u;
    attribute_sheet.source_rule_limit_end = attribute_start + 1u;
    attribute_sheet.source_rule_token_bloom = empty_bloom;
    attribute_sheet.source_rule_token_bloom_words = 2u;
    CHECK(stylesheet_add_css(
              &attribute_sheet, "[data-probe]{padding-left:11px}",
              strlen("[data-probe]{padding-left:11px}")));
    attribute_sheet.source_rule_limit_end = 0;
    attribute_sheet.source_rule_head_limit_end = 0;
    attribute_sheet.source_rule_relevant_limit_end = 0;
    attribute_sheet.source_rule_token_bloom = NULL;
    attribute_sheet.source_rule_token_bloom_words = 0;
    lxb_dom_node_t *attribute = find_id(
        lxb_dom_interface_node(attribute_document.html), "attribute");
    ComputedStyle attribute_style = style_for_node(
        &attribute_sheet, attribute, NULL);
    CHECK(attribute != NULL
          && attribute_sheet.count == attribute_start + 1u
          && attribute_style.padding.left == 11);
    stylesheet_destroy(&attribute_sheet);
    document_destroy(&attribute_document);
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

    puts("test: failed bootstrap reveals a text-rich staged body only");
    static const char staged_body_page[] =
        "<!doctype html><style>html{visibility:hidden}body{display:none;opacity:0}</style><body>"
        "<header><h1>Published server heading</h1></header><main><p>"
        "This complete server-rendered introduction remains useful when the "
        "optional bootstrap cannot add its ready class. It contains enough "
        "ordinary prose to distinguish a page shell from an empty loader."
        "</p></main><aside id=staged-dialog style='display:none'>"
        "<h2>Closed dialog must stay hidden</h2></aside></body>";
    PocDocument staged_body_document;
    Stylesheet staged_body_sheet;
    CHECK(document_parse(&staged_body_document, &budget, staged_body_page,
                         strlen(staged_body_page), 18)
          && stylesheet_build(&staged_body_sheet, &budget,
                              &staged_body_document, 480));
    lxb_dom_node_t *staged_body = document_body_node(&staged_body_document);
    lxb_dom_node_t *staged_dialog = find_id(
        lxb_dom_interface_node(staged_body_document.html), "staged-dialog");
    ComputedStyle staged_body_before = style_for_node(
        &staged_body_sheet, staged_body, NULL);
    lxb_dom_node_t *staged_html = staged_body == NULL
        ? NULL : staged_body->parent;
    ComputedStyle staged_html_before = style_for_node(
        &staged_body_sheet, staged_html, NULL);
    CHECK(staged_body_before.display == DISPLAY_NONE
          && staged_body_before.opacity == 0
          && staged_html_before.visibility_hidden);
    stylesheet_enable_static_custom_element_fallback(&staged_body_sheet);
    ComputedStyle staged_html_after = style_for_node(
        &staged_body_sheet, staged_html, NULL);
    ComputedStyle staged_body_after = style_for_node(
        &staged_body_sheet, staged_body, NULL);
    CHECK(staged_html_after.display == DISPLAY_BLOCK
          && !staged_html_after.hidden
          && !staged_html_after.visibility_hidden
          && staged_body_after.display == DISPLAY_BLOCK
          && !staged_body_after.hidden
          && !staged_body_after.visibility_hidden
          && staged_body_after.opacity == 255
          && staged_dialog != NULL
          && style_for_node(
                 &staged_body_sheet, staged_dialog, &staged_body_after).display
                 == DISPLAY_NONE);
    stylesheet_destroy(&staged_body_sheet);
    document_destroy(&staged_body_document);
    CHECK(budget.current == 0);

    puts("test: staged root recovery respects a bare hidden body");
    static const char bare_hidden_body_page[] =
        "<!doctype html><body hidden><main>"
        "<h1>Explicitly hidden server document</h1><p>"
        "This complete text-rich document deliberately uses the boolean hidden "
        "attribute on its body, so static script fallback must preserve that "
        "authored state even though the attribute has no explicit value."
        "</p></main></body>";
    PocDocument bare_hidden_body_document;
    Stylesheet bare_hidden_body_sheet;
    CHECK(document_parse(
              &bare_hidden_body_document, &budget, bare_hidden_body_page,
              strlen(bare_hidden_body_page), 12u)
          && stylesheet_build(
              &bare_hidden_body_sheet, &budget,
              &bare_hidden_body_document, 480));
    lxb_dom_node_t *bare_hidden_body = document_body_node(
        &bare_hidden_body_document);
    stylesheet_enable_static_custom_element_fallback(
        &bare_hidden_body_sheet);
    ComputedStyle bare_hidden_body_style = style_for_node(
        &bare_hidden_body_sheet, bare_hidden_body, NULL);
    CHECK(bare_hidden_body != NULL
          && lxb_dom_element_has_attribute(
                 lxb_dom_interface_element(bare_hidden_body),
                 (const lxb_char_t *) "hidden", 6u)
          && bare_hidden_body_style.display == DISPLAY_NONE);
    stylesheet_destroy(&bare_hidden_body_sheet);
    document_destroy(&bare_hidden_body_document);
    CHECK(budget.current == 0);

    puts("test: staged root recovery preserves geometry hiding");
    static const char transformed_body_page[] =
        "<!doctype html><style>body{display:none;transform:scale(0)}"
        "</style><body><main><h1>Geometry-hidden document</h1><p>"
        "This otherwise readable server document deliberately carries an "
        "independent zero-scale transform which static recovery must not "
        "erase merely because the same root also uses display none."
        "</p></main></body>";
    PocDocument transformed_body_document;
    Stylesheet transformed_body_sheet;
    CHECK(document_parse(
              &transformed_body_document, &budget, transformed_body_page,
              strlen(transformed_body_page), 16u)
          && stylesheet_build(
              &transformed_body_sheet, &budget,
              &transformed_body_document, 480));
    lxb_dom_node_t *transformed_body = document_body_node(
        &transformed_body_document);
    stylesheet_enable_static_custom_element_fallback(
        &transformed_body_sheet);
    ComputedStyle transformed_body_style = style_for_node(
        &transformed_body_sheet, transformed_body, NULL);
    if (!(transformed_body_style.display == DISPLAY_NONE
          && transformed_body_style.hidden
          && transformed_body_style.has_transform
          && transformed_body_style.transform_scale_q6 == 0)) {
        fprintf(stderr,
                "staged geometry display=%d hidden=%d transform=%d scale=%u\n",
                transformed_body_style.display, transformed_body_style.hidden,
                transformed_body_style.has_transform,
                transformed_body_style.transform_scale_q6);
    }
    CHECK(transformed_body_style.display == DISPLAY_NONE
          && transformed_body_style.hidden
          && transformed_body_style.has_transform
          && transformed_body_style.transform_scale_q6 == 0);
    stylesheet_destroy(&transformed_body_sheet);
    document_destroy(&transformed_body_document);
    CHECK(budget.current == 0);

    puts("test: staged root recovery skips a large document head");
    const size_t staged_head_capacity = 48u * 1024u;
    char *staged_head_page = malloc(staged_head_capacity);
    CHECK(staged_head_page != NULL);
    size_t staged_head_used = (size_t) snprintf(
        staged_head_page, staged_head_capacity,
        "<!doctype html><style>html{display:none}</style><head>");
    for (size_t i = 0; i < 600u; i++) {
        int wrote = snprintf(
            staged_head_page + staged_head_used,
            staged_head_capacity - staged_head_used,
            "<meta name='fixture-%zu' content='bounded'>", i);
        CHECK(wrote > 0 && (size_t) wrote
              < staged_head_capacity - staged_head_used);
        staged_head_used += (size_t) wrote;
    }
    int staged_head_tail = snprintf(
        staged_head_page + staged_head_used,
        staged_head_capacity - staged_head_used,
        "</head><body><main><h1>Useful body after a large head</h1>"
        "<p>This complete server-rendered body must remain discoverable even "
        "when hundreds of metadata nodes precede it in the document head. "
        "The fallback scan skips that non-rendered subtree without growing "
        "its fixed work bound.</p></main></body>");
    CHECK(staged_head_tail > 0 && (size_t) staged_head_tail
          < staged_head_capacity - staged_head_used);
    staged_head_used += (size_t) staged_head_tail;
    PocDocument staged_head_document;
    Stylesheet staged_head_sheet;
    CHECK(document_parse(&staged_head_document, &budget, staged_head_page,
                         staged_head_used, 1024u)
          && stylesheet_build(&staged_head_sheet, &budget,
                              &staged_head_document, 480));
    lxb_dom_node_t *staged_head_body = document_body_node(
        &staged_head_document);
    lxb_dom_node_t *staged_head_html = staged_head_body == NULL
        ? NULL : staged_head_body->parent;
    CHECK(style_for_node(
              &staged_head_sheet, staged_head_html, NULL).display
              == DISPLAY_NONE);
    stylesheet_enable_static_custom_element_fallback(&staged_head_sheet);
    CHECK(style_for_node(
              &staged_head_sheet, staged_head_html, NULL).display
              == DISPLAY_BLOCK);
    stylesheet_destroy(&staged_head_sheet);
    document_destroy(&staged_head_document);
    free(staged_head_page);
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
        "const streamNames=Reflect.ownKeys(ReadableStream.prototype).map(String).sort().join(','),streamOk=typeof ReadableStream.prototype.pipeTo==='function'&&streamNames==='Symbol(Symbol.asyncIterator),Symbol(Symbol.toStringTag),cancel,constructor,getReader,locked,pipeThrough,pipeTo,tee,values'&&Object.prototype.toString.call(new ReadableStream({start(c){c.close()}}))==='[object ReadableStream]'&&Function.prototype.toString.call(ReadableStream).includes('[native code]')&&Function.prototype.toString.call(ReadableStream.prototype.getReader).includes('[native code]');"
        "const lockStream=new ReadableStream({start(controller){controller.close();}}),lockReader=lockStream.getReader();let secondReaderError='';try{lockStream.getReader();}catch(error){secondReaderError=error.name;}lockReader.releaseLock();const cancelResponse=new Response('cancel');cancelResponse.body.cancel();const streamStateOk=secondReaderError==='TypeError'&&!lockStream.locked&&cancelResponse.bodyUsed;"
        "localStorage.setItem('theme','dark');"
        "localStorage.alpha='one';localStorage.setItem('beta','two');"
        "Promise.all([navigator.storage.estimate(),navigator.storage.persisted(),navigator.storage.persist()]).then(values=>{const estimate=values[0],ok=Number.isFinite(estimate.usage)&&estimate.usage>0&&Number.isFinite(estimate.quota)&&estimate.quota>=estimate.usage&&values[1]===false&&values[2]===false;document.body.setAttribute('data-storage-manager',ok?'site':'bad');},error=>{document.body.setAttribute('data-storage-manager',error&&error.name==='SecurityError'?'opaque':'bad');});"
        "navigator.keyboard.getLayoutMap().then(map=>{const ok=map instanceof KeyboardLayoutMap&&Object.prototype.toString.call(map)==='[object KeyboardLayoutMap]'&&map.size===0&&map.get('KeyA')===undefined&&!map.has('KeyA')&&[...map].length===0&&navigator.keyboard===navigator.keyboard&&Object.prototype.toString.call(navigator.keyboard)==='[object Keyboard]';document.body.setAttribute('data-keyboard',ok?'empty':'bad');});"
        "Promise.all([navigator.getBattery(),navigator.permissions.query({name:'geolocation'}),isSecureContext?navigator.gpu.requestAdapter():Promise.resolve(null)]).then(values=>{const battery=values[0],permission=values[1],ok=battery instanceof BatteryManager&&battery instanceof EventTarget&&battery.charging===true&&battery.chargingTime===0&&battery.dischargingTime===Infinity&&battery.level===1&&permission instanceof PermissionStatus&&permission instanceof EventTarget&&permission.name==='geolocation'&&permission.state==='denied'&&values[2]===null&&navigator.connection instanceof NetworkInformation&&navigator.connection instanceof EventTarget&&navigator.connection.effectiveType==='3g'&&navigator.connection.rtt===300&&navigator.connection.downlink===1.5&&navigator.connection.saveData===false;document.body.setAttribute('data-platform-status',ok?'honest':'bad');});"
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
        "performance.mark('startup');const heapMemory=performance.memory,heapMemoryProto=Object.getPrototypeOf(heapMemory),perfOk=performance.getEntriesByName('startup').length===1&&PerformanceObserver.supportedEntryTypes.includes('resource')&&Object.prototype.toString.call(heapMemory)==='[object MemoryInfo]'&&heapMemory!==performance.memory&&Object.getOwnPropertyNames(heapMemory).length===0&&Object.getOwnPropertyNames(heapMemoryProto).join(',')==='totalJSHeapSize,usedJSHeapSize,jsHeapSizeLimit'&&heapMemory.constructor===Object&&Number.isFinite(heapMemory.jsHeapSizeLimit)&&Number.isFinite(heapMemory.totalJSHeapSize)&&Number.isFinite(heapMemory.usedJSHeapSize)&&heapMemory.jsHeapSizeLimit>=heapMemory.totalJSHeapSize&&heapMemory.totalJSHeapSize>=heapMemory.usedJSHeapSize&&heapMemory.usedJSHeapSize>0;"
        "const svg=document.createElementNS('http://www.w3.org/2000/svg','svg');"
        "const blob=new Blob(['PSP'],{type:'text/plain'});const blobURL=URL.createObjectURL(blob);const workerBlob=new Blob(['self.ready=true;'],{type:'text/javascript'});const workerURL=URL.createObjectURL(workerBlob);const worker=new Worker(workerURL);const builtinSource=Function.prototype.toString;const blobOk=blob.size===3&&blob.type==='text/plain'&&Object.getOwnPropertyNames(blob).length===0&&blob._bytes===undefined&&blobURL.startsWith('blob:https://')&&worker instanceof Worker&&worker instanceof EventTarget&&Object.prototype.toString.call(worker)==='[object Worker]'&&Object.getPrototypeOf(Worker.prototype)===EventTarget.prototype&&Object.getOwnPropertyNames(worker).length===0&&Blob.name==='Blob'&&URL.createObjectURL.name==='createObjectURL'&&URL.revokeObjectURL.name==='revokeObjectURL'&&builtinSource.call(Blob).includes('[native code]')&&builtinSource.call(Blob.prototype.slice).includes('[native code]')&&builtinSource.call(MutationObserver).includes('[native code]')&&builtinSource.call(PerformanceObserver.prototype.observe).includes('[native code]')&&builtinSource.call(Worker).includes('[native code]')&&builtinSource.call(Worker.prototype.postMessage).includes('[native code]');worker.terminate();URL.revokeObjectURL(workerURL);URL.revokeObjectURL(blobURL);"
        "const interfaceHost=document.createElement('div');document.body.appendChild(interfaceHost);const shadow=interfaceHost.attachShadow({mode:'closed'});const frame=document.createElement('iframe');frame.id='shadow-frame';frame.style.cssText='width:300px;height:65px';frame.setAttribute('sandbox','allow-same-origin');shadow.appendChild(frame);const shadowFrameRect=frame.getBoundingClientRect(),shadowHostRect=interfaceHost.getBoundingClientRect();const iterator=document.createNodeIterator(interfaceHost,NodeFilter.SHOW_ELEMENT|NodeFilter.SHOW_TEXT);const nativeSource=Function.prototype.toString;const prototypeQueriesOk=[Document.prototype,DocumentFragment.prototype,Element.prototype].every(proto=>typeof proto.querySelector==='function'&&typeof proto.querySelectorAll==='function'&&nativeSource.call(proto.querySelector).includes('[native code]')&&nativeSource.call(proto.querySelectorAll).includes('[native code]'));const frameEval=frame.contentWindow.eval;frame.contentWindow.frameOnly=40;delete globalThis.frameEvalLeak;const directFrameEval=frameEval('frameOnly+2')===42;frameEval('frameEvalLeak=7');const isolatedFrameEval=frame.contentWindow.frameEvalLeak===7&&globalThis.frameEvalLeak===undefined;frameEval('function persistentFrameFunction(){return 39} var persistentFrameVar=3');const persistentFrameDeclarations=frameEval('persistentFrameFunction()+persistentFrameVar')===42;frameEval('function outerFrameFn(){\\n  function nestedFrameFn(){return 1}\\n  var nestedFrameVar=2;\\n  return nestedFrameFn()+nestedFrameVar}\\nconst frameConfig={a:41};\\nclass FrameKind{}\\nlet frameLetValue=outerFrameFn();');const frameOwn=name=>Object.prototype.hasOwnProperty.call(frame.contentWindow,name);const nestedFrameNotLeaked=frameOwn('outerFrameFn')&&!frameOwn('nestedFrameFn')&&!frameOwn('nestedFrameVar')&&frame.contentWindow.nestedFrameFn===undefined;const lexicalFrameDeclarations=frameEval('frameConfig.a+1')===42&&frameEval('typeof FrameKind')==='function'&&frameEval('frameLetValue')===3&&frameEval('const frameOnce=5;frameOnce*2')===10&&frameEval('frameOnce')===5;frame.contentDocument.title='Child title';const frameThisAndDocument=frameEval('this===window&&document.title===\"Child title\"');const staleFrame=document.createElement('iframe'),staleWindow=staleFrame.contentWindow,eventShape=new Event('tilefinch-shape');let stalePostOk=true;try{staleWindow.postMessage({probe:true},'*');}catch(_){stalePostOk=false;}dispatchEvent(eventShape);const eventPrototypeOk=['composedPath','preventDefault','stopPropagation','stopImmediatePropagation'].every(name=>typeof Event.prototype[name]==='function'&&!Object.prototype.hasOwnProperty.call(eventShape,name));const interfaceBits=[interfaceHost instanceof HTMLDivElement,interfaceHost.nodeType===Node.ELEMENT_NODE,frame instanceof HTMLIFrameElement,shadow instanceof ShadowRoot,shadow.getElementById('shadow-frame')===frame,frame.parentNode===shadow,frame.parentElement===null,shadowFrameRect.width===300,shadowFrameRect.height>=65,frame.isConnected,frame.contentDocument instanceof Document&&frame.contentDocument!==document,Object.prototype.toString.call(frame.contentWindow)==='[object Window]',Object.prototype.toString.call(frame.contentDocument)==='[object HTMLDocument]','eval' in frame.contentWindow,typeof frameEval==='function',frameEval('21*2')===42,directFrameEval,isolatedFrameEval,persistentFrameDeclarations,nestedFrameNotLeaked,lexicalFrameDeclarations,frameThisAndDocument,stalePostOk,nativeSource.call(frameEval).includes('[native code]'),prototypeQueriesOk,eventPrototypeOk,interfaceHost.shadowRoot===null,iterator.nextNode()!==null,interfaceHost.isConnected,interfaceHost.getBoundingClientRect().width>0,document.scripts.length>0,document.styleSheets.length>0],interfaceOk=interfaceBits.every(Boolean);"
        "const positionFirst=document.createElement('i'),positionSecond=document.createElement('i'),positionDetached=document.createElement('i');interfaceHost.append(positionFirst,positionSecond);const disconnectedForward=positionFirst.compareDocumentPosition(positionDetached),disconnectedReverse=positionDetached.compareDocumentPosition(positionFirst);let invalidPositionThrew=false;try{positionFirst.compareDocumentPosition({});}catch(error){invalidPositionThrew=error instanceof TypeError;}const positionOk=Node.DOCUMENT_POSITION_DISCONNECTED===1&&Node.prototype.DOCUMENT_POSITION_CONTAINED_BY===16&&positionFirst.compareDocumentPosition(positionFirst)===0&&interfaceHost.compareDocumentPosition(positionFirst)===(Node.DOCUMENT_POSITION_FOLLOWING|Node.DOCUMENT_POSITION_CONTAINED_BY)&&positionFirst.compareDocumentPosition(interfaceHost)===(Node.DOCUMENT_POSITION_PRECEDING|Node.DOCUMENT_POSITION_CONTAINS)&&positionFirst.compareDocumentPosition(positionSecond)===Node.DOCUMENT_POSITION_FOLLOWING&&positionSecond.compareDocumentPosition(positionFirst)===Node.DOCUMENT_POSITION_PRECEDING&&(disconnectedForward&(Node.DOCUMENT_POSITION_DISCONNECTED|Node.DOCUMENT_POSITION_IMPLEMENTATION_SPECIFIC))===(Node.DOCUMENT_POSITION_DISCONNECTED|Node.DOCUMENT_POSITION_IMPLEMENTATION_SPECIFIC)&&(disconnectedForward&6)!==0&&(disconnectedReverse&6)!==0&&(disconnectedForward&6)!==(disconnectedReverse&6)&&invalidPositionThrew;"
        "interfaceHost.style.setProperty('--CaseSensitive','kept');interfaceHost.style.cssFloat='left';interfaceHost.style.placeItems='center end';interfaceHost.style.placeSelf='start end';interfaceHost.style.placeContent='end center';const cssomNameOk=interfaceHost.style.getPropertyValue('--CaseSensitive')==='kept'&&interfaceHost.style.getPropertyValue('float')==='left'&&interfaceHost.style.alignItems==='center'&&interfaceHost.style.justifyItems==='end'&&interfaceHost.style.placeItems==='center end'&&interfaceHost.style.alignSelf==='start'&&interfaceHost.style.justifySelf==='end'&&interfaceHost.style.placeSelf==='start end'&&interfaceHost.style.alignContent==='end'&&interfaceHost.style.justifyContent==='center'&&interfaceHost.style.placeContent==='end center';"
        "const mutationShapeObserver=new MutationObserver(()=>{}),mutationShapeOk=Object.getOwnPropertyNames(mutationShapeObserver).length===0&&Object.prototype.toString.call(mutationShapeObserver)==='[object MutationObserver]'&&Object.prototype.toString.call(new PerformanceObserver(()=>{}))==='[object PerformanceObserver]';const xhrProbe=new XMLHttpRequest,xhrPrototypeNames=Object.getOwnPropertyNames(XMLHttpRequest.prototype),xhrShapeOk=XMLHttpRequest.name==='XMLHttpRequest'&&Object.prototype.toString.call(xhrProbe)==='[object XMLHttpRequest]'&&xhrProbe instanceof EventTarget&&Object.getPrototypeOf(XMLHttpRequest.prototype)===XMLHttpRequestEventTarget.prototype&&xhrProbe.upload instanceof XMLHttpRequestUpload&&xhrProbe.upload instanceof EventTarget&&['readyState','response','responseText','responseType','responseURL','responseXML','status','statusText','timeout','upload','withCredentials','onreadystatechange'].every(name=>xhrPrototypeNames.includes(name))&&!xhrPrototypeNames.some(name=>name[0]==='_'||name==='emit')&&Function.prototype.toString.call(XMLHttpRequest).includes('[native code]')&&Function.prototype.toString.call(XMLHttpRequest.prototype.open).includes('[native code]')&&Function.prototype.toString.call(XMLHttpRequestEventTarget.prototype.addEventListener).includes('[native code]');"
        "let navigatorConstructed=false,uaDataConstructed=false,gpuConstructed=false,wgslConstructed=false,pluginsConstructed=false,mimeTypesConstructed=false,clipboardConstructed=false,storageManagerConstructed=false,keyboardConstructed=false,keyboardMapConstructed=false;try{new Navigator}catch(_){navigatorConstructed=true}try{new NavigatorUAData}catch(_){uaDataConstructed=true}try{new GPU}catch(_){gpuConstructed=true}try{new WGSLLanguageFeatures}catch(_){wgslConstructed=true}try{new PluginArray}catch(_){pluginsConstructed=true}try{new MimeTypeArray}catch(_){mimeTypesConstructed=true}try{new Clipboard}catch(_){clipboardConstructed=true}try{new StorageManager}catch(_){storageManagerConstructed=true}try{new Keyboard}catch(_){keyboardConstructed=true}try{new KeyboardLayoutMap}catch(_){keyboardMapConstructed=true}const navigatorIdentityOwn=Object.getOwnPropertyNames(navigator).some(name=>['userAgent','appCodeName','appName','appVersion','language','languages','platform','product','productSub','vendor','vendorSub','maxTouchPoints','cookieEnabled','onLine','hardwareConcurrency','deviceMemory','userAgentData','gpu','plugins','mimeTypes','pdfViewerEnabled','webdriver','javaEnabled','getGamepads','clipboard','storage','keyboard','connection','getBattery','permissions','sendBeacon'].includes(name)),navigatorShapeOk=navigator instanceof Navigator&&Object.prototype.toString.call(navigator)==='[object Navigator]'&&!navigatorIdentityOwn&&Object.getPrototypeOf(navigator)===Navigator.prototype&&clientInformation===navigator&&navigator.appCodeName==='Mozilla'&&navigator.appName==='Netscape'&&navigator.product==='Gecko'&&navigator.vendor===''&&navigator.webdriver===false&&navigator.pdfViewerEnabled===false&&navigator.javaEnabled()===false&&navigator.plugins instanceof PluginArray&&navigator.mimeTypes instanceof MimeTypeArray&&navigator.plugins.length===0&&navigator.mimeTypes.length===0&&navigator.plugins.item(0)===null&&navigator.mimeTypes.namedItem('application/pdf')===null&&Object.prototype.toString.call(navigator.plugins)==='[object PluginArray]'&&Object.prototype.toString.call(navigator.mimeTypes)==='[object MimeTypeArray]'&&navigator.clipboard instanceof Clipboard&&Object.prototype.toString.call(navigator.clipboard)==='[object Clipboard]'&&navigator.storage instanceof StorageManager&&navigator.storage===navigator.storage&&Object.prototype.toString.call(navigator.storage)==='[object StorageManager]'&&navigator.keyboard instanceof Keyboard&&navigator.keyboard===navigator.keyboard&&Object.prototype.toString.call(navigator.keyboard)==='[object Keyboard]'&&navigator.userAgentData instanceof NavigatorUAData&&Object.prototype.toString.call(navigator.userAgentData)==='[object NavigatorUAData]'&&navigatorConstructed&&uaDataConstructed&&pluginsConstructed&&mimeTypesConstructed&&clipboardConstructed&&storageManagerConstructed&&keyboardConstructed&&keyboardMapConstructed;const platformStatusShapeOk=navigator.connection===navigator.connection&&navigator.connection instanceof NetworkInformation&&Object.prototype.toString.call(navigator.connection)==='[object NetworkInformation]'&&navigator.permissions===navigator.permissions&&navigator.permissions instanceof Permissions&&Object.prototype.toString.call(navigator.permissions)==='[object Permissions]'&&typeof navigator.getBattery==='function';const gpuEntryOk=!isSecureContext?navigator.gpu===undefined:navigator.gpu===navigator.gpu&&navigator.gpu instanceof GPU&&Object.prototype.toString.call(navigator.gpu)==='[object GPU]'&&Object.getOwnPropertyNames(navigator.gpu).length===0&&navigator.gpu.wgslLanguageFeatures instanceof WGSLLanguageFeatures&&Object.prototype.toString.call(navigator.gpu.wgslLanguageFeatures)==='[object WGSLLanguageFeatures]'&&navigator.gpu.wgslLanguageFeatures.size===0&&[...navigator.gpu.wgslLanguageFeatures].length===0&&navigator.gpu.getPreferredCanvasFormat()==='bgra8unorm'&&gpuConstructed&&wgslConstructed;const capabilitiesOk=navigatorShapeOk&&platformStatusShapeOk&&gpuEntryOk&&typeof RTCPeerConnection==='undefined'&&typeof RTCDataChannel==='undefined';"
        "const encoder=new TextEncoder,encodedReplacement=encoder.encode(String.fromCharCode(0xd800)),intoTarget=new Uint8Array(4),intoResult=encoder.encodeInto('A€B',intoTarget);const streamingDecoder=new TextDecoder,streamFirst=streamingDecoder.decode(new Uint8Array([0xe2,0x82]),{stream:true}),streamSecond=streamingDecoder.decode(new Uint8Array([0xac]));let fatalEncoding='';try{new TextDecoder('utf-8',{fatal:true}).decode(new Uint8Array([0xc0]));}catch(error){fatalEncoding=error.name;}const encodingOk=encodedReplacement.join(',')==='239,191,189'&&intoResult.read===2&&intoResult.written===4&&intoTarget.join(',')==='65,226,130,172'&&streamFirst===''&&streamSecond==='€'&&new TextDecoder().decode(new Uint8Array([0xef,0xbb,0xbf,65]))==='A'&&new TextDecoder('utf8',{ignoreBOM:true}).decode(new Uint8Array([0xef,0xbb,0xbf,65]))==='﻿A'&&new TextDecoder().decode(new Uint8Array([0xc0,0xaf])).length===2&&fatalEncoding==='TypeError';"
        "let invalidBase64Name='';try{atob('!');}catch(error){invalidBase64Name=error.name;}const base64Ok=atob(btoa('PSP\\0'))==='PSP\\0'&&atob('UFM')==='PS'&&atob('UFM=')==='PS'&&invalidBase64Name==='InvalidCharacterError';"
        "const abortController=new AbortController,abortSignal=abortController.signal;let abortSeen=false;abortSignal.addEventListener('abort',()=>abortSeen=true,{once:true});abortController.abort();const abortOk=abortSignal.aborted&&abortSeen&&abortSignal.reason instanceof DOMException&&abortSignal.reason.name==='AbortError';"
        "const template=document.createElement('template');template.innerHTML='<template for=target><b>update</b></template><template data-assistant-presentation-flush></template>';const templateClone=template.cloneNode(true),clonedTarget=templateClone.content.querySelector('template[for=target]'),flushTemplate=template.content.querySelector('template[data-assistant-presentation-flush]'),fragmentProbe=document.createDocumentFragment(),fragmentText=document.createTextNode('fragment');fragmentProbe.appendChild(fragmentText);const fragmentHost=document.createElement('div');fragmentHost.appendChild(fragmentProbe);const equalProbe=document.createElement('p');equalProbe.textContent='equal';const templateOk=template.children.length===0&&template.content.querySelector('template[for=target]')!==null&&template.content.querySelectorAll('template[for],template[data-assistant-presentation-flush]').length===2&&flushTemplate.matches('template[data-assistant-presentation-flush]')&&clonedTarget.content.querySelector('b').textContent==='update'&&equalProbe.isEqualNode(equalProbe.cloneNode(true))&&fragmentProbe.childNodes.length===0&&fragmentHost.textContent==='fragment';"
        "let detachedCanvasScans=0;const savedQuerySelectorAll=Element.prototype.querySelectorAll;Element.prototype.querySelectorAll=function(...args){detachedCanvasScans++;return savedQuerySelectorAll.apply(this,args);};const parsedDocument=new DOMParser().parseFromString('<p id=parsed>One &amp; <b>two</b></p>','text/html'),xmlDocument=new DOMParser().parseFromString('<x/>','application/xml');Element.prototype.querySelectorAll=savedQuerySelectorAll;const parsedElement=parsedDocument.getElementById('parsed'),domParserBits=[detachedCanvasScans===0,parsedDocument instanceof Document,parsedDocument.nodeType===Node.DOCUMENT_NODE,parsedDocument.defaultView===null,parsedDocument.contentType==='text/html',parsedDocument.documentElement.textContent==='One & two',parsedElement?.querySelector('b').textContent==='two',xmlDocument instanceof Document&&xmlDocument.documentElement?.localName==='x'],domParserOk=domParserBits.every(Boolean);"
        "const workDocument=document.implementation.createHTMLDocument('');workDocument.documentElement.innerHTML='<style id=\"adopted-style\">#adopted-probe{color:rgb(1,2,3)}</style>';const adoptedStyle=workDocument.getElementById('adopted-style');document.head.appendChild(adoptedStyle);const createdDocumentOk=adoptedStyle instanceof HTMLStyleElement&&adoptedStyle.ownerDocument===document&&adoptedStyle.parentElement===document.head&&adoptedStyle.textContent.includes('#adopted-probe');"
        "const moveA=document.createElement('div'),moveB=document.createElement('div'),moveOne=document.createElement('i'),moveTwo=document.createElement('i'),moveThree=document.createElement('i');moveOne.id='move-one';moveTwo.id='move-two';moveThree.id='move-three';moveA.appendChild(moveOne);moveA.appendChild(moveTwo);moveB.appendChild(moveOne);const crossAppendOk=moveA.firstChild===moveTwo&&moveB.firstChild===moveOne;moveA.insertBefore(moveTwo,moveTwo);moveA.appendChild(moveThree);moveA.appendChild(moveTwo);const reorderOk=moveA.firstChild===moveThree&&moveA.lastChild===moveTwo;const orphanReference=document.createElement('i');let invalidInsertThrew=false;try{moveA.insertBefore(moveOne,orphanReference);}catch(_){invalidInsertThrew=true;}const invalidInsertPreserved=moveOne.parentElement===moveB&&moveA.firstChild===moveThree;moveA.insertBefore(moveOne,moveTwo);const crossInsertOk=moveB.childNodes.length===0&&moveA.childNodes.length===3&&moveA.childNodes[0]===moveThree&&moveA.childNodes[1]===moveOne&&moveA.childNodes[2]===moveTwo;const preinsertOk=crossAppendOk&&reorderOk&&invalidInsertThrew&&invalidInsertPreserved&&crossInsertOk;"
        "const platformOk=streamOk&&streamStateOk&&storageOk&&localStorage.length===1&&capabilitiesOk&&mutationShapeOk&&xhrShapeOk&&cssomNameOk&&encodingOk&&base64Ok&&abortOk&&templateOk&&domParserOk&&createdDocumentOk&&preinsertOk&&positionOk&&randomBytes.some(value=>value!==0)&&crypto.randomUUID().length===36&&svg.namespaceURI.includes('svg')&&perfOk&&blobOk&&interfaceOk&&top===window&&parent===window;"
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
    /* The combined standards fixture intentionally exercises the entire
       bootstrap in one realm.  This small test-only reserve keeps additions
       from turning unrelated API assertions into a hard heap-boundary test. */
    bool mutation_ok = scripts_run_document(&mutated, &budget,
                                            4 * MIB + 192 * 1024,
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
    size_t storage_manager_state_length = 0;
    const char *storage_manager_state = document_attribute(
        document_body_node(&mutated), "data-storage-manager",
        &storage_manager_state_length);
    CHECK(storage_manager_state != NULL
          && storage_manager_state_length == 6
          && memcmp(storage_manager_state, "opaque", 6) == 0);
    size_t keyboard_state_length = 0;
    const char *keyboard_state = document_attribute(
        document_body_node(&mutated), "data-keyboard",
        &keyboard_state_length);
    CHECK(keyboard_state != NULL && keyboard_state_length == 5
          && memcmp(keyboard_state, "empty", 5) == 0);
    size_t platform_status_length = 0;
    const char *platform_status = document_attribute(
        document_body_node(&mutated), "data-platform-status",
        &platform_status_length);
    CHECK(platform_status != NULL && platform_status_length == 6
          && memcmp(platform_status, "honest", 6) == 0);
    CHECK(mutation_scripts.success && mutation_scripts.relayout_required
          && mutation_scripts.dom_mutations >= 4
          && mutation_scripts.events_dispatched >= 2
          && mutation_scripts.performance_now_calls >= 1
          /* Canvas and Worker are both admitted on first use. */
          && mutation_scripts.bootstrap_lazy_module_loads == 2
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

    puts("test: Worker bootstrap keeps compiler private and binds one policy URL");
    static const char worker_boundary_page[] =
        "<!doctype html><body></body>";
    PocDocument worker_boundary_document;
    CHECK(document_parse(
        &worker_boundary_document, &budget, worker_boundary_page,
        strlen(worker_boundary_page), 8));
    static const char worker_boundary_csp[] =
        "content-security-policy: worker-src https://worker-boundary.test\n";
    CHECK(tilefinch_csp_parse_response_headers(
        &worker_boundary_document.content_security_policy,
        "https://worker-boundary.test/", worker_boundary_csp,
        sizeof(worker_boundary_csp) - 1u, false));
    ScriptResult worker_boundary_result = {0};
    ScriptRuntime *worker_boundary_runtime = script_runtime_create_deferred(
        &worker_boundary_document, &budget, 4 * MIB, 1000,
        "https://worker-boundary.test/", NULL, &worker_boundary_result);
    bool worker_boundary_evaluated = worker_boundary_runtime != NULL
          && script_runtime_evaluate_diagnostic(
              worker_boundary_runtime,
              "(()=>{const NativeWeakMap=WeakMap,NativeString=String,"
              "nativeMapGet=Map.prototype.get,nativeWeakMapGet="
              "WeakMap.prototype.get,NativeDecoder=TextDecoder,"
              "nativeDecode=TextDecoder.prototype.decode,nativeFromCodePoint="
              "String.fromCodePoint;"
              "let poisonCalls=0,sensitiveCalls=0,mapCalls=0,weakCalls=0,"
              "decoderCalls=0,fromCodePointCalls=0,leaked,coercions=0;"
              "globalThis.WeakMap=function(){poisonCalls++;leaked="
              "globalThis.__tilefinchRunWorker;return new NativeWeakMap()};"
              "const blob=new Blob([new Uint8Array([255])]);"
              "const blobURL=URL.createObjectURL(blob),copy="
              "__tilefinchBlobBytes(blob),first=copy[0];copy[0]=0;"
              "const copySafe=__tilefinchBlobBytes(blob)[0]===first;"
              "const changingURL={toString(){coercions++;return blobURL}};"
              "const poisonObject={toString(){return "
              "'https://worker-boundary.test/allowed.js'},"
              "startsWith(){return true},slice(){return ''}};"
              "globalThis.String=value=>{"
              "if(value===changingURL){sensitiveCalls++;return poisonObject}"
              "if(value===poisonObject){sensitiveCalls++;return blobURL}"
              "return NativeString(value)};"
              "Map.prototype.get=function(){mapCalls++;return null};"
              "WeakMap.prototype.get=function(){weakCalls++;return null};"
              "TextDecoder.prototype.decode=function(){decoderCalls++;"
              "return \"postMessage('POLICY-BYPASS')\"};"
              "globalThis.TextDecoder=class{decode(){decoderCalls++;return "
              "\"postMessage('POLICY-BYPASS')\"}};"
              "String.fromCodePoint=function(){fromCodePointCalls++;return "
              "\"postMessage('POLICY-BYPASS')\"};"
              "const worker=new Worker(changingURL);globalThis.WeakMap="
              "NativeWeakMap;globalThis.String=NativeString;"
              "Map.prototype.get=nativeMapGet;WeakMap.prototype.get="
              "nativeWeakMapGet;globalThis.TextDecoder=NativeDecoder;"
              "TextDecoder.prototype.decode=nativeDecode;"
              "String.fromCodePoint=nativeFromCodePoint;"
              "const done=kind=>{pocSummary=kind+':' +coercions+':'"
              "+poisonCalls+':'+sensitiveCalls+':'+mapCalls+':'"
              "+weakCalls+':'+decoderCalls+':'+fromCodePointCalls+':'"
              "+copySafe+':'+typeof leaked;"
              "worker.terminate();URL.revokeObjectURL(blobURL)};"
              "worker.onmessage=event=>done(event.data);"
              "worker.onerror=()=>done('POLICY-BLOCKED')})()",
              "<worker-boundary>", &worker_boundary_result);
    if (!worker_boundary_evaluated) {
        fprintf(stderr, "worker boundary error=\"%s\" context=\"%s\"\n",
                worker_boundary_result.error,
                worker_boundary_result.error_source_context);
    }
    CHECK(worker_boundary_evaluated);
    for (size_t worker_turn = 0; worker_turn < 12; worker_turn++) {
        CHECK(script_runtime_advance(
            worker_boundary_runtime, worker_turn, 8,
            &worker_boundary_result));
    }
    if (strcmp(worker_boundary_result.summary,
               "POLICY-BLOCKED:1:0:0:0:0:0:0:true:undefined") != 0) {
        fprintf(stderr, "worker boundary summary=\"%s\" error=\"%s\"\n",
                worker_boundary_result.summary, worker_boundary_result.error);
    }
    CHECK(strcmp(worker_boundary_result.summary,
                 "POLICY-BLOCKED:1:0:0:0:0:0:0:true:undefined") == 0
          && worker_boundary_result.bootstrap_lazy_module_loads == 1
          && worker_boundary_result.bootstrap_lazy_module_failures == 0);
    script_runtime_destroy(worker_boundary_runtime);
    document_destroy(&worker_boundary_document);
    CHECK(budget.current == 0);

    puts("test: bounded Worker startup is asynchronous and hides page globals");
    static const char worker_lifecycle_page[] =
        "<!doctype html><body></body>";
    PocDocument worker_lifecycle_document;
    CHECK(document_parse(
        &worker_lifecycle_document, &budget, worker_lifecycle_page,
        strlen(worker_lifecycle_page), 8));
    ScriptResult worker_lifecycle_result = {0};
    ScriptRuntime *worker_lifecycle_runtime = script_runtime_create_deferred(
        &worker_lifecycle_document, &budget, 4 * MIB, 1000,
        "https://worker-lifecycle.test/", NULL, &worker_lifecycle_result);
    bool worker_lifecycle_evaluated = worker_lifecycle_runtime != NULL
          && script_runtime_evaluate_diagnostic(
              worker_lifecycle_runtime,
              "(()=>{let reports=[];const finish=value=>{reports.push(value);"
              "if(reports.length===3)pocSummary=reports.sort().join(':')};"
              "const terminatedURL=URL.createObjectURL(new Blob(["
              "\"postMessage('terminated-ran')\"]));const terminated="
              "new Worker(terminatedURL);terminated.onmessage=event=>"
              "finish(event.data);terminated.terminate();"
              "URL.revokeObjectURL(terminatedURL);"
              "const behaviorURL=URL.createObjectURL(new Blob(["
              "\"postMessage([typeof document,typeof window,'document' in self,'window' in self,typeof __tilefinchRunWorker,typeof navigator,typeof location,typeof navigator.userAgent,location.href.startsWith('blob:'),Object.prototype.toString.call(navigator),Object.prototype.toString.call(location),Object.prototype.toString.call(self),self instanceof DedicatedWorkerGlobalScope,self instanceof WorkerGlobalScope,navigator instanceof WorkerNavigator,location instanceof WorkerLocation,navigator.constructor.name,location.constructor.name,this===self,this===globalThis,typeof speechSynthesis,typeof SpeechSynthesisUtterance])\"]));"
              "const messageURL=URL.createObjectURL(new Blob(["
              "\"onmessage=e=>postMessage([e instanceof MessageEvent,e.isTrusted,e.origin,e.source===null,e.ports.length,e.data])\"]));"
              "const behavior=new Worker(behaviorURL);behavior.onmessage="
              "event=>{finish(event.data.join(','));behavior.terminate();"
              "const messaging=new Worker(messageURL);messaging.onmessage="
              "message=>{finish(['message-shape',message instanceof MessageEvent,"
              "message.isTrusted,message.currentTarget===messaging,"
              "message.target===messaging,message.eventPhase===Event.AT_TARGET,"
              "message.origin,message.source===null,message.ports.length,"
              "message.data.join(',')].join(','));messaging.terminate()};"
              "messaging.postMessage('payload');URL.revokeObjectURL(messageURL)};"
              "URL.revokeObjectURL(behaviorURL);const errorURL="
              "URL.createObjectURL(new Blob([\"throw new Error('boom')\"]));"
              "let synchronous=false;try{const failing=new Worker(errorURL);"
              "failing.onerror=event=>{const shape=event instanceof ErrorEvent"
              "&&event.currentTarget===failing&&event.target===failing"
              "&&event.eventPhase===Event.AT_TARGET&&event.cancelable"
              "&&event.error===null&&event.message==='boom';"
              "event.preventDefault();finish('async-error,'+shape);"
              "failing.terminate()}}"
              "catch(_){synchronous=true}URL.revokeObjectURL(errorURL);"
              "const lookup=Object.getOwnPropertyDescriptor(globalThis,"
              "'__tilefinchBlobForURL'),clone=Object.getOwnPropertyDescriptor("
              "globalThis,'__tilefinchCloneWorkerValue'),constructed="
              "new MessageEvent('message',{data:7,origin:'https://origin.test'}),"
              "boundedPorts=new MessageEvent('message',{ports:Array(20)}).ports;"
              "let messageShape=Object.prototype.toString.call(constructed)==="
              "'[object MessageEvent]'&&Object.getOwnPropertyNames(constructed)"
              ".join(',')==='isTrusted'&&Object.getOwnPropertyNames("
              "MessageEvent.prototype).join(',')==='data,origin,lastEventId,source,ports,userActivation,initMessageEvent,constructor'&&constructed.data===7"
              "&&constructed.origin==='https://origin.test'&&"
              "constructed.source===null&&constructed.ports.length===0;"
              "messageShape=messageShape&&Object.isFrozen(constructed.ports)"
              "&&boundedPorts.length===16&&Object.isFrozen(boundedPorts);"
              "pocSummary=synchronous?'sync-error':typeof "
              "__tilefinchRunWorker==='undefined'&&lookup&&!lookup.writable&&"
              "!lookup.configurable&&clone&&!clone.writable&&!clone.configurable&&"
              "Object.getOwnPropertyNames(behavior).length===0&&messageShape&&"
              "globalThis.isSecureContext===true"
              "?'pending':'bridge-leaked'})()",
              "<worker-lifecycle>", &worker_lifecycle_result);
    if (!worker_lifecycle_evaluated) {
        fprintf(stderr, "worker lifecycle evaluation error=\"%s\"\n",
                worker_lifecycle_result.error);
    }
    CHECK(worker_lifecycle_evaluated);
    for (size_t worker_turn = 0; worker_turn < 4; worker_turn++) {
        CHECK(script_runtime_advance(
            worker_lifecycle_runtime, worker_turn, 8,
            &worker_lifecycle_result));
    }
    const char *expected_worker_lifecycle =
        "async-error,true:message-shape,true,true,true,true,true,,true,0,"
        "true,true,,true,0,payload:"
        "undefined,undefined,false,false,undefined,object,object,string,true,"
        "[object WorkerNavigator],[object WorkerLocation],"
        "[object DedicatedWorkerGlobalScope],true,true,true,true,"
        "WorkerNavigator,WorkerLocation,true,true,undefined,undefined";
    bool worker_lifecycle_valid = strcmp(
            worker_lifecycle_result.summary, expected_worker_lifecycle) == 0
        && worker_lifecycle_result.bootstrap_lazy_module_loads == 1
        && worker_lifecycle_result.bootstrap_lazy_module_failures == 0;
    if (!worker_lifecycle_valid)
        fprintf(stderr, "worker lifecycle summary=%s error=%s lazy=%zu/%zu\n",
                worker_lifecycle_result.summary,
                worker_lifecycle_result.error,
                worker_lifecycle_result.bootstrap_lazy_module_loads,
                worker_lifecycle_result.bootstrap_lazy_module_failures);
    CHECK(worker_lifecycle_valid);
    script_runtime_destroy(worker_lifecycle_runtime);
    document_destroy(&worker_lifecycle_document);
    CHECK(budget.current == 0);

    puts("test: Worker preserves self and globalThis event handlers");
    PocDocument worker_handler_document;
    CHECK(document_parse(
        &worker_handler_document, &budget, worker_lifecycle_page,
        strlen(worker_lifecycle_page), 8));
    ScriptResult worker_handler_result = {0};
    ScriptRuntime *worker_handler_runtime = script_runtime_create_deferred(
        &worker_handler_document, &budget, 4 * MIB, 1000,
        "https://worker-handlers.test/", NULL, &worker_handler_result);
    CHECK(worker_handler_runtime != NULL
          && script_runtime_evaluate_diagnostic(
              worker_handler_runtime,
              "(()=>{pocSummary='pending';let replies=[];"
              "const done=value=>{replies.push(value);if(replies.length===2)"
              "pocSummary=replies.sort().join(':')};"
              "const selfURL=URL.createObjectURL(new Blob(["
              "\"var URL=self.URL;const TextDecoder=self.TextDecoder;"
              "self.onmessage=e=>self.postMessage('self-'+e.data)\"])),"
              "globalURL=URL.createObjectURL(new Blob(["
              "\"var Blob=globalThis.Blob;let Headers=globalThis.Headers;"
              "globalThis.onmessage=e=>globalThis.postMessage('global-'+e.data)\"])),"
              "a=new Worker(selfURL),b=new Worker(globalURL);"
              "a.onmessage=e=>done(e.data);b.onmessage=e=>done(e.data);"
              "a.postMessage(1);b.postMessage(2);"
              "setTimeout(()=>{a.terminate();b.terminate();"
              "URL.revokeObjectURL(selfURL);URL.revokeObjectURL(globalURL)},20)})()",
              "<worker-handler-bindings>", &worker_handler_result));
    for (size_t worker_turn = 0; worker_turn < 8; worker_turn++) {
        CHECK(script_runtime_advance(
            worker_handler_runtime, worker_turn * 10u, 8,
            &worker_handler_result));
    }
    if (strcmp(worker_handler_result.summary, "global-2:self-1") != 0)
        fprintf(stderr, "worker handler summary=%s error=%s\n",
                worker_handler_result.summary, worker_handler_result.error);
    CHECK(strcmp(worker_handler_result.summary, "global-2:self-1") == 0);
    script_runtime_destroy(worker_handler_runtime);
    document_destroy(&worker_handler_document);
    CHECK(budget.current == 0);

    puts("test: Worker exposes one worker-global identity without Window DOM");
    PocDocument worker_global_document;
    CHECK(document_parse(
        &worker_global_document, &budget, worker_lifecycle_page,
        strlen(worker_lifecycle_page), 8));
    ScriptResult worker_global_result = {0};
    ScriptRuntime *worker_global_runtime = script_runtime_create_deferred(
        &worker_global_document, &budget, 4 * MIB, 1000,
        "https://worker-global.test/", NULL, &worker_global_result);
    CHECK(worker_global_runtime != NULL
          && script_runtime_evaluate_diagnostic(
              worker_global_runtime,
              "(()=>{pocSummary='pending';const source=\"postMessage(["
              "self===globalThis,self.self===self,self.globalThis===self,"
              "typeof Window,typeof document,typeof self.Window,"
              "typeof self.document]"
              ".join(':'))\","
              "url=URL.createObjectURL(new Blob([source])),worker=new Worker(url);"
              "worker.onmessage=event=>{pocSummary=event.data;worker.terminate();"
              "URL.revokeObjectURL(url)}})()",
              "<worker-global-identity>", &worker_global_result));
    for (size_t worker_turn = 0; worker_turn < 4; worker_turn++) {
        CHECK(script_runtime_advance(
            worker_global_runtime, worker_turn, 8,
            &worker_global_result));
    }
    if (strcmp(worker_global_result.summary,
               "true:true:true:undefined:undefined:undefined:undefined")
        != 0) {
        fprintf(stderr, "worker global summary=%s error=%s\n",
                worker_global_result.summary,
                worker_global_result.error);
    }
    CHECK(strcmp(worker_global_result.summary,
                 "true:true:true:undefined:undefined:undefined:undefined")
          == 0);
    script_runtime_destroy(worker_global_runtime);
    document_destroy(&worker_global_document);
    CHECK(budget.current == 0);

    puts("test: Worker importScripts is synchronous and fails visibly");
    PocDocument worker_import_document;
    CHECK(document_parse(
        &worker_import_document, &budget, worker_lifecycle_page,
        strlen(worker_lifecycle_page), 8));
    ScriptResult worker_import_result = {0};
    ScriptRuntime *worker_import_runtime = script_runtime_create_deferred(
        &worker_import_document, &budget, 4 * MIB, 1000,
        "https://worker-import.test/", NULL, &worker_import_result);
    CHECK(worker_import_runtime != NULL
          && script_runtime_evaluate_diagnostic(
              worker_import_runtime,
              "(()=>{pocSummary='pending';const dependency="
              "URL.createObjectURL(new Blob([\"globalThis.imported='loaded'\"])),"
              "missing=URL.createObjectURL(new Blob(['0']));"
              "URL.revokeObjectURL(missing);const source='importScripts('+"
              "JSON.stringify(dependency)+');let failure=\\\"none\\\";try{'"
              "+'importScripts('+JSON.stringify(missing)+')}catch(error){'"
              "+'failure=error.name}postMessage(globalThis.imported+\\\":\\\"+failure)',"
              "url=URL.createObjectURL(new Blob([source])),worker=new Worker(url);"
              "worker.onmessage=event=>{pocSummary=event.data;worker.terminate();"
              "URL.revokeObjectURL(url);URL.revokeObjectURL(dependency)}})()",
              "<worker-import-scripts>", &worker_import_result));
    for (size_t worker_turn = 0; worker_turn < 8; worker_turn++) {
        CHECK(script_runtime_advance(
            worker_import_runtime, worker_turn, 8,
            &worker_import_result));
    }
    if (strcmp(worker_import_result.summary, "loaded:NetworkError") != 0)
        fprintf(stderr, "worker import summary=%s error=%s\n",
                worker_import_result.summary, worker_import_result.error);
    CHECK(strcmp(worker_import_result.summary, "loaded:NetworkError") == 0);
    script_runtime_destroy(worker_import_runtime);
    document_destroy(&worker_import_document);
    CHECK(budget.current == 0);

    puts("test: Worker owns a branded Performance timeline");
    PocDocument worker_performance_document;
    CHECK(document_parse(
        &worker_performance_document, &budget, worker_lifecycle_page,
        strlen(worker_lifecycle_page), 8));
    ScriptResult worker_performance_result = {0};
    ScriptRuntime *worker_performance_runtime = script_runtime_create_deferred(
        &worker_performance_document, &budget, 4 * MIB, 1000,
        "https://worker-performance.test/", NULL,
        &worker_performance_result);
    CHECK(worker_performance_runtime != NULL
          && script_runtime_evaluate_diagnostic(
              worker_performance_runtime,
              "(()=>{pocSummary='pending';const ownerOrigin=performance.timeOrigin,"
              "source=\"const ownBefore=Object.getOwnPropertyNames(performance);performance.probe=1;postMessage({tag:Object.prototype.toString.call(performance),instance:performance instanceof Performance,constructorName:Performance.name,origin:performance.timeOrigin,now:performance.now(),ownBefore,hasMemory:'memory' in performance,timing:typeof performance.timing,navigation:typeof performance.navigation,entryTypes:PerformanceObserver.supportedEntryTypes.join(','),entryNames:performance.getEntries().map(e=>e.entryType).join(','),navigatorAPIs:[typeof navigator.connection,typeof navigator.permissions,typeof navigator.storage,typeof navigator.gpu].join(',')})\","
              "url=URL.createObjectURL(new Blob([source])),worker=new Worker(url);"
              "worker.onmessage=event=>{const value=event.data;"
              "pocSummary=performance instanceof Performance"
              "&&Object.prototype.toString.call(performance)==='[object Performance]'"
              "&&performance.probe===undefined&&value.tag==='[object Performance]'"
              "&&value.instance&&value.constructorName==='Performance'"
              "&&value.ownBefore.length===0&&!value.hasMemory"
              "&&value.timing==='undefined'&&value.navigation==='undefined'"
              "&&value.entryTypes==='mark,measure,resource'"
              "&&!value.entryNames.split(',').some(type=>"
              "type==='navigation'||type==='paint')"
              "&&value.navigatorAPIs==='object,object,object,object'"
              "&&Number.isFinite(value.origin)&&value.origin>=ownerOrigin"
              "&&Number.isFinite(value.now)&&value.now>=0"
              "?'WORKER-PERFORMANCE-OK':'WORKER-PERFORMANCE-FAILED:'"
              "+JSON.stringify({ownerInstance:performance instanceof Performance,"
              "ownerTag:Object.prototype.toString.call(performance),"
              "ownerProbe:performance.probe,value,ownerOrigin});"
              "worker.terminate();URL.revokeObjectURL(url)}})()",
              "<worker-performance>", &worker_performance_result));
    for (size_t worker_turn = 0; worker_turn < 4; worker_turn++) {
        CHECK(script_runtime_advance(
            worker_performance_runtime, worker_turn, 8,
            &worker_performance_result));
    }
    if (strcmp(worker_performance_result.summary,
               "WORKER-PERFORMANCE-OK") != 0) {
        fprintf(stderr, "worker performance summary=%s error=%s\n",
                worker_performance_result.summary,
                worker_performance_result.error);
    }
    CHECK(strcmp(worker_performance_result.summary,
                 "WORKER-PERFORMANCE-OK") == 0);
    script_runtime_destroy(worker_performance_runtime);
    document_destroy(&worker_performance_document);
    CHECK(budget.current == 0);

    puts("test: Worker listener removal takes effect during dispatch");
    static const char worker_listener_removal_page[] =
        "<!doctype html><body></body>";
    PocDocument worker_listener_removal_document;
    CHECK(document_parse(
        &worker_listener_removal_document, &budget,
        worker_listener_removal_page,
        strlen(worker_listener_removal_page), 8));
    ScriptResult worker_listener_removal_result = {0};
    ScriptRuntime *worker_listener_removal_runtime =
        script_runtime_create_deferred(
            &worker_listener_removal_document, &budget, 4 * MIB, 1000,
            "https://worker-listener-removal.test/", NULL,
            &worker_listener_removal_result);
    CHECK(worker_listener_removal_runtime != NULL);
    CHECK(script_runtime_evaluate_diagnostic(
        worker_listener_removal_runtime,
        "(()=>{pocSummary='pending';const url=URL.createObjectURL(new Blob(["
        "\"postMessage('ready')\"]));const worker=new Worker(url);"
        "const stale=()=>{pocSummary='stale-listener-ran'};"
        "worker.addEventListener('message',()=>{"
        "worker.removeEventListener('message',stale);"
        "pocSummary='removed';worker.terminate();URL.revokeObjectURL(url)});"
        "worker.addEventListener('message',stale)})()",
        "<worker-listener-removal>", &worker_listener_removal_result));
    for (size_t worker_turn = 0; worker_turn < 4; worker_turn++) {
        CHECK(script_runtime_advance(
            worker_listener_removal_runtime, worker_turn, 8,
            &worker_listener_removal_result));
    }
    CHECK(strcmp(worker_listener_removal_result.summary, "removed") == 0);
    script_runtime_destroy(worker_listener_removal_runtime);
    document_destroy(&worker_listener_removal_document);
    CHECK(budget.current == 0);

    puts("test: Worker event handlers retain registration order");
    CHECK(document_parse(
        &worker_listener_removal_document, &budget,
        worker_listener_removal_page,
        strlen(worker_listener_removal_page), 8));
    memset(&worker_listener_removal_result, 0,
           sizeof(worker_listener_removal_result));
    worker_listener_removal_runtime = script_runtime_create_deferred(
        &worker_listener_removal_document, &budget, 4 * MIB, 1000,
        "https://worker-handler-order.test/", NULL,
        &worker_listener_removal_result);
    CHECK(worker_listener_removal_runtime != NULL);
    CHECK(script_runtime_evaluate_diagnostic(
        worker_listener_removal_runtime,
        "(()=>{pocSummary='pending';const order=[],url="
        "URL.createObjectURL(new Blob([\"postMessage('ready')\"])),"
        "worker=new Worker(url),finish=value=>{order.push(value);"
        "if(order.length===3){pocSummary=order.join(',');worker.terminate();"
        "URL.revokeObjectURL(url)}};"
        "worker.addEventListener('message',()=>finish('first'));"
        "worker.onmessage=()=>finish('handler');"
        "worker.addEventListener('message',()=>finish('last'))})()",
        "<worker-handler-order>", &worker_listener_removal_result));
    for (size_t worker_turn = 0; worker_turn < 4; worker_turn++) {
        CHECK(script_runtime_advance(
            worker_listener_removal_runtime, worker_turn, 8,
            &worker_listener_removal_result));
    }
    if (strcmp(worker_listener_removal_result.summary,
               "first,handler,last") != 0) {
        fprintf(stderr, "worker handler order summary=%s error=%s\n",
                worker_listener_removal_result.summary,
                worker_listener_removal_result.error);
    }
    CHECK(strcmp(worker_listener_removal_result.summary,
                 "first,handler,last") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
        worker_listener_removal_runtime,
        "(()=>{pocSummary='scope-pending';const source=\"const order=[];"
        "const finish=value=>{order.push(value);if(order.length===3)"
        "postMessage(order.join(','))};self.addEventListener('message',"
        "()=>finish('first'));self.onmessage=()=>finish('handler');"
        "self.addEventListener('message',()=>finish('last'))\",url="
        "URL.createObjectURL(new Blob([source])),worker=new Worker(url);"
        "worker.onmessage=event=>{pocSummary=event.data;worker.terminate();"
        "URL.revokeObjectURL(url)};worker.postMessage('go')})()",
        "<worker-scope-handler-order>", &worker_listener_removal_result));
    for (size_t worker_turn = 0; worker_turn < 6; worker_turn++) {
        CHECK(script_runtime_advance(
            worker_listener_removal_runtime, worker_turn + 4, 8,
            &worker_listener_removal_result));
    }
    CHECK(strcmp(worker_listener_removal_result.summary,
                 "first,handler,last") == 0);
    script_runtime_destroy(worker_listener_removal_runtime);
    document_destroy(&worker_listener_removal_document);
    CHECK(budget.current == 0);

    puts("test: secure-context status follows the native document URL");
    PocDocument insecure_context_document;
    CHECK(document_parse(
        &insecure_context_document, &budget, worker_lifecycle_page,
        strlen(worker_lifecycle_page), 8));
    ScriptResult insecure_context_result = {0};
    ScriptRuntime *insecure_context_runtime = script_runtime_create_deferred(
        &insecure_context_document, &budget, 2 * MIB, 1000,
        "http://insecure-context.test/", NULL, &insecure_context_result);
    CHECK(insecure_context_runtime != NULL
          && script_runtime_evaluate_diagnostic(
              insecure_context_runtime,
              "pocSummary=globalThis.isSecureContext===false?"
              "'INSECURE-CONTEXT-OK':'INSECURE-CONTEXT-FAILED'",
              "<insecure-context>", &insecure_context_result)
          && strcmp(insecure_context_result.summary,
                    "INSECURE-CONTEXT-OK") == 0);
    script_runtime_destroy(insecure_context_runtime);
    document_destroy(&insecure_context_document);
    CHECK(budget.current == 0);

    puts("test: PerformanceObserver delivers bounded asynchronous records");
    static const char performance_observer_page[] =
        "<!doctype html><body></body>";
    PocDocument performance_observer_document;
    CHECK(document_parse(
        &performance_observer_document, &budget, performance_observer_page,
        strlen(performance_observer_page), 8));
    ScriptResult performance_observer_result = {0};
    ScriptRuntime *performance_observer_runtime =
        script_runtime_create_deferred(
            &performance_observer_document, &budget, 4 * MIB, 1000,
            "https://performance-observer.test/", NULL,
            &performance_observer_result);
    CHECK(performance_observer_runtime != NULL
          && script_runtime_evaluate_diagnostic(
              performance_observer_runtime,
              "(()=>{__tilefinchRecordResourceTiming("
              "'https://performance-observer.test/api.js','script',2000,"
              "5000,7000,9000,12000,1234,true,false);const resource="
              "performance.getEntriesByName("
              "'https://performance-observer.test/api.js','resource')[0],"
              "resourceJSON=resource&&resource.toJSON(),resourceShape="
              "Object.prototype.toString.call(resource)==="
              "'[object PerformanceResourceTiming]'&&"
              "Object.keys(resource).length===0&&resource.initiatorType==="
              "'script'&&resource.startTime<=resource.requestStart&&"
              "resource.requestStart<=resource.responseStart&&"
              "resource.responseStart<=resource.responseEnd&&"
              "resource.duration===resource.responseEnd-resource.startTime&&"
              "resource.transferSize===1234&&resource.encodedBodySize===1234&&"
              "resource.decodedBodySize===1234&&resourceJSON.transferSize===1234;"
              "let synchronous=true;const observer=new "
              "PerformanceObserver((list,self,options)=>{const all="
              "list.getEntries(),named=list.getEntriesByName('observed',"
              "'mark'),marks=list.getEntriesByType('mark');pocSummary="
              "!synchronous&&self===observer&&list instanceof "
              "PerformanceObserverEntryList&&all.length===2&&named.length===1"
              "&&marks.length===1&&options.droppedEntriesCount===0&&"
              "observer.takeRecords().length===0?'PERFORMANCE-OBSERVER-OK':"
              "'PERFORMANCE-OBSERVER-FAILED';observer.disconnect()});"
              "const hidden=Object.getOwnPropertyNames(observer).length===0;"
              "observer.observe({entryTypes:['mark','measure','unsupported']});"
              "performance.mark('observed',{startTime:3});"
              "performance.measure('span','observed');synchronous=false;"
              "pocSummary=hidden&&resourceShape?'pending':['state-leaked',hidden,"
              "Object.prototype.toString.call(resource),Object.keys(resource).length,"
              "resource&&resource.initiatorType,resource&&resource.startTime,"
              "resource&&resource.requestStart,resource&&resource.responseStart,"
              "resource&&resource.responseEnd,resource&&resource.duration,"
              "resource&&resource.transferSize,resource&&resource.encodedBodySize,"
              "resource&&resource.decodedBodySize,resourceJSON&&"
              "resourceJSON.transferSize].join(':')})()",
              "<performance-observer>", &performance_observer_result));
    if (strcmp(performance_observer_result.summary, "pending") != 0) {
        fprintf(stderr, "performance resource summary=%s error=%s\n",
                performance_observer_result.summary,
                performance_observer_result.error);
    }
    CHECK(strcmp(performance_observer_result.summary, "pending") == 0);
    for (size_t observer_turn = 0; observer_turn < 3; observer_turn++) {
        CHECK(script_runtime_advance(
            performance_observer_runtime, observer_turn, 8,
            &performance_observer_result));
    }
    CHECK(strcmp(performance_observer_result.summary,
                 "PERFORMANCE-OBSERVER-OK") == 0);
    script_runtime_destroy(performance_observer_runtime);
    document_destroy(&performance_observer_document);
    CHECK(budget.current == 0);

    puts("test: isolated frame messaging and trusted delivery");
    static const char parent_message_page[] =
        "<!doctype html><body><script>window.addEventListener('message',"
        "event=>globalThis.pocSummary=String(event.isTrusted)+':' +"
        "String(event instanceof MessageEvent)+':' +"
        "Object.prototype.toString.call(event)+':' +event.origin+':' +"
        "event.data.event);</script></body>";
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
              child_message_runtime, true, false, false, true,
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
                 child_message_runtime, false, true, false, true,
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
                    "true:true:[object MessageEvent]:https://child.test:hello")
                 == 0);
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
    static BrowserSession opaque_message_session;
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
              opaque_message_runtime, true, false, true, true,
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
        failed_message_runtime, true, false, false, true,
        count_test_message,
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
                 failed_message_runtime, false, true, false, true,
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
