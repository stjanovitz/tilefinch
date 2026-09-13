bool navigation_init(NavigationSession *session, Budget *budget,
                     size_t history_capacity)
{
    if (session == NULL || budget == NULL || history_capacity == 0
        || history_capacity > 64) return false;
    memset(session, 0, sizeof(*session));
    session->budget = budget;
    session->history = budget_calloc(budget, history_capacity,
                                     sizeof(*session->history));
    if (session->history == NULL) {
        memset(session, 0, sizeof(*session));
        return false;
    }
    session->history_capacity = history_capacity;
    session->stream_delivery.chunk_bytes = 16384;
    session->progressive_preview_lookahead_percent = 50;
    session->performance.low_memory_last_rollback_available = true;
    return true;
}

void navigation_set_stream_delivery(
    NavigationSession *session, size_t chunk_bytes, uint64_t irregular_seed,
    size_t irregular_max_chunk_bytes, size_t stall_every_chunks,
    size_t cancel_after_bytes, size_t truncate_after_bytes)
{
    if (session == NULL || chunk_bytes > 65536
        || irregular_max_chunk_bytes > 65536) return;
    session->stream_delivery = (FetchStreamOptions) {
        .chunk_bytes = chunk_bytes,
        .irregular_seed = irregular_seed,
        .irregular_max_chunk_bytes = irregular_max_chunk_bytes,
        .stall_every_chunks = stall_every_chunks,
        .cancel_after_bytes = cancel_after_bytes,
        .truncate_after_bytes = truncate_after_bytes
    };
}

void navigation_enable_scripts(NavigationSession *session,
                               size_t js_memory_limit,
                               unsigned timeout_ms)
{
    if (session == NULL) return;
    session->scripts_enabled = js_memory_limit != 0 && timeout_ms != 0;
    session->js_memory_limit = js_memory_limit;
    session->js_timeout_ms = timeout_ms;
    if (!session->scripts_enabled && session->page.runtime != NULL) {
        /* Disabling the global setting is immediate, even if a replacement
           navigation later fails. Keep the already-rendered DOM and retire
           only author realms and their pending callbacks. */
        navigation_retire_current_page_scripts(session);
        session->page.script_result.success = true;
        snprintf(session->page.script_result.summary,
                 sizeof(session->page.script_result.summary),
                 "JavaScript disabled by browser setting");
    }
}

bool navigation_set_script_execution_policy(
    NavigationSession *session, const ScriptExecutionPolicy *policy)
{
    if (session == NULL || policy == NULL
        || session->page.runtime != NULL) return false;
    session->script_execution_policy = *policy;
    return true;
}

void navigation_attach_browser_session(NavigationSession *session,
                                       BrowserSession *browser_session)
{
    if (session != NULL) session->browser_session = browser_session;
}

bool navigation_set_candidate_commit_hooks(
    NavigationSession *session, NavigationCandidatePrepareCallback prepare,
    NavigationCandidateFinalizeCallback abort,
    NavigationCandidateFinalizeCallback commit, void *opaque)
{
    if (session == NULL
        || ((prepare == NULL || abort == NULL || commit == NULL)
            && (prepare != NULL || abort != NULL || commit != NULL))) {
        return false;
    }
    session->candidate_prepare = prepare;
    session->candidate_abort = abort;
    session->candidate_commit = commit;
    session->candidate_commit_opaque = opaque;
    return true;
}

bool navigation_set_progressive_paint_hook(
    NavigationSession *session, NavigationProgressivePaintCallback paint,
    void *opaque)
{
    if (session == NULL) return false;
    session->progressive_paint = paint;
    session->progressive_paint_opaque = paint == NULL ? NULL : opaque;
    return true;
}

void navigation_set_deferred_image_ready_hook(
    NavigationSession *session,
    NavigationDeferredImageReadyCallback callback, void *opaque)
{
    if (session == NULL) return;
    session->deferred_image_ready = callback;
    session->deferred_image_ready_opaque = callback == NULL ? NULL : opaque;
}

bool navigation_set_progressive_preview_lookahead(
    NavigationSession *session, unsigned percent)
{
    if (session == NULL || percent > 200) return false;
    session->progressive_preview_lookahead_percent = percent;
    return true;
}

