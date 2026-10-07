/* Lazy function compilation in the vendored QuickJS
   (JS_SetLazyFunctionThreshold): a function kept lazy is compiled only on
   its first call. With preparsing (JS_SetLazyFunctionPreparse, the default)
   its body is only validated when the script compiles (by a preparser that
   builds nothing and gives up on what it cannot vouch for); without, it is
   parsed and checked in full. Every case runs the same script three times: eagerly,
   lazily with full parses and lazily with preparsing (threshold 1: every
   eligible function), and requires the same result from all three. The lazy
   runs must defer the same functions (so a regression that silently
   compiles everything eagerly fails too), the preparsing run must have
   skipped bodies, and every deferred body of the script must compile. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilefinch/budget.h"
#include "tilefinch/budget_quickjs.h"

#define MIB (1024u * 1024u)

static size_t test_stack_limit(void)
{
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
    return 4u * MIB;
#endif
#elif defined(__SANITIZE_ADDRESS__)
    return 4u * MIB;
#endif
    return 256u * 1024u;
}

/* ---------------------------------------------------------------- realms */

typedef struct {
    const char *name;
    const char *source;
} ModuleSource;

typedef struct {
    Budget budget;
    BudgetQuickJSPool *pool;
    JSRuntime *rt;
    JSContext *ctx;
    const ModuleSource *modules;
    int strip_flags;
} Realm;

static JSModuleDef *realm_module_loader(JSContext *ctx, const char *name,
                                        void *opaque)
{
    Realm *realm = opaque;
    for (const ModuleSource *m = realm->modules; m != NULL && m->name != NULL;
         m++) {
        if (strcmp(m->name, name) != 0) continue;
        JSValue value = JS_Eval(ctx, m->source, strlen(m->source), name,
                                JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(value)) return NULL;
        JSModuleDef *module = JS_VALUE_GET_PTR(value);
        JS_FreeValue(ctx, value);
        return module;
    }
    JS_ThrowReferenceError(ctx, "could not load module '%s'", name);
    return NULL;
}

static bool realm_open_preparse(Realm *realm, uint32_t threshold, bool preparse);

static bool realm_open(Realm *realm, uint32_t threshold)
{
    return realm_open_preparse(realm, threshold, true);
}

