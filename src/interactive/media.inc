typedef struct {
    HostMediaPlayer *player;
    Budget *budget;
    PspUiMediaState ui;
    uint64_t generation;
    bool route_enabled;
    bool native_player;
    bool pace_real_time;
    bool pending_open;
    bool open_failed;
    bool suspended_for_internal_view;
    bool seek_preview_started;
    bool seek_preview_was_playing;
    int64_t dom_node_handle;
    uint16_t *seek_preview_pixels;
    size_t last_decoded_frames;
    unsigned no_frame_ms;
    char source[NAVIGATION_URL_LIMIT];
} InteractiveMediaSession;

#define INTERACTIVE_MEDIA_PREVIEW_WIDTH 128
#define INTERACTIVE_MEDIA_PREVIEW_HEIGHT 72

static void interactive_media_init(InteractiveMediaSession *media,
                                   bool route_enabled,
                                   bool pace_real_time,
                                   const FontFace *title_font)
{
    if (media == NULL) return;
    memset(media, 0, sizeof(*media));
    psp_ui_media_init(&media->ui);
    psp_ui_media_set_title_font(&media->ui, title_font);
    media->route_enabled = route_enabled;
    media->pace_real_time = pace_real_time;
}

static void interactive_media_print_status(const InteractiveMediaSession *media)
{
#ifdef TILEFINCH_HAVE_HOST_MEDIA
    HostMediaStats stats;
    if (media == NULL || media->player == NULL
        || !host_media_get_stats(media->player, &stats)) {
        puts("media status=unavailable");
        return;
    }
    printf("media status=%s source-kind=%s itag=%d resolver-bytes=%zu "
           "codec=%s audio=%s source=%dx%d output=%dx%d "
           "fps=%u/%u time-us=%llu duration-us=%llu frames=%zu "
           "dropped=%zu packets=%zu audio-streams=%zu audio-packets=%zu "
           "audio-samples=%llu/%llu audio-dropped=%llu audio-format=%u/%u "
           "audio-output=%s audio-time-us=%llu av-skew-us=%lld "
           "working-set=%zu\n",
           stats.ended ? "ended" : (stats.playing ? "playing" : "paused"),
           stats.source_kind, stats.resolved_itag, stats.resolver_bytes,
           stats.video_codec, stats.audio_codec,
           stats.source_width, stats.source_height,
           stats.output_width, stats.output_height,
           stats.frame_rate_numerator, stats.frame_rate_denominator,
           (unsigned long long) stats.current_time_us,
           (unsigned long long) stats.duration_us,
           stats.decoded_frames, stats.dropped_frames,
           stats.video_packets, stats.audio_streams, stats.audio_packets,
           (unsigned long long) stats.presented_audio_samples,
           (unsigned long long) stats.decoded_audio_samples,
           (unsigned long long) stats.dropped_audio_samples,
           stats.audio_sample_rate, stats.audio_channels,
           stats.audio_output_active ? "native" : "decode-only",
           (unsigned long long) stats.audio_time_us,
           (long long) stats.audio_video_skew_us,
           stats.working_set_bytes);
#else
    (void) media;
    puts("media status=disabled");
#endif
}

static bool interactive_media_sync_ui(InteractiveMediaSession *media)
{
    if (media == NULL || media->player == NULL) return true;
#ifdef TILEFINCH_HAVE_HOST_MEDIA
    HostMediaStats stats;
    if (!host_media_get_stats(media->player, &stats)) return false;
    psp_ui_media_set(&media->ui, media->ui.visible,
                     stats.playing, stats.ended,
                     stats.current_time_us, stats.duration_us,
                     stats.title);
#endif
    return true;
}

static bool interactive_media_open(InteractiveMediaSession *media,
                                   Budget *budget,
                                   NavigationSession *navigation,
                                   const char *source,
                                   bool native_player,
                                   char *error, size_t error_size)
{
    if (media == NULL || budget == NULL || navigation == NULL
        || source == NULL || source[0] == '\0') return false;
#ifdef TILEFINCH_HAVE_HOST_MEDIA
    HostMediaPlayer *player = host_media_create(
        budget, navigation->browser_session, source, 640, 360,
        media->pace_real_time, error, error_size);
    if (player == NULL
        || !(native_player
              ? host_media_attach_native_player(
                    player, navigation, error, error_size)
              : host_media_attach_first_video(
                    player, navigation, error, error_size))) {
        host_media_destroy(player);
        return false;
    }
    host_media_set_playing(player, false);
    media->player = player;
    media->budget = budget;
    media->native_player = native_player;
    media->open_failed = false;
    media->generation = navigation->generation;
    snprintf(media->source, sizeof(media->source), "%s", source);
    media->ui.visible = true;
    psp_ui_media_show_controls(&media->ui);
    return interactive_media_sync_ui(media);
#else
    (void) native_player;
    (void) error;
    (void) error_size;
    return false;
#endif
}

