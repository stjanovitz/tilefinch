/* Null transport for hermetic PSP qualification builds.

   Implements the bounded tilefinch/fetch.h contract with every network
   operation reporting failure, so the engine links and fixture-driven
   flows (which never fetch) run without libcurl or a socket stack.

   NOTE for the emulator/hardware milestones: src/fetch.c mixes the curl
   transport with pure protocol logic (fetch_request_validate,
   fetch_compute_referrer, fetch_resolve_url, fetch_response_header_value,
   fetch_cors_response_allows, fetch_accepted_critical_client_hints).
   Before navigation flows run on PSP, that pure logic should move to a
   transport-independent TU shared by every backend; the null stubs below
   for those six functions are placeholders until then. */

#include "tilefinch/fetch.h"

#include "tilefinch/session.h"

#include <stdio.h>
#include <string.h>

void fetch_response_security_metadata_reset(
    FetchResponseSecurityMetadata *metadata)
{
    if (metadata == NULL) return;
    memset(metadata, 0, sizeof(*metadata));
    metadata->version = FETCH_RESPONSE_SECURITY_METADATA_VERSION;
}

bool fetch_response_security_metadata_collect(
    FetchResponseSecurityMetadata *metadata, const char *name,
    size_t name_length, const char *value, size_t value_length,
    bool snapshot_stored)
{
    (void) metadata; (void) name; (void) name_length;
    (void) value; (void) value_length; (void) snapshot_stored;
    return false;
}

bool fetch_response_security_metadata_from_snapshot(
    FetchResponseSecurityMetadata *metadata, const char *headers,
    size_t headers_length, bool snapshot_truncated)
{
    (void) headers; (void) headers_length; (void) snapshot_truncated;
    fetch_response_security_metadata_reset(metadata);
    return false;
}

bool fetch_prepare_page_request(
    const FetchPageSecurityContext *security_context,
    const FetchPageRequestDescriptor *descriptor,
    FetchPreparedPageRequest *prepared, FetchRequestValidationError *error)
{
    (void) security_context; (void) descriptor; (void) prepared;
    if (error != NULL) *error = FETCH_REQUEST_VALIDATION_CONTEXT;
    return false;
}

bool fetch_prepare_page_request_context(
    const TilefinchRequestContext *context,
    const char *referrer_source_url, const char *referrer_policy,
    const BrowserSession *session,
    const struct TilefinchContentSecurityPolicy *content_security_policy,
    struct ContentBlocker *content_blocker, const FetchRequest *transport,
    FetchPreparedPageRequest *prepared, FetchRequestValidationError *error)
{
    (void) context; (void) referrer_source_url; (void) referrer_policy;
    (void) session; (void) content_security_policy; (void) content_blocker;
    (void) transport; (void) prepared;
    if (error != NULL) *error = FETCH_REQUEST_VALIDATION_CONTEXT;
    return false;
}

const FetchRequest *fetch_prepared_page_request(
    const FetchPreparedPageRequest *prepared)
{
    (void) prepared;
    return NULL;
}

const TilefinchRequestContext *fetch_prepared_page_request_context(
    const FetchPreparedPageRequest *prepared)
{
    (void) prepared;
    return NULL;
}

bool fetch_request_validate(const FetchRequest *request, FetchRequestValidationError *error)
{
    (void) request;
    (void) error;
    return false;
}

bool fetch_accepted_critical_client_hints(const char *accept_ch, const char *critical_ch, char *output, size_t output_size)
{
    (void) accept_ch;
    (void) critical_ch;
    (void) output;
    (void) output_size;
    return false;
}

/* fetch_inject_failure_once lives in fetch_fault.c and is shared by
   every transport backend. */

bool fetch_url(Budget *budget, const char *url, size_t maximum_bytes, long timeout_ms, FetchResult *result)
{
    (void) budget;
    (void) url;
    (void) maximum_bytes;
    (void) timeout_ms;
    (void) result;
    return false;
}

bool fetch_url_cancelable(Budget *budget, const char *url, size_t maximum_bytes, long timeout_ms, FetchCancelCallback cancel, void *cancel_opaque, FetchResult *result)
{
    (void) budget;
    (void) url;
    (void) maximum_bytes;
    (void) timeout_ms;
    (void) cancel;
    (void) cancel_opaque;
    (void) result;
    return false;
}

bool fetch_request_cancelable(Budget *budget, const char *url, const FetchRequest *request, size_t maximum_bytes, long timeout_ms, FetchCancelCallback cancel, void *cancel_opaque, FetchResult *result)
{
    (void) budget;
    (void) url;
    (void) request;
    (void) maximum_bytes;
    (void) timeout_ms;
    (void) cancel;
    (void) cancel_opaque;
    (void) result;
    return false;
}

bool fetch_request_single_hop(Budget *budget, const char *url, const FetchRequest *request, size_t maximum_bytes, long timeout_ms, FetchResult *result)
{
    (void) budget;
    (void) url;
    (void) request;
    (void) maximum_bytes;
    (void) timeout_ms;
    (void) result;
    return false;
}

