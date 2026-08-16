void script_runtime_detach_document(ScriptRuntime *runtime,
                                    const PocDocument *document)
{
    uintptr_t owner = document_owner_identity(document);
    if (runtime == NULL || owner == 0) return;
    DomBridge *bridge = &runtime->bridge;

    bool preserve_foreign_source_scope = false;
    if (bridge->current_script_source_active) {
        for (size_t i = 0; i < bridge->script_source_node_count; i++) {
            const ScriptSourceNode *source = &bridge->script_source_nodes[i];
            if (source->section == bridge->current_script_source_section
                && strcmp(source->key,
                          bridge->current_script_source_key) == 0
                && source->owner_document_identity != owner) {
                preserve_foreign_source_scope = true;
                break;
            }
        }
    }

    /* Clear work before invalidating its handle so request cancellation and
       pin accounting still see the slot. Owner identities were captured while
       nodes were live; no retirement decision dereferences a DOM pointer. */
    for (size_t i = 0; i < SCRIPT_DYNAMIC_TASK_LIMIT; i++) {
        ScriptDynamicTask *task = &bridge->dynamic_scripts[i];
        bool owned = task->active
            && task->owner_document_identity == owner;
        if (!owned && task->active && task->owner_document_identity == 0) {
            size_t slot = 0;
            owned = js_rt_bridge_node_slot_for_handle(
                        bridge, task->node_handle, &slot)
                && bridge->node_owner_document_identities[slot] == owner;
        }
        if (owned) {
            js_rt_dynamic_task_clear(bridge, task, true, true);
        }
    }
    for (size_t i = 0; i < bridge->node_count; i++) {
        if (bridge->node_owner_document_identities[i] == owner) {
            bridge_invalidate_node_slot(bridge, i);
        }
    }
    bridge_scroll_intents_detach_document(bridge, owner);
    script_element_states_detach_document(bridge, document);

    size_t source_output = 0;
    for (size_t i = 0; i < bridge->script_source_node_count; i++) {
        if (bridge->script_source_nodes[i].owner_document_identity == owner) {
            continue;
        }
        if (source_output != i) {
            bridge->script_source_nodes[source_output] =
                bridge->script_source_nodes[i];
        }
        source_output++;
    }
    if (source_output != bridge->script_source_node_count) {
        memset(bridge->script_source_nodes + source_output, 0,
               (bridge->script_source_node_count - source_output)
                   * sizeof(bridge->script_source_nodes[0]));
    }
    bridge->script_source_node_count = source_output;

    if (bridge->current_script_owner_document_identity == owner) {
        (void) document_current_script_set(
            runtime->context, JS_NULL, &runtime->result);
    }
    if (bridge->current_script_source_active
        && !preserve_foreign_source_scope) {
        bridge->current_script_source_active = false;
        bridge->current_script_source_key[0] = '\0';
    }

    if (runtime->document == document) {
        memset(&bridge->mutations, 0, sizeof(bridge->mutations));
        bridge->document = NULL;
        bridge->layout = NULL;
        bridge->images = NULL;
        bridge->stylesheet = NULL;
        runtime->document = NULL;
        runtime->relayout_dirty = false;
    } else {
        size_t mutation_output = 0;
        for (size_t i = 0; i < bridge->mutations.count; i++) {
            if (bridge->mutations.records[i].owner_document_identity
                == owner) {
                continue;
            }
            if (mutation_output != i) {
                bridge->mutations.records[mutation_output] =
                    bridge->mutations.records[i];
            }
            mutation_output++;
        }
        if (mutation_output == 0) {
            memset(&bridge->mutations, 0, sizeof(bridge->mutations));
        } else {
            memset(bridge->mutations.records + mutation_output, 0,
                   (bridge->mutations.count - mutation_output)
                       * sizeof(bridge->mutations.records[0]));
            bridge->mutations.count = mutation_output;
        }
    }
}

bool script_runtime_rebind_document(ScriptRuntime *runtime,
                                    PocDocument *document,
                                    ScriptResult *result)
{
    if (runtime == NULL || document == NULL || document->html == NULL)
        return false;
    /* Direct embedders may rebind while the old tree is still live.  Retire
       only that document's state.  A caller which used the required explicit
       pre-destruction detach leaves runtime->document NULL; in that case the
       remaining handles intentionally belong to other live documents (for
       example, the streamed script corpus driving a section replacement). */
    if (runtime->document != NULL && runtime->document != document) {
        script_runtime_detach_document(runtime, runtime->document);
    }
    runtime->document = document;
    runtime->bridge.document = document;
    runtime->bridge.document_base_dirty = true;
    runtime->relayout_dirty = false;
    bool rebound = runtime_call_host(
        runtime, SCRIPT_HOST_REBIND_DOCUMENT, 0, NULL, NULL);
    js_rt_runtime_update_result(runtime, result);
    return rebound;
}

