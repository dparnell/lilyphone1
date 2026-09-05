#include <Arduino.h>
#include "utilities.h"
#include <GxEPD2_BW.h>
#include <TouchDrvCSTXXX.hpp>
#include <TinyGPS++.h>
#include "lvgl.h"
#include "ui_phone1.h"
#include <Fonts/FreeMonoBold9pt7b.h>
#include "main.h"
#include "peripheral.h"
#include "modem_service.h"
#include "phone_store.h"
#include "system_clock.h"
#include "udp_relay.h"
#include "mesh_net.h"
#include "mesh_companion.h"
#include <Preferences.h>
#include <esp_heap_caps.h>

Preferences preferences;

TinyGsm modem(SerialAT);

XPowersPPM PPM;
BQ27220 bq27220;
Audio audio;

TouchDrvCSTXXX touch;
GxEPD2_BW<GxEPD2_310_GDEQ031T10, GxEPD2_310_GDEQ031T10::HEIGHT> display(GxEPD2_310_GDEQ031T10(BOARD_EPD_CS, BOARD_EPD_DC, BOARD_EPD_RST, BOARD_EPD_BUSY)); // GDEQ031T10 240x320, UC8253, (no inking, backside mark KEGMO 3100)

uint8_t *decodebuffer = NULL;
lv_timer_t *flush_timer = NULL;
int disp_refr_mode = DISP_REFR_MODE_PART;

// One flashing full refresh every this many "the whole screen changed"
// requests; everything in between is a fast partial update.
#define DISP_FULL_REFRESH_EVERY 8
#define DISP_HIBERNATE_DELAY_MS 4000

/* A drag produces a new scroll position every refresh period, and the panel
 * takes a few hundred milliseconds to show one - so painting them all puts the
 * display permanently behind the finger. While the touch is held, updates are
 * limited to this; letting go paints the final position immediately. */
#define DISP_DRAG_UPDATE_MS 700

// Start at the threshold so the first screen after boot gets a clean full pass.
static int         changes_since_full = DISP_FULL_REFRESH_EVERY;
static lv_timer_t *hibernate_timer    = NULL;
static bool        panel_hibernating  = true;
static volatile bool touch_is_down    = false; // set by touchpad_read
static uint32_t    last_flush_ms      = 0;
const char HelloWorld[] = "LilyPhone1";

bool peri_init_st[E_PERI_NUM_MAX] = {0};


/* Prefers internal RAM: LVGL renders pixel by pixel into the draw buffer and
 * GxEPD2 reads the packed one byte by byte, and PSRAM is several times slower
 * for that kind of access. Falls back to PSRAM rather than failing to boot. */
static void *disp_buf_alloc(size_t bytes, bool prefer_psram = false)
{
    void *p = NULL;

    /* Internal RAM by choice, because both of these are walked pixel by pixel
     * over the whole screen on every update and PSRAM is several times slower
     * at that. The exception is when the companion link is on: the Bluetooth
     * and WiFi stacks want more internal memory than is left once a full screen
     * buffer has been taken out of it, and a link that cannot start at all is
     * worse than drawing that is not quite as quick. */
    if(!prefer_psram) p = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if(p == NULL) {
        if(!prefer_psram) {
            Serial.printf("[DISP] %u bytes did not fit in internal RAM, using PSRAM\n",
                          (unsigned)bytes);
        }
        p = ps_malloc(bytes);
    }

    // Whichever was asked for, take the other rather than boot with no display.
    if(p == NULL) p = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if(p) memset(p, 0, bytes);
    return p;
}

static bool ink_screen_init()
{
    display.init(115200, true, 2, false);
    display.setRotation(0);
    display.setFont(&FreeMonoBold9pt7b);
    if (display.epd2.WIDTH < 104) display.setFont(0);
    display.setTextColor(GxEPD_BLACK);
    int16_t tbx, tby; uint16_t tbw, tbh;
    display.getTextBounds(HelloWorld, 0, 0, &tbx, &tby, &tbw, &tbh);
    // center bounding box by transposition of origin:
    uint16_t x = ((display.width() - tbw) / 2) - tbx;
    uint16_t y = ((display.height() - tbh) / 2) - tby;
    display.setFullWindow();
    display.firstPage();
    do
    {
        display.fillScreen(GxEPD_WHITE);
        display.setCursor(x, y);
        display.print(HelloWorld);
    }
    while (display.nextPage());
    display.hibernate();
    return true;
}

