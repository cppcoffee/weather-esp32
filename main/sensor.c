#include "sensor.h"

#include <math.h>
#include <stdio.h>

#include "ahtxx.h"
#include "audio.h"
#include "bmp280.h"
#include "button.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_SPIRAM_MODE_OCT
#define SENSOR_PIN_OVERLAPS_PSRAM(pin) ((pin) >= 33 && (pin) <= 37)
_Static_assert(!SENSOR_PIN_OVERLAPS_PSRAM(SENSOR_I2C_SDA_GPIO) && !SENSOR_PIN_OVERLAPS_PSRAM(SENSOR_I2C_SCL_GPIO),
               "Sensor I2C wiring overlaps GPIO33-37 reserved for octal PSRAM");
#endif
_Static_assert(SENSOR_I2C_SDA_GPIO != SENSOR_I2C_SCL_GPIO && SENSOR_I2C_SDA_GPIO != BUTTON_GPIO
                   && SENSOR_I2C_SCL_GPIO != BUTTON_GPIO && SENSOR_I2C_SDA_GPIO != VOICE_BUTTON_GPIO
                   && SENSOR_I2C_SCL_GPIO != VOICE_BUTTON_GPIO && VOICE_BUTTON_GPIO != BUTTON_GPIO
                   && SENSOR_I2C_SDA_GPIO != AUDIO_I2C_SDA_GPIO && SENSOR_I2C_SDA_GPIO != AUDIO_I2C_SCL_GPIO
                   && SENSOR_I2C_SCL_GPIO != AUDIO_I2C_SDA_GPIO && SENSOR_I2C_SCL_GPIO != AUDIO_I2C_SCL_GPIO,
               "Sensor I2C pins overlap button or audio pins");

#define SENSOR_I2C_PORT           I2C_NUM_1
#define SENSOR_BMP280_FORCED_WAIT 20

static const char *log_tag = "sensor";

static struct {
    bool                    initialized;
    i2c_master_bus_handle_t bus;
    ahtxx_handle_t          aht20;
    bmp280_handle_t         bmp280;
    char                    name[24];
} state = {.name = "none"};

static bool
valid_temp(float t)
{
    return isfinite(t) && t >= -40.0f && t <= 85.0f;
}

static bool
valid_humidity(float h)
{
    return isfinite(h) && h >= 0.0f && h <= 100.0f;
}

static bool
valid_pressure(float p)
{
    return isfinite(p) && p >= 300.0f && p <= 1100.0f;
}

esp_err_t
sensor_init(void)
{
    if (state.initialized) {
        return (state.aht20 || state.bmp280) ? ESP_OK : ESP_ERR_NOT_FOUND;
    }
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = SENSOR_I2C_PORT,
        .sda_io_num = SENSOR_I2C_SDA_GPIO,
        .scl_io_num = SENSOR_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &state.bus);
    if (err != ESP_OK) {
        ESP_LOGW(log_tag, "I2C1 init failed: %s", esp_err_to_name(err));
        return err;
    }
    state.initialized = true;

    ahtxx_config_t aht_cfg = I2C_AHT20_CONFIG_DEFAULT;
    aht_cfg.i2c_address = SENSOR_AHT20_ADDR;
    if (ahtxx_init(state.bus, &aht_cfg, &state.aht20) != ESP_OK) {
        state.aht20 = NULL;
    }

    bmp280_config_t bmp_cfg = I2C_BMP280_CONFIG_DEFAULT;
    bmp_cfg.i2c_address = SENSOR_BMP280_ADDR;
    bmp_cfg.power_mode = BMP280_POWER_MODE_FORCED;
    bmp_cfg.temperature_oversampling = BMP280_TEMPERATURE_OVERSAMPLING_1X;
    bmp_cfg.pressure_oversampling = BMP280_PRESSURE_OVERSAMPLING_1X;
    if (bmp280_init(state.bus, &bmp_cfg, &state.bmp280) != ESP_OK) {
        state.bmp280 = NULL;
    }

    if (state.aht20 && state.bmp280) {
        snprintf(state.name, sizeof(state.name), "AHT20+BMP280");
    } else if (state.aht20) {
        snprintf(state.name, sizeof(state.name), "AHT20");
    } else if (state.bmp280) {
        snprintf(state.name, sizeof(state.name), "BMP280");
    } else {
        snprintf(state.name, sizeof(state.name), "none");
        ESP_LOGW(log_tag, "no sensor found at AHT20=0x%02X BMP280=0x%02X (SDA=%d SCL=%d)", SENSOR_AHT20_ADDR,
                 SENSOR_BMP280_ADDR, SENSOR_I2C_SDA_GPIO, SENSOR_I2C_SCL_GPIO);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(log_tag, "environmental sensor ready: %s (SDA=%d SCL=%d)", state.name, SENSOR_I2C_SDA_GPIO,
             SENSOR_I2C_SCL_GPIO);
    return ESP_OK;
}

bool
sensor_read(sensor_reading_t *out)
{
    if (out == NULL) {
        return false;
    }
    *out = (sensor_reading_t){.temperature_c = NAN, .humidity_percent = NAN, .pressure_hpa = NAN, .valid = false};
    if (!state.initialized || (!state.aht20 && !state.bmp280)) {
        return false;
    }

    float aht_t = NAN, aht_h = NAN;
    bool  aht_ok = state.aht20 && ahtxx_get_measurement(state.aht20, &aht_t, &aht_h) == ESP_OK;

    float bmp_t = NAN, bmp_p_pa = NAN;
    bool  bmp_ok = false;
    if (state.bmp280 && bmp280_set_power_mode(state.bmp280, BMP280_POWER_MODE_FORCED) == ESP_OK) {
        // Wait for 1x forced conversion (~6.4 ms max) before polling
        // status/data registers.
        vTaskDelay(pdMS_TO_TICKS(SENSOR_BMP280_FORCED_WAIT));
        bmp_ok = bmp280_get_measurements(state.bmp280, &bmp_t, &bmp_p_pa) == ESP_OK;
    }
    float bmp_p_hpa = bmp_p_pa * 0.01f;

    // Prefer AHT20 for temperature/humidity on the combo module.
    float temp = aht_ok && valid_temp(aht_t) ? aht_t : (bmp_ok && valid_temp(bmp_t) ? bmp_t : NAN);
    float hum = aht_ok && valid_humidity(aht_h) ? aht_h : NAN;
    float pres = bmp_ok && valid_pressure(bmp_p_hpa) ? bmp_p_hpa : NAN;

    if (!isfinite(temp) && !isfinite(hum) && !isfinite(pres)) {
        return false;
    }
    out->temperature_c = temp;
    out->humidity_percent = hum;
    out->pressure_hpa = pres;
    out->valid = true;
    return true;
}

const char *
sensor_name(void)
{
    return state.name;
}
