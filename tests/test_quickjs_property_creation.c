/* Property creation shortcuts in the vendored QuickJS (README change 27):
   new closures take a prebuilt per-realm shape ('length', 'name' and a
   constructor's lazily created 'prototype') instead of three property
   definitions, OP_append stores spread elements straight into the array
   literal, and OP_define_field adds an object literal's new field to an
   ordinary extensible object without the general definition path, and an
   ordinary descriptor object's fields are read from their slots. Each is
   a shortcut for a result the general path defines; these cases pin that
   result (key order, attributes, prototypes, constructor bits, receivers
   and descriptors that must keep the general path) so a shortcut that
   diverges fails. */
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

typedef struct {
    Budget budget;
    BudgetQuickJSPool *pool;
    JSRuntime *rt;
    JSContext *ctx;
} Realm;

static bool realm_open(Realm *realm)
{
    memset(realm, 0, sizeof(*realm));
    budget_init(&realm->budget, 16u * MIB);
    realm->pool = budget_quickjs_pool_create(&realm->budget);
    if (realm->pool == NULL) return false;
    realm->rt = JS_NewRuntime2(budget_quickjs_pool_allocator(), realm->pool);
    if (realm->rt == NULL) return false;
    JS_SetMaxStackSize(realm->rt, test_stack_limit());
    realm->ctx = JS_NewContext(realm->rt);
    return realm->ctx != NULL;
}

/* Everything the realm allocated must be returned, including the closure
   shapes the context keeps. */
static bool realm_close(Realm *realm)
{
    if (realm->ctx) JS_FreeContext(realm->ctx);
    if (realm->rt) JS_FreeRuntime(realm->rt);
    if (realm->pool) {
        (void) budget_quickjs_pool_trim(realm->pool, 0);
        if (!budget_quickjs_pool_destroy(realm->pool)) return false;
    }
    return realm->budget.current == 0;
}

/* Runs a script whose completion value is "ok" or a description of what
   differed; prints and returns 1 on anything else. */
static int expect_ok(JSContext *ctx, const char *name, const char *source)
{
    JSValue value = JS_Eval(ctx, source, strlen(source), name,
                            JS_EVAL_TYPE_GLOBAL);
    int failed = 1;
    if (JS_IsException(value)) {
        JSValue exception = JS_GetException(ctx);
        const char *text = JS_ToCString(ctx, exception);
        fprintf(stderr, "%s: exception %s\n", name, text ? text : "?");
        JS_FreeCString(ctx, text);
        JS_FreeValue(ctx, exception);
    } else {
        const char *text = JS_ToCString(ctx, value);
        if (text && strcmp(text, "ok") == 0) {
            failed = 0;
        } else {
            fprintf(stderr, "%s: %s\n", name, text ? text : "?");
        }
        JS_FreeCString(ctx, text);
    }
    JS_FreeValue(ctx, value);
    return failed;
}

#define CHECK_PRELUDE \
    "const errs = [];" \
    "const check = (c, m) => { if (!c) errs.push(m); };" \
    "const desc = (o, k) => JSON.stringify(Object.getOwnPropertyDescriptor(o, k));" \
    "const keys = (o) => Reflect.ownKeys(o).map(String).join();"

