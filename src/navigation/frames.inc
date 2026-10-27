/* Child frames: cross-frame message queueing and delivery, detached-frame
   retirement, <iframe> discovery and fetch, frame presentation, completion
   and progress, and the frame capability trace. */

static bool navigation_queue_frame_message(void *opaque,
                                           ScriptRuntime *source,
                                           long target_frame_handle,
                                           const char *json,
                                           const char *target_origin,
                                           const char *task_kind,
                                           uint64_t task_sequence)
{
    NavigationSession *session = opaque;
    if (session == NULL || source == NULL || json == NULL
        || target_origin == NULL) {
        return false;
    }
#if !defined(TILEFINCH_NO_TRACE)
    if (session->page.message_count == NAVIGATION_MESSAGE_LIMIT
        && (getenv("TILEFINCH_TRACE_FRAME_MESSAGES") != NULL
            || getenv("TILEFINCH_TRACE_FRAME_MESSAGE_DROPS") != NULL)) {
        fprintf(stderr,
                "frame-message-rejected reason=queue-full direction=%s "
                "source=%ld target=%ld\n",
                source == session->page.runtime ? "parent-to-child"
                                                : "child-to-parent",
                source == session->page.runtime ? 0L : -1L,
                target_frame_handle);
    }
#endif
    if (session->page.message_count == NAVIGATION_MESSAGE_LIMIT) return false;
    NavigationMessage *message =
        &session->page.messages[session->page.message_count];
    size_t json_length = strlen(json), origin_length = strlen(target_origin);
    if (json_length > 65536 || origin_length > NAVIGATION_URL_LIMIT) {
        return false;
    }
    if (strcmp(target_origin, "*") != 0
        && strcmp(target_origin, "null") != 0) {
        char canonical[TILEFINCH_ORIGIN_SERIALIZED_LIMIT] = {0};
        if (!tilefinch_url_origin(
                target_origin, canonical, sizeof(canonical))
            || strcmp(canonical, target_origin) != 0) return false;
    }
    message->json = budget_malloc(session->budget, json_length + 1);
    message->target_origin = budget_malloc(session->budget,
                                            origin_length + 1);
    if (message->json == NULL || message->target_origin == NULL) {
        budget_free(session->budget, message->json);
        budget_free(session->budget, message->target_origin);
        memset(message, 0, sizeof(*message));
        return false;
    }
    memcpy(message->json, json, json_length + 1);
    memcpy(message->target_origin, target_origin, origin_length + 1);
    message->source = source;
    if (session->next_frame_message_sequence != UINT64_MAX) {
        session->next_frame_message_sequence++;
    }
    message->sequence = session->next_frame_message_sequence;
    message->source_frame_handle = 0;
    const ScriptResult *source_result = &session->page.script_result;
    if (source != session->page.runtime) {
        for (size_t i = 0; i < session->page.frame_count; i++) {
            if (!session->page.frames[i].retired
                && session->page.frames[i].runtime == source) {
                source_result = &session->page.frames[i].script_result;
                message->source_frame_handle =
                    session->page.frames[i].parent_handle;
                message->source_generation =
                    session->page.frames[i].lifecycle_generation;
                if (session->page.frames[i].opaque_origin) {
                    snprintf(message->source_origin,
                             sizeof(message->source_origin), "null");
                } else if (!tilefinch_url_origin(
                        session->page.frames[i].url,
                        message->source_origin,
                        sizeof(message->source_origin))) {
                    message->source_frame_handle = 0;
                }
                break;
            }
        }
        if (message->source_frame_handle == 0) {
            budget_free(session->budget, message->json);
            budget_free(session->budget, message->target_origin);
            memset(message, 0, sizeof(*message));
            return false;
        }
    } else if (!tilefinch_url_origin(
                   session->page.document_url, message->source_origin,
                   sizeof(message->source_origin))) {
        budget_free(session->budget, message->json);
        budget_free(session->budget, message->target_origin);
        memset(message, 0, sizeof(*message));
        return false;
    } else {
        message->source_generation = session->generation;
    }
    message->target_frame_handle = target_frame_handle;
    if (target_frame_handle > 0) {
        bool target_found = false;
        for (size_t i = 0; i < session->page.frame_count; i++) {
            NavigationFrame *target = &session->page.frames[i];
            if (!target->retired
                && target->parent_runtime == source
                && target->parent_handle == target_frame_handle) {
                message->target_generation = target->lifecycle_generation;
                target_found = true;
                break;
            }
        }
        if (!target_found) {
            budget_free(session->budget, message->json);
            budget_free(session->budget, message->target_origin);
            memset(message, 0, sizeof(*message));
            /* WindowProxy creation can precede native iframe discovery by a
               turn.  Posting to that not-yet-known browsing context is a
               silent drop, not an internal engine failure exposed to page
               script. */
            session->frame_messages_dropped++;
            return true;
        }
    }
    session->page.message_count++;
    session->frame_messages_posted++;
    if (source == session->page.runtime) session->frame_messages_to_child++;
    else session->frame_messages_to_parent++;
#if !defined(TILEFINCH_NO_TRACE)
    const char *frame_trace_value = getenv("TILEFINCH_TRACE_FRAME_MESSAGES");
    if (frame_trace_value != NULL) {
        /* Explicitly opt-in because challenge payloads can contain transient
           tokens. Keep each record bounded even though the queue itself
           accepts larger structured-clone JSON. */
        size_t trace_limit = 2048;
        char *trace_end = NULL;
        unsigned long requested = strtoul(frame_trace_value, &trace_end, 10);
        if (trace_end != frame_trace_value && *trace_end == '\0'
            && requested >= 256 && requested <= 65536) {
            trace_limit = (size_t) requested;
        }
        fprintf(stderr,
                "frame-message-json sequence=%llu time-ms=%llu direction=%s "
                "source=%ld source-generation=%llu target=%ld "
                "target-generation=%llu json=\"%.*s\"\n",
                (unsigned long long) message->sequence,
                (unsigned long long) (navigation_now_us() / 1000u),
                source == session->page.runtime ? "parent-to-child"
                                                : "child-to-parent",
                message->source_frame_handle,
                (unsigned long long) message->source_generation,
                message->target_frame_handle,
                (unsigned long long) message->target_generation,
                (int) (json_length < trace_limit ? json_length : trace_limit),
                json);
    }
#endif
#if !defined(TILEFINCH_NO_TRACE) || defined(TILEFINCH_PSP_VALIDATION_LOG)
    /* The last-message, event-log and reject-reason fields below are read
       only by the host interactive lab and tests. */
    const char *event = strstr(json, "\"event\":");
    if (event != NULL) {
        event = strchr(event + 8, '"');
        if (event != NULL) {
            event++;
            const char *end = strchr(event, '"');
            if (end != NULL) {
                size_t length = (size_t) (end - event);
                if (length >= sizeof(session->last_frame_message_event)) {
                    length = sizeof(session->last_frame_message_event) - 1;
                }
                memcpy(session->last_frame_message_event, event, length);
                session->last_frame_message_event[length] = '\0';
                session->last_frame_message_event_sequence =
                    message->sequence;
                session->last_frame_message_event_source =
                    message->source_frame_handle;
                session->last_frame_message_event_source_generation =
                    message->source_generation;
                char record[112];
                int written = snprintf(
                    record, sizeof(record), "%s:%.*s",
                    source == session->page.runtime ? "P>C" : "C>P",
                    (int) length, event);
                if (written > 0 && (size_t) written < sizeof(record)) {
                    size_t record_length = (size_t) written;
                    size_t used = strlen(session->frame_message_event_log);
                    while (used != 0
                           && used + 1 + record_length
                                  >= sizeof(session->frame_message_event_log)) {
                        char *separator = strchr(
                            session->frame_message_event_log, ',');
                        if (separator == NULL) {
                            session->frame_message_event_log[0] = '\0';
                            used = 0;
                            break;
                        }
                        size_t removed = (size_t) (
                            separator + 1 - session->frame_message_event_log);
                        memmove(session->frame_message_event_log,
                                separator + 1, used - removed + 1);
                        used -= removed;
                    }
                    snprintf(session->frame_message_event_log + used,
                             sizeof(session->frame_message_event_log) - used,
                             "%s%s", used == 0 ? "" : ",", record);
                }
                session->last_frame_message_enqueued_us = navigation_now_us();
                session->last_frame_message_source_generation =
                    message->source_generation;
                session->last_frame_message_target_generation =
                    message->target_generation;
                session->last_frame_message_task_sequence = task_sequence;
                snprintf(session->last_frame_message_task_kind,
                         sizeof(session->last_frame_message_task_kind), "%.*s",
                         96, task_kind == NULL ? "unknown" : task_kind);
                session->last_frame_message_pending_tasks =
                    source_result->pending_tasks;
                session->last_frame_message_root_timers =
                    source_result->root_timers;
                session->last_frame_message_root_network =
                    source_result->root_pending_network;
                session->last_frame_message_root_workers =
                    source_result->root_workers;
                session->last_frame_message_async_completed =
                    source_result->async_network_logical_completed;
                session->last_frame_message_async_rejected =
                    source_result->async_network_logical_rejected;
                session->last_frame_message_async_cancelled =
                    source_result->async_network_logical_cancelled;
                session->last_frame_message_async_timed_out =
                    source_result->async_network_logical_timed_out;
                session->last_frame_message_xhr_responses =
                    source_result->xhr_response_count;
                session->last_frame_message_promise_rejections =
                    source_result->promise_rejections;
                session->last_frame_message_callback_errors =
                    source_result->uncaught_callback_errors;
                session->last_frame_message_watchdog_polls =
                    source_result->watchdog_polls;
                session->last_frame_message_interrupted =
                    source_result->interrupted;
                if (length == 6 && memcmp(event, "reject", 6) == 0) {
                    const char *reason = strstr(json, "\"reason\":");
                    if (reason != NULL) {
                        reason += 9;
                        while (*reason == ' ' || *reason == '"') reason++;
                        const char *end = reason;
                        while (*end != '\0' && *end != '"' && *end != ','
                               && *end != '}') end++;
                        size_t reason_length = (size_t) (end - reason);
                        if (reason_length >=
                            sizeof(session->last_frame_reject_reason)) {
                            reason_length =
                                sizeof(session->last_frame_reject_reason) - 1;
                        }
                        memcpy(session->last_frame_reject_reason, reason,
                               reason_length);
                        session->last_frame_reject_reason[reason_length] = '\0';
                    }
                }
            }
        }
    }
#else
    (void) source_result;
    (void) task_kind;
    (void) task_sequence;
#endif
    return true;
}

