#include "tilefinch/budget.h"
#include "tilefinch/fetch.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifndef TILEFINCH_TEST_SOURCE_DIR
#define TILEFINCH_TEST_SOURCE_DIR "."
#endif

typedef struct {
    unsigned char bytes[512];
    size_t length;
    size_t headers;
    size_t stalls;
} Consumer;

static bool headers(void *opaque, const FetchResult *metadata)
{
    Consumer *consumer = opaque;
    consumer->headers++;
    return metadata->status_code == 200
        && strstr(metadata->content_type, "text/html") != NULL;
}

static bool body(void *opaque, const unsigned char *data, size_t length)
{
    Consumer *consumer = opaque;
    if (length > sizeof(consumer->bytes) - consumer->length) return false;
    memcpy(consumer->bytes + consumer->length, data, length);
    consumer->length += length;
    return true;
}

static bool stall(void *opaque)
{
    Consumer *consumer = opaque;
    consumer->stalls++;
    return true;
}

static bool replay_begin(void)
{
    char error[256] = {0};
    return fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-stream", error,
        sizeof(error));
}

static FetchRequest request(void)
{
    return (FetchRequest) {
        .method = "GET",
        .send_low_client_hints = true,
        .sec_fetch_user = true,
        .upgrade_insecure_requests = true
    };
}

static bool test_ca_bundle_configuration(void)
{
    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    bool ok = fetch_transport_version() != NULL
        && fetch_transport_version()[0] != '\0'
        && fetch_transport_tls_version() != NULL
        && fetch_transport_tls_version()[0] != '\0'
        && fetch_transport_http2_version() != NULL
        && fetch_transport_http2_version()[0] != '\0'
        && strcmp(fetch_http_version_name(0), "unknown") == 0
        && fetch_set_ca_bundle_path("/tmp/tilefinch-roots.pem")
        && fetch_ca_bundle_path() != NULL
        && strcmp(
               fetch_ca_bundle_path(), "/tmp/tilefinch-roots.pem") == 0
        && !fetch_set_ca_bundle_path("/tmp/roots.pem\ninjected")
        && strcmp(
               fetch_ca_bundle_path(), "/tmp/tilefinch-roots.pem") == 0;
    FetchScheduler *scheduler =
        fetch_scheduler_create(&budget, 1, 4096);
    ok = ok && scheduler != NULL
        && !fetch_set_ca_bundle_path("/tmp/replaced.pem")
        && strcmp(
               fetch_ca_bundle_path(), "/tmp/tilefinch-roots.pem") == 0;
    fetch_scheduler_destroy(scheduler);
    ok = ok && fetch_set_ca_bundle_path(NULL)
        && fetch_ca_bundle_path() == NULL
        && budget.current == 0;
    return ok;
}

/*
 * Handshake counters (docs/engineering/PSP_TRANSPORT.md).
 *
 * The replay transport never opens a socket, so it must report every
 * handshake field as absent rather than as a measured zero: a scheduler that
 * served only replayed responses has classified no transfers at all, and the
 * rendered line must say "absent" instead of "0" for the µs totals.
 */
static bool test_replay_reports_absent_handshake_fields(void)
{
    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    fetch_tls_handshake_counters_reset();
    if (!replay_begin()) return false;
    FetchScheduler *scheduler = fetch_scheduler_create(&budget, 1, 4096);
    FetchRequest fetch_request = request();
    uint64_t id = scheduler == NULL ? 0 : fetch_scheduler_enqueue(
        scheduler, "https://stream.test/document", &fetch_request,
        4096, 1000);
    FetchResult result = {.budget = &budget};
    bool success = false;
    bool taken = false;
    for (size_t i = 0; id != 0 && i < 32; i++) {
        (void) fetch_scheduler_pump(scheduler, 1, 0);
        if (fetch_scheduler_take(scheduler, id, &success, &result)) {
            taken = true;
            break;
        }
    }
    FetchTlsHandshakeCounters view = {.transfers = 99};
    FetchTlsHandshakeCounters totals = {.transfers = 99};
    char rendered[128] = {0};
    bool ok = taken && success
        && !result.tls_handshake_measured
        && result.tls_handshake_us == 0
        && !result.tls_connection_reuse_known
        && !result.tls_connection_reused
        && result.tls_version[0] == '\0'
        && fetch_scheduler_tls_handshake_counters(scheduler, &view)
        && view.transfers == 0 && view.full_handshakes == 0
        && view.reused_connections == 0 && view.measured == 0
        && view.total_handshake_us == 0 && view.maximum_handshake_us == 0;
    fetch_tls_handshake_counters(&totals);
    ok = ok && totals.transfers == 0 && totals.measured == 0
        && fetch_tls_handshake_counters_format(
               &totals, rendered, sizeof(rendered))
        && strcmp(rendered,
                  "transfers=0 full=0 reused=0 measured=0 "
                  "total-us=absent max-us=absent") == 0
        && !fetch_scheduler_tls_handshake_counters(NULL, &view)
        && view.transfers == 0;
    if (!ok) {
        fprintf(stderr,
                "replay handshake fields taken=%d success=%d measured=%d "
                "reuse-known=%d rendered=\"%s\"\n",
                (int) taken, (int) success,
                (int) result.tls_handshake_measured,
                (int) result.tls_connection_reuse_known, rendered);
    }
    fetch_result_destroy(&result);
    fetch_scheduler_destroy(scheduler);
    fetch_trace_end();
    return ok && budget.current == 0;
}

/* The rendered line is the transport log's contract, so pin both shapes. */
static bool test_handshake_counter_rendering(void)
{
    FetchTlsHandshakeCounters counters = {
        .transfers = 5,
        .full_handshakes = 2,
        .reused_connections = 3,
        .measured = 2,
        .total_handshake_us = 1900000,
        .maximum_handshake_us = 1100000
    };
    char rendered[128] = {0};
    char narrow[8] = {0};
    bool ok = fetch_tls_handshake_counters_format(
                  &counters, rendered, sizeof(rendered))
        && strcmp(rendered,
                  "transfers=5 full=2 reused=3 measured=2 "
                  "total-us=1900000 max-us=1100000") == 0;
    /* Classified but unmeasured transfers must not render as "0 µs". */
    counters.measured = 0;
    counters.total_handshake_us = 0;
    counters.maximum_handshake_us = 0;
    ok = ok && fetch_tls_handshake_counters_format(
                   &counters, rendered, sizeof(rendered))
        && strcmp(rendered,
                  "transfers=5 full=2 reused=3 measured=0 "
                  "total-us=absent max-us=absent") == 0
        && !fetch_tls_handshake_counters_format(
               &counters, narrow, sizeof(narrow))
        && narrow[0] == '\0'
        && !fetch_tls_handshake_counters_format(NULL, rendered,
                                                sizeof(rendered))
        && !fetch_tls_handshake_counters_format(&counters, NULL, 16);
    if (!ok) fprintf(stderr, "handshake rendering \"%s\"\n", rendered);
    return ok;
}

