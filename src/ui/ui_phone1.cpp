
#include "ui_phone1.h"
#include "assets.h"
#include "stdio.h"
#include "ui_phone1_port.h"
#include "system_clock.h"
#include "timezone_db.h"
#include "Arduino.h"

#define SETTING_PAGE_MAX_ITEM 7
#define GET_BUFF_LEN(a) sizeof(a)/sizeof(a[0])

#define FONT_BOLD_SIZE_14 &Font_Mono_Bold_14
#define FONT_BOLD_SIZE_15 &Font_Mono_Bold_15
#define FONT_BOLD_SIZE_16 &Font_Mono_Bold_16
#define FONT_BOLD_SIZE_17 &Font_Mono_Bold_17
#define FONT_BOLD_SIZE_18 &Font_Mono_Bold_18
#define FONT_BOLD_SIZE_19 &Font_Mono_Bold_19

#define FONT_BOLD_MONO_SIZE_14 &Font_Mono_Bold_14
#define FONT_BOLD_MONO_SIZE_15 &Font_Mono_Bold_15
#define FONT_BOLD_MONO_SIZE_16 &Font_Mono_Bold_16
#define FONT_BOLD_MONO_SIZE_17 &Font_Mono_Bold_17
#define FONT_BOLD_MONO_SIZE_18 &Font_Mono_Bold_18
#define FONT_BOLD_MONO_SIZE_19 &Font_Mono_Bold_19

#define GLOBAL_BUF_LEN 30
static char global_buf[GLOBAL_BUF_LEN];

static lv_timer_t *touch_chk_timer = NULL;
static lv_timer_t *taskbar_update_timer = NULL;
static lv_obj_t *label_list[10] = {0};
uint16_t taskbar_statue[TASKBAR_ID_MAX] = {0};

