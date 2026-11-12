/* Watch-next extras and the watch page's channel link: bounded fields from
   the owner card and from playlist suggestions, never a service URL. */

/* The owner card names the channel by ID, links it by handle and shows a
   small avatar. Only the first card counts; a later one (a collaborator or
   an unrelated module) never replaces it. */
static void lite_watch_owner(const YoutubeLiteSpan *renderer, YoutubeLiteWatch *watch)
{
    YoutubeLiteSpan endpoint, browse, thumbnail, list, first;
    char id[YOUTUBE_CHANNEL_ID_CAPACITY] = {0};
    if (watch->owner_id[0] != '\0'
        || !lite_json_member(renderer, "navigationEndpoint", &endpoint)
        || !lite_json_member(&endpoint, "browseEndpoint", &browse)
        || !lite_member_string(&browse, "browseId", id, sizeof(id), false)
        || !lite_valid_account_id(id, true)) return;
    memcpy(watch->owner_id, id, sizeof(id));
    char handle[YOUTUBE_LITE_HANDLE_LIMIT] = {0};
    bool valid = lite_member_string(&browse, "canonicalBaseUrl", handle, sizeof(handle), false)
        && handle[0] == '/' && handle[1] == '@' && handle[2] != '\0';
    for (const char *at = handle + 2; valid && *at != '\0'; at++)
        valid = isalnum((unsigned char) *at) || strchr("._-%", *at) != NULL;
    if (valid) memcpy(watch->owner_handle, handle, sizeof(handle));
    if (watch->video.channel[0] == '\0')
        (void) lite_text_runs(renderer, "title", watch->video.channel, sizeof(watch->video.channel));
    char avatar[YOUTUBE_LITE_AVATAR_LIMIT] = {0};
    const char *cursor = NULL;
    if (lite_json_member(renderer, "thumbnail", &thumbnail)
        && lite_json_member(&thumbnail, "thumbnails", &list)
        && lite_array_next(&list, &cursor, &first)
        && lite_member_string(&first, "url", avatar + 6, sizeof(avatar) - 6u, false)) {
        /* Protocol-relative URLs are common in owner cards. */
        const char *url = avatar + 6;
        if (url[0] == '/' && url[1] == '/') url = memcpy(avatar, "https:", 6);
        if ((strncmp(url, "https://yt3.ggpht.com/", 22) == 0
                || strncmp(url, "https://yt3.googleusercontent.com/", 34) == 0)
            && strpbrk(url, "\"'<> \\") == NULL)
            snprintf(watch->owner_avatar, sizeof(watch->owner_avatar), "%s", url);
    }
}

/* Consumes the watch-next and history extras: the owner card, playlist
   suggestions (only with a card table) and skipped containers. False leaves
   a video lockup to the ordinary card parser. */
static bool lite_related_extra(const YoutubeLiteSpan *renderer, size_t kind,
    bool watch_route, YoutubeLiteWatch *watch, YoutubeLiteVideo *videos, size_t *count)
{
    if (kind == YOUTUBE_LITE_RENDERER_OWNER
        || kind == YOUTUBE_LITE_RENDERER_DESKTOP_OWNER) {
        if (watch_route) lite_watch_owner(renderer, watch);
        return true;
    }
    if (kind >= YOUTUBE_LITE_RENDERER_SKIPPED) return true;
    /* Mixes (RD...) are generated per viewer and are not shown. */
    YoutubeLiteVideo record;
    char type[48];
    if (!watch_route || videos == NULL
        || (kind == YOUTUBE_LITE_RENDERER_LOCKUP
            && (!lite_member_string(renderer, "contentType", type, sizeof(type), false)
                || strcmp(type, "LOCKUP_CONTENT_TYPE_PLAYLIST") != 0)))
        return kind == YOUTUBE_LITE_RENDERER_COMPACT_PLAYLIST;
    lite_parse_playlist_record(renderer, &record);
    if (record.playlist_id[0] == '\0' || strncmp(record.playlist_id, "RD", 2) == 0)
        return kind == YOUTUBE_LITE_RENDERER_COMPACT_PLAYLIST
            || record.playlist_id[0] != '\0';
    record.playlist = true;
    for (size_t i = 0; i < *count; i++)
        if (videos[i].playlist && strcmp(videos[i].playlist_id, record.playlist_id) == 0)
            return true;
    if (*count < YOUTUBE_LITE_MAXIMUM_RESULTS) videos[(*count)++] = record;
    return true;
}

/* The channel ID the watch page links, or "" for a plain-text name. */
static const char *lite_watch_channel_link(const YoutubeLiteWatch *watch)
{
    return watch->video.channel[0] == '\0' ? ""
        : watch->channel_id[0] != '\0' ? watch->channel_id : watch->owner_id;
}

/* The channel name, linked to the channel route, leads the action row:
   down from the play target reaches it, and Subscribe follows it. A handle
   or avatar is used only from an owner card naming the same channel as the
   player response. */
static bool lite_html_watch_channel(YoutubeLiteHtml *html, const YoutubeLiteWatch *watch)
{
    const char *id = lite_watch_channel_link(watch);
    if (id[0] == '\0') return true;
    bool owner = strcmp(watch->owner_id, id) == 0;
    return lite_html_text(html, "<a id=yt-watch-channel class=channel-link "
            "href=\"https://www.youtube.com")
        && (owner && watch->owner_handle[0] != '\0'
            ? lite_html_escape(html, watch->owner_handle)
            : lite_html_format(html, "/channel/%s", id))
        && lite_html_text(html, "\">")
        && (!owner || watch->owner_avatar[0] == '\0'
            || (lite_html_text(html, "<img class=channel-avatar src=\"")
                && lite_html_escape(html, watch->owner_avatar)
                && lite_html_text(html, "\" width=24 height=24 loading=lazy alt=\"\">")))
        && lite_html_escape(html, watch->video.channel)
        && lite_html_text(html, "</a>");
}

/* Library pages link each other: Watch Later, Playlists and History. */
static bool lite_html_library_nav(YoutubeLiteHtml *html)
{
    return lite_html_ui_markup(html,
        "<nav><a class=comments-link href=\"https://www.youtube.com/playlist?list=WL\">{{Watch Later}}</a> "
        "<a class=comments-link href=\"https://www.youtube.com/feed/playlists\">{{Playlists}}</a> "
        "<a class=comments-link href=\"https://www.youtube.com/feed/history\">{{History}}</a></nav>");
}
