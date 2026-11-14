/* Microphone capture natives (tilefinch/media_capture.h): the command
   bridge the lazy media-capture module captures, the permission state
   permissions.query() reads, and each realm's live-track bookkeeping.

   Every admission decision is made here, never in JavaScript: a page that
   calls the bridge directly meets the same checks as getUserMedia(). In
   order, a request needs a document realm, a secure context up to the
   top-level page, a frame chain the top-level page delegated "microphone"
   to, a visible page, a microphone, and the user's permission for the
   top-level origin -- remembered, granted this page load, or asked now in
   the browser's own prompt. The prompt runs inside the page's script, as
   alert() does, and its open time is paused out of the script watchdog. */
#include "js_runtime_internal.h"
#include "tilefinch/media_capture.h"
#include "tilefinch/platform.h"
#include "tilefinch/url.h"

#include <stdio.h>
#include <string.h>

/* Command opcodes, shared with src/bootstrap/media-capture.js. */
enum {
    MEDIA_CAPTURE_COMMAND_REQUEST = 0,
    MEDIA_CAPTURE_COMMAND_RELEASE,
    MEDIA_CAPTURE_COMMAND_CLONE,
    MEDIA_CAPTURE_COMMAND_PERMISSION,
    MEDIA_CAPTURE_COMMAND_DEVICES,
    /* 5 is unassigned: samples reach pages only through Web Audio. */
    MEDIA_CAPTURE_COMMAND_ENABLE = 6
};

_Static_assert(MEDIA_CAPTURE_TRACK_LIMIT <= 8u,
               "media_capture_disabled holds one bit per track slot");

/* The realm holds no track: of an ended session, or at teardown. */
static void media_capture_tracks_forget(ScriptRuntime *runtime)
{
    runtime->media_capture_live_tracks = 0;
    runtime->media_capture_generation = 0;
    memset(runtime->media_capture_tracks, 0,
           sizeof(runtime->media_capture_tracks));
    runtime->media_capture_disabled = 0;
}

/* The slot holding TRACK, or MEDIA_CAPTURE_TRACK_LIMIT. */
static unsigned media_capture_track_slot(const ScriptRuntime *runtime,
                                         uint32_t track)
{
    unsigned slot = 0;
    while (track != 0 && slot < MEDIA_CAPTURE_TRACK_LIMIT
           && runtime->media_capture_tracks[slot] != track)
        slot++;
    return track == 0 ? MEDIA_CAPTURE_TRACK_LIMIT : slot;
}

/* Names one more live track of the realm's session (the caller checked
   MEDIA_CAPTURE_TRACK_LIMIT); an enabled track's new serial. */
static uint32_t media_capture_track_add(ScriptRuntime *runtime)
{
    unsigned slot = 0;
    while (runtime->media_capture_tracks[slot] != 0) slot++;
    uint32_t track = runtime->media_capture_last_track + 1u;
    if (track == 0) track = 1u;
    runtime->media_capture_last_track = track;
    runtime->media_capture_tracks[slot] = track;
    runtime->media_capture_disabled &= (uint8_t) ~(1u << slot);
    runtime->media_capture_live_tracks++;
    return track;
}

static void media_capture_track_remove(ScriptRuntime *runtime, unsigned slot)
{
    runtime->media_capture_tracks[slot] = 0;
    runtime->media_capture_disabled &= (uint8_t) ~(1u << slot);
    runtime->media_capture_live_tracks--;
    if (runtime->media_capture_live_tracks == 0)
        runtime->media_capture_generation = 0;
}

JsRtMediaTrackState js_rt_media_capture_track_state(
    const ScriptRuntime *runtime, uint32_t track)
{
    if (runtime == NULL || runtime->media_capture_live_tracks == 0
        || runtime->media_capture_generation
               != tilefinch_media_capture_generation()
        || media_capture_session_end_reason(runtime->media_capture_generation)
               != MEDIA_CAPTURE_END_NONE)
        return JS_RT_MEDIA_TRACK_ENDED;
    unsigned slot = media_capture_track_slot(runtime, track);
    if (slot == MEDIA_CAPTURE_TRACK_LIMIT) return JS_RT_MEDIA_TRACK_ENDED;
    return (runtime->media_capture_disabled & (1u << slot)) != 0
        ? JS_RT_MEDIA_TRACK_DISABLED : JS_RT_MEDIA_TRACK_LIVE;
}