static const char closure_source[] =
    "(() => {" CHECK_PRELUDE
    "function named(a, b) {}"
    "const arrow = (x, y, z) => x;"
    "const anon = function () {};"
    "const defaults = function (a, b = 1, c) {};"
    "const rest = function (a, ...r) {};"
    "async function af(a) {}"
    "const aarrow = async (a, b) => a;"
    "function* gen(a) {}"
    "async function* agen() {}"
    "const o = { m(a) {}, ['c' + 'k'](a, b) {}, get g() { return 1; } };"
    "class K { method(x) {} static s() {} }"
    /* key order */
    "check(keys(named) === 'length,name,prototype', 'named keys ' + keys(named));"
    "check(keys(anon) === 'length,name,prototype', 'anon keys ' + keys(anon));"
    "check(keys(arrow) === 'length,name', 'arrow keys ' + keys(arrow));"
    "check(keys(af) === 'length,name', 'async keys ' + keys(af));"
    "check(keys(aarrow) === 'length,name', 'async arrow keys ' + keys(aarrow));"
    "check(keys(gen) === 'length,name,prototype', 'generator keys ' + keys(gen));"
    "check(keys(agen) === 'length,name,prototype', 'async generator keys ' + keys(agen));"
    "check(keys(o.m) === 'length,name', 'method keys ' + keys(o.m));"
    "check(keys(K.prototype.method) === 'length,name', 'class method keys');"
    /* attributes and values */
    "check(desc(named, 'length') === '{\"value\":2,\"writable\":false,\"enumerable\":false,\"configurable\":true}', 'length ' + desc(named, 'length'));"
    "check(desc(named, 'name') === '{\"value\":\"named\",\"writable\":false,\"enumerable\":false,\"configurable\":true}', 'name ' + desc(named, 'name'));"
    "check(desc(af, 'name') === '{\"value\":\"af\",\"writable\":false,\"enumerable\":false,\"configurable\":true}', 'async name ' + desc(af, 'name'));"
    "check(desc(named, 'prototype') === '{\"value\":{},\"writable\":true,\"enumerable\":false,\"configurable\":false}', 'prototype ' + desc(named, 'prototype'));"
    "check(arrow.length === 3 && defaults.length === 1 && rest.length === 1 && aarrow.length === 2, 'lengths');"
    "check(anon.name === 'anon' && arrow.name === 'arrow' && (() => {}).name === '', 'inferred names');"
    "check(o.m.name === 'm' && o.ck.name === 'ck' && Object.getOwnPropertyDescriptor(o, 'g').get.name === 'get g', 'method names');"
    "check(K.prototype.method.name === 'method' && K.s.name === 's', 'class method names');"
    /* prototypes and constructors */
    "check(Object.getPrototypeOf(named) === Function.prototype, 'function prototype');"
    "check(Object.getPrototypeOf(af) === Object.getPrototypeOf(async function () {}) && Object.getPrototypeOf(af)[Symbol.toStringTag] === 'AsyncFunction', 'async prototype');"
    "check(named.prototype.constructor === named && Object.getPrototypeOf(named.prototype) === Object.prototype, 'lazy prototype object');"
    "check((function () {}).prototype !== (function () {}).prototype, 'prototype per function');"
    "check(new named() instanceof named, 'constructible');"
    "const throwsType = (f) => { try { f(); return false; } catch (e) { return e instanceof TypeError; } };"
    "check(throwsType(() => new arrow()), 'arrow constructible');"
    "check(throwsType(() => new af()), 'async constructible');"
    "check(throwsType(() => new o.m()), 'method constructible');"
    "check(af() instanceof Promise, 'async call');"
    /* later changes stay on the one function */
    "const f1 = function () {}, f2 = function () {}, f3 = function () {};"
    "f1.extra = 1;"
    "check(!('extra' in f2) && keys(f1) === 'length,name,prototype,extra', 'added property');"
    "delete f2.name;"
    "check(f2.name === '' && keys(f2) === 'length,prototype' && f3.name === 'f3', 'deleted name');"
    "Object.defineProperty(f1, 'length', { value: 9 });"
    "check(f1.length === 9 && f3.length === 0, 'redefined length');"
    "Object.freeze(f3);"
    "const f4 = function () {};"
    "check(Object.isFrozen(f3) && !Object.isFrozen(f4) && Object.isExtensible(f4), 'frozen function');"
    "check(desc(f4, 'length') === '{\"value\":0,\"writable\":false,\"enumerable\":false,\"configurable\":true}', 'fresh length after freeze ' + desc(f4, 'length'));"
    "f4.prototype = 5;"
    "check(f4.prototype === 5 && typeof (function () {}).prototype === 'object', 'prototype writable');"
    /* captures */
    "const fs = [];"
    "for (let i = 0; i < 100; i++) fs.push(() => i);"
    "check(fs.every((f, i) => f() === i), 'captures');"
    "return errs.join('; ') || 'ok';"
    "})()";

