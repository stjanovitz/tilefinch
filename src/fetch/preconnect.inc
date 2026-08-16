/*
 * Speculative preconnect from the HOME surface
 * (docs/engineering/PSP_TRANSPORT.md).
 *
 * When a HOME tile holds focus during the user's think-time, it is a host they
 * are about to open.  This unit opens a single speculative TCP+TLS connection
 * to that host, share-attached so the warm DNS entry and TLS session it leaves
 * behind are the ones the real navigation reuses/resumes.  It is connect-only
 * (CURLOPT_CONNECT_ONLY): the TLS handshake completes with full certificate
 * validation, then the transfer stops -- no HTTP request, no bytes beyond the
 * handshake, no content fetched.
 *
 * On PSP the shared transport worker owns the CONNECT_ONLY easy and advances
 * it on its existing multi handle. A dedicated compact descriptor means this
 * never consumes one of the six page/resource response lanes. Host builds
 * retain the deterministic cooperative multi below. The pure dwell/
 * eligibility state machine carries the bounds the PSP-only call site
 * enforces, so those bounds remain host-testable without a device.
 */

static FetchPreconnectCounters fetch_preconnect_counters_state;

/* Speculation must never retain the shared transport indefinitely merely
   because a background DNS/TCP/TLS attempt stopped making progress. This is
   intentionally shorter than an authoritative navigation's timeout: a miss
   just falls back to the ordinary connection when the user presses X. */
#define FETCH_PRECONNECT_TIMEOUT_MS 8000L

static struct {
    CURLM *multi;
    CURL *easy;
    bool outstanding;  /* a speculative handle is live (started, not torn down) */
    bool connected;    /* the handshake finished; the host is parked warm */
    bool holds_global; /* this unit owns a curl_global_acquire reference */
    bool background_backend; /* PSP worker owns easy/multi and handshake */
    char origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT]; /* scheme://host:port key */
} fetch_preconnect_state;

void fetch_preconnect_counters(FetchPreconnectCounters *counters)
{
    if (counters == NULL) return;
    *counters = fetch_preconnect_counters_state;
}

void fetch_preconnect_counters_reset(void)
{
    memset(&fetch_preconnect_counters_state, 0,
           sizeof(fetch_preconnect_counters_state));
}

uint32_t fetch_preconnect_tile_key(const char *url_or_host)
{
    if (url_or_host == NULL) return 0;
    /* FNV-1a over the raw target string.  Two tiles pointing at the same host
       hash equal only if their resolved strings match; the caller feeds the
       resolved origin so per-tile spelling does not fragment the key. */
    uint32_t hash = 2166136261u;
    for (const unsigned char *p = (const unsigned char *) url_or_host;
         *p != '\0'; p++) {
        hash ^= (uint32_t) *p;
        hash *= 16777619u;
    }
    /* Never collide with the "no target" sentinel the dwell step treats as
       ineligible. */
    return hash == 0u ? 1u : hash;
}

FetchPreconnectDwellAction fetch_preconnect_dwell_step(
    FetchPreconnectDwell *dwell, bool eligible, uint32_t tile_key,
    uint32_t elapsed_ms, uint32_t dwell_threshold_ms)
{
    if (dwell == NULL) return FETCH_PRECONNECT_DWELL_IDLE;
    /* Ineligible (not HOME, no focused tile, network not READY, quiescing, or
       budget pressure) or no resolvable target: retire any dwell in progress.
       Only a dwell that had already fired a START owns a speculative
       connection worth cancelling. */
    if (!eligible || tile_key == 0u) {
        bool had_started = dwell->armed && dwell->started;
        dwell->armed = false;
        dwell->started = false;
        dwell->tile_key = 0u;
        dwell->elapsed_ms = 0u;
        return had_started ? FETCH_PRECONNECT_DWELL_CANCEL
                           : FETCH_PRECONNECT_DWELL_IDLE;
    }
    /* Focus arrived on a new target: (re)arm the dwell and, if the previous
       target had a speculative connection, cancel it. */
    if (!dwell->armed || dwell->tile_key != tile_key) {
        bool had_started = dwell->armed && dwell->started;
        dwell->armed = true;
        dwell->started = false;
        dwell->tile_key = tile_key;
        dwell->elapsed_ms = 0u;
        return had_started ? FETCH_PRECONNECT_DWELL_CANCEL
                           : FETCH_PRECONNECT_DWELL_IDLE;
    }
    /* Same target already settled: one speculative connection per episode. */
    if (dwell->started) return FETCH_PRECONNECT_DWELL_IDLE;
    /* Accumulate dwell time (saturating) until the settle threshold. */
    uint32_t next = dwell->elapsed_ms + elapsed_ms;
    dwell->elapsed_ms = next < dwell->elapsed_ms ? UINT32_MAX : next;
    if (dwell->elapsed_ms >= dwell_threshold_ms) {
        dwell->started = true;
        return FETCH_PRECONNECT_DWELL_START;
    }
    return FETCH_PRECONNECT_DWELL_IDLE;
}

