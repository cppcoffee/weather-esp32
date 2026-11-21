#include "sensor.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "button.h"
#include "driver/i2c_master.h"
#include "epd.h"
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
                   && SENSOR_I2C_SCL_GPIO != BUTTON_GPIO && SENSOR_I2C_SDA_GPIO != AUDIO_I2C_SDA_GPIO
                   && SENSOR_I2C_SDA_GPIO != AUDIO_I2C_SCL_GPIO && SENSOR_I2C_SCL_GPIO != AUDIO_I2C_SDA_GPIO
                   && SENSOR_I2C_SCL_GPIO != AUDIO_I2C_SCL_GPIO,
               "Sensor I2C pins overlap button or audio pins");

#define SENSOR_I2C_PORT       I2C_NUM_1
#define SENSOR_I2C_SPEED_HZ   100000
#define SENSOR_I2C_TIMEOUT_MS 100

static const char *log_tag = "sensor";

enum baro_kind {
    BARO_NONE,
    BARO_BME280,
    BARO_BMP280,
    BARO_BME680,
    BARO_BMP3XX,
};

enum hyg_kind {
    HYG_NONE,
    HYG_AHT20,
    HYG_SHT4X,
    HYG_SHT3X,
};

struct bme280_cal {
    uint16_t t1;
    int16_t t2, t3;
    uint16_t p1;
    int16_t p2, p3, p4, p5, p6, p7, p8, p9;
    uint8_t h1, h3;
    int16_t h2, h4, h5;
    int8_t h6;
};

struct bme680_cal {
    uint16_t t1;
    int16_t t2;
    int8_t t3;
    uint16_t p1;
    int16_t p2, p4, p5, p8, p9;
    int8_t p3, p6, p7;
    uint8_t p10;
    uint16_t h1, h2;
    int8_t h3, h4, h5, h7;
    uint8_t h6;
};

struct bmp3_cal {
    double t1, t2, t3;
    double p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11;
};

static struct {
    bool initialized;
    i2c_master_bus_handle_t bus;
    enum baro_kind baro;
    i2c_master_dev_handle_t baro_dev;
    struct bme280_cal bme280;
    struct bme680_cal bme680;
    struct bmp3_cal bmp3;

    enum hyg_kind hyg;
    i2c_master_dev_handle_t hyg_dev;
    char name[48];
} state = {.name = "none"};

static uint16_t u16le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int16_t i16le(const uint8_t *p)
{
    return (int16_t)u16le(p);
}

static uint16_t u16be(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

#ifndef configTICK_RATE_HZ
#define configTICK_RATE_HZ 100
#endif

static void delay_ms(unsigned ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    if ((uint64_t)ticks * 1000ULL < (uint64_t)ms * (uint64_t)configTICK_RATE_HZ) {
        ticks++;
    }
    // Add 1 tick so a call near the end of the current tick period still waits at least ms.
    vTaskDelay(ticks + 1);
}

static uint8_t crc8_sensirion(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static bool valid_temp(float t)
{
    return isfinite(t) && t >= -40.0f && t <= 85.0f;
}

static bool valid_humidity(float h)
{
    return isfinite(h) && h >= 0.0f && h <= 100.0f;
}

static bool valid_pressure(float p)
{
    return isfinite(p) && p >= 300.0f && p <= 1100.0f;
}

static esp_err_t reg_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(dev, &reg, 1, buf, len, SENSOR_I2C_TIMEOUT_MS);
}

static esp_err_t reg_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value)
{
    uint8_t payload[2] = {reg, value};
    return i2c_master_transmit(dev, payload, sizeof(payload), SENSOR_I2C_TIMEOUT_MS);
}

static esp_err_t add_device(uint8_t addr, i2c_master_dev_handle_t *out)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = SENSOR_I2C_SPEED_HZ,
    };
    return i2c_master_bus_add_device(state.bus, &cfg, out);
}

