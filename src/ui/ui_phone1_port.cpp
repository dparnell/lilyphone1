
#include "Arduino.h"
#include "ui_phone1_port.h"
#include "main.h"
#include "utilities.h"

#include "FS.h"
#include "SD.h"
#include "SPI.h"
#include <TinyGPS++.h>
#include "peripheral.h"
#include "WiFi.h"
#include "modem_service.h"
#include <Preferences.h>
#include <ctype.h>
#include <TouchDrvCSTXXX.hpp>


// extern 
extern TouchDrvCSTXXX touch;


volatile int default_language = DEFAULT_LANGUAGE_EN;
volatile bool default_keypad_light = false;
volatile bool default_motor_status = false;
volatile bool default_gps_status = true;
volatile bool default_lora_status = true;
volatile bool default_gyro_status = true;
volatile bool default_a7682_status = true;

// Notification preferences, persisted in NVS.
#define NOTIFY_PREFS_NAMESPACE "notify"
static bool notify_vibrate_call = true;
static bool notify_vibrate_text = true;
// Off by default: the only speaker on this board is the modem's, and whether
// one is fitted is not something the firmware can find out.
static bool notify_sound_text = false;
// ----

void ui_disp_full_refr(void)
{
    disp_full_refr();
}

void ui_disp_hibernate(void)
{
    disp_hibernate_now();
}
//************************************[ screen 0 ]****************************************** menu
//************************************[ screen 1 ]****************************************** lora

float ui_lora_get_freq(void)
{
    return LORA_FREQ;
}
int ui_lora_get_mode(void)
{
    return lora_get_mode();
}
void ui_lora_set_mode(int mode)
{
    lora_set_mode(mode);
}
void ui_lora_send(const char *str)
{
    lora_transmit(str);
}
void ui_lora_recv_loop(void)
{
    lora_receive_loop();
}
bool ui_lora_get_recv(const char **str, int *rssi)
{
    return lora_get_recv(str, rssi);
}
void ui_lora_set_recv_flag(void)
{
    lora_set_recv_flag();
}
//************************************[ screen 2 ]****************************************** setting
#if 1
// set function
// DEFAULT_LANGUAGE_CN、DEFAULT_LANGUAGE_EN
void ui_setting_set_language(int language)
{
    default_language = language;
}
void ui_setting_set_keypad_light(bool on)
{
    digitalWrite(BOARD_KEYBOARD_LED, on);
    default_keypad_light = on;
}
void ui_setting_set_motor_status(bool on)
{
    digitalWrite(BOARD_MOTOR_PIN, on);
    default_motor_status = on;
}
void ui_setting_set_gps_status(bool on)
{
    // enable GPS module power
    digitalWrite(BOARD_GPS_EN, on);
    default_gps_status = on;
}
void ui_setting_set_lora_status(bool on)
{
    // enable LORA module power
    digitalWrite(BOARD_LORA_EN, on);
    default_lora_status = on;
}
void ui_setting_set_gyro_status(bool on)
{
    // enable gyroscope module power
    digitalWrite(BOARD_1V8_EN, on);
    default_gyro_status = on;
}
void ui_setting_set_a7682_status(bool on)
{
    // enable 7682 module power
    digitalWrite(BOARD_6609_EN, on);
    digitalWrite(BOARD_A7682E_PWRKEY, on);
    default_a7682_status = on;
}

// get function
int ui_setting_get_language(void)
{
    return default_language;
}
bool ui_setting_get_keypad_light(void)
{
    return default_keypad_light;
}
bool ui_setting_get_motor_status(void)
{
    return default_motor_status;
}
bool ui_setting_get_gps_status(void)
{
    return default_gps_status;
}
bool ui_setting_get_lora_status(void)
{
    return default_lora_status;
}
bool ui_setting_get_gyro_status(void)
{
    return default_gyro_status;
}
bool ui_setting_get_a7682_status(void)
{
    return default_a7682_status;
}

// About System
const char *ui_setting_get_sf_ver(void)
{
    return UI_T_DECK_PRO_VERSION;
}
const char *ui_setting_get_hd_ver(void)
{
    return BOARD_T_DECK_PRO_VERSION;
}

void ui_setting_get_sd_capacity(uint64_t *total, uint64_t *used)
{
    if(ui_test_sd_card())
    {
        if(total)
            *total = SD.totalBytes() / (1024 * 1024);
        if(used)
            *used = SD.usedBytes() / (1024 * 1024);

        printf("total=%lluMB, used=%lluMB\n", *total, *used);

        uint64_t cardSize = SD.cardSize() / (1024 * 1024);
        Serial.printf("SD Card Size: %lluMB\n", cardSize);

        uint64_t totalSize = SD.totalBytes() / (1024 * 1024);
        Serial.printf("SD Card Total: %lluMB\n", totalSize);

        uint64_t usedSize = SD.usedBytes() / (1024 * 1024);
        Serial.printf("SD Card Used: %lluMB\n", usedSize);
    }
}

