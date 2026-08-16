static bool clock_source_argument(
    JSContext *context, int argc, JSValueConst *argv,
    ScriptDeterministicClockSource first,
    ScriptDeterministicClockSource last,
    ScriptDeterministicClockSource *source)
{
    int32_t value = -1;
    if (argc != 1 || JS_ToInt32(context, &value, argv[0]) < 0
        || value < (int32_t) first || value > (int32_t) last) {
        JS_ThrowRangeError(context,
                           "invalid deterministic clock observation source");
        return false;
    }
    *source = (ScriptDeterministicClockSource) value;
    return true;
}

static void clock_increment(uint64_t *value)
{
    if (*value < UINT64_MAX) (*value)++;
}

static JSValue js_css_supports(JSContext *context,
                               JSValueConst this_value,
                               int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || bridge->stylesheet == NULL
        || argc < 1 || argc > 2) return JS_FALSE;
    size_t first_length = 0;
    const char *first = JS_ToCStringLen(
        context, &first_length, argv[0]);
    if (first == NULL) return JS_EXCEPTION;
    bool supported = false;
    if (argc == 1) {
        supported = stylesheet_supports_matches(
            (Stylesheet *) bridge->stylesheet, first, first_length);
    } else {
        size_t second_length = 0;
        const char *second = JS_ToCStringLen(
            context, &second_length, argv[1]);
        if (second == NULL) {
            JS_FreeCString(context, first);
            return JS_EXCEPTION;
        }
        if (first_length <= 95 && second_length <= 223) {
            char query[320];
            memcpy(query, first, first_length);
            query[first_length] = ':';
            memcpy(query + first_length + 1, second, second_length);
            supported = stylesheet_supports_matches(
                (Stylesheet *) bridge->stylesheet, query,
                first_length + second_length + 1u);
        }
        JS_FreeCString(context, second);
    }
    JS_FreeCString(context, first);
    return JS_NewBool(context, supported);
}

static JSValue js_stylesheet_has_motion_keyframes(
    JSContext *context, JSValueConst this_value, int argc,
    JSValueConst *argv)
{
    (void) this_value;
    (void) argc;
    (void) argv;
    DomBridge *bridge = JS_GetContextOpaque(context);
    return JS_NewBool(
        context, bridge != NULL && bridge->stylesheet != NULL
                     && bridge->stylesheet->has_motion_keyframes);
}

static double performance_clock_value(DomBridge *bridge, bool advance,
                                      ScriptDeterministicClockSource source)
{
    bool deterministic = bridge != NULL && bridge->deterministic_clock;
    uint64_t current = deterministic
        ? bridge->clock_monotonic_elapsed_ms : js_rt_monotonic_time_ns();
    double elapsed = deterministic
        ? (double) current
        : (bridge == NULL || current < bridge->performance_origin_ns
           ? 0.0
           : (double) (current - bridge->performance_origin_ns)
             / 1000000.0);
    if (deterministic) {
        clock_increment(&bridge->clock_source_counts[source]);
        if (advance
            && bridge->clock_monotonic_elapsed_ms
                   <= UINT64_MAX
                      - SCRIPT_DETERMINISTIC_CLOCK_CALL_QUANTUM_MS) {
            bridge->clock_monotonic_elapsed_ms +=
                SCRIPT_DETERMINISTIC_CLOCK_CALL_QUANTUM_MS;
        }
    }
    if (bridge != NULL && bridge->result != NULL) {
        bridge->result->performance_now_calls++;
        bridge->result->performance_now_last_ms = elapsed;
    }
    return elapsed;
}

static JSValue js_performance_now(JSContext *context,
                                  JSValueConst this_value,
                                  int argc, JSValueConst *argv)
{
    (void) this_value;
    ScriptDeterministicClockSource source;
    if (!clock_source_argument(
            context, argc, argv, SCRIPT_CLOCK_SOURCE_PERFORMANCE_NOW,
            SCRIPT_CLOCK_SOURCE_IDLE_DEADLINE_TIME_REMAINING, &source)) {
        return JS_EXCEPTION;
    }
    DomBridge *bridge = JS_GetContextOpaque(context);
    return JS_NewFloat64(
        context, performance_clock_value(bridge, true, source));
}

static JSValue js_performance_sample(JSContext *context,
                                     JSValueConst this_value,
                                     int argc, JSValueConst *argv)
{
    (void) this_value;
    ScriptDeterministicClockSource source;
    if (!clock_source_argument(
            context, argc, argv, SCRIPT_CLOCK_SOURCE_ANIMATION_FRAME,
            SCRIPT_CLOCK_SOURCE_IDLE_CALLBACK_START, &source)) {
        return JS_EXCEPTION;
    }
    DomBridge *bridge = JS_GetContextOpaque(context);
    return JS_NewFloat64(
        context, performance_clock_value(bridge, false, source));
}

static JSValue js_date_now(JSContext *context, JSValueConst this_value,
                           int argc, JSValueConst *argv)
{
    (void) this_value;
    ScriptDeterministicClockSource source;
    if (!clock_source_argument(
            context, argc, argv, SCRIPT_CLOCK_SOURCE_DATE_NOW,
            SCRIPT_CLOCK_SOURCE_DATE_CONSTRUCTOR, &source)) {
        return JS_EXCEPTION;
    }
    DomBridge *bridge = JS_GetContextOpaque(context);
    bool deterministic = bridge != NULL && bridge->deterministic_clock;
    uint64_t milliseconds = 0;
    if (deterministic) {
        const uint64_t maximum_date_ms = UINT64_C(8640000000000000);
        milliseconds = bridge->clock_wall_elapsed_ms
                           > maximum_date_ms - bridge->clock_origin_ms
            ? maximum_date_ms
            : bridge->clock_origin_ms + bridge->clock_wall_elapsed_ms;
        if (bridge->clock_wall_elapsed_ms
                <= UINT64_MAX
                   - SCRIPT_DETERMINISTIC_CLOCK_CALL_QUANTUM_MS) {
            bridge->clock_wall_elapsed_ms +=
                SCRIPT_DETERMINISTIC_CLOCK_CALL_QUANTUM_MS;
        }
        clock_increment(&bridge->clock_source_counts[source]);
    } else milliseconds = wall_time_ms();
    double current = (double) milliseconds;
    if (bridge != NULL && bridge->result != NULL) {
        bridge->result->date_now_calls++;
        bridge->result->date_now_last_ms = current;
    }
    return JS_NewFloat64(context, current);
}

