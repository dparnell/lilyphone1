
#include <Wire.h>
#include <SPI.h>
#include <Arduino.h>
#include "SensorBHI260AP.hpp"
#include "utilities.h"
#include "peripheral.h"

SensorBHI260AP bhy;
struct bhy2_data_xyz accel_data;
struct bhy2_data_xyz gyro_data;
struct bhy2_data_xyz magn_data;

float accel_factor;
float gyro_factor;
float magn_factor;

/* What this particular BHI260AP turned out to have.
 *
 * The BHI260AP is a sensor hub, not a fixed set of sensors: the accelerometer
 * and gyroscope are its own, but a magnetometer is an external part wired to its
 * secondary bus, which may or may not be fitted. Nothing on the main I2C bus can
 * say - the hub is the only thing that knows, and configure() refuses a sensor
 * it does not present, which is what these record.
 *
 * The orientation sensor is the one that matters for a compass. It is the hub's
 * own fusion of all three, giving a heading already corrected for tilt and for
 * the iron in the phone around it - the parts of a compass that are genuinely
 * hard to get right.
 */
static bool have_magnetometer = false;
static bool have_orientation  = false;

static struct bhy2_data_orientation orientation_data;

static void orientation_process_callback(uint8_t sensor_id, uint8_t *data_ptr,
                                         uint32_t len, uint64_t *timestamp)
{
    bhy2_parse_orientation(data_ptr, &orientation_data);
}

void accel_process_callback(uint8_t sensor_id, uint8_t *data_ptr, uint32_t len, uint64_t *timestamp)
{
    accel_factor = get_sensor_default_scaling(sensor_id);
    bhy2_parse_xyz(data_ptr, &accel_data);
    // Serial.print(bhy.getSensorName(sensor_id));
    // Serial.print(":");
    // Serial.printf("x: %f, y: %f, z: %f;\r\n",
    //               accel_data.x * accel_factor,
    //               accel_data.y * accel_factor,
    //               accel_data.z * accel_factor
    //              );
}

void gyro_process_callback(uint8_t sensor_id, uint8_t *data_ptr, uint32_t len, uint64_t *timestamp)
{
    gyro_factor = get_sensor_default_scaling(sensor_id);
    bhy2_parse_xyz(data_ptr, &gyro_data);
    // Serial.print(bhy.getSensorName(sensor_id));
    // Serial.print(":");
    // Serial.printf("x: %f, y: %f, z: %f;\r\n",
    //               gyro_data.x * gyro_factor,
    //               gyro_data.y * gyro_factor,
    //               gyro_data.z * gyro_factor
    //              );
}

void magn_process_callback(uint8_t sensor_id, uint8_t *data_ptr, uint32_t len, uint64_t *timestamp)
{
    magn_factor = get_sensor_default_scaling(sensor_id);
    bhy2_parse_xyz(data_ptr, &magn_data);
    Serial.print(bhy.getSensorName(sensor_id));
    // Serial.print(":");
    // Serial.printf("x: %f, y: %f, z: %f;\r\n",
    //               magn_data.x * magn_factor,
    //               magn_data.y * magn_factor,
    //               magn_data.z * magn_factor
    //              );
}

/**
 * int val_type 
 *      1 --- acceleration
 *      2 --- gyroscope
 *      3 --- Magnetometer
*/
void BHI260AP_get_val(int val_type, float *x, float *y, float *z)
{
    bhy.update();

    switch (val_type) {
        case 1:
            *x = accel_data.x * accel_factor;
            *y = accel_data.y * accel_factor;
            *z = accel_data.z * accel_factor;
         break;
        case 2 :
            *x = gyro_data.x * gyro_factor;
            *y = gyro_data.y * gyro_factor;
            *z = gyro_data.z * gyro_factor;
         break;
        case 3:
            *x = magn_data.x * magn_factor;
            *y = magn_data.y * magn_factor;
            *z = magn_data.z * magn_factor;
         break;
        default:
            *x = 0;
            *y = 0;
            *z = 0;
            break;
    }
}

bool BHI260AP_init(void)
{
    // Set the reset pin and interrupt pin, if any
    bhy.setPins(BOARD_GYROSCOPDE_RST, BOARD_GYROSCOPDE_INT);

    if (!bhy.init(Wire, BOARD_I2C_SDA, BOARD_I2C_SCL, BHI260AP_SLAVE_ADDRESS_L)) {
        Serial.print("Failed to init BHI260AP - ");
        Serial.println(bhy.getError());
        return false;
    }

    // Output all available sensors to Serial
    // bhy.printSensors(Serial);

    float sample_rate = 100.0;      /* Read out hintr_ctrl measured at 100Hz */
    uint32_t report_latency_ms = 0; /* Report immediately */

    // Enable acceleration
    bhy.configure(SENSOR_ID_ACC_PASS, sample_rate, report_latency_ms);
    // Enable gyroscope
    bhy.configure(SENSOR_ID_GYRO_PASS, sample_rate, report_latency_ms);

    // Set the acceleration sensor result callback function
    bhy.onResultEvent(SENSOR_ID_ACC_PASS, accel_process_callback);

    // Set the gyroscope sensor result callback function
    bhy.onResultEvent(SENSOR_ID_GYRO_PASS, gyro_process_callback);

    // Set the Magnetometer sensor result callback function
    bhy.onResultEvent(SENSOR_ID_MAG_PASS, magn_process_callback);
    bhy.onResultEvent(SENSOR_ID_ORI, orientation_process_callback);

    /* Whether there is a compass here at all. The vendor firmware registered a
     * magnetometer callback but never enabled the sensor, so that path had
     * never produced a reading and said nothing about the hardware either way.
     * configure() checks the hub's own list of what it presents, so a refusal
     * here is the answer rather than a failure. */
    have_magnetometer = bhy.configure(SENSOR_ID_MAG_PASS, sample_rate, report_latency_ms);
    have_orientation  = bhy.configure(SENSOR_ID_ORI, sample_rate, report_latency_ms);

    Serial.printf("[BHI260AP] magnetometer %s, orientation %s\n",
                  have_magnetometer ? "present" : "absent",
                  have_orientation ? "present" : "absent");

    // The whole list, once, since this is the only thing that can produce it.
    bhy.printSensors(Serial);

    return true;
}

bool BHI260AP_has_compass(void)
{
    return have_orientation || have_magnetometer;
}

bool BHI260AP_get_heading(float *heading, float *pitch, float *roll)
{
    if(!have_orientation) return false;

    bhy.update();

    // The hub reports Euler angles in 1/32768ths of a turn.
    const float scale = 360.0f / 32768.0f;

    if(heading) *heading = orientation_data.heading * scale;
    if(pitch)   *pitch   = orientation_data.pitch * scale;
    if(roll)    *roll    = orientation_data.roll * scale;

    return true;
}