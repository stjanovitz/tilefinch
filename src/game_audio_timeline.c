/* AudioParam automation timelines: the specification's event-list rules
   (Web Audio API, "AudioParam" and "Computation of Value"), bounded. */
#include "tilefinch/game_audio_timeline.h"

#include <math.h>
#include <string.h>

void tilefinch_audio_timeline_init(TilefinchAudioTimeline *timeline,
                                   float intrinsic, float minimum,
                                   float maximum)
{
    memset(timeline, 0, sizeof(*timeline));
    timeline->intrinsic = intrinsic;
    timeline->minimum = minimum;
    timeline->maximum = maximum;
}

static bool timeline_is_ramp(uint8_t kind)
{
    return kind == TILEFINCH_AUDIO_EVENT_LINEAR
        || kind == TILEFINCH_AUDIO_EVENT_EXPONENTIAL;
}

double tilefinch_audio_segment_value(const TilefinchAudioSegment *s,
                                     const float *curve, size_t count,
                                     double t)
{
    switch (s->kind) {
    case TILEFINCH_AUDIO_SEGMENT_LINEAR:
        if (t >= s->end || s->end <= s->start) return s->v1;
        return s->v0 + ((double) s->v1 - s->v0) * (t - s->start)
            / (s->end - s->start);
    case TILEFINCH_AUDIO_SEGMENT_EXPONENTIAL:
        if (t >= s->end || s->end <= s->start) return s->v1;
        return s->v0 * pow((double) s->v1 / s->v0,
                           (t - s->start) / (s->end - s->start));
    case TILEFINCH_AUDIO_SEGMENT_TARGET:
        return s->v1 + ((double) s->v0 - s->v1)
            * exp(-(t - s->start) / s->tau);
    case TILEFINCH_AUDIO_SEGMENT_CURVE: {
        if (count < 2u) return s->v0;
        double duration = s->end - s->start;
        if (t >= s->end || duration <= 0) return curve[count - 1u];
        double position = (t - s->start) * (double) (count - 1u) / duration;
        size_t k = position <= 0 ? 0u : (size_t) position;
        if (k >= count - 1u) return curve[count - 1u];
        return curve[k] + ((double) curve[k + 1u] - curve[k])
            * (position - (double) k);
    }
    default:
        return s->v0;
    }
}

size_t tilefinch_audio_timeline_segments(const TilefinchAudioTimeline *timeline,
                                         TilefinchAudioSegment *segments)
{
    size_t n = 0;
    segments[n++] = (TilefinchAudioSegment) {
        .start = -INFINITY, .kind = TILEFINCH_AUDIO_SEGMENT_HOLD,
        .v0 = timeline->intrinsic, .v1 = timeline->intrinsic};
    for (size_t at = 0; at < timeline->count; at++) {
        const TilefinchAudioEvent *e = &timeline->events[at];
        TilefinchAudioSegment *last = &segments[n - 1u];
        switch (e->kind) {
        case TILEFINCH_AUDIO_EVENT_SET:
            segments[n++] = (TilefinchAudioSegment) {.start = e->time,
                .kind = TILEFINCH_AUDIO_SEGMENT_HOLD, .v0 = e->value,
                .v1 = e->value};
            break;
        case TILEFINCH_AUDIO_EVENT_LINEAR:
        case TILEFINCH_AUDIO_EVENT_EXPONENTIAL: {
            /* The ramp runs from the previous event: a held value, or a
               target that had not started when the ramp arrived (the
               specification's "replaces the SetTarget event"). */
            if (isfinite(last->start)) {
                float v0 = last->v0;
                bool exponential = e->kind == TILEFINCH_AUDIO_EVENT_EXPONENTIAL;
                /* Exponential through or from zero holds V0 until T1. */
                bool hold = exponential && (v0 == 0.0f || e->value == 0.0f
                    || (v0 < 0.0f) != (e->value < 0.0f));
                last->kind = hold ? TILEFINCH_AUDIO_SEGMENT_HOLD
                    : exponential ? TILEFINCH_AUDIO_SEGMENT_EXPONENTIAL
                                  : TILEFINCH_AUDIO_SEGMENT_LINEAR;
                last->v0 = v0;
                last->v1 = hold ? v0 : e->value;
                last->end = e->time;
            }
            segments[n++] = (TilefinchAudioSegment) {.start = e->time,
                .kind = TILEFINCH_AUDIO_SEGMENT_HOLD, .v0 = e->value,
                .v1 = e->value};
            break;
        }
        case TILEFINCH_AUDIO_EVENT_TARGET: {
            float from = (float) tilefinch_audio_segment_value(
                last, timeline->curve, timeline->curve_count, e->time);
            if (e->aux <= 0)
                segments[n++] = (TilefinchAudioSegment) {.start = e->time,
                    .kind = TILEFINCH_AUDIO_SEGMENT_HOLD, .v0 = e->value,
                    .v1 = e->value};
            else
                segments[n++] = (TilefinchAudioSegment) {.start = e->time,
                    .kind = TILEFINCH_AUDIO_SEGMENT_TARGET, .v0 = from,
                    .v1 = e->value, .tau = e->aux};
            break;
        }
        case TILEFINCH_AUDIO_EVENT_CURVE: {
            TilefinchAudioSegment curve = {.start = e->time,
                .end = e->time + timeline->curve_duration,
                .kind = TILEFINCH_AUDIO_SEGMENT_CURVE,
                .v0 = timeline->curve[0],
                .v1 = timeline->curve[timeline->curve_count - 1u]};
            float held = (float) tilefinch_audio_segment_value(
                &curve, timeline->curve, timeline->curve_count,
                timeline->curve_end);
            segments[n++] = curve;
            segments[n++] = (TilefinchAudioSegment) {
                .start = timeline->curve_end,
                .kind = TILEFINCH_AUDIO_SEGMENT_HOLD, .v0 = held, .v1 = held};
            break;
        }
        default:
            break;
        }
    }
    return n;
}