static bool test_retained_failure_replay_delay(void)
{
    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    char error[256] = {0};
    bool ready = fetch_trace_replay_begin_response_keyed(
        TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-response-key-failure",
        error, sizeof(error));
    FetchScheduler *scheduler = ready
        ? fetch_scheduler_create(&budget, 1, 4096) : NULL;
    FetchRequest fetch_request = {.method = "GET"};
    uint64_t id = scheduler == NULL ? 0 : fetch_scheduler_enqueue(
        scheduler, "https://visual-failure.test/resource",
        &fetch_request, 4096, 1000);
    FetchResult result = {.budget = &budget};
    bool success = true;
    bool premature = false;
    size_t first = id == 0 ? SIZE_MAX
        : fetch_scheduler_pump(scheduler, 1, 0);
    premature = id != 0
        && fetch_scheduler_take(scheduler, id, &success, &result);
    size_t second = id == 0 ? SIZE_MAX
        : fetch_scheduler_pump(scheduler, 1, 0);
    premature = premature || (id != 0
        && fetch_scheduler_take(scheduler, id, &success, &result));
    size_t third = id == 0 ? 0
        : fetch_scheduler_pump(scheduler, 1, 0);
    bool taken = id != 0
        && fetch_scheduler_take(scheduler, id, &success, &result);
    FetchTraceReplayStats stats = {0};
    bool ok = ready && scheduler != NULL && id != 0 && !premature
        && first == 0 && second == 0 && third == 1
        && taken && !success
        && result.status_code == 503
        && strcmp(result.error, "fixture retained failure") == 0
        && strcmp(result.server, "fixture") == 0
        && result.trace_delay_pumps == 3
        && result.data == NULL && result.set_cookie_count == 0
        && fetch_trace_replay_stats(&stats)
        && stats.response_keyed && stats.record_count == 3
        && stats.request_count == 1 && stats.matched_request_count == 1
        && stats.served_request_count == 0
        && stats.rejected_request_count == 1
        && stats.unmatched_request_count == 0
        && stats.conflicting_request_count == 0
        && stats.invalid_route_request_count == 0
        && stats.claimed_record_count == 1
        && stats.occurrence_claim_count == 0
        && stats.reusable_claim_count == 1
        && stats.occurrence_exhausted_count == 0
        && fetch_trace_replay_record_was_claimed(0);
    fetch_result_destroy(&result);

    uint64_t external_id = ok ? fetch_scheduler_enqueue(
        scheduler, "https://visual-failure.test/external-cancel",
        &fetch_request, 4096, 1000) : 0;
    result = (FetchResult) {.budget = &budget};
    success = true;
    size_t external_first = external_id == 0 ? SIZE_MAX
        : fetch_scheduler_pump(scheduler, 1, 0);
    bool external_premature = external_id != 0
        && fetch_scheduler_take(
               scheduler, external_id, &success, &result);
    size_t external_second = external_id == 0 ? 0
        : fetch_scheduler_pump(scheduler, 1, 0);
    bool external_taken = external_id != 0
        && fetch_scheduler_take(scheduler, external_id, &success, &result);
    ok = ok && external_id != 0 && external_first == 0
        && !external_premature && external_second == 1
        && external_taken && !success
        && result.trace_external_cancel && !result.timed_out
        && result.trace_delay_pumps == 2 && result.status_code == 0
        && strcmp(result.error,
                  "fixture retained external cancellation") == 0
        && result.data == NULL && result.set_cookie_count == 0
        && fetch_trace_replay_stats(&stats)
        && stats.request_count == 2 && stats.matched_request_count == 2
        && stats.served_request_count == 0
        && stats.rejected_request_count == 2
        && stats.invalid_route_request_count == 0
        && stats.claimed_record_count == 2
        && stats.reusable_claim_count == 2
        && fetch_trace_replay_record_was_claimed(1);
    fetch_result_destroy(&result);

    uint64_t invalid_id = ok ? fetch_scheduler_enqueue(
        scheduler, "https://visual-failure.test/impossible-success",
        &fetch_request, 4096, 1000) : 0;
    result = (FetchResult) {.budget = &budget};
    success = true;
    size_t invalid_pump = invalid_id == 0 ? 0
        : fetch_scheduler_pump(scheduler, 1, 0);
    bool invalid_taken = invalid_id != 0
        && fetch_scheduler_take(scheduler, invalid_id, &success, &result);
    ok = ok && invalid_id != 0 && invalid_pump == 1
        && invalid_taken && !success
        && strstr(result.error, "response is corrupt") != NULL
        && result.data == NULL && result.set_cookie_count == 0
        && fetch_trace_replay_stats(&stats)
        && stats.request_count == 3 && stats.matched_request_count == 2
        && stats.rejected_request_count == 2
        && stats.invalid_route_request_count == 1
        && stats.claimed_record_count == 2
        && !fetch_trace_replay_record_was_claimed(2);
    fetch_result_destroy(&result);
    fetch_scheduler_destroy(scheduler);
    fetch_trace_end();

    memset(error, 0, sizeof(error));
    bool strict_ready = fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-response-key-failure",
        error, sizeof(error));
    scheduler = strict_ready
        ? fetch_scheduler_create(&budget, 1, 4096) : NULL;
    uint64_t strict_id = scheduler == NULL ? 0 : fetch_scheduler_enqueue(
        scheduler, "https://visual-failure.test/external-cancel",
        &fetch_request, 4096, 1000);
    bool strict_completed = false;
    result = (FetchResult) {.budget = &budget};
    success = true;
    for (size_t pump = 0; pump < 3 && strict_id != 0; pump++) {
        ok = ok && fetch_scheduler_pump(scheduler, 1, 0) == 0;
        strict_completed = strict_completed || fetch_scheduler_take(
            scheduler, strict_id, &success, &result);
    }
    bool strict_cancelled = strict_id != 0 && !strict_completed
        && fetch_scheduler_cancel(
               scheduler, strict_id, "strict caller cancellation");
    bool strict_taken = strict_id != 0 && fetch_scheduler_take(
        scheduler, strict_id, &success, &result);
    ok = ok && strict_ready && scheduler != NULL && strict_id != 0
        && !strict_completed && strict_cancelled && strict_taken && !success
        && result.trace_external_cancel && result.trace_delay_pumps == 2
        && strcmp(result.error, "strict caller cancellation") == 0
        && result.data == NULL && result.set_cookie_count == 0;
    fetch_result_destroy(&result);
    fetch_scheduler_destroy(scheduler);
    fetch_trace_end();

    memset(error, 0, sizeof(error));
    bool overflow_ready = fetch_trace_replay_begin_response_keyed(
        TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-response-key-occurrence",
        error, sizeof(error));
    scheduler = overflow_ready
        ? fetch_scheduler_create(&budget, 1, 4096) : NULL;
    uint64_t overflow_id = scheduler == NULL ? 0 : fetch_scheduler_enqueue(
        scheduler, "https://visual-occurrence.test/delay-overflow",
        &fetch_request, 4096, 1000);
    result = (FetchResult) {.budget = &budget};
    success = true;
    size_t overflow_pump = overflow_id == 0 ? 0
        : fetch_scheduler_pump(scheduler, 1, 0);
    bool overflow_taken = overflow_id != 0 && fetch_scheduler_take(
        scheduler, overflow_id, &success, &result);
    ok = ok && overflow_ready && scheduler != NULL && overflow_id != 0
        && overflow_pump == 1 && overflow_taken && !success
        && result.trace_delay_pumps == 1000001
        && strstr(result.error, "metadata is corrupt") != NULL
        && result.data == NULL && result.set_cookie_count == 0
        && fetch_trace_replay_stats(&stats)
        && stats.request_count == 1 && stats.matched_request_count == 0
        && stats.invalid_route_request_count == 1
        && stats.claimed_record_count == 0
        && !fetch_trace_replay_record_was_claimed(10);
    fetch_result_destroy(&result);
    fetch_scheduler_destroy(scheduler);
    fetch_trace_end();
    return ok && budget.current == 0;
}

