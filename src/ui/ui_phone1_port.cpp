
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
#include "mesh_net.h"
#include "mesh_companion.h"
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
volatile bool default_a7682_status = true;

/* The 1.8V sensor rail, as asked for rather than as it stands. The two differ:
 * LTR553_init() raises the rail to look for a sensor and puts it back down when
 * none answers, so the rail can be off while the setting is on. The switch
 * shows what was asked for - a switch that flips itself back is worse than one
 * that reports honestly - and ear detect asks sensor_rail_is_on() instead,
 * because that is a question about what can actually be read. */
static bool default_sensor_status = true;

static void power_save(void);

// Notification preferences, persisted in NVS.
#define NOTIFY_PREFS_NAMESPACE "notify"
/* Which modules are switched on, kept apart from the notification preferences
 * because these are read at the very top of setup() - before the display, the
 * filesystem or anything else - to decide what gets power at all. */
#define POWER_PREFS_NAMESPACE  "power"
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
    power_save();

    /* There is nothing to read from an unpowered receiver, and a task polling a
     * dead port only burns CPU and fills the log with "no fix". */
    if(!on) {
        gps_task_suspend();
        return;
    }

    /* A receiver that has just been given power knows nothing: it comes back at
     * its defaults, having forgotten the baud rate and the message set it was
     * configured with - and if it was switched off when the phone booted it was
     * never configured at all. So this is a full gps_init() rather than a
     * resume. It blocks for around a second, which is what a switch press can
     * afford and a timer could not.
     *
     * The task is suspended across it because that task is the only other user
     * of the serial port. Coming back, it is the mesh that decides whether the
     * receiver is wanted running. */
    gps_task_suspend();
    delay(150);            // let the module's supply come up before talking to it
    gps_init();

    if(mesh_net_wants_gps()) gps_task_resume();
}
void ui_setting_set_lora_status(bool on)
{
    // enable LORA module power
    digitalWrite(BOARD_LORA_EN, on);
    default_lora_status = on;
    power_save();

    /* A companion app connected to a node whose radio has just been switched
     * off is connected to something that can no longer send or hear anything.
     * The link goes down with the radio and comes back with it, without the
     * user's choice of link having changed in between. */
    mesh_companion_set_node_powered(on);

    // And the mesh task stops driving a radio that is not there.
    mesh_net_set_powered(on);
}
void ui_setting_set_sensor_status(bool on)
{
    default_sensor_status = on;
    power_save();

    // peri_ltr553.cpp owns the rail; it is the only part left on it.
    sensor_rail_set(on);
}
void ui_setting_set_a7682_status(bool on)
{
    // enable 7682 module power
    digitalWrite(BOARD_6609_EN, on);
    default_a7682_status = on;
    power_save();

    /* PWRKEY is a button, not a switch. The module starts on a pulse and stops
     * on a longer one, and the line left high is the button held down - so this
     * is the same sequence the boot path uses rather than a level. Without it a
     * modem that was switched off when the phone booted has power but was never
     * told to start, and answers nothing for the rest of the run. */
    if(on) {
        delay(10);
        digitalWrite(BOARD_A7682E_PWRKEY, LOW);
        delay(10);
        digitalWrite(BOARD_A7682E_PWRKEY, HIGH);
        delay(50);
        digitalWrite(BOARD_A7682E_PWRKEY, LOW);
    } else {
        digitalWrite(BOARD_A7682E_PWRKEY, LOW);
    }

    // The task that owns the serial port stops using it, and sets the modem up
    // again from scratch when it comes back.
    modem_set_powered(on);
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
bool ui_setting_get_sensor_status(void)
{
    return default_sensor_status;
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
bool ui_gps_reset(void)
{
    return gps_reset();
}
bool ui_gps_is_running(void)
{
    return gps_is_running();
}
bool ui_gps_has_fix(void)
{
    return gps_has_fix();
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
// What is still up, for the screens that need to know
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
static int autolock_choice;
static void notify_save(void);

// Off until it has been shown to work on this hardware; see ui_proximity_tick().
static bool prox_enabled = false;
// The modem's own network status LED; see ui_setting_set_netlight().
static bool netlight_on = true;

static void notify_save(void)
{
    Preferences prefs;
    if(!prefs.begin(NOTIFY_PREFS_NAMESPACE, false)) return;

    prefs.putBool("vib_call", notify_vibrate_call);
    prefs.putBool("vib_text", notify_vibrate_text);
    prefs.putBool("snd_text", notify_sound_text);
    prefs.putInt("autolock", autolock_choice);
    prefs.putBool("ear", prox_enabled);
    prefs.putBool("netlight", netlight_on);
    prefs.end();
}

/* The values the auto lock steps through. Off first, so a press from the
 * default lands on the shortest useful delay rather than the longest. */
static const struct {
    uint32_t    ms;
    const char *text;
} autolock_choices[] = {
    { 0,           "Off"   },
    { 30 * 1000,   "30 s"  },
    { 60 * 1000,   "1 min" },
    { 120 * 1000,  "2 min" },
    { 300 * 1000,  "5 min" },
};

uint32_t ui_setting_get_autolock_ms(void)
{
    return autolock_choices[autolock_choice].ms;
}

const char *ui_setting_autolock_text(void)
{
    return autolock_choices[autolock_choice].text;
}

void ui_setting_autolock_next(void)
{
    autolock_choice++;
    if(autolock_choice >= (int)(sizeof(autolock_choices) / sizeof(autolock_choices[0]))) {
        autolock_choice = 0;
    }
    notify_save();
}

static void power_save(void)
{
    Preferences prefs;
    if(!prefs.begin(POWER_PREFS_NAMESPACE, false)) return;

    prefs.putBool("gps",     default_gps_status);
    prefs.putBool("lora",    default_lora_status);
    prefs.putBool("modem",   default_a7682_status);
    prefs.putBool("sensors", default_sensor_status);
    prefs.end();
}

/* What was switched off last time.
 *
 * Called from the top of setup(), before any module is given power, because a
 * module that is meant to be off should never come up at all - not come up and
 * then be switched back down once the UI exists. Nothing here touches hardware:
 * main.cpp reads these back and decides what to power and what to skip.
 *
 * Everything defaults to on, so a phone with nothing stored behaves as it
 * always did. There is no crash latch on these the way there is on the
 * companion link, because the failure they can cause is the safe one: the worst
 * a remembered value does here is leave a module off, which is recoverable from
 * the settings screen. A remembered value that turns something *on* is the kind
 * that needs a latch.
 */
void ui_power_load(void)
{
    Preferences prefs;
    if(!prefs.begin(POWER_PREFS_NAMESPACE, true)) return;

    default_gps_status    = prefs.getBool("gps",     default_gps_status);
    default_lora_status   = prefs.getBool("lora",    default_lora_status);
    default_a7682_status  = prefs.getBool("modem",   default_a7682_status);
    default_sensor_status = prefs.getBool("sensors", default_sensor_status);
    prefs.end();

    Serial.printf("[POWER] remembered: gps %s, lora %s, modem %s, sensors %s\n",
                  default_gps_status    ? "on" : "OFF",
                  default_lora_status   ? "on" : "OFF",
                  default_a7682_status  ? "on" : "OFF",
                  default_sensor_status ? "on" : "OFF");
}

void ui_settings_load(void)
{
    Preferences prefs;
    if(!prefs.begin(NOTIFY_PREFS_NAMESPACE, true)) return;

    notify_vibrate_call = prefs.getBool("vib_call", notify_vibrate_call);
    notify_vibrate_text = prefs.getBool("vib_text", notify_vibrate_text);
    notify_sound_text   = prefs.getBool("snd_text", notify_sound_text);
    autolock_choice     = prefs.getInt("autolock", autolock_choice);
    prox_enabled        = prefs.getBool("ear", prox_enabled);
    netlight_on         = prefs.getBool("netlight", netlight_on);
    prefs.end();

    // Remembered from a board that had the sensor, or from before it was known
    // to be missing. Either way it cannot do anything here.
    if(!ui_setting_ear_detect_available()) prox_enabled = false;

    if(autolock_choice < 0 ||
       autolock_choice >= (int)(sizeof(autolock_choices) / sizeof(autolock_choices[0]))) {
        autolock_choice = 0;
    }

    Serial.printf("[NOTIFY] call buzz %d, text buzz %d, text sound %d, auto lock %s\n",
                  notify_vibrate_call, notify_vibrate_text, notify_sound_text,
                  ui_setting_autolock_text());
}

//************************************[ ear detect ]****************************
/* Ignoring the touch panel while the phone is against your face.
 *
 * A capacitive panel cannot tell a cheek from a fingertip, so a call held to the
 * ear is a call being hung up, muted and dialled into by the side of your head.
 * The LTR-553ALS has a proximity channel a few centimetres deep, which is what
 * every phone uses to solve this.
 *
 * Three things keep a misreading from being serious:
 *
 * - It only ever runs while a call is connected. A sensor stuck reporting
 *   "near" cannot lock the phone up, because outside a call nothing asks it.
 * - It suppresses the touch panel only. The keyboard still works, so there is
 *   always a way to end the call - a physical key needs real travel, where a
 *   capacitive panel only needs skin.
 * - The threshold is relative to a baseline taken when the call starts, and the
 *   baseline follows the reading down while nothing is near. Cover glass
 *   crosstalk and ambient infrared both offset the raw count, and neither is
 *   the same on two devices or in two rooms.
 *
 * The numbers below are a starting point rather than a measurement. What the
 * sensor actually reads against a face is logged during a call so it can be
 * tuned against the real thing.
 */
#define PROX_NEAR_RISE   100   // counts above the baseline before it is an ear
#define PROX_FAR_RISE     50   // and back under this before it is not
#define PROX_MIN_NEAR    100   // an absolute floor, whatever the baseline says

static bool     prox_near     = false;
static uint16_t prox_baseline = 0;
static bool     prox_suppress = false;   // ...and whether it is being acted on
static bool     prox_have_base = false;
static bool     prox_was_in_call = false;
static uint32_t prox_logged   = 0;

void ui_setting_set_ear_detect(bool on)
{
    prox_enabled   = on;
    prox_have_base = false;              // take a fresh baseline where it stands
    if(!on) prox_near = prox_suppress = false;   // never leave the panel dead
    notify_save();

    Serial.printf("[PROX] ear detect %s\n", on ? "on" : "off");
}

bool ui_setting_get_ear_detect(void) { return prox_enabled; }

bool ui_setting_ear_detect_available(void)
{
    /* The sensor sits on the 1.8V rail that the sensor switch controls, so
     * turning that off takes the proximity sensor with it. */
    return peri_init_st[E_PERI_LTR_553ALS] && sensor_rail_is_on();
}

/* Shown as a value rather than a switch, so the row can report that there is
 * nothing to switch. On this board the LTR-553ALS does not answer at all -
 * neither cold nor with its supply forced up - so the setting would otherwise
 * be one that turns on and does nothing. */
const char *ui_setting_ear_detect_text(void)
{
    if(!ui_setting_ear_detect_available()) return "No sensor";

    return prox_enabled ? "On" : "Off";
}

void ui_setting_ear_detect_next(void)
{
    if(!ui_setting_ear_detect_available()) return;

    ui_setting_set_ear_detect(!prox_enabled);
}

bool ui_touch_suppressed(void) { return prox_suppress; }

/* Watching the sensor whenever the setting is on, rather than only during a
 * call.
 *
 * Sampling only during a call was the obvious way round and the wrong one: the
 * numbers these thresholds have to be set against could then only be seen by
 * ringing somebody, and if nothing appeared there was no way to tell which of
 * the preconditions had failed. Reading it always makes the sensor testable by
 * waving a hand at the phone. Acting on the reading is still confined to a
 * connected call, which is the part that matters for safety.
 */
void ui_proximity_tick(void)
{
    if(!prox_enabled) {
        prox_near = prox_suppress = false;
        prox_have_base = false;
        return;
    }

    if(!ui_setting_ear_detect_available()) {
        prox_suppress = false;

        // Once. A sensor that is not fitted will not become fitted, and saying
        // so every few seconds for the life of the boot is just noise.
        static bool warned = false;
        if(!warned) {
            warned = true;
            Serial.printf("[PROX] ear detect needs the light sensor, and %s\n",
                          LTR553_status());
        }
        return;
    }

    uint16_t ps      = LTR_553ALS_get_ps();
    bool     in_call = (ui_phone_get_call_state() == MODEM_CALL_ACTIVE);

    /* A fresh baseline when the setting is switched on, and again when a call
     * connects: both are moments when the phone is being looked at rather than
     * held against a face. */
    if(!prox_have_base || (in_call && !prox_was_in_call)) {
        prox_have_base = true;
        prox_baseline  = ps;
        prox_near      = false;
    }
    prox_was_in_call = in_call;

    if(prox_near) {
        if(ps < prox_baseline + PROX_FAR_RISE) prox_near = false;
    } else {
        if(ps < prox_baseline) prox_baseline = ps;   // follow the drift down
        if(ps > prox_baseline + PROX_NEAR_RISE && ps > PROX_MIN_NEAR) prox_near = true;
    }

    prox_suppress = prox_near && in_call;

    // Once a second, so the thresholds above can be set against what the sensor
    // really reads rather than what it ought to.
    if(millis() - prox_logged > 1000) {
        prox_logged = millis();
        Serial.printf("[PROX] %u (baseline %u) %s%s\n",
                      (unsigned)ps, (unsigned)prox_baseline,
                      prox_near ? "near" : "clear",
                      prox_suppress ? ", touch off" : (in_call ? ", in call" : ""));
    }
}

//************************************[ modem net light ]***********************
/* The blinking LED on the modem module.
 *
 * It is the module's network status indicator, driven by the modem's own
 * firmware rather than by anything here - there is no pin on the ESP32 for it,
 * so the only way to reach it is an AT command. AT+CNETLIGHT is SIMCom's
 * command for this and is documented for their earlier modules; whether this
 * one honours it is not something the datasheet to hand answers, so the request
 * goes down the normal path and the modem's reply is logged either way.
 *
 * Sent again whenever the modem comes back on the network, because a setting
 * like this is volatile on some modules and survives on others, and sending it
 * twice costs nothing.
 */
void ui_setting_set_netlight(bool on)
{
    netlight_on = on;
    notify_save();

    modem_request_at(on ? "AT+CNETLIGHT=1" : "AT+CNETLIGHT=0");
    Serial.printf("[MODEM] asked for the network light %s\n", on ? "on" : "off");
}

bool ui_setting_get_netlight(void) { return netlight_on; }

/* Asking the module what it can do about its LEDs.
 *
 * Its own row rather than a side effect of the switch above, because that only
 * fired on the off-edge - and with the setting remembered as off, pressing it
 * turned the light back on and surveyed nothing. A diagnostic that runs on one
 * edge of a persisted toggle is a diagnostic that will not run when you want
 * it. Temporary: it goes once the right command is known. */
const char *ui_modem_led_probe_text(void)
{
    return "Run";
}

void ui_modem_led_probe(void)
{
    Serial.println("[MODEM] LED probe requested");
    modem_request_led_survey();
}

void ui_netlight_apply(void)
{
    // Only worth saying when it is not what the module does by default.
    if(netlight_on) return;

    modem_request_at("AT+CNETLIGHT=0");
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

    if(!notify_sound_text) return;

    // Not over the top of a call: the tone comes out of the same speaker path
    // the call is using.
    if(modem_get_call_state() != MODEM_CALL_IDLE) {
        Serial.println("[NOTIFY] no text tone during a call");
        return;
    }

    Serial.println("[NOTIFY] playing the text tone");
    modem_play_tone();
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
