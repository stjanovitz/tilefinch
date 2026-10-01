#include "tilefinch/script_census.h"
#ifdef CONFIG_TILEFINCH_EXECUTION_CENSUS
#include "tilefinch/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__PSP__)
#include <pspkernel.h>
#include "psp_thread_contract.h"
#else
#include <pthread.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif
#endif

/* One sampler for the browser owner thread, not one per frame/worker realm.
   All storage is fixed, no rows are evicted, overflow is reported explicitly.
   Samples are estimates of CPU attribution, not exact helper timings. */
#define CENSUS_RUNTIMES 8u
#define CENSUS_NATIVES 512u
#define CENSUS_FUNCTIONS JS_EXECUTION_CENSUS_FUNCTION_LIMIT
#define CENSUS_FILES 256u
#define CENSUS_FUNCTION_ZONES 8192u
#define CENSUS_FUNCTION_NATIVES 4096u
#ifdef CONFIG_TILEFINCH_REFCOUNT_CENSUS
#define CENSUS_REFERENCE_BYTES ((CENSUS_FUNCTIONS + 2u) * 3u * sizeof(uint64_t))
#else
#define CENSUS_REFERENCE_BYTES 0u
#endif
#if defined(__PSP__)
#define CENSUS_STACK 8192u
#else
#define CENSUS_STACK 65536u
#endif
static struct {
    JSRuntime *runtimes[CENSUS_RUNTIMES];
    BudgetReservation reservation;
    Budget *budget;
    unsigned phase, stop;
    unsigned detailed;
    uint32_t function_count, file_count, job_id, active_job;
    uint64_t job_begin_us, job_samples, job_phases[SCRIPT_SPLIT_KIND_COUNT],
             job_zones[JS_CENSUS_COUNT];
    struct { char label[128]; } files[CENSUS_FILES];
    struct {
        char name[48];
        uint32_t file, line, column, bytes, job;
        uint64_t cpu_us, job_us;
    } functions[CENSUS_FUNCTIONS];
    struct {
        uint32_t function, zone, job;
        uint64_t cpu_us, job_us;
    } function_zones[CENSUS_FUNCTION_ZONES];
    uint64_t function_zone_overflow_us;
    struct {
        uint32_t function, native, job;
        uint64_t cpu_us, job_us;
    } function_natives[CENSUS_FUNCTION_NATIVES];
    uint64_t function_native_overflow_us;
    uint64_t job_references[3];
    unsigned zone_family[JS_CENSUS_COUNT];
    uint64_t bins[SCRIPT_SPLIT_KIND_COUNT + 1][JS_CENSUS_COUNT];
    struct { uintptr_t address; uint64_t cpu_us, job_us; uint32_t job; } natives[CENSUS_NATIVES];
    uint64_t samples, failed, gaps_us, outside_us, native_overflow_us;
    uint64_t function_overflow, function_unknown_us, file_overflow;
#if defined(__PSP__)
    SceUID thread, owner, mutex;
#else
    pthread_t thread, owner;
#if defined(__APPLE__)
    mach_port_t owner_port;
#else
    clockid_t owner_clock;
#endif
#endif
} census;
static const char *const family_names[] = {
    "family-native", "family-memory", "family-property", "family-call",
    "family-branch", "family-stack", "family-other", "family-unknown"
};

