/* AudioParam automation timelines against the Web Audio specification's
   event rules: insertion, ramps after holds and targets, exponential
   restrictions, curves, both cancellations, the bounds and folding. */
#include "tilefinch/game_audio_timeline.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "check failed at %s:%d: %s\n",                       \
                __FILE__, __LINE__, #condition);                             \
        exit(1);                                                             \
    }                                                                        \
} while (0)

#define NEAR(actual, expected) do {                                          \
    double a_ = (actual), e_ = (expected);                                   \
    if (!(fabs(a_ - e_) <= 1e-5 + 1e-5 * fabs(e_))) {                        \
        fprintf(stderr, "at %s:%d: %s = %.9g, expected %.9g\n",              \
                __FILE__, __LINE__, #actual, a_, e_);                        \
        exit(1);                                                             \
    }                                                                        \
} while (0)

typedef TilefinchAudioTimeline Timeline;
enum { OK = TILEFINCH_AUDIO_TIMELINE_OK };

static Timeline make(float intrinsic)
{
    Timeline t;
    tilefinch_audio_timeline_init(&t, intrinsic, -1e6f, 1e6f);
    return t;
}

static int set(Timeline *t, double now, double value, double time)
{
    return tilefinch_audio_timeline_insert(t, now, TILEFINCH_AUDIO_EVENT_SET,
                                           value, time, 0, NULL, 0);
}

static int linear(Timeline *t, double now, double value, double time)
{
    return tilefinch_audio_timeline_insert(t, now, TILEFINCH_AUDIO_EVENT_LINEAR,
                                           value, time, 0, NULL, 0);
}

static int exponential(Timeline *t, double now, double value, double time)
{
    return tilefinch_audio_timeline_insert(
        t, now, TILEFINCH_AUDIO_EVENT_EXPONENTIAL, value, time, 0, NULL, 0);
}

static int target(Timeline *t, double now, double value, double time, double tau)
{
    return tilefinch_audio_timeline_insert(t, now, TILEFINCH_AUDIO_EVENT_TARGET,
                                           value, time, tau, NULL, 0);
}

static int curve(Timeline *t, double now, const float *values, size_t count,
                 double time, double duration)
{
    return tilefinch_audio_timeline_insert(t, now, TILEFINCH_AUDIO_EVENT_CURVE,
                                           0, time, duration, values, count);
}

static double at(const Timeline *t, double time)
{
    return tilefinch_audio_timeline_value(t, time);
}

static void test_beep(void)
{
    /* gain.setValueAtTime(.3, t); gain.exponentialRampToValueAtTime(.01, t + .3) */
    Timeline t = make(1.0f);
    CHECK(set(&t, 0, .3, 1.0) == OK);
    CHECK(exponential(&t, 0, .01, 1.3) == OK);
    NEAR(at(&t, .5), 1.0);
    NEAR(at(&t, 1.0), .3);
    NEAR(at(&t, 1.15), (double) .3f * pow((double) .01f / (double) .3f, .5));
    NEAR(at(&t, 1.3), .01);
    NEAR(at(&t, 5.0), .01);
    /* The same with a linear ramp. */
    t = make(1.0f);
    CHECK(set(&t, 0, 0, 1.0) == OK);
    CHECK(linear(&t, 0, 1, 2.0) == OK);
    NEAR(at(&t, 1.25), .25);
    NEAR(at(&t, 2.5), 1.0);
}

static void test_ramp_without_previous_event(void)
{
    /* A ramp with no earlier event starts from the value now. */
    Timeline t = make(0.0f);
    CHECK(linear(&t, .5, 1, 1.5) == OK);
    NEAR(at(&t, .5), 0);
    NEAR(at(&t, 1.0), .5);
    NEAR(at(&t, 1.5), 1);
    CHECK(t.count == 2u);
}

