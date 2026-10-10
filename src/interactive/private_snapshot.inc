#include "tilefinch/pixel_math.h"

/* Never export exception text: an author error can contain a password, URL,
   token or account identifier. Only these fixed, browser-owned categories
   cross the private diagnostic boundary. */
static const char *private_snapshot_error_kind(const char *message)
{
    if (message == NULL || *message == '\0') return "none";
    static const struct { const char *prefix; const char *kind; } types[] = {
        {"TypeError:", "type"}, {"RangeError:", "range"},
        {"ReferenceError:", "reference"}, {"SyntaxError:", "syntax"},
        {"InternalError:", "internal"}, {"Error:", "error"}
    };
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
        if (strncmp(message, types[i].prefix, strlen(types[i].prefix)) == 0)
            return types[i].kind;
    return "other";
}

/* Match browser-owned failure prefixes only. Never serialize the message,
   which can contain a navigation target or an author exception. */
static const char *private_snapshot_navigation_error_kind(const char *message)
{
    if (message == NULL || *message == '\0') return "none";
    static const struct { const char *prefix; const char *kind; } types[] = {
        {"candidate session staging", "candidate-memory"},
        {"navigation streaming startup", "stream-setup"},
        {"HTML parser", "html"}, {"HTML parsing", "html"},
        {"stylesheet", "style"}, {"post-script stylesheet", "style"},
        {"layout", "layout"}, {"resumable layout", "layout"},
        {"JavaScript runtime", "runtime"}, {"document script", "script"},
        {"candidate commit", "commit"}, {"candidate page", "candidate"},
        {"navigation URL", "url-policy"}, {"navigation source", "url-policy"},
        {"effective navigation URL", "url-policy"},
        {"response security", "response-policy"},
        {"response effective", "response-policy"},
        {"document referrer", "response-policy"},
        {"external font", "font"}, {"visible image", "priority-image"},
        {"external image", "image"}, {"history", "history"}
    };
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
        if (strncmp(message, types[i].prefix, strlen(types[i].prefix)) == 0)
            return types[i].kind;
    return "other";
}

/* Private-console diagnostic: redact a visual copy before rasterization or
   serialization. Never mutate the live DOM or change its field values. */
static bool private_snapshot_contains(const char *text, size_t length,
                                      const char *value, size_t value_length)
{
    if (value == NULL || value_length == 0 || value_length > length) return false;
    for (size_t i = 0; i <= length - value_length; i++)
        if (memcmp(text + i, value, value_length) == 0) return true;
    return false;
}

