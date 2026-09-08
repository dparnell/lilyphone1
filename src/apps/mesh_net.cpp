/**
 * MeshCore on the T-Deck-Pro.
 *
 * This owns the SX1262, runs a MeshCore mesh, announces this node, records the
 * nodes it hears, and carries text between them - both privately to one node
 * and on the public channel that every MeshCore node shares.
 *
 * MeshCore itself is not thread-safe and the radio has to be serviced promptly,
 * so everything that touches the mesh happens on the mesh task. The UI never
 * calls into it: it reads the mirrored node and message tables under a mutex,
 * and asks for a send by leaving a note the task picks up.
 *
 * MeshCore expects to own the radio, which is why the vendor's LoRa demo went:
 * two drivers cannot both drive an SX1262.
 *
 * The mesh runs on its own task. It has to be serviced promptly - a missed
 * receive window loses a packet - and the LVGL task blocks for hundreds of
 * milliseconds at a time pushing frames to the e-paper, so it cannot be run
 * from a UI timer. SPI is shared with the display and the SD card; every device
 * has its own chip select and RadioLib takes the bus per transaction, which is
 * what makes that safe.
 */
#include <Arduino.h>
#include <Preferences.h>
#include <SPIFFS.h>
#include <RadioLib.h>

#include <MeshCore.h>
#include <helpers/ArduinoHelpers.h>
#include <helpers/StaticPoolPacketManager.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/IdentityStore.h>
#include <helpers/AdvertDataHelpers.h>
#include <helpers/BaseChatMesh.h>
#include <helpers/radiolib/CustomSX1262Wrapper.h>

#include "mesh_net.h"
#include "mesh_companion_int.h"
#include "peripheral.h"
#include "utilities.h"

/* Declared here rather than by including the UI port header, which would drag
 * LVGL in for the sake of one number. */
extern "C" uint16_t ui_battery_27220_get_voltage(void);

#define MESH_PREFS_NAMESPACE "mesh"

/* The public MeshCore settings. Anyone this phone is meant to talk to has to be
 * using the same ones, so these are the values the MeshCore community publishes
 * rather than anything chosen here.
 *
 * Note that some local meshes have since moved to "narrow" settings - 62.5kHz
 * bandwidth with a lower spreading factor - so if nothing is ever heard on the
 * right frequency, that is the next thing to check against whoever else is out
 * there. */
static int8_t tx_power_dbm  = MESH_TX_POWER_MAX;
static bool   tx_power_pending = false;

/* Off unless asked for: an advert floods the mesh, so a position in one travels
 * further than the radio reaches and is readable by anyone running MeshCore. */
static int    loc_policy = MESH_LOC_OFF;

/* A position given rather than measured. What a node that lives on a shelf
 * needs in order to appear on a map: its GPS may never see the sky, and a fix
 * it cannot get is not a location it can share. Zero means none was given. */
static double fixed_lat = 0.0, fixed_lon = 0.0;
static bool   pos_from_fixed = false;

/* MeshCore's own tuning, in the units its virtuals want. These defaults are
 * what the library does when nothing overrides them, so a node that has never
 * been tuned behaves exactly as before. */
static float   rx_delay_base   = 10.0f;
static float   airtime_factor  = 1.0f;
static uint8_t multi_acks      = 0;
static bool    manual_contacts = false;

/* The presets. The wide ones are what the MeshCore community publishes per
 * region; the narrow one is what the mesh in Victoria actually runs, and the
 * difference is not only the frequency - bandwidth, spreading factor and coding
 * rate all differ, and all four have to match to hear anything. Several meshes
 * have moved to narrow settings like these. */
static const mesh_radio_t mesh_presets[] = {
    { "Victoria AU", 916.575f, 62.5f,  7, 8 },
    { "Aus / NZ",    915.800f, 250.0f, 10, 5 },
    { "EU / UK",     869.525f, 250.0f, 10, 5 },
    { "US / Canada", 910.525f, 250.0f, 10, 5 },
};

#define MESH_PRESET_COUNT  ((int)(sizeof(mesh_presets) / sizeof(mesh_presets[0])))
// One past the presets is the custom entry.
#define MESH_REGION_CUSTOM MESH_PRESET_COUNT
#define MESH_REGION_COUNT  (MESH_PRESET_COUNT + 1)

static int  region_idx     = 0;   // Victoria, which is where this phone lives
static bool region_pending = false;

/* Only meaningful while the custom entry is selected, but kept regardless so
 * that stepping away from it and back does not lose what was typed. */
static mesh_radio_t custom_radio = { "Custom", 916.575f, 62.5f, 7, 8 };

// How often this node announces itself unprompted.
#define MESH_ADVERT_PERIOD_MS (10 * 60 * 1000)

//************************************[ board ]*********************************
/* The little MeshCore needs to know about the hardware. Most of MainBoard is
 * optional; this fills in the parts that have a real answer on this board and
 * leaves the rest at their defaults. */
class TDeckProBoard : public mesh::MainBoard {
    uint8_t startup_reason = 0;

public:
    const char *getManufacturerName() const override { return "LilyGo T-Deck-Pro"; }

    uint16_t getBattMilliVolts() override {
        // The fuel gauge owns this; the mesh only uses it for telemetry.
        return ui_battery_27220_get_voltage();
    }

    uint8_t getStartupReason() const override { return startup_reason; }

    void reboot() override { esp_restart(); }

    uint32_t getIRQGpio() override { return BOARD_LORA_INT; }
};

//************************************[ the mesh ]******************************
static TDeckProBoard      board;
static ArduinoMillis      ms_clock;
static VolatileRTCClock   rtc_clock;
static StdRNG             rng;
static SimpleMeshTables   tables;

static SPIClass          &radio_spi = SPI;
static CustomSX1262       radio = new Module(BOARD_LORA_CS, BOARD_LORA_INT,
                                             BOARD_LORA_RST, BOARD_LORA_BUSY, radio_spi);
