/* HTML's HostGetCodeForEval operation unwraps only authentic TrustedScript
   instances.  The bootstrap helper is non-configurable and validates its
   private WeakMap brand; ordinary objects therefore retain ECMAScript's
   non-string eval behavior without invoking author coercion hooks. */
#if defined(TILEFINCH_QUICKJS_DYNAMIC_CODE_POLICY)
static int js_runtime_host_get_code_for_eval(
    JSContext *context, JSValueConst input, JSValue *code_out, void *opaque)
{
    (void) opaque;
    if (code_out == NULL) return -1;
    *code_out = JS_UNDEFINED;
    JSValue global = JS_GetGlobalObject(context);
    if (JS_IsException(global)) return -1;
    JSValue unwrap = JS_GetPropertyStr(
        context, global, "__tilefinchTrustedScriptForEval");
    JS_FreeValue(context, global);
    if (JS_IsException(unwrap)) return -1;
    if (!JS_IsFunction(context, unwrap)) {
        JS_FreeValue(context, unwrap);
        return 0;
    }
    JSValue result = JS_Call(context, unwrap, JS_UNDEFINED, 1, &input);
    JS_FreeValue(context, unwrap);
    if (JS_IsException(result)) return -1;
    if (JS_IsUndefined(result)) {
        JS_FreeValue(context, result);
        return 0;
    }
    if (!JS_IsString(result)) {
        JS_FreeValue(context, result);
        JS_ThrowInternalError(
            context, "TrustedScript eval hook returned a non-string value");
        return -1;
    }
    *code_out = result;
    return 1;
}
#endif

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

/* DOM "parse a selector" for querySelector(), matches() and closest():
   true when the argument is a valid selector list (selector_syntax.c). */
static JSValue js_selector_syntax_valid(JSContext *context,
                                        JSValueConst this_value,
                                        int argc, JSValueConst *argv)
{
    (void) this_value;
    if (argc < 1) return JS_FALSE;
    size_t length = 0;
    const char *text = JS_ToCStringLen(context, &length, argv[0]);
    if (text == NULL) return JS_EXCEPTION;
    bool valid = selector_syntax_valid_mode(text, length,
                                           argc > 1 && JS_ToBool(context, argv[1]) > 0);
    JS_FreeCString(context, text);
    return JS_NewBool(context, valid);
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
                     && bridge->stylesheet->motion_css_keyframes != 0);
}

