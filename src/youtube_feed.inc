/* Personalized responses never enter the persistent or document caches.
   One bootstrap, one feed and an optional recent-history request, with
   bounded renderer walks; all RAM-owned. */
enum { LITE_FEED_BYTES = 512u * 1024u };
/* One framing step examines at most this many response bytes, and the
   located responseContext object may be at most this large. */
enum {
    LITE_FEED_SCAN_BYTES = 16u * 1024u,
    LITE_FEED_CONTEXT_BYTES = 16u * 1024u
};
/* YoutubeLiteFeedJob.phase. The values are reported as numbers by
   youtube_lite_feed_diagnostics, so they never change. */
typedef enum {
    LITE_FEED_BOOTSTRAP = 0,        /* fetching the bootstrap page */
    LITE_FEED_IDENTITY = 1,         /* scanning it for identity and login */
    LITE_FEED_API = 2,              /* fetching the browse/action API */
    LITE_FEED_BUILD = 3,            /* building the feed page */
    LITE_FEED_API_FRAMING = 4,      /* framing the API response */
    LITE_FEED_HISTORY_FETCH = 5,    /* fetching optional recent history */
    LITE_FEED_HISTORY_FRAMING = 6,  /* framing the history response */
    LITE_FEED_HISTORY_VIDEO = 7,    /* reading its first history video */
    LITE_FEED_PLAYLIST_RECHECK = 8, /* waiting to re-read a playlist edit */
    LITE_FEED_ACCOUNT_STATE = 9     /* confirming exact account state */
} LiteFeedPhase;
struct YoutubeLiteFeedJob {
    Budget *budget;
    BrowserSession *session;
    FetchScheduler *scheduler;
    uint64_t request;
    uint64_t began_us;
    long timeout_ms;
    unsigned phase; /* LiteFeedPhase */
    size_t cursor;
    bool subscriptions;
    bool history_attempted;
    bool history_request;
    char like_video_id[YOUTUBE_VIDEO_ID_CAPACITY];
    bool like_selected;
    YoutubeLiteAccountAction action;
    char action_target[YOUTUBE_ACCOUNT_ID_CAPACITY];
    char action_video[YOUTUBE_VIDEO_ID_CAPACITY];
    bool action_prepared;
    bool action_submitted;
    bool action_readback;
    unsigned action_rechecks;
    uint64_t action_recheck_us;
    uint64_t login_identity;
    bool compact;
    bool request_signed;
    bool request_admitted, configuration_valid, clock_available;
    bool api_authenticated;
    bool transported;
    bool same_origin;
    long http_status;
    long bootstrap_status, api_status, history_status;
    long transport_code, tls_verify_result;
    bool tls_verify_result_available, tls_verification_failed, transport_timed_out;
    YoutubeLiteFeedFailure failure;
    uint64_t authenticated_identity;
    uint64_t rejected_identity;
    unsigned history_outcome;
    YoutubeLiteLoadStatus status;
    YoutubeLiteIdentity identity;
    YoutubeLiteBootstrapAccount account;
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
    YoutubeLiteAccountDiagnostics account_diagnostics;
    unsigned history_metadata;
#endif
};

bool youtube_lite_feed_diagnostics(const YoutubeLiteFeedJob *job,
                                  YoutubeLiteFeedDiagnostics *result)
{
    if (job == NULL || result == NULL) return false;
    *result = (YoutubeLiteFeedDiagnostics) {
        .phase = job->phase, .http_status = job->http_status,
        .bootstrap_status = job->bootstrap_status, .api_status = job->api_status,
        .history_status = job->history_status, .failure = job->failure,
        .history_outcome = job->history_outcome,
        .transported = job->transported, .same_origin = job->same_origin,
        .bootstrap_signed_in = job->account.logged_in,
        .auth_user_known = job->identity.auth_user_known,
        .request_signed = job->request_signed, .api_authenticated = job->api_authenticated,
        .request_admitted = job->request_admitted, .configuration_valid = job->configuration_valid,
        .clock_available = job->clock_available,
        .result_count = job->document.result_count,
        .transport_code = job->transport_code,
        .tls_verify_result = job->tls_verify_result,
        .tls_verify_result_available = job->tls_verify_result_available,
        .tls_verification_failed = job->tls_verification_failed,
        .transport_timed_out = job->transport_timed_out
    };
    return true;
}

uint64_t youtube_lite_feed_authenticated_identity(const YoutubeLiteFeedJob *job)
{
    return job == NULL ? 0 : job->authenticated_identity;
}

uint64_t youtube_lite_feed_rejected_identity(const YoutubeLiteFeedJob *job)
{
    return job == NULL ? 0 : job->rejected_identity;
}

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
bool youtube_lite_account_diagnostics(const YoutubeLiteFeedJob *job,
                                     YoutubeLiteAccountDiagnostics *result)
{
    if (job == NULL || result == NULL) return false;
    *result = job->account_diagnostics;
    result->phase = job->phase;
    result->prepared = job->action_prepared;
    result->submitted = job->action_submitted;
    result->rechecks = job->action_rechecks;
    return true;
}

