#!/usr/bin/env python3
"""Host regression for the real application, UI, parser, epd.c and text/icon fonts.

Does NOT test real net.c, net_portal.c, portal_web.c, portal_dns.c (WiFi, provisioning, synchronization/events)
or hardware timing. Only net calls and ESP-IDF hardware/RTOS I/O are stubbed;
time is fixed, and every full 4736-byte SPI framebuffer is captured, not hashed.
Retry headings and retained bodies are checked with the real renderer and fonts;
the 5 s boot wait and stepped connect/retry sleeps are traced without waiting, and
hardware delays are ignored. Separate harnesses check the real weather.c parser and
the real UI/font renderer.
UI assertions inspect text/fonts, five equally spaced centered day columns,
single-line low°-high° ranges, icons, probabilities, today's umbrella advice and
forecast/footer pixel isolation;
application reference frames use the public UI API, not a duplicate body layout.

Requires only Python's standard library and a host C compiler (prefers cc).
All generated headers, harnesses and executables live in TemporaryDirectory.

    python3 tools/test_app.py
    python3 tools/test_app.py --baseline /path/to/old/main

--baseline takes an old main directory containing the application C files and headers.
Both versions must implement five-day weather_t (codes + precipitation probabilities)
and the advice-banner/forecast-first/sensor-footer UI contract and pass all assertions.
Their entire per-scenario traces must also match byte for byte (requests, timezone
calls, full frames, retry checks, sleeps and net calls). Direct parser/UI
regressions run only against current sources. Old current-weather/one-day,
code-less or probability-less contracts, old layouts and old fixed-30-second
retry cadences are incompatible;
--baseline does not translate them or
silently skip their assertions.
"""

import argparse
import copy
import json
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from datetime import date, datetime, timedelta
from pathlib import Path

SOURCES = ("main.c", "button.c", "ui.c", "weather.c", "weather_json.c", "epd.c", "fonts_aa.c", "font_clock.c", "font_icons.c", "voice_audio.c")
# Independent WMO -> official Weather Icons name/codepoint oracle, not ui.c's enum.
ICON_ORACLE = {
    "WI_SUN": ("wi-day-sunny", 0xF00D, (0, 1)),
    "WI_PARTLY_CLOUDY": ("wi-day-cloudy", 0xF002, (2,)),
    "WI_CLOUD": ("wi-cloudy", 0xF013, (3,)),
    "WI_FOG": ("wi-fog", 0xF014, (45, 48)),
    "WI_DRIZZLE": ("wi-day-sprinkle", 0xF00B, (51, 53, 55, 56, 57)),
    "WI_RAIN": ("wi-rain", 0xF019, (61, 63, 65, 66, 67)),
    "WI_SNOW": ("wi-snow", 0xF01B, (71, 73, 75, 77)),
    "WI_SHOWERS": ("wi-showers", 0xF01A, (80, 81, 82)),
    "WI_SNOW_SHOWERS": ("wi-day-snow", 0xF00A, (85, 86)),
    "WI_THUNDER": ("wi-thunderstorm", 0xF01E, (95,)),
    "WI_HAIL": ("wi-hail", 0xF015, (96, 99)),
}

# SSD1680 has 296 gate lines; the framebuffer is 128x296 panel RAM (16 bytes/row).
VISIBLE_W = 296
FRAME_BYTES = 16 * VISIBLE_W
# S: success, G: geolocation HTTP failure, W: weather HTTP failure, P: parse failure.
# Each character is one update and one refresh, including every failed retry.
# Values are (update outcomes, provisioning attempts, total SPI frames,
# transient connect failures before the first success). Every case also sends one
# blank-white frame before the boot wait to clear the previous session's image.
CASES = {
    "steady": ("SSSS", 0, 5, 0),
    "initial_failure": ("GSSS", 0, 5, 0),
    "refresh_failure": ("SGGS", 0, 5, 0),
    "weather_failure": ("SWWS", 0, 5, 0),
    "provisioning_retry": ("SSSS", 2, 7, 0),
    "all_failure": ("GGGG", 0, 5, 0),
    "initial_weather_failure": ("WSSS", 0, 5, 0),
    "parse_failure": ("SPPS", 0, 5, 0),
    "initial_parse_failure": ("PPSS", 0, 5, 0),
    "retry_reset": ("GGSG", 0, 5, 0),
    "retry_counter_width": ("G" * 10 + "SG", 0, 13, 0),
    "provisioning_initial_failure": ("GSSS", 2, 7, 0),
    "connect_backoff": ("SSSS", 0, 11, 6),
    "button_toggle": ("SS", 0, 5, 0),
    "button_switch_full": ("SS", 0, 11, 0),
    "button_retry": ("GS", 0, 4, 0),
}
# Presses are "cycle:milliseconds-after-the-update-frame"; each adds one redraw frame.
PRESSES = {
    "button_toggle": "0:5000,0:15000",
    # Switches 1..7 are partial; the 8th must use the full waveform.
    "button_switch_full": "0:1000,0:2000,0:3000,0:4000,0:5000,0:6000,0:7000,0:8000",
    "button_retry": "0:2000",
}
GEO_URL = "http://ip-api.com/json/?fields=status,city,lat,lon"
WEATHER_URL = (
    "https://api.open-meteo.com/v1/forecast?latitude=30.5728&longitude=104.0668"
    "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
    "&hourly=weather_code,temperature_2m,precipitation_probability"
    "&temperature_unit=celsius"
    "&timezone=auto&forecast_days=5&forecast_hours=21"
)
HEADERS = {
    "esp_err.h": """#pragma once
#define ESP_OK 0
#define ESP_ERR_NOT_SUPPORTED -2
#define ESP_ERR_INVALID_STATE -3
typedef int esp_err_t;
static inline const char *esp_err_to_name(esp_err_t err) { (void)err; return "stub"; }
""",
    "esp_log.h": """#pragma once
#include <assert.h>
#define ESP_ERROR_CHECK(x) assert((x) == 0)
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
""",
    "esp_timer.h": """#pragma once
#include <stdint.h>
static inline int64_t esp_timer_get_time(void) { return 0; }
""",
    "freertos/FreeRTOS.h": """#pragma once
#define pdMS_TO_TICKS(ms) (ms)
""",
    "freertos/task.h": """#pragma once
void vTaskDelay(unsigned ms);
""",
    "driver/gpio.h": """#pragma once
#include <stdint.h>
typedef struct { uint64_t pin_bit_mask; int mode, pull_up_en, pull_down_en, intr_type; } gpio_config_t;
#define GPIO_MODE_OUTPUT 1
#define GPIO_MODE_INPUT 2
#define GPIO_PULLUP_ENABLE 1
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
int gpio_config(const gpio_config_t *cfg);
int gpio_set_level(int pin, int level);
int gpio_get_level(int pin);
""",
    "driver/spi_master.h": """#pragma once
#include <stddef.h>
typedef void *spi_device_handle_t;
typedef struct { size_t length; const void *tx_buffer; } spi_transaction_t;
typedef struct { int mosi_io_num, miso_io_num, sclk_io_num, quadwp_io_num,
    quadhd_io_num, max_transfer_sz; } spi_bus_config_t;
typedef struct { int clock_speed_hz, mode, spics_io_num,
    queue_size; } spi_device_interface_config_t;
#define SPI2_HOST 2
#define SPI_DMA_CH_AUTO 0
static inline int spi_bus_initialize(int h, const spi_bus_config_t *c, int d) { return 0; }
static inline int spi_bus_add_device(int h, const spi_device_interface_config_t *c,
    spi_device_handle_t *s) { *s = (void *)1; return 0; }
int spi_device_polling_transmit(spi_device_handle_t spi, spi_transaction_t *t);
""",
}
FIXTURES = r'''
#pragma once
#include <stdio.h>
#include <string.h>
#include "weather.h"

_Static_assert(WEATHER_DAYS == 5, "the forecast contract requires exactly five days");
_Static_assert(WEATHER_HOURS == 21, "the hourly contract requires now + 20 hours");
static weather_t sample_weather(unsigned cycle) {
    const char *dates[WEATHER_DAYS] = {
        "2024-12-29", "2024-12-30", "2024-12-31", "2025-01-01", "2025-01-02"};
    const int codes[] = {0, 2, 3, 95, 45, 61, 71};
    const float probabilities[] = {10.25f, 49.6f, 49.4f, 5.25f, 80, 0, 20};
    weather_t w = {.utc_offset = cycle < 3 ? 28800 : 19800, .ok = true};
    strcpy(w.city, "Chengdu");
    for (int i = 0; i < WEATHER_DAYS; i++) {
        strcpy(w.days[i].date, dates[i]);
        w.days[i].tmax = 20.25f + cycle + i * 2;
        w.days[i].tmin = -6.75f + cycle + i * 1.5f;
        unsigned index = (cycle + i) % (sizeof(codes) / sizeof(codes[0]));
        w.days[i].code = codes[index];
        w.days[i].precip_probability = probabilities[index];
    }
    for (int i = 0; i < WEATHER_HOURS; i++) {
        w.hours[i].hour = (8 + i) % 24;
        w.hours[i].temperature_c = 14.5f + cycle + i * 0.5f;
        w.hours[i].code = codes[(cycle + i) % (sizeof(codes) / sizeof(codes[0]))];
        w.hours[i].precip_probability = (float)((cycle * 3 + i * 7) % 101);
    }
    return w;
}
static int sample_response(char *buf, size_t cap, unsigned cycle, bool bad) {
    weather_t w = sample_weather(cycle);
    char last_max[24], last_code[24], last_probability[24];
    /* All three rejected fields still exercise day FIVE at application level. */
    if (bad && cycle % 3 == 0) strcpy(last_max, "null");
    else snprintf(last_max, sizeof(last_max), "%.2f", (double)w.days[4].tmax);
    if (bad && cycle % 3 == 1) strcpy(last_code, "null");
    else snprintf(last_code, sizeof(last_code), "%d", w.days[4].code);
    if (bad && cycle % 3 == 2) strcpy(last_probability, "null");
    else snprintf(last_probability, sizeof(last_probability), "%.2f", (double)w.days[4].precip_probability);
    int n = snprintf(buf, cap,
        "{\"utc_offset_seconds\":%d,"
        "\"current_units\":{\"temperature_2m\":\"°C\",\"relative_humidity_2m\":\"%%\"},"
        "\"daily_units\":{\"time\":\"iso8601\",\"weather_code\":\"wmo code\","
        "\"temperature_2m_max\":\"°C\",\"temperature_2m_min\":\"°C\","
        "\"precipitation_probability_max\":\"%%\"},"
        "\"hourly_units\":{\"time\":\"iso8601\",\"weather_code\":\"wmo code\","
        "\"temperature_2m\":\"°C\",\"precipitation_probability\":\"%%\"},"
        /* Legacy current data is metadata, never a substitute for a sensor. */
        "\"current\":{\"temperature_2m\":%.1f,\"relative_humidity_2m\":99,\"weather_code\":63},"
        "\"daily\":{\"time\":[\"2024-12-29\",\"2024-12-30\",\"2024-12-31\","
        "\"2025-01-01\",\"2025-01-02\"],\"temperature_2m_max\":[%.2f,%.2f,%.2f,%.2f,%s],"
        "\"temperature_2m_min\":[%.2f,%.2f,%.2f,%.2f,%.2f],"
        "\"weather_code\":[%d,%d,%d,%d,%s],"
        "\"precipitation_probability_max\":[%.2f,%.2f,%.2f,%.2f,%s]},"
        "\"hourly\":{\"time\":[",
        bad ? 19800 : w.utc_offset, 81.5 + cycle,
        (double)w.days[0].tmax, (double)w.days[1].tmax, (double)w.days[2].tmax,
        (double)w.days[3].tmax, last_max,
        (double)w.days[0].tmin, (double)w.days[1].tmin, (double)w.days[2].tmin,
        (double)w.days[3].tmin, (double)w.days[4].tmin,
        w.days[0].code, w.days[1].code, w.days[2].code, w.days[3].code, last_code,
        (double)w.days[0].precip_probability, (double)w.days[1].precip_probability,
        (double)w.days[2].precip_probability, (double)w.days[3].precip_probability, last_probability);
    if (n < 0 || (size_t)n >= cap) return -1;
    for (int i = 0; i < WEATHER_HOURS; i++) {
        int total = 8 + i;
        n += snprintf(buf + n, cap - (size_t)n, "%s\"2024-12-%02dT%02d:00\"", i ? "," : "",
            29 + total / 24, total % 24);
        if (n < 0 || (size_t)n >= cap) return -1;
    }
    n += snprintf(buf + n, cap - (size_t)n, "],\"weather_code\":[");
    for (int i = 0; i < WEATHER_HOURS; i++) {
        n += snprintf(buf + n, cap - (size_t)n, "%s%d", i ? "," : "", w.hours[i].code);
        if (n < 0 || (size_t)n >= cap) return -1;
    }
    n += snprintf(buf + n, cap - (size_t)n, "],\"temperature_2m\":[");
    for (int i = 0; i < WEATHER_HOURS; i++) {
        n += snprintf(buf + n, cap - (size_t)n, "%s%.2f", i ? "," : "", (double)w.hours[i].temperature_c);
        if (n < 0 || (size_t)n >= cap) return -1;
    }
    n += snprintf(buf + n, cap - (size_t)n, "],\"precipitation_probability\":[");
    for (int i = 0; i < WEATHER_HOURS; i++) {
        n += snprintf(buf + n, cap - (size_t)n, "%s%.2f", i ? "," : "", (double)w.hours[i].precip_probability);
        if (n < 0 || (size_t)n >= cap) return -1;
    }
    n += snprintf(buf + n, cap - (size_t)n, "]}}");
    return n;
}
'''
HARNESS = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "epd.h"
#include "net.h"
#include "ui.h"
#include "device_mcp.h"
#include "fixtures.h"

typedef int esp_err_t;
#define ESP_OK 0
void app_main(void);
esp_err_t audio_init(void) { return ESP_OK; }
/* voice.c needs the real esp_xiaozhi/opus stack, so the app harness links stubs. */
esp_err_t voice_init(void) { return ESP_OK; }
static bool weather_reserved;
static unsigned weather_attempts;
void voice_net_ready(void) { assert(!weather_reserved && weather_attempts > 0); }
void voice_net_pause(void) {}
bool voice_weather_begin(void) { assert(!weather_reserved); weather_reserved = true; return true; }
void voice_weather_end(void) { assert(weather_reserved); weather_reserved = false; weather_attempts++; }
static jmp_buf done;
static const char *pattern;
static unsigned cycle;
static int portal_attempts, connect_failures;
static bool updating, have_weather, boot_delay_done;
static unsigned retries;
enum { FRAME_BYTES = PANEL_W / 8 * PANEL_H, BTN_GPIO = 4, BTN_TICK = 20, BTN_PRESS_MS = 100 };
static weather_t last_good;
static unsigned long wait_ms, wait_elapsed, press_started;
static unsigned long idle_ms, connect_wait_ms;
static unsigned connect_attempts;
static bool button_pressed_level;
static unsigned press_cycle[8], press_at[8], press_count, press_next;
static int expected_view;
static unsigned button_frames;
static bool reset_test, credentials_erased;
static unsigned reset_frames;

