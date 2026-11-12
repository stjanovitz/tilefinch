/*
 * Channel pages. The Home tab lists the shelves the channel curates, in its
 * order, each under its own heading with a few entries and its "See all"
 * link; the Videos, Shorts and Playlists tabs list that tab's entries with
 * a next-page link. Entries are written to the page as the bounded walk
 * finds them, so no entry table is kept. The header (name, subscriber line,
 * avatar, banner) is read first from the page's header and metadata objects,
 * which follow the contents in the response. With channel banners hidden
 * (the Channel banners setting, or tilefinch_banners=0) the banner is not
 * read and the page names no banner image, so none is fetched or decoded.
 * The header's description line
 * opens the full-description view (tilefinch_view=description), read from
 * the About panel continuation the page header names, or else from the page.
 */
#define LITE_CHANNEL_SHELVES 8u
#define LITE_CHANNEL_SHELF_ITEMS 4u
#define LITE_CHANNEL_HOME_ITEMS 24u
#define LITE_CHANNEL_TAB_ITEMS 32u
#define LITE_CHANNEL_HEADER_BYTES (64u * 1024u)

enum { LITE_CHANNEL_HOME = 0, LITE_CHANNEL_VIDEOS, LITE_CHANNEL_SHORTS,
       LITE_CHANNEL_PLAYLISTS, LITE_CHANNEL_OTHER };

/* The channel's own path (/@name, /channel/ID, /c/name, /user/name), its
   length, and which tab the rest of the path selects. */
static const char *lite_channel_path(const char *url, size_t *base,
                                     unsigned *tab)
{
    static const char *const tabs[] = {"featured", "videos", "shorts", "playlists"};
    TilefinchUrl parsed;
    *base = 0;
    *tab = LITE_CHANNEL_OTHER;
    if (url == NULL || !tilefinch_url_parse(url, &parsed)
        || parsed.path_length < 2u) return NULL;
    const char *path = parsed.value + parsed.path_offset;
    /* "/@name/videos/" selects the same tab as "/@name/videos". */
    size_t length = lite_path_length(&parsed);
    const char *end = path + length;
    const char *at = memchr(path + 1, '/', length - 1u);
    if (path[1] != '@' && at != NULL) at = memchr(at + 1, '/', (size_t) (end - at - 1));
    if (at == NULL) at = end;
    *base = (size_t) (at - path);
    size_t rest = (size_t) (end - at);
    if (rest <= 1u) *tab = LITE_CHANNEL_HOME;
    for (unsigned i = 0; i < 4u; i++)
        if (rest == strlen(tabs[i]) + 1u && memcmp(at + 1, tabs[i], rest - 1u) == 0)
            *tab = i;
    return path;
}

/* The smallest listed image at least `minimum` pixels wide (else the
   widest), from a sources/thumbnails list, when it is a YouTube image. */
static void lite_channel_image(const YoutubeLiteSpan *scope, unsigned minimum,
                               char output[256])
{
    YoutubeLiteSpan list, item, width;
    if (!lite_json_key(scope, "sources", NULL, &list)
        && !lite_json_key(scope, "thumbnails", NULL, &list)) return;
    const char *cursor = NULL;
    unsigned best = 0;
    for (unsigned n = 0; n < 16u && lite_array_next(&list, &cursor, &item); n++) {
        unsigned pixels = 0;
        if (lite_json_member(&item, "width", &width))
            for (const char *at = width.start; at < width.end && *at >= '0'
                 && *at <= '9' && pixels < 100000u; at++)
                pixels = pixels * 10u + (unsigned) (*at - '0');
        bool better = best == 0 || (pixels >= minimum
            ? best < minimum || pixels < best : best < minimum && pixels > best);
        char url[256];
        if (!better || !lite_member_string(&item, "url", url + 6, sizeof(url) - 6u, false))
            continue;
        const char *value = url + 6;
        if (value[0] == '/' && value[1] == '/') { memcpy(url, "https:", 6); value = url; }
        if (strncmp(value, "https://yt3.googleusercontent.com/", 34) != 0
            && strncmp(value, "https://yt3.ggpht.com/", 22) != 0) continue;
        snprintf(output, 256, "%s", value);
        best = pixels == 0 ? 1u : pixels;
    }
}

