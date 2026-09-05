/**
 * The MeshCore companion protocol.
 *
 * A MeshCore companion app - the official phone app, or anything else speaking
 * the same protocol - drives a radio over a byte stream: it sends command
 * frames and the radio answers with response frames, plus pushes of its own
 * when something arrives off the air. This is the radio half of that.
 *
 * Two things make this implementation different from the stock companion
 * firmware. That firmware is *only* a radio: the app is its entire user
 * interface. This device has its own screen and keyboard, so the app is a
 * second way in rather than the only one, and both drive the same node - a
 * message typed here and one sent from the app go out over the same identity,
 * and a message that arrives is delivered to both. The other difference follows
 * from that: contacts are not persisted here, because they are rebuilt from the
 * adverts nodes send anyway, so an app that reconnects after a restart will
 * re-add whatever it has that this node has not heard yet.
 *
 * Everything that touches MeshCore runs on the mesh task, because MeshCore is
 * not thread-safe and the UI must not reach any of it - the UI's part is
 * choosing the link and reading the status, which is what mesh_companion.h
 * exposes. The one exception is bringing a link up or down, which happens on a
 * task of its own: it blocks for a long time and goes far deeper into the stack
 * than the mesh task has to give.
 *
 * Frames are handed to a BaseSerialInterface, which is where the two link
 * variants differ and the only place they do: Bluetooth is a GATT service whose
 * writes arrive on the BLE task and are queued, WiFi is a TCP server on an
 * access point this device puts up. One at a time - each wants a sizeable bite
 * of internal memory, and the WiFi link cannot have the radio while the UDP
 * hotspot is using it.
 */
#include <Arduino.h>
#include <Preferences.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include <helpers/esp32/SerialBLEInterface.h>
#include <helpers/esp32/SerialWifiInterface.h>

#include "mesh_companion.h"
#include "mesh_companion_int.h"
#include "mesh_net.h"
#include "udp_relay.h"

extern "C" uint16_t ui_battery_27220_get_voltage(void);

#define MESH_PREFS_NAMESPACE "mesh"

/* What this node tells the app it understands.
 *
 * Deliberately not the newest: the version code is a promise about which
 * commands will work, and claiming a high one invites the app to ask for
 * things - custom variables, statistics, flood scoping - that this
 * implementation does not answer. Seven covers everything a conversation
 * needs. Anything beyond it is refused honestly with "unsupported command"
 * rather than by going quiet. */
#define FIRMWARE_VER_CODE   7
#define FIRMWARE_VERSION    "lilyphone1"
#define FIRMWARE_BUILD_DATE "05 Sep 2026"

//************************************[ the protocol ]**************************
#define CMD_APP_START              1
#define CMD_SEND_TXT_MSG           2
#define CMD_SEND_CHANNEL_TXT_MSG   3
#define CMD_GET_CONTACTS           4
#define CMD_GET_DEVICE_TIME        5
#define CMD_SET_DEVICE_TIME        6
#define CMD_SEND_SELF_ADVERT       7
#define CMD_SET_ADVERT_NAME        8
#define CMD_ADD_UPDATE_CONTACT     9
#define CMD_SYNC_NEXT_MESSAGE      10
#define CMD_SET_RADIO_PARAMS       11
#define CMD_SET_RADIO_TX_POWER     12
#define CMD_RESET_PATH             13
#define CMD_SET_ADVERT_LATLON      14
#define CMD_REMOVE_CONTACT         15
#define CMD_SHARE_CONTACT          16
#define CMD_EXPORT_CONTACT         17
#define CMD_IMPORT_CONTACT         18
#define CMD_REBOOT                 19
#define CMD_GET_BATT_AND_STORAGE   20
#define CMD_SET_TUNING_PARAMS      21
#define CMD_DEVICE_QUERY           22
#define CMD_GET_CONTACT_BY_KEY     30
#define CMD_GET_CHANNEL            31
#define CMD_SET_CHANNEL            32
#define CMD_SET_OTHER_PARAMS       38
#define CMD_GET_TUNING_PARAMS      43

#define RESP_CODE_OK                  0
#define RESP_CODE_ERR                 1
#define RESP_CODE_CONTACTS_START      2
#define RESP_CODE_CONTACT             3
#define RESP_CODE_END_OF_CONTACTS     4
#define RESP_CODE_SELF_INFO           5
#define RESP_CODE_SENT                6
#define RESP_CODE_CONTACT_MSG_RECV    7
#define RESP_CODE_CHANNEL_MSG_RECV    8
#define RESP_CODE_CURR_TIME           9
#define RESP_CODE_NO_MORE_MESSAGES    10
#define RESP_CODE_EXPORT_CONTACT      11
#define RESP_CODE_BATT_AND_STORAGE    12
#define RESP_CODE_DEVICE_INFO         13
#define RESP_CODE_CONTACT_MSG_RECV_V3 16
#define RESP_CODE_CHANNEL_MSG_RECV_V3 17
#define RESP_CODE_CHANNEL_INFO        18
#define RESP_CODE_TUNING_PARAMS       23

// Sent to the app whenever, rather than in answer to anything.
#define PUSH_CODE_ADVERT           0x80
#define PUSH_CODE_PATH_UPDATED     0x81
#define PUSH_CODE_SEND_CONFIRMED   0x82
#define PUSH_CODE_MSG_WAITING      0x83
#define PUSH_CODE_NEW_ADVERT       0x8A

#define ERR_CODE_UNSUPPORTED_CMD   1
#define ERR_CODE_NOT_FOUND         2
#define ERR_CODE_TABLE_FULL        3
#define ERR_CODE_BAD_STATE         4
#define ERR_CODE_FILE_IO_ERROR     5
#define ERR_CODE_ILLEGAL_ARG       6

//************************************[ state ]*********************************
static BaseChatMesh *chat_mesh = NULL;

