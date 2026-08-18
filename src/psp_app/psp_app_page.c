/* Page load, reader presentation, session persistence, and profile-backed
 * document pages for the PSP browser. The built-in start surface is native
 * chrome and never comes through this generator.
 */
#include "psp_app_internal.h"

PspProfilePageKind psp_profile_page_kind(const char *url)
{
    if (url == NULL) return PSP_PROFILE_PAGE_NONE;
    if (strcmp(url, BROWSER_PROFILE_HOMEPAGE_URL) == 0)
        return PSP_PROFILE_PAGE_HOMEPAGE;
    if (strcmp(url, "https://tilefinch.local/bookmarks") == 0)
        return PSP_PROFILE_PAGE_BOOKMARKS;
    if (strcmp(url, "https://tilefinch.local/history") == 0)
        return PSP_PROFILE_PAGE_HISTORY;
    return PSP_PROFILE_PAGE_NONE;
}

bool psp_set_presentation_css(
    BrowserEngine *engine, const PspUiState *ui,
    const BrowserProfile *profile, bool reader_mode,
    const char *url, unsigned font_percent, bool relayout)
{
    char css[SITE_ADAPTER_READER_CSS_LIMIT];
    size_t length = 0;
    if (reader_mode) {
        char adapter[32];
        if (!site_adapter_reader_css(
                url,
                ui->reader_font_serif
                    ? SITE_ADAPTER_READER_FONT_SERIF
                    : SITE_ADAPTER_READER_FONT_SANS,
                font_percent, css, sizeof(css), adapter, sizeof(adapter))) {
            return false;
        }
        length = strlen(css);
        printf(
            "tilefinch-reader: adapter=%s site-scale=%u font=%s css=%zuB\n",
            adapter, font_percent,
            ui->reader_font_serif ? "serif" : "sans", length);
    } else if (font_percent != 100u) {
        int written = snprintf(
            css, sizeof(css), "html{font-size:%u%%!important}",
            font_percent);
        if (written < 0 || (size_t) written >= sizeof(css)) return false;
        length = (size_t) written;
    } else {
        css[0] = '\0';
    }
    bool cosmetic = profile != NULL
        && browser_profile_content_blocker_mode(profile)
               != CONTENT_BLOCKER_OFF
        && browser_profile_content_blocker_cosmetic_hiding(profile)
        && url != NULL
        && !browser_profile_content_blocker_site_allowed(profile, url);
    if (cosmetic) {
        if (length != 0) {
            if (length + 1u >= sizeof(css)) return false;
            css[length++] = '\n';
        }
        size_t cosmetic_length = 0;
        if (!content_blocker_cosmetic_css(
                css + length, sizeof(css) - length, &cosmetic_length))
            return false;
        length += cosmetic_length;
    }
    if (browser_profile_cookie_banner_hidden(profile, url)) {
        if (length != 0) {
            if (length + 1u >= sizeof(css)) return false;
            css[length++] = '\n';
        }
        size_t cookie_length = 0;
        if (!content_blocker_cookie_banner_css(
                css + length, sizeof(css) - length, &cookie_length))
            return false;
        length += cookie_length;
    }
    return relayout
        ? browser_engine_apply_user_css(engine, css, length)
        : browser_engine_set_user_css(engine, css, length);
}

void psp_leave_reader_for_navigation(
    BrowserEngine *engine, PspUiState *ui, const BrowserProfile *profile,
    const char *url)
{
    unsigned global_percent = browser_profile_page_font_percent(profile);
    ui->reader_mode = false;
    ui->page_font_percent = global_percent;
    if (!psp_set_presentation_css(
            engine, ui, profile, false, url, global_percent, false)) {
        /* Never let a page-specific reader sheet leak into the next origin.
           Removing all user CSS is the allocation-free safe fallback. */
        (void) browser_engine_set_user_css(engine, "", 0);
    }
}