#endif
//************************************[ screen 3 ]****************************************** GPS
void ui_gps_task_suspend(void)
{
    gps_task_suspend();
}
void ui_gps_task_resume(void)
{
    gps_task_resume();
}
void ui_gps_get_coord(double *lat, double *lng)
{
    gps_get_coord(lat, lng);
}
void ui_gps_get_data(uint16_t *year, uint8_t *month, uint8_t *day)
{
    gps_get_data(year, month, day);
}
void ui_gps_get_time(uint8_t *hour, uint8_t *minute, uint8_t *second)
{
    gps_get_time(hour, minute, second);
}

void ui_gps_get_satellites(uint32_t *vsat)
{
    gps_get_satellites(vsat);
}
void ui_gps_get_speed(double *speed)
{
    gps_get_speed(speed);
}
//************************************[ screen 4 ]****************************************** Wifi Scan
int is_chinese_utf8(const char *str) {
    unsigned char c = (unsigned char)str[0];
    return (c >= 0xE0 && c <= 0xEF);  // 检查第一个字节是否在 UTF-8 的中文字符范围内
}

void ui_wifi_get_scan_info(ui_wifi_scan_info_t *list, int list_len)
{
    int n = WiFi.scanNetworks();
    if(n > list_len)
        n = list_len;
    
    memset(list, 0, (sizeof(*list) * list_len));
    for(int i = 0; i < n; i++)
    {
        const char *str = WiFi.SSID(i).c_str();
        if(is_chinese_utf8(str))
            continue;
        strncpy(list[i].name, WiFi.SSID(i).c_str(), 16);
        list[i].rssi = WiFi.RSSI(i);
    }
}
//************************************[ screen 5 ]****************************************** Test
bool ui_test_get(int peri_id)
{
    return peri_init_st[peri_id];
}
bool ui_test_sd_card(void) 
{
    return peri_init_st[E_PERI_SD];
}
bool ui_test_a7682e(void) 
{
    return peri_init_st[E_PERI_A7682E];
}
bool ui_test_pcm5102a(void)
{
    return false;
}

//************************************[ screen 6 ]****************************************** Battery
#if 1

// BQ25896
bool ui_battery_25896_is_vbus_in(void)
{
    return PPM.isVbusIn();
}

bool ui_batt_25896_is_chg(void)
{
    if(PPM.isCharging() == false) {
        return false;
    } else {
        return true;
    }
    // return true;
}
float ui_batt_25896_get_vbus(void)
{
    return (PPM.getVbusVoltage() *1.0 / 1000.0 );
    // return 4.5;
}
float ui_batt_25896_get_vsys(void)
{
    return (PPM.getSystemVoltage() * 1.0 / 1000.0);
    // return 4.5;
}
float ui_batt_25896_get_vbat(void)
{
    return (PPM.getBattVoltage() * 1.0 / 1000.0);
    // return 4.5;
}
float ui_batt_25896_get_volt_targ(void)
{
    return (PPM.getChargeTargetVoltage() * 1.0 / 1000.0);
    // return 4.5; 
}
float ui_batt_25896_get_chg_curr(void)
{
    return (PPM.getChargeCurrent());
    // return 4.5;
}
float ui_batt_25896_get_pre_curr(void)
{
    return (PPM.getPrechargeCurr());;
    // return 4.5;
}
const char * ui_batt_25896_get_chg_st(void)
{
    return PPM.getChargeStatusString();
    // return "hello";
}
const char * ui_batt_25896_get_vbus_st(void)
{
    return PPM.getBusStatusString();
    // return "hello";
}
const char * ui_batt_25896_get_ntc_st(void)
{
    return PPM.getNTCStatusString();
    // return "hello";
}
/* 27220 */
bool ui_battery_27220_is_vaild(void) {return peri_init_st[E_PERI_BQ27220]; }
bool ui_battery_27220_get_input(void) { return bq27220.getIsCharging();}
bool ui_battery_27220_get_charge_finish(void) { return bq27220.getCharingFinish();}
uint16_t ui_battery_27220_get_status(void) 
{
    BQ27220BatteryStatus batt;
    bq27220.getBatteryStatus(&batt);
    return batt.full;
}
uint16_t ui_battery_27220_get_voltage(void) { return bq27220.getVoltage(); }
int16_t ui_battery_27220_get_current(void) { return bq27220.getCurrent(); }
uint16_t ui_battery_27220_get_temperature(void) { return bq27220.getTemperature(); }
uint16_t ui_battery_27220_get_full_capacity(void) { return bq27220.getFullChargeCapacity(); }
uint16_t ui_battery_27220_get_design_capacity(void) { return bq27220.getDesignCapacity(); }
uint16_t ui_battery_27220_get_remain_capacity(void) { return bq27220.getRemainingCapacity(); }
uint16_t ui_battery_27220_get_percent(void) { return bq27220.getStateOfCharge(); }
uint16_t ui_battery_27220_get_health(void) { return bq27220.getStateOfHealth(); }
const char * ui_battert_27220_get_percent_level(void)
{
    int percent = bq27220.getStateOfCharge();
    const char * str = NULL;
    if(percent < 20)      str =  LV_SYMBOL_BATTERY_EMPTY;
    else if(percent < 40) str =  LV_SYMBOL_BATTERY_1;
    else if(percent < 65) str =  LV_SYMBOL_BATTERY_2;
    else if(percent < 90) str =  LV_SYMBOL_BATTERY_3;
    else                  str =  LV_SYMBOL_BATTERY_FULL;
    return str;
}
#endif


