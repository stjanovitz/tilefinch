/* Bounded native AudioNode/AudioParam state for browser audio scheduling.
   The JavaScript reference remains selectable by trusted runtime options. */
typedef struct AudioSlotRuntime AudioSlotRuntime;

enum {
    AUDIO_SLOT_NODE_DEST,
    AUDIO_SLOT_NODE_GAIN,
    AUDIO_SLOT_NODE_PAN,
    AUDIO_SLOT_NODE_OSC,
    AUDIO_SLOT_NODE_BUFFER,
    AUDIO_SLOT_NODE_FILTER,
    AUDIO_SLOT_NODE_STREAM,
    AUDIO_SLOT_NODE_ANALYSER
};

enum {
    AUDIO_SLOT_PARAM_GAIN,
    AUDIO_SLOT_PARAM_PAN,
    AUDIO_SLOT_PARAM_FREQUENCY,
    AUDIO_SLOT_PARAM_DETUNE,
    AUDIO_SLOT_PARAM_RATE,
    AUDIO_SLOT_PARAM_FILTER_FREQUENCY,
    AUDIO_SLOT_PARAM_Q,
    AUDIO_SLOT_PARAM_FILTER_GAIN,
    AUDIO_SLOT_PARAMS
};
/* JS prototypes, in cap.install order. */
#define AUDIO_SLOT_PROTOTYPES 13u
typedef struct AudioSlotContext AudioSlotContext;

typedef struct {
    bool used, started, loop, buffer_set;
    unsigned generation, kind, type, lane_epoch;
    uint32_t voice, cache_revision;
    uint8_t source_mask;
    double values[AUDIO_SLOT_PARAMS], defaults[AUDIO_SLOT_PARAMS], loop_start, loop_end;
    /* Engine automation lanes, created on a parameter's first automation
       and valid only for lane_epoch's engine. */
    uint32_t lanes[AUDIO_SLOT_PARAMS];
    /* A parameter whose settled lane was returned keeps the time of the
       setValueAtTime it settled on (bit per kind), so automation that
       resumes on a new lane ramps from there, as it would have. */
    double anchor_time[AUDIO_SLOT_PARAMS];
    uint8_t anchored;
    /* AnalyserNode: its engine history (lane_epoch's engine), settings,
       and the smoothed spectrum of the history at spectrum_stamp. */
    uint32_t analyser, spectrum_stamp;
    unsigned fft_size;
    bool spectrum_valid;
    double min_db, max_db, smoothing;
    float *spectrum;
    /* MediaStreamAudioSourceNode: the serial of the track it plays
       (js_rt_media_capture_track_state), and whether its voice's program
       is the silence of a disabled track. */
    uint32_t capture_track;
    bool capture_silent;
    JSValue target, buffer, params[AUDIO_SLOT_PARAMS];
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
    JSValue context, channels[2]; /* getChannelData views, made once */
    TilefinchGameAudioBufferInfo info;
    unsigned engine_epoch;
} AudioSlotBufferRef;

struct AudioSlotRuntime {
    ScriptRuntime *host;
    Budget *budget;
    JSClassID context_class, node_class, slot_param_class, buffer_class, float_class, byte_class;
    /* Browser-thread scratch for analysis and the microphone pump:
       FFT_SIZE_MAX time samples, then 2 * FFT_SIZE_MAX FFT values. */
    float *fft_scratch;
    uint32_t stream_cursor;
    unsigned epoch, context_records, node_records, param_records, buffer_records;
    bool retired;
    JSValue prototypes[AUDIO_SLOT_PROTOTYPES], notify_value, dom_value, promise_value, resolve_value, reject_value,
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
    for (unsigned i = 0; i < AUDIO_SLOT_PARAMS; i++)
        JS_MarkValue(rt, n->params[i], mark);
}

/* Lanes go with their node. Only the engine that made them is touched, and
   only on the browser thread (finalizers run there); a closed or retired
   context's engine is already gone. */
static void audio_slot_release_lanes(AudioSlotRuntime *screen, AudioSlotNode *n)
{
    TilefinchGameAudio *audio = screen->host->game_audio;
    bool live = audio != NULL && !screen->retired && n->lane_epoch == screen->epoch;
    for (unsigned i = 0; i < AUDIO_SLOT_PARAMS; i++) {
        if (n->lanes[i] != 0u && live)
            tilefinch_game_audio_lane_release(audio, n->lanes[i]);
        n->lanes[i] = 0u;
    }
    if (n->analyser != 0u && live)
        tilefinch_game_audio_analyser_release(audio, n->analyser);
    n->analyser = 0u;
    budget_free(screen->budget, n->spectrum);
    n->spectrum = NULL;
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
        JSValue target = n->target, buffer = n->buffer, params[AUDIO_SLOT_PARAMS];
        audio_slot_release_lanes(screen, n);
        for (unsigned i = 0; i < AUDIO_SLOT_PARAMS; i++) {
            params[i] = n->params[i];
            n->params[i] = JS_UNDEFINED;
        }
        n->target = n->buffer = JS_UNDEFINED;
        n->used = false;
        owner->node_count--;
        owner->revision++;
        JS_FreeValueRT(rt, target);
        JS_FreeValueRT(rt, buffer);
        for (unsigned i = 0; i < AUDIO_SLOT_PARAMS; i++)
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
    if (r) {
        JS_MarkValue(rt, r->context, mark);
        JS_MarkValue(rt, r->channels[0], mark);
        JS_MarkValue(rt, r->channels[1], mark);
    }
}