static SerialBLEInterface  *ble_link  = NULL;
static SerialWifiInterface *wifi_link = NULL;
static BaseSerialInterface *active_link = NULL;

static int  link_mode    = MESH_LINK_OFF;
static int  link_wanted  = MESH_LINK_OFF;
static bool ble_started  = false;   // BLEDevice::init() is a one-way door
static bool wifi_started = false;

static uint32_t ble_pin  = 0;
static char     ap_ssid[MESH_LINK_SSID_LEN] = "lilyphone1-mesh";
static char     ap_pass[MESH_LINK_PASS_LEN] = "meshcore";
static char     link_detail[64] = "";
static char     link_address[32] = "";

static uint32_t frames_rx = 0;
static uint32_t frames_tx = 0;

static uint8_t cmd_frame[MAX_FRAME_SIZE + 1];
static uint8_t out_frame[MAX_FRAME_SIZE + 1];

static uint8_t app_target_ver = 0;

/* Messages held for the app. It collects them one at a time, and they are kept
 * while it is away so that a conversation is not lost because a phone was in
 * somebody's pocket. */
#define OFFLINE_QUEUE_SIZE 16
struct queued_frame_t {
    uint8_t len;
    uint8_t buf[MAX_FRAME_SIZE];
};
static queued_frame_t offline_queue[OFFLINE_QUEUE_SIZE];
static int            offline_queue_len = 0;

/* Acknowledgements the app is waiting on. MeshCore names a packet hash rather
 * than a message, so the only way to report a delivery is to remember what was
 * sent and match on the hash coming back. Several may be outstanding, since the
 * app is free to send faster than the mesh answers. */
#define ACK_TABLE_SIZE 8
struct ack_wait_t {
    uint32_t      ack;
    unsigned long sent_ms;
    ContactInfo  *contact;
};
static ack_wait_t ack_table[ACK_TABLE_SIZE];
static int        next_ack_idx = 0;

/* A contacts listing in progress. The app asks once and the contacts are then
 * fed out one frame per pass, so that a long list does not monopolise the mesh
 * task or overrun the link's own queue. */
static ContactsIterator contacts_iter(0);
static bool             iter_running = false;
static uint32_t         iter_since   = 0;
static uint32_t         iter_newest_lastmod = 0;

//************************************[ helpers ]*******************************
static void link_set_detail(const char *text)
{
    snprintf(link_detail, sizeof(link_detail), "%s", text ? text : "");
}

static bool link_up(void)
{
    return active_link != NULL && active_link->isConnected();
}

static void write_frame(const uint8_t *buf, int len)
{
    if(active_link == NULL) return;

    if(active_link->writeFrame(buf, len) > 0) frames_tx++;
}

static void write_ok(void)
{
    uint8_t buf[1] = { RESP_CODE_OK };
    write_frame(buf, 1);
}

static void write_err(uint8_t code)
{
    uint8_t buf[2] = { RESP_CODE_ERR, code };
    write_frame(buf, 2);
}

/* One contact, in the layout the app expects. The same shape is used both as a
 * reply to a listing and as the push that announces somebody new. */
static void write_contact(uint8_t code, const ContactInfo &c)
{
    int i = 0;

    out_frame[i++] = code;
    memcpy(&out_frame[i], c.id.pub_key, PUB_KEY_SIZE);       i += PUB_KEY_SIZE;
    out_frame[i++] = c.type;
    out_frame[i++] = c.flags;
    out_frame[i++] = c.out_path_len;
    memcpy(&out_frame[i], c.out_path, MAX_PATH_SIZE);        i += MAX_PATH_SIZE;
    StrHelper::strzcpy((char *)&out_frame[i], c.name, 32);   i += 32;
    memcpy(&out_frame[i], &c.last_advert_timestamp, 4);      i += 4;
    memcpy(&out_frame[i], &c.gps_lat, 4);                    i += 4;
    memcpy(&out_frame[i], &c.gps_lon, 4);                    i += 4;
    memcpy(&out_frame[i], &c.lastmod, 4);                    i += 4;

    write_frame(out_frame, i);
}

static void read_contact(ContactInfo &c, uint32_t &last_mod, const uint8_t *frame, int len)
{
    int i = 1;   // [0] is the command code

    memcpy(c.id.pub_key, &frame[i], PUB_KEY_SIZE);  i += PUB_KEY_SIZE;
    c.type         = frame[i++];
    c.flags        = frame[i++];
    c.out_path_len = frame[i++];
    memcpy(c.out_path, &frame[i], MAX_PATH_SIZE);   i += MAX_PATH_SIZE;
    memcpy(c.name, &frame[i], 32);                  i += 32;
    c.name[31] = 0;
    memcpy(&c.last_advert_timestamp, &frame[i], 4); i += 4;

    if(len >= i + 8) {   // the location is optional
        memcpy(&c.gps_lat, &frame[i], 4);  i += 4;
        memcpy(&c.gps_lon, &frame[i], 4);  i += 4;

        if(len >= i + 4) memcpy(&last_mod, &frame[i], 4);
    }
}

static void queue_offline(const uint8_t *frame, int len)
{
    if(offline_queue_len >= OFFLINE_QUEUE_SIZE) {
        // Drop the oldest rather than the newest: what just arrived is the part
        // of the conversation somebody is waiting on.
        memmove(&offline_queue[0], &offline_queue[1],
                sizeof(queued_frame_t) * (OFFLINE_QUEUE_SIZE - 1));
        offline_queue_len = OFFLINE_QUEUE_SIZE - 1;
    }

    offline_queue[offline_queue_len].len = (uint8_t)len;
    memcpy(offline_queue[offline_queue_len].buf, frame, len);
    offline_queue_len++;

    if(link_up()) {
        uint8_t tickle[1] = { PUSH_CODE_MSG_WAITING };
        write_frame(tickle, 1);
    }
}

