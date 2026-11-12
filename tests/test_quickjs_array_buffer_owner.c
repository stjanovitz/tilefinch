#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tilefinch/budget.h"
#include "tilefinch/budget_quickjs.h"

typedef struct {
    uint8_t bytes[8];
    unsigned releases;
    unsigned wrong_pointer;
} Owner;

static void release_owner(JSRuntime *runtime, void *opaque, void *pointer)
{
    (void)runtime;
    Owner *owner = opaque;
    owner->releases++;
    if (pointer != owner->bytes) owner->wrong_pointer++;
}

static int one_case(int length, int destination_first, int borrowed,
                    int refuse, const char *method_name)
{
    Budget budget;
    budget_init(&budget, 4u * 1024u * 1024u);
    BudgetQuickJSPool *pool = budget_quickjs_pool_create(&budget);
    if (!pool) return 1;
    JSRuntime *runtime = JS_NewRuntime2(budget_quickjs_pool_allocator(), pool);
    JSContext *context = runtime ? JS_NewContext(runtime) : NULL;
    Owner owner = {{1, 2, 3, 4, 5, 6, 7, 8}, 0, 0};
    JSValue source = JS_UNDEFINED, destination = JS_UNDEFINED;
    JSValue method = JS_UNDEFINED, view = JS_UNDEFINED;
    int failed = 0, allocation_refused = 0;
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "ArrayBuffer owner: %s, length=%d order=%d borrowed=%d refusal=%d (%s)\n", \
            #condition, length, destination_first, borrowed, refuse, method_name); \
    failed = 1; goto done; } } while (0)
    CHECK(context != NULL);
    source = JS_NewArrayBuffer(context, owner.bytes, sizeof(owner.bytes),
                              borrowed ? NULL : release_owner, &owner, 0);
    CHECK(!JS_IsException(source));
    JSValue global = JS_GetGlobalObject(context);
    JSValue constructor = JS_GetPropertyStr(context, global, "Uint8Array");
    JS_FreeValue(context, global);
    view = JS_CallConstructor(context, constructor, 1, &source);
    JS_FreeValue(context, constructor);
    CHECK(!JS_IsException(view));
    if (length == -1) {
        /* An attached buffer still owns its backing until finalization. */
        JS_FreeValue(context, view); view = JS_UNDEFINED;
        JS_FreeValue(context, source); source = JS_UNDEFINED;
    } else if (length == -2) {
        JS_DetachArrayBuffer(context, source);
        JS_DetachArrayBuffer(context, source);
    } else {
        method = JS_GetPropertyStr(context, source, method_name);
        CHECK(JS_IsFunction(context, method));
        JSValue argument = JS_NewInt32(context, length);
        if (refuse) JS_TestRefuseAllocation((unsigned)refuse);
        destination = JS_Call(context, method, source, 1, &argument);
        allocation_refused = refuse && !JS_TestRefusalPending();
        JS_TestRefuseAllocation(0);
        if (allocation_refused) {
            CHECK(JS_IsException(destination));
            JSValue exception = JS_GetException(context);
            JS_FreeValue(context, exception);
            size_t size;
            CHECK(JS_GetArrayBuffer(context, &size, source) == owner.bytes);
            CHECK(size == sizeof(owner.bytes) && owner.releases == 0);
        } else {
            CHECK(!JS_IsException(destination));
            size_t size;
            uint8_t *bytes = JS_GetArrayBuffer(context, &size, destination);
            CHECK(size == (size_t)length);
            CHECK(length == 0 || bytes != NULL);
            for (int i = 0; i < length; i++)
                CHECK(bytes[i] == (i < 8 ? owner.bytes[i] : 0));
            CHECK(owner.releases == (unsigned)(!borrowed && length != 8));
        }
    }
    if (length == -2 || (length >= 0 && !allocation_refused)) {
        int32_t view_length = -1;
        JSValue value = JS_GetPropertyStr(context, view, "length");
        CHECK(JS_ToInt32(context, &view_length, value) == 0);
        JS_FreeValue(context, value);
        CHECK(view_length == 0);
    }
    if (destination_first) {
        JS_FreeValue(context, destination); destination = JS_UNDEFINED;
    }
    JS_FreeValue(context, view); view = JS_UNDEFINED;
    JS_FreeValue(context, source); source = JS_UNDEFINED;
    if (!destination_first && length == 8 && !allocation_refused)
        CHECK(owner.releases == 0);
done:
    if (context) {
        JS_FreeValue(context, view);
        JS_FreeValue(context, source);
        JS_FreeValue(context, destination);
        JS_FreeValue(context, method);
        JS_FreeContext(context);
    }
    if (runtime) JS_FreeRuntime(runtime);
    if (owner.releases != (unsigned)!borrowed || owner.wrong_pointer) {
        fprintf(stderr, "ArrayBuffer release count=%u wrong=%u\n",
                owner.releases, owner.wrong_pointer);
        failed = 1;
    }
    budget_quickjs_pool_destroy(pool);
    if (budget.current != 0) failed = 1;
    return failed;
#undef CHECK
}

int main(void)
{
    const int lengths[] = {-2, -1, 0, 4, 8, 16};
    const char *methods[] = {"transfer", "transferToFixedLength"};
    for (unsigned method = 0; method < 2; method++)
        for (unsigned i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++)
            for (int borrowed = 0; borrowed < 2; borrowed++)
                for (int order = 0; order < 2; order++) {
                    if (one_case(lengths[i], order, borrowed, 0, methods[method])) return 1;
                    if (lengths[i] >= 0)
                        for (int refusal = 1; refusal <= 5; refusal++)
                            if (one_case(lengths[i], order, borrowed, refusal,
                                         methods[method])) return 1;
                }
    puts("QuickJS external ArrayBuffer ownership: PASS");
    return 0;
}
