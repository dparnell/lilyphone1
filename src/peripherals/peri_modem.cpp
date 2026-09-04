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
static bool sms_send(const char *number, const char *text)
{
    char cmd[CONTACT_NUMBER_LEN + 16];
    char line[MODEM_LINE_MAX];

    while(SerialAT.available()) SerialAT.read();

    snprintf(cmd, sizeof(cmd), "AT+CMGS=\"%s\"", number);
    modem_write_line(cmd);

    // Wait for the prompt.
    uint32_t start = millis();
    bool prompt = false;
    while(millis() - start < 5000 && !prompt) {
        while(SerialAT.available()) {
            if((char)SerialAT.read() == '>') {
                prompt = true;
                break;
            }
        }
        if(!prompt) vTaskDelay(pdMS_TO_TICKS(10));
    }

    if(!prompt) {
        Serial.println("[MODEM] no send prompt");
        // Abort the half-issued command so the modem does not stay in text mode.
        SerialAT.write(0x1B);
        return false;
    }

    SerialAT.print(text);
    SerialAT.write(0x1A); // Ctrl-Z ends the body

    // The network can take a while to accept a message.
    start = millis();
    while(millis() - start < 60000) {
        if(modem_read_line(line, sizeof(line), 500) < 0) continue;

        if(strncmp(line, "+CMGS:", 6) == 0) continue; // reference number
        if(strcmp(line, "OK") == 0) return true;
        if(strcmp(line, "ERROR") == 0) return false;
        if(strncmp(line, "+CMS ERROR", 10) == 0) return false;
        handle_urc(line);
    }
    return false;
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
            // Start, hold briefly, stop. A module without CPTONE answers ERROR
            // to both, which costs a couple of milliseconds and does nothing.
            modem_exec("AT+CPTONE=1", NULL, 0, 2000);
            vTaskDelay(pdMS_TO_TICKS(500));
            modem_exec("AT+CPTONE=0", NULL, 0, 2000);
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

        // 5. While a call is up, confirm what it is really doing.
        if(status_get_call() != MODEM_CALL_IDLE && millis() - last_call_poll > MODEM_CALL_POLL_MS) {
            last_call_poll = millis();
            modem_poll_call();
        }

        // 6. Periodic signal / registration refresh. Losing the modem here
        //    means it was powered down (the settings screen can do that), so
        //    drop back to detection and reconfigure it when it returns.
        if(millis() - last_status > MODEM_STATUS_PERIOD_MS) {
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

        vTaskDelay(pdMS_TO_TICKS(50));
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

    if(status_lock == NULL || req_queue == NULL || rx_queue == NULL) {
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
