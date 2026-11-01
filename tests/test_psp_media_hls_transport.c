/*
 * psp_media_hls.c against a scripted background transport. The PSP worker
 * is not built on the host, so this file renames the worker entry points the
 * HLS context calls and supplies them here; everything else (hop admission,
 * request preparation, retry policy, playlist parsing) is the shipping code.
 *
 * Device regression: the playlist hop failed inside the worker
 * before any status line (DNS stub timeout, then CURLE_OPERATION_TIMEDOUT at
 * the 3 s connect budget). The worker published no headers, so the HLS pump
 * waited for headers forever and the open died at the 45 s resolve-phase
 * timeout instead of retrying the playlist.
 */
#define fetch_background_transport_available fake_transport_available
#define fetch_background_transport_cancel fake_transport_cancel
#define fetch_background_transport_enqueue_media_hop_stream_sized_diagnosed \
    fake_transport_enqueue_hop
#define fetch_background_transport_enqueue_media_stream_sized_diagnosed \
    fake_transport_enqueue
#define fetch_background_transport_progress fake_transport_progress
#define fetch_background_transport_take_chunk fake_transport_take_chunk
#define fetch_background_transport_take_fetch_result_consumed \
    fake_transport_take_result
#define fetch_background_transport_take_headers fake_transport_take_headers

#include "../src/psp_media_hls.c"

#include <stdlib.h>

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "psp-media-hls-transport failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

#define DOCUMENT "https://video.test/talks/clip"
#define PLAYLIST "https://hls.video.test/master.m3u8"

typedef enum {
    /* Queued or running on the worker; nothing published yet. */
    FAKE_RUNNING = 0,
    FAKE_RETIRED,
    /* The hop failed before a status line: complete, no headers. */
    FAKE_FAILED_BEFORE_HEADERS,
    /* Complete and reported successful, but no header snapshot was ever
       published (must never deliver a body unadmitted). */
    FAKE_COMPLETE_WITHOUT_HEADERS,
    FAKE_PLAYLIST
} FakeOutcome;

#define FAKE_LIMIT 8u

static struct {
    FakeOutcome outcome[FAKE_LIMIT];
    size_t enqueued;
    size_t cancelled;
    size_t taken;
    long connect_timeout_ms;
    bool body_taken;
    bool admission_closed;
} fake;

bool fake_transport_available(void) { return true; }

bool fake_transport_cancel(uint64_t request_id, const char *reason)
{
    (void) request_id; (void) reason;
    fake.cancelled++;
    return true;
}

static uint64_t fake_enqueue(const FetchRequest *request,
                             FetchBackgroundEnqueueStatus *status)
{
    if (request != NULL) fake.connect_timeout_ms = request->connect_timeout_ms;
    if (fake.admission_closed) {
        if (status != NULL) *status = FETCH_BACKGROUND_ENQUEUE_ADMISSION_CLOSED;
        return 0;
    }
    if (fake.enqueued >= FAKE_LIMIT) {
        if (status != NULL) *status = FETCH_BACKGROUND_ENQUEUE_UNAVAILABLE;
        return 0;
    }
    fake.enqueued++;
    return (uint64_t) fake.enqueued;
}

uint64_t fake_transport_enqueue_hop(
    const char *url, const FetchRequest *request, size_t maximum_bytes,
    long timeout_ms, size_t publication_bytes,
    FetchBackgroundEnqueueStatus *status)
{
    (void) url; (void) maximum_bytes; (void) timeout_ms;
    (void) publication_bytes;
    return fake_enqueue(request, status);
}

uint64_t fake_transport_enqueue(
    const char *url, const FetchRequest *request, size_t maximum_bytes,
    long timeout_ms, size_t publication_bytes,
    FetchBackgroundEnqueueStatus *status)
{
    (void) url; (void) maximum_bytes; (void) timeout_ms;
    (void) publication_bytes;
    return fake_enqueue(request, status);
}

static FakeOutcome fake_outcome(uint64_t id)
{
    return id == 0 || id > FAKE_LIMIT ? FAKE_RUNNING : fake.outcome[id - 1];
}

bool fake_transport_progress(uint64_t request_id,
                             FetchBackgroundProgress *progress)
{
    if (request_id == 0 || request_id > fake.enqueued) return false;
    if (fake_outcome(request_id) == FAKE_RETIRED) return false;
    bool complete = fake_outcome(request_id) != FAKE_RUNNING;
    *progress = (FetchBackgroundProgress) {
        .active = !complete,
        .complete = complete
    };
    return true;
}

