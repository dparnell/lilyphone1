/**
 * MeshCore on the T-Deck-Pro.
 *
 * This is the foundation layer: it owns the SX1262, runs a MeshCore mesh, sends
 * this node's advertisement, and records the nodes it hears. Messaging sits on
 * top of BaseChatMesh and is not wired up yet - what this proves is that the
 * radio, the crypto and the protocol all come up on this board.
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
#include <helpers/radiolib/CustomSX1262Wrapper.h>

#include "mesh_net.h"
#include "utilities.h"

/* Declared here rather than by including the UI port header, which would drag
 * LVGL in for the sake of one number. */
extern "C" uint16_t ui_battery_27220_get_voltage(void);

#define MESH_PREFS_NAMESPACE "mesh"

/* The public MeshCore settings for this region. Anyone the phone is meant to
 * talk to has to be using the same ones. */
#define MESH_FREQ_MHZ     915.0f
#define MESH_BANDWIDTH    250.0f
#define MESH_SPREADING    10
#define MESH_CODING_RATE  5
#define MESH_TX_POWER_DBM 22

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

static void node_record(const mesh::Identity &id, const char *name, int hops,
                        float snr, float rssi)
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
    entry.heard_ms = millis();

    if(found < 0) {
        if(node_count < MESH_NET_NODES_MAX) node_count++;
        found = node_count - 1;
    }

    // Shuffle down so the most recently heard is always first.
    for(int i = found; i > 0; i--) nodes[i] = nodes[i - 1];
    nodes[0] = entry;

    xSemaphoreGive(node_lock);

    Serial.printf("[MESH] heard %s (%02X%02X%02X%02X) snr %d hops %d\n",
                  entry.name, entry.pubkey_prefix[0], entry.pubkey_prefix[1],
                  entry.pubkey_prefix[2], entry.pubkey_prefix[3], entry.snr, hops);
}

class PhoneMesh : public mesh::Mesh {
public:
    PhoneMesh(mesh::Radio &r, mesh::MillisecondClock &m, mesh::RNG &rn,
              mesh::RTCClock &rt, mesh::PacketManager &mgr, mesh::MeshTables &t)
        : mesh::Mesh(r, m, rn, rt, mgr, t) { }

    void onAdvertRecv(mesh::Packet *packet, const mesh::Identity &id, uint32_t timestamp,
                      const uint8_t *app_data, size_t app_data_len) override {
        AdvertDataParser parser(app_data, app_data_len);

        node_record(id, parser.isValid() ? parser.getName() : NULL,
                    packet->path_len, _radio->getLastSNR(), _radio->getLastRSSI());
        packets_rx++;
    }

    /* A phone in a pocket is a poor repeater and forwarding costs battery, so
     * this node listens and speaks for itself but does not relay. */
    bool allowPacketForward(const mesh::Packet *packet) override { return false; }
};

static mesh::PacketManager *packet_mgr = NULL;
static PhoneMesh           *the_mesh   = NULL;
static TaskHandle_t         mesh_task_h = NULL;
static bool                 mesh_running = false;

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

//************************************[ task ]**********************************
static void mesh_task(void *param)
{
    uint32_t next_advert = 0;

    for(;;) {
        the_mesh->loop();

        if(millis() > next_advert) {
            next_advert = millis() + MESH_ADVERT_PERIOD_MS;
            mesh_net_advertise();
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

    // The bus is already up from main; the radio just needs its own settings.
    int state = radio.std_init(&radio_spi);
    if(!state) {
        Serial.println("[MESH] the radio would not start");
        return false;
    }

    packet_mgr = new StaticPoolPacketManager(16);
    the_mesh   = new PhoneMesh(radio_driver, ms_clock, rng, rtc_clock, *packet_mgr, tables);

    mesh::LocalIdentity self;
    identity_load(self);
    the_mesh->self_id = self;

    radio_driver.begin();
    the_mesh->begin();
    radio_driver.setParams(MESH_FREQ_MHZ, MESH_BANDWIDTH, MESH_SPREADING, MESH_CODING_RATE);
    radio.setOutputPower(MESH_TX_POWER_DBM);

    char key[16];
    mesh_net_get_self_key(key, sizeof(key));
    Serial.printf("[MESH] up as \"%s\" (%s) on %.1fMHz sf%d\n",
                  self_name, key, MESH_FREQ_MHZ, MESH_SPREADING);

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

void mesh_net_advertise(void)
{
    if(!mesh_running || the_mesh == NULL) return;

    uint8_t app_data[MAX_ADVERT_DATA_SIZE];
    AdvertDataBuilder builder(ADV_TYPE_CHAT, self_name);
    uint8_t len = builder.encodeTo(app_data);

    mesh::Packet *pkt = the_mesh->createAdvert(the_mesh->self_id, app_data, len);
    if(pkt) {
        the_mesh->sendFlood(pkt);
        packets_tx++;
        Serial.println("[MESH] advertised");
    }
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
        ok = true;
    }
    xSemaphoreGive(node_lock);
    return ok;
}

uint32_t mesh_net_packets_rx(void) { return packets_rx; }
uint32_t mesh_net_packets_tx(void) { return packets_tx; }
