/**
 * WiFi access point that relays UDP out over the cellular link.
 *
 * The point of it is to let a laptop run a WireGuard tunnel through the phone's
 * mobile data. The laptop joins the phone's access point and points its tunnel
 * endpoint at the phone; everything it sends is forwarded to the real endpoint
 * over the modem, and everything that comes back goes to the client.
 *
 * It is a relay to one configured far end, not a router, and that is a
 * limitation of the platform rather than a choice: the Arduino core's lwIP is
 * built with CONFIG_LWIP_IP_FORWARD off, so a packet addressed to some other
 * host is dropped in ip4_input before any code here could see it. Only packets
 * addressed to this device arrive at all. For a tunnel that is enough - a
 * WireGuard peer has exactly one endpoint - but it will not serve as a general
 * gateway.
 *
 * The other thing to know is the speed. Datagrams cross to the modem over a
 * 115200 baud serial link wrapped in AT commands, so the ceiling is somewhere
 * around 50kbit/s and latency is tens of milliseconds per packet. It is a
 * usable control channel, not a usable internet connection. Raising the link
 * with AT+IPR is the one change that would move that number much.
 */
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include <esp_heap_caps.h>

#include "udp_relay.h"
#include "modem_service.h"

#define RELAY_PREFS_NAMESPACE "relay"

static udp_relay_cfg_t    cfg;
static udp_relay_status_t status;

static WiFiUDP  sock;
static bool     sock_listening = false;
static IPAddress client_ip;
static uint16_t  client_port = 0;

static TaskHandle_t relay_task_h = NULL;

static void relay_set_detail(const char *text)
{
    strncpy(status.detail, text, sizeof(status.detail) - 1);
    status.detail[sizeof(status.detail) - 1] = '\0';
}

//************************************[ configuration ]*************************
void udp_relay_init(void)
{
    memset(&cfg, 0, sizeof(cfg));
    memset(&status, 0, sizeof(status));

    // Defaults that at least produce a joinable network out of the box.
    strncpy(cfg.ssid, "lilyphone1", sizeof(cfg.ssid) - 1);
    strncpy(cfg.pass, "lilyphone", sizeof(cfg.pass) - 1);
    cfg.port        = 51820;  // WireGuard's usual
    cfg.listen_port = 51820;

    Preferences prefs;
    if(prefs.begin(RELAY_PREFS_NAMESPACE, true)) {
        String apn  = prefs.getString("apn", cfg.apn);
        String host = prefs.getString("host", cfg.host);
        String ssid = prefs.getString("ssid", cfg.ssid);
        String pass = prefs.getString("pass", cfg.pass);

        strncpy(cfg.apn,  apn.c_str(),  sizeof(cfg.apn) - 1);
        strncpy(cfg.host, host.c_str(), sizeof(cfg.host) - 1);
        strncpy(cfg.ssid, ssid.c_str(), sizeof(cfg.ssid) - 1);
        strncpy(cfg.pass, pass.c_str(), sizeof(cfg.pass) - 1);

        cfg.port        = (uint16_t)prefs.getUShort("port", cfg.port);
        cfg.listen_port = (uint16_t)prefs.getUShort("listen", cfg.listen_port);
        prefs.end();
    }
}

const udp_relay_cfg_t *udp_relay_get_cfg(void)
{
    return &cfg;
}

void udp_relay_set_cfg(const udp_relay_cfg_t *in)
{
    if(in == NULL) return;
    cfg = *in;

    Preferences prefs;
    if(!prefs.begin(RELAY_PREFS_NAMESPACE, false)) return;

    prefs.putString("apn", cfg.apn);
    prefs.putString("host", cfg.host);
    prefs.putString("ssid", cfg.ssid);
    prefs.putString("pass", cfg.pass);
    prefs.putUShort("port", cfg.port);
    prefs.putUShort("listen", cfg.listen_port);
    prefs.end();
}

//************************************[ the pump ]******************************
/* Runs on its own task rather than an LVGL timer: a datagram should not wait
 * behind a several hundred millisecond e-paper repaint. */