static JSValue js_stylesheet_motion_source(
    JSContext *context, JSValueConst this_value, int argc,
    JSValueConst *argv)
{
    (void) this_value;
    (void) argc;
    (void) argv;
    DomBridge *bridge = JS_GetContextOpaque(context);
    const Stylesheet *sheet = bridge == NULL ? NULL : bridge->stylesheet;
    /* The captured, non-author-visible capability also supplies content
       identity without copying the optional source on a cache hit. */
    if (argc > 0 && JS_ToBool(context, argv[0]))
        return JS_NewInt64(context, sheet == NULL ? 0 : (int64_t) sheet->build_generation);
    /* An incomplete prefix cannot replace the DOM style fallback: later
       inline rules may be absent even though the normal cascade has them. */
    if (sheet == NULL || sheet->motion_css == NULL
        || sheet->motion_css_bounded_out) return JS_UNDEFINED;
    return JS_NewStringLen(context, sheet->motion_css, sheet->motion_css_length);
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

/* A negative value selects scheduler.js's caller-driven deterministic clock.
   Otherwise the timer wheel uses this side-effect-free monotonic sample as its
   sole clock. Keeping the domains disjoint prevents owner-supplied elapsed time
   from charging callback wall time a second time. */
static double js_rt_scheduler_sample(const DomBridge *bridge)
{
    if (bridge == NULL || bridge->deterministic_clock
        || !bridge->wall_clock_timers)
        return -1.0;
    uint64_t current = js_rt_monotonic_time_ns();
    return current >= bridge->performance_origin_ns
        ? (double) (current - bridge->performance_origin_ns)
              / 1000000.0
        : 0.0;
}

static JSValue js_scheduler_now(JSContext *context,
                                JSValueConst this_value,
                                int argc, JSValueConst *argv)
{
    (void) this_value;
    (void) argc;
    (void) argv;
    double elapsed = js_rt_scheduler_sample(JS_GetContextOpaque(context));
    return elapsed < 0 ? JS_NewInt32(context, -1)
                       : JS_NewFloat64(context, elapsed);
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
        if (!bridge->fixed_wall_clock && bridge->clock_wall_elapsed_ms
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

/* The default time zone Intl reports must be the one Date's local-time
   methods use. QuickJS derives local time from localtime_r(), so:
   - PSP: the newlib build compiles QuickJS's tm_gmtoff read to a constant
     zero (cmake/TilefinchDependencies.cmake), so Date is UTC and so is Intl.
   - A deterministic realm replaces Date's local methods with UTC ones.
   - Host: report the IANA name libc is using when it is genuinely known
     (TZ naming an installed zoneinfo file, or the /etc/localtime link),
     otherwise the current offset as an ECMA-402 offset identifier. */
#if !defined(__PSP__) && !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>

static bool host_time_zone_name_valid(const char *name)
{
    size_t length = strlen(name);
    if (length == 0 || length >= 64 || name[0] == '/' || name[0] == '.'
        || strstr(name, "..") != NULL || strstr(name, "//") != NULL
        || name[length - 1] == '/') {
        return false;
    }
    for (size_t at = 0; at < length; at++) {
        unsigned char c = (unsigned char) name[at];
        if (!isalnum(c) && c != '/' && c != '_' && c != '-' && c != '+')
            return false;
    }
    return true;
}

static bool host_time_zone_installed(const char *name)
{
    static const char *const roots[] = {
        "/usr/share/zoneinfo/", "/var/db/timezone/zoneinfo/"};
    for (size_t at = 0; at < sizeof(roots) / sizeof(roots[0]); at++) {
        char path[160];
        struct stat info;
        int written = snprintf(path, sizeof(path), "%s%s", roots[at], name);
        if (written > 0 && (size_t) written < sizeof(path)
            && stat(path, &info) == 0 && S_ISREG(info.st_mode)) {
            return true;
        }
    }
    return false;
}

static const char *host_time_zone_from_path(const char *path)
{
    const char *marker = strstr(path, "zoneinfo/");
    return marker == NULL ? NULL : marker + strlen("zoneinfo/");
}
#endif

static void js_rt_default_time_zone(const DomBridge *bridge, char *out,
                                    size_t capacity)
{
    snprintf(out, capacity, "UTC");
    if (bridge != NULL && bridge->deterministic_clock) return;
#if !defined(__PSP__) && !defined(_WIN32)
    const char *candidate = NULL;
    char link[256];
    const char *tz = getenv("TZ");
    if (tz != NULL) {
        if (tz[0] == ':') tz++;
        candidate = tz[0] == '/' ? host_time_zone_from_path(tz) : tz;
        /* POSIX: an empty TZ means UTC. */
        if (tz[0] == '\0') return;
    } else {
        ssize_t length = readlink("/etc/localtime", link, sizeof(link) - 1);
        if (length > 0) {
            link[length] = '\0';
            candidate = host_time_zone_from_path(link);
        }
    }
    if (candidate != NULL && host_time_zone_name_valid(candidate)
        && host_time_zone_installed(candidate)) {
        static const char *const utc_aliases[] = {
            "UTC", "Etc/UTC", "Etc/UCT", "UCT", "Etc/GMT", "GMT",
            "Etc/Universal", "Universal", "Etc/Zulu", "Zulu", "Etc/GMT0",
            "GMT0", "Etc/GMT+0", "Etc/GMT-0", "GMT+0", "GMT-0",
            "Etc/Greenwich", "Greenwich"};
        for (size_t at = 0; at < sizeof(utc_aliases) / sizeof(utc_aliases[0]);
             at++) {
            if (strcmp(candidate, utc_aliases[at]) == 0) return;
        }
        if (strlen(candidate) < capacity) {
            snprintf(out, capacity, "%s", candidate);
            return;
        }
    }
    time_t now = time(NULL);
    struct tm local;
    if (localtime_r(&now, &local) == NULL) return;
    long offset_minutes = local.tm_gmtoff / 60;
    if (offset_minutes == 0) return;
    long magnitude = offset_minutes < 0 ? -offset_minutes : offset_minutes;
    snprintf(out, capacity, "%c%02ld:%02ld", offset_minutes < 0 ? '-' : '+',
             magnitude / 60, magnitude % 60);
#endif
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

/* HTMLOrSVGElement.nonce: (handle) reads [[CryptographicNonce]];
   (handle, value) sets it without touching the content attribute and
   returns false when the element cannot hold a slot, so the caller falls
   back to the attribute. */
JS_RT_GUARDED_NATIVE(static, js_element_nonce)(JSContext *context, JSValueConst this_value,
                                int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = argc > 0
        ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    size_t length = 0;
    if (argc < 2) {
        const char *nonce = document_element_nonce(node, &length);
        return JS_NewStringLen(context, nonce == NULL ? "" : nonce, length);
    }
    const char *value = JS_ToCStringLen(context, &length, argv[1]);
    if (value == NULL) return JS_EXCEPTION;
    bool stored = bridge != NULL && document_element_set_nonce(
        bridge->document, node, value, length);
    JS_FreeCString(context, value);
    return JS_NewBool(context, stored);
}

static JSValue js_post_message_impl(JSContext *context,
                                    int argc, JSValueConst *argv,
                                    bool keyed)
{
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || argc < 3) {
        return JS_ThrowTypeError(context, "frame messaging is not configured");
    }
    int64_t target = 0;
    if (JS_ToInt64(context, &target, argv[0]) < 0) return JS_EXCEPTION;
    size_t json_length = 0, origin_length = 0, task_kind_length = 0;
    const char *json = JS_ToCStringLen(context, &json_length, argv[1]);
    const char *origin = JS_ToCStringLen(context, &origin_length, argv[2]);
    const char *task_kind = argc >= 4
        ? JS_ToCStringLen(context, &task_kind_length, argv[3]) : NULL;
    int64_t task_sequence = 0;
    bool task_valid = argc < 4 || (task_kind != NULL
        && (argc < 5 || JS_ToInt64(context, &task_sequence, argv[4]) == 0));
    if (json == NULL || origin == NULL || !task_valid) {
        if (json != NULL) JS_FreeCString(context, json);
        if (origin != NULL) JS_FreeCString(context, origin);
        if (task_kind != NULL) JS_FreeCString(context, task_kind);
        return JS_EXCEPTION;
    }
    if (task_sequence < 0) {
        JS_FreeCString(context, task_kind);
        JS_FreeCString(context, origin);
        JS_FreeCString(context, json);
        return JS_ThrowRangeError(
            context, "message task sequence must be non-negative");
    }
    bool valid = json_length <= SCRIPT_FRAME_MESSAGE_BYTE_LIMIT
        && origin_length <= 2048;
    /* All coercions (which may run JS) are complete. The navigation callback
       copies these bytes before it returns, without calling page script. */
    const uint8_t *key_bytes = NULL;
    size_t key_length = 0;
    JSValue key_buffer = JS_UNDEFINED;
    if (keyed && valid) {
        if (argc < 6) {
            JS_ThrowTypeError(context, "missing private message data");
        } else {
            size_t offset = 0, element = 0, storage = 0;
            /* The private caller provides a Uint8Array. Validate the view
               bounds without invoking any author-defined property hooks. */
            key_buffer = JS_GetTypedArrayBuffer(
                context, argv[5], &offset, &key_length, &element);
            if (!JS_IsException(key_buffer)) {
                const uint8_t *data = JS_GetArrayBuffer(
                    context, &storage, key_buffer);
                if (data != NULL && element == 1 && offset <= storage
                    && key_length <= storage - offset) {
                    key_bytes = data + offset;
                } else if (!JS_HasException(context)) {
                    JS_ThrowTypeError(context, "invalid private message data");
                }
            }
        }
        if (key_bytes == NULL) {
            JS_FreeValue(context, key_buffer);
            if (task_kind != NULL) JS_FreeCString(context, task_kind);
            JS_FreeCString(context, origin);
            JS_FreeCString(context, json);
            return JS_EXCEPTION;
        }
        valid = key_length != 0
            && key_length <= SCRIPT_FRAME_MESSAGE_BYTE_LIMIT - json_length
            && memchr(json, '\0', json_length) == NULL
            && memchr(origin, '\0', origin_length) == NULL;
    }
    /* A WindowProxy may outlive a detached iframe's browsing context.
       postMessage to that discarded target is a silent no-op in browsers;
       absence of our optional navigation bridge must not leak as an author
       exception. */
    bool queued = valid && (bridge->post_message == NULL
        || bridge->post_message(
            bridge->post_message_opaque, bridge->host, (long) target,
            json, origin, task_kind == NULL ? "unknown" : task_kind,
            (uint64_t) task_sequence, key_bytes, key_length));
    JS_FreeValue(context, key_buffer);
    if (task_kind != NULL) JS_FreeCString(context, task_kind);
    JS_FreeCString(context, origin);
    JS_FreeCString(context, json);
    if (!queued) return JS_ThrowInternalError(context,
                                               "message queue rejected postMessage");
    return JS_UNDEFINED;
}

static JSValue js_post_message(JSContext *context, JSValueConst this_value,
                               int argc, JSValueConst *argv)
{
    (void) this_value;
    return js_post_message_impl(context, argc, argv, false);
}

static JSValue js_post_key_message(JSContext *context, JSValueConst this_value,
                                   int argc, JSValueConst *argv)
{
    (void) this_value;
    return js_post_message_impl(context, argc, argv, true);
}

/* FNV-1a over a URL up to its fragment; 0 means "none". */
static uint64_t js_rt_popunder_hash(const char *url)
{
    if (url == NULL || url[0] == '\0') return 0;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const char *at = url; *at != '\0' && *at != '#'; at++) {
        hash ^= (unsigned char) *at;
        hash *= UINT64_C(1099511628211);
    }
    return hash == 0 ? 1 : hash;
}

/* The popunder guard compares sites (scheme and registrable domain). */
static uint64_t js_rt_popunder_site(const char *url)
{
    char site[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    return url != NULL && tilefinch_url_site_key(url, site, sizeof(site))
        ? js_rt_popunder_hash(site) : 0;
}

/* Popunder guard. Tilefinch opens no auxiliary windows, so a page that
   answers a user activation by opening the content in a new window and
   sending this (opener) document elsewhere would replace the page the user
   asked for with that other destination. While the activation that asked
   for the window is transient, a script navigation of the top-level
   document is refused when its site is none of: the document's, the
   requested window's, and the activated hyperlink's destination's. The
   hyperlink, when there is one, is followed instead: it is what the user
   pressed. Unless the page cancelled the click, the browser follows it as
   the click's default action, so the guard leaves it to that one load
   (script_runtime_note_link_default). Runs at both ends of the pattern,
   since pages call window.open() and assign location in either order. */
static void js_rt_bridge_filter_popunder(DomBridge *bridge)
{
    if (bridge == NULL || !bridge->navigation_requested
        || bridge->new_window_expires_ms == 0 || bridge->host == NULL
        || bridge->host->document_scope != SCRIPT_DOCUMENT_SCOPE_TOP_LEVEL
        || bridge->document_url == NULL) return;
    if (js_rt_bridge_elapsed_ms(bridge) >= bridge->new_window_expires_ms) {
        bridge->new_window_expires_ms = 0;
        return;
    }
    uint64_t target = js_rt_popunder_site(bridge->navigation_url);
    if (target == 0 || target == js_rt_popunder_site(bridge->document_url)
        || target == bridge->new_window_site
        || target == js_rt_popunder_site(bridge->activation_link_url))
        return;
    bool follow_link = bridge->activation_link_url != NULL
        && !bridge->activation_link_default
        && js_rt_popunder_hash(bridge->activation_link_url)
               != js_rt_popunder_hash(bridge->document_url);
#if !defined(TILEFINCH_NO_TRACE)
    if (getenv("TILEFINCH_TRACE_NAVIGATION_REQUESTS") != NULL) {
        fprintf(stderr,
                "tilefinch-script-navigation: refused=popunder url=\"%s\" "
                "follow=\"%s\"\n", bridge->navigation_url,
                follow_link ? bridge->activation_link_url : "");
    }
#endif
    bridge->navigation_requested = false;
    bridge->navigation_replace = false;
    bridge->navigation_user_activated = false;
    bridge->navigation_url[0] = '\0';
    if (!follow_link) return;
    snprintf(bridge->navigation_url, sizeof(bridge->navigation_url), "%s",
             bridge->activation_link_url);
    bridge->navigation_requested = true;
    bridge->navigation_user_activated = true;
    bridge->navigation_follows_link = true;
}

/* window.open() opens nothing (it returns null). During a user activation,
   a request for an HTTP(S) document arms the popunder guard. A blank
   window, which the page would fill itself, does not: pages commonly fall
   back to navigating in place when such a window fails to open. */
static JSValue js_note_new_window(JSContext *context,
                                  JSValueConst this_value,
                                  int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || argc < 1
        || !js_rt_bridge_user_activation_is_active(bridge))
        return JS_UNDEFINED;
    size_t length = 0;
    const char *url = JS_ToCStringLen(context, &length, argv[0]);
    if (url == NULL) return JS_EXCEPTION;
    char resolved[TILEFINCH_URL_SERIALIZED_LIMIT];
    const char *base = js_rt_bridge_calculated_base_url(bridge);
    bool document = length > 0 && length < sizeof(resolved) && base != NULL
        && tilefinch_url_resolve(base, url, resolved, sizeof(resolved))
        && (strncmp(resolved, "https://", 8) == 0
            || strncmp(resolved, "http://", 7) == 0);
    JS_FreeCString(context, url);
    if (!document) return JS_UNDEFINED;
    bridge->new_window_site = js_rt_popunder_site(resolved);
    bridge->new_window_expires_ms = bridge->user_activation_expires_ms;
    js_rt_bridge_filter_popunder(bridge);
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
        JS_RT_XHR_COUNT(bridge->host, navigation_refused);
        JS_FreeCString(context, url);
        return JS_ThrowTypeError(context, "invalid navigation URL");
    }
    snprintf(bridge->navigation_url, sizeof(bridge->navigation_url), "%s",
             url);
    bridge->navigation_requested = true;
    bridge->navigation_follows_link = false;
    JS_RT_XHR_COUNT(bridge->host, navigation_accepted);
    bridge->navigation_replace = argc > 1
                                 && JS_ToBool(context, argv[1]) > 0;
    /* Snapshot at the request boundary. The host may not consume this until
       a later turn, after the transient activation itself has expired. */
    bridge->navigation_user_activated =
        js_rt_bridge_user_activation_is_active(bridge);
#if !defined(TILEFINCH_NO_TRACE)
    if (getenv("TILEFINCH_TRACE_NAVIGATION_REQUESTS") != NULL) {
        fprintf(stderr,
                "tilefinch-script-navigation: replace=%d url=\"%s\"\n",
                bridge->navigation_replace ? 1 : 0,
                bridge->navigation_url);
    }
#endif
    JS_FreeCString(context, url);
    js_rt_bridge_filter_popunder(bridge);
    return JS_UNDEFINED;
}

static JSValue js_user_activation_state(JSContext *context,
                                        JSValueConst this_value,
                                        int argc, JSValueConst *argv)
{
    (void) this_value;
    (void) argc;
    (void) argv;
    DomBridge *bridge = JS_GetContextOpaque(context);
    unsigned state = 0;
    if (js_rt_bridge_user_activation_is_active(bridge)) state |= 1u;
    if (bridge != NULL && bridge->user_activation_has_been_active) state |= 2u;
    return JS_NewInt32(context, (int32_t) state);
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
    size_t title_length = 0;
    const char *title = document_attribute(node, "title", &title_length);
    if (title != NULL) {
        if (title_length >= sizeof(request.title)) {
            title_length = sizeof(request.title) - 1u;
            /* Cut at a character boundary, never inside a UTF-8
               sequence: back off while the first dropped byte continues
               one. */
            while (title_length > 0
                   && ((unsigned char) title[title_length] & 0xC0u)
                          == 0x80u)
                title_length--;
        }
        memcpy(request.title, title, title_length);
        request.title[title_length] = '\0';
    }
    TilefinchCorsSettings cors_settings = document_cors_settings(node);
    if (cors_settings != TILEFINCH_CORS_SETTINGS_NONE) {
        request.mode = TILEFINCH_REQUEST_MODE_CORS;
        request.credentials =
            tilefinch_cors_settings_credentials(cors_settings, true);
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
    memcpy(bridge->media_title, request.title, sizeof(bridge->media_title));
    bridge->media_requested = true;
    bridge_pin_node_slot(bridge, slot);
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
    /* Positive requests observe geometry, just like a root size read.
       Refresh a dirty extent as well as a truncated one: a page can grow
       or shrink and call scrollTo in the same callback. Zero needs no
       measurement on a full layout. The resulting geometry can be adopted
       by the relayout that publishes the mutation. This forced layout is
       deliberate (CSSOM View clamps to the current scrolling area, and
       Chromium updates layout for every scrollTo but (0, 0)): the
       accepted position is returned and read back as scrollY in this task,
       so deferring it would publish an unclamped position. */
    if (next != 0) js_rt_bridge_note_parser_writes_for_layout(bridge);
    if (js_rt_bridge_layout_truncated(bridge)
        || (next > 0 && bridge->relayout_dirty != NULL
            && *bridge->relayout_dirty))
        (void) js_rt_bridge_flush_synchronous_layout(bridge, NULL);
    const LayoutDocument *layout = js_rt_bridge_layout(bridge);
    if (layout == NULL && js_rt_bridge_layout_truncated(bridge)
        && bridge->layout_region_end > 0)
        layout = bridge->layout;
    if (layout != NULL) {
        int maximum = layout->height > bridge->viewport_height
            ? layout->height - bridge->viewport_height : 0;
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

/* __tilefinchGeometry's fields, in definition order. */
enum {
    GEOMETRY_x, GEOMETRY_y, GEOMETRY_retained, GEOMETRY_authoritative,
    GEOMETRY_width, GEOMETRY_height, GEOMETRY_clientWidth,
    GEOMETRY_clientHeight, GEOMETRY_scrollWidth, GEOMETRY_scrollHeight,
    GEOMETRY_scrollLeft, GEOMETRY_scrollTop, GEOMETRY_FIELD_COUNT,
    GEOMETRY_clipLeft = GEOMETRY_FIELD_COUNT, GEOMETRY_clipTop,
    GEOMETRY_clipRight, GEOMETRY_clipBottom, GEOMETRY_inRoot,
    GEOMETRY_OBSERVER_FIELD_COUNT
};
static const char *const geometry_field_names[GEOMETRY_FIELD_COUNT] = {
    "x", "y", "retained", "authoritative", "width", "height",
    "clientWidth", "clientHeight", "scrollWidth", "scrollHeight",
    "scrollLeft", "scrollTop",
};

static JSValue js_element_geometry(JSContext *context,
                                   JSValueConst this_value,
                                   int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = bridge == NULL || argc < 1 ? NULL
                           : js_rt_bridge_node_arg(context, bridge, argv[0]);
    /* Optional private array-refill modes: -4 adds client fragments,
       -1 is pre-transform layout
       geometry; -2 is an observer root; non-negative is an observer
       target with a root handle (0 denotes the viewport). */
    int32_t mode = -3;
    if (argc > 2 && JS_IsNumber(argv[2])
        && JS_ToInt32(context, &mode, argv[2]) < 0) return JS_EXCEPTION;
    bool observe = mode >= 0;
    bool untransformed = mode == -1;
    bool fragments_requested = mode == -4;
    if (bridge != NULL && bridge->result != NULL) {
        bridge->result->geometry_queries++;
    }
    size_t node_name_length = 0;
    const char *node_name = node == NULL ? NULL
                            : document_element_name(node, &node_name_length);
    bool document_root = node_name != NULL && node_name_length == 4
                         && strncasecmp(node_name, "html", 4) == 0;
    bool viewport_root = document_root
                         || (node_name != NULL && node_name_length == 4
                             && strncasecmp(node_name, "body", 4) == 0);
    js_rt_bridge_note_parser_writes_for_layout(bridge);
    bool dirty = bridge != NULL && bridge->relayout_dirty != NULL
        && *bridge->relayout_dirty;
    /* Script never reads a truncated layout (js_rt_bridge_layout): a
       provisional one is completed first, like a dirty one, unless this
       box is one its proven region makes the complete layout's own
       (js_rt_bridge_proven_box), answered below as the complete layout
       would. The page root's extent is never proven. */
    bool proven = bridge != NULL && !dirty && !viewport_root
        && js_rt_bridge_proven_box(bridge, node) != NULL;
    if (bridge != NULL && !proven
        && (bridge->layout == NULL
            || js_rt_bridge_layout_truncated(bridge) || dirty)) {
        if (js_rt_bridge_flush_synchronous_layout(bridge, node)) {
            if (bridge->result != NULL)
                bridge->result->geometry_synchronous_layouts++;
            if (!js_rt_notify_style_flushed(context)) return JS_EXCEPTION;
            /* A watcher may reach author-overridden script properties.
               Re-resolve the handle after that JavaScript boundary. */
            node = argc < 1 ? NULL
                : js_rt_bridge_node_arg(context, bridge, argv[0]);
        }
    }
    const LayoutDocument *layout = js_rt_bridge_layout(bridge);
    const LayoutNodeBox *box = node == NULL || layout == NULL
                               ? NULL
                               : layout_box_for_node(layout, node);
    /* Still truncated: the box was proven (no completion needed), or the
       complete build was refused (memory). A proven box is answered
       exactly. Anything else
       is unknown, and is not reported where script would act on it (the
       pre-layout 300x65 at the top made every lazy loader fire at once):
       the complete layout places it at or below the region, or nowhere,
       so it reads as an empty box at the region's end. The page root
       reads the published extent the reader scrolls (and navigation
       clamps to). */
    const LayoutDocument *extent = layout;
    bool unknown = false;
    int unknown_y = 0;
    if (layout == NULL && js_rt_bridge_layout_truncated(bridge)
        && bridge->layout_region_end > 0) {
        extent = bridge->layout;
        box = viewport_root ? NULL : js_rt_bridge_proven_box(bridge, node);
        if (box != NULL) {
            layout = bridge->layout;
        } else if (node != NULL && !viewport_root) {
            unknown = true;
            unknown_y = bridge->layout_region_end;
        }
    }
    if (box == NULL && document_manual_shadow_excludes(node)) {
        /* A manual slot excludes the light subtree from the box tree.
           Don't synthesize its authored width/height in the JS fallback. */
        unknown = true;
        unknown_y = 0;
    }
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
    const LayoutInlineFragment *fragment =
        node != NULL && layout != NULL && (box == NULL || fragments_requested)
        ? layout_inline_fragment_first(layout, node) : NULL;
    LayoutNodeBox inline_box;
    if (fragment != NULL) {
        int left = fragment->x, top = fragment->y;
        int right = left + fragment->width, bottom = top + fragment->height;
        const LayoutInlineFragment *part = fragment;
        inline_box = (LayoutNodeBox) {
            .command_end = part->command, .cssom_geometry_authoritative = true
        };
        for (; part != NULL; part = layout_inline_fragment_next(layout, part)) {
            if (part->x < left) left = part->x;
            if (part->y < top) top = part->y;
            if (part->x + part->width > right) right = part->x + part->width;
            if (part->y + part->height > bottom) bottom = part->y + part->height;
            inline_box.command_start = part->command;
        }
        inline_box.x = left;
        inline_box.y = top;
        inline_box.width = right - left;
        inline_box.height = bottom - top;
        box = &inline_box;
    }
    int x = box == NULL ? 0 : box->x;
    int y = box == NULL ? unknown_y : box->y;
    LayoutScrollPlacement placement;
    bool in_root = false;
    if (observe && node != NULL && layout != NULL) {
        LayoutObserverMargin margins = {0};
        if (argc >= 8) {
            for (unsigned side = 0; side < 4; side++) {
                int32_t value = 0;
                if (JS_ToInt32(context, &value, argv[3 + side]) < 0)
                    return JS_EXCEPTION;
                margins.values[side] = value;
            }
            uint32_t mask;
            if (JS_ToUint32(context, &mask, argv[7]) < 0) return JS_EXCEPTION;
            margins.percent_mask = mask;
        }
        lxb_dom_node_t *root = mode == 0 ? NULL
            : js_rt_bridge_node_arg(context, bridge, argv[2]);
        bool found = layout_node_observer_placement(layout, node, root,
                                                    argc >= 8 ? &margins : NULL,
                                                    &placement);
        in_root = mode == 0 || (root != NULL && found);
        x += placement.dx;
        y += placement.dy;
    } else if (!untransformed && layout != NULL && node != NULL && box != NULL
               && box->cssom_geometry_authoritative
               && box->positioned_ancestor_distance != 0) {
        if (layout_node_scroll_placement(layout, NULL, node, &placement)) {
            x += placement.dx;
            y += placement.dy;
        }
    } else for (lxb_dom_node_t *parent =
             node == NULL || untransformed ? NULL : node->parent;
         parent != NULL; parent = parent->parent) {
        if (bridge != NULL && bridge->result != NULL) {
            bridge->result->geometry_ancestor_visits++;
        }
        const LayoutNodeBox *ancestor = layout == NULL ? NULL
            : layout_box_for_node(layout, parent);
        if (ancestor != NULL) {
            x -= ancestor->scroll_x;
            y -= ancestor->scroll_y;
        }
    }
    int fixed_dy = 0;
    bool fixed_geometry = false;
    int viewport_dy = 0;
    if (bridge != NULL
        && fixed_box_offset(layout, box, bridge->viewport_height,
                            untransformed ? 0 : bridge->page_scroll_y,
                            &fixed_geometry,
                            &fixed_dy)) {
        viewport_dy = fixed_dy;
    } else if (!untransformed && bridge != NULL) {
        viewport_dy = -bridge->page_scroll_y;
        if (!fixed_geometry && layout != NULL && layout->sticky_count != 0)
            viewport_dy += layout_node_sticky_offset(
                layout, node, bridge->page_scroll_y, bridge->viewport_height);
    }
    y += viewport_dy;
    LayoutNodeBox layout_box;
    if (untransformed && box != NULL && box != &inline_box) {
        layout_box_untransformed(layout, box, &layout_box);
        box = &layout_box;
        x = box->x;
        y = box->y + viewport_dy;
    }
    /* Every value is computed before anything is stored: defining entries
       on a caller-supplied array (a Proxy passes JS_IsArray) can run
       script. */
    int values[GEOMETRY_OBSERVER_FIELD_COUNT];
#define SET_GEOMETRY(name, value) (values[GEOMETRY_##name] = (value))
    SET_GEOMETRY(x, x);
    SET_GEOMETRY(y, y);
    SET_GEOMETRY(retained, box != NULL);
    SET_GEOMETRY(
        authoritative,
        unknown || (fragments_requested && box == NULL
                    && layout_inline_fragments_complete(layout))
        || (box != NULL
            && (box->cssom_geometry_authoritative || fixed_geometry)));
    if (box != NULL
        && (box->cssom_geometry_authoritative || fixed_geometry)
        && bridge != NULL && bridge->result != NULL) {
        bridge->result->geometry_retained_fast_paths++;
    }
    int fallback_width = unknown ? 0
                         : bridge == NULL || layout == NULL
                         ? (viewport_root && bridge != NULL
                            ? bridge->viewport_width : 300) : 0;
    int fallback_height = unknown ? 0
                          : bridge == NULL || layout == NULL
                          ? (viewport_root && bridge != NULL
                             ? bridge->viewport_height : 65) : 0;
    ElementScrollIntent *intent = box == NULL
        ? bridge_scroll_intent_find(bridge, node) : NULL;
    bool clipped_root = mode == -2 && box != NULL
        && (box->clips_x || box->clips_y);
    if (clipped_root) {
        SET_GEOMETRY(x, x + box->clip_inset_left);
        SET_GEOMETRY(y, y + box->clip_inset_top);
    }
    SET_GEOMETRY(width, box == NULL ? fallback_width
                         : (clipped_root ? box->client_width : box->width));
    SET_GEOMETRY(height, box == NULL ? fallback_height
                          : (clipped_root ? box->client_height : box->height));
    /* CSSOM View defines the root element's client box as the layout
       viewport.  A retained layout box for <html> is an implementation
       detail and may legitimately have no content box of its own. */
    SET_GEOMETRY(clientWidth, document_root && bridge != NULL
                                ? bridge->viewport_width
                                : (box == NULL ? fallback_width
                                               : box->client_width));
    SET_GEOMETRY(clientHeight, document_root && bridge != NULL
                                 ? bridge->viewport_height
                                 : (box == NULL ? fallback_height
                                                : box->client_height));
    /* The root element's scrolling area is the document's: at least the
       viewport, and as large as the laid-out page. Its own retained box
       has no content extent (pages read documentElement.scrollHeight to
       find the page length; it was 0). */
    int scroll_width = box == NULL ? fallback_width : box->content_width;
    int scroll_height = box == NULL ? fallback_height : box->content_height;
    if (document_root && bridge != NULL) {
        int page_width = extent == NULL ? 0
            : (extent->scroll_width > extent->width
               ? extent->scroll_width : extent->width);
        int page_height = extent == NULL ? 0 : extent->height;
        scroll_width = page_width > bridge->viewport_width
            ? page_width : bridge->viewport_width;
        scroll_height = page_height > bridge->viewport_height
            ? page_height : bridge->viewport_height;
    }
    SET_GEOMETRY(scrollWidth, scroll_width);
    SET_GEOMETRY(scrollHeight, scroll_height);
    SET_GEOMETRY(scrollLeft, box == NULL
                               ? (intent == NULL ? 0 : intent->scroll_x)
                               : box->scroll_x);
    SET_GEOMETRY(scrollTop, box == NULL
                              ? (intent == NULL ? 0 : intent->scroll_y)
                              : box->scroll_y);
    if (observe) {
        SET_GEOMETRY(clipLeft, in_root ? placement.clip_left : 0);
        SET_GEOMETRY(clipTop, in_root ? placement.clip_top + viewport_dy : 0);
        SET_GEOMETRY(clipRight, in_root ? placement.clip_right : 0);
        SET_GEOMETRY(clipBottom, in_root ? placement.clip_bottom + viewport_dy : 0);
        /* A browser page owes its first observation after layout, not a
           synthetic invisible entry before any geometry has published.
           The runtime can also run without a layout owner (unit/worker
           contexts); those still get their initial notification. */
        SET_GEOMETRY(inRoot, layout == NULL && !unknown && bridge != NULL
                     && bridge->expect_navigation_layout ? -1 : in_root);
    }
#undef SET_GEOMETRY
    /* getBoundingClientRect passes its own array to refill, indexed in
       geometry_field_names order, sparing an object and twelve named properties per
       call; every other caller takes the object. */
    bool refill = !fragments_requested && argc > 1
        && JS_IsArray(context, argv[1]) > 0;
    /* Fragment publication walks retained layout storage. Use a fresh
       native array, not a script-supplied proxy that can replace layout. */
    JSValue result = fragments_requested ? JS_NewArray(context)
        : refill ? JS_DupValue(context, argv[1]) : JS_NewObject(context);
    if (JS_IsException(result)) return result;
    if (!refill && bridge != NULL && bridge->result != NULL)
        bridge->result->geometry_result_objects++;
    int count = observe && refill ? GEOMETRY_OBSERVER_FIELD_COUNT
                                  : GEOMETRY_FIELD_COUNT;
    for (int i = 0; i < count; i++) {
        JSValue value = JS_NewInt32(context, values[i]);
        if ((refill || fragments_requested
             ? js_rt_define_index(context, result, (uint32_t) i, value)
             : js_rt_define_field(context, result, geometry_field_names[i],
                                  value)) < 0) {
            JS_FreeValue(context, result);
            return JS_EXCEPTION;
        }
    }
    if (fragments_requested && fragment != NULL) {
        int dx = x - inline_box.x, dy = y - inline_box.y;
        uint32_t at = GEOMETRY_FIELD_COUNT;
        for (const LayoutInlineFragment *part = fragment; part != NULL;
             part = layout_inline_fragment_next(layout, part)) {
            if (js_rt_define_index(context, result, at++,
                                   JS_NewInt32(context, part->x + dx)) < 0
                || js_rt_define_index(context, result, at++,
                                      JS_NewInt32(context, part->y + dy)) < 0
                || js_rt_define_index(context, result, at++,
                                      JS_NewInt32(context, part->width)) < 0
                || js_rt_define_index(context, result, at++,
                                      JS_NewInt32(context, part->height)) < 0) {
                JS_FreeValue(context, result);
                return JS_EXCEPTION;
            }
        }
    }
    return result;
}

JS_RT_GUARDED_NATIVE(static, js_set_element_scroll)(JSContext *context,
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
    /* The new ArrayBuffer intrinsics exist now and no author code has run
       in this realm since they were created. */
    if (!js_wasm_guard_buffer_transfer(realm)) {
        host->frame_realms[slot].intrinsic_state = 3;
        JS_RetireContext(realm);
        JS_Throw(caller, JS_GetException(realm));
        return false;
    }
    host->frame_realms[slot].intrinsic_state = 2;
    if (host->interface_snapshot) JS_SnapshotInterfaces(realm);
    return true;
}

/* What a frame realm's global inherits from: every name the embedding
   page's global has, read through that global with its own receiver. It is
   an ordinary exotic object, not a Proxy, so a name neither global has is
   unresolvable: `missing` throws a ReferenceError, `typeof missing` is
   "undefined" and `"missing" in window` is false. An assignment still
   creates the frame's own binding. */
static JSClassID frame_fallback_class_id;

static int frame_fallback_get_own_property(JSContext *context,
                                           JSPropertyDescriptor *descriptor,
                                           JSValueConst object, JSAtom atom)
{
    (void) object;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || bridge->host == NULL
        || bridge->host->context == NULL) return 0;
    JSValue page = JS_GetGlobalObject(bridge->host->context);
    int present = JS_HasProperty(context, page, atom);
    if (present > 0 && descriptor != NULL) {
        JSValue value = JS_GetProperty(context, page, atom);
        if (JS_IsException(value)) present = -1;
        else {
            descriptor->flags = JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE;
            descriptor->value = value;
            descriptor->getter = JS_UNDEFINED;
            descriptor->setter = JS_UNDEFINED;
        }
    }
    JS_FreeValue(context, page);
    return present;
}

static JSClassExoticMethods frame_fallback_exotic = {
    .get_own_property = frame_fallback_get_own_property,
};

static JSClassDef frame_fallback_class = {
    .class_name = "Object",
    .exotic = &frame_fallback_exotic,
};

static bool frame_realm_inherit_page_global(JSContext *realm, JSValueConst global)
{
    JSRuntime *runtime = JS_GetRuntime(realm);
    if (frame_fallback_class_id == 0) JS_NewClassID(&frame_fallback_class_id);
    if (!JS_IsRegisteredClass(runtime, frame_fallback_class_id)
        && JS_NewClass(runtime, frame_fallback_class_id,
                       &frame_fallback_class) < 0) return false;
    JSValue fallback = JS_NewObjectProtoClass(realm, JS_NULL,
                                              frame_fallback_class_id);
    if (JS_IsException(fallback)) return false;
    int set = JS_SetPrototype(realm, global, fallback);
    JS_FreeValue(realm, fallback);
    return set >= 0;
}

static bool legacy_define_frame_window_members(JSContext *realm,
                                               JSValueConst global);

/* A frame window owns the interface objects and members a top-level window
   does. Its realm defines only the ECMAScript intrinsics and the frame's
   own window bindings; every other browser name is the page's, reached
   through frame_realm_inherit_page_global. The WindowProxy reports those
   names as its own, taken from the page window as author code starts, so
   Object.getOwnPropertyNames(frame) lists them and their descriptors read
   the page's same objects (nothing is copied per frame). Names a page
   script later adds to its own window are not the frame's. */
static bool js_rt_capture_frame_page_names(ScriptRuntime *host, JSValueConst global)
{
    JSPropertyEnum *names = NULL;
    uint32_t count = 0;
    if (JS_GetOwnPropertyNames(host->context, &names, &count, global,
                               JS_GPN_STRING_MASK) < 0) return false;
    static const char private_prefix[] = "__tilefinch";
    uint32_t kept = 0;
    for (uint32_t i = 0; i < count; i++) {
        const char *name = JS_AtomToCString(host->context, names[i].atom);
        bool private_name = name == NULL
            || strncmp(name, private_prefix, sizeof(private_prefix) - 1u) == 0;
        if (name != NULL) JS_FreeCString(host->context, name);
        else {
            JSValue exception = JS_GetException(host->context);
            JS_FreeValue(host->context, exception);
        }
        if (private_name) JS_FreeAtom(host->context, names[i].atom);
        else names[kept++] = names[i];
    }
    host->frame_page_names = names;
    host->frame_page_name_count = kept;
    return true;
}

/* Whether atom is a page window name the frame global inherits: one of the
   captured names that the page window still owns and the realm does not. */
static int frame_scope_inherits(ScriptRuntime *host, JSContext *realm,
                                JSValueConst realm_global,
                                JSValueConst page_global, uint32_t index)
{
    JSAtom atom = host->frame_page_names[index].atom;
    int own = JS_GetOwnProperty(realm, NULL, realm_global, atom);
    if (own != 0) return own < 0 ? -1 : 0;
    return JS_GetOwnProperty(host->context, NULL, page_global, atom);
}

/* frameScope(scope, true, true): the WindowProxy's own keys, the realm
   global's followed by the inherited page names. frameScope(scope, true,
   key): whether key is an inherited page name. */
static JSValue frame_scope_keys(JSContext *context, ScriptRuntime *host,
                                size_t slot, JSValueConst query)
{
    JSContext *realm = host->frame_realms[slot].context;
    JSValueConst realm_global = host->frame_realms[slot].global;
    JSValue page_global = JS_GetGlobalObject(host->context);
    JSValue result = JS_UNDEFINED;
    if (!JS_IsBool(query)) {
        JSAtom atom = JS_ValueToAtom(context, query);
        int found = 0;
        if (atom == JS_ATOM_NULL) found = -1;
        for (uint32_t i = 0; found == 0 && i < host->frame_page_name_count;
             i++) {
            if (host->frame_page_names[i].atom != atom) continue;
            found = frame_scope_inherits(host, realm, realm_global,
                                         page_global, i);
            if (found == 0) break;
        }
        if (atom != JS_ATOM_NULL) JS_FreeAtom(context, atom);
        JS_FreeValue(host->context, page_global);
        return found < 0 ? JS_EXCEPTION : JS_NewBool(context, found > 0);
    }
    JSPropertyEnum *own = NULL;
    uint32_t own_count = 0;
    if (JS_GetOwnPropertyNames(realm, &own, &own_count, realm_global,
                               JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK) < 0) {
        JS_FreeValue(host->context, page_global);
        return JS_Throw(context, JS_GetException(realm));
    }
    result = JS_NewArray(context);
    uint32_t length = 0;
    for (uint32_t i = 0; !JS_IsException(result) && i < own_count; i++) {
        if (JS_SetPropertyUint32(context, result, length++,
                                 JS_AtomToValue(context, own[i].atom)) < 0) {
            JS_FreeValue(context, result);
            result = JS_EXCEPTION;
        }
    }
    JS_FreePropertyEnum(realm, own, own_count);
    for (uint32_t i = 0; !JS_IsException(result)
                         && i < host->frame_page_name_count; i++) {
        int inherited = frame_scope_inherits(host, realm, realm_global,
                                             page_global, i);
        if (inherited < 0
            || (inherited > 0
                && JS_SetPropertyUint32(
                       context, result, length++,
                       JS_AtomToValue(context,
                                      host->frame_page_names[i].atom)) < 0)) {
            JS_FreeValue(context, result);
            result = JS_EXCEPTION;
        }
    }
    JS_FreeValue(host->context, page_global);
    return result;
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
            if (argc > 1 && JS_ToBool(context, argv[1])) {
                if (!frame_realm_prepare(context, host, i))
                    return JS_EXCEPTION;
                return argc > 2 ? frame_scope_keys(context, host, i, argv[2])
                                : JS_UNDEFINED;
            }
#if defined(TILEFINCH_QUICKJS_DYNAMIC_CODE_POLICY)
            JS_RetireContext(host->frame_realms[i].context);
#endif
            JS_FreeValue(host->frame_realms[i].context,
                         host->frame_realms[i].global);
            JS_FreeContext(host->frame_realms[i].context);
            host->frame_realms[i].context = NULL;
            host->frame_realms[i].global = JS_UNDEFINED;
            host->frame_realms[i].intrinsic_state = 0;
            host->frame_realms[i].frame_handle = 0;
            return JS_UNDEFINED;
        }
        if (host->frame_realms[i].context != NULL) continue;
        JSContext *realm =
            JS_NewContextRaw(host->runtime);
        if (realm == NULL) return JS_ThrowOutOfMemory(context);
        /* BaseObjects defines eval/globalThis and autoinit properties that
           cannot be installed over the Window bindings. Install this core
           before exposing the global; defer the remaining intrinsic groups. */
        if (JS_AddIntrinsicBaseObjects(realm) || JS_AddIntrinsicEval(realm)) {
            JSValue exception = JS_GetException(realm);
            JS_FreeContext(realm);
            return JS_Throw(context, exception);
        }
        JS_SetContextOpaque(realm, bridge);
#if defined(TILEFINCH_QUICKJS_DYNAMIC_CODE_POLICY)
        JS_SetDynamicCodeEnabled(realm, bridge->dynamic_code_allowed);
        JS_SetHostGetCodeForEval(
            realm, js_runtime_host_get_code_for_eval, NULL);
#endif
        JSValue global = JS_GetGlobalObject(realm);
        if (!frame_realm_inherit_page_global(realm, global)
            || !legacy_define_frame_window_members(realm, global)) {
            JSValue exception = JS_GetException(realm);
            JS_FreeValue(realm, global);
            JS_FreeContext(realm);
            return JS_Throw(context, exception);
        }
        host->frame_realms[i].context = realm;
        host->frame_realms[i].global = JS_DupValue(realm, global);
        host->frame_realms[i].dynamic_code_allowed =
            bridge->dynamic_code_allowed;
        host->frame_realms[i].frame_handle = 0;
        return global;
    }
    if (argc > 1 && JS_ToBool(context, argv[1]))
        return JS_ThrowTypeError(context, "frame realm is retired");
    return argc != 0 ? JS_UNDEFINED
        : JS_ThrowRangeError(context, "frame realm quota exceeded");
}

/* __tilefinchBindFrameRealm(scope, proxy, handle): the frame's WindowProxy
   becomes the realm's top-level this (its own eval and Function see it), and
   the frame element stays addressable while the realm lives. The realm's
   own eval is the frame's window.eval for every caller: a parent's
   contentWindow.eval(source) is an indirect eval in the frame realm under
   the frame's dynamic-code policy, as in other browsers. */
static JSValue js_bind_frame_realm(JSContext *context,
                                   JSValueConst this_value,
                                   int argc, JSValueConst *argv)
{
    (void) this_value;
    if (argc < 2 || !JS_IsObject(argv[0]) || !JS_IsObject(argv[1]))
        return JS_ThrowTypeError(context,
                                 "frame realm binding requires scope and proxy");
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || bridge->host == NULL)
        return JS_ThrowInternalError(context, "frame runtime unavailable");
#ifdef JS_RT_TEST_INSTRUMENTS
    if (bridge->host->refused_frame_setups != 0) {
        bridge->host->refused_frame_setups--;
        return JS_ThrowInternalError(context, "frame setup refused");
    }
#endif
    int64_t frame_handle = 0;
    if (argc > 2 && JS_ToInt64(context, &frame_handle, argv[2]) < 0)
        return JS_EXCEPTION;
    for (size_t i = 0; i < SCRIPT_FRAME_REALM_LIMIT; i++) {
        if (bridge->host->frame_realms[i].context != NULL
            && JS_VALUE_GET_PTR(bridge->host->frame_realms[i].global)
                   == JS_VALUE_GET_PTR(argv[0])) {
#if defined(TILEFINCH_QUICKJS_DYNAMIC_CODE_POLICY)
            JS_SetGlobalThis(bridge->host->frame_realms[i].context, argv[1]);
#endif
            bridge->host->frame_realms[i].frame_handle = (long) frame_handle;
            js_rt_bridge_keep_node_handle(bridge, frame_handle);
            break;
        }
    }
    return JS_UNDEFINED;
}

