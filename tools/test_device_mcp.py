#!/usr/bin/env python3
"""Host regression for the real device_mcp.c and managed cJSON, using stdlib + cc.

    python3 tools/test_device_mcp.py
    python3 tools/test_device_mcp.py --sanitize
    python3 tools/test_device_mcp.py --baseline .cache/mcp-ablation/client-baseline/main

Run `idf.py reconfigure`/`idf.py build` first to download managed dependencies.
The real mcp-c-sdk public headers type-check our API stubs; no third-party source
is copied. Each scenario runs in a fresh process (no private snapshot reset).
Builds live in TemporaryDirectory; --baseline saves raw results and measurements
under .cache/mcp-ablation. This tests device integration,
not MCP protocol/SDK implementation, networking, SNTP transport, or I2S hardware.
"""

import argparse
import datetime as dt
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


HEADERS = {
    "esp_err.h": r"""
#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
""",
    "esp_timer.h": r"""
#pragma once
#include <stdint.h>
int64_t esp_timer_get_time(void);
""",
    "esp_system.h": r"""
#pragma once
#include <stdint.h>
uint32_t esp_get_free_heap_size(void);
uint32_t esp_get_free_internal_heap_size(void);
""",
    "freertos/FreeRTOS.h": r"""
#pragma once
#include <pthread.h>
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
void test_enter_critical(portMUX_TYPE *lock);
void test_exit_critical(portMUX_TYPE *lock);
#define portENTER_CRITICAL(lock) test_enter_critical(lock)
#define portEXIT_CRITICAL(lock) test_exit_critical(lock)
""",
}