static unsigned zone_family(unsigned zone)
{
    const char *name = JS_ExecutionCensusName(zone);
    if (zone == JS_CENSUS_NATIVE) return 0;
    if (zone == JS_CENSUS_ALLOCATE || zone == JS_CENSUS_RELEASE) return 1;
    if ((zone >= JS_CENSUS_GET_OWN && zone <= JS_CENSUS_SET_SETTER)
        || zone == JS_CENSUS_GET_PROPERTY || zone == JS_CENSUS_SET_PROPERTY
        || zone == JS_CENSUS_DEFINE_PROPERTY || strstr(name, "field")
        || strstr(name, "array_el") || strstr(name, "delete") || strstr(name, "proto")) return 2;
    if (zone == JS_CENSUS_CLOSURE || strstr(name, "call")
        || strstr(name, "return") || strstr(name, "apply")) return 3;
    if (!strncmp(name, "if_", 3) || !strncmp(name, "goto", 4)
        || !strcmp(name, "eq") || !strcmp(name, "neq")
        || !strcmp(name, "strict_eq") || !strcmp(name, "strict_neq")
        || !strcmp(name, "lt") || !strcmp(name, "lte")
        || !strcmp(name, "gt") || !strcmp(name, "gte")) return 4;
    if (!strncmp(name, "get_loc", 7) || !strncmp(name, "put_loc", 7)
        || !strncmp(name, "set_loc", 7) || !strncmp(name, "get_arg", 7)
        || !strncmp(name, "put_arg", 7) || !strncmp(name, "get_var", 7)
        || !strncmp(name, "put_var", 7) || !strncmp(name, "push_", 5)
        || !strncmp(name, "dup", 3) || !strncmp(name, "drop", 4)
        || !strncmp(name, "swap", 4) || !strncmp(name, "rot", 3)) return 5;
    return zone == JS_CENSUS_OUTSIDE || !strcmp(name, "unknown") ? 7 : 6;
}
#if !defined(__PSP__)
static pthread_mutex_t census_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

static void lock_census(void)
{
#if defined(__PSP__)
    (void)sceKernelWaitSema(census.mutex, 1, NULL);
#else
    (void)pthread_mutex_lock(&census_mutex);
#endif
}
static void unlock_census(void)
{
#if defined(__PSP__)
    (void)sceKernelSignalSema(census.mutex, 1);
#else
    (void)pthread_mutex_unlock(&census_mutex);
#endif
}
static bool census_owner(void)
{
#if defined(__PSP__)
    return census.owner == sceKernelGetThreadId();
#else
    return pthread_equal(census.owner, pthread_self()) != 0;
#endif
}
static bool owner_cpu_us(uint64_t *value)
{
#if defined(__PSP__)
    SceKernelThreadRunStatus info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    /* runClocks is already the microsecond thread run counter, as used by
       the transport probe. SysClock2USec is not a low/high-word unpacker. */
    if (sceKernelReferThreadRunStatus(census.owner, &info) < 0) return false;
    *value = ((uint64_t)info.runClocks.hi << 32) | info.runClocks.low;
#elif defined(__APPLE__)
    thread_basic_info_data_t info;
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    if (thread_info(census.owner_port, THREAD_BASIC_INFO,
                    (thread_info_t)&info, &count) != KERN_SUCCESS) return false;
    *value = ((uint64_t)info.user_time.seconds + info.system_time.seconds)
        * UINT64_C(1000000) + info.user_time.microseconds + info.system_time.microseconds;
#else
    struct timespec t;
    if (clock_gettime(census.owner_clock, &t) != 0) return false;
    *value = (uint64_t)t.tv_sec * UINT64_C(1000000) + (uint64_t)t.tv_nsec / 1000;
#endif
    return true;
}

static void copy_label(char *out, size_t capacity, const char *text)
{
    size_t at = 0;
    while (text && *text && at < capacity - 1) {
        unsigned char c = (unsigned char)*text++;
        out[at++] = c >= 33 && c <= 126 && c != '"' && c != '\\' ? (char)c : '_';
    }
    out[at] = '\0';
}

static uint32_t register_function(void *opaque, const char *file,
    const char *name, int line, int column, int bytes)
{
    (void)opaque;
    lock_census();
    if (census.function_count == CENSUS_FUNCTIONS) {
        census.function_overflow++;
        unlock_census(); return 0;
    }
    char label[128] = {0}; copy_label(label, sizeof(label), file);
    unsigned f;
    for (f = 0; f < census.file_count; f++)
        if (!strcmp(census.files[f].label, label)) break;
    if (f == census.file_count && f < CENSUS_FILES) {
        memcpy(census.files[f].label, label, sizeof(label));
        census.file_count++;
    }
    if (f == CENSUS_FILES) census.file_overflow++;
    uint32_t id = ++census.function_count;
    copy_label(census.functions[id - 1].name,
        sizeof(census.functions[id - 1].name), name);
    census.functions[id - 1].file = f;
    census.functions[id - 1].line = line > 0 ? (uint32_t)line : 0;
    census.functions[id - 1].column = column > 0 ? (uint32_t)column : 0;
    census.functions[id - 1].bytes = bytes > 0 ? (uint32_t)bytes : 0;
    unlock_census(); return id;
}