static int take_offline(uint8_t *frame)
{
    if(offline_queue_len <= 0) return 0;

    int len = offline_queue[0].len;
    memcpy(frame, offline_queue[0].buf, len);

    offline_queue_len--;
    memmove(&offline_queue[0], &offline_queue[1], sizeof(queued_frame_t) * offline_queue_len);
    return len;
}

//************************************[ settings ]******************************
static void companion_save(void)
{
    Preferences prefs;
    if(!prefs.begin(MESH_PREFS_NAMESPACE, false)) return;

    prefs.putInt("link", link_wanted);
    prefs.putUInt("blepin", ble_pin);
    prefs.putString("apssid", ap_ssid);
    prefs.putString("appass", ap_pass);
    prefs.end();
}

/* The crash latch.
 *
 * The link is remembered and comes back on its own at boot, which is what makes
 * it useful - and also what would make a link that cannot start into a device
 * that cannot be used: it would fail, restart, and fail again with nobody able
 * to reach the setting that turns it off.
 *
 * So the attempt is written down before it is made and rubbed out once the link
 * has been up and stable for a while. Finding it still written at boot means
 * the last attempt did not survive, and the link is left off with an
 * explanation rather than tried again.
 */
#define LINK_SETTLE_MS 20000   // up this long before an attempt counts as good

static void latch_set(int mode)
{
    Preferences prefs;
    if(!prefs.begin(MESH_PREFS_NAMESPACE, false)) return;

    prefs.putUChar("linktry", (uint8_t)mode);
    prefs.end();   // committed here, which is the point - the crash comes next
}

static void latch_clear(void)
{
    Preferences prefs;
    if(!prefs.begin(MESH_PREFS_NAMESPACE, false)) return;

    prefs.putUChar("linktry", (uint8_t)MESH_LINK_OFF);
    prefs.end();
}

static void companion_load(void)
{
    int latched = MESH_LINK_OFF;

    Preferences prefs;
    if(prefs.begin(MESH_PREFS_NAMESPACE, true)) {
        link_wanted = prefs.getInt("link", MESH_LINK_OFF);
        ble_pin     = prefs.getUInt("blepin", 0);
        latched     = prefs.getUChar("linktry", MESH_LINK_OFF);
        prefs.getString("apssid", ap_ssid, sizeof(ap_ssid));
        prefs.getString("appass", ap_pass, sizeof(ap_pass));
        prefs.end();
    }

    if(link_wanted < MESH_LINK_OFF || link_wanted > MESH_LINK_WIFI) link_wanted = MESH_LINK_OFF;
    if(ap_ssid[0] == '\0') snprintf(ap_ssid, sizeof(ap_ssid), "lilyphone1-mesh");

    if(latched != MESH_LINK_OFF) {
        Serial.println("[LINK] the last attempt to start the link did not survive; left off");
        link_set_detail("last attempt crashed - turn it on again to retry");
        link_wanted = MESH_LINK_OFF;
        latch_clear();
        companion_save();
    }

    /* The pairing code is generated once and then kept. A code that changed on
     * every boot would unpair the phone every time the battery went flat. */
    if(ble_pin < 100000 || ble_pin > 999999) {
        ble_pin = 100000 + (esp_random() % 900000);
        companion_save();
    }
}

//************************************[ link control ]*************************
/* How much internal RAM each radio needs to have a chance of starting. These
 * are not the stacks' documented requirements - there are none - but enough
 * headroom that a failure is a real failure rather than this device having
 * already spent the memory on the display and the modem. Refusing with a number
 * beats aborting inside the Bluetooth stack, which is what happens otherwise. */
#define BLE_MIN_INTERNAL_FREE   64000
#define WIFI_MIN_INTERNAL_FREE  48000

static TaskHandle_t  link_task_h   = NULL;
static volatile bool link_changing = false;
static bool          link_latched  = false;
static unsigned long link_up_since = 0;

/* Refusing to start a radio, and saying the useful thing rather than the merely
 * true one. There is enough memory for either radio at startup - the link is a
 * remembered setting, and the display gives up its fast drawing buffer when it
 * is on - but not once a full screen buffer has already been taken out of
 * internal RAM. So the answer to being short now is a restart. */
static void link_no_room(const char *what, size_t have, size_t want)
{
    link_set_detail("restart the phone to start the link");
    link_mode = MESH_LINK_OFF;

    Serial.printf("[LINK] %s wants about %uK of internal RAM and %uK is free; "
                  "it will start on the next boot instead\n",
                  what, (unsigned)(want / 1024), (unsigned)(have / 1024));
}

static void link_stop(void)
{
    // Cleared before anything is torn down, so the mesh task cannot be part way
    // through a frame on a link that is being taken apart underneath it.
    BaseSerialInterface *going = active_link;
    active_link = NULL;

    if(going) going->disable();

    if(wifi_started) {
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_OFF);
        wifi_started = false;
    }

    link_mode = MESH_LINK_OFF;
    link_address[0] = '\0';
}

static void link_start_ble(void)
{
    if(ble_link == NULL) ble_link = new SerialBLEInterface();

    if(!ble_started) {
        /* The Bluetooth stack wants tens of kilobytes of internal RAM, and the
         * display's draw buffer and the modem have already taken a large bite
         * of it. Checked rather than attempted, because the stack aborts on its
         * way up rather than returning a failure. */
        size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        Serial.printf("[LINK] %u bytes of internal RAM free before bluetooth starts\n",
                      (unsigned)internal_free);

        if(internal_free < BLE_MIN_INTERNAL_FREE) {
            link_no_room("bluetooth", internal_free, BLE_MIN_INTERNAL_FREE);
            return;
        }

        char name[MESH_NET_NAME_LEN];
        mesh_net_get_self_name(name, sizeof(name));

        /* begin() may rewrite the name, so it gets its own buffer of the size
         * that call documents rather than the node name's. */
        char ble_name[48];
        snprintf(ble_name, sizeof(ble_name), "%s", name[0] ? name : "@@MAC");

        ble_link->begin("MeshCore-", ble_name, ble_pin);
        ble_started = true;
    }

    ble_link->enable();
    link_mode     = MESH_LINK_BLE;
    link_up_since = millis();
    active_link   = ble_link;

    char name[MESH_NET_NAME_LEN];
    mesh_net_get_self_name(name, sizeof(name));
    snprintf(link_address, sizeof(link_address), "MeshCore-%s", name);
    link_set_detail("");

    Serial.printf("[LINK] bluetooth up as \"%s\", pin %06u\n", link_address, (unsigned)ble_pin);
}

