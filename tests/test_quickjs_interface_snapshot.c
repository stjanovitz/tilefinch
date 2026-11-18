/* The interface snapshot (JS_SnapshotInterfaces, JS_EVAL_FLAG_TRUSTED).

   The browser's script shares a realm with author code that may replace
   any built-in. After the snapshot, browser code keeps reading the
   interfaces as they were: these cases run the same kernels as browser
   code before and after "page" code wraps, pollutes, replaces and deletes
   built-ins, and require that no page function runs inside them and their
   results do not change, while the page still sees its own changes. They
   also cover what the mechanism must not cost or break: no twin before a
   change, none from browser code's own change, a twin freed with its
   object, many twins, and a refused twin failing the page's write
   cleanly. The same holds for the global bindings browser code names. */
#include <quickjs.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "check failed at %s:%d: %s\n",                     \
                __FILE__, __LINE__, #condition);                             \
        return 1;                                                            \
    }                                                                        \
} while (0)

static JSValue js_trusted(JSContext *context, JSValueConst this_value,
                          int argc, JSValueConst *argv)
{
    (void) this_value;
    (void) argc;
    size_t length;
    const char *source = JS_ToCStringLen(context, &length, argv[0]);
    if (source == NULL) return JS_EXCEPTION;
    JSValue value = JS_Eval(context, source, length, "<browser>",
                            JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_TRUSTED);
    JS_FreeCString(context, source);
    return value;
}

/* page(source): another classic script of the page's */
static JSValue js_page(JSContext *context, JSValueConst this_value,
                       int argc, JSValueConst *argv)
{
    (void) this_value;
    (void) argc;
    size_t length;
    const char *source = JS_ToCStringLen(context, &length, argv[0]);
    if (source == NULL) return JS_EXCEPTION;
    JSValue value = JS_Eval(context, source, length, "<page>",
                            JS_EVAL_TYPE_GLOBAL);
    JS_FreeCString(context, source);
    return value;
}

/* install(f): calls f as a module load does, between
   JS_EnterBrowserCode and JS_LeaveBrowserCode. f stands for the module's
   own code, so it is made browser code: a page function between the
   bracket and browser code is not the load's own work. */
static JSValue js_install(JSContext *context, JSValueConst this_value,
                          int argc, JSValueConst *argv)
{
    (void) this_value;
    (void) argc;
    JS_MarkBrowserFunction(context, argv[0]);
    void *previous = JS_EnterBrowserCode(context);
    JSValue value = JS_Call(context, argv[0], JS_UNDEFINED, 0, NULL);
    JS_LeaveBrowserCode(context, previous);
    return value;
}

static JSValue js_snapshot(JSContext *context, JSValueConst this_value,
                           int argc, JSValueConst *argv)
{
    (void) this_value;
    (void) argc;
    (void) argv;
    static const char *const namespaces[] = {"Math", "JSON", "Reflect"};
    JSValue global = JS_GetGlobalObject(context);
    JS_SnapshotInterfaces(context);
    for (size_t i = 0; i < sizeof(namespaces) / sizeof(namespaces[0]); i++) {
        JSValue value = JS_GetPropertyStr(context, global, namespaces[i]);
        JS_SnapshotObject(context, value);
        JS_FreeValue(context, value);
    }
    JS_FreeValue(context, global);
    return JS_UNDEFINED;
}

/* bracketed(object, key): a function with no frame of its own (as the JS
   profiler's trampoline is) that calls object[key]() inside
   JS_EnterBrowserCode, as a host native doing the browser's work does */
static JSValue js_bracketed_call(JSContext *context, JSValueConst this_value,
                                 int argc, JSValueConst *argv, int magic,
                                 JSValue *data)
{
    (void) this_value;
    (void) argc;
    (void) argv;
    (void) magic;
    void *previous = JS_EnterBrowserCode(context);
    JSAtom key = JS_ValueToAtom(context, data[1]);
    JSValue method = key == JS_ATOM_NULL ? JS_EXCEPTION
        : JS_GetProperty(context, data[0], key);
    JSValue value = JS_IsException(method) ? JS_EXCEPTION
        : JS_Call(context, method, data[0], 0, NULL);
    JS_FreeValue(context, method);
    if (key != JS_ATOM_NULL) JS_FreeAtom(context, key);
    JS_LeaveBrowserCode(context, previous);
    return value;
}

static JSValue js_bracketed(JSContext *context, JSValueConst this_value,
                            int argc, JSValueConst *argv)
{
    (void) this_value;
    if (argc < 2) return JS_ThrowTypeError(context, "bracketed(object, key)");
    return JS_NewCFunctionData(context, js_bracketed_call, 0, 0, 2, argv);
}

static JSValue js_counts(JSContext *context, JSValueConst this_value,
                         int argc, JSValueConst *argv, int magic)
{
    (void) this_value;
    (void) argc;
    (void) argv;
    uint32_t members, twins;
    JS_GetSnapshotCounts(context, &members, &twins);
    return JS_NewUint32(context, magic ? members : twins);
}

static JSValue js_collect(JSContext *context, JSValueConst this_value,
                          int argc, JSValueConst *argv)
{
    (void) this_value;
    (void) argc;
    (void) argv;
    JS_RunGC(JS_GetRuntime(context));
    return JS_UNDEFINED;
}

/* A realm with the helpers; `plan` false builds it without the snapshot,
   as interface_snapshot=0 does. */
