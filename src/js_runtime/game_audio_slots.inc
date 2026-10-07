/* Bounded native AudioNode/AudioParam state for browser audio scheduling.
   The JavaScript reference remains selectable by trusted runtime options. */
typedef struct AudioSlotRuntime AudioSlotRuntime;

enum {
    AUDIO_SLOT_NODE_DEST,
    AUDIO_SLOT_NODE_GAIN,
    AUDIO_SLOT_NODE_PAN,
    AUDIO_SLOT_NODE_OSC,
    AUDIO_SLOT_NODE_BUFFER
};

enum {
    AUDIO_SLOT_PARAM_GAIN,
    AUDIO_SLOT_PARAM_PAN,
    AUDIO_SLOT_PARAM_FREQUENCY,
    AUDIO_SLOT_PARAM_DETUNE,
    AUDIO_SLOT_PARAM_RATE
};
typedef struct AudioSlotContext AudioSlotContext;

typedef struct {
    bool used, started, loop;
    unsigned generation, kind, type;
    uint32_t voice, cache_revision;
    uint8_t source_mask;
    double values[5], defaults[5], loop_start, loop_end;
    JSValue target, buffer, params[5];
} AudioSlotNode;

struct AudioSlotContext {
    AudioSlotRuntime *screen;
    unsigned references, engine_epoch, revision, node_count, source_count;
    bool closed, running;
    double elapsed, running_started;
    AudioSlotNode nodes[16];
    unsigned generations[16];
    unsigned active_indices[4];
    JSValue active[4], destination, param_proto, buffer_proto, notify;
};

typedef struct {
    AudioSlotContext *owner;
    JSValue context;
    unsigned index, generation;
} AudioSlotNodeRef;

typedef struct {
    JSValue node;
    unsigned kind;
} AudioSlotParamRef;

typedef struct {
    JSValue context;
    TilefinchGameAudioBufferInfo info;
    unsigned engine_epoch;
} AudioSlotBufferRef;

struct AudioSlotRuntime {
    ScriptRuntime *host;
    Budget *budget;
    JSClassID context_class, node_class, slot_param_class, buffer_class, float_class;
    unsigned epoch, context_records, node_records, param_records, buffer_records;
    bool retired;
    JSValue prototypes[10], notify_value, dom_value, promise_value, resolve_value, reject_value,
        active_context_root;
    JSAtom iterator_atom;
};

/* QuickJS class IDs are process-global. Reuse the same four IDs across
   navigations; only each runtime's registrations and records are fresh. */
static JSClassID audio_slot_class_ids[4];

static void *audio_slot_allocate(AudioSlotRuntime *screen, size_t size)
{
#ifndef __PSP__
    unsigned *remaining = &tilefinch_test_faults()->audio_slot_allocation_fail_at;
    if (*remaining && --*remaining == 0)
        return NULL;
#endif
    return budget_calloc_category(screen->budget, BUDGET_CATEGORY_JAVASCRIPT, 1, size);
}

static AudioSlotRuntime *audio_slot_state_context(JSContext *ctx)
{
    DomBridge *b = JS_GetContextOpaque(ctx);
    return b && b->host ? b->host->game_audio_slots : NULL;
}

static AudioSlotRuntime *audio_slot_state_runtime(JSRuntime *rt)
{
    ScriptRuntime *host = JS_GetRuntimeOpaque(rt);
    return host ? host->game_audio_slots : NULL;
}

static bool audio_slot_create_engine(AudioSlotRuntime *s)
{
    return s && !s->retired && js_rt_game_audio_engine(s->host) != NULL;
}

static double audio_slot_source3_clock(JSContext *ctx, AudioSlotRuntime *s)
{
    (void) ctx;
    return performance_clock_value(&s->host->bridge, true, SCRIPT_CLOCK_SOURCE_PERFORMANCE_NOW);
}

static bool audio_slot_equal(JSValueConst a, JSValueConst b)
{
    return JS_VALUE_GET_TAG(a) == JS_VALUE_GET_TAG(b) && JS_VALUE_GET_PTR(a) == JS_VALUE_GET_PTR(b);
}

static AudioSlotContext *audio_slot_context(JSContext *ctx, JSValueConst obj)
{
    AudioSlotRuntime *screen = audio_slot_state_context(ctx);
    if (!screen) {
        JS_ThrowTypeError(ctx, "Audio slots are unavailable");
        return NULL;
    }
    return JS_GetOpaque2(ctx, obj, screen->context_class);
}

static AudioSlotNode *audio_slot_node(JSContext *ctx, JSValueConst obj, AudioSlotNodeRef **out)
{
    AudioSlotRuntime *screen = audio_slot_state_context(ctx);
    if (!screen) {
        JS_ThrowTypeError(ctx, "Audio slots are unavailable");
        return NULL;
    }
    AudioSlotNodeRef *ref = JS_GetOpaque2(ctx, obj, screen->node_class);
    if (!ref)
        return NULL;
    AudioSlotNode *node = &ref->owner->nodes[ref->index];
    if (!node->used || node->generation != ref->generation) {
        JS_ThrowTypeError(ctx, "Retired audio node");
        return NULL;
    }
    if (out)
        *out = ref;
    return node;
}

static void audio_slot_context_release(AudioSlotContext *owner)
{
    AudioSlotRuntime *screen = owner->screen;
    if (--owner->references == 0) {
        screen->context_records--;
        budget_free(screen->budget, owner);
    }
}

static void audio_slot_node_mark(JSRuntime *rt, JSValueConst object, JS_MarkFunc *mark)
{
    AudioSlotRuntime *screen = audio_slot_state_runtime(rt);
    AudioSlotNodeRef *r = JS_GetOpaque(object, screen->node_class);
    if (!r)
        return;
    JS_MarkValue(rt, r->context, mark);
    AudioSlotNode *n = &r->owner->nodes[r->index];
    if (!n->used || n->generation != r->generation)
        return;
    JS_MarkValue(rt, n->target, mark);
    JS_MarkValue(rt, n->buffer, mark);
    for (unsigned i = 0; i < 5; i++)
        JS_MarkValue(rt, n->params[i], mark);
}

static void audio_slot_node_finalizer(JSRuntime *rt, JSValue object)
{
    AudioSlotRuntime *screen = audio_slot_state_runtime(rt);
    AudioSlotNodeRef *r = JS_GetOpaque(object, screen->node_class);
    if (!r)
        return;
    AudioSlotContext *owner = r->owner;
    AudioSlotNode *n = &owner->nodes[r->index];
    if (n->used && n->generation == r->generation) {
        JSValue target = n->target, buffer = n->buffer, params[5];
        for (unsigned i = 0; i < 5; i++) {
            params[i] = n->params[i];
            n->params[i] = JS_UNDEFINED;
        }
        n->target = n->buffer = JS_UNDEFINED;
        n->used = false;
        owner->node_count--;
        owner->revision++;
        JS_FreeValueRT(rt, target);
        JS_FreeValueRT(rt, buffer);
        for (unsigned i = 0; i < 5; i++)
            JS_FreeValueRT(rt, params[i]);
    }
    JS_FreeValueRT(rt, r->context);
    screen->node_records--;
    budget_free(screen->budget, r);
    audio_slot_context_release(owner);
}

static void audio_slot_context_mark(JSRuntime *rt, JSValueConst object, JS_MarkFunc *mark)
{
    AudioSlotRuntime *screen = audio_slot_state_runtime(rt);
    AudioSlotContext *o = JS_GetOpaque(object, screen->context_class);
    if (!o)
        return;
    JS_MarkValue(rt, o->destination, mark);
    JS_MarkValue(rt, o->param_proto, mark);
    JS_MarkValue(rt, o->buffer_proto, mark);
    JS_MarkValue(rt, o->notify, mark);
    for (unsigned i = 0; i < 4; i++)
        JS_MarkValue(rt, o->active[i], mark);
}

static void audio_slot_context_finalizer(JSRuntime *rt, JSValue object)
{
    AudioSlotRuntime *screen = audio_slot_state_runtime(rt);
    AudioSlotContext *o = JS_GetOpaque(object, screen->context_class);
    if (!o)
        return;
    JSValue destination = o->destination;
    o->destination = JS_UNDEFINED;
    JS_FreeValueRT(rt, destination);
    for (unsigned i = 0; i < 4; i++) {
        JSValue v = o->active[i];
        o->active[i] = JS_UNDEFINED;
        JS_FreeValueRT(rt, v);
    }
    JS_FreeValueRT(rt, o->param_proto);
    JS_FreeValueRT(rt, o->buffer_proto);
    JS_FreeValueRT(rt, o->notify);
    audio_slot_context_release(o); /* Never touch a mixer from a finalizer. */
}

static void audio_slot_param_mark(JSRuntime *rt, JSValueConst object, JS_MarkFunc *mark)
{
    AudioSlotRuntime *screen = audio_slot_state_runtime(rt);
    AudioSlotParamRef *r = JS_GetOpaque(object, screen->slot_param_class);
    if (r)
        JS_MarkValue(rt, r->node, mark);
}

static void audio_slot_param_finalizer(JSRuntime *rt, JSValue object)
{
    AudioSlotRuntime *screen = audio_slot_state_runtime(rt);
    AudioSlotParamRef *r = JS_GetOpaque(object, screen->slot_param_class);
    if (r) {
        JS_FreeValueRT(rt, r->node);
        screen->param_records--;
        budget_free(screen->budget, r);
    }
}

static void audio_slot_buffer_mark(JSRuntime *rt, JSValueConst object, JS_MarkFunc *mark)
{
    AudioSlotRuntime *screen = audio_slot_state_runtime(rt);
    AudioSlotBufferRef *r = JS_GetOpaque(object, screen->buffer_class);
    if (r)
        JS_MarkValue(rt, r->context, mark);
}

static void audio_slot_buffer_finalizer(JSRuntime *rt, JSValue object)
{
    AudioSlotRuntime *screen = audio_slot_state_runtime(rt);
    AudioSlotBufferRef *r = JS_GetOpaque(object, screen->buffer_class);
    if (r) {
        JS_FreeValueRT(rt, r->context);
        screen->buffer_records--;
        budget_free(screen->budget, r);
    }
}

static JSValue audio_slot_error(JSContext *ctx, const char *message, const char *name)
{
    AudioSlotRuntime *screen = audio_slot_state_context(ctx);
    JSValue args[] = {JS_NewString(ctx, message), JS_NewString(ctx, name)},
            e = JS_CallConstructor(ctx, screen->dom_value, 2, args);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    return JS_IsException(e) ? e : JS_Throw(ctx, e);
}

