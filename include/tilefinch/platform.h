#ifndef TILEFINCH_PLATFORM_H
#define TILEFINCH_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"

typedef struct {
    uint32_t buttons;
    int16_t analog_x;
    int16_t analog_y;
} TilefinchPlatformInput;

enum {
    TILEFINCH_PLATFORM_BUTTON_UP = 1u << 0,
    TILEFINCH_PLATFORM_BUTTON_DOWN = 1u << 1,
    TILEFINCH_PLATFORM_BUTTON_LEFT = 1u << 2,
    TILEFINCH_PLATFORM_BUTTON_RIGHT = 1u << 3,
    TILEFINCH_PLATFORM_BUTTON_CROSS = 1u << 4,
    TILEFINCH_PLATFORM_BUTTON_CIRCLE = 1u << 5,
    TILEFINCH_PLATFORM_BUTTON_LTRIGGER = 1u << 6,
    TILEFINCH_PLATFORM_BUTTON_RTRIGGER = 1u << 7
};

typedef struct {
    void *context;
    uint64_t (*wall_time_ns)(void *context);
    uint64_t (*monotonic_time_ns)(void *context);
    /* Native microsecond clock for 32-bit devices whose kernel already
       exposes this unit. Hot scheduling checkpoints should use this rather
       than manufacturing nanoseconds and dividing them back down. */
    uint64_t (*monotonic_time_us)(void *context);
    bool (*secure_random)(void *context, void *data, size_t length);
    bool (*read_asset)(void *context, Budget *budget, const char *path,
                       size_t maximum_bytes, unsigned char **data,
                       size_t *length);
    bool (*poll_input)(void *context, TilefinchPlatformInput *input);
    bool (*present_rgb565)(void *context, const uint16_t *pixels,
                           size_t width, size_t height, size_t stride_pixels);
    /* Called at deterministic engine work boundaries. Returning false asks
       the current operation to cancel without committing partial state. */
    bool (*cooperate)(void *context, const char *phase,
                      size_t completed_work_units);
    void (*log_message)(void *context, const char *message);
} TilefinchPlatformServices;

void tilefinch_platform_set_services(const TilefinchPlatformServices *services);

uint64_t tilefinch_platform_wall_time_ns(void);
uint64_t tilefinch_platform_monotonic_time_ns(void);
uint64_t tilefinch_platform_monotonic_time_us(void);
bool tilefinch_platform_secure_random(void *data, size_t length);
bool tilefinch_platform_read_asset(Budget *budget, const char *path,
                                size_t maximum_bytes, unsigned char **data,
                                size_t *length);
bool tilefinch_platform_poll_input(TilefinchPlatformInput *input);
bool tilefinch_platform_present_rgb565(const uint16_t *pixels, size_t width,
                                    size_t height, size_t stride_pixels);
bool tilefinch_platform_cooperate(const char *phase,
                               size_t completed_work_units);
void tilefinch_platform_log_message(const char *message);

#endif