static bool test_accepted_critical_client_hints(void)
{
    char accepted[128] = "stale";
    if (!fetch_accepted_critical_client_hints(
            "Sec-CH-UA-Arch, Sec-CH-UA-Model",
            "Sec-CH-UA-Arch", accepted, sizeof(accepted))
        || strcmp(accepted, "Sec-CH-UA-Arch") != 0) return false;

    /* Critical-CH is not an opt-in by itself, and unsupported hints remain
       unavailable even when both response fields name them. */
    if (!fetch_accepted_critical_client_hints(
            "Sec-CH-UA-Model", "Sec-CH-UA-Arch",
            accepted, sizeof(accepted))
        || accepted[0] != '\0'
        || !fetch_accepted_critical_client_hints(
            "Sec-CH-UA-WoW64", "Sec-CH-UA-WoW64",
            accepted, sizeof(accepted))
        || accepted[0] != '\0') return false;

    /* Preserve Critical-CH order while canonicalizing whitespace and removing
       case-insensitive duplicates. Unsupported entries do not poison valid
       supported entries in the same well-formed lists. */
    if (!fetch_accepted_critical_client_hints(
            " sec-ch-ua-model , Sec-CH-UA-Arch, Sec-CH-UA-Arch ",
            " Sec-CH-UA-Arch, sec-ch-ua-arch, Sec-CH-UA-WoW64, "
            "Sec-CH-UA-Model ",
            accepted, sizeof(accepted))
        || strcmp(accepted,
                  "Sec-CH-UA-Arch, Sec-CH-UA-Model") != 0) return false;

    static const struct {
        const char *accept_ch;
        const char *critical_ch;
    } malformed[] = {
        {"Sec-CH-UA-Arch,", "Sec-CH-UA-Arch"},
        {"Sec-CH-UA-Arch", "Sec-CH-UA-Arch,"},
        {"Sec-CH-UA-Arch;v=1", "Sec-CH-UA-Arch"},
        {"Sec-CH-UA-Arch", "Sec-CH-UA-Arch,,Sec-CH-UA-Model"},
        {NULL, "Sec-CH-UA-Arch"},
        {"Sec-CH-UA-Arch", NULL}
    };
    for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); i++) {
        strcpy(accepted, "stale");
        if (!fetch_accepted_critical_client_hints(
                malformed[i].accept_ch, malformed[i].critical_ch,
                accepted, sizeof(accepted))
            || accepted[0] != '\0') return false;
    }
    return true;
}

static bool test_request_validation_before_replay(void)
{
    FetchRequest candidate = request();
    FetchRequestValidationError error = FETCH_REQUEST_VALIDATION_OK;

#define REJECT_HEADER(field, value) do {                                    \
    candidate = request();                                                   \
    candidate.field = (value);                                               \
    error = FETCH_REQUEST_VALIDATION_OK;                                     \
    if (fetch_request_validate(&candidate, &error)                            \
        || error != FETCH_REQUEST_VALIDATION_HEADER_VALUE) return false;     \
} while (0)
    REJECT_HEADER(content_type, "text/plain\r\nX-Injected: yes");
    REJECT_HEADER(cookie, "safe=value\nX-Injected: yes");
    REJECT_HEADER(if_none_match, "value\twith-tab");
    REJECT_HEADER(if_modified_since, "date\x7f");
    REJECT_HEADER(referer, "https://example.test/\rbad");
    REJECT_HEADER(origin, "https://example.test/\nbad");
    REJECT_HEADER(accept, "text/html\t,*/*");
    REJECT_HEADER(sec_fetch_dest, "document\r");
    REJECT_HEADER(sec_fetch_dest, "");
    REJECT_HEADER(sec_fetch_mode, "navigate\n");
    REJECT_HEADER(sec_fetch_mode, "");
    REJECT_HEADER(sec_fetch_site, "same-origin\x1f");
    REJECT_HEADER(sec_fetch_site, "");
    REJECT_HEADER(user_agent, "Tilefinch\x7f");
    REJECT_HEADER(referrer_policy, "no-referrer\t");