static void test_exponential_rules(void)
{
    Timeline t = make(1.0f);
    CHECK(exponential(&t, 0, 0, 1) == TILEFINCH_AUDIO_TIMELINE_RANGE_ERROR);
    CHECK(t.count == 0u);
    /* From zero, or through a sign change: V0 holds until the end time. */
    CHECK(set(&t, 0, 0, 0) == OK);
    CHECK(exponential(&t, 0, 1, 1) == OK);
    NEAR(at(&t, .5), 0);
    NEAR(at(&t, 1), 1);
    t = make(1.0f);
    CHECK(set(&t, 0, -1, 0) == OK);
    CHECK(exponential(&t, 0, 1, 1) == OK);
    NEAR(at(&t, .99), -1);
    NEAR(at(&t, 1), 1);
}

static void test_ramp_after_target(void)
{
    /* A target that has started: the ramp continues from its value now. */
    Timeline t = make(0.0f);
    CHECK(target(&t, 0, 1, 0, .1) == OK);
    NEAR(at(&t, .1), 1 - exp(-1));
    CHECK(linear(&t, .2, 0, .4) == OK);
    double from = 1 - exp(-2);
    NEAR(at(&t, .2), from);
    NEAR(at(&t, .3), from / 2);
    NEAR(at(&t, .4), 0);
    /* A target that has not started is replaced by a hold of the value
       before it; the ramp runs from there. */
    t = make(0.0f);
    CHECK(target(&t, 0, 1, 1, .1) == OK);
    CHECK(linear(&t, 0, .5, 2) == OK);
    NEAR(at(&t, .5), 0);
    NEAR(at(&t, 1.5), .25);
    NEAR(at(&t, 3), .5);
    /* A target after a ramp starts from the ramp's end value. */
    t = make(0.0f);
    CHECK(set(&t, 0, 0, 0) == OK);
    CHECK(linear(&t, 0, 1, 1) == OK);
    CHECK(target(&t, 0, 0, 2, .5) == OK);
    NEAR(at(&t, 1.5), 1);
    NEAR(at(&t, 2.5), exp(-1));
    /* Time constant zero jumps at its start time. */
    t = make(0.0f);
    CHECK(target(&t, 0, 3, 1, 0) == OK);
    NEAR(at(&t, .9), 0);
    NEAR(at(&t, 1), 3);
}

static void test_curves(void)
{
    static const float up[] = {0, 1};
    static const float shape[] = {0, 1, .5};
    Timeline t = make(.25f);
    CHECK(curve(&t, 0, shape, 3, 1, 2) == OK);
    NEAR(at(&t, .5), .25);
    NEAR(at(&t, 1.5), .5);
    NEAR(at(&t, 2.5), .75);
    NEAR(at(&t, 3.5), .5);
    /* The curve owns [T, T + D). */
    CHECK(set(&t, 0, 5, 2) == TILEFINCH_AUDIO_TIMELINE_OVERLAP);
    CHECK(set(&t, 0, 5, 1) == TILEFINCH_AUDIO_TIMELINE_OVERLAP);
    CHECK(set(&t, 0, 5, 3) == OK);
    NEAR(at(&t, 3), 5);
    CHECK(curve(&t, 0, up, 2, 4, 1) == TILEFINCH_AUDIO_TIMELINE_SECOND_CURVE);
    /* A curve may not swallow an existing event. */
    t = make(0);
    CHECK(set(&t, 0, 1, 1.5) == OK);
    CHECK(curve(&t, 0, up, 2, 1, 1) == TILEFINCH_AUDIO_TIMELINE_OVERLAP);
    CHECK(curve(&t, 0, up, 2, 1.5, 1) == OK);
    /* Sample counts and durations. */
    t = make(0);
    CHECK(curve(&t, 0, up, 1, 1, 1) == TILEFINCH_AUDIO_TIMELINE_INVALID_STATE);
    CHECK(curve(&t, 0, up, 65, 1, 1) == TILEFINCH_AUDIO_TIMELINE_POINT_QUOTA);
    CHECK(curve(&t, 0, up, 2, 1, 0) == TILEFINCH_AUDIO_TIMELINE_RANGE_ERROR);
    CHECK(t.count == 0u);
}

