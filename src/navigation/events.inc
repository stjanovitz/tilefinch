/* Event dispatch into the page and its frames, frame activation, and
   external script evaluation. */

bool navigation_set_gamepad_state(
    NavigationSession *session, const TilefinchGamepadState *state)
{
    if (session == NULL || state == NULL || !session->page.loaded)
        return false;
    if (session->page.runtime == NULL) return !state->connected;
    return script_runtime_set_gamepad_state(session->page.runtime, state);
}

bool navigation_dispatch_event(NavigationSession *session,
                               const char *selector,
                               const char *event_type)
{
    if (session == NULL || session->page.runtime == NULL
        || !session->page.loaded) return false;
    if (!script_runtime_dispatch(session->page.runtime, selector, event_type,
                                 &session->page.script_result)) {
        (void) navigation_retire_failed_document_refresh(
            session, session->page.runtime);
        return false;
    }
    int scroll_y = 0;
    if (script_runtime_consume_scroll(session->page.runtime, &scroll_y)) {
        int maximum = navigation_max_scroll(session);
        navigation_set_scroll(session, scroll_y < maximum ? scroll_y : maximum);
    }
    return !script_runtime_consume_relayout(session->page.runtime)
           || navigation_relayout(session);
}

bool navigation_dispatch_node_event(NavigationSession *session,
                                    lxb_dom_node_t *node,
                                    const char *event_type)
{
    if (session == NULL || session->page.runtime == NULL
        || !session->page.loaded || node == NULL) return false;
    if (!script_runtime_dispatch_node(session->page.runtime, node, event_type,
                                      &session->page.script_result)) {
        (void) navigation_retire_failed_document_refresh(
            session, session->page.runtime);
        return false;
    }
    int scroll_y = 0;
    if (script_runtime_consume_scroll(session->page.runtime, &scroll_y)) {
        int maximum = navigation_max_scroll(session);
        navigation_set_scroll(session, scroll_y < maximum ? scroll_y : maximum);
    }
    return !script_runtime_consume_relayout(session->page.runtime)
           || navigation_relayout(session);
}

bool navigation_dispatch_node_activation(NavigationSession *session,
                                         lxb_dom_node_t *node)
{
    if (session == NULL || session->page.runtime == NULL
        || !session->page.loaded || node == NULL) return false;
    if (!script_runtime_dispatch_activation_node(
            session->page.runtime, node, &session->page.script_result)) {
        (void) navigation_retire_failed_document_refresh(
            session, session->page.runtime);
        return false;
    }
    int scroll_y = 0;
    if (script_runtime_consume_scroll(session->page.runtime, &scroll_y)) {
        int maximum = navigation_max_scroll(session);
        navigation_set_scroll(session, scroll_y < maximum ? scroll_y : maximum);
    }
    return !script_runtime_consume_relayout(session->page.runtime)
           || navigation_relayout(session);
}

static NavigationFrame *navigation_loaded_frame_for_element(
    NavigationSession *session, ScriptRuntime *parent_runtime,
    lxb_dom_node_t *frame_element)
{
    if (session == NULL || frame_element == NULL
        || !session->page.loaded) return NULL;
    for (size_t i = 0; i < session->page.frame_count; i++) {
        NavigationFrame *candidate = &session->page.frames[i];
        if (!candidate->retired && candidate->loaded
            && candidate->runtime != NULL
            && candidate->presentation != NULL
            && candidate->parent_runtime == parent_runtime
            && candidate->element == frame_element) {
            return candidate;
        }
    }
    return NULL;
}

static void navigation_notify_frame_user_activation(
    NavigationSession *session, NavigationFrame *frame)
{
    if (session == NULL || frame == NULL) return;
    /* HTML activation notification marks the active Window and every
       ancestor Window. Do this before dispatch so child handlers and parent
       message callbacks observe one coherent activation snapshot. */
    script_runtime_notify_user_activation(session->page.runtime);
    NavigationFrame *current = frame;
    for (size_t depth = 0; current != NULL && depth < NAVIGATION_FRAME_LIMIT;
         depth++) {
        script_runtime_notify_user_activation(current->runtime);
        if (current->parent_frame_slot_plus_one == 0) break;
        size_t parent_slot = current->parent_frame_slot_plus_one - 1u;
        if (parent_slot >= session->page.frame_count) break;
        current = &session->page.frames[parent_slot];
        if (current->retired || !current->loaded) break;
    }
}

