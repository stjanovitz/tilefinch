/* JSON.parse reads its argument string in place (third_party/quickjs
   README, "JSON.parse without a UTF-8 copy"): 8-bit (Latin-1) strings and
   16-bit (UTF-16) strings are tokenized directly instead of through the
   UTF-8 copy JSON.parse used to make.

   The UTF-8 tokenizer is still in the engine (JS_ParseJSON, which import
   maps and the embedder use), and it is exactly what JSON.parse ran
   before: this test exposes it to script as __utf8Parse(text) (the old
   JS_ToCStringLen copy plus JS_ParseJSON) and requires both paths to give
   the same value, or the same error name, message and position, for every
   input below, including every truncation and every single-character
   substitution of a set of documents in ASCII, Latin-1 and UTF-16 form.
   Then the reviver's source text, surrogates, numbers, __proto__, nesting,
   JSON.rawJSON and huge inputs, and finally the memory the change saves:
   a large JSON.parse that fits a heap in which the UTF-8 copy is refused. */
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

/* JSON.parse as it was: a UTF-8 copy of ToString(text), then the UTF-8
   tokenizer under the same "<input>" file name. */
static JSValue utf8_parse(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv)
{
    (void) this_val;
    size_t length;
    const char *text = JS_ToCStringLen(ctx, &length,
                                       argc > 0 ? argv[0] : JS_UNDEFINED);
    if (text == NULL) return JS_EXCEPTION;
    JSValue value = JS_ParseJSON(ctx, text, length, "<input>");
    JS_FreeCString(ctx, text);
    return value;
}

static bool realm_open(Realm *realm, size_t limit)
{
    memset(realm, 0, sizeof(*realm));
    budget_init(&realm->budget, limit);
    realm->pool = budget_quickjs_pool_create(&realm->budget);
    if (realm->pool == NULL) return false;
    realm->rt = JS_NewRuntime2(budget_quickjs_pool_allocator(), realm->pool);
    if (realm->rt == NULL) return false;
    JS_SetMaxStackSize(realm->rt, test_stack_limit());
    realm->ctx = JS_NewContext(realm->rt);
    if (realm->ctx == NULL) return false;
    JSValue global = JS_GetGlobalObject(realm->ctx);
    int set = JS_SetPropertyStr(realm->ctx, global, "__utf8Parse",
                                JS_NewCFunction(realm->ctx, utf8_parse,
                                                "__utf8Parse", 1));
    JS_FreeValue(realm->ctx, global);
    return set >= 0;
}

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

static void report_exception(JSContext *ctx, const char *name)
{
    JSValue exception = JS_GetException(ctx);
    const char *text = JS_ToCString(ctx, exception);
    fprintf(stderr, "%s: exception %s\n", name, text ? text : "?");
    JS_FreeCString(ctx, text);
    JS_FreeValue(ctx, exception);
}

