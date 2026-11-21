#ifndef __SENSOR_H__
#define __SENSOR_H__

#include <stdbool.h>

#include "esp_err.h"
#include "ui.h"

// Local I2C environmental sensor bus (separate from ES8311's I2C_NUM_0 on GPIO11/12).
#define SENSOR_I2C_SDA_GPIO 5
#define SENSOR_I2C_SCL_GPIO 6
#define SENSOR_AHT20_ADDR   0x38
#define SENSOR_BMP280_ADDR  0x77 // 0x77 when SDO is high (AHT20+BMP280 combo default); 0x76 when SDO is low

// Initialize the fixed-address AHT20 and BMP280 sensors on the local I2C bus.
esp_err_t sensor_init(void);

// Sample the initialized sensor(s) into *out. Returns true if at least one valid field
// was measured; otherwise sets *out to invalid (valid=false, fields=NAN).
bool sensor_read(sensor_reading_t *out);

// Short human-readable summary of initialized chip(s) or "none".
const char *sensor_name(void);

#endif
