/*
 * Replay transport lane: the background transport's API answered from a
 * response-keyed HTTP replay (lab and validation builds only; a shipping
 * build has no fetch trace and compiles every route below away).
 *
 * Page media on the PSP fetches through the background worker: HLS
 * playlists, variants and segments (psp_media_hls.c), provider range
 * windows (media_http.c) and the page-media probe's worker hops. The worker
 * neither records nor replays, so a replay used to decline all of it and a
 * recorded page could never play its video. While a response-keyed replay is
 * active this lane takes every worker request instead: the request is
 * answered at enqueue, on the caller's (browser) thread, from the same record
 * store and with the same media rules the scheduler uses
 * (trace_replay.inc: reusable stream records, Range windows from a stored
 * body, effective-URL and query aliases), and its body is read from the
 * record file through a read-ahead slice as the caller takes it. The
 * record's body hash is checked over the bytes as they are handed out, not
 * by reading the whole body at enqueue: on the PSP that read is host0 I/O
 * on the browser thread, and at every segment boundary it held the picture
 * on screen for as long as the next segment took to read (90 to 250 ms).
 * Nothing reaches the network: an unrecorded request fails like a refused
 * one.
 *
 * A single-hop request whose record followed a redirect is answered with
 * that redirect (302 to the recorded effective URL), so the caller's
 * browser-thread redirect policy runs and a playlist rebases its relative
 * URIs exactly as live; the follow-up request resolves through the
 * effective-URL alias. Response cookies are not replayed through the lane.
 *
 * Host recording (lab only, fetch_trace_media_capture): during a capture the
 * same lane performs each worker request synchronously through the ordinary
 * captured fetch path, so the recording holds exactly the requests the PSP's
 * media code makes, in their device shape. A capture byte ceiling bounds it.
 * The PSP never captures through the lane: there the worker stays declined
 * during a capture, as before.
 *
 * Lane request ids carry FETCH_REPLAY_LANE_ID_TAG, far above any worker id,
 * so a take or cancel reaches the lane and nothing else.
 */
#define FETCH_REPLAY_LANE_LIMIT 8u
#define FETCH_REPLAY_LANE_ID_TAG (UINT64_C(1) << 62)
#if defined(__PSP__)
/* Replay holds only metadata and a file per operation. */
#define FETCH_REPLAY_LANE_BUDGET_BYTES (1024u * 1024u)
#else
/* A host capture holds each response body (a segment is at most 2 MiB). */
#define FETCH_REPLAY_LANE_BUDGET_BYTES (24u * 1024u * 1024u)
#endif
#define FETCH_REPLAY_LANE_PUBLICATION_BYTES (48u * 1024u)
/* A body is read from its record file ahead of the caller in slices of this
   size. HLS takes one TS packet (188 bytes) at a time; read straight from
   the file, every few takes was a host0 round trip on the browser thread. */
#define FETCH_REPLAY_LANE_READ_AHEAD_BYTES (16u * 1024u)

typedef struct {
    uint64_t id;
    FetchResult *result;
    FILE *body;
    /* A captured response's body (host recording), else NULL. */
    const unsigned char *memory;
    size_t length;
    size_t delivered;
    size_t publication_bytes;
    /* Read-ahead slice of the record file (lane budget; NULL when refused,
       then reads go straight to the file). */
    unsigned char *ahead;
    size_t ahead_fill;
    size_t ahead_position;
    /* Bytes of the body taken from the record file so far. */
    size_t file_read;
    /* The body's recorded hash is checked as it is read: the running hash
       covers every byte handed out, and the read that completes the body
       fails when it differs (trace_replay.inc skips the open-time hash). */
    uint64_t hash;
    uint64_t expected_hash;
    bool verify;
    bool streaming;
    bool headers_taken;
    bool success;
} FetchReplayLaneOperation;

static struct {
    Budget budget;
    bool budget_ready;
    uint64_t next_id;
    FetchReplayLaneOperation operations[FETCH_REPLAY_LANE_LIMIT];
#if !defined(__PSP__)
    bool capture;
    uint64_t capture_limit;
    uint64_t captured_bytes;
#endif
} fetch_replay_lane;

#define FETCH_REPLAY_LANE_ROUTE(condition, call) \
    do { if (condition) return call; } while (0)