bool navigation_test_configure_frame_messaging(
    NavigationSession *session, NavigationFrame *frame)
{
    if (session == NULL || frame == NULL || frame->runtime == NULL) {
        return false;
    }
    return script_runtime_configure_messaging(
        frame->runtime, true, true, false, true,
        navigation_queue_frame_message, session);
}

static NavigationFrame *frame_for_handle(NavigationSession *session,
                                         ScriptRuntime *parent_runtime,
                                         long handle)
{
    for (size_t i = 0; i < session->page.frame_count; i++) {
        if (!session->page.frames[i].retired
            && session->page.frames[i].parent_runtime == parent_runtime
            && session->page.frames[i].parent_handle == handle) {
            return &session->page.frames[i];
        }
    }
    return NULL;
}

static NavigationFrame *frame_for_runtime(NavigationSession *session,
                                          ScriptRuntime *runtime)
{
    if (session == NULL || runtime == NULL) return NULL;
    for (size_t i = 0; i < session->page.frame_count; i++) {
        NavigationFrame *frame = &session->page.frames[i];
        if (!frame->retired && frame->runtime == runtime) return frame;
    }
    return NULL;
}

static void navigation_drop_frame_messages(NavigationSession *session,
                                           const NavigationFrame *frame);
static void navigation_retire_frame_descendants(
    NavigationSession *session, size_t ancestor_slot,
    const char *reason);

static bool navigation_frame_presentation_dimensions(
    const NavigationSession *session, const NavigationFrame *frame,
    int *width, int *height);

static bool navigation_deliver_frame_messages(NavigationSession *session)
{
    enum {
        NAVIGATION_MESSAGE_TARGET_TOP = -1,
        NAVIGATION_MESSAGE_SOURCE_UNAVAILABLE = -2
    };
    size_t delivered = 0;
    size_t deferred_run = 0;
    while (session->page.message_count != 0
           && delivered < NAVIGATION_MESSAGE_LIMIT) {
        NavigationMessage message = session->page.messages[0];
        memmove(session->page.messages, session->page.messages + 1,
                (session->page.message_count - 1)
                    * sizeof(*session->page.messages));
        session->page.message_count--;
        memset(&session->page.messages[session->page.message_count], 0,
               sizeof(*session->page.messages));
        ScriptRuntime *target = NULL;
        NavigationFrame *target_frame = NULL;
        NavigationFrame *target_owner_frame = NULL;
        NavigationFrame *source_frame = frame_for_runtime(
            session, message.source);
        long source_handle = message.source_frame_handle;
        bool lifecycle_valid = true;
        if (message.target_frame_handle > 0) {
            target_frame = frame_for_handle(
                session, message.source, message.target_frame_handle);
            if (target_frame != NULL
                && target_frame->lifecycle_generation
                       == message.target_generation) {
                target = target_frame->runtime;
                target_owner_frame = target_frame;
            } else {
                lifecycle_valid = false;
            }
        } else if (source_frame != NULL) {
            if (message.target_frame_handle
                == NAVIGATION_MESSAGE_TARGET_TOP) {
                target = session->page.runtime;
                target_owner_frame = NULL;
            } else {
                target = source_frame->parent_runtime;
                target_owner_frame = navigation_frame_parent(
                    session, source_frame);
            }
            lifecycle_valid = source_frame->parent_handle
                                  == message.source_frame_handle
                && source_frame->lifecycle_generation
                       == message.source_generation;
        } else {
            lifecycle_valid = false;
        }
        /* A WindowProxy exists before its remote document has finished
           creating a ScriptRuntime.  postMessage queues a task against that
           stable browsing-context identity; it must not be mistaken for a
           message to a vanished target merely because frame loading has not
           reached runtime creation yet. Preserve FIFO order for that target
           by rotating the message to the tail. This also lets a ready sibling
           or parent task behind it run; returning it to the head used to make
           one slow iframe block every other browsing context. Stop after one
           complete deferred-only rotation so the bounded owner turn cannot
           spin. A loaded frame with no runtime is a genuine non-script target
           and continues through the ordinary drop path. */
        bool target_task_deferred = lifecycle_valid
            && ((target_frame != NULL && target == NULL
                 && !target_frame->retired && !target_frame->loaded
                 && target_frame->fetch_pending)
                || (target != NULL
                    && !script_runtime_message_dispatch_ready(target)));
        if (target_task_deferred) {
            session->page.messages[session->page.message_count] = message;
            session->page.message_count++;
            session->frame_messages_deferred++;
            deferred_run++;
            if (deferred_run > session->frame_message_max_deferred_run) {
                session->frame_message_max_deferred_run = deferred_run;
            }
            if (deferred_run >= session->page.message_count) break;
            continue;
        }
        deferred_run = 0;
        bool target_allowed = lifecycle_valid && target != NULL;
        const char *drop_reason = !lifecycle_valid
            ? "lifecycle" : target == NULL ? "target-missing" : NULL;
        if (target_allowed && strcmp(message.target_origin, "*") != 0) {
            char receiver_origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT] = {0};
            if (target_owner_frame != NULL
                && target_owner_frame->opaque_origin) {
                snprintf(receiver_origin, sizeof(receiver_origin), "null");
            } else {
                const char *receiver_url = target == session->page.runtime
                    ? session->page.document_url : target_owner_frame->url;
                (void) url_origin(receiver_url, receiver_origin,
                                  sizeof(receiver_origin));
            }
            target_allowed = strcmp(message.target_origin,
                                    receiver_origin) == 0;
            if (!target_allowed) drop_reason = "origin";
        }
        session->last_frame_message_sequence = message.sequence;
        session->last_frame_message_source = source_handle;
        session->last_frame_message_target =
            target == session->page.runtime ? 0
                : (target_frame != NULL ? message.target_frame_handle
                                        : (source_frame != NULL
                                               ? source_frame->parent_handle
                                               : 0));
        bool nested_top_message = message.target_frame_handle
                == NAVIGATION_MESSAGE_TARGET_TOP
            && source_frame != NULL
            && source_frame->parent_runtime != session->page.runtime;
        /* The top realm does not own a DOM handle for a nested iframe. Until
           the bounded WindowProxy table represents transitive descendants,
           expose a standards-safe null source rather than aliasing the
           nested parent's unrelated handle in the top document. */
        long dispatch_source_handle = target_frame == NULL
            ? (nested_top_message ? NAVIGATION_MESSAGE_SOURCE_UNAVAILABLE
                                  : source_handle)
            : 0;
#if defined(TILEFINCH_NO_TRACE)
        (void) drop_reason;
#endif
#if !defined(TILEFINCH_NO_TRACE)
        if (!target_allowed
            && (getenv("TILEFINCH_TRACE_FRAME_MESSAGES") != NULL
                || getenv("TILEFINCH_TRACE_FRAME_MESSAGE_DROPS") != NULL)) {
            fprintf(stderr,
                    "frame-message-dropped sequence=%llu reason=%s "
                    "source=%ld target=%ld\n",
                    (unsigned long long) message.sequence,
                    drop_reason == NULL ? "policy" : drop_reason,
                    source_handle, message.target_frame_handle);
        }
#endif
        bool ok = !target_allowed || script_runtime_dispatch_message(
            target, message.json, message.source_origin,
            dispatch_source_handle,
            target == session->page.runtime
                ? &session->page.script_result
                : &target_owner_frame->script_result);
        budget_free(session->budget, message.target_origin);
        budget_free(session->budget, message.json);
        if (!ok && navigation_retire_failed_document_refresh(
                session, target)) return false;
        if (!ok && target_owner_frame != NULL) {
            /* A failed child browsing context is contained like a crashed
               renderer process. It must not turn a later postMessage into a
               top-level navigation failure or leave queued pointers to the
               realm which is about to be destroyed. */
            size_t owner_slot = (size_t) (
                target_owner_frame - session->page.frames);
            navigation_retire_frame_descendants(
                session, owner_slot, "ancestor iframe failed");
            navigation_drop_frame_messages(session, target_owner_frame);
            if (target_owner_frame->parent_runtime != NULL) {
                (void) script_runtime_set_frame_window_state(
                    target_owner_frame->parent_runtime,
                    target_owner_frame->parent_handle, false, false, false,
                    NULL, false, navigation_frame_parent_result(
                              session, target_owner_frame));
            }
            if (tilefinch_trace_js_roots()
                && target_owner_frame->runtime != NULL) {
                script_runtime_report_roots(
                    target_owner_frame->runtime, stderr, "frame-failed",
                    "child", target_owner_frame->parent_handle);
            }
            navigation_frame_presentation_destroy(
                session, target_owner_frame);
            script_runtime_destroy(target_owner_frame->runtime);
            target_owner_frame->runtime = NULL;
            images_destroy(&target_owner_frame->images);
            document_destroy(&target_owner_frame->document);
            target_owner_frame->fetch_pending = false;
            target_owner_frame->loaded = false;
            target_owner_frame->retired = true;
            session->frames_failed++;
            session->frame_messages_dropped++;
            delivered++;
            continue;
        }
        if (!ok) {
            (void) navigation_retire_failed_document_refresh(
                session, target);
            return false;
        }
        if (target_allowed) session->frame_messages_delivered++;
        else session->frame_messages_dropped++;
        if (ok && target_allowed && target_owner_frame != NULL
            && script_runtime_consume_relayout(target)) {
            target_owner_frame->presentation_dirty = true;
        }
        delivered++;
    }
    /* The parent can resize an iframe without the child changing (Turnstile
       shrinks its widget frame from the default 300x150 to 300x65 after the
       child has painted). A snapshot taken at the old size was then drawn
       squashed into the new box; re-snapshot at the new size. */
    for (size_t i = 0; i < session->page.frame_count; i++) {
        NavigationFrame *frame = &session->page.frames[i];
        if (frame->retired || !frame->loaded || frame->runtime == NULL
            || frame->presentation == NULL || frame->presentation_dirty)
            continue;
        int width = 0, height = 0;
        if (navigation_frame_presentation_dimensions(
                session, frame, &width, &height)
            && (width != frame->presentation->width
                || height != frame->presentation->height))
            frame->presentation_dirty = true;
    }
    /* A mutating child message may rebuild at most one opaque child snapshot
       per owner tick. Remaining dirty children retain their flag, preventing
       an animation-heavy frame from forcing an unbounded raster cascade. */
    for (size_t i = 0; i < session->page.frame_count; i++) {
        NavigationFrame *frame = &session->page.frames[i];
        if (frame->retired || !frame->loaded || frame->runtime == NULL
            || !frame->presentation_dirty) continue;
        if (!navigation_refresh_frame_presentation_or_defer(
                session, frame)) return false;
        /* A permanently unpublishable frame (for example, one whose element
           was detached) keeps its retry latch, but must not starve a later
           child whose snapshot can be refreshed. The fixed frame table keeps
           this scan bounded; still stop after the first successful publish. */
        if (!frame->presentation_dirty) break;
    }
    return true;
}

