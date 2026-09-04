#ifndef __UI_PORT_PHONE1_H__
#define __UI_PORT_PHONE1_H__

/*********************************************************************************
 *                                  INCLUDES
 * *******************************************************************************/
#include "lvgl.h"

#include "peripheral.h"
#include "ui_phone1.h"
#include "utilities.h"
#include "modem_service.h"
#include "phone_store.h"

#ifdef __cplusplus
extern "C" {
#endif
/*********************************************************************************
 *                                   DEFINES
 * *******************************************************************************/
// default language
#define DEFAULT_LANGUAGE_EN 0 // English
#define DEFAULT_LANGUAGE_CN 1 // Chinese

/*********************************************************************************
 *                                   MACROS
 * *******************************************************************************/

/*********************************************************************************
 *                                  TYPEDEFS
 * *******************************************************************************/

/*********************************************************************************
 *                              GLOBAL PROTOTYPES
 * *******************************************************************************/
void ui_disp_full_refr(void);
/* Parks the e-paper panel immediately. The driver otherwise does it a few
 * seconds after the last update, which is too late before a deep sleep. */
void ui_disp_hibernate(void);

// [ screen 1 ] --- lora
float ui_lora_get_freq(void);
int ui_lora_get_mode(void);
void ui_lora_set_mode(int mode);
void ui_lora_send(const char *str);
void ui_lora_recv_loop(void);
bool ui_lora_get_recv(const char **str, int *rssi);
void ui_lora_set_recv_flag(void);

// [ screen 2 ] --- setting
void ui_setting_set_language(int language);
void ui_setting_set_keypad_light(bool on);
void ui_setting_set_motor_status(bool on);
void ui_setting_set_gps_status(bool on);
void ui_setting_set_lora_status(bool on);
void ui_setting_set_gyro_status(bool on);
void ui_setting_set_a7682_status(bool on);

int ui_setting_get_language(void);
bool ui_setting_get_keypad_light(void);
bool ui_setting_get_motor_status(void);
bool ui_setting_get_gps_status(void);  
bool ui_setting_get_lora_status(void); 
bool ui_setting_get_gyro_status(void); 
bool ui_setting_get_a7682_status(void);

// setting - > About System
const char *ui_setting_get_sf_ver(void);
const char *ui_setting_get_hd_ver(void);
void ui_setting_get_sd_capacity(uint64_t *total, uint64_t *used);

// [ screen 3 ] --- GPS
void ui_gps_task_suspend(void);
void ui_gps_task_resume(void);
void ui_gps_get_coord(double *lat, double *lng);
void ui_gps_get_data(uint16_t *year, uint8_t *month, uint8_t *day);
void ui_gps_get_time(uint8_t *hour, uint8_t *minute, uint8_t *second);
void ui_gps_get_satellites(uint32_t *vsat);
void ui_gps_get_speed(double *speed);

// [ screen 4 ] --- Wifi Scan
void ui_wifi_get_scan_info(ui_wifi_scan_info_t *list, int list_len);

// [ screen 5 ] --- State
bool ui_test_get(int peri_id);
bool ui_test_sd_card(void);
bool ui_test_a7682e(void);
bool ui_test_pcm5102(void);

// [ screen 6 ] --- Battery
/* 25896 */
bool ui_battery_25896_is_vbus_in(void);
bool ui_batt_25896_is_chg(void);
float ui_batt_25896_get_vbus(void);
float ui_batt_25896_get_vsys(void);
float ui_batt_25896_get_vbat(void);
float ui_batt_25896_get_volt_targ(void);
float ui_batt_25896_get_chg_curr(void);
float ui_batt_25896_get_pre_curr(void);
const char * ui_batt_25896_get_chg_st(void);
const char * ui_batt_25896_get_vbus_st(void);
const char * ui_batt_25896_get_ntc_st(void);

/* 27220 */
bool ui_battery_27220_is_vaild(void);
bool ui_battery_27220_get_input(void);
bool ui_battery_27220_get_charge_finish(void);
uint16_t ui_battery_27220_get_status(void);
uint16_t ui_battery_27220_get_voltage(void);
int16_t ui_battery_27220_get_current(void);
uint16_t ui_battery_27220_get_temperature(void);
uint16_t ui_battery_27220_get_full_capacity(void);
uint16_t ui_battery_27220_get_design_capacity(void);
uint16_t ui_battery_27220_get_remain_capacity(void);
uint16_t ui_battery_27220_get_percent(void);
uint16_t ui_battery_27220_get_health(void);
const char * ui_battert_27220_get_percent_level(void);

// [ screen 7 ] --- Input
int ui_input_get_touch_coord(int *x, int *y);
int ui_input_get_keypay_val(char *v);
void ui_input_set_keypay_flag(void);
int ui_other_get_LTR(int *ch0, int *ch1, int *ps);
int ui_other_get_gyro(float *gyro_x, float *gyro_y, float *gyro_z);

// [ screen 8 ] --- telephony
/* Thin wrappers over the modem service so the UI keeps talking to one layer.
 * All of them return immediately; the modem task does the work. */
void ui_phone_dial(const char *number);
void ui_phone_answer(void);
void ui_phone_hang_up(void);
modem_call_state_t ui_phone_get_call_state(void);
void ui_phone_get_call_number(char *buf, int len);
uint32_t ui_phone_get_call_duration(void);
bool ui_phone_is_registered(void);
uint8_t ui_phone_get_signal(void);
void ui_phone_get_operator(char *buf, int len);
/* Buzzes the motor for `ms` and releases it from a one-shot timer, so the
 * caller never blocks the UI. Does nothing when the user turned the motor off
 * in the settings. */
void ui_phone_vibrate(int ms);

// [ screen 13 ] --- messaging
uint32_t ui_sms_send(const char *number, const char *text);
modem_send_state_t ui_sms_get_send_state(uint32_t send_id);
bool ui_sms_poll_received(modem_sms_rx_t *out);

/* Queue a raw AT command on the modem task; the reply goes to the monitor. */
void ui_modem_at(const char *cmd);

// shutdown
void ui_shutdown_on(void);

// [ screen 10 ] --- A7682E
bool ui_pcm5102_cb(const char *at_cmd);void ui_pcm5102_stop(void);
void ui_pcm5102_stop(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif
#endif
