#ifndef TILEFINCH_PSP_BOOT_HANDOFF_H
#define TILEFINCH_PSP_BOOT_HANDOFF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Private process-start arguments, never page or persistent state. */
#define PSP_BOOT_MODULE_PREFIX "--tilefinch-module-v1="
#define PSP_BOOT_MODULE_ARGUMENT_BYTES (sizeof(PSP_BOOT_MODULE_PREFIX) + 35u)
#define PSP_BOOT_MODULE_GATE_NAME "tilefinch-boot-gate"
#define PSP_BOOT_MODULE_READY_NAME "tilefinch-boot-ready"
#define PSP_BOOT_MODULE_WAIT_US 3000000u
#define PSP_BOOT_MODULE_JOIN_US 1000000u

typedef struct {
    int32_t gate, ready, module, thread;
} PspBootModuleArgument;

bool psp_boot_module_encode(char *output, size_t capacity,
                            const PspBootModuleArgument *argument);
bool psp_boot_module_decode(const char *text, PspBootModuleArgument *argument);
bool psp_boot_module_format(const uint8_t *header, size_t bytes);

typedef struct {
    void *context;
    int (*signal_ready)(void *, int32_t);
    int (*wait_gate)(void *, int32_t, uint32_t);
    int (*join_thread)(void *, int32_t, uint32_t);
    int (*delete_thread)(void *, int32_t);
    int (*delete_gate)(void *, int32_t);
    int (*stop_module)(void *, int32_t);
    int (*unload_module)(void *, int32_t);
    /* 1 means positively confirmed absent, 0 present, negative refused. */
    int (*module_absent)(void *, int32_t);
} PspBootModuleOps;

typedef enum {
    PSP_BOOT_MODULE_COMPLETE,
    PSP_BOOT_MODULE_INVALID,
    PSP_BOOT_MODULE_READY_FAILED,
    PSP_BOOT_MODULE_GATE_FAILED,
    PSP_BOOT_MODULE_JOIN_FAILED,
    PSP_BOOT_MODULE_THREAD_FAILED,
    PSP_BOOT_MODULE_SEMAPHORE_FAILED,
    PSP_BOOT_MODULE_STOP_FAILED,
    PSP_BOOT_MODULE_UNLOAD_FAILED,
    PSP_BOOT_MODULE_STILL_PRESENT
} PspBootModuleResult;

/* Only a consumed release gate transfers restart authority to the browser.
   Earlier refusals leave the launcher responsible for process replacement. */
bool psp_boot_module_receiver_owns_fallback(PspBootModuleResult result);

/* A refusal requires process replacement, never running the browser with
   uncertain launcher ownership. No allocation or libc initialization here. */
PspBootModuleResult psp_boot_module_receive(
    const PspBootModuleArgument *, const PspBootModuleOps *);

#endif