static void audio_slot_buffer_finalizer(JSRuntime *rt, JSValue object)
{
    AudioSlotRuntime *screen = audio_slot_state_runtime(rt);
    AudioSlotBufferRef *r = JS_GetOpaque(object, screen->buffer_class);
    if (r) {
        /* The engine slot returns once no voice plays it; views keep their
           own reference to the samples. */
        TilefinchGameAudio *audio = screen->host->game_audio;
        if (audio != NULL && !screen->retired && r->engine_epoch == screen->epoch)
            tilefinch_game_audio_buffer_release(audio, r->info.handle);
        JS_FreeValueRT(rt, r->channels[0]);
        JS_FreeValueRT(rt, r->channels[1]);
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

/* Context lifecycle (0 resume, 1 suspend, 2 close) and stop (4). Voices,
   parameters and automation go through programs and lanes below. */
static uint32_t audio_slot_emit(JSContext *ctx, int op, unsigned argc, const double *v)
{
    AudioSlotRuntime *screen = audio_slot_state_context(ctx);
    uint32_t h = argc > 1 ? (uint32_t) v[1] : 0;
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
    case 4:
        tilefinch_game_audio_stop(screen->host->game_audio, h, v[2]);
        return 1;
    }
    return 0;
}

static double audio_slot_clamp(double v, double low, double high)
{
    return v < low ? low : v > high ? high : v;
}

static uint32_t audio_slot_lane_of(AudioSlotRuntime *screen, const AudioSlotNode *n,
                                   unsigned kind)
{
    return n->lane_epoch == screen->epoch ? n->lanes[kind] : 0u;
}

/* A source's program: its own pitch parameters, then the chain to the
   destination. Unautomated gains and the panner fold into static gains;
   automated gains name their lanes (at most four); the first
   BiquadFilterNode on the chain filters the source, and the first
   AnalyserNode hears it there (before any panner). A chain that does not
   reach the destination within eight nodes is silent. */
static bool audio_slot_program(JSContext *ctx, AudioSlotContext *o, AudioSlotNode *source,
                               TilefinchGameAudioProgram *p)
{
    AudioSlotRuntime *screen = o->screen;
    memset(p, 0, sizeof(*p));
    unsigned pitch = source->kind == AUDIO_SLOT_NODE_OSC ? AUDIO_SLOT_PARAM_FREQUENCY
                                                         : AUDIO_SLOT_PARAM_RATE;
    p->pitch = source->values[pitch];
    p->pitch_lane = audio_slot_lane_of(screen, source, pitch);
    p->detune = source->values[AUDIO_SLOT_PARAM_DETUNE];
    p->detune_lane = audio_slot_lane_of(screen, source, AUDIO_SLOT_PARAM_DETUNE);
    double gain = 1, pan = 0;
    bool panner = false, reached = false;
    unsigned lanes = 0;
    JSValue target = source->target;
    for (unsigned visits = 0; !JS_IsUndefined(target) && visits < 8; visits++) {
        AudioSlotNodeRef *ref = JS_GetOpaque(target, screen->node_class);
        if (!ref)
            break;
        AudioSlotNode *n = &o->nodes[ref->index];
        if (!n->used || n->generation != ref->generation)
            break;
        if (n->kind == AUDIO_SLOT_NODE_GAIN) {
            uint32_t lane = audio_slot_lane_of(screen, n, AUDIO_SLOT_PARAM_GAIN);
            if (lane == 0u)
                gain *= n->values[AUDIO_SLOT_PARAM_GAIN];
            else if (lanes == TILEFINCH_GAME_AUDIO_PROGRAM_GAIN_LANES) {
                audio_slot_error(ctx, "A source passes through at most four automated gains",
                                 "NotSupportedError");
                return false;
            } else {
                if (p->filter == TILEFINCH_GAME_AUDIO_FILTER_NONE)
                    p->pre_filter |= (uint8_t) (1u << lanes);
                p->gain_lanes[lanes++] = lane;
            }
        }
        if (n->kind == AUDIO_SLOT_NODE_PAN) {
            panner = true;
            pan = audio_slot_clamp(pan + n->values[AUDIO_SLOT_PARAM_PAN], -1, 1);
        }
        if (n->kind == AUDIO_SLOT_NODE_ANALYSER && p->analyser == 0u) {
            p->analyser = n->lane_epoch == screen->epoch ? n->analyser : 0u;
            p->tap_gain = audio_slot_clamp(gain, 0, 4);
            p->tap_lanes = (uint8_t) ((1u << lanes) - 1u);
            p->tap_after_filter = p->filter != TILEFINCH_GAME_AUDIO_FILTER_NONE;
        }
        if (n->kind == AUDIO_SLOT_NODE_DEST) {
            reached = true;
            break;
        }
        if (n->kind == AUDIO_SLOT_NODE_FILTER && p->filter == TILEFINCH_GAME_AUDIO_FILTER_NONE) {
            static const unsigned kinds[4] = {AUDIO_SLOT_PARAM_FILTER_FREQUENCY,
                AUDIO_SLOT_PARAM_DETUNE, AUDIO_SLOT_PARAM_Q, AUDIO_SLOT_PARAM_FILTER_GAIN};
            p->filter = (uint8_t) n->type;
            for (unsigned i = 0; i < 4; i++) {
                p->filter_values[i] = n->values[kinds[i]];
                p->filter_lanes[i] = audio_slot_lane_of(screen, n, kinds[i]);
            }
        }
        target = n->target;
    }
    gain = audio_slot_clamp(gain, 0, 4);
    p->muted = !reached;
    if (!panner) {
        p->gain_left = p->gain_right = gain;
    } else {
        double angle = (pan + 1) * 3.14159265358979323846 / 4;
        p->gain_left = audio_slot_clamp(gain * cos(angle), 0, 4);
        p->gain_right = audio_slot_clamp(gain * sin(angle), 0, 4);
    }
    /* A disabled track is silence at the source: the filter, analysers
       and destination all hear zeros (a filter rings out from what it
       heard before, as on a quiet input). */
    p->source_silent = source->kind == AUDIO_SLOT_NODE_STREAM && source->capture_silent;
    return true;
}

/* Active sources whose chain passes through node (bit per active index). */
static unsigned audio_slot_through(AudioSlotContext *o, AudioSlotNode *node)
{
    AudioSlotRuntime *screen = o->screen;
    if (node->cache_revision == o->revision)
        return node->source_mask;
    unsigned mask = 0;
    for (unsigned s = 0; s < o->source_count; s++) {
        AudioSlotNode *source = &o->nodes[o->active_indices[s]];
        if (source == node) {
            mask |= 1u << s;
            continue;
        }
        JSValue target = source->target;
        for (unsigned visits = 0; !JS_IsUndefined(target) && visits < 8; visits++) {
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

/* Republishes the programs of the playing sources in mask, all or none.
   A graph change one of them cannot draw (a fifth automated gain on its
   path) throws and publishes nothing; the caller undoes the change, so the
   graph stays one every playing source can draw and later calls work. */
static bool audio_slot_refresh(JSContext *ctx, AudioSlotContext *o, unsigned mask)
{
    AudioSlotRuntime *screen = o->screen;
    enum { ACTIVE = sizeof(o->active_indices) / sizeof(o->active_indices[0]) };
    TilefinchGameAudioProgram programs[ACTIVE];
    uint32_t voices[ACTIVE];
    unsigned count = 0;
    for (unsigned i = 0; i < o->source_count && i < ACTIVE; i++)
        if (mask & (1u << i)) {
            AudioSlotNode *source = &o->nodes[o->active_indices[i]];
            if (!source->voice)
                continue;
            if (!audio_slot_program(ctx, o, source, &programs[count]))
                return false;
            voices[count++] = source->voice;
        }
    for (unsigned i = 0; i < count; i++)
        (void) tilefinch_game_audio_set_program(screen->host->game_audio, voices[i],
                                                &programs[i]);
    return true;
}

/* Returns lanes that can no longer change what is heard to the engine's
   16 without a garbage collection: each automated parameter of the
   context whose timeline has settled (tilefinch_game_audio_lane_settled)
   takes its value as a static one, keeping the event time as its anchor.
   The usual effect (an oscillator and a gain per sound, a ramp on the
   gain) otherwise holds two lanes per sound until the cycle collector
   frees the nodes. Returns how many lanes were freed. */
static unsigned audio_slot_reclaim_lanes(JSContext *ctx, AudioSlotContext *o, double now)
{
    AudioSlotRuntime *screen = o->screen;
    TilefinchGameAudio *audio = screen->host->game_audio;
    if (audio == NULL || screen->retired || o->closed || !isfinite(now))
        return 0;
    struct {
        AudioSlotNode *node;
        unsigned kind;
        uint32_t lane;
        double value, anchor_time;
        uint8_t anchored;
    } freed[TILEFINCH_GAME_AUDIO_LANE_LIMIT];
    unsigned count = 0;
    for (unsigned i = 0; i < 16; i++) {
        AudioSlotNode *n = &o->nodes[i];
        if (!n->used || n->lane_epoch != screen->epoch)
            continue;
        for (unsigned kind = 0; kind < AUDIO_SLOT_PARAMS; kind++) {
            double value = 0, anchor = 0;
            bool anchored = false;
            if (n->lanes[kind] == 0u || count == TILEFINCH_GAME_AUDIO_LANE_LIMIT ||
                !tilefinch_game_audio_lane_settled(audio, n->lanes[kind], now, &value, &anchor,
                                                   &anchored))
                continue;
            freed[count].node = n;
            freed[count].kind = kind;
            freed[count].lane = n->lanes[kind];
            freed[count].value = n->values[kind];
            freed[count].anchor_time = n->anchor_time[kind];
            freed[count++].anchored = n->anchored;
            n->lanes[kind] = 0u;
            n->values[kind] = value;
            n->anchor_time[kind] = anchor;
            if (anchored)
                n->anchored |= (uint8_t) (1u << kind);
            else
                n->anchored &= (uint8_t) ~(1u << kind);
        }
    }
    if (count == 0u)
        return 0;
    /* The playing programs rebuild on the static values before the lanes
       go. Dropping lanes never lengthens a path, so this cannot refuse;
       if it did, everything is put back. */
    if (!audio_slot_refresh(ctx, o, (1u << o->source_count) - 1)) {
        if (JS_HasException(ctx))
            JS_FreeValue(ctx, JS_GetException(ctx));
        while (count-- > 0) {
            AudioSlotNode *n = freed[count].node;
            n->lanes[freed[count].kind] = freed[count].lane;
            n->values[freed[count].kind] = freed[count].value;
            n->anchor_time[freed[count].kind] = freed[count].anchor_time;
            n->anchored = freed[count].anchored;
        }
        return 0;
    }
    for (unsigned i = 0; i < count; i++)
        tilefinch_game_audio_lane_release(audio, freed[i].lane);
    return count;
}

/* A node whose AudioParam was read and its param hold each other, so only
   the cycle collector frees it: a full table runs one collection before it
   refuses, as the lane and analyser pools do. */
static bool audio_slot_node_room(AudioSlotRuntime *screen, AudioSlotContext *o)
{
    if (o->node_count < 16)
        return true;
    JS_RunGC(screen->host->runtime);
    return o->node_count < 16;
}

static JSValue audio_slot_make_node(JSContext *ctx, JSValueConst context, JSValueConst proto,
                                    unsigned kind)
{
    AudioSlotRuntime *screen = audio_slot_state_context(ctx);
    AudioSlotContext *o = audio_slot_context(ctx, context);
    if (!o || !audio_slot_live(ctx, o))
        return JS_EXCEPTION;
    if (!audio_slot_node_room(screen, o))
        return audio_slot_error(ctx, "Audio node limit reached", "QuotaExceededError");
    unsigned index = 0;
    while (index < 16 && o->nodes[index].used)
        index++;
    AudioSlotNodeRef *r = audio_slot_allocate(screen, sizeof(*r));
    if (!r)
        return JS_ThrowOutOfMemory(ctx);
    JSValueConst effective_proto =
        JS_IsObject(proto) ? proto
        : screen->prototypes[kind == AUDIO_SLOT_NODE_DEST       ? 7
                             : kind == AUDIO_SLOT_NODE_FILTER   ? 10
                             : kind == AUDIO_SLOT_NODE_STREAM   ? 11
                             : kind == AUDIO_SLOT_NODE_ANALYSER ? 12
                                                                : kind + 2];
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
    n->lane_epoch = screen->epoch;
    for (unsigned i = 0; i < AUDIO_SLOT_PARAMS; i++)
        n->params[i] = JS_UNDEFINED;
    n->values[AUDIO_SLOT_PARAM_GAIN] = n->defaults[AUDIO_SLOT_PARAM_GAIN] = 1;
    n->values[AUDIO_SLOT_PARAM_FREQUENCY] = n->defaults[AUDIO_SLOT_PARAM_FREQUENCY] = 440;
    n->values[AUDIO_SLOT_PARAM_RATE] = n->defaults[AUDIO_SLOT_PARAM_RATE] = 1;
    n->values[AUDIO_SLOT_PARAM_FILTER_FREQUENCY] =
        n->defaults[AUDIO_SLOT_PARAM_FILTER_FREQUENCY] = 350;
    n->values[AUDIO_SLOT_PARAM_Q] = n->defaults[AUDIO_SLOT_PARAM_Q] = 1;
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

static const char *const audio_slot_filter_types[] = {
    "lowpass", "highpass", "bandpass", "lowshelf", "highshelf", "peaking", "notch", "allpass"};

/* TilefinchGameAudioFilterType, or 0 for an unknown name (enum values are
   case sensitive). */
static unsigned audio_slot_filter_type(JSContext *ctx, JSValueConst value)
{
    const char *text = JS_ToCString(ctx, value);
    if (!text)
        return 0;
    unsigned type = 0;
    for (unsigned i = 0; i < 8; i++)
        if (!strcmp(text, audio_slot_filter_types[i]))
            type = i + 1;
    JS_FreeCString(ctx, text);
    return type;
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

/* The serial of the track a MediaStreamAudioSourceNode of STREAM plays
   (media-capture.js chooses as Web Audio does), or 0 when it is not live;
   -1 with an exception. */
static int64_t audio_slot_stream_track(JSContext *ctx, ScriptRuntime *host,
                                       JSValueConst stream)
{
    if (!JS_IsFunction(ctx, host->media_capture_stream_track))
        return 0;
    JSValue chosen = JS_Call(ctx, host->media_capture_stream_track, JS_UNDEFINED, 1, &stream);
    if (JS_IsException(chosen))
        return -1;
    uint32_t track = 0;
    int status = JS_ToUint32(ctx, &track, chosen);
    JS_FreeValue(ctx, chosen);
    return status < 0 ? -1 : (int64_t) track;
}

static bool audio_slot_fft_size(JSContext *ctx, uint32_t size)
{
    if (size < 32 || size > 32768 || (size & (size - 1)) != 0) {
        audio_slot_error(ctx, "fftSize must be a power of two from 32 to 32768", "IndexSizeError");
        return false;
    }
    if (size > TILEFINCH_GAME_AUDIO_FFT_SIZE_MAX) {
        audio_slot_error(ctx, "fftSize is limited to 2048", "NotSupportedError");
        return false;
    }
    return true;
}

static bool audio_slot_analyser_range(JSContext *ctx, double min_db, double max_db,
                                      double smoothing)
{
    if (!isfinite(min_db) || !isfinite(max_db) || !isfinite(smoothing)) {
        JS_ThrowTypeError(ctx, "AnalyserNode values must be finite");
        return false;
    }
    if (min_db >= max_db) {
        audio_slot_error(ctx, "minDecibels must be below maxDecibels", "IndexSizeError");
        return false;
    }
    if (smoothing < 0 || smoothing > 1) {
        audio_slot_error(ctx, "smoothingTimeConstant must be from 0 to 1", "IndexSizeError");
        return false;
    }
    return true;
}

static bool audio_slot_fft_scratch(AudioSlotRuntime *screen)
{
    if (!screen->fft_scratch)
        screen->fft_scratch = audio_slot_allocate(
            screen, 3u * TILEFINCH_GAME_AUDIO_FFT_SIZE_MAX * sizeof(float));
    return screen->fft_scratch != NULL;
}

/* A started source (or a microphone stream) holds one of the four
   active slots until its voice completes. */
static void audio_slot_add_active(JSContext *ctx, AudioSlotContext *o, JSValueConst self,
                                  unsigned index, uint32_t voice)
{
    unsigned s = o->source_count;
    o->active[s] = JS_DupValue(ctx, self);
    o->active_indices[s] = index;
    o->source_count++;
    o->nodes[index].voice = voice;
    o->nodes[index].started = true;
    o->revision++;
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
    if (JS_ToUint32(ctx, &kind, argv[2]) < 0 || kind < 1 || kind > AUDIO_SLOT_NODE_ANALYSER)
        return JS_ThrowTypeError(ctx, "private node kind");
    /* Preserve the reference shim's refusal precedence before option getters.
       The post-getter live/make_node checks remain for reentrant getters. */
    if (!audio_slot_live(ctx, o))
        return JS_EXCEPTION;
    if (!audio_slot_node_room(screen, o))
        return audio_slot_error(ctx, "Too many audio nodes", "QuotaExceededError");
    double gain = 1, pan = 0, freq = 440, detune = 0, rate = 1, loop_start = 0, loop_end = 0,
           q = 1, filter_gain = 0, min_db = -100, max_db = -30, smoothing = 0.8;
    uint32_t fft_size = 2048;
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
    if (kind == AUDIO_SLOT_NODE_FILTER) {
        JSValue v = JS_GetPropertyStr(ctx, argv[3], "type");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v)) {
            type = audio_slot_filter_type(ctx, v);
            JS_FreeValue(ctx, v);
            if (!type)
                return JS_HasException(ctx) ? JS_EXCEPTION
                                            : JS_ThrowTypeError(ctx, "Unknown BiquadFilterType");
        }
        freq = 350;
        if (!audio_slot_read_number(ctx, argv[3], "Q", 1, &q) ||
            !audio_slot_read_number(ctx, argv[3], "detune", 0, &detune) ||
            !audio_slot_read_number(ctx, argv[3], "frequency", 350, &freq) ||
            !audio_slot_read_number(ctx, argv[3], "gain", 0, &filter_gain))
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
    if (kind == AUDIO_SLOT_NODE_STREAM) {
        buffer = JS_GetPropertyStr(ctx, argv[3], "mediaStream");
        if (JS_IsException(buffer))
            return JS_EXCEPTION;
        if (!JS_IsObject(buffer)) {
            JS_FreeValue(ctx, buffer);
            return JS_ThrowTypeError(ctx, "MediaStreamAudioSourceNode needs a mediaStream");
        }
    }
    if (kind == AUDIO_SLOT_NODE_ANALYSER) {
        JSValue v = JS_GetPropertyStr(ctx, argv[3], "fftSize");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        int status = JS_IsUndefined(v) ? 0 : JS_ToUint32(ctx, &fft_size, v);
        JS_FreeValue(ctx, v);
        if (status < 0 || !audio_slot_read_number(ctx, argv[3], "maxDecibels", -30, &max_db) ||
            !audio_slot_read_number(ctx, argv[3], "minDecibels", -100, &min_db) ||
            !audio_slot_read_number(ctx, argv[3], "smoothingTimeConstant", 0.8, &smoothing) ||
            !audio_slot_fft_size(ctx, fft_size) ||
            !audio_slot_analyser_range(ctx, min_db, max_db, smoothing))
            return JS_EXCEPTION;
    }
    if (!audio_slot_live(ctx, o)) {
        JS_FreeValue(ctx, buffer);
        return JS_EXCEPTION;
    }
    uint32_t capture_track = 0;
    JsRtMediaTrackState capture_state = JS_RT_MEDIA_TRACK_ENDED;
    if (kind == AUDIO_SLOT_NODE_STREAM) {
        /* The microphone plays only through a live track of this realm,
           the one the source chose from its stream. */
        int64_t chosen = audio_slot_stream_track(ctx, screen->host, buffer);
        if (chosen < 0) {
            JS_FreeValue(ctx, buffer);
            return JS_EXCEPTION;
        }
        capture_track = (uint32_t) chosen;
        capture_state = js_rt_media_capture_track_state(screen->host, capture_track);
        if (capture_state == JS_RT_MEDIA_TRACK_ENDED || !audio_slot_live(ctx, o)) {
            JS_FreeValue(ctx, buffer);
            return JS_HasException(ctx) ? JS_EXCEPTION
                : audio_slot_error(ctx, "The MediaStream has no live microphone track",
                                   "InvalidStateError");
        }
        if (o->source_count == 4) {
            JS_FreeValue(ctx, buffer);
            return audio_slot_error(ctx, "No game-audio voice is available",
                                    "QuotaExceededError");
        }
    }
    if ((kind == AUDIO_SLOT_NODE_STREAM || kind == AUDIO_SLOT_NODE_ANALYSER) &&
        !audio_slot_create_engine(screen)) {
        JS_FreeValue(ctx, buffer);
        return JS_ThrowOutOfMemory(ctx);
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
    if (kind == AUDIO_SLOT_NODE_FILTER) {
        n->values[AUDIO_SLOT_PARAM_FREQUENCY] = n->defaults[AUDIO_SLOT_PARAM_FREQUENCY] = 440;
        n->values[AUDIO_SLOT_PARAM_FILTER_FREQUENCY] =
            n->defaults[AUDIO_SLOT_PARAM_FILTER_FREQUENCY] = audio_slot_clamp(freq, 0, 22050);
        n->values[AUDIO_SLOT_PARAM_Q] = n->defaults[AUDIO_SLOT_PARAM_Q] =
            audio_slot_clamp(q, -1000, 1000);
        n->values[AUDIO_SLOT_PARAM_FILTER_GAIN] = n->defaults[AUDIO_SLOT_PARAM_FILTER_GAIN] =
            audio_slot_clamp(filter_gain, -100, 100);
    }
    n->type = type;
    n->loop = loop;
    n->loop_start = fmax(0, loop_start);
    n->loop_end = fmax(0, loop_end);
    n->fft_size = fft_size;
    n->min_db = min_db;
    n->max_db = max_db;
    n->smoothing = smoothing;
    TilefinchGameAudio *audio = screen->host->game_audio;
    if (kind == AUDIO_SLOT_NODE_ANALYSER) {
        uint32_t handle = 0;
        if (!tilefinch_game_audio_analyser_create(audio, &handle)) {
            /* Unreachable analysers hold histories until collected. */
            JS_RunGC(screen->host->runtime);
            if (!n->used || screen->epoch != n->lane_epoch ||
                !tilefinch_game_audio_analyser_create(audio, &handle)) {
                JS_FreeValue(ctx, object);
                return audio_slot_error(ctx, "Too many AnalyserNodes (2)", "QuotaExceededError");
            }
        }
        n->analyser = handle;
    }
    if (kind == AUDIO_SLOT_NODE_STREAM) {
        AudioSlotNodeRef *ref = JS_GetOpaque(object, screen->node_class);
        n->buffer = buffer;
        n->capture_track = capture_track;
        n->capture_silent = capture_state == JS_RT_MEDIA_TRACK_DISABLED;
        TilefinchGameAudioProgram program;
        uint32_t voice = 0;
        double now = audio_slot_clock_time(ctx, o);
        if (JS_HasException(ctx) || !audio_slot_program(ctx, o, n, &program)) {
            JS_FreeValue(ctx, object);
            return JS_EXCEPTION;
        }
        if (!tilefinch_game_audio_start_stream(audio, now, &program, &voice)) {
            JS_FreeValue(ctx, object);
            return audio_slot_error(ctx, "No game-audio voice is available", "QuotaExceededError");
        }
        audio_slot_add_active(ctx, o, object, ref->index, voice);
        return object;
    }
    if (!JS_IsUndefined(buffer) && !JS_IsNull(buffer)) {
        AudioSlotBufferRef *b = JS_GetOpaque(buffer, screen->buffer_class);
        if (!b || !audio_slot_equal(b->context, argv[0])) {
            JS_FreeValue(ctx, buffer);
            JS_FreeValue(ctx, object);
            return JS_ThrowTypeError(ctx, "AudioBuffer owner mismatch");
        }
        n->buffer = buffer;
        n->buffer_set = true;
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
            if (!audio_slot_emit(ctx, 0, 1, v))
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
            if (audio_slot_emit(ctx, 1, 1, v)) {
                o->elapsed = elapsed;
                o->running = false;
                changed = true;
            } else if (!JS_HasException(ctx))
                audio_slot_error(ctx, "Native audio lifecycle publication refused",
                                 "InvalidStateError");
        }
    } else if (op == 2 && !o->closed) {
        double elapsed = audio_slot_clock_time(ctx, o);
        double v[] = {2};
        bool published = !JS_HasException(ctx)
            && (o->engine_epoch != screen->epoch
                || audio_slot_emit(ctx, 2, 1, v));
        if (published && !JS_HasException(ctx)) {
            o->elapsed = elapsed;
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
    if (!audio_slot_refresh(ctx, r->owner, (1u << r->owner->source_count) - 1)) {
        /* The edge is not made (or kept): the graph is as before the call. */
        n->target = old;
        r->owner->revision++;
        JS_FreeValue(ctx, target);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, old);
    return op ? JS_UNDEFINED : JS_DupValue(ctx, argv[0]);
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
    if (op == 6)
        return JS_NewString(ctx, audio_slot_filter_types[n->type - 1]);
    if (op == 20 || op == 21)
        return JS_NewUint32(ctx, op == 20 ? n->fft_size : n->fft_size / 2);
    if (op >= 22 && op <= 24)
        return JS_NewFloat64(ctx, op == 22 ? n->min_db : op == 23 ? n->max_db : n->smoothing);
    if (op == 25)
        return JS_DupValue(ctx, n->buffer);
    unsigned kind = (unsigned) op - 10;
    if (kind >= AUDIO_SLOT_PARAMS)
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
        /* [[buffer set]]: a source takes one AudioBuffer for its life. */
        if (b && n->buffer_set)
            return audio_slot_error(ctx, "AudioBufferSourceNode.buffer may be set once",
                                    "InvalidStateError");
        JSValue old = n->buffer;
        n->buffer = JS_IsNull(value) ? JS_UNDEFINED : JS_DupValue(ctx, value);
        if (b)
            n->buffer_set = true;
        JS_FreeValue(ctx, old);
        return JS_UNDEFINED;
    }
    if (op == 6) {
        /* An unknown BiquadFilterType is ignored, as for any enum attribute. */
        unsigned type = audio_slot_filter_type(ctx, value);
        if (JS_HasException(ctx))
            return JS_EXCEPTION;
        if (!audio_slot_live(ctx, r->owner))
            return JS_EXCEPTION;
        if (type && type != n->type) {
            unsigned previous = n->type;
            n->type = type;
            if (!audio_slot_refresh(ctx, r->owner, audio_slot_through(r->owner, n))) {
                n->type = previous;
                return JS_EXCEPTION;
            }
        }
        return JS_UNDEFINED;
    }
    if (op == 2) {
        if (!audio_slot_live(ctx, r->owner))
            return JS_EXCEPTION;
        n->loop = JS_ToBool(ctx, value);
        return JS_UNDEFINED;
    }
    if (op == 20) {
        uint32_t size = 0;
        if (JS_ToUint32(ctx, &size, value) < 0 || !audio_slot_fft_size(ctx, size))
            return JS_EXCEPTION;
        if (size != n->fft_size) {
            /* A new size starts its smoothing over. */
            n->fft_size = size;
            n->spectrum_valid = false;
            if (n->spectrum)
                memset(n->spectrum, 0, TILEFINCH_GAME_AUDIO_FFT_SIZE_MAX / 2 * sizeof(float));
        }
        return JS_UNDEFINED;
    }
    if (op >= 22 && op <= 24) {
        double v = 0;
        if (JS_ToFloat64(ctx, &v, value) < 0 ||
            !audio_slot_analyser_range(ctx, op == 22 ? v : n->min_db, op == 23 ? v : n->max_db,
                                       op == 24 ? v : n->smoothing))
            return JS_EXCEPTION;
        *(op == 22 ? &n->min_db : op == 23 ? &n->max_db : &n->smoothing) = v;
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
    /* loopStart and loopEnd are WebIDL doubles: a non-finite value throws;
       any finite one is kept and read back (start() decides what loops). */
    if (!isfinite(v))
        return JS_ThrowTypeError(ctx, "The provided double value is non-finite");
    if (op == 3)
        n->loop_start = v;
    if (op == 4)
        n->loop_end = v;
    return JS_UNDEFINED;
}

static void *audio_slot_typed_elements(JSContext *ctx, JSValueConst value, JSClassID class_id,
                                       size_t element_size, size_t *count);
static TilefinchGameAudio *audio_slot_engine_for(AudioSlotContext *o, AudioSlotNode *n);

/* AnalyserNode reads (ops): 0 getFloatFrequencyData, 1 getByteFrequencyData,
   2 getFloatTimeDomainData, 3 getByteTimeDomainData. The spectrum is
   computed here, on demand, and reused until the output advances. */
static JSValue audio_slot_analyser_data(JSContext *ctx, JSValueConst self, int argc,
                                        JSValueConst *argv, int op, JSValue *data)
{
    AudioSlotRuntime *screen = audio_slot_state_context(ctx);
    (void) data;
    AudioSlotNodeRef *r;
    AudioSlotNode *n = audio_slot_node(ctx, self, &r);
    if (!n)
        return JS_EXCEPTION;
    if (n->kind != AUDIO_SLOT_NODE_ANALYSER)
        return JS_ThrowTypeError(ctx, "Audio node subtype mismatch");
    bool bytes = op == 1 || op == 3;
    size_t count = 0;
    void *out = audio_slot_typed_elements(ctx, argc ? argv[0] : JS_UNDEFINED,
                                          bytes ? screen->byte_class : screen->float_class,
                                          bytes ? 1 : 4, &count);
    if (!out)
        return JS_EXCEPTION;
    if (!audio_slot_fft_scratch(screen))
        return JS_ThrowOutOfMemory(ctx);
    float *time = screen->fft_scratch;
    unsigned size = n->fft_size;
    uint32_t stamp = n->spectrum_stamp;
    TilefinchGameAudio *audio = audio_slot_engine_for(r->owner, n);
    if (!audio || !tilefinch_game_audio_analyser_read(audio, n->analyser, time, size, &stamp))
        memset(time, 0, size * sizeof(float));
    if (op >= 2) {
        if (count > size)
            count = size;
        if (op == 2)
            memcpy(out, time, count * sizeof(float));
        else
            for (size_t i = 0; i < count; i++) {
                float v = floorf(128.0f * (1.0f + time[i]));
                ((uint8_t *) out)[i] = (uint8_t) (v < 0 ? 0 : v > 255 ? 255 : v);
            }
        return JS_UNDEFINED;
    }
    if (!n->spectrum) {
        n->spectrum =
            audio_slot_allocate(screen, TILEFINCH_GAME_AUDIO_FFT_SIZE_MAX / 2 * sizeof(float));
        if (!n->spectrum)
            return JS_ThrowOutOfMemory(ctx);
    }
    if (!n->spectrum_valid || n->spectrum_stamp != stamp) {
        tilefinch_game_audio_spectrum(time, size, (float) n->smoothing, n->spectrum,
                                      time + TILEFINCH_GAME_AUDIO_FFT_SIZE_MAX);
        n->spectrum_stamp = stamp;
        n->spectrum_valid = true;
    }
    if (count > size / 2)
        count = size / 2;
    float low = (float) n->min_db, scale = (float) (255.0 / (n->max_db - n->min_db));
    for (size_t k = 0; k < count; k++) {
        float db = 20.0f * log10f(n->spectrum[k]);
        if (op == 0)
            ((float *) out)[k] = db;
        else {
            float v = floorf(scale * (db - low));
            ((uint8_t *) out)[k] = (uint8_t) (!(v > 0) ? 0 : v > 255 ? 255 : v);
        }
    }
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
            audio_slot_emit(ctx, 4, 3, v);
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
    if (!isfinite(offset) || offset < 0 || !isfinite(duration) || duration < 0)
        return JS_ThrowRangeError(ctx, "Audio offset and duration must be nonnegative");
    if (r->owner->source_count == 4)
        return audio_slot_error(ctx, "No game-audio voice is available", "QuotaExceededError");
    TilefinchGameAudioProgram program;
    if (!audio_slot_program(ctx, r->owner, n, &program))
        return JS_EXCEPTION;
    double now = audio_slot_clock_time(ctx, r->owner);
    if (JS_HasException(ctx))
        return JS_EXCEPTION;
    /* The specification's actual loop points: loopStart and loopEnd loop
       only as a valid pair (0 <= loopStart < loopEnd, loopEnd > 0); any
       other pair loops the whole buffer (0 and 0 to the engine). */
    bool loop_points = n->loop_start >= 0 && n->loop_end > 0 && n->loop_start < n->loop_end;
    TilefinchGameAudioVoiceStart start = {
        .buffer = b ? b->info.handle : 0u,
        .oscillator = (uint8_t) (b ? 0u : n->type),
        .offset = offset,
        .duration = duration,
        .loop = n->loop,
        .loop_start = loop_points ? n->loop_start : 0,
        .loop_end = loop_points ? n->loop_end : 0,
        .now = now,
        .when = now + delay};
    uint32_t voice = 0;
    if (!tilefinch_game_audio_start_program(screen->host->game_audio, &start, &program, &voice))
        return audio_slot_error(ctx, "No game-audio voice is available", "QuotaExceededError");
    audio_slot_add_active(ctx, r->owner, self, r->index, voice);
    return JS_UNDEFINED;
}

static AudioSlotNode *audio_slot_param_node(JSContext *ctx, JSValueConst self,
                                            AudioSlotParamRef **p, AudioSlotNodeRef **r)
{
    AudioSlotRuntime *screen = audio_slot_state_context(ctx);
    *p = JS_GetOpaque2(ctx, self, screen->slot_param_class);
    return *p ? audio_slot_node(ctx, (*p)->node, r) : NULL;
}

/* Nominal ranges. Automation values are clamped to them when computed. */
static double audio_slot_minimum(unsigned kind)
{
    static const double minimum[AUDIO_SLOT_PARAMS] = {0, -1, 1, -1200, .25, 0, -1000, -100};
    return minimum[kind];
}

static double audio_slot_maximum(unsigned kind)
{
    static const double maximum[AUDIO_SLOT_PARAMS] = {4, 1, 20000, 1200, 4, 22050, 1000, 100};
    return maximum[kind];
}

static TilefinchGameAudio *audio_slot_engine_for(AudioSlotContext *o, AudioSlotNode *n)
{
    AudioSlotRuntime *screen = o->screen;
    if (screen->retired || o->closed || o->engine_epoch != screen->epoch ||
        n->lane_epoch != screen->epoch)
        return NULL;
    return screen->host->game_audio;
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
    /* An automated value reads as the timeline computes it now, from the
       same segments the mixer renders. */
    TilefinchGameAudio *audio = audio_slot_engine_for(r->owner, n);
    if (op == 0 && audio != NULL && n->lanes[p->kind] != 0u) {
        double now = audio_slot_clock_time(ctx, r->owner);
        if (JS_HasException(ctx))
            return JS_EXCEPTION;
        double computed = 0;
        if (tilefinch_game_audio_lane_value(audio, n->lanes[p->kind], now, &computed))
            value = computed;
    }
    return JS_NewFloat64(ctx, value);
}

static JSValue audio_slot_timeline_error(JSContext *ctx, TilefinchAudioTimelineStatus status)
{
    switch (status) {
    case TILEFINCH_AUDIO_TIMELINE_TYPE_ERROR:
        return JS_ThrowTypeError(ctx, "AudioParam automation values must be finite");
    case TILEFINCH_AUDIO_TIMELINE_RANGE_ERROR:
        return JS_ThrowRangeError(ctx, "AudioParam automation argument is out of range");
    case TILEFINCH_AUDIO_TIMELINE_INVALID_STATE:
        return audio_slot_error(ctx, "An audio curve needs at least two samples",
                                "InvalidStateError");
    case TILEFINCH_AUDIO_TIMELINE_OVERLAP:
        return audio_slot_error(ctx, "Automation overlaps a value curve", "NotSupportedError");
    case TILEFINCH_AUDIO_TIMELINE_SECOND_CURVE:
        return audio_slot_error(ctx, "One value curve per AudioParam is supported",
                                "NotSupportedError");
    case TILEFINCH_AUDIO_TIMELINE_HORIZON:
        return audio_slot_error(ctx, "Scheduling exceeds the bounded ten-second window",
                                "NotSupportedError");
    case TILEFINCH_AUDIO_TIMELINE_EVENT_QUOTA:
        return audio_slot_error(ctx, "AudioParam automation is limited to 32 events",
                                "QuotaExceededError");
    case TILEFINCH_AUDIO_TIMELINE_POINT_QUOTA:
        return audio_slot_error(ctx, "Audio curves are limited to 64 samples",
                                "QuotaExceededError");
    default:
        return JS_UNDEFINED;
    }
}

/* The parameter's lane, made on its first automation. Unreachable nodes
   may still hold lanes until the cycle collector finds them, so one
   collection precedes a refusal. */
static bool audio_slot_param_lane(JSContext *ctx, AudioSlotContext *o, AudioSlotNode *n,
                                  unsigned kind, double now, uint32_t *lane)
{
    AudioSlotRuntime *screen = o->screen;
    if (n->lanes[kind] != 0u) {
        *lane = n->lanes[kind];
        return true;
    }
    if (!audio_slot_create_engine(screen)) {
        JS_ThrowOutOfMemory(ctx);
        return false;
    }
    TilefinchGameAudio *audio = screen->host->game_audio;
    double low = audio_slot_minimum(kind), high = audio_slot_maximum(kind);
    if (!tilefinch_game_audio_lane_create(audio, n->values[kind], low, high, lane)
        && (audio_slot_reclaim_lanes(ctx, o, now) == 0u || !n->used
            || screen->epoch != n->lane_epoch
            || !tilefinch_game_audio_lane_create(audio, n->values[kind], low, high, lane))) {
        JS_RunGC(screen->host->runtime);
        if (!n->used || screen->epoch != n->lane_epoch ||
            !tilefinch_game_audio_lane_create(audio, n->values[kind], low, high, lane)) {
            audio_slot_error(ctx, "Too many automated AudioParams (16)", "QuotaExceededError");
            return false;
        }
    }
    if (n->anchored & (1u << kind))
        (void) tilefinch_game_audio_lane_anchor(audio, *lane, n->anchor_time[kind],
                                                n->values[kind]);
    n->lanes[kind] = *lane;
    /* Programs now follow the lane instead of the static value. A path
       that cannot take it keeps the parameter static: no lane stays
       attached for every later program build to trip over. */
    if (!audio_slot_refresh(ctx, o, audio_slot_through(o, n))) {
        n->lanes[kind] = 0u;
        tilefinch_game_audio_lane_release(audio, *lane);
        *lane = 0u;
        return false;
    }
    n->anchored &= (uint8_t) ~(1u << kind);
    return true;
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
    if (!isfinite(v))
        return JS_UNDEFINED;
    double value = audio_slot_clamp(v, audio_slot_minimum(p->kind), audio_slot_maximum(p->kind));
    TilefinchGameAudio *audio = audio_slot_engine_for(r->owner, n);
    if (audio != NULL && n->lanes[p->kind] != 0u) {
        /* With events this is setValueAtTime(value, currentTime). */
        double now = audio_slot_clock_time(ctx, r->owner);
        if (JS_HasException(ctx))
            return JS_EXCEPTION;
        TilefinchAudioTimelineStatus status =
            tilefinch_game_audio_lane_set_value(audio, n->lanes[p->kind], now, value);
        if (status != TILEFINCH_AUDIO_TIMELINE_OK)
            return audio_slot_timeline_error(ctx, status);
        n->values[p->kind] = value;
        return JS_UNDEFINED;
    }
    double previous = n->values[p->kind];
    n->values[p->kind] = value;
    if (r->owner->closed)
        return JS_UNDEFINED;
    if (n->anchored & (1u << p->kind)) {
        /* With an anchored event this is setValueAtTime(value, currentTime). */
        double now = audio_slot_clock_time(ctx, r->owner);
        if (JS_HasException(ctx)) {
            n->values[p->kind] = previous;
            return JS_EXCEPTION;
        }
        n->anchor_time[p->kind] = now;
    }
    if (!audio_slot_refresh(ctx, r->owner, audio_slot_through(r->owner, n))) {
        n->values[p->kind] = previous;
        return JS_EXCEPTION;
    }
    return JS_UNDEFINED;
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

/* ops: 0 cancelScheduledValues, 1 setValueCurveAtTime, 2 setTargetAtTime,
   3 setValueAtTime, 4 linearRampToValueAtTime, 5 exponentialRampToValueAtTime,
   6 cancelAndHoldAtTime. */
static JSValue audio_slot_param_schedule(JSContext *ctx, JSValueConst self, int argc,
                                         JSValueConst *argv, int op, JSValue *data)
{
    (void) data;
    AudioSlotParamRef *p;
    AudioSlotNodeRef *r;
    AudioSlotNode *n = audio_slot_param_node(ctx, self, &p, &r);
    if (!n)
        return JS_EXCEPTION;
    static const unsigned arity[7] = {1, 3, 3, 2, 2, 2, 1};
    if ((unsigned) argc < arity[op])
        return JS_ThrowTypeError(ctx, "AudioParam automation arguments required");
    double value = 0, when = 0, aux = 0;
    float points[TILEFINCH_AUDIO_TIMELINE_CURVE_LIMIT];
    size_t count = 0;
    if (op == 0 || op == 6) {
        if (JS_ToFloat64(ctx, &when, argv[0]) < 0)
            return JS_EXCEPTION;
    } else if (op == 1) {
        if (JS_ToFloat64(ctx, &when, argv[1]) < 0 || JS_ToFloat64(ctx, &aux, argv[2]) < 0)
            return JS_EXCEPTION;
        if (!audio_slot_curve_values(ctx, argv[0], points, &count))
            return JS_EXCEPTION;
    } else {
        if (JS_ToFloat64(ctx, &value, argv[0]) < 0 || JS_ToFloat64(ctx, &when, argv[1]) < 0 ||
            (op == 2 && JS_ToFloat64(ctx, &aux, argv[2]) < 0))
            return JS_EXCEPTION;
    }
    if (!audio_slot_live(ctx, r->owner))
        return JS_EXCEPTION;
    double now = audio_slot_clock_time(ctx, r->owner);
    if (JS_HasException(ctx))
        return JS_EXCEPTION;
    TilefinchGameAudio *audio = r->owner->screen->host->game_audio;
    bool cancel = op == 0 || op == 6;
    if (cancel && (n->lanes[p->kind] == 0u || audio == NULL)) {
        /* Nothing is scheduled; only the arguments are checked. */
        if (!isfinite(when))
            return JS_ThrowTypeError(ctx, "AudioParam automation values must be finite");
        if (when < 0)
            return JS_ThrowRangeError(ctx, "AudioParam automation argument is out of range");
        /* An anchored value's event goes as its lane's would have. */
        if (op == 0 && (n->anchored & (1u << p->kind))
            && n->anchor_time[p->kind] >= fmax(when, now))
            n->anchored &= (uint8_t) ~(1u << p->kind);
        return JS_DupValue(ctx, self);
    }
    if (p->kind == AUDIO_SLOT_PARAM_PAN) {
        /* The panner folds into static gains: only an immediate value. */
        if (op == 3 && isfinite(value) && isfinite(when) && when >= 0 && when <= now) {
            JSValue arg = JS_NewFloat64(ctx, value);
            JSValue result = audio_slot_param_set(ctx, self, 1, &arg);
            JS_FreeValue(ctx, arg);
            return JS_IsException(result) ? result : JS_DupValue(ctx, self);
        }
        return audio_slot_error(ctx, "StereoPannerNode.pan automation is unavailable",
                                "NotSupportedError");
    }
    uint32_t lane = 0;
    if (!audio_slot_param_lane(ctx, r->owner, n, p->kind, now, &lane))
        return JS_EXCEPTION;
    audio = r->owner->screen->host->game_audio;
    static const TilefinchAudioEventKind kinds[7] = {0, TILEFINCH_AUDIO_EVENT_CURVE,
        TILEFINCH_AUDIO_EVENT_TARGET, TILEFINCH_AUDIO_EVENT_SET, TILEFINCH_AUDIO_EVENT_LINEAR,
        TILEFINCH_AUDIO_EVENT_EXPONENTIAL, 0};
    TilefinchAudioTimelineStatus status =
        cancel ? tilefinch_game_audio_lane_cancel(audio, lane, now, when, op == 6)
               : tilefinch_game_audio_lane_insert(audio, lane, now, kinds[op], value, when, aux,
                                                  points, count);
    if (status != TILEFINCH_AUDIO_TIMELINE_OK)
        return audio_slot_timeline_error(ctx, status);
    return JS_DupValue(ctx, self);
}

/* Why decodeAudioData refused bytes it could not decode. Only PCM WAV
   decodes here; compressed formats are named so pages and logs say why. */
static const char *audio_slot_decode_refusal(const uint8_t *bytes, size_t length)
{
    MediaMp3FrameHeader header;
    if (length >= 4u && memcmp(bytes, "OggS", 4u) == 0)
        return "Ogg audio (Vorbis, Opus) is not supported; use PCM WAV";
    if (length >= 4u && memcmp(bytes, "RIFF", 4u) == 0)
        return "Only bounded PCM WAV audio is supported (8 buffers, 512 KiB)";
    if ((length >= 10u && media_mp3_id3v2_length(bytes, length) != 0u) ||
        (length >= 4u && media_mp3_parse_header(bytes, &header)))
        return "MP3 is not decoded for Web Audio here; use PCM WAV";
    return "Only bounded PCM WAV audio is supported";
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
        return audio_slot_error(ctx, audio_slot_decode_refusal(bytes, length), "EncodingError");
    }
    b->context = JS_DupValue(ctx, argv[0]);
    b->channels[0] = b->channels[1] = JS_UNDEFINED;
    b->engine_epoch = screen->epoch;
    JS_SetOpaque(object, b);
    screen->buffer_records++;
    return object;
}

/* cap.newBuffer(prototype, context or null, numberOfChannels, length,
   sampleRate): createBuffer, and new AudioBuffer(options), which belongs to
   the page's open game AudioContext. */
static JSValue audio_slot_new_buffer(JSContext *ctx, JSValueConst self, int argc,
                                     JSValueConst *argv)
{
    (void) self;
    AudioSlotRuntime *screen = audio_slot_state_context(ctx);
    if (argc != 5)
        return JS_ThrowTypeError(ctx, "private buffer args");
    uint32_t channels = 0, length = 0;
    double rate = 0;
    if (JS_ToUint32(ctx, &channels, argv[2]) < 0 || JS_ToUint32(ctx, &length, argv[3]) < 0 ||
        JS_ToFloat64(ctx, &rate, argv[4]) < 0)
        return JS_EXCEPTION;
    JSValueConst context = JS_IsObject(argv[1]) ? argv[1] : screen->active_context_root;
    AudioSlotContext *o = JS_GetOpaque(context, screen->context_class);
    if (!o)
        return audio_slot_error(ctx, "An AudioBuffer needs an open AudioContext",
                                "NotSupportedError");
    if (!audio_slot_live(ctx, o))
        return JS_EXCEPTION;
    if (channels == 0 || channels > 2)
        return audio_slot_error(ctx, "AudioBuffers have one or two channels here",
                                "NotSupportedError");
    if (length == 0)
        return audio_slot_error(ctx, "An AudioBuffer needs at least one frame",
                                "NotSupportedError");
    if (!(rate >= TILEFINCH_GAME_AUDIO_BUFFER_RATE_MIN) ||
        rate > TILEFINCH_GAME_AUDIO_BUFFER_RATE_MAX)
        return audio_slot_error(ctx, "AudioBuffer sample rates are 8000-96000 Hz here",
                                "NotSupportedError");
    AudioSlotBufferRef *b = audio_slot_allocate(screen, sizeof(*b));
    if (!b)
        return JS_ThrowOutOfMemory(ctx);
    JSValue object = JS_NewObjectProtoClass(
        ctx, JS_IsObject(argv[0]) ? argv[0] : screen->prototypes[9], screen->buffer_class);
    if (JS_IsException(object)) {
        budget_free(screen->budget, b);
        return object;
    }
    if (!audio_slot_create_engine(screen) ||
        !tilefinch_game_audio_create_buffer(screen->host->game_audio, channels, length,
                                            (uint32_t) (rate + .5), &b->info)) {
        JS_FreeValue(ctx, object);
        budget_free(screen->budget, b);
        return audio_slot_error(ctx,
                                "AudioBuffer exceeds the game-audio PCM pool (512 KiB at "
                                "4 bytes per sample) or its eight buffers",
                                "NotSupportedError");
    }
    b->context = JS_DupValue(ctx, context);
    b->channels[0] = b->channels[1] = JS_UNDEFINED;
    b->engine_epoch = screen->epoch;
    JS_SetOpaque(object, b);
    screen->buffer_records++;
    return object;
}

static void audio_slot_storage_free(JSRuntime *rt, void *opaque, void *ptr)
{
    (void) rt;
    (void) ptr;
    tilefinch_game_audio_storage_release(opaque);
}

static TilefinchGameAudio *audio_slot_buffer_engine(JSContext *ctx, AudioSlotBufferRef *b)
{
    AudioSlotRuntime *screen = audio_slot_state_context(ctx);
    if (screen->retired || b->engine_epoch != screen->epoch || !screen->host->game_audio) {
        audio_slot_error(ctx, "The AudioBuffer's AudioContext is closed", "InvalidStateError");
        return NULL;
    }
    return screen->host->game_audio;
}

/* getChannelData: the engine's own float plane, shared, made once per
   channel. Writes are heard when the mixer next reaches those frames. */
static JSValue audio_slot_channel_view(JSContext *ctx, AudioSlotBufferRef *b, uint32_t channel)
{
    if (!JS_IsUndefined(b->channels[channel]))
        return JS_DupValue(ctx, b->channels[channel]);
    TilefinchGameAudio *audio = audio_slot_buffer_engine(ctx, b);
    if (!audio)
        return JS_EXCEPTION;
    TilefinchGameAudioStorage *storage = NULL;
    float *plane = tilefinch_game_audio_channel_data(audio, b->info.handle, channel, &storage);
    if (!plane)
        return audio_slot_error(ctx, "AudioBuffer samples exceed the game-audio PCM pool",
                                "NotSupportedError");
    JSValue array = JS_NewArrayBuffer(ctx, (uint8_t *) plane,
                                      (size_t) b->info.frames * sizeof(float),
                                      audio_slot_storage_free, storage, false);
    if (JS_IsException(array)) {
        /* QuickJS does not call the free function on failure. */
        tilefinch_game_audio_storage_release(storage);
        return array;
    }
    /* Offset and length are explicit; a one-argument view over this
       external buffer reported length 0 here. */
    JSValue arguments[3] = {array, JS_NewInt32(ctx, 0), JS_NewUint32(ctx, b->info.frames)};
    JSValue view = JS_NewTypedArray(ctx, 3, arguments, JS_TYPED_ARRAY_FLOAT32);
    JS_FreeValue(ctx, array);
    if (JS_IsException(view))
        return view;
    b->channels[channel] = JS_DupValue(ctx, view);
    return view;
}

/* A Float32Array (or Uint8Array) argument's elements, or NULL with an
   exception. */
static void *audio_slot_typed_elements(JSContext *ctx, JSValueConst value, JSClassID class_id,
                                       size_t element_size, size_t *count)
{
    if (!JS_IsObject(value) || JS_GetClassID(value) != class_id) {
        JS_ThrowTypeError(ctx, element_size == 4 ? "A Float32Array is required"
                                                 : "A Uint8Array is required");
        return NULL;
    }
    size_t offset = 0, length = 0, element = 0, storage = 0;
    JSValue owner = JS_GetTypedArrayBuffer(ctx, value, &offset, &length, &element);
    if (JS_IsException(owner))
        return NULL;
    uint8_t *bytes = JS_GetArrayBuffer(ctx, &storage, owner);
    JS_FreeValue(ctx, owner);
    if (!bytes || element != element_size || offset > storage || length > storage - offset) {
        if (!JS_HasException(ctx))
            JS_ThrowTypeError(ctx, "The typed array is detached");
        return NULL;
    }
    *count = length / element_size;
    return bytes + offset;
}

static float *audio_slot_float_elements(JSContext *ctx, JSValueConst value, size_t *count)
{
    return audio_slot_typed_elements(ctx, value, audio_slot_state_context(ctx)->float_class, 4,
                                     count);
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

/* ops: 0 getChannelData(channel), 1 copyFromChannel(destination, channel,
   offset), 2 copyToChannel(source, channel, offset). */
static JSValue audio_slot_buffer_method(JSContext *ctx, JSValueConst self, int argc,
                                        JSValueConst *argv, int op, JSValue *data)
{
    (void) data;
    AudioSlotRuntime *s = audio_slot_state_context(ctx);
    AudioSlotBufferRef *b = JS_GetOpaque2(ctx, self, s->buffer_class);
    if (!b)
        return JS_EXCEPTION;
    if (argc < (op == 0 ? 1 : 2))
        return JS_ThrowTypeError(ctx, "AudioBuffer arguments required");
    uint32_t channel = 0, offset = 0;
    if (JS_ToUint32(ctx, &channel, argv[op == 0 ? 0 : 1]) < 0 ||
        (op != 0 && argc > 2 && JS_ToUint32(ctx, &offset, argv[2]) < 0))
        return JS_EXCEPTION;
    if (channel >= b->info.channels)
        return audio_slot_error(ctx, "No such AudioBuffer channel", "IndexSizeError");
    if (op == 0)
        return audio_slot_channel_view(ctx, b, channel);
    size_t count = 0;
    float *elements = audio_slot_float_elements(ctx, argv[0], &count);
    if (!elements)
        return JS_EXCEPTION;
    if (offset >= b->info.frames)
        return JS_UNDEFINED;
    if (count > b->info.frames - offset)
        count = b->info.frames - offset;
    if (op == 1 && JS_IsUndefined(b->channels[channel])) {
        /* Reading integer PCM needs no float copy. */
        TilefinchGameAudio *audio = audio_slot_buffer_engine(ctx, b);
        if (!audio)
            return JS_EXCEPTION;
        tilefinch_game_audio_copy_from_channel(audio, b->info.handle, channel, offset, elements,
                                               count);
        return JS_UNDEFINED;
    }
    JSValue view = audio_slot_channel_view(ctx, b, channel);
    if (JS_IsException(view))
        return view;
    size_t plane_count = 0;
    float *plane = audio_slot_float_elements(ctx, view, &plane_count);
    JS_FreeValue(ctx, view);
    if (!plane)
        return JS_EXCEPTION;
    /* A transferred or detached view has fewer elements than frames. */
    if (offset >= plane_count)
        return JS_UNDEFINED;
    if (count > plane_count - offset)
        count = plane_count - offset;
    if (op == 1)
        memmove(elements, plane + offset, count * sizeof(float));
    else
        memmove(plane + offset, elements, count * sizeof(float));
    return JS_UNDEFINED;
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

static JSValue audio_slot_install_slots(JSContext *ctx, JSValueConst self, int argc,
                                        JSValueConst *argv)
{
    AudioSlotRuntime *screen = audio_slot_state_context(ctx);
    (void) self;
    if (argc != 3)
        return JS_ThrowTypeError(ctx, "private slots install");
    for (unsigned i = 0; i < AUDIO_SLOT_PROTOTYPES; i++) {
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
    INSTALL(audio_slot_accessor(ctx, screen->prototypes[10], "frequency", audio_slot_node_get,
                                500 + 10 + AUDIO_SLOT_PARAM_FILTER_FREQUENCY, NULL));
    INSTALL(audio_slot_accessor(ctx, screen->prototypes[10], "detune", audio_slot_node_get,
                                500 + 10 + AUDIO_SLOT_PARAM_DETUNE, NULL));
    INSTALL(audio_slot_accessor(ctx, screen->prototypes[10], "Q", audio_slot_node_get,
                                500 + 10 + AUDIO_SLOT_PARAM_Q, NULL));
    INSTALL(audio_slot_accessor(ctx, screen->prototypes[10], "gain", audio_slot_node_get,
                                500 + 10 + AUDIO_SLOT_PARAM_FILTER_GAIN, NULL));
    INSTALL(audio_slot_accessor(ctx, screen->prototypes[10], "type", audio_slot_node_get, 506,
                                audio_slot_node_set));
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
                              audio_slot_param_schedule, 3));
    INSTALL(audio_slot_method(ctx, screen->prototypes[8], "linearRampToValueAtTime", 2,
                              audio_slot_param_schedule, 4));
    INSTALL(audio_slot_method(ctx, screen->prototypes[8], "exponentialRampToValueAtTime", 2,
                              audio_slot_param_schedule, 5));
    INSTALL(audio_slot_method(ctx, screen->prototypes[8], "cancelAndHoldAtTime", 1,
                              audio_slot_param_schedule, 6));
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
    INSTALL(audio_slot_accessor(ctx, screen->prototypes[11], "mediaStream", audio_slot_node_get,
                                600 + 25, NULL));
    const char *analyserprops[] = {"fftSize", "frequencyBinCount", "minDecibels", "maxDecibels",
                                   "smoothingTimeConstant"};
    for (int i = 0; i < 5; i++)
        INSTALL(audio_slot_accessor(ctx, screen->prototypes[12], analyserprops[i],
                                    audio_slot_node_get, 720 + i,
                                    i == 1 ? NULL : audio_slot_node_set));
    const char *analysermethods[] = {"getFloatFrequencyData", "getByteFrequencyData",
                                     "getFloatTimeDomainData", "getByteTimeDomainData"};
    for (int i = 0; i < 4; i++)
        INSTALL(audio_slot_method(ctx, screen->prototypes[12], analysermethods[i], 1,
                                  audio_slot_analyser_data, i));
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
        /* A microphone source has no ended event: its track ended. */
        bool scheduled = o->nodes[o->active_indices[i]].kind != AUDIO_SLOT_NODE_STREAM;
        o->nodes[o->active_indices[i]].voice = 0;
        for (unsigned j = i + 1; j < o->source_count; j++) {
            o->active[j - 1] = o->active[j];
            o->active_indices[j - 1] = o->active_indices[j];
        }
        o->active[--o->source_count] = JS_UNDEFINED;
        o->revision++;
        JSValue result = scheduled ? audio_slot_notify(ctx, o, source, "ended") : JS_UNDEFINED;
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
                           "AudioContext",         "webkitAudioContext", "BiquadFilterNode",
                           "MediaStreamAudioSourceNode", "AnalyserNode"};
#define AUDIO_SLOT_PUBLISHED 14u
    JSAtom atoms[AUDIO_SLOT_PUBLISHED] = {0};
    JSPropertyDescriptor old[AUDIO_SLOT_PUBLISHED];
    JSValue values[AUDIO_SLOT_PUBLISHED];
    int found[AUDIO_SLOT_PUBLISHED] = {0};
    for (unsigned i = 0; i < AUDIO_SLOT_PUBLISHED; i++) {
        old[i] = (JSPropertyDescriptor) {
            .value = JS_UNDEFINED, .getter = JS_UNDEFINED, .setter = JS_UNDEFINED};
        values[i] = JS_UNDEFINED;
    }
    JSValue global = JS_GetGlobalObject(ctx);
    bool okay = !JS_IsException(global);
    unsigned published = 0;
    /* Capture and admit every descriptor before exposing the first constructor.
       The fresh array and this function remain inside the bootstrap closure. */
    for (unsigned i = 0; okay && i < AUDIO_SLOT_PUBLISHED; i++) {
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
    for (unsigned i = 0; okay && i < AUDIO_SLOT_PUBLISHED; i++) {
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
    for (unsigned i = 0; i < AUDIO_SLOT_PUBLISHED; i++) {
        JS_FreeAtom(ctx, atoms[i]);
        JS_FreeValue(ctx, values[i]);
        JS_FreeValue(ctx, old[i].value);
        JS_FreeValue(ctx, old[i].getter);
        JS_FreeValue(ctx, old[i].setter);
    }
    JS_FreeValue(ctx, global);
#undef AUDIO_SLOT_PUBLISHED
    return okay ? JS_UNDEFINED : JS_EXCEPTION;
}

static void audio_slots_release_values(ScriptRuntime *host)
{
    AudioSlotRuntime *s = host->game_audio_slots;
    if (!s || !host->context)
        return;
    for (unsigned i = 0; i < AUDIO_SLOT_PROTOTYPES; i++) {
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

/* At each advance, on the browser thread and without JavaScript: the
   microphone's new samples into the engine for MediaStreamAudioSourceNodes,
   and whether they may be heard (headphones only). Each stream source
   follows its own track: silent while the page disabled it, stopped once
   it or the capture session ended. */
_Static_assert(TILEFINCH_GAME_AUDIO_STREAM_SAMPLES / 2 <= MEDIA_CAPTURE_READ_SAMPLES,
               "one pump read fits what the capture ring keeps readable");

static void audio_slots_pump(ScriptRuntime *host)
{
    AudioSlotRuntime *s = host->game_audio_slots;
    TilefinchGameAudio *audio = host->game_audio;
    AudioSlotContext *o = s && !s->retired && audio
        ? JS_GetOpaque(s->active_context_root, s->context_class) : NULL;
    if (!o || o->closed || o->engine_epoch != s->epoch)
        return;
    unsigned streams = 0;
    for (unsigned i = 0; i < o->source_count; i++) {
        AudioSlotNode *n = &o->nodes[o->active_indices[i]];
        if (n->kind != AUDIO_SLOT_NODE_STREAM || !n->voice)
            continue;
        JsRtMediaTrackState state = js_rt_media_capture_track_state(host, n->capture_track);
        if (state == JS_RT_MEDIA_TRACK_ENDED) {
            tilefinch_game_audio_stop(audio, n->voice, 0);
            continue;
        }
        streams++;
        bool silent = state == JS_RT_MEDIA_TRACK_DISABLED;
        if (silent == n->capture_silent)
            continue;
        n->capture_silent = silent;
        TilefinchGameAudioProgram program;
        if (audio_slot_program(host->context, o, n, &program)) {
            (void) tilefinch_game_audio_set_program(audio, n->voice, &program);
            continue;
        }
        /* A graph the program cannot express: no script is running to see
           the error, so the source ends instead of playing unsilenced. */
        JS_FreeValue(host->context, JS_GetException(host->context));
        tilefinch_game_audio_stop(audio, n->voice, 0);
    }
    if (!streams) {
        /* No microphone audio stays in the engine once its sources end. */
        if (s->stream_cursor != UINT32_MAX && tilefinch_game_audio_stream_reset(audio))
            s->stream_cursor = UINT32_MAX;
        return;
    }
    tilefinch_game_audio_stream_audible(audio, tilefinch_platform_audio_output_private());
    if (!audio_slot_fft_scratch(s))
        return;
    uint32_t count = media_capture_session_read_since(
        s->fft_scratch, TILEFINCH_GAME_AUDIO_STREAM_SAMPLES / 2, &s->stream_cursor);
    tilefinch_game_audio_stream_write(audio, s->fft_scratch, count);
    memset(s->fft_scratch, 0, count * sizeof(float));
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
    s->stream_cursor = UINT32_MAX;
    s->active_context_root = s->notify_value = s->dom_value = s->promise_value = s->resolve_value =
        s->reject_value = JS_UNDEFINED;
    for (unsigned i = 0; i < AUDIO_SLOT_PROTOTYPES; i++)
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
        argument = JS_NewInt32(ctx, 0);
        sample = JS_NewTypedArray(ctx, 1, &argument, JS_TYPED_ARRAY_UINT8);
        if (!JS_IsException(sample))
            s->byte_class = JS_GetClassID(sample);
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
        {"publish", audio_slot_publish_slots, 1},    {"newBuffer", audio_slot_new_buffer, 5}};

    if (JS_IsException(cap))
        okay = false;
    for (unsigned i = 0; okay && i < 8; i++) {
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
                                     JS_READ_OBJ_BYTECODE | JS_READ_OBJ_ROM_DATA |
                                         JS_READ_OBJ_PLATFORM_METHODS);
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