/* Evaluates a script that returns "ok" or a description of what failed. */
static int expect_ok(JSContext *ctx, const char *name, const char *source)
{
    JSValue value = JS_Eval(ctx, source, strlen(source), name,
                            JS_EVAL_TYPE_GLOBAL);
    int failed = 1;
    if (JS_IsException(value)) {
        report_exception(ctx, name);
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

/* Shared helpers: same() is deep equality with Object.is for primitives,
   key order, descriptors and prototypes; outcome() is a value or an
   error's name, message and first stack line (the "<input>:line:col"
   frame); check() compares JSON.parse with __utf8Parse. */
#define JSON_TEST_PRELUDE                                                    \
    "const errs = []; let checked = 0;"                                      \
    "const fail = (m) => { if (errs.length < 12) errs.push(m); };"           \
    "const eq = (a, b, m) => { if (!Object.is(a, b))"                        \
    "  fail(m + ': ' + String(a) + ' !== ' + String(b)); };"                 \
    "const show = (s) => typeof s === 'string' ?"                            \
    "  escape(s.length > 48 ? s.slice(0, 48) + '...' : s) : String(s);"      \
    "function same(a, b) {"                                                  \
    "  if (typeof a !== typeof b) return false;"                             \
    "  if (a === null || typeof a !== 'object') return Object.is(a, b);"     \
    "  if (Array.isArray(a) !== Array.isArray(b)) return false;"             \
    "  if (Object.getPrototypeOf(a) !== Object.getPrototypeOf(b)) return false;" \
    "  const ka = Reflect.ownKeys(a), kb = Reflect.ownKeys(b);"              \
    "  if (ka.length !== kb.length) return false;"                           \
    "  for (let i = 0; i < ka.length; i++) {"                                \
    "    if (ka[i] !== kb[i]) return false;"                                 \
    "    const da = Object.getOwnPropertyDescriptor(a, ka[i]);"              \
    "    const db = Object.getOwnPropertyDescriptor(b, kb[i]);"              \
    "    if (da.enumerable !== db.enumerable || da.writable !== db.writable" \
    "        || da.configurable !== db.configurable) return false;"          \
    "    if (!same(da.value, db.value)) return false;"                       \
    "  }"                                                                    \
    "  return true;"                                                         \
    "}"                                                                      \
    "function outcome(f, s) {"                                               \
    "  try { return { ok: true, v: f(s) }; } catch (e) {"                    \
    "    return { ok: false, name: e && e.name, msg: e && e.message,"        \
    "             at: e && typeof e.stack === 'string' ?"                    \
    "                 e.stack.split('\\n')[0] : '' };"                       \
    "  }"                                                                    \
    "}"                                                                      \
    "function check(s, label) {"                                             \
    "  checked++;"                                                           \
    "  const a = outcome(JSON.parse, s), b = outcome(__utf8Parse, s);"       \
    "  if (a.ok !== b.ok) {"                                                 \
    "    fail(label + ' ' + show(s) + ': parsed ' + a.ok + ' vs ' + b.ok +"  \
    "         ' (' + (a.ok ? b.msg : a.msg) + ')');"                         \
    "  } else if (a.ok) {"                                                   \
    "    if (!same(a.v, b.v)) fail(label + ' ' + show(s) + ': value');"      \
    "  } else if (a.name !== b.name || a.msg !== b.msg || a.at !== b.at) {"  \
    "    fail(label + ' ' + show(s) + ': ' + [a.name, a.msg, a.at].join('|') +" \
    "         ' vs ' + [b.name, b.msg, b.at].join('|'));"                    \
    "  }"                                                                    \
    "}"

/* Documents, each parsed as given and wrapped in an array beside a
   Latin-1 and a UTF-16 element (so an ASCII document is also read as 8-bit
   non-ASCII and as 16-bit text), then truncated at every character and
   with every character replaced by each of a set of significant ones. */
static const char differential_source[] =
    "(() => {"
    JSON_TEST_PRELUDE
    "const docs = ["
    "  '{\"a\":1,\"b\":[true,false,null],\"c\":\"x\\\\ny\",\"d\":-0,\"e\":1.5e3,"
    "\"f\":{\"g\":\"\\\\u00e9\\\\ud83d\\\\ude00\"}}',"
    "  '[1,-2,3.25,-0.5,1e-7,1E+21,123456789,1234567890,-1234567890,"
    "9007199254740993,0.1,4.35,5e-324,1e400,-1e400,0,-0,-0.0,100e-2]',"
    "  '{\"__proto__\":{\"x\":1},\"a\":\"__proto__\",\"a\":2}',"
    "  ' \\t\\n\\r[ \"\\u00e9\", \"\\u4e2d\\u6587\", \"\\\\u4e2d\", \"\\ud83d\\ude00\","
    " \"\\\\ud800\", \"\\\\udc00x\", \"a\\\\u0000b\","
    " \"\\\\/\\\\\\\\\\\\\"\\\\b\\\\f\\\\n\\\\r\\\\t\" ] \\n',"
    "  '{\"k\\u00e9y\":\"v\\u00e9\",\"\\u4e2d\":\"\\ud83d\\ude00\","
    "\"lone\":\"\\ud800\",\"lone2\":\"\\udc00\\ud800\",\"\\u00ff\":\"\\u0100\"}',"
    "  '\"top-level string\"', '42', '-0', 'true', 'false', 'null', '\"\"',"
    "  '[]', '{}', '[[[[{\"a\":[[{}]]}]]]]', '1.5', '-12345678901234567890',"
    "  '{\"a\" : [ 1 , 2 ] , \"b\" : { } }',"
    "];"
    "const subs = ['\"', '\\\\', '\\n', ' ', '0', '9', '-', '+', '.', 'e',"
    "  '}', ']', '{', '[', ',', ':', 'u', 'x', 'n', 't', '/', '\\'',"
    "  '\\u00e9', '\\u00ff', '\\u0100', '\\u4e2d', '\\ud800', '\\udc00',"
    "  '\\0', '\\x01', '\\x7f', '\\u2028'];"
    "const forms = [];"
    "for (const d of docs) {"
    "  forms.push(d, '[' + d + ',\"\\u00e9\"]', '[' + d + ',\"\\u4e2d\"]');"
    "}"
    "for (const d of forms) {"
    "  check(d, 'doc');"
    "  for (let k = 0; k < d.length; k++) check(d.slice(0, k), 'prefix');"
    "  for (let j = 0; j < d.length; j++)"
    "    for (const r of subs) check(d.slice(0, j) + r + d.slice(j + 1), 'sub');"
    "}"
    /* Errors on later lines and columns after astral characters */
    "for (const s of ['[1,\\n  2,\\n  x]', '[\"\\ud83d\\ude00\", x]',"
    "  '[\"\\ud800\", x]', '[\"\\u00e9\\u00e9\",\\n\\n \\u00e9]',"
    "  '\\n\\n\\n   {\"a\" 1}', '{\"a\":1,}', '[1,]', '[01]', '[1.]', '[1e]',"
    "  '[-]', '[- 1]', '[-\\u00e9]', '[-\\u4e2d]', '[-\\ud83d\\ude00]',"
    "  '[-\\ud800]', '\"\\\\', '\"\\\\u12', '\"\\\\u12x4\"', '\"\\\\x\"',"
    "  '\"\\\\\\u00e9\"', '\"\\\\\\u4e2d\"', '\"abc', '\"a\\nb\"', '[1 2]',"
    "  'undefinedx', 'NaN', 'Infinity', '[nul]', '[truex]', '\\u00e9',"
    "  '\\u4e2d', '\\ufeff[]', '[1]\\u00a0', '[1]\\u2028', '\\u000b[]',"
    "  '\\f[]', \"'a'\", '/**/1', '+1', '.5', '0x10', '[' + 'x'.repeat(300) + ']',"
    "  '[\\u4e2d' + 'x'.repeat(300) + ']', '[' + '\\u4e2dx'.repeat(300) + ']',"
    "  '{\"' + 'k'.repeat(70000) + '\"x}']) check(s, 'error');"
    /* Non-string arguments go through ToString first */
    "for (const v of [123, -0, true, null, undefined, [1, 2], [], {},"
    "  { toString() { return '[1,\"\\u4e2d\"]'; } }]) check(v, 'tostring');"
    "let sym = outcome(JSON.parse, Symbol('s'));"
    "eq(sym.ok === false && sym.name, 'TypeError', 'symbol argument');"
    "eq(checked > 50000, true, 'cases checked');"
    "return errs.join('; ') || 'ok';"
    "})()";

/* Semantics that do not need the old path to state them. */
static const char semantics_source[] =
    "(() => {"
    JSON_TEST_PRELUDE
    /* Surrogates: pairs and lone halves survive both escaped and raw */
    "eq(JSON.parse('\"\\ud83d\\ude00\"'), '\\ud83d\\ude00', 'raw pair');"
    "eq(JSON.parse('\"\\\\ud83d\\\\ude00\"'), '\\ud83d\\ude00', 'escaped pair');"
    "eq(JSON.parse('\"\\ud800\"').charCodeAt(0), 0xd800, 'raw lone high');"
    "eq(JSON.parse('\"\\ud800\"').length, 1, 'raw lone high length');"
    "eq(JSON.parse('\"x\\udc00\"').charCodeAt(1), 0xdc00, 'raw lone low');"
    "eq(JSON.parse('\"\\\\udc00\\\\ud800\"'), '\\udc00\\ud800', 'escaped reversed');"
    "eq(JSON.parse('[\"\\u00e9\\u00ff\"]')[0], '\\u00e9\\u00ff', 'latin-1');"
    "eq(JSON.parse('{\"\\u4e2d\":\"\\u6587\"}')['\\u4e2d'], '\\u6587', 'wide key');"
    /* Numbers */
    "eq(JSON.parse('-0'), -0, 'negative zero');"
    "eq(JSON.parse('[-0]')[0], -0, 'negative zero element');"
    "eq(JSON.parse('999999999'), 999999999, 'nine digits');"
    "eq(JSON.parse('-999999999'), -999999999, 'nine digits negative');"
    "eq(JSON.parse('2147483648'), 2147483648, 'above int32');"
    "eq(JSON.parse('9007199254740993'), 9007199254740992, 'rounding');"
    "eq(JSON.parse('0.1'), 0.1, 'fraction');"
    "eq(JSON.parse('1e400'), Infinity, 'overflow');"
    "eq(JSON.parse('[1E+2]')[0], 100, 'exponent');"
    /* __proto__ is an own data property, never the prototype */
    "let o = JSON.parse('{\"__proto__\":{\"x\":1},\"\\u4e2d\":0}');"
    "eq(Object.getPrototypeOf(o), Object.prototype, '__proto__ prototype');"
    "eq(Object.keys(o).join(), '__proto__,\\u4e2d', '__proto__ own key');"
    "eq(o.x, undefined, '__proto__ not inherited');"
    "o = JSON.parse('{\"a\":1,\"a\":2}'); eq(o.a, 2, 'duplicate key');"
    /* Reviver: order, deletion, replacement and source text, in 8-bit and
       16-bit input; the source of a replaced holder value is withheld */
    "for (const tail of ['', '\\u00e9', '\\u4e2d']) {"
    "  const seen = [];"
    "  const text = '{\"a\":[1.50,\"' + tail + 'x\",true,null],\"b\":-0,"
    "\"c\":{\"d\":1e2},\"z\":\"' + tail + '\"}';"
    "  const v = JSON.parse(text, function (k, val, ctx) {"
    "    seen.push(k + '=' + (ctx && 'source' in ctx ? ctx.source : '-'));"
    "    if (k === 'd') return undefined;"
    "    if (k === 'b') this.z = 'changed';"
    "    return k === '1' ? val + '!' : val; });"
    "  eq(seen.join(' '), '0=1.50 1=\"' + tail + 'x\" 2=true 3=null a=- b=-0"
    " d=1e2 c=- z=- =-', 'reviver ' + escape(tail));"
    "  eq(v.a[1], tail + 'x!', 'reviver replace ' + escape(tail));"
    "  eq('d' in v.c, false, 'reviver delete ' + escape(tail));"
    "  eq(v.z, 'changed', 'reviver holder ' + escape(tail));"
    "}"
    /* Errors: messages and positions */
    "const at = (s) => { const r = outcome(JSON.parse, s);"
    "  return r.ok ? 'ok' : r.name + ': ' + r.msg + r.at.replace(/^ *at <input>/, ' @'); };"
    "eq(at('[1,\\n  2,\\n  x]'), \"SyntaxError: unexpected token: 'x' @:3:3\", 'line 3');"
    "eq(at('[\"\\ud83d\\ude00\", x]'), \"SyntaxError: unexpected token: 'x' @:1:7\", 'astral column');"
    "eq(at('[\"\\ud800\", x]'), \"SyntaxError: unexpected token: 'x' @:1:7\", 'lone column');"
    "eq(at('[\\u4e2d]'), 'SyntaxError: unexpected character @:1:2', 'wide character');"
    "eq(at('[\"a'), 'SyntaxError: Unexpected end of JSON input @:1:2', 'open string');"
    "eq(at('\"\\\\q\"'), 'SyntaxError: Bad escaped character @:1:3', 'escape');"
    "eq(at('{\"a\":1,}'), 'SyntaxError: expecting property name @:1:8', 'trailing comma');"
    "eq(at('[01]'), 'SyntaxError: Unexpected number @:1:2', 'leading zero');"
    "eq(at(''), 'SyntaxError: Unexpected end of JSON input @:1:1', 'empty');"
    "eq(at('[1] x'), 'SyntaxError: unexpected data at the end @:1:5', 'trailing data');"
    "eq(at('[' + 'y'.repeat(300) + '\\u4e2d]'), at('[' + 'y'.repeat(300) + ']'),"
    "   'long identifier, 8 and 16 bit');"
    /* Nesting: deep input parses or fails the same way on both paths */
    /* (compared without recursion: same() would overflow the stack) */
    "const spine = (v) => { let d = 0; for (;;) {"
    "  if (Array.isArray(v) && v.length) v = v[0];"
    "  else if (v && typeof v === 'object' && 'a' in v) v = v.a;"
    "  else return d + ':' + JSON.stringify(v); d++; } };"
    "const nest = (s, label) => {"
    "  const a = outcome(JSON.parse, s), b = outcome(__utf8Parse, s);"
    "  if (a.ok !== b.ok) fail(label + ': parsed ' + a.ok + ' vs ' + b.ok);"
    "  else if (a.ok ? spine(a.v) !== spine(b.v) : a.msg !== b.msg || a.name !== b.name)"
    "    fail(label + ': ' + (a.ok ? spine(a.v) : a.msg) + ' vs ' + (b.ok ? spine(b.v) : b.msg)); };"
    "for (const n of [10, 100, 300, 30000]) {"
    "  nest('['.repeat(n) + ']'.repeat(n), 'nest ' + n);"
    "  nest('['.repeat(n) + '\"\\u4e2d\"' + ']'.repeat(n), 'nest wide ' + n);"
    "  nest('{\"a\":'.repeat(n) + '1' + '}'.repeat(n), 'nest object ' + n);"
    "}"
    "const depth = (f, mid) => { let lo = 1, hi = 1 << 20;"
    "  while (lo < hi) { const m = (lo + hi + 1) >> 1;"
    "    if (outcome(f, '['.repeat(m) + mid + ']'.repeat(m)).ok) lo = m;"
    "    else hi = m - 1; } return lo; };"
    "const d8 = depth(JSON.parse, ''), dw = depth(JSON.parse, '\"\\u4e2d\"');"
    "const du = depth(__utf8Parse, '');"
    "eq(Math.abs(d8 - du) <= 2 && Math.abs(dw - du) <= 2, true,"
    "   'nesting limit ' + d8 + '/' + dw + ' vs ' + du);"
    "eq(d8 > 300, true, 'nesting at least 300 deep: ' + d8);"
    /* JSON.rawJSON parses its text in place too */
    "eq(JSON.isRawJSON(JSON.rawJSON('\"\\u4e2d\"')), true, 'rawJSON wide');"
    "eq(JSON.stringify({ a: JSON.rawJSON('1e1000') }), '{\"a\":1e1000}', 'rawJSON number');"
    "eq(JSON.stringify([JSON.rawJSON('\"\\u00e9\"')]), '[\"\\u00e9\"]', 'rawJSON latin-1');"
    "eq(outcome(JSON.rawJSON, '{}').name, 'SyntaxError', 'rawJSON object');"
    "eq(outcome(JSON.rawJSON, '\"\\u4e2d').name, 'SyntaxError', 'rawJSON unterminated');"
    /* A rope argument is flattened once and read in place */
    "const rope = '[\"' + 'a'.repeat(9000) + '\",\"' + '\\u4e2d'.repeat(9000) + '\"]';"
    "const pr = JSON.parse(rope);"
    "eq(pr[0].length + pr[1].length, 18000, 'rope');"
    "eq(pr[1].charCodeAt(8999), 0x4e2d, 'rope wide');"
    /* Huge inputs: 8-bit and 16-bit, compared with the old path */
    "for (const ch of ['x', '\\u00e9', '\\u4e2d', '\\ud83d\\ude00']) {"
    "  const items = [];"
    "  for (let i = 0; i < 40000; i++)"
    "    items.push({ i, s: ch + i, f: i / 8, b: (i & 1) === 0, n: null,"
    "                 e: 'q\\n\"' + ch });"
    "  const big = JSON.stringify(items);"
    "  check(big, 'huge ' + escape(ch));"
    "  const v = JSON.parse(big);"
    "  eq(v.length, 40000, 'huge length ' + escape(ch));"
    "  eq(v[39999].s, ch + 39999, 'huge string ' + escape(ch));"
    "  eq(v[12345].f, 12345 / 8, 'huge number ' + escape(ch));"
    "  eq(v[7].e, 'q\\n\"' + ch, 'huge escape ' + escape(ch));"
    "}"
    "const long = '\\u4e2d'.repeat(1 << 20);"
    "eq(JSON.parse('\"' + long + '\"'), long, 'one long string');"
    "eq(JSON.parse('\"' + 'x'.repeat(1 << 21) + '\"').length, 1 << 21, 'one long latin-1 string');"
    "return errs.join('; ') || 'ok';"
    "})()";

static int run_script(const char *name, const char *source)
{
    Realm realm;
    int failed = 0;
    if (!realm_open(&realm, 256u * MIB)) {
        fprintf(stderr, "%s: realm\n", name);
        failed = 1;
    } else {
        failed |= expect_ok(realm.ctx, name, source);
    }
    if (!realm_close(&realm)) {
        fprintf(stderr, "%s: memory not returned\n", name);
        failed = 1;
    }
    return failed;
}

/* Peak Budget bytes above the level before `call` ran, or 0 if it
   failed. */
static size_t parse_peak(Realm *realm, const char *call)
{
    JS_RunGC(realm->rt);
    (void) budget_quickjs_pool_trim(realm->pool, 0);
    size_t before = realm->budget.current;
    realm->budget.peak = before;
    JSValue value = JS_Eval(realm->ctx, call, strlen(call), "<peak>",
                            JS_EVAL_TYPE_GLOBAL);
    size_t peak = realm->budget.peak - before;
    if (JS_IsException(value)) {
        report_exception(realm->ctx, call);
        peak = 0;
    }
    JS_FreeValue(realm->ctx, value);
    return peak;
}

/* Calls `call` under a Budget limit `headroom` bytes above the current
   level; returns 1 if it returned true, 0 if it was refused for memory,
   -1 for any other outcome. */
static int parse_within(Realm *realm, const char *call, size_t headroom)
{
    JS_RunGC(realm->rt);
    (void) budget_quickjs_pool_trim(realm->pool, 0);
    size_t saved = realm->budget.limit;
    realm->budget.limit = realm->budget.current + headroom;
    JSValue value = JS_Eval(realm->ctx, call, strlen(call), "<within>",
                            JS_EVAL_TYPE_GLOBAL);
    realm->budget.limit = saved;
    int result = -1;
    if (!JS_IsException(value)) {
        result = JS_ToBool(realm->ctx, value) == 1 ? 1 : -1;
    } else {
        /* The engine throws null when even the error object is refused */
        JSValue exception = JS_GetException(realm->ctx);
        const char *text = JS_ToCString(realm->ctx, exception);
        if (JS_IsNull(exception) ||
            (text != NULL && strstr(text, "out of memory") != NULL))
            result = 0;
        else fprintf(stderr, "%s: %s\n", call, text ? text : "?");
        JS_FreeCString(realm->ctx, text);
        JS_FreeValue(realm->ctx, exception);
    }
    JS_FreeValue(realm->ctx, value);
    return result;
}

/* The field failure: a page's large JSON.parse refused because the UTF-8
   copy of its input did not fit. Builds a document like the ones that ran
   out of memory (records with text), measures the peak of the in-place
   parse and of the old copy-then-parse path, and gives the heap headroom
   halfway between: the in-place parse must succeed and the old path must
   be refused. For 16-bit text the copy is three bytes per character
   reserved up front; for Latin-1 text it is the length plus one byte per
   non-ASCII character. The third document is one long escaped string
   (embedded markup), whose buffer the in-place parse sizes once instead of
   growing it by reallocation; the fourth is ASCII, read in place by both
   paths, and checks that buffer is sized to the decoded, not the raw,
   length. Each build leaves `text` and `expect` (a
   check of the parsed value). */
static int run_bounded_heap(void)
{
    static const struct {
        const char *name;
        const char *build;
    } inputs[] = {
        {"utf16",
         "var items = []; for (let i = 0; i < 20000; i++) items.push({ id: i,"
         " title: '\\u4e2d\\u6587 \\u6807\\u9898 ' + i + ' '.repeat(40), n: null });"
         " var text = JSON.stringify(items); items = null;"
         " var expect = (v) => v.length === 20000 && v[7].n === null;"},
        {"latin1",
         "var items = []; for (let i = 0; i < 20000; i++) items.push({ id: i,"
         " title: 'caf\\u00e9 cr\\u00e8me \\u00e0 ' + i + '\\u00e9'.repeat(40), n: null });"
         " var text = JSON.stringify(items); items = null;"
         " var expect = (v) => v.length === 20000 && v[7].n === null;"},
        {"escaped",
         "var text = JSON.stringify({ html: '<p class=\"a\">\\u4e2d\\u6587</p>\\n'"
         ".repeat(60000) });"
         " var expect = (v) => v.html.length === 60000 * 20"
         " && v.html.charCodeAt(13) === 0x4e2d;"},
        /* ASCII text, which the old path read in place: \u escapes take six
           characters each, so a buffer sized from the raw text would be
           six times too large (a captured page then ran out of memory) */
        {"u-escaped",
         "var text = '{\"s\":\"' + '\\\\u0041\\\\u00e9\\\\n'.repeat(150000) + '\"}';"
         " var expect = (v) => v.s.length === 450000"
         " && v.s.charCodeAt(1) === 0xe9;"},
    };
    int failed = 0;
    for (size_t k = 0; k < sizeof(inputs) / sizeof(inputs[0]); k++) {
        Realm realm;
        if (!realm_open(&realm, 256u * MIB)) {
            fprintf(stderr, "bounded heap: realm\n");
            failed = 1;
        } else {
            JSValue built = JS_Eval(realm.ctx, inputs[k].build,
                                    strlen(inputs[k].build), "<build>",
                                    JS_EVAL_TYPE_GLOBAL);
            if (JS_IsException(built)) {
                report_exception(realm.ctx, "build");
                failed = 1;
            }
            JS_FreeValue(realm.ctx, built);
            static const char direct[] = "expect(JSON.parse(text))";
            static const char copy[] = "expect(__utf8Parse(text))";
            /* A warm-up parse flattens a rope input once (the flat form
               is cached in the rope), so neither measured path pays it. */
            (void) parse_peak(&realm, direct);
            size_t direct_peak = parse_peak(&realm, direct);
            size_t copy_peak = parse_peak(&realm, copy);
            size_t headroom = (direct_peak + copy_peak) / 2;
            int direct_fits = parse_within(&realm, direct, headroom);
            int copy_fits = parse_within(&realm, copy, headroom);
            int direct_again = parse_within(&realm, direct, headroom);
            printf("bounded heap %s: in-place peak %zu, UTF-8 copy peak %zu,"
                   " headroom %zu: in-place %s, copy %s\n",
                   inputs[k].name, direct_peak, copy_peak, headroom,
                   direct_fits == 1 ? "parsed" : "FAILED",
                   copy_fits == 0 ? "out of memory" : "UNEXPECTED");
            if (direct_peak == 0 || copy_peak <= direct_peak ||
                direct_fits != 1 || copy_fits != 0 || direct_again != 1)
                failed = 1;
        }
        if (!realm_close(&realm)) {
            fprintf(stderr, "bounded heap: memory not returned\n");
            failed = 1;
        }
    }
    return failed;
}

/* Review regressions, each under a fixed heap headroom the in-place parse
   must fit and a wasteful parse would not:
   - a 16-bit string whose only escape comes at its end gets its 16-bit
     buffer at once: 1,000,002 bytes, where a narrow buffer widened at the
     prefix would hold both (1.5 MB);
   - a long number in 8-bit text is converted in place, as the old path
     did for ASCII, not copied (2 MiB here);
   - a long number in 16-bit text is narrowed once, into one allocation of
     its own length (1 MiB) - still far below the old path's 3-byte-per-
     character copy of the whole text.
   The input is built and flattened (one unmeasured parse) first. */
static int run_bounded_exact(void)
{
    static const struct {
        const char *name;
        const char *build;
        const char *call;
        size_t headroom;
    } cases[] = {
        {"wide prefix, trailing escape",
         "var text = '{\"s\":\"' + '\\u4e2d'.repeat(500000) + '\\\\n\"}';",
         "JSON.parse(text).s.length === 500001", 1250u * 1024u},
        {"long number, 8-bit",
         "var text = '[' + '1'.repeat(1 << 21) + ']';",
         "JSON.parse(text)[0] === Infinity", 512u * 1024u},
        {"long number, 16-bit",
         "var text = '[\"\\u4e2d\",' + '1'.repeat(1 << 20) + ']';",
         "JSON.parse(text)[1] === Infinity", 1536u * 1024u},
    };
    int failed = 0;
    for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        Realm realm;
        if (!realm_open(&realm, 256u * MIB)) {
            fprintf(stderr, "bounded exact: realm\n");
            failed = 1;
        } else {
            JSValue built = JS_Eval(realm.ctx, cases[k].build,
                                    strlen(cases[k].build), "<build>",
                                    JS_EVAL_TYPE_GLOBAL);
            if (JS_IsException(built)) {
                report_exception(realm.ctx, "build");
                failed = 1;
            }
            JS_FreeValue(realm.ctx, built);
            (void) parse_peak(&realm, cases[k].call);
            size_t peak = parse_peak(&realm, cases[k].call);
            int fits = parse_within(&realm, cases[k].call, cases[k].headroom);
            printf("bounded heap %s: in-place peak %zu, headroom %zu: %s\n",
                   cases[k].name, peak, cases[k].headroom,
                   fits == 1 ? "parsed" : "FAILED");
            if (fits != 1 || peak == 0 || peak > cases[k].headroom)
                failed = 1;
        }
        if (!realm_close(&realm)) {
            fprintf(stderr, "bounded exact: memory not returned\n");
            failed = 1;
        }
    }
    return failed;
}

/* An allocator whose realloc always moves (allocate, copy, free), as a
   fragmented device heap may: the old and new block are both live, so a
   buffer that is reallocated wider costs both at once. The Budget pool on
   the host often grows a large block in place, which hides that. */
typedef struct {
    size_t current, peak, limit;
} MovingHeap;

static void *moving_malloc(JSMallocState *s, size_t size)
{
    MovingHeap *heap = s->opaque;
    if (heap->current + size > heap->limit) return NULL;
    size_t *block = malloc(sizeof(size_t) * 2 + size);
    if (block == NULL) return NULL;
    block[0] = size;
    heap->current += size;
    if (heap->current > heap->peak) heap->peak = heap->current;
    return block + 2;
}

static void moving_free(JSMallocState *s, void *ptr)
{
    MovingHeap *heap = s->opaque;
    if (ptr == NULL) return;
    size_t *block = (size_t *) ptr - 2;
    heap->current -= block[0];
    free(block);
}

static void *moving_realloc(JSMallocState *s, void *ptr, size_t size)
{
    if (ptr == NULL) return moving_malloc(s, size);
    if (size == 0) {
        moving_free(s, ptr);
        return NULL;
    }
    void *moved = moving_malloc(s, size);
    if (moved == NULL) return NULL;
    size_t old = ((size_t *) ptr)[-2];
    memcpy(moved, ptr, old < size ? old : size);
    moving_free(s, ptr);
    return moved;
}

static size_t moving_usable_size(const void *ptr)
{
    return ptr != NULL ? ((const size_t *) ptr)[-2] : 0;
}

static const JSMallocFunctions moving_functions = {
    moving_malloc, moving_free, moving_realloc, moving_usable_size
};

/* A 16-bit string whose only escape is at its end (500,000 CJK characters
   and "\n") needs its 1,000,002-byte 16-bit buffer once. Sized narrow from
   the escapes alone and widened at the prefix, it needed the narrow
   500,001 bytes and the wide copy together under a moving realloc. */
static int run_moving_realloc(void)
{
    MovingHeap heap = {0, 0, 256u * MIB};
    JSRuntime *rt = JS_NewRuntime2(&moving_functions, &heap);
    JSContext *ctx = rt ? JS_NewContext(rt) : NULL;
    int failed = ctx == NULL;
    if (!failed) {
        JS_SetMaxStackSize(rt, test_stack_limit());
        static const char build[] =
            "var text = '{\"s\":\"' + '\\u4e2d'.repeat(500000) + '\\\\n\"}';"
            " JSON.parse(text).s.length";
        static const char call[] = "JSON.parse(text).s.length === 500001";
        JSValue v = JS_Eval(ctx, build, strlen(build), "<build>",
                            JS_EVAL_TYPE_GLOBAL);
        failed |= JS_IsException(v);
        JS_FreeValue(ctx, v);
        JS_RunGC(rt);
        size_t before = heap.current;
        heap.peak = before;
        heap.limit = before + 1250u * 1024u;
        v = JS_Eval(ctx, call, strlen(call), "<call>", JS_EVAL_TYPE_GLOBAL);
        int parsed = !JS_IsException(v) && JS_ToBool(ctx, v) == 1;
        if (JS_IsException(v)) report_exception(ctx, "moving realloc");
        JS_FreeValue(ctx, v);
        heap.limit = 256u * MIB;
        printf("moving realloc, wide prefix and trailing escape: peak %zu,"
               " headroom %u: %s\n", heap.peak - before, 1250u * 1024u,
               parsed ? "parsed" : "FAILED");
        failed |= !parsed;
    }
    if (ctx) JS_FreeContext(ctx);
    if (rt) JS_FreeRuntime(rt);
    if (heap.current != 0) {
        fprintf(stderr, "moving realloc: %zu bytes not returned\n", heap.current);
        failed = 1;
    }
    return failed;
}

int main(void)
{
    int failed = 0;
    failed |= run_script("<differential>", differential_source);
    failed |= run_script("<semantics>", semantics_source);
    failed |= run_bounded_heap();
    failed |= run_bounded_exact();
    failed |= run_moving_realloc();
    puts(failed ? "QuickJS JSON.parse: FAIL" : "QuickJS JSON.parse: PASS");
    return failed;
}