static CustomSX1262Wrapper radio_driver(radio, board);

static char     self_name[MESH_NET_NAME_LEN] = "lilyphone1";
static uint32_t packets_rx = 0;
static uint32_t packets_tx = 0;

/* Heard nodes, newest first. A small fixed table: this is a phone, not a
 * repeater, and a list longer than the screen is of no use to anyone. */
static mesh_node_t nodes[MESH_NET_NODES_MAX];
static int         node_count = 0;
static SemaphoreHandle_t node_lock = NULL;

/* Bumped whenever anything the UI draws has changed. Screens on this display
 * cost hundreds of milliseconds to redraw, so they compare this rather than
 * rebuilding on a timer. */
static uint32_t chat_rev = 0;

static void node_record(const mesh::Identity &id, const char *name, int hops,
                        bool has_path, float snr, float rssi)
{
    if(node_lock == NULL) return;
    xSemaphoreTake(node_lock, portMAX_DELAY);

    // Already known? Refresh it in place and move it to the front.
    int found = -1;
    for(int i = 0; i < node_count; i++) {
        if(memcmp(nodes[i].pubkey_prefix, id.pub_key, sizeof(nodes[i].pubkey_prefix)) == 0) {
            found = i;
            break;
        }
    }

    mesh_node_t entry;
    memset(&entry, 0, sizeof(entry));
    memcpy(entry.pubkey_prefix, id.pub_key, sizeof(entry.pubkey_prefix));
    snprintf(entry.name, sizeof(entry.name), "%s", (name && name[0]) ? name : "(unnamed)");
    entry.snr      = (int8_t)snr;
    entry.rssi     = (int16_t)rssi;
    entry.hops     = (uint8_t)hops;
    entry.has_path = has_path ? 1 : 0;
    entry.heard_ms = millis();

    if(found < 0) {
        if(node_count < MESH_NET_NODES_MAX) node_count++;
        found = node_count - 1;
    }

    // Shuffle down so the most recently heard is always first.
    for(int i = found; i > 0; i--) nodes[i] = nodes[i - 1];
    nodes[0] = entry;

    chat_rev++;
    xSemaphoreGive(node_lock);

    Serial.printf("[MESH] heard %s (%02X%02X%02X%02X) snr %d hops %d\n",
                  entry.name, entry.pubkey_prefix[0], entry.pubkey_prefix[1],
                  entry.pubkey_prefix[2], entry.pubkey_prefix[3], entry.snr, hops);
}

//************************************[ the message log ]***********************
/* MeshCore keeps contacts and routes; what was actually said is the
 * application's business, so it is kept here. In PSRAM, because at this length
 * it is more than internal memory should be asked to give up, and only in
 * memory - a mesh conversation is a conversation in the moment rather than a
 * record to keep, and writing every message through SPIFFS would put the
 * filesystem in the path of the radio task.
 *
 * Guarded by node_lock, which covers both tables: the UI reads them from the
 * LVGL task while the mesh task is writing them. */
static mesh_msg_t *msg_log   = NULL;
static int         msg_count = 0;

/* Whether a stored message belongs to a conversation. NULL means the public
 * channel, which is not anybody's conversation in particular. */
static bool msg_matches(const mesh_msg_t *m, const uint8_t peer[4])
{
    if(peer == NULL)  return m->is_channel != 0;
    if(m->is_channel) return false;
    return memcmp(m->peer, peer, 4) == 0;
}

static void msg_add(const uint8_t peer[4], const char *text, bool outgoing,
                    uint8_t status, bool unread, bool is_channel)
{
    if(msg_log == NULL || node_lock == NULL) return;

    xSemaphoreTake(node_lock, portMAX_DELAY);

    if(msg_count >= MESH_MSG_MAX) {
        // Drop the oldest so the log stays bounded.
        memmove(&msg_log[0], &msg_log[1], sizeof(mesh_msg_t) * (MESH_MSG_MAX - 1));
        msg_count = MESH_MSG_MAX - 1;
    }

    mesh_msg_t *m = &msg_log[msg_count++];
    memset(m, 0, sizeof(*m));

    if(peer) memcpy(m->peer, peer, sizeof(m->peer));
    snprintf(m->text, sizeof(m->text), "%s", text ? text : "");
    m->at_ms      = millis();
    m->outgoing   = outgoing ? 1 : 0;
    m->status     = status;
    m->unread     = unread ? 1 : 0;
    m->is_channel = is_channel ? 1 : 0;

    chat_rev++;
    xSemaphoreGive(node_lock);
}

/* Records the outcome of the message that is still in flight. MeshCore
 * acknowledges by a hash of the packet rather than by message id, so the one
 * being answered is simply the newest one still waiting - and only one is ever
 * in flight, because this is a phone with one person typing. */
static void msg_settle(uint8_t status)
{
    if(msg_log == NULL || node_lock == NULL) return;

    xSemaphoreTake(node_lock, portMAX_DELAY);
    for(int i = msg_count - 1; i >= 0; i--) {
        if(!msg_log[i].outgoing || msg_log[i].status != MESH_ST_SENDING) continue;

        msg_log[i].status = status;
        chat_rev++;
        break;
    }
    xSemaphoreGive(node_lock);
}

//************************************[ the node ]******************************
class PhoneMesh : public BaseChatMesh {
    uint32_t pending_ack  = 0;
    uint8_t  pending_key[4] = { 0 };

public:
    PhoneMesh(mesh::Radio &r, mesh::MillisecondClock &m, mesh::RNG &rn,
              mesh::RTCClock &rt, mesh::PacketManager &mgr, mesh::MeshTables &t)
        : BaseChatMesh(r, m, rn, rt, mgr, t) { }