/* A description without the trailing line breaks it often ends with. */
static void lite_channel_trim(char *text)
{
    size_t length = strlen(text);
    while (length != 0 && (unsigned char) text[length - 1u] <= ' ') text[--length] = '\0';
}

#if defined(__GNUC__)
__attribute__((noinline))
#endif
static void lite_channel_header_parse(YoutubeLiteBuildWork *work, size_t found)
{
    YoutubeLiteChannel *channel = &work->channel;
    const char *data = work->decoded;
    YoutubeLiteSpan window = lite_build_window(data, work->decoded_length, found), value;
    /* The entry walk ends where the header and metadata begin: the header
       holds its own continuation (the About panel), not the next page. */
    work->metadata_start = found;
    if (lite_json_key(&window, "channelMetadataRenderer", NULL, &value)) {
        (void) lite_member_string(&value, "title", channel->name, sizeof(channel->name), true);
        char id[YOUTUBE_CHANNEL_ID_CAPACITY] = {0};
        if (lite_member_string(&value, "externalId", id, sizeof(id), false)
            && lite_valid_account_id(id, true)) memcpy(work->channel_id, id, sizeof(id));
        (void) lite_member_string(&value, "description", channel->description,
                                  sizeof(channel->description), true);
        lite_channel_trim(channel->description);
    }
    /* The page header object ends where the metadata begins. */
    static const char *const names[] = {"pageHeaderViewModel", "c4TabbedHeaderRenderer"};
    size_t start = found > LITE_CHANNEL_HEADER_BYTES ? found - LITE_CHANNEL_HEADER_BYTES : 0;
    YoutubeLiteSpan header = {0};
    for (unsigned i = 0; i < 2u && header.start == NULL; i++) {
        size_t length = strlen(names[i]);
        for (size_t at = found; at > start + length + 1u; at--) {
            if (data[at - 1u] == '"' && memcmp(data + at - 1u - length, names[i], length) == 0
                && data[at - 2u - length] == '"') {
                YoutubeLiteSpan scope = {data + at - 2u - length, data + found};
                (void) lite_json_key(&scope, names[i], NULL, &header);
                break;
            }
        }
    }
    if (header.start == NULL) return;
    work->metadata_start = (size_t) (header.start - data);
    YoutubeLiteSpan rows, row, parts, part, text;
    if (lite_json_key(&header, "metadataRows", NULL, &rows)) {
        const char *row_cursor = NULL;
        size_t used = 0;
        for (unsigned r = 0; r < 4u && lite_array_next(&rows, &row_cursor, &row); r++) {
            const char *part_cursor = NULL;
            if (!lite_json_member(&row, "metadataParts", &parts)) continue;
            for (unsigned p = 0; p < 4u && lite_array_next(&parts, &part_cursor, &part); p++) {
                char content[64];
                if (!lite_json_member(&part, "text", &text)
                    || !lite_member_string(&text, "content", content, sizeof(content), true)
                    || content[0] == '@' || content[0] == '\0') continue;
                int n = snprintf(channel->stats + used, sizeof(channel->stats) - used,
                                 "%s%s", used ? " \xc2\xb7 " : "", content);
                if (n < 0 || (size_t) n >= sizeof(channel->stats) - used) {
                    channel->stats[used] = '\0'; r = 4u; break;
                }
                used += (size_t) n;
            }
        }
    } else {
        (void) lite_text_runs(&header, "subscriberCountText",
                              channel->stats, sizeof(channel->stats));
    }
    /* The description preview opens the About panel by its continuation. */
    char token[YOUTUBE_LITE_CONTINUATION_LIMIT];
    if (lite_json_key(&header, "descriptionPreviewViewModel", NULL, &value)
        && lite_next_continuation(value.start, (size_t) (value.end - value.start), token)
        && strlen(token) < sizeof(channel->about_token))
        memcpy(channel->about_token, token, strlen(token) + 1u);
    if (lite_json_key(&header, "avatar", NULL, &value))
        lite_channel_image(&value, 48u, channel->avatar);
    if (work->channel_banners && lite_json_key(&header, "banner", NULL, &value))
        lite_channel_image(&value, 480u, channel->banner);
    if (lite_json_key(&header, "subscribed", NULL, &value)
        || lite_json_key(&header, "isSubscribed", NULL, &value)) {
        size_t length = (size_t) (value.end - value.start);
        if (length == 4u && memcmp(value.start, "true", 4) == 0) channel->subscribed = 1;
        else if (length == 5u && memcmp(value.start, "false", 5) == 0) channel->subscribed = 0;
    }
}

