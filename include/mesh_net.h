#ifndef __MESH_NET_H__
#define __MESH_NET_H__

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
#define MESH_NET_NAME_LEN  32
#define MESH_NET_NODES_MAX 24

/* A message is short by design: MeshCore packets are small and the airtime of a
 * long one at spreading factor 10 is measured in seconds. */
#define MESH_TEXT_LEN      161
#define MESH_MSG_MAX       80

// mesh_msg_t::status
#define MESH_ST_OK         0   // received, or sent with nothing to wait for
#define MESH_ST_SENDING    1
#define MESH_ST_FAILED     2
#define MESH_ST_DELIVERED  3   // the other end acknowledged it

/*********************************************************************************
 *                                  TYPEDEFS
 * *******************************************************************************/
/* One node heard advertising itself. MeshCore nodes announce their identity and
 * name periodically, so this fills in on its own once the radio is listening. */
typedef struct {
    char     name[MESH_NET_NAME_LEN];
    uint8_t  pubkey_prefix[4];  // enough of the public key to tell nodes apart
    int8_t   snr;               // tenths of a dB as MeshCore reports it, rounded
    int16_t  rssi;
    uint32_t heard_ms;          // millis() when it was last heard
    uint8_t  hops;
    uint8_t  has_path;          // a route back is known, so messages go direct
    uint8_t  unread;            // messages from it that have not been looked at
} mesh_node_t;

/* One message in a mesh conversation. `peer` is the node it was with, or all
 * zeroes when it was on the public channel, which every node can read. */
typedef struct {
    uint8_t  peer[4];
    char     text[MESH_TEXT_LEN];
    uint32_t at_ms;
    uint8_t  outgoing;
    uint8_t  status;
    uint8_t  unread;
    uint8_t  is_channel;
} mesh_msg_t;

/*********************************************************************************
 *                              GLOBAL PROTOTYPES
 * *******************************************************************************/
/* Brings up the radio and starts the mesh task. Safe to call once, from setup.
 * Returns false when the radio would not start. */
bool mesh_net_init(void);
bool mesh_net_is_running(void);

/* This node's own name and the first bytes of its public key, which is how it
 * appears to everyone else. */
void mesh_net_get_self_name(char *buf, int len);
void mesh_net_get_self_key(char *buf, int len);
/* Renames this node and announces the change. Persisted. */
void mesh_net_set_self_name(const char *name);

/* Sends an advertisement now rather than waiting for the next one. */
void mesh_net_advertise(void);

/* The radio settings, which every node on a mesh has to agree on exactly: get
 * any one of the four wrong and nothing is heard at all, which looks identical
 * to a radio that never started.
 *
 * They come as named presets, since a local mesh usually publishes a set, plus
 * a custom entry for one that does not match any of them. All persisted. */
typedef struct {
    const char *name;
    float       freq_mhz;
    float       bandwidth_khz;
    uint8_t     spreading_factor;
    uint8_t     coding_rate;
} mesh_radio_t;

/* The settings actually in force, whether from a preset or set by hand. */
void        mesh_net_get_radio(mesh_radio_t *out);
const char *mesh_net_region_name(void);
void        mesh_net_region_next(void);
bool        mesh_net_region_is_custom(void);

/* Sets all four by hand, which selects the custom preset. Retunes the radio. */
void        mesh_net_set_custom(float freq_mhz, float bandwidth_khz,
                                uint8_t spreading_factor, uint8_t coding_rate);

/* Transmit power in dBm. The ceiling is the SX1262's, not a legal limit - what
 * is allowed where you are is your business. Persisted. */
#define MESH_TX_POWER_MAX 22
int8_t      mesh_net_get_tx_power(void);
void        mesh_net_set_tx_power(int8_t dbm);

int  mesh_net_node_count(void);
/* Copies out one heard node, newest first. False when `idx` is past the end. */
bool mesh_net_get_node(int idx, mesh_node_t *out);

/* --- chat ---
 *
 * There is nobody to add: nodes announce themselves and this one keeps whoever
 * it hears, so the list of nodes above is also the list of people to talk to.
 * A message goes either to one of them or to the public channel, which is the
 * shared frequency-wide conversation everyone running MeshCore can read.
 *
 * Pass NULL as `peer` throughout to mean the public channel.
 */
const char *mesh_net_channel_name(void);

/* Queues a message. It appears in the log immediately as sending, and settles
 * to delivered or failed once the mesh has had its say. False when the radio is
 * down, the text is too long, or one is already on its way. */
bool mesh_net_send_text(const uint8_t peer[4], const char *text);
/* True while a message is still in flight, which is when a send would fail. */
bool mesh_net_send_busy(void);

/* The log for one conversation, oldest first. */
int  mesh_net_msg_count(const uint8_t peer[4]);
bool mesh_net_get_msg(const uint8_t peer[4], int idx, mesh_msg_t *out);
void mesh_net_mark_read(const uint8_t peer[4]);
int  mesh_net_unread(const uint8_t peer[4]);
int  mesh_net_unread_total(void);

/* Bumped whenever the log or the node list changes, so a screen can tell at a
 * glance whether it needs rebuilding - redrawing this display is expensive. */
uint32_t mesh_net_revision(void);

/* Counters, for telling "nothing is out there" apart from "the radio is deaf". */
uint32_t mesh_net_packets_rx(void);
uint32_t mesh_net_packets_tx(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif
#endif