    /* A phone in a pocket is a poor repeater and forwarding costs battery, so
     * this node listens and speaks for itself but does not relay. */
    bool allowPacketForward(const mesh::Packet *packet) override { return false; }

    /* The tuning an app can set. Overriding these is what makes those commands
     * mean something - without them the settings would be stored, reported back
     * and never consulted, which is worse than refusing them. */
    float getAirtimeBudgetFactor() const override { return airtime_factor; }

    int calcRxDelay(float score, uint32_t air_time) const override {
        if(rx_delay_base <= 0.0f) return 0;   // zero means no back-off at all
        return (int)((pow(rx_delay_base, 0.85f - score) - 1.0) * air_time);
    }

    uint8_t getExtraAckTransmitCount() const override { return multi_acks; }

    /* Off means the app curates the contact list itself, and adverts heard on
     * the air are reported but not kept. */
    bool isAutoAddEnabled() const override { return !manual_contacts; }

    /* Somebody announced themselves. BaseChatMesh has already added them as a
     * contact by the time this is called, which is what makes the node list and
     * the list of people to talk to the same list. */
    void onDiscoveredContact(ContactInfo &contact, bool is_new, uint8_t path_len,
                             const uint8_t *path) override {
        node_record(contact.id, contact.name, path_len,
                    contact.out_path_len != OUT_PATH_UNKNOWN,
                    _radio->getLastSNR(), _radio->getLastRSSI());
        packets_rx++;

        companion_on_advert(contact, is_new);
        companion_note_advert_path(contact, path_len, path);
    }

    void onContactPathUpdated(const ContactInfo &contact) override {
        node_record(contact.id, contact.name, contact.out_path_len,
                    contact.out_path_len != OUT_PATH_UNKNOWN,
                    _radio->getLastSNR(), _radio->getLastRSSI());

        companion_on_path_updated(contact);
    }

    /* An acknowledgement answers whoever sent the message - the app, or this
     * device's own screen. The app's outstanding sends are checked first
     * because there may be several of them, where there is only ever one of
     * ours. */
    ContactInfo *processAck(const uint8_t *data) override {
        packets_rx++;

        ContactInfo *from = companion_process_ack(this, data);
        if(from) return from;

        if(pending_ack == 0 || memcmp(data, &pending_ack, 4) != 0) return NULL;

        msg_settle(MESH_ST_DELIVERED);
        pending_ack = 0;

        return lookupContactByPubKey(pending_key, sizeof(pending_key));
    }

    void onMessageRecv(const ContactInfo &contact, mesh::Packet *pkt,
                       uint32_t sender_timestamp, const char *text) override {
        msg_add(contact.id.pub_key, text, false, MESH_ST_OK, true, false);
        packets_rx++;

        // Delivered to both: this device shows it, and the app can collect it.
        companion_queue_msg(contact, pkt, sender_timestamp, TXT_TYPE_PLAIN, text);

        Serial.printf("[MESH] %s: %s\n", contact.name, text);
    }

    void onSignedMessageRecv(const ContactInfo &contact, mesh::Packet *pkt,
                             uint32_t sender_timestamp, const uint8_t *sender_prefix,
                             const char *text) override {
        onMessageRecv(contact, pkt, sender_timestamp, text);
    }

    void onChannelMessageRecv(const mesh::GroupChannel &channel, mesh::Packet *pkt,
                              uint32_t timestamp, const char *text) override {
        // MeshCore puts "<sender>: " on the front of channel text already.
        msg_add(NULL, text, false, MESH_ST_OK, true, true);
        packets_rx++;

        companion_queue_channel_msg(findChannelIdx(channel), pkt, timestamp, text);

        Serial.printf("[MESH] channel: %s\n", text);
    }

    /* Commands are for repeaters and room servers to act on. This device has no
     * answer for one and does not show it as something somebody said, but a
     * companion app driving a repeater is exactly who asked for it, so it is
     * passed along rather than dropped. */
    void onCommandDataRecv(const ContactInfo &contact, mesh::Packet *pkt,
                           uint32_t sender_timestamp, const char *text) override {
        companion_queue_msg(contact, pkt, sender_timestamp, TXT_TYPE_CLI_DATA, text);
    }

    /* Nothing is served from here, so a request gets an empty reply. */
    uint8_t onContactRequest(const ContactInfo &contact, uint32_t sender_timestamp,
                             const uint8_t *data, uint8_t len, uint8_t *reply) override {
        return 0;
    }

    void onContactResponse(const ContactInfo &contact, const uint8_t *data,
                           uint8_t len) override { }

    /* How long to wait for an acknowledgement before calling it lost. A flooded
     * message has to cross the mesh and be answered back across it, so it gets
     * considerably longer than one sent down a route that is already known. */
    uint32_t calcFloodTimeoutMillisFor(uint32_t pkt_airtime_millis) const override {
        return 12000 + (pkt_airtime_millis * 12);
    }

    uint32_t calcDirectTimeoutMillisFor(uint32_t pkt_airtime_millis,
                                        uint8_t path_len) const override {
        return 6000 + ((pkt_airtime_millis * 2) * (path_len + 2));
    }

    /* BaseChatMesh keeps one timeout for the last message sent, whoever sent
     * it, so only settle the log when the outstanding send is this device's -
     * otherwise a message the app sent would mark one of ours as lost. The app
     * times its own sends out, which is what the estimate in the reply to
     * CMD_SEND_TXT_MSG is for. */
    void onSendTimeout() override {
        if(pending_ack == 0) return;

        msg_settle(MESH_ST_FAILED);
        pending_ack = 0;
        Serial.println("[MESH] no acknowledgement, giving up");
    }

    /* Remembers what to match the next acknowledgement against. */
    void expectAck(uint32_t ack, const uint8_t *pub_key) {
        pending_ack = ack;
        memcpy(pending_key, pub_key, sizeof(pending_key));
    }
};