static void link_start_wifi(void)
{
    if(udp_relay_is_on()) {
        // Both want the radio in access point mode, and it only has one.
        link_set_detail("turn the hotspot off first");
        link_mode = MESH_LINK_OFF;
        return;
    }

    if(wifi_link == NULL) wifi_link = new SerialWifiInterface();

    bool   open_network  = strlen(ap_pass) < 8;
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    Serial.printf("[LINK] %u bytes of internal RAM free before the wifi radio starts\n",
                  (unsigned)internal_free);

    if(internal_free < WIFI_MIN_INTERNAL_FREE) {
        link_no_room("the wifi radio", internal_free, WIFI_MIN_INTERNAL_FREE);
        return;
    }

    WiFi.mode(WIFI_AP);
    if(!WiFi.softAP(ap_ssid, open_network ? NULL : ap_pass)) {
        link_set_detail("could not start the access point");
        link_mode = MESH_LINK_OFF;
        return;
    }
    wifi_started = true;

    wifi_link->begin(MESH_LINK_PORT);
    wifi_link->enable();
    link_mode     = MESH_LINK_WIFI;
    link_up_since = millis();
    active_link   = wifi_link;

    snprintf(link_address, sizeof(link_address), "%s:%d",
             WiFi.softAPIP().toString().c_str(), MESH_LINK_PORT);
    link_set_detail("");

    Serial.printf("[LINK] wifi up, join \"%s\"%s then connect to %s\n",
                  ap_ssid, open_network ? " (open)" : "", link_address);
}

static void link_apply(void)
{
    if(link_wanted == link_mode) return;

    link_stop();

    if(link_wanted == MESH_LINK_OFF) {
        Serial.println("[LINK] off");
        if(link_latched) {
            latch_clear();
            link_latched = false;
        }
        return;
    }

    // Written down before the attempt, so that a boot which does not come back
    // leaves evidence of what was being tried. Rubbed out by companion_service()
    // once the link has been up long enough to call it good.
    latch_set(link_wanted);
    link_latched = true;

    if(link_wanted == MESH_LINK_BLE) link_start_ble();
    else                             link_start_wifi();

    if(link_mode == MESH_LINK_OFF) {
        // Refused rather than crashed, so there is nothing to warn about later.
        latch_clear();
        link_latched = false;
    }
}

/* Bringing either radio up is slow and goes far deeper into the stack than the
 * mesh task has to give - BLEDevice::init() alone is more than its eight
 * kilobytes - and it must not stall the radio while it happens. So a change of
 * link gets a task of its own, with room to work, which exists for the length
 * of the change and then goes away.
 *
 * This runs at boot as well as on a change: the link is a remembered setting
 * and comes back on its own, which is also when there is the most internal
 * memory free for it. */
static void link_task(void *param)
{
    (void)param;

    link_apply();

    link_changing = false;
    link_task_h   = NULL;
    vTaskDelete(NULL);
}

static void link_request(void)
{
    if(link_changing) return;

    link_changing = true;
    if(xTaskCreate(link_task, "link", 1024 * 12, NULL, 4, &link_task_h) != pdPASS) {
        link_changing = false;
        link_set_detail("no room to start the link");
    }
}

