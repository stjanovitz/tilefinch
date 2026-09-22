#include "tilefinch/frame_pumps.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return 1; \
        } \
    } while (0)

/* Pump pairs which can claim the same contended resource within one frame.
   Every entry is a stated decision. A new pump, a dropped yield, or a new
   resource claim changes the computed set and fails this test until the
   author either adds a yield relation or records the reason here. */
typedef struct {
    FramePumpId first;
    FramePumpId second;
    const char *reason;
} FramePumpOverlap;

static const FramePumpOverlap expected_overlaps[] = {
    /* Deliberate. */
    { FRAME_PUMP_PROVIDER_PRERESOLVE, FRAME_PUMP_PAGE_IDLE,
      "pre-resolution takes the first slice and page idle may continue one "
      "bounded slice after it" },
    { FRAME_PUMP_PROVIDER_PRERESOLVE, FRAME_PUMP_DEFERRED_IMAGE,
      "pre-resolution runs before the frame is presented; the image unit "
      "runs after the input response is already visible" },
    /* User-started foreground Memory Stick writers. Each is modal or
       explicitly requested, and none is optional work that could yield
       without stalling what the user asked for. */
    { FRAME_PUMP_UPDATE_SESSION, FRAME_PUMP_SCREENSHOT,
      "both user-started; the capture is four rows per frame" },
    { FRAME_PUMP_UPDATE_SESSION, FRAME_PUMP_OFFLINE_DOWNLOAD,
      "both user-started foreground transfers" },
    { FRAME_PUMP_SCREENSHOT, FRAME_PUMP_OFFLINE_DOWNLOAD,
      "both user-started; the capture is four rows per frame" },
};

static int test_policy_table(void)
{
    const char *defect = NULL;
    if (!frame_pumps_policy_valid(&defect)) {
        fprintf(stderr, "frame pump policy: %s\n", defect);
        return 1;
    }
    for (unsigned id = 0; id < FRAME_PUMP_COUNT; id++)
        CHECK(frame_pump_policy((FramePumpId) id) != NULL);
    CHECK(frame_pump_policy(FRAME_PUMP_COUNT) == NULL);
    return 0;
}