bool psp_reader_navigation_prepare(
    BrowserEngine *engine, PspUiState *ui, const BrowserProfile *profile,
    PspReaderNavigation *navigation, const char *url)
{
    if (engine == NULL || ui == NULL || profile == NULL
        || navigation == NULL || url == NULL || !ui->reader_mode
        || navigation->pending) {
        return false;
    }
    unsigned destination_percent =
        browser_profile_page_font_percent(profile);
    if (ui->remember_reader_site_scale) {
        (void) browser_profile_reader_site_font_percent(
            profile, url, &destination_percent);
    }
    PspReaderNavigation prepared = {
        .pending = true,
        .incumbent_percent = ui->page_font_percent,
        .destination_percent = destination_percent
    };
    /* Set, rather than apply, the destination sheet. The incumbent pixels
       stay immutable while the candidate is loading, while the candidate's
       very first stylesheet build sees Reader CSS and avoids doing a full
       author layout only to throw it away. */
    if (!psp_set_presentation_css(
            engine, ui, profile, true, url,
            destination_percent, false)) {
        return false;
    }
    *navigation = prepared;
    ui->page_font_percent = destination_percent;
    return true;
}

void psp_reader_navigation_finish(
    BrowserEngine *engine, PspUiState *ui, const BrowserProfile *profile,
    PspReaderNavigation *navigation, const char *current_url,
    bool succeeded)
{
    if (engine == NULL || ui == NULL || profile == NULL
        || navigation == NULL || !navigation->pending) {
        return;
    }
    if (succeeded) {
        ui->reader_mode = true;
        ui->page_font_percent = navigation->destination_percent;
    } else {
        const char *incumbent_url =
            current_url == NULL ? ui->url : current_url;
        ui->reader_mode = true;
        ui->page_font_percent = navigation->incumbent_percent;
        /* Candidate failure leaves the incumbent page and its already-drawn
           Reader layout intact. Restore only the configured sheet so the
           next rebuild or navigation cannot inherit the failed target's
           site adapter. */
        if (!psp_set_presentation_css(
                engine, ui, profile, true, incumbent_url,
                navigation->incumbent_percent, false)) {
            (void) browser_engine_set_user_css(engine, "", 0);
            ui->reader_mode = false;
            ui->page_font_percent =
                browser_profile_page_font_percent(profile);
        }
    }
    *navigation = (PspReaderNavigation) {0};
}

static bool psp_navigation_start_retryable(const char *error)
{
    return error != NULL
        && (strstr(error, "memory") != NULL
            || strstr(error, "scheduler") != NULL
            || strstr(error, "allocation") != NULL
            || strstr(error, "curl") != NULL
            || strstr(error, "budget") != NULL);
}

static bool psp_retry_navigation_url_after_reclaim(
    BrowserEngine *engine, const char *url, size_t maximum_bytes,
    long timeout_ms, bool record_history)
{
    if (browser_engine_begin_navigation_url(
            engine, url, maximum_bytes, timeout_ms, record_history)) {
        return true;
    }
    char first_error[256];
    snprintf(first_error, sizeof(first_error), "%s",
             browser_engine_last_error(engine));
    if (!psp_navigation_start_retryable(first_error)) return false;
    BrowserOptionalMemoryReclaim reclaim = {0};
    if (!browser_engine_reclaim_optional_memory(engine, &reclaim)
        || reclaim.total_bytes == 0) return false;
    printf("tilefinch-navigation-retry: kind=url reclaimed=%zu "
           "first=\"%.160s\"\n", reclaim.total_bytes, first_error);
    return browser_engine_begin_navigation_url(
        engine, url, maximum_bytes, timeout_ms, record_history);
}

bool psp_retry_navigation_action_after_reclaim(
    BrowserEngine *engine, const ControllerAction *action,
    size_t maximum_bytes, long timeout_ms)
{
    if (browser_engine_begin_navigation_action(
            engine, action, maximum_bytes, timeout_ms)) return true;
    char first_error[256];
    snprintf(first_error, sizeof(first_error), "%s",
             browser_engine_last_error(engine));
    if (!psp_navigation_start_retryable(first_error)) return false;
    BrowserOptionalMemoryReclaim reclaim = {0};
    if (!browser_engine_reclaim_optional_memory(engine, &reclaim)
        || reclaim.total_bytes == 0) return false;
    printf("tilefinch-navigation-retry: kind=action reclaimed=%zu "
           "first=\"%.160s\"\n", reclaim.total_bytes, first_error);
    return browser_engine_begin_navigation_action(
        engine, action, maximum_bytes, timeout_ms);
}

