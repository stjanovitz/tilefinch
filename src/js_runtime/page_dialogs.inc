/* window.alert(), confirm() and prompt(): the HTML simple dialogs.

   Native, not bootstrap JavaScript: the three operations hold no realm
   state, so a page realm pays only for three function objects.

   A call converts its arguments as WebIDL does (so a Symbol still throws),
   then asks the realm's per-load policy (tilefinch/page_dialog.h) and the
   frontend's platform page_dialog service. Script stays suspended inside
   that service call: no task, timer, rAF callback or event of this page can
   run until it returns, which is the specification's pause. Anything that
   refuses -- the realm, the policy, or a frontend that cannot show a dialog
   now -- is the "cannot show simple dialogs" outcome, and the page gets the
   default-deny answer at once: undefined, false or null. */

/* One dialog at a time per process: a frontend never runs page script while
   its dialog is open, so a second request can only come from a broken
   frontend or an unforeseen reentry, and is refused. */
static bool page_dialog_open;

/* The realm whose per-load policy decides, and whether the requester may
   ask at all. A child frame walks to its top-level realm through the parents
   navigation named for it; the walk is bounded by the frame limit. */
static ScriptRuntime *page_dialog_owner(ScriptRuntime *runtime,
                                        PageDialogVerdict *verdict)
{
    ScriptRuntime *owner = runtime;
    for (unsigned depth = 0; owner != NULL; depth++) {
        if (owner->document_scope != SCRIPT_DOCUMENT_SCOPE_CHILD_FRAME)
            return owner;
        if (owner->dialog_parent == NULL || depth >= 8u) break;
        owner = owner->dialog_parent;
    }
    *verdict = PAGE_DIALOG_DENIED_FRAME;
    return NULL;
}

/* ToString() of ARGUMENT, read as at most PAGE_DIALOG_SOURCE_UNITS code
   units, into a QuickJS C string the caller frees. SLICE is the realm's
   String.prototype.slice, captured before any page script ran. */
static const char *page_dialog_argument(JSContext *context,
                                        JSValueConst argument,
                                        JSValueConst slice, size_t *length)
{
    JSValue text = JS_ToString(context, argument);
    if (JS_IsException(text)) return NULL;
    JSValue units = JS_GetPropertyStr(context, text, "length");
    double count = 0;
    bool long_text = !JS_IsException(units)
        && JS_ToFloat64(context, &count, units) == 0
        && count > (double) PAGE_DIALOG_SOURCE_UNITS;
    JS_FreeValue(context, units);
    if (long_text && JS_IsFunction(context, slice)) {
        JSValue bounds[2] = {
            JS_NewInt32(context, 0),
            JS_NewInt32(context, (int32_t) PAGE_DIALOG_SOURCE_UNITS)
        };
        JSValue prefix = JS_Call(context, slice, text, 2, bounds);
        JS_FreeValue(context, text);
        if (JS_IsException(prefix)) return NULL;
        text = prefix;
    } else if (long_text) {
        JS_FreeValue(context, text);
        JS_ThrowInternalError(context, "dialog text unavailable");
        return NULL;
    }
    const char *bytes = JS_ToCStringLen(context, length, text);
    JS_FreeValue(context, text);
    return bytes;
}

static JSValue page_dialog_default(JSContext *context, PageDialogKind kind)
{
    if (kind == PAGE_DIALOG_CONFIRM) return JS_FALSE;
    if (kind == PAGE_DIALOG_PROMPT) return JS_NULL;
    (void) context;
    return JS_UNDEFINED;
}

/* The time a dialog was open is not script time: move this entry's
   watchdog deadlines past it, so a slow answer does not stop the script
   that asked. */
static void page_dialog_extend_watchdog(ScriptRuntime *runtime,
                                        uint64_t opened_ms)
{
    uint64_t now_ms = monotonic_time_ms();
    uint64_t open_ms = now_ms > opened_ms ? now_ms - opened_ms : 0;
    Watchdog *watchdog = &runtime->watchdog;
    watchdog->deadline_ms = watchdog->deadline_ms > UINT64_MAX - open_ms
        ? UINT64_MAX : watchdog->deadline_ms + open_ms;
    if (runtime->execution_deadline_cap_ms != 0u
        && runtime->execution_deadline_cap_ms != UINT64_MAX) {
        runtime->execution_deadline_cap_ms =
            runtime->execution_deadline_cap_ms > UINT64_MAX - open_ms
            ? UINT64_MAX : runtime->execution_deadline_cap_ms + open_ms;
    }
    watchdog->slice_started_ns = js_rt_monotonic_time_ns();
    watchdog->slice_work_units = 0;
}