static JSValue js_document_body_text(JSContext *context,
                                     JSValueConst this_value,
                                     int argc, JSValueConst *argv)
{
    (void) this_value; (void) argc; (void) argv;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || bridge->document == NULL) {
        return JS_NewString(context, "");
    }
    const char *body_text = document_body_text(bridge->document);
    if (body_text == NULL) return JS_ThrowOutOfMemory(context);
    return JS_NewStringLen(context, body_text,
                           bridge->document->body_text_length);
}

static JSValue js_document_current_script(JSContext *context,
                                          JSValueConst this_value,
                                          int argc, JSValueConst *argv)
{
    (void) this_value; (void) argc; (void) argv;
    DomBridge *bridge = JS_GetContextOpaque(context);
    return bridge == NULL ? JS_NULL
                          : JS_DupValue(context, bridge->current_script);
}

static JSValue js_script_force_async(JSContext *context,
                                     JSValueConst this_value,
                                     int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = argc > 0
        ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    ScriptElementState *state = js_rt_script_element_state_find(bridge, node);
    return JS_NewBool(context, state != NULL && state->programmatic
                               && state->html && state->force_async);
}

static JSValue js_script_async_assigned(JSContext *context,
                                        JSValueConst this_value,
                                        int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = argc > 0
        ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    ScriptElementState *state = js_rt_script_element_state_find(bridge, node);
    if (state != NULL && state->programmatic && state->html) {
        state->force_async = false;
    }
    return JS_UNDEFINED;
}

static JSValue js_post_message(JSContext *context,
                               JSValueConst this_value,
                               int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || argc < 3) {
        return JS_ThrowTypeError(context, "frame messaging is not configured");
    }
    int64_t target = 0;
    if (JS_ToInt64(context, &target, argv[0]) < 0) return JS_EXCEPTION;
    size_t json_length = 0, origin_length = 0;
    const char *json = JS_ToCStringLen(context, &json_length, argv[1]);
    const char *origin = JS_ToCStringLen(context, &origin_length, argv[2]);
    if (json == NULL || origin == NULL) {
        if (json != NULL) JS_FreeCString(context, json);
        if (origin != NULL) JS_FreeCString(context, origin);
        return JS_EXCEPTION;
    }
    bool valid = json_length <= 65536 && origin_length <= 2048;
    /* A WindowProxy may outlive a detached iframe's browsing context.
       postMessage to that discarded target is a silent no-op in browsers;
       absence of our optional navigation bridge must not leak as an author
       exception. */
    bool queued = valid && (bridge->post_message == NULL
        || bridge->post_message(
            bridge->post_message_opaque, bridge->host, (long) target,
            json, origin));
    JS_FreeCString(context, origin);
    JS_FreeCString(context, json);
    if (!queued) return JS_ThrowInternalError(context,
                                               "message queue rejected postMessage");
    return JS_UNDEFINED;
}

static JSValue js_request_navigation(JSContext *context,
                                     JSValueConst this_value,
                                     int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || argc < 1)
        return JS_ThrowTypeError(context, "navigation URL is required");
    size_t length = 0;
    const char *url = JS_ToCStringLen(context, &length, argv[0]);
    if (url == NULL) return JS_EXCEPTION;
    bool valid = length > 0 && length < sizeof(bridge->navigation_url)
                 && (strncmp(url, "https://", 8) == 0
                     || strncmp(url, "http://", 7) == 0);
    if (!valid) {
        JS_FreeCString(context, url);
        return JS_ThrowTypeError(context, "invalid navigation URL");
    }
    snprintf(bridge->navigation_url, sizeof(bridge->navigation_url), "%s",
             url);
    bridge->navigation_requested = true;
    bridge->navigation_replace = argc > 1
                                 && JS_ToBool(context, argv[1]) > 0;
    JS_FreeCString(context, url);
    return JS_UNDEFINED;
}

static JSValue js_request_media(JSContext *context,
                                JSValueConst this_value,
                                int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    int64_t handle = 0;
    int32_t command = -1;
    size_t slot = 0;
    if (bridge == NULL || argc < 2
        || JS_ToInt64(context, &handle, argv[0]) < 0
        || JS_ToInt32(context, &command, argv[1]) < 0
        || command < SCRIPT_MEDIA_COMMAND_LOAD
        || command > SCRIPT_MEDIA_COMMAND_SEEK
        || !js_rt_bridge_node_slot_for_handle(bridge, handle, &slot)) {
        return JS_FALSE;
    }
    lxb_dom_node_t *node = bridge->nodes[slot];
    if (node == NULL
        || (node->local_name != LXB_TAG_VIDEO
            && node->local_name != LXB_TAG_AUDIO)) {
        return JS_FALSE;
    }
    ScriptMediaRequest request = {
        .command = (ScriptMediaCommand) command,
        .node_handle = handle
    };
    if (argc >= 3) {
        size_t length = 0;
        const char *source = JS_ToCStringLen(context, &length, argv[2]);
        if (source == NULL) return JS_EXCEPTION;
        bool valid = length == 0
            || (length < sizeof(request.source)
                && (strncmp(source, "https://", 8) == 0
                    || strncmp(source, "http://", 7) == 0));
        if (valid) memcpy(request.source, source, length + 1u);
        JS_FreeCString(context, source);
        if (!valid) return JS_FALSE;
    }
    if ((command == SCRIPT_MEDIA_COMMAND_LOAD
         || command == SCRIPT_MEDIA_COMMAND_PLAY
         || command == SCRIPT_MEDIA_COMMAND_SEEK)
        && request.source[0] == '\0') return JS_FALSE;
    if (command == SCRIPT_MEDIA_COMMAND_SEEK) {
        if (argc < 4
            || JS_ToFloat64(context, &request.value, argv[3]) < 0
            || !(request.value >= 0)) {
            return JS_FALSE;
        }
    }
    char *source_copy = NULL;
    if (request.source[0] != '\0') {
        source_copy = js_rt_dynamic_copy_text(
            bridge->budget, request.source);
        if (source_copy == NULL) {
            return JS_ThrowOutOfMemory(context);
        }
    }
    budget_free(bridge->budget, bridge->media_source);
    bridge->media_command = request.command;
    bridge->media_node_handle = request.node_handle;
    bridge->media_value = request.value;
    bridge->media_source = source_copy;
    bridge->media_requested = true;
    bridge->node_retention_flags[slot] |= BRIDGE_NODE_NATIVE_PIN;
    return JS_TRUE;
}