/* Failure-only diagnostics must not inflate the already-ratcheted controller
   loop on Allegrex. Keep this out of main even under whole-TU optimization. */
__attribute__((noinline))
void psp_report_job_failure(
    const char *kind, const char *checkpoint, int status, long http_status,
    const char *error)
{
    printf("tilefinch-job-failure: kind=%s status=%d http=%ld "
           "error=\"%.300s\" system-free=%d system-largest=%d\n",
           kind == NULL ? "unknown" : kind, status, http_status,
           error == NULL ? "" : error,
           sceKernelTotalFreeMemSize(), sceKernelMaxFreeMemSize());
    if (checkpoint != NULL) psp_log_checkpoint(checkpoint);
}

bool psp_begin_page_load(BrowserEngine *engine, PspUiState *ui,
                                const BrowserProfile *profile,
                                const uint16_t *frame,
                                PspTextInputService *text_input,
                                const char *url, bool record_history,
                                size_t maximum_bytes, long timeout_ms)
{
    BrowserSession *security_session = browser_engine_session(engine);
    if (security_session != NULL) {
        (void) browser_session_set_third_party_cookie_site_allowed(
            security_session, url,
            browser_profile_third_party_cookie_site_allowed(profile, url));
    }
    bool javascript_allowed =
        browser_profile_javascript_allowed_for_url(profile, url);
    if (!browser_engine_set_javascript_enabled(engine, javascript_allowed)) {
        psp_ui_show_status(ui, "JAVASCRIPT POLICY UNAVAILABLE", 240);
        return false;
    }
    psp_leave_reader_for_navigation(engine, ui, profile, url);
    /* The destination belongs to chrome as soon as the user accepts it.
       Candidate navigation still preserves the incumbent page pixels and
       history until commit, but leaving its old URL in the omnibar makes the
       browser look as though it ignored the request. */
    psp_ui_set_navigation_target(ui, url);
    psp_ui_set_loading(ui, true, -1);
    psp_ui_show_status(ui, "LOADING  O CANCEL", 120);
    psp_navigation_cooperate_begin(ui, frame, engine);
    psp_text_input_before_navigation(text_input);
    bool started = psp_retry_navigation_url_after_reclaim(
        engine, url, maximum_bytes, timeout_ms, record_history);
    if (!started) {
        (void) psp_write_failure_report(
            "navigation-start", browser_engine_last_error(engine), url,
            0, 0);
        psp_report_job_failure(
            "navigation-url-start", "interactive-navigation-start-failed",
            -1, 0, browser_engine_last_error(engine));
        psp_navigation_cooperate_end("interactive-start-failed");
        psp_ui_set_loading(ui, false, 0);
        psp_ui_show_status(ui, browser_engine_last_error(engine), 300);
    }
    return started;
}

bool psp_replace_focused_text(
    BrowserEngine *engine, const uint16_t *frame, PspUiState *ui,
    PspTextInputService *text_input, bool voice_requested,
    bool *submit_requested)
{
    if (submit_requested != NULL) *submit_requested = false;
    ControllerTextInputInfo text_info = {0};
    if (!browser_engine_text_input_info(engine, &text_info)
        || !text_info.editable
        || (voice_requested && !text_info.voice_allowed)) {
        if (voice_requested)
            psp_ui_show_status(ui, "VOICE UNAVAILABLE FOR THIS FIELD", 180);
        return false;
    }
    char current[PSP_TEXT_INPUT_CAPACITY + 1] = {0};
    char replacement[PSP_TEXT_INPUT_CAPACITY + 1] = {0};
    size_t current_length = 0;
    (void) browser_engine_text_value(
        engine, current, sizeof(current), &current_length);
    (void) current_length;
    PspTextInputRequest request = {
        .description = "ENTER TEXT",
        .initial = current,
        .keyboard_url_mode = text_info.keyboard_url_mode,
        .allow_submit = !text_info.multiline
    };
    bool requested_submit = false;
    bool accepted = voice_requested
        ? psp_text_input_request_voice(
              text_input, frame, ui, &request,
              replacement, sizeof(replacement))
        : psp_text_input_request_with_submit(
              text_input, frame, ui, &request,
              replacement, sizeof(replacement), &requested_submit);
    if (!accepted
        || !browser_engine_replace_text(
               engine, replacement, strlen(replacement))) return false;
    if (submit_requested != NULL) *submit_requested = requested_submit;
    if (!requested_submit) {
        psp_ui_show_status(
            ui,
            request.allow_submit
                ? "TEXT UPDATED - START ENTER"
                : "TEXT UPDATED",
            120);
    }
    return true;
}