static unsigned expected_retry_seconds(unsigned failures) {
    unsigned s = 5;
    while (failures > 1 && s < 60) {
        s *= 2;
        failures--;
    }
    return s > 60 ? 60 : s;
}
/* main.c waits in 20 ms slices, so the app-level interval has to come from the same
   schedule the trace asserts: 30 min after a success, the stepped retry otherwise.
   check_app_frame already counted this failure, so `retries` is the current attempt. */
static unsigned long expected_wait_ms(void) {
    if (pattern[cycle] == 'S') return 1800000UL;
    return (unsigned long)expected_retry_seconds(retries) * 1000UL;
}
int gpio_config(const gpio_config_t *cfg) { (void)cfg; return 0; }
static int dc_level = 1, last_cmd;
static unsigned switch_count;
static bool expect_switch_refresh;
int gpio_set_level(int pin, int level) {
    if (pin == EPD_DC) dc_level = level;
    return 0;
}
int gpio_get_level(int pin) { return pin == BTN_GPIO ? (button_pressed_level ? 0 : 1) : 0; }
void vTaskDelay(unsigned ms) {
    /* epd.c settles for 2/10 ms; every application wait is sliced at BTN_TICK so the button is
       serviced, so accumulate the slices and trace the app-level interval when it completes. */
    if (!updating) {
        if (ms < (unsigned)BTN_TICK) return;
        assert(ms == (unsigned)BTN_TICK);
        idle_ms += ms;
        if (!boot_delay_done) {
            assert(idle_ms <= 5000);
            if (idle_ms == 5000) {
                boot_delay_done = true;
                idle_ms = 0;
                puts("boot_wait:5000");
            }
        } else if (connect_wait_ms) {
            assert(idle_ms <= connect_wait_ms);
            if (idle_ms == connect_wait_ms) {
                printf("connect_wait:%lu\n", connect_wait_ms);
                idle_ms = 0;
                connect_wait_ms = 0;
            }
        }
        return;
    }
    /* epd.c settling delays are 2/10 ms; the application waits in 20 ms slices. */
    if (ms < (unsigned)BTN_TICK) return;
    if (reset_test) {
        if (reset_frames == 0) {
            assert(ms == BTN_TICK);
            button_pressed_level = true;
            wait_elapsed += ms;
            assert(wait_elapsed <= 5000 + 2 * BTN_TICK);
        } else {
            assert(reset_frames == 1 && credentials_erased && ms == 1000);
            puts("reset_notice_wait");
        }
        return;
    }
    assert(ms == BTN_TICK);
    if (!wait_elapsed) wait_ms = expected_wait_ms();
    wait_elapsed += ms;
    if (button_pressed_level) {
        if (wait_elapsed - press_started >= BTN_PRESS_MS) {
            button_pressed_level = false;
            /* Page switches 1..7 are partial; the 8th is a full refresh. */
            switch_count++;
            expect_switch_refresh = true;
        }
    } else if (press_next < press_count && press_cycle[press_next] == cycle &&
               wait_elapsed >= press_at[press_next]) {
        button_pressed_level = true;
        press_started = wait_elapsed;
        expected_view = !expected_view;
        button_frames++;
        press_next++;
    }
    assert(wait_ms > 0);
    if (wait_elapsed < wait_ms) return;
    printf("sleep:%lu\n", wait_ms);
    wait_ms = 0;
    wait_elapsed = 0;
    if (++cycle == strlen(pattern)) longjmp(done, 1);
    printf("cycle:%u\n", cycle);
}

time_t time(time_t *out) {
    time_t fixed = 1730622600;
    if (out) *out = fixed;
    return fixed;
}

static void check_retry_font(void) {
    /* Missing glyphs silently disappear in epd_text, including in a reference frame. */
    const uint16_t required[] = {0x65e0, 0x7f51, 0x7edc, 0x540e, 0x91cd, 0x8bd5,
        '#', 's', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9'};
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        bool visible = false;
        for (unsigned j = 0; j < font_mid.n; j++) {
            if (font_mid.cps[j] != required[i]) continue;
            const aaglyph *g = &font_mid.glyphs[j];
            for (unsigned k = 0; k < (unsigned)((g->w + 7) / 8) * g->h; k++) {
                if (font_mid.data[g->offset + k]) visible = true;
            }
        }
        if (!visible) {
            fprintf(stderr, "font_mid: missing visible retry glyph U+%04X\n", required[i]);
            exit(2);
        }
    }
}

static bool boot_checked, have_last_frame;
static unsigned char last_frame[FRAME_BYTES];
static void check_boot_frame(unsigned char *fb) {
    unsigned char saved[FRAME_BYTES];
    weather_t none = {0};
    sensor_reading_t sensor = {0};
    memcpy(saved, fb, FRAME_BYTES);
    /* No forecast yet: exactly the placeholder page, before the first update runs. */
    ui_draw_weather(&none, &sensor);
    ui_draw_status(UI_STATUS_CONNECTING, 0);
    bool matches = memcmp(saved, fb, FRAME_BYTES) == 0;
    memcpy(fb, saved, FRAME_BYTES);
    if (!matches) {
        fprintf(stderr, "boot frame is not the placeholder page\n");
        exit(2);
    }
}

static void check_app_frame(unsigned char *fb) {
    unsigned char saved[FRAME_BYTES];
    char heading[64];
    bool button = button_frames > 0;
    bool success = pattern[cycle] == 'S';
    memcpy(saved, fb, sizeof(saved));
    if (!success) check_retry_font();

    /* Rebuild the whole frame through the public renderers, then overlay the title. */
    weather_t expected;
    if (success) expected = sample_weather(cycle);
    else if (have_weather) expected = last_good;
    else memset(&expected, 0, sizeof(expected));
    sensor_reading_t sensor = {0};
    if (expected_view) ui_draw_hourly(&expected, &sensor);
    else ui_draw_weather(&expected, &sensor);
    epd_fill_rect(0, 0, EPD_W, 22, EPD_WHITE);
    unsigned attempt = 0;
    if (success) {
        epd_text(8, 16, &font_mid, "Chengdu");
        if (expected_view) {
            const char *label = "未来10小时";
            epd_text(174 - epd_text_w(&font_mid, label) / 2, 16, &font_mid, label);
        } else {
            /* Independent policy expectations for sample_weather's explicit fixture table. */
            const char *advice[] = {"降水概率低", "降水概率低", "降水概率低", "雷雨注意安全",
                                   "今日带伞", "降水概率低", "降水概率低"};
            const char *banner = advice[cycle % 7];
            epd_text(174 - epd_text_w(&font_mid, banner) / 2, 16, &font_mid, banner);
        }
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(heading, sizeof(heading), "%H:%M", &tm);
        epd_text(EPD_W - 8 - epd_text_w(&font_clock, heading), 16, &font_clock, heading);
    } else {
        attempt = button ? retries : retries + 1;
        unsigned delay = expected_retry_seconds(attempt);
        snprintf(heading, sizeof(heading), "无网络 %us后重试 #%u", delay, attempt);
        if (8 + epd_text_w(&font_mid, heading) > EPD_W)
            snprintf(heading, sizeof(heading), "无网络 %us重试 #%u", delay, attempt);
        epd_text(8, 16, &font_mid, heading);
    }
    ui_draw_status(success ? UI_STATUS_UPDATED : UI_STATUS_RETRY, attempt);
    bool matches = memcmp(saved, fb, FRAME_BYTES) == 0;
    /* The SPI buffer is epd.c's writable framebuffer; leave no reference pixels behind. */
    memcpy(fb, saved, FRAME_BYTES);
    if (!matches) {
        fprintf(stderr, "cycle %u: %s (%s) does not match the complete SPI frame\n", cycle,
            success ? (expected_view ? "hourly strip / city / label / clock"
                                     : "five-day forecast / city / advice / clock")
                    : (have_weather ? "retry heading / retained weather body"
                                    : "retry heading / initial placeholder body"),
            button ? "button" : "update");
        exit(2);
    }
    if (button) {
        button_frames--;
        return;
    }
    if (success) {
        last_good = sample_weather(cycle);
        have_weather = true;
        retries = 0;
    } else {
        unsigned attempt = retries + 1;
        printf("retry:%u:%u:%s\n", attempt, expected_retry_seconds(attempt),
            have_weather ? "weather" : "offline");
        retries = attempt;
    }
}

/* The status corner is x >= 200, y >= 110; a refresh that changes nothing else is a status
   refresh, so it is traced separately and must not repaint the page. */
static bool status_only_frame(const unsigned char *fb) {
    if (!have_last_frame || memcmp(fb, last_frame, FRAME_BYTES) == 0) return false;
    for (int x = 0; x < 296; x++) {
        for (int y = 0; y < 128; y++) {
            if (x >= 200 && y >= 110) continue;
            int px = 127 - y, py = x, byte = py * 16 + (px >> 3), bit = 0x80 >> (px & 7);
            if (((fb[byte] & bit) != 0) != ((last_frame[byte] & bit) != 0)) return false;
        }
    }
    return true;
}

int spi_device_polling_transmit(spi_device_handle_t spi, spi_transaction_t *t) {
    if (t->length == 8) {
        const unsigned char *p = t->tx_buffer;
        if (!dc_level) {
            last_cmd = p[0];
        } else if (last_cmd == 0x22 && expect_switch_refresh) {
            expect_switch_refresh = false;
            unsigned expected = switch_count % 8 == 0 ? 0xC7 : 0xC0;
            if (p[0] != expected) {
                fprintf(stderr, "page switch %u used update sequence 0x%02X; expected 0x%02X "
                    "(switches 1..7 partial, 8th full)\n", switch_count, p[0], expected);
                exit(2);
            }
        }
    }
    if (t->length == FRAME_BYTES * 8) {
        /* epd_refresh submits its own mutable framebuffer, not a read-only copy. */
        unsigned char *p = (unsigned char *)t->tx_buffer;
        bool status = status_only_frame(p);
        if (status) {
            puts("status");
            memcpy(last_frame, p, FRAME_BYTES);
            return 0;
        }
        if (reset_test && button_pressed_level) {
            assert(reset_frames < 2);
            unsigned char saved[FRAME_BYTES];
            memcpy(saved, p, FRAME_BYTES);
            if (reset_frames == 0) {
                const char *notice = "WiFi 重置中";
                epd_clear(EPD_WHITE);
                epd_text(148 - epd_text_w(&font_mid, notice) / 2, 70, &font_mid, notice);
            } else {
                ui_draw_portal_hint();
            }
            assert(memcmp(saved, p, FRAME_BYTES) == 0);
            memcpy(p, saved, FRAME_BYTES);
            puts(reset_frames++ == 0 ? "reset_notice_frame" : "portal_hint_frame");
        } else if (updating) {
            check_app_frame(p);
        } else if (!boot_checked) {
            boot_checked = true;
            check_boot_frame(p);
        }
        memcpy(last_frame, p, FRAME_BYTES);
        have_last_frame = true;
        printf("frame:");
        for (int i = 0; i < FRAME_BYTES; i++) printf("%02x", p[i]);
        puts("");
    }
    return 0;
}

void net_init(void) { puts("net_init"); }
bool net_has_stored(void) { return portal_attempts <= 0; }
bool net_try_stored(int timeout) {
    assert(timeout == 25000);
    puts("connect");
    if (reset_test && reset_frames == 2) longjmp(done, 1);
    if (portal_attempts > 0) return false;
    if (connect_failures > 0) {
        connect_failures--;
        connect_wait_ms = (unsigned long)expected_retry_seconds(++connect_attempts) * 1000UL;
        return false;
    }
    return true;
}
void net_portal(void) {
    puts("portal");
    if (portal_attempts > 0) portal_attempts--;
}
const char *net_ap_ssid(void) { return "weather-123ABC"; }
const char *net_ap_pass(void) { return "weather123ABC"; }
void net_forget_creds(void) {
    assert(reset_test && reset_frames == 1 && !credentials_erased);
    assert(wait_elapsed == 5000 + 2 * BTN_TICK);
    credentials_erased = true;
    puts("forget");
}
void net_time_sync(void) { assert(weather_reserved); puts("time_sync"); }
void device_mcp_publish(const weather_t *weather, const sensor_reading_t *sensor, bool retrying, bool updated) {
    static weather_t previous;
    static bool published;
    assert(weather_reserved && sensor && !sensor->valid);
    assert(updated == (pattern[cycle] == 'S'));
    assert(retrying == !updated);
    if (updated) {
        assert(weather && weather->ok);
        previous = *weather;
        published = true;
    } else if (published) {
        assert(weather && memcmp(weather, &previous, sizeof(previous)) == 0);
    } else {
        assert(weather == NULL);
    }
}
void net_apply_tz(int offset) {
    assert(offset == 28800 || offset == 19800);
    printf("timezone:%d\n", offset);
    setenv("TZ", offset == 28800 ? "XXX-08:00" : "XXX-05:30", 1);
    tzset();
}
int net_http_get(const char *url, char *buf, size_t cap) {
    assert(weather_reserved);
    assert(cycle < strlen(pattern));
    printf("request:%s\n", url);
    if (strstr(url, "ip-api.com")) {
        updating = true;
        if (pattern[cycle] == 'G') return -1;
        return snprintf(buf, cap,
            "{\"status\":\"success\",\"city\":\"Chengdu\",\"lat\":30.5728,\"lon\":104.0668}");
    }
    assert(strcmp(url,
        "https://api.open-meteo.com/v1/forecast?latitude=30.5728&longitude=104.0668"
        "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
        "&hourly=weather_code,temperature_2m,precipitation_probability"
        "&temperature_unit=celsius"
        "&timezone=auto&forecast_days=5&forecast_hours=21") == 0);
    if (pattern[cycle] == 'W') return -1;
    /* Reject day FIVE's null temperature/code/probability; never apply a failed offset. */
    int count = sample_response(buf, cap, cycle, pattern[cycle] == 'P');
    assert(count >= 0 && (size_t)count < cap);
    return count;
}