static bool audio_slot_live(JSContext *ctx, AudioSlotContext *o)
{
    AudioSlotRuntime *screen = audio_slot_state_context(ctx);
    if (screen->retired || o->closed || o->engine_epoch != screen->epoch) {
        audio_slot_error(ctx, "AudioContext is closed", "InvalidStateError");
        return false;
    }
    return true;
}

static double audio_slot_clock_time(JSContext *ctx, AudioSlotContext *o)
{
    double out = o->elapsed;
    if (o->running) {
        double ms = audio_slot_source3_clock(ctx, o->screen);
        if (JS_HasException(ctx))
            return NAN;
        out += (ms - o->running_started) / 1000;
    }
    return out;
}

static bool audio_slot_delay_at(JSContext *ctx, AudioSlotContext *o, double when, double *delay)
{
    if (!isfinite(when) || when < 0) {
        JS_ThrowTypeError(ctx, "Audio time must be a finite nonnegative number");
        return false;
    }
    if (!audio_slot_live(ctx, o))
        return false;
    double now = audio_slot_clock_time(ctx, o);
    if (JS_HasException(ctx))
        return false;
    double d = when - now;
    *delay = d > 0 ? d : 0;
    if (*delay > 10) {
        audio_slot_error(ctx, "Scheduling exceeds the bounded ten-second window",
                         "NotSupportedError");
        return false;
    }
    return true;
}

static uint32_t audio_slot_emit(JSContext *ctx, int op, unsigned argc, const double *v,
                                const float *points, size_t count)
{
    AudioSlotRuntime *screen = audio_slot_state_context(ctx);
    if (op == 11)
        for (size_t i = 0; i < count; i++)
            if (!isfinite(points[i])) {
                JS_ThrowTypeError(ctx, "Audio curve samples must be finite");
                return 0;
            }
    uint32_t voice = 0, h = argc > 1 ? (uint32_t) v[1] : 0;
    switch (op) {
    case 0:
        if (!js_rt_bridge_user_activation_is_active(&screen->host->bridge))
            return 0;
        return audio_slot_create_engine(screen) &&
               tilefinch_game_audio_resume(screen->host->game_audio);
    case 1:
        return js_rt_game_audio_lifecycle(screen->host, false);
    case 2:
        if (!js_rt_game_audio_lifecycle(screen->host, true))
            return 0;
        screen->epoch++;
        return 1;
    case 3:
        return tilefinch_game_audio_start(screen->host->game_audio, h, v[2], v[3], v[4], v[5], v[6],
                                          v[7] != 0, v[8], v[9], v[10], &voice)
                   ? voice
                   : 0;
    case 4:
        tilefinch_game_audio_stop(screen->host->game_audio, h, v[2]);
        return 1;
    case 5:
        return tilefinch_game_audio_start_oscillator(screen->host->game_audio,
                                                     (TilefinchGameAudioOscillatorType) h, v[2],
                                                     v[3], v[4], v[5], &voice)
                   ? voice
                   : 0;
    case 6:
        return tilefinch_game_audio_update_voice(screen->host->game_audio, h, v[2], v[3]);
    case 7:
        return tilefinch_game_audio_update_oscillator(screen->host->game_audio, h, v[2]);
    case 8:
        return tilefinch_game_audio_schedule_envelope_target(screen->host->game_audio, h, v[2],
                                                             v[3], v[4], v[5]);
    case 9:
        return tilefinch_game_audio_cancel_envelope(screen->host->game_audio, h);
    case 10:
        return tilefinch_game_audio_cancel_pitch_curve(screen->host->game_audio, h);
    case 11:
        return tilefinch_game_audio_schedule_curve(screen->host->game_audio, h, v[2] != 0, points,
                                                   count, v[4], v[5], v[6], v[7]);
    }
    return 0;
}

static double audio_slot_clamp(double v, double low, double high)
{
    return v < low ? low : v > high ? high : v;
}

static void audio_slot_source_mix(AudioSlotContext *o, AudioSlotNode *source,
                                  AudioSlotNode *override, double value, double *left,
                                  double *right)
{
    AudioSlotRuntime *screen = o->screen;
    double gain = 1, pan = 0;
    bool panner = false;
    JSValue target = source->target;
    for (unsigned visits = 0; !JS_IsUndefined(target) && visits < 4; visits++) {
        AudioSlotNodeRef *ref = JS_GetOpaque(target, screen->node_class);
        if (!ref)
            break;
        AudioSlotNode *n = &o->nodes[ref->index];
        if (!n->used || n->generation != ref->generation)
            break;
        if (n->kind == AUDIO_SLOT_NODE_GAIN)
            gain *= n == override ? value : n->values[AUDIO_SLOT_PARAM_GAIN];
        if (n->kind == AUDIO_SLOT_NODE_PAN) {
            panner = true;
            pan = audio_slot_clamp(pan + n->values[AUDIO_SLOT_PARAM_PAN], -1, 1);
        }
        target = n->target;
    }
    gain = audio_slot_clamp(gain, 0, 4);
    if (!panner) {
        *left = *right = gain;
        return;
    }
    double angle = (pan + 1) * 3.14159265358979323846 / 4;
    *left = audio_slot_clamp(gain * cos(angle), 0, 4);
    *right = audio_slot_clamp(gain * sin(angle), 0, 4);
}

static unsigned audio_slot_through(AudioSlotContext *o, AudioSlotNode *node)
{
    AudioSlotRuntime *screen = o->screen;
    if (node->cache_revision == o->revision)
        return node->source_mask;
    unsigned mask = 0;
    for (unsigned s = 0; s < o->source_count; s++) {
        AudioSlotNode *source = &o->nodes[o->active_indices[s]];
        JSValue target = source->target;
        for (unsigned visits = 0; !JS_IsUndefined(target) && visits < 4; visits++) {
            AudioSlotNodeRef *ref = JS_GetOpaque(target, screen->node_class);
            if (!ref)
                break;
            AudioSlotNode *n = &o->nodes[ref->index];
            if (!n->used || n->generation != ref->generation)
                break;
            if (n == node) {
                mask |= 1u << s;
                break;
            }
            target = n->target;
        }
    }
    node->cache_revision = o->revision;
    node->source_mask = (uint8_t) mask;
    return mask;
}

static void audio_slot_refresh(JSContext *ctx, AudioSlotContext *o, unsigned mask)
{
    for (unsigned i = 0; i < o->source_count; i++)
        if (mask & (1u << i)) {
            AudioSlotNode *source = &o->nodes[o->active_indices[i]];
            if (!source->voice)
                continue;
            double l, r;
            audio_slot_source_mix(o, source, NULL, 0, &l, &r);
            double v[] = {6, source->voice, l, r};
            audio_slot_emit(ctx, 6, 4, v, NULL, 0);
            if (JS_HasException(ctx))
                return;
        }
}

static double audio_slot_effective(AudioSlotNode *n)
{
    return n->values[AUDIO_SLOT_PARAM_DETUNE] == 0
               ? n->values[AUDIO_SLOT_PARAM_FREQUENCY]
               : n->values[AUDIO_SLOT_PARAM_FREQUENCY] *
                     pow(2, n->values[AUDIO_SLOT_PARAM_DETUNE] / 1200);
}

static JSValue audio_slot_make_node(JSContext *ctx, JSValueConst context, JSValueConst proto,
                                    unsigned kind)
{
    AudioSlotRuntime *screen = audio_slot_state_context(ctx);
    AudioSlotContext *o = audio_slot_context(ctx, context);
    if (!o || !audio_slot_live(ctx, o))
        return JS_EXCEPTION;
    if (o->node_count == 16)
        return audio_slot_error(ctx, "Audio node limit reached", "QuotaExceededError");
    unsigned index = 0;
    while (index < 16 && o->nodes[index].used)
        index++;
    AudioSlotNodeRef *r = audio_slot_allocate(screen, sizeof(*r));
    if (!r)
        return JS_ThrowOutOfMemory(ctx);
    JSValueConst effective_proto =
        JS_IsObject(proto) ? proto
                           : screen->prototypes[kind == AUDIO_SLOT_NODE_DEST ? 7 : kind + 2];
    JSValue object = JS_NewObjectProtoClass(ctx, effective_proto, screen->node_class);
    if (JS_IsException(object)) {
        budget_free(screen->budget, r);
        return object;
    }
    AudioSlotNode *n = &o->nodes[index];
    memset(n, 0, sizeof(*n));
    n->used = true;
    n->kind = kind;
    n->type = 1;
    n->generation = ++o->generations[index];
    n->cache_revision = UINT32_MAX;
    n->target = n->buffer = JS_UNDEFINED;
    for (unsigned i = 0; i < 5; i++)
        n->params[i] = JS_UNDEFINED;
    n->values[AUDIO_SLOT_PARAM_GAIN] = n->defaults[AUDIO_SLOT_PARAM_GAIN] = 1;
    n->values[AUDIO_SLOT_PARAM_FREQUENCY] = n->defaults[AUDIO_SLOT_PARAM_FREQUENCY] = 440;
    n->values[AUDIO_SLOT_PARAM_RATE] = n->defaults[AUDIO_SLOT_PARAM_RATE] = 1;
    r->owner = o;
    r->context = JS_DupValue(ctx, context);
    r->index = index;
    r->generation = n->generation;
    o->references++;
    o->node_count++;
    screen->node_records++;
    JS_SetOpaque(object, r);
    if (JS_DefinePropertyValueStr(ctx, object, "channelCount", JS_NewInt32(ctx, 2),
                                  JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE | JS_PROP_ENUMERABLE) <
            0 ||
        (kind == AUDIO_SLOT_NODE_DEST &&
         JS_DefinePropertyValueStr(ctx, object, "maxChannelCount", JS_NewInt32(ctx, 2),
                                   JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE) < 0) ||
        ((kind == AUDIO_SLOT_NODE_OSC || kind == AUDIO_SLOT_NODE_BUFFER) &&
         JS_DefinePropertyValueStr(ctx, object, "onended", JS_NULL,
                                   JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE | JS_PROP_ENUMERABLE) <
             0)) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    return object;
}