static void interactive_media_destroy(InteractiveMediaSession *media)
{
    if (media == NULL) return;
    const FontFace *title_font = media->ui.title_font;
#ifdef TILEFINCH_HAVE_HOST_MEDIA
    host_media_destroy(media->player);
#endif
    media->player = NULL;
    if (media->budget != NULL && media->seek_preview_pixels != NULL)
        budget_free(media->budget, media->seek_preview_pixels);
    media->seek_preview_pixels = NULL;
    media->seek_preview_started = false;
    media->seek_preview_was_playing = false;
    media->source[0] = '\0';
    media->pending_open = false;
    media->open_failed = false;
    media->dom_node_handle = 0;
    media->generation = 0;
    media->native_player = false;
    psp_ui_media_init(&media->ui);
    psp_ui_media_set_title_font(&media->ui, title_font);
}

static bool interactive_media_sync_route(InteractiveMediaSession *media,
                                         Budget *budget,
                                         NavigationSession *navigation)
{
    if (media == NULL || !media->route_enabled) return true;
    media->budget = budget;
    const NavigationEntry *entry = navigation_current(navigation);
    const char *url = entry == NULL ? NULL : entry->url;
    bool supported = youtube_watch_url_supported(url);
    if (!supported) {
        char current_id[YOUTUBE_VIDEO_ID_CAPACITY] = {0};
        char next_id[YOUTUBE_VIDEO_ID_CAPACITY] = {0};
        bool same_internal_video =
            youtube_watch_url_video_id(media->source, current_id)
            && youtube_watch_url_video_id(url, next_id)
            && strcmp(current_id, next_id) == 0;
        if (same_internal_video && media->player != NULL) {
#ifdef TILEFINCH_HAVE_HOST_MEDIA
            host_media_set_playing(media->player, false);
#endif
            media->suspended_for_internal_view = true;
            media->ui.visible = false;
            return true;
        }
        if (media->source[0] != '\0'
            && media->generation != navigation->generation) {
            interactive_media_destroy(media);
        }
        return true;
    }
#ifdef TILEFINCH_HAVE_HOST_MEDIA
    if (media->suspended_for_internal_view && media->player != NULL) {
        char current_id[YOUTUBE_VIDEO_ID_CAPACITY] = {0};
        char next_id[YOUTUBE_VIDEO_ID_CAPACITY] = {0};
        if (youtube_watch_url_video_id(media->source, current_id)
            && youtube_watch_url_video_id(url, next_id)
            && strcmp(current_id, next_id) == 0) {
            char error[256] = {0};
            if (!host_media_reattach_native_player(
                    media->player, navigation,
                    error, sizeof(error))) {
                fprintf(stderr,
                        "native media reattachment failed: %s\n",
                        error[0] == '\0' ? "unknown error" : error);
                psp_ui_media_set_error(
                    &media->ui, error[0] == '\0'
                        ? "VIDEO RESTORE FAILED" : error);
                return true;
            }
            media->suspended_for_internal_view = false;
            media->generation = navigation->generation;
            media->ui.visible = true;
            psp_ui_media_show_controls(&media->ui);
            return interactive_media_sync_ui(media);
        }
    }
#endif
    if ((media->player != NULL || media->pending_open || media->open_failed)
        && media->generation == navigation->generation
        && strcmp(media->source, url) == 0) return true;
    interactive_media_destroy(media);
    media->pending_open = true;
    media->open_failed = false;
    media->native_player = true;
    media->generation = navigation->generation;
    snprintf(media->source, sizeof(media->source), "%s", url);
    psp_ui_media_set_resolving(&media->ui, "YouTube video");
    printf("native-media route=youtube state=resolving url=\"%s\"\n", url);
    return true;
}