int main(int argc, char **argv) {
    assert(argc >= 3 && argc <= 5);
    pattern = argv[1];
    portal_attempts = atoi(argv[2]);
    connect_failures = argc > 3 ? atoi(argv[3]) : 0;
    reset_test = argc > 4 && strcmp(argv[4], "hold") == 0;
    if (argc > 4 && argv[4][0] && !reset_test) {
        char *spec = strdup(argv[4]);
        assert(spec);
        char *save = NULL;
        for (char *token = strtok_r(spec, ",", &save); token; token = strtok_r(NULL, ",", &save)) {
            assert(press_count < sizeof(press_at) / sizeof(press_at[0]));
            assert(sscanf(token, "%u:%u", &press_cycle[press_count], &press_at[press_count]) == 2);
            press_count++;
        }
        free(spec);
    }
    setenv("TZ", "UTC0", 1);
    tzset();
    puts("cycle:0");
    if (setjmp(done) == 0) {
        app_main();
        return 1; /* The application must run until the final simulated sleep. */
    }
    return 0;
}
'''
WEATHER_HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "net.h"
#include "fixtures.h"

static const char *response;
static unsigned requests;

int net_http_get(const char *url, char *buf, size_t cap) {
    const char *body;
    if (++requests == 1) {
        assert(strcmp(url, "http://ip-api.com/json/?fields=status,city,lat,lon") == 0);
        body = "{\"status\":\"success\",\"city\":\"Chengdu\",\"lat\":30.5728,\"lon\":104.0668}";
    } else {
        assert(requests == 2);
        assert(strcmp(url,
            "https://api.open-meteo.com/v1/forecast?latitude=30.5728&longitude=104.0668"
            "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
            "&hourly=weather_code,temperature_2m,precipitation_probability"
            "&temperature_unit=celsius"
            "&timezone=auto&forecast_days=5&forecast_hours=21") == 0);
        body = response;
    }
    int count = snprintf(buf, cap, "%s", body);
    assert(count >= 0 && (size_t)count < cap);
    return count;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    response = argv[1];
    /* Failures must clear a previously successful weather result. */
    weather_t w = sample_weather(7);
    bool result = weather_update(&w);
    assert(requests == 2);
    printf("{\"result\":%d,\"ok\":%d,\"utc_offset\":%d,\"days\":[",
        result, w.ok, w.utc_offset);
    if (result && w.ok) {
        assert(strcmp(w.city, "Chengdu") == 0);
        for (int i = 0; i < WEATHER_DAYS; i++) {
            assert(memchr(w.days[i].date, 0, sizeof(w.days[i].date)));
            printf("%s{\"date\":\"%s\",\"tmax\":%.9g,\"tmin\":%.9g,\"code\":%d,"
                "\"precip_probability\":%.9g}",
                i ? "," : "", w.days[i].date, (double)w.days[i].tmax, (double)w.days[i].tmin,
                w.days[i].code, (double)w.days[i].precip_probability);
        }
        printf("],\"hours\":[");
        for (int i = 0; i < WEATHER_HOURS; i++) {
            printf("%s{\"hour\":%d,\"temperature\":%.9g,\"code\":%d,"
                "\"precip_probability\":%.9g}",
                i ? "," : "", w.hours[i].hour, (double)w.hours[i].temperature_c,
                w.hours[i].code, (double)w.hours[i].precip_probability);
        }
    }
    puts("]}");
    return 0;
}
'''


UI_WRAPPER = r'''
#include "epd.h"
void traced_text(int x, int y, const aafont *font, const char *text);
/* Instrument only ui.c; epd.c remains the real, unmodified pixel renderer. */
#define epd_text traced_text
#include "ui.c"
'''
UI_HARNESS = r'''
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "epd.h"
#include "net.h"
#include "ui.h"
#include "font_icons.h"
#include "fixtures.h"

int gpio_config(const gpio_config_t *cfg) { (void)cfg; return 0; }
int gpio_set_level(int pin, int level) { (void)pin; (void)level; return 0; }
int gpio_get_level(int pin) { (void)pin; return 0; }
void vTaskDelay(unsigned ms) { assert(ms < 1000); }
time_t time(time_t *out) {
    time_t fixed = 1730622600;
    if (out) *out = fixed;
    return fixed;
}
const char *net_ap_ssid(void) { return "weather-123ABC"; }

void traced_text(int x, int y, const aafont *font, const char *text) {
    int pen = x;
    int left = EPD_W, top = EPD_H, right = -1, bottom = -1;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        unsigned c = *p++;
        int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
        c &= extra == 3 ? 7 : extra == 2 ? 15 : extra == 1 ? 31 : 127;
        for (int n = 0; n < extra; n++) {
            assert((*p & 0xc0) == 0x80);
            c = (c << 6) | (*p++ & 63);
        }
        const aaglyph *glyph = NULL;
        for (unsigned i = 0; i < font->n; i++)
            if (font->cps[i] == c) glyph = &font->glyphs[i];
        if (!glyph) {
            fprintf(stderr, "missing UI glyph U+%04X in text: %s\n", c, text);
            exit(2);
        }
        bool visible = false;
        for (unsigned row = 0; row < glyph->h; row++) {
            for (unsigned col = 0; col < glyph->w; col++) {
                unsigned index = glyph->offset + row * ((glyph->w + 7) / 8) + col / 8;
                if (!(font->data[index] & (0x80 >> (col & 7)))) continue;
                int px = pen + glyph->xoff + col, py = y + glyph->yoff + row;
                if (px < 0 || px >= EPD_W || py < 0 || py >= EPD_H) {
                    fprintf(stderr, "clipped UI glyph U+%04X at %d,%d in: %s\n", c, px, py, text);
                    exit(2);
                }
                if (px < left) left = px;
                if (px > right) right = px;
                if (py < top) top = py;
                if (py > bottom) bottom = py;
                visible = true;
            }
        }
        if (!visible && c != ' ') {
            fprintf(stderr, "blank UI glyph U+%04X in text: %s\n", c, text);
            exit(2);
        }
        pen += glyph->adv;
    }
    const char *name = font == &font_mid ? "mid" : font == &font_temp ? "temp" :
                       font == &font_small ? "small" : font == &font_clock ? "clock" :
                       font == &font_huge ? "huge" :
                                              font == &font_icon_daily ? "icon_daily" : font == &font_icon ? "icon" : "other";
    printf("text:%d:%d:%d:%s:%d:%d:%d:%d:%s\n", x, y, epd_text_w(font, text), name,
        left, top, right, bottom, text);
    epd_text(x, y, font, text);
}
int spi_device_polling_transmit(spi_device_handle_t spi, spi_transaction_t *t) {
    if (t->length == PANEL_W / 8 * PANEL_H * 8) {
        const unsigned char *p = t->tx_buffer;
        printf("frame:");
        for (int i = 0; i < PANEL_W / 8 * PANEL_H; i++) printf("%02x", p[i]);
        puts("");
    }
    return 0;
}
int main(int argc, char **argv) {
    assert(argc >= 2 && argc <= 5);
    const char *mode = argv[1];
    if (!strcmp(mode, "icon_metrics")) {
        /* Read the linked font tables directly, independently of UI selection/rendering. */
        const aafont *fonts[] = {&font_icon_daily, &font_icon};
        const char *names[] = {"icon_daily", "icon"};
        assert(fonts[0]->data != fonts[1]->data && fonts[0]->glyphs != fonts[1]->glyphs);
        printf("{");
        for (unsigned f = 0; f < 2; f++) {
            const aafont *font = fonts[f];
            printf("%s\"%s\":{\"height\":%u,\"glyphs\":[", f ? "," : "", names[f], font->line_height);
            for (unsigned i = 0; i < font->n; i++) {
                const aaglyph *g = &font->glyphs[i];
                printf("%s{\"cp\":%u,\"offset\":%u,\"w\":%u,\"h\":%u,\"adv\":%u,"
                       "\"xoff\":%d,\"yoff\":%d,\"ink\":[", i ? "," : "", font->cps[i],
                       g->offset, g->w, g->h, g->adv, g->xoff, g->yoff);
                unsigned ink = 0;
                for (unsigned row = 0; row < g->h; row++) {
                    for (unsigned col = 0; col < g->w; col++) {
                        unsigned index = g->offset + row * ((g->w + 7) / 8) + col / 8;
                        if (font->data[index] & (0x80 >> (col & 7)))
                            printf("%s[%d,%d]", ink++ ? "," : "", g->xoff + (int)col, g->yoff + (int)row);
                    }
                }
                printf("]}");
            }
            printf("]}");
        }
        puts("}");
        return 0;
    }
    if (!strcmp(mode, "clock_metrics")) {
        assert(font_clock.line_height == 16 && font_clock.data != font_small.data);
        const aafont *fonts[] = {&font_small, &font_clock};
        unsigned ink[2] = {0};
        for (unsigned f = 0; f < 2; f++) {
            const char *required = "0123456789:";
            for (const char *p = required; *p; p++) {
                const aaglyph *g = NULL;
                for (unsigned i = 0; i < fonts[f]->n; i++)
                    if (fonts[f]->cps[i] == (unsigned)*p) g = &fonts[f]->glyphs[i];
                assert(g && g->w && g->h && g->adv);
                assert(g->xoff >= 0 && g->xoff + g->w <= g->adv);
                assert(16 + g->yoff >= 0 && 16 + g->yoff + g->h <= 22);
                unsigned visible = 0;
                for (unsigned row = 0; row < g->h; row++)
                    for (unsigned col = 0; col < g->w; col++)
                        visible += !!(fonts[f]->data[g->offset + row * ((g->w + 7) / 8) + col / 8]
                                      & (0x80 >> (col & 7)));
                assert(visible);
                ink[f] += visible;
            }
        }
        assert(ink[1] > ink[0]);
        for (unsigned hour = 0; hour < 24; hour++) {
            for (unsigned minute = 0; minute < 60; minute++) {
                char text[6];
                snprintf(text, sizeof(text), "%02u:%02u", hour, minute);
                int width = epd_text_w(&font_clock, text);
                assert(width > 0 && 292 - width >= 250);
            }
        }
        printf("clock:1440:%u:%u\n", ink[0], ink[1]);
        return 0;
    }
    if (!strcmp(mode, "metrics")) {
        /* Only public renderer metrics; no production range formatting/selection. */
        const aafont *fonts[] = {&font_temp, &font_small};
        const char *names[] = {"temp", "small"};
        /* Everything the range oracle needs, including the degree sign. */
        const char *glyphs[] = {"-", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "°"};
        printf("{");
        for (int f = 0; f < 2; f++) {
            printf("%s\"%s\":{", f ? "," : "", names[f]);
            for (size_t i = 0; i < sizeof(glyphs) / sizeof(glyphs[0]); i++) {
                printf("%s\"%s\":%d", i ? "," : "", glyphs[i],
                    epd_text_w(fonts[f], glyphs[i]));
            }
            printf("}");
        }
        puts("}");
        return 0;
    }
    weather_t w = sample_weather(0);
    /* Distinct signed/fractional values make swapped or repeated days detectable. */
    const float high[WEATHER_DAYS] = {25.25f, 18.75f, -1.25f, 3.75f, 12.25f};
    const float low[WEATHER_DAYS] = {12.25f, -2.75f, -15.25f, -8.75f, 0.25f};
    const int codes[WEATHER_DAYS] = {0, 2, 45, 61, 95};
    const float probabilities[WEATHER_DAYS] = {10.25f, 39.6f, 40, 80.75f, 100};
    for (int i = 0; i < WEATHER_DAYS; i++) {
        w.days[i].tmax = high[i];
        w.days[i].tmin = low[i];
        w.days[i].code = codes[i];
        w.days[i].precip_probability = probabilities[i];
    }
    if (!strcmp(mode, "city_long")) strcpy(w.city, "ABCDEFGHIJKLMNOPQRSTUVW");
    sensor_reading_t sensor = {.temperature_c = 91.2f, .humidity_percent = 17, .valid = false};
    const sensor_reading_t *reading = &sensor;
    if (!strcmp(mode, "sensor_null")) reading = NULL;
    if (!strcmp(mode, "sensor_invalid_poison")) {
        sensor.temperature_c = -123.4f;
        sensor.humidity_percent = 98;
    }
    if (!strncmp(mode, "sensor_", 7) && strcmp(mode, "sensor_null") &&
        strcmp(mode, "sensor_invalid_poison")) {
        sensor = (sensor_reading_t){.temperature_c = 21.4f, .humidity_percent = 65, .valid = true};
        if (!strcmp(mode, "sensor_sample")) {
            sensor.temperature_c = 23.4f;
            sensor.humidity_percent = 56;
        }
        if (!strcmp(mode, "sensor_temp_wide")) sensor.temperature_c = FLT_MAX;
        if (!strcmp(mode, "sensor_temp_nan")) sensor.temperature_c = NAN;
        if (!strcmp(mode, "sensor_temp_inf")) sensor.temperature_c = INFINITY;
        if (!strcmp(mode, "sensor_humidity_nan")) sensor.humidity_percent = NAN;
        if (!strcmp(mode, "sensor_humidity_negative")) sensor.humidity_percent = -1;
        if (!strcmp(mode, "sensor_humidity_over100")) sensor.humidity_percent = 101;
        if (!strcmp(mode, "sensor_temp")) sensor.temperature_c = -8.7f;
        if (!strcmp(mode, "sensor_humidity")) sensor.humidity_percent = 43;
    }
    if (!strcmp(mode, "no_forecast_sensor")) {
        sensor = (sensor_reading_t){.temperature_c = 21.4f, .humidity_percent = 65, .valid = true};
    }
    if (!strncmp(mode, "no_forecast", 11)) {
        w.ok = false;
        if (!strcmp(mode, "no_forecast_poison")) {
            strcpy(w.city, "Ignored");
            for (int i = 0; i < WEATHER_DAYS; i++) {
                strcpy(w.days[i].date, "2031-09-19");
                w.days[i].tmax = 97;
                w.days[i].tmin = 96;
                w.days[i].code = i % 2 ? 100 : -1;
                w.days[i].precip_probability = i % 2 ? NAN : 100;
            }
        }
    }
    if (!strcmp(mode, "all_code")) {
        assert(argc == 3);
        for (int i = 0; i < WEATHER_DAYS; i++) w.days[i].code = atoi(argv[2]);
    }
    if (!strcmp(mode, "policy")) {
        assert(argc == 4);
        w.days[0].code = atoi(argv[2]);
        w.days[0].precip_probability = strtof(argv[3], NULL);
    }
    if (!strcmp(mode, "range")) {
        assert(argc == 5);
        int day = atoi(argv[2]);
        assert(day >= 0 && day < WEATHER_DAYS);
        w.days[day].tmin = strtof(argv[3], NULL);
        w.days[day].tmax = strtof(argv[4], NULL);
    }
    if (!strncmp(mode, "day_", 4)) {
        assert(argc == ((!strcmp(mode, "day_code") || !strcmp(mode, "day_prob")) ? 4 : 3));
        int day = atoi(argv[2]);
        assert(day >= 0 && day < WEATHER_DAYS);
        if (!strcmp(mode, "day_date")) strcpy(w.days[day].date, "2031-09-19");
        else if (!strcmp(mode, "day_max")) w.days[day].tmax = 38.25f;
        else if (!strcmp(mode, "day_min")) w.days[day].tmin = -29.25f;
        else if (!strcmp(mode, "day_code")) w.days[day].code = atoi(argv[3]);
        else if (!strcmp(mode, "day_prob")) w.days[day].precip_probability = strtof(argv[3], NULL);
        else abort();
    }
    if (!strncmp(mode, "hourly", 6)) {
        const int hour_codes[5] = {0, 2, 45, 61, 95};
        const float hour_probabilities[5] = {10.25f, 39.6f, 40, 80.75f, 100};
        for (int i = 0; i < WEATHER_HOURS; i++) {
            w.hours[i].hour = (8 + i) % 24;
            w.hours[i].temperature_c = 20.25f + i * 0.5f;
            w.hours[i].code = hour_codes[i % 5];
            w.hours[i].precip_probability = hour_probabilities[i % 5];
        }
        if (!strcmp(mode, "hourly_no_forecast")) w.ok = false;
        if (!strcmp(mode, "hourly_all_code")) {
            assert(argc == 3);
            for (int i = 0; i < WEATHER_HOURS; i++) w.hours[i].code = atoi(argv[2]);
        }
        if (!strcmp(mode, "hourly_code")) {
            assert(argc == 4);
            int hour = atoi(argv[2]);
            assert(hour >= 0 && hour < 10);
            w.hours[hour].code = atoi(argv[3]);
        }
        setenv("TZ", "UTC0", 1);
        tzset();
        epd_init();
        ui_draw_hourly(&w, reading);
        epd_refresh();
        return 0;
    }
    if (!strncmp(mode, "status_", 7)) {
        const char *name = mode + 7;
        ui_status_t status = UI_STATUS_NONE;
        unsigned attempt = 0;
        if (!strcmp(name, "connecting")) status = UI_STATUS_CONNECTING;
        else if (!strcmp(name, "fetching")) status = UI_STATUS_FETCHING;
        else if (!strcmp(name, "syncing")) status = UI_STATUS_SYNCING;
        else if (!strcmp(name, "updated")) status = UI_STATUS_UPDATED;
        else if (!strcmp(name, "retry")) { status = UI_STATUS_RETRY; attempt = 12; }
        else assert(!strcmp(name, "none"));
        setenv("TZ", "UTC0", 1);
        tzset();
        epd_init();
        ui_draw_weather(&w, reading);
        ui_draw_status(status, attempt);
        epd_refresh();
        return 0;
    }
    setenv("TZ", "UTC0", 1);
    tzset();
    epd_init();
    ui_draw_weather(!strcmp(mode, "weather_null") ? NULL : &w, reading);
    epd_refresh();
    return 0;
}
'''