static void take_sample(uint64_t delta)
{
    uintptr_t native;
    unsigned phase = __atomic_load_n(&census.phase, __ATOMIC_RELAXED);
    unsigned zone = JS_ReadExecutionCensus(&native);
    uint32_t function = JS_ReadExecutionCensusFunction();
    lock_census();
    census.samples++;
    /* Long scheduling gaps are not assigned to whatever happens to run next. */
    if (delta > 20000) census.gaps_us += delta;
    else if (phase >= SCRIPT_SPLIT_KIND_COUNT) census.outside_us += delta;
    else {
        if (zone >= JS_CENSUS_COUNT) zone = JS_CENSUS_OUTSIDE;
        census.bins[phase][zone] += delta;
        if (census.active_job) {
            census.job_samples++;
            census.job_phases[phase] += delta;
            if (phase == SCRIPT_SPLIT_JS) census.job_zones[zone] += delta;
        }
        if (phase == SCRIPT_SPLIT_JS && census.detailed) {
            if (function && function <= census.function_count) {
                census.functions[function - 1].cpu_us += delta;
                /* A bounded, non-evicting joint table preserves the operation
                   mix of hot functions, not only a whole-page histogram. */
                unsigned family = census.zone_family[zone];
                unsigned at = (function * 2654435761u + family) % CENSUS_FUNCTION_ZONES;
                unsigned probe;
                for (probe = 0; probe < 32; probe++, at = (at + 1) % CENSUS_FUNCTION_ZONES) {
                    if (census.function_zones[at].function &&
                        (census.function_zones[at].function != function ||
                         census.function_zones[at].zone != family)) continue;
                    census.function_zones[at].function = function;
                    census.function_zones[at].zone = family;
                    census.function_zones[at].cpu_us += delta;
                    if (census.active_job) {
                        if (census.function_zones[at].job != census.active_job) {
                            census.function_zones[at].job = census.active_job;
                            census.function_zones[at].job_us = 0;
                        }
                        census.function_zones[at].job_us += delta;
                    }
                    break;
                }
                if (probe == 32) census.function_zone_overflow_us += delta;
                if (census.active_job) {
                    if (census.functions[function - 1].job != census.active_job) {
                        census.functions[function - 1].job = census.active_job;
                        census.functions[function - 1].job_us = 0;
                    }
                    census.functions[function - 1].job_us += delta;
                }
            } else census.function_unknown_us += delta;
        }
        if (phase == SCRIPT_SPLIT_JS && zone == JS_CENSUS_NATIVE && native) {
            unsigned slot;
            for (slot = 0; slot < CENSUS_NATIVES; slot++) {
                if (census.natives[slot].address == native || !census.natives[slot].address) {
                    census.natives[slot].address = native;
                    census.natives[slot].cpu_us += delta;
                    if (census.active_job) {
                        if (census.natives[slot].job != census.active_job) {
                            census.natives[slot].job = census.active_job;
                            census.natives[slot].job_us = 0;
                        }
                        census.natives[slot].job_us += delta;
                    }
                    /* Caller identity survives the native entry. Keep the
                       same sample's native body cost attached to that caller,
                       rather than inferring it from its family-wide total. */
                    if (census.detailed && function && function <= census.function_count) {
                        unsigned at = (function * 2654435761u + slot) % CENSUS_FUNCTION_NATIVES;
                        unsigned probe;
                        for (probe = 0; probe < 32;
                             probe++, at = (at + 1) % CENSUS_FUNCTION_NATIVES) {
                            if (census.function_natives[at].function &&
                                (census.function_natives[at].function != function ||
                                 census.function_natives[at].native != slot)) continue;
                            census.function_natives[at].function = function;
                            census.function_natives[at].native = slot;
                            census.function_natives[at].cpu_us += delta;
                            if (census.active_job) {
                                if (census.function_natives[at].job != census.active_job) {
                                    census.function_natives[at].job = census.active_job;
                                    census.function_natives[at].job_us = 0;
                                }
                                census.function_natives[at].job_us += delta;
                            }
                            break;
                        }
                        if (probe == 32) census.function_native_overflow_us += delta;
                    }
                    break;
                }
            }
            if (slot == CENSUS_NATIVES) census.native_overflow_us += delta;
        }
    }
    unlock_census();
}
#if defined(__PSP__)
static int census_run(SceSize args, void *argp)
#else
static void *census_run(void *opaque)
#endif
{
#if defined(__PSP__)
    (void)args; (void)argp;
#else
    (void)opaque;
#endif
    uint64_t previous = 0;
    bool valid = owner_cpu_us(&previous);
    unsigned jitter = 0;
    while (!__atomic_load_n(&census.stop, __ATOMIC_RELAXED)) {
        /* Coprime varying intervals avoid locking to repeated opcode patterns. */
        unsigned delay = 997 + (jitter++ % 13) * 37;
#if defined(__PSP__)
        (void)sceKernelDelayThread(delay);
#else
        struct timespec pause = {0, (long)delay * 1000};
        (void)nanosleep(&pause, NULL);
#endif
        uint64_t now;
        if (!owner_cpu_us(&now)) {
            lock_census(); census.failed++; unlock_census(); valid = false;
        } else {
            if (valid && now >= previous) take_sample(now - previous);
            previous = now; valid = true;
        }
    }
#if defined(__PSP__)
    return 0;
#else
    return NULL;
#endif
}