static bool audio_slot_read_number(JSContext *ctx, JSValueConst options, const char *name,
                                   double fallback, double *out)
{
    JSValue v = JS_GetPropertyStr(ctx, options, name);
    if (JS_IsException(v))
        return false;
    int status = 0;
    if (JS_IsUndefined(v) || JS_IsNull(v))
        *out = fallback;
    else
        status = JS_ToFloat64(ctx, out, v);
    JS_FreeValue(ctx, v);
    return status == 0;
}

static JSValue audio_slot_create_node(JSContext *ctx, JSValueConst self, int argc,
                                      JSValueConst *argv)
{
    AudioSlotRuntime *screen = audio_slot_state_context(ctx);
    (void) self;
    if (argc != 4)
        return JS_ThrowTypeError(ctx, "private node factory");
    AudioSlotContext *o = audio_slot_context(ctx, argv[0]);
    if (!o)
        return JS_EXCEPTION;
    uint32_t kind = 0;
    if (JS_ToUint32(ctx, &kind, argv[2]) < 0 || kind < 1 || kind > 4)
        return JS_ThrowTypeError(ctx, "private node kind");
    /* Preserve the reference shim's refusal precedence before option getters.
       The post-getter live/make_node checks remain for reentrant getters. */
    if (!audio_slot_live(ctx, o))
        return JS_EXCEPTION;
    if (o->node_count == 16)
        return audio_slot_error(ctx, "Too many audio nodes", "QuotaExceededError");
    double gain = 1, pan = 0, freq = 440, detune = 0, rate = 1, loop_start = 0, loop_end = 0;
    bool loop = false;
    unsigned type = 1;
    JSValue buffer = JS_UNDEFINED;
    if (kind == AUDIO_SLOT_NODE_GAIN && !audio_slot_read_number(ctx, argv[3], "gain", 1, &gain))
        return JS_EXCEPTION;
    if (kind == AUDIO_SLOT_NODE_PAN && !audio_slot_read_number(ctx, argv[3], "pan", 0, &pan))
        return JS_EXCEPTION;
    if (kind == AUDIO_SLOT_NODE_OSC) {
        JSValue v = JS_GetPropertyStr(ctx, argv[3], "type");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        bool default_type = JS_IsUndefined(v) || JS_IsNull(v);
        const char *text = default_type ? "sine" : JS_ToCString(ctx, v);
        if (!text) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        const char *types[] = {"sine", "square", "sawtooth", "triangle"};
        type = 0;
        for (unsigned i = 0; i < 4; i++)
            if (!strcasecmp(text, types[i]))
                type = i + 1;
        if (!default_type)
            JS_FreeCString(ctx, text);
        JS_FreeValue(ctx, v);
        if (!type)
            return audio_slot_error(ctx, "Unsupported oscillator type", "InvalidStateError");
        if (!audio_slot_read_number(ctx, argv[3], "frequency", 440, &freq) ||
            !audio_slot_read_number(ctx, argv[3], "detune", 0, &detune))
            return JS_EXCEPTION;
    }
    if (kind == AUDIO_SLOT_NODE_BUFFER) {
        buffer = JS_GetPropertyStr(ctx, argv[3], "buffer");
        if (JS_IsException(buffer))
            return JS_EXCEPTION;
        JSValue v = JS_GetPropertyStr(ctx, argv[3], "loop");
        if (JS_IsException(v)) {
            JS_FreeValue(ctx, buffer);
            return JS_EXCEPTION;
        }
        loop = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        if (!audio_slot_read_number(ctx, argv[3], "loopStart", 0, &loop_start) ||
            !audio_slot_read_number(ctx, argv[3], "loopEnd", 0, &loop_end) ||
            !audio_slot_read_number(ctx, argv[3], "playbackRate", 1, &rate) ||
            !audio_slot_read_number(ctx, argv[3], "detune", 0, &detune)) {
            JS_FreeValue(ctx, buffer);
            return JS_EXCEPTION;
        }
    }
    if (!audio_slot_live(ctx, o)) {
        JS_FreeValue(ctx, buffer);
        return JS_EXCEPTION;
    }
    JSValue object = audio_slot_make_node(ctx, argv[0], argv[1], kind);
    if (JS_IsException(object)) {
        JS_FreeValue(ctx, buffer);
        return object;
    }
    AudioSlotNode *n = audio_slot_node(ctx, object, NULL);
    n->values[AUDIO_SLOT_PARAM_GAIN] = n->defaults[AUDIO_SLOT_PARAM_GAIN] = gain;
    n->values[AUDIO_SLOT_PARAM_PAN] = n->defaults[AUDIO_SLOT_PARAM_PAN] = pan;
    n->values[AUDIO_SLOT_PARAM_FREQUENCY] = n->defaults[AUDIO_SLOT_PARAM_FREQUENCY] = freq;
    n->values[AUDIO_SLOT_PARAM_DETUNE] = n->defaults[AUDIO_SLOT_PARAM_DETUNE] = detune;
    n->values[AUDIO_SLOT_PARAM_RATE] = n->defaults[AUDIO_SLOT_PARAM_RATE] = rate;
    n->type = type;
    n->loop = loop;
    n->loop_start = fmax(0, loop_start);
    n->loop_end = fmax(0, loop_end);
    if (!JS_IsUndefined(buffer) && !JS_IsNull(buffer)) {
        AudioSlotBufferRef *b = JS_GetOpaque(buffer, screen->buffer_class);
        if (!b || !audio_slot_equal(b->context, argv[0])) {
            JS_FreeValue(ctx, buffer);
            JS_FreeValue(ctx, object);
            return JS_ThrowTypeError(ctx, "AudioBuffer owner mismatch");
        }
        n->buffer = buffer;
    } else
        JS_FreeValue(ctx, buffer);
    return object;
}

static JSValue audio_slot_create_context(JSContext *ctx, JSValueConst self, int argc,
                                         JSValueConst *argv)
{
    AudioSlotRuntime *screen = audio_slot_state_context(ctx);
    (void) self;
    if (argc != 2)
        return JS_ThrowTypeError(ctx, "private context factory");
    bool exclusive = JS_ToBool(ctx, argv[1]);
    if (exclusive && !JS_IsUndefined(screen->active_context_root))
        return audio_slot_error(ctx, "Only one game AudioContext is available",
                                "QuotaExceededError");
    AudioSlotContext *o = audio_slot_allocate(screen, sizeof(*o));
    if (!o)
        return JS_ThrowOutOfMemory(ctx);
    o->screen = audio_slot_state_context(ctx);
    o->references = 1;
    o->engine_epoch = screen->epoch;
    o->destination = JS_UNDEFINED;
    for (unsigned i = 0; i < 4; i++)
        o->active[i] = JS_UNDEFINED;
    o->param_proto = JS_DupValue(ctx, screen->prototypes[8]);
    o->buffer_proto = JS_DupValue(ctx, screen->prototypes[9]);
    o->notify = JS_DupValue(ctx, screen->notify_value);
    JSValue object = JS_NewObjectProtoClass(
        ctx, JS_IsObject(argv[0]) ? argv[0] : screen->prototypes[1], screen->context_class);
    if (JS_IsException(object)) {
        JS_FreeValue(ctx, o->param_proto);
        JS_FreeValue(ctx, o->buffer_proto);
        JS_FreeValue(ctx, o->notify);
        budget_free(screen->budget, o);
        return object;
    }
    JS_SetOpaque(object, o);
    screen->context_records++;
    if (JS_DefinePropertyValueStr(ctx, object, "onstatechange", JS_NULL,
                                  JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE | JS_PROP_ENUMERABLE) <
        0) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    o->destination = audio_slot_make_node(ctx, object, screen->prototypes[7], AUDIO_SLOT_NODE_DEST);
    if (JS_IsException(o->destination)) {
        o->destination = JS_UNDEFINED;
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    if (exclusive)
        screen->active_context_root = JS_DupValue(ctx, object);
    return object;
}

static JSValue audio_slot_context_get(JSContext *ctx, JSValueConst self, int argc,
                                      JSValueConst *argv, int magic, JSValue *data)
{
    (void) argc;
    (void) argv;
    (void) data;
    AudioSlotContext *o = audio_slot_context(ctx, self);
    if (!o)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        return JS_NewString(ctx, o->closed ? "closed" : o->running ? "running" : "suspended");
    case 1: {
        double value = audio_slot_clock_time(ctx, o);
        return JS_HasException(ctx) ? JS_EXCEPTION : JS_NewFloat64(ctx, value);
    }
    case 2:
        return JS_DupValue(ctx, o->destination);
    case 3:
        return JS_NewInt32(ctx, 44100);
    }
    return JS_UNDEFINED;
}

static void audio_slot_drop_active(JSContext *ctx, AudioSlotContext *o)
{
    for (unsigned i = 0; i < o->source_count; i++) {
        o->nodes[o->active_indices[i]].voice = 0;
        JSValue source = o->active[i];
        o->active[i] = JS_UNDEFINED;
        JS_FreeValue(ctx, source);
    }
    o->source_count = 0;
    o->revision++;
}

static JSValue audio_slot_notify(JSContext *ctx, AudioSlotContext *o, JSValueConst receiver,
                                 const char *event)
{
    JSValue args[] = {JS_DupValue(ctx, receiver), JS_NewString(ctx, event)},
            result = JS_Call(ctx, o->notify, JS_UNDEFINED, 2, args);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    return result;
}