static bool media_capture_secure(const ScriptRuntime *runtime)
{
    return runtime != NULL && !runtime->bridge.opaque_origin
        && runtime->bridge.document_url != NULL
        && tilefinch_url_potentially_trustworthy(runtime->bridge.document_url);
}

/* The top-level realm whose origin holds the permission, walking delegated
   frames; NULL with *VERDICT set when the chain refuses. Bounded by the
   frame depth. */
static ScriptRuntime *media_capture_owner(ScriptRuntime *runtime,
                                          MediaCaptureVerdict *verdict)
{
    ScriptRuntime *owner = runtime;
    for (unsigned depth = 0; owner != NULL; depth++) {
        if (!media_capture_secure(owner)) {
            *verdict = MEDIA_CAPTURE_DENIED_INSECURE;
            return NULL;
        }
        if (owner->document_scope != SCRIPT_DOCUMENT_SCOPE_CHILD_FRAME)
            return owner;
        if (!owner->media_capture_frame_allowed
            || owner->media_capture_parent == NULL || depth >= 8u) break;
        owner = owner->media_capture_parent;
    }
    *verdict = MEDIA_CAPTURE_DENIED_POLICY;
    return NULL;
}

static bool media_capture_origin(const ScriptRuntime *owner,
                                 char origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT])
{
    return tilefinch_url_origin(owner->bridge.document_url, origin,
                                TILEFINCH_ORIGIN_SERIALIZED_LIMIT)
        && strcmp(origin, "null") != 0;
}

static ScriptRuntime *media_capture_runtime(JSContext *context)
{
    DomBridge *bridge = JS_GetContextOpaque(context);
    ScriptRuntime *runtime = bridge != NULL ? bridge->host : NULL;
    /* Only the document realm itself: never a worker's context. */
    return runtime != NULL && runtime->context == context ? runtime : NULL;
}

/* What permissions.query({name: "microphone"}) says for RUNTIME. */
static MediaCapturePermission media_capture_state(ScriptRuntime *runtime)
{
    MediaCaptureVerdict verdict = MEDIA_CAPTURE_GRANTED;
    ScriptRuntime *owner = runtime == NULL
        ? NULL : media_capture_owner(runtime, &verdict);
    char origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    if (owner == NULL || !media_capture_origin(owner, origin))
        return MEDIA_CAPTURE_PERMISSION_DENIED;
    MediaCapturePermission stored =
        tilefinch_platform_media_capture_permission(origin, NULL);
    if (stored == MEDIA_CAPTURE_PERMISSION_PROMPT
        && owner->media_capture_session_grant
        && owner->media_capture_revoke_epoch == media_capture_revoke_epoch())
        return MEDIA_CAPTURE_PERMISSION_GRANTED;
    return stored;
}

/* Asks the user. GRANTED, or the refusal. */
static MediaCaptureVerdict media_capture_ask(ScriptRuntime *runtime,
                                             ScriptRuntime *owner,
                                             const char *origin,
                                             bool can_remember)
{
    if (owner->media_capture_dismissals >= MEDIA_CAPTURE_DISMISS_LIMIT)
        return MEDIA_CAPTURE_DENIED_EMBARGO;
    /* After a dismissal, only a new activation of the page may ask again:
       a page cannot reopen the prompt in a loop. */
    if (owner->media_capture_dismissals != 0
        && owner->media_capture_dismiss_gesture
               == owner->bridge.user_activation_generation)
        return MEDIA_CAPTURE_DENIED_EMBARGO;
    if (owner->media_capture_prompting) return MEDIA_CAPTURE_DENIED_UNAVAILABLE;
    char host[MEDIA_CAPTURE_HOST_BYTES];
    if (!page_dialog_title_host(owner->bridge.document_url, host, sizeof(host)))
        host[0] = '\0';
    TilefinchMediaCapturePrompt request = {
        .host = host,
        .origin = origin,
        .from_frame = owner != runtime,
        .can_remember = can_remember
    };
    MediaCapturePromptAnswer answer = MEDIA_CAPTURE_PROMPT_DISMISSED;
    uint64_t opened_ms = js_rt_monotonic_time_ns() / UINT64_C(1000000);
    owner->media_capture_prompting = true;
    bool shown = tilefinch_platform_media_capture_prompt(&request, &answer);
    owner->media_capture_prompting = false;
    js_rt_extend_watchdog_after_modal(runtime, opened_ms);
    if (!shown) return MEDIA_CAPTURE_DENIED_UNAVAILABLE;
    switch (answer) {
    case MEDIA_CAPTURE_PROMPT_ALLOW_ALWAYS:
        if (tilefinch_platform_media_capture_remember(
                origin, MEDIA_CAPTURE_PERMISSION_GRANTED)) {
            media_capture_permission_noted();
            return MEDIA_CAPTURE_GRANTED;
        }
        /* A full table keeps it for this page load instead. */
        /* fall through */
    case MEDIA_CAPTURE_PROMPT_ALLOW_ONCE:
        owner->media_capture_session_grant = true;
        owner->media_capture_revoke_epoch = media_capture_revoke_epoch();
        media_capture_permission_noted();
        return MEDIA_CAPTURE_GRANTED;
    case MEDIA_CAPTURE_PROMPT_BLOCK:
        (void) tilefinch_platform_media_capture_remember(
            origin, MEDIA_CAPTURE_PERMISSION_DENIED);
        owner->media_capture_session_grant = false;
        media_capture_permission_noted();
        return MEDIA_CAPTURE_DENIED_BLOCKED;
    case MEDIA_CAPTURE_PROMPT_DISMISSED:
    default:
        if (owner->media_capture_dismissals < UINT8_MAX)
            owner->media_capture_dismissals++;
        owner->media_capture_dismiss_gesture =
            owner->bridge.user_activation_generation;
        return MEDIA_CAPTURE_DENIED_DISMISSED;
    }
}

