
#include "utilities.h"
#include "peripheral.h"
#include "system_clock.h"
#include <TinyGPS++.h>

/* clang-format off */

TinyGPSPlus gps;

/* Satellites in view, which is a different question from satellites in use.
 *
 * The count the rest of this file reports comes from GGA and means "used in the
 * fix" - so it is zero both when the antenna hears nothing and when it hears
 * plenty but cannot get the four it needs to place itself. Those want opposite
 * responses: one is a hardware or sky problem, the other is only patience. GSV
 * carries the number in view, and its third field is that number.
 *
 * A multi-constellation receiver sends one GSV per constellation, so these are
 * summed rather than taken one at a time. */
static TinyGPSCustom gps_in_view(gps, "GPGSV", 3);
static TinyGPSCustom glo_in_view(gps, "GLGSV", 3);
static TinyGPSCustom gal_in_view(gps, "GAGSV", 3);
static TinyGPSCustom gnss_in_view(gps, "GNGSV", 3);

static int satellites_in_view(void)
{
    int total = 0;

    if(gps_in_view.isValid())  total += atoi(gps_in_view.value());
    if(glo_in_view.isValid())  total += atoi(glo_in_view.value());
    if(gal_in_view.isValid())  total += atoi(gal_in_view.value());
    if(gnss_in_view.isValid()) total += atoi(gnss_in_view.value());

    return total;
}
static bool GPS_Recovery();
bool setupGPS();
void displayInfo();

static TaskHandle_t gps_handle;
static double gps_lat=0, gps_lng=0, gps_altitude=0, gps_speed=0;
static uint16_t gps_year=0;
static uint8_t gps_month=0, gps_day=0;
static uint8_t gps_hour=0, gps_minute=0, gps_second=0;
static uint32_t gps_vsat=0;

bool updated_time_from_gps = false;

uint8_t buffer[256];

bool gps_init(void)
{   
    bool result = false;
    // L76K GPS USE 9600 BAUDRATE
    // result = setupGPS();
    if(!result) {
        // Set u-blox m10q gps baudrate 38400
        /* Four times the default receive buffer, because the cost of running
         * out is silent corruption rather than an error: NMEA arrives
         * continuously at 38400 baud, and a couple of hundred bytes is only
         * about sixty milliseconds of it. Anything that keeps this task off the
         * CPU for longer - and a Bluetooth stack servicing connection events
         * is exactly that sort of thing - loses bytes out of the middle of
         * sentences, which then fail their checksums and are thrown away. */
        SerialGPS.setRxBufferSize(1024);
        SerialGPS.begin(38400, SERIAL_8N1, BOARD_GPS_RXD, BOARD_GPS_TXD);
        result = GPS_Recovery();
        if (!result) {
            SerialGPS.updateBaudRate(9600);
            result = GPS_Recovery();
            if (!result) {
                Serial.println("GPS Connect failed~!");
                result = false;
            }
            SerialGPS.updateBaudRate(38400);
        }
    }
    /* Only once. gps_init() is called again when the module is switched back
     * on - a receiver that has lost power comes back at its defaults and needs
     * the whole configuring again - but the task it created is still there. */
    if(result && gps_handle == NULL) {
        Serial.println("GPS Task Create...!");
        gps_task_create();
    }
    return result;
}

void gps_task(void *param)
{
    vTaskSuspend(gps_handle);
    while(1)
    {
        while (Serial.available()) {
            SerialGPS.write(Serial.read());
        }

        while (SerialGPS.available()) {
            int c = SerialGPS.read();
            // Serial.write(c);
            if (gps.encode(c)) {
                displayInfo();
            }
        }

        if (millis() > 30000 && gps.charsProcessed() < 10) {
            Serial.println(F("No GPS detected: check wiring."));
            delay(1000);
        }

        /* Every so often, enough to tell the ways this fails apart.
         *
         * No characters at all is a receiver that has stopped or lost power.
         * Characters arriving but checksums failing is data being dropped
         * before it is read - the receiver is fine and something else is
         * holding the CPU. Both counts healthy with no satellites is simply a
         * receiver that cannot see the sky, and nothing to do with the
         * firmware at all. */
        static uint32_t next_report = 0;
        if (millis() > next_report) {
            next_report = millis() + 10000;

            Serial.printf("[GPS] %u chars, %u good sentences, %u bad checksums, "
                          "%d in view, %u in use\n",
                          (unsigned)gps.charsProcessed(), (unsigned)gps.passedChecksum(),
                          (unsigned)gps.failedChecksum(), satellites_in_view(),
                          (unsigned)gps_vsat);
        }

        delay(1);
    }
}