static void fetch_preconnect_log(const char *event)
{
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
    /* The origin (scheme+host+port) is a user bookmark/built-in card, not a
       full navigation URL, so it carries no query token or credentials. */
    psp_log_printf(
        "tilefinch-preconnect: event=%s host=%s started=%zu completed=%zu "
        "reused=%zu cancelled=%zu\n",
        event,
        fetch_preconnect_state.origin[0] == '\0'
            ? "-" : fetch_preconnect_state.origin,
        fetch_preconnect_counters_state.started,
        fetch_preconnect_counters_state.completed,
        fetch_preconnect_counters_state.reused,
        fetch_preconnect_counters_state.cancelled);
#else
    (void) event;
#endif
}

/* Releases the speculative handle, its multi, and this unit's transport
   reference.  Order matters: the handle and multi are freed back to curl's
   bounded pool before the reference that would destroy that pool is dropped. */
static void fetch_preconnect_teardown(void)
{
    if (fetch_preconnect_state.background_backend) {
        fetch_background_preconnect_retire();
        fetch_preconnect_state.background_backend = false;
    }
    if (fetch_preconnect_state.easy != NULL) {
        if (fetch_preconnect_state.multi != NULL) {
            (void) curl_multi_remove_handle(fetch_preconnect_state.multi,
                                            fetch_preconnect_state.easy);
        }
        curl_easy_cleanup(fetch_preconnect_state.easy);
        fetch_preconnect_state.easy = NULL;
    }
    if (fetch_preconnect_state.multi != NULL) {
        (void) curl_multi_cleanup(fetch_preconnect_state.multi);
        fetch_preconnect_state.multi = NULL;
    }
    fetch_preconnect_state.outstanding = false;
    fetch_preconnect_state.connected = false;
    fetch_preconnect_state.origin[0] = '\0';
    if (fetch_preconnect_state.holds_global) {
        (void) curl_global_release();
        fetch_preconnect_state.holds_global = false;
    }
}

/* Normalizes a full URL or bare host into a scheme://host:port origin, the key
   used both to dedup and as the CONNECT_ONLY target.  A bare host is assumed
   HTTPS.  Returns false for empty or unparseable input. */
static bool fetch_preconnect_origin(const char *url_or_host, char *output,
                                    size_t output_size)
{
    if (url_or_host == NULL || url_or_host[0] == '\0') return false;
    const char *candidate = url_or_host;
    char scheme_buffer[TILEFINCH_URL_SERIALIZED_LIMIT];
    if (strstr(url_or_host, "://") == NULL) {
        int written = snprintf(scheme_buffer, sizeof(scheme_buffer),
                               "https://%s", url_or_host);
        if (written < 0 || (size_t) written >= sizeof(scheme_buffer)) {
            return false;
        }
        candidate = scheme_buffer;
    }
    return tilefinch_url_origin(candidate, output, output_size);
}

