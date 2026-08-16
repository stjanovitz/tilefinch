#include "tilefinch/native_trace.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include <lightning.h>

#define NATIVE_TRACE_STACK_CAPACITY 64

typedef struct {
    const JSValue *argv;
    JSValue result;
    int32_t stack[NATIVE_TRACE_STACK_CAPACITY];
} NativeTraceFrame;

typedef int (*NativeTraceEntry)(NativeTraceFrame *frame);

struct NativeTrace {
    jit_state_t *state;
    NativeTraceEntry entry;
    unsigned int required_arg_count;
};

static int jit_initialized;

static int native_trace_make_executable(jit_state_t *_jit)
{
    jit_word_t code_size = 0;
    unsigned char *code = jit_get_code(&code_size);
    if (code == NULL || code_size <= 0)
        return 0;

#if defined(__PSP__)
    /* The PSP backend will use the SDK cache APIs once linked into the target
       build. Keeping this branch explicit prevents desktop cache assumptions
       from silently reaching MIPS. */
    (void) code;
    return 0;
#else
    long page_size_value = sysconf(_SC_PAGESIZE);
    if (page_size_value <= 0)
        return 0;
    uintptr_t page_size = (uintptr_t)page_size_value;
    uintptr_t start = (uintptr_t)code & ~(page_size - 1);
    uintptr_t end = ((uintptr_t)code + (uintptr_t)code_size + page_size - 1) &
                    ~(page_size - 1);
    if (mprotect((void *)start, end - start, PROT_READ | PROT_EXEC) != 0)
        return 0;
    __builtin___clear_cache((char *)code, (char *)code + code_size);
    return 1;
#endif
}

static jit_node_t *native_trace_guard_arg_int(jit_state_t *_jit,
                                               int argument_index)
{
    const jit_word_t value_offset =
        (jit_word_t)offsetof(NativeTraceFrame, argv);
    jit_ldxi(JIT_R1, JIT_R0, value_offset);
    jit_addi(JIT_R1, JIT_R1,
             (jit_word_t)argument_index * (jit_word_t)sizeof(JSValue));
#ifdef JS_NAN_BOXING
    jit_ldxi_i(JIT_R2, JIT_R1, (jit_word_t)sizeof(uint32_t));
#else
    jit_ldxi_i(JIT_R2, JIT_R1, (jit_word_t)offsetof(JSValue, tag));
#endif
    return jit_bnei(JIT_R2, JS_TAG_INT);
}

static void native_trace_load_arg_value(jit_state_t *_jit,
                                        int argument_index,
                                        int stack_index)
{
    const jit_word_t argv_offset =
        (jit_word_t)offsetof(NativeTraceFrame, argv);
    const jit_word_t stack_offset =
        (jit_word_t)offsetof(NativeTraceFrame, stack) +
        (jit_word_t)stack_index * (jit_word_t)sizeof(int32_t);
    jit_ldxi(JIT_R1, JIT_R0, argv_offset);
    jit_ldxi_i(JIT_R2, JIT_R1,
               (jit_word_t)argument_index * (jit_word_t)sizeof(JSValue));
    jit_stxi_i(stack_offset, JIT_R0, JIT_R2);
}

static void native_trace_store_constant(jit_state_t *_jit, int stack_index,
                                        int32_t value)
{
    const jit_word_t offset =
        (jit_word_t)offsetof(NativeTraceFrame, stack) +
        (jit_word_t)stack_index * (jit_word_t)sizeof(int32_t);
    jit_movi(JIT_R1, value);
    jit_stxi_i(offset, JIT_R0, JIT_R1);
}

static void native_trace_binary(jit_state_t *_jit, NativeTraceOpcode opcode,
                                int left_index, int right_index)
{
    const jit_word_t base = (jit_word_t)offsetof(NativeTraceFrame, stack);
    const jit_word_t left = base +
        (jit_word_t)left_index * (jit_word_t)sizeof(int32_t);
    const jit_word_t right = base +
        (jit_word_t)right_index * (jit_word_t)sizeof(int32_t);
    jit_ldxi_i(JIT_R1, JIT_R0, left);
    jit_ldxi_i(JIT_R2, JIT_R0, right);
    switch (opcode) {
    case NATIVE_TRACE_ADD_I32: jit_addr(JIT_R1, JIT_R1, JIT_R2); break;
    case NATIVE_TRACE_SUB_I32: jit_subr(JIT_R1, JIT_R1, JIT_R2); break;
    case NATIVE_TRACE_MUL_I32: jit_mulr(JIT_R1, JIT_R1, JIT_R2); break;
    case NATIVE_TRACE_XOR_I32: jit_xorr(JIT_R1, JIT_R1, JIT_R2); break;
    case NATIVE_TRACE_AND_I32: jit_andr(JIT_R1, JIT_R1, JIT_R2); break;
    case NATIVE_TRACE_OR_I32: jit_orr(JIT_R1, JIT_R1, JIT_R2); break;
    case NATIVE_TRACE_SHL_I32:
        jit_andi(JIT_R2, JIT_R2, 31);
        jit_lshr(JIT_R1, JIT_R1, JIT_R2);
        break;
    case NATIVE_TRACE_SAR_I32:
        jit_andi(JIT_R2, JIT_R2, 31);
        jit_rshr(JIT_R1, JIT_R1, JIT_R2);
        break;
    case NATIVE_TRACE_SHR_I32:
        jit_andi(JIT_R2, JIT_R2, 31);
        jit_rshr_u(JIT_R1, JIT_R1, JIT_R2);
        break;
    default: abort();
    }
    jit_stxi_i(left, JIT_R0, JIT_R1);
}

