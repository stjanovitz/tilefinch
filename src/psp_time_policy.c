#include "tilefinch/psp_time.h"

#include <stdbool.h>
#include <stdint.h>

static bool psp_time_leap_year(unsigned year)
{
    return (year % 4u == 0u && year % 100u != 0u)
        || year % 400u == 0u;
}

static unsigned psp_time_days_in_month(unsigned year, unsigned month)
{
    static const unsigned days[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    if (month < 1u || month > 12u) return 0u;
    return month == 2u && psp_time_leap_year(year)
        ? 29u : days[month - 1u];
}

PspTimeStatus psp_time_classify_utc(
    const PspTimeFields *fields, time_t *epoch)
{
    if (epoch != NULL) *epoch = 0;
    if (fields == NULL) return PSP_TIME_UNREADABLE;
    if (fields->month < 1u || fields->month > 12u
        || fields->day < 1u
        || fields->day > psp_time_days_in_month(
               fields->year, fields->month)
        || fields->hour > 23u || fields->minute > 59u
        || fields->second > 59u || fields->microsecond > 999999u) {
        return PSP_TIME_INVALID_FIELDS;
    }
    if (fields->year < 1970u || fields->year > 2037u)
        return PSP_TIME_OUT_OF_TIME_T_RANGE;

    uint64_t days = 0;
    for (unsigned year = 1970u; year < fields->year; year++)
        days += psp_time_leap_year(year) ? 366u : 365u;
    for (unsigned month = 1u; month < fields->month; month++)
        days += psp_time_days_in_month(fields->year, month);
    days += fields->day - 1u;
    uint64_t value = days * UINT64_C(86400)
        + (uint64_t) fields->hour * UINT64_C(3600)
        + (uint64_t) fields->minute * UINT64_C(60)
        + fields->second;
    if (value > UINT64_C(0x7fffffff))
        return PSP_TIME_OUT_OF_TIME_T_RANGE;
    if (epoch != NULL) *epoch = (time_t) value;
    return PSP_TIME_OK;
}

const char *psp_time_status_name(PspTimeStatus status)
{
    static const char *const names[] = {
        "ok", "unreadable", "invalid-fields", "out-of-time-t-range"
    };
    return (unsigned) status <= PSP_TIME_OUT_OF_TIME_T_RANGE
        ? names[status] : "unknown";
}