static JSValue js_window_page_dialog(JSContext *context,
                                     JSValueConst this_value, int argc,
                                     JSValueConst *argv, int magic,
                                     JSValue *data)
{
    (void) this_value;
    PageDialogKind kind = (PageDialogKind) magic;
    /* WebIDL conversion comes first, even for a call that is then refused:
       alert() is alert(""), alert(undefined) shows "undefined", and the
       optional confirm/prompt arguments default to "" for undefined. */
    bool message_given = kind == PAGE_DIALOG_ALERT
        ? argc > 0 : argc > 0 && !JS_IsUndefined(argv[0]);
    bool default_given = kind == PAGE_DIALOG_PROMPT && argc > 1
        && !JS_IsUndefined(argv[1]);
    size_t message_length = 0, default_length = 0;
    const char *message = message_given
        ? page_dialog_argument(context, argv[0], data[0], &message_length)
        : NULL;
    if (message_given && message == NULL) return JS_EXCEPTION;
    const char *default_value = default_given
        ? page_dialog_argument(context, argv[1], data[0], &default_length)
        : NULL;
    if (default_given && default_value == NULL) {
        JS_FreeCString(context, message);
        return JS_EXCEPTION;
    }

    DomBridge *bridge = JS_GetContextOpaque(context);
    ScriptRuntime *runtime = bridge != NULL ? bridge->host : NULL;
    PageDialogVerdict verdict = PAGE_DIALOG_ADMITTED;
    ScriptRuntime *owner = NULL;
    /* Only the document realm itself: worker realms have no such globals,
       and same-origin frame contexts reach these functions as the
       document's own. */
    if (runtime == NULL || runtime->context != context)
        verdict = PAGE_DIALOG_DENIED_REALM;
    else
        owner = page_dialog_owner(runtime, &verdict);
    uint64_t now_ms = monotonic_time_ms();
    if (verdict == PAGE_DIALOG_ADMITTED && !owner->page_visibility_desired)
        verdict = PAGE_DIALOG_DENIED_HIDDEN;
    if (verdict == PAGE_DIALOG_ADMITTED && page_dialog_open)
        verdict = PAGE_DIALOG_DENIED_NESTED;
    if (verdict == PAGE_DIALOG_ADMITTED)
        verdict = page_dialog_policy_admit(
            &owner->page_dialogs,
            owner->bridge.user_activation_generation, now_ms);

    char host[PAGE_DIALOG_HOST_BYTES];
    char *buffers = NULL;
    if (verdict == PAGE_DIALOG_ADMITTED) {
        /* One bounded block on the page's ceiling: the displayed message,
           the displayed default, and the answer. */
        buffers = budget_malloc(runtime->budget, PAGE_DIALOG_MESSAGE_BYTES
                                    + 2u * PAGE_DIALOG_PROMPT_BYTES);
        if (buffers == NULL) verdict = PAGE_DIALOG_DENIED_UNAVAILABLE;
    }
    JSValue result = page_dialog_default(context, kind);
    if (verdict == PAGE_DIALOG_ADMITTED) {
        char *shown_message = buffers;
        char *shown_default = buffers + PAGE_DIALOG_MESSAGE_BYTES;
        char *answer = shown_default + PAGE_DIALOG_PROMPT_BYTES;
        (void) page_dialog_sanitize_text(
            message, message_length, false, PAGE_DIALOG_MESSAGE_LINES,
            shown_message, PAGE_DIALOG_MESSAGE_BYTES, NULL);
        (void) page_dialog_sanitize_text(
            default_value, default_length, true, 1u, shown_default,
            PAGE_DIALOG_PROMPT_BYTES, NULL);
        /* The title is the requester's own host; an opaque origin has
           none, and the frontend says "This page". */
        if (runtime->bridge.opaque_origin
            || !page_dialog_title_host(runtime->bridge.document_url, host,
                                       sizeof(host)))
            host[0] = '\0';
        /* The answer starts as the default itself. What the frontend shows
           is shown_default. */
        (void) page_dialog_answer_text(default_value, default_length, answer,
                                       PAGE_DIALOG_PROMPT_BYTES);
        TilefinchPageDialogRequest request = {
            .kind = kind,
            .host = host,
            .message = shown_message,
            .default_value = shown_default,
            .offer_suppress = owner->page_dialogs.shown != 0
        };
        TilefinchPageDialogResponse response = {
            .outcome = PAGE_DIALOG_OUTCOME_DISMISSED,
            .text = answer,
            .text_capacity = PAGE_DIALOG_PROMPT_BYTES
        };
        PageDialogPolicy before = owner->page_dialogs;
        page_dialog_policy_opened(&owner->page_dialogs, now_ms);
        page_dialog_open = true;
        bool shown = tilefinch_platform_page_dialog(&request, &response);
        page_dialog_open = false;
        page_dialog_extend_watchdog(runtime, now_ms);
        if (!shown) {
            /* Nothing was on screen: give back the count and the rate
               slot, so a frontend's refusal is never held against the
               page's next dialog. */
            owner->page_dialogs.shown = before.shown;
            owner->page_dialogs.since_gesture = before.since_gesture;
            owner->page_dialogs.opened_next = before.opened_next;
            memcpy(owner->page_dialogs.opened_ms, before.opened_ms,
                   sizeof(before.opened_ms));
            verdict = PAGE_DIALOG_DENIED_UNAVAILABLE;
        } else {
            page_dialog_policy_answered(&owner->page_dialogs,
                                        response.outcome);
            if (response.outcome == PAGE_DIALOG_OUTCOME_ACCEPTED) {
                if (kind == PAGE_DIALOG_CONFIRM) {
                    result = JS_TRUE;
                } else if (kind == PAGE_DIALOG_PROMPT) {
                    answer[PAGE_DIALOG_PROMPT_BYTES - 1u] = '\0';
                    /* The page gets the user's string as it is, still
                       bounded and made valid UTF-8 here. */
                    char *clean = shown_message;
                    size_t clean_length = page_dialog_answer_text(
                        answer, strlen(answer), clean,
                        PAGE_DIALOG_PROMPT_BYTES);
                    result = JS_NewStringLen(context, clean, clean_length);
                }
            }
        }
    }
#ifdef JS_RT_TEST_INSTRUMENTS
    /* Refusal counts are only read by tests. */
    if (verdict != PAGE_DIALOG_ADMITTED)
        page_dialog_policy_note_denied(
            owner != NULL ? &owner->page_dialogs
                : runtime != NULL ? &runtime->page_dialogs : NULL,
            verdict);
#endif
    if (buffers != NULL) budget_free(runtime->budget, buffers);
    JS_FreeCString(context, message);
    JS_FreeCString(context, default_value);
    return result;
}