void script_census_phase(unsigned phase)
{
    __atomic_store_n(&census.phase, phase, __ATOMIC_RELAXED);
}

void script_census_attach(JSRuntime *runtime, Budget *budget)
{
    const char *option = getenv("TILEFINCH_EXECUTION_CENSUS");
    if (!option || (strcmp(option, "1") != 0 && strcmp(option, "2") != 0)) return;
    unsigned slot = CENSUS_RUNTIMES;
    for (unsigned i = 0; i < CENSUS_RUNTIMES; i++) {
        if (census.runtimes[i] == runtime) return;
        if (!census.runtimes[i] && slot == CENSUS_RUNTIMES) slot = i;
    }
    if (slot == CENSUS_RUNTIMES || (census.budget
        && (census.budget != budget || !census_owner()))) return;
    if (!census.budget) {
        memset(&census, 0, sizeof(census));
        census.detailed = !strcmp(option, "2");
        for (unsigned z = 0; z < JS_CENSUS_COUNT; z++) census.zone_family[z] = zone_family(z);
        JS_ResetExecutionCensusPaths();
        census.phase = SCRIPT_SPLIT_KIND_COUNT;
        if (!budget_reservation_acquire(&census.reservation, budget,
                BUDGET_CATEGORY_JAVASCRIPT,
                CENSUS_STACK + sizeof(census) + CENSUS_REFERENCE_BYTES)) return;
#if defined(__PSP__)
        census.owner = sceKernelGetThreadId();
        census.mutex = sceKernelCreateSema("tf-census-lock", 0, 1, 1, NULL);
        SceKernelThreadInfo info;
        if (census.mutex < 0 || psp_thread_snapshot(census.owner, &info) < 0) goto failed;
        census.thread = sceKernelCreateThread("tf-census", census_run,
            info.currentPriority > 1 ? info.currentPriority - 1 : 1,
            CENSUS_STACK, PSP_THREAD_ATTR_USER, NULL);
        if (census.thread < 0 || sceKernelStartThread(census.thread, 0, NULL) < 0) goto failed;
#else
        census.owner = pthread_self();
#if defined(__APPLE__)
        census.owner_port = mach_thread_self();
#else
        if (pthread_getcpuclockid(census.owner, &census.owner_clock) != 0) goto failed;
#endif
        pthread_attr_t attr;
        if (pthread_attr_init(&attr) != 0) goto failed;
        int result = pthread_attr_setstacksize(&attr, CENSUS_STACK);
        if (!result) result = pthread_create(&census.thread, &attr, census_run, NULL);
        (void)pthread_attr_destroy(&attr);
        if (result) goto failed;
#endif
        census.budget = budget;
    }
    census.runtimes[slot] = runtime;
    if (census.detailed)
        JS_SetExecutionCensusFunctionHook(runtime, register_function, NULL);
    JS_SetExecutionCensus(runtime, 1);
    return;
failed:
#if defined(__PSP__)
    if (census.thread > 0) (void)sceKernelDeleteThread(census.thread);
    if (census.mutex > 0) (void)sceKernelDeleteSema(census.mutex);
#elif defined(__APPLE__)
    if (census.owner_port) (void)mach_port_deallocate(mach_task_self(), census.owner_port);
#endif
    budget_reservation_release(&census.reservation);
    tilefinch_platform_log_message("tilefinch-execution-census: unavailable");
}