static bool navigation_frame_element_connected(
    const NavigationSession *session, const NavigationFrame *frame)
{
    if (session == NULL || frame == NULL || frame->parent_runtime == NULL
        || frame->parent_handle <= 0) return false;
    lxb_dom_node_t *node = script_runtime_node_handle_resolve(
        frame->parent_runtime, frame->parent_handle);
    for (lxb_dom_node_t *at = node; at != NULL; at = at->parent) {
        if (at->type == LXB_DOM_NODE_TYPE_DOCUMENT) return true;
    }
    return false;
}

static void navigation_drop_frame_messages(NavigationSession *session,
                                           const NavigationFrame *frame)
{
    size_t write = 0;
    for (size_t read = 0; read < session->page.message_count; read++) {
        NavigationMessage *message = &session->page.messages[read];
        bool drop = (message->source == frame->runtime
                     && message->source_generation
                            == frame->lifecycle_generation)
            || (message->target_frame_handle == frame->parent_handle
                && message->target_generation
                       == frame->lifecycle_generation);
        if (drop) {
            budget_free(session->budget, message->json);
            budget_free(session->budget, message->target_origin);
            session->frame_messages_dropped++;
            continue;
        }
        if (write != read) session->page.messages[write] = *message;
        write++;
    }
    for (size_t i = write; i < session->page.message_count; i++) {
        memset(&session->page.messages[i], 0,
               sizeof(session->page.messages[i]));
    }
    session->page.message_count = write;
}

static bool navigation_retire_detached_frames(NavigationSession *session)
{
    /* Parents are appended before descendants. Retire in reverse order so a
       nested frame never resolves its element through an already-destroyed
       parent runtime. */
    for (size_t cursor = session->page.frame_count; cursor != 0; cursor--) {
        size_t i = cursor - 1u;
        NavigationFrame *frame = &session->page.frames[i];
        if (frame->retired
            || navigation_frame_element_connected(session, frame)) continue;
        navigation_retire_frame_descendants(
            session, i, "ancestor iframe detached");
        if (frame->fetch_pending) {
            (void) fetch_scheduler_cancel(session->page.resource_scheduler,
                                          frame->fetch_request_id,
                                          "iframe detached");
            (void) fetch_scheduler_discard(session->page.resource_scheduler,
                                           frame->fetch_request_id);
            frame->fetch_pending = false;
        }
        navigation_drop_frame_messages(session, frame);
        if (frame->parent_runtime != NULL
            && !script_runtime_set_frame_window_state(
                frame->parent_runtime, frame->parent_handle, false, false,
                false,
                NULL, false, navigation_frame_parent_result(
                          session, frame))) return false;
        if (tilefinch_trace_js_roots()
            && frame->runtime != NULL) {
            script_runtime_report_roots(frame->runtime, stderr,
                                        "pre-detach", "child",
                                        frame->parent_handle);
        }
        navigation_frame_presentation_destroy(session, frame);
        script_runtime_destroy(frame->runtime);
        frame->runtime = NULL;
        images_destroy(&frame->images);
        document_destroy(&frame->document);
        frame->loaded = false;
        frame->retired = true;
        session->frames_detached++;
    }
    return true;
}

static bool collect_iframes(ScriptRuntime *runtime, lxb_dom_node_t *node,
                            long *frames, size_t capacity,
                            size_t *count)
{
    if (runtime == NULL || node == NULL) return true;
    lxb_dom_node_t *boundary_parent = node->parent;
    size_t visited = 0;
    while (node != NULL && node != boundary_parent && *count < capacity) {
        if (++visited > NAVIGATION_DOM_TRAVERSAL_NODE_LIMIT) return false;
        if (node_name_is(node, "iframe")) {
            long handle = script_runtime_node_weak_handle(runtime, node);
            if (handle == 0) return false;
            if (*count < capacity) frames[(*count)++] = handle;
        }
        if (*count == capacity) break;
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != NULL && node != boundary_parent
               && node->next == NULL) node = node->parent;
        if (node == NULL || node == boundary_parent) break;
        node = node->next;
    }
    return true;
}

static bool frame_element_was_discovered(const NavigationPage *page,
                                         ScriptRuntime *runtime,
                                         long handle)
{
    for (size_t i = 0; i < page->discovered_frame_element_count; i++) {
        if (page->discovered_frame_runtimes[i] == runtime
            && page->discovered_frame_handles[i] == handle) return true;
    }
    return false;
}

