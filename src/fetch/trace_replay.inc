static bool trace_replay_diagnostics_enabled(void)
{
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
    return true;
#else
    return tilefinch_trace_replay_diagnostics();
#endif
}

/* Route-selection decisions (unmatched, exhausted, invalid, rekeyed).  A
   PSP validation EBOOT has no stderr in its log, so these go to the engine
   log there; the host keeps stderr. */
static void trace_replay_decision(const char *format, ...)
{
    if (!trace_replay_diagnostics_enabled()) return;
    char line[512];
    va_list arguments;
    va_start(arguments, format);
    (void) vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
#if defined(__PSP__)
    tilefinch_platform_log_message(line);
#else
    fprintf(stderr, "%s\n", line);
#endif
}

/* Lab and validation builds only: a page can mint a fresh random identifier
   into a request URL on every run (chatgpt.com's conversation stream carries
   operationId=<uuid>), so the recorded URL can never recur.  When opted in,
   a query value shaped as a textual UUID (8-4-4-4-12 hexadecimal digits)
   matches any other UUID-shaped value of the same parameter; the scheme,
   host, path, parameter names and order, every other value, and the
   fragment still compare exactly.  Validation EBOOTs set the variable from
   boot.cfg's trace_volatile_uuids. */
#ifndef TILEFINCH_NO_FETCH_TRACE
static bool trace_replay_volatile_uuids(void)
{
#if defined(TILEFINCH_NO_TRACE) && !defined(TILEFINCH_PSP_VALIDATION_LOG)
    return false;
#else
    return getenv("TILEFINCH_REPLAY_VOLATILE_UUIDS") != NULL;
#endif
}

/* See FetchTrace.replay_pump_us. Validation EBOOTs set the variable from
   boot.cfg's trace_replay_pump_us. */
static uint32_t trace_replay_pump_us(void)
{
#if defined(TILEFINCH_NO_TRACE) && !defined(TILEFINCH_PSP_VALIDATION_LOG)
    return 0;
#else
    const char *value = getenv("TILEFINCH_REPLAY_PUMP_US");
    long parsed = value == NULL ? 0 : strtol(value, NULL, 10);
    return parsed > 0 && parsed <= 1000000 ? (uint32_t) parsed : 0u;
#endif
}
#endif

static bool trace_query_value_is_uuid(const char *value, size_t length)
{
    if (length != 36) return false;
    for (size_t index = 0; index < length; index++) {
        bool dash = index == 8 || index == 13 || index == 18 || index == 23;
        if (dash ? value[index] != '-'
                 : !isxdigit((unsigned char) value[index])) return false;
    }
    return true;
}

/* One query component: [start, stop) up to '&' or the end of the query. */
static const char *trace_query_component_stop(const char *start,
                                              const char *end)
{
    const char *stop = memchr(start, '&', (size_t) (end - start));
    return stop == NULL ? end : stop;
}

/* Length of the component's name prefix including '=', or 0 when its value
   is not UUID-shaped (the component then compares byte for byte). */
static size_t trace_query_uuid_name_length(const char *start,
                                           const char *stop)
{
    const char *equals = memchr(start, '=', (size_t) (stop - start));
    if (equals == NULL
        || !trace_query_value_is_uuid(
               equals + 1, (size_t) (stop - equals - 1))) return 0;
    return (size_t) (equals + 1 - start);
}

static bool trace_replay_url_equal(const char *recorded,
                                   const char *requested)
{
    if (strcmp(recorded, requested) == 0) return true;
    if (!fetch_trace.replay_volatile_uuids) return false;
    const char *recorded_query = strchr(recorded, '?');
    const char *requested_query = strchr(requested, '?');
    if (recorded_query == NULL || requested_query == NULL
        || recorded_query - recorded != requested_query - requested
        || memcmp(recorded, requested,
                  (size_t) (recorded_query - recorded)) != 0) return false;
    const char *recorded_end = recorded_query
        + strcspn(recorded_query, "#");
    const char *requested_end = requested_query
        + strcspn(requested_query, "#");
    if (strcmp(recorded_end, requested_end) != 0) return false;
    const char *a = recorded_query + 1, *b = requested_query + 1;
    for (;;) {
        const char *a_stop = trace_query_component_stop(a, recorded_end);
        const char *b_stop = trace_query_component_stop(b, requested_end);
        size_t a_name = trace_query_uuid_name_length(a, a_stop);
        size_t b_name = trace_query_uuid_name_length(b, b_stop);
        bool same = a_name != 0 && a_name == b_name
            ? memcmp(a, b, a_name) == 0
            : a_stop - a == b_stop - b
              && memcmp(a, b, (size_t) (a_stop - a)) == 0;
        if (!same) return false;
        if (a_stop == recorded_end || b_stop == requested_end) {
            return a_stop == recorded_end && b_stop == requested_end;
        }
        a = a_stop + 1;
        b = b_stop + 1;
    }
}

