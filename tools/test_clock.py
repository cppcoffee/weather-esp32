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
#include "epd_spi_test.h"
#include "device_mcp.h"
#include "button.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "epd.h"
#include "esp_err.h"
#include "sensor.h"
#include "ui.h"
#include "voice_feedback.h"

void app_main(void);
static pthread_mutex_t display = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static pthread_t app_thread, clock_thread, button_thread;
static _Thread_local bool clock_worker, display_held;
static void (*clock_entry)(void *);
static void (*button_entry)(void *);
static button_event_t queued[8];
static unsigned queued_count, sample_target, sample_done;
static atomic_int pin_level = 1;
static atomic_bool synced;
static atomic_llong wall_time = 1735689540LL; /* 2024-12-31 23:59:00 UTC */
static atomic_llong uptime;
static unsigned generation, completed, frames, pending_notifies, notify_done;
static struct epd_test_spi panel;
static unsigned page_switches, refreshed_switches, full_updates;
static bool dc_level = true;
static bool allow_forecast_pixels, allow_hourly_pixels, allow_page_switch, allow_status_pixels, allow_sensor_pixels;
static unsigned forecast_pixels, sensor_reads, sensor_publications;
static sensor_reading_t mock_sensor;
static bool ready, blocked, stopping, allow_weather_return, erased, reprovisioned;
enum { FRAME_BYTES = PANEL_W / 8 * PANEL_H };
static unsigned char frame[FRAME_BYTES];

