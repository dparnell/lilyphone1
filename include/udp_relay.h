#ifndef __UDP_RELAY_H__
#define __UDP_RELAY_H__

/*********************************************************************************
 *                                  INCLUDES
 * *******************************************************************************/
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif
/*********************************************************************************
 *                                   DEFINES
 * *******************************************************************************/
/* The largest datagram carried either way. WireGuard should be configured with
 * an MTU comfortably under this - see the README - because anything larger is
 * dropped rather than fragmented. */
#define UDP_RELAY_MTU        1472

#define UDP_RELAY_APN_LEN    32
#define UDP_RELAY_HOST_LEN   48
#define UDP_RELAY_SSID_LEN   24
#define UDP_RELAY_PASS_LEN   32

typedef enum {
    UDP_RELAY_OFF = 0,
    UDP_RELAY_STARTING,   // bringing the access point and the data context up
    UDP_RELAY_RUNNING,
    UDP_RELAY_FAILED,
} udp_relay_state_t;

typedef struct {
    char     apn[UDP_RELAY_APN_LEN];    // blank lets the network choose
    char     host[UDP_RELAY_HOST_LEN];  // the far end of the tunnel
    uint16_t port;
    uint16_t listen_port;               // what the client points its client at
    char     ssid[UDP_RELAY_SSID_LEN];
    char     pass[UDP_RELAY_PASS_LEN];  // under 8 characters means an open network
} udp_relay_cfg_t;

typedef struct {
    udp_relay_state_t state;
    bool     client_seen;
    uint32_t to_modem;      // datagrams forwarded out
    uint32_t to_client;     // datagrams handed back
    uint32_t dropped;
    char     detail[64];    // why it failed, or what it is waiting on
} udp_relay_status_t;

/*********************************************************************************
 *                              GLOBAL PROTOTYPES
 * *******************************************************************************/
/* Restores the saved configuration. Call once during startup. */
void udp_relay_init(void);

const udp_relay_cfg_t *udp_relay_get_cfg(void);
void udp_relay_set_cfg(const udp_relay_cfg_t *cfg); // persisted

/* Brings up the access point and the cellular socket, or takes them down. Both
 * return immediately; watch udp_relay_get_status() for progress. */
void udp_relay_start(void);
void udp_relay_stop(void);
bool udp_relay_is_on(void);

void udp_relay_get_status(udp_relay_status_t *out);

#ifdef __cplusplus
} /*extern "C"*/
#endif
#endif