void gps_task_create(void)
{
    xTaskCreate(gps_task, "gps_task", 1024 * 3, NULL, GPS_PRIORITY, &gps_handle);
    // vTaskSuspend(gps_handle);
}

/* Guarded, because gps_handle is only set once gps_init() has got as far as
 * creating the task - and vTaskSuspend(NULL) suspends the *calling* task, so
 * asking to pause a GPS that never started would have stopped the UI instead. */
/* Whether gps_init() has ever got as far as creating the task. False means the
 * receiver was switched off when the phone booted and was never set up. */
bool gps_is_started(void)
{
    return gps_handle != NULL;
}

void gps_task_suspend(void)
{
    if(gps_handle) vTaskSuspend(gps_handle);
}

void gps_task_resume(void)
{
    if(gps_handle) vTaskResume(gps_handle);
}

/* Whether the receiver is being read at all. It is suspended unless the GPS
 * screen is open or the mesh wants a position, so "no fix" and "not looking"
 * are different things and a status icon has to tell them apart. */
bool gps_is_running(void)
{
    return gps_handle != NULL && eTaskGetState(gps_handle) != eSuspended;
}

/* A position from the last few seconds. Validity alone is not enough - it stays
 * true on the last fix long after the receiver has lost the sky. */
bool gps_has_fix(void)
{
    return gps.location.isValid() && gps.location.age() < 10000;
}

/* Restarts the receiver from nothing.
 *
 * A warm start: the ephemeris goes, the almanac and the last known position
 * stay. Bad ephemeris is what a stuck receiver usually has, and throwing it
 * away is enough to unstick one - while keeping the almanac is what lets the
 * next fix take a minute instead of a quarter of an hour, since the almanac is
 * broadcast slowly and takes over twelve minutes of clear sky to collect again.
 *
 * A cold start would discard that too. It is the right tool only for an almanac
 * that is actually corrupt, which is rare, and the wrong one indoors where the
 * replacement may never arrive - a "reset" that leaves the receiver worse off
 * for a quarter of an hour is a trap rather than a repair.
 *
 * The GPS task is the only other user of the serial port, so it is paused for
 * the duration rather than raced with. This blocks its caller for over a
 * second, which is acceptable for something behind a button and would not be
 * anywhere else.
 */
bool gps_reset(void)
{
    if(gps_handle == NULL) {
        Serial.println("[GPS] not running, so there is nothing to reset");
        return false;
    }

    gps_task_suspend();

    /* UBX-CFG-RST: clear the ephemeris only, controlled software reset. Not
     * acknowledged - by the time it would answer it has restarted - so there is
     * nothing to wait for but the receiver coming back. */
    const uint8_t cfg_rst[] = {
        0xB5, 0x62, 0x06, 0x04, 0x04, 0x00, 0x01, 0x00, 0x01, 0x00, 0x10, 0x6A
    };
    SerialGPS.write(cfg_rst, sizeof(cfg_rst));
    delay(1200);

    // Forget the last fix as well, or the screen goes on showing a position
    // from before the reset as though it were current.
    gps_lat = gps_lng = gps_altitude = gps_speed = 0;
    gps_year = 0;
    gps_month = gps_day = 0;
    gps_hour = gps_minute = gps_second = 0;
    gps_vsat = 0;

    // The same two-baud dance gps_init() does, since a reset receiver comes back
    // at whatever its defaults are rather than at whatever it was set to.
    while(SerialGPS.available()) SerialGPS.read();

    bool ok = GPS_Recovery();
    if(!ok) {
        SerialGPS.updateBaudRate(9600);
        ok = GPS_Recovery();
        SerialGPS.updateBaudRate(38400);
        if(ok) ok = GPS_Recovery();
    }

    gps_task_resume();

    Serial.printf("[GPS] reset %s\n", ok ? "done; the next fix should take about a minute"
                                          : "failed, the receiver did not answer");
    return ok;
}