void navigation_set_progressive_paint_preserves_incumbent(
    NavigationSession *session, bool preserves)
{
    if (session != NULL)
        session->progressive_paint_preserves_incumbent = preserves;
}

bool navigation_set_replacement_mode(
    NavigationSession *session, NavigationReplacementMode mode)
{
    if (session == NULL
        || (mode != NAVIGATION_REPLACEMENT_TRANSACTIONAL
            && mode != NAVIGATION_REPLACEMENT_LOW_MEMORY)) return false;
    session->replacement_mode = mode;
    return true;
}

bool navigation_set_replacement_hooks(
    NavigationSession *session, NavigationReplacementFreezeCallback freeze,
    NavigationReplacementOutcomeCallback outcome, void *opaque)
{
    if (session == NULL || ((freeze == NULL) != (outcome == NULL))) {
        return false;
    }
    session->replacement_freeze = freeze;
    session->replacement_outcome = outcome;
    session->replacement_hook_opaque = opaque;
    return true;
}

static ScriptRemoteDocumentBinding navigation_remote_document_binding(
    const NavigationSession *session)
{
    if (session == NULL) return (ScriptRemoteDocumentBinding) {0};
    return (ScriptRemoteDocumentBinding) {
        .element_lookup = session->remote_element_lookup,
        .element_opaque = session->remote_element_opaque,
        .selector_lookup = session->remote_selector_lookup,
        .selector_opaque = session->remote_selector_opaque,
        .selector_collect = session->remote_selector_collect,
        .selector_collect_opaque = session->remote_selector_collect_opaque,
        .descendant_collect = session->remote_descendant_collect,
        .descendant_collect_opaque =
            session->remote_descendant_collect_opaque,
        .node_read = session->remote_node_read,
        .node_read_opaque = session->remote_node_read_opaque,
        .node_write = session->remote_node_write,
        .node_write_opaque = session->remote_node_write_opaque,
        .node_visibility = session->node_visibility,
        .node_visibility_opaque = session->node_visibility_opaque,
        .section_identity = session->runtime_section_identity
    };
}

bool navigation_rebind_top_level_remote_document(
    NavigationSession *session,
    const ScriptRemoteDocumentBinding *binding)
{
    if (session == NULL) return false;
    ScriptRemoteDocumentBinding next = {0};
    if (binding != NULL) next = *binding;
    if (session->page.runtime != NULL
        && !script_runtime_rebind_remote_document(
               session->page.runtime, &next)) return false;

    session->remote_element_lookup = next.element_lookup;
    session->remote_element_opaque = next.element_opaque;
    session->remote_selector_lookup = next.selector_lookup;
    session->remote_selector_opaque = next.selector_opaque;
    session->remote_selector_collect = next.selector_collect;
    session->remote_selector_collect_opaque = next.selector_collect_opaque;
    session->remote_descendant_collect = next.descendant_collect;
    session->remote_descendant_collect_opaque = next.descendant_collect_opaque;
    session->remote_node_read = next.node_read;
    session->remote_node_read_opaque = next.node_read_opaque;
    session->remote_node_write = next.node_write;
    session->remote_node_write_opaque = next.node_write_opaque;
    session->node_visibility = next.node_visibility;
    session->node_visibility_opaque = next.node_visibility_opaque;
    session->runtime_section_identity = next.section_identity;
    return true;
}

void navigation_set_remote_selector_lookup(
    NavigationSession *session, ScriptRemoteSelectorLookupCallback callback,
    void *opaque)
{
    ScriptRemoteDocumentBinding binding =
        navigation_remote_document_binding(session);
    binding.selector_lookup = callback;
    binding.selector_opaque = opaque;
    (void) navigation_rebind_top_level_remote_document(session, &binding);
}

bool navigation_set_runtime_section_identity(NavigationSession *session,
                                             size_t section_identity)
{
    ScriptRemoteDocumentBinding binding =
        navigation_remote_document_binding(session);
    binding.section_identity = section_identity;
    return navigation_rebind_top_level_remote_document(session, &binding);
}

