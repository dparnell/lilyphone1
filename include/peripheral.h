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
    E_PERI_BHI260AP,
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
void keypad_read(lv_indev_drv_t * indev_drv, lv_indev_data_t * data);

// gyro
bool BHI260AP_init(void);
void BHI260AP_get_val(int val_type, float *x, float *y, float *z);

// LTR553
bool LTR553_init(void);
uint16_t LTR_553ALS_get_channel(int ch); // ch 0~1
uint16_t LTR_553ALS_get_ps(void);

// gps u-blox m10q
bool gps_init(void);
void gps_task_create(void);
void gps_task_suspend(void);
void gps_task_resume(void);
void gps_get_coord(double *lat, double *lng);
void gps_get_data(uint16_t *year, uint8_t *month, uint8_t *day);
void gps_get_time(uint8_t *hour, uint8_t *minute, uint8_t *second);
void gps_get_satellites(uint32_t *vsat);
void gps_get_speed(double *speed);

#endif