static bool navigation_finish_frame_activation(
    NavigationSession *session, NavigationFrame *frame,
    lxb_dom_node_t *target)
{
    if (session == NULL || frame == NULL || target == NULL) return false;
    navigation_notify_frame_user_activation(session, frame);
    if (!script_runtime_dispatch_activation_node(
            frame->runtime, target, &frame->script_result)) {
        (void) navigation_retire_failed_document_refresh(
            session, frame->runtime);
        return false;
    }
    bool relayout = script_runtime_consume_relayout(frame->runtime);
    if (relayout) {
        if (!navigation_refresh_frame_presentation_or_defer(
                session, frame)) return false;
    }
    return navigation_deliver_frame_messages(session);
}

static bool navigation_finish_frame_pointer_click(
    NavigationSession *session, NavigationFrame *frame,
    lxb_dom_node_t *target, int client_x, int client_y,
    int offset_x, int offset_y)
{
    if (session == NULL || frame == NULL || target == NULL) return false;
    navigation_notify_frame_user_activation(session, frame);
    long target_handle = script_runtime_node_handle(
        frame->runtime, target);
    if (target_handle == 0) return false;
    static const unsigned phases[] = {2u, 3u, 5u};
    static const unsigned buttons[] = {1u, 0u, 0u};
    bool trace = tilefinch_trace_frame_controls();
    for (size_t i = 0; i < sizeof(phases) / sizeof(phases[0]); i++) {
        lxb_dom_node_t *live_target =
            script_runtime_node_handle_resolve_connected(
                frame->runtime, target_handle);
        if (live_target == NULL) {
            if (trace)
                fprintf(stderr, "frame-pointer-phase=%u failure=resolve\n",
                        phases[i]);
            script_runtime_node_handle_release(frame->runtime, target_handle);
            return false;
        }
        if (!script_runtime_dispatch_pointer_node(
                frame->runtime, live_target, phases[i],
                client_x, client_y, offset_x, offset_y, buttons[i],
                &frame->script_result)) {
            if (trace)
                fprintf(stderr,
                        "frame-pointer-phase=%u failure=dispatch error=\"%s\"\n",
                        phases[i], frame->script_result.error);
            script_runtime_node_handle_release(frame->runtime, target_handle);
            (void) navigation_retire_failed_document_refresh(
                session, frame->runtime);
            return false;
        }
        if (trace)
            fprintf(stderr, "frame-pointer-phase=%u success\n", phases[i]);
    }
    script_runtime_node_handle_release(frame->runtime, target_handle);
    bool relayout = script_runtime_consume_relayout(frame->runtime);
    if (relayout) {
        if (!navigation_refresh_frame_presentation_or_defer(
                session, frame)) return false;
    }
    bool delivered = navigation_deliver_frame_messages(session);
    if (trace && !delivered)
        fprintf(stderr, "frame-pointer-failure=message-delivery\n");
    return delivered;
}