static int test_admission(void)
{
    FramePumpFrame frame = {0};
    frame_pumps_begin(&frame);
    CHECK(frame_pumps_admit(&frame, FRAME_PUMP_SITE_RESTORE_STORAGE, 0, 0));

    /* A fact named by the policy refuses; an unrelated fact does not. */
    CHECK(!frame_pumps_admit(&frame, FRAME_PUMP_SITE_RESTORE_STORAGE,
                             FRAME_FACT_INPUT_BUTTONS, 0));
    CHECK(frame_pumps_admit(&frame, FRAME_PUMP_SITE_RESTORE_STORAGE,
                            FRAME_FACT_INPUT_INTENT, 0));
    CHECK(!frame_pumps_admit(&frame, FRAME_PUMP_PAGE_IDLE,
                             FRAME_FACT_INPUT_INTENT, 0));

    /* Cache restoration alone stays out of the association ladder. */
    frame_pumps_begin(&frame);
    CHECK(frame_pumps_admit(&frame, FRAME_PUMP_SITE_RESTORE_STORAGE,
                            FRAME_FACT_NETWORK_WARMING, 0));
    CHECK(!frame_pumps_admit(&frame, FRAME_PUMP_SITE_RESTORE_CACHE,
                             FRAME_FACT_NETWORK_WARMING, 0));

    /* Yielding to pending work does not need that work to have run. */
    frame_pumps_begin(&frame);
    CHECK(!frame_pumps_admit(&frame, FRAME_PUMP_SITE_RESTORE_STORAGE, 0,
                             FRAME_PUMP_BIT(FRAME_PUMP_SCREENSHOT)));
    CHECK(frame_pumps_admit(&frame, FRAME_PUMP_SITE_RESTORE_STORAGE, 0, 0));
    /* Pending work the policy does not name is ignored. */
    CHECK(frame_pumps_admit(&frame, FRAME_PUMP_PAGE_IDLE, 0,
                            FRAME_PUMP_BIT(FRAME_PUMP_SCREENSHOT)));

    /* Yielding to a consumed slice needs the slice, not the admission. */
    frame_pumps_begin(&frame);
    CHECK(frame_pumps_admit(&frame, FRAME_PUMP_SITE_RESTORE_STORAGE, 0, 0));
    CHECK(frame_pumps_admit(&frame, FRAME_PUMP_PAGE_IDLE, 0, 0));
    frame_pumps_begin(&frame);
    CHECK(frame_pumps_admit(&frame, FRAME_PUMP_SITE_RESTORE_STORAGE, 0, 0));
    frame_pumps_mark_ran(&frame, FRAME_PUMP_SITE_RESTORE_STORAGE);
    CHECK(!frame_pumps_admit(&frame, FRAME_PUMP_PROVIDER_PRERESOLVE, 0, 0));
    CHECK(!frame_pumps_admit(&frame, FRAME_PUMP_PAGE_IDLE, 0, 0));
    /* A new frame forgets the slice. */
    frame_pumps_begin(&frame);
    CHECK(frame_pumps_admit(&frame, FRAME_PUMP_PAGE_IDLE, 0, 0));

    /* A writer which started and finished inside this frame was never seen
       as pending. Its recorded slice alone defers restoration. */
    frame_pumps_begin(&frame);
    CHECK(frame_pumps_admit(&frame, FRAME_PUMP_SCREENSHOT, 0, 0));
    frame_pumps_mark_ran(&frame, FRAME_PUMP_SCREENSHOT);
    CHECK(!frame_pumps_admit(&frame, FRAME_PUMP_SITE_RESTORE_STORAGE, 0, 0));

    /* A frame performs one Memory Stick operation and one long idle slice.
       These were contention the per-site predicates allowed by omission. */
    frame_pumps_begin(&frame);
    CHECK(frame_pumps_admit(&frame, FRAME_PUMP_OFFLINE_DOWNLOAD, 0, 0));
    frame_pumps_mark_ran(&frame, FRAME_PUMP_OFFLINE_DOWNLOAD);
    CHECK(!frame_pumps_admit(&frame, FRAME_PUMP_PAGE_IDLE, 0, 0));
    CHECK(!frame_pumps_admit(&frame, FRAME_PUMP_BASELINE_FONTS, 0, 0));
    frame_pumps_begin(&frame);
    CHECK(frame_pumps_admit(&frame, FRAME_PUMP_SITE_RESTORE_STORAGE, 0, 0));
    frame_pumps_mark_ran(&frame, FRAME_PUMP_SITE_RESTORE_STORAGE);
    CHECK(!frame_pumps_admit(&frame, FRAME_PUMP_BASELINE_FONTS, 0, 0));
    /* Expendable cache bodies wait for the required faces, not the reverse. */
    frame_pumps_begin(&frame);
    CHECK(!frame_pumps_admit(&frame, FRAME_PUMP_SITE_RESTORE_CACHE, 0,
                             FRAME_PUMP_BIT(FRAME_PUMP_BASELINE_FONTS)));
    CHECK(frame_pumps_admit(&frame, FRAME_PUMP_SITE_RESTORE_STORAGE, 0,
                            FRAME_PUMP_BIT(FRAME_PUMP_BASELINE_FONTS)));
    CHECK(frame_pumps_admit(&frame, FRAME_PUMP_BASELINE_FONTS, 0, 0));

    /* The font read yields only to a contending handshake. */
    frame_pumps_begin(&frame);
    CHECK(!frame_pumps_admit(&frame, FRAME_PUMP_BASELINE_FONTS,
                             FRAME_FACT_PRECONNECT_CONTENDS, 0));
    CHECK(frame_pumps_admit(&frame, FRAME_PUMP_BASELINE_FONTS,
                            FRAME_FACT_NETWORK_WARMING, 0));

    CHECK(!frame_pumps_admit(NULL, FRAME_PUMP_PAGE_IDLE, 0, 0));
    CHECK(!frame_pumps_admit(&frame, FRAME_PUMP_COUNT, 0, 0));
    return 0;
}