static JSValue audio_slot_context_action(JSContext *ctx, JSValueConst self, int argc,
                                         JSValueConst *argv, int op, JSValue *data)
{
    AudioSlotRuntime *screen = audio_slot_state_context(ctx);
    (void) argc;
    (void) argv;
    (void) data;
    AudioSlotContext *o = audio_slot_context(ctx, self);
    if (!o)
        return JS_EXCEPTION;
    bool changed = false;
    if (o->closed && op != 2)
        audio_slot_error(ctx, "AudioContext is closed", "InvalidStateError");
    else if (op == 0 && !o->running) {
        if (!JS_HasException(ctx) && audio_slot_live(ctx, o)) {
            double v[] = {0};
            if (!audio_slot_emit(ctx, 0, 1, v, NULL, 0))
                audio_slot_error(ctx, "Audio resume requires user activation", "NotAllowedError");
            else {
                o->running = true;
                o->running_started = audio_slot_source3_clock(ctx, screen);
                changed = true;
            }
        }
    } else if (op == 1 && o->running) {
        double elapsed = audio_slot_clock_time(ctx, o);
        if (!JS_HasException(ctx)) {
            double v[] = {1};
            if (audio_slot_emit(ctx, 1, 1, v, NULL, 0)) {
                o->elapsed = elapsed;
                o->running = false;
                changed = true;
            } else if (!JS_HasException(ctx))
                audio_slot_error(ctx, "Native audio lifecycle publication refused",
                                 "InvalidStateError");
        }
    } else if (op == 2 && !o->closed) {
        double v[] = {2};
        bool published = o->engine_epoch != screen->epoch || audio_slot_emit(ctx, 2, 1, v, NULL, 0);
        if (published && !JS_HasException(ctx)) {
            o->closed = true;
            o->running = false;
            audio_slot_drop_active(ctx, o);
            if (audio_slot_equal(self, screen->active_context_root)) {
                JSValue root = screen->active_context_root;
                screen->active_context_root = JS_UNDEFINED;
                JS_FreeValue(ctx, root);
            }
            changed = true;
        } else if (!JS_HasException(ctx))
            audio_slot_error(ctx, "Native audio lifecycle publication refused",
                             "InvalidStateError");
    }
    if (!JS_HasException(ctx) && changed) {
        JSValue result = audio_slot_notify(ctx, o, self, "statechange");
        if (JS_IsException(result) && screen->host->watchdog.interrupted)
            return result;
        JS_FreeValue(ctx, result);
    }
    JSValue value = JS_HasException(ctx) ? JS_GetException(ctx) : JS_UNDEFINED;
    bool error = !JS_IsUndefined(value);
    JSValue result = JS_Call(ctx, error ? screen->reject_value : screen->resolve_value,
                             screen->promise_value, 1, &value);
    JS_FreeValue(ctx, value);
    return result;
}

static JSValue audio_slot_node_connect(JSContext *ctx, JSValueConst self, int argc,
                                       JSValueConst *argv, int op, JSValue *data)
{
    (void) data;
    AudioSlotNodeRef *r;
    AudioSlotNode *n = audio_slot_node(ctx, self, &r);
    if (!n)
        return JS_EXCEPTION;
    if (!audio_slot_live(ctx, r->owner))
        return JS_EXCEPTION;
    JSValue target = JS_UNDEFINED;
    if (!op) {
        if (argc < 1)
            return JS_ThrowTypeError(ctx, "AudioNode destination required");
        AudioSlotNodeRef *to;
        AudioSlotNode *node = audio_slot_node(ctx, argv[0], &to);
        if (!node)
            return JS_EXCEPTION;
        if (to->owner != r->owner)
            return JS_ThrowTypeError(ctx, "Audio nodes must share a context");
        target = JS_DupValue(ctx, argv[0]);
    }
    JSValue old = n->target;
    n->target = target;
    r->owner->revision++;
    JS_FreeValue(ctx, old);
    audio_slot_refresh(ctx, r->owner, (1u << r->owner->source_count) - 1);
    return JS_HasException(ctx) ? JS_EXCEPTION : op ? JS_UNDEFINED : JS_DupValue(ctx, argv[0]);
}

static JSValue audio_slot_node_get(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv,
                                   int op, JSValue *data)
{
    AudioSlotRuntime *screen = audio_slot_state_context(ctx);
    (void) argc;
    (void) argv;
    (void) data;
    AudioSlotNodeRef *r;
    AudioSlotNode *n = audio_slot_node(ctx, self, &r);
    if (!n)
        return JS_EXCEPTION;
    unsigned required = (unsigned) op / 100;
    op %= 100;
    if (required && n->kind != required)
        return JS_ThrowTypeError(ctx, "Audio node subtype mismatch");
    if (op == 0)
        return JS_DupValue(ctx, r->context);
    if (op == 1)
        return JS_IsUndefined(n->buffer) ? JS_NULL : JS_DupValue(ctx, n->buffer);
    if (op == 2)
        return JS_NewBool(ctx, n->loop);
    if (op == 3)
        return JS_NewFloat64(ctx, n->loop_start);
    if (op == 4)
        return JS_NewFloat64(ctx, n->loop_end);
    if (op == 5) {
        const char *types[] = {"sine", "square", "sawtooth", "triangle"};
        return JS_NewString(ctx, types[n->type - 1]);
    }
    unsigned kind = (unsigned) op - 10;
    if (kind > 4)
        return JS_UNDEFINED;
    if (JS_IsUndefined(n->params[kind])) {
        AudioSlotParamRef *p = audio_slot_allocate(screen, sizeof(*p));
        if (!p)
            return JS_ThrowOutOfMemory(ctx);
        JSValue obj = JS_NewObjectProtoClass(ctx, r->owner->param_proto, screen->slot_param_class);
        if (JS_IsException(obj)) {
            budget_free(screen->budget, p);
            return obj;
        }
        p->node = JS_DupValue(ctx, self);
        p->kind = kind;
        JS_SetOpaque(obj, p);
        n->params[kind] = obj;
        screen->param_records++;
    }
    return JS_DupValue(ctx, n->params[kind]);
}

static JSValue audio_slot_node_set(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv,
                                   int op, JSValue *data)
{
    AudioSlotRuntime *screen = audio_slot_state_context(ctx);
    (void) data;
    AudioSlotNodeRef *r;
    AudioSlotNode *n = audio_slot_node(ctx, self, &r);
    if (!n)
        return JS_EXCEPTION;
    JSValueConst value = argc ? argv[0] : JS_UNDEFINED;
    unsigned required = (unsigned) op / 100;
    op %= 100;
    if (required && n->kind != required)
        return JS_ThrowTypeError(ctx, "Audio node subtype mismatch");
    if (op == 1) {
        AudioSlotBufferRef *b =
            JS_IsNull(value) ? NULL : JS_GetOpaque2(ctx, value, screen->buffer_class);
        if (!b && !JS_IsNull(value))
            return JS_EXCEPTION;
        if (b && !audio_slot_equal(b->context, r->context))
            return JS_ThrowTypeError(ctx, "AudioBuffer owner mismatch");
        if (!audio_slot_live(ctx, r->owner))
            return JS_EXCEPTION;
        JSValue old = n->buffer;
        n->buffer = JS_IsNull(value) ? JS_UNDEFINED : JS_DupValue(ctx, value);
        JS_FreeValue(ctx, old);
        return JS_UNDEFINED;
    }
    if (op == 2) {
        if (!audio_slot_live(ctx, r->owner))
            return JS_EXCEPTION;
        n->loop = JS_ToBool(ctx, value);
        return JS_UNDEFINED;
    }
    if (op == 5) {
        const char *text = JS_ToCString(ctx, value);
        if (!text)
            return JS_EXCEPTION;
        const char *types[] = {"sine", "square", "sawtooth", "triangle"};
        unsigned type = 0;
        for (unsigned i = 0; i < 4; i++)
            if (!strcasecmp(text, types[i]))
                type = i + 1;
        JS_FreeCString(ctx, text);
        if (!audio_slot_live(ctx, r->owner))
            return JS_EXCEPTION;
        if (!type)
            return audio_slot_error(ctx, "Unsupported oscillator type", "InvalidStateError");
        n->type = type;
        return JS_UNDEFINED;
    }
    double v = 0;
    if (JS_ToFloat64(ctx, &v, value) < 0 || !audio_slot_live(ctx, r->owner))
        return JS_EXCEPTION;
    if (op == 3)
        n->loop_start = v;
    if (op == 4)
        n->loop_end = v;
    return JS_UNDEFINED;
}

static JSValue audio_slot_source_action(JSContext *ctx, JSValueConst self, int argc,
                                        JSValueConst *argv, int stop, JSValue *data)
{
    AudioSlotRuntime *screen = audio_slot_state_context(ctx);
    (void) data;
    AudioSlotNodeRef *r;
    AudioSlotNode *n = audio_slot_node(ctx, self, &r);
    if (!n)
        return JS_EXCEPTION;
    if (n->kind != AUDIO_SLOT_NODE_OSC && n->kind != AUDIO_SLOT_NODE_BUFFER)
        return JS_ThrowTypeError(ctx, "Scheduled source required");
    unsigned required = (unsigned) stop / 100;
    stop %= 100;
    if (required && n->kind != required)
        return JS_ThrowTypeError(ctx, "Audio node subtype mismatch");
    double when = 0, offset = 0, duration = 0;
    if (argc && !JS_IsUndefined(argv[0]) && JS_ToFloat64(ctx, &when, argv[0]) < 0)
        return JS_EXCEPTION;
    if (!stop && n->kind == AUDIO_SLOT_NODE_BUFFER) {
        if (argc > 1 && !JS_IsUndefined(argv[1]) && JS_ToFloat64(ctx, &offset, argv[1]) < 0)
            return JS_EXCEPTION;
        if (argc > 2 && !JS_IsUndefined(argv[2]) && JS_ToFloat64(ctx, &duration, argv[2]) < 0)
            return JS_EXCEPTION;
    }
    if (!audio_slot_live(ctx, r->owner))
        return JS_EXCEPTION;
    if (stop) {
        if (!n->started)
            return audio_slot_error(ctx, "Source has not started", "InvalidStateError");
        if (n->voice) {
            double delay;
            if (!audio_slot_delay_at(ctx, r->owner, when, &delay))
                return JS_EXCEPTION;
            double v[] = {4, n->voice, delay};
            audio_slot_emit(ctx, 4, 3, v, NULL, 0);
        }
        return JS_UNDEFINED;
    }
    if (n->started)
        return audio_slot_error(ctx, "Source may start only once", "InvalidStateError");
    AudioSlotBufferRef *b = NULL;
    if (n->kind == AUDIO_SLOT_NODE_BUFFER) {
        b = JS_GetOpaque(n->buffer, screen->buffer_class);
        if (!b || b->engine_epoch != screen->epoch)
            return audio_slot_error(ctx, "No decoded AudioBuffer is attached", "InvalidStateError");
    }
    double delay;
    if (!audio_slot_delay_at(ctx, r->owner, when, &delay))
        return JS_EXCEPTION;
    double left, right;
    audio_slot_source_mix(r->owner, n, NULL, 0, &left, &right);
    uint32_t voice;
    if (r->owner->source_count == 4)
        return audio_slot_error(ctx, "No game-audio voice is available", "QuotaExceededError");
    if (n->kind == AUDIO_SLOT_NODE_OSC) {
        double v[] = {5, n->type, audio_slot_effective(n), left, right, delay};
        voice = audio_slot_emit(ctx, 5, 6, v, NULL, 0);
    } else {
        double rate =
            n->values[AUDIO_SLOT_PARAM_RATE] * pow(2, n->values[AUDIO_SLOT_PARAM_DETUNE] / 1200);
        double v[] = {3,     b->info.handle, offset,        duration,    rate, left,
                      right, n->loop,        n->loop_start, n->loop_end, delay};
        voice = audio_slot_emit(ctx, 3, 11, v, NULL, 0);
    }
    if (!voice)
        return JS_HasException(ctx) ? JS_EXCEPTION
                                    : audio_slot_error(ctx, "No game-audio voice is available",
                                                       "QuotaExceededError");
    unsigned s = r->owner->source_count;
    r->owner->active[s] = JS_DupValue(ctx, self);
    r->owner->active_indices[s] = r->index;
    r->owner->source_count++;
    n->voice = voice;
    n->started = true;
    r->owner->revision++;
    return JS_UNDEFINED;
}

