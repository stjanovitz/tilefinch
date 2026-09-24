#ifndef TILEFINCH_PSP_UPDATE_CHECK_H
#define TILEFINCH_PSP_UPDATE_CHECK_H

#include <stdbool.h>

/*
 * The optional background update-available check, one per boot.
 *
 *   DISARMED --(eligible)--> ARMED --(idle, connected frame)--> RUNNING
 *      ^                     |  ^                                  |
 *      |                pause|  |resume                  finished  |
 *      |                     v  |                                  v
 *      +---(ineligible)---- PAUSED                               SPENT
 *
 * A fired check that cannot start (no clock, rate-limited, no session) is
 * SPENT without running. "Eligible" is always the same rule: this boot may
 * run background update work (not an offline start, not a traced run, not
 * a validation run that drives updates itself) and the profile asks for a
 * check with a trusted endpoint for its channel. The caller computes the
 * second half with psp_app_update_check_configured(); the boot half is
 * latched here once. Pure data: host-tested, used by the PSP frontend.
 */

typedef enum {
    PSP_UPDATE_CHECK_DISARMED = 0,
    PSP_UPDATE_CHECK_ARMED,
    PSP_UPDATE_CHECK_PAUSED,    /* armed, held while Wi-Fi sign-in runs */
    PSP_UPDATE_CHECK_RUNNING,   /* the update session is checking */
    PSP_UPDATE_CHECK_SPENT      /* fired this boot */
} PspUpdateCheckState;

typedef enum {
    PSP_UPDATE_CHECK_STARTED,
    PSP_UPDATE_CHECK_RATE_LIMITED,
    PSP_UPDATE_CHECK_NOT_STARTED
} PspUpdateCheckOutcome;

typedef struct {
    PspUpdateCheckState state;
    bool background_allowed;  /* the boot half of eligibility */
    bool toasted;             /* the "update ready" notice was shown */
    unsigned attempts;
    unsigned ratelimited;
    unsigned completed;
    unsigned available;
} PspUpdateCheck;

static inline void psp_update_check_init(PspUpdateCheck *check,
                                         bool background_allowed,
                                         bool configured)
{
    *check = (PspUpdateCheck) {
        .state = background_allowed && configured
            ? PSP_UPDATE_CHECK_ARMED : PSP_UPDATE_CHECK_DISARMED,
        .background_allowed = background_allowed
    };
}

static inline bool psp_update_check_armed(const PspUpdateCheck *check)
{
    return check->state == PSP_UPDATE_CHECK_ARMED;
}

static inline bool psp_update_check_running(const PspUpdateCheck *check)
{
    return check->state == PSP_UPDATE_CHECK_RUNNING;
}

/* Settings changed eligibility (the check toggled, the channel changed). A
   running check completes; a paused one stays paused while still eligible.
   A spent check re-arms, so turning the check back on retries this boot. */
static inline void psp_update_check_rearm(PspUpdateCheck *check,
                                          bool configured)
{
    if (check->state == PSP_UPDATE_CHECK_RUNNING) return;
    if (!(check->background_allowed && configured))
        check->state = PSP_UPDATE_CHECK_DISARMED;
    else if (check->state != PSP_UPDATE_CHECK_PAUSED)
        check->state = PSP_UPDATE_CHECK_ARMED;
}

/* The update session was destroyed (its endpoint or channel was replaced):
   a running check is abandoned, then eligibility is re-evaluated. */
static inline void psp_update_check_reset(PspUpdateCheck *check,
                                          bool configured)
{
    if (check->state == PSP_UPDATE_CHECK_RUNNING)
        check->state = PSP_UPDATE_CHECK_SPENT;
    psp_update_check_rearm(check, configured);
}

/* The armed check reached its idle, connected frame. */
static inline void psp_update_check_fired(PspUpdateCheck *check,
                                          PspUpdateCheckOutcome outcome)
{
    if (check->state != PSP_UPDATE_CHECK_ARMED) return;
    if (outcome == PSP_UPDATE_CHECK_STARTED) {
        check->state = PSP_UPDATE_CHECK_RUNNING;
        check->attempts++;
        return;
    }
    if (outcome == PSP_UPDATE_CHECK_RATE_LIMITED) check->ratelimited++;
    check->state = PSP_UPDATE_CHECK_SPENT;
}

/* The update session stopped. Returns whether a running check ended, which
   is when its result should be recorded. */
static inline bool psp_update_check_finished(PspUpdateCheck *check)
{
    if (check->state != PSP_UPDATE_CHECK_RUNNING) return false;
    check->state = PSP_UPDATE_CHECK_SPENT;
    return true;
}

/* Wi-Fi sign-in owns the session: hold an armed check until it closes. */
static inline void psp_update_check_pause(PspUpdateCheck *check)
{
    if (check->state == PSP_UPDATE_CHECK_ARMED)
        check->state = PSP_UPDATE_CHECK_PAUSED;
}

static inline void psp_update_check_resume(PspUpdateCheck *check)
{
    if (check->state == PSP_UPDATE_CHECK_PAUSED)
        check->state = PSP_UPDATE_CHECK_ARMED;
}

#endif
