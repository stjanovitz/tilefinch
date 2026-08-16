#include "tilefinch/native_trace.h"

#include <stdio.h>

int main(void)
{
    const NativeTraceInstruction program[] = {
        {NATIVE_TRACE_GET_ARG, 0},
        {NATIVE_TRACE_CONST_I32, 17},
        {NATIVE_TRACE_MUL_I32, 0},
        {NATIVE_TRACE_GET_ARG, 1},
        {NATIVE_TRACE_XOR_I32, 0},
        {NATIVE_TRACE_CONST_I32, 255},
        {NATIVE_TRACE_AND_I32, 0},
        {NATIVE_TRACE_RETURN_I32, 0},
    };
    NativeTrace *trace = native_trace_compile(
        program, sizeof(program) / sizeof(program[0]), 2);
    if (trace == NULL)
        return 1;

    JSValue args[] = {JS_NewInt32(NULL, 9), JS_NewInt32(NULL, 0x55)};
    JSValue result = JS_UNDEFINED;
    if (!native_trace_run(trace, 2, args, &result) ||
        JS_VALUE_GET_TAG(result) != JS_TAG_INT ||
        JS_VALUE_GET_INT(result) != (((9 * 17) ^ 0x55) & 255)) {
        native_trace_destroy(trace);
        return 2;
    }

    args[0] = JS_UNDEFINED;
    result = JS_NewInt32(NULL, 123);
    if (native_trace_run(trace, 2, args, &result) ||
        JS_VALUE_GET_INT(result) != 123) {
        native_trace_destroy(trace);
        return 3;
    }

    native_trace_destroy(trace);

    JSRuntime *runtime = JS_NewRuntime();
    JSContext *context = runtime == NULL ? NULL : JS_NewContext(runtime);
    if (context == NULL) {
        JS_FreeRuntime(runtime);
        return 4;
    }
    const char source[] =
        "function bitmix(a,b){return ((((((a^b)&255)<<2)^a)&1023)<<1);}" 
        "let ok=true;"
        "function expected(a,b){return ((((((a^b)&255)<<2)^a)&1023)<<1);}" 
        "for(let i=0;i<200;i++)ok=ok&&bitmix(i,0x55)===expected(i,0x55);"
        "ok&&bitmix('9',0x55)===expected(9,0x55);";
    JSValue evaluated = JS_Eval(context, source, sizeof(source) - 1,
                                "<native-trace-test>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(evaluated) || JS_VALUE_GET_TAG(evaluated) != JS_TAG_BOOL ||
        !JS_VALUE_GET_BOOL(evaluated)) {
        JS_FreeValue(context, evaluated);
        JS_FreeContext(context);
        JS_FreeRuntime(runtime);
        return 5;
    }
    JS_FreeValue(context, evaluated);
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    puts("native-trace:ok");
    return 0;
}