bool navigation_activate_frame(NavigationSession *session,
                               lxb_dom_node_t *frame_element)
{
    NavigationFrame *frame = navigation_loaded_frame_for_element(
        session, session == NULL ? NULL : session->page.runtime,
        frame_element);
    if (frame == NULL) return false;
    bool trace_frame_controls =
        tilefinch_trace_frame_messages()
        || tilefinch_trace_frame_controls();
    lxb_dom_node_t *target = NULL;
    for (size_t depth = 0; depth < NAVIGATION_FRAME_LIMIT; depth++) {
        const LayoutDocument *layout = &frame->presentation->layout;
        const ControlRegion *nested_region = NULL;
        uint64_t nested_area = 0;
        if (trace_frame_controls) {
            fprintf(stderr,
                    "frame-activation-controls frame=%zu depth=%zu "
                    "controls=%zu links=%zu boxes=%zu\n",
                    (size_t) (frame - session->page.frames), depth,
                    layout->control_count, layout->link_count,
                    layout->node_box_count);
        }
        for (size_t i = 0; i < layout->control_count; i++) {
            const ControlRegion *control = &layout->controls[i];
            if (trace_frame_controls) {
                size_t name_length = 0, type_length = 0, role_length = 0,
                       id_length = 0;
                const char *name = document_element_name(
                    control->node, &name_length);
                const char *type = document_attribute(
                    control->node, "type", &type_length);
                const char *role = document_attribute(
                    control->node, "role", &role_length);
                const char *id = document_attribute(
                    control->node, "id", &id_length);
                fprintf(stderr,
                        "frame-activation-control index=%zu kind=%u "
                        "rect=%d,%d,%d,%d tag=\"%.*s\" type=\"%.*s\" "
                        "role=\"%.*s\" id=\"%.*s\"\n",
                        i, (unsigned) control->type, control->x, control->y,
                        control->width, control->height,
                        (int) name_length, name == NULL ? "" : name,
                        (int) type_length, type == NULL ? "" : type,
                        (int) role_length, role == NULL ? "" : role,
                        (int) id_length, id == NULL ? "" : id);
            }
            if (control->node == NULL || control->width <= 0
                || control->height <= 0
                || control->type == CONTROL_RESIZE) continue;
            if (control->type == CONTROL_FRAME) {
                uint64_t area = (uint64_t) (unsigned) control->width
                              * (uint64_t) (unsigned) control->height;
                if (area > nested_area) {
                    nested_area = area;
                    nested_region = control;
                }
                continue;
            }
            target = control->node;
            if (control->type == CONTROL_TOGGLE) break;
        }
        if (target != NULL) break;
        if (nested_region == NULL) return false;
        NavigationFrame *nested = navigation_loaded_frame_for_element(
            session, frame->runtime, nested_region->node);
        if (nested == NULL) return false;
        frame = nested;
    }
    if (target == NULL) return false;
    return navigation_finish_frame_activation(session, frame, target);
}

bool navigation_activate_frame_at(NavigationSession *session,
                                  lxb_dom_node_t *frame_element,
                                  int offset_x, int offset_y)
{
    NavigationFrame *frame = navigation_loaded_frame_for_element(
        session, session == NULL ? NULL : session->page.runtime,
        frame_element);
    if (frame == NULL || offset_x < 0 || offset_y < 0) return false;
    for (size_t depth = 0; depth < NAVIGATION_FRAME_LIMIT; depth++) {
        const LayoutDocument *layout = &frame->presentation->layout;
        bool trace_frame_controls =
            tilefinch_trace_frame_controls();
        if (trace_frame_controls) {
            fprintf(stderr,
                    "frame-pointer-hit depth=%zu point=%d,%d controls=%zu "
                    "links=%zu boxes=%zu\n",
                    depth, offset_x, offset_y, layout->control_count,
                    layout->link_count, layout->node_box_count);
        }
        const ControlRegion *control = layout_control_at(
            layout, offset_x, offset_y);
        if (control != NULL && control->type == CONTROL_FRAME) {
            NavigationFrame *nested = navigation_loaded_frame_for_element(
                session, frame->runtime, control->node);
            if (nested == NULL) return false;
            offset_x -= control->x;
            offset_y -= control->y;
            if (offset_x < 0 || offset_y < 0) return false;
            frame = nested;
            continue;
        }
        lxb_dom_node_t *target = control == NULL ? NULL : control->node;
        int target_x = control == NULL ? 0 : control->x;
        int target_y = control == NULL ? 0 : control->y;
        if (target == NULL) {
            const LinkRegion *link = layout_link_at(
                layout, offset_x, offset_y);
            if (link != NULL) {
                target = link->node;
                target_x = link->x;
                target_y = link->y;
            }
        }
        if (target == NULL) {
            /* Layout stores the deepest painted containing box first, which
               is the pointer target used by the top-level controller too. */
            for (size_t i = 0; i < layout->node_box_count; i++) {
                const LayoutNodeBox *box = &layout->node_boxes[i];
                if (box->node != NULL && box->width > 0 && box->height > 0
                    && offset_x >= box->x
                    && offset_x < box->x + box->width
                    && offset_y >= box->y
                    && offset_y < box->y + box->height) {
                    target = box->node;
                    target_x = box->x;
                    target_y = box->y;
                    break;
                }
            }
        }
        return target != NULL
            && navigation_finish_frame_pointer_click(
                session, frame, target, offset_x, offset_y,
                offset_x - target_x, offset_y - target_y);
    }
    return false;
}

