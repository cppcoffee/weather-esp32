#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "ui.h"

// Local I2C environmental sensor bus (separate from ES8311's I2C_NUM_0 on GPIO11/12).
#define SENSOR_I2C_SDA_GPIO 5
#define SENSOR_I2C_SCL_GPIO 6

// Scan the I2C bus and initialize any supported temperature/humidity/pressure chips
// (BME280, BME680/688, AHT20/21, SHT4x, SHT3x, BMP280, BMP388/390).
esp_err_t sensor_init(void);

// Sample the detected sensor(s) into *out. Returns true if at least one valid field
// was measured; otherwise sets *out to invalid (valid=false, fields=NAN).
bool sensor_read(sensor_reading_t *out);

// Short human-readable summary of detected chip(s) or "none".
const char *sensor_name(void);
