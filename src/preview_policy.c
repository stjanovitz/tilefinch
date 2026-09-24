#include "tilefinch/preview_policy.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

void preview_policy_init(PreviewPolicy *policy)
{
    if (policy == NULL) return;
    memset(policy, 0, sizeof(*policy));
    policy->extension_elements = PREVIEW_POLICY_EXTENSION_ELEMENTS;
}

void preview_policy_observe(PreviewPolicy *policy,
                            const PreviewPolicyInput *input)
{
    if (policy == NULL || input == NULL || input->reader_y <= 0) return;
    /* Once the reader has scrolled, the first screen is not what they see:
       only following them is worth a layout. */
    if (policy->phase == PREVIEW_PHASE_FIRST_SCREEN
        || policy->phase == PREVIEW_PHASE_SETTLED)
        policy->phase = PREVIEW_PHASE_FOLLOWING;
}

static int preview_policy_max(int a, int64_t b)
{
    if (b > INT_MAX) return INT_MAX;
    return b > a ? (int) b : a;
}

static size_t preview_policy_growth(const PreviewPolicy *policy,
                                    const PreviewPolicyInput *input)
{
    return input->parsed_bytes >= policy->attempt_bytes
        ? input->parsed_bytes - policy->attempt_bytes : 0;
}

static PreviewDecision preview_policy_none(const char *reason)
{
    return (PreviewDecision) { .action = PREVIEW_ACTION_NONE,
                               .reason = reason };
}

PreviewDecision preview_policy_decide(const PreviewPolicy *policy,
                                      const PreviewPolicyInput *input)
{
    if (policy == NULL || input == NULL) return preview_policy_none("input");
    if (policy->building) return preview_policy_none("building");
    size_t growth = preview_policy_growth(policy, input);
    int base = input->base_limit_y > 0 ? input->base_limit_y : 1;
    int64_t viewport = input->viewport_height > 0 ? input->viewport_height : 0;
    PreviewDecision decision = {0};
    switch (policy->phase) {
        case PREVIEW_PHASE_NONE:
            decision.action = PREVIEW_ACTION_FIRST;
            decision.limit_y = base;
            decision.fresh = growth >= PREVIEW_POLICY_REFRESH_BYTES;
            break;
        case PREVIEW_PHASE_FIRST_SCREEN:
            if (growth < PREVIEW_POLICY_REFRESH_BYTES)
                return preview_policy_none("no-growth");
            if (policy->refreshes >= PREVIEW_POLICY_REFRESH_LIMIT)
                return preview_policy_none("refreshes");
            decision.action = PREVIEW_ACTION_REFRESH;
            /* The first screen and two more, so the reader's first page
               presses land on content. */
            decision.limit_y = preview_policy_max(base, 3 * viewport);
            /* A replacement never covers less than the preview on screen. */
            if (input->live && input->live_truncated)
                decision.limit_y = preview_policy_max(
                    decision.limit_y, input->live_content_end_y);
            decision.fresh = true;
            break;
        case PREVIEW_PHASE_SETTLED:
            return preview_policy_none("settled");
        case PREVIEW_PHASE_FOLLOWING: {
            if (policy->extensions >= PREVIEW_POLICY_EXTENSION_LIMIT)
                return preview_policy_none("extensions");
            if (policy->refused
                && (input->parsed_bytes < policy->refused_bytes
                    || input->parsed_bytes - policy->refused_bytes
                           < PREVIEW_POLICY_EXTENSION_BYTES))
                return preview_policy_none("refused");
            int64_t reader = input->reader_y > 0 ? input->reader_y : 0;
            if (input->live) {
                /* Within two screens of where laid-out content ends, early
                   enough to lay out before the reader gets there. */
                if (reader + 3 * viewport <= input->live_content_end_y)
                    return preview_policy_none("ahead");
                if (!input->live_truncated) {
                    /* It covers everything parsed: wait for content. */
                    size_t closed = input->closed_content_elements
                            >= input->live_closed_elements
                        ? input->closed_content_elements
                              - input->live_closed_elements
                        : 0;
                    if (growth < PREVIEW_POLICY_EXTENSION_BYTES
                        || closed < policy->extension_elements)
                        return preview_policy_none("no-content");
                }
            } else if (growth < PREVIEW_POLICY_EXTENSION_BYTES) {
                /* The one on screen was dropped (a stylesheet rebuild);
                   rebuild once more of the body is here. */
                return preview_policy_none("no-growth");
            }
            decision.action = PREVIEW_ACTION_EXTEND;
            /* Through the screens after the reader's, and at least twice
               as deep as the last one: each preview is a layout from the
               top, so doubling keeps the total to about two layouts of
               where the reader ends up. */
            decision.limit_y = preview_policy_max(base, reader + 3 * viewport);
            if (input->live) {
                decision.limit_y = preview_policy_max(
                    decision.limit_y, 2 * (int64_t) input->live_content_end_y);
            }
            decision.fresh = true;
            break;
        }
        case PREVIEW_PHASE_ABANDONED:
        default:
            return preview_policy_none("abandoned");
    }
    if (policy->checks >= PREVIEW_POLICY_CHECK_LIMIT && !decision.fresh)
        return preview_policy_none("checks");
    return decision;
}