NativeTrace *native_trace_compile(const NativeTraceInstruction *instructions,
                                  size_t instruction_count,
                                  unsigned int required_arg_count)
{
    if (instructions == NULL || instruction_count == 0 ||
        required_arg_count > INT_MAX)
        return NULL;
    if (!jit_initialized) {
        init_jit("psp-browser-native-trace");
        jit_initialized = 1;
    }

    NativeTrace *trace = calloc(1, sizeof(*trace));
    if (trace == NULL)
        return NULL;
    trace->required_arg_count = required_arg_count;

    jit_state_t *_jit = jit_new_state();
    if (_jit == NULL) {
        free(trace);
        return NULL;
    }
    trace->state = _jit;

    jit_node_t *argument;
    jit_node_t *guard_failures[NATIVE_TRACE_STACK_CAPACITY];
    size_t guard_failure_count = 0;
    int stack_depth = 0;
    int saw_return = 0;

    jit_prolog();
    argument = jit_arg();
    jit_getarg(JIT_R0, argument);

    for (size_t index = 0; index < instruction_count; index++) {
        NativeTraceInstruction instruction = instructions[index];
        switch (instruction.opcode) {
        case NATIVE_TRACE_GET_ARG:
            if (instruction.operand < 0 ||
                (unsigned int)instruction.operand >= required_arg_count ||
                stack_depth >= NATIVE_TRACE_STACK_CAPACITY ||
                guard_failure_count >= NATIVE_TRACE_STACK_CAPACITY)
                goto invalid;
            guard_failures[guard_failure_count++] =
                native_trace_guard_arg_int(_jit, instruction.operand);
            native_trace_load_arg_value(_jit, instruction.operand,
                                        stack_depth++);
            break;
        case NATIVE_TRACE_CONST_I32:
            if (stack_depth >= NATIVE_TRACE_STACK_CAPACITY)
                goto invalid;
            native_trace_store_constant(_jit, stack_depth++,
                                        instruction.operand);
            break;
        case NATIVE_TRACE_ADD_I32:
        case NATIVE_TRACE_SUB_I32:
        case NATIVE_TRACE_MUL_I32:
        case NATIVE_TRACE_XOR_I32:
        case NATIVE_TRACE_AND_I32:
        case NATIVE_TRACE_OR_I32:
        case NATIVE_TRACE_SHL_I32:
        case NATIVE_TRACE_SAR_I32:
        case NATIVE_TRACE_SHR_I32:
            if (stack_depth < 2)
                goto invalid;
            native_trace_binary(_jit, instruction.opcode,
                                stack_depth - 2, stack_depth - 1);
            stack_depth--;
            break;
        case NATIVE_TRACE_RETURN_I32:
            if (stack_depth != 1 || index + 1 != instruction_count)
                goto invalid;
            jit_ldxi_i(JIT_R1, JIT_R0,
                       (jit_word_t)offsetof(NativeTraceFrame, stack));
#ifdef JS_NAN_BOXING
            jit_stxi_i((jit_word_t)offsetof(NativeTraceFrame, result),
                       JIT_R0, JIT_R1);
            jit_movi(JIT_R2, JS_TAG_INT);
            jit_stxi_i((jit_word_t)offsetof(NativeTraceFrame, result) +
                       (jit_word_t)sizeof(uint32_t), JIT_R0, JIT_R2);
#else
            jit_stxi_i((jit_word_t)offsetof(NativeTraceFrame, result) +
                       (jit_word_t)offsetof(JSValue, u), JIT_R0, JIT_R1);
            jit_movi(JIT_R2, JS_TAG_INT);
            jit_stxi((jit_word_t)offsetof(NativeTraceFrame, result) +
                     (jit_word_t)offsetof(JSValue, tag), JIT_R0, JIT_R2);
#endif
            jit_movi(JIT_R1, 1);
            jit_retr(JIT_R1);
            saw_return = 1;
            break;
        }
    }
    if (!saw_return)
        goto invalid;

    jit_node_t *failure = jit_label();
    for (size_t index = 0; index < guard_failure_count; index++)
        jit_patch_at(guard_failures[index], failure);
    jit_movi(JIT_R1, 0);
    jit_retr(JIT_R1);

    trace->entry = (NativeTraceEntry)jit_emit();
    if (trace->entry == NULL || !native_trace_make_executable(_jit))
        goto invalid;
    return trace;

invalid:
    jit_destroy_state();
    free(trace);
    return NULL;
}

void native_trace_destroy(NativeTrace *trace)
{
    if (trace == NULL)
        return;
    jit_state_t *_jit = trace->state;
    jit_destroy_state();
    free(trace);
}

int native_trace_run(const NativeTrace *trace, int argc,
                     const JSValue *argv, JSValue *result)
{
    if (trace == NULL || argv == NULL || result == NULL || argc < 0 ||
        (unsigned int)argc < trace->required_arg_count)
        return 0;
    NativeTraceFrame frame = {
        .argv = argv,
        .result = JS_UNDEFINED,
        .stack = {0},
    };
    if (!trace->entry(&frame))
        return 0;
    *result = frame.result;
    return 1;
}