void script_runtime_relocate_document_storage(ScriptRuntime *runtime,
                                              PocDocument *document)
{
    if (runtime == NULL || document == NULL) return;
    runtime->document = document;
    runtime->bridge.document = document;
    runtime->bridge.document_base_dirty = true;
}

size_t script_runtime_collect_and_trim(ScriptRuntime *runtime)
{
    if (runtime == NULL || runtime->runtime == NULL
        || runtime->quickjs_pool == NULL) return 0;
    /* A DOM wrapper can participate in a self-cycle (classList and author
       listeners are common examples).  Bellard QuickJS removes that cycle
       in one pass and clears its WeakRef/FinalizationRegistry target in the
       next.  This explicit full-collection boundary therefore needs both
       bounded passes before allocator trimming. */
    JS_RunGC(runtime->runtime);
    JS_RunGC(runtime->runtime);
    /* QuickJS raises its automatic-GC threshold from the allocation peak
       which triggered a collection. An embedding-requested full collection
       can reduce the live heap dramatically while leaving that old threshold
       above the hard memory limit, so a later synchronous allocation burst
       reaches OOM without another automatic collection. Re-arm the ordinary
       1.5x heuristic from post-collection live usage, retaining at least the
       dependency's default 256 KiB growth window and a bounded pre-limit
       reserve. */
    JSMemoryUsage usage;
    memset(&usage, 0, sizeof(usage));
    JS_ComputeMemoryUsage(runtime->runtime, &usage);
    if (usage.malloc_size >= 0) {
        uint64_t current = (uint64_t) usage.malloc_size;
        uint64_t growth = current >> 1;
        if (growth < 256u * 1024u) growth = 256u * 1024u;
        uint64_t threshold = current > UINT64_MAX - growth
            ? UINT64_MAX : current + growth;
        if (usage.malloc_limit > 0
            && threshold >= (uint64_t) usage.malloc_limit) {
            uint64_t limit = (uint64_t) usage.malloc_limit;
            uint64_t reserve = limit / 8u;
            if (reserve > 256u * 1024u) reserve = 256u * 1024u;
            threshold = limit > reserve ? limit - reserve : current;
            if (threshold < current) threshold = current;
        }
        JS_SetGCThreshold(
            runtime->runtime,
            threshold > SIZE_MAX ? SIZE_MAX : (size_t) threshold);
    }
    return budget_quickjs_pool_trim(runtime->quickjs_pool, 0);
}

size_t script_runtime_heap_remaining(const ScriptRuntime *runtime)
{
    if (runtime == NULL || runtime->runtime == NULL
        || runtime->quickjs_pool == NULL) return 0;
    size_t limit = runtime->base_memory_limit;
    if (runtime->boot_window_active) {
        limit = runtime->boot_window_bytes > SIZE_MAX - limit
            ? SIZE_MAX : limit + runtime->boot_window_bytes;
    }
    /* The budget allocator maintains the exact QuickJS malloc_size at every
       allocation/free, so admission can be O(1). JS_ComputeMemoryUsage walks
       the entire live heap and is reserved for explicit diagnostics. */
    size_t current =
        budget_quickjs_pool_js_malloc_current(runtime->quickjs_pool);
    return current < limit ? limit - current : 0;
}

size_t script_runtime_inline_execution_reserve(
    const ScriptRuntime *runtime, size_t source_length)
{
    if (runtime == NULL || runtime->runtime == NULL) return 0;
    const uint64_t minimum = 512u * 1024u;
    const uint64_t maximum = 2u * 1024u * 1024u;
    const uint64_t qualified_minimum = 4u * 1024u * 1024u;
    uint64_t limit = runtime->base_memory_limit;
    if (runtime->boot_window_active) {
        limit = runtime->boot_window_bytes > UINT64_MAX - limit
            ? UINT64_MAX : limit + runtime->boot_window_bytes;
    }
    if (limit < qualified_minimum) return 0;
    /* A fixed half-heap reserve made a small late inline script disappear
       whenever platform/test-library state happened to occupy just over half
       the heap. Scale the prospective expansion with source size while
       retaining a 512 KiB floor for DOM work and a 2 MiB ceiling for large
       bootstraps. This remains conservative on PSP without making execution
       depend on incidental pre-script heap occupancy. */
    uint64_t expansion = source_length > UINT64_MAX / 4u
        ? UINT64_MAX : (uint64_t) source_length * 4u;
    uint64_t reserve = minimum;
    if (expansion > reserve) reserve = expansion;
    if (reserve > maximum) reserve = maximum;
    if (reserve > limit / 2u) reserve = limit / 2u;
    return reserve > SIZE_MAX ? SIZE_MAX : (size_t) reserve;
}