static bool fetch_replay_lane_capturing(void)
{
#if defined(__PSP__)
    return false;
#else
    return fetch_replay_lane.capture && fetch_trace.mode == FETCH_TRACE_CAPTURE;
#endif
}

static bool fetch_replay_lane_active(void)
{
    return (fetch_trace.mode == FETCH_TRACE_REPLAY
            && fetch_trace.replay_response_keyed)
        || fetch_replay_lane_capturing();
}

#if !defined(__PSP__)
bool fetch_trace_media_capture(bool enabled, uint64_t byte_limit)
{
    fetch_replay_lane.capture = enabled;
    fetch_replay_lane.capture_limit = byte_limit;
    fetch_replay_lane.captured_bytes = 0;
    return !enabled || fetch_trace.mode == FETCH_TRACE_CAPTURE;
}

uint64_t fetch_trace_media_captured_bytes(void)
{
    return fetch_replay_lane.captured_bytes;
}

/* The capture form of one lane request: the ordinary captured fetch, one
   hop at a time for a single-hop request, as the worker would perform it. */
static bool fetch_replay_lane_capture(Budget *budget, const char *url,
                                      const FetchRequest *request,
                                      size_t maximum_bytes, long timeout_ms,
                                      bool single_hop, FetchResult *result)
{
    if (fetch_replay_lane.capture_limit != 0
        && fetch_replay_lane.captured_bytes
               >= fetch_replay_lane.capture_limit) {
        snprintf(result->error, sizeof(result->error),
                 "media capture byte ceiling reached");
        return false;
    }
    if (fetch_replay_lane.capture_limit != 0
        && maximum_bytes > fetch_replay_lane.capture_limit
                               - fetch_replay_lane.captured_bytes) {
        maximum_bytes = (size_t) (fetch_replay_lane.capture_limit
                                  - fetch_replay_lane.captured_bytes);
    }
    bool fetched = single_hop
        ? fetch_request_single_hop(budget, url, request, maximum_bytes,
                                   timeout_ms, result)
        : fetch_request_cancelable(budget, url, request, maximum_bytes,
                                   timeout_ms, NULL, NULL, result);
    fetch_replay_lane.captured_bytes += result->length;
    return fetched;
}
#endif

static bool fetch_replay_lane_owns(uint64_t request_id)
{
    return (request_id & FETCH_REPLAY_LANE_ID_TAG) != 0;
}

static FetchReplayLaneOperation *fetch_replay_lane_find(uint64_t request_id)
{
    for (size_t i = 0; i < FETCH_REPLAY_LANE_LIMIT; i++) {
        FetchReplayLaneOperation *operation =
            &fetch_replay_lane.operations[i];
        if (operation->id != 0 && operation->id == request_id)
            return operation;
    }
    return NULL;
}

static void fetch_replay_lane_release(FetchReplayLaneOperation *operation)
{
    if (operation == NULL || operation->id == 0) return;
    if (operation->body != NULL) (void) fclose(operation->body);
    if (operation->ahead != NULL)
        budget_free(&fetch_replay_lane.budget, operation->ahead);
    /* operation->memory is the result's own body. */
    fetch_result_free(operation->result);
    *operation = (FetchReplayLaneOperation) {0};
}

static size_t fetch_replay_lane_operations(void)
{
    size_t count = 0;
    for (size_t i = 0; i < FETCH_REPLAY_LANE_LIMIT; i++)
        count += fetch_replay_lane.operations[i].id != 0;
    return count;
}

static void fetch_replay_lane_cancel_all(void)
{
    for (size_t i = 0; i < FETCH_REPLAY_LANE_LIMIT; i++)
        fetch_replay_lane_release(&fetch_replay_lane.operations[i]);
}

static bool fetch_replay_lane_initialize(void)
{
    if (!fetch_replay_lane.budget_ready) {
        budget_init(&fetch_replay_lane.budget,
                    FETCH_REPLAY_LANE_BUDGET_BYTES);
        fetch_replay_lane.budget_ready = true;
    }
    return true;
}