#undef REJECT_HEADER

    candidate = request();
    candidate.method = "GET\r\nX-Injected: yes";
    error = FETCH_REQUEST_VALIDATION_OK;
    if (fetch_request_validate(&candidate, &error)
        || error != FETCH_REQUEST_VALIDATION_METHOD) return false;
    candidate.method = "GET /";
    if (fetch_request_validate(&candidate, NULL)) return false;
    candidate.method = "";
    if (fetch_request_validate(&candidate, NULL)) return false;
    static const char *const forbidden_methods[] = {
        "CONNECT", "trace", "TrAcK"
    };
    for (size_t i = 0;
         i < sizeof(forbidden_methods) / sizeof(forbidden_methods[0]); i++) {
        candidate = request();
        candidate.method = forbidden_methods[i];
        error = FETCH_REQUEST_VALIDATION_OK;
        if (fetch_request_validate(&candidate, &error)
            || error != FETCH_REQUEST_VALIDATION_METHOD) return false;
    }

    candidate = request();
    candidate.body = "x";
    candidate.body_length = SIZE_MAX;
    error = FETCH_REQUEST_VALIDATION_OK;
    if (fetch_request_validate(&candidate, &error)
        || error != FETCH_REQUEST_VALIDATION_BODY) return false;
    candidate.body_length = 64u * 1024u + 1u;
    if (fetch_request_validate(&candidate, NULL)) return false;
    candidate = request();
    candidate.body = "x";
    candidate.body_length = 1;
    if (fetch_request_validate(&candidate, NULL)) return false;
    candidate.method = "HEAD";
    if (fetch_request_validate(&candidate, NULL)) return false;

    candidate = request();
    candidate.credentials = (FetchCredentialPolicy) -1;
    error = FETCH_REQUEST_VALIDATION_OK;
    if (fetch_request_validate(&candidate, &error)
        || error != FETCH_REQUEST_VALIDATION_CREDENTIALS) return false;
    candidate = request();
    candidate.initiator_url = "not an absolute HTTP URL";
    error = FETCH_REQUEST_VALIDATION_OK;
    if (fetch_request_validate(&candidate, &error)
        || error != FETCH_REQUEST_VALIDATION_CONTEXT) return false;

    candidate = request();
    candidate.enforce_cors = true;
    candidate.origin = "https://stream.test";
    error = FETCH_REQUEST_VALIDATION_OK;
    if (fetch_request_validate(&candidate, &error)
        || error != FETCH_REQUEST_VALIDATION_CONTEXT) return false;
    candidate.initiator_url = "https://other.test/page";
    if (fetch_request_validate(&candidate, &error)
        || error != FETCH_REQUEST_VALIDATION_CONTEXT) return false;
    candidate.initiator_url = "https://stream.test/page";
    candidate.origin = "https://stream.test/path";
    if (fetch_request_validate(&candidate, &error)
        || error != FETCH_REQUEST_VALIDATION_CONTEXT) return false;
    candidate.origin = "https://stream.test";
    candidate.credentials = FETCH_CREDENTIALS_SAME_ORIGIN;
    candidate.credential_origin = "https://stream.test";
    if (!fetch_request_validate(&candidate, NULL)) return false;
    candidate.cors_cached_response_validated = true;
    error = FETCH_REQUEST_VALIDATION_OK;
    if (fetch_request_validate(&candidate, &error)
        || error != FETCH_REQUEST_VALIDATION_CONTEXT) return false;
    candidate.if_none_match = "\"cached\"";
    if (!fetch_request_validate(&candidate, NULL)) return false;
    candidate.enforce_cors = false;
    error = FETCH_REQUEST_VALIDATION_OK;
    if (fetch_request_validate(&candidate, &error)
        || error != FETCH_REQUEST_VALIDATION_CONTEXT) return false;
    candidate.enforce_cors = true;
    candidate.credential_origin = "https://other.test";
    error = FETCH_REQUEST_VALIDATION_OK;
    if (fetch_request_validate(&candidate, &error)
        || error != FETCH_REQUEST_VALIDATION_CONTEXT) return false;

    /* Keep the requested token at the end of its allocation. This catches
       token scanners which accidentally classify the terminating NUL as an
       HTTP token character and read beyond the string. */
    char terminal_hint_token[] = "Sec-CH-UA-Arch";
    candidate = request();
    candidate.send_client_hints = true;
    candidate.client_hint_tokens = terminal_hint_token;
    candidate.client_hint_origin = "https://stream.test";
    if (!fetch_request_validate(&candidate, NULL)) return false;
    candidate.client_hint_tokens = "Sec-CH-UA-Arch,";
    error = FETCH_REQUEST_VALIDATION_OK;
    if (fetch_request_validate(&candidate, &error)
        || error != FETCH_REQUEST_VALIDATION_CONTEXT) return false;

    candidate = request();
    candidate.extra_headers = "X-One: value\nX-Two: other";
    if (!fetch_request_validate(&candidate, NULL)) return false;
    const char *bad_extra_headers[] = {
        "Bad Name: value",
        "Host: attacker.test",
        "Content-Length: 0",
        "Transfer-Encoding: chunked",
        "Sec-Fetch-User: ?1",
        "Sec-CH-UA-Woof: forged",
        "Proxy-Authorization: Basic forged",
        "Accept-Encoding: identity",
        "Upgrade-Insecure-Requests: 0",
        "Cookie2: bypass=yes",
        "X-HTTP-Method-Override: TRACE",
        "X-One: value\r\nX-Injected: yes",
        "X-One: value\twith-tab",
        "X-One: value\nInjected",
        "X-One: value\n\nX-Two: value",
        "X-One: value\n"
    };
    for (size_t i = 0;
         i < sizeof(bad_extra_headers) / sizeof(bad_extra_headers[0]); i++) {
        candidate.extra_headers = bad_extra_headers[i];
        error = FETCH_REQUEST_VALIDATION_OK;
        if (fetch_request_validate(&candidate, &error)
            || error != FETCH_REQUEST_VALIDATION_EXTRA_HEADERS) return false;
    }
    candidate = request();
    candidate.extra_headers = "Authorization: Bearer fixture";
    if (!fetch_request_validate(&candidate, NULL)) return false;
    candidate.credentials = FETCH_CREDENTIALS_OMIT;
    candidate.extra_headers = "Cookie: bypass=yes";
    error = FETCH_REQUEST_VALIDATION_OK;
    if (fetch_request_validate(&candidate, &error)
        || error != FETCH_REQUEST_VALIDATION_EXTRA_HEADERS) return false;

    TilefinchRequestContext page_context = {
        .target_url = "https://stream.test/document",
        .initiator_url = "https://stream.test/page",
        .top_level_url = "https://stream.test/page",
        .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_IMAGE
    };
    FetchRequest page_transport = {
        .send_low_client_hints = true,
        .upgrade_insecure_requests = true
    };
    FetchPreparedPageRequest prepared;
    if (!fetch_prepare_page_request_context(
            &page_context, page_context.initiator_url, NULL, NULL,
            NULL, NULL, &page_transport, &prepared, NULL)) return false;
    const FetchRequest *authorized = fetch_prepared_page_request(&prepared);
    if (authorized == NULL) return false;
    candidate = *authorized;
    if (!fetch_request_validate(&candidate, NULL)) return false;
    FetchRequest unprepared = candidate;
    unprepared.prepared_page_version = 0;
    if (fetch_request_validate(&unprepared, NULL)) return false;
    candidate.sec_fetch_dest = "script";
    error = FETCH_REQUEST_VALIDATION_OK;
    if (fetch_request_validate(&candidate, &error)
        || error != FETCH_REQUEST_VALIDATION_CONTEXT) return false;
    page_context.mode = TILEFINCH_REQUEST_MODE_CORS;
    if (!fetch_prepare_page_request_context(
            &page_context, page_context.initiator_url, NULL, NULL,
            NULL, NULL, &page_transport, &prepared, NULL)) return false;
    authorized = fetch_prepared_page_request(&prepared);
    if (authorized == NULL) return false;
    candidate = *authorized;
    if (!candidate.enforce_cors
        || !fetch_request_validate(&candidate, NULL)) {
        fprintf(stderr, "transport-gated page CORS request was rejected\n");
        return false;
    }
    candidate.cors_response_check_deferred = true;
    if (fetch_request_validate(&candidate, NULL)) {
        fprintf(stderr, "double-gated page CORS request was accepted\n");
        return false;
    }
    candidate.enforce_cors = false;
    if (!fetch_request_validate(&candidate, NULL)) {
        fprintf(stderr, "deferred page CORS request was rejected\n");
        return false;
    }

    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    if (!replay_begin()) return false;
    FetchResult result = {.budget = &budget};
    candidate = request();
    candidate.credentials = FETCH_CREDENTIALS_OMIT;
    bool rejected_userinfo = !fetch_request_cancelable(
        &budget, "https://user:secret@stream.test/document", &candidate,
        4096, 1000, NULL, NULL, &result);
    bool rejected_non_http = !fetch_request_cancelable(
        &budget, "file:///tmp/document", &candidate, 4096, 1000,
        NULL, NULL, &result);
    page_context.target_url = "https://stream.test/other";
    page_context.mode = TILEFINCH_REQUEST_MODE_NO_CORS;
    if (!fetch_prepare_page_request_context(
            &page_context, page_context.initiator_url, NULL, NULL,
            NULL, NULL, &page_transport, &prepared, NULL)) return false;
    authorized = fetch_prepared_page_request(&prepared);
    if (authorized == NULL) return false;
    candidate = *authorized;
    bool rejected_page_target = !fetch_request_cancelable(
        &budget, "https://stream.test/document", &candidate,
        4096, 1000, NULL, NULL, &result);
    candidate = request();
    candidate.method = "GET\r\nX-Injected: yes";
    bool rejected_method = !fetch_request_cancelable(
        &budget, "https://stream.test/document", &candidate, 4096, 1000,
        NULL, NULL, &result);
    candidate = request();
    candidate.content_type = "text/plain\r\nX-Injected: yes";
    bool rejected_content_type = !fetch_request_cancelable(
        &budget, "https://stream.test/document", &candidate, 4096, 1000,
        NULL, NULL, &result);
    candidate = request();
    bool valid_after_rejections = fetch_request_cancelable(
        &budget, "HTTPS://STREAM.TEST:443/a/../document",
        &candidate, 4096, 1000, NULL, NULL, &result);
    bool sync_ok = rejected_userinfo && rejected_non_http
        && rejected_page_target
        && rejected_method && rejected_content_type
        && valid_after_rejections && result.status_code == 200
        && result.length == 383;
    fetch_result_destroy(&result);
    fetch_trace_end();
    if (!sync_ok || budget.current != 0 || !replay_begin()) return false;

    FetchScheduler *scheduler = fetch_scheduler_create(&budget, 1, 4096);
    if (fetch_scheduler_create(&budget, 1, SIZE_MAX) != NULL) return false;
    candidate = request();
    candidate.credentials = FETCH_CREDENTIALS_OMIT;
    uint64_t bad_url_id = scheduler == NULL ? 1
        : fetch_scheduler_enqueue(
              scheduler, "https://user:secret@stream.test/document",
              &candidate, 4096, 1000);
    candidate = request();
    candidate.method = "POST\nX-Injected: yes";
    uint64_t bad_method_id = scheduler == NULL ? 1
        : fetch_scheduler_enqueue(
              scheduler, "https://stream.test/document", &candidate,
              4096, 1000);
    candidate = request();
    candidate.content_type = "text/plain\rX-Injected: yes";
    uint64_t bad_content_type_id = scheduler == NULL ? 1
        : fetch_scheduler_enqueue(
              scheduler, "https://stream.test/document", &candidate,
              4096, 1000);
    candidate = request();
    uint64_t valid_id = scheduler == NULL ? 0
        : fetch_scheduler_enqueue(
              scheduler,
              "HTTPS://STREAM.TEST:443/a/../document",
              &candidate, 4096, 1000);
    uint64_t oversized_id = scheduler == NULL ? 1
        : fetch_scheduler_enqueue(
              scheduler, "https://stream.test/document", &candidate,
              SIZE_MAX, 1000);
    bool success = false, taken = false;
    memset(&result, 0, sizeof(result));
    result.budget = &budget;
    for (size_t i = 0; valid_id != 0 && i < 4 && !taken; i++) {
        (void) fetch_scheduler_pump(scheduler, 1, 0);
        taken = fetch_scheduler_take(
            scheduler, valid_id, &success, &result);
    }
    bool scheduler_ok = bad_url_id == 0 && bad_method_id == 0
        && bad_content_type_id == 0
        && oversized_id == 0
        && valid_id != 0 && taken && success && result.status_code == 200
        && result.length == 383;
    fetch_result_destroy(&result);
    fetch_scheduler_destroy(scheduler);
    fetch_trace_end();
    return scheduler_ok && budget.current == 0;
}