time_t time(time_t *out) {
    time_t now = (time_t)atomic_load(&wall_time);
    if (out) *out = now;
    return now;
}
int64_t esp_timer_get_time(void) { return atomic_load(&uptime); }
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
static void *button_start(void *arg) {
    button_entry(arg); return NULL;
}
unsigned xTaskGetTickCount(void) { return 0; }
void xTaskDelayUntil(unsigned *last, unsigned ms) {
    assert(last && ms == BUTTON_POLL_MS);
    pthread_mutex_lock(&gate);
    sample_done = *last;
    pthread_cond_broadcast(&changed);
    while (sample_done == sample_target && !stopping) pthread_cond_wait(&changed, &gate);
    bool stop = stopping;
    (*last)++;
    pthread_mutex_unlock(&gate);
    if (stop) pthread_exit(NULL);
}
void *xQueueCreate(unsigned capacity, size_t size) {
    assert(capacity == 8 && size == sizeof(button_event_t)); return queued;
}
int xQueueSend(void *q, const void *item, unsigned timeout) {
    assert(q == queued && timeout == 0);
    pthread_mutex_lock(&gate);
    assert(queued_count < 8);
    queued[queued_count++] = *(const button_event_t *)item;
    pthread_mutex_unlock(&gate);
    return 1;
}
int xQueueReceive(void *q, void *item, unsigned timeout) {
    assert(q == queued && timeout == 0);
    pthread_mutex_lock(&gate);
    bool ready = queued_count != 0;
    if (ready) {
        *(button_event_t *)item = queued[0];
        if (queued[0] == BUTTON_TAP) page_switches++;
        memmove(queued, queued + 1, --queued_count * sizeof(*queued));
    }
    pthread_mutex_unlock(&gate);
    return ready;
}
int xQueueReset(void *q) {
    assert(q == queued);
    pthread_mutex_lock(&gate);
    queued_count = 0;
    pthread_mutex_unlock(&gate);
    return 1;
}
static void sample_button(int level, unsigned n) {
    pthread_mutex_lock(&gate);
    atomic_store(&pin_level, level);
    sample_target += n;
    pthread_cond_broadcast(&changed);
    while (sample_done != sample_target) pthread_cond_wait(&changed, &gate);
    pthread_mutex_unlock(&gate);
}
int xTaskCreate(void (*entry)(void *), const char *name, unsigned stack, void *arg, unsigned priority, TaskHandle_t *handle) {
    assert(stack >= 2048);
    if (!strcmp(name, "button")) {
        button_entry = entry;
        assert(!pthread_create(&button_thread, NULL, button_start, arg));
        if (handle) *handle = (TaskHandle_t)&button_thread;
        return 1;
    }
    assert(!strcmp(name, "clock"));
    clock_entry = entry;
    if (handle) *handle = (TaskHandle_t)&clock_thread;
    assert(!pthread_create(&clock_thread, NULL, clock_start, arg));
    return 1;
}
int xTaskNotifyGive(TaskHandle_t task) {
    assert(task == (TaskHandle_t)&clock_thread);
    pthread_mutex_lock(&gate);
    pending_notifies++;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&gate);
    return 1;
}
unsigned long ulTaskNotifyTake(int clear_on_exit, unsigned timeout) {
    assert(clock_worker && clear_on_exit == pdTRUE && timeout == 1000 && !display_held);
    static unsigned seen, seen_notify;
    pthread_mutex_lock(&gate);
    ready = true;
    completed = seen;
    notify_done = seen_notify;
    pthread_cond_broadcast(&changed);
    while (seen == generation && seen_notify == pending_notifies && !stopping) {
        pthread_cond_wait(&changed, &gate);
    }
    bool stop = stopping;
    unsigned long count = pending_notifies - seen_notify;
    seen_notify = pending_notifies;
    seen = generation;
    pthread_mutex_unlock(&gate);
    if (stop) pthread_exit(NULL);
    return count;
}
void vTaskDelay(unsigned ms) {
    assert(!clock_worker || ms < 1000); /* Boot/button and panel-settling delays only. */
}
int gpio_config(const gpio_config_t *cfg) { return 0; }
int gpio_set_level(int pin, int level) {
    if (pin == EPD_DC) dc_level = level;
    return 0;
}
int gpio_get_level(int pin) { return pin == 4 ? atomic_load(&pin_level) : 0; } /* Idle button, idle EPD BUSY. */
int spi_device_polling_transmit(spi_device_handle_t spi, spi_transaction_t *transaction) {
    pthread_mutex_lock(&gate);
    if (epd_test_transfer(&panel, dc_level, transaction)) {
        assert(display_held);
        bool full = refreshed_switches != page_switches && page_switches % 8 == 0;
        assert(panel.update == (full ? 0xC7 : 0x0F));
        if (full) full_updates++;
        if (frames && !full) {
            unsigned x_start = TEST_ROW_BYTES, x_end = 0, y_start = PANEL_H, y_end = 0;
            for (unsigned y = 0; y < PANEL_H; y++) for (unsigned x = 0; x < TEST_ROW_BYTES; x++) {
                size_t offset = y * TEST_ROW_BYTES + x;
                assert(panel.old[offset] == frame[offset]);
                if (panel.bw[offset] != frame[offset]) {
                    if (x < x_start) x_start = x;
                    if (x > x_end) x_end = x;
                    if (y < y_start) y_start = y;
                    y_end = y;
                }
            }
            assert(panel.x_start == x_start && panel.x_end == x_end);
            assert(panel.y_start == y_start && panel.y_end == y_end);
        }
        refreshed_switches = page_switches;
        memcpy(frame, panel.bw, sizeof(frame));
        frames++;
    }
    pthread_mutex_unlock(&gate);
    return 0;
}
esp_err_t audio_init(void) { return ESP_OK; }
esp_err_t sensor_init(void) { return ESP_OK; }
bool sensor_read(sensor_reading_t *out) {
    pthread_mutex_lock(&gate);
    sensor_reads++;
    *out = mock_sensor;
    pthread_mutex_unlock(&gate);
    return out->valid;
}
const char *sensor_name(void) { return "mock"; }
void device_mcp_set_sensor_model(const char *model) { (void)model; }
void device_mcp_publish_display_page(const char *page) { (void)page; }
void device_mcp_publish_sensor(const sensor_reading_t *sensor) {
    assert(sensor != NULL);
    pthread_mutex_lock(&gate);
    sensor_publications++;
    pthread_mutex_unlock(&gate);
}
esp_err_t voice_init(void) {
    return voice_feedback_publish(VOICE_STATE_WAIT_NETWORK, NULL) ? ESP_OK : ESP_FAIL;
}
void voice_net_ready(void) {}
void voice_enable_wake(void) {}
void voice_net_pause(void) {}
bool voice_weather_begin(void) { return true; }
void voice_weather_end(void) {}
forecast_stamp_t device_mcp_publish_forecast(const weather_t *w, const forecast_stamp_t *fetched, bool retrying) {
    return (forecast_stamp_t){0};
}
void net_init(void) {}
bool net_try_stored(int timeout) { return true; }
bool net_has_stored(void) { return true; }
void net_forget_creds(void) { assert(!display_held); erased = true; }
void net_portal(void) {
    assert(erased && !display_held);
    reprovisioned = true;
    pthread_exit(NULL);
}
const char *net_ap_ssid(void) { return "weather-DEMO"; }
void net_apply_tz(int offset) {}
bool net_time_is_synced(void) { return atomic_load(&synced); }
bool weather_update(weather_t *weather, forecast_stamp_t *fetched) {
    assert(!display_held); /* A network wait must not prevent the clock from drawing. */
    pthread_mutex_lock(&gate);
    blocked = true; pthread_cond_broadcast(&changed);
    while (!allow_weather_return) pthread_cond_wait(&changed, &gate);
    pthread_mutex_unlock(&gate);
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
    forecast_pixels = 0;
    for (int x = 0; x < 296; x++) for (int y = 0; y < 128; y++) {
        int px = 127 - y, index = x * 16 + (px >> 3), mask = 0x80 >> (px & 7);
        if ((before[index] ^ frame[index]) & mask) {
            bool clock_pixel = x >= 230 && y < 22;
            bool status_pixel = x >= 200 && y >= 110;
            bool sensor_pixel = x < 200 && y >= 110;
            bool forecast_pixel = (x >= 110 && x < 230 && y < 22)
                                  || (x >= 18 && x < 70 && y > 22 && y < 46);
            assert(clock_pixel || (allow_status_pixels && status_pixel)
                   || (allow_sensor_pixels && sensor_pixel)
                   || (allow_forecast_pixels && forecast_pixel)
                   || (allow_hourly_pixels && y > 22 && y < 108) || allow_page_switch);
            forecast_pixels += !clock_pixel;
            pixels++;
        }
    }
    assert((pixels != 0) == expect_refresh);
    pthread_mutex_unlock(&gate);
}