static AudioSlotNode *audio_slot_param_node(JSContext *ctx, JSValueConst self,
                                            AudioSlotParamRef **p, AudioSlotNodeRef **r)
{
    AudioSlotRuntime *screen = audio_slot_state_context(ctx);
    *p = JS_GetOpaque2(ctx, self, screen->slot_param_class);
    return *p ? audio_slot_node(ctx, (*p)->node, r) : NULL;
}

static double audio_slot_minimum(unsigned kind)
{
    return kind == AUDIO_SLOT_PARAM_GAIN        ? 0
           : kind == AUDIO_SLOT_PARAM_PAN       ? -1
           : kind == AUDIO_SLOT_PARAM_FREQUENCY ? 1
           : kind == AUDIO_SLOT_PARAM_DETUNE    ? -1200
                                                : .25;
}

static double audio_slot_maximum(unsigned kind)
{
    return kind == AUDIO_SLOT_PARAM_GAIN        ? 4
           : kind == AUDIO_SLOT_PARAM_PAN       ? 1
           : kind == AUDIO_SLOT_PARAM_FREQUENCY ? 20000
           : kind == AUDIO_SLOT_PARAM_DETUNE    ? 1200
                                                : 4;
}

static JSValue audio_slot_param_get(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv,
                                    int op, JSValue *data)
{
    (void) argc;
    (void) argv;
    (void) data;
    AudioSlotParamRef *p;
    AudioSlotNodeRef *r;
    AudioSlotNode *n = audio_slot_param_node(ctx, self, &p, &r);
    if (!n)
        return JS_EXCEPTION;
    double value = op == 0   ? n->values[p->kind]
                   : op == 1 ? audio_slot_minimum(p->kind)
                   : op == 2 ? audio_slot_maximum(p->kind)
                             : n->defaults[p->kind];
    return JS_NewFloat64(ctx, value);
}

static JSValue audio_slot_param_set(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    AudioSlotParamRef *p;
    AudioSlotNodeRef *r;
    AudioSlotNode *n = audio_slot_param_node(ctx, self, &p, &r);
    if (!n)
        return JS_EXCEPTION;
    double v = 0;
    if (!argc)
        return JS_ThrowTypeError(ctx, "Audio value required");
    if (JS_ToFloat64(ctx, &v, argv[0]) < 0 ||
        (!r->owner->closed && !audio_slot_live(ctx, r->owner)))
        return JS_EXCEPTION;
    if (isfinite(v))
        n->values[p->kind] =
            audio_slot_clamp(v, audio_slot_minimum(p->kind), audio_slot_maximum(p->kind));
    if (r->owner->closed)
        return JS_UNDEFINED;
    if (p->kind == AUDIO_SLOT_PARAM_GAIN || p->kind == AUDIO_SLOT_PARAM_PAN)
        audio_slot_refresh(ctx, r->owner, audio_slot_through(r->owner, n));
    else if (n->kind == AUDIO_SLOT_NODE_OSC && n->voice) {
        double values[] = {7, n->voice, audio_slot_effective(n)};
        audio_slot_emit(ctx, 7, 3, values, NULL, 0);
    }
    return JS_HasException(ctx) ? JS_EXCEPTION : JS_UNDEFINED;
}

static bool audio_slot_curve_values(JSContext *ctx, JSValueConst object, float *out, size_t *count)
{
    AudioSlotRuntime *screen = audio_slot_state_context(ctx);
    if (JS_IsObject(object) && JS_GetClassID(object) == screen->float_class) {
        size_t offset = 0, length = 0, element = 0, storage = 0;
        JSValue owner = JS_GetTypedArrayBuffer(ctx, object, &offset, &length, &element);
        if (JS_IsException(owner)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            audio_slot_error(ctx, "An audio curve needs at least two samples", "InvalidStateError");
            return false;
        }
        uint8_t *bytes = JS_GetArrayBuffer(ctx, &storage, owner);
        bool valid = bytes && element == 4 && offset <= storage && length <= storage - offset &&
                     length % 4 == 0;
        size_t n = valid ? length / 4 : 0;
        if (n >= 2 && n <= 64) {
            memcpy(out, bytes + offset, length);
            *count = n;
            JS_FreeValue(ctx, owner);
            return true;
        }
        JS_FreeValue(ctx, owner);
        audio_slot_error(ctx,
                         n > 64 ? "Audio curves are limited to 64 samples"
                                : "An audio curve needs at least two samples",
                         n > 64 ? "QuotaExceededError" : "InvalidStateError");
        return false;
    }
    JSValue method = JS_GetProperty(ctx, object, screen->iterator_atom);
    if (JS_IsException(method))
        return false;
    if (!JS_IsFunction(ctx, method)) {
        JS_FreeValue(ctx, method);
        JS_ThrowTypeError(ctx, "Audio curve needs Float32Array or bounded iterable");
        return false;
    }
    JSValue iterator = JS_Call(ctx, method, object, 0, NULL);
    JS_FreeValue(ctx, method);
    if (JS_IsException(iterator))
        return false;
    if (!JS_IsObject(iterator)) {
        JS_FreeValue(ctx, iterator);
        JS_ThrowTypeError(ctx, "Iterator must be an object");
        return false;
    }
    JSValue next = JS_GetPropertyStr(ctx, iterator, "next");
    if (JS_IsException(next)) {
        JS_FreeValue(ctx, iterator);
        return false;
    }
    bool done = false, close_iterator = false;
    size_t n = 0;
    for (unsigned visits = 0; visits < 65; visits++) {
        JSValue step = JS_Call(ctx, next, iterator, 0, NULL);
        if (JS_IsException(step))
            break;
        if (!JS_IsObject(step)) {
            JS_FreeValue(ctx, step);
            JS_ThrowTypeError(ctx, "Iterator result must be an object");
            break;
        }
        JSValue flag = JS_GetPropertyStr(ctx, step, "done");
        if (JS_IsException(flag)) {
            JS_FreeValue(ctx, step);
            break;
        }
        int end = JS_ToBool(ctx, flag);
        JS_FreeValue(ctx, flag);
        if (end) {
            JS_FreeValue(ctx, step);
            done = true;
            break;
        }
        JSValue value = JS_GetPropertyStr(ctx, step, "value");
        JS_FreeValue(ctx, step);
        double number = 0;
        if (JS_IsException(value)) {
            JS_FreeValue(ctx, value);
            break;
        }
        if (n == 64) {
            close_iterator = true;
            JS_FreeValue(ctx, value);
            audio_slot_error(ctx, "Audio curves are limited to 64 samples", "QuotaExceededError");
            break;
        }
        if (JS_ToFloat64(ctx, &number, value) < 0) {
            close_iterator = true;
            JS_FreeValue(ctx, value);
            break;
        }
        JS_FreeValue(ctx, value);
        out[n++] = (float) number;
    }
    JS_FreeValue(ctx, next);
    /* Iterator step/value abrupt completions propagate without IteratorClose. */
    if (close_iterator) {
        JSValue saved = JS_HasException(ctx) ? JS_GetException(ctx) : JS_UNDEFINED;
        JSValue close = JS_GetPropertyStr(ctx, iterator, "return");
        if (JS_IsFunction(ctx, close)) {
            JSValue result = JS_Call(ctx, close, iterator, 0, NULL);
            JS_FreeValue(ctx, result);
        }
        JS_FreeValue(ctx, close);
        if (JS_HasException(ctx))
            JS_FreeValue(ctx, JS_GetException(ctx));
        if (!JS_IsUndefined(saved))
            JS_Throw(ctx, saved);
        else
            audio_slot_error(ctx, "Audio curves are limited to 64 samples", "QuotaExceededError");
    }
    JS_FreeValue(ctx, iterator);
    if (!done)
        return false;
    if (n < 2) {
        audio_slot_error(ctx, "An audio curve needs at least two samples", "InvalidStateError");
        return false;
    }
    *count = n;
    return true;
}