static mesh::PacketManager *packet_mgr = NULL;
static PhoneMesh           *the_mesh   = NULL;
static TaskHandle_t         mesh_task_h = NULL;
static bool                 mesh_running = false;

//************************************[ the public channel ]********************
/* Every MeshCore node ships with the same key for the public channel, so it is
 * not a secret and is not meant to be one - it is the open conversation on the
 * frequency, readable by anyone in range. Private conversations are the ones
 * addressed to a single node, which are encrypted to that node's key. */
#define MESH_PUBLIC_PSK  "izOH6cXN6mrJ5e26oRXNcg=="
#define MESH_PUBLIC_NAME "Public channel"

static ChannelDetails *public_channel = NULL;

//************************************[ outgoing ]******************************
/* One message waiting for its turn on the radio. The UI leaves it here and the
 * mesh task picks it up, because MeshCore may only be touched from the task
 * that owns it. */
static volatile bool send_pending = false;
static bool          send_channel = false;
static uint8_t       send_peer[4];
static char          send_text[MESH_TEXT_LEN];

static void send_service(void)
{
    if(!send_pending || the_mesh == NULL) return;

    bool    to_channel;
    uint8_t peer[4];
    char    text[MESH_TEXT_LEN];

    xSemaphoreTake(node_lock, portMAX_DELAY);
    to_channel = send_channel;
    memcpy(peer, send_peer, sizeof(peer));
    snprintf(text, sizeof(text), "%s", send_text);
    send_pending = false;
    xSemaphoreGive(node_lock);

    if(to_channel) {
        /* Nobody acknowledges a channel message - it is a broadcast to whoever
         * is listening - so getting it onto the air is the whole outcome. */
        if(public_channel == NULL) {
            Serial.println("[MESH] no public channel to send on");
            msg_settle(MESH_ST_FAILED);
            return;
        }

        uint32_t stamp = the_mesh->getRTCClock()->getCurrentTimeUnique();
        bool ok = the_mesh->sendGroupMessage(stamp, public_channel->channel, self_name,
                                            text, strlen(text));
        if(ok) packets_tx++;
        msg_settle(ok ? MESH_ST_OK : MESH_ST_FAILED);

        Serial.printf("[MESH] channel %s: \"%s: %s\" stamped %u\n",
                      ok ? "send queued" : "SEND FAILED", self_name, text, (unsigned)stamp);
        return;
    }

    ContactInfo *c = the_mesh->lookupContactByPubKey(peer, sizeof(peer));
    if(c == NULL) {
        // Heard once, but long enough ago that MeshCore has forgotten them.
        msg_settle(MESH_ST_FAILED);
        return;
    }

    uint32_t ack = 0, est_timeout = 0;
    int rc = the_mesh->sendMessage(*c, the_mesh->getRTCClock()->getCurrentTimeUnique(),
                                   0, text, ack, est_timeout);
    if(rc == MSG_SEND_FAILED) {
        msg_settle(MESH_ST_FAILED);
        return;
    }

    the_mesh->expectAck(ack, c->id.pub_key);
    packets_tx++;

    Serial.printf("[MESH] sent to %s %s, waiting up to %ums for an ack\n",
                  c->name, rc == MSG_SEND_SENT_DIRECT ? "direct" : "by flood",
                  (unsigned)est_timeout);
}

//************************************[ identity ]******************************
/* The identity is this node's name on the mesh for good: everyone who knows
 * this node knows it by its public key, so losing the key means introducing
 * itself all over again. MeshCore's own store keeps it on the filesystem
 * alongside the display name. */
static void identity_load(mesh::LocalIdentity &self)
{
    IdentityStore store(SPIFFS, "/identity");
    store.begin();

    if(store.load("self", self, self_name, sizeof(self_name))) {
        Serial.println("[MESH] identity restored");
        return;
    }

    self = mesh::LocalIdentity(&rng);
    store.save("self", self, self_name);
    Serial.println("[MESH] new identity generated");
}

//************************************[ region ]********************************
static void region_save(void)
{
    Preferences prefs;
    if(!prefs.begin(MESH_PREFS_NAMESPACE, false)) return;

    prefs.putInt("region", region_idx);
    prefs.putFloat("freq", custom_radio.freq_mhz);
    prefs.putFloat("bw", custom_radio.bandwidth_khz);
    prefs.putUChar("sf", custom_radio.spreading_factor);
    prefs.putUChar("cr", custom_radio.coding_rate);
    prefs.putChar("txpower", tx_power_dbm);
    prefs.putInt("locpol", loc_policy);
    prefs.putDouble("fixlat", fixed_lat);
    prefs.putDouble("fixlon", fixed_lon);
    prefs.putFloat("rxdelay", rx_delay_base);
    prefs.putFloat("airtime", airtime_factor);
    prefs.putUChar("acks", multi_acks);
    prefs.putBool("manualc", manual_contacts);
    prefs.end();
}

static void region_load(void)
{
    Preferences prefs;
    if(!prefs.begin(MESH_PREFS_NAMESPACE, true)) return;

    region_idx = prefs.getInt("region", region_idx);
    custom_radio.freq_mhz         = prefs.getFloat("freq", custom_radio.freq_mhz);
    custom_radio.bandwidth_khz    = prefs.getFloat("bw", custom_radio.bandwidth_khz);
    custom_radio.spreading_factor = prefs.getUChar("sf", custom_radio.spreading_factor);
    custom_radio.coding_rate      = prefs.getUChar("cr", custom_radio.coding_rate);
    tx_power_dbm                  = prefs.getChar("txpower", tx_power_dbm);
    loc_policy                    = prefs.getInt("locpol", loc_policy);
    fixed_lat                     = prefs.getDouble("fixlat", fixed_lat);
    fixed_lon                     = prefs.getDouble("fixlon", fixed_lon);
    rx_delay_base                 = prefs.getFloat("rxdelay", rx_delay_base);
    airtime_factor                = prefs.getFloat("airtime", airtime_factor);
    multi_acks                    = prefs.getUChar("acks", multi_acks);
    manual_contacts               = prefs.getBool("manualc", manual_contacts);
    prefs.end();

    if(region_idx < 0 || region_idx >= MESH_REGION_COUNT) region_idx = 0;
    if(tx_power_dbm < -9 || tx_power_dbm > MESH_TX_POWER_MAX) tx_power_dbm = MESH_TX_POWER_MAX;
    if(loc_policy < MESH_LOC_OFF || loc_policy > MESH_LOC_ALWAYS) loc_policy = MESH_LOC_OFF;
}