/* "a · b · c" from the members of an About panel that are present, each a
   string or a {content} text. */
static void lite_channel_about_line(const YoutubeLiteSpan *about,
    const char *const *keys, unsigned count, char *output, size_t capacity)
{
    size_t used = 0;
    output[0] = '\0';
    for (unsigned i = 0; i < count; i++) {
        char text[YOUTUBE_LITE_METADATA_LIMIT] = {0};
        YoutubeLiteSpan value;
        if (!lite_member_string(about, keys[i], text, sizeof(text), true)
            && (!lite_json_member(about, keys[i], &value)
                || !lite_member_string(&value, "content", text, sizeof(text), true)))
            continue;
        if (text[0] == '\0') continue;
        int n = snprintf(output + used, capacity - used, "%s%s",
                         used ? " \xc2\xb7 " : "", text);
        if (n < 0 || (size_t) n >= capacity - used) { output[used] = '\0'; return; }
        used += (size_t) n;
    }
}

/* A channel link's destination: the site a YouTube redirect names, or a
   YouTube path. Only http(s) targets without spaces or controls. */
static bool lite_channel_link_target(const char *url, char output[256])
{
    char target[256];
    if (strncmp(url, "https://www.youtube.com/redirect?", 33) == 0) {
        if (!lite_query_value(url, "q", target, sizeof(target))) return false;
    } else if (url[0] == '/' && url[1] != '/') {
        int n = snprintf(target, sizeof(target), "https://www.youtube.com%s", url);
        if (n < 0 || (size_t) n >= sizeof(target)) return false;
    } else if (strlen(url) < sizeof(target)) {
        memcpy(target, url, strlen(url) + 1u);
    } else {
        return false;
    }
    if (strncmp(target, "https://", 8) != 0 && strncmp(target, "http://", 7) != 0)
        return false;
    for (const char *at = target; *at != '\0'; at++)
        if ((unsigned char) *at <= ' ' || *at == 0x7f) return false;
    memcpy(output, target, strlen(target) + 1u);
    return true;
}

/* The About panel response (the full-description view): the description,
   the counts and dates it states, and up to eight links. It names the
   channel only by its canonical URL. */
#if defined(__GNUC__)
__attribute__((noinline))
#endif
static void lite_channel_about_parse(YoutubeLiteBuildWork *work, size_t found)
{
    static const char *const stats[] = {"subscriberCountText", "videoCountText"};
    static const char *const facts[] = {"viewCountText", "joinedDateText", "country"};
    YoutubeLiteChannel *channel = &work->channel;
    YoutubeLiteSpan window = lite_build_window(work->decoded, work->decoded_length, found);
    YoutubeLiteSpan about, list, item, model, link, title;
    if (!lite_json_key(&window, "aboutChannelViewModel", NULL, &about)) return;
    (void) lite_member_string(&about, "description", channel->description,
                              sizeof(channel->description), true);
    lite_channel_trim(channel->description);
    char text[1024], id[YOUTUBE_CHANNEL_ID_CAPACITY] = {0};
    if (lite_member_string(&about, "channelId", id, sizeof(id), false)
        && lite_valid_account_id(id, true)) memcpy(work->channel_id, id, sizeof(id));
    const char *handle;
    if (lite_member_string(&about, "canonicalChannelUrl", text, sizeof(text), false)
        && (handle = strstr(text, "/@")) != NULL)
        snprintf(channel->name, sizeof(channel->name), "%s", handle + 1);
    lite_channel_about_line(&about, stats, 2u, channel->stats, sizeof(channel->stats));
    lite_channel_about_line(&about, facts, 3u, channel->facts, sizeof(channel->facts));
    if (!lite_json_member(&about, "links", &list)) return;
    const char *cursor = NULL;
    for (unsigned n = 0; n < 16u && channel->link_count < YOUTUBE_LITE_CHANNEL_LINKS
         && lite_array_next(&list, &cursor, &item); n++) {
        char url[256], name[64] = {0}, label[96] = {0};
        if (!lite_json_member(&item, "channelExternalLinkViewModel", &model)
            || !lite_json_member(&model, "link", &link)
            || !lite_json_key_string(&link, "url", text, sizeof(text))
            || !lite_channel_link_target(text, url)) continue;
        if (lite_json_member(&model, "title", &title))
            (void) lite_member_string(&title, "content", name, sizeof(name), true);
        (void) lite_member_string(&link, "content", label, sizeof(label), true);
        if (name[0] == '\0' && label[0] == '\0') continue;
        size_t room = sizeof(channel->links) - channel->link_bytes;
        int written = snprintf(channel->links + channel->link_bytes, room, "%s%c%s%c%s",
                               url, '\0', name, '\0', label);
        if (written < 0 || (size_t) written + 1u > room) break;
        channel->link_bytes += (unsigned) written + 1u;
        channel->link_count++;
    }
}