static JSValue audio_slot_param_schedule(JSContext *ctx, JSValueConst self, int argc,
                                         JSValueConst *argv, int op, JSValue *data)
{
    (void) data;
    AudioSlotParamRef *p;
    AudioSlotNodeRef *r;
    AudioSlotNode *n = audio_slot_param_node(ctx, self, &p, &r);
    if (!n)
        return JS_EXCEPTION;
    double when = 0, duration = 0, value = 0;
    float points[64];
    size_t count = 0;
    if (op == 0) {
        if (argc && !JS_IsUndefined(argv[0]) && JS_ToFloat64(ctx, &when, argv[0]) < 0)
            return JS_EXCEPTION;
    } else if (op == 1) {
        if (argc < 3)
            return JS_ThrowTypeError(ctx, "Audio curve arguments required");
        if (JS_ToFloat64(ctx, &when, argv[1]) < 0 || JS_ToFloat64(ctx, &duration, argv[2]) < 0)
            return JS_EXCEPTION;
        if (!isfinite(when) || when < 0 || !isfinite(duration) || duration <= 0)
            return JS_ThrowRangeError(ctx, "Curve time and duration are out of range");
        if (p->kind != AUDIO_SLOT_PARAM_GAIN && p->kind != AUDIO_SLOT_PARAM_FREQUENCY)
            return audio_slot_error(ctx,
                                    "Curves are supported on gain and oscillator frequency only",
                                    "NotSupportedError");
        if (!audio_slot_curve_values(ctx, argv[0], points, &count))
            return JS_EXCEPTION;
    } else {
        if (argc < 3)
            return JS_ThrowTypeError(ctx, "Audio target arguments required");
        if (JS_ToFloat64(ctx, &value, argv[0]) < 0 || JS_ToFloat64(ctx, &duration, argv[2]) < 0)
            return JS_EXCEPTION;
        if (!isfinite(value) || !isfinite(duration) || duration <= 0)
            return JS_ThrowTypeError(ctx, "Audio target and time constant must be finite");
        if (p->kind != AUDIO_SLOT_PARAM_GAIN) {
            JSValue arg = JS_NewFloat64(ctx, value);
            JSValue result = audio_slot_param_set(ctx, self, 1, &arg);
            JS_FreeValue(ctx, arg);
            return JS_IsException(result) ? result : JS_DupValue(ctx, self);
        }
        if (JS_ToFloat64(ctx, &when, argv[1]) < 0)
            return JS_EXCEPTION;
        value = audio_slot_clamp(value, audio_slot_minimum(p->kind), audio_slot_maximum(p->kind));
    }
    if (!audio_slot_live(ctx, r->owner))
        return JS_EXCEPTION;
    if (op == 0 && p->kind != AUDIO_SLOT_PARAM_GAIN && p->kind != AUDIO_SLOT_PARAM_FREQUENCY)
        return JS_DupValue(ctx, self);
    double delay;
    if (!audio_slot_delay_at(ctx, r->owner, when, &delay))
        return JS_EXCEPTION;
    if (op == 0 && delay > .001)
        return audio_slot_error(ctx, "Future automation cancellation is unavailable",
                                "NotSupportedError");
    if (op == 1 && (delay + duration > 10 || (p->kind == AUDIO_SLOT_PARAM_FREQUENCY &&
                                              n->values[AUDIO_SLOT_PARAM_DETUNE] != 0)))
        return audio_slot_error(ctx,
                                p->kind == AUDIO_SLOT_PARAM_FREQUENCY
                                    ? "Pitch curve exceeds the bounded profile"
                                    : "Audio curve exceeds the ten-second window",
                                "NotSupportedError");
    if (p->kind == AUDIO_SLOT_PARAM_FREQUENCY) {
        if (op == 0) {
            if (n->voice) {
                double v[] = {10, n->voice};
                audio_slot_emit(ctx, 10, 2, v, NULL, 0);
            }
        } else if (op == 1) {
            double v[] = {11, n->voice, 1, 0, 1, 1, delay, duration};
            if (!n->voice || !audio_slot_emit(ctx, 11, 8, v, points, count))
                return JS_HasException(ctx)
                           ? JS_EXCEPTION
                           : audio_slot_error(ctx,
                                              "Pitch curve needs a live voice and empty timeline",
                                              "NotSupportedError");
        } else
            return audio_slot_error(ctx, "Only gain target envelopes are supported",
                                    "NotSupportedError");
        return JS_DupValue(ctx, self);
    }
    if (p->kind != AUDIO_SLOT_PARAM_GAIN)
        return audio_slot_error(ctx, "Only gain target envelopes are supported",
                                "NotSupportedError");
    unsigned mask = audio_slot_through(r->owner, n);
    if (op == 1 && !mask)
        return audio_slot_error(ctx, "Gain curves need a live connected source",
                                "NotSupportedError");
    for (unsigned s = 0; s < r->owner->source_count; s++)
        if (mask & (1u << s)) {
            AudioSlotNode *source = &r->owner->nodes[r->owner->active_indices[s]];
            if (op == 0) {
                double v[] = {9, source->voice};
                audio_slot_emit(ctx, 9, 2, v, NULL, 0);
                continue;
            }
            double left, right;
            audio_slot_source_mix(r->owner, source, n, op == 1 ? 1 : value, &left, &right);
            if (op == 1) {
                double v[] = {11, source->voice, 0, 0, left, right, delay, duration};
                if (!audio_slot_emit(ctx, 11, 8, v, points, count))
                    return JS_HasException(ctx)
                               ? JS_EXCEPTION
                               : audio_slot_error(ctx,
                                                  "Gain curve needs an empty automation timeline",
                                                  "NotSupportedError");
            } else {
                double v[] = {8, source->voice, left, right, delay, duration};
                if (!audio_slot_emit(ctx, 8, 6, v, NULL, 0))
                    return audio_slot_error(ctx, "Gain target exceeds the bounded timeline",
                                            "NotSupportedError");
            }
        }
    return JS_DupValue(ctx, self);
}

static JSValue audio_slot_param_at_time(JSContext *ctx, JSValueConst self, int argc,
                                        JSValueConst *argv)
{
    JSValue result = audio_slot_param_set(ctx, self, argc, argv);
    if (JS_IsException(result))
        return result;
    return JS_DupValue(ctx, self);
}

static JSValue audio_slot_make_buffer(JSContext *ctx, JSValueConst self, int argc,
                                      JSValueConst *argv)
{
    (void) self;
    AudioSlotRuntime *screen = audio_slot_state_context(ctx);
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "private buffer args");
    AudioSlotContext *o = audio_slot_context(ctx, argv[0]);
    if (!o)
        return JS_EXCEPTION;
    /* Closed decode retains the bounded reference shim's EncodingError. */
    if (screen->retired || o->closed || o->engine_epoch != screen->epoch)
        return audio_slot_error(ctx, "Invalid audio data", "EncodingError");
    size_t length = 0;
    uint8_t *bytes = JS_GetArrayBuffer(ctx, &length, argv[1]);
    if (!bytes || !length) {
        if (JS_HasException(ctx))
            JS_FreeValue(ctx, JS_GetException(ctx));
        return audio_slot_error(ctx, "Invalid audio data", "EncodingError");
    }
    AudioSlotBufferRef *b = audio_slot_allocate(screen, sizeof(*b));
    if (!b)
        return JS_ThrowOutOfMemory(ctx);
    JSValue object = JS_NewObjectProtoClass(ctx, o->buffer_proto, screen->buffer_class);
    if (JS_IsException(object)) {
        budget_free(screen->budget, b);
        return object;
    }
    if (!audio_slot_create_engine(screen) ||
        !tilefinch_game_audio_decode_wav(screen->host->game_audio, bytes, length, &b->info)) {
        JS_FreeValue(ctx, object);
        budget_free(screen->budget, b);
        return audio_slot_error(ctx, "Only bounded PCM WAV audio is supported", "EncodingError");
    }
    b->context = JS_DupValue(ctx, argv[0]);
    b->engine_epoch = screen->epoch;
    JS_SetOpaque(object, b);
    screen->buffer_records++;
    return object;
}

static JSValue audio_slot_buffer_get(JSContext *ctx, JSValueConst self, int argc,
                                     JSValueConst *argv, int op, JSValue *data)
{
    AudioSlotRuntime *screen = audio_slot_state_context(ctx);
    (void) argc;
    (void) argv;
    (void) data;
    AudioSlotBufferRef *b = JS_GetOpaque2(ctx, self, screen->buffer_class);
    if (!b)
        return JS_EXCEPTION;
    return op == 0   ? JS_NewUint32(ctx, b->info.frames)
           : op == 1 ? JS_NewUint32(ctx, b->info.sample_rate)
           : op == 2 ? JS_NewUint32(ctx, b->info.channels)
                     : JS_NewFloat64(ctx, (double) b->info.frames / b->info.sample_rate);
}

static JSValue audio_slot_buffer_method(JSContext *ctx, JSValueConst self, int argc,
                                        JSValueConst *argv, int op, JSValue *data)
{
    (void) argc;
    (void) argv;
    (void) op;
    (void) data;
    AudioSlotRuntime *s = audio_slot_state_context(ctx);
    if (!JS_GetOpaque2(ctx, self, s->buffer_class))
        return JS_EXCEPTION;
    return audio_slot_error(ctx, "Decoded PCM access is unavailable", "NotSupportedError");
}

static bool audio_slot_install_admitted(JSContext *ctx)
{
    (void) ctx;
#ifndef __PSP__
    unsigned *remaining = &tilefinch_test_faults()->audio_slot_install_fail_at;
    if (*remaining && --*remaining == 0) {
        JS_ThrowOutOfMemory(ctx);
        return false;
    }
#endif
    return true;
}

static bool audio_slot_method(JSContext *ctx, JSValueConst proto, const char *name, int length,
                              JSCFunctionData *fn, int magic)
{
    if (!audio_slot_install_admitted(ctx))
        return false;
    JSValue f = JS_NewCFunctionData(ctx, fn, length, magic, 0, NULL);
    if (JS_IsException(f))
        return false;
    JSValue label = JS_NewString(ctx, name);
    if (JS_IsException(label)) {
        JS_FreeValue(ctx, f);
        return false;
    }
    if (JS_DefinePropertyValueStr(ctx, f, "name", label, JS_PROP_CONFIGURABLE) < 0) {
        JS_FreeValue(ctx, f);
        return false;
    }
    return JS_DefinePropertyValueStr(ctx, proto, name, f,
                                     JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE) >= 0;
}

static bool audio_slot_accessor(JSContext *ctx, JSValueConst proto, const char *name,
                                JSCFunctionData *get, int magic, JSCFunctionData *set)
{
    if (!audio_slot_install_admitted(ctx))
        return false;
    JSAtom a = JS_NewAtom(ctx, name);
    if (a == JS_ATOM_NULL)
        return false;
    JSValue g = JS_NewCFunctionData(ctx, get, 0, magic, 0, NULL), s = JS_UNDEFINED;
    bool okay = !JS_IsException(g);
    if (okay && set) {
        s = JS_NewCFunctionData(ctx, set, 1, magic, 0, NULL);
        okay = !JS_IsException(s);
    }
    if (okay)
        okay = JS_DefineProperty(ctx, proto, a, JS_UNDEFINED, g, s,
                                 JS_PROP_HAS_GET | JS_PROP_HAS_SET | JS_PROP_CONFIGURABLE |
                                     JS_PROP_ENUMERABLE) >= 0;
    JS_FreeValue(ctx, g);
    JS_FreeValue(ctx, s);
    JS_FreeAtom(ctx, a);
    return okay;
}

static JSValue audio_slot_param_value_set(JSContext *ctx, JSValueConst self, int argc,
                                          JSValueConst *argv, int magic, JSValue *data)
{
    (void) magic;
    (void) data;
    return audio_slot_param_set(ctx, self, argc, argv);
}

