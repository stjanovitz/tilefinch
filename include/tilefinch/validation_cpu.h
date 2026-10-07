#ifndef TILEFINCH_VALIDATION_CPU_H
#define TILEFINCH_VALIDATION_CPU_H
#include <stdint.h>
/* Numeric batch IDs shared by the reducer and post-window device report. */
enum {
    SCRIPT_RUNTIME_CPU_ADVANCE, SCRIPT_RUNTIME_CPU_PREPARATION,
    SCRIPT_RUNTIME_CPU_CALLBACK, SCRIPT_RUNTIME_CPU_JOBS,
    SCRIPT_RUNTIME_CPU_WEBGL, SCRIPT_RUNTIME_CPU_REFRESH,
    SCRIPT_RUNTIME_CPU_PHASES
};
typedef struct {
    uint64_t inclusive_cpu_us[SCRIPT_RUNTIME_CPU_PHASES];
    uint64_t exclusive_cpu_us[SCRIPT_RUNTIME_CPU_PHASES];
    uint64_t calls[SCRIPT_RUNTIME_CPU_PHASES];
    uint64_t observer_us, maximum_query_us;
    uint32_t errors;
} ScriptRuntimeCpuMetrics;
#endif