void navigation_enable_document_scripts(
    NavigationSession *session, size_t maximum_scripts,
    size_t maximum_total_bytes, size_t maximum_file_bytes,
    long timeout_ms)
{
    if (session == NULL) return;
    session->document_scripts_enabled = maximum_scripts != 0
                                        && maximum_total_bytes != 0
                                        && maximum_file_bytes != 0
                                        && timeout_ms > 0;
    session->maximum_scripts = maximum_scripts;
    session->maximum_script_bytes = maximum_total_bytes;
    session->maximum_script_file_bytes = maximum_file_bytes;
    session->script_timeout_ms = timeout_ms;
}

void navigation_enable_external_resources(
    NavigationSession *session, size_t maximum_stylesheets,
    size_t maximum_stylesheet_bytes, size_t maximum_stylesheet_file_bytes,
    size_t maximum_images, size_t maximum_image_bytes,
    size_t maximum_image_file_bytes, size_t maximum_decoded_image_bytes,
    long timeout_ms)
{
    if (session == NULL || maximum_stylesheets == 0
        || maximum_stylesheet_bytes == 0
        || maximum_stylesheet_file_bytes == 0 || maximum_images == 0
        || maximum_image_bytes == 0 || maximum_image_file_bytes == 0
        || maximum_decoded_image_bytes == 0 || timeout_ms <= 0) return;
    session->external_resources_enabled = true;
    session->maximum_stylesheets = maximum_stylesheets;
    session->maximum_stylesheet_bytes = maximum_stylesheet_bytes;
    session->maximum_stylesheet_file_bytes = maximum_stylesheet_file_bytes;
    session->maximum_images = maximum_images;
    session->maximum_image_bytes = maximum_image_bytes;
    session->maximum_image_file_bytes = maximum_image_file_bytes;
    session->maximum_decoded_image_bytes = maximum_decoded_image_bytes;
    session->resource_timeout_ms = timeout_ms;
}

void navigation_enable_web_fonts(
    NavigationSession *session, size_t maximum_attempts,
    size_t maximum_total_encoded_bytes,
    size_t maximum_single_encoded_bytes,
    size_t maximum_face_backend_bytes, long timeout_ms)
{
#ifndef TILEFINCH_HAVE_FREETYPE
    (void) session;
    (void) maximum_attempts;
    (void) maximum_total_encoded_bytes;
    (void) maximum_single_encoded_bytes;
    (void) maximum_face_backend_bytes;
    (void) timeout_ms;
    return;
#else
    if (session == NULL || maximum_attempts == 0
        || maximum_attempts
               > TILEFINCH_WEB_FONT_FAMILY_LIMIT * 2u
                 * STYLE_WEB_FONT_SOURCE_LIMIT
        || maximum_total_encoded_bytes == 0
        || maximum_single_encoded_bytes == 0
        || maximum_single_encoded_bytes > maximum_total_encoded_bytes
        || maximum_face_backend_bytes == 0 || timeout_ms <= 0) return;
    session->web_fonts_enabled = true;
    session->maximum_font_attempts = maximum_attempts;
    session->maximum_font_bytes = maximum_total_encoded_bytes;
    session->maximum_font_file_bytes = maximum_single_encoded_bytes;
    session->maximum_font_face_backend_bytes = maximum_face_backend_bytes;
    session->font_timeout_ms = timeout_ms;
#endif
}

bool navigation_set_user_css(NavigationSession *session, const char *css,
                             size_t length)
{
    if (session == NULL || session->budget == NULL
        || (css == NULL && length != 0) || length > 128 * 1024) return false;
    char *copy = NULL;
    if (length != 0) {
        copy = budget_malloc(session->budget, length + 1);
        if (copy == NULL) return false;
        memcpy(copy, css, length);
        copy[length] = '\0';
    }
    budget_free(session->budget, session->user_css);
    session->user_css = copy;
    session->user_css_length = length;
    session->style_rebuild_required = true;
    return true;
}

void navigation_set_reader_candidate_mode(NavigationSession *session,
                                          bool enabled)
{
    if (session != NULL) session->prepare_reader_candidates = enabled;
}

void navigation_enable_frame_capability_trace(NavigationSession *session,
                                              bool enabled)
{
    if (session != NULL) session->trace_frame_capabilities = enabled;
}

void navigation_enable_page_capability_trace(NavigationSession *session,
                                             bool enabled)
{
    if (session != NULL) session->trace_page_capabilities = enabled;
}

void navigation_enable_diagnostic_frame_safari(NavigationSession *session,
                                               bool enabled)
{
    if (session != NULL) session->diagnostic_frame_safari = enabled;
}

