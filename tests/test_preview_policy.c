#include "tilefinch/preview_policy.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do {                                              \
    if (!(condition)) {                                                    \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n",                    \
                __FILE__, __LINE__, #condition);                           \
        return 1;                                                          \
    }                                                                      \
} while (0)

enum { VIEWPORT = 272, BASE = 408 };

static PreviewPolicyInput input_at(size_t bytes, size_t elements, int reader)
{
    return (PreviewPolicyInput) {
        .parsed_bytes = bytes,
        .closed_content_elements = elements,
        .reader_y = reader,
        .viewport_height = VIEWPORT,
        .base_limit_y = BASE
    };
}

/* Runs one attempt through the policy as the stream does and paints it. */
static PreviewDecision attempt(PreviewPolicy *policy, PreviewPolicyInput *input,
                               int content_end, bool truncated, bool filled)
{
    preview_policy_observe(policy, input);
    PreviewDecision decision = preview_policy_decide(policy, input);
    if (decision.action == PREVIEW_ACTION_NONE) return decision;
    preview_policy_note_check(policy, &decision);
    preview_policy_attempt_started(policy, &decision, input->parsed_bytes);
    PreviewPaintResult result = {
        .retained = true, .content_end_y = content_end,
        .truncated = truncated, .first_screen_filled = filled
    };
    int previous = input->live ? input->live_content_end_y : 0;
    preview_policy_painted(policy, &decision, &result, previous);
    input->live = true;
    input->live_truncated = truncated;
    input->live_content_end_y = content_end;
    input->live_closed_elements = input->closed_content_elements;
    return decision;
}

int main(void)
{
    PreviewPolicy policy;

    /* First paint, then first-screen refreshes on 16 KiB growth, at most
       three, never shallower than the preview on screen. */
    preview_policy_init(&policy);
    CHECK(preview_policy_awaiting_first(&policy));
    PreviewPolicyInput in = input_at(10000, 60, 0);
    PreviewDecision d = attempt(&policy, &in, 300, true, false);
    CHECK(d.action == PREVIEW_ACTION_FIRST && d.limit_y == BASE
          && policy.phase == PREVIEW_PHASE_FIRST_SCREEN
          && !preview_policy_awaiting_first(&policy));
    in.parsed_bytes = 20000;
    CHECK(preview_policy_decide(&policy, &in).action == PREVIEW_ACTION_NONE);
    in.parsed_bytes = 10000 + PREVIEW_POLICY_REFRESH_BYTES;
    d = attempt(&policy, &in, 500, true, false);
    CHECK(d.action == PREVIEW_ACTION_REFRESH && d.limit_y == 3 * VIEWPORT
          && policy.refreshes == 1);
    in.parsed_bytes += PREVIEW_POLICY_REFRESH_BYTES;
    in.live_content_end_y = 900;
    d = attempt(&policy, &in, 920, true, false);
    CHECK(d.action == PREVIEW_ACTION_REFRESH && d.limit_y == 900);
    in.parsed_bytes += PREVIEW_POLICY_REFRESH_BYTES;
    CHECK(attempt(&policy, &in, 920, true, false).action
          == PREVIEW_ACTION_REFRESH);
    /* Out of refreshes: settled. */
    CHECK(policy.phase == PREVIEW_PHASE_SETTLED);
    in.parsed_bytes += PREVIEW_POLICY_REFRESH_BYTES;
    d = preview_policy_decide(&policy, &in);
    CHECK(d.action == PREVIEW_ACTION_NONE && strcmp(d.reason, "settled") == 0);

    /* A filled first screen settles at once. */
    preview_policy_init(&policy);
    in = input_at(10000, 60, 0);
    CHECK(attempt(&policy, &in, 408, true, true).action
          == PREVIEW_ACTION_FIRST
          && policy.phase == PREVIEW_PHASE_SETTLED);

    /* The reader scrolls: followed. A truncated preview extends at once,
       through the screens after theirs and at least twice as deep. */
    in.reader_y = 200;
    d = attempt(&policy, &in, 1200, true, true);
    CHECK(policy.phase == PREVIEW_PHASE_FOLLOWING
          && d.action == PREVIEW_ACTION_EXTEND
          && d.limit_y == 200 + 3 * VIEWPORT && policy.extensions == 1);
    /* Far enough ahead: nothing. */
    d = preview_policy_decide(&policy, &in);
    CHECK(d.action == PREVIEW_ACTION_NONE && strcmp(d.reason, "ahead") == 0);
    /* Within two screens of the end: deeper, reader + 3 screens vs twice
       the content, whichever is more. */
    in.reader_y = 700;
    d = attempt(&policy, &in, 2400, true, true);
    CHECK(d.action == PREVIEW_ACTION_EXTEND && d.limit_y == 2 * 1200);
    in.reader_y = 2300;
    d = preview_policy_decide(&policy, &in);
    CHECK(d.action == PREVIEW_ACTION_EXTEND
          && d.limit_y == 2 * 2400);

    /* Once it covers everything parsed, bytes alone do not earn a layout:
       new rendering elements must close too. */
    preview_policy_init(&policy);
    in = input_at(10000, 60, 0);
    (void) attempt(&policy, &in, 400, false, true);
    in.reader_y = 300;
    preview_policy_observe(&policy, &in);
    in.parsed_bytes += 64 * 1024;
    d = preview_policy_decide(&policy, &in);
    CHECK(d.action == PREVIEW_ACTION_NONE
          && strcmp(d.reason, "no-content") == 0);
    in.closed_content_elements += PREVIEW_POLICY_EXTENSION_ELEMENTS;
    d = attempt(&policy, &in, 400, false, true);
    /* It laid out nothing new (hidden markup): the next needs twice the
       elements; one that does lay out more resets that. */
    CHECK(d.action == PREVIEW_ACTION_EXTEND
          && policy.extension_elements
                 == 2 * PREVIEW_POLICY_EXTENSION_ELEMENTS);
    in.parsed_bytes += PREVIEW_POLICY_EXTENSION_BYTES;
    in.closed_content_elements += PREVIEW_POLICY_EXTENSION_ELEMENTS;
    CHECK(preview_policy_decide(&policy, &in).action
          == PREVIEW_ACTION_NONE);
    in.closed_content_elements += PREVIEW_POLICY_EXTENSION_ELEMENTS;
    CHECK(attempt(&policy, &in, 900, false, true).action
              == PREVIEW_ACTION_EXTEND
          && policy.extension_elements == PREVIEW_POLICY_EXTENSION_ELEMENTS);

    /* The preview on screen was dropped (a stylesheet rebuild): following
       rebuilds once more body is here. */
    in.live = false;
    in.parsed_bytes += 1024;
    CHECK(preview_policy_decide(&policy, &in).action == PREVIEW_ACTION_NONE);
    in.parsed_bytes += PREVIEW_POLICY_EXTENSION_BYTES;
    d = preview_policy_decide(&policy, &in);
    CHECK(d.action == PREVIEW_ACTION_EXTEND
          && d.limit_y == in.reader_y + 3 * VIEWPORT);

    /* A costly gate refused the extension (the stylesheet could not
       follow): the next waits for more body, not the very next feed. */
    preview_policy_refused(&policy, &d, in.parsed_bytes);
    d = preview_policy_decide(&policy, &in);
    CHECK(d.action == PREVIEW_ACTION_NONE
          && strcmp(d.reason, "refused") == 0);
    in.parsed_bytes += PREVIEW_POLICY_EXTENSION_BYTES;
    d = preview_policy_decide(&policy, &in);
    CHECK(d.action == PREVIEW_ACTION_EXTEND);
    preview_policy_attempt_started(&policy, &d, in.parsed_bytes);
    preview_policy_failed(&policy);
    CHECK(!policy.refused);

    /* Extensions are bounded per navigation. */
    policy.extensions = PREVIEW_POLICY_EXTENSION_LIMIT;
    d = preview_policy_decide(&policy, &in);
    CHECK(d.action == PREVIEW_ACTION_NONE
          && strcmp(d.reason, "extensions") == 0);

    /* A failed first attempt leaves it available; repeated checks without
       new input are capped, and growth reopens them. */
    preview_policy_init(&policy);
    in = input_at(10000, 60, 0);
    for (unsigned i = 0; i < PREVIEW_POLICY_CHECK_LIMIT; i++) {
        d = preview_policy_decide(&policy, &in);
        CHECK(d.action == PREVIEW_ACTION_FIRST);
        preview_policy_note_check(&policy, &d);
        preview_policy_attempt_started(&policy, &d, in.parsed_bytes);
        CHECK(preview_policy_decide(&policy, &in).action
              == PREVIEW_ACTION_NONE);
        preview_policy_failed(&policy);
        CHECK(preview_policy_awaiting_first(&policy));
    }
    d = preview_policy_decide(&policy, &in);
    CHECK(d.action == PREVIEW_ACTION_NONE && strcmp(d.reason, "checks") == 0);
    preview_policy_reopen_checks(&policy);
    CHECK(preview_policy_decide(&policy, &in).action == PREVIEW_ACTION_FIRST);
    policy.checks = PREVIEW_POLICY_CHECK_LIMIT;
    in.parsed_bytes += PREVIEW_POLICY_REFRESH_BYTES;
    d = preview_policy_decide(&policy, &in);
    CHECK(d.action == PREVIEW_ACTION_FIRST && d.fresh);

    /* Given up under memory pressure: nothing more, ever. */
    preview_policy_abandon(&policy);
    CHECK(!preview_policy_awaiting_first(&policy));
    in.reader_y = 5000;
    preview_policy_observe(&policy, &in);
    d = preview_policy_decide(&policy, &in);
    CHECK(policy.phase == PREVIEW_PHASE_ABANDONED
          && d.action == PREVIEW_ACTION_NONE
          && strcmp(d.reason, "abandoned") == 0);

    puts("preview-policy-tests status=PASS");
    return 0;
}