bool fetch_request_stream_cancelable(Budget *budget, const char *url, const FetchRequest *request, size_t maximum_bytes, long timeout_ms, FetchCancelCallback cancel, void *cancel_opaque, const FetchStreamOptions *stream, FetchStreamMetrics *metrics, FetchResult *result)
{
    (void) budget;
    (void) url;
    (void) request;
    (void) maximum_bytes;
    (void) timeout_ms;
    (void) cancel;
    (void) cancel_opaque;
    (void) stream;
    (void) metrics;
    (void) result;
    return false;
}

FetchScheduler * fetch_scheduler_create(Budget *budget, size_t maximum_concurrent, size_t maximum_reserved_bytes)
{
    (void) budget;
    (void) maximum_concurrent;
    (void) maximum_reserved_bytes;
    return NULL;
}

FetchScheduler * fetch_scheduler_create_ex(
    Budget *budget, size_t maximum_concurrent, size_t maximum_reserved_bytes,
    char *error, size_t error_size)
{
    if (error != NULL && error_size != 0)
        snprintf(error, error_size, "%s", "network unavailable");
    return fetch_scheduler_create(
        budget, maximum_concurrent, maximum_reserved_bytes);
}

FetchSchedulerDomain * fetch_scheduler_domain_create(Budget *budget, size_t maximum_active_slots)
{
    (void) budget;
    (void) maximum_active_slots;
    return NULL;
}

FetchSchedulerDomain * fetch_scheduler_domain_retain(FetchSchedulerDomain *domain)
{
    (void) domain;
    return NULL;
}

void fetch_scheduler_domain_release(FetchSchedulerDomain *domain)
{
    (void) domain;
}

bool fetch_scheduler_domain_metrics(const FetchSchedulerDomain *domain, FetchSchedulerDomainMetrics *metrics)
{
    (void) domain;
    (void) metrics;
    return false;
}

FetchScheduler * fetch_scheduler_create_in_domain(FetchSchedulerDomain *domain, size_t maximum_concurrent, size_t maximum_reserved_bytes)
{
    (void) domain;
    (void) maximum_concurrent;
    (void) maximum_reserved_bytes;
    return NULL;
}

uint64_t fetch_scheduler_enqueue(FetchScheduler *scheduler, const char *url, const FetchRequest *request, size_t maximum_bytes, long timeout_ms)
{
    (void) scheduler;
    (void) url;
    (void) request;
    (void) maximum_bytes;
    (void) timeout_ms;
    return 0;
}

uint64_t fetch_scheduler_enqueue_stream(FetchScheduler *scheduler, const char *url, const FetchRequest *request, size_t maximum_bytes, long timeout_ms, const FetchStreamOptions *stream)
{
    (void) scheduler;
    (void) url;
    (void) request;
    (void) maximum_bytes;
    (void) timeout_ms;
    (void) stream;
    return 0;
}

bool fetch_scheduler_enqueue_would_block(
    const FetchScheduler *scheduler, size_t maximum_bytes)
{
    (void) scheduler;
    (void) maximum_bytes;
    return false;
}

const char *fetch_scheduler_last_error(const FetchScheduler *scheduler)
{
    (void) scheduler;
    return "network unavailable";
}

size_t fetch_scheduler_pump(FetchScheduler *scheduler, size_t maximum_completions, unsigned maximum_wait_ms)
{
    (void) scheduler;
    (void) maximum_completions;
    (void) maximum_wait_ms;
    return 0;
}

size_t fetch_scheduler_pump_bounded(FetchScheduler *scheduler, size_t maximum_completions, unsigned maximum_wait_ms, const FetchPumpQuota *quota, FetchPumpMetrics *metrics)
{
    (void) scheduler;
    (void) maximum_completions;
    (void) maximum_wait_ms;
    (void) quota;
    (void) metrics;
    return 0;
}

bool fetch_scheduler_take(FetchScheduler *scheduler, uint64_t request_id, bool *success, FetchResult *result)
{
    (void) scheduler;
    (void) request_id;
    (void) success;
    (void) result;
    return false;
}

bool fetch_scheduler_take_stream(FetchScheduler *scheduler, uint64_t request_id, bool *success, FetchStreamMetrics *metrics, FetchResult *result)
{
    (void) scheduler;
    (void) request_id;
    (void) success;
    (void) metrics;
    (void) result;
    return false;
}

bool fetch_scheduler_stream_metrics(const FetchScheduler *scheduler, uint64_t request_id, FetchStreamMetrics *metrics)
{
    (void) scheduler;
    (void) request_id;
    (void) metrics;
    return false;
}

bool fetch_scheduler_request_progress(const FetchScheduler *scheduler, uint64_t request_id, FetchRequestProgress *progress)
{
    (void) scheduler;
    (void) request_id;
    (void) progress;
    return false;
}

