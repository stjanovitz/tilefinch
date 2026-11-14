/* Source positions from the vendored QuickJS line tables (pc2line).

   The engine stores each function's bytecode-to-source table in a compact
   encoding of its own (see PC2LINE_SHORT_FIRST in quickjs.c) instead of
   upstream's. The positions it reports must not change: every Error.stack
   frame and every thrown error's position stays the same line and column.

   The test generates scripts in three layouts (one long minified line,
   indented lines, and random line breaks with long runs of spaces and
   multi-line literals) that record the stack of many errors: explicit
   captures, TypeErrors and ReferenceErrors raised inside expressions split
   over lines, throws from nested and class code, loops, arrow callbacks,
   and SyntaxErrors from eval. Each script runs four ways: compiled and run
   at once, through a bytecode write and read, with every function lazy
   (compiled at first call), and lazy through a bytecode write and read.
   All four must print exactly the golden file, which was written by the
   same program built against upstream's encoding.

   The table must also stay compact: the corpus' tables, compiled eagerly,
   take at most 85% of upstream's bytes for the same code (about 79%;
   page scripts, mostly minified, take about 73%).

   --print writes the corpus output instead of comparing it (how the golden
   file was made). */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs.h"

/* The corpus' line-table bytes with upstream's encoding (eager compile). */
#define UPSTREAM_PC2LINE_BYTES 26355

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    bool failed;
} Text;

static void text_put(Text *text, const char *format, ...)
{
    if (text->failed) return;
    for (;;) {
        va_list args;
        va_start(args, format);
        size_t room = text->capacity - text->length;
        int written = vsnprintf(text->data == NULL ? NULL
                                                   : text->data + text->length,
                                room, format, args);
        va_end(args);
        if (written < 0) {
            text->failed = true;
            return;
        }
        if ((size_t) written < room) {
            text->length += (size_t) written;
            return;
        }
        size_t capacity = text->capacity == 0 ? 4096u : text->capacity * 2u;
        while (capacity - text->length <= (size_t) written) capacity *= 2u;
        char *grown = realloc(text->data, capacity);
        if (grown == NULL) {
            text->failed = true;
            return;
        }
        text->data = grown;
        text->capacity = capacity;
    }
}

/* ------------------------------------------------------------ generator */

enum { LAYOUT_MINIFIED, LAYOUT_INDENTED, LAYOUT_RANDOM, LAYOUT_COUNT };

typedef struct {
    uint32_t state;
    int layout;
    int depth;
    Text *out;
} Generator;

static uint32_t next_random(Generator *g)
{
    g->state = g->state * 1103515245u + 12345u;
    return (g->state >> 8) & 0xffffffu;
}

static uint32_t below(Generator *g, uint32_t limit)
{
    return next_random(g) % limit;
}

/* Whitespace between two tokens: none in the minified layout, a line break
   and indentation in the indented one, and in the random one sometimes a
   line break (CRLF too), several, or a long run of spaces. */
static void gap(Generator *g)
{
    if (g->layout == LAYOUT_MINIFIED) {
        if (below(g, 4) == 0) text_put(g->out, " ");
        return;
    }
    if (g->layout == LAYOUT_INDENTED) {
        text_put(g->out, "\n%*s", 2 * g->depth, "");
        return;
    }
    switch (below(g, 10)) {
    case 0: case 1: case 2:
        text_put(g->out, "\n%*s", (int) below(g, 12), "");
        break;
    case 3:
        text_put(g->out, "\r\n");
        break;
    case 4:
        text_put(g->out, "%*s", 100 + (int) below(g, 300), "");
        break;
    case 5:
        text_put(g->out, "%.*s", 2 + (int) below(g, 30),
                 "\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n");
        break;
    default:
        if (below(g, 2) == 0) text_put(g->out, " ");
        break;
    }
}

static void statement(Generator *g, int function, int functions);

static void capture(Generator *g)
{
    text_put(g->out, "r.push(p(new Error(");
    gap(g);
    text_put(g->out, ").stack));");
}