BrowserNavigationJobQuota psp_navigation_quota(void)
{
    return (BrowserNavigationJobQuota) {
        .load = {
            .fetch = {
                .maximum_body_bytes = 16u * KIB,
            .maximum_body_callbacks = 1,
            .maximum_time_us = 2000
        },
            /* PSP Lexbor/script callbacks can make a 16 KiB parser delivery
               visibly monopolize the UI. Keep the same byte stream and
               ordering while returning to controller polling more often. */
            .maximum_parser_body_bytes = 2u * KIB,
            .maximum_parser_time_us = 2000,
            .maximum_finalize_work_units = 2048,
            .maximum_finalize_time_us = 2000
        }
    };
}

bool psp_run_initial_page_load(
    BrowserEngine *engine, PspUiState *ui, const uint16_t *frame,
    const char *url, size_t maximum_bytes, long timeout_ms,
    const char *argv0, bool dump_provisional, bool *stopped)
{
    if (stopped != NULL) *stopped = false;
    psp_ui_set_loading(ui, true, -1);
    psp_ui_show_status(ui, "LOADING PAGE  O CANCEL", 600);
    psp_navigation_cooperate_begin(ui, frame, engine);
    if (!browser_engine_begin_navigation_url(
            engine, url, maximum_bytes, timeout_ms, true)) {
        psp_navigation_cooperate_end("initial-start-failed");
        return false;
    }
    uint64_t started_us = (uint64_t) sceKernelGetSystemTimeWide();
    BrowserNavigationJobStatus status = BROWSER_NAVIGATION_JOB_PENDING;
    bool provisional_dump_attempted = false;
    bool provisional_dumped = false;
    size_t pump_boundaries = 0;
    while (status == BROWSER_NAVIGATION_JOB_PENDING) {
        psp_log_heartbeat();
        SceCtrlData pad = {0};
        if (sceCtrlPeekBufferPositive(&pad, 1) > 0
            && (pad.Buttons & PSP_CTRL_CIRCLE) != 0) {
            browser_engine_cancel_navigation(
                engine, "cancelled from PSP browser UI");
            status = BROWSER_NAVIGATION_JOB_CANCELLED;
        } else {
            BrowserNavigationJobQuota quota = psp_navigation_quota();
            status = browser_engine_pump_navigation(engine, &quota);
            /*
             * The pump returning is a cooperation point in its own right:
             * the loop polls CIRCLE at the top of the next pass, so the
             * console is responsive here. Say so. Without it the gap clock
             * ran from the session's first byte to the commit's first parse
             * window, so a download that yielded three hundred times was
             * reported as one un-yielding stretch -- and, worse, a HOME exit
             * pressed during the download was not turned into a cancellation
             * until the commit reached its first checkpoint, because only
             * this checkpoint samples psp_home_exit_pending().
             */
            pump_boundaries++;
            if (!tilefinch_platform_cooperate(
                    "navigation-pump", pump_boundaries)
                && status == BROWSER_NAVIGATION_JOB_PENDING) {
                browser_engine_cancel_navigation(
                    engine, "cancelled from PSP browser UI");
                status = BROWSER_NAVIGATION_JOB_CANCELLED;
            }
            if (psp_navigation_cancel_requested()) {
                if (status == BROWSER_NAVIGATION_JOB_PENDING) {
                    browser_engine_cancel_navigation(
                        engine, "cancelled from PSP browser UI");
                }
                status = BROWSER_NAVIGATION_JOB_CANCELLED;
            }
        }
        if (dump_provisional && !provisional_dump_attempted) {
            BrowserProvisionalViewport provisional = {0};
            if (browser_engine_provisional_viewport(engine, &provisional)) {
                provisional_dump_attempted = true;
                uint32_t dump_operation =
                    psp_log_operation_begin("provisional-frame-dump");
                provisional_dumped = psp_dump_frame_named(
                    argv0, "frame-provisional.ppm",
                    provisional.pixels, provisional.pixel_count);
                printf(
                    "tilefinch-psp-script: provisional-frame-dumped=%d "
                    "frame=%zu/%zu y=%d\n",
                    provisional_dumped ? 1 : 0,
                    provisional.current_frame + 1u,
                    provisional.frame_count, provisional.scroll_y);
                psp_log_operation_end(
                    dump_operation, "provisional-frame-dump",
                    provisional_dumped ? "ok" : "failed");
            }
        }
        BrowserNavigationJobMetrics metrics = {0};
        (void) browser_engine_navigation_job_metrics(engine, &metrics);
        psp_ui_set_loading(
            ui, true, (int) metrics.completion_per_mille);
        uint64_t now_us = (uint64_t) sceKernelGetSystemTimeWide();
        if (status == BROWSER_NAVIGATION_JOB_PENDING
            && now_us - started_us >= PSP_NAVIGATION_JOB_TIMEOUT_US) {
            browser_engine_cancel_navigation(
                engine, "PSP navigation watchdog expired");
            status = BROWSER_NAVIGATION_JOB_CANCELLED;
        }
    }
    BrowserNavigationJobMetrics metrics = {0};
    (void) browser_engine_navigation_job_metrics(engine, &metrics);
    BrowserEngineMetrics engine_metrics = {0};
    (void) browser_engine_metrics(engine, &engine_metrics);
    const NavigationPerformance *performance =
        &engine_metrics.navigation;
    printf("tilefinch-navigation-job: initial status=%d pumps=%zu "
           "body=%zuB yields=%zu max-pump=%lluus parser-max=%lluus "
           "preview=%zu/%zu nav-start=%lluus capture=%lluus first=%lluus "
           "scrolls=%zu y=%d bytes=%zu "
           "headers=%lluus first-body=%lluus first-dom=%lluus "
           "css-preload-timeout=%zuB/%zu/%zu "
           "complete=%zuB/%lluus failure=%zu/%zuB/%ld/\"%s\" "
           "source=%zu nodes=%zu style-refresh=%lluus "
           "preview-work=%lluus/%lluus/%zu/%zu/%lluus/%lluus/%lluus "
           "preview-raster=%lluus/%lluus/%lluus/%lluus "
           "style-builds=%zu continuation=%zu/%zu/%lluus "
           "rules=%zu+%zu discovery=%lluus context=%lluus append=%lluus "
           "refresh-failures=%zu "
           "parser=%lluus runtime=%lluus css=%lluus "
           "script=%lluus/%lluus "
           "finalize=%lluus transform=%lluus/%zu/%zu "
           "irreducible=%lluus/%zu elapsed=%lluus\n",
           (int) status, metrics.pump_calls, metrics.load.body_bytes,
           metrics.load.quota_yields,
           (unsigned long long) metrics.load.maximum_pump_us,
           (unsigned long long) metrics.load.maximum_parser_pump_us,
           metrics.provisional_paints,
           metrics.provisional_frame_count,
           (unsigned long long) metrics.navigation_session_started_us,
           (unsigned long long) metrics.provisional_capture_started_us,
           (unsigned long long) metrics.provisional_first_present_us,
           metrics.provisional_scrolls,
           metrics.provisional_scroll_y,
           metrics.provisional_bytes,
           (unsigned long long) performance->response_headers_us,
           (unsigned long long) performance->first_body_byte_us,
           (unsigned long long) performance->first_dom_us,
           performance->stylesheet_preload_timeout_bytes,
           performance->stylesheet_preload_timeout_requests,
           performance->stylesheet_preload_timeout_responses,
           performance->stylesheet_preload_completed_bytes,
           (unsigned long long)
               performance->stylesheet_preload_last_completed_us,
           performance->stylesheet_preload_failure_stage,
           performance->stylesheet_preload_failure_bytes,
           performance->stylesheet_preload_failure_status,
           performance->stylesheet_preload_failure_error,
           performance->streaming_preview_source_bytes,
           performance->streaming_preview_node_count,
           (unsigned long long)
               performance->streaming_preview_style_refresh_us,
           (unsigned long long)
               performance->streaming_preview_layout_us,
           (unsigned long long)
               performance->streaming_preview_paint_us,
           performance->streaming_preview_attempts,
           performance->streaming_preview_empty_raster_skips,
           (unsigned long long)
               performance->streaming_preview_first_attempt_us,
           (unsigned long long)
               performance->streaming_preview_last_attempt_us,
           (unsigned long long)
               performance->streaming_preview_content_ready_us,
           (unsigned long long)
               performance->streaming_preview_frame_alloc_us,
           (unsigned long long)
               performance->streaming_preview_cache_init_us,
           (unsigned long long)
               performance->streaming_preview_raster_us,
           (unsigned long long)
               performance->streaming_preview_present_us,
           performance->blocking_stylesheet_builds,
           performance->blocking_stylesheet_continuations,
           performance->blocking_stylesheet_continuation_fallbacks,
           (unsigned long long)
               performance->blocking_stylesheet_continuation_us,
           performance->blocking_stylesheet_continuation_rules_before,
           performance->blocking_stylesheet_continuation_rules,
           (unsigned long long)
               performance->blocking_stylesheet_continuation_discovery_us,
           (unsigned long long)
               performance->blocking_stylesheet_continuation_context_us,
           (unsigned long long)
               performance->blocking_stylesheet_continuation_append_us,
           performance->streaming_preview_style_refresh_failures,
           (unsigned long long) performance->parser_feed_us,
           (unsigned long long) performance->parser_runtime_startup_us,
           (unsigned long long) performance->parser_stylesheet_us,
           (unsigned long long) performance->parser_script_compile_us,
           (unsigned long long) performance->parser_script_execute_us,
           (unsigned long long) metrics.load.finalize_us,
           (unsigned long long) metrics.maximum_transform_slice_us,
           metrics.transform_slices,
           metrics.transform_quota_overruns,
           (unsigned long long) metrics.maximum_irreducible_unit_us,
           metrics.irreducible_unit_overruns,
           (unsigned long long) metrics.elapsed_us);
    psp_report_blocking_script_samples(performance);
    psp_report_background_transport_metrics();
    if (dump_provisional && !provisional_dump_attempted) {
        printf("tilefinch-psp-script: provisional-frame-dumped=0 "
               "reason=unavailable\n");
    }
    psp_ui_set_loading(
        ui, false,
        status == BROWSER_NAVIGATION_JOB_SUCCEEDED ? 1000 : 0);
    if (stopped != NULL)
        *stopped = status == BROWSER_NAVIGATION_JOB_CANCELLED;
    psp_navigation_cooperate_end("initial");
    return status == BROWSER_NAVIGATION_JOB_SUCCEEDED;
}