static JSValue js_request_scroll(JSContext *context, JSValueConst this_value,
                                 int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    double requested = 0;
    if (bridge == NULL || argc < 1 || JS_ToFloat64(context, &requested,
                                                   argv[0]) < 0) {
        return JS_ThrowTypeError(context, "scroll position is required");
    }
    /* Return the position the native viewport can actually represent.  The
       JavaScript scroll properties and geometry bridge must observe that
       accepted position in this task, before navigation consumes the queued
       history update.  The navigation-side clamp remains as a defensive
       boundary for callers that do not originate in JavaScript. */
    if (!(requested >= 0)) requested = 0; /* Also makes NaN become zero. */
    if (requested > INT_MAX) requested = INT_MAX;
    int next = (int) requested;
    if (bridge->layout != NULL) {
        int maximum = bridge->layout->height > bridge->viewport_height
            ? bridge->layout->height - bridge->viewport_height : 0;
        if (next > maximum) next = maximum;
    }
    bridge->page_scroll_y = next;
    bridge->scroll_y = next;
    bridge->scroll_requested = true;
    return JS_NewInt32(context, next);
}

static bool fixed_box_offset(const LayoutDocument *layout,
                             const LayoutNodeBox *box, int viewport_height,
                             int *dy)
{
    if (layout == NULL || box == NULL || dy == NULL
        || viewport_height <= 0) return false;
    for (size_t at = layout->fixed_count; at > 0; at--) {
        const FixedRange *range = &layout->fixed_ranges[at - 1];
        if (box->command_start < range->command_start
            || box->command_end > range->command_end) continue;
        int target_y = range->from_bottom
                       ? viewport_height - range->inset - range->height
                       : range->inset;
        *dy = target_y - range->origin_y;
        return true;
    }
    return false;
}

static JSValue js_element_geometry(JSContext *context,
                                   JSValueConst this_value,
                                   int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = bridge == NULL || argc < 1 ? NULL
                           : js_rt_bridge_node_arg(context, bridge, argv[0]);
    if (bridge != NULL && bridge->result != NULL) {
        bridge->result->geometry_queries++;
    }
    if (bridge != NULL
        && (bridge->layout == NULL
            || (bridge->relayout_dirty != NULL
                && *bridge->relayout_dirty))) {
        if (js_rt_bridge_flush_synchronous_layout(bridge)
            && bridge->result != NULL) {
            bridge->result->geometry_synchronous_layouts++;
        }
    }
    const LayoutNodeBox *box = node == NULL || bridge->layout == NULL
                               ? NULL
                               : layout_box_for_node(bridge->layout, node);
#ifndef TILEFINCH_NO_TRACE
    if (getenv("TILEFINCH_TRACE_GEOMETRY") != NULL) {
        size_t name_length = 0, id_length = 0;
        const char *name = node == NULL ? NULL
            : document_element_name(node, &name_length);
        const char *id = node == NULL ? NULL
            : document_attribute(node, "id", &id_length);
        fprintf(stderr,
                "geometry-query node=%.*s id=%.*s retained=%d box=%d,%d,%dx%d\n",
                (int) name_length, name == NULL ? "" : name,
                (int) id_length, id == NULL ? "" : id,
                box != NULL, box == NULL ? 0 : box->x,
                box == NULL ? 0 : box->y,
                box == NULL ? 0 : box->width,
                box == NULL ? 0 : box->height);
    }
#endif
    JSValue result = JS_NewObject(context);
    if (JS_IsException(result)) return result;
    int x = box == NULL ? 0 : box->x;
    int y = box == NULL ? 0 : box->y;
    for (lxb_dom_node_t *parent = node == NULL ? NULL : node->parent;
         parent != NULL; parent = parent->parent) {
        if (bridge != NULL && bridge->result != NULL) {
            bridge->result->geometry_ancestor_visits++;
        }
        const LayoutNodeBox *ancestor = bridge->layout == NULL ? NULL
            : layout_box_for_node(bridge->layout, parent);
        if (ancestor != NULL) {
            x -= ancestor->scroll_x;
            y -= ancestor->scroll_y;
        }
    }
    int fixed_dy = 0;
    if (bridge != NULL
        && fixed_box_offset(bridge->layout, box, bridge->viewport_height,
                            &fixed_dy)) {
        y += fixed_dy;
    } else {
        y -= bridge == NULL ? 0 : bridge->page_scroll_y;
    }
#define SET_GEOMETRY(name, value) \
    do { \
        if (JS_SetPropertyStr(context, result, name, \
                              JS_NewInt32(context, value)) < 0) { \
            JS_FreeValue(context, result); \
            return JS_EXCEPTION; \
        } \
    } while (0)
    SET_GEOMETRY("x", x);
    SET_GEOMETRY("y", y);
    SET_GEOMETRY("retained", box != NULL);
    SET_GEOMETRY(
        "authoritative",
        box != NULL && box->cssom_geometry_authoritative);
    if (box != NULL && box->cssom_geometry_authoritative
        && bridge != NULL && bridge->result != NULL) {
        bridge->result->geometry_retained_fast_paths++;
    }
    size_t node_name_length = 0;
    const char *node_name = node == NULL ? NULL
                            : document_element_name(node, &node_name_length);
    bool document_root = node_name != NULL && node_name_length == 4
                         && strncasecmp(node_name, "html", 4) == 0;
    bool viewport_root = document_root
                         || (node_name != NULL && node_name_length == 4
                             && strncasecmp(node_name, "body", 4) == 0);
    int fallback_width = bridge == NULL || bridge->layout == NULL
                         ? (viewport_root && bridge != NULL
                            ? bridge->viewport_width : 300) : 0;
    int fallback_height = bridge == NULL || bridge->layout == NULL
                          ? (viewport_root && bridge != NULL
                             ? bridge->viewport_height : 65) : 0;
    ElementScrollIntent *intent = box == NULL
        ? bridge_scroll_intent_find(bridge, node) : NULL;
    SET_GEOMETRY("width", box == NULL ? fallback_width : box->width);
    SET_GEOMETRY("height", box == NULL ? fallback_height : box->height);
    /* CSSOM View defines the root element's client box as the layout
       viewport.  A retained layout box for <html> is an implementation
       detail and may legitimately have no content box of its own. */
    SET_GEOMETRY("clientWidth", document_root && bridge != NULL
                                ? bridge->viewport_width
                                : (box == NULL ? fallback_width
                                               : box->client_width));
    SET_GEOMETRY("clientHeight", document_root && bridge != NULL
                                 ? bridge->viewport_height
                                 : (box == NULL ? fallback_height
                                                : box->client_height));
    SET_GEOMETRY("scrollWidth", box == NULL ? fallback_width
                                             : box->content_width);
    SET_GEOMETRY("scrollHeight", box == NULL ? fallback_height
                                              : box->content_height);
    SET_GEOMETRY("scrollLeft", box == NULL
                               ? (intent == NULL ? 0 : intent->scroll_x)
                               : box->scroll_x);
    SET_GEOMETRY("scrollTop", box == NULL
                              ? (intent == NULL ? 0 : intent->scroll_y)
                              : box->scroll_y);
#undef SET_GEOMETRY
    return result;
}