void script_census_detach(JSRuntime *runtime)
{
    bool found = false, remaining = false;
    for (unsigned i = 0; i < CENSUS_RUNTIMES; i++) {
        if (census.runtimes[i] == runtime) {
            JS_SetExecutionCensus(runtime, 0);
            JS_SetExecutionCensusFunctionHook(runtime, NULL, NULL);
            census.runtimes[i] = NULL; found = true;
        }
        if (census.runtimes[i]) remaining = true;
    }
    if (!found || remaining) return;
    __atomic_store_n(&census.stop, 1, __ATOMIC_RELAXED);
#if defined(__PSP__)
    (void)sceKernelWaitThreadEnd(census.thread, NULL);
    (void)sceKernelDeleteThread(census.thread);
    (void)sceKernelDeleteSema(census.mutex);
#else
    (void)pthread_join(census.thread, NULL);
#if defined(__APPLE__)
    (void)mach_port_deallocate(mach_task_self(), census.owner_port);
#endif
#endif
    budget_reservation_release(&census.reservation);
    census.budget = NULL;
}

void script_census_log(const char *label)
{
    /* Live-mark observer logs are foreign-thread snapshots. Only the owner
       emits census rows; it cannot race its own attach/detach. */
    if (!census.budget || !census_owner()) return;
    static const char *phases[] = {"js","style","query","mutate","fetch","layout","compile","gc","host"};
    char line[640];
    lock_census();
    snprintf(line, sizeof(line), "tilefinch-execution-census: label=%.64s samples=%llu failed=%llu gaps-us=%llu outside-us=%llu native-overflow-us=%llu anchor=%llx functions=%u function-overflow=%llu function-unknown-us=%llu function-zone-overflow-us=%llu function-native-overflow-us=%llu file-overflow=%llu reserved-bytes=%zu",
        label ? label : "-", (unsigned long long)census.samples,
        (unsigned long long)census.failed, (unsigned long long)census.gaps_us,
        (unsigned long long)census.outside_us, (unsigned long long)census.native_overflow_us,
        (unsigned long long)(uintptr_t)JS_RunGC, census.function_count,
        (unsigned long long)census.function_overflow,
        (unsigned long long)census.function_unknown_us,
        (unsigned long long)census.function_zone_overflow_us,
        (unsigned long long)census.function_native_overflow_us,
        (unsigned long long)census.file_overflow,
        (size_t)(CENSUS_STACK + sizeof(census) + CENSUS_REFERENCE_BYTES));
    tilefinch_platform_log_message(line);
    for (unsigned p = 0; p < SCRIPT_SPLIT_KIND_COUNT; p++)
        for (unsigned z = 0; z < JS_CENSUS_COUNT; z++) {
            if (!census.bins[p][z]) continue;
            snprintf(line, sizeof(line), "tilefinch-execution-bin: label=%.64s phase=%s zone=%s cpu-us=%llu",
                label ? label : "-", phases[p], JS_ExecutionCensusName(z),
                (unsigned long long)census.bins[p][z]);
            tilefinch_platform_log_message(line);
        }
    for (unsigned n = 0; n < CENSUS_NATIVES; n++) {
        if (!census.natives[n].address) continue;
        snprintf(line, sizeof(line), "tilefinch-execution-native: label=%.64s address=%llx cpu-us=%llu",
            label ? label : "-", (unsigned long long)census.natives[n].address,
            (unsigned long long)census.natives[n].cpu_us);
        tilefinch_platform_log_message(line);
    }
    for (unsigned f = 0; f < census.function_count; f++) {
        uint64_t references[3];
        JS_ReadExecutionCensusReferences(f + 1, references);
        if (!census.functions[f].cpu_us && !references[0] && !references[1]) continue;
        unsigned file = census.functions[f].file;
        snprintf(line, sizeof(line), "tilefinch-execution-function: label=%.64s id=%u cpu-us=%llu file=%s name=%s line=%u column=%u bytes=%u",
            label ? label : "-", f + 1, (unsigned long long)census.functions[f].cpu_us,
            file < census.file_count ? census.files[file].label : "<file-overflow>",
            census.functions[f].name, census.functions[f].line,
            census.functions[f].column, census.functions[f].bytes);
        tilefinch_platform_log_message(line);
        if (references[0] || references[1]) {
            snprintf(line, sizeof(line), "tilefinch-execution-function-refs: label=%.64s function=%u retain=%llu release=%llu final=%llu",
                label ? label : "-", f + 1, (unsigned long long)references[0],
                (unsigned long long)references[1], (unsigned long long)references[2]);
            tilefinch_platform_log_message(line);
        }
    }
    uint64_t paths[JS_CENSUS_COUNT];
    for (unsigned i = 0; i < CENSUS_FUNCTION_NATIVES; i++) {
        if (!census.function_natives[i].function) continue;
        snprintf(line, sizeof(line), "tilefinch-execution-function-native: label=%.64s function=%u address=%llx cpu-us=%llu",
            label ? label : "-", census.function_natives[i].function,
            (unsigned long long)census.natives[census.function_natives[i].native].address,
            (unsigned long long)census.function_natives[i].cpu_us);
        tilefinch_platform_log_message(line);
    }
    for (unsigned i = 0; i < CENSUS_FUNCTION_ZONES; i++) {
        if (!census.function_zones[i].function) continue;
        snprintf(line, sizeof(line), "tilefinch-execution-function-zone: label=%.64s function=%u zone=%s cpu-us=%llu",
            label ? label : "-", census.function_zones[i].function,
            family_names[census.function_zones[i].zone],
            (unsigned long long)census.function_zones[i].cpu_us);
        tilefinch_platform_log_message(line);
    }
    if (census.detailed && JS_ReadExecutionCensusPaths(paths, JS_CENSUS_COUNT))
        for (unsigned z = 256; z < JS_CENSUS_COUNT; z++) {
            if (!paths[z]) continue;
            snprintf(line, sizeof(line), "tilefinch-execution-path: label=%.64s path=%s count=%llu",
                label ? label : "-", JS_ExecutionCensusName(z), (unsigned long long)paths[z]);
            tilefinch_platform_log_message(line);
        }
    unlock_census();
}