void script_runtime_bound_allocator_cache(ScriptRuntime *runtime,
                                          size_t small_class_limit,
                                          size_t large_class_limit)
{
    if (runtime == NULL) return;
    budget_quickjs_pool_set_cache_limits(
        runtime->quickjs_pool, small_class_limit, large_class_limit);
}

bool script_runtime_commit_same_document(ScriptRuntime *runtime,
                                         const char *url,
                                         const char *old_url,
                                         ScriptResult *result)
{
    if (runtime == NULL || url == NULL || old_url == NULL) return false;
    char previous_url[TILEFINCH_URL_SERIALIZED_LIMIT];
    snprintf(previous_url, sizeof(previous_url), "%s", runtime->document_url);
    if (!bridge_set_document_url(&runtime->bridge, url)) return false;
    JSValue arguments[2] = {
        JS_NewString(runtime->context, url),
        JS_NewString(runtime->context, old_url)
    };
    bool committed = !JS_IsException(arguments[0])
        && !JS_IsException(arguments[1])
        && runtime_call_host(runtime, SCRIPT_HOST_COMMIT_SAME_DOCUMENT, 2,
                        arguments, NULL);
    JS_FreeValue(runtime->context, arguments[1]);
    JS_FreeValue(runtime->context, arguments[0]);
    if (!committed) {
        memcpy(runtime->document_url, previous_url, strlen(previous_url) + 1);
        runtime->bridge.document_base_dirty = true;
    }
    if (!committed && runtime->result.error[0] == '\0') {
        js_rt_record_exception(runtime->context, &runtime->result);
    }
    js_rt_runtime_update_result(runtime, result);
    return committed;
}

bool script_runtime_restore_same_document(ScriptRuntime *runtime,
                                          const char *url,
                                          const char *old_url,
                                          ScriptResult *result)
{
    if (runtime == NULL || url == NULL || old_url == NULL) return false;
    char previous_url[TILEFINCH_URL_SERIALIZED_LIMIT];
    snprintf(previous_url, sizeof(previous_url), "%s", runtime->document_url);
    if (!bridge_set_document_url(&runtime->bridge, url)) return false;
    JSValue arguments[2] = {
        JS_NewString(runtime->context, url),
        JS_NewString(runtime->context, old_url)
    };
    bool restored = !JS_IsException(arguments[0])
        && !JS_IsException(arguments[1])
        && runtime_call_host(runtime, SCRIPT_HOST_RESTORE_SAME_DOCUMENT, 2,
                        arguments, NULL);
    JS_FreeValue(runtime->context, arguments[1]);
    JS_FreeValue(runtime->context, arguments[0]);
    if (!restored) {
        memcpy(runtime->document_url, previous_url, strlen(previous_url) + 1);
        runtime->bridge.document_base_dirty = true;
    }
    if (!restored && runtime->result.error[0] == '\0')
        js_rt_record_exception(runtime->context, &runtime->result);
    js_rt_runtime_update_result(runtime, result);
    return restored;
}

static bool script_runtime_section_state_call(
    ScriptRuntime *runtime, ScriptHostCallback callback, size_t section,
    bool completes_mutations, size_t *count, ScriptResult *result)
{
    if (runtime == NULL || section > INT64_MAX)
        return false;
    js_rt_runtime_arm_watchdog(runtime);
    JSValue argument = JS_NewInt64(runtime->context, (int64_t) section);
    JSValue returned = JS_UNDEFINED;
    bool ok = !JS_IsException(argument)
        && runtime_call_host(runtime, callback, 1, &argument, &returned);
    int64_t converted = 0;
    if (ok && JS_ToInt64(runtime->context, &converted, returned) < 0) {
        js_rt_record_exception(runtime->context, &runtime->result);
        ok = false;
    }
    JS_FreeValue(runtime->context, returned);
    JS_FreeValue(runtime->context, argument);
    /* Restore applies ordinary DOM setters and can enqueue mutation-observer
       jobs. Complete the same microtask/derived-document boundary used by
       author-script and event entry points so aggregate metadata and lazy
       caches cannot remain stale after a section swap. Save is read-only and
       deliberately avoids this PSP-expensive DOM traversal. */
    if (ok && completes_mutations) ok = js_rt_runtime_run_jobs(runtime);
    if (ok && completes_mutations) ok = js_rt_runtime_refresh(runtime);
    if (count != NULL) *count = ok && converted > 0
        ? (size_t) converted : 0;
    runtime->result.success = ok;
    js_rt_runtime_update_result(runtime, result);
    return ok;
}

