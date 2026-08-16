#include "tilefinch/psp_boot_order.h"

#include <stdio.h>
#include <string.h>

PspBootPlan psp_boot_plan(const PspBootInputs *inputs)
{
    PspBootPlan plan = {
        .surface = PSP_BOOT_SURFACE_ENGINE_FIRST,
        .deferred_navigation = false
    };
    if (inputs == NULL) return plan;
    /*
     * Any path that requires a specific initial document keeps the old order.
     * Passive automation is filtered before this function so it can exercise
     * native HOME without changing the URL/trace guarantees.
     */
    if (inputs->url_overridden || inputs->trace_replay
        || inputs->validation_mode) {
        return plan;
    }
    plan.surface = PSP_BOOT_SURFACE_NATIVE_HOME;
    /*
     * Both of these used to block the first frame on a network navigation.
     * They are still honoured, just from behind the surface instead of in
     * front of it. Restoring a page wins over a custom homepage for the same
     * reason it does today: it is the more specific request.
     */
    plan.deferred_navigation =
        inputs->restore_last_page || inputs->custom_homepage;
    return plan;
}

void psp_boot_queue_init(PspBootQueue *queue)
{
    if (queue == NULL) return;
    memset(queue, 0, sizeof(*queue));
}

bool psp_boot_queue_request(PspBootQueue *queue, const char *url)
{
    if (queue == NULL || url == NULL || url[0] == '\0') return false;
    if (queue->ready) {
        queue->pending = false;
        queue->url[0] = '\0';
        return true;
    }
    snprintf(queue->url, sizeof(queue->url), "%s", url);
    queue->pending = true;
    return false;
}

void psp_boot_queue_set_ready(PspBootQueue *queue, bool ready)
{
    if (queue == NULL) return;
    queue->ready = ready;
}

const char *psp_boot_queue_take(PspBootQueue *queue)
{
    if (queue == NULL || !queue->ready || !queue->pending) return NULL;
    queue->pending = false;
    return queue->url;
}