static JSValue js_set_element_scroll(JSContext *context,
                                     JSValueConst this_value,
                                     int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = bridge == NULL || argc < 3 ? NULL
                           : js_rt_bridge_node_arg(context, bridge, argv[0]);
    int32_t x = 0, y = 0;
    if (node == NULL
        || JS_ToInt32(context, &x, argv[1]) < 0
        || JS_ToInt32(context, &y, argv[2]) < 0) {
        return JS_ThrowTypeError(context, "invalid element scroll");
    }
    ElementScrollIntent *intent =
        bridge_scroll_intent_set(bridge, node, x, y);
    bool applied = bridge->layout != NULL
        && layout_scroll_node(bridge->layout, node,
                              intent == NULL ? x : intent->scroll_x,
                              intent == NULL ? y : intent->scroll_y);
    return JS_NewBool(context, intent != NULL || applied);
}

static const char *frame_eval_value_type(JSContext *context,
                                         JSValueConst value)
{
    if (JS_IsUndefined(value)) return "undefined";
    if (JS_IsNull(value)) return "object";
    if (JS_IsBool(value)) return "boolean";
    if (JS_IsNumber(value)) return "number";
    if (JS_IsString(value)) return "string";
    if (JS_IsSymbol(value)) return "symbol";
    if (JS_IsFunction(context, value)) return "function";
    return "object";
}

static bool set_frame_eval_telemetry(JSContext *context, uint64_t count,
                                     int64_t source_length,
                                     const char *state, uint64_t elapsed_ms)
{
    char telemetry[128];
    if (elapsed_ms == UINT64_MAX) {
        snprintf(telemetry, sizeof(telemetry), "%llu:%lld:%s",
                 (unsigned long long) count, (long long) source_length, state);
    } else {
        snprintf(telemetry, sizeof(telemetry), "%llu:%lld:%s:%llums",
                 (unsigned long long) count, (long long) source_length, state,
                 (unsigned long long) elapsed_ms);
    }
    JSValue global = JS_GetGlobalObject(context);
    if (JS_IsException(global)) return false;
    int result = JS_SetPropertyStr(context, global,
                                   "__tilefinchFrameEvalTelemetry",
                                   JS_NewString(context, telemetry));
    JS_FreeValue(context, global);
    return result >= 0;
}

static JSValue js_frame_eval(JSContext *context, JSValueConst this_value,
                             int argc, JSValueConst *argv, int magic,
                             JSValue *function_data)
{
    (void) this_value; (void) magic;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || !bridge->dynamic_code_allowed) {
        return JS_ThrowTypeError(
            context,
            "dynamic code compilation disabled by Content Security Policy");
    }
    JSValue source = JS_ToString(context,
        argc > 0 ? argv[0] : JS_UNDEFINED);
    if (JS_IsException(source)) return source;

    JSValue length_value = JS_GetPropertyStr(context, source, "length");
    int64_t source_length = 0;
    if (JS_IsException(length_value)
        || JS_ToInt64(context, &source_length, length_value) < 0) {
        JS_FreeValue(context, length_value);
        JS_FreeValue(context, source);
        return JS_EXCEPTION;
    }
    JS_FreeValue(context, length_value);
    if (source_length > 64 * 1024) {
        JS_FreeValue(context, source);
        return JS_ThrowRangeError(context, "frame eval exceeds bounded size");
    }

    uint64_t count = bridge == NULL ? 0 : ++bridge->frame_eval_count;
    uint64_t started = monotonic_time_ms();
    if (!set_frame_eval_telemetry(context, count, source_length, "running",
                                  UINT64_MAX)) {
        JS_FreeValue(context, source);
        return JS_EXCEPTION;
    }
    JSValue arguments[2] = { function_data[1], source };
    JSValue value = JS_Call(context, function_data[0], JS_UNDEFINED,
                            2, arguments);
    JS_FreeValue(context, source);
    if (JS_IsException(value)) return value;
    if (!set_frame_eval_telemetry(context, count, source_length,
                                  frame_eval_value_type(context, value),
                                  monotonic_time_ms() - started)) {
        JS_FreeValue(context, value);
        return JS_EXCEPTION;
    }
    return value;
}

