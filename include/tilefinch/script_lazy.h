#ifndef TILEFINCH_SCRIPT_LAZY_H
#define TILEFINCH_SCRIPT_LAZY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"

/* A conservative description of a Webpack module-factory table.  Planning
   never changes or owns source: callers must keep the original bytes alive
   until the plan is destroyed or handed to the runtime. */
typedef enum {
    SCRIPT_LAZY_FACTORY_ARROW = 0,
    SCRIPT_LAZY_FACTORY_FUNCTION
} ScriptLazyFactoryKind;

typedef struct {
    size_t key_offset;
    size_t key_length;
    size_t source_offset;
    size_t source_length;
    uint8_t arity;
    ScriptLazyFactoryKind kind;
} ScriptLazyFactory;

typedef struct ScriptLazyWebpackPlan {
    Budget *budget;
    ScriptLazyFactory *factories;
    size_t factory_count;
    size_t factory_capacity;
    size_t factory_source_bytes;
    size_t largest_factory_bytes;
    bool strict_mode;
} ScriptLazyWebpackPlan;

typedef struct {
    size_t source_offset;
    size_t source_length;
} ScriptResourceLoaderStatement;

typedef struct ScriptResourceLoaderPlan {
    Budget *budget;
    ScriptResourceLoaderStatement *statements;
    size_t statement_count;
    size_t statement_capacity;
    size_t statement_source_bytes;
    size_t largest_statement_bytes;
} ScriptResourceLoaderPlan;

/* Recognizes only the canonical Webpack chunk-registration envelope and a
   module object made entirely of simple function/arrow factories.  Planning
   first applies a bounded prefix signature check and an 8 MiB source ceiling;
   its shared lexer allowance is linear in accepted source length.  Ambiguous
   JavaScript, malformed delimiters, unsupported parameters, trailing code,
   or a larger input reject the whole plan for ordinary eager evaluation. */
bool script_lazy_webpack_plan_create(Budget *budget, const char *source,
                                     size_t source_length,
                                     ScriptLazyWebpackPlan *plan);

/* Replaces each planned factory with a small native-trampoline wrapper.  The
   untouched registration envelope and optional Webpack runtime callback are
   copied byte-for-byte.  The returned JavaScript-category allocation is
   NUL-terminated and owned by the caller. */
bool script_lazy_webpack_render(Budget *budget, const char *source,
                                size_t source_length,
                                const ScriptLazyWebpackPlan *plan,
                                uint32_t bundle_id, char **output,
                                size_t *output_length);

void script_lazy_webpack_plan_destroy(ScriptLazyWebpackPlan *plan);

/* A common module loader emits large classic-script responses as an ordered
   sequence of independent `mw.loader.impl(...)` registrations.
   Recognize only that complete envelope, using the same bounded JavaScript
   lexer as the Webpack planner.  Callers may then compile one registration
   at a time without changing their authored order or accepting an arbitrary
   oversized script. */
bool script_resource_loader_plan_create(
    Budget *budget, const char *source, size_t source_length,
    ScriptResourceLoaderPlan *plan);
void script_resource_loader_plan_destroy(ScriptResourceLoaderPlan *plan);

#endif