static const char spread_source[] =
    "(() => {" CHECK_PRELUDE
    "const same = (a, b) => a.length === b.length && a.every((v, i) => Object.is(v, b[i]));"
    "const a = [1, 'two', { three: 3 }, 4.5, null, undefined];"
    "const b = [...a];"
    "check(b !== a && same(a, b), 'copy');"
    "check(same([0, ...a, 9, ...a], [0, 1, 'two', a[2], 4.5, null, undefined, 9, 1, 'two', a[2], 4.5, null, undefined]), 'positions');"
    "check(same([...'héllo'], ['h', 'é', 'l', 'l', 'o']), 'string');"
    "check(same([...new Set([3, 1, 3, 2])], [3, 1, 2]), 'set');"
    "check(same([...[1, , 3]], [1, undefined, 3]) && Object.keys([...[1, , 3]]).join() === '0,1,2', 'hole');"
    "check(same([...(function* () { yield 1; yield 2; })()], [1, 2]), 'generator');"
    "check(same((function () { return [...arguments]; })(5, 6), [5, 6]), 'arguments');"
    "const c = [...'abc'];"
    "check(same([...c, ...c], ['a', 'b', 'c', 'a', 'b', 'c']), 'compact source');"
    "const custom = [1, 2]; custom[Symbol.iterator] = function* () { yield 'x'; };"
    "check(same([...custom], ['x']), 'own iterator');"
    "const long = []; for (let i = 0; i < 1000; i++) long.push({ i });"
    "const copy = [...long, ...long];"
    "check(copy.length === 2000 && copy[1999] === long[999] && copy[1000].i === 0, 'long');"
    "check(Array.isArray(copy) && Object.getOwnPropertyDescriptor(copy, 5).writable, 'element attributes');"
    "const saved = Array.prototype[Symbol.iterator];"
    "Array.prototype[Symbol.iterator] = function* () { yield 'patched'; };"
    "const patched = [...[1, 2, 3]];"
    "Array.prototype[Symbol.iterator] = saved;"
    "check(same(patched, ['patched']), 'patched iterator');"
    "return errs.join('; ') || 'ok';"
    "})()";

static const char literal_source[] =
    "(() => {" CHECK_PRELUDE
    "const o = { a: 1, b: 'x', c: null };"
    "check(keys(o) === 'a,b,c' && o.a === 1 && o.b === 'x' && o.c === null, 'literal');"
    "check(desc(o, 'a') === '{\"value\":1,\"writable\":true,\"enumerable\":true,\"configurable\":true}', 'attributes ' + desc(o, 'a'));"
    "const dup = { a: 1, b: 2, a: 3 };"
    "check(keys(dup) === 'a,b' && dup.a === 3, 'duplicate key ' + keys(dup));"
    "const np = { __proto__: null, a: 1 };"
    "check(Object.getPrototypeOf(np) === null && keys(np) === 'a', 'null prototype');"
    "let setterCalls = 0;"
    "Object.defineProperty(Object.prototype, 'trap', { set(v) { setterCalls++; }, configurable: true });"
    "const shadow = { trap: 1 };"
    "delete Object.prototype.trap;"
    "check(setterCalls === 0 && shadow.trap === 1 && keys(shadow) === 'trap', 'defines, not sets');"
    "const many = []; for (let i = 0; i < 50; i++) many.push({ x: i, y: i + 1, z: [i] });"
    "check(many.every((m, i) => m.x === i && m.y === i + 1 && m.z[0] === i && keys(m) === 'x,y,z'), 'repeated literals');"
    /* class fields on receivers the shortcut must leave alone */
    "class Plain { x = 1; y = this.x + 1; }"
    "const p = new Plain();"
    "check(keys(p) === 'x,y' && p.y === 2, 'class fields');"
    "const trapped = [];"
    "class ProxyBase { constructor() { return new Proxy({}, { defineProperty(t, k, d) { trapped.push(k); return Reflect.defineProperty(t, k, d); } }); } }"
    "class ProxyFields extends ProxyBase { f = 1; g = 2; }"
    "const pf = new ProxyFields();"
    "check(trapped.join() === 'f,g' && pf.f === 1, 'proxy receiver ' + trapped.join());"
    "class FrozenBase { constructor() { return Object.freeze({}); } }"
    "class FrozenFields extends FrozenBase { f = 1; }"
    "let frozenThrew = false; try { new FrozenFields(); } catch (e) { frozenThrew = e instanceof TypeError; }"
    "check(frozenThrew, 'non-extensible receiver');"
    "class LockedBase { constructor() { const o = {}; Object.defineProperty(o, 'f', { value: 0, writable: true, configurable: false }); return o; } }"
    "class LockedFields extends LockedBase { f = 1; }"
    "let lockedThrew = false; try { new LockedFields(); } catch (e) { lockedThrew = e instanceof TypeError; }"
    "check(lockedThrew, 'non-configurable existing field');"
    "class ArrayFields extends Array { f = 1; }"
    "const af = new ArrayFields();"
    "check(af.f === 1 && Array.isArray(af) && keys(af) === 'length,f', 'array receiver ' + keys(af));"
    "class Existing { constructor() { this.f = 0; } }"
    "class Redefined extends Existing { f = 7; }"
    "const r = new Redefined();"
    "check(r.f === 7 && keys(r) === 'f', 'redefined field');"
    "return errs.join('; ') || 'ok';"
    "})()";

