#!/usr/bin/env python3
"""Host regression for main/sensor.c using an in-memory I2C master stub."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


HEADERS = {
    "sdkconfig.h": "#pragma once\n#define CONFIG_SPIRAM_MODE_OCT 1\n",
    "esp_err.h": """#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_TIMEOUT 0x107
static inline const char *esp_err_to_name(esp_err_t e) { (void)e; return "err"; }
""",
    "esp_log.h": """#pragma once
static inline void test_log(const char *fmt, ...) { (void)fmt; }
#define ESP_LOGI(tag, ...) ((void)(tag), test_log(__VA_ARGS__))
#define ESP_LOGW(tag, ...) ((void)(tag), test_log(__VA_ARGS__))
#define ESP_LOGE(tag, ...) ((void)(tag), test_log(__VA_ARGS__))
""",
    "freertos/FreeRTOS.h": """#pragma once
#include <stdint.h>
typedef unsigned TickType_t;
#define configTICK_RATE_HZ 100
#define pdMS_TO_TICKS(ms) ((TickType_t)(((uint64_t)(ms) * configTICK_RATE_HZ) / 1000U))
""",
    "freertos/task.h": """#pragma once
#include "freertos/FreeRTOS.h"
void vTaskDelay(TickType_t ticks);
""",
    "driver/i2c_master.h": """#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define I2C_NUM_1 1
#define I2C_CLK_SRC_DEFAULT 0
#define I2C_ADDR_BIT_LEN_7 0

typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;

typedef struct {
    int i2c_port;
    int sda_io_num;
    int scl_io_num;
    int clk_source;
    int glitch_ignore_cnt;
    struct {
        bool enable_internal_pullup;
    } flags;
} i2c_master_bus_config_t;

