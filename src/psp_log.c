#define TILEFINCH_PSP_LOG_IMPLEMENTATION 1
#include "tilefinch/psp_log.h"
#include "tilefinch/psp_threads.h"

#include <pspiofilemgr.h>
#include <pspkernel.h>

#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#if !defined(TILEFINCH_PSP_VALIDATION_LOG)

bool psp_log_start(const char *argv0)
{
    (void) argv0;
    return false;
}

int psp_log_install_exception_handler(void)
{
    return -1;
}

bool psp_log_start_watchdog(uint32_t timeout_ms)
{
    (void) timeout_ms;
    return false;
}

void psp_log_stop_watchdog(void) {}
void psp_log_heartbeat(void) {}
void psp_log_set_phase(PspLogPhase phase) { (void) phase; }
PspLogPhase psp_log_phase(void) { return PSP_LOG_PHASE_BOOT; }

uint32_t psp_log_operation_begin(const char *action)
{
    (void) action;
    return 0;
}

void psp_log_operation_end(uint32_t sequence, const char *action,
                           const char *result)
{
    (void) sequence;
    (void) action;
    (void) result;
}

int psp_log_printf(const char *format, ...)
{
    (void) format;
    return 0;
}

FILE *psp_log_file(void) { return NULL; }

bool psp_log_flush(bool synchronize_device)
{
    (void) synchronize_device;
    return false;
}

bool psp_log_healthy(void) { return false; }
void psp_log_checkpoint(const char *name) { (void) name; }
void psp_log_emergency(const char *state) { (void) state; }
void psp_log_finish(const char *outcome) { (void) outcome; }

#else

#define PSP_LOG_LINE_BYTES 1024u
#define PSP_CRASH_RECORD_BYTES 512u
#define PSP_LOG_STREAM_BUFFER_BYTES (64u * 1024u)
#define PSP_LOG_STDOUT_BUFFER_BYTES (8u * 1024u)

static FILE *validation_log;
/*
 * Validation can emit several hundred aggregate/failure lines during a media
 * soak. Keep those writes out of the frame loop: stdio drains this fixed BSS
 * buffer in large sequential writes, while explicit checkpoints and failure
 * paths still flush and synchronize it for crash diagnosis. Shipping builds
 * compile this entire block out.
 */
static unsigned char validation_stream_buffer[PSP_LOG_STREAM_BUFFER_BYTES];
static unsigned char validation_stdout_buffer[PSP_LOG_STDOUT_BUFFER_BYTES];
static char validation_path[768];
static char crash_record_path[768];
static SceUID crash_fd = -1;
static SceUID log_semaphore = -1;
static SceUID crash_semaphore = -1;
static SceUID watchdog_thread = -1;
static _Atomic uint32_t current_phase = PSP_LOG_PHASE_BOOT;
static _Atomic uint32_t current_operation;
static _Atomic uint32_t heartbeat_ms;
static _Atomic uint32_t watchdog_timeout_ms;
static _Atomic bool watchdog_running;
static _Atomic bool persistent_healthy;
static _Atomic bool watchdog_reported;
static bool persistent_paths_on_memory_stick;

static const char *const phase_names[PSP_LOG_PHASE_COUNT] = {
    "boot", "config", "assets", "engine", "network", "navigation",
    "report", "render", "interactive", "input", "voice", "media",
    "cleanup", "halted"
};

static uint32_t now_ms(void)
{
    return (uint32_t) (
        (uint64_t) sceKernelGetSystemTimeWide() / UINT64_C(1000));
}

static bool path_is_on_memory_stick(const char *path)
{
    return path != NULL
        && (strncmp(path, "ms0:", 4u) == 0
            || strncmp(path, "msfat0:", 7u) == 0);
}

static int sync_persistent_storage(void)
{
    /* A PSPLink host0: validation run keeps every diagnostic artifact on the
       Mac. Touching ms0: in that mode adds latency and violates the device
       loop's zero-Memory-Stick-I/O contract without making the host file any
       more durable. fflush/fclose are the only meaningful operations there. */
    if (!persistent_paths_on_memory_stick) return 0;
    int result = sceIoSync("ms0:", 0);
    if (result < 0) result = sceIoSync("msfat0:", 0);
    return result;
}

static void sibling_path(char *output, size_t size, const char *argv0,
                         const char *name)
{
    if (output == NULL || size == 0) return;
    const char *slash = argv0 == NULL ? NULL : strrchr(argv0, '/');
    if (slash == NULL) {
        snprintf(output, size, "%s", name);
        return;
    }
    snprintf(output, size, "%.*s/%s", (int) (slash - argv0), argv0, name);
}