static bool run_complete(Consumer *consumer, bool irregular)
{
    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    if (!replay_begin()) return false;
    FetchScheduler *scheduler = fetch_scheduler_create(&budget, 1, 4096);
    FetchRequest fetch_request = request();
    FetchStreamOptions stream = {
        .on_headers = headers,
        .on_body = body,
        .on_stall = stall,
        .opaque = consumer,
        .chunk_bytes = irregular ? 0 : 31,
        .irregular_seed = irregular ? UINT64_C(0x123456789abcdef) : 0,
        .irregular_max_chunk_bytes = irregular ? 37 : 0,
        .stall_every_chunks = 3
    };
    uint64_t id = scheduler == NULL ? 0 : fetch_scheduler_enqueue_stream(
        scheduler, "https://stream.test/document", &fetch_request,
        4096, 1000, &stream);
    FetchPumpQuota quota = {
        .maximum_body_callbacks = 1,
        .maximum_body_bytes = 13,
        .maximum_time_us = 100000
    };
    bool success = false, taken = false, bounded = id != 0;
    FetchResult result = {.budget = &budget};
    FetchStreamMetrics stream_metrics = {0};
    for (size_t pump_count = 0; bounded && pump_count < 128; pump_count++) {
        FetchPumpMetrics pump;
        (void) fetch_scheduler_pump_bounded(
            scheduler, 1, 0, &quota, &pump);
        bounded = pump.body_callbacks <= 1 && pump.body_bytes <= 13;
        if (fetch_scheduler_take_stream(
                scheduler, id, &success, &stream_metrics, &result)) {
            taken = true;
            break;
        }
    }
    bool ok = bounded && taken && success && consumer->length == 383
        && result.length == 383 && consumer->headers == 1
        && stream_metrics.bytes_received == 383
        && stream_metrics.maximum_pump_bytes <= 13
        && stream_metrics.pump_calls > 1
        && stream_metrics.quota_yields > 0
        && consumer->stalls == stream_metrics.replay_stalls;
    fetch_result_destroy(&result);
    fetch_scheduler_destroy(scheduler);
    fetch_trace_end();
    return ok && budget.current == 0;
}

