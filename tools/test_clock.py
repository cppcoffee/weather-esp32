#!/usr/bin/env python3
"""Exercise the real clock task, main loop, UI and EPD with a blocked weather request.

pthread-backed RTOS stubs advance wall time deterministically. Checks minute changes,
midnight, clock corrections, full redraw equivalence, both pages, retry and setup.
Does not validate FreeRTOS scheduling or physical display latency/ghosting.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_app import HEADERS


HARNESS = r'''
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "device_mcp.h"
#include "epd.h"
#include "esp_err.h"
#include "ui.h"
#include "voice_feedback.h"

void app_main(void);
static pthread_mutex_t display = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static pthread_t app_thread, clock_thread;
static _Thread_local bool clock_worker, display_held;
static void (*clock_entry)(void *);
static atomic_llong wall_time = 1735689540LL; /* 2024-12-31 23:59:00 UTC */
static unsigned generation, completed, frames;
static bool ready, blocked, stopping;
enum { FRAME_BYTES = PANEL_W / 8 * PANEL_H };
static unsigned char frame[FRAME_BYTES];

time_t time(time_t *out) {
    time_t now = (time_t)atomic_load(&wall_time);
    if (out) *out = now;
    return now;
}
void *xSemaphoreCreateMutex(void) { return &display; }
int xSemaphoreTake(void *semaphore, unsigned timeout) {
    assert(semaphore == &display && !display_held);
    assert(!pthread_mutex_lock(semaphore)); display_held = true; return 1;
}
int xSemaphoreGive(void *semaphore) {
    assert(semaphore == &display && display_held);
    display_held = false; assert(!pthread_mutex_unlock(semaphore)); return 1;
}
static void *clock_start(void *arg) {
    clock_worker = true; clock_entry(arg); return NULL;
}
int xTaskCreate(void (*entry)(void *), const char *name, unsigned stack, void *arg, unsigned priority, void *handle) {
    assert(!strcmp(name, "clock") && stack >= 2048);
    clock_entry = entry;
    assert(!pthread_create(&clock_thread, NULL, clock_start, arg));
    return 1;
}
void vTaskDelay(unsigned ms) {
    if (!clock_worker || ms < 1000) return; /* Boot/button and panel-settling delays. */
    assert(ms == 1000 && !display_held);
    static unsigned seen;
    pthread_mutex_lock(&gate);
    ready = true; completed = seen; pthread_cond_broadcast(&changed);
    while (seen == generation && !stopping) pthread_cond_wait(&changed, &gate);
    bool stop = stopping;
    seen = generation;
    pthread_mutex_unlock(&gate);
    if (stop) pthread_exit(NULL);
}
int gpio_config(const gpio_config_t *cfg) { return 0; }
int gpio_set_level(int pin, int level) { return 0; }
int gpio_get_level(int pin) { return pin == 4 ? 1 : 0; } /* Idle button, idle EPD BUSY. */
int spi_device_polling_transmit(spi_device_handle_t spi, spi_transaction_t *transaction) {
    if (transaction->length == FRAME_BYTES * 8) {
        assert(display_held);
        pthread_mutex_lock(&gate);
        memcpy(frame, transaction->tx_buffer, sizeof(frame)); frames++;
        pthread_mutex_unlock(&gate);
    }
    return 0;
}
esp_err_t audio_init(void) { return ESP_OK; }
esp_err_t voice_init(void) { return ESP_OK; }
voice_feedback_t voice_feedback_get(void) {
    return (voice_feedback_t){.state = VOICE_STATE_WAIT_NETWORK};
}
void voice_net_ready(void) {}
void voice_net_pause(void) {}
bool voice_weather_begin(void) { return true; }
void voice_weather_end(void) {}
void device_mcp_publish(const weather_t *w, const sensor_reading_t *sensor, bool retrying, bool updated) {}
void net_init(void) {}
bool net_try_stored(int timeout) { return true; }
bool net_has_stored(void) { return true; }
void net_forget_creds(void) {}
void net_portal(void) { abort(); }
const char *net_ap_ssid(void) { return "weather-DEMO"; }
void net_apply_tz(int offset) {}
void net_time_sync(void) {}
bool weather_update(weather_t *weather) {
    assert(!display_held); /* A network wait must not prevent the clock from drawing. */
    pthread_mutex_lock(&gate);
    blocked = true; pthread_cond_broadcast(&changed);
    while (!stopping) pthread_cond_wait(&changed, &gate);
    pthread_mutex_unlock(&gate);
    pthread_exit(NULL);
    return false;
}
static void *app_start(void *arg) { app_main(); return NULL; }

static void advance(long seconds, bool expect_refresh) {
    unsigned char before[FRAME_BYTES];
    pthread_mutex_lock(&gate);
    memcpy(before, frame, sizeof(before));
    unsigned previous_frames = frames;
    atomic_fetch_add(&wall_time, seconds);
    generation++; pthread_cond_broadcast(&changed);
    while (completed != generation) pthread_cond_wait(&changed, &gate);
    assert(frames == previous_frames + (expect_refresh ? 1 : 0));
    unsigned pixels = 0;
    for (int x = 0; x < 296; x++) for (int y = 0; y < 128; y++) {
        int px = 127 - y, index = x * 16 + (px >> 3), mask = 0x80 >> (px & 7);
        if ((before[index] ^ frame[index]) & mask) {
            assert(x >= 230 && y < 22); pixels++;
        }
    }
    assert((pixels != 0) == expect_refresh);
    pthread_mutex_unlock(&gate);
}

int main(void) {
    setenv("TZ", "UTC0", 1); tzset();
    assert(!pthread_create(&app_thread, NULL, app_start, NULL));
    pthread_mutex_lock(&gate);
    while (!ready || !blocked) pthread_cond_wait(&changed, &gate);
    pthread_mutex_unlock(&gate);
    advance(59, false);
    advance(1, true); /* Midnight: 23:59 -> 00:00, with no weather response. */
    unsigned char updated[FRAME_BYTES];
    xSemaphoreTake(&display, 0);
    memcpy(updated, frame, sizeof(updated));
    weather_t weather = {0}; sensor_reading_t sensor = {0};
    ui_draw_weather(&weather, &sensor); ui_draw_status(UI_STATUS_FETCHING, 0); epd_refresh();
    assert(!memcmp(updated, frame, sizeof(frame))); /* Old digits were completely erased. */
    xSemaphoreGive(&display);
    advance(59, false);
    advance(1, true);
    advance(-60, true); /* SNTP correcting backward must update too. */
    advance(3600, true);
    advance(86400, false); /* Same visible HH:MM needs no clock-only refresh. */
    xSemaphoreTake(&display, 0);
    ui_draw_hourly(&weather, &sensor); epd_refresh();
    xSemaphoreGive(&display);
    advance(60, true);
    xSemaphoreTake(&display, 0);
    ui_draw_retry(UINT_MAX, 60); epd_refresh();
    xSemaphoreGive(&display);
    advance(60, true);
    xSemaphoreTake(&display, 0);
    ui_draw_portal_hint(); epd_refresh_full();
    xSemaphoreGive(&display);
    advance(60, false); /* A timer must never overwrite the setup instructions. */
    xSemaphoreTake(&display, 0);
    ui_draw_weather(&weather, &sensor); epd_refresh();
    xSemaphoreGive(&display);
    advance(60, true); /* Clock resumes after leaving setup. */
    for (unsigned minute = 0; minute < 60; minute++) advance(60, true);
    pthread_mutex_lock(&gate);
    stopping = true; pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&gate);
    assert(!pthread_join(app_thread, NULL)); assert(!pthread_join(clock_thread, NULL));
    puts("PASS clock: blocked request, 60 consecutive minutes, unchanged minute, midnight, clock corrections, redraw erasure, hourly, retry, setup/resume; clock pixels isolated; display serialized");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    project = Path(__file__).resolve().parents[1]
    compiler = shutil.which("cc")
    assert compiler, "host C compiler is required"
    with tempfile.TemporaryDirectory(prefix="weather-clock-") as directory:
        work = Path(directory)
        for name, contents in HEADERS.items():
            path = work / "stubs" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(contents)
        (work / "clock.c").write_text(HARNESS)
        sources = ["main.c", "button.c", "ui.c", "epd.c", "fonts_aa.c", "font_clock.c", "font_icons.c"]
        flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"] if args.sanitize else []
        subprocess.run([
            compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra",
            "-Wno-unused-parameter", "-pthread", *flags, "-I", str(work / "stubs"),
            "-I", str(project / "main"), *(str(project / "main" / name) for name in sources),
            str(work / "clock.c"), "-lm", "-o", str(work / "clock"),
        ], check=True, timeout=60)
        subprocess.run([str(work / "clock")], check=True, timeout=15)


if __name__ == "__main__":
    main()