BrowserSessionPersistenceLimits psp_site_data_limits(
    unsigned cache_megabytes)
{
    BrowserSessionPersistenceLimits limits;
    browser_session_persistence_limits_default(&limits);
    if (cache_megabytes != 0) {
        limits.maximum_cache_bytes = (size_t) cache_megabytes * MIB;
    }
    return limits;
}

bool psp_site_data_load(
    BrowserSession *session, const char *path,
    BrowserSessionPersistenceMask mask,
    const BrowserSessionPersistenceLimits *limits)
{
    BrowserSessionPersistenceStatus status =
        browser_session_persistence_load(session, path, mask, limits);
    bool loaded = status == BROWSER_SESSION_PERSISTENCE_OK;
    if (!loaded && status != BROWSER_SESSION_PERSISTENCE_NOT_FOUND) {
        printf("tilefinch-site-data: load=%s path=%s\n",
               browser_session_persistence_status_name(status), path);
    }
    return loaded;
}

bool psp_site_data_save(
    const BrowserSession *session, const char *path,
    BrowserSessionPersistenceMask mask,
    const BrowserSessionPersistenceLimits *limits)
{
    BrowserSessionPersistenceStatus status =
        browser_session_persistence_save(session, path, mask, limits);
    if (status != BROWSER_SESSION_PERSISTENCE_OK) {
        printf("tilefinch-site-data: save=%s path=%s\n",
               browser_session_persistence_status_name(status), path);
        return false;
    }
    return true;
}