//************************************[ commands ]******************************
static void handle_frame(int len)
{
    uint8_t cmd = cmd_frame[0];

    frames_rx++;
    cmd_frame[len] = 0;   // so a trailing text field can be read as a string

    if(chat_mesh == NULL) {
        write_err(ERR_CODE_BAD_STATE);
        return;
    }

    if(cmd == CMD_DEVICE_QUERY && len >= 2) {
        app_target_ver = cmd_frame[1];

        int i = 0;
        out_frame[i++] = RESP_CODE_DEVICE_INFO;
        out_frame[i++] = FIRMWARE_VER_CODE;
        out_frame[i++] = MAX_CONTACTS / 2;
        out_frame[i++] = MAX_GROUP_CHANNELS;
        memcpy(&out_frame[i], &ble_pin, 4);  i += 4;
        memset(&out_frame[i], 0, 12);
        strncpy((char *)&out_frame[i], FIRMWARE_BUILD_DATE, 12);   i += 12;
        StrHelper::strzcpy((char *)&out_frame[i], "LilyGo T-Deck-Pro", 40);  i += 40;
        StrHelper::strzcpy((char *)&out_frame[i], FIRMWARE_VERSION, 20);     i += 20;
        write_frame(out_frame, i);

    } else if(cmd == CMD_APP_START && len >= 8) {
        // cmd_frame[1..7] are reserved; the app's name follows.
        Serial.printf("[LINK] app \"%s\" connected\n", (const char *)&cmd_frame[8]);
        iter_running = false;   // abandon any listing the last app left running

        mesh_radio_t radio;
        mesh_net_get_radio(&radio);

        char name[MESH_NET_NAME_LEN];
        mesh_net_get_self_name(name, sizeof(name));

        int32_t  lat = 0, lon = 0;   // this node does not put its location in adverts
        uint32_t freq_khz = (uint32_t)(radio.freq_mhz * 1000.0f);
        uint32_t bw_hz    = (uint32_t)(radio.bandwidth_khz * 1000.0f);

        int i = 0;
        out_frame[i++] = RESP_CODE_SELF_INFO;
        out_frame[i++] = ADV_TYPE_CHAT;
        out_frame[i++] = (uint8_t)mesh_net_get_tx_power();
        out_frame[i++] = MESH_TX_POWER_MAX;
        memcpy(&out_frame[i], chat_mesh->self_id.pub_key, PUB_KEY_SIZE);  i += PUB_KEY_SIZE;
        memcpy(&out_frame[i], &lat, 4);  i += 4;
        memcpy(&out_frame[i], &lon, 4);  i += 4;
        out_frame[i++] = 0;   // multi_acks
        out_frame[i++] = 0;   // advert location policy: never
        out_frame[i++] = 0;   // telemetry modes
        out_frame[i++] = 0;   // contacts are added from adverts, not by hand
        memcpy(&out_frame[i], &freq_khz, 4);  i += 4;
        memcpy(&out_frame[i], &bw_hz, 4);     i += 4;
        out_frame[i++] = radio.spreading_factor;
        out_frame[i++] = radio.coding_rate;

        int nlen = strlen(name);
        if(i + nlen > MAX_FRAME_SIZE) nlen = MAX_FRAME_SIZE - i;
        memcpy(&out_frame[i], name, nlen);  i += nlen;
        write_frame(out_frame, i);

    } else if(cmd == CMD_SEND_TXT_MSG && len >= 14) {
        int i = 1;
        uint8_t  txt_type = cmd_frame[i++];
        uint8_t  attempt  = cmd_frame[i++];
        uint32_t timestamp;
        memcpy(&timestamp, &cmd_frame[i], 4);  i += 4;

        ContactInfo *to = chat_mesh->lookupContactByPubKey(&cmd_frame[i], 6);
        i += 6;

        if(to == NULL) {
            write_err(ERR_CODE_NOT_FOUND);
        } else if(txt_type != TXT_TYPE_PLAIN && txt_type != TXT_TYPE_CLI_DATA) {
            write_err(ERR_CODE_UNSUPPORTED_CMD);
        } else {
            const char *text = (const char *)&cmd_frame[i];
            uint32_t    ack = 0, est_timeout = 0;
            int         rc;

            if(txt_type == TXT_TYPE_CLI_DATA) {
                /* The node's own clock rather than the app's, or replay
                 * protection at the far end may throw the command away. */
                timestamp = chat_mesh->getRTCClock()->getCurrentTimeUnique();
                rc = chat_mesh->sendCommandData(*to, timestamp, attempt, text, est_timeout);
            } else {
                rc = chat_mesh->sendMessage(*to, timestamp, attempt, text, ack, est_timeout);
            }

            if(rc == MSG_SEND_FAILED) {
                write_err(ERR_CODE_TABLE_FULL);
            } else {
                if(ack) {
                    ack_table[next_ack_idx].ack     = ack;
                    ack_table[next_ack_idx].sent_ms = millis();
                    ack_table[next_ack_idx].contact = to;
                    next_ack_idx = (next_ack_idx + 1) % ACK_TABLE_SIZE;
                }

                out_frame[0] = RESP_CODE_SENT;
                out_frame[1] = (rc == MSG_SEND_SENT_FLOOD) ? 1 : 0;
                memcpy(&out_frame[2], &ack, 4);
                memcpy(&out_frame[6], &est_timeout, 4);
                write_frame(out_frame, 10);
            }
        }

    } else if(cmd == CMD_SEND_CHANNEL_TXT_MSG && len >= 7) {
        int i = 1;
        uint8_t txt_type    = cmd_frame[i++];
        uint8_t channel_idx = cmd_frame[i++];
        uint32_t timestamp;
        memcpy(&timestamp, &cmd_frame[i], 4);  i += 4;

        ChannelDetails channel;
        char name[MESH_NET_NAME_LEN];
        mesh_net_get_self_name(name, sizeof(name));

        if(txt_type != TXT_TYPE_PLAIN) {
            write_err(ERR_CODE_UNSUPPORTED_CMD);
        } else if(chat_mesh->getChannel(channel_idx, channel) &&
                  chat_mesh->sendGroupMessage(timestamp, channel.channel, name,
                                              (const char *)&cmd_frame[i], len - i)) {
            write_ok();
        } else {
            write_err(ERR_CODE_NOT_FOUND);
        }

    } else if(cmd == CMD_GET_CONTACTS) {
        if(iter_running) {
            write_err(ERR_CODE_BAD_STATE);
        } else {
            iter_since = 0;
            if(len >= 5) memcpy(&iter_since, &cmd_frame[1], 4);

            uint8_t  reply[5];
            uint32_t count = chat_mesh->getNumContacts();
            reply[0] = RESP_CODE_CONTACTS_START;
            memcpy(&reply[1], &count, 4);
            write_frame(reply, 5);

            contacts_iter       = chat_mesh->startContactsIterator();
            iter_running        = true;
            iter_newest_lastmod = 0;
        }

    } else if(cmd == CMD_GET_CONTACT_BY_KEY && len >= 1 + PUB_KEY_SIZE) {
        ContactInfo *c = chat_mesh->lookupContactByPubKey(&cmd_frame[1], PUB_KEY_SIZE);
        if(c) write_contact(RESP_CODE_CONTACT, *c);
        else  write_err(ERR_CODE_NOT_FOUND);

    } else if(cmd == CMD_ADD_UPDATE_CONTACT && len >= 1 + PUB_KEY_SIZE + 3) {
        ContactInfo *existing = chat_mesh->lookupContactByPubKey(&cmd_frame[1], PUB_KEY_SIZE);
        uint32_t last_mod = chat_mesh->getRTCClock()->getCurrentTime();

        if(existing) {
            read_contact(*existing, last_mod, cmd_frame, len);
            existing->lastmod = last_mod;
            write_ok();
        } else {
            ContactInfo c;
            memset(&c, 0, sizeof(c));
            read_contact(c, last_mod, cmd_frame, len);
            c.lastmod    = last_mod;
            c.sync_since = 0;

            if(chat_mesh->addContact(c)) write_ok();
            else                         write_err(ERR_CODE_TABLE_FULL);
        }

    } else if(cmd == CMD_REMOVE_CONTACT && len >= 1 + PUB_KEY_SIZE) {
        ContactInfo *c = chat_mesh->lookupContactByPubKey(&cmd_frame[1], PUB_KEY_SIZE);
        if(c && chat_mesh->removeContact(*c)) write_ok();
        else                                  write_err(ERR_CODE_NOT_FOUND);

    } else if(cmd == CMD_RESET_PATH && len >= 1 + PUB_KEY_SIZE) {
        ContactInfo *c = chat_mesh->lookupContactByPubKey(&cmd_frame[1], PUB_KEY_SIZE);
        if(c) {
            // Back to flooding, which is how a node is reached when the route
            // it used to answer on has stopped working.
            c->out_path_len = OUT_PATH_UNKNOWN;
            write_ok();
        } else {
            write_err(ERR_CODE_NOT_FOUND);
        }

    } else if(cmd == CMD_SHARE_CONTACT && len >= 1 + PUB_KEY_SIZE) {
        ContactInfo *c = chat_mesh->lookupContactByPubKey(&cmd_frame[1], PUB_KEY_SIZE);
        if(c == NULL)                            write_err(ERR_CODE_NOT_FOUND);
        else if(chat_mesh->shareContactZeroHop(*c)) write_ok();
        else                                     write_err(ERR_CODE_TABLE_FULL);

    } else if(cmd == CMD_EXPORT_CONTACT) {
        uint8_t out_len = 0;

        if(len < 1 + PUB_KEY_SIZE) {   // export this node itself
            char name[MESH_NET_NAME_LEN];
            mesh_net_get_self_name(name, sizeof(name));

            mesh::Packet *pkt = chat_mesh->createSelfAdvert(name);
            if(pkt) {
                pkt->header |= ROUTE_TYPE_FLOOD;   // how it would normally travel
                out_frame[0] = RESP_CODE_EXPORT_CONTACT;
                out_len = pkt->writeTo(&out_frame[1]);
                chat_mesh->releasePacket(pkt);
            }
        } else {
            ContactInfo *c = chat_mesh->lookupContactByPubKey(&cmd_frame[1], PUB_KEY_SIZE);
            out_frame[0] = RESP_CODE_EXPORT_CONTACT;
            if(c) out_len = chat_mesh->exportContact(*c, &out_frame[1]);
        }

        if(out_len > 0) write_frame(out_frame, out_len + 1);
        else            write_err(ERR_CODE_NOT_FOUND);

    } else if(cmd == CMD_IMPORT_CONTACT && len > 1) {
        if(chat_mesh->importContact(&cmd_frame[1], len - 1)) write_ok();
        else                                                 write_err(ERR_CODE_ILLEGAL_ARG);

    } else if(cmd == CMD_SYNC_NEXT_MESSAGE) {
        int out_len = take_offline(out_frame);
        if(out_len > 0) {
            write_frame(out_frame, out_len);
        } else {
            out_frame[0] = RESP_CODE_NO_MORE_MESSAGES;
            write_frame(out_frame, 1);
        }

    } else if(cmd == CMD_GET_DEVICE_TIME) {
        uint8_t  reply[5];
        uint32_t now = chat_mesh->getRTCClock()->getCurrentTime();
        reply[0] = RESP_CODE_CURR_TIME;
        memcpy(&reply[1], &now, 4);
        write_frame(reply, 5);

    } else if(cmd == CMD_SET_DEVICE_TIME && len >= 5) {
        uint32_t secs;
        memcpy(&secs, &cmd_frame[1], 4);

        // Only ever forwards: the mesh uses timestamps to tell a new message
        // from one it has already seen, and a clock that went backwards would
        // make replayed traffic look new.
        if(secs >= chat_mesh->getRTCClock()->getCurrentTime()) {
            chat_mesh->getRTCClock()->setCurrentTime(secs);
            write_ok();
        } else {
            write_err(ERR_CODE_ILLEGAL_ARG);
        }

    } else if(cmd == CMD_SEND_SELF_ADVERT) {
        mesh_net_advertise();
        write_ok();

    } else if(cmd == CMD_SET_ADVERT_NAME && len >= 2) {
        char name[MESH_NET_NAME_LEN];
        int  nlen = len - 1;
        if(nlen > (int)sizeof(name) - 1) nlen = sizeof(name) - 1;
        memcpy(name, &cmd_frame[1], nlen);
        name[nlen] = 0;

        mesh_net_set_self_name(name);
        write_ok();

    } else if(cmd == CMD_SET_ADVERT_LATLON) {
        // Adverts from this node carry no location, so there is nothing to set.
        write_ok();

    } else if(cmd == CMD_SET_RADIO_PARAMS && len >= 11) {
        uint32_t freq_khz, bw_hz;
        memcpy(&freq_khz, &cmd_frame[1], 4);
        memcpy(&bw_hz, &cmd_frame[5], 4);
        uint8_t sf = cmd_frame[9];
        uint8_t cr = cmd_frame[10];

        /* The same bounds the protocol documents, checked here so that a
         * rejected setting can be reported as such - mesh_net_set_custom()
         * refuses silently, which is right for a button on this device but
         * would leave the app waiting. */
        if(freq_khz < 150000 || freq_khz > 2500000 ||
           bw_hz < 7000 || bw_hz > 500000 ||
           sf < 5 || sf > 12 || cr < 5 || cr > 8) {
            write_err(ERR_CODE_ILLEGAL_ARG);
        } else {
            mesh_net_set_custom((float)freq_khz / 1000.0f, (float)bw_hz / 1000.0f, sf, cr);
            write_ok();
        }

    } else if(cmd == CMD_SET_RADIO_TX_POWER && len >= 2) {
        int8_t power = (int8_t)cmd_frame[1];
        if(power < -9 || power > MESH_TX_POWER_MAX) {
            write_err(ERR_CODE_ILLEGAL_ARG);
        } else {
            mesh_net_set_tx_power(power);
            write_ok();
        }

    } else if(cmd == CMD_GET_TUNING_PARAMS) {
        // The defaults MeshCore ships with; this node does not expose tuning.
        uint32_t rx_delay_base = 0, airtime_factor = 1000;
        int i = 0;
        out_frame[i++] = RESP_CODE_TUNING_PARAMS;
        memcpy(&out_frame[i], &rx_delay_base, 4);   i += 4;
        memcpy(&out_frame[i], &airtime_factor, 4);  i += 4;
        write_frame(out_frame, i);

    } else if(cmd == CMD_SET_TUNING_PARAMS || cmd == CMD_SET_OTHER_PARAMS) {
        // Accepted and ignored: answering with an error here would stop some
        // apps part-way through connecting, over settings this node has no use
        // for.
        write_ok();

    } else if(cmd == CMD_GET_BATT_AND_STORAGE) {
        uint8_t  reply[11];
        uint16_t millivolts = ui_battery_27220_get_voltage();
        uint32_t used  = SPIFFS.usedBytes() / 1024;
        uint32_t total = SPIFFS.totalBytes() / 1024;

        int i = 0;
        reply[i++] = RESP_CODE_BATT_AND_STORAGE;
        memcpy(&reply[i], &millivolts, 2);  i += 2;
        memcpy(&reply[i], &used, 4);        i += 4;
        memcpy(&reply[i], &total, 4);       i += 4;
        write_frame(reply, i);

    } else if(cmd == CMD_GET_CHANNEL && len >= 2) {
        ChannelDetails channel;
        if(chat_mesh->getChannel(cmd_frame[1], channel)) {
            int i = 0;
            out_frame[i++] = RESP_CODE_CHANNEL_INFO;
            out_frame[i++] = cmd_frame[1];
            StrHelper::strzcpy((char *)&out_frame[i], channel.name, 32);  i += 32;
            memcpy(&out_frame[i], channel.channel.secret, 16);            i += 16;
            write_frame(out_frame, i);
        } else {
            write_err(ERR_CODE_NOT_FOUND);
        }

    } else if(cmd == CMD_SET_CHANNEL && len >= 2 + 32 + 16) {
        // 128-bit keys only, which is what MeshCore's own channels use.
        if(len >= 2 + 32 + 32) {
            write_err(ERR_CODE_UNSUPPORTED_CMD);
        } else {
            ChannelDetails channel;
            StrHelper::strncpy(channel.name, (const char *)&cmd_frame[2], 32);
            memset(channel.channel.secret, 0, sizeof(channel.channel.secret));
            memcpy(channel.channel.secret, &cmd_frame[2 + 32], 16);

            if(chat_mesh->setChannel(cmd_frame[1], channel)) write_ok();
            else                                             write_err(ERR_CODE_NOT_FOUND);
        }

    } else if(cmd == CMD_REBOOT && len >= 7 && memcmp(&cmd_frame[1], "reboot", 6) == 0) {
        Serial.println("[LINK] the app asked for a restart");
        esp_restart();

    } else {
        write_err(ERR_CODE_UNSUPPORTED_CMD);
    }
}

