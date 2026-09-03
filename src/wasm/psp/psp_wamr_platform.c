#include "platform_api_vmcore.h"
#include "platform_api_extension.h"
#include "wasm_export.h"

#include <pspkernel.h>
#include <stdlib.h>
#include <string.h>

int bh_platform_init(void) { return 0; }
void bh_platform_destroy(void) {}
void *os_malloc(unsigned size) { return malloc(size); }
void *os_realloc(void *pointer, unsigned size) { return realloc(pointer, size); }
void os_free(void *pointer) { free(pointer); }
int os_dumps_proc_mem_info(char *out, unsigned size)
{ (void)out; (void)size; return -1; }
uint64 os_time_get_boot_us(void) { return sceKernelGetSystemTimeWide(); }
uint64 os_time_thread_cputime_us(void) { return sceKernelGetSystemTimeWide(); }
korp_tid os_self_thread(void) { return sceKernelGetThreadId(); }
uint8 *os_thread_get_stack_boundary(void) { return NULL; }
void os_thread_jit_write_protect_np(bool enabled) { (void)enabled; }

void *os_mmap(void *hint, size_t size, int prot, int flags, os_file_handle file)
{
    (void)hint; (void)prot; (void)flags; (void)file;
    /* The component runtime is initialized with Tilefinch's Budget-owned
       fixed pool. Linear memory must use that allocator too: allocating it
       from the PSP process heap escapes the page budget and commonly fails
       once the browser has fragmented the remaining system heap. */
    if (size == 0 || size > UINT32_MAX) return NULL;
    void *memory = wasm_runtime_malloc((uint32_t) size);
    if (memory != NULL) memset(memory, 0, size);
    return memory;
}
void os_munmap(void *address, size_t size)
{
    (void)size;
    if (address != NULL) wasm_runtime_free(address);
}
int os_mprotect(void *address, size_t size, int prot)
{ (void)address; (void)size; (void)prot; return 0; }
void os_dcache_flush(void) { sceKernelDcacheWritebackAll(); }
void os_icache_flush(void *start, size_t length)
{ (void)start; (void)length; sceKernelIcacheInvalidateAll(); }

static int wasm_mutex_init(korp_mutex *mutex, bool recursive)
{
    if (mutex == NULL) return -1;
    mutex->semaphore = sceKernelCreateSema(
        "tf_wasm_mutex", 0, 1, 1, NULL);
    mutex->owner = -1;
    mutex->recursion = 0;
    mutex->recursive = recursive;
    return mutex->semaphore < 0 ? -1 : 0;
}

int os_mutex_init(korp_mutex *mutex) { return wasm_mutex_init(mutex, false); }
int os_recursive_mutex_init(korp_mutex *mutex)
{ return wasm_mutex_init(mutex, true); }
int os_mutex_destroy(korp_mutex *mutex)
{
    if (mutex == NULL || mutex->semaphore < 0) return -1;
    int result = sceKernelDeleteSema(mutex->semaphore);
    mutex->semaphore = -1;
    mutex->owner = -1;
    mutex->recursion = 0;
    return result < 0 ? -1 : 0;
}
int os_mutex_lock(korp_mutex *mutex)
{
    if (mutex == NULL || mutex->semaphore < 0) return -1;
    int self = sceKernelGetThreadId();
    if (mutex->recursive && mutex->owner == self) {
        if (mutex->recursion == UINT_MAX) return -1;
        mutex->recursion++;
        return 0;
    }
    if (sceKernelWaitSema(mutex->semaphore, 1, NULL) < 0) return -1;
    mutex->owner = self;
    mutex->recursion = 1;
    return 0;
}
int os_mutex_unlock(korp_mutex *mutex)
{
    if (mutex == NULL || mutex->semaphore < 0
        || mutex->owner != sceKernelGetThreadId()
        || mutex->recursion == 0) return -1;
    if (--mutex->recursion != 0) return 0;
    mutex->owner = -1;
    return sceKernelSignalSema(mutex->semaphore, 1) < 0 ? -1 : 0;
}

int os_cond_init(korp_cond *condition)
{
    if (condition == NULL) return -1;
    condition->semaphore = sceKernelCreateSema(
        "tf_wasm_cond", 0, 0, 32767, NULL);
    condition->waiters = 0;
    return condition->semaphore < 0 ? -1 : 0;
}
int os_cond_destroy(korp_cond *condition)
{
    if (condition == NULL || condition->semaphore < 0) return -1;
    int result = sceKernelDeleteSema(condition->semaphore);
    condition->semaphore = -1;
    condition->waiters = 0;
    return result < 0 ? -1 : 0;
}
static int wasm_cond_wait(
    korp_cond *condition, korp_mutex *mutex, SceUInt *timeout)
{
    if (condition == NULL || mutex == NULL || condition->semaphore < 0)
        return -1;
    condition->waiters++;
    if (os_mutex_unlock(mutex) != 0) {
        condition->waiters--;
        return -1;
    }
    int waited = sceKernelWaitSema(condition->semaphore, 1, timeout);
    if (waited < 0 && condition->waiters != 0) condition->waiters--;
    if (os_mutex_lock(mutex) != 0) return -1;
    return waited < 0 ? BHT_TIMED_OUT : BHT_OK;
}
int os_cond_wait(korp_cond *condition, korp_mutex *mutex)
{ return wasm_cond_wait(condition, mutex, NULL); }
int os_cond_reltimedwait(korp_cond *condition, korp_mutex *mutex, uint64 useconds)
{
    SceUInt timeout = useconds > UINT_MAX ? UINT_MAX : (SceUInt)useconds;
    return wasm_cond_wait(condition, mutex, &timeout);
}
int os_cond_signal(korp_cond *condition)
{
    if (condition == NULL || condition->semaphore < 0) return -1;
    if (condition->waiters == 0) return 0;
    condition->waiters--;
    return sceKernelSignalSema(condition->semaphore, 1) < 0 ? -1 : 0;
}
int os_cond_broadcast(korp_cond *condition)
{
    if (condition == NULL || condition->semaphore < 0) return -1;
    unsigned waiters = condition->waiters;
    condition->waiters = 0;
    return waiters == 0
        || sceKernelSignalSema(condition->semaphore, (int)waiters) >= 0
        ? 0 : -1;
}

int os_thread_env_init(void) { return 0; }
void os_thread_env_destroy(void) {}
bool os_thread_env_inited(void) { return true; }
int os_thread_signal_init(os_signal_handler handler) { (void)handler; return 0; }
void os_thread_signal_destroy(void) {}
bool os_thread_signal_inited(void) { return true; }
void os_thread_jit_write_protect_np(bool enabled);
int os_usleep(uint32 useconds) { return sceKernelDelayThread(useconds); }
int os_blocking_op_init(void) { return 0; }
void os_begin_blocking_op(void) {}
void os_end_blocking_op(void) {}
os_file_handle os_get_invalid_handle(void) { return -1; }
os_raw_file_handle os_invalid_raw_handle(void) { return -1; }

/* A user PRX has no process crt0, but newlib's defensive abort path still
   references _exit. WebAssembly never uses it for ordinary traps. */
__attribute__((noreturn)) void _exit(int status)
{
    sceKernelExitDeleteThread(status);
    for (;;) {}
}