bool fake_transport_take_headers(uint64_t request_id, FetchResult *metadata)
{
    if (fake_outcome(request_id) == FAKE_PLAYLIST) {
        metadata->status_code = 200;
        snprintf(metadata->effective_url, sizeof(metadata->effective_url), "%s",
                 PLAYLIST);
        return true;
    }
    return false;
}

bool fake_transport_take_chunk(uint64_t request_id,
                               unsigned char *destination, size_t capacity,
                               size_t *length)
{
    if (length != NULL) *length = 0;
    static const char playlist[] =
        "#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXTINF:2,\nclip.ts\n"
        "#EXT-X-ENDLIST\n";
    if (fake_outcome(request_id) == FAKE_PLAYLIST && !fake.body_taken
        && capacity >= sizeof(playlist) - 1u) {
        memcpy(destination, playlist, sizeof(playlist) - 1u);
        *length = sizeof(playlist) - 1u;
        fake.body_taken = true;
        return true;
    }
    return false;
}

bool fake_transport_take_result(uint64_t request_id, Budget *budget,
                                FetchResult *result, size_t consumed)
{
    (void) budget; (void) consumed;
    fake.taken++;
    if (fake_outcome(request_id) == FAKE_COMPLETE_WITHOUT_HEADERS
        || fake_outcome(request_id) == FAKE_PLAYLIST) {
        result->status_code = 200;
        return true;
    }
    result->transport_code = 28;
    result->timed_out = true;
    snprintf(result->error, sizeof(result->error), "%s",
             "Connection timed out after 3000 milliseconds");
    return false;
}

typedef struct {
    Budget budget;
    BrowserSession session;
    PspMediaHlsContext *hls;
} Fixture;

static bool fixture_init(Fixture *fixture, bool page_redirects)
{
    memset(&fake, 0, sizeof(fake));
    memset(fixture, 0, sizeof(*fixture));
    budget_init(&fixture->budget, 4u * 1024u * 1024u);
    if (!browser_session_init(
            &fixture->session, &fixture->budget, 128u * 1024u)) return false;
    char error[192] = {0};
    fixture->hls = psp_media_hls_create(
        &fixture->budget, &fixture->session, PLAYLIST, DOCUMENT,
        TILEFINCH_REQUEST_MODE_NO_CORS, TILEFINCH_CREDENTIALS_INCLUDE,
        PSP_MEDIA_HLS_BOUND_FIRMWARE, NULL, page_redirects,
        error, sizeof(error));
    return fixture->hls != NULL;
}

static void fixture_destroy(Fixture *fixture)
{
    psp_media_hls_destroy(fixture->hls);
    browser_session_destroy(&fixture->session);
    CHECK(fixture->budget.current == 0);
}

/* Pumps like the open loop does, at most `pumps` times, until the open
   leaves PENDING. */
static PspMediaHlsOpenStatus pump(Fixture *fixture, unsigned pumps,
                                  char *error, size_t error_size)
{
    PspMediaHlsOpenStatus status = PSP_MEDIA_HLS_OPEN_PENDING;
    for (unsigned i = 0; i < pumps && status == PSP_MEDIA_HLS_OPEN_PENDING;
         i++)
        status = psp_media_hls_pump(fixture->hls, error, error_size);
    return status;
}

/* A running playlist hop keeps the open pending; nothing is failed early. */
static void test_running_playlist_stays_pending(bool page_redirects)
{
    Fixture fixture;
    CHECK(fixture_init(&fixture, page_redirects));
    char error[192] = {0};
    CHECK(pump(&fixture, 50, error, sizeof(error))
          == PSP_MEDIA_HLS_OPEN_PENDING);
    CHECK(fake.enqueued == 1);
    CHECK(fake.taken == 0);
    /* The connect budget includes name resolution; a slow PSP lookup must
       not consume it (src/psp_media_hls_policy.h). */
    CHECK(fake.connect_timeout_ms == PSP_HLS_CONNECT_TIMEOUT_MS);
    CHECK(fake.connect_timeout_ms >= 8000);
    fixture_destroy(&fixture);
}

/* The device failure: the playlist hop completes with a transport timeout
   and no headers. The pump must take the terminal result, retry the
   playlist once (a timeout is retryable), and then fail the open with the
   transport's reason, instead of waiting for headers that never come. */