HARNESS = r'''
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "audio.h"
#include "cJSON.h"
#include "device_mcp.h"
#include "esp_mcp_engine.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "net.h"

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)
#define EPOCH 1735689600LL /* 2025-01-01T00:00:00Z */

static _Atomic long long wall = EPOCH;
static _Atomic long long uptime = 10000000;
static _Atomic bool synced;
static _Thread_local unsigned lock_depth;
static _Atomic long allocations, live_json;
static _Atomic long fail_at = -1;

void test_enter_critical(portMUX_TYPE *lock)
{
    CHECK(lock_depth == 0);
    CHECK(pthread_mutex_lock(lock) == 0);
    lock_depth++;
}

void test_exit_critical(portMUX_TYPE *lock)
{
    CHECK(lock_depth == 1);
    lock_depth--;
    CHECK(pthread_mutex_unlock(lock) == 0);
}

time_t device_test_time(time_t *out)
{
    CHECK(lock_depth == 0);
    time_t value = (time_t)atomic_load(&wall);
    if (out) *out = value;
    return value;
}

bool net_time_is_synced(void)
{
    CHECK(lock_depth == 0);
    return atomic_load(&synced);
}

int64_t esp_timer_get_time(void)
{
    CHECK(lock_depth == 0);
    return atomic_load(&uptime);
}

#if TEST_CURRENT_MCP
#define SENSOR_PRESSURE(value) .pressure_hpa = (value),
void net_get_status(net_status_t *out)
{
    CHECK(lock_depth == 0);
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->connected = true;
    snprintf(out->ssid, sizeof(out->ssid), "TestWiFi");
    out->rssi = -52;
}
#else
#define SENSOR_PRESSURE(value)
#endif

uint32_t esp_get_free_heap_size(void)
{
    CHECK(lock_depth == 0);
    return 120000;
}

uint32_t esp_get_free_internal_heap_size(void)
{
    CHECK(lock_depth == 0);
    return 80000;
}

static void *json_malloc(size_t size)
{
    CHECK(lock_depth == 0); /* JSON building must not hold the snapshot lock. */
    long call = atomic_fetch_add(&allocations, 1) + 1;
    if (call == atomic_load(&fail_at)) return NULL;
    void *p = malloc(size);
    if (p) atomic_fetch_add(&live_json, 1);
    return p;
}

static void json_free(void *p)
{
    CHECK(lock_depth == 0);
    if (p) {
        CHECK(atomic_fetch_sub(&live_json, 1) > 0);
        free(p);
    }
}

#define TOOL_COUNT (TEST_CURRENT_MCP ? 5 : 3)
struct esp_mcp_property_s { int min, max; };
struct esp_mcp_property_list_s { int volume; };
struct esp_mcp_tool_s {
    const char *name;
    char *description;
    esp_mcp_tool_callback_ex_t callback;
    char *annotations;
    esp_mcp_property_t *property;
    bool owned;
};
struct esp_mcp_s {
    esp_mcp_tool_t *tools[TOOL_COUNT];
    int count;
};
struct esp_mcp_tool_result_s {
    char *text;
    int calls;
    esp_err_t error;
};

static int creates, annotates, adds, destroys, live_tools;
static int fail_create, fail_annotation, fail_add;
static const char *names[] = {
    "self.get_current_time", "self.get_weather_forecast", "self.get_sensor_reading",
    "self.get_device_status", "self.audio_speaker.set_volume"
};

static int current_volume = 60;
static esp_err_t volume_error;
static bool fail_property_create, fail_property_attach;

esp_err_t audio_get_volume(int *volume) { *volume = current_volume; return ESP_OK; }
esp_err_t audio_set_volume(int volume) {
    if (volume < 0 || volume > 100) return ESP_ERR_INVALID_ARG;
    if (volume_error != ESP_OK) return volume_error;
    current_volume = volume;
    return ESP_OK;
}
esp_mcp_property_t *esp_mcp_property_create_with_range(const char *name, int min, int max) {
    CHECK(!strcmp(name, "volume") && min == 0 && max == 100);
    if (fail_property_create) return NULL;
    esp_mcp_property_t *prop = malloc(sizeof(*prop));
    CHECK(prop);
    *prop = (esp_mcp_property_t){min, max};
    return prop;
}
esp_err_t esp_mcp_property_destroy(esp_mcp_property_t *prop) { free(prop); return ESP_OK; }
esp_err_t esp_mcp_tool_add_property(esp_mcp_tool_t *tool, esp_mcp_property_t *prop) {
    CHECK(!tool->property && !strcmp(tool->name, names[4]));
    if (fail_property_attach) return ESP_FAIL;
    tool->property = prop;
    return ESP_OK;
}
int esp_mcp_property_list_get_property_int(const esp_mcp_property_list_t *list, const char *name) {
    CHECK(list && !strcmp(name, "volume"));
    return list->volume;
}

esp_mcp_tool_t *esp_mcp_tool_create_ex(const char *name, const char *title,
                                     const char *description, esp_mcp_tool_callback_ex_t callback)
{
    CHECK(name && title == NULL && description && *description && callback);
    CHECK(creates < TOOL_COUNT && strcmp(name, names[creates]) == 0);
    if (++creates == fail_create) return NULL;
    esp_mcp_tool_t *tool = calloc(1, sizeof(*tool));
    CHECK(tool);
    tool->name = name;
    tool->description = strdup(description);
    CHECK(tool->description);
    tool->callback = callback;
    live_tools++;
    return tool;
}

esp_err_t esp_mcp_tool_set_annotations_json(esp_mcp_tool_t *tool, const char *text)
{
    CHECK(tool && !tool->owned && !tool->annotations && text);
    if (++annotates == fail_annotation) return ESP_ERR_INVALID_STATE;
    const char *end = NULL;
    cJSON *json = cJSON_ParseWithOpts(text, &end, 1);
    CHECK(json && end && *end == '\0' && cJSON_IsObject(json));
    CHECK(cJSON_GetArraySize(json) == 4);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "readOnlyHint")) ==
          (strcmp(tool->name, "self.audio_speaker.set_volume") != 0));
    CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(json, "destructiveHint")));
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "idempotentHint")));
    CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(json, "openWorldHint")));
    cJSON_Delete(json);
    tool->annotations = strdup(text);
    CHECK(tool->annotations);
    return ESP_OK;
}

esp_err_t esp_mcp_add_tool(esp_mcp_t *mcp, esp_mcp_tool_t *tool)
{
    CHECK(mcp && mcp->count < TOOL_COUNT && tool && tool->annotations && !tool->owned);
    if (++adds == fail_add) return ESP_FAIL;
    mcp->tools[mcp->count++] = tool;
    tool->owned = true;
    return ESP_OK;
}

esp_err_t esp_mcp_tool_destroy(esp_mcp_tool_t *tool)
{
    CHECK(tool && !tool->owned && live_tools > 0);
    free(tool->property);
    free(tool->annotations);
    free(tool->description);
    free(tool);
    destroys++;
    live_tools--;
    return ESP_OK;
}

esp_err_t esp_mcp_destroy(esp_mcp_t *mcp)
{
    CHECK(mcp);
    for (int i = 0; i < mcp->count; i++) {
        CHECK(mcp->tools[i]->owned);
        mcp->tools[i]->owned = false;
        CHECK(esp_mcp_tool_destroy(mcp->tools[i]) == ESP_OK);
        mcp->tools[i] = NULL;
    }
    mcp->count = 0;
    return ESP_OK;
}

esp_err_t esp_mcp_tool_result_add_text(esp_mcp_tool_result_t *result, const char *text)
{
    CHECK(lock_depth == 0 && result && text && result->calls == 0 && !result->text);
    result->calls++;
    if (result->error != ESP_OK) return result->error;
    /* Like the SDK builder, copy: the module frees its cJSON print buffer. */
    result->text = strdup(text);
    CHECK(result->text);
    return ESP_OK;
}

static esp_mcp_tool_callback_ex_t callback(esp_mcp_t *mcp, int index)
{
    CHECK(mcp->count == TOOL_COUNT && index >= 0 && index < TOOL_COUNT);
    return mcp->tools[index]->callback;
}

static char *invoke(esp_mcp_t *mcp, int index)
{
    esp_mcp_tool_result_t result = {0};
    CHECK(callback(mcp, index)(NULL, &result) == ESP_OK);
    CHECK(result.calls == 1 && result.text);
    return result.text;
}

static void emit(esp_mcp_t *mcp, const char *label, int index)
{
    char *text = invoke(mcp, index);
    printf("%s\t%s\n", label, text);
    free(text);
    CHECK(atomic_load(&live_json) == 0);
}

static weather_t fixture(int variant)
{
    weather_t w = {.ok = true, .utc_offset = variant ? -12600 : 28800};
    snprintf(w.city, sizeof(w.city), "%s", variant ? "Other city" : "Cache \"city\"");
    CHECK(WEATHER_DAYS == 5 && WEATHER_HOURS == 21);
    for (int i = 0; i < WEATHER_DAYS; i++) {
        weather_day_t *d = &w.days[i];
        snprintf(d->date, sizeof(d->date), "2025-01-%02d", i + 1 + variant * 10);
        d->tmin = -8.5f + i + variant * 20;
        d->tmax = 1.25f + i + variant * 20;
        d->code = (i * 23 + variant) % 100;
        d->precip_probability = i * 24.5f + variant;
    }
    for (int i = 0; i < WEATHER_HOURS; i++) {
        weather_hour_t *h = &w.hours[i];
#if TEST_CURRENT_MCP
        h->date = 20250101 + variant * 10 + (20 + i + variant) / 24;
#endif
        h->hour = (20 + i + variant) % 24;
        h->temperature_c = -4.75f + i * 0.5f + variant * 20;
        h->code = (i * 7 + variant) % 100;
        h->precip_probability = i * 4.5f + variant;
    }
    return w;
}

static void registration(void)
{
    esp_mcp_t engine = {0};
    CHECK(device_mcp_register(NULL) == ESP_ERR_INVALID_ARG);
    CHECK(creates == 0 && live_tools == 0);
    CHECK(device_mcp_register(&engine) == ESP_OK);
    CHECK(engine.count == TOOL_COUNT && creates == TOOL_COUNT && annotates == TOOL_COUNT && adds == TOOL_COUNT && destroys == 0);
    CHECK(esp_mcp_destroy(&engine) == ESP_OK && live_tools == 0 && destroys == TOOL_COUNT);
    for (int stage = 0; stage < 3; stage++) {
        for (int position = 1; position <= TOOL_COUNT; position++) {
            creates = annotates = adds = destroys = 0;
            fail_create = stage == 0 ? position : 0;
            fail_annotation = stage == 1 ? position : 0;
            fail_add = stage == 2 ? position : 0;
            esp_err_t expected = stage == 0 ? ESP_ERR_NO_MEM : stage == 1 ? ESP_ERR_INVALID_STATE : ESP_FAIL;
            CHECK(device_mcp_register(&engine) == expected);
            /* Previous successes are owned by the engine, per device_mcp.h. */
            CHECK(engine.count == position - 1 && live_tools == position - 1);
            CHECK(creates == position);
            CHECK(annotates == position - (stage == 0));
            CHECK(adds == position - (stage != 2));
            CHECK(destroys == (stage != 0));
            for (int i = 0; i < engine.count; i++) {
                CHECK(engine.tools[i]->owned && engine.tools[i]->callback);
            }
            CHECK(esp_mcp_destroy(&engine) == ESP_OK);
            CHECK(live_tools == 0 && destroys == position - (stage == 0));
            CHECK(atomic_load(&live_json) == 0);
        }
    }
#if TEST_CURRENT_MCP
    fail_create = fail_annotation = fail_add = 0;
    fail_property_create = true;
    creates = 0;
    CHECK(device_mcp_register(&engine) == ESP_ERR_NO_MEM && engine.count == 4);
    CHECK(esp_mcp_destroy(&engine) == ESP_OK && !live_tools);
    fail_property_create = false;
    fail_property_attach = true;
    creates = 0;
    CHECK(device_mcp_register(&engine) == ESP_FAIL && engine.count == 4);
    CHECK(esp_mcp_destroy(&engine) == ESP_OK && !live_tools);
#endif
    printf("PASS registration: %d tools; %d failure/ownership paths\n", TOOL_COUNT, 3 * TOOL_COUNT);
}

#if TEST_CURRENT_MCP
static void volume(esp_mcp_t *mcp)
{
    CHECK(mcp->tools[4]->property && mcp->tools[4]->property->min == 0 && mcp->tools[4]->property->max == 100);
    char *text = invoke(mcp, 3);
    CHECK(strstr(text, "\"audio_speaker\":{\"volume\":60}") && strstr(text, "\"read_at\":null")
          && strstr(text, "\"display_page\":\"placeholder\"") && strstr(text, "\"sensor_model\":\"unknown\"")
          && strstr(text, "\"wifi\":{\"connected\":true,\"ssid\":\"TestWiFi\",\"rssi\":-52}")
          && strstr(text, "\"memory\":{\"free_heap_bytes\":120000,\"free_internal_bytes\":80000}"));
    free(text);
    synced = true;
    esp_mcp_tool_result_t result = {0};
    esp_mcp_property_list_t args = {.volume = 28};
    CHECK(callback(mcp, 4)(&args, &result) == ESP_OK && current_volume == 28 && result.calls == 1);
    CHECK(!strcmp(result.text, "{\"volume\":28,\"saved\":true,\"applied_at\":\"2025-01-01T00:00:00+00:00\"}"));
    free(result.text);
    result = (esp_mcp_tool_result_t){0};
    args.volume = 101;
    CHECK(callback(mcp, 4)(&args, &result) == ESP_ERR_INVALID_ARG && !result.calls && current_volume == 28);
    args.volume = 0;
    volume_error = ESP_FAIL;
    CHECK(callback(mcp, 4)(&args, &result) == ESP_FAIL && !result.calls && current_volume == 28);
    text = invoke(mcp, 3);
    CHECK(strstr(text, "28"));
    free(text);
    puts("PASS MCP speaker status, set, range and failure");
}

#endif

static void publish(const weather_t *weather, const sensor_reading_t *sensor, bool retrying, bool updated)
{
#if TEST_CURRENT_MCP
    forecast_stamp_t fetched = {
        .fetched_at = net_synchronized_time(),
        .fetched_uptime_us = esp_timer_get_time(),
    };
    device_mcp_publish_forecast(weather, updated ? &fetched : NULL, retrying);
    device_mcp_publish_sensor(sensor);
#else
    device_mcp_publish(weather, sensor, retrying, updated);
#endif
}

static void descriptions(esp_mcp_t *mcp)
{
    for (int i = 0; i < mcp->count; i++) {
        cJSON *json = cJSON_CreateObject();
        CHECK(json && cJSON_AddStringToObject(json, "description", mcp->tools[i]->description));
        char *text = cJSON_PrintUnformatted(json);
        CHECK(text);
        printf("%s\t%s\n", mcp->tools[i]->name, text);
        cJSON_free(text);
        cJSON_Delete(json);
    }
}

static void behavior(esp_mcp_t *mcp)
{
    weather_t w = fixture(0);
    weather_t partial = fixture(1);
    partial.ok = false;
    sensor_reading_t sensor = {.valid = true, .temperature_c = 23.75f, .humidity_percent = 61.5f, SENSOR_PRESSURE(1013.25f)};
    emit(mcp, "initial_time", 0);
    emit(mcp, "initial_forecast", 1);
    emit(mcp, "initial_sensor", 2);
    synced = true;
    emit(mcp, "synced_no_timezone", 0);
    publish(&partial, &sensor, true, true);
    emit(mcp, "first_failure", 1);
    synced = false;
    publish(&w, &sensor, false, true);
    emit(mcp, "unknown_fetch", 1);
    emit(mcp, "unsynced_timezone", 0);
    synced = true;
    wall = EPOCH + 7;
    uptime = 13500000;
    emit(mcp, "sntp_does_not_date_old_cache", 1);
    emit(mcp, "east8", 0);
    wall = EPOCH + 8;
    emit(mcp, "east8_next_second", 0);
    publish(&w, &sensor, false, false);
    emit(mcp, "republish_does_not_date_old_cache", 1);
    wall = EPOCH;
    uptime = 20000000;
    publish(&w, &sensor, false, true);
    emit(mcp, "success", 1);
    emit(mcp, "real_sensor", 2);
    /* Caller-owned inputs must be copied, not retained by pointer. */
    memset(&w, 0, sizeof(w));
    memset(&sensor, 0, sizeof(sensor));
    emit(mcp, "copied_forecast", 1);
    emit(mcp, "copied_sensor", 2);
    uptime = 22500000;
    wall = EPOCH + 864000;
    emit(mcp, "wall_forward", 1);
    wall = EPOCH - 864000;
    uptime = 23750000;
    emit(mcp, "wall_backward", 1);
    publish(&partial, NULL, true, true);
    emit(mcp, "failed_partial", 1);
    emit(mcp, "null_sensor", 2);
    w = fixture(1);
    publish(&w, NULL, false, false);
    emit(mcp, "not_an_update", 1);
    uptime = 1819999999;
    emit(mcp, "before_stale", 1);
    uptime = 1820000000;
    emit(mcp, "at_stale", 1);
    wall = EPOCH + 1800;
    publish(&w, NULL, false, true);
    emit(mcp, "recovered", 1);
    emit(mcp, "negative_half_hour", 0);
    w.utc_offset = 19800;
    publish(&w, NULL, false, true);
    emit(mcp, "positive_half_hour", 0);
    int offsets[] = {0, 50400, -50400, 50460, -50460, 1};
    const char *labels[] = {"utc", "plus14", "minus14", "offset_too_high", "offset_too_low", "offset_seconds"};
    for (int i = 0; i < 6; i++) {
        w.utc_offset = offsets[i];
        publish(&w, NULL, false, true);
        emit(mcp, labels[i], 0);
    }
    w.utc_offset = 28800;
    publish(&w, NULL, false, true);
    wall = 1704067199;
    emit(mcp, "reset_clock", 0);
    wall = 1704067200;
    emit(mcp, "minimum_clock", 0);
    const sensor_reading_t samples[] = {
        {.valid = false, .temperature_c = 25, .humidity_percent = 50},
        {.valid = true, .temperature_c = NAN, .humidity_percent = 50},
        {.valid = true, .temperature_c = INFINITY, .humidity_percent = 50},
        {.valid = true, .temperature_c = -INFINITY, .humidity_percent = 50},
        {.valid = true, .temperature_c = 25, .humidity_percent = NAN},
        {.valid = true, .temperature_c = 25, .humidity_percent = INFINITY},
        {.valid = true, .temperature_c = 25, .humidity_percent = -INFINITY},
        {.valid = true, .temperature_c = 25, .humidity_percent = -0.25f},
        {.valid = true, .temperature_c = 25, .humidity_percent = 100.25f},
        {.valid = true, .temperature_c = -12.5f, .humidity_percent = 0, SENSOR_PRESSURE(299.5f)},
        {.valid = true, .temperature_c = 35.25f, .humidity_percent = 100, SENSOR_PRESSURE(1012.5f)},
    };
    for (int i = 0; i < 11; i++) {
        char label[32];
        snprintf(label, sizeof(label), "sensor_%d", i);
        publish(NULL, &samples[i], false, false);
        emit(mcp, label, 2);
    }
}

static void allocation_sweep(esp_mcp_t *mcp, int tool, const char *label)
{
    allocations = 0;
    char *expected = invoke(mcp, tool);
    long total = atomic_load(&allocations);
    CHECK(total > 0 && atomic_load(&live_json) == 0);
    for (long nth = 1; nth <= total; nth++) {
        allocations = 0;
        fail_at = nth;
        esp_mcp_tool_result_t result = {0};
        esp_err_t err = callback(mcp, tool)(NULL, &result);
        fail_at = -1;
        if (err != ESP_ERR_NO_MEM || result.calls != 0 || result.text || atomic_load(&live_json) != 0) {
            fprintf(stderr, "allocation failure: %s allocation %ld/%ld, err=%d calls=%d live=%ld\n",
                    label, nth, total, err, result.calls, atomic_load(&live_json));
            exit(1);
        }
        /* Recovery must be complete, identical, and leave the cached snapshot intact. */
        char *recovered = invoke(mcp, tool);
        CHECK(strcmp(expected, recovered) == 0);
        free(recovered);
        CHECK(atomic_load(&live_json) == 0);
    }
    for (int i = 0; i < 2; i++) {
        esp_mcp_tool_result_t result = {.error = i ? ESP_ERR_NO_MEM : ESP_FAIL};
        CHECK(callback(mcp, tool)(NULL, &result) == result.error);
        CHECK(result.calls == 1 && result.text == NULL && atomic_load(&live_json) == 0);
    }
    free(expected);
    printf("PASS allocation: %s (%ld failure points + builder errors)\n", label, total);
}

static void allocation_tests(esp_mcp_t *mcp)
{
    allocation_sweep(mcp, 0, "unavailable time");
    allocation_sweep(mcp, 1, "unavailable forecast");
    allocation_sweep(mcp, 2, "unavailable sensor");
    weather_t w = fixture(0);
    sensor_reading_t sensor = {.valid = true, .temperature_c = 23.75f, .humidity_percent = 61.5f, SENSOR_PRESSURE(1013.25f)};
    publish(&w, &sensor, false, true);
    allocation_sweep(mcp, 1, "unknown fetch time");
    synced = true;
    publish(&w, &sensor, false, true);
    allocation_sweep(mcp, 0, "synchronized time");
    allocation_sweep(mcp, 1, "complete 5-day/21-hour forecast");
    allocation_sweep(mcp, 2, "valid sensor");
    sensor.temperature_c = NAN;
    publish(NULL, &sensor, true, false);
    allocation_sweep(mcp, 2, "invalid sensor");
    allocation_sweep(mcp, 1, "retrying forecast");
}

static cJSON *field(cJSON *json, const char *name)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(json, name);
    CHECK(item);
    return item;
}

static void number(cJSON *json, const char *name, double expected)
{
    cJSON *item = field(json, name);
    CHECK(cJSON_IsNumber(item) && item->valuedouble == expected);
}

static cJSON *invoke_json(esp_mcp_t *mcp, int index)
{
    char *text = invoke(mcp, index);
    const char *end = NULL;
    cJSON *json = cJSON_ParseWithOpts(text, &end, 1);
    CHECK(json && cJSON_IsObject(json) && end && *end == '\0');
    free(text);
    return json;
}

#if TEST_CURRENT_MCP
static void sample_times(esp_mcp_t *mcp)
{
    sensor_reading_t sample = {.valid = true, .temperature_c = 23, .humidity_percent = 56};
    device_mcp_publish_sensor(&sample);
    synced = true;
    wall = EPOCH + 9;
    uptime = 14500000;
    weather_t w = fixture(0);
    // A forecast received before SNTP must keep its acquisition stamp when published later.
    forecast_stamp_t fetched = {.fetched_uptime_us = 12000000};
    device_mcp_publish_forecast(&w, &fetched, false);
    cJSON *json = invoke_json(mcp, 2);
    CHECK(cJSON_IsNull(field(json, "sampled_at")));
    number(json, "sample_age_seconds", 4.5);
    cJSON_Delete(json);
    json = invoke_json(mcp, 1);
    CHECK(cJSON_IsNull(field(json, "fetched_at")));
    number(json, "age", 2.5);
    cJSON_Delete(json);

    device_mcp_publish_sensor(&sample);
    wall = EPOCH + 1000;
    uptime = 15750000;
    device_mcp_publish_forecast(&w, NULL, true);
    json = invoke_json(mcp, 2);
    cJSON *at = field(json, "sampled_at");
    CHECK(cJSON_IsString(at) && !strcmp(at->valuestring, "2025-01-01T08:00:09+08:00"));
    number(json, "sample_age_seconds", 1.25);
    number(json, "temperature_c", 23);
    cJSON_Delete(json);
    puts("PASS acquisition timestamps survive SNTP sync, clock jumps and forecast publication");
}
#endif

static _Atomic bool start_threads, stop_writer;
static _Atomic long publications;

static void *writer(void *unused)
{
    (void)unused;
    weather_t variants[] = {fixture(0), fixture(1)};
    sensor_reading_t sensors[] = {
        {.valid = true, .temperature_c = 10.25f, .humidity_percent = 20.5f},
        {.valid = true, .temperature_c = 30.25f, .humidity_percent = 40.5f},
    };
    while (!atomic_load(&start_threads)) sched_yield();
    int v = 0;
    while (!atomic_load(&stop_writer)) {
        publish(&variants[v], &sensors[v], v != 0, true);
        atomic_fetch_add(&publications, 1);
        v = !v;
        sched_yield();
    }
    return NULL;
}

static void *reader(void *engine)
{
    esp_mcp_t *mcp = engine;
    while (!atomic_load(&start_threads)) sched_yield();
    for (int n = 0; n < 1200; n++) {
        cJSON *json = invoke_json(mcp, 1);
        cJSON *city = field(json, "city");
        CHECK(cJSON_IsString(city));
        int v = strcmp(city->valuestring, "Other city") == 0;
        weather_t expected = fixture(v);
        CHECK(strcmp(city->valuestring, expected.city) == 0);
        CHECK(cJSON_IsTrue(field(json, "available")));
        CHECK(cJSON_IsTrue(field(json, "fetched_at_known")));
        CHECK(cJSON_IsBool(field(json, "retrying")) && cJSON_IsTrue(field(json, "retrying")) == v);
        CHECK(cJSON_IsBool(field(json, "stale")) && cJSON_IsTrue(field(json, "stale")) == v);
        number(json, "fetched_at", EPOCH);
        number(json, "age", 0);
        number(json, "utc_offset_seconds", expected.utc_offset);
        cJSON *days = field(json, "days"), *hours = field(json, "hours");
        CHECK(cJSON_IsArray(days) && cJSON_GetArraySize(days) == 5);
        CHECK(cJSON_IsArray(hours) && cJSON_GetArraySize(hours) == 21);
        for (int i = 0; i < 5; i++) {
            cJSON *d = cJSON_GetArrayItem(days, i);
            cJSON *date = field(d, "date");
            CHECK(cJSON_IsString(date) && strcmp(date->valuestring, expected.days[i].date) == 0);
            number(d, "temperature_min_c", expected.days[i].tmin);
            number(d, "temperature_max_c", expected.days[i].tmax);
            number(d, "weather_code", expected.days[i].code);
            number(d, "precip_probability", expected.days[i].precip_probability);
        }
        for (int i = 0; i < 21; i++) {
            cJSON *h = cJSON_GetArrayItem(hours, i);
            number(h, "hour", expected.hours[i].hour);
            number(h, "temperature_c", expected.hours[i].temperature_c);
            number(h, "weather_code", expected.hours[i].code);
            number(h, "precip_probability", expected.hours[i].precip_probability);
        }
        cJSON_Delete(json);
        /* Separate calls can observe different generations, but never torn pairs. */
        json = invoke_json(mcp, 2);
        CHECK(cJSON_IsTrue(field(json, "available")));
        cJSON *temperature = field(json, "temperature_c");
        CHECK(cJSON_IsNumber(temperature));
        CHECK(temperature->valuedouble == 10.25 || temperature->valuedouble == 30.25);
        number(json, "humidity_percent", temperature->valuedouble == 10.25 ? 20.5 : 40.5);
        cJSON_Delete(json);
    }
    return NULL;
}

static void concurrent(esp_mcp_t *mcp)
{
    synced = true;
    weather_t w = fixture(0);
    sensor_reading_t sensor = {.valid = true, .temperature_c = 10.25f, .humidity_percent = 20.5f};
    publish(&w, &sensor, false, true);
    pthread_t producer, consumers[2];
    CHECK(pthread_create(&producer, NULL, writer, NULL) == 0);
    for (int i = 0; i < 2; i++) CHECK(pthread_create(&consumers[i], NULL, reader, mcp) == 0);
    start_threads = true;
    for (int i = 0; i < 2; i++) CHECK(pthread_join(consumers[i], NULL) == 0);
    stop_writer = true;
    CHECK(pthread_join(producer, NULL) == 0);
    CHECK(atomic_load(&publications) > 100 && atomic_load(&live_json) == 0);
    puts("PASS concurrency: 2400 full forecast + 2400 sensor reads under concurrent publication");
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    cJSON_Hooks hooks = {.malloc_fn = json_malloc, .free_fn = json_free};
    cJSON_InitHooks(&hooks);
    if (strcmp(argv[1], "registration") == 0) {
        registration();
    } else {
        esp_mcp_t engine = {0};
        CHECK(device_mcp_register(&engine) == ESP_OK);
        if (strcmp(argv[1], "behavior") == 0) behavior(&engine);
        else if (strcmp(argv[1], "descriptions") == 0) descriptions(&engine);
        else if (strcmp(argv[1], "allocation") == 0) allocation_tests(&engine);
        else if (strcmp(argv[1], "concurrent") == 0) concurrent(&engine);
#if TEST_CURRENT_MCP
        else if (strcmp(argv[1], "volume") == 0) volume(&engine);
        else if (strcmp(argv[1], "sample_times") == 0) sample_times(&engine);
#endif
        else CHECK(!"unknown scenario");
        CHECK(esp_mcp_destroy(&engine) == ESP_OK && live_tools == 0);
    }
    CHECK(atomic_load(&live_json) == 0 && lock_depth == 0);
    cJSON_InitHooks(NULL);
    return 0;
}
'''


