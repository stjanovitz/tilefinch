/* Owner-thread diagnostic flags. Set these before launching the process;
 * each translation unit samples them once. Shipping builds retain neither
 * getenv calls nor diagnostic environment-name strings. */
#ifndef TILEFINCH_DIAGNOSTIC_TRACE_H
#define TILEFINCH_DIAGNOSTIC_TRACE_H
#include <stdbool.h>
#ifndef TILEFINCH_NO_TRACE
#include <stdlib.h>
#endif

static inline bool tilefinch_dump_js_memory(void)
{
#ifdef TILEFINCH_NO_TRACE
    return false;
#else
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("TILEFINCH_DUMP_JS_MEMORY") != NULL;
    return enabled != 0;
#endif
}

static inline bool tilefinch_dump_js_pool(void)
{
#ifdef TILEFINCH_NO_TRACE
    return false;
#else
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("TILEFINCH_DUMP_JS_POOL") != NULL;
    return enabled != 0;
#endif
}

static inline bool tilefinch_dump_js_profile(void)
{
#ifdef TILEFINCH_NO_TRACE
    return false;
#else
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("TILEFINCH_DUMP_JS_PROFILE") != NULL;
    return enabled != 0;
#endif
}

static inline bool tilefinch_trace_base64(void)
{
#ifdef TILEFINCH_NO_TRACE
    return false;
#else
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("TILEFINCH_TRACE_BASE64") != NULL;
    return enabled != 0;
#endif
}

static inline bool tilefinch_trace_callback_source(void)
{
#ifdef TILEFINCH_NO_TRACE
    return false;
#else
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("TILEFINCH_TRACE_CALLBACK_SOURCE") != NULL;
    return enabled != 0;
#endif
}

static inline bool tilefinch_trace_dpu(void)
{
#ifdef TILEFINCH_NO_TRACE
    return false;
#else
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("TILEFINCH_TRACE_DPU") != NULL;
    return enabled != 0;
#endif
}

static inline bool tilefinch_trace_eval_source(void)
{
#ifdef TILEFINCH_NO_TRACE
    return false;
#else
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("TILEFINCH_TRACE_EVAL_SOURCE") != NULL;
    return enabled != 0;
#endif
}

static inline bool tilefinch_trace_frame_controls(void)
{
#ifdef TILEFINCH_NO_TRACE
    return false;
#else
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("TILEFINCH_TRACE_FRAME_CONTROLS") != NULL;
    return enabled != 0;
#endif
}

static inline bool tilefinch_trace_frame_messages(void)
{
#ifdef TILEFINCH_NO_TRACE
    return false;
#else
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("TILEFINCH_TRACE_FRAME_MESSAGES") != NULL;
    return enabled != 0;
#endif
}

static inline bool tilefinch_trace_js_interrupts(void)
{
#ifdef TILEFINCH_NO_TRACE
    return false;
#else
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("TILEFINCH_TRACE_JS_INTERRUPTS") != NULL;
    return enabled != 0;
#endif
}

static inline bool tilefinch_trace_js_startup(void)
{
#ifdef TILEFINCH_NO_TRACE
    return false;
#else
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("TILEFINCH_TRACE_JS_STARTUP") != NULL;
    return enabled != 0;
#endif
}

static inline bool tilefinch_trace_lazy_scripts(void)
{
#ifdef TILEFINCH_NO_TRACE
    return false;
#else
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("TILEFINCH_TRACE_LAZY_SCRIPTS") != NULL;
    return enabled != 0;
#endif
}

static inline bool tilefinch_trace_mutation_policy(void)
{
#ifdef TILEFINCH_NO_TRACE
    return false;
#else
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("TILEFINCH_TRACE_MUTATION_POLICY") != NULL;
    return enabled != 0;
#endif
}

static inline bool tilefinch_trace_promise_rejections(void)
{
#ifdef TILEFINCH_NO_TRACE
    return false;
#else
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("TILEFINCH_TRACE_PROMISE_REJECTIONS") != NULL;
    return enabled != 0;
#endif
}

static inline bool tilefinch_trace_runtime_steps(void)
{
#ifdef TILEFINCH_NO_TRACE
    return false;
#else
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("TILEFINCH_TRACE_RUNTIME_STEPS") != NULL;
    return enabled != 0;
#endif
}

static inline bool tilefinch_trace_script_failures(void)
{
#ifdef TILEFINCH_NO_TRACE
    return false;
#else
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("TILEFINCH_TRACE_SCRIPT_FAILURES") != NULL;
    return enabled != 0;
#endif
}

static inline bool tilefinch_trace_scroll(void)
{
#ifdef TILEFINCH_NO_TRACE
    return false;
#else
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("TILEFINCH_TRACE_SCROLL") != NULL;
    return enabled != 0;
#endif
}

static inline bool tilefinch_trace_tasks(void)
{
#ifdef TILEFINCH_NO_TRACE
    return false;
#else
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("TILEFINCH_TRACE_TASKS") != NULL;
    return enabled != 0;
#endif
}

static inline bool tilefinch_trace_worker_source(void)
{
#ifdef TILEFINCH_NO_TRACE
    return false;
#else
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("TILEFINCH_TRACE_WORKER_SOURCE") != NULL;
    return enabled != 0;
#endif
}

#endif
