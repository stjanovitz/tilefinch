/* Pinned atom tables (JS_PinAtomTable, JS_WRITE_OBJ_PINNED_ATOMS).

   Bytecode restored with JS_READ_OBJ_ROM_DATA runs in place only when it
   has no atoms to relocate. The bootstrap pins the atoms its modules name
   at JS_ATOM_END and up, first in every runtime, and writes the modules
   naming them by index. These cases cover what that relies on: the pinned
   output runs from ROM (no function code in the heap) and behaves as the
   ordinary serialization does, wide (UTF-16) atoms survive, an atom the
   table lacks still relocates, a runtime without the same table refuses the
   bytecode instead of misreading it, and a table cannot be pinned once the
   runtime has interned other atoms or when it repeats one. */
#include "tilefinch/budget.h"
#include "tilefinch/budget_quickjs.h"

#include <quickjs.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "check failed at %s:%d: %s\n",                     \
                __FILE__, __LINE__, #condition);                             \
        return 1;                                                            \
    }                                                                        \
} while (0)

/* Names only the table holds, a wide one among them, and one ("extra")
   it does not. */
static const char program[] =
    "(() => { const record = { alphaName: 1, betaName: 2 };"
    " record['\xce\xbb\xce\xbf\xce\xb3\xce\xbf\xcf\x82'] = 3;"
    " function gammaHelper(entry) { return entry.alphaName + entry.betaName"
    " + entry['\xce\xbb\xce\xbf\xce\xb3\xce\xbf\xcf\x82']; }"
    " globalThis.pinnedResult = gammaHelper(record) + ':' + Object.keys(record)"
    ".join(',') + ':' + typeof globalThis.extra; })()";

/* The table in the bytecode string encoding: leb128 count, then per atom
   leb128(length << 1 | wide) and its Latin-1 bytes or UTF-16LE units. */
static size_t build_table(unsigned char *table, bool repeat)
{
    static const char *const narrow[] = {
        "record", "alphaName", "betaName", "gammaHelper", "entry",
        "pinnedResult"};
    static const uint16_t wide[] = {0x03bb, 0x03bf, 0x03b3, 0x03bf, 0x03c2};
    size_t at = 1, count = 0;
    for (size_t i = 0; i < sizeof(narrow) / sizeof(narrow[0]); i++) {
        size_t length = strlen(narrow[i]);
        table[at++] = (unsigned char) (length << 1);
        memcpy(table + at, narrow[i], length);
        at += length;
        count++;
    }
    table[at++] = (unsigned char) ((5u << 1) | 1u);
    for (size_t i = 0; i < 5; i++) {
        table[at++] = (unsigned char) (wide[i] & 0xffu);
        table[at++] = (unsigned char) (wide[i] >> 8);
    }
    count++;
    if (repeat) {
        table[at++] = (unsigned char) (6u << 1);
        memcpy(table + at, "record", 6);
        at += 6;
        count++;
    }
    table[0] = (unsigned char) count;
    return at;
}

typedef struct {
    Budget budget;
    BudgetQuickJSPool *pool;
    JSRuntime *runtime;
    JSContext *context;
} Realm;

static bool realm_open(Realm *realm, const unsigned char *table,
                       size_t table_length, bool keep_debug)
{
    memset(realm, 0, sizeof(*realm));
    budget_init(&realm->budget, 8u * 1024u * 1024u);
    realm->pool = budget_quickjs_pool_create(&realm->budget);
    realm->runtime = realm->pool == NULL ? NULL
        : JS_NewRuntime2(budget_quickjs_pool_allocator(), realm->pool);
    if (realm->runtime == NULL) return false;
    if (!keep_debug)
        JS_SetStripInfo(realm->runtime, JS_STRIP_SOURCE | JS_STRIP_DEBUG);
    if (table != NULL
        && JS_PinAtomTable(realm->runtime, table, table_length) != 0)
        return false;
    realm->context = JS_NewContext(realm->runtime);
    return realm->context != NULL;
}

