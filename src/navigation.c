#include "tilefinch/navigation.h"
#include "tilefinch/file_download.h"
#include "tilefinch/preview_policy.h"
#include "diagnostic_trace.h"
#include "tilefinch_test_faults.h"
#include "tilefinch_compiler.h"
#include "tilefinch/content_blocker.h"
#include "tilefinch/declarative_refresh.h"
#include "tilefinch/fetch.h"
#include "tilefinch/frame_sandbox.h"
#include "tilefinch/image_retarget.h"
#include "tilefinch/linked_video_preview.h"
#include "tilefinch/script_loader.h"
#include "tilefinch/platform.h"
#include "tilefinch/pixel_math.h"
#include "tilefinch/render.h"
#include "tilefinch/request_context.h"
#include "tilefinch/resource_integrity.h"
#include "tilefinch/site_identity.h"
#include "tilefinch/site_adapter.h"
#include "tilefinch/youtube_lite.h"
#include "tilefinch/url.h"
#include "tilefinch/work_vector.h"
#include "tilefinch/work_ledger.h"

#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <lexbor/ns/const.h>

#define budget_malloc(b, s) budget_malloc_category((b), BUDGET_CATEGORY_NAVIGATION, (s))
#define budget_calloc(b, n, s) budget_calloc_category((b), BUDGET_CATEGORY_NAVIGATION, (n), (s))
#define budget_realloc(b, p, s) budget_realloc_category((b), BUDGET_CATEGORY_NAVIGATION, (p), (s))

#define NAVIGATION_DOM_TRAVERSAL_NODE_LIMIT 65536u


/* The frame and page capability traces are host-lab instrumentation: only
   interactive_main.c enables them. A PSP build without tracing or the
   validation log reads them as constant false, so the probe scripts, report
   formatting and stack-source capture they guard are compiled out. */
#if defined(TILEFINCH_NO_TRACE) && !defined(TILEFINCH_PSP_VALIDATION_LOG)
#define NAVIGATION_TRACES_FRAME_CAPABILITIES(session) ((void) (session), false)
#define NAVIGATION_TRACES_PAGE_CAPABILITIES(session) ((void) (session), false)
#define NAVIGATION_CAPABILITY_TRACES_COMPILED 0
#else
#define NAVIGATION_CAPABILITY_TRACES_COMPILED 1
#define NAVIGATION_TRACES_FRAME_CAPABILITIES(session) \
    ((session)->trace_frame_capabilities)
#define NAVIGATION_TRACES_PAGE_CAPABILITIES(session) \
    ((session)->trace_page_capabilities)
#endif

/* Counters only tests, the host lab and the validation log read
   (NavigationRefreshStats beyond its chain guard and the refresh's counted
   latch, installed_app_heap_raises): builds without tracing or that log keep
   the fields but never fill them. */
#if defined(TILEFINCH_NO_TRACE) && !defined(TILEFINCH_PSP_VALIDATION_LOG)
#define NAVIGATION_TEST_COUNTERS 0
#else
#define NAVIGATION_TEST_COUNTERS 1
#endif

/* Navigation remains one private translation unit so candidate/page state is
   never exported.  The ordered implementation seams mirror its lifecycle. */
static void navigation_bind_node_retirement(NavigationSession *session);

/* last_error stays a plain status: the frontends show it. The numbers go to
   the diagnostic record beside it. */
static void navigation_note_layout_failure(
    NavigationSession *session, unsigned phase, bool cancelled,
    size_t refusals)
{
    snprintf(session->last_error, sizeof(session->last_error), "%s",
             NAVIGATION_LAYOUT_FAILED_ERROR);
    session->last_layout_failure = (NavigationLayoutFailure) {
        .phase = phase,
        .cancelled = cancelled,
        .refusals = refusals,
        .current = session->budget->current,
        .limit = session->budget->limit
    };
}

#include "navigation/page_lifecycle.inc"
#include "navigation/frames.inc"
#include "navigation/declarative_refresh.inc"
#include "navigation/configuration.inc"
#include "navigation/document_stream.inc"
#include "navigation/stylesheet_checkpoint.inc"
#include "navigation/stream_runtime.inc"
#include "navigation/parser_script_checkpoint.inc"
/* Candidate adoption is a distinct transactional boundary. */
#include "navigation/commit_transaction.inc"
#include "navigation/progressive_preview.inc"
#include "navigation/commit_html.inc"
#include "navigation/load_state.inc"
#include "navigation/history.inc"
#include "navigation/mutation_classify.inc"
#include "navigation/style_sources.inc"
#include "navigation/layout_reuse.inc"
#include "navigation/relayout.inc"
#include "navigation/deferred_images.inc"
#include "navigation/runtime_advance.inc"
#include "navigation/events.inc"

void navigation_destroy(NavigationSession *session)
{
    if (session == NULL) return;
    page_destroy(session);
    /* Direct NavigationSession owners do not pass through BrowserEngine's
       teardown coordinator. Reap the process-wide JPEG slot here as well so
       its retained body and arena cannot outlive this session's Budget. */
    (void) images_decode_worker_shutdown(session->budget);
    navigation_compiled_stylesheet_cache_discard(session);
    if (session->budget != NULL) {
        for (size_t i = 0; i < session->history_count; i++) {
            entry_destroy(session->budget, &session->history[i]);
        }
        budget_free(session->budget, session->history);
        budget_free(session->budget, session->user_css);
    }
    memset(session, 0, sizeof(*session));
}

#ifndef __PSP__
/* Exercise the transfer/retirement ordering without a site fixture or an
   allocator-dependent OOM threshold. The production teardown is the oracle. */
void navigation_test_retired_stream_teardown(NavigationSession *session);
void navigation_test_retired_stream_teardown(NavigationSession *session)
{
    NavigationDocumentStream stream = {
        .session = session,
        .runtime = session->page.runtime,
        .runtime_transferred = true,
        .scheduler_transferred = true
    };
    navigation_retire_current_page_scripts(session);
    navigation_stream_destroy_staging(&stream);
}
#endif