static bool bme280_compensate(const struct bme280_cal *c, bool has_humidity, int32_t adc_t, int32_t adc_p,
                              int32_t adc_h, float *temp_c, float *hum_pct, float *pres_hpa)
{
    if (adc_t == 0x80000 || adc_p == 0x80000) {
        return false;
    }
    double var1 = ((double)adc_t / 16384.0 - (double)c->t1 / 1024.0) * (double)c->t2;
    double var2 = ((double)adc_t / 131072.0 - (double)c->t1 / 8192.0);
    var2 = var2 * var2 * (double)c->t3;
    double t_fine = var1 + var2;
    *temp_c = (float)(t_fine / 5120.0);

    double v1 = t_fine / 2.0 - 64000.0;
    double v2 = v1 * v1 * (double)c->p6 / 32768.0;
    v2 = v2 + v1 * (double)c->p5 * 2.0;
    v2 = v2 / 4.0 + (double)c->p4 * 65536.0;
    v1 = ((double)c->p3 * v1 * v1 / 524288.0 + (double)c->p2 * v1) / 524288.0;
    v1 = (1.0 + v1 / 32768.0) * (double)c->p1;
    if (v1 == 0.0) {
        *pres_hpa = NAN;
    } else {
        double p = 1048576.0 - (double)adc_p;
        p = (p - v2 / 4096.0) * 6250.0 / v1;
        v1 = (double)c->p9 * p * p / 2147483648.0;
        v2 = p * (double)c->p8 / 32768.0;
        p = p + (v1 + v2 + (double)c->p7) / 16.0;
        *pres_hpa = (float)(p / 100.0);
    }

    if (has_humidity && adc_h != 0x8000) {
        double h = t_fine - 76800.0;
        h = ((double)adc_h - ((double)c->h4 * 64.0 + (double)c->h5 / 16384.0 * h))
            * ((double)c->h2 / 65536.0
               * (1.0 + (double)c->h6 / 67108864.0 * h * (1.0 + (double)c->h3 / 67108864.0 * h)));
        h = h * (1.0 - (double)c->h1 * h / 524288.0);
        if (h > 100.0) {
            h = 100.0;
        } else if (h < 0.0) {
            h = 0.0;
        }
        *hum_pct = (float)h;
    } else {
        *hum_pct = NAN;
    }
    return true;
}

static bool bme680_compensate(const struct bme680_cal *c, int32_t adc_t, int32_t adc_p, int32_t adc_h, float *temp_c,
                              float *hum_pct, float *pres_hpa)
{
    if (adc_t == 0x80000 || adc_p == 0x80000) {
        return false;
    }
    double var1 = ((double)adc_t / 16384.0 - (double)c->t1 / 1024.0) * (double)c->t2;
    double var2 = ((double)adc_t / 131072.0 - (double)c->t1 / 8192.0);
    var2 = var2 * var2 * ((double)c->t3 * 16.0);
    double t_fine = var1 + var2;
    double t_comp = t_fine / 5120.0;
    *temp_c = (float)t_comp;

    double pv1 = t_fine / 2.0 - 64000.0;
    double pv2 = pv1 * pv1 * ((double)c->p6 / 131072.0);
    pv2 = pv2 + pv1 * (double)c->p5 * 2.0;
    pv2 = pv2 / 4.0 + (double)c->p4 * 65536.0;
    pv1 = (((double)c->p3 * pv1 * pv1) / 16384.0 + (double)c->p2 * pv1) / 524288.0;
    pv1 = (1.0 + pv1 / 32768.0) * (double)c->p1;
    if (pv1 == 0.0) {
        *pres_hpa = NAN;
    } else {
        double p = 1048576.0 - (double)adc_p;
        p = ((p - pv2 / 4096.0) * 6250.0) / pv1;
        double pv3 = ((double)c->p9 * p * p) / 2147483648.0;
        double pv4 = p * ((double)c->p8 / 32768.0);
        double pv5 = (p / 256.0) * (p / 256.0) * (p / 256.0) * ((double)c->p10 / 131072.0);
        p = p + (pv3 + pv4 + pv5 + (double)c->p7 * 128.0) / 16.0;
        *pres_hpa = (float)(p / 100.0);
    }

    if (adc_h != 0x8000) {
        double hv1 = (double)adc_h - ((double)c->h1 * 16.0 + ((double)c->h3 / 2.0) * t_comp);
        double hv2 = hv1
                     * (((double)c->h2 / 262144.0)
                        * (1.0 + ((double)c->h4 / 16384.0) * t_comp + ((double)c->h5 / 1048576.0) * t_comp * t_comp));
        double hv3 = (double)c->h6 / 16384.0;
        double hv4 = (double)c->h7 / 2097152.0;
        double h = hv2 + (hv3 + hv4 * t_comp) * hv2 * hv2;
        if (h > 100.0) {
            h = 100.0;
        } else if (h < 0.0) {
            h = 0.0;
        }
        *hum_pct = (float)h;
    } else {
        *hum_pct = NAN;
    }
    return true;
}