static JSValue media_capture_refusal(JSContext *context,
                                     MediaCaptureVerdict verdict)
{
    char text[128];
    snprintf(text, sizeof(text), "%s:%s",
             media_capture_verdict_error_name(verdict),
             media_capture_verdict_message(verdict));
    return JS_NewString(context, text);
}

/* The realm's tracks are of a session that has ended: forget them, and
   keep the session so its tracks still get their "ended" at the next task
   boundary (js_rt_media_capture_take_ended). */
static void media_capture_forget_stale(ScriptRuntime *runtime)
{
    if (runtime->media_capture_live_tracks == 0
        || media_capture_session_end_reason(runtime->media_capture_generation)
               == MEDIA_CAPTURE_END_NONE)
        return;
    uint32_t *pending = runtime->media_capture_ended_pending;
    /* Two sessions can end between task boundaries only with two user
       stops; past that, the older one's delivery is the one dropped. */
    if (pending[0] != 0 && pending[1] != 0) {
        pending[0] = pending[1];
        pending[1] = 0;
    }
    pending[pending[0] == 0 ? 0 : 1] = runtime->media_capture_generation;
    media_capture_tracks_forget(runtime);
}

static JSValue media_capture_request(JSContext *context,
                                     ScriptRuntime *runtime)
{
    MediaCaptureVerdict verdict = MEDIA_CAPTURE_GRANTED;
    ScriptRuntime *owner = NULL;
    char origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    if (runtime == NULL) verdict = MEDIA_CAPTURE_DENIED_REALM;
    else owner = media_capture_owner(runtime, &verdict);
    if (verdict == MEDIA_CAPTURE_GRANTED && !media_capture_origin(owner, origin))
        verdict = MEDIA_CAPTURE_DENIED_INSECURE;
    /* A page that is not on screen never prompts and never starts the
       microphone. */
    if (verdict == MEDIA_CAPTURE_GRANTED && !owner->page_visibility_desired)
        verdict = MEDIA_CAPTURE_DENIED_HIDDEN;
    if (verdict == MEDIA_CAPTURE_GRANTED
        && !tilefinch_platform_media_capture_available())
        verdict = MEDIA_CAPTURE_NO_DEVICE;
    if (verdict == MEDIA_CAPTURE_GRANTED) {
        media_capture_forget_stale(runtime);
        if (runtime->media_capture_live_tracks >= MEDIA_CAPTURE_TRACK_LIMIT)
            verdict = MEDIA_CAPTURE_DENIED_LIMIT;
    }
    bool can_remember = false;
    MediaCapturePermission stored = verdict == MEDIA_CAPTURE_GRANTED
        ? tilefinch_platform_media_capture_permission(origin, &can_remember)
        : MEDIA_CAPTURE_PERMISSION_PROMPT;
    if (verdict == MEDIA_CAPTURE_GRANTED
        && stored == MEDIA_CAPTURE_PERMISSION_DENIED)
        verdict = MEDIA_CAPTURE_DENIED_BLOCKED;
    if (verdict == MEDIA_CAPTURE_GRANTED
        && owner->media_capture_revoke_epoch != media_capture_revoke_epoch())
        owner->media_capture_session_grant = false;
    if (verdict == MEDIA_CAPTURE_GRANTED
        && stored != MEDIA_CAPTURE_PERMISSION_GRANTED
        && !owner->media_capture_session_grant)
        verdict = media_capture_ask(runtime, owner, origin, can_remember);
    uint32_t generation = 0;
    MediaCaptureFormat format = {0};
    if (verdict == MEDIA_CAPTURE_GRANTED)
        verdict = media_capture_session_open(&generation, &format);
    if (verdict != MEDIA_CAPTURE_GRANTED)
        return media_capture_refusal(context, verdict);
    if (runtime->media_capture_live_tracks != 0
        && runtime->media_capture_generation != generation)
        media_capture_tracks_forget(runtime);
    runtime->media_capture_generation = generation;
    uint32_t track = media_capture_track_add(runtime);
    JSValue result = JS_NewObject(context);
    if (JS_IsException(result)
        || js_rt_define_field(context, result, "generation",
                              JS_NewUint32(context, generation)) < 0
        || js_rt_define_field(context, result, "track",
                              JS_NewUint32(context, track)) < 0
        || js_rt_define_field(context, result, "sampleRate",
                              JS_NewUint32(context, format.sample_rate)) < 0
        || js_rt_define_field(context, result, "channelCount",
                              JS_NewUint32(context, format.channels)) < 0
        || js_rt_define_field(context, result, "latency",
                              JS_NewFloat64(context, format.latency)) < 0) {
        /* The page never saw the track: give it back. */
        media_capture_track_remove(
            runtime, media_capture_track_slot(runtime, track));
        media_capture_session_release(generation, 1u);
        JS_FreeValue(context, result);
        return JS_EXCEPTION;
    }
    return result;
}

