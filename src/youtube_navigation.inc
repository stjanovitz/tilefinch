/* Bounded, script-free search and chapter controls owned by the provider. */
typedef struct { unsigned date, duration, sort; } YoutubeLiteFilters;

static YoutubeLiteFilters lite_search_filters(const char *url)
{
    YoutubeLiteFilters filters = {0};
    unsigned *values[] = {&filters.date, &filters.duration, &filters.sort};
    static const char *const names[] = {"tilefinch_date", "tilefinch_duration", "tilefinch_sort"};
    static const unsigned limits[] = {4, 3, 1};
    for (unsigned i = 0; i < 3; i++) {
        char value[4] = {0};
        if (lite_query_value(url, names[i], value, sizeof(value))
            && value[0] >= '0' && (unsigned) (value[0] - '0') <= limits[i] && value[1] == '\0')
            *values[i] = (unsigned) (value[0] - '0');
    }
    return filters;
}

static bool lite_filter_suffix(const char *url, char suffix[96])
{
    YoutubeLiteFilters f = lite_search_filters(url);
    suffix[0] = '\0';
    if (!f.date && !f.duration && !f.sort) return true;
    int n = snprintf(suffix, 96, "&tilefinch_date=%u&tilefinch_duration=%u&tilefinch_sort=%u",
        f.date, f.duration, f.sort);
    return n >= 0 && n < 96;
}

/* YouTube's public filter declarations encode sort in field 1 and a nested
   filter message in field 2. Current duration values are 4/5/2, not 1/3/2.
   Never pass an arbitrary author-supplied service parameter through. */
static bool lite_search_params(const char *url, char output[48])
{
    YoutubeLiteFilters f = lite_search_filters(url);
    unsigned char bytes[8];
    size_t n = 0, used = 0;
    if (f.sort) { bytes[n++] = 8; bytes[n++] = 3; }
    if (f.date || f.duration) {
        static const unsigned char durations[] = {0, 4, 5, 2};
        bytes[n++] = 18; bytes[n++] = (f.date != 0) * 2 + (f.duration != 0) * 2;
        if (f.date) { bytes[n++] = 8; bytes[n++] = f.date + 1; }
        if (f.duration) { bytes[n++] = 24; bytes[n++] = durations[f.duration]; }
    }
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char encoded[16];
    for (size_t i = 0; i < n; i += 3) {
        unsigned value = (unsigned) bytes[i] << 16;
        if (i + 1 < n) value |= (unsigned) bytes[i + 1] << 8;
        if (i + 2 < n) value |= bytes[i + 2];
        encoded[used++] = alphabet[value >> 18];
        encoded[used++] = alphabet[(value >> 12) & 63];
        encoded[used++] = i + 1 < n ? alphabet[(value >> 6) & 63] : '=';
        encoded[used++] = i + 2 < n ? alphabet[value & 63] : '=';
    }
    encoded[used] = '\0';
    return lite_url_encode(encoded, output, 48);
}

static bool lite_html_filter_select(YoutubeLiteHtml *html, const char *name,
    const char *label, const char *const *options, unsigned count, unsigned selected)
{
    /* Spatial navigation intentionally admits clickable HTML labels. These
       compact captions are display-only; put their accessible name directly
       on the select so the d-pad has one stop per field. */
    bool ok = lite_html_text(html, "<div class=filter-field><span class=filter-label>")
        && lite_html_escape(html, tilefinch_ui_logical_text(label))
        && lite_html_format(html, "</span><select id=%s name=%s aria-label=\"", name, name)
        && lite_html_escape(html, tilefinch_ui_logical_text(label))
        && lite_html_text(html, "\">");
    for (unsigned i = 0; ok && i < count; i++)
        ok = lite_html_format(html, "<option value=%u%s>", i, i == selected ? " selected" : "")
            && lite_html_escape(html, tilefinch_ui_logical_text(options[i]))
            && lite_html_text(html, "</option>");
    return ok && lite_html_text(html, "</select></div>");
}

static bool lite_html_search_filters(YoutubeLiteHtml *html, const char *url, const char *query)
{
    static const char *const dates[] = {"Any time", "Today", "This week", "This month", "This year"};
    static const char *const durations[] = {"Any duration", "Under 3 minutes", "3-20 minutes", "Over 20 minutes"};
    static const char *const sorts[] = {"Relevance", "Popularity"};
    YoutubeLiteFilters f = lite_search_filters(url);
    return lite_html_ui_markup(html,
        "<details id=yt-filter-panel class=search-filters><summary>{{Filters}}</summary>"
        "<form action='https://www.youtube.com/results' method=get>"
        "<input type=hidden name=search_query value=\"")
        && lite_html_escape(html, query) && lite_html_text(html, "\"><div class=filter-fields>")
        && lite_html_filter_select(html, "tilefinch_date", "Upload date", dates, 5, f.date)
        && lite_html_filter_select(html, "tilefinch_duration", "Duration", durations, 4, f.duration)
        && lite_html_filter_select(html, "tilefinch_sort", "Sort by", sorts, 2, f.sort)
        && lite_html_ui_markup(html, "</div><button id=yt-filter-apply type=submit>{{Apply filters}}</button></form></details>");
}