def require(condition, message):
    if not condition:
        raise AssertionError(message)


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


def run(command, work, env, timeout=60):
    result = subprocess.run(
        [str(arg) for arg in command], cwd=work, env=env,
        capture_output=True, text=True, timeout=timeout, check=False,
    )
    if result.returncode:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(map(str, command))}\n"
            f"{result.stdout}{result.stderr}"
        )
    if result.stderr:
        print(result.stderr, file=sys.stderr, end="")
    return result.stdout


def expected_forecast(variant, *, legacy=False):
    return {
        "city": "Other city" if variant else 'Cache "city"',
        "utc_offset_seconds": -12600 if variant else 28800,
        "days": [
            {
                "date": f"2025-01-{i + 1 + variant * 10:02d}",
                "temperature_min_c": -8.5 + i + variant * 20,
                "temperature_max_c": 1.25 + i + variant * 20,
                "weather_code": (i * 23 + variant) % 100,
                "precip_probability": i * 24.5 + variant,
            }
            for i in range(5)
        ],
        "hours": [
            {
                "hour": (20 + i + variant) % 24,
                **({} if legacy else {"date": f"2025-01-{1 + variant * 10 + (20 + i + variant) // 24:02d}"}),
                "temperature_c": -4.75 + i * 0.5 + variant * 20,
                "weather_code": (i * 7 + variant) % 100,
                "precip_probability": i * 4.5 + variant,
            }
            for i in range(21)
        ],
    }


