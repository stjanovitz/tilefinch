#include "tilefinch/psp_lifecycle.h"

#include <stdio.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return false; \
    } \
} while (0)

static bool test_ordered_coalesced_suspend_resume(void)
{
    PspLifecycle lifecycle;
    psp_lifecycle_init(&lifecycle);
    CHECK(psp_lifecycle_state(&lifecycle) == PSP_LIFECYCLE_RUNNING);
    CHECK(psp_lifecycle_presentation_allowed(&lifecycle));

    psp_lifecycle_notify_suspend(&lifecycle);
    psp_lifecycle_notify_resume(&lifecycle);
    psp_lifecycle_notify_resume(&lifecycle);
    CHECK(!psp_lifecycle_presentation_allowed(&lifecycle));
    CHECK(psp_lifecycle_poll(&lifecycle)
          == PSP_LIFECYCLE_ACTION_QUIESCE);
    CHECK(psp_lifecycle_poll(&lifecycle) == PSP_LIFECYCLE_ACTION_NONE);
    psp_lifecycle_complete_quiesce(&lifecycle);
    CHECK(psp_lifecycle_state(&lifecycle) == PSP_LIFECYCLE_SUSPENDED);
    CHECK(psp_lifecycle_poll(&lifecycle)
          == PSP_LIFECYCLE_ACTION_RECOVER);
    psp_lifecycle_complete_recovery(&lifecycle);
    CHECK(psp_lifecycle_state(&lifecycle) == PSP_LIFECYCLE_RUNNING);
    CHECK(psp_lifecycle_presentation_allowed(&lifecycle));
    CHECK(lifecycle.quiesce_count == 1);
    CHECK(lifecycle.recovery_count == 1);
    CHECK(psp_lifecycle_poll(&lifecycle) == PSP_LIFECYCLE_ACTION_NONE);
    return true;
}

static bool test_repeated_cycle_and_suspend_priority(void)
{
    PspLifecycle lifecycle;
    psp_lifecycle_init(&lifecycle);
    psp_lifecycle_notify_resume(&lifecycle);
    CHECK(psp_lifecycle_poll(&lifecycle) == PSP_LIFECYCLE_ACTION_NONE);

    psp_lifecycle_notify_suspend(&lifecycle);
    CHECK(psp_lifecycle_poll(&lifecycle)
          == PSP_LIFECYCLE_ACTION_QUIESCE);
    psp_lifecycle_complete_quiesce(&lifecycle);
    psp_lifecycle_notify_resume(&lifecycle);
    CHECK(psp_lifecycle_poll(&lifecycle)
          == PSP_LIFECYCLE_ACTION_RECOVER);

    /* A second sleep before recovery completes must quiesce again first. */
    psp_lifecycle_notify_suspend(&lifecycle);
    psp_lifecycle_notify_resume(&lifecycle);
    CHECK(psp_lifecycle_poll(&lifecycle)
          == PSP_LIFECYCLE_ACTION_QUIESCE);
    psp_lifecycle_complete_quiesce(&lifecycle);
    CHECK(psp_lifecycle_poll(&lifecycle)
          == PSP_LIFECYCLE_ACTION_RECOVER);
    psp_lifecycle_complete_recovery(&lifecycle);
    CHECK(lifecycle.quiesce_count == 2);
    CHECK(lifecycle.recovery_count == 2);
    CHECK(psp_lifecycle_presentation_allowed(&lifecycle));
    return true;
}

int main(void)
{
    if (!test_ordered_coalesced_suspend_resume()
        || !test_repeated_cycle_and_suspend_priority()) return 1;
    puts("psp-lifecycle-tests: ok");
    return 0;
}
