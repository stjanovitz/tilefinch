#ifndef _PLATFORM_INTERNAL_H
#define _PLATFORM_INTERNAL_H

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#define BH_PLATFORM_PSP 1
typedef int korp_tid;
typedef struct {
    int semaphore;
    int owner;
    unsigned recursion;
    bool recursive;
} korp_mutex;
typedef struct {
    int semaphore;
    unsigned waiters;
} korp_cond;
typedef int korp_thread;
typedef korp_mutex korp_rwlock;
typedef int korp_sem;

#define OS_THREAD_MUTEX_INITIALIZER { -1, -1, 0, false }
#define BH_APPLET_PRESERVED_STACK_SIZE (2 * BH_KB)
#define BH_THREAD_DEFAULT_PRIORITY 100
#define os_printf printf
#define os_vprintf vprintf
#define os_getpagesize() 4096
#define BH_HAS_DLFCN 0
#define BH_TIME_T_MAX INT32_MAX
#define os_thread_local_attribute

typedef int os_file_handle;
typedef int os_raw_file_handle;
typedef void *os_dir_stream;
typedef void (*os_signal_handler)(void *signal_address);
os_file_handle os_get_invalid_handle(void);
os_raw_file_handle os_invalid_raw_handle(void);
int os_thread_signal_init(os_signal_handler handler);
void os_thread_signal_destroy(void);
bool os_thread_signal_inited(void);

#endif
