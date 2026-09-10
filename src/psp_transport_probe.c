/* Validation-only linker interposition. No shipping dependency, allocation,
   socket-mode change, or change to curl's scheduling/ownership model. */
#include <curl/curl.h>
#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <pspkernel.h>
#include <pspnet_inet.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include "tilefinch/psp_log.h"

enum { PROBE_DNS, PROBE_CONNECT, PROBE_SELECT, PROBE_RECV, PROBE_SEND,
       PROBE_SEMA, PROBE_DELAY, PROBE_NET_SELECT, PROBE_COUNT };
typedef struct { unsigned calls; uint64_t total, maximum; int fd; } ProbeCall;
static atomic_int probe_owner = ATOMIC_VAR_INIT(-1);
static ProbeCall probe_calls[PROBE_COUNT];
static unsigned probe_nonblock_checks, probe_nonblock_bad, probe_nonblock_unreadable;
static uint64_t probe_select_zero_max;

static uint64_t probe_now(void) { return sceKernelGetSystemTimeWide(); }
static bool probe_active(void)
{
    int owner = atomic_load_explicit(&probe_owner, memory_order_relaxed);
    return owner >= 0 && owner == sceKernelGetThreadId();
}
static void probe_record(unsigned kind, uint64_t start, int fd)
{
    ProbeCall *entry = &probe_calls[kind];
    uint64_t elapsed = probe_now() - start;
    entry->calls++;
    entry->total += elapsed;
    if (elapsed > entry->maximum) { entry->maximum = elapsed; entry->fd = fd; }
}
static bool probe_cpu(int thread, uint64_t *clocks, SceKernelThreadRunStatus *status)
{
    memset(status, 0, sizeof(*status));
    status->size = sizeof(*status);
    if (sceKernelReferThreadRunStatus(thread, status) < 0) return false;
    *clocks = ((uint64_t) status->runClocks.hi << 32) | status->runClocks.low;
    return true;
}

extern CURLMcode __real_curl_multi_perform(CURLM *, int *);
extern CURLMcode __real_curl_multi_poll(CURLM *, struct curl_waitfd *, unsigned int, int, int *);
static CURLMcode probe_multi_call(CURLM *multi, int *running,
    struct curl_waitfd *extra, unsigned int extra_count, int timeout_ms, bool poll)
{
    int thread = sceKernelGetThreadId(), expected = -1;
    if (!atomic_compare_exchange_strong(&probe_owner, &expected, thread))
        return poll ? __real_curl_multi_poll(multi, extra, extra_count, timeout_ms, running)
                    : __real_curl_multi_perform(multi, running);
    memset(probe_calls, 0, sizeof(probe_calls));
    probe_nonblock_checks = probe_nonblock_bad = probe_nonblock_unreadable = 0;
    probe_select_zero_max = 0;
    uint64_t before_cpu = 0, after_cpu = 0;
    SceKernelThreadRunStatus before, after;
    bool cpu_valid = probe_cpu(thread, &before_cpu, &before);
    uint64_t start = probe_now();
    CURLMcode result = poll ? __real_curl_multi_poll(multi, extra, extra_count, timeout_ms, running)
                           : __real_curl_multi_perform(multi, running);
    int saved_errno = errno;
    uint64_t elapsed = probe_now() - start;
    cpu_valid = probe_cpu(thread, &after_cpu, &after) && cpu_valid && after_cpu >= before_cpu;
    ProbeCall calls[PROBE_COUNT];
    memcpy(calls, probe_calls, sizeof(calls));
    /* Only this thread accesses records until logging ends. Nested or
       concurrent multi calls are not instrumented and cannot overwrite them. */
    if (elapsed >= 100000 || probe_nonblock_checks) {
        psp_log_printf("tilefinch-transport-call: wall=%lluus cpu=%lluus cpu-valid=%u "
            "nonblock=%u bad=%u unreadable=%u select-zero-max=%lluus op=%s\n",
            (unsigned long long) elapsed,
            (unsigned long long) (cpu_valid ? after_cpu - before_cpu : 0),
            (unsigned) cpu_valid, probe_nonblock_checks, probe_nonblock_bad,
            probe_nonblock_unreadable, (unsigned long long) probe_select_zero_max,
            poll ? "poll" : "perform");
        psp_log_printf("tilefinch-transport-scheduling: priority=%d preempt=%u interrupt=%u release=%u\n",
            before.currentPriority,
            (unsigned) (after.threadPreemptCount - before.threadPreemptCount),
            (unsigned) (after.intrPreemptCount - before.intrPreemptCount),
            (unsigned) (after.releaseCount - before.releaseCount));
        static const char *const names[PROBE_COUNT] = {
            "dns", "connect", "select", "recv", "send", "sema", "delay", "net-select" };
        for (unsigned i = 0; i < PROBE_COUNT; i++) {
            const ProbeCall *entry = &calls[i];
            psp_log_printf("tilefinch-transport-primitive: kind=%s calls=%u total=%lluus max=%lluus fd=%d\n",
                names[i], entry->calls, (unsigned long long) entry->total,
                (unsigned long long) entry->maximum, entry->fd);
        }
    }
    atomic_store_explicit(&probe_owner, -1, memory_order_release);
    errno = saved_errno;
    return result;
}