static bool fetch_replay_lane_same_url(const char *left, const char *right)
{
    char left_key[TILEFINCH_URL_SERIALIZED_LIMIT];
    char right_key[TILEFINCH_URL_SERIALIZED_LIMIT];
    return tilefinch_url_request_key(left, left_key, sizeof(left_key))
        && tilefinch_url_request_key(right, right_key, sizeof(right_key))
        && strcmp(left_key, right_key) == 0;
}

static uint64_t fetch_replay_lane_enqueue(
    const char *url, const FetchRequest *request, size_t maximum_bytes,
    long timeout_ms, bool streaming, bool single_hop,
    size_t publication_bytes, FetchBackgroundEnqueueStatus *status)
{
    if (status != NULL) *status = FETCH_BACKGROUND_ENQUEUE_UNSUPPORTED;
    if (url == NULL || maximum_bytes == 0
        || !fetch_request_validate(request, NULL)) return 0;
    FetchReplayLaneOperation *operation = NULL;
    for (size_t i = 0; i < FETCH_REPLAY_LANE_LIMIT; i++) {
        if (fetch_replay_lane.operations[i].id == 0) {
            operation = &fetch_replay_lane.operations[i];
            break;
        }
    }
    if (operation == NULL) {
        if (status != NULL) *status = FETCH_BACKGROUND_ENQUEUE_SATURATED;
        return 0;
    }
    (void) fetch_replay_lane_initialize();
    Budget *budget = &fetch_replay_lane.budget;
#if !defined(__PSP__)
    /* A captured fetch shares the transport's budget, as every live
       request must while the shared curl state is held. */
    if (fetch_replay_lane_capturing() && curl_global_users != 0)
        budget = curl_budget;
#endif
    FetchResult *result = fetch_result_create(budget);
    if (result == NULL) {
        if (status != NULL) *status = FETCH_BACKGROUND_ENQUEUE_MEMORY;
        return 0;
    }
    const char *method = request == NULL || request->method == NULL
        ? "GET" : request->method;
    WriteContext writer = {
        .result = result,
        .maximum_bytes = maximum_bytes,
        .defer_replay_body = true,
        .replay_verify_on_delivery = true
    };
    bool served = false;
    FILE *body = NULL;
    const unsigned char *memory = NULL;
    size_t length = 0;
#if !defined(__PSP__)
    if (fetch_replay_lane_capturing()) {
        served = fetch_replay_lane_capture(budget, url, request,
                                           maximum_bytes, timeout_ms,
                                           single_hop, result);
        memory = (const unsigned char *) result->data;
        length = served ? result->length : 0;
        fetch_result_discard_all_cookie_storage(result);
    } else
#endif
    {
        (void) timeout_ms;
        fetch_trace.replay_media_lane_request_count++;
        fetch_trace.replay_media_lane_call = true;
        served = trace_replay_response(
            budget, method, url, request,
            fetch_effective_user_agent(request, false), false,
            maximum_bytes, result, &writer, NULL);
        fetch_trace.replay_media_lane_call = false;
        fetch_result_discard_all_cookie_storage(result);
        body = writer.deferred_replay_body;
        length = served ? writer.deferred_replay_length : 0;
        trace_replay_rekey_release(budget, &writer.replay_rekey);
    }
    if (served && single_hop && memory == NULL
        && !fetch_trace.replay_media_served_alias
        && result->effective_url[0] != '\0'
        && !fetch_replay_lane_same_url(result->effective_url, url)) {
        /* The recording followed a redirect: hand the hop its redirect. */
        if (body != NULL) (void) fclose(body);
        body = NULL;
        length = 0;
        char location[TILEFINCH_URL_SERIALIZED_LIMIT];
        snprintf(location, sizeof(location), "%s", result->effective_url);
        result->status_code = 302;
        snprintf(result->effective_url, sizeof(result->effective_url), "%s",
                 url);
        result->content_type[0] = '\0';
        int written = snprintf(result->response_headers,
                               sizeof(result->response_headers),
                               "location: %s\ncontent-length: 0\n",
                               location);
        result->response_headers_length = written > 0
            && (size_t) written < sizeof(result->response_headers)
            ? (size_t) written : 0;
    } else if (served) {
        /* The record holds curl's decoded body; frame the served copy as
           what it now is. A Range answer was framed when it was cut. */
        TraceReplayRange range;
        trace_request_range(request, &range);
        if (!range.present && result->status_code == 200)
            trace_replay_reframe_headers(result, length, NULL);
    }
    if (served && memory == NULL)
        fetch_trace.replay_media_lane_served_count++;
    if (++fetch_replay_lane.next_id >= FETCH_REPLAY_LANE_ID_TAG)
        fetch_replay_lane.next_id = 1;
    *operation = (FetchReplayLaneOperation) {
        .id = FETCH_REPLAY_LANE_ID_TAG | fetch_replay_lane.next_id,
        .result = result,
        .body = body,
        .memory = memory,
        .length = length,
        .publication_bytes = publication_bytes == 0
            || publication_bytes > FETCH_REPLAY_LANE_PUBLICATION_BYTES
            ? FETCH_REPLAY_LANE_PUBLICATION_BYTES : publication_bytes,
        .hash = writer.replay_body_hash,
        .expected_hash = writer.replay_expected_body_hash,
        .verify = body != NULL && writer.replay_body_hash_required,
        .streaming = streaming,
        .success = served
    };
    if (!served && result->error[0] == '\0') {
        snprintf(result->error, sizeof(result->error),
                 "HTTP replay has no record for this media request");
    }
    if (status != NULL) *status = FETCH_BACKGROUND_ENQUEUE_ADMITTED;
    return operation->id;
}