static bool private_snapshot_page(BrowserEngine *engine, NavigationSession *navigation, const char *path)
{
    enum { MAX_COMMANDS = 16384, MAX_HEIGHT = 4352, MAX_TEXT = 65536 };
    if (path == NULL || *path == '\0' || !navigation->page.loaded) return false;
    size_t budget_current = navigation->budget->current;
    size_t budget_peak = navigation->budget->peak;
    size_t budget_failures = navigation->budget->failure_count;
    const LayoutDocument *source = &navigation->page.layout;
    if (source->count > MAX_COMMANDS || source->control_count > 256) return false;
    LayoutDocument visual = {0};
    TileCache cache = {0};
    uint16_t *frame = NULL;
    FILE *image = NULL, *text = NULL;
    char text_path[1024];
    int written = snprintf(text_path, sizeof(text_path), "%s.txt", path);
    if (written <= 0 || (size_t) written >= sizeof(text_path)) return false;
    bool ok = layout_clone_visual(&visual, source);
    if (!ok) return false;
    for (size_t i = 0; i < visual.count; i++) {
        DrawCommand *command = &visual.commands[i];
        bool redact = false;
        for (size_t j = 0; j < source->control_count && !redact; j++) {
            const ControlRegion *control = &source->controls[j];
            if (control->type != CONTROL_INPUT && control->type != CONTROL_TEXTAREA
                && control->type != CONTROL_EDITABLE) continue;
            const LayoutNodeBox *box = layout_box_for_node(source, control->node);
            /* Refuse if ownership is unavailable instead of guessing the
               painted rectangle of transformed, clipped or bidi fields. */
            if (box == NULL) { ok = false; goto done; }
            redact = i >= box->command_start && i < box->command_end;
            size_t length = 0;
            const char *value = document_control_value(control->node, &length);
            if (value == NULL)
                value = document_attribute(control->node, "value", &length);
            if (!redact && command->type == DRAW_TEXT && command->text != NULL)
                redact = private_snapshot_contains(command->text,
                    command->text_length, value, length)
                    || private_snapshot_contains(value, length,
                        command->text, command->text_length);
        }
        if (redact) {
            /* Changing type also excludes retained bidi glyphs and shadows. */
            *command = (DrawCommand) {
                .type = DRAW_FILL_RECT, .color = 0xff555555u,
                .x = command->x, .y = command->y,
                .width = command->width, .height = command->height,
                .z_index = command->z_index,
                .opacity_scale = command->opacity_scale
            };
        }
    }
    ok = tile_cache_init(&cache, source->budget, &visual, 8);
    if (!ok) goto done;
    frame = budget_malloc(source->budget, 480u * 272u * sizeof(*frame));
    ok = frame != NULL && tile_cache_set_frame(&cache, frame, 480u * 272u);
    if (!ok) goto done;
    int height = visual.height < 272 ? 272 : visual.height;
    if (height > MAX_HEIGHT) height = MAX_HEIGHT;
    image = fopen(path, "wb");
    text = fopen(text_path, "wb");
    if (image == NULL || text == NULL
        || chmod(path, 0600) != 0 || chmod(text_path, 0600) != 0) {
        ok = false; goto done;
    }
    ok = fprintf(image, "P6\n480 %d\n255\n", height) > 0;
    ok = ok && fprintf(text, "Redacted rendered text; page height %d, capture height %d.\n",
                       visual.height, height) > 0;
    const NavigationEntry *visible_entry = navigation_current(navigation);
    ok = ok && fprintf(text, "Presentation diagnostics: scroll-y=%d\n",
                       visible_entry == NULL ? 0 : visible_entry->scroll_y) > 0;
    const ScriptResult *js = &navigation->page.script_result;
    size_t script_elements = 0, executable_elements = 0, forms = 0, hidden_inputs = 0;
    size_t load_handlers = 0, refresh_elements = 0;
    lxb_dom_node_t *root = navigation->page.document.html == NULL ? NULL
        : lxb_dom_interface_node(navigation->page.document.html);
    lxb_dom_node_t *node = root;
    for (size_t visited = 0; node != NULL && visited < 16384u; visited++) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            script_elements += node->local_name == LXB_TAG_SCRIPT;
            forms += node->local_name == LXB_TAG_FORM;
            size_t length = 0;
            const char *type = document_attribute(node, "type", &length);
            if (node->local_name == LXB_TAG_SCRIPT)
                executable_elements += script_type_attribute_classify(type, length, NULL);
            hidden_inputs += node->local_name == LXB_TAG_INPUT && type != NULL
                && length == 6u && strncasecmp(type, "hidden", 6u) == 0;
            const char *load = document_attribute(node, "onload", &length);
            load_handlers += load != NULL && length != 0;
            refresh_elements += document_is_refresh_meta(node);
        }
        if (node->first_child != NULL) { node = node->first_child; continue; }
        while (node != NULL && node != root && node->next == NULL) node = node->parent;
        if (node == NULL || node == root) break;
        node = node->next;
    }
    ok = ok && fprintf(text,
        "Document diagnostics: nodes=%zu script-enabled=%u discovered=%zu "
        "attempted=%zu loaded=%zu failed=%zu quota-skips=%zu pressure-skips=%zu "
        "refresh-state=%u refresh-examined=%u refresh-delay=%u frames=%zu "
        "script-elements=%zu executable-elements=%zu forms=%zu hidden-inputs=%zu "
        "load-handlers=%zu refresh-elements=%zu realm-enabled=%u heap-limit=%zu "
        "timeout=%u degraded=%u optional-sheds=%zu\n",
        navigation->page.document.node_count,
        navigation->document_scripts_enabled,
        navigation->script_discovered, navigation->script_attempted,
        navigation->script_loaded, navigation->script_failed,
        navigation->script_skipped_quota, navigation->script_skipped_pressure,
        (unsigned) navigation->page.refresh.state,
        navigation->page.refresh.examined,
        navigation->page.refresh.delay_seconds,
        navigation->page.frame_count, script_elements, executable_elements,
        forms, hidden_inputs, load_handlers, refresh_elements,
        navigation->scripts_enabled, navigation->js_memory_limit,
        navigation->js_timeout_ms, navigation->page.script_degradation_observed,
        navigation->performance.optional_work_sheds) > 0;
    ok = ok && fprintf(text,
        "Runtime diagnostics: script-failures=%zu dynamic-failures=%zu "
        "dynamic-quota-refusals=%zu request-quota-refusals=%zu "
        "active-requests=%zu queued-requests=%zu xhr-sends=%zu "
        "xhr-responses=%zu xhr-status=%ld unhandled-rejections=%zu "
        "handled-rejections=%zu rejection-kind=%s callback-kind=%s "
        "script-kind=%s xhr-bytes=%zu xhr-text-units=%zu timers=%zu "
        "pending-tasks=%zu pending-network=%zu heap-refusals=%zu "
        "dynamic-queued=%zu dynamic-completed=%zu\n",
        js->scripts_failed, js->dynamic_scripts_failed,
        js->dynamic_scripts_quota_rejected, js->async_network_quota_rejected,
        js->async_network_active_native, js->async_network_pending_logical,
        js->xhr_send_calls, js->xhr_response_count, js->last_xhr_status,
        js->promise_rejections, js->promise_rejections_handled,
        private_snapshot_error_kind(js->last_promise_rejection),
        private_snapshot_error_kind(js->last_uncaught_callback_error),
        private_snapshot_error_kind(js->error),
        js->last_xhr_response_bytes, js->last_xhr_response_text_units,
        js->root_timers, js->pending_tasks, js->root_pending_network,
        script_runtime_heap_rejections(navigation->page.runtime),
        js->dynamic_scripts_queued, js->dynamic_scripts_completed) > 0;
    ok = ok && fprintf(text,
        "Dynamic fetch diagnostics: dynamic-response-limit-failures=%zu "
        "dynamic-memory-failures=%zu dynamic-transport-failures=%zu "
        "dynamic-authority-failures=%zu dynamic-source-failures=%zu\n",
        js->dynamic_script_response_limit_failures,
        js->dynamic_script_memory_failures,
        js->dynamic_script_transport_failures,
        js->dynamic_script_authority_failures,
        js->dynamic_script_source_failures) > 0;
    ScriptSchedulerSnapshot scheduler = {0};
    bool scheduler_ok = script_runtime_scheduler_snapshot(
        navigation->page.runtime, &scheduler);
    ok = ok && fprintf(text,
        "Scheduler diagnostics: available=%u due-timers=%zu "
        "checkpoint-continuations=%zu promise-jobs=%u classic-continuation=%u "
        "pending-navigation=%u\n",
        scheduler_ok, scheduler.due_timers, scheduler.checkpoint_continuations,
        scheduler.quickjs_jobs_pending, scheduler.classic_script_continuation,
        script_runtime_has_pending_navigation(navigation->page.runtime)) > 0;
    ScriptXHRDiagnostics responses = {0};
    BrowserCookieDiagnostics cookie_diagnostics = {0};
    browser_session_cookie_diagnostics(navigation->browser_session, false,
                                       &cookie_diagnostics);
    BrowserSiteDataUsage google_usage = {0}, youtube_usage = {0};
    (void) browser_session_site_data_usage(navigation->browser_session,
        "https://accounts.google.com/", &google_usage);
    (void) browser_session_site_data_usage(navigation->browser_session,
        "https://m.youtube.com/", &youtube_usage);
    ok = ok && fprintf(text,
        "Cookie admission diagnostics: enabled=%u http=%zu script=%zu refused=%zu "
        "byte-refusals=%zu slot-refusals=%zu allocation-refusals=%zu "
        "policy-ignored=%zu evictions=%zu peak-values=%zu limit=%zu "
        "google-count=%zu google-bytes=%zu youtube-count=%zu youtube-bytes=%zu\n",
        cookie_diagnostics.enabled, cookie_diagnostics.http_attempts,
        cookie_diagnostics.script_attempts, cookie_diagnostics.refused,
        cookie_diagnostics.byte_refusals, cookie_diagnostics.slot_refusals,
        cookie_diagnostics.allocation_refusals, cookie_diagnostics.policy_ignored,
        cookie_diagnostics.evictions, cookie_diagnostics.peak_value_bytes,
        navigation->browser_session == NULL ? 0 : navigation->browser_session->maximum_cookie_bytes,
        google_usage.cookie_count, google_usage.cookie_bytes,
        youtube_usage.cookie_count, youtube_usage.cookie_bytes) > 0;
    ok = ok && fprintf(text,
        "YouTube feed diagnostics: attempted=%u complete=%u bootstrap-status=%ld "
        "bootstrap-bytes=%zu same-origin=%u destination=%u login-known=%u logged-in=%u "
        "authorization-present=%u api-status=%ld api-bytes=%zu json=%u error=%u "
        "api-login-known=%u api-logged-out=%u results=%zu message=%u signin-required=%u\n",
        private_youtube_feed_attempted, private_youtube_feed_okay,
        private_youtube_feed.bootstrap_status, private_youtube_feed.bootstrap_bytes,
        private_youtube_feed.bootstrap_same_origin, private_youtube_feed.bootstrap_destination,
        private_youtube_feed.login_known,
        private_youtube_feed.logged_in, private_youtube_feed.authorization_present,
        private_youtube_feed.api_status,
        private_youtube_feed.api_bytes, private_youtube_feed.api_json,
        private_youtube_feed.api_error, private_youtube_feed.api_login_known,
        private_youtube_feed.api_logged_out, private_youtube_feed.result_count,
        private_youtube_feed.api_message, private_youtube_feed.api_signin_required) > 0;
    ok = ok && fprintf(text,
        "Page memory diagnostics: current=%zu peak=%zu limit=%zu refusals=%zu\n",
        budget_current, budget_peak, navigation->budget->limit,
        budget_failures) > 0;
    size_t provider_rows = 0;
    for (size_t i = 0; i < navigation->page.layout.node_box_count; i++) {
        size_t length = 0;
        const char *name = document_attribute(navigation->page.layout.node_boxes[i].node,
                                              "class", &length);
        if (name != NULL && length == 10 && memcmp(name, "result-row", 10) == 0)
            provider_rows++;
    }
    ok = ok && fprintf(text, "YouTube rating diagnostics: state=%u selected=%u\n",
        (unsigned) browser_engine_youtube_like_state(engine),
        browser_engine_youtube_like_selected(engine) ? 1u : 0u) > 0;
    YoutubeLiteRatingDiagnostics rating = {0};
    (void) browser_engine_youtube_rating_diagnostics(engine, &rating);
    ok = ok && fprintf(text, "YouTube rating response diagnostics: bootstrap=%ld api=%ld bytes=%zu phase=%u flags=%u\n",
        rating.bootstrap_status, rating.api_status, rating.api_bytes,
        rating.phase, rating.response_flags) > 0;
    ok = ok && fprintf(text, "YouTube provider diagnostics: state=%u rows=%zu\n",
        (unsigned) browser_engine_youtube_feed_state(engine), provider_rows) > 0;
    ok = ok && fprintf(text, "YouTube session diagnostics: authenticated=%u history-flags=%u cookies=%zu\n",
        youtube_lite_session_signed_in(browser_engine_session(engine)) ? 1u : 0u,
        browser_engine_youtube_history_metadata(engine), youtube_usage.cookie_count) > 0;
    unsigned entered = script_runtime_private_probe(navigation->page.runtime, false);
    unsigned failed = script_runtime_private_probe(navigation->page.runtime, true);
    ok = ok && fprintf(text, "Local stage diagnostics: entered=%u failed=%u\n",
                       entered, failed) > 0;
    bool responses_ok = script_runtime_xhr_diagnostics(
        navigation->page.runtime, false, &responses);
    ok = ok && fprintf(text,
        "Navigation diagnostics: accepted=%zu refused=%zu consumed=%zu\n",
        responses.navigation_accepted, responses.navigation_refused,
        responses.navigation_consumed) > 0;
    ok = ok && fprintf(text,
        "Navigation outcome diagnostics: http-status=%ld transport-code=%ld "
        "tls-failed=%u timed-out=%u error-present=%u committed=%zu error-kind=%s\n",
        navigation->last_http_status, navigation->last_transport_code,
        navigation->last_tls_verification_failed,
        navigation->last_transport_timed_out,
        navigation->last_error[0] != '\0', navigation->loads_committed,
        private_snapshot_navigation_error_kind(navigation->last_error)) > 0;
    unsigned layout_phase = 0, layout_cancelled = 0;
    size_t layout_refusals = 0, layout_current = 0, layout_limit = 0;
    bool layout_diagnostic = sscanf(navigation->last_error,
        "layout construction failed phase=%u cancelled=%u refusals=%zu current=%zu limit=%zu",
        &layout_phase, &layout_cancelled, &layout_refusals,
        &layout_current, &layout_limit) == 5;
    ok = ok && fprintf(text,
        "Layout failure diagnostics: available=%u phase=%u cancelled=%u refusals=%zu current=%zu limit=%zu\n",
        layout_diagnostic, layout_diagnostic ? layout_phase : 0,
        layout_diagnostic ? layout_cancelled : 0,
        layout_diagnostic ? layout_refusals : 0,
        layout_diagnostic ? layout_current : 0,
        layout_diagnostic ? layout_limit : 0) > 0;
    ok = ok && fprintf(text, "Response diagnostics: available=%u count=%zu "
                       "overwritten=%zu json-observer-installed=%u json-parse-calls=%zu "
                       "json-parse-failures=%zu\n", responses_ok, responses.count,
                       responses.overwritten, (unsigned) responses.json_parse_observer_installed,
                       responses.json_parse_calls,
                       responses.json_parse_failures) > 0;