static bool realm_open_preparse(Realm *realm, uint32_t threshold, bool preparse)
{
    memset(realm, 0, sizeof(*realm));
    budget_init(&realm->budget, 16u * MIB);
    realm->pool = budget_quickjs_pool_create(&realm->budget);
    if (realm->pool == NULL) return false;
    realm->rt = JS_NewRuntime2(budget_quickjs_pool_allocator(), realm->pool);
    if (realm->rt == NULL) return false;
    JS_SetMemoryLimit(realm->rt, 8u * MIB);
    JS_SetMaxStackSize(realm->rt, test_stack_limit());
    realm->ctx = JS_NewContext(realm->rt);
    if (realm->ctx == NULL) return false;
    JS_SetModuleLoaderFunc(realm->rt, NULL, realm_module_loader, realm);
    JS_SetLazyFunctionThreshold(realm->rt, threshold);
    JS_SetLazyFunctionPreparse(realm->rt, preparse);
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

static JSLazyFunctionStats realm_stats(Realm *realm)
{
    JSLazyFunctionStats stats;
    JS_GetLazyFunctionStats(realm->rt, &stats);
    return stats;
}

static void run_jobs(Realm *realm)
{
    JSContext *job_context = NULL;
    for (unsigned turns = 0; turns < 1024
         && JS_ExecutePendingJob(realm->rt, &job_context) > 0; turns++) {}
}

/* The pending exception as "Name: message", or its string form. */
static char *exception_text(JSContext *ctx)
{
    JSValue exception = JS_GetException(ctx);
    const char *text = JS_ToCString(ctx, exception);
    size_t length = text != NULL ? strlen(text) : 0;
    char *copy = malloc(length + 5);
    if (copy != NULL) {
        memcpy(copy, "EXC ", 4);
        if (text != NULL) memcpy(copy + 4, text, length);
        copy[length + 4] = '\0';
    }
    JS_FreeCString(ctx, text);
    JS_FreeValue(ctx, exception);
    return copy;
}

static char *value_text(JSContext *ctx, JSValue value)
{
    if (JS_IsException(value)) return exception_text(ctx);
    const char *text = JS_ToCString(ctx, value);
    JS_FreeValue(ctx, value);
    if (text == NULL) return exception_text(ctx);
    char *copy = strdup(text);
    JS_FreeCString(ctx, text);
    return copy;
}

/* Run a classic script (compile, then run) and return String(result). */
static char *realm_run_script(Realm *realm, const char *source)
{
    JSValue compiled = JS_Eval(realm->ctx, source, strlen(source), "case.js",
                               JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(compiled)) return exception_text(realm->ctx);
    JSValue value = JS_EvalFunction(realm->ctx, compiled);
    char *text = value_text(realm->ctx, value);
    run_jobs(realm);
    return text;
}

/* Evaluate a module and return String(globalThis.result) once its jobs ran. */
static char *realm_run_module(Realm *realm, const char *source)
{
    JSValue promise = JS_Eval(realm->ctx, source, strlen(source), "case.mjs",
                              JS_EVAL_TYPE_MODULE);
    if (JS_IsException(promise)) return exception_text(realm->ctx);
    JS_FreeValue(realm->ctx, promise);
    run_jobs(realm);
    JSValue global = JS_GetGlobalObject(realm->ctx);
    JSValue result = JS_GetPropertyStr(realm->ctx, global, "result");
    JS_FreeValue(realm->ctx, global);
    return value_text(realm->ctx, result);
}

/* ----------------------------------------------------------- cases */

typedef struct {
    const char *name;
    const char *source;
    const char *expected;
    bool module;
    const ModuleSource *modules;
    bool compile_failures_allowed;
    bool nothing_to_preparse; /* every lazy body needs a full parse */
    bool all_preparsed; /* the scan takes every function the script defers */
} LazyCase;

static const ModuleSource library_modules[] = {
    { "lib", "export let counter = 0;"
             "export function bump(){ counter++; return counter; }"
             "export const table = { twice(v){ return v * 2; } };"
             "export default function(){ return 'default:' + counter; }" },
    { NULL, NULL }
};

static const LazyCase cases[] = {
    { "captured accessor returns stay owned across replacement and calls",
      "function make() { var value={id:17}; return function(){return value;}; }"
      "function probe() { const getter=make(), node={};"
      " Object.defineProperty(node,'value',{get:getter,configurable:true});"
      " const a=node.value, b=getter.call(null,99), c=getter.apply(null,[77]);"
      " delete node.value; return [a===b,b===c,a.id].join(','); } probe()",
      "true,true,17" },

    { "field accessors preserve receivers for reads, methods and length",
      "function probe() { const base={"
      " get value(){return this.id;}, get length(){return this.id+1;},"
      " get method(){return function(){return this.id+2;};}, set missing(v){} };"
      " const child=Object.create(Object.create(base)); child.id=7;"
      " return [child.value,child.length,child.method(),child.missing].join(',');"
      " } probe()",
      "7,8,9," },

    { "field getter remains alive while deleting itself and its prototype",
      "function probe() { let calls=0; const base={get value(){calls++;"
      " delete base.value; Object.setPrototypeOf(this,null); return this.id;}};"
      " const child=Object.create(base); child.id=11;"
      " return [child.value,child.value,calls].join(','); } probe()",
      "11,,1" },

    { "field access keeps proxy get traps and the original thrown value",
      "function probe() { const marker={}, log=[]; const base={"
      " get value(){log.push(this.id);throw marker;}};"
      " const proxy=new Proxy(base,{get(target,key,receiver){log.push(key);"
      " return Reflect.get(target,key,receiver);}});"
      " const child=Object.create(proxy); child.id=9;"
      " try {child.value;} catch(e){log.push(e===marker);}"
      " Object.defineProperty(child,'value',{value:13});"
      " log.push(child.value); return log.join(','); } probe()",
      "value,9,true,13" },

    { "field getters observe replacement, nesting and callable proxies",
      "function probe() { let calls=0; const child={id:5};"
      " Object.defineProperty(child,'value',{configurable:true,get:"
      " new Proxy(function(){return this.id;},{apply(fn,receiver,args){"
      " calls++;return Reflect.apply(fn,receiver,args);}})});"
      " const first=child.value; Object.defineProperty(child,'value',"
      " {get(){return this.other+1;}});"
      " Object.defineProperty(child,'other',{get(){return 20;}});"
      " return [first,child.value,calls].join(','); } probe()",
      "5,21,1" },

    { "ASCII identifier spans retain escape and Unicode continuations",
      "function parseNames() { let alpha=1, alpha\\u0032=2, a\u00e9=3,"
      " a\u200c=4, $under_5=5;"
      " return alpha+alpha2+a\u00e9+a\u200c+$under_5; } parseNames()",
      "15",
      false, NULL, false, true },

    { "private names and escaped leading ASCII keep their spelling",
      "function names() { let \\u0061bc=7;"
      " class Box { #abc=3; #\\u0064ef=5; read(){return this.#abc+this.#def;} }"
      " return abc+new Box().read(); } names()",
      "15",
      false, NULL, false, true },

    { "repeated binding lookups preserve shadowing and scope exits",
      "function outer(a) { var n=2; let x=3;"
      " function read(b) { let y=4; var r=a+n+x+y+b;"
      " { let x=20; r+=a+n+x+y+b; }"
      " r+=a+n+x+y+b; return r; } return read(5); } outer(1)",
      "62" },

    { "repeated lookups preserve ordered with checks and eval bindings",
      "function marker() { return 0; } marker();"
      "function probe() { var x=1, result=0;"
      " with({x:2}) { with({}) { result+=x; result+=x; }"
      " with({x:3}) { result+=x; result+=x; } }"
      " eval('var y=4'); result+=y; result+=y;"
      " return function() { return result+x; }; } probe()()",
      "19",
      false, NULL, false, true },

    { "arguments and function-expression names retain their bindings",
      "var named=function inner(a) {"
      " let n=typeof missingName; n+=typeof missingName;"
      " n+=arguments[0]+arguments[0]; n+=inner===inner;"
      " return (()=>n+':' + arguments[0])(); }; named(3)",
      "undefinedundefined6true:3" },

    { "default-argument scope stays separate from body declarations",
      "var x=7; function defaults(a=x+x, b=()=>x+x) {"
      " var x=30; return a+':' + b()+':' + (x+x); } defaults()",
      "14:14:60" },

    /* chatgpt.com's startup watchdog clears its probe with `_&&=(...)` on a
       captured variable; its first call failed on the PSP with "invalid
       assignment left-hand side". */
    /* chatgpt.com's composer render: a template literal holding a raw
       line break, then an assignment on the next line. */
    { "template literal line break before an assignment",
      "function view() {\n"
      "  let o = {}, ae = 'a\\nb', w = { x: 1 };\n"
      "  return function render() {\n"
      "    { let e = [w.x && ae.includes(`\n`) && 'nl', 'k']; o.j !== e && (o.j = e) }\n"
      "    { let e = `line1\nline2`; o.k !== e && (o.k = e) }\n"
      "    return o.j.join() + '|' + o.k.length;\n"
      "  };\n"
      "}\n"
      "view()()",
      "nl,k|11" },

    { "assignment operators on captured variables",
      "function state() {\n"
      "  let a = 1, b = null, c = 0, d = 5, e, f, o = { x: 3, y: 4 };\n"
      "  function run() {\n"
      "    a &&= (a + 1, 'A'); b ?\?= 'B'; c ||= 'C'; d += 2; d **= 2; d -= 1;\n"
      "    [e, f] = [d, a]; ({ x: e, y: f } = o); e++; --f;\n"
      "    return [a, b, c, d, e, f].join();\n"
      "  }\n"
      "  return run;\n"
      "}\n"
      "state()()",
      "A,B,C,48,4,3" },

    { "closures over parameters, locals and block bindings",
      "function outer(a, b) {\n"
      "  var v = a + 1; let l = b * 2; const c = 'c';\n"
      "  function inner(x) { return [a, b, v, l, c, x].join(); }\n"
      "  { let blockOnly = 'blk'; var viaBlock = function() { return blockOnly + v; }; }\n"
      "  v = 10;\n"
      "  return inner('x') + '|' + viaBlock();\n"
      "}\n"
      "outer(1, 2)",
      "1,2,10,4,c,x|blk10" },

    { "per-iteration loop bindings",
      "var fs = [];\n"
      "for (let i = 0; i < 3; i++) { fs.push(function() { return i * 10; }); }\n"
      "for (var j = 0; j < 2; j++) { fs.push(() => j); }\n"
      "fs.map(f => f()).join()",
      "0,10,20,2,2" },

    { "nested lazy functions reach every level",
      "var g = 'G';\n"
      "function level1(p1) {\n"
      "  var l1 = p1 + 'a';\n"
      "  return function level2(p2) {\n"
      "    var l2 = p2 + 'b';\n"
      "    return function level3(p3) {\n"
      "      return (() => [g, p1, l1, p2, l2, p3].join('.'))();\n"
      "    };\n"
      "  };\n"
      "}\n"
      "level1('1')('2')('3') + ' ' + level1('x')('y')('z')",
      "G.1.1a.2.2b.3 G.x.xa.y.yb.z" },

    { "variables assigned after the closure is created",
      "function counter() {\n"
      "  let n = 0;\n"
      "  const api = { up() { return ++n; }, get value() { return n; },\n"
      "                set value(v) { n = v; } };\n"
      "  return api;\n"
      "}\n"
      "var c = counter(); c.up(); c.up(); c.value = 40; c.up(); c.up();\n"
      "c.value",
      "42" },

    { "this, arguments and new.target in arrows",
      "function Maker(a) {\n"
      "  const read = () => [typeof this, this.tag, arguments.length, arguments[0],\n"
      "                      new.target === Maker].join();\n"
      "  this.tag = 'T';\n"
      "  this.read = read;\n"
      "}\n"
      "var m = new Maker(7, 8);\n"
      "var plain = { tag: 'P', f() { return (() => this.tag + arguments[1])(); } };\n"
      "m.read() + '|' + plain.f(1, 'two')",
      "object,T,2,7,true|Ptwo" },

    { "sloppy this and strict this",
      "function sloppy() { return this === globalThis; }\n"
      "function strict() { 'use strict'; return this === undefined; }\n"
      "function inheritsStrict() { 'use strict'; return (function() { return this; })(); }\n"
      "[sloppy(), strict(), inheritsStrict() === undefined].join()",
      "true,true,true" },

    { "super, home objects and methods",
      "var base = { greet() { return 'base'; } };\n"
      "var derived = { __proto__: base,\n"
      "  greet() { return 'derived>' + super.greet(); },\n"
      "  later() { return (() => super.greet())(); } };\n"
      "class A { hi(x) { return 'A' + x; } static s() { return 'sA'; }\n"
      "          get g() { return 'gA'; } }\n"
      "class B extends A {\n"
      "  constructor(v) { super(); this.v = v; const f = () => this.v; this.f = f; }\n"
      "  hi(x) { return 'B' + super.hi(x) + (() => super.hi(x + 1))(); }\n"
      "  static s() { return 'sB' + super.s(); }\n"
      "  get g() { return 'gB' + super.g; }\n"
      "}\n"
      "var b = new B(9);\n"
      "[derived.greet(), derived.later(), b.hi(1), B.s(), b.g, b.f()].join()",
      "derived>base,base,BA1A2,sBsA,gBgA,9" },

    { "private fields, methods and accessors",
      "class Box {\n"
      "  #v; static #count = 0;\n"
      "  constructor(v) { this.#v = v; Box.#count++; }\n"
      "  #twice() { return this.#v * 2; }\n"
      "  get #label() { return 'box' + this.#v; }\n"
      "  read() { return [this.#v, this.#twice(), this.#label, #v in this].join(); }\n"
      "  later() { return () => this.#twice() + Box.#count; }\n"
      "  static count() { return Box.#count; }\n"
      "}\n"
      "var one = new Box(3); new Box(4);\n"
      "one.read() + '|' + one.later()() + '|' + Box.count()",
      "3,6,box3,true|8|2",
      false, NULL, false, true },

    { "class fields and static blocks",
      "var seen = [];\n"
      "class F {\n"
      "  x = 1; y = () => this.x + 1;\n"
      "  static z = 5;\n"
      "  static { seen.push(() => this.z); }\n"
      "  method() { return this.y() + seen[0](); }\n"
      "}\n"
      "new F().method()",
      "7" },

    { "default, destructured and rest parameters",
      "var k = 100;\n"
      "function params(a, b = a + k, { c, d: [e = 'E'] = [] } = {}, ...rest) {\n"
      "  var inner = () => [a, b, c, e, rest.length].join();\n"
      "  return inner();\n"
      "}\n"
      "function lengths(a, b = 1, c) {}\n"
      "function withClosureDefault(a, f = () => a * 2) { a = 5; return f(); }\n"
      "[params(1), params(1, 2, { c: 3, d: [4] }, 5, 6), lengths.length,\n"
      " withClosureDefault(3)].join('|')",
      "1,101,,E,0|1,2,3,4,2|1|6" },

    { "function names, lengths and name inference",
      "var named = function inner(a, b) {};\n"
      "var anon = function(a) {};\n"
      "var arrow = (x, y, z) => x;\n"
      "var key = 'computed';\n"
      "var obj = { method(p) {}, [key](q, r) {}, get prop() { return 1; } };\n"
      "class C { static s(a) {} m() {} }\n"
      "[named.name, named.length, anon.name, anon.length, arrow.name, arrow.length,\n"
      " obj.method.name, obj.computed.name, obj.computed.length,\n"
      " Object.getOwnPropertyDescriptor(obj, 'prop').get.name, C.s.name, C.prototype.m.name].join()",
      "inner,2,anon,1,arrow,3,method,computed,2,get prop,s,m" },

    { "named function expressions",
      "var fact = function f(n) { return n <= 1 ? 1 : n * f(n - 1); };\n"
      "var sloppyAssign = function g() { g = 1; return typeof g; };\n"
      "var strictAssign = function h() { 'use strict'; try { h = 1; } catch (e) { return e.name; } };\n"
      "[fact(5), sloppyAssign(), strictAssign()].join()",
      "120,function,TypeError" },

    { "assigning a constant from a lazy function",
      "const c = 1;\n"
      "let l = 2;\n"
      "function writeConst() { c = 5; }\n"
      "function writeLet() { l = 3; return l; }\n"
      "function compound() { c += 1; }\n"
      "var r = [];\n"
      "try { writeConst(); } catch (e) { r.push(e.name); }\n"
      "try { compound(); } catch (e) { r.push(e.name); }\n"
      "r.push(writeLet(), c);\n"
      "r.join()",
      "TypeError,TypeError,3,1" },

    { "temporal dead zone through a closure",
      "function early() { return later; }\n"
      "var r;\n"
      "try { early(); } catch (e) { r = e.name; }\n"
      "let later = 'ok';\n"
      "r + ',' + early()",
      "ReferenceError,ok" },

    { "hoisted declarations and block functions",
      "var r = [hoisted()];\n"
      "function hoisted() { return 'h'; }\n"
      "{ function inBlock() { return 'b'; } r.push(inBlock()); }\n"
      "r.push(typeof inBlock);\n"
      "r.join()",
      "h,b,function" },

    { "mapped and unmapped arguments",
      "function mapped(a) { arguments[0] = 'changed'; return a; }\n"
      "function unmapped(a) { 'use strict'; arguments[0] = 'changed'; return a; }\n"
      "function spread() { return Array.prototype.slice.call(arguments).reverse().join(''); }\n"
      "[mapped('orig'), unmapped('orig'), spread(1, 2, 3)].join()",
      "changed,orig,321" },

    { "generators and async functions",
      "function* gen(n) { for (let i = 0; i < n; i++) yield () => i; }\n"
      "var out = [];\n"
      "for (const f of gen(3)) out.push(f());\n"
      "async function later(v) { await null; return v + 1; }\n"
      "var asyncArrow = async x => (await later(x)) * 2;\n"
      "async function* agen() { yield 'a'; yield await later(1); }\n"
      "var o = { async m() { return 'm'; }, *g() { yield 'g'; } };\n"
      "(async () => {\n"
      "  const r = [out.join(''), await later(1), await asyncArrow(2), await o.m(), o.g().next().value];\n"
      "  for await (const v of agen()) r.push(v);\n"
      "  globalThis.asyncResult = r.join();\n"
      "})();\n"
      "'started'",
      "started" },

    { "labels, loops, switch, try and finally",
      "function control(n) {\n"
      "  var log = [];\n"
      "  outer: for (var i = 0; i < n; i++) {\n"
      "    for (var j = 0; j < n; j++) {\n"
      "      if (j === 2) continue outer;\n"
      "      if (i === 3) break outer;\n"
      "      switch (j) { case 0: log.push('z'); break; default: log.push(j); }\n"
      "    }\n"
      "  }\n"
      "  try { throw new Error('boom'); } catch ({ message }) { log.push(message); }\n"
      "  finally { log.push('fin'); }\n"
      "  return log.join('');\n"
      "}\n"
      "control(5)",
      "z1z1z1boomfin" },

    { "templates, tagged templates and regular expressions",
      "function tag(strings, ...values) { return strings.raw.join('_') + values.join(); }\n"
      "function build(x) { return tag`a${x}b\\n${x + 1}c` + `|${x}|`; }\n"
      "function sameSite() { return (s => s)`x`; }\n"
      "function words(s) { return s.match(/\\w+/g).length + (/b/.test(s) ? 'b' : ''); }\n"
      "[build(1), sameSite() === sameSite(), words('a b c')].join(' ')",
      "a_b\\n_c1,2|1| true 3b" },

    { "closures created from one stub before its first call",
      "function make(v) { return function read() { return v; }; }\n"
      "var fs = [make(1), make(2), make(3)];\n"
      "fs[2]() + fs[0]() * 10 + fs[1]() * 100",
      "213" },

    { "recursion and mutual recursion",
      "function even(n) { return n === 0 ? true : odd(n - 1); }\n"
      "function odd(n) { return n === 0 ? false : even(n - 1); }\n"
      "function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }\n"
      "[even(10), odd(7), fib(15)].join()",
      "true,true,610" },

    { "arrow functions with expression bodies inside lists",
      "var ops = [x => x + 1, (x, y = 2) => x * y, async x => x, x => ({ x }), x => y => x - y];\n"
      "[ops[0](1), ops[1](3), typeof ops[2](0).then, ops[3](4).x, ops[4](9)(1)].join()",
      "2,6,function,4,8",
      false, NULL, false, true },

    { "direct eval and with keep functions eager and correct",
      "function viaEval(a) { var local = 'L'; return eval('local + a'); }\n"
      "function aroundEval(a) { var local = 'L2'; var f = function() { return local + a; }; eval(''); return f(); }\n"
      "function viaWith(o) { with (o) { return (function() { return p; })(); } }\n"
      "var p = 'global-p';\n"
      "[viaEval(1), aroundEval(2), viaWith({ p: 'with-p' }), viaWith({})].join()",
      "L1,L22,with-p,global-p",
      false, NULL, false, true },

    { "Function.prototype.toString keeps the text",
      "function f(a, b) { return a /* keep */ + b; }\n"
      "var arrow = (x) => x * 2;\n"
      "var o = { m(y) { return y; }, get g() { return 1; } };\n"
      "class K { static s() { return 's'; } }\n"
      "var before = [f, arrow, o.m, K.s].map(String);\n"
      "f(1, 2); arrow(1); o.m(1); K.s();\n"
      "var after = [f, arrow, o.m, K.s].map(String);\n"
      "before.join('#') + (before.join('#') === after.join('#') ? ' same' : ' differs')",
      "function f(a, b) { return a /* keep */ + b; }#(x) => x * 2#m(y) { return y; }"
      "#s() { return 's'; } same" },

    { "error positions from lazily compiled functions",
      "function first() {\n"
      "  return second();\n"
      "}\n"
      "  function second() {\n"
      "    var x = 1; x = x.missing.deeper;\n"
      "  }\n"
      "try { first(); } catch (e) { e.stack.split('\\n').slice(0, 3).map(s => s.trim()).join(' / '); }",
      "at second (case.js:5:29) / at first (case.js:2:3) / at <eval> (case.js:7:12)" },

    { "positions on long minified lines",
      "var a=1;function one(){return 1}function two(){return two.caller}function three(){null.x}\n"
      "var z=1;try{three()}catch(e){e.stack.split('\\n')[0].trim()}",
      "at three (case.js:1:87)" },

    { "positions after multi-line code preceding a function",
      "var s = `line1\n"
      "line2`; function after() {\n"
      "  var q = 1;\n"
      "  q.w.e;\n"
      "}\n"
      "try { after(); } catch (e) { e.stack.split('\\n')[0].trim() }",
      "at after (case.js:4:6)" },

    { "non-ASCII text",
      "function \u00e9t\u00e9(\u00e0) { return '\u00e7a:' + \u00e0 + '\u2603'; }\n"
      "function thrower() { var m = '\u00fc\u00fc'; null.m; }\n"
      "var pos; try { thrower(); } catch (e) { pos = e.stack.split('\\n')[0].trim(); }\n"
      "\u00e9t\u00e9('\u00df') + ' ' + String(\u00e9t\u00e9).length + ' ' + pos",
      "\u00e7a:\u00df\u2603 43 at thrower (case.js:2:40)" },

    { "global declarations made after compile",
      "function readLater() { return typeof declaredLater + (typeof lateGlobal); }\n"
      "var r = readLater();\n"
      "var declaredLater = 1;\n"
      "globalThis.lateGlobal = 2;\n"
      "r + '|' + readLater()",
      "undefinedundefined|numbernumber" },

    { "super() from an arrow in a derived constructor",
      "class A { constructor(x) { this.a = x; } }\n"
      "class B extends A {\n"
      "  constructor(x) { const init = () => super(x * 2); init(); this.b = new.target === B; }\n"
      "}\n"
      "class C extends A { constructor() { const early = () => this; try { early(); } catch (e) { super(1); this.e = e.name; } } }\n"
      "var b = new B(4), c = new C();\n"
      "[b.a, b.b, c.a, c.e].join()",
      "8,true,1,ReferenceError",
      false, NULL, false, true },

    { "class name bindings seen from methods",
      "class K { self() { return K; } rename() { try { K = null; } catch (e) { return e.name; } } }\n"
      "var Expr = class Inner { who() { return Inner.name; } };\n"
      "var k = new K();\n"
      "[k.self() === K, k.rename(), new Expr().who()].join()",
      "true,TypeError,Inner" },

    { "catch parameters, switch cases and sloppy block functions",
      "function scopes(v) {\n"
      "  var out = [];\n"
      "  try { throw v; } catch (err) { out.push(function() { return 'caught ' + err; }); }\n"
      "  switch (v) { case 1: let inCase = 'case'; out.push(() => inCase); break; }\n"
      "  if (true) { function blockFn() { return 'block ' + v; } }\n"
      "  out.push(blockFn);\n"
      "  return out.map(f => f()).join('|');\n"
      "}\n"
      "scopes(1)",
      "caught 1|case|block 1" },

    { "with and arguments inside a lazy function",
      "function viaWithInside(o) { with (o) { return (function() { return p + q; })(); } }\n"
      "var q = '-global-q';\n"
      "function argsFromInner() { return function() { return (() => arguments[0] + arguments.length)(); }('a', 'b'); }\n"
      "[viaWithInside({ p: 'with-p' }), argsFromInner()].join()",
      "with-p-global-q,a2" },

    { "identifiers that are keywords elsewhere",
      "var f1 = await => await + 1;\n"
      "var f2 = (async) => async * 2;\n"
      "var f3 = (of, get, set, let_) => [of, get, set, let_].join('');\n"
      "function notGen() { var yield = 3; return yield; }\n"
      "var o = { get: function get() { return 'g'; }, static(x) { return x; } };\n"
      "[f1(1), f2(2), f3('o', 'g', 's', 'l'), notGen(), o.get(), o.static(5)].join()",
      "2,4,ogsl,3,g,5" },

    { "regular expressions, divisions and escapes at function starts",
      "var t = x => /a\\/b/.test(x) ? x.length / 2 : -1;\n"
      "function \\u0065scaped(\\u0061) { return a + '\\u00e9'; }\n"
      "var div = (a, b) => a /b/ 1;\n"
      "[t('xa/b'), escaped('x'), div(8, 2)].join()",
      "2,xé,4" },

    { "static blocks, async arrows and class expressions in functions",
      "function factory(base) {\n"
      "  return class extends base {\n"
      "    #secret = 's';\n"
      "    static { this.made = (async () => await 'made')(); }\n"
      "    reveal() { return () => this.#secret + super.name(); }\n"
      "    static tag() { return 'tag'; }\n"
      "  };\n"
      "}\n"
      "class Base { name() { return 'base'; } }\n"
      "var K = factory(Base);\n"
      "new K().reveal()() + K.tag()",
      "sbasetag" },

    { "deep nesting",
      "function build(n) { var s = ''; for (var i = 0; i < n; i++) s += 'function(){ var d' + (i + 1) + ' = ' + (i + 1) + '; return '; s += '[d1, d' + n + '].join()'; for (var j = 0; j < n; j++) s += '}()'; return s; }\n"
      "var deep = (0, eval)('(function(){ return ' + build(40) + '; })');\n"
      "function nested(a){return function(b){return function(c){return function(d){return function(e){return [a,b,c,d,e].join('')}}}}}\n"
      "deep() + ' ' + nested(1)(2)(3)(4)(5)",
      "1,40 12345" },

    { "long functions keep exact text",
      "var long = 'function longFn() { var s = \"' + 'x'.repeat(3000) + '\"; return s.length; }';\n"
      "function holder() { return 'h'; }\n"
      "var f = (0, eval)('(' + long + ')');\n"
      "function longLiteral() { return 'yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy'.length + (() => 1)(); }\n"
      "var textBefore = String(longLiteral);\n"
      "[f(), String(f) === long, String(holder).length, longLiteral(), textBefore === String(longLiteral), textBefore.length].join()",
      "3000,true,33,848,true,905" },

    { "line numbers across templates and comments",
      "var t = `a\n"
      "b`;\n"
      "/* comment\n"
      "   lines */ function after(\n"
      "  a,\n"
      "  b) {\n"
      "  // body\n"
      "  return a.b.c;\n"
      "}\n"
      "try { after(1, 2); } catch (e) { e.stack.split('\\n')[0].trim() + ' ' + after.lineNumber + ':' + after.columnNumber }",
      "at after (case.js:8:13) 4:13" },

    { "first calls at the native stack limit",
      /* compiling leaf() needs far more native stack than calling it */
      "function leaf(v) { return ((((((((((((((((((((((((v + 1)))))))))))))))))))))))) * 2; }\n"
      "function rec(n) { return n === 0 ? 0 : 1 + rec(n - 1); }\n"
      "var max = 0;\n"
      "for (var d = 64; d < 1000000; d *= 2) { try { rec(d); max = d; } catch (e) { break; } }\n"
      "for (var step = max; step >= 1; step >>= 1) { try { rec(max + step); max += step; } catch (e) {} }\n"
      "function deep(n) { return n === 0 ? leaf(1) : deep(n - 1); }\n"
      "var names = {}, ok = false;\n"
      "for (var k = max * 2; k >= 0 && !ok; k -= (k > max ? max >> 3 : 1)) {\n"
      "  try { deep(k); ok = true; } catch (e) { names[e.name + ':' + e.message] = 1; }\n"
      "}\n"
      "var kinds = Object.keys(names);\n"
      "(kinds.every(function (n) { return n === 'InternalError:stack overflow'; }) ? 'clean' : kinds.join('|'))\n"
      "  + (ok ? ' ok' : ' never')",
      "clean ok", false, NULL, true },

    { "module bindings, imports and import.meta",
      "import def, { counter, bump, table } from 'lib';\n"
      "export function useImports() { return [counter, bump(), counter, table.twice(4), def()].join(); }\n"
      "const where = () => typeof import.meta + (() => typeof import.meta)();\n"
      "let local = 'x';\n"
      "function setLocal() { local = 'y'; }\n"
      "async function later() { return (await import('lib')).counter; }\n"
      "setLocal();\n"
      "later().then(v => { globalThis.result = [useImports(), local, where(), v].join('|'); });",
      "0,1,1,8,default:1|y|objectobject|0", true, library_modules },

    { "strict mode inherited from a module",
      "function strictThis() { return this === undefined; }\n"
      "function assignUndeclared() { try { undeclaredName = 1; return 'no'; } catch (e) { return e.name; } }\n"
      "globalThis.result = [strictThis(), assignUndeclared()].join();",
      "true,ReferenceError", true, NULL },

    /* ---- preparsing: what the scan must get right in a skipped body.
       Every body below is only scanned in the preparsed run; its first
       call parses it and must see the same variables. Outer bindings share
       their names with the body's own declarations, as minified code's
       do, so a scan that drops the wrong name or keeps a wrong scope reads
       another binding (or fails its first compile). */

    { "scan: strings, comments and templates holding brackets and quotes",
      "var e = 'outer-e', t = 'outer-t';\n"
      "function f(n) {\n"
      "  var a = '}{)(][', b = \"\\\"}\\\\\", c = `${'}'}${`in${n}`}}`; // } ' \"\n"
      "  /* } { ` ' */ var d = `x${ {k: `}`}.k }y${ ((q) => q + '`')(e) }`;\n"
      "  return [a, b, c, d, e].join('|');\n"
      "}\n"
      "function g() { return t; }\n"
      "f(1) + '|' + g()",
      "}{)(][|\"}\\|}in1}|x}youter-e`|outer-e|outer-t",
      false, NULL, false, false, true },

    { "scan: regular expressions and divisions",
      "var e = 6, t = 2, n = 3;\n"
      "function f(x) {\n"
      "  var r = [];\n"
      "  if (x) /}/.test('}') && r.push('if');\n"
      "  { } /[/{]/.test('{') && r.push('block');\n"
      "  r.push(e / t / n, x.length / 2 / 1, (e) / t, [e][0] / t);\n"
      "  r.push(typeof /x/, (() => /a\\/b/)().source, 'a/b'.split(/\\//).length);\n"
      "  var o = { v: 8 }; r.push(o.v / 2, e++ / 2, --e / t);\n"
      "  return r.join(',');\n"
      "}\n"
      "f('ab')",
      "if,block,1,1,3,3,object,a\\/b,2,4,3,3",
      false, NULL, false, false, true },

    { "scan: names a skipped body declares at every level",
      "var e = 'E', t = 'T', n = 'N', r = 'R', i = 'I', a = 'A', o = 'O';\n"
      "function f(p) {\n"
      "  var out = [e];\n"
      "  let [t, { n: [r] = ['r'], ...i }] = ['t', { x: 1 }];\n"
      "  for (let a = 0; a < 1; a++) out.push(a, o);\n"
      "  for (const o of ['o']) out.push(o);\n"
      "  try { throw 'c'; } catch ({ length: a }) { out.push(a); }\n"
      "  { let e = 'inner'; out.push(e); }\n"
      "  function n2() { return typeof n; }\n"
      "  class K { m() { return typeof K; } }\n"
      "  out.push(t, r, Object.keys(i).join(''), n2(), new K().m(), a, e);\n"
      "  return out.join(',');\n"
      "}\n"
      "f(0)",
      "E,0,O,o,1,inner,t,r,x,string,function,A,E",
      false, NULL, false, false, true },

    { "scan: a block's names do not hide the outer ones around it",
      "var x = 'outer', y = 'outer-y';\n"
      "function f() {\n"
      "  var r = [x];\n"
      "  for (let i = 0; i < 1; i++) { let x = 'block'; r.push(x); }\n"
      "  if (true) { const y = 'block-y'; r.push(y); }\n"
      "  r.push(x, y, (() => { let x = 'arrow'; return x; })(), x);\n"
      "  return r.join(',');\n"
      "}\n"
      "f()",
      "outer,block,block-y,outer,outer-y,arrow,outer",
      false, NULL, false, false, true },

    { "scan: this, arguments, new.target and super through nested functions",
      "function Outer() {\n"
      "  const read = () => {\n"
      "    const inner = function () { return this === undefined ? 'u' : typeof this; };\n"
      "    const nested = () => [typeof this, arguments.length, new.target === Outer].join();\n"
      "    return nested() + '|' + inner.call(7) + '|' + (function () { return arguments.length; })(1, 2, 3);\n"
      "  };\n"
      "  this.r = read();\n"
      "}\n"
      "var base = { who() { return 'base'; } };\n"
      "var obj = { __proto__: base, who() { const f = () => { const g = () => super.who(); return g(); }; return 'obj>' + f(); } };\n"
      "new Outer(1, 2).r + '|' + obj.who()",
      "object,2,true|object|3|obj>base",
      false, NULL, false, false, true },

    { "scan: arguments of the enclosing function from arrows only",
      "function outer(a, b) {\n"
      "  var viaArrow = () => { return arguments[1]; };\n"
      "  var own = function () { return arguments.length; };\n"
      "  return viaArrow() + ':' + own(1, 2, 3, 4);\n"
      "}\n"
      "outer('x', 'y')",
      "y:4",
      false, NULL, false, false, true },

    { "scan: classes, methods, accessors, fields and computed keys",
      "var k = 'dyn', e = 'E', t = 'T';\n"
      "function f() {\n"
      "  class A {\n"
      "    static s = e; x = t;\n"
      "    [k + 1]() { return this.x + k; }\n"
      "    get g() { return e; } set g(v) { this.x = v; }\n"
      "    static { this.t = typeof t; }\n"
      "    async am() { return 'am'; } *gen() { yield e; }\n"
      "  }\n"
      "  const a = new A(); a.g = 'set';\n"
      "  const o = { e, t, [k]: 1, get v() { return t; }, m(e) { return e; }, 'q'(n) { return n; } };\n"
      "  return [a.dyn1(), a.g, A.s, A.t, [...a.gen()][0], o.e, o.dyn, o.v, o.m(3), o.q(4)].join();\n"
      "}\n"
      "f()",
      "setdyn,E,E,string,E,E,1,T,3,4",
      false, NULL, false, false, true },

    { "scan: default and destructured parameters of nested functions",
      "var e = 'E', t = 'T', n = 'N';\n"
      "function f() {\n"
      "  const g = (e = t, { t: [n] = [e] } = {}) => [e, n].join('');\n"
      "  function h({ a: e = n, ...t }, [n2 = e] = []) { return e + Object.keys(t).length + n2; }\n"
      "  const k = async function ({ e }) { return e; };\n"
      "  return [g(), g('x', { t: ['y'] }), h({ b: 1 }), typeof k({})].join();\n"
      "}\n"
      "f()",
      "TT,xy,N1N,object",
      false, NULL, false, false, true },

    { "scan: await and yield before regular expressions",
      "function* gen() { yield /a}b/.source; yield /c/g.flags; }\n"
      "async function asy() { return await /d}/.source; }\n"
      "function sloppy() { var yield = 8, await = 2; return yield / await / 2; }\n"
      "var r = [...gen()].join() + ',' + sloppy();\n"
      "asy().then(v => { globalThis.asyncResult = r + ',' + v; });\n"
      "'started'",
      "started",
      /* (the preparser gives up on yield and await used as names) */
      false, NULL, false, false, false },

    { "scan: a 'use strict' directive in a skipped body",
      "function strictBody() { 'use strict'; var r = [];\n"
      "  try { undeclaredStrict = 1; r.push('no'); } catch (e) { r.push(e.name); }\n"
      "  r.push((function () { return this; })() === undefined); return r.join(); }\n"
      "function sloppyBody() { var r = []; undeclaredSloppy = 2; r.push(typeof undeclaredSloppy);\n"
      "  r.push((function () { return this; })() === globalThis); return r.join(); }\n"
      "strictBody() + '|' + sloppyBody()",
      "ReferenceError,true|number,true",
      false, NULL, false, false, true },

    { "scan: temporal dead zone and late declarations around a skipped body",
      "function f() {\n"
      "  const read = () => { return later + (typeof hoisted) + typeof declaredAfter; };\n"
      "  let r;\n"
      "  try { read(); } catch (e) { r = e.name; }\n"
      "  let later = 'L';\n"
      "  function hoisted() {}\n"
      "  var declaredAfter = 1;\n"
      "  return r + ',' + read();\n"
      "}\n"
      "f()",
      "ReferenceError,Lfunctionnumber",
      false, NULL, false, false, true },

    { "scan: automatic semicolons, labels and keywords as names",
      "var a = 'A', b = 'B', c = 'C', g = { lastIndex: 4 };\n"
      "function f() {\n"
      "  let x = a\n"
      "  let y = b\n"
      "  ;(c)\n"
      "  var o = { if: 1, class: 2, new: 3, let: 4, of: 5, async: 6, get: 7, static: 8 }\n"
      "  var s = o.if + o.class + o.new + o.let + o.of + o.async + o.get + o.static\n"
      "  outer: for (var i = 0; i < 3; i++) { for (;;) { if (i === 1) continue outer; break outer; } }\n"
      "  var q = x\n"
      "  /b/g.lastIndex\n"
      "  return [x, y, s, i, q].join()\n"
      "}\n"
      "f()",
      "A,B,36,0,NaN",
      false, NULL, false, false, true },

    { "scan: identifiers that are keywords only sometimes",
      "var of = 'of-var', async = x => 'async-fn:' + x;\n"
      "function f() {\n"
      "  var r = [of, async(1)];\n"
      "  for (var v of [of]) r.push(v);\n"
      "  var async2 = async (q) => q; r.push(typeof async2(1).then);\n"
      "  var get = 'get', set = 'set'; r.push(get + set);\n"
      "  return r.join();\n"
      "}\n"
      "f()",
      "of-var,async-fn:1,of-var,function,getset",
      false, NULL, false, false, false },

    { "scan: a for head's names, then its body's",
      "var x = 3, i = 'outer-i';\n"
      "function f() {\n"
      "  var r = [];\n"
      "  for (let i = 0; i < x; i++) { let x = 1; r.push(i + x); }\n"
      "  for (const k in { a: 1 }) { const i = k; r.push(i); }\n"
      "  r.push(i);\n"
      "  return r.join();\n"
      "}\n"
      "f()",
      "1,2,3,a,outer-i", false, NULL, false, false, true },

    { "scan: an arrow's expression body ends where its expression does",
      "var e = 'E', t = 'T', q = 'Q', w = 'W', s = 'S';\n"
      "function f() {\n"
      "  var list = [q => q + 1, q], g = t => t, h = t;\n"
      "  var c = true ? w => w : w, d = { k: s => s, v: s };\n"
      "  return [list[1], g('g'), h, typeof c, d.v, `${(e => e)(1)}${e}`].join();\n"
      "}\n"
      "f()",
      "Q,g,T,function,S,1E", false, NULL, false, false, true },

    { "scan: a name used only as a shorthand property",
      "var only = 'shorthand', other = 1;\n"
      "function f() { return { only, other: 2 }; }\n"
      "f().only + f().other",
      "shorthand2", false, NULL, false, false, true },

    { "scan: eval in a skipped body and eval after one",
      "var z = 'global-z';\n"
      "function withEval() { var z = 'local-z'; return (function () { return eval('z'); })(); }\n"
      "function evalAfter() {\n"
      "  var w = 'local-w';\n"
      "  var inner = function () { return typeof w + ':' + w; };\n"
      "  eval('w = \"changed\"');\n"
      "  return inner();\n"
      "}\n"
      "function plain() { return z; }\n"
      "withEval() + '|' + evalAfter() + '|' + plain()",
      "local-z|string:changed|global-z", false, NULL, false, true },

    { "scan: bodies the scan gives up on still compile",
      "var e = 'E';\n"
      "function priv() { class P { #x = e; get() { return this.#x; } } return new P().get(); }\n"
      "function uni() { var é = e; return é; }\n"
      "function esc() { var \\u0061b = e; return ab; }\n"
      "function nbsp() { return e; }\n"
      "[priv(), uni(), esc(), nbsp()].join()",
      "E,E,E,E" },

    { "await in module functions and top level",
      "async function twice(v) { return v * 2; }\n"
      "const value = await twice(21);\n"
      "globalThis.result = String(value) + (() => value)();",
      "4242", true, NULL },
};

/* Compile the case's script without running it, then compile every
   deferred body it has (JS_CompileLazyFunctions): bodies that the run does
   not call must compile too. */
static char *compile_every_body(const LazyCase *test, uint64_t *compiled)
{
    char buffer[160];
    Realm realm;
    char *failure = NULL;
    *compiled = 0;
    if (!realm_open_preparse(&realm, 1u, true)) return strdup("realm open failed");
    realm.modules = test->modules;
    JSValue value = JS_Eval(realm.ctx, test->source, strlen(test->source),
                            test->module ? "case.mjs" : "case.js",
                            (test->module ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL)
                            | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(value)) {
        failure = exception_text(realm.ctx);
    } else {
        JSLazyFunctionStats loaded = realm_stats(&realm);
        if (test->all_preparsed && loaded.preparsed != loaded.deferred) {
            snprintf(buffer, sizeof(buffer), "%llu of %llu deferred functions scanned",
                     (unsigned long long) loaded.preparsed,
                     (unsigned long long) loaded.deferred);
            failure = strdup(buffer);
        }
        if (JS_CompileLazyFunctions(realm.ctx, value, compiled) < 0 && failure == NULL)
            failure = exception_text(realm.ctx);
        JS_FreeValue(realm.ctx, value);
    }
    JSLazyFunctionStats stats = realm_stats(&realm);
    if (failure == NULL
        && (stats.compile_failures != 0
            || (stats.preparse_restarts != 0 && !test->nothing_to_preparse)))
        failure = strdup("a compile failed or was repeated without preparsing");
    if (!realm_close(&realm) && failure == NULL)
        failure = strdup("realm did not return every byte");
    return failure;
}

enum { MODE_EAGER, MODE_PARSE, MODE_PREPARSE, MODE_COUNT };
static const char *const mode_names[MODE_COUNT] = { "eager", "lazy", "preparsed" };

static int run_case(const LazyCase *test)
{
    char *results[MODE_COUNT] = { NULL, NULL, NULL };
    JSLazyFunctionStats stats[MODE_COUNT];
    for (int mode = 0; mode < MODE_COUNT; mode++) {
        Realm realm;
        if (!realm_open_preparse(&realm, mode == MODE_EAGER ? 0u : 1u,
                                 mode == MODE_PREPARSE)) {
            fprintf(stderr, "%s: realm open failed\n", test->name);
            return 1;
        }
        realm.modules = test->modules;
        results[mode] = test->module
            ? realm_run_module(&realm, test->source)
            : realm_run_script(&realm, test->source);
        if (results[mode] != NULL && strcmp(results[mode], "started") == 0) {
            JSValue global = JS_GetGlobalObject(realm.ctx);
            JSValue async = JS_GetPropertyStr(realm.ctx, global, "asyncResult");
            JS_FreeValue(realm.ctx, global);
            char *text = value_text(realm.ctx, async);
            char *joined = malloc(strlen(text) + 16);
            sprintf(joined, "started %s", text);
            free(text);
            free(results[mode]);
            results[mode] = joined;
        }
        stats[mode] = realm_stats(&realm);
        if (!realm_close(&realm)) {
            fprintf(stderr, "%s: realm did not return every byte (%s)\n",
                    test->name, mode_names[mode]);
            return 1;
        }
    }
    int failed = 0;
    const char *expected = test->expected;
    if (strcmp(expected, "started") == 0) {
        /* compare lazy with eager only; the eager run is the oracle */
        expected = results[MODE_EAGER];
    }
    for (int mode = 0; mode < MODE_COUNT; mode++) {
        if (results[mode] == NULL || expected == NULL
            || strcmp(results[mode], expected) != 0) {
            fprintf(stderr, "%s:\n  expected  %s\n  eager     %s\n  lazy      %s\n"
                            "  preparsed %s\n", test->name, test->expected,
                    results[MODE_EAGER] ? results[MODE_EAGER] : "(null)",
                    results[MODE_PARSE] ? results[MODE_PARSE] : "(null)",
                    results[MODE_PREPARSE] ? results[MODE_PREPARSE] : "(null)");
            failed = 1;
            break;
        }
    }
    const JSLazyFunctionStats *lazy = &stats[MODE_PARSE], *pre = &stats[MODE_PREPARSE];
    if (stats[MODE_EAGER].deferred != 0 || lazy->deferred == 0
        || (lazy->compile_failures != 0 && !test->compile_failures_allowed)
        || lazy->preparsed != 0) {
        fprintf(stderr, "%s: eager deferred %llu, lazy deferred %llu "
                        "failures %llu preparsed %llu\n", test->name,
                (unsigned long long) stats[MODE_EAGER].deferred,
                (unsigned long long) lazy->deferred,
                (unsigned long long) lazy->compile_failures,
                (unsigned long long) lazy->preparsed);
        failed = 1;
    }
    /* preparsing defers and compiles the same functions */
    if (pre->deferred != lazy->deferred
        || pre->deferred_source_bytes != lazy->deferred_source_bytes
        || pre->compiled != lazy->compiled
        || pre->compiled_source_bytes != lazy->compiled_source_bytes
        || pre->compile_failures != lazy->compile_failures
        || (pre->preparsed == 0) != test->nothing_to_preparse
        || pre->preparsed > pre->deferred
        /* only a direct eval after a scanned body compiles a case again */
        || (pre->preparse_restarts != 0 && !test->nothing_to_preparse)) {
        fprintf(stderr, "%s: lazy deferred %llu (%llu bytes) compiled %llu (%llu bytes)"
                        " failures %llu; preparsed run deferred %llu (%llu bytes)"
                        " compiled %llu (%llu bytes) failures %llu, preparsed %llu"
                        " restarts %llu\n",
                test->name,
                (unsigned long long) lazy->deferred,
                (unsigned long long) lazy->deferred_source_bytes,
                (unsigned long long) lazy->compiled,
                (unsigned long long) lazy->compiled_source_bytes,
                (unsigned long long) lazy->compile_failures,
                (unsigned long long) pre->deferred,
                (unsigned long long) pre->deferred_source_bytes,
                (unsigned long long) pre->compiled,
                (unsigned long long) pre->compiled_source_bytes,
                (unsigned long long) pre->compile_failures,
                (unsigned long long) pre->preparsed,
                (unsigned long long) pre->preparse_restarts);
        failed = 1;
    }
    uint64_t every = 0;
    char *failure = compile_every_body(test, &every);
    if (failure != NULL) {
        fprintf(stderr, "%s: compiling every deferred body: %s\n", test->name, failure);
        free(failure);
        failed = 1;
    }
    for (int mode = 0; mode < MODE_COUNT; mode++) free(results[mode]);
    return failed;
}

/* ------------------------------------------------------- early errors */

/* Early errors reject the whole script when it compiles, in every mode:
   eagerly, lazily and with preparsing, whether or not the function holding
   the error is ever called. (The preparser gives up on a body it cannot
   vouch for and the parser reports the error itself.) The first 32 are the
   cases the scanning preparser left to the first call, or caught itself. */
static const char *const early_errors[] = {
    "function f() { let a; let a; }",
    "function f() { return 1 +; }",
    "function f() { 'use strict'; with ({}) {} }",
    "function f() { break; }",
    "function f() { continue nowhere; }",
    "class A { m() { return this.#undeclared; } }",
    "function* g() { var yield = 1; }",
    "async function f() { var await = 1; }",
    "function f() { 'use strict'; var eval = 1; }",
    "function f(a = 1) { 'use strict'; }",
    "var o = { m() { super(); } };",
    "function f() { new.target = 1; }",
    "var f = () => { return 08n; };",
    "function f() { x => { let x; var x; }; }",
    "function outer() { function inner() { return import.meta; } }",
    "function f() { 'use strict'; return 010; }",
    "function f() { for (let i, j of []) {} }",
    "function f() { label: label: ; }",
    "function f(a, a) { 'use strict'; }",
    "var f = (a, a) => 1;",
    "function f() { var br\\u0065ak = 1; }",
    "function f() { var alpha\\u002d = 1; }",
    "function f() { var alpha\\x32 = 1; }",
    "function f() { return (1; }",
    "function f() { return [1); }",
    "function f() { return 'open; }",
    "function f() { return `open${1}; }",
    "function f() { return /open; }",
    "function f() { /* open }",
    "function f() { 'use strict'; return '\\07'; }",
    "function f() { return 10in []; }",
    "function f() { return 1; } var = 2;",
    /* more that the validating preparser checks itself */
    "function f() { return a || b ?? c; }",
    "function f() { return (a, b) = c; }",
    "function f() { a?.b = 1; }",
    "function f() { ++a++; }",
    "function f() { return /(?<n>a)(?<n>b)/; }",
    "function f() { return /a/gg; }",
    "function f() { return '\\u{110000}'; }",
    "function f() { return `\\xZZ`; }",
    "function f() { return { __proto__: 1, __proto__: 2 }; }",
    "function f() { return { a = 1 }; }",
    "function f() { const c; }",
    "function f() { for (let x = 1 of y) ; }",
    "function f() { let x; { var x; } }",
    "function f(x) { let x; }",
    "function f() { try {} catch (e) { let e; } }",
    "function f() { return { get a(b) {} }; }",
    "function f() { return class { constructor() {} constructor() {} }; }",
    "function f() { return class { static prototype() {} }; }",
    "function f() { return class { static { return; } }; }",
    "function f() { return class extends B { m() { super(); } }; }",
    "function f() { return function () { super.x; }; }",
    "function f() { yield 1; }",
    "async function f() { return () => { await 1; }; }",
    "function f() { for await (const x of y) ; }",
    "function f() { throw\nnew Error(); }",
    "function f() { return async a\n=> a; }",
    "function f() { if (a) let b = 1; }",
    "function f() { switch (a) { default: case 1: default: } }",
    "function f() { a: { continue a; } }",
    "function f() { 'use strict'; delete x; }",
    "function f() { 'use strict'; arguments = 1; }",
    "function f() { return class { x = arguments; }; }",
    "function f() { return [...a, b] = c; }",
    "function f() { return [...a = 1] = c; }",
    "function f() { return a\n=> a; }",
    "function f() { return 1.toString(); }",
    "function f() { return 0b12; }",
    /* the parser's look-ahead over the call leaves the literal's error
       pending when the inner function is preparsed */
    "function f() { (function () { var s = '\\u{110000}'; })(); }",
    "function f() { (function () { return `\\08`; })(); }",
    "function f() { 'use strict'; (function () { return `\\08`; } 010)(); }",
    /* checks the preparser can only reach in strict or module code, or
       at the top level */
    "function f() { 'use strict'; yield 1; }",
    "var f = () => { new.target; };",
    "function f() { 'use strict'; return function (a, a) {}; }",
    "function f() { 'use strict'; return function (a = 1) { 'use strict'; }; }",
    "function f() { var a = 1 var b = 2; }",
    "function f() { var [...[a] = b] = c; }",
    "function f() { { { var x; } let x; } }",
    "module: export function f() { await 1; }",
    "module: export function f() { return () => { await 1; }; }",
};

static int early_error_case(const char *source, int mode, char **ptext)
{
    Realm realm;
    int failed = 0;
    if (!realm_open_preparse(&realm, mode == 0 ? 0u : 1u, mode == 2)) return 1;
    bool module = strncmp(source, "module: ", 8) == 0;
    if (module) source += 8;
    JSValue compiled = JS_Eval(realm.ctx, source, strlen(source), "early.js",
                               (module ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL)
                               | JS_EVAL_FLAG_COMPILE_ONLY);
    char *text = NULL;
    bool at_compile = JS_IsException(compiled);
    if (at_compile) {
        text = exception_text(realm.ctx);
    } else {
        if (JS_CompileLazyFunctions(realm.ctx, compiled, NULL) < 0)
            text = exception_text(realm.ctx);
        JS_FreeValue(realm.ctx, compiled);
    }
    if (text == NULL || strncmp(text, "EXC SyntaxError", 15) != 0 || !at_compile) {
        fprintf(stderr, "early error (%s): %s -> %s (%s)\n",
                mode == 0 ? "eager" : mode == 1 ? "lazy" : "preparsed",
                source, text ? text : "no error",
                at_compile ? "at compile" : "not at compile");
        failed = 1;
    }
    *ptext = text;
    if (!realm_close(&realm)) failed = 1;
    return failed;
}

/* An invalid function that is never called still rejects its script: the
   script's first statement must not run. (With the scanning preparser the
   body was only checked by its first call, so the script ran.) */
static int run_invalid_unused_function(void)
{
    static const char *const sources[] = {
        "globalThis.ran = 1;\n"
        "function unused(a, b) { var s = a + b; for (let i = 0; i < a; i++) s += i;"
        " let t = s; let t = 2; return t; }\n",
        "globalThis.ran = 1;\n"
        "var o = { m(list) { return list.map(x => x * 2).filter(y => y > 1)"
        ".reduce((p, q) => p + q, 0) + (a || b ?? c); } };\n",
        "globalThis.ran = 1;\n"
        "class K { m(v) { if (v) { return [v, v + 1, v + 2].join(','); }"
        " return v.x.y.z.w.u.t.s = /[/; } }\n",
    };
    int failed = 0;
    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
        for (int mode = 0; mode < 3; mode++) {
            Realm realm;
            if (!realm_open_preparse(&realm, mode == 0 ? 0u : 1u, mode == 2)) return 1;
            JSValue result = JS_Eval(realm.ctx, sources[i], strlen(sources[i]),
                                     "unused.js", JS_EVAL_TYPE_GLOBAL);
            char *text = JS_IsException(result) ? exception_text(realm.ctx) : NULL;
            JS_FreeValue(realm.ctx, result);
            JSValue global = JS_GetGlobalObject(realm.ctx);
            JSValue ran = JS_GetPropertyStr(realm.ctx, global, "ran");
            bool side_effect = !JS_IsUndefined(ran);
            JS_FreeValue(realm.ctx, ran);
            JS_FreeValue(realm.ctx, global);
            JSLazyFunctionStats stats = realm_stats(&realm);
            if (text == NULL || strncmp(text, "EXC SyntaxError", 15) != 0 || side_effect
                || (mode == 2 && stats.preparse_fallbacks == 0)) {
                fprintf(stderr, "invalid unused function (%s): %s -> %s, %s, %llu fallbacks\n",
                        mode == 0 ? "eager" : mode == 1 ? "lazy" : "preparsed",
                        sources[i], text ? text : "no error",
                        side_effect ? "script ran" : "script did not run",
                        (unsigned long long) stats.preparse_fallbacks);
                failed = 1;
            }
            free(text);
            if (!realm_close(&realm)) failed = 1;
        }
    }
    return failed;
}