static uint64_t trace_hash_bytes(uint64_t hash, const char *bytes,
                                 size_t length)
{
    for (size_t index = 0; index < length; index++) {
        hash ^= (unsigned char) bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

/* Hashes the URL as trace_replay_url_equal() compares it: with the opt-in,
   each UUID-shaped query value hashes as one fixed marker byte (a C0
   control, which a serialized URL never contains). */
static uint64_t trace_hash_replay_url(uint64_t hash, const char *url)
{
    const char *query = fetch_trace.replay_volatile_uuids
        ? strchr(url, '?') : NULL;
    if (query == NULL) return trace_hash_bytes(hash, url, strlen(url));
    const char *end = query + strcspn(query, "#");
    hash = trace_hash_bytes(hash, url, (size_t) (query + 1 - url));
    for (const char *component = query + 1;;) {
        const char *stop = trace_query_component_stop(component, end);
        size_t name = trace_query_uuid_name_length(component, stop);
        hash = name != 0
            ? trace_hash_bytes(trace_hash_bytes(hash, component, name),
                               "\x01", 1)
            : trace_hash_bytes(hash, component,
                               (size_t) (stop - component));
        if (stop == end) break;
        hash = trace_hash_bytes(hash, "&", 1);
        component = stop + 1;
    }
    return trace_hash_bytes(hash, end, strlen(end));
}

/* The other half of the volatile-UUID opt-in: a server echoes the
   identifiers its client minted (an operation id in the URL, pending
   message ids in the form body) into the response, and the page discards
   a response whose identifiers are not its own.  Pair the UUIDs of the
   recorded request with the live request's -- same field, same text since
   the previous '&', '?' or UUID, same ordinal -- and rewrite each recorded
   UUID in the served header snapshot and body to its live partner.  UUIDs
   have one length, so offsets, lengths and the recorded-body hash check
   (computed over the retained bytes before rewriting) are unchanged.
   Any unpaired, conflicting or over-limit case serves the record as
   retained. */
#define TRACE_REKEY_UUID_LENGTH 36u
#define TRACE_REKEY_PAIR_LIMIT 16u
#define TRACE_REKEY_OCCURRENCE_LIMIT 512u
#define TRACE_REKEY_CONTEXT_LIMIT 96u

struct TraceReplayRekey {
    size_t pair_count;
    char from[TRACE_REKEY_PAIR_LIMIT][TRACE_REKEY_UUID_LENGTH];
    char to[TRACE_REKEY_PAIR_LIMIT][TRACE_REKEY_UUID_LENGTH];
    bool conflict;
    size_t occurrence_count;
    size_t occurrence_offset[TRACE_REKEY_OCCURRENCE_LIMIT];
    unsigned char occurrence_pair[TRACE_REKEY_OCCURRENCE_LIMIT];
};

/* A UUID at text[index] not embedded in a longer hexadecimal run. */
static bool trace_rekey_uuid_at(const char *text, size_t length,
                                size_t index)
{
    return length - index >= TRACE_REKEY_UUID_LENGTH
        && trace_query_value_is_uuid(text + index, TRACE_REKEY_UUID_LENGTH)
        && (index == 0 || !isxdigit((unsigned char) text[index - 1]))
        && (length - index == TRACE_REKEY_UUID_LENGTH
            || !isxdigit((unsigned char)
                             text[index + TRACE_REKEY_UUID_LENGTH]));
}

/* Finds the next UUID at or after *cursor and the text leading to it since
   the previous field separator or UUID. */
static bool trace_rekey_next(const char *text, size_t length,
                             size_t *cursor, size_t *uuid,
                             size_t *context)
{
    size_t start = *cursor;
    for (size_t index = *cursor; index < length; index++) {
        if (text[index] == '&' || text[index] == '?') {
            start = index + 1;
            continue;
        }
        if (!trace_rekey_uuid_at(text, length, index)) continue;
        *uuid = index;
        *context = start;
        *cursor = index + TRACE_REKEY_UUID_LENGTH;
        return true;
    }
    return false;
}

static void trace_rekey_add(TraceReplayRekey *rekey, const char *from,
                            const char *to)
{
    if (memcmp(from, to, TRACE_REKEY_UUID_LENGTH) == 0) return;
    for (size_t pair = 0; pair < rekey->pair_count; pair++) {
        bool same_from = memcmp(rekey->from[pair], from,
                                TRACE_REKEY_UUID_LENGTH) == 0;
        bool same_to = memcmp(rekey->to[pair], to,
                              TRACE_REKEY_UUID_LENGTH) == 0;
        if (same_from && same_to) return;
        if (same_from || same_to) {
            rekey->conflict = true;
            return;
        }
    }
    if (rekey->pair_count == TRACE_REKEY_PAIR_LIMIT) {
        rekey->conflict = true;
        return;
    }
    memcpy(rekey->from[rekey->pair_count], from, TRACE_REKEY_UUID_LENGTH);
    memcpy(rekey->to[rekey->pair_count], to, TRACE_REKEY_UUID_LENGTH);
    rekey->pair_count++;
}

/* Pairs the n-th recorded UUID behind a given context with the n-th live
   UUID behind the same context. */
static void trace_rekey_pair_text(TraceReplayRekey *rekey,
                                  const char *recorded,
                                  size_t recorded_length,
                                  const char *live, size_t live_length)
{
    size_t recorded_cursor = 0, recorded_uuid = 0, recorded_context = 0;
    while (!rekey->conflict
           && trace_rekey_next(recorded, recorded_length, &recorded_cursor,
                               &recorded_uuid, &recorded_context)) {
        size_t context_length = recorded_uuid - recorded_context;
        if (context_length > TRACE_REKEY_CONTEXT_LIMIT) continue;
        size_t ordinal = 0, cursor = 0, uuid = 0, context = 0;
        for (size_t scan = 0; trace_rekey_next(
                 recorded, recorded_length, &scan, &uuid, &context)
             && uuid < recorded_uuid;) {
            if (uuid - context == context_length
                && memcmp(recorded + context, recorded + recorded_context,
                          context_length) == 0) ordinal++;
        }
        while (trace_rekey_next(live, live_length, &cursor, &uuid,
                                &context)) {
            if (uuid - context != context_length
                || memcmp(live + context, recorded + recorded_context,
                          context_length) != 0) continue;
            if (ordinal-- != 0) continue;
            trace_rekey_add(rekey, recorded + recorded_uuid, live + uuid);
            break;
        }
    }
}

static void trace_replay_rekey_rewrite(const TraceReplayRekey *rekey,
                                       char *text, size_t length)
{
    for (size_t index = 0; rekey != NULL && index < length; index++) {
        if (!trace_rekey_uuid_at(text, length, index)) continue;
        for (size_t pair = 0; pair < rekey->pair_count; pair++) {
            if (memcmp(text + index, rekey->from[pair],
                       TRACE_REKEY_UUID_LENGTH) != 0) continue;
            memcpy(text + index, rekey->to[pair], TRACE_REKEY_UUID_LENGTH);
            break;
        }
    }
}

/* Rewrites the part of the retained body at [offset, offset + length)
   held in bytes; an occurrence may straddle chunk boundaries. */
static void trace_replay_rekey_apply(const TraceReplayRekey *rekey,
                                     size_t offset, unsigned char *bytes,
                                     size_t length)
{
    if (rekey == NULL) return;
    for (size_t index = 0; index < rekey->occurrence_count; index++) {
        size_t start = rekey->occurrence_offset[index];
        if (start >= offset + length) break;
        if (start + TRACE_REKEY_UUID_LENGTH <= offset) continue;
        const char *to = rekey->to[rekey->occurrence_pair[index]];
        size_t first = start > offset ? start : offset;
        size_t last = start + TRACE_REKEY_UUID_LENGTH < offset + length
            ? start + TRACE_REKEY_UUID_LENGTH : offset + length;
        for (size_t at = first; at < last; at++) {
            bytes[at - offset] = (unsigned char) to[at - start];
        }
    }
}

static void trace_replay_rekey_release(Budget *budget,
                                       TraceReplayRekey **rekey)
{
    if (rekey == NULL || *rekey == NULL) return;
    budget_free(budget, *rekey);
    *rekey = NULL;
}

/* Indexes the retained body's occurrences of paired UUIDs.  Leaves the
   file at offset zero, as trace_open_body_hash() does. */
static bool trace_replay_rekey_index_body(TraceReplayRekey *rekey,
                                          FILE *body, size_t length)
{
    enum { BLOCK = 4096 };
    char window[BLOCK + TRACE_REKEY_UUID_LENGTH + 1];
    size_t carried = 0, base = 0, first = 0, remaining = length;
    bool ok = true;
    while (ok && remaining != 0) {
        size_t wanted = remaining < BLOCK ? remaining : BLOCK;
        if (fread(window + carried, 1, wanted, body) != wanted) {
            ok = false;
            break;
        }
        remaining -= wanted;
        size_t filled = carried + wanted;
        /* Decide only positions whose UUID and trailing boundary byte are
           in the window, or which end the body. */
        size_t decided = remaining == 0 ? filled
                                        : filled - TRACE_REKEY_UUID_LENGTH;
        for (size_t index = first; ok && index < decided; index++) {
            if (!trace_rekey_uuid_at(window, filled, index)) continue;
            for (size_t pair = 0; pair < rekey->pair_count; pair++) {
                if (memcmp(window + index, rekey->from[pair],
                           TRACE_REKEY_UUID_LENGTH) != 0) continue;
                if (rekey->occurrence_count
                        == TRACE_REKEY_OCCURRENCE_LIMIT) {
                    ok = false;
                    break;
                }
                rekey->occurrence_offset[rekey->occurrence_count] =
                    base + index;
                rekey->occurrence_pair[rekey->occurrence_count] =
                    (unsigned char) pair;
                rekey->occurrence_count++;
                break;
            }
        }
        /* Carry the undecided tail behind one decided byte, which is the
           leading boundary of the next window's first position. */
        size_t keep_from = decided - 1;
        carried = filled - keep_from;
        memmove(window, window + keep_from, carried);
        base += keep_from;
        first = 1;
    }
    clearerr(body);
    if (fseek(body, 0, SEEK_SET) != 0) ok = false;
    return ok;
}

static TraceReplayRekey *trace_replay_rekey_build(
    Budget *budget, size_t sequence, TraceReplayScratch *scratch,
    const char *live_url, const FetchRequest *request,
    size_t recorded_request_length, uint64_t recorded_request_hash,
    bool has_request_hash, FetchResult *result, FILE *body,
    size_t body_length)
{
    if (!fetch_trace.replay_volatile_uuids) return NULL;
    TraceReplayRekey *rekey = budget_calloc(budget, 1, sizeof(*rekey));
    if (rekey == NULL) return NULL;
    const char *recorded_query = strchr(scratch->recorded_url, '?');
    const char *live_query = strchr(live_url, '?');
    if (recorded_query != NULL && live_query != NULL) {
        trace_rekey_pair_text(rekey, recorded_query, strlen(recorded_query),
                              live_query, strlen(live_query));
    }
    /* Capture keeps request bodies up to 16 KiB beside the record; use one
       only when it is exactly the body the record's hash names. */
    char *recorded_body = NULL;
    if (!rekey->conflict && request != NULL && request->body != NULL
        && request->body_length != 0 && has_request_hash
        && recorded_request_length != 0
        && recorded_request_length <= TRACE_CAPTURE_REQUEST_BODY_LIMIT
        && trace_path(scratch->meta_path, sizeof(scratch->meta_path),
                      sequence, "request")
        && (recorded_body = budget_malloc(
                budget, recorded_request_length)) != NULL) {
        FILE *file = fopen(scratch->meta_path, "rb");
        bool read = file != NULL
            && fread(recorded_body, 1, recorded_request_length, file)
                   == recorded_request_length
            && fgetc(file) == EOF && !ferror(file);
        if (file != NULL) (void) fclose(file);
        if (read && trace_body_hash(recorded_body, recorded_request_length)
                        == recorded_request_hash) {
            trace_rekey_pair_text(rekey, recorded_body,
                                  recorded_request_length, request->body,
                                  request->body_length);
        }
    }
    budget_free(budget, recorded_body);
    bool indexed = !rekey->conflict && rekey->pair_count != 0
        && trace_replay_rekey_index_body(rekey, body, body_length);
    if (rekey->pair_count != 0 || rekey->conflict) {
        trace_replay_decision(
            "HTTP replay volatile-uuid record=%zu pairs=%zu "
            "occurrences=%zu conflict=%d applied=%d",
            sequence, rekey->pair_count, rekey->occurrence_count,
            (int) rekey->conflict, (int) indexed);
    }
    if (!indexed) {
        trace_replay_rekey_release(budget, &rekey);
        return NULL;
    }
    trace_replay_rekey_rewrite(rekey, result->response_headers,
                               result->response_headers_length);
    return rekey;
}

static bool trace_parse_security_metadata(
    const char *value, FetchResponseSecurityMetadata *metadata)
{
    if (value == NULL || metadata == NULL) return false;
    unsigned version = 0, allow_origin = 0, allow_credentials = 0;
    unsigned credentials_value = 0, corp_state = 0, corp = 0;
    unsigned nosniff_state = 0, nosniff = 0, hsts_state = 0;
    unsigned hsts_include = 0, referrer = 0, csp = 0, frame = 0;
    unsigned snapshot_truncated = 0;
    unsigned long long hsts_age = 0;
    char trailing = '\0';
    if (sscanf(
            value,
            "%u,%u,%u,%u,%u,%u,%u,%u,%u,%llu,%u,%u,%u,%u,%u%c",
            &version, &allow_origin, &allow_credentials,
            &credentials_value, &corp_state, &corp, &nosniff_state,
            &nosniff, &hsts_state, &hsts_age, &hsts_include, &referrer,
            &csp, &frame, &snapshot_truncated, &trailing) != 15
        || version != FETCH_RESPONSE_SECURITY_METADATA_VERSION
        || allow_origin > FETCH_SECURITY_FIELD_TRUNCATED
        || allow_credentials > FETCH_SECURITY_FIELD_TRUNCATED
        || credentials_value > 1u
        || corp_state > FETCH_SECURITY_FIELD_TRUNCATED
        || corp > TILEFINCH_CORP_CROSS_ORIGIN
        || nosniff_state > FETCH_SECURITY_FIELD_TRUNCATED
        || nosniff > 1u || hsts_state > FETCH_SECURITY_FIELD_TRUNCATED
        || hsts_include > 1u || referrer > FETCH_SECURITY_FIELD_TRUNCATED
        || csp > FETCH_SECURITY_FIELD_TRUNCATED
        || frame > FETCH_SECURITY_FIELD_TRUNCATED
        || snapshot_truncated > 1u) return false;
    fetch_response_security_metadata_reset(metadata);
    metadata->allow_origin_state = (FetchSecurityFieldState) allow_origin;
    metadata->allow_credentials_state =
        (FetchSecurityFieldState) allow_credentials;
    metadata->allow_credentials = credentials_value != 0;
    metadata->corp_state = (FetchSecurityFieldState) corp_state;
    metadata->corp = (TilefinchCrossOriginResourcePolicy) corp;
    metadata->nosniff_state = (FetchSecurityFieldState) nosniff_state;
    metadata->nosniff = nosniff != 0;
    metadata->hsts_state = (FetchSecurityFieldState) hsts_state;
    metadata->hsts_max_age = (uint64_t) hsts_age;
    metadata->hsts_include_subdomains = hsts_include != 0;
    metadata->referrer_policy_state = (FetchSecurityFieldState) referrer;
    metadata->csp_snapshot_state = (FetchSecurityFieldState) csp;
    metadata->frame_policy_snapshot_state =
        (FetchSecurityFieldState) frame;
    metadata->snapshot_truncated = snapshot_truncated != 0;
    return true;
}

static bool trace_replay_response_with_scratch(
    Budget *budget, const char *method, const char *url,
    const char *logical_url,
    const FetchRequest *request, const char *effective_user_agent,
    bool diagnostic_mobile_safari,
    size_t maximum_bytes, FetchResult *result, TraceReplayScratch *scratch,
    WriteContext *writer, size_t sequence, bool require_request_identity,
    TraceReplayRecordStatus *record_status, bool *rejected)
{
    if (record_status == NULL || rejected == NULL) return false;
    *record_status = TRACE_REPLAY_RECORD_INVALID;
    *rejected = false;
    if (!trace_extra_header_shape_with_cache(
            request,
            scratch->request_extra_header_shape,
            sizeof(scratch->request_extra_header_shape))) return false;
    if (!trace_path(scratch->meta_path, sizeof(scratch->meta_path), sequence,
                    "meta")
        || !trace_path(scratch->body_path, sizeof(scratch->body_path),
                       sequence, "body")) {
        return false;
    }
    uint64_t meta_started_ns = tilefinch_platform_monotonic_time_ns();
    FILE *meta = fopen(scratch->meta_path, "rb");
    if (meta == NULL) {
        snprintf(result->error, sizeof(result->error),
                 "HTTP replay record %zu is missing for %.12s %.420s",
                 sequence, method, url);
        return false;
    }
    unsigned trace_version = 0;
    bool method_match = false, url_match = false, url_exact = false;
    bool logical_url_seen = false, logical_url_match = false;
    bool recorded_success = true;
    size_t recorded_length = SIZE_MAX, cookie_count = 0;
    size_t recorded_header_count = SIZE_MAX, parsed_header_count = 0;
    size_t recorded_request_length = SIZE_MAX;
    size_t recorded_cookie_bytes = SIZE_MAX;
    size_t recorded_extra_header_bytes = SIZE_MAX;
    uint64_t recorded_request_hash = 0, recorded_response_hash = 0;
    bool has_request_hash = false, has_response_hash = false;
    int recorded_client_hints = -1, recorded_low_client_hints = -1;
    int recorded_fetch_user = -1, recorded_upgrade_insecure = -1;
    int recorded_credentials = -1, recorded_cf_clearance = -1;
    int recorded_allow_http_errors = -1;
    int recorded_enforce_cors = -1;
    int recorded_redirect_same_origin_only = -1;
    int recorded_cors_cached_response_validated = -1;
    int recorded_diagnostic_mobile_safari = -1;
    int recorded_referrer_policy_metadata_valid = -1;
    int recorded_referrer_policy_present = -1;
    int recorded_security_headers_truncated = -1;
    int recorded_set_cookies_truncated = -1;
    bool recorded_security_metadata_seen = false;
    bool recorded_security_allow_origin_seen = false;
    FetchResponseSecurityMetadata recorded_security_metadata = {0};
    bool recorded_referrer_policy_seen = false;
    bool redirect_origin_tainted_seen = false;
    char recorded_credential_origin[2048] = {0};
    bool has_recorded_credential_origin = false;
    bool has_request_initiator_url = false;
    bool has_request_referrer_source = false;
    bool has_request_referrer_policy = false;
    bool has_request_content_type = false, request_content_type_match = false;
    bool has_request_extra_shape = false;
    bool request_extra_shape_with_cache_match = false;
    bool request_extra_shape_legacy_match = false;
    bool has_request_if_none_match = false, request_if_none_match_match = false;
    bool has_request_if_modified_since = false;
    bool request_if_modified_since_match = false;
    bool has_request_referer = false, request_referer_match = false;
    bool request_referer_same_origin_race = false;
    bool has_request_origin = false, request_origin_match = false;
    bool has_request_accept = false, request_accept_match = false;
    bool has_request_fetch_dest = false, request_fetch_dest_match = false;
    bool has_request_fetch_mode = false, request_fetch_mode_match = false;
    bool has_request_fetch_site = false, request_fetch_site_match = false;
    bool has_request_client_hint_tokens = false;
    bool request_client_hint_tokens_match = false;
    bool has_request_client_hint_origin = false;
    bool request_client_hint_origin_match = false;
    bool has_request_user_agent = false, request_user_agent_match = false;
    bool trace_fields_valid = true;
    bool indexed_fields_valid = true;
    bool cookie_value_seen[FETCH_RESPONSE_COOKIE_CAPACITY] = {false};
    bool cookie_url_seen[FETCH_RESPONSE_COOKIE_CAPACITY] = {false};
    uint64_t v6_fields_seen = 0;
#define TRACE_SEE(field) do {                                                \
    if (!trace_mark_field(&v6_fields_seen, (field))) {                       \
        trace_fields_valid = false;                                         \
    }                                                                       \
} while (0)
    while (fgets(scratch->line, sizeof(scratch->line), meta) != NULL) {
        size_t length = strlen(scratch->line);
        while (length != 0
               && (scratch->line[length - 1] == '\n'
                   || scratch->line[length - 1] == '\r')) {
            scratch->line[--length] = '\0';
        }
        char *equals = strchr(scratch->line, '=');
        if (equals == NULL) {
            trace_fields_valid = false;
            continue;
        }
        *equals++ = '\0';
        if (strcmp(scratch->line, "psp-http-trace") == 0) {
            TRACE_SEE(TRACE_V6_VERSION);
            size_t parsed = 0;
            if (trace_parse_size_value(equals, &parsed)
                && parsed >= 1 && parsed <= 13) {
                trace_version = (unsigned) parsed;
            } else {
                trace_fields_valid = false;
            }
        }
        else if (strcmp(scratch->line, "cookie-values") == 0) {
            TRACE_SEE(TRACE_V6_COOKIE_VALUES);
            if (strcmp(equals, "raw") != 0
                && strcmp(equals, "redacted") != 0) {
                trace_fields_valid = false;
            }
        }
        else if (strcmp(scratch->line, "method") == 0) {
            TRACE_SEE(TRACE_V6_METHOD);
            method_match = strcmp(equals, method) == 0;
        }
        else if (strcmp(scratch->line, "url") == 0) {
            TRACE_SEE(TRACE_V6_URL);
            url_match = trace_replay_url_equal(equals, url);
            url_exact = strcmp(equals, url) == 0;
            if (fetch_trace.replay_volatile_uuids) {
                snprintf(scratch->recorded_url,
                         sizeof(scratch->recorded_url), "%s", equals);
            }
        }
        else if (strcmp(scratch->line, "logical-request-url") == 0) {
            TRACE_SEE(TRACE_V9_LOGICAL_REQUEST_URL);
            logical_url_seen = true;
            bool recorded_logical_valid = tilefinch_url_normalize(
                    equals, scratch->logical_url,
                    sizeof(scratch->logical_url))
                && strcmp(equals, scratch->logical_url) == 0
                && tilefinch_url_request_key(
                       scratch->logical_url, scratch->request_key,
                       sizeof(scratch->request_key))
                && trace_replay_url_equal(scratch->request_key, url);
            if (!recorded_logical_valid) trace_fields_valid = false;
            logical_url_match = recorded_logical_valid
                && trace_replay_url_equal(scratch->logical_url, logical_url);
        }
        else if (strcmp(scratch->line, "success") == 0) {
            TRACE_SEE(TRACE_V6_SUCCESS);
            int parsed = 0;
            if (!trace_parse_bool_value(equals, &parsed)) {
                trace_fields_valid = false;
            } else {
                recorded_success = parsed != 0;
            }
        }
        else if (strcmp(scratch->line, "async-delay-pumps") == 0) {
            TRACE_SEE(TRACE_V6_ASYNC_DELAY);
            if (!trace_parse_size_value(equals,
                                        &result->trace_delay_pumps)) {
                trace_fields_valid = false;
            }
        }
        else if (strcmp(scratch->line, "external-cancel") == 0) {
            TRACE_SEE(TRACE_V6_EXTERNAL_CANCEL);
            int parsed = 0;
            if (!trace_parse_bool_value(equals, &parsed)) {
                trace_fields_valid = false;
            } else {
                result->trace_external_cancel = parsed != 0;
            }
        }
        else if (strcmp(scratch->line, "transport-timeout") == 0) {
            TRACE_SEE(TRACE_V6_TRANSPORT_TIMEOUT);
            int parsed = 0;
            if (!trace_parse_bool_value(equals, &parsed)) {
                trace_fields_valid = false;
            } else {
                result->timed_out = parsed != 0;
            }
        }
        else if (strcmp(scratch->line, "redirect-origin-tainted") == 0) {
            TRACE_SEE(TRACE_V8_REDIRECT_ORIGIN_TAINTED);
            int parsed = 0;
            if (redirect_origin_tainted_seen
                || !trace_parse_bool_value(equals, &parsed)) {
                trace_fields_valid = false;
            } else {
                redirect_origin_tainted_seen = true;
                result->redirect_origin_tainted = parsed != 0;
            }
        }
        else if (strcmp(scratch->line, "error") == 0) {
            TRACE_SEE(TRACE_V6_ERROR);
            if (!trace_copy_value(result->error, sizeof(result->error),
                                  equals)) trace_fields_valid = false;
        }
        else if (strcmp(scratch->line, "request-body-length") == 0) {
            TRACE_SEE(TRACE_V6_REQUEST_BODY_LENGTH);
            if (!trace_parse_size_value(equals, &recorded_request_length))
                trace_fields_valid = false;
        }
        else if (strcmp(scratch->line, "request-body-hash") == 0) {
            TRACE_SEE(TRACE_V6_REQUEST_BODY_HASH);
            has_request_hash = trace_parse_hash_value(
                equals, &recorded_request_hash);
            if (!has_request_hash) trace_fields_valid = false;
        }
        else if (strcmp(scratch->line, "request-content-type") == 0) {
            TRACE_SEE(TRACE_V6_REQUEST_CONTENT_TYPE);
            has_request_content_type = true;
            request_content_type_match = strcmp(
                equals, request == NULL || request->content_type == NULL
                            ? "" : request->content_type) == 0;
        }
        else if (strcmp(scratch->line, "request-cookie-bytes") == 0) {
            TRACE_SEE(TRACE_V6_REQUEST_COOKIE_BYTES);
            if (!trace_parse_size_value(equals, &recorded_cookie_bytes))
                trace_fields_valid = false;
        }
        else if (strcmp(scratch->line, "request-has-cf-clearance") == 0) {
            TRACE_SEE(TRACE_V6_REQUEST_CF_CLEARANCE);
            if (!trace_parse_bool_value(equals, &recorded_cf_clearance))
                trace_fields_valid = false;
        }
        else if (strcmp(scratch->line, "request-extra-header-bytes") == 0) {
            TRACE_SEE(TRACE_V6_REQUEST_EXTRA_BYTES);
            if (!trace_parse_size_value(equals,
                                        &recorded_extra_header_bytes)) {
                trace_fields_valid = false;
            }
        }
        else if (strcmp(scratch->line, "request-extra-header-shape") == 0) {
            TRACE_SEE(TRACE_V6_REQUEST_EXTRA_SHAPE);
            has_request_extra_shape = true;
            const char *separator = strchr(
                scratch->request_extra_header_shape, ';');
            const char *legacy_expected = separator == NULL
                ? scratch->request_extra_header_shape : separator + 1;
            /* Metadata fields are keyed and may be reordered. Record both
               interpretations here, then select one after the final version
               is known instead of depending on psp-http-trace appearing
               first in the file. */
            request_extra_shape_with_cache_match = strcmp(
                equals, scratch->request_extra_header_shape) == 0;
            request_extra_shape_legacy_match = strcmp(
                equals, legacy_expected) == 0;
        }
        else if (strcmp(scratch->line, "request-allow-http-errors") == 0) {
            TRACE_SEE(TRACE_V6_REQUEST_ALLOW_ERRORS);
            if (!trace_parse_bool_value(equals,
                                        &recorded_allow_http_errors)) {
                trace_fields_valid = false;
            }
        }
        else if (strcmp(scratch->line, "request-enforce-cors") == 0) {
            TRACE_SEE(TRACE_V8_REQUEST_ENFORCE_CORS);
            if (!trace_parse_bool_value(equals, &recorded_enforce_cors)) {
                trace_fields_valid = false;
            }
        }
        else if (strcmp(scratch->line,
                        "request-redirect-same-origin-only") == 0) {
            TRACE_SEE(TRACE_V8_REQUEST_REDIRECT_SAME_ORIGIN_ONLY);
            if (!trace_parse_bool_value(
                    equals, &recorded_redirect_same_origin_only)) {
                trace_fields_valid = false;
            }
        }
        else if (strcmp(
                     scratch->line,
                     "request-cors-cached-response-validated") == 0) {
            TRACE_SEE(TRACE_V8_REQUEST_CORS_CACHED_RESPONSE_VALIDATED);
            if (!trace_parse_bool_value(
                    equals,
                    &recorded_cors_cached_response_validated)) {
                trace_fields_valid = false;
            }
        }
        else if (strcmp(scratch->line, "request-if-none-match") == 0) {
            TRACE_SEE(TRACE_V6_REQUEST_IF_NONE_MATCH);
            has_request_if_none_match = true;
            request_if_none_match_match = strcmp(
                equals, request == NULL || request->if_none_match == NULL
                            ? "" : request->if_none_match) == 0;
        }
        else if (strcmp(scratch->line, "request-if-modified-since") == 0) {
            TRACE_SEE(TRACE_V6_REQUEST_IF_MODIFIED_SINCE);
            has_request_if_modified_since = true;
            request_if_modified_since_match = strcmp(
                equals,
                request == NULL || request->if_modified_since == NULL
                    ? "" : request->if_modified_since) == 0;
        }
        else if (strcmp(scratch->line, "request-referer") == 0) {
            TRACE_SEE(TRACE_V6_REQUEST_REFERER);
            has_request_referer = true;
            /* Engine-supplemental capture passes (an older lab enriching a
               trace with media bodies) recorded these request headers
               empty, while the pipeline now populates them.  A recorded
               empty value therefore acts as a wildcard for this and the
               four header fields below; a recorded non-empty value still
               requires an exact match. */
            request_referer_match = equals[0] == '\0' || strcmp(
                equals, request == NULL || request->referer == NULL
                            ? "" : request->referer) == 0;
            /* Which same-origin importer fetches a shared module first is
               a module-map race in the capture browser, not a semantic
               property of the record; remember whether both referers are
               same-origin with the record so script-dest matching below
               can tolerate the race. */
            request_referer_same_origin_race =
                !request_referer_match && equals[0] != '\0'
                && request != NULL && request->referer != NULL
                && tilefinch_url_same_origin(equals, url)
                && tilefinch_url_same_origin(request->referer, url);
        }
        else if (strcmp(scratch->line, "request-origin") == 0) {
            TRACE_SEE(TRACE_V6_REQUEST_ORIGIN);
            has_request_origin = true;
            request_origin_match = strcmp(
                equals, request == NULL || request->origin == NULL
                            ? "" : request->origin) == 0;
        }
        else if (strcmp(scratch->line, "request-accept") == 0) {
            TRACE_SEE(TRACE_V6_REQUEST_ACCEPT);
            has_request_accept = true;
            request_accept_match = equals[0] == '\0' || strcmp(
                equals, request == NULL || request->accept == NULL
                            ? "" : request->accept) == 0;
        }
        else if (strcmp(scratch->line, "request-sec-fetch-dest") == 0) {
            TRACE_SEE(TRACE_V6_REQUEST_FETCH_DEST);
            has_request_fetch_dest = true;
            request_fetch_dest_match = equals[0] == '\0' || strcmp(
                equals, request == NULL || request->sec_fetch_dest == NULL
                            ? "" : request->sec_fetch_dest) == 0;
        }
        else if (strcmp(scratch->line, "request-sec-fetch-mode") == 0) {
            TRACE_SEE(TRACE_V6_REQUEST_FETCH_MODE);
            has_request_fetch_mode = true;
            request_fetch_mode_match = equals[0] == '\0' || strcmp(
                equals, request == NULL || request->sec_fetch_mode == NULL
                            ? "" : request->sec_fetch_mode) == 0;
        }
        else if (strcmp(scratch->line, "request-sec-fetch-site") == 0) {
            TRACE_SEE(TRACE_V6_REQUEST_FETCH_SITE);
            has_request_fetch_site = true;
            request_fetch_site_match = equals[0] == '\0' || strcmp(
                equals, request == NULL || request->sec_fetch_site == NULL
                            ? "" : request->sec_fetch_site) == 0;
        }
        else if (strcmp(scratch->line, "request-send-client-hints") == 0) {
            TRACE_SEE(TRACE_V6_REQUEST_CLIENT_HINTS);
            if (!trace_parse_bool_value(equals, &recorded_client_hints))
                trace_fields_valid = false;
        }
        else if (strcmp(scratch->line,
                        "request-client-hint-tokens") == 0) {
            TRACE_SEE(TRACE_V7_REQUEST_CLIENT_HINT_TOKENS);
            has_request_client_hint_tokens = trace_copy_value(
                scratch->request_client_hint_tokens,
                sizeof(scratch->request_client_hint_tokens), equals);
            request_client_hint_tokens_match =
                has_request_client_hint_tokens
                && strcmp(
                    scratch->request_client_hint_tokens,
                    request == NULL || request->client_hint_tokens == NULL
                        ? "" : request->client_hint_tokens) == 0;
            if (!has_request_client_hint_tokens) trace_fields_valid = false;
        }
        else if (strcmp(scratch->line,
                        "request-client-hint-origin") == 0) {
            TRACE_SEE(TRACE_V7_REQUEST_CLIENT_HINT_ORIGIN);
            has_request_client_hint_origin = trace_copy_value(
                scratch->request_client_hint_origin,
                sizeof(scratch->request_client_hint_origin), equals);
            request_client_hint_origin_match = has_request_client_hint_origin
                && strcmp(
                    scratch->request_client_hint_origin,
                    request == NULL || request->client_hint_origin == NULL
                        ? "" : request->client_hint_origin) == 0;
            if (!has_request_client_hint_origin) trace_fields_valid = false;
        }
        else if (strcmp(scratch->line, "request-send-low-client-hints") == 0) {
            TRACE_SEE(TRACE_V6_REQUEST_LOW_CLIENT_HINTS);
            if (!trace_parse_bool_value(equals,
                                        &recorded_low_client_hints)) {
                trace_fields_valid = false;
            }
        }
        else if (strcmp(scratch->line, "request-sec-fetch-user") == 0) {
            TRACE_SEE(TRACE_V6_REQUEST_FETCH_USER);
            if (!trace_parse_bool_value(equals, &recorded_fetch_user))
                trace_fields_valid = false;
        }
        else if (strcmp(scratch->line, "request-upgrade-insecure") == 0) {
            TRACE_SEE(TRACE_V6_REQUEST_UPGRADE_INSECURE);
            if (!trace_parse_bool_value(equals,
                                        &recorded_upgrade_insecure)) {
                trace_fields_valid = false;
            }
        }
        else if (strcmp(scratch->line, "request-user-agent") == 0) {
            TRACE_SEE(TRACE_V6_REQUEST_USER_AGENT);
            has_request_user_agent = true;
            request_user_agent_match = strcmp(
                equals, effective_user_agent == NULL
                            ? "" : effective_user_agent) == 0;
        }
        else if (strcmp(scratch->line,
                        "request-diagnostic-mobile-safari") == 0) {
            TRACE_SEE(TRACE_V6_REQUEST_DIAGNOSTIC_SAFARI);
            if (!trace_parse_bool_value(
                    equals, &recorded_diagnostic_mobile_safari)) {
                trace_fields_valid = false;
            }
        }
        else if (strcmp(scratch->line, "request-credentials") == 0) {
            TRACE_SEE(TRACE_V6_REQUEST_CREDENTIALS);
            size_t parsed = 0;
            if (!trace_parse_size_value(equals, &parsed)
                || parsed > (size_t) FETCH_CREDENTIALS_SAME_ORIGIN) {
                trace_fields_valid = false;
            } else {
                recorded_credentials = (int) parsed;
            }
        }
        else if (strcmp(scratch->line, "request-credential-origin") == 0) {
            TRACE_SEE(TRACE_V6_REQUEST_CREDENTIAL_ORIGIN);
            has_recorded_credential_origin = trace_copy_value(
                recorded_credential_origin,
                sizeof(recorded_credential_origin), equals);
            if (!has_recorded_credential_origin) trace_fields_valid = false;
        }
        else if (strcmp(scratch->line, "request-initiator-url") == 0) {
            TRACE_SEE(TRACE_V6_REQUEST_INITIATOR_URL);
            has_request_initiator_url = trace_copy_value(
                scratch->request_initiator_url,
                sizeof(scratch->request_initiator_url), equals);
            if (!has_request_initiator_url) trace_fields_valid = false;
        }
        else if (strcmp(scratch->line, "request-referrer-source") == 0) {
            TRACE_SEE(TRACE_V6_REQUEST_REFERRER_SOURCE);
            has_request_referrer_source = trace_copy_value(
                scratch->request_referrer_source,
                sizeof(scratch->request_referrer_source), equals);
            if (!has_request_referrer_source) trace_fields_valid = false;
        }
        else if (strcmp(scratch->line, "request-referrer-policy") == 0) {
            TRACE_SEE(TRACE_V6_REQUEST_REFERRER_POLICY);
            has_request_referrer_policy = trace_copy_value(
                scratch->request_referrer_policy,
                sizeof(scratch->request_referrer_policy), equals);
            if (!has_request_referrer_policy) trace_fields_valid = false;
        }
        else if (strcmp(scratch->line, "status") == 0) {
            TRACE_SEE(TRACE_V6_STATUS);
            size_t parsed = 0;
            if (!trace_parse_size_value(equals, &parsed)
                || parsed > (size_t) LONG_MAX) {
                trace_fields_valid = false;
            } else {
                result->status_code = (long) parsed;
            }
        }
        else if (strcmp(scratch->line, "length") == 0) {
            TRACE_SEE(TRACE_V6_LENGTH);
            if (!trace_parse_size_value(equals, &recorded_length))
                trace_fields_valid = false;
        }
        else if (strcmp(scratch->line, "response-body-hash") == 0) {
            TRACE_SEE(TRACE_V6_RESPONSE_BODY_HASH);
            has_response_hash = trace_parse_hash_value(
                equals, &recorded_response_hash);
            if (!has_response_hash) trace_fields_valid = false;
        }
        else if (strcmp(scratch->line, "effective-url") == 0) {
            TRACE_SEE(TRACE_V6_EFFECTIVE_URL);
            if (!trace_copy_value(result->effective_url,
                                  sizeof(result->effective_url), equals)) {
                trace_fields_valid = false;
            }
        }
        else if (strcmp(scratch->line, "content-type") == 0) {
            TRACE_SEE(TRACE_V6_CONTENT_TYPE);
            if (!trace_copy_value(result->content_type,
                                  sizeof(result->content_type), equals)) {
                trace_fields_valid = false;
            }
        }
        else if (strcmp(scratch->line, "etag") == 0) {
            TRACE_SEE(TRACE_V6_ETAG);
            if (!trace_copy_value(result->etag, sizeof(result->etag), equals))
                trace_fields_valid = false;
        }
        else if (strcmp(scratch->line, "last-modified") == 0) {
            TRACE_SEE(TRACE_V6_LAST_MODIFIED);
            if (!trace_copy_value(result->last_modified,
                                  sizeof(result->last_modified), equals)) {
                trace_fields_valid = false;
            }
        }
        else if (strcmp(scratch->line, "cf-mitigated") == 0) {
            TRACE_SEE(TRACE_V6_CF_MITIGATED);
            if (!trace_copy_value(result->cf_mitigated,
                                  sizeof(result->cf_mitigated), equals)) {
                trace_fields_valid = false;
            }
        }
        else if (strcmp(scratch->line, "accept-ch") == 0) {
            TRACE_SEE(TRACE_V6_ACCEPT_CH);
            if (!trace_copy_value(result->accept_ch,
                                  sizeof(result->accept_ch), equals)) {
                trace_fields_valid = false;
            }
        }
        else if (strcmp(scratch->line, "critical-ch") == 0) {
            TRACE_SEE(TRACE_V6_CRITICAL_CH);
            if (!trace_copy_value(result->critical_ch,
                                  sizeof(result->critical_ch), equals)) {
                trace_fields_valid = false;
            }
        }
        else if (strcmp(scratch->line, "server") == 0) {
            TRACE_SEE(TRACE_V6_SERVER);
            if (!trace_copy_value(result->server, sizeof(result->server),
                                  equals)) trace_fields_valid = false;
        }
        else if (strcmp(scratch->line, "cf-ray") == 0) {
            TRACE_SEE(TRACE_V6_CF_RAY);
            if (!trace_copy_value(result->cf_ray, sizeof(result->cf_ray),
                                  equals)) trace_fields_valid = false;
        }
        else if (strcmp(
                     scratch->line,
                     "response-referrer-policy-metadata-valid") == 0) {
            TRACE_SEE(TRACE_V9_RESPONSE_REFERRER_POLICY_METADATA_VALID);
            if (!trace_parse_bool_value(
                    equals, &recorded_referrer_policy_metadata_valid)) {
                trace_fields_valid = false;
            } else {
                result->response_referrer_policy_metadata_valid =
                    recorded_referrer_policy_metadata_valid != 0;
            }
        }
        else if (strcmp(
                     scratch->line,
                     "response-referrer-policy-present") == 0) {
            TRACE_SEE(TRACE_V9_RESPONSE_REFERRER_POLICY_PRESENT);
            if (!trace_parse_bool_value(
                    equals, &recorded_referrer_policy_present)) {
                trace_fields_valid = false;
            } else {
                result->response_referrer_policy_header_present =
                    recorded_referrer_policy_present != 0;
            }
        }
        else if (strcmp(scratch->line,
                        "response-referrer-policy") == 0) {
            TRACE_SEE(TRACE_V9_RESPONSE_REFERRER_POLICY);
            recorded_referrer_policy_seen = trace_copy_value(
                result->response_referrer_policy,
                sizeof(result->response_referrer_policy), equals);
            if (!recorded_referrer_policy_seen) trace_fields_valid = false;
        }
        else if (strcmp(
                     scratch->line,
                     "response-security-headers-truncated") == 0) {
            TRACE_SEE(TRACE_V10_RESPONSE_SECURITY_HEADERS_TRUNCATED);
            if (!trace_parse_bool_value(
                    equals, &recorded_security_headers_truncated)) {
                trace_fields_valid = false;
            } else {
                result->response_security_headers_truncated =
                    recorded_security_headers_truncated != 0;
            }
        }
        else if (strcmp(scratch->line,
                        "response-security-metadata") == 0) {
            TRACE_SEE(TRACE_V11_RESPONSE_SECURITY_METADATA);
            recorded_security_metadata_seen = trace_parse_security_metadata(
                equals, &recorded_security_metadata);
            if (!recorded_security_metadata_seen) trace_fields_valid = false;
        }
        else if (strcmp(scratch->line,
                        "response-security-allow-origin") == 0) {
            TRACE_SEE(TRACE_V11_RESPONSE_SECURITY_ALLOW_ORIGIN);
            recorded_security_allow_origin_seen = trace_copy_value(
                recorded_security_metadata.allow_origin,
                sizeof(recorded_security_metadata.allow_origin), equals);
            if (!recorded_security_allow_origin_seen) {
                trace_fields_valid = false;
            }
        }
        else if (strcmp(scratch->line, "response-header-count") == 0) {
            TRACE_SEE(TRACE_V6_RESPONSE_HEADER_COUNT);
            if (!trace_parse_size_value(equals, &recorded_header_count)) {
                trace_fields_valid = false;
            }
        }
        else if (strncmp(scratch->line, "response-header-", 16) == 0) {
            size_t index = 0;
            if (!trace_parse_size_value(scratch->line + 16, &index)
                || index != parsed_header_count) {
                indexed_fields_valid = false;
            }
            size_t value_length = strlen(equals);
            if (value_length + 1 < sizeof(result->response_headers)
                                   - result->response_headers_length) {
                memcpy(result->response_headers
                           + result->response_headers_length,
                       equals, value_length);
                result->response_headers_length += value_length;
                result->response_headers[result->response_headers_length++] =
                    '\n';
                result->response_headers[result->response_headers_length] =
                    '\0';
                parsed_header_count++;
            } else {
                indexed_fields_valid = false;
            }
        }
        else if (strcmp(scratch->line, "set-cookie-count") == 0) {
            TRACE_SEE(TRACE_V6_SET_COOKIE_COUNT);
            if (!trace_parse_size_value(equals, &cookie_count)) {
                trace_fields_valid = false;
            }
        }
        else if (strcmp(scratch->line, "set-cookies-truncated") == 0) {
            TRACE_SEE(TRACE_V12_SET_COOKIES_TRUNCATED);
            if (!trace_parse_bool_value(
                    equals, &recorded_set_cookies_truncated)) {
                trace_fields_valid = false;
            }
        }
        else if (strncmp(scratch->line, "set-cookie-url-", 15) == 0) {
            size_t index = 0;
            bool index_ok = trace_parse_size_value(
                scratch->line + 15, &index)
                && index < FETCH_RESPONSE_COOKIE_CAPACITY;
            size_t url_length = strlen(equals);
            if (!index_ok || (index_ok && cookie_url_seen[index])) {
                indexed_fields_valid = false;
            }
            if (index_ok && url_length != 0 && url_length < 2048) {
                char *copy = budget_malloc(budget, url_length + 1);
                if (copy != NULL) {
                    memcpy(copy, equals, url_length + 1);
                    budget_free(budget, result->set_cookie_urls[index]);
                    result->set_cookie_urls[index] = copy;
                    cookie_url_seen[index] = true;
                } else {
                    indexed_fields_valid = false;
                }
            } else {
                indexed_fields_valid = false;
            }
        }
        else if (strncmp(scratch->line, "set-cookie-", 11) == 0) {
            size_t index = 0;
            bool index_ok = trace_parse_size_value(
                scratch->line + 11, &index)
                && index < FETCH_RESPONSE_COOKIE_CAPACITY;
            if (!index_ok || (index_ok && cookie_value_seen[index])) {
                indexed_fields_valid = false;
            }
            size_t cookie_length = strlen(equals);
            char *copy = NULL;
            if (index_ok && !cookie_value_seen[index]
                && cookie_length < FETCH_SET_COOKIE_LIMIT) {
                copy = budget_malloc(budget, cookie_length + 1);
                if (copy != NULL) memcpy(copy, equals, cookie_length + 1);
            }
            if (copy == NULL) {
                indexed_fields_valid = false;
            } else {
                result->set_cookies[index] = copy;
                cookie_value_seen[index] = true;
                if (result->set_cookie_count <= index) {
                    result->set_cookie_count = index + 1;
                }
            }
        }
    }
#undef TRACE_SEE
    bool cookie_values_complete = true;
    bool cookie_urls_complete = true;
    for (size_t i = 0; i < result->set_cookie_count; i++) {
        if (result->set_cookies[i] == NULL) cookie_values_complete = false;
        if (result->set_cookie_urls[i] == NULL) cookie_urls_complete = false;
    }
    bool v6_cookie_indices_complete = true;
    for (size_t i = 0; i < FETCH_RESPONSE_COOKIE_CAPACITY; i++) {
        bool expected = i < cookie_count;
        if (cookie_value_seen[i] != expected
            || cookie_url_seen[i] != expected) {
            v6_cookie_indices_complete = false;
        }
    }
    bool meta_ok = !ferror(meta) && fclose(meta) == 0;
    TRACE_REPLAY_IO_OPEN(meta_started_ns);
    size_t request_length = request == NULL ? 0 : request->body_length;
    const char *request_body = request == NULL || request->body == NULL
                               ? "" : request->body;
    uint64_t request_hash = trace_version == 1
        ? trace_body_hash_v1(request_body, request_length)
        : trace_body_hash(request_body, request_length);
    size_t request_cookie_bytes = request == NULL || request->cookie == NULL
        ? 0 : strlen(request->cookie);
    size_t request_extra_header_bytes =
        request == NULL || request->extra_headers == NULL
            ? 0 : strlen(request->extra_headers);
    bool request_has_cf_clearance = request != NULL
        && request->cookie != NULL
        && strstr(request->cookie, "cf_clearance=") != NULL;
    uint64_t v6_required_fields = trace_required_field_mask(
        (unsigned) TRACE_V6_FIELD_COUNT);
    bool v6_complete = trace_version < 6
        || (trace_fields_valid
            && indexed_fields_valid
            && (v6_fields_seen & v6_required_fields) == v6_required_fields
            && v6_cookie_indices_complete
            && recorded_request_length != SIZE_MAX && has_request_hash
            && has_request_content_type
            && recorded_cookie_bytes != SIZE_MAX
            && recorded_cf_clearance >= 0
            && recorded_extra_header_bytes != SIZE_MAX
            && has_request_extra_shape && recorded_allow_http_errors >= 0
            && has_request_if_none_match && has_request_if_modified_since
            && has_request_referer && has_request_origin
            && has_request_accept && has_request_fetch_dest
            && has_request_fetch_mode && has_request_fetch_site
            && recorded_client_hints >= 0
            && recorded_low_client_hints >= 0 && recorded_fetch_user >= 0
            && recorded_upgrade_insecure >= 0 && has_request_user_agent
            && recorded_diagnostic_mobile_safari >= 0
            && recorded_credentials >= 0
            && has_recorded_credential_origin
            && has_request_initiator_url
            && has_request_referrer_source
            && has_request_referrer_policy && recorded_length != SIZE_MAX
            && has_response_hash);
    uint64_t v7_required_fields = trace_required_field_mask(
        (unsigned) TRACE_V7_FIELD_COUNT);
    bool v7_complete = trace_version < 7
        || (trace_fields_valid
            && (v6_fields_seen & v7_required_fields) == v7_required_fields
            && has_request_client_hint_tokens
            && has_request_client_hint_origin);
    uint64_t v8_required_fields = trace_required_field_mask(
        (unsigned) TRACE_V8_FIELD_COUNT);
    bool v8_complete = trace_version < 8
        || (trace_fields_valid
            && (v6_fields_seen & v8_required_fields) == v8_required_fields
            && redirect_origin_tainted_seen
            && recorded_enforce_cors >= 0
            && recorded_redirect_same_origin_only >= 0
            && recorded_cors_cached_response_validated >= 0);
    uint64_t v9_required_fields = trace_required_field_mask(
        (unsigned) TRACE_V9_FIELD_COUNT);
    bool referrer_policy_metadata_shape =
        recorded_referrer_policy_metadata_valid >= 0
        && recorded_referrer_policy_present >= 0
        && recorded_referrer_policy_seen
        && referrer_policy_normalized(
               result->response_referrer_policy)
        && (recorded_referrer_policy_metadata_valid != 0
            || (recorded_referrer_policy_present == 0
                && result->response_referrer_policy[0] == '\0'))
        && (recorded_referrer_policy_present != 0
            || result->response_referrer_policy[0] == '\0');
    bool v9_complete = trace_version < 9
        || (trace_fields_valid
            && (v6_fields_seen & v9_required_fields) == v9_required_fields
            && logical_url_seen && referrer_policy_metadata_shape);
    uint64_t v10_required_fields = trace_required_field_mask(
        (unsigned) TRACE_V10_FIELD_COUNT);
    bool v10_complete = trace_version < 10
        || (trace_fields_valid
            && (v6_fields_seen & v10_required_fields)
                   == v10_required_fields
            && recorded_security_headers_truncated >= 0);
    uint64_t v11_required_fields = trace_required_field_mask(
        (unsigned) TRACE_V11_FIELD_COUNT);
    bool v11_complete = trace_version < 11
        || (trace_fields_valid
            && (v6_fields_seen & v11_required_fields)
                   == v11_required_fields
            && recorded_security_metadata_seen
            && recorded_security_allow_origin_seen
            && ((recorded_security_metadata.allow_origin_state
                       == FETCH_SECURITY_FIELD_VALID)
                == (recorded_security_metadata.allow_origin[0] != '\0')));
    uint64_t v12_required_fields = trace_required_field_mask(
        (unsigned) TRACE_V12_FIELD_COUNT);
    bool v12_complete = trace_version < 12
        || (trace_fields_valid
            && (v6_fields_seen & v12_required_fields)
                   == v12_required_fields
            && recorded_set_cookies_truncated >= 0);
    /* A delayed capture starts after an earlier navigation has established
       session state.  Replaying that boundary necessarily begins as a fresh
       navigation, so only browser-derived source/user-activation context
       differs: the wire request remains the same method/URL and document
       destination, navigation mode, credentials, cookie shape, headers, and
       body.  Keep
       ordinary traces exact and scope this exception to record zero of an
       explicitly marked delayed-top-level corpus. */
    bool delayed_replay_boundary =
        fetch_trace.replay_starts_at_delayed_top_level
        && sequence == 0 && request != NULL
        && request->sec_fetch_dest != NULL
        && strcasecmp(request->sec_fetch_dest, "document") == 0
        && request->sec_fetch_mode != NULL
        && strcasecmp(request->sec_fetch_mode, "navigate") == 0
        && request_fetch_dest_match && request_fetch_mode_match;
    bool request_extra_shape_match = trace_version >= 13
        ? request_extra_shape_with_cache_match
        : request_extra_shape_legacy_match;
    bool v6_shape_match = trace_version < 6
           /* Cookie values are redacted at capture, so byte length is the
              only comparator left -- and pages that mint session cookies
              with random-length values (mediawiki's client prefs and
              session buckets) can never reproduce the captured length.
              Presence is the semantic property; exact bytes stay visible
              in the diagnostics line. */
        || (((recorded_cookie_bytes == 0) == (request_cookie_bytes == 0)
             || recorded_cookie_bytes == SIZE_MAX)
            && recorded_cf_clearance == (int) request_has_cf_clearance
            && recorded_extra_header_bytes == request_extra_header_bytes
            && request_content_type_match && request_extra_shape_match
               /* allow_http_errors is client-side error-tolerance policy,
                  never serialized onto the wire: the capture tags the same
                  GET differently across its preload and in-page phases, so
                  equality only strands records (avatars and feed media can
                  be recorded as preloads).  Diagnostics keep both raw
                  values visible. */
            && request_if_none_match_match
            && request_if_modified_since_match
            && (request_referer_match || delayed_replay_boundary
                || (request_referer_same_origin_race
                    && request_fetch_dest_match
                    && request != NULL
                    && request->sec_fetch_dest != NULL
                    && strcasecmp(request->sec_fetch_dest,
                                  "script") == 0))
            && request_origin_match && request_accept_match
            && request_fetch_dest_match && request_fetch_mode_match
            && (request_fetch_site_match || delayed_replay_boundary)
            && request_user_agent_match);
    bool v7_shape_match = trace_version < 7
        || (request_client_hint_tokens_match
            && request_client_hint_origin_match);
    bool v8_shape_match = trace_version < 8
        || (recorded_enforce_cors
                == (int) (request != NULL && request->enforce_cors)
            && recorded_redirect_same_origin_only
                == (int) (request != NULL
                          && request->redirect_same_origin_only)
            && recorded_cors_cached_response_validated
                == (int) (request != NULL
                          && request->cors_cached_response_validated));
    /* v8 and older keyed only the fragmentless wire URL.  Retain those
       records for the fragmentless requests they represented, but never let
       a legacy record alias a fragment-bearing logical navigation. */
    bool v9_shape_match = trace_version >= 9
        ? logical_url_match
        : strchr(logical_url, '#') == NULL;
    bool cache_mode_trace_match = trace_version >= 13
        || request == NULL || request->cache_mode == FETCH_CACHE_DEFAULT;
    bool credential_origin_match = !has_recorded_credential_origin
        || recorded_credential_origin[0] == '\0'
        || trace_source_context_match(
               recorded_credential_origin,
               request == NULL ? NULL : request->credential_origin);
    bool initiator_url_match = !has_request_initiator_url
        || scratch->request_initiator_url[0] == '\0'
        || trace_source_context_match(
               scratch->request_initiator_url,
               request == NULL ? NULL : request->initiator_url);
    /* Recorded-empty referrer source/policy are wildcards for the same
       reason as the empty request headers above: engine-supplemental
       capture passes wrote them blank. */
    bool referrer_source_match = !has_request_referrer_source
        || scratch->request_referrer_source[0] == '\0'
        || delayed_replay_boundary
        || trace_source_context_match(
               scratch->request_referrer_source,
               request == NULL ? NULL : request->referrer_source);
    bool record_integrity = meta_ok && trace_version != 0
        && v6_complete && v7_complete && v8_complete && v9_complete
        && v10_complete
        && v11_complete
        && v12_complete
        && !(recorded_success
             && (result->status_code == 0
                 || result->trace_external_cancel
                 || result->timed_out))
        /* Old records did not serialize redirect-origin taint. They remain
           usable for non-CORS lab replay, but CORS replay must not silently
           infer an untainted chain and expose a same-origin final response. */
        && (request == NULL || !request->enforce_cors
            || redirect_origin_tainted_seen)
        && recorded_length != SIZE_MAX
        && cookie_count <= FETCH_RESPONSE_COOKIE_CAPACITY
        && result->set_cookie_count == cookie_count
        && cookie_values_complete
        && (trace_version < 4 || cookie_urls_complete)
        && result->trace_delay_pumps <= FETCH_TRACE_ASYNC_DELAY_PUMP_LIMIT
        && (recorded_header_count == SIZE_MAX
            || parsed_header_count == recorded_header_count);
    if (!record_integrity) {
        if (trace_replay_diagnostics_enabled()) {
            fprintf(
                stderr,
                "HTTP replay integrity record=%zu meta=%d version=%u "
                "complete=%d/%d/%d/%d/%d/%d/%d success-state=%d "
                "cors-taint=%d length=%d cookies=%zu/%zu/%d/%d "
                "delay=%zu headers=%zu/%zu\n",
                sequence, (int) meta_ok, trace_version,
                (int) v6_complete, (int) v7_complete,
                (int) v8_complete, (int) v9_complete,
                (int) v10_complete, (int) v11_complete,
                (int) v12_complete,
                (int) !(recorded_success
                        && (result->status_code == 0
                            || result->trace_external_cancel
                            || result->timed_out)),
                (int) (request == NULL || !request->enforce_cors
                       || redirect_origin_tainted_seen),
                (int) (recorded_length != SIZE_MAX),
                result->set_cookie_count, cookie_count,
                (int) cookie_values_complete,
                (int) (trace_version < 4 || cookie_urls_complete),
                result->trace_delay_pumps, parsed_header_count,
                recorded_header_count);
        }
        snprintf(result->error, sizeof(result->error),
                 "HTTP replay record %zu metadata is corrupt", sequence);
        return false;
    }
    result->set_cookies_truncated = trace_version >= 12
        && recorded_set_cookies_truncated != 0;
    if (trace_version < 10 && request != NULL
        && request->page_context != NULL) {
        TilefinchRequestDestination destination =
            request->page_context->destination;
        if (destination == TILEFINCH_DESTINATION_SCRIPT
            || destination == TILEFINCH_DESTINATION_STYLE
            || destination == TILEFINCH_DESTINATION_IMAGE
            || destination == TILEFINCH_DESTINATION_FONT) {
            /* Legacy traces cannot prove that no security header was dropped
               from their bounded response snapshot. Typed resources fail
               closed instead of gaining replay-only authority. */
            result->response_security_headers_truncated = true;
        }
    }
    if (!fetch_response_security_metadata_from_snapshot(
            &result->security, result->response_headers,
            result->response_headers_length,
            result->response_security_headers_truncated)) {
        snprintf(result->error, sizeof(result->error),
                 "HTTP replay record %zu security metadata is corrupt",
                 sequence);
        return false;
    }
    if (trace_version >= 11) {
        result->security = recorded_security_metadata;
        if (result->security.referrer_policy_state
                == FETCH_SECURITY_FIELD_VALID) {
            snprintf(result->security.referrer_policy,
                     sizeof(result->security.referrer_policy), "%s",
                     result->response_referrer_policy);
        }
    }
    /* v9+ serialized Referrer-Policy independently of the bounded generic
       header snapshot. Preserve that typed authority on replay even when an
       earlier oversized field displaced the textual header. */
    if (trace_version >= 9 && trace_version < 11) {
        result->security.referrer_policy_state =
            recorded_referrer_policy_present == 0
                ? FETCH_SECURITY_FIELD_ABSENT
                : recorded_referrer_policy_metadata_valid != 0
                    ? FETCH_SECURITY_FIELD_VALID
                    : FETCH_SECURITY_FIELD_INVALID;
        result->security.referrer_policy[0] = '\0';
        if (result->security.referrer_policy_state
                == FETCH_SECURITY_FIELD_VALID) {
            snprintf(result->security.referrer_policy,
                     sizeof(result->security.referrer_policy), "%s",
                     result->response_referrer_policy);
        }
    }
    /* Lab and validation builds only: a live page mints fresh tokens into
       request bodies (chatgpt.com's sentinel), so a recorded POST can only be
       replayed against a new run with its body identity waived. Validation
       EBOOTs set the variable from boot.cfg's trace_ignore_request_body. */
    static int ignore_request_body = -1;
#if defined(TILEFINCH_NO_TRACE) && !defined(TILEFINCH_PSP_VALIDATION_LOG)
    ignore_request_body = 0;
#else
    if (ignore_request_body < 0)
        ignore_request_body =
            getenv("TILEFINCH_REPLAY_IGNORE_REQUEST_BODY") != NULL;
#endif
    bool request_body_match = ignore_request_body
        || ((recorded_request_length == SIZE_MAX
             || recorded_request_length == request_length)
            && (!has_request_hash || recorded_request_hash == request_hash));
    bool request_identity_match = method_match && url_match
        && v6_shape_match && v7_shape_match && v8_shape_match
        && v9_shape_match && cache_mode_trace_match
        && request_body_match
           /* Client hints are advisory request decoration; a body served
              without them is identical to one served with them for the
              static resources this trace holds.  Engine-supplemental
              records predate low-hint sending, so require only that the
              request offers at least the recorded hints. */
        && (trace_version < 3
            || ((!recorded_client_hints
                 || (request != NULL && request->send_client_hints))
                && (!recorded_low_client_hints
                    || (request != NULL
                        && request->send_low_client_hints))))
        && (recorded_fetch_user < 0 || delayed_replay_boundary
            || recorded_fetch_user
                 == (request != NULL && request->sec_fetch_user))
        && (recorded_upgrade_insecure < 0
            || recorded_upgrade_insecure
                 == (request != NULL
                     && request->upgrade_insecure_requests))
        && (recorded_diagnostic_mobile_safari < 0
            || recorded_diagnostic_mobile_safari
                 == (int) diagnostic_mobile_safari)
        && (recorded_credentials < 0
            || recorded_credentials == (int) (request == NULL
                ? FETCH_CREDENTIALS_INCLUDE : request->credentials))
        && credential_origin_match && initiator_url_match
        && referrer_source_match
        && (!has_request_referrer_policy
            || scratch->request_referrer_policy[0] == '\0'
            || strcmp(scratch->request_referrer_policy,
                      request == NULL || request->referrer_policy == NULL
                        ? "" : request->referrer_policy) == 0);
    if (!request_identity_match) {
        if (trace_replay_diagnostics_enabled()) {
            fprintf(stderr,
                    "HTTP replay identity record=%zu method=%d url=%d "
                    "v6-shape=%d v7-shape=%d v8-shape=%d "
                    "cookie-bytes=%zu/%zu "
                    "cf-clearance=%d/%d extra-bytes=%zu/%zu "
                    "content-type=%d extra-shape=%d allow-errors=%d/%d "
                    "validators=%d/%d referer=%d origin=%d accept=%d "
                    "fetch=%d/%d/%d user-agent=%d body=%zu/%zu/%d "
                    "hints=%d/%d/%d/%d policy=%d "
                    "fetch-user=%d/%d upgrade=%d/%d safari=%d/%d "
                    "credentials=%d/%d source-origin=%d/%d/%d\n",
                    sequence, method_match, url_match,
                    v6_shape_match, v7_shape_match, v8_shape_match,
                    recorded_cookie_bytes, request_cookie_bytes,
                    recorded_cf_clearance, (int) request_has_cf_clearance,
                    recorded_extra_header_bytes, request_extra_header_bytes,
                    request_content_type_match, request_extra_shape_match,
                    recorded_allow_http_errors,
                    (int) (request != NULL && request->allow_http_errors),
                    request_if_none_match_match,
                    request_if_modified_since_match,
                    request_referer_match, request_origin_match,
                    request_accept_match, request_fetch_dest_match,
                    request_fetch_mode_match, request_fetch_site_match,
                    request_user_agent_match, recorded_request_length,
                    request_length,
                    (int) (!has_request_hash
                           || recorded_request_hash == request_hash),
                    recorded_client_hints,
                    (int) (request != NULL && request->send_client_hints),
                    recorded_low_client_hints,
                    (int) (request != NULL
                           && request->send_low_client_hints),
                    (int) (!has_request_referrer_policy
                           || strcmp(
                                  scratch->request_referrer_policy,
                                  request == NULL
                                    || request->referrer_policy == NULL
                                      ? "" : request->referrer_policy) == 0),
                    recorded_fetch_user,
                    (int) (request != NULL && request->sec_fetch_user),
                    recorded_upgrade_insecure,
                    (int) (request != NULL
                           && request->upgrade_insecure_requests),
                    recorded_diagnostic_mobile_safari,
                    (int) diagnostic_mobile_safari, recorded_credentials,
                    (int) (request == NULL ? FETCH_CREDENTIALS_INCLUDE
                                           : request->credentials),
                    credential_origin_match, initiator_url_match,
                    referrer_source_match);
        }
        if (require_request_identity) {
            *record_status = TRACE_REPLAY_RECORD_NO_MATCH;
            snprintf(result->error, sizeof(result->error),
                     "HTTP replay record %zu does not match %s %s",
                     sequence, method, url);
            return false;
        }
        fetch_trace.replay_request_shape_mismatch_count++;
    }
    *record_status = TRACE_REPLAY_RECORD_MATCHED;
    /* A record matched through the UUID query waiver answers for this
       request's URL, not the recorded identifier; a recorded redirect
       target is left alone. */
    if (url_match && !url_exact
        && trace_replay_url_equal(result->effective_url, url)) {
        snprintf(result->effective_url, sizeof(result->effective_url), "%s",
                 url);
    }
    /* A streaming consumer may intentionally stop inside a larger, complete
       captured response. Verify that entire capture below, then enforce the
       request byte ceiling while delivering slices, as live transport does. */
    bool bounded_stream = recorded_success && writer != NULL
        && writer->defer_replay_body && writer->stream != NULL;
    if (recorded_length > maximum_bytes && !bounded_stream) {
        result->response_limit_exceeded = true;
        snprintf(result->error, sizeof(result->error),
                 "HTTP replay record %zu exceeds request quota", sequence);
        return false;
    }
    /* A live response that crossed its byte cap fails with the received
       prefix still in the result; the capture recorded exactly that prefix.
       The deferred (scheduler) path replays it the same way, so consumers
       that use a capped prefix see what the device saw. */
    bool limit_truncated = !recorded_success && writer != NULL
        && writer->defer_replay_body
        && result->status_code >= 200 && result->status_code <= 299
        && strcmp(result->error, "response quota exceeded") == 0;
    /* CORS is a response-metadata gate. A denied successful record must not
       even open the body file, both to preserve the no-body-exposure boundary
       and to avoid leaking a replay FILE on this early return. */
    if ((recorded_success || limit_truncated)
        && !replay_cors_response_allows(request, url, result)) {
        /* Response-keyed selection resolved one retained record before this
           browser-policy gate. Count the occurrence as deterministically
           claimed without claiming that the caller received its response. */
        *rejected = fetch_trace.replay_response_keyed;
        return false;
    }
    if (writer != NULL) {
        writer->replay_body_hash = trace_body_hash("", 0);
        writer->replay_expected_body_hash = recorded_response_hash;
        writer->replay_body_hash_required = trace_version >= 6;
    }
    uint64_t body_started_ns = tilefinch_platform_monotonic_time_ns();
    FILE *body = fopen(scratch->body_path, "rb");
    TRACE_REPLAY_IO_OPEN(body_started_ns);
    if (body == NULL) {
        snprintf(result->error, sizeof(result->error),
                 "HTTP replay body %zu is missing", sequence);
        return false;
    }
    if (trace_version >= 6) {
        uint64_t body_hash = 0;
        /* One large read per 32 KiB, not per 4 KiB: over host0 each read is
           a USB round trip. */
        unsigned char *hash_buffer = budget_malloc(budget, 32u * 1024u);
        uint64_t hash_started_ns = tilefinch_platform_monotonic_time_ns();
        bool hashed = trace_open_body_hash_buffer(
            body, recorded_length, &body_hash, hash_buffer,
            hash_buffer == NULL ? 0 : 32u * 1024u);
        TRACE_REPLAY_IO_READ(hash_started_ns, recorded_length);
        budget_free(budget, hash_buffer);
        if (!hashed || body_hash != recorded_response_hash) {
            (void) fclose(body);
            snprintf(result->error, sizeof(result->error),
                     "HTTP replay body %zu is corrupt", sequence);
            return false;
        }
    }
    if (!recorded_success && !limit_truncated) {
        uint64_t body_hash = 0;
        bool body_ok = trace_open_body_hash(body, recorded_length, &body_hash)
            && (trace_version < 6 || body_hash == recorded_response_hash);
        if (fclose(body) != 0) body_ok = false;
        if (!body_ok) {
            snprintf(result->error, sizeof(result->error),
                     "HTTP replay body %zu is corrupt", sequence);
        }
        *rejected = body_ok;
        return false;
    }
    TraceReplayRekey *rekey = trace_replay_rekey_build(
        budget, sequence, scratch, url, request, recorded_request_length,
        recorded_request_hash, has_request_hash, result, body,
        recorded_length);
    if (writer != NULL && writer->defer_replay_body) {
        writer->deferred_replay_body = body;
        writer->deferred_replay_length = recorded_length;
        writer->replay_rekey = rekey;
        if (limit_truncated) {
            writer->replay_limit_truncated = true;
            result->error[0] = '\0';
        }
        return true;
    }
    if (writer != NULL && writer->stream != NULL) {
        const FetchStreamOptions *options = writer->stream;
        if (options->on_headers != NULL
            && !options->on_headers(options->opaque, result)) {
            trace_replay_rekey_release(budget, &rekey);
            fclose(body);
            writer->consumer_failed = true;
            snprintf(result->error, sizeof(result->error),
                     "stream consumer rejected response headers");
            return false;
        }
        writer->headers_delivered = true;
        if (writer->metrics != NULL) {
            writer->metrics->headers_delivered = true;
        }
        size_t maximum_chunk = options->irregular_seed != 0
            ? options->irregular_max_chunk_bytes : options->chunk_bytes;
        if (maximum_chunk == 0) maximum_chunk = 16384;
        if (maximum_chunk > 65536) {
            trace_replay_rekey_release(budget, &rekey);
            fclose(body);
            snprintf(result->error, sizeof(result->error),
                     "replay stream chunk exceeds 64 KiB ceiling");
            return false;
        }
        unsigned char *chunk = budget_malloc(budget, maximum_chunk);
        if (chunk == NULL) {
            trace_replay_rekey_release(budget, &rekey);
            fclose(body);
            snprintf(result->error, sizeof(result->error),
                     "replay stream buffer exceeded shared memory budget");
            return false;
        }
        uint64_t random = options->irregular_seed;
        bool body_ok = true;
        while (writer->delivered < recorded_length) {
            if (writer->cancel != NULL
                && writer->cancel(writer->cancel_opaque)) {
                writer->cancelled = true;
                if (writer->metrics != NULL) writer->metrics->cancelled = true;
                snprintf(result->error, sizeof(result->error),
                         "request cancelled");
                body_ok = false;
                break;
            }
            if (options->cancel_after_bytes != 0
                && writer->delivered >= options->cancel_after_bytes) {
                writer->cancelled = true;
                if (writer->metrics != NULL) writer->metrics->cancelled = true;
                snprintf(result->error, sizeof(result->error),
                         "injected stream cancellation at byte %zu",
                         writer->delivered);
                body_ok = false;
                break;
            }
            if (options->truncate_after_bytes != 0
                && writer->delivered >= options->truncate_after_bytes) {
                if (writer->metrics != NULL) writer->metrics->truncated = true;
                snprintf(result->error, sizeof(result->error),
                         "injected truncated response at byte %zu",
                         writer->delivered);
                body_ok = false;
                break;
            }
            size_t wanted = maximum_chunk;
            if (options->irregular_seed != 0) {
                random ^= random << 13;
                random ^= random >> 7;
                random ^= random << 17;
                wanted = 1 + (size_t) (random % maximum_chunk);
            }
            size_t remaining = recorded_length - writer->delivered;
            if (wanted > remaining) wanted = remaining;
            if (options->cancel_after_bytes > writer->delivered
                && wanted > options->cancel_after_bytes - writer->delivered) {
                wanted = options->cancel_after_bytes - writer->delivered;
            }
            if (options->truncate_after_bytes > writer->delivered
                && wanted > options->truncate_after_bytes
                            - writer->delivered) {
                wanted = options->truncate_after_bytes - writer->delivered;
            }
            size_t offset = writer->delivered;
            uint64_t read_started_ns = tilefinch_platform_monotonic_time_ns();
            size_t read_bytes = fread(chunk, 1, wanted, body);
            TRACE_REPLAY_IO_READ(read_started_ns, read_bytes);
            bool read = read_bytes == wanted;
            if (read) {
                writer->replay_body_hash = trace_body_hash_with_offset(
                    (const char *) chunk, wanted, writer->replay_body_hash);
                trace_replay_rekey_apply(rekey, offset, chunk, wanted);
            }
            if (!read
                || receive_data((char *) chunk, 1, wanted, writer) != wanted) {
                if (result->error[0] == '\0') {
                    snprintf(result->error, sizeof(result->error),
                             writer->consumer_failed
                               ? "stream consumer rejected response body"
                               : "HTTP replay body %zu is corrupt", sequence);
                }
                body_ok = false;
                break;
            }
            if (options->stall_every_chunks != 0
                && writer->delivered < recorded_length
                && (writer->metrics == NULL
                    ? (writer->delivered / maximum_chunk)
                          % options->stall_every_chunks == 0
                    : writer->metrics->chunks_received
                          % options->stall_every_chunks == 0)) {
                if (writer->metrics != NULL) {
                    writer->metrics->replay_stalls++;
                }
                if (options->on_stall != NULL
                    && !options->on_stall(options->opaque)) {
                    writer->consumer_failed = true;
                    snprintf(result->error, sizeof(result->error),
                             "stream consumer rejected replay stall");
                    body_ok = false;
                    break;
                }
            }
        }
        if (body_ok) {
            body_ok = fgetc(body) == EOF && !ferror(body)
                && (!writer->replay_body_hash_required
                    || writer->replay_body_hash
                         == writer->replay_expected_body_hash);
            if (!body_ok) {
                snprintf(result->error, sizeof(result->error),
                         "HTTP replay body %zu is corrupt", sequence);
            }
        }
        bool close_ok = fclose(body) == 0;
        budget_free(budget, chunk);
        trace_replay_rekey_release(budget, &rekey);
        return body_ok && close_ok;
    }
    result->data = budget_malloc(budget, recorded_length + 1);
    if (result->data == NULL) {
        trace_replay_rekey_release(budget, &rekey);
        fclose(body);
        snprintf(result->error, sizeof(result->error),
                 "HTTP replay exceeded shared memory budget");
        return false;
    }
    uint64_t read_started_ns = tilefinch_platform_monotonic_time_ns();
    size_t read = fread(result->data, 1, recorded_length, body);
    TRACE_REPLAY_IO_READ(read_started_ns, read);
    bool body_ok = read == recorded_length
                   && fgetc(body) == EOF && !ferror(body)
                   && fclose(body) == 0;
    if (body_ok && trace_version >= 6) {
        body_ok = trace_body_hash(result->data, recorded_length)
                  == recorded_response_hash;
    }
    if (body_ok) {
        trace_replay_rekey_apply(rekey, 0, (unsigned char *) result->data,
                                 recorded_length);
    }
    trace_replay_rekey_release(budget, &rekey);
    if (!body_ok) {
        budget_free(budget, result->data);
        result->data = NULL;
        snprintf(result->error, sizeof(result->error),
                 "HTTP replay body %zu is corrupt", sequence);
        return false;
    }
    result->data[recorded_length] = '\0';
    result->length = recorded_length;
    result->capacity = recorded_length + 1;
    return true;
}

static bool trace_record_request_key(
    size_t sequence, const char *method, const char *url,
    FetchCacheMode cache_mode, TraceReplayScratch *scratch, bool *valid)
{
    if (method == NULL || url == NULL || scratch == NULL || valid == NULL) {
        return false;
    }
    *valid = false;
    if (!trace_path(scratch->meta_path, sizeof(scratch->meta_path), sequence,
                    "meta")) return false;
    uint64_t meta_started_ns = tilefinch_platform_monotonic_time_ns();
    FILE *meta = fopen(scratch->meta_path, "rb");
    if (meta == NULL) return false;
    bool version_seen = false, method_seen = false, url_seen = false;
    bool extra_shape_seen = false;
    bool method_match = false, url_match = false, parse_ok = true;
    unsigned trace_version = 0;
    FetchCacheMode recorded_cache_mode = FETCH_CACHE_DEFAULT;
    bool cache_mode_seen = false;
    while (fgets(scratch->line, sizeof(scratch->line), meta) != NULL) {
        size_t length = strlen(scratch->line);
        while (length != 0
               && (scratch->line[length - 1] == '\n'
                   || scratch->line[length - 1] == '\r')) {
            scratch->line[--length] = '\0';
        }
        char *equals = strchr(scratch->line, '=');
        if (equals == NULL) continue;
        *equals++ = '\0';
        if (strcmp(scratch->line, "psp-http-trace") == 0) {
            size_t parsed = 0;
            bool field_ok = !version_seen
                && trace_parse_size_value(equals, &parsed)
                && parsed >= 1 && parsed <= 13;
            if (field_ok) trace_version = (unsigned) parsed;
            version_seen = true;
            parse_ok = parse_ok && field_ok;
        } else if (strcmp(scratch->line, "method") == 0) {
            if (method_seen) parse_ok = false;
            method_seen = true;
            method_match = strcmp(equals, method) == 0;
        } else if (strcmp(scratch->line, "url") == 0) {
            if (url_seen) parse_ok = false;
            url_seen = true;
            url_match = trace_replay_url_equal(equals, url);
        } else if (strcmp(
                       scratch->line,
                       "request-extra-header-shape") == 0) {
            bool field_ok = !extra_shape_seen;
            if (field_ok) {
                cache_mode_seen = trace_cache_mode_from_header_shape(
                    equals, &recorded_cache_mode);
            }
            extra_shape_seen = true;
            parse_ok = parse_ok && field_ok;
        }
    }
    if (ferror(meta) || fclose(meta) != 0) parse_ok = false;
    TRACE_REPLAY_IO_OPEN(meta_started_ns);
    *valid = parse_ok && version_seen && method_seen && url_seen
        && (trace_version < 13 || cache_mode_seen);
    return *valid && method_match && url_match
        && (trace_version < 13 || recorded_cache_mode == cache_mode);
}

typedef struct {
    bool success;
    long status;
    bool external_cancel;
    bool transport_timeout;
} TraceResponseKeyDescriptor;

static uint64_t trace_response_key_hash(
    const char *method, const char *url, FetchCacheMode cache_mode)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = trace_hash_bytes(hash, method, strlen(method));
    hash = trace_hash_bytes(hash, "\n", 1);
    hash = trace_hash_replay_url(hash, url);
    hash ^= (unsigned) cache_mode;
    hash *= UINT64_C(1099511628211);
    return hash;
}