static bool fetch_replay_lane_progress(uint64_t request_id,
                                       FetchBackgroundProgress *progress)
{
    FetchReplayLaneOperation *operation = fetch_replay_lane_find(request_id);
    if (operation == NULL || progress == NULL) return false;
    size_t left = operation->length - operation->delivered;
    bool drained = !operation->streaming || left == 0;
    *progress = (FetchBackgroundProgress) {
        .received_body_bytes = operation->streaming
            ? operation->delivered : operation->length,
        .available_body_bytes = operation->streaming
            ? (left < operation->publication_bytes
                   ? left : operation->publication_bytes) : 0,
        .status_code = drained ? operation->result->status_code : 0,
        .active = !drained,
        .complete = drained
    };
    return true;
}

static void fetch_replay_lane_copy_headers(FetchResult *destination,
                                           const FetchResult *source)
{
    destination->status_code = source->status_code;
    snprintf(destination->effective_url, sizeof(destination->effective_url),
             "%s", source->effective_url);
    snprintf(destination->content_type, sizeof(destination->content_type),
             "%s", source->content_type);
    snprintf(destination->etag, sizeof(destination->etag), "%s",
             source->etag);
    snprintf(destination->last_modified, sizeof(destination->last_modified),
             "%s", source->last_modified);
    snprintf(destination->server, sizeof(destination->server), "%s",
             source->server);
    memcpy(destination->response_headers, source->response_headers,
           source->response_headers_length + 1u);
    destination->response_headers_length = source->response_headers_length;
    destination->security = source->security;
    snprintf(destination->response_referrer_policy,
             sizeof(destination->response_referrer_policy), "%s",
             source->response_referrer_policy);
    destination->response_referrer_policy_header_present =
        source->response_referrer_policy_header_present;
    destination->response_referrer_policy_metadata_valid =
        source->response_referrer_policy_metadata_valid;
    destination->response_security_headers_truncated =
        source->response_security_headers_truncated;
}

static void fetch_replay_lane_media_view(
    FetchBackgroundMediaResponse *destination,
    const FetchReplayLaneOperation *operation)
{
    const FetchResult *source = operation->result;
    *destination = (FetchBackgroundMediaResponse) {
        .length = operation->length,
        .received_body_bytes = operation->streaming
            ? operation->delivered : operation->length,
        .status_code = source->status_code,
        .success = operation->success
    };
    snprintf(destination->effective_url, sizeof(destination->effective_url),
             "%s", source->effective_url);
    (void) fetch_response_header_value(
        source, "content-length", destination->content_length,
        sizeof(destination->content_length));
    (void) fetch_response_header_value(
        source, "content-range", destination->content_range,
        sizeof(destination->content_range));
    snprintf(destination->error, sizeof(destination->error), "%s",
             source->error);
}