static JSValue js_create_frame_eval(JSContext *context,
                                    JSValueConst this_value,
                                    int argc, JSValueConst *argv)
{
    (void) this_value;
    if (argc < 2 || !JS_IsFunction(context, argv[0])
        || !JS_IsObject(argv[1])) {
        return JS_ThrowTypeError(context,
                                 "frame eval requires evaluator and scope");
    }
    JSValue function_data[2] = { argv[0], argv[1] };
    JSValue function = JS_NewCFunctionData(context, js_frame_eval, 1, 0,
                                           2, function_data);
    if (JS_IsException(function)) return function;
    if (JS_DefinePropertyValueStr(context, function, "name",
                                  JS_NewString(context, "eval"),
                                  JS_PROP_CONFIGURABLE) < 0) {
        JS_FreeValue(context, function);
        return JS_EXCEPTION;
    }
    return function;
}

static JSValue js_storage_get(JSContext *context, JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge->session == NULL || bridge->opaque_origin || argc < 2) {
        return JS_NULL;
    }
    bool local = JS_ToBool(context, argv[0]) > 0;
    const char *key = JS_ToCString(context, argv[1]);
    if (key == NULL) return JS_EXCEPTION;
    const char *value = NULL;
    size_t length = 0;
    bool found = browser_session_storage_get(
        bridge->session, bridge->document_url, local, key, &value, &length);
    JS_FreeCString(context, key);
    return found ? JS_NewStringLen(context, value, length) : JS_NULL;
}

static JSValue js_storage_available(JSContext *context,
                                    JSValueConst this_value,
                                    int argc, JSValueConst *argv)
{
    (void) this_value;
    (void) argc;
    (void) argv;
    DomBridge *bridge = JS_GetContextOpaque(context);
    return JS_NewBool(context, bridge != NULL && bridge->session != NULL
                               && !bridge->opaque_origin);
}

static JSValue js_storage_set(JSContext *context, JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (argc < 3) return JS_FALSE;
    if (bridge->session == NULL) return JS_TRUE;
    if (bridge->opaque_origin) return JS_FALSE;
    bool local = JS_ToBool(context, argv[0]) > 0;
    const char *key = JS_ToCString(context, argv[1]);
    size_t value_length = 0;
    const char *value = JS_ToCStringLen(context, &value_length, argv[2]);
    if (key == NULL || value == NULL) {
        if (key != NULL) JS_FreeCString(context, key);
        if (value != NULL) JS_FreeCString(context, value);
        return JS_EXCEPTION;
    }
    bool stored = browser_session_storage_set(
        bridge->session, bridge->document_url, local, key, value,
        value_length);
    JS_FreeCString(context, value);
    JS_FreeCString(context, key);
    return JS_NewBool(context, stored);
}

static JSValue js_storage_length(JSContext *context,
                                 JSValueConst this_value,
                                 int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge->session == NULL || bridge->opaque_origin || argc < 1) {
        return JS_NewInt32(context, 0);
    }
    size_t length = browser_session_storage_length(
        bridge->session, bridge->document_url,
        JS_ToBool(context, argv[0]) > 0);
    return JS_NewInt64(context, (int64_t) length);
}

static JSValue js_storage_key(JSContext *context, JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge->session == NULL || bridge->opaque_origin || argc < 2) {
        return JS_NULL;
    }
    int64_t index = -1;
    if (JS_ToInt64(context, &index, argv[1]) < 0) return JS_EXCEPTION;
    const char *key = NULL;
    bool found = index >= 0 && browser_session_storage_key(
        bridge->session, bridge->document_url,
        JS_ToBool(context, argv[0]) > 0, (size_t) index, &key);
    return found ? JS_NewString(context, key) : JS_NULL;
}

static JSValue js_storage_remove(JSContext *context,
                                 JSValueConst this_value,
                                 int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge->session == NULL || bridge->opaque_origin || argc < 2) {
        return JS_UNDEFINED;
    }
    bool local = JS_ToBool(context, argv[0]) > 0;
    const char *key = JS_ToCString(context, argv[1]);
    if (key == NULL) return JS_EXCEPTION;
    browser_session_storage_remove(bridge->session, bridge->document_url,
                                   local, key);
    JS_FreeCString(context, key);
    return JS_UNDEFINED;
}

static JSValue js_storage_clear(JSContext *context, JSValueConst this_value,
                                int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge->session != NULL && !bridge->opaque_origin && argc > 0) {
        browser_session_storage_clear(bridge->session, bridge->document_url,
                                      JS_ToBool(context, argv[0]) > 0);
    }
    return JS_UNDEFINED;
}

static JSValue js_cookie_get(JSContext *context, JSValueConst this_value,
                             int argc, JSValueConst *argv)
{
    (void) this_value; (void) argc; (void) argv;
    DomBridge *bridge = JS_GetContextOpaque(context);
    char cookies[4096];
    if (bridge->session == NULL || bridge->opaque_origin
        || !browser_session_cookie_get(bridge->session,
                                       bridge->document_url, cookies,
                                       sizeof(cookies))) {
        return JS_NewString(context, "");
    }
    return JS_NewString(context, cookies);
}