void gps_get_coord(double *lat, double *lng)
{
    *lat = gps_lat;
    *lng = gps_lng;
}

void gps_get_data(uint16_t *year, uint8_t *month, uint8_t *day)
{
    *year = gps_year;
    *month = gps_month;
    *day = gps_day;
}

void gps_get_time(uint8_t *hour, uint8_t *minute, uint8_t *second)
{
    *hour = gps_hour;
    *minute = gps_minute;
    *second = gps_second;
}

void gps_get_satellites(uint32_t *vsat)
{
    *vsat = gps_vsat;   // Visible Satellites
}

void gps_get_speed(double *speed)
{
    *speed = gps_speed;
}

/* clang-format on */
/* Called for every completed sentence.
 *
 * The values it caches feed the GPS screen and the clock, so they follow every
 * sentence - but the logging does not. A u-blox emits several sentences per
 * navigation epoch, so this ran twenty-odd serial writes several times a
 * second, from the highest priority task in the firmware, over a USB link that
 * blocks when the host is not reading. A task blocked on logging is a task not
 * reading the receiver, and NMEA that is not read in time is NMEA lost - so the
 * report is one line a second regardless of how much arrives.
 */
void displayInfo()
{
    if (gps.location.isValid()) {
        gps_lat = gps.location.lat();
        gps_lng = gps.location.lng();
    }

    if (gps.date.isValid()) {
        gps_year  = gps.date.year();
        gps_month = gps.date.month();
        gps_day   = gps.date.day();
    }

    if (gps.time.isValid()) {
        gps_hour   = gps.time.hour();
        gps_minute = gps.time.minute();
        gps_second = gps.time.second();

        if (!updated_time_from_gps && gps.date.isValid()) {
            /* Satellite time is UTC. It goes through system_clock rather than
             * mktime() because mktime reads its input as local time, and the
             * modem may have put a real time zone in force by now. */
            if (system_clock_set_utc(CLOCK_SRC_GPS, gps_year, gps_month, gps_day,
                                     gps_hour, gps_minute, gps_second)) {
                updated_time_from_gps = true;
                Serial.println("[GPS] clock set from satellite time");
            }
        }
    }

    if (gps.satellites.isValid()) gps_vsat  = gps.satellites.value();
    if (gps.speed.isValid())      gps_speed = gps.speed.kmph();

    static uint32_t next_log = 0;
    if (millis() < next_log) return;
    next_log = millis() + 1000;

    if (gps.location.isValid()) {
        Serial.printf("[GPS] %.6f, %.6f  %u sats  %.1f kmph  %02u:%02u:%02u\n",
                      gps_lat, gps_lng, (unsigned)gps_vsat, gps_speed,
                      gps_hour, gps_minute, gps_second);
    } else {
        /* Time without a position is the ordinary way to have no fix: the
         * receiver has decoded the clock off a satellite it can hear but cannot
         * hear the four it needs to place itself. Indoors, that can go on
         * indefinitely. */
        int in_view = satellites_in_view();

        Serial.printf("[GPS] no fix yet, %d in view, %s\n", in_view,
                      in_view == 0 ? "nothing is being heard at all - antenna or sky"
                                   : "which is not yet the four a position needs");
    }
}

/* clang-format off */

