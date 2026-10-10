/* Personalized responses never enter the persistent or document caches.
   One bootstrap, one feed and an optional recent-history request, with
   bounded renderer walks; all RAM-owned. */
enum { LITE_FEED_BYTES = 512u * 1024u };
struct YoutubeLiteFeedJob {
    Budget *budget;
    BrowserSession *session;
    FetchScheduler *scheduler;
    uint64_t request;
    uint64_t began_us;
    long timeout_ms;
    unsigned phase; /* 0 bootstrap, 1 identity, 2 API, 3 build, 4 JSON framing,
                       5 history fetch, 6 history framing, 7 first history video */
    size_t cursor;
    bool subscriptions;
    bool history_attempted;
    bool history_request;
    char like_video_id[YOUTUBE_VIDEO_ID_CAPACITY];
    bool like_selected;
    bool compact;
    bool logged_in;
    unsigned auth_user;
    char client_name[16];
    YoutubeLiteLoadStatus status;
    YoutubeLiteIdentity identity;
    FetchResult response;
    YoutubeLiteBuildWork *build;
    YoutubeLiteDocument document;
    char target[512];
    char json_stack[48];
    unsigned json_depth;
    bool json_string;
    bool json_escape;
    bool json_started;
    bool json_done;
    bool json_error;
    size_t context_begin;
    size_t context_end;
    size_t history_contents_begin;
    size_t history_contents_end;
#ifndef __PSP__
    YoutubeLiteRatingDiagnostics rating_diagnostics;
    unsigned history_metadata;
#endif
};

#ifndef __PSP__
unsigned youtube_lite_feed_history_metadata(const YoutubeLiteFeedJob *job)
{
    return job == NULL ? 0u : job->history_metadata;
}

bool youtube_lite_rating_diagnostics(const YoutubeLiteFeedJob *job,
                                    YoutubeLiteRatingDiagnostics *result)
{
    if (job == NULL || result == NULL) return false;
    *result = job->rating_diagnostics;
    result->phase = job->phase;
    return true;
}