static const char descriptor_source[] =
    "(() => {" CHECK_PRELUDE
    "const g = function () { return 7; }, s = function (v) { this.seen = v; };"
    "const o = {};"
    "Object.defineProperty(o, 'a', { enumerable: true, get: g });"
    "check(desc(o, 'a') === JSON.stringify({ get: g, set: undefined, enumerable: true, configurable: false }) && o.a === 7, 'getter ' + desc(o, 'a'));"
    "Object.defineProperty(o, 'b', { value: 1, writable: true });"
    "check(desc(o, 'b') === '{\"value\":1,\"writable\":true,\"enumerable\":false,\"configurable\":false}', 'data ' + desc(o, 'b'));"
    "Object.defineProperty(o, 'c', { value: 2, writable: 0, enumerable: 'yes', configurable: {} });"
    "check(desc(o, 'c') === '{\"value\":2,\"writable\":false,\"enumerable\":true,\"configurable\":true}', 'truthiness ' + desc(o, 'c'));"
    "Object.defineProperty(o, 'd', { set: s, get: undefined, configurable: true });"
    "o.d = 5; check(o.seen === 5 && o.d === undefined, 'setter');"
    "Object.defineProperty(o, 'd', { enumerable: true });"
    "check(desc(o, 'd') === JSON.stringify({ get: undefined, set: s, enumerable: true, configurable: true }), 'partial redefinition ' + desc(o, 'd'));"
    "Object.defineProperty(o, 'e', Object.assign(Object.create(null), { value: 3, enumerable: true }));"
    "check(o.e === 3 && Object.keys(o).includes('e'), 'null-prototype descriptor');"
    "Object.defineProperty(o, 'f', Object.create({ get: g }));"
    "check(o.f === 7, 'inherited getter field');"
    "Object.defineProperty(o, 'h', Object.create(Object.create({ enumerable: true, value: 9 })));"
    "check(o.h === 9 && Object.keys(o).includes('h'), 'fields two prototypes up');"
    "Object.prototype.enumerable = true;"
    "try { Object.defineProperty(o, 'i', { value: 4 }); } finally { delete Object.prototype.enumerable; }"
    "check(Object.keys(o).includes('i'), 'field inherited from Object.prototype');"
    "const order = [];"
    "const traced = { get enumerable() { order.push('enumerable'); return true; }, get value() { order.push('value'); return 8; }, get writable() { order.push('writable'); return false; } };"
    "Object.defineProperty(o, 'j', traced);"
    "check(order.join() === 'enumerable,value,writable' && o.j === 8, 'accessor fields ' + order.join());"
    "const trapped = [];"
    "const proxy = new Proxy({ value: 6 }, { has(t, k) { trapped.push('has:' + String(k)); return k in t; }, get(t, k) { trapped.push('get:' + String(k)); return t[k]; } });"
    "Object.defineProperty(o, 'k', proxy);"
    "check(o.k === 6 && trapped.join() === 'has:enumerable,has:configurable,has:value,get:value,has:writable,has:get,has:set', 'proxy descriptor ' + trapped.join());"
    "const throwsType = (f, m) => { try { f(); return false; } catch (e) { return e instanceof TypeError && (!m || e.message === m); } };"
    "check(throwsType(() => Object.defineProperty({}, 'x', { get: 1 }), 'invalid getter'), 'invalid getter');"
    "check(throwsType(() => Object.defineProperty({}, 'x', { set: {} }), 'invalid setter'), 'invalid setter');"
    "check(throwsType(() => Object.defineProperty({}, 'x', { get: g, value: 1 }), 'cannot have setter/getter and value or writable'), 'getter and value');"
    "check(throwsType(() => Object.defineProperty({}, 'x', { set: s, writable: false })), 'setter and writable');"
    "check(throwsType(() => Object.defineProperty({}, 'x', 1)), 'primitive descriptor');"
    "check(Reflect.defineProperty(o, 'l', { value: 10, enumerable: true }) && o.l === 10, 'Reflect.defineProperty');"
    "const many = Object.defineProperties({}, { p: { value: 1, enumerable: true }, q: { get: g, enumerable: true } });"
    "check(keys(many) === 'p,q' && many.q === 7, 'defineProperties');"
    "const created = Object.create(null, { r: { value: 2, writable: true } });"
    "check(desc(created, 'r') === '{\"value\":2,\"writable\":true,\"enumerable\":false,\"configurable\":false}', 'Object.create');"
    "const arr = [];"
    "Object.defineProperty(arr, 0, { value: 'x', writable: true, enumerable: true, configurable: true });"
    "check(arr.length === 1 && arr[0] === 'x', 'array index');"
    "const frozen = Object.freeze({});"
    "check(throwsType(() => Object.defineProperty(frozen, 'x', { value: 1 })), 'non-extensible target');"
    "return errs.join('; ') || 'ok';"
    "})()";