static void statement(Generator *g, int function, int functions)
{
    gap(g);
    switch (below(g, 16)) {
    case 0:
    case 1:
        capture(g);
        break;
    case 2:
        text_put(g->out, "try{x=null.a");
        gap(g);
        text_put(g->out, ".b}catch(e){r.push(p(e.stack))}");
        break;
    case 3:
        text_put(g->out, "try{u%u(", below(g, 1000));
        gap(g);
        text_put(g->out, ")}catch(e){r.push(p(e.stack))}");
        break;
    case 4:
        text_put(g->out, "try{(0,o.f");
        gap(g);
        text_put(g->out, ")(x)}catch(e){r.push(p(e.stack))}");
        break;
    case 5: {
        /* Arithmetic only: a run of bytecode with one position per
           statement, so pc gaps grow. */
        uint32_t repeat = 1 + below(g, 24);
        for (uint32_t i = 0; i < repeat; i++) {
            text_put(g->out, "x=(x*31+%u)%%1000003;", below(g, 100000));
            if (below(g, 5) == 0) gap(g);
        }
        break;
    }
    case 6:
        if (g->depth >= 3) {
            capture(g);
            break;
        }
        text_put(g->out, "for(let k=0;k<2;");
        gap(g);
        text_put(g->out, "k++){");
        g->depth++;
        statement(g, function, functions);
        statement(g, function, functions);
        g->depth--;
        gap(g);
        text_put(g->out, "}");
        break;
    case 7:
        text_put(g->out, "if(d<2)F[%u](d+1);",
                 (unsigned) ((function + 1 + (int) below(g, 5)) % functions));
        break;
    case 8:
        text_put(g->out, "[1].forEach(v=>{");
        gap(g);
        capture(g);
        text_put(g->out, "});");
        break;
    case 9:
        text_put(g->out, "x+=`a\n\nb${x}\nc`.length;");
        gap(g);
        capture(g);
        break;
    case 10: {
        /* Long literals push columns past one byte of difference. */
        uint32_t length = 20 + below(g, 900);
        text_put(g->out, "x+=\"");
        for (uint32_t i = 0; i < length; i++)
            text_put(g->out, "%c", 'a' + (int) (i % 26u));
        text_put(g->out, "\xc3\xa9\xe6\x97\xa5\".length;");
        capture(g);
        break;
    }
    case 11:
        text_put(g->out, "/* note%.*s */", (int) below(g, 6), "\n\n\n\n\n\n");
        capture(g);
        break;
    case 12:
        text_put(g->out, "try{(function q%d(){", function);
        gap(g);
        text_put(g->out, "throw new Error(\"t\")})()}catch(e){r.push(p(e.stack))}");
        break;
    case 13:
        text_put(g->out, "try{new (class{constructor(){");
        gap(g);
        text_put(g->out, "null.z}})}catch(e){r.push(p(e.stack))}");
        break;
    case 14:
        text_put(g->out, "try{eval(\"1+\\n\\n  +*\")}catch(e){r.push(p(e.stack))}");
        break;
    default:
        text_put(g->out, "x=x+o.n");
        gap(g);
        text_put(g->out, "*%u;", below(g, 100));
        capture(g);
        break;
    }
}

static void generate(Text *out, int layout, uint32_t seed)
{
    Generator g = { seed * 2654435761u + 1u, layout, 0, out };
    const int functions = 16;
    text_put(out, "var r=[],x=1,o={n:2},F=[];");
    /* One line per stack: frames joined by '|', each "name line:col". */
    text_put(out, "function p(s){return s.split(\"\\n\").map(function(l){"
                  "return l.trim().replace(/^at /,\"\").replace("
                  "/\\(lt-[0-9]-[0-9]\\.js:([0-9:]+)\\)$/,\"$1\")})"
                  ".filter(function(l){return l}).join(\"|\")}");
    for (int f = 0; f < functions; f++) {
        gap(&g);
        text_put(out, "F[%d]=function f%d(d){", f, f);
        g.depth = 1;
        int statements = 4 + (int) below(&g, 10);
        for (int s = 0; s < statements; s++) statement(&g, f, functions);
        g.depth = 0;
        gap(&g);
        text_put(out, "};");
    }
    gap(&g);
    text_put(out, "for(var i=0;i<F.length;i++)F[i](0);r.join(\"\\n\")");
}

/* ------------------------------------------------------------ runs */

enum { RUN_EVAL, RUN_BYTECODE, RUN_LAZY, RUN_LAZY_BYTECODE, RUN_COUNT };

static const char *const run_names[RUN_COUNT] = {
    "eval", "bytecode", "lazy", "lazy-bytecode"
};

/* Runs one script and appends its result string; false on an engine
   failure (an exception escaping the script, a failed write or read). */
static bool run_script(int run, const char *name, const Text *source,
                       Text *out, int64_t *pc2line_bytes)
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
    JSValue compiled = JS_Eval(ctx, source->data, source->length, name,
                               JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(compiled)) goto done;
    if (pc2line_bytes != NULL) {
        JSMemoryUsage usage;
        JS_ComputeMemoryUsage(rt, &usage);
        *pc2line_bytes += usage.js_func_pc2line_size;
    }
    if (run == RUN_BYTECODE || run == RUN_LAZY_BYTECODE) {
        size_t length = 0;
        uint8_t *bytes = JS_WriteObject(ctx, &length, compiled,
                                        JS_WRITE_OBJ_BYTECODE);
        JS_FreeValue(ctx, compiled);
        compiled = JS_UNDEFINED;
        if (bytes == NULL) goto done;
        compiled = JS_ReadObject(ctx, bytes, length, JS_READ_OBJ_BYTECODE);
        js_free(ctx, bytes);
        if (JS_IsException(compiled)) goto done;
    }
    result = JS_EvalFunction(ctx, compiled);
    compiled = JS_UNDEFINED;
    if (JS_IsException(result)) goto done;
    const char *string = JS_ToCString(ctx, result);
    if (string == NULL) goto done;
    text_put(out, "== %s\n%s\n", name, string);
    JS_FreeCString(ctx, string);
    ok = !out->failed;