#ifndef TILEFINCH_NO_FETCH_TRACE
static bool trace_record_request_key_hash(
    size_t sequence, TraceReplayScratch *scratch, uint64_t *hash)
{
    if (scratch == NULL || hash == NULL
        || !trace_path(scratch->meta_path, sizeof(scratch->meta_path),
                       sequence, "meta")) return false;
    uint64_t meta_started_ns = tilefinch_platform_monotonic_time_ns();
    FILE *meta = fopen(scratch->meta_path, "rb");
    if (meta == NULL) return false;
    char method[16] = {0};
    unsigned trace_version = 0;
    FetchCacheMode cache_mode = FETCH_CACHE_DEFAULT;
    bool version_seen = false, cache_mode_seen = false;
    bool extra_shape_seen = false;
    bool method_seen = false, url_seen = false, parse_ok = true;
    while (fgets(scratch->line, sizeof(scratch->line), meta) != NULL) {
        size_t length = strlen(scratch->line);
        while (length != 0
               && (scratch->line[length - 1] == '\n'
                   || scratch->line[length - 1] == '\r')) {
            scratch->line[--length] = '\0';
        }
        char *equals = strchr(scratch->line, '=');
        if (equals == NULL) continue;
        *equals++ = '\0';
        if (strcmp(scratch->line, "psp-http-trace") == 0) {
            size_t parsed = 0;
            bool field_ok = !version_seen
                && trace_parse_size_value(equals, &parsed)
                && parsed >= 1 && parsed <= 13;
            if (field_ok) trace_version = (unsigned) parsed;
            version_seen = true;
            parse_ok = parse_ok && field_ok;
        } else if (strcmp(scratch->line, "method") == 0) {
            bool field_ok = !method_seen
                && strlen(equals) < sizeof(method);
            if (field_ok) snprintf(method, sizeof(method), "%s", equals);
            method_seen = true;
            parse_ok = parse_ok && field_ok;
        } else if (strcmp(scratch->line, "url") == 0) {
            bool field_ok = !url_seen
                && strlen(equals) < sizeof(scratch->request_key);
            if (field_ok) {
                snprintf(scratch->request_key,
                         sizeof(scratch->request_key), "%s", equals);
            }
            url_seen = true;
            parse_ok = parse_ok && field_ok;
        } else if (strcmp(
                       scratch->line,
                       "request-extra-header-shape") == 0) {
            bool field_ok = !extra_shape_seen;
            if (field_ok) {
                cache_mode_seen = trace_cache_mode_from_header_shape(
                    equals, &cache_mode);
            }
            extra_shape_seen = true;
            parse_ok = parse_ok && field_ok;
        }
    }
    if (ferror(meta) || fclose(meta) != 0) parse_ok = false;
    TRACE_REPLAY_IO_OPEN(meta_started_ns);
    parse_ok = parse_ok && version_seen && method_seen && url_seen
        && (trace_version < 13 || cache_mode_seen);
    if (parse_ok) {
        *hash = trace_response_key_hash(method, scratch->request_key,
                                        cache_mode);
    }
    scratch->request_key[0] = '\0';
    return parse_ok;
}
#endif

