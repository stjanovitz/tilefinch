#ifndef TILEFINCH_NATIVE_TRACE_H
#define TILEFINCH_NATIVE_TRACE_H

#include <stddef.h>
#include <stdint.h>

#include <quickjs.h>

typedef enum {
    NATIVE_TRACE_GET_ARG,
    NATIVE_TRACE_CONST_I32,
    NATIVE_TRACE_ADD_I32,
    NATIVE_TRACE_SUB_I32,
    NATIVE_TRACE_MUL_I32,
    NATIVE_TRACE_XOR_I32,
    NATIVE_TRACE_AND_I32,
    NATIVE_TRACE_OR_I32,
    NATIVE_TRACE_SHL_I32,
    NATIVE_TRACE_SAR_I32,
    NATIVE_TRACE_SHR_I32,
    NATIVE_TRACE_RETURN_I32,
} NativeTraceOpcode;

typedef struct {
    NativeTraceOpcode opcode;
    int32_t operand;
} NativeTraceInstruction;

typedef struct NativeTrace NativeTrace;

NativeTrace *native_trace_compile(const NativeTraceInstruction *instructions,
                                  size_t instruction_count,
                                  unsigned int required_arg_count);
void native_trace_destroy(NativeTrace *trace);

/* Returns true when all guards passed and writes an owned, non-reference
   result. A false return leaves result untouched so the interpreter can run. */
int native_trace_run(const NativeTrace *trace, int argc,
                     const JSValue *argv, JSValue *result);

#endif
