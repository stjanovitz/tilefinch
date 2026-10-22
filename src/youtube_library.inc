/* Provider-owned account controls: no credentials or service tokens in HTML. */
#if defined(__GNUC__)
__attribute__((noinline))
#endif
static bool lite_html_account_action(YoutubeLiteHtml *html,
    YoutubeLiteAccountAction action, const char *target, const char *video,
    const char *label, const char *control_id, const char *icon)
{
    bool channel = action == YOUTUBE_LITE_ACTION_SUBSCRIBE
        || action == YOUTUBE_LITE_ACTION_UNSUBSCRIBE;
    if (!lite_valid_account_id(target, channel)
        || (!channel && !lite_valid_video_id(video))) return false;
    return lite_html_format(html,
        "<a id=\"%s\" class=\"comments-link%s\" data-yt-action=%u data-yt-target=\"%s\" "
        "data-yt-video=\"%s\" href=\"#tilefinch-account-action\" ",
        control_id, icon ? " icon-action" : "", (unsigned) action, target, channel ? "" : video)
        && lite_html_action_content(html, label, icon)
        && lite_html_text(html, "</a> ");
}

static bool lite_html_subscribe(YoutubeLiteHtml *html, const char *channel)
{
    if (!lite_valid_account_id(channel, true)) return true;
    return lite_html_text(html, "<p>")
        && lite_html_account_action(html, YOUTUBE_LITE_ACTION_SUBSCRIBE,
            channel, "", "Subscribe", "yt-subscribe-action", NULL)
        && lite_html_account_action(html, YOUTUBE_LITE_ACTION_UNSUBSCRIBE,
            channel, "", "Unsubscribe", "yt-unsubscribe-action", NULL)
        && lite_html_text(html, "</p>");
}

static bool lite_html_account_notice(YoutubeLiteHtml *html)
{
    return lite_html_ui_markup(html,
        "<p id=yt-library-status class=hint style='min-height:32px'>{{Select an action.}}</p>");
}

static bool lite_parse_playlist_card(const YoutubeLiteSpan *scope,
    YoutubeLiteSpan *renderer, YoutubeLiteVideo *record)
{
    static const char *const keys[] = {
        "playlistRenderer", "gridPlaylistRenderer", "lockupViewModel"
    };
    YoutubeLiteSpan first = {0};
    for (unsigned i = 0; i < 3u; i++) {
        YoutubeLiteSpan found;
        if (lite_json_key(scope, keys[i], NULL, &found)
            && (first.start == NULL || found.start < first.start)) first = found;
    }
    if (first.start == NULL) return false;
    *renderer = first;
    *record = (YoutubeLiteVideo) {0};
    if (!lite_json_key_string(&first, "playlistId", record->playlist_id,
                             sizeof(record->playlist_id))) {
        char type[48] = {0};
        if (!lite_member_string(&first, "contentType", type, sizeof(type), false)
            || strcmp(type, "LOCKUP_CONTENT_TYPE_PLAYLIST") != 0
            || !lite_member_string(&first, "contentId", record->playlist_id,
                sizeof(record->playlist_id), false)) return true;
    }
    if (!lite_valid_account_id(record->playlist_id, false)) {
        record->playlist_id[0] = '\0'; return true;
    }
    if (!lite_text_runs(&first, "title", record->title, sizeof(record->title))) {
        YoutubeLiteSpan metadata;
        if (lite_json_key(&first, "lockupMetadataViewModel", NULL, &metadata)) {
            YoutubeLiteSpan title;
            if (lite_json_member(&metadata, "title", &title))
                (void) lite_member_string(&title, "content", record->title,
                                         sizeof(record->title), true);
        }
    }
    if (record->title[0] == '\0') snprintf(record->title,
        sizeof(record->title), "%s", tilefinch_ui_logical_text("Playlist"));
    return true;
}

static bool lite_html_playlist_card(YoutubeLiteHtml *html,
    const YoutubeLiteVideo *record, const char *save_video)
{
    return lite_html_format(html,
        "<section><a class=more href=\"https://www.youtube.com/playlist?list=%s\">",
        record->playlist_id)
        && lite_html_escape(html, record->title)
        && lite_html_text(html, "</a>")
        && (save_video[0] == '\0' || lite_html_account_action(html,
            YOUTUBE_LITE_ACTION_PLAYLIST_ADD, record->playlist_id, save_video,
            "Add to this playlist", "", NULL))
        && lite_html_text(html, "</section>");
}

static void lite_channel_metadata(const YoutubeLiteSpan *scope,
                                  char channel[YOUTUBE_CHANNEL_ID_CAPACITY])
{
    YoutubeLiteSpan object;
    if (channel[0] == '\0'
        && lite_json_key(scope, "channelMetadataRenderer", NULL, &object)) {
        char id[YOUTUBE_CHANNEL_ID_CAPACITY] = {0};
        if (lite_member_string(&object, "externalId", id, sizeof(id), false)
            && lite_valid_account_id(id, true)) memcpy(channel, id, sizeof(id));
    }
}