//************************************[ the seam ]******************************
void companion_attach(BaseChatMesh *chat)
{
    chat_mesh = chat;

    memset(ack_table, 0, sizeof(ack_table));
    companion_load();

    /* The link itself is not started here. Startup asks first (see
     * mesh_companion_link_saved) so the display knows where to put its buffers,
     * and starts it afterwards with mesh_companion_boot() - by which time what
     * is left of internal memory is what the radio actually gets. */
}

void companion_service(void)
{
    // Nothing may touch the link while it is being taken down or brought up.
    if(link_changing) return;

    if(active_link == NULL) return;

    /* The link has been up long enough to call the attempt good, so the note
     * saying it was being tried can go - otherwise the next boot would think
     * this one had crashed. */
    if(link_latched && millis() - link_up_since > LINK_SETTLE_MS) {
        latch_clear();
        link_latched = false;
    }

    active_link->loop();

    int len = active_link->checkRecvFrame(cmd_frame);
    if(len > 0) {
        handle_frame(len);
        return;
    }

    /* Feeding a contacts listing out one frame at a time, and only while the
     * link is keeping up, so that a long list neither monopolises the mesh task
     * nor overruns the link's own queue. */
    if(iter_running && !active_link->isWriteBusy()) {
        ContactInfo c;

        if(contacts_iter.hasNext(chat_mesh, c)) {
            if(c.lastmod > iter_since) {
                write_contact(RESP_CODE_CONTACT, c);
                if(c.lastmod > iter_newest_lastmod) iter_newest_lastmod = c.lastmod;
            }
        } else {
            out_frame[0] = RESP_CODE_END_OF_CONTACTS;
            // The newest change the app has now seen, so it can ask for only
            // what is newer than this next time.
            memcpy(&out_frame[1], &iter_newest_lastmod, 4);
            write_frame(out_frame, 5);
            iter_running = false;
        }
    }
}

