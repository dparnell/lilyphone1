#ifndef __CALENDAR_STORE_H__
#define __CALENDAR_STORE_H__

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Appointments, kept in RAM and mirrored to SPIFFS the way the contact book
 * is. An event is one record whether it happens once or every week: the
 * repeat rule is a field, and "does it happen on this day" is a question the
 * store answers rather than a list of copies it keeps.
 *
 * Times are local wall-clock fields, not epochs. An event at 09:00 every Monday
 * is at 09:00 whatever the zone is set to when it comes round, which is what
 * anyone writing it down means. Nothing in here is thread-safe; only the LVGL
 * task calls it, like the phone store. */
#define CAL_TITLE_LEN   48
#define CAL_NOTES_LEN   96
#define CAL_EVENT_MAX   64

enum {
    CAL_REPEAT_NONE = 0,
    CAL_REPEAT_DAILY,
    CAL_REPEAT_WEEKDAYS,     // Monday to Friday
    CAL_REPEAT_WEEKLY,
    CAL_REPEAT_MONTHLY,
    CAL_REPEAT_YEARLY,
    CAL_REPEAT_MAX,
};

#define CAL_REMIND_NONE  (-1)   // remind_min: minutes before the start, or none

typedef struct {
    uint32_t id;
    char     title[CAL_TITLE_LEN];
    char     notes[CAL_NOTES_LEN];
    uint16_t year;               // the first occurrence, local
    uint8_t  month;              // 1..12
    uint8_t  day;                // 1..31
    uint8_t  hour;               // 0..23
    uint8_t  minute;             // 0..59
    bool     all_day;
    uint8_t  repeat;             // CAL_REPEAT_*
    int16_t  remind_min;         // CAL_REMIND_NONE, or minutes before
    uint32_t fired;              // start (local epoch) of the occurrence last reminded
} cal_event_t;

void     calendar_init(void);
unsigned calendar_revision(void);       // bumped on every change, for screens

int                calendar_count(void);
const cal_event_t *calendar_get(int idx);
const cal_event_t *calendar_find(uint32_t id);
bool               calendar_add(const cal_event_t *ev, uint32_t *id_out);
bool               calendar_update(const cal_event_t *ev);     // matched by id
bool               calendar_remove(uint32_t id);

/* Whether the event falls on this day, given its repeat rule. */
bool calendar_occurs_on(const cal_event_t *ev, int year, int month, int day);

/* The events on one day, earliest first. Returns how many were written. */
int calendar_day_events(int year, int month, int day, uint32_t *ids, int max);

/* Whether any event falls on this day - cheap enough to ask for 42 days. */
bool calendar_day_busy(int year, int month, int day);

/* The next event from now, today or within the coming week; NULL when none.
 * `when` receives the start of that occurrence as a local epoch. */
const cal_event_t *calendar_next_upcoming(uint32_t *when);

/* Called about once a minute. Returns an event whose reminder is due now, once
 * per occurrence, or NULL. Nothing is due while the clock has not been set. */
const cal_event_t *calendar_reminder_due(uint32_t *occurrence_start);

// Names and choice-cycling for the editor.
const char *calendar_repeat_name(int repeat);
const char *calendar_remind_name(int16_t remind_min);
int16_t     calendar_remind_next(int16_t remind_min);

// Calendar arithmetic the screens need too.
int  calendar_days_in_month(int year, int month);
int  calendar_weekday(int year, int month, int day);   // 0 = Monday .. 6 = Sunday
const char *calendar_month_name(int month);
const char *calendar_weekday_name(int weekday);

#ifdef __cplusplus
}
#endif
#endif