static size_t append_literal(char *buffer, size_t capacity, size_t used,
                             const char *text)
{
    if (buffer == NULL || text == NULL) return used;
    while (*text != '\0' && used < capacity) buffer[used++] = *text++;
    return used;
}

static size_t append_hex32(char *buffer, size_t capacity, size_t used,
                           uint32_t value)
{
    static const char digits[] = "0123456789abcdef";
    for (int shift = 28; shift >= 0 && used < capacity; shift -= 4) {
        buffer[used++] = digits[(value >> shift) & 0x0fu];
    }
    return used;
}

static bool crash_record_write(const char *state,
                               bool synchronize_device)
{
    if (crash_fd < 0 || crash_semaphore < 0
        || sceKernelPollSema(crash_semaphore, 1) < 0) return false;
    char record[PSP_CRASH_RECORD_BYTES];
    memset(record, ' ', sizeof(record));
    size_t used = 0;
    used = append_literal(record, sizeof(record), used,
                          "tilefinch-crash-v1 state=");
    used = append_literal(record, sizeof(record), used,
                          state == NULL ? "unknown" : state);
    used = append_literal(record, sizeof(record), used, " phase=");
    uint32_t phase = current_phase;
    used = append_literal(
        record, sizeof(record), used,
        phase < PSP_LOG_PHASE_COUNT ? phase_names[phase] : "invalid");
    used = append_literal(record, sizeof(record), used, " operation=0x");
    used = append_hex32(
        record, sizeof(record), used, current_operation);
    used = append_literal(record, sizeof(record), used, " heartbeat-ms=0x");
    used = append_hex32(record, sizeof(record), used, heartbeat_ms);
    if (used >= sizeof(record)) used = sizeof(record) - 1;
    record[used] = '\n';
    SceOff offset = sceIoLseek(crash_fd, 0, PSP_SEEK_SET);
    int written = offset < 0
        ? -1 : sceIoWrite(crash_fd, record, sizeof(record));
    int synchronized = synchronize_device ? sync_persistent_storage() : 0;
    bool okay = offset >= 0
        && written == (int) sizeof(record);
    /* Some real firmware/storage-driver combinations reject sceIoSync even
       though the checked write and stdio flush succeed.  Keep the attempt,
       but do not declare both diagnostic artifacts unusable solely because
       this optional durability hint is unsupported. */
    (void) synchronized;
    (void) sceKernelSignalSema(crash_semaphore, 1);
    if (!okay) persistent_healthy = 0;
    return okay;
}

static int watchdog_main(SceSize argument_size, void *arguments)
{
    (void) argument_size;
    (void) arguments;
    while (watchdog_running) {
        (void) sceKernelDelayThread(500000);
        if (!watchdog_running) break;
        uint32_t current = now_ms();
        uint32_t last = heartbeat_ms;
        uint32_t timeout = watchdog_timeout_ms;
        if (timeout != 0 && last != 0
            && (uint32_t) (current - last) >= timeout) {
            if (!watchdog_reported) {
                watchdog_reported = 1;
                crash_record_write("suspected-hang", true);
            }
        } else if (watchdog_reported) {
            watchdog_reported = 0;
            crash_record_write("watchdog-recovered", false);
        }
    }
    return 0;
}