ContactInfo *companion_process_ack(BaseChatMesh *chat, const uint8_t *data)
{
    for(int i = 0; i < ACK_TABLE_SIZE; i++) {
        if(ack_table[i].ack == 0 || memcmp(data, &ack_table[i].ack, 4) != 0) continue;

        uint32_t trip_ms = millis() - ack_table[i].sent_ms;

        out_frame[0] = PUSH_CODE_SEND_CONFIRMED;
        memcpy(&out_frame[1], data, 4);
        memcpy(&out_frame[5], &trip_ms, 4);
        write_frame(out_frame, 9);

        // The same acknowledgement can arrive more than once.
        ack_table[i].ack = 0;
        return ack_table[i].contact;
    }
    return NULL;
}

void companion_on_advert(const ContactInfo &contact, bool is_new)
{
    if(!link_up()) return;

    if(is_new) {
        // Somebody the app has never seen, so it gets the whole contact.
        write_contact(PUSH_CODE_NEW_ADVERT, contact);
    } else {
        out_frame[0] = PUSH_CODE_ADVERT;
        memcpy(&out_frame[1], contact.id.pub_key, PUB_KEY_SIZE);
        write_frame(out_frame, 1 + PUB_KEY_SIZE);
    }
}

void companion_on_path_updated(const ContactInfo &contact)
{
    if(!link_up()) return;

    out_frame[0] = PUSH_CODE_PATH_UPDATED;
    memcpy(&out_frame[1], contact.id.pub_key, PUB_KEY_SIZE);
    write_frame(out_frame, 1 + PUB_KEY_SIZE);
}