LEGACY_FORECAST_METADATA = {
    "age_unit": "seconds_monotonic",
    "weather_code_system": "WMO; unmapped codes are unknown, not clear sky",
    "precip_probability_unit": "percent; forecast probability, not observed rain",
    "hours_time_basis": "ordered at fetch; local hour only; does not roll forward",
}


def parse_records(output):
    records = {}

    def unique_object(pairs):
        value = {}
        for key, item in pairs:
            require(key not in value, f"duplicate JSON member: {key}")
            value[key] = item
        return value

    def reject_constant(value):
        raise AssertionError(f"non-JSON numeric constant: {value}")

    for line in output.splitlines():
        label, text = line.split("\t", 1)
        require(label not in records, f"duplicate record: {label}")
        value = json.loads(text, object_pairs_hook=unique_object, parse_constant=reject_constant)
        require(isinstance(value, dict), f"{label}: result must be an object")
        records[label] = value
    return records


def same_json(actual, expected):
    # JSON has one number type; Python bool must not pass as numeric 0/1.
    if type(expected) in (int, float):
        return type(actual) in (int, float) and actual == expected
    if type(actual) is not type(expected):
        return False
    if isinstance(expected, dict):
        return actual.keys() == expected.keys() and all(
            same_json(actual[key], value) for key, value in expected.items()
        )
    if isinstance(expected, list):
        return len(actual) == len(expected) and all(map(same_json, actual, expected))
    return actual == expected