static bool test_deterministic_irregular(void)
{
    Consumer left = {0}, right = {0}, regular = {0};
    return run_complete(&left, true) && run_complete(&right, true)
        && run_complete(&regular, false)
        && left.length == right.length && left.length == regular.length
        && memcmp(left.bytes, right.bytes, left.length) == 0
        && memcmp(left.bytes, regular.bytes, left.length) == 0;
}

static bool test_cancel_and_truncate(void)
{
    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    if (!replay_begin()) return false;
    FetchScheduler *scheduler = fetch_scheduler_create(&budget, 1, 4096);
    Consumer consumer = {0};
    FetchRequest fetch_request = request();
    FetchStreamOptions stream = {
        .on_headers = headers, .on_body = body, .opaque = &consumer,
        .chunk_bytes = 7
    };
    uint64_t id = scheduler == NULL ? 0 : fetch_scheduler_enqueue_stream(
        scheduler, "https://stream.test/document", &fetch_request,
        4096, 1000, &stream);
    FetchPumpQuota quota = {1, 7, 100000};
    (void) fetch_scheduler_pump_bounded(scheduler, 1, 0, &quota, NULL);
    bool cancelled = id != 0
        && fetch_scheduler_cancel(scheduler, id, "test cancellation");
    bool success = true;
    FetchResult result = {.budget = &budget};
    FetchStreamMetrics metrics = {0};
    bool taken = fetch_scheduler_take_stream(
        scheduler, id, &success, &metrics, &result);
    bool ok = cancelled && taken && !success && metrics.cancelled
        && result.length == consumer.length && consumer.length <= 7;
    fetch_result_destroy(&result);
    fetch_scheduler_destroy(scheduler);
    fetch_trace_end();
    if (!ok || budget.current != 0 || !replay_begin()) return false;

    scheduler = fetch_scheduler_create(&budget, 1, 4096);
    memset(&consumer, 0, sizeof(consumer));
    stream.opaque = &consumer;
    stream.truncate_after_bytes = 29;
    id = scheduler == NULL ? 0 : fetch_scheduler_enqueue_stream(
        scheduler, "https://stream.test/document", &fetch_request,
        4096, 1000, &stream);
    taken = false;
    for (size_t i = 0; id != 0 && i < 32; i++) {
        (void) fetch_scheduler_pump_bounded(scheduler, 1, 0, &quota, NULL);
        if (fetch_scheduler_take_stream(
                scheduler, id, &success, &metrics, &result)) {
            taken = true;
            break;
        }
    }
    ok = taken && !success && metrics.truncated
        && metrics.bytes_received == 29 && consumer.length == 29;
    fetch_result_destroy(&result);
    fetch_scheduler_destroy(scheduler);
    fetch_trace_end();
    if (!ok || budget.current != 0) return false;

    /*
     * A platform network restart must be able to retire every easy handle
     * before its native stack disappears while leaving ordinary failure
     * results for the page/resource owners to consume.
     */
    scheduler = fetch_scheduler_create(&budget, 2, 8192);
    FetchRequest cancel_request = request();
    uint64_t cancel_ids[2] = {
        scheduler == NULL ? 0 : fetch_scheduler_enqueue(
            scheduler, "https://cancel-all.invalid/one",
            &cancel_request, 4096, 1000),
        scheduler == NULL ? 0 : fetch_scheduler_enqueue(
            scheduler, "https://cancel-all.invalid/two",
            &cancel_request, 4096, 1000)
    };
    size_t pending_before = fetch_scheduler_pending(scheduler);
    size_t cancelled_all = fetch_scheduler_cancel_all(
        scheduler, "platform network restart");
    ok = scheduler != NULL && cancel_ids[0] != 0 && cancel_ids[1] != 0
        && pending_before == 2
        && cancelled_all == 2
        && fetch_scheduler_pending(scheduler) == 0
        && fetch_scheduler_cancel_all(
               scheduler, "duplicate restart") == 0;
    for (size_t i = 0; i < 2; i++) {
        success = true;
        result = (FetchResult) {.budget = &budget};
        ok = ok && fetch_scheduler_take(
            scheduler, cancel_ids[i], &success, &result)
            && !success
            && strcmp(result.error, "platform network restart") == 0;
        fetch_result_destroy(&result);
    }
    fetch_scheduler_destroy(scheduler);
    if (!ok) {
        fprintf(stderr,
                "cancel-all state ids=%llu,%llu pending=%zu cancelled=%zu\n",
                (unsigned long long) cancel_ids[0],
                (unsigned long long) cancel_ids[1],
                pending_before, cancelled_all);
    }
    return ok && budget.current == 0;
}

static bool test_buffered_fetch_respects_pump_quota(void)
{
    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    if (!replay_begin()) return false;
    FetchScheduler *scheduler = fetch_scheduler_create(&budget, 1, 4096);
    FetchRequest fetch_request = request();
    uint64_t id = scheduler == NULL ? 0 : fetch_scheduler_enqueue(
        scheduler, "https://stream.test/document", &fetch_request,
        4096, 1000);
    FetchPumpQuota quota = {
        .maximum_body_callbacks = 1,
        .maximum_body_bytes = 13,
        .maximum_time_us = 100000
    };
    bool success = false, taken = false, bounded = id != 0;
    bool yielded = false;
    bool progress_observed = false;
    bool completion_observed = false;
    size_t previous_received = 0;
    size_t pumps = 0;
    FetchResult result = {.budget = &budget};
    for (; bounded && pumps < 128; pumps++) {
        FetchPumpMetrics pump;
        (void) fetch_scheduler_pump_bounded(
            scheduler, 1, 0, &quota, &pump);
        bounded = pump.body_callbacks <= 1 && pump.body_bytes <= 13;
        yielded = yielded || pump.quota_yielded;
        FetchRequestProgress progress;
        if (fetch_scheduler_request_progress(scheduler, id, &progress)) {
            progress_observed = true;
            bounded = bounded
                && progress.received_body_bytes >= previous_received;
            previous_received = progress.received_body_bytes;
            completion_observed = completion_observed || progress.complete;
        }
        if (fetch_scheduler_take(
                scheduler, id, &success, &result)) {
            taken = true;
            break;
        }
    }
    bool ok = bounded && yielded && progress_observed && completion_observed
        && previous_received == 383 && pumps > 1 && taken && success
        && result.status_code == 200 && result.length == 383
        && result.data != NULL && result.data[result.length] == '\0';
    fetch_result_destroy(&result);
    fetch_scheduler_destroy(scheduler);
    fetch_trace_end();
    return ok && budget.current == 0;
}