/* Route ranking reads the fields needed to choose a record and rejects the
   impossible successful/failure-state combination before assigning a rank.
   The selected record is always reparsed by
   trace_replay_response_with_scratch(), which remains the sole authority for
   headers, cookies, delay, error, body, and full integrity semantics. */
static bool trace_response_key_descriptor(
    size_t sequence, TraceReplayScratch *scratch,
    TraceResponseKeyDescriptor *descriptor)
{
    if (scratch == NULL || descriptor == NULL
        || !trace_path(scratch->meta_path, sizeof(scratch->meta_path),
                       sequence, "meta")) return false;
    memset(descriptor, 0, sizeof(*descriptor));
    uint64_t meta_started_ns = tilefinch_platform_monotonic_time_ns();
    FILE *meta = fopen(scratch->meta_path, "rb");
    if (meta == NULL) return false;
    bool success_seen = false, status_seen = false;
    bool external_cancel_seen = false, transport_timeout_seen = false;
    bool valid = true;
    while (valid
           && fgets(scratch->line, sizeof(scratch->line), meta) != NULL) {
        size_t length = strlen(scratch->line);
        while (length != 0
               && (scratch->line[length - 1] == '\n'
                   || scratch->line[length - 1] == '\r')) {
            scratch->line[--length] = '\0';
        }
        char *equals = strchr(scratch->line, '=');
        if (equals == NULL) continue;
        *equals++ = '\0';
        if (strcmp(scratch->line, "success") == 0) {
            int parsed = 0;
            valid = !success_seen
                && trace_parse_bool_value(equals, &parsed);
            descriptor->success = parsed != 0;
            success_seen = true;
        } else if (strcmp(scratch->line, "status") == 0) {
            size_t parsed = 0;
            valid = !status_seen && trace_parse_size_value(equals, &parsed)
                && parsed <= (size_t) LONG_MAX
                && (parsed == 0 || (parsed >= 100 && parsed <= 599));
            descriptor->status = valid ? (long) parsed : 0;
            status_seen = true;
        } else if (strcmp(scratch->line, "external-cancel") == 0) {
            int parsed = 0;
            valid = !external_cancel_seen
                && trace_parse_bool_value(equals, &parsed);
            descriptor->external_cancel = parsed != 0;
            external_cancel_seen = true;
        } else if (strcmp(scratch->line, "transport-timeout") == 0) {
            int parsed = 0;
            valid = !transport_timeout_seen
                && trace_parse_bool_value(equals, &parsed);
            descriptor->transport_timeout = parsed != 0;
            transport_timeout_seen = true;
        }
    }
    bool meta_ok = !ferror(meta);
    if (fclose(meta) != 0) meta_ok = false;
    TRACE_REPLAY_IO_OPEN(meta_started_ns);
    valid = valid && meta_ok && success_seen && status_seen
        && !(descriptor->success
             && (descriptor->status == 0
                 || descriptor->external_cancel
                 || descriptor->transport_timeout));
    return valid;
}

