/* Heavy pages: how much author script a page is loading, what it would cost
   on the PSP, and, with the ASK policy, holding its big dynamic scripts
   until the user answers. See script_runtime_heavy_state() in
   include/tilefinch/js_runtime.h and the model in script_admission.h. */

#define SCRIPT_HEAVY_TEXT_VISIT_LIMIT 8192u

/* Non-space server text under <body> (document_body_visible_content): what
   a reader sees before any author script has built the page. A page larger
   than SCRIPT_HEAVY_TEXT_VISIT_LIMIT nodes is content. */
static size_t heavy_visible_text_bytes(const PocDocument *document)
{
    DocumentVisibleContent content = document_body_visible_content(
        document, SCRIPT_HEAVY_TEXT_VISIT_LIMIT);
    if (content.truncated
        && content.text_bytes < SCRIPT_HEAVY_SHELL_TEXT_BYTES)
        return SCRIPT_HEAVY_SHELL_TEXT_BYTES;
    return content.text_bytes;
}

static void heavy_measure(ScriptRuntime *runtime, ScriptHeavyState *state)
{
    DomBridge *bridge = &runtime->bridge;
    memset(state, 0, sizeof(*state));
    state->answered = bridge->heavy.answered;
    state->allowed = bridge->heavy.allowed;
    state->script_bytes = bridge->heavy.script_bytes;
    state->restored_bytes = bridge->heavy.restored_bytes;
    state->disk_restored_bytes = bridge->heavy.disk_restored_bytes;
    state->waiting_bytes = bridge->heavy.waiting_bytes;
    state->waiting_scripts = bridge->heavy.waiting_scripts;
    state->refused_scripts = bridge->heavy.refused_scripts;
    state->oversized_scripts = bridge->heavy.oversized_scripts;
    state->oversized_bytes = bridge->heavy.oversized_bytes;
    state->memory_refusals = bridge->heavy.memory_refusals;
    js_rt_saturating_add_size(&state->memory_refusals,
                              script_runtime_heap_rejections(runtime));
    js_rt_saturating_add_size(&state->memory_refusals,
                              runtime->result.dynamic_script_memory_failures);
    state->largest_unit_bytes = bridge->heavy.largest_unit_bytes;
    state->waiting = bridge->heavy.waiting_scripts != 0
        && !bridge->heavy.answered;
    /* Restored bytecode skips the compile but still runs; bytecode read
       from the Memory Stick tier also pays the read. */
    size_t total = state->script_bytes;
    size_t restored = bridge->heavy.restored_bytes < total
        ? bridge->heavy.restored_bytes : total;
    size_t from_disk = bridge->heavy.disk_restored_bytes < restored
        ? bridge->heavy.disk_restored_bytes : restored;
    uint64_t estimate = (uint64_t) script_admission_start_ms(total - restored)
        + script_admission_restore_ms(restored)
        + script_admission_disk_read_ms(from_disk)
        + script_admission_run_ms(restored);
    state->estimate_ms = estimate > UINT32_MAX
        ? UINT32_MAX : (uint32_t) estimate;
    state->waiting_ms = script_admission_start_ms(state->waiting_bytes);
    state->memory_needed =
        script_admission_run_memory(state->waiting_bytes);
    state->memory_free = script_runtime_heap_admission_available(runtime);
    /* What the server sent, measured once: at commit, before deferred and
       dynamic author scripts build anything (script_runtime_heavy_commit),
       or else the first time anyone asks. */
    if (!bridge->heavy.visible_text_known) {
        bridge->heavy.visible_text_bytes =
            heavy_visible_text_bytes(bridge->document);
        bridge->heavy.visible_text_known = true;
    }
    state->visible_text_bytes = bridge->heavy.visible_text_bytes;
    /* A class only ever escalates: a heavy page stays heavy while its
       scripts run, and any page whose waiting scripts cannot fit becomes
       OVER, a heavy shell included (m.vk.ru turns heavy on its first
       wave, before its 3.0 MiB chunk arrives and proves too large). */
    if (bridge->heavy.page_class != SCRIPT_HEAVY_CLASS_OVER) {
        ScriptHeavyClass measured = script_admission_classify(
            state->visible_text_bytes, state->estimate_ms,
            state->waiting_bytes == 0 ? 0 : state->largest_unit_bytes,
            state->memory_needed, state->memory_free);
        /* A script already refused as too large proves the same. */
        if (bridge->heavy.oversized_scripts != 0)
            measured = SCRIPT_HEAVY_CLASS_OVER;
        if (measured == SCRIPT_HEAVY_CLASS_OVER
            || (bridge->heavy.page_class == SCRIPT_HEAVY_CLASS_NONE
                && measured != SCRIPT_HEAVY_CLASS_NONE))
            bridge->heavy.page_class = measured;
    }
    state->page_class = bridge->heavy.page_class;
}

