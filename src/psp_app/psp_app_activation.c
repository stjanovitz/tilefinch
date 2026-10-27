/* Page activation for the PSP frontend: a Cross (or Enter) press resolved
 * into the page's ControllerAction, and that action routed to media, an
 * internal or local destination, a download or a navigation. Split out of
 * psp_app_dispatch_heavy_action along its two halves (F-R7) so the routing
 * decision can grow host coverage without the resolution's PSP-only
 * cooperate and text-entry machinery.
 */
#include "psp_app_internal.h"

/* Resolves the press. Returns false when it was consumed without an
   action to route (a status was shown, focus moved or text entry ran). */
__attribute__((noinline))
bool psp_app_activate_page(
    PspApp *app, PspAppFrameState *frame, bool enter_only,
    const uint16_t *engine_frame, PspPageActivation *activation)
{
    BrowserEngine *engine = app->browser->engine;
    ControllerAction *action = &activation->action;
    bool activated = false;
    bool preview = browser_engine_navigation_pending(engine);
    if (preview) {
        if (enter_only) return false;
        /* X on the loading page: a web link leaves it at once; a
           control or a same-page link acts once it has loaded. */
        BrowserPreviewActivation outcome =
            browser_engine_provisional_activate(
                engine, frame->preview_pointer,
                frame->preview_pointer_x, frame->preview_pointer_y,
                action);
        if (outcome == BROWSER_PREVIEW_ACTIVATION_DEFERRED) {
            psp_ui_show_status(&app->process->presentation.ui,
                               "OPENS WHEN THE PAGE LOADS", 120);
            return false;
        }
        if (outcome != BROWSER_PREVIEW_ACTIVATION_NAVIGATE) return false;
        browser_engine_cancel_navigation(
            engine, "superseded by a link on the loading page");
        psp_navigation_cooperate_end("preview-link");
        (void) psp_engine_views_refresh(app->views, engine);
        activated = true;
    }
    size_t activations_before = 0;
    if (!preview) {
        if (app->views->controller == NULL) {
            psp_ui_show_status(&app->process->presentation.ui, "PAGE CONTROLS UNAVAILABLE",
                               180);
            return false;
        }
        if (app->views->controller->focus_kind == CONTROLLER_FOCUS_NONE) {
            /* A provider result authors autofocus on its primary Play
               surface. Optional thumbnail/font relayout can briefly
               leave the controller without a resolved region even
               though that DOM target is still live. Restore that exact
               target and spend this same Cross press on activation;
               only pages without authored autofocus retain the ordinary
               focus-first, activate-second interaction. */
            if (browser_engine_restore_autofocus(engine)) {
                frame->page_dirty = true;
            } else {
                frame->page_dirty =
                    browser_engine_focus_move(engine, true)
                    || frame->page_dirty;
                return false;
            }
        }
        ControllerTextInputInfo text_info = {0};
        bool focused_text =
            browser_engine_text_input_info(engine, &text_info)
            && text_info.editable;
        if (enter_only && (!focused_text || text_info.multiline)) {
            psp_ui_show_status(
                &app->process->presentation.ui,
                "ENTER IS NOT AVAILABLE HERE", 120);
            return false;
        }
        bool submit_after_edit = false;
        if (focused_text && !enter_only) {
            if (frame->pointer_activation
                && !browser_engine_pointer_commit_click(engine)) {
                psp_ui_show_status(
                    &app->process->presentation.ui, "FIELD IS NO LONGER AVAILABLE", 120);
                return false;
            }
            (void) psp_engine_views_refresh(app->views, engine);
            text_info = (ControllerTextInputInfo) {0};
            if (!browser_engine_text_input_info(
                    engine, &text_info)
                || !text_info.editable) {
                return false;
            }
            if (psp_replace_focused_text(
                    engine, engine_frame, &app->process->presentation.ui, &app->process->text_input,
                    false, &submit_after_edit)) {
                frame->page_dirty = true;
                (void) psp_engine_views_refresh(app->views, engine);
            }
            app->interactive->previous_buttons = 0;
            if (!submit_after_edit) return false;
        }
        activations_before = app->views->controller->activations;
        size_t relayouts_before =
            browser_engine_page_incremental_relayouts(engine);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        uint64_t activation_started_us = sceKernelGetSystemTimeWide();
        size_t activation_relayouts =
            browser_engine_page_incremental_relayouts(engine);
#endif
        /* Author activation can synchronously relayout. Keep only the
           last completed framebuffer visible at owner-thread safe
           points, and retain presses for replay after this transaction. */
        bool activation_scope = !psp_navigation_cooperate_active()
            && engine_frame != NULL
            && !app->process->presentation.ui.page_gamepad_capture;
        if (activation_scope)
            psp_runtime_cooperate_begin(
                &app->process->presentation.ui, engine_frame,
                &app->interactive->toolbar_input);
        activated = browser_engine_activate(engine, action);
        uint32_t activation_buttons = 0;
        if (activation_scope
            && psp_runtime_cooperate_end(&activation_buttons))
            app->interactive->previous_buttons =
                psp_ui_buttons(activation_buttons);
        /* An activation that relaid out a page over a live canvas
           (a menu screen switch) is composed later in this
           iteration; running the animation callbacks first would
           only delay it. They run next iteration, as after a focus
           move (never two iterations in a row). */
        if (activated && frame->canvas_live
            && !frame->runtime_deferred
            && browser_engine_page_incremental_relayouts(engine)
                   != relayouts_before)
            frame->defer_runtime = true;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        BrowserValidationCounters activation_counters;
        (void) browser_engine_validation_counters(
            engine, &activation_counters);
        printf("tilefinch-control-activation: ok=%u kind=%d elapsed=%lluus relayouts=%zu\n",
               activated ? 1u : 0u, activated ? (int) action->type : -1,
               (unsigned long long) (sceKernelGetSystemTimeWide() - activation_started_us),
               activation_counters.incremental_relayouts - activation_relayouts);
        if (activated && activation_counters.incremental_relayouts != activation_relayouts) {
            const LayoutPerformance *cost = &activation_counters.layout_performance;
            printf("tilefinch-control-layout: flow=%lluus styles=%lluus spatial=%lluus full=%zu fast=%zu\n",
                   (unsigned long long) cost->flow_us,
                   (unsigned long long) cost->style_resolve_us,
                   (unsigned long long) cost->spatial_index_us,
                   activation_counters.full_relayouts,
                   activation_counters.fast_relayouts);
            printf("tilefinch-control-layout-phases: total=%lluus root=%lluus compact=%lluus focus=%lluus paint=%lluus spatial=%lluus finalize=%lluus\n",
                (unsigned long long) cost->total_us,
                (unsigned long long) cost->root_style_us,
                (unsigned long long) cost->compact_us,
                (unsigned long long) cost->focus_index_us,
                (unsigned long long) cost->paint_order_us,
                (unsigned long long) cost->spatial_index_us,
                (unsigned long long) cost->finalize_us);
            if (cost->flow_phase_transitions != 0)
                printf("tilefinch-control-flow: other=%lluus style=%lluus pseudo=%lluus intrinsic=%lluus margin=%lluus inline=%lluus cooperate=%lluus switches=%u\n",
                    (unsigned long long) cost->flow_phase_us[LAYOUT_FLOW_OTHER],
                    (unsigned long long) cost->flow_phase_us[LAYOUT_FLOW_STYLE],
                    (unsigned long long) cost->flow_phase_us[LAYOUT_FLOW_PSEUDO],
                    (unsigned long long) cost->flow_phase_us[LAYOUT_FLOW_INTRINSIC],
                    (unsigned long long) cost->flow_phase_us[LAYOUT_FLOW_MARGIN],
                    (unsigned long long) cost->flow_phase_us[LAYOUT_FLOW_INLINE],
                    (unsigned long long) cost->flow_phase_us[LAYOUT_FLOW_COOPERATE],
                    (unsigned) cost->flow_phase_transitions);
        }
#endif
        /* The press can land on something that focuses a text field
           rather than on the field itself: a <label> laid over it
           (ChatGPT's composer), or a control whose handler calls
           focus(). A phone opens its keyboard for that focus, so
           open text entry here too; otherwise the first press looked
           like it did nothing and only a second one typed. */
        if (activated && !focused_text && !enter_only
            && action->type == CONTROLLER_ACTION_CONTROL) {
            (void) psp_engine_views_refresh(app->views, engine);
            (void) browser_engine_adopt_script_focus(engine);
            ControllerTextInputInfo focused_after = {0};
            if (browser_engine_text_input_info(
                    engine, &focused_after)
                && focused_after.editable) {
                bool submit_focused = false;
                if (psp_replace_focused_text(
                        engine, engine_frame,
                        &app->process->presentation.ui,
                        &app->process->text_input, false,
                        &submit_focused)) {
                    frame->page_dirty = true;
                    (void) psp_engine_views_refresh(
                        app->views, engine);
                }
                app->interactive->previous_buttons = 0;
                return false;
            }
            /* The handler may focus the field a task or a module
               load later; keep watching while the press's user
               activation lasts (5 s, as in Chromium). */
            app->interactive->script_text_focus_deadline_us =
                (uint64_t) sceKernelGetSystemTimeWide()
                + UINT64_C(5000000);
        }
    }
    activation->activated = activated;
    activation->preview = preview;
    activation->activations_before = activations_before;
    return true;
}