static bool test_allocation_failure(void)
{
    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    if (!replay_begin()) return false;
    FetchScheduler *scheduler = fetch_scheduler_create(&budget, 1, 4096);
    Consumer consumer = {0};
    FetchRequest fetch_request = request();
    FetchStreamOptions stream = {
        .on_body = body, .opaque = &consumer, .chunk_bytes = 7
    };
    budget_inject_failure_after(&budget, 0);
    uint64_t id = scheduler == NULL ? 0 : fetch_scheduler_enqueue_stream(
        scheduler, "https://stream.test/document", &fetch_request,
        4096, 1000, &stream);
    budget_clear_failure_injection(&budget);
    bool success = true;
    FetchResult result = {.budget = &budget};
    FetchStreamMetrics metrics = {0};
    if (id != 0) (void) fetch_scheduler_pump(scheduler, 1, 0);
    bool taken = id != 0 && fetch_scheduler_take_stream(
        scheduler, id, &success, &metrics, &result);
    bool ok = id == 0 || (taken && !success);
    fetch_result_destroy(&result);
    fetch_scheduler_destroy(scheduler);
    fetch_trace_end();
    return ok && budget.current == 0;
}

static bool test_lazy_shared_scheduler_domain(void)
{
    Budget budget;
    budget_init(&budget, 4u * 1024u * 1024u);

    /* Keep libcurl's process-wide bounded pool alive so the measurements below
       isolate scheduler/domain metadata rather than first-user transport
       initialization. */
    FetchScheduler *sentinel = fetch_scheduler_create(&budget, 1, 4096);
    if (sentinel == NULL) return false;
    size_t baseline = budget.current;

    char create_error[96] = {0};
    budget_inject_failure_after(&budget, 0);
    FetchScheduler *diagnostic_failure = fetch_scheduler_create_ex(
        &budget, 1, 4096, create_error, sizeof(create_error));
    budget_clear_failure_injection(&budget);
    bool diagnostic_ok = diagnostic_failure == NULL
        && strstr(create_error, "memory") != NULL
        && budget.current == baseline;
    fetch_scheduler_destroy(diagnostic_failure);

    FetchSchedulerDomain *domain = fetch_scheduler_domain_create(&budget, 2);
    FetchScheduler *left = fetch_scheduler_create_in_domain(
        domain, 2, 8192);
    FetchScheduler *right = fetch_scheduler_create_in_domain(
        domain, 2, 8192);
    FetchSchedulerDomainMetrics metrics = {0};
    bool ok = diagnostic_ok && domain != NULL && left != NULL && right != NULL
        && fetch_scheduler_domain_metrics(domain, &metrics)
        && metrics.maximum_active_slots == 2
        && metrics.active_slots == 0 && metrics.peak_active_slots == 0
        && metrics.rejected_enqueues == 0
        && metrics.active_views == 2 && metrics.peak_active_views == 2
        /* Two idle views and their domain must not contain even one former
           10-KiB ScheduledFetch payload. */
        && budget.current >= baseline
        && budget.current - baseline < 4096;

    size_t idle_current = budget.current;
    budget_inject_failure_after(&budget, 0);
    FetchScheduler *failed_view = fetch_scheduler_create_in_domain(
        domain, 1, 4096);
    budget_clear_failure_injection(&budget);
    ok = ok && failed_view == NULL && budget.current == idle_current
        && fetch_scheduler_domain_metrics(domain, &metrics)
        && metrics.active_views == 2 && metrics.peak_active_views == 2;

    FetchRequest fetch_request = request();
    fetch_inject_failure_once(FETCH_INJECT_TIMEOUT);
    uint64_t left_id = left == NULL ? 0 : fetch_scheduler_enqueue(
        left, "https://stream.test/document", &fetch_request, 4096, 1000);
    ok = ok && left_id != 0
        && fetch_scheduler_domain_metrics(domain, &metrics)
        && metrics.active_slots == 1 && metrics.peak_active_slots == 1;

    fetch_inject_failure_once(FETCH_INJECT_TLS);
    uint64_t right_id = right == NULL ? 0 : fetch_scheduler_enqueue(
        right, "https://stream.test/document", &fetch_request, 4096, 1000);
    ok = ok && right_id != 0
        && fetch_scheduler_domain_metrics(domain, &metrics)
        && metrics.active_slots == 2 && metrics.peak_active_slots == 2;

    /* Local space remains in both independent views, but their shared domain
       applies creator-independent backpressure. The rejected request does not
       consume the one-shot fault. */
    fetch_inject_failure_once(FETCH_INJECT_REDIRECT);
    uint64_t rejected = left == NULL ? 1 : fetch_scheduler_enqueue(
        left, "https://stream.test/document", &fetch_request, 4096, 1000);
    fetch_inject_failure_once(FETCH_INJECT_NONE);
    ok = ok && rejected == 0
        && strstr(fetch_scheduler_last_error(left), "slots") != NULL
        && fetch_scheduler_domain_metrics(domain, &metrics)
        && metrics.active_slots == 2 && metrics.rejected_enqueues == 1;

    bool success = true;
    FetchResult result = {.budget = &budget};
    ok = ok && fetch_scheduler_take(left, left_id, &success, &result)
        && !success;
    fetch_result_destroy(&result);
    ok = ok && fetch_scheduler_domain_metrics(domain, &metrics)
        && metrics.active_slots == 1;

    /* Slot reservation and lazy metadata allocation roll back together. */
    budget_inject_failure_after(&budget, 0);
    uint64_t allocation_failure = fetch_scheduler_enqueue(
        left, "https://stream.test/document", &fetch_request, 4096, 1000);
    budget_clear_failure_injection(&budget);
    ok = ok && allocation_failure == 0
        && strstr(fetch_scheduler_last_error(left), "memory") != NULL
        && fetch_scheduler_domain_metrics(domain, &metrics)
        && metrics.active_slots == 1 && metrics.rejected_enqueues == 1;

    fetch_inject_failure_once(FETCH_INJECT_CANCELLED);
    uint64_t discard_id = fetch_scheduler_enqueue(
        left, "https://stream.test/document", &fetch_request, 4096, 1000);
    ok = ok && discard_id != 0
        && fetch_scheduler_discard(left, discard_id)
        && fetch_scheduler_domain_metrics(domain, &metrics)
        && metrics.active_slots == 1;
    result = (FetchResult) {.budget = &budget};
    ok = ok && fetch_scheduler_take(right, right_id, &success, &result)
        && !success;
    fetch_result_destroy(&result);
    ok = ok && fetch_scheduler_domain_metrics(domain, &metrics)
        && metrics.active_slots == 0 && metrics.peak_active_slots == 2;

    fetch_scheduler_destroy(left);
    fetch_scheduler_destroy(right);
    ok = ok && fetch_scheduler_domain_metrics(domain, &metrics)
        && metrics.active_views == 0 && metrics.peak_active_views == 2;
    fetch_scheduler_domain_release(domain);
    ok = ok && budget.current == baseline;

    /* Releasing the creator reference before a view is safe. Destruction of a
       still-occupied view releases both its slot and the final domain ref. */
    domain = fetch_scheduler_domain_create(&budget, 1);
    FetchSchedulerDomain *observer = fetch_scheduler_domain_retain(domain);
    left = fetch_scheduler_create_in_domain(domain, 1, 4096);
    fetch_scheduler_domain_release(domain);
    fetch_inject_failure_once(FETCH_INJECT_TIMEOUT);
    left_id = left == NULL ? 0 : fetch_scheduler_enqueue(
        left, "https://stream.test/document", &fetch_request, 4096, 1000);
    ok = ok && observer != NULL && left != NULL && left_id != 0;
    fetch_scheduler_destroy(left);
    ok = ok && fetch_scheduler_domain_metrics(observer, &metrics)
        && metrics.active_slots == 0 && metrics.active_views == 0;
    fetch_scheduler_domain_release(observer);
    ok = ok && budget.current == baseline;

    fetch_scheduler_destroy(sentinel);
    return ok && budget.current == 0;
}

