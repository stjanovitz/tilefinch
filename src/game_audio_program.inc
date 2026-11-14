/* Program voices, automation lanes and float32 buffers for game_audio.c.
 *
 * The browser thread owns every lane's specification timeline and compiles
 * it into at most GAME_AUDIO_LANE_SEGMENTS segments, published under a
 * sequence counter. The mixer copies a lane's segments when that counter
 * changes and evaluates them per sample, so automation needs no JavaScript
 * after scheduling. A torn read keeps the previous complete copy for one
 * block; the mixer never waits for the browser thread.
 *
 * Lifetimes: a released lane or buffer is freed only once no voice that the
 * mixer may still read references it (program generations the mixer has not
 * acknowledged count as references) and no mixer block that began before
 * then is still running (mix_epoch is odd while a block runs).
 */

#define GAME_AUDIO_SEGMENT_WORDS 5u
#define GAME_AUDIO_FRAMES_PER_SECOND 44100.0

struct TilefinchGameAudioStorage {
    Budget *budget;
    uint32_t references; /* browser thread only */
    size_t bytes;
    float samples[];
};

typedef struct {
    int32_t start; /* frames from the lane base; INT32_MIN: from the start */
    uint32_t kind; /* TilefinchAudioSegmentKind */
    float a, b, c;
} GameAudioLaneSegment;

struct GameAudioLane {
    TilefinchAudioTimeline timeline; /* browser thread */
    double base_time;                /* browser: context time of the base */
    uint32_t handle;
    uint32_t free_epoch;
    bool retired;
    float minimum, maximum;          /* immutable after creation */
    _Atomic uint32_t sequence;       /* odd while the browser publishes */
    _Atomic uint32_t shared_base, shared_count, shared_curve_count;
    _Atomic uint32_t shared_words[GAME_AUDIO_LANE_SEGMENTS * GAME_AUDIO_SEGMENT_WORDS];
    _Atomic uint32_t shared_curve[TILEFINCH_AUDIO_TIMELINE_CURVE_LIMIT];
    /* Mixer copy. Initialized by the browser before any program names the
       lane; afterwards mixer-owned. */
    uint32_t mixer_sequence, mixer_block, base, count, curve_count;
    GameAudioLaneSegment segments[GAME_AUDIO_LANE_SEGMENTS];
    float curve[TILEFINCH_AUDIO_TIMELINE_CURVE_LIMIT];
};

struct GameAudioProgramScratch {
    float source[2][TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES];
    float gain[TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES];
    float lane[TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES];
    float sum[2u * TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES];
    int32_t program_sum[2u * TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES];
    /* Each analyser's input for this block. */
    float tap[TILEFINCH_GAME_AUDIO_ANALYSER_LIMIT][TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES];
    uint32_t tapped; /* bit per analyser whose tap this block has cleared */
};

