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

/* The public MeshCore settings. Anyone this phone is meant to talk to has to be
 * using the same ones, so these are the values the MeshCore community publishes
 * rather than anything chosen here.
 *
 * Note that some local meshes have since moved to "narrow" settings - 62.5kHz
 * bandwidth with a lower spreading factor - so if nothing is ever heard on the
 * right frequency, that is the next thing to check against whoever else is out
 * there. */
#define MESH_TX_POWER_DBM 22

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
    prefs.end();

    if(region_idx < 0 || region_idx >= MESH_REGION_COUNT) region_idx = 0;
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
            mesh_net_advertise();
        }

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

    region_load();

    mesh::LocalIdentity self;
    identity_load(self);
    the_mesh->self_id = self;

    radio_driver.begin();
    the_mesh->begin();
    mesh_radio_t r;
    mesh_net_get_radio(&r);
    radio_driver.setParams(r.freq_mhz, r.bandwidth_khz, r.spreading_factor, r.coding_rate);
    radio.setOutputPower(MESH_TX_POWER_DBM);

    char key[16];
    mesh_net_get_self_key(key, sizeof(key));
    Serial.printf("[MESH] up as \"%s\" (%s), %s: %.3fMHz bw%.1f sf%d cr%d\n",
                  self_name, key, mesh_net_region_name(), r.freq_mhz,
                  r.bandwidth_khz, r.spreading_factor, r.coding_rate);

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