static void lite_build_channel_header_pump(YoutubeLiteBuildWork *work)
{
    /* The full-description view reads the About panel response; a page
       without that panel (the view's fallback) is read as a channel page. */
    YoutubeLiteChannel *channel = &work->channel;
    bool about = work->description_requested && !channel->about_scanned;
    size_t found = 0, offset = work->scan_offset;
    YoutubeLiteScanStatus status = work->decoded == NULL ? YOUTUBE_LITE_SCAN_EXHAUSTED
        : lite_bytes_scan_pump(work->decoded, work->decoded_length, work->decoded_length,
            about ? "\"aboutChannelViewModel\"" : "\"channelMetadataRenderer\"",
            &offset, &found);
    lite_build_seek(work, offset);
    if (status == YOUTUBE_LITE_SCAN_PENDING) return;
    if (status == YOUTUBE_LITE_SCAN_FOUND) {
        /* The name also appears as a string in the response's own lists. */
        const char *after = lite_json_space(work->decoded + offset,
                                            work->decoded + work->decoded_length);
        if (after == work->decoded + work->decoded_length || *after != ':') return;
        if (about) lite_channel_about_parse(work, found);
        else lite_channel_header_parse(work, found);
    } else if (about && work->decoded != NULL) {
        channel->about_scanned = true;
        lite_build_seek(work, 0);
        return;
    }
    lite_build_begin_emission(work);
}

/* A shelf's "See all" target: a playlist, or a tab of a channel. */
static void lite_channel_link(const YoutubeLiteSpan *scope, char output[160])
{
    char url[160], full[192], id[YOUTUBE_ACCOUNT_ID_CAPACITY];
    if (lite_find_bytes(scope->start, scope->end, "Panel") != NULL
        || !lite_json_key_string(scope, "url", url, sizeof(url)) || url[0] != '/')
        return;
    snprintf(full, sizeof(full), "https://www.youtube.com%s", url);
    YoutubeLiteRoute route = youtube_lite_route(full);
    if (route == YOUTUBE_LITE_ROUTE_PLAYLIST && lite_query_value(full, "list", id, sizeof(id))
        && lite_valid_account_id(id, false))
        snprintf(output, 160, "https://www.youtube.com/playlist?list=%s", id);
    else if (route == YOUTUBE_LITE_ROUTE_CHANNEL)
        snprintf(output, 160, "%.*s", (int) strcspn(full, "?#"), full);
}