//************************************[ screen 8 ]****************************************** telephony

void ui_phone_dial(const char *number)
{
    Serial.printf("[PHONE] dialing %s\n", number);
    modem_dial(number);
}

void ui_phone_answer(void)
{
    modem_answer();
}

void ui_phone_hang_up(void)
{
    modem_hangup();
}

modem_call_state_t ui_phone_get_call_state(void)
{
    return modem_get_call_state();
}

void ui_phone_get_call_number(char *buf, int len)
{
    modem_get_call_number(buf, len);
}

uint32_t ui_phone_get_call_duration(void)
{
    return modem_get_call_duration();
}

bool ui_phone_is_registered(void)
{
    return modem_is_registered();
}

uint8_t ui_phone_get_signal(void)
{
    return modem_get_signal();
}

void ui_phone_get_operator(char *buf, int len)
{
    modem_get_operator(buf, len);
}

static void vibrate_off_cb(lv_timer_t *t)
{
    // Back to however the manual motor switch in the settings had it, rather
    // than blindly off, so a buzz does not cancel a deliberate motor test.
    digitalWrite(BOARD_MOTOR_PIN, default_motor_status ? HIGH : LOW);
    lv_timer_del(t);
}

void ui_phone_vibrate(int ms)
{
    digitalWrite(BOARD_MOTOR_PIN, HIGH);

    lv_timer_t *t = lv_timer_create(vibrate_off_cb, ms, NULL);
    lv_timer_set_repeat_count(t, 1);
}

//************************************[ notifications ]*************************
static void notify_save(void)
{
    Preferences prefs;
    if(!prefs.begin(NOTIFY_PREFS_NAMESPACE, false)) return;

    prefs.putBool("vib_call", notify_vibrate_call);
    prefs.putBool("vib_text", notify_vibrate_text);
    prefs.putBool("snd_text", notify_sound_text);
    prefs.end();
}

void ui_settings_load(void)
{
    Preferences prefs;
    if(!prefs.begin(NOTIFY_PREFS_NAMESPACE, true)) return;

    notify_vibrate_call = prefs.getBool("vib_call", notify_vibrate_call);
    notify_vibrate_text = prefs.getBool("vib_text", notify_vibrate_text);
    notify_sound_text   = prefs.getBool("snd_text", notify_sound_text);
    prefs.end();

    Serial.printf("[NOTIFY] call buzz %d, text buzz %d, text sound %d\n",
                  notify_vibrate_call, notify_vibrate_text, notify_sound_text);
}

void ui_setting_set_vibrate_call(bool on) { notify_vibrate_call = on; notify_save(); }
void ui_setting_set_vibrate_text(bool on) { notify_vibrate_text = on; notify_save(); }
void ui_setting_set_sound_text(bool on)   { notify_sound_text   = on; notify_save(); }

bool ui_setting_get_vibrate_call(void) { return notify_vibrate_call; }
bool ui_setting_get_vibrate_text(void) { return notify_vibrate_text; }
bool ui_setting_get_sound_text(void)   { return notify_sound_text; }

void ui_notify_incoming_call(void)
{
    if(notify_vibrate_call) ui_phone_vibrate(400);
}

void ui_notify_incoming_text(void)
{
    if(notify_vibrate_text) ui_phone_vibrate(250);

    // Not over the top of a call: the tone comes out of the same speaker path
    // the call is using.
    if(notify_sound_text && modem_get_call_state() == MODEM_CALL_IDLE) {
        modem_play_tone();
    }
}

//************************************[ screen 13 ]***************************************** messaging

uint32_t ui_sms_send(const char *number, const char *text)
{
    return modem_send_sms(number, text);
}

modem_send_state_t ui_sms_get_send_state(uint32_t send_id)
{
    return modem_get_send_state(send_id);
}

bool ui_sms_poll_received(modem_sms_rx_t *out)
{
    return modem_poll_sms(out);
}

void ui_modem_at(const char *cmd)
{
    modem_request_at(cmd);
}

//************************************[ screen 9 ]****************************************** Input

void ui_shutdown_on(void)
{
    disp_hibernate_now();
    PPM.shutdown();
    Serial.println("Shutdown .....");
}