static bool bmp3_compensate(const struct bmp3_cal *c, uint32_t adc_t, uint32_t adc_p, float *temp_c, float *pres_hpa)
{
    double dt = (double)adc_t - c->t1;
    double t_lin = dt * c->t2 + dt * dt * c->t3;
    *temp_c = (float)t_lin;
    double t2 = t_lin * t_lin;
    double t3 = t2 * t_lin;
    double out1 = c->p5 + c->p6 * t_lin + c->p7 * t2 + c->p8 * t3;
    double out2 = (double)adc_p * (c->p1 + c->p2 * t_lin + c->p3 * t2 + c->p4 * t3);
    double out3 = (double)adc_p * (double)adc_p * (c->p9 + c->p10 * t_lin)
                  + (double)adc_p * (double)adc_p * (double)adc_p * c->p11;
    *pres_hpa = (float)((out1 + out2 + out3) / 100.0);
    return true;
}

static bool init_bme280_or_bmp280(i2c_master_dev_handle_t dev, bool is_bme280)
{
    (void)reg_write(dev, 0xE0, 0xB6);
    delay_ms(10);
    uint8_t status = 0x01;
    for (int i = 0; i < 10; i++) {
        if (reg_read(dev, 0xF3, &status, 1) == ESP_OK && (status & 0x01) == 0) {
            break;
        }
        delay_ms(10);
    }
    uint8_t cal[24] = {0};
    if (reg_read(dev, 0x88, cal, sizeof(cal)) != ESP_OK) {
        return false;
    }
    struct bme280_cal *c = &state.bme280;
    *c = (struct bme280_cal){
        .t1 = u16le(cal + 0),
        .t2 = i16le(cal + 2),
        .t3 = i16le(cal + 4),
        .p1 = u16le(cal + 6),
        .p2 = i16le(cal + 8),
        .p3 = i16le(cal + 10),
        .p4 = i16le(cal + 12),
        .p5 = i16le(cal + 14),
        .p6 = i16le(cal + 16),
        .p7 = i16le(cal + 18),
        .p8 = i16le(cal + 20),
        .p9 = i16le(cal + 22),
    };
    if (c->t1 == 0 || c->p1 == 0) {
        return false;
    }
    if (is_bme280) {
        uint8_t h1 = 0, hcal[7] = {0};
        if (reg_read(dev, 0xA1, &h1, 1) != ESP_OK || reg_read(dev, 0xE1, hcal, sizeof(hcal)) != ESP_OK) {
            return false;
        }
        c->h1 = h1;
        c->h2 = i16le(hcal + 0);
        c->h3 = hcal[2];
        c->h4 = (int16_t)(((int16_t)(int8_t)hcal[3] << 4) | (hcal[4] & 0x0F));
        c->h5 = (int16_t)(((int16_t)(int8_t)hcal[5] << 4) | ((hcal[4] >> 4) & 0x0F));
        c->h6 = (int8_t)hcal[6];
    }
    state.baro = is_bme280 ? BARO_BME280 : BARO_BMP280;
    state.baro_dev = dev;
    return true;
}

static bool init_bme680(i2c_master_dev_handle_t dev)
{
    (void)reg_write(dev, 0xE0, 0xB6);
    delay_ms(10);
    uint8_t c1[25] = {0}, c2[15] = {0};
    if (reg_read(dev, 0x89, c1, sizeof(c1)) != ESP_OK || reg_read(dev, 0xE1, c2, sizeof(c2)) != ESP_OK) {
        return false;
    }
    state.bme680 = (struct bme680_cal){
        .t1 = u16le(c2 + 8),
        .t2 = i16le(c1 + 1),
        .t3 = (int8_t)c1[3],
        .p1 = u16le(c1 + 5),
        .p2 = i16le(c1 + 7),
        .p3 = (int8_t)c1[9],
        .p4 = i16le(c1 + 11),
        .p5 = i16le(c1 + 13),
        .p7 = (int8_t)c1[15],
        .p6 = (int8_t)c1[16],
        .p8 = i16le(c1 + 19),
        .p9 = i16le(c1 + 21),
        .p10 = c1[23],
        .h2 = (uint16_t)(((uint16_t)c2[0] << 4) | ((c2[1] >> 4) & 0x0F)),
        .h1 = (uint16_t)(((uint16_t)c2[2] << 4) | (c2[1] & 0x0F)),
        .h3 = (int8_t)c2[3],
        .h4 = (int8_t)c2[4],
        .h5 = (int8_t)c2[5],
        .h6 = c2[6],
        .h7 = (int8_t)c2[7],
    };
    state.baro = BARO_BME680;
    state.baro_dev = dev;
    return true;
}