def validate_descriptions(output, *, legacy=False):
    records = parse_records(output)
    names = {"self.get_current_time", "self.get_weather_forecast", "self.get_sensor_reading"}
    if not legacy:
        names.update(("self.get_device_status", "self.audio_speaker.set_volume"))
    require(set(records) == names, "descriptions: unexpected tool list")
    for name, value in records.items():
        require(set(value) == {"description"} and isinstance(value["description"], str)
                and value["description"].strip(), f"{name}: missing tool description")
    if not legacy:
        description = records["self.get_weather_forecast"]["description"].casefold()
        for phrase in (
            "wmo", "unknown", "percent", "monotonic seconds", "not observed rain",
            "at fetch", "local date", "repeated local hour",
        ):
            require(phrase in description, f"weather description missing semantics: {phrase!r}")
    print(f"PASS descriptions: {len(names)} tools{' (legacy)' if legacy else '; forecast unit/time semantics'}")


def validate_behavior(output, *, legacy=False):
    records = parse_records(output)
    checked = set()

    def expect(label, expected):
        checked.add(label)
        require(label in records, f"missing result: {label}")
        actual = records[label]
        require(
            same_json(actual, expected),
            f"{label}:\nexpected {expected!r}\nactual   {actual!r}",
        )

    def unavailable_time(label, timezone_known):
        expect(label, {
            "synchronized": False, "source": "device_clock_sntp",
            "timezone_known": timezone_known, "local_time": None,
            "reason": "clock_not_synchronized" if timezone_known else "timezone_unknown",
        })

    epoch = 1735689600

    def clock(label, now, offset):
        timezone = dt.timezone(dt.timedelta(seconds=offset))
        expect(label, {
            "synchronized": True, "source": "device_clock_sntp", "timezone_known": True,
            "local_time": dt.datetime.fromtimestamp(now, timezone).isoformat(timespec="seconds"),
            "unix_time": now, "utc_offset_seconds": offset,
        })

    def forecast(label, variant=0, *, age: float = 0, fetched_at: "int | None" = epoch, retrying=False):
        known = fetched_at is not None
        next_refresh = None if retrying else max(0.0, 1800.0 - age)
        expect(label, {
            "available": True, "source": "Open-Meteo_cached_forecast", "retrying": retrying,
            **expected_forecast(variant, legacy=legacy), "fetched_at_known": known, "fetched_at": fetched_at,
            "age": age, "stale": retrying or not known or age >= 1800,
            **({} if legacy else {
                "refresh_interval_seconds": 1800,
                "next_refresh_in_seconds": next_refresh,
            }),
            **(LEGACY_FORECAST_METADATA if legacy else {}),
        })

    def sensor(label, temperature=None, humidity=None, pressure=None, reason=None, sampled_at=None, extra=None):
        expected = {
            "available": reason is None, "source": "local_sensor",
            **({} if legacy else {"sensor_model": "unknown"}),
            "temperature_c": temperature, "humidity_percent": humidity,
            **({} if legacy else {"pressure_hpa": pressure}),
        }
        if reason:
            expected["reason"] = reason
        elif not legacy:
            expected.update(sampled_at=sampled_at, sample_age_seconds=0)
        if extra and not legacy:
            expected.update(extra)
        expect(label, expected)

    unavailable_time("initial_time", False)
    unavailable_time("synced_no_timezone", False)
    for label, retrying in (("initial_forecast", False), ("first_failure", True)):
        expect(label, {
            "available": False, "source": "Open-Meteo_cached_forecast",
            "retrying": retrying, "reason": "forecast_not_available",
        })
    sensor("initial_sensor", reason="sensor_unavailable")
    forecast("unknown_fetch", fetched_at=None)
    unavailable_time("unsynced_timezone", True)
    forecast("sntp_does_not_date_old_cache", fetched_at=None, age=3.5)
    clock("east8", epoch + 7, 28800)
    clock("east8_next_second", epoch + 8, 28800)
    forecast("republish_does_not_date_old_cache", fetched_at=None, age=3.5)
    for label in ("success", "copied_forecast"):
        forecast(label)
    for label in ("real_sensor", "copied_sensor"):
        sensor(label, 23.75, 61.5, 1013.25, sampled_at="2025-01-01T08:00:00+08:00")
    forecast("wall_forward", age=2.5)
    forecast("wall_backward", age=3.75)
    forecast("failed_partial", age=3.75, retrying=True)
    sensor("null_sensor", reason="sensor_unavailable")
    forecast("not_an_update", age=3.75)
    forecast("before_stale", age=1799.999999)
    forecast("at_stale", age=1800)
    forecast("recovered", 1, fetched_at=epoch + 1800)
    clock("negative_half_hour", epoch + 1800, -12600)
    clock("positive_half_hour", epoch + 1800, 19800)
    for label, offset in (("utc", 0), ("plus14", 50400), ("minus14", -50400)):
        clock(label, epoch + 1800, offset)
    for label in ("offset_too_high", "offset_too_low", "offset_seconds"):
        unavailable_time(label, False)
    unavailable_time("reset_clock", True)
    clock("minimum_clock", 1704067200, 28800)
    for i in range(9):
        sensor(f"sensor_{i}", reason="sensor_unavailable" if i == 0 else "invalid_sensor_reading")
    sensor("sensor_9", -12.5, 0, None, sampled_at="2024-01-01T08:00:00+08:00")
    sensor("sensor_10", 35.25, 100, 1012.5, sampled_at="2024-01-01T08:00:00+08:00", extra={
        "pressure_delta_hpa": -0.75, "pressure_trend": "steady", "pressure_trend_window_minutes": 30,
    })
    require(checked == records.keys(), f"unvalidated records: {records.keys() - checked}")
    require(len(checked) == 44, "behavior scenario must retain all 44 results")
    print(f"PASS behavior: {len(checked)} results; time, sensor, full forecast and cache lifecycle")
    return records


