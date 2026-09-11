/**
 * Appointments, in RAM and on SPIFFS. See calendar_store.h.
 *
 * The file is one event per line, tab separated, like the contact book, and
 * is rewritten whole on every change - there are at most 64 of them and a
 * change is a person pressing Save.
 *
 * Local time throughout. mktime() is the right tool here, unlike in the clock
 * code: these fields *are* local wall-clock time, which is exactly what mktime
 * takes, and a reminder is compared against time(NULL) after the conversion.
 */
#include <Arduino.h>
#include "FS.h"
#include "SPIFFS.h"
#include <time.h>
#include "calendar_store.h"
#include "system_clock.h"

#define EVENTS_PATH "/events.tsv"

static cal_event_t *events   = NULL;
static int          event_num = 0;
static uint32_t     next_id   = 1;
static unsigned     revision  = 0;

//************************************[ helpers ]*******************************
static void str_copy(char *dst, const char *src, size_t len)
{
    if(src == NULL) { dst[0] = '\0'; return; }
    strncpy(dst, src, len - 1);
    dst[len - 1] = '\0';
}

static void escape_append(String &out, const char *src)
{
    for(const char *p = src; *p; p++) {
        switch(*p) {
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += *p;     break;
        }
    }
}

static void unescape(const char *src, char *dst, size_t len)
{
    size_t o = 0;
    for(const char *p = src; *p && o + 1 < len; p++) {
        if(*p == '\\' && p[1]) {
            p++;
            switch(*p) {
                case 'n': dst[o++] = '\n'; break;
                case 'r': dst[o++] = '\r'; break;
                case 't': dst[o++] = '\t'; break;
                default:  dst[o++] = *p;   break;
            }
        } else {
            dst[o++] = *p;
        }
    }
    dst[o] = '\0';
}

static int split_fields(char *line, char **fields, int max_fields)
{
    int n = 0;
    char *p = line;
    while(n < max_fields) {
        fields[n++] = p;
        char *tab = strchr(p, '\t');
        if(tab == NULL) break;
        *tab = '\0';
        p = tab + 1;
    }
    return n;
}