/* Shapes released by dead objects and found again by the next object
   built the same way: keys, values and prototypes must be the new
   object's, including when the old prototype died and another object
   may now live at its address. */
static const char released_shape_source[] =
    "(() => {" CHECK_PRELUDE
    "for (let round = 0; round < 3; round++) {"
    "  for (let i = 0; i < 200; i++) {"
    "    const o = { a: i, b: i + 1, c: [i], d: 'x' + i };"
    "    if (keys(o) !== 'a,b,c,d' || o.d !== 'x' + i || o.c[0] !== i) { errs.push('literal ' + i); break; }"
    "    const P = { tag: i };"
    "    const q = Object.create(P); q.a = 1; q.b = 2;"
    "    if (Object.getPrototypeOf(q) !== P || q.tag !== i || keys(q) !== 'a,b') { errs.push('prototype ' + i); break; }"
    "    class C { constructor(v) { this.x = v; this.y = v * 2; } }"
    "    const c = new C(i);"
    "    if (Object.getPrototypeOf(c) !== C.prototype || c.y !== 2 * i || keys(c) !== 'x,y') { errs.push('class ' + i); break; }"
    "    const n = Object.create(null); n.k = i;"
    "    if (Object.getPrototypeOf(n) !== null || n.k !== i) { errs.push('null prototype ' + i); break; }"
    "  }"
    "}"
    "const kept = [];"
    "for (let i = 0; i < 100; i++) { const o = { p: i }; o['v' + (i % 40)] = i; kept.push(o); }"
    "check(kept.every((o, i) => o.p === i && o['v' + (i % 40)] === i && Object.keys(o).length === 2), 'kept objects');"
    "const frozen = Object.freeze({ a: 1, b: 2 });"
    "const next = { a: 3, b: 4 }; next.c = 5;"
    "check(Object.isFrozen(frozen) && !Object.isFrozen(next) && next.c === 5 && keys(next) === 'a,b,c', 'frozen sibling');"
    "return errs.join('; ') || 'ok';"
    "})()";

/* A second realm's closures take its own Function.prototype. */
static const char realm_source[] =
    "(() => {"
    "const f = function () {}, a = async () => 0;"
    "return Object.getPrototypeOf(f) === Function.prototype"
    " && Object.getPrototypeOf(a) === Object.getPrototypeOf(async function () {})"
    " && f.prototype.constructor === f ? 'ok' : 'realm prototypes';"
    "})()";

static int run_second_realm(void)
{
    Realm realm;
    if (!realm_open(&realm)) return 1;
    JSContext *second = JS_NewContext(realm.rt);
    int failed = second == NULL;
    if (!failed) {
        failed |= expect_ok(realm.ctx, "<realm-1>", realm_source);
        failed |= expect_ok(second, "<realm-2>", realm_source);
        /* each realm's function must not take the other's prototype */
        JSValue f1 = JS_Eval(realm.ctx, "(function () {})", 16, "<f1>", 0);
        JSValue f2 = JS_Eval(second, "(function () {})", 16, "<f2>", 0);
        JSValue p1 = JS_GetPrototype(realm.ctx, f1);
        JSValue p2 = JS_GetPrototype(second, f2);
        if (!JS_IsObject(p1) || !JS_IsObject(p2)
            || JS_VALUE_GET_PTR(p1) == JS_VALUE_GET_PTR(p2)) {
            fprintf(stderr, "second realm: shared Function.prototype\n");
            failed = 1;
        }
        JS_FreeValue(realm.ctx, p1);
        JS_FreeValue(second, p2);
        JS_FreeValue(realm.ctx, f1);
        JS_FreeValue(second, f2);
        JS_FreeContext(second);
    }
    if (!realm_close(&realm)) {
        fprintf(stderr, "second realm: memory not returned\n");
        failed = 1;
    }
    return failed;
}