static size_t timeline_active(const TilefinchAudioSegment *segments,
                              size_t count, double t)
{
    size_t active = 0;
    for (size_t at = 1; at < count && segments[at].start <= t; at++)
        active = at;
    return active;
}

/* The automation's value at t before nominal-range clamping. */
static double timeline_raw_value(const TilefinchAudioTimeline *timeline,
                                 double t)
{
    TilefinchAudioSegment segments[TILEFINCH_AUDIO_TIMELINE_SEGMENT_LIMIT];
    size_t count = tilefinch_audio_timeline_segments(timeline, segments);
    return tilefinch_audio_segment_value(
        &segments[timeline_active(segments, count, t)], timeline->curve,
        timeline->curve_count, t);
}

double tilefinch_audio_timeline_value(const TilefinchAudioTimeline *timeline,
                                      double time)
{
    double value = timeline_raw_value(timeline, time);
    if (!(value >= timeline->minimum)) value = timeline->minimum;
    if (value > timeline->maximum) value = timeline->maximum;
    return value;
}

static void timeline_remove(TilefinchAudioTimeline *timeline, size_t at)
{
    if (timeline->events[at].kind == TILEFINCH_AUDIO_EVENT_CURVE)
        timeline->curve_count = 0;
    memmove(&timeline->events[at], &timeline->events[at + 1u],
            (timeline->count - at - 1u) * sizeof(timeline->events[0]));
    timeline->count--;
}

static void timeline_place(TilefinchAudioTimeline *timeline, size_t at,
                           TilefinchAudioEvent event)
{
    memmove(&timeline->events[at + 1u], &timeline->events[at],
            (timeline->count - at) * sizeof(timeline->events[0]));
    timeline->events[at] = event;
    timeline->count++;
}

/* Fold events wholly before now into at most two equivalent events, so a
   long-running page does not exhaust the bound. Values from now on are
   unchanged, and events at or after now (which a cancellation at now may
   still remove) are kept exactly. */