bool fetch_preconnect(const char *url_or_host, Budget *budget)
{
    char origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    if (!fetch_preconnect_origin(url_or_host, origin, sizeof(origin))) {
        return false;
    }
    /* Already speculating on this exact host: reuse it, do not reconnect. */
    if (fetch_preconnect_state.outstanding
        && strcmp(fetch_preconnect_state.origin, origin) == 0) {
        fetch_preconnect_counters_state.reused++;
        fetch_preconnect_log("reused");
        return true;
    }
    /* At most one outstanding: a different host replaces the current one. */
    if (fetch_preconnect_state.outstanding) {
        fetch_preconnect_cancel("replaced");
    }
    /* A failed speculative admission is simply a miss. Never fall back to
       PSP browser-thread curl after the worker capability exists. */
    if (fetch_background_transport_available()) {
        if (!fetch_background_preconnect_begin(
                budget, origin, FETCH_PRECONNECT_TIMEOUT_MS)) return false;
        fetch_preconnect_state.outstanding = true;
        fetch_preconnect_state.connected = false;
        fetch_preconnect_state.background_backend = true;
        (void) snprintf(fetch_preconnect_state.origin,
                        sizeof(fetch_preconnect_state.origin), "%s", origin);
        fetch_preconnect_counters_state.started++;
        fetch_preconnect_log("started");
        return true;
    }
    /* Bring up (or attach to) the shared transport and hold a reference for as
       long as the speculative connection lives, so its warm DNS/TLS-session
       cache -- and the connection itself -- survive scheduler churn during the
       user's think-time and are still there when navigation acquires. */
    if (!curl_global_acquire(budget)) return false;
    CURLM *multi = curl_multi_init();
    CURL *easy = curl_easy_init();
    if (multi == NULL || easy == NULL) {
        if (easy != NULL) curl_easy_cleanup(easy);
        if (multi != NULL) (void) curl_multi_cleanup(multi);
        (void) curl_global_release();
        return false;
    }
    if (!curl_easy_attach_shared_state(easy)
        || curl_easy_setopt(easy, CURLOPT_URL, origin) != CURLE_OK
        || curl_easy_setopt(easy, CURLOPT_CONNECT_ONLY, 1L) != CURLE_OK
        || curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L) != CURLE_OK
        || curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS,
                            FETCH_PRECONNECT_TIMEOUT_MS) != CURLE_OK
        || curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS,
                            FETCH_PRECONNECT_TIMEOUT_MS) != CURLE_OK
        || curl_easy_setopt(easy, TILEFINCH_CURL_PROTOCOLS_OPT,
                            TILEFINCH_CURL_HTTP_PROTOCOLS) != CURLE_OK
        || !fetch_configure_http_version(easy)
        || !fetch_configure_tls(easy)
        || curl_multi_add_handle(multi, easy) != CURLM_OK) {
        curl_easy_cleanup(easy);
        (void) curl_multi_cleanup(multi);
        (void) curl_global_release();
        return false;
    }
    fetch_preconnect_state.multi = multi;
    fetch_preconnect_state.easy = easy;
    fetch_preconnect_state.outstanding = true;
    fetch_preconnect_state.connected = false;
    fetch_preconnect_state.holds_global = true;
    (void) snprintf(fetch_preconnect_state.origin,
                    sizeof(fetch_preconnect_state.origin), "%s", origin);
    fetch_preconnect_counters_state.started++;
    fetch_preconnect_log("started");
    return true;
}

void fetch_preconnect_pump(void)
{
    if (fetch_preconnect_state.outstanding
        && fetch_preconnect_state.background_backend) {
        if (fetch_preconnect_state.connected) return;
        bool complete = false;
        bool success = false;
        if (!fetch_background_preconnect_poll(&complete, &success)) {
            fetch_preconnect_log("failed");
            fetch_preconnect_teardown();
            return;
        }
        if (!complete) return;
        if (success) {
            fetch_preconnect_state.connected = true;
            fetch_preconnect_counters_state.completed++;
            fetch_preconnect_log("completed");
        } else {
            fetch_preconnect_log("failed");
            fetch_preconnect_teardown();
        }
        return;
    }
    if (!fetch_preconnect_state.outstanding || fetch_preconnect_state.connected
        || fetch_preconnect_state.multi == NULL) {
        return;
    }
    int running = 0;
    if (curl_multi_perform(fetch_preconnect_state.multi, &running)
        != CURLM_OK) {
        /* A hard multi error is a failed speculation, not a user cancel. */
        fetch_preconnect_log("failed");
        fetch_preconnect_teardown();
        return;
    }
    int remaining = 0;
    CURLMsg *message;
    while ((message = curl_multi_info_read(fetch_preconnect_state.multi,
                                           &remaining)) != NULL) {
        if (message->msg != CURLMSG_DONE
            || message->easy_handle != fetch_preconnect_state.easy) {
            continue;
        }
        if (message->data.result == CURLE_OK) {
            /* Handshake done: the host is warm.  Keep the handle parked (its
               socket stays open under CONNECT_ONLY, its session is now in the
               shared cache) and stop pumping. */
            fetch_preconnect_state.connected = true;
            fetch_preconnect_counters_state.completed++;
            fetch_preconnect_log("completed");
        } else {
            /* Handshake failed (DNS/connect/TLS): tear down.  A failed
               speculation is neither a completion nor a user cancel. */
            fetch_preconnect_log("failed");
            fetch_preconnect_teardown();
        }
        return;
    }
}

void fetch_preconnect_cancel(const char *reason)
{
    if (!fetch_preconnect_state.outstanding) return;
    /* Only an in-flight connection is a "cancel"; a completed one being torn
       down on leaving HOME is ordinary cleanup. */
    if (!fetch_preconnect_state.connected) {
        fetch_preconnect_counters_state.cancelled++;
    }
    fetch_preconnect_log(reason == NULL ? "cancelled" : reason);
    fetch_preconnect_teardown();
}

bool fetch_preconnect_active(void)
{
    return fetch_preconnect_state.outstanding;
}