#ifdef JS_RT_TEST_INSTRUMENTS
/* __tilefinchRefuseFrameSetups(count), host and validation realms only:
   the next count frame realm setups fail in js_bind_frame_realm, after the
   realm is published for its caller to retire. */
static JSValue js_refuse_frame_setups(JSContext *context,
                                      JSValueConst this_value,
                                      int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    uint32_t count = 0;
    if (bridge == NULL || bridge->host == NULL || argc < 1
        || JS_ToUint32(context, &count, argv[0]) < 0)
        return JS_ThrowTypeError(context, "frame setup refusal unavailable");
    bridge->host->refused_frame_setups = count;
    return JS_UNDEFINED;
}
#endif

static JSValue js_storage_get(JSContext *context, JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge->session == NULL || bridge->opaque_origin || argc < 2) {
        return JS_NULL;
    }
    bool local = JS_ToBool(context, argv[0]) > 0;
    size_t key_length = 0;
    const char *key = JS_ToCStringLen(context, &key_length, argv[1]);
    if (key == NULL) return JS_EXCEPTION;
    const char *value = NULL;
    size_t length = 0;
    bool found = browser_session_storage_get_scoped(
        bridge->session, bridge->document_url, local, bridge->session_storage_scope,
        key, key_length,
        &value, &length);
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
                               && !bridge->opaque_origin
                               && browser_session_site_data_allowed(
                                      bridge->session));
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
    BrowserSiteStorageInfo site = {0};
    size_t used = usage.storage_bytes > SIZE_MAX - usage.opfs_bytes
        ? SIZE_MAX : usage.storage_bytes + usage.opfs_bytes;
    size_t quota = browser_session_site_storage_info_for(
                       bridge->session, bridge->document_url, &site)
        ? site.limit_bytes : BROWSER_SITE_STORAGE_INITIAL_BYTES;
    if (quota < used) quota = used;
    JSValue values = JS_NewArray(context);
    if (JS_IsException(values)) return values;
    if (js_rt_define_index(
            context, values, 0,
            JS_NewInt64(context, (int64_t) used)) < 0
        || js_rt_define_index(
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
    if (js_rt_define_index(
            context, values, 0,
            JS_NewInt64(context, (int64_t) limit)) < 0
        || js_rt_define_index(
               context, values, 1,
               JS_NewInt64(context, (int64_t) total)) < 0
        || js_rt_define_index(
               context, values, 2,
               JS_NewInt64(context, (int64_t) used)) < 0) {
        JS_FreeValue(context, values);
        return JS_EXCEPTION;
    }
    return values;
}

/* Whether a Blob of argv[0] bytes fits the heap the realm can obtain,
   collecting once before saying no. The bootstrap refuses one that does
   not with a RangeError before allocating, so the page gets an error it
   can catch instead of an allocation refusal, which the memory stop rule
   would judge. Blobs have no cap of their own. */
static JSValue js_blob_fits(JSContext *context, JSValueConst this_value,
                            int argc, JSValueConst *argv)
{
    (void) this_value;
    double size = 0.0;
    if (argc < 1 || JS_ToFloat64(context, &size, argv[0]) < 0)
        return JS_EXCEPTION;
    DomBridge *bridge = JS_GetContextOpaque(context);
    ScriptRuntime *runtime = bridge == NULL ? NULL : bridge->host;
    if (runtime == NULL || !(size > 0.0)) return JS_TRUE;
    if (size <= (double) script_runtime_heap_obtainable(runtime))
        return JS_TRUE;
    (void) script_runtime_collect_and_trim(runtime);
    return JS_NewBool(
        context, size <= (double) script_runtime_heap_obtainable(runtime));
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
    size_t key_length = 0;
    const char *key = JS_ToCStringLen(context, &key_length, argv[1]);
    size_t value_length = 0;
    const char *value = JS_ToCStringLen(context, &value_length, argv[2]);
    if (key == NULL || value == NULL) {
        if (key != NULL) JS_FreeCString(context, key);
        if (value != NULL) JS_FreeCString(context, value);
        return JS_EXCEPTION;
    }
    bool stored = browser_session_storage_set_scoped(
        bridge->session, bridge->document_url, local, bridge->session_storage_scope,
        key, key_length, value, value_length);
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
    size_t length = browser_session_storage_length_scoped(
        bridge->session, bridge->document_url,
        JS_ToBool(context, argv[0]) > 0, bridge->session_storage_scope);
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
    size_t key_length = 0;
    bool found = index >= 0 && (uint64_t) index <= SIZE_MAX
        && browser_session_storage_key_scoped(
        bridge->session, bridge->document_url,
        JS_ToBool(context, argv[0]) > 0, bridge->session_storage_scope,
        (size_t) index, &key, &key_length);
    return found ? JS_NewStringLen(context, key, key_length) : JS_NULL;
}

static JSValue js_storage_remove(JSContext *context,
                                 JSValueConst this_value,
                                 int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge->session == NULL || bridge->opaque_origin || argc < 2) {
        return JS_TRUE;
    }
    bool local = JS_ToBool(context, argv[0]) > 0;
    size_t key_length = 0;
    const char *key = JS_ToCStringLen(context, &key_length, argv[1]);
    if (key == NULL) return JS_EXCEPTION;
    /* False when the Memory Stick refused to record it: the item stays. */
    bool removed = browser_session_storage_remove_scoped(
        bridge->session, bridge->document_url, local, bridge->session_storage_scope,
        key, key_length);
    JS_FreeCString(context, key);
    return JS_NewBool(context, removed);
}