uint64_t navigation_begin(NavigationSession *session)
{
    if (session == NULL) return 0;
    session->generation++;
    if (session->generation == 0) session->generation++;
    session->cancelled = false;
    session->navigation_started_us = navigation_now_us();
    session->last_http_status = 0;
    session->last_transport_code = 0;
    session->last_tls_verify_result = 0;
    session->last_tls_verify_result_available = false;
    session->last_tls_verification_failed = false;
    session->last_transport_timed_out = false;
    session->last_tls12_compatibility_retry = false;
    session->last_tls_version[0] = '\0';
    session->last_tls_peer_issuer[0] = '\0';
    session->last_error[0] = '\0';
    /* Direct navigation_commit_html callers do not pass through the
       asynchronous load-state metric reset. A prior provisional paint must
       never suppress the next navigation's preview/image continuation. */
    session->performance.partial_paints = 0;
    session->loads_started++;
    return session->generation;
}

void navigation_cancel(NavigationSession *session, uint64_t generation)
{
    if (session == NULL || generation == 0
        || generation != session->generation || session->cancelled) return;
    session->cancelled = true;
    session->loads_cancelled++;
}

static bool history_push(NavigationSession *session, const char *url,
                         const char *title)
{
    NavigationEntry entry = {0};
    entry.url = bounded_copy(session->budget, url, NAVIGATION_URL_LIMIT);
    entry.title = bounded_copy(session->budget, title,
                               NAVIGATION_TITLE_LIMIT);
    if (entry.url == NULL || entry.title == NULL) {
        entry_destroy(session->budget, &entry);
        return false;
    }
    /* Allocate the complete candidate before pruning forward history or
       evicting the oldest entry.  Allocation failure must leave the visible
       history cursor and every existing entry untouched. */
    if (session->history_count != 0
        && session->history_index + 1 < session->history_count) {
        for (size_t i = session->history_index + 1;
             i < session->history_count; i++) {
            entry_destroy(session->budget, &session->history[i]);
            session->history_pruned++;
        }
        session->history_count = session->history_index + 1;
    }
    if (session->history_count == session->history_capacity) {
        entry_destroy(session->budget, &session->history[0]);
        memmove(session->history, session->history + 1,
                (session->history_capacity - 1) * sizeof(*session->history));
        memset(&session->history[session->history_capacity - 1], 0,
               sizeof(*session->history));
        session->history_count--;
        if (session->history_index > 0) session->history_index--;
        session->history_pruned++;
    }
    session->history[session->history_count] = entry;
    session->history_index = session->history_count;
    session->history_count++;
    return true;
}