static void relay_task(void *param)
{
    static uint8_t buf[UDP_RELAY_MTU];

    for(;;) {
        if(status.state != UDP_RELAY_RUNNING && status.state != UDP_RELAY_STARTING) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if(status.state == UDP_RELAY_STARTING) {
            if(modem_udp_is_open()) {
                status.state = UDP_RELAY_RUNNING;
                relay_set_detail("waiting for a client");
            } else {
                char why[64];
                modem_udp_get_error(why, sizeof(why));
                if(why[0]) {
                    status.state = UDP_RELAY_FAILED;
                    relay_set_detail(why);
                }
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
        }

        // From the client, out to the far end.
        int len = sock.parsePacket();
        if(len > 0) {
            if(len > UDP_RELAY_MTU) {
                sock.flush();
                status.dropped++;
            } else {
                int got = sock.read(buf, len);

                // Whoever spoke last is who replies go to; there is only one.
                client_ip   = sock.remoteIP();
                client_port = sock.remotePort();
                if(!status.client_seen) {
                    status.client_seen = true;
                    relay_set_detail("client connected");
                    Serial.printf("[RELAY] client %s:%u\n", client_ip.toString().c_str(),
                                  (unsigned)client_port);
                }

                if(got > 0 && modem_udp_send(buf, (uint16_t)got)) {
                    status.to_modem++;
                } else {
                    // The radio is not keeping up; better to lose a datagram
                    // than to stall, which is what UDP expects anyway.
                    status.dropped++;
                }
            }
        }

        // From the far end, back to the client.
        uint16_t in_len = 0;
        if(modem_udp_receive(buf, sizeof(buf), &in_len) && in_len > 0) {
            if(client_port != 0) {
                sock.beginPacket(client_ip, client_port);
                sock.write(buf, in_len);
                sock.endPacket();
                status.to_client++;
            } else {
                status.dropped++;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

//************************************[ control ]*******************************
void udp_relay_start(void)
{
    if(status.state == UDP_RELAY_RUNNING || status.state == UDP_RELAY_STARTING) return;

    if(cfg.host[0] == '\0') {
        status.state = UDP_RELAY_FAILED;
        relay_set_detail("no endpoint set");
        return;
    }

    status.to_modem    = 0;
    status.to_client   = 0;
    status.dropped     = 0;
    status.client_seen = false;
    client_port        = 0;

    // Under eight characters WPA2 will not take, so run open instead of
    // silently failing to start.
    bool open_network = strlen(cfg.pass) < 8;

    /* The radio wants tens of kilobytes of internal RAM, and the display's draw
     * buffer has already taken a large bite of it, so say how much is left
     * before asking - a failure here is otherwise hard to account for. */
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    Serial.printf("[RELAY] %u bytes of internal RAM free before the radio starts\n",
                  (unsigned)internal_free);

    WiFi.mode(WIFI_AP);
    if(!WiFi.softAP(cfg.ssid, open_network ? NULL : cfg.pass)) {
        status.state = UDP_RELAY_FAILED;
        relay_set_detail(internal_free < 48000 ? "not enough memory for the radio"
                                               : "could not start the access point");
        return;
    }

    Serial.printf("[RELAY] AP \"%s\"%s at %s\n", cfg.ssid,
                  open_network ? " (open)" : "",
                  WiFi.softAPIP().toString().c_str());

    sock.begin(cfg.listen_port);
    sock_listening = true;

    modem_udp_open(cfg.apn, cfg.host, cfg.port, cfg.listen_port);

    status.state = UDP_RELAY_STARTING;
    relay_set_detail("opening the data connection");

    if(relay_task_h == NULL) {
        xTaskCreate(relay_task, "udp_relay", 1024 * 4, NULL, 5, &relay_task_h);
    }
}

void udp_relay_stop(void)
{
    if(status.state == UDP_RELAY_OFF) return;

    modem_udp_close();

    if(sock_listening) {
        sock.stop();
        sock_listening = false;
    }

    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);

    status.state       = UDP_RELAY_OFF;
    status.client_seen = false;
    client_port        = 0;
    relay_set_detail("");

    Serial.println("[RELAY] stopped");
}

bool udp_relay_is_on(void)
{
    return status.state == UDP_RELAY_RUNNING || status.state == UDP_RELAY_STARTING;
}

void udp_relay_get_status(udp_relay_status_t *out)
{
    if(out) *out = status;
}