static bool init_bmp3xx(i2c_master_dev_handle_t dev)
{
    (void)reg_write(dev, 0x7E, 0xB6);
    delay_ms(10);
    uint8_t c[21] = {0};
    if (reg_read(dev, 0x31, c, sizeof(c)) != ESP_OK) {
        return false;
    }
    state.bmp3 = (struct bmp3_cal){
        .t1 = (double)u16le(c + 0) * 256.0,
        .t2 = (double)u16le(c + 2) / 1073741824.0,
        .t3 = (double)(int8_t)c[4] / 281474976710656.0,
        .p1 = ((double)i16le(c + 5) - 16384.0) / 1048576.0,
        .p2 = ((double)i16le(c + 7) - 16384.0) / 536870912.0,
        .p3 = (double)(int8_t)c[9] / 4294967296.0,
        .p4 = (double)(int8_t)c[10] / 137438953472.0,
        .p5 = (double)u16le(c + 11) * 8.0,
        .p6 = (double)u16le(c + 13) / 64.0,
        .p7 = (double)(int8_t)c[15] / 256.0,
        .p8 = (double)(int8_t)c[16] / 32768.0,
        .p9 = (double)i16le(c + 17) / 281474976710656.0,
        .p10 = (double)(int8_t)c[19] / 281474976710656.0,
        .p11 = (double)(int8_t)c[20] / 36893488147419103232.0,
    };
    state.baro = BARO_BMP3XX;
    state.baro_dev = dev;
    return true;
}

static bool probe_baro(uint8_t addr)
{
    if (i2c_master_probe(state.bus, addr, 50) != ESP_OK) {
        return false;
    }
    i2c_master_dev_handle_t dev = NULL;
    if (add_device(addr, &dev) != ESP_OK) {
        return false;
    }
    uint8_t id = 0;
    if (reg_read(dev, 0xD0, &id, 1) == ESP_OK) {
        if (id == 0x60 && init_bme280_or_bmp280(dev, true)) {
            ESP_LOGI(log_tag, "detected BME280 at 0x%02X", addr);
            return true;
        }
        if ((id == 0x58 || id == 0x56 || id == 0x57) && init_bme280_or_bmp280(dev, false)) {
            ESP_LOGI(log_tag, "detected BMP280 (id=0x%02X) at 0x%02X", id, addr);
            return true;
        }
        if (id == 0x61 && init_bme680(dev)) {
            ESP_LOGI(log_tag, "detected BME680/688 at 0x%02X", addr);
            return true;
        }
    }
    if (reg_read(dev, 0x00, &id, 1) == ESP_OK && (id == 0x50 || id == 0x60) && init_bmp3xx(dev)) {
        ESP_LOGI(log_tag, "detected BMP388/390 (id=0x%02X) at 0x%02X", id, addr);
        return true;
    }
    i2c_master_bus_rm_device(dev);
    return false;
}

static bool probe_aht20(void)
{
    if (i2c_master_probe(state.bus, 0x38, 50) != ESP_OK) {
        return false;
    }
    i2c_master_dev_handle_t dev = NULL;
    if (add_device(0x38, &dev) != ESP_OK) {
        return false;
    }
    uint8_t status = 0;
    if (reg_read(dev, 0x71, &status, 1) != ESP_OK) {
        i2c_master_bus_rm_device(dev);
        return false;
    }
    if ((status & 0x08) == 0) {
        const uint8_t init_cmd[3] = {0xBE, 0x08, 0x00};
        if (i2c_master_transmit(dev, init_cmd, sizeof(init_cmd), SENSOR_I2C_TIMEOUT_MS) != ESP_OK) {
            i2c_master_bus_rm_device(dev);
            return false;
        }
        delay_ms(10);
    }
    state.hyg = HYG_AHT20;
    state.hyg_dev = dev;
    ESP_LOGI(log_tag, "detected AHT20/21 at 0x38");
    return true;
}

