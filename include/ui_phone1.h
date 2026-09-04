#ifndef __UI_PHONE1_H__
#define __UI_PHONE1_H__

#ifdef __cplusplus
extern "C" {
#endif
/*********************************************************************************
 *                                  INCLUDES
 * *******************************************************************************/
#include "lvgl.h"
#include "ui_scr_mrg.h"
#include "phone_store.h"

/*********************************************************************************
 *                                   MACROS
 * *******************************************************************************/
#define DECKPRO_COLOR_BG        lv_color_white()
#define DECKPRO_COLOR_FG        lv_color_black()
#define UI_SLIDING_DISTANCE     40
#define UI_WIFI_SCAN_ITEM_MAX   13

/*********************************************************************************
 *                                  TYPEDEFS
 * *******************************************************************************/
enum {
    SCREEN0_ID = 0, // menu
    SCREEN1_ID,
    SCREEN1_1_ID,
    SCREEN1_2_ID,
    SCREEN2_ID,    // config
    SCREEN2_1_ID,  // time
    SCREEN2_2_ID,  // settings
    SCREEN3_ID,    // gps
    SCREEN4_ID,    // wifi
    SCREEN4_1_ID,  // wifi settings
    SCREEN4_2_ID,  // wifi scan
    SCREEN5_ID,    // test
    SCREEN6_ID,    // battery
    SCREEN6_1_ID,
    SCREEN6_2_ID,
    SCREEN8_ID,   // dial screen
    SCREEN8_1_ID, // in-call screen
    SCREEN9_ID,   // shutdown
    SCREEN11_ID,  // sleep
    SCREEN12_ID,   // contacts
    SCREEN12_1_ID, // contact details
    SCREEN12_2_ID, // contact editor
    SCREEN13_ID,   // messages, one row per conversation
    SCREEN13_1_ID, // conversation
    SCREEN13_2_ID, // compose
};

/* Why the contacts list was opened. When it is a pick the list hands a number
 * back to the screen underneath instead of opening the contact. */
enum {
    UI_PICK_NONE = 0,
    UI_PICK_DIAL,
    UI_PICK_COMPOSE,
};

typedef void (*ui_indev_read_cb)(int);

enum {
    TASKBAR_ID_CHARGE,
    TASKBAR_ID_CHARGE_FINISH,
    TASKBAR_ID_BATTERY_PERCENT,
    TASKBAR_ID_WIFI,
    TASKBAR_ID_SIGNAL,
    TASKBAR_ID_UNREAD,
    TASKBAR_ID_MAX,
};

struct menu_btn {
    uint16_t idx;
    const void *icon;   // bitmap icon, or NULL to fall back to `symbol`
    const char *symbol; // LV_SYMBOL_* drawn in its place
    const char *name;
    lv_coord_t pos_x;
    lv_coord_t pos_y; 
};

enum{
    UI_SETTING_TYPE_SW,
    UI_SETTING_TYPE_SUB,
};

typedef struct _ui_setting
{
    const char *name;
    int type;
    void (*set_cb)(bool);
    bool (*get_cb)(void);
    int sub_id;
    lv_obj_t *obj;
    lv_obj_t *st;
} ui_setting_handle;

typedef struct _ui_test {
    const char *name;
    int peri_id;
    lv_obj_t *obj;
    lv_obj_t *st;
    bool (*cb)(int);
} ui_test_handle;

typedef struct _ui_a7682 {
    const char *name;
    lv_obj_t *obj;
    lv_obj_t *st;
    bool (*cb)(const char *at_cmd);
} ui_a7682_handle;

typedef struct {
    char name[16];
    int rssi;
}ui_wifi_scan_info_t;

/*********************************************************************************
 *                              GLOBAL PROTOTYPES
 * *******************************************************************************/
void ui_phone1_entry(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif
#endif
