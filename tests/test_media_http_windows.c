/*
 * The media range window cache against a scripted background worker.
 *
 * tests/test_media_http_range.c drives a real loopback server, so it cannot
 * hold one response half-delivered while the reader seeks elsewhere: the
 * window bookkeeping that depends on exactly when a fill lands is out of its
 * reach. This file includes src/media_http.c with the worker entry points and
 * the clock renamed to a deterministic fake, so a test decides how many bytes
 * of every request have arrived before each read. Every byte of the fake
 * resource is a function of its offset, so a read that returns another
 * window's bytes is caught directly, and the fake counts every byte it
 * delivers, which is what the device pays for on Wi-Fi.
 */
#define fetch_background_transport_available fake_transport_available
#define fetch_background_transport_initialize fake_transport_initialize
#define fetch_background_transport_stream_shape_supported \
    fake_transport_stream_shape_supported
#define fetch_background_transport_enqueue_media_stream_sized_diagnosed \
    fake_transport_enqueue_stream
#define fetch_background_transport_enqueue_media_diagnosed \
    fake_transport_enqueue_fixed
#define fetch_background_transport_progress fake_transport_progress
#define fetch_background_transport_take_media_headers \
    fake_transport_take_headers
#define fetch_background_transport_take_chunk fake_transport_take_chunk
#define fetch_background_transport_take_media_result_consumed \
    fake_transport_take_consumed
#define fetch_background_transport_take_media fake_transport_take_media
#define fetch_background_transport_cancel fake_transport_cancel
#define fetch_background_transport_wait fake_transport_wait
#define tilefinch_platform_monotonic_time_us fake_monotonic_time_us
#define tilefinch_platform_cooperate fake_cooperate