static JSValue js_cookie_set(JSContext *context, JSValueConst this_value,
                             int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge->session == NULL || bridge->opaque_origin || argc < 1) {
        return JS_FALSE;
    }
    const char *cookie = JS_ToCString(context, argv[0]);
    if (cookie == NULL) return JS_EXCEPTION;
    if (getenv("TILEFINCH_TRACE_COOKIE") != NULL) {
        const char *equals = strchr(cookie, '=');
        size_t name_length = equals == NULL ? 0 : (size_t) (equals - cookie);
        const char *attributes = strchr(cookie, ';');
        fprintf(stderr, "page-cookie-set name=%.*s value-bytes=%zu expires=%d max-age=%d path=%d domain=%d\n",
                (int) name_length, cookie,
                equals == NULL ? 0 : (attributes == NULL
                  ? strlen(equals + 1) : (size_t) (attributes - equals - 1)),
                attributes != NULL && (strstr(attributes, "expires=") != NULL
                                       || strstr(attributes, "Expires=") != NULL),
                attributes != NULL && (strstr(attributes, "max-age=") != NULL
                                       || strstr(attributes, "Max-Age=") != NULL),
                attributes != NULL && (strstr(attributes, "path=") != NULL
                                       || strstr(attributes, "Path=") != NULL),
                attributes != NULL && (strstr(attributes, "domain=") != NULL
                                       || strstr(attributes, "Domain=") != NULL));
    }
    TilefinchRequestContext request = {
        .target_url = bridge->document_url,
        .initiator_url = bridge->document_url,
        .top_level_url = bridge->top_level_url,
        .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_SAME_ORIGIN,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_OTHER
    };
    bool stored = browser_session_cookie_set_context(
        bridge->session, &request, cookie);
    JS_FreeCString(context, cookie);
    return JS_NewBool(context, stored);
}

static JSValue js_console_log(JSContext *context, JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || getenv("TILEFINCH_TRACE_CONSOLE") == NULL
        || bridge->console_messages >= 64) return JS_UNDEFINED;
    bridge->console_messages++;
    const char *level = argc > 0 ? JS_ToCString(context, argv[0]) : NULL;
    fprintf(stderr, "page-console[%s]", level == NULL ? "log" : level);
    if (level != NULL) JS_FreeCString(context, level);
    for (int i = 1; i < argc; i++) {
        const char *value = JS_ToCString(context, argv[i]);
        fprintf(stderr, " %s", value == NULL ? "<unprintable>" : value);
        if (value != NULL) JS_FreeCString(context, value);
        if (JS_IsObject(argv[i])) {
            JSValue stack = JS_GetPropertyStr(context, argv[i], "stack");
            if (JS_IsString(stack)) {
                const char *trace = JS_ToCString(context, stack);
                if (trace != NULL && trace[0] != '\0') {
                    fprintf(stderr, "\n%s", trace);
                    JS_FreeCString(context, trace);
                }
            }
            JS_FreeValue(context, stack);
        }
    }
    fputc('\n', stderr);
    return JS_UNDEFINED;
}

static JSValue js_resolve_url(JSContext *context, JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || argc < 1) return JS_NULL;
    const char *reference = JS_ToCString(context, argv[0]);
    if (reference == NULL) return JS_EXCEPTION;
    const char *base = bridge->document_url;
    const char *base_argument = NULL;
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        base_argument = JS_ToCString(context, argv[1]);
        if (base_argument == NULL) {
            JS_FreeCString(context, reference);
            return JS_EXCEPTION;
        }
        base = base_argument;
    }
    const char *effective_base = base != NULL && strstr(base, "://") != NULL
                                 ? base : "https://fixture.invalid/";
    char resolved[4096];
    bool ok = tilefinch_url_resolve(effective_base, reference, resolved,
                                 sizeof(resolved));
    if (base_argument != NULL) JS_FreeCString(context, base_argument);
    JS_FreeCString(context, reference);
    return ok ? JS_NewString(context, resolved) : JS_NULL;
}

const char *js_rt_bridge_calculated_base_url(DomBridge *bridge)
{
    if (bridge == NULL || bridge->document == NULL
        || bridge->document_url == NULL) return NULL;
    if (bridge->document_base_dirty) {
        if (!document_base_url(
                bridge->document, bridge->document_url,
                bridge->calculated_base_url,
                sizeof(bridge->calculated_base_url))) return NULL;
        bridge->document_base_dirty = false;
    }
    return bridge->calculated_base_url;
}

static bool bridge_set_document_url(DomBridge *bridge, const char *url)
{
    if (bridge == NULL || bridge->document_url == NULL || url == NULL) {
        return false;
    }
    char normalized[TILEFINCH_URL_SERIALIZED_LIMIT];
    if (!tilefinch_url_normalize(url, normalized, sizeof(normalized))
        || !tilefinch_url_same_origin(bridge->document_url, normalized)) {
        return false;
    }
    /* Observe the first connected <base href> against the old fallback URL
       before changing location. An unchanged authoritative element keeps its
       frozen URL; a document with no base will recalculate from the new URL
       after the dirty bit below is set. */
    if (js_rt_bridge_calculated_base_url(bridge) == NULL) return false;
    memcpy(bridge->document_url, normalized, strlen(normalized) + 1);
    bridge->document_base_dirty = true;
    return true;
}

static JSValue js_document_base_uri(JSContext *context,
                                    JSValueConst this_value,
                                    int argc, JSValueConst *argv)
{
    (void) this_value; (void) argc; (void) argv;
    DomBridge *bridge = JS_GetContextOpaque(context);
    const char *base = js_rt_bridge_calculated_base_url(bridge);
    return base == NULL ? JS_ThrowInternalError(
                              context, "could not calculate document base URL")
                        : JS_NewString(context, base);
}