static uint32_t game_audio_float_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static float game_audio_bits_float(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static uint32_t game_audio_context_frame(double seconds)
{
    return (uint32_t) (int64_t) llround(seconds * GAME_AUDIO_FRAMES_PER_SECOND);
}

/* ---------------------------------------------------------------- storage */

static TilefinchGameAudioStorage *game_audio_storage_create(Budget *budget,
                                                            size_t samples)
{
    if (samples > (SIZE_MAX - sizeof(TilefinchGameAudioStorage)) / sizeof(float))
        return NULL;
    size_t bytes = samples * sizeof(float);
    TilefinchGameAudioStorage *storage = budget_calloc_category(
        budget, BUDGET_CATEGORY_JAVASCRIPT, 1,
        sizeof(TilefinchGameAudioStorage) + bytes);
    if (storage == NULL) return NULL;
    storage->budget = budget;
    storage->references = 1u;
    storage->bytes = bytes;
    return storage;
}

void tilefinch_game_audio_storage_release(TilefinchGameAudioStorage *storage)
{
    if (storage == NULL || storage->references == 0u) return;
    if (--storage->references == 0u) budget_free(storage->budget, storage);
}

/* -------------------------------------------------------------- lifetimes */

/* A retired object found unreferenced may go now when no block is running,
   or once the block that was running when first seen has finished. */
static bool game_audio_quiescent(TilefinchGameAudio *audio, uint32_t *free_epoch)
{
    uint32_t epoch = atomic_load_explicit(&audio->mix_epoch, memory_order_seq_cst);
    if ((epoch & 1u) == 0u) return true;
    if (*free_epoch == UINT32_MAX) {
        *free_epoch = epoch;
        return false;
    }
    return epoch != *free_epoch;
}

static bool game_audio_voice_busy(const GameAudioVoice *voice)
{
    uint32_t state = atomic_load_explicit(&voice->active, memory_order_acquire);
    return state >= 1u && state <= 4u;
}

static bool game_audio_lane_referenced(TilefinchGameAudio *audio, size_t slot)
{
    for (size_t i = 0; i < TILEFINCH_GAME_AUDIO_VOICE_LIMIT; i++) {
        GameAudioVoice *voice = &audio->voices[i];
        if (!game_audio_voice_busy(voice)) continue;
        uint32_t generation = atomic_load_explicit(&voice->program_generation,
                                                   memory_order_relaxed);
        if (atomic_load_explicit(&voice->program_ack, memory_order_acquire)
            == generation)
            voice->program_lane_mask = voice->program_lane_current;
        if ((voice->program_lane_mask >> slot) & 1u) return true;
    }
    return false;
}

static bool game_audio_buffer_in_use(TilefinchGameAudio *audio, size_t slot)
{
    for (size_t i = 0; i < TILEFINCH_GAME_AUDIO_VOICE_LIMIT; i++) {
        GameAudioVoice *voice = &audio->voices[i];
        if (voice->kind == 0u && voice->buffer_index == slot
            && game_audio_voice_busy(voice)) return true;
    }
    return false;
}

static void game_audio_buffer_clear(TilefinchGameAudio *audio, GameAudioBuffer *buffer)
{
    budget_free(audio->budget, buffer->samples);
    tilefinch_game_audio_storage_release(buffer->storage);
    audio->pcm_bytes -= buffer->bytes;
    buffer->samples = NULL;
    buffer->planes = NULL;
    buffer->storage = NULL;
    buffer->bytes = 0;
    buffer->frames = 0;
    buffer->retired = false;
    buffer->free_epoch = UINT32_MAX;
}

/* Browser-thread reclamation, at every entry that may allocate or release. */
static void game_audio_collect(TilefinchGameAudio *audio)
{
    for (size_t slot = 0; slot < TILEFINCH_GAME_AUDIO_LANE_LIMIT; slot++) {
        GameAudioLane *lane = audio->lanes[slot];
        if (lane == NULL || !lane->retired) continue;
        if (game_audio_lane_referenced(audio, slot)) {
            lane->free_epoch = UINT32_MAX;
            continue;
        }
        if (!game_audio_quiescent(audio, &lane->free_epoch)) continue;
        audio->lanes[slot] = NULL;
        budget_free(audio->budget, lane);
    }
    for (size_t slot = 0; slot < TILEFINCH_GAME_AUDIO_ANALYSER_LIMIT; slot++) {
        GameAudioAnalyser *analyser = &audio->analysers[slot];
        if (analyser->history == NULL || !analyser->retired) continue;
        /* Program masks name analysers above the sixteen lane bits. */
        if (game_audio_lane_referenced(audio, TILEFINCH_GAME_AUDIO_LANE_LIMIT + slot)) {
            analyser->free_epoch = UINT32_MAX;
            continue;
        }
        if (!game_audio_quiescent(audio, &analyser->free_epoch)) continue;
        budget_free(audio->budget, analyser->history);
        analyser->history = NULL;
        analyser->retired = false;
    }
    for (size_t slot = 0; slot < TILEFINCH_GAME_AUDIO_BUFFER_LIMIT; slot++) {
        GameAudioBuffer *buffer = &audio->buffers[slot];
        bool stale_samples = buffer->samples != NULL && buffer->planes != NULL;
        if (!buffer->retired && !stale_samples) continue;
        if (game_audio_buffer_in_use(audio, slot)) {
            buffer->free_epoch = UINT32_MAX;
            continue;
        }
        if (!game_audio_quiescent(audio, &buffer->free_epoch)) continue;
        if (buffer->retired) {
            game_audio_buffer_clear(audio, buffer);
            continue;
        }
        /* Promoted to float: the integer copy served voices started earlier. */
        size_t bytes = (size_t) buffer->frames * buffer->channels * sizeof(int16_t);
        budget_free(audio->budget, buffer->samples);
        buffer->samples = NULL;
        buffer->bytes -= bytes;
        audio->pcm_bytes -= bytes;
        buffer->free_epoch = UINT32_MAX;
    }
}

/* ------------------------------------------------------------------ lanes */

static GameAudioLane *game_audio_lane(TilefinchGameAudio *audio, uint32_t handle,
                                      size_t *index)
{
    size_t slot = (size_t) (handle & 0x1fu);
    uint32_t generation = handle >> 5;
    if (audio == NULL || slot == 0 || slot > TILEFINCH_GAME_AUDIO_LANE_LIMIT)
        return NULL;
    GameAudioLane *lane = audio->lanes[slot - 1u];
    if (lane == NULL || lane->retired || generation == 0
        || audio->lane_generations[slot - 1u] != generation) return NULL;
    if (index != NULL) *index = slot - 1u;
    return lane;
}

/* Compile the timeline relative to now: segments wholly in the past go,
   and the one running now is restated from now (exact for holds, linear,
   exponential and target segments, whose remainders are the same curves).
   Curves keep their start, at most ten seconds back. */
static void game_audio_lane_publish(GameAudioLane *lane, double now)
{
    TilefinchAudioSegment segments[TILEFINCH_AUDIO_TIMELINE_SEGMENT_LIMIT];
    size_t count = tilefinch_audio_timeline_segments(&lane->timeline, segments);
    const TilefinchAudioTimeline *timeline = &lane->timeline;
    GameAudioLaneSegment out[GAME_AUDIO_LANE_SEGMENTS];
    size_t written = 0;
    for (size_t at = 0; at < count && written < GAME_AUDIO_LANE_SEGMENTS; at++) {
        TilefinchAudioSegment s = segments[at];
        double next = at + 1u < count ? segments[at + 1u].start : INFINITY;
        if (next <= now && at + 1u < count) continue;
        if (s.start < now && s.kind != TILEFINCH_AUDIO_SEGMENT_CURVE) {
            float value = (float) tilefinch_audio_segment_value(
                &s, timeline->curve, timeline->curve_count, now);
            s.v0 = value;
            s.start = written == 0u && s.kind == TILEFINCH_AUDIO_SEGMENT_HOLD
                ? -INFINITY : now;
        }
        GameAudioLaneSegment g = {0};
        double offset = (s.start - now) * GAME_AUDIO_FRAMES_PER_SECOND;
        g.start = !isfinite(s.start) || offset < -1073741824.0 ? INT32_MIN
            : (int32_t) llround(offset);
        g.kind = s.kind;
        double length = (s.end - s.start) * GAME_AUDIO_FRAMES_PER_SECOND;
        switch (s.kind) {
        case TILEFINCH_AUDIO_SEGMENT_LINEAR:
            if (!(length > 0)) { g.kind = TILEFINCH_AUDIO_SEGMENT_HOLD; g.a = s.v1; break; }
            g.a = s.v0;
            g.b = (float) (((double) s.v1 - s.v0) / length);
            break;
        case TILEFINCH_AUDIO_SEGMENT_EXPONENTIAL:
            if (!(length > 0)) { g.kind = TILEFINCH_AUDIO_SEGMENT_HOLD; g.a = s.v1; break; }
            g.a = s.v0;
            g.b = (float) (log((double) s.v1 / s.v0) / length);
            break;
        case TILEFINCH_AUDIO_SEGMENT_TARGET: {
            g.a = s.v1;
            g.b = s.v0 - s.v1;
            /* A time constant far below a frame still starts at v0 and
               reaches the target by the next frame, as the timeline's own
               value does: a per-frame rate beyond float range would give
               expf(-inf * 0), NaN, at the segment's first frame. */
            double rate = -1.0 / (s.tau * GAME_AUDIO_FRAMES_PER_SECOND);
            g.c = rate >= -FLT_MAX ? (float) rate : -FLT_MAX;
            break;
        }
        case TILEFINCH_AUDIO_SEGMENT_CURVE: {
            double duration = timeline->curve_duration * GAME_AUDIO_FRAMES_PER_SECOND;
            g.a = (float) ((double) (timeline->curve_count - 1u) / duration);
            g.b = (float) (timeline->curve_count - 1u);
            break;
        }
        default:
            g.kind = TILEFINCH_AUDIO_SEGMENT_HOLD;
            g.a = s.v0;
            break;
        }
        out[written++] = g;
    }
    if (written == 0u)
        out[written++] = (GameAudioLaneSegment) {.start = INT32_MIN,
            .kind = TILEFINCH_AUDIO_SEGMENT_HOLD, .a = timeline->intrinsic};
    /* The first segment runs from the start: a hold, or the curve or ramp
       running now (whose start is at or before now). */
    if (out[0].kind == TILEFINCH_AUDIO_SEGMENT_HOLD) out[0].start = INT32_MIN;
    lane->base_time = now;
    uint32_t sequence = atomic_load_explicit(&lane->sequence, memory_order_relaxed);
    atomic_store_explicit(&lane->sequence, sequence + 1u, memory_order_seq_cst);
    atomic_store_explicit(&lane->shared_base, game_audio_context_frame(now),
                          memory_order_relaxed);
    atomic_store_explicit(&lane->shared_count, (uint32_t) written, memory_order_relaxed);
    for (size_t at = 0; at < written; at++) {
        _Atomic uint32_t *w = &lane->shared_words[at * GAME_AUDIO_SEGMENT_WORDS];
        atomic_store_explicit(&w[0], (uint32_t) out[at].start, memory_order_relaxed);
        atomic_store_explicit(&w[1], out[at].kind, memory_order_relaxed);
        atomic_store_explicit(&w[2], game_audio_float_bits(out[at].a), memory_order_relaxed);
        atomic_store_explicit(&w[3], game_audio_float_bits(out[at].b), memory_order_relaxed);
        atomic_store_explicit(&w[4], game_audio_float_bits(out[at].c), memory_order_relaxed);
    }
    atomic_store_explicit(&lane->shared_curve_count, timeline->curve_count,
                          memory_order_relaxed);
    for (size_t at = 0; at < timeline->curve_count; at++)
        atomic_store_explicit(&lane->shared_curve[at],
                              game_audio_float_bits(timeline->curve[at]),
                              memory_order_relaxed);
    atomic_store_explicit(&lane->sequence, sequence + 2u, memory_order_release);
}

bool tilefinch_game_audio_lane_create(TilefinchGameAudio *audio,
                                      double intrinsic, double minimum,
                                      double maximum, uint32_t *handle)
{
    if (audio == NULL || handle == NULL || !isfinite(intrinsic)
        || !isfinite(minimum) || !isfinite(maximum) || minimum > maximum)
        return false;
    game_audio_collect(audio);
    size_t slot = TILEFINCH_GAME_AUDIO_LANE_LIMIT;
    for (size_t at = 0; at < TILEFINCH_GAME_AUDIO_LANE_LIMIT; at++)
        if (audio->lanes[at] == NULL) { slot = at; break; }
    if (slot == TILEFINCH_GAME_AUDIO_LANE_LIMIT) return false;
    GameAudioLane *lane = budget_calloc_category(
        audio->budget, BUDGET_CATEGORY_JAVASCRIPT, 1, sizeof(*lane));
    if (lane == NULL) return false;
    tilefinch_audio_timeline_init(&lane->timeline, (float) intrinsic,
                                  (float) minimum, (float) maximum);
    lane->minimum = (float) minimum;
    lane->maximum = (float) maximum;
    lane->free_epoch = UINT32_MAX;
    atomic_init(&lane->sequence, 0u);
    atomic_init(&lane->shared_base, 0u);
    atomic_init(&lane->shared_count, 0u);
    atomic_init(&lane->shared_curve_count, 0u);
    for (size_t at = 0; at < GAME_AUDIO_LANE_SEGMENTS * GAME_AUDIO_SEGMENT_WORDS; at++)
        atomic_init(&lane->shared_words[at], 0u);
    for (size_t at = 0; at < TILEFINCH_AUDIO_TIMELINE_CURVE_LIMIT; at++)
        atomic_init(&lane->shared_curve[at], 0u);
    game_audio_lane_publish(lane, 0.0);
    /* The mixer copy starts complete: one held intrinsic value. */
    lane->count = 1u;
    lane->segments[0] = (GameAudioLaneSegment) {.start = INT32_MIN,
        .kind = TILEFINCH_AUDIO_SEGMENT_HOLD, .a = (float) intrinsic};
    lane->mixer_sequence = 0u;
    lane->mixer_block = UINT32_MAX;
    uint32_t generation = audio->lane_generations[slot] + 1u;
    if (generation == 0u || generation > (UINT32_MAX >> 5)) generation = 1u;
    audio->lane_generations[slot] = generation;
    lane->handle = (generation << 5) | (uint32_t) (slot + 1u);
    audio->lanes[slot] = lane;
    *handle = lane->handle;
    return true;
}

void tilefinch_game_audio_lane_release(TilefinchGameAudio *audio, uint32_t handle)
{
    GameAudioLane *lane = game_audio_lane(audio, handle, NULL);
    if (lane == NULL) return;
    lane->retired = true;
    lane->free_epoch = UINT32_MAX;
    game_audio_collect(audio);
}

TilefinchAudioTimelineStatus tilefinch_game_audio_lane_insert(
    TilefinchGameAudio *audio, uint32_t handle, double now,
    TilefinchAudioEventKind kind, double value, double time, double aux,
    const float *curve, size_t count)
{
    GameAudioLane *lane = game_audio_lane(audio, handle, NULL);
    if (lane == NULL || !isfinite(now)) return TILEFINCH_AUDIO_TIMELINE_INVALID_STATE;
    TilefinchAudioTimelineStatus status = tilefinch_audio_timeline_insert(
        &lane->timeline, now, kind, value, time, aux, curve, count);
    if (status == TILEFINCH_AUDIO_TIMELINE_OK) game_audio_lane_publish(lane, now);
    return status;
}

TilefinchAudioTimelineStatus tilefinch_game_audio_lane_cancel(
    TilefinchGameAudio *audio, uint32_t handle, double now, double time, bool hold)
{
    GameAudioLane *lane = game_audio_lane(audio, handle, NULL);
    if (lane == NULL || !isfinite(now)) return TILEFINCH_AUDIO_TIMELINE_INVALID_STATE;
    TilefinchAudioTimelineStatus status = tilefinch_audio_timeline_cancel(
        &lane->timeline, now, time, hold);
    if (status == TILEFINCH_AUDIO_TIMELINE_OK) game_audio_lane_publish(lane, now);
    return status;
}

TilefinchAudioTimelineStatus tilefinch_game_audio_lane_set_value(
    TilefinchGameAudio *audio, uint32_t handle, double now, double value)
{
    GameAudioLane *lane = game_audio_lane(audio, handle, NULL);
    if (lane == NULL || !isfinite(now)) return TILEFINCH_AUDIO_TIMELINE_INVALID_STATE;
    TilefinchAudioTimelineStatus status = tilefinch_audio_timeline_set_value(
        &lane->timeline, now, value);
    if (status == TILEFINCH_AUDIO_TIMELINE_OK) game_audio_lane_publish(lane, now);
    return status;
}

bool tilefinch_game_audio_lane_settled(TilefinchGameAudio *audio, uint32_t handle,
                                       double now, double *value, double *anchor,
                                       bool *anchored)
{
    GameAudioLane *lane = game_audio_lane(audio, handle, NULL);
    float held = 0.0f;
    if (lane == NULL || value == NULL || anchor == NULL || anchored == NULL
        || !tilefinch_audio_timeline_settled(&lane->timeline, now, &held, anchor,
                                             anchored)) return false;
    *value = held;
    return true;
}

bool tilefinch_game_audio_lane_anchor(TilefinchGameAudio *audio, uint32_t handle,
                                      double time, double value)
{
    GameAudioLane *lane = game_audio_lane(audio, handle, NULL);
    /* The held value is the lane's intrinsic one, so the mixer's copy
       (one hold) is already current. */
    return lane != NULL && tilefinch_audio_timeline_anchor(&lane->timeline, time,
                                                           (float) value);
}

bool tilefinch_game_audio_lane_value(TilefinchGameAudio *audio, uint32_t handle,
                                     double time, double *value)
{
    GameAudioLane *lane = game_audio_lane(audio, handle, NULL);
    if (lane == NULL || value == NULL || !isfinite(time)) return false;
    *value = tilefinch_audio_timeline_value(&lane->timeline, time);
    return true;
}

/* Mixer: take a new complete publication, else keep the last one. */
static void game_audio_lane_sync(GameAudioLane *lane, uint32_t block)
{
    if (lane->mixer_block == block) return;
    lane->mixer_block = block;
    uint32_t sequence = atomic_load_explicit(&lane->sequence, memory_order_acquire);
    if ((sequence & 1u) != 0u || sequence == lane->mixer_sequence) return;
    uint32_t count = atomic_load_explicit(&lane->shared_count, memory_order_relaxed);
    uint32_t curve_count = atomic_load_explicit(&lane->shared_curve_count,
                                                memory_order_relaxed);
    if (count == 0u || count > GAME_AUDIO_LANE_SEGMENTS
        || curve_count > TILEFINCH_AUDIO_TIMELINE_CURVE_LIMIT) return;
    GameAudioLaneSegment segments[GAME_AUDIO_LANE_SEGMENTS];
    float curve[TILEFINCH_AUDIO_TIMELINE_CURVE_LIMIT];
    uint32_t base = atomic_load_explicit(&lane->shared_base, memory_order_relaxed);
    for (uint32_t at = 0; at < count; at++) {
        _Atomic uint32_t *w = &lane->shared_words[at * GAME_AUDIO_SEGMENT_WORDS];
        segments[at].start = (int32_t) atomic_load_explicit(&w[0], memory_order_relaxed);
        segments[at].kind = atomic_load_explicit(&w[1], memory_order_relaxed);
        segments[at].a = game_audio_bits_float(atomic_load_explicit(&w[2], memory_order_relaxed));
        segments[at].b = game_audio_bits_float(atomic_load_explicit(&w[3], memory_order_relaxed));
        segments[at].c = game_audio_bits_float(atomic_load_explicit(&w[4], memory_order_relaxed));
    }
    for (uint32_t at = 0; at < curve_count; at++)
        curve[at] = game_audio_bits_float(
            atomic_load_explicit(&lane->shared_curve[at], memory_order_relaxed));
    atomic_thread_fence(memory_order_acquire);
    if (atomic_load_explicit(&lane->sequence, memory_order_relaxed) != sequence) return;
    memcpy(lane->segments, segments, count * sizeof(segments[0]));
    memcpy(lane->curve, curve, curve_count * sizeof(curve[0]));
    lane->count = count;
    lane->curve_count = curve_count;
    lane->base = base;
    lane->mixer_sequence = sequence;
}

/* Values for n frames from rel (frames after the lane base), clamped to the
   parameter's nominal range. Exponential and target runs step by one
   per-frame ratio from a value computed at each run's start. */
static void game_audio_lane_render(const GameAudioLane *lane, int32_t rel,
                                   size_t n, float *out)
{
    size_t index = 0;
    while (index + 1u < lane->count && lane->segments[index + 1u].start <= rel) index++;
    size_t position = 0;
    for (size_t guard = 0; position < n && guard < n + GAME_AUDIO_LANE_SEGMENTS; guard++) {
        const GameAudioLaneSegment *s = &lane->segments[index];
        int64_t frame = (int64_t) rel + (int64_t) position;
        int64_t limit = (int64_t) (n - position);
        if (index + 1u < lane->count) {
            int64_t until = (int64_t) lane->segments[index + 1u].start - frame;
            if (until <= 0) { index++; continue; }
            if (until < limit) limit = until;
        }
        size_t run = (size_t) limit;
        float *at = out + position;
        float elapsed = s->start == INT32_MIN || frame < s->start
            ? 0.0f : (float) (frame - s->start);
        switch (s->kind) {
        case TILEFINCH_AUDIO_SEGMENT_LINEAR:
            for (size_t i = 0; i < run; i++)
                at[i] = s->a + s->b * (elapsed + (float) i);
            break;
        case TILEFINCH_AUDIO_SEGMENT_EXPONENTIAL: {
            float value = s->a * expf(s->b * elapsed), ratio = expf(s->b);
            for (size_t i = 0; i < run; i++) { at[i] = value; value *= ratio; }
            break;
        }
        case TILEFINCH_AUDIO_SEGMENT_TARGET: {
            float delta = s->b * expf(s->c * elapsed), ratio = expf(s->c);
            for (size_t i = 0; i < run; i++) { at[i] = s->a + delta; delta *= ratio; }
            break;
        }
        case TILEFINCH_AUDIO_SEGMENT_CURVE: {
            uint32_t last = lane->curve_count;
            for (size_t i = 0; i < run; i++) {
                float x = (elapsed + (float) i) * s->a;
                if (last < 2u) { at[i] = last ? lane->curve[0] : 0.0f; continue; }
                if (!(x > 0.0f)) x = 0.0f;
                if (x >= s->b) { at[i] = lane->curve[last - 1u]; continue; }
                uint32_t k = (uint32_t) x;
                float from = lane->curve[k];
                at[i] = from + (lane->curve[k + 1u] - from) * (x - (float) k);
            }
            break;
        }
        default:
            for (size_t i = 0; i < run; i++) at[i] = s->a;
            break;
        }
        position += run;
        if (position < n && index + 1u < lane->count) index++;
    }
    for (; position < n; position++) out[position] = lane->segments[lane->count - 1u].a;
    for (size_t i = 0; i < n; i++) {
        float v = out[i];
        if (!(v >= lane->minimum)) v = lane->minimum;
        if (v > lane->maximum) v = lane->maximum;
        out[i] = v;
    }
}

static float game_audio_lane_at(const GameAudioLane *lane, int32_t rel)
{
    float value;
    game_audio_lane_render(lane, rel, 1u, &value);
    return value;
}

/* --------------------------------------------------------------- programs */

/* Word layout: 0 gain left, 1 gain right, 2 pitch, 3 detune (floats);
   4 gain lane slots (8 bits each, slot + 1); 5 pitch lane | detune lane
   << 8 | pre-filter mask << 16 | filter type << 24; 6 filter lane slots;
   7-10 filter values; 11 tap gain (float); 12 analyser slot + 1 | tap
   lanes << 8 | tap after filter << 12 | muted << 13 | source silent << 14. */
static GameAudioAnalyser *game_audio_analyser(TilefinchGameAudio *audio, uint32_t handle,
                                              size_t *index)
{
    size_t slot = (size_t) (handle & 0xfu);
    if (audio == NULL || slot == 0 || slot > TILEFINCH_GAME_AUDIO_ANALYSER_LIMIT)
        return NULL;
    GameAudioAnalyser *analyser = &audio->analysers[slot - 1u];
    if (analyser->history == NULL || analyser->retired
        || analyser->generation != (handle >> 4)) return NULL;
    if (index != NULL) *index = slot - 1u;
    return analyser;
}

static bool game_audio_lane_slot(TilefinchGameAudio *audio, uint32_t handle,
                                 uint32_t *slot, uint32_t *mask)
{
    if (handle == 0u) { *slot = 0u; return true; }
    size_t index = 0;
    if (game_audio_lane(audio, handle, &index) == NULL) return false;
    *slot = (uint32_t) index + 1u;
    *mask |= 1u << index;
    return true;
}

static bool game_audio_program_encode(TilefinchGameAudio *audio,
                                      const TilefinchGameAudioProgram *p,
                                      uint32_t words[GAME_AUDIO_PROGRAM_WORDS],
                                      uint32_t *mask)
{
    if (!isfinite(p->gain_left) || !isfinite(p->gain_right) || !isfinite(p->pitch)
        || !isfinite(p->detune) || !isfinite(p->tap_gain)
        || p->filter > TILEFINCH_GAME_AUDIO_FILTER_ALLPASS)
        return false;
    uint32_t slots[4] = {0}, filter_slots[4] = {0}, pitch = 0, detune = 0;
    *mask = 0u;
    for (size_t at = 0; at < TILEFINCH_GAME_AUDIO_PROGRAM_GAIN_LANES; at++)
        if (!game_audio_lane_slot(audio, p->gain_lanes[at], &slots[at], mask)) return false;
    if (!game_audio_lane_slot(audio, p->pitch_lane, &pitch, mask)
        || !game_audio_lane_slot(audio, p->detune_lane, &detune, mask)) return false;
    for (size_t at = 0; at < 4u; at++) {
        if (!isfinite(p->filter_values[at])) return false;
        if (p->filter != TILEFINCH_GAME_AUDIO_FILTER_NONE
            && !game_audio_lane_slot(audio, p->filter_lanes[at], &filter_slots[at], mask))
            return false;
    }
    words[0] = game_audio_float_bits((float) p->gain_left);
    words[1] = game_audio_float_bits((float) p->gain_right);
    words[2] = game_audio_float_bits((float) p->pitch);
    words[3] = game_audio_float_bits((float) p->detune);
    words[4] = slots[0] | (slots[1] << 8) | (slots[2] << 16) | (slots[3] << 24);
    words[5] = pitch | (detune << 8) | ((uint32_t) (p->pre_filter & 15u) << 16)
        | ((uint32_t) p->filter << 24);
    words[6] = filter_slots[0] | (filter_slots[1] << 8) | (filter_slots[2] << 16)
        | (filter_slots[3] << 24);
    for (size_t at = 0; at < 4u; at++)
        words[7u + at] = game_audio_float_bits((float) p->filter_values[at]);
    uint32_t analyser = 0;
    if (p->analyser != 0u) {
        size_t index = 0;
        if (game_audio_analyser(audio, p->analyser, &index) == NULL) return false;
        analyser = (uint32_t) index + 1u;
        *mask |= 1u << (TILEFINCH_GAME_AUDIO_LANE_LIMIT + index);
    }
    words[11] = game_audio_float_bits((float) p->tap_gain);
    words[12] = analyser | ((uint32_t) (p->tap_lanes & 15u) << 8)
        | (p->tap_after_filter ? 1u << 12 : 0u) | (p->muted ? 1u << 13 : 0u)
        | (p->source_silent ? 1u << 14 : 0u);
    return true;
}

static void game_audio_program_store(GameAudioVoice *voice,
                                     const uint32_t words[GAME_AUDIO_PROGRAM_WORDS],
                                     uint32_t mask)
{
    uint32_t generation = atomic_load_explicit(&voice->program_generation,
                                               memory_order_relaxed);
    atomic_store_explicit(&voice->program_generation, generation + 1u, memory_order_seq_cst);
    for (size_t at = 0; at < GAME_AUDIO_PROGRAM_WORDS; at++)
        atomic_store_explicit(&voice->program_words[at], words[at], memory_order_relaxed);
    atomic_store_explicit(&voice->program_generation, generation + 2u, memory_order_release);
    /* Until the mixer acknowledges this generation it may still use lanes
       the previous program named. */
    voice->program_lane_mask |= mask;
    voice->program_lane_current = mask;
}

/* Mixer: decode a new complete program. False when this voice has no
   complete program yet (it then waits a block). */
static bool game_audio_program_sync(GameAudioVoice *voice)
{
    uint32_t generation = atomic_load_explicit(&voice->program_generation,
                                               memory_order_acquire);
    GameAudioProgramState *state = &voice->mixer_program;
    if ((generation & 1u) != 0u || generation == state->generation)
        return state->generation != 0u;
    uint32_t words[GAME_AUDIO_PROGRAM_WORDS];
    for (size_t at = 0; at < GAME_AUDIO_PROGRAM_WORDS; at++)
        words[at] = atomic_load_explicit(&voice->program_words[at], memory_order_relaxed);
    atomic_thread_fence(memory_order_acquire);
    if (atomic_load_explicit(&voice->program_generation, memory_order_relaxed) != generation)
        return state->generation != 0u;
    GameAudioProgramState next = {0};
    next.generation = generation;
    next.gain_left = game_audio_bits_float(words[0]);
    next.gain_right = game_audio_bits_float(words[1]);
    next.pitch = game_audio_bits_float(words[2]);
    next.detune_factor = exp2f(game_audio_bits_float(words[3]) / 1200.0f);
    next.detune = game_audio_bits_float(words[3]);
    for (size_t at = 0; at < 4u; at++) {
        next.gain_lanes[at] = (uint8_t) (words[4] >> (8u * at));
        next.filter_lanes[at] = (uint8_t) (words[6] >> (8u * at));
        next.filter_values[at] = game_audio_bits_float(words[7u + at]);
    }
    next.pitch_lane = (uint8_t) words[5];
    next.detune_lane = (uint8_t) (words[5] >> 8);
    next.pre_filter = (uint8_t) ((words[5] >> 16) & 15u);
    next.filter = (uint8_t) (words[5] >> 24);
    next.tap_gain = game_audio_bits_float(words[11]);
    next.analyser = (uint8_t) (words[12] & 0xffu);
    next.tap_lanes = (uint8_t) ((words[12] >> 8) & 15u);
    next.tap_after_filter = ((words[12] >> 12) & 1u) != 0u;
    next.muted = ((words[12] >> 13) & 1u) != 0u;
    next.source_silent = ((words[12] >> 14) & 1u) != 0u;
    if (next.filter != state->filter) memset(&voice->filter_state, 0, sizeof(voice->filter_state));
    *state = next;
    atomic_store_explicit(&voice->program_ack, generation, memory_order_release);
    return true;
}

/* ---------------------------------------------------------------- filters */

/* Audio EQ Cookbook coefficients as the Web Audio specification states
   them (Q in dB for lowpass and highpass, linear for the others; shelf
   slope 1), at the computed frequency f0 = frequency * 2^(detune / 1200)
   clamped to [0, Nyquist]. Where those formulas divide by zero (f0 at 0
   or the Nyquist frequency, a linear Q of 0 or below, or an alpha too
   large for single precision) the filter is the formulas' limit, a
   constant gain, as browsers make it. Between 0 and 10 Hz a
   single-precision biquad cannot place its cutoff (cos w0 rounds to 1, and
   the lowpass stops passing DC), so the cutoff is 10 Hz there. */
static void game_audio_filter_gain(float k, float out[5])
{
    out[0] = k;
    out[1] = out[2] = out[3] = out[4] = 0.0f;
}

void tilefinch_game_audio_filter_coefficients(uint8_t type, float frequency,
                                              float detune, float q, float gain,
                                              float out[5])
{
    const float nyquist = 22050.0f;
    float f0 = frequency * exp2f(detune / 1200.0f);
    if (!(f0 > 0.0f)) f0 = 0.0f;
    if (f0 > nyquist) f0 = nyquist;
    float a = powf(10.0f, gain / 40.0f);
    bool linear_q = type == TILEFINCH_GAME_AUDIO_FILTER_BANDPASS
        || type == TILEFINCH_GAME_AUDIO_FILTER_PEAKING
        || type == TILEFINCH_GAME_AUDIO_FILTER_NOTCH
        || type == TILEFINCH_GAME_AUDIO_FILTER_ALLPASS;
    /* The limit as alpha grows without bound (Q -> 0 for a linear Q). */
    float wide = type == TILEFINCH_GAME_AUDIO_FILTER_BANDPASS ? 1.0f
        : type == TILEFINCH_GAME_AUDIO_FILTER_PEAKING ? a * a
        : type == TILEFINCH_GAME_AUDIO_FILTER_ALLPASS ? -1.0f : 0.0f;
    if (f0 == 0.0f || f0 == nyquist) {
        bool low = f0 == 0.0f;
        switch (type) {
        case TILEFINCH_GAME_AUDIO_FILTER_LOWPASS: game_audio_filter_gain(low ? 0 : 1, out); return;
        case TILEFINCH_GAME_AUDIO_FILTER_HIGHPASS: game_audio_filter_gain(low ? 1 : 0, out); return;
        case TILEFINCH_GAME_AUDIO_FILTER_BANDPASS: game_audio_filter_gain(0, out); return;
        case TILEFINCH_GAME_AUDIO_FILTER_LOWSHELF: game_audio_filter_gain(low ? 1 : a * a, out); return;
        case TILEFINCH_GAME_AUDIO_FILTER_HIGHSHELF: game_audio_filter_gain(low ? a * a : 1, out); return;
        default: game_audio_filter_gain(1, out); return;
        }
    }
    if (linear_q && !(q > 0.0f)) {
        game_audio_filter_gain(wide, out);
        return;
    }
    if (f0 < 10.0f) f0 = 10.0f;
    float w0 = 2.0f * 3.14159265f * f0 / 44100.0f;
    float cw = cosf(w0), sw = sinf(w0);
    float alpha_q = sw / (2.0f * q);
    float alpha_db = sw * 0.5f * powf(10.0f, -q / 20.0f);
    float alpha_s = sw / 2.0f * 1.41421356f;
    float root = 2.0f * sqrtf(a) * alpha_s;
    float alpha = linear_q ? alpha_q : alpha_db;
    if (!isfinite(alpha) && type != TILEFINCH_GAME_AUDIO_FILTER_LOWSHELF
        && type != TILEFINCH_GAME_AUDIO_FILTER_HIGHSHELF) {
        game_audio_filter_gain(wide, out);
        return;
    }
    float b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
    switch (type) {
    case TILEFINCH_GAME_AUDIO_FILTER_LOWPASS:
        b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = b0;
        a0 = 1 + alpha_db; a1 = -2 * cw; a2 = 1 - alpha_db;
        break;
    case TILEFINCH_GAME_AUDIO_FILTER_HIGHPASS:
        b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = b0;
        a0 = 1 + alpha_db; a1 = -2 * cw; a2 = 1 - alpha_db;
        break;
    case TILEFINCH_GAME_AUDIO_FILTER_BANDPASS:
        b0 = alpha_q; b1 = 0; b2 = -alpha_q;
        a0 = 1 + alpha_q; a1 = -2 * cw; a2 = 1 - alpha_q;
        break;
    case TILEFINCH_GAME_AUDIO_FILTER_NOTCH:
        b0 = 1; b1 = -2 * cw; b2 = 1;
        a0 = 1 + alpha_q; a1 = -2 * cw; a2 = 1 - alpha_q;
        break;
    case TILEFINCH_GAME_AUDIO_FILTER_ALLPASS:
        b0 = 1 - alpha_q; b1 = -2 * cw; b2 = 1 + alpha_q;
        a0 = 1 + alpha_q; a1 = -2 * cw; a2 = 1 - alpha_q;
        break;
    case TILEFINCH_GAME_AUDIO_FILTER_PEAKING:
        b0 = 1 + alpha_q * a; b1 = -2 * cw; b2 = 1 - alpha_q * a;
        a0 = 1 + alpha_q / a; a1 = -2 * cw; a2 = 1 - alpha_q / a;
        break;
    case TILEFINCH_GAME_AUDIO_FILTER_LOWSHELF:
        b0 = a * ((a + 1) - (a - 1) * cw + root);
        b1 = 2 * a * ((a - 1) - (a + 1) * cw);
        b2 = a * ((a + 1) - (a - 1) * cw - root);
        a0 = (a + 1) + (a - 1) * cw + root;
        a1 = -2 * ((a - 1) + (a + 1) * cw);
        a2 = (a + 1) + (a - 1) * cw - root;
        break;
    case TILEFINCH_GAME_AUDIO_FILTER_HIGHSHELF:
        b0 = a * ((a + 1) + (a - 1) * cw + root);
        b1 = -2 * a * ((a - 1) + (a + 1) * cw);
        b2 = a * ((a + 1) + (a - 1) * cw - root);
        a0 = (a + 1) - (a - 1) * cw + root;
        a1 = 2 * ((a - 1) - (a + 1) * cw);
        a2 = (a + 1) - (a - 1) * cw - root;
        break;
    default:
        break;
    }
    if (!(a0 != 0.0f) || !isfinite(a0)) {
        game_audio_filter_gain(wide, out);
        return;
    }
    out[0] = b0 / a0; out[1] = b1 / a0; out[2] = b2 / a0;
    out[3] = a1 / a0; out[4] = a2 / a0;
    for (size_t at = 0; at < 5u; at++)
        if (!isfinite(out[at])) {
            game_audio_filter_gain(wide, out);
            break;
        }
}

/* ------------------------------------------------------------ the program
   voice. Samples are normalized floats (int16 / 32768); the block's float
   sum joins the integer paths' accumulator before their one final clip. */

static const GameAudioLane *game_audio_mixer_lane(TilefinchGameAudio *audio,
                                                  uint8_t slot)
{
    if (slot == 0u || slot > TILEFINCH_GAME_AUDIO_LANE_LIMIT) return NULL;
    GameAudioLane *lane = audio->lanes[slot - 1u];
    if (lane == NULL) return NULL;
    game_audio_lane_sync(lane, audio->mix_blocks);
    return lane;
}

static int32_t game_audio_lane_rel(const GameAudioLane *lane, uint32_t context_frame)
{
    return (int32_t) (context_frame - lane->base);
}

/* Returns false when the voice completed within the block. */
static bool game_audio_program_mix_voice(
    TilefinchGameAudio *audio, GameAudioVoice *voice, uint32_t block_start,
    size_t frames, uint32_t stop_frame, float *sum)
{
    GameAudioProgramScratch *scratch = audio->scratch;
    const GameAudioProgramState *p = &voice->mixer_program;
    /* As on the integer paths, a stop reached first wins over a start. */
    size_t end = frames;
    bool alive = true;
    if (stop_frame != UINT32_MAX) {
        if (game_audio_frame_reached(block_start, stop_frame)) return false;
        uint32_t until = stop_frame - block_start;
        if (until < frames) { end = until; alive = false; }
    }
    size_t begin = 0;
    if (!game_audio_frame_reached(block_start, voice->start_output_frame)) {
        uint32_t wait = voice->start_output_frame - block_start;
        if (wait >= end) return alive;
        begin = wait;
    }
    size_t n = end - begin;
    if (n == 0u) return alive;
    uint32_t context_frame = block_start + (uint32_t) begin - voice->lane_anchor;
    const GameAudioLane *pitch_lane = game_audio_mixer_lane(audio, p->pitch_lane);
    const GameAudioLane *detune_lane = game_audio_mixer_lane(audio, p->detune_lane);
    float *left = scratch->source[0], *right = scratch->source[1];
    float *pitch = scratch->lane;
    if (pitch_lane != NULL)
        game_audio_lane_render(pitch_lane, game_audio_lane_rel(pitch_lane, context_frame),
                               n, pitch);
    float detune = p->detune_factor;
    bool stereo = false;
    size_t produced = n;
    if (voice->kind == 1u) {
        uint32_t phase = voice->oscillator_phase;
        for (size_t f = 0; f < n; f++) {
            if (detune_lane != NULL && f % TILEFINCH_GAME_AUDIO_FILTER_FRAMES == 0u)
                detune = exp2f(game_audio_lane_at(detune_lane, game_audio_lane_rel(
                    detune_lane, context_frame + (uint32_t) f)) / 1200.0f);
            float hz = (pitch_lane != NULL ? pitch[f] : p->pitch) * detune;
            if (!(hz > 0.0f)) hz = 0.0f;
            if (hz > 22000.0f) hz = 22000.0f;
            left[f] = (float) game_audio_oscillator_sample(voice->oscillator_type, phase)
                * (1.0f / 32768.0f);
            phase += (uint32_t) (int32_t) (hz * (float) (4294967296.0 / 44100.0));
        }
        voice->oscillator_phase = phase;
    } else if (voice->kind == 2u) {
        /* The microphone: mono, at the output rate. A voice starts reading
           once a prefill has gathered, and after an underrun waits for one
           again; one that fell too far behind skips to the newest. */
        const int16_t *ring = audio->stream;
        uint32_t written = atomic_load_explicit(&audio->stream_written,
                                                memory_order_acquire);
        uint32_t cursor = voice->position_frame, waiting = written - cursor;
        bool primed = voice->fraction_q16 != 0u;
        if (waiting > TILEFINCH_GAME_AUDIO_STREAM_SAMPLES
                          - TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES) {
            cursor = written - TILEFINCH_GAME_AUDIO_STREAM_PREFILL;
            waiting = TILEFINCH_GAME_AUDIO_STREAM_PREFILL;
        }
        if (waiting >= TILEFINCH_GAME_AUDIO_STREAM_PREFILL) primed = true;
        size_t take = ring == NULL || !primed ? 0u : waiting < n ? waiting : n;
        for (size_t f = 0; f < take; f++)
            left[f] = (float) ring[(cursor + (uint32_t) f)
                                   & (TILEFINCH_GAME_AUDIO_STREAM_SAMPLES - 1u)]
                * (1.0f / 32768.0f);
        for (size_t f = take; f < n; f++) left[f] = 0.0f;
        if (take < n) primed = false;
        voice->position_frame = cursor + (uint32_t) take;
        voice->fraction_q16 = primed ? 1u : 0u;
    } else {
        GameAudioBuffer *buffer = &audio->buffers[voice->buffer_index];
        const float *planes = voice->program_planes;
        const int16_t *samples = buffer->samples;
        uint32_t channels = buffer->channels, stride = buffer->frames;
        stereo = channels == 2u;
        float base = (float) buffer->sample_rate / 44100.0f;
        uint32_t position = voice->position_frame, fraction = voice->fraction_q16;
        uint32_t step = 0;
        for (size_t f = 0; f < n; f++) {
            if (f == 0u || pitch_lane != NULL
                || (detune_lane != NULL && f % TILEFINCH_GAME_AUDIO_FILTER_FRAMES == 0u)) {
                if (detune_lane != NULL && f % TILEFINCH_GAME_AUDIO_FILTER_FRAMES == 0u)
                    detune = exp2f(game_audio_lane_at(detune_lane, game_audio_lane_rel(
                        detune_lane, context_frame + (uint32_t) f)) / 1200.0f);
                float rate = base * (pitch_lane != NULL ? pitch[f] : p->pitch) * detune;
                if (!(rate > 0.0f)) rate = 0.0f;
                if (rate > 16.0f) rate = 16.0f;
                step = (uint32_t) (rate * 65536.0f);
            }
            uint32_t boundary = voice->loop ? voice->loop_end_frame : voice->end_frame;
            if (position >= boundary) {
                if (voice->loop) position = voice->loop_begin_frame;
                else { produced = f; alive = false; break; }
            }
            if (planes != NULL) {
                left[f] = planes[position];
                right[f] = stereo ? planes[stride + position] : left[f];
            } else {
                size_t at = (size_t) position * channels;
                left[f] = (float) samples[at] * (1.0f / 32768.0f);
                right[f] = stereo ? (float) samples[at + 1u] * (1.0f / 32768.0f) : left[f];
            }
            fraction += step;
            position += fraction >> 16;
            fraction &= 0xffffu;
            if (!voice->loop && position >= voice->end_frame) {
                produced = f + 1u;
                alive = false;
                break;
            }
        }
        voice->position_frame = position;
        voice->fraction_q16 = fraction;
    }
    n = produced;
    /* A silent source feeds zeros into everything after it (its stream
       cursor still moved on above). */
    if (p->source_silent) {
        memset(left, 0, n * sizeof(*left));
        if (stereo) memset(right, 0, n * sizeof(*right));
    }
    /* Automated gains in chain order: before the filter and the analyser,
       before the filter only, (the filter), after the filter but before
       the analyser, then the rest. The analyser hears the signal after
       the first group (an analyser before the filter) or the third. */
    bool filtered = p->filter != TILEFINCH_GAME_AUDIO_FILTER_NONE;
    size_t analyser = p->analyser != 0u && p->analyser <= TILEFINCH_GAME_AUDIO_ANALYSER_LIMIT
        && audio->analysers[p->analyser - 1u].history != NULL ? p->analyser : 0u;
    unsigned pre = filtered ? p->pre_filter & 15u : 0u;
    unsigned taps = analyser != 0u ? p->tap_lanes & 15u : 0u;
    unsigned groups[4] = {pre & taps, pre & ~taps & 15u, ~pre & taps & 15u,
                          ~pre & ~taps & 15u};
    int tap_group = filtered && !p->tap_after_filter ? 0 : 2;
    for (int stage = 0; stage < 4; stage++) {
        bool any = false;
        float *gain = scratch->gain;
        for (size_t at = 0; at < TILEFINCH_GAME_AUDIO_PROGRAM_GAIN_LANES; at++) {
            if (((groups[stage] >> at) & 1u) == 0u) continue;
            const GameAudioLane *lane = game_audio_mixer_lane(audio, p->gain_lanes[at]);
            if (lane == NULL) continue;
            float *target = any ? scratch->lane : gain;
            game_audio_lane_render(lane, game_audio_lane_rel(lane, context_frame), n, target);
            if (any) for (size_t f = 0; f < n; f++) gain[f] *= target[f];
            any = true;
        }
        if (any) {
            for (size_t f = 0; f < n; f++) left[f] *= gain[f];
            if (stereo) for (size_t f = 0; f < n; f++) right[f] *= gain[f];
        }
        if (stage == tap_group && analyser != 0u) {
            size_t slot = analyser - 1u;
            float *tap = scratch->tap[slot];
            if (((scratch->tapped >> slot) & 1u) == 0u) {
                memset(tap, 0, frames * sizeof(*tap));
                scratch->tapped |= 1u << slot;
            }
            /* Stereo is down-mixed as speakers are: the mean. */
            float scale = p->tap_gain * (stereo ? 0.5f : 1.0f);
            tap += begin;
            if (stereo) for (size_t f = 0; f < n; f++) tap[f] += (left[f] + right[f]) * scale;
            else for (size_t f = 0; f < n; f++) tap[f] += left[f] * scale;
        }
        if (stage != 1 || !filtered) continue;
        GameAudioFilterState *filter = &voice->filter_state;
        const GameAudioLane *lanes[4];
        for (size_t at = 0; at < 4u; at++)
            lanes[at] = game_audio_mixer_lane(audio, p->filter_lanes[at]);
        for (size_t f = 0; f < n; f += TILEFINCH_GAME_AUDIO_FILTER_FRAMES) {
            float inputs[4];
            for (size_t at = 0; at < 4u; at++)
                inputs[at] = lanes[at] != NULL ? game_audio_lane_at(lanes[at],
                    game_audio_lane_rel(lanes[at], context_frame + (uint32_t) f))
                    : p->filter_values[at];
            if (!filter->valid || memcmp(inputs, filter->inputs, sizeof(inputs)) != 0) {
                tilefinch_game_audio_filter_coefficients(p->filter, inputs[0], inputs[1],
                                               inputs[2], inputs[3], filter->coefficients);
                memcpy(filter->inputs, inputs, sizeof(inputs));
                filter->valid = true;
            }
            const float *k = filter->coefficients;
            size_t last = f + TILEFINCH_GAME_AUDIO_FILTER_FRAMES < n
                ? f + TILEFINCH_GAME_AUDIO_FILTER_FRAMES : n;
            for (size_t channel = 0; channel < (stereo ? 2u : 1u); channel++) {
                float *x = channel == 0u ? left : right;
                float z1 = filter->z[channel][0], z2 = filter->z[channel][1];
                for (size_t i = f; i < last; i++) {
                    float in = x[i], out = k[0] * in + z1;
                    z1 = k[1] * in - k[3] * out + z2;
                    z2 = k[2] * in - k[4] * out;
                    x[i] = out;
                }
                /* No denormal or runaway state survives a sub-block. */
                if (!(fabsf(z1) > 1e-20f) || !isfinite(z1)) z1 = 0.0f;
                if (!(fabsf(z2) > 1e-20f) || !isfinite(z2)) z2 = 0.0f;
                filter->z[channel][0] = z1;
                filter->z[channel][1] = z2;
            }
        }
    }
    /* A path that never reaches the destination, and the microphone while
       it would feed back through the speaker, stay out of the output. */
    if (p->muted || (voice->kind == 2u
                     && !atomic_load_explicit(&audio->stream_audible, memory_order_relaxed)))
        return alive;
    float *out = sum + begin * 2u;
    for (size_t f = 0; f < n; f++) {
        out[f * 2u] += left[f] * p->gain_left;
        out[f * 2u + 1u] += (stereo ? right[f] : left[f]) * p->gain_right;
    }
    return alive;
}

/* Render every program voice of a block. Returns the integer sum the
   integer paths start from, or NULL when no program voice sounded. */
static const int32_t *game_audio_program_mix(
    TilefinchGameAudio *audio, size_t frames, uint32_t block_start,
    const bool *program, bool *alive, bool *completed, const uint32_t *stop_frames)
{
    GameAudioProgramScratch *scratch = audio->scratch;
    bool any = false;
    for (size_t i = 0; i < TILEFINCH_GAME_AUDIO_VOICE_LIMIT; i++)
        any |= program[i] && alive[i];
    if (!any || scratch == NULL) return NULL;
    memset(scratch->sum, 0, frames * 2u * sizeof(float));
    scratch->tapped = 0u;
    for (size_t i = 0; i < TILEFINCH_GAME_AUDIO_VOICE_LIMIT; i++) {
        if (!program[i] || !alive[i]) continue;
        GameAudioVoice *voice = &audio->voices[i];
        if (!game_audio_program_sync(voice)) continue;
        if (!game_audio_program_mix_voice(audio, voice, block_start, frames,
                                          stop_frames[i], scratch->sum)) {
            alive[i] = false;
            completed[i] = true;
        }
    }
    for (size_t slot = 0; slot < TILEFINCH_GAME_AUDIO_ANALYSER_LIMIT; slot++) {
        if (((scratch->tapped >> slot) & 1u) == 0u) continue;
        GameAudioAnalyser *analyser = &audio->analysers[slot];
        uint32_t written = atomic_load_explicit(&analyser->written, memory_order_relaxed);
        for (size_t f = 0; f < frames; f++)
            analyser->history[(written + (uint32_t) f)
                              & (TILEFINCH_GAME_AUDIO_ANALYSER_HISTORY - 1u)]
                = scratch->tap[slot][f];
        atomic_store_explicit(&analyser->written, written + (uint32_t) frames,
                              memory_order_release);
    }
    for (size_t at = 0; at < frames * 2u; at++) {
        float value = scratch->sum[at] * 32768.0f;
        if (!(value > -16777216.0f)) value = value != value ? 0.0f : -16777216.0f;
        if (value > 16777216.0f) value = 16777216.0f;
        scratch->program_sum[at] = (int32_t) value;
    }
    return scratch->program_sum;
}

/* ------------------------------------------------------ program admission */

static bool game_audio_program_has_lanes(const TilefinchGameAudioProgram *p)
{
    for (size_t at = 0; at < TILEFINCH_GAME_AUDIO_PROGRAM_GAIN_LANES; at++)
        if (p->gain_lanes[at] != 0u) return true;
    return p->pitch_lane != 0u || p->detune_lane != 0u
        || p->filter != TILEFINCH_GAME_AUDIO_FILTER_NONE
        || p->analyser != 0u || p->muted || p->source_silent;
}

static double game_audio_program_rate(const TilefinchGameAudioProgram *p)
{
    return p->detune == 0 ? p->pitch : p->pitch * pow(2.0, p->detune / 1200.0);
}

static bool game_audio_scratch(TilefinchGameAudio *audio)
{
    if (audio->scratch != NULL) return true;
    audio->scratch = budget_calloc_category(audio->budget, BUDGET_CATEGORY_JAVASCRIPT, 1,
                                            sizeof(*audio->scratch));
    return audio->scratch != NULL;
}

/* Restate long-lived lanes so the mixer's 32-bit frame offsets stay small. */
static void game_audio_program_refresh_lanes(TilefinchGameAudio *audio,
                                             uint32_t mask, double now)
{
    for (size_t slot = 0; slot < TILEFINCH_GAME_AUDIO_LANE_LIMIT; slot++) {
        GameAudioLane *lane = audio->lanes[slot];
        if (((mask >> slot) & 1u) != 0u && lane != NULL
            && now - lane->base_time > 3600.0)
            game_audio_lane_publish(lane, now);
    }
}

static void game_audio_voice_program_mode(GameAudioVoice *voice)
{
    memset(&voice->filter_state, 0, sizeof(voice->filter_state));
    atomic_store_explicit(&voice->program_mode, 1u, memory_order_release);
}

bool tilefinch_game_audio_start_program(
    TilefinchGameAudio *audio, const TilefinchGameAudioVoiceStart *start,
    const TilefinchGameAudioProgram *program, uint32_t *voice_handle)
{
    if (audio == NULL || start == NULL || program == NULL || voice_handle == NULL
        || !isfinite(start->now) || !isfinite(start->when)) return false;
    game_audio_collect(audio);
    double delay = start->when - start->now;
    if (delay < 0) delay = 0;
    uint32_t words[GAME_AUDIO_PROGRAM_WORDS], mask = 0;
    if (!game_audio_program_encode(audio, program, words, &mask)) return false;
    bool lanes = game_audio_program_has_lanes(program);
    double rate = game_audio_program_rate(program);
    GameAudioBuffer *buffer = NULL;
    if (start->buffer != 0u) {
        buffer = game_audio_buffer(audio, start->buffer, NULL);
        if (buffer == NULL) return false;
    } else if (start->oscillator < TILEFINCH_GAME_AUDIO_OSCILLATOR_SINE
               || start->oscillator > TILEFINCH_GAME_AUDIO_OSCILLATOR_TRIANGLE) {
        return false;
    }
    bool simple = !lanes && game_audio_valid_gain(program->gain_left)
        && game_audio_valid_gain(program->gain_right)
        && (buffer == NULL
            ? rate >= 1.0 && rate <= 20000.0
            : buffer->samples != NULL && buffer->planes == NULL
                && rate >= 0.25 && rate <= 4.0);
    uint32_t now_frame = atomic_load_explicit(&audio->output_frame, memory_order_acquire);
    uint32_t anchor = now_frame - game_audio_context_frame(start->now);
    if (simple) {
        bool started = buffer != NULL
            ? game_audio_start_voice(audio, start->buffer, start->offset, start->duration,
                rate, program->gain_left, program->gain_right, start->loop,
                start->loop_start, start->loop_end, delay, anchor, NULL, voice_handle)
            : game_audio_start_oscillator_voice(audio,
                (TilefinchGameAudioOscillatorType) start->oscillator, rate,
                program->gain_left, program->gain_right, delay, anchor, NULL,
                voice_handle);
        return started;
    }
    if (!game_audio_scratch(audio)) return false;
    game_audio_program_refresh_lanes(audio, mask, start->now);
    GameAudioProgramStart staged = {.words = words, .mask = mask};
    if (buffer != NULL)
        return game_audio_start_voice(audio, start->buffer, start->offset, start->duration,
            1.0, 0, 0, start->loop, start->loop_start, start->loop_end, delay, anchor,
            &staged, voice_handle);
    return game_audio_start_oscillator_voice(audio,
        (TilefinchGameAudioOscillatorType) start->oscillator, 440.0, 0, 0, delay,
        anchor, &staged, voice_handle);
}

bool tilefinch_game_audio_set_program(TilefinchGameAudio *audio, uint32_t voice_handle,
                                      const TilefinchGameAudioProgram *program)
{
    GameAudioVoice *voice = game_audio_voice(audio, voice_handle);
    if (voice == NULL || program == NULL) return false;
    uint32_t state = atomic_load_explicit(&voice->active, memory_order_acquire);
    if (state == 0u || state == 5u) return false;
    game_audio_collect(audio);
    uint32_t words[GAME_AUDIO_PROGRAM_WORDS], mask = 0;
    if (!game_audio_program_encode(audio, program, words, &mask)) return false;
    double rate = game_audio_program_rate(program);
    if (atomic_load_explicit(&voice->program_mode, memory_order_acquire) == 0u) {
        bool simple = !game_audio_program_has_lanes(program)
            && game_audio_valid_gain(program->gain_left)
            && game_audio_valid_gain(program->gain_right);
        if (simple && voice->kind == 1u) {
            uint32_t step = 0;
            if (game_audio_oscillator_step(rate, &step)) {
                atomic_store_explicit(&voice->gains_q8,
                    game_audio_gains_q8(program->gain_left, program->gain_right),
                    memory_order_release);
                atomic_store_explicit(&voice->step_fraction, step, memory_order_release);
                return true;
            }
        } else if (simple && rate == voice->legacy_rate) {
            atomic_store_explicit(&voice->gains_q8,
                game_audio_gains_q8(program->gain_left, program->gain_right),
                memory_order_release);
            return true;
        }
        if (!game_audio_scratch(audio)) return false;
        game_audio_program_store(voice, words, mask);
        game_audio_voice_program_mode(voice);
        return true;
    }
    game_audio_program_store(voice, words, mask);
    return true;
}

/* ---------------------------------------------------------- float buffers */

bool tilefinch_game_audio_create_buffer(
    TilefinchGameAudio *audio, uint32_t channels, uint32_t frames,
    uint32_t sample_rate, TilefinchGameAudioBufferInfo *info)
{
    if (audio == NULL || info == NULL || channels < 1u || channels > 2u || frames == 0u
        || sample_rate < TILEFINCH_GAME_AUDIO_BUFFER_RATE_MIN
        || sample_rate > TILEFINCH_GAME_AUDIO_BUFFER_RATE_MAX) return false;
    game_audio_collect(audio);
    size_t samples = (size_t) frames * channels;
    if (samples / channels != frames
        || samples > TILEFINCH_GAME_AUDIO_PCM_BYTE_LIMIT / sizeof(float)) return false;
    size_t bytes = samples * sizeof(float);
    if (bytes > TILEFINCH_GAME_AUDIO_PCM_BYTE_LIMIT - audio->pcm_bytes) return false;
    size_t slot = TILEFINCH_GAME_AUDIO_BUFFER_LIMIT;
    for (size_t at = 0; at < TILEFINCH_GAME_AUDIO_BUFFER_LIMIT; at++)
        if (game_audio_buffer_free(&audio->buffers[at])) { slot = at; break; }
    if (slot == TILEFINCH_GAME_AUDIO_BUFFER_LIMIT) return false;
    TilefinchGameAudioStorage *storage = game_audio_storage_create(audio->budget, samples);
    if (storage == NULL) return false;
    GameAudioBuffer *buffer = &audio->buffers[slot];
    buffer->generation++;
    if (buffer->generation == 0 || buffer->generation > (UINT32_MAX >> 4))
        buffer->generation = 1;
    buffer->samples = NULL;
    buffer->storage = storage;
    buffer->planes = storage->samples;
    buffer->bytes = bytes;
    buffer->frames = frames;
    buffer->sample_rate = sample_rate;
    buffer->channels = (uint8_t) channels;
    buffer->retired = false;
    buffer->free_epoch = UINT32_MAX;
    audio->pcm_bytes += bytes;
    info->handle = (buffer->generation << 4) | (uint32_t) (slot + 1u);
    info->frames = frames;
    info->sample_rate = sample_rate;
    info->channels = (uint8_t) channels;
    return true;
}

float *tilefinch_game_audio_channel_data(
    TilefinchGameAudio *audio, uint32_t handle, uint32_t channel,
    TilefinchGameAudioStorage **storage_out)
{
    size_t slot = 0;
    GameAudioBuffer *buffer = game_audio_buffer(audio, handle, &slot);
    if (buffer == NULL || storage_out == NULL || channel >= buffer->channels) return NULL;
    game_audio_collect(audio);
    if (buffer->planes == NULL) {
        /* Promote integer PCM. A voice that is playing it keeps reading the
           integer copy, which goes once no such voice remains; otherwise the
           float copy replaces it at once. The mixer reads only the buffers
           of busy voices, so an unplayed copy can be freed immediately. */
        size_t samples = (size_t) buffer->frames * buffer->channels;
        if (samples > TILEFINCH_GAME_AUDIO_PCM_BYTE_LIMIT / sizeof(float)) return NULL;
        size_t bytes = samples * sizeof(float), integer = samples * sizeof(int16_t);
        bool busy = game_audio_buffer_in_use(audio, slot);
        size_t needed = busy ? bytes : bytes - integer;
        if (needed > TILEFINCH_GAME_AUDIO_PCM_BYTE_LIMIT - audio->pcm_bytes) return NULL;
        TilefinchGameAudioStorage *storage = game_audio_storage_create(audio->budget, samples);
        if (storage == NULL) return NULL;
        for (size_t c = 0; c < buffer->channels; c++)
            for (size_t f = 0; f < buffer->frames; f++)
                storage->samples[c * buffer->frames + f] =
                    (float) buffer->samples[f * buffer->channels + c] * (1.0f / 32768.0f);
        if (!busy) {
            budget_free(audio->budget, buffer->samples);
            buffer->samples = NULL;
            buffer->bytes -= integer;
            audio->pcm_bytes -= integer;
        }
        buffer->storage = storage;
        buffer->planes = storage->samples;
        buffer->bytes += bytes;
        audio->pcm_bytes += bytes;
        buffer->free_epoch = UINT32_MAX;
    }
    buffer->storage->references++;
    *storage_out = buffer->storage;
    return buffer->planes + (size_t) channel * buffer->frames;
}

bool tilefinch_game_audio_copy_from_channel(
    TilefinchGameAudio *audio, uint32_t handle, uint32_t channel,
    uint32_t start, float *out, size_t count)
{
    GameAudioBuffer *buffer = game_audio_buffer(audio, handle, NULL);
    if (buffer == NULL || out == NULL || channel >= buffer->channels) return false;
    if (start >= buffer->frames) return true;
    size_t available = buffer->frames - start;
    if (count > available) count = available;
    if (buffer->planes != NULL) {
        memcpy(out, buffer->planes + (size_t) channel * buffer->frames + start,
               count * sizeof(float));
        return true;
    }
    for (size_t at = 0; at < count; at++)
        out[at] = (float) buffer->samples[((size_t) start + at) * buffer->channels + channel]
            * (1.0f / 32768.0f);
    return true;
}

void tilefinch_game_audio_buffer_release(TilefinchGameAudio *audio, uint32_t handle)
{
    GameAudioBuffer *buffer = game_audio_buffer(audio, handle, NULL);
    if (buffer == NULL) return;
    buffer->retired = true;
    buffer->free_epoch = UINT32_MAX;
    game_audio_collect(audio);
}

size_t tilefinch_game_audio_pcm_bytes(const TilefinchGameAudio *audio)
{
    return audio != NULL ? audio->pcm_bytes : 0u;
}

/* ------------------------------------------------------- microphone voices */

bool tilefinch_game_audio_start_stream(
    TilefinchGameAudio *audio, double now,
    const TilefinchGameAudioProgram *program, uint32_t *voice_handle)
{
    if (audio == NULL || program == NULL || voice_handle == NULL || !isfinite(now))
        return false;
    game_audio_collect(audio);
    uint32_t words[GAME_AUDIO_PROGRAM_WORDS], mask = 0;
    if (!game_audio_program_encode(audio, program, words, &mask)) return false;
    if (!game_audio_scratch(audio)) return false;
    if (audio->stream == NULL) {
        audio->stream = budget_calloc_category(audio->budget, BUDGET_CATEGORY_JAVASCRIPT,
            TILEFINCH_GAME_AUDIO_STREAM_SAMPLES, sizeof(*audio->stream));
        if (audio->stream == NULL) return false;
    }
    GameAudioVoice *voice = game_audio_claim_voice(audio, voice_handle);
    if (voice == NULL) return false;
    uint32_t now_frame = atomic_load_explicit(&audio->output_frame, memory_order_acquire);
    uint32_t anchor = now_frame - game_audio_context_frame(now);
    game_audio_program_refresh_lanes(audio, mask, now);
    voice->kind = 2u;
    voice->oscillator_type = 0u;
    voice->start_output_frame = now_frame == UINT32_MAX ? now_frame - 1u : now_frame;
    atomic_store_explicit(&voice->stop_output_frame, UINT32_MAX, memory_order_relaxed);
    /* Reading starts with what arrives from now on, after a prefill. */
    voice->position_frame = atomic_load_explicit(&audio->stream_written,
                                                 memory_order_relaxed);
    voice->fraction_q16 = 0u;
    voice->loop = false;
    GameAudioProgramStart staged = {.words = words, .mask = mask};
    game_audio_stage_program(voice, &staged, anchor);
    game_audio_publish_voice(audio, voice);
    return true;
}

void tilefinch_game_audio_stream_write(TilefinchGameAudio *audio,
                                       const float *samples, size_t count)
{
    if (audio == NULL || audio->stream == NULL || samples == NULL) return;
    /* A burst larger than half the ring keeps its newest part. */
    if (count > TILEFINCH_GAME_AUDIO_STREAM_SAMPLES / 2u) {
        samples += count - TILEFINCH_GAME_AUDIO_STREAM_SAMPLES / 2u;
        count = TILEFINCH_GAME_AUDIO_STREAM_SAMPLES / 2u;
    }
    uint32_t written = atomic_load_explicit(&audio->stream_written, memory_order_relaxed);
    for (size_t at = 0; at < count; at++) {
        float value = samples[at] * 32768.0f;
        if (!(value > -32768.0f)) value = value != value ? 0.0f : -32768.0f;
        if (value > 32767.0f) value = 32767.0f;
        audio->stream[(written + (uint32_t) at) & (TILEFINCH_GAME_AUDIO_STREAM_SAMPLES - 1u)]
            = (int16_t) value;
    }
    atomic_store_explicit(&audio->stream_written, written + (uint32_t) count,
                          memory_order_release);
}

bool tilefinch_game_audio_stream_reset(TilefinchGameAudio *audio)
{
    if (audio == NULL || audio->stream == NULL) return true;
    for (size_t i = 0; i < TILEFINCH_GAME_AUDIO_VOICE_LIMIT; i++)
        if (audio->voices[i].kind == 2u && game_audio_voice_busy(&audio->voices[i]))
            return false;
    memset(audio->stream, 0, TILEFINCH_GAME_AUDIO_STREAM_SAMPLES * sizeof(*audio->stream));
    return true;
}

void tilefinch_game_audio_stream_audible(TilefinchGameAudio *audio, bool audible)
{
    if (audio != NULL)
        atomic_store_explicit(&audio->stream_audible, audible, memory_order_relaxed);
}

/* -------------------------------------------------------------- analysers */

bool tilefinch_game_audio_analyser_create(TilefinchGameAudio *audio, uint32_t *handle)
{
    if (audio == NULL || handle == NULL) return false;
    game_audio_collect(audio);
    for (size_t slot = 0; slot < TILEFINCH_GAME_AUDIO_ANALYSER_LIMIT; slot++) {
        GameAudioAnalyser *analyser = &audio->analysers[slot];
        if (analyser->history != NULL) continue;
        analyser->history = budget_calloc_category(audio->budget, BUDGET_CATEGORY_JAVASCRIPT,
            TILEFINCH_GAME_AUDIO_ANALYSER_HISTORY, sizeof(float));
        if (analyser->history == NULL) return false;
        if (++analyser->generation >= (UINT32_MAX >> 4)) analyser->generation = 1u;
        atomic_store_explicit(&analyser->written, 0u, memory_order_relaxed);
        analyser->cleared_at = 0u;
        analyser->free_epoch = UINT32_MAX;
        analyser->retired = false;
        *handle = (analyser->generation << 4) | (uint32_t) (slot + 1u);
        return true;
    }
    return false;
}

void tilefinch_game_audio_analyser_release(TilefinchGameAudio *audio, uint32_t handle)
{
    GameAudioAnalyser *analyser = game_audio_analyser(audio, handle, NULL);
    if (analyser == NULL) return;
    analyser->retired = true;
    analyser->free_epoch = UINT32_MAX;
    game_audio_collect(audio);
}

bool tilefinch_game_audio_analyser_read(TilefinchGameAudio *audio, uint32_t handle,
                                        float *out, size_t count, uint32_t *stamp)
{
    size_t slot = 0;
    GameAudioAnalyser *analyser = game_audio_analyser(audio, handle, &slot);
    if (analyser == NULL || out == NULL || stamp == NULL
        || count > TILEFINCH_GAME_AUDIO_FFT_SIZE_MAX) return false;
    *stamp = atomic_load_explicit(&audio->output_frame, memory_order_acquire);
    if (!game_audio_lane_referenced(audio, TILEFINCH_GAME_AUDIO_LANE_LIMIT + slot)) {
        /* Nothing feeds it, and no block can until a program names it: what
           it heard before is gone, as in a graph that stopped pulling it. */
        uint32_t written = atomic_load_explicit(&analyser->written, memory_order_relaxed);
        if (written != analyser->cleared_at) {
            memset(analyser->history, 0, TILEFINCH_GAME_AUDIO_ANALYSER_HISTORY * sizeof(float));
            analyser->cleared_at = written;
        }
        memset(out, 0, count * sizeof(*out));
        return true;
    }
    /* The mixer writes at most one block past written while this copies;
       the history holds twice the largest read. */
    uint32_t written = atomic_load_explicit(&analyser->written, memory_order_acquire);
    uint32_t from = written - (uint32_t) count;
    for (size_t at = 0; at < count; at++)
        out[at] = analyser->history[(from + (uint32_t) at)
                                    & (TILEFINCH_GAME_AUDIO_ANALYSER_HISTORY - 1u)];
    return true;
}

/* Iterative radix-2 FFT in single precision (the PSP's FPU has no double).
   Twiddles advance by rotation within a stage; each stage starts exact. */
static void game_audio_fft(float *re, float *im, size_t n)
{
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; (j & bit) != 0u; bit >>= 1) j ^= bit;
        j |= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        size_t half = len >> 1;
        float angle = -6.28318530717958647692f / (float) len;
        float step_re = cosf(angle), step_im = sinf(angle);
        float w_re = 1.0f, w_im = 0.0f;
        for (size_t k = 0; k < half; k++) {
            for (size_t i = k; i < n; i += len) {
                size_t j = i + half;
                float t_re = re[j] * w_re - im[j] * w_im;
                float t_im = re[j] * w_im + im[j] * w_re;
                re[j] = re[i] - t_re;
                im[j] = im[i] - t_im;
                re[i] += t_re;
                im[i] += t_im;
            }
            float next = w_re * step_re - w_im * step_im;
            w_im = w_re * step_im + w_im * step_re;
            w_re = next;
        }
    }
}