static bool remember_discovered_frame(NavigationPage *page,
                                      ScriptRuntime *runtime,
                                      long handle)
{
    if (runtime == NULL || handle == 0
        || frame_element_was_discovered(page, runtime, handle)) return false;
    if (page->discovered_frame_element_count
        >= NAVIGATION_FRAME_DISCOVERY_LIMIT) return false;
    size_t slot = page->discovered_frame_element_count++;
    page->discovered_frame_handles[slot] = handle;
    page->discovered_frame_runtimes[slot] = runtime;
    return true;
}

static TilefinchRequestContext navigation_frame_request_context(
    const NavigationSession *session, const NavigationFrame *frame)
{
    return (TilefinchRequestContext) {
        .target_url = frame == NULL ? NULL : frame->url,
        .initiator_url = frame == NULL ? NULL : frame->parent_url,
        .top_level_url = session == NULL ? NULL
                                         : session->page.document_url,
        .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_NAVIGATE,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_FRAME
    };
}

static uint64_t navigation_enqueue_frame_fetch(
    NavigationSession *session, NavigationFrame *frame,
    const char *client_hint_tokens, const char *client_hint_origin)
{
    if (session == NULL || frame == NULL
        || session->page.resource_scheduler == NULL) return 0;
    TilefinchRequestContext request_context =
        navigation_frame_request_context(session, frame);
    NavigationFrame *parent = navigation_frame_parent(session, frame);
    const TilefinchContentSecurityPolicy *parent_csp = parent == NULL
        ? &session->page.document.content_security_policy
        : &parent->document.content_security_policy;
    FetchRequest transport = {
        .allow_http_errors = true,
        .send_client_hints = client_hint_tokens != NULL,
        .client_hint_tokens = client_hint_tokens,
        .client_hint_origin = client_hint_origin,
        .send_low_client_hints = true,
        .accept = "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
        .upgrade_insecure_requests = true,
        .user_agent = session->diagnostic_frame_safari
            ? "Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.4 Mobile/15E148 Safari/604.1"
            : NULL
    };
    FetchPreparedPageRequest prepared;
    if (!fetch_prepare_page_request_context(
            &request_context, frame->parent_url,
            frame->referrer_policy, session->browser_session,
            parent_csp, NULL,
            &transport, &prepared, NULL)) return 0;
    const FetchRequest *request = fetch_prepared_page_request(&prepared);
    if (request == NULL) return 0;
    /* Inline app documents share the page's bounded per-script allowance.
       Responses remain charged to the shared Budget and capped at 8 MiB. */
    size_t frame_bytes = session->maximum_script_file_bytes;
    if (frame_bytes < 512u * 1024u) frame_bytes = 512u * 1024u;
    if (frame_bytes > 8u * 1024u * 1024u) frame_bytes = 8u * 1024u * 1024u;
    return fetch_scheduler_enqueue(
        session->page.resource_scheduler, frame->url, request,
        frame_bytes, 15000);
}

/* A child frame's scripts share the page's per-script source ceiling:
   memory-based admission decides whether each one compiles. A fixed
   512 KiB here refused DataDome's 523 KB challenge script inside its
   iframe on etsy.com and nytimes.com. */
static size_t navigation_frame_script_file_bytes(
    const NavigationSession *session)
{
    return session->maximum_script_file_bytes != 0
        ? session->maximum_script_file_bytes : 512u * 1024u;
}

static bool navigation_frame_descends_from(
    const NavigationSession *session, const NavigationFrame *candidate,
    size_t ancestor_slot)
{
    if (session == NULL || candidate == NULL) return false;
    size_t parent_plus_one = candidate->parent_frame_slot_plus_one;
    for (size_t depth = 0;
         parent_plus_one != 0 && depth < NAVIGATION_FRAME_LIMIT; depth++) {
        size_t slot = parent_plus_one - 1u;
        if (slot == ancestor_slot) return true;
        if (slot >= session->page.frame_count) return false;
        parent_plus_one =
            session->page.frames[slot].parent_frame_slot_plus_one;
    }
    return false;
}

static void navigation_retire_frame_descendants(
    NavigationSession *session, size_t ancestor_slot,
    const char *reason)
{
    if (session == NULL) return;
    for (size_t cursor = session->page.frame_count; cursor != 0; cursor--) {
        size_t slot = cursor - 1u;
        NavigationFrame *candidate = &session->page.frames[slot];
        if (candidate->retired || slot == ancestor_slot
            || !navigation_frame_descends_from(
                   session, candidate, ancestor_slot)) continue;
        if (candidate->fetch_pending
            && session->page.resource_scheduler != NULL) {
            (void) fetch_scheduler_cancel(
                session->page.resource_scheduler,
                candidate->fetch_request_id,
                reason == NULL ? "ancestor iframe retired" : reason);
            (void) fetch_scheduler_discard(
                session->page.resource_scheduler,
                candidate->fetch_request_id);
        }
        navigation_drop_frame_messages(session, candidate);
        navigation_frame_presentation_destroy(session, candidate);
        script_runtime_destroy(candidate->runtime);
        candidate->runtime = NULL;
        images_destroy(&candidate->images);
        document_destroy(&candidate->document);
        candidate->fetch_pending = false;
        candidate->loaded = false;
        candidate->retired = true;
    }
}

