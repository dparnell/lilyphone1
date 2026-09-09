#ifndef __PERIPHERAL_H__
#define __PERIPHERAL_H__

#include <stdint.h>
#include "lvgl.h"

#define GPS_PRIORITY     (configMAX_PRIORITIES - 1)
#define LORA_PRIORITY    (configMAX_PRIORITIES - 2)
#define WS2812_PRIORITY  (configMAX_PRIORITIES - 3)
#define BATTERY_PRIORITY (configMAX_PRIORITIES - 4)
#define A7682E_PRIORITY  (configMAX_PRIORITIES - 5)

enum {
    E_PERI_LORA = 0,
    E_PERI_TOUCH,
    E_PERI_KYEPAD,
    E_PERI_BQ25896,
    E_PERI_BQ27220,
    E_PERI_SD,
    E_PERI_GPS,
    E_PERI_LTR_553ALS,
    E_PERI_A7682E,
    E_PERI_INK_SCREEN,
    E_PERI_MIC,
    E_PERI_NUM_MAX,
};

/* The SX1262 belongs to MeshCore now - see src/apps/mesh_net.cpp. The vendor's
 * demo driver was removed because two drivers cannot both own one radio. */

// keypad
#define KEYPAD_PRESS   1
#define KEYPAD_RELEASE 0

bool keypad_init(int address);
/* Whether this key was already held down as the keypad started, which is how
 * a key held through boot can change what the firmware does. */
bool keypad_boot_key_held(char c);
void keypad_read(lv_indev_drv_t * indev_drv, lv_indev_data_t * data);

/* The 1.8V sensor rail. It fed two parts: the LTR-553ALS below, and a BHI260AP
 * motion hub that nothing in this firmware ever read - so the hub's driver was
 * removed and the rail now comes up only if a light sensor answers on it. The
 * board still carries the hub; it simply has no power and no code. */
void sensor_rail_set(bool on);
bool sensor_rail_is_on(void);

// LTR553
bool LTR553_init(void);
/* Why it is not available, for whatever asks after the boot log has scrolled. */
const char *LTR553_status(void);
uint16_t LTR_553ALS_get_channel(int ch); // ch 0~1
uint16_t LTR_553ALS_get_ps(void);

// gps u-blox m10q
bool gps_init(void);
void gps_task_create(void);
void gps_task_suspend(void);
void gps_task_resume(void);
bool gps_is_started(void);
/* Cold starts the receiver and reconfigures it. Blocks for over a second,
 * and the first fix afterwards takes minutes rather than seconds. */
bool gps_reset(void);
/* Whether the receiver is being read, and whether it currently knows where it
 * is. Not the same question: it is only read while something wants it. */
bool gps_is_running(void);
bool gps_has_fix(void);
void gps_get_coord(double *lat, double *lng);
void gps_get_data(uint16_t *year, uint8_t *month, uint8_t *day);
void gps_get_time(uint8_t *hour, uint8_t *minute, uint8_t *second);
void gps_get_satellites(uint32_t *vsat);
void gps_get_speed(double *speed);

#endif