//************************************[ Other fun ]******************************************
#if 1
static lv_obj_t *scr_back_btn_create(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t * btn = lv_btn_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_height(btn, 30);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 3, 3);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label2 = lv_label_create(btn);
    lv_obj_align(label2, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_color(label2, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(label2, LV_SYMBOL_LEFT);

    // Every caller already passes the screen name, so show it: without a title
    // the deeper screens were only identifiable by their contents.
    if(text && text[0]) {
        lv_obj_t *title = lv_label_create(parent);
        lv_obj_set_style_text_font(title, FONT_BOLD_SIZE_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(title, DECKPRO_COLOR_FG, LV_PART_MAIN);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        // A height as well as a width, or a title longer than the bar wraps to
        // a second line and lands on top of the screen content below it.
        lv_obj_set_size(title, LV_HOR_RES - 80, 18);
        lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_label_set_text(title, text);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);
    }

    lv_obj_t *label = lv_label_create(parent);
    lv_obj_align_to(label, label2, LV_ALIGN_OUT_RIGHT_MID, 5, -1);
    lv_obj_set_style_text_font(label, FONT_BOLD_MONO_SIZE_15, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(label, text);
    lv_obj_add_flag(label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(label, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_ext_click_area(label, 20);

    return label;
}

static const char *line_full_format(int max_c, const char *str1, const char *str2)
{
    int len1 = 0, len2 = 0;
    int j;

    len1 = strlen(str1);

    strncpy(global_buf, str1, len1);

    len2 = strlen(str2);
    for(j = len1; j < max_c -1 - len2; j++){
        global_buf[j] = ' ';
    }
    strncpy(global_buf + j, str2, len2);
    j = j + len2;
    
    global_buf[j] = '\0'; 

    printf("[%d] buf: %s\n", __LINE__, global_buf);

    return (const char *)global_buf;
}

/* Modal confirmation over whatever screen is showing.
 *
 * Deleting is one tap and there is no undo, so every delete goes through here.
 * The button map is file scope on purpose: lv_btnmatrix_set_map stores the
 * array by pointer rather than copying it, and the dialog outlives the call
 * that built it. Only one confirmation is ever up at a time. */
static const char *ui_confirm_btns[3];
static void (*ui_confirm_action)(void) = NULL;

static void ui_confirm_event(lv_event_t *e)
{
    lv_obj_t *mbox = lv_event_get_current_target(e);
    uint16_t  id   = lv_msgbox_get_active_btn(mbox);

    void (*action)(void) = ui_confirm_action;
    ui_confirm_action = NULL;

    lv_msgbox_close(mbox);

    if(id == 0 && action) action();
    ui_disp_full_refr();
}

static void ui_confirm(const char *title, const char *body, const char *confirm_text,
                       void (*action)(void))
{
    ui_confirm_btns[0] = confirm_text;
    ui_confirm_btns[1] = "Cancel";
    ui_confirm_btns[2] = "";
    ui_confirm_action  = action;

    lv_obj_t *mbox = lv_msgbox_create(NULL, title, body, ui_confirm_btns, false);
    lv_obj_set_width(mbox, lv_pct(88));
    lv_obj_set_style_bg_color(mbox, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_text_color(mbox, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_border_color(mbox, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_border_width(mbox, 2, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(mbox, 0, LV_PART_MAIN);
    lv_obj_set_style_text_font(mbox, FONT_BOLD_SIZE_15, LV_PART_MAIN);
    lv_obj_center(mbox);

    // The default modal backdrop is half-transparent black, which on a 1bpp
    // panel dithers into noise. An opaque box with a hard border reads better.
    lv_obj_set_style_bg_opa(lv_obj_get_parent(mbox), LV_OPA_TRANSP, LV_PART_MAIN);

    lv_obj_add_event_cb(mbox, ui_confirm_event, LV_EVENT_VALUE_CHANGED, NULL);
    ui_disp_full_refr();
}

/* A button in the top right of a screen, opposite the back button. */
static lv_obj_t *scr_action_btn_create(lv_obj_t *parent, const char *symbol, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 34, 30);
    lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, -3, 3);
    lv_obj_set_style_bg_color(btn, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(btn);
    lv_obj_center(label);
    lv_obj_set_style_text_color(label, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(label, symbol);

    return btn;
}

/* The scrolling list body every app screen uses below its title bar. */
static lv_obj_t *scr_app_list_create(lv_obj_t *parent)
{
    lv_obj_t *list = lv_list_create(parent);
    lv_obj_set_size(list, lv_pct(96), LV_VER_RES - 36);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_all(list, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(list, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(list, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(list, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    return list;
}

/* One tappable row: a bold first line and, optionally, a quieter second one.
 * `badge` is drawn at the right of the first line - used for timestamps. */
static lv_obj_t *scr_row_create(lv_obj_t *list, const char *title, const char *subtitle,
                                const char *badge, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *row = lv_btn_create(list);
    lv_obj_set_size(row, lv_pct(100), subtitle ? 46 : 34);
    lv_obj_set_style_bg_color(row, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_text_color(row, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_outline_width(row, 1, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 4, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    // Both labels get an explicit height, not just a width. LV_LABEL_LONG_DOT
    // wraps the text and only writes the ellipsis once it overflows the
    // label's *height*, so a label left at LV_SIZE_CONTENT grows to fit the
    // whole string and spills out over the rest of the list.
    // A single glyph badge such as a tick only needs a sliver; a timestamp needs
    // a column. Reserving the column either way would cut titles short for no
    // reason - zone names run to thirty characters.
    lv_coord_t badge_w = 0;
    if(badge) badge_w = (strlen(badge) <= 4) ? 24 : 64;

    lv_obj_t *first = lv_label_create(row);
    lv_obj_set_style_text_font(first, FONT_BOLD_SIZE_15, LV_PART_MAIN);
    lv_label_set_long_mode(first, LV_LABEL_LONG_DOT);
    lv_obj_set_size(first, 208 - badge_w, 18);
    lv_label_set_text(first, title);
    lv_obj_align(first, subtitle ? LV_ALIGN_TOP_LEFT : LV_ALIGN_LEFT_MID, 2, subtitle ? 1 : 0);

    if(subtitle) {
        lv_obj_t *second = lv_label_create(row);
        lv_obj_set_style_text_font(second, FONT_BOLD_SIZE_14, LV_PART_MAIN);
        lv_label_set_long_mode(second, LV_LABEL_LONG_DOT);
        lv_obj_set_size(second, 208, 16);
        lv_label_set_text(second, subtitle);
        lv_obj_align(second, LV_ALIGN_BOTTOM_LEFT, 2, -1);
    }

    if(badge) {
        lv_obj_t *tag = lv_label_create(row);
        lv_obj_set_style_text_font(tag, FONT_BOLD_SIZE_14, LV_PART_MAIN);
        lv_label_set_text(tag, badge);
        lv_obj_align(tag, subtitle ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_RIGHT_MID, -2, subtitle ? 1 : 0);
    }

    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user_data);
    return row;
}

/* Explains an empty list rather than leaving the user staring at blank paper. */
static lv_obj_t *scr_empty_note_create(lv_obj_t *parent, const char *text)
{
    lv_obj_t *note = lv_label_create(parent);
    lv_obj_set_width(note, lv_pct(90));
    lv_obj_set_style_text_font(note, FONT_BOLD_SIZE_15, LV_PART_MAIN);
    lv_obj_set_style_text_color(note, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_pad_top(note, 60, LV_PART_MAIN);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_label_set_text(note, text);
    return note;
}

/* A row of wide buttons across the bottom of a screen. */
static lv_obj_t *scr_action_bar_create(lv_obj_t *parent, lv_coord_t height)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, lv_pct(96), height);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_color(bar, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(bar, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(bar, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    return bar;
}

static lv_obj_t *scr_bar_btn_create(lv_obj_t *bar, const char *text, lv_coord_t width,
                                    lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_btn_create(bar);
    lv_obj_set_size(btn, width, 34);
    lv_obj_set_style_bg_color(btn, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_text_color(btn, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);

    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_font(label, FONT_BOLD_SIZE_15, LV_PART_MAIN);
    lv_label_set_text(label, text);
    lv_obj_center(label);

    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    return btn;
}

/* A labelled single-line text field. */
static lv_obj_t *scr_field_create(lv_obj_t *parent, const char *label_text, lv_coord_t y,
                                  const char *value, int max_len)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, FONT_BOLD_SIZE_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(label, label_text);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 10, y);

    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, max_len);
    lv_obj_set_width(ta, lv_pct(92));
    lv_obj_set_style_text_font(ta, FONT_BOLD_SIZE_16, LV_PART_MAIN);
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, y + 16);
    if(value && value[0]) lv_textarea_set_text(ta, value);

    return ta;
}
#endif
//************************************[ screen 0 ]****************************************** menu
#if 1
#define MENU_BTN_NUM (sizeof(menu_btn_list) / sizeof(menu_btn_list[0]))

static ui_indev_read_cb ui_get_gesture_dir = NULL;

static lv_obj_t *menu_screen1;
static lv_obj_t *menu_screen2;
static lv_obj_t *ui_Panel4;

static lv_obj_t * menu_taskbar = NULL;
static lv_obj_t * menu_taskbar_time = NULL;
static lv_obj_t * menu_taskbar_charge = NULL;
static lv_obj_t * menu_taskbar_battery = NULL;
static lv_obj_t * menu_taskbar_battery_percent = NULL;
static lv_obj_t * menu_taskbar_wifi = NULL;
static lv_obj_t * menu_taskbar_signal = NULL;
static lv_obj_t * menu_taskbar_unread = NULL;

static int page_num = 0;
static int page_curr = 0;

/* The phone apps lead, so the things you reach for every day are on the first
 * page; the utilities and the two power actions follow. */
static struct menu_btn menu_btn_list[] = 
{
    {SCREEN8_ID,  &img_A7682E,  NULL,                "Phone" ,   23,     13},  // Page one
    {SCREEN12_ID, NULL,         LV_SYMBOL_LIST,      "Contacts", 95,     13},
    {SCREEN13_ID, NULL,         LV_SYMBOL_ENVELOPE,  "Messages", 167,    13},
    {SCREEN2_ID,  &img_setting, NULL,                "Setting",  23,     101},
    {SCREEN3_ID,  &img_GPS,     NULL,                "GPS",      95,     101},
    {SCREEN4_ID,  &img_wifi,    NULL,                "Wifi",     167,    101},
    {SCREEN1_ID,  &img_lora,    NULL,                "Lora",     23,     189},
    {SCREEN6_ID,  &img_batt,    NULL,                "Battery",  95,     189},
    {SCREEN5_ID,  &img_test,    NULL,                "Test",     167,    189},

    {SCREEN11_ID, &img_PCM5102, NULL,                "Sleep",    23,     13},  // Page two
    {SCREEN9_ID,  NULL,         LV_SYMBOL_POWER,     "Shutdown", 95,     13},
};

static void menu_btn_event_cb(lv_event_t *e)
{
    struct menu_btn *tgr = (struct menu_btn *)e->user_data;
    scr_mgr_push(tgr->idx, false);
}

static void menu_get_gesture_dir(int dir)
{
    if(MENU_BTN_NUM <= 9) return;

    if(dir == LV_DIR_LEFT) {
        if(page_curr < page_num){
            page_curr++;
            // ui_disp_full_refr();
        }
        else{
            return ;
        }
    } else if(dir == LV_DIR_RIGHT) {
        if(page_curr > 0){
            page_curr--;
        }
        else{
            return ;
        }
    }   

    if(page_curr == 1) {
        lv_obj_clear_flag(menu_screen2, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(menu_screen1, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(lv_obj_get_child(ui_Panel4, 0), lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(lv_obj_get_child(ui_Panel4, 1), lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);

    } else if(page_curr == 0) {
        lv_obj_clear_flag(menu_screen1, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(menu_screen2, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(lv_obj_get_child(ui_Panel4, 0), lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(lv_obj_get_child(ui_Panel4, 1), lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

static void menu_btn_create(lv_obj_t *parent, struct menu_btn *info)
{
    lv_obj_t * btn = lv_btn_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_width(btn, 50);
    lv_obj_set_height(btn, 50);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_OVERFLOW_VISIBLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);     /// Flags
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_radius(btn, 18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_spread(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, 255, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_outline_width(btn, 3, LV_PART_MAIN | LV_STATE_PRESSED);

    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_font(label, FONT_BOLD_MONO_SIZE_14, LV_PART_MAIN);
    lv_obj_set_width(label, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_height(label, LV_SIZE_CONTENT);    /// 1
    lv_obj_set_x(label, 0);
    lv_obj_set_y(label, 20);
    lv_obj_set_align(label, LV_ALIGN_BOTTOM_MID);
    lv_obj_set_style_text_color(label, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(label, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_set_x(btn, info->pos_x);
    lv_obj_set_y(btn, info->pos_y);
    if(info->icon) {
        lv_obj_set_style_bg_img_src(btn, info->icon, LV_PART_MAIN | LV_STATE_DEFAULT);
    } else {
        // A symbol works as a background image source, but it is drawn with the
        // button's own text font - which the child label overrides for itself.
        lv_obj_set_style_text_font(btn, &lv_font_montserrat_26, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_img_src(btn, info->symbol, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    lv_label_set_text(label, (info->name));
    lv_obj_set_style_border_width(label, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(btn, menu_btn_event_cb, LV_EVENT_CLICKED, (void *)info);
}

static void create0(lv_obj_t *parent) 
{
    int status_bar_height = 25;

    menu_taskbar = lv_obj_create(parent);
    lv_obj_set_size(menu_taskbar, LV_HOR_RES, status_bar_height);
    lv_obj_set_style_pad_all(menu_taskbar, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(menu_taskbar, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(menu_taskbar, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(menu_taskbar, LV_OBJ_FLAG_SCROLLABLE);
    

    menu_taskbar_time = lv_label_create(menu_taskbar);
    lv_obj_set_style_border_width(menu_taskbar_time, 0, 0);
    lv_label_set_text(menu_taskbar_time, "XX:XX");
    lv_obj_set_style_text_font(menu_taskbar_time, &Font_Mono_Bold_14, LV_PART_MAIN);
    lv_obj_align(menu_taskbar_time, LV_ALIGN_LEFT_MID, 10, 0);

    lv_obj_t *status_parent = lv_obj_create(menu_taskbar);
    lv_obj_set_size(status_parent, lv_pct(80)-2, status_bar_height-2);
    lv_obj_set_style_pad_all(status_parent, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(status_parent, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(status_parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_parent, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(status_parent, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(status_parent, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(status_parent, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(status_parent, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(status_parent, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_column(status_parent, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_scrollbar_mode(status_parent, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(status_parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(status_parent, LV_ALIGN_RIGHT_MID, 0, 0);

    // An envelope for anything unread and a handset once the network has the
    // phone registered: the two things you want to know without opening an app.
    menu_taskbar_unread = lv_label_create(status_parent);
    lv_label_set_text_fmt(menu_taskbar_unread, "%s", LV_SYMBOL_ENVELOPE);
    lv_obj_add_flag(menu_taskbar_unread, LV_OBJ_FLAG_HIDDEN);

    menu_taskbar_signal = lv_label_create(status_parent);
    lv_label_set_text_fmt(menu_taskbar_signal, "%s", LV_SYMBOL_CALL);
    lv_obj_add_flag(menu_taskbar_signal, LV_OBJ_FLAG_HIDDEN);

    menu_taskbar_wifi = lv_label_create(status_parent);
    lv_label_set_text_fmt(menu_taskbar_wifi, "%s", LV_SYMBOL_WIFI);
    lv_obj_add_flag(menu_taskbar_wifi, LV_OBJ_FLAG_HIDDEN);

    menu_taskbar_charge = lv_label_create(status_parent);
    lv_label_set_text_fmt(menu_taskbar_charge, "%s", LV_SYMBOL_CHARGE);
    lv_obj_add_flag(menu_taskbar_charge, LV_OBJ_FLAG_HIDDEN);

    if(taskbar_statue[TASKBAR_ID_WIFI])
        lv_obj_clear_flag(menu_taskbar_wifi, LV_OBJ_FLAG_HIDDEN);

    if(taskbar_statue[TASKBAR_ID_CHARGE])
        lv_obj_clear_flag(menu_taskbar_charge, LV_OBJ_FLAG_HIDDEN);

    menu_taskbar_battery = lv_label_create(status_parent);
    
    menu_taskbar_battery_percent = lv_label_create(status_parent);
    lv_obj_set_style_text_font(menu_taskbar_battery_percent, &Font_Mono_Bold_14, LV_PART_MAIN);

    //
    page_num = MENU_BTN_NUM / 9;

    menu_screen1 = lv_obj_create(parent);
    lv_obj_set_size(menu_screen1, lv_pct(100), LV_VER_RES - status_bar_height);
    lv_obj_set_style_bg_color(menu_screen1, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(menu_screen1, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(menu_screen1, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(menu_screen1, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_border_side(menu_screen1, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(menu_screen1, 0, LV_PART_MAIN);
    lv_obj_align(menu_screen1, LV_ALIGN_BOTTOM_MID, 0, 0);
    // lv_obj_add_flag(menu_screen1, LV_OBJ_FLAG_HIDDEN);

    menu_screen2 = lv_obj_create(parent);
    lv_obj_set_size(menu_screen2, lv_pct(100), LV_VER_RES - status_bar_height);
    lv_obj_set_style_bg_color(menu_screen2, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(menu_screen2, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(menu_screen2, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(menu_screen2, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_border_side(menu_screen2, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(menu_screen2, 0, LV_PART_MAIN);
    lv_obj_align(menu_screen2, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(menu_screen2, LV_OBJ_FLAG_HIDDEN);

    for(int i = 0; i < MENU_BTN_NUM; i++) {
        if(i < 9) {
            menu_btn_create(menu_screen1, &menu_btn_list[i]);
        } else {
            menu_btn_create(menu_screen2, &menu_btn_list[i]);
        }
    }

    if(MENU_BTN_NUM > 9) {
        ui_Panel4 = lv_obj_create(parent);
        lv_obj_set_width(ui_Panel4, 240);
        lv_obj_set_height(ui_Panel4, 25);
        lv_obj_set_align(ui_Panel4, LV_ALIGN_BOTTOM_MID);
        lv_obj_set_flex_flow(ui_Panel4, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(ui_Panel4, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_clear_flag(ui_Panel4, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
        lv_obj_set_style_radius(ui_Panel4, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(ui_Panel4, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(ui_Panel4, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(ui_Panel4, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_width(ui_Panel4, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_spread(ui_Panel4, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(ui_Panel4, 0, LV_PART_SCROLLBAR | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_width(ui_Panel4, 0, LV_PART_SCROLLBAR | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_spread(ui_Panel4, 0, LV_PART_SCROLLBAR | LV_STATE_DEFAULT);

        lv_obj_t *ui_Button11 = lv_btn_create(ui_Panel4);
        lv_obj_set_width(ui_Button11, 10);
        lv_obj_set_height(ui_Button11, 10);
        lv_obj_add_flag(ui_Button11, LV_OBJ_FLAG_SCROLL_ON_FOCUS);     /// Flags
        lv_obj_clear_flag(ui_Button11, LV_OBJ_FLAG_CHECKABLE);      /// Flags
        lv_obj_set_style_radius(ui_Button11, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(ui_Button11, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(ui_Button11, DECKPRO_COLOR_FG, LV_PART_MAIN | LV_STATE_DEFAULT);

        lv_obj_t *ui_Button12 = lv_btn_create(ui_Panel4);
        lv_obj_set_width(ui_Button12, 10);
        lv_obj_set_height(ui_Button12, 10);
        lv_obj_add_flag(ui_Button12, LV_OBJ_FLAG_SCROLL_ON_FOCUS);     /// Flags
        lv_obj_clear_flag(ui_Button12, LV_OBJ_FLAG_CHECKABLE);      /// Flags
        lv_obj_set_style_radius(ui_Button12, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(ui_Button12, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

static void entry0(void) {
    ui_get_gesture_dir = menu_get_gesture_dir;
    lv_timer_resume(touch_chk_timer);
    lv_timer_resume(taskbar_update_timer);

    lv_label_set_text_fmt(menu_taskbar_battery, "%s", ui_battert_27220_get_percent_level());

    lv_label_set_text_fmt(menu_taskbar_battery_percent, "%d", ui_battery_27220_get_percent());
}
static void exit0(void) {
    ui_get_gesture_dir = NULL;
    lv_timer_pause(touch_chk_timer);
    lv_timer_pause(taskbar_update_timer);
}
static void destroy0(void) {
    if(menu_taskbar) {
        lv_obj_del(menu_taskbar);
        menu_taskbar = NULL;
    }
}

static scr_lifecycle_t screen0 = {
    .create = create0,
    .entry = entry0,
    .exit  = exit0,
    .destroy = destroy0,
};
#endif
//************************************[ screen 1 ]****************************************** lora
// --------------------- screen 1 --------------------- lora
#if 1
lv_obj_t * scr1_list;
static lv_obj_t *scr1_lab_buf[20];

static void scr1_list_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * obj = lv_event_get_target(e);
    for(int i = 0; i < lv_obj_get_child_cnt(obj); i++) 
    {
        lv_obj_t * child = lv_obj_get_child(obj, i);
        if(lv_obj_check_type(child, &lv_label_class)) {
            char *str = lv_label_get_text(child);

            if(strcmp("- Auto Test", str) == 0)
            {
                scr_mgr_push(SCREEN1_1_ID, false);
            }
            if(strcmp("- Manual Test", str) == 0)
            {
                scr_mgr_push(SCREEN1_2_ID, false);
            }
            printf("%s\n", str);
        }
    }
}

static void scr1_item_create(const char *name, lv_event_cb_t cb)
{
    lv_obj_t * obj = lv_obj_class_create_obj(&lv_list_btn_class, scr1_list);
    lv_obj_class_init_obj(obj);
    lv_obj_set_size(obj, LV_PCT(100), LV_SIZE_CONTENT);

    lv_obj_t *label = lv_label_create(obj);
    lv_label_set_text(label, name);
    lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 10, 0);

    lv_obj_set_height(obj, LV_VER_RES / 6);
    lv_obj_set_style_text_font(obj, FONT_BOLD_SIZE_15, LV_PART_MAIN);
    // lv_obj_set_style_bg_color(obj, lv_color_hex(EPD_COLOR_BG), LV_PART_MAIN);
    // lv_obj_set_style_text_color(obj, lv_color_hex(EPD_COLOR_FG), LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_outline_width(obj, 1, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(obj, 10, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_add_event_cb(obj, cb, LV_EVENT_CLICKED, NULL); 
}

static void scr1_btn_event_cb(lv_event_t * e)
{
    if(e->code == LV_EVENT_CLICKED){
        // ui_full_refresh();
        scr_mgr_pop(false);
    }
}

static void create1(lv_obj_t *parent) 
{
    scr1_list = lv_list_create(parent);
    lv_obj_set_size(scr1_list, lv_pct(93), lv_pct(91));
    lv_obj_align(scr1_list, LV_ALIGN_BOTTOM_MID, 0, 0);
    // lv_obj_set_style_bg_color(scr1_list, lv_color_hex(EPD_COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_pad_top(scr1_list, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(scr1_list, 15, LV_PART_MAIN);
    lv_obj_set_style_radius(scr1_list, 0, LV_PART_MAIN);
    // lv_obj_set_style_outline_pad(scr1_list, 1, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr1_list, 0, LV_PART_MAIN);
    // lv_obj_set_style_border_color(scr1_list, lv_color_hex(EPD_COLOR_FG), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(scr1_list, 0, LV_PART_MAIN);

    scr1_item_create("- Auto Test", scr1_list_event);
    // scr1_item_create("-Manual Test", scr1_list_event);

    // back
    scr_back_btn_create(parent, "Lora", scr1_btn_event_cb);
}

static void entry1(void) 
{
    ui_disp_full_refr();
}
static void exit1(void) {
    ui_disp_full_refr();
}
static void destroy1(void) { }

static scr_lifecycle_t screen1 = {
    .create = create1,
    .entry = entry1,
    .exit  = exit1,
    .destroy = destroy1,
};
#endif
// --------------------- screen 1.1 --------------------- Auto Send
#if 1
static lv_obj_t *scr1_1_cont;
static lv_obj_t *lora_lab_buf[11] = {0};
static lv_obj_t *lora_sw_btn;
static lv_obj_t *lora_sw_btn_info;
static lv_timer_t *lora_RT_timer = NULL;
static lv_timer_t *lora_recv_timer = NULL;
static int lora_cnt = 0;

static void scr1_1_btn_event_cb(lv_event_t * e)
{
    if(e->code == LV_EVENT_CLICKED){
        scr_mgr_pop(false);
    }
}

static void lora_mode_sw_event(lv_event_t * e)
{
    if(e->code == LV_EVENT_CLICKED){
        if(ui_lora_get_mode() == LORA_MODE_SEND) {
            ui_lora_set_mode(LORA_MODE_RECV);
            lv_label_set_text(lora_sw_btn_info, "Recv");
            for(int i = 0; i < GET_BUFF_LEN(lora_lab_buf); i++){
                lv_label_set_text_fmt(lora_lab_buf[i], " ", i);
            }
            lora_cnt = 0;
        } else if(ui_lora_get_mode() == LORA_MODE_RECV) {
            ui_lora_set_mode(LORA_MODE_SEND);
            lv_label_set_text(lora_sw_btn_info, "Send");
            for(int i = 0; i < GET_BUFF_LEN(lora_lab_buf); i++){
                lv_label_set_text_fmt(lora_lab_buf[i], " ", i);
            }
            lora_cnt = 0;
        }
    }
}

static void lora_recv_loop_event(lv_timer_t *t)
{
    ui_lora_recv_loop();
}

static void lora_RT_timer_event(lv_timer_t *t)
{
    static int data = 0;
    char buf[32];
    const char *recv_info = NULL;
    int recv_rssi = 0;
    
    if(ui_lora_get_mode() == LORA_MODE_SEND) 
    {
        lv_snprintf(buf, 32, "DeckPro #%d", data++);
        lv_label_set_text_fmt(lora_lab_buf[lora_cnt], "send-> %s", buf);
        ui_lora_send(buf);

        lora_cnt++;
        if(lora_cnt >= GET_BUFF_LEN(lora_lab_buf)) {
            lora_cnt = 0;
        }
    }
    else if(ui_lora_get_mode() == LORA_MODE_RECV)
    {
        if(ui_lora_get_recv(&recv_info, &recv_rssi))
        {
            ui_lora_set_recv_flag();
            lv_label_set_text_fmt(lora_lab_buf[lora_cnt], "recv-> %s [%d]", recv_info, recv_rssi);

            lora_cnt++;
            if(lora_cnt >= GET_BUFF_LEN(lora_lab_buf)) {
                lora_cnt = 0;
            }
        }
    }
}

static lv_obj_t * scr2_create_label(lv_obj_t *parent)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_width(label, LV_HOR_RES - 26);
    lv_obj_set_style_text_font(label, FONT_BOLD_SIZE_15, LV_PART_MAIN);   
    lv_obj_set_style_border_width(label, 0, LV_PART_MAIN);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    return label;
}
static void create1_1(lv_obj_t *parent) 
{
    scr1_1_cont = lv_obj_create(parent);
    lv_obj_set_size(scr1_1_cont, lv_pct(100), lv_pct(85));
    lv_obj_set_style_bg_color(scr1_1_cont, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(scr1_1_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr1_1_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(scr1_1_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr1_1_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(scr1_1_cont, 13, LV_PART_MAIN);
    lv_obj_set_flex_flow(scr1_1_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(scr1_1_cont, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_column(scr1_1_cont, 5, LV_PART_MAIN);
    lv_obj_set_align(scr1_1_cont, LV_ALIGN_BOTTOM_MID);

    for(int i = 0; i < GET_BUFF_LEN(lora_lab_buf); i++){
        lora_lab_buf[i] = scr2_create_label(scr1_1_cont);
        lv_label_set_text_fmt(lora_lab_buf[i], " ", i);
    }

    lora_sw_btn = lv_btn_create(parent);
    lv_obj_set_size(lora_sw_btn, 70, 25);
    lv_obj_set_style_radius(lora_sw_btn, 5, LV_PART_MAIN);
    lv_obj_set_style_border_width(lora_sw_btn, 2, LV_PART_MAIN);
    lora_sw_btn_info = lv_label_create(lora_sw_btn);
    lv_obj_set_style_text_font(lora_sw_btn_info, FONT_BOLD_SIZE_15, LV_PART_MAIN);
    lv_obj_set_style_text_align(lora_sw_btn_info, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lora_sw_btn_info, "Send");
    lv_obj_center(lora_sw_btn_info);
    lv_obj_align(lora_sw_btn, LV_ALIGN_TOP_MID, 0, 5);
    lv_obj_add_event_cb(lora_sw_btn, lora_mode_sw_event, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lab = lv_label_create(parent);
    lv_obj_set_style_text_font(lab, FONT_BOLD_SIZE_15, LV_PART_MAIN);
    lv_label_set_text_fmt(lab, "%.1fM", ui_lora_get_freq());
    lv_obj_align(lab, LV_ALIGN_TOP_RIGHT, -10, 10);

    ui_lora_set_mode(LORA_MODE_SEND);
    lora_cnt = 0;

    // back
    scr_back_btn_create(parent, ("Lora"), scr1_1_btn_event_cb);
}
static void entry1_1(void) 
{
    ui_disp_full_refr();
    lora_RT_timer = lv_timer_create(lora_RT_timer_event, 2000, NULL);
    lora_recv_timer = lv_timer_create(lora_recv_loop_event, 400, NULL);
}
static void exit1_1(void) {
    ui_disp_full_refr();
    if(lora_RT_timer) {
        lv_timer_del(lora_RT_timer);
        lora_RT_timer = NULL;
    }
    if(lora_recv_timer) {
        lv_timer_del(lora_recv_timer);
        lora_recv_timer = NULL;
    }
}
static void destroy1_1(void) { }

static scr_lifecycle_t screen1_1 = {
    .create = create1_1,
    .entry = entry1_1,
    .exit  = exit1_1,
    .destroy = destroy1_1,
};
#endif
//************************************[ screen 2 ]****************************************** Setting
// --------------------- screen 2.1 --------------------- Time
#if 1
static lv_obj_t *scr2_1_cont;

static void scr2_1_btn_event_cb(lv_event_t * e)
{    
    if(e->code == LV_EVENT_CLICKED){
        scr_mgr_pop(false);
    }
}


extern bool updated_time_from_gps;

static void scr2_1_zone_event(lv_event_t *e)
{
    LV_UNUSED(e);
    scr_mgr_push(SCREEN2_1_1_ID, false);
}

static lv_obj_t   *scr2_1_status = NULL;
static lv_timer_t *scr2_1_timer  = NULL;

/* Shows what the clock currently reads and which source set it. Without this
 * there is no way to tell whether the time came off the network, off a
 * satellite fix, or is still the power-on default. */
static void scr2_1_render(void)
{
    char line[160];
    char when[40] = "--:--:--";

    if(system_clock_is_set()) {
        time_t    now = time(NULL);
        struct tm now_tm;
        localtime_r(&now, &now_tm);
        strftime(when, sizeof(when), "%Y-%m-%d  %H:%M:%S", &now_tm);
    }

    char operator_name[MODEM_OPERATOR_LEN];
    ui_phone_get_operator(operator_name, sizeof(operator_name));

    lv_snprintf(line, sizeof(line),
                "%s\n\nSource: %s\nZone: %s (%s)\nNetwork: %s",
                when, system_clock_source_name(),
                system_clock_zone_label(),
                system_clock_zone_is_manual() ? "chosen" : "automatic",
                ui_phone_is_registered() ? (operator_name[0] ? operator_name : "registered")
                                         : "searching");

    lv_label_set_text(scr2_1_status, line);
}

static void scr2_1_timer_event(lv_timer_t *t)
{
    LV_UNUSED(t);
    scr2_1_render();
}

static void create2_1(lv_obj_t *parent) 
{
    scr2_1_status = lv_label_create(parent);
    lv_obj_set_width(scr2_1_status, lv_pct(92));
    lv_obj_set_style_text_font(scr2_1_status, FONT_BOLD_SIZE_15, LV_PART_MAIN);
    lv_obj_set_style_text_color(scr2_1_status, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_text_align(scr2_1_status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(scr2_1_status, LV_LABEL_LONG_WRAP);
    lv_obj_align(scr2_1_status, LV_ALIGN_TOP_MID, 0, 40);
    scr2_1_render();

    lv_obj_t *bar = scr_action_bar_create(parent, 40);
    scr_bar_btn_create(bar, LV_SYMBOL_SETTINGS "  Time zone", 150, scr2_1_zone_event, NULL);

    lv_obj_t *back2_1_label = scr_back_btn_create(parent, ("Time"), scr2_1_btn_event_cb);
}
static void entry2_1(void) 
{
    ui_disp_full_refr();
    updated_time_from_gps = false;
    gps_task_resume();

    // Slow enough that watching the clock sync does not thrash the panel.
    if(scr2_1_timer == NULL) {
        scr2_1_timer = lv_timer_create(scr2_1_timer_event, 5000, NULL);
    }
}
static void exit2_1(void) {
    ui_disp_full_refr();
    gps_task_suspend();

    if(scr2_1_timer) {
        lv_timer_del(scr2_1_timer);
        scr2_1_timer = NULL;
    }
}
static void destroy2_1(void) { scr2_1_status = NULL; }

static scr_lifecycle_t screen2_1 = {
    .create = create2_1,
    .entry = entry2_1,
    .exit  = exit2_1,
    .destroy = destroy2_1,
};
#endif
// --------------------- screen 2.1.1 --------------------- Time zone picker
#if 1
/* Four hundred odd zones do not fit in a dropdown, so this is a filter field
 * over a list. The hardware keyboard types into the field and the list narrows
 * as you go.
 *
 * The list is capped rather than paged: building four hundred rows would be
 * both slow and expensive in widget memory, and a filter of even two characters
 * brings the count under the cap. */
#define SCR2_1_1_MAX_ROWS 24

static lv_obj_t   *scr2_1_1_filter  = NULL;
static lv_obj_t   *scr2_1_1_list    = NULL;
static lv_obj_t   *scr2_1_1_count   = NULL;
static lv_timer_t *scr2_1_1_debounce = NULL;

static void scr2_1_1_back_event(lv_event_t *e)
{
    if(e->code == LV_EVENT_CLICKED) {
        scr_mgr_pop(false);
    }
}

static void scr2_1_1_auto_event(lv_event_t *e)
{
    LV_UNUSED(e);

    system_clock_clear_zone();
    scr_mgr_pop(false);
}

static void scr2_1_1_row_event(lv_event_t *e)
{
    int  idx = (int)(intptr_t)lv_event_get_user_data(e);
    char name[TIMEZONE_NAME_MAX];

    const char *posix = timezone_posix_at(idx);
    if(posix == NULL || !timezone_name_at(idx, name, sizeof(name))) return;

    system_clock_set_zone(name, posix);
    scr_mgr_pop(false);
}

static void scr2_1_1_populate(void)
{
    int  matches[SCR2_1_1_MAX_ROWS];
    int  total = 0;
    char name[TIMEZONE_NAME_MAX];

    const char *filter = lv_textarea_get_text(scr2_1_1_filter);
    if(filter == NULL) filter = "";

    lv_obj_clean(scr2_1_1_list);

    int shown = timezone_search(filter, matches, SCR2_1_1_MAX_ROWS, &total);

    // Rows are single line so as many zones as possible fit on the panel; a
    // tick marks whichever one is currently in force.
    const char *active = system_clock_zone_label();

    // Following the network is the sensible default, so offer it first - but
    // only when the list has not been filtered down to a search.
    if(filter[0] == '\0') {
        scr_row_create(scr2_1_1_list, "Automatic (network)", NULL,
                       system_clock_zone_is_manual() ? NULL : LV_SYMBOL_OK,
                       scr2_1_1_auto_event, NULL);
    }

    for(int i = 0; i < shown; i++) {
        if(!timezone_name_at(matches[i], name, sizeof(name))) continue;

        bool current = system_clock_zone_is_manual() && strcmp(name, active) == 0;
        scr_row_create(scr2_1_1_list, name, NULL, current ? LV_SYMBOL_OK : NULL,
                       scr2_1_1_row_event, (void *)(intptr_t)matches[i]);
    }

    if(total == 0) {
        lv_label_set_text(scr2_1_1_count, "No zone matches that");
    } else if(total > shown) {
        lv_label_set_text_fmt(scr2_1_1_count, "%d of %d - keep typing", shown, total);
    } else {
        lv_label_set_text_fmt(scr2_1_1_count, "%d zone%s", total, total == 1 ? "" : "s");
    }

    ui_disp_full_refr();
}

/* Rebuilding on every keystroke would mean a full panel repaint per character,
 * so the list is refreshed a short while after typing stops. */
static void scr2_1_1_debounce_event(lv_timer_t *t)
{
    LV_UNUSED(t);

    scr2_1_1_populate();

    lv_timer_del(scr2_1_1_debounce);
    scr2_1_1_debounce = NULL;
}

static void scr2_1_1_filter_event(lv_event_t *e)
{
    LV_UNUSED(e);

    if(scr2_1_1_debounce) {
        lv_timer_reset(scr2_1_1_debounce);
        return;
    }
    scr2_1_1_debounce = lv_timer_create(scr2_1_1_debounce_event, 450, NULL);
}

static void create2_1_1(lv_obj_t *parent)
{
    scr2_1_1_filter = lv_textarea_create(parent);
    lv_textarea_set_one_line(scr2_1_1_filter, true);
    lv_textarea_set_placeholder_text(scr2_1_1_filter, "Type to filter");
    lv_textarea_set_max_length(scr2_1_1_filter, TIMEZONE_NAME_MAX - 1);
    lv_obj_set_width(scr2_1_1_filter, lv_pct(94));
    lv_obj_set_style_text_font(scr2_1_1_filter, FONT_BOLD_SIZE_15, LV_PART_MAIN);
    lv_obj_align(scr2_1_1_filter, LV_ALIGN_TOP_MID, 0, 32);
    lv_obj_add_event_cb(scr2_1_1_filter, scr2_1_1_filter_event, LV_EVENT_VALUE_CHANGED, NULL);

    scr2_1_1_count = lv_label_create(parent);
    lv_obj_set_style_text_font(scr2_1_1_count, FONT_BOLD_SIZE_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(scr2_1_1_count, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_size(scr2_1_1_count, lv_pct(94), 16);
    lv_label_set_long_mode(scr2_1_1_count, LV_LABEL_LONG_DOT);
    lv_label_set_text(scr2_1_1_count, " ");
    lv_obj_align(scr2_1_1_count, LV_ALIGN_TOP_MID, 0, 74);

    scr2_1_1_list = lv_list_create(parent);
    lv_obj_set_size(scr2_1_1_list, lv_pct(96), LV_VER_RES - 96);
    lv_obj_align(scr2_1_1_list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_all(scr2_1_1_list, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_row(scr2_1_1_list, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(scr2_1_1_list, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(scr2_1_1_list, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr2_1_1_list, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(scr2_1_1_list, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(scr2_1_1_list, LV_SCROLLBAR_MODE_OFF);

    scr2_1_1_populate();

    scr_back_btn_create(parent, "Time zone", scr2_1_1_back_event);
}

static void entry2_1_1(void)
{
    lv_group_focus_obj(scr2_1_1_filter);
    ui_disp_full_refr();
}

static void exit2_1_1(void)
{
    if(scr2_1_1_debounce) {
        lv_timer_del(scr2_1_1_debounce);
        scr2_1_1_debounce = NULL;
    }
    ui_disp_full_refr();
}

static void destroy2_1_1(void)
{
    scr2_1_1_filter = NULL;
    scr2_1_1_list   = NULL;
    scr2_1_1_count  = NULL;
}

static scr_lifecycle_t screen2_1_1 = {
    .create = create2_1_1,
    .entry = entry2_1_1,
    .exit  = exit2_1_1,
    .destroy = destroy2_1_1,
};
#endif
// --------------------- screen 2.2 --------------------- About System
#if 1
static lv_obj_t *scr2_2_cont;

static void scr2_2_btn_event_cb(lv_event_t * e)
{
    if(e->code == LV_EVENT_CLICKED){
        scr_mgr_pop(false);
    }
}

static void create2_2(lv_obj_t *parent) 
{
    lv_obj_t *info = lv_label_create(parent);
    lv_obj_set_width(info, LV_HOR_RES * 0.9);
    lv_obj_set_style_text_color(info, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_text_font(info, &Font_Mono_Bold_14, LV_PART_MAIN);
    lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);

    String str = "";

    str += "                           \n";
    str += line_full_format(28, "SF Version:", ui_setting_get_sf_ver());
    str += "\n                           \n";

    str += line_full_format(28, "HD Version:", ui_setting_get_hd_ver());
    str += "\n                           \n";

    char buf[30];
    uint64_t total=0, used=0;
    ui_setting_get_sd_capacity(&total, &used);
    lv_snprintf(buf, 30, "%lluMB", total);
    str += line_full_format(28, "SD total:", (const char *)buf);
    str += "\n                           \n";

    lv_snprintf(buf, 30, "%lluMB", used);
    str += line_full_format(28, "SD used:", (const char *)buf);
    str += "\n                           \n";


    lv_label_set_text_fmt(info, "%s", str.c_str());
    
    lv_obj_align(info, LV_ALIGN_TOP_MID, 0, 35);
    
    lv_obj_t *back2_2_label = scr_back_btn_create(parent, ("About System"), scr2_2_btn_event_cb);
}
static void entry2_2(void) 
{
    ui_disp_full_refr();
}
static void exit2_2(void) {
    ui_disp_full_refr();
}
static void destroy2_2(void) { }

static scr_lifecycle_t screen2_2 = {
    .create = create2_2,
    .entry = entry2_2,
    .exit  = exit2_2,
    .destroy = destroy2_2,
};
#endif
// --------------------- screen 2 --------------------- Setting
#if 1
static lv_obj_t *setting_list;
static lv_obj_t *setting_page;
static int setting_num = 0;
static int setting_page_num = 0;
static int setting_curr_page = 0;
static ui_setting_handle setting_handle_list[] = {
    {.name = "- Time",   .type=UI_SETTING_TYPE_SUB, .sub_id = SCREEN2_1_ID},
    {.name = "Keypad Backlight", .type=UI_SETTING_TYPE_SW,  .set_cb = ui_setting_set_keypad_light, .get_cb = ui_setting_get_keypad_light},
    {.name = "Motor Status",     .type=UI_SETTING_TYPE_SW,  .set_cb = ui_setting_set_motor_status, .get_cb = ui_setting_get_motor_status},
    {.name = "Power GPS",        .type=UI_SETTING_TYPE_SW,  .set_cb = ui_setting_set_gps_status,   .get_cb = ui_setting_get_gps_status},
    {.name = "Power Lora",       .type=UI_SETTING_TYPE_SW,  .set_cb = ui_setting_set_lora_status,  .get_cb = ui_setting_get_lora_status},
    {.name = "Power Gyro",       .type=UI_SETTING_TYPE_SW,  .set_cb = ui_setting_set_gyro_status,  .get_cb = ui_setting_get_gyro_status},
    {.name = "Power A7682",      .type=UI_SETTING_TYPE_SW,  .set_cb = ui_setting_set_a7682_status, .get_cb = ui_setting_get_a7682_status},
    {.name = "- About System",   .type=UI_SETTING_TYPE_SUB, .sub_id = SCREEN2_2_ID},
};

static void setting_item_create(int curr_apge);

static void scr2_btn_event_cb(lv_event_t * e)
{
    if(e->code == LV_EVENT_CLICKED){
        scr_mgr_pop(false);
    }
}

static void setting_scr_event(lv_event_t *e)
{
    lv_obj_t *tgt = (lv_obj_t *)e->target;
    ui_setting_handle *h = (ui_setting_handle *)e->user_data;

    if(e->code == LV_EVENT_CLICKED) {
        switch (h->type)
        {
        case UI_SETTING_TYPE_SW:
            h->set_cb(!h->get_cb());
            lv_label_set_text_fmt(h->st, "%s", (h->get_cb() ? "ON" : "OFF"));
            break;
        case UI_SETTING_TYPE_SUB:
            scr_mgr_push(h->sub_id, false);
            break;
        default:
            break;
        }
    }
}

static void setting_page_switch_cb(lv_event_t *e)
{
    char opt = (int)e->user_data;
    
    if(setting_num < SETTING_PAGE_MAX_ITEM) return;

    int child_cnt = lv_obj_get_child_cnt(setting_list);
    
    for(int i = 0; i < child_cnt; i++)
    {
        lv_obj_t *child = lv_obj_get_child(setting_list, 0);
        if(child)
            lv_obj_del(child);
    }

    if(opt == 'p')
    {
        setting_curr_page = (setting_curr_page < setting_page_num) ? setting_curr_page + 1 : 0;
    }
    else if(opt == 'n')
    {
        setting_curr_page = (setting_curr_page > 0) ? setting_curr_page - 1 : setting_page_num;
    }

    setting_item_create(setting_curr_page);
    lv_label_set_text_fmt(setting_page, "%d / %d", setting_curr_page, setting_page_num);
}

static void setting_item_create(int curr_apge)
{
    printf("setting_curr_page = %d\n", setting_curr_page);
    int start = (curr_apge * SETTING_PAGE_MAX_ITEM);
    int end = start + SETTING_PAGE_MAX_ITEM;
    if(end > setting_num) end = setting_num;

    printf("start=%d, end=%d\n", start, end);

    for(int i = start; i < end; i++) {
        ui_setting_handle *h = &setting_handle_list[i];
        

        switch (h->type)
        {
        case UI_SETTING_TYPE_SW:
            h->obj = lv_list_add_btn(setting_list, NULL, h->name);
            h->st = lv_label_create(h->obj);
            lv_obj_set_style_text_font(h->st, FONT_BOLD_SIZE_15, LV_PART_MAIN);
            lv_obj_align(h->st, LV_ALIGN_RIGHT_MID, 0, 0);
            lv_label_set_text_fmt(h->st, "%s", (h->get_cb() ? "ON" : "OFF"));
            break;
        case UI_SETTING_TYPE_SUB:
            h->obj = lv_list_add_btn(setting_list, NULL, h->name);
            break;
        default:
            break;
        }

        // style
        lv_obj_set_height(h->obj, 28);
        lv_obj_set_style_text_font(h->obj, FONT_BOLD_SIZE_14, LV_PART_MAIN);
        lv_obj_set_style_bg_color(h->obj, DECKPRO_COLOR_BG, LV_PART_MAIN);
        lv_obj_set_style_text_color(h->obj, DECKPRO_COLOR_FG, LV_PART_MAIN);
        lv_obj_set_style_border_width(h->obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(h->obj, 1, LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_outline_width(h->obj, 3, LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_radius(h->obj, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_add_event_cb(h->obj, setting_scr_event, LV_EVENT_CLICKED, (void *)h);
    }
}

static void create2(lv_obj_t *parent) 
{
    setting_list = lv_list_create(parent);
    lv_obj_set_size(setting_list, LV_HOR_RES, lv_pct(88));
    lv_obj_align(setting_list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(setting_list, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_pad_top(setting_list, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_row(setting_list, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(setting_list, 0, LV_PART_MAIN);
    // lv_obj_set_style_outline_pad(setting_list, 2, LV_PART_MAIN);
    lv_obj_set_style_border_width(setting_list, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(setting_list, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(setting_list, 0, LV_PART_MAIN);

    setting_num = sizeof(setting_handle_list) / sizeof(setting_handle_list[0]);
    setting_page_num = setting_num / SETTING_PAGE_MAX_ITEM;

    setting_item_create(setting_curr_page);

    lv_obj_t * ui_Button2 = lv_btn_create(parent);
    lv_obj_set_width(ui_Button2, 71);
    lv_obj_set_height(ui_Button2, 40);
    lv_obj_set_x(ui_Button2, -70);
    lv_obj_set_y(ui_Button2, 130);
    lv_obj_set_align(ui_Button2, LV_ALIGN_CENTER);
    lv_obj_add_flag(ui_Button2, LV_OBJ_FLAG_SCROLL_ON_FOCUS);     /// Flags
    lv_obj_clear_flag(ui_Button2, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_bg_color(ui_Button2, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Button2, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_Button2, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui_Button2, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_spread(ui_Button2, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_Button2, 0, LV_PART_MAIN | LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(ui_Button2, 0, LV_PART_MAIN | LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_spread(ui_Button2, 0, LV_PART_MAIN | LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_radius(ui_Button2, 10, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t * ui_Label1 = lv_label_create(ui_Button2);
    lv_obj_set_width(ui_Label1, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_height(ui_Label1, LV_SIZE_CONTENT);    /// 1
    lv_obj_set_align(ui_Label1, LV_ALIGN_CENTER);
    lv_label_set_text(ui_Label1, "Back");
    lv_obj_set_style_text_color(ui_Label1, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Label1, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t * ui_Button14 = lv_btn_create(parent);
    lv_obj_set_width(ui_Button14, 71);
    lv_obj_set_height(ui_Button14, 40);
    lv_obj_set_x(ui_Button14, 70);
    lv_obj_set_y(ui_Button14, 130);
    lv_obj_set_align(ui_Button14, LV_ALIGN_CENTER);
    lv_obj_add_flag(ui_Button14, LV_OBJ_FLAG_SCROLL_ON_FOCUS);     /// Flags
    lv_obj_clear_flag(ui_Button14, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_bg_color(ui_Button14, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Button14, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_Button14, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui_Button14, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_spread(ui_Button14, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_Button14, 0, LV_PART_MAIN | LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(ui_Button14, 0, LV_PART_MAIN | LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_spread(ui_Button14, 0, LV_PART_MAIN | LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_radius(ui_Button14, 10, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t * ui_Label15 = lv_label_create(ui_Button14);
    lv_obj_set_width(ui_Label15, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_height(ui_Label15, LV_SIZE_CONTENT);    /// 1
    lv_obj_set_align(ui_Label15, LV_ALIGN_CENTER);
    lv_label_set_text(ui_Label15, "Next");
    lv_obj_set_style_text_color(ui_Label15, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Label15, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_add_event_cb(ui_Button2, setting_page_switch_cb, LV_EVENT_CLICKED, (void*)'n');
    lv_obj_add_event_cb(ui_Button14, setting_page_switch_cb, LV_EVENT_CLICKED, (void*)'p');

    setting_page = lv_label_create(parent);
    lv_obj_set_width(setting_page, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_height(setting_page, LV_SIZE_CONTENT);    /// 1
    lv_obj_align(setting_page, LV_ALIGN_BOTTOM_MID, 0, -23);
    lv_label_set_text_fmt(setting_page, "%d / %d", setting_curr_page, setting_page_num);
    lv_obj_set_style_text_color(setting_page, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(setting_page, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    scr_back_btn_create(parent, ("Setting"), scr2_btn_event_cb);
}
static void entry2(void) {
    ui_disp_full_refr();
}
static void exit2(void) {
    ui_disp_full_refr();
}
static void destroy2(void) { }

static scr_lifecycle_t screen2 = {
    .create = create2,
    .entry = entry2,
    .exit  = exit2,
    .destroy = destroy2,
};
#endif
//************************************[ screen 3 ]****************************************** GPS
#if 1
#define line_max 23
static lv_obj_t *scr3_cont;
static lv_obj_t *scr3_cnt_lab;
static lv_timer_t *GPS_loop_timer = NULL;

static void gps_set_line(lv_obj_t *label, const char *str1, const char *str2)
{
    int w2 = strlen(str2);
    int w1 = line_max - w2;
    lv_label_set_text_fmt(label, "%-*s%-*s", w1, str1, w2, str2);
}

static lv_obj_t * scr3_create_label(lv_obj_t *parent)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_width(label, lv_pct(90));
    lv_obj_set_style_text_font(label, FONT_BOLD_MONO_SIZE_15, LV_PART_MAIN);   
    lv_obj_set_style_border_width(label, 1, LV_PART_MAIN);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_border_side(label, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    return label;
}

static void scr3_GPS_updata(void)
{
    double lat      = 0; // Latitude
    double lon      = 0; // Longitude
    double speed    = 0; // Speed over ground
    float alt      = 0; // Altitude
    float accuracy = 0; // Accuracy
    uint32_t   vsat     = 0; // Visible Satellites
    int   usat     = 0; // Used Satellites
    uint16_t   year     = 0; // 
    uint8_t   month    = 0; // 
    uint8_t   day      = 0; // 
    uint8_t   hour     = 0; // 
    uint8_t   min      = 0; // 
    uint8_t   sec      = 0; // 

    static int cnt = 0;

    lv_label_set_text_fmt(scr3_cnt_lab, " %05d ", ++cnt);

    ui_gps_get_coord(&lat, &lon);
    ui_gps_get_data(&year, &month, &day);
    ui_gps_get_time(&hour, &min, &sec);
    ui_gps_get_satellites(&vsat);
    ui_gps_get_speed(&speed);

    char buf[32];

    lv_snprintf(buf, 16, "%0.1f", lat);
    gps_set_line(label_list[0], "Latitude:", buf);

    lv_snprintf(buf, 16, "%0.1f", lon);
    gps_set_line(label_list[1], "Longitude:", buf);

    lv_snprintf(buf, 16, "%0.3fkmph", speed);
    gps_set_line(label_list[2], "Speed:", buf);

    lv_snprintf(buf, 16, "%d", vsat);
    gps_set_line(label_list[3], "vsat:", buf);
    
    lv_snprintf(buf, 16, "%d", year);
    gps_set_line(label_list[4], "year:", buf);

    lv_snprintf(buf, 16, "%d", month);
    gps_set_line(label_list[5], "month:", buf);

    lv_snprintf(buf, 16, "%d", day);
    gps_set_line(label_list[6], "day:", buf);

    lv_snprintf(buf, 16, "%02d:%02d:%02d", hour, min, sec);
    gps_set_line(label_list[7], "time:", buf);

    // lv_snprintf(buf, 16, "%0.1f", alt);
    // gps_set_line(label_list[3], "alt:", buf);

    // lv_snprintf(buf, 16, "%d", usat);
    // gps_set_line(label_list[5], "usat:", buf);

}

static void GPS_loop_timer_event(lv_timer_t * t)
{
    scr3_GPS_updata();
}

static void scr3_btn_event_cb(lv_event_t * e)
{
    if(e->code == LV_EVENT_CLICKED){
        scr_mgr_pop(false);
    }
}

static void create3(lv_obj_t *parent) 
{   
    scr3_cont = lv_obj_create(parent);
    lv_obj_set_size(scr3_cont, lv_pct(100), lv_pct(88));
    lv_obj_set_style_bg_color(scr3_cont, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(scr3_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr3_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(scr3_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr3_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(scr3_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(scr3_cont, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_column(scr3_cont, 0, LV_PART_MAIN);
    lv_obj_set_align(scr3_cont, LV_ALIGN_BOTTOM_LEFT);
    lv_obj_set_flex_flow(scr3_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr3_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);

    for(int i = 0; i < sizeof(label_list) / sizeof(label_list[0]); i++) {
        label_list[i] = scr3_create_label(scr3_cont);
        lv_label_set_text(label_list[i], " ");
    }

    scr3_cnt_lab = lv_label_create(parent);
    lv_obj_set_style_text_font(scr3_cnt_lab, FONT_BOLD_MONO_SIZE_15, LV_PART_MAIN);
    lv_obj_set_style_radius(scr3_cnt_lab, 5, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr3_cnt_lab, 2, LV_PART_MAIN);
    lv_obj_set_style_text_align(scr3_cnt_lab, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text_fmt(scr3_cnt_lab, " %05d ", 0);
    lv_obj_center(scr3_cnt_lab);
    lv_obj_align(scr3_cnt_lab, LV_ALIGN_TOP_RIGHT, -10, 10);

    lv_obj_t *back3_label = scr_back_btn_create(parent, ("GPS"), scr3_btn_event_cb);
}
static void entry3(void) 
{
    scr3_GPS_updata();

    ui_gps_task_resume();

    GPS_loop_timer = lv_timer_create(GPS_loop_timer_event, 3000, NULL);
    ui_disp_full_refr();
}
static void exit3(void) {
    ui_gps_task_suspend();
    if(GPS_loop_timer) {
        lv_timer_del(GPS_loop_timer);
        GPS_loop_timer = NULL;
    }
    ui_disp_full_refr();
}
static void destroy3(void) { }

static scr_lifecycle_t screen3 = {
    .create = create3,
    .entry = entry3,
    .exit  = exit3,
    .destroy = destroy3,
};

#undef line_max

#endif
//************************************[ screen 4 ]****************************************** Wifi Scan
// --------------------- screen 4 --------------------- WIFI
#if 1
lv_obj_t * scr4_list;
static lv_obj_t *scr4_lab_buf[20];

static void scr4_list_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * obj = lv_event_get_target(e);
    for(int i = 0; i < lv_obj_get_child_cnt(obj); i++) 
    {
        lv_obj_t * child = lv_obj_get_child(obj, i);
        if(lv_obj_check_type(child, &lv_label_class)) {
            char *str = lv_label_get_text(child);

            if(strcmp("- WIFI Config", str) == 0)
            {
                scr_mgr_push(SCREEN4_1_ID, false);
            }
            if(strcmp("- WIFI Scan", str) == 0)
            {
                scr_mgr_push(SCREEN4_2_ID, false);
            }
            printf("%s\n", str);
        }
    }
}

static void scr4_item_create(const char *name, lv_event_cb_t cb)
{
    lv_obj_t * obj = lv_obj_class_create_obj(&lv_list_btn_class, scr4_list);
    lv_obj_class_init_obj(obj);
    lv_obj_set_size(obj, LV_PCT(100), LV_SIZE_CONTENT);

    lv_obj_t *label = lv_label_create(obj);
    lv_label_set_text(label, name);
    lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 10, 0);

    lv_obj_set_height(obj, LV_VER_RES / 6);
    lv_obj_set_style_text_font(obj, FONT_BOLD_SIZE_15, LV_PART_MAIN);
    // lv_obj_set_style_bg_color(obj, lv_color_hex(EPD_COLOR_BG), LV_PART_MAIN);
    // lv_obj_set_style_text_color(obj, lv_color_hex(EPD_COLOR_FG), LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_outline_width(obj, 1, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(obj, 10, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_add_event_cb(obj, cb, LV_EVENT_CLICKED, NULL); 
}

static void scr4_btn_event_cb(lv_event_t * e)
{
    if(e->code == LV_EVENT_CLICKED){
        // ui_full_refresh();
        scr_mgr_pop(false);
    }
}

static void create4(lv_obj_t *parent) 
{
    scr4_list = lv_list_create(parent);
    lv_obj_set_size(scr4_list, lv_pct(93), lv_pct(91));
    lv_obj_align(scr4_list, LV_ALIGN_BOTTOM_MID, 0, 0);
    // lv_obj_set_style_bg_color(scr4_list, lv_color_hex(EPD_COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_pad_top(scr4_list, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(scr4_list, 15, LV_PART_MAIN);
    lv_obj_set_style_radius(scr4_list, 0, LV_PART_MAIN);
    // lv_obj_set_style_outline_pad(scr4_list, 1, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr4_list, 0, LV_PART_MAIN);
    // lv_obj_set_style_border_color(scr4_list, lv_color_hex(EPD_COLOR_FG), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(scr4_list, 0, LV_PART_MAIN);

    scr4_item_create("- WIFI Config", scr4_list_event);
    scr4_item_create("- WIFI Scan", scr4_list_event);

    // back
    scr_back_btn_create(parent, "WIFI", scr4_btn_event_cb);
}

static void entry4(void) 
{
    ui_disp_full_refr();
}
static void exit4(void) {
    ui_disp_full_refr();
}
static void destroy4(void) { }

static scr_lifecycle_t screen4 = {
    .create = create4,
    .entry = entry4,
    .exit  = exit4,
    .destroy = destroy4,
};
#endif
// --------------------- screen 4.2 --------------------- Wifi Config
#if 1
static void scr4_1_btn_event_cb(lv_event_t * e)
{
    if(e->code == LV_EVENT_CLICKED){
        scr_mgr_pop(false);
    }
}

/* The access point settings the modem is configured with when this screen is
 * opened. They go through the modem service one at a time; the replies land in
 * the serial monitor, which is where this screen has always reported results. */
static const char *wifi_ap_setup_cmds[] = {
    "AT+CGDATA=\"\",1",        // enter data mode
    "AT+CGPADDR",              // report the assigned address
    "AT+CWMAP=0",              // stop the AP while it is reconfigured
    "AT+CWSSID=lilyphone1",
    "AT+CWAUTH=5,3,password",
    "AT+CWMOCH=4,0",           // mode and channel
    "AT+CWISO=1",              // isolate clients from each other
    "AT+CWMAP=1",              // bring the AP back up
};

static void create4_1(lv_obj_t *parent)
{
    lv_obj_t *info = lv_label_create(parent);
    lv_obj_set_width(info, LV_HOR_RES * 0.9);
    lv_obj_set_style_text_font(info, FONT_BOLD_SIZE_15, LV_PART_MAIN);
    lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_label_set_text(info, "Configuring the modem access point.\n\nSSID: lilyphone1\n\nProgress is written to the serial monitor.");
    lv_obj_align(info, LV_ALIGN_CENTER, 0, 0);

    // back
    scr_back_btn_create(parent, "Wifi Config", scr4_1_btn_event_cb);
}

static void entry4_1(void)
{
    for(int i = 0; i < GET_BUFF_LEN(wifi_ap_setup_cmds); i++) {
        ui_modem_at(wifi_ap_setup_cmds[i]);
    }

    ui_disp_full_refr();
}
static void exit4_1(void) {
    ui_disp_full_refr();
}
static void destroy4_1(void) { }

static scr_lifecycle_t screen4_1 = {
    .create = create4_1,
    .entry = entry4_1,
    .exit  = exit4_1,
    .destroy = destroy4_1,
};
#endif
// --------------------- screen 4.2 --------------------- Wifi Scan
#if 1
static lv_obj_t *scr4_2_cont;
static lv_obj_t *wifi_scan_lab;
static lv_timer_t *wifi_scan_timer = NULL;

static ui_wifi_scan_info_t wifi_info_list[UI_WIFI_SCAN_ITEM_MAX];

static void scr4_2_btn_event_cb(lv_event_t * e)
{
    if(e->code == LV_EVENT_CLICKED){
        scr_mgr_pop(false);
    }
}

static void show_wifi_scan(void)
{
#define BUFF_LEN 400
    char buf[BUFF_LEN];
    int ret = 0, offs = 0;

    ret = lv_snprintf(buf + offs, BUFF_LEN, "       NAME      | RSSI\n");
    offs = offs + ret;
    ret = lv_snprintf(buf + offs, BUFF_LEN, "-----------------------\n");
    offs = offs + ret;

    for(int i = 0; i < UI_WIFI_SCAN_ITEM_MAX; i++) {
        if(strcmp(wifi_info_list[i].name, "") == 0 && wifi_info_list[i].rssi == 0)
        {
            break;
        }
        if(i == UI_WIFI_SCAN_ITEM_MAX - 1) {
            ret = lv_snprintf(buf + offs, BUFF_LEN, "%-16.16s | %4d", wifi_info_list[i].name, wifi_info_list[i].rssi);
            break;
        }

        ret = lv_snprintf(buf + offs, BUFF_LEN, "%-16.16s | %4d\n", wifi_info_list[i].name, wifi_info_list[i].rssi);
        offs = offs + ret;
    }
    lv_label_set_text(wifi_scan_lab, buf);
#undef BUFF_LEN
}

static void wifi_scan_timer_event(lv_timer_t *t)
{
    ui_wifi_get_scan_info(wifi_info_list, UI_WIFI_SCAN_ITEM_MAX);
    show_wifi_scan();
}

static void create4_2(lv_obj_t *parent) 
{
    scr4_2_cont = lv_obj_create(parent);
    lv_obj_set_size(scr4_2_cont, lv_pct(100), lv_pct(90));
    lv_obj_set_style_bg_color(scr4_2_cont, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(scr4_2_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr4_2_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(scr4_2_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr4_2_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(scr4_2_cont, 13, LV_PART_MAIN);
    lv_obj_set_flex_flow(scr4_2_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(scr4_2_cont, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_column(scr4_2_cont, 5, LV_PART_MAIN);
    lv_obj_set_align(scr4_2_cont, LV_ALIGN_BOTTOM_MID);

    wifi_scan_lab = lv_label_create(scr4_2_cont);
    lv_obj_set_width(wifi_scan_lab, lv_pct(95));
    lv_obj_set_style_pad_all(wifi_scan_lab, 0, LV_PART_MAIN);
    lv_obj_set_style_text_font(wifi_scan_lab, FONT_BOLD_MONO_SIZE_15, LV_PART_MAIN);   
    lv_obj_set_style_border_width(wifi_scan_lab, 0, LV_PART_MAIN);
    lv_label_set_long_mode(wifi_scan_lab, LV_LABEL_LONG_WRAP);

    lv_obj_t *back4_label = scr_back_btn_create(parent, ("Wifi"), scr4_2_btn_event_cb);
}
static void entry4_2(void)
{
    ui_modem_at("AT+CWSTASCANSYN=1");
    ui_disp_full_refr();
}

static void exit4_2(void) {
    ui_disp_full_refr();
    ui_modem_at("AT+CWSTASCANSYN=0");
}

static void destroy4_2(void) { }

static scr_lifecycle_t screen4_2 = {
    .create = create4_2,
    .entry = entry4_2,
    .exit  = exit4_2,
    .destroy = destroy4_2,
};
#endif
//************************************[ screen 5 ]****************************************** Test
#if 1
static lv_obj_t *test_list;
static lv_obj_t *test_page;
static int test_num = 0;
static int test_page_num = 0;
static int test_curr_page = 0;

static ui_test_handle test_handle_list[] = {
    { .name="Lora",       .peri_id=E_PERI_LORA       , .cb=ui_test_get },
    { .name="Touch",      .peri_id=E_PERI_TOUCH      , .cb=ui_test_get },
    { .name="BQ25896",    .peri_id=E_PERI_BQ25896    , .cb=ui_test_get },
    { .name="BQ27220",    .peri_id=E_PERI_BQ27220    , .cb=ui_test_get },
    { .name="SD Card",    .peri_id=E_PERI_SD         , .cb=ui_test_get },
    { .name="A7682E",     .peri_id=E_PERI_A7682E     , .cb=ui_test_get },
    { .name="Keypad",     .peri_id=E_PERI_KYEPAD     , .cb=ui_test_get },
    { .name="GPS",        .peri_id=E_PERI_GPS        , .cb=ui_test_get },
    { .name="BHI260AP",   .peri_id=E_PERI_BHI260AP   , .cb=ui_test_get },
    { .name="LTR_553ALS", .peri_id=E_PERI_LTR_553ALS , .cb=ui_test_get },
    { .name="INK_SCREEN", .peri_id=E_PERI_INK_SCREEN , .cb=ui_test_get },
};

static void test_item_create(int curr_apge);

static void scr5_btn_event_cb(lv_event_t * e)
{
    if(e->code == LV_EVENT_CLICKED){
        scr_mgr_pop(false);
    }
}

static void test_page_switch_cb(lv_event_t *e)
{
    char opt = (int)e->user_data;
    
    if(test_num < SETTING_PAGE_MAX_ITEM) return;

    int child_cnt = lv_obj_get_child_cnt(test_list);
    
    for(int i = 0; i < child_cnt; i++)
    {
        lv_obj_t *child = lv_obj_get_child(test_list, 0);
        if(child)
            lv_obj_del(child);
    }

    if(opt == 'p')
    {
        test_curr_page = (test_curr_page < test_page_num) ? test_curr_page + 1 : 0;
    }
    else if(opt == 'n')
    {
        test_curr_page = (test_curr_page > 0) ? test_curr_page - 1 : test_page_num;
    }

    test_item_create(test_curr_page);
    lv_label_set_text_fmt(test_page, "%d / %d", test_curr_page, test_page_num);
}

static void test_item_create(int curr_apge)
{
    printf("test_curr_page = %d\n", test_curr_page);
    int start = (curr_apge * SETTING_PAGE_MAX_ITEM);
    int end = start + SETTING_PAGE_MAX_ITEM;
    if(end > test_num) end = test_num;

    printf("start=%d, end=%d\n", start, end);

    for(int i = start; i < end; i++) {
        ui_test_handle *h = &test_handle_list[i];
        h->obj = lv_list_add_btn(test_list, NULL, h->name);
        h->st = lv_label_create(h->obj);
        lv_obj_set_style_text_font(h->st, FONT_BOLD_SIZE_15, LV_PART_MAIN);
        lv_obj_align(h->st, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_label_set_text_fmt(h->st, "%s", (h->cb(h->peri_id) ? "PASS" : "----"));
        // style
        lv_obj_set_style_text_font(h->obj, FONT_BOLD_SIZE_15, LV_PART_MAIN);
        lv_obj_set_style_bg_color(h->obj, DECKPRO_COLOR_BG, LV_PART_MAIN);
        lv_obj_set_style_text_color(h->obj, DECKPRO_COLOR_FG, LV_PART_MAIN);
        lv_obj_set_style_border_width(h->obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(h->obj, 1, LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_outline_width(h->obj, 3, LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_radius(h->obj, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
        // lv_obj_add_event_cb(h->obj, test_scr_event, LV_EVENT_CLICKED, (void *)h);
    }
}

static void create5(lv_obj_t *parent) 
{
    test_list = lv_list_create(parent);
    lv_obj_set_size(test_list, LV_HOR_RES, lv_pct(88));
    lv_obj_align(test_list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(test_list, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_pad_top(test_list, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_row(test_list, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(test_list, 0, LV_PART_MAIN);
    // lv_obj_set_style_outline_pad(test_list, 2, LV_PART_MAIN);
    lv_obj_set_style_border_width(test_list, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(test_list, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(test_list, 0, LV_PART_MAIN);

    test_num = sizeof(test_handle_list) / sizeof(test_handle_list[0]);
    test_page_num = test_num / SETTING_PAGE_MAX_ITEM;
    test_item_create(test_curr_page);

    lv_obj_t * ui_Button2 = lv_btn_create(parent);
    lv_obj_set_width(ui_Button2, 71);
    lv_obj_set_height(ui_Button2, 40);
    lv_obj_set_x(ui_Button2, -70);
    lv_obj_set_y(ui_Button2, 130);
    lv_obj_set_align(ui_Button2, LV_ALIGN_CENTER);
    lv_obj_add_flag(ui_Button2, LV_OBJ_FLAG_SCROLL_ON_FOCUS);     /// Flags
    lv_obj_clear_flag(ui_Button2, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_bg_color(ui_Button2, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Button2, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_Button2, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui_Button2, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_spread(ui_Button2, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_Button2, 0, LV_PART_MAIN | LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(ui_Button2, 0, LV_PART_MAIN | LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_spread(ui_Button2, 0, LV_PART_MAIN | LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_radius(ui_Button2, 10, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t * ui_Label1 = lv_label_create(ui_Button2);
    lv_obj_set_width(ui_Label1, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_height(ui_Label1, LV_SIZE_CONTENT);    /// 1
    lv_obj_set_align(ui_Label1, LV_ALIGN_CENTER);
    lv_label_set_text(ui_Label1, "Back");
    lv_obj_set_style_text_color(ui_Label1, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Label1, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t * ui_Button14 = lv_btn_create(parent);
    lv_obj_set_width(ui_Button14, 71);
    lv_obj_set_height(ui_Button14, 40);
    lv_obj_set_x(ui_Button14, 70);
    lv_obj_set_y(ui_Button14, 130);
    lv_obj_set_align(ui_Button14, LV_ALIGN_CENTER);
    lv_obj_add_flag(ui_Button14, LV_OBJ_FLAG_SCROLL_ON_FOCUS);     /// Flags
    lv_obj_clear_flag(ui_Button14, LV_OBJ_FLAG_SCROLLABLE);      /// Flags
    lv_obj_set_style_bg_color(ui_Button14, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Button14, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_Button14, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui_Button14, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_spread(ui_Button14, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_Button14, 0, LV_PART_MAIN | LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(ui_Button14, 0, LV_PART_MAIN | LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_spread(ui_Button14, 0, LV_PART_MAIN | LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_radius(ui_Button14, 10, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t * ui_Label15 = lv_label_create(ui_Button14);
    lv_obj_set_width(ui_Label15, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_height(ui_Label15, LV_SIZE_CONTENT);    /// 1
    lv_obj_set_align(ui_Label15, LV_ALIGN_CENTER);
    lv_label_set_text(ui_Label15, "Next");
    lv_obj_set_style_text_color(ui_Label15, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Label15, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_add_event_cb(ui_Button2, test_page_switch_cb, LV_EVENT_CLICKED, (void*)'n');
    lv_obj_add_event_cb(ui_Button14, test_page_switch_cb, LV_EVENT_CLICKED, (void*)'p');

    test_page = lv_label_create(parent);
    lv_obj_set_width(test_page, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_height(test_page, LV_SIZE_CONTENT);    /// 1
    lv_obj_align(test_page, LV_ALIGN_BOTTOM_MID, 0, -23);
    lv_label_set_text_fmt(test_page, "%d / %d", test_curr_page, test_page_num);
    lv_obj_set_style_text_color(test_page, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(test_page, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *back5_label = scr_back_btn_create(parent, ("Test"), scr5_btn_event_cb);
}
static void entry5(void) 
{
    ui_disp_full_refr();
}
static void exit5(void) {
    ui_disp_full_refr();
}
static void destroy5(void) { }

static scr_lifecycle_t screen5 = {
    .create = create5,
    .entry = entry5,
    .exit  = exit5,
    .destroy = destroy5,
};
#endif
//************************************[ screen 6 ]****************************************** Battery
// --------------------- screen 6 --------------------- Battery
#if 1
lv_obj_t * scr6_list;
static lv_obj_t *scr6_lab_buf[20];

static void scr6_list_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * obj = lv_event_get_target(e);
    for(int i = 0; i < lv_obj_get_child_cnt(obj); i++) 
    {
        lv_obj_t * child = lv_obj_get_child(obj, i);
        if(lv_obj_check_type(child, &lv_label_class)) {
            char *str = lv_label_get_text(child);

            if(strcmp("- BQ25896", str) == 0)
            {
                scr_mgr_push(SCREEN6_1_ID, false);
            }
            if(strcmp("- BQ27220", str) == 0)
            {
                scr_mgr_push(SCREEN6_2_ID, false);
            }
            printf("%s\n", str);
        }
    }
}

static void scr6_item_create(const char *name, lv_event_cb_t cb)
{
    lv_obj_t * obj = lv_obj_class_create_obj(&lv_list_btn_class, scr6_list);
    lv_obj_class_init_obj(obj);
    lv_obj_set_size(obj, LV_PCT(100), LV_SIZE_CONTENT);

    lv_obj_t *label = lv_label_create(obj);
    lv_label_set_text(label, name);
    lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 10, 0);

    lv_obj_set_height(obj, LV_VER_RES / 6);
    lv_obj_set_style_text_font(obj, FONT_BOLD_SIZE_15, LV_PART_MAIN);
    // lv_obj_set_style_bg_color(obj, lv_color_hex(EPD_COLOR_BG), LV_PART_MAIN);
    // lv_obj_set_style_text_color(obj, lv_color_hex(EPD_COLOR_FG), LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_outline_width(obj, 1, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(obj, 10, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_add_event_cb(obj, cb, LV_EVENT_CLICKED, NULL); 
}

static void scr6_btn_event_cb(lv_event_t * e)
{
    if(e->code == LV_EVENT_CLICKED){
        // ui_full_refresh();
        scr_mgr_pop(false);
    }
}

static void create6(lv_obj_t *parent) 
{
    scr6_list = lv_list_create(parent);
    lv_obj_set_size(scr6_list, lv_pct(93), lv_pct(91));
    lv_obj_align(scr6_list, LV_ALIGN_BOTTOM_MID, 0, 0);
    // lv_obj_set_style_bg_color(scr6_list, lv_color_hex(EPD_COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_pad_top(scr6_list, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(scr6_list, 15, LV_PART_MAIN);
    lv_obj_set_style_radius(scr6_list, 0, LV_PART_MAIN);
    // lv_obj_set_style_outline_pad(scr6_list, 1, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr6_list, 0, LV_PART_MAIN);
    // lv_obj_set_style_border_color(scr6_list, lv_color_hex(EPD_COLOR_FG), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(scr6_list, 0, LV_PART_MAIN);

    scr6_item_create("- BQ25896", scr6_list_event);
    scr6_item_create("- BQ27220", scr6_list_event);

    // back
    scr_back_btn_create(parent, "Battery", scr6_btn_event_cb);
}

static void entry6(void) 
{
    ui_disp_full_refr();
}
static void exit6(void) {
    ui_disp_full_refr();
}
static void destroy6(void) { }

static scr_lifecycle_t screen6 = {
    .create = create6,
    .entry = entry6,
    .exit  = exit6,
    .destroy = destroy6,
};
#endif
// --------------------- screen 6.1 --------------------- BQ25896
#if 1
#define line_max 23

static lv_timer_t *batt_6_1_timer = NULL;

static void battery_set_line(lv_obj_t *label, const char *str1, const char *str2)
{
    int w2 = strlen(str2);
    int w1 = line_max - w2;
    lv_label_set_text_fmt(label, "%-*s%-*s", w1, str1, w2, str2);
}

static lv_obj_t * scr6_1_create_label(lv_obj_t *parent)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_width(label, lv_pct(90));
    lv_obj_set_style_text_font(label, FONT_BOLD_MONO_SIZE_15, LV_PART_MAIN);   
    lv_obj_set_style_border_width(label, 1, LV_PART_MAIN);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_border_side(label, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    return label;
}

static void scr6_1_battert_updata(void)
{
    char buf[line_max];

    battery_set_line(label_list[0], "Charging:", (ui_batt_25896_is_chg() == true ? "Charging" : "Not charged"));

    lv_snprintf(buf, line_max, "%.2fV", ui_batt_25896_get_vbus());
    battery_set_line(label_list[1], "VBUS:", buf);

    lv_snprintf(buf, line_max, "%.2fV", ui_batt_25896_get_vsys());
    battery_set_line(label_list[2], "VSYS:", buf);

    lv_snprintf(buf, line_max, "%.2fV", ui_batt_25896_get_vbat());
    battery_set_line(label_list[3], "VBAT:", buf);

    lv_snprintf(buf, line_max, "%.2fv", ui_batt_25896_get_volt_targ());
    battery_set_line(label_list[4], "VOLT Target:", buf);

    lv_snprintf(buf, line_max, "%.2fmA", ui_batt_25896_get_chg_curr());
    battery_set_line(label_list[5], "Charge Curr:", buf);

    lv_snprintf(buf, line_max, "%.2fmA", ui_batt_25896_get_pre_curr());
    battery_set_line(label_list[6], "Prechg Curr:", buf);

    lv_snprintf(buf, line_max, "%s", ui_batt_25896_get_chg_st());
    battery_set_line(label_list[7], "CHG ST:", buf);

    lv_snprintf(buf, line_max, "%s", ui_batt_25896_get_vbus_st());
    battery_set_line(label_list[8], "VBUS Status:", buf);

    lv_snprintf(buf, line_max, "%s", ui_batt_25896_get_ntc_st());
    battery_set_line(label_list[9], " ", buf);
}

static void batt_6_1_updata_timer_event(lv_timer_t *t) 
{
    scr6_1_battert_updata();
}

static void scr6_1_btn_event_cb(lv_event_t * e)
{
    if(e->code == LV_EVENT_CLICKED){
        scr_mgr_pop(false);
    }
}

static void create6_1(lv_obj_t *parent) 
{
    lv_obj_t *scr6_1_cont = lv_obj_create(parent);
    lv_obj_set_size(scr6_1_cont, lv_pct(100), lv_pct(88));
    lv_obj_set_style_bg_color(scr6_1_cont, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(scr6_1_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr6_1_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(scr6_1_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr6_1_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(scr6_1_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(scr6_1_cont, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_column(scr6_1_cont, 0, LV_PART_MAIN);
    lv_obj_set_align(scr6_1_cont, LV_ALIGN_BOTTOM_LEFT);
    lv_obj_set_flex_flow(scr6_1_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr6_1_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);

    for(int i = 0; i < sizeof(label_list) / sizeof(label_list[0]); i++) {
        label_list[i] = scr6_1_create_label(scr6_1_cont);
    }

    scr_back_btn_create(parent, ("BQ25896"), scr6_1_btn_event_cb);
}
static void entry6_1(void) 
{
    scr6_1_battert_updata();
    ui_disp_full_refr();
    batt_6_1_timer = lv_timer_create(batt_6_1_updata_timer_event, 5000, NULL);
}
static void exit6_1(void) {
    if(batt_6_1_timer) {
        lv_timer_del(batt_6_1_timer);
        batt_6_1_timer = NULL;
    }
    ui_disp_full_refr();
}
static void destroy6_1(void) { }

static scr_lifecycle_t screen6_1 = {
    .create = create6_1,
    .entry = entry6_1,
    .exit  = exit6_1,
    .destroy = destroy6_1,
};
#undef line_max

#endif
// --------------------- screen 6.2 --------------------- BQ27220
#if 1

#define line_max 23

static lv_timer_t *batt_6_2_timer = NULL;

static lv_obj_t * scr6_2_create_label(lv_obj_t *parent)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_width(label, lv_pct(90));
    lv_obj_set_style_text_font(label, FONT_BOLD_MONO_SIZE_15, LV_PART_MAIN);   
    lv_obj_set_style_border_width(label, 1, LV_PART_MAIN);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_border_side(label, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    return label;
}

static void scr6_2_battert_updata(void)
{
    char buf[line_max];

    battery_set_line(label_list[0],"VBUS ST::", (ui_battery_27220_get_input() == true ? "Connected" : "Disonnected"));

    if(ui_battery_27220_get_input() == true ){
        lv_snprintf(buf, line_max, "%s", (ui_battery_27220_get_charge_finish()? "Finsish":"Charging"));
    } else {
        lv_snprintf(buf, line_max, "%s", "Discharge");
    }
    battery_set_line(label_list[1],"Charing ST:", buf);

    lv_snprintf(buf, line_max, "0x%x", ui_battery_27220_get_status());
    battery_set_line(label_list[2],"Battery ST:", buf);

    lv_snprintf(buf, line_max, "%dmV", ui_battery_27220_get_voltage());
    battery_set_line(label_list[3], "Voltage:", buf);

    lv_snprintf(buf, line_max, "%dmA", ui_battery_27220_get_current());
    battery_set_line(label_list[4], "Current:", buf);

    lv_snprintf(buf, line_max, "%.2fC", (float)(ui_battery_27220_get_temperature() / 10.0 - 273.0));
    battery_set_line(label_list[5], "Temperature:", buf);

    lv_snprintf(buf, line_max, "%dmAh", ui_battery_27220_get_remain_capacity());
    battery_set_line(label_list[6], "Cap Remain:", buf);

    lv_snprintf(buf, line_max, "%dmAh", ui_battery_27220_get_full_capacity());
    battery_set_line(label_list[7], "Cap Full:", buf);

    lv_snprintf(buf, line_max, "%d%%", ui_battery_27220_get_percent());
    battery_set_line(label_list[8], "Cap Percent:", buf);

    lv_snprintf(buf, line_max, "%d%%", ui_battery_27220_get_health());
    battery_set_line(label_list[9], "CapHealth:", buf);
}

static void batt_6_2_updata_timer_event(lv_timer_t *t) 
{
    scr6_2_battert_updata();
}

static void scr6_2_btn_event_cb(lv_event_t * e)
{
    if(e->code == LV_EVENT_CLICKED){
        scr_mgr_pop(false);
    }
}

static void create6_2(lv_obj_t *parent) 
{   
    lv_obj_t *scr6_2_cont = lv_obj_create(parent);
    lv_obj_set_size(scr6_2_cont, lv_pct(100), lv_pct(88));
    lv_obj_set_style_bg_color(scr6_2_cont, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(scr6_2_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr6_2_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(scr6_2_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr6_2_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(scr6_2_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(scr6_2_cont, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_column(scr6_2_cont, 0, LV_PART_MAIN);
    lv_obj_set_align(scr6_2_cont, LV_ALIGN_BOTTOM_LEFT);
    lv_obj_set_flex_flow(scr6_2_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr6_2_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);

    for(int i = 0; i < sizeof(label_list) / sizeof(label_list[0]); i++) {
        label_list[i] = scr6_2_create_label(scr6_2_cont);
    }
    // back
    scr_back_btn_create(parent, ("BQ27220"), scr6_btn_event_cb);
}

static void entry6_2(void) 
{
    scr6_2_battert_updata();
    ui_disp_full_refr();
    batt_6_2_timer = lv_timer_create(batt_6_2_updata_timer_event, 5000, NULL);
}
static void exit6_2(void) {
    if(batt_6_2_timer) {
        lv_timer_del(batt_6_2_timer);
        batt_6_2_timer = NULL;
    }
    ui_disp_full_refr();
}

static void destroy6_2(void) { }

static scr_lifecycle_t screen6_2 = {
    .create = create6_2,
    .entry = entry6_2,
    .exit  = exit6_2,
    .destroy = destroy6_2,
};
#undef line_max
#endif

//************************************[ phone apps ]****************************************
/* Shared state for the dialer, contacts and messaging screens.
 *
 * The screen manager destroys a screen's widgets when it is popped, so a screen
 * cannot hand a value straight to the one that pushed it. These few variables
 * are the hand-off: whoever pushes a screen sets the subject first, and the
 * pushed screen reads it in its create()/entry().
 *
 * The revision counters are the other half of that: a list built in create()
 * goes stale when a message arrives or a contact is edited underneath it, so
 * each list screen records the revision it was built from and repopulates
 * itself in entry() when the number has moved on. Rebuilding in place matters -
 * popping and pushing from inside entry() would re-enter the screen manager
 * while it is still walking its own stack. */
#if 1
static char ui_active_number[CONTACT_NUMBER_LEN] = {0}; // who the pushed screen is about
static char ui_compose_prefill[SMS_TEXT_LEN] = {0};     // body to open the composer with
static int  ui_active_contact = -1;                     // contact index, -1 when it is a new one
static int  ui_pick_mode = UI_PICK_NONE;                // why the contact list was opened
static bool ui_pick_ready = false;                      // a pick landed in ui_active_number

static uint32_t ui_sms_revision      = 0;
static uint32_t ui_contacts_revision = 0;

/* The message currently with the modem. Watched centrally rather than by the
 * composer, so that navigating away from the composer still leaves the log
 * with the right delivery status. */
static uint32_t ui_send_watch_id  = 0;
static int      ui_send_watch_idx = -1;

static void ui_set_active_number(const char *number)
{
    if(number == NULL) number = "";
    lv_snprintf(ui_active_number, CONTACT_NUMBER_LEN, "%s", number);
    ui_active_contact = contacts_find_by_number(ui_active_number);
}

/* Message bodies arrive with the sender's line breaks in them, and a list row
 * has exactly one line to spare. Collapse every run of whitespace to a single
 * space and drop control characters, so a row cannot grow past its height.
 * Bytes above 0x7F are passed through untouched to keep UTF-8 intact. */
static void ui_message_snippet(char *buf, int len, const char *text)
{
    int  o     = 0;
    bool space = false;

    if(text == NULL) text = "";
    while(*text && isspace((unsigned char)*text)) text++; // leading whitespace

    for(const char *p = text; *p && o < len - 1; p++) {
        unsigned char c = (unsigned char)*p;

        if(c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            space = true;
            continue;
        }
        if(c < 0x20 || c == 0x7F) continue; // other control characters

        if(space && o > 0 && o < len - 1) buf[o++] = ' ';
        space = false;
        if(o < len - 1) buf[o++] = (char)c;
    }
    buf[o] = '\0';
}

/* "14:05" for today, "04/09" for anything older. The list rows have only a
 * narrow column beside the name, so they use this rather than the full stamp. */
static void ui_format_stamp_compact(char *buf, int len, uint32_t ts)
{
    if(ts == 0) {
        lv_snprintf(buf, len, "--:--");
        return;
    }

    time_t    when = (time_t)ts;
    time_t    now  = time(NULL);
    struct tm when_tm;
    struct tm now_tm;

    localtime_r(&when, &when_tm);
    localtime_r(&now, &now_tm);

    if(when_tm.tm_year == now_tm.tm_year && when_tm.tm_yday == now_tm.tm_yday) {
        strftime(buf, len, "%H:%M", &when_tm);
    } else {
        strftime(buf, len, "%d/%m", &when_tm);
    }
}

/* "14:05" for today, "04/09 14:05" for anything older. Shows "--:--" until the
 * GPS has given us a clock. */
static void ui_format_stamp(char *buf, int len, uint32_t ts)
{
    if(ts == 0) {
        lv_snprintf(buf, len, "--:--");
        return;
    }

    time_t    when = (time_t)ts;
    time_t    now  = time(NULL);
    struct tm when_tm;
    struct tm now_tm;

    localtime_r(&when, &when_tm);
    localtime_r(&now, &now_tm);

    if(when_tm.tm_year == now_tm.tm_year && when_tm.tm_yday == now_tm.tm_yday) {
        strftime(buf, len, "%H:%M", &when_tm);
    } else {
        strftime(buf, len, "%d/%m %H:%M", &when_tm);
    }
}

#endif
//************************************[ screen 8 ]****************************************** dialer
#if 1
static lv_obj_t *scr8_number_ta = NULL;
static lv_obj_t *scr8_name_label = NULL;

/* Redraws the "who is this" line under the dialled digits. */
static void scr8_update_name(void)
{
    if(scr8_name_label == NULL) return;

    const char *typed = lv_textarea_get_text(scr8_number_ta);
    int         idx   = (typed && typed[0]) ? contacts_find_by_number(typed) : -1;
    const contact_t *c = contacts_get(idx);

    lv_label_set_text(scr8_name_label, c ? c->name : " ");
}

static void scr8_keypad_event(lv_event_t *e)
{
    lv_obj_t   *btnm = (lv_obj_t *)lv_event_get_target(e);
    uint32_t    id   = lv_btnmatrix_get_selected_btn(btnm);
    const char *txt  = lv_btnmatrix_get_btn_text(btnm, id);

    if(txt == NULL) return;

    if(strcmp(txt, LV_SYMBOL_CALL) == 0) {
        const char *number = lv_textarea_get_text(scr8_number_ta);
        if(number == NULL || number[0] == '\0') return;
        ui_set_active_number(number);
        ui_phone_dial(number);
        scr_mgr_push(SCREEN8_1_ID, false);
    } else if(strcmp(txt, LV_SYMBOL_LIST) == 0) {
        ui_pick_mode = UI_PICK_DIAL;
        scr_mgr_push(SCREEN12_ID, false);
    } else if(strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) {
        lv_textarea_del_char(scr8_number_ta);
        scr8_update_name();
    } else {
        lv_textarea_add_text(scr8_number_ta, txt);
        scr8_update_name();
    }
}

static const char * btnm_map[] = { "1", "2", "3", "\n",
                                   "4", "5", "6", "\n",
                                   "7", "8", "9", "\n",
                                   "*", "0", "#", "\n",
                                   LV_SYMBOL_CALL, LV_SYMBOL_LIST, LV_SYMBOL_BACKSPACE, ""
                                 };

static void scr8_btn_event_cb(lv_event_t * e)
{
    if(e->code == LV_EVENT_CLICKED){
        scr_mgr_pop(false);
    }
}

/* Save whatever has been typed as a new contact. */
static void scr8_save_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);

    const char *number = lv_textarea_get_text(scr8_number_ta);
    if(number == NULL || number[0] == '\0') return;

    ui_set_active_number(number);
    scr_mgr_push(SCREEN12_2_ID, false);
}

static void create8(lv_obj_t *parent)
{
    scr8_number_ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(scr8_number_ta, true);
    lv_textarea_set_max_length(scr8_number_ta, CONTACT_NUMBER_LEN - 1);
    lv_obj_set_width(scr8_number_ta, lv_pct(94));
    lv_obj_align(scr8_number_ta, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_set_style_text_font(scr8_number_ta, &Font_Mono_Bold_20, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(scr8_number_ta, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_line_space(scr8_number_ta, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Naming the number as it is dialled is the difference between a keypad
    // and a phone.
    scr8_name_label = lv_label_create(parent);
    lv_obj_set_style_text_font(scr8_name_label, FONT_BOLD_SIZE_15, LV_PART_MAIN);
    lv_obj_set_style_text_color(scr8_name_label, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_label_set_long_mode(scr8_name_label, LV_LABEL_LONG_DOT);
    lv_obj_set_size(scr8_name_label, lv_pct(94), 18);
    lv_obj_set_style_text_align(scr8_name_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(scr8_name_label, " ");
    lv_obj_align(scr8_name_label, LV_ALIGN_TOP_MID, 0, 84);

    lv_obj_t * btnm1 = lv_btnmatrix_create(parent);
    lv_btnmatrix_set_map(btnm1, btnm_map);
    lv_obj_set_size(btnm1, lv_pct(100)-2, lv_pct(60));
    lv_obj_set_style_border_width(btnm1, 0, 0);
    lv_obj_align(btnm1, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(btnm1, scr8_keypad_event, LV_EVENT_VALUE_CHANGED, NULL);

    scr_back_btn_create(parent, "Phone", scr8_btn_event_cb);
    scr_action_btn_create(parent, LV_SYMBOL_PLUS, scr8_save_event_cb);
}

static void entry8(void)
{
    // Coming back from the contact picker, adopt whatever was chosen there.
    if(ui_pick_ready && ui_pick_mode == UI_PICK_DIAL) {
        lv_textarea_set_text(scr8_number_ta, ui_active_number);
    }
    ui_pick_ready = false;
    ui_pick_mode  = UI_PICK_NONE;

    lv_group_focus_obj(scr8_number_ta);
    scr8_update_name();
    ui_disp_full_refr();
}

static void exit8(void) {
    ui_disp_full_refr();
}

static void destroy8(void)
{
    scr8_number_ta  = NULL;
    scr8_name_label = NULL;
}

static scr_lifecycle_t screen8 = {
    .create = create8,
    .entry = entry8,
    .exit  = exit8,
    .destroy = destroy8,
};
#endif
// --------------------- screen 8.1 --------------------- in call
#if 1
static lv_obj_t   *scr8_1_status = NULL;
static lv_obj_t   *scr8_1_answer = NULL;
static lv_timer_t *scr8_1_timer  = NULL;
static modem_call_state_t scr8_1_shown_state = MODEM_CALL_IDLE;

static void scr8_1_answer_event(lv_event_t *e)
{
    LV_UNUSED(e);
    ui_phone_answer();
}

static void scr8_1_hangup_event(lv_event_t *e)
{
    LV_UNUSED(e);
    ui_phone_hang_up();
    scr_mgr_pop(false);
}

static void scr8_1_render(void)
{
    modem_call_state_t state = ui_phone_get_call_state();
    char buf[40];

    switch(state) {
        case MODEM_CALL_INCOMING:
            lv_label_set_text(scr8_1_status, "Incoming call");
            break;

        case MODEM_CALL_DIALING:
            lv_label_set_text(scr8_1_status, "Calling...");
            break;

        case MODEM_CALL_ACTIVE: {
            uint32_t secs = ui_phone_get_call_duration() / 1000;
            lv_snprintf(buf, sizeof(buf), "Connected  %02u:%02u",
                        (unsigned)(secs / 60), (unsigned)(secs % 60));
            lv_label_set_text(scr8_1_status, buf);
            break;
        }

        default:
            lv_label_set_text(scr8_1_status, "Call ended");
            break;
    }

    // Answering only makes sense while the other end is still ringing.
    if(state == MODEM_CALL_INCOMING) {
        lv_obj_clear_flag(scr8_1_answer, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(scr8_1_answer, LV_OBJ_FLAG_HIDDEN);
    }

    if(state != scr8_1_shown_state) {
        scr8_1_shown_state = state;
        ui_disp_full_refr();
    }
}

/* The e-ink panel repaints in full for every update, so the call timer ticks
 * every few seconds rather than every second. */
static void scr8_1_timer_event(lv_timer_t *t)
{
    LV_UNUSED(t);

    // Leave the screen once the end of the call has been shown once.
    if(ui_phone_get_call_state() == MODEM_CALL_IDLE && scr8_1_shown_state == MODEM_CALL_IDLE) {
        scr_mgr_pop(false);
        return;
    }
    scr8_1_render();
}

static void create8_1(lv_obj_t *parent)
{
    char number[CONTACT_NUMBER_LEN];
    ui_phone_get_call_number(number, sizeof(number));
    if(number[0] == '\0') {
        lv_snprintf(number, sizeof(number), "%s", ui_active_number);
    }

    const char *name = number[0] ? contacts_display_name(number) : "Unknown caller";

    lv_obj_t *who = lv_label_create(parent);
    lv_obj_set_width(who, lv_pct(92));
    lv_obj_set_style_text_font(who, &Font_Mono_Bold_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(who, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_text_align(who, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(who, LV_LABEL_LONG_WRAP);
    lv_label_set_text(who, name);
    lv_obj_align(who, LV_ALIGN_TOP_MID, 0, 62);

    // When the name came from the contact book, still show the raw number.
    lv_obj_t *sub = lv_label_create(parent);
    lv_obj_set_width(sub, lv_pct(92));
    lv_obj_set_style_text_font(sub, FONT_BOLD_SIZE_15, LV_PART_MAIN);
    lv_obj_set_style_text_color(sub, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
    lv_obj_set_height(sub, 18);
    lv_label_set_text(sub, (strcmp(name, number) == 0) ? " " : number);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 126);

    scr8_1_status = lv_label_create(parent);
    lv_obj_set_width(scr8_1_status, lv_pct(92));
    lv_obj_set_style_text_font(scr8_1_status, FONT_BOLD_SIZE_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(scr8_1_status, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_text_align(scr8_1_status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(scr8_1_status, "Calling...");
    lv_obj_align(scr8_1_status, LV_ALIGN_CENTER, 0, 20);

    lv_obj_t *bar = scr_action_bar_create(parent, 44);
    scr8_1_answer = scr_bar_btn_create(bar, LV_SYMBOL_CALL "  Answer", 106, scr8_1_answer_event, NULL);
    scr_bar_btn_create(bar, LV_SYMBOL_CLOSE "  Hang up", 106, scr8_1_hangup_event, NULL);

    scr_back_btn_create(parent, "Call", scr8_btn_event_cb);
}

static void entry8_1(void)
{
    scr8_1_shown_state = MODEM_CALL_IDLE;
    scr8_1_render();

    if(ui_phone_get_call_state() == MODEM_CALL_INCOMING) {
        ui_phone_vibrate(400);
    }

    if(scr8_1_timer == NULL) {
        scr8_1_timer = lv_timer_create(scr8_1_timer_event, 3000, NULL);
    }
    ui_disp_full_refr();
}

static void exit8_1(void)
{
    if(scr8_1_timer) {
        lv_timer_del(scr8_1_timer);
        scr8_1_timer = NULL;
    }
    ui_disp_full_refr();
}

static void destroy8_1(void)
{
    scr8_1_status = NULL;
    scr8_1_answer = NULL;
}

static scr_lifecycle_t screen8_1 = {
    .create = create8_1,
    .entry = entry8_1,
    .exit  = exit8_1,
    .destroy = destroy8_1,
};
#endif
//************************************[ screen 12 ]***************************************** contacts
#if 1
static lv_obj_t *scr12_list = NULL;
static uint32_t  scr12_built_revision = 0;

static void scr12_row_event(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const contact_t *c = contacts_get(idx);
    if(c == NULL) return;

    if(ui_pick_mode != UI_PICK_NONE) {
        // Opened to choose a number for the dialer or the composer: hand the
        // number back and let the screen underneath pick it up.
        lv_snprintf(ui_active_number, CONTACT_NUMBER_LEN, "%s", c->number);
        ui_active_contact = idx;
        ui_pick_ready     = true;
        scr_mgr_pop(false);
        return;
    }

    ui_active_contact = idx;
    lv_snprintf(ui_active_number, CONTACT_NUMBER_LEN, "%s", c->number);
    scr_mgr_push(SCREEN12_1_ID, false);
}

static void scr12_populate(void)
{
    lv_obj_clean(scr12_list);

    int count = contacts_count();
    if(count == 0) {
        scr_empty_note_create(scr12_list,
            "No contacts yet.\n\nUse " LV_SYMBOL_PLUS " to add one, or save a number from the dialer.");
    }

    for(int i = 0; i < count; i++) {
        const contact_t *c = contacts_get(i);
        scr_row_create(scr12_list, c->name, c->number, NULL, scr12_row_event, (void *)(intptr_t)i);
    }

    scr12_built_revision = ui_contacts_revision;
}

static void scr12_back_event(lv_event_t *e)
{
    if(e->code == LV_EVENT_CLICKED) {
        ui_pick_mode  = UI_PICK_NONE;
        ui_pick_ready = false;
        scr_mgr_pop(false);
    }
}

static void scr12_add_event(lv_event_t *e)
{
    LV_UNUSED(e);

    ui_active_contact   = -1;
    ui_active_number[0] = '\0';
    scr_mgr_push(SCREEN12_2_ID, false);
}

static void create12(lv_obj_t *parent)
{
    scr12_list = scr_app_list_create(parent);
    scr12_populate();

    scr_back_btn_create(parent, ui_pick_mode == UI_PICK_NONE ? "Contacts" : "Choose contact",
                        scr12_back_event);
    if(ui_pick_mode == UI_PICK_NONE) {
        scr_action_btn_create(parent, LV_SYMBOL_PLUS, scr12_add_event);
    }
}

static void entry12(void)
{
    if(scr12_built_revision != ui_contacts_revision) {
        scr12_populate();
    }
    ui_disp_full_refr();
}

static void exit12(void)
{
    ui_disp_full_refr();
}

static void destroy12(void)
{
    scr12_list = NULL;
}

static scr_lifecycle_t screen12 = {
    .create = create12,
    .entry = entry12,
    .exit  = exit12,
    .destroy = destroy12,
};
#endif
// --------------------- screen 12.1 --------------------- contact details
#if 1
static void scr12_1_back_event(lv_event_t *e)
{
    if(e->code == LV_EVENT_CLICKED) {
        scr_mgr_pop(false);
    }
}

static void scr12_1_call_event(lv_event_t *e)
{
    LV_UNUSED(e);
    if(ui_active_number[0] == '\0') return;

    ui_phone_dial(ui_active_number);
    scr_mgr_push(SCREEN8_1_ID, false);
}

static void scr12_1_message_event(lv_event_t *e)
{
    LV_UNUSED(e);
    if(ui_active_number[0] == '\0') return;

    ui_compose_prefill[0] = '\0';
    scr_mgr_push(SCREEN13_2_ID, false);
}

static void scr12_1_edit_event(lv_event_t *e)
{
    LV_UNUSED(e);
    scr_mgr_push(SCREEN12_2_ID, false);
}

static void scr12_1_delete_event(lv_event_t *e)
{
    LV_UNUSED(e);

    if(contacts_remove(ui_active_contact)) {
        ui_active_contact = -1;
        ui_contacts_revision++;
        scr_mgr_pop(false); // the list rebuilds itself on the way back
    }
}

static void create12_1(lv_obj_t *parent)
{
    const contact_t *c = contacts_get(ui_active_contact);
    const char *name   = c ? c->name : ui_active_number;

    lv_obj_t *name_label = lv_label_create(parent);
    lv_obj_set_width(name_label, lv_pct(92));
    lv_obj_set_style_text_font(name_label, &Font_Mono_Bold_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(name_label, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_text_align(name_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(name_label, name);
    lv_obj_align(name_label, LV_ALIGN_TOP_MID, 0, 60);

    lv_obj_t *number_label = lv_label_create(parent);
    lv_obj_set_width(number_label, lv_pct(92));
    lv_obj_set_style_text_font(number_label, FONT_BOLD_SIZE_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(number_label, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_text_align(number_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(number_label, LV_LABEL_LONG_DOT);
    lv_obj_set_height(number_label, 18);
    lv_label_set_text(number_label, ui_active_number);
    lv_obj_align(number_label, LV_ALIGN_TOP_MID, 0, 104);

    lv_obj_t *bar = scr_action_bar_create(parent, 86);
    scr_bar_btn_create(bar, LV_SYMBOL_CALL "  Call", 106, scr12_1_call_event, NULL);
    scr_bar_btn_create(bar, LV_SYMBOL_ENVELOPE "  Text", 106, scr12_1_message_event, NULL);
    scr_bar_btn_create(bar, LV_SYMBOL_EDIT "  Edit", 106, scr12_1_edit_event, NULL);
    scr_bar_btn_create(bar, LV_SYMBOL_TRASH "  Delete", 106, scr12_1_delete_event, NULL);

    scr_back_btn_create(parent, "Contact", scr12_1_back_event);
}

static void entry12_1(void)
{
    ui_disp_full_refr();
}

static void exit12_1(void)
{
    ui_disp_full_refr();
}

static void destroy12_1(void) { }

static scr_lifecycle_t screen12_1 = {
    .create = create12_1,
    .entry = entry12_1,
    .exit  = exit12_1,
    .destroy = destroy12_1,
};
#endif
// --------------------- screen 12.2 --------------------- contact editor
#if 1
static lv_obj_t *scr12_2_name_ta   = NULL;
static lv_obj_t *scr12_2_number_ta = NULL;

static void scr12_2_back_event(lv_event_t *e)
{
    if(e->code == LV_EVENT_CLICKED) {
        scr_mgr_pop(false);
    }
}

/* Digits always go to the number field, wherever the keyboard focus happens to
 * be: the on-screen pad exists precisely because the number is the awkward
 * field to fill from the hardware keyboard. */
static void scr12_2_keypad_event(lv_event_t *e)
{
    lv_obj_t   *btnm = (lv_obj_t *)lv_event_get_target(e);
    uint32_t    id   = lv_btnmatrix_get_selected_btn(btnm);
    const char *txt  = lv_btnmatrix_get_btn_text(btnm, id);

    if(txt == NULL) return;

    if(strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) {
        lv_textarea_del_char(scr12_2_number_ta);
    } else {
        lv_textarea_add_text(scr12_2_number_ta, txt);
    }
}

static void scr12_2_save_event(lv_event_t *e)
{
    LV_UNUSED(e);

    const char *name   = lv_textarea_get_text(scr12_2_name_ta);
    const char *number = lv_textarea_get_text(scr12_2_number_ta);

    if(number == NULL || number[0] == '\0') return;

    if(ui_active_contact >= 0) {
        contacts_update(ui_active_contact, name, number);
    } else {
        contacts_add(name, number);
    }
    ui_contacts_revision++;

    ui_set_active_number(number);
    scr_mgr_pop(false);
}

static const char *scr12_2_keypad_map[] = { "1", "2", "3", "\n",
                                            "4", "5", "6", "\n",
                                            "7", "8", "9", "\n",
                                            "+", "0", LV_SYMBOL_BACKSPACE, ""
                                          };

static void create12_2(lv_obj_t *parent)
{
    const contact_t *c = contacts_get(ui_active_contact);

    scr12_2_name_ta = scr_field_create(parent, "Name", 36,
                                       c ? c->name : NULL, CONTACT_NAME_LEN - 1);
    scr12_2_number_ta = scr_field_create(parent, "Number", 96,
                                         c ? c->number : ui_active_number, CONTACT_NUMBER_LEN - 1);

    lv_obj_t *pad = lv_btnmatrix_create(parent);
    lv_btnmatrix_set_map(pad, scr12_2_keypad_map);
    lv_obj_set_size(pad, lv_pct(96), 112);
    lv_obj_set_style_border_width(pad, 0, LV_PART_MAIN);
    lv_obj_align(pad, LV_ALIGN_BOTTOM_MID, 0, -48);
    lv_obj_add_event_cb(pad, scr12_2_keypad_event, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *bar = scr_action_bar_create(parent, 38);
    scr_bar_btn_create(bar, LV_SYMBOL_OK "  Save", 106, scr12_2_save_event, NULL);
    scr_bar_btn_create(bar, LV_SYMBOL_CLOSE "  Cancel", 106, scr12_2_back_event, NULL);

    scr_back_btn_create(parent, ui_active_contact >= 0 ? "Edit contact" : "New contact",
                        scr12_2_back_event);
}

static void entry12_2(void)
{
    // The name is the field the hardware keyboard is for, so start there.
    lv_group_focus_obj(scr12_2_name_ta);
    ui_disp_full_refr();
}

static void exit12_2(void)
{
    ui_disp_full_refr();
}

static void destroy12_2(void)
{
    scr12_2_name_ta   = NULL;
    scr12_2_number_ta = NULL;
}

static scr_lifecycle_t screen12_2 = {
    .create = create12_2,
    .entry = entry12_2,
    .exit  = exit12_2,
    .destroy = destroy12_2,
};
#endif
//************************************[ screen 13 ]***************************************** messages
#if 1
static lv_obj_t *scr13_list = NULL;
static uint32_t  scr13_built_revision = 0;

static void scr13_row_event(lv_event_t *e)
{
    int thread = (int)(intptr_t)lv_event_get_user_data(e);
    const char *number = sms_thread_number(thread);
    if(number == NULL) return;

    ui_set_active_number(number);
    scr_mgr_push(SCREEN13_1_ID, false);
}

static void scr13_populate(void)
{
    lv_obj_clean(scr13_list);

    int threads = sms_thread_count();
    if(threads == 0) {
        scr_empty_note_create(scr13_list, "No messages yet.\n\nUse " LV_SYMBOL_PLUS " to write one.");
    }

    for(int i = 0; i < threads; i++) {
        const char      *number = sms_thread_number(i);
        const sms_msg_t *last   = sms_thread_last(i);
        char title[CONTACT_NAME_LEN + 8];
        char snippet[48];
        char badge[16];

        // An unread marker in the title is easier to spot on e-ink than a
        // separate dot would be.
        lv_snprintf(title, sizeof(title), "%s%s",
                    sms_thread_unread(number) > 0 ? LV_SYMBOL_ENVELOPE " " : "",
                    contacts_display_name(number));

        // A row previews the conversation; the whole message is one tap away.
        ui_message_snippet(snippet, sizeof(snippet), last ? last->text : "");
        if(last && last->dir == SMS_DIR_OUT) {
            // Without this a thread you last replied to reads as if they said it.
            char sent[sizeof(snippet)];
            lv_snprintf(sent, sizeof(sent), "You: %s", snippet);
            lv_snprintf(snippet, sizeof(snippet), "%s", sent);
        }

        ui_format_stamp_compact(badge, sizeof(badge), last ? last->ts : 0);

        scr_row_create(scr13_list, title, snippet, badge,
                       scr13_row_event, (void *)(intptr_t)i);
    }

    scr13_built_revision = ui_sms_revision;
}

static void scr13_back_event(lv_event_t *e)
{
    if(e->code == LV_EVENT_CLICKED) {
        scr_mgr_pop(false);
    }
}

static void scr13_new_event(lv_event_t *e)
{
    LV_UNUSED(e);

    ui_active_number[0]   = '\0';
    ui_active_contact     = -1;
    ui_compose_prefill[0] = '\0';
    scr_mgr_push(SCREEN13_2_ID, false);
}

static void create13(lv_obj_t *parent)
{
    scr13_list = scr_app_list_create(parent);
    scr13_populate();

    scr_back_btn_create(parent, "Messages", scr13_back_event);
    scr_action_btn_create(parent, LV_SYMBOL_PLUS, scr13_new_event);
}

static void entry13(void)
{
    if(scr13_built_revision != ui_sms_revision) {
        scr13_populate();
    }
    ui_disp_full_refr();
}

static void exit13(void)
{
    ui_disp_full_refr();
}

static void destroy13(void)
{
    scr13_list = NULL;
}

static scr_lifecycle_t screen13 = {
    .create = create13,
    .entry = entry13,
    .exit  = exit13,
    .destroy = destroy13,
};
#endif
// --------------------- screen 13.1 --------------------- conversation
#if 1
// Only the tail of a long conversation is built, to bound widget memory.
#define SCR13_1_MAX_BUBBLES 25

static lv_obj_t *scr13_1_cont = NULL;
static uint32_t  scr13_1_built_revision = 0;

static void scr13_1_populate(void);

static void scr13_1_back_event(lv_event_t *e)
{
    if(e->code == LV_EVENT_CLICKED) {
        scr_mgr_pop(false);
    }
}

static void scr13_1_reply_event(lv_event_t *e)
{
    LV_UNUSED(e);

    ui_compose_prefill[0] = '\0';
    scr_mgr_push(SCREEN13_2_ID, false);
}

static void scr13_1_call_event(lv_event_t *e)
{
    LV_UNUSED(e);
    if(ui_active_number[0] == '\0') return;

    ui_phone_dial(ui_active_number);
    scr_mgr_push(SCREEN8_1_ID, false);
}

// The message a tapped bubble refers to, as an index into the whole log.
static int scr13_1_delete_target = -1;

/* Removing an entry shifts every later index down, including the one the
 * pending-send watcher is holding on to. */
static void ui_sms_delete_at(int idx)
{
    if(!sms_delete(idx)) return;

    if(ui_send_watch_id != 0) {
        if(ui_send_watch_idx == idx)     ui_send_watch_id = 0; // the watched message is gone
        else if(ui_send_watch_idx > idx) ui_send_watch_idx--;
    }
    ui_sms_revision++;
}

static void scr13_1_do_delete_thread(void)
{
    sms_thread_delete(ui_active_number);

    // Absolute indexes are meaningless now that a whole conversation left.
    ui_send_watch_id = 0;
    ui_sms_revision++;
    scr_mgr_pop(false);
}

static void scr13_1_do_delete_message(void)
{
    ui_sms_delete_at(scr13_1_delete_target);
    scr13_1_delete_target = -1;

    // Nothing left to look at once the last message in the thread is gone.
    if(sms_thread_msg_count(ui_active_number) == 0) {
        scr_mgr_pop(false);
        return;
    }
    scr13_1_populate();
}

/* Tapping a message offers to delete just that one. */
static void scr13_1_bubble_event(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const sms_msg_t *m = sms_get(idx);
    if(m == NULL) return;

    char preview[64];
    ui_message_snippet(preview, sizeof(preview), m->text);

    scr13_1_delete_target = idx;
    ui_confirm("Delete message?", preview, LV_SYMBOL_TRASH "  Delete",
               scr13_1_do_delete_message);
}

static void scr13_1_delete_event(lv_event_t *e)
{
    LV_UNUSED(e);

    ui_confirm("Delete conversation?",
               "Every message in this conversation is removed from the phone.",
               LV_SYMBOL_TRASH "  Delete", scr13_1_do_delete_thread);
}

/* One message, placed at `y` in the conversation; returns the y the next
 * bubble should start at. Received messages sit against the left edge, sent
 * ones are pushed right, which is the only cue this display needs to tell them
 * apart. The bubbles are positioned by hand rather than by a flex layout
 * because this build of LVGL has no margin properties to offset one child
 * against the rest. */
static lv_coord_t scr13_1_bubble_create(lv_obj_t *parent, const sms_msg_t *m, int log_idx,
                                        lv_coord_t y)
{
    char head[48];
    char stamp[16];

    ui_format_stamp(stamp, sizeof(stamp), m->ts);

    if(m->dir == SMS_DIR_OUT) {
        const char *mark = (m->status == SMS_ST_PENDING) ? "  sending..."
                         : (m->status == SMS_ST_FAILED)  ? "  not sent"
                                                         : "";
        lv_snprintf(head, sizeof(head), "%s%s", stamp, mark);
    } else {
        lv_snprintf(head, sizeof(head), "%s", stamp);
    }

    lv_obj_t *bubble = lv_obj_create(parent);
    lv_obj_set_width(bubble, 180);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(bubble, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_border_color(bubble, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_border_width(bubble, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(bubble, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bubble, 5, LV_PART_MAIN);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(bubble, m->dir == SMS_DIR_OUT ? 42 : 0, y);

    // Tapping a bubble is how a single message gets deleted. A plain object is
    // not clickable by default.
    lv_obj_add_flag(bubble, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(bubble, scr13_1_bubble_event, LV_EVENT_CLICKED, (void *)(intptr_t)log_idx);

    lv_obj_t *text = lv_label_create(bubble);
    lv_obj_set_width(text, lv_pct(100));
    lv_obj_set_style_text_font(text, FONT_BOLD_SIZE_15, LV_PART_MAIN);
    lv_obj_set_style_text_color(text, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
    lv_label_set_text_fmt(text, "%s\n%s", head, m->text);
    if(m->dir == SMS_DIR_OUT) {
        lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    }

    // The height only exists once the wrapped text has been laid out.
    lv_obj_update_layout(bubble);
    return y + lv_obj_get_height(bubble) + 6;
}

static void scr13_1_populate(void)
{
    lv_obj_clean(scr13_1_cont);

    int total = sms_thread_msg_count(ui_active_number);
    int first = total > SCR13_1_MAX_BUBBLES ? total - SCR13_1_MAX_BUBBLES : 0;

    if(total == 0) {
        scr_empty_note_create(scr13_1_cont, "No messages in this conversation.");
    }

    lv_coord_t y = 0;
    for(int i = first; i < total; i++) {
        int              log_idx = sms_thread_msg_index(ui_active_number, i);
        const sms_msg_t *m       = sms_get(log_idx);
        if(m) y = scr13_1_bubble_create(scr13_1_cont, m, log_idx, y);
    }

    // Open on the newest message, the way a conversation is normally read.
    lv_obj_update_layout(scr13_1_cont);
    lv_obj_scroll_to_y(scr13_1_cont, lv_obj_get_scroll_bottom(scr13_1_cont), LV_ANIM_OFF);

    scr13_1_built_revision = ui_sms_revision;
}

static void create13_1(lv_obj_t *parent)
{
    scr13_1_cont = lv_obj_create(parent);
    lv_obj_set_size(scr13_1_cont, lv_pct(96), LV_VER_RES - 36 - 44);
    lv_obj_align(scr13_1_cont, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_set_style_bg_color(scr13_1_cont, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr13_1_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr13_1_cont, 2, LV_PART_MAIN);
    lv_obj_set_scroll_dir(scr13_1_cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scr13_1_cont, LV_SCROLLBAR_MODE_OFF);

    scr13_1_populate();

    // "All" rather than a bare bin, so it is clear this button is not the way
    // to remove one message - tapping the message itself is.
    lv_obj_t *bar = scr_action_bar_create(parent, 38);
    scr_bar_btn_create(bar, LV_SYMBOL_EDIT "  Reply", 76, scr13_1_reply_event, NULL);
    scr_bar_btn_create(bar, LV_SYMBOL_CALL "  Call", 68, scr13_1_call_event, NULL);
    scr_bar_btn_create(bar, LV_SYMBOL_TRASH " All", 64, scr13_1_delete_event, NULL);

    scr_back_btn_create(parent, contacts_display_name(ui_active_number), scr13_1_back_event);
}

static void entry13_1(void)
{
    sms_thread_mark_read(ui_active_number);

    // Rebuild when the log moved on beneath us - a reply we just sent, or a
    // message that arrived while the composer was open.
    if(scr13_1_built_revision != ui_sms_revision) {
        scr13_1_populate();
    }
    ui_disp_full_refr();
}

static void exit13_1(void)
{
    ui_disp_full_refr();
}

static void destroy13_1(void)
{
    scr13_1_cont = NULL;
}

static scr_lifecycle_t screen13_1 = {
    .create = create13_1,
    .entry = entry13_1,
    .exit  = exit13_1,
    .destroy = destroy13_1,
};
#endif
// --------------------- screen 13.2 --------------------- compose
#if 1
static lv_obj_t   *scr13_2_to_label = NULL;
static lv_obj_t   *scr13_2_body_ta  = NULL;
static lv_obj_t   *scr13_2_status   = NULL;
static lv_timer_t *scr13_2_timer    = NULL;

static void scr13_2_back_event(lv_event_t *e)
{
    if(e->code == LV_EVENT_CLICKED) {
        scr_mgr_pop(false);
    }
}

static void scr13_2_update_to(void)
{
    if(scr13_2_to_label == NULL) return;

    if(ui_active_number[0] == '\0') {
        lv_label_set_text(scr13_2_to_label, "To: (choose a contact)");
    } else {
        lv_label_set_text_fmt(scr13_2_to_label, "To: %s", contacts_display_name(ui_active_number));
    }
}

static void scr13_2_pick_event(lv_event_t *e)
{
    LV_UNUSED(e);

    ui_pick_mode = UI_PICK_COMPOSE;
    scr_mgr_push(SCREEN12_ID, false);
}

/* Reports how the send that this screen started ended up. The log itself is
 * corrected centrally, so this only has to keep the label honest while the
 * screen is open. */
static void scr13_2_status_timer(lv_timer_t *t)
{
    LV_UNUSED(t);

    if(ui_send_watch_id != 0) return; // still with the modem

    const sms_msg_t *m = sms_get(ui_send_watch_idx);
    lv_label_set_text(scr13_2_status,
                      (m && m->status == SMS_ST_FAILED) ? "Could not send" : "Sent");

    lv_timer_del(scr13_2_timer);
    scr13_2_timer = NULL;
    ui_disp_full_refr();
}

static void scr13_2_send_event(lv_event_t *e)
{
    LV_UNUSED(e);

    const char *body = lv_textarea_get_text(scr13_2_body_ta);

    if(ui_active_number[0] == '\0') {
        lv_label_set_text(scr13_2_status, "Choose who to send to first");
        return;
    }
    if(body == NULL || body[0] == '\0') {
        lv_label_set_text(scr13_2_status, "Nothing to send");
        return;
    }
    if(ui_send_watch_id != 0) {
        lv_label_set_text(scr13_2_status, "Still sending the last one");
        return;
    }

    uint32_t send_id = ui_sms_send(ui_active_number, body);
    if(send_id == 0) {
        lv_label_set_text(scr13_2_status, "Modem is busy, try again");
        return;
    }

    // Log it straight away as pending so the conversation reads correctly
    // however long the network takes.
    ui_send_watch_idx = sms_add(ui_active_number, body, (uint32_t)time(NULL),
                                SMS_DIR_OUT, SMS_ST_PENDING, false);
    ui_send_watch_id  = send_id;
    ui_sms_revision++;

    lv_textarea_set_text(scr13_2_body_ta, "");
    lv_label_set_text(scr13_2_status, "Sending...");

    if(scr13_2_timer == NULL) {
        scr13_2_timer = lv_timer_create(scr13_2_status_timer, 500, NULL);
    }
}

static void create13_2(lv_obj_t *parent)
{
    scr13_2_to_label = lv_label_create(parent);
    lv_obj_set_width(scr13_2_to_label, lv_pct(92));
    lv_obj_set_style_text_font(scr13_2_to_label, FONT_BOLD_SIZE_15, LV_PART_MAIN);
    lv_obj_set_style_text_color(scr13_2_to_label, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_label_set_long_mode(scr13_2_to_label, LV_LABEL_LONG_DOT);
    lv_obj_set_height(scr13_2_to_label, 18);
    lv_obj_align(scr13_2_to_label, LV_ALIGN_TOP_LEFT, 10, 38);
    scr13_2_update_to();

    scr13_2_body_ta = lv_textarea_create(parent);
    lv_textarea_set_max_length(scr13_2_body_ta, SMS_TEXT_LEN - 1);
    lv_textarea_set_placeholder_text(scr13_2_body_ta, "Message");
    lv_obj_set_size(scr13_2_body_ta, lv_pct(92), 148);
    lv_obj_set_style_text_font(scr13_2_body_ta, FONT_BOLD_SIZE_15, LV_PART_MAIN);
    lv_obj_align(scr13_2_body_ta, LV_ALIGN_TOP_MID, 0, 60);
    if(ui_compose_prefill[0]) lv_textarea_set_text(scr13_2_body_ta, ui_compose_prefill);

    scr13_2_status = lv_label_create(parent);
    lv_obj_set_width(scr13_2_status, lv_pct(92));
    lv_obj_set_style_text_font(scr13_2_status, FONT_BOLD_SIZE_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(scr13_2_status, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(scr13_2_status, " ");
    lv_obj_align(scr13_2_status, LV_ALIGN_TOP_LEFT, 10, 216);

    lv_obj_t *bar = scr_action_bar_create(parent, 38);
    scr_bar_btn_create(bar, LV_SYMBOL_UP "  Send", 106, scr13_2_send_event, NULL);
    scr_bar_btn_create(bar, LV_SYMBOL_LIST "  To", 106, scr13_2_pick_event, NULL);

    scr_back_btn_create(parent, "New message", scr13_2_back_event);
}

static void entry13_2(void)
{
    if(ui_pick_ready && ui_pick_mode == UI_PICK_COMPOSE) {
        scr13_2_update_to();
    }
    ui_pick_ready = false;
    ui_pick_mode  = UI_PICK_NONE;

    lv_group_focus_obj(scr13_2_body_ta);
    ui_disp_full_refr();
}

static void exit13_2(void)
{
    // Keep an unsent draft so stepping into the contact picker is not
    // destructive; a sent message clears the field before we get here.
    const char *body = lv_textarea_get_text(scr13_2_body_ta);
    lv_snprintf(ui_compose_prefill, SMS_TEXT_LEN, "%s", body ? body : "");

    ui_disp_full_refr();
}

static void destroy13_2(void)
{
    if(scr13_2_timer) {
        lv_timer_del(scr13_2_timer);
        scr13_2_timer = NULL;
    }
    scr13_2_to_label = NULL;
    scr13_2_body_ta  = NULL;
    scr13_2_status   = NULL;
}

static scr_lifecycle_t screen13_2 = {
    .create = create13_2,
    .entry = entry13_2,
    .exit  = exit13_2,
    .destroy = destroy13_2,
};
#endif

//************************************[ screen 9 ]****************************************** Shutdown
#if 1
static lv_timer_t *shutdown_timer = NULL;

static void scr9_btn_event_cb(lv_event_t * e)
{
    if(e->code == LV_EVENT_CLICKED){
        scr_mgr_pop(false);
    }
}

static void shutdown_timer_event(lv_timer_t* t)
{
    ui_shutdown_on();
    lv_timer_del(t);
}

static void create9(lv_obj_t *parent)
{
    if(ui_battery_25896_is_vbus_in()) 
    {
        lv_obj_t * label = lv_label_create(parent);
        lv_obj_set_width(label, lv_pct(95));
        lv_obj_set_style_text_font(label, FONT_BOLD_SIZE_15, LV_PART_MAIN);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_label_set_text(label, "The shutdown function can only be used when the "
                            "battery is connected alone, and cannot be shut down when connected to USB.");
        lv_obj_center(label);

        // back 
        scr_back_btn_create(parent, "Shoutdown", scr8_btn_event_cb);
    } 
    else 
    {
        lv_obj_t * img = lv_img_create(parent);
        lv_img_set_src(img, &img_start);
        lv_obj_center(img);

        lv_timer_create(shutdown_timer_event, 2000, (void *)parent);
    }
}
static void entry9(void) 
{
    ui_disp_full_refr();
}
static void exit9(void) {
    ui_disp_full_refr();
}
static void destroy9(void) { }

static scr_lifecycle_t screen9 = {
    .create = create9,
    .entry = entry9,
    .exit  = exit9,
    .destroy = destroy9,
};
#endif
//************************************[ screen 10 ]***************************************** pcm5102

//************************************[ screen 11 ]****************************************** Sleep
#if 1
#include <TouchDrvCSTXXX.hpp>
static void scr11_btn_event_cb(lv_event_t * e)
{
    if(e->code == LV_EVENT_CLICKED){
        scr_mgr_pop(false);
    }
}

static void create11(lv_obj_t *parent)
{
    extern TouchDrvCSTXXX touch;

    touch.sleep();

    lora_sleep();

    SerialGPS.end();
    
    // pinMode(BOARD_GPS_PPS, OUTPUT);
    // pinMode(BOARD_GPS_RXD, OUTPUT);
    // pinMode(BOARD_GPS_TXD, OUTPUT);
    // pinMode(BOARD_LORA_RST, OUTPUT);
    // pinMode(BOARD_TOUCH_RST, OUTPUT);
    // pinMode(BOARD_LORA_BUSY, OUTPUT);

    // digitalWrite(BOARD_GPS_PPS, LOW);
    // digitalWrite(BOARD_GPS_RXD, LOW);
    // digitalWrite(BOARD_GPS_TXD, LOW);
    // digitalWrite(BOARD_LORA_RST, LOW);
    // digitalWrite(BOARD_TOUCH_RST, LOW);
    // digitalWrite(BOARD_LORA_BUSY, LOW);

    gpio_reset_pin((gpio_num_t)BOARD_GPS_PPS);
    gpio_reset_pin((gpio_num_t)BOARD_GPS_RXD);
    gpio_reset_pin((gpio_num_t)BOARD_GPS_TXD);
    gpio_reset_pin((gpio_num_t)BOARD_LORA_RST);
    gpio_reset_pin((gpio_num_t)BOARD_TOUCH_RST);
    gpio_reset_pin((gpio_num_t)BOARD_LORA_BUSY);

    digitalWrite(BOARD_6609_EN, LOW);
    digitalWrite(BOARD_LORA_EN, LOW);
    digitalWrite(BOARD_GPS_EN, LOW);
    
    digitalWrite(BOARD_1V8_EN, LOW);
    digitalWrite(BOARD_A7682E_PWRKEY, LOW);

    // gpio_hold_en((gpio_num_t)BOARD_GPS_PPS);
    // gpio_hold_en((gpio_num_t)BOARD_TOUCH_RST);
    // gpio_hold_en((gpio_num_t)BOARD_GPS_RXD);
    // gpio_hold_en((gpio_num_t)BOARD_GPS_TXD);
    // gpio_hold_en((gpio_num_t)BOARD_LORA_RST);
    // gpio_hold_en((gpio_num_t)BOARD_LORA_BUSY);
    gpio_hold_en((gpio_num_t)BOARD_6609_EN);
    gpio_hold_en((gpio_num_t)BOARD_LORA_EN);
    gpio_hold_en((gpio_num_t)BOARD_GPS_EN);
    gpio_hold_en((gpio_num_t)BOARD_1V8_EN);
    gpio_hold_en((gpio_num_t)BOARD_A7682E_PWRKEY);
    gpio_deep_sleep_hold_en();

    
    // esp_sleep_enable_ext0_wakeup((gpio_num_t)ENCODER_KEY, 0);                            
    esp_sleep_enable_ext1_wakeup((1UL << BOARD_BOOT_PIN), ESP_EXT1_WAKEUP_ANY_LOW);   // Hibernate using user keys
    esp_deep_sleep_start();

    // back 
    scr_back_btn_create(parent, "Sleep", scr8_btn_event_cb);
}
static void entry11(void) 
{
    ui_disp_full_refr();
}
static void exit11(void) {
    ui_disp_full_refr();
}
static void destroy11(void) { }

static scr_lifecycle_t screen11 = {
    .create = create11,
    .entry = entry11,
    .exit  = exit11,
    .destroy = destroy11,
};
#endif
//************************************[ UI ENTRY ]******************************************
static lv_obj_t *menu_keypad;
static lv_timer_t *menu_timer = NULL;

static void indev_get_gesture_dir(lv_timer_t *t)
{
    lv_indev_data_t data;
    lv_indev_t * indev_pointer = lv_indev_get_next(NULL);
    lv_coord_t diff_x = 0;
    lv_coord_t diff_y = 0;

    static lv_point_t last_point;
    static bool is_press = false;

    _lv_indev_read(indev_pointer, &data);

    if(data.state == LV_INDEV_STATE_PR){

        if(is_press == false) {
            is_press = true;
            last_point = data.point;
        }

        diff_x = last_point.x - data.point.x;
        diff_y = last_point.x - data.point.y;

        if(diff_x > UI_SLIDING_DISTANCE) { // right
            if(ui_get_gesture_dir) {
                ui_get_gesture_dir(LV_DIR_LEFT);
            }
            last_point.x = 0;
        } else if(diff_x < -UI_SLIDING_DISTANCE) { // left
            if(ui_get_gesture_dir) {
                ui_get_gesture_dir(LV_DIR_RIGHT);
            }
            last_point.x = 0;
        }
        // Serial.printf("x=%d, y=%d\n", data.point.x, data.point.y);
    }else{
        is_press = false;
        last_point.x = 0;
        last_point.y = 0;
    }
}

static void menu_keypay_get_event(lv_timer_t *t)
{
    static int sec = 0;
    static int press = false;
    char keypay_v;
    int ret = ui_input_get_keypay_val(&keypay_v);

    if(ret > 0)
    {
        sec = 0;
        press = true;
        ui_input_set_keypay_flag();
        lv_label_set_text_fmt(menu_keypad, "%c", keypay_v);
    }

    if(press){
        sec++;
        if(sec > 20) {
            sec = 0;
            press = false;
            lv_label_set_text(menu_keypad, " ");
        }
    }
}

static void menu_taskbar_update_timer_cb(lv_timer_t *t)
{

    struct tm timeinfo;
    time_t now;

    static int tick = 0;
    tick++;
    static int last_hour = 0;
    static int last_min = 0;
    bool charge = 0;
    bool finish = 0;
    bool wifi = 0;
    int percent = 0;

    time(&now);
    localtime_r(&now, &timeinfo);
    if(last_hour != timeinfo.tm_hour || last_min != timeinfo.tm_min) {
        lv_label_set_text_fmt(menu_taskbar_time, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
        last_hour = timeinfo.tm_hour;
        last_min = timeinfo.tm_min;
    }

    if(tick % 10 == 0)
    {
        finish = ui_battery_27220_get_charge_finish();
        percent = ui_battery_27220_get_percent();

        if(taskbar_statue[TASKBAR_ID_CHARGE_FINISH] != finish) 
        {
            if(finish){
                lv_label_set_text_fmt(menu_taskbar_charge, "%s", LV_SYMBOL_OK);
            } else {
                lv_label_set_text_fmt(menu_taskbar_charge, "%s", LV_SYMBOL_CHARGE);
            }
            taskbar_statue[TASKBAR_ID_CHARGE_FINISH] = finish;
        }

        if(taskbar_statue[TASKBAR_ID_BATTERY_PERCENT] != percent) 
        {
            lv_label_set_text_fmt(menu_taskbar_battery_percent, "%d", percent);
            lv_label_set_text_fmt(menu_taskbar_battery, "%s", ui_battert_27220_get_percent_level());
            taskbar_statue[TASKBAR_ID_BATTERY_PERCENT] = percent;
        }
    }
    

    bool registered = ui_phone_is_registered();
    if(taskbar_statue[TASKBAR_ID_SIGNAL] != (uint16_t)registered)
    {
        if(registered) {
            lv_obj_clear_flag(menu_taskbar_signal, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(menu_taskbar_signal, LV_OBJ_FLAG_HIDDEN);
        }
        taskbar_statue[TASKBAR_ID_SIGNAL] = registered;
    }

    uint16_t unread = sms_unread_total();
    if(taskbar_statue[TASKBAR_ID_UNREAD] != unread)
    {
        if(unread > 0) {
            lv_obj_clear_flag(menu_taskbar_unread, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(menu_taskbar_unread, LV_OBJ_FLAG_HIDDEN);
        }
        taskbar_statue[TASKBAR_ID_UNREAD] = unread;
    }

    charge = ui_battery_27220_get_input();
    if(taskbar_statue[TASKBAR_ID_CHARGE] != charge)
    {
        if(charge) {
            lv_obj_clear_flag(menu_taskbar_charge, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(menu_taskbar_charge, LV_OBJ_FLAG_HIDDEN);
        }
        taskbar_statue[TASKBAR_ID_CHARGE] = charge;
    }

}

/* Runs for as long as the phone is on, whatever screen is showing. This is
 * what makes the device answerable: a call or a message can arrive while the
 * user is in the settings, and something has to notice.
 *
 * It also owns the outcome of an outgoing message, so navigating away from the
 * composer still leaves the log with the right delivery status. */
static void phone_event_timer_cb(lv_timer_t *t)
{
    LV_UNUSED(t);

    modem_sms_rx_t rx;
    while(ui_sms_poll_received(&rx)) {
        sms_add(rx.number, rx.text, rx.ts, SMS_DIR_IN, SMS_ST_OK, true);
        ui_sms_revision++;
        ui_phone_vibrate(250);
    }

    if(ui_send_watch_id != 0) {
        modem_send_state_t st = ui_sms_get_send_state(ui_send_watch_id);
        if(st == MODEM_SEND_OK || st == MODEM_SEND_FAILED) {
            sms_set_status(ui_send_watch_idx, st == MODEM_SEND_OK ? SMS_ST_OK : SMS_ST_FAILED);
            ui_send_watch_id = 0;
            ui_sms_revision++;
        }
    }

    // Raise the call screen when a call starts ringing. Only on the transition:
    // pushing it back whenever the state happens to be INCOMING would fight a
    // user who deliberately backed out of it.
    static modem_call_state_t last_call_state = MODEM_CALL_IDLE;
    modem_call_state_t call_state = ui_phone_get_call_state();

    if(call_state == MODEM_CALL_INCOMING && last_call_state != MODEM_CALL_INCOMING &&
       scr_mgr_current_id() != SCREEN8_1_ID) {
        scr_mgr_push(SCREEN8_1_ID, false);
    }
    last_call_state = call_state;
}

void ui_phone1_entry(void)
{
    lv_disp_t *disp = lv_disp_get_default();
    disp->theme = lv_theme_mono_init(disp, false, LV_FONT_DEFAULT);

    touch_chk_timer = lv_timer_create(indev_get_gesture_dir, LV_INDEV_DEF_READ_PERIOD, NULL);
    lv_timer_pause(touch_chk_timer);

    lv_timer_create(phone_event_timer_cb, 500, NULL);

    taskbar_update_timer = lv_timer_create(menu_taskbar_update_timer_cb, 1000, NULL);
    lv_timer_pause(taskbar_update_timer);

    scr_mgr_init();

    scr_mgr_register(SCREEN0_ID,    &screen0);      // menu
    scr_mgr_register(SCREEN1_ID,    &screen1);      // Lora
    scr_mgr_register(SCREEN1_1_ID,  &screen1_1);    // - Auto send
    scr_mgr_register(SCREEN2_ID,    &screen2);      // Setting
    scr_mgr_register(SCREEN2_1_ID,  &screen2_1);    //  - Time
    scr_mgr_register(SCREEN2_1_1_ID,&screen2_1_1);  //     - time zone picker
    scr_mgr_register(SCREEN2_2_ID,  &screen2_2);    //  - About System
    scr_mgr_register(SCREEN3_ID,    &screen3);      // 
    scr_mgr_register(SCREEN4_ID,    &screen4);      // WIFI
    scr_mgr_register(SCREEN4_1_ID,  &screen4_1);    //  - WIFI Config
    scr_mgr_register(SCREEN4_2_ID,  &screen4_2);    //  - WIFI Scan
    scr_mgr_register(SCREEN5_ID,    &screen5);      // 
    scr_mgr_register(SCREEN6_ID,    &screen6);      // Battery
    scr_mgr_register(SCREEN6_1_ID,  &screen6_1);    //  - BQ25896
    scr_mgr_register(SCREEN6_2_ID,  &screen6_2);    //  - BQ27220
    scr_mgr_register(SCREEN8_ID,    &screen8);      // Phone - dialer
    scr_mgr_register(SCREEN8_1_ID,  &screen8_1);    //  - in call
    scr_mgr_register(SCREEN9_ID,    &screen9);      // Shutdown
    scr_mgr_register(SCREEN11_ID,   &screen11);
    scr_mgr_register(SCREEN12_ID,   &screen12);     // Contacts
    scr_mgr_register(SCREEN12_1_ID, &screen12_1);   //  - details
    scr_mgr_register(SCREEN12_2_ID, &screen12_2);   //  - editor
    scr_mgr_register(SCREEN13_ID,   &screen13);     // Messages
    scr_mgr_register(SCREEN13_1_ID, &screen13_1);   //  - conversation
    scr_mgr_register(SCREEN13_2_ID, &screen13_2);   //  - compose
    

    scr_mgr_switch(SCREEN0_ID, false); // set root screen
    scr_mgr_set_anim(LV_SCR_LOAD_ANIM_OVER_LEFT, LV_SCR_LOAD_ANIM_OVER_LEFT, LV_SCR_LOAD_ANIM_OVER_LEFT);

    // menu_keypad = lv_label_create(lv_layer_top());
    // lv_obj_set_style_text_font(menu_keypad, FONT_BOLD_MONO_SIZE_15, LV_PART_MAIN);
    // lv_label_set_text(menu_keypad, " ");
    // lv_obj_align(menu_keypad, LV_ALIGN_BOTTOM_RIGHT, -10, -10);

    // menu_timer = lv_timer_create(menu_keypay_get_event, 40, NULL);
}