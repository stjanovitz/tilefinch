/* Shared by the PSP media session and its host journey. Only the firmware
   open/close boundary is substituted in the replay, not queue navigation. */
static void psp_media_playlist_projection(PspMediaSession *media)
{
    PspUiMediaPresentation *p = &media->ui_presentation;
    YoutubePlaylistEntry entries[YOUTUBE_PLAYLIST_VIEW_LIMIT];
    size_t count = youtube_playlist_view(media->playlist, entries, YOUTUBE_PLAYLIST_VIEW_LIMIT);
    if (count > YOUTUBE_PLAYLIST_VIEW_LIMIT) count = YOUTUBE_PLAYLIST_VIEW_LIMIT;
    /* Titles are prepared in the view, then the queue is stored in one
       fenced step: a supervised player copy may be drawing it (AU-05). */
    for (size_t i = 0; i < count; i++)
        psp_ui_media_prepare_row_text(&media->ui, media->budget, entries[i].title,
                                      sizeof(entries[i].title));
    bool held = psp_ui_media_fence_acquire(&media->ui);
    p->queue_active = media->playlist != NULL;
    p->queue_count = (uint8_t) count;
    p->queue_position = (uint32_t) youtube_playlist_position(media->playlist);
    p->queue_next = youtube_playlist_has_next(media->playlist);
    p->queue_loading = youtube_playlist_loading(media->playlist);
    p->queue_failed = youtube_playlist_failed(media->playlist);
    p->queue_retry = youtube_playlist_can_retry(media->playlist);
    p->queue_terminal = youtube_playlist_selecting(media->playlist)
        && p->queue_failed && !p->queue_retry;
    if (youtube_playlist_selecting(media->playlist) && p->queue_failed)
        media->ui.retry_unavailable = p->queue_terminal;
    for (size_t i = 0; i < count; i++)
        memcpy(p->queue_titles[i], entries[i].title, sizeof(p->queue_titles[i]));
    snprintf(p->queue_error, sizeof(p->queue_error), "%s", youtube_playlist_error(media->playlist));
    psp_ui_media_fence_release(&media->ui, held);
}

bool psp_media_open_playlist_route(PspMediaSession *media, const char *url,
                                  const char *playlist_url, uint64_t generation)
{
    size_t index; bool all;
    char expected[YOUTUBE_VIDEO_ID_CAPACITY];
    if (!media || !generation || media->system_suspended
        || !youtube_lite_playlist_selection(url, &index, &all)
        || !youtube_watch_url_video_id(url, expected)) return false;
    YoutubePlaylistQueue *queue = youtube_playlist_begin(media->budget, media->session,
        playlist_url, index, all, all ? NULL : expected);
    if (!queue) return false;
    youtube_playlist_set_initial_start(queue,
        all ? 0 : youtube_watch_url_start_time_us(url));
    media->provider_direct_route = true; /* A queue never retains an unrelated pipeline. */
    psp_media_close(media);
    media->playlist = queue;
    media->provider_direct_route = true;
    media->generation = generation;
    media->playlist_end_handled = false;
    media->ui_presentation.automatic_next = false;
    psp_ui_media_set_resolving(&media->ui, tilefinch_ui_text("Loading playlist..."));
    psp_media_playlist_projection(media);
    return true;
}

