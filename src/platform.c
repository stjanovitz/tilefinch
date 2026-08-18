#include "tilefinch/platform.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define budget_malloc(b, s) budget_malloc_category((b), BUDGET_CATEGORY_RESOURCE, (s))
#define budget_calloc(b, n, s) budget_calloc_category((b), BUDGET_CATEGORY_RESOURCE, (n), (s))
#define budget_realloc(b, p, s) budget_realloc_category((b), BUDGET_CATEGORY_RESOURCE, (p), (s))

#if defined(__linux__)
#include <sys/random.h>
#endif

static TilefinchPlatformServices installed_services;

void tilefinch_platform_set_services(const TilefinchPlatformServices *services)
{
    if (services == NULL) memset(&installed_services, 0,
                                 sizeof(installed_services));
    else installed_services = *services;
}

uint64_t tilefinch_platform_wall_time_ns(void)
{
    if (installed_services.wall_time_ns != NULL) {
        return installed_services.wall_time_ns(installed_services.context);
    }
#if defined(TIME_UTC)
    struct timespec value;
    if (timespec_get(&value, TIME_UTC) == TIME_UTC) {
        return (uint64_t) value.tv_sec * UINT64_C(1000000000)
               + (uint64_t) value.tv_nsec;
    }
#endif
    time_t seconds = time(NULL);
    return seconds < 0 ? 0
        : (uint64_t) seconds * UINT64_C(1000000000);
}

uint64_t tilefinch_platform_monotonic_time_ns(void)
{
    if (installed_services.monotonic_time_ns != NULL) {
        return installed_services.monotonic_time_ns(
            installed_services.context);
    }
#if defined(CLOCK_MONOTONIC)
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) == 0) {
        return (uint64_t) value.tv_sec * UINT64_C(1000000000)
               + (uint64_t) value.tv_nsec;
    }
#endif
    clock_t ticks = clock();
    if (ticks < 0 || CLOCKS_PER_SEC <= 0) return tilefinch_platform_wall_time_ns();
    return (uint64_t) ticks * UINT64_C(1000000000)
           / (uint64_t) CLOCKS_PER_SEC;
}

uint64_t tilefinch_platform_monotonic_time_us(void)
{
    if (installed_services.monotonic_time_us != NULL) {
        return installed_services.monotonic_time_us(
            installed_services.context);
    }
    return tilefinch_platform_monotonic_time_ns() / UINT64_C(1000);
}

const char *tilefinch_platform_preferred_language(void)
{
    if (installed_services.preferred_language != NULL) {
        const char *language = installed_services.preferred_language(
            installed_services.context);
        if (language != NULL && language[0] != '\0') return language;
    }
    return "en";
}

TilefinchDateFormat tilefinch_platform_preferred_date_format(void)
{
    if (installed_services.preferred_date_format != NULL) {
        TilefinchDateFormat format =
            installed_services.preferred_date_format(
                installed_services.context);
        if (format >= TILEFINCH_DATE_FORMAT_YEAR_MONTH_DAY
            && format <= TILEFINCH_DATE_FORMAT_DAY_MONTH_YEAR) {
            return format;
        }
    }
    return TILEFINCH_DATE_FORMAT_YEAR_MONTH_DAY;
}

bool tilefinch_platform_secure_random(void *data, size_t length)
{
    if (data == NULL && length != 0) return false;
    if (installed_services.secure_random != NULL) {
        return installed_services.secure_random(installed_services.context,
                                                data, length);
    }
#if defined(__APPLE__)
    arc4random_buf(data, length);
    return true;
#elif defined(__linux__)
    size_t offset = 0;
    while (offset < length) {
        ssize_t received = getrandom((unsigned char *) data + offset,
                                     length - offset, 0);
        if (received < 0 && errno == EINTR) continue;
        if (received <= 0) return false;
        offset += (size_t) received;
    }
    return true;
#else
    (void) data;
    (void) length;
    return false;
#endif
}

bool tilefinch_platform_read_asset(Budget *budget, const char *path,
                                size_t maximum_bytes, unsigned char **data,
                                size_t *length)
{
    if (budget == NULL || path == NULL || data == NULL || length == NULL
        || maximum_bytes == 0) return false;
    *data = NULL;
    *length = 0;
    if (installed_services.read_asset != NULL) {
        return installed_services.read_asset(
            installed_services.context, budget, path, maximum_bytes,
            data, length);
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return false; }
    long end = ftell(file);
    if (end <= 0 || (size_t) end > maximum_bytes
        || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    unsigned char *contents = budget_malloc(budget, (size_t) end);
    if (contents == NULL) { fclose(file); return false; }
    size_t received = fread(contents, 1, (size_t) end, file);
    bool ok = received == (size_t) end && ferror(file) == 0;
    fclose(file);
    if (!ok) {
        budget_free(budget, contents);
        return false;
    }
    *data = contents;
    *length = received;
    return true;
}

bool tilefinch_platform_poll_input(TilefinchPlatformInput *input)
{
    if (input == NULL || installed_services.poll_input == NULL) return false;
    return installed_services.poll_input(installed_services.context, input);
}

bool tilefinch_platform_present_rgb565(const uint16_t *pixels, size_t width,
                                    size_t height, size_t stride_pixels)
{
    if (pixels == NULL || width == 0 || height == 0
        || stride_pixels < width
        || installed_services.present_rgb565 == NULL) return false;
    return installed_services.present_rgb565(
        installed_services.context, pixels, width, height, stride_pixels);
}

bool tilefinch_platform_cooperate(const char *phase,
                               size_t completed_work_units)
{
    if (installed_services.cooperate == NULL) return true;
    return installed_services.cooperate(
        installed_services.context, phase == NULL ? "engine" : phase,
        completed_work_units);
}

void tilefinch_platform_log_message(const char *message)
{
    if (message == NULL) return;
    if (installed_services.log_message != NULL) {
        installed_services.log_message(installed_services.context, message);
    } else {
        fputs(message, stderr);
        fputc('\n', stderr);
    }
}