static int run_case(const char *name, const char *source)
{
    Realm realm;
    if (!realm_open(&realm)) {
        fprintf(stderr, "%s: realm\n", name);
        return 1;
    }
    int failed = expect_ok(realm.ctx, name, source);
    /* Run it again after a collection: the closure shapes and any shape
       the first run left behind must still be right. */
    JS_RunGC(realm.rt);
    failed |= expect_ok(realm.ctx, name, source);
    if (!realm_close(&realm)) {
        fprintf(stderr, "%s: memory not returned\n", name);
        failed = 1;
    }
    return failed;
}

/* A closure-heavy script under a series of heap limits, each refusing
   the script's allocations from a different point on: the prebuilt
   shapes, the objects built from them and the literal, spread and field
   paths must fail cleanly and return everything. */
static int run_allocation_failures(void)
{
    static const char source[] =
        "const fs = []; for (let i = 0; i < 8; i++) {"
        " fs.push(function () { return i; }, () => i, async () => i);"
        " fs.push([...fs]); fs.push({ a: i, b: fs.length }); } fs.length";
    int failed = 0;
    int completed = 0;
    unsigned refused = 0;
    for (size_t limit = 64u * 1024u; limit <= 2u * MIB && !completed;
         limit += 2048u) {
        Realm realm;
        memset(&realm, 0, sizeof(realm));
        budget_init(&realm.budget, 16u * MIB);
        realm.pool = budget_quickjs_pool_create(&realm.budget);
        if (realm.pool == NULL) return 1;
        realm.rt = JS_NewRuntime2(budget_quickjs_pool_allocator(), realm.pool);
        if (realm.rt == NULL) return 1;
        JS_SetMaxStackSize(realm.rt, test_stack_limit());
        realm.ctx = JS_NewContext(realm.rt);
        if (realm.ctx != NULL) {
            JS_SetMemoryLimit(realm.rt, limit);
            JSValue value = JS_Eval(realm.ctx, source, strlen(source),
                                    "<refusals>", JS_EVAL_TYPE_GLOBAL);
            if (!JS_IsException(value)) {
                int32_t length = 0;
                if (JS_ToInt32(realm.ctx, &length, value) != 0 || length != 40) {
                    fprintf(stderr, "refusals: limit %zu gave %d\n", limit, length);
                    failed = 1;
                }
                completed = 1;
            } else {
                refused++;
                JS_FreeValue(realm.ctx, JS_GetException(realm.ctx));
            }
            JS_FreeValue(realm.ctx, value);
            JS_SetMemoryLimit(realm.rt, 0);
        }
        if (!realm_close(&realm)) {
            fprintf(stderr, "refusals: memory not returned at limit %zu\n", limit);
            failed = 1;
            break;
        }
    }
    printf("refusals: %u memory limits refused the script before one let it complete\n",
           refused);
    if (!completed || refused == 0) {
        fprintf(stderr, "refusals: %u refused, completed=%d\n", refused, completed);
        failed = 1;
    }
    return failed;
}

/* One refused allocation at a time, at each allocation of the shortcut
   paths in turn. QuickJS carves small blocks from per-size arenas, and
   the pool only sees an arena refill, so a refusal can only land on an
   allocation that finds its size class full. The case therefore first
   fills one size class with 'pad' strings of one length (every length
   class from 24 to 160 bytes, every fill level of an arena), compiles the
   script, arms a single refusal of the next pool allocation, and runs it:
   whichever allocation of the new paths first needs a refill fails
   (the per-realm closure shapes, the closure object and its properties,
   literal fields, spread elements). The run must then either have thrown
   or have succeeded with no exception left pending, and an unrelated
   script must run correctly afterwards. */
