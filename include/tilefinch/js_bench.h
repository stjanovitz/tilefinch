/* JavaScript engine micro-benchmark: a fixed set of synthetic kernels
   (interpreter dispatch, double arithmetic, property access, calls,
   allocation churn, strings, dtoa, JSON, regexp, GC, poll handler cost) and
   the compile of a directory of recorded script bodies, each timed on the
   platform monotonic clock. The same source builds the host tool
   (tilefinch-js-bench) and the PSP validation mode (boot.cfg
   validation_js_bench=1), so one PPSSPP or device run gives per-kernel
   costs that compare directly with the host. Diagnostic only. */
#ifndef TILEFINCH_JS_BENCH_H
#define TILEFINCH_JS_BENCH_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    /* Multiplies every kernel's iteration count (1 = the defaults; 0 = a
       smoke run of one iteration per kernel). */
    unsigned scale;
    /* Best of this many repeats per kernel (at least 1). */
    unsigned repeat;
    /* Directory of NNNN.meta/NNNN.body HTTP trace records; every record
       whose content-type is JavaScript is compiled (module, compile only).
       NULL skips the trace compile. */
    const char *trace_dir;
    /* Compile-only lazy function threshold for the compile kernels. */
    uint32_t lazy_threshold;
    /* QuickJS heap limit for the bench runtime, in bytes. */
    size_t memory_limit;
    /* Comma-separated kernel names to run, or NULL/empty for all. Explicit
       filters are bounded to 512 bytes and must name existing kernels;
       compile_trace additionally requires trace_dir. Refusal runs no work. */
    const char *only;
} JsBenchOptions;

/* One report line (without a newline) per result. */
typedef void (*JsBenchEmit)(void *opaque, const char *line);

void js_bench_default_options(JsBenchOptions *options);

/* Runs the benchmark and emits `tilefinch-js-bench:` lines. Returns 0 when
   every kernel ran, or the count of kernels that failed. */
int js_bench_run(const JsBenchOptions *options, JsBenchEmit emit,
                 void *opaque);

#endif
