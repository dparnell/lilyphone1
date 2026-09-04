/**
 * Contact book and message log, kept in RAM and mirrored to SPIFFS.
 *
 * Both tables live in PSRAM when it is available; the message log alone is a
 * little over 20KB, which is more than we want to take out of internal DRAM.
 * Every mutation rewrites the whole file - the tables are small and writes are
 * rare (a message arriving, a contact being edited), so an append-and-compact
 * scheme would only buy complexity.
 */
#include <Arduino.h>
#include "FS.h"
#include "SPIFFS.h"
#include "phone_store.h"

#define CONTACTS_PATH "/contacts.tsv"
#define MESSAGES_PATH "/messages.tsv"

// Numbers are considered the same contact when this many trailing digits agree.
#define MATCH_DIGITS 7

static contact_t  *contact_list = NULL;
static sms_msg_t  *msg_list     = NULL;
static int         contact_num  = 0;
static int         msg_num      = 0;
static bool        store_ready  = false;

//************************************[ helpers ]*******************************
static void *store_alloc(size_t size)
{
    void *p = ps_calloc(1, size);
    if(p == NULL) {
        p = calloc(1, size);
    }
    return p;
}

static void str_copy(char *dst, const char *src, size_t len)
{
    if(src == NULL) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, len - 1);
    dst[len - 1] = '\0';
}