#if !defined(__PSP__)
    ok = ok && fprintf(text, "Script staging census: count=%zu overwritten=%zu\n",
                       responses.staging_count, responses.staging_overwritten) > 0;
    for (size_t i = 0; i < responses.staging_count && i < 16 && ok; i++) {
        const size_t *r = responses.staging_rows[i];
        ok = fprintf(text,
            "Script staging diagnostics: sequence=%zu bound=%zu free=%zu other=%zu dom=%zu js=%zu style=%zu resource=%zu layout=%zu render=%zu session=%zu navigation=%zu\n",
            r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9],
            r[10], r[11]) > 0;
    }
#endif
    ok = ok && fprintf(text, "Outcome diagnostics: installed=%u unrecognized=%zu "
        "rejected=%zu verification-required=%zu redirect=%zu "
        "credential-transition=%zu protocol-error=%zu challenge-update=%zu\n",
        (unsigned) responses.outcome_classifier_installed,
        responses.outcome_counts[0], responses.outcome_counts[1],
        responses.outcome_counts[2], responses.outcome_counts[3],
        responses.outcome_counts[4], responses.outcome_counts[5],
        responses.outcome_counts[6]) > 0;
    ok = ok && fprintf(text, "Handoff diagnostics: navigation=%zu form-post=%zu "
        "native-account=%zu close=%zu prerequisite=%zu other=%zu\n",
        responses.outcome_counts[7], responses.outcome_counts[8],
        responses.outcome_counts[9], responses.outcome_counts[10],
        responses.outcome_counts[11], responses.outcome_counts[12]) > 0;
    ok = ok && fprintf(text, "Promise outcome diagnostics: type=%zu range=%zu "
        "reference=%zu syntax=%zu internal=%zu error=%zu other=%zu undefined=%zu\n",
        responses.rejection_kinds[0], responses.rejection_kinds[1],
        responses.rejection_kinds[2], responses.rejection_kinds[3],
        responses.rejection_kinds[4], responses.rejection_kinds[5],
        responses.rejection_kinds[6], responses.rejection_kinds[7]) > 0;
    for (size_t i = 0; i < SCRIPT_PRIVATE_OUTCOME_COUNT && ok; i++)
        if (responses.rejection_stages[i] != 0)
            ok = fprintf(text, "Promise stage diagnostics: stage=%zu count=%zu\n",
                         i, responses.rejection_stages[i]) > 0;
    for (size_t i = 0; i < responses.count && ok; i++) {
        const uint32_t *r = responses.rows[i];
        ok = fprintf(text, "XHR metadata: sequence=%u status=%u bytes=%u "
            "text-units=%u phase=%u text-reads=%u response-reads=%u "
            "state-events=%u progress-events=%u load-events=%u\n",
            (unsigned) r[0], (unsigned) r[1], (unsigned) r[2], (unsigned) r[3],
            (unsigned) r[4], (unsigned) r[5], (unsigned) r[6], (unsigned) r[7],
            (unsigned) r[8], (unsigned) r[9]) > 0;
    }
    size_t text_bytes = 0;
    for (size_t i = 0; i < visual.count && ok && text_bytes < MAX_TEXT; i++) {
        const DrawCommand *command = &visual.commands[i];
        if (command->type != DRAW_TEXT || command->text == NULL) continue;
        size_t length = command->text_length;
        if (length > MAX_TEXT - text_bytes) length = MAX_TEXT - text_bytes;
        ok = fwrite(command->text, 1, length, text) == length;
        text_bytes += length;
        if (text_bytes < MAX_TEXT) {
            ok = ok && fputc('\n', text) != EOF;
            text_bytes++;
        }
    }
    unsigned char row[480 * 3];
    for (int y = 0; y < height && ok; y += 272) {
        int maximum_scroll = visual.height > 272 ? visual.height - 272 : 0;
        int origin = y < maximum_scroll ? y : maximum_scroll;
        ok = tile_cache_render_frame(&cache, origin, 480, 272, NULL);
        int rows = height - y < 272 ? height - y : 272;
        for (int r = 0; r < rows && ok; r++) {
            for (int x = 0; x < 480; x++) {
                uint16_t pixel = frame[(size_t) (r + y - origin) * 480 + (size_t) x];
                row[x * 3] = tilefinch_rgb565_red_code(pixel) * 255u / 31u;
                row[x * 3 + 1] = tilefinch_rgb565_green_code(pixel) * 255u / 63u;
                row[x * 3 + 2] = tilefinch_rgb565_blue_code(pixel) * 255u / 31u;
            }
            ok = fwrite(row, 1, sizeof(row), image) == sizeof(row);
        }
    }
done:
    if (image != NULL && fclose(image) != 0) ok = false;
    if (text != NULL && fclose(text) != 0) ok = false;
    tile_cache_destroy(&cache);
    budget_free(source->budget, frame);
    layout_destroy(&visual);
    if (!ok) { unlink(path); unlink(text_path); }
    return ok;
}