/* The same SyntaxError, with the same position, whether or not bodies were
   preparsed before the parse hit it; the script is not compiled again. */
static int run_retry_error_positions(void)
{
    static const char source[] =
        "function a(x) { return [x, x + 1, `t${x}`].map(v => v * 2); }\n"
        "function b(y) { if (y) { return { y }; } return /re}/.test(y); }\n"
        "var broken = ;\n";
    static const char valid[] =
        "function a(x) { return [x, x + 1, `t${x}`].map(v => v * 2); }\n"
        "function b(y) { if (y) { return { y }; } return /re}/.test(y); }\n";
    char *texts[2] = { NULL, NULL };
    int lines[2] = { -1, -1 }, failed = 0;
    for (int preparse = 0; preparse < 2; preparse++) {
        Realm realm;
        if (!realm_open_preparse(&realm, 1u, preparse)) return 1;
        JSValue compiled = JS_Eval(realm.ctx, source, sizeof(source) - 1, "retry.js",
                                   JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(compiled)) {
            JSValue exception = JS_GetException(realm.ctx);
            JSValue line = JS_GetPropertyStr(realm.ctx, exception, "lineNumber");
            JS_ToInt32(realm.ctx, &lines[preparse], line);
            JS_FreeValue(realm.ctx, line);
            const char *message = JS_ToCString(realm.ctx, exception);
            texts[preparse] = strdup(message ? message : "");
            JS_FreeCString(realm.ctx, message);
            JS_FreeValue(realm.ctx, exception);
        } else {
            JS_FreeValue(realm.ctx, compiled);
        }
        JSLazyFunctionStats stats = realm_stats(&realm);
        if (preparse && stats.preparse_restarts != 0) failed = 1;
        /* (the functions before the error are preparsed: without it) */
        compiled = JS_Eval(realm.ctx, valid, sizeof(valid) - 1, "valid.js",
                           JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(compiled)) failed = 1;
        JS_FreeValue(realm.ctx, compiled);
        stats = realm_stats(&realm);
        if (stats.preparsed != (preparse ? 2u : 0u)) {
            fprintf(stderr, "retried compile error: %llu preparsed\n",
                    (unsigned long long) stats.preparsed);
            failed = 1;
        }
        if (!realm_close(&realm)) failed = 1;
    }
    if (texts[0] == NULL || texts[1] == NULL || strcmp(texts[0], texts[1]) != 0
        || lines[0] != 3 || lines[1] != 3) failed = 1;
    if (failed)
        fprintf(stderr, "retried compile error: %s (line %d) / %s (line %d)\n",
                texts[0] ? texts[0] : "none", lines[0],
                texts[1] ? texts[1] : "none", lines[1]);
    free(texts[0]);
    free(texts[1]);
    return failed;
}