void psp_report_blocking_script_samples(
    const NavigationPerformance *performance)
{
    if (performance == NULL) return;
    for (size_t i = 0; i < performance->blocking_script_sample_count; i++) {
        const NavigationBlockingScriptSample *sample =
            &performance->blocking_script_samples[i];
        printf("tilefinch-navigation-script: ordinal=%zu external=%d "
               "source=%zuB nodes=%zu mutations=%zu total=%lluus "
               "metadata=%lluus runtime=%lluus stylesheet=%lluus "
               "fingerprint=%lluus process=%lluus compile=%lluus "
               "host-callback=%lluus execute=%lluus ok=%d\n",
               sample->ordinal, sample->external ? 1 : 0,
               sample->source_bytes, sample->node_count,
               sample->dom_mutations,
               (unsigned long long) sample->total_us,
               (unsigned long long) sample->metadata_us,
               (unsigned long long) sample->runtime_startup_us,
               (unsigned long long) sample->stylesheet_us,
               (unsigned long long) sample->stylesheet_fingerprint_us,
               (unsigned long long) sample->process_us,
               (unsigned long long) sample->compile_us,
               (unsigned long long) sample->host_callback_us,
               (unsigned long long) sample->execute_us,
               sample->succeeded ? 1 : 0);
    }
    if (performance->blocking_script_samples_dropped != 0) {
        printf("tilefinch-navigation-script: dropped=%zu\n",
               performance->blocking_script_samples_dropped);
    }
}

