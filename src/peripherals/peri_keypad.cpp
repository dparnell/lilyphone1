
#include <Adafruit_TCA8418.h>
#include "utilities.h"
#include "peripheral.h"
#include "lvgl.h"

#define KEYPAD_ROWS 4
#define KEYPAD_COLS 10
#define KEYPAD_PRESS_VAL_MIN   129
#define KEYPAD_PRESS_VAL_MAX   163
#define KEYPAD_RELEASE_VAL_MIN 1
#define KEYPAD_RELEASE_VAL_MAX 35

const char keymap[KEYPAD_ROWS][KEYPAD_COLS] = {
    {'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p'},
    {'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', LV_KEY_BACKSPACE},
    {  0, 'z', 'x', 'c', 'v', 'b', 'n', 'm', '$', LV_KEY_ENTER},
    {  0,   0,   0,   0,   0,   0,   0, ' ',   0, 0},
};

const char shift_keymap[KEYPAD_ROWS][KEYPAD_COLS] = {
    {'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P'},
    {'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', LV_KEY_BACKSPACE},
    {  0, 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '$', LV_KEY_ENTER},
    {  0,   0,   0,   0,   0,   0,   0, ' ',   0, 0},
};

const char sym_keymap[KEYPAD_ROWS][KEYPAD_COLS] = {
    {'#', '1', '2', '3', '(', ')', '_', '-', '+', '@'},
    {'*', '4', '5', '6', '/', ':', ';', '\'', '"', LV_KEY_BACKSPACE},
    {  0, '7', '8', '9', '?', '!', ',', '.', '$', LV_KEY_ENTER},
    {  0,   0,   0,   0,   0,   0,   0, ' ',   0, 0},
};

const char alt_keymap[KEYPAD_ROWS][KEYPAD_COLS] = {
    {LV_KEY_ESC, 0, LV_KEY_END, 0, 0, 0, LV_KEY_UP, 0, 0, LV_KEY_PREV},
    {LV_KEY_HOME, 0, LV_KEY_DOWN, 0, 0, 0, 0, 0, 0, LV_KEY_BACKSPACE},
    {  0, 0, 0, 0, 0, 0, 0, 0, 0, LV_KEY_ENTER},
    {  0,   0,   0,   0,   0,   0,   0, ' ',   0, 0},
};


bool shift = false;
bool alt = false;
bool sym = false;

#define K_ALT 29
#define K_LEFT_SHIFT 34
#define K_SYM 31
#define K_RIGHT_SHIFT 30


Adafruit_TCA8418 keypad; 

bool keypad_init(int address)
{
    if(!i2cIsInit(0)){
        Wire.begin(BOARD_KEYBOARD_SDA, BOARD_KEYBOARD_SCL);
        Wire.beginTransmission(address);
        Wire.endTransmission(true);
    }

    if (!keypad.begin(address, &Wire)) {
        // Serial.println("keypad not found, check wiring & pullups!");
        log_e("keypad not found, check wiring & pullups!");
        return false;
    }

    // configure the size of the keypad matrix.
    // all other pins will be inputs
    keypad.matrix(KEYPAD_ROWS, KEYPAD_COLS);

    // flush the internal buffer
    keypad.flush();

    return true;
}


void keypad_read(lv_indev_drv_t * indev_drv, lv_indev_data_t * data) {
    char c = -1;
    int state = -1;
    int row, col;
    int k = keypad.getEvent();
    int v = keypad.available();

    if(k >=KEYPAD_RELEASE_VAL_MIN && k <= KEYPAD_RELEASE_VAL_MAX){ // release event
        k = k - KEYPAD_RELEASE_VAL_MIN;

        if(k == K_LEFT_SHIFT || k == K_RIGHT_SHIFT) {
            shift = false;
        } else if(k == K_ALT) {
            alt = false;
        } else if(k == K_SYM) {
            sym = false;
        } else {
            state = KEYPAD_RELEASE;        
            data->state = LV_INDEV_STATE_PRESSED;
        }
    }   

    if(k >=KEYPAD_PRESS_VAL_MIN && k <= KEYPAD_PRESS_VAL_MAX){ // press event
        k = k - KEYPAD_PRESS_VAL_MIN;

        if(k == K_LEFT_SHIFT || k == K_RIGHT_SHIFT) {
            shift = true;
        } else if(k == K_ALT) {
            alt = true;
        } else if(k == K_SYM) {
            sym = true;
        } else {
            state = KEYPAD_PRESS;
            data->state = LV_INDEV_STATE_RELEASED;
        }
    }

    if(state != -1){
        row = k / KEYPAD_COLS;
        col = (KEYPAD_COLS-1) - k % KEYPAD_COLS;
        if(shift) {
            c = shift_keymap[row][col];
        } else if(sym) {
            c = sym_keymap[row][col];
        } else if(alt) {
            c = alt_keymap[row][col];
        } else {
            c = keymap[row][col];
        }

        if(c > 32) {
            Serial.printf("k=%d, v=%d, press:%d, %d, '%c'\n", k, v, row, col, c);
        } else {
            Serial.printf("k=%d, v=%d, press:%d, %d, %d\n", k, v, row, col, c);
        }

        data->key = c;
    }

    data->continue_reading = v > 0;
}