done:
    if (!ok && JS_HasException(ctx)) {
        JSValue error = JS_GetException(ctx);
        const char *message = JS_ToCString(ctx, error);
        fprintf(stderr, "%s (%s): %s\n", name, run_names[run],
                message != NULL ? message : "?");
        JS_FreeCString(ctx, message);
        JS_FreeValue(ctx, error);
    }
    JS_FreeValue(ctx, compiled);
    JS_FreeValue(ctx, result);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return ok;
}

static bool run_corpus(int run, Text *out, int64_t *pc2line_bytes)
{
    for (int layout = 0; layout < LAYOUT_COUNT; layout++) {
        for (uint32_t seed = 1; seed <= 2; seed++) {
            Text source = {0};
            generate(&source, layout, seed);
            char name[32];
            snprintf(name, sizeof(name), "lt-%d-%u.js", layout, seed);
            bool ok = !source.failed
                && run_script(run, name, &source, out, pc2line_bytes);
            free(source.data);
            if (!ok) return false;
        }
    }
    return true;
}

static char *read_file(const char *path, size_t *length)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return NULL;
    char *data = NULL;
    if (fseek(file, 0, SEEK_END) == 0) {
        long size = ftell(file);
        if (size >= 0 && fseek(file, 0, SEEK_SET) == 0) {
            data = malloc((size_t) size + 1u);
            if (data != NULL
                && fread(data, 1, (size_t) size, file) == (size_t) size) {
                data[size] = '\0';
                *length = (size_t) size;
            } else {
                free(data);
                data = NULL;
            }
        }
    }
    fclose(file);
    return data;
}

/* Reports the first differing line of a run against the golden text. */
static void report_difference(const char *run, const char *expected,
                              const char *actual)
{
    size_t line = 1;
    const char *e = expected, *a = actual;
    while (*e != '\0' && *e == *a) {
        if (*e == '\n') line++;
        e++;
        a++;
    }
    const char *e_line = e, *a_line = a;
    while (e_line > expected && e_line[-1] != '\n') e_line--;
    while (a_line > actual && a_line[-1] != '\n') a_line--;
    fprintf(stderr, "%s: differs from the golden file at line %zu\n"
                    "  expected: %.*s\n  actual:   %.*s\n",
            run, line, (int) strcspn(e_line, "\n"), e_line,
            (int) strcspn(a_line, "\n"), a_line);
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--print") == 0) {
        Text out = {0};
        int64_t bytes = 0;
        if (!run_corpus(RUN_EVAL, &out, &bytes)) return 1;
        fwrite(out.data, 1, out.length, stdout);
        fprintf(stderr, "pc2line bytes %lld\n", (long long) bytes);
        free(out.data);
        return 0;
    }
    if (argc != 2) {
        fprintf(stderr, "usage: %s GOLDEN | --print\n", argv[0]);
        return 2;
    }
    size_t golden_length = 0;
    char *golden = read_file(argv[1], &golden_length);
    if (golden == NULL) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }
    int failures = 0;
    for (int run = 0; run < RUN_COUNT; run++) {
        Text out = {0};
        int64_t bytes = 0;
        if (!run_corpus(run, &out, run == RUN_EVAL ? &bytes : NULL)) {
            fprintf(stderr, "%s: the corpus did not run\n", run_names[run]);
            failures++;
        } else if (out.length != golden_length
                   || memcmp(out.data, golden, golden_length) != 0) {
            report_difference(run_names[run], golden, out.data);
            failures++;
        }
        if (run == RUN_EVAL
            && (bytes <= 0 || bytes * 20 > (int64_t) UPSTREAM_PC2LINE_BYTES * 17)) {
            fprintf(stderr, "line tables take %lld bytes; upstream's take "
                            "%d, and at most 85%% of that is allowed\n",
                    (long long) bytes, UPSTREAM_PC2LINE_BYTES);
            failures++;
        }
        free(out.data);
    }
    free(golden);
    if (failures == 0) printf("quickjs line table tests passed\n");
    return failures == 0 ? 0 : 1;
}