__attribute__((noinline, cold))
void psp_report_background_transport_metrics(void)
{
    FetchBackgroundTransportMetrics metrics = {0};
    if (!fetch_background_transport_metrics(&metrics)) return;
    printf("tilefinch-background-transport: stream-starts=%zu "
           "fixed-starts=%zu peak-stream=%zu peak-fixed=%zu "
           "slots=%zu queued=%zu running=%zu complete=%zu "
           "performs=%zu polls=%zu headers=%zu bodies=%zu "
           "completions=%zu multi=%d running=%d\n",
           metrics.streaming_started,
           metrics.fixed_started,
           metrics.peak_streaming_active,
           metrics.peak_fixed_active,
           metrics.occupied_slots,
           metrics.queued_slots,
           metrics.running_slots,
           metrics.complete_slots,
           metrics.worker_performs,
           metrics.worker_polls,
           metrics.header_callbacks,
           metrics.body_callbacks,
           metrics.completions,
           metrics.last_multi_code,
           metrics.last_running);
    printf("tilefinch-background-transport-worker: perform-max=%uus "
           "perform-over-33ms=%u perform-over-100ms=%u "
           "setup=%u/%uus steady-max=%uus priority-failures=%u "
           "service-max=%uus poll-max=%uus loop-max=%uus "
           "run-clocks=%lluus preemptions=%u priority=%d\n",
           metrics.worker_perform_max_us,
           metrics.worker_perform_over_33ms,
           metrics.worker_perform_over_100ms,
           metrics.worker_setup_performs,
           metrics.worker_setup_perform_max_us,
           metrics.worker_steady_perform_max_us,
           metrics.worker_priority_failures,
           metrics.worker_service_max_us,
           metrics.worker_poll_max_us,
           metrics.worker_loop_max_us,
           (unsigned long long) metrics.worker_run_clocks,
           metrics.worker_thread_preemptions,
           metrics.worker_priority);
}