def weather_cases():
    def dates(start):
        first = date.fromisoformat(start)
        return [(first + timedelta(days=i)).isoformat() for i in range(5)]

    def hour_times(start="2024-12-29T08:00"):
        first = datetime.fromisoformat(start)
        # isoformat keeps years below 1000 zero-padded; strftime("%Y") does not.
        return [(first + timedelta(hours=i)).isoformat(timespec="minutes") for i in range(21)]

    def encode(response):
        return json.dumps(response, ensure_ascii=False, separators=(",", ":"))

    def expected(response):
        daily = response["daily"]
        hourly = response["hourly"]
        return {"utc_offset": response["utc_offset_seconds"], "days": [
            {"date": daily["time"][i], "tmax": daily["temperature_2m_max"][i],
             "tmin": daily["temperature_2m_min"][i], "code": daily["weather_code"][i],
             "precip_probability": daily["precipitation_probability_max"][i]}
            for i in range(5)], "hours": [
            {"hour": int(hourly["time"][i][11:13]),
             "temperature": hourly["temperature_2m"][i],
             "code": hourly["weather_code"][i],
             "precip_probability": hourly["precipitation_probability"][i]}
            for i in range(21)]}

    base = {
        "utc_offset_seconds": 28800,
        "daily": {"time": dates("2024-12-29"),
                  "temperature_2m_max": [25.25, 18.75, -1.25, 3.75, 12.25],
                  "temperature_2m_min": [12.25, -2.75, -15.25, -8.75, 0.25],
                  "weather_code": [0, 2, 45, 61, 95],
                  "precipitation_probability_max": [10.25, 39.6, 40, 80.75, 100]},
        "hourly": {"time": hour_times(),
                   "temperature_2m": [14.5 + i * 0.5 for i in range(21)],
                   "weather_code": [(i * 3) % 8 for i in range(21)],
                   "precipitation_probability": [(i * 7) % 101 for i in range(21)]},
    }
    yield "minimal_no_current_or_units", encode(base), expected(base)
    units = {
        "current_units": {"temperature_2m": "°C", "relative_humidity_2m": "%",
                          "weather_code": "wmo code"},
        "daily_units": {"time": "iso8601", "weather_code": "wmo code",
                        "temperature_2m_max": "°C", "temperature_2m_min": "°C",
                        "precipitation_probability_max": "%"},
        # This is allowed legacy metadata, not a required current-reading source.
        "current": {"temperature_2m": 81.5, "relative_humidity_2m": 99, "weather_code": 63},
    }
    yield "units_before_data", encode({**units, **base}), expected(base)
    yield "units_after_data", encode({**base, **units}), expected(base)
    reordered = {"utc_offset_seconds": 28800,
                 "hourly": dict(reversed(list(base["hourly"].items()))),
                 **units,
                 "daily": dict(reversed(list(base["daily"].items())))}
    yield "reordered_pretty", json.dumps(
        reordered, ensure_ascii=False, indent=2, separators=(",", " : ")), expected(base)
    scoped = {"metadata": {"utc_offset_seconds": -43200,
                           "daily": {key: [-99] * 5 for key in base["daily"]}}, **base}
    scoped["daily"] = {"metadata": {key: [88] * 5 for key in base["daily"]},
                       **base["daily"]}
    scoped["quoted"] = 'braces } [ and escaped "daily": {"time": [0]}'
    yield "object_scope_and_quoted_decoys", encode(scoped), expected(base)

    # Every legal value appears in EVERY position, including unmapped WMO codes:
    # the parser accepts 0..99 while the UI independently chooses unknown icons.
    for code in range(100):
        response = copy.deepcopy(base)
        response["daily"]["weather_code"] = [(code + i) % 100 for i in range(5)]
        yield f"code_range_rotated_{code}", encode(response), expected(response)
    code_scope = {"weather_code": [99] * 5, **units, **copy.deepcopy(base)}
    code_scope["daily"] = {"metadata": {"weather_code": [88] * 5}, **base["daily"]}
    yield "code_direct_scope_before_nested_decoys", encode(code_scope), expected(base)
    for label, value in (("missing", None), ("null", None), ("scalar", 63), ("short", [0] * 4)):
        response = copy.deepcopy(code_scope)
        if label == "missing":
            del response["daily"]["weather_code"]
        else:
            response["daily"]["weather_code"] = value
        # A unit-metadata array or legacy current code cannot rescue a bad daily array.
        response["daily_units"]["weather_code"] = [0, 2, 45, 61, 95]
        response["fallback"] = copy.deepcopy(base)
        yield f"code_scope_decoys_{label}", encode(response), None

    probability_field = "precipitation_probability_max"
    valid_probabilities = (0, 0.25, 39.4, 39.5, 39.6, 40, 80.75, 99.75, 100)
    for n in range(len(valid_probabilities)):
        response = copy.deepcopy(base)
        response["daily"][probability_field] = [
            valid_probabilities[(n + i) % len(valid_probabilities)] for i in range(5)]
        yield f"probability_fractional_endpoints_rotated_{n}", encode(response), expected(response)
    exponent_probability = copy.deepcopy(base)
    exponent_probability["daily"][probability_field] = [0, 40, 100, 39.6, 0.5]
    raw_exponent = copy.deepcopy(exponent_probability)
    tokens = ("0e0", "4e1", "1e2", "3.96e1", "5E-1")
    for i in range(5):
        raw_exponent["daily"][probability_field][i] = f"EXPONENT_{i}"
    raw = encode(raw_exponent)
    for i, token in enumerate(tokens):
        raw = raw.replace(f'"EXPONENT_{i}"', token)
    yield "probability_valid_exponents", raw, expected(exponent_probability)
    probability_scope = {
        probability_field: [99] * 5, **units, **copy.deepcopy(base)}
    probability_scope["daily"] = {"metadata": {probability_field: [88] * 5}, **base["daily"]}
    yield "probability_direct_scope_before_decoys", encode(probability_scope), expected(base)
    for label, value in (("missing", None), ("null", None), ("string", "80"),
                         ("scalar", 80), ("short", [80] * 4)):
        response = copy.deepcopy(probability_scope)
        if label == "missing":
            del response["daily"][probability_field]
        else:
            response["daily"][probability_field] = value
        response["daily_units"][probability_field] = [10, 20, 30, 40, 50]
        response["fallback"] = copy.deepcopy(base)
        yield f"probability_units_or_scope_cannot_rescue_{label}", encode(response), None

    for start in ("2025-01-30", "2025-02-27", "2024-02-27", "2000-02-27",
                  "1900-02-27", "0001-01-01", "9999-12-27",
                  "2024-02-29", "2025-12-29"):
        response = copy.deepcopy(base)
        response["daily"]["time"] = dates(start)
        # The hourly strip must start on daily[0]'s local date; weather.c rejects a
        # strip anchored to any other day, so both arrays move together.
        response["hourly"]["time"] = hour_times(f"{start}T08:00")
        yield f"calendar_{start}", encode(response), expected(response)

    # The same fully valid strip is rejected the moment it leaves daily[0]'s date, even
    # when it is internally consecutive and every value parses.
    for label, start in (("next_day", "2024-12-30T00:00"),
                         ("previous_day", "2024-12-28T08:00")):
        response = copy.deepcopy(base)
        response["hourly"]["time"] = hour_times(start)
        yield f"hourly_starts_{label}", encode(response), None
    for label, high, low, offset in (
        ("negative_fractional", [-1.25, -2.5, -3.75, -4.125, -5.875],
         [-10.5, -11.75, -12.25, -13.875, -14.125], -12600),
        ("zero_equal", [0] * 5, [0] * 5, 0),
        ("equal_fractional", [2.25] * 5, [2.25] * 5, 19800),
        ("finite_float_limits", [3.4028234e38] * 5, [-3.4028234e38] * 5, 28800),
    ):
        response = copy.deepcopy(base)
        response["daily"]["temperature_2m_max"] = high
        response["daily"]["temperature_2m_min"] = low
        response["utc_offset_seconds"] = offset
        yield label, encode(response), expected(response)
    exponent = copy.deepcopy(base)
    exponent["daily"]["temperature_2m_max"] = [12.5] * 5
    exponent["daily"]["temperature_2m_min"] = [-7.5] * 5
    yield "exponent_numbers", encode(exponent).replace("12.5", "1.25e+1").replace(
        "-7.5", "-7.5E0"), expected(exponent)

    # The hourly strip is part of the contract: a response without a usable one is
    # not a forecast, even when every daily field is perfect.
    for label in ("missing", "null", "short", "scalar"):
        response = copy.deepcopy(base)
        if label == "missing":
            del response["hourly"]
        elif label == "short":
            for field in response["hourly"]:
                response["hourly"][field] = response["hourly"][field][:20]
        else:
            response["hourly"] = None if label == "null" else 63
        yield f"hourly_{label}", encode(response), None

    required = (("daily",), ("utc_offset_seconds",), ("daily", "time"),
                ("daily", "temperature_2m_max"), ("daily", "temperature_2m_min"),
                ("daily", "weather_code"), ("daily", "precipitation_probability_max"))
    for path in required:
        variants = [("missing", None), ("null", None), ("array", []), ("object", {}),
                    ("number", 18.5), ("string", "18.5"), ("true", True), ("false", False)]
        if path == ("utc_offset_seconds",):
            variants += [("fractional", 19800.5), ("int_overflow", 2147483648),
                         ("int_underflow", -2147483649), ("nonfinite", float("inf"))]
        for label, value in variants:
            response = copy.deepcopy(base)
            target = response if len(path) == 1 else response[path[0]]
            if label == "missing":
                del target[path[-1]]
            else:
                target[path[-1]] = value
            # Neither unit keys nor later nested valid data may rescue a bad direct member.
            response = {**units, **response, "fallback": copy.deepcopy(base)}
            if len(path) == 2:
                response["daily"]["metadata"] = copy.deepcopy(base["daily"])
            yield "_".join((*path, label)), encode(response), None

    for field in base["daily"]:
        for length in (0, 1, 2, 3, 4, 6):
            response = copy.deepcopy(base)
            values = response["daily"][field]
            response["daily"][field] = (values + values)[:length]
            yield f"{field}_length_{length}", encode(response), None
    for length in (0, 4, 6):
        response = copy.deepcopy(base)
        for field, values in response["daily"].items():
            response["daily"][field] = (values + values)[:length]
        yield f"all_arrays_length_{length}", encode(response), None

    invalid_numbers = (
        ("null", None), ("string", "25"), ("true", True), ("false", False),
        ("object", {}), ("array", []), ("nested_number", [25]),
        ("nan", float("nan")), ("infinity", float("inf")),
        ("negative_infinity", -float("inf")), ("double_overflow", 1e300),
        ("negative_double_overflow", -1e300), ("float_overflow", 3.5e38),
        ("negative_float_overflow", -3.5e38),
    )
    invalid_tokens = ("+1", "01", "-01", "1.", ".5", "1e", "1e+", "-", "0x10",
                      "nan", "inf", "-inf", "1e309", "-1e309")
    for field in ("temperature_2m_max", "temperature_2m_min"):
        for i in range(5):
            for label, value in invalid_numbers:
                response = copy.deepcopy(base)
                response["daily"][field][i] = value
                yield f"{field}_day{i + 1}_{label}", encode(response), None
            for token in invalid_tokens:
                response = copy.deepcopy(base)
                response["daily"][field][i] = "RAW_NUMBER"
                raw = encode(response).replace('"RAW_NUMBER"', token)
                yield f"{field}_day{i + 1}_token_{token}", raw, None
    invalid_probabilities = (*invalid_numbers, ("negative", -0.01), ("above_range", 100.01),
                             ("negative_rounds_to_float_zero", -1e-100),
                             ("above_range_rounds_to_float_100", 100.000001))
    for i in range(5):
        for label, value in invalid_probabilities:
            response = copy.deepcopy(base)
            response["daily"][probability_field][i] = value
            yield f"probability_day{i + 1}_{label}", encode(response), None
        for token in invalid_tokens:
            response = copy.deepcopy(base)
            response["daily"][probability_field][i] = "RAW_PROBABILITY"
            yield f"probability_day{i + 1}_token_{token}", encode(response).replace(
                '"RAW_PROBABILITY"', token), None

    invalid_codes = (*invalid_numbers, ("negative", -1), ("above_range", 100),
                     ("fractional", 12.5), ("integral_float", 12.0),
                     ("tiny_fraction_rounded_to_zero", 1e-100),
                     ("int_overflow", 2147483648), ("int_underflow", -2147483649))
    code_tokens = (*invalid_tokens, "0.0", "1e0", "1E+0", "0e0", "1e-999",
                   "99.0000000000000000000000001", "4294967296")
    for i in range(5):
        for label, value in invalid_codes:
            response = copy.deepcopy(base)
            response["daily"]["weather_code"][i] = value
            yield f"weather_code_day{i + 1}_{label}", encode(response), None
        for token in code_tokens:
            response = copy.deepcopy(base)
            response["daily"]["weather_code"][i] = "RAW_CODE"
            yield f"weather_code_day{i + 1}_token_{token}", encode(response).replace(
                '"RAW_CODE"', token), None

    for i in range(5):
        response = copy.deepcopy(base)
        response["daily"]["temperature_2m_min"][i] = response["daily"]["temperature_2m_max"][i] + 0.25
        yield f"day{i + 1}_min_above_max", encode(response), None

    invalid_dates = (
        None, 20241229, True, False, {}, [], ["2024-12-29"], "", "2024-2-29", "2024-02-9",
        "2024/12/29", "2024-12-29T00:00", "2024-12-29Z", " 2024-12-29", "2024-12-29 ",
        "2024-12-29\n", "2024-12-29\x00", "2024-12-290", "24-12-29", "10000-01-01",
        "0000-01-01", "2024-00-01", "2024-13-01", "2024-01-00", "2024-01-32",
        "2024-04-31", "2024-06-31", "2024-09-31", "2024-11-31", "2025-02-29",
        "1900-02-29", "2100-02-29", "2024-02-30", "2024-aa-29", "２０２４-12-29",
    )
    for i in range(5):
        for n, value in enumerate(invalid_dates):
            response = copy.deepcopy(base)
            response["daily"]["time"][i] = value
            yield f"time_day{i + 1}_invalid_{n}", encode(response), None
        response = copy.deepcopy(base)
        response["daily"]["time"][i] = "2025-04-01"
        yield f"time_day{i + 1}_nonconsecutive", encode(response), None
        if i:
            response = copy.deepcopy(base)
            response["daily"]["time"][i] = response["daily"]["time"][i - 1]
            yield f"time_day{i + 1}_duplicate", encode(response), None
            response = copy.deepcopy(base)
            for j in range(i, 5):
                response["daily"]["time"][j] = (
                    date.fromisoformat(response["daily"]["time"][j]) + timedelta(days=1)).isoformat()
            yield f"time_day{i + 1}_gap", encode(response), None
    response = copy.deepcopy(base)
    response["daily"]["time"].reverse()
    yield "time_descending", encode(response), None

    raw = encode(base)
    # Every proper prefix must fail, including cuts after otherwise complete data arrays.
    for end in range(len(raw)):
        yield f"truncated_json_{end}", raw[:end], None
    for label, malformed in (
        ("root_array", "[" + raw + "]"), ("root_string", encode(raw)),
        ("root_null", "null"), ("trailing_value", raw + " false"),
        ("trailing_garbage", raw + " garbage"), ("trailing_comma", raw[:-1] + ",}"),
        ("missing_comma", raw.replace(',"daily"', '"daily"', 1)),
        ("bad_date_escape", raw.replace("2024-12-29", r"2024-12-2\q")),
        ("bad_date_control", raw.replace("2024-12-29", "2024-12-2\x01")),
    ):
        yield label, malformed, None