static bool navigation_load_frames_for_context(
    NavigationSession *session, ScriptRuntime *parent_runtime,
    const PocDocument *parent_document, const char *parent_url,
    size_t parent_frame_slot_plus_one)
{
    if (session == NULL || parent_runtime == NULL
        || parent_document == NULL || parent_document->html == NULL
        || parent_url == NULL) return true;
    long handles[NAVIGATION_FRAME_DISCOVERY_LIMIT] = {0};
    size_t count = 0;
    if (!collect_iframes(
            parent_runtime,
            lxb_dom_interface_node(parent_document->html), handles,
            NAVIGATION_FRAME_DISCOVERY_LIMIT, &count)) {
        /* An oversized document loses frame discovery for this pass, but a
           bounded scan overflow must not abort an otherwise healthy runtime
           advance: the page keeps rendering, and a later mutation retries
           discovery. This holds for the top document as well as for a
           nested child. */
        session->frames_failed++;
        return true;
    }
    if (count == 0) return true;
    if (parent_frame_slot_plus_one == 0
        && !script_runtime_configure_messaging(
            parent_runtime, false, true, false, true,
            navigation_queue_frame_message, session)) return false;
    for (size_t i = 0; i < count; i++) {
        lxb_dom_node_t *node = script_runtime_node_handle_resolve_connected(
            parent_runtime, handles[i]);
        if (node == NULL) continue;
        size_t srcdoc_length = 0;
        const char *srcdoc = document_attribute(
            node, "srcdoc", &srcdoc_length);
        /* Same-origin srcdoc/about:blank/Blob frames live in the bounded
           bootstrap frame realm. They must never also enqueue a remote
           fetch or dispatch a second load/error event here. */
        if (srcdoc != NULL) continue;
        long parent_handle = script_runtime_node_handle(
            parent_runtime, node);
        NavigationFrame *frame = NULL;
        for (size_t slot = 0; slot < session->page.frame_count; slot++) {
            NavigationFrame *candidate = &session->page.frames[slot];
            if (!candidate->retired
                && candidate->parent_runtime == parent_runtime
                && candidate->parent_handle == parent_handle) {
                frame = candidate;
                break;
            }
        }
        bool known = frame_element_was_discovered(
            &session->page, parent_runtime, handles[i]);
        size_t source_length = 0;
        const char *source = document_attribute(node, "src",
                                                &source_length);
        size_t sandbox_length = 0;
        const char *sandbox = document_attribute(
            node, "sandbox", &sandbox_length);
        uint32_t sandbox_flags = tilefinch_frame_sandbox_parse(
            sandbox, sandbox_length, sandbox != NULL);
        /* Missing src is the local about:blank document installed by the
           bootstrap. It is not a network failure. A later src mutation is
           intentionally reconsidered because no C frame slot is retained. */
        if (source == NULL || source_length == 0
            || source_length >= NAVIGATION_URL_LIMIT) continue;
        char reference[NAVIGATION_URL_LIMIT];
        memcpy(reference, source, source_length);
        reference[source_length] = '\0';
        char resolved_url[NAVIGATION_URL_LIMIT];
        if (!fetch_resolve_url(parent_url, reference, resolved_url,
                               sizeof(resolved_url))) {
            session->frames_failed++;
            continue;
        }
        if (frame != NULL && !frame->retired
            && strcmp(frame->url, resolved_url) == 0
            && frame->sandbox_flags == sandbox_flags) continue;
        if (!known && !remember_discovered_frame(
                &session->page, parent_runtime, handles[i])) {
            continue;
        }
        session->frames_discovered++;
        if (frame != NULL && !frame->retired) {
            size_t frame_slot = (size_t) (frame - session->page.frames);
            navigation_retire_frame_descendants(
                session, frame_slot, "ancestor iframe source changed");
            if (frame->fetch_pending) {
                (void) fetch_scheduler_cancel(
                    session->page.resource_scheduler,
                    frame->fetch_request_id, "iframe source changed");
                (void) fetch_scheduler_discard(
                    session->page.resource_scheduler,
                    frame->fetch_request_id);
            }
            navigation_drop_frame_messages(session, frame);
            navigation_frame_presentation_destroy(session, frame);
            script_runtime_destroy(frame->runtime);
            images_destroy(&frame->images);
            document_destroy(&frame->document);
            frame->runtime = NULL;
            frame->fetch_pending = false;
            frame->loaded = false;
            frame->retired = true;
        }
        if (frame == NULL) {
            for (size_t slot = 0; slot < session->page.frame_count; slot++) {
                if (session->page.frames[slot].retired) {
                    frame = &session->page.frames[slot];
                    break;
                }
            }
        }
        bool append_frame = frame == NULL;
        if (append_frame) {
            if (session->page.frame_count >= NAVIGATION_FRAME_LIMIT) {
                session->frames_failed++;
                if (!navigation_dispatch_frame_parent_event(
                        session, parent_runtime,
                        parent_frame_slot_plus_one, node, "error")) {
                    return false;
                }
                continue;
            }
            frame = &session->page.frames[session->page.frame_count];
        } else {
            /* Retirement has already released every owned pointer. Reusing
               the fixed slot keeps repeated iframe attach/detach bounded. */
            memset(frame, 0, sizeof(*frame));
        }
        snprintf(frame->url, sizeof(frame->url), "%s", resolved_url);
        snprintf(frame->parent_url, sizeof(frame->parent_url), "%s",
                 parent_url);
        frame->element = node;
        frame->parent_runtime = parent_runtime;
        frame->parent_frame_slot_plus_one = parent_frame_slot_plus_one;
        frame->parent_handle = parent_handle;
        frame->sandbox_flags = sandbox_flags;
        frame->opaque_origin = !tilefinch_frame_sandbox_allows(
            sandbox_flags, TILEFINCH_SANDBOX_ALLOW_SAME_ORIGIN);
        NavigationFrame *parent_frame = navigation_frame_parent(
            session, frame);
        const char *initiator_referrer_policy = parent_frame == NULL
            ? session->page.referrer_policy
            : parent_frame->referrer_policy;
        snprintf(frame->referrer_policy, sizeof(frame->referrer_policy),
                 "%s", initiator_referrer_policy);
        if (session->next_frame_lifecycle_generation != UINT64_MAX) {
            session->next_frame_lifecycle_generation++;
        }
        frame->lifecycle_generation =
            session->next_frame_lifecycle_generation;
        if (session->page.resource_scheduler == NULL) {
            session->frames_failed++;
            if (!navigation_dispatch_frame_parent_event(
                    session, parent_runtime,
                    parent_frame_slot_plus_one, node, "error")) {
                return false;
            }
            continue;
        }
        char cached_hint_tokens[BROWSER_CLIENT_HINT_TOKEN_LIMIT] = {0};
        char cached_hint_origin[BROWSER_ORIGIN_LIMIT] = {0};
        TilefinchRequestContext frame_hint_context = {
            .target_url = frame->url,
            .initiator_url = session->page.document_url,
            .top_level_url = session->page.document_url,
            .method = "GET",
            .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
            .credentials = TILEFINCH_CREDENTIALS_OMIT,
            .destination = TILEFINCH_DESTINATION_FRAME
        };
        if (tilefinch_request_same_site(&frame_hint_context)) {
            (void) browser_session_client_hints_get(
                session->browser_session, frame->url,
                cached_hint_tokens, sizeof(cached_hint_tokens),
                cached_hint_origin, sizeof(cached_hint_origin));
        }
        if (frame->fetch_started_us == 0) {
            frame->fetch_started_us = navigation_now_us();
        }
        frame->fetch_request_id = navigation_enqueue_frame_fetch(
            session, frame,
            cached_hint_tokens[0] == '\0' ? NULL : cached_hint_tokens,
            cached_hint_origin[0] == '\0' ? NULL : cached_hint_origin);
        frame->fetch_pending = frame->fetch_request_id != 0;
        if (append_frame) session->page.frame_count++;
        char parent_origin[NAVIGATION_URL_LIMIT] = {0};
        char child_origin[NAVIGATION_URL_LIMIT] = {0};
        bool same_origin = url_origin(parent_url, parent_origin,
                                      sizeof(parent_origin))
            && url_origin(frame->url, child_origin, sizeof(child_origin))
            && strcmp(parent_origin, child_origin) == 0
            && !frame->opaque_origin;
        if (!script_runtime_set_frame_window_state(
                parent_runtime, frame->parent_handle, true,
                same_origin, frame->opaque_origin, frame->url,
                false,
                navigation_frame_parent_result(session, frame))) return false;
        if (!frame->fetch_pending) {
            session->frames_failed++;
            if (!navigation_dispatch_frame_parent_event(
                    session, parent_runtime,
                    parent_frame_slot_plus_one, node, "error")) {
                return false;
            }
        }
    }
    return navigation_deliver_frame_messages(session);
}

static bool navigation_load_frames(NavigationSession *session,
                                   const char *parent_url)
{
    if (session == NULL || session->page.runtime == NULL
        || parent_url == NULL) return true;
    /* Discovery walks a whole document. Gate each context on its
       connected-mutation generation so a steady runtime tick that mutated
       nothing pays one comparison instead of a bounded DOM traversal per
       context. The stored value is generation + 1: zero means "never
       walked", so the first pass after commit always runs; a src change or a
       script-inserted iframe bumps the generation, and parser-appended frames
       change the node count, either of which re-runs it. */
    uint64_t observed = session->page.document.content_generation + 1u;
    size_t observed_nodes = session->page.document.node_count;
    if (session->page.frame_discovery_generation != observed
        || session->page.frame_discovery_node_count != observed_nodes) {
        if (!navigation_load_frames_for_context(
                session, session->page.runtime, &session->page.document,
                parent_url, 0)) return false;
        session->page.frame_discovery_generation = observed;
        session->page.frame_discovery_node_count = observed_nodes;
    }
    /* frame_count may grow while walking. Each newly completed child is
       considered exactly once per outer runtime tick, and the fixed table
       supplies the nesting/depth bound. */
    for (size_t i = 0; i < session->page.frame_count; i++) {
        NavigationFrame *frame = &session->page.frames[i];
        if (frame->retired || !frame->loaded || frame->runtime == NULL) {
            continue;
        }
        uint64_t frame_observed = frame->document.content_generation + 1u;
        size_t frame_observed_nodes = frame->document.node_count;
        if (frame->discovery_generation == frame_observed
            && frame->discovery_node_count == frame_observed_nodes) continue;
        if (!navigation_load_frames_for_context(
                session, frame->runtime, &frame->document, frame->url,
                i + 1u)) return false;
        frame->discovery_generation = frame_observed;
        frame->discovery_node_count = frame_observed_nodes;
    }
    return true;
}

static bool navigation_frame_presentation_dimensions(
    const NavigationSession *session, const NavigationFrame *frame,
    int *width, int *height)
{
    if (session == NULL || frame == NULL || frame->element == NULL
        || width == NULL || height == NULL) return false;
    int frame_width = 300;
    int frame_height = 150;
    const LayoutDocument *parent_layout = navigation_frame_parent_layout(
        (NavigationSession *) session, frame);
    const LayoutNodeBox *box = parent_layout == NULL ? NULL
        : layout_box_for_node(parent_layout, frame->element);
    if (box != NULL) {
        if (box->client_width > 0) frame_width = box->client_width;
        else if (box->content_width > 0) frame_width = box->content_width;
        else if (box->width > 0) frame_width = box->width;
        if (box->client_height > 0) frame_height = box->client_height;
        else if (box->content_height > 0) frame_height = box->content_height;
        else if (box->height > 0) frame_height = box->height;
    }
    int maximum_width = session->viewport.css_width > 0
        ? session->viewport.css_width : 480;
    int maximum_height = session->viewport.css_height > 0
        ? session->viewport.css_height : 272;
    if (frame_width > maximum_width) frame_width = maximum_width;
    if (frame_height > maximum_height) frame_height = maximum_height;
    if (frame_width < 1 || frame_height < 1) return false;
    *width = frame_width;
    *height = frame_height;
    return true;
}