typedef struct {
    int dev_addr_length;
    uint16_t device_address;
    uint32_t scl_speed_hz;
} i2c_device_config_t;

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *cfg, i2c_master_bus_handle_t *ret_bus);
esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t addr, int timeout_ms);
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *cfg, i2c_master_dev_handle_t *ret_dev);
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t dev);
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *tx, size_t tx_len, int timeout_ms);
esp_err_t i2c_master_receive(i2c_master_dev_handle_t dev, uint8_t *rx, size_t rx_len, int timeout_ms);
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_len, int timeout_ms);
""",
}

HARNESS = r'''
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/i2c_master.h"
#include "freertos/task.h"
#include "sensor.h"

struct fake_dev {
    uint8_t addr;
    bool present;
    uint8_t regs[256];
    uint8_t latched_raw[8];
    uint8_t rx_buf[16];
    size_t rx_len;
    bool corrupt_crc;
    bool is_sht4x;
    bool is_bme280_family;
    unsigned reset_ready_ms;
    unsigned measure_ready_ms;
};

static struct fake_dev devs[4];
static size_t dev_count;
static uint8_t gas_ctrl_written = 0xFF;
static unsigned sim_time_ms;

void vTaskDelay(TickType_t ticks)
{
    /* At 100 Hz FreeRTOS tick rate, a 0-tick delay means <10 ms was truncated to 0 ms. */
    assert(ticks > 0);
    sim_time_ms += ticks * (1000U / configTICK_RATE_HZ);
}

static uint8_t crc8(const uint8_t *d, size_t n)
{
    uint8_t c = 0xFF;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int b = 0; b < 8; b++) c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x31) : (uint8_t)(c << 1);
    }
    return c;
}

static struct fake_dev *find_dev(uint16_t addr)
{
    for (size_t i = 0; i < dev_count; i++) {
        if (devs[i].present && devs[i].addr == addr) return &devs[i];
    }
    return NULL;
}

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *cfg, i2c_master_bus_handle_t *ret_bus)
{
    assert(cfg->i2c_port == I2C_NUM_1 && cfg->sda_io_num == 5 && cfg->scl_io_num == 6);
    assert(cfg->flags.enable_internal_pullup);
    *ret_bus = (void *)1;
    return ESP_OK;
}

esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t addr, int timeout_ms)
{
    assert(bus == (void *)1 && timeout_ms > 0);
    return find_dev(addr) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *cfg, i2c_master_dev_handle_t *ret_dev)
{
    assert(bus == (void *)1 && cfg->scl_speed_hz == 100000);
    struct fake_dev *d = find_dev(cfg->device_address);
    assert(d != NULL);
    *ret_dev = d;
    return ESP_OK;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t dev)
{
    assert(dev != NULL);
    return ESP_OK;
}

static void put_le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)(v >> 8); }
static void put_be20(uint8_t *p, int32_t v) { p[0] = (uint8_t)(v >> 12); p[1] = (uint8_t)(v >> 4); p[2] = (uint8_t)((v & 0x0F) << 4); }

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t handle, const uint8_t *tx, size_t tx_len, int timeout_ms)
{
    struct fake_dev *d = handle;
    assert(d && tx && tx_len > 0 && timeout_ms > 0);
    if (d->addr == 0x44 && tx_len == 1 && tx[0] == 0xFD && !d->is_sht4x) return ESP_FAIL;
    if (tx_len == 2) {
        d->regs[tx[0]] = tx[1];
        if (tx[0] == 0x71) gas_ctrl_written = tx[1];
        if (d->is_bme280_family && tx[0] == 0xE0 && tx[1] == 0xB6) {
            d->reset_ready_ms = sim_time_ms + 10;
            put_be20(&d->regs[0xF7], 0x80000);
            put_be20(&d->regs[0xFA], 0x80000);
            d->regs[0xFD] = 0x80;
            d->regs[0xFE] = 0x00;
        }
        if (d->is_bme280_family && tx[0] == 0xF4 && (tx[1] & 0x03) != 0) {
            d->measure_ready_ms = sim_time_ms + 10;
        }
    }
    return ESP_OK;
}

esp_err_t i2c_master_receive(i2c_master_dev_handle_t handle, uint8_t *rx, size_t rx_len, int timeout_ms)
{
    struct fake_dev *d = handle;
    assert(d && rx && rx_len <= d->rx_len && timeout_ms > 0);
    memcpy(rx, d->rx_buf, rx_len);
    if (d->corrupt_crc) rx[rx_len - 1] ^= 0xFF;
    return ESP_OK;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t handle, const uint8_t *tx, size_t tx_len,
                                      uint8_t *rx, size_t rx_len, int timeout_ms)
{
    struct fake_dev *d = handle;
    assert(d && tx && tx_len == 1 && rx && timeout_ms > 0);
    uint8_t reg = tx[0];
    assert((size_t)reg + rx_len <= sizeof(d->regs));
    if (d->is_bme280_family) {
        if (reg == 0x88 && sim_time_ms < d->reset_ready_ms) return ESP_FAIL;
        if (reg == 0xF3) {
            d->regs[0xF3] = (uint8_t)((sim_time_ms < d->reset_ready_ms ? 0x01 : 0x00)
                                      | (sim_time_ms < d->measure_ready_ms ? 0x08 : 0x00));
        }
        if (reg == 0xF7 && sim_time_ms >= d->measure_ready_ms) {
            memcpy(&d->regs[0xF7], d->latched_raw, sizeof(d->latched_raw));
        }
    }
    memcpy(rx, &d->regs[reg], rx_len);
    return ESP_OK;
}

static void setup_bme280(struct fake_dev *d, uint8_t addr, uint8_t chip_id)
{
    memset(d, 0, sizeof(*d));
    d->addr = addr;
    d->present = true;
    d->is_bme280_family = true;
    d->regs[0xD0] = chip_id;
    /* Official Bosch BME280 datasheet §4.2.3 calibration test vector. */
    put_le16(&d->regs[0x88], 27504);
    put_le16(&d->regs[0x8A], (uint16_t)26435);
    put_le16(&d->regs[0x8C], (uint16_t)(int16_t)-1000);
    put_le16(&d->regs[0x8E], 36477);
    put_le16(&d->regs[0x90], (uint16_t)(int16_t)-10685);
    put_le16(&d->regs[0x92], 3024);
    put_le16(&d->regs[0x94], 2855);
    put_le16(&d->regs[0x96], 140);
    put_le16(&d->regs[0x98], (uint16_t)(int16_t)-7);
    put_le16(&d->regs[0x9A], 15500);
    put_le16(&d->regs[0x9C], (uint16_t)(int16_t)-14600);
    put_le16(&d->regs[0x9E], 6000);
    d->regs[0xA1] = 75;
    put_le16(&d->regs[0xE1], 360);
    d->regs[0xE3] = 0;
    /* dig_H4 = 320 (0x140 -> E4=0x14, E5[3:0]=0), dig_H5 = 50 (0x032 -> E6=0x03, E5[7:4]=2) */
    d->regs[0xE4] = 0x14;
    d->regs[0xE5] = 0x20;
    d->regs[0xE6] = 0x03;
    d->regs[0xE7] = 30;
    put_be20(&d->latched_raw[0], 415148);
    put_be20(&d->latched_raw[3], 519888);
    d->latched_raw[6] = 0x6D; /* adc_H = 28000 -> ~41% RH */
    d->latched_raw[7] = 0x60;
    put_be20(&d->regs[0xF7], 0x80000);
    put_be20(&d->regs[0xFA], 0x80000);
    d->regs[0xFD] = 0x80;
    d->regs[0xFE] = 0x00;
}

static void setup_aht20(struct fake_dev *d, double temp_c, double hum_pct)
{
    memset(d, 0, sizeof(*d));
    d->addr = 0x38;
    d->present = true;
    d->regs[0x71] = 0x18; /* calibrated */
    uint32_t rh = (uint32_t)llround(hum_pct * 1048576.0 / 100.0);
    uint32_t rt = (uint32_t)llround((temp_c + 50.0) * 1048576.0 / 200.0);
    d->rx_buf[0] = 0x18;
    d->rx_buf[1] = (uint8_t)(rh >> 12);
    d->rx_buf[2] = (uint8_t)(rh >> 4);
    d->rx_buf[3] = (uint8_t)(((rh & 0x0F) << 4) | ((rt >> 16) & 0x0F));
    d->rx_buf[4] = (uint8_t)(rt >> 8);
    d->rx_buf[5] = (uint8_t)(rt & 0xFF);
    d->rx_buf[6] = crc8(d->rx_buf, 6);
    d->rx_len = 7;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    sensor_reading_t r = {0};
    if (!strcmp(argv[1], "bme280")) {
        setup_bme280(&devs[0], 0x76, 0x60);
        dev_count = 1;
        assert(sensor_init() == ESP_OK);
        assert(!strcmp(sensor_name(), "BME280"));
        assert(sensor_read(&r) && r.valid);
        assert(fabsf(r.temperature_c - 25.08f) < 0.05f);
        assert(fabsf(r.pressure_hpa - 1006.53f) < 0.1f);
        assert(r.humidity_percent > 35.0f && r.humidity_percent < 50.0f);
        puts("PASS BME280 (Bosch datasheet reference vector)");
    } else if (!strcmp(argv[1], "aht20_bmp280")) {
        setup_bme280(&devs[0], 0x77, 0x58);
        setup_aht20(&devs[1], 22.5, 58.0);
        dev_count = 2;
        assert(sensor_init() == ESP_OK);
        assert(!strcmp(sensor_name(), "AHT20+BMP280"));
        assert(sensor_read(&r) && r.valid);
        /* Combo prefers AHT20 for T/H and BMP280 for pressure. */
        assert(fabsf(r.temperature_c - 22.5f) < 0.05f);
        assert(fabsf(r.humidity_percent - 58.0f) < 0.1f);
        assert(fabsf(r.pressure_hpa - 1006.53f) < 0.1f);
        /* Corrupted AHT20 CRC falls back cleanly to BMP280 T/P without bogus humidity. */
        devs[1].corrupt_crc = true;
        assert(sensor_read(&r) && r.valid);
        assert(fabsf(r.temperature_c - 25.08f) < 0.05f);
        assert(isnan(r.humidity_percent));
        assert(fabsf(r.pressure_hpa - 1006.53f) < 0.1f);
        puts("PASS AHT20+BMP280 combo & CRC fallback");
    } else if (!strcmp(argv[1], "bme680")) {
        memset(&devs[0], 0, sizeof(devs[0]));
        devs[0].addr = 0x76;
        devs[0].present = true;
        devs[0].regs[0xD0] = 0x61;
        devs[0].regs[0x1D] = 0x80; /* new_data=1, measuring=0 */
        put_le16(&devs[0].regs[0xE9], 26000);
        put_le16(&devs[0].regs[0x8A], 26500);
        devs[0].regs[0x8C] = 3;
        put_le16(&devs[0].regs[0x8E], 36000);
        put_le16(&devs[0].regs[0x90], (uint16_t)(int16_t)-10500);
        devs[0].regs[0x92] = 88;
        put_le16(&devs[0].regs[0x94], 7000);
        put_le16(&devs[0].regs[0x96], (uint16_t)(int16_t)-100);
        devs[0].regs[0x98] = 30;
        devs[0].regs[0x99] = 30;
        put_le16(&devs[0].regs[0x9C], (uint16_t)(int16_t)-2500);
        put_le16(&devs[0].regs[0x9E], (uint16_t)(int16_t)-1000);
        devs[0].regs[0xA0] = 30;
        /* par_h1 = 700 (0x2BC), par_h2 = 1000 (0x3E8) */
        devs[0].regs[0xE1] = 0x3E;
        devs[0].regs[0xE2] = 0x8C;
        devs[0].regs[0xE3] = 0x2B;
        devs[0].regs[0xE4] = 0;
        devs[0].regs[0xE5] = 45;
        devs[0].regs[0xE6] = 20;
        devs[0].regs[0xE7] = 100;
        devs[0].regs[0xE8] = (uint8_t)(int8_t)-100;
        put_be20(&devs[0].regs[0x1F], 390000);
        put_be20(&devs[0].regs[0x22], 500000);
        devs[0].regs[0x25] = 0x65;
        devs[0].regs[0x26] = 0x00;
        dev_count = 1;
        assert(sensor_init() == ESP_OK);
        assert(!strcmp(sensor_name(), "BME680"));
        assert(sensor_read(&r) && r.valid && gas_ctrl_written == 0x00);
        assert(r.temperature_c > 10.0f && r.temperature_c < 40.0f);
        assert(r.pressure_hpa > 800.0f && r.pressure_hpa < 1100.0f);
        assert(r.humidity_percent >= 0.0f && r.humidity_percent <= 100.0f);
        puts("PASS BME680 (gas heater disabled)");
    } else if (!strcmp(argv[1], "sht4x_bmp3xx")) {
        memset(&devs[0], 0, sizeof(devs[0]));
        devs[0].addr = 0x44;
        devs[0].present = true;
        devs[0].is_sht4x = true;
        uint16_t rt = (uint16_t)llround((24.0 + 45.0) * 65535.0 / 175.0);
        uint16_t rh = (uint16_t)llround((52.0 + 6.0) * 65535.0 / 125.0);
        devs[0].rx_buf[0] = (uint8_t)(rt >> 8);
        devs[0].rx_buf[1] = (uint8_t)(rt & 0xFF);
        devs[0].rx_buf[2] = crc8(devs[0].rx_buf, 2);
        devs[0].rx_buf[3] = (uint8_t)(rh >> 8);
        devs[0].rx_buf[4] = (uint8_t)(rh & 0xFF);
        devs[0].rx_buf[5] = crc8(devs[0].rx_buf + 3, 2);
        devs[0].rx_len = 6;

        memset(&devs[1], 0, sizeof(devs[1]));
        devs[1].addr = 0x77;
        devs[1].present = true;
        devs[1].regs[0x00] = 0x50; /* BMP388 */
        devs[1].regs[0x03] = 0x60; /* drdy_press & drdy_temp */
        put_le16(&devs[1].regs[0x31], 27500);
        put_le16(&devs[1].regs[0x33], 19500);
        devs[1].regs[0x35] = (uint8_t)(int8_t)-6;
        put_le16(&devs[1].regs[0x36], 6800);
        put_le16(&devs[1].regs[0x38], 5500);
        devs[1].regs[0x3A] = 6;
        devs[1].regs[0x3B] = 1;
        put_le16(&devs[1].regs[0x3C], 18500);
        put_le16(&devs[1].regs[0x3E], 23000);
        devs[1].regs[0x40] = 3;
        devs[1].regs[0x41] = (uint8_t)(int8_t)-6;
        put_le16(&devs[1].regs[0x42], 4000);
        devs[1].regs[0x44] = 8;
        devs[1].regs[0x45] = (uint8_t)(int8_t)-10;
        /* Pick raw values yielding ~1013 hPa */
        uint32_t adc_t = 8388608;
        uint32_t adc_p = 7600000;
        devs[1].regs[0x04] = (uint8_t)(adc_p & 0xFF);
        devs[1].regs[0x05] = (uint8_t)((adc_p >> 8) & 0xFF);
        devs[1].regs[0x06] = (uint8_t)((adc_p >> 16) & 0xFF);
        devs[1].regs[0x07] = (uint8_t)(adc_t & 0xFF);
        devs[1].regs[0x08] = (uint8_t)((adc_t >> 8) & 0xFF);
        devs[1].regs[0x09] = (uint8_t)((adc_t >> 16) & 0xFF);
        dev_count = 2;
        assert(sensor_init() == ESP_OK);
        assert(!strcmp(sensor_name(), "SHT4x+BMP3xx"));
        assert(sensor_read(&r) && r.valid);
        assert(fabsf(r.temperature_c - 24.0f) < 0.05f);
        assert(fabsf(r.humidity_percent - 52.0f) < 0.1f);
        assert(r.pressure_hpa >= 300.0f && r.pressure_hpa <= 1100.0f);
        puts("PASS SHT4x+BMP3xx combo");
    } else if (!strcmp(argv[1], "unknown")) {
        memset(&devs[0], 0, sizeof(devs[0]));
        devs[0].addr = 0x55;
        devs[0].present = true;
        dev_count = 1;
        assert(sensor_init() == ESP_ERR_NOT_FOUND);
        assert(!strcmp(sensor_name(), "unknown@0x55"));
        assert(!sensor_read(&r) && !r.valid);
        puts("PASS unknown I2C device scan & fail-closed");
    } else {
        abort();
    }
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    project = Path(__file__).resolve().parents[1]
    compiler = shutil.which("cc") or "cc"
    flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"] if args.sanitize else []
    env = dict(os.environ, LC_ALL="C", ASAN_OPTIONS="halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1")
    with tempfile.TemporaryDirectory(prefix="weather-sensor-") as directory:
        work = Path(directory)
        for name, contents in HEADERS.items():
            path = work / "stubs" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(contents, encoding="utf-8")
        harness = work / "harness.c"
        harness.write_text(HARNESS, encoding="utf-8")
        exe = work / "test_sensor"
        subprocess.run([
            compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", *flags,
            "-I", str(work / "stubs"), "-I", str(project / "main"),
            str(project / "main/sensor.c"), str(harness), "-lm", "-o", str(exe),
        ], check=True, env=env, timeout=60)
        for case in ("bme280", "aht20_bmp280", "bme680", "sht4x_bmp3xx", "unknown"):
            subprocess.run([str(exe), case], check=True, env=env, timeout=15)


if __name__ == "__main__":
    main()