static bool fetch_replay_lane_take_headers(uint64_t request_id,
                                           FetchResult *metadata)
{
    FetchReplayLaneOperation *operation = fetch_replay_lane_find(request_id);
    if (operation == NULL || metadata == NULL || !operation->streaming
        || !operation->success || operation->headers_taken) return false;
    operation->headers_taken = true;
    fetch_replay_lane_copy_headers(metadata, operation->result);
    return true;
}

static bool fetch_replay_lane_take_media_headers(
    uint64_t request_id, FetchBackgroundMediaResponse *metadata)
{
    FetchReplayLaneOperation *operation = fetch_replay_lane_find(request_id);
    if (operation == NULL || metadata == NULL || !operation->streaming
        || !operation->success || operation->headers_taken) return false;
    operation->headers_taken = true;
    fetch_replay_lane_media_view(metadata, operation);
    return true;
}

/* Fills destination from the read-ahead slice, refilling it from the record
   file; a read at least a slice long goes straight to the destination. */
static size_t fetch_replay_lane_file_read(FetchReplayLaneOperation *operation,
                                          unsigned char *destination,
                                          size_t length)
{
    size_t done = 0;
    while (done < length) {
        size_t buffered = operation->ahead_fill - operation->ahead_position;
        if (buffered != 0) {
            size_t copy = length - done < buffered ? length - done : buffered;
            memcpy(destination + done,
                   operation->ahead + operation->ahead_position, copy);
            operation->ahead_position += copy;
            done += copy;
            continue;
        }
        size_t left = operation->length - operation->file_read;
        if (left == 0) break;
        if (operation->ahead == NULL
            && length - done < FETCH_REPLAY_LANE_READ_AHEAD_BYTES)
            operation->ahead = budget_malloc(
                &fetch_replay_lane.budget, FETCH_REPLAY_LANE_READ_AHEAD_BYTES);
        uint64_t started_ns = tilefinch_platform_monotonic_time_ns();
        size_t got;
        if (operation->ahead == NULL
            || length - done >= FETCH_REPLAY_LANE_READ_AHEAD_BYTES) {
            size_t wanted = length - done < left ? length - done : left;
            got = fread(destination + done, 1, wanted, operation->body);
            done += got;
        } else {
            size_t wanted = left < FETCH_REPLAY_LANE_READ_AHEAD_BYTES
                ? left : FETCH_REPLAY_LANE_READ_AHEAD_BYTES;
            got = fread(operation->ahead, 1, wanted, operation->body);
            operation->ahead_fill = got;
            operation->ahead_position = 0;
        }
        TRACE_REPLAY_IO_READ(started_ns, got);
        operation->file_read += got;
        if (got == 0) break;
    }
    return done;
}

static bool fetch_replay_lane_read(FetchReplayLaneOperation *operation,
                                   unsigned char *destination, size_t length)
{
    if (length == 0) return true;
    if (operation->memory != NULL) {
        memcpy(destination, operation->memory + operation->delivered, length);
        return true;
    }
    size_t read = operation->body == NULL
        ? 0 : fetch_replay_lane_file_read(operation, destination, length);
    if (read == length && operation->verify) {
        operation->hash = trace_body_hash_with_offset(
            (const char *) destination, length, operation->hash);
        if (operation->delivered + length == operation->length
            && operation->hash != operation->expected_hash) {
            operation->success = false;
            snprintf(operation->result->error,
                     sizeof(operation->result->error),
                     "HTTP replay body is corrupt");
            return false;
        }
    }
    if (read == length) return true;
    operation->success = false;
    snprintf(operation->result->error, sizeof(operation->result->error),
             "HTTP replay body ended early");
    return false;
}

static bool fetch_replay_lane_take_chunk(uint64_t request_id,
                                         unsigned char *destination,
                                         size_t capacity, size_t *length)
{
    FetchReplayLaneOperation *operation = fetch_replay_lane_find(request_id);
    if (operation == NULL || destination == NULL || length == NULL
        || capacity == 0 || !operation->streaming || !operation->success
        || !operation->headers_taken
        || operation->delivered == operation->length) return false;
    size_t take = operation->length - operation->delivered;
    if (take > operation->publication_bytes)
        take = operation->publication_bytes;
    if (take > capacity) take = capacity;
    if (!fetch_replay_lane_read(operation, destination, take)) return false;
    operation->delivered += take;
    *length = take;
    return true;
}