static bool page_capability_trace_setup(NavigationSession *session)
{
    if (!session->trace_page_capabilities) return true;
    static const char setup[] =
        "(()=>{const seen=new Map(),missing=new Set(),events=[],first=[],shapes=new Map(),traced=new WeakMap(),rawByProxy=new WeakMap();"
        "const state=globalThis.__tilefinchPageTrace={seen,missing,events,first,shapes,observerEntryTypes:[],domQueries:[],domMutations:[]};const record=name=>{const count=seen.get(name)||0;seen.set(name,count+1);if(count===0&&first.length<512)first.push(name);if(events.length===512)events.shift();events.push(name);},shape=(name,value)=>{if(!shapes.has(name))shapes.set(name,value===null?'null':typeof value);};globalThis.addEventListener('error',()=>{if(state.failureEvents===undefined){state.failureEvents=events.slice(-128);state.failureFirst=first.slice()}},{capture:true});"
        "const trace=(target,label,depth=0)=>{if(target===null||(typeof target!=='object'&&typeof target!=='function'))return target;if(rawByProxy.has(target))return target;if(typeof target.nodeType==='number'||ArrayBuffer.isView(target)||target instanceof ArrayBuffer)return target;if(traced.has(target))return traced.get(target);const raw=value=>rawByProxy.get(value)||value,rawArgs=args=>args.map(raw),proxy=new Proxy(target,{has(object,key){const present=Reflect.has(object,key);if(typeof key!=='symbol'){const name=label+'.'+String(key);record('has:'+name);if(!present)missing.add(name);}return present;},get(object,key){let name=label;if(typeof key!=='symbol'){name=label+'.'+String(key);record('get:'+name);if(!(key in object))missing.add(name);}const descriptor=Reflect.getOwnPropertyDescriptor(object,key);if(descriptor&&Object.prototype.hasOwnProperty.call(descriptor,'value')&&!descriptor.configurable&&!descriptor.writable){shape(name,descriptor.value);return descriptor.value;}const value=Reflect.get(object,key,object);shape(name,value);if(name==='call:new:PerformanceObserver.observe.options.entryTypes'&&Array.isArray(value))state.observerEntryTypes=value.slice(0,8).map(item=>String(item).slice(0,48));return depth===0&&typeof value!=='function'?value:trace(value,name,Math.max(0,depth-1));},set(object,key,value,receiver){if(typeof key!=='symbol'){const name=label+'.'+String(key)+'=';record('set:'+name);if(!(key in object))missing.add(name);}return Reflect.set(object,key,raw(value),raw(receiver));},apply(target,thisArg,args){const name='call:'+label;record(name);const unwrapped=rawArgs(args),callArgs=name==='call:new:PerformanceObserver.observe'&&unwrapped.length!==0?[trace(unwrapped[0],name+'.options',1),...unwrapped.slice(1)]:unwrapped,value=Reflect.apply(target,raw(thisArg),callArgs);shape(name,value);return trace(value,name,name==='call:performance.getEntries'?2:Math.max(0,depth-1));},construct(target,args,newTarget){const name='new:'+label;record(name);const value=Reflect.construct(target,rawArgs(args),raw(newTarget));shape(name,value);return depth===0?value:trace(value,name,depth-1);}});traced.set(target,proxy);rawByProxy.set(proxy,target);return proxy;};"
        "const install=(name,depth=2)=>{const value=globalThis[name];if(!value)return;const wrapped=trace(value,name,depth),descriptor=Object.getOwnPropertyDescriptor(globalThis,name);if(descriptor&&descriptor.configurable)Object.defineProperty(globalThis,name,{configurable:true,enumerable:descriptor.enumerable,writable:true,value:wrapped});else globalThis[name]=wrapped;};globalThis.__tilefinchTraceValue=trace;globalThis.__tilefinchGlobalProxy=trace(globalThis,'global',2);"
        "const rawDocument=globalThis.document;"
        "for(const name of ['navigator','performance','crypto','screen','history','Intl','trustedTypes'])install(name);"
        "for(const name of ['XMLHttpRequest','PerformanceObserver','Blob','URL','Worker','ReadableStream','MutationObserver','RTCPeerConnection','RTCSessionDescription','RTCIceCandidate','RTCDataChannel','GPU','TrustedHTML','TrustedScript','TrustedScriptURL','TrustedTypePolicy','TrustedTypePolicyFactory'])if(typeof globalThis[name]==='function')install(name);"
        "const xhrGet=XMLHttpRequest.prototype.getResponseHeader;XMLHttpRequest.prototype.getResponseHeader=function(name){const value=xhrGet.call(this,name);globalThis.__tilefinchXHRLastHeaderRead=String(name)+'='+String(value);return value;};})();";
    if (!script_runtime_evaluate_diagnostic(
            session->page.runtime, setup, "<page-capability-trace>",
            &session->page.script_result)) return false;
    static const char query_setup[] =
        "(()=>{const state=globalThis.__tilefinchPageTrace,hash=value=>{let h="
        "2166136261,text=String(value);for(let i=0;i<text.length;i++){h^="
        "text.charCodeAt(i);h=Math.imul(h,16777619)}return(h>>>0).toString(16)},"
        "category=value=>{"
        "if(typeof value!=='string')return'n';const text=value.trim();"
        "return/^#[A-Za-z_][\\w-]*$/.test(text)?'i':"
        "/^[A-Za-z][\\w-]*$/.test(text)?'t':"
        "/^[.][A-Za-z_][\\w-]*$/.test(text)?'c':'x'},"
        "trace=(prototype,name,kind)=>{const original=prototype&&prototype[name];"
        "if(typeof original!=='function')return;prototype[name]=function(value){"
        "const result=original.call(this,value),type=category(value);let extra='';"
        "if(result===null&&type==='i'){const found=document.getElementById("
        "value.trim().slice(1)),root=this.getRootNode?.(),rootKind="
        "root===document?'d':root?.host?'s':'o';extra=':'+(found?'y':'n')+':'"
        "+(found&&this.contains?.(found)?'y':'n')+':'+rootKind+':'"
        "+(root?.host?(root.host.isConnected?'c':'d'):'-')}"
        "if(state.domQueries.length<64)"
        "state.domQueries.push(kind+':'+(this.localName||this.nodeType||0)+':'"
        "+(this.isConnected?'c':'d')+':'+type+':'"
        "+(result===null?'null':name==='querySelectorAll'?String(result.length):"
        "String(result.localName||'node'))+(type==='i'?':'+hash(value.trim()."
        "slice(1)):'')+extra);return result}};"
        "state.rawFragmentQueryAll=DocumentFragment.prototype.querySelectorAll;"
        "for(const [prototype,kind]of[[Document.prototype,'d'],"
        "[DocumentFragment.prototype,'f'],[Element.prototype,'e']]){"
        "trace(prototype,'querySelector',kind+'1');"
        "trace(prototype,'querySelectorAll',kind+'a')}const setAttribute="
        "Element.prototype.setAttribute;Element.prototype.setAttribute=function("
        "name,value){const result=setAttribute.call(this,name,value);if(String(name)."
        "toLowerCase()==='id'&&state.domMutations.length<64)state.domMutations.push("
        "'id:'+String(this.localName)+':'+hash(value)+':'"
        "+(this.isConnected?'c':'d'));return result};const appendChild="
        "Node.prototype.appendChild;Node.prototype.appendChild=function(child){"
        "const result=appendChild.call(this,child);if(state.domMutations.length<64)"
        "state.domMutations.push('append:'+(this.localName||this.nodeType||0)+':'"
        "+(child?.localName||child?.nodeType||0)+':'"
        "+(child?.id?hash(child.id):'-')+':'"
        "+(child?.isConnected?'c':'d'));return result};const inner="
        "Object.getOwnPropertyDescriptor(Element.prototype,'innerHTML');if(inner?."
        "set)Object.defineProperty(Element.prototype,'innerHTML',{...inner,set(value)"
        "{inner.set.call(this,value);if(state.domMutations.length<64)state."
        "domMutations.push('html:'+String(this.localName)+':'"
        "+String(value).length+':'+String(this.childNodes.length))}})})();";
    if (!script_runtime_evaluate_diagnostic(
        session->page.runtime, query_setup, "<page-query-trace>",
        &session->page.script_result)) return false;
    static const char mutation_setup[] =
        "(()=>{const state=globalThis.__tilefinchPageTrace,hash=value=>{let h="
        "2166136261,text=String(value);for(let i=0;i<text.length;i++){h^="
        "text.charCodeAt(i);h=Math.imul(h,16777619)}return(h>>>0).toString(16)},"
        "push=(kind,node)=>{if(state.domMutations.length>=64||!node)return;"
        "state.domMutations.push(kind+':'+(node.localName||node.nodeType||0)+':'"
        "+(node.id?hash(node.id):'-')+':'"
        "+(node.isConnected?'c':'d')+':'+(node.getRootNode?.()?.host?'s':'d'))};"
        "const observer=new MutationObserver(records=>{for(const record of records)"
        "{if(state.domMutations.length>=64)break;if(record.type==='attributes')"
        "push('id',record.target);else{for(const node of record.addedNodes)push('a',node);"
        "for(const node of record.removedNodes)push('r',node)}}}),observe=root=>"
        "observer.observe(root,{subtree:true,childList:true,attributes:true,"
        "attributeFilter:['id']}),snapshot=(kind,root)=>{if(state.domMutations.length"
        ">=64)return;const ids=state.rawFragmentQueryAll.call(root,'[id]'),hashes=[];"
        "for(let i=0;i<ids.length&&i<8;i++)hashes.push(hash(ids[i].id));state."
        "domMutations.push(kind+':'+root.childNodes.length+':'+hashes.join(','))},"
        "instrument=root=>{observe(root);const inner=Object.getOwnPropertyDescriptor("
        "root,'innerHTML');if(inner?.set&&inner.configurable)Object.defineProperty("
        "root,'innerHTML',{...inner,set(value){inner.set.call(this,value);snapshot("
        "'shadow-html:'+String(value).length,this)}});for(const name of['appendChild',"
        "'insertBefore','replaceChild','append','prepend','replaceChildren']){const "
        "original=root[name];if(typeof original!=='function')continue;try{root[name]="
        "function(...args){const result=original.apply(this,args);snapshot('shadow-'+"
        "name,this);return result}}catch(_){}}},attachShadow=Element.prototype.attachShadow;"
        "Element.prototype.attachShadow=function(init){const root=attachShadow.call("
        "this,init);instrument(root);push('shadow',this);return root};observe(document);"
        "state.domMutationObserver=observer})();";
    return script_runtime_evaluate_diagnostic(
        session->page.runtime, mutation_setup, "<page-mutation-trace>",
        &session->page.script_result);
}