void preview_policy_note_check(PreviewPolicy *policy,
                               const PreviewDecision *decision)
{
    if (policy == NULL || decision == NULL) return;
    if (decision->fresh) policy->checks = 0;
    if (policy->checks != UINT_MAX) policy->checks++;
}

void preview_policy_refused(PreviewPolicy *policy,
                            const PreviewDecision *decision,
                            size_t parsed_bytes)
{
    if (policy == NULL || decision == NULL
        || decision->action != PREVIEW_ACTION_EXTEND) return;
    policy->refused = true;
    policy->refused_bytes = parsed_bytes;
}

void preview_policy_reopen_checks(PreviewPolicy *policy)
{
    if (policy != NULL) policy->checks = 0;
}

void preview_policy_attempt_started(PreviewPolicy *policy,
                                    const PreviewDecision *decision,
                                    size_t parsed_bytes)
{
    if (policy == NULL || decision == NULL) return;
    policy->building = true;
    policy->refused = false;
    policy->attempt_bytes = parsed_bytes;
    if (decision->action == PREVIEW_ACTION_EXTEND) policy->extensions++;
    else if (decision->action == PREVIEW_ACTION_REFRESH) policy->refreshes++;
}

void preview_policy_painted(PreviewPolicy *policy,
                            const PreviewDecision *decision,
                            const PreviewPaintResult *result,
                            int previous_content_end_y)
{
    if (policy == NULL || decision == NULL || result == NULL) return;
    policy->building = false;
    switch (decision->action) {
        case PREVIEW_ACTION_FIRST:
        case PREVIEW_ACTION_REFRESH:
            policy->phase = result->first_screen_filled
                    || policy->refreshes >= PREVIEW_POLICY_REFRESH_LIMIT
                ? PREVIEW_PHASE_SETTLED : PREVIEW_PHASE_FIRST_SCREEN;
            break;
        case PREVIEW_ACTION_EXTEND:
            /* One that laid out nothing new (hidden markup) makes the next
               wait for twice the elements; one that did resets that. */
            if (result->content_end_y > previous_content_end_y) {
                policy->extension_elements =
                    PREVIEW_POLICY_EXTENSION_ELEMENTS;
            } else if (policy->extension_elements
                       <= PREVIEW_POLICY_EXTENSION_ELEMENTS_LIMIT / 2u) {
                policy->extension_elements *= 2u;
            }
            break;
        case PREVIEW_ACTION_NONE:
        default:
            break;
    }
}

void preview_policy_failed(PreviewPolicy *policy)
{
    if (policy != NULL) policy->building = false;
}

void preview_policy_abandon(PreviewPolicy *policy)
{
    if (policy == NULL) return;
    policy->building = false;
    policy->phase = PREVIEW_PHASE_ABANDONED;
}

bool preview_policy_awaiting_first(const PreviewPolicy *policy)
{
    return policy != NULL && policy->phase == PREVIEW_PHASE_NONE
        && !policy->building;
}