static int run_one_shot_refusals(void)
{
    static const char source[] =
        "(function () {"
        " const f = function () { return 1; }, g = () => 2, h = async () => 3;"
        " const o = { p: 1, q: 2, r: 3, s: 4 };"
        " const a = [...[1, 2, 3], ...'ab', ...[o, f]];"
        " Object.defineProperty(o, 't', { value: 5, enumerable: true });"
        " return f() + g() + a.length + o.t;"
        "})()";
    static const char after[] = "[1, 2, 3].map((x) => x * 2).join()";
    int failed = 0;
    unsigned refused = 0, completed = 0;
    char buffer[200];
    memset(buffer, 'p', sizeof(buffer));
    for (int length = 8; length <= 144 && !failed; length += 8) {
        int fills = 4096 / (length + 16) + 2;
        for (int count = 0; count <= fills && !failed; count++) {
            Realm realm;
            if (!realm_open(&realm)) return 1;
            JSValue pad[300];
            int padded = count < 300 ? count : 300;
            for (int i = 0; i < padded; i++) {
                buffer[0] = (char) ('A' + i % 26);
                pad[i] = JS_NewStringLen(realm.ctx, buffer, (size_t) length);
            }
            JSValue compiled = JS_Eval(realm.ctx, source, sizeof(source) - 1,
                                       "<one-shot>",
                                       JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
            if (JS_IsException(compiled)) {
                fprintf(stderr, "one-shot: compile failed\n");
                failed = 1;
            } else {
                budget_inject_failure_after(&realm.budget, 0);
                size_t injected = realm.budget.injected_failure_count;
                JSValue value = JS_EvalFunction(realm.ctx, compiled);
                bool hit = realm.budget.injected_failure_count != injected;
                budget_clear_failure_injection(&realm.budget);
                if (JS_IsException(value)) {
                    JS_FreeValue(realm.ctx, JS_GetException(realm.ctx));
                    refused++;
                } else {
                    int32_t result = 0;
                    if (JS_HasException(realm.ctx)) {
                        fprintf(stderr, "one-shot: pad %d x %d: the script "
                                "succeeded with an exception pending\n",
                                count, length);
                        failed = 1;
                    } else if (JS_ToInt32(realm.ctx, &result, value) != 0
                               || result != 15) {
                        fprintf(stderr, "one-shot: pad %d x %d: result %d\n",
                                count, length, result);
                        failed = 1;
                    }
                    if (hit) completed++;
                }
                JS_FreeValue(realm.ctx, value);
                JSValue check = JS_Eval(realm.ctx, after, sizeof(after) - 1,
                                        "<after>", JS_EVAL_TYPE_GLOBAL);
                const char *text = JS_IsException(check) ? NULL
                    : JS_ToCString(realm.ctx, check);
                if (text == NULL || strcmp(text, "2,4,6") != 0
                    || JS_HasException(realm.ctx)) {
                    fprintf(stderr, "one-shot: pad %d x %d: later script "
                            "gave %s\n", count, length, text ? text : "an exception");
                    failed = 1;
                }
                JS_FreeCString(realm.ctx, text);
                JS_FreeValue(realm.ctx, check);
            }
            for (int i = 0; i < padded; i++)
                JS_FreeValue(realm.ctx, pad[i]);
            if (!realm_close(&realm)) {
                fprintf(stderr, "one-shot: pad %d x %d: memory not returned\n",
                        count, length);
                failed = 1;
            }
        }
    }
    printf("one-shot refusals: %u runs threw, %u absorbed a refusal and completed\n",
           refused, completed);
    if (refused == 0) {
        fprintf(stderr, "one-shot: no refusal reached the script\n");
        failed = 1;
    }
    return failed;
}

int main(void)
{
    int failed = 0;
    failed |= run_case("<closures>", closure_source);
    failed |= run_case("<spread>", spread_source);
    failed |= run_case("<literals>", literal_source);
    failed |= run_case("<descriptors>", descriptor_source);
    failed |= run_case("<released shapes>", released_shape_source);
    failed |= run_second_realm();
    failed |= run_allocation_failures();
    failed |= run_one_shot_refusals();
    puts(failed ? "QuickJS property creation: FAIL" : "QuickJS property creation: PASS");
    return failed;
}