/* Build one bounded visual snapshot of a child browsing context. The child
   DOM and its control nodes stay inside NavigationFrame; the parent receives
   only opaque pixels attached to the iframe element. This is deliberately a
   mutation-triggered operation rather than a heartbeat repaint. */
static bool navigation_refresh_frame_presentation(
    NavigationSession *session, NavigationFrame *frame)
{
    if (session == NULL || frame == NULL || frame->runtime == NULL
        || frame->document.html == NULL || frame->element == NULL
        || session->images != &session->page.images) return false;
#if !defined(__PSP__)
    if (tilefinch_test_faults()->refuse_next_frame_presentation) {
        tilefinch_test_faults()->refuse_next_frame_presentation = false;
        return false;
    }
#endif
    int width = 0, height = 0;
    if (!navigation_frame_presentation_dimensions(
            session, frame, &width, &height)) return false;
    if ((size_t) width > SIZE_MAX / (size_t) height) return false;
    size_t pixels = (size_t) width * (size_t) height;
    if (pixels > SIZE_MAX / 4u) return false;

    NavigationFramePresentation *fresh = budget_calloc(
        session->budget, 1u, sizeof(*fresh));
    if (fresh == NULL) return false;
    fresh->width = width;
    fresh->height = height;
    ViewportContext viewport;
    int device_width = session->viewport.device_width > 0
        ? session->viewport.device_width : 480;
    int device_height = session->viewport.device_height > 0
        ? session->viewport.device_height : 272;
    /* Nested browsing-context snapshots may be attached after this layout is
       committed. Reserve the complete bounded frame capacity first so the
       ImageResource table cannot relocate underneath retained paint data. */
    /* The snapshot is an image in the parent's CSS pixels: one child CSS
       pixel per snapshot pixel, and the parent's own scale applies when it
       draws the image. Using the physical display as the child's device
       made every frame narrower than the screen paint at display/frame
       scale (1.6x for Turnstile's 300px widget) and crop. */
    bool okay = images_reserve_capacity(
            &frame->images, session->budget, NAVIGATION_FRAME_LIMIT)
        && viewport_context_init(&viewport, width, height, width, height)
        && stylesheet_build_context(
            &fresh->stylesheet, session->budget, &frame->document,
            &viewport)
        && layout_build_context(
            &fresh->layout, session->budget, &frame->document,
            &fresh->stylesheet, session->fonts, &frame->images, &viewport);

    uint16_t *rgb565 = NULL;
    unsigned char *rgba = NULL;
    TileCache cache = {0};
    if (okay) {
        rgb565 = budget_malloc_category(
            session->budget, BUDGET_CATEGORY_RENDER,
            pixels * sizeof(*rgb565));
        rgba = budget_malloc_category(
            session->budget, BUDGET_CATEGORY_RESOURCE, pixels * 4u);
        okay = rgb565 != NULL && rgba != NULL
            && tile_cache_init(&cache, session->budget, &fresh->layout, 2u)
            && tile_cache_set_frame(&cache, rgb565, pixels)
            && tile_cache_render_frame(
                &cache, 0, width, height, NULL);
    }
    if (okay) {
        for (size_t i = 0; i < pixels; i++) {
            unsigned red = tilefinch_rgb565_red_code(rgb565[i]);
            unsigned green = tilefinch_rgb565_green_code(rgb565[i]);
            unsigned blue = tilefinch_rgb565_blue_code(rgb565[i]);
            rgba[i * 4u] = (unsigned char) ((red << 3) | (red >> 2));
            rgba[i * 4u + 1u] =
                (unsigned char) ((green << 2) | (green >> 4));
            rgba[i * 4u + 2u] =
                (unsigned char) ((blue << 3) | (blue >> 2));
            rgba[i * 4u + 3u] = 255u;
        }
        ImageResources *parent_images = navigation_frame_parent_images(
            session, frame);
        okay = parent_images != NULL && images_replace_with_decoded_surface(
            parent_images, session->budget, frame->element,
            rgba, width, height);
    }
    tile_cache_destroy(&cache);
    budget_free(session->budget, rgb565);
    if (!okay) {
        budget_free(session->budget, rgba);
        layout_destroy(&fresh->layout);
        stylesheet_destroy(&fresh->stylesheet);
        budget_free(session->budget, fresh);
        return false;
    }
    /* Ownership of rgba transferred to ImageResources. Mark the opaque
       browsing-context snapshot as cross-origin defensively even though the
       canvas bridge does not admit iframe elements as image sources. */
    ImageResources *parent_images = navigation_frame_parent_images(
        session, frame);
    for (size_t i = 0; parent_images != NULL
                       && i < parent_images->count; i++) {
        ImageResource *resource = &parent_images->items[i];
        if (resource->node == frame->element && !resource->is_mask
            && !resource->is_background) resource->cross_origin = true;
    }

    NavigationFramePresentation *retired = frame->presentation;
    frame->presentation = fresh;
    /* A child browsing context owns the iframe's CSS viewport while screen
       continues to describe the physical display. Publish the new realm
       dimensions only after the replacement snapshot succeeds so a refused
       refresh cannot pair an old layout with new viewport globals. */
    script_runtime_set_viewport(
        frame->runtime, width, height, device_width, device_height);
    script_runtime_set_stylesheet(
        frame->runtime, &frame->presentation->stylesheet);
    script_runtime_set_layout(
        frame->runtime, &frame->presentation->layout, height);
    (void) script_runtime_consume_relayout(frame->runtime);
    if (retired != NULL) {
        layout_destroy(&retired->layout);
        stylesheet_destroy(&retired->stylesheet);
        budget_free(session->budget, retired);
    }
    /* The immediate parent layout must acquire the new DRAW_IMAGE command.
       A nested snapshot is then propagated outward one bounded frame at a
       time; the top-level page alone publishes final damage. */
    NavigationFrame *parent = navigation_frame_parent(session, frame);
    return parent == NULL ? navigation_relayout(session)
                          : navigation_refresh_frame_presentation_or_defer(
                                session, parent);
}

/* A child mutation has already committed before its opaque snapshot is
   rebuilt. Refusing the bounded layout/render allocation is therefore a
   deferrable publication failure while the owning page remains live. Keep
   the dirty bit latched so a later owner tick retries instead of leaving the
   old child pixels permanently published. */
static bool navigation_refresh_frame_presentation_or_defer(
    NavigationSession *session, NavigationFrame *frame)
{
    if (session == NULL || frame == NULL) return false;
    frame->presentation_dirty = false;
    if (navigation_refresh_frame_presentation(session, frame)) return true;
    if (!session->page.loaded) return false;
    frame->presentation_dirty = true;
    return true;
}

#if !defined(__PSP__)
void navigation_test_refuse_next_frame_presentation(void)
{
    tilefinch_test_faults()->refuse_next_frame_presentation = true;
}

bool navigation_test_refresh_frame_presentation(
    NavigationSession *session, NavigationFrame *frame)
{
    return navigation_refresh_frame_presentation(session, frame);
}
#endif

static bool navigation_install_frame_capability_trace(
    NavigationFrame *frame)
{
    if (frame == NULL || frame->runtime == NULL) return false;
    static const char trace_setup[] =
        "(()=>{const seen=new Map(),missing=new Set(),traced=new WeakMap(),rawByProxy=new WeakMap();"
        "globalThis.__tilefinchCapabilityTrace={seen,missing};"
        "const record=name=>seen.set(name,(seen.get(name)||0)+1),raw=value=>rawByProxy.get(value)||value,rawArgs=args=>args.map(raw),traceObject=(target,label,depth=1)=>{"
        "if(target===null||(typeof target!=='object'&&typeof target!=='function'))return target;if(rawByProxy.has(target)||traced.has(target))return rawByProxy.has(target)?target:traced.get(target);const proxy=new Proxy(target,{"
        "get(object,key){let name=label;if(typeof key!=='symbol'){name=label+'.'+String(key);record(name);if(!(key in object))missing.add(name);}const descriptor=Reflect.getOwnPropertyDescriptor(object,key);if(descriptor&&Object.prototype.hasOwnProperty.call(descriptor,'value')&&!descriptor.configurable&&!descriptor.writable)return descriptor.value;const value=Reflect.get(object,key,object);return depth===0&&typeof value!=='function'?value:traceObject(value,name,Math.max(0,depth-1));},"
        "set(object,key,value){if(typeof key!=='symbol'){const name=label+'.'+String(key)+'=';record(name);if(!(key in object))missing.add(name);}return Reflect.set(object,key,raw(value),object);},"
        "apply(target,thisArg,args){const name='call:'+label;record(name);return Reflect.apply(target,raw(thisArg),rawArgs(args));},"
        "construct(target,args,newTarget){const name='new:'+label;record(name);return Reflect.construct(target,rawArgs(args),raw(newTarget));}});traced.set(target,proxy);rawByProxy.set(proxy,target);return proxy;};"
        /* Do not proxy document here. DOM bootstrap bookkeeping performs
           thousands of private-property probes while a challenge builds
           its shadow tree; tracing those probes both hides the useful
           browser-capability signal and can make the validation run hit
           its heap ceiling. The objects below are the standards surfaces
           this diagnostic is intended to inventory. */
        "for(const name of ['performance','screen','history'])if(globalThis[name])globalThis[name]=traceObject(globalThis[name],name);"
        "for(const name of ['XMLHttpRequest','PerformanceObserver','Blob','URL','Worker','MutationObserver'])if(typeof globalThis[name]==='function')globalThis[name]=traceObject(globalThis[name],name);})();";
    return script_runtime_evaluate_diagnostic(
        frame->runtime, trace_setup, "<frame-capability-trace>",
        &frame->script_result);
}

