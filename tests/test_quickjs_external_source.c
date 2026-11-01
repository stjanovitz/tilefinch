/* External function source in the vendored QuickJS
   (JS_SetExternalSourceHooks): a script compiled with JS_STRIP_SOURCE whose
   text the embedder keeps elsewhere must answer Function.prototype.toString()
   exactly as the unstripped engine does, for every function form, eagerly,
   lazily (with and without preparsing) and after a bytecode round trip;
   keep its line numbers; fall back to the "[native code]" form when the
   embedder no longer has the text (without allocating for the text when
   the embedder says so first, JS_SetExternalSourceQuery()); and release
   each text exactly once. Each case runs with and without the query
   hook. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilefinch/budget.h"
#include "tilefinch/budget_quickjs.h"

#define MIB (1024u * 1024u)

/* ------------------------------------------------------------ embedder */

typedef struct {
    const char *text;   /* the registered text (borrowed) */
    uint32_t length;
    uint32_t id;
    bool dropped;       /* simulates an evicted entry */
    unsigned reads;
    unsigned releases;
    int pinned; /* texts required by uncompiled lazy functions */
} TextStore;

static int store_read(void *opaque, uint32_t id, uint32_t offset,
                      uint32_t length, char *dest)
{
    TextStore *store = opaque;
    store->reads++;
    if (id != store->id || store->dropped || offset > store->length
        || length > store->length - offset) return -1;
    memcpy(dest, store->text + offset, length);
    return 0;
}

/* the query hook (JS_SetExternalSourceQuery()) is installed */
static bool use_query = true;

static int store_query(void *opaque, uint32_t id, uint32_t offset,
                       uint32_t length)
{
    TextStore *store = opaque;
    if (id != store->id || store->dropped || offset > store->length
        || length > store->length - offset) return -1;
    for (uint32_t i = 0; i < length; i++)
        if ((unsigned char) store->text[offset + i] >= 0x80) return 0;
    return 1;
}

static void store_pin(void *opaque, uint32_t id, int required)
{
    TextStore *store = opaque;
    (void) id;
    if (required) store->pinned++;
    else store->pinned--;
}

static void store_release(void *opaque, uint32_t id)
{
    TextStore *store = opaque;
    (void) id;
    store->releases++;
}

/* --------------------------------------------------------------- realms */

typedef struct {
    Budget budget;
    BudgetQuickJSPool *pool;
    JSRuntime *rt;
    JSContext *ctx;
    TextStore store;
} Realm;

static bool realm_open(Realm *realm, uint32_t lazy_threshold, bool preparse)
{
    memset(realm, 0, sizeof(*realm));
    budget_init(&realm->budget, 32u * MIB);
    realm->pool = budget_quickjs_pool_create(&realm->budget);
    if (realm->pool == NULL) return false;
    realm->rt = JS_NewRuntime2(budget_quickjs_pool_allocator(), realm->pool);
    if (realm->rt == NULL) return false;
    JS_SetMemoryLimit(realm->rt, 16u * MIB);
    JS_SetMaxStackSize(realm->rt, 1u * MIB);
    realm->ctx = JS_NewContext(realm->rt);
    if (realm->ctx == NULL) return false;
    JS_SetLazyFunctionThreshold(realm->rt, lazy_threshold);
    JS_SetLazyFunctionPreparse(realm->rt, preparse);
    JS_SetExternalSourceHooks(realm->rt, store_read, store_release,
                              store_pin, &realm->store);
    if (use_query) JS_SetExternalSourceQuery(realm->rt, store_query);
    return true;
}

static bool realm_close(Realm *realm)
{
    if (realm->ctx != NULL) JS_FreeContext(realm->ctx);
    if (realm->rt != NULL) JS_FreeRuntime(realm->rt);
    if (realm->pool == NULL) return false;
    (void) budget_quickjs_pool_trim(realm->pool, 0);
    return budget_quickjs_pool_destroy(realm->pool)
        && realm->budget.current == 0;
}