bool setupGPS()
{
    // L76K GPS USE 9600 BAUDRATE
    SerialGPS.begin(9600, SERIAL_8N1, BOARD_GPS_RXD, BOARD_GPS_TXD);
    bool result = false;
    uint32_t startTimeout ;
    for (int i = 0; i < 3; ++i) {
        SerialGPS.write("$PCAS03,0,0,0,0,0,0,0,0,0,0,,,0,0*02\r\n");
        delay(5);
        // Get version information
        startTimeout = millis() + 3000;
        Serial.print("Try to init L76K . Wait stop .");
        while (SerialGPS.available()) {
            Serial.print(".");
            SerialGPS.readString();
            if (millis() > startTimeout) {
                Serial.println("Wait L76K stop NMEA timeout!");
                return false;
            }
        };
        Serial.println();
        SerialGPS.flush();
        delay(200);

        SerialGPS.write("$PCAS06,0*1B\r\n");
        startTimeout = millis() + 500;
        String ver = "";
        while (!SerialGPS.available()) {
            if (millis() > startTimeout) {
                Serial.println("Get L76K timeout!");
                return false;
            }
        }
        SerialGPS.setTimeout(10);
        ver = SerialGPS.readStringUntil('\n');
        if (ver.startsWith("$GPTXT,01,01,02")) {
            Serial.println("L76K GNSS init succeeded, using L76K GNSS Module\n");
            result = true;
            break;
        }
        delay(500);
    }
    // Initialize the L76K Chip, use GPS + GLONASS
    SerialGPS.write("$PCAS04,5*1C\r\n");
    delay(250);
    SerialGPS.write("$PCAS03,1,1,1,1,1,1,1,1,1,1,,,0,0*26\r\n");
    delay(250);
    // Switch to Vehicle Mode, since SoftRF enables Aviation < 2g
    SerialGPS.write("$PCAS11,3*1E\r\n");
    return result;
}


static int getAck(uint8_t *buffer, uint16_t size, uint8_t requestedClass, uint8_t requestedID)
{
    uint16_t    ubxFrameCounter = 0;
    bool        ubxFrame = 0;
    uint32_t    startTime = millis();
    uint16_t    needRead;

    while (millis() - startTime < 800) {
        while (SerialGPS.available()) {
            int c = SerialGPS.read();
            switch (ubxFrameCounter) {
            case 0:
                if (c == 0xB5) {
                    ubxFrameCounter++;
                }
                break;
            case 1:
                if (c == 0x62) {
                    ubxFrameCounter++;
                } else {
                    ubxFrameCounter = 0;
                }
                break;
            case 2:
                if (c == requestedClass) {
                    ubxFrameCounter++;
                } else {
                    ubxFrameCounter = 0;
                }
                break;
            case 3:
                if (c == requestedID) {
                    ubxFrameCounter++;
                } else {
                    ubxFrameCounter = 0;
                }
                break;
            case 4:
                needRead = c;
                ubxFrameCounter++;
                break;
            case 5:
                needRead |=  (c << 8);
                ubxFrameCounter++;
                break;
            case 6:
                if (needRead >= size) {
                    ubxFrameCounter = 0;
                    break;
                }
                if (SerialGPS.readBytes(buffer, needRead) != needRead) {
                    ubxFrameCounter = 0;
                } else {
                    return needRead;
                }
                break;

            default:
                break;
            }
        }
    }
    return 0;
}

static bool GPS_Recovery()
{
    uint8_t cfg_clear1[] = {0xB5, 0x62, 0x06, 0x09, 0x0D, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x1C, 0xA2};
    uint8_t cfg_clear2[] = {0xB5, 0x62, 0x06, 0x09, 0x0D, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x1B, 0xA1};
    uint8_t cfg_clear3[] = {0xB5, 0x62, 0x06, 0x09, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x03, 0x1D, 0xB3};
    SerialGPS.write(cfg_clear1, sizeof(cfg_clear1));

    if (getAck(buffer, 256, 0x05, 0x01)) {
        Serial.println("Get ack successes!");
    }
    SerialGPS.write(cfg_clear2, sizeof(cfg_clear2));
    if (getAck(buffer, 256, 0x05, 0x01)) {
        Serial.println("Get ack successes!");
    }
    SerialGPS.write(cfg_clear3, sizeof(cfg_clear3));
    if (getAck(buffer, 256, 0x05, 0x01)) {
        Serial.println("Get ack successes!");
    }

    // UBX-CFG-RATE, Size 8, 'Navigation/measurement rate settings'
    uint8_t cfg_rate[] = {0xB5, 0x62, 0x06, 0x08, 0x00, 0x00, 0x0E, 0x30};
    SerialGPS.write(cfg_rate, sizeof(cfg_rate));
    if (getAck(buffer, 256, 0x06, 0x08)) {
        Serial.println("Get ack successes!");
    } else {
        return false;
    }
    return true;
}
