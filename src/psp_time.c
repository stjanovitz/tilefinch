#include <psprtc.h>

#include <time.h>

#include "tilefinch/psp_time.h"

_Static_assert(sizeof(time_t) >= 4u,
               "PSP TLS clock policy requires a Unix-sized time_t");

PspTimeStatus psp_time_read_utc_fields(
    PspTimeFields *fields, time_t *epoch)
{
    if (fields != NULL) *fields = (PspTimeFields) {0};
    if (epoch != NULL) *epoch = 0;
    ScePspDateTime date = {0};
    if (sceRtcGetCurrentClock(&date, 0) < 0) return PSP_TIME_UNREADABLE;
    PspTimeFields sampled = {
        .year = date.year,
        .month = date.month,
        .day = date.day,
        .hour = date.hour,
        .minute = date.minute,
        .second = date.second,
        .microsecond = date.microsecond
    };
    if (fields != NULL) *fields = sampled;
    return psp_time_classify_utc(&sampled, epoch);
}

PspTimeStatus psp_time_read_utc(time_t *epoch)
{
    return psp_time_read_utc_fields(NULL, epoch);
}

/*
 * mbedTLS calls libc time() during X.509 validation. PSP newlib's conversion
 * has not been reliable on hardware, so use the RTC's UTC clock directly.
 */
time_t time(time_t *output)
{
    time_t result = 0;
    (void) psp_time_read_utc(&result);
    if (output != NULL) *output = result;
    return result;
}