uint32_t script_census_job_begin(uint64_t begin_us)
{
    if (!census.budget || !census.detailed || !census_owner()) return 0;
    lock_census();
    if (census.active_job || census.job_id == UINT32_MAX) { unlock_census(); return 0; }
    census.active_job = ++census.job_id;
    census.job_begin_us = begin_us;
    census.job_samples = 0;
    JS_ReadExecutionCensusReferences(UINT32_MAX, census.job_references);
    memset(census.job_phases, 0, sizeof(census.job_phases));
    memset(census.job_zones, 0, sizeof(census.job_zones));
    uint32_t result = census.active_job;
    unlock_census(); return result;
}

void script_census_job_end(uint32_t token, uint64_t end_us)
{
    if (!token || !census.budget || !census_owner()) return;
    lock_census();
    if (token != census.active_job) { unlock_census(); return; }
    census.active_job = 0;
    if (end_us < census.job_begin_us || end_us - census.job_begin_us < 100000) {
        unlock_census(); return;
    }
    char line[320];
    snprintf(line, sizeof(line), "tilefinch-execution-job: id=%u begin-us=%llu end-us=%llu samples=%llu",
        token, (unsigned long long)census.job_begin_us, (unsigned long long)end_us,
        (unsigned long long)census.job_samples);
    tilefinch_platform_log_message(line);
    uint64_t references[3];
    JS_ReadExecutionCensusReferences(UINT32_MAX, references);
    if (references[0] || references[1]) {
        snprintf(line, sizeof(line), "tilefinch-execution-job-refs: id=%u retain=%llu release=%llu final=%llu",
            token, (unsigned long long)(references[0] - census.job_references[0]),
            (unsigned long long)(references[1] - census.job_references[1]),
            (unsigned long long)(references[2] - census.job_references[2]));
        tilefinch_platform_log_message(line);
    }
    static const char *phases[] = {"js","style","query","mutate","fetch","layout","compile","gc","host"};
    for (unsigned p = 0; p < SCRIPT_SPLIT_KIND_COUNT; p++) {
        if (!census.job_phases[p]) continue;
        snprintf(line, sizeof(line), "tilefinch-execution-job-phase: id=%u phase=%s cpu-us=%llu",
            token, phases[p], (unsigned long long)census.job_phases[p]);
        tilefinch_platform_log_message(line);
    }
    for (unsigned z = 0; z < JS_CENSUS_COUNT; z++) {
        if (!census.job_zones[z]) continue;
        snprintf(line, sizeof(line), "tilefinch-execution-job-zone: id=%u zone=%s cpu-us=%llu",
            token, JS_ExecutionCensusName(z), (unsigned long long)census.job_zones[z]);
        tilefinch_platform_log_message(line);
    }
    for (unsigned f = 0; f < census.function_count; f++) {
        if (census.functions[f].job != token) continue;
        snprintf(line, sizeof(line), "tilefinch-execution-job-function: id=%u function=%u cpu-us=%llu",
            token, f + 1, (unsigned long long)census.functions[f].job_us);
        tilefinch_platform_log_message(line);
    }
    for (unsigned n = 0; n < CENSUS_NATIVES; n++) {
        if (census.natives[n].job != token) continue;
        snprintf(line, sizeof(line), "tilefinch-execution-job-native: id=%u address=%llx cpu-us=%llu",
            token, (unsigned long long)census.natives[n].address,
            (unsigned long long)census.natives[n].job_us);
        tilefinch_platform_log_message(line);
    }
    for (unsigned i = 0; i < CENSUS_FUNCTION_ZONES; i++) {
        if (census.function_zones[i].job != token) continue;
        snprintf(line, sizeof(line), "tilefinch-execution-job-function-zone: id=%u function=%u zone=%s cpu-us=%llu",
            token, census.function_zones[i].function,
            family_names[census.function_zones[i].zone],
            (unsigned long long)census.function_zones[i].job_us);
        tilefinch_platform_log_message(line);
    }
    for (unsigned i = 0; i < CENSUS_FUNCTION_NATIVES; i++) {
        if (census.function_natives[i].job != token) continue;
        snprintf(line, sizeof(line), "tilefinch-execution-job-function-native: id=%u function=%u address=%llx cpu-us=%llu",
            token, census.function_natives[i].function,
            (unsigned long long)census.natives[census.function_natives[i].native].address,
            (unsigned long long)census.function_natives[i].job_us);
        tilefinch_platform_log_message(line);
    }
    unlock_census();
}
#endif