CURLMcode __wrap_curl_multi_perform(CURLM *multi, int *running)
{ return probe_multi_call(multi, running, NULL, 0, 0, false); }
CURLMcode __wrap_curl_multi_poll(CURLM *multi, struct curl_waitfd *extra,
    unsigned int extra_count, int timeout_ms, int *numfds)
{ return probe_multi_call(multi, numfds, extra, extra_count, timeout_ms, true); }

/* Preserve the primitive's errno even when timing uses PSP services. Timings
   are inclusive: DNS may internally call another wrapped primitive. */
#define PROBE_RETURN(type, kind, descriptor, expression) \
    if (!probe_active()) return (expression); \
    uint64_t start = probe_now(); \
    type result = (expression); \
    int saved_errno = errno; \
    probe_record(kind, start, descriptor); \
    errno = saved_errno; \
    return result

extern struct hostent *__real_gethostbyname(const char *);
extern int __real_sceNetInetSelect(int, fd_set *, fd_set *, fd_set *, struct SceNetInetTimeval *);
int __wrap_sceNetInetSelect(int n, fd_set *r, fd_set *w, fd_set *e, struct SceNetInetTimeval *timeout)
{
    /* For net-select the diagnostic argument is the requested timeout in
       microseconds, not a libc descriptor; zero must mean exactly zero. */
    int64_t micros = timeout == NULL ? -1
        : (int64_t) timeout->tv_sec * 1000000 + timeout->tv_usec;
    int requested = micros > INT_MAX ? INT_MAX : micros < INT_MIN ? INT_MIN : (int) micros;
    PROBE_RETURN(int, PROBE_NET_SELECT, requested, __real_sceNetInetSelect(n, r, w, e, timeout));
}
extern int __real_sceKernelWaitSema(SceUID, int, SceUInt *);
int __wrap_sceKernelWaitSema(SceUID id, int count, SceUInt *timeout)
{ PROBE_RETURN(int, PROBE_SEMA, id, __real_sceKernelWaitSema(id, count, timeout)); }
extern int __real_sceKernelDelayThread(SceUInt);
int __wrap_sceKernelDelayThread(SceUInt delay)
{ PROBE_RETURN(int, PROBE_DELAY, (int) delay, __real_sceKernelDelayThread(delay)); }
struct hostent *__wrap_gethostbyname(const char *name)
{ PROBE_RETURN(struct hostent *, PROBE_DNS, -1, __real_gethostbyname(name)); }
extern int __real_connect(int, const struct sockaddr *, socklen_t);
int __wrap_connect(int fd, const struct sockaddr *address, socklen_t length)
{ PROBE_RETURN(int, PROBE_CONNECT, fd, __real_connect(fd, address, length)); }
extern ssize_t __real_recv(int, void *, size_t, int);
ssize_t __wrap_recv(int fd, void *data, size_t length, int flags)
{ PROBE_RETURN(ssize_t, PROBE_RECV, fd, __real_recv(fd, data, length, flags)); }
extern ssize_t __real_send(int, const void *, size_t, int);
ssize_t __wrap_send(int fd, const void *data, size_t length, int flags)
{ PROBE_RETURN(ssize_t, PROBE_SEND, fd, __real_send(fd, data, length, flags)); }

extern int __real_select(int, fd_set *, fd_set *, fd_set *, struct timeval *);
int __wrap_select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *errorfds, struct timeval *timeout)
{
    if (!probe_active()) return __real_select(nfds, readfds, writefds, errorfds, timeout);
    bool zero = timeout != NULL && timeout->tv_sec == 0 && timeout->tv_usec == 0;
    uint64_t start = probe_now();
    int result = __real_select(nfds, readfds, writefds, errorfds, timeout);
    int saved_errno = errno;
    uint64_t elapsed = probe_now() - start;
    if (zero && elapsed > probe_select_zero_max) probe_select_zero_max = elapsed;
    probe_record(PROBE_SELECT, start, nfds - 1);
    errno = saved_errno;
    return result;
}

extern int __real_curlx_nonblock(curl_socket_t, int);
int __wrap_curlx_nonblock(curl_socket_t fd, int nonblock)
{
    int result = __real_curlx_nonblock(fd, nonblock), saved_errno = errno;
    if (result == 0 && nonblock && probe_active()) {
        int value = -1;
        socklen_t length = sizeof(value);
        probe_nonblock_checks++;
        /* libc maps curl's descriptor to the firmware socket. Passing fd
           straight to sceNetInetGetsockopt would inspect a different socket. */
        if (getsockopt(fd, SOL_SOCKET, SO_NONBLOCK, &value, &length) < 0 || length != sizeof(value))
            probe_nonblock_unreadable++;
        else if (!value) probe_nonblock_bad++;
    }
    errno = saved_errno;
    return result;
}