static void realm_close(Realm *realm)
{
    if (realm->context != NULL) JS_FreeContext(realm->context);
    if (realm->runtime != NULL) JS_FreeRuntime(realm->runtime);
    if (realm->pool != NULL) budget_quickjs_pool_destroy(realm->pool);
}

static uint8_t *compile(Realm *realm, int flags, size_t *length)
{
    JSValue function = JS_Eval(realm->context, program, sizeof(program) - 1u,
                               "<pinned>", JS_EVAL_TYPE_GLOBAL
                                   | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(function)) return NULL;
    uint8_t *bytes = JS_WriteObject(realm->context, length, function, flags);
    JS_FreeValue(realm->context, function);
    return bytes;
}

/* Runs `bytes` in `realm` from ROM; returns the result text (static) or
   NULL, and the function code the restore copied into the heap. */
static const char *run(Realm *realm, const uint8_t *bytes, size_t length,
                       int64_t *copied_code)
{
    static char text[128];
    JSMemoryUsage before, after;
    JS_ComputeMemoryUsage(realm->runtime, &before);
    JSValue function = JS_ReadObject(realm->context, bytes, length,
                                     JS_READ_OBJ_BYTECODE
                                         | JS_READ_OBJ_ROM_DATA);
    if (JS_IsException(function)) {
        JS_FreeValue(realm->context, JS_GetException(realm->context));
        return NULL;
    }
    JS_ComputeMemoryUsage(realm->runtime, &after);
    *copied_code = after.js_func_code_size - before.js_func_code_size;
    JSValue value = JS_EvalFunction(realm->context, function);
    if (JS_IsException(value)) {
        JS_FreeValue(realm->context, JS_GetException(realm->context));
        return NULL;
    }
    JS_FreeValue(realm->context, value);
    JSValue global = JS_GetGlobalObject(realm->context);
    JSValue result = JS_GetPropertyStr(realm->context, global, "pinnedResult");
    JS_FreeValue(realm->context, global);
    const char *string = JS_ToCString(realm->context, result);
    JS_FreeValue(realm->context, result);
    if (string == NULL) return NULL;
    snprintf(text, sizeof(text), "%s", string);
    JS_FreeCString(realm->context, string);
    return text;
}