static void test_cancel_scheduled_values(void)
{
    Timeline t = make(0);
    CHECK(set(&t, 0, 1, 1) == OK);
    CHECK(set(&t, 0, 2, 2) == OK);
    CHECK(set(&t, 0, 3, 3) == OK);
    /* Future cancellation removes events at or after the time. */
    CHECK(tilefinch_audio_timeline_cancel(&t, 0, 2, false) == OK);
    CHECK(t.count == 1u);
    NEAR(at(&t, 2.5), 1);
    NEAR(at(&t, 3.5), 1);
    /* A ramp ending after the time goes, so its start value holds. */
    t = make(0);
    CHECK(set(&t, 0, 0, 0) == OK);
    CHECK(linear(&t, 0, 1, 2) == OK);
    CHECK(tilefinch_audio_timeline_cancel(&t, .5, 1, false) == OK);
    NEAR(at(&t, 1.5), 0);
    /* A running curve goes as well. */
    static const float up[] = {0, 1};
    t = make(.5f);
    CHECK(curve(&t, 0, up, 2, 0, 2) == OK);
    CHECK(tilefinch_audio_timeline_cancel(&t, 1, 1, false) == OK);
    CHECK(t.count == 0u && t.curve_count == 0u);
    NEAR(at(&t, 1.5), .5);
}

static void test_cancel_and_hold(void)
{
    /* A ramp after the time ends early, at the value it had then. */
    Timeline t = make(0);
    CHECK(set(&t, 0, 0, 0) == OK);
    CHECK(linear(&t, 0, 1, 2) == OK);
    CHECK(tilefinch_audio_timeline_cancel(&t, .25, 1, true) == OK);
    NEAR(at(&t, .5), .25);
    NEAR(at(&t, 1), .5);
    NEAR(at(&t, 1.5), .5);
    /* An exponential ramp the same way. */
    t = make(0);
    CHECK(set(&t, 0, 1, 0) == OK);
    CHECK(exponential(&t, 0, .01, 2) == OK);
    CHECK(tilefinch_audio_timeline_cancel(&t, 0, 1, true) == OK);
    NEAR(at(&t, 3), .1);
    /* A running target holds its value at the time. */
    t = make(0);
    CHECK(target(&t, 0, 1, 0, .5) == OK);
    CHECK(set(&t, 0, 9, 3) == OK);
    CHECK(tilefinch_audio_timeline_cancel(&t, 0, 1, true) == OK);
    NEAR(at(&t, 1), 1 - exp(-2));
    NEAR(at(&t, 4), 1 - exp(-2));
    /* A running curve holds its value at the time. */
    static const float up[] = {0, 1};
    t = make(0);
    CHECK(curve(&t, 0, up, 2, 0, 2) == OK);
    CHECK(tilefinch_audio_timeline_cancel(&t, 0, 1, true) == OK);
    NEAR(at(&t, .5), .25);
    NEAR(at(&t, 1.5), .5);
    /* A new event may follow the shortened curve. */
    CHECK(set(&t, 0, 2, 1.5) == OK);
    NEAR(at(&t, 1.6), 2);
}