static char *value_text(JSContext *ctx, JSValue value)
{
    if (JS_IsException(value)) {
        JSValue exception = JS_GetException(ctx);
        const char *text = JS_ToCString(ctx, exception);
        char *copy = NULL;
        if (text != NULL && (copy = malloc(strlen(text) + 5)) != NULL)
            sprintf(copy, "EXC %s", text);
        JS_FreeCString(ctx, text);
        JS_FreeValue(ctx, exception);
        return copy != NULL ? copy : strdup("EXC");
    }
    const char *text = JS_ToCString(ctx, value);
    JS_FreeValue(ctx, value);
    char *copy = strdup(text != NULL ? text : "(null)");
    JS_FreeCString(ctx, text);
    return copy;
}

static char *eval_text(Realm *realm, const char *source)
{
    return value_text(realm->ctx,
        JS_Eval(realm->ctx, source, strlen(source), "probe.js",
                JS_EVAL_TYPE_GLOBAL));
}

/* Compiles `source` (stripped with external text when `external`), then
   runs it. */
static JSValue realm_compile(Realm *realm, const char *source, bool external,
                             uint32_t id)
{
    size_t length = strlen(source);
    if (external) {
        realm->store.text = source;
        realm->store.length = (uint32_t) length;
        realm->store.id = id;
        JS_SetStripInfo(realm->rt, JS_STRIP_SOURCE);
        JS_SetExternalSourceId(realm->rt, id, (uint32_t) length);
    }
    JSValue compiled = JS_Eval(realm->ctx, source, length, "app.js",
                               JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    if (external) {
        JS_SetStripInfo(realm->rt, 0);
        if (JS_SetExternalSourceId(realm->rt, 0, 0) != 0) {
            fprintf(stderr, "external id not consumed\n");
            JS_FreeValue(realm->ctx, compiled);
            return JS_EXCEPTION;
        }
    }
    return compiled;
}

/* --------------------------------------------------------------- script */

/* Every form of author function, a nested function inside a function that
   is compiled lazily, non-ASCII text before and inside functions (offsets
   are in UTF-8 bytes), and one function over 16 KiB long. The probe
   returns the JSON array of their toString() texts plus a stack frame. */
static char *make_script(void)
{
    static const char head[] =
        "// pr\xc3\xa9" "ambule \xe2\x9c\x93 \xf0\x9f\x98\x80\n"
        "function decl(a, b) { return a + b; }\n"
        "var expr = function (x) { return x * 2 };\n"
        "var named = function inner(y) { return inner.name + y };\n"
        "var arrow = (p, q) => p - q;\n"
        "var arrowBlock = x => { return x; };\n"
        "async function af() { await 1; return 2 }\n"
        "var asyncArrow = async (z) => z;\n"
        "function* gen() { yield 1; yield 2 }\n"
        "async function* agen() { yield 1 }\n"
        "class Plain { m() { return 1 } }\n"
        "class Derived extends Plain {\n"
        "  constructor(v) { super(); this.v = v }\n"
        "  static s() { return 's' }\n"
        "  get g() { return this.v }\n"
        "  set g(v) { this.v = v }\n"
        "  async am() { return 1 }\n"
        "  *gm() { yield 1 }\n"
        "  ['com' + 'puted']() { return 2 }\n"
        "  #priv() { return 3 }\n"
        "  callPriv() { return this.#priv() }\n"
        "  static { globalThis.staticRan = 1 }\n"
        "}\n"
        "var obj = { meth(a) { return a }, get acc() { return 1 },\n"
        "  set acc(v) {}, async am2() {}, *g2() {},\n"
        "  ['k' + 1]: function () { return 'k' }, arrowProp: () => 5 };\n"
        "function outer() { function innerDecl(n) { return n + 1 } return innerDecl }\n"
        "function lazyOuter(k) {\n"
        "  /* long enough to be compiled lazily at every threshold below:"
        " .................................................................. */\n"
        "  var made = function lazyInner(q) { return q * 3 + k };\n"
        "  var arrowIn = (r) => { return made(r) };\n"
        "  return [made, arrowIn];\n"
        "}\n"
        "function uncalledLazy(w) {\n"
        "  /* never called before toString ................................."
        " .................................................................. */\n"
        "  return function deepInner() { return w };\n"
        "}\n"
        "function uni() { return 'h\xc3\xa9llo \xe2\x9c\x93 \xf0\x9f\x98\x80' }\n"
        "function thrower() {\n"
        "  null.boom;\n"
        "}\n"
        "function big() {\n"
        "  var t = 0;\n";
    static const char tail[] =
        "  return t;\n"
        "}\n"
        "var evalFn = (0, eval)('(function ev(){ return 1 })');\n"
        "var ctorFn = new Function('a', 'return a');\n"
        "globalThis.probe = function () {\n"
        "  var d = Object.getOwnPropertyDescriptor(Derived.prototype, 'g');\n"
        "  var oa = Object.getOwnPropertyDescriptor(obj, 'acc');\n"
        "  var lz = lazyOuter(1); lz[1](2);\n"
        "  var list = [decl, expr, named, arrow, arrowBlock, af, asyncArrow,\n"
        "    gen, agen, Plain, Plain.prototype.m, Derived, Derived.s, d.get,\n"
        "    d.set, Derived.prototype.am, Derived.prototype.gm,\n"
        "    Derived.prototype.computed, Derived.prototype.callPriv,\n"
        "    obj.meth, oa.get, oa.set, obj.am2, obj.g2, obj.k1, obj.arrowProp,\n"
        "    outer, outer(), lazyOuter, lz[0], lz[1], uncalledLazy,\n"
        "    uncalledLazy(4), uni, thrower, big, evalFn, ctorFn, Math.max,\n"
        "    decl.bind(null), globalThis.odd];\n"
        "  var stack = '';\n"
        "  try { thrower() } catch (e) { stack = e.stack.split('\\n')[0].trim() }\n"
        "  return JSON.stringify([list.map(function (f) {\n"
        "    return Function.prototype.toString.call(f) }), stack,\n"
        "    big(), uni(), new Derived(5).g, staticRan]);\n"
        "};\n";
    size_t big_lines = 900; /* ~20 KiB: spans two 16 KiB blocks */
    size_t malformed = 6000; /* a malformed UTF-8 run longer than a window */
    size_t capacity = sizeof(head) + sizeof(tail) + big_lines * 32u
        + malformed + 256u;
    char *source = malloc(capacity);
    if (source == NULL) return NULL;
    size_t at = (size_t) snprintf(source, capacity, "%s", head);
    for (size_t i = 0; i < big_lines; i++)
        at += (size_t) snprintf(source + at, capacity - at,
                                "  t += %zu * (t %% 7);\n", i);
    at += (size_t) snprintf(source + at, capacity - at, "%s", tail);
    /* not ASCII, and decoded in windows: a lead byte, then a long run of
       continuation bytes, then text */
    at += (size_t) snprintf(source + at, capacity - at,
                            "globalThis.odd = function odd() { /* \xe2");
    memset(source + at, 0x80, malformed);
    at += malformed;
    snprintf(source + at, capacity - at, " \xc3\xa9 */ return 2 };\n");
    return source;
}

/* The probe's result for `source` compiled in a fresh realm. Mode:
   0 keep text, 1 external, 2 external then dropped before the probe,
   3 external, written and read back with the text named,
   4 external, written and read back without a text. */
static char *probe(const char *source, uint32_t lazy_threshold,
                   bool preparse, int mode, unsigned *releases,
                   unsigned *reads)
{
    Realm realm;
    char *text = NULL;
    if (!realm_open(&realm, lazy_threshold, preparse)) return NULL;
    JSValue compiled = realm_compile(&realm, source, mode != 0, 7u);
    if (!JS_IsException(compiled) && (mode == 3 || mode == 4)) {
        size_t length = 0;
        uint8_t *bytes = JS_WriteObject(realm.ctx, &length, compiled,
                                        JS_WRITE_OBJ_BYTECODE);
        JS_FreeValue(realm.ctx, compiled);
        compiled = JS_EXCEPTION;
        if (bytes != NULL) {
            if (mode == 3)
                JS_SetExternalSourceId(realm.rt, 8u, realm.store.length);
            realm.store.id = 8u;
            compiled = JS_ReadObject(realm.ctx, bytes, length,
                                     JS_READ_OBJ_BYTECODE);
            js_free(realm.ctx, bytes);
        }
    }
    if (JS_IsException(compiled)) {
        text = value_text(realm.ctx, compiled);
    } else {
        JSValue result = JS_EvalFunction(realm.ctx, compiled);
        JS_FreeValue(realm.ctx, result);
        if (mode == 2) {
            /* an embedder drops only a text no lazy function needs */
            if (realm.store.pinned != 0) {
                fprintf(stderr, "dropping a required text\n");
                *releases = 99;
            }
            realm.store.dropped = true;
        }
        text = eval_text(&realm, "probe()");
    }
    *reads = realm.store.reads;
    if (mode == 1 && lazy_threshold == 1 && realm.store.pinned != 1) {
        /* uncalledLazy's nested deepInner, among others, is uncompiled */
        fprintf(stderr, "lazy functions did not require the text (%d)\n",
                realm.store.pinned);
        free(text);
        text = NULL;
    }
    if (!realm_close(&realm)) {
        fprintf(stderr, "realm leaked %zu bytes\n", realm.budget.current);
        free(text);
        return NULL;
    }
    if (*releases != 99) *releases = realm.store.releases;
    if (realm.store.pinned != 0) {
        fprintf(stderr, "required count unbalanced: %d\n", realm.store.pinned);
        free(text);
        return NULL;
    }
    return text;
}

static const char native_decl[] = "function decl() {\\n    [native code]\\n}";

static int run_tostring_equality(void)
{
    char *source = make_script();
    if (source == NULL) return 1;
    int failed = 0;
    static const struct { uint32_t threshold; bool preparse; } modes[] = {
        { 0, false }, { 1, false }, { 1, true }, { 64, true }
    };
    unsigned releases = 0, reads = 0;
    char *expected = probe(source, 0, false, 0, &releases, &reads);
    if (expected == NULL || strncmp(expected, "[[", 2) != 0
        || strstr(expected, "app.js:") == NULL) {
        fprintf(stderr, "reference probe failed: %s\n",
                expected ? expected : "(null)");
        free(source);
        free(expected);
        return 1;
    }
    for (size_t m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
        for (int mode = 1; mode <= 3; mode += 2) {
            char *got = probe(source, modes[m].threshold, modes[m].preparse,
                              mode, &releases, &reads);
            if (got == NULL || strcmp(got, expected) != 0) {
                fprintf(stderr,
                        "toString differs (threshold %u preparse %d mode %d)"
                        "\nwanted %.400s\ngot    %.400s\n",
                        modes[m].threshold, modes[m].preparse, mode,
                        expected, got ? got : "(null)");
                failed = 1;
            }
            /* mode 3: the compile's text, then the read's */
            if (releases != (mode == 3 ? 2u : 1u)) {
                fprintf(stderr, "released %u times (threshold %u mode %d)\n",
                        releases, modes[m].threshold, mode);
                failed = 1;
            }
            if (reads < 30) {
                fprintf(stderr, "only %u external reads\n", reads);
                failed = 1;
            }
            free(got);
        }
        /* dropped text: native form, same behaviour otherwise (an
           eager compile: a lazily compiled function requires its text) */
        for (int mode = 2; mode <= 4; mode += 2) {
            if (modes[m].threshold != 0 && mode == 2) continue;
            char *got = probe(source, modes[m].threshold, modes[m].preparse,
                              mode, &releases, &reads);
            if (modes[m].threshold != 0) {
                /* uncompiled lazy functions need the text: no read-back */
                if (got == NULL || strstr(got, "without its text") == NULL
                    || releases != 1) {
                    fprintf(stderr, "lazy read-back without text: %.200s\n",
                            got ? got : "(null)");
                    failed = 1;
                }
                free(got);
                continue;
            }
            if (got == NULL || strstr(got, native_decl) == NULL
                || strstr(got, "app.js:") == NULL
                || strstr(got, "\"h\xc3\xa9llo") == NULL) {
                fprintf(stderr, "fallback (threshold %u mode %d): %.300s\n",
                        modes[m].threshold, mode, got ? got : "(null)");
                failed = 1;
            }
            /* mode 4 named no text for the read: only the compile's */
            if (releases != 1) {
                fprintf(stderr, "fallback released %u times\n", releases);
                failed = 1;
            }
            free(got);
        }
    }
    free(expected);
    free(source);
    return failed;
}

/* A compile that is not stripped, fails, or defines no function still
   releases the text exactly once; an id nothing consumed stays pending. */
static int run_release_accounting(void)
{
    int failed = 0;
    static const char *const sources[] = {
        "1 + 1",             /* no function */
        "function f( {",     /* syntax error */
        "var g = function () { return 1 }",
    };
    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
        Realm realm;
        if (!realm_open(&realm, 1u, true)) return 1;
        JSValue compiled = realm_compile(&realm, sources[i], true, 3u);
        if (!JS_IsException(compiled)) {
            JSValue result = JS_EvalFunction(realm.ctx, compiled);
            JS_FreeValue(realm.ctx, result);
        } else {
            JS_FreeValue(realm.ctx, JS_GetException(realm.ctx));
        }
        if (!realm_close(&realm) || realm.store.releases != 1) {
            fprintf(stderr, "release accounting case %zu: %u\n", i,
                    realm.store.releases);
            failed = 1;
        }
    }
    /* length mismatch: the text is not used */
    Realm realm;
    if (!realm_open(&realm, 0u, false)) return 1;
    JS_SetStripInfo(realm.rt, JS_STRIP_SOURCE);
    JS_SetExternalSourceId(realm.rt, 5u, 3u);
    realm.store.id = 5u;
    static const char src[] = "globalThis.h = function h() { return 1 }";
    JSValue v = JS_Eval(realm.ctx, src, strlen(src), "a.js",
                        JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(realm.ctx, v);
    JS_SetStripInfo(realm.rt, 0);
    char *text = eval_text(&realm, "String(h)");
    if (text == NULL || strcmp(text, "function h() {\n    [native code]\n}") != 0) {
        fprintf(stderr, "length mismatch: %s\n", text ? text : "(null)");
        failed = 1;
    }
    free(text);
    if (JS_SetExternalSourceId(realm.rt, 9u, 1u) != 0) failed = 1;
    if (JS_SetExternalSourceId(realm.rt, 0, 0) != 9u) failed = 1;
    if (!realm_close(&realm) || realm.store.releases != 1) {
        fprintf(stderr, "mismatch released %u\n", realm.store.releases);
        failed = 1;
    }
    return failed;
}

/* A compile repeated without preparsing (js_parse_preparse_retry) keeps
   the external text. */
static int run_preparse_retry(void)
{
    /* A body the preparser cannot vouch for forces a full parse. */
    static const char source[] =
        "function a() { var re = /[/]/; return 'a' + re.source }\n"
        "function b(x) { return x < 1 ? 'b' : `t${x}` }\n"
        "globalThis.r = String(a) + '|' + String(b) + '|' + a() + b(2);";
    Realm realm;
    if (!realm_open(&realm, 1u, true)) return 1;
    JSValue compiled = realm_compile(&realm, source, true, 11u);
    JSValue result = JS_IsException(compiled) ? compiled
        : JS_EvalFunction(realm.ctx, compiled);
    char *text = value_text(realm.ctx, result);
    int failed = text == NULL || strncmp(text, "EXC", 3) == 0;
    if (failed) fprintf(stderr, "retry compile: %s\n", text ? text : "");
    free(text);
    text = eval_text(&realm, "r");
    if (text == NULL || strcmp(text,
            "function a() { var re = /[/]/; return 'a' + re.source }|"
            "function b(x) { return x < 1 ? 'b' : `t${x}` }|a[/]t2") != 0) {
        fprintf(stderr, "retry: %s\n", text ? text : "(null)");
        failed = 1;
    }
    free(text);
    if (!realm_close(&realm) || realm.store.releases != 1) {
        fprintf(stderr, "retry released %u\n", realm.store.releases);
        failed = 1;
    }
    return failed;
}

/* toString() of large functions with little heap left. The external text
   is read straight into the result (ASCII), or a window at a time
   (other), after the collection checkpoint, so under any heap limit at
   which the unstripped engine (holding the text in the heap) returns it,
   the external text must come back too, exactly (compared by length and
   hash); with less room the refusal is a clean out-of-memory error and
   the realm keeps working. */
static uint32_t fnv_units(const uint16_t *units, size_t count)
{
    uint32_t x = 2166136261u;
    for (size_t i = 0; i < count; i++) { x ^= units[i]; x *= 16777619u; }
    return x;
}

/* "ok", "oom" or "bad": measure(big) under a heap limit of `limit`, or
   (0) `room` bytes above the heap in use, in a realm compiled with
   (external) or without stripped text; *used gets the heap in use. */
static const char *tight_tostring_mode(const char *source, bool external,
                                       size_t limit, size_t room,
                                       const char *expected, size_t *used,
                                       bool drop)
{
    Realm realm;
    const char *outcome = "bad";
    if (!realm_open(&realm, 0u, false)) return "bad";
    JS_SetMemoryLimit(realm.rt, 64u * MIB);
    JSValue compiled = realm_compile(&realm, source, external, 21u);
    JSValue result = JS_IsException(compiled) ? compiled
        : JS_EvalFunction(realm.ctx, compiled);
    JS_FreeValue(realm.ctx, result);
    JSValue global = JS_GetGlobalObject(realm.ctx);
    JSValue measure = JS_GetPropertyStr(realm.ctx, global, "measure");
    JSValue big = JS_GetPropertyStr(realm.ctx, global, "big");
    JS_FreeValue(realm.ctx, global);
    if (drop) realm.store.dropped = true; /* evicted: nothing requires it */
    JS_RunGC(realm.rt);
    *used = budget_quickjs_pool_js_malloc_current(realm.pool);
    JS_SetMemoryLimit(realm.rt, limit != 0 ? limit : *used + room);
    char *text = value_text(realm.ctx,
        JS_Call(realm.ctx, measure, JS_UNDEFINED, 1, &big));
    if (text != NULL && strcmp(text, expected) == 0) outcome = "ok";
    else if (text != NULL && strstr(text, "out of memory") != NULL)
        outcome = "oom";
    free(text);
    /* the realm works again with room */
    JS_SetMemoryLimit(realm.rt, 64u * MIB);
    text = value_text(realm.ctx,
        JS_Call(realm.ctx, measure, JS_UNDEFINED, 1, &big));
    if (text == NULL || strcmp(text, expected) != 0) outcome = "bad";
    free(text);
    JS_FreeValue(realm.ctx, big);
    JS_FreeValue(realm.ctx, measure);
    if (!realm_close(&realm)) outcome = "bad";
    if (external && realm.store.releases != 1) outcome = "bad";
    return outcome;
}

static const char *tight_tostring(const char *source, bool external,
                                  size_t limit, size_t room,
                                  const char *expected, size_t *used)
{
    return tight_tostring_mode(source, external, limit, room, expected, used,
                               false);
}

static int run_tight_memory_tostring(void)
{
    enum { BODY = 300 * 1024 };
    int failed = 0;
    for (int unicode = 0; unicode < 2; unicode++) {
        size_t capacity = BODY + 4096;
        char *source = malloc(capacity);
        uint16_t *units = malloc(capacity * sizeof(uint16_t));
        if (source == NULL || units == NULL) return 1;
        size_t at = (size_t) snprintf(source, capacity,
            "globalThis.measure = function (f) { var s ="
            " Function.prototype.toString.call(f), x = 2166136261;"
            " for (var i = 0; i < s.length; i++) { x ^= s.charCodeAt(i);"
            " x = Math.imul(x, 16777619) >>> 0 } return s.length + ':' + x };\n");
        size_t start = at;
        at += (size_t) snprintf(source + at, capacity - at,
                                "function big() { return '");
        while (at < start + BODY) {
            if (unicode) {
                memcpy(source + at, "\xc3\xa9\xe2\x9c\x93", 5);
                at += 5;
            } else {
                source[at++] = 'x';
            }
        }
        at += (size_t) snprintf(source + at, capacity - at, "' }\n");
        size_t end = at - 1;
        size_t count = 0;
        for (size_t i = start; i < end; ) {
            unsigned char c = (unsigned char) source[i];
            if (c < 0x80) { units[count++] = c; i++; }
            else if (c < 0xe0) {
                units[count++] = (uint16_t) (((c & 0x1f) << 6)
                                             | (source[i + 1] & 0x3f));
                i += 2;
            } else {
                units[count++] = (uint16_t) (((c & 0x0f) << 12)
                    | ((source[i + 1] & 0x3f) << 6) | (source[i + 2] & 0x3f));
                i += 3;
            }
        }
        char expected[64];
        snprintf(expected, sizeof(expected), "%zu:%u", count,
                 fnv_units(units, count));
        size_t result_bytes = unicode ? count * 2 : count;
        bool any_ok = false;
        /* from a quarter of the result to three results of room */
        for (size_t room = result_bytes / 4; room <= result_bytes * 3;
             room += result_bytes / 4) {
            /* the same heap limit for both: the unstripped realm already
               holds the text, the other reads it in */
            size_t keep_used = 0, ext_used = 0;
            const char *keep = tight_tostring(source, false, 0, room,
                                              expected, &keep_used);
            const char *ext = tight_tostring(source, true, keep_used + room,
                                             0, expected, &ext_used);
            if (strcmp(ext, "bad") == 0 || strcmp(keep, "bad") == 0
                || (strcmp(keep, "ok") == 0 && strcmp(ext, "ok") != 0)) {
                fprintf(stderr, "tight toString (%s) room %zu: keep %s, "
                        "external %s\n", unicode ? "unicode" : "ascii", room,
                        keep, ext);
                failed = 1;
            }
            if (strcmp(ext, "ok") == 0) any_ok = true;
        }
        /* too little room for the result: a clean refusal */
        size_t used = 0;
        const char *tight = tight_tostring(source, true, 0, result_bytes / 4,
                                           expected, &used);
        if (strcmp(tight, "oom") != 0) {
            fprintf(stderr, "tight toString (%s): %s, wanted a refusal\n",
                    unicode ? "unicode" : "ascii", tight);
            failed = 1;
        }
        /* the text was dropped: with the query hook the "[native code]"
           form needs no room for the text (without it, the text's size is
           allocated first and refused) */
        static const char native_big[] =
            "function big() {\n    [native code]\n}";
        uint16_t native_units[sizeof(native_big)];
        for (size_t i = 0; i < sizeof(native_big) - 1; i++)
            native_units[i] = (unsigned char) native_big[i];
        char native_expected[64];
        snprintf(native_expected, sizeof(native_expected), "%zu:%u",
                 sizeof(native_big) - 1,
                 fnv_units(native_units, sizeof(native_big) - 1));
        const char *dropped = tight_tostring_mode(source, true, 0, 16u * 1024u,
                                                  native_expected, &used,
                                                  true);
        if (strcmp(dropped, use_query ? "ok" : "oom") != 0) {
            fprintf(stderr, "dropped toString (%s, query %d): %s\n",
                    unicode ? "unicode" : "ascii", use_query, dropped);
            failed = 1;
        }
        if (!any_ok) {
            fprintf(stderr, "tight toString (%s): never succeeded\n",
                    unicode ? "unicode" : "ascii");
            failed = 1;
        }
        free(source);
        free(units);
    }
    return failed;
}

/* A short non-ASCII function whose realm has no free room but holds
   collectable garbage: with the query hook (the browser's path) toString()
   collects before it allocates, as the ASCII path does, and asks only for
   what the span needs, not a 4 KiB window. */
static int run_short_unicode_after_garbage(void)
{
    static const char source[] =
        "function u(a) { return a + '\xc3\xa9\xe2\x9c\x93' }\n";
    static const char expected[] =
        "function u(a) { return a + '\xc3\xa9\xe2\x9c\x93' }";
    Realm realm;
    if (!realm_open(&realm, 0u, false)) return 1;
    JSValue compiled = realm_compile(&realm, source, true, 31u);
    JSValue result = JS_IsException(compiled) ? compiled
        : JS_EvalFunction(realm.ctx, compiled);
    int failed = JS_IsException(result);
    JS_FreeValue(realm.ctx, result);
    JSValue global = JS_GetGlobalObject(realm.ctx);
    JSValue u = JS_GetPropertyStr(realm.ctx, global, "u");
    JS_FreeValue(realm.ctx, global);
    JS_RunGC(realm.rt);
    /* cycles only a collection frees */
    static const char garbage[] =
        "(function () { for (var i = 0; i < 400; i++) {"
        " var a = {x: [i, i, i, i]}, b = {a: a}; a.b = b } })()";
    JS_FreeValue(realm.ctx, JS_Eval(realm.ctx, garbage, sizeof(garbage) - 1,
                                    "garbage.js", JS_EVAL_TYPE_GLOBAL));
    size_t used = budget_quickjs_pool_js_malloc_current(realm.pool);
    /* no room but what collecting the garbage returns */
    JS_SetMemoryLimit(realm.rt, used);
    JSValue to_string = JS_GetPropertyStr(realm.ctx, u, "toString");
    char *text = value_text(realm.ctx,
        JS_Call(realm.ctx, to_string, u, 0, NULL));
    JS_FreeValue(realm.ctx, to_string);
    if (text == NULL || strcmp(text, expected) != 0) {
        fprintf(stderr, "short unicode toString after garbage (query %d): "
                "%s\n", use_query, text != NULL ? text : "(null)");
        failed = 1;
    }
    free(text);
    JS_SetMemoryLimit(realm.rt, 64u * MIB);
    JS_FreeValue(realm.ctx, u);
    if (!realm_close(&realm)) {
        fprintf(stderr, "short unicode toString: realm leaked\n");
        failed = 1;
    }
    return failed;
}

int main(void)
{
    int failed = 0;
    for (int query = 1; query >= 0; query--) {
        use_query = query != 0;
        failed |= run_tight_memory_tostring();
        failed |= run_tostring_equality();
        failed |= run_release_accounting();
        failed |= run_preparse_retry();
        if (use_query) failed |= run_short_unicode_after_garbage();
    }
    if (failed) {
        fprintf(stderr, "tilefinch-quickjs-external-source-tests: FAILED\n");
        return 1;
    }
    printf("tilefinch-quickjs-external-source-tests: ok\n");
    return 0;
}