#ifndef TILEFINCH_NO_FETCH_TRACE
static void trace_response_key_index_build(void)
{
#if defined(__PSP__) && !defined(TILEFINCH_PSP_VALIDATION_LOG)
    /* Replay is validation instrumentation, not page work: other PSP builds
       keep the allocation-free bounded scan. */
    return;
#else
    size_t count = fetch_trace.replay_record_count;
    if (count == 0 || count > FETCH_TRACE_ROUTE_INDEX_LIMIT) return;
#if defined(__PSP__)
    /* Validation device replay: fixed storage, never page Budget or heap. */
    static TraceReplayScratch static_scratch;
    TraceReplayRouteIndexEntry *index = trace_replay_static_route_index;
    TraceReplayScratch *scratch = &static_scratch;
    memset(index, 0, count * sizeof(*index));
#else
    TraceReplayRouteIndexEntry *index = calloc(count, sizeof(*index));
    TraceReplayScratch *scratch = calloc(1, sizeof(*scratch));
    if (index == NULL || scratch == NULL) {
        free(index);
        free(scratch);
        return;
    }
#endif
    for (size_t sequence = 0; sequence < count; sequence++) {
        TraceResponseKeyDescriptor descriptor;
        uint64_t key_hash = 0;
        index[sequence].key_valid = trace_record_request_key_hash(
            sequence, scratch, &key_hash);
        if (!index[sequence].key_valid) continue;
        index[sequence].key_hash = key_hash;
        index[sequence].descriptor_valid = trace_response_key_descriptor(
            sequence, scratch, &descriptor);
        if (!index[sequence].descriptor_valid) continue;
        index[sequence].rank =
            descriptor.success && descriptor.status != 0 ? 2
          : (descriptor.status != 0 ? 1 : 0);
    }
#if !defined(__PSP__)
    free(scratch);
#endif
    fetch_trace.replay_route_index = index;
    fetch_trace.replay_route_index_count = count;
#endif
}
#endif