static void str_trim(char *s)
{
    int len = strlen(s);
    while(len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
    int lead = 0;
    while(s[lead] && isspace((unsigned char)s[lead])) {
        lead++;
    }
    if(lead) {
        memmove(s, s + lead, len - lead + 1);
    }
}

/* Message bodies may contain tabs and newlines, which would otherwise break the
 * one-record-per-line file format. */
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

/* Splits `line` in place on tabs. Returns the number of fields found. */
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

bool phone_number_match(const char *a, const char *b)
{
    if(a == NULL || b == NULL) return false;

    // Compare from the back, digits only, so that "+61412345678", "0412345678"
    // and "412345678" all resolve to the same contact.
    int ia = strlen(a);
    int ib = strlen(b);
    int matched = 0;

    while(ia > 0 && ib > 0) {
        while(ia > 0 && !isdigit((unsigned char)a[ia - 1])) ia--;
        while(ib > 0 && !isdigit((unsigned char)b[ib - 1])) ib--;
        if(ia == 0 || ib == 0) break;
        if(a[ia - 1] != b[ib - 1]) return false;
        ia--;
        ib--;
        if(++matched >= MATCH_DIGITS) return true;
    }

    // Short numbers (service codes) have to agree over their whole length.
    while(ia > 0 && !isdigit((unsigned char)a[ia - 1])) ia--;
    while(ib > 0 && !isdigit((unsigned char)b[ib - 1])) ib--;
    return (ia == 0 && ib == 0 && matched > 0);
}

//************************************[ contacts ]******************************
static void contacts_read(void)
{
    contact_num = 0;

    File f = SPIFFS.open(CONTACTS_PATH, FILE_READ);
    if(!f) return;

    while(f.available() && contact_num < CONTACTS_MAX) {
        String line = f.readStringUntil('\n');
        line.trim();
        if(line.length() == 0) continue;

        char buf[CONTACT_NAME_LEN + CONTACT_NUMBER_LEN + 8];
        str_copy(buf, line.c_str(), sizeof(buf));

        char *fields[2];
        if(split_fields(buf, fields, 2) < 2) continue;

        str_copy(contact_list[contact_num].name, fields[0], CONTACT_NAME_LEN);
        str_copy(contact_list[contact_num].number, fields[1], CONTACT_NUMBER_LEN);
        contact_num++;
    }
    f.close();

    Serial.printf("[STORE] loaded %d contacts\n", contact_num);
}

bool contacts_save(void)
{
    File f = SPIFFS.open(CONTACTS_PATH, FILE_WRITE);
    if(!f) {
        Serial.println("[STORE] cannot write contacts");
        return false;
    }
    for(int i = 0; i < contact_num; i++) {
        f.printf("%s\t%s\n", contact_list[i].name, contact_list[i].number);
    }
    f.close();
    return true;
}

int contacts_count(void)
{
    return contact_num;
}

const contact_t *contacts_get(int idx)
{
    if(idx < 0 || idx >= contact_num) return NULL;
    return &contact_list[idx];
}

int contacts_find_by_number(const char *number)
{
    for(int i = 0; i < contact_num; i++) {
        if(phone_number_match(contact_list[i].number, number)) return i;
    }
    return -1;
}

const char *contacts_display_name(const char *number)
{
    int idx = contacts_find_by_number(number);
    if(idx >= 0 && contact_list[idx].name[0]) return contact_list[idx].name;
    return number;
}

/* Moves the entry at `idx` so the table stays sorted by name (case
 * insensitive). Returns where it ended up. */
static int contacts_reorder(int idx)
{
    contact_t moving = contact_list[idx];

    while(idx > 0 && strcasecmp(contact_list[idx - 1].name, moving.name) > 0) {
        contact_list[idx] = contact_list[idx - 1];
        idx--;
    }
    while(idx < contact_num - 1 && strcasecmp(contact_list[idx + 1].name, moving.name) < 0) {
        contact_list[idx] = contact_list[idx + 1];
        idx++;
    }
    contact_list[idx] = moving;
    return idx;
}

int contacts_add(const char *name, const char *number)
{
    if(!store_ready || number == NULL) return -1;

    contact_t entry;
    str_copy(entry.name, name, CONTACT_NAME_LEN);
    str_copy(entry.number, number, CONTACT_NUMBER_LEN);
    str_trim(entry.name);
    str_trim(entry.number);

    if(entry.number[0] == '\0') return -1;
    if(entry.name[0] == '\0') str_copy(entry.name, entry.number, CONTACT_NAME_LEN);

    int existing = contacts_find_by_number(entry.number);
    if(existing >= 0) {
        contacts_update(existing, entry.name, entry.number);
        return contacts_find_by_number(entry.number);
    }

    if(contact_num >= CONTACTS_MAX) return -1;

    contact_list[contact_num] = entry;
    contact_num++;
    int idx = contacts_reorder(contact_num - 1);

    contacts_save();
    return idx;
}

bool contacts_update(int idx, const char *name, const char *number)
{
    if(idx < 0 || idx >= contact_num) return false;

    contact_t entry;
    str_copy(entry.name, name, CONTACT_NAME_LEN);
    str_copy(entry.number, number, CONTACT_NUMBER_LEN);
    str_trim(entry.name);
    str_trim(entry.number);

    if(entry.number[0] == '\0') return false;
    if(entry.name[0] == '\0') str_copy(entry.name, entry.number, CONTACT_NAME_LEN);

    contact_list[idx] = entry;
    contacts_reorder(idx);
    contacts_save();
    return true;
}

bool contacts_remove(int idx)
{
    if(idx < 0 || idx >= contact_num) return false;

    for(int i = idx; i < contact_num - 1; i++) {
        contact_list[i] = contact_list[i + 1];
    }
    contact_num--;
    memset(&contact_list[contact_num], 0, sizeof(contact_t));

    contacts_save();
    return true;
}

//************************************[ messages ]******************************
static void messages_read(void)
{
    msg_num = 0;

    File f = SPIFFS.open(MESSAGES_PATH, FILE_READ);
    if(!f) return;

    while(f.available() && msg_num < SMS_MAX) {
        String line = f.readStringUntil('\n');
        if(line.length() == 0) continue;

        // The body is the last field and is escaped, so it never contains a tab.
        char buf[SMS_TEXT_LEN * 2 + CONTACT_NUMBER_LEN + 32];
        str_copy(buf, line.c_str(), sizeof(buf));

        char *fields[6];
        if(split_fields(buf, fields, 6) < 6) continue;

        sms_msg_t *m = &msg_list[msg_num];
        memset(m, 0, sizeof(*m));
        m->dir    = (uint8_t)atoi(fields[0]);
        m->status = (uint8_t)atoi(fields[1]);
        m->unread = (uint8_t)atoi(fields[2]);
        m->ts     = (uint32_t)strtoul(fields[3], NULL, 10);
        str_copy(m->number, fields[4], CONTACT_NUMBER_LEN);
        unescape(fields[5], m->text, SMS_TEXT_LEN);
        msg_num++;
    }
    f.close();

    Serial.printf("[STORE] loaded %d messages\n", msg_num);
}

bool sms_save(void)
{
    File f = SPIFFS.open(MESSAGES_PATH, FILE_WRITE);
    if(!f) {
        Serial.println("[STORE] cannot write messages");
        return false;
    }
    for(int i = 0; i < msg_num; i++) {
        String line;
        line.reserve(SMS_TEXT_LEN + 64);
        line += msg_list[i].dir;
        line += '\t';
        line += msg_list[i].status;
        line += '\t';
        line += msg_list[i].unread;
        line += '\t';
        line += msg_list[i].ts;
        line += '\t';
        line += msg_list[i].number;
        line += '\t';
        escape_append(line, msg_list[i].text);
        line += '\n';
        f.print(line);
    }
    f.close();
    return true;
}

int sms_count(void)
{
    return msg_num;
}

const sms_msg_t *sms_get(int idx)
{
    if(idx < 0 || idx >= msg_num) return NULL;
    return &msg_list[idx];
}

int sms_add(const char *number, const char *text, uint32_t ts, int dir, int status, bool unread)
{
    if(!store_ready || number == NULL) return -1;

    if(msg_num >= SMS_MAX) {
        // Drop the oldest message so the log stays bounded.
        memmove(&msg_list[0], &msg_list[1], sizeof(sms_msg_t) * (SMS_MAX - 1));
        msg_num = SMS_MAX - 1;
    }

    sms_msg_t *m = &msg_list[msg_num];
    memset(m, 0, sizeof(*m));
    str_copy(m->number, number, CONTACT_NUMBER_LEN);
    str_copy(m->text, text, SMS_TEXT_LEN);
    m->ts     = ts;
    m->dir    = (uint8_t)dir;
    m->status = (uint8_t)status;
    m->unread = unread ? 1 : 0;

    msg_num++;
    sms_save();
    return msg_num - 1;
}

bool sms_delete(int idx)
{
    if(idx < 0 || idx >= msg_num) return false;

    for(int i = idx; i < msg_num - 1; i++) {
        msg_list[i] = msg_list[i + 1];
    }
    msg_num--;
    memset(&msg_list[msg_num], 0, sizeof(sms_msg_t));

    sms_save();
    return true;
}

bool sms_set_status(int idx, int status)
{
    if(idx < 0 || idx >= msg_num) return false;
    if(msg_list[idx].status == (uint8_t)status) return true;
    msg_list[idx].status = (uint8_t)status;
    sms_save();
    return true;
}

/* Threads are derived rather than stored: walk the log backwards and collect
 * each number the first time it is seen, which yields most-recent-first. */
static int thread_collect(const char **numbers, int max)
{
    int n = 0;
    for(int i = msg_num - 1; i >= 0 && n < max; i--) {
        bool seen = false;
        for(int j = 0; j < n; j++) {
            if(phone_number_match(numbers[j], msg_list[i].number)) {
                seen = true;
                break;
            }
        }
        if(!seen) numbers[n++] = msg_list[i].number;
    }
    return n;
}

int sms_thread_count(void)
{
    const char *numbers[SMS_MAX];
    return thread_collect(numbers, SMS_MAX);
}

const char *sms_thread_number(int thread)
{
    const char *numbers[SMS_MAX];
    int n = thread_collect(numbers, SMS_MAX);
    if(thread < 0 || thread >= n) return NULL;
    return numbers[thread];
}

const sms_msg_t *sms_thread_last(int thread)
{
    const char *number = sms_thread_number(thread);
    if(number == NULL) return NULL;

    for(int i = msg_num - 1; i >= 0; i--) {
        if(phone_number_match(msg_list[i].number, number)) return &msg_list[i];
    }
    return NULL;
}

int sms_thread_unread(const char *number)
{
    int n = 0;
    for(int i = 0; i < msg_num; i++) {
        if(msg_list[i].unread && phone_number_match(msg_list[i].number, number)) n++;
    }
    return n;
}

int sms_thread_msg_count(const char *number)
{
    int n = 0;
    for(int i = 0; i < msg_num; i++) {
        if(phone_number_match(msg_list[i].number, number)) n++;
    }
    return n;
}

int sms_thread_msg_index(const char *number, int idx)
{
    for(int i = 0; i < msg_num; i++) {
        if(phone_number_match(msg_list[i].number, number)) {
            if(idx-- == 0) return i;
        }
    }
    return -1;
}

const sms_msg_t *sms_thread_msg(const char *number, int idx)
{
    int abs = sms_thread_msg_index(number, idx);
    return abs < 0 ? NULL : &msg_list[abs];
}

void sms_thread_mark_read(const char *number)
{
    bool changed = false;
    for(int i = 0; i < msg_num; i++) {
        if(msg_list[i].unread && phone_number_match(msg_list[i].number, number)) {
            msg_list[i].unread = 0;
            changed = true;
        }
    }
    if(changed) sms_save();
}

void sms_thread_delete(const char *number)
{
    int out = 0;
    for(int i = 0; i < msg_num; i++) {
        if(phone_number_match(msg_list[i].number, number)) continue;
        if(out != i) msg_list[out] = msg_list[i];
        out++;
    }
    if(out == msg_num) return;

    memset(&msg_list[out], 0, sizeof(sms_msg_t) * (msg_num - out));
    msg_num = out;
    sms_save();
}

int sms_unread_total(void)
{
    int n = 0;
    for(int i = 0; i < msg_num; i++) {
        if(msg_list[i].unread) n++;
    }
    return n;
}

//************************************[ init ]**********************************
bool phone_store_init(void)
{
    if(store_ready) return true;

    contact_list = (contact_t *)store_alloc(sizeof(contact_t) * CONTACTS_MAX);
    msg_list     = (sms_msg_t *)store_alloc(sizeof(sms_msg_t) * SMS_MAX);

    if(contact_list == NULL || msg_list == NULL) {
        Serial.println("[STORE] out of memory");
        if(contact_list) free(contact_list);
        if(msg_list) free(msg_list);
        contact_list = NULL;
        msg_list = NULL;
        return false;
    }

    store_ready = true;
    contacts_read();
    messages_read();
    return true;
}
