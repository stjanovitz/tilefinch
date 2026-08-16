#include "tilefinch/psp_boot_order.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return false; \
    } \
} while (0)

static bool test_default_boot_shows_native_home(void)
{
    PspBootInputs inputs = {0};
    PspBootPlan plan = psp_boot_plan(&inputs);
    CHECK(plan.surface == PSP_BOOT_SURFACE_NATIVE_HOME
          && !plan.deferred_navigation);

    /* A null input set is a programming error, not a licence to change the
       order the harness depends on. */
    plan = psp_boot_plan(NULL);
    CHECK(plan.surface == PSP_BOOT_SURFACE_ENGINE_FIRST
          && !plan.deferred_navigation);
    return true;
}

static bool test_document_directed_paths_keep_engine_first(void)
{
    PspBootInputs url = {.url_overridden = true};
    PspBootInputs trace = {.trace_replay = true};
    PspBootInputs validation = {.validation_mode = true};
    CHECK(psp_boot_plan(&url).surface == PSP_BOOT_SURFACE_ENGINE_FIRST);
    CHECK(psp_boot_plan(&trace).surface == PSP_BOOT_SURFACE_ENGINE_FIRST);
    CHECK(psp_boot_plan(&validation).surface
          == PSP_BOOT_SURFACE_ENGINE_FIRST);

    /* A document-directed boot never acquires a deferred navigation either:
       its first navigation is the one it was told to make. */
    PspBootInputs harness_with_restore = {
        .trace_replay = true, .restore_last_page = true,
        .custom_homepage = true
    };
    PspBootPlan plan = psp_boot_plan(&harness_with_restore);
    CHECK(plan.surface == PSP_BOOT_SURFACE_ENGINE_FIRST
          && !plan.deferred_navigation);
    return true;
}

static bool test_boot_navigations_move_behind_the_surface(void)
{
    PspBootInputs restore = {.restore_last_page = true};
    PspBootPlan plan = psp_boot_plan(&restore);
    CHECK(plan.surface == PSP_BOOT_SURFACE_NATIVE_HOME
          && plan.deferred_navigation);

    PspBootInputs homepage = {.custom_homepage = true};
    plan = psp_boot_plan(&homepage);
    CHECK(plan.surface == PSP_BOOT_SURFACE_NATIVE_HOME
          && plan.deferred_navigation);

    PspBootInputs both = {.restore_last_page = true, .custom_homepage = true};
    plan = psp_boot_plan(&both);
    CHECK(plan.surface == PSP_BOOT_SURFACE_NATIVE_HOME
          && plan.deferred_navigation);
    return true;
}

static bool test_activation_waits_for_readiness(void)
{
    PspBootQueue queue;
    psp_boot_queue_init(&queue);
    CHECK(!queue.ready && !queue.pending);

    /* Activating before the browser is up cannot start a navigation; it is
       remembered instead of dropped. */
    CHECK(!psp_boot_queue_request(&queue, "https://example.org/one"));
    CHECK(queue.pending);
    CHECK(psp_boot_queue_take(&queue) == NULL);

    /* The most recent choice is the one the user is looking at. */
    CHECK(!psp_boot_queue_request(&queue, "https://example.org/two"));
    psp_boot_queue_set_ready(&queue, true);
    const char *taken = psp_boot_queue_take(&queue);
    CHECK(taken != NULL && strcmp(taken, "https://example.org/two") == 0);
    /* Taken once, not once per frame. */
    CHECK(psp_boot_queue_take(&queue) == NULL);

    /* Once ready, requests start immediately and leave nothing queued. */
    CHECK(psp_boot_queue_request(&queue, "https://example.org/three"));
    CHECK(!queue.pending && psp_boot_queue_take(&queue) == NULL);

    /* An empty target is not a navigation. */
    CHECK(!psp_boot_queue_request(&queue, ""));
    CHECK(!psp_boot_queue_request(&queue, NULL));
    CHECK(!queue.pending);
    return true;
}

int main(void)
{
    if (!test_default_boot_shows_native_home()
        || !test_document_directed_paths_keep_engine_first()
        || !test_boot_navigations_move_behind_the_surface()
        || !test_activation_waits_for_readiness()) return 1;
    puts("psp-boot-order-tests: ok");
    return 0;
}
