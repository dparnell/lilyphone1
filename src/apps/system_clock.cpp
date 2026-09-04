/**
 * The one place the system clock is set from.
 *
 * There are two sources that can tell this device what time it is - the GPS and
 * the cellular network - and they need arbitrating: whichever wrote last would
 * otherwise win, and the network keeps refreshing. They also need a shared
 * UTC-to-epoch conversion. mktime() is not it: mktime reads its input as *local*
 * time, so once a time zone is in force (which the network gives us, and which
 * is the whole point of reading the clock off the modem) feeding it UTC lands
 * the clock out by the offset.
 */
#include <Arduino.h>
#include <sys/time.h>
#include <time.h>
#include "system_clock.h"

static clock_source_t clock_src        = CLOCK_SRC_NONE;
static int            clock_offset_min = 0;
static bool           clock_offset_set = false;

/* Days between 1970-01-01 and the given civil date, proleptic Gregorian and
 * valid for any year. Howard Hinnant's days_from_civil. */
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;

    const int64_t  era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);                     // [0, 399]
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; // [0, 365]
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;          // [0, 146096]

    return era * 146097 + (int64_t)doe - 719468;
}

uint32_t system_clock_epoch_from_utc(int year, int month, int day,
                                     int hour, int minute, int second)
{
    // Reject the placeholder dates a modem reports before the network has
    // given it anything, and anything else obviously wrong.
    //
    // The day is allowed to sit one outside the month because converting a
    // local time to UTC can step over midnight: days_from_civil is linear in
    // the day, so a 0 resolves to the last of the previous month and a 32 to
    // the first of the next, which saves every caller normalising by hand.
    if(year < 2024 || year > 2099) return 0;
    if(month < 1 || month > 12 || day < 0 || day > 32) return 0;
    if(hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60) return 0;

    int64_t epoch = days_from_civil(year, (unsigned)month, (unsigned)day) * 86400LL
                  + hour * 3600LL + minute * 60LL + second;

    return epoch > 0 ? (uint32_t)epoch : 0;
}

bool system_clock_set_utc(clock_source_t src, int year, int month, int day,
                          int hour, int minute, int second)
{
    if(src == CLOCK_SRC_NONE) return false;
    if(src < clock_src) return false; // a better source already set the clock

    uint32_t epoch = system_clock_epoch_from_utc(year, month, day, hour, minute, second);
    if(epoch == 0) return false;

    struct timeval tv;
    tv.tv_sec  = (time_t)epoch;
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);

    clock_src = src;

    Serial.printf("[CLOCK] set to %04d-%02d-%02d %02d:%02d:%02d UTC from %s\n",
                  year, month, day, hour, minute, second, system_clock_source_name());
    return true;
}

void system_clock_set_utc_offset(int minutes)
{
    // Sanity: real zones run from UTC-12 to UTC+14.
    if(minutes < -12 * 60 || minutes > 14 * 60) return;
    if(clock_offset_set && clock_offset_min == minutes) return;

    clock_offset_min = minutes;
    clock_offset_set = true;

    // POSIX writes the offset as the amount to ADD to local time to reach UTC,
    // so the sign is inverted against the usual "UTC+10" way of saying it.
    int  posix = -minutes;
    char tz[24];
    snprintf(tz, sizeof(tz), "UTC%c%d:%02d", posix < 0 ? '-' : '+', abs(posix) / 60, abs(posix) % 60);

    setenv("TZ", tz, 1);
    tzset();

    Serial.printf("[CLOCK] local offset %+d min, TZ=%s\n", minutes, tz);
}

bool system_clock_is_set(void)
{
    return clock_src != CLOCK_SRC_NONE;
}

clock_source_t system_clock_get_source(void)
{
    return clock_src;
}

const char *system_clock_source_name(void)
{
    switch(clock_src) {
        case CLOCK_SRC_NETWORK: return "network";
        case CLOCK_SRC_GPS:     return "GPS";
        default:                return "not set";
    }
}

int system_clock_get_utc_offset(void)
{
    return clock_offset_min;
}

bool system_clock_has_utc_offset(void)
{
    return clock_offset_set;
}