void script_runtime_heavy_commit(ScriptRuntime *runtime)
{
    if (runtime == NULL || runtime->bridge.heavy.visible_text_known) return;
    runtime->bridge.heavy.visible_text_bytes =
        heavy_visible_text_bytes(runtime->bridge.document);
    runtime->bridge.heavy.visible_text_known = true;
}

void script_runtime_arm_memory_rescue(ScriptRuntime *runtime)
{
    if (runtime == NULL) return;
    runtime->memory_rescue_armed = true;
}

/* From the heap growth hook as it refuses: the realm is out of memory but
   has not yet run any JavaScript after the refusal. Reads the DOM and
   allocates only from the page Budget, never from the QuickJS heap. */
void js_rt_memory_rescue_capture(ScriptRuntime *runtime)
{
    if (!runtime->memory_rescue_armed
        || runtime->memory_rescue.markup != NULL
        || runtime->memory_rescue_captures >= SCRIPT_MEMORY_RESCUE_CAPTURES
        || runtime->bridge.document == NULL) return;
    runtime->memory_rescue_captures++;
    (void) document_body_snapshot_capture_without_scripts(
        runtime->bridge.document, &runtime->memory_rescue);
}

bool script_runtime_take_memory_rescue(ScriptRuntime *runtime,
                                       DocumentBodySnapshot *snapshot)
{
    if (snapshot != NULL) *snapshot = (DocumentBodySnapshot) {0};
    if (runtime == NULL || runtime->memory_rescue.markup == NULL)
        return false;
    if (snapshot == NULL) {
        document_body_snapshot_destroy(&runtime->memory_rescue);
        return false;
    }
    *snapshot = runtime->memory_rescue;
    runtime->memory_rescue = (DocumentBodySnapshot) {0};
    return true;
}

void script_runtime_set_heavy_policy(ScriptRuntime *runtime,
                                     ScriptHeavyPolicy policy)
{
    if (runtime == NULL) return;
    runtime->bridge.heavy.policy = policy;
}

void script_runtime_note_script_source(ScriptRuntime *runtime, size_t bytes,
                                       size_t largest_unit_bytes)
{
    if (runtime == NULL) return;
    DomBridge *bridge = &runtime->bridge;
    bridge->heavy.script_bytes = bytes > SIZE_MAX - bridge->heavy.script_bytes
        ? SIZE_MAX : bridge->heavy.script_bytes + bytes;
    if (largest_unit_bytes > bridge->heavy.largest_unit_bytes)
        bridge->heavy.largest_unit_bytes = largest_unit_bytes;
}

void script_runtime_note_oversized_script(ScriptRuntime *runtime,
                                          size_t bytes)
{
    if (runtime == NULL) return;
    js_rt_saturating_add_size(&runtime->bridge.heavy.oversized_scripts, 1);
    js_rt_saturating_add_size(&runtime->bridge.heavy.oversized_bytes, bytes);
}

void script_runtime_note_memory_refusal(ScriptRuntime *runtime)
{
    if (runtime == NULL) return;
    js_rt_saturating_add_size(&runtime->bridge.heavy.memory_refusals, 1);
}

bool script_heavy_state_low_on_memory(const ScriptHeavyState *state)
{
    return state != NULL && state->memory_refusals != 0
        && state->memory_free < SCRIPT_HEAVY_LOW_MEMORY_BYTES;
}

void js_rt_heavy_note_restored(ScriptRuntime *runtime, size_t source_bytes,
                               bool from_disk)
{
    if (runtime == NULL) return;
    DomBridge *bridge = &runtime->bridge;
    js_rt_saturating_add_size(&bridge->heavy.restored_bytes, source_bytes);
    if (from_disk) {
        js_rt_saturating_add_size(&bridge->heavy.disk_restored_bytes,
                                  source_bytes);
    }
}

bool script_runtime_heavy_state(ScriptRuntime *runtime,
                                ScriptHeavyState *state)
{
    if (runtime == NULL || state == NULL) return false;
    heavy_measure(runtime, state);
    return true;
}