def compare_behavior(before, after):
    require(before.keys() == after.keys(), "baseline/after behavior labels differ")
    for label, old in before.items():
        new = after[label]
        normalized = dict(old)
        if old.get("source") == "Open-Meteo_cached_forecast":
            for key, expected in LEGACY_FORECAST_METADATA.items():
                require(key not in new, f"{label}: removed metadata {key} still present after")
                if old["available"]:
                    require(key in old and same_json(old[key], expected),
                            f"{label}: incorrect/missing baseline metadata {key}")
                    del normalized[key]
                else:
                    require(key not in old, f"{label}: unavailable baseline has metadata {key}")
        comparable = dict(new)
        if old.get("source") == "local_sensor":
            for key in ("sensor_model", "sampled_at", "sample_age_seconds", "pressure_hpa",
                        "pressure_delta_hpa", "pressure_trend", "pressure_trend_window_minutes"):
                comparable.pop(key, None)
        if old.get("source") == "Open-Meteo_cached_forecast":
            comparable.pop("refresh_interval_seconds", None)
            comparable.pop("next_refresh_in_seconds", None)
        if "hours" in comparable:
            comparable["hours"] = [{key: val for key, val in hour.items() if key != "date"}
                                   for hour in comparable["hours"]]
        require(same_json(comparable, normalized),
                f"{label}: dynamic baseline/after mismatch\nbaseline {normalized!r}\nafter    {new!r}")
    print("PASS baseline equivalence: 44 results; verified static metadata removal and additive hourly dates")


