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
} mesh_node_t;

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

/* The regulatory region, which is what picks the frequency. Stepped through a
 * short list rather than typed, since only a few are legal anywhere and the
 * exact numbers matter - a node on the wrong one hears nothing at all, which
 * looks identical to a radio that never started. Persisted. */
const char *mesh_net_region_name(void);
float       mesh_net_region_freq(void);
void        mesh_net_region_next(void);

int  mesh_net_node_count(void);
/* Copies out one heard node, newest first. False when `idx` is past the end. */
bool mesh_net_get_node(int idx, mesh_node_t *out);

/* Counters, for telling "nothing is out there" apart from "the radio is deaf". */
uint32_t mesh_net_packets_rx(void);
uint32_t mesh_net_packets_tx(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif
#endif
