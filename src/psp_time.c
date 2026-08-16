#include <psprtc.h>

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

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
    if (month < 1 || month > 12) return 0;
    return month == 2 && psp_time_leap_year(year)
        ? 29 : days[month - 1];
}

static bool psp_time_epoch_utc(const ScePspDateTime *date,
                               uint64_t *epoch)
{
    if (date == NULL || epoch == NULL || date->year < 1970
        || date->year > 2037 || date->month < 1 || date->month > 12
        || date->day < 1
        || date->day > psp_time_days_in_month(
               date->year, date->month)
        || date->hour > 23 || date->minute > 59
        || date->second > 59) return false;
    uint64_t days = 0;
    for (unsigned year = 1970; year < date->year; year++)
        days += psp_time_leap_year(year) ? 366u : 365u;
    for (unsigned month = 1; month < date->month; month++)
        days += psp_time_days_in_month(date->year, month);
    days += date->day - 1u;
    *epoch = days * UINT64_C(86400)
        + (uint64_t) date->hour * UINT64_C(3600)
        + (uint64_t) date->minute * UINT64_C(60)
        + date->second;
    return *epoch <= UINT64_C(0x7fffffff);
}

/*
 * mbedTLS calls libc time() during X.509 validation. PSP newlib's conversion
 * has not been reliable on hardware, so use the RTC's UTC clock directly.
 */
time_t time(time_t *output)
{
    ScePspDateTime date;
    uint64_t epoch = 0;
    time_t result = 0;
    if (sceRtcGetCurrentClock(&date, 0) >= 0
        && sceRtcCheckValid(&date) == 0
        && psp_time_epoch_utc(&date, &epoch)) {
        result = (time_t) epoch;
    }
    if (output != NULL) *output = result;
    return result;
}