bool psp_log_start(const char *argv0)
{
    char previous_path[768];
    char previous_crash_path[768];
    sibling_path(validation_path, sizeof(validation_path), argv0,
                 "tilefinch-validation.txt");
    sibling_path(previous_path, sizeof(previous_path), argv0,
                 "tilefinch-validation.previous.txt");
    sibling_path(crash_record_path, sizeof(crash_record_path), argv0,
                 "tilefinch-crash.txt");
    sibling_path(previous_crash_path, sizeof(previous_crash_path), argv0,
                 "tilefinch-crash.previous.txt");
    persistent_paths_on_memory_stick =
        path_is_on_memory_stick(validation_path)
        || path_is_on_memory_stick(crash_record_path);

    SceIoStat current_stat;
    SceIoStat crash_stat;
    bool current_exists =
        sceIoGetstat(validation_path, &current_stat) >= 0;
    bool crash_exists =
        sceIoGetstat(crash_record_path, &crash_stat) >= 0;
    int validation_previous_remove = sceIoRemove(previous_path);
    int validation_rotate = sceIoRename(validation_path, previous_path);
    int crash_previous_remove = sceIoRemove(previous_crash_path);
    int crash_rotate = sceIoRename(crash_record_path, previous_crash_path);
    bool validation_rotation_safe =
        !current_exists || validation_rotate >= 0;
    bool crash_rotation_safe = !crash_exists || crash_rotate >= 0;
    validation_log = validation_rotation_safe
        ? fopen(validation_path, "w") : NULL;
    if (validation_log != NULL) {
        (void) setvbuf(
            validation_log, (char *) validation_stream_buffer,
            _IOFBF, sizeof(validation_stream_buffer));
    }
    /* PSPLink stdout is diagnostic mirroring, not the durable artifact. It
       must not turn every log line into a host-I/O scheduling point either. */
    (void) setvbuf(
        stdout, (char *) validation_stdout_buffer,
        _IOFBF, sizeof(validation_stdout_buffer));
    crash_fd = crash_rotation_safe
        ? sceIoOpen(
              crash_record_path,
              PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0666)
        : -1;
    log_semaphore = sceKernelCreateSema(
        "tilefinch_log", 0, 1, 1, NULL);
    crash_semaphore = sceKernelCreateSema(
        "tilefinch_crash", 0, 1, 1, NULL);
    if (log_semaphore < 0 && validation_log != NULL) {
        (void) fclose(validation_log);
        validation_log = NULL;
    }
    if (crash_semaphore < 0 && crash_fd >= 0) {
        (void) sceIoClose(crash_fd);
        crash_fd = -1;
    }
    persistent_healthy =
        validation_log != NULL && crash_fd >= 0
        && log_semaphore >= 0 && crash_semaphore >= 0;
    heartbeat_ms = now_ms();
    crash_record_write(
        persistent_healthy ? "armed" : "log-admission-failed", true);
    if (persistent_healthy) {
        psp_log_printf(
            "tilefinch-log: validation-path=%s crash-path=%s\n",
            validation_path, crash_record_path);
        psp_log_printf(
            "tilefinch-log: rotation validation-remove=0x%08x "
            "validation-rotate=0x%08x crash-remove=0x%08x "
            "crash-rotate=0x%08x\n",
            (unsigned) validation_previous_remove,
            (unsigned) validation_rotate,
            (unsigned) crash_previous_remove,
            (unsigned) crash_rotate);
    }
    return persistent_healthy != 0;
}

int psp_log_install_exception_handler(void)
{
    /*
     * PSPSDK exposes default exception registration only through its
     * kernel-mode import library. Tilefinch intentionally remains a
     * user-mode homebrew application, so retain the interface for the
     * validation report but do not silently acquire kernel assumptions.
     */
    return -1;
}

bool psp_log_start_watchdog(uint32_t timeout_ms)
{
    if (watchdog_running) return true;
    watchdog_timeout_ms = timeout_ms;
    watchdog_reported = 0;
    watchdog_running = 1;
    watchdog_thread = sceKernelCreateThread(
        "tilefinch_watchdog", watchdog_main,
        TILEFINCH_PSP_THREAD_PRIORITY_WATCHDOG, 16u * 1024u,
        PSP_THREAD_ATTR_USER, NULL);
    if (watchdog_thread < 0
        || sceKernelStartThread(watchdog_thread, 0, NULL) < 0) {
        if (watchdog_thread >= 0) {
            (void) sceKernelDeleteThread(watchdog_thread);
        }
        watchdog_thread = -1;
        watchdog_running = 0;
        return false;
    }
    return true;
}

void psp_log_stop_watchdog(void)
{
    if (!watchdog_running) return;
    watchdog_running = 0;
    if (watchdog_thread >= 0) {
        (void) sceKernelWaitThreadEnd(watchdog_thread, NULL);
        (void) sceKernelDeleteThread(watchdog_thread);
    }
    watchdog_thread = -1;
}

void psp_log_heartbeat(void)
{
    heartbeat_ms = now_ms();
}

void psp_log_set_phase(PspLogPhase phase)
{
    current_phase = phase < PSP_LOG_PHASE_COUNT
        ? (uint32_t) phase : (uint32_t) PSP_LOG_PHASE_HALTED;
    psp_log_heartbeat();
}

PspLogPhase psp_log_phase(void)
{
    uint32_t phase = current_phase;
    return phase < PSP_LOG_PHASE_COUNT
        ? (PspLogPhase) phase : PSP_LOG_PHASE_HALTED;
}