static void timeline_prune(TilefinchAudioTimeline *timeline, double now)
{
    size_t past = 0;
    while (past < timeline->count && timeline->events[past].time < now) past++;
    if (past == 0) return;
    size_t last = past - 1u;
    TilefinchAudioEvent e = timeline->events[last];
    TilefinchAudioEvent replacement[2];
    size_t keep = 0;
    switch (e.kind) {
    case TILEFINCH_AUDIO_EVENT_SET:
        replacement[keep++] = e;
        break;
    case TILEFINCH_AUDIO_EVENT_LINEAR:
    case TILEFINCH_AUDIO_EVENT_EXPONENTIAL:
        replacement[keep++] = (TilefinchAudioEvent) {.time = e.time,
            .value = e.value, .kind = TILEFINCH_AUDIO_EVENT_SET};
        break;
    case TILEFINCH_AUDIO_EVENT_TARGET: {
        if (last < 2u) return;
        TilefinchAudioSegment segments[TILEFINCH_AUDIO_TIMELINE_SEGMENT_LIMIT];
        size_t count = tilefinch_audio_timeline_segments(timeline, segments);
        size_t active = timeline_active(segments, count, e.time);
        replacement[keep++] = (TilefinchAudioEvent) {.time = e.time,
            .value = segments[active].v0, .kind = TILEFINCH_AUDIO_EVENT_SET};
        replacement[keep++] = e;
        break;
    }
    case TILEFINCH_AUDIO_EVENT_CURVE:
        if (timeline->curve_end > now) replacement[keep++] = e;
        else {
            float held = (float) timeline_raw_value(timeline,
                                                    timeline->curve_end);
            replacement[keep++] = (TilefinchAudioEvent) {
                .time = timeline->curve_end, .value = held,
                .kind = TILEFINCH_AUDIO_EVENT_SET};
        }
        break;
    default:
        return;
    }
    /* Equal counts still matter: a finished curve becomes a hold. */
    if (keep > past) return;
    bool curve_kept = keep != 0u
        && replacement[keep - 1u].kind == TILEFINCH_AUDIO_EVENT_CURVE;
    for (size_t at = 0; at < past && !curve_kept; at++)
        if (timeline->events[at].kind == TILEFINCH_AUDIO_EVENT_CURVE)
            timeline->curve_count = 0;
    memmove(&timeline->events[keep], &timeline->events[past],
            (timeline->count - past) * sizeof(timeline->events[0]));
    memcpy(timeline->events, replacement, keep * sizeof(replacement[0]));
    timeline->count = (uint8_t) (timeline->count - past + keep);
}

static bool timeline_float(double value, float *out)
{
    if (!isfinite(value)) return false;
    *out = (float) value;
    return isfinite(*out);
}

