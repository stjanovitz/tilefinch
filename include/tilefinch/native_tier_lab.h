#ifndef TILEFINCH_NATIVE_TIER_LAB_H
#define TILEFINCH_NATIVE_TIER_LAB_H

#include <stdint.h>
#include <quickjs.h>

/* Isolated host-lab instrumentation, absent from the PSP and normal core. */
void tilefinch_native_tier_snapshot(JSRuntime *runtime,
                                    uint64_t *epoch, uint64_t *calls,
                                    uint64_t *completed);
uint64_t tilefinch_native_tier_lazy_compile_ns(void);
void tilefinch_native_tier_job_begin(void);
void tilefinch_native_tier_job_report(JSRuntime *runtime, uint64_t wall_us);

#endif
