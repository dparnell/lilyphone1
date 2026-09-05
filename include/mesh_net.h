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
