#ifndef TILEFINCH_SCRIPT_LOADER_H
#define TILEFINCH_SCRIPT_LOADER_H

#include <stdbool.h>
#include <stddef.h>

#include "tilefinch/navigation.h"

typedef struct {
    size_t discovered;
    size_t attempted;
    size_t loaded;
    size_t failed;
    size_t skipped_cross_origin;
    size_t skipped_module;
    size_t skipped_quota;
    size_t skipped_pressure;
    size_t pressure_collections;
    size_t pressure_reclaimed_bytes;
    size_t pressure_capped_requests;
    size_t bytes;
    size_t cache_hits;
    size_t parser_blocking;
    size_t deferred;
    size_t asynchronous;
    size_t modules;
    size_t module_map_hits;
    size_t lazy_webpack_candidates;
    size_t lazy_webpack_applied;
    size_t lazy_webpack_fallbacks;
    size_t lazy_webpack_factories;
    size_t lazy_webpack_source_bytes;
    size_t inline_data_fast_paths;
    size_t inline_data_fast_path_bytes;
} ExternalScriptMetrics;

typedef struct {
    long parser_executed[256];
    size_t parser_executed_count;
    ExternalScriptMetrics early;
} StreamingScriptState;

/* Parser-blocking scripts observe the author stylesheet state that precedes
   them. Deferred, asynchronous, module, and inert script elements do not
   block parsing and must not force the blocking resource pipeline to run at
   their closing tag. */
bool document_script_is_parser_blocking(lxb_dom_node_t *element);

bool document_scripts_process_closed(
    ScriptRuntime *runtime, Budget *budget, BrowserSession *session,
    const char *base_url, const char *document_url,
    const char *referrer_policy,
    const TilefinchContentSecurityPolicy *content_security_policy,
    size_t maximum_scripts, size_t maximum_total_bytes,
    size_t maximum_file_bytes, long timeout_ms, FetchScheduler *scheduler,
    lxb_dom_node_t *element, StreamingScriptState *state);
bool document_scripts_finish_streaming(
    PocDocument *document, ScriptRuntime *runtime, Budget *budget,
    BrowserSession *session, const char *base_url,
    const char *document_url,
    const char *referrer_policy, size_t maximum_scripts,
    size_t maximum_total_bytes, size_t maximum_file_bytes, long timeout_ms,
    FetchScheduler *scheduler, StreamingScriptState *state,
    ExternalScriptMetrics *metrics, ScriptResult *result);

/* Legacy whole-document classic-script entry point.  All three limits remain
   enforced for compatibility, but new navigation code should configure the
   realm once and use the document pipeline below. */
bool external_scripts_load(NavigationSession *navigation,
                           const char *document_url,
                           size_t maximum_scripts,
                           size_t maximum_total_bytes,
                           size_t maximum_file_bytes,
                           long timeout_ms,
                           ExternalScriptMetrics *metrics);
/* `maximum_scripts` is the realm-wide executable quota. Parser discovery uses
   the fixed hard bound so repeated module roots can share one module-map
   admission and a bounded set of critical bootstrap roots can be reserved
   without changing source-order execution. Cumulative count and total-byte
   authority lives in the ScriptRuntime realm configured by navigation;
   `maximum_total_bytes` is retained as an ABI-compatible hint and must
   describe that same realm, while `maximum_file_bytes` bounds each root and
   transitive module response. */
bool document_scripts_execute(PocDocument *document,
                              ScriptRuntime *runtime, Budget *budget,
                              BrowserSession *session,
                              const char *base_url,
                              const char *document_url,
                              const char *referrer_policy,
                              size_t maximum_scripts,
                              size_t maximum_total_bytes,
                              size_t maximum_file_bytes,
                              long timeout_ms,
                              FetchScheduler *scheduler,
                              ExternalScriptMetrics *metrics,
                              ScriptResult *result);
bool document_body_scripts_execute(PocDocument *document,
                                   ScriptRuntime *runtime, Budget *budget,
                                   BrowserSession *session,
                                   const char *base_url,
                                   const char *document_url,
                                   const char *referrer_policy,
                                   size_t maximum_scripts,
                                   size_t maximum_total_bytes,
                                   size_t maximum_file_bytes,
                                   long timeout_ms,
                                   FetchScheduler *scheduler,
                                   ExternalScriptMetrics *metrics,
                                   ScriptResult *result);

#endif