static bool fetch_replay_lane_take_fetch_result(
    uint64_t request_id, Budget *budget, FetchResult *result,
    size_t consumed_body_bytes)
{
    FetchReplayLaneOperation *operation = fetch_replay_lane_find(request_id);
    if (operation == NULL || budget == NULL || result == NULL
        || !operation->streaming
        || (operation->success
            && operation->delivered != operation->length)) return false;
    result->budget = budget;
    fetch_replay_lane_copy_headers(result, operation->result);
    result->received_body_bytes = operation->delivered;
    result->timed_out = operation->result->timed_out;
    result->response_limit_exceeded =
        operation->result->response_limit_exceeded;
    bool assembled = consumed_body_bytes == operation->length;
    snprintf(result->error, sizeof(result->error), "%s",
             assembled ? operation->result->error
                       : "background streamed response was incomplete");
    bool success = assembled && operation->success;
    fetch_replay_lane_release(operation);
    return success;
}

static bool fetch_replay_lane_take_media_result(
    uint64_t request_id, FetchBackgroundMediaResponse *result,
    size_t consumed_body_bytes)
{
    FetchReplayLaneOperation *operation = fetch_replay_lane_find(request_id);
    if (operation == NULL || result == NULL || !operation->streaming
        || (operation->success
            && operation->delivered != operation->length)) return false;
    fetch_replay_lane_media_view(result, operation);
    result->success = consumed_body_bytes == operation->length
        && operation->success;
    if (consumed_body_bytes != operation->length) {
        snprintf(result->error, sizeof(result->error), "%s",
                 "background streamed response was incomplete");
    }
    bool success = result->success;
    fetch_replay_lane_release(operation);
    return success;
}

static bool fetch_replay_lane_take_media(
    uint64_t request_id, unsigned char *destination, size_t capacity,
    FetchBackgroundMediaResponse *result)
{
    FetchReplayLaneOperation *operation = fetch_replay_lane_find(request_id);
    if (operation == NULL || result == NULL || operation->streaming)
        return false;
    bool copied = operation->length <= capacity
        && (operation->length == 0 || destination != NULL)
        && fetch_replay_lane_read(operation, destination, operation->length);
    fetch_replay_lane_media_view(result, operation);
    if (!copied && result->error[0] == '\0') {
        snprintf(result->error, sizeof(result->error), "%s",
                 "background result destination was too small");
    }
    result->success = copied && operation->success;
    fetch_replay_lane_release(operation);
    return true;
}

static bool fetch_replay_lane_take(uint64_t request_id,
                                   unsigned char *destination,
                                   size_t capacity,
                                   FetchBackgroundResult *result)
{
    FetchReplayLaneOperation *operation = fetch_replay_lane_find(request_id);
    if (operation == NULL || result == NULL || operation->streaming)
        return false;
    const FetchResult *source = operation->result;
    memset(result, 0, sizeof(*result));
    bool copied = operation->length <= capacity
        && (operation->length == 0 || destination != NULL)
        && fetch_replay_lane_read(operation, destination, operation->length);
    result->length = operation->length;
    result->received_body_bytes = operation->length;
    result->status_code = source->status_code;
    result->success = copied && operation->success;
    snprintf(result->effective_url, sizeof(result->effective_url), "%s",
             source->effective_url);
    snprintf(result->content_type, sizeof(result->content_type), "%s",
             source->content_type);
    (void) fetch_response_header_value(source, "content-range",
                                       result->content_range,
                                       sizeof(result->content_range));
    memcpy(result->response_headers, source->response_headers,
           source->response_headers_length + 1u);
    result->response_headers_length = source->response_headers_length;
    result->security = source->security;
    snprintf(result->error, sizeof(result->error), "%s",
             copied ? source->error
                    : "background result destination was too small");
    fetch_replay_lane_release(operation);
    return true;
}

static bool fetch_replay_lane_cancel(uint64_t request_id)
{
    FetchReplayLaneOperation *operation = fetch_replay_lane_find(request_id);
    if (operation == NULL) return false;
    fetch_replay_lane_release(operation);
    return true;
}
