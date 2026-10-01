#ifndef TILEFINCH_SCRIPT_CENSUS_H
#define TILEFINCH_SCRIPT_CENSUS_H
#include "tilefinch/budget.h"
#include "tilefinch/script_split.h"
#include <quickjs.h>

#ifdef CONFIG_TILEFINCH_EXECUTION_CENSUS
void script_census_attach(JSRuntime *runtime, Budget *budget);
void script_census_detach(JSRuntime *runtime);
void script_census_phase(unsigned phase);
void script_census_log(const char *label);
uint32_t script_census_job_begin(uint64_t begin_us);
void script_census_job_end(uint32_t token, uint64_t end_us);
#else
static inline void script_census_attach(JSRuntime *r, Budget *b) { (void)r; (void)b; }
static inline void script_census_detach(JSRuntime *r) { (void)r; }
static inline void script_census_phase(unsigned p) { (void)p; }
static inline void script_census_log(const char *l) { (void)l; }
static inline uint32_t script_census_job_begin(uint64_t b) { (void)b; return 0; }
static inline void script_census_job_end(uint32_t t, uint64_t e) { (void)t; (void)e; }
#endif
#endif