/* Powering the panel down after every update means the next one pays a wake-up
 * and re-init before it can draw. Hibernating a few seconds after the last
 * update keeps a burst of screens fast while still parking the panel when the
 * user stops interacting. */
static void hibernate_timer_cb(lv_timer_t *t)
{
    LV_UNUSED(t);

    if(!panel_hibernating) {
        display.hibernate();
        panel_hibernating = true;
    }
    lv_timer_del(hibernate_timer);
    hibernate_timer = NULL;
}

static void hibernate_schedule(void)
{
    if(hibernate_timer == NULL) {
        hibernate_timer = lv_timer_create(hibernate_timer_cb, DISP_HIBERNATE_DELAY_MS, NULL);
    } else {
        lv_timer_reset(hibernate_timer);
    }
}

void disp_hibernate_now(void)
{
    if(hibernate_timer) {
        lv_timer_del(hibernate_timer);
        hibernate_timer = NULL;
    }
    if(!panel_hibernating) {
        display.hibernate();
        panel_hibernating = true;
    }
}

static void flush_timer_cb(lv_timer_t *t)
{
    static int idx = 0;
    lv_disp_t *disp = lv_disp_get_default();
    if(disp->rendering_in_progress == false) {
        /* Mid drag: the finger has moved on several times over by the time one
         * of these lands, so paint at most occasionally and leave the timer
         * running. The release paints the position it actually ended at. */
        if(touch_is_down && (millis() - last_flush_ms) < DISP_DRAG_UPDATE_MS) {
            return;
        }

        lv_coord_t w = LV_HOR_RES;
        lv_coord_t h = LV_VER_RES;

        /* A full window update is the flashing one and costs seconds, against a
         * few hundred milliseconds for a partial. Screens ask for a full
         * refresh on every transition, which is far more often than the panel
         * needs it, so treat the request as "the whole screen changed" and only
         * actually flash periodically to clear accumulated ghosting. */
        bool full = false;
        if(disp_refr_mode == DISP_REFR_MODE_FULL) {
            if(++changes_since_full >= DISP_FULL_REFRESH_EVERY) {
                changes_since_full = 0;
                full = true;
            }
        }

        if(full) {
            display.setFullWindow();
        } else {
            display.setPartialWindow(0, 0, w, h);
        }

        panel_hibernating = false;

        display.firstPage();
        do {
            display.drawInvertedBitmap(0, 0, decodebuffer, w, h - 3, GxEPD_BLACK);
        }
        while (display.nextPage());

        last_flush_ms = millis();
        hibernate_schedule();

        Serial.printf("flush_timer_cb:%d, %s\n", idx++, (full ? "full" : "part"));

        disp_refr_mode = DISP_REFR_MODE_PART;
        lv_timer_pause(flush_timer);
    }
}

static void dips_render_start_cb(struct _lv_disp_drv_t * disp_drv)
{
    if(flush_timer == NULL) {
        flush_timer = lv_timer_create(flush_timer_cb, 10, NULL);
    } else {
        lv_timer_resume(flush_timer);
    }
}

static void disp_flush(lv_disp_drv_t * disp_drv, const lv_area_t * area, lv_color_t * color_p)
{
    uint32_t w = (area->x2 - area->x1);
    uint32_t h = (area->y2 - area->y1);

    uint16_t epd_idx = 0;

    union flush_buf_pixel pixel;

    for(int i = 0; i < w * h; i += 8) {
        pixel.bit.b1 = (color_p + i + 7)->full;
        pixel.bit.b2 = (color_p + i + 6)->full;
        pixel.bit.b3 = (color_p + i + 5)->full;
        pixel.bit.b4 = (color_p + i + 4)->full;
        pixel.bit.b5 = (color_p + i + 3)->full;
        pixel.bit.b6 = (color_p + i + 2)->full;
        pixel.bit.b7 = (color_p + i + 1)->full;
        pixel.bit.b8 = (color_p + i + 0)->full;
        decodebuffer[epd_idx] = pixel.full;
        epd_idx++;
    }

    // Serial.printf("x1=%d, y1=%d, x2=%d, y2=%d\n", area->x1, area->y1, area->x2, area->y2);

    /*IMPORTANT!!!
     *Inform the graphics library that you are ready with the flushing*/
    lv_disp_flush_ready(disp_drv);
}