static bool lite_chapter_number(const YoutubeLiteSpan *value, unsigned *number)
{
    const char *at = value->start;
    const char *end = value->end;
    if (at < end && *at == '"') { at++; end--; }
    if (at == end || end - at > 9) return false;
    unsigned n = 0;
    for (; at < end; at++) {
        if (*at < '0' || *at > '9' || n > (604800000u - (unsigned) (*at - '0')) / 10u)
            return false;
        n = n * 10 + (unsigned) (*at - '0');
    }
    *number = n;
    return true;
}

/* Returns the end of one declared chapter object, never an old visibility
   answer or an arbitrary service URL. All links bind to this video's ID. */
static const char *lite_chapter_next(YoutubeLiteWatch *watch, const YoutubeLiteSpan *scope)
{
    YoutubeLiteSpan object, other, value;
    bool found = lite_json_key(scope, "chapterRenderer", NULL, &object);
    if (lite_json_key(scope, "macroMarkersListItemRenderer", NULL, &other)
        && (!found || other.start < object.start)) { object = other; found = true; }
    if (!found) return NULL;
    unsigned seconds = 0;
    char title[80] = {0};
    bool valid = lite_json_member(&object, "timeRangeStartMillis", &value)
        && lite_chapter_number(&value, &seconds);
    if (valid) seconds /= 1000u;
    else {
        YoutubeLiteSpan endpoint;
        char id[YOUTUBE_VIDEO_ID_CAPACITY] = {0};
        valid = lite_json_key(&object, "watchEndpoint", NULL, &endpoint)
            && lite_member_string(&endpoint, "videoId", id, sizeof(id), false)
            && strcmp(id, watch->video.id) == 0
            && lite_json_member(&endpoint, "startTimeSeconds", &value)
            && lite_chapter_number(&value, &seconds);
    }
    valid = valid && seconds <= 604800u
        && (!watch->duration_seconds || seconds < watch->duration_seconds)
        && lite_text_runs(&object, "title", title, sizeof(title)) && title[0];
    if (valid && watch->chapter_count < YOUTUBE_LITE_MAXIMUM_CHAPTERS
        && (!watch->chapter_count || seconds > watch->chapters[watch->chapter_count - 1].seconds)) {
        YoutubeLiteChapter *chapter = &watch->chapters[watch->chapter_count++];
        chapter->seconds = seconds;
        memcpy(chapter->title, title, sizeof(title));
    }
    return object.end;
}

static bool lite_html_chapters(YoutubeLiteHtml *html, const YoutubeLiteWatch *watch)
{
    if (!watch->chapter_count) return true;
    bool ok = lite_html_ui_markup(html,
        "<details id=yt-chapters class=chapters><summary>{{Chapters}}</summary>");
    for (unsigned i = 0; ok && i < watch->chapter_count; i++) {
        const YoutubeLiteChapter *chapter = &watch->chapters[i];
        ok = lite_html_format(html,
            "<a class=chapter data-tilefinch-provider-media href=\"https://www.youtube.com/watch?v=%s&amp;t=%u\">%u:%02u &mdash; ",
            watch->video.id, chapter->seconds, chapter->seconds / 60u, chapter->seconds % 60u)
            && lite_html_escape(html, chapter->title) && lite_html_text(html, "</a>");
    }
    return ok && lite_html_text(html, "</details>");
}

static void lite_chapters_parse(YoutubeLiteWatch *watch, const char *data, size_t length)
{
    if (data == NULL) return;
    size_t offset = 0;
    for (unsigned steps = 0; offset < length && steps < 4096
        && watch->chapter_count < YOUTUBE_LITE_MAXIMUM_CHAPTERS; steps++) {
        size_t window = length - offset;
        if (window > 48u * 1024u) window = 48u * 1024u;
        YoutubeLiteSpan scope = {data + offset, data + offset + window};
        const char *after = lite_chapter_next(watch, &scope);
        offset = after ? (size_t) (after - data)
            : window == length - offset ? length : offset + 16u * 1024u;
    }
}