#if !defined(__PSP__)
bool navigation_test_install_frame_capability_trace(
    NavigationFrame *frame)
{
    return navigation_install_frame_capability_trace(frame);
}
#endif

static bool navigation_complete_frame(NavigationSession *session,
                                      NavigationFrame *frame,
                                      bool fetched_ok, FetchResult *fetch)
{
    uint64_t response_completed_us = navigation_now_us();
    ScriptNavigationTiming navigation_timing = navigation_timing_from_fetch(
        frame->fetch_started_us, response_completed_us, 0, fetch);
    char navigation_content_type[sizeof(fetch->content_type)];
    snprintf(navigation_content_type, sizeof(navigation_content_type), "%s",
             fetch->content_type);
    navigation_timing.content_type = navigation_content_type;
    frame->fetch_pending = false;
    frame->http_status = fetch->status_code;
    snprintf(frame->server, sizeof(frame->server), "%s", fetch->server);
    snprintf(frame->cf_ray, sizeof(frame->cf_ray), "%s", fetch->cf_ray);
    char request_referrer_policy[sizeof(frame->referrer_policy)];
    snprintf(request_referrer_policy, sizeof(request_referrer_policy), "%s",
             frame->referrer_policy);
    char response_referrer_policy[sizeof(frame->referrer_policy)];
    snprintf(response_referrer_policy, sizeof(response_referrer_policy),
             "%s", frame->referrer_policy);
    bool referrer_policy_header_present = false;
    if (!fetch_response_referrer_policy(
            fetch, &referrer_policy_header_present,
            response_referrer_policy)) {
        /* A live/v9 result carries this policy outside the bounded ordinary
           header snapshot. Corrupt direct metadata is a response failure,
           not permission to fall back to a possibly truncated field list. */
        fetched_ok = false;
        response_referrer_policy[0] = '\0';
    }
    TilefinchRequestContext request_context =
        navigation_frame_request_context(session, frame);
    if (fetched_ok) {
        navigation_accept_response_cookies(
            session->browser_session, &request_context, fetch,
            fetch->effective_url[0] == '\0'
                ? frame->url : fetch->effective_url);
    }
    const char *effective = fetch->effective_url[0] == '\0'
        ? frame->url : fetch->effective_url;
    /* Provider-owned pages are native top-level surfaces, not executable
       embedded service applications. Cookies above still honor HTTP policy. */
    if (site_adapter_handles_navigation("GET", effective)) fetched_ok = false;
    char document_referrer[NAVIGATION_URL_LIMIT] = {0};
    /* A non-serializable initiator (for example a local about: document)
       produces an empty Document.referrer; it does not invalidate an
       otherwise admitted and successfully fetched child response. */
    (void) fetch_compute_referrer(
        frame->parent_url, effective, request_referrer_policy,
        document_referrer, sizeof(document_referrer));
    TilefinchRequestContext top_hint_context = {
        .target_url = effective,
        .initiator_url = session->page.document_url,
        .top_level_url = session->page.document_url,
        .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_OMIT,
        .destination = TILEFINCH_DESTINATION_FRAME
    };
    bool top_level_hint_eligible =
        tilefinch_request_same_site(&top_hint_context);
    char prior_hints[BROWSER_CLIENT_HINT_TOKEN_LIMIT] = {0};
    char prior_hint_origin[BROWSER_ORIGIN_LIMIT] = {0};
    char accepted_hints[BROWSER_CLIENT_HINT_TOKEN_LIMIT] = {0};
    char accepted_critical[BROWSER_CLIENT_HINT_TOKEN_LIMIT] = {0};
    if (top_level_hint_eligible) {
        (void) browser_session_client_hints_get(
            session->browser_session, effective,
            prior_hints, sizeof(prior_hints),
            prior_hint_origin, sizeof(prior_hint_origin));
    }
    bool hints_valid = fetched_ok && top_level_hint_eligible
        && fetch_accepted_critical_client_hints(
               fetch->accept_ch, fetch->accept_ch,
               accepted_hints, sizeof(accepted_hints));
    bool critical_valid = fetched_ok && top_level_hint_eligible
        && fetch_accepted_critical_client_hints(
               fetch->accept_ch, fetch->critical_ch,
               accepted_critical, sizeof(accepted_critical));
    bool retry_client_hints = fetched_ok
        && !frame->client_hint_retried
        && tilefinch_url_potentially_trustworthy(effective)
        && critical_valid && accepted_critical[0] != '\0'
        && !fetch_client_hint_tokens_cover(
               prior_hints, accepted_critical);
    if (hints_valid && accepted_hints[0] != '\0') {
        (void) browser_session_client_hints_put(
            session->browser_session, effective, accepted_hints);
    }
    if (retry_client_hints) {
        char retry_url[NAVIGATION_URL_LIMIT];
        if (!tilefinch_url_request_key(effective, retry_url,
                                    sizeof(retry_url))) {
            fetch_result_destroy(fetch);
            session->frames_failed++;
            return navigation_dispatch_frame_parent_event(
                session, frame->parent_runtime,
                frame->parent_frame_slot_plus_one,
                frame->element, "error");
        }
        char hint_origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
        if (!tilefinch_url_origin(retry_url, hint_origin,
                              sizeof(hint_origin))) {
            fetch_result_destroy(fetch);
            session->frames_failed++;
            return navigation_dispatch_frame_parent_event(
                session, frame->parent_runtime,
                frame->parent_frame_slot_plus_one,
                frame->element, "error");
        }
        /* Critical-CH belongs to the final response origin, not to an earlier
           redirector.  Restart that canonical URL directly so high-entropy
           hints and response-path cookies are sent only to the origin which
           requested them.  parent_url remains the immutable frame initiator
           used for SameSite, Referrer, and Sec-Fetch-Site decisions. */
        snprintf(frame->url, sizeof(frame->url), "%s", retry_url);
        frame->client_hint_retried = true;
        session->client_hint_retries++;
        frame->fetch_request_id = navigation_enqueue_frame_fetch(
            session, frame,
            accepted_hints[0] == '\0' ? accepted_critical : accepted_hints,
            hint_origin);
        fetch_result_destroy(fetch);
        frame->fetch_pending = frame->fetch_request_id != 0;
        if (!frame->fetch_pending) {
            session->frames_failed++;
            if (!navigation_dispatch_frame_parent_event(
                    session, frame->parent_runtime,
                    frame->parent_frame_slot_plus_one,
                    frame->element, "error")) return false;
        }
        return true;
    }
    snprintf(frame->referrer_policy, sizeof(frame->referrer_policy), "%s",
             response_referrer_policy);
    char final_url[NAVIGATION_URL_LIMIT];
    snprintf(final_url, sizeof(final_url), "%s", effective);
    bool final_same_origin = tilefinch_url_same_origin(
        frame->parent_url, final_url);
    bool exposed_same_origin = final_same_origin && !frame->opaque_origin;
    bool scripts_allowed = tilefinch_frame_sandbox_allows(
        frame->sandbox_flags, TILEFINCH_SANDBOX_ALLOW_SCRIPTS);
    TilefinchContentSecurityPolicy response_csp;
    bool security_snapshot_truncated =
        fetch->security.version == FETCH_RESPONSE_SECURITY_METADATA_VERSION
        ? fetch->security.csp_snapshot_state
              == FETCH_SECURITY_FIELD_TRUNCATED
          || fetch->security.frame_policy_snapshot_state
              == FETCH_SECURITY_FIELD_TRUNCATED
        : fetch->response_security_headers_truncated;
    bool response_policy_valid = tilefinch_csp_parse_response_headers(
        &response_csp, final_url, fetch->response_headers,
        fetch->response_headers_length,
        security_snapshot_truncated);
    bool embedding_allowed = response_policy_valid
        && tilefinch_frame_embedding_allowed(
               &response_csp, final_url, frame->parent_url,
               fetch->response_headers, fetch->response_headers_length);
    bool ok = fetched_ok && fetch->status_code >= 200
              && fetch->status_code < 400
              && embedding_allowed
              && document_parse_with_transport_encoding(
                     &frame->document, session->budget, fetch->data,
                     fetch->length, 512,
                     tilefinch_encoding_from_content_type(
                         fetch->content_type));
    if (ok) {
        frame->document.content_security_policy = response_csp;
        if (response_csp.header_present)
            (void) document_nonce_hiding_enable(&frame->document);
    }
    fetch_result_destroy(fetch);
    if (ok) {
        /* Redirects change both the child document URL and the WindowProxy
           origin boundary. Do not retain the requested URL's permissions. */
        snprintf(frame->url, sizeof(frame->url), "%s", final_url);
        ok = script_runtime_set_frame_window_state(
            frame->parent_runtime, frame->parent_handle, true,
            exposed_same_origin, frame->opaque_origin, final_url,
            true,
            navigation_frame_parent_result(session, frame));
    }
    if (ok && scripts_allowed) {
        int frame_width = 0, frame_height = 0;
        ViewportContext frame_viewport;
        ok = navigation_frame_presentation_dimensions(
                session, frame, &frame_width, &frame_height)
            && viewport_context_init(
                &frame_viewport, frame_width, frame_height,
                session->viewport.device_width > 0
                    ? session->viewport.device_width : 480,
                session->viewport.device_height > 0
                    ? session->viewport.device_height : 272);
        if (!ok) {
            frame->runtime = NULL;
        } else {
            frame->runtime = navigation_create_runtime(
                session, &frame->document, frame->url,
                document_referrer, frame->fetch_started_us,
                frame->referrer_policy, true,
                &frame_viewport,
                SCRIPT_DOCUMENT_SCOPE_CHILD_FRAME,
                session->page.fetch_domain,
                NULL, NULL, session->fonts,
                &frame->script_result);
        }
        ok = frame->runtime != NULL
            && script_runtime_configure_messaging(
                frame->runtime, true, exposed_same_origin,
                frame->opaque_origin,
                frame->parent_frame_slot_plus_one == 0,
                navigation_queue_frame_message,
                session);
        if (ok) script_runtime_set_images(frame->runtime, &frame->images);
    }
    if (ok && frame->runtime != NULL && NAVIGATION_TRACES_FRAME_CAPABILITIES(session)) {
        ok = navigation_install_frame_capability_trace(frame);
    }
    if (ok && frame->runtime != NULL) {
        (void) script_runtime_record_navigation_timing(
            frame->runtime, &navigation_timing);
        ExternalScriptMetrics metrics = {0};
        char frame_base_url[NAVIGATION_URL_LIMIT];
        ok = navigation_document_resource_base(
            &frame->document, frame->url, frame_base_url);
        if (ok) {
            ok = document_scripts_execute(
                &frame->document, frame->runtime, session->budget,
                session->browser_session, frame_base_url, frame->url,
                frame->referrer_policy, 16,
                session->maximum_script_bytes > 1024u * 1024u
                    ? session->maximum_script_bytes : 1024u * 1024u,
                navigation_frame_script_file_bytes(session), 15000,
                session->page.resource_scheduler,
                &metrics, &frame->script_result)
                && script_runtime_finish_loading(frame->runtime,
                                                 &frame->script_result);
        }
    }
    if (!ok && navigation_retire_failed_document_refresh(
            session, frame->runtime)) return false;
    if (!ok) {
        session->frames_failed++;
        navigation_drop_frame_messages(session, frame);
        frame->fetch_pending = false;
        frame->loaded = false;
        frame->retired = true;
        bool state_ok = script_runtime_set_frame_window_state(
            frame->parent_runtime, frame->parent_handle, false, false, false,
            NULL, false, navigation_frame_parent_result(session, frame));
        bool event_ok = navigation_dispatch_frame_parent_event(
            session, frame->parent_runtime,
            frame->parent_frame_slot_plus_one,
            frame->element, "error");
        navigation_frame_presentation_destroy(session, frame);
        script_runtime_destroy(frame->runtime);
        frame->runtime = NULL;
        images_destroy(&frame->images);
        document_destroy(&frame->document);
        return state_ok && event_ok;
    }
    frame->loaded = true;
    session->frames_loaded++;
    if (!navigation_refresh_frame_presentation_or_defer(
            session, frame)) return false;
    if (tilefinch_trace_js_roots()) {
        script_runtime_report_roots(frame->runtime, stderr, "frame-loaded",
                                    "child", frame->parent_handle);
    }
    return navigation_dispatch_frame_parent_event(
        session, frame->parent_runtime,
        frame->parent_frame_slot_plus_one,
        frame->element, "load");
}