TilefinchAudioTimelineStatus tilefinch_audio_timeline_insert(
    TilefinchAudioTimeline *timeline, double now, TilefinchAudioEventKind kind,
    double value, double time, double aux, const float *curve, size_t count)
{
    float number = 0.0f;
    if (kind != TILEFINCH_AUDIO_EVENT_CURVE && !timeline_float(value, &number))
        return TILEFINCH_AUDIO_TIMELINE_TYPE_ERROR;
    if (!isfinite(time)
        || ((kind == TILEFINCH_AUDIO_EVENT_TARGET
             || kind == TILEFINCH_AUDIO_EVENT_CURVE) && !isfinite(aux)))
        return TILEFINCH_AUDIO_TIMELINE_TYPE_ERROR;
    /* The bound comes first: samples beyond it are never read. */
    if (kind == TILEFINCH_AUDIO_EVENT_CURVE) {
        if (count > TILEFINCH_AUDIO_TIMELINE_CURVE_LIMIT)
            return TILEFINCH_AUDIO_TIMELINE_POINT_QUOTA;
        if (curve == NULL && count != 0u)
            return TILEFINCH_AUDIO_TIMELINE_TYPE_ERROR;
        for (size_t at = 0; at < count; at++)
            if (!isfinite(curve[at])) return TILEFINCH_AUDIO_TIMELINE_TYPE_ERROR;
    }
    if (time < 0) return TILEFINCH_AUDIO_TIMELINE_RANGE_ERROR;
    if (kind == TILEFINCH_AUDIO_EVENT_EXPONENTIAL && number == 0.0f)
        return TILEFINCH_AUDIO_TIMELINE_RANGE_ERROR;
    if (kind == TILEFINCH_AUDIO_EVENT_TARGET && aux < 0)
        return TILEFINCH_AUDIO_TIMELINE_RANGE_ERROR;
    if (kind == TILEFINCH_AUDIO_EVENT_CURVE) {
        if (aux <= 0) return TILEFINCH_AUDIO_TIMELINE_RANGE_ERROR;
        if (count < 2u) return TILEFINCH_AUDIO_TIMELINE_INVALID_STATE;
    }
    if (kind < TILEFINCH_AUDIO_EVENT_SET || kind > TILEFINCH_AUDIO_EVENT_CURVE)
        return TILEFINCH_AUDIO_TIMELINE_TYPE_ERROR;
    if (time < now) time = now;
    double horizon = kind == TILEFINCH_AUDIO_EVENT_CURVE ? time + aux : time;
    if (horizon - now > TILEFINCH_AUDIO_TIMELINE_HORIZON_SECONDS)
        return TILEFINCH_AUDIO_TIMELINE_HORIZON;
    timeline_prune(timeline, now);
    /* A value curve owns [T, T + D): nothing else may start inside it, and
       a new curve may not contain another event strictly inside. */
    for (size_t at = 0; at < timeline->count; at++) {
        const TilefinchAudioEvent *e = &timeline->events[at];
        if (e->kind == TILEFINCH_AUDIO_EVENT_CURVE && time >= e->time
            && time < timeline->curve_end)
            return TILEFINCH_AUDIO_TIMELINE_OVERLAP;
        if (kind == TILEFINCH_AUDIO_EVENT_CURVE && e->time > time
            && e->time < time + aux)
            return TILEFINCH_AUDIO_TIMELINE_OVERLAP;
    }
    if (kind == TILEFINCH_AUDIO_EVENT_CURVE && timeline->curve_count != 0u)
        return TILEFINCH_AUDIO_TIMELINE_SECOND_CURVE;
    size_t position = 0;
    while (position < timeline->count
           && timeline->events[position].time <= time) position++;
    /* Ramps start from the previous event. None: an implicit
       setValueAtTime(current value, now). A setTarget that has started:
       continue from its value now. One that has not: the ramp replaces it,
       starting from the value just before it. */
    bool implicit = false, replace_target = false;
    float implicit_value = 0.0f;
    if (timeline_is_ramp((uint8_t) kind)) {
        if (position == 0u) {
            implicit = true;
            implicit_value = (float) timeline_raw_value(timeline, now);
        } else if (timeline->events[position - 1u].kind
                   == TILEFINCH_AUDIO_EVENT_TARGET) {
            const TilefinchAudioEvent *target = &timeline->events[position - 1u];
            if (target->time > now) {
                replace_target = true;
                TilefinchAudioSegment segments[TILEFINCH_AUDIO_TIMELINE_SEGMENT_LIMIT];
                size_t n = tilefinch_audio_timeline_segments(timeline, segments);
                implicit_value = segments[timeline_active(
                    segments, n, target->time)].v0;
            } else {
                implicit = true;
                implicit_value = (float) timeline_raw_value(timeline, now);
            }
        }
    }
    if ((size_t) timeline->count + 1u + (implicit ? 1u : 0u)
        > TILEFINCH_AUDIO_TIMELINE_EVENT_LIMIT)
        return TILEFINCH_AUDIO_TIMELINE_EVENT_QUOTA;
    if (replace_target) {
        TilefinchAudioEvent *target = &timeline->events[position - 1u];
        *target = (TilefinchAudioEvent) {.time = target->time,
            .value = implicit_value, .kind = TILEFINCH_AUDIO_EVENT_SET};
    }
    if (implicit) {
        timeline_place(timeline, position, (TilefinchAudioEvent) {
            .time = now, .value = implicit_value,
            .kind = TILEFINCH_AUDIO_EVENT_SET});
        position++;
    }
    TilefinchAudioEvent event = {.time = time, .value = number,
        .kind = (uint8_t) kind};
    if (kind == TILEFINCH_AUDIO_EVENT_TARGET) event.aux = aux;
    if (kind == TILEFINCH_AUDIO_EVENT_CURVE) {
        memcpy(timeline->curve, curve, count * sizeof(float));
        timeline->curve_count = (uint8_t) count;
        timeline->curve_duration = aux;
        timeline->curve_end = time + aux;
    }
    timeline_place(timeline, position, event);
    return TILEFINCH_AUDIO_TIMELINE_OK;
}