static bool read_sht4x(i2c_master_dev_handle_t dev, float *temp_c, float *hum_pct)
{
    const uint8_t cmd = 0xFD;
    uint8_t raw[6] = {0};
    if (i2c_master_transmit(dev, &cmd, 1, SENSOR_I2C_TIMEOUT_MS) != ESP_OK) {
        return false;
    }
    delay_ms(10);
    if (i2c_master_receive(dev, raw, sizeof(raw), SENSOR_I2C_TIMEOUT_MS) != ESP_OK) {
        return false;
    }
    if (crc8_sensirion(raw, 2) != raw[2] || crc8_sensirion(raw + 3, 2) != raw[5]) {
        return false;
    }
    double t = -45.0 + 175.0 * (double)u16be(raw) / 65535.0;
    double h = -6.0 + 125.0 * (double)u16be(raw + 3) / 65535.0;
    if (h < 0.0) {
        h = 0.0;
    } else if (h > 100.0) {
        h = 100.0;
    }
    *temp_c = (float)t;
    *hum_pct = (float)h;
    return true;
}

static bool read_sht3x(i2c_master_dev_handle_t dev, float *temp_c, float *hum_pct)
{
    const uint8_t cmd[2] = {0x24, 0x00};
    uint8_t raw[6] = {0};
    if (i2c_master_transmit(dev, cmd, sizeof(cmd), SENSOR_I2C_TIMEOUT_MS) != ESP_OK) {
        return false;
    }
    delay_ms(16);
    if (i2c_master_receive(dev, raw, sizeof(raw), SENSOR_I2C_TIMEOUT_MS) != ESP_OK) {
        return false;
    }
    if (crc8_sensirion(raw, 2) != raw[2] || crc8_sensirion(raw + 3, 2) != raw[5]) {
        return false;
    }
    *temp_c = (float)(-45.0 + 175.0 * (double)u16be(raw) / 65535.0);
    *hum_pct = (float)(100.0 * (double)u16be(raw + 3) / 65535.0);
    return true;
}

static bool probe_sht(uint8_t addr)
{
    if (i2c_master_probe(state.bus, addr, 50) != ESP_OK) {
        return false;
    }
    i2c_master_dev_handle_t dev = NULL;
    if (add_device(addr, &dev) != ESP_OK) {
        return false;
    }
    float t = NAN, h = NAN;
    if (read_sht4x(dev, &t, &h)) {
        state.hyg = HYG_SHT4X;
        state.hyg_dev = dev;
        ESP_LOGI(log_tag, "detected SHT4x at 0x%02X", addr);
        return true;
    }
    if (read_sht3x(dev, &t, &h)) {
        state.hyg = HYG_SHT3X;
        state.hyg_dev = dev;
        ESP_LOGI(log_tag, "detected SHT3x at 0x%02X", addr);
        return true;
    }
    i2c_master_bus_rm_device(dev);
    return false;
}