static JSValue js_set_document_url(JSContext *context,
                                   JSValueConst this_value,
                                   int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || argc < 1) return JS_FALSE;
    const char *url = JS_ToCString(context, argv[0]);
    if (url == NULL) return JS_EXCEPTION;
    bool updated = bridge_set_document_url(bridge, url);
    JS_FreeCString(context, url);
    return JS_NewBool(context, updated);
}

static bool fill_secure_random(uint8_t *data, size_t length)
{
    return tilefinch_platform_secure_random(data, length);
}

static JSValue js_crypto_random_fill(JSContext *context,
                                     JSValueConst this_value,
                                     int argc, JSValueConst *argv)
{
    (void) this_value;
    if (argc < 1) {
        return JS_ThrowTypeError(context, "getRandomValues requires a typed array");
    }
    size_t offset = 0, length = 0, bytes_per_element = 0;
    JSValue buffer = JS_GetTypedArrayBuffer(context, argv[0], &offset,
                                            &length, &bytes_per_element);
    if (JS_IsException(buffer)) return buffer;
    size_t buffer_length = 0;
    uint8_t *bytes = JS_GetArrayBuffer(context, &buffer_length, buffer);
    if (bytes == NULL || offset > buffer_length
        || length > buffer_length - offset || length > 65536) {
        JS_FreeValue(context, buffer);
        return JS_ThrowRangeError(context, "random request exceeds quota");
    }
    DomBridge *bridge = JS_GetContextOpaque(context);
    bool filled = true;
    if (bridge != NULL && bridge->deterministic_entropy) {
        size_t written = 0;
        while (written < length) {
            uint64_t random = replay_random_next(&bridge->entropy_state);
            size_t chunk = length - written < sizeof(random)
                ? length - written : sizeof(random);
            /* One stream word is consumed per call chunk. Bytes are always
               emitted least-significant first, including on big-endian PSP
               toolchains, and an unused partial-word suffix is discarded. */
            for (size_t index = 0; index < chunk; index++) {
                bytes[offset + written + index] =
                    (uint8_t) (random >> (index * 8u));
            }
            written += chunk;
        }
    } else {
        filled = fill_secure_random(bytes + offset, length);
    }
    JS_FreeValue(context, buffer);
    if (!filled) {
        return JS_ThrowInternalError(context,
                                     "secure random source unavailable");
    }
    return JS_DupValue(context, argv[0]);
}

static JSValue js_crypto_digest_sha256(JSContext *context,
                                       JSValueConst this_value,
                                       int argc, JSValueConst *argv)
{
    (void) this_value;
    if (argc < 1) {
        return JS_ThrowTypeError(context, "SHA-256 digest requires a byte view");
    }
    size_t offset = 0, length = 0, bytes_per_element = 0;
    JSValue buffer = JS_GetTypedArrayBuffer(context, argv[0], &offset,
                                            &length, &bytes_per_element);
    if (JS_IsException(buffer)) return buffer;
    size_t buffer_length = 0;
    const uint8_t *bytes = JS_GetArrayBuffer(context, &buffer_length, buffer);
    bool valid = (bytes != NULL || buffer_length == 0)
        && offset <= buffer_length && length <= buffer_length - offset;
    if (!valid) {
        JS_FreeValue(context, buffer);
        return JS_ThrowTypeError(context, "invalid digest byte view");
    }
    if (length > SCRIPT_CRYPTO_DIGEST_INPUT_LIMIT) {
        JS_FreeValue(context, buffer);
        return JS_ThrowRangeError(context,
                                  "digest input exceeds bounded size");
    }
    uint8_t digest[TILEFINCH_SHA256_DIGEST_BYTES];
    const uint8_t *input = length == 0 ? NULL : bytes + offset;
    bool digested = tilefinch_sha256_digest(input, length, digest);
    JS_FreeValue(context, buffer);
    if (!digested) {
        return JS_ThrowInternalError(context, "SHA-256 digest failed");
    }
    return JS_NewArrayBufferCopy(context, digest, sizeof(digest));
}

static JSValue js_base64_encode_bytes(JSContext *context,
                                      JSValueConst this_value,
                                      int argc, JSValueConst *argv)
{
    (void) this_value;
    if (argc < 1) return JS_ThrowTypeError(context, "byte array required");
    size_t offset = 0, length = 0, bytes_per_element = 0;
    JSValue buffer = JS_GetTypedArrayBuffer(context, argv[0], &offset,
                                            &length, &bytes_per_element);
    if (JS_IsException(buffer)) return buffer;
    size_t buffer_length = 0;
    uint8_t *bytes = JS_GetArrayBuffer(context, &buffer_length, buffer);
    if (bytes == NULL || bytes_per_element != 1 || offset > buffer_length
        || length > buffer_length - offset || length > 256u * 1024u) {
        JS_FreeValue(context, buffer);
        return JS_ThrowRangeError(context, "base64 input exceeds bounded size");
    }
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t output_length = ((length + 2u) / 3u) * 4u;
    char *output = js_malloc(context, output_length + 1u);
    if (output == NULL) {
        JS_FreeValue(context, buffer);
        return JS_EXCEPTION;
    }
    size_t source = offset, end = offset + length, destination = 0;
    while (source < end) {
        uint32_t a = bytes[source++];
        bool has_b = source < end;
        uint32_t b = has_b ? bytes[source++] : 0;
        bool has_c = source < end;
        uint32_t c = has_c ? bytes[source++] : 0;
        uint32_t value = (a << 16) | (b << 8) | c;
        output[destination++] = alphabet[(value >> 18) & 63u];
        output[destination++] = alphabet[(value >> 12) & 63u];
        output[destination++] = has_b ? alphabet[(value >> 6) & 63u] : '=';
        output[destination++] = has_c ? alphabet[value & 63u] : '=';
    }
    output[output_length] = '\0';
    JSValue result = JS_NewStringLen(context, output, output_length);
    js_free(context, output);
    JS_FreeValue(context, buffer);
    return result;
}