static unsigned lite_rating_response_flags(const YoutubeLiteFeedJob *job)
{
    unsigned flags = job->json_done && !job->json_error ? 1u : 0u;
    YoutubeLiteSpan root = {job->response.data, job->response.data + job->response.length}, value;
    static const char *const names[] = {"responseContext", "actions", "error",
        "updateToggleButtonStateCommand", "onResponseReceivedActions", "toastRenderer", "likeStatus", "status",
        "frameworkUpdates", "entityBatchUpdate", "likeStatusEntity"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (lite_json_key(&root, names[i], NULL, &value)) flags |= 2u << i;
    if (lite_json_member(&root, "likeStatus", &value)) flags |= 4096u;
    char status[16];
    if (lite_json_key_string(&root, "likeStatus", status, sizeof(status))) {
        if (strcmp(status, "LIKE") == 0) flags |= 8192u;
        else if (strcmp(status, "INDIFFERENT") == 0) flags |= 16384u;
        else if (strcmp(status, "DISLIKE") == 0) flags |= 32768u;
    }
    return flags;
}
#endif

uint64_t youtube_lite_session_login_identity(BrowserSession *session)
{
    const char *origin = "https://m.youtube.com/";
    TilefinchRequestContext context = lite_supplemental_context(origin, origin);
    context.initiator_url = origin;
    char cookies[4096] = {0};
    const char *value = NULL;
    if (browser_session_cookie_header_context(session, &context, cookies, sizeof(cookies))) {
        const char *names[] = {"SAPISID=", "__Secure-3PAPISID="};
        for (unsigned i = 0; value == NULL && i < 2u; i++) {
            const char *at = cookies;
            for (unsigned j = 0; *at != '\0' && j < BROWSER_COOKIE_ENTRIES; j++) {
                if (strncmp(at, names[i], strlen(names[i])) == 0) {
                    value = at + strlen(names[i]); break;
                }
                at = strchr(at, ';');
                if (at == NULL) break;
                at++; if (*at == ' ') at++;
            }
        }
    }
    uint64_t identity = 0;
    if (value != NULL && *value != '\0' && *value != ';') {
        identity = UINT64_C(14695981039346656037);
        for (size_t i = 0; value[i] != '\0' && value[i] != ';' && i < sizeof(cookies); i++)
            identity = (identity ^ (unsigned char) value[i]) * UINT64_C(1099511628211);
        if (identity == 0) identity = 1;
    }
    lite_probe_wipe(cookies, sizeof(cookies));
    return identity; /* RAM-only change detection, never an authentication proof. */
}

bool youtube_lite_session_signed_in(BrowserSession *session)
{
    return youtube_lite_session_login_identity(session) != 0;
}

static bool lite_feed_enqueue(YoutubeLiteFeedJob *job, bool api)
{
    const char *origin = "https://m.youtube.com/";
    char endpoint[512] = "https://m.youtube.com/";
    char body[4096] = {0}, visitor[2048] = {0}, headers[512] = {0};
    char authorization[96] = {0};
    TilefinchRequestContext context = lite_primary_context(origin, origin);
    context.user_activated = false;
    FetchRequest request = {.method = "GET", .accept = "text/html",
        .user_agent = YOUTUBE_LITE_MOBILE_UA, .allow_http_errors = true,
        .redirect_same_origin_only = true};
    bool okay = true;
    if (api) {
        int count = snprintf(endpoint, sizeof(endpoint),
            "https://m.youtube.com/youtubei/v1/%s?key=%s&prettyPrint=false",
            job->like_video_id[0] != '\0'
                ? (job->like_selected ? "like/like" : "like/removelike") : "browse",
            job->identity.api_key);
        okay = count > 0 && (size_t) count < sizeof(endpoint)
            && lite_identity_valid(&job->identity) && strcmp(job->client_name, "MWEB") == 0
            && lite_json_quote(job->identity.visitor, visitor, sizeof(visitor));
        count = snprintf(body, sizeof(body),
            "{\"context\":{\"client\":{\"clientName\":\"MWEB\","
            "\"clientVersion\":\"%s\",\"hl\":\"%s\",\"gl\":\"US\","
            "\"visitorData\":%s}},%s\"%s\"%s}",
            job->identity.client_version, lite_preferred_language(),
            okay ? visitor : "\"\"",
            job->like_video_id[0] != '\0' ? "\"target\":{\"videoId\":" : "\"browseId\":",
            job->like_video_id[0] != '\0' ? job->like_video_id
                : job->history_request ? "FEhistory"
                : job->subscriptions ? "FEsubscriptions" : "FEwhat_to_watch",
            job->like_video_id[0] != '\0' ? "}" : "");
        okay = okay && count > 0 && (size_t) count < sizeof(body);
        size_t body_length = okay ? (size_t) count : 0u;
        context = lite_supplemental_context(origin, endpoint);
        context.initiator_url = origin;
        okay = okay && job->logged_in && lite_feed_authorization(job->session,
            &context, tilefinch_platform_wall_time_ns() / UINT64_C(1000000000),
            authorization, sizeof(authorization));
        count = snprintf(headers, sizeof(headers),
            "X-YouTube-Client-Name: 2\nX-YouTube-Client-Version: %s"
            "\nAuthorization: %s\nX-Origin: https://m.youtube.com"
            "\nX-Goog-AuthUser: %u\nX-Youtube-Bootstrap-Logged-In: true",
            job->identity.client_version, authorization, job->auth_user);
        okay = okay && count > 0 && (size_t) count < sizeof(headers);
        request = (FetchRequest) {.method = "POST", .body = body,
            .body_length = body_length, .content_type = "application/json",
            .accept = "application/json", .extra_headers = headers,
            .user_agent = YOUTUBE_LITE_MOBILE_UA, .allow_http_errors = true,
            .redirect_same_origin_only = true};
    }
    FetchPreparedPageRequest prepared;
    uint64_t now = tilefinch_platform_monotonic_time_us();
    uint64_t elapsed = now >= job->began_us ? (now - job->began_us) / 1000u : 0;
    okay = okay && elapsed < (uint64_t) job->timeout_ms
        && fetch_prepare_page_request_context(&context, origin,
            "strict-origin-when-cross-origin", job->session, NULL, NULL,
            &request, &prepared, NULL);
    if (okay) {
        snprintf(job->target, sizeof(job->target), "%s", endpoint);
        job->request = fetch_scheduler_enqueue(job->scheduler, endpoint,
            fetch_prepared_page_request(&prepared), api
                ? (job->like_video_id[0] != '\0' ? 64u * 1024u : LITE_FEED_BYTES)
                : YOUTUBE_LITE_MAXIMUM_SOURCE_BYTES,
            job->history_request && job->timeout_ms - (long) elapsed > 3000L
                ? 3000L : job->timeout_ms - (long) elapsed);
    }
    lite_probe_wipe(&prepared, sizeof(prepared));
    lite_probe_wipe(body, sizeof(body)); lite_probe_wipe(visitor, sizeof(visitor));
    lite_probe_wipe(headers, sizeof(headers)); lite_probe_wipe(authorization, sizeof(authorization));
    lite_probe_wipe(endpoint, sizeof(endpoint));
    return okay && job->request != 0;
}

YoutubeLiteFeedJob *youtube_lite_feed_begin(Budget *budget,
    BrowserSession *session, bool subscriptions, bool compact, long timeout_ms)
{
    if (budget == NULL || session == NULL || timeout_ms <= 0) return NULL;
    YoutubeLiteFeedJob *job = budget_calloc_category(budget,
        BUDGET_CATEGORY_RESOURCE, 1, sizeof(*job));
    if (job == NULL) return NULL;
    job->budget = budget; job->session = session;
    job->subscriptions = subscriptions; job->compact = compact;
    job->timeout_ms = timeout_ms;
    job->began_us = tilefinch_platform_monotonic_time_us();
    job->identity.version = YOUTUBE_LITE_IDENTITY_CACHE_VERSION;
    job->scheduler = fetch_scheduler_create_ex(budget, 1,
        YOUTUBE_LITE_MAXIMUM_SOURCE_BYTES, NULL, 0);
    if (job->scheduler != NULL)
        (void) fetch_scheduler_enable_background_transport(job->scheduler, true);
    if (job->scheduler == NULL || !lite_feed_enqueue(job, false)) {
        youtube_lite_feed_destroy(job);
        return NULL;
    }
    return job;
}

YoutubeLiteFeedJob *youtube_lite_rating_begin(Budget *budget,
    BrowserSession *session, const char *video_id, bool liked, long timeout_ms)
{
    if (budget == NULL || session == NULL || video_id == NULL || !lite_valid_video_id(video_id)
        || !youtube_lite_session_signed_in(session)) return NULL;
    YoutubeLiteFeedJob *job = youtube_lite_feed_begin(budget, session, false, false, timeout_ms);
    if (job != NULL) {
        snprintf(job->like_video_id, sizeof(job->like_video_id), "%s", video_id);
        job->like_selected = liked;
    }
    return job;
}

/* Entity keys encode the video ID as protobuf field 2. Decode a bounded
   base64/base64url key (including URL-escaped padding), never a substring
   search: an update for another video must not confirm this request. */
static bool lite_rating_entity_matches(const char *key, const char *video_id)
{
    unsigned char decoded[96];
    size_t count = 0, symbols = 0, padding = 0, length = strlen(key);
    unsigned bits = 0, accumulator = 0;
    for (size_t at = 0; at < length; at++) {
        unsigned char byte = (unsigned char) key[at];
        if (byte == '%') {
            unsigned high, low;
            if (length - at < 3u || !lite_hex(key[at + 1u], &high)
                || !lite_hex(key[at + 2u], &low)) return false;
            byte = (unsigned char) ((high << 4u) | low);
            at += 2u;
        }
        if (byte == '=') {
            if (++padding > 2u) return false;
            continue;
        }
        unsigned value;
        if (padding != 0) return false;
        if (byte >= 'A' && byte <= 'Z') value = byte - 'A';
        else if (byte >= 'a' && byte <= 'z') value = byte - 'a' + 26u;
        else if (byte >= '0' && byte <= '9') value = byte - '0' + 52u;
        else if (byte == '+' || byte == '-') value = 62u;
        else if (byte == '/' || byte == '_') value = 63u;
        else return false;
        symbols++;
        accumulator = (accumulator << 6u) | value;
        bits += 6u;
        if (bits >= 8u) {
            bits -= 8u;
            if (count == sizeof(decoded)) return false;
            decoded[count++] = (unsigned char) (accumulator >> bits);
            accumulator &= (1u << bits) - 1u;
        }
    }
    if (bits == 6u || accumulator != 0u
        || (padding != 0u && ((symbols + padding) % 4u != 0u
            || padding != (bits == 4u ? 2u : bits == 2u ? 1u : 0u)))) return false;
    return count >= 13u && decoded[0] == 0x12u && decoded[1] == 11u
        && strlen(video_id) == 11u && memcmp(decoded + 2u, video_id, 11u) == 0;
}

static bool lite_like_confirmed(const char *data, size_t length,
                                const char *video_id, bool liked)
{
    YoutubeLiteSpan root = {data, data + length}, actions, action, state, value;
    bool confirmed = false;
    /* The rating endpoint returns this state directly on newer responses.
       Only a root member describes this request; nested renderer metadata
       can describe another video. An explicit mismatch must not fall through
       to an older command that happens to match. */
    if (lite_json_member(&root, "likeStatus", &value)) {
        char status[16];
        if (!lite_member_string(&root, "likeStatus", status, sizeof(status), false)
            || strcmp(status, liked ? "LIKE" : "INDIFFERENT") != 0)
            return false;
        confirmed = true;
    }
    YoutubeLiteSpan framework, batch, mutations, mutation, payload;
    if (lite_json_member(&root, "frameworkUpdates", &framework)
        && lite_json_member(&framework, "entityBatchUpdate", &batch)
        && lite_json_member(&batch, "mutations", &mutations)) {
        const char *at = NULL;
        for (unsigned i = 0; i < 64u && lite_array_next(&mutations, &at, &mutation); i++) {
            char key[128], status[16], mutation_key[128], type[40];
            if (!lite_json_member(&mutation, "payload", &payload)
                || !lite_json_member(&payload, "likeStatusEntity", &state)
                || !lite_member_string(&state, "key", key, sizeof(key), false)
                || !lite_rating_entity_matches(key, video_id)) continue;
            if ((lite_json_member(&mutation, "entityKey", &value)
                    && (!lite_member_string(&mutation, "entityKey", mutation_key,
                            sizeof(mutation_key), false) || strcmp(key, mutation_key) != 0))
                || (lite_json_member(&mutation, "type", &value)
                    && (!lite_member_string(&mutation, "type", type, sizeof(type), false)
                        || strcmp(type, "ENTITY_MUTATION_TYPE_REPLACE") != 0))
                || !lite_member_string(&state, "likeStatus", status, sizeof(status), false)
                || strcmp(status, liked ? "LIKE" : "INDIFFERENT") != 0)
                return false;
            confirmed = true;
        }
    }
    if (!lite_json_member(&root, "actions", &actions)) return confirmed;
    const char *cursor = NULL;
    for (unsigned i = 0; i < 64u && lite_array_next(&actions, &cursor, &action); i++) {
        char button[32];
        if (lite_json_member(&action, "updateToggleButtonStateCommand", &state)
            && lite_member_string(&state, "buttonId", button, sizeof(button), false)
            && strcmp(button, "like-button") == 0
            && lite_json_member(&state, "toggled", &value)) {
            if (value.end - value.start != (liked ? 4 : 5)
                || memcmp(value.start, liked ? "true" : "false", liked ? 4u : 5u) != 0)
                return false;
            confirmed = true;
        }
    }
    return confirmed;
}

static bool lite_feed_authenticated(const YoutubeLiteSpan *context)
{
    YoutubeLiteSpan services, service, params, param, flag;
    if (lite_json_member(context, "mainAppWebResponseContext", &flag)
        && lite_json_member(&flag, "loggedOut", &services))
        return services.end - services.start == 5
            && memcmp(services.start, "false", 5) == 0;
    if (!lite_json_member(context, "serviceTrackingParams", &services)) return false;
    const char *cursor = NULL;
    for (unsigned i = 0; i < 64u && lite_array_next(&services, &cursor, &service); i++) {
        if (!lite_json_member(&service, "params", &params)) continue;
        const char *at = NULL;
        for (unsigned j = 0; j < 64u && lite_array_next(&params, &at, &param); j++) {
            char key[32], value[8];
            if (lite_member_string(&param, "key", key, sizeof(key), false)
                && strcmp(key, "logged_in") == 0
                && lite_member_string(&param, "value", value, sizeof(value), false))
                return strcmp(value, "1") == 0;
        }
    }
    return false;
}

static bool lite_feed_json_pump(YoutubeLiteFeedJob *job)
{
    const char *data = job->response.data;
    size_t stop = job->response.length - job->cursor;
    if (stop > 16u * 1024u) stop = 16u * 1024u;
    stop += job->cursor;
    for (size_t at = job->cursor; at < stop; at++) {
        char byte = data[at];
        if (!job->json_started || job->json_done) {
            if (isspace((unsigned char) byte)) continue;
            if (job->json_done || byte != '{') return false;
            job->json_started = true;
            job->json_stack[job->json_depth++] = '}';
            continue;
        }
        if (job->json_string) {
            if (job->json_escape) job->json_escape = false;
            else if (byte == '\\') job->json_escape = true;
            else if (byte == '"') job->json_string = false;
            else if ((unsigned char) byte < 0x20u) return false;
            continue;
        }
        if (byte == '"') {
            /* Identify only literal root keys, never nested or string data.
               Lookahead is at most the fixed key plus whitespace (64 bytes). */
            if (job->json_depth == 1u) {
                static const char *const keys[] = {"\"responseContext\"", "\"error\"", "\"contents\""};
                for (unsigned k = 0; k < (job->history_request ? 3u : 2u); k++) {
                    size_t length = strlen(keys[k]);
                    if (length > job->response.length - at
                        || memcmp(data + at, keys[k], length) != 0) continue;
                    size_t value = at + length;
                    size_t bound = job->response.length - value;
                    if (bound > 64u) bound = 64u;
                    bound += value;
                    while (value < bound && isspace((unsigned char) data[value])) value++;
                    if (value == bound || data[value++] != ':') continue;
                    while (value < bound && isspace((unsigned char) data[value])) value++;
                    if (value == bound) continue;
                    if (k == 1u) job->json_error = true;
                    else if (k == 0u && data[value] == '{' && job->context_begin == 0u)
                        job->context_begin = value;
                    else if (k == 2u && (data[value] == '{' || data[value] == '[')
                        && job->history_contents_begin == 0u) job->history_contents_begin = value;
                }
            }
            job->json_string = true;
        } else if (byte == '{' || byte == '[') {
            if (job->json_depth >= sizeof(job->json_stack)) return false;
            job->json_stack[job->json_depth++] = byte == '{' ? '}' : ']';
        } else if (byte == '}' || byte == ']') {
            if (job->json_depth == 0u || job->json_stack[job->json_depth - 1u] != byte)
                return false;
            job->json_depth--;
            if (job->json_depth == 1u && job->context_begin != 0u
                && job->context_end == 0u && at > job->context_begin)
                job->context_end = at + 1u;
            if (job->json_depth == 1u && job->history_contents_begin != 0u
                && job->history_contents_end == 0u && at > job->history_contents_begin)
                job->history_contents_end = at + 1u;
            if (job->json_depth == 0u) job->json_done = true;
        }
    }
    job->cursor = stop;
    return true;
}

/* Recommendations are already parsed into owned fields before history is
   requested. Drop their raw response rather than retaining two large bodies.
   History is optional and never makes a usable feed fail. */
static void lite_feed_history_finish(YoutubeLiteFeedJob *job)
{
    lite_probe_release(&job->response);
    job->history_request = false;
    job->phase = 3u;
}

static void lite_feed_history_video_pump(YoutubeLiteFeedJob *job)
{
    if (job->cursor >= job->history_contents_end) {
        lite_feed_history_finish(job);
        return;
    }
    YoutubeLiteSpan scope = lite_build_window(job->response.data, job->history_contents_end, job->cursor);
    YoutubeLiteSpan renderer;
    size_t kind = 0;
    if (lite_next_video_renderer(&scope, scope.start, false, &renderer, &kind)) {
        YoutubeLiteVideo recent;
        if (lite_parse_video_renderer(&renderer, kind, &recent)) {
#ifndef __PSP__
            /* Fixed feature presence only: no history IDs, titles, times or
               response bytes cross the host diagnostic boundary. */
            static const char *const names[] = {"startTimeSeconds", "percentDurationWatched",
                "thumbnailOverlayResumePlaybackProgressRenderer", "thumbnailOverlayProgressBarViewModel",
                "thumbnailBottomOverlayViewModel", "progressBar", "percentWatched",
                "progressBarStartPosition", "resumePlaybackEndpoint"};
            YoutubeLiteSpan value;
            job->history_metadata = 1u;
            for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
                if (lite_json_key(&renderer, names[i], NULL, &value))
                    job->history_metadata |= 2u << i;
            if (lite_json_member(&renderer, "percentDurationWatched", &value)) {
                job->history_metadata |= 65536u;
            }
            if (lite_json_key(&renderer, "percentDurationWatched", NULL, &value)) {
                if (value.start < value.end && *value.start == '"') job->history_metadata |= 131072u;
                if (value.start < value.end && *value.start >= '0' && *value.start <= '9')
                    job->history_metadata |= 262144u;
            }
            static const char *const containers[] = {"thumbnail", "thumbnailOverlays",
                "navigationEndpoint", "inlinePlaybackEndpoint", "onTap", "metadata",
                "videoPreviewThumbnailRenderer", "thumbnailRenderer", "loggingDirectives", "menu", "buttons"};
            for (unsigned i = 0; i < sizeof(containers) / sizeof(containers[0]); i++) {
                YoutubeLiteSpan container;
                if (lite_json_member(&renderer, containers[i], &container)
                    && lite_json_key(&container, "percentDurationWatched", NULL, &value))
                    job->history_metadata |= 1u << (19u + i);
            }
            if (recent.has_resume) job->history_metadata |= 16384u;
            if (recent.has_progress) job->history_metadata |= 32768u;
#endif
            bool duplicate = false;
            for (size_t i = 0; i < job->build->video_count; i++) {
                YoutubeLiteVideo *video = &job->build->videos[i];
                if (strcmp(video->id, recent.id) != 0) continue;
                if (recent.has_resume) {
                    video->resume_seconds = recent.resume_seconds;
                    video->has_resume = true;
                }
                if (recent.has_progress) {
                    video->has_progress = true;
                    video->progress_percent = recent.progress_percent;
                }
                duplicate = true;
                break;
            }
            if (!duplicate) {
                job->build->watch.video = recent;
                job->build->recent_video = true;
            }
            lite_feed_history_finish(job);
        } else job->cursor = (size_t) (renderer.end - job->response.data);
    } else if (scope.end == job->response.data + job->history_contents_end) {
        lite_feed_history_finish(job);
    } else job->cursor += YOUTUBE_LITE_BUILD_SCAN_BYTES;
}

YoutubeLiteLoadStatus youtube_lite_feed_pump(YoutubeLiteFeedJob *job,
                                            const FetchPumpQuota *quota)
{
    if (job == NULL) return YOUTUBE_LITE_LOAD_FAILED;
    if (job->status != YOUTUBE_LITE_LOAD_PENDING) return job->status;
    if (job->phase == 7u) {
        lite_feed_history_video_pump(job);
        return job->status;
    }
    if (job->phase == 6u) {
        bool parsed = lite_feed_json_pump(job);
        if (!parsed) {
            lite_feed_history_finish(job);
            return job->status;
        }
        if (job->cursor != job->response.length) return job->status;
        bool complete = job->json_done && !job->json_error && job->context_end > job->context_begin
            && job->context_end - job->context_begin <= 16u * 1024u;
        YoutubeLiteSpan context = {job->response.data + job->context_begin,
                                  job->response.data + job->context_end};
        if (!complete || !lite_feed_authenticated(&context)
            || job->history_contents_end <= job->history_contents_begin) lite_feed_history_finish(job);
        else { job->cursor = job->history_contents_begin; job->phase = 7u; }
        return job->status;
    }
    if (job->phase == 4u) {
        if (!lite_feed_json_pump(job)) job->status = YOUTUBE_LITE_LOAD_FAILED;
        if (job->status != YOUTUBE_LITE_LOAD_PENDING || job->cursor != job->response.length)
            return job->status;
        if (job->like_video_id[0] != '\0') {
#ifndef __PSP__
            job->rating_diagnostics.response_flags = lite_rating_response_flags(job);
#endif
            job->status = job->json_done && !job->json_error
                && lite_like_confirmed(job->response.data, job->response.length,
                    job->like_video_id, job->like_selected)
                ? YOUTUBE_LITE_LOAD_SUCCEEDED : YOUTUBE_LITE_LOAD_FAILED;
            return job->status;
        }
        bool complete = job->json_done && !job->json_error && job->context_end > job->context_begin
            && job->context_end - job->context_begin <= 16u * 1024u;
        YoutubeLiteSpan context = {job->response.data + job->context_begin,
                                  job->response.data + job->context_end};
        if (!complete || !lite_feed_authenticated(&context)) {
            job->status = YOUTUBE_LITE_LOAD_FAILED;
            return job->status;
        }
        const char *url = job->subscriptions
            ? "https://www.youtube.com/?tilefinch_feed=subscriptions"
            : "https://www.youtube.com/?tilefinch_feed=recommended";
        job->build = lite_build_work_create(job->budget, url, NULL, 0,
            job->response.length, NULL, 0, job->response.data, job->response.length,
            job->compact, false, NULL, 0);
        if (job->build == NULL) job->status = YOUTUBE_LITE_LOAD_FAILED;
        else job->phase = 3u;
        return job->status;
    }
    if (job->phase == 1u) {
        size_t stop = job->response.length - job->cursor;
        if (stop > 16u * 1024u) stop = 16u * 1024u;
        stop += job->cursor;
        for (size_t at = job->cursor; at < stop; at++) {
            char byte = job->response.data[at];
            if (byte == 'I' && job->identity.api_key[0] == '\0')
                (void) lite_json_key_string_at(job->response.data, job->response.length,
                    at, "INNERTUBE_API_KEY", job->identity.api_key, sizeof(job->identity.api_key));
            if (byte == 'I' && job->identity.client_version[0] == '\0')
                (void) lite_json_key_string_at(job->response.data, job->response.length,
                    at, "INNERTUBE_CONTEXT_CLIENT_VERSION", job->identity.client_version,
                    sizeof(job->identity.client_version));
            if (byte == 'I' && job->client_name[0] == '\0')
                (void) lite_json_key_string_at(job->response.data, job->response.length,
                    at, "INNERTUBE_CLIENT_NAME", job->client_name, sizeof(job->client_name));
            if (byte == 'V' && job->identity.visitor[0] == '\0')
                (void) lite_json_key_string_at(job->response.data, job->response.length,
                    at, "VISITOR_DATA", job->identity.visitor, sizeof(job->identity.visitor));
            if (byte == 'L' && at >= 1u && job->response.data[at - 1u] == '"'
                && job->response.length - at >= 17u
                && memcmp(job->response.data + at, "LOGGED_IN\"", 10u) == 0) {
                const char *value = job->response.data + at + 10u;
                const char *end = job->response.data + job->response.length;
                while (value < end && isspace((unsigned char) *value)) value++;
                if (value < end && *value++ == ':') {
                    while (value < end && isspace((unsigned char) *value)) value++;
                    job->logged_in = end - value >= 4 && memcmp(value, "true", 4) == 0;
                }
            }
            if (byte == 'S' && at > 0 && job->response.data[at - 1u] == '"') {
                /* SESSION_INDEX is numeric service configuration, not a fixed
                   assumption about which account the user selected. */
                char index[8];
                if (lite_json_key_string_at(job->response.data, job->response.length,
                    at, "SESSION_INDEX", index, sizeof(index))) {
                    char *end; unsigned long value = strtoul(index, &end, 10);
                    if (*end == '\0' && value <= 99u) job->auth_user = (unsigned) value;
                } else if (job->response.length - at > 15u
                    && memcmp(job->response.data + at, "SESSION_INDEX\"", 14u) == 0) {
                    const char *value = job->response.data + at + 14u;
                    const char *end = job->response.data + job->response.length;
                    while (value < end && isspace((unsigned char) *value)) value++;
                    if (value < end && *value++ == ':') {
                        while (value < end && isspace((unsigned char) *value)) value++;
                        unsigned number = 0, digits = 0;
                        while (value < end && *value >= '0' && *value <= '9' && digits < 3u) {
                            number = number * 10u + (unsigned) (*value++ - '0'); digits++;
                        }
                        if (digits != 0 && number <= 99u && value < end
                            && (*value == ',' || *value == '}' || isspace((unsigned char) *value)))
                            job->auth_user = number;
                    }
                }
            }
        }
        job->cursor = stop;
        if (stop != job->response.length) return job->status;
        lite_probe_release(&job->response);
        if (!lite_feed_enqueue(job, true)) job->status = YOUTUBE_LITE_LOAD_FAILED;
        else job->phase = 2u;
        return job->status;
    }
    if (job->phase == 3u) {
        if (!job->subscriptions && job->like_video_id[0] == '\0'
            && !job->history_attempted && job->build->phase == YOUTUBE_LITE_BUILD_EMIT_HEADER) {
            job->history_attempted = true;
            job->history_request = true;
            job->build->decoded = NULL;
            job->build->decoded_length = 0;
            lite_probe_release(&job->response);
            job->cursor = job->json_depth = 0;
            job->json_string = job->json_escape = job->json_started = job->json_done = job->json_error = false;
            job->context_begin = job->context_end = 0;
            if (lite_feed_enqueue(job, true)) job->phase = 5u;
            else lite_feed_history_finish(job);
            return job->status;
        }
        lite_build_work_pump(job->build);
        if (job->build->phase == YOUTUBE_LITE_BUILD_FAILED)
            job->status = YOUTUBE_LITE_LOAD_FAILED;
        else if (lite_build_work_take_document(job->build, &job->document))
            job->status = YOUTUBE_LITE_LOAD_SUCCEEDED;
        return job->status;
    }
    bool fetched = false;
    (void) fetch_scheduler_pump_bounded(job->scheduler, 1, 1, quota, NULL);
    if (!fetch_scheduler_take(job->scheduler, job->request, &fetched, &job->response))
        return job->status;
    job->request = 0;
#ifndef __PSP__
    if (job->like_video_id[0] != '\0') {
        if (job->phase == 0u) job->rating_diagnostics.bootstrap_status = job->response.status_code;
        else {
            job->rating_diagnostics.api_status = job->response.status_code;
            job->rating_diagnostics.api_bytes = job->response.length;
        }
    }
#endif
    if (!fetched || job->response.status_code != 200 || job->response.data == NULL
        || !tilefinch_url_same_origin(job->response.effective_url, "https://m.youtube.com/")) {
        if (job->phase == 5u) lite_feed_history_finish(job);
        else job->status = YOUTUBE_LITE_LOAD_FAILED;
        return job->status;
    }
    TilefinchRequestContext context = job->phase == 0u
        ? lite_primary_context("https://m.youtube.com/", job->target)
        : lite_supplemental_context("https://m.youtube.com/", job->target);
    context.initiator_url = "https://m.youtube.com/";
    lite_accept_cookies(job->session, &context, &job->response, job->target);
    if (job->phase == 0u) job->phase = 1u;
    else { job->phase = job->history_request ? 6u : 4u; job->cursor = 0; }
    return job->status;
}

bool youtube_lite_feed_take_document(YoutubeLiteFeedJob *job,
                                    YoutubeLiteDocument *document)
{
    if (job == NULL || document == NULL || job->status != YOUTUBE_LITE_LOAD_SUCCEEDED
        || job->document.html == NULL) return false;
    *document = job->document;
    job->document = (YoutubeLiteDocument) {0};
    return true;
}

void youtube_lite_feed_destroy(YoutubeLiteFeedJob *job)
{
    if (job == NULL) return;
    fetch_scheduler_destroy(job->scheduler);
    lite_build_work_destroy(job->build);
    lite_probe_release(&job->response);
    youtube_lite_document_destroy(&job->document);
    Budget *budget = job->budget;
    lite_probe_wipe(job, sizeof(*job));
    budget_free(budget, job);
}
