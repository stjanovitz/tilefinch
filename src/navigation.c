#include "tilefinch/navigation.h"
#include "tilefinch/content_blocker.h"
#include "tilefinch/fetch.h"
#include "tilefinch/frame_sandbox.h"
#include "tilefinch/script_loader.h"
#include "tilefinch/platform.h"
#include "tilefinch/render.h"
#include "tilefinch/request_context.h"
#include "tilefinch/resource_integrity.h"
#include "tilefinch/url.h"

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


/* Navigation remains one private translation unit so candidate/page state is
   never exported.  The ordered implementation seams mirror its lifecycle. */
#include "navigation/page_lifecycle.inc"
#include "navigation/configuration.inc"
#include "navigation/document_stream.inc"
#include "navigation/load_state.inc"
#include "navigation/history_runtime.inc"