//************************************[ position ]******************************
// Defined with the location policy, below the setter that first needs it.
static void gps_follow_policy(void);

bool mesh_net_get_position(double *lat, double *lon)
{
    double la = 0, lo = 0;

    /* The GPS driver only writes these once it has a valid fix, so all zeroes
     * means it has never had one. Null Island is a fair thing to spend as a
     * sentinel. */
    gps_get_coord(&la, &lo);

    /* A live fix wins, because a phone that has moved should say where it is
     * now rather than where it was put. Without one, what it was told is far
     * better than nothing - and on this board, indoors, that is the usual
     * case. */
    if(la == 0.0 && lo == 0.0) {
        if(fixed_lat == 0.0 && fixed_lon == 0.0) return false;

        la = fixed_lat;
        lo = fixed_lon;
        pos_from_fixed = true;
    } else {
        pos_from_fixed = false;
    }

    if(lat) *lat = la;
    if(lon) *lon = lo;
    return true;
}

bool mesh_net_position_is_fixed(void)
{
    return pos_from_fixed;
}

void mesh_net_set_fixed_position(double lat, double lon)
{
    if(lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) return;

    fixed_lat = lat;
    fixed_lon = lon;

    /* A position handed over for adverts that never reaches one is no use to
     * anybody, and asking for it is the whole reason this command exists - so
     * sharing comes on with it rather than leaving the caller to wonder why
     * nothing happened. It stays visible and switchable on the device. */
    if(loc_policy == MESH_LOC_OFF) {
        loc_policy = MESH_LOC_ALWAYS;
        Serial.println("[MESH] a position was set, so sharing it is now on");
    }

    region_save();
    gps_follow_policy();
    Serial.printf("[MESH] position set by hand: %.5f, %.5f\n", lat, lon);
}

int mesh_net_get_loc_policy(void)
{
    return loc_policy;
}

bool mesh_net_wants_gps(void)
{
    return loc_policy != MESH_LOC_OFF;
}

/* The receiver runs while its position is wanted.
 *
 * This was the hole under position sharing: the GPS task is suspended except
 * while the GPS screen is showing, which is the vendor's design and reasonable
 * for a battery - but it meant the cached coordinates only ever advanced while
 * somebody was looking at them. Sharing a position the phone never reads shares
 * whatever it last saw, or nothing at all.
 */
static void gps_follow_policy(void)
{
    if(mesh_net_wants_gps()) gps_task_resume();
    else                     gps_task_suspend();
}

void mesh_net_set_loc_policy(int policy)
{
    if(policy < MESH_LOC_OFF || policy > MESH_LOC_ALWAYS) return;

    loc_policy = policy;
    region_save();
    gps_follow_policy();
}

const char *mesh_net_loc_policy_name(void)
{
    switch(loc_policy) {
        case MESH_LOC_ON_ANNOUNCE: return "When I announce";
        case MESH_LOC_ALWAYS:      return "With every advert";
        default:                   return "Off";
    }
}

void mesh_net_get_tuning(uint32_t *rx, uint32_t *af)
{
    if(rx) *rx = (uint32_t)(rx_delay_base * 1000.0f);
    if(af) *af = (uint32_t)(airtime_factor * 1000.0f);
}

void mesh_net_set_tuning(uint32_t rx, uint32_t af)
{
    rx_delay_base  = rx / 1000.0f;
    airtime_factor = af / 1000.0f;
    region_save();

    Serial.printf("[MESH] tuning: rx delay base %.3f, airtime factor %.3f\n",
                  rx_delay_base, airtime_factor);
}

bool mesh_net_get_manual_contacts(void) { return manual_contacts; }

void mesh_net_set_manual_contacts(bool on)
{
    manual_contacts = on;
    region_save();
    Serial.printf("[MESH] contacts are %s\n", on ? "added by hand" : "added from adverts");
}

uint8_t mesh_net_get_multi_acks(void) { return multi_acks; }

void mesh_net_set_multi_acks(uint8_t count)
{
    multi_acks = count;
    region_save();
}

int8_t mesh_net_get_tx_power(void)
{
    return tx_power_dbm;
}

void mesh_net_set_tx_power(int8_t dbm)
{
    if(dbm < -9 || dbm > MESH_TX_POWER_MAX) return;

    tx_power_dbm = dbm;
    region_save();

    // Same reason as a retune: the radio belongs to the mesh task.
    tx_power_pending = true;
}

void mesh_net_get_radio(mesh_radio_t *out)
{
    if(out == NULL) return;
    *out = (region_idx == MESH_REGION_CUSTOM) ? custom_radio : mesh_presets[region_idx];
}

const char *mesh_net_region_name(void)
{
    return (region_idx == MESH_REGION_CUSTOM) ? custom_radio.name
                                              : mesh_presets[region_idx].name;
}

bool mesh_net_region_is_custom(void)
{
    return region_idx == MESH_REGION_CUSTOM;
}

/* Applies whatever is now selected. Retuning touches the radio and the mesh
 * task is in the middle of using it, so this leaves a note rather than reaching
 * in from the UI task. */
