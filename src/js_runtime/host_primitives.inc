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
    /* High Resolution Time deliberately coarsens non-isolated realms to a
       100us clock.  Tilefinch does not expose cross-origin isolation, so
       publishing the host/PSP nanosecond clock here both fingerprints the
       device and gives page code a stronger timing side channel than a
       normal browser.  Do the reduction in integer space so repeated reads
       stay monotonic and never round up beyond the sampled host time.  The
       deterministic replay clock is an explicit test/capture contract and
       retains its one-millisecond call quantum. */
    static const uint64_t performance_quantum_ns = UINT64_C(100000);
    bool deterministic = bridge != NULL && bridge->deterministic_clock;
    uint64_t current = deterministic
        ? bridge->clock_monotonic_elapsed_ms : js_rt_monotonic_time_ns();
    double elapsed = 0.0;
    if (deterministic) elapsed = (double) current;
    else if (bridge != NULL && current >= bridge->performance_origin_ns) {
        uint64_t elapsed_ns = current - bridge->performance_origin_ns;
        elapsed = (double) (elapsed_ns / performance_quantum_ns) / 10.0;
    }
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
#if !defined(TILEFINCH_NO_TRACE)
    if (getenv("TILEFINCH_TRACE_NAVIGATION_REQUESTS") != NULL) {
        fprintf(stderr,
                "tilefinch-script-navigation: replace=%d url=\"%s\"\n",
                bridge->navigation_replace ? 1 : 0,
                bridge->navigation_url);
    }