int main(void)
{
    static const char expected[] =
        "6:alphaName,betaName,\xce\xbb\xce\xbf\xce\xb3\xce\xbf\xcf\x82:undefined";
    unsigned char table[256], repeated[256];
    size_t table_length = build_table(table, false);
    size_t repeated_length = build_table(repeated, true);

    /* The ordinary serialization relocates its atoms, so ROM data is
       copied into the heap; the pinned one runs in place. Same result. */
    Realm writer;
    CHECK(realm_open(&writer, table, table_length, false));
    CHECK(JS_GetPinnedAtomCount(writer.runtime) == 7u);
    size_t plain_length = 0, pinned_length = 0;
    uint8_t *plain = compile(&writer, JS_WRITE_OBJ_BYTECODE, &plain_length);
    uint8_t *pinned = compile(&writer, JS_WRITE_OBJ_BYTECODE
                                           | JS_WRITE_OBJ_PINNED_ATOMS,
                              &pinned_length);
    CHECK(plain != NULL && pinned != NULL);
    CHECK(pinned_length < plain_length);

    Realm reader;
    CHECK(realm_open(&reader, table, table_length, false));
    int64_t copied = -1;
    const char *text = run(&reader, plain, plain_length, &copied);
    CHECK(text != NULL && strcmp(text, expected) == 0);
    CHECK(copied > 0);
    copied = -1;
    text = run(&reader, pinned, pinned_length, &copied);
    CHECK(text != NULL && strcmp(text, expected) == 0);
    /* "extra" is not pinned: it relocates, so this program's code is
       copied too. A program naming pinned atoms only runs in place. */
    CHECK(copied > 0);
    realm_close(&reader);

    /* Only pinned and predefined atoms: no relocation, no heap copy. */
    {
        static const char only_pinned[] =
            "globalThis.pinnedResult = [1, 2].join() + typeof gammaHelper";
        JSValue function = JS_Eval(writer.context, only_pinned,
                                   sizeof(only_pinned) - 1u, "<pinned-only>",
                                   JS_EVAL_TYPE_GLOBAL
                                       | JS_EVAL_FLAG_COMPILE_ONLY);
        CHECK(!JS_IsException(function));
        size_t length = 0;
        uint8_t *bytes = JS_WriteObject(writer.context, &length, function,
                                        JS_WRITE_OBJ_BYTECODE
                                            | JS_WRITE_OBJ_PINNED_ATOMS);
        JS_FreeValue(writer.context, function);
        CHECK(bytes != NULL);
        CHECK(realm_open(&reader, table, table_length, false));
        copied = -1;
        text = run(&reader, bytes, length, &copied);
        CHECK(text != NULL && strcmp(text, "1,2undefined") == 0);
        CHECK(copied == 0);
        realm_close(&reader);

        /* A runtime without the table, or with another one, refuses it
           rather than reading pinned indices as its own atoms. */
        CHECK(realm_open(&reader, NULL, 0, false));
        CHECK(run(&reader, bytes, length, &copied) == NULL);
        realm_close(&reader);
        unsigned char other[256];
        size_t other_length = build_table(other, false);
        other[2] ^= 0x20; /* "record" -> "Record" */
        CHECK(realm_open(&reader, other, other_length, false));
        CHECK(run(&reader, bytes, length, &copied) == NULL);
        realm_close(&reader);
        js_free(writer.context, bytes);
    }

    /* The ordinary serialization in a pinned runtime stays portable. */
    CHECK(realm_open(&reader, NULL, 0, false));
    text = run(&reader, plain, plain_length, &copied);
    CHECK(text != NULL && strcmp(text, expected) == 0);
    realm_close(&reader);

    js_free(writer.context, plain);
    js_free(writer.context, pinned);
    realm_close(&writer);

    /* Pinning must come first, cannot repeat, and takes distinct atoms. */
    {
        Budget budget;
        budget_init(&budget, 4u * 1024u * 1024u);
        BudgetQuickJSPool *pool = budget_quickjs_pool_create(&budget);
        CHECK(pool != NULL);
        JSRuntime *runtime =
            JS_NewRuntime2(budget_quickjs_pool_allocator(), pool);
        CHECK(runtime != NULL);
        CHECK(JS_PinAtomTable(runtime, repeated, repeated_length) != 0);
        CHECK(JS_GetPinnedAtomCount(runtime) == 0u);
        CHECK(JS_PinAtomTable(runtime, table, table_length - 1u) != 0);
        CHECK(JS_GetPinnedAtomCount(runtime) == 0u);
        CHECK(JS_PinAtomTable(runtime, table, table_length) == 0);
        CHECK(JS_PinAtomTable(runtime, table, table_length) != 0);
        CHECK(JS_GetPinnedAtomCount(runtime) == 7u);
        JS_FreeRuntime(runtime);
        runtime = JS_NewRuntime2(budget_quickjs_pool_allocator(), pool);
        CHECK(runtime != NULL);
        JSContext *context = JS_NewContext(runtime);
        CHECK(context != NULL);
        JSAtom early = JS_NewAtom(context, "unrelatedEarlyName");
        CHECK(JS_PinAtomTable(runtime, table, table_length) != 0);
        CHECK(JS_GetPinnedAtomCount(runtime) == 0u);
        JS_FreeAtom(context, early);
        JS_FreeContext(context);
        JS_FreeRuntime(runtime);
        budget_quickjs_pool_destroy(pool);
        CHECK(budget.current == 0);
    }
    puts("QuickJS pinned atom tables: PASS");
    return 0;
}