void companion_queue_msg(const ContactInfo &from, mesh::Packet *pkt,
                         uint32_t sender_timestamp, uint8_t txt_type, const char *text)
{
    int i = 0;

    if(app_target_ver >= 3) {
        out_frame[i++] = RESP_CODE_CONTACT_MSG_RECV_V3;
        out_frame[i++] = (int8_t)(pkt->getSNR() * 4);
        out_frame[i++] = 0;
        out_frame[i++] = 0;
    } else {
        out_frame[i++] = RESP_CODE_CONTACT_MSG_RECV;
    }

    memcpy(&out_frame[i], from.id.pub_key, 6);  i += 6;   // the prefix is enough
    out_frame[i++] = pkt->isRouteFlood() ? pkt->path_len : 0xFF;
    out_frame[i++] = txt_type;
    memcpy(&out_frame[i], &sender_timestamp, 4);  i += 4;

    int tlen = strlen(text);
    if(i + tlen > MAX_FRAME_SIZE) tlen = MAX_FRAME_SIZE - i;
    memcpy(&out_frame[i], text, tlen);  i += tlen;

    queue_offline(out_frame, i);
}

void companion_queue_channel_msg(int channel_idx, mesh::Packet *pkt,
                                 uint32_t timestamp, const char *text)
{
    int i = 0;

    if(app_target_ver >= 3) {
        out_frame[i++] = RESP_CODE_CHANNEL_MSG_RECV_V3;
        out_frame[i++] = (int8_t)(pkt->getSNR() * 4);
        out_frame[i++] = 0;
        out_frame[i++] = 0;
    } else {
        out_frame[i++] = RESP_CODE_CHANNEL_MSG_RECV;
    }

    out_frame[i++] = (uint8_t)channel_idx;
    out_frame[i++] = pkt->isRouteFlood() ? pkt->path_len : 0xFF;
    out_frame[i++] = TXT_TYPE_PLAIN;
    memcpy(&out_frame[i], &timestamp, 4);  i += 4;

    int tlen = strlen(text);
    if(i + tlen > MAX_FRAME_SIZE) tlen = MAX_FRAME_SIZE - i;
    memcpy(&out_frame[i], text, tlen);  i += tlen;

    queue_offline(out_frame, i);
}

//************************************[ public API ]****************************
bool mesh_companion_link_saved(void)
{
    // No node to drive means no reason to spend the memory on a link.
    return chat_mesh != NULL && link_wanted != MESH_LINK_OFF;
}

void mesh_companion_boot(void)
{
    if(!mesh_companion_link_saved()) return;

    link_request();
}

int mesh_companion_get_link(void)
{
    return link_wanted;
}

void mesh_companion_set_link(int want)
{
    if(want < MESH_LINK_OFF || want > MESH_LINK_WIFI) return;

    link_wanted = want;
    companion_save();

    /* Starting either radio blocks for a while and wants far more stack than
     * the UI task can spare, so the change is handed to a task of its own
     * rather than done here. */
    link_set_detail("starting");
    link_request();
}

bool mesh_companion_is_connected(void)
{
    return link_up();
}

void mesh_companion_get_detail(char *buf, int len)
{
    if(buf && len > 0) snprintf(buf, len, "%s", link_detail);
}

uint32_t mesh_companion_ble_pin(void)
{
    return ble_pin;
}

void mesh_companion_ble_name(char *buf, int len)
{
    if(buf == NULL || len <= 0) return;

    char name[MESH_NET_NAME_LEN];
    mesh_net_get_self_name(name, sizeof(name));
    snprintf(buf, len, "MeshCore-%s", name);
}

void mesh_companion_get_wifi(char *ssid, int ssid_len, char *pass, int pass_len)
{
    if(ssid && ssid_len > 0) snprintf(ssid, ssid_len, "%s", ap_ssid);
    if(pass && pass_len > 0) snprintf(pass, pass_len, "%s", ap_pass);
}

void mesh_companion_set_wifi(const char *ssid, const char *pass)
{
    if(ssid && ssid[0]) snprintf(ap_ssid, sizeof(ap_ssid), "%s", ssid);
    if(pass)            snprintf(ap_pass, sizeof(ap_pass), "%s", pass);

    companion_save();

    /* A running access point keeps the name it started with, so it is put back
     * up under the new one. */
    if(link_mode == MESH_LINK_WIFI) {
        link_mode = MESH_LINK_OFF;   // so link_apply() sees a change to make
        link_request();
    }
}

void mesh_companion_get_address(char *buf, int len)
{
    if(buf && len > 0) snprintf(buf, len, "%s", link_address);
}

uint32_t mesh_companion_frames_rx(void) { return frames_rx; }
uint32_t mesh_companion_frames_tx(void) { return frames_tx; }
int      mesh_companion_queued(void)    { return offline_queue_len; }