/*
 * Speculative lanes (the navigation preload scanner) must not spend the byte
 * pool an authoritative fetch is about to need. Reservations are taken in
 * full at enqueue and only released at completion, so on a slow link several
 * requests are outstanding at once; this exercises the headroom predicate the
 * preload launcher gates on, against a pool sized the way
 * navigation_stream_prepare_runtime sizes it (preload lane + authoritative
 * lanes, in units of the largest single file limit).
 */
static bool test_reservation_headroom_gate(void)
{
    enum {
        UNIT = 4096,
        PRELOAD_LANES = 2,
        AUTHORITATIVE_LANES = 4,
        POOL = (PRELOAD_LANES + AUTHORITATIVE_LANES) * UNIT,
        /* What a preload must leave free: the blocking stylesheet checkpoint
           plus the parser-blocking script queued behind it. */
        RESERVE = 2 * UNIT
    };
    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    FetchScheduler *scheduler = fetch_scheduler_create(&budget, 12, POOL);
    if (scheduler == NULL) return false;
    FetchRequest fetch_request = {.method = "GET"};

    /* Never pumped, so each stays in flight and keeps its whole reservation -
       the shape a real network produces and hermetic replay never does. */
    static const char *const holds[] = {
        "https://headroom-one.invalid/a", "https://headroom-two.invalid/b",
        "https://headroom-three.invalid/c", "https://headroom-four.invalid/d",
        "https://headroom-five.invalid/e", "https://headroom-six.invalid/f"
    };
    enum { HOLD_LIMIT = sizeof(holds) / sizeof(holds[0]) };
    uint64_t hold_ids[HOLD_LIMIT] = {0};
    bool ok = fetch_scheduler_reservation_available(scheduler, UNIT, RESERVE)
        /* A request larger than the whole pool is refused outright. */
        && !fetch_scheduler_reservation_available(scheduler, POOL + 1u, 0)
        /* Degenerate arguments never claim room. */
        && !fetch_scheduler_reservation_available(NULL, UNIT, RESERVE)
        && !fetch_scheduler_reservation_available(scheduler, 0, 0);

    size_t launched = 0;
    for (size_t i = 0; ok && i < HOLD_LIMIT; i++) {
        /* This is the loop the preload launcher runs: consult the gate, and
           only reserve when the authoritative headroom survives the reserve.
           More candidates are offered than the pool can take, so the gate -
           not the candidate list - has to be what stops the sweep. */
        if (!fetch_scheduler_reservation_available(
                scheduler, UNIT, RESERVE)) break;
        hold_ids[i] = fetch_scheduler_enqueue(
            scheduler, holds[i], &fetch_request, UNIT, 1000);
        if (hold_ids[i] == 0) ok = false;
        launched++;
    }

    size_t reserved = 0, maximum_reserved = 0;
    fetch_scheduler_reservation_state(
        scheduler, &reserved, &maximum_reserved, NULL, NULL);
    /* Four of the six fit. The fifth would drop the pool below the two units
       the authoritative lanes are owed, so speculation stops there however
       many candidates remain queued. */
    ok = ok && launched == 4
        && reserved == 4u * UNIT && maximum_reserved == POOL
        && !fetch_scheduler_reservation_available(scheduler, UNIT, RESERVE)
        /* The point of the gate: what it held back is still enqueueable by an
           authoritative caller, which asks for no headroom of its own. */
        && fetch_scheduler_reservation_available(scheduler, UNIT, 0)
        && fetch_scheduler_reservation_available(scheduler, RESERVE, 0);

    /* Attempted unconditionally: this is the enqueue the device regression
       refused, so it must be exercised even when an assertion above has
       already failed, to name the production symptom in the diagnostic. */
    uint64_t authoritative = fetch_scheduler_enqueue(
        scheduler, "https://headroom-blocking.invalid/app.css",
        &fetch_request, UNIT, 1000);
    ok = ok && authoritative != 0
        && strstr(fetch_scheduler_last_error(scheduler),
                  "exceeds scheduler byte bound") == NULL;

    /* Completions reopen the speculative lane, so a preload the gate skipped
       is deferred rather than lost: the launcher is re-entered on progress and
       finds room again. */
    for (size_t i = 0; ok && i < 2; i++) {
        FetchResult result = {.budget = &budget};
        bool success = false;
        (void) fetch_scheduler_cancel(
            scheduler, hold_ids[i], "headroom test");
        ok = fetch_scheduler_take(scheduler, hold_ids[i], &success, &result);
        fetch_result_destroy(&result);
        hold_ids[i] = 0;
    }
    ok = ok && fetch_scheduler_reservation_available(scheduler, UNIT, RESERVE);

    if (!ok) {
        fprintf(stderr,
                "reservation headroom gate: launched=%zu reserved=%zu/%zu "
                "authoritative=%llu error=\"%s\"\n",
                launched, reserved, maximum_reserved,
                (unsigned long long) authoritative,
                fetch_scheduler_last_error(scheduler));
    }
    (void) fetch_scheduler_cancel_all(scheduler, "headroom test teardown");
    fetch_scheduler_destroy(scheduler);
    return ok && budget.current == 0;
}

int main(void)
{
    static const struct {
        const char *name;
        bool (*run)(void);
    } cases[] = {
        {"ca-bundle", test_ca_bundle_configuration},
        {"critical-client-hints", test_accepted_critical_client_hints},
        {"request-validation", test_request_validation_before_replay},
        {"handshake-counters", test_handshake_counter_rendering},
        {"absent-handshake", test_replay_reports_absent_handshake_fields},
        {"retained-failure-delay", test_retained_failure_replay_delay},
        {"deterministic-irregular", test_deterministic_irregular},
        {"cancel-truncate", test_cancel_and_truncate},
        {"buffered-pump-quota", test_buffered_fetch_respects_pump_quota},
        {"allocation-failure", test_allocation_failure},
        {"shared-scheduler-domain", test_lazy_shared_scheduler_domain},
        {"reservation-headroom", test_reservation_headroom_gate}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (!cases[i].run()) {
            fprintf(stderr, "fetch stream scheduler test failed: %s\n",
                    cases[i].name);
            return 1;
        }
    }
    puts("fetch stream scheduler tests passed");
    return 0;
}