typedef struct {
    size_t sequence;
    size_t candidate_count;
    int rank;
    bool found;
    bool occurrence;
    bool exhausted;
} TraceResponseKeySelection;

/* Native ranked-occurrence-v2 uses no route index or candidate allocation.
   One bounded record-order scan ranks successful HTTP records before
   captured HTTP failures and status-zero failures. The highest rank is the
   retained outcome: rank 2 can be served, while ranks 1 and 0 are claimed
   and replayed as matched rejections by the authoritative full parser. A
   singleton is reusable. Multiple top-rank records are claimed exactly once
   in record-id order even when their bytes are identical; lower ranks never
   become a fallback after exhaustion. */
static bool trace_response_key_select(
    const char *method, const char *url, FetchCacheMode cache_mode,
    TraceReplayScratch *scratch,
    FetchResult *result, TraceResponseKeySelection *selection)
{
    if (method == NULL || url == NULL || scratch == NULL || result == NULL
        || selection == NULL) return false;
    *selection = (TraceResponseKeySelection) {
        .sequence = SIZE_MAX,
        .rank = -1
    };
    int selected_rank = -1;
    size_t first_sequence = SIZE_MAX;
    size_t first_unclaimed = SIZE_MAX;
    bool indexed = fetch_trace.replay_route_index != NULL
        && fetch_trace.replay_route_index_count
           == fetch_trace.replay_record_count;
    uint64_t requested_hash = indexed
        ? trace_response_key_hash(method, url, cache_mode) : 0;
    for (size_t sequence = 0;
         sequence < fetch_trace.replay_record_count; sequence++) {
        const TraceReplayRouteIndexEntry *entry = indexed
            ? &fetch_trace.replay_route_index[sequence] : NULL;
        if (entry != NULL && !entry->key_valid) {
            fetch_trace.replay_invalid_route_request_count++;
            snprintf(result->error, sizeof(result->error),
                     "HTTP response-keyed replay record %zu metadata is corrupt",
                     sequence);
            return false;
        }
        if (entry != NULL && entry->key_hash != requested_hash) continue;
        bool key_valid = false;
        bool key_match = trace_record_request_key(
            sequence, method, url, cache_mode, scratch, &key_valid);
        if (!key_valid) {
            fetch_trace.replay_invalid_route_request_count++;
            snprintf(result->error, sizeof(result->error),
                     "HTTP response-keyed replay record %zu metadata is corrupt",
                     sequence);
            return false;
        }
        if (!key_match) continue;
        if (entry != NULL && !entry->descriptor_valid) {
            fetch_trace.replay_invalid_route_request_count++;
            snprintf(result->error, sizeof(result->error),
                     "HTTP response-keyed replay record %zu response is corrupt",
                     sequence);
            return false;
        }
        int rank = entry == NULL ? -1 : entry->rank;
        if (entry == NULL) {
            TraceResponseKeyDescriptor candidate;
            if (!trace_response_key_descriptor(
                    sequence, scratch, &candidate)) {
                fetch_trace.replay_invalid_route_request_count++;
                snprintf(
                    result->error, sizeof(result->error),
                    "HTTP response-keyed replay record %zu response is corrupt",
                    sequence);
                return false;
            }
            rank = candidate.success && candidate.status != 0 ? 2
                 : (candidate.status != 0 ? 1 : 0);
        }
        if (rank > selected_rank) {
            selected_rank = rank;
            selection->candidate_count = 1;
            first_sequence = sequence;
            first_unclaimed = trace_replay_record_claimed(sequence)
                ? SIZE_MAX : sequence;
        } else if (rank == selected_rank) {
            selection->candidate_count++;
            if (first_unclaimed == SIZE_MAX
                && !trace_replay_record_claimed(sequence)) {
                first_unclaimed = sequence;
            }
        }
    }
    if (selected_rank < 0) return true;
    selection->found = true;
    selection->rank = selected_rank;
    selection->occurrence = selection->candidate_count > 1;
    if (selection->occurrence) {
        selection->sequence = first_unclaimed;
        selection->exhausted = first_unclaimed == SIZE_MAX;
    } else {
        /* A singleton is reusable regardless of its claimed-ledger bit. */
        selection->sequence = first_sequence;
    }
    return true;
}