static void region_apply(void)
{
    mesh_radio_t r;
    mesh_net_get_radio(&r);

    region_save();
    region_pending = true;

    Serial.printf("[MESH] %s: %.3fMHz bw%.1f sf%d cr%d\n",
                  mesh_net_region_name(), r.freq_mhz, r.bandwidth_khz,
                  r.spreading_factor, r.coding_rate);
}

void mesh_net_region_next(void)
{
    region_idx = (region_idx + 1) % MESH_REGION_COUNT;
    region_apply();
}

void mesh_net_set_custom(float freq_mhz, float bandwidth_khz,
                         uint8_t spreading_factor, uint8_t coding_rate)
{
    // Bounds are the radio's own limits, not anything to do with what is legal
    // to transmit on - that part is the operator's business.
    if(freq_mhz < 137.0f || freq_mhz > 1020.0f) return;
    if(bandwidth_khz < 7.8f || bandwidth_khz > 500.0f) return;
    if(spreading_factor < 5 || spreading_factor > 12) return;
    if(coding_rate < 5 || coding_rate > 8) return;

    custom_radio.freq_mhz         = freq_mhz;
    custom_radio.bandwidth_khz    = bandwidth_khz;
    custom_radio.spreading_factor = spreading_factor;
    custom_radio.coding_rate      = coding_rate;

    region_idx = MESH_REGION_CUSTOM;
    region_apply();
}

// Defined with the rest of the advert handling, below the task that calls it.
static void advert_send(bool deliberate, bool flood = true);

//************************************[ the clock ]*****************************
/* Keeping the mesh's clock in step with the phone's.
 *
 * Every message this node sends carries a timestamp, and that is the time the
 * receiving node shows to whoever is reading. MeshCore's own clock only counts
 * up from a fixed date built into it, so it needs telling.
 *
 * Seeding it once at startup is not enough, and was the bug: the phone does not
 * know the time that early - it arrives later from the cellular network or a
 * GPS fix - so the mesh clock stayed at its built-in date and everything sent
 * from here was stamped two years in the past. It still travelled, but it
 * landed at the wrong end of everybody's conversation, which looks exactly like
 * a message that never arrived. */
static void clock_service(void)
{
    static uint32_t next_check = 0;

    if(millis() < next_check) return;
    next_check = millis() + 30000;

    time_t now = time(NULL);
    if(now < 1700000000) return;   // the phone does not know either, yet

    uint32_t mesh_now = rtc_clock.getCurrentTime();
    int32_t  drift    = (int32_t)((uint32_t)now - mesh_now);

    if(drift > -60 && drift < 60) return;

    rtc_clock.setCurrentTime((uint32_t)now);
    Serial.printf("[MESH] clock set from the phone: %u, was %u (%d seconds out)\n",
                  (unsigned)now, (unsigned)mesh_now, (int)drift);
}

//************************************[ channels ]******************************
/* MeshCore keeps group channels in RAM and nowhere else, so a channel added
 * from an app lasted until the next restart and then quietly was not there any
 * more. Name and key are all that need keeping - setChannel() recomputes the
 * hash it matches on from the key. */
#define CHANNEL_NAME_LEN 32
#define CHANNEL_KEY_LEN  16

struct stored_channel_t {
    char    name[CHANNEL_NAME_LEN];
    uint8_t key[CHANNEL_KEY_LEN];
};

void mesh_net_save_channels(void)
{
    if(the_mesh == NULL) return;

    stored_channel_t saved[MAX_GROUP_CHANNELS];
    memset(saved, 0, sizeof(saved));

    for(int i = 0; i < MAX_GROUP_CHANNELS; i++) {
        ChannelDetails ch;
        if(!the_mesh->getChannel(i, ch)) continue;

        snprintf(saved[i].name, sizeof(saved[i].name), "%s", ch.name);
        memcpy(saved[i].key, ch.channel.secret, CHANNEL_KEY_LEN);
    }

    Preferences prefs;
    if(!prefs.begin(MESH_PREFS_NAMESPACE, false)) return;

    prefs.putBytes("channels", saved, sizeof(saved));
    prefs.end();

    Serial.println("[MESH] channels saved");
}

static void channels_load(void)
{
    if(the_mesh == NULL) return;

    stored_channel_t saved[MAX_GROUP_CHANNELS];

    Preferences prefs;
    if(!prefs.begin(MESH_PREFS_NAMESPACE, true)) return;

    size_t got = prefs.getBytes("channels", saved, sizeof(saved));
    prefs.end();

    if(got != sizeof(saved)) return;   // never saved, or saved by an older build

    for(int i = 0; i < MAX_GROUP_CHANNELS; i++) {
        // An all-zero key is an empty slot, not a channel with a key of zeroes.
        bool empty = true;
        for(int k = 0; k < CHANNEL_KEY_LEN && empty; k++) {
            if(saved[i].key[k] != 0) empty = false;
        }
        if(empty) continue;

        ChannelDetails ch;
        memset(&ch, 0, sizeof(ch));
        snprintf(ch.name, sizeof(ch.name), "%s", saved[i].name);
        memcpy(ch.channel.secret, saved[i].key, CHANNEL_KEY_LEN);

        the_mesh->setChannel(i, ch);
        Serial.printf("[MESH] channel %d restored: %s\n", i, ch.name);
    }
}