static JSValue audio_slot_param_at_time_method(JSContext *ctx, JSValueConst self, int argc,
                                               JSValueConst *argv, int magic, JSValue *data)
{
    (void) magic;
    (void) data;
    return audio_slot_param_at_time(ctx, self, argc, argv);
}

static JSValue audio_slot_install_slots(JSContext *ctx, JSValueConst self, int argc,
                                        JSValueConst *argv)
{
    AudioSlotRuntime *screen = audio_slot_state_context(ctx);
    (void) self;
    if (argc != 3)
        return JS_ThrowTypeError(ctx, "private slots install");
    for (unsigned i = 0; i < 10; i++) {
        JS_FreeValue(ctx, screen->prototypes[i]);
        screen->prototypes[i] = JS_GetPropertyUint32(ctx, argv[0], i);
        if (JS_IsException(screen->prototypes[i]))
            return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, screen->notify_value);
    screen->notify_value = JS_DupValue(ctx, argv[1]);
    JS_FreeValue(ctx, screen->dom_value);
    screen->dom_value = JS_DupValue(ctx, argv[2]);
#define INSTALL(expression)                                                                        \
    do {                                                                                           \
        if (!(expression))                                                                         \
            return JS_EXCEPTION;                                                                   \
    } while (0)
    const char *ctxprops[] = {"state", "currentTime", "destination", "sampleRate"};
    for (int i = 0; i < 4; i++)
        INSTALL(audio_slot_accessor(ctx, screen->prototypes[0], ctxprops[i], audio_slot_context_get,
                                    i, NULL));
    INSTALL(
        audio_slot_method(ctx, screen->prototypes[0], "resume", 0, audio_slot_context_action, 0));
    INSTALL(
        audio_slot_method(ctx, screen->prototypes[0], "suspend", 0, audio_slot_context_action, 1));
    INSTALL(
        audio_slot_method(ctx, screen->prototypes[0], "close", 0, audio_slot_context_action, 2));
    INSTALL(
        audio_slot_method(ctx, screen->prototypes[2], "connect", 1, audio_slot_node_connect, 0));
    INSTALL(
        audio_slot_method(ctx, screen->prototypes[2], "disconnect", 0, audio_slot_node_connect, 1));
    INSTALL(
        audio_slot_accessor(ctx, screen->prototypes[2], "context", audio_slot_node_get, 0, NULL));
    INSTALL(audio_slot_accessor(ctx, screen->prototypes[3], "gain", audio_slot_node_get,
                                100 + 10 + AUDIO_SLOT_PARAM_GAIN, NULL));
    INSTALL(audio_slot_accessor(ctx, screen->prototypes[4], "pan", audio_slot_node_get,
                                200 + 10 + AUDIO_SLOT_PARAM_PAN, NULL));
    INSTALL(audio_slot_accessor(ctx, screen->prototypes[5], "frequency", audio_slot_node_get,
                                300 + 10 + AUDIO_SLOT_PARAM_FREQUENCY, NULL));
    INSTALL(audio_slot_accessor(ctx, screen->prototypes[5], "detune", audio_slot_node_get,
                                300 + 10 + AUDIO_SLOT_PARAM_DETUNE, NULL));
    INSTALL(audio_slot_accessor(ctx, screen->prototypes[5], "type", audio_slot_node_get, 305,
                                audio_slot_node_set));
    INSTALL(audio_slot_accessor(ctx, screen->prototypes[6], "playbackRate", audio_slot_node_get,
                                400 + 10 + AUDIO_SLOT_PARAM_RATE, NULL));
    INSTALL(audio_slot_accessor(ctx, screen->prototypes[6], "detune", audio_slot_node_get,
                                400 + 10 + AUDIO_SLOT_PARAM_DETUNE, NULL));
    const char *bufferprops[] = {"buffer", "loop", "loopStart", "loopEnd"};
    for (int i = 0; i < 4; i++)
        INSTALL(audio_slot_accessor(ctx, screen->prototypes[6], bufferprops[i], audio_slot_node_get,
                                    400 + i + 1, audio_slot_node_set));
    for (int i = 5; i <= 6; i++) {
        INSTALL(audio_slot_method(ctx, screen->prototypes[i], "start", 0, audio_slot_source_action,
                                  i == 6 ? 400 : 300));
        INSTALL(audio_slot_method(ctx, screen->prototypes[i], "stop", 0, audio_slot_source_action,
                                  i == 6 ? 401 : 301));
    }
    INSTALL(audio_slot_accessor(ctx, screen->prototypes[8], "value", audio_slot_param_get, 0,
                                audio_slot_param_value_set));
    INSTALL(
        audio_slot_accessor(ctx, screen->prototypes[8], "minValue", audio_slot_param_get, 1, NULL));
    INSTALL(
        audio_slot_accessor(ctx, screen->prototypes[8], "maxValue", audio_slot_param_get, 2, NULL));
    INSTALL(audio_slot_accessor(ctx, screen->prototypes[8], "defaultValue", audio_slot_param_get, 3,
                                NULL));
    INSTALL(audio_slot_method(ctx, screen->prototypes[8], "cancelScheduledValues", 0,
                              audio_slot_param_schedule, 0));
    INSTALL(audio_slot_method(ctx, screen->prototypes[8], "setValueCurveAtTime", 3,
                              audio_slot_param_schedule, 1));
    INSTALL(audio_slot_method(ctx, screen->prototypes[8], "setTargetAtTime", 3,
                              audio_slot_param_schedule, 2));
    INSTALL(audio_slot_method(ctx, screen->prototypes[8], "setValueAtTime", 2,
                              audio_slot_param_at_time_method, 0));
    INSTALL(audio_slot_method(ctx, screen->prototypes[8], "linearRampToValueAtTime", 2,
                              audio_slot_param_at_time_method, 0));
    INSTALL(audio_slot_method(ctx, screen->prototypes[8], "exponentialRampToValueAtTime", 2,
                              audio_slot_param_at_time_method, 0));
    const char *meta[] = {"length", "sampleRate", "numberOfChannels", "duration"};
    for (int i = 0; i < 4; i++)
        INSTALL(audio_slot_accessor(ctx, screen->prototypes[9], meta[i], audio_slot_buffer_get, i,
                                    NULL));
    INSTALL(audio_slot_method(ctx, screen->prototypes[9], "getChannelData", 1,
                              audio_slot_buffer_method, 0));
    INSTALL(audio_slot_method(ctx, screen->prototypes[9], "copyFromChannel", 2,
                              audio_slot_buffer_method, 1));
    INSTALL(audio_slot_method(ctx, screen->prototypes[9], "copyToChannel", 2,
                              audio_slot_buffer_method, 2));
#undef INSTALL
    return JS_HasException(ctx) ? JS_EXCEPTION : JS_UNDEFINED;
}

static JSValue audio_slot_complete(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    AudioSlotRuntime *screen = audio_slot_state_context(ctx);
    (void) self;
    uint32_t handle = 0;
    if (!argc || JS_ToUint32(ctx, &handle, argv[0]) < 0)
        return JS_EXCEPTION;
    AudioSlotContext *o = JS_GetOpaque(screen->active_context_root, screen->context_class);
    if (!o || o->closed || o->engine_epoch != screen->epoch)
        return JS_FALSE;
    for (unsigned i = 0; i < o->source_count; i++) {
        if (o->nodes[o->active_indices[i]].voice != handle)
            continue;
        JSValue source = o->active[i];
        o->nodes[o->active_indices[i]].voice = 0;
        for (unsigned j = i + 1; j < o->source_count; j++) {
            o->active[j - 1] = o->active[j];
            o->active_indices[j - 1] = o->active_indices[j];
        }
        o->active[--o->source_count] = JS_UNDEFINED;
        o->revision++;
        JSValue result = audio_slot_notify(ctx, o, source, "ended");
        JS_FreeValue(ctx, source);
        if (JS_IsException(result))
            return result;
        JS_FreeValue(ctx, result);
        return JS_TRUE;
    }
    return JS_FALSE;
}

static JSValue audio_slot_host_suspend(JSContext *ctx, JSValueConst self, int argc,
                                       JSValueConst *argv)
{
    AudioSlotRuntime *screen = audio_slot_state_context(ctx);
    (void) self;
    (void) argc;
    (void) argv;
    AudioSlotContext *o = JS_GetOpaque(screen->active_context_root, screen->context_class);
    if (!o || o->closed || o->engine_epoch != screen->epoch)
        return JS_UNDEFINED;
    if (o->running) {
        double elapsed = audio_slot_clock_time(ctx, o);
        if (JS_HasException(ctx))
            return JS_EXCEPTION;
        o->elapsed = elapsed;
        o->running = false;
    }
    return JS_UNDEFINED;
}

static JSValue audio_slot_host_complete(JSContext *ctx, JSValueConst self, int argc,
                                        JSValueConst *argv)
{
    AudioSlotRuntime *screen = audio_slot_state_context(ctx);
    uint32_t generation = 0;
    if (argc < 2 || !JS_IsNumber(argv[0]) || !JS_IsNumber(argv[1]) ||
        JS_ToUint32(ctx, &generation, argv[1]) < 0)
        return JS_ThrowTypeError(ctx, "private completion packet");
    if (generation != screen->epoch)
        return JS_FALSE;
    return audio_slot_complete(ctx, self, 1, argv);
}