static bool media_capture_track_argument(JSContext *context, int argc,
                                         JSValueConst *argv, uint32_t *track)
{
    *track = 0;
    return argc > 1 && JS_ToUint32(context, track, argv[1]) == 0;
}

JSValue js_media_capture_command(JSContext *context, JSValueConst this_value,
                                 int argc, JSValueConst *argv)
{
    (void) this_value;
    int32_t operation = -1;
    if (argc < 1 || JS_ToInt32(context, &operation, argv[0]) != 0)
        return JS_EXCEPTION;
    ScriptRuntime *runtime = media_capture_runtime(context);
    uint32_t track = 0;
    unsigned slot = MEDIA_CAPTURE_TRACK_LIMIT;
    switch (operation) {
    case MEDIA_CAPTURE_COMMAND_REQUEST:
        return media_capture_request(context, runtime);
    case MEDIA_CAPTURE_COMMAND_RELEASE:
        /* A stale or unknown track is ignored. */
        if (!media_capture_track_argument(context, argc, argv, &track))
            return JS_EXCEPTION;
        if (runtime != NULL) {
            media_capture_forget_stale(runtime);
            slot = media_capture_track_slot(runtime, track);
        }
        if (slot != MEDIA_CAPTURE_TRACK_LIMIT) {
            uint32_t generation = runtime->media_capture_generation;
            media_capture_track_remove(runtime, slot);
            media_capture_session_release(generation, 1u);
        }
        return JS_UNDEFINED;
    case MEDIA_CAPTURE_COMMAND_CLONE: {
        /* A clone shares the live source: no prompt, the same session.
           The clone's serial, or 0 when the source track is not live. */
        if (!media_capture_track_argument(context, argc, argv, &track))
            return JS_EXCEPTION;
        uint32_t opened = 0, generation = runtime != NULL
            ? runtime->media_capture_generation : 0;
        bool shared = runtime != NULL && generation != 0
            && media_capture_track_slot(runtime, track)
                   != MEDIA_CAPTURE_TRACK_LIMIT
            && runtime->media_capture_live_tracks < MEDIA_CAPTURE_TRACK_LIMIT
            && tilefinch_media_capture_generation() == generation;
        if (shared) {
            /* The session is live, so this only counts one more track. */
            shared = media_capture_session_open(&opened, NULL)
                         == MEDIA_CAPTURE_GRANTED;
            if (shared && opened != generation) {
                media_capture_session_release(opened, 1u);
                shared = false;
            }
        }
        return JS_NewUint32(context,
                            shared ? media_capture_track_add(runtime) : 0u);
    }
    case MEDIA_CAPTURE_COMMAND_ENABLE: {
        /* track.enabled: a disabled track's sources play silence. */
        if (!media_capture_track_argument(context, argc, argv, &track))
            return JS_EXCEPTION;
        bool enabled = argc > 2 && JS_ToBool(context, argv[2]);
        if (runtime != NULL) slot = media_capture_track_slot(runtime, track);
        if (slot != MEDIA_CAPTURE_TRACK_LIMIT) {
            if (enabled)
                runtime->media_capture_disabled &= (uint8_t) ~(1u << slot);
            else runtime->media_capture_disabled |= (uint8_t) (1u << slot);
        }
        return JS_UNDEFINED;
    }
    case MEDIA_CAPTURE_COMMAND_PERMISSION:
        if (runtime != NULL) runtime->media_capture_permission_watch = true;
        return JS_NewInt32(context, (int32_t) media_capture_state(runtime));
    case MEDIA_CAPTURE_COMMAND_DEVICES:
        return JS_NewBool(context,
                          runtime != NULL
                              && tilefinch_platform_media_capture_available());
    default:
        return JS_ThrowRangeError(context, "unknown media capture command");
    }
}

