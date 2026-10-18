#include "tilefinch/psp_boot_handoff.h"

#include <limits.h>
#include <string.h>

static bool argument_valid(const PspBootModuleArgument *arg)
{
    return arg != NULL && arg->gate > 0 && arg->ready > 0
        && arg->module > 0 && arg->thread > 0
        && arg->gate != arg->ready && arg->module != arg->thread;
}

bool psp_boot_module_encode(char *output, size_t capacity,
                            const PspBootModuleArgument *arg)
{
    if (output == NULL || capacity < PSP_BOOT_MODULE_ARGUMENT_BYTES
        || !argument_valid(arg)) return false;
    static const char hex[] = "0123456789abcdef";
    const int32_t ids[] = {arg->gate, arg->ready, arg->module, arg->thread};
    size_t at = sizeof(PSP_BOOT_MODULE_PREFIX) - 1u;
    memcpy(output, PSP_BOOT_MODULE_PREFIX, at);
    for (unsigned item = 0; item < 4u; item++) {
        uint32_t id = (uint32_t) ids[item];
        for (unsigned digit = 0; digit < 8u; digit++)
            output[at++] = hex[(id >> (28u - digit * 4u)) & 15u];
        if (item != 3u) output[at++] = ',';
    }
    output[at] = '\0';
    return true;
}

bool psp_boot_module_decode(const char *text, PspBootModuleArgument *arg)
{
    if (text == NULL || arg == NULL) return false;
    size_t at = 0;
    for (; at < sizeof(PSP_BOOT_MODULE_PREFIX) - 1u; at++)
        if (text[at] != PSP_BOOT_MODULE_PREFIX[at]) return false;
    uint32_t ids[4] = {0};
    for (unsigned item = 0; item < 4u; item++) {
        for (unsigned digit = 0; digit < 8u; digit++) {
            unsigned char c = (unsigned char) text[at++];
            unsigned n = c >= '0' && c <= '9' ? (unsigned) (c - '0')
                : c >= 'a' && c <= 'f' ? (unsigned) (c - 'a') + 10u : 16u;
            if (n == 16u) return false;
            ids[item] = (ids[item] << 4u) | n;
        }
        if (ids[item] > INT32_MAX) return false;
        if (item != 3u && text[at++] != ',') return false;
    }
    if (text[at] != '\0') return false;
    PspBootModuleArgument parsed = {(int32_t) ids[0], (int32_t) ids[1],
                                    (int32_t) ids[2], (int32_t) ids[3]};
    if (!argument_valid(&parsed)) return false;
    *arg = parsed;
    return true;
}

bool psp_boot_module_format(const uint8_t *h, size_t n)
{
    return h != NULL && n >= 20u && h[0] == 0x7fu && h[1] == 'E'
        && h[2] == 'L' && h[3] == 'F' && h[4] == 1u && h[5] == 1u
        && h[6] == 1u && h[16] == 0xa0u && h[17] == 0xffu
        && h[18] == 8u && h[19] == 0u;
}

PspBootModuleResult psp_boot_module_receive(
    const PspBootModuleArgument *arg, const PspBootModuleOps *ops)
{
    if (!argument_valid(arg) || ops == NULL || ops->signal_ready == NULL
        || ops->wait_gate == NULL || ops->join_thread == NULL
        || ops->delete_thread == NULL || ops->delete_gate == NULL
        || ops->stop_module == NULL || ops->unload_module == NULL
        || ops->module_absent == NULL) return PSP_BOOT_MODULE_INVALID;
    void *ctx = ops->context;
    if (ops->signal_ready(ctx, arg->ready) < 0) return PSP_BOOT_MODULE_READY_FAILED;
    if (ops->wait_gate(ctx, arg->gate, PSP_BOOT_MODULE_WAIT_US) < 0)
        return PSP_BOOT_MODULE_GATE_FAILED;
    if (ops->join_thread(ctx, arg->thread, PSP_BOOT_MODULE_JOIN_US) < 0)
        return PSP_BOOT_MODULE_JOIN_FAILED;
    if (ops->delete_thread(ctx, arg->thread) < 0) return PSP_BOOT_MODULE_THREAD_FAILED;
    if (ops->delete_gate(ctx, arg->gate) < 0) return PSP_BOOT_MODULE_SEMAPHORE_FAILED;
    if (ops->stop_module(ctx, arg->module) < 0) return PSP_BOOT_MODULE_STOP_FAILED;
    if (ops->unload_module(ctx, arg->module) < 0) return PSP_BOOT_MODULE_UNLOAD_FAILED;
    if (ops->module_absent(ctx, arg->module) != 1) return PSP_BOOT_MODULE_STILL_PRESENT;
    return PSP_BOOT_MODULE_COMPLETE;
}

bool psp_boot_module_receiver_owns_fallback(PspBootModuleResult result)
{
    return result >= PSP_BOOT_MODULE_JOIN_FAILED
        && result <= PSP_BOOT_MODULE_STILL_PRESENT;
}