bool fetch_scheduler_request(FetchScheduler *scheduler, const char *url, const FetchRequest *request, size_t maximum_bytes, long timeout_ms, FetchResult *result)
{
    (void) scheduler;
    (void) url;
    (void) request;
    (void) maximum_bytes;
    (void) timeout_ms;
    (void) result;
    return false;
}

bool fetch_scheduler_cancel(FetchScheduler *scheduler, uint64_t request_id, const char *reason)
{
    (void) scheduler;
    (void) request_id;
    (void) reason;
    return false;
}

bool fetch_scheduler_discard(FetchScheduler *scheduler, uint64_t request_id)
{
    (void) scheduler;
    (void) request_id;
    return false;
}

size_t fetch_scheduler_pending(const FetchScheduler *scheduler)
{
    (void) scheduler;
    return 0;
}

void fetch_scheduler_reservation_state(const FetchScheduler *scheduler,
                                       size_t *reserved_bytes,
                                       size_t *maximum_reserved_bytes,
                                       size_t *domain_active_slots,
                                       size_t *domain_maximum_slots)
{
    (void) scheduler;
    if (reserved_bytes != NULL) *reserved_bytes = 0;
    if (maximum_reserved_bytes != NULL) *maximum_reserved_bytes = 0;
    if (domain_active_slots != NULL) *domain_active_slots = 0;
    if (domain_maximum_slots != NULL) *domain_maximum_slots = 0;
}

bool fetch_scheduler_reservation_available(const FetchScheduler *scheduler,
                                           size_t maximum_bytes,
                                           size_t reserve_bytes)
{
    (void) scheduler;
    (void) maximum_bytes;
    (void) reserve_bytes;
    return false;
}

void fetch_scheduler_debug_dump(const FetchScheduler *scheduler,
                                const char *label)
{
    (void) scheduler;
    (void) label;
}

bool fetch_scheduler_uses_virtual_replay(const FetchScheduler *scheduler)
{
    (void) scheduler;
    return false;
}

void fetch_scheduler_destroy(FetchScheduler *scheduler)
{
    (void) scheduler;
}

bool fetch_trace_capture_begin(const char *directory, char *error, size_t error_size)
{
    (void) directory;
    (void) error;
    (void) error_size;
    return false;
}

bool fetch_trace_capture_arm_top_level(const char *directory, size_t request_ordinal, char *error, size_t error_size)
{
    (void) directory;
    (void) request_ordinal;
    (void) error;
    (void) error_size;
    return false;
}

bool fetch_trace_replay_begin(const char *directory, char *error, size_t error_size)
{
    (void) directory;
    (void) error;
    (void) error_size;
    return false;
}

bool fetch_trace_replay_begin_response_keyed(const char *directory, char *error, size_t error_size)
{
    (void) directory;
    (void) error;
    (void) error_size;
    return false;
}

bool fetch_trace_replay_active(void)
{
    return false;
}

bool fetch_trace_replay_stats(FetchTraceReplayStats *stats)
{
    (void) stats;
    return false;
}

bool fetch_trace_replay_record_was_claimed(size_t sequence)
{
    (void) sequence;
    return false;
}

bool fetch_trace_replay_seed_session(struct BrowserSession *session, char *error, size_t error_size)
{
    (void) session;
    (void) error;
    (void) error_size;
    return false;
}

void fetch_trace_end(void)
{

}

bool fetch_trace_clock_origin_ms(uint64_t *origin_ms)
{
    (void) origin_ms;
    return false;
}

FetchResult * fetch_result_create(Budget *budget)
{
    (void) budget;
    return NULL;
}

bool fetch_result_share_body(FetchResult *result)
{
    (void) result;
    return false;
}

void fetch_result_destroy(FetchResult *result)
{
    (void) result;
}

void fetch_result_free(FetchResult *result)
{
    (void) result;
}

bool fetch_resolve_url(const char *base_url, const char *reference, char *output, size_t output_size)
{
    (void) base_url;
    (void) reference;
    (void) output;
    (void) output_size;
    return false;
}

bool fetch_response_header_value(const FetchResult *result, const char *name, char *output, size_t output_size)
{
    (void) result;
    (void) name;
    (void) output;
    (void) output_size;
    return false;
}

bool fetch_response_referrer_policy(const FetchResult *result, bool *header_present, char policy[FETCH_REFERRER_POLICY_LIMIT])
{
    (void) result;
    (void) header_present;
    (void) policy;
    return false;
}

bool fetch_cors_response_allows(const FetchResult *result, const char *request_origin, bool credentials_included)
{
    (void) result;
    (void) request_origin;
    (void) credentials_included;
    return false;
}

bool fetch_compute_referrer(const char *document_url, const char *target_url, const char *policy, char *output, size_t output_size)
{
    (void) document_url;
    (void) target_url;
    (void) policy;
    (void) output;
    (void) output_size;
    return false;
}
