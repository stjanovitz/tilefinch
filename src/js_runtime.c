#include "tilefinch/js_runtime.h"
#include "tilefinch/budget_quickjs.h"
#include "tilefinch/fetch.h"
#include "tilefinch/request_context.h"
#include "tilefinch/script_lazy.h"
#include "tilefinch/style.h"
#include "tilefinch/platform.h"
#include "tilefinch/sha256.h"
#include "tilefinch/url.h"
#include "tilefinch/user_agent.h"

#include "js_runtime_internal.h"

#include <stdio.h>
#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <errno.h>
#if defined(__linux__)
#endif

#include <quickjs.h>
#include <stb_image.h>
#include <lexbor/dom/interface.h>
#include <lexbor/dom/interfaces/document.h>
#include <lexbor/dom/interfaces/comment.h>
#include <lexbor/dom/interfaces/document_fragment.h>
#include <lexbor/dom/interfaces/element.h>

#include <lexbor/dom/interfaces/node.h>
#include <lexbor/dom/interfaces/text.h>
#include <lexbor/html/interfaces/template_element.h>
#include <lexbor/html/interfaces/element.h>
#include <lexbor/html/serialize.h>

/* The runtime remains one translation unit: these ordered private seams keep
   QuickJS/DOM state static while separating responsibilities for review. */
#include "js_runtime/bridge_state.inc"
#include "js_runtime/host_primitives.inc"
#include "js_runtime/evaluation.inc"
#include "js_runtime/dynamic_scripts.inc"
#include "js_runtime/document_state.inc"
#include "js_runtime/event_loop.inc"
#include "js_runtime/result_snapshot.inc"
#include "js_runtime/runtime_creation.inc"
#include "js_runtime/document_evaluation.inc"
#include "js_runtime/runtime_loop.inc"
#include "js_runtime/dispatch_state.inc"
#include "js_runtime/destruction_runners.inc"