static unsigned lite_account_response_flags(const YoutubeLiteFeedJob *job)
{
    if (job->response.data == NULL) return 0;
    static const char *const keys[] = {"responseContext", "error",
        "playlistAddToOptionRenderer", "playlistAddToOptionViewModel",
        "subscribeButtonRenderer", "subscribeButtonViewModel",
        "containsSelectedVideos", "subscribed", "channelId", "channelIds",
        "subscribeEndpoint", "unsubscribeEndpoint", "playlistId", "isDisabled",
        "playlistEditResults", "playlistEditVideoAddedResultData",
        "playlistEditVideoRemovedResultData", "status", "addToPlaylistRenderer",
        "alertRenderer", "onResponseReceivedActions", "addToPlaylistCommand",
        "showMiniplayerCommand", "contents", "popup", "loggedOut", "isSubscribed",
        "updateSubscribeButtonAction"};
    const char *data = job->response.data;
    size_t length = job->response.length;
    YoutubeLiteSpan root = {data, data + length}, value;
    unsigned flags = 0;
    for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        if (lite_json_key(&root, keys[i], NULL, &value)) flags |= 1u << i;
    return flags;
}
#endif

/*
 * The identity is asked for several times a frame while a feed is pending or
 * a login offer is up, so it is cached rather than re-read from the jar each
 * time. Its answer depends only on the identity cookies' own fields and
 * bytes, on site-data permission and on the cookie clock (expiry). Every jar
 * mutation changes some row's value pointer (a replacement allocates before
 * it frees), length, creation sequence, expiry or flags; only an identity
 * cookie's bytes can change under all of those, and they are hashed. So a key
 * over the rows plus the clock's second tells exactly when the jar must be
 * read again. A replayed capture's cookie clock can move within one wall
 * second; the identity then follows on the next second.
 * Main-thread state, like the session it describes.
 */
static struct {
    const BrowserSession *session;
    uint64_t key;
    uint64_t identity;
    bool valid;
} lite_login_identity_cache;

static uint64_t lite_login_identity_mix(uint64_t hash, uint64_t value)
{
    return (hash ^ value) * UINT64_C(1099511628211);
}

static uint64_t lite_login_identity_key(const BrowserSession *session,
                                        int64_t now)
{
    uint64_t key = lite_login_identity_mix(
        UINT64_C(14695981039346656037), (uintptr_t) session);
    key = lite_login_identity_mix(key, session->site_data_allowed);
    key = lite_login_identity_mix(key, (uint64_t) now);
    for (size_t i = 0; i < BROWSER_COOKIE_ENTRIES; i++) {
        const BrowserCookieEntry *entry = &session->cookies[i];
        if (entry->value == NULL) continue;
        key = lite_login_identity_mix(key, i);
        key = lite_login_identity_mix(key, (uintptr_t) entry->value);
        key = lite_login_identity_mix(key, entry->value_length);
        key = lite_login_identity_mix(key, entry->creation_sequence);
        key = lite_login_identity_mix(key, (uint64_t) entry->expires_at);
        key = lite_login_identity_mix(key, (uint64_t) entry->host_only
            | (uint64_t) entry->secure << 1 | (uint64_t) entry->http_only << 2
            | (uint64_t) entry->partitioned << 3
            | (uint64_t) entry->same_site << 4);
        if (strcmp(entry->name, "SAPISID") == 0
            || strcmp(entry->name, "__Secure-3PAPISID") == 0) {
            for (size_t at = 0; at < entry->value_length; at++)
                key = lite_login_identity_mix(
                    key, (unsigned char) entry->value[at]);
        }
    }
    return key;
}

uint64_t youtube_lite_session_login_identity(BrowserSession *session)
{
    if (session == NULL) return 0;
    uint64_t key = lite_login_identity_key(session, (int64_t) time(NULL));
    if (lite_login_identity_cache.valid
        && lite_login_identity_cache.session == session
        && lite_login_identity_cache.key == key)
        return lite_login_identity_cache.identity;
#ifndef __PSP__
    tilefinch_test_faults()->youtube_login_identity_builds++;
#endif
    const char *origin = "https://m.youtube.com/";
    TilefinchRequestContext context = lite_supplemental_context(origin, origin);
    context.initiator_url = origin;
    const char *value = NULL;
    size_t length = 0;
    uint64_t identity = 0;
    if (lite_session_sapisid(session, &context, &value, &length)) {
        identity = UINT64_C(14695981039346656037);
        for (size_t i = 0; i < length; i++)
            identity = (identity ^ (unsigned char) value[i]) * UINT64_C(1099511628211);
        if (identity == 0) identity = 1;
    }
    lite_login_identity_cache.session = session;
    lite_login_identity_cache.key = key;
    lite_login_identity_cache.identity = identity;
    lite_login_identity_cache.valid = true;
    return identity; /* RAM-only change detection, never an authentication proof. */
}

/* Google's account cookies as YouTube receives them at sign-in: the SID
   family, its API keys (SAPISID signs requests) and LOGIN_INFO. */
static const char *const lite_account_cookies[] = {
    "SAPISID", "__Secure-3PAPISID", "__Secure-1PAPISID", "APISID",
    "SID", "HSID", "SSID", "__Secure-1PSID", "__Secure-3PSID",
    "__Secure-1PSIDTS", "__Secure-3PSIDTS",
    "SIDCC", "__Secure-1PSIDCC", "__Secure-3PSIDCC", "LOGIN_INFO"
};

