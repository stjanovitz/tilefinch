#ifndef TILEFINCH_PSP_TIME_H
#define TILEFINCH_PSP_TIME_H

#include <time.h>

/* Keep the legacy-safe Unix timestamp range used by PSP TLS separate from
   malformed or unreadable RTC input. Zero is a fail-closed return value, not
   a fabricated timestamp or an assertion that the clock read succeeded. */
typedef enum {
    PSP_TIME_OK = 0,
    PSP_TIME_UNREADABLE,
    PSP_TIME_INVALID_FIELDS,
    PSP_TIME_OUT_OF_TIME_T_RANGE
} PspTimeStatus;

typedef struct {
    unsigned year;
    unsigned month;
    unsigned day;
    unsigned hour;
    unsigned minute;
    unsigned second;
    unsigned microsecond;
} PspTimeFields;

PspTimeStatus psp_time_classify_utc(
    const PspTimeFields *fields, time_t *epoch);
const char *psp_time_status_name(PspTimeStatus status);

/* PSP implementation backed by sceRtcGetCurrentClock(). */
PspTimeStatus psp_time_read_utc(time_t *epoch);
PspTimeStatus psp_time_read_utc_fields(
    PspTimeFields *fields, time_t *epoch);

#endif
