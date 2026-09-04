/**
 * Modem service for the A7682E.
 *
 * One task owns SerialAT. It is the only code allowed to read or write the
 * port, which is what makes unsolicited result codes usable: previously the
 * passthrough task raced with whatever screen happened to be sending AT
 * commands, so RING and +CMTI notifications were swallowed and the phone could
 * neither see an incoming call nor an incoming message.
 *
 * The UI talks to the task through two queues - requests going down, received
 * messages coming up - plus a small mutex-protected snapshot of the call and
 * network state that the UI polls from its LVGL timers. Nothing the UI calls
 * blocks on the modem.
 */
#include <Arduino.h>
#include "utilities.h"
#include "peripheral.h"
#include "modem_service.h"
#include "system_clock.h"

#define MODEM_LINE_MAX     256
#define MODEM_REQ_QUEUE    6
#define MODEM_RX_QUEUE     8
#define MODEM_TASK_STACK   (1024 * 6)

// How often the task refreshes signal strength and registration.
#define MODEM_STATUS_PERIOD_MS 15000
// How often the state of a call in progress is confirmed with the modem.
#define MODEM_CALL_POLL_MS     2000
// How long to wait before asking for the network time again. Carriers vary in
// how promptly they broadcast NITZ after registration and some never do, so a
// failed read must not turn into a tight loop of AT commands.
#define MODEM_CLOCK_RETRY_MS   30000

enum {
    REQ_DIAL = 0,
    REQ_ANSWER,
    REQ_HANGUP,
    REQ_SEND_SMS,
    REQ_RAW_AT,
    REQ_TONE,
};

typedef struct {
    uint8_t  type;
    uint32_t send_id;
    char     number[CONTACT_NUMBER_LEN];
    char     text[SMS_TEXT_LEN];
} modem_req_t;

typedef struct {
    modem_call_state_t call_state;
    char               call_number[CONTACT_NUMBER_LEN];
    uint32_t           call_connect_ms;
    bool               registered;
    uint8_t            signal;
    char               op_name[MODEM_OPERATOR_LEN];
    uint32_t           send_id;
    modem_send_state_t send_state;
} modem_status_t;

static QueueHandle_t     req_queue    = NULL;
static QueueHandle_t     rx_queue     = NULL;
static SemaphoreHandle_t status_lock  = NULL;
static TaskHandle_t      modem_task_h = NULL;

static modem_status_t status;      // guarded by status_lock
static uint32_t       next_send_id = 1;

/* SIM slots that an URC told us about but that have not been read out yet.
 * Reading a message is itself an AT exchange, so it can never happen from
 * inside the URC handler - that handler may be running in the middle of
 * another command. The task drains this list when it is between commands.
 * Only the modem task touches it. */
#define PENDING_SMS_MAX 8
static int pending_sms[PENDING_SMS_MAX];
static int pending_sms_num = 0;

/* Set when the network announces a time or zone change, and once at startup, so
 * the task knows to read the module's clock. Like the SMS slots this cannot be
 * acted on from the URC handler, which may be running mid-command. */
static bool clock_read_pending = true;

// Set by the +CIPRXGET URC; see the UDP section further down.
static bool udp_rx_pending = false;

static void pending_sms_push(int index)
{
    for(int i = 0; i < pending_sms_num; i++) {
        if(pending_sms[i] == index) return;
    }
    if(pending_sms_num < PENDING_SMS_MAX) {
        pending_sms[pending_sms_num++] = index;
    } else {
        Serial.println("[MODEM] too many unread slots, will pick them up on the next scan");
    }
}

//************************************[ state helpers ]*************************
static void status_set_call(modem_call_state_t st, const char *number)
{
    xSemaphoreTake(status_lock, portMAX_DELAY);
    status.call_state = st;
    if(number) {
        strncpy(status.call_number, number, CONTACT_NUMBER_LEN - 1);
        status.call_number[CONTACT_NUMBER_LEN - 1] = '\0';
    }
    if(st == MODEM_CALL_ACTIVE) {
        if(status.call_connect_ms == 0) status.call_connect_ms = millis();
    } else {
        status.call_connect_ms = 0;
    }
    if(st == MODEM_CALL_IDLE) {
        status.call_number[0] = '\0';
    }
    xSemaphoreGive(status_lock);
}

static modem_call_state_t status_get_call(void)
{
    xSemaphoreTake(status_lock, portMAX_DELAY);
    modem_call_state_t st = status.call_state;
    xSemaphoreGive(status_lock);
    return st;
}

//************************************[ line I/O ]******************************
/* Reads one CR/LF delimited line into `buf`. Returns the length, or -1 when
 * `timeout_ms` elapsed with nothing to show for it. Blank lines are skipped so
 * callers never have to filter them. */
