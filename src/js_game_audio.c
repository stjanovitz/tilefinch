/* Game-audio natives: the realm's engine handle and lifecycle, and the
   reference-build decode and command bridges (__tilefinchGameAudioDecode,
   __tilefinchGameAudioCommand) with their validation-build measurement.
   Split from js_dom_bindings.c; the native slots that shipping builds use
   live in js_runtime/game_audio_slots.inc. */
#include "js_runtime_internal.h"
#include "tilefinch/platform.h"

#include <string.h>

TilefinchGameAudio *js_rt_game_audio_engine(ScriptRuntime *runtime)
{
    if (runtime == NULL) return NULL;
    if (runtime->game_audio == NULL)
        runtime->game_audio = tilefinch_game_audio_create(runtime->budget);
    return runtime->game_audio;
}

bool js_rt_game_audio_lifecycle(ScriptRuntime *runtime, bool destroy)
{
    if (runtime == NULL) return false;
    if (destroy) {
        tilefinch_game_audio_destroy(runtime->game_audio);
        runtime->game_audio = NULL;
    } else tilefinch_game_audio_suspend(runtime->game_audio);
    return true;
}

#ifdef TILEFINCH_GAME_AUDIO_REFERENCE
static TilefinchGameAudio *runtime_game_audio(DomBridge *bridge)
{
    return bridge == NULL ? NULL : js_rt_game_audio_engine(bridge->host);
}

JSValue js_game_audio_decode(JSContext *context,
                             JSValueConst this_value,
                             int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    size_t length = 0;
    uint8_t *bytes = argc > 0
        ? JS_GetArrayBuffer(context, &length, argv[0]) : NULL;
    if (bytes == NULL || length == 0) return JS_NULL;
    TilefinchGameAudioBufferInfo info = {0};
    if (!tilefinch_game_audio_decode_wav(
            runtime_game_audio(bridge), bytes, length, &info)) return JS_NULL;
    JSValue object = JS_NewObject(context);
    if (JS_IsException(object)) return object;
    if (JS_SetPropertyStr(context, object, "handle",
                          JS_NewUint32(context, info.handle)) < 0
        || JS_SetPropertyStr(context, object, "length",
                             JS_NewUint32(context, info.frames)) < 0
        || JS_SetPropertyStr(context, object, "sampleRate",
                             JS_NewUint32(context, info.sample_rate)) < 0
        || JS_SetPropertyStr(context, object, "numberOfChannels",
                             JS_NewInt32(context, info.channels)) < 0) {
        JS_FreeValue(context, object);
        return JS_EXCEPTION;
    }
    return object;
}

#ifdef TILEFINCH_PSP_VALIDATION_LOG
static bool game_audio_native_measurement;
static unsigned game_audio_native_depth;
static ScriptGameAudioNativeMetrics game_audio_native_metrics;

void script_runtime_game_audio_native_measure(bool enabled)
{
    game_audio_native_measurement = enabled;
    memset(&game_audio_native_metrics, 0, sizeof(game_audio_native_metrics));
}

void script_runtime_game_audio_native_take(ScriptGameAudioNativeMetrics *metrics)
{
    if (metrics != NULL) *metrics = game_audio_native_metrics;
    memset(&game_audio_native_metrics, 0, sizeof(game_audio_native_metrics));
}