static void test_bounds_and_errors(void)
{
    Timeline t = make(0);
    CHECK(set(&t, 0, NAN, 1) == TILEFINCH_AUDIO_TIMELINE_TYPE_ERROR);
    CHECK(set(&t, 0, 1, INFINITY) == TILEFINCH_AUDIO_TIMELINE_TYPE_ERROR);
    CHECK(set(&t, 0, 1e39, 1) == TILEFINCH_AUDIO_TIMELINE_TYPE_ERROR);
    CHECK(set(&t, 0, 1, -1) == TILEFINCH_AUDIO_TIMELINE_RANGE_ERROR);
    CHECK(target(&t, 0, 1, 1, -1) == TILEFINCH_AUDIO_TIMELINE_RANGE_ERROR);
    CHECK(set(&t, 0, 1, 10.5) == TILEFINCH_AUDIO_TIMELINE_HORIZON);
    static const float up[] = {0, 1};
    CHECK(curve(&t, 0, up, 2, 9, 2) == TILEFINCH_AUDIO_TIMELINE_HORIZON);
    CHECK(t.count == 0u);
    /* Thirty-two events; the next is refused without changing anything. */
    for (int i = 0; i < 32; i++) CHECK(set(&t, 0, i, .1 * (i + 1)) == OK);
    CHECK(set(&t, 0, 99, 5) == TILEFINCH_AUDIO_TIMELINE_EVENT_QUOTA);
    CHECK(linear(&t, 0, 99, 5) == TILEFINCH_AUDIO_TIMELINE_EVENT_QUOTA);
    CHECK(t.count == 32u);
    NEAR(at(&t, 4), 31);
    /* Times before now clamp to now. */
    t = make(0);
    CHECK(set(&t, 2, 7, 1) == OK);
    NEAR(at(&t, 1.5), 0);
    NEAR(at(&t, 2), 7);
    /* Values clamp to the nominal range when read. */
    tilefinch_audio_timeline_init(&t, 0, 0, 1);
    CHECK(set(&t, 0, 3, 0) == OK);
    NEAR(at(&t, 1), 1);
}

static void test_folding_past_events(void)
{
    /* A page that schedules forever never exhausts the bound. */
    Timeline t = make(0);
    for (int i = 0; i < 400; i++) {
        double now = i * .05;
        CHECK(set(&t, now, i, now + .01) == OK);
        CHECK(linear(&t, now, i + .5, now + .03) == OK);
        CHECK(t.count <= 6u);
    }
    NEAR(at(&t, 399 * .05 + .02), 399.25);
    /* A running target folds to an equivalent hold plus itself. */
    t = make(0);
    CHECK(set(&t, 0, 1, 0) == OK);
    CHECK(set(&t, 0, 2, .1) == OK);
    CHECK(target(&t, 0, 4, .2, .5) == OK);
    double before = at(&t, 1.0);
    CHECK(set(&t, .9, 5, 3) == OK);
    CHECK(t.count == 3u);
    NEAR(at(&t, 1.0), before);
    /* A finished curve folds to its held end value. */
    static const float shape[] = {0, 1, .5};
    t = make(0);
    CHECK(curve(&t, 0, shape, 3, 0, 1) == OK);
    CHECK(set(&t, 2, 9, 3) == OK);
    CHECK(t.curve_count == 0u);
    NEAR(at(&t, 2.5), .5);
    static const float up[] = {0, 1};
    CHECK(curve(&t, 2, up, 2, 3.5, 1) == OK);
}

static void test_value_setter(void)
{
    /* No events: only the intrinsic value changes. */
    Timeline t = make(1);
    CHECK(tilefinch_audio_timeline_set_value(&t, 1, .5) == OK);
    CHECK(t.count == 0u);
    NEAR(at(&t, 0), .5);
    /* With events: also setValueAtTime(value, now). */
    CHECK(set(&t, 1, 0, 2) == OK);
    CHECK(tilefinch_audio_timeline_set_value(&t, 1.5, .75) == OK);
    NEAR(at(&t, 1.6), .75);
    NEAR(at(&t, 2.5), 0);
    CHECK(tilefinch_audio_timeline_set_value(&t, 1.5, NAN)
          == TILEFINCH_AUDIO_TIMELINE_TYPE_ERROR);
}