static bool eval_equals(Realm *realm, const char *source, const char *expected);

/* A direct eval found after a body was skipped compiles the script again
   without skipping: the functions around the eval must not be lazy. */
static int run_eval_restart(void)
{
    static const char source[] =
        "function outer() { var w = 'w';\n"
        "  var inner = function () { return w + [1, 2, 3].map(x => x * 2).join(''); };\n"
        "  eval('w = \"v\"'); return inner(); }\n"
        "outer()";
    Realm realm;
    int failed = 0;
    if (!realm_open_preparse(&realm, 1u, true)) return 1;
    failed = !eval_equals(&realm, source, "v246");
    JSLazyFunctionStats stats = realm_stats(&realm);
    if (stats.preparse_restarts != 1 || stats.preparsed != 0) {
        fprintf(stderr, "eval restart: restarts %llu preparsed %llu\n",
                (unsigned long long) stats.preparse_restarts,
                (unsigned long long) stats.preparsed);
        failed = 1;
    }
    if (!realm_close(&realm)) failed = 1;
    return failed;
}

static int run_early_errors(void)
{
    int failed = 0;
    for (size_t i = 0; i < sizeof(early_errors) / sizeof(early_errors[0]); i++) {
        /* the same error in every mode */
        char *texts[3];
        for (int mode = 0; mode < 3; mode++)
            failed |= early_error_case(early_errors[i], mode, &texts[mode]);
        for (int mode = 1; mode < 3; mode++) {
            if (texts[0] && texts[mode] && strcmp(texts[0], texts[mode]) != 0) {
                fprintf(stderr, "early error %s: %s / %s\n", early_errors[i],
                        texts[0], texts[mode]);
                failed = 1;
            }
        }
        for (int mode = 0; mode < 3; mode++) free(texts[mode]);
    }
    failed |= run_invalid_unused_function();
    failed |= run_retry_error_positions();
    failed |= run_eval_restart();
    return failed;
}