static bool interactive_media_set_visible(InteractiveMediaSession *media,
                                          NavigationSession *navigation,
                                          bool visible)
{
    if (media == NULL || media->player == NULL) return false;
#ifdef TILEFINCH_HAVE_HOST_MEDIA
    char error[256] = {0};
    host_media_set_playing(media->player, visible && media->ui.playing);
    if (media->native_player
        && !host_media_set_native_player_visible(
               media->player, navigation, visible,
               error, sizeof(error))) {
        fprintf(stderr, "native media visibility failed: %s\n", error);
        return false;
    }
    media->ui.visible = visible;
    if (visible) psp_ui_media_show_controls(&media->ui);
    return interactive_media_sync_ui(media);
#else
    (void) navigation;
    (void) visible;
    return false;
#endif
}

static bool interactive_media_seek(InteractiveMediaSession *media,
                                   NavigationSession *navigation,
                                   TileCache *cache,
                                   uint64_t target_time_us)
{
    if (media == NULL || media->player == NULL) return false;
#ifdef TILEFINCH_HAVE_HOST_MEDIA
    char error[256] = {0};
    bool changed = false;
    if (!host_media_seek(
            media->player, navigation, target_time_us, &changed,
            error, sizeof(error))) {
        fprintf(stderr, "media seek failed: %s\n", error);
        return false;
    }
    if (changed) {
        tile_cache_invalidate_image_identity(
            cache, host_media_surface_identity(media->player, navigation));
        tile_cache_invalidate_rect(
            cache, 0, 0, 480, navigation->page.layout.height);
    }
    psp_ui_media_show_controls(&media->ui);
    return interactive_media_sync_ui(media);
#else
    (void) navigation;
    (void) cache;
    (void) target_time_us;
    return false;
#endif
}

static bool interactive_media_preview_seek(
    InteractiveMediaSession *media, NavigationSession *navigation,
    TileCache *cache, uint64_t target_time_us)
{
    if (media == NULL || media->player == NULL || media->budget == NULL)
        return false;
#ifdef TILEFINCH_HAVE_HOST_MEDIA
    if (media->seek_preview_pixels == NULL) {
        media->seek_preview_pixels = budget_calloc_category(
            media->budget, BUDGET_CATEGORY_RESOURCE,
            INTERACTIVE_MEDIA_PREVIEW_WIDTH
                * INTERACTIVE_MEDIA_PREVIEW_HEIGHT,
            sizeof(*media->seek_preview_pixels));
        if (media->seek_preview_pixels == NULL) {
            psp_ui_media_cancel_seek_preview(&media->ui);
            return false;
        }
    }
    if (!media->seek_preview_started) {
        media->seek_preview_was_playing = media->ui.playing;
        media->seek_preview_started = true;
        host_media_set_playing(media->player, false);
    }
    uint64_t original_time_us = media->ui.current_time_us;
    char error[256] = {0};
    bool changed = false;
    bool restored = false;
    bool sought_target = host_media_seek(
        media->player, navigation, target_time_us, &changed,
        error, sizeof(error));
    bool ok = sought_target
        && host_media_copy_current_frame_rgb565(
            media->player, media->seek_preview_pixels,
            INTERACTIVE_MEDIA_PREVIEW_WIDTH,
            INTERACTIVE_MEDIA_PREVIEW_HEIGHT,
            INTERACTIVE_MEDIA_PREVIEW_WIDTH);
    if (sought_target)
        restored = host_media_seek(
            media->player, navigation, original_time_us, &changed,
            error, sizeof(error));
    if (!ok || !restored) {
        fprintf(stderr, "media preview seek failed: %s\n",
                error[0] == '\0' ? "frame unavailable" : error);
        media->seek_preview_started = false;
        psp_ui_media_cancel_seek_preview(&media->ui);
        host_media_set_playing(
            media->player, media->seek_preview_was_playing);
        (void) interactive_media_sync_ui(media);
        return false;
    }
    if (changed) {
        tile_cache_invalidate_image_identity(
            cache, host_media_surface_identity(media->player, navigation));
        tile_cache_invalidate_rect(
            cache, 0, 0, 480, navigation->page.layout.height);
    }
    if (!interactive_media_sync_ui(media)) return false;
    psp_ui_media_set_seek_preview(&media->ui, target_time_us);
    return true;
#else
    (void) navigation;
    (void) cache;
    (void) target_time_us;
    return false;
#endif
}