static bool trace_replay_response(Budget *budget, const char *method,
                                  const char *url,
                                  const FetchRequest *request,
                                  const char *effective_user_agent,
                                  bool diagnostic_mobile_safari,
                                  size_t maximum_bytes, FetchResult *result,
                                  WriteContext *writer,
                                  bool *terminal_matched)
{
    if (terminal_matched != NULL) *terminal_matched = false;
    char normalized_logical_url[TILEFINCH_URL_SERIALIZED_LIMIT];
    char canonical_url[TILEFINCH_URL_SERIALIZED_LIMIT];
    if (!fetch_request_validate(request, NULL)
        || !tilefinch_url_normalize(
               url, normalized_logical_url,
               sizeof(normalized_logical_url))
        || !tilefinch_url_request_key(normalized_logical_url, canonical_url,
                                   sizeof(canonical_url))) return false;
#ifndef __PSP__
    {
        TilefinchTestFaults *faults = tilefinch_test_faults();
        size_t slot = faults->replayed_request_count++
            % (sizeof(faults->replayed_requests)
               / sizeof(faults->replayed_requests[0]));
        snprintf(faults->replayed_requests[slot].url,
                 sizeof(faults->replayed_requests[slot].url), "%s",
                 canonical_url);
        snprintf(faults->replayed_requests[slot].user_agent,
                 sizeof(faults->replayed_requests[slot].user_agent), "%s",
                 effective_user_agent == NULL ? "" : effective_user_agent);
    }
#endif
    TraceReplayScratch *scratch = budget_malloc(budget, sizeof(*scratch));
    if (scratch == NULL) return false;
    fetch_trace.replay_request_count++;
    bool ok = false, found = false, terminal_accounted = false;
    size_t matched_sequence = SIZE_MAX;
    if (fetch_trace.replay_response_keyed) {
        TraceResponseKeySelection selection;
        bool selected = trace_response_key_select(
            method, canonical_url,
            request == NULL ? FETCH_CACHE_DEFAULT : request->cache_mode,
            scratch, result, &selection);
        if (!selected) {
            /* The selector records corrupt route metadata as invalid. */
            terminal_accounted = true;
        } else if (selection.found && !selection.exhausted) {
            size_t sequence = selection.sequence;
            TraceReplayRecordStatus status = TRACE_REPLAY_RECORD_INVALID;
            bool rejected = false;
            ok = trace_replay_response_with_scratch(
                budget, method, canonical_url, normalized_logical_url,
                request, effective_user_agent, diagnostic_mobile_safari,
                maximum_bytes, result, scratch, writer, sequence, false,
                &status, &rejected);
            if (status == TRACE_REPLAY_RECORD_MATCHED
                && (ok || rejected)) {
                trace_replay_claim_record(sequence);
                fetch_trace.replay_matched_request_count++;
                found = true;
                matched_sequence = sequence;
                if (selection.occurrence) {
                    fetch_trace.replay_occurrence_claim_count++;
                } else {
                    fetch_trace.replay_reusable_claim_count++;
                }
                if (rejected) fetch_trace.replay_rejected_request_count++;
                terminal_accounted = true;
            } else {
                /* A selected record which cannot pass the authoritative
                   parser, integrity, quota, or consumer path is invalid. It
                   is not claimed, matched, or exposed as a later fallback. */
                ok = false;
                fetch_trace.replay_invalid_route_request_count++;
                terminal_accounted = true;
                if (result->error[0] == '\0') {
                    snprintf(
                        result->error, sizeof(result->error),
                        "HTTP response-keyed replay selected record %zu "
                        "could not be served",
                        sequence);
                }
                trace_replay_decision(
                    "HTTP response-keyed replay invalid record=%zu "
                    "method=%.12s url=%.300s error=%.120s",
                    sequence, method, canonical_url, result->error);
            }
        } else if (selection.exhausted) {
            fetch_trace.replay_conflicting_request_count++;
            fetch_trace.replay_occurrence_exhausted_count++;
            terminal_accounted = true;
            snprintf(
                result->error, sizeof(result->error),
                "HTTP response-keyed replay occurrence sequence exhausted "
                "for %s %s",
                method, canonical_url);
            trace_replay_decision(
                "HTTP response-keyed replay exhausted method=%.12s "
                "candidates=%zu url=%.420s",
                method, selection.candidate_count, canonical_url);
        } else {
            fetch_trace.replay_unmatched_request_count++;
            terminal_accounted = true;
            snprintf(result->error, sizeof(result->error),
                     "HTTP response-keyed replay has no record for %s %s",
                     method, canonical_url);
            trace_replay_decision(
                "HTTP response-keyed replay unmatched method=%.12s "
                "url=%.420s",
                method, canonical_url);
        }
    } else {
        bool indexed = fetch_trace.replay_route_index != NULL
            && fetch_trace.replay_route_index_count
               == fetch_trace.replay_record_count;
        uint64_t requested_hash = indexed
            ? trace_response_key_hash(
                  method, canonical_url,
                  request == NULL ? FETCH_CACHE_DEFAULT
                                  : request->cache_mode) : 0;
        for (size_t sequence = 0;
             sequence < fetch_trace.replay_record_count; sequence++) {
            if (trace_replay_record_claimed(sequence)) continue;
            const TraceReplayRouteIndexEntry *entry = indexed
                ? &fetch_trace.replay_route_index[sequence] : NULL;
            if (entry != NULL && !entry->key_valid) {
                snprintf(result->error, sizeof(result->error),
                         "HTTP replay record %zu metadata is corrupt",
                         sequence);
                break;
            }
            if (entry != NULL && entry->key_hash != requested_hash) continue;
            bool key_valid = false;
            bool key_match = trace_record_request_key(
                sequence, method, canonical_url,
                request == NULL ? FETCH_CACHE_DEFAULT : request->cache_mode,
                scratch, &key_valid);
            if (!key_valid) {
                snprintf(result->error, sizeof(result->error),
                         "HTTP replay record %zu metadata is corrupt",
                         sequence);
                break;
            }
            if (!key_match) continue;
            TraceReplayRecordStatus status = TRACE_REPLAY_RECORD_INVALID;
            bool rejected = false;
            ok = trace_replay_response_with_scratch(
                budget, method, canonical_url, normalized_logical_url,
                request, effective_user_agent, diagnostic_mobile_safari,
                maximum_bytes, result, scratch, writer, sequence, true,
                &status, &rejected);
            if (status == TRACE_REPLAY_RECORD_NO_MATCH) {
                fetch_result_destroy(result);
                continue;
            }
            if (status == TRACE_REPLAY_RECORD_MATCHED) {
                trace_replay_claim_record(sequence);
                fetch_trace.replay_matched_request_count++;
                found = true;
                matched_sequence = sequence;
                if (rejected) fetch_trace.replay_rejected_request_count++;
            }
            break;
        }
    }
    if (!terminal_accounted && !found && result->error[0] == '\0') {
        fetch_trace.replay_unmatched_request_count++;
        snprintf(result->error, sizeof(result->error),
                 fetch_trace.replay_response_keyed
                   ? "HTTP response-keyed replay has no record for %s %s"
                   : "HTTP replay request does not match any unclaimed "
                     "record for %s %s",
                 method, canonical_url);
        trace_replay_decision(
            "HTTP replay unmatched method=%.12s url=%.420s",
            method, canonical_url);
    }
    if (ok) {
        fetch_trace.replay_served_request_count++;
        char served_date[64];
        int64_t served_epoch = 0;
        if (fetch_response_header_value(result, "date", served_date,
                                        sizeof(served_date))
            && browser_cookie_parse_date(served_date, strlen(served_date),
                                         &served_epoch)) {
            browser_session_advance_cookie_clock(served_epoch);
        }
    } else if (found && trace_replay_diagnostics_enabled()) {
        fprintf(stderr,
                "HTTP replay matched-but-not-served record=%zu method=%.12s "
                "url=%.420s error=%.220s\n",
                matched_sequence, method, canonical_url,
                result->error[0] == '\0' ? "unspecified" : result->error);
    }
    if (!ok || (request != NULL
                && request->credentials == FETCH_CREDENTIALS_OMIT)) {
        fetch_result_discard_all_cookie_storage(result);
    }
    budget_free(budget, scratch);
    if (terminal_matched != NULL) *terminal_matched = found;
    return ok;
}