TilefinchAudioTimelineStatus tilefinch_audio_timeline_cancel(
    TilefinchAudioTimeline *timeline, double now, double time, bool hold)
{
    if (!isfinite(time)) return TILEFINCH_AUDIO_TIMELINE_TYPE_ERROR;
    if (time < 0) return TILEFINCH_AUDIO_TIMELINE_RANGE_ERROR;
    if (time < now) time = now;
    timeline_prune(timeline, now);
    if (!hold) {
        for (size_t at = timeline->count; at-- > 0;) {
            const TilefinchAudioEvent *e = &timeline->events[at];
            bool running_curve = e->kind == TILEFINCH_AUDIO_EVENT_CURVE
                && e->time < time && timeline->curve_end > time;
            if (e->time >= time || running_curve) timeline_remove(timeline, at);
        }
        return TILEFINCH_AUDIO_TIMELINE_OK;
    }
    /* cancelAndHoldAtTime: E1 is the last event at or before the time, E2
       the first after it. */
    size_t after = 0;
    while (after < timeline->count && timeline->events[after].time <= time)
        after++;
    float held = (float) timeline_raw_value(timeline, time);
    bool ramp_after = after < timeline->count
        && timeline_is_ramp(timeline->events[after].kind);
    bool insert = false;
    if (!ramp_after && after > 0u) {
        const TilefinchAudioEvent *e1 = &timeline->events[after - 1u];
        insert = e1->kind == TILEFINCH_AUDIO_EVENT_TARGET
            || (e1->kind == TILEFINCH_AUDIO_EVENT_CURVE
                && timeline->curve_end > time);
    }
    /* Everything after E2 (or after E1) goes; then the bound cannot fail
       for the one inserted event unless the list was already full up to
       the cancellation time. */
    size_t kept = after + (ramp_after ? 1u : 0u);
    if (insert && kept >= TILEFINCH_AUDIO_TIMELINE_EVENT_LIMIT)
        return TILEFINCH_AUDIO_TIMELINE_EVENT_QUOTA;
    while (timeline->count > kept) timeline_remove(timeline, timeline->count - 1u);
    if (ramp_after) {
        TilefinchAudioEvent *e2 = &timeline->events[after];
        e2->time = time;
        e2->value = held;
    } else if (insert) {
        if (timeline->events[after - 1u].kind == TILEFINCH_AUDIO_EVENT_CURVE)
            timeline->curve_end = time;
        timeline_place(timeline, after, (TilefinchAudioEvent) {.time = time,
            .value = held, .kind = TILEFINCH_AUDIO_EVENT_SET});
    }
    return TILEFINCH_AUDIO_TIMELINE_OK;
}

TilefinchAudioTimelineStatus tilefinch_audio_timeline_set_value(
    TilefinchAudioTimeline *timeline, double now, double value)
{
    float number = 0.0f;
    if (!timeline_float(value, &number)) return TILEFINCH_AUDIO_TIMELINE_TYPE_ERROR;
    timeline_prune(timeline, now);
    if (timeline->count != 0u) {
        TilefinchAudioTimelineStatus status = tilefinch_audio_timeline_insert(
            timeline, now, TILEFINCH_AUDIO_EVENT_SET, value, now, 0, NULL, 0);
        if (status != TILEFINCH_AUDIO_TIMELINE_OK) return status;
    }
    timeline->intrinsic = number;
    return TILEFINCH_AUDIO_TIMELINE_OK;
}

bool tilefinch_audio_timeline_settled(TilefinchAudioTimeline *timeline,
                                      double now, float *value,
                                      double *anchor, bool *anchored)
{
    if (!isfinite(now)) return false;
    timeline_prune(timeline, now);
    if (timeline->count == 0u) {
        *value = timeline->intrinsic;
        *anchored = false;
    } else if (timeline->count == 1u
               && timeline->events[0].kind == TILEFINCH_AUDIO_EVENT_SET
               && timeline->events[0].time < now) {
        *value = timeline->events[0].value;
        *anchor = timeline->events[0].time;
        *anchored = true;
    } else {
        /* A setTargetAtTime that is the last event and began at least 32
           time constants ago is within e^-32 (about 1e-14) of its target:
           the value is the target, as browsers snap to it. A ramp after a
           started target starts from now, so no anchor is needed. */
        const TilefinchAudioEvent *last = &timeline->events[timeline->count - 1u];
        if (last->kind != TILEFINCH_AUDIO_EVENT_TARGET || !(last->time < now)
            || !(now - last->time >= 32.0 * last->aux)) return false;
        *value = last->value;
        *anchored = false;
    }
    return *value >= timeline->minimum && *value <= timeline->maximum;
}

bool tilefinch_audio_timeline_anchor(TilefinchAudioTimeline *timeline,
                                     double time, float value)
{
    if (timeline->count != 0u || !isfinite(time) || !isfinite(value))
        return false;
    timeline->events[0] = (TilefinchAudioEvent) {.time = time, .value = value,
        .kind = TILEFINCH_AUDIO_EVENT_SET};
    timeline->count = 1u;
    timeline->intrinsic = value;
    return true;
}