static bool read_baro(float *temp_c, float *hum_pct, float *pres_hpa)
{
    *temp_c = NAN;
    *hum_pct = NAN;
    *pres_hpa = NAN;
    if (state.baro == BARO_BME280 || state.baro == BARO_BMP280) {
        bool is_bme = state.baro == BARO_BME280;
        if ((is_bme && reg_write(state.baro_dev, 0xF2, 0x01) != ESP_OK)
            || reg_write(state.baro_dev, 0xF5, 0x00) != ESP_OK || reg_write(state.baro_dev, 0xF4, 0x25) != ESP_OK)
        {
            return false;
        }
        delay_ms(15);
        uint8_t status = 0x08;
        for (int i = 0; i < 10; i++) {
            if (reg_read(state.baro_dev, 0xF3, &status, 1) == ESP_OK && (status & 0x09) == 0) {
                break;
            }
            delay_ms(10);
        }
        uint8_t raw[8] = {0};
        if (reg_read(state.baro_dev, 0xF7, raw, is_bme ? 8 : 6) != ESP_OK) {
            return false;
        }
        int32_t adc_p = ((int32_t)raw[0] << 12) | ((int32_t)raw[1] << 4) | ((int32_t)raw[2] >> 4);
        int32_t adc_t = ((int32_t)raw[3] << 12) | ((int32_t)raw[4] << 4) | ((int32_t)raw[5] >> 4);
        int32_t adc_h = is_bme ? (((int32_t)raw[6] << 8) | (int32_t)raw[7]) : 0x8000;
        return bme280_compensate(&state.bme280, is_bme, adc_t, adc_p, adc_h, temp_c, hum_pct, pres_hpa);
    }
    if (state.baro == BARO_BME680) {
        if (reg_write(state.baro_dev, 0x71, 0x00) != ESP_OK || reg_write(state.baro_dev, 0x72, 0x01) != ESP_OK
            || reg_write(state.baro_dev, 0x75, 0x00) != ESP_OK || reg_write(state.baro_dev, 0x74, 0x25) != ESP_OK)
        {
            return false;
        }
        delay_ms(15);
        uint8_t status = 0;
        for (int i = 0; i < 10; i++) {
            if (reg_read(state.baro_dev, 0x1D, &status, 1) == ESP_OK && (status & 0x80) != 0 && (status & 0x20) == 0) {
                break;
            }
            delay_ms(10);
        }
        uint8_t raw[8] = {0};
        if (reg_read(state.baro_dev, 0x1F, raw, sizeof(raw)) != ESP_OK) {
            return false;
        }
        int32_t adc_p = ((int32_t)raw[0] << 12) | ((int32_t)raw[1] << 4) | ((int32_t)raw[2] >> 4);
        int32_t adc_t = ((int32_t)raw[3] << 12) | ((int32_t)raw[4] << 4) | ((int32_t)raw[5] >> 4);
        int32_t adc_h = ((int32_t)raw[6] << 8) | (int32_t)raw[7];
        return bme680_compensate(&state.bme680, adc_t, adc_p, adc_h, temp_c, hum_pct, pres_hpa);
    }
    if (state.baro == BARO_BMP3XX) {
        if (reg_write(state.baro_dev, 0x1C, 0x00) != ESP_OK || reg_write(state.baro_dev, 0x1B, 0x13) != ESP_OK) {
            return false;
        }
        delay_ms(15);
        uint8_t status = 0;
        for (int i = 0; i < 10; i++) {
            if (reg_read(state.baro_dev, 0x03, &status, 1) == ESP_OK && (status & 0x60) == 0x60) {
                break;
            }
            delay_ms(10);
        }
        uint8_t raw[6] = {0};
        if (reg_read(state.baro_dev, 0x04, raw, sizeof(raw)) != ESP_OK) {
            return false;
        }
        uint32_t adc_p = (uint32_t)raw[0] | ((uint32_t)raw[1] << 8) | ((uint32_t)raw[2] << 16);
        uint32_t adc_t = (uint32_t)raw[3] | ((uint32_t)raw[4] << 8) | ((uint32_t)raw[5] << 16);
        return bmp3_compensate(&state.bmp3, adc_t, adc_p, temp_c, pres_hpa);
    }
    return false;
}

static bool read_aht20(float *temp_c, float *hum_pct)
{
    const uint8_t trigger[3] = {0xAC, 0x33, 0x00};
    if (i2c_master_transmit(state.hyg_dev, trigger, sizeof(trigger), SENSOR_I2C_TIMEOUT_MS) != ESP_OK) {
        return false;
    }
    delay_ms(80);
    uint8_t raw[7] = {0};
    for (int attempt = 0; attempt < 5; attempt++) {
        if (i2c_master_receive(state.hyg_dev, raw, sizeof(raw), SENSOR_I2C_TIMEOUT_MS) != ESP_OK) {
            return false;
        }
        if ((raw[0] & 0x80) == 0) {
            break;
        }
        delay_ms(10);
    }
    if ((raw[0] & 0x80) != 0 || crc8_sensirion(raw, 6) != raw[6]) {
        return false;
    }
    uint32_t raw_h = ((uint32_t)raw[1] << 12) | ((uint32_t)raw[2] << 4) | ((uint32_t)raw[3] >> 4);
    uint32_t raw_t = (((uint32_t)raw[3] & 0x0F) << 16) | ((uint32_t)raw[4] << 8) | (uint32_t)raw[5];
    *hum_pct = (float)((double)raw_h * 100.0 / 1048576.0);
    *temp_c = (float)((double)raw_t * 200.0 / 1048576.0 - 50.0);
    return true;
}

static bool read_hyg(float *temp_c, float *hum_pct)
{
    *temp_c = NAN;
    *hum_pct = NAN;
    if (state.hyg == HYG_AHT20) {
        return read_aht20(temp_c, hum_pct);
    }
    if (state.hyg == HYG_SHT4X) {
        return read_sht4x(state.hyg_dev, temp_c, hum_pct);
    }
    if (state.hyg == HYG_SHT3X) {
        return read_sht3x(state.hyg_dev, temp_c, hum_pct);
    }
    return false;
}