static void lite_channel_shelf_begin(YoutubeLiteChannel *channel,
                                     const char *value, const char *end)
{
    channel->in_shelf = true;
    channel->shelf_items = 0;
    channel->shelf_title[0] = channel->shelf_url[0] = '\0';
    const char *title = lite_find_bytes(value, end - value > 512 ? value + 512 : end, "\"title\"");
    if (title == NULL) return;
    YoutubeLiteSpan scope = {title, end}, span, endpoint;
    if (!lite_json_key(&scope, "title", NULL, &span)) {
        /* A title too large for the window still names the shelf first. */
        (void) lite_json_key_string_truncated(&scope, "text",
            channel->shelf_title, sizeof(channel->shelf_title));
        return;
    }
    YoutubeLiteSpan named = {title, span.end};
    if (!lite_text_runs(&named, "title", channel->shelf_title, sizeof(channel->shelf_title)))
        (void) lite_json_key_string_truncated(&span, "content",
            channel->shelf_title, sizeof(channel->shelf_title));
    const char *next = lite_json_space(span.end, end);
    if (next < end && *next == ',') {
        YoutubeLiteSpan rest = {lite_json_space(next + 1, end), end};
        if (rest.end - rest.start > 10 && memcmp(rest.start, "\"endpoint\"", 10) == 0
            && lite_json_key(&rest, "endpoint", NULL, &endpoint)) span.end = endpoint.end;
    }
    lite_channel_link(&span, channel->shelf_url);
}

static bool lite_channel_close_shelf(YoutubeLiteBuildWork *work)
{
    YoutubeLiteChannel *channel = &work->channel;
    bool open = channel->shelf_open;
    channel->in_shelf = channel->shelf_open = false;
    /* Each "See all" names its shelf; one line, clipped to the width. */
    return !open || ((channel->shelf_url[0] == '\0'
            || (lite_html_text(&work->html, "<a class=\"more see-all\" href=\"")
                && lite_html_escape(&work->html, channel->shelf_url)
                && lite_html_ui_markup(&work->html, "\">{{See all}}")
                && (channel->shelf_title[0] == '\0'
                    || (lite_html_text(&work->html, ": ")
                        && lite_html_escape(&work->html, channel->shelf_title)))
                && lite_html_text(&work->html, "</a>")))
        && lite_html_text(&work->html, "</section>"));
}

static bool lite_parse_shorts(const YoutubeLiteSpan *renderer, YoutubeLiteVideo *video)
{
    YoutubeLiteSpan text;
    *video = (YoutubeLiteVideo) {0};
    if (!lite_json_key_string(renderer, "videoId", video->id, sizeof(video->id))
        || !lite_valid_video_id(video->id)) return false;
    if (lite_json_key(renderer, "primaryText", NULL, &text))
        (void) lite_member_string(&text, "content", video->title, sizeof(video->title), true);
    /* The mobile Short names itself only in its label: "Title, 51 thousand
       views - play Short". */
    char *cut;
    if (video->title[0] == '\0'
        && lite_member_string(renderer, "accessibilityText", video->title,
                              sizeof(video->title), true)
        && (cut = strrchr(video->title, ',')) != NULL && cut[1] == ' ') *cut = '\0';
    if (lite_json_key(renderer, "secondaryText", NULL, &text))
        (void) lite_member_string(&text, "content", video->views, sizeof(video->views), true);
    if (video->title[0] == '\0') snprintf(video->title, sizeof(video->title), "YouTube video");
    return true;
}

enum { LITE_CHANNEL_KEY_SHELVES = 3u, LITE_CHANNEL_KEY_CONTINUATION = 3u,
       LITE_CHANNEL_KEY_TITLED = 4u, LITE_CHANNEL_KEY_HEADLINED = 8u,
       LITE_CHANNEL_KEY_LOCKUP = 10u, LITE_CHANNEL_KEY_SHORTS = 11u };