static int base64_digit(unsigned char value)
{
    if (value >= 'A' && value <= 'Z') return value - 'A';
    if (value >= 'a' && value <= 'z') return value - 'a' + 26;
    if (value >= '0' && value <= '9') return value - '0' + 52;
    if (value == '+') return 62;
    if (value == '/') return 63;
    return -1;
}

static JSValue new_latin1_string(JSContext *context, const uint8_t *input,
                                 size_t length)
{
    if (length > SIZE_MAX / 2u) return JS_EXCEPTION;
    size_t capacity = length * 2u;
    char *utf8 = js_malloc(context, capacity == 0 ? 1 : capacity);
    if (utf8 == NULL) return JS_EXCEPTION;
    size_t output = 0;
    for (size_t index = 0; index < length; index++) {
        uint8_t value = input[index];
        if (value < 0x80u) {
            utf8[output++] = (char) value;
        } else {
            utf8[output++] = (char) (0xc0u | (value >> 6));
            utf8[output++] = (char) (0x80u | (value & 0x3fu));
        }
    }
    JSValue result = JS_NewStringLen(context, utf8, output);
    js_free(context, utf8);
    return result;
}

static JSValue js_base64_decode_string(JSContext *context,
                                       JSValueConst this_value,
                                       int argc, JSValueConst *argv)
{
    (void) this_value;
    if (argc < 1) return JS_ThrowTypeError(context, "base64 string required");
    size_t length = 0;
    const char *input = JS_ToCStringLen(context, &length, argv[0]);
    if (input == NULL) return JS_EXCEPTION;
    if (length > 768u * 1024u || length % 4u == 1u) {
        JS_FreeCString(context, input);
        return JS_ThrowRangeError(context,
                                  "base64 input exceeds bounded size");
    }
    size_t output_capacity = (length / 4u) * 3u
                             + (length % 4u == 0 ? 0u
                                : length % 4u - 1u);
    uint8_t *output = js_malloc(context, output_capacity == 0
                                         ? 1 : output_capacity);
    if (output == NULL) {
        JS_FreeCString(context, input);
        return JS_EXCEPTION;
    }
    size_t source = 0, destination = 0;
    bool valid = true;
    while (source < length) {
        int a = base64_digit((unsigned char) input[source++]);
        int b = source < length
              ? base64_digit((unsigned char) input[source++]) : -1;
        bool has_c = source < length;
        int c = has_c ? base64_digit((unsigned char) input[source++]) : -1;
        bool has_d = source < length;
        int d = has_d ? base64_digit((unsigned char) input[source++]) : -1;
        if (a < 0 || b < 0 || (has_c && c < 0) || (has_d && d < 0)) {
            valid = false;
            break;
        }
        uint32_t packed = ((uint32_t) a << 18)
                        | ((uint32_t) b << 12)
                        | ((uint32_t) (c < 0 ? 0 : c) << 6)
                        | (uint32_t) (d < 0 ? 0 : d);
        output[destination++] = (uint8_t) (packed >> 16);
        if (c >= 0) output[destination++] = (uint8_t) (packed >> 8);
        if (d >= 0) output[destination++] = (uint8_t) packed;
    }
    JSValue result = valid
        ? new_latin1_string(context, output, destination)
        : JS_ThrowTypeError(context, "invalid base64 input");
    js_free(context, output);
    JS_FreeCString(context, input);
    return result;
}

static JSValue js_replay_math_random(JSContext *context,
                                     JSValueConst this_value,
                                     int argc, JSValueConst *argv)
{
    (void) this_value; (void) argc; (void) argv;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || !bridge->deterministic_entropy)
        return JS_ThrowInternalError(context, "deterministic entropy disabled");
    uint64_t random = replay_random_next(&bridge->entropy_state);
    /* IEEE-754 binary64 can represent every 53-bit integer exactly. The
       contract discards the low 11 bits, then scales the upper 53 by 2^-53,
       producing a stable value in [0, 1) without host-endian dependence. */
    return JS_NewFloat64(context, (double) (random >> 11) *
                         (1.0 / 9007199254740992.0));
}

/*
 * Asynchronous page callbacks can be very short in QuickJS bytecode while
 * spending most of their time in bounded native DOM helpers.  Such a callback
 * may finish before the VM's ordinary interrupt-counter cadence reaches the
 * embedder.  The bootstrap calls this at the single task-entry boundary so
 * cancellation and the UI cooperate hook are observed before author code,
 * without adding a poll to every ordinary JavaScript function call.
 */
static JSValue js_callback_checkpoint(JSContext *context,
                                      JSValueConst this_value,
                                      int argc, JSValueConst *argv)
{
    (void) this_value;
    (void) argc;
    (void) argv;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || bridge->host == NULL)
        return JS_ThrowInternalError(context, "callback runtime unavailable");
    if (js_rt_runtime_callback_checkpoint(bridge->host))
        return JS_UNDEFINED;
    JSValue exception = JS_ThrowInternalError(
        context, "callback interrupted at task boundary");
    JS_SetUncatchableException(context, true);
    return exception;
}

bool js_rt_install_function(JSContext *context, JSValue global,
                      const char *name, JSCFunction *function,
                      int arguments)
{
    JSValue value = JS_NewCFunction(context, function, name, arguments);
    if (JS_IsException(value)) return false;
    DomBridge *bridge = JS_GetContextOpaque(context);
    bool test_network_override =
        bridge != NULL
        && bridge->allow_test_network_primitive_overrides
        && (strcmp(name, "__tilefinchFetchSync") == 0
            || strcmp(name, "__tilefinchFetchAsync") == 0
            || strcmp(name, "__tilefinchCancelNetwork") == 0);
    int flags = test_network_override
        ? JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE : 0;
    return JS_DefinePropertyValueStr(
               context, global, name, value, flags) >= 0;
}
