#include "tilefinch/psp_update_check.h"

#include <stdio.h>

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "UPDATE CHECK failed at %s:%d: %s\n",              \
                __FILE__, __LINE__, #condition);                             \
        return 1;                                                            \
    }                                                                        \
} while (0)

static int test_boot_eligibility(void)
{
    PspUpdateCheck check;
    psp_update_check_init(&check, true, true);
    CHECK(psp_update_check_armed(&check));
    /* An offline, traced, or validation-driven boot never arms, and no
       later settings change can arm it. */
    psp_update_check_init(&check, false, true);
    CHECK(check.state == PSP_UPDATE_CHECK_DISARMED);
    psp_update_check_rearm(&check, true);
    psp_update_check_reset(&check, true);
    CHECK(check.state == PSP_UPDATE_CHECK_DISARMED);
    psp_update_check_init(&check, true, false);
    CHECK(check.state == PSP_UPDATE_CHECK_DISARMED);
    psp_update_check_rearm(&check, true);
    CHECK(psp_update_check_armed(&check));
    return 0;
}

static int test_fire_and_finish(void)
{
    PspUpdateCheck check;
    psp_update_check_init(&check, true, true);
    psp_update_check_fired(&check, PSP_UPDATE_CHECK_STARTED);
    CHECK(psp_update_check_running(&check) && check.attempts == 1);
    /* A running check completes through a settings change. */
    psp_update_check_rearm(&check, false);
    CHECK(psp_update_check_running(&check));
    CHECK(psp_update_check_finished(&check));
    CHECK(check.state == PSP_UPDATE_CHECK_SPENT);
    CHECK(!psp_update_check_finished(&check));
    /* Firing is only meaningful while armed. */
    psp_update_check_fired(&check, PSP_UPDATE_CHECK_STARTED);
    CHECK(check.state == PSP_UPDATE_CHECK_SPENT && check.attempts == 1);

    psp_update_check_init(&check, true, true);
    psp_update_check_fired(&check, PSP_UPDATE_CHECK_RATE_LIMITED);
    CHECK(check.state == PSP_UPDATE_CHECK_SPENT && check.ratelimited == 1);
    psp_update_check_init(&check, true, true);
    psp_update_check_fired(&check, PSP_UPDATE_CHECK_NOT_STARTED);
    CHECK(check.state == PSP_UPDATE_CHECK_SPENT && check.attempts == 0);
    /* Turning the check back on retries it this boot. */
    psp_update_check_rearm(&check, true);
    CHECK(psp_update_check_armed(&check));
    return 0;
}

static int test_session_reset_abandons_running_check(void)
{
    PspUpdateCheck check;
    psp_update_check_init(&check, true, true);
    psp_update_check_fired(&check, PSP_UPDATE_CHECK_STARTED);
    psp_update_check_reset(&check, true);
    CHECK(psp_update_check_armed(&check));
    CHECK(!psp_update_check_finished(&check));
    psp_update_check_fired(&check, PSP_UPDATE_CHECK_STARTED);
    psp_update_check_reset(&check, false);
    CHECK(check.state == PSP_UPDATE_CHECK_DISARMED);
    return 0;
}

static int test_sign_in_pause(void)
{
    PspUpdateCheck check;
    psp_update_check_init(&check, true, true);
    psp_update_check_pause(&check);
    CHECK(check.state == PSP_UPDATE_CHECK_PAUSED);
    /* Still eligible: a settings change keeps it paused. */
    psp_update_check_rearm(&check, true);
    CHECK(check.state == PSP_UPDATE_CHECK_PAUSED);
    psp_update_check_resume(&check);
    CHECK(psp_update_check_armed(&check));
    /* Turned off during sign-in: closing sign-in must not re-arm it. */
    psp_update_check_pause(&check);
    psp_update_check_rearm(&check, false);
    psp_update_check_resume(&check);
    CHECK(check.state == PSP_UPDATE_CHECK_DISARMED);
    /* Only an armed check pauses. */
    psp_update_check_init(&check, true, true);
    psp_update_check_fired(&check, PSP_UPDATE_CHECK_STARTED);
    psp_update_check_pause(&check);
    psp_update_check_resume(&check);
    CHECK(psp_update_check_running(&check));
    return 0;
}

int main(void)
{
    if (test_boot_eligibility() != 0 || test_fire_and_finish() != 0
        || test_session_reset_abandons_running_check() != 0
        || test_sign_in_pause() != 0) return 1;
    puts("psp-update-check-tests status=PASS");
    return 0;
}
