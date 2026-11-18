/* The class-field initializer's source position (third_party/quickjs
   README, `class-field-position`).

   The engine compiles a class's field initializers into one hidden
   function. It starts that function at the token inside the class that
   first needs it, not at the start of the buffer, so building its line
   table no longer walks the line/column cache back to offset 0 and forward
   again once per class with fields.

   Two checks:

   - Positions. A script with a throwing instance field, a static field
     that reads a stack, and a field that calls a function constructing
     another class prints exactly the stacks the engine printed before the
     change (the golden below). It runs four ways: compiled and run at
     once, through a bytecode write and read, with every function lazy, and
     lazy through a bytecode write and read.
   - Scaling. One generated line of 3,000 small classes with one field
     each, 600 bytes apart, compiles in under three times the time of the
     same line without the fields. Before the change the ratio grew with
     the file's size (about 40 times here). */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "quickjs.h"

static const char position_script[] =
    "var pad = 1;\n"
    "\n"
    "  class A {\n"
    "    a = 1;\n"
    "    x = (() => { throw new Error(\"field\"); })();\n"
    "    static s = 2;\n"
    "  }\n"
    "function g(){ return new A(); }\n"
    "   class B { y = g(); static z = (() => { try { null.q } catch (e) "
    "{ return e.stack } })(); }\n"
    "var out = B.z + \"|\";\n"
    "try { new B() } catch (e) { out += e.stack }\n"
    "out;\n";

/* Printed by the engine before the change, for the same script. */
static const char position_golden[] =
    "    at <anonymous> (t.js:9:53)\n"
    "    at <anonymous> (t.js:9:89)\n"
    "    at <eval> (t.js:1:9)\n"
    "|    at <anonymous> (t.js:5:33)\n"
    "    at <anonymous> (t.js:5:46)\n"
    "    at A (t.js:7:3)\n"
    "    at g (t.js:8:27)\n"
    "    at <anonymous> (t.js:9:19)\n"
    "    at B (t.js:9:93)\n"
    "    at <eval> (t.js:11:12)\n";

enum { RUN_EVAL, RUN_BYTECODE, RUN_LAZY, RUN_LAZY_BYTECODE, RUN_COUNT };

static const char *const run_names[RUN_COUNT] = {
    "eval", "bytecode", "lazy", "lazy-bytecode"
};

static void report_exception(JSContext *ctx, const char *what)
{
    JSValue error = JS_GetException(ctx);
    const char *message = JS_ToCString(ctx, error);
    fprintf(stderr, "%s: %s\n", what, message != NULL ? message : "?");
    JS_FreeCString(ctx, message);
    JS_FreeValue(ctx, error);
}

static bool check_positions(int run)
{
    JSRuntime *rt = JS_NewRuntime();
    if (rt == NULL) return false;
    JSContext *ctx = JS_NewContext(rt);
    if (ctx == NULL) {
        JS_FreeRuntime(rt);
        return false;
    }
    if (run == RUN_LAZY || run == RUN_LAZY_BYTECODE)
        JS_SetLazyFunctionThreshold(rt, 1);
    bool ok = false;
    JSValue result = JS_UNDEFINED;
    JSValue compiled = JS_Eval(ctx, position_script,
                               sizeof(position_script) - 1u, "t.js",
                               JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(compiled)) {
        report_exception(ctx, run_names[run]);
        goto done;
    }
    if (run == RUN_BYTECODE || run == RUN_LAZY_BYTECODE) {
        size_t length = 0;
        uint8_t *bytes = JS_WriteObject(ctx, &length, compiled,
                                        JS_WRITE_OBJ_BYTECODE);
        JS_FreeValue(ctx, compiled);
        compiled = JS_UNDEFINED;
        if (bytes == NULL) {
            fprintf(stderr, "%s: bytecode write failed\n", run_names[run]);
            goto done;
        }
        compiled = JS_ReadObject(ctx, bytes, length, JS_READ_OBJ_BYTECODE);
        js_free(ctx, bytes);
        if (JS_IsException(compiled)) {
            report_exception(ctx, run_names[run]);
            goto done;
        }
    }
    result = JS_EvalFunction(ctx, compiled);
    compiled = JS_UNDEFINED;
    if (JS_IsException(result)) {
        report_exception(ctx, run_names[run]);
        goto done;
    }
    const char *text = JS_ToCString(ctx, result);
    if (text == NULL) goto done;
    ok = strcmp(text, position_golden) == 0;
    if (!ok)
        fprintf(stderr, "%s: stacks differ\nexpected:\n%sactual:\n%s\n",
                run_names[run], position_golden, text);
    JS_FreeCString(ctx, text);
done:
    JS_FreeValue(ctx, compiled);
    JS_FreeValue(ctx, result);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return ok;
}