static void listDir(fs::FS &fs, const char * dirname, uint8_t levels){
    Serial.printf("Listing spiffs directory: %s\n", dirname);

    File root = fs.open(dirname);
    if(!root){
        Serial.println("- failed to open directory");
        return;
    }
    if(!root.isDirectory()){
        Serial.println(" - not a directory");
        return;
    }

    File file = root.openNextFile();
    while(file){
        if(file.isDirectory()){
            Serial.print("  DIR : ");
            Serial.println(file.name());
            if(levels){
                listDir(fs, file.path(), levels -1);
            }
        } else {
            Serial.print("  FILE: ");
            Serial.print(file.name());
            Serial.print("\tSIZE: ");
            Serial.println(file.size());
        }
        file = root.openNextFile();
    }
}

static void touchpad_read(lv_indev_drv_t * indev_drv, lv_indev_data_t * data)
{
    static lv_coord_t last_x = 0;
    static lv_coord_t last_y = 0;

    uint8_t touched = touch.getPoint(&last_x, &last_y, 1);

    touch_is_down = touched != 0;

    if(touched) {
        // Touch is sampled every 10ms; logging every sample floods the USB CDC
        // link and can stall the LVGL task waiting on it.
        data->state = LV_INDEV_STATE_PR;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
    /*Set the last pressed coordinates*/
    data->point.x = last_x;
    data->point.y = last_y;
}

static void lvgl_init(void)
{
    lv_init();

    static lv_disp_draw_buf_t draw_buf_dsc_1;

    /* One buffer, not two: the panel push happens synchronously in the LVGL
     * task, so there is never a second frame being rendered alongside it and
     * the second buffer only cost memory. */
    /* The drawing buffer is by far the largest thing on this device that wants
     * internal RAM - a byte per pixel of the whole screen - so it is what gives
     * way when a companion link needs the room. */
    bool link_wants_room = mesh_companion_link_saved();

    lv_color_t *buf_1 = (lv_color_t *)disp_buf_alloc(sizeof(lv_color_t) * DISP_BUF_SIZE,
                                                     link_wants_room);
    lv_disp_draw_buf_init(&draw_buf_dsc_1, buf_1, NULL, LCD_HOR_SIZE * LCD_VER_SIZE);

    if(link_wants_room) {
        Serial.println("[DISP] drawing buffer in PSRAM, to leave internal RAM for the "
                       "companion link");
    }

    // Packed 1bpp, so an eighth of the pixel count. The old size was the pixel
    // count itself - eight times more than drawInvertedBitmap ever reads. Small
    // enough to stay in internal RAM either way, and read byte by byte on every
    // flush, so it is the last thing that should move.
    decodebuffer = (uint8_t *)disp_buf_alloc(DISP_BUF_SIZE / 8 + 64);
    // lv_disp_draw_buf_init(&draw_buf, lv_disp_buf_p, NULL, DISP_BUF_SIZE);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_HOR_SIZE;
    disp_drv.ver_res = LCD_VER_SIZE;
    disp_drv.flush_cb = disp_flush;
    disp_drv.render_start_cb = dips_render_start_cb;
    disp_drv.draw_buf = &draw_buf_dsc_1;
    // disp_drv.rounder_cb = display_driver_rounder_cb;
    disp_drv.full_refresh = 1;

    lv_disp_drv_register(&disp_drv);

    /*------------------
     * Touchpad
     * -----------------*/
    /*Register a touchpad input device*/
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touchpad_read;
    lv_indev_drv_register(&indev_drv);

    /*------------------
     * Keypad
     * -----------------*/
    /*Register a keypad input device*/
    static lv_indev_drv_t kp_drv;
    lv_indev_drv_init(&kp_drv);
    kp_drv.type = LV_INDEV_TYPE_KEYPAD;
    kp_drv.read_cb = keypad_read;
    lv_indev_t *indev = lv_indev_drv_register(&kp_drv);

    lv_group_t *g = lv_group_create();
    lv_group_set_default(g);
    lv_indev_set_group(indev, g);

}

static bool bq25896_init(void)
{
    // BQ25896 --- 0x6B
    Wire.beginTransmission(BOARD_I2C_ADDR_BQ25896);
    if (Wire.endTransmission() == 0)
    {
        // battery_25896.begin();
        PPM.init(Wire, BOARD_I2C_SDA, BOARD_I2C_SCL, BOARD_I2C_ADDR_BQ25896);
        // set battery charge voltage
        PPM.setChargeTargetVoltage(4288);

        // Set charge current
        PPM.setChargerConstantCurr(1024);

        // Enable measure
        PPM.enableMeasure();

        return true;
    }
    return false;
}

static bool bq27220_init(void)
{
    bool ret = bq27220.init();
    // if(ret) 
    //     bq27220.reset();
    return ret;
}

static bool sd_care_init(void)
{
    if(!SD.begin(BOARD_SD_CS)){
        Serial.println("[SD CARD] Card Mount Failed");
        return false;
    }

    uint64_t cardSize = SD.cardSize() / (1024 * 1024);
    Serial.printf("SD Card Size: %lluMB\n", cardSize);

    uint64_t totalSize = SD.totalBytes() / (1024 * 1024);
    Serial.printf("SD Card Total: %lluMB\n", totalSize);

    uint64_t usedSize = SD.usedBytes() / (1024 * 1024);
    Serial.printf("SD Card Used: %lluMB\n", usedSize);
    return true;
}

static bool A7682E_init(void)
{
    // Set module baud rate and UART pins
    SerialAT.begin(115200, SERIAL_8N1, BOARD_A7682E_TXD, BOARD_A7682E_RXD);

    Serial.println("Powering up modem...");

    // power on
    digitalWrite(BOARD_A7682E_PWRKEY, LOW);
    delay(10);
    digitalWrite(BOARD_A7682E_PWRKEY, HIGH);
    delay(50);
    digitalWrite(BOARD_A7682E_PWRKEY, LOW);
    delay(10);

    Serial.print("Waiting for modem to wake up");
    int retry_cnt = 5;
    int retry = 0;
    while (!modem.testAT(1000)) {
        Serial.println(".");
        if (retry++ > retry_cnt) {
            digitalWrite(BOARD_A7682E_PWRKEY, LOW);
            delay(100);
            digitalWrite(BOARD_A7682E_PWRKEY, HIGH);
            delay(1000);
            digitalWrite(BOARD_A7682E_PWRKEY, LOW);

            Serial.println("\n[A7682E] Init Fail");
            break;
        }
    }
    
    Serial.println();
    bool alive = retry < retry_cnt;
    if(alive) {
        Serial.println("Modem successfully initialized :)");
    }

    delay(200);

    // From here on the modem service task owns SerialAT; nothing else may read
    // or write it, or the unsolicited RING and +CMTI notifications get eaten.
    modem_service_init(alive);

    return alive;
}

void disp_full_refr(void)
{
    disp_refr_mode = DISP_REFR_MODE_FULL;
}

void setup() {
  gpio_hold_dis((gpio_num_t)BOARD_6609_EN);
  gpio_hold_dis((gpio_num_t)BOARD_LORA_EN);
  gpio_hold_dis((gpio_num_t)BOARD_GPS_EN);
  gpio_hold_dis((gpio_num_t)BOARD_1V8_EN);
  gpio_hold_dis((gpio_num_t)BOARD_A7682E_PWRKEY);

  gpio_deep_sleep_hold_dis();

  Serial.begin(115200);

  // IO
  pinMode(BOARD_KEYBOARD_LED, OUTPUT);
  pinMode(BOARD_MOTOR_PIN, OUTPUT);
  pinMode(BOARD_6609_EN, OUTPUT);         // enable 7682 module
  pinMode(BOARD_LORA_EN, OUTPUT);         // enable LORA module
  pinMode(BOARD_GPS_EN, OUTPUT);          // enable GPS module
  pinMode(BOARD_1V8_EN, OUTPUT);          // enable gyroscope module
  pinMode(BOARD_A7682E_PWRKEY, OUTPUT); 
  digitalWrite(BOARD_KEYBOARD_LED, LOW);
  digitalWrite(BOARD_MOTOR_PIN, LOW);
  digitalWrite(BOARD_6609_EN, HIGH);
  digitalWrite(BOARD_LORA_EN, HIGH);
  digitalWrite(BOARD_GPS_EN, HIGH);
  digitalWrite(BOARD_1V8_EN, HIGH);
  digitalWrite(BOARD_A7682E_PWRKEY, HIGH);

  // LORA、SD、EPD use the same SPI, in order to avoid mutual influence;
  // before powering on, all CS signals should be pulled high and in an unselected state;
  pinMode(BOARD_LORA_CS, OUTPUT); 
  digitalWrite(BOARD_LORA_CS, HIGH);
  pinMode(BOARD_LORA_RST, OUTPUT); 
  digitalWrite(BOARD_LORA_RST, HIGH);
  pinMode(BOARD_SD_CS, OUTPUT); 
  digitalWrite(BOARD_SD_CS, HIGH);
  pinMode(BOARD_EPD_CS, OUTPUT); 
  digitalWrite(BOARD_EPD_CS, HIGH);


  // i2c devices
  byte error, address;
  int nDevices = 0;
  Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);
  Serial.printf(" ------------- I2C ------------- \n");
  for(address = 0x01; address < 0x7F; address++){
      Wire.beginTransmission(address);
      error = Wire.endTransmission();
      if(error == 0){ // 0: success.
          nDevices++;
          if(address == BOARD_I2C_ADDR_TOUCH){
              // flag_Touch_init = true;
              Serial.printf("[0x%x] TOUCH found!\n", address);
          } else if (address == BOARD_I2C_ADDR_LTR_553ALS) {
              Serial.printf("[0x%x] LTR_553ALS found!\n", address);
          } else if (address == BOARD_I2C_ADDR_GYROSCOPDE) {
              Serial.printf("[0x%x] GYROSCOPDE found!\n", address);
          } else if (address == BOARD_I2C_ADDR_KEYBOARD) {
              Serial.printf("[0x%x] KEYBOARD found!\n", address);
          } else if (address == BOARD_I2C_ADDR_BQ27220) {
              Serial.printf("[0x%x] BQ27220 found!\n", address);
          } else if (address == BOARD_I2C_ADDR_BQ25896) {
              Serial.printf("[0x%x] BQ25896 found!\n", address);
          }
      }
  }

  Serial.printf(" ------------- SPIFFS ------------- \n");

  if(!SPIFFS.begin(true)){
      Serial.println("SPIFFS Mount Failed");
      return;
  }

  listDir(SPIFFS, "/", 0);
  Serial.println(" ------------- PERI ------------- ");

  // SPI
  SPI.begin(BOARD_SPI_SCK, BOARD_SPI_MISO, BOARD_SPI_MOSI);

  // init peripheral
  touch.setPins(BOARD_TOUCH_RST, BOARD_TOUCH_INT);
  peri_init_st[E_PERI_INK_SCREEN] = ink_screen_init();
  peri_init_st[E_PERI_TOUCH]      = touch.begin(Wire, BOARD_I2C_ADDR_TOUCH, BOARD_TOUCH_SDA, BOARD_TOUCH_SCL);
  peri_init_st[E_PERI_KYEPAD]     = keypad_init(BOARD_I2C_ADDR_KEYBOARD);
  peri_init_st[E_PERI_BQ25896]    = bq25896_init();
  peri_init_st[E_PERI_BQ27220]    = bq27220_init();
  peri_init_st[E_PERI_SD]         = sd_care_init();
  peri_init_st[E_PERI_GPS]        = gps_init();
  peri_init_st[E_PERI_BHI260AP]   = BHI260AP_init();
  peri_init_st[E_PERI_LTR_553ALS] = LTR553_init();
  // Restore the chosen time zone before anything renders a clock.
  system_clock_init();

  peri_init_st[E_PERI_A7682E]     = A7682E_init();

  phone_store_init();
  udp_relay_init();

  // After the SPI bus is up, and after the display: the mesh shares the bus.
  peri_init_st[E_PERI_LORA] = mesh_net_init();

  lvgl_init();

  /* After the display has taken the memory it needs, so that what is left is
   * what the radio gets - and the link is what decides where the drawing buffer
   * went, a few lines up. */
  mesh_companion_boot();

  ui_phone1_entry();

  disp_full_refr();
}

void loop() {
    lv_task_handler();
    
    delay(1);
}

