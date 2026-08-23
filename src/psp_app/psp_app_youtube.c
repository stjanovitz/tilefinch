/* Bounded YouTube result pre-resolution for the PSP frontend.
 *
 * The resolver is deliberately separate from the media session until the
 * user activates Play. It may obtain signed media descriptors from YouTube,
 * but it never opens those CDN URLs speculatively. One generation-bearing
 * slot means rapid focus changes can only cancel or replace one operation.
 */
#include "psp_app_internal.h"

static void psp_youtube_preresolve_increment(unsigned *counter)
{
    if (counter != NULL && *counter != UINT_MAX) (*counter)++;
}

static void psp_youtube_preresolve_clear_facts(
    PspYoutubePreresolve *preresolve)
{
    if (preresolve == NULL) return;
    preresolve->video_id[0] = '\0';
    preresolve->generation = 0;
    preresolve->focus_since_us = 0;
    preresolve->started_us = 0;
    preresolve->ready_us = 0;
    preresolve->maximum_height = 0;
    preresolve->state = PSP_YOUTUBE_PRERESOLVE_IDLE;
}

void psp_youtube_preresolve_reset(
    PspYoutubePreresolve *preresolve, const char *reason)
{
    if (preresolve == NULL) return;
    if (preresolve->job != NULL) {
        YoutubeResolveJobMetrics metrics = {0};
        (void) youtube_resolve_job_metrics(preresolve->job, &metrics);
        youtube_resolve_job_cancel(
            preresolve->job,
            reason == NULL ? "YouTube pre-resolution cancelled" : reason);
        youtube_resolve_job_destroy(preresolve->job);
        preresolve->job = NULL;
        if (preresolve->state != PSP_YOUTUBE_PRERESOLVE_READY)
            psp_youtube_preresolve_increment(&preresolve->cancellations);
        printf(
            "tilefinch-youtube-preresolve: event=retired reason=\"%.48s\" "
            "phase=%s attempts=%u chunks=%zu\n",
            reason == NULL ? "reset" : reason,
            metrics.phase == NULL ? "none" : metrics.phase,
            metrics.attempts, metrics.request_chunks);
    }
    psp_youtube_preresolve_clear_facts(preresolve);
}

static bool psp_youtube_preresolve_same_candidate(
    const PspYoutubePreresolve *preresolve,
    const char video_id[YOUTUBE_VIDEO_ID_CAPACITY],
    uint64_t generation, int maximum_height)
{
    return preresolve != NULL && video_id != NULL
        && preresolve->generation == generation
        && preresolve->maximum_height == maximum_height
        && strcmp(preresolve->video_id, video_id) == 0;
}

static bool psp_youtube_video_id_valid(const char *video_id)
{
    if (video_id == NULL) return false;
    size_t length = strnlen(video_id, YOUTUBE_VIDEO_ID_CAPACITY);
    if (length == 0 || length >= YOUTUBE_VIDEO_ID_CAPACITY) return false;
    for (size_t at = 0; at < length; at++) {
        unsigned char byte = (unsigned char) video_id[at];
        if (!((byte >= 'a' && byte <= 'z')
              || (byte >= 'A' && byte <= 'Z')
              || (byte >= '0' && byte <= '9')
              || byte == '-' || byte == '_')) return false;
    }
    return true;
}

