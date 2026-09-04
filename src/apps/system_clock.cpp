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
#include <Preferences.h>
#include <sys/time.h>
#include <time.h>
#include "system_clock.h"
#include "timezone_db.h"

#define CLOCK_PREFS_NAMESPACE "clock"
#define CLOCK_PREFS_ZONE      "tz_name"

static clock_source_t clock_src        = CLOCK_SRC_NONE;
static int            clock_offset_min = 0;
static bool           clock_offset_set = false;

static bool clock_zone_manual = false;
static char clock_zone_label[TIMEZONE_NAME_MAX] = "UTC";

static void clock_apply_tz(const char *posix)
{
    setenv("TZ", posix, 1);
    tzset();
    Serial.printf("[CLOCK] TZ=%s\n", posix);
}

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

    // Remember what the network said even while a manual zone is in force, so
    // clearing the zone can fall straight back to it.
    bool changed = !clock_offset_set || clock_offset_min != minutes;

    clock_offset_min = minutes;
    clock_offset_set = true;

    if(clock_zone_manual || !changed) return;

    // POSIX writes the offset as the amount to ADD to local time to reach UTC,
    // so the sign is inverted against the usual "UTC+10" way of saying it.
    int  posix = -minutes;
    char tz[24];
    snprintf(tz, sizeof(tz), "UTC%c%d:%02d", posix < 0 ? '-' : '+', abs(posix) / 60, abs(posix) % 60);

    snprintf(clock_zone_label, sizeof(clock_zone_label), "UTC%c%d:%02d",
             minutes < 0 ? '-' : '+', abs(minutes) / 60, abs(minutes) % 60);

    clock_apply_tz(tz);
}

void system_clock_set_zone(const char *name, const char *posix)
{
    if(name == NULL || posix == NULL || name[0] == '\0' || posix[0] == '\0') return;

    clock_zone_manual = true;
    snprintf(clock_zone_label, sizeof(clock_zone_label), "%s", name);
    clock_apply_tz(posix);

    Preferences prefs;
    if(prefs.begin(CLOCK_PREFS_NAMESPACE, false)) {
        prefs.putString(CLOCK_PREFS_ZONE, name);
        prefs.end();
    }
}

void system_clock_clear_zone(void)
{
    clock_zone_manual = false;

    Preferences prefs;
    if(prefs.begin(CLOCK_PREFS_NAMESPACE, false)) {
        prefs.remove(CLOCK_PREFS_ZONE);
        prefs.end();
    }

    if(clock_offset_set) {
        // Re-apply the network's offset, which set_utc_offset skipped while the
        // manual zone was in force.
        int saved = clock_offset_min;
        clock_offset_set = false;
        system_clock_set_utc_offset(saved);
    } else {
        snprintf(clock_zone_label, sizeof(clock_zone_label), "UTC");
        clock_apply_tz("UTC0");
    }
}

bool system_clock_zone_is_manual(void)
{
    return clock_zone_manual;
}

const char *system_clock_zone_label(void)
{
    return clock_zone_label;
}

void system_clock_init(void)
{
    // Start from a known zone rather than whatever the C library defaults to.
    clock_apply_tz("UTC0");

    Preferences prefs;
    if(!prefs.begin(CLOCK_PREFS_NAMESPACE, true)) return;

    String saved = prefs.getString(CLOCK_PREFS_ZONE, "");
    prefs.end();

    if(saved.length() == 0) return;

    int idx = timezone_find(saved.c_str());
    const char *posix = timezone_posix_at(idx);
    if(posix == NULL) {
        Serial.printf("[CLOCK] saved zone '%s' is not in the table\n", saved.c_str());
        return;
    }

    clock_zone_manual = true;
    snprintf(clock_zone_label, sizeof(clock_zone_label), "%s", saved.c_str());
    clock_apply_tz(posix);
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