//************************************[ task ]**********************************
static void mesh_task(void *param)
{
    uint32_t next_advert = 0;

    for(;;) {
        if(region_pending) {
            mesh_radio_t r;
            mesh_net_get_radio(&r);

            region_pending = false;
            radio_driver.setParams(r.freq_mhz, r.bandwidth_khz,
                                   r.spreading_factor, r.coding_rate);
            // Say hello on the new settings; nobody there has heard this node.
            advert_send(false);
        }

        if(tx_power_pending) {
            tx_power_pending = false;
            radio.setOutputPower(tx_power_dbm);
        }

        clock_service();
        send_service();
        companion_service();
        the_mesh->loop();

        if(millis() > next_advert) {
            next_advert = millis() + MESH_ADVERT_PERIOD_MS;
            advert_send(false);   // a timer went off, nobody asked
        }

        // Short enough not to miss a receive window, long enough to yield.
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

//************************************[ public API ]****************************
bool mesh_net_init(void)
{
    if(mesh_running) return true;

    node_lock = xSemaphoreCreateMutex();
    if(node_lock == NULL) return false;

    msg_log = (mesh_msg_t *)ps_malloc(sizeof(mesh_msg_t) * MESH_MSG_MAX);
    if(msg_log == NULL) msg_log = (mesh_msg_t *)malloc(sizeof(mesh_msg_t) * MESH_MSG_MAX);
    if(msg_log == NULL) {
        Serial.println("[MESH] no room for the message log");
        return false;
    }
    memset(msg_log, 0, sizeof(mesh_msg_t) * MESH_MSG_MAX);

    // The bus is already up from main; the radio just needs its own settings.
    int state = radio.std_init(&radio_spi);
    if(!state) {
        Serial.println("[MESH] the radio would not start");
        return false;
    }

    packet_mgr = new StaticPoolPacketManager(16);
    the_mesh   = new PhoneMesh(radio_driver, ms_clock, rng, rtc_clock, *packet_mgr, tables);

    region_load();

    mesh::LocalIdentity self;
    identity_load(self);
    the_mesh->self_id = self;

    /* MeshCore stamps messages with the wall clock and uses it to tell a new
     * message from one it has already seen, so give it the phone's time when
     * the phone has one. Its own clock only counts up from a fixed date. */
    time_t now = time(NULL);
    if(now > 1700000000) rtc_clock.setCurrentTime((uint32_t)now);

    radio_driver.begin();
    the_mesh->begin();

    public_channel = the_mesh->addChannel(MESH_PUBLIC_NAME, MESH_PUBLIC_PSK);
    if(public_channel == NULL) {
        Serial.println("[MESH] the public channel would not open");
    } else {
        // The hash is what other nodes match against, so it is the one number
        // worth comparing when channel traffic is not getting through.
        Serial.printf("[MESH] public channel open, hash %02X%02X\n",
                      public_channel->channel.hash[0], public_channel->channel.hash[1]);
    }

    // After the public channel, so anything an app set can replace it.
    channels_load();

    mesh_radio_t r;
    mesh_net_get_radio(&r);
    radio_driver.setParams(r.freq_mhz, r.bandwidth_khz, r.spreading_factor, r.coding_rate);
    radio.setOutputPower(tx_power_dbm);

    /* The companion protocol drives the same node the screen does, so it is
     * attached once the mesh exists and serviced from the same task. */
    companion_attach(the_mesh);

    char key[16];
    mesh_net_get_self_key(key, sizeof(key));
    Serial.printf("[MESH] up as \"%s\" (%s), %s: %.3fMHz bw%.1f sf%d cr%d\n",
                  self_name, key, mesh_net_region_name(), r.freq_mhz,
                  r.bandwidth_khz, r.spreading_factor, r.coding_rate);

    // Whatever was remembered, the receiver should be in the matching state.
    gps_follow_policy();

    mesh_running = true;
    xTaskCreate(mesh_task, "mesh", 1024 * 8, NULL, 6, &mesh_task_h);
    return true;
}

bool mesh_net_is_running(void)
{
    return mesh_running;
}

void mesh_net_get_self_name(char *buf, int len)
{
    if(buf && len > 0) snprintf(buf, len, "%s", self_name);
}

void mesh_net_get_self_key(char *buf, int len)
{
    if(buf == NULL || len <= 0) return;
    buf[0] = '\0';
    if(the_mesh == NULL) return;

    const uint8_t *k = the_mesh->self_id.pub_key;
    snprintf(buf, len, "%02X%02X%02X%02X", k[0], k[1], k[2], k[3]);
}

void mesh_net_set_self_name(const char *name)
{
    if(name == NULL || name[0] == '\0') return;

    snprintf(self_name, sizeof(self_name), "%s", name);

    if(the_mesh) {
        IdentityStore store(SPIFFS, "/identity");
        store.begin();
        store.save("self", the_mesh->self_id, self_name);
    }

    mesh_net_advertise();  // so everyone learns the new name
}

/* Announcing this node.
 *
 * `deliberate` separates an advert the user asked for from the ones that go out
 * on their own every few minutes, which is the whole point of the "when I
 * announce" setting: share where you are because you meant to, not because a
 * timer went off in your pocket. */
static void advert_send(bool deliberate, bool flood)
{
    if(!mesh_running || the_mesh == NULL) return;

    double lat = 0, lon = 0;
    bool with_loc = (loc_policy == MESH_LOC_ALWAYS) ||
                    (loc_policy == MESH_LOC_ON_ANNOUNCE && deliberate);

    // No fix means no position, whatever the setting says.
    if(with_loc && !mesh_net_get_position(&lat, &lon)) with_loc = false;

    uint8_t app_data[MAX_ADVERT_DATA_SIZE];
    uint8_t len;

    if(with_loc) {
        AdvertDataBuilder builder(ADV_TYPE_CHAT, self_name, lat, lon);
        len = builder.encodeTo(app_data);
    } else {
        AdvertDataBuilder builder(ADV_TYPE_CHAT, self_name);
        len = builder.encodeTo(app_data);
    }

    mesh::Packet *pkt = the_mesh->createAdvert(the_mesh->self_id, app_data, len);
    if(pkt == NULL) return;

    /* Flooded by default, so the whole mesh learns this node exists. Zero hop
     * is what an app asks for when it only wants whoever is in direct radio
     * range to hear - introducing yourself to the room rather than the town. */
    if(flood) the_mesh->sendFlood(pkt);
    else      the_mesh->sendZeroHop(pkt);

    packets_tx++;

    Serial.printf("[MESH] advertised%s%s", flood ? "" : " to direct neighbours only",
                  with_loc ? "" : "\n");
    if(with_loc) Serial.printf(" from %.5f, %.5f\n", lat, lon);
}

void mesh_net_advertise(void)
{
    advert_send(true);
}

void mesh_net_advertise_zero_hop(void)
{
    advert_send(true, false);
}

int mesh_net_node_count(void)
{
    return node_count;
}

bool mesh_net_get_node(int idx, mesh_node_t *out)
{
    if(out == NULL || node_lock == NULL) return false;

    bool ok = false;
    xSemaphoreTake(node_lock, portMAX_DELAY);
    if(idx >= 0 && idx < node_count) {
        *out = nodes[idx];

        // Counted here rather than kept on the node, so there is only ever one
        // place that decides what has been read: the log itself.
        int unread = 0;
        for(int i = 0; i < msg_count; i++) {
            if(msg_log[i].unread && msg_matches(&msg_log[i], out->pubkey_prefix)) unread++;
        }
        out->unread = (uint8_t)(unread > 255 ? 255 : unread);
        ok = true;
    }
    xSemaphoreGive(node_lock);
    return ok;
}

//************************************[ chat ]**********************************
const char *mesh_net_channel_name(void)
{
    return MESH_PUBLIC_NAME;
}

bool mesh_net_send_busy(void)
{
    if(msg_log == NULL || node_lock == NULL) return false;

    bool busy = send_pending;
    xSemaphoreTake(node_lock, portMAX_DELAY);
    for(int i = msg_count - 1; i >= 0 && !busy; i--) {
        if(msg_log[i].outgoing && msg_log[i].status == MESH_ST_SENDING) busy = true;
    }
    xSemaphoreGive(node_lock);
    return busy;
}

bool mesh_net_send_text(const uint8_t peer[4], const char *text)
{
    if(!mesh_running || the_mesh == NULL) {
        Serial.println("[MESH] send refused: the radio is not running");
        return false;
    }
    if(text == NULL || text[0] == '\0' || strlen(text) >= MESH_TEXT_LEN) {
        Serial.println("[MESH] send refused: nothing to send, or too long");
        return false;
    }
    if(peer == NULL && public_channel == NULL) {
        Serial.println("[MESH] send refused: the public channel is not open");
        return false;
    }

    /* Only one at a time: an acknowledgement says which packet arrived, not
     * which message, so a second in flight could not be told from the first. */
    if(mesh_net_send_busy()) {
        Serial.println("[MESH] send refused: the last one is still in flight");
        return false;
    }

    // Logged before it is queued, so it is on screen the moment the composer
    // closes rather than after the radio has had its turn.
    msg_add(peer, text, true, MESH_ST_SENDING, false, peer == NULL);

    xSemaphoreTake(node_lock, portMAX_DELAY);
    memset(send_peer, 0, sizeof(send_peer));
    if(peer) memcpy(send_peer, peer, sizeof(send_peer));
    send_channel = (peer == NULL);
    snprintf(send_text, sizeof(send_text), "%s", text);
    send_pending = true;
    xSemaphoreGive(node_lock);

    return true;
}

int mesh_net_msg_count(const uint8_t peer[4])
{
    if(msg_log == NULL || node_lock == NULL) return 0;

    int n = 0;
    xSemaphoreTake(node_lock, portMAX_DELAY);
    for(int i = 0; i < msg_count; i++) {
        if(msg_matches(&msg_log[i], peer)) n++;
    }
    xSemaphoreGive(node_lock);
    return n;
}

bool mesh_net_get_msg(const uint8_t peer[4], int idx, mesh_msg_t *out)
{
    if(msg_log == NULL || node_lock == NULL || out == NULL || idx < 0) return false;

    bool ok = false;
    xSemaphoreTake(node_lock, portMAX_DELAY);
    for(int i = 0; i < msg_count; i++) {
        if(!msg_matches(&msg_log[i], peer)) continue;
        if(idx-- > 0) continue;

        *out = msg_log[i];
        ok = true;
        break;
    }
    xSemaphoreGive(node_lock);
    return ok;
}

void mesh_net_mark_read(const uint8_t peer[4])
{
    if(msg_log == NULL || node_lock == NULL) return;

    xSemaphoreTake(node_lock, portMAX_DELAY);
    for(int i = 0; i < msg_count; i++) {
        if(msg_log[i].unread && msg_matches(&msg_log[i], peer)) {
            msg_log[i].unread = 0;
            chat_rev++;
        }
    }
    xSemaphoreGive(node_lock);
}

int mesh_net_unread(const uint8_t peer[4])
{
    if(msg_log == NULL || node_lock == NULL) return 0;

    int n = 0;
    xSemaphoreTake(node_lock, portMAX_DELAY);
    for(int i = 0; i < msg_count; i++) {
        if(msg_log[i].unread && msg_matches(&msg_log[i], peer)) n++;
    }
    xSemaphoreGive(node_lock);
    return n;
}

int mesh_net_unread_total(void)
{
    if(msg_log == NULL || node_lock == NULL) return 0;

    int n = 0;
    xSemaphoreTake(node_lock, portMAX_DELAY);
    for(int i = 0; i < msg_count; i++) {
        if(msg_log[i].unread) n++;
    }
    xSemaphoreGive(node_lock);
    return n;
}

uint32_t mesh_net_revision(void)
{
    return chat_rev;
}

uint32_t mesh_net_packets_rx(void) { return packets_rx; }
uint32_t mesh_net_packets_tx(void) { return packets_tx; }