static bool interactive_media_execute_intent(
    InteractiveMediaSession *media, NavigationSession *navigation,
    TileCache *cache, PspUiMediaIntent intent)
{
    if (media == NULL) return false;
    if (intent.action == PSP_UI_MEDIA_ACTION_RETRY) {
#ifdef TILEFINCH_HAVE_HOST_MEDIA
        host_media_destroy(media->player);
#endif
        media->player = NULL;
        media->open_failed = false;
        media->pending_open = media->source[0] != '\0';
        if (media->pending_open)
            psp_ui_media_set_resolving(&media->ui, "YouTube video");
        return media->pending_open;
    }
    if (media->player == NULL) return false;
    switch (intent.action) {
        case PSP_UI_MEDIA_ACTION_NONE:
            return true;
        case PSP_UI_MEDIA_ACTION_PLAY_PAUSE:
#ifdef TILEFINCH_HAVE_HOST_MEDIA
            host_media_set_playing(media->player, !media->ui.playing);
#endif
            return interactive_media_sync_ui(media);
        case PSP_UI_MEDIA_ACTION_PREVIEW_SEEK:
            return interactive_media_preview_seek(
                media, navigation, cache, intent.seek_time_us);
        case PSP_UI_MEDIA_ACTION_CANCEL_SEEK_PREVIEW:
#ifdef TILEFINCH_HAVE_HOST_MEDIA
            if (media->seek_preview_started)
                host_media_set_playing(
                    media->player, media->seek_preview_was_playing);
#endif
            media->seek_preview_started = false;
            return interactive_media_sync_ui(media);
        case PSP_UI_MEDIA_ACTION_SEEK:
        {
            bool resume = media->seek_preview_started
                && media->seek_preview_was_playing;
            media->seek_preview_started = false;
            bool sought = interactive_media_seek(
                media, navigation, cache, intent.seek_time_us);
#ifdef TILEFINCH_HAVE_HOST_MEDIA
            if (sought && resume) {
                host_media_set_playing(media->player, true);
                sought = interactive_media_sync_ui(media);
            }
#endif
            return sought;
        }
        case PSP_UI_MEDIA_ACTION_RETRY:
            return true;
        case PSP_UI_MEDIA_ACTION_CLOSE:
            return interactive_media_set_visible(
                media, navigation, false);
    }
    return false;
}

static bool interactive_media_button(InteractiveMediaSession *media,
                                     NavigationSession *navigation,
                                     TileCache *cache,
                                     uint32_t button)
{
    if (media == NULL || !media->ui.visible) return false;
    PspUiInput input = {
        .pressed = button,
        .held = button,
        .analog_x = 128,
        .analog_y = 128
    };
    return interactive_media_execute_intent(
        media, navigation, cache,
        psp_ui_media_update(&media->ui, &input));
}

static bool interactive_media_activate_at(InteractiveMediaSession *media,
                                          NavigationSession *navigation,
                                          TileCache *cache,
                                          int x, int y)
{
    if (media == NULL || !media->ui.visible) return false;
    return interactive_media_execute_intent(
        media, navigation, cache,
        psp_ui_media_activate_at(&media->ui, x, y, 480, 272));
}