def max_forecast_bytes(output):
    records = parse_records(output)
    return max(
        len(line.split("\t", 1)[1].encode("utf-8"))
        for line in output.splitlines()
        if records[line.split("\t", 1)[0]].get("source") == "Open-Meteo_cached_forecast"
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--sanitize", action="store_true", help="enable host AddressSanitizer and UBSan")
    parser.add_argument("--baseline", type=Path, help="old main/ directory; compare all 44 behavior results")
    args = parser.parse_args()
    project = Path(__file__).resolve().parents[1]
    compiler = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")
    require(compiler, "no host C compiler found (cc/clang/gcc)")
    cjson_dir, cjson_source = locate_cjson(project)
    sdk_include = project / "managed_components" / "espressif__mcp-c-sdk" / "include"
    for name in ("esp_mcp_engine.h", "esp_mcp_tool.h", "esp_mcp_property.h"):
        require((sdk_include / name).is_file(), f"missing managed MCP header: {sdk_include / name}")
    sources = {"after": project / "main"}
    if args.baseline:
        sources = {"baseline": args.baseline.resolve(), **sources}
    for source in sources.values():
        for name in ("device_mcp.c", "device_mcp.h"):
            require((source / name).is_file(), f"missing device module: {source / name}")
    artifacts = project / ".cache" / "mcp-ablation" if args.baseline else None
    if artifacts:
        artifacts.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, LC_ALL="C", TZ="UTC0")
    if args.sanitize:
        env["ASAN_OPTIONS"] = "halt_on_error=1:abort_on_error=1"
        env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    with tempfile.TemporaryDirectory(prefix="weather-test-device-mcp-") as directory:
        work = Path(directory)
        for name, text in HEADERS.items():
            path = work / "stubs" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        harness = work / "harness.c"
        harness.write_text(HARNESS, encoding="utf-8")
        flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"] if args.sanitize else []
        behaviors = {}
        measurements = {}
        for label, source in sources.items():
            print(f"Testing {label}: {source}")
            executable = work / f"device_mcp-{label}.out"
            run([
                compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Dtime=device_test_time",
                f"-DTEST_CURRENT_MCP={int(label != 'baseline')}", "-Wall", "-Wextra", "-Werror", "-O1", *flags,
                "-I", work / "stubs", "-I", source, "-I", sdk_include, "-I", cjson_dir,
                source / "device_mcp.c", cjson_source, harness, "-pthread", "-lm", "-o", executable,
            ], work, env)
            scenarios = ["registration", "behavior", "descriptions", "allocation", "concurrent"]
            if label != "baseline":
                scenarios.extend(("volume", "sample_times"))
            for scenario in scenarios:
                output = run([executable, scenario], work, env)
                if artifacts:
                    (artifacts / f"{label}-{scenario}.txt").write_text(output, encoding="utf-8")
                if scenario == "behavior":
                    behaviors[label] = validate_behavior(output, legacy=label == "baseline")
                    data = (source / "device_mcp.c").read_bytes()
                    measurements[label] = {
                        "max_forecast_json_bytes": max_forecast_bytes(output),
                        "source_bytes": len(data),
                        "source_lines": len(data.splitlines()),
                        "source_nonblank_lines": sum(bool(line.strip()) for line in data.splitlines()),
                    }
                elif scenario == "descriptions":
                    validate_descriptions(output, legacy=label == "baseline")
                else:
                    print(output, end="")
        if args.baseline:
            assert artifacts is not None
            compare_behavior(behaviors["baseline"], behaviors["after"])
            before, after = (measurements[label]["max_forecast_json_bytes"] for label in ("baseline", "after"))
            measurements["forecast_json_bytes_saved"] = before - after
            print(f"Max weather JSON text (44-result fixtures, UTF-8): baseline={before} B, "
                  f"after={after} B, saved={before - after} B ({(before - after) / before:.2%})")
            for label in ("baseline", "after"):
                item = measurements[label]
                print(f"device_mcp.c {label}: {item['source_bytes']} bytes, {item['source_lines']} lines, "
                      f"{item['source_nonblank_lines']} nonblank lines")
            (artifacts / "measurements.json").write_text(
                json.dumps(measurements, indent=2) + "\n", encoding="utf-8",
            )
            print(f"Raw scenario outputs and measurements: {artifacts.relative_to(project)}")
    print(f"PASS device MCP host regression{' (ASan/UBSan)' if args.sanitize else ''}")
    print("Scope: API stubs do not validate MCP protocol/SDK implementation, real SNTP/networking, or I2S.")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