bool script_runtime_answer_heavy(ScriptRuntime *runtime, bool run)
{
    if (runtime == NULL) return false;
    DomBridge *bridge = &runtime->bridge;
    bool waiting = bridge->heavy.waiting_scripts != 0
        && !bridge->heavy.answered;
    bridge->heavy.answered = true;
    bridge->heavy.allowed = run;
    if (bridge->heavy.frame_gate) {
        bridge->heavy.frame_held = false;
        bridge->heavy.frame_held_bytes = 0;
    }
    return waiting;
}

bool script_runtime_heavy_frames_ask(const ScriptRuntime *top)
{
    return top != NULL
        && top->bridge.heavy.policy == SCRIPT_HEAVY_POLICY_ASK
        && !top->bridge.heavy.frames_answered;
}

void script_runtime_answer_heavy_frames(ScriptRuntime *top, bool run)
{
    if (top == NULL) return;
    top->bridge.heavy.frames_answered = true;
    top->bridge.heavy.frames_allowed = run;
}

void script_runtime_heavy_frame_begin(ScriptRuntime *frame,
                                      const ScriptRuntime *top,
                                      bool can_hold)
{
    if (frame == NULL) return;
    DomBridge *bridge = &frame->bridge;
    bridge->heavy.frame_gate = true;
    bridge->heavy.frame_can_hold = can_hold;
    if (top == NULL) return;
    /* The page's answer for its frames, never its own. */
    bridge->heavy.policy = top->bridge.heavy.policy;
    bridge->heavy.answered = top->bridge.heavy.frames_answered;
    bridge->heavy.allowed = top->bridge.heavy.frames_allowed;
}

ScriptHeavyFrameVerdict script_runtime_heavy_frame_verdict(
    ScriptRuntime *runtime, size_t bytes)
{
    if (runtime == NULL) return SCRIPT_HEAVY_FRAME_RUN;
    DomBridge *bridge = &runtime->bridge;
    if (!bridge->heavy.frame_gate || bytes < SCRIPT_HEAVY_UNIT_BYTES
        || bridge->heavy.policy == SCRIPT_HEAVY_POLICY_RUN
        || (bridge->heavy.answered && bridge->heavy.allowed))
        return SCRIPT_HEAVY_FRAME_RUN;
    if (bridge->heavy.answered
        || bridge->heavy.policy == SCRIPT_HEAVY_POLICY_REFUSE) {
        bridge->heavy.answered = true;
        bridge->heavy.allowed = false;
        bridge->heavy.refused_scripts++;
        return SCRIPT_HEAVY_FRAME_REFUSE;
    }
    /* No kept response or completed-script record: continuing is safer
       than rebuilding a realm and repeating external effects. */
    if (!bridge->heavy.frame_can_hold) return SCRIPT_HEAVY_FRAME_RUN;
    bridge->heavy.frame_held = true;
    bridge->heavy.frame_held_bytes = bytes;
    return SCRIPT_HEAVY_FRAME_HOLD;
}

bool script_runtime_heavy_frame_replay_safe(const ScriptRuntime *runtime)
{
    return runtime != NULL
        && runtime->bridge.heavy.frame_completed_count == 0;
}

bool script_runtime_heavy_frame_rollback_quota(ScriptRuntime *runtime,
                                               size_t slots, size_t bytes)
{
    if (runtime == NULL) return false;
    DomBridge *bridge = &runtime->bridge;
    if (!bridge->heavy.frame_gate || !bridge->heavy.frame_held
        || slots > bridge->script_quota_count
        || bytes > bridge->script_quota_bytes) {
        bridge->heavy.frame_held = false;
        bridge->heavy.frame_held_bytes = 0;
        bridge->heavy.frame_can_hold = false;
        return false;
    }
    bridge->script_quota_count -= slots;
    bridge->script_quota_bytes -= bytes;
    return true;
}

bool script_runtime_frame_script_was_completed(const ScriptRuntime *runtime,
                                               long handle)
{
    if (runtime == NULL || handle == 0) return false;
    const DomBridge *bridge = &runtime->bridge;
    for (size_t i = 0; i < bridge->heavy.frame_completed_count; i++) {
        if (bridge->heavy.frame_completed_scripts[i] == handle) return true;
    }
    return false;
}