static void test_segments(void)
{
    /* The compiled form the mixer evaluates agrees with reads. */
    Timeline t = make(1);
    CHECK(set(&t, 0, .3, 1) == OK);
    CHECK(exponential(&t, 0, .01, 1.3) == OK);
    CHECK(target(&t, 0, .5, 2, .1) == OK);
    TilefinchAudioSegment s[TILEFINCH_AUDIO_TIMELINE_SEGMENT_LIMIT];
    size_t n = tilefinch_audio_timeline_segments(&t, s);
    CHECK(n == 4u);
    CHECK(s[0].kind == TILEFINCH_AUDIO_SEGMENT_HOLD && isinf(s[0].start));
    CHECK(s[1].kind == TILEFINCH_AUDIO_SEGMENT_EXPONENTIAL && s[1].end == 1.3);
    CHECK(s[2].kind == TILEFINCH_AUDIO_SEGMENT_HOLD);
    CHECK(s[3].kind == TILEFINCH_AUDIO_SEGMENT_TARGET);
    NEAR(tilefinch_audio_segment_value(&s[3], NULL, 0, 2.1),
         .5 + ((double) .01f - .5) * exp(-1));
}

/* Settled timelines (a lane its owner may return for a static value) and
   the anchor a resumed timeline restarts from. */
static void test_settled_and_anchor(void)
{
    float value = 0;
    double anchor = -1;
    bool anchored = true;
    Timeline t = make(.25f);
    CHECK(tilefinch_audio_timeline_settled(&t, 1, &value, &anchor, &anchored));
    CHECK(value == .25f && !anchored);
    /* A ramp still running, or a set at now, is not settled. */
    CHECK(set(&t, 0, 0, 0) == OK && linear(&t, 0, .5, 1) == OK);
    CHECK(!tilefinch_audio_timeline_settled(&t, .5, &value, &anchor, &anchored));
    CHECK(!tilefinch_audio_timeline_settled(&t, 1, &value, &anchor, &anchored));
    CHECK(tilefinch_audio_timeline_settled(&t, 1.5, &value, &anchor, &anchored));
    CHECK(value == .5f && anchored && anchor == 1);
    /* Anchoring a fresh timeline there: a ramp starts from the old event. */
    Timeline resumed = make(.5f);
    CHECK(tilefinch_audio_timeline_anchor(&resumed, anchor, value));
    CHECK(!tilefinch_audio_timeline_anchor(&resumed, anchor, value));
    CHECK(linear(&resumed, 2, 1, 3) == OK);
    NEAR(tilefinch_audio_timeline_value(&resumed, 2), .5 + .5 * (2 - 1) / (3 - 1.0));
    /* A target settles after 32 time constants, on its target. */
    Timeline aim = make(1);
    CHECK(target(&aim, 0, .5, 1, .01) == OK);
    CHECK(!tilefinch_audio_timeline_settled(&aim, 1.3, &value, &anchor, &anchored));
    CHECK(tilefinch_audio_timeline_settled(&aim, 1.33, &value, &anchor, &anchored));
    CHECK(value == .5f && !anchored);
    /* Events still ahead, and out-of-range values, are not settled. */
    Timeline ahead = make(0);
    CHECK(set(&ahead, 0, 1, 5) == OK);
    CHECK(!tilefinch_audio_timeline_settled(&ahead, 1, &value, &anchor, &anchored));
    Timeline narrow;
    tilefinch_audio_timeline_init(&narrow, 0, 0, 1);
    CHECK(set(&narrow, 0, 2, 0) == OK);
    CHECK(!tilefinch_audio_timeline_settled(&narrow, 1, &value, &anchor, &anchored));
}

int main(void)
{
    test_beep();
    test_ramp_without_previous_event();
    test_exponential_rules();
    test_ramp_after_target();
    test_curves();
    test_cancel_scheduled_values();
    test_cancel_and_hold();
    test_bounds_and_errors();
    test_folding_past_events();
    test_value_setter();
    test_segments();
    test_settled_and_anchor();
    puts("game audio timeline: specification event rules passed");
    return 0;
}