/* One entry: a video, live or upcoming stream, Short, or playlist. */
#if defined(__GNUC__)
__attribute__((noinline))
#endif
static bool lite_channel_item(YoutubeLiteBuildWork *work, size_t kind,
                              const YoutubeLiteSpan *renderer)
{
    YoutubeLiteChannel *channel = &work->channel;
    YoutubeLiteVideo record;
    bool video = kind == LITE_CHANNEL_KEY_SHORTS ? lite_parse_shorts(renderer, &record)
        : kind < LITE_CHANNEL_KEY_LOCKUP ? lite_parse_video_renderer(renderer,
              kind < LITE_CHANNEL_KEY_HEADLINED ? YOUTUBE_LITE_RENDERER_COMPACT_VIDEO
                  : YOUTUBE_LITE_RENDERER_VIDEO_WITH_CONTEXT, &record)
        : kind == LITE_CHANNEL_KEY_LOCKUP && lite_parse_video_renderer(
              renderer, YOUTUBE_LITE_RENDERER_LOCKUP, &record);
    if (!video) {
        if (kind < LITE_CHANNEL_KEY_LOCKUP || kind == LITE_CHANNEL_KEY_SHORTS) return true;
        lite_parse_playlist_record(renderer, &record);
        if (record.playlist_id[0] == '\0') return true;
    }
    if (channel->in_shelf && !channel->shelf_open) {
        if (!lite_html_text(&work->html, "<section class=shelf>")
            || (channel->shelf_title[0] != '\0'
                && (!lite_html_text(&work->html, "<h2>")
                    || !lite_html_escape(&work->html, channel->shelf_title)
                    || !lite_html_text(&work->html, "</h2>")))) return false;
        channel->shelf_open = true;
        channel->shelves++;
    }
    channel->shelf_items++;
    work->video_count++;
    return video ? lite_html_video(&work->html, &record, false, NULL, SIZE_MAX)
        : lite_html_playlist_row(&work->html, &record);
}

/* One step of the entry walk: at most one renderer or one window advance. */
static bool lite_build_channel_items_pump(YoutubeLiteBuildWork *work)
{
    static const YoutubeLiteRendererKey keys[] = {
        LITE_KEY("shelfRenderer"), LITE_KEY("richShelfRenderer"),
        LITE_KEY("reelShelfRenderer"), LITE_KEY("continuationItemRenderer"),
        LITE_KEY("compactVideoRenderer"), LITE_KEY("videoRenderer"),
        LITE_KEY("gridVideoRenderer"), LITE_KEY("channelFeaturedVideoRenderer"),
        LITE_KEY("videoWithContextRenderer"), LITE_KEY("reelItemRenderer"),
        LITE_KEY("lockupViewModel"), LITE_KEY("shortsLockupViewModel"),
        LITE_KEY("compactPlaylistRenderer"), LITE_KEY("playlistRenderer"),
        LITE_KEY("gridPlaylistRenderer")
    };
    YoutubeLiteChannel *channel = &work->channel;
    if (work->description_requested) {
        /* The full-description view lists no entries and has no next page. */
        work->next_continuation[0] = '\0';
        work->phase = YOUTUBE_LITE_BUILD_EMIT_CONTINUATION;
        return true;
    }
    bool home = channel->tab == LITE_CHANNEL_HOME;
    const char *data = work->decoded;
    size_t length = work->metadata_start != 0 ? work->metadata_start : work->decoded_length;
    /* A tab past its entry bound still walks on for its next-page token. */
    bool room = work->video_count < (home ? LITE_CHANNEL_HOME_ITEMS : LITE_CHANNEL_TAB_ITEMS);
    if (data != NULL && work->scan_offset < length && (room || !home)) {
        YoutubeLiteSpan window = lite_build_window(data, length, work->scan_offset);
        bool tail = window.end == data + length;
        size_t kind = 0;
        const char *value = NULL;
        const char *key = lite_next_key(&window, window.start, keys,
            sizeof(keys) / sizeof(keys[0]), &kind, &value);
        if (key == NULL && !tail) {
            /* The whole window was searched; keep only a key's width. */
            lite_build_seek(work, (size_t) (window.end - data) - 64u);
            return true;
        }
        if (key != NULL && !tail
            && (size_t) (key - window.start) > YOUTUBE_LITE_BUILD_SCAN_BYTES) {
            lite_build_seek(work, (size_t) (key - data));
            return true;
        }
        if (key != NULL && kind < LITE_CHANNEL_KEY_SHELVES) {
            if (!lite_channel_close_shelf(work)) return false;
            if (!home || channel->shelves < LITE_CHANNEL_SHELVES) {
                lite_channel_shelf_begin(channel, value, window.end);
                lite_build_seek(work, (size_t) (value - data));
                return true;
            }
        } else if (key != NULL) {
            YoutubeLiteSpan renderer;
            bool ok = true;
            if (!lite_json_span(value, window.end, &renderer)) {
                lite_build_seek(work, (size_t) (key + 1 - data));
                return true;
            }
            if (kind == LITE_CHANNEL_KEY_CONTINUATION) {
                char token[YOUTUBE_LITE_CONTINUATION_LIMIT];
                if (work->next_continuation[0] == '\0'
                    && lite_next_continuation(key, (size_t) (renderer.end - key), token))
                    memcpy(work->next_continuation, token, sizeof(token));
            } else if (room && (!channel->in_shelf
                                || channel->shelf_items < LITE_CHANNEL_SHELF_ITEMS)) {
                ok = lite_channel_item(work, kind, &renderer);
            }
            lite_build_seek(work, (size_t) (renderer.end - data));
            return ok;
        }
    }
    work->phase = YOUTUBE_LITE_BUILD_EMIT_CONTINUATION;
    if (home) work->next_continuation[0] = '\0';
    return lite_channel_close_shelf(work)
        && (work->video_count != 0 || lite_html_ui_markup(&work->html,
            "<p class=empty>{{No videos found.}}</p>"));
}

