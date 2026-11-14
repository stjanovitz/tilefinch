#ifndef TILEFINCH_GAME_AUDIO_TIMELINE_H
#define TILEFINCH_GAME_AUDIO_TIMELINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * One AudioParam's automation event list, with the Web Audio
 * specification's insertion, ramp, target, curve and cancellation rules,
 * bounded for the game profile. Browser thread only: times are context
 * seconds (double), values are float as in the specification.
 *
 * The list compiles to self-contained segments, each running until the next
 * one starts. Reads of AudioParam.value and the mixer's rendering both come
 * from the same segments, so a read agrees with what is heard.
 *
 * Bounds: 32 events (events wholly in the past are folded away first), one
 * value curve of 2-64 samples, and times at most ten seconds ahead.
 */
#define TILEFINCH_AUDIO_TIMELINE_EVENT_LIMIT 32u
#define TILEFINCH_AUDIO_TIMELINE_CURVE_LIMIT 64u
#define TILEFINCH_AUDIO_TIMELINE_SEGMENT_LIMIT (TILEFINCH_AUDIO_TIMELINE_EVENT_LIMIT + 2u)
#define TILEFINCH_AUDIO_TIMELINE_HORIZON_SECONDS 10.0

typedef enum {
    TILEFINCH_AUDIO_EVENT_SET = 1,
    TILEFINCH_AUDIO_EVENT_LINEAR,
    TILEFINCH_AUDIO_EVENT_EXPONENTIAL,
    TILEFINCH_AUDIO_EVENT_TARGET,
    TILEFINCH_AUDIO_EVENT_CURVE
} TilefinchAudioEventKind;

/* Each status names the exception a binding throws. */
typedef enum {
    TILEFINCH_AUDIO_TIMELINE_OK = 0,
    TILEFINCH_AUDIO_TIMELINE_TYPE_ERROR,     /* TypeError: non-finite input */
    TILEFINCH_AUDIO_TIMELINE_RANGE_ERROR,    /* RangeError */
    TILEFINCH_AUDIO_TIMELINE_INVALID_STATE,  /* InvalidStateError: < 2 samples */
    TILEFINCH_AUDIO_TIMELINE_OVERLAP,        /* NotSupportedError: curve overlap */
    TILEFINCH_AUDIO_TIMELINE_SECOND_CURVE,   /* NotSupportedError: second curve */
    TILEFINCH_AUDIO_TIMELINE_HORIZON,        /* NotSupportedError: > 10 s ahead */
    TILEFINCH_AUDIO_TIMELINE_EVENT_QUOTA,    /* QuotaExceededError: 32 events */
    TILEFINCH_AUDIO_TIMELINE_POINT_QUOTA     /* QuotaExceededError: 64 samples */
} TilefinchAudioTimelineStatus;

typedef struct {
    double time;   /* SET: time; ramps: end time; TARGET, CURVE: start */
    double aux;    /* TARGET: time constant */
    float value;   /* SET, ramps: value; TARGET: target */
    uint8_t kind;  /* TilefinchAudioEventKind */
} TilefinchAudioEvent;

typedef struct {
    TilefinchAudioEvent events[TILEFINCH_AUDIO_TIMELINE_EVENT_LIMIT];
    float curve[TILEFINCH_AUDIO_TIMELINE_CURVE_LIMIT];
    /* The one curve's sampling duration and effective end: cancelAndHold
       may end it early without resampling it. */
    double curve_duration, curve_end;
    float intrinsic, minimum, maximum;
    uint8_t count, curve_count;
} TilefinchAudioTimeline;

typedef enum {
    TILEFINCH_AUDIO_SEGMENT_HOLD = 0,
    TILEFINCH_AUDIO_SEGMENT_LINEAR,
    TILEFINCH_AUDIO_SEGMENT_EXPONENTIAL,
    TILEFINCH_AUDIO_SEGMENT_TARGET,
    TILEFINCH_AUDIO_SEGMENT_CURVE
} TilefinchAudioSegmentKind;

/* HOLD: v0. LINEAR, EXPONENTIAL: v0 at start to v1 at end. TARGET: from v0
   toward v1 with time constant tau. CURVE: the timeline's samples over
   [start, end), end being start plus the sampling duration. The first
   segment starts at -INFINITY. */
typedef struct {
    double start, end, tau;
    float v0, v1;
    uint8_t kind;
} TilefinchAudioSegment;

void tilefinch_audio_timeline_init(TilefinchAudioTimeline *timeline,
                                   float intrinsic, float minimum,
                                   float maximum);
/* setValueAtTime, linear/exponentialRampToValueAtTime, setTargetAtTime
   (aux = time constant) and setValueCurveAtTime (aux = duration). Times
   before now are clamped to now. Nothing changes unless the result is OK. */
TilefinchAudioTimelineStatus tilefinch_audio_timeline_insert(
    TilefinchAudioTimeline *timeline, double now, TilefinchAudioEventKind kind,
    double value, double time, double aux, const float *curve, size_t count);
/* cancelScheduledValues (hold false) and cancelAndHoldAtTime (hold true). */
TilefinchAudioTimelineStatus tilefinch_audio_timeline_cancel(
    TilefinchAudioTimeline *timeline, double now, double time, bool hold);
/* The AudioParam.value setter: [[current value]] and, when events exist,
   setValueAtTime(value, now). */
TilefinchAudioTimelineStatus tilefinch_audio_timeline_set_value(
    TilefinchAudioTimeline *timeline, double now, double value);
/* The computed value at a context time, clamped to the nominal range. */
double tilefinch_audio_timeline_value(const TilefinchAudioTimeline *timeline,
                                      double time);
/* Compiles the list; returns the segment count (at least one). */
size_t tilefinch_audio_timeline_segments(const TilefinchAudioTimeline *timeline,
                                         TilefinchAudioSegment *segments);
/* True when the value can no longer change from now on: no events (the
   intrinsic value); once past events are folded, a single setValueAtTime
   before now (*anchored, at *anchor); or a last setTargetAtTime begun at
   least 32 time constants before now (its target). *value is in range.
   The owner may then drop the timeline for a static value, and anchor a
   new one there (below) if automation resumes. */
bool tilefinch_audio_timeline_settled(TilefinchAudioTimeline *timeline,
                                      double now, float *value,
                                      double *anchor, bool *anchored);
/* An empty timeline restarts from setValueAtTime(value, time) at a time
   that may be past, so a later ramp starts where a dropped timeline's
   would have. False (unchanged) when it already has events. */
bool tilefinch_audio_timeline_anchor(TilefinchAudioTimeline *timeline,
                                     double time, float value);
/* One segment's unclamped value at time t (t at or after its start). */
double tilefinch_audio_segment_value(const TilefinchAudioSegment *segment,
                                     const float *curve, size_t count,
                                     double t);

#endif