bool navigation_dispatch_node_pointer(
    NavigationSession *session, lxb_dom_node_t *node, unsigned phase,
    int client_x, int client_y, int offset_x, int offset_y,
    unsigned buttons)
{
    if (session == NULL || session->page.runtime == NULL
        || !session->page.loaded || (node == NULL && phase != 1u)) return false;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    uint64_t pointer_started = navigation_now_us();
#endif
    if (!script_runtime_dispatch_pointer_node(
        session->page.runtime, node, phase, client_x, client_y,
            offset_x, offset_y, buttons, &session->page.script_result)) {
        (void) navigation_retire_failed_document_refresh(
            session, session->page.runtime);
        return false;
    }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    uint64_t pointer_dispatched = navigation_now_us();
#endif
    int scroll_y = 0;
    if (script_runtime_consume_scroll(session->page.runtime, &scroll_y)) {
        int maximum = navigation_max_scroll(session);
        navigation_set_scroll(session, scroll_y < maximum ? scroll_y : maximum);
    }
    bool finished = !script_runtime_consume_relayout(session->page.runtime)
        || navigation_relayout(session);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    if (phase == 5u) {
        char pointer_phases[160];
        snprintf(pointer_phases, sizeof(pointer_phases),
            "tilefinch-pointer-phases: script=%lluus settle=%lluus",
            (unsigned long long) (pointer_dispatched - pointer_started),
            (unsigned long long) (navigation_now_us() - pointer_dispatched));
        tilefinch_platform_log_message(pointer_phases);
    }
#endif
    return finished;
}

bool navigation_dispatch_node_input_event(NavigationSession *session,
                                          lxb_dom_node_t *node,
                                          const char *event_type,
                                          const char *data,
                                          const char *input_type,
                                          const char *current_value)
{
    if (session == NULL || session->page.runtime == NULL
        || !session->page.loaded || node == NULL) return false;
    if (!script_runtime_dispatch_input_node(
            session->page.runtime, node, event_type, data, input_type,
            current_value,
            &session->page.script_result)) {
        (void) navigation_retire_failed_document_refresh(
            session, session->page.runtime);
        return false;
    }
    return !script_runtime_consume_relayout(session->page.runtime)
           || navigation_relayout(session);
}

bool navigation_dispatch_node_submit_event(NavigationSession *session,
                                           lxb_dom_node_t *form,
                                           lxb_dom_node_t *submitter)
{
    if (session == NULL || session->page.runtime == NULL
        || !session->page.loaded || form == NULL) {
        return false;
    }
    if (!script_runtime_dispatch_submit_node(
            session->page.runtime, form, submitter,
            &session->page.script_result)) {
        (void) navigation_retire_failed_document_refresh(
            session, session->page.runtime);
        return false;
    }
    return !script_runtime_consume_relayout(session->page.runtime)
           || navigation_relayout(session);
}

bool navigation_evaluate_external_script(NavigationSession *session,
                                         lxb_dom_node_t *script_node,
                                         const char *source,
                                         size_t source_length,
                                         const char *source_url)
{
    return navigation_evaluate_external_script_response(
        session, script_node, source, source_length, source_url, false);
}

bool navigation_evaluate_external_script_response(
    NavigationSession *session, lxb_dom_node_t *script_node,
    const char *source, size_t source_length, const char *source_url,
    bool response_no_store)
{
    if (session == NULL || session->page.runtime == NULL
        || !session->page.loaded || script_node == NULL) return false;
    size_t integrity_length = 0;
    const char *integrity = document_attribute(
        script_node, "integrity", &integrity_length);
    TilefinchIntegrityResult integrity_result =
        tilefinch_resource_integrity_verify(
            integrity, integrity_length, (const uint8_t *) source,
            source_length);
    if (integrity_result == TILEFINCH_INTEGRITY_MISMATCH
        || integrity_result == TILEFINCH_INTEGRITY_INVALID) {
        (void) navigation_dispatch_node_event(session, script_node, "error");
        return false;
    }
    bool evaluated = script_runtime_evaluate_external_classic_cached(
        session->page.runtime, script_node, source, source_length,
        source_url, source_url, response_no_store,
        &session->page.script_result);
    if (navigation_retire_failed_document_refresh(
            session, session->page.runtime)) return false;
    bool relayout_ok = !script_runtime_consume_relayout(session->page.runtime)
                       || navigation_relayout(session);
    return evaluated && relayout_ok;
}