int psp_log_printf(const char *format, ...)
{
    char line[PSP_LOG_LINE_BYTES];
    va_list arguments;
    va_start(arguments, format);
    int requested = vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    size_t length = requested < 0 ? 0 : (size_t) requested;
    bool truncated = length >= sizeof(line);
    if (truncated) {
        static const char suffix[] = "...[truncated]\n";
        length = sizeof(line) - 1;
        size_t suffix_length = sizeof(suffix) - 1;
        memcpy(line + length - suffix_length, suffix, suffix_length);
    }

    bool locked = log_semaphore >= 0
        && sceKernelWaitSema(log_semaphore, 1, NULL) >= 0;
    if (length != 0) {
        (void) fwrite(line, 1, length, stdout);
    }
    if (validation_log != NULL && length != 0) {
        size_t written = fwrite(line, 1, length, validation_log);
        if (written != length || ferror(validation_log)) {
            persistent_healthy = 0;
            crash_record_write("log-write-failed", true);
        }
    }
    if (locked) (void) sceKernelSignalSema(log_semaphore, 1);
    return truncated ? (int) length : requested;
}

FILE *psp_log_file(void)
{
    return validation_log;
}

bool psp_log_flush(bool synchronize_device)
{
    bool locked = log_semaphore >= 0
        && sceKernelWaitSema(log_semaphore, 1, NULL) >= 0;
    bool okay = validation_log != NULL && locked
        && fflush(validation_log) == 0 && !ferror(validation_log);
    /* PSPLink output is a best-effort mirror. Its availability must never
       downgrade the durable on-card diagnostic artifact. */
    (void) fflush(stdout);
    if (synchronize_device) (void) sync_persistent_storage();
    if (locked) (void) sceKernelSignalSema(log_semaphore, 1);
    if (!okay) {
        persistent_healthy = 0;
        crash_record_write("log-flush-failed", false);
    }
    return okay;
}

bool psp_log_healthy(void)
{
    return persistent_healthy != 0;
}

void psp_log_checkpoint(const char *name)
{
    psp_log_heartbeat();
    uint32_t phase = current_phase;
    psp_log_printf(
        "tilefinch-checkpoint: phase=%s name=%s operation=%lu "
        "heartbeat-ms=%lu log-healthy=%d\n",
        phase < PSP_LOG_PHASE_COUNT ? phase_names[phase] : "invalid",
        name == NULL ? "unnamed" : name, current_operation,
        heartbeat_ms, persistent_healthy ? 1 : 0);
    /*
     * Write both artifacts, then synchronize the device once.
     *
     * sceIoSync flushes the Memory Stick, not an individual file, so syncing
     * after the crash record and again after the log flush paid for the same
     * slow device operation twice per checkpoint. The emulator makes it free
     * and the hardware does not: on a real card this is tens to hundreds of
     * milliseconds each, across roughly a dozen checkpoints before the
     * browser is interactive. Ordering the two writes ahead of a single sync
     * leaves durability identical — both records are already on the device
     * when it runs.
     */
    crash_record_write("running", false);
    (void) psp_log_flush(true);
}

uint32_t psp_log_operation_begin(const char *action)
{
    uint32_t sequence = ++current_operation;
    psp_log_heartbeat();
    psp_log_printf("tilefinch-operation: sequence=%lu phase=begin "
                   "action=%s\n", sequence,
                   action == NULL ? "unknown" : action);
    crash_record_write("operation", false);
    return sequence;
}

void psp_log_operation_end(uint32_t sequence, const char *action,
                           const char *result)
{
    psp_log_heartbeat();
    psp_log_printf("tilefinch-operation: sequence=%lu phase=end "
                   "action=%s result=%s\n", sequence,
                   action == NULL ? "unknown" : action,
                   result == NULL ? "unknown" : result);
    crash_record_write("operation-complete", false);
}

void psp_log_emergency(const char *state)
{
    crash_record_write(state == NULL ? "emergency" : state, true);
}

void psp_log_finish(const char *outcome)
{
    psp_log_stop_watchdog();
    psp_log_printf("tilefinch-log: finish outcome=%s healthy=%d\n",
                   outcome == NULL ? "unknown" : outcome,
                   persistent_healthy ? 1 : 0);
    (void) psp_log_flush(true);
    crash_record_write(
        outcome == NULL ? "unknown" : outcome, true);
    /* The HOME callback is process-lifetime and can run concurrently after
       ordinary cleanup. Keep both descriptors and their independent locks
       alive until sceKernelExitGame tears down the process, avoiding a
       use-after-close window in the crash path itself. */
}

#endif