/* The channel's base URL (its Home tab), escaped for an attribute. */
static bool lite_channel_home_href(YoutubeLiteHtml *html, const char *path, size_t base)
{
    return lite_html_text(html, "https://www.youtube.com")
        && lite_html_escape_bytes(html, path, base);
}

/* The header's description line: the start of the description on one
   line, clamped, linked to the full-description view. The view reads the
   About panel when the page header names its continuation (links, counts,
   joined date); otherwise it reads the page again for its description. */
static bool lite_channel_about_link(YoutubeLiteHtml *html,
    const YoutubeLiteChannel *channel, const char *path, size_t base)
{
    char line[YOUTUBE_LITE_SNIPPET_LIMIT];
    size_t length = tilefinch_utf8_complete_prefix(channel->description,
        strnlen(channel->description, sizeof(line) - 1u));
    memcpy(line, channel->description, length);
    line[length] = '\0';
    for (char *at = line; *at != '\0'; at++)
        if (*at == '\r' || *at == '\n' || *at == '\t') *at = ' ';
    lite_clamp_snippet(line, sizeof(line), YOUTUBE_LITE_SNIPPET_BUDGET);
    char token[YOUTUBE_LITE_ABOUT_TOKEN_LIMIT * 3u] = {0};
    if (channel->about_token[0] != '\0'
        && !lite_url_encode(channel->about_token, token, sizeof(token))) token[0] = '\0';
    return lite_html_ui_markup(html, "<a id=yt-channel-about class=channel-about title=\"{{View full description}}\" href=\"")
        && lite_channel_home_href(html, path, base)
        && lite_html_text(html, "?tilefinch_view=description")
        && (token[0] == '\0' || (lite_html_text(html, "&amp;tilefinch_token=")
            && lite_html_text(html, token)))
        && lite_html_text(html, "#description\">")
        && lite_html_escape(html, line) && lite_html_text(html, "</a>");
}