static bool navigation_progress_frames(NavigationSession *session)
{
    if (!navigation_retire_detached_frames(session)) return false;
    if (session->page.resource_scheduler == NULL) return true;
    (void) fetch_scheduler_pump(session->page.resource_scheduler,
                                NAVIGATION_FRAME_LIMIT, 4);
    for (size_t i = 0; i < session->page.frame_count; i++) {
        NavigationFrame *frame = &session->page.frames[i];
        if (!frame->fetch_pending) continue;
        bool fetched_ok = false;
        FetchResult *fetch = fetch_result_create(session->budget);
        if (fetch == NULL) return false;
        if (!fetch_scheduler_take(session->page.resource_scheduler,
                                  frame->fetch_request_id,
                                  &fetched_ok, fetch)) {
            fetch_result_free(fetch);
            continue;
        }
        if (!navigation_complete_frame(session, frame, fetched_ok, fetch)) {
            fetch_result_free(fetch);
            return false;
        }
        fetch_result_free(fetch);
    }
    return navigation_deliver_frame_messages(session);
}

bool navigation_collect_frame_capability_trace(NavigationSession *session)
{
    if (session == NULL || !NAVIGATION_TRACES_FRAME_CAPABILITIES(session)) return true;
    static const char report[] =
        "{globalThis.__tilefinchFrameSummaryBeforeTrace=globalThis.pocSummary;"
        "const trace=globalThis.__tilefinchCapabilityTrace,"
        "missing=trace?[...trace.missing].filter(name=>"
        "!/^element[.][a-z0-9-]+[.]__(handle|shadow)/.test(name)).sort():[],"
        "seen=trace?[...trace.seen.entries()].sort((a,b)=>"
        "b[1]-a[1]||a[0].localeCompare(b[0])).slice(0,64):[];"
        "globalThis.pocSummary='missing='+missing.join(',')+'|seen='+"
        "seen.map(item=>item[0]+'*'+item[1]).join(',')+'|evals='+"
        "(Array.isArray(globalThis.__tilefinchEvalSources)?"
        "globalThis.__tilefinchEvalSources.map(source=>source.length).join(','):"
        "'unavailable');}";
    for (size_t i = 0; i < session->page.frame_count; i++) {
        NavigationFrame *frame = &session->page.frames[i];
        char function_name[64] = {0};
        if (frame->runtime != NULL
            && stack_function_name(frame->script_result.last_reject_stack,
                                   function_name,
                                    sizeof(function_name))) {
            char select_function[320];
            snprintf(select_function, sizeof(select_function),
                     "try{globalThis.__tilefinchRejectFunction="
                     "__tilefinchDiagnosticLookup('%s')}catch(error){"
                     "globalThis.__tilefinchRejectLookupError=String(error)}",
                     function_name);
            if (!script_runtime_evaluate_diagnostic(
                    frame->runtime, select_function,
                    "<frame-reject-function>", &frame->script_result)) {
                return false;
            }
        }
        if (frame->runtime != NULL) {
            if (!script_runtime_evaluate_diagnostic(
                    frame->runtime, report, "<frame-capability-report>",
                    &frame->script_result)) return false;
            snprintf(frame->capability_trace,
                     sizeof(frame->capability_trace), "%s",
                     frame->script_result.summary);
            snprintf(session->last_frame_trace,
                     sizeof(session->last_frame_trace), "%s",
                     frame->capability_trace);
            snprintf(session->last_frame_trace_url,
                     sizeof(session->last_frame_trace_url), "%s",
                     frame->url);
            static const char restore[] =
                "globalThis.pocSummary="
                "globalThis.__tilefinchFrameSummaryBeforeTrace;";
            (void) script_runtime_evaluate_diagnostic(
                frame->runtime, restore, "<frame-capability-restore>",
                &frame->script_result);
        }
        collect_stack_source_context(frame,
            frame->script_result.last_reject_stack);
    }
    return true;
}