void tilefinch_game_audio_spectrum(float *time, size_t n, float smoothing,
                                   float *spectrum, float *scratch)
{
    if (time == NULL || spectrum == NULL || scratch == NULL || n < 32u
        || n > TILEFINCH_GAME_AUDIO_FFT_SIZE_MAX || (n & (n - 1u)) != 0u) return;
    float *re = scratch, *im = scratch + n;
    /* Blackman window (alpha 0.16): 0.42 - 0.5 cos(x) + 0.08 cos(2x). */
    float step_re = cosf(6.28318530717958647692f / (float) n);
    float step_im = sinf(6.28318530717958647692f / (float) n);
    float c = 1.0f, s = 0.0f;
    for (size_t at = 0; at < n; at++) {
        re[at] = time[at] * (0.42f - 0.5f * c + 0.08f * (2.0f * c * c - 1.0f));
        im[at] = 0.0f;
        float next = c * step_re - s * step_im;
        s = c * step_im + s * step_re;
        c = next;
    }
    game_audio_fft(re, im, n);
    float scale = 1.0f / (float) n;
    for (size_t k = 0; k < n / 2u; k++) {
        float magnitude = sqrtf(re[k] * re[k] + im[k] * im[k]) * scale;
        float value = smoothing * spectrum[k] + (1.0f - smoothing) * magnitude;
        spectrum[k] = isfinite(value) ? value : 0.0f;
    }
}