static void publish_status(voice_state_t state, const char *code, bool expect_refresh) {
    unsigned char before[FRAME_BYTES];
    pthread_mutex_lock(&gate);
    memcpy(before, frame, sizeof(before));
    unsigned previous_frames = frames;
    unsigned target = pending_notifies + 1;
    pthread_mutex_unlock(&gate);
    assert(voice_feedback_publish(state, code));
    pthread_mutex_lock(&gate);
    while (notify_done < target) pthread_cond_wait(&changed, &gate);
    assert(frames == previous_frames + (expect_refresh ? 1 : 0));
    assert(panel.updates == frames);
    unsigned pixels = 0;
    for (int x = 0; x < 296; x++) for (int y = 0; y < 128; y++) {
        int px = 127 - y, index = x * 16 + (px >> 3), mask = 0x80 >> (px & 7);
        if ((before[index] ^ frame[index]) & mask) {
            assert(x >= 200 && y >= 110);
            pixels++;
        }
    }
    assert((pixels != 0) == expect_refresh);
    pthread_mutex_unlock(&gate);
}

static void assert_hour_column(const unsigned char *before, int before_x, int after_x) {
    for (int x = 0; x < 28; x++) for (int y = 24; y < 108; y++) {
        int px = 127 - y, mask = 0x80 >> (px & 7);
        assert((before[(before_x + x) * 16 + (px >> 3)] & mask)
               == (frame[(after_x + x) * 16 + (px >> 3)] & mask));
    }
}