#endif
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
        .node_handle = handle,
        .audio_only = node->local_name == LXB_TAG_AUDIO,
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE
    };
    size_t crossorigin_length = 0;
    const char *crossorigin = document_attribute(
        node, "crossorigin", &crossorigin_length);
    if (crossorigin != NULL) {
        request.mode = TILEFINCH_REQUEST_MODE_CORS;
        request.credentials =
            crossorigin_length == sizeof("use-credentials") - 1u
            && strncasecmp(
                   crossorigin, "use-credentials",
                   sizeof("use-credentials") - 1u) == 0
                ? TILEFINCH_CREDENTIALS_INCLUDE
                : TILEFINCH_CREDENTIALS_SAME_ORIGIN;
    }
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
    bridge->media_audio_only = request.audio_only;
    bridge->media_node_handle = request.node_handle;
    bridge->media_value = request.value;
    bridge->media_mode = request.mode;
    bridge->media_credentials = request.credentials;
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
                             int scroll_y, bool *matched, int *dy)
{
    if (matched != NULL) *matched = false;
    if (layout == NULL || box == NULL || dy == NULL
        || viewport_height <= 0) return false;
    const FixedRange *owner = NULL;
    size_t owner_index = SIZE_MAX;
    size_t owner_span = SIZE_MAX;
    for (size_t at = 0; at < layout->fixed_count; at++) {
        const FixedRange *range = &layout->fixed_ranges[at];
        if (box->command_start < range->command_start
            || box->command_end > range->command_end) continue;
        size_t span = range->command_end >= range->command_start
            ? range->command_end - range->command_start : SIZE_MAX;
        if (owner == NULL || span < owner_span
            || (span == owner_span && at < owner_index)) {
            owner = range;
            owner_index = at;
            owner_span = span;
        }
    }
    if (owner == NULL) return false;
    if (matched != NULL) *matched = true;
    if (owner->scroll_end != INT_MAX
        && scroll_y >= owner->scroll_end) return false;
    int target_y = owner->from_bottom
                   ? viewport_height - owner->inset - owner->height
                   : owner->inset;
    *dy = target_y - owner->origin_y;
    return true;
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
    bool fixed_geometry = false;
    if (bridge != NULL
        && fixed_box_offset(bridge->layout, box, bridge->viewport_height,
                            bridge->page_scroll_y, &fixed_geometry,
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
        box != NULL
            && (box->cssom_geometry_authoritative || fixed_geometry));
    if (box != NULL
        && (box->cssom_geometry_authoritative || fixed_geometry)
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

static bool frame_realm_prepare(JSContext *caller, ScriptRuntime *host,
                                size_t slot)
{
    if (host->frame_realms[slot].intrinsic_state == 2) return true;
    JSContext *realm = host->frame_realms[slot].context;
    if (realm == NULL || host->frame_realms[slot].intrinsic_state != 0) {
        JS_ThrowTypeError(caller, "frame realm unavailable");
        return false;
    }
    host->frame_realms[slot].intrinsic_state = 1;
#if defined(PSP_BROWSER_BELLARD_QUICKJS)
    /* Same order as JS_NewContext, on the original raw global object. These
       native initializers do not execute author code. A partial failure is
       terminal: retrying would replace prototypes under retained values. */
    if (JS_AddIntrinsicDate(realm) || JS_AddIntrinsicStringNormalize(realm)
        || JS_AddIntrinsicRegExp(realm) || JS_AddIntrinsicJSON(realm)
        || JS_AddIntrinsicProxy(realm) || JS_AddIntrinsicMapSet(realm)
        || JS_AddIntrinsicTypedArrays(realm) || JS_AddIntrinsicPromise(realm)
        || JS_AddIntrinsicWeakRef(realm)) {
        host->frame_realms[slot].intrinsic_state = 3;
        JS_RetireContext(realm);
        JS_Throw(caller, JS_GetException(realm));
        return false;
    }
#endif
    host->frame_realms[slot].intrinsic_state = 2;
    return true;
}

static JSValue js_frame_scope(JSContext *context, JSValueConst this_value,
                             int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || bridge->host == NULL)
        return JS_ThrowInternalError(context, "frame runtime unavailable");
    ScriptRuntime *host = bridge->host;
    for (size_t i = 0; i < SCRIPT_FRAME_REALM_LIMIT; i++) {
        if (argc != 0) {
            if (!JS_IsObject(argv[0]) || !host->frame_realms[i].context
                || JS_VALUE_GET_PTR(argv[0]) !=
                   JS_VALUE_GET_PTR(host->frame_realms[i].global)) continue;
            if (argc > 1 && JS_ToBool(context, argv[1]))
                return frame_realm_prepare(context, host, i)
                    ? JS_UNDEFINED : JS_EXCEPTION;
#if defined(TILEFINCH_QUICKJS_DYNAMIC_CODE_POLICY)
            JS_RetireContext(host->frame_realms[i].context);
#endif
            JS_FreeValue(host->frame_realms[i].context,
                         host->frame_realms[i].global);
            JS_FreeContext(host->frame_realms[i].context);
            host->frame_realms[i].context = NULL;
            host->frame_realms[i].global = JS_UNDEFINED;
            host->frame_realms[i].intrinsic_state = 0;
            return JS_UNDEFINED;
        }
        if (host->frame_realms[i].context != NULL) continue;
        JSContext *realm =
#if defined(PSP_BROWSER_BELLARD_QUICKJS)
            JS_NewContextRaw(host->runtime);
#else
            JS_NewContext(host->runtime);
#endif
        if (realm == NULL) return JS_ThrowOutOfMemory(context);
#if defined(PSP_BROWSER_BELLARD_QUICKJS)
        /* BaseObjects defines eval/globalThis and autoinit properties that
           cannot be installed over the Window bindings. Install this core
           before exposing the global; defer the remaining intrinsic groups. */
        if (JS_AddIntrinsicBaseObjects(realm) || JS_AddIntrinsicEval(realm)) {
            JSValue exception = JS_GetException(realm);
            JS_FreeContext(realm);
            return JS_Throw(context, exception);
        }
#endif
        JS_SetContextOpaque(realm, bridge);
#if defined(TILEFINCH_QUICKJS_DYNAMIC_CODE_POLICY)
        JS_SetDynamicCodeEnabled(realm, bridge->dynamic_code_allowed);
#endif
        JSValue global = JS_GetGlobalObject(realm);
        host->frame_realms[i].context = realm;
        host->frame_realms[i].global = JS_DupValue(realm, global);
#if !defined(PSP_BROWSER_BELLARD_QUICKJS)
        host->frame_realms[i].intrinsic_state = 2;
#endif
        return global;
    }
    if (argc > 1 && JS_ToBool(context, argv[1]))
        return JS_ThrowTypeError(context, "frame realm is retired");
    return argc != 0 ? JS_UNDEFINED
        : JS_ThrowRangeError(context, "frame realm quota exceeded");
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
    JSContext *realm = NULL;
    for (size_t i = 0; i < SCRIPT_FRAME_REALM_LIMIT; i++) {
        if (bridge->host->frame_realms[i].context != NULL
            && JS_VALUE_GET_PTR(bridge->host->frame_realms[i].global)
                   == JS_VALUE_GET_PTR(function_data[0])) {
            if (!frame_realm_prepare(context, bridge->host, i)) {
                JS_FreeValue(context, source);
                return JS_EXCEPTION;
            }
            realm = bridge->host->frame_realms[i].context;
            break;
        }
    }
    size_t bytes = 0;
    const char *text = JS_ToCStringLen(context, &bytes, source);
    JSValue value;
    if (text == NULL) value = JS_EXCEPTION;
    else if (realm == NULL)
        value = JS_ThrowTypeError(context, "frame realm is retired");
    else {
        /* A genuine persistent global environment retains live var/lexical
           bindings and closures. The WindowProxy remains top-level this. */
        value = JS_EvalThis(realm, function_data[1], text, bytes,
                            "<frame-eval>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(value))
            value = JS_Throw(context, JS_GetException(realm));
    }
    if (text != NULL) JS_FreeCString(context, text);
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
    if (argc < 2 || !JS_IsObject(argv[0])
        || !JS_IsObject(argv[1])) {
        return JS_ThrowTypeError(context,
                                 "frame eval requires evaluator and scope");
    }
#if defined(TILEFINCH_QUICKJS_DYNAMIC_CODE_POLICY)
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || bridge->host == NULL)
        return JS_ThrowInternalError(context, "frame runtime unavailable");
    for (size_t i = 0; i < SCRIPT_FRAME_REALM_LIMIT; i++) {
        if (bridge->host->frame_realms[i].context != NULL
            && JS_VALUE_GET_PTR(bridge->host->frame_realms[i].global)
                   == JS_VALUE_GET_PTR(argv[0])) {
            JS_SetGlobalThis(bridge->host->frame_realms[i].context, argv[1]);
            break;
        }
    }
#endif
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

static JSValue js_storage_estimate(JSContext *context,
                                   JSValueConst this_value,
                                   int argc, JSValueConst *argv)
{
    (void) this_value;
    (void) argc;
    (void) argv;
    DomBridge *bridge = JS_GetContextOpaque(context);
    BrowserSiteDataUsage usage = {0};
    if (bridge == NULL || bridge->session == NULL || bridge->opaque_origin
        || !browser_session_site_data_usage(
               bridge->session, bridge->document_url, &usage)) {
        return JS_NULL;
    }
    size_t quota = bridge->session->maximum_storage_bytes;
    if (quota < usage.storage_bytes) quota = usage.storage_bytes;
    JSValue values = JS_NewArray(context);
    if (JS_IsException(values)) return values;
    if (JS_SetPropertyUint32(
            context, values, 0,
            JS_NewInt64(context, (int64_t) usage.storage_bytes)) < 0
        || JS_SetPropertyUint32(
               context, values, 1,
               JS_NewInt64(context, (int64_t) quota)) < 0) {
        JS_FreeValue(context, values);
        return JS_EXCEPTION;
    }
    return values;
}

static JSValue js_heap_memory_snapshot(JSContext *context,
                                       JSValueConst this_value,
                                       int argc, JSValueConst *argv)
{
    (void) this_value;
    (void) argc;
    (void) argv;
    DomBridge *bridge = JS_GetContextOpaque(context);
    ScriptRuntime *runtime = bridge == NULL ? NULL : bridge->host;
    if (runtime == NULL || runtime->quickjs_pool == NULL) return JS_NULL;
    size_t limit = runtime->base_memory_limit;
    if (runtime->boot_window_active) {
        limit = runtime->boot_window_bytes > SIZE_MAX - limit
            ? SIZE_MAX : limit + runtime->boot_window_bytes;
    }
    size_t used = budget_quickjs_pool_js_malloc_current(
        runtime->quickjs_pool);
    size_t total = budget_quickjs_pool_reserved_current(
        runtime->quickjs_pool);
    if (total < used) total = used;
    if (limit < total) limit = total;
    JSValue values = JS_NewArray(context);
    if (JS_IsException(values)) return values;
    if (JS_SetPropertyUint32(
            context, values, 0,
            JS_NewInt64(context, (int64_t) limit)) < 0
        || JS_SetPropertyUint32(
               context, values, 1,
               JS_NewInt64(context, (int64_t) total)) < 0
        || JS_SetPropertyUint32(
               context, values, 2,
               JS_NewInt64(context, (int64_t) used)) < 0) {
        JS_FreeValue(context, values);
        return JS_EXCEPTION;
    }
    return values;
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
    bridge->document_url_revision++;
    if (bridge->document_url_revision == 0u)
        bridge->document_url_revision = 1u;
    return true;
}

static JSValue js_current_document_url(JSContext *context,
                                       JSValueConst this_value,
                                       int argc, JSValueConst *argv)
{
    (void) this_value; (void) argc; (void) argv;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || bridge->document_url == NULL) {
        return JS_ThrowInternalError(context,
                                     "document URL is unavailable");
    }
    return JS_NewString(context, bridge->document_url);
}

static JSValue js_secure_context(JSContext *context,
                                 JSValueConst this_value,
                                 int argc, JSValueConst *argv)
{
    (void) this_value; (void) argc; (void) argv;
    DomBridge *bridge = JS_GetContextOpaque(context);
    /* Bind the answer to the native document identity.  Page script can
       replace or poison Location accessors, but it cannot promote an
       untrustworthy transport into a secure context. */
    return JS_NewBool(context,
                      bridge != NULL && bridge->document_url != NULL
                      && tilefinch_url_potentially_trustworthy(
                             bridge->document_url));
}

static JSValue js_document_url_revision(JSContext *context,
                                        JSValueConst this_value,
                                        int argc, JSValueConst *argv)
{
    (void) this_value; (void) argc; (void) argv;
    DomBridge *bridge = JS_GetContextOpaque(context);
    return bridge == NULL
        ? JS_ThrowInternalError(context, "document URL is unavailable")
        : JS_NewUint32(context, bridge->document_url_revision);
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

#define SCRIPT_COMPRESSION_INPUT_LIMIT (1024u * 1024u)
#define SCRIPT_COMPRESSION_OUTPUT_LIMIT \
    (SCRIPT_COMPRESSION_INPUT_LIMIT + 64u * 1024u)

static voidpf js_compression_allocate(voidpf opaque, uInt items, uInt size)
{
    Budget *budget = opaque;
    if (budget == NULL || (size != 0u && items > SIZE_MAX / size)) return NULL;
    return budget_calloc_category(
        budget, BUDGET_CATEGORY_JAVASCRIPT, items, size);
}

static void js_compression_free(voidpf opaque, voidpf address)
{
    if (opaque != NULL) budget_free(opaque, address);
}

static bool js_compression_buffer_source(
    JSContext *context, JSValueConst value, const uint8_t **bytes,
    size_t *length, JSValue *owner)
{
    *owner = JS_UNDEFINED;
    *length = 0;
    *bytes = JS_GetArrayBuffer(context, length, value);
    if (*bytes != NULL) return true;
    JSValue exception = JS_GetException(context);
    JS_FreeValue(context, exception);
    size_t offset = 0, view_length = 0, element_bytes = 0;
    JSValue buffer = JS_GetTypedArrayBuffer(
        context, value, &offset, &view_length, &element_bytes);
    (void) element_bytes;
    if (JS_IsException(buffer)) return false;
    size_t buffer_length = 0;
    uint8_t *buffer_bytes = JS_GetArrayBuffer(
        context, &buffer_length, buffer);
    if ((buffer_bytes == NULL && buffer_length != 0)
        || offset > buffer_length || view_length > buffer_length - offset) {
        JS_FreeValue(context, buffer);
        return false;
    }
    *bytes = buffer_bytes == NULL ? NULL : buffer_bytes + offset;
    *length = view_length;
    *owner = buffer;
    return true;
}

static int js_compression_window_bits(const char *format, size_t length)
{
    if (length == 7 && memcmp(format, "deflate", 7) == 0)
        return MAX_WBITS;
    if (length == 11 && memcmp(format, "deflate-raw", 11) == 0)
        return -MAX_WBITS;
    if (length == 4 && memcmp(format, "gzip", 4) == 0)
        return MAX_WBITS + 16;
    return 0;
}

/* CompressionStream is installed lazily by streams.js.  This native seam is
   deliberately one-shot: the JavaScript facade owns a bounded list of input
   chunks and calls here only from flush().  That keeps zlib state out of the
   page lifecycle while replacing multi-million-opcode JavaScript codec
   fallbacks with a charged, bounded native operation. */
static JSValue js_compression_run(JSContext *context,
                                  JSValueConst this_value,
                                  int argc, JSValueConst *argv)
{
    (void) this_value;
    int32_t decompress = 0;
    if (argc < 3 || JS_ToInt32(context, &decompress, argv[0]) < 0)
        return JS_ThrowTypeError(context, "compression operation is required");
    size_t format_length = 0;
    const char *format = JS_ToCStringLen(
        context, &format_length, argv[1]);
    if (format == NULL) return JS_EXCEPTION;
    int window_bits = js_compression_window_bits(format, format_length);
    JS_FreeCString(context, format);
    if (window_bits == 0 || (decompress != 0 && decompress != 1))
        return JS_ThrowTypeError(context, "unsupported compression format");

    const uint8_t *input = NULL;
    size_t input_length = 0;
    JSValue input_owner = JS_UNDEFINED;
    if (!js_compression_buffer_source(
            context, argv[2], &input, &input_length, &input_owner)) {
        return JS_ThrowTypeError(context,
                                 "compression input must be a BufferSource");
    }
    if (input_length > SCRIPT_COMPRESSION_INPUT_LIMIT) {
        JS_FreeValue(context, input_owner);
        return JS_ThrowRangeError(context,
                                  "compression input exceeds bounded size");
    }
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || bridge->budget == NULL) {
        JS_FreeValue(context, input_owner);
        return JS_ThrowInternalError(context,
                                     "compression runtime is detached");
    }

    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    stream.zalloc = js_compression_allocate;
    stream.zfree = js_compression_free;
    stream.opaque = bridge->budget;
    int status = decompress
        ? inflateInit2(&stream, window_bits)
        : deflateInit2(&stream, Z_BEST_SPEED, Z_DEFLATED, window_bits,
                       MAX_MEM_LEVEL, Z_DEFAULT_STRATEGY);
    if (status != Z_OK) {
        JS_FreeValue(context, input_owner);
        return status == Z_MEM_ERROR ? JS_ThrowOutOfMemory(context)
                                     : JS_ThrowInternalError(
                                           context,
                                           "could not initialize compression");
    }

    size_t capacity = 0;
    if (decompress) {
        capacity = input_length > 16u * 1024u
            ? input_length : 16u * 1024u;
        if (capacity > SCRIPT_COMPRESSION_OUTPUT_LIMIT)
            capacity = SCRIPT_COMPRESSION_OUTPUT_LIMIT;
    } else {
        uLong bound = deflateBound(&stream, (uLong) input_length);
        capacity = (size_t) bound;
        if (capacity < 64u) capacity = 64u;
    }
    uint8_t *output = capacity <= SCRIPT_COMPRESSION_OUTPUT_LIMIT
        ? budget_malloc_category(bridge->budget,
                                 BUDGET_CATEGORY_JAVASCRIPT, capacity)
        : NULL;
    if (output == NULL) {
        if (decompress) (void) inflateEnd(&stream);
        else (void) deflateEnd(&stream);
        JS_FreeValue(context, input_owner);
        return capacity > SCRIPT_COMPRESSION_OUTPUT_LIMIT
            ? JS_ThrowRangeError(context,
                                 "compressed output exceeds bounded size")
            : JS_ThrowOutOfMemory(context);
    }

    stream.next_in = (Bytef *) input;
    stream.avail_in = (uInt) input_length;
    stream.next_out = output;
    stream.avail_out = (uInt) capacity;
    size_t used = 0;
    bool complete = false;
    bool refused = false;
    for (size_t turn = 0; turn < 32u; turn++) {
        status = decompress ? inflate(&stream, Z_NO_FLUSH)
                            : deflate(&stream, Z_FINISH);
        used = capacity - stream.avail_out;
        if (status == Z_STREAM_END) {
            complete = stream.avail_in == 0;
            break;
        }
        if (status != Z_OK || !decompress || stream.avail_out != 0) break;
        if (capacity == SCRIPT_COMPRESSION_OUTPUT_LIMIT) {
            refused = true;
            break;
        }
        size_t grown = capacity > SCRIPT_COMPRESSION_OUTPUT_LIMIT / 2u
            ? SCRIPT_COMPRESSION_OUTPUT_LIMIT : capacity * 2u;
        uint8_t *replacement = budget_realloc_category(
            bridge->budget, BUDGET_CATEGORY_JAVASCRIPT, output, grown);
        if (replacement == NULL) {
            refused = true;
            break;
        }
        output = replacement;
        capacity = grown;
        stream.next_out = output + used;
        stream.avail_out = (uInt) (capacity - used);
    }
    if (decompress) (void) inflateEnd(&stream);
    else (void) deflateEnd(&stream);
    JS_FreeValue(context, input_owner);

    JSValue result = JS_EXCEPTION;
    if (complete) {
        result = JS_NewArrayBufferCopy(context, output, used);
    } else if (refused || status == Z_MEM_ERROR) {
        result = refused
            ? JS_ThrowRangeError(context,
                                 "decompressed output exceeds bounded size")
            : JS_ThrowOutOfMemory(context);
    } else {
        result = JS_ThrowTypeError(context, "invalid compressed data");
    }
    budget_free(bridge->budget, output);
    return result;
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

static bool base64_whitespace(unsigned char value)
{
    return value == '\t' || value == '\n' || value == '\f'
        || value == '\r' || value == ' ';
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
    if (length > 1024u * 1024u) {
        JS_FreeCString(context, input);
        return JS_ThrowRangeError(context,
                                  "base64 input exceeds bounded size");
    }
    size_t digit_count = 0, padding = 0;
    bool padding_started = false, valid = true;
    for (size_t i = 0; i < length; i++) {
        unsigned char value = (unsigned char) input[i];
        if (base64_whitespace(value)) continue;
        if (value == '=') {
            padding_started = true;
            if (++padding > 2u) valid = false;
        } else if (base64_digit(value) < 0 || padding_started) {
            valid = false;
        } else {
            digit_count++;
        }
    }
    size_t remainder = digit_count % 4u;
    if ((padding == 0 && remainder == 1u)
        || (padding != 0
            && ((digit_count + padding) % 4u != 0u
                || (padding == 1u && remainder != 3u)
                || (padding == 2u && remainder != 2u)))) {
        valid = false;
    }
    if (!valid) {
        JS_FreeCString(context, input);
        return JS_ThrowTypeError(context, "invalid base64 input");
    }
    size_t output_capacity = (digit_count / 4u) * 3u
                             + (remainder == 0u ? 0u : remainder - 1u);
    uint8_t *output = NULL;
#if defined(PSP_BROWSER_BELLARD_QUICKJS)
    JSValue result = JS_NewStringLenLatin1UninitializedPortable(
        context, output_capacity, &output);
    if (JS_IsException(result)) {
        JS_FreeCString(context, input);
        return JS_EXCEPTION;
    }
#else
    JSValue result = JS_UNDEFINED;
    if (output_capacity != 0) {
        output = js_malloc(context, output_capacity);
        if (output == NULL) {
            JS_FreeCString(context, input);
            return JS_EXCEPTION;
        }
    }
#endif
    size_t destination = 0;
    uint32_t packed = 0;
    unsigned group = 0;
    for (size_t source = 0; source < length; source++) {
        unsigned char value = (unsigned char) input[source];
        if (base64_whitespace(value) || value == '=') continue;
        packed = (packed << 6) | (uint32_t) base64_digit(value);
        if (++group == 4u) {
            output[destination++] = (uint8_t) (packed >> 16);
            output[destination++] = (uint8_t) (packed >> 8);
            output[destination++] = (uint8_t) packed;
            packed = 0;
            group = 0;
        }
    }
    if (group == 2u) {
        output[destination++] = (uint8_t) (packed >> 4);
    } else if (group == 3u) {
        output[destination++] = (uint8_t) (packed >> 10);
        output[destination++] = (uint8_t) (packed >> 2);
    }
    if (destination != output_capacity) {
#if defined(PSP_BROWSER_BELLARD_QUICKJS)
        JS_FreeValue(context, result);
#else
        js_free(context, output);
#endif
        result = JS_ThrowTypeError(context, "invalid base64 input");
#if !defined(PSP_BROWSER_BELLARD_QUICKJS)
    } else {
        result = JS_NewStringLen(
            context, output_capacity == 0 ? "" : (const char *) output,
            output_capacity);
        js_free(context, output);
#endif
    }
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
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || bridge->host == NULL)
        return JS_ThrowInternalError(context, "callback runtime unavailable");
    if (js_rt_runtime_callback_checkpoint(bridge->host))
        return JS_UNDEFINED;
    char task[97] = {0};
    if (argc > 0 && JS_IsString(argv[0])) {
        size_t length = 0;
        const char *value = JS_ToCStringLen(context, &length, argv[0]);
        if (value != NULL) {
            if (length >= sizeof(task)) length = sizeof(task) - 1u;
            memcpy(task, value, length);
            task[length] = '\0';
            JS_FreeCString(context, value);
        } else {
            JSValue secondary = JS_GetException(context);
            JS_FreeValue(context, secondary);
        }
    }
    char message[160];
    snprintf(message, sizeof(message),
             "callback interrupted at task boundary%s%s",
             task[0] == '\0' ? "" : ": ", task);
#if defined(PSP_BROWSER_BELLARD_QUICKJS)
    JSValue exception = JS_ThrowInternalError(context, "%s", message);
    JS_SetUncatchableException(context, true);
    return exception;
#else
    JSValue exception = JS_NewInternalError(context, "%s", message);
    if (JS_IsException(exception)) return exception;
    JS_SetUncatchableError(context, exception);
    return JS_Throw(context, exception);
#endif
}

/* Event-handler attributes are script, but they are not eval().  Compile the
   exact attribute body through this host-only path so CSP's unsafe-eval gate
   can remain closed while script-src's unsafe-inline decision is still
   honoured.  The bootstrap calls this lazily on first dispatch and retains a
   bounded number of compiled handlers. */
static JSValue js_compile_inline_event_handler(JSContext *context,
                                               JSValueConst this_value,
                                               int argc,
                                               JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || bridge->host == NULL || bridge->document == NULL
        || argc < 1) {
        return JS_ThrowInternalError(context,
                                     "inline handler runtime unavailable");
    }
    if (!tilefinch_csp_allows_script_attribute(
            &bridge->document->content_security_policy)) {
        return JS_NULL;
    }

    size_t source_length = 0;
    const char *source = JS_ToCStringLen(context, &source_length, argv[0]);
    if (source == NULL) return JS_EXCEPTION;
    enum { INLINE_HANDLER_SOURCE_LIMIT = 64 * 1024 };
    if (source_length > INLINE_HANDLER_SOURCE_LIMIT) {
        JS_FreeCString(context, source);
        return JS_ThrowRangeError(context,
                                  "inline event handler exceeds bounded size");
    }

    static const char prefix[] = "(function(event){\n";
    static const char suffix[] = "\n})";
    if (source_length > SIZE_MAX - sizeof(prefix) - sizeof(suffix)) {
        JS_FreeCString(context, source);
        return JS_ThrowRangeError(context, "inline event handler is too large");
    }
    size_t length = sizeof(prefix) - 1u + source_length
        + sizeof(suffix) - 1u;
    char *program = budget_malloc(bridge->host->budget, length + 1u);
    if (program == NULL) {
        JS_FreeCString(context, source);
        return JS_ThrowOutOfMemory(context);
    }
    size_t used = 0;
    memcpy(program + used, prefix, sizeof(prefix) - 1u);
    used += sizeof(prefix) - 1u;
    memcpy(program + used, source, source_length);
    used += source_length;
    memcpy(program + used, suffix, sizeof(suffix));
    JS_FreeCString(context, source);
    JSValue function = JS_Eval(context, program, length,
                               "<inline-event-handler>",
                               JS_EVAL_TYPE_GLOBAL);
    budget_free(bridge->host->budget, program);
    return function;
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
            || strcmp(name, "__tilefinchCancelNetwork") == 0
            || strcmp(name, "__tilefinchWebSocketStart") == 0
            || strcmp(name, "__tilefinchWebSocketSend") == 0
            || strcmp(name, "__tilefinchWebSocketClose") == 0
            || strcmp(name, "__tilefinchMultiplayerStart") == 0
            || strcmp(name, "__tilefinchMultiplayerSend") == 0
            || strcmp(name, "__tilefinchMultiplayerAccept") == 0
            || strcmp(name, "__tilefinchMultiplayerAddRemoteCode") == 0
            || strcmp(name, "__tilefinchMultiplayerClose") == 0);
    int flags = test_network_override
        ? JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE : 0;
    return JS_DefinePropertyValueStr(
               context, global, name, value, flags) >= 0;
}