static int test_order_is_observed(void)
{
    FramePumpFrame frame = {0};
    frame_pumps_begin(&frame);
    (void) frame_pumps_admit(&frame, FRAME_PUMP_OFFLINE_DOWNLOAD, 0, 0);
    (void) frame_pumps_admit(&frame, FRAME_PUMP_PAGE_IDLE, 0, 0);
    (void) frame_pumps_admit(&frame, FRAME_PUMP_PAGE_IDLE, 0, 0);
    (void) frame_pumps_admit(&frame, FRAME_PUMP_BASELINE_FONTS, 0, 0);
    CHECK(frame.order_violations == 0);
    (void) frame_pumps_admit(&frame, FRAME_PUMP_SCREENSHOT, 0, 0);
    CHECK(frame.order_violations == 1);
    /* The count survives the frame so an exit report can publish it. */
    frame_pumps_begin(&frame);
    CHECK(frame.order_violations == 1);
    (void) frame_pumps_admit(&frame, FRAME_PUMP_SCREENSHOT, 0, 0);
    CHECK(frame.order_violations == 1);
    return 0;
}

/* A call site may only record a slice the policy admitted in this frame. */
static int test_slices_require_admission(void)
{
    FramePumpFrame frame = {0};
    frame_pumps_begin(&frame);
    CHECK(frame_pumps_admit(&frame, FRAME_PUMP_SCREENSHOT, 0, 0));
    frame_pumps_mark_ran(&frame, FRAME_PUMP_SCREENSHOT);
    CHECK(frame.wiring_violations == 0);
    CHECK(frame.admissions[FRAME_PUMP_SCREENSHOT] == 1);
    CHECK(frame.slices[FRAME_PUMP_SCREENSHOT] == 1);

    /* Never asked. */
    frame_pumps_mark_ran(&frame, FRAME_PUMP_UPDATE_SESSION);
    CHECK(frame.wiring_violations == 1);
    /* Asked and refused. */
    CHECK(!frame_pumps_admit(&frame, FRAME_PUMP_PAGE_IDLE,
                             FRAME_FACT_INPUT_BUTTONS, 0));
    CHECK(frame.admissions[FRAME_PUMP_PAGE_IDLE] == 0);
    frame_pumps_mark_ran(&frame, FRAME_PUMP_PAGE_IDLE);
    CHECK(frame.wiring_violations == 2);
    /* Admitted last frame is not admitted now. */
    frame_pumps_begin(&frame);
    frame_pumps_mark_ran(&frame, FRAME_PUMP_SCREENSHOT);
    CHECK(frame.wiring_violations == 3);
    return 0;
}

/* A yield is a refusal owed to another pump. A refusing frame fact is not
   one, so a run's yield counts say which overlap decisions arbitrated. */
static int test_yields_count_only_pump_relations(void)
{
    FramePumpFrame frame = {0};
    frame_pumps_begin(&frame);
    CHECK(!frame_pumps_admit(&frame, FRAME_PUMP_BASELINE_FONTS,
                             FRAME_FACT_PRECONNECT_CONTENDS, 0));
    CHECK(frame.yields[FRAME_PUMP_BASELINE_FONTS] == 0);

    /* yields_to_ran: the restore consumed the Memory Stick this frame. */
    CHECK(frame_pumps_admit(&frame, FRAME_PUMP_SITE_RESTORE_STORAGE, 0, 0));
    frame_pumps_mark_ran(&frame, FRAME_PUMP_SITE_RESTORE_STORAGE);
    CHECK(!frame_pumps_admit(&frame, FRAME_PUMP_BASELINE_FONTS, 0, 0));
    CHECK(frame.yields[FRAME_PUMP_BASELINE_FONTS] == 1);

    /* yields_to_active: fonts still have work, cache bodies wait. */
    frame_pumps_begin(&frame);
    CHECK(!frame_pumps_admit(&frame, FRAME_PUMP_SITE_RESTORE_CACHE, 0,
                             FRAME_PUMP_BIT(FRAME_PUMP_BASELINE_FONTS)));
    CHECK(frame.yields[FRAME_PUMP_SITE_RESTORE_CACHE] == 1);
    CHECK(frame.admissions[FRAME_PUMP_SITE_RESTORE_CACHE] == 0);
    return 0;
}

/* Run one frame using only what a real call site can do: sample, ask, and
   record a slice after a successful admission. `work` is the pumps with
   something to do; a pump in `finishes` completes inside its slice, so later
   pumps sample it as no longer pending and only its recorded slice remains --
   the case a yields_to_active relation alone would miss. Returns the pumps
   which ran. */