void youtube_lite_session_register_credentials(BrowserSession *session)
{
    browser_session_set_credential_cookies(
        session, "youtube.com", lite_account_cookies,
        sizeof(lite_account_cookies) / sizeof(lite_account_cookies[0]));
}

bool youtube_lite_session_signed_in(BrowserSession *session)
{
    return youtube_lite_session_login_identity(session) != 0;
}

static bool lite_feed_enqueue(YoutubeLiteFeedJob *job, bool api)
{
    const char *origin = "https://m.youtube.com/";
    char endpoint[512];
    char body[4096], visitor[2048], headers[512];
    char authorization[96];
    authorization[0] = '\0';
    /* Each request buffer is terminated by its writer before use, and wiped
       below on every outcome; clearing unused capacity first is redundant. */
    strcpy(endpoint, origin);
    TilefinchRequestContext context = lite_primary_context(origin, origin);
    FetchRequest request = {.method = "GET", .accept = "text/html",
        .user_agent = YOUTUBE_LITE_MOBILE_UA, .allow_http_errors = true,
        .redirect_same_origin_only = true};
    bool okay = true;
    bool playlist_action = job->action == YOUTUBE_LITE_ACTION_PLAYLIST_ADD
        || job->action == YOUTUBE_LITE_ACTION_PLAYLIST_REMOVE;
    bool action_query = job->action != YOUTUBE_LITE_ACTION_NONE
        && (!job->action_prepared || job->action_submitted);
    if (api) {
        int count = snprintf(endpoint, sizeof(endpoint),
            "https://m.youtube.com/youtubei/v1/%s?key=%s&prettyPrint=false",
            job->action != YOUTUBE_LITE_ACTION_NONE
                ? playlist_action ? (action_query ? "playlist/get_add_to_playlist" : "browse/edit_playlist")
                : action_query ? "browse"
                : job->action == YOUTUBE_LITE_ACTION_SUBSCRIBE ? "subscription/subscribe" : "subscription/unsubscribe"
                : job->like_video_id[0] != '\0'
                ? (job->like_selected ? "like/like" : "like/removelike") : "browse",
            job->identity.api_key);
        okay = count > 0 && (size_t) count < sizeof(endpoint)
            && lite_identity_valid(&job->identity) && strcmp(job->account.client_name, "MWEB") == 0
            && lite_json_quote(job->identity.visitor, visitor, sizeof(visitor));
        char payload[512];
        if (job->action != YOUTUBE_LITE_ACTION_NONE) {
            if (playlist_action && action_query)
                count = snprintf(payload, sizeof(payload), "\"videoIds\":[\"%s\"]", job->action_video);
            else if (playlist_action)
                count = snprintf(payload, sizeof(payload),
                    "\"playlistId\":\"%s\",\"actions\":[{\"action\":\"%s\",\"%s\":\"%s\"}]",
                    job->action_target,
                    job->action == YOUTUBE_LITE_ACTION_PLAYLIST_ADD ? "ACTION_ADD_VIDEO" : "ACTION_REMOVE_VIDEO_BY_VIDEO_ID",
                    job->action == YOUTUBE_LITE_ACTION_PLAYLIST_ADD ? "addedVideoId" : "removedVideoId", job->action_video);
            else if (action_query)
                count = snprintf(payload, sizeof(payload), "\"browseId\":\"%s\"", job->action_target);
            else count = snprintf(payload, sizeof(payload), "\"channelIds\":[\"%s\"]", job->action_target);
        } else {
            count = snprintf(payload, sizeof(payload), "%s\"%s\"%s",
                job->like_video_id[0] != '\0' ? "\"target\":{\"videoId\":" : "\"browseId\":",
                job->like_video_id[0] != '\0' ? job->like_video_id
                    : job->history_request ? "FEhistory"
                    : job->subscriptions ? "FEsubscriptions" : "FEwhat_to_watch",
                job->like_video_id[0] != '\0' ? "}" : "");
        }
        okay = okay && count > 0 && (size_t) count < sizeof(payload);
        count = snprintf(body, sizeof(body),
            "{\"context\":{\"client\":{\"clientName\":\"MWEB\",\"clientVersion\":\"%s\","
            "\"hl\":\"%s\",\"gl\":\"US\",\"visitorData\":%s}},%s}",
            job->identity.client_version, lite_preferred_language(), okay ? visitor : "\"\"", payload);
        okay = okay && count > 0 && (size_t) count < sizeof(body);
        lite_secure_wipe(payload, sizeof(payload));
        size_t body_length = okay ? (size_t) count : 0u;
        context = lite_supplemental_context(origin, endpoint);
        context.initiator_url = origin;
        job->configuration_valid = okay;
        job->clock_available = tilefinch_platform_wall_time_ns() != 0;
        if (!job->history_request && job->action == YOUTUBE_LITE_ACTION_NONE
            && job->like_video_id[0] == '\0')
            job->login_identity = youtube_lite_session_login_identity(job->session);
        if (!okay) job->failure = YOUTUBE_FEED_FAILURE_CONFIGURATION;
        okay = okay && job->account.logged_in && lite_feed_authorization(job->session,
            &context, tilefinch_platform_wall_time_ns() / UINT64_C(1000000000),
            authorization, sizeof(authorization));
        job->request_signed = okay;
        if (!okay && job->failure == YOUTUBE_FEED_FAILURE_NONE)
            job->failure = YOUTUBE_FEED_FAILURE_AUTHORIZATION;
        count = snprintf(headers, sizeof(headers),
            "X-YouTube-Client-Name: 2\nX-YouTube-Client-Version: %s"
            "\nAuthorization: %s\nX-Origin: https://m.youtube.com"
            "\nX-Goog-AuthUser: %u\nX-Youtube-Bootstrap-Logged-In: true",
            job->identity.client_version, authorization, job->identity.auth_user);
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
        && fetch_prepare_page_request_context(&context, endpoint,
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
    lite_secure_wipe(&prepared, sizeof(prepared));
    lite_secure_wipe(body, sizeof(body)); lite_secure_wipe(visitor, sizeof(visitor));
    lite_secure_wipe(headers, sizeof(headers)); lite_secure_wipe(authorization, sizeof(authorization));
    lite_secure_wipe(endpoint, sizeof(endpoint));
    if ((!okay || job->request == 0) && job->failure == YOUTUBE_FEED_FAILURE_NONE)
        job->failure = YOUTUBE_FEED_FAILURE_ADMISSION;
    if (api && !job->history_request) job->request_admitted = okay && job->request != 0;
    return okay && job->request != 0;
}

static YoutubeLiteFeedJob *lite_feed_create(Budget *budget,
    BrowserSession *session, long timeout_ms)
{
    if (budget == NULL || session == NULL || timeout_ms <= 0) return NULL;
    YoutubeLiteFeedJob *job = budget_calloc_category(budget,
        BUDGET_CATEGORY_RESOURCE, 1, sizeof(*job));
    if (job == NULL) return NULL;
    job->budget = budget; job->session = session;
    job->timeout_ms = timeout_ms;
    job->began_us = tilefinch_platform_monotonic_time_us();
    job->identity.version = YOUTUBE_LITE_IDENTITY_CACHE_VERSION;
    job->scheduler = fetch_scheduler_create_ex(budget, 1,
        YOUTUBE_LITE_MAXIMUM_SOURCE_BYTES, NULL, 0);
    if (job->scheduler != NULL)
        (void) fetch_scheduler_enable_background_transport(job->scheduler, true);
    job->login_identity = youtube_lite_session_login_identity(session);
    if (job->scheduler == NULL) {
        youtube_lite_feed_destroy(job);
        return NULL;
    }
    return job;
}

YoutubeLiteFeedJob *youtube_lite_feed_begin(Budget *budget,
    BrowserSession *session, bool subscriptions, bool compact, long timeout_ms)
{
    YoutubeLiteFeedJob *job = lite_feed_create(budget, session, timeout_ms);
    if (job == NULL) return NULL;
    job->subscriptions = subscriptions; job->compact = compact;
    if (!lite_feed_enqueue(job, false)) { youtube_lite_feed_destroy(job); return NULL; }
    return job;
}

YoutubeLiteFeedJob *youtube_lite_account_action_begin(Budget *budget,
    BrowserSession *session, YoutubeLiteAccountAction action,
    const char *target_id, const char *video_id, long timeout_ms)
{
    bool channel = action == YOUTUBE_LITE_ACTION_SUBSCRIBE || action == YOUTUBE_LITE_ACTION_UNSUBSCRIBE;
    if (action < YOUTUBE_LITE_ACTION_PLAYLIST_ADD || action > YOUTUBE_LITE_ACTION_UNSUBSCRIBE
        || !lite_valid_account_id(target_id, channel)
        || (!channel && (video_id == NULL || !lite_valid_video_id(video_id)))
        || session == NULL || !youtube_lite_session_signed_in(session)) return NULL;
    YoutubeLiteFeedJob *job = lite_feed_create(budget, session, timeout_ms);
    if (job == NULL) return NULL;
    job->action = action;
    snprintf(job->action_target, sizeof(job->action_target), "%s", target_id);
    if (!channel) snprintf(job->action_video, sizeof(job->action_video), "%s", video_id);
    if (!lite_feed_enqueue(job, false)) { youtube_lite_feed_destroy(job); return NULL; }
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

static int lite_feed_authentication(const YoutubeLiteSpan *context)
{
    YoutubeLiteSpan services, service, params, param, flag;
    if (lite_json_member(context, "mainAppWebResponseContext", &flag)
        && lite_json_member(&flag, "loggedOut", &services))
        return services.end - services.start == 5 && memcmp(services.start, "false", 5) == 0 ? 1
            : services.end - services.start == 4 && memcmp(services.start, "true", 4) == 0 ? -1 : 0;
    if (!lite_json_member(context, "serviceTrackingParams", &services)) return 0;
    const char *cursor = NULL;
    for (unsigned i = 0; i < 64u && lite_array_next(&services, &cursor, &service); i++) {
        if (!lite_json_member(&service, "params", &params)) continue;
        const char *at = NULL;
        for (unsigned j = 0; j < 64u && lite_array_next(&params, &at, &param); j++) {
            char key[32], value[8];
            if (lite_member_string(&param, "key", key, sizeof(key), false)
                && strcmp(key, "logged_in") == 0
                && lite_member_string(&param, "value", value, sizeof(value), false))
                return strcmp(value, "1") == 0 ? 1 : strcmp(value, "0") == 0 ? -1 : 0;
        }
    }
    return 0;
}

static bool lite_feed_authenticated(const YoutubeLiteSpan *context)
{
    return lite_feed_authentication(context) > 0;
}

static bool lite_feed_json_pump(YoutubeLiteFeedJob *job)
{
    const char *data = job->response.data;
    size_t stop = job->response.length - job->cursor;
    if (stop > LITE_FEED_SCAN_BYTES) stop = LITE_FEED_SCAN_BYTES;
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
    if (job->history_outcome == 1) job->history_outcome = 2;
    lite_secure_release(&job->response);
    job->history_request = false;
    job->phase = LITE_FEED_BUILD;
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

#if defined(__GNUC__)
__attribute__((noinline))
#endif
static void lite_feed_next_api(YoutubeLiteFeedJob *job)
{
    lite_secure_release(&job->response);
    job->cursor = job->json_depth = 0;
    job->json_string = job->json_escape = job->json_started = job->json_done = job->json_error = false;
    job->context_begin = job->context_end = 0;
    if (lite_feed_enqueue(job, true)) job->phase = job->history_request
        ? LITE_FEED_HISTORY_FETCH : LITE_FEED_API;
    else if (job->history_request) lite_feed_history_finish(job);
    else job->status = YOUTUBE_LITE_LOAD_FAILED;
}

static void lite_account_unconfirmed(YoutubeLiteFeedJob *job)
{
    /* Only a completed playlist edit can need eventual-consistency reads.
       action_submitted remains latched: these requests can never edit again.
       Allow three five-second propagation intervals, within the job deadline. */
    if (job->action < YOUTUBE_LITE_ACTION_SUBSCRIBE && job->action_submitted
        && job->action_rechecks < 3u) {
        job->action_rechecks++;
        job->action_recheck_us = tilefinch_platform_monotonic_time_us()
            + 5000000u;
        lite_secure_release(&job->response);
        job->phase = LITE_FEED_PLAYLIST_RECHECK;
    } else job->status = YOUTUBE_LITE_LOAD_FAILED;
}

static bool lite_account_channel_matches(const YoutubeLiteSpan *object, const char *target)
{
    char id[YOUTUBE_CHANNEL_ID_CAPACITY];
    YoutubeLiteSpan value, endpoint, ids, extra;
    if (lite_json_member(object, "channelId", &value))
        return lite_json_string(value.start, value.end, id, sizeof(id))
            && strcmp(id, target) == 0;
    const char *keys[] = {"subscribeEndpoint", "unsubscribeEndpoint"};
    for (unsigned i = 0; i < 2u; i++) {
        if (!lite_json_key(object, keys[i], NULL, &endpoint)
            || !lite_json_member(&endpoint, "channelIds", &ids)) continue;
        const char *at = NULL;
        if (lite_array_next(&ids, &at, &value)
            && lite_json_string(value.start, value.end, id, sizeof(id))
            && strcmp(id, target) == 0 && !lite_array_next(&ids, &at, &extra)) return true;
    }
    return false;
}

#if defined(__GNUC__)
__attribute__((noinline))
#endif
static void lite_account_state_pump(YoutubeLiteFeedJob *job)
{
    bool channel = job->action >= YOUTUBE_LITE_ACTION_SUBSCRIBE;
    const char *data = job->response.data;
    size_t length = job->response.length;
    YoutubeLiteSpan scope = lite_build_window(data, length, job->cursor), object;
    bool mutation = channel && job->action_submitted && !job->action_readback;
    const char *key = mutation ? "updateSubscribeButtonAction"
        : channel ? "subscribeButtonRenderer" : "playlistAddToOptionRenderer";
    bool found = lite_json_key(&scope, key, NULL, &object);
    if (channel && !mutation) {
        YoutubeLiteSpan modern;
        if (lite_json_key(&scope, "subscribeButtonViewModel", NULL, &modern)
            && (!found || modern.start < object.start)) { object = modern; found = true; }
    }
    if (found) {
        char target[YOUTUBE_ACCOUNT_ID_CAPACITY];
        bool selected = false, known = false;
#ifndef __PSP__
        job->account_diagnostics.candidates++;
        static const char *const fields[] = {"channelId", "subscribed", "channelIds",
            "subscribeEndpoint", "unsubscribeEndpoint", "playlistId", "containsSelectedVideos"};
        YoutubeLiteSpan diagnostic;
        for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
            if (lite_json_member(&object, fields[i], &diagnostic)) job->account_diagnostics.state_flags |= 1u << i;
            if (lite_json_key(&object, fields[i], NULL, &diagnostic)) job->account_diagnostics.state_flags |= 128u << i;
        }
#endif
        if (channel ? lite_account_channel_matches(&object, job->action_target)
            : lite_member_string(&object, "playlistId", target, sizeof(target), false)
                && strcmp(target, job->action_target) == 0) {
#ifndef __PSP__
            job->account_diagnostics.matching++;
            static const char *const model_fields[] = {"subscribed", "isSubscribed", "onTap",
                "onSubscribe", "onUnsubscribe", "subscriptionState", "subscriptionStateEntity",
                "stateEntityStoreKey", "subscriptionStateEntityStoreKey", "entityKey", "buttonText",
                "entityStoreKey", "subscribeButton", "subscribedButton", "buttonViewModel"};
            for (unsigned i = 0; i < sizeof(model_fields) / sizeof(model_fields[0]); i++)
                if (lite_json_key(&object, model_fields[i], NULL, &diagnostic))
                    job->account_diagnostics.model_flags |= 1u << i;
#endif
            if (channel) {
                YoutubeLiteSpan state;
                known = (lite_json_member(&object, "subscribed", &state)
                    || lite_json_member(&object, "isSubscribed", &state))
                    && ((state.end - state.start == 4 && memcmp(state.start, "true", 4) == 0)
                        || (state.end - state.start == 5 && memcmp(state.start, "false", 5) == 0));
                selected = known && state.end - state.start == 4;
            } else {
                char state[16];
                if (lite_member_string(&object, "containsSelectedVideos", state, sizeof(state), false)) {
                    selected = strcmp(state, "ALL") == 0;
                    known = selected || strcmp(state, "NONE") == 0;
                }
            }
#ifndef __PSP__
            job->account_diagnostics.selection = known ? selected ? 1u : 2u : 0u;
#endif
            /* The explicit action may proceed for an exact, state-less channel
               model. Unknown state can never confirm success. */
            if (known || (channel && !job->action_submitted)) {
                bool desired = job->action == YOUTUBE_LITE_ACTION_SUBSCRIBE
                    || job->action == YOUTUBE_LITE_ACTION_PLAYLIST_ADD;
                if (known && selected == desired) job->status = YOUTUBE_LITE_LOAD_SUCCEEDED;
                else if (job->action_submitted) lite_account_unconfirmed(job);
                else {
                    job->action_prepared = true;
                    lite_feed_next_api(job);
                }
                return;
            }
        }
        job->cursor = (size_t) (object.end - data);
    } else if (scope.end == data + length) {
        if (mutation) {
            job->action_readback = true;
            lite_feed_next_api(job);
        } else lite_account_unconfirmed(job);
    }
    else job->cursor += YOUTUBE_LITE_BUILD_SCAN_BYTES;
}

/* The responseContext the framing pass located, when it is complete and
   within LITE_FEED_CONTEXT_BYTES. */
static bool lite_feed_context(const YoutubeLiteFeedJob *job,
                              YoutubeLiteSpan *context)
{
    *context = (YoutubeLiteSpan) {job->response.data + job->context_begin,
                                  job->response.data + job->context_end};
    return job->context_end > job->context_begin
        && job->context_end - job->context_begin <= LITE_FEED_CONTEXT_BYTES;
}

/* LITE_FEED_IDENTITY: read the bootstrap page, then send the API request. */
static YoutubeLiteLoadStatus lite_feed_identity_pump(YoutubeLiteFeedJob *job)
{
    /* Account actions need the account index; every request signs with
       it when the page states one. */
    if (!lite_bootstrap_scan_pump(job->response.data, job->response.length,
            &job->cursor, true, &job->identity, &job->account))
        return job->status;
    if (job->account.login_known && !job->account.logged_in) {
        job->rejected_identity = job->login_identity;
        job->failure = YOUTUBE_FEED_FAILURE_AUTHENTICATION;
        job->status = YOUTUBE_LITE_LOAD_FAILED;
        return job->status;
    }
    if (job->action != YOUTUBE_LITE_ACTION_NONE
        && (!job->identity.auth_user_known || !job->account.logged_in)) {
        job->status = YOUTUBE_LITE_LOAD_FAILED; return job->status;
    }
    lite_secure_release(&job->response);
    if (!lite_feed_enqueue(job, true)) job->status = YOUTUBE_LITE_LOAD_FAILED;
    else job->phase = LITE_FEED_API;
    return job->status;
}

/* LITE_FEED_API_FRAMING: frame the API response, then confirm an account
   action, a rating, or the signed-in feed and start building its page. */
static YoutubeLiteLoadStatus lite_feed_api_framing_pump(YoutubeLiteFeedJob *job)
{
    if (!lite_feed_json_pump(job)) {
        job->failure = YOUTUBE_FEED_FAILURE_JSON;
        job->status = YOUTUBE_LITE_LOAD_FAILED;
    }
    if (job->status != YOUTUBE_LITE_LOAD_PENDING || job->cursor != job->response.length)
        return job->status;
    if (job->action != YOUTUBE_LITE_ACTION_NONE) {
        YoutubeLiteSpan root = {job->response.data, job->response.data + job->response.length}, error;
        if (!job->json_done || job->json_error || lite_json_member(&root, "error", &error)) {
            job->status = YOUTUBE_LITE_LOAD_FAILED; return job->status;
        }
        if (job->action_prepared && !job->action_submitted) {
            job->action_submitted = true;
            if (job->action >= YOUTUBE_LITE_ACTION_SUBSCRIBE) { job->cursor = 0; job->phase = LITE_FEED_ACCOUNT_STATE; }
            else lite_feed_next_api(job);
        } else {
            YoutubeLiteSpan context;
            if (!lite_feed_context(job, &context) || !lite_feed_authenticated(&context))
                job->status = YOUTUBE_LITE_LOAD_FAILED;
            else { job->cursor = 0; job->phase = LITE_FEED_ACCOUNT_STATE; }
        }
        return job->status;
    }
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
    YoutubeLiteSpan context;
    bool complete = job->json_done && !job->json_error
        && lite_feed_context(job, &context);
    if (!complete || !lite_feed_authenticated(&context)) {
        job->failure = complete ? YOUTUBE_FEED_FAILURE_AUTHENTICATION : YOUTUBE_FEED_FAILURE_JSON;
        if (complete && lite_feed_authentication(&context) < 0)
            job->rejected_identity = job->login_identity;
        job->status = YOUTUBE_LITE_LOAD_FAILED;
        return job->status;
    }
    /* Server proof belongs to the session that signed this request, not
       a replacement cookie jar. Even server rotation requires a fresh
       request before the replacement can be offered for persistence. */
    if (job->login_identity == 0 || job->login_identity
        != youtube_lite_session_login_identity(job->session)) {
        job->failure = YOUTUBE_FEED_FAILURE_SESSION_CHANGED;
        job->status = YOUTUBE_LITE_LOAD_FAILED;
        return job->status;
    }
    job->api_authenticated = true;
    job->authenticated_identity = job->login_identity;
    const char *url = job->subscriptions
        ? "https://www.youtube.com/?tilefinch_feed=subscriptions"
        : "https://www.youtube.com/?tilefinch_feed=recommended";
#ifndef __PSP__
    bool refuse_build = tilefinch_test_faults()->refuse_next_youtube_feed_build;
    tilefinch_test_faults()->refuse_next_youtube_feed_build = false;
    job->build = refuse_build ? NULL : lite_build_work_create(job->budget, url, NULL, 0,
#else
    job->build = lite_build_work_create(job->budget, url, NULL, 0,
#endif
        job->response.length, NULL, 0, job->response.data, job->response.length,
        job->compact, false, NULL, 0);
    if (job->build == NULL) {
        job->failure = YOUTUBE_FEED_FAILURE_BUILD;
        job->status = YOUTUBE_LITE_LOAD_FAILED;
    }
    else job->phase = LITE_FEED_BUILD;
    return job->status;
}

YoutubeLiteLoadStatus youtube_lite_feed_pump(YoutubeLiteFeedJob *job,
                                            const FetchPumpQuota *quota)
{
    if (job == NULL) return YOUTUBE_LITE_LOAD_FAILED;
    if (job->status != YOUTUBE_LITE_LOAD_PENDING) return job->status;
    /* Keep the session binding through optional history and construction,
       not only at the first authenticated response. */
    if (job->authenticated_identity != 0 && job->authenticated_identity
        != youtube_lite_session_login_identity(job->session)) {
        job->failure = YOUTUBE_FEED_FAILURE_SESSION_CHANGED;
        job->status = YOUTUBE_LITE_LOAD_CANCELLED;
        return job->status;
    }
    uint64_t now = tilefinch_platform_monotonic_time_us();
#ifndef __PSP__
    if (job->history_request && tilefinch_test_faults()->expire_youtube_history) {
        tilefinch_test_faults()->expire_youtube_history = false;
        now = job->began_us + (uint64_t) job->timeout_ms * 1000u;
    }
#endif
    if (now >= job->began_us && (now - job->began_us) / 1000u >= (uint64_t) job->timeout_ms) {
        if (job->api_authenticated && job->build != NULL
            && job->like_video_id[0] == '\0' && job->action == YOUTUBE_LITE_ACTION_NONE) {
            /* An authenticated feed is already owned. Optional history and
               bounded page construction must not revoke it at the deadline. */
            if (job->history_request) {
                if (job->request != 0) {
                    (void) fetch_scheduler_cancel(job->scheduler, job->request, "optional history deadline");
                    job->request = 0;
                }
                lite_feed_history_finish(job);
                job->history_outcome = 3;
            }
            job->history_attempted = true;
        } else {
            job->failure = YOUTUBE_FEED_FAILURE_TIMEOUT;
            job->status = YOUTUBE_LITE_LOAD_FAILED; return job->status;
        }
    }
    if (job->action != YOUTUBE_LITE_ACTION_NONE
        && (job->login_identity == 0 || job->login_identity != youtube_lite_session_login_identity(job->session))) {
        job->status = YOUTUBE_LITE_LOAD_CANCELLED; return job->status;
    }
    if (job->phase == LITE_FEED_PLAYLIST_RECHECK || job->phase == LITE_FEED_ACCOUNT_STATE) {
        if (job->phase == LITE_FEED_ACCOUNT_STATE) lite_account_state_pump(job);
        else if (now >= job->action_recheck_us) lite_feed_next_api(job);
        return job->status;
    }
    if (job->phase == LITE_FEED_HISTORY_VIDEO) {
        lite_feed_history_video_pump(job);
        return job->status;
    }
    if (job->phase == LITE_FEED_HISTORY_FRAMING) {
        bool parsed = lite_feed_json_pump(job);
        if (!parsed) {
            job->history_outcome = 4;
            lite_feed_history_finish(job);
            return job->status;
        }
        if (job->cursor != job->response.length) return job->status;
        YoutubeLiteSpan context;
        bool complete = job->json_done && !job->json_error
            && lite_feed_context(job, &context);
        if (!complete || !lite_feed_authenticated(&context)) {
            job->history_outcome = 4;
            lite_feed_history_finish(job);
        } else if (job->history_contents_end <= job->history_contents_begin) lite_feed_history_finish(job);
        else { job->cursor = job->history_contents_begin; job->phase = LITE_FEED_HISTORY_VIDEO; }
        return job->status;
    }
    if (job->phase == LITE_FEED_API_FRAMING) return lite_feed_api_framing_pump(job);
    if (job->phase == LITE_FEED_IDENTITY) return lite_feed_identity_pump(job);
    if (job->phase == LITE_FEED_BUILD) {
        if (!job->subscriptions && job->like_video_id[0] == '\0'
            && !job->history_attempted && job->build->phase == YOUTUBE_LITE_BUILD_EMIT_HEADER) {
            job->history_attempted = true;
            job->history_request = true;
            job->history_outcome = 1;
            job->build->decoded = NULL;
            job->build->decoded_length = 0;
            lite_feed_next_api(job);
            return job->status;
        }
        lite_build_work_pump(job->build);
        if (job->build->phase == YOUTUBE_LITE_BUILD_FAILED) {
            job->failure = YOUTUBE_FEED_FAILURE_BUILD;
            job->status = YOUTUBE_LITE_LOAD_FAILED;
        } else if (lite_build_work_take_document(job->build, &job->document))
            job->status = YOUTUBE_LITE_LOAD_SUCCEEDED;
        return job->status;
    }
    bool fetched = false;
    (void) fetch_scheduler_pump_bounded(job->scheduler, 1, 1, quota, NULL);
    if (!fetch_scheduler_take(job->scheduler, job->request, &fetched, &job->response))
        return job->status;
    job->request = 0;
    job->transport_code = job->response.transport_code;
    job->tls_verify_result = job->response.tls_verify_result;
    job->tls_verify_result_available = job->response.tls_verify_result_available;
    job->tls_verification_failed = job->response.tls_verification_failed;
    job->transport_timed_out = job->response.timed_out;
    job->http_status = job->response.status_code;
    if (job->phase == LITE_FEED_BOOTSTRAP) job->bootstrap_status = job->http_status;
    else if (job->history_request) job->history_status = job->http_status;
    else job->api_status = job->http_status;
    job->transported = fetched;
    job->same_origin = tilefinch_url_same_origin(job->response.effective_url, "https://m.youtube.com/");
#ifndef __PSP__
    if (job->action != YOUTUBE_LITE_ACTION_NONE) {
        job->account_diagnostics.status = job->response.status_code;
        job->account_diagnostics.bytes = job->response.length;
        job->account_diagnostics.flags = lite_account_response_flags(job);
        if (job->action_prepared && !job->action_submitted && job->response.data != NULL) {
            job->account_diagnostics.mutation_flags = job->account_diagnostics.flags;
            YoutubeLiteSpan root = {job->response.data, job->response.data + job->response.length};
            char status[32], playlist[YOUTUBE_ACCOUNT_ID_CAPACITY];
            job->account_diagnostics.mutation_bound = lite_member_string(&root, "playlistId", playlist, sizeof(playlist), false)
                && strcmp(playlist, job->action_target) == 0;
            if (lite_member_string(&root, "status", status, sizeof(status), false))
                job->account_diagnostics.mutation_status = strcmp(status, "STATUS_SUCCEEDED") == 0 ? 1u
                    : strcmp(status, "STATUS_FAILED") == 0 ? 2u : 3u;
        }
    }
    if (job->like_video_id[0] != '\0') {
        if (job->phase == LITE_FEED_BOOTSTRAP) job->rating_diagnostics.bootstrap_status = job->response.status_code;
        else {
            job->rating_diagnostics.api_status = job->response.status_code;
            job->rating_diagnostics.api_bytes = job->response.length;
        }
    }
#endif
    if (!fetched || job->response.status_code != 200 || job->response.data == NULL
        || !tilefinch_url_same_origin(job->response.effective_url, "https://m.youtube.com/")) {
        if (job->phase == LITE_FEED_HISTORY_FETCH) {
            lite_feed_history_finish(job);
            job->history_outcome = 4;
        }
        else {
            job->failure = !fetched ? YOUTUBE_FEED_FAILURE_TRANSPORT
                : job->http_status != 200 ? YOUTUBE_FEED_FAILURE_HTTP
                : !job->same_origin ? YOUTUBE_FEED_FAILURE_ORIGIN : YOUTUBE_FEED_FAILURE_JSON;
            job->status = YOUTUBE_LITE_LOAD_FAILED;
        }
        return job->status;
    }
    TilefinchRequestContext context = job->phase == LITE_FEED_BOOTSTRAP
        ? lite_primary_context("https://m.youtube.com/", job->target)
        : lite_supplemental_context("https://m.youtube.com/", job->target);
    context.initiator_url = "https://m.youtube.com/";
    lite_accept_cookies(job->session, &context, &job->response, job->target);
    if (job->action != YOUTUBE_LITE_ACTION_NONE)
        job->login_identity = youtube_lite_session_login_identity(job->session);
    if (job->phase == LITE_FEED_BOOTSTRAP) {
        job->account = (YoutubeLiteBootstrapAccount) {0};
        job->cursor = 0;
        job->phase = LITE_FEED_IDENTITY;
    } else { job->phase = job->history_request
        ? LITE_FEED_HISTORY_FRAMING : LITE_FEED_API_FRAMING; job->cursor = 0; }
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
    lite_secure_release(&job->response);
    youtube_lite_document_destroy(&job->document);
    Budget *budget = job->budget;
    lite_secure_wipe(job, sizeof(*job));
    budget_free(budget, job);
}