static bool psp_media_playlist_tick(PspMediaSession *media)
{
    if (!media->playlist || !media->ui.visible || media->system_suspended) return false;
    bool changed = false;
    if (psp_media_playlist_end_edge(media->ui.ended,
                                   &media->playlist_end_handled)) {
        if (media->ui_presentation.automatic_next && youtube_playlist_has_next(media->playlist))
            changed = youtube_playlist_step(media->playlist, 1);
        else if (!youtube_playlist_has_next(media->playlist)) {
            /* At the actual end, keep recovery and return discoverable rather
               than leaving a stopped picture with no next video. */
            media->ui_presentation.track_menu_open = true;
            media->ui_presentation.track_menu_tab = PSP_UI_MEDIA_TAB_QUEUE;
            media->ui_presentation.track_menu_selection = 0;
            psp_ui_media_show_controls(&media->ui);
            changed = true;
        }
    }
    FetchPumpQuota quota = {.maximum_time_us = 1500, .maximum_body_callbacks = 2, .maximum_body_bytes = 16384};
    /* Metadata prefetch is optional during active decode. Start only near
       the tail of the retained window; one bounded provider pump per turn. */
    if (youtube_playlist_should_pump(media->playlist,
        media->have_frame && !media->ui.resolving
            && !media->ui.buffering && !media->ui.ended))
        changed = youtube_playlist_pump(media->playlist, &quota) || changed;
    char url[NAVIGATION_URL_LIMIT];
    if (youtube_playlist_take_video(media->playlist, url, sizeof(url))) {
        YoutubePlaylistQueue *queue = media->playlist;
        bool automatic = media->ui_presentation.automatic_next;
        media->playlist = NULL;
        psp_media_close(media); /* Also handles consecutive duplicate IDs. */
        media->playlist = queue;
        if (!psp_media_open_provider_route(media, url, media->generation)) {
            /* The close hid the player. A refused open must not leave a
               hidden queue owning its page job, which nothing would pump
               or free; show why the queue stopped instead. */
            youtube_playlist_destroy(media->playlist);
            media->playlist = NULL;
            psp_media_raise_error(media, "PLAYLIST VIDEO COULD NOT OPEN", NULL);
            /* The close dropped the route's source; there is nothing to
               retry, so the panel offers only Back. */
            media->ui.retry_unavailable = true;
            psp_media_playlist_projection(media);
            return true;
        }
        media->playlist_end_handled = false;
        media->ui_presentation.automatic_next = automatic;
        changed = true;
    }
    if (changed) psp_media_playlist_projection(media);
    if (youtube_playlist_selecting(media->playlist) && youtube_playlist_failed(media->playlist) && !media->ui.failed) {
        psp_media_raise_error(media, "PLAYLIST COULD NOT LOAD",
                             youtube_playlist_error(media->playlist));
        psp_media_playlist_projection(media);
        changed = true;
    } else if (youtube_playlist_selecting(media->playlist) && !media->ui.resolving && !media->ui.failed) {
        if (media->playback) media_playback_set_playing(media->playback, false);
        psp_ui_media_set_resolving(&media->ui, tilefinch_ui_text("Loading playlist..."));
        changed = true;
    }
    return changed;
}

static __attribute__((noinline)) bool psp_media_playlist_intent(PspMediaSession *media, PspUiMediaIntent intent)
{
    if (media->playlist) {
        if (intent.action == PSP_UI_MEDIA_ACTION_RETRY && youtube_playlist_selecting(media->playlist)
            && youtube_playlist_failed(media->playlist)) {
            youtube_playlist_retry(media->playlist);
            if (!youtube_playlist_failed(media->playlist))
                psp_ui_media_set_resolving(&media->ui, tilefinch_ui_text("Loading playlist..."));
            psp_media_playlist_projection(media);
            return true;
        }
        bool queue_action = true;
        bool changing_video = false;
        switch (intent.action) {
            case PSP_UI_MEDIA_ACTION_NEXT: changing_video = youtube_playlist_step(media->playlist, 1); break;
            case PSP_UI_MEDIA_ACTION_PREVIOUS: changing_video = youtube_playlist_step(media->playlist, -1); break;
            case PSP_UI_MEDIA_ACTION_QUEUE_SELECT:
                if (intent.track_index < media->ui_presentation.queue_count)
                    changing_video = youtube_playlist_select(media->playlist,
                        youtube_playlist_position(media->playlist) + intent.track_index);
                break;
            case PSP_UI_MEDIA_ACTION_AUTOMATIC_NEXT:
                media->ui_presentation.automatic_next = intent.track_index != 0; break;
            case PSP_UI_MEDIA_ACTION_QUEUE_RETRY:
                youtube_playlist_retry(media->playlist);
                if (youtube_playlist_selecting(media->playlist)
                    && !youtube_playlist_failed(media->playlist))
                    psp_ui_media_set_resolving(&media->ui, tilefinch_ui_text("Loading playlist..."));
                break;
            default: queue_action = false; break;
        }
        if (queue_action) {
            if (changing_video) {
                if (media->playback) media_playback_set_playing(media->playback, false);
                psp_ui_media_set_resolving(&media->ui, tilefinch_ui_text("Loading playlist..."));
            }
            psp_media_playlist_projection(media); return true;
        }
    }
    return false;
}