void psp_youtube_preresolve_tick(
    PspYoutubePreresolve *preresolve, Budget *budget,
    BrowserSession *session, const char *focused_video_id,
    uint64_t generation, int maximum_height, uint64_t now_us,
    bool thumbnail_settled, bool eligible, bool transport_capacity,
    bool pump_allowed)
{
    if (preresolve == NULL) return;
    bool candidate = generation != 0 && maximum_height > 0
        && psp_youtube_video_id_valid(focused_video_id);
    if (!candidate) {
        if (preresolve->video_id[0] != '\0')
            psp_youtube_preresolve_reset(preresolve, "focus unavailable");
        return;
    }
    if (!psp_youtube_preresolve_same_candidate(
            preresolve, focused_video_id, generation, maximum_height)) {
        psp_youtube_preresolve_reset(preresolve, "focus changed");
        snprintf(
            preresolve->video_id, sizeof(preresolve->video_id), "%s",
            focused_video_id);
        preresolve->generation = generation;
        preresolve->maximum_height = maximum_height;
        preresolve->focus_since_us = now_us;
        preresolve->state = thumbnail_settled
            ? PSP_YOUTUBE_PRERESOLVE_DWELL
            : PSP_YOUTUBE_PRERESOLVE_THUMBNAIL;
        return;
    }
    if (!eligible) {
        /* A transferred job deliberately keeps the consumed marker until
           focus leaves the row. Every earlier interruption invalidates both
           an in-flight resolver and dwell time accumulated behind a modal. */
        if (preresolve->state != PSP_YOUTUBE_PRERESOLVE_CONSUMED)
            psp_youtube_preresolve_reset(
                preresolve, "speculation no longer eligible");
        return;
    }
    if (preresolve->state == PSP_YOUTUBE_PRERESOLVE_CONSUMED) return;
    if (preresolve->state == PSP_YOUTUBE_PRERESOLVE_READY) {
        if (now_us >= preresolve->ready_us
            && now_us - preresolve->ready_us
                   < PSP_YOUTUBE_PRERESOLVE_RETENTION_US) {
            return;
        }
        psp_youtube_preresolve_reset(preresolve, "cached result aged out");
        snprintf(
            preresolve->video_id, sizeof(preresolve->video_id), "%s",
            focused_video_id);
        preresolve->generation = generation;
        preresolve->maximum_height = maximum_height;
        preresolve->focus_since_us = now_us;
        preresolve->state = thumbnail_settled
            ? PSP_YOUTUBE_PRERESOLVE_DWELL
            : PSP_YOUTUBE_PRERESOLVE_THUMBNAIL;
        return;
    }
    if (preresolve->state == PSP_YOUTUBE_PRERESOLVE_FAILED) return;
    if (preresolve->state == PSP_YOUTUBE_PRERESOLVE_THUMBNAIL) {
        if (!thumbnail_settled) return;
        preresolve->state = PSP_YOUTUBE_PRERESOLVE_DWELL;
    }
    if (preresolve->job == NULL) {
        if (preresolve->state != PSP_YOUTUBE_PRERESOLVE_DWELL) return;
        if (now_us < preresolve->focus_since_us
            || now_us - preresolve->focus_since_us
                   < PSP_YOUTUBE_PRERESOLVE_DWELL_US
            || !transport_capacity) {
            return;
        }
        if (budget_pressure_required(
                budget, PSP_YOUTUBE_PRERESOLVE_WORKING_BYTES,
                PSP_YOUTUBE_PRERESOLVE_RESERVE_BYTES)) {
            psp_youtube_preresolve_increment(
                &preresolve->memory_deferrals);
            return;
        }
        char watch_url[96] = {0};
        int watch_length = snprintf(
            watch_url, sizeof(watch_url),
            "https://www.youtube.com/watch?v=%s", preresolve->video_id);
        if (watch_length < 0 || (size_t) watch_length >= sizeof(watch_url)) {
            preresolve->state = PSP_YOUTUBE_PRERESOLVE_FAILED;
            return;
        }
        const YoutubeResolveJobLimits limits = {
            .watch_response_bytes = PSP_YOUTUBE_PRERESOLVE_WATCH_BYTES,
            .player_response_bytes = PSP_YOUTUBE_PRERESOLVE_PLAYER_BYTES
        };
        preresolve->job = youtube_resolve_job_begin_bounded(
            budget, session, watch_url, maximum_height, 30000,
            &limits, NULL, NULL);
        if (preresolve->job == NULL) {
            preresolve->state = PSP_YOUTUBE_PRERESOLVE_FAILED;
            printf(
                "tilefinch-youtube-preresolve: event=start-refused "
                "video=%s\n", preresolve->video_id);
            return;
        }
        preresolve->started_us = now_us;
        preresolve->state = PSP_YOUTUBE_PRERESOLVE_RESOLVING;
        psp_youtube_preresolve_increment(&preresolve->starts);
        printf(
            "tilefinch-youtube-preresolve: event=started video=%s "
            "quality=%d generation=%llu\n",
            preresolve->video_id, maximum_height,
            (unsigned long long) generation);
    }
    if (!pump_allowed) return;
    YoutubeResolveJobStatus status =
        youtube_resolve_job_pump(preresolve->job);
    if (status == YOUTUBE_RESOLVE_JOB_PENDING) return;
    YoutubeResolveJobMetrics metrics = {0};
    (void) youtube_resolve_job_metrics(preresolve->job, &metrics);
    if (status == YOUTUBE_RESOLVE_JOB_COMPLETE) {
        preresolve->state = PSP_YOUTUBE_PRERESOLVE_READY;
        preresolve->ready_us = now_us;
        psp_youtube_preresolve_increment(&preresolve->completions);
        printf(
            "tilefinch-youtube-preresolve: event=ready video=%s "
            "elapsed=%lluus attempts=%u chunks=%zu\n",
            preresolve->video_id,
            (unsigned long long) (now_us - preresolve->started_us),
            metrics.attempts, metrics.request_chunks);
        return;
    }
    printf(
        "tilefinch-youtube-preresolve: event=failed video=%s "
        "phase=%s detail=\"%.120s\"\n",
        preresolve->video_id,
        metrics.phase == NULL ? "none" : metrics.phase,
        youtube_resolve_job_error(preresolve->job));
    youtube_resolve_job_destroy(preresolve->job);
    preresolve->job = NULL;
    preresolve->state = PSP_YOUTUBE_PRERESOLVE_FAILED;
}