static void page_capability_trace_report(NavigationSession *session)
{
    if (!session->trace_page_capabilities || session->page.runtime == NULL)
        return;
    static const char report[] =
        "{const t=__tilefinchPageTrace;globalThis.__tilefinchPageSummaryBeforeTrace=globalThis.pocSummary;const missing=[...t.missing].filter(name=>!/^global[.][A-Za-z0-9]{5,}=?$/.test(name)).sort(),events=t.failureEvents||t.events,first=t.failureFirst||t.first,important=first.filter(name=>!name.includes(':global.')||/(Worker|Blob|URL|Stream|Observer|WebAssembly|Intl|crypto|performance|navigator|trustedTypes|Trusted)/.test(name)),types=[...t.shapes].filter(([name])=>/^(?:(?:new|call):)?(?:navigator|performance|crypto|Blob|URL|Worker|ReadableStream|trustedTypes|Trusted)/.test(name)).slice(0,24).map(([name,type])=>name+':'+type);globalThis.pocSummary='types='+types.join(',')+'|observer-types='+t.observerEntryTypes.join(',')+'|xhr-header='+(globalThis.__tilefinchXHRLastHeaderRead||'none')+'|first='+important.slice(0,48).join(',')+'|tail='+events.slice(-32).join(',')+'|missing='+missing.join(',')+'|dom-queries='+t.domQueries.join(',')+'|dom-mutations='+t.domMutations.join(',');}";
    if (script_runtime_evaluate_diagnostic(
            session->page.runtime, report, "<page-capability-report>",
            &session->page.script_result)) {
        snprintf(session->last_page_trace, sizeof(session->last_page_trace),
                 "%s", session->page.script_result.summary);
        static const char restore[] =
            "globalThis.pocSummary="
            "globalThis.__tilefinchPageSummaryBeforeTrace;";
        (void) script_runtime_evaluate_diagnostic(
            session->page.runtime, restore, "<page-capability-restore>",
            &session->page.script_result);
    }
}

