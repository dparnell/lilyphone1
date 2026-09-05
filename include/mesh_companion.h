#ifndef __MESH_COMPANION_H__
#define __MESH_COMPANION_H__

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
/* How the MeshCore companion app reaches this node. Only one at a time: the two
 * radios each want a sizeable bite of internal memory, and the WiFi link runs
 * the device as an access point, which it cannot do while the hotspot has it. */
enum {
    MESH_LINK_OFF = 0,
    MESH_LINK_BLE,
    MESH_LINK_WIFI,
};

#define MESH_LINK_SSID_LEN 24
#define MESH_LINK_PASS_LEN 32
#define MESH_LINK_PORT     5000

/*********************************************************************************
 *                              GLOBAL PROTOTYPES
 * *******************************************************************************/
/* The link the companion app connects over. Persisted, and applied as soon as
 * it is set - except that turning Bluetooth off only stops it advertising:
 * the stack keeps the memory it claimed until the next restart. */
int  mesh_companion_get_link(void);
void mesh_companion_set_link(int link);
/* The chosen link, named for showing. */
const char *mesh_companion_link_name(void);

/* Whether the link is set to come up, and the node is there for it to drive.
 *
 * Asked during startup, before the display buffers are allocated, because the
 * answer decides where they go: the Bluetooth and WiFi stacks want more
 * internal memory than is left once a full screen buffer has been taken out of
 * it, so when the link is on, the drawing buffer goes to PSRAM instead. That
 * costs some drawing speed and is the whole reason the link is a setting rather
 * than always on. */
bool mesh_companion_link_saved(void);

/* Starts the remembered link. Called once during startup, after the display has
 * taken the memory it needs, so that what is left is what the radio gets. */
void mesh_companion_boot(void);

/* Whether an app is connected right now. */
bool mesh_companion_is_connected(void);

/* Why the link is not up, or what it is doing. Empty when all is well. */
void mesh_companion_get_detail(char *buf, int len);

/* What the user has to type into the app to pair over Bluetooth. Generated once
 * and then persisted, so it does not change under a paired phone. */
uint32_t mesh_companion_ble_pin(void);
/* The name this node advertises itself under over Bluetooth. */
void mesh_companion_ble_name(char *buf, int len);

/* The access point the WiFi link puts up, and where the app should connect. A
 * password under eight characters is not one WPA2 will take, so the network
 * runs open instead. */
void mesh_companion_get_wifi(char *ssid, int ssid_len, char *pass, int pass_len);
void mesh_companion_set_wifi(const char *ssid, const char *pass);
void mesh_companion_get_address(char *buf, int len);

/* Frames exchanged with the app since it connected, for telling a link that is
 * up but idle apart from one that is not working. */
uint32_t mesh_companion_frames_rx(void);
uint32_t mesh_companion_frames_tx(void);
/* Messages held for an app that is not connected. */
int  mesh_companion_queued(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif
#endif