YoutubeResolveJob **psp_youtube_preresolve_job_for_open(
    PspYoutubePreresolve *preresolve, const char *url,
    uint64_t generation, int maximum_height, uint64_t now_us)
{
    char video_id[YOUTUBE_VIDEO_ID_CAPACITY] = {0};
    if (preresolve == NULL || preresolve->job == NULL
        || preresolve->state == PSP_YOUTUBE_PRERESOLVE_FAILED
        || !youtube_watch_url_video_id(url, video_id)
        || !psp_youtube_preresolve_same_candidate(
               preresolve, video_id, generation, maximum_height)) {
        return NULL;
    }
    if (preresolve->state != PSP_YOUTUBE_PRERESOLVE_READY
        && preresolve->started_us != 0
        && (now_us < preresolve->started_us
            || now_us - preresolve->started_us
                   >= PSP_YOUTUBE_PRERESOLVE_PENDING_ADOPT_US)) {
        psp_youtube_preresolve_reset(
            preresolve, "pending resolver too old for activation");
        return NULL;
    }
    return &preresolve->job;
}

void psp_youtube_preresolve_note_taken(PspYoutubePreresolve *preresolve)
{
    if (preresolve == NULL || preresolve->job != NULL) return;
    printf(
        "tilefinch-youtube-preresolve: event=adopted video=%s ready=%d\n",
        preresolve->video_id,
        preresolve->state == PSP_YOUTUBE_PRERESOLVE_READY ? 1 : 0);
    preresolve->ready_us = 0;
    preresolve->started_us = 0;
    preresolve->state = PSP_YOUTUBE_PRERESOLVE_CONSUMED;
}

/* Keep the URL copy and parse out of the per-frame wrapper's stack. This runs
   only when the committed navigation or semantic focus identity changes. */
__attribute__((noinline))
static bool psp_youtube_focused_video_id(
    const BrowserEngine *engine,
    char video_id[YOUTUBE_VIDEO_ID_CAPACITY])
{
    char focused_url[256] = {0};
    video_id[0] = '\0';
    return browser_engine_focused_provider_media_url(
               engine, focused_url, sizeof(focused_url))
        && youtube_watch_url_video_id(focused_url, video_id);
}

/* The interactive loop calls this once per frame. Keep it out of line: the
   candidate observation and transport snapshot are cold unless focus rests
   on a Play card, and neither belongs in the loop's Allegrex frame. */