int main(void) {
    setenv("TZ", "UTC0", 1); tzset();
    assert(!pthread_create(&app_thread, NULL, app_start, NULL));
    pthread_mutex_lock(&gate);
    while (!ready || !blocked || notify_done != pending_notifies) pthread_cond_wait(&changed, &gate);
    assert(sensor_reads == 1 && sensor_publications == 1);
    pthread_mutex_unlock(&gate);
    unsigned voice_bytes = 0;
    sample_button(0, 8); sample_button(1, 3);
    assert(queued_count == 1 && queued[0] == BUTTON_TAP);
    allow_page_switch = true;
    advance(0, true); /* The visible page switches while the HTTP stub stays blocked. */
    assert(queued_count == 0);
    allow_page_switch = false;
    for (unsigned i = 1; i < 16; i++) {
        if (i == 7 || i == 15) {
            publish_status(VOICE_STATE_LISTENING, NULL, true);
            voice_bytes = panel.image_bytes;
            assert(voice_bytes < FRAME_BYTES);
            publish_status(VOICE_STATE_WAIT_NETWORK, NULL, true);
        }
        sample_button(0, 8); sample_button(1, 3);
        allow_page_switch = true;
        advance(0, true);
        allow_page_switch = false;
    }
    assert(page_switches == 16 && full_updates == 2);
    sample_button(0, 8); sample_button(1, 3);
    assert(queued_count == 1 && queued[0] == BUTTON_TAP);
    sample_button(0, BUTTON_LONG_PRESS_MS / BUTTON_POLL_MS + 10);
    sample_button(1, 3);
    assert(queued_count == 1 && queued[0] == BUTTON_HOLD); /* Continues while HTTP blocks. */
    advance(59, false); /* No refresh while the clock is unsynchronized. */
    atomic_store(&synced, true);
    advance(0, false); /* SNTP alone cannot tell us the local time zone. */
    weather_t weather = {.ok = true, .utc_offset = 8 * 3600};
    sensor_reading_t sensor = {0};
    unsigned char updated[FRAME_BYTES];
    xSemaphoreTake(&display, 0);
    setenv("TZ", "XXX-8", 1); tzset();
    ui_draw_hourly(&weather, &sensor); ui_draw_status(UI_STATUS_FETCHING, 0); epd_refresh();
    memcpy(updated, frame, sizeof(updated));
    epd_fill_rect(230, 0, PANEL_H - 230, 22, EPD_WHITE);
    epd_text(292 - epd_text_w(&font_clock, "07:59"), 16, &font_clock, "07:59");
    epd_refresh();
    assert(!memcmp(updated, frame, sizeof(frame))); /* 23:59 UTC is 07:59 local. */
    xSemaphoreGive(&display);
    advance(1, true); /* Local time rolls to 08:00 while HTTP is blocked. */
    unsigned clock_bytes = panel.image_bytes;
    assert(clock_bytes < FRAME_BYTES);
    xSemaphoreTake(&display, 0);
    memcpy(updated, frame, sizeof(updated));
    ui_draw_hourly(&weather, &sensor); ui_draw_status(UI_STATUS_FETCHING, 0); epd_refresh();
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
    ui_draw_portal_hint(); epd_refresh();
    xSemaphoreGive(&display);
    advance(60, false); /* A timer must never overwrite the setup instructions. */
    xSemaphoreTake(&display, 0);
    ui_draw_weather(&weather, &sensor); epd_refresh();
    xSemaphoreGive(&display);
    advance(60, true); /* Clock resumes after leaving setup. */
    for (unsigned minute = 0; minute < 60; minute++) advance(60, true);

    /* With a real calendar date on screen, midnight changes only annotations. */
    xSemaphoreTake(&display, 0);
    setenv("TZ", "UTC0", 1); tzset();
    weather.utc_offset = 0;
    atomic_store(&wall_time, 1735689540LL);
    strcpy(weather.city, "Test");
    strcpy(weather.days[0].date, "2024-12-31");
    ui_draw_weather(&weather, &sensor); ui_draw_status(UI_STATUS_UPDATED, 0); epd_refresh();
    memcpy(updated, frame, sizeof(updated));
    forecast_stamp_t stamp = {.fetched_at = time(NULL)};
    ui_set_forecast_stamp(&stamp);
    ui_draw_weather(&weather, &sensor); ui_draw_status(UI_STATUS_UPDATED, 0); epd_refresh();
    /* The fetch timestamp affects freshness, but is not printed in the footer. */
    assert(!memcmp(updated, frame, sizeof(updated)));
    xSemaphoreGive(&display);
    allow_forecast_pixels = true;
    atomic_store(&uptime, 1800000000LL);
    advance(0, true); /* The monotonic age reaches 30 minutes at the same wall time. */
    assert(forecast_pixels > 0);
    advance(60, true); /* Midnight removes the old "today" label even without a new fetch. */
    assert(forecast_pixels > 0);
    xSemaphoreTake(&display, 0);
    memcpy(updated, frame, sizeof(updated));
    ui_draw_weather(&weather, &sensor); ui_draw_status(UI_STATUS_UPDATED, 0); epd_refresh();
    assert(!memcmp(updated, frame, sizeof(frame)));
    xSemaphoreGive(&display);

    /* Across midnight, the next cached hour and its weather move into column one. */
    xSemaphoreTake(&display, 0);
    atomic_store(&wall_time, 1735689540LL);
    for (int i = 0; i < WEATHER_HOURS; i++) {
        weather.hours[i] = (weather_hour_t){.date = i ? 20250101 : 20241231,
                                           .hour = i ? i - 1 : 23, .temperature_c = 5 + i};
    }
    ui_draw_hourly(&weather, &sensor); ui_draw_status(UI_STATUS_UPDATED, 0); epd_refresh();
    memcpy(updated, frame, sizeof(updated));
    xSemaphoreGive(&display);
    allow_hourly_pixels = true;
    advance(60, true);
    assert_hour_column(updated, 36, 8);
    advance(20 * 3600, true); /* All 21 cached hours have expired. */
    memcpy(updated, frame, sizeof(updated));
    xSemaphoreTake(&display, 0);
    ui_draw_hourly(NULL, &sensor); epd_refresh();
    assert_hour_column(updated, 8, 8);
    assert_hour_column(updated, 260, 260);
    xSemaphoreGive(&display);

    /* A repeated wall hour still advances in chronological API order. */
    xSemaphoreTake(&display, 0);
    atomic_store(&wall_time, 1735689540LL);
    weather.hours[1].date = 20241231;
    weather.hours[1].hour = 23;
    for (int i = 2; i < WEATHER_HOURS; i++) weather.hours[i].hour = i - 2;
    ui_draw_hourly(&weather, &sensor); ui_draw_status(UI_STATUS_UPDATED, 0); epd_refresh();
    memcpy(updated, frame, sizeof(updated));
    xSemaphoreGive(&display);
    advance(60, true);
    assert_hour_column(updated, 36, 8);
    advance(3600, true);
    assert_hour_column(updated, 64, 8);

    /* Rapid voice status changes wake clock_task immediately and share the refresh cadence. */
    const voice_state_t rapid[] = {
        VOICE_STATE_CONNECTING, VOICE_STATE_READY, VOICE_STATE_LISTENING, VOICE_STATE_WAITING,
        VOICE_STATE_SPEAKING, VOICE_STATE_FOLLOWUP, VOICE_STATE_LISTENING, VOICE_STATE_WAITING,
        VOICE_STATE_SPEAKING, VOICE_STATE_OFFLINE, VOICE_STATE_ERROR, VOICE_STATE_READY,
    };
    for (unsigned i = 0; i < sizeof(rapid) / sizeof(rapid[0]); i++) {
        publish_status(rapid[i], NULL, true);
    }
    publish_status(VOICE_STATE_WAIT_NETWORK, NULL, true);

    /* Sensor polling is due at 10 minutes of monotonic time, regardless of wall time. */
    pthread_mutex_lock(&gate);
    mock_sensor = (sensor_reading_t){.temperature_c = 23.5f, .humidity_percent = 58.0f, .pressure_hpa = 1013.2f, .valid = true};
    unsigned before_poll = sensor_reads;
    assert(sensor_publications == before_poll);
    pthread_mutex_unlock(&gate);
    atomic_fetch_add(&uptime, 599LL * 1000000);
    advance(0, false);
    assert(sensor_reads == before_poll && sensor_publications == before_poll);

    /* A changed reading refreshes only x < 200, y >= 110 and publishes to MCP. */
    allow_sensor_pixels = true;
    atomic_fetch_add(&uptime, 1000000);
    advance(0, true);
    allow_sensor_pixels = false;
    assert(sensor_reads == before_poll + 1 && sensor_publications == before_poll + 1);
    /* An identical reading at the next poll is published without refreshing the screen. */
    atomic_fetch_add(&uptime, 600LL * 1000000);
    advance(0, false);
    assert(sensor_reads == before_poll + 2 && sensor_publications == before_poll + 2);

    assert(!erased && !reprovisioned); /* WiFi credentials stay intact during HTTP I/O. */
    pthread_mutex_lock(&gate);
    allow_weather_return = true; pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&gate);
    assert(!pthread_join(app_thread, NULL));
    assert(erased && reprovisioned);
    pthread_mutex_lock(&gate);
    stopping = true; pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&gate);
    assert(!pthread_join(clock_thread, NULL)); assert(!pthread_join(button_thread, NULL));
    assert(panel.updates == frames && full_updates == 2);
    assert(memcmp(panel.old, panel.bw, FRAME_BYTES) == 0);
    puts("PASS clock: blocked HTTP taps and hold, local time, retry, setup/resume, forecast expiry, hourly rollover, regional window and full waveform only on page switches 8 and 16");
    puts("PASS sensor cadence: one boot sample, 10-minute polls, MCP publication and changed-region refresh");
    printf("0x24 image bytes: clock %u, voice status %u, full frame %u\n",
           clock_bytes, voice_bytes, FRAME_BYTES);
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
            if name == "esp_timer.h":
                contents = "#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time(void);\n"
            elif name == "freertos/FreeRTOS.h":
                contents += (
                    "#include <assert.h>\n#include <pthread.h>\n"
                    "typedef pthread_mutex_t portMUX_TYPE;\n"
                    "#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER\n"
                    "#define portENTER_CRITICAL(lock) assert(!pthread_mutex_lock(lock))\n"
                    "#define portEXIT_CRITICAL(lock) assert(!pthread_mutex_unlock(lock))\n"
                )
            path.write_text(contents)
        (work / "clock.c").write_text(HARNESS)
        sources = ["main.c", "button.c", "ui.c", "epd.c", "voice_feedback.c", "fonts_aa.c", "font_clock.c", "font_icons.c"]
        flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"] if args.sanitize else []
        subprocess.run([
            compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra",
            "-Wno-unused-parameter", "-pthread", *flags, "-I", str(work / "stubs"),
            "-I", str(project / "main"), "-I", str(project / "tools"),
            *(str(project / "main" / name) for name in sources),
            str(work / "clock.c"), "-lm", "-o", str(work / "clock"),
        ], check=True, timeout=60)
        subprocess.run([str(work / "clock")], check=True, timeout=15)


if __name__ == "__main__":
    main()