static JSValue js_storage_clear(JSContext *context, JSValueConst this_value,
                                int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge->session == NULL || bridge->opaque_origin || argc < 1)
        return JS_TRUE;
    /* False when some item could not be removed; those stay. */
    return JS_NewBool(context, browser_session_storage_clear_scoped(
        bridge->session, bridge->document_url,
        JS_ToBool(context, argv[0]) > 0, bridge->session_storage_scope));
}

static JSValue js_cookie_get(JSContext *context, JSValueConst this_value,
                             int argc, JSValueConst *argv)
{
    (void) this_value; (void) argc; (void) argv;
    DomBridge *bridge = JS_GetContextOpaque(context);
    char cookies[BROWSER_COOKIE_HEADER_CAPACITY];
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
    if (tilefinch_trace_cookie()) {
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
#ifdef JS_RT_TEST_INSTRUMENTS
    /* Host and validation realms only: a test page reads what the console
       received, the browser's own warnings included, by defining
       __tilefinchTestConsoleSink(level, ...values). Replacing console.warn
       shows a page only its own calls, as in browsers. */
    if (bridge != NULL && bridge->host != NULL && bridge->host->test_probes) {
        JSValue global = JS_GetGlobalObject(context);
        JSValue sink = JS_GetPropertyStr(context, global,
                                         "__tilefinchTestConsoleSink");
        JS_FreeValue(context, global);
        if (JS_IsFunction(context, sink)) {
            JSValue sunk = JS_Call(context, sink, JS_UNDEFINED, argc, argv);
            JS_FreeValue(context, sink);
            if (JS_IsException(sunk)) return sunk;
            JS_FreeValue(context, sunk);
        } else {
            JS_FreeValue(context, sink);
        }
    }
#endif
    if (bridge == NULL || !tilefinch_trace_console()
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
    /* __tilefinchResolveURL(input, base, documentBase). The resolver
       parses only http(s) bases. An opaque base passed by script (blob:,
       data:, about:blank: no '/' after the scheme) is used as given, so
       absolute input resolves and relative input fails. The document's
       own URL (a fixture file path, about:blank) and a hierarchical
       non-special base such as "x:/", which pages use to normalize paths,
       resolve against the fixture, whose path is the same. */
    bool document_base = argc > 2 && JS_ToBool(context, argv[2]) > 0;
    const char *colon = base == NULL ? NULL : strchr(base, ':');
    bool opaque = base_argument != NULL && !document_base && colon != NULL
        && colon[1] != '/';
    const char *effective_base = opaque
        || (base != NULL && strstr(base, "://") != NULL)
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
    /* pushState/replaceState to another URL: the page may now show another
       video, so declarations the old URL's markup made are forgotten. */
    if (strcmp(bridge->document_url, normalized) != 0)
        media_declared_video_cache_destroy(bridge->document);
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

/* Lower-case an ASCII DNS name into output; false for anything a host
   parser would reject or rewrite (non-ASCII, ports, paths, credentials,
   spaces) or an over-long name. *numeric reports an all-digit IPv4 form. */
static bool document_domain_ascii_host(const char *text, size_t length,
                                       char output[254], bool *numeric)
{
    if (length == 0 || length > 253) return false;
    *numeric = true;
    for (size_t at = 0; at < length; at++) {
        unsigned char byte = (unsigned char) text[at];
        if (byte >= 'A' && byte <= 'Z') byte = (unsigned char) (byte + 32);
        if (!((byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9')
              || byte == '-' || byte == '.')) return false;
        if (byte != '.' && (byte < '0' || byte > '9')) *numeric = false;
        output[at] = (char) byte;
    }
    output[length] = '\0';
    return true;
}

/* HTML's document.domain setter checks: (value, current host) is valid when
   the value equals the host, or is a dot-boundary suffix of a DNS host and
   not a public suffix. IP hosts (including IPv6, which never passes the
   ASCII check) and opaque origins are refused. The setter itself is a no-op
   (origin-keyed agent clusters), so this only decides whether it throws. */
static JSValue js_document_domain_valid(JSContext *context,
                                        JSValueConst this_value,
                                        int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (argc < 2 || bridge == NULL || bridge->opaque_origin) return JS_FALSE;
    size_t value_length = 0, host_length = 0;
    const char *value = JS_ToCStringLen(context, &value_length, argv[0]);
    if (value == NULL) return JS_EXCEPTION;
    const char *host_text = JS_ToCStringLen(context, &host_length, argv[1]);
    if (host_text == NULL) {
        JS_FreeCString(context, value);
        return JS_EXCEPTION;
    }
    char wanted[254], host[254];
    bool wanted_numeric = true, host_numeric = true;
    bool same = value_length == host_length
        && memcmp(value, host_text, value_length) == 0;
    bool valid = !same && document_domain_ascii_host(value, value_length, wanted,
                                            &wanted_numeric)
        && document_domain_ascii_host(host_text, host_length, host,
                                      &host_numeric);
    JS_FreeCString(context, value);
    JS_FreeCString(context, host_text);
    if (same) return JS_TRUE;
    bool public_suffix = true;
    valid = valid && !wanted_numeric && !host_numeric
        && value_length < host_length
        && host[host_length - value_length - 1] == '.'
        && strcmp(host + host_length - value_length, wanted) == 0
        && tilefinch_public_suffix_classify(wanted, &public_suffix)
        && !public_suffix;
    return JS_NewBool(context, valid);
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

/* Complete buffered text bodies should not pay one interpreted iteration per
   byte. Malformed input stays in the JavaScript decoder because it implements
   the Encoding Standard's exact replacement behavior; this seam only accepts
   a wholly valid UTF-8 sequence (tilefinch_utf8_valid). */
static JSValue js_utf8_decode_valid(JSContext *context,
                                    JSValueConst this_value,
                                    int argc, JSValueConst *argv)
{
    (void) this_value;
    if (argc < 1)
        return JS_ThrowTypeError(context, "UTF-8 byte array required");
    size_t offset = 0u, length = 0u, bytes_per_element = 0u;
    JSValue buffer = JS_GetTypedArrayBuffer(
        context, argv[0], &offset, &length, &bytes_per_element);
    if (JS_IsException(buffer)) return buffer;
    size_t buffer_length = 0u;
    uint8_t *bytes = JS_GetArrayBuffer(context, &buffer_length, buffer);
    if ((bytes == NULL && buffer_length != 0u) || bytes_per_element != 1u
        || offset > buffer_length || length > buffer_length - offset) {
        JS_FreeValue(context, buffer);
        return JS_ThrowTypeError(context, "UTF-8 byte array required");
    }
    /* Keep this aligned with the buffered text decoder bound in platform.js
       and JS_FETCH_MAXIMUM_BYTES. Responses admitted by fetch/XHR must not
       fall back to one interpreted concatenation per byte merely because
       they are larger than the former 256 KiB helper ceiling. */
    if (length > 1024u * 1024u) {
        JS_FreeValue(context, buffer);
        return JS_NULL;
    }
    bytes = bytes == NULL ? NULL : bytes + offset;
    if (!tilefinch_utf8_valid(bytes, length)) {
        JS_FreeValue(context, buffer);
        return JS_NULL;
    }
    bool strip_bom = argc > 1 && JS_ToBool(context, argv[1]) > 0;
    if (strip_bom && length >= 3u && bytes[0] == 0xefu
        && bytes[1] == 0xbbu && bytes[2] == 0xbfu) {
        bytes += 3u;
        length -= 3u;
    }
    JSValue result = JS_NewStringLen(
        context, (const char *) (bytes == NULL ? (uint8_t *) "" : bytes),
        length);
    JS_FreeValue(context, buffer);
    return result;
}

/* TextDecoder's other encodings come from the Encoding Standard tables the
   document parser decodes with (text_encoding.c). The single-byte ones are
   stateless, so a streamed decode needs no carried state. */
static bool js_text_decoder_single_byte(TilefinchEncoding encoding)
{
    return (encoding >= TILEFINCH_ENCODING_IBM866
            && encoding <= TILEFINCH_ENCODING_X_MAC_CYRILLIC)
        || encoding == TILEFINCH_ENCODING_X_USER_DEFINED;
}

static bool js_text_decoder_utf16(TilefinchEncoding encoding)
{
    return encoding == TILEFINCH_ENCODING_UTF16LE
        || encoding == TILEFINCH_ENCODING_UTF16BE;
}

/* True when `bytes` are whole UTF-16 code units with every surrogate
   paired: what fatal UTF-16 decoding accepts. */
static bool js_utf16_well_formed(const unsigned char *bytes, size_t length,
                                 bool big_endian)
{
    if ((length & 1u) != 0u) return false;
    unsigned high = big_endian ? 0u : 1u;
    for (size_t at = 0; at < length; at += 2u) {
        unsigned unit = (unsigned) bytes[at + high] << 8
            | bytes[at + (high ^ 1u)];
        if (unit < 0xd800u || unit > 0xdfffu) continue;
        if (unit > 0xdbffu || at + 2u >= length) return false;
        unsigned next = (unsigned) bytes[at + 2u + high] << 8
            | bytes[at + 2u + (high ^ 1u)];
        if (next < 0xdc00u || next > 0xdfffu) return false;
        at += 2u;
    }
    return true;
}

static TilefinchEncoding js_text_decoder_encoding(JSContext *context,
                                                  JSValueConst label)
{
    size_t length = 0;
    const char *text = JS_ToCStringLen(context, &length, label);
    if (text == NULL) return TILEFINCH_ENCODING_NONE;
    TilefinchEncoding encoding = tilefinch_encoding_for_label(text, length);
    JS_FreeCString(context, text);
    return encoding;
}

/* __tilefinchEncodingName(label): the name of the encoding `label` names,
   when TextDecoder can decode it (UTF-8, which the bootstrap decodes,
   UTF-16LE/BE or a single-byte encoding); otherwise null. */
static JSValue js_encoding_name(JSContext *context, JSValueConst this_value,
                                int argc, JSValueConst *argv)
{
    (void) this_value;
    if (argc < 1) return JS_NULL;
    TilefinchEncoding encoding = js_text_decoder_encoding(context, argv[0]);
    if (encoding != TILEFINCH_ENCODING_UTF8
        && !js_text_decoder_single_byte(encoding)
        && !js_text_decoder_utf16(encoding)) return JS_NULL;
    return JS_NewString(context, tilefinch_encoding_name(encoding));
}

/* __tilefinchDecodeBytes(name, bytes, fatal): `bytes` (a Uint8Array)
   decoded completely with a single-byte encoding or UTF-16LE/BE (the
   bootstrap strips the BOM and carries streamed partial units); null when
   fatal and a byte has no mapping (none of the single-byte tables maps a
   byte to U+FFFD) or a UTF-16 surrogate or unit is incomplete. */
static JSValue js_decode_bytes(JSContext *context,
                               JSValueConst this_value,
                               int argc, JSValueConst *argv)
{
    (void) this_value;
    if (argc < 3) return JS_ThrowTypeError(context, "byte array required");
    TilefinchEncoding encoding = js_text_decoder_encoding(context, argv[0]);
    int fatal = JS_ToBool(context, argv[2]);
    bool utf16 = js_text_decoder_utf16(encoding);
    if ((!utf16 && !js_text_decoder_single_byte(encoding)) || fatal < 0)
        return JS_ThrowRangeError(context, "unsupported encoding");
    size_t offset = 0u, length = 0u, bytes_per_element = 0u;
    JSValue buffer = JS_GetTypedArrayBuffer(
        context, argv[1], &offset, &length, &bytes_per_element);
    if (JS_IsException(buffer)) return buffer;
    size_t buffer_length = 0u;
    uint8_t *bytes = JS_GetArrayBuffer(context, &buffer_length, buffer);
    if ((bytes == NULL && buffer_length != 0u) || bytes_per_element != 1u
        || offset > buffer_length || length > buffer_length - offset
        || length > 1024u * 1024u) {
        JS_FreeValue(context, buffer);
        return JS_ThrowTypeError(context, "byte array required");
    }
    const unsigned char *input = bytes == NULL ? NULL : bytes + offset;
    if (fatal && utf16
        && !js_utf16_well_formed(
               input, length, encoding == TILEFINCH_ENCODING_UTF16BE)) {
        JS_FreeValue(context, buffer);
        return JS_NULL;
    }
    /* Each byte becomes at most three UTF-8 bytes; keep the decoder's
       eight bytes of headroom so one call consumes everything. */
    size_t capacity = tilefinch_decoder_maximum_output(encoding, length) + 8u;
    unsigned char *output = js_malloc(context, capacity);
    if (output == NULL) {
        JS_FreeValue(context, buffer);
        return JS_EXCEPTION;
    }
    TilefinchDecoder decoder;
    tilefinch_decoder_init(&decoder, encoding);
    size_t remaining = length;
    size_t written = tilefinch_decoder_decode(
        &decoder, &input, &remaining, output, capacity, true);
    JS_FreeValue(context, buffer);
    bool refused = false;
    for (size_t at = 0; fatal && !utf16 && !refused && at + 3u <= written;
         at++)
        refused = output[at] == 0xefu && output[at + 1u] == 0xbfu
            && output[at + 2u] == 0xbdu;
    JSValue result = remaining != 0u
        ? JS_ThrowInternalError(context, "decoder output overflow")
        : refused ? JS_NULL
        : JS_NewStringLen(context, (const char *) output, written);
    js_free(context, output);
    return result;
}

static JSValue js_utf8_encode(JSContext *context,
                              JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
    (void) this_value;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(context, "well-formed string required");
    const uint32_t maximum = 256u * 1024u;
    JSValue units_value = JS_GetPropertyStr(context, argv[0], "length");
    uint32_t units = 0u;
    if (JS_IsException(units_value)
        || JS_ToUint32(context, &units, units_value) < 0) {
        JS_FreeValue(context, units_value);
        return JS_EXCEPTION;
    }
    JS_FreeValue(context, units_value);
    /* Every UTF-16 code unit contributes at least one output byte, so reject
       oversized strings before JS_ToCStringLen can allocate a UTF-8 copy. */
    if (units > maximum)
        return JS_ThrowRangeError(context,
                                  "encoded text exceeds bounded size");
    size_t length = 0u;
    const char *bytes = JS_ToCStringLen(context, &length, argv[0]);
    if (bytes == NULL) return JS_EXCEPTION;
    /* Keep TextEncoder bounded at its existing PSP-facing ceiling. The
       bootstrap first applies the Web IDL USVString conversion, so QuickJS's
       UTF-8 conversion never sees an unmatched surrogate here. */
    if (length > maximum) {
        JS_FreeCString(context, bytes);
        return JS_ThrowRangeError(context,
                                  "encoded text exceeds bounded size");
    }
    JSValue buffer = JS_NewArrayBufferCopy(
        context, (const uint8_t *) bytes, length);
    JS_FreeCString(context, bytes);
    if (JS_IsException(buffer)) return buffer;
    /* QuickJS's public constructor helper expects the optional byteOffset and
       length slots to be present even when they are undefined. Supplying only
       argv[0] lets its ArrayBuffer branch read beyond the argument array. */
    JSValueConst arguments[3] = { buffer, JS_UNDEFINED, JS_UNDEFINED };
    JSValue result = JS_NewTypedArray(
        context, 3, arguments, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(context, buffer);
    return result;
}

/* Prepare a Blob string, measure a well-formed chunk, or write a chunk
   into the admitted Blob. The two-argument form checks the only full
   string allocation (linearizing a rope) before QuickJS can attempt it.
   All other temporary allocations are limited to one 4097-unit chunk. */
static JSValue js_blob_utf8_chunk(JSContext *context, JSValueConst this_value,
                                 int argc, JSValueConst *argv)
{
    (void) this_value;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(context, "well-formed string required");
    JSValue units_value = JS_GetPropertyStr(context, argv[0], "length");
    uint32_t units = 0u;
    if (JS_IsException(units_value)
        || JS_ToUint32(context, &units, units_value) < 0) {
        JS_FreeValue(context, units_value);
        return JS_EXCEPTION;
    }
    JS_FreeValue(context, units_value);
    if (argc == 2) {
        if (JS_VALUE_GET_TAG(argv[0]) == JS_TAG_STRING)
            return JS_DupValue(context, argv[0]);
        double needed = 0.0;
        if (JS_ToFloat64(context, &needed, argv[1]) < 0) return JS_EXCEPTION;
        JSValue charge = JS_NewFloat64(context, needed + (double) units * 2.0);
        JSValue fits = js_blob_fits(context, JS_UNDEFINED, 1, &charge);
        if (JS_IsException(fits)) return fits;
        if (!JS_ToBool(context, fits))
            return JS_ThrowRangeError(context, "Blob exceeds the page's memory");
        return JS_ToString(context, argv[0]);
    }
    if (units > 4097u)
        return JS_ThrowRangeError(context, "Blob text chunk exceeds bounded size");
    size_t length = 0u;
    const char *text = JS_ToCStringLen(context, &length, argv[0]);
    if (text == NULL) return JS_EXCEPTION;
    JSValue result = JS_NewInt32(context, (int32_t) length);
    if (argc >= 3 && length != 0u) {
        size_t capacity = 0u;
        uint32_t offset = 0u;
        uint8_t *out = JS_GetArrayBuffer(context, &capacity, argv[1]);
        if (out == NULL || JS_ToUint32(context, &offset, argv[2]) < 0)
            result = JS_EXCEPTION;
        else if (offset > capacity || length > capacity - offset)
            result = JS_ThrowRangeError(context, "Blob text exceeds allocation");
        else
            memcpy(out + offset, text, length);
    }
    JS_FreeCString(context, text);
    return result;
}

static JSValue js_media_matches(JSContext *context, JSValueConst this_value,
                                int argc, JSValueConst *argv)
{
    (void) this_value;
    if (argc < 6 || !JS_IsString(argv[0])) return JS_FALSE;
    JSValue units_value = JS_GetPropertyStr(context, argv[0], "length");
    uint32_t units = 0;
    int32_t width, height, device_width, device_height;
    double resolution;
    bool valid = !JS_IsException(units_value)
        && JS_ToUint32(context, &units, units_value) >= 0;
    JS_FreeValue(context, units_value);
    if (!valid) return JS_EXCEPTION;
    if (units > 4096) return JS_FALSE;
    if (JS_ToInt32(context, &width, argv[1]) < 0
        || JS_ToInt32(context, &height, argv[2]) < 0
        || JS_ToFloat64(context, &resolution, argv[3]) < 0
        || JS_ToInt32(context, &device_width, argv[4]) < 0
        || JS_ToInt32(context, &device_height, argv[5]) < 0) return JS_EXCEPTION;
    size_t length = 0;
    const char *query = JS_ToCStringLen(context, &length, argv[0]);
    if (!query) return JS_EXCEPTION;
    bool matches = stylesheet_media_matches_viewport(
        width, height, resolution, device_width, device_height, query,
        length);
    JS_FreeCString(context, query);
    return JS_NewBool(context, matches);
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
    if (bridge != NULL && bridge->result != NULL) {
        js_rt_saturating_add_size(&bridge->result->crypto_random_calls, 1u);
        js_rt_saturating_add_size(&bridge->result->crypto_random_bytes,
                                  length);
    }
    return JS_DupValue(context, argv[0]);
}

/* Private structured-clone primitive. JavaScript validates the transferable
   with the captured ArrayBuffer byteLength getter before calling this sink;
   repeat the native class check so a poisoned bootstrap cannot detach an
   unrelated object. A WebAssembly.Memory buffer is never detached (its
   detach key forbids it). With a true second argument nothing is detached:
   the result says whether the buffer may be, so a transfer list can be
   refused before any of it commits. */
static JSValue js_detach_array_buffer(JSContext *context,
                                      JSValueConst this_value,
                                      int argc, JSValueConst *argv)
{
    (void) this_value;
    if (argc < 1) {
        return JS_ThrowTypeError(context, "ArrayBuffer transfer required");
    }
    size_t length = 0u;
    uint8_t *bytes = JS_GetArrayBuffer(context, &length, argv[0]);
    if (bytes == NULL && length != 0u) return JS_EXCEPTION;
    if (bytes == NULL) {
        /* A valid zero-length ArrayBuffer returns NULL without an exception.
           Calling byteLength through the platform's captured intrinsic has
           already established the brand; clear only a native class error. */
        JSValue exception = JS_GetException(context);
        if (!JS_IsUndefined(exception) && !JS_IsNull(exception)) {
            JS_FreeValue(context, exception);
            return JS_ThrowTypeError(context, "ArrayBuffer transfer required");
        }
        JS_FreeValue(context, exception);
    }
    bool detachable = !js_wasm_buffer_aliases_memory(context, argv[0]);
    if (argc > 1 && JS_ToBool(context, argv[1]))
        return JS_NewBool(context, detachable);
    if (!detachable) {
        return JS_ThrowTypeError(
            context, "a WebAssembly.Memory buffer cannot be detached");
    }
    JS_DetachArrayBuffer(context, argv[0]);
    return JS_UNDEFINED;
}

/* Structured clone must reject Proxy objects without executing any of their
   traps. JavaScript reflection cannot perform that brand check safely. */
static JSValue js_is_proxy(JSContext *context, JSValueConst this_value,
                           int argc, JSValueConst *argv)
{
    (void) context;
    (void) this_value;
    return JS_NewBool(context, argc > 0 && JS_IsProxy(argv[0]));
}

static JSValue js_crypto_start(JSContext *context,
                               const TilefinchWebJobParams *params,
                               bool sliced);

/* digest(source, algorithm, sliced): algorithm is a TilefinchWebDigest the
   bootstrap derived from the normalized WebCrypto name. */
static JSValue js_crypto_digest(JSContext *context,
                                JSValueConst this_value,
                                int argc, JSValueConst *argv)
{
    (void) this_value;
    int32_t algorithm = -1;
    if (argc < 2 || JS_ToInt32(context, &algorithm, argv[1]) != 0
        || algorithm < (int32_t) TILEFINCH_WEB_DIGEST_SHA1
        || algorithm > (int32_t) TILEFINCH_WEB_DIGEST_SHA512) {
        return JS_ThrowTypeError(context,
                                 "digest requires a byte view and algorithm");
    }
    size_t length = 0;
    const uint8_t *bytes = NULL;
    DomBridge *bridge = JS_GetContextOpaque(context);
    ScriptRuntime *host = bridge == NULL ? NULL : bridge->host;
    JSContext *candidates[1u + SCRIPT_FRAME_REALM_LIMIT
                             + SCRIPT_WORKER_REALM_LIMIT];
    size_t candidate_count = 0u;
    candidates[candidate_count++] = context;
    if (host != NULL) {
        for (size_t i = 0; i < SCRIPT_FRAME_REALM_LIMIT; i++) {
            JSContext *candidate = host->frame_realms[i].context;
            if (candidate != NULL && candidate != context)
                candidates[candidate_count++] = candidate;
        }
        for (size_t i = 0; i < SCRIPT_WORKER_REALM_LIMIT; i++) {
            JSContext *candidate = host->worker_realms[i].context;
            if (candidate != NULL && candidate != context)
                candidates[candidate_count++] = candidate;
        }
    }
    for (size_t i = 0; i < candidate_count && bytes == NULL; i++) {
        JSContext *candidate = candidates[i];
        size_t raw_length = 0;
        bytes = JS_GetArrayBuffer(candidate, &raw_length, argv[0]);
        if (bytes != NULL) {
            length = raw_length;
            break;
        }
        JSValue exception = JS_GetException(candidate);
        JS_FreeValue(candidate, exception);
        size_t offset = 0, view_length = 0, bytes_per_element = 0;
        JSValue buffer = JS_GetTypedArrayBuffer(
            candidate, argv[0], &offset, &view_length, &bytes_per_element);
        (void) bytes_per_element;
        if (JS_IsException(buffer)) {
            exception = JS_GetException(candidate);
            JS_FreeValue(candidate, exception);
            continue;
        }
        size_t buffer_length = 0;
        const uint8_t *buffer_bytes = JS_GetArrayBuffer(
            candidate, &buffer_length, buffer);
        bool valid = (buffer_bytes != NULL || buffer_length == 0)
            && offset <= buffer_length
            && view_length <= buffer_length - offset;
        if (valid) {
            bytes = buffer_bytes;
            if (offset != 0u) bytes += offset;
            length = view_length;
        }
        JS_FreeValue(candidate, buffer);
        if (!valid) {
            exception = JS_GetException(candidate);
            JS_FreeValue(candidate, exception);
        }
    }
    if (bytes == NULL && length == 0u)
        return JS_ThrowTypeError(context, "BufferSource required");
    if (length > SCRIPT_CRYPTO_DIGEST_INPUT_LIMIT) {
        return JS_ThrowRangeError(context,
                                  "digest input exceeds bounded size");
    }
    /* compat.js asks for a job (the third argument) when subtle-crypto.js
       can run one; a digest over a slice's work then returns one. */
    TilefinchWebJobParams params = {
        .kind = TILEFINCH_WEB_JOB_DIGEST,
        .hash = (TilefinchWebDigest) algorithm,
        .a = length == 0 ? NULL : bytes, .a_length = length
    };
    JSValue result = js_crypto_start(context, &params,
                                     argc > 2 && JS_ToBool(context, argv[2]));
    if (JS_IsNull(result))
        return JS_ThrowInternalError(context, "digest failed");
    if (!JS_IsException(result) && bridge != NULL && bridge->result != NULL) {
        js_rt_saturating_add_size(&bridge->result->crypto_digest_calls, 1u);
        js_rt_saturating_add_size(&bridge->result->crypto_digest_bytes,
                                  length);
    }
    return result;
}

/* A CryptoKey's secret: an object with no prototype and no properties
   that only compat.js's private key state holds, so page script never
   reaches the bytes. The finalizer zeroes them when the key is
   collected. */
typedef struct {
    size_t length;
    uint8_t bytes[];
} JsCryptoKeyData;

static JSClassID js_crypto_key_class_id;

static void js_crypto_key_finalizer(JSRuntime *runtime, JSValue value)
{
    JsCryptoKeyData *key = JS_GetOpaque(value, js_crypto_key_class_id);
    if (key == NULL) return;
    tilefinch_web_crypto_zeroize(key->bytes, key->length);
    js_free_rt(runtime, key);
}

static JSClassDef js_crypto_key_class = {
    .class_name = "CryptoKeyData",
    .finalizer = js_crypto_key_finalizer,
};

static bool js_crypto_class(JSRuntime *runtime, JSClassID *id,
                            const JSClassDef *definition)
{
    if (*id == 0) JS_NewClassID(id);
    return JS_IsRegisteredClass(runtime, *id)
        || JS_NewClass(runtime, *id, definition) >= 0;
}

/* The bootstrap passes its own Uint8Array copies of every BufferSource. */
static bool js_crypto_bytes(JSContext *context, JSValueConst value,
                            const uint8_t **bytes, size_t *length)
{
    size_t offset = 0, view_length = 0, element = 0, buffer_length = 0;
    JSValue buffer = JS_GetTypedArrayBuffer(context, value, &offset,
                                            &view_length, &element);
    if (JS_IsException(buffer)) return false;
    const uint8_t *data = JS_GetArrayBuffer(context, &buffer_length, buffer);
    JS_FreeValue(context, buffer);
    if ((data == NULL && buffer_length != 0) || offset > buffer_length
        || view_length > buffer_length - offset) {
        JS_ThrowTypeError(context, "byte view required");
        return false;
    }
    *bytes = data == NULL ? NULL : data + offset;
    *length = view_length;
    return true;
}

/* key(bytes) makes key data from a Uint8Array, or returns null when it
   holds more than TILEFINCH_WEB_CRYPTO_INPUT_LIMIT bytes; key(data) copies
   the bytes back out as an ArrayBuffer (exportKey); key(data, true)
   duplicates the opaque native allocation without exposing any bytes. */
static JSValue js_crypto_key(JSContext *context, JSValueConst this_value,
                             int argc, JSValueConst *argv)
{
    (void) this_value;
    if (argc < 1) return JS_ThrowTypeError(context, "key data required");
    JsCryptoKeyData *key = JS_GetOpaque(argv[0], js_crypto_key_class_id);
    const uint8_t *bytes = NULL;
    size_t length = 0;
    if (key != NULL) {
        if (argc < 2 || !JS_IsBool(argv[1]) || !JS_ToBool(context, argv[1]))
            return JS_NewArrayBufferCopy(context, key->bytes, key->length);
        bytes = key->bytes;
        length = key->length;
    } else if (!js_crypto_bytes(context, argv[0], &bytes, &length))
        return JS_EXCEPTION;
    if (length > TILEFINCH_WEB_CRYPTO_INPUT_LIMIT) return JS_NULL;
    if (!js_crypto_class(JS_GetRuntime(context), &js_crypto_key_class_id,
                         &js_crypto_key_class)) return JS_EXCEPTION;
    JSValue object = JS_NewObjectProtoClass(context, JS_NULL,
                                            js_crypto_key_class_id);
    if (JS_IsException(object)) return object;
    key = js_malloc(context, sizeof(*key) + length + 1u);
    if (key == NULL) {
        JS_FreeValue(context, object);
        return JS_EXCEPTION;
    }
    key->length = length;
    if (length != 0) memcpy(key->bytes, bytes, length);
    JS_SetOpaque(object, key);
    return object;
}

typedef enum {
    JS_CRYPTO_HMAC = 0,
    JS_CRYPTO_GCM_ENCRYPT,
    JS_CRYPTO_GCM_DECRYPT,
    JS_CRYPTO_CBC_ENCRYPT,
    JS_CRYPTO_CBC_DECRYPT,
    JS_CRYPTO_CTR,
    JS_CRYPTO_PBKDF2,
    JS_CRYPTO_HKDF,
    JS_CRYPTO_DIGEST,
    JS_CRYPTO_STEP
} JsCryptoOperation;

/* A slice's start and length: with the task boundaries' 16 ms advance
   limit, about two slices run per turn before input and presentation get
   one. */
typedef struct {
    uint64_t started, length;
} JsCryptoSlice;

static bool js_crypto_slice_more(void *context)
{
    const JsCryptoSlice *slice = context;
    uint64_t now = js_rt_monotonic_time_ns();
    return now >= slice->started && now - slice->started < slice->length;
}

static JsCryptoSlice js_crypto_slice(JSContext *context)
{
    JsCryptoSlice slice = {js_rt_monotonic_time_ns(), SCRIPT_CRYPTO_SLICE_NS};
#ifdef JS_RT_TEST_INSTRUMENTS
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge != NULL && bridge->host != NULL
        && bridge->host->crypto_slice_ns != 0)
        slice.length = bridge->host->crypto_slice_ns;
#else
    (void) context;
#endif
    return slice;
}

/* An operation run in slices (src/web_crypto.c), as an object with no
   prototype or properties that only the bootstrap's job records hold. It
   owns copies of the bytes it reads, so the caller's may change or be
   zeroed, and its output from the first slice on; the finalizer zeroes
   and frees what it still holds. */
typedef struct {
    TilefinchWebJob *job;
    uint8_t *copies, *output;
    size_t copies_length, capacity;
} JsCryptoJob;

static JSClassID js_crypto_job_class_id;

static void js_crypto_job_release(JSRuntime *runtime, JsCryptoJob *job)
{
    if (job->job != NULL) {
        tilefinch_web_job_end(job->job);
        js_free_rt(runtime, job->job);
    }
    tilefinch_web_crypto_zeroize(job->copies, job->copies_length);
    js_free_rt(runtime, job->copies);
    tilefinch_web_crypto_zeroize(job->output, job->capacity);
    js_free_rt(runtime, job->output);
    *job = (JsCryptoJob) {0};
}

static void js_crypto_job_finalizer(JSRuntime *runtime, JSValue value)
{
    JsCryptoJob *job = JS_GetOpaque(value, js_crypto_job_class_id);
    if (job == NULL) return;
    js_crypto_job_release(runtime, job);
    js_free_rt(runtime, job);
}

static JSClassDef js_crypto_job_class = {
    .class_name = "CryptoJob",
    .finalizer = js_crypto_job_finalizer,
};

static void js_crypto_free_output(JSRuntime *runtime, void *opaque,
                                  void *bytes)
{
    (void) opaque;
    js_free_rt(runtime, bytes);
}

/* A job object for params. With copy, the job reads copies of the bytes,
   so it may outlive the call; without, it must end within it. null for an
   OperationError. */
static JSValue js_crypto_new_job(JSContext *context,
                                 TilefinchWebJobParams params,
                                 size_t capacity, bool copy)
{
    JSValue object = JS_NewObjectProtoClass(context, JS_NULL,
                                            js_crypto_job_class_id);
    if (JS_IsException(object)) return object;
    JsCryptoJob *job = js_mallocz(context, sizeof(*job));
    if (job == NULL) {
        JS_FreeValue(context, object);
        return JS_EXCEPTION;
    }
    JS_SetOpaque(object, job);
    job->capacity = capacity != 0 ? capacity : 1u;
    const uint8_t **sources[] = {&params.key, &params.a, &params.b,
                                 &params.c};
    const size_t lengths[] = {params.key_length, params.a_length,
                              params.b_length, params.c_length};
    if (copy) {
        /* Each is at most a few MiB, so the sum cannot wrap. */
        for (size_t i = 0; i < 4u; i++) job->copies_length += lengths[i];
        job->copies = js_malloc(context, job->copies_length + 1u);
        if (job->copies == NULL) goto fail;
        uint8_t *at = job->copies;
        for (size_t i = 0; i < 4u; i++) {
            if (lengths[i] == 0) continue;
            memcpy(at, *sources[i], lengths[i]);
            *sources[i] = at;
            at += lengths[i];
        }
    }
    /* Zeroed, a job holds nothing to free. */
    job->job = js_mallocz(context, tilefinch_web_job_size());
    if (job->job == NULL) goto fail;
    if (tilefinch_web_job_begin(job->job, &params)) return object;
    JS_FreeValue(context, object);
    return JS_NULL;
fail:
    JS_FreeValue(context, object);
    return JS_EXCEPTION;
}

/* Runs a slice of job (all of it when whole): false while work remains,
   the output as an ArrayBuffer, or null for an OperationError. A job
   that settled holds nothing more. */
static JSValue js_crypto_step(JSContext *context, JsCryptoJob *job,
                              bool whole)
{
    if (job->job == NULL) return JS_NULL;
    if (job->output == NULL) {
        job->output = js_malloc(context, job->capacity);
        if (job->output == NULL) return JS_EXCEPTION;
    }
    JsCryptoSlice slice = js_crypto_slice(context);
    int status = tilefinch_web_job_run(job->job, job->output,
                                       whole ? NULL : js_crypto_slice_more,
                                       &slice);
    if (status == 0) return JS_FALSE;
    JSValue result = JS_NULL;
    if (status > 0) {
        /* The buffer takes the output without a copy. */
        result = JS_NewArrayBuffer(context, job->output,
                                   tilefinch_web_job_length(job->job),
                                   js_crypto_free_output, NULL, false);
        if (!JS_IsException(result)) job->output = NULL;
    }
    js_crypto_job_release(JS_GetRuntime(context), job);
    return result;
}

/* The output of params: in the call when its work fits a slice (or
   sliced is false), otherwise a job object to step. */
static JSValue js_crypto_start(JSContext *context,
                               const TilefinchWebJobParams *params,
                               bool sliced)
{
    size_t capacity = 0, work = 0;
    if (!tilefinch_web_job_plan(params, &capacity, &work)) return JS_NULL;
    if (!js_crypto_class(JS_GetRuntime(context), &js_crypto_job_class_id,
                         &js_crypto_job_class)) return JS_EXCEPTION;
    sliced = sliced && work > TILEFINCH_WEB_CRYPTO_SLICE_WORK;
    JSValue object = js_crypto_new_job(context, *params, capacity, sliced);
    if (sliced || JS_IsException(object) || JS_IsNull(object))
        return object;
    JSValue result = js_crypto_step(
        context, JS_GetOpaque(object, js_crypto_job_class_id), true);
    JS_FreeValue(context, object);
    return result;
}

/* run(operation, key data, a, b, c, d) -> ArrayBuffer, a job, or null for
   an OperationError. The bootstrap checks the WebCrypto parameter rules;
   the arguments by operation are
     HMAC: hash, data
     GCM: iv, additional data, data, tag bytes
     CBC: iv (16 bytes), data
     CTR: counter block, counter bits, data
     PBKDF2: hash, salt, iterations, job
     HKDF: hash, salt, info, output bytes.
   An operation whose work is over a slice
   (TILEFINCH_WEB_CRYPTO_SLICE_WORK) returns a job instead of its output,
   and run(step, job) runs its next slice: false while work remains, then
   its output or null. PBKDF2 runs one slice of its derivation and answers
   whether it is complete: the job (a Uint8Array the bootstrap holds) is
   the derivation's state (TilefinchWebPbkdf2State), then its output. */
static JSValue js_crypto_run(JSContext *context, JSValueConst this_value,
                             int argc, JSValueConst *argv)
{
    (void) this_value;
    int32_t operation = -1;
    uint32_t numbers[3] = {0, 0, 0};
    /* The native row declares six arguments, so QuickJS pads argv. */
    if (argc < 2 || JS_ToInt32(context, &operation, argv[0]) < 0
        || operation < JS_CRYPTO_HMAC || operation > JS_CRYPTO_STEP
        || operation == JS_CRYPTO_DIGEST)
        return JS_ThrowTypeError(context, "invalid crypto operation");
    if (operation == JS_CRYPTO_STEP) {
        JsCryptoJob *job = JS_GetOpaque(argv[1], js_crypto_job_class_id);
        if (job == NULL)
            return JS_ThrowTypeError(context, "invalid crypto operation");
        return js_crypto_step(context, job, false);
    }
    JsCryptoKeyData *key = JS_GetOpaque(argv[1], js_crypto_key_class_id);
    if (key == NULL)
        return JS_ThrowTypeError(context, "invalid crypto operation");
    const uint8_t *bytes[3] = {NULL, NULL, NULL};
    size_t lengths[3] = {0, 0, 0};
    /* Which of a..d are byte views (bit set); the others are numbers,
       taken in order. */
    static const uint8_t views[] = {
        0x2, 0x7, 0x7, 0x3, 0x3, 0x5, 0xa, 0x6
    };
    size_t view = 0, number = 0;
    for (int slot = 0; slot < 4; slot++) {
        JSValueConst value = argv[2 + slot];
        if (views[operation] & (1u << slot)) {
            if (view < 3 && !js_crypto_bytes(context, value, &bytes[view],
                                             &lengths[view]))
                return JS_EXCEPTION;
            size_t overhead = 0u;
            if (operation == JS_CRYPTO_PBKDF2 && view == 1)
                overhead = TILEFINCH_WEB_PBKDF2_STATE_BYTES;
            if ((operation == JS_CRYPTO_GCM_DECRYPT && view == 2)
                || (operation == JS_CRYPTO_CBC_DECRYPT && view == 1))
                overhead = 16u;
            if (view < 3 && lengths[view] >
                    TILEFINCH_WEB_CRYPTO_INPUT_LIMIT + overhead)
                return JS_NULL;
            view++;
        } else if (number < 3 && !JS_IsUndefined(value)) {
            if (JS_ToUint32(context, &numbers[number], value) < 0)
                return JS_EXCEPTION;
            number++;
        } else number++;
    }
    TilefinchWebDigest hash = (TilefinchWebDigest) numbers[0];
    if (operation == JS_CRYPTO_PBKDF2) {
        if (lengths[1] <= TILEFINCH_WEB_PBKDF2_STATE_BYTES) return JS_NULL;
        /* The bootstrap's own buffer, so it may be written. */
        uint8_t *job = (uint8_t *) bytes[1];
        TilefinchWebPbkdf2State state;
        memcpy(&state, job, sizeof(state));
        JsCryptoSlice slice = js_crypto_slice(context);
        int status = tilefinch_web_pbkdf2_slice(
            hash, key->bytes, key->length, bytes[0], lengths[0], numbers[1],
            job + sizeof(state), lengths[1] - sizeof(state), &state,
            js_crypto_slice_more, &slice);
        memcpy(job, &state, sizeof(state));
        tilefinch_web_crypto_zeroize(&state, sizeof(state));
        return status < 0 ? JS_NULL : JS_NewBool(context, status > 0);
    }
    /* The byte views are a, b and c in order; HKDF's output length is its
       second number, the tag or counter bits the others' first. */
    TilefinchWebJobParams params = {
        .kind = (TilefinchWebJobKind) operation, .hash = hash,
        .number = operation == JS_CRYPTO_HKDF ? numbers[1] : numbers[0],
        .key = key->bytes, .key_length = key->length,
        .a = bytes[0], .a_length = lengths[0],
        .b = bytes[1], .b_length = lengths[1],
        .c = bytes[2], .c_length = lengths[2]
    };
    return js_crypto_start(context, &params, true);
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

enum {
    JS_OPFS_STAT = 0,
    JS_OPFS_CREATE,
    JS_OPFS_READ,
    JS_OPFS_WRITE,
    JS_OPFS_REMOVE,
    JS_OPFS_LIST,
    JS_OPFS_LOCK,
    JS_OPFS_UNLOCK
};

static JSValue js_opfs_result(JSContext *context, BrowserOpfsResult status)
{
    JSValue result = JS_NewArray(context);
    if (!JS_IsException(result)
        && JS_DefinePropertyValueUint32(
               context, result, 0, JS_NewInt32(context, status),
               JS_PROP_C_W_E) < 0) {
        JS_FreeValue(context, result);
        return JS_EXCEPTION;
    }
    return result;
}

/* One private command sink keeps the OPFS surface lazy while the native
   boundary remains authoritative for origin, secure-context, site-data and
   captive-portal policy. JavaScript supplies only a canonical path and
   bounded payload, never an origin. */
static JSValue js_opfs_command(JSContext *context, JSValueConst this_value,
                               int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    int32_t command = -1;
    if (argc < 2)
        return JS_ThrowTypeError(context, "OPFS command and path required");
    if (JS_ToInt32(context, &command, argv[0]) < 0) return JS_EXCEPTION;
    if (command == JS_OPFS_UNLOCK) {
        int64_t lease = 0;
        if (argc > 2 && JS_ToInt64(context, &lease, argv[2]) < 0)
            return JS_EXCEPTION;
        if (lease > 0 && bridge != NULL)
            browser_session_opfs_unlock(bridge->session, bridge, (uint64_t) lease);
        return JS_UNDEFINED;
    }
    const char *path = JS_ToCString(context, argv[1]);
    if (path == NULL) return JS_EXCEPTION;
    bool authorized = bridge != NULL && bridge->session != NULL
        && !bridge->opaque_origin && bridge->document_url != NULL
        && tilefinch_url_potentially_trustworthy(bridge->document_url)
        && browser_session_site_data_allowed(bridge->session)
        && !browser_session_captive_portal_active(bridge->session);
    if (!authorized) {
        JS_FreeCString(context, path);
        return js_opfs_result(context, BROWSER_OPFS_UNAVAILABLE);
    }
    BrowserOpfsResult status = BROWSER_OPFS_INVALID_PATH;
    BrowserOpfsView view = {0};
    JSValue result = JS_UNDEFINED;
    if (command == JS_OPFS_STAT) {
        status = browser_session_opfs_stat(
            bridge->session, bridge->document_url, path, &view);
        result = js_opfs_result(context, status);
        if (status == BROWSER_OPFS_OK && !JS_IsException(result)) {
            if (JS_DefinePropertyValueUint32(
                    context, result, 1,
                    JS_NewInt32(context, (int32_t) view.kind),
                    JS_PROP_C_W_E) < 0
                || JS_DefinePropertyValueUint32(
                       context, result, 2,
                       JS_NewInt64(context, (int64_t) view.data_length),
                       JS_PROP_C_W_E) < 0
                || JS_DefinePropertyValueUint32(
                       context, result, 3,
                       JS_NewInt64(context, view.last_modified_ms),
                       JS_PROP_C_W_E) < 0
                || JS_DefinePropertyValueUint32(
                       context, result, 4,
                       JS_NewInt64(context, (int64_t) view.generation),
                       JS_PROP_C_W_E) < 0) {
                JS_FreeValue(context, result);
                result = JS_EXCEPTION;
            }
        }
    } else if (command == JS_OPFS_CREATE) {
        int32_t kind = -1;
        if (argc > 2) {
            if (JS_ToInt32(context, &kind, argv[2]) < 0) {
                JS_FreeCString(context, path);
                return JS_EXCEPTION;
            }
            status = browser_session_opfs_create(
                bridge->session, bridge->document_url, path,
                (BrowserOpfsKind) kind);
        }
        result = js_opfs_result(context, status);
    } else if (command == JS_OPFS_READ) {
        status = browser_session_opfs_read(
            bridge->session, bridge->document_url, path, &view);
        if (status == BROWSER_OPFS_OK && view.kind != BROWSER_OPFS_FILE)
            status = BROWSER_OPFS_TYPE_MISMATCH;
        result = js_opfs_result(context, status);
        if (status == BROWSER_OPFS_OK && !JS_IsException(result)) {
            JSValue data = JS_NewArrayBufferCopy(
                context, view.data == NULL
                    ? (const uint8_t *) "" : view.data,
                view.data_length);
            if (JS_IsException(data)
                || JS_DefinePropertyValueUint32(
                       context, result, 1, data, JS_PROP_C_W_E) < 0
                || JS_DefinePropertyValueUint32(
                       context, result, 2,
                       JS_NewInt64(context, view.last_modified_ms),
                       JS_PROP_C_W_E) < 0
                || JS_DefinePropertyValueUint32(
                       context, result, 3,
                       JS_NewInt64(context, (int64_t) view.generation),
                       JS_PROP_C_W_E) < 0) {
                if (JS_IsException(data)) JS_FreeValue(context, data);
                JS_FreeValue(context, result);
                result = JS_EXCEPTION;
            }
        }
    } else if (command == JS_OPFS_WRITE) {
        const uint8_t *bytes = NULL;
        size_t length = 0;
        JSValue owner = JS_UNDEFINED;
        int64_t expected_generation = 0, shared_lease = 0;
        if (argc < 4) {
            JS_FreeCString(context, path);
            return JS_ThrowTypeError(context, "OPFS write requires bytes");
        }
        /* Coerce before borrowing the bytes: a valueOf() could otherwise
           detach the buffer under the pointer that is written to storage. */
        if (JS_ToInt64(context, &expected_generation, argv[3]) < 0
            || (argc > 4 && JS_ToInt64(context, &shared_lease, argv[4]) < 0)) {
            JS_FreeCString(context, path);
            return JS_EXCEPTION;
        }
        if (!js_compression_buffer_source(
                context, argv[2], &bytes, &length, &owner)) {
            JS_FreeCString(context, path);
            return JS_ThrowTypeError(context, "OPFS write requires bytes");
        }
        if (shared_lease > 0) {
            status = browser_session_opfs_commit_shared(bridge->session,
                bridge->document_url, path, bridge, (uint64_t) shared_lease,
                bytes, length);
        } else {
            status = browser_session_opfs_stat(
                bridge->session, bridge->document_url, path, &view);
            if (status == BROWSER_OPFS_OK) {
                if (expected_generation <= 0 || view.generation != (uint64_t) expected_generation)
                    status = BROWSER_OPFS_STALE;
                else {
                    status = browser_session_opfs_write(
                        bridge->session, bridge->document_url, path, bytes, length);
                    if (status == BROWSER_OPFS_OK)
                        status = browser_session_opfs_stat(
                            bridge->session, bridge->document_url, path, &view);
                }
            }
        }
        JS_FreeValue(context, owner);
        result = js_opfs_result(context, status);
        if (shared_lease == 0 && status == BROWSER_OPFS_OK && !JS_IsException(result)
            && JS_DefinePropertyValueUint32(
                   context, result, 1,
                   JS_NewInt64(context, (int64_t) view.generation),
                   JS_PROP_C_W_E) < 0) {
            JS_FreeValue(context, result);
            result = JS_EXCEPTION;
        }
    } else if (command == JS_OPFS_LOCK) {
        uint64_t lease = 0;
        status = browser_session_opfs_lock(
            bridge->session, bridge->document_url, path, bridge,
            argc > 2 && JS_ToBool(context, argv[2]) > 0, &lease);
        result = js_opfs_result(context, status);
        if (status == BROWSER_OPFS_OK
            && (JS_IsException(result) || JS_DefinePropertyValueUint32(
                    context, result, 1, JS_NewInt64(context, (int64_t) lease),
                    JS_PROP_C_W_E) < 0)) {
            browser_session_opfs_unlock(bridge->session, bridge, lease);
            JS_FreeValue(context, result);
            result = JS_EXCEPTION;
        }
    } else if (command == JS_OPFS_REMOVE) {
        int recursive = argc > 2 ? JS_ToBool(context, argv[2]) : 0;
        if (recursive < 0) {
            JS_FreeCString(context, path);
            return JS_EXCEPTION;
        }
        status = browser_session_opfs_remove(
            bridge->session, bridge->document_url, path,
            recursive > 0);
        result = js_opfs_result(context, status);
    } else if (command == JS_OPFS_LIST) {
        BrowserOpfsView parent_view = {0};
        status = browser_session_opfs_stat(
            bridge->session, bridge->document_url, path, &parent_view);
        if (status == BROWSER_OPFS_OK
            && parent_view.kind != BROWSER_OPFS_DIRECTORY)
            status = BROWSER_OPFS_TYPE_MISMATCH;
        result = js_opfs_result(context, status);
        if (status == BROWSER_OPFS_OK && !JS_IsException(result)) {
            uint32_t output = 1;
            for (size_t index = 0;
                 index < BROWSER_SITE_STORAGE_SITE_ITEMS; index++) {
                const char *name = NULL;
                BrowserOpfsResult child = browser_session_opfs_child(
                    bridge->session, bridge->document_url, path,
                    index, &name, &view);
                if (child == BROWSER_OPFS_NOT_FOUND) break;
                if (child != BROWSER_OPFS_OK || name == NULL) {
                    status = child;
                    break;
                }
                if (JS_DefinePropertyValueUint32(
                        context, result, output++,
                        JS_NewString(context, name), JS_PROP_C_W_E) < 0
                    || JS_DefinePropertyValueUint32(
                           context, result, output++,
                           JS_NewInt32(context, (int32_t) view.kind),
                           JS_PROP_C_W_E) < 0) {
                    if (JS_HasException(context)) {
                        JS_FreeValue(context, result);
                        JS_FreeCString(context, path);
                        return JS_EXCEPTION;
                    }
                    status = BROWSER_OPFS_QUOTA_EXCEEDED;
                    break;
                }
            }
            if (status != BROWSER_OPFS_OK) {
                JS_FreeValue(context, result);
                result = js_opfs_result(context, status);
            }
        }
    } else {
        result = js_opfs_result(context, BROWSER_OPFS_INVALID_PATH);
    }
    JS_FreeCString(context, path);
    return result;
}

/* Blob-URL Fetch bypasses the network scheduler, so it cannot use the
   completion-time SRI check in event_loop.inc. Keep that local fast path
   fail-closed through the same bounded native verifier instead of silently
   accepting integrity metadata it advertised. */
static JSValue js_resource_integrity_verify(
    JSContext *context, JSValueConst this_value,
    int argc, JSValueConst *argv)
{
    (void) this_value;
    if (argc < 2) {
        return JS_ThrowTypeError(
            context, "integrity metadata and BufferSource required");
    }
    size_t metadata_length = 0;
    const char *metadata = JS_ToCStringLen(
        context, &metadata_length, argv[0]);
    if (metadata == NULL) return JS_EXCEPTION;
    const uint8_t *bytes = NULL;
    size_t length = 0;
    JSValue owner = JS_UNDEFINED;
    bool valid = metadata_length <= TILEFINCH_INTEGRITY_METADATA_LIMIT
        && strlen(metadata) == metadata_length
        && js_compression_buffer_source(
            context, argv[1], &bytes, &length, &owner);
    TilefinchIntegrityResult result = TILEFINCH_INTEGRITY_INVALID;
    if (valid) {
        result = tilefinch_resource_integrity_verify(
            metadata, metadata_length, bytes, length);
    }
    bool threw = JS_HasException(context);
    JS_FreeValue(context, owner);
    JS_FreeCString(context, metadata);
    if (threw) return JS_EXCEPTION;
    return JS_NewInt32(context, (int32_t) result);
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

/* Base64 of `count` bytes from `source`, written to `output`. With
   `latin1_utf8`, `source` is validated UTF-8 of Latin-1 code units (one
   byte below 0x80, two bytes otherwise) and each unit is one input byte. */
static void base64_encode_into(char *output, const unsigned char *source,
                               size_t count, bool latin1_utf8)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t at = 0, destination = 0;
#define TILEFINCH_BASE64_NEXT_BYTE()                                       \
    (latin1_utf8 && source[at] >= 0x80u                                    \
         ? (at += 2u, (uint32_t) (((source[at - 2u] & 0x03u) << 6)         \
                                  | (source[at - 1u] & 0x3Fu)))            \
         : (uint32_t) source[at++])
    for (size_t remaining = count; remaining != 0;) {
        uint32_t a = TILEFINCH_BASE64_NEXT_BYTE();
        bool has_b = remaining > 1u, has_c = remaining > 2u;
        uint32_t b = has_b ? TILEFINCH_BASE64_NEXT_BYTE() : 0u;
        uint32_t c = has_c ? TILEFINCH_BASE64_NEXT_BYTE() : 0u;
        remaining -= has_c ? 3u : has_b ? 2u : 1u;
        uint32_t value = (a << 16) | (b << 8) | c;
        output[destination++] = alphabet[(value >> 18) & 63u];
        output[destination++] = alphabet[(value >> 12) & 63u];
        output[destination++] = has_b ? alphabet[(value >> 6) & 63u] : '=';
        output[destination++] = has_c ? alphabet[value & 63u] : '=';
    }
#undef TILEFINCH_BASE64_NEXT_BYTE
}

/* The base64 string of `count` input bytes. No fixed length cap: the
   result is one string charged to the realm's heap, so the realm's memory
   limit (and the engine's string length limit) bound it, and a refusal is
   an ordinary catchable exception. The output is written in place, with
   no intermediate copy. */
static JSValue base64_encoded_string(JSContext *context,
                                     const unsigned char *source,
                                     size_t count, bool latin1_utf8)
{
    if (count > (SIZE_MAX / 4u) * 3u - 2u)
        return JS_ThrowRangeError(context, "Invalid string length");
    size_t output_length = ((count + 2u) / 3u) * 4u;
    uint8_t *output = NULL;
    JSValue result = JS_NewStringLenLatin1UninitializedPortable(
        context, output_length, &output);
    if (JS_IsException(result)) return result;
    if (output_length != 0)
        base64_encode_into((char *) output, source, count, latin1_utf8);
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
        || length > buffer_length - offset) {
        JS_FreeValue(context, buffer);
        return JS_ThrowTypeError(context, "byte array required");
    }
    JSValue result = base64_encoded_string(context, bytes + offset, length,
                                           false);
    JS_FreeValue(context, buffer);
    return result;
}

/* btoa's work: the string's code units as bytes, refused at the first unit
   above 255 (returned as its index, so script can report it exactly as
   before), otherwise base64. The engine lends an ASCII string's own bytes,
   so the only allocation is the result; other strings cost their UTF-8
   copy, which is read in place. */
static JSValue js_base64_encode_latin1(JSContext *context,
                                       JSValueConst this_value,
                                       int argc, JSValueConst *argv)
{
    (void) this_value;
    if (argc < 1) return JS_ThrowTypeError(context, "string required");
    size_t utf8_length = 0;
    const char *utf8 = JS_ToCStringLen(context, &utf8_length, argv[0]);
    if (utf8 == NULL) return JS_EXCEPTION;
    const unsigned char *source = (const unsigned char *) utf8;
    size_t units = 0;
    int64_t bad = -1;
    for (size_t at = 0; at < utf8_length; units++) {
        unsigned char lead = source[at];
        if (lead < 0x80u) {
            at++;
        } else if ((lead == 0xC2u || lead == 0xC3u)
                   && utf8_length - at > 1u
                   && (source[at + 1u] & 0xC0u) == 0x80u) {
            at += 2u;
        } else {
            bad = (int64_t) units;
            break;
        }
    }
    JSValue result = bad >= 0 ? JS_NewInt64(context, bad)
        : base64_encoded_string(context, source, units,
                                units != utf8_length);
    JS_FreeCString(context, utf8);
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

/* atob's work. Like the encoders it has no fixed length cap: valid input
   is ASCII, which the engine lends in place, so the only allocation is the
   result string (three quarters of the input), charged to and bounded by
   the realm's heap. */
static JSValue js_base64_decode_string(JSContext *context,
                                       JSValueConst this_value,
                                       int argc, JSValueConst *argv)
{
    (void) this_value;
    if (argc < 1) return JS_ThrowTypeError(context, "base64 string required");
    size_t length = 0;
    const char *input = JS_ToCStringLen(context, &length, argv[0]);
    if (input == NULL) return JS_EXCEPTION;
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
    JSValue result = JS_NewStringLenLatin1UninitializedPortable(
        context, output_capacity, &output);
    if (JS_IsException(result)) {
        JS_FreeCString(context, input);
        return JS_EXCEPTION;
    }
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
        JS_FreeValue(context, result);
        result = JS_ThrowTypeError(context, "invalid base64 input");
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

/* A refused task checkpoint is host control flow, not an author-catchable
   API error. Set the flag after throwing: Bellard's throw clears it first. */
JSValue js_rt_throw_task_interruption(JSContext *context, const char *message)
{
    JSValue exception = JS_ThrowInternalError(context, "%s", message);
    JS_SetUncatchableException(context, true);
    return exception;
}

/* Short callbacks can finish before VM polls. The bootstrap checks the
   single task-entry boundary so native work still observes cancellation. */
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
    return js_rt_throw_task_interruption(context, message);
}

static JSValue js_microtask_job(JSContext *context, int argc,
                                JSValueConst *argv)
{
    (void) argc;
    return JS_Call(context, argv[0], JS_UNDEFINED, 0, NULL);
}

static JSValue js_enqueue_microtask(JSContext *context,
                                   JSValueConst this_value, int argc,
                                   JSValueConst *argv)
{
    (void) this_value;
    if (argc != 1 || !JS_IsFunction(context, argv[0]))
        return JS_ThrowTypeError(context, "callback required");
    /* Use the ordinary Budget-owned job queue, without creating an author
       Promise or consulting its constructor/@@species. The bootstrap wrapper
       reports callback exceptions as uncaught errors, not rejections. */
    if (JS_EnqueueJob(context, js_microtask_job, 1, argv) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue js_queue_checkpoint_continuation(
    JSContext *context, JSValueConst this_value, int argc,
    JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || bridge->host == NULL)
        return JS_ThrowInternalError(context, "script runtime is unavailable");
    if (argc != 1 || !JS_IsFunction(context, argv[0]))
        return JS_ThrowTypeError(context, "callback required");
    ScriptRuntime *runtime = bridge->host;
    if (runtime->checkpoint_continuation_count
            >= SCRIPT_CHECKPOINT_CONTINUATION_LIMIT) {
        return JS_ThrowRangeError(
            context, "checkpoint continuation limit reached");
    }
    size_t tail = ((size_t) runtime->checkpoint_continuation_head
                   + runtime->checkpoint_continuation_count)
                % SCRIPT_CHECKPOINT_CONTINUATION_LIMIT;
    runtime->checkpoint_continuations[tail] =
        JS_DupValue(context, argv[0]);
    runtime->checkpoint_continuation_count++;
    return JS_UNDEFINED;
}

/* Event-handler attributes are script, but they are not eval().  Compile the
   exact attribute body through this host-only path so CSP's unsafe-eval gate
   can remain closed while script-src's unsafe-inline decision is still
   honoured.  The bootstrap calls this lazily on first dispatch and retains a
   bounded number of compiled handlers.

   HTML runs the body with the element, its form owner (form-associated
   elements only) and the document on the scope chain ahead of the global
   object, so `onload="media='all'"` assigns the link's media.  Arguments 1..3
   are those scope objects, outermost first (document, form, element); each
   non-object is skipped.  With scopes present the body is compiled inside one
   sloppy enclosing function,

       (function(){with(this[0])with(this[1])return function(event){BODY}})

   which is called once with the array of scope objects as `this`.  `this` is
   a keyword, never an identifier lookup, so no helper name is visible to (or
   shadowable by) the handler body or the scope objects, and the handler's
   own `this` stays the dispatch receiver.  `with` lives only in the
   enclosing function, so a body that opts into strict mode stays valid. */
/* A compile's exception is the heap's refusal: QuickJS's out-of-memory
   InternalError (its own message property, which no page code sets), or
   null when even that could not be allocated. A compile runs none of the
   handler's code, so neither comes from the page. */
static bool inline_handler_compile_refused(JSContext *context,
                                           JSValueConst error)
{
    if (JS_IsNull(error)) return true;
    if (!JS_IsError(context, error)) return false;
    JSPropertyDescriptor descriptor;
    JSAtom atom = JS_NewAtom(context, "message");
    bool refused = false;
    if (atom != JS_ATOM_NULL
        && JS_GetOwnProperty(context, &descriptor, error, atom) > 0) {
        size_t length = 0;
        const char *message = JS_IsString(descriptor.value)
            ? JS_ToCStringLen(context, &length, descriptor.value) : NULL;
        refused = message != NULL && length == 13u
            && memcmp(message, "out of memory", 13u) == 0;
        if (message != NULL) JS_FreeCString(context, message);
        JS_FreeValue(context, descriptor.value);
        JS_FreeValue(context, descriptor.getter);
        JS_FreeValue(context, descriptor.setter);
    }
    if (atom != JS_ATOM_NULL) JS_FreeAtom(context, atom);
    return refused;
}

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
#ifndef TILEFINCH_NO_TRACE
        /* Site-census ledger: an inline event handler the page's CSP
           refused (the policy query at realm creation is not a refusal). */
        if (tilefinch_trace_census())
            puts("census-csp-refusal what=event-handler-attribute url=\"\"");
#endif
        return JS_NULL;
    }

    enum { INLINE_HANDLER_SCOPE_LIMIT = 3 };
    JSValueConst scopes[INLINE_HANDLER_SCOPE_LIMIT];
    size_t scope_count = 0;
    for (int i = 1; i < argc && i <= INLINE_HANDLER_SCOPE_LIMIT; i++)
        if (JS_IsObject(argv[i])) scopes[scope_count++] = argv[i];

    size_t source_length = 0;
    const char *source = JS_ToCStringLen(context, &source_length, argv[0]);
    if (source == NULL) return JS_EXCEPTION;
    enum { INLINE_HANDLER_SOURCE_LIMIT = 64 * 1024 };
    if (source_length > INLINE_HANDLER_SOURCE_LIMIT) {
        JS_FreeCString(context, source);
        return JS_ThrowRangeError(context,
                                  "inline event handler exceeds bounded size");
    }

    static const char scope_prefix[] = "function(){";
    static const char scope_with[] = "with(this[0])";
    static const char scope_return[] = "return ";
    static const char prefix[] = "function(event){\n";
    static const char suffix[] = "\n}";
    size_t scope_text = scope_count == 0 ? 0u
        : sizeof(scope_prefix) - 1u + scope_count * (sizeof(scope_with) - 1u)
            + sizeof(scope_return) - 1u + 1u;
    size_t length = 1u + scope_text + sizeof(prefix) - 1u + source_length
        + sizeof(suffix) - 1u + 1u;
    char *program = budget_malloc(bridge->host->budget, length + 1u);
    if (program == NULL) {
        JS_FreeCString(context, source);
        return JS_ThrowOutOfMemory(context);
    }
    size_t used = 0;
    program[used++] = '(';
    if (scope_count > 0) {
        memcpy(program + used, scope_prefix, sizeof(scope_prefix) - 1u);
        used += sizeof(scope_prefix) - 1u;
        for (size_t i = 0; i < scope_count; i++) {
            memcpy(program + used, scope_with, sizeof(scope_with) - 1u);
            program[used + 10u] = (char) ('0' + i);
            used += sizeof(scope_with) - 1u;
        }
        memcpy(program + used, scope_return, sizeof(scope_return) - 1u);
        used += sizeof(scope_return) - 1u;
    }
    memcpy(program + used, prefix, sizeof(prefix) - 1u);
    used += sizeof(prefix) - 1u;
    memcpy(program + used, source, source_length);
    used += source_length;
    memcpy(program + used, suffix, sizeof(suffix) - 1u);
    used += sizeof(suffix) - 1u;
    if (scope_count > 0) program[used++] = '}';
    program[used++] = ')';
    program[used] = '\0';
    JS_FreeCString(context, source);
#if !defined(__PSP__)
    TilefinchTestFaults *faults = tilefinch_test_faults();
    unsigned refuse = faults->inline_handler_refuse_allocation;
    faults->inline_handler_refuse_allocation = 0;
    if (refuse != 0) JS_TestRefuseAllocation(refuse);
#endif
    JSValue function = JS_Eval(context, program, length,
                               "<inline-event-handler>",
                               JS_EVAL_TYPE_GLOBAL);
#if !defined(__PSP__)
    if (refuse != 0) JS_TestRefuseAllocation(0);
#endif
    budget_free(bridge->host->budget, program);
    if (JS_IsException(function)) {
        /* The heap refused the compile: the browser's refusal, not an
           error in the page's handler, so it is not reported to the
           page. The handler stays uncompiled and is compiled again at its
           next dispatch. */
        JSValue error = JS_GetException(context);
        if (inline_handler_compile_refused(context, error)) {
            JS_FreeValue(context, error);
            return JS_NULL;
        }
        return JS_Throw(context, error);
    }
    if (scope_count == 0) return function;
    JSValue chain = JS_NewArray(context);
    for (size_t i = 0; i < scope_count && !JS_IsException(chain); i++)
        if (js_rt_define_index(context, chain, (uint32_t) i,
                                 JS_DupValue(context, scopes[i])) < 0) {
            JS_FreeValue(context, chain);
            chain = JS_EXCEPTION;
        }
    JSValue handler = JS_IsException(chain) ? JS_EXCEPTION
        : JS_Call(context, function, chain, 0, NULL);
    JS_FreeValue(context, chain);
    JS_FreeValue(context, function);
    return handler;
}

static JSValue script_profile_native_value(JSContext *context,
                                           JSCFunction *function,
                                           const char *name, int arguments);

#ifdef TILEFINCH_SCRIPT_SPLIT
/* The bridge natives the script split times (script_split.h): a plain C
   wrapper per native, so calls keep the engine's direct native path (the
   profiler's trampoline, when on, wraps the wrapper). */
#define SCRIPT_SPLIT_NATIVE(function, kind)                                  \
    static JSValue split_##function(JSContext *context,                      \
                                    JSValueConst this_value, int argc,       \
                                    JSValueConst *argv)                      \
    {                                                                        \
        unsigned token = script_split_enter(kind);                           \
        JSValue value = function(context, this_value, argc, argv);           \
        script_split_leave(token);                                           \
        return value;                                                        \
    }
SCRIPT_SPLIT_NATIVE(js_computed_style_get, SCRIPT_SPLIT_STYLE)
SCRIPT_SPLIT_NATIVE(js_computed_style_read, SCRIPT_SPLIT_STYLE)
SCRIPT_SPLIT_NATIVE(js_computed_style_values, SCRIPT_SPLIT_STYLE)
SCRIPT_SPLIT_NATIVE(js_transition_snapshot, SCRIPT_SPLIT_STYLE)
SCRIPT_SPLIT_NATIVE(js_dom_query, SCRIPT_SPLIT_QUERY)
SCRIPT_SPLIT_NATIVE(js_dom_query_all, SCRIPT_SPLIT_QUERY)
SCRIPT_SPLIT_NATIVE(js_dom_closest, SCRIPT_SPLIT_QUERY)
SCRIPT_SPLIT_NATIVE(js_dom_matches, SCRIPT_SPLIT_QUERY)
SCRIPT_SPLIT_NATIVE(js_dom_query_selector_method, SCRIPT_SPLIT_QUERY)
SCRIPT_SPLIT_NATIVE(js_dom_query_selector_all_method, SCRIPT_SPLIT_QUERY)
SCRIPT_SPLIT_NATIVE(js_dom_set_attribute, SCRIPT_SPLIT_MUTATE)
SCRIPT_SPLIT_NATIVE(js_dom_remove_attribute, SCRIPT_SPLIT_MUTATE)
SCRIPT_SPLIT_NATIVE(js_dom_insert_before, SCRIPT_SPLIT_MUTATE)
SCRIPT_SPLIT_NATIVE(js_dom_append, SCRIPT_SPLIT_MUTATE)
SCRIPT_SPLIT_NATIVE(js_dom_remove, SCRIPT_SPLIT_MUTATE)
SCRIPT_SPLIT_NATIVE(js_style_set, SCRIPT_SPLIT_MUTATE)
SCRIPT_SPLIT_NATIVE(js_dom_set_inner_html, SCRIPT_SPLIT_MUTATE)
#undef SCRIPT_SPLIT_NATIVE
static JSCFunction *script_split_native(JSCFunction *function)
{
    static const struct {
        JSCFunction *function;
        JSCFunction *wrapper;
    } natives[] = {
        { js_computed_style_get, split_js_computed_style_get },
        { js_computed_style_read, split_js_computed_style_read },
        { js_computed_style_values, split_js_computed_style_values },
        { js_transition_snapshot, split_js_transition_snapshot },
        { js_dom_query, split_js_dom_query },
        { js_dom_query_all, split_js_dom_query_all },
        { js_dom_closest, split_js_dom_closest },
        { js_dom_matches, split_js_dom_matches },
        { js_dom_query_selector_method, split_js_dom_query_selector_method },
        { js_dom_query_selector_all_method,
          split_js_dom_query_selector_all_method },
        { js_dom_set_attribute, split_js_dom_set_attribute },
        { js_dom_remove_attribute, split_js_dom_remove_attribute },
        { js_dom_insert_before, split_js_dom_insert_before },
        { js_dom_append, split_js_dom_append },
        { js_dom_remove, split_js_dom_remove },
        { js_style_set, split_js_style_set },
        { js_dom_set_inner_html, split_js_dom_set_inner_html },
    };
    for (size_t i = 0; i < sizeof(natives) / sizeof(natives[0]); i++)
        if (natives[i].function == function) return natives[i].wrapper;
    return function;
}
#endif

bool js_rt_install_native(JSContext *context, JSValue target,
                          const JsRtNative *native)
{
    JSCFunction *function = native->function;
#ifdef TILEFINCH_SCRIPT_SPLIT
    function = script_split_native(function);
#endif
    JSValue value = (native->flags & JS_RT_NATIVE_UNPROFILED)
        ? JS_NewCFunction(context, function, native->name, native->arguments)
        : script_profile_native_value(
              context, function, native->name, native->arguments);
    if (JS_IsException(value)) return false;
    /* The host's natives do the browser's work whoever calls them: a
       global they set or an interface they read is the browser's, not what
       the calling page made of it (the interface snapshot). */
    JS_MarkBrowserNative(context, value);
    int flags = 0;
    if (native->flags & JS_RT_NATIVE_TEST_OVERRIDABLE) {
        DomBridge *bridge = JS_GetContextOpaque(context);
        if (bridge != NULL && bridge->allow_test_network_primitive_overrides)
            flags = JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE;
    }
    if (flags == 0 && (native->flags & JS_RT_NATIVE_TRANSIENT))
        flags = JS_PROP_CONFIGURABLE;
    /* Only TRUE counts: lazy modules install their bridges after author
       code ran, and a page-owned non-configurable name answers FALSE. The
       module would then capture the page's value as its bridge. */
    if (JS_DefinePropertyValueStr(
            context, target, native->name, value, flags) <= 0)
        return false;
    /* A test's replacement of an overridable native must reach the
       bootstrap, which the interface snapshot would otherwise keep from
       it. (A no-op when the target is not the global object.) */
    if (flags & JS_PROP_WRITABLE) JS_LeaveGlobalToPage(context, native->name);
    return true;
}

bool js_rt_install_natives(JSContext *context, JSValue target,
                           const JsRtNative *natives, size_t count)
{
    for (size_t i = 0; i < count; i++) {
#if TILEFINCH_LAB_ENVIRONMENT
        if ((natives[i].flags & JS_RT_NATIVE_COMPRESSION_STREAMS)
            && tilefinch_lab_getenv("TILEFINCH_DISABLE_COMPRESSION_STREAMS")
                   != NULL)
            continue;
#endif
        if (!js_rt_install_native(context, target, &natives[i])) return false;
    }
    return true;
}