static JSValue js_game_audio_command_impl(JSContext *context,
#else
JSValue js_game_audio_command(JSContext *context,
#endif
                              JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    int32_t command = -1;
    if (bridge == NULL || bridge->host == NULL || argc < 1) return JS_FALSE;
    if (JS_ToInt32(context, &command, argv[0]) < 0) return JS_EXCEPTION;
    switch ((TilefinchGameAudioCommand) command) {
    case TILEFINCH_GAME_AUDIO_COMMAND_RESUME:
        if (!js_rt_bridge_user_activation_is_active(bridge)) return JS_FALSE;
        return tilefinch_game_audio_resume(runtime_game_audio(bridge))
            ? JS_TRUE : JS_FALSE;
    case TILEFINCH_GAME_AUDIO_COMMAND_SUSPEND:
        return JS_NewBool(context, js_rt_game_audio_lifecycle(bridge->host, false));
    case TILEFINCH_GAME_AUDIO_COMMAND_CLOSE:
        return JS_NewBool(context, js_rt_game_audio_lifecycle(bridge->host, true));
    case TILEFINCH_GAME_AUDIO_COMMAND_START_OSCILLATOR: {
        int32_t type = 0;
        double frequency = 0, gain_left = 1, gain_right = 1, delay = 0;
        if (argc < 6 || JS_ToInt32(context, &type, argv[1]) < 0
            || JS_ToFloat64(context, &frequency, argv[2]) < 0
            || JS_ToFloat64(context, &gain_left, argv[3]) < 0
            || JS_ToFloat64(context, &gain_right, argv[4]) < 0
            || JS_ToFloat64(context, &delay, argv[5]) < 0)
            return JS_FALSE;
        bridge = JS_GetContextOpaque(context);
        if (bridge == NULL || bridge->host == NULL) return JS_FALSE;
        uint32_t voice = 0;
        if (!tilefinch_game_audio_start_oscillator(
                bridge->host->game_audio,
                (TilefinchGameAudioOscillatorType) type,
                frequency, gain_left, gain_right, delay, &voice))
            return JS_FALSE;
        return JS_NewUint32(context, voice);
    }
    default:
        break;
    }
    uint32_t handle = 0;
    if (argc < 2) return JS_FALSE;
    if (JS_ToUint32(context, &handle, argv[1]) < 0) return JS_EXCEPTION;
    /* Every coercion can run author code, which may close the context.
       Resolve the runtime-owned engine only after the last one, so a nested
       close() cannot leave a stale native pointer in this command. */
    switch ((TilefinchGameAudioCommand) command) {
    case TILEFINCH_GAME_AUDIO_COMMAND_STOP: {
        double delay = 0;
        if (argc > 2 && JS_ToFloat64(context, &delay, argv[2]) < 0)
            return JS_FALSE;
        bridge = JS_GetContextOpaque(context);
        if (bridge == NULL || bridge->host == NULL) return JS_FALSE;
        tilefinch_game_audio_stop(bridge->host->game_audio, handle, delay);
        return JS_TRUE;
    }
    case TILEFINCH_GAME_AUDIO_COMMAND_SET_GAIN: {
        double gain_left = 1, gain_right = 1;
        if (argc < 4
            || JS_ToFloat64(context, &gain_left, argv[2]) < 0
            || JS_ToFloat64(context, &gain_right, argv[3]) < 0)
            return JS_FALSE;
        bridge = JS_GetContextOpaque(context);
        return bridge != NULL && bridge->host != NULL
            && tilefinch_game_audio_update_voice(
                bridge->host->game_audio, handle, gain_left, gain_right)
            ? JS_TRUE : JS_FALSE;
    }
    case TILEFINCH_GAME_AUDIO_COMMAND_SET_FREQUENCY: {
        double frequency = 0;
        if (argc < 3
            || JS_ToFloat64(context, &frequency, argv[2]) < 0)
            return JS_FALSE;
        bridge = JS_GetContextOpaque(context);
        return bridge != NULL && bridge->host != NULL
            && tilefinch_game_audio_update_oscillator(
                bridge->host->game_audio, handle, frequency)
            ? JS_TRUE : JS_FALSE;
    }
    case TILEFINCH_GAME_AUDIO_COMMAND_GAIN_TARGET: {
        double gain_left = 0, gain_right = 0;
        double delay = 0, time_constant = 0;
        if (argc < 6
            || JS_ToFloat64(context, &gain_left, argv[2]) < 0
            || JS_ToFloat64(context, &gain_right, argv[3]) < 0
            || JS_ToFloat64(context, &delay, argv[4]) < 0
            || JS_ToFloat64(context, &time_constant, argv[5]) < 0)
            return JS_FALSE;
        bridge = JS_GetContextOpaque(context);
        return bridge != NULL && bridge->host != NULL
            && tilefinch_game_audio_schedule_envelope_target(
                bridge->host->game_audio, handle, gain_left, gain_right,
                delay, time_constant)
            ? JS_TRUE : JS_FALSE;
    }
    case TILEFINCH_GAME_AUDIO_COMMAND_CANCEL_GAIN:
        bridge = JS_GetContextOpaque(context);
        return bridge != NULL && bridge->host != NULL
            && tilefinch_game_audio_cancel_envelope(
                bridge->host->game_audio, handle) ? JS_TRUE : JS_FALSE;
    case TILEFINCH_GAME_AUDIO_COMMAND_CANCEL_PITCH:
        bridge = JS_GetContextOpaque(context);
        return bridge != NULL && bridge->host != NULL
            && tilefinch_game_audio_cancel_pitch_curve(
                bridge->host->game_audio, handle) ? JS_TRUE : JS_FALSE;
    case TILEFINCH_GAME_AUDIO_COMMAND_CURVE: {
        int32_t pitch = 0;
        double left = 1, right = 1, delay = 0, duration = 0;
        if (argc < 8) return JS_FALSE;
        if (JS_ToInt32(context, &pitch, argv[2]) < 0
            || JS_ToFloat64(context, &left, argv[4]) < 0
            || JS_ToFloat64(context, &right, argv[5]) < 0
            || JS_ToFloat64(context, &delay, argv[6]) < 0
            || JS_ToFloat64(context, &duration, argv[7]) < 0)
            return JS_EXCEPTION;
        if (pitch != 0 && pitch != 1) return JS_FALSE;
        /* Coercion may close the context or detach the view. Collect bytes
           only afterward, copy them before releasing the backing buffer,
           and resolve the runtime-owned engine last. */
        size_t offset = 0, length = 0, element_size = 0, storage = 0;
        JSValue buffer = JS_GetTypedArrayBuffer(
            context, argv[3], &offset, &length, &element_size);
        if (JS_IsException(buffer)) return JS_EXCEPTION;
        uint8_t *bytes = JS_GetArrayBuffer(context, &storage, buffer);
        float values[TILEFINCH_GAME_AUDIO_CURVE_POINT_LIMIT];
        bool valid = bytes != NULL && element_size == sizeof(float)
            && offset <= storage && length <= storage - offset
            && length % sizeof(float) == 0u
            && length / sizeof(float) >= 2u
            && length / sizeof(float) <= TILEFINCH_GAME_AUDIO_CURVE_POINT_LIMIT;
        if (valid) memcpy(values, bytes + offset, length);
        JS_FreeValue(context, buffer);
        if (!valid) return JS_FALSE;
        for (size_t at = 0; at < length / sizeof(float); at++) {
            uint32_t bits = 0u;
            memcpy(&bits, &values[at], sizeof(bits));
            if ((bits & UINT32_C(0x7fffffff)) >= UINT32_C(0x7f800000))
                return JS_ThrowTypeError(context,
                    "Audio curve samples must be finite");
        }
        bridge = JS_GetContextOpaque(context);
        return bridge != NULL && bridge->host != NULL
            && tilefinch_game_audio_schedule_curve(
                bridge->host->game_audio, handle, pitch != 0, values,
                length / sizeof(float), left, right, delay, duration)
            ? JS_TRUE : JS_FALSE;
    }
    case TILEFINCH_GAME_AUDIO_COMMAND_START_BUFFER: {
        double offset = 0, duration = 0, rate = 1;
        double gain_left = 1, gain_right = 1;
        double loop_start = 0, loop_end = 0, delay = 0;
        int loop = 0;
        if (argc < 11
            || JS_ToFloat64(context, &offset, argv[2]) < 0
            || JS_ToFloat64(context, &duration, argv[3]) < 0
            || JS_ToFloat64(context, &rate, argv[4]) < 0
            || JS_ToFloat64(context, &gain_left, argv[5]) < 0
            || JS_ToFloat64(context, &gain_right, argv[6]) < 0
            || (loop = JS_ToBool(context, argv[7])) < 0
            || JS_ToFloat64(context, &loop_start, argv[8]) < 0
            || JS_ToFloat64(context, &loop_end, argv[9]) < 0
            || JS_ToFloat64(context, &delay, argv[10]) < 0) return JS_FALSE;
        bridge = JS_GetContextOpaque(context);
        if (bridge == NULL || bridge->host == NULL) return JS_FALSE;
        uint32_t voice = 0;
        if (!tilefinch_game_audio_start(
                bridge->host->game_audio, handle, offset, duration, rate,
                gain_left, gain_right, loop > 0, loop_start, loop_end, delay,
                &voice)) return JS_FALSE;
        return JS_NewUint32(context, voice);
    }
    default:
        return JS_FALSE;
    }
}

#ifdef TILEFINCH_PSP_VALIDATION_LOG
JSValue js_game_audio_command(JSContext *context,
                             JSValueConst this_value,
                             int argc, JSValueConst *argv)
{
    if (!game_audio_native_measurement)
        return js_game_audio_command_impl(context, this_value, argc, argv);
    /* Observe an already-integer opcode without repeating author coercion.
       Slot 12 is unknown/non-integer input. Reentrant calls are counted but
       not timed twice; their time belongs to the outer native boundary. */
    _Static_assert(SCRIPT_GAME_AUDIO_NATIVE_COMMANDS
                       == TILEFINCH_GAME_AUDIO_COMMAND_COUNT + 1u,
                   "one metrics slot per command plus the unknown slot");
    unsigned slot = SCRIPT_GAME_AUDIO_NATIVE_COMMANDS - 1u;
    if (argc > 0 && JS_VALUE_GET_TAG(argv[0]) == JS_TAG_INT) {
        int command = JS_VALUE_GET_INT(argv[0]);
        if (command >= 0 && command < (int) slot) slot = (unsigned) command;
    }
    bool outer = game_audio_native_depth++ == 0;
    uint64_t before = outer ? tilefinch_platform_monotonic_time_us() : 0;
    JSValue result = js_game_audio_command_impl(context, this_value, argc, argv);
    uint64_t after = outer ? tilefinch_platform_monotonic_time_us() : 0;
    game_audio_native_depth--;
    if (outer) {
        game_audio_native_metrics.command_calls[slot]++;
        if (after >= before)
            game_audio_native_metrics.command_us[slot] += after - before;
    } else {
        game_audio_native_metrics.nested_calls++;
    }
    return result;
}
#endif
#endif /* TILEFINCH_GAME_AUDIO_REFERENCE */