static JSValue audio_slot_publish_slots(JSContext *ctx, JSValueConst self, int argc,
                                        JSValueConst *argv)
{
    (void) self;
    if (argc != 1)
        return JS_ThrowTypeError(ctx, "private audio publication packet");
    const char *names[] = {"AudioParam",           "AudioNode",         "GainNode",
                           "StereoPannerNode",     "OscillatorNode",    "AudioBufferSourceNode",
                           "AudioDestinationNode", "AudioBuffer",       "BaseAudioContext",
                           "AudioContext",         "webkitAudioContext"};
    JSAtom atoms[11] = {0};
    JSPropertyDescriptor old[11];
    JSValue values[11];
    int found[11] = {0};
    for (unsigned i = 0; i < 11; i++) {
        old[i] = (JSPropertyDescriptor) {
            .value = JS_UNDEFINED, .getter = JS_UNDEFINED, .setter = JS_UNDEFINED};
        values[i] = JS_UNDEFINED;
    }
    JSValue global = JS_GetGlobalObject(ctx);
    bool okay = !JS_IsException(global);
    unsigned published = 0;
    /* Capture and admit every descriptor before exposing the first constructor.
       The fresh array and this function remain inside the bootstrap closure. */
    for (unsigned i = 0; okay && i < 11; i++) {
        atoms[i] = JS_NewAtom(ctx, names[i]);
        if (atoms[i] == JS_ATOM_NULL) {
            okay = false;
            break;
        }
        found[i] = JS_GetOwnProperty(ctx, &old[i], global, atoms[i]);
        if (found[i] < 0) {
            okay = false;
            break;
        }
        if (found[i] && !(old[i].flags & JS_PROP_CONFIGURABLE)) {
            JS_ThrowTypeError(ctx, "Audio constructor property is not configurable");
            okay = false;
            break;
        }
        values[i] = JS_GetPropertyUint32(ctx, argv[0], i);
        okay = JS_IsFunction(ctx, values[i]) && !JS_HasException(ctx);
    }
    for (unsigned i = 0; okay && i < 11; i++) {
#ifndef __PSP__
        unsigned *remaining = &tilefinch_test_faults()->audio_slot_publish_fail_at;
        if (*remaining && --*remaining == 0) {
            JS_ThrowOutOfMemory(ctx);
            okay = false;
            break;
        }
#endif
        okay = JS_DefinePropertyValue(ctx, global, atoms[i], JS_DupValue(ctx, values[i]),
                                      JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE |
                                          JS_PROP_ENUMERABLE) >= 0;
        if (okay)
            published++;
    }
    if (!okay) {
        JSValue failure = JS_HasException(ctx) ? JS_GetException(ctx) : JS_UNDEFINED;
        for (unsigned i = 0; i < published; i++) {
            unsigned flags =
                (old[i].flags & (JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE | JS_PROP_WRITABLE)) |
                JS_PROP_HAS_CONFIGURABLE | JS_PROP_HAS_ENUMERABLE;
            flags |= (old[i].flags & JS_PROP_GETSET) ? JS_PROP_HAS_GET | JS_PROP_HAS_SET
                                                     : JS_PROP_HAS_VALUE | JS_PROP_HAS_WRITABLE;
            int status = found[i] ? JS_DefineProperty(ctx, global, atoms[i], old[i].value,
                                                      old[i].getter, old[i].setter, flags)
                                  : JS_DeleteProperty(ctx, global, atoms[i], 0);
            if (status < 0 && JS_HasException(ctx))
                JS_FreeValue(ctx, JS_GetException(ctx));
        }
        if (!JS_IsUndefined(failure))
            JS_Throw(ctx, failure);
        else
            JS_ThrowTypeError(ctx, "Audio constructor publication failed");
    }
    for (unsigned i = 0; i < 11; i++) {
        JS_FreeAtom(ctx, atoms[i]);
        JS_FreeValue(ctx, values[i]);
        JS_FreeValue(ctx, old[i].value);
        JS_FreeValue(ctx, old[i].getter);
        JS_FreeValue(ctx, old[i].setter);
    }
    JS_FreeValue(ctx, global);
    return okay ? JS_UNDEFINED : JS_EXCEPTION;
}

static void audio_slots_release_values(ScriptRuntime *host)
{
    AudioSlotRuntime *s = host->game_audio_slots;
    if (!s || !host->context)
        return;
    for (unsigned i = 0; i < 10; i++) {
        JS_FreeValue(host->context, s->prototypes[i]);
        s->prototypes[i] = JS_UNDEFINED;
    }
    JSValue *values[] = {&s->notify_value, &s->dom_value, &s->promise_value, &s->resolve_value,
                         &s->reject_value};
    for (unsigned i = 0; i < 5; i++) {
        JS_FreeValue(host->context, *values[i]);
        *values[i] = JS_UNDEFINED;
    }
    JS_FreeAtom(host->context, s->iterator_atom);
    s->iterator_atom = JS_ATOM_NULL;
}

static void audio_slots_retire(ScriptRuntime *host)
{
    AudioSlotRuntime *s = host->game_audio_slots;
    if (!s || s->retired)
        return;
    s->retired = true;
    s->epoch++;
    AudioSlotContext *o = JS_GetOpaque(s->active_context_root, s->context_class);
    if (o) {
        o->closed = true;
        o->running = false;
        audio_slot_drop_active(host->context, o);
    }
    JSValue root = s->active_context_root;
    s->active_context_root = JS_UNDEFINED;
    if (host->context)
        JS_FreeValue(host->context, root);
}

static bool audio_slots_initialize(ScriptRuntime *host)
{
    JSContext *ctx = host->context;
    if (host->game_audio_slots)
        return false;
    AudioSlotRuntime *s =
        budget_calloc_category(host->budget, BUDGET_CATEGORY_JAVASCRIPT, 1, sizeof(*s));
    if (!s) {
        JS_ThrowOutOfMemory(ctx);
        return false;
    }
    s->host = host;
    s->budget = host->budget;
    s->active_context_root = s->notify_value = s->dom_value = s->promise_value = s->resolve_value =
        s->reject_value = JS_UNDEFINED;
    for (unsigned i = 0; i < 10; i++)
        s->prototypes[i] = JS_UNDEFINED;
    host->game_audio_slots = s;
    JS_SetRuntimeOpaque(host->runtime, host);
    if (audio_slot_class_ids[0] == 0)
        for (unsigned i = 0; i < 4; i++)
            JS_NewClassID(&audio_slot_class_ids[i]);
    s->context_class = audio_slot_class_ids[0];
    s->node_class = audio_slot_class_ids[1];
    s->slot_param_class = audio_slot_class_ids[2];
    s->buffer_class = audio_slot_class_ids[3];
    JSClassDef defs[] = {{.class_name = "AudioContext",
                          .finalizer = audio_slot_context_finalizer,
                          .gc_mark = audio_slot_context_mark},
                         {.class_name = "AudioNode",
                          .finalizer = audio_slot_node_finalizer,
                          .gc_mark = audio_slot_node_mark},
                         {.class_name = "AudioParam",
                          .finalizer = audio_slot_param_finalizer,
                          .gc_mark = audio_slot_param_mark},
                         {.class_name = "AudioBuffer",
                          .finalizer = audio_slot_buffer_finalizer,
                          .gc_mark = audio_slot_buffer_mark}};
    JSClassID ids[] = {s->context_class, s->node_class, s->slot_param_class, s->buffer_class};
    bool okay = true;
    for (unsigned i = 0; i < 4; i++)
        if (JS_NewClass(host->runtime, ids[i], &defs[i]) < 0) {
            okay = false;
            break;
        }
    JSValue global = JS_GetGlobalObject(ctx);
    if (okay) {
        s->promise_value = JS_GetPropertyStr(ctx, global, "Promise");
        s->resolve_value = JS_GetPropertyStr(ctx, s->promise_value, "resolve");
        s->reject_value = JS_GetPropertyStr(ctx, s->promise_value, "reject");
        JSValue symbol = JS_GetPropertyStr(ctx, global, "Symbol"),
                iterator = JS_GetPropertyStr(ctx, symbol, "iterator");
        if (!JS_IsException(iterator))
            s->iterator_atom = JS_ValueToAtom(ctx, iterator);
        JS_FreeValue(ctx, iterator);
        JS_FreeValue(ctx, symbol);
        JSValue argument = JS_NewInt32(ctx, 0),
                sample = JS_NewTypedArray(ctx, 1, &argument, JS_TYPED_ARRAY_FLOAT32);
        if (!JS_IsException(sample))
            s->float_class = JS_GetClassID(sample);
        JS_FreeValue(ctx, sample);
        okay = !JS_HasException(ctx) && s->iterator_atom != JS_ATOM_NULL;
    }
    JS_FreeValue(ctx, global);
    JSValue cap = JS_NewObjectProto(ctx, JS_NULL), factory = JS_UNDEFINED, returned = JS_UNDEFINED;

    struct {
        const char *name;
        JSCFunction *fn;
        int length;
    } methods[] = {
        {"context", audio_slot_create_context, 2},   {"node", audio_slot_create_node, 4},
        {"buffer", audio_slot_make_buffer, 2},       {"install", audio_slot_install_slots, 3},
        {"hostSuspend", audio_slot_host_suspend, 0}, {"hostComplete", audio_slot_host_complete, 2},
        {"publish", audio_slot_publish_slots, 1}};

    if (JS_IsException(cap))
        okay = false;
    for (unsigned i = 0; okay && i < 7; i++) {
        JSValue function = JS_NewCFunction(ctx, methods[i].fn, methods[i].name, methods[i].length);
        if (JS_IsException(function)) {
            okay = false;
            break;
        }
        if (i == 4)
            host->game_audio_host_suspend = JS_DupValue(ctx, function);
        if (i == 5)
            host->game_audio_host_complete = JS_DupValue(ctx, function);
        okay = JS_DefinePropertyValueStr(ctx, cap, methods[i].name, function, 0) >= 0;
    }
    if (okay) {
        JSValue code = JS_ReadObject(ctx, browser_bootstrap_game_audio_slots_bytecode.data,
                                     browser_bootstrap_game_audio_slots_bytecode.bytecode_length,
                                     JS_READ_OBJ_BYTECODE | JS_READ_OBJ_ROM_DATA);
        factory = JS_IsException(code) ? code : JS_EvalFunction(ctx, code);
        okay = !JS_IsException(factory) && JS_IsFunction(ctx, factory);
    }
    if (okay) {
        returned = JS_Call(ctx, factory, JS_UNDEFINED, 1, &cap);
        okay = !JS_IsException(returned);
    }
    JS_FreeValue(ctx, returned);
    JS_FreeValue(ctx, factory);
    JS_FreeValue(ctx, cap);
    if (!okay) {
        JS_FreeValue(ctx, host->game_audio_host_suspend);
        JS_FreeValue(ctx, host->game_audio_host_complete);
        host->game_audio_host_suspend = host->game_audio_host_complete = JS_UNDEFINED;
        audio_slots_retire(host);
        audio_slots_release_values(host);
    }
    return okay;
}
#ifndef __PSP__
bool js_rt_audio_slot_test_snapshot(ScriptRuntime *host, unsigned counts[5])
{
    AudioSlotRuntime *s = host ? host->game_audio_slots : NULL;
    if (!s || !counts)
        return false;
    counts[0] = s->context_records;
    counts[1] = s->node_records;
    counts[2] = s->param_records;
    counts[3] = s->buffer_records;
    AudioSlotContext *o = JS_GetOpaque(s->active_context_root, s->context_class);
    counts[4] = o ? o->source_count : 0;
    return true;
}
#endif