static void test_failure_before_headers_retries_then_fails(
    bool page_redirects)
{
    Fixture fixture;
    CHECK(fixture_init(&fixture, page_redirects));
    char error[192] = {0};
    CHECK(pump(&fixture, 3, error, sizeof(error))
          == PSP_MEDIA_HLS_OPEN_PENDING);
    CHECK(fake.enqueued == 1);
    fake.outcome[0] = FAKE_FAILED_BEFORE_HEADERS;
    CHECK(pump(&fixture, 3, error, sizeof(error))
          == PSP_MEDIA_HLS_OPEN_PENDING);
    CHECK(fake.taken == 1);
    CHECK(fake.enqueued == 2);
    fake.outcome[1] = FAKE_FAILED_BEFORE_HEADERS;
    CHECK(pump(&fixture, 50, error, sizeof(error))
          == PSP_MEDIA_HLS_OPEN_FAILED);
    CHECK(fake.taken == 2);
    CHECK(fake.enqueued == 2);
    CHECK(strstr(error, "timed out") != NULL);
    CHECK(psp_media_hls_failed(fixture.hls));
    fixture_destroy(&fixture);
}

/* A worker result that claims success without ever having published (and
   so admitted) its headers is refused: no body reaches the parser without
   passing response authority. */
static void test_unadmitted_success_is_refused(bool page_redirects)
{
    Fixture fixture;
    CHECK(fixture_init(&fixture, page_redirects));
    fake.outcome[0] = FAKE_COMPLETE_WITHOUT_HEADERS;
    char error[192] = {0};
    CHECK(pump(&fixture, 50, error, sizeof(error))
          == PSP_MEDIA_HLS_OPEN_FAILED);
    CHECK(fake.taken == 1);
    CHECK(strstr(error, "without admitted headers") != NULL);
    MediaHlsStats stats = {0};
    CHECK(psp_media_hls_stats(fixture.hls, &stats));
    CHECK(stats.playlist_bytes_received == 0);
    fixture_destroy(&fixture);
}

static void test_network_retirement(bool page_redirects)
{
    Fixture fixture;
    CHECK(fixture_init(&fixture, page_redirects));
    char error[192] = {0};
    CHECK(pump(&fixture, 1, error, sizeof(error)) == PSP_MEDIA_HLS_OPEN_PENDING);
    fake.outcome[0] = FAKE_RETIRED;
    fake.admission_closed = true;
    CHECK(pump(&fixture, 20, error, sizeof(error)) == PSP_MEDIA_HLS_OPEN_PENDING);
    CHECK(fake.enqueued == 1 && fake.taken == 0 && !fake.body_taken);
    fake.admission_closed = false;
    fake.outcome[1] = FAKE_PLAYLIST;
    CHECK(pump(&fixture, 3, error, sizeof(error)) == PSP_MEDIA_HLS_OPEN_PENDING);
    /* One replacement playlist, followed by its first segment. */
    CHECK(fake.enqueued == 3 && fixture.hls->source[0] != NULL);
    fixture_destroy(&fixture);

    /* Repeated losses spend only the existing playlist retry allowance. */
    CHECK(fixture_init(&fixture, page_redirects));
    fake.outcome[0] = fake.outcome[1] = FAKE_RETIRED;
    CHECK(pump(&fixture, 20, error, sizeof(error)) == PSP_MEDIA_HLS_OPEN_FAILED);
    CHECK(fake.enqueued == 2 && !fake.body_taken);
    fixture_destroy(&fixture);
}

static void test_partial_segment_retirement(bool page_redirects)
{
    Fixture fixture;
    CHECK(fixture_init(&fixture, page_redirects));
    char error[192] = {0};
    unsigned char bytes[256];
    size_t length = 0;
    uint64_t handle = psp_hls_start_sized(fixture.hls, PLAYLIST, 4096, 256,
                                         false, error, sizeof(error));
    CHECK(handle != 0);
    fake.outcome[0] = FAKE_PLAYLIST;
    CHECK(psp_hls_poll(fixture.hls, handle, bytes, sizeof(bytes), &length,
                      error, sizeof(error)) == MEDIA_HLS_TRANSPORT_CHUNK);
    CHECK(length != 0);
    fake.outcome[0] = FAKE_RETIRED;
    CHECK(psp_hls_poll(fixture.hls, handle, bytes, sizeof(bytes), &length,
                      error, sizeof(error)) == MEDIA_HLS_TRANSPORT_ERROR);
    CHECK(length == 0 && fake.enqueued == 1);
    CHECK(fixture.hls->last_transport_retryable);
    CHECK(psp_hls_request(fixture.hls, handle) == NULL);
    /* A dead logical handle cannot silently fetch and append a new body. */
    CHECK(psp_hls_poll(fixture.hls, handle, bytes, sizeof(bytes), &length,
                      error, sizeof(error)) == MEDIA_HLS_TRANSPORT_ERROR);
    CHECK(fake.enqueued == 1 && !fixture.hls->last_transport_retryable);
    fixture_destroy(&fixture);
}