/* ------------------------------------------------------------ helpers */

static JSValue eval_value(Realm *realm, const char *source)
{
    return JS_Eval(realm->ctx, source, strlen(source), "eval.js",
                   JS_EVAL_TYPE_GLOBAL);
}

static bool eval_equals(Realm *realm, const char *source, const char *expected)
{
    char *text = value_text(realm->ctx, eval_value(realm, source));
    bool okay = text != NULL && strcmp(text, expected) == 0;
    if (!okay) {
        fprintf(stderr, "%s\n  expected %s\n  got      %s\n", source, expected,
                text ? text : "(null)");
    }
    free(text);
    return okay;
}

/* ----------------------------------------------- counting and memory */

/* Only called functions are compiled, each once, and the heap of a script
   whose functions mostly never run is smaller than with eager compilation. */
static int run_deferral_census(void)
{
    char *source = malloc(64 * 1024);
    if (source == NULL) return 1;
    size_t used = 0;
    for (int i = 0; i < 200; i++) {
        used += (size_t) sprintf(source + used,
            "function unused%d(a, b) { var t = [a, b, %d]; for (var i = 0; i < t.length; i++) "
            "{ t[i] = String(t[i]) + 'suffix-%d'; } return t.join(':') + unused%d.name; }\n",
            i, i, i, i);
    }
    used += (size_t) sprintf(source + used,
        "function used(x) { return x + 1; }\n"
        "used(1) + used(2);\n");
    size_t heap[2] = { 0, 0 };
    JSLazyFunctionStats stats[2];
    int failed = 0;
    for (int lazy = 0; lazy < 2; lazy++) {
        Realm realm;
        if (!realm_open(&realm, lazy ? 1u : 0u)) return 1;
        JSValue compiled = JS_Eval(realm.ctx, source, used, "census.js",
                                   JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
        JSValue value = JS_IsException(compiled) ? JS_EXCEPTION
            : JS_EvalFunction(realm.ctx, compiled);
        char *text = value_text(realm.ctx, value);
        if (text == NULL || strcmp(text, "5") != 0) failed = 1;
        free(text);
        JS_RunGC(realm.rt);
        JSMemoryUsage usage;
        JS_ComputeMemoryUsage(realm.rt, &usage);
        heap[lazy] = (size_t) usage.malloc_size;
        stats[lazy] = realm_stats(&realm);
        if (!realm_close(&realm)) failed = 1;
    }
    if (stats[1].deferred != 201 || stats[1].compiled != 1
        || stats[1].compile_failures != 0 || stats[0].deferred != 0) {
        fprintf(stderr, "census: deferred %llu compiled %llu failures %llu\n",
                (unsigned long long) stats[1].deferred,
                (unsigned long long) stats[1].compiled,
                (unsigned long long) stats[1].compile_failures);
        failed = 1;
    }
    /* each unused function keeps its text (~150 bytes) and a stub, not its
       bytecode, constants and line table */
    if (!(heap[1] + 30u * 1024u < heap[0])) {
        fprintf(stderr, "census: lazy heap %zu, eager heap %zu\n", heap[1], heap[0]);
        failed = 1;
    }
    free(source);
    return failed;
}

static int run_long_identifier(void)
{
    /* Cross the old 128-byte temporary buffer, then end directly at EOF.
       The same long ASCII prefix must also accept a Unicode continuation. */
    char name[1025], source[4096];
    memset(name, 'a', sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    int failed = 0;
    for (unsigned lazy = 0; lazy < 2; lazy++) {
        snprintf(source, sizeof(source), "var %s=19; var %s\u00e9=23;",
                 name, name);
        Realm realm;
        if (!realm_open(&realm, lazy)) return 1;
        JSValue result = eval_value(&realm, source);
        if (JS_IsException(result)) failed = 1;
        JS_FreeValue(realm.ctx, result);
        if (!eval_equals(&realm, name, "19")) failed = 1;
        snprintf(source, sizeof(source), "%s\u00e9", name);
        if (!eval_equals(&realm, source, "23")) failed = 1;
        if (!realm_close(&realm)) failed = 1;
    }
    return failed;
}

/* The threshold keeps small functions eager. */
static int run_threshold(void)
{
    Realm realm;
    if (!realm_open(&realm, 60u)) return 1;
    int failed = !eval_equals(&realm,
        "function small(a) { return a; }\n"
        "function large(a) { var longer = [a, a, a, a, a, a, a, a, a, a, a, a, a]; return longer.length; }\n"
        "small(1) + large(1)",
        "14");
    JSLazyFunctionStats stats = realm_stats(&realm);
    if (stats.deferred != 1 || stats.compiled != 1) {
        fprintf(stderr, "threshold: deferred %llu compiled %llu\n",
                (unsigned long long) stats.deferred,
                (unsigned long long) stats.compiled);
        failed = 1;
    }
    if (!realm_close(&realm)) failed = 1;
    return failed;
}

/* A stripped script keeps a private copy of each lazy function's text for
   its compile; toString() shows the native form before and after. */
static int run_stripped_source(void)
{
    Realm realm;
    if (!realm_open(&realm, 1u)) return 1;
    JS_SetStripInfo(realm.rt, JS_STRIP_SOURCE);
    static const char stripped[] =
        "function outer(a) { function inner(b) { return a + b + a * b + a - b; } return inner; }\n"
        "globalThis.outer = outer; globalThis.before = String(outer);";
    JSValue compiled = JS_Eval(realm.ctx, stripped, sizeof(stripped) - 1u,
        "stripped.js", JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    JS_SetStripInfo(realm.rt, 0);
    JSValue value = JS_IsException(compiled) ? JS_EXCEPTION
        : JS_EvalFunction(realm.ctx, compiled);
    char *text = value_text(realm.ctx, value);
    free(text);
    int failed = !eval_equals(&realm,
        "var inner = outer(4); [inner(2), before === String(outer), String(inner),"
        " String(outer)].join('|')",
        "16|true|function inner() {\n    [native code]\n}|function outer() {\n    [native code]\n}");
    failed |= !eval_equals(&realm,
        "try { outer(1)(null.x); } catch (e) { e.stack.split('\\n')[0].trim() }",
        "at <eval> (eval.js:1:20)");
    JSLazyFunctionStats stats = realm_stats(&realm);
    if (stats.deferred != 2 || stats.compiled != 2) {
        fprintf(stderr, "stripped: deferred %llu compiled %llu\n",
                (unsigned long long) stats.deferred,
                (unsigned long long) stats.compiled);
        failed = 1;
    }
    if (!realm_close(&realm)) failed = 1;
    return failed;
}

/* ----------------------------------------------------- serialization */

static const char serialized_module[] =
    "let hits = 0;\n"
    "const tag = 'm';\n"
    "export function hit(n) { hits += n; return (() => tag + hits)(); }\n"
    "export class Counter { #n = 0; up() { return ++this.#n; } }\n"
    "export const arrow = (a, b = 2) => { const inner = x => x * b; return inner(a); };\n"
    "export function early() { return 'early'; }\n"
    "export function text() { return String(hit); }\n"
    "export function position() { try { null.q; } catch (e) { return e.stack.split('\\n')[0].trim(); } }\n"
    "early();\n";

static const ModuleSource serialized_modules[] = {
    { "serialized", serialized_module },
    { NULL, NULL }
};

static const char serialized_probe[] =
    "import { hit, Counter, arrow, text, position, early } from 'serialized';\n"
    "const c = new Counter(); c.up();\n"
    "globalThis.result = [hit(1), hit(2), c.up(), arrow(5), arrow.length, hit.name,\n"
    "  text() === 'function hit(n) { hits += n; return (() => tag + hits)(); }', position(), early()].join('|');\n";

static const char serialized_expected[] =
    "m1|m3|2|10|1|hit|true|at position (serialized:8:40)|early";

/* Module bytecode written with lazy functions restores them as lazy
   functions (compiled at first call) in another runtime, with the same
   results as a module compiled eagerly. */
static int run_serialized_module(bool strip)
{
    int failed = 0;
    size_t length = 0;
    uint8_t *bytes = NULL;
    {
        Realm realm;
        if (!realm_open(&realm, 1u)) return 1;
        if (strip) JS_SetStripInfo(realm.rt, JS_STRIP_SOURCE);
        JSValue compiled = JS_Eval(realm.ctx, serialized_module,
                                   sizeof(serialized_module) - 1, "serialized",
                                   JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        JS_SetStripInfo(realm.rt, 0);
        if (JS_IsException(compiled)) {
            free(exception_text(realm.ctx));
            failed = 1;
        } else {
            uint8_t *written = JS_WriteObject(realm.ctx, &length, compiled,
                                              JS_WRITE_OBJ_BYTECODE);
            if (written != NULL && (bytes = malloc(length)) != NULL)
                memcpy(bytes, written, length);
            js_free(realm.ctx, written);
            JS_FreeValue(realm.ctx, compiled);
        }
        JSLazyFunctionStats stats = realm_stats(&realm);
        if (stats.deferred == 0) failed = 1;
        if (!realm_close(&realm)) failed = 1;
    }
    if (bytes == NULL) return 1;
    Realm realm;
    if (!realm_open(&realm, 0u)) return 1;
    JSValue module = JS_ReadObject(realm.ctx, bytes, length, JS_READ_OBJ_BYTECODE);
    free(bytes);
    if (JS_IsException(module)) {
        char *text = exception_text(realm.ctx);
        fprintf(stderr, "serialized: read failed %s\n", text);
        free(text);
        realm_close(&realm);
        return 1;
    }
    if (JS_ResolveModule(realm.ctx, module) < 0) failed = 1;
    /* register it under its name, run it, then import it from a probe */
    JSValue evaluated = JS_EvalFunction(realm.ctx, module);
    if (JS_IsException(evaluated)) {
        free(exception_text(realm.ctx));
        failed = 1;
    } else {
        JS_FreeValue(realm.ctx, evaluated);
    }
    run_jobs(&realm);
    char *text = realm_run_module(&realm, serialized_probe);
    const char *expected = serialized_expected;
    char stripped_expected[256];
    if (strip) {
        /* the stripped module has no toString() text */
        snprintf(stripped_expected, sizeof(stripped_expected), "%s",
                 "m1|m3|2|10|1|hit|false|at position (serialized:8:40)|early");
        expected = stripped_expected;
    }
    if (text == NULL || strcmp(text, expected) != 0) {
        fprintf(stderr, "serialized%s:\n  expected %s\n  got      %s\n",
                strip ? " (stripped)" : "", expected, text ? text : "(null)");
        failed = 1;
    }
    free(text);
    JSLazyFunctionStats stats = realm_stats(&realm);
    if (stats.restored == 0 || stats.compiled == 0 || stats.compile_failures != 0) {
        fprintf(stderr, "serialized: restored %llu compiled %llu failures %llu\n",
                (unsigned long long) stats.restored,
                (unsigned long long) stats.compiled,
                (unsigned long long) stats.compile_failures);
        failed = 1;
    }
    if (!realm_close(&realm)) failed = 1;
    return failed;
}

/* Bytecode written after some lazy functions compiled keeps their compiled
   bodies (and the lazy functions nested in them) and the rest lazy. */
static int run_serialized_after_calls(void)
{
    static const char script[] =
        "var calls = 0;\n"
        "function outer(a) {\n"
        "  var mid = function(b) { return function innermost(c) { return a + b + c; }; };\n"
        "  var spare = function(d) { return 'spare' + a + d; };\n"
        "  return globalThis.second ? spare : mid;\n"
        "}\n"
        "function untouched(x) { return x * 3; }\n"
        "function neverInFirst(y) { return 'late' + y; }\n"
        "globalThis.second ? [outer(1)(2), neverInFirst(3), untouched(2)].join()\n"
        "  : [outer(1)(2)(3), outer(10)(20)(30), untouched(5), String(outer).length].join();\n";
    int failed = 0;
    size_t length = 0;
    uint8_t *bytes = NULL;
    char *first = NULL;
    {
        Realm realm;
        if (!realm_open(&realm, 1u)) return 1;
        JSValue compiled = JS_Eval(realm.ctx, script, sizeof(script) - 1, "calls.js",
                                   JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(compiled)) {
            free(exception_text(realm.ctx));
            realm_close(&realm);
            return 1;
        }
        JSValue kept = JS_DupValue(realm.ctx, compiled);
        first = value_text(realm.ctx, JS_EvalFunction(realm.ctx, compiled));
        JSLazyFunctionStats stats = realm_stats(&realm);
        if (stats.compiled < 3) failed = 1;
        uint8_t *written = JS_WriteObject(realm.ctx, &length, kept,
                                          JS_WRITE_OBJ_BYTECODE);
        if (written != NULL && (bytes = malloc(length)) != NULL)
            memcpy(bytes, written, length);
        js_free(realm.ctx, written);
        JS_FreeValue(realm.ctx, kept);
        if (!realm_close(&realm)) failed = 1;
    }
    if (bytes == NULL || first == NULL) return 1;
    Realm realm;
    if (!realm_open(&realm, 0u)) return 1;
    free(value_text(realm.ctx, eval_value(&realm, "globalThis.second = true")));
    JSValue read = JS_ReadObject(realm.ctx, bytes, length, JS_READ_OBJ_BYTECODE);
    free(bytes);
    char *second = JS_IsException(read) ? exception_text(realm.ctx)
        : value_text(realm.ctx, JS_EvalFunction(realm.ctx, read));
    JSLazyFunctionStats stats = realm_stats(&realm);
    if (strcmp(first, "6,60,15,199") != 0 || second == NULL
        || strcmp(second, "spare12,late3,6") != 0
        /* spare() and neverInFirst() were still lazy when written */
        || stats.restored != 2 || stats.compiled != 2
        || stats.compile_failures != 0) {
        fprintf(stderr, "serialized after calls: %s then %s (restored %llu)\n",
                first, second ? second : "(null)",
                (unsigned long long) stats.restored);
        failed = 1;
    }
    free(first);
    free(second);
    if (!realm_close(&realm)) failed = 1;
    return failed;
}

/* --------------------------------------------------------- failures */

/* A first-call compile that runs out of memory throws, leaves the function
   lazy and every byte accounted for, and the next call compiles it. */
static int run_compile_memory_failure(size_t allowance, bool *refused)
{
    Realm realm;
    if (!realm_open(&realm, 1u)) return 1;
    int failed = 0;
    free(value_text(realm.ctx, eval_value(&realm,
        "function big(a) { var o = { p: a, q: 'text-' + a, r: [a, a + 1, a + 2] };\n"
        "  return Object.keys(o).map(function(k) { return k + ':' + o[k]; }).join(',')\n"
        "    + [1, 2, 3].map(x => x * a).join(''); }\n"
        "globalThis.big = big; 'ok'")));
    JS_RunGC(realm.rt);
    JSMemoryUsage usage;
    JS_ComputeMemoryUsage(realm.rt, &usage);
    JS_SetMemoryLimit(realm.rt, (size_t) usage.malloc_size + allowance);
    JSValue global = JS_GetGlobalObject(realm.ctx);
    JSValue big = JS_GetPropertyStr(realm.ctx, global, "big");
    JS_FreeValue(realm.ctx, global);
    JSValue attempt = JS_Call(realm.ctx, big, JS_UNDEFINED, 0, NULL);
    JS_FreeValue(realm.ctx, big);
    JS_SetMemoryLimit(realm.rt, 8u * MIB);
    free(value_text(realm.ctx, attempt));
    JSLazyFunctionStats stats = realm_stats(&realm);
    if (stats.compile_failures != 0) *refused = true;
    if (!eval_equals(&realm, "big(2)", "p:2,q:text-2,r:2,3,4246")) failed = 1;
    if (!eval_equals(&realm, "big(3)", "p:3,q:text-3,r:3,4,5369")) failed = 1;
    stats = realm_stats(&realm);
    /* big() and its two callbacks, each compiled exactly once */
    if (stats.compiled != 3) {
        fprintf(stderr, "memory failure: compiled %llu\n",
                (unsigned long long) stats.compiled);
        failed = 1;
    }
    if (!realm_close(&realm)) failed = 1;
    return failed;
}

/* A first-call compile refused for lack of memory fails uncatchably: a
   page that catches the error and calls again (React's render loop retries
   a unit of work that threw) must not recompile the body on every turn.
   One attempt, then the script ends with "out of memory"; once memory is
   available again the function compiles. */
static int run_compile_memory_retry_loop(size_t allowance, bool *refused)
{
    Realm realm;
    if (!realm_open(&realm, 1u)) return 1;
    int failed = 0;
    free(value_text(realm.ctx, eval_value(&realm,
        "function big(a) { var o = { p: a, q: 'text-' + a, r: [a, a + 1, a + 2] };\n"
        "  return Object.keys(o).map(function(k) { return k + ':' + o[k]; }).join(',')\n"
        "    + [1, 2, 3].map(x => x * a).join(''); }\n"
        "globalThis.big = big; globalThis.caught = 0;\n"
        "globalThis.retry = function(n) { for (var i = 0; i < n; i++) {\n"
        "  try { return big(i); } catch (e) { globalThis.caught++; } }\n"
        "  return 'gave up'; }; retry(0)")));
    /* retry(0) compiled retry() itself, not big(). */
    JS_RunGC(realm.rt);
    JSMemoryUsage usage;
    JS_ComputeMemoryUsage(realm.rt, &usage);
    JSLazyFunctionStats before = realm_stats(&realm);
    JS_SetMemoryLimit(realm.rt, (size_t) usage.malloc_size + allowance);
    JSValue global = JS_GetGlobalObject(realm.ctx);
    JSValue retry = JS_GetPropertyStr(realm.ctx, global, "retry");
    JS_FreeValue(realm.ctx, global);
    JSValue rounds = JS_NewInt32(realm.ctx, 1000);
    JSValue result = JS_Call(realm.ctx, retry, JS_UNDEFINED, 1, &rounds);
    JS_FreeValue(realm.ctx, retry);
    char *text = value_text(realm.ctx, result);
    JS_SetMemoryLimit(realm.rt, 8u * MIB);
    JSLazyFunctionStats after = realm_stats(&realm);
    uint64_t failures = after.compile_failures - before.compile_failures;
    if (failures != 0) {
        *refused = true;
        /* Refused for memory: one attempt, uncatchable, "out of memory". */
        uint64_t memory = after.memory_failures - before.memory_failures;
        if (failures > 1 || memory != failures || text == NULL
            || strstr(text, "out of memory") == NULL
            || !eval_equals(&realm, "caught", "0")) {
            fprintf(stderr, "memory retry loop (allowance %zu): %llu "
                    "compile failures, %llu for memory, result %s\n",
                    allowance, (unsigned long long) failures,
                    (unsigned long long) memory, text ? text : "(null)");
            failed = 1;
        }
    }
    free(text);
    if (!eval_equals(&realm, "big(2)", "p:2,q:text-2,r:2,3,4246")) failed = 1;
    if (!realm_close(&realm)) failed = 1;
    return failed;
}

/* The interrupt handler can stop a long first-call compile; the call fails
   as an interrupted execution does (uncatchable), the function stays lazy,
   and a later call compiles it. */
static int interrupt_when_armed(JSRuntime *rt, void *opaque)
{
    (void) rt;
    return *(const int *) opaque;
}

static int run_interrupted_first_call(void)
{
    Realm realm;
    if (!realm_open(&realm, 1u)) return 1;
    int armed = 0, failed = 0;
    JS_SetInterruptHandler(realm.rt, interrupt_when_armed, &armed);
    char *source = malloc(64 * 1024);
    if (source == NULL) return 1;
    size_t used = (size_t) sprintf(source, "function big() { var x = 0;");
    for (int i = 0; i < 4000; i++) used += (size_t) sprintf(source + used, "x+=1;");
    used += (size_t) sprintf(source + used, "return x; }\nglobalThis.big = big; 'ok'");
    free(value_text(realm.ctx, JS_Eval(realm.ctx, source, used, "big.js",
                                       JS_EVAL_TYPE_GLOBAL)));
    free(source);
    JSValue global = JS_GetGlobalObject(realm.ctx);
    JSValue big = JS_GetPropertyStr(realm.ctx, global, "big");
    JS_FreeValue(realm.ctx, global);
    /* uncatchable: the page's catch does not run */
    static const char attempt[] =
        "globalThis.caught = false; try { big(); } catch (e) { globalThis.caught = true; }";
    armed = 1;
    JSValue first = JS_Eval(realm.ctx, attempt, sizeof(attempt) - 1u, "attempt.js",
                            JS_EVAL_TYPE_GLOBAL);
    armed = 0;
    char *first_text = value_text(realm.ctx, first);
    if (first_text == NULL || strcmp(first_text, "EXC InternalError: interrupted") != 0
        || !eval_equals(&realm, "caught", "false")) {
        fprintf(stderr, "interrupted first call: %s\n", first_text ? first_text : "(null)");
        failed = 1;
    }
    free(first_text);
    JSValue second = JS_Call(realm.ctx, big, JS_UNDEFINED, 0, NULL);
    char *text = value_text(realm.ctx, second);
    if (text == NULL || strcmp(text, "4000") != 0) failed = 1;
    free(text);
    JS_FreeValue(realm.ctx, big);
    JSLazyFunctionStats stats = realm_stats(&realm);
    if (stats.compile_failures != 1 || stats.compiled != 1) failed = 1;
    if (!realm_close(&realm)) failed = 1;
    return failed;
}

/* The public C call/get-property paths pass COPY_ARGV. A trivial captured
   getter does not mutate borrowed arguments, still owns its returned value,
   and must continue polling the watchdog even without an interpreter frame. */
static int run_native_capture_getter(void)
{
    Realm realm;
    if (!realm_open(&realm, 0u)) return 1;
    static const char source[] =
        "function make(){var value={id:23};return function(){return value;};}"
        "globalThis.captureGetter=make();"
        "Object.defineProperty(globalThis,'captureValue',{get:captureGetter});";
    int failed = 0, armed = 0;
    JSValue value = JS_Eval(realm.ctx, source, sizeof(source)-1,
                             "capture-getter.js", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(value)) failed = 1;
    JS_FreeValue(realm.ctx, value);
    JSValue global = JS_GetGlobalObject(realm.ctx);
    JSValue getter = JS_GetPropertyStr(realm.ctx, global, "captureGetter");
    JSValue args[] = {JS_NewString(realm.ctx, "borrowed"), JS_NewInt32(realm.ctx, 41)};
    JSValue first = JS_Call(realm.ctx, getter, JS_NULL, 2, args);
    JSValue second = JS_GetPropertyStr(realm.ctx, global, "captureValue");
    if (!JS_IsObject(first) || !JS_IsObject(second)
        || JS_VALUE_GET_PTR(first) != JS_VALUE_GET_PTR(second)) failed = 1;
    const char *argument = JS_ToCString(realm.ctx, args[0]);
    int32_t number = 0;
    if (argument == NULL || strcmp(argument, "borrowed") != 0
        || JS_ToInt32(realm.ctx, &number, args[1]) != 0 || number != 41) failed = 1;
    JS_FreeCString(realm.ctx, argument);
    JS_FreeValue(realm.ctx, args[0]);
    JS_FreeValue(realm.ctx, args[1]);
    JS_FreeValue(realm.ctx, first);
    JS_FreeValue(realm.ctx, second);
    JS_SetInterruptHandler(realm.rt, interrupt_when_armed, &armed);
    armed = 1;
    bool interrupted = false;
    for (unsigned i = 0; i < 20000; i++) {
        value = JS_Call(realm.ctx, getter, JS_NULL, 0, NULL);
        if (JS_IsException(value)) {
            JS_FreeValue(realm.ctx, JS_GetException(realm.ctx));
            interrupted = true;
            break;
        }
        JS_FreeValue(realm.ctx, value);
    }
    armed = 0;
    if (!interrupted) failed = 1;
    JS_FreeValue(realm.ctx, getter);
    JS_FreeValue(realm.ctx, global);
    if (!realm_close(&realm)) failed = 1;
    if (failed) fprintf(stderr, "native captured getter failed\n");
    return failed;
}

int main(void)
{
    int failed = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (run_case(&cases[i]) != 0) failed = 1;
    }
    if (run_early_errors() != 0) failed = 1;
    if (run_long_identifier() != 0) failed = 1;
    if (run_deferral_census() != 0) failed = 1;
    if (run_threshold() != 0) failed = 1;
    if (run_stripped_source() != 0) failed = 1;
    if (run_serialized_module(false) != 0) failed = 1;
    if (run_serialized_module(true) != 0) failed = 1;
    if (run_serialized_after_calls() != 0) failed = 1;
    if (run_interrupted_first_call() != 0) failed = 1;
    if (run_native_capture_getter() != 0) failed = 1;
    bool refused = false;
    for (size_t allowance = 0; allowance <= 16384; allowance += 128) {
        if (run_compile_memory_failure(allowance, &refused) != 0) {
            fprintf(stderr, "lazy compile under memory limit %zu failed\n", allowance);
            failed = 1;
            break;
        }
    }
    if (!refused) {
        fprintf(stderr, "no memory limit refused a lazy compile\n");
        failed = 1;
    }
    refused = false;
    for (size_t allowance = 0; allowance <= 16384; allowance += 128) {
        if (run_compile_memory_retry_loop(allowance, &refused) != 0) {
            failed = 1;
            break;
        }
    }
    if (!refused) {
        fprintf(stderr, "no memory limit refused a lazy compile in the "
                "retry loop\n");
        failed = 1;
    }
    puts(failed ? "QuickJS lazy functions: FAIL" : "QuickJS lazy functions: PASS");
    return failed;
}