static JSContext *open_realm_with(JSRuntime **runtime, bool plan)
{
    *runtime = JS_NewRuntime();
    if (*runtime == NULL) return NULL;
    JSContext *context = JS_NewContext(*runtime);
    if (context == NULL) return NULL;
    if (plan) JS_PlanInterfaceSnapshot(context);
    JSValue global = JS_GetGlobalObject(context);
    JS_SetPropertyStr(context, global, "trusted",
                      JS_NewCFunction(context, js_trusted, "trusted", 1));
    JS_SetPropertyStr(context, global, "page",
                      JS_NewCFunction(context, js_page, "page", 1));
    JS_SetPropertyStr(context, global, "install",
                      JS_NewCFunction(context, js_install, "install", 1));
    JS_SetPropertyStr(context, global, "snapshot",
                      JS_NewCFunction(context, js_snapshot, "snapshot", 0));
    JS_SetPropertyStr(context, global, "twins",
                      JS_NewCFunctionMagic(context, js_counts, "twins", 0,
                                           JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(context, global, "members",
                      JS_NewCFunctionMagic(context, js_counts, "members", 0,
                                           JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(context, global, "collect",
                      JS_NewCFunction(context, js_collect, "collect", 0));
    JS_SetPropertyStr(context, global, "bracketed",
                      JS_NewCFunction(context, js_bracketed, "bracketed", 2));
    JS_FreeValue(context, global);
    return context;
}

static JSContext *open_realm(JSRuntime **runtime)
{
    return open_realm_with(runtime, true);
}

/* Evaluates `setup` (the browser's start-up, ending in the snapshot) and
   then the page's script; 0 when its `result` global is "OK". A page
   script exists only after the snapshot, as in the browser. */
static int run_page_after(JSContext *context, const char *name,
                          const char *setup, const char *source)
{
    JSValue value = JS_UNDEFINED;
    if (setup != NULL)
        value = JS_Eval(context, setup, strlen(setup), "<setup>",
                        JS_EVAL_TYPE_GLOBAL);
    if (!JS_IsException(value)) {
        JS_FreeValue(context, value);
        value = JS_Eval(context, source, strlen(source), name,
                        JS_EVAL_TYPE_GLOBAL);
    }
    if (JS_IsException(value)) {
        JSValue error = JS_GetException(context);
        const char *text = JS_ToCString(context, error);
        fprintf(stderr, "%s: threw %s\n", name, text ? text : "?");
        JS_FreeCString(context, text);
        JS_FreeValue(context, error);
        return 1;
    }
    JS_FreeValue(context, value);
    JSValue global = JS_GetGlobalObject(context);
    JSValue result = JS_GetPropertyStr(context, global, "result");
    JS_FreeValue(context, global);
    const char *text = JS_ToCString(context, result);
    int failed = text == NULL || strcmp(text, "OK") != 0;
    if (failed) fprintf(stderr, "%s: %s\n", name, text ? text : "?");
    else if (strncmp(name, "refused", 7) != 0) printf("%s: OK\n", name);
    JS_FreeCString(context, text);
    JS_FreeValue(context, result);
    return failed;
}

static int run_page(JSContext *context, const char *name, const char *source)
{
    return run_page_after(context, name, NULL, source);
}

/* The browser's kernels: everything a page can redirect by changing a
   built-in. */
static const char browser[] =
    "trusted(`globalThis.B = {"
    "push(a, x) { a.push(x); return a.length; },"
    "forof(a) { let s = 0; for (const v of a) s += v; return s; },"
    "spread(a) { return Math.max(...a); },"
    "destructure(a) { const [x, y] = a; return x + y; },"
    "map(k) { const m = new Map(); m.set(k, 1);"
    " return m.get(k) + (m.has(k) ? 1 : 0); },"
    "mapFromIterable() { return new Map([[1, 2]]).size; },"
    "set() { const s = new Set([1, 2]); s.add(3); let n = 0;"
    " for (const v of s) n += v; return n; },"
    "weak(o) { const w = new WeakMap(); w.set(o, 7); return w.get(o); },"
    "call(f, t, x) { return f.call(t, x); },"
    "apply(f, t, a) { return f.apply(t, a); },"
    "bind(f, t) { return f.bind(t)(); },"
    "hasOwn(o, k) { return o.hasOwnProperty(k); },"
    "toStr(o) { return String(o) + o.toString(); },"
    "statics(o) { return Object.keys(o).length + Array.from([1, 2]).length"
    " + (Array.isArray([]) ? 1 : 0); },"
    "strings(s) { return s.slice(1).toUpperCase().indexOf('B'); },"
    "regexp(s) { return /b+/.exec(s)[0] + s.replace(/a/g, 'x')"
    " + s.split(/b/).length; },"
    "then(p) { return p.then(v => v); },"
    "arrFrom(a) { return Array.from(a).map(v => v * 2).filter(v => v > 2)"
    ".join(','); },"
    "setOnPlain() { const o = {}; o.polluted = 1;"
    " return Object.keys(o).length; },"
    "inOp() { return 'polluted' in {}; },"
    "readDefault() { const options = {}; return options.capture; },"
    "forIn() { let n = 0; for (const k in { a: 1 }) n++; return n; },"
    "hole() { const a = []; a.length = 2; return a[0]; },"
    "json(o) { return JSON.stringify(o); },"
    "species(a) { return a.map(x => x).constructor === Array; },"
    "reflect() { return Reflect.ownKeys(Object.prototype).length + ':'"
    " + typeof Object.getOwnPropertyDescriptor(Array.prototype, 'push')"
    ".value + ':' + (Object.getOwnPropertyDescriptor(Array.prototype,"
    " 'push').value === this.savedPush); },"
    "savedPush: Array.prototype.push,"
    "instance(x) { return x instanceof Array; },"
    "extend() { Array.prototype.tfLater = function later() { return 42; };"
    " return [].tfLater(); },"
    "later() { return [].tfLater(); },"
    "};`);"
    "snapshot();"
    "var bad = [], calls = 0;"
    "function want(c, m) { if (!c) bad.push(m); }"
    "var RA = Reflect.apply;"
    "function count(f) { return function () { calls++;"
    " return RA(f, this, arguments); }; }"
    "function runAll(label) { calls = 0;"
    " var r = [B.push([], 1), B.forof([1, 2, 3]), B.spread([1, 5, 2]),"
    " B.destructure([1, 2]), B.map('k'), B.mapFromIterable(), B.set(),"
    " B.weak({}), B.call(function (x) { return x + 1; }, null, 1),"
    " B.apply(function (x) { return x + 1; }, null, [1]),"
    " B.bind(function () { return 3; }, null), B.hasOwn({ a: 1 }, 'a'),"
    " B.toStr({}), B.statics({ a: 1 }), B.strings('abc'), B.regexp('abba'),"
    " B.arrFrom([1, 2, 3]), B.setOnPlain(), B.inOp(), B.readDefault(),"
    " B.forIn(), B.hole(), B.json({ a: [1] }), B.species([1]), B.reflect(),"
    " B.instance([])];"
    " B.then(Promise.resolve(1));"
    " want(calls === 0, label + ': page code ran ' + calls + ' times');"
    " var text = ''; for (var i = 0; i < r.length; i++) text += r[i] + '|';"
    " return text; }"
    "var expected = runAll('none');"
    "want(twins() === 0, 'twins before any change: ' + twins());";

static const char finish[] =
    "var result = bad.length ? bad.join(' ; ') : 'OK';";

static int test_page_changes(void)
{
    JSRuntime *runtime;
    JSContext *context = open_realm(&runtime);
    CHECK(context != NULL);
    char source[16384];
    snprintf(source, sizeof(source), "%s%s%s", browser,
        /* browser code's own change, made as a module load makes it, is
           no page change */
        "var marked = members();"
        "want(install(() => B.extend()) === 42 && twins() === 0,"
        " 'a browser change made a twin');"
        "want(members() > marked, 'a published method is not an interface');"
        /* wrap: every writable method of the core built-ins */
        "var undo = [];"
        "function wrapAll(o) { var names = Object.getOwnPropertyNames(o)"
        ".concat(Object.getOwnPropertySymbols(o));"
        " for (var i = 0; i < names.length; i++) {"
        " var d = Object.getOwnPropertyDescriptor(o, names[i]);"
        " if (!d || typeof d.value !== 'function' || !d.writable"
        " || names[i] === 'constructor') continue;"
        " (function (k, f) { undo.push(function () { o[k] = f; });"
        " o[k] = count(f); })(names[i], d.value); } }"
        "var targets = [Array.prototype,"
        " Object.getPrototypeOf([][Symbol.iterator]()),"
        " Object.getPrototypeOf(new Set()[Symbol.iterator]()),"
        " Map.prototype, Set.prototype, WeakMap.prototype,"
        " Function.prototype, Object.prototype, String.prototype,"
        " RegExp.prototype, Promise.prototype, Object, Array, Reflect, Math,"
        " JSON];"
        "for (var i = 0; i < targets.length; i++) wrapAll(targets[i]);"
        "calls = 0; [].push(1);"
        "want(calls > 0, 'wrap: the page lost its own patch');"
        "want(runAll('wrap') === expected, 'wrap: results changed');"
        "want(twins() === targets.length, 'wrap: twins ' + twins());"
        /* the browser's later method is still there for both */
        "calls = 0; want(B.later() === 42 && calls === 0, 'wrap: later');"
        "for (i = undo.length - 1; i >= 0; i--) undo[i](); undo.length = 0;"
        /* a browser change after the page's reaches both views */
        "install(() => trusted("
        "'Array.prototype.tfAfter = function () { return 7; }'));"
        "want([].tfAfter() === 7, 'the page misses a browser method');"
        "want(trusted('[].tfAfter()') === 7, 'the browser misses its own');"
        /* A member the page changed since stays the page's when the
           browser changes it again (a module replacing its forwarding
           member, or removing a placeholder): the browser's change is
           made in its own view, and a member the page made read-only
           does not refuse it. */
        "Array.prototype.tfAfter = function () { return 'page'; };"
        "install(() => trusted("
        "'Array.prototype.tfAfter = function () { return 8; }'));"
        "want([].tfAfter() === 'page', 'a browser change took the page member');"
        "want(trusted('[].tfAfter()') === 8, 'the browser misses its change');"
        "Object.defineProperty(Array.prototype, 'tfFixed', {"
        " value: function () { return 'page'; }, configurable: true });"
        "install(() => trusted(`Object.defineProperty(Array.prototype,"
        " 'tfFixed', { value: function () { return 9; }, writable: true,"
        " configurable: true })`));"
        "want([].tfFixed() === 'page' && trusted('[].tfFixed()') === 9,"
        " 'a page read-only member');"
        "install(() => trusted('delete Array.prototype.tfAfter;"
        " delete Array.prototype.tfFixed'));"
        "want([].tfAfter() === 'page' && [].tfFixed() === 'page'"
        " && trusted('typeof [].tfAfter + typeof [].tfFixed')"
        " === 'undefinedundefined', 'a browser delete');"
        /* the page gives the member up: the views agree again */
        "delete Array.prototype.tfAfter; delete Array.prototype.tfFixed;"
        "install(() => trusted("
        "'Array.prototype.tfAfter = function () { return 7; }'));"
        "want([].tfAfter() === 7 && trusted('[].tfAfter()') === 7,"
        " 'a browser change after the page gave the member up');"
        /* proto: accessors, values and dense elements on the prototypes */
        "Object.defineProperty(Object.prototype, 'polluted', {"
        " configurable: true, enumerable: true,"
        " get() { calls++; return 9; }, set(v) { calls++; } });"
        "Object.prototype.capture = true; Array.prototype[0] = 'hole';"
        "want(runAll('proto') === expected, 'proto: results changed: '"
        " + runAll('proto') + ' / ' + expected);"
        "want(({}).polluted === 9 && [,][0] === 'hole',"
        " 'proto: the page lost its own change');"
        "delete Object.prototype.polluted; delete Object.prototype.capture;"
        "delete Array.prototype[0];"
        /* replace: statics replaced, a method deleted, a prototype moved */
        "var keys = Object.keys, from = Array.from,"
        " push = Array.prototype.push;"
        "Object.keys = function () { calls++; return []; };"
        "Array.from = function () { calls++; return []; };"
        "delete Array.prototype.push;"
        "Object.setPrototypeOf(Array.prototype,"
        " { get push() { calls++; return function () {}; } });"
        "Object.defineProperty(Array, Symbol.hasInstance,"
        " { configurable: true, value() { calls++; return false; } });"
        "want(runAll('replace') === expected, 'replace: results changed');"
        "delete Array[Symbol.hasInstance];"
        "Object.setPrototypeOf(Array.prototype, Object.prototype);"
        "Array.prototype.push = push; Object.keys = keys; Array.from = from;"
        "want(runAll('restored') === expected, 'restored: results changed');",
        finish);
    int failed = run_page(context, "page-changes", source);
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    return failed;
}

/* A twin belongs to its object, and many of them stay findable. */
static int test_twin_lifetime(void)
{
    JSRuntime *runtime;
    JSContext *context = open_realm(&runtime);
    CHECK(context != NULL);
    static const char source[] =
        "var bad = [];"
        "function want(c, m) { if (!c) bad.push(m); }"
        "trusted(`for (let i = 0; i < 60; i++)"
        " globalThis['I' + i] = function () {};"
        " for (let i = 0; i < 60; i++)"
        " globalThis['I' + i].prototype.m = function () { return i; };"
        " globalThis.callAll = () => { let s = 0;"
        " for (let i = 0; i < 60; i++) s += new globalThis['I' + i]().m();"
        " return s; };`);"
        "snapshot();"
        "var before = callAll();"
        "for (var i = 0; i < 60; i++)"
        " globalThis['I' + i].prototype.m = function () { return -1; };"
        "want(twins() === 60, 'twins ' + twins());"
        "want(callAll() === before, 'a browser read reached a page method');"
        "want(new I7().m() === -1, 'the page lost its method');"
        /* the browser drops every second interface: its twin goes, the
           others stay (a page's delete would leave the browser its
           binding) */
        "trusted('for (let i = 0; i < 60; i += 2) delete globalThis[\"I\" + i]');"
        "collect();"
        "want(twins() === 30, 'twins after collection ' + twins());"
        "want(trusted('new I7().m()') === 7, 'a kept twin was lost');"
        "want(trusted('new I59().m()') === 59, 'the last twin was lost');"
        "var result = bad.length ? bad.join(' ; ') : 'OK';";
    int failed = run_page(context, "twin-lifetime", source);
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    return failed;
}

/* Code compiled at run time by browser code is not browser code, and a
   page that never changes an interface costs nothing. */
static int test_run_time_compiles(void)
{
    JSRuntime *runtime;
    JSContext *context = open_realm(&runtime);
    CHECK(context != NULL);
    JS_SetLazyFunctionThreshold(runtime, 64);
    static const char source[] =
        "var bad = [];"
        "function want(c, m) { if (!c) bad.push(m); }"
        "trusted(`globalThis.compile = (s) => (0, eval)(s);"
        " globalThis.make = (s) => new Function(s);`);"
        "trusted(`globalThis.lazyBrowser = function () { const a = [];"
        " a.push(1); a.push(2); a.push(3); a.push(4); a.pop();"
        " /* long enough to be compiled lazily ............................"
        " ................................................................"
        " ................................................................ */"
        " return a.length; };`);"
        "snapshot();"
        "var calls = 0, push = Array.prototype.push;"
        "Array.prototype.push = function (x) { calls++;"
        " return push.call(this, x); };"
        "compile('[].push(1)'); make('[].push(1)')();"
        "want(calls === 2, 'run-time code read the browser view: ' + calls);"
        /* a function of the browser's compiled on its first call (a long
           one, with lazy compilation on) is still browser code */
        "want(lazyBrowser() === 3, 'a lazily compiled browser function');"
        "want(calls === 2, 'a lazy browser function ran page code');"
        "want(twins() === 1, 'twins ' + twins());"
        "var result = bad.length ? bad.join(' ; ') : 'OK';";
    int failed = run_page(context, "run-time-compiles", source);
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    return failed;
}

/* Global bindings: browser code names an object-valued global variable
   and gets the browser's, whatever the page assigned, declared, redefined
   or deleted under that name; the page gets its own. So does an accessor
   global the browser defined (a lazy placeholder) that the page redefines
   or deletes; an assignment through its setter, and globals the page
   made, stay shared state. */
static int test_global_bindings(void)
{
    JSRuntime *runtime;
    JSContext *context = open_realm(&runtime);
    CHECK(context != NULL);
    static const char setup[] =
        "var bad = [], calls = 0;"
        "function want(c, m) { if (!c) bad.push(m); }"
        /* a browser script declares names an earlier one already used */
        "trusted(`globalThis.useLater = () => typeof laterConst + typeof"
        " laterVar + typeof laterFn;`);"
        "trusted(`const laterConst = 1; var laterVar = 'x';"
        " function laterFn() {}`);"
        "trusted(`globalThis.config = { a: 1 };"
        " globalThis.cfgNamed = { a: 1 }; globalThis.cfgSame = { a: 1 };"
        " globalThis.cfgGone = { a: 1 }; globalThis.cfgRO = { a: 1 };"
        " Object.defineProperty(globalThis, 'lazyThing',"
        " { configurable: true, get() { return 5; } });"
        " Object.defineProperty(globalThis, 'LazyClass',"
        " { configurable: true, get() { return undefined; } });"
        " Object.defineProperty(globalThis, 'lazyGone',"
        " { configurable: true, get() { return 6; } });"
        " let slot = 0; Object.defineProperty(globalThis, 'lazySlot',"
        " { configurable: true, get() { return slot; },"
        " set(v) { slot = v; } });"
        " globalThis.G = {"
        " lazyClass() { return typeof LazyClass === 'function'"
        " ? new LazyClass().v : typeof LazyClass; },"
        " map() { return new Map([[1, 2]]).get(1); },"
        " string() { return String(12) + Number('3'); },"
        " math() { return Math.max(1, 2); },"
        " viaGlobal() { return new globalThis.Set([1]).size; },"
        " promise() { return typeof Promise.resolve; },"
        " lazy() { return lazyThing; },"
        " viaLazy() { return globalThis.lazyThing; },"
        " cfg() { return config && config.a; },"
        " named() { return cfgNamed.a; }, same() { return cfgSame.a; },"
        " cfgGone() { return typeof cfgGone === 'object' ? cfgGone.a"
        " : typeof cfgGone; }, ro() { return cfgRO.a; },"
        " weak() { return typeof WeakMap; },"
        " has() { return 'lazyThing' in globalThis; },"
        " gone() { return lazyGone + ':' + globalThis.lazyGone; },"
        " slot() { return lazySlot; },"
        " nc() { return typeof LazyNC === 'function' ? new LazyNC().v"
        " : String(LazyNC); },"
        " na() { return typeof LazyNA === 'function' ? new LazyNA().v"
        " : String(LazyNA); },"
        " late(name) { return typeof globalThis[name]; } };"
        " for (const name of ['LazyNC', 'LazyNA'])"
        " Object.defineProperty(globalThis, name,"
        " { configurable: true, get() { return 'placeholder'; } });"
        /* what a lazy module's load does: remove the placeholder, then
           bind the name */
        " globalThis.loadLazy = (name) => { delete globalThis[name];"
        " const seen = String(globalThis[name]);"
        " globalThis[name] = class { constructor() { this.v = 8; } };"
        " return seen; };`);"
        "snapshot();";
    static const char source[] =
        "function all() { return [G.map(), G.string(), G.math(),"
        " G.viaGlobal(), G.promise(), G.cfg(), G.weak()].join('|'); }"
        "var expected = all();"
        "want(useLater() === 'numberstringfunction', 'later declarations: '"
        " + useLater());"
        /* a page function that named the globals before changing them */
        "function early() { return [typeof Map, typeof String, typeof config]"
        ".join(); }"
        "early();"
        "var RealMap = Map, counted = function () { calls++; };"
        /* assignment by name and through the global object */
        "Map = function Map() { calls++; return {get() { calls++; }}; };"
        "globalThis.String = function () { calls++; return 'page'; };"
        "window = globalThis; window.Number = counted; window.Set = counted;"
        /* redefinition, deletion, a replaced accessor, a nulled object */
        "Object.defineProperty(globalThis, 'Promise',"
        " { configurable: true, get() { calls++; return 1; } });"
        "delete globalThis.WeakMap;"
        "delete globalThis.lazyGone; lazySlot = 3;"
        "Object.defineProperty(globalThis, 'lazyThing',"
        " { configurable: true, writable: true, value: 'page' });"
        "config = null;"
        /* declarations in a later script: var, function, let, class */
        "page('var Math = { max() { calls++; return -1; } };"
        " function Reflect() { calls++; }');"
        "page('let JSON = 1; class Symbol {}');"
        "calls = 0;"
        /* an accessor global the browser defined stays the browser's when
           the page redefines or deletes it; its setter is shared state */
        "want(G.lazy() === 5 && G.viaLazy() === 5 && G.has(),"
        " 'a replaced accessor global: ' + G.lazy() + ',' + G.viaLazy());"
        "want(G.gone() === '6:6' && typeof lazyGone === 'undefined',"
        " 'a deleted accessor global: ' + G.gone());"
        "want(G.slot() === 3 && lazySlot === 3, 'the setter of an accessor global:"
        " ' + G.slot());"
        "want(all() === expected, 'browser read a page binding: ' + all()"
        " + ' / ' + expected);"
        "want(calls === 0, 'page code ran ' + calls);"
        /* browser closures made after the changes bind the same way */
        "want(trusted('[typeof Map.prototype.get, String(1), Math.max(3, 4),"
        " typeof WeakMap, typeof JSON.parse, typeof Symbol.iterator,"
        " lazyThing, config.a].join()')"
        " === 'function,1,4,function,function,symbol,5,1', 'late browser code: '"
        " + trusted('[typeof Map.prototype.get, String(1), Math.max(3, 4),"
        " typeof WeakMap, typeof JSON.parse, typeof Symbol.iterator,"
        " lazyThing, config.a].join()'));"
        "want(calls === 0, 'page code ran in late browser code ' + calls);"
        /* the page has its own bindings, in old and new functions */
        "want(Map !== RealMap && String(1) === 'page' && Math.max(1) === -1"
        " && typeof WeakMap === 'undefined' && lazyThing === 'page'"
        " && config === null && JSON === 1 && Promise === 1,"
        " 'the page lost a binding');"
        "want(early() === 'function,function,object',"
        " 'an earlier page function: ' + early());"
        /* A browser change to a global the page has changed since is the
           browser's alone (the page replaced it, deleted it, or made it
           read-only); one the page only named, or set back to the value it
           had, still reaches both views, also once the browser's own
           change made it read-only. */
        "trusted('globalThis.config = { a: 2 }');"
        "want(G.cfg() === 2 && config === null,"
        " 'a browser assignment to a global the page replaced: ' + G.cfg()"
        " + ',' + config);"
        "function namesIt() { return cfgNamed.a; } namesIt();"
        "trusted('globalThis.cfgNamed = { a: 2 }');"
        "want(G.named() === 2 && namesIt() === 2 && cfgNamed.a === 2,"
        " 'a browser assignment to a global the page named: ' + G.named()"
        " + ',' + namesIt());"
        "trusted(`Object.defineProperty(globalThis, 'cfgNamed',"
        " { value: { a: 3 }, writable: false })`);"
        "trusted(`Object.defineProperty(globalThis, 'cfgNamed',"
        " { value: { a: 4 } })`);"
        "want(G.named() === 4 && namesIt() === 4,"
        " 'a browser definition after its own read-only one: ' + G.named()"
        " + ',' + namesIt());"
        "cfgSame = cfgSame;"
        "trusted('globalThis.cfgSame = { a: 2 }');"
        "want(G.same() === 2 && cfgSame.a === 2,"
        " 'a browser assignment to a global the page set back: ' + G.same()"
        " + ',' + cfgSame.a);"
        "delete globalThis.cfgGone;"
        "trusted(`Object.defineProperty(globalThis, 'cfgGone',"
        " { value: { a: 2 }, writable: true, configurable: true })`);"
        "want(G.cfgGone() === 2 && typeof cfgGone === 'undefined'"
        " && !('cfgGone' in globalThis),"
        " 'a browser definition of a global the page deleted: '"
        " + G.cfgGone() + ',' + typeof cfgGone);"
        "Object.defineProperty(globalThis, 'cfgRO', { writable: false });"
        "trusted(`Object.defineProperty(globalThis, 'cfgRO',"
        " { value: { a: 2 } })`);"
        "want(G.ro() === 2 && cfgRO.a === 1,"
        " 'a browser definition of a global the page made read-only: '"
        " + G.ro() + ',' + cfgRO.a);"
        /* A class the browser defines on first use, under a name both a
           browser function and a page function named while it was only a
           placeholder: the page's later assignment is the page's alone. */
        "function usesLazy() { return typeof LazyClass === 'function'"
        " ? new LazyClass().v : typeof LazyClass; }"
        "want(usesLazy() === 'undefined' && G.lazyClass() === 'undefined',"
        " 'placeholder');"
        "trusted(`Object.defineProperty(globalThis, 'LazyClass', {"
        " configurable: true, writable: true,"
        " value: class { constructor() { this.v = 7; } } })`);"
        "want(usesLazy() === 7 && G.lazyClass() === 7, 'lazy class: '"
        " + usesLazy() + ',' + G.lazyClass());"
        "LazyClass = function () { this.v = -1; };"
        "want(usesLazy() === -1 && G.lazyClass() === 7,"
        " 'lazy class after the page assigned it: ' + usesLazy() + ','"
        " + G.lazyClass());"
        /* a global the page made is the page's state: no pinning */
        "pageThing = { v: 1 }; var before = G.late('pageThing');"
        "pageThing = 5;"
        "want(before === 'object' && G.late('pageThing') === 'number',"
        " 'a page global was pinned');"
        /* A page binding the module's load cannot change (one the page
           made non-configurable, as a value or an accessor): the
           placeholder's removal and the module's binding are made in the
           browser's view alone, never copying the page's binding there,
           and the page's setter is not handed the browser's value. */
        "var handed = 0, PageNC = function () { this.v = -8; };"
        "Object.defineProperty(globalThis, 'LazyNC', { value: PageNC,"
        " writable: false, configurable: false });"
        "Object.defineProperty(globalThis, 'LazyNA', { get() { return PageNC; },"
        " set(v) { handed++; }, configurable: false });"
        "var seenNC = install(() => loadLazy('LazyNC')),"
        " seenNA = install(() => loadLazy('LazyNA'));"
        "want(seenNC === 'undefined' && seenNA === 'undefined',"
        " 'a refused delete of a kept global: ' + seenNC + ',' + seenNA);"
        "want(G.nc() === 8 && G.na() === 8 && handed === 0,"
        " 'a non-configurable page binding: ' + G.nc() + ',' + G.na() + ','"
        " + handed);"
        "want(LazyNC === PageNC && LazyNA === PageNC && new LazyNC().v === -8,"
        " 'the page lost its non-configurable binding');"
        /* A browser change the page's binding refuses (Reflect returns
           false) changed nothing: the page's functions are not made
           interfaces by it, so what the page later sets on them is what
           browser code reads. */
        "page('var pageDefined = function () {}, pageDeleted = function () {};"
        " pageDefined.v = 1; pageDeleted.v = 1;');"
        "var refused = trusted(`[Reflect.defineProperty(globalThis, 'pageDefined',"
        " { get() { return 1; } }), Reflect.deleteProperty(globalThis,"
        " 'pageDeleted')].join()`);"
        "pageDefined.v = 2; pageDeleted.v = 3;"
        "var after = trusted('[pageDefined.v, pageDeleted.v].join()');"
        "want(refused === 'false,false' && after === '2,3',"
        " 'a refused browser change made a page function an interface: '"
        " + refused + ' ' + after);"
        "var result = bad.length ? bad.join(' ; ') : 'OK';";
    int failed = run_page_after(context, "global-bindings", setup, source);
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    return failed;
}

/* Platform interface members: a function the page puts on an instance,
   or defines in a subclass, does not stand in for the interface's member
   when browser code calls it; the page's own calls get the page's. What
   is the page's to define stays so: methods the interface does not have,
   and toString and the like on its own objects. */
static int test_interface_members(void)
{
    JSRuntime *runtime;
    JSContext *context = open_realm(&runtime);
    CHECK(context != NULL);
    static const char setup[] =
        "var bad = [], calls = 0;"
        "function want(c, m) { if (!c) bad.push(m); }"
        "trusted(`globalThis.Iface = class Iface {"
        " remove() { return 'iface'; }"
        " get kind() { return 'iface'; }"
        " helper() { return this.remove() + ':' + this.kind; } };"
        " globalThis.callRemove = (o) => o.remove();"
        " globalThis.viaNative = (o) => Reflect.get(o, 'remove').call(o);"
        " globalThis.str = (o) => String(o) + ':' + o.toString();"
        " globalThis.fire = (l) => l.handleEvent();"
        " globalThis.connected = (o) => o.connectedCallback();"
        " globalThis.lateMember = () => {"
        " Iface.prototype.late = function () { return 1; };"
        " return new Iface().late(); };`);"
        "snapshot();";
    static const char source[] =
        "var o = new Iface();"
        "o.remove = function () { calls++; return 'page'; };"
        "Object.defineProperty(o, 'kind', { get() { calls++; return 'page'; } });"
        "class Sub extends Iface { remove() { calls++; return 'sub'; }"
        " connectedCallback() { return 'cb'; } }"
        "var s = new Sub(), b = new Iface();"
        "b.remove = (function () { calls++; return 'bound'; }).bind(null);"
        "want(callRemove(o) === 'iface' && callRemove(s) === 'iface'"
        " && callRemove(b) === 'iface' && viaNative(o) === 'iface'"
        " && o.helper() === 'iface:iface' && s.helper() === 'iface:iface',"
        " 'browser code called a page function for an interface member');"
        "want(calls === 0, 'page code ran ' + calls);"
        "want(o.remove() === 'page' && s.remove() === 'sub'"
        " && b.remove() === 'bound' && o.kind === 'page',"
        " 'the page lost its own member');"
        "want(connected(s) === 'cb', 'a method the interface lacks');"
        /* the browser completes a platform interface late, outside any
           module load: its own change, read by browser code */
        "want(lateMember() === 1 && twins() === 0,"
        " 'a late browser member: ' + twins());"
        "want(str({ toString() { return 'mine'; } }) === 'mine:mine',"
        " 'toString on a page object');"
        "want(fire({ handleEvent() { return 'h'; } }) === 'h', 'handleEvent');"
        "var result = bad.length ? bad.join(' ; ') : 'OK';";
    int failed = run_page_after(context, "interface-members", setup, source);
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    return failed;
}

/* A browser function the page can steer: it walks a key path the page
   chose with ordinary reads, so "__proto__" leads it out of the page's
   value to Object.prototype, where it defines the page's name. That write
   is browser code's, but not the browser's own: outside a module load a
   change to an ECMAScript prototype is copied on like a page's, and
   browser code does not read it. */
static int test_steered_write(void)
{
    JSRuntime *runtime;
    JSContext *context = open_realm(&runtime);
    CHECK(context != NULL);
    static const char setup[] =
        "var bad = [];"
        "function want(c, m) { if (!c) bad.push(m); }"
        "trusted(`globalThis.inject = (value, path, key) => {"
        " const parts = path.split('.'); let current = value;"
        " for (let i = 0; i + 1 < parts.length; i++) {"
        " let next = current[parts[i]];"
        " if (next === undefined) { next = {}; current[parts[i]] = next; }"
        " current = next; }"
        " Object.defineProperty(current, parts[parts.length - 1],"
        " { value: key, writable: true, enumerable: true,"
        " configurable: true }); };"
        " globalThis.merge = (target, source) => {"
        " for (const k in source) {"
        " if (source[k] && typeof source[k] === 'object') {"
        " merge(target[k], source[k]); } else target[k] = source[k]; } };"
        " globalThis.browserReads = () => [({}).steered, [].merged,"
        " ({}).capture].join();"
        " globalThis.bWrite = () => { Object.prototype.viaPage = 1; };`);"
        "snapshot();";
    static const char source[] =
        "inject({}, '__proto__.steered', 1);"
        "inject({}, 'a.__proto__.capture', true);"
        "merge([], JSON.parse('{\"__proto__\": {\"merged\": 2}}'));"
        "want(({}).steered === 1 && [].merged === 2 && ({}).capture === true,"
        " 'the page does not see the property it steered');"
        "want(browserReads() === ',,', 'browser code reads the steered"
        " property: ' + browserReads());"
        "want(twins() === 2, 'twins ' + twins());"
        /* a module load's own change to the same prototypes is the
           browser's, also after the page's */
        "install(() => trusted('Object.prototype.tfOwn = 5'));"
        "want(trusted('({}).tfOwn') === 5 && ({}).tfOwn === 5,"
        " 'a module load change');"
        /* but not one of browser code that page code called during the
           load (a page function the module reached) */
        "function pageHop() { bWrite(); }"
        "install(() => pageHop());"
        "want(trusted('({}).viaPage') === undefined && ({}).viaPage === 1,"
        " 'browser code the page called in a module load: '"
        " + trusted('({}).viaPage'));"
        "delete Object.prototype.steered; delete Object.prototype.capture;"
        "delete Array.prototype.merged; delete Object.prototype.tfOwn;"
        "delete Object.prototype.viaPage;"
        "var result = bad.length ? bad.join(' ; ') : 'OK';";
    int failed = run_page_after(context, "steered-write", setup, source);
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    return failed;
}

/* Browser changes to an interface the page changed first, beyond a plain
   member: a member the browser's view holds as an accessor, a receiver
   set, the prototype, and an object the page made non-extensible. The
   page keeps what it made and the browser's change is made in its own
   view (js_snap_page_changed()), whole: never half applied, never copied
   back from the page. */
static int test_browser_changes(void)
{
    JSRuntime *runtime;
    JSContext *context = open_realm(&runtime);
    CHECK(context != NULL);
    static const char setup[] =
        "var bad = [];"
        "function want(c, m) { if (!c) bad.push(m); }"
        "trusted(`globalThis.Acc = function Acc() {};"
        " Object.defineProperty(Acc.prototype, 'mode', { configurable: true,"
        " get() { return 'browser'; }, set(v) { this.setTo = v; } });"
        " globalThis.bAssignMode = (v) => { Acc.prototype.mode = v; };"
        " globalThis.bModeKind = () => typeof Object.getOwnPropertyDescriptor("
        "Acc.prototype, 'mode').get;"
        " globalThis.Recv = function Recv() {};"
        " Recv.prototype.m = function () { return 'b1'; };"
        " globalThis.bReflectSet = () => Reflect.set({}, 'm',"
        " function () { return 'b2'; }, Recv.prototype);"
        " globalThis.bRecv = () => new Recv().m();"
        " globalThis.Base = function Base() {};"
        " Base.prototype.who = function () { return 'base'; };"
        " globalThis.Later = function Later() {};"
        " Later.prototype.who = function () { return 'later'; };"
        " globalThis.Rep = function Rep() {}; globalThis.Sealed = function Sealed() {};"
        " Object.setPrototypeOf(Rep.prototype, Base.prototype);"
        " Object.setPrototypeOf(Sealed.prototype, Base.prototype);"
        " globalThis.bReparent = (C) => { try { Object.setPrototypeOf("
        "C.prototype, Later.prototype); return 'ok'; }"
        " catch (e) { return 'threw'; } };"
        " globalThis.bWho = (C) => new C().who();"
        " globalThis.Ext = function Ext() {};"
        " Ext.prototype.a = function () { return 1; };"
        " globalThis.bExtend = () => { Ext.prototype.added = function () {"
        " return 2; }; Object.defineProperty(Ext.prototype, 'defined',"
        " { value: 3, configurable: true });"
        " return new Ext().added() + new Ext().defined; };"
        " globalThis.Iface = function Iface() {};"
        " Iface.prototype.m = function () { return 'browser'; };`);"
        "snapshot();";
    static const char source[] =
        /* the browser's view holds an accessor, the page's a data member:
           the browser's assignment calls its accessor, on the object */
        "Object.defineProperty(Acc.prototype, 'mode', { value: 'page',"
        " writable: true, configurable: true });"
        "install(() => bAssignMode('module'));"
        "want(Acc.prototype.mode === 'page', 'accessor: the page lost its"
        " member: ' + Acc.prototype.mode);"
        "want(bModeKind() === 'function' && Acc.prototype.setTo === 'module',"
        " 'accessor: the browser view lost its accessor: ' + bModeKind());"
        /* a receiver set by browser code */
        "Recv.prototype.m = function () { return 'page'; };"
        "install(() => bReflectSet());"
        "want(new Recv().m() === 'page', 'receiver set: the page lost its"
        " member');"
        "want(bRecv() === 'b2', 'receiver set: the browser view: ' + bRecv());"
        /* the page changed the prototype: the browser's change is its own */
        "var pageProto = { who() { return 'page'; } };"
        "Object.setPrototypeOf(Rep.prototype, pageProto);"
        "want(bWho(Rep) === 'base', 'the browser reads the page prototype');"
        "want(install(() => bReparent(Rep)) === 'ok' && bWho(Rep) === 'later'"
        " && new Rep().who() === 'page', 'reparent after the page: '"
        " + bWho(Rep) + ',' + new Rep().who());"
        /* the page made it non-extensible: the browser's change is made,
           whole, in its view; the page's stays as the page made it */
        "Sealed.prototype.mark = 1; Object.preventExtensions(Sealed.prototype);"
        "var r = install(() => bReparent(Sealed));"
        "want(r === 'ok' && bWho(Sealed) === 'later' && new Sealed().who()"
        " === 'base', 'reparent of a non-extensible prototype: ' + r + ','"
        " + bWho(Sealed) + ',' + new Sealed().who());"
        "Object.preventExtensions(Ext.prototype);"
        "var added; try { added = install(() => bExtend()); }"
        " catch (e) { added = 'threw ' + e; }"
        "want(added === 5 && !('added' in Ext.prototype),"
        " 'a new member on a non-extensible prototype: ' + added);"
        /* a host native that brackets its work and has no frame of its
           own is the browser's also as a promise handler */
        "Iface.prototype.m = function () { return 'page'; };"
        "var b = bracketed(Iface.prototype, 'm'), viaJob;"
        "want(b() === 'browser', 'a bracketed call: ' + b());"
        "Promise.prototype.then.call(Promise.prototype.then.call("
        "Promise.resolve(), b), v => { viaJob = v; });"
        "globalThis.finish = () => { want(viaJob === 'browser',"
        " 'a bracketed promise handler: ' + viaJob);"
        " globalThis.result = bad.length ? bad.join(' ; ') : 'OK'; };";
    JSValue value = JS_Eval(context, setup, strlen(setup), "<setup>",
                            JS_EVAL_TYPE_GLOBAL);
    CHECK(!JS_IsException(value));
    JS_FreeValue(context, value);
    value = JS_Eval(context, source, strlen(source), "browser-changes",
                    JS_EVAL_TYPE_GLOBAL);
    int failed = JS_IsException(value);
    if (failed) {
        JSValue error = JS_GetException(context);
        const char *text = JS_ToCString(context, error);
        fprintf(stderr, "browser-changes: threw %s\n", text ? text : "?");
        JS_FreeCString(context, text);
        JS_FreeValue(context, error);
    }
    JS_FreeValue(context, value);
    JSContext *job_context;
    while (!failed && JS_ExecutePendingJob(runtime, &job_context) > 0) {}
    if (!failed)
        failed = run_page(context, "browser-changes", "finish();");
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    return failed;
}

/* A prototype change browser code makes is checked for a cycle in the
   chain browser code walks, which continues from twins: where the change
   is made in the browser's view alone, where it reaches both views, and
   a page's change to an object the browser's view reaches the same way.
   A refused change leaves the chain browser code walks finite. */
/* The browser's own bindings of globals the page changed, as the kept
   table holds them. A non-configurable variable the page only named is
   not one: the browser's changes reach both views. Otherwise the browser's
   changes to a variable its closures hold are made there, and change
   those closures' variable only when they are made (a refused accessor
   definition leaves it; a read-only definition keeps it attached); a
   deletion there unbinds the name for the browser, also while the page's
   binding holds undefined; and, on a global object the page made
   non-extensible, a lazy module's load of a name the page deleted or
   replaced binds it for the browser. */
static int test_kept_globals(void)
{
    JSRuntime *runtime;
    JSContext *context = open_realm(&runtime);
    CHECK(context != NULL);
    static const char setup[] =
        "var bad = [];"
        "function want(c, m) { if (!c) bad.push(m); }"
        "trusted(`var NCV = { tag: 'browser' }, NCB = { t: 'b0' };"
        " globalThis.PV = { t: 'b0' }; globalThis.NSV = { a: 1 };"
        " globalThis.UV = { a: 1 };"
        " Object.defineProperty(globalThis, 'NEV', { value: { t: 'b0' },"
        " writable: true, configurable: true, enumerable: false });"
        " globalThis.G = { ncv() { return NCV.tag; }, ncb() { return NCB.t; },"
        " nev() { const v = globalThis.NEV; return v && v.t || typeof v; },"
        " pv() { return PV.t + '/' + globalThis.PV.t; },"
        " nsv() { return typeof NSV === 'object' ? NSV.a : typeof NSV; },"
        " uv() { return typeof UV === 'object' ? UV.a : typeof UV; } };"
        " globalThis.bdef = (n, d) => Reflect.defineProperty(globalThis, n, d);"
        " globalThis.bdel = (n) => Reflect.deleteProperty(globalThis, n);"
        " globalThis.bset = (n, v) => { globalThis[n] = v; };`);"
        "snapshot();";
    static const char source[] =
        /* a non-configurable variable the page replaced: the browser's
           refused accessor definition leaves its closures' variable */
        "function namesNCV() { return NCV.tag; }"
        "NCV = { tag: 'page' };"
        "var refused = bdef('NCV', { get() { return 1; } });"
        "want(refused === false && G.ncv() === 'browser'"
        " && namesNCV() === 'page', 'a refused accessor definition: '"
        " + refused + ',' + G.ncv() + ',' + namesNCV());"
        /* a non-configurable variable the page only named: the browser's
           assignment and definition reach both views */
        "function namesNCB() { return NCB.t; }"
        "bset('NCB', { t: 'b1' }); var assigned = namesNCB() + G.ncb();"
        "bdef('NCB', { value: { t: 'b2' } });"
        "want(assigned === 'b1b1' && namesNCB() === 'b2' && G.ncb() === 'b2',"
        " 'a browser change to a non-configurable variable the page named: '"
        " + assigned + ',' + namesNCB() + ',' + G.ncb());"
        /* a variable the page made non-configurable: a read-only data
           definition and the next one reach browser closures and browser
           reads of the property alike */
        "function namesPV() { return PV.t; }"
        "Object.defineProperty(globalThis, 'PV', { configurable: false });"
        "var ro = bdef('PV', { value: { t: 'b1' }, writable: false }),"
        " read1 = G.pv(), again = bdef('PV', { value: { t: 'b2' } });"
        "want(ro && read1 === 'b1/b1' && again && G.pv() === 'b2/b2'"
        " && namesPV() === 'b0', 'a read-only definition of a kept variable: '"
        " + ro + ' ' + read1 + ' ' + again + ' ' + G.pv() + ' ' + namesPV());"
        /* deleting a kept variable unbinds it for the browser; its next
           definition and assignment bind it there */
        "NSV = { a: 2 };"
        "Object.defineProperty(globalThis, 'NSV', { configurable: false });"
        "var deleted = bdel('NSV'), unbound = G.nsv();"
        "want(deleted === true && unbound === 'undefined' && NSV.a === 2,"
        " 'a browser deletion of a kept variable: ' + deleted + ',' + unbound);"
        "bdef('NSV', { value: { a: 3 }, writable: true, configurable: true });"
        "var defined = G.nsv(); bset('NSV', { a: 4 });"
        "want(defined === 3 && G.nsv() === 4 && NSV.a === 2,"
        " 'a browser definition after its deletion: ' + defined + ','"
        " + G.nsv());"
        /* the page set the variable to undefined: once the browser deleted
           it in its view alone, its definition stays there too */
        "UV = undefined;"
        "bdel('UV'); bdef('UV', { value: { a: 5 }, writable: true,"
        " configurable: true, enumerable: true });"
        "want(G.uv() === 5 && UV === undefined,"
        " 'a page binding equal to an unbound one: ' + G.uv() + ',' + UV);"
        /* a shared non-enumerable global the page only named, which the
           browser set to undefined (in both views): its next binding
           reaches both views too, as it is not unbound */
        "(function () { return NEV; })();"
        "bset('NEV', undefined); bset('NEV', { t: 'b2' });"
        "var pageNEV = globalThis.NEV && globalThis.NEV.t || typeof NEV;"
        "want(pageNEV === 'b2' && G.nev() === 'b2',"
        " 'a shared global the browser set to undefined, then bound: '"
        " + pageNEV + '/' + G.nev());"
        "var result = bad.length ? bad.join(' ; ') : 'OK';";
    int failed = run_page_after(context, "kept-globals", setup, source);
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);

    context = open_realm(&runtime);
    CHECK(context != NULL);
    static const char lazy_setup[] =
        "var bad = [];"
        "function want(c, m) { if (!c) bad.push(m); }"
        "trusted(`for (const n of ['LazyGone', 'LazyPage'])"
        " Object.defineProperty(globalThis, n,"
        " { configurable: true, get() { return 'placeholder'; } });"
        " globalThis.G = { use(n) { const C = globalThis[n];"
        " return typeof C === 'function' ? new C().v : String(C); } };"
        " globalThis.loadLazy = (name) => { delete globalThis[name];"
        " const seen = String(globalThis[name]);"
        " globalThis[name] = class { constructor() { this.v = 8; } };"
        " return seen; };`);"
        "snapshot();";
    static const char lazy_source[] =
        "var Page = function () { this.v = -1; };"
        "delete globalThis.LazyGone;"
        "Object.defineProperty(globalThis, 'LazyPage', { value: Page,"
        " writable: true, configurable: true });"
        "Object.preventExtensions(globalThis);"
        "var seen = [install(() => loadLazy('LazyGone')),"
        " install(() => loadLazy('LazyPage'))].join();"
        "want(seen === 'undefined,undefined' && G.use('LazyGone') === 8"
        " && G.use('LazyPage') === 8, 'a load on a non-extensible global: '"
        " + seen + ' ' + G.use('LazyGone') + ',' + G.use('LazyPage'));"
        "want(!('LazyGone' in globalThis) && LazyPage === Page,"
        " 'the page lost its binding on a non-extensible global');"
        "var result = bad.length ? bad.join(' ; ') : 'OK';";
    failed |= run_page_after(context, "kept-globals-non-extensible",
                             lazy_setup, lazy_source);
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    return failed;
}

/* A browser script that runs after the snapshot declares no global: the
   engine refuses it (it would take the page's reference), and the page's
   binding is unchanged. Before the snapshot, and in a realm without one,
   the same declarations are made. */
#define DECLARATIONS_SETUP                                                  \
        "var bad = [];"                                                     \
        "function want(c, m) { if (!c) bad.push(m); }"                      \
        "function declare(n) {"                                             \
        " return ['var x' + n + ' = 1', 'function f' + n + '() {}',"        \
        " 'let y' + n + ' = 1'].map((code) => {"                            \
        " try { trusted(code); return 'made'; }"                            \
        " catch (e) { return e instanceof InternalError ? 'refused'"        \
        " : String(e); } }).join(); }"                                      \
        "var before = declare(1);"                                          \
        "want(before === 'made,made,made', 'before the snapshot: ' + before);"

static int test_browser_declarations(void)
{
    static const char setup[] = DECLARATIONS_SETUP "snapshot();";
    /* a realm without the snapshot takes none */
    static const char setup_off[] = DECLARATIONS_SETUP;
    static const char source[] =
        "var Page = function () {}; Map = Page;"
        "var after = declare(2);"
        "want(after === 'refused,refused,refused', 'after the snapshot: '"
        " + after);"
        "want(Map === Page && typeof x2 === 'undefined'"
        " && typeof f2 === 'undefined'"
        " && trusted('typeof Map.prototype.get') === 'function',"
        " 'a refused declaration changed a binding');"
        "var result = bad.length ? bad.join(' ; ') : 'OK';";
    static const char source_off[] =
        "var after = declare(2);"
        "want(after === 'made,made,made', 'without the snapshot: ' + after);"
        "var result = bad.length ? bad.join(' ; ') : 'OK';";
    int failed = 0;
    for (int with_snapshot = 1; with_snapshot >= 0; with_snapshot--) {
        JSRuntime *runtime;
        JSContext *context = open_realm_with(&runtime, with_snapshot);
        CHECK(context != NULL);
        failed |= run_page_after(context, with_snapshot
                                 ? "browser-declarations"
                                 : "browser-declarations-off",
                                 with_snapshot ? setup : setup_off,
                                 with_snapshot ? source : source_off);
        JS_FreeContext(context);
        JS_FreeRuntime(runtime);
    }
    return failed;
}

static int test_cyclic_reparent(void)
{
    JSRuntime *runtime;
    JSContext *context = open_realm(&runtime);
    CHECK(context != NULL);
    static const char setup[] =
        "var bad = [];"
        "function want(c, m) { if (!c) bad.push(m); }"
        "trusted(`for (const n of ['A', 'B', 'C', 'D'])"
        " globalThis[n] = function () {};"
        " globalThis.E = function () {}; globalThis.NSX = { a: 1 };"
        " globalThis.bSet = (o, p) => Reflect.setPrototypeOf(o, p);"
        " globalThis.bMissing = (C) => new C().missing;"
        " globalThis.bMissingGlobal = () => globalThis.missingName;`);"
        "snapshot();";
    static const char source[] =
        /* the page reparented A.prototype: the browser's reparent is its
           view's alone, and its own chain from y reaches A.prototype */
        "Object.setPrototypeOf(A.prototype, {});"
        "var y = Object.create(A.prototype);"
        "var r = bSet(A.prototype, y);"
        "want(r === false, 'a cycle in the browser view alone: ' + r);"
        "if (r === false) want(bMissing(A) === undefined, 'lookup');"
        /* B.prototype leads to C.prototype in the browser's view only;
           C.prototype's reparent reaches both views */
        "Object.setPrototypeOf(B.prototype, null);"
        "C.prototype.m = function () {};"
        "want(bSet(B.prototype, C.prototype) === true, 'a browser reparent');"
        "r = bSet(C.prototype, B.prototype);"
        "want(r === false, 'a cycle in the browser view of both: ' + r);"
        "if (r === false) want(bMissing(C) === undefined && bMissing(B)"
        " === undefined, 'lookups');"
        /* the browser's view of D.prototype leads to the page's o: the
           page's own reparent of o onto D.prototype would close a cycle
           there (and without the snapshot, where both views are one) */
        "Object.setPrototypeOf(D.prototype, null);"
        "var o = {};"
        "want(bSet(D.prototype, o) === true, 'a browser reparent to o');"
        "r = Reflect.setPrototypeOf(o, D.prototype);"
        "want(r === false, 'a page change closing a browser view cycle: '"
        " + r);"
        "if (r === false) want(bMissing(D) === undefined, 'lookup of D');"
        /* The same for the global object, which has no twin (naming a
           pinned global gives it kept bindings): the browser's view of
           E.prototype leads to it, so the page's reparent of the global
           object onto E.prototype would close a cycle there. Last, as an
           accepted one leaves the global's chain looping for the
           browser. */
        "function namesNSX() { return NSX; }"
        "Object.setPrototypeOf(E.prototype, null);"
        "want(bSet(E.prototype, globalThis) === true,"
        " 'a browser reparent to the global object');"
        "r = Reflect.setPrototypeOf(globalThis, E.prototype);"
        "want(r === false, 'a page reparent of the global object closing a"
        " browser view cycle: ' + r);"
        "if (r === false) want(bMissingGlobal() === undefined,"
        " 'lookup of a missing global');"
        "var result = bad.length ? bad.join(' ; ') : 'OK';";
    int failed = run_page_after(context, "cyclic-reparent", setup, source);
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    return failed;
}

/* Promise jobs. A job runs with no script frame, so what it does on its
   own (resolving the promise its reaction settles with the handler's
   result, the rest of an await's resumption) acts for the code the job
   runs for, not for the host draining the queue: a page's patch of
   Promise.prototype.then sees the page's promise work exactly as often as
   it does without the snapshot (zone.js counts on this), and the
   browser's promise work, or the host's outside any script, still reads
   the browser's view. Each scenario runs in a realm with the snapshot and
   in one without, and the counts are compared. */
static const char promise_browser[] =
    "trusted(`globalThis.bReturn = (p) => () => p;"
    " globalThis.bChain = (p) => Promise.resolve().then(() => p);"
    " globalThis.bAsync = async (p) => { await null; return p; };"
    " globalThis.bResolve = (p) => new Promise(r => r(p));"
    " globalThis.bThen = (q, f) => q.then(f);"
    " globalThis.bInvoke = (f) => { f(); };"
    " globalThis.bAsyncReturn = async (p) => p;"
    " globalThis.bAsyncAwait = async (p) => { await p; };`);";

static const char promise_page[] =
    "var t = Promise.prototype.then, n = 0, m = 0, counts = [];"
    "Promise.prototype.then = function (...a) { n++;"
    " return t.apply(this, a); };"
    "var p = Promise.resolve(1), getter = { configurable: true,"
    " get() { m++; return undefined; } };"
    /* an await of a promise whose constructor is not Promise resolves a
       new promise with it, which calls its then */
    "function ctor(c) { Object.defineProperty(Promise.prototype,"
    " 'constructor', { value: c, configurable: true, writable: true }); }"
    "var scenarios = ["
    /* the page's own promise work: as often as without the snapshot */
    " () => { new Promise(r => r(p)); },"
    " () => { t.call(Promise.resolve(), () => p); },"
    " () => { (async () => p)(); },"
    " () => { (async () => { await null; return p; })(); },"
    " () => { t.call(Promise.resolve(), bReturn(p)); },"
    " () => { t.call(Promise.reject(1), null, () => p); },"
    " () => { Promise.resolve(1).finally(() => {}); },"
    " () => { Object.defineProperty(Object.prototype, 'then', getter);"
    " (async function* () { await null; yield 1; })().next(); },"
    " () => { Object.defineProperty(Object.prototype, 'then', getter);"
    " (async () => { await null; return {}; })(); },"
    /* a page async function the browser calls (an event listener, a
       timer): its first segment returns, or awaits, as the page's */
    " () => { bInvoke(async () => p); },"
    " () => { ctor(function Z() {}); bInvoke(async () => { await p; }); },"
    /* the browser's: its own view with the snapshot */
    " () => { bChain(p); },"
    " () => { bAsync(p); },"
    " () => { bResolve(p); },"
    /* a browser async function the page calls: its first segment
       returns, or awaits, as the browser's */
    " () => { bAsyncReturn(p); },"
    " () => { ctor(function Z() {}); bAsyncAwait(p); },"
    /* the browser asks, the page's handler answers: the page's view for
       the handler's result */
    " () => { bThen(Promise.resolve(), () => p); },"
    "];"
    "function start(i) { n = 0; m = 0; scenarios[i](); }"
    "function stop() { delete Object.prototype.then; ctor(Promise);"
    " counts.push(n + ':' + m); }";

enum { PROMISE_PAGE_SCENARIOS = 11, PROMISE_SCENARIOS = 17 };

static void drain_jobs(JSRuntime *runtime)
{
    JSContext *job_context;
    while (JS_ExecutePendingJob(runtime, &job_context) > 0) {}
}

static int eval_ok(JSContext *context, const char *source)
{
    JSValue value = JS_Eval(context, source, strlen(source), "<page>",
                            JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(value)) {
        JSValue error = JS_GetException(context);
        const char *text = JS_ToCString(context, error);
        fprintf(stderr, "promise-jobs: threw %s\n", text ? text : "?");
        JS_FreeCString(context, text);
        JS_FreeValue(context, error);
        return 0;
    }
    JS_FreeValue(context, value);
    return 1;
}

/* Fills counts[] ("then calls:Object.prototype.then reads") for every
   scenario and the host's frameless resolution last. */
static int promise_counts(bool with_snapshot,
                          char counts[][16], size_t count_size)
{
    JSRuntime *runtime;
    JSContext *context = open_realm(&runtime);
    CHECK(context != NULL);
    CHECK(eval_ok(context, promise_browser));
    if (with_snapshot) CHECK(eval_ok(context, "snapshot();"));
    CHECK(eval_ok(context, promise_page));
    for (int i = 0; i < PROMISE_SCENARIOS; i++) {
        char call[32];
        snprintf(call, sizeof(call), "start(%d);", i);
        CHECK(eval_ok(context, call));
        drain_jobs(runtime);
        CHECK(eval_ok(context, "stop();"));
    }
    /* the host resolves a promise with the page's p outside any script */
    CHECK(eval_ok(context, "n = 0; m = 0;"));
    JSValue global = JS_GetGlobalObject(context);
    JSValue page_promise = JS_GetPropertyStr(context, global, "p");
    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(context, resolving);
    CHECK(!JS_IsException(promise));
    JSValue value = JS_Call(context, resolving[0], JS_UNDEFINED, 1,
                            (JSValueConst *)&page_promise);
    JS_FreeValue(context, value);
    JS_FreeValue(context, resolving[0]);
    JS_FreeValue(context, resolving[1]);
    JS_FreeValue(context, promise);
    JS_FreeValue(context, page_promise);
    JS_FreeValue(context, global);
    drain_jobs(runtime);
    CHECK(eval_ok(context, "stop();"));
    JSValue list = JS_Eval(context, "counts.join(',')", 16, "<page>",
                           JS_EVAL_TYPE_GLOBAL);
    const char *text = JS_ToCString(context, list);
    CHECK(text != NULL);
    const char *at = text;
    for (int i = 0; i <= PROMISE_SCENARIOS; i++) {
        size_t length = strcspn(at, ",");
        CHECK(length > 0 && length < count_size);
        memcpy(counts[i], at, length);
        counts[i][length] = '\0';
        at += length + (at[length] == ',');
    }
    JS_FreeCString(context, text);
    JS_FreeValue(context, list);
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    return 0;
}

static int test_promise_jobs(void)
{
    char with[PROMISE_SCENARIOS + 1][16], without[PROMISE_SCENARIOS + 1][16];
    CHECK(promise_counts(true, with, sizeof(with[0])) == 0);
    CHECK(promise_counts(false, without, sizeof(without[0])) == 0);
    int failed = 0;
    for (int i = 0; i <= PROMISE_SCENARIOS; i++) {
        bool page = i < PROMISE_PAGE_SCENARIOS;
        const char *want = page ? without[i]
            : i == PROMISE_SCENARIOS - 1 ? "1:0" : "0:0";
        if (strcmp(without[i], "0:0") == 0 || strcmp(with[i], want) != 0) {
            fprintf(stderr, "promise-jobs: scenario %d: %s with the snapshot,"
                    " %s without, want %s\n", i, with[i], without[i], want);
            failed = 1;
        }
    }
    if (!failed) printf("promise-jobs: OK\n");
    return failed;
}

/* The embedder's view outside any script: Iface.prototype.m as the host
   reads it, called. The browser's, unless a script or job left a view of
   its own behind. */
static int embedder_reads_browser(JSContext *context)
{
    JSValue global = JS_GetGlobalObject(context);
    JSValue iface = JS_GetPropertyStr(context, global, "Iface");
    JSValue proto = JS_GetPropertyStr(context, iface, "prototype");
    JSValue m = JS_GetPropertyStr(context, proto, "m");
    JSValue got = JS_Call(context, m, proto, 0, NULL);
    const char *text = JS_ToCString(context, got);
    int browser = text != NULL && strcmp(text, "browser") == 0;
    JS_FreeCString(context, text);
    JS_FreeValue(context, got);
    JS_FreeValue(context, m);
    JS_FreeValue(context, proto);
    JS_FreeValue(context, iface);
    JS_FreeValue(context, global);
    return browser;
}

/* Async generators driven from the other side, through return() and
   throw() as well as next(): a page generator browser code drives, and a
   browser generator page code drives. Each generator's own code, its
   finally block after an await included, reads its own view, and once a
   script or any promise job has run the view is the embedder's again. */
static int test_async_generator_views(void)
{
    JSRuntime *runtime;
    JSContext *context = open_realm(&runtime);
    CHECK(context != NULL);
    static const char setup[] =
        "var seen = [];"
        "trusted(`globalThis.Iface = function Iface() {};"
        " Iface.prototype.m = function () { return 'browser'; };"
        " globalThis.bGen = async function* () { try { yield 1; yield 2; }"
        " finally { await null; seen.push('browser gen:'"
        " + Iface.prototype.m()); } };"
        " globalThis.bReturn = (it) => { it.next(); return it.return(5); };"
        " globalThis.bThrow = (it) => { it.next();"
        " return it.throw(new Error('x')).catch(() => {}); };`);"
        "snapshot();"
        "Iface.prototype.m = function () { return 'page'; };"
        "var pg = async function* () { try { yield 1; yield 2; }"
        " finally { await null; seen.push('page gen:' + Iface.prototype.m()); } };"
        "var scenarios = ["
        " () => bReturn(pg()),"
        " () => bThrow(pg()),"
        " () => { const it = bGen(); it.next(); it.return(5); },"
        " () => { const it = bGen(); it.next();"
        " it.throw(new Error('x')).catch(() => {}); },"
        "];";
    static const char *const want[] = {
        "page gen:page", "page gen:page", "browser gen:browser",
        "browser gen:browser",
    };
    CHECK(eval_ok(context, setup));
    int failed = 0;
    for (int i = 0; i < 4; i++) {
        char call[48];
        snprintf(call, sizeof(call), "seen.length = 0; scenarios[%d]();", i);
        CHECK(eval_ok(context, call));
        int views_ok = embedder_reads_browser(context), jobs = 0;
        JSContext *job_context;
        while (JS_ExecutePendingJob(runtime, &job_context) > 0) {
            jobs++;
            views_ok &= embedder_reads_browser(context);
        }
        JSValue list = JS_Eval(context, "seen.join()", 11, "<page>",
                               JS_EVAL_TYPE_GLOBAL);
        const char *text = JS_ToCString(context, list);
        if (!views_ok || jobs == 0 || text == NULL
            || strcmp(text, want[i]) != 0) {
            fprintf(stderr, "async-generator-views: scenario %d: %s, %d"
                    " jobs, generator read %s, want %s\n", i,
                    views_ok ? "view restored" : "a view was left behind",
                    jobs, text ? text : "?", want[i]);
            failed = 1;
        }
        JS_FreeCString(context, text);
        JS_FreeValue(context, list);
    }
    if (!failed) printf("async-generator-views: OK\n");
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    return failed;
}

#if !defined(__PSP__)
/* Refusing each allocation of the page's first change in turn either fails
   that change with no twin left behind, or completes it; either way the
   browser's view is intact and a later change succeeds. */
static int test_refused_twin(void)
{
    unsigned refused = 0, completed = 0;
    for (unsigned nth = 1; nth < 400; nth++) {
        JSRuntime *runtime;
        JSContext *context = open_realm(&runtime);
        CHECK(context != NULL);
        static const char setup[] =
            "trusted('globalThis.read = () => [3, 4].map(v => v * 2)"
            ".join()');"
            "snapshot(); var before = read();"
            "var wrapper = function () { return []; };";
        JSValue value = JS_Eval(context, setup, strlen(setup), "setup",
                                JS_EVAL_TYPE_GLOBAL);
        CHECK(!JS_IsException(value));
        JS_FreeValue(context, value);
        static const char change[] = "Array.prototype.map = wrapper;";
        JS_TestRefuseAllocation(nth);
        value = JS_Eval(context, change, strlen(change), "change",
                        JS_EVAL_TYPE_GLOBAL);
        bool pending = JS_TestRefusalPending() != 0;
        JS_TestRefuseAllocation(0);
        bool threw = JS_IsException(value);
        if (threw) JS_FreeValue(context, JS_GetException(context));
        JS_FreeValue(context, value);
        static const char check[] =
            "var bad = [];"
            "function want(c, m) { if (!c) bad.push(m); }"
            "want(read() === before, 'the browser view changed');"
            "want(twins() === (Array.prototype.map === wrapper ? 1 : 0),"
            " 'twins ' + twins());"
            "Array.prototype.map = wrapper;"
            "want(read() === before && twins() === 1 && [1].map().length === 0,"
            " 'a later change failed');"
            "var result = bad.length ? bad.join(' ; ') : 'OK';";
        char name[32];
        snprintf(name, sizeof(name), "refused-twin-%u", nth);
        int failed = run_page(context, name, check);
        JS_FreeContext(context);
        JS_FreeRuntime(runtime);
        if (failed) return 1;
        if (threw) refused++; else completed++;
        if (pending) break;  /* the change needs fewer allocations */
    }
    printf("refused-twin: %u refused, %u completed\n", refused, completed);
    CHECK(refused > 0);
    return 0;
}
#endif

int main(void)
{
    int failed = test_page_changes();
    failed |= test_twin_lifetime();
    failed |= test_run_time_compiles();
    failed |= test_global_bindings();
    failed |= test_interface_members();
    failed |= test_steered_write();
    failed |= test_browser_changes();
    failed |= test_kept_globals();
    failed |= test_browser_declarations();
    failed |= test_cyclic_reparent();
    failed |= test_promise_jobs();
    failed |= test_async_generator_views();
#if !defined(__PSP__)
    failed |= test_refused_twin();
#endif
    return failed;
}