void script_runtime_frame_script_completed(ScriptRuntime *runtime,
                                           long handle)
{
    if (runtime == NULL || handle == 0
        || !runtime->bridge.heavy.frame_gate
        || !runtime->bridge.heavy.frame_can_hold
        || runtime->bridge.heavy.policy != SCRIPT_HEAVY_POLICY_ASK
        || runtime->bridge.heavy.answered) return;
    DomBridge *bridge = &runtime->bridge;
    if (script_runtime_frame_script_was_completed(runtime, handle)) return;
    enum { FRAME_COMPLETED_LIMIT = 256 };
    if (bridge->heavy.frame_completed_scripts == NULL) {
        bridge->heavy.frame_completed_scripts = budget_malloc(
            runtime->budget, FRAME_COMPLETED_LIMIT * sizeof(long));
        if (bridge->heavy.frame_completed_scripts == NULL) {
            bridge->heavy.frame_can_hold = false;
            return;
        }
    }
    if (bridge->heavy.frame_completed_count == FRAME_COMPLETED_LIMIT) {
        bridge->heavy.frame_can_hold = false;
        return;
    }
    bridge->heavy.frame_completed_scripts[
        bridge->heavy.frame_completed_count++] = handle;
}

void script_runtime_heavy_frame_counts(const ScriptRuntime *runtime,
                                       size_t *waiting_scripts,
                                       size_t *waiting_bytes,
                                       size_t *refused_scripts)
{
    bool waiting = runtime != NULL
        && runtime->bridge.heavy.waiting_scripts != 0
        && !runtime->bridge.heavy.answered;
    *waiting_scripts = waiting ? runtime->bridge.heavy.waiting_scripts : 0;
    *waiting_bytes = waiting ? runtime->bridge.heavy.waiting_bytes : 0;
    *refused_scripts = runtime == NULL ? 0
        : runtime->bridge.heavy.refused_scripts;
}

bool script_runtime_heavy_frame_held(const ScriptRuntime *runtime,
                                     size_t *bytes)
{
    bool held = runtime != NULL && runtime->bridge.heavy.frame_held;
    if (bytes != NULL)
        *bytes = held ? runtime->bridge.heavy.frame_held_bytes : 0;
    return held;
}

/* Called for a READY dynamic script before it runs. True holds it (it stays
   READY for a later turn). A refused script is marked failed, so it fires
   an error event like a script that did not load. */
bool js_rt_heavy_gate_holds(ScriptRuntime *runtime, ScriptDynamicTask *task)
{
    DomBridge *bridge = &runtime->bridge;
    if (task == NULL || !task->success) return false;
    if (task->source_length < SCRIPT_HEAVY_UNIT_BYTES) {
        if (!task->heavy_counted) {
            task->heavy_counted = true;
            script_runtime_note_script_source(
                runtime, task->source_length, task->source_length);
        }
        return false;
    }
    if (!task->heavy_counted) {
        task->heavy_counted = true;
        script_runtime_note_script_source(
            runtime, task->source_length, task->source_length);
        bridge->heavy.waiting_bytes += task->source_length;
        bridge->heavy.waiting_scripts++;
    }
    bool release = false;
    if (bridge->heavy.policy == SCRIPT_HEAVY_POLICY_RUN
        || (bridge->heavy.answered && bridge->heavy.allowed)) {
        release = true;
    } else if (bridge->heavy.answered && !bridge->heavy.allowed) {
        task->success = false;
        bridge->heavy.refused_scripts++;
        release = true;
    } else {
        /* A child frame's big script is asked about by its size alone. */
        bool ask = bridge->heavy.frame_gate;
        if (!ask) {
            ScriptHeavyState state;
            heavy_measure(runtime, &state);
            ask = state.page_class == SCRIPT_HEAVY_CLASS_OVER
                || state.page_class == SCRIPT_HEAVY_CLASS_SHELL;
        }
        if (!ask) {
            release = true;
        } else if (bridge->heavy.policy == SCRIPT_HEAVY_POLICY_REFUSE) {
            bridge->heavy.answered = true;
            bridge->heavy.allowed = false;
            task->success = false;
            bridge->heavy.refused_scripts++;
            release = true;
        }
    }
    if (release) {
        if (bridge->heavy.waiting_bytes >= task->source_length)
            bridge->heavy.waiting_bytes -= task->source_length;
        if (bridge->heavy.waiting_scripts != 0)
            bridge->heavy.waiting_scripts--;
        return false;
    }
    return true;
}