static int modem_read_line(char *buf, int len, uint32_t timeout_ms)
{
    int      pos   = 0;
    uint32_t start = millis();

    while(millis() - start < timeout_ms) {
        while(SerialAT.available()) {
            char c = (char)SerialAT.read();
            if(c == '\n' || c == '\r') {
                if(pos > 0) {
                    buf[pos] = '\0';
                    return pos;
                }
                continue; // blank line
            }
            if(pos < len - 1) buf[pos++] = c;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    buf[pos] = '\0';
    return pos > 0 ? pos : -1;
}

static void modem_write_line(const char *cmd)
{
    SerialAT.print(cmd);
    SerialAT.print("\r\n");
}

//************************************[ URC handling ]**************************
/* Extracts the first double-quoted field of an URC, e.g. the number out of
 * `+CLIP: "+61412345678",145,...`. Returns false when there is none. */
static bool urc_quoted_field(const char *line, int which, char *out, int len)
{
    const char *p = line;
    for(int i = 0; i <= which; i++) {
        p = strchr(p, '"');
        if(p == NULL) return false;
        p++;
        if(i < which) {
            p = strchr(p, '"');
            if(p == NULL) return false;
            p++;
        }
    }

    int n = 0;
    while(*p && *p != '"' && n < len - 1) out[n++] = *p++;
    out[n] = '\0';
    return true;
}

/* The service centre timestamp is the last quoted field of +CMGR, which is
 * steadier than counting fields: the optional <alpha> in front of it is present
 * only when the number is in the SIM phonebook. */
static bool urc_quoted_last(const char *line, char *out, int len)
{
    const char *close = strrchr(line, '"');
    if(close == NULL || close == line) return false;

    const char *open = close - 1;
    while(open > line && *open != '"') open--;
    if(*open != '"') return false;
    open++;

    int n = 0;
    while(open < close && n < len - 1) out[n++] = *open++;
    out[n] = '\0';
    return n > 0;
}

/* Decodes a UCS2 message body into UTF-8.
 *
 * In text mode with CSCS="GSM" the modem hands back a body whose data coding
 * scheme is UCS2 as a plain hex string, so anything the sender's phone could
 * not fit in the GSM 7-bit alphabet arrives looking like "004C00690065...".
 * Curly quotes and emoji both force that, which is why reaction messages in
 * particular show up as hex.
 *
 * Returns false and leaves `out` alone if the input is not well formed UCS2. */
static bool ucs2_hex_to_utf8(const char *hex, char *out, size_t out_len)
{
    size_t n = strlen(hex);
    if(n < 4 || (n % 4) != 0) return false;

    size_t o = 0;
    for(size_t i = 0; i + 3 < n; i += 4) {
        uint32_t cp = 0;

        for(int k = 0; k < 4; k++) {
            char c = hex[i + k];
            if(c >= '0' && c <= '9')      cp = (cp << 4) | (uint32_t)(c - '0');
            else if(c >= 'A' && c <= 'F') cp = (cp << 4) | (uint32_t)(c - 'A' + 10);
            else if(c >= 'a' && c <= 'f') cp = (cp << 4) | (uint32_t)(c - 'a' + 10);
            else return false;
        }

        // Anything above the BMP - every emoji - arrives as a surrogate pair.
        if(cp >= 0xD800 && cp <= 0xDBFF) {
            if(i + 7 >= n) return false;

            uint32_t lo = 0;
            for(int k = 4; k < 8; k++) {
                char c = hex[i + k];
                if(c >= '0' && c <= '9')      lo = (lo << 4) | (uint32_t)(c - '0');
                else if(c >= 'A' && c <= 'F') lo = (lo << 4) | (uint32_t)(c - 'A' + 10);
                else if(c >= 'a' && c <= 'f') lo = (lo << 4) | (uint32_t)(c - 'a' + 10);
                else return false;
            }
            if(lo < 0xDC00 || lo > 0xDFFF) return false;

            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            i += 4;
        } else if(cp >= 0xDC00 && cp <= 0xDFFF) {
            return false; // a trailing surrogate with nothing in front of it
        }

        if(cp < 0x80) {
            if(o + 1 >= out_len) break;
            out[o++] = (char)cp;
        } else if(cp < 0x800) {
            if(o + 2 >= out_len) break;
            out[o++] = (char)(0xC0 | (cp >> 6));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else if(cp < 0x10000) {
            if(o + 3 >= out_len) break;
            out[o++] = (char)(0xE0 | (cp >> 12));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            if(o + 4 >= out_len) break;
            out[o++] = (char)(0xF0 | (cp >> 18));
            out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }

    out[o] = '\0';
    return true;
}

/* True when the modem can carry this text as it stands. CSCS="GSM" means the
 * body is taken as GSM 7-bit, so anything outside plain ASCII has to go as UCS2
 * instead - deliberately conservative, since a few ASCII characters differ in
 * the GSM alphabet but all of them survive the round trip. */
static bool text_is_gsm7(const char *s)
{
    for(const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if(*p >= 0x80) return false;
    }
    return true;
}

/* The other direction from ucs2_hex_to_utf8: UTF-8 in, the hex a UCS2 message
 * body is written as out. Returns false on malformed input or no room. */
static bool utf8_to_ucs2_hex(const char *utf8, char *out, size_t out_len)
{
    const unsigned char *p = (const unsigned char *)utf8;
    size_t o = 0;

    while(*p) {
        uint32_t cp;
        int      n;

        if(*p < 0x80)                { cp = *p;        n = 1; }
        else if((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; n = 2; }
        else if((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; n = 3; }
        else if((*p & 0xF8) == 0xF0) { cp = *p & 0x07; n = 4; }
        else return false;

        for(int k = 1; k < n; k++) {
            if((p[k] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (uint32_t)(p[k] & 0x3F);
        }
        p += n;

        uint16_t units[2];
        int      count = 1;

        if(cp >= 0x10000) {
            cp -= 0x10000;
            units[0] = (uint16_t)(0xD800 + (cp >> 10));
            units[1] = (uint16_t)(0xDC00 + (cp & 0x3FF));
            count = 2;
        } else {
            units[0] = (uint16_t)cp;
        }

        for(int k = 0; k < count; k++) {
            if(o + 5 > out_len) return false;
            snprintf(out + o, out_len - o, "%04X", units[k]);
            o += 4;
        }
    }

    out[o] = '\0';
    return o > 0;
}

/* Fallback for firmwares that do not report the data coding scheme.
 *
 * Requires a hex letter as well as the right shape, so that a message whose
 * whole body is digits - a one time code, say - is not mistaken for UCS2 and
 * turned into nonsense. Real UCS2 text almost always carries one. */
static bool looks_like_ucs2_hex(const char *s)
{
    size_t n = strlen(s);
    if(n < 4 || (n % 4) != 0) return false;

    bool has_letter = false;
    for(size_t i = 0; i < n; i++) {
        char c = s[i];
        if(c >= '0' && c <= '9') continue;
        if((c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')) {
            has_letter = true;
            continue;
        }
        return false;
    }
    return has_letter;
}

/* A modem timestamp, converted to UTC. */
typedef struct {
    int  year, mon, day;
    int  hour, min, sec;
    int  offset_min;   // local offset east of Greenwich, when the stamp carried one
    bool has_offset;
} modem_stamp_t;

/* Parses `25/09/04,10:22:33+40` as used by both +CMGR and +CCLK?: a local time
 * followed by the offset from GMT in quarter hours, so +40 means UTC+10. The
 * offset is optional - some firmwares leave it off +CCLK?.
 *
 * The fields come back as UTC, with `day` possibly sitting one outside the
 * month when removing the offset stepped over midnight; system_clock takes
 * that in its stride. */
static bool parse_modem_stamp(const char *stamp, modem_stamp_t *out)
{
    int year, mon, day, hour, min, sec;
    int quarters = 0;

    int n = sscanf(stamp, "%d/%d/%d,%d:%d:%d%d", &year, &mon, &day, &hour, &min, &sec, &quarters);
    if(n < 6) return false;

    out->has_offset = (n >= 7);
    out->offset_min = out->has_offset ? quarters * 15 : 0;

    // Work in minutes so the half and quarter hour zones survive.
    int utc_minutes = hour * 60 + min - out->offset_min;
    int date_shift  = 0;

    while(utc_minutes < 0)     { utc_minutes += 24 * 60; date_shift--; }
    while(utc_minutes >= 1440) { utc_minutes -= 24 * 60; date_shift++; }

    out->year = year + 2000;
    out->mon  = mon;
    out->day  = day + date_shift;
    out->hour = utc_minutes / 60;
    out->min  = utc_minutes % 60;
    out->sec  = sec;
    return true;
}

/* Pulls the sender, the timestamp and the data coding scheme out of a +CMGR
 * header:
 *
 *   +CMGR: "REC READ","+614...",,"25/09/04,10:22:33+40",145,4,0,8,"+614...",145,7
 *
 * With CSDH=1 the tail after the timestamp carries <tooa>,<fo>,<pid>,<dcs> and
 * then a second quoted field, the service centre number - which is why the
 * timestamp is found by its shape rather than by being the last quoted field.
 *
 * `dcs` comes back as -1 when the firmware reported none. */
static void parse_cmgr_header(const char *line, char *number, int number_len,
                              char *stamp, int stamp_len, int *dcs)
{
    number[0] = '\0';
    stamp[0]  = '\0';
    *dcs      = -1;

    urc_quoted_field(line, 1, number, number_len);

    for(int i = 0; i < 6; i++) {
        char field[40];
        int  a, b, c, d, e, f;

        if(!urc_quoted_field(line, i, field, sizeof(field))) break;
        if(sscanf(field, "%d/%d/%d,%d:%d:%d", &a, &b, &c, &d, &e, &f) != 6) continue;

        strncpy(stamp, field, stamp_len - 1);
        stamp[stamp_len - 1] = '\0';

        const char *p = strstr(line, field);
        if(p == NULL) break;
        p += strlen(field);
        if(*p == '"') p++;

        // ,<tooa>,<fo>,<pid>,<dcs> - the fourth comma is the one before dcs.
        int commas = 0;
        while(*p) {
            if(*p == '"') break; // reached the service centre, so no dcs here
            if(*p == ',') {
                if(++commas == 4) {
                    p++;
                    if(isdigit((unsigned char)*p)) *dcs = atoi(p);
                    break;
                }
            }
            p++;
        }
        break;
    }
}

/* Epoch seconds for a +CMGR service centre timestamp. Returns 0 when the stamp
 * cannot be parsed, in which case the caller falls back to the local clock. */
static uint32_t parse_sms_timestamp(const char *stamp)
{
    modem_stamp_t t;

    if(!parse_modem_stamp(stamp, &t)) return 0;
    return system_clock_epoch_from_utc(t.year, t.mon, t.day, t.hour, t.min, t.sec);
}

static void sms_deliver(const char *number, const char *text, uint32_t ts)
{
    modem_sms_rx_t rx;
    memset(&rx, 0, sizeof(rx));
    strncpy(rx.number, number, CONTACT_NUMBER_LEN - 1);
    strncpy(rx.text, text, SMS_TEXT_LEN - 1);
    rx.ts = ts ? ts : (uint32_t)time(NULL);

    Serial.printf("[MODEM] SMS from %s: %s\n", rx.number, rx.text);

    if(xQueueSend(rx_queue, &rx, 0) != pdTRUE) {
        Serial.println("[MODEM] receive queue full, message dropped");
    }
}

static void handle_urc(const char *line);

/* Sends a command and consumes lines until a final result code. Response lines
 * that are actually URCs are dispatched instead of being handed back, so an
 * incoming call during an AT exchange is not lost. `first_resp`, when given,
 * receives the first non-URC informational line. */
static bool modem_exec(const char *cmd, char *first_resp, int resp_len, uint32_t timeout_ms)
{
    char line[MODEM_LINE_MAX];
    bool got_resp = false;

    if(first_resp && resp_len > 0) first_resp[0] = '\0';

    // Drop anything still buffered from a previous exchange.
    while(SerialAT.available()) SerialAT.read();

    modem_write_line(cmd);

    uint32_t start = millis();
    while(millis() - start < timeout_ms) {
        if(modem_read_line(line, sizeof(line), 200) < 0) continue;

        if(strcmp(line, "OK") == 0) return true;
        if(strcmp(line, "ERROR") == 0) return false;
        if(strncmp(line, "+CME ERROR", 10) == 0) return false;
        if(strncmp(line, "+CMS ERROR", 10) == 0) return false;
        if(strcmp(line, cmd) == 0) continue; // command echo

        if(strncmp(line, "RING", 4) == 0 || strncmp(line, "+CLIP:", 6) == 0 ||
           strncmp(line, "+CMTI:", 6) == 0 || strncmp(line, "NO CARRIER", 10) == 0 ||
           strncmp(line, "VOICE CALL:", 11) == 0 || strncmp(line, "BUSY", 4) == 0 ||
           strncmp(line, "NO ANSWER", 9) == 0) {
            handle_urc(line);
            continue;
        }

        if(!got_resp && first_resp && resp_len > 0) {
            strncpy(first_resp, line, resp_len - 1);
            first_resp[resp_len - 1] = '\0';
            got_resp = true;
        }
    }
    return false;
}

/* Fetches one stored message, hands it to the UI and deletes it from the SIM so
 * the (very small) SIM store does not fill up. */
static void sms_fetch(int index)
{
    char cmd[32];
    char line[MODEM_LINE_MAX];
    char number[CONTACT_NUMBER_LEN] = {0};
    char stamp[40] = {0};
    // Room for a UCS2 body, which arrives as four hex characters per character
    // of actual message.
    char body[SMS_HEX_BODY_MAX] = {0};
    int  dcs = -1;
    bool have_header = false;
    bool have_body   = false;

    snprintf(cmd, sizeof(cmd), "AT+CMGR=%d", index);

    while(SerialAT.available()) SerialAT.read();
    modem_write_line(cmd);

    uint32_t start = millis();
    while(millis() - start < 5000) {
        if(modem_read_line(line, sizeof(line), 200) < 0) continue;

        if(strncmp(line, "+CMS ERROR", 10) == 0 || strcmp(line, "ERROR") == 0) return;
        if(strcmp(line, cmd) == 0) continue;

        if(strncmp(line, "+CMGR:", 6) == 0) {
            parse_cmgr_header(line, number, sizeof(number), stamp, sizeof(stamp), &dcs);
            have_header = true;
            continue;
        }

        if(have_header && !have_body) {
            // The line straight after the header is the body whatever it says,
            // so a message that reads "OK" is not mistaken for the result code.
            strncpy(body, line, sizeof(body) - 1);
            have_body = true;
            continue;
        }

        if(strcmp(line, "OK") == 0) break;

        if(have_header) {
            // Multi-line body: keep the newlines the sender wrote.
            int used = strlen(body);
            if(used < (int)sizeof(body) - 2) {
                body[used] = '\n';
                strncpy(body + used + 1, line, sizeof(body) - used - 2);
            }
        }
    }

    if(have_header && number[0]) {
        // Bits 3..2 of the data coding scheme select the alphabet; 10 is UCS2.
        bool ucs2 = (dcs >= 0) ? ((dcs & 0x0C) == 0x08) : looks_like_ucs2_hex(body);

        char decoded[SMS_TEXT_LEN];
        if(ucs2 && ucs2_hex_to_utf8(body, decoded, sizeof(decoded))) {
            sms_deliver(number, decoded, parse_sms_timestamp(stamp));
        } else {
            sms_deliver(number, body, parse_sms_timestamp(stamp));
        }
    }

    snprintf(cmd, sizeof(cmd), "AT+CMGD=%d", index);
    modem_exec(cmd, NULL, 0, 3000);
}

static void handle_urc(const char *line)
{
    if(strncmp(line, "RING", 4) == 0) {
        if(status_get_call() == MODEM_CALL_IDLE) {
            status_set_call(MODEM_CALL_INCOMING, NULL);
        }
        return;
    }

    if(strncmp(line, "+CLIP:", 6) == 0) {
        char number[CONTACT_NUMBER_LEN];
        if(urc_quoted_field(line, 0, number, sizeof(number))) {
            status_set_call(MODEM_CALL_INCOMING, number);
        }
        return;
    }

    if(strncmp(line, "VOICE CALL: BEGIN", 17) == 0) {
        status_set_call(MODEM_CALL_ACTIVE, NULL);
        return;
    }

    if(strncmp(line, "VOICE CALL: END", 15) == 0 || strncmp(line, "NO CARRIER", 10) == 0 ||
       strncmp(line, "BUSY", 4) == 0 || strncmp(line, "NO ANSWER", 9) == 0) {
        status_set_call(MODEM_CALL_IDLE, NULL);
        return;
    }

    // The network pushed a time or time zone update. SIMCom firmwares differ
    // over which of these they emit, so watch for all of them and just take it
    // as a hint to re-read the module clock.
    if(strncmp(line, "*PSUTTZ:", 8) == 0 || strncmp(line, "+CTZV:", 6) == 0 ||
       strncmp(line, "+CTZE:", 6) == 0 || strncmp(line, "DST:", 4) == 0) {
        clock_read_pending = true;
        return;
    }

    /* Data waiting on the UDP socket. Like a message arriving, this only
     * records the fact - collecting it is an AT exchange of its own. */
    if(strncmp(line, "+CIPRXGET: 1", 12) == 0) {
        udp_rx_pending = true;
        return;
    }

    if(strncmp(line, "+CMTI:", 6) == 0) {
        // +CMTI: "SM",3 - a message landed in slot 3.
        const char *comma = strrchr(line, ',');
        if(comma) pending_sms_push(atoi(comma + 1));
        return;
    }
}

//************************************[ requests ]******************************
static void set_send_state(uint32_t send_id, modem_send_state_t st)
{
    xSemaphoreTake(status_lock, portMAX_DELAY);
    status.send_id    = send_id;
    status.send_state = st;
    xSemaphoreGive(status_lock);
}

/* AT+CMGS is the one command that does not follow the plain request/response
 * shape: the modem answers with a `>` prompt and then waits for the body
 * terminated by Ctrl-Z. */
/* Folds the punctuation that forces UCS2 back into ASCII so a message can go
 * out over GSM-7 after all. Anything else that cannot be represented becomes a
 * question mark - a lossy last resort, but the alternative is not sending. */
static void ascii_fold(const char *in, char *out, size_t out_len)
{
    static const struct {
        const char *utf8;
        const char *ascii;
    } map[] = {
        { "\xE2\x80\x9C", "\"" },  // left double quote
        { "\xE2\x80\x9D", "\"" },  // right double quote
        { "\xE2\x80\x98", "'" },   // left single quote
        { "\xE2\x80\x99", "'" },   // right single quote
        { "\xE2\x80\xA6", "..." }, // ellipsis
        { "\xE2\x80\x93", "-" },   // en dash
        { "\xE2\x80\x94", "-" },   // em dash
    };

    const unsigned char *p = (const unsigned char *)in;
    size_t o = 0;

    while(*p && o + 1 < out_len) {
        if(*p < 0x80) {
            out[o++] = (char)*p++;
            continue;
        }

        bool mapped = false;
        for(int i = 0; i < (int)(sizeof(map) / sizeof(map[0])); i++) {
            size_t n = strlen(map[i].utf8);
            if(strncmp((const char *)p, map[i].utf8, n) != 0) continue;

            size_t a = strlen(map[i].ascii);
            if(o + a + 1 >= out_len) break;
            memcpy(out + o, map[i].ascii, a);
            o += a;
            p += n;
            mapped = true;
            break;
        }
        if(mapped) continue;

        // Skip the whole sequence, not just its first byte.
        int n = 1;
        if((*p & 0xE0) == 0xC0)      n = 2;
        else if((*p & 0xF0) == 0xE0) n = 3;
        else if((*p & 0xF8) == 0xF0) n = 4;
        p += n;

        out[o++] = '?';
    }

    out[o] = '\0';
}

/* One attempt at AT+CMGS. `ucs2` selects the coding scheme and whether the body
 * goes out as hex. */
static bool sms_send_body(const char *number, const char *text, bool ucs2)
{
    // Room for a destination number written as UCS2 hex, four times its length.
    char cmd[CONTACT_NUMBER_LEN * 4 + 24];
    char line[MODEM_LINE_MAX];
    char hex[SMS_HEX_BODY_MAX];
    char number_hex[CONTACT_NUMBER_LEN * 4 + 4];

    if(ucs2) {
        if(!utf8_to_ucs2_hex(text, hex, sizeof(hex)) ||
           !utf8_to_ucs2_hex(number, number_hex, sizeof(number_hex))) {
            Serial.println("[MODEM] cannot encode message as UCS2");
            return false;
        }

        /* The module will not accept UCS2 data while the character set is GSM:
         * it answers +CMS ERROR: 305, invalid text mode parameter. Under
         * CSCS="UCS2" every string parameter is hex - the destination number
         * included, which is why that is encoded too. */
        if(!modem_exec("AT+CSCS=\"UCS2\"", NULL, 0, 2000)) {
            Serial.println("[MODEM] modem would not switch to the UCS2 character set");
            return false;
        }
        if(!modem_exec("AT+CSMP=17,167,0,8", NULL, 0, 2000)) {
            Serial.println("[MODEM] modem would not take a UCS2 coding scheme");
            modem_exec("AT+CSCS=\"GSM\"", NULL, 0, 2000);
            return false;
        }
    }

    /* Text mode, every time. In PDU mode AT+CMGS takes a length rather than a
     * quoted number, so the command below would be a syntax error and the
     * prompt would never come - a nasty way to lose messages if anything has
     * reset the modem since it was configured. */
    modem_exec("AT+CMGF=1", NULL, 0, 2000);

    while(SerialAT.available()) SerialAT.read();

    /* Terminated with CR alone: the LF that modem_write_line would add lands
     * after the modem has switched to accepting the message body, where it
     * would become the first character of the message. */
    snprintf(cmd, sizeof(cmd), "AT+CMGS=\"%s\"", ucs2 ? number_hex : number);
    SerialAT.print(cmd);
    SerialAT.print("\r");

    /* Wait for the '>' prompt, keeping anything else the modem says on the way
     * so that a failure can be explained instead of merely reported. */
    char     reply[MODEM_LINE_MAX];
    int      reply_len = 0;
    uint32_t start = millis();
    bool     prompt = false;

    while(millis() - start < 10000 && !prompt) {
        while(SerialAT.available()) {
            char c = (char)SerialAT.read();
            if(c == '>') {
                prompt = true;
                break;
            }
            if(c != '\r' && c != '\n' && reply_len < (int)sizeof(reply) - 1) {
                reply[reply_len++] = c;
            }
        }
        if(!prompt) vTaskDelay(pdMS_TO_TICKS(10));
    }
    reply[reply_len] = '\0';

    bool sent = false;

    if(!prompt) {
        Serial.printf("[MODEM] no send prompt; modem said: %s\n",
                      reply[0] ? reply : "(nothing at all)");
        // Abort the half-issued command so the modem does not stay in text mode.
        SerialAT.write(0x1B);
    } else {
        SerialAT.print(ucs2 ? hex : text);
        SerialAT.write(0x1A); // Ctrl-Z ends the body

        // The network can take a while to accept a message.
        start = millis();
        while(millis() - start < 60000) {
            if(modem_read_line(line, sizeof(line), 500) < 0) continue;

            if(strncmp(line, "+CMGS:", 6) == 0) continue; // reference number
            if(strcmp(line, "OK") == 0) {
                sent = true;
                break;
            }
            if(strcmp(line, "ERROR") == 0 || strncmp(line, "+CMS ERROR", 10) == 0) {
                Serial.printf("[MODEM] send refused: %s\n", line);
                break;
            }
            handle_urc(line);
        }
    }

    /* Whatever happened, put the character set and coding scheme back. Leaving
     * CSCS at UCS2 would hex-encode the strings in every later reply, so this
     * matters more than the coding scheme does. */
    if(ucs2) {
        modem_exec("AT+CSCS=\"GSM\"", NULL, 0, 2000);
        modem_exec("AT+CSMP=17,167,0,0", NULL, 0, 2000);
    }

    Serial.printf("[MODEM] %s send %s\n", ucs2 ? "UCS2" : "GSM-7",
                  sent ? "accepted by the network" : "failed");
    return sent;
}

static bool sms_send(const char *number, const char *text)
{
    Serial.printf("[MODEM] sending to %s: %s\n", number, text);

    if(text_is_gsm7(text)) {
        return sms_send_body(number, text, false);
    }

    if(sms_send_body(number, text, true)) return true;

    /* Text mode UCS2 is where firmwares differ: some will not take the coding
     * scheme, others want the body in a different form. Rather than drop the
     * message, fold what cannot be represented down to ASCII and send it the
     * ordinary way. A reaction that arrives reading Liked "..." with plain
     * quotes is worth far more than one that never arrives at all. */
    char folded[SMS_TEXT_LEN];
    ascii_fold(text, folded, sizeof(folded));

    Serial.printf("[MODEM] UCS2 send failed, retrying as GSM-7: %s\n", folded);
    return sms_send_body(number, folded, false);
}

/* Ways of making a noise, tried until one is accepted.
 *
 * Which of these a given A76xx firmware implements is not something the code
 * can find out ahead of time, and an unsupported command simply answers ERROR.
 * So try them, say in the log which one worked, and stop asking once it is
 * clear that none of them will. */
typedef struct {
    const char *name;
    const char *start;
    const char *stop;    // NULL when the start command stops on its own
    uint32_t    hold_ms;
} tone_method_t;

static const tone_method_t tone_methods[] = {
    // Frequency and durations in one go, and it stops itself.
    { "SIMTONE", "AT+SIMTONE=1,1000,300,100,700", NULL,          0   },
    // A named tone that plays until it is told to stop.
    { "CPTONE",  "AT+CPTONE=1",                   "AT+CPTONE=0", 600 },
    // Some firmwares spell the same idea this way.
    { "CTONE",   "AT+CTONE=1",                    "AT+CTONE=0",  600 },
};

static int  tone_first     = 0;    // the one that worked last time
static bool tone_supported = true; // until every one of them has been refused

static void tone_play(void)
{
    if(!tone_supported) return;

    /* Send the output somewhere audible first: 3 is the loudspeaker. Firmwares
     * without this command answer ERROR and are none the worse for it. */
    modem_exec("AT+CSDVC=3", NULL, 0, 2000);

    int count = (int)(sizeof(tone_methods) / sizeof(tone_methods[0]));

    for(int n = 0; n < count; n++) {
        int                  i = (tone_first + n) % count;
        const tone_method_t *m = &tone_methods[i];

        if(!modem_exec(m->start, NULL, 0, 2000)) {
            Serial.printf("[MODEM] %s refused\n", m->name);
            continue;
        }

        if(m->stop) {
            vTaskDelay(pdMS_TO_TICKS(m->hold_ms));
            modem_exec(m->stop, NULL, 0, 2000);
        }

        if(tone_first != i) {
            Serial.printf("[MODEM] notification tone plays with %s\n", m->name);
            tone_first = i;
        }
        return;
    }

    Serial.println("[MODEM] no tone command this modem accepts; text sound will stay silent");
    tone_supported = false;
}

static void handle_request(const modem_req_t *req)
{
    char cmd[CONTACT_NUMBER_LEN + 16];

    switch(req->type) {
        case REQ_DIAL:
            snprintf(cmd, sizeof(cmd), "ATD%s;", req->number);
            status_set_call(MODEM_CALL_DIALING, req->number);
            if(!modem_exec(cmd, NULL, 0, 10000)) {
                status_set_call(MODEM_CALL_IDLE, NULL);
            }
            break;

        case REQ_ANSWER:
            if(modem_exec("ATA", NULL, 0, 10000)) {
                status_set_call(MODEM_CALL_ACTIVE, NULL);
            } else {
                status_set_call(MODEM_CALL_IDLE, NULL);
            }
            break;

        case REQ_HANGUP:
            modem_exec("AT+CHUP", NULL, 0, 5000);
            status_set_call(MODEM_CALL_IDLE, NULL);
            break;

        case REQ_SEND_SMS:
            set_send_state(req->send_id, MODEM_SEND_BUSY);
            set_send_state(req->send_id,
                           sms_send(req->number, req->text) ? MODEM_SEND_OK : MODEM_SEND_FAILED);
            break;

        case REQ_TONE:
            tone_play();
            break;

        case REQ_RAW_AT: {
            char resp[MODEM_LINE_MAX];
            bool ok = modem_exec(req->text, resp, sizeof(resp), 5000);
            Serial.printf("[MODEM] %s -> %s %s\n", req->text, ok ? "OK" : "ERROR", resp);
            break;
        }

        default:
            break;
    }
}

//************************************[ UDP socket ]****************************
/* A single UDP socket, for relaying a tunnel out over the cellular data
 * context. TinyGSM only ever opens TCP on this modem, so the A76xx socket
 * commands are driven directly.
 *
 * Everything here runs on the modem task, which owns the serial port. Packets
 * cross between it and the WiFi side through the two queues.
 *
 * Manual receive (CIPRXGET=1) is used rather than letting the modem push data
 * unsolicited: the task is line oriented and an unannounced binary blob in the
 * middle of an AT exchange would be indistinguishable from a reply. This way
 * the modem only says that something is waiting, and the task reads it when it
 * is between commands.
 */
#define MODEM_UDP_LINK      0   // socket number; one is all the relay needs
#define MODEM_UDP_TXQ_DEPTH 4
#define MODEM_UDP_RXQ_DEPTH 4

typedef struct {
    uint16_t len;
    uint8_t  data[MODEM_UDP_MTU];
} udp_dgram_t;

typedef struct {
    char     apn[40];
    char     host[48];
    uint16_t port;
    uint16_t local_port;
} udp_open_req_t;

static QueueHandle_t udp_txq = NULL;   // WiFi side -> modem
static QueueHandle_t udp_rxq = NULL;   // modem -> WiFi side

static volatile bool udp_open_wanted = false;
static bool          udp_is_open     = false;
static udp_open_req_t udp_req;
static char          udp_error[64]  = {0};
static uint32_t      udp_retry_at   = 0;

static void udp_set_error(const char *why)
{
    strncpy(udp_error, why, sizeof(udp_error) - 1);
    udp_error[sizeof(udp_error) - 1] = '\0';
    Serial.printf("[UDP] %s\n", why);
}

/* Reads exactly `len` bytes of payload. Used after a +CIPRXGET header, where
 * the data is binary and cannot be read a line at a time. */
static bool udp_read_payload(uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    uint16_t got   = 0;
    uint32_t start = millis();

    while(got < len && millis() - start < timeout_ms) {
        while(SerialAT.available() && got < len) {
            buf[got++] = (uint8_t)SerialAT.read();
        }
        if(got < len) vTaskDelay(pdMS_TO_TICKS(2));
    }
    return got == len;
}

/* Brings up the data context and the socket. */
static bool udp_open_socket(void)
{
    char cmd[128];

    if(udp_req.apn[0]) {
        snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,\"IP\",\"%s\"", udp_req.apn);
        modem_exec(cmd, NULL, 0, 5000);
    }

    // Manual receive, and report the sender with each datagram.
    modem_exec("AT+CIPRXGET=1", NULL, 0, 3000);
    modem_exec("AT+CIPSRIP=1", NULL, 0, 3000);

    /* NETOPEN answers +NETOPEN: 0 on success and errors if it is already up,
     * which is not a failure worth reporting. */
    char resp[MODEM_LINE_MAX];
    if(!modem_exec("AT+NETOPEN", resp, sizeof(resp), 30000)) {
        if(!modem_exec("AT+NETOPEN?", resp, sizeof(resp), 5000) ||
           strstr(resp, "+NETOPEN: 1") == NULL) {
            udp_set_error("no data context - check the APN");
            return false;
        }
    }

    snprintf(cmd, sizeof(cmd), "AT+CIPOPEN=%d,\"UDP\",,,%u",
             MODEM_UDP_LINK, (unsigned)udp_req.local_port);
    if(!modem_exec(cmd, NULL, 0, 20000)) {
        udp_set_error("the modem refused a UDP socket");
        return false;
    }

    Serial.printf("[UDP] socket open, relaying to %s:%u\n", udp_req.host, udp_req.port);
    udp_error[0] = '\0';
    return true;
}

static void udp_close_socket(void)
{
    char cmd[32];

    snprintf(cmd, sizeof(cmd), "AT+CIPCLOSE=%d", MODEM_UDP_LINK);
    modem_exec(cmd, NULL, 0, 10000);
    modem_exec("AT+NETCLOSE", NULL, 0, 10000);

    udp_is_open = false;
    Serial.println("[UDP] socket closed");
}

/* Pushes one queued datagram out to the far end. */
static void udp_pump_tx(void)
{
    static udp_dgram_t out;   // static: too big for the task stack
    char cmd[96];
    char line[MODEM_LINE_MAX];

    if(xQueueReceive(udp_txq, &out, 0) != pdTRUE) return;

    /* The destination goes on every send. A UDP socket opened without a peer
     * can address anywhere, which is what lets the far end be changed without
     * tearing the socket down. */
    snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%d,%u,\"%s\",%u",
             MODEM_UDP_LINK, (unsigned)out.len, udp_req.host, (unsigned)udp_req.port);

    while(SerialAT.available()) SerialAT.read();
    SerialAT.print(cmd);
    SerialAT.print("\r");

    // Wait for the '>' prompt, as with sending a message.
    uint32_t start  = millis();
    bool     prompt = false;
    while(millis() - start < 3000 && !prompt) {
        while(SerialAT.available()) {
            if((char)SerialAT.read() == '>') {
                prompt = true;
                break;
            }
        }
        if(!prompt) vTaskDelay(pdMS_TO_TICKS(2));
    }

    if(!prompt) {
        udp_set_error("no send prompt for a datagram");
        return;
    }

    SerialAT.write(out.data, out.len);

    start = millis();
    while(millis() - start < 5000) {
        if(modem_read_line(line, sizeof(line), 200) < 0) continue;
        if(strncmp(line, "+CIPSEND:", 9) == 0) continue;
        if(strcmp(line, "OK") == 0) return;
        if(strcmp(line, "ERROR") == 0 || strncmp(line, "+CIP", 4) == 0) {
            Serial.printf("[UDP] send refused: %s\n", line);
            return;
        }
        handle_urc(line);
    }
}

/* Collects one waiting datagram from the modem. */
static void udp_pump_rx(void)
{
    static udp_dgram_t in;   // static: too big for the task stack
    char cmd[48];
    char line[MODEM_LINE_MAX];

    udp_rx_pending = false;

    snprintf(cmd, sizeof(cmd), "AT+CIPRXGET=2,%d,%d", MODEM_UDP_LINK, MODEM_UDP_MTU);
    while(SerialAT.available()) SerialAT.read();
    SerialAT.print(cmd);
    SerialAT.print("\r");

    uint32_t start = millis();
    while(millis() - start < 5000) {
        if(modem_read_line(line, sizeof(line), 200) < 0) continue;

        if(strcmp(line, "ERROR") == 0) return;

        if(strncmp(line, "+CIPRXGET:", 10) == 0) {
            /* +CIPRXGET: 2,<link>,<read>,<remaining>[,"<ip>",<port>] and then
             * exactly <read> bytes of payload. */
            int mode = 0, link = 0, got = 0, left = 0;
            if(sscanf(line + 10, "%d,%d,%d,%d", &mode, &link, &got, &left) < 3) continue;
            if(mode != 2 || got <= 0) continue;

            if(got > MODEM_UDP_MTU) got = MODEM_UDP_MTU;

            if(!udp_read_payload(in.data, (uint16_t)got, 3000)) {
                Serial.println("[UDP] short read on an incoming datagram");
                return;
            }
            in.len = (uint16_t)got;

            if(xQueueSend(udp_rxq, &in, 0) != pdTRUE) {
                Serial.println("[UDP] receive queue full, datagram dropped");
            }

            // More may be waiting; the next loop will come back for it.
            if(left > 0) udp_rx_pending = true;
            continue;
        }

        if(strcmp(line, "OK") == 0) return;
        handle_urc(line);
    }
}

/* Called from the task loop, between commands. */
static void udp_service(void)
{
    if(udp_open_wanted && !udp_is_open) {
        if(udp_retry_at != 0 && millis() < udp_retry_at) return;

        if(udp_open_socket()) {
            udp_is_open  = true;
            udp_retry_at = 0;
        } else {
            // Do not hammer the modem while whatever is wrong stays wrong.
            udp_retry_at = millis() + 15000;
        }
        return;
    }

    if(!udp_open_wanted && udp_is_open) {
        udp_close_socket();
        return;
    }

    if(!udp_is_open) return;

    if(udp_rx_pending) udp_pump_rx();
    udp_pump_tx();
}

//************************************[ UDP public API ]************************
void modem_udp_open(const char *apn, const char *host, uint16_t port, uint16_t local_port)
{
    if(host == NULL || host[0] == '\0') return;

    memset(&udp_req, 0, sizeof(udp_req));
    if(apn) strncpy(udp_req.apn, apn, sizeof(udp_req.apn) - 1);
    strncpy(udp_req.host, host, sizeof(udp_req.host) - 1);
    udp_req.port       = port;
    udp_req.local_port = local_port;

    udp_error[0]    = '\0';
    udp_retry_at    = 0;
    udp_open_wanted = true;
}

void modem_udp_close(void)
{
    udp_open_wanted = false;
}

bool modem_udp_is_open(void)
{
    return udp_is_open;
}

void modem_udp_get_error(char *buf, int len)
{
    if(buf == NULL || len <= 0) return;
    strncpy(buf, udp_error, len - 1);
    buf[len - 1] = '\0';
}

bool modem_udp_send(const uint8_t *data, uint16_t len)
{
    if(udp_txq == NULL || data == NULL || len == 0 || len > MODEM_UDP_MTU) return false;

    static udp_dgram_t pkt;   // only ever touched by the caller's task
    pkt.len = len;
    memcpy(pkt.data, data, len);

    return xQueueSend(udp_txq, &pkt, 0) == pdTRUE;
}

bool modem_udp_receive(uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{
    if(udp_rxq == NULL || buf == NULL) return false;

    static udp_dgram_t pkt;
    if(xQueueReceive(udp_rxq, &pkt, 0) != pdTRUE) return false;

    uint16_t n = pkt.len > buf_len ? buf_len : pkt.len;
    memcpy(buf, pkt.data, n);
    if(out_len) *out_len = n;
    return true;
}

//************************************[ task ]**********************************
static void modem_configure(void)
{
    // Text mode with caller ID and index-style new message notifications. The
    // modem is asked to store incoming messages rather than push them inline,
    // which survives us being busy with another command.
    modem_exec("ATE0", NULL, 0, 2000);            // no echo, keeps parsing simple
    modem_exec("AT+CMEE=1", NULL, 0, 2000);       // numeric error codes
    modem_exec("AT+CLIP=1", NULL, 0, 2000);       // caller ID on incoming calls
    modem_exec("AT+CMGF=1", NULL, 0, 2000);       // SMS text mode
    modem_exec("AT+CSCS=\"GSM\"", NULL, 0, 2000);
    modem_exec("AT+CPMS=\"SM\",\"SM\",\"SM\"", NULL, 0, 5000);
    modem_exec("AT+CNMI=2,1,0,0,0", NULL, 0, 2000);

    // Report the data coding scheme with each message, so a UCS2 body can be
    // told apart from one that merely looks like hex.
    modem_exec("AT+CSDH=1", NULL, 0, 2000);
    // The coding scheme for what we send. sms_send switches to UCS2 per message
    // when the text needs it and puts this back afterwards.
    modem_exec("AT+CSMP=17,167,0,0", NULL, 0, 2000);

    // Let the network set the module's clock (NITZ) and tell us when it moves.
    // This is what makes AT+CCLK? worth reading: without CTZU the module just
    // free-runs from whatever it powered up with.
    modem_exec("AT+CTZU=1", NULL, 0, 2000);
    modem_exec("AT+CTZR=1", NULL, 0, 2000);

    clock_read_pending = true;
}

/* Drains any messages that arrived while we were powered down. */
static void sms_drain_stored(void)
{
    char line[MODEM_LINE_MAX];
    int  indexes[16];
    int  n = 0;

    while(SerialAT.available()) SerialAT.read();
    modem_write_line("AT+CMGL=\"ALL\"");

    uint32_t start = millis();
    while(millis() - start < 10000) {
        if(modem_read_line(line, sizeof(line), 300) < 0) continue;
        if(strcmp(line, "OK") == 0) break;
        if(strcmp(line, "ERROR") == 0 || strncmp(line, "+CMS ERROR", 10) == 0) return;

        if(strncmp(line, "+CMGL:", 6) == 0 && n < (int)(sizeof(indexes) / sizeof(indexes[0]))) {
            indexes[n++] = atoi(line + 6);
        }
    }

    for(int i = 0; i < n; i++) {
        sms_fetch(indexes[i]);
    }
}

/* Asks the modem what the current call is actually doing.
 *
 * The URCs alone are not enough to trust: whether a module announces
 * `VOICE CALL: BEGIN` and `NO CARRIER` for every transition varies with
 * firmware, and a call that quietly went away would otherwise leave the UI
 * showing "Calling..." forever. Polled only while a call is up, so it costs
 * nothing the rest of the time. */
static void modem_poll_call(void)
{
    char line[MODEM_LINE_MAX];
    char number[CONTACT_NUMBER_LEN] = {0};
    int  stat = -1;

    while(SerialAT.available()) SerialAT.read();
    modem_write_line("AT+CLCC");

    uint32_t start = millis();
    while(millis() - start < 3000) {
        if(modem_read_line(line, sizeof(line), 200) < 0) continue;

        if(strcmp(line, "OK") == 0) break;
        // An error means the module does not answer +CLCC; leave the state
        // exactly as the URCs left it rather than guessing.
        if(strcmp(line, "ERROR") == 0 || strncmp(line, "+CME ERROR", 10) == 0) return;

        if(strncmp(line, "+CLCC:", 6) == 0) {
            int id, dir, s;
            if(stat < 0 && sscanf(line, "+CLCC: %d,%d,%d", &id, &dir, &s) == 3) {
                stat = s;
                urc_quoted_field(line, 0, number, sizeof(number));
            }
            continue;
        }

        handle_urc(line);
    }

    const char *who = number[0] ? number : NULL;

    switch(stat) {
        case -1: status_set_call(MODEM_CALL_IDLE, NULL);      break; // no call listed
        case 0:  status_set_call(MODEM_CALL_ACTIVE, who);     break;
        case 4:                                                       // incoming
        case 5:  status_set_call(MODEM_CALL_INCOMING, who);   break; // waiting
        default: status_set_call(MODEM_CALL_DIALING, who);    break; // dialing / alerting
    }
}

/* Reads the module's real time clock and hands it to the system clock.
 *
 * AT+CTZU=1 asks the module to keep this clock in step with the network's NITZ
 * broadcast, so this is the network's idea of the time. It also carries the
 * local UTC offset, which is the one thing a GPS fix cannot tell us.
 *
 * Returns true once a genuine time has been read, so the caller can stop
 * asking; carriers vary in how promptly they send NITZ after registration, and
 * some never do. */
static bool modem_sync_clock(void)
{
    char resp[MODEM_LINE_MAX];
    char stamp[40];

    if(!modem_exec("AT+CCLK?", resp, sizeof(resp), 3000)) return false;
    if(!urc_quoted_last(resp, stamp, sizeof(stamp))) return false;

    modem_stamp_t t;
    if(!parse_modem_stamp(stamp, &t)) {
        Serial.printf("[MODEM] cannot read clock from '%s'\n", stamp);
        return false;
    }

    // A module that has not heard NITZ yet reports its power-on placeholder,
    // which system_clock rejects on the year.
    if(!system_clock_set_utc(CLOCK_SRC_NETWORK, t.year, t.mon, t.day, t.hour, t.min, t.sec)) {
        return false;
    }

    if(t.has_offset) system_clock_set_utc_offset(t.offset_min);
    return true;
}

/* Returns false when the modem did not answer at all, which is how the task
 * notices it was powered down from the settings screen. */
static bool modem_refresh_status(void)
{
    char resp[MODEM_LINE_MAX];

    if(modem_exec("AT+CSQ", resp, sizeof(resp), 3000)) {
        int rssi = 99, ber = 0;
        if(sscanf(resp, "+CSQ: %d,%d", &rssi, &ber) >= 1) {
            xSemaphoreTake(status_lock, portMAX_DELAY);
            status.signal = (uint8_t)rssi;
            xSemaphoreGive(status_lock);
        }
    } else {
        xSemaphoreTake(status_lock, portMAX_DELAY);
        status.signal     = 99;
        status.registered = false;
        xSemaphoreGive(status_lock);
        return false;
    }

    if(modem_exec("AT+CREG?", resp, sizeof(resp), 3000)) {
        int mode = 0, stat = 0;
        if(sscanf(resp, "+CREG: %d,%d", &mode, &stat) == 2) {
            xSemaphoreTake(status_lock, portMAX_DELAY);
            // 1 = registered at home, 5 = registered while roaming
            status.registered = (stat == 1 || stat == 5);
            xSemaphoreGive(status_lock);
        }
    }

    if(modem_exec("AT+COPS?", resp, sizeof(resp), 5000)) {
        char name[MODEM_OPERATOR_LEN];
        if(urc_quoted_field(resp, 0, name, sizeof(name))) {
            xSemaphoreTake(status_lock, portMAX_DELAY);
            strncpy(status.op_name, name, MODEM_OPERATOR_LEN - 1);
            status.op_name[MODEM_OPERATOR_LEN - 1] = '\0';
            xSemaphoreGive(status_lock);
        }
    }

    return true;
}

static void modem_task(void *param)
{
    char        line[MODEM_LINE_MAX];
    modem_req_t req;
    uint32_t    last_status    = 0;
    uint32_t    last_call_poll = 0;
    uint32_t    last_clock_try = 0; // 0 means "not tried yet", so the first go is immediate
    bool        configured     = false;

    for(;;) {
        if(!configured) {
            if(modem_exec("AT", NULL, 0, 1000)) {
                Serial.println("[MODEM] configuring");
                modem_configure();
                sms_drain_stored();
                modem_refresh_status();
                last_status = millis();
                configured  = true;
            } else {
                // The modem is not answering yet (still booting, or powered
                // down from the settings screen). Retry without busy-waiting.
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
        }

        // 1. Anything the modem said on its own.
        while(SerialAT.available()) {
            if(modem_read_line(line, sizeof(line), 100) > 0) {
                Serial.printf("[MODEM] < %s\n", line);
                handle_urc(line);
            }
        }

        // 2. Messages an URC pointed at, now that no command is in flight.
        while(pending_sms_num > 0) {
            int index = pending_sms[0];
            pending_sms_num--;
            memmove(&pending_sms[0], &pending_sms[1], sizeof(int) * pending_sms_num);
            sms_fetch(index);
        }

        // 3. One queued request.
        if(xQueueReceive(req_queue, &req, 0) == pdTRUE) {
            handle_request(&req);
        }

        // 4. Pick up a network time when one has been announced, or until the
        //    first read succeeds. Registration comes first - there is no NITZ
        //    to have received before the phone is on a network.
        if(clock_read_pending && configured && modem_is_registered() &&
           (last_clock_try == 0 || millis() - last_clock_try > MODEM_CLOCK_RETRY_MS)) {
            last_clock_try = millis();
            if(modem_sync_clock()) clock_read_pending = false;
        }

        // 5. The relay socket, if one is up.
        udp_service();

        // 6. While a call is up, confirm what it is really doing.
        if(status_get_call() != MODEM_CALL_IDLE && millis() - last_call_poll > MODEM_CALL_POLL_MS) {
            last_call_poll = millis();
            modem_poll_call();
        }

        // 7. Periodic signal / registration refresh. Losing the modem here
        //    means it was powered down (the settings screen can do that), so
        //    drop back to detection and reconfigure it when it returns.
        if(millis() - last_status > MODEM_STATUS_PERIOD_MS && !udp_is_open) {
            last_status = millis();
            if(!modem_refresh_status()) {
                Serial.println("[MODEM] stopped answering, waiting for it to come back");
                status_set_call(MODEM_CALL_IDLE, NULL);
                pending_sms_num    = 0;
                clock_read_pending = true;
                last_clock_try     = 0;
                configured         = false;
            }
        }

        // A relay wants the loop tight; idle otherwise so the CPU is free.
        vTaskDelay(pdMS_TO_TICKS(udp_is_open ? 2 : 50));
    }
}

//************************************[ public API ]****************************
void modem_service_init(bool alive)
{
    if(modem_task_h != NULL) return;

    Serial.printf("[MODEM] service starting, modem %s at boot\n", alive ? "responded" : "silent");

    memset(&status, 0, sizeof(status));
    status.signal = 99;

    status_lock = xSemaphoreCreateMutex();
    req_queue   = xQueueCreate(MODEM_REQ_QUEUE, sizeof(modem_req_t));
    rx_queue    = xQueueCreate(MODEM_RX_QUEUE, sizeof(modem_sms_rx_t));
    udp_txq     = xQueueCreate(MODEM_UDP_TXQ_DEPTH, sizeof(udp_dgram_t));
    udp_rxq     = xQueueCreate(MODEM_UDP_RXQ_DEPTH, sizeof(udp_dgram_t));

    if(status_lock == NULL || req_queue == NULL || rx_queue == NULL ||
       udp_txq == NULL || udp_rxq == NULL) {
        Serial.println("[MODEM] service allocation failed");
        return;
    }

    xTaskCreate(modem_task, "modem", MODEM_TASK_STACK, NULL, A7682E_PRIORITY, &modem_task_h);
}

static void modem_post(const modem_req_t *req)
{
    if(req_queue == NULL) return;
    if(xQueueSend(req_queue, req, pdMS_TO_TICKS(50)) != pdTRUE) {
        Serial.println("[MODEM] request queue full");
    }
}

modem_call_state_t modem_get_call_state(void)
{
    if(status_lock == NULL) return MODEM_CALL_IDLE;
    return status_get_call();
}

void modem_get_call_number(char *buf, int len)
{
    if(buf == NULL || len <= 0) return;
    buf[0] = '\0';
    if(status_lock == NULL) return;

    xSemaphoreTake(status_lock, portMAX_DELAY);
    strncpy(buf, status.call_number, len - 1);
    buf[len - 1] = '\0';
    xSemaphoreGive(status_lock);
}

uint32_t modem_get_call_duration(void)
{
    if(status_lock == NULL) return 0;

    xSemaphoreTake(status_lock, portMAX_DELAY);
    uint32_t connected = status.call_connect_ms;
    xSemaphoreGive(status_lock);

    return connected ? (millis() - connected) : 0;
}

void modem_dial(const char *number)
{
    if(number == NULL || number[0] == '\0') return;

    modem_req_t req;
    memset(&req, 0, sizeof(req));
    req.type = REQ_DIAL;
    strncpy(req.number, number, CONTACT_NUMBER_LEN - 1);
    modem_post(&req);
}

void modem_answer(void)
{
    modem_req_t req;
    memset(&req, 0, sizeof(req));
    req.type = REQ_ANSWER;
    modem_post(&req);
}

void modem_hangup(void)
{
    modem_req_t req;
    memset(&req, 0, sizeof(req));
    req.type = REQ_HANGUP;
    modem_post(&req);
}

uint32_t modem_send_sms(const char *number, const char *text)
{
    if(number == NULL || number[0] == '\0' || text == NULL || text[0] == '\0') return 0;
    if(req_queue == NULL) return 0;

    modem_req_t req;
    memset(&req, 0, sizeof(req));
    req.type    = REQ_SEND_SMS;
    req.send_id = next_send_id++;
    strncpy(req.number, number, CONTACT_NUMBER_LEN - 1);
    strncpy(req.text, text, SMS_TEXT_LEN - 1);

    set_send_state(req.send_id, MODEM_SEND_BUSY);
    modem_post(&req);
    return req.send_id;
}

modem_send_state_t modem_get_send_state(uint32_t send_id)
{
    if(status_lock == NULL || send_id == 0) return MODEM_SEND_IDLE;

    xSemaphoreTake(status_lock, portMAX_DELAY);
    modem_send_state_t st = (status.send_id == send_id) ? status.send_state : MODEM_SEND_IDLE;
    xSemaphoreGive(status_lock);
    return st;
}

bool modem_poll_sms(modem_sms_rx_t *out)
{
    if(rx_queue == NULL || out == NULL) return false;
    return xQueueReceive(rx_queue, out, 0) == pdTRUE;
}

bool modem_is_registered(void)
{
    if(status_lock == NULL) return false;

    xSemaphoreTake(status_lock, portMAX_DELAY);
    bool reg = status.registered;
    xSemaphoreGive(status_lock);
    return reg;
}

uint8_t modem_get_signal(void)
{
    if(status_lock == NULL) return 99;

    xSemaphoreTake(status_lock, portMAX_DELAY);
    uint8_t sig = status.signal;
    xSemaphoreGive(status_lock);
    return sig;
}

void modem_get_operator(char *buf, int len)
{
    if(buf == NULL || len <= 0) return;
    buf[0] = '\0';
    if(status_lock == NULL) return;

    xSemaphoreTake(status_lock, portMAX_DELAY);
    strncpy(buf, status.op_name, len - 1);
    buf[len - 1] = '\0';
    xSemaphoreGive(status_lock);
}

void modem_play_tone(void)
{
    modem_req_t req;
    memset(&req, 0, sizeof(req));
    req.type = REQ_TONE;
    modem_post(&req);
}

void modem_request_at(const char *cmd)
{
    if(cmd == NULL || cmd[0] == '\0') return;

    modem_req_t req;
    memset(&req, 0, sizeof(req));
    req.type = REQ_RAW_AT;
    strncpy(req.text, cmd, SMS_TEXT_LEN - 1);
    modem_post(&req);
}