#include "../src/media_http.c"

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        printf("media-http-window-test failed at %s:%d: %s\n", \
               __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

#define WINDOW 4096u
#define FAKE_REQUESTS 8u

static unsigned char byte_at(uint64_t offset)
{
    return (unsigned char) ((offset * 2654435761u) >> 13);
}

typedef struct {
    uint64_t id;
    uint64_t offset;
    size_t length;
    size_t delivered;
    size_t taken;
    bool streaming;
    bool headers_taken;
} FakeRequest;

static struct {
    FakeRequest requests[FAKE_REQUESTS];
    uint64_t next_id;
    uint64_t clock_us;
    bool deliver_on_wait;
    /* A fixed take copies the body, then reports a response the range must
       refuse, as the native worker can. */
    bool reject_fixed;
    size_t issued;
    size_t delivered_bytes;
} fake;

static void fake_reset(void)
{
    memset(&fake, 0, sizeof(fake));
    fake.next_id = 1;
    fake.clock_us = 1000;
    fake.deliver_on_wait = true;
}

static FakeRequest *fake_find(uint64_t id)
{
    for (size_t at = 0; at < FAKE_REQUESTS; at++)
        if (id != 0 && fake.requests[at].id == id) return &fake.requests[at];
    return NULL;
}

static uint64_t fake_enqueue(const char *url, bool streaming)
{
    const char *span = strstr(url, "range=");
    unsigned long long first = 0, last = 0;
    if (span == NULL || sscanf(span, "range=%llu-%llu", &first, &last) != 2
        || last < first) return 0;
    for (size_t at = 0; at < FAKE_REQUESTS; at++) {
        if (fake.requests[at].id != 0) continue;
        fake.requests[at] = (FakeRequest) {
            .id = fake.next_id++, .offset = first,
            .length = (size_t) (last - first + 1u), .streaming = streaming
        };
        fake.issued++;
        return fake.requests[at].id;
    }
    return 0;
}

/* The network moves: every live request receives up to `bytes` more. */
static void fake_deliver(size_t bytes)
{
    for (size_t at = 0; at < FAKE_REQUESTS; at++) {
        FakeRequest *request = &fake.requests[at];
        if (request->id == 0) continue;
        size_t left = request->length - request->delivered;
        size_t moved = bytes < left ? bytes : left;
        request->delivered += moved;
        fake.delivered_bytes += moved;
    }
}

bool fake_transport_available(void) { return true; }

bool fake_transport_initialize(Budget *budget)
{
    (void) budget;
    return true;
}

bool fake_transport_stream_shape_supported(
    const char *url, const FetchRequest *request, size_t maximum_bytes,
    long timeout_ms, bool single_hop)
{
    (void) url; (void) request; (void) maximum_bytes; (void) timeout_ms;
    (void) single_hop;
    return true;
}

uint64_t fake_transport_enqueue_stream(
    const char *url, const FetchRequest *request, size_t maximum_bytes,
    long timeout_ms, size_t publication_bytes,
    FetchBackgroundEnqueueStatus *status)
{
    (void) request; (void) maximum_bytes; (void) timeout_ms;
    (void) publication_bytes;
    uint64_t id = fake_enqueue(url, true);
    *status = id != 0 ? FETCH_BACKGROUND_ENQUEUE_ADMITTED
                      : FETCH_BACKGROUND_ENQUEUE_SATURATED;
    return id;
}

uint64_t fake_transport_enqueue_fixed(
    const char *url, const FetchRequest *request, size_t maximum_bytes,
    long timeout_ms, FetchBackgroundEnqueueStatus *status)
{
    (void) request; (void) maximum_bytes; (void) timeout_ms;
    uint64_t id = fake_enqueue(url, false);
    *status = id != 0 ? FETCH_BACKGROUND_ENQUEUE_ADMITTED
                      : FETCH_BACKGROUND_ENQUEUE_SATURATED;
    return id;
}

bool fake_transport_progress(uint64_t id, FetchBackgroundProgress *progress)
{
    FakeRequest *request = fake_find(id);
    if (request == NULL) return false;
    *progress = (FetchBackgroundProgress) {
        .received_body_bytes = request->delivered,
        .available_body_bytes = request->delivered - request->taken,
        .status_code = 200, .active = true,
        .complete = request->delivered == request->length
    };
    return true;
}

bool fake_transport_take_headers(
    uint64_t id, FetchBackgroundMediaResponse *metadata)
{
    FakeRequest *request = fake_find(id);
    if (request == NULL || !request->streaming || request->headers_taken
        || request->delivered == 0) return false;
    request->headers_taken = true;
    memset(metadata, 0, sizeof(*metadata));
    metadata->status_code = 200;
    snprintf(metadata->content_length, sizeof(metadata->content_length),
             "%zu", request->length);
    return true;
}

bool fake_transport_take_chunk(
    uint64_t id, unsigned char *destination, size_t capacity, size_t *length)
{
    FakeRequest *request = fake_find(id);
    *length = 0;
    if (request == NULL) return false;
    size_t count = request->delivered - request->taken;
    if (count > capacity) count = capacity;
    for (size_t at = 0; at < count; at++)
        destination[at] = byte_at(request->offset + request->taken + at);
    request->taken += count;
    *length = count;
    return count != 0;
}

bool fake_transport_take_consumed(
    uint64_t id, FetchBackgroundMediaResponse *result, size_t consumed)
{
    FakeRequest *request = fake_find(id);
    memset(result, 0, sizeof(*result));
    if (request == NULL || request->delivered != request->length) return false;
    result->status_code = 200;
    result->length = request->length;
    result->success = consumed == request->length;
    request->id = 0;
    return result->success;
}

bool fake_transport_take_media(
    uint64_t id, unsigned char *destination, size_t capacity,
    FetchBackgroundMediaResponse *result)
{
    FakeRequest *request = fake_find(id);
    memset(result, 0, sizeof(*result));
    if (request == NULL || request->delivered != request->length
        || request->length > capacity) return false;
    for (size_t at = 0; at < request->length; at++)
        destination[at] = byte_at(request->offset + at);
    result->status_code = fake.reject_fixed ? 403 : 200;
    result->length = request->length;
    result->success = !fake.reject_fixed;
    request->id = 0;
    return true;
}

bool fake_transport_cancel(uint64_t id, const char *reason)
{
    (void) reason;
    FakeRequest *request = fake_find(id);
    if (request == NULL) return false;
    request->id = 0;
    return true;
}

void fake_transport_wait(unsigned milliseconds)
{
    (void) milliseconds;
    if (fake.deliver_on_wait) fake_deliver(WINDOW);
}

uint64_t fake_monotonic_time_us(void) { return fake.clock_us += 100; }

bool fake_cooperate(const char *phase, size_t progress)
{
    (void) phase; (void) progress;
    return true;
}

static size_t wrong_bytes(uint64_t offset, const unsigned char *bytes,
                          size_t length)
{
    size_t wrong = 0;
    for (size_t at = 0; at < length; at++)
        if (bytes[at] != byte_at(offset + at)) wrong++;
    return wrong;
}

/* Two owned successors and a muxed stream's immediate limit of two, as
   psp_media_open.c and psp_media_video_lookahead_limit configure the video
   range on the device. */
static MediaHttpRange *open_scripted_range(
    Budget *budget, unsigned windows, unsigned limit, size_t publication)
{
    MediaHttpRangeOptions options = {
        .cache_bytes = WINDOW,
        .lookahead_windows = 2,
        .initial_lookahead_windows = 1,
        .stream_publication_bytes = publication,
        .timeout_ms = 15000
    };
    char error[256] = {0};
    MediaHttpRange *range = media_http_range_create(
        budget, NULL, "https://media.test/v?x=1",
        (uint64_t) WINDOW * windows, &options, error, sizeof(error));
    if (range == NULL) {
        printf("media-http-window-test: create failed: %s\n", error);
        return NULL;
    }
    media_http_range_set_lookahead_limit(range, limit);
    return range;
}

/*
 * MD-01. A sequential refill streams into an unused owned slot. When the
 * active window changes under it (a promotion back into a retained window)
 * and the reader then demands the streamed window, the install must take the
 * streamed slot's bytes; it used to label the active buffer with the new
 * offset and keep the old window's bytes in it.
 */
static void test_streamed_window_installs_its_own_bytes(void)
{
    fake_reset();
    Budget budget;
    budget_init(&budget, 4u * 1024u * 1024u);
    MediaHttpRange *range = open_scripted_range(&budget, 64u, 2u, 1024u);
    CHECK(range != NULL);
    if (range == NULL) return;
    MediaRangeReader reader = media_http_range_reader(range);
    unsigned char buffer[256];

    /* 1. Window 0, then play into window 1: window 0 is retained. */
    CHECK(reader.read(reader.opaque, 0, buffer, 256));
    CHECK(wrong_bytes(0, buffer, 256) == 0);
    CHECK(reader.read(reader.opaque, 2048, buffer, 256));
    fake_deliver(WINDOW);
    (void) media_http_range_pump(range);
    CHECK(reader.read(reader.opaque, WINDOW + 16u, buffer, 256));
    CHECK(wrong_bytes(WINDOW + 16u, buffer, 256) == 0);

    /* 2. Seek forward to window 10. */
    CHECK(reader.read(reader.opaque, 10u * WINDOW, buffer, 256));
    CHECK(wrong_bytes(10u * WINDOW, buffer, 256) == 0);

    /* 3. A quarter into window 10, window 11 is issued as a streamed fill
       and its first kilobyte arrives. */
    fake.deliver_on_wait = false;
    CHECK(reader.poll(reader.opaque, 10u * WINDOW + 2048u, buffer, 256)
          == MEDIA_RANGE_READ_COMPLETE);
    fake_deliver(1024u);
    (void) media_http_range_pump(range);
    MediaHttpRangeStats stats = {0};
    CHECK(media_http_range_stats(range, &stats));
    CHECK(stats.cache_offset == 10u * WINDOW);
    /* Before the fix this fill streamed into the free slot; now window 1 is
       retained across the seek too, so it is a fixed fill. Either way the
       read below must see window 11's bytes. */
    CHECK(range->fill_request != 0 && range->fill_offset == 11u * WINDOW);

    /* 4. Seek back into window 0, served from its retained slot while the
       streamed fill stays in flight. */
    CHECK(reader.poll(reader.opaque, 100, buffer, 256)
          == MEDIA_RANGE_READ_COMPLETE);
    CHECK(wrong_bytes(100, buffer, 256) == 0);
    CHECK(media_http_range_stats(range, &stats));
    CHECK(stats.cache_offset == 0);
    CHECK(range->fill_request != 0 && range->fill_offset == 11u * WINDOW);

    /* 5. Seek forward into window 11 as its streamed fill completes. */
    MediaRangeReadStatus status = reader.poll(
        reader.opaque, 11u * WINDOW + 2000u, buffer, 256);
    if (status == MEDIA_RANGE_READ_WOULD_BLOCK) {
        fake_deliver(WINDOW);
        status = reader.poll(reader.opaque, 11u * WINDOW + 2000u, buffer, 256);
    }
    CHECK(status == MEDIA_RANGE_READ_COMPLETE);
    size_t wrong = wrong_bytes(11u * WINDOW + 2000u, buffer, 256);
    if (wrong != 0)
        printf("media-http-window-test: window 11 read returned %zu/256 "
               "foreign bytes\n", wrong);
    CHECK(wrong == 0);
    CHECK(media_http_range_stats(range, &stats));
    CHECK(stats.cache_offset == 11u * WINDOW);
    status = reader.poll(reader.opaque, 11u * WINDOW + 3000u, buffer, 256);
    CHECK(status == MEDIA_RANGE_READ_COMPLETE
          && wrong_bytes(11u * WINDOW + 3000u, buffer, 256) == 0);
    /* Window 0, which was active when the streamed window installed, is
       still owned: going back costs no request. */
    size_t issued = fake.issued;
    CHECK(reader.poll(reader.opaque, 300, buffer, 256)
          == MEDIA_RANGE_READ_COMPLETE);
    CHECK(wrong_bytes(300, buffer, 256) == 0);
    CHECK(fake.issued == issued);

    media_http_range_destroy(range);
    CHECK(budget.current == 0);
}

/*
 * The streamed half of MD-01 from a fresh open: one window, a seek (now
 * retained in a slot), a streamed successor of the seek target in the other
 * slot, a return to the first window, then the successor while its last
 * bytes are published but not yet drained.
 */
static void test_streamed_install_with_unrelated_active_window(void)
{
    fake_reset();
    Budget budget;
    budget_init(&budget, 4u * 1024u * 1024u);
    MediaHttpRange *range = open_scripted_range(&budget, 64u, 2u, 1024u);
    CHECK(range != NULL);
    if (range == NULL) return;
    MediaRangeReader reader = media_http_range_reader(range);
    unsigned char buffer[256];
    CHECK(reader.read(reader.opaque, 0, buffer, 256)
          && wrong_bytes(0, buffer, 256) == 0);
    CHECK(reader.read(reader.opaque, 10u * WINDOW, buffer, 256)
          && wrong_bytes(10u * WINDOW, buffer, 256) == 0);
    fake.deliver_on_wait = false;
    CHECK(reader.poll(reader.opaque, 10u * WINDOW + 2048u, buffer, 256)
          == MEDIA_RANGE_READ_COMPLETE);
    fake_deliver(1024u);
    (void) media_http_range_pump(range);
    CHECK(range->fill_streaming && range->fill_offset == 11u * WINDOW);
    size_t issued = fake.issued;
    CHECK(reader.poll(reader.opaque, 100, buffer, 256)
          == MEDIA_RANGE_READ_COMPLETE);
    CHECK(wrong_bytes(100, buffer, 256) == 0);
    CHECK(fake.issued == issued);
    CHECK(range->fill_streaming && range->fill_offset == 11u * WINDOW);
    fake_deliver(WINDOW);
    MediaRangeReadStatus status = reader.poll(
        reader.opaque, 11u * WINDOW + 2000u, buffer, 256);
    CHECK(status == MEDIA_RANGE_READ_COMPLETE
          && wrong_bytes(11u * WINDOW + 2000u, buffer, 256) == 0);
    MediaHttpRangeStats stats = {0};
    CHECK(media_http_range_stats(range, &stats)
          && stats.cache_offset == 11u * WINDOW);
    CHECK(reader.poll(reader.opaque, 11u * WINDOW + 3500u, buffer, 256)
          == MEDIA_RANGE_READ_COMPLETE
          && wrong_bytes(11u * WINDOW + 3500u, buffer, 256) == 0);
    media_http_range_destroy(range);
    CHECK(budget.current == 0);
}

/*
 * A range with no owned slots takes every response into the active buffer.
 * When the take copied a body and the response was then refused, the active
 * window kept its old offset over the refused bytes.
 */
static void test_rejected_take_retires_the_active_window(void)
{
    fake_reset();
    Budget budget;
    budget_init(&budget, 4u * 1024u * 1024u);
    MediaHttpRangeOptions options = {
        .cache_bytes = WINDOW, .stream_publication_bytes = 1024u,
        .timeout_ms = 15000
    };
    char error[256] = {0};
    MediaHttpRange *range = media_http_range_create(
        &budget, NULL, "https://media.test/a?x=1", (uint64_t) WINDOW * 16u,
        &options, error, sizeof(error));
    CHECK(range != NULL);
    if (range == NULL) return;
    MediaRangeReader reader = media_http_range_reader(range);
    unsigned char buffer[256];
    CHECK(reader.read(reader.opaque, 100, buffer, 256)
          && wrong_bytes(100, buffer, 256) == 0);
    fake.reject_fixed = true;
    CHECK(!reader.read(reader.opaque, 5u * WINDOW, buffer, 256));
    fake.reject_fixed = false;
    size_t issued = fake.issued;
    CHECK(reader.read(reader.opaque, 100, buffer, 256));
    size_t wrong = wrong_bytes(100, buffer, 256);
    if (wrong != 0)
        printf("media-http-window-test: window 0 returned %zu/256 bytes of "
               "a refused response\n", wrong);
    CHECK(wrong == 0);
    CHECK(fake.issued == issued + 1u);
    media_http_range_destroy(range);
    CHECK(budget.current == 0);
}

/*
 * The same property over a scripted scrub: play into a window, seek
 * elsewhere, return behind the first spot, then jump past the seek target,
 * forty times at offsets and delivery sizes drawn from a fixed LCG. Every
 * completed read must return the bytes at its own offset. Deterministic and
 * bounded; it is one input, not a search.
 */
static uint32_t scrub_next(uint32_t *state)
{
    *state = *state * 1103515245u + 12345u;
    return *state >> 8;
}

static bool scrub_read(MediaHttpRange *range, MediaRangeReader reader,
                       uint64_t offset, size_t length, uint32_t *state,
                       size_t *corrupt)
{
    unsigned char buffer[512];
    for (unsigned attempt = 0; attempt < 64u; attempt++) {
        MediaRangeReadStatus status =
            reader.poll(reader.opaque, offset, buffer, length);
        if (status == MEDIA_RANGE_READ_COMPLETE) {
            if (wrong_bytes(offset, buffer, length) != 0) (*corrupt)++;
            return true;
        }
        if (status == MEDIA_RANGE_READ_FAILED) return false;
        /* Sometimes the frame pump runs before the network moves again,
           sometimes the next read finds published bytes nobody drained. */
        uint32_t draw = scrub_next(state);
        if ((draw & 1u) != 0) (void) media_http_range_pump(range);
        fake_deliver(128u + (draw >> 4) % 2048u);
    }
    return false;
}

static bool scrub_play(MediaHttpRange *range, MediaRangeReader reader,
                       uint64_t from, size_t bytes, uint32_t *state,
                       size_t *corrupt, size_t *reads)
{
    for (size_t played = 0; played < bytes; played += 300u) {
        if (!scrub_read(range, reader, from + played, 256u, state, corrupt))
            return false;
        (*reads)++;
        uint32_t draw = scrub_next(state);
        if ((draw & 3u) == 0) (void) media_http_range_pump(range);
        fake_deliver((draw >> 4) % 1024u);
    }
    return true;
}

static void test_scripted_scrub_never_returns_foreign_bytes(void)
{
    const unsigned windows = 32u;
    uint32_t state = 0x1109u;
    size_t corrupt = 0, reads = 0, failed = 0;
    /* A fresh range per cycle reaches the state where an owned slot is
       still free (and the next refill streams into it) as well as the
       full-slot state the second pass of each cycle runs in. */
    for (unsigned cycle = 0; cycle < 40u; cycle++) {
        fake_reset();
        fake.deliver_on_wait = false;
        Budget budget;
        budget_init(&budget, 4u * 1024u * 1024u);
        MediaHttpRange *range =
            open_scripted_range(&budget, windows, 2u, 512u);
        CHECK(range != NULL);
        if (range == NULL) return;
        MediaRangeReader reader = media_http_range_reader(range);
        for (unsigned pass = 0; pass < 2u; pass++) {
            uint64_t first = scrub_next(&state) % (windows - 4u);
            uint64_t second = scrub_next(&state) % (windows - 4u);
            if (second + 2u >= first && second <= first + 2u)
                second = (first + 8u) % (windows - 4u);
            uint64_t a = first * WINDOW, b = second * WINDOW;
            size_t jump = scrub_next(&state) % (WINDOW - 512u);
            bool played =
                scrub_play(range, reader, a, WINDOW + 600u, &state,
                           &corrupt, &reads)
                && scrub_play(range, reader, b, WINDOW / 3u + 300u, &state,
                              &corrupt, &reads)
                && scrub_play(range, reader, a + 100u, 600u, &state,
                              &corrupt, &reads)
                && scrub_play(range, reader, b + WINDOW + jump, 900u,
                              &state, &corrupt, &reads);
            if (!played) failed++;
        }
        media_http_range_destroy(range);
        CHECK(budget.current == 0);
    }
    if (corrupt != 0)
        printf("media-http-window-test: scrub returned foreign bytes in "
               "%zu of %zu reads\n", corrupt, reads);
    CHECK(corrupt == 0);
    CHECK(failed == 0);
    CHECK(reads > 1000u);
}

/*
 * MD-02. A muxed fragmented MP4 whose fragments hold the video run and then
 * the audio run is read in decode order, so one range serves two read
 * positions up to a fragment apart. Each window must be downloaded about
 * once: the cache used to evict the window the other track still read, and
 * refetched 256 KiB windows on every switch.
 *
 * Returns the bytes the fake delivered, in tenths of the file size.
 */
static unsigned two_position_fetch_x10(
    unsigned fragment_x10, unsigned limit, size_t step_bytes,
    size_t *corrupt_out)
{
    fake_reset();
    fake.deliver_on_wait = false;
    Budget budget;
    budget_init(&budget, 4u * 1024u * 1024u);
    const unsigned windows = 128u;
    MediaHttpRange *range = open_scripted_range(&budget, windows, limit, 256u);
    if (range == NULL) {
        failures++;
        return 0;
    }
    MediaRangeReader reader = media_http_range_reader(range);
    uint64_t total = (uint64_t) WINDOW * windows;
    size_t fragment = (size_t) WINDOW * fragment_x10 / 10u;
    size_t video_part = fragment * 85u / 100u;
    const size_t video_samples = 120u, audio_samples = 172u;
    size_t video_size = video_part / video_samples;
    size_t audio_size = (fragment - video_part) / audio_samples;
    unsigned char buffer[1024];
    size_t corrupt = 0;
    unsigned long steps = 0;
    for (uint64_t base = 0; base + fragment <= total; base += fragment) {
        size_t video = 0, audio = 0;
        while (video < video_samples || audio < audio_samples) {
            bool take_video = audio >= audio_samples
                || (video < video_samples
                    && video * audio_samples <= audio * video_samples);
            uint64_t offset = take_video ? base + video * video_size
                : base + video_part + audio * audio_size;
            size_t length = take_video ? video_size : audio_size;
            MediaRangeReadStatus status =
                reader.poll(reader.opaque, offset, buffer, length);
            if (status == MEDIA_RANGE_READ_COMPLETE) {
                if (wrong_bytes(offset, buffer, length) != 0) corrupt++;
            }
            if (status != MEDIA_RANGE_READ_WOULD_BLOCK) {
                if (take_video) video++;
                else audio++;
            }
            /* One frame: the network moves a little, then the range pump. */
            fake_deliver(step_bytes);
            (void) media_http_range_pump(range);
            if (++steps > 2000000ul) goto done;
        }
    }
done:
    media_http_range_destroy(range);
    CHECK(budget.current == 0);
    *corrupt_out = corrupt;
    return (unsigned) (fake.delivered_bytes * 10u / total);
}

static void test_two_read_positions_fetch_each_window_about_once(void)
{
    /* Before the fix the two-position cases fetched 23x to 81x the file.
       With one owned slot (a budget fallback; muxed streams ask for two) the
       range holds two windows for two positions plus a crossing; a sample
       that straddles a crossing there takes a shifted window that overlaps
       the next aligned one. */
    static const struct {
        unsigned fragment_x10;
        unsigned limit;
        size_t step;
        unsigned maximum_x10;
    } cases[] = {
        { 1u, 2u, 700u, 10u },   /* tight interleave: one reader in effect */
        { 8u, 2u, 300u, 15u },
        { 8u, 2u, 700u, 15u },
        { 8u, 2u, 2000u, 15u },
        { 8u, 1u, 700u, 15u },
        { 25u, 2u, 700u, 15u },
        { 25u, 1u, 700u, 25u },
    };
    for (size_t at = 0; at < sizeof(cases) / sizeof(cases[0]); at++) {
        size_t corrupt = 0;
        unsigned fetched = two_position_fetch_x10(
            cases[at].fragment_x10, cases[at].limit, cases[at].step,
            &corrupt);
        printf("media-http-window-test: fragment %u.%u windows, limit %u, "
               "%zuB/step: fetched %u.%ux the file\n",
               cases[at].fragment_x10 / 10u, cases[at].fragment_x10 % 10u,
               cases[at].limit, cases[at].step, fetched / 10u,
               fetched % 10u);
        CHECK(corrupt == 0);
        CHECK(fetched >= 10u && fetched <= cases[at].maximum_x10);
    }
}

int main(void)
{
    media_http_test_force_background_available(1);
    test_streamed_window_installs_its_own_bytes();
    test_streamed_install_with_unrelated_active_window();
    test_rejected_take_retires_the_active_window();
    test_scripted_scrub_never_returns_foreign_bytes();
    test_two_read_positions_fetch_each_window_about_once();
    media_http_test_force_background_available(-1);
    if (failures != 0) {
        printf("media-http-window-test: %d failure(s)\n", failures);
        return 1;
    }
    puts("media-http-window-test: ok");
    return 0;
}