void psp_report_budget_counters(const Budget *budget,
                                       const char *phase)
{
    if (budget == NULL) return;
    printf("tilefinch-budget: phase=%s current=%zu peak=%zu limit=%zu "
           "allocs=%zu frees=%zu failures=%zu external=%zu/%zu\n",
           phase == NULL ? "unknown" : phase,
           budget->current, budget->peak, budget->limit,
           budget->allocation_count, budget->free_count,
           budget->failure_count, budget->external_reserved,
           budget->external_reserved_peak);
    for (size_t index = 0; index < BUDGET_CATEGORY_COUNT; index++) {
        const BudgetCategoryStats *stats = &budget->categories[index];
        printf("tilefinch-budget-category: phase=%s name=%s current=%zu "
               "peak=%zu active=%zu allocs=%zu frees=%zu\n",
               phase == NULL ? "unknown" : phase,
               budget_category_name((BudgetCategory) index),
               stats->current, stats->peak, stats->active_allocations,
               stats->allocation_count, stats->free_count);
    }
}

void psp_profile_record_current(
    BrowserProfile *profile, PspProfileStore *store,
    const NavigationSession *navigation, uint64_t now_us)
{
    const NavigationEntry *entry = navigation_current(navigation);
    if (entry == NULL || entry->url == NULL
        || strncmp(entry->url, "tilefinch://", 12u) == 0
        || strncmp(
               entry->url, "https://tilefinch.local/",
               strlen("https://tilefinch.local/")) == 0)
        return;
    if (browser_profile_record_history(
            profile, entry->url, entry->title))
        psp_profile_store_mark_dirty(store, now_us);
}

bool psp_recovery_record_current(
    const BrowserProfile *profile, const char *path,
    const NavigationSession *navigation)
{
    if (!browser_profile_restore_last_page(profile)
        || path == NULL || navigation == NULL) return false;
    const NavigationEntry *entry = navigation_current(navigation);
    if (entry == NULL || entry->url == NULL
        || strncmp(
               entry->url, "https://tilefinch.local/",
               strlen("https://tilefinch.local/")) == 0) {
        return false;
    }
    bool saved = browser_recovery_save(
        path, entry->url, entry->scroll_y);
    if (!saved) {
        printf("tilefinch-recovery: save failed\n");
    }
    return saved;
}

bool psp_profile_open_page_history(
    BrowserEngine *engine, PspUiState *ui, BrowserProfile *profile,
    PspProfilePageKind kind, bool record_history)
{
    if (kind == PSP_PROFILE_PAGE_NONE) return false;
    char *html = NULL;
    size_t length = 0;
    bool built = kind == PSP_PROFILE_PAGE_HOMEPAGE
               ? browser_profile_build_homepage(profile, &html, &length)
               : browser_profile_build_page(
                     profile, kind == PSP_PROFILE_PAGE_HISTORY,
                     &html, &length);
    if (!built)
        return false;
    const char *url = kind == PSP_PROFILE_PAGE_HOMEPAGE
               ? BROWSER_PROFILE_HOMEPAGE_URL
               : (kind == PSP_PROFILE_PAGE_HISTORY
                      ? "https://tilefinch.local/history"
                      : "https://tilefinch.local/bookmarks");
    psp_leave_reader_for_navigation(engine, ui, profile, url);
    bool loaded = browser_engine_commit_html(
        engine, url, html, length, record_history);
    budget_free(browser_engine_budget(engine), html);
    if (loaded) {
        loaded = browser_engine_refresh_shell(engine)
            && browser_engine_render_frame(engine, NULL);
    }
    psp_sync_ui(ui, engine, profile);
    return loaded;
}

bool psp_profile_open_page(
    BrowserEngine *engine, PspUiState *ui, BrowserProfile *profile,
    PspProfilePageKind kind)
{
    return psp_profile_open_page_history(
        engine, ui, profile, kind, true);
}