static bool interactive_media_advance(InteractiveMediaSession *media,
                                      NavigationSession *navigation,
                                      TileCache *cache,
                                      unsigned elapsed_ms,
                                      bool *frame_changed)
{
    if (frame_changed != NULL) *frame_changed = false;
    if (media == NULL) return true;
#ifdef TILEFINCH_HAVE_HOST_MEDIA
    ScriptMediaRequest dom_request;
    if (navigation->page.runtime != NULL
        && script_runtime_consume_media_request(
            navigation->page.runtime, &dom_request)) {
        char dom_error[256] = {0};
        if (media->player == NULL
            || strcmp(media->source, dom_request.source) != 0) {
            interactive_media_destroy(media);
            if (!interactive_media_open(
                    media, media->budget, navigation,
                    dom_request.source, false,
                    dom_error, sizeof(dom_error))) {
                (void) script_runtime_update_media_state(
                    navigation->page.runtime, dom_request.node_handle,
                    SCRIPT_MEDIA_STATE_ERROR, 0, 0);
                fprintf(stderr, "DOM media open failed: %s\n", dom_error);
                return true;
            }
        }
        media->dom_node_handle = dom_request.node_handle;
        if (dom_request.command == SCRIPT_MEDIA_COMMAND_SEEK) {
            (void) interactive_media_seek(
                media, navigation, cache,
                (uint64_t) (dom_request.value * 1000000.0));
        } else if (dom_request.command == SCRIPT_MEDIA_COMMAND_PAUSE
                   || dom_request.command == SCRIPT_MEDIA_COMMAND_LOAD) {
            host_media_set_playing(media->player, false);
        } else if (dom_request.command == SCRIPT_MEDIA_COMMAND_PLAY) {
            host_media_set_playing(media->player, true);
        }
        (void) interactive_media_sync_ui(media);
    }
    if (media->pending_open) {
        char open_error[256] = {0};
        char source[NAVIGATION_URL_LIMIT];
        snprintf(source, sizeof(source), "%s", media->source);
        media->pending_open = false;
        if (!interactive_media_open(
                media, media->budget, navigation,
                source, true, open_error, sizeof(open_error))) {
            media->open_failed = true;
            psp_ui_media_set_error(
                &media->ui, open_error[0] == '\0'
                    ? "VIDEO UNAVAILABLE" : open_error);
            fprintf(stderr,
                    "native media route unavailable url=\"%s\" error=\"%s\"\n",
                    source, open_error[0] == '\0'
                        ? "host backend disabled" : open_error);
            return true;
        }
        printf("native-media route=youtube state=ready url=\"%s\"\n",
               source);
        if (frame_changed != NULL) *frame_changed = true;
    }
    if (media->suspended_for_internal_view) return true;
    if (media->player == NULL) return true;
    char error[256] = {0};
    bool changed = false;
    if (!host_media_advance(
            media->player, navigation, elapsed_ms, &changed,
            error, sizeof(error))) {
        fprintf(stderr, "media advance failed: %s\n", error);
        psp_ui_media_set_error(
            &media->ui, error[0] == '\0' ? "PLAYBACK FAILED" : error);
        return true;
    }
    if (changed) {
        tile_cache_invalidate_image_identity(
            cache, host_media_surface_identity(media->player, navigation));
        tile_cache_invalidate_rect(
            cache, 0, 0, 480, navigation->page.layout.height);
    }
    psp_ui_media_tick(&media->ui, elapsed_ms);
    if (!interactive_media_sync_ui(media)) return false;
    HostMediaStats progress_stats;
    if (host_media_get_stats(media->player, &progress_stats)) {
        if (progress_stats.playing
            && progress_stats.decoded_frames == media->last_decoded_frames) {
            media->no_frame_ms =
                elapsed_ms > UINT_MAX - media->no_frame_ms
                    ? UINT_MAX : media->no_frame_ms + elapsed_ms;
        } else {
            media->no_frame_ms = 0;
        }
        media->last_decoded_frames = progress_stats.decoded_frames;
        psp_ui_media_set_buffering(
            &media->ui,
            progress_stats.playing && media->no_frame_ms >= 750u,
            progress_stats.current_time_us);
    }
    if (media->dom_node_handle != 0
        && navigation->page.runtime != NULL) {
        HostMediaStats stats;
        if (host_media_get_stats(media->player, &stats)) {
            ScriptMediaState state = stats.ended
                ? SCRIPT_MEDIA_STATE_ENDED
                : (stats.playing ? SCRIPT_MEDIA_STATE_PLAYING
                                 : SCRIPT_MEDIA_STATE_PAUSED);
            (void) script_runtime_update_media_state(
                navigation->page.runtime, media->dom_node_handle, state,
                (double) stats.current_time_us / 1000000.0,
                (double) stats.duration_us / 1000000.0);
        }
    }
    if (frame_changed != NULL) *frame_changed = changed;
    return true;
#else
    (void) navigation;
    (void) cache;
    (void) elapsed_ms;
    /* A build without the optional host decoder still has a valid inactive
       media session. Runtime ticks must remain a no-op success so ordinary
       scripted pages do not fail merely because video support was omitted. */
    return true;
#endif
}

static void interactive_media_composite(const InteractiveMediaSession *media,
                                        uint16_t *frame,
                                        int width, int height, int stride)
{
    if (media == NULL) return;
    PspUiMediaPreview preview = {
        .pixels = media->seek_preview_pixels,
        .width = INTERACTIVE_MEDIA_PREVIEW_WIDTH,
        .height = INTERACTIVE_MEDIA_PREVIEW_HEIGHT,
        .stride = INTERACTIVE_MEDIA_PREVIEW_WIDTH
    };
    psp_ui_media_composite_with_preview(
        &media->ui,
        media->ui.seek_preview_active ? &preview : NULL,
        frame, width, height, stride);
}