static void format_sensor_name(uint8_t first_unknown)
{
    static const char *const baro_names[] = {
        [BARO_NONE] = "",         [BARO_BME280] = "BME280", [BARO_BMP280] = "BMP280",
        [BARO_BME680] = "BME680", [BARO_BMP3XX] = "BMP3xx",
    };
    static const char *const hyg_names[] = {
        [HYG_NONE] = "",
        [HYG_AHT20] = "AHT20",
        [HYG_SHT4X] = "SHT4x",
        [HYG_SHT3X] = "SHT3x",
    };
    if (state.hyg != HYG_NONE && state.baro != BARO_NONE) {
        snprintf(state.name, sizeof(state.name), "%s+%s", hyg_names[state.hyg], baro_names[state.baro]);
    } else if (state.baro != BARO_NONE) {
        snprintf(state.name, sizeof(state.name), "%s", baro_names[state.baro]);
    } else if (state.hyg != HYG_NONE) {
        snprintf(state.name, sizeof(state.name), "%s", hyg_names[state.hyg]);
    } else if (first_unknown != 0) {
        snprintf(state.name, sizeof(state.name), "unknown@0x%02X", first_unknown);
    } else {
        snprintf(state.name, sizeof(state.name), "none");
    }
}

esp_err_t sensor_init(void)
{
    if (state.initialized) {
        return (state.baro != BARO_NONE || state.hyg != HYG_NONE) ? ESP_OK : ESP_ERR_NOT_FOUND;
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

    uint8_t first_found = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_master_probe(state.bus, addr, 20) == ESP_OK) {
            ESP_LOGI(log_tag, "I2C1 scan found device at 0x%02X", addr);
            if (first_found == 0) {
                first_found = addr;
            }
        }
    }

    // Probe Bosch barometers/all-in-ones first (0x76, 0x77), then dedicated hygrometers.
    if (!probe_baro(0x76)) {
        (void)probe_baro(0x77);
    }
    if (!probe_aht20()) {
        if (!probe_sht(0x44)) {
            (void)probe_sht(0x45);
        }
    }

    format_sensor_name(first_found);
    if (state.baro == BARO_NONE && state.hyg == HYG_NONE) {
        ESP_LOGW(log_tag, "no supported environmental sensor found on SDA=%d SCL=%d (%s)", SENSOR_I2C_SDA_GPIO,
                 SENSOR_I2C_SCL_GPIO, state.name);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(log_tag, "environmental sensor ready: %s (SDA=%d SCL=%d)", state.name, SENSOR_I2C_SDA_GPIO,
             SENSOR_I2C_SCL_GPIO);
    return ESP_OK;
}

bool sensor_read(sensor_reading_t *out)
{
    if (out == NULL) {
        return false;
    }
    *out = (sensor_reading_t){.temperature_c = NAN, .humidity_percent = NAN, .pressure_hpa = NAN, .valid = false};
    if (!state.initialized || (state.baro == BARO_NONE && state.hyg == HYG_NONE)) {
        return false;
    }

    float baro_t = NAN, baro_h = NAN, baro_p = NAN;
    bool baro_ok = state.baro != BARO_NONE && read_baro(&baro_t, &baro_h, &baro_p);
    float hyg_t = NAN, hyg_h = NAN;
    bool hyg_ok = state.hyg != HYG_NONE && read_hyg(&hyg_t, &hyg_h);

    // Prefer dedicated hygrometer for temperature/humidity on combo modules (e.g. AHT20+BMP280).
    float temp = hyg_ok && valid_temp(hyg_t) ? hyg_t : (baro_ok && valid_temp(baro_t) ? baro_t : NAN);
    float hum = hyg_ok && valid_humidity(hyg_h) ? hyg_h : (baro_ok && valid_humidity(baro_h) ? baro_h : NAN);
    float pres = baro_ok && valid_pressure(baro_p) ? baro_p : NAN;

    bool any = isfinite(temp) || isfinite(hum) || isfinite(pres);
    if (!any) {
        return false;
    }
    out->temperature_c = temp;
    out->humidity_percent = hum;
    out->pressure_hpa = pres;
    out->valid = true;
    return true;
}

const char *sensor_name(void)
{
    return state.name;
}
