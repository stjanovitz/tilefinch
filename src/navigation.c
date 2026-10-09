#include "tilefinch/navigation.h"
#include "tilefinch/preview_policy.h"
#include "diagnostic_trace.h"
#include "tilefinch_test_faults.h"
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
#define NAVIGATION_SCRIPT_SNAPSHOT_LIMIT SCRIPT_DOM_HANDLE_SLOT_CAPACITY


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
#include "navigation/page_lifecycle.inc"
#include "navigation/declarative_refresh.inc"
#include "navigation/configuration.inc"
#include "navigation/document_stream.inc"
#include "navigation/load_state.inc"
#include "navigation/history_runtime.inc"