BUTTON_HARNESS = r'''
#include <stdbool.h>
#include <stdio.h>
#include "button.h"
#include "driver/gpio.h"

static int level = 1; /* released: the button is active low */

int gpio_config(const gpio_config_t *cfg) { (void)cfg; return 0; }
int gpio_get_level(int pin) { (void)pin; return level; }

/* Each feed() is a run of BUTTON_POLL_MS samples at one level. */
static void feed(bool pressed, unsigned ms, const char *label) {
    level = pressed ? 0 : 1;
    for (unsigned t = 0; t < ms; t += BUTTON_POLL_MS) {
        button_event_t event = button_poll(BUTTON_POLL_MS);
        if (event != BUTTON_NONE) {
            const char *name = event == BUTTON_TAP ? "tap" : "hold";
            printf("event:%s@%s\n", name, label);
        }
    }
}

int main(void) {
    button_init();
    feed(false, 200, "idle");
    feed(true, 40, "debounce");
    feed(true, 4980, "before-threshold");
    feed(true, 20, "threshold");   /* one HOLD at exactly 5 s after debounce */
    feed(true, 200, "still-held");
    feed(false, 200, "hold-release");
    feed(true, 100, "tap");
    feed(false, 200, "tap-release");
    feed(true, 5000, "below");     /* 4.98 s after debounce: a TAP on release */
    feed(false, 200, "below-release");
    feed(true, 20, "bounce");      /* single 20 ms samples never settle */
    feed(false, 20, "bounce");
    feed(true, 20, "bounce");
    feed(false, 120, "bounce");
    return 0;
}
'''


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def run(command, work, timeout):
    return subprocess.run(
        [str(arg) for arg in command], cwd=work, capture_output=True,
        env=dict(os.environ, LC_ALL="C", TZ="UTC0"), check=True, timeout=timeout,
    ).stdout


def locate_cjson(project):
    root = project / "managed_components" / "espressif__cjson"
    candidates = sorted(
        path for path in root.rglob("cJSON.c")
        if (path.parent / "cJSON.h").is_file()
    )
    require(
        len(candidates) == 1,
        "managed espressif/cjson is missing; run `idf.py reconfigure` or `idf.py build` first",
    )
    return candidates[0].parent, candidates[0]


def compile_app(compiler, source, cjson_dir, cjson_source, work, label):
    sources = list(SOURCES)
    for name in sources:
        require((source / name).is_file(), f"missing source: {source / name}")
    executable = work / f"{label}.out"
    run([
        compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra",
        "-Wno-unused-parameter", "-Wno-unused-variable",
        "-I", work / "stubs", "-I", source, "-I", cjson_dir,
        *(source / name for name in sources), cjson_source, work / "harness.c",
        "-lm", "-o", executable,
    ], work, 60)
    return executable


def compile_weather(compiler, source, cjson_dir, cjson_source, work):
    executable = work / "weather.out"
    run([
        compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra",
        "-Wno-unused-parameter", "-Wno-unused-variable",
        "-I", work / "stubs", "-I", source, "-I", cjson_dir,
        source / "weather.c", source / "weather_json.c", cjson_source, work / "weather_harness.c",
        "-lm", "-o", executable,
    ], work, 60)
    return executable


def compile_ui(compiler, source, work):
    executable = work / "ui.out"
    run([
        compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra",
        "-Wno-unused-parameter", "-Wno-unused-variable", "-Wno-unused-function",
        "-I", work / "stubs", "-I", source,
        work / "ui_wrapper.c", source / "epd.c", source / "fonts_aa.c", source / "font_clock.c", source / "font_icons.c",
        work / "ui_harness.c", "-lm", "-o", executable,
    ], work, 60)
    return executable


VOICE_AUDIO_HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include "voice_audio.h"