//************************************[ dates ]*********************************
int calendar_days_in_month(int year, int month)
{
    static const int dim[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if(month < 1 || month > 12) return 30;
    if(month == 2) {
        bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
        return leap ? 29 : 28;
    }
    return dim[month - 1];
}

/* Zeller, rearranged so Monday is 0: the week starts on Monday on the grid. */
int calendar_weekday(int year, int month, int day)
{
    if(month < 3) { month += 12; year--; }
    int k = year % 100, j = year / 100;
    int h = (day + 13 * (month + 1) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;   // 0 = Saturday
    return (h + 5) % 7;
}

const char *calendar_month_name(int month)
{
    static const char *names[] = { "January", "February", "March", "April", "May", "June", "July",
                                   "August", "September", "October", "November", "December" };
    return (month >= 1 && month <= 12) ? names[month - 1] : "?";
}

const char *calendar_weekday_name(int weekday)
{
    static const char *names[] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
    return (weekday >= 0 && weekday < 7) ? names[weekday] : "?";
}

static uint32_t local_epoch(int year, int month, int day, int hour, int minute)
{
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year  = year - 1900;
    t.tm_mon   = month - 1;
    t.tm_mday  = day;
    t.tm_hour  = hour;
    t.tm_min   = minute;
    t.tm_isdst = -1;
    time_t e = mktime(&t);
    return e < 0 ? 0 : (uint32_t)e;
}

/* The date `offset` days from now, in local time. */
static void local_date_offset(int offset, int *year, int *month, int *day)
{
    time_t t = time(NULL) + (time_t)offset * 86400;
    struct tm *lt = localtime(&t);
    *year  = lt->tm_year + 1900;
    *month = lt->tm_mon + 1;
    *day   = lt->tm_mday;
}

static int date_cmp(int y1, int m1, int d1, int y2, int m2, int d2)
{
    if(y1 != y2) return y1 - y2;
    if(m1 != m2) return m1 - m2;
    return d1 - d2;
}

//************************************[ file ]**********************************
static void events_read(void)
{
    event_num = 0;
    next_id   = 1;

    File f = SPIFFS.open(EVENTS_PATH, FILE_READ);
    if(!f) return;

    while(f.available() && event_num < CAL_EVENT_MAX) {
        String line = f.readStringUntil('\n');
        line.trim();
        if(line.length() == 0) continue;

        char buf[CAL_TITLE_LEN * 2 + CAL_NOTES_LEN * 2 + 96];
        str_copy(buf, line.c_str(), sizeof(buf));

        char *fld[12];
        if(split_fields(buf, fld, 12) < 12) continue;

        cal_event_t *ev = &events[event_num];
        memset(ev, 0, sizeof(*ev));
        ev->id = strtoul(fld[0], NULL, 10);
        unescape(fld[1], ev->title, CAL_TITLE_LEN);
        unescape(fld[2], ev->notes, CAL_NOTES_LEN);
        ev->year       = atoi(fld[3]);
        ev->month      = atoi(fld[4]);
        ev->day        = atoi(fld[5]);
        ev->hour       = atoi(fld[6]);
        ev->minute     = atoi(fld[7]);
        ev->all_day    = atoi(fld[8]) != 0;
        ev->repeat     = atoi(fld[9]);
        ev->remind_min = atoi(fld[10]);
        ev->fired      = strtoul(fld[11], NULL, 10);

        if(ev->repeat >= CAL_REPEAT_MAX) ev->repeat = CAL_REPEAT_NONE;
        if(ev->id >= next_id) next_id = ev->id + 1;
        event_num++;
    }
    f.close();

    Serial.printf("[CAL] loaded %d events\n", event_num);
}

static bool events_save(void)
{
    File f = SPIFFS.open(EVENTS_PATH, FILE_WRITE);
    if(!f) {
        Serial.println("[CAL] cannot write events");
        return false;
    }
    for(int i = 0; i < event_num; i++) {
        const cal_event_t *ev = &events[i];
        String line;
        line += String(ev->id);          line += '\t';
        escape_append(line, ev->title);  line += '\t';
        escape_append(line, ev->notes);  line += '\t';
        line += String(ev->year);        line += '\t';
        line += String(ev->month);       line += '\t';
        line += String(ev->day);         line += '\t';
        line += String(ev->hour);        line += '\t';
        line += String(ev->minute);      line += '\t';
        line += String(ev->all_day ? 1 : 0); line += '\t';
        line += String(ev->repeat);      line += '\t';
        line += String(ev->remind_min);  line += '\t';
        line += String(ev->fired);       line += '\n';
        f.print(line);
    }
    f.close();
    return true;
}

//************************************[ public API ]****************************
void calendar_init(void)
{
    if(events == NULL) {
        events = (cal_event_t *)ps_calloc(CAL_EVENT_MAX, sizeof(cal_event_t));
        if(events == NULL) events = (cal_event_t *)calloc(CAL_EVENT_MAX, sizeof(cal_event_t));
        if(events == NULL) { Serial.println("[CAL] no room for events"); return; }
    }
    events_read();
}

unsigned calendar_revision(void) { return revision; }
int      calendar_count(void)    { return event_num; }

const cal_event_t *calendar_get(int idx)
{
    return (events && idx >= 0 && idx < event_num) ? &events[idx] : NULL;
}

const cal_event_t *calendar_find(uint32_t id)
{
    for(int i = 0; i < event_num; i++) if(events[i].id == id) return &events[i];
    return NULL;
}

bool calendar_add(const cal_event_t *ev, uint32_t *id_out)
{
    if(events == NULL || event_num >= CAL_EVENT_MAX) return false;

    events[event_num] = *ev;
    events[event_num].id = next_id++;
    events[event_num].fired = 0;
    if(id_out) *id_out = events[event_num].id;
    event_num++;
    revision++;
    return events_save();
}

bool calendar_update(const cal_event_t *ev)
{
    for(int i = 0; i < event_num; i++) {
        if(events[i].id != ev->id) continue;
        uint32_t fired = events[i].fired;
        events[i] = *ev;
        // A reminder already given for an occurrence is not given again just
        // because the title was edited; a change of time is a new occurrence.
        events[i].fired = fired;
        revision++;
        return events_save();
    }
    return false;
}

bool calendar_remove(uint32_t id)
{
    for(int i = 0; i < event_num; i++) {
        if(events[i].id != id) continue;
        memmove(&events[i], &events[i + 1], sizeof(cal_event_t) * (event_num - i - 1));
        event_num--;
        revision++;
        return events_save();
    }
    return false;
}

bool calendar_occurs_on(const cal_event_t *ev, int year, int month, int day)
{
    if(date_cmp(year, month, day, ev->year, ev->month, ev->day) < 0) return false;

    switch(ev->repeat) {
        case CAL_REPEAT_NONE:     return date_cmp(year, month, day, ev->year, ev->month, ev->day) == 0;
        case CAL_REPEAT_DAILY:    return true;
        case CAL_REPEAT_WEEKDAYS: return calendar_weekday(year, month, day) < 5;
        case CAL_REPEAT_WEEKLY:   return calendar_weekday(year, month, day) ==
                                         calendar_weekday(ev->year, ev->month, ev->day);
        case CAL_REPEAT_MONTHLY:  return day == ev->day;
        case CAL_REPEAT_YEARLY:   return month == ev->month && day == ev->day;
        default:                  return false;
    }
}

/* Earliest first; an all-day event sorts before anything with a time. */
static int start_key(const cal_event_t *ev)
{
    return ev->all_day ? -1 : ev->hour * 60 + ev->minute;
}

int calendar_day_events(int year, int month, int day, uint32_t *ids, int max)
{
    int n = 0;
    for(int i = 0; i < event_num && n < max; i++) {
        if(!calendar_occurs_on(&events[i], year, month, day)) continue;

        // Insertion sort by start time as they are found.
        int at = n;
        while(at > 0 && start_key(calendar_find(ids[at - 1])) > start_key(&events[i])) {
            ids[at] = ids[at - 1];
            at--;
        }
        ids[at] = events[i].id;
        n++;
    }
    return n;
}

bool calendar_day_busy(int year, int month, int day)
{
    for(int i = 0; i < event_num; i++) {
        if(calendar_occurs_on(&events[i], year, month, day)) return true;
    }
    return false;
}

const cal_event_t *calendar_next_upcoming(uint32_t *when)
{
    if(!system_clock_is_set()) return NULL;

    uint32_t now = time(NULL);
    const cal_event_t *best = NULL;
    uint32_t best_at = 0;

    for(int off = 0; off <= 7 && best == NULL; off++) {
        int y, m, d;
        local_date_offset(off, &y, &m, &d);

        for(int i = 0; i < event_num; i++) {
            const cal_event_t *ev = &events[i];
            if(!calendar_occurs_on(ev, y, m, d)) continue;

            uint32_t at = ev->all_day ? local_epoch(y, m, d, 0, 0) : local_epoch(y, m, d, ev->hour, ev->minute);
            // An all-day event is upcoming for the whole of its day.
            if(ev->all_day ? at + 86400 <= now : at < now) continue;

            if(best == NULL || at < best_at) { best = ev; best_at = at; }
        }
    }

    if(best && when) *when = best_at;
    return best;
}

const cal_event_t *calendar_reminder_due(uint32_t *occurrence_start)
{
    if(!system_clock_is_set()) return NULL;

    uint32_t now = time(NULL);

    for(int i = 0; i < event_num; i++) {
        cal_event_t *ev = &events[i];
        if(ev->remind_min < 0) continue;

        /* Today, tomorrow and the day after: a day's notice on an event just
         * after midnight is the far edge of what has to be looked at. */
        for(int off = 0; off <= 2; off++) {
            int y, m, d;
            local_date_offset(off, &y, &m, &d);
            if(!calendar_occurs_on(ev, y, m, d)) continue;

            uint32_t start = local_epoch(y, m, d, ev->hour, ev->minute);
            uint32_t due   = start - (uint32_t)ev->remind_min * 60;

            /* A three-minute window, so a tick that lands late still catches
             * it, and one that has already been given is not given twice -
             * including across a restart, since `fired` is in the file. */
            if(now < due || now > due + 180) continue;
            if(ev->fired == start) continue;

            ev->fired = start;
            events_save();
            if(occurrence_start) *occurrence_start = start;
            return ev;
        }
    }
    return NULL;
}

const char *calendar_repeat_name(int repeat)
{
    switch(repeat) {
        case CAL_REPEAT_NONE:     return "Once";
        case CAL_REPEAT_DAILY:    return "Every day";
        case CAL_REPEAT_WEEKDAYS: return "Weekdays";
        case CAL_REPEAT_WEEKLY:   return "Every week";
        case CAL_REPEAT_MONTHLY:  return "Every month";
        case CAL_REPEAT_YEARLY:   return "Every year";
        default:                  return "?";
    }
}

/* The reminder choices, stepped through in this order by the editor. */
static const int16_t remind_choices[] = { CAL_REMIND_NONE, 0, 5, 15, 30, 60, 120, 1440 };

const char *calendar_remind_name(int16_t remind_min)
{
    switch(remind_min) {
        case CAL_REMIND_NONE: return "None";
        case 0:    return "At the time";
        case 5:    return "5 min before";
        case 15:   return "15 min before";
        case 30:   return "30 min before";
        case 60:   return "1 hour before";
        case 120:  return "2 hours before";
        case 1440: return "1 day before";
        default: {
            static char buf[20];
            snprintf(buf, sizeof(buf), "%d min before", remind_min);
            return buf;
        }
    }
}

int16_t calendar_remind_next(int16_t remind_min)
{
    int n = sizeof(remind_choices) / sizeof(remind_choices[0]);
    for(int i = 0; i < n; i++) {
        if(remind_choices[i] == remind_min) return remind_choices[(i + 1) % n];
    }
    return remind_choices[0];
}