bool navigation_collect_page_capability_trace(NavigationSession *session)
{
    if (session == NULL || !session->trace_page_capabilities
        || session->page.runtime == NULL) return false;
    page_capability_trace_report(session);
    return session->last_page_trace[0] != '\0';
}

typedef struct {
    lxb_dom_node_t *node;
    char *url;
    uint64_t request_id;
    size_t maximum_bytes;
    bool script;
    bool module;
    bool cors;
    bool render_blocking_style;
    TilefinchCredentialsMode credentials;
    bool launched;
    bool complete;
} NavigationPreload;

#define NAVIGATION_PRELOAD_LIMIT 8
#define NAVIGATION_PRELOAD_CONCURRENCY 2
#define NAVIGATION_PRELOAD_SCAN_TAG_LIMIT 4096
#define NAVIGATION_STYLESHEET_INPUT_LIMIT 64

typedef struct {
    lxb_dom_node_t *node;
    uint64_t hash;
    bool style;
} NavigationStylesheetInput;

typedef enum {
    NAVIGATION_PARSER_CHECKPOINT_OK = 0,
    NAVIGATION_PARSER_CHECKPOINT_SOFT_REFUSAL,
    NAVIGATION_PARSER_CHECKPOINT_HARD_FAILURE
} NavigationParserCheckpointOutcome;

typedef enum {
    NAVIGATION_PARSER_REFUSAL_NONE = 0,
    NAVIGATION_PARSER_REFUSAL_METADATA,
    NAVIGATION_PARSER_REFUSAL_FINGERPRINT,
    NAVIGATION_PARSER_REFUSAL_STYLESHEET,
    NAVIGATION_PARSER_REFUSAL_MUTATION,
    NAVIGATION_PARSER_REFUSAL_CSSOM,
    NAVIGATION_PARSER_REFUSAL_SCRIPT
} NavigationParserRefusalReason;

