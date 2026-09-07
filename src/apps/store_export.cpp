/**
 * Getting the contacts and the messages off the phone.
 *
 * The device keeps both in SPIFFS, which is soldered on and cannot be read
 * anywhere else. The SD card is the only storage here that can be taken out and
 * put in a computer, and until now nothing wrote to it at all.
 *
 * CSV rather than the TSV kept internally: the point of an export is that
 * something else opens it, and a spreadsheet will take a quoted field without
 * asking questions. Message bodies carry the sender's own line breaks, and those
 * survive inside the quotes.
 */
#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <time.h>

#include "store_export.h"
#include "phone_store.h"
#include "peripheral.h"
#include "main.h"

/* One CSV field, quoted and escaped.
 *
 * Everything is quoted rather than only the fields that need it. A phone number
 * beginning with a plus sign, a name with a comma in it and a message with a
 * newline are all ordinary here, and deciding case by case is how an export
 * ends up subtly wrong in one row out of a hundred. */
static void write_field(File &f, const char *text, bool last)
{
    f.write('"');

    for(const char *p = text; *p; p++) {
        if(*p == '"') f.write('"');   // a quote inside a quoted field is doubled
        f.write((uint8_t)*p);
    }

    f.write('"');
    if(last) f.print("\r\n");         // CRLF, which is what the CSV convention says
    else     f.write(',');
}

/* The time, for stamping a filename. Falls back to the uptime when the clock
 * has never been set, which still keeps two exports apart. */
static void export_stamp(char *buf, int len)
{
    time_t now = time(NULL);

    if(now > 1700000000) {
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        strftime(buf, len, "%Y%m%d-%H%M%S", &tm_now);
    } else {
        snprintf(buf, len, "up%lu", (unsigned long)(millis() / 1000));
    }
}

static bool export_contacts(const char *path, int *rows)
{
    File f = SD.open(path, FILE_WRITE);
    if(!f) return false;

    f.print("name,number\r\n");

    int n = contacts_count();
    for(int i = 0; i < n; i++) {
        const contact_t *c = contacts_get(i);
        if(c == NULL) continue;

        write_field(f, c->name, false);
        write_field(f, c->number, true);
    }

    f.close();
    *rows = n;
    return true;
}

static bool export_messages(const char *path, int *rows)
{
    File f = SD.open(path, FILE_WRITE);
    if(!f) return false;

    f.print("when,direction,number,name,status,text\r\n");

    int n = sms_count();
    for(int i = 0; i < n; i++) {
        const sms_msg_t *m = sms_get(i);
        if(m == NULL) continue;

        /* A readable timestamp rather than the epoch seconds held internally:
         * this file is for reading. Messages that arrived before the clock was
         * set have none, and say so rather than claiming 1970. */
        char when[24];
        if(m->ts > 0) {
            time_t    t = (time_t)m->ts;
            struct tm tm_msg;
            localtime_r(&t, &tm_msg);
            strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm_msg);
        } else {
            snprintf(when, sizeof(when), "unknown");
        }

        const char *status = (m->status == SMS_ST_PENDING) ? "sending"
                           : (m->status == SMS_ST_FAILED)  ? "failed"
                                                           : "ok";

        write_field(f, when, false);
        write_field(f, m->dir == SMS_DIR_OUT ? "sent" : "received", false);
        write_field(f, m->number, false);
        write_field(f, contacts_display_name(m->number), false);
        write_field(f, status, false);
        write_field(f, m->text, true);
    }

    f.close();
    *rows = n;
    return true;
}

bool store_export_to_sd(char *detail, int detail_len)
{
    if(detail && detail_len > 0) detail[0] = '\0';

    if(!peri_init_st[E_PERI_SD]) {
        snprintf(detail, detail_len, "No SD card.");
        return false;
    }

    if(contacts_count() == 0 && sms_count() == 0) {
        snprintf(detail, detail_len, "Nothing to export.");
        return false;
    }

    // mkdir on a directory that already exists fails, which is not a problem.
    SD.mkdir(STORE_EXPORT_DIR);

    char stamp[24];
    export_stamp(stamp, sizeof(stamp));

    char contacts_path[80], messages_path[80];
    snprintf(contacts_path, sizeof(contacts_path), "%s/contacts-%s.csv", STORE_EXPORT_DIR, stamp);
    snprintf(messages_path, sizeof(messages_path), "%s/messages-%s.csv", STORE_EXPORT_DIR, stamp);

    int c_rows = 0, m_rows = 0;

    if(!export_contacts(contacts_path, &c_rows)) {
        snprintf(detail, detail_len, "Could not write to the card.");
        return false;
    }

    if(!export_messages(messages_path, &m_rows)) {
        snprintf(detail, detail_len, "Contacts written, messages failed.");
        return false;
    }

    snprintf(detail, detail_len, "%d contacts and %d messages to\n%s",
             c_rows, m_rows, STORE_EXPORT_DIR);

    Serial.printf("[EXPORT] %s and %s\n", contacts_path, messages_path);
    return true;
}