bool script_runtime_save_section_state(ScriptRuntime *runtime,
                                       size_t section, size_t *count,
                                       ScriptResult *result)
{
    return script_runtime_section_state_call(
        runtime, SCRIPT_HOST_SAVE_SECTION_STATE, section, false, count, result);
}

bool script_runtime_restore_section_state(ScriptRuntime *runtime,
                                          size_t section, size_t *count,
                                          ScriptResult *result)
{
    return script_runtime_section_state_call(
        runtime, SCRIPT_HOST_RESTORE_SECTION_STATE, section, true, count, result);
}

bool script_runtime_set_section_identity(ScriptRuntime *runtime,
                                         size_t section_identity)
{
    if (runtime == NULL
        || (runtime->document_scope == SCRIPT_DOCUMENT_SCOPE_CHILD_FRAME
            && section_identity != 0)) return false;
    runtime->bridge.section_identity = section_identity;
    return true;
}

bool script_runtime_set_current_script_source(
    ScriptRuntime *runtime, size_t section_identity, size_t element_ordinal)
{
    if (runtime == NULL) return false;
    int written = snprintf(
        runtime->bridge.current_script_source_key,
        sizeof(runtime->bridge.current_script_source_key),
        "s:%zu:%zu:script", section_identity, element_ordinal);
    if (written <= 0
        || (size_t) written
           >= sizeof(runtime->bridge.current_script_source_key)) return false;
    runtime->bridge.current_script_source_section = section_identity;
    runtime->bridge.current_script_source_active = true;
    return true;
}

void script_runtime_clear_current_script_source(ScriptRuntime *runtime)
{
    if (runtime == NULL) return;
    runtime->bridge.current_script_source_active = false;
    runtime->bridge.current_script_source_key[0] = '\0';
}

bool script_runtime_register_script_source_node(
    ScriptRuntime *runtime, lxb_dom_node_t *node,
    size_t section_identity, size_t element_ordinal)
{
    if (runtime == NULL || node == NULL || section_identity > INT64_MAX
        || runtime->bridge.script_source_node_count
           >= SCRIPT_SOURCE_NODE_LIMIT) return false;
    DomBridge *bridge = &runtime->bridge;
    if (bridge->script_source_node_count
        == bridge->script_source_node_capacity) {
        size_t capacity = bridge->script_source_node_capacity == 0
            ? 16 : bridge->script_source_node_capacity * 2;
        if (capacity > SCRIPT_SOURCE_NODE_LIMIT)
            capacity = SCRIPT_SOURCE_NODE_LIMIT;
        ScriptSourceNode *resized = budget_realloc_category(
            bridge->budget, BUDGET_CATEGORY_JAVASCRIPT,
            bridge->script_source_nodes, capacity * sizeof(*resized));
        if (resized == NULL) return false;
        bridge->script_source_nodes = resized;
        bridge->script_source_node_capacity = capacity;
    }
    ScriptSourceNode *source =
        &bridge->script_source_nodes[bridge->script_source_node_count];
    int written = snprintf(source->key, sizeof(source->key),
                           "s:%zu:%zu:script", section_identity,
                           element_ordinal);
    if (written <= 0 || (size_t) written >= sizeof(source->key)) return false;
    source->node = node;
    source->owner_document_identity = js_rt_node_owner_identity(node);
    source->section = section_identity;
    bridge->script_source_node_count++;
    return true;
}

void script_runtime_clear_script_source_nodes(ScriptRuntime *runtime)
{
    if (runtime == NULL) return;
    budget_free(runtime->bridge.budget,
                runtime->bridge.script_source_nodes);
    runtime->bridge.script_source_nodes = NULL;
    runtime->bridge.script_source_node_count = 0;
    runtime->bridge.script_source_node_capacity = 0;
}

static void capture_cached_error_source(ScriptRuntime *runtime)
{
    if (runtime == NULL || runtime->session == NULL
        || runtime->result.error_source_context[0] != '\0') return;
    const char *open = strchr(runtime->result.error, '(');
    const char *close = open == NULL ? NULL : strchr(open, ')');
    if (close == NULL) return;
    const char *column_colon = close;
    while (column_colon > open && column_colon[-1] != ':') column_colon--;
    if (column_colon == open) return;
    const char *line_colon = column_colon - 1;
    while (line_colon > open && line_colon[-1] != ':') line_colon--;
    if (line_colon == open) return;
    size_t url_length = (size_t) ((line_colon - 1) - (open + 1));
    if (url_length == 0 || url_length >= 2048) return;
    char url[2048];
    memcpy(url, open + 1, url_length);
    url[url_length] = '\0';
    const BrowserCacheEntry *entry = browser_session_cache_lookup(
        runtime->session, url);
    if (entry != NULL) {
        js_rt_capture_error_source_context((const char *) entry->data,
                                     entry->length, url,
                                     &runtime->result);
    }
}
