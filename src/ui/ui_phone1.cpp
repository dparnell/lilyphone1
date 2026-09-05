
#include "ui_phone1.h"
#include "assets.h"
#include "ui_fonts.h"
#include "stdio.h"
#include "ui_phone1_port.h"
#include "system_clock.h"
#include "timezone_db.h"
#include "udp_relay.h"
#include "mesh_net.h"
#include "mesh_companion.h"
#include "Arduino.h"

#define SETTING_PAGE_MAX_ITEM 7
#define GET_BUFF_LEN(a) sizeof(a)/sizeof(a[0])

/* The ui_font_mono_* wrappers, not Font_Mono_Bold_* directly: the raw fonts
 * have no icon glyphs and draw a placeholder box for any LV_SYMBOL_*. */
#define FONT_BOLD_SIZE_14 &ui_font_mono_14
#define FONT_BOLD_SIZE_15 &ui_font_mono_15
#define FONT_BOLD_SIZE_16 &ui_font_mono_16
#define FONT_BOLD_SIZE_17 &ui_font_mono_17
#define FONT_BOLD_SIZE_18 &ui_font_mono_18
#define FONT_BOLD_SIZE_19 &ui_font_mono_19
#define FONT_BOLD_SIZE_20 &ui_font_mono_20

#define FONT_BOLD_MONO_SIZE_14 &ui_font_mono_14
#define FONT_BOLD_MONO_SIZE_15 &ui_font_mono_15
#define FONT_BOLD_MONO_SIZE_16 &ui_font_mono_16
#define FONT_BOLD_MONO_SIZE_17 &ui_font_mono_17
#define FONT_BOLD_MONO_SIZE_18 &ui_font_mono_18
#define FONT_BOLD_MONO_SIZE_19 &ui_font_mono_19

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
    /* The arrow alone, sized as a target rather than fitted to the glyph: the
     * screen name used to sit beside it and served as extra area to press, so
     * now that the name is centred the button has to be big enough on its own. */
    lv_obj_t * btn = lv_btn_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_size(btn, 40, 30);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 3, 3);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_ext_click_area(btn, 10);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *arrow = lv_label_create(btn);
    lv_obj_align(arrow, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_color(arrow, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_label_set_text(arrow, LV_SYMBOL_LEFT);

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

    return btn;
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
static void ui_notice_event(lv_event_t *e)
{
    lv_msgbox_close(lv_event_get_current_target(e));
    ui_disp_full_refr();
}

/* Says something happened and gets out of the way. Used where an action can
 * fail with nothing on screen to show it - a reaction has no status line of
 * its own the way the composer does. */
static const char *ui_notice_btns[2];

static void ui_notice(const char *title, const char *body)
{
    ui_notice_btns[0] = "OK";
    ui_notice_btns[1] = "";

    lv_obj_t *mbox = lv_msgbox_create(NULL, title, body, ui_notice_btns, false);
    lv_obj_set_width(mbox, lv_pct(88));
    lv_obj_set_style_bg_color(mbox, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_text_color(mbox, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_border_color(mbox, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_border_width(mbox, 2, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(mbox, 0, LV_PART_MAIN);
    lv_obj_set_style_text_font(mbox, FONT_BOLD_SIZE_15, LV_PART_MAIN);
    lv_obj_center(mbox);
    lv_obj_set_style_bg_opa(lv_obj_get_parent(mbox), LV_OPA_TRANSP, LV_PART_MAIN);

    lv_obj_t *btns = lv_msgbox_get_btns(mbox);
    if(btns) lv_obj_set_size(btns, lv_pct(100), 40);

    lv_obj_add_event_cb(mbox, ui_notice_event, LV_EVENT_VALUE_CHANGED, NULL);
    ui_disp_full_refr();
}

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

/* Scrolling that this panel can keep up with.
 *
 * LVGL gives every object elastic overshoot and momentum. Both keep animating
 * after the finger lifts, and on e-paper every frame of that is a full repaint
 * that the display cannot deliver in time - so the list crawls along behind a
 * queue of them. One-to-one scrolling that stops when the finger stops is the
 * only kind worth having here. */
static void scr_scroll_for_epaper(lv_obj_t *obj)
{
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLL_ELASTIC);
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
    scr_scroll_for_epaper(list);
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
static lv_obj_t * menu_taskbar_companion = NULL;

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
    {SCREEN1_ID,  &img_lora,    NULL,                "Mesh",     23,     189},
    {SCREEN6_ID,  &img_batt,    NULL,                "Battery",  95,     189},
    {SCREEN5_ID,  &img_test,    NULL,                "Test",     167,    189},

    {SCREEN16_ID, NULL,         LV_SYMBOL_WIFI,      "Hotspot",  23,     13},  // Page two
    {SCREEN11_ID, &img_PCM5102, NULL,                "Sleep",    95,     13},
    {SCREEN9_ID,  NULL,         LV_SYMBOL_POWER,     "Shutdown", 167,    13},
};

static void menu_btn_event_cb(lv_event_t *e)
{
    struct menu_btn *tgr = (struct menu_btn *)e->user_data;
    scr_mgr_push(tgr->idx, false);
}

static void menu_get_gesture_dir(int dir, lv_coord_t from_x, lv_coord_t from_y)
{
    LV_UNUSED(from_x);

    /* Pulled down from the top edge: the quick settings. Started further down
     * the screen it is just a stray drag across the icons, so it is ignored. */
    if(dir == LV_DIR_BOTTOM) {
        if(from_y <= UI_SHADE_PULL_ZONE) scr_mgr_push(SCREEN14_ID, false);
        return;
    }
    if(dir == LV_DIR_TOP) return;

    if(MENU_BTN_NUM <= 9) return;

    if(dir == LV_DIR_LEFT) {
        if(page_curr < page_num){
            page_curr++;
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

    // Swapping pages replaces everything below the taskbar, so tell the driver
    // the whole screen changed and let it decide whether to flash.
    ui_disp_full_refr();
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
    lv_obj_set_style_text_font(menu_taskbar_time, FONT_BOLD_SIZE_14, LV_PART_MAIN);
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

    /* And a mark that a companion app is driving the mesh, since from here that
     * is invisible - the phone in your pocket is doing the talking. Drawn as
     * whichever link it came in over, so it says how as well as whether. */
    menu_taskbar_companion = lv_label_create(status_parent);
    lv_label_set_text_fmt(menu_taskbar_companion, "%s", LV_SYMBOL_BLUETOOTH);
    lv_obj_add_flag(menu_taskbar_companion, LV_OBJ_FLAG_HIDDEN);

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

    /* The icons are created hidden, but the state they reflect outlived the
     * last time this screen existed - and the refresh only acts on a *change*,
     * so anything already true when the menu is rebuilt would stay invisible
     * until it happened to change again. */
    if(taskbar_statue[TASKBAR_ID_UNREAD])
        lv_obj_clear_flag(menu_taskbar_unread, LV_OBJ_FLAG_HIDDEN);

    if(taskbar_statue[TASKBAR_ID_SIGNAL])
        lv_obj_clear_flag(menu_taskbar_signal, LV_OBJ_FLAG_HIDDEN);

    if(taskbar_statue[TASKBAR_ID_COMPANION]) {
        lv_label_set_text(menu_taskbar_companion,
                          mesh_companion_get_link() == MESH_LINK_WIFI ? LV_SYMBOL_WIFI
                                                                     : LV_SYMBOL_BLUETOOTH);
        lv_obj_clear_flag(menu_taskbar_companion, LV_OBJ_FLAG_HIDDEN);
    }

    menu_taskbar_battery = lv_label_create(status_parent);
    
    menu_taskbar_battery_percent = lv_label_create(status_parent);
    lv_obj_set_style_text_font(menu_taskbar_battery_percent, FONT_BOLD_SIZE_14, LV_PART_MAIN);

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
//************************************[ screen 1 ]****************************************** mesh
#if 1
/* The nodes this phone can hear on the MeshCore network, and the public channel
 * they all share. This doubles as the list of people to talk to: MeshCore nodes
 * announce themselves periodically, so it fills in on its own once the radio is
 * listening and there is nobody to add by hand.
 *
 * An empty list after a few minutes means nobody is in range on these settings,
 * which is worth telling apart from a radio that never started, hence the
 * packet counts in the header. */
static lv_obj_t   *scr1_list   = NULL;
static lv_obj_t   *scr1_header = NULL;
static lv_timer_t *scr1_timer  = NULL;

/* Which mesh conversation the conversation and compose screens are showing.
 * The channel is not anybody's key, so it gets its own flag rather than a
 * reserved value. */
static uint8_t ui_mesh_peer[4]  = { 0 };
static bool    ui_mesh_channel  = true;
static char    ui_mesh_title[MESH_NET_NAME_LEN] = "";

/* What the mesh_net chat calls take: NULL is the public channel. */
static const uint8_t *ui_mesh_target(void)
{
    return ui_mesh_channel ? NULL : ui_mesh_peer;
}

static void scr1_btn_event_cb(lv_event_t * e)
{
    if(e->code == LV_EVENT_CLICKED){
        scr_mgr_pop(false);
    }
}

static void scr1_self_event(lv_event_t *e)
{
    LV_UNUSED(e);
    scr_mgr_push(SCREEN1_1_ID, false);
}

static void scr1_channel_event(lv_event_t *e)
{
    LV_UNUSED(e);

    ui_mesh_channel = true;
    lv_snprintf(ui_mesh_title, sizeof(ui_mesh_title), "%s", mesh_net_channel_name());
    scr_mgr_push(SCREEN1_4_ID, false);
}

static void scr1_node_event(lv_event_t *e)
{
    mesh_node_t node;

    if(!mesh_net_get_node((int)(intptr_t)lv_event_get_user_data(e), &node)) return;

    ui_mesh_channel = false;
    memcpy(ui_mesh_peer, node.pubkey_prefix, sizeof(ui_mesh_peer));
    lv_snprintf(ui_mesh_title, sizeof(ui_mesh_title), "%s", node.name);
    scr_mgr_push(SCREEN1_4_ID, false);
}

/* How long ago, in the shortest form that is still honest. */
static void scr1_ago(char *buf, int len, uint32_t heard_ms)
{
    uint32_t secs = (millis() - heard_ms) / 1000;

    if(secs < 60)        lv_snprintf(buf, len, "%us", (unsigned)secs);
    else if(secs < 3600) lv_snprintf(buf, len, "%um", (unsigned)(secs / 60));
    else                 lv_snprintf(buf, len, "%uh", (unsigned)(secs / 3600));
}

static void scr1_populate(void)
{
    char detail[64];
    char right[16];

    lv_obj_clean(scr1_list);

    /* The channel first, because it is the one conversation that is there
     * before anybody has been heard from. */
    int unread = mesh_net_unread(NULL);
    lv_snprintf(detail, sizeof(detail), "Everyone in range");
    if(unread > 0) lv_snprintf(right, sizeof(right), "%d new", unread);
    else           right[0] = '\0';

    scr_row_create(scr1_list, mesh_net_channel_name(), detail,
                   right[0] ? right : NULL, scr1_channel_event, NULL);

    int count = mesh_net_node_count();

    if(count == 0) {
        scr_empty_note_create(scr1_list,
            mesh_net_is_running()
                ? "Listening.\n\nNodes appear here as they announce themselves."
                : "The radio did not start.");
        return;
    }

    for(int i = 0; i < count; i++) {
        mesh_node_t node;
        if(!mesh_net_get_node(i, &node)) continue;

        scr1_ago(right, sizeof(right), node.heard_ms);
        if(node.unread > 0) lv_snprintf(right, sizeof(right), "%d new", node.unread);

        lv_snprintf(detail, sizeof(detail), "%02X%02X%02X%02X  %d hop%s  snr %d%s",
                    node.pubkey_prefix[0], node.pubkey_prefix[1],
                    node.pubkey_prefix[2], node.pubkey_prefix[3],
                    node.hops, node.hops == 1 ? "" : "s", node.snr,
                    node.has_path ? "  routed" : "");

        scr_row_create(scr1_list, node.name, detail, right,
                       scr1_node_event, (void *)(intptr_t)i);
    }
}

static void scr1_render_header(void)
{
    char name[MESH_NET_NAME_LEN];
    char key[16];

    if(scr1_header == NULL) return;

    mesh_net_get_self_name(name, sizeof(name));
    mesh_net_get_self_key(key, sizeof(key));

    lv_label_set_text_fmt(scr1_header, "%s (%s)   rx %u  tx %u",
                          name, key,
                          (unsigned)mesh_net_packets_rx(), (unsigned)mesh_net_packets_tx());
}

static void scr1_timer_event(lv_timer_t *t)
{
    LV_UNUSED(t);

    static uint32_t shown = 0xFFFFFFFF;

    // Only when something was actually heard: this panel is slow to redraw.
    uint32_t rev = mesh_net_revision();
    if(rev == shown) return;
    shown = rev;

    scr1_populate();
    scr1_render_header();
    ui_disp_full_refr();
}

static void create1(lv_obj_t *parent)
{
    scr1_header = lv_label_create(parent);
    lv_obj_set_style_text_font(scr1_header, FONT_BOLD_SIZE_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(scr1_header, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_label_set_long_mode(scr1_header, LV_LABEL_LONG_DOT);
    lv_obj_set_size(scr1_header, lv_pct(94), 16);
    lv_obj_align(scr1_header, LV_ALIGN_TOP_MID, 0, 34);

    scr1_list = scr_app_list_create(parent);
    lv_obj_set_size(scr1_list, lv_pct(96), LV_VER_RES - 56);
    lv_obj_align(scr1_list, LV_ALIGN_BOTTOM_MID, 0, 0);

    scr1_populate();
    scr1_render_header();

    scr_back_btn_create(parent, "Mesh", scr1_btn_event_cb);
    scr_action_btn_create(parent, LV_SYMBOL_SETTINGS, scr1_self_event);
}

static void entry1(void)
{
    scr1_populate();
    scr1_render_header();

    if(scr1_timer == NULL) {
        scr1_timer = lv_timer_create(scr1_timer_event, 3000, NULL);
    }
    ui_disp_full_refr();
}

static void exit1(void) {
    if(scr1_timer) {
        lv_timer_del(scr1_timer);
        scr1_timer = NULL;
    }
    ui_disp_full_refr();
}

static void destroy1(void)
{
    scr1_list   = NULL;
    scr1_header = NULL;
}

static scr_lifecycle_t screen1 = {
    .create = create1,
    .entry = entry1,
    .exit  = exit1,
    .destroy = destroy1,
};
#endif
// --------------------- screen 1.1 --------------------- this node
#if 1
/* This node's own name, which is what everyone else sees in their list. The
 * public key underneath it is the identity proper and cannot be changed - it is
 * generated once and kept, because losing it means every node that knows this
 * one no longer does. */
static lv_obj_t *scr1_1_name = NULL;
static lv_obj_t *scr1_1_list = NULL;

static void scr1_1_populate(void);

/* A row that only reports something; rows are buttons and need a callback
 * whether or not there is anything to do with a press. */
static void scr1_1_inert_event(lv_event_t *e) { LV_UNUSED(e); }

static void scr1_1_btn_event_cb(lv_event_t * e)
{
    if(e->code == LV_EVENT_CLICKED){
        scr_mgr_pop(false);
    }
}

static void scr1_1_save_event(lv_event_t *e)
{
    LV_UNUSED(e);

    const char *name = lv_textarea_get_text(scr1_1_name);
    if(name && name[0]) mesh_net_set_self_name(name);

    scr_mgr_pop(false);
}

static void scr1_1_advertise_event(lv_event_t *e)
{
    LV_UNUSED(e);

    mesh_net_advertise();

    double lat, lon;
    if(mesh_net_get_loc_policy() != MESH_LOC_OFF && !mesh_net_get_position(&lat, &lon)) {
        // Sharing is on but there is nothing to share, which is worth saying:
        // the advert went out, just without a position in it.
        ui_notice("Announced", "Sent without a position - the GPS has no fix yet.");
    }
}

/* Off / only when the user asks / every advert. Off by default and deliberately
 * three-way: an advert floods the mesh and is relayed well past radio range, so
 * "share because I meant to" is a different thing from "share always". */
static void scr1_1_location_event(lv_event_t *e)
{
    LV_UNUSED(e);

    mesh_net_set_loc_policy((mesh_net_get_loc_policy() + 1) % 3);
    scr1_1_populate();
    ui_disp_full_refr();
}

static void scr1_1_radio_event(lv_event_t *e)
{
    LV_UNUSED(e);
    scr_mgr_push(SCREEN1_2_ID, false);
}

static void scr1_1_companion_event(lv_event_t *e)
{
    LV_UNUSED(e);
    scr_mgr_push(SCREEN1_6_ID, false);
}

/* Everything about this node except its name, which needs a keyboard and so
 * gets the field above. A list rather than a wall of buttons: there are now
 * more of these than an action bar can hold, and half of them carry a value
 * worth showing next to the label. */
static void scr1_1_populate(void)
{
    char key[16];
    char value[56];

    lv_obj_clean(scr1_1_list);

    mesh_net_get_self_key(key, sizeof(key));
    scr_row_create(scr1_1_list, "Key", key, NULL, scr1_1_inert_event, NULL);

    mesh_radio_t r;
    mesh_net_get_radio(&r);
    lv_snprintf(value, sizeof(value), "%s  %.3f", mesh_net_region_name(), r.freq_mhz);
    scr_row_create(scr1_1_list, "Radio", value, NULL, scr1_1_radio_event, NULL);

    /* The position, and whether there is one to share, since a setting that
     * says "on" while the GPS has no fix would be misleading. */
    double lat, lon;
    if(mesh_net_get_loc_policy() == MESH_LOC_OFF) {
        lv_snprintf(value, sizeof(value), "Off");
    } else if(mesh_net_get_position(&lat, &lon)) {
        lv_snprintf(value, sizeof(value), "%s  %.4f, %.4f",
                    mesh_net_loc_policy_name(), lat, lon);
    } else {
        lv_snprintf(value, sizeof(value), "%s  (no fix yet)", mesh_net_loc_policy_name());
    }
    scr_row_create(scr1_1_list, "Share location", value, NULL, scr1_1_location_event, NULL);

    scr_row_create(scr1_1_list, "Companion app", mesh_companion_link_name(), NULL,
                   scr1_1_companion_event, NULL);

    scr_row_create(scr1_1_list, "Announce now", "Tell the mesh this node is here", NULL,
                   scr1_1_advertise_event, NULL);
}

static void create1_1(lv_obj_t *parent)
{
    char name[MESH_NET_NAME_LEN];

    mesh_net_get_self_name(name, sizeof(name));
    scr1_1_name = scr_field_create(parent, "Name on the mesh", 38, name, MESH_NET_NAME_LEN - 1);

    scr1_1_list = scr_app_list_create(parent);
    lv_obj_set_size(scr1_1_list, lv_pct(96), LV_VER_RES - 100 - 46);
    lv_obj_align(scr1_1_list, LV_ALIGN_TOP_MID, 0, 100);

    scr1_1_populate();

    lv_obj_t *bar = scr_action_bar_create(parent, 38);
    scr_bar_btn_create(bar, LV_SYMBOL_OK "  Save", 106, scr1_1_save_event, NULL);
    scr_bar_btn_create(bar, LV_SYMBOL_CLOSE "  Cancel", 106, scr1_1_btn_event_cb, NULL);

    scr_back_btn_create(parent, "This node", scr1_1_btn_event_cb);
}

static void entry1_1(void)
{
    // Coming back from the radio or companion screens, the rows are stale.
    scr1_1_populate();

    lv_group_focus_obj(scr1_1_name);
    ui_disp_full_refr();
}

static void exit1_1(void) {
    ui_disp_full_refr();
}

static void destroy1_1(void)
{
    scr1_1_name = NULL;
    scr1_1_list = NULL;
}

static scr_lifecycle_t screen1_1 = {
    .create = create1_1,
    .entry = entry1_1,
    .exit  = exit1_1,
    .destroy = destroy1_1,
};
#endif
// --------------------- screen 1.2 --------------------- mesh radio settings
#if 1
/* The four numbers every node on a mesh has to agree on. Presets cover the
 * published regional settings; editing any one of them switches to the custom
 * preset, seeded from whatever was showing, since someone who changes a value
 * by hand is no longer on a published set. */
enum {
    SCR1_2_FREQ = 0,
    SCR1_2_BW,
    SCR1_2_SF,
    SCR1_2_CR,
    SCR1_2_FIELD_MAX,
};

static int       scr1_2_editing = SCR1_2_FREQ;
static lv_obj_t *scr1_2_list    = NULL;

static void scr1_2_populate(void);

static const struct {
    const char *name;
    const char *unit;
} scr1_2_fields[SCR1_2_FIELD_MAX] = {
    { "Frequency",        "MHz" },
    { "Bandwidth",        "kHz" },
    { "Spreading factor", ""    },
    { "Coding rate",      ""    },
};

static void scr1_2_field_value(int field, char *buf, int len)
{
    mesh_radio_t r;
    mesh_net_get_radio(&r);

    switch(field) {
        case SCR1_2_FREQ: lv_snprintf(buf, len, "%.3f", r.freq_mhz); break;
        case SCR1_2_BW:   lv_snprintf(buf, len, "%.1f", r.bandwidth_khz); break;
        case SCR1_2_SF:   lv_snprintf(buf, len, "%d", r.spreading_factor); break;
        case SCR1_2_CR:   lv_snprintf(buf, len, "%d", r.coding_rate); break;
        default:          buf[0] = '\0'; break;
    }
}

static void scr1_2_back_event(lv_event_t *e)
{
    if(e->code == LV_EVENT_CLICKED) scr_mgr_pop(false);
}

static void scr1_2_field_event(lv_event_t *e)
{
    scr1_2_editing = (int)(intptr_t)lv_event_get_user_data(e);
    scr_mgr_push(SCREEN1_3_ID, false);
}

static void scr1_2_preset_event(lv_event_t *e)
{
    LV_UNUSED(e);
    mesh_net_region_next();
    scr1_2_populate();
    ui_disp_full_refr();
}

static void scr1_2_populate(void)
{
    char value[32];

    lv_obj_clean(scr1_2_list);

    scr_row_create(scr1_2_list, "Preset", mesh_net_region_name(), NULL,
                   scr1_2_preset_event, NULL);

    for(int i = 0; i < SCR1_2_FIELD_MAX; i++) {
        char shown[40];
        scr1_2_field_value(i, value, sizeof(value));
        lv_snprintf(shown, sizeof(shown), "%s %s", value, scr1_2_fields[i].unit);

        scr_row_create(scr1_2_list, scr1_2_fields[i].name, shown, NULL,
                       scr1_2_field_event, (void *)(intptr_t)i);
    }
}

static void create1_2(lv_obj_t *parent)
{
    lv_obj_t *note = lv_label_create(parent);
    lv_obj_set_width(note, lv_pct(94));
    lv_obj_set_style_text_font(note, FONT_BOLD_SIZE_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(note, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_label_set_text(note, "All four must match the other nodes exactly.");
    lv_obj_align(note, LV_ALIGN_TOP_MID, 0, 34);

    scr1_2_list = scr_app_list_create(parent);
    lv_obj_set_size(scr1_2_list, lv_pct(96), LV_VER_RES - 60);
    lv_obj_align(scr1_2_list, LV_ALIGN_BOTTOM_MID, 0, 0);

    scr1_2_populate();

    scr_back_btn_create(parent, "Mesh radio", scr1_2_back_event);
}

static void entry1_2(void)
{
    scr1_2_populate();
    ui_disp_full_refr();
}

static void exit1_2(void) { ui_disp_full_refr(); }

static void destroy1_2(void) { scr1_2_list = NULL; }

static scr_lifecycle_t screen1_2 = {
    .create = create1_2,
    .entry = entry1_2,
    .exit  = exit1_2,
    .destroy = destroy1_2,
};
#endif
// --------------------- screen 1.3 --------------------- one radio value
#if 1
static lv_obj_t *scr1_3_field = NULL;

static void scr1_3_back_event(lv_event_t *e)
{
    if(e->code == LV_EVENT_CLICKED) scr_mgr_pop(false);
}

static void scr1_3_save_event(lv_event_t *e)
{
    LV_UNUSED(e);

    mesh_radio_t r;
    mesh_net_get_radio(&r);

    const char *text = lv_textarea_get_text(scr1_3_field);
    if(text == NULL || text[0] == '\0') return;

    float value = atof(text);

    switch(scr1_2_editing) {
        case SCR1_2_FREQ: r.freq_mhz         = value; break;
        case SCR1_2_BW:   r.bandwidth_khz    = value; break;
        case SCR1_2_SF:   r.spreading_factor = (uint8_t)value; break;
        case SCR1_2_CR:   r.coding_rate      = (uint8_t)value; break;
        default: return;
    }

    // Anything typed by hand is by definition no longer a published preset.
    mesh_net_set_custom(r.freq_mhz, r.bandwidth_khz, r.spreading_factor, r.coding_rate);
    scr_mgr_pop(false);
}

static const char *scr1_3_keypad_map[] = { "1", "2", "3", "\n",
                                           "4", "5", "6", "\n",
                                           "7", "8", "9", "\n",
                                           ".", "0", LV_SYMBOL_BACKSPACE, ""
                                         };

static void scr1_3_keypad_event(lv_event_t *e)
{
    lv_obj_t   *btnm = (lv_obj_t *)lv_event_get_target(e);
    const char *txt  = lv_btnmatrix_get_btn_text(btnm, lv_btnmatrix_get_selected_btn(btnm));

    if(txt == NULL) return;

    if(strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) lv_textarea_del_char(scr1_3_field);
    else                                      lv_textarea_add_text(scr1_3_field, txt);
}

static void create1_3(lv_obj_t *parent)
{
    char value[32];
    scr1_2_field_value(scr1_2_editing, value, sizeof(value));

    scr1_3_field = scr_field_create(parent, scr1_2_fields[scr1_2_editing].name, 38,
                                    value, 12);

    lv_obj_t *hint = lv_label_create(parent);
    lv_obj_set_width(hint, lv_pct(92));
    lv_obj_set_style_text_font(hint, FONT_BOLD_SIZE_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 92);

    switch(scr1_2_editing) {
        case SCR1_2_FREQ:
            lv_label_set_text(hint, "In MHz, for example 916.575. Transmitting where you are "
                                    "not licensed to is on you, not the radio."); break;
        case SCR1_2_BW:
            lv_label_set_text(hint, "In kHz. Narrow meshes usually run 62.5, wider ones 250."); break;
        case SCR1_2_SF:
            lv_label_set_text(hint, "5 to 12. Lower is faster and shorter ranged."); break;
        default:
            lv_label_set_text(hint, "5 to 8."); break;
    }

    lv_obj_t *pad = lv_btnmatrix_create(parent);
    lv_btnmatrix_set_map(pad, scr1_3_keypad_map);
    lv_obj_set_size(pad, lv_pct(96), 112);
    lv_obj_set_style_border_width(pad, 0, LV_PART_MAIN);
    lv_obj_align(pad, LV_ALIGN_BOTTOM_MID, 0, -48);
    lv_obj_add_event_cb(pad, scr1_3_keypad_event, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *bar = scr_action_bar_create(parent, 38);
    scr_bar_btn_create(bar, LV_SYMBOL_OK "  Save", 106, scr1_3_save_event, NULL);
    scr_bar_btn_create(bar, LV_SYMBOL_CLOSE "  Cancel", 106, scr1_3_back_event, NULL);

    scr_back_btn_create(parent, scr1_2_fields[scr1_2_editing].name, scr1_3_back_event);
}

static void entry1_3(void)
{
    lv_group_focus_obj(scr1_3_field);
    ui_disp_full_refr();
}

static void exit1_3(void) { ui_disp_full_refr(); }

static void destroy1_3(void) { scr1_3_field = NULL; }

static scr_lifecycle_t screen1_3 = {
    .create = create1_3,
    .entry = entry1_3,
    .exit  = exit1_3,
    .destroy = destroy1_3,
};
#endif
// --------------------- screen 1.4 --------------------- mesh conversation
#if 1
/* What has been said with one node, or on the public channel.
 *
 * The same shape as the SMS conversation, deliberately - it is the same idea -
 * but the differences are worth showing rather than hiding. A mesh message is
 * stamped with how long ago it was rather than a clock time, because nodes do
 * not agree on the time. And a private message is acknowledged end to end, so
 * "delivered" here means the far node actually answered, which is more than a
 * text message ever tells you; a channel message is a broadcast that nobody
 * acknowledges, so the most it can say is that it went out. */
static lv_obj_t   *scr1_4_cont  = NULL;
static lv_timer_t *scr1_4_timer = NULL;
static uint32_t    scr1_4_built = 0;

static void scr1_4_populate(void);

static void scr1_4_back_event(lv_event_t *e)
{
    if(e->code == LV_EVENT_CLICKED) scr_mgr_pop(false);
}

static void scr1_4_reply_event(lv_event_t *e)
{
    LV_UNUSED(e);
    scr_mgr_push(SCREEN1_5_ID, false);
}

static lv_coord_t scr1_4_bubble_create(lv_obj_t *parent, const mesh_msg_t *m, lv_coord_t y)
{
    char head[48];
    char ago[16];

    scr1_ago(ago, sizeof(ago), m->at_ms);

    if(m->outgoing) {
        const char *mark = (m->status == MESH_ST_SENDING)   ? "  sending..."
                         : (m->status == MESH_ST_FAILED)    ? "  not delivered"
                         : (m->status == MESH_ST_DELIVERED) ? "  delivered"
                                                            : "  sent";
        lv_snprintf(head, sizeof(head), "%s ago%s", ago, mark);
    } else {
        lv_snprintf(head, sizeof(head), "%s ago", ago);
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
    lv_obj_set_pos(bubble, m->outgoing ? 42 : 0, y);

    lv_obj_t *text = lv_label_create(bubble);
    lv_obj_set_width(text, lv_pct(100));
    lv_obj_set_style_text_font(text, FONT_BOLD_SIZE_15, LV_PART_MAIN);
    lv_obj_set_style_text_color(text, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
    lv_label_set_text_fmt(text, "%s\n%s", head, m->text);
    if(m->outgoing) lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

    // The height only exists once the wrapped text has been laid out.
    lv_obj_update_layout(bubble);
    return y + lv_obj_get_height(bubble) + 6;
}

static void scr1_4_populate(void)
{
    const uint8_t *peer = ui_mesh_target();

    lv_obj_clean(scr1_4_cont);

    int total = mesh_net_msg_count(peer);

    if(total == 0) {
        scr_empty_note_create(scr1_4_cont,
            ui_mesh_channel
                ? "Nothing on the channel yet.\n\nAnything sent here goes to every node in range."
                : "Nothing said yet.");
        scr1_4_built = mesh_net_revision();
        return;
    }

    /* Only the tail. Older messages are still in the log, but laying out and
     * pushing a long conversation to this display is slow enough to feel it. */
    int first = total > 12 ? total - 12 : 0;
    lv_coord_t y = 0;

    for(int i = first; i < total; i++) {
        mesh_msg_t m;
        if(!mesh_net_get_msg(peer, i, &m)) continue;

        y = scr1_4_bubble_create(scr1_4_cont, &m, y);
    }

    lv_obj_update_layout(scr1_4_cont);
    lv_obj_scroll_to_y(scr1_4_cont, lv_obj_get_scroll_bottom(scr1_4_cont), LV_ANIM_OFF);

    scr1_4_built = mesh_net_revision();
}

/* A message can arrive, or one being sent can settle, while this is on screen.
 * Both show up as a change of revision. */
static void scr1_4_timer_event(lv_timer_t *t)
{
    LV_UNUSED(t);

    if(mesh_net_revision() == scr1_4_built) return;

    mesh_net_mark_read(ui_mesh_target());
    scr1_4_populate();
    ui_disp_full_refr();
}

static void create1_4(lv_obj_t *parent)
{
    scr1_4_cont = lv_obj_create(parent);
    lv_obj_set_size(scr1_4_cont, lv_pct(96), LV_VER_RES - 36 - 44);
    lv_obj_align(scr1_4_cont, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_set_style_bg_color(scr1_4_cont, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr1_4_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr1_4_cont, 2, LV_PART_MAIN);
    lv_obj_set_scroll_dir(scr1_4_cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scr1_4_cont, LV_SCROLLBAR_MODE_OFF);
    scr_scroll_for_epaper(scr1_4_cont);

    scr1_4_populate();

    lv_obj_t *bar = scr_action_bar_create(parent, 38);
    scr_bar_btn_create(bar, LV_SYMBOL_EDIT "  Reply", 106, scr1_4_reply_event, NULL);

    scr_back_btn_create(parent, ui_mesh_title, scr1_4_back_event);
}

static void entry1_4(void)
{
    mesh_net_mark_read(ui_mesh_target());

    if(scr1_4_built != mesh_net_revision()) scr1_4_populate();

    if(scr1_4_timer == NULL) {
        scr1_4_timer = lv_timer_create(scr1_4_timer_event, 3000, NULL);
    }
    ui_disp_full_refr();
}

static void exit1_4(void)
{
    if(scr1_4_timer) {
        lv_timer_del(scr1_4_timer);
        scr1_4_timer = NULL;
    }
    ui_disp_full_refr();
}

static void destroy1_4(void) { scr1_4_cont = NULL; }

static scr_lifecycle_t screen1_4 = {
    .create = create1_4,
    .entry = entry1_4,
    .exit  = exit1_4,
    .destroy = destroy1_4,
};
#endif
// --------------------- screen 1.5 --------------------- mesh compose
#if 1
static lv_obj_t *scr1_5_body   = NULL;
static lv_obj_t *scr1_5_status = NULL;

static void scr1_5_back_event(lv_event_t *e)
{
    if(e->code == LV_EVENT_CLICKED) scr_mgr_pop(false);
}

static void scr1_5_send_event(lv_event_t *e)
{
    LV_UNUSED(e);

    const char *body = lv_textarea_get_text(scr1_5_body);

    if(body == NULL || body[0] == '\0') {
        lv_label_set_text(scr1_5_status, "Nothing to send");
        return;
    }
    if(mesh_net_send_busy()) {
        lv_label_set_text(scr1_5_status, "Still sending the last one");
        return;
    }
    if(!mesh_net_send_text(ui_mesh_target(), body)) {
        lv_label_set_text(scr1_5_status, "The radio would not take it");
        return;
    }

    /* Straight back to the conversation rather than waiting on the mesh: the
     * message is already in the log as sending, and the screen behind shows it
     * that way until it settles. */
    scr_mgr_pop(false);
}

static void create1_5(lv_obj_t *parent)
{
    lv_obj_t *to = lv_label_create(parent);
    lv_obj_set_width(to, lv_pct(92));
    lv_obj_set_style_text_font(to, FONT_BOLD_SIZE_15, LV_PART_MAIN);
    lv_obj_set_style_text_color(to, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_label_set_long_mode(to, LV_LABEL_LONG_DOT);
    lv_obj_set_height(to, 18);
    lv_obj_align(to, LV_ALIGN_TOP_LEFT, 10, 38);
    lv_label_set_text_fmt(to, "To: %s", ui_mesh_title);

    scr1_5_body = lv_textarea_create(parent);
    lv_textarea_set_max_length(scr1_5_body, MESH_TEXT_LEN - 1);
    lv_textarea_set_placeholder_text(scr1_5_body, "Message");
    lv_obj_set_size(scr1_5_body, lv_pct(92), 148);
    lv_obj_set_style_text_font(scr1_5_body, FONT_BOLD_SIZE_15, LV_PART_MAIN);
    lv_obj_align(scr1_5_body, LV_ALIGN_TOP_MID, 0, 60);

    scr1_5_status = lv_label_create(parent);
    lv_obj_set_width(scr1_5_status, lv_pct(92));
    lv_obj_set_style_text_font(scr1_5_status, FONT_BOLD_SIZE_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(scr1_5_status, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_label_set_long_mode(scr1_5_status, LV_LABEL_LONG_WRAP);
    lv_obj_align(scr1_5_status, LV_ALIGN_TOP_LEFT, 10, 212);
    lv_label_set_text(scr1_5_status,
                      ui_mesh_channel ? "Everyone in range can read this."
                                      : "Encrypted to this node alone.");

    lv_obj_t *bar = scr_action_bar_create(parent, 38);
    scr_bar_btn_create(bar, LV_SYMBOL_UP "  Send", 106, scr1_5_send_event, NULL);
    scr_bar_btn_create(bar, LV_SYMBOL_CLOSE "  Cancel", 106, scr1_5_back_event, NULL);

    scr_back_btn_create(parent, "Mesh message", scr1_5_back_event);
}

static void entry1_5(void)
{
    lv_group_focus_obj(scr1_5_body);
    ui_disp_full_refr();
}

static void exit1_5(void) { ui_disp_full_refr(); }

static void destroy1_5(void)
{
    scr1_5_body   = NULL;
    scr1_5_status = NULL;
}

static scr_lifecycle_t screen1_5 = {
    .create = create1_5,
    .entry = entry1_5,
    .exit  = exit1_5,
    .destroy = destroy1_5,
};
#endif

// --------------------- screen 1.6 --------------------- companion app
#if 1
/* Letting a MeshCore companion app drive this node.
 *
 * The app talks the MeshCore companion protocol over either Bluetooth or WiFi,
 * and gets the same node the screen does - the same identity, the same
 * contacts, the same conversations. One link at a time: each radio wants a
 * sizeable bite of memory, and the WiFi link puts this device up as an access
 * point, which it cannot do while the hotspot has the radio.
 */
static lv_obj_t   *scr1_6_list  = NULL;
static lv_timer_t *scr1_6_timer = NULL;

// Which value screen 1.7 is editing.
enum {
    SCR1_6_EDIT_SSID = 0,
    SCR1_6_EDIT_PASS,
};
static int scr1_6_editing = SCR1_6_EDIT_SSID;

static void scr1_6_populate(void);

static void scr1_6_back_event(lv_event_t *e)
{
    if(e->code == LV_EVENT_CLICKED) scr_mgr_pop(false);
}

/* A row that only reports something. Rows are buttons, so they need a callback
 * whether or not there is anything to do with a press. */
static void scr1_6_inert_event(lv_event_t *e) { LV_UNUSED(e); }

static void scr1_6_link_event(lv_event_t *e)
{
    LV_UNUSED(e);

    int next = (mesh_companion_get_link() + 1) % 3;

    if(next == MESH_LINK_WIFI && udp_relay_is_on()) {
        ui_notice("WiFi link",
                  "The hotspot is using the WiFi radio.\n\nTurn it off first.");
        next = MESH_LINK_OFF;
    }

    mesh_companion_set_link(next);
    scr1_6_populate();
    ui_disp_full_refr();
}

static void scr1_6_edit_event(lv_event_t *e)
{
    scr1_6_editing = (int)(intptr_t)lv_event_get_user_data(e);
    scr_mgr_push(SCREEN1_7_ID, false);
}

static void scr1_6_populate(void)
{
    char value[64];
    char detail[64];
    int  link = mesh_companion_get_link();

    lv_obj_clean(scr1_6_list);

    scr_row_create(scr1_6_list, "Link", mesh_companion_link_name(), NULL,
                   scr1_6_link_event, NULL);

    if(link == MESH_LINK_OFF) {
        scr_empty_note_create(scr1_6_list,
            "Off.\n\nTurn the link on to let a MeshCore app on your phone use "
            "this node's radio and identity.");
        return;
    }

    /* What is actually happening, which is the first thing anybody looks at
     * when an app will not connect. */
    mesh_companion_get_detail(detail, sizeof(detail));
    if(detail[0]) {
        lv_snprintf(value, sizeof(value), "%s", detail);
    } else if(mesh_companion_is_connected()) {
        lv_snprintf(value, sizeof(value), "App connected");
    } else {
        lv_snprintf(value, sizeof(value), "Waiting for an app");
    }
    scr_row_create(scr1_6_list, "Status", value, NULL, scr1_6_inert_event, NULL);

    if(link == MESH_LINK_BLE) {
        mesh_companion_ble_name(value, sizeof(value));
        scr_row_create(scr1_6_list, "Bluetooth name", value, NULL,
                       scr1_6_inert_event, NULL);

        lv_snprintf(value, sizeof(value), "%06u", (unsigned)mesh_companion_ble_pin());
        scr_row_create(scr1_6_list, "Pairing code", value, NULL,
                       scr1_6_inert_event, NULL);
    } else {
        char ssid[MESH_LINK_SSID_LEN];
        char pass[MESH_LINK_PASS_LEN];
        mesh_companion_get_wifi(ssid, sizeof(ssid), pass, sizeof(pass));

        scr_row_create(scr1_6_list, "Network", ssid, NULL,
                       scr1_6_edit_event, (void *)(intptr_t)SCR1_6_EDIT_SSID);

        // Under eight characters is not a password WPA2 will take, so the
        // network runs open instead - which is worth saying plainly.
        scr_row_create(scr1_6_list, "Password",
                       strlen(pass) < 8 ? "(open network)" : pass, NULL,
                       scr1_6_edit_event, (void *)(intptr_t)SCR1_6_EDIT_PASS);

        mesh_companion_get_address(value, sizeof(value));
        scr_row_create(scr1_6_list, "Connect to", value[0] ? value : "not up yet", NULL,
                       scr1_6_inert_event, NULL);
    }

    lv_snprintf(value, sizeof(value), "%u in  %u out",
                (unsigned)mesh_companion_frames_rx(), (unsigned)mesh_companion_frames_tx());
    scr_row_create(scr1_6_list, "Frames", value, NULL, scr1_6_inert_event, NULL);

    int queued = mesh_companion_queued();
    if(queued > 0) {
        lv_snprintf(value, sizeof(value), "%d message%s", queued, queued == 1 ? "" : "s");
        scr_row_create(scr1_6_list, "Held for the app", value, NULL,
                       scr1_6_inert_event, NULL);
    }
}

/* The link comes up on the mesh task, so nothing here knows when it happened
 * except by looking. Only redraws when something changed - this panel is slow. */
static void scr1_6_timer_event(lv_timer_t *t)
{
    LV_UNUSED(t);

    static int      shown_state  = -1;
    static uint32_t shown_frames = 0xFFFFFFFF;

    int      state  = mesh_companion_is_connected() ? 1 : 0;
    uint32_t frames = mesh_companion_frames_rx() + mesh_companion_frames_tx();

    if(state == shown_state && frames == shown_frames) return;
    shown_state  = state;
    shown_frames = frames;

    scr1_6_populate();
    ui_disp_full_refr();
}

static void create1_6(lv_obj_t *parent)
{
    scr1_6_list = scr_app_list_create(parent);
    lv_obj_set_size(scr1_6_list, lv_pct(96), LV_VER_RES - 40);
    lv_obj_align(scr1_6_list, LV_ALIGN_BOTTOM_MID, 0, 0);

    scr1_6_populate();

    scr_back_btn_create(parent, "Companion app", scr1_6_back_event);
}

static void entry1_6(void)
{
    scr1_6_populate();

    if(scr1_6_timer == NULL) {
        scr1_6_timer = lv_timer_create(scr1_6_timer_event, 2000, NULL);
    }
    ui_disp_full_refr();
}

static void exit1_6(void)
{
    if(scr1_6_timer) {
        lv_timer_del(scr1_6_timer);
        scr1_6_timer = NULL;
    }
    ui_disp_full_refr();
}

static void destroy1_6(void) { scr1_6_list = NULL; }

static scr_lifecycle_t screen1_6 = {
    .create = create1_6,
    .entry = entry1_6,
    .exit  = exit1_6,
    .destroy = destroy1_6,
};
#endif
// --------------------- screen 1.7 --------------------- one companion setting
#if 1
static lv_obj_t *scr1_7_field = NULL;

static void scr1_7_back_event(lv_event_t *e)
{
    if(e->code == LV_EVENT_CLICKED) scr_mgr_pop(false);
}

static void scr1_7_save_event(lv_event_t *e)
{
    LV_UNUSED(e);

    const char *text = lv_textarea_get_text(scr1_7_field);
    if(text == NULL) return;

    if(scr1_6_editing == SCR1_6_EDIT_SSID) mesh_companion_set_wifi(text, NULL);
    else                                   mesh_companion_set_wifi(NULL, text);

    scr_mgr_pop(false);
}

static void create1_7(lv_obj_t *parent)
{
    char ssid[MESH_LINK_SSID_LEN];
    char pass[MESH_LINK_PASS_LEN];

    mesh_companion_get_wifi(ssid, sizeof(ssid), pass, sizeof(pass));

    bool is_ssid = (scr1_6_editing == SCR1_6_EDIT_SSID);
    const char *title = is_ssid ? "Network name" : "Password";

    scr1_7_field = scr_field_create(parent, title, 38,
                                    is_ssid ? ssid : pass,
                                    is_ssid ? MESH_LINK_SSID_LEN - 1 : MESH_LINK_PASS_LEN - 1);

    lv_obj_t *hint = lv_label_create(parent);
    lv_obj_set_width(hint, lv_pct(92));
    lv_obj_set_style_text_font(hint, FONT_BOLD_SIZE_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 92);
    lv_label_set_text(hint, is_ssid
        ? "The network your phone joins before the app can reach this node."
        : "Eight characters or more, or leave it short to run the network open.");

    lv_obj_t *bar = scr_action_bar_create(parent, 38);
    scr_bar_btn_create(bar, LV_SYMBOL_OK "  Save", 106, scr1_7_save_event, NULL);
    scr_bar_btn_create(bar, LV_SYMBOL_CLOSE "  Cancel", 106, scr1_7_back_event, NULL);

    scr_back_btn_create(parent, title, scr1_7_back_event);
}

static void entry1_7(void)
{
    lv_group_focus_obj(scr1_7_field);
    ui_disp_full_refr();
}

static void exit1_7(void) { ui_disp_full_refr(); }

static void destroy1_7(void) { scr1_7_field = NULL; }

static scr_lifecycle_t screen1_7 = {
    .create = create1_7,
    .entry = entry1_7,
    .exit  = exit1_7,
    .destroy = destroy1_7,
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

    scr_back_btn_create(parent, ("Time"), scr2_1_btn_event_cb);
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
    scr_scroll_for_epaper(scr2_1_1_list);

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
    lv_obj_set_style_text_font(info, FONT_BOLD_SIZE_14, LV_PART_MAIN);
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
    
    scr_back_btn_create(parent, ("About System"), scr2_2_btn_event_cb);
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
/* The four power switches share one glyph deliberately: they are the same kind
 * of control, and picking a different near-miss icon for each would read as
 * four unrelated settings. Rows with no apt glyph are better left bare than
 * given a misleading one. */
static ui_setting_handle setting_handle_list[] = {
    {.name = "Time",             .icon = LV_SYMBOL_REFRESH,  .type=UI_SETTING_TYPE_SUB, .sub_id = SCREEN2_1_ID},
    {.name = "Auto Lock",        .icon = LV_SYMBOL_POWER,    .type=UI_SETTING_TYPE_CHOICE,
     .text_cb = ui_setting_autolock_text, .next_cb = ui_setting_autolock_next},
    {.name = "Vibrate on Call",  .icon = LV_SYMBOL_CALL,     .type=UI_SETTING_TYPE_SW,  .set_cb = ui_setting_set_vibrate_call, .get_cb = ui_setting_get_vibrate_call},
    {.name = "Vibrate on Text",  .icon = LV_SYMBOL_ENVELOPE, .type=UI_SETTING_TYPE_SW,  .set_cb = ui_setting_set_vibrate_text, .get_cb = ui_setting_get_vibrate_text},
    {.name = "Sound on Text",    .icon = LV_SYMBOL_VOLUME_MAX, .type=UI_SETTING_TYPE_SW, .set_cb = ui_setting_set_sound_text,  .get_cb = ui_setting_get_sound_text},
    {.name = "Ear Detect",       .icon = LV_SYMBOL_CALL,     .type=UI_SETTING_TYPE_CHOICE,
     .text_cb = ui_setting_ear_detect_text, .next_cb = ui_setting_ear_detect_next},
    {.name = "Keypad Backlight", .icon = LV_SYMBOL_KEYBOARD, .type=UI_SETTING_TYPE_SW,  .set_cb = ui_setting_set_keypad_light, .get_cb = ui_setting_get_keypad_light},
    {.name = "Motor Status",     .icon = LV_SYMBOL_BELL,     .type=UI_SETTING_TYPE_SW,  .set_cb = ui_setting_set_motor_status, .get_cb = ui_setting_get_motor_status},
    {.name = "Power GPS",        .icon = LV_SYMBOL_POWER,    .type=UI_SETTING_TYPE_SW,  .set_cb = ui_setting_set_gps_status,   .get_cb = ui_setting_get_gps_status},
    {.name = "Power Lora",       .icon = LV_SYMBOL_POWER,    .type=UI_SETTING_TYPE_SW,  .set_cb = ui_setting_set_lora_status,  .get_cb = ui_setting_get_lora_status},
    {.name = "Power Gyro",       .icon = LV_SYMBOL_POWER,    .type=UI_SETTING_TYPE_SW,  .set_cb = ui_setting_set_gyro_status,  .get_cb = ui_setting_get_gyro_status},
    {.name = "Power A7682",      .icon = LV_SYMBOL_POWER,    .type=UI_SETTING_TYPE_SW,  .set_cb = ui_setting_set_a7682_status, .get_cb = ui_setting_get_a7682_status},
    {.name = "About System",     .icon = LV_SYMBOL_FILE,     .type=UI_SETTING_TYPE_SUB, .sub_id = SCREEN2_2_ID},
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
        case UI_SETTING_TYPE_CHOICE:
            h->next_cb();
            lv_label_set_text(h->st, h->text_cb());
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
            h->obj = lv_list_add_btn(setting_list, h->icon, h->name);
            h->st = lv_label_create(h->obj);
            lv_obj_set_style_text_font(h->st, FONT_BOLD_SIZE_15, LV_PART_MAIN);
            lv_obj_align(h->st, LV_ALIGN_RIGHT_MID, 0, 0);
            lv_label_set_text_fmt(h->st, "%s", (h->get_cb() ? "ON" : "OFF"));
            break;
        case UI_SETTING_TYPE_CHOICE:
            h->obj = lv_list_add_btn(setting_list, h->icon, h->name);
            h->st = lv_label_create(h->obj);
            lv_obj_set_style_text_font(h->st, FONT_BOLD_SIZE_15, LV_PART_MAIN);
            lv_obj_align(h->st, LV_ALIGN_RIGHT_MID, 0, 0);
            lv_label_set_text(h->st, h->text_cb());
            break;
        case UI_SETTING_TYPE_SUB:
            h->obj = lv_list_add_btn(setting_list, h->icon, h->name);
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
    scr_scroll_for_epaper(setting_list);
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

    scr_back_btn_create(parent, ("GPS"), scr3_btn_event_cb);
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
    scr_scroll_for_epaper(scr4_list);
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

    scr_back_btn_create(parent, ("Wifi"), scr4_2_btn_event_cb);
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
    { .name="Mesh radio", .peri_id=E_PERI_LORA       , .cb=ui_test_get },
    { .name="Touch",      .peri_id=E_PERI_TOUCH      , .cb=ui_test_get },
    { .name="BQ25896",    .peri_id=E_PERI_BQ25896    , .cb=ui_test_get },
    { .name="BQ27220",    .peri_id=E_PERI_BQ27220    , .cb=ui_test_get },
    { .name="SD Card",    .peri_id=E_PERI_SD         , .cb=ui_test_get },
    { .name="A7682E",     .peri_id=E_PERI_A7682E     , .cb=ui_test_get },
    { .name="Keypad",     .peri_id=E_PERI_KYEPAD     , .cb=ui_test_get },
    { .name="GPS",        .peri_id=E_PERI_GPS        , .cb=ui_test_get },
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
        lv_label_set_text_fmt(h->st, "%s", (h->cb(h->peri_id) ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE));
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
    scr_scroll_for_epaper(test_list);
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

    scr_back_btn_create(parent, ("Test"), scr5_btn_event_cb);
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
    scr_scroll_for_epaper(scr6_list);
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

/* Queues a message and logs it as pending in one step. Shared by the composer
 * and by sending a reaction, so both end up watched the same way. */
static bool ui_sms_dispatch(const char *number, const char *text)
{
    if(number == NULL || number[0] == '\0') return false;
    if(text == NULL || text[0] == '\0') return false;
    if(ui_send_watch_id != 0) return false; // one in flight is enough

    uint32_t send_id = ui_sms_send(number, text);
    if(send_id == 0) return false;

    // Logged straight away as pending so the conversation reads correctly
    // however long the network takes.
    ui_send_watch_idx = sms_add(number, text, (uint32_t)time(NULL),
                                SMS_DIR_OUT, SMS_ST_PENDING, false);
    ui_send_watch_id  = send_id;
    ui_sms_revision++;
    return true;
}

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

/* Recognising the message another phone sends when someone reacts to one of
 * ours, so it can be shown attached to the message it is about rather than as
 * a message in its own right.
 *
 * SMS has no reaction protocol. The reacting phone simply sends a sentence that
 * quotes the message it refers to - and the curly quotes and emoji it uses are
 * exactly what push the whole thing out of the GSM alphabet into UCS2, which is
 * why these arrive as hex before the modem layer decodes them.
 *
 * So this is pattern matching on what the common phones actually send, and it
 * is best effort: anything not recognised is shown as the ordinary message it
 * appears to be. */
typedef struct {
    char        emoji[8];             // glyph for the badge on the target
    const char *word;                 // wording when there is no target to pin it to
    char        quoted[SMS_TEXT_LEN]; // the excerpt naming the target
    bool        removal;              // a reaction being taken back
} ui_reaction_t;

static const struct {
    const char *prefix;
    const char *word;
    const char *emoji;
} reaction_prefixes[] = {
    { "Laughed at ", "Laughed at", "\xF0\x9F\x98\x82" }, // tears of joy
    { "Emphasized ", "Emphasised", "\xE2\x80\xBC"     }, // double exclamation
    { "Emphasised ", "Emphasised", "\xE2\x80\xBC"     },
    { "Questioned ", "Questioned", "\xE2\x9D\x93"     }, // question mark
    { "Disliked ",   "Disliked",   "\xF0\x9F\x91\x8E" }, // thumbs down
    { "Liked ",      "Liked",      "\xF0\x9F\x91\x8D" }, // thumbs up
    { "Loved ",      "Loved",      "\xE2\x9D\xA4"     }, // heart
    { "Removed ",    "Removed a reaction from", ""    },
};

/* Written as byte escapes rather than literal characters so the mapping does
 * not depend on this file's encoding. */
#define UI_REACT_LIKE      "\xF0\x9F\x91\x8D" // thumbs up
#define UI_REACT_DISLIKE   "\xF0\x9F\x91\x8E" // thumbs down
#define UI_REACT_LOVE      "\xE2\x9D\xA4"     // heart
#define UI_REACT_LAUGH     "\xF0\x9F\x98\x82" // tears of joy
#define UI_REACT_EMPHASISE "\xE2\x80\xBC"     // double exclamation
#define UI_REACT_QUESTION  "\xE2\x9D\x93"     // question mark

static const struct {
    const char *utf8;
    const char *word;
} reaction_emoji[] = {
    { UI_REACT_LIKE,      "Liked"      },
    { UI_REACT_DISLIKE,   "Disliked"   },
    { UI_REACT_LOVE,      "Loved"      },
    { UI_REACT_LAUGH,     "Laughed at" },
    { UI_REACT_EMPHASISE, "Emphasised" },
    { UI_REACT_QUESTION,  "Questioned" },
};

/* Shortens `s` in place to at most `max_chars` characters, cutting on a UTF-8
 * boundary and marking the cut with an ellipsis. A reaction quotes the message
 * it is about and has to fit one segment alongside the wording - and going out
 * as UCS2 leaves only seventy characters to play with. */
static void ui_utf8_truncate(char *s, int max_chars);

static int ui_utf8_seq_len(unsigned char c)
{
    if(c < 0x80) return 1;
    if((c & 0xE0) == 0xC0) return 2;
    if((c & 0xF0) == 0xE0) return 3;
    if((c & 0xF8) == 0xF0) return 4;
    return 1;
}

/* Copies out the text between the first opening quote and the last closing one.
 * Phones use curly quotes, three bytes each in UTF-8; straight ones are taken
 * as well. `open_at` receives where the opening quote started. */
static bool ui_quoted_span(const char *s, char *out, int out_len, const char **open_at)
{
    const char *open = NULL;
    const char *close = NULL;

    for(const char *p = s; *p; p++) {
        if((unsigned char)p[0] == 0xE2 && (unsigned char)p[1] == 0x80 && (unsigned char)p[2] == 0x9C) {
            if(open_at) *open_at = p;
            open = p + 3;
            break;
        }
        if(*p == '"') {
            if(open_at) *open_at = p;
            open = p + 1;
            break;
        }
    }
    if(open == NULL) return false;

    for(const char *p = open; *p; p++) {
        if((unsigned char)p[0] == 0xE2 && (unsigned char)p[1] == 0x80 && (unsigned char)p[2] == 0x9D) close = p;
        else if(*p == '"') close = p;
    }
    if(close == NULL || close <= open) return false;

    int n = 0;
    while(open < close && n < out_len - 1) out[n++] = *open++;
    out[n] = '\0';
    return n > 0;
}

static void ui_utf8_truncate(char *s, int max_chars)
{
    char *p = s;
    int   chars = 0;

    while(*p && chars < max_chars) {
        p += ui_utf8_seq_len((unsigned char)*p);
        chars++;
    }
    if(*p == '\0') return; // nothing was dropped

    memcpy(p, "\xE2\x80\xA6", 3); // U+2026
    p[3] = '\0';
}

static bool ui_reaction_parse(const char *text, ui_reaction_t *out)
{
    if(text == NULL || text[0] == '\0') return false;

    const char *open_at = NULL;

    memset(out, 0, sizeof(*out));
    if(!ui_quoted_span(text, out->quoted, sizeof(out->quoted), &open_at)) return false;

    for(int i = 0; i < (int)GET_BUFF_LEN(reaction_prefixes); i++) {
        if(strncmp(text, reaction_prefixes[i].prefix, strlen(reaction_prefixes[i].prefix)) != 0) continue;

        out->word    = reaction_prefixes[i].word;
        out->removal = (reaction_prefixes[i].emoji[0] == '\0');
        lv_snprintf(out->emoji, sizeof(out->emoji), "%s", reaction_prefixes[i].emoji);
        return true;
    }

    /* The other shape is an emoji followed by ` to "..."`. Both conditions are
     * needed: an ordinary message can easily contain ` to ` and a quote, but not
     * while also starting with an emoji. */
    if((unsigned char)text[0] < 0x80) return false;

    const char *to = strstr(text, " to ");
    if(to == NULL || open_at == NULL || to > open_at) return false;

    // Show whatever glyph they actually sent - the emoji font can draw it even
    // when it is not one of the six the named reactions map to.
    int seq = ui_utf8_seq_len((unsigned char)text[0]);
    if(seq >= (int)sizeof(out->emoji)) seq = (int)sizeof(out->emoji) - 1;
    memcpy(out->emoji, text, seq);
    out->emoji[seq] = '\0';

    out->word = "Reacted to";
    for(int i = 0; i < (int)GET_BUFF_LEN(reaction_emoji); i++) {
        if(strstr(text, reaction_emoji[i].utf8) != NULL) {
            out->word = reaction_emoji[i].word;
            break;
        }
    }
    return true;
}

/* Does `message` look like the message a reaction quoted?
 *
 * Both sides are flattened first, because the quote comes back with the
 * sender's line breaks collapsed, and a long message is quoted only as far as
 * the sending phone chose to - so this is a prefix test with any trailing
 * ellipsis removed rather than an equality test. */
static bool ui_reaction_matches(const char *message, const char *quoted)
{
    char a[SMS_TEXT_LEN];
    char b[SMS_TEXT_LEN];

    ui_message_snippet(a, sizeof(a), message);
    ui_message_snippet(b, sizeof(b), quoted);

    int n = (int)strlen(b);
    if(n >= 3 && (unsigned char)b[n - 3] == 0xE2 &&
       (unsigned char)b[n - 2] == 0x80 && (unsigned char)b[n - 1] == 0xA6) {
        n -= 3; // U+2026 horizontal ellipsis
    }
    while(n > 0 && (b[n - 1] == '.' || b[n - 1] == ' ')) n--;

    // Too short to be sure it names one message rather than any of them.
    if(n < 4) return false;

    return strncmp(a, b, (size_t)n) == 0;
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
    lv_obj_set_style_text_font(scr8_number_ta, FONT_BOLD_SIZE_20, LV_PART_MAIN);
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
    lv_obj_set_style_text_font(who, FONT_BOLD_SIZE_20, LV_PART_MAIN);
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

    /* Taller than a list row: these are the most urgent controls in the phone,
     * and since a call raises this screen over a locked one they are often the
     * first thing pressed after picking the phone up. */
    lv_obj_t *bar = scr_action_bar_create(parent, 58);
    scr8_1_answer = scr_bar_btn_create(bar, LV_SYMBOL_CALL "  Answer", 108, scr8_1_answer_event, NULL);
    lv_obj_t *hangup = scr_bar_btn_create(bar, LV_SYMBOL_CLOSE "  Hang up", 108, scr8_1_hangup_event, NULL);

    lv_obj_set_height(scr8_1_answer, 48);
    lv_obj_set_height(hangup, 48);

    scr_back_btn_create(parent, "Call", scr8_btn_event_cb);
}

static void entry8_1(void)
{
    scr8_1_shown_state = MODEM_CALL_IDLE;
    scr8_1_render();

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
    lv_obj_set_style_text_font(name_label, FONT_BOLD_SIZE_20, LV_PART_MAIN);
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
        ui_reaction_t reaction;
        if(last && ui_reaction_parse(last->text, &reaction)) {
            lv_snprintf(snippet, sizeof(snippet), "%s %s \"%s\"",
                        reaction.emoji, reaction.word, reaction.quoted);
        } else {
            ui_message_snippet(snippet, sizeof(snippet), last ? last->text : "");
        }
        if(last && last->dir == SMS_DIR_OUT) {
            // Without a prefix a thread you last replied to reads as if they
            // said it. The delivery state goes here too: sending returns to
            // this screen, so this is where a failure has to be visible.
            const char *prefix = (last->status == SMS_ST_FAILED)  ? LV_SYMBOL_WARNING " not sent: "
                               : (last->status == SMS_ST_PENDING) ? "Sending: "
                                                                  : "You: ";
            char line[sizeof(snippet)];
            lv_snprintf(line, sizeof(line), "%s%s", prefix, snippet);
            lv_snprintf(snippet, sizeof(snippet), "%s", line);
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
/* How much of a message a reaction we send quotes back. A UCS2 segment holds
 * seventy characters and the wording takes a dozen of them. */
#define SCR13_1_QUOTE_CHARS 50
// The reaction picker's grid: three rows tall enough to aim a finger at.
#define SCR13_1_REACT_ROWS  3
#define SCR13_1_REACT_ROW_H 46

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
/* Sends a reaction the way every other phone does over SMS: there is no
 * protocol for it, so it goes as an ordinary message naming the reaction and
 * quoting what it is about. The curly quotes are part of that convention, and
 * are also what push the message into UCS2 on the way out. */
static void scr13_1_send_reaction(const char *emoji)
{
    const sms_msg_t *target = sms_get(scr13_1_delete_target);
    if(target == NULL) return;

    const char *word = NULL;
    for(int i = 0; i < (int)GET_BUFF_LEN(reaction_emoji); i++) {
        if(strcmp(emoji, reaction_emoji[i].utf8) == 0) {
            word = reaction_emoji[i].word;
            break;
        }
    }
    if(word == NULL) return;

    char quote[SMS_TEXT_LEN];
    ui_message_snippet(quote, sizeof(quote), target->text);
    ui_utf8_truncate(quote, SCR13_1_QUOTE_CHARS);

    char text[SMS_TEXT_LEN];
    lv_snprintf(text, sizeof(text), "%s \xE2\x80\x9C%s\xE2\x80\x9D", word, quote);

    if(ui_sms_dispatch(ui_active_number, text)) {
        // The reaction lands in the log as an outgoing message, and the pass in
        // populate pins it to the message it names, same as a received one.
        scr13_1_populate();
    } else {
        ui_notice("Not sent", "The modem is busy with another message. Try again in a moment.");
    }
}

static const char *scr13_1_react_map[] = {
    UI_REACT_LIKE,  UI_REACT_DISLIKE,   UI_REACT_LOVE,     "\n",
    UI_REACT_LAUGH, UI_REACT_EMPHASISE, UI_REACT_QUESTION, "\n",
    LV_SYMBOL_TRASH " Delete", "Cancel", ""
};

static void scr13_1_react_choice_event(lv_event_t *e)
{
    lv_obj_t   *mbox = lv_event_get_current_target(e);
    const char *txt  = lv_msgbox_get_active_btn_text(mbox);

    char chosen[8] = {0};
    bool remove    = false;

    if(txt) {
        if(strcmp(txt, LV_SYMBOL_TRASH " Delete") == 0) remove = true;
        else if(strcmp(txt, "Cancel") != 0) lv_snprintf(chosen, sizeof(chosen), "%s", txt);
    }

    lv_msgbox_close(mbox);

    if(remove) scr13_1_do_delete_message();
    else if(chosen[0]) scr13_1_send_reaction(chosen);

    ui_disp_full_refr();
}

/* Tapping a message is how it gets reacted to or deleted. */
static void scr13_1_bubble_event(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const sms_msg_t *m = sms_get(idx);
    if(m == NULL) return;

    char preview[48];
    ui_message_snippet(preview, sizeof(preview), m->text);
    ui_utf8_truncate(preview, 32);

    scr13_1_delete_target = idx;

    lv_obj_t *mbox = lv_msgbox_create(NULL, "React", preview, scr13_1_react_map, false);
    lv_obj_set_width(mbox, lv_pct(96));
    lv_obj_set_style_bg_color(mbox, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_text_color(mbox, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_border_color(mbox, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_border_width(mbox, 2, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(mbox, 0, LV_PART_MAIN);
    lv_obj_set_style_text_font(mbox, FONT_BOLD_SIZE_15, LV_PART_MAIN);
    lv_obj_center(mbox);

    // The default modal backdrop is half-transparent black, which on a 1bpp
    // panel dithers into noise.
    lv_obj_set_style_bg_opa(lv_obj_get_parent(mbox), LV_OPA_TRANSP, LV_PART_MAIN);

    /* lv_msgbox sizes its button matrix for a single row - the height it picks
     * is one line plus a margin - so a map with three rows in it ends up a
     * third of a row tall per button. Size it for the rows it actually has, and
     * give it the chain with the big emoji on the end. */
    lv_obj_t *btns = lv_msgbox_get_btns(mbox);
    if(btns) {
        lv_obj_set_size(btns, lv_pct(100), SCR13_1_REACT_ROWS * SCR13_1_REACT_ROW_H);
        lv_obj_set_style_text_font(btns, &ui_font_react, LV_PART_ITEMS);
        lv_obj_set_style_pad_all(btns, 3, LV_PART_MAIN);
        lv_obj_set_style_pad_row(btns, 4, LV_PART_MAIN);
        lv_obj_set_style_pad_column(btns, 4, LV_PART_MAIN);
    }

    lv_obj_add_event_cb(mbox, scr13_1_react_choice_event, LV_EVENT_VALUE_CHANGED, NULL);
    ui_disp_full_refr();
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
                                        lv_coord_t y, const char *badge)
{
    char head[48];
    char stamp[16];
    char body[SMS_TEXT_LEN];

    ui_format_stamp(stamp, sizeof(stamp), m->ts);

    if(m->dir == SMS_DIR_OUT) {
        const char *mark = (m->status == SMS_ST_PENDING) ? "  sending..."
                         : (m->status == SMS_ST_FAILED)  ? "  not sent"
                                                         : "";
        lv_snprintf(head, sizeof(head), "%s%s", stamp, mark);
    } else {
        lv_snprintf(head, sizeof(head), "%s", stamp);
    }

    /* A reaction only lands here when its target is not on screen - normally it
     * is drawn as a badge on the message it refers to instead. Shown as a short
     * annotation, since the text a phone sends quotes that message in full. */
    ui_reaction_t reaction;
    bool is_reaction = ui_reaction_parse(m->text, &reaction);

    if(is_reaction) {
        // Enough of the excerpt to tell which message it was about.
        if(strlen(reaction.quoted) > 28) lv_snprintf(reaction.quoted + 25, 4, "...");
        lv_snprintf(body, sizeof(body), "%s \"%s\"", reaction.word, reaction.quoted);
    } else {
        lv_snprintf(body, sizeof(body), "%s", m->text);
    }

    lv_obj_t *bubble = lv_obj_create(parent);
    lv_obj_set_width(bubble, 180);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(bubble, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_border_color(bubble, DECKPRO_COLOR_FG, LV_PART_MAIN);
    // No outline on a reaction, so it reads as a note against the conversation
    // rather than as another message in it.
    lv_obj_set_style_border_width(bubble, is_reaction ? 0 : 1, LV_PART_MAIN);
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
    lv_label_set_text_fmt(text, "%s\n%s", head, body);
    if(m->dir == SMS_DIR_OUT) {
        lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    }

    // The height only exists once the wrapped text has been laid out.
    lv_obj_update_layout(bubble);
    lv_coord_t bubble_h = lv_obj_get_height(bubble);
    lv_coord_t extra    = 0;

    /* The reactions somebody sent for this message, hanging off its lower inner
     * corner the way a phone shows them. A sibling rather than a child, so it
     * can overhang the bubble instead of being clipped inside it. */
    if(badge && badge[0]) {
        lv_coord_t bubble_x = lv_obj_get_x(bubble);
        lv_coord_t bubble_w = lv_obj_get_width(bubble);

        lv_obj_t *tag = lv_obj_create(parent);
        lv_obj_set_size(tag, 26, 20);
        lv_obj_set_style_bg_color(tag, DECKPRO_COLOR_BG, LV_PART_MAIN);
        lv_obj_set_style_border_color(tag, DECKPRO_COLOR_FG, LV_PART_MAIN);
        lv_obj_set_style_border_width(tag, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(tag, 10, LV_PART_MAIN);
        lv_obj_set_style_pad_all(tag, 0, LV_PART_MAIN);
        lv_obj_clear_flag(tag, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(tag, m->dir == SMS_DIR_OUT ? bubble_x + 2 : bubble_x + bubble_w - 28,
                       y + bubble_h - 10);

        lv_obj_t *glyph = lv_label_create(tag);
        lv_obj_set_style_text_font(glyph, FONT_BOLD_SIZE_15, LV_PART_MAIN);
        lv_obj_set_style_text_color(glyph, DECKPRO_COLOR_FG, LV_PART_MAIN);
        lv_label_set_text(glyph, badge);
        lv_obj_center(glyph);

        extra = 12; // room for the part that hangs below the bubble
    }

    return y + bubble_h + extra + 6;
}

static void scr13_1_populate(void)
{
    lv_obj_clean(scr13_1_cont);

    int total = sms_thread_msg_count(ui_active_number);
    int first = total > SCR13_1_MAX_BUBBLES ? total - SCR13_1_MAX_BUBBLES : 0;

    if(total == 0) {
        scr_empty_note_create(scr13_1_cont, "No messages in this conversation.");
    }

    /* Work out which of these are reactions before drawing anything, so each
     * one can be pinned to the message it names instead of taking a line of its
     * own. A reaction quotes its target, so the target is the nearest earlier
     * message the quote matches.
     *
     * One that finds no target - because the message it refers to has scrolled
     * out of the window, or came from a phone whose wording is not recognised -
     * falls through and is drawn as an ordinary annotation. */
    char badges[SCR13_1_MAX_BUBBLES][8];
    bool attached[SCR13_1_MAX_BUBBLES];

    memset(badges, 0, sizeof(badges));
    memset(attached, 0, sizeof(attached));

    for(int i = first; i < total; i++) {
        const sms_msg_t *m = sms_thread_msg(ui_active_number, i);
        ui_reaction_t    r;

        if(m == NULL || !ui_reaction_parse(m->text, &r)) continue;

        /* One of ours that the network turned down is left as a message, where
         * the bubble shows it as not sent. Pinned as a badge it would look
         * exactly like one that got through. */
        if(m->dir == SMS_DIR_OUT && m->status == SMS_ST_FAILED) continue;

        for(int j = i - 1; j >= first; j--) {
            const sms_msg_t *target = sms_thread_msg(ui_active_number, j);

            if(target == NULL || attached[j - first]) continue;
            if(!ui_reaction_matches(target->text, r.quoted)) continue;

            // Taking a reaction back clears the badge rather than adding one.
            if(r.removal) badges[j - first][0] = '\0';
            else lv_snprintf(badges[j - first], sizeof(badges[0]), "%s", r.emoji);

            attached[i - first] = true;
            break;
        }
    }

    lv_coord_t y = 0;
    for(int i = first; i < total; i++) {
        int              log_idx = sms_thread_msg_index(ui_active_number, i);
        const sms_msg_t *m       = sms_get(log_idx);

        if(m == NULL) continue;
        if(attached[i - first]) continue; // drawn on its target instead

        y = scr13_1_bubble_create(scr13_1_cont, m, log_idx, y, badges[i - first]);
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
    scr_scroll_for_epaper(scr13_1_cont);

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

    if(!ui_sms_dispatch(ui_active_number, body)) {
        lv_label_set_text(scr13_2_status, "Modem is busy, try again");
        return;
    }

    // Clear before leaving, or exit13_2 keeps the sent text as a draft.
    lv_textarea_set_text(scr13_2_body_ta, "");

    /* Straight back to whatever opened the composer, rather than waiting on the
     * network: the message is already in the log as pending, and the screen
     * behind shows it that way. The outcome is watched centrally, so it lands
     * wherever the user happens to be by then. */
    scr_mgr_pop(false);
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
    lv_textarea_set_max_length(scr13_2_body_ta, SMS_COMPOSE_MAX);
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

    
    // The panel is normally parked a few seconds after the last update, which
    // never arrives once the CPU is asleep, so park it here.
    ui_disp_hibernate();

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
//************************************[ screen 14 ]***************************************** quick settings
#if 1
/* Pulled down from the top of the menu. The settings here are the ones worth
 * changing without walking into the settings screen - what the phone does when
 * it wants attention, and the way out to a locked screen. */
static lv_obj_t *scr14_list = NULL;

static const struct {
    const char *name;
    const char *icon;
    void (*set)(bool);
    bool (*get)(void);
} scr14_toggles[] = {
    { "Keypad Backlight", LV_SYMBOL_KEYBOARD,   ui_setting_set_keypad_light, ui_setting_get_keypad_light },
    { "Vibrate on Call",  LV_SYMBOL_CALL,       ui_setting_set_vibrate_call, ui_setting_get_vibrate_call },
    { "Vibrate on Text",  LV_SYMBOL_ENVELOPE,   ui_setting_set_vibrate_text, ui_setting_get_vibrate_text },
    { "Sound on Text",    LV_SYMBOL_VOLUME_MAX, ui_setting_set_sound_text,   ui_setting_get_sound_text   },
};

static lv_obj_t *scr14_state_of(lv_obj_t *row)
{
    // The value label is the row's last child, added right after the row.
    return lv_obj_get_child(row, lv_obj_get_child_cnt(row) - 1);
}

/* One row of the shade: an icon, a name, and its current value on the right. */
static lv_obj_t *scr14_row_create(const char *icon, const char *name, const char *value,
                                  lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *row = lv_list_add_btn(scr14_list, icon, name);

    lv_obj_set_height(row, 34);
    lv_obj_set_style_text_font(row, FONT_BOLD_SIZE_15, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_text_color(row, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_outline_width(row, 1, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(row, 8, LV_PART_MAIN);

    lv_obj_t *state = lv_label_create(row);
    lv_obj_set_style_text_font(state, FONT_BOLD_SIZE_15, LV_PART_MAIN);
    lv_label_set_text(state, value);

    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user_data);
    return row;
}

/* Auto lock is here as well as in the settings screen. This is where the
 * manual lock button is, so it is where someone looks to find out how long the
 * phone waits before doing it by itself. */
static void scr14_autolock_event(lv_event_t *e)
{
    ui_setting_autolock_next();

    lv_obj_t *state = scr14_state_of((lv_obj_t *)lv_event_get_target(e));
    if(state) lv_label_set_text(state, ui_setting_autolock_text());

    ui_disp_full_refr();
}

static void scr14_back_event(lv_event_t *e)
{
    if(e->code == LV_EVENT_CLICKED) {
        scr_mgr_pop(false);
    }
}

static void scr14_toggle_event(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if(idx < 0 || idx >= (int)GET_BUFF_LEN(scr14_toggles)) return;

    scr14_toggles[idx].set(!scr14_toggles[idx].get());

    lv_obj_t *state = scr14_state_of((lv_obj_t *)lv_event_get_target(e));
    if(state) lv_label_set_text(state, scr14_toggles[idx].get() ? "ON" : "OFF");

    ui_disp_full_refr();
}

static void scr14_lock_event(lv_event_t *e)
{
    LV_UNUSED(e);

    /* Switch rather than push: it clears the screen stack, so nothing is left
     * behind the lock screen holding widgets that a stray keypress could still
     * reach through the input group. */
    scr_mgr_switch(SCREEN15_ID, false);
}

static void create14(lv_obj_t *parent)
{
    lv_obj_t *bar = scr_action_bar_create(parent, 44);
    scr_bar_btn_create(bar, LV_SYMBOL_POWER "  Lock screen", 190, scr14_lock_event, NULL);

    scr14_list = lv_list_create(parent);
    scr_scroll_for_epaper(scr14_list);
    lv_obj_set_size(scr14_list, lv_pct(96), LV_VER_RES - 36 - 52);
    lv_obj_align(scr14_list, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_set_style_pad_all(scr14_list, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_row(scr14_list, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(scr14_list, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(scr14_list, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr14_list, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(scr14_list, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(scr14_list, LV_SCROLLBAR_MODE_OFF);

    scr14_row_create(LV_SYMBOL_POWER, "Auto Lock", ui_setting_autolock_text(),
                     scr14_autolock_event, NULL);

    for(int i = 0; i < (int)GET_BUFF_LEN(scr14_toggles); i++) {
        scr14_row_create(scr14_toggles[i].icon, scr14_toggles[i].name,
                         scr14_toggles[i].get() ? "ON" : "OFF",
                         scr14_toggle_event, (void *)(intptr_t)i);
    }

    scr_back_btn_create(parent, "Quick settings", scr14_back_event);
}

static void entry14(void)
{
    ui_disp_full_refr();
}

static void exit14(void)
{
    ui_disp_full_refr();
}

static void destroy14(void)
{
    scr14_list = NULL;
}

static scr_lifecycle_t screen14 = {
    .create = create14,
    .entry = entry14,
    .exit  = exit14,
    .destroy = destroy14,
};
#endif
//************************************[ screen 15 ]***************************************** lock
#if 1
/* A locked screen is a good fit for e-paper: the clock stays on the glass for
 * nothing once it has been drawn, and there is nothing here to press by
 * accident. Swiping up is the way out - deliberate enough not to happen in a
 * pocket, which is the point of locking in the first place. */
static lv_obj_t   *scr15_clock  = NULL;
static lv_obj_t   *scr15_detail = NULL;
static lv_timer_t *scr15_timer  = NULL;

static void scr15_render(void)
{
    if(scr15_clock == NULL) return;

    char when[48] = "--:--";
    char detail[160];

    if(system_clock_is_set()) {
        time_t    now = time(NULL);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        strftime(when, sizeof(when), "%H:%M", &tm_now);
    }
    lv_label_set_text(scr15_clock, when);

    char date[40] = "";
    if(system_clock_is_set()) {
        time_t    now = time(NULL);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        strftime(date, sizeof(date), "%a %d %b", &tm_now);
    }

    /* What is waiting, from either radio. A count on its own says something is
     * there but not whether it is worth unlocking for, so whoever wrote last is
     * named where there is a name to give. */
    char pending[96] = "";
    int  pos = 0;

    int unread = sms_unread_total();
    if(unread > 0) {
        const char *from = NULL;
        for(int i = sms_count() - 1; i >= 0 && from == NULL; i--) {
            const sms_msg_t *m = sms_get(i);
            if(m && m->unread) from = contacts_display_name(m->number);
        }

        pos = lv_snprintf(pending, sizeof(pending), "\n\n%s %d unread\n%s%s",
                          LV_SYMBOL_ENVELOPE, unread,
                          from ? "from " : "", from ? from : "");
        if(pos > (int)sizeof(pending) - 1) pos = (int)sizeof(pending) - 1;
    }

    /* With a companion app attached, the mesh is being read on a phone - so
     * what is worth knowing from here is that the link is up, not a count of
     * messages somebody else has already seen. */
    if(mesh_companion_is_connected()) {
        lv_snprintf(pending + pos, sizeof(pending) - pos, "\n\n%s Companion app",
                    mesh_companion_get_link() == MESH_LINK_WIFI ? LV_SYMBOL_WIFI
                                                                : LV_SYMBOL_BLUETOOTH);
    } else {
        int mesh_unread = mesh_net_unread_total();
        if(mesh_unread > 0) {
            lv_snprintf(pending + pos, sizeof(pending) - pos, "\n\n%s %d on the mesh",
                        LV_SYMBOL_ENVELOPE, mesh_unread);
        }
    }

    lv_snprintf(detail, sizeof(detail), "%s%s\n\n%s %d%%",
                date, pending,
                ui_battert_27220_get_percent_level(), ui_battery_27220_get_percent());

    lv_label_set_text(scr15_detail, detail);
}

/* Only repaints when the minute changes: a clock that redrew every tick would
 * keep the panel busy for no benefit. */
static void scr15_timer_event(lv_timer_t *t)
{
    LV_UNUSED(t);

    static int shown_minute = -1;
    static int shown_unread = -1;

    time_t    now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    /* The link coming up or going down changes what is shown as surely as a
     * message arriving does, so it counts as a change worth repainting for. */
    int unread = sms_unread_total() + mesh_net_unread_total()
               + (mesh_companion_is_connected() ? 100000 : 0);
    if(tm_now.tm_min == shown_minute && unread == shown_unread) return;

    shown_minute = tm_now.tm_min;
    shown_unread = unread;

    scr15_render();
    ui_disp_full_refr();
}

static void scr15_gesture(int dir, lv_coord_t from_x, lv_coord_t from_y)
{
    LV_UNUSED(from_x);
    LV_UNUSED(from_y);

    if(dir == LV_DIR_TOP) scr_mgr_switch(SCREEN0_ID, false);
}

static void create15(lv_obj_t *parent)
{
    scr15_clock = lv_label_create(parent);
    lv_obj_set_width(scr15_clock, lv_pct(92));
    lv_obj_set_style_text_font(scr15_clock, &lv_font_montserrat_26, LV_PART_MAIN);
    lv_obj_set_style_text_color(scr15_clock, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_text_align(scr15_clock, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(scr15_clock, LV_ALIGN_TOP_MID, 0, 70);

    scr15_detail = lv_label_create(parent);
    lv_obj_set_width(scr15_detail, lv_pct(92));
    lv_obj_set_style_text_font(scr15_detail, FONT_BOLD_SIZE_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(scr15_detail, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_text_align(scr15_detail, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(scr15_detail, LV_LABEL_LONG_WRAP);
    lv_obj_align(scr15_detail, LV_ALIGN_TOP_MID, 0, 120);

    lv_obj_t *hint = lv_label_create(parent);
    lv_obj_set_width(hint, lv_pct(92));
    lv_obj_set_style_text_font(hint, FONT_BOLD_SIZE_15, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(hint, LV_SYMBOL_UP "\nSwipe up to unlock");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -20);

    scr15_render();
}

static void entry15(void)
{
    ui_get_gesture_dir = scr15_gesture;
    lv_timer_resume(touch_chk_timer);

    if(scr15_timer == NULL) {
        scr15_timer = lv_timer_create(scr15_timer_event, 5000, NULL);
    }

    scr15_render();
    ui_disp_full_refr();
}

static void exit15(void)
{
    ui_get_gesture_dir = NULL;
    lv_timer_pause(touch_chk_timer);

    if(scr15_timer) {
        lv_timer_del(scr15_timer);
        scr15_timer = NULL;
    }
    ui_disp_full_refr();
}

static void destroy15(void)
{
    scr15_clock  = NULL;
    scr15_detail = NULL;
}

static scr_lifecycle_t screen15 = {
    .create = create15,
    .entry = entry15,
    .exit  = exit15,
    .destroy = destroy15,
};
#endif
//************************************[ screen 16 ]***************************************** hotspot
#if 1
/* The phone as a WiFi access point that relays a UDP tunnel out over mobile
 * data - enough to carry WireGuard from a laptop with no other way out.
 *
 * See src/apps/udp_relay.cpp for why this relays to one configured endpoint
 * instead of routing, and for what to expect of the throughput. */
static lv_obj_t *scr16_list   = NULL;
static lv_obj_t *scr16_status = NULL;
static lv_obj_t *scr16_toggle = NULL;
static lv_timer_t *scr16_timer = NULL;

// Which field the shared editor was opened for.
enum {
    SCR16_FIELD_HOST = 0,
    SCR16_FIELD_PORT,
    SCR16_FIELD_APN,
    SCR16_FIELD_SSID,
    SCR16_FIELD_PASS,
    SCR16_FIELD_MAX,
};

static int scr16_editing = SCR16_FIELD_HOST;

static const struct {
    const char *name;
    const char *icon;
    bool        numeric;
} scr16_fields[SCR16_FIELD_MAX] = {
    { "Endpoint",  LV_SYMBOL_UPLOAD,   false },
    { "Port",      LV_SYMBOL_UPLOAD,   true  },
    { "APN",       LV_SYMBOL_SETTINGS, false },
    { "AP name",   LV_SYMBOL_WIFI,     false },
    { "AP key",    LV_SYMBOL_WIFI,     false },
};

static void scr16_field_value(int field, char *buf, int len)
{
    const udp_relay_cfg_t *c = udp_relay_get_cfg();

    switch(field) {
        case SCR16_FIELD_HOST: lv_snprintf(buf, len, "%s", c->host[0] ? c->host : "not set"); break;
        case SCR16_FIELD_PORT: lv_snprintf(buf, len, "%u", (unsigned)c->port); break;
        case SCR16_FIELD_APN:  lv_snprintf(buf, len, "%s", c->apn[0] ? c->apn : "auto"); break;
        case SCR16_FIELD_SSID: lv_snprintf(buf, len, "%s", c->ssid); break;
        case SCR16_FIELD_PASS: lv_snprintf(buf, len, "%s", strlen(c->pass) >= 8 ? "set" : "open"); break;
        default: buf[0] = '\0'; break;
    }
}

static void scr16_render(void)
{
    udp_relay_status_t st;
    char line[160];

    if(scr16_status == NULL) return;

    udp_relay_get_status(&st);

    const char *state = (st.state == UDP_RELAY_RUNNING)  ? "On"
                      : (st.state == UDP_RELAY_STARTING) ? "Starting"
                      : (st.state == UDP_RELAY_FAILED)   ? "Failed"
                                                         : "Off";

    if(st.state == UDP_RELAY_OFF) {
        lv_snprintf(line, sizeof(line), "%s", state);
    } else {
        lv_snprintf(line, sizeof(line), "%s - %s\nout %u  in %u  lost %u",
                    state, st.detail,
                    (unsigned)st.to_modem, (unsigned)st.to_client, (unsigned)st.dropped);
    }
    lv_label_set_text(scr16_status, line);

    lv_obj_t *label = lv_obj_get_child(scr16_toggle, 0);
    if(label) lv_label_set_text(label, udp_relay_is_on() ? LV_SYMBOL_STOP "  Stop" : LV_SYMBOL_PLAY "  Start");
}

static void scr16_timer_event(lv_timer_t *t)
{
    LV_UNUSED(t);

    static uint32_t shown = 0xFFFFFFFF;
    udp_relay_status_t st;
    udp_relay_get_status(&st);

    // Only repaint when something actually moved: this panel is slow and the
    // counters tick constantly while a tunnel is up.
    uint32_t fingerprint = st.to_modem + st.to_client + st.dropped + (uint32_t)st.state * 7919;
    if(fingerprint == shown) return;
    shown = fingerprint;

    scr16_render();
    ui_disp_full_refr();
}

static void scr16_back_event(lv_event_t *e)
{
    if(e->code == LV_EVENT_CLICKED) scr_mgr_pop(false);
}

static void scr16_toggle_event(lv_event_t *e)
{
    LV_UNUSED(e);

    if(udp_relay_is_on()) udp_relay_stop();
    else                  udp_relay_start();

    scr16_render();
    ui_disp_full_refr();
}

static void scr16_field_event(lv_event_t *e)
{
    scr16_editing = (int)(intptr_t)lv_event_get_user_data(e);
    scr_mgr_push(SCREEN16_1_ID, false);
}

static void scr16_populate(void)
{
    lv_obj_clean(scr16_list);

    for(int i = 0; i < SCR16_FIELD_MAX; i++) {
        char value[64];
        scr16_field_value(i, value, sizeof(value));

        lv_obj_t *row = lv_list_add_btn(scr16_list, scr16_fields[i].icon, scr16_fields[i].name);

        lv_obj_set_height(row, 32);
        lv_obj_set_style_text_font(row, FONT_BOLD_SIZE_15, LV_PART_MAIN);
        lv_obj_set_style_bg_color(row, DECKPRO_COLOR_BG, LV_PART_MAIN);
        lv_obj_set_style_text_color(row, DECKPRO_COLOR_FG, LV_PART_MAIN);
        lv_obj_set_style_border_color(row, DECKPRO_COLOR_FG, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
        lv_obj_set_style_outline_width(row, 1, LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_radius(row, 8, LV_PART_MAIN);

        lv_obj_t *state = lv_label_create(row);
        lv_obj_set_style_text_font(state, FONT_BOLD_SIZE_14, LV_PART_MAIN);
        lv_label_set_long_mode(state, LV_LABEL_LONG_DOT);
        lv_obj_set_size(state, 96, 16);
        lv_obj_set_style_text_align(state, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
        lv_label_set_text(state, value);

        lv_obj_add_event_cb(row, scr16_field_event, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

static void create16(lv_obj_t *parent)
{
    scr16_status = lv_label_create(parent);
    lv_obj_set_width(scr16_status, lv_pct(94));
    lv_obj_set_style_text_font(scr16_status, FONT_BOLD_SIZE_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(scr16_status, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_label_set_long_mode(scr16_status, LV_LABEL_LONG_WRAP);
    lv_obj_align(scr16_status, LV_ALIGN_TOP_MID, 0, 34);

    scr16_list = lv_list_create(parent);
    scr_scroll_for_epaper(scr16_list);
    lv_obj_set_size(scr16_list, lv_pct(96), LV_VER_RES - 84 - 52);
    lv_obj_align(scr16_list, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_set_style_pad_all(scr16_list, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_row(scr16_list, 5, LV_PART_MAIN);
    lv_obj_set_style_radius(scr16_list, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(scr16_list, DECKPRO_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr16_list, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(scr16_list, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(scr16_list, LV_SCROLLBAR_MODE_OFF);

    scr16_populate();

    lv_obj_t *bar = scr_action_bar_create(parent, 44);
    scr16_toggle = scr_bar_btn_create(bar, LV_SYMBOL_PLAY "  Start", 190, scr16_toggle_event, NULL);

    scr_back_btn_create(parent, "Hotspot", scr16_back_event);
    scr16_render();
}

static void entry16(void)
{
    scr16_populate();
    scr16_render();

    if(scr16_timer == NULL) {
        scr16_timer = lv_timer_create(scr16_timer_event, 1000, NULL);
    }
    ui_disp_full_refr();
}

static void exit16(void)
{
    if(scr16_timer) {
        lv_timer_del(scr16_timer);
        scr16_timer = NULL;
    }
    ui_disp_full_refr();
}

static void destroy16(void)
{
    scr16_list   = NULL;
    scr16_status = NULL;
    scr16_toggle = NULL;
}

static scr_lifecycle_t screen16 = {
    .create = create16,
    .entry = entry16,
    .exit  = exit16,
    .destroy = destroy16,
};
#endif
// --------------------- screen 16.1 --------------------- one hotspot field
#if 1
/* One field at a time, because five text boxes will not fit on this screen and
 * the hardware keyboard can only be pointed at one of them anyway. */
static lv_obj_t *scr16_1_field = NULL;

static void scr16_1_back_event(lv_event_t *e)
{
    if(e->code == LV_EVENT_CLICKED) scr_mgr_pop(false);
}

static void scr16_1_save_event(lv_event_t *e)
{
    LV_UNUSED(e);

    udp_relay_cfg_t cfg = *udp_relay_get_cfg();
    const char *text = lv_textarea_get_text(scr16_1_field);
    if(text == NULL) text = "";

    switch(scr16_editing) {
        case SCR16_FIELD_HOST: lv_snprintf(cfg.host, sizeof(cfg.host), "%s", text); break;
        case SCR16_FIELD_APN:  lv_snprintf(cfg.apn,  sizeof(cfg.apn),  "%s", text); break;
        case SCR16_FIELD_SSID: lv_snprintf(cfg.ssid, sizeof(cfg.ssid), "%s", text); break;
        case SCR16_FIELD_PASS: lv_snprintf(cfg.pass, sizeof(cfg.pass), "%s", text); break;
        case SCR16_FIELD_PORT: {
            int port = atoi(text);
            if(port > 0 && port <= 65535) {
                // The client points its tunnel at this phone on the same port
                // it would have used for the far end, which is one less thing
                // to get wrong in the client's configuration.
                cfg.port        = (uint16_t)port;
                cfg.listen_port = (uint16_t)port;
            }
            break;
        }
        default: break;
    }

    udp_relay_set_cfg(&cfg);
    scr_mgr_pop(false);
}

static const char *scr16_1_keypad_map[] = { "1", "2", "3", "\n",
                                            "4", "5", "6", "\n",
                                            "7", "8", "9", "\n",
                                            ".", "0", LV_SYMBOL_BACKSPACE, ""
                                          };

static void scr16_1_keypad_event(lv_event_t *e)
{
    lv_obj_t   *btnm = (lv_obj_t *)lv_event_get_target(e);
    const char *txt  = lv_btnmatrix_get_btn_text(btnm, lv_btnmatrix_get_selected_btn(btnm));

    if(txt == NULL) return;

    if(strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) lv_textarea_del_char(scr16_1_field);
    else                                      lv_textarea_add_text(scr16_1_field, txt);
}

static void create16_1(lv_obj_t *parent)
{
    char value[64];

    // The port and an address are digits, and digits live on the keyboard's
    // symbol layer, so those get a pad on screen.
    bool numeric = scr16_fields[scr16_editing].numeric ||
                   scr16_editing == SCR16_FIELD_HOST;

    switch(scr16_editing) {
        case SCR16_FIELD_PASS:
            lv_snprintf(value, sizeof(value), "%s", udp_relay_get_cfg()->pass);
            break;
        case SCR16_FIELD_APN:
            lv_snprintf(value, sizeof(value), "%s", udp_relay_get_cfg()->apn);
            break;
        default:
            scr16_field_value(scr16_editing, value, sizeof(value));
            if(strcmp(value, "not set") == 0 || strcmp(value, "auto") == 0) value[0] = '\0';
            break;
    }

    scr16_1_field = scr_field_create(parent, scr16_fields[scr16_editing].name, 40,
                                     value, 47);

    lv_obj_t *hint = lv_label_create(parent);
    lv_obj_set_width(hint, lv_pct(92));
    lv_obj_set_style_text_font(hint, FONT_BOLD_SIZE_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, DECKPRO_COLOR_FG, LV_PART_MAIN);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 100);

    switch(scr16_editing) {
        case SCR16_FIELD_HOST:
            lv_label_set_text(hint, "Address of the far end of the tunnel."); break;
        case SCR16_FIELD_PORT:
            lv_label_set_text(hint, "Used at both ends: the client points its tunnel at this phone on the same port."); break;
        case SCR16_FIELD_APN:
            lv_label_set_text(hint, "Leave empty to let the network choose."); break;
        case SCR16_FIELD_PASS:
            lv_label_set_text(hint, "Eight characters or more, or the network is left open."); break;
        default:
            lv_label_set_text(hint, " "); break;
    }

    if(numeric) {
        lv_obj_t *pad = lv_btnmatrix_create(parent);
        lv_btnmatrix_set_map(pad, scr16_1_keypad_map);
        lv_obj_set_size(pad, lv_pct(96), 112);
        lv_obj_set_style_border_width(pad, 0, LV_PART_MAIN);
        lv_obj_align(pad, LV_ALIGN_BOTTOM_MID, 0, -48);
        lv_obj_add_event_cb(pad, scr16_1_keypad_event, LV_EVENT_VALUE_CHANGED, NULL);
    }

    lv_obj_t *bar = scr_action_bar_create(parent, 38);
    scr_bar_btn_create(bar, LV_SYMBOL_OK "  Save", 106, scr16_1_save_event, NULL);
    scr_bar_btn_create(bar, LV_SYMBOL_CLOSE "  Cancel", 106, scr16_1_back_event, NULL);

    scr_back_btn_create(parent, scr16_fields[scr16_editing].name, scr16_1_back_event);
}

static void entry16_1(void)
{
    lv_group_focus_obj(scr16_1_field);
    ui_disp_full_refr();
}

static void exit16_1(void)
{
    ui_disp_full_refr();
}

static void destroy16_1(void)
{
    scr16_1_field = NULL;
}

static scr_lifecycle_t screen16_1 = {
    .create = create16_1,
    .entry = entry16_1,
    .exit  = exit16_1,
    .destroy = destroy16_1,
};
#endif
//************************************[ UI ENTRY ]******************************************
static lv_obj_t *menu_keypad;
static lv_timer_t *menu_timer = NULL;

/* The touch panel specifically.
 *
 * lv_indev_drv_register() inserts at the head of the list, so
 * lv_indev_get_next(NULL) hands back whichever device was registered *last* -
 * the keypad. Reading that here did two wrong things: it never produced a
 * coordinate, so no swipe was ever detected, and it popped events off the
 * keyboard FIFO that LVGL's own keypad handling should have had. */
static lv_indev_t *indev_get_pointer(void)
{
    lv_indev_t *indev = NULL;

    while((indev = lv_indev_get_next(indev)) != NULL) {
        if(indev->driver && indev->driver->type == LV_INDEV_TYPE_POINTER) return indev;
    }
    return NULL;
}

static void indev_get_gesture_dir(lv_timer_t *t)
{
    LV_UNUSED(t);

    static lv_point_t press_start;
    static bool       is_press = false;
    static bool       fired    = false;

    lv_indev_t *indev = indev_get_pointer();
    if(indev == NULL) return;

    lv_indev_data_t data;
    _lv_indev_read(indev, &data);

    if(data.state != LV_INDEV_STATE_PR) {
        is_press = false;
        fired    = false;
        return;
    }

    if(!is_press) {
        is_press    = true;
        fired       = false;
        press_start = data.point;
    }

    // Captured before dispatching: the handler may switch screens, and the one
    // that arrives will have replaced this.
    ui_indev_read_cb cb = ui_get_gesture_dir;
    if(fired || cb == NULL) return;

    // Measured from where the finger landed, and fired at most once per swipe.
    // The previous version reset the anchor to x=0 after firing, which made the
    // very next sample look like a full swipe in the opposite direction.
    lv_coord_t dx = data.point.x - press_start.x;
    lv_coord_t dy = data.point.y - press_start.y;
    int        dir = 0;

    // Whichever axis the finger travelled furthest along is the one it meant.
    if(LV_ABS(dx) >= LV_ABS(dy)) {
        if(dx <= -UI_SLIDING_DISTANCE)     dir = LV_DIR_LEFT;
        else if(dx >= UI_SLIDING_DISTANCE) dir = LV_DIR_RIGHT;
    } else {
        if(dy >= UI_SLIDING_DISTANCE)      dir = LV_DIR_BOTTOM;
        else if(dy <= -UI_SLIDING_DISTANCE) dir = LV_DIR_TOP;
    }

    if(dir == 0) return;

    fired = true;
    cb(dir, press_start.x, press_start.y);
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

    /* A companion app has the mesh, so its messages are being read on a phone
     * and counting them here would be counting somebody else's post. Texts are
     * still this device's own business either way. */
    bool companion = mesh_companion_is_connected();

    uint16_t unread = sms_unread_total();
    if(!companion) unread += mesh_net_unread_total();

    if(taskbar_statue[TASKBAR_ID_UNREAD] != unread)
    {
        if(unread > 0) {
            lv_obj_clear_flag(menu_taskbar_unread, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(menu_taskbar_unread, LV_OBJ_FLAG_HIDDEN);
        }
        taskbar_statue[TASKBAR_ID_UNREAD] = unread;
    }

    if(taskbar_statue[TASKBAR_ID_COMPANION] != (uint16_t)companion)
    {
        if(companion) {
            lv_label_set_text(menu_taskbar_companion,
                              mesh_companion_get_link() == MESH_LINK_WIFI ? LV_SYMBOL_WIFI
                                                                          : LV_SYMBOL_BLUETOOTH);
            lv_obj_clear_flag(menu_taskbar_companion, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(menu_taskbar_companion, LV_OBJ_FLAG_HIDDEN);
        }
        taskbar_statue[TASKBAR_ID_COMPANION] = companion;
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
// How often the motor pulses while a call is ringing.
#define RING_BUZZ_PERIOD_MS 2000

/* Brings a message screen that is being looked at right now up to date.
 *
 * The list screens only rebuild in entry(), which does not run again while the
 * screen stays on top - so a message arriving while the user is reading the
 * conversation would not appear until they navigated away and back.
 *
 * `same_thread` says whether the change belongs to the conversation that is
 * open; repainting it for a message from someone else would cost a panel
 * refresh and change nothing on screen. */
static void ui_sms_refresh_visible(bool same_thread)
{
    switch(scr_mgr_current_id()) {
        case SCREEN13_ID:
            // Any message reorders the conversation list or changes a marker.
            if(scr13_list) {
                scr13_populate();
                ui_disp_full_refr();
            }
            break;

        case SCREEN13_1_ID:
            if(same_thread && scr13_1_cont) {
                // Read, by definition - it is on screen in front of the user.
                sms_thread_mark_read(ui_active_number);
                scr13_1_populate();
                ui_disp_full_refr();
            }
            break;

        default:
            break;
    }
}

static void phone_event_timer_cb(lv_timer_t *t)
{
    LV_UNUSED(t);

    uint32_t revision_before  = ui_sms_revision;
    bool     touched_open_thread = false;

    modem_sms_rx_t rx;
    while(ui_sms_poll_received(&rx)) {
        sms_add(rx.number, rx.text, rx.ts, SMS_DIR_IN, SMS_ST_OK, true);
        ui_sms_revision++;
        ui_notify_incoming_text();

        if(phone_number_match(rx.number, ui_active_number)) touched_open_thread = true;
    }

    if(ui_send_watch_id != 0) {
        modem_send_state_t st = ui_sms_get_send_state(ui_send_watch_id);
        if(st == MODEM_SEND_OK || st == MODEM_SEND_FAILED) {
            // Read the number before the watch is cleared: an open conversation
            // showing "sending..." needs to be told how it turned out.
            const sms_msg_t *sent = sms_get(ui_send_watch_idx);
            if(sent && phone_number_match(sent->number, ui_active_number)) {
                touched_open_thread = true;
            }

            sms_set_status(ui_send_watch_idx, st == MODEM_SEND_OK ? SMS_ST_OK : SMS_ST_FAILED);
            ui_send_watch_id = 0;
            ui_sms_revision++;
        }
    }

    if(ui_sms_revision != revision_before) {
        ui_sms_refresh_visible(touched_open_thread);
    }

    // Raise the call screen when a call starts ringing. Only on the transition:
    // pushing it back whenever the state happens to be INCOMING would fight a
    // user who deliberately backed out of it.
    static modem_call_state_t last_call_state = MODEM_CALL_IDLE;
    static uint32_t           last_ring_buzz  = 0;
    modem_call_state_t call_state = ui_phone_get_call_state();

    if(call_state == MODEM_CALL_INCOMING && last_call_state != MODEM_CALL_INCOMING &&
       scr_mgr_current_id() != SCREEN8_1_ID) {
        scr_mgr_push(SCREEN8_1_ID, false);
    }

    // Keep buzzing for as long as it rings, rather than once when the call
    // screen appears - a single pulse is easy to miss in a pocket.
    if(call_state == MODEM_CALL_INCOMING) {
        if(last_call_state != MODEM_CALL_INCOMING ||
           lv_tick_elaps(last_ring_buzz) >= RING_BUZZ_PERIOD_MS) {
            last_ring_buzz = lv_tick_get();
            ui_notify_incoming_call();
        }
    }

    /* Lock after a spell of nothing happening. lv_disp_get_inactive_time counts
     * from the last touch or keypress, so it covers both.
     *
     * Never while a call is up, and never while the call screen is showing:
     * locking the phone as it rings, or halfway through a conversation, is the
     * worst possible moment for it. */
    uint32_t autolock = ui_setting_get_autolock_ms();
    if(autolock > 0 &&
       call_state == MODEM_CALL_IDLE &&
       scr_mgr_current_id() != SCREEN15_ID &&
       scr_mgr_current_id() != SCREEN8_1_ID &&
       lv_disp_get_inactive_time(NULL) > autolock) {
        scr_mgr_switch(SCREEN15_ID, false);
    }

    last_call_state = call_state;
}

static void ui_proximity_timer_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    ui_proximity_tick();
}

void ui_phone1_entry(void)
{
    // Before any label exists: the wrappers are zeroed until this runs.
    ui_fonts_init();
    ui_settings_load();

    /* Fast enough to feel immediate when the phone reaches an ear, and idle
     * whenever there is no call to suppress touch during. */
    lv_timer_create(ui_proximity_timer_cb, 250, NULL);

    lv_disp_t *disp = lv_disp_get_default();
    disp->theme = lv_theme_mono_init(disp, false, LV_FONT_DEFAULT);

    touch_chk_timer = lv_timer_create(indev_get_gesture_dir, LV_INDEV_DEF_READ_PERIOD, NULL);
    lv_timer_pause(touch_chk_timer);

    lv_timer_create(phone_event_timer_cb, 500, NULL);

    taskbar_update_timer = lv_timer_create(menu_taskbar_update_timer_cb, 1000, NULL);
    lv_timer_pause(taskbar_update_timer);

    scr_mgr_init();

    scr_mgr_register(SCREEN0_ID,    &screen0);      // menu
    scr_mgr_register(SCREEN1_ID,    &screen1);      // Mesh
    scr_mgr_register(SCREEN1_1_ID,  &screen1_1);    //  - this node
    scr_mgr_register(SCREEN1_2_ID,  &screen1_2);    //  - radio settings
    scr_mgr_register(SCREEN1_3_ID,  &screen1_3);    //     - one value
    scr_mgr_register(SCREEN1_4_ID,  &screen1_4);    //  - conversation
    scr_mgr_register(SCREEN1_5_ID,  &screen1_5);    //     - compose
    scr_mgr_register(SCREEN1_6_ID,  &screen1_6);    //  - companion app link
    scr_mgr_register(SCREEN1_7_ID,  &screen1_7);    //     - one setting
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
    scr_mgr_register(SCREEN14_ID,   &screen14);     // Quick settings
    scr_mgr_register(SCREEN15_ID,   &screen15);     // Lock screen
    scr_mgr_register(SCREEN16_ID,   &screen16);     // Hotspot
    scr_mgr_register(SCREEN16_1_ID, &screen16_1);   //  - one setting
    

    scr_mgr_switch(SCREEN0_ID, false); // set root screen
    scr_mgr_set_anim(LV_SCR_LOAD_ANIM_OVER_LEFT, LV_SCR_LOAD_ANIM_OVER_LEFT, LV_SCR_LOAD_ANIM_OVER_LEFT);

    // menu_keypad = lv_label_create(lv_layer_top());
    // lv_obj_set_style_text_font(menu_keypad, FONT_BOLD_MONO_SIZE_15, LV_PART_MAIN);
    // lv_label_set_text(menu_keypad, " ");
    // lv_obj_align(menu_keypad, LV_ALIGN_BOTTOM_RIGHT, -10, -10);

    // menu_timer = lv_timer_create(menu_keypay_get_event, 40, NULL);
}