typedef struct {
    NavigationSession *session;
    PocDocument *document;
    DocumentParser *parser;
    uint64_t started_us;
    uint64_t response_headers_elapsed_us;
    uint64_t parse_us;
    char document_url[NAVIGATION_URL_LIMIT];
    char resource_base_url[NAVIGATION_URL_LIMIT];
    /* The request referrer uses the incumbent document's policy. The response
       policy below governs requests emitted by the new document, so retain
       both until its realm is created. */
    char referrer_source_url[NAVIGATION_URL_LIMIT];
    char initiator_referrer_policy[128];
    char document_referrer[NAVIGATION_URL_LIMIT];
    char referrer_policy[128];
    ScriptRuntime *runtime;
    /* Complete, visibly useful body prefix observed before an author parser
       callback. Bounded refresh attempts may preserve later server-rendered
       actions without serializing the body before every script. It is a
       rollback source only; the live document remains authoritative unless
       hydration collapses its content. */
    DocumentBodySnapshot server_body_snapshot;
    size_t server_body_snapshot_node_count;
    size_t server_body_snapshot_text_bytes;
    uint8_t server_body_snapshot_refresh_attempts;
    FetchSchedulerDomain *fetch_domain;
    FetchScheduler *scheduler;
    StreamingScriptState scripts;
    ScriptResult script_result;
    Stylesheet blocking_stylesheet;
    /* Parser-time canvas drawing must survive retirement of a short-lived
       script realm and move atomically with the candidate document. */
    ImageResources canvas_images;
    StylesheetDocumentResources stylesheet_resources;
    ExternalStylesheetStats blocking_stylesheets;
    LayoutReuseCache *preview_layout_reuse;
    LayoutDocument cssom_layout;
    const FontSet *fonts;
    const ImageResources *images;
    ViewportContext viewport;
    int viewport_width;
    NavigationPreload preloads[NAVIGATION_PRELOAD_LIMIT];
    size_t preload_count;
    char preload_scan_tag[NAVIGATION_PRELOAD_SCAN_TAG_LIMIT];
    size_t preload_scan_tag_length;
    char preload_scan_quote;
    unsigned preload_scan_raw_text;
    bool preload_scan_in_tag;
    bool preload_scan_in_comment;
    bool preload_scan_tag_overflow;
    bool preload_scan_done;
    bool preload_scan_disabled;
    NavigationStylesheetEventRecord stylesheet_events[
        NAVIGATION_STYLESHEET_EVENT_LIMIT];
    size_t stylesheet_event_count;
    uint64_t preload_deadline_us;
    uint64_t blocking_stylesheet_fingerprint;
    NavigationStylesheetInput *stylesheet_inputs;
    size_t stylesheet_input_count;
    size_t parsed_body_bytes;
    size_t closed_element_count;
    size_t preview_checkpoint_checks;
    /* Parser-blocking JavaScript is useful only while its bounded cumulative
       stage remains healthy. These counters are staging-only: EOF either
       adopts the live realm or commits the still-authoritative server DOM. */
    uint64_t parser_script_stage_us;
    size_t parser_script_stage_work;
    size_t parser_script_consecutive_failures;
    bool head_closed;
    bool body_closed;
    bool blocking_stylesheet_ready;
    bool blocking_stylesheet_user_context_neutral;
    bool cssom_layout_ready;
    bool blocking_stylesheet_fingerprint_valid;
    bool stylesheet_input_overflow;
    bool stylesheet_event_baseline_adopted;
    bool runtime_transferred;
    bool scheduler_transferred;
    bool isolates_remote_document;
    bool runtime_document_replacement;
    bool script_disabled_after_failure;
    bool optional_work_shed;
    bool parser_script_circuit_open;
    NavigationParserCheckpointOutcome parser_checkpoint_outcome;
    NavigationParserRefusalReason pending_parser_refusal;
    bool pre_eof_preview_attempted;
    bool pre_eof_preview_painted;
    bool pre_eof_preview_style_refresh_attempted;
} NavigationDocumentStream;