/* Banner, avatar, name, subscriber line, description, subscribe and tabs. */
static bool lite_build_channel_intro(YoutubeLiteBuildWork *work)
{
    static const char *const tabs[][2] = {
        {"{{Home}}", ""}, {"{{Videos}}", "/videos"},
        {"{{Shorts}}", "/shorts"}, {"{{Playlists}}", "/playlists"}
    };
    YoutubeLiteChannel *channel = &work->channel;
    YoutubeLiteHtml *html = &work->html;
    bool ok = lite_html_text(html, "<section class=channel-head>");
    if (ok && channel->banner[0] != '\0')
        ok = lite_html_text(html, "<img class=channel-banner src=\"")
            && lite_html_escape(html, channel->banner)
            && lite_html_text(html, "\" alt=\"\">");
    ok = ok && lite_html_text(html, "<div class=channel-id>");
    if (ok && channel->avatar[0] != '\0')
        ok = lite_html_text(html, "<img class=channel-avatar src=\"")
            && lite_html_escape(html, channel->avatar)
            && lite_html_text(html, "\" width=48 height=48 alt=\"\">");
    ok = ok && lite_html_text(html, "<div class=channel-names>");
    if (ok && channel->name[0] != '\0')
        ok = lite_html_text(html, "<h1>") && lite_html_escape(html, channel->name)
            && lite_html_text(html, "</h1>");
    if (ok && channel->stats[0] != '\0')
        ok = lite_html_text(html, "<p class=hint>") && lite_html_escape(html, channel->stats)
            && lite_html_text(html, "</p>");
    /* Signed in, an empty reserved line under the subscriber line, as on
       the watch page, which an account action's result is painted into. */
    if (ok && work->signed_in)
        ok = lite_html_text(html, "<p id=yt-library-status class=watch-status>&nbsp;</p>");
    ok = ok && lite_html_text(html, "</div>");
    /* Signed out there is no subscribe control at all. Signed in it is the
       watch page's bell, beside the name. */
    if (ok && work->signed_in && lite_valid_account_id(work->channel_id, true))
        ok = lite_html_text(html, "<div class=channel-actions>")
            && lite_html_channel_subscribe(html, work->channel_id, channel->subscribed)
            && lite_html_text(html, "</div>");
    ok = ok && lite_html_text(html, "</div>");
    size_t base = 0;
    unsigned tab = 0;
    const char *path = lite_channel_path(work->url, &base, &tab);
    if (work->description_requested) tab = LITE_CHANNEL_OTHER;
    if (ok && path != NULL && channel->description[0] != '\0' && !work->description_requested)
        ok = lite_channel_about_link(html, channel, path, base);
    if (ok && path != NULL) {
        ok = lite_html_text(html, "<nav class=channel-tabs>");
        for (unsigned i = 0; ok && i < 4u; i++)
            ok = lite_html_text(html, i == tab
                    ? "<a class=\"comments-link on\" aria-current=page href=\"https://www.youtube.com"
                    : "<a class=comments-link href=\"https://www.youtube.com")
                && lite_html_escape_bytes(html, path, base)
                && lite_html_text(html, tabs[i][1])
                && lite_html_text(html, "\">")
                && lite_html_ui_markup(html, tabs[i][0])
                && lite_html_text(html, "</a> ");
        ok = ok && lite_html_text(html, "</nav>");
    }
    return ok && lite_html_text(html, "</section>");
}

/* The full-description view: the whole description, the channel's links
   and the facts its About panel states, then the way back. */
static bool lite_build_channel_description(YoutubeLiteBuildWork *work)
{
    if (!work->description_requested) return true;
    const YoutubeLiteChannel *channel = &work->channel;
    YoutubeLiteHtml *html = &work->html;
    size_t base = 0;
    unsigned tab = 0;
    const char *path = lite_channel_path(work->url, &base, &tab);
    bool ok = channel->description[0] == '\0'
        || (lite_html_ui_markup(html,
                "<h2>{{Description}}</h2><div id=description class=description>")
            && lite_html_multiline(html, channel->description)
            && lite_html_text(html, "</div>"));
    if (ok && channel->link_count != 0)
        ok = lite_html_ui_markup(html, "<h2>{{Links}}</h2><ul class=channel-links>");
    const char *link = channel->links;
    for (unsigned i = 0; ok && i < channel->link_count; i++) {
        const char *name = link + strlen(link) + 1u;
        const char *label = name + strlen(name) + 1u;
        ok = lite_html_text(html, "<li><a href=\"") && lite_html_escape(html, link)
            && lite_html_text(html, "\">")
            && lite_html_escape(html, name[0] != '\0' ? name : label)
            && lite_html_text(html, "</a>");
        if (ok && name[0] != '\0' && label[0] != '\0' && strcmp(name, label) != 0)
            ok = lite_html_text(html, " <span class=hint>")
                && lite_html_escape(html, label) && lite_html_text(html, "</span>");
        ok = ok && lite_html_text(html, "</li>");
        link = label + strlen(label) + 1u;
    }
    if (ok && channel->link_count != 0) ok = lite_html_text(html, "</ul>");
    if (ok && channel->facts[0] != '\0')
        ok = lite_html_text(html, "<p class=hint>") && lite_html_escape(html, channel->facts)
            && lite_html_text(html, "</p>");
    if (ok && path != NULL)
        ok = lite_html_text(html, "<p><a class=comments-link href=\"")
            && lite_channel_home_href(html, path, base)
            && lite_html_ui_markup(html, "\">{{Back to channel}}</a></p>");
    return ok;
}
