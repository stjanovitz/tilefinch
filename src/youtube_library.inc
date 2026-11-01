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
        "data-yt-video=\"%s\" href=\"" YOUTUBE_LITE_FRAGMENT_ACCOUNT_ACTION "\" ",
        control_id, icon ? " icon-action" : "", (unsigned) action, target, channel ? "" : video)
        && lite_html_action_content(html, label, icon)
        && lite_html_text(html, "</a> ");
}

/* A channel's subscribe control, shown only signed in: the watch page's
   bell. A state the page states (1 subscribed, 0 not) shows only the
   action that changes it, the lit bell being Unsubscribe. Otherwise the
   bell subscribes and Unsubscribe waits in the account menu, as on the
   watch page. */
static bool lite_html_channel_subscribe(YoutubeLiteHtml *html,
    const char *channel, int subscribed)
{
    if (subscribed == 1) return lite_html_account_action(html, YOUTUBE_LITE_ACTION_UNSUBSCRIBE,
            channel, "", "Unsubscribe", "yt-unsubscribe-action", "bell-on");
    return lite_html_account_action(html, YOUTUBE_LITE_ACTION_SUBSCRIBE,
            channel, "", "Subscribe", "yt-subscribe-action", "bell")
        && (subscribed == 0 || (lite_html_text(html,
                "<details id=yt-channel-account class=watch-account-menu><summary class=icon-action ")
            && lite_html_action_content(html, "Account", "more")
            && lite_html_text(html, "</summary><div class=watch-account-panel><p>")
            && lite_html_account_action(html, YOUTUBE_LITE_ACTION_UNSUBSCRIBE,
                channel, "", "Unsubscribe", "yt-unsubscribe-action", NULL)
            && lite_html_text(html, "</p></div></details>")));
}

static bool lite_html_account_notice(YoutubeLiteHtml *html)
{
    return lite_html_ui_markup(html,
        "<p id=yt-library-status class=hint style='min-height:32px'>{{Select an action.}}</p>");
}

/* One playlist renderer: its ID, title, count line and a thumbnail video. */
static void lite_parse_playlist_record(const YoutubeLiteSpan *first,
    YoutubeLiteVideo *record)
{
    *record = (YoutubeLiteVideo) {0};
    if (!lite_json_key_string(first, "playlistId", record->playlist_id,
                             sizeof(record->playlist_id))) {
        char type[48] = {0};
        if (!lite_member_string(first, "contentType", type, sizeof(type), false)
            || strcmp(type, "LOCKUP_CONTENT_TYPE_PLAYLIST") != 0
            || !lite_member_string(first, "contentId", record->playlist_id,
                sizeof(record->playlist_id), false)) return;
    }
    if (!lite_valid_account_id(record->playlist_id, false)) {
        record->playlist_id[0] = '\0'; return;
    }
    YoutubeLiteSpan metadata;
    if (!lite_text_runs(first, "title", record->title, sizeof(record->title))
        && lite_json_key(first, "lockupMetadataViewModel", NULL, &metadata)) {
        YoutubeLiteSpan title;
        if (lite_json_member(&metadata, "title", &title))
            (void) lite_member_string(&title, "content", record->title,
                                     sizeof(record->title), true);
    }
    if (record->title[0] == '\0') snprintf(record->title,
        sizeof(record->title), "%s", tilefinch_ui_logical_text("Playlist"));
    if (!lite_text_runs(first, "videoCountText", record->views, sizeof(record->views))
        && lite_json_key(first, "thumbnailBadgeViewModel", NULL, &metadata))
        (void) lite_member_string(&metadata, "text", record->views,
                                  sizeof(record->views), true);
    const char *thumb = lite_find_bytes(first->start, first->end, "/vi/");
    for (size_t i = 0; thumb != NULL && i + 1u < sizeof(record->id)
         && thumb + 4 + i < first->end && thumb[4 + i] != '/'; i++)
        record->id[i] = thumb[4 + i];
    if (!lite_valid_video_id(record->id)) record->id[0] = '\0';
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
    lite_parse_playlist_record(&first, record);
    return true;
}

/* A playlist on a channel page: the result card's shape, opening the
   playlist route, labelled as a playlist with its count. */
static bool lite_html_playlist_row(YoutubeLiteHtml *html,
    const YoutubeLiteVideo *record)
{
    return lite_html_format(html, "<div class=result-row><a class=card href=\""
            "https://www.youtube.com/playlist?list=%s\"><span class=thumb-wrap>",
            record->playlist_id)
        && (record->id[0] == '\0' || lite_html_format(html,
            "<img class=thumb src=\"https://i.ytimg.com/vi/%s/mqdefault.jpg\" "
            "width=160 height=90 loading=lazy alt=\"\">", record->id))
        && lite_html_ui_markup(html, "<span class=duration>{{Playlist}}</span></span>"
            "<span class=info><span class=title>")
        && lite_html_escape(html, record->title)
        && lite_html_text(html, "</span><span class=meta>")
        && lite_html_escape(html, record->views)
        && lite_html_text(html, "</span></span></a></div>");
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