JSValue js_media_capture_permission(JSContext *context,
                                    JSValueConst this_value, int argc,
                                    JSValueConst *argv)
{
    (void) this_value;
    (void) argc;
    (void) argv;
    ScriptRuntime *runtime = media_capture_runtime(context);
    if (runtime != NULL) runtime->media_capture_permission_watch = true;
    static const char *const names[3] = {"prompt", "granted", "denied"};
    return JS_NewString(context, names[media_capture_state(runtime)]);
}

uint32_t js_rt_media_capture_take_ended(ScriptRuntime *runtime)
{
    if (runtime != NULL && runtime->media_capture_ended_pending[0] != 0) {
        uint32_t generation = runtime->media_capture_ended_pending[0];
        runtime->media_capture_ended_pending[0] =
            runtime->media_capture_ended_pending[1];
        runtime->media_capture_ended_pending[1] = 0;
        return generation;
    }
    if (runtime == NULL || runtime->media_capture_live_tracks == 0
        || media_capture_session_end_reason(runtime->media_capture_generation)
               == MEDIA_CAPTURE_END_NONE)
        return 0;
    uint32_t generation = runtime->media_capture_generation;
    media_capture_tracks_forget(runtime);
    return generation;
}

bool js_rt_media_capture_take_permission_change(ScriptRuntime *runtime)
{
    if (runtime == NULL) return false;
    if (runtime->media_capture_revoke_epoch != media_capture_revoke_epoch()) {
        runtime->media_capture_session_grant = false;
        runtime->media_capture_revoke_epoch = media_capture_revoke_epoch();
    }
    uint32_t epoch = tilefinch_media_capture_permission_epoch();
    if (runtime->media_capture_permission_epoch == epoch) return false;
    runtime->media_capture_permission_epoch = epoch;
    return runtime->media_capture_permission_watch;
}

bool js_rt_media_capture_event_pending(const ScriptRuntime *runtime)
{
    return runtime != NULL
        && (runtime->media_capture_ended_pending[0] != 0
            || (runtime->media_capture_live_tracks != 0
                && media_capture_session_end_reason(
                       runtime->media_capture_generation)
                       != MEDIA_CAPTURE_END_NONE)
            || (runtime->media_capture_permission_watch
                && runtime->media_capture_permission_epoch
                       != tilefinch_media_capture_permission_epoch()));
}

void js_rt_media_capture_release(ScriptRuntime *runtime)
{
    if (runtime == NULL || runtime->media_capture_live_tracks == 0) return;
    uint32_t generation = runtime->media_capture_generation;
    unsigned tracks = runtime->media_capture_live_tracks;
    media_capture_tracks_forget(runtime);
    media_capture_session_release(generation, tracks);
}

void script_runtime_set_media_capture_parent(ScriptRuntime *child,
                                             ScriptRuntime *parent,
                                             bool allowed)
{
    if (child == NULL || child == parent) return;
    child->media_capture_parent = allowed ? parent : NULL;
    child->media_capture_frame_allowed = allowed && parent != NULL;
}

bool script_runtime_media_capture_allowed(ScriptRuntime *runtime)
{
    MediaCaptureVerdict verdict = MEDIA_CAPTURE_GRANTED;
    return runtime != NULL && media_capture_owner(runtime, &verdict) != NULL;
}