static uint16_t simulate_frame(uint32_t facts, uint16_t work,
                               uint16_t finishes, uint32_t *wiring_violations)
{
    FramePumpFrame frame = {0};
    uint16_t pending = work;
    frame_pumps_begin(&frame);
    for (unsigned id = 0; id < FRAME_PUMP_COUNT; id++) {
        if ((work & FRAME_PUMP_BIT(id)) == 0) continue;
        if (!frame_pumps_admit(&frame, (FramePumpId) id, facts, pending))
            continue;
        frame_pumps_mark_ran(&frame, (FramePumpId) id);
        if ((finishes & FRAME_PUMP_BIT(id)) != 0)
            pending &= (uint16_t) ~FRAME_PUMP_BIT(id);
    }
    *wiring_violations += frame.wiring_violations;
    return frame.ran;
}

static int test_resource_overlaps_are_pinned(void)
{
    bool observed[FRAME_PUMP_COUNT][FRAME_PUMP_COUNT];
    uint32_t wiring_violations = 0;
    memset(observed, 0, sizeof(observed));
    /* Only pumps somebody yields to while active can change an outcome by
       finishing, so only their completion needs enumerating. */
    uint16_t watched = 0;
    for (unsigned id = 0; id < FRAME_PUMP_COUNT; id++)
        watched |= frame_pump_policy((FramePumpId) id)->yields_to_active;
    for (uint32_t facts = 0; facts <= FRAME_FACT_ALL; facts++) {
        for (uint32_t work = 0; work < (1u << FRAME_PUMP_COUNT); work++) {
          /* Enumerate every subset of the watched pumps which have work. */
          uint16_t candidates = (uint16_t) (work & watched);
          for (uint16_t finishes = candidates;;
               finishes = (uint16_t) ((finishes - 1u) & candidates)) {
            uint16_t ran = simulate_frame(
                facts, (uint16_t) work, finishes, &wiring_violations);
            for (unsigned a = 0; a < FRAME_PUMP_COUNT; a++) {
                if ((ran & FRAME_PUMP_BIT(a)) == 0) continue;
                for (unsigned b = a + 1u; b < FRAME_PUMP_COUNT; b++) {
                    if ((ran & FRAME_PUMP_BIT(b)) == 0) continue;
                    if ((frame_pump_policy((FramePumpId) a)->resources
                         & frame_pump_policy((FramePumpId) b)->resources)
                        != 0) observed[a][b] = true;
                }
            }
            if (finishes == 0) break;
          }
        }
    }
    int failures = 0;
    CHECK(wiring_violations == 0);
    size_t expected_count =
        sizeof(expected_overlaps) / sizeof(expected_overlaps[0]);
    for (size_t i = 0; i < expected_count; i++) {
        const FramePumpOverlap *overlap = &expected_overlaps[i];
        CHECK(overlap->first < overlap->second);
        CHECK(overlap->reason != NULL && overlap->reason[0] != '\0');
        if (!observed[overlap->first][overlap->second]) {
            fprintf(stderr,
                    "stale overlap waiver: %s + %s can no longer share a "
                    "frame; remove it\n",
                    frame_pump_policy(overlap->first)->name,
                    frame_pump_policy(overlap->second)->name);
            failures++;
        }
        observed[overlap->first][overlap->second] = false;
    }
    for (unsigned a = 0; a < FRAME_PUMP_COUNT; a++) {
        for (unsigned b = a + 1u; b < FRAME_PUMP_COUNT; b++) {
            if (!observed[a][b]) continue;
            fprintf(stderr,
                    "undeclared overlap: %s and %s can claim the same "
                    "resource in one frame; add a yield or state why not\n",
                    frame_pump_policy((FramePumpId) a)->name,
                    frame_pump_policy((FramePumpId) b)->name);
            failures++;
        }
    }
    return failures == 0 ? 0 : 1;
}

int main(void)
{
    if (test_policy_table() != 0) return 1;
    if (test_admission() != 0) return 1;
    if (test_order_is_observed() != 0) return 1;
    if (test_slices_require_admission() != 0) return 1;
    if (test_yields_count_only_pump_relations() != 0) return 1;
    if (test_resource_overlaps_are_pinned() != 0) return 1;
    puts("frame pumps: PASS");
    return 0;
}