typedef struct {
    Budget *budget;
    void *held;
    unsigned calls;
    unsigned release_stage;
    bool refuse;
    bool no_progress;
} MemoryRecovery;

static bool recover_memory(void *opaque, size_t needed, unsigned *stage)
{
    MemoryRecovery *recovery = opaque;
    CHECK(needed == 0); /* Also handles a fragmented native heap. */
    CHECK(*stage == recovery->calls);
    recovery->calls++;
    if (recovery->refuse) return false;
    if (!recovery->no_progress) (*stage)++;
    if (*stage == recovery->release_stage) {
        budget_free(recovery->budget, recovery->held);
        recovery->held = NULL;
    }
    return true;
}

/* Real playlist admission and parsing, then a source allocation refusal.
   Recovery must retain that parsed playlist, not refetch or silently raise
   the Budget. Cancellation after recovery must return every allocation. */
static void test_source_memory_recovery(
    bool page_redirects, unsigned release_stage, bool refuse, bool no_progress)
{
    Fixture fixture;
    CHECK(fixture_init(&fixture, page_redirects));
    fake.outcome[0] = FAKE_PLAYLIST;
    char error[192] = {0};
    CHECK(pump(&fixture, 1, error, sizeof(error))
          == PSP_MEDIA_HLS_OPEN_PENDING);
    CHECK(fake.body_taken);
    MemoryRecovery recovery = {
        .budget = &fixture.budget, .release_stage = release_stage,
        .refuse = refuse, .no_progress = no_progress
    };
    size_t original_limit = fixture.budget.limit;
    recovery.held = budget_malloc(&fixture.budget,
        budget_remaining(&fixture.budget) - 256u * 1024u);
    CHECK(recovery.held != NULL);
    psp_media_hls_set_memory_recovery(
        fixture.hls, &recovery, recover_memory);
    PspMediaHlsOpenStatus result = pump(&fixture, 1, error, sizeof(error));
    bool recovered = !refuse && !no_progress && release_stage <= 2u;
    CHECK(result == (recovered ? PSP_MEDIA_HLS_OPEN_PENDING
                              : PSP_MEDIA_HLS_OPEN_FAILED));
    CHECK((fixture.hls->source[0] != NULL) == recovered);
    CHECK(recovery.calls == (refuse || no_progress ? 1u
                           : release_stage <= 2u ? release_stage : 2u));
    CHECK(fake.enqueued == 1u);
    CHECK(fixture.budget.limit == original_limit);
    if (recovered) CHECK(error[0] == '\0');
    else CHECK(strstr(error, "HLS source exceeds memory budget") != NULL);
    budget_free(&fixture.budget, recovery.held);
    fixture_destroy(&fixture);
}

static void test_source_without_pressure(bool page_redirects)
{
    Fixture fixture;
    CHECK(fixture_init(&fixture, page_redirects));
    fake.outcome[0] = FAKE_PLAYLIST;
    MemoryRecovery recovery = {.budget = &fixture.budget};
    psp_media_hls_set_memory_recovery(
        fixture.hls, &recovery, recover_memory);
    char error[192] = {0};
    CHECK(pump(&fixture, 2, error, sizeof(error))
          == PSP_MEDIA_HLS_OPEN_PENDING);
    CHECK(fixture.hls->source[0] != NULL);
    CHECK(recovery.calls == 0);
    CHECK(error[0] == '\0');
    fixture_destroy(&fixture);
}

int main(void)
{
    for (int redirects = 0; redirects < 2; redirects++) {
        test_running_playlist_stays_pending(redirects != 0);
        test_failure_before_headers_retries_then_fails(redirects != 0);
        test_unadmitted_success_is_refused(redirects != 0);
        test_network_retirement(redirects != 0);
        test_partial_segment_retirement(redirects != 0);
        test_source_without_pressure(redirects != 0);
        test_source_memory_recovery(redirects != 0, 1u, false, false);
        test_source_memory_recovery(redirects != 0, 2u, false, false);
        test_source_memory_recovery(redirects != 0, 3u, false, false);
        test_source_memory_recovery(redirects != 0, 1u, true, false);
        test_source_memory_recovery(redirects != 0, 1u, false, true);
    }
    if (failures != 0) {
        fprintf(stderr, "%d psp-media-hls-transport check(s) failed\n",
                failures);
        return EXIT_FAILURE;
    }
    printf("psp-media-hls-transport tests passed\n");
    return EXIT_SUCCESS;
}