/* Routes a resolved page action. */
__attribute__((noinline))
void psp_app_route_controller_action(
    PspApp *app, PspAppFrameState *frame, const uint16_t *engine_frame,
    ControllerAction *action)
{
    BrowserEngine *engine = app->browser->engine;
    BrowserProfile *profile = app->browser->profile;
    BrowserTabs *tabs = app->browser->tabs;
    bool navigates =
        action->type == CONTROLLER_ACTION_NAVIGATE
        || action->type
             == CONTROLLER_ACTION_FORM_SUBMIT;
    if (action->type == CONTROLLER_ACTION_MEDIA) {
        if (browser_session_captive_portal_active(
                app->browser->session)) {
            psp_ui_show_status(
                &app->process->presentation.ui,
                "MEDIA IS DISABLED DURING WI-FI SIGN-IN", 180);
            return;
        }
        if (action->media_kind == MEDIA_DISCOVERY_WEBM) {
            psp_ui_show_status(
                &app->process->presentation.ui,
                "VIDEO FORMAT COMING SOON", 180);
            return;
        }
        if (action->media_audio_only
            && action->media_kind == MEDIA_DISCOVERY_HLS) {
            psp_ui_show_status(
                &app->process->presentation.ui,
                "AUDIO HLS FORMAT COMING SOON", 180);
            return;
        }
        char entry_buffer[NAVIGATION_URL_LIMIT];
        const char *entry_url = psp_app_current_url(
            engine, entry_buffer, sizeof(entry_buffer));
#ifdef TILEFINCH_PSP_LIVE_NETWORK
        if (!psp_action_network_ready(
                app, "GET", action->url, engine_frame))
            return;
#endif
        bool opened = entry_url != NULL
            && (action->media_audio_only
                ? psp_media_open_page_audio(
                      &app->browser->media, action->url, entry_url,
                      browser_engine_page_generation(engine),
                      action->media_node_handle, action->media_mode,
                      action->media_credentials, true, false)
                : action->media_kind == MEDIA_DISCOVERY_HLS
                ? psp_media_open_page_hls(
                      &app->browser->media, action->url, entry_url,
                      browser_engine_page_generation(engine),
                      action->media_node_handle, action->media_mode,
                      action->media_credentials, true, false)
                : psp_media_open_page_source(
                      &app->browser->media, action->url, entry_url,
                      browser_engine_page_generation(engine),
                      action->media_node_handle, action->media_mode,
                      action->media_credentials, true, false));
        (void) browser_engine_update_media_state(
            engine, action->media_node_handle,
            opened ? SCRIPT_MEDIA_STATE_LOADING
                   : SCRIPT_MEDIA_STATE_ERROR,
            0.0, 0.0);
        if (!opened) {
            psp_ui_show_status(
                &app->process->presentation.ui,
                action->media_audio_only
                    ? "AUDIO SOURCE COULD NOT OPEN"
                    : "VIDEO SOURCE COULD NOT OPEN", 180);
        }
        frame->page_dirty = true;
        return;
    }
    if (navigates && action->prefer_native_media
        && youtube_watch_url_supported(action->url)) {
        if (browser_session_captive_portal_active(
                app->browser->session)) {
            psp_ui_show_status(
                &app->process->presentation.ui,
                "MEDIA IS DISABLED DURING WI-FI SIGN-IN", 180);
            return;
        }
        char entry_buffer[NAVIGATION_URL_LIMIT];
        const char *entry_url = psp_app_current_url(
            engine, entry_buffer, sizeof(entry_buffer));
#ifdef TILEFINCH_PSP_LIVE_NETWORK
        if (!psp_action_network_ready(
                app, "GET", action->url, engine_frame))
            return;
#endif
        /* The result document remains the rollback surface. Its
           optional thumbnails and script requests are no longer
           useful while the player owns the screen, and retiring
           them makes the resolver's first worker descriptor
           available immediately. */
        int youtube_quality = (int)
            browser_profile_youtube_quality(profile);
        YoutubeResolveJob **prepared_resolver =
            entry_url == NULL ? NULL
            : psp_youtube_preresolve_job_for_open(
                  &app->browser->youtube_preresolve,
                  action->url,
                  browser_engine_page_generation(engine),
                  youtube_quality, frame->ui_sample_us);
        bool offered_prepared_resolver =
            prepared_resolver != NULL
            && *prepared_resolver != NULL;
        size_t queue_index; bool queue_all;
        bool playlist = entry_url != NULL
            && youtube_lite_route(entry_url) == YOUTUBE_LITE_ROUTE_PLAYLIST
            && youtube_lite_playlist_selection(action->url, &queue_index, &queue_all);
        bool opened = entry_url != NULL && (playlist
            ? psp_media_open_playlist_route(&app->browser->media, action->url,
                entry_url, browser_engine_page_generation(engine))
            : psp_media_open_provider_route_prepared(
                &app->browser->media, action->url,
                browser_engine_page_generation(engine),
                prepared_resolver));
        if (opened && offered_prepared_resolver
            && prepared_resolver != NULL
            && *prepared_resolver == NULL) {
            psp_youtube_preresolve_note_taken(
                &app->browser->youtube_preresolve);
        }
        if (opened) {
            (void) browser_engine_cancel_network_work(
                engine, "provider video selected");
            /* Reclaim only when the selected decoder cannot fit.
               Even then the loop presents the player before it
               spends one bounded reclaim phase per frame. */
            app->interactive->provider_handoff_reclaim_pump_us =
                0;
            browser_engine_prepare_optional_memory_reclaim(
                engine,
                psp_media_startup_headroom_bytes(
                    &app->browser->media),
                &app->interactive->provider_handoff_reclaim);
            /* Publish one complete native-player loading surface
               before resolver or decoder work may enter a
               cooperative scope. A prepared result can otherwise
               reach the first decoded frame in this same browser
               iteration, making the 565->8888 bridge expand the
               old page/partially cleared footer instead of the
               intended full loading UI. This costs one presented
               frame, never a network round trip; reclaim remains
               independently conditional below. */
            app->interactive->provider_handoff_present_pending =
                true;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            printf("tilefinch-provider-handoff: phase=armed "
                   "pressure=%d free=%zu target=%zu\n",
                   browser_engine_optional_memory_reclaim_pending(
                       &app->interactive
                            ->provider_handoff_reclaim) ? 1 : 0,
                   budget_remaining(app->browser->budget),
                   app->interactive->provider_handoff_reclaim
                       .target_remaining_bytes);
#endif
            psp_ui_set_loading(
                &app->process->presentation.ui, false, 0);
            frame->page_dirty = true;
            return;
        }
        /* A synchronous admission refusal falls through to the
           ordinary watch-page navigation. The visible Details
           action always takes that path directly. */
        action->prefer_native_media = false;
    }
    if (navigates
        && psp_internal_action_url(
               action->url, "address")) {
        char entry_buffer[NAVIGATION_URL_LIMIT];
        const char *entry_url = psp_app_current_url(
            engine, entry_buffer, sizeof(entry_buffer));
        char destination[NAVIGATION_URL_LIMIT] = {0};
        bool accepted = psp_request_omnibox(
            &app->process->text_input, engine_frame, &app->process->presentation.ui, profile,
            entry_url,
            false, true,
            destination, sizeof(destination));
        app->interactive->previous_buttons = 0;
        if (!accepted) return;
        snprintf(
            action->url, sizeof(action->url), "%s",
            destination);
        snprintf(
            action->method, sizeof(action->method),
            "%s", "GET");
        controller_action_clear_body(action);
        action->content_type[0] = '\0';
    }
    /* `tilefinch://home` is the same destination spelled as an
       internal action, which only a page's own link can emit;
       it is not a URL any other navigation site can carry, so
       it stays local rather than widening the shared
       classifier. */
    PspLocalRoute action_route =
        psp_local_route_classify(action->url);
    if (navigates
        && (psp_internal_action_url(action->url, "home")
            || psp_local_route_open(
                   &action_route, PSP_ROUTE_ORIGIN_ACTIVATION)
                == PSP_LOCAL_OPEN_NATIVE_HOME)) {
        psp_text_input_before_navigation(&app->process->text_input);
        psp_show_native_home(app);
        return;
    }
    if (navigates
        && psp_internal_action_url(
               action->url, "retry")) {
        char entry_buffer[NAVIGATION_URL_LIMIT];
        const char *entry_url = psp_app_current_url(
            engine, entry_buffer, sizeof(entry_buffer));
        if (entry_url == NULL
            || entry_url[0] == '\0') {
            psp_ui_show_status(
                &app->process->presentation.ui, "Nothing to retry", 180);
            return;
        }
        snprintf(
            action->url, sizeof(action->url), "%s",
            entry_url);
        snprintf(
            action->method, sizeof(action->method),
            "%s", "GET");
        controller_action_clear_body(action);
        action->content_type[0] = '\0';
    }
    /* The retry action above may have replaced the URL. */
    action_route = psp_local_route_classify(action->url);
    if (navigates
        && psp_local_route_open(
               &action_route, PSP_ROUTE_ORIGIN_ACTIVATION)
            == PSP_LOCAL_OPEN_OFFLINE) {
        BrowserHistoryEntryText source_entry;
        const char *source_entry_url =
            browser_engine_current_entry_text(
                engine, &source_entry, NULL)
            ? source_entry.url : NULL;
        bool youtube_save = strncmp(
                action->url,
                "https://tilefinch.local/offline/youtube?id=",
                strlen("https://tilefinch.local/offline/youtube?id="))
            == 0;
        bool library_root = strcmp(
                action->url,
                "https://tilefinch.local/offline") == 0;
        bool offline_source = source_entry_url != NULL
            && psp_offline_url(source_entry_url);
        bool trusted_source = library_root
            || offline_source
            || (youtube_save && source_entry_url != NULL
                && youtube_watch_url_supported(
                       source_entry_url));
        const char *stopping = trusted_source
            ? psp_offline_store_route_cancel_status(action->url) : NULL;
        bool zip_preview = stopping != NULL;
        if (zip_preview)
            psp_work_cooperate_begin(&app->process->presentation.ui, engine_frame,
                true, true, false, stopping, "zip-preview", NULL, NULL);
        PspOfflineRouteResult offline_result =
            trusted_source
            ? psp_offline_store_handle_url(
                  &app->browser->offline_store, engine, profile,
                  action->url,
                  source_entry_url == NULL
                      ? NULL : source_entry.title,
                  true)
            : PSP_OFFLINE_ROUTE_ERROR;
        if (zip_preview)
            psp_navigation_cooperate_end_adopting_buttons(
                "zip-preview", &app->interactive->previous_buttons);
        if (!trusted_source)
            psp_ui_show_status(
                &app->process->presentation.ui, "OFFLINE SAVE LINK REFUSED", 180);
        else
            psp_ui_show_status(
                &app->process->presentation.ui,
                psp_offline_store_status(&app->browser->offline_store),
                240);
        if (offline_result != PSP_OFFLINE_ROUTE_NONE) {
            app->process->presentation.ui.reader_mode = false;
            app->process->presentation.ui.basic_mode = false;
            (void) psp_engine_views_refresh(app->views, engine);
            psp_sync_ui(&app->process->presentation.ui, engine, profile);
            if (offline_result == PSP_OFFLINE_ROUTE_ZIP_PREVIEW)
                psp_ui_show_zip_install(&app->process->presentation.ui,
                    zip_install_preview(app->browser->offline_store.zip_install));
            if (tabs != NULL)
                (void) browser_tabs_capture_active(
                    tabs, app->views->navigation);
            psp_tabs_sync_ui(&app->process->presentation.ui, tabs, &app->process->presentation.tab_view);
            frame->page_dirty = offline_result
                != PSP_OFFLINE_ROUTE_ERROR || frame->page_dirty;
            return;
        }
    }
    if (navigates && action->prefer_download) {
        psp_app_offer_file(app, frame, action->url,
                           "DOWNLOAD PREVIEW UNAVAILABLE");
        return;
    }
#ifdef TILEFINCH_PSP_LIVE_NETWORK
    if (navigates && !psp_action_network_ready(
            app, action->method, action->url, engine_frame))
        return;
#endif
    if (navigates) {
        if (!browser_engine_set_javascript_enabled(
                engine,
                browser_profile_javascript_allowed_for_url(
                    profile, action->url))) {
            psp_ui_show_status(
                &app->process->presentation.ui,
                "JAVASCRIPT POLICY UNAVAILABLE", 240);
            return;
        }
        bool reader_navigation =
            psp_reader_navigation_prepare(
                engine, &app->process->presentation.ui, profile,
                &app->interactive->reader_navigation,
                action->url);
        if (!reader_navigation) {
            psp_leave_reader_for_navigation(
                engine, &app->process->presentation.ui, profile,
                action->url);
        }
        psp_ui_set_loading(&app->process->presentation.ui, true, -1);
        psp_ui_show_status(
            &app->process->presentation.ui,
            reader_navigation
                ? "LOADING READER PAGE"
                : "LOADING - CIRCLE CANCELS",
            reader_navigation ? 300 : 120);
        psp_navigation_cooperate_begin(
            &app->process->presentation.ui, engine_frame, engine);
        psp_text_input_before_navigation(&app->process->text_input);
    }
    bool action_ok = action->type
            == CONTROLLER_ACTION_NONE
        || (navigates
            ? psp_retry_navigation_action_after_reclaim(
                  engine, action, PSP_NAVIGATION_DOCUMENT_BYTES, PSP_NAVIGATION_TIMEOUT_MS)
            : browser_engine_execute_action(
                  engine, action, PSP_NAVIGATION_DOCUMENT_BYTES, PSP_NAVIGATION_TIMEOUT_MS));
    if (action_ok) {
        if (navigates) {
            app->interactive->navigation_job_started_us =
                (uint64_t) sceKernelGetSystemTimeWide();
        } else {
            (void) psp_engine_views_refresh(app->views, engine);
            frame->page_dirty = true;
        }
    } else {
        if (navigates) {
            (void) psp_engine_views_refresh(app->views, engine);
            char incumbent_buffer[NAVIGATION_URL_LIMIT];
            const char *incumbent_url = psp_app_current_url(
                engine, incumbent_buffer, sizeof(incumbent_buffer));
            psp_reader_navigation_finish(
                engine, &app->process->presentation.ui, profile,
                &app->interactive->reader_navigation,
                incumbent_url,
                false);
            (void) psp_engine_views_refresh(app->views, engine);
            (void) psp_write_failure_report(
                "navigation-action-start",
                browser_engine_last_error(engine),
                action->url, 0, 0);
            psp_report_job_failure(
                "navigation-action-start",
                "interactive-action-start-failed",
                -1, 0,
                browser_engine_last_error(engine));
            psp_navigation_cooperate_end(
                "interactive-action-start-failed");
            psp_ui_set_loading(
                &app->process->presentation.ui, false, 0);
        }
        psp_ui_show_status(
            &app->process->presentation.ui, browser_engine_last_error(engine), 300);
    }
}

__attribute__((noinline))
void psp_app_dispatch_activation(
    PspApp *app, PspAppFrameState *frame, bool enter_only,
    const uint16_t *engine_frame)
{
    BrowserEngine *engine = app->browser->engine;
    PspPageActivation activation;
    if (!psp_app_activate_page(
            app, frame, enter_only, engine_frame, &activation)) return;
    if (activation.activated) {
        psp_app_route_controller_action(
            app, frame, engine_frame, &activation.action);
    } else if (!activation.preview
               && app->views->controller->activations
                      == activation.activations_before) {
        /* Resolution can fail only after the retained target truly
           leaves the current layout. Do not silently consume Cross:
           move to a live target and paint its focus in this frame.
           A failure after a handler began is deliberately excluded
           so no page-side action is followed by an unrelated move. */
        bool recovered = browser_engine_restore_autofocus(engine)
            || browser_engine_focus_move(engine, true);
        if (recovered) {
            frame->page_dirty = true;
        } else {
            psp_ui_show_status(
                &app->process->presentation.ui,
                "PAGE ITEM IS NO LONGER AVAILABLE", 120);
        }
    }
}