__attribute__((noinline))
void psp_app_youtube_preresolve_tick(
    PspApp *app, const PspAppFrameState *frame, const PspUiIntent *intent,
    bool render_job_pending, bool site_data_restore_work,
    bool offline_download_active, bool input_active)
{
    if (app == NULL || app->browser == NULL || app->views == NULL
        || app->process == NULL || frame == NULL || intent == NULL) return;
    PspYoutubePreresolve *preresolve =
        &app->browser->youtube_preresolve;
    const BrowserController *controller = app->views->controller;
    uint64_t generation = app->views->navigation->generation;
    size_t focus_moves = controller == NULL ? 0 : controller->focus_moves;
    size_t focus_index = controller == NULL ? 0 : controller->focus_index;
    ControllerFocusKind focus_kind = controller == NULL
        ? CONTROLLER_FOCUS_NONE : controller->focus_kind;
    bool focus_changed = !preresolve->observation_valid
        || preresolve->observed_generation != generation
        || preresolve->observed_focus_moves != focus_moves
        /* A thumbnail relayout can preserve the semantic target while
           rebuilding region indices. Sample the actual controller identity
           too, so a repaired or pointer-selected focus cannot leave the
           resolver associated with the preceding row. */
        || preresolve->observed_focus_index != focus_index
        || preresolve->observed_focus_kind != focus_kind;
    if (focus_changed) {
        preresolve->observation_valid = true;
        preresolve->observed_generation = generation;
        preresolve->observed_focus_moves = focus_moves;
        preresolve->observed_focus_index = focus_index;
        preresolve->observed_focus_kind = focus_kind;
        preresolve->observed_video_id[0] = '\0';
        (void) psp_youtube_focused_video_id(
            app->browser->engine, preresolve->observed_video_id);
    }
    bool focused_provider_result =
        preresolve->observed_video_id[0] != '\0';
    bool thumbnail_settled = !focused_provider_result;
    if (focused_provider_result
        && (focus_changed
            || preresolve->state == PSP_YOUTUBE_PRERESOLVE_IDLE
            || preresolve->state == PSP_YOUTUBE_PRERESOLVE_THUMBNAIL)) {
        thumbnail_settled =
            browser_engine_prepare_focused_provider_media_thumbnail(
                app->browser->engine);
    } else if (focused_provider_result) {
        thumbnail_settled = true;
    }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    if (focus_changed) {
        printf(
            "tilefinch-youtube-preresolve: event=focus-sampled "
            "generation=%llu moves=%zu kind=%d index=%zu video=%s "
            "thumbnail=%s\n",
            (unsigned long long) generation, focus_moves,
            controller == NULL ? -1 : (int) controller->focus_kind,
            controller == NULL ? 0u : controller->focus_index,
            preresolve->observed_video_id[0] == '\0'
                ? "none" : preresolve->observed_video_id,
            thumbnail_settled
                ? "settled" : "pending");
    }
#endif
    bool pump_allowed = intent->action == PSP_UI_ACTION_NONE
        && intent->pointer_phase == PSP_UI_POINTER_NONE
        && intent->scroll_delta == 0
        && !input_active
        && !frame->page_dirty && !render_job_pending
        && !site_data_restore_work;
    const NavigationEntry *current =
        navigation_current(app->views->navigation);
    bool eligible = focused_provider_result
        && app->process->presentation.ui.screen == PSP_UI_SCREEN_PAGE
        && current != NULL
        && !browser_engine_navigation_pending(app->browser->engine)
        && !app->browser->media.ui.visible
        && !psp_media_open_work_pending(&app->browser->media)
        && !psp_media_decode_work_pending(&app->browser->media)
        && app->browser->media.playback == NULL
        && !offline_download_active
        && !psp_navigation_cooperate_active();
    bool transport_capacity = false;
    bool admission_due = eligible
        && thumbnail_settled
        && preresolve->state == PSP_YOUTUBE_PRERESOLVE_DWELL
        && preresolve->job == NULL
        && preresolve->focus_since_us != 0
        && frame->ui_sample_us
               >= preresolve->focus_since_us
        && frame->ui_sample_us
               - preresolve->focus_since_us
                   >= PSP_YOUTUBE_PRERESOLVE_DWELL_US;
    if (admission_due) {
        FetchBackgroundTransportMetrics transport = {0};
        /* Do not let a sequence of optional thumbnail requests starve a
           settled Play target. The resolver uses the media admission class,
           so it can safely occupy a free reserved slot without cancelling
           or overtaking a request already in flight. Its browser-thread
           response work remains behind pump_allowed below. */
        transport_capacity =
            fetch_background_transport_metrics(&transport)
            && transport.occupied_slots < FETCH_BACKGROUND_REQUEST_LIMIT;
    }
    psp_youtube_preresolve_tick(
        preresolve, app->browser->budget,
        app->browser->session,
        focused_provider_result ? preresolve->observed_video_id : NULL,
        generation,
        (int) browser_profile_youtube_quality(app->browser->profile),
        frame->ui_sample_us, thumbnail_settled, eligible,
        transport_capacity, pump_allowed);
}