/* ------------------------------------------------------------ scaling */

enum { CLASS_COUNT = 3000, CLASS_SPACING = 600 };

/* One line: class i, then a string literal that pads the unit to
   CLASS_SPACING bytes. */
static char *generate_classes(bool fields, size_t *length)
{
    size_t capacity = (size_t) CLASS_COUNT * CLASS_SPACING + 64u;
    char *text = malloc(capacity);
    if (text == NULL) return NULL;
    size_t used = 0;
    for (int i = 0; i < CLASS_COUNT; i++) {
        int head = fields
            ? snprintf(text + used, capacity - used,
                       "class C%d{f=%d}var v%d=\"", i, i, i)
            : snprintf(text + used, capacity - used,
                       "class C%d{}var v%d=\"", i, i);
        if (head < 0 || (size_t) head + 3u > CLASS_SPACING) {
            free(text);
            return NULL;
        }
        used += (size_t) head;
        size_t pad = CLASS_SPACING - (size_t) head - 2u;
        memset(text + used, 'a' + i % 26, pad);
        used += pad;
        text[used++] = '"';
        text[used++] = ';';
    }
    text[used] = '\0';
    *length = used;
    return text;
}

static double now_seconds(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double) t.tv_sec + (double) t.tv_nsec / 1e9;
}

/* The fastest of three compiles, in seconds; negative on failure. */
static double compile_time(const char *source, size_t length)
{
    double best = -1.0;
    for (int attempt = 0; attempt < 3; attempt++) {
        JSRuntime *rt = JS_NewRuntime();
        if (rt == NULL) return -1.0;
        JSContext *ctx = JS_NewContext(rt);
        if (ctx == NULL) {
            JS_FreeRuntime(rt);
            return -1.0;
        }
        double started = now_seconds();
        JSValue compiled = JS_Eval(ctx, source, length, "classes.js",
                                   JS_EVAL_TYPE_GLOBAL
                                       | JS_EVAL_FLAG_COMPILE_ONLY);
        double elapsed = now_seconds() - started;
        bool failed = JS_IsException(compiled);
        if (failed) report_exception(ctx, "classes.js");
        JS_FreeValue(ctx, compiled);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        if (failed) return -1.0;
        if (best < 0.0 || elapsed < best) best = elapsed;
    }
    return best;
}

static bool check_scaling(void)
{
    size_t with_length = 0, without_length = 0;
    char *with_fields = generate_classes(true, &with_length);
    char *without_fields = generate_classes(false, &without_length);
    bool ok = false;
    if (with_fields == NULL || without_fields == NULL) {
        fprintf(stderr, "scaling: could not generate the scripts\n");
        goto done;
    }
    double with_time = compile_time(with_fields, with_length);
    double without_time = compile_time(without_fields, without_length);
    if (with_time < 0.0 || without_time <= 0.0) {
        fprintf(stderr, "scaling: a script did not compile\n");
        goto done;
    }
    printf("scaling: %d classes, %zu bytes: with fields %.1f ms, "
           "without %.1f ms (ratio %.2f)\n",
           CLASS_COUNT, with_length, with_time * 1e3, without_time * 1e3,
           with_time / without_time);
    ok = with_time < 3.0 * without_time;
    if (!ok)
        fprintf(stderr, "scaling: classes with fields compile %.1f times "
                        "slower than without; at most 3 is allowed\n",
                with_time / without_time);
done:
    free(with_fields);
    free(without_fields);
    return ok;
}

int main(void)
{
    int failures = 0;
    for (int run = 0; run < RUN_COUNT; run++)
        if (!check_positions(run)) failures++;
    if (!check_scaling()) failures++;
    if (failures == 0) printf("quickjs class field position tests passed\n");
    return failures == 0 ? 0 : 1;
}