int main(void)
{
    /* The microphone is on the left channel, so the right channel must be dropped, not mixed. */
    const int16_t stereo[6] = {1, 9, 2, 8, 3, 7};
    int16_t mono[3] = {0};
    voice_stereo_to_mono(stereo, mono, 3);
    if (mono[0] != 1 || mono[1] != 2 || mono[2] != 3) {
        printf("downmix %d %d %d\n", mono[0], mono[1], mono[2]);
        return 1;
    }

    /* Playback needs interleaved stereo again: both channels carry the same sample. */
    int16_t out[6] = {0};
    voice_mono_to_stereo(mono, out, 3);
    for (int i = 0; i < 3; i++) {
        if (out[i * 2] != mono[i] || out[i * 2 + 1] != mono[i]) {
            printf("upmix index %d\n", i);
            return 1;
        }
    }

    printf("ok\n");
    return 0;
}
'''


def compile_voice_audio(compiler, source, work):
    (work / "voice_audio_harness.c").write_text(VOICE_AUDIO_HARNESS, encoding="utf-8")
    executable = work / "voice_audio.out"
    run([
        compiler, "-std=c11", "-Wall", "-Wextra", "-I", source,
        source / "voice_audio.c", work / "voice_audio_harness.c", "-o", executable,
    ], work, 60)
    return executable


def check_voice_audio(executable, work):
    """Stereo/mono conversion of the real voice_audio.c."""
    out = run([executable], work, 10).decode("utf-8").strip()
    require(out == "ok", f"voice_audio: expected ok, got {out!r}")


def compile_button(compiler, source, work):
    executable = work / "button.out"
    run([
        compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra",
        "-Wno-unused-parameter", "-I", work / "stubs", "-I", source,
        source / "button.c", work / "button_harness.c", "-o", executable,
    ], work, 60)
    return executable


def check_button(executable, work):
    """Debounce and hold timing of the real button state machine."""
    trace = run([executable], work, 10).decode("utf-8").splitlines()
    require(trace == ["event:hold@threshold", "event:tap@tap-release", "event:tap@below-release"],
            f"button: expected one hold at 5 s, taps on shorter releases and no events during bounce; got {trace}")


def check_button_reset(executable, work):
    """Drive a held button through reset, then simulate completing setup."""
    trace = run([executable, "S", 0, 0, "hold"], work, 10).decode("ascii").splitlines()
    events = [line for line in trace if not line.startswith("frame:")]
    require(events[-6:] == ["reset_notice_frame", "forget", "reset_notice_wait",
                            "portal_hint_frame", "portal", "connect"],
            f"button reset: expected reset screen before erase and setup before reconnect; got {events}")


def validate_weather(trace, case, expected):
    actual = json.loads(trace)
    context = f"current/weather/{case}"
    success = expected is not None
    require(actual["result"] == success and actual["ok"] == success,
            f"{context}: expected return={str(success).lower()}, w.ok={str(success).lower()}; "
            f"got return={actual['result']}, w.ok={actual['ok']}")
    if not success:
        return
    require(actual["utc_offset"] == expected["utc_offset"],
            f"{context}: wrong UTC offset: {actual['utc_offset']}")
    require(len(actual["days"]) == 5, f"{context}: expected all five parsed days")
    for i, (got, want) in enumerate(zip(actual["days"], expected["days"]), 1):
        require(got["date"] == want["date"],
                f"{context}: day {i}: expected date={want['date']}, got {got['date']}")
        require(got["code"] == want["code"],
                f"{context}: day {i}: expected code={want['code']}, got {got['code']}")
        for field in ("tmax", "tmin", "precip_probability"):
            require(math.isfinite(got[field]) and math.isclose(
                got[field], want[field], rel_tol=2e-7, abs_tol=1e-6),
                f"{context}: day {i}: expected {field}={want[field]}, got {got[field]}")


def ui_capture(executable, work, mode, day=None, value=None):
    command = [executable, mode] + ([] if day is None else [day])
    if value is not None:
        if mode == "range":
            command.extend(value)
        else:
            command.append(value)
    trace = run(command, work, 10)
    texts, frames = [], []
    for line in trace.decode("utf-8").splitlines():
        if line.startswith("text:"):
            _, x, y, width, font, left, top, right, bottom, text = line.split(":", 9)
            texts.append({"x": int(x), "y": int(y), "width": int(width), "font": font, "text": text,
                          "bounds": (int(left), int(top), int(right), int(bottom))})
        elif line.startswith("status"):
            actual.append("status")
        elif line.startswith("frame:"):
            frames.append(bytes.fromhex(line[6:]))
        else:
            raise AssertionError(f"ui/{mode}: unexpected trace: {line}")
    require(len(frames) == 1 and len(frames[0]) == FRAME_BYTES,
            f"ui/{mode}: expected one complete real SPI framebuffer")
    frame = frames[0]
    # epd.h's fixed landscape/CW panel mapping; the body layout is not duplicated.
    ink = {(x, y) for x in range(VISIBLE_W) for y in range(128)
           if not frame[x * 16 + (127 - y) // 8] & (0x80 >> ((127 - y) & 7))}
    return {"frame": frame, "texts": texts, "ink": ink}


def region(capture, y0, y1, x0=0, x1=VISIBLE_W):
    return {(x, y) for x, y in capture["ink"] if x0 <= x < x1 and y0 <= y < y1}


def expected_range(low, high, metrics, available=True):
    """Construct the low°-high° text and select a font independently of ui.c."""
    # The UI receives float32 readings. Use their exact value before matching C's
    # %.0f formatting, including negative zero and ties-to-even, not roundf().
    low = struct.unpack("f", struct.pack("f", low))[0]
    high = struct.unpack("f", struct.pack("f", high))[0]
    text = f"{low:.0f}°-{high:.0f}°" if (
        available and math.isfinite(low) and math.isfinite(high) and low <= high) else "--"

    def width(font, value):
        return sum(metrics[font][character] for character in value)

    font = "temp" if width("temp", text) <= 48 else "small"
    if width(font, text) > 48:
        text, font = "--", "temp"
    return text, font, width(font, text)


def probability_text(value, available=True):
    if not available or not math.isfinite(value) or not 0 <= value <= 100:
        return "--%"
    # Percentages are nonnegative: floor(x + .5) matches C roundf, not Python's round.
    return f"{math.floor(value + 0.5)}%"


def retry_seconds(failures):
    """Independent copy of the public retry schedule: 5, 10, 20, 40, 60, 60..."""
    delay = 5
    while failures > 1 and delay < 60:
        delay *= 2
        failures -= 1
    return min(delay, 60)


def expected_advice(code, probability, available=True):
    """Independent contract oracle; never call the production recommendation code."""
    if probability_text(probability, available) == "--%":
        return "预报待更新"
    if code in (95, 96, 99):
        return "雷雨注意安全"
    if (math.floor(probability + 0.5) >= 51):
        return "今日带伞"
    known_codes = {0, 1, 2, 3, 45, 48, 51, 53, 55, 56, 57, 61, 63, 65, 66, 67,
                   71, 73, 75, 77, 80, 81, 82, 85, 86}
    if code in known_codes:
        return "降水概率低"
    return "预报待更新"


def check_icon_fonts(executable, work, source):
    """Check header aliases against official names, then inspect the real linked glyphs."""
    definitions = re.findall(
        r'#define\s+(WI_\w+)\s+"([^"]+)"\s*//\s*(wi-[\w-]+),\s*U\+([0-9A-F]+)',
        (source / "font_icons.h").read_text(encoding="utf-8"))
    require(len(definitions) == len(ICON_ORACLE) and
            {item[0] for item in definitions} == ICON_ORACLE.keys(),
            "ui/icons: expected all eleven documented WI_* macros")
    for macro, encoded, official, cp in definitions:
        name, expected_cp, _ = ICON_ORACLE[macro]
        expected_bytes = "".join(f"\\x{byte:02X}" for byte in chr(expected_cp).encode("utf-8"))
        require((official, int(cp, 16), encoded.upper()) == (name, expected_cp, expected_bytes.upper()),
                f"ui/icons: {macro} must be {name} U+{expected_cp:04X}, including its UTF-8 bytes")

    fonts = json.loads(run([executable, "icon_metrics"], work, 10))
    cps = sorted(cp for _, cp, _ in ICON_ORACLE.values())
    for name, advance, height in (("icon_daily", 40, 27), ("icon", 26, 23)):
        font = fonts[name]
        require(font["height"] == height and [g["cp"] for g in font["glyphs"]] == cps,
                f"ui/{name}: wrong height, missing glyphs or unsorted/duplicate codepoints")
        offset = 0
        for g in font["glyphs"]:
            context = f"ui/{name}/U+{g['cp']:04X}"
            require(g["adv"] == advance and 0 < g["w"] <= advance and 0 < g["h"] <= height,
                    f"{context}: wrong fixed advance or glyph size")
            require(g["offset"] == offset and g["xoff"] == (advance - g["w"]) // 2 and
                    g["yoff"] == -height + (height - g["h"]) // 2,
                    f"{context}: noncontiguous bitmap or incorrect centering offsets")
            offset += ((g["w"] + 7) // 8) * g["h"]
            require(g["ink"] and all(0 <= x < advance and -height <= y < 0 for x, y in g["ink"]),
                    f"{context}: blank or out-of-cell bitmap")
            # Ink, not just the declared rectangle, must be centered (half-pixel rounding).
            xs, ys = zip(*g["ink"])
            require(abs((min(xs) + max(xs) + 1) / 2 - advance / 2) <= 0.5 and
                    abs((min(ys) + max(ys) + 1) / 2 + height / 2) <= 0.5,
                    f"{context}: visible ink is not centered")
        require(len({tuple(map(tuple, g["ink"])) for g in font["glyphs"]}) == 11,
                f"ui/{name}: all eleven real glyph bitmaps must differ")
    for daily, hourly in zip(fonts["icon_daily"]["glyphs"], fonts["icon"]["glyphs"]):
        require(daily["w"] > hourly["w"] and daily["h"] > hourly["h"],
                f"ui/icons: U+{daily['cp']:04X} must have distinct daily/hourly sizes")
    return {name: {g["cp"]: g for g in font["glyphs"]} for name, font in fonts.items()}


def check_ui(executable, work, source):
    """Assert the public layout bands, not a duplicate implementation of the icons."""
    count = 1
    clock_metrics = run([executable, "clock_metrics"], work, 10).decode("ascii").strip().split(":")
    require(len(clock_metrics) == 4 and clock_metrics[:2] == ["clock", "1440"] and
            int(clock_metrics[3]) > int(clock_metrics[2]),
            "ui: all 1440 clock times must fit; bold clock glyphs must have more ink than regular")
    centers = [18 + 26 + i * 52 for i in range(5)]
    boundaries = [18 + i * 52 for i in range(6)]
    metrics = json.loads(run([executable, "metrics"], work, 10))
    icon_fonts = check_icon_fonts(executable, work, source)
    codepoints = {code: cp for _, cp, codes in ICON_ORACLE.values() for code in codes}

    clock = time.strftime("%H:%M", time.gmtime(1730622600))

    # Row anchors of the five-day page (main/ui.c) with the exact pixel span each row may
    # paint, so a row that silently moves is a failure instead of a shrug: 今天 is the
    # tallest date label (29..43), icons run 46..72, temperatures 77..88 and
    # probabilities 94..102.
    DATE_BASE, TEMP_BASE, PRECIP_BASE = 42, 89, 103
    ICON_BAND = (46, 73)
    DATE_BAND = (29, 45)
    TEMP_BAND = (75, 91)
    PRECIP_BAND = (92, 106)

    def header(result, advice, available=True, long_city=False):
        entries = [e for e in result["texts"] if e["y"] <= 21]
        banners = [e for e in entries if e["text"] == advice]
        require(len(entries) == 3 and len(banners) == 1,
                f"ui: expected exactly city/advice/clock, with banner {advice!r}; got {entries}")
        banner = banners[0]
        require(banner["y"] == 16 and banner["font"] == "mid" and
                abs(banner["x"] + banner["width"] / 2 - 174) <= 0.5 and
                110 <= banner["x"] and banner["x"] + banner["width"] <= 250,
                "ui: today's advice must be centered at (174,16), without city/clock overlap")
        cities = [e for e in entries if e["x"] == 8 and e["y"] == 16 and e["font"] == "mid"]
        require(len(cities) == 1 and cities[0]["width"] <= 96,
                "ui: city must remain at (8,16) and fit within 96px")
        city = cities[0]["text"]
        if long_city:
            suffix = "..." if city.endswith("...") else "…" if city.endswith("…") else ""
            require(suffix and city[:-len(suffix)] and
                    "ABCDEFGHIJKLMNOPQRSTUVW".startswith(city[:-len(suffix)]),
                    f"ui: oversized city must be shortened with an ellipsis, got {city!r}")
        else:
            require(city == ("Chengdu" if available else "无数据"),
                    f"ui: unexpected normal/no-forecast city: {city!r}")
        require(any(e["text"] == clock and e["font"] == "clock" and e["y"] == 16 and
                    e["x"] + e["width"] == 292 and e["x"] >= 250 and
                    250 <= e["bounds"][0] <= e["bounds"][2] < 292 and
                    0 <= e["bounds"][1] <= e["bounds"][3] < 22 for e in entries),
                "ui: clock text must use the bold clock font and stay right-aligned")

    def capture(mode, day=None, value=None):
        nonlocal count
        count += 1
        result = ui_capture(executable, work, mode, day, value)
        require(not any(e["font"] == "huge" for e in result["texts"]),
                f"ui/{mode}: the forecast-first layout must not use large sensor text")
        require(all((x, y) in result["ink"] for y in (22, 108) for x in range(6, 294)),
                f"ui/{mode}: expected unchanged header line22 and footer divider line108")
        available = not (mode.startswith("no_forecast") or mode == "weather_null")
        today_code, today_probability = 0, 10.25
        if mode == "all_code":
            today_code = int(day)
        elif mode == "policy":
            today_code, today_probability = int(day), float(value)
        elif day == 0 and mode == "day_code":
            today_code = int(value)
        elif day == 0 and mode == "day_prob":
            today_probability = float(value)
        header(result, expected_advice(today_code, today_probability, available),
               available, mode == "city_long")
        return result

    def footer(result, temperature="--.- °C", humidity="-- %"):
        entries = [e for e in result["texts"] if e["y"] >= 109]
        expected = {8: "温度", 46: temperature, 110: "湿度", 146: humidity}
        require(len(entries) == 4 and {e["x"]: e["text"] for e in entries} == expected,
                f"ui: expected footer {expected}, got {entries}")
        require(all(e["y"] == 124 and e["font"] == "mid" for e in entries),
                "ui: footer labels/values must use font_mid at baseline124, never font downgrade")
        require(all(e["width"] <= (100 if e["x"] == 46 else 96)
                    for e in entries if e["x"] in (46, 198)),
                "ui: footer values exceeding their fixed width must fall back to unknown")

    def icon(result, day):
        # Normalize actual rendered pixels to a column's center; shapes stay in ui.c.
        return frozenset((x - centers[day], y) for x, y in region(
            result, ICON_BAND[0], ICON_BAND[1], boundaries[day], boundaries[day + 1]))

    def probability(result, day, value, available=True):
        entries = [e for e in result["texts"] if e["y"] == PRECIP_BASE and
                   boundaries[day] <= e["x"] + e["width"] / 2 < boundaries[day + 1]]
        expected = probability_text(value, available)
        require(len(entries) == 1 and entries[0]["text"] == expected and
                entries[0]["font"] == "small" and entries[0]["width"] <= 48 and
                abs(entries[0]["x"] + entries[0]["width"] / 2 - centers[day]) <= 0.5,
                f"ui: day {day + 1}: expected centered probability {expected!r} at "
                f"baseline{PRECIP_BASE}; got {entries}")

    def advice_pixel(x, y):
        return 0 <= y < 22 and 110 <= x < 250

    forecast = capture("forecast")
    footer(forecast)
    long_city = capture("city_long")
    require(region(long_city, 22, 128) == region(forecast, 22, 128) and
            region(long_city, 0, 22, 110, VISIBLE_W) == region(forecast, 0, 22, 110, VISIBLE_W),
            "ui: city ellipsis must not overwrite the advice, clock or forecast/footer")
    for mode in ("sensor_null", "sensor_invalid_poison"):
        result = capture(mode)
        footer(result)
        require(result["frame"] == forecast["frame"],
                f"ui/{mode}: invalid/NULL sensors must display identical unknown readings")
    require(not any(re.search(r"\d", e["text"]) for e in forecast["texts"] if e["y"] >= 109),
            "ui: default footer must not contain forecast or dummy sensor numbers")

    # Today is a fixed 今天 label in font_mid; the other columns are MM/DD in font_small.
    labels = (("今天", "mid"), ("12/30", "small"), ("12/31", "small"),
              ("01/01", "small"), ("01/02", "small"))
    high = (25.25, 18.75, -1.25, 3.75, 12.25)
    low = (12.25, -2.75, -15.25, -8.75, 0.25)
    codes = (0, 2, 45, 61, 95)
    probabilities = (10.25, 39.6, 40, 80.75, 100)
    for i in range(5):
        probability(forecast, i, probabilities[i])
        headers = [e for e in forecast["texts"] if e["text"] == labels[i][0]]
        require(len(headers) == 1 and headers[0]["y"] == DATE_BASE and
                headers[0]["font"] == labels[i][1] and
                abs(headers[0]["x"] + headers[0]["width"] / 2 - centers[i]) <= 0.5,
                f"ui: day {i + 1}: date label {labels[i][0]!r} must be centered at "
                f"baseline{DATE_BASE} in font_{labels[i][1]}")
        expected_text, expected_font, expected_width = expected_range(low[i], high[i], metrics)
        ranges = [e for e in forecast["texts"] if e["y"] == TEMP_BASE and
                  boundaries[i] <= e["x"] + e["width"] / 2 < boundaries[i + 1]]
        require(len(ranges) == 1 and ranges[0]["text"] == expected_text and
                ranges[0]["font"] == expected_font and ranges[0]["width"] == expected_width and
                abs(ranges[0]["x"] + ranges[0]["width"] / 2 - centers[i]) <= 0.5,
                f"ui: day {i + 1}: expected one single-line low°-high° range {expected_text!r} "
                f"in font_{expected_font} at baseline{TEMP_BASE}; got {ranges}")
    require(boundaries[0] == (VISIBLE_W - 5 * 52) // 2 and
            boundaries[-1] == VISIBLE_W - boundaries[0] and
            all(centers[i + 1] - centers[i] == 52 for i in range(4)),
            "ui: five forecast columns must be equally spaced and centered")

    # Every body row sits on one of the contract baselines: no stale row survives, and
    # high and low share the single temperature row instead of two stacked ones.
    require({e["y"] for e in forecast["texts"] if e["font"] not in ("icon_daily", "icon")} ==
            {16, DATE_BASE, TEMP_BASE, PRECIP_BASE, 124},
            "ui: five-day text must only use the city/date/temperature/probability/footer "
            f"baselines, got {sorted({e['y'] for e in forecast['texts']})}")

    # Every date/range affects only its own grid cell, never icons/probability/footer.
    bands = {"date": DATE_BAND, "max": TEMP_BAND, "min": TEMP_BAND}
    for i in range(5):
        for field in ("date", "max", "min"):
            changed = capture(f"day_{field}", i)
            delta = forecast["ink"] ^ changed["ink"]
            if field == "date" and i == 0:
                # Column one is the fixed 今天 label, so its date string is invisible.
                require(not delta and any(e["text"] == "今天" and e["y"] == DATE_BASE
                                         for e in changed["texts"]),
                        "ui: day 1 must keep the 今天 label whatever its date string is")
                continue
            y0, y1 = bands[field]
            require(delta and all(boundaries[i] <= x < boundaries[i + 1] and y0 <= y < y1
                                  for x, y in delta),
                    f"ui: day {i + 1} {field} must change only its own visible grid cell")
            if field == "date":
                require(any(e["text"] == "09/19" and e["y"] == DATE_BASE and
                            e["font"] == "small" for e in changed["texts"]),
                        f"ui: day {i + 1} date change is not drawn")
                continue
            tmin, tmax = low[i], high[i]
            if field == "max":
                tmax = 38.25
            else:
                tmin = -29.25
            expected_text, expected_font, expected_width = expected_range(tmin, tmax, metrics)
            require(any(e["y"] == TEMP_BASE and e["text"] == expected_text and
                        e["font"] == expected_font and e["width"] == expected_width and
                        boundaries[i] <= e["x"] + e["width"] / 2 < boundaries[i + 1]
                        for e in changed["texts"]),
                    f"ui: day {i + 1} {field} change {expected_text!r} is not drawn at "
                    f"baseline{TEMP_BASE}")

    # Font fallback, invalid pairs and formatting come from an independent oracle.
    ranges = ((-12.0, -3.0), (-999.0, -99.0), (-0.4, 0.4), (-0.6, 0.6),
              (12.25, 3.75), (float("nan"), 10), (10, float("inf")),
              (-float("inf"), 10), (1e30, 1e30))
    for i in range(5):
        for tmin, tmax in ranges:
            changed = capture("range", i, (tmin, tmax))
            expected_text, expected_font, expected_width = expected_range(tmin, tmax, metrics)
            entries = [e for e in changed["texts"] if e["y"] == TEMP_BASE and
                       boundaries[i] <= e["x"] + e["width"] / 2 < boundaries[i + 1]]
            require(len(entries) == 1 and entries[0]["text"] == expected_text and
                    entries[0]["font"] == expected_font and
                    entries[0]["width"] == expected_width and
                    abs(entries[0]["x"] + entries[0]["width"] / 2 - centers[i]) <= 0.5,
                    f"ui: day {i + 1} range ({tmin}, {tmax}) must be one {expected_font} "
                    f"text {expected_text!r} at baseline{TEMP_BASE}; got {entries}")
            delta = forecast["ink"] ^ changed["ink"]
            require(delta and all(boundaries[i] <= x < boundaries[i + 1] and
                                  TEMP_BAND[0] <= y < TEMP_BAND[1] for x, y in delta),
                    f"ui: day {i + 1} range ({tmin}, {tmax}) must repaint only its own "
                    f"y{TEMP_BAND[0]}..{TEMP_BAND[1] - 1} cell")

    # Render references via the public API, not copies of sun/cloud/rain algorithms.
    # All eleven families and unknown in-/out-of-range codes must be pixel-distinct.
    references = {}
    for code in (0, 2, 3, 45, 51, 61, 71, 80, 85, 95, 96, 4, 100, -1):
        result = capture("all_code", code)
        expected = icon(result, 0)
        require(expected and all(icon(result, i) == expected for i in range(5)),
                f"ui: code {code}: all five columns must render the same nonblank icon")
        delta = forecast["ink"] ^ result["ink"]
        require(all((boundaries[0] <= x < boundaries[-1] and
                     ICON_BAND[0] <= y < ICON_BAND[1]) or
                    advice_pixel(x, y) for x, y in delta),
                f"ui: changing weather codes may affect only icons and today's advice, code={code}")
        references[code] = expected
        if code in (4, 100, -1):
            questions = [e for e in result["texts"] if e["text"] == "?" and
                         ICON_BAND[0] <= e["y"] < ICON_BAND[1]]
            require(len(questions) == 5 and all(
                sum(boundaries[i] <= e["x"] + e["width"] / 2 < boundaries[i + 1]
                    for e in questions) == 1 for i in range(5)),
                f"ui: unknown code {code} must draw a visible ? in each column")
    require(len({references[c] for c in (0, 2, 3, 45, 51, 61, 71, 80, 85, 95, 96, -1)}) == 12,
            "ui: all eleven weather families and ? must be distinguishable")
    require(references[4] == references[100] == references[-1] and
            all(references[c] != references[4] for c in (0, 2, 3, 45, 51, 61, 71, 95)),
            "ui: unknown codes must use ?, never a known-weather or sunny icon")
    for i in range(5):
        require(icon(forecast, i) == references[codes[i]],
                f"ui: day {i + 1}: icon must reflect its own code {codes[i]}")
        for code, expected in references.items():
            changed = capture("day_code", i, code)
            delta = forecast["ink"] ^ changed["ink"]
            body_delta = {(x, y) for x, y in delta if y >= 22}
            require(icon(changed, i) == expected and
                    bool(body_delta) == (expected != references[codes[i]]) and
                    all((boundaries[i] <= x < boundaries[i + 1] and
                         ICON_BAND[0] <= y < ICON_BAND[1]) or
                        (i == 0 and advice_pixel(x, y)) for x, y in delta),
                    f"ui: day {i + 1} code={code} may change only its own icon and, for today, advice")

    # Each day's numeric probability is independently visible. Only TODAY may also
    # change the recommendation; tomorrow's high/unknown probability never does.
    for i in range(5):
        for value in (0, 0.5, 39.5, 49.4, 49.5, 49.6, 50, 50.5, 90.5, 100,
                      float("nan"), float("inf"), -0.1, 100.1):
            changed = capture("day_prob", i, value)
            probability(changed, i, value)
            delta = forecast["ink"] ^ changed["ink"]
            body_delta = {(x, y) for x, y in delta if y >= 22}
            require(bool(body_delta) == (probability_text(value) != probability_text(probabilities[i])) and
                    all((boundaries[i] <= x < boundaries[i + 1] and
                         PRECIP_BAND[0] <= y < PRECIP_BAND[1]) or
                        (i == 0 and advice_pixel(x, y)) for x, y in delta),
                    f"ui: day {i + 1} probability={value} may change only its probability cell / today's advice")

    # Explicit policy examples are independent of both production code and the
    # capture helper's oracle. Future days include a 100% thunderstorm on purpose.
    policy_cases = []
    for code in (0, 1, 2, 3, 45, 48):
        policy_cases += [(code, 49.4, "降水概率低"), (code, 49.6, "降水概率低"),
                         (code, 50.4, "降水概率低"), (code, 50.5, "今日带伞")]
    policy_cases += [(0, 0, "降水概率低"), (0, 39.6, "降水概率低"),
                     (0, 49.5, "降水概率低"), (0, 50, "降水概率低"),
                     (0, 50.4, "降水概率低"), (0, 50.5, "今日带伞"), (0, 51, "今日带伞")]
    for code in (51, 53, 55, 56, 57, 61, 63, 65, 66, 67, 71, 73, 75, 77, 80, 81, 82, 85, 86):
        policy_cases += [(code, 0, "降水概率低"), (code, 41, "降水概率低"),
                         (code, 49.6, "降水概率低"), (code, 50.5, "今日带伞")]
    for code in (95, 96, 99):
        policy_cases += [(code, value, "雷雨注意安全") for value in (0, 39.4, 49.6, 100)]
    for code in (4, 44, 49, 52, 54, 58, 62, 64, 68, 72, 74, 76, 78, 83, 84, 87, 97, 98, 100, -1):
        policy_cases += [(code, 0, "预报待更新"), (code, 39.6, "预报待更新"),
                         (code, 49.6, "预报待更新"), (code, 50.5, "今日带伞")]
    for code in (0, 45, 61, 71, 95, 96, 99, 4):
        policy_cases += [(code, value, "预报待更新")
                         for value in (float("nan"), float("inf"), -0.1, 100.1)]
    for code, value, advice in policy_cases:
        changed = capture("policy", code, value)
        header(changed, advice)
        probability(changed, 0, value)
        delta = forecast["ink"] ^ changed["ink"]
        require(all((boundaries[0] <= x < boundaries[1] and
                     (ICON_BAND[0] <= y < ICON_BAND[1] or
                      PRECIP_BAND[0] <= y < PRECIP_BAND[1])) or advice_pixel(x, y)
                    for x, y in delta),
                f"ui: today's code={code}/probability={value} must not alter other days or footer")

    absent = capture("no_forecast")
    footer(absent)
    require(capture("no_forecast_poison")["frame"] == absent["frame"],
            "ui: invalid weather must not expose stale dates, temperatures or weather codes")
    require(capture("weather_null")["frame"] == absent["frame"],
            "ui: NULL weather must show the same pending advice and unknown forecast")
    grid = [e for e in absent["texts"] if 24 <= e["y"] <= 107]
    require(sum(e["text"] == "--/--" and e["y"] == DATE_BASE and e["font"] == "small"
                for e in grid) == 5,
            "ui: absent forecast must still show five unknown dates")
    require(sum(e["text"] == "--" and e["y"] == TEMP_BASE and e["font"] == "temp"
                for e in grid) == 5,
            "ui: absent forecast must show five unknown single-line ranges")
    for i in range(5):
        cells = [e for e in grid if boundaries[i] <= e["x"] + e["width"] / 2 < boundaries[i + 1]]
        probability(absent, i, 0, available=False)
        require(sum(e["text"] == "--/--" and e["y"] == DATE_BASE for e in cells) == 1 and
                sum(e["text"] == "--" and e["y"] == TEMP_BASE for e in cells) == 1,
                f"ui: missing forecast day {i + 1} needs its own date and range placeholders")
        require(icon(absent, i) == references[4] and icon(absent, i) != references[0],
                f"ui: missing forecast day {i + 1} must show ?, not pretend to be sunny")
    require(region(absent, 109, 128) == region(forecast, 109, 128),
            "ui: absent forecast must retain the same unknown sensor footer")
    require(not any("无网络" in e["text"] and e["y"] > 22 for e in absent["texts"]),
            "ui: no-forecast body must be the grid, not a central no-network message")

    valid = capture("sensor_valid")
    footer(valid, "21.4 °C", "65 %")
    delta = forecast["ink"] ^ valid["ink"]
    require(delta and all(y >= 109 for x, y in delta),
            "ui: valid sensors may change only the footer, never the forecast or header")
    for mode, temperature, humidity, x0, x1 in (
        ("sensor_temp", "-8.7 °C", "65 %", 46, 146),
        ("sensor_humidity", "21.4 °C", "43 %", 146, 198),
        ("sensor_sample", "23.4 °C", "56 %", 46, 198),
        ("sensor_temp_wide", "--.- °C", "65 %", 46, 146),
        ("sensor_temp_nan", "--.- °C", "65 %", 46, 146),
        ("sensor_temp_inf", "--.- °C", "65 %", 46, 146),
        ("sensor_humidity_nan", "21.4 °C", "-- %", 146, 198),
        ("sensor_humidity_negative", "21.4 °C", "-- %", 146, 198),
        ("sensor_humidity_over100", "21.4 °C", "-- %", 146, 198),
    ):
        changed = capture(mode)
        footer(changed, temperature, humidity)
        delta = valid["ink"] ^ changed["ink"]
        require(delta and all(109 <= y < 128 and x0 <= x < x1 for x, y in delta),
                f"ui/{mode}: sensor change must affect only its own fixed footer area")
    no_forecast_sensor = capture("no_forecast_sensor")
    footer(no_forecast_sensor, "21.4 °C", "65 %")
    require(region(no_forecast_sensor, 109, 128) == region(valid, 109, 128),
            "ui: real sensors must work independently of forecast availability")
    require(region(no_forecast_sensor, 0, 109) == region(absent, 0, 109),
            "ui: sensor data must not turn missing forecast values or icons into known weather")

    # Footer status corner: short text, font_mid, right of the sensors and right-aligned to
    # the visible edge, so it can never collide with the temperature/humidity values.
    statuses = {
        "status_connecting": "正在联网",
        "status_fetching": "正在取数",
        "status_syncing": "同步时间",
        "status_updated": "已更新",
        "status_retry": "重试 #12",
    }
    for mode, text_expected in statuses.items():
        count += 1
        result = ui_capture(executable, work, mode)
        entries = [e for e in result["texts"] if e["y"] == 124]
        found = [e for e in entries if e["text"] == text_expected]
        require(len(found) == 1,
                f"ui/{mode}: expected status {text_expected!r} on the footer row, got "
                f"{[e['text'] for e in entries]}")
        entry = found[0]
        require(entry["font"] == "mid" and entry["x"] >= 200 and
                entry["x"] + entry["width"] == 290 and entry["width"] <= 90,
                f"ui/{mode}: status must be font_mid, right-aligned to 290 and clear of the "
                f"sensors; got {entry}")
    # Hourly page: ten equal columns, one row per quantity, ink never leaves the grid.
    count += 1
    hourly = ui_capture(executable, work, "hourly")
    header(hourly, "未来10小时")
    hour_centers = [8 + 14 + i * 28 for i in range(10)]
    labels = [e for e in hourly["texts"] if e["y"] == DATE_BASE]
    require([e["text"] for e in labels] == [f"{(8 + i) % 24:02d}" for i in range(10)],
            f"ui/hourly: expected ten hour labels, got {[e['text'] for e in labels]}")
    for i, entry in enumerate(labels):
        require(entry["font"] == "small" and
                abs(entry["x"] + entry["width"] / 2 - hour_centers[i]) <= 0.5,
                f"ui/hourly: hour {i} must be centered at {hour_centers[i]} in font_small; "
                f"got {entry}")
    temps = [e for e in hourly["texts"] if e["y"] == TEMP_BASE]
    # The fixture is 20.25 + 0.5*i, so %.0f rounds to alternating whole degrees.
    require([e["text"] for e in temps] ==
            ["20°", "21°", "21°", "22°", "22°", "23°", "23°", "24°", "24°", "25°"],
            f"ui/hourly: expected ten temperatures, got {[e['text'] for e in temps]}")
    for i, entry in enumerate(temps):
        require(entry["font"] == "temp" and entry["width"] <= 26 and
                abs(entry["x"] + entry["width"] / 2 - hour_centers[i]) <= 0.5,
                f"ui/hourly: temperature {i} must be font_temp, <=33px wide and centered; "
                f"got {entry}")
    precip = [e for e in hourly["texts"] if e["y"] == PRECIP_BASE]
    # 100% is wider than a 28px column, so the sign is dropped rather than overlapping.
    require([e["text"] for e in precip] == ["10%", "40%", "40%", "81%", "100",
                                            "10%", "40%", "40%", "81%", "100"],
            f"ui/hourly: expected ten probabilities, got {[e['text'] for e in precip]}")
    require(all(e["font"] == "small" and e["width"] <= 26 for e in precip),
            f"ui/hourly: probabilities must be font_small within a column; got {precip}")
    strays = [(x, y) for x, y in hourly["ink"] if 26 <= y < 104 and not (8 <= x < 288)]
    require(not strays, f"ui/hourly: ink outside the ten columns: {strays[:4]}")

    # Match both sizes to an independent WMO/official-codepoint oracle and the actual
    # linked bitmap, not a reference obtained by asking ui.c to render the same code.
    for compact, baseline in ((False, forecast), (True, hourly)):
        font = "icon" if compact else "icon_daily"
        cols, start, step, height, advance = (10, 8, 28, 23, 26) if compact else (5, 18, 52, 27, 40)
        base = 46 + height
        unknown_font, unknown_base = ("small", 62) if compact else ("mid", 66)

        def check_icon_calls(result, codes):
            entries = [e for e in result["texts"] if e["font"] in ("icon", "icon_daily") or
                       e["text"] == "?"]
            require(len(entries) == cols, f"ui/{font}: expected exactly {cols} icons")
            masks = []
            for col, (entry, code) in enumerate(zip(entries, codes)):
                cp = codepoints.get(code)
                cx = start + step * col + step // 2
                left, top, right, bottom = entry["bounds"]
                context = f"ui/{font}/column{col}/code{code}"
                require(entry["text"] == (chr(cp) if cp is not None else "?") and
                        entry["font"] == (font if cp is not None else unknown_font) and
                        entry["y"] == (base if cp is not None else unknown_base),
                        f"{context}: wrong real font/codepoint/baseline: {entry}")
                require(abs(entry["x"] + entry["width"] / 2 - cx) <= 0.5 and
                        start + step * col <= left <= right < start + step * (col + 1) and
                        46 <= top <= bottom < 46 + height,
                        f"{context}: icon is not centered or escapes its ink band/cell")
                actual = region(result, 46, 46 + height, start + step * col, start + step * (col + 1))
                require(actual, f"{context}: empty framebuffer icon")
                if cp is not None:
                    require(entry["width"] == advance, f"{context}: wrong fixed advance")
                    g = icon_fonts[font][cp]
                    expected = {(cx - advance // 2 + x, base + y) for x, y in g["ink"]}
                    require(actual == expected, f"{context}: framebuffer differs from real glyph bitmap")
                masks.append(frozenset((x - cx, y) for x, y in actual))
            return masks

        baseline_codes = [codes[i % 5] for i in range(cols)]
        baseline_masks = check_icon_calls(baseline, baseline_codes)
        samples = {}
        for code in range(-1, 101):
            count += 1
            result = ui_capture(executable, work, "hourly_all_code" if compact else "all_code", code)
            masks = check_icon_calls(result, [code] * cols)
            require(all(mask == masks[0] for mask in masks),
                    f"ui/{font}/code{code}: icons differ across columns")
            samples[code] = masks[0]
            delta = baseline["ink"] ^ result["ink"]
            require(all((start <= x < start + cols * step and 46 <= y < 46 + height) or
                        (not compact and advice_pixel(x, y)) for x, y in delta),
                    f"ui/{font}/code{code}: icon update changed unrelated page pixels")
        representatives = [-1] + [wmo[0] for _, _, wmo in ICON_ORACLE.values()]
        require(len({samples[code] for code in representatives}) == 12,
                f"ui/{font}: eleven families and unknown must all be pixel-distinct")
        require(all(samples[code] == samples[-1] for code in range(-1, 101) if code not in codepoints),
                f"ui/{font}: undefined WMO codes must all use the same ?")
        # Change one column at a time, including both edge columns and unknown icons.
        for col in range(cols):
            for code in representatives:
                count += 1
                result = ui_capture(executable, work, "hourly_code" if compact else "day_code", col, code)
                changed_codes = baseline_codes.copy()
                changed_codes[col] = code
                check_icon_calls(result, changed_codes)
                delta = baseline["ink"] ^ result["ink"]
                body_delta = {(x, y) for x, y in delta if y >= 22}
                require(bool(body_delta) == (samples[code] != baseline_masks[col]) and
                        all((start + col * step <= x < start + (col + 1) * step and
                             46 <= y < 46 + height) or
                            (not compact and col == 0 and advice_pixel(x, y)) for x, y in delta),
                        f"ui/{font}/column{col}/code{code}: single icon change leaked outside its cell")

    blank = ui_capture(executable, work, "status_none")
    require(not any(e["y"] == 124 and e["x"] >= 200 for e in blank["texts"]),
            "ui/status_none: no status text may be drawn")
    require(not any(x >= 200 and y >= 110 for x, y in blank["ink"]),
            "ui/status_none: the status corner must stay blank")
    return count


def validate(trace, case, label, success_frames=None):
    pattern, portals, frames, connect_failures = CASES[case]
    context = f"{label}/{case}"
    actual = []
    captured = []
    for line in trace.splitlines():
        if line.startswith(b"frame:"):
            require(len(line) == 6 + FRAME_BYTES * 2, f"{context}: incomplete SPI frame")
            captured.append(bytes.fromhex(line[6:].decode("ascii")))
            require(len(captured[-1]) == FRAME_BYTES, f"{context}: invalid SPI frame")
            actual.append("frame")
        else:
            actual.append(line.decode("ascii"))
    require(actual.count("frame") == frames,
            f"{context}: expected {frames} frames, got {actual.count('frame')}")
    require(actual.count("portal") == portals, f"{context}: expected {portals} portals")

    # Presses are serviced during the wait that follows their cycle's update frame, so
    # each one redraws exactly one extra frame before that cycle's sleep is traced.
    presses = {}
    for token in PRESSES.get(case, "").split(","):
        if token:
            press_cycle, _after_ms = token.split(":")
            presses.setdefault(int(press_cycle), []).append(int(_after_ms))

    # Boot paints the placeholder page before the network is up; the glass keeps the last
    # session's image otherwise. The C harness pins its exact pixels; here it must at least
    # not be the blank page the panel would otherwise show.
    require(captured[0] != b"\xff" * FRAME_BYTES,
            f"{context}: boot frame is blank instead of the placeholder page")

    # Boot waits 5 s, then stored credentials are retried with the stepped schedule.
    # Every update refreshes once, then sleeps 30 minutes on success or the current
    # retry interval on failure. Provisioning is only used without stored credentials.
    expected = ["cycle:0", "net_init", "frame", "boot_wait:5000"]
    expected += ["connect", "frame", "portal"] * portals
    for failure in range(connect_failures):
        expected.append("connect")
        # A failed attempt repaints the title with the retry counter, so the screen is
        # never left sitting on the placeholder without a hint.
        expected.append("frame")
        expected.append(f"connect_wait:{retry_seconds(failure + 1) * 1000}")
    expected.append("connect")
    # Which cycle drew each captured frame; boot/portal/press frames are not update frames.
    frame_cycles = [None] * (1 + portals + connect_failures)
    have_weather = False
    retries = 0
    for cycle, outcome in enumerate(pattern):
        if cycle:
            expected.append(f"cycle:{cycle}")
        expected.append("status")  # footer status: fetching
        expected.append(f"request:{GEO_URL}")
        if outcome != "G":
            expected.append(f"request:{WEATHER_URL}")
        if outcome == "S":
            expected.append(f"timezone:{28800 if cycle < 3 else 19800}")
            if not have_weather:
                expected.append("status")  # footer status: syncing
                expected.append("time_sync")
            have_weather = True
            retries = 0
            sleep_ms = 1800000
        else:
            retries += 1
            delay = retry_seconds(retries)
            body = "weather" if have_weather else "offline"
            expected.append(f"retry:{retries}:{delay}:{body}")
            sleep_ms = delay * 1000
        expected.append("frame")
        frame_cycles.append(cycle)
        expected += ["frame"] * len(presses.get(cycle, []))
        frame_cycles += [None] * len(presses.get(cycle, []))
        expected.append(f"sleep:{sleep_ms}")
    for number, (got, want) in enumerate(zip(actual, expected), 1):
        require(got == want, f"{context}: event {number}: expected {want!r}, got {got!r}")
    require(len(actual) == len(expected),
            f"{context}: expected {len(expected)} events, got {len(actual)}")
    require(len(frame_cycles) == len(captured),
            f"{context}: frame/cycle accounting mismatch")

    # Recovery must redraw the actual weather, not just remove the retry title.
    # Same-cycle values/timezone match the independent all-success scenario.
    by_cycle = {cycle: frame for cycle, frame in zip(frame_cycles, captured)
                if cycle is not None}
    require(sorted(by_cycle) == list(range(len(pattern))),
            f"{context}: expected one update frame per cycle")
    if success_frames is not None:
        for cycle, outcome in enumerate(pattern):
            if outcome != "S" or cycle not in success_frames:
                continue
            # A press before this cycle leaves the app on the hourly page, so only an
            # even number of toggles can be byte-compared with the daily steady frame.
            toggles = sum(len(presses.get(earlier, [])) for earlier in range(cycle))
            if toggles % 2 == 0:
                require(by_cycle[cycle] == success_frames[cycle],
                        f"{context}: cycle {cycle}: recovered weather differs from steady frame")
            else:
                require(by_cycle[cycle] != success_frames[cycle],
                        f"{context}: cycle {cycle}: the pressed hourly page is still the daily frame")
    return by_cycle


def compare(current, baseline, case):
    if current == baseline:
        return
    for number, (got, want) in enumerate(zip(current.splitlines(), baseline.splitlines()), 1):
        if got != want:
            if got.startswith(b"frame:") and want.startswith(b"frame:"):
                detail = "complete SPI frame differs"
            else:
                detail = f"current={got!r}, baseline={want!r}"
            raise AssertionError(f"{case}: baseline mismatch at event {number}: {detail}")
    raise AssertionError(f"{case}: baseline trace lengths/endings differ")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--baseline", type=Path, metavar="PATH",
                        help="five-day/code/probability + advice-banner main directory for app comparison; "
                             "old current-weather/probability-less contracts are incompatible")
    parser.add_argument("--only", choices=("all", "app", "weather", "ui"), default="all",
                        help="run one harness while other agents' sources are still in progress")
    args = parser.parse_args()
    require(not args.baseline or args.only in ("all", "app"),
            "--baseline compares application traces; use --only all or --only app")
    # IDF adds a cross-target clang to PATH; cc remains the host compiler.
    compiler = shutil.which("cc") or shutil.which("clang")
    require(compiler, "no host C compiler found; install clang or cc")
    project = Path(__file__).resolve().parents[1]
    source = project / "main"
    baseline_source = args.baseline.resolve() if args.baseline else None
    cjson_dir = cjson_source = None
    if args.only in ("all", "app", "weather") or baseline_source:
        cjson_dir, cjson_source = locate_cjson(project)
    with tempfile.TemporaryDirectory(prefix="weather-test-app-") as directory:
        work = Path(directory)
        for name, text in HEADERS.items():
            path = work / "stubs" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        (work / "harness.c").write_text(HARNESS, encoding="utf-8")
        (work / "weather_harness.c").write_text(WEATHER_HARNESS, encoding="utf-8")
        (work / "fixtures.h").write_text(FIXTURES, encoding="utf-8")
        (work / "ui_wrapper.c").write_text(UI_WRAPPER, encoding="utf-8")
        (work / "ui_harness.c").write_text(UI_HARNESS, encoding="utf-8")
        (work / "button_harness.c").write_text(BUTTON_HARNESS, encoding="utf-8")
        current = (compile_app(compiler, source, cjson_dir, cjson_source, work, "current")
                   if args.only in ("all", "app") else None)
        weather = compile_weather(compiler, source, cjson_dir, cjson_source, work) if args.only in ("all", "weather") else None
        ui = compile_ui(compiler, source, work) if args.only in ("all", "ui") else None
        button = (compile_button(compiler, source, work)
                  if args.only in ("all", "app") else None)
        voice_audio = (compile_voice_audio(compiler, source, work)
                       if args.only in ("all", "app") else None)
        baseline = (compile_app(compiler, baseline_source, cjson_dir, cjson_source, work, "baseline")
                    if baseline_source else None)
        failures = []

        def failure(error, context):
            if isinstance(error, subprocess.CalledProcessError):
                detail = error.stderr.decode("utf-8", errors="replace").strip()
                message = f"{context}: harness exited {error.returncode}: {detail}"
            elif isinstance(error, subprocess.TimeoutExpired):
                message = f"{context}: harness exceeded {error.timeout}s (simulated delays only)"
            else:
                message = str(error)
            failures.append(message)
            print(f"FAIL: {message}", file=sys.stderr, flush=True)

        current_frames = baseline_frames = None
        app_cases = CASES if current else {}
        for case, (pattern, portals, frames, connect_failures) in app_cases.items():
            label = "current"
            try:
                trace = run([current, pattern, portals, connect_failures,
                              PRESSES.get(case, "")], work, 10)
                captured = validate(trace, case, label, current_frames)
                if case == "steady":
                    current_frames = captured
                if baseline:
                    label = "baseline"
                    old_trace = run([baseline, pattern, portals, connect_failures,
                                     PRESSES.get(case, "")], work, 10)
                    captured = validate(old_trace, case, label, baseline_frames)
                    if case == "steady":
                        baseline_frames = captured
                    compare(trace, old_trace, case)
            except (AssertionError, subprocess.SubprocessError) as error:
                failure(error, f"{label}/{case}")
                continue
            suffix = "; baseline byte-identical" if baseline else ""
            print(f"PASS {case}: {frames} frames, {portals} portals; cadence/pixels checked{suffix}",
                  flush=True)
        weather_count = 0
        app_failures = len(failures)
        for case, response, expected in weather_cases() if weather else ():
            weather_count += 1
            try:
                trace = run([weather, response], work, 10)
                validate_weather(trace, case, expected)
            except (AssertionError, ValueError, subprocess.SubprocessError) as error:
                failure(error, f"current/weather/{case}")
        parser_failures = len(failures) - app_failures
        if weather and not parser_failures:
            print(f"PASS weather parsing: {weather_count} cases (current only)", flush=True)
        if button:
            try:
                check_button(button, work)
                check_button_reset(current, work)
                print("PASS button: debounced tap and 5 s hold; reset notice, erase, setup and reconnect", flush=True)
            except (AssertionError, ValueError, subprocess.SubprocessError) as error:
                failure(error, "current/button")
        if voice_audio:
            try:
                check_voice_audio(voice_audio, work)
                print("PASS voice_audio: left-channel downmix, stereo upmix", flush=True)
            except (AssertionError, ValueError, subprocess.SubprocessError) as error:
                failure(error, "current/voice_audio")
        ui_count = 0
        if ui:
            try:
                ui_count = check_ui(ui, work, source)
                print(f"PASS UI: {ui_count} rendered cases; five-day cells/icons/probabilities, "
                      "today's advice, footer-only sensors, visible/unclipped glyphs checked", flush=True)
            except (AssertionError, ValueError, subprocess.SubprocessError) as error:
                failure(error, "current/ui")
        require(not failures, f"{len(failures)} regressions failed "
                f"({parser_failures}/{weather_count} direct parser cases)")
    print(f"PASS: {len(app_cases)} app scenarios, {weather_count} parser cases, {ui_count} UI cases; "
          "no ESP-IDF build or source-tree artifacts")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError) and error.stderr:
            print(error.stderr.decode(errors="replace").rstrip(), file=sys.stderr)
        sys.exit(1)