static bool runtime_install_page_dialogs(ScriptRuntime *host,
                                         JSValueConst global)
{
    JSContext *context = host->context;
    JSValue string_constructor = JS_GetPropertyStr(context, global, "String");
    JSValue string_prototype = JS_IsObject(string_constructor)
        ? JS_GetPropertyStr(context, string_constructor, "prototype")
        : JS_UNDEFINED;
    JSValue slice = JS_IsObject(string_prototype)
        ? JS_GetPropertyStr(context, string_prototype, "slice")
        : JS_UNDEFINED;
    JS_FreeValue(context, string_prototype);
    JS_FreeValue(context, string_constructor);
    bool ok = JS_IsFunction(context, slice);
    static const char *const names[3] = {"alert", "confirm", "prompt"};
    for (int kind = 0; ok && kind < 3; kind++) {
        /* WebIDL: own, writable, enumerable, configurable operations of the
           Window global; every argument is optional, so length is 0. */
        JSValue function = JS_NewCFunctionData(
            context, js_window_page_dialog, 0, kind, 1, &slice);
        if (JS_IsException(function)) {
            ok = false;
            break;
        }
        if (JS_DefinePropertyValueStr(context, function, "name",
                                      JS_NewString(context, names[kind]),
                                      JS_PROP_CONFIGURABLE) < 0) {
            JS_FreeValue(context, function);
            ok = false;
            break;
        }
        ok = JS_DefinePropertyValueStr(context, global, names[kind],
                                       function, JS_PROP_C_W_E) >= 0;
    }
    JS_FreeValue(context, slice);
    return ok;
}

void script_runtime_set_dialog_parent(ScriptRuntime *child,
                                      ScriptRuntime *parent)
{
    if (child == NULL || child == parent) return;
    child->dialog_parent = parent;
}

bool script_runtime_dialogs_allowed(ScriptRuntime *runtime)
{
    PageDialogVerdict verdict = PAGE_DIALOG_ADMITTED;
    return runtime != NULL && page_dialog_owner(runtime, &verdict) != NULL;
}

#ifdef JS_RT_TEST_INSTRUMENTS
bool script_runtime_page_dialog_policy(ScriptRuntime *runtime,
                                       PageDialogPolicy *policy)
{
    PageDialogVerdict verdict = PAGE_DIALOG_ADMITTED;
    ScriptRuntime *owner = runtime == NULL
        ? NULL : page_dialog_owner(runtime, &verdict);
    if (policy == NULL) return false;
    if (owner == NULL) {
        memset(policy, 0, sizeof(*policy));
        if (runtime != NULL) *policy = runtime->page_dialogs;
        return false;
    }
    *policy = owner->page_dialogs;
    return true;
}
#endif
