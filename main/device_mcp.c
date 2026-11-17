#include "device_mcp.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "cJSON.h"
#include "esp_mcp_engine.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "net.h"

#define FORECAST_STALE_SECONDS (30 * 60)
#define MIN_SYNCED_EPOCH       1704067200 // 2024-01-01; reject an unset/reset system clock

typedef struct {
    weather_t weather;
    sensor_reading_t sensor;
    bool retrying;
    time_t fetched_at;
    int64_t fetched_uptime_us;
} device_snapshot_t;

static device_snapshot_t s_snapshot;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

void device_mcp_publish(const weather_t *weather, const sensor_reading_t *sensor, bool retrying, bool forecast_updated)
{
    time_t now = time(NULL);
    bool clock_known = net_time_is_synced() && now >= MIN_SYNCED_EPOCH;
    int64_t uptime = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    if (forecast_updated && weather && weather->ok) {
        s_snapshot.weather = *weather;

        s_snapshot.fetched_at = clock_known ? now : 0;
        s_snapshot.fetched_uptime_us = uptime;
    }
    s_snapshot.sensor = sensor ? *sensor : (sensor_reading_t){0};
    s_snapshot.retrying = retrying;
    portEXIT_CRITICAL(&s_lock);
}

static device_snapshot_t snapshot(void)
{
    device_snapshot_t copy;
    // Never allocate JSON or wait for network/audio while holding this lock.
    portENTER_CRITICAL(&s_lock);
    copy = s_snapshot;
    portEXIT_CRITICAL(&s_lock);
    return copy;
}

static esp_err_t send_json(esp_mcp_tool_result_t *result, cJSON *json, bool complete)
{
    char *text = complete ? cJSON_PrintUnformatted(json) : NULL;
    cJSON_Delete(json);
    if (!text) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_mcp_tool_result_add_text(result, text);
    cJSON_free(text);
    return err;
}

static esp_err_t get_time(const esp_mcp_property_list_t *properties, esp_mcp_tool_result_t *result)
{
    (void)properties;
    device_snapshot_t copy = snapshot();
    time_t now = time(NULL);
    int offset = copy.weather.utc_offset;
    bool timezone_known = copy.weather.ok && offset >= -14 * 3600 && offset <= 14 * 3600 && offset % 60 == 0;
    bool synchronized = net_time_is_synced() && now >= MIN_SYNCED_EPOCH && timezone_known;
    char iso[40] = {0};
    if (synchronized) {
        // Use the same numeric UTC offset as the forecast; do not race global TZ updates.
        time_t local = now + offset;
        struct tm tm;
        synchronized = gmtime_r(&local, &tm) != NULL;
        if (synchronized) {
            char date[24];
            synchronized = strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &tm) != 0;
            if (synchronized) {
                snprintf(iso, sizeof(iso), "%s%c%02d:%02d", date, offset < 0 ? '-' : '+', abs(offset / 3600),
                         abs(offset % 3600) / 60);
            }
        }
    }
    cJSON *json = cJSON_CreateObject();
    bool ok = json && cJSON_AddBoolToObject(json, "synchronized", synchronized)
              && cJSON_AddStringToObject(json, "source", "device_clock_sntp")
              && cJSON_AddBoolToObject(json, "timezone_known", timezone_known);
    if (ok && synchronized) {
        ok = cJSON_AddStringToObject(json, "local_time", iso) && cJSON_AddNumberToObject(json, "unix_time", (double)now)
             && cJSON_AddNumberToObject(json, "utc_offset_seconds", offset);
    } else if (ok) {
        ok = cJSON_AddNullToObject(json, "local_time")
             && cJSON_AddStringToObject(json, "reason", timezone_known ? "clock_not_synchronized" : "timezone_unknown");
    }
    return send_json(result, json, ok);
}

static esp_err_t get_sensor(const esp_mcp_property_list_t *properties, esp_mcp_tool_result_t *result)
{
    (void)properties;
    sensor_reading_t sensor = snapshot().sensor;
    bool available = sensor.valid && isfinite(sensor.temperature_c) && isfinite(sensor.humidity_percent)
                     && sensor.humidity_percent >= 0 && sensor.humidity_percent <= 100;
    cJSON *json = cJSON_CreateObject();
    bool ok = json && cJSON_AddBoolToObject(json, "available", available)
              && cJSON_AddStringToObject(json, "source", "local_sensor");
    if (ok && available) {
        ok = cJSON_AddNumberToObject(json, "temperature_c", sensor.temperature_c)
             && cJSON_AddNumberToObject(json, "humidity_percent", sensor.humidity_percent);
    } else if (ok) {
        ok = cJSON_AddNullToObject(json, "temperature_c") && cJSON_AddNullToObject(json, "humidity_percent")
             && cJSON_AddStringToObject(json, "reason", sensor.valid ? "invalid_sensor_reading" : "sensor_unavailable");
    }
    return send_json(result, json, ok);
}

static cJSON *day_json(const weather_day_t *day)
{
    cJSON *json = cJSON_CreateObject();
    if (!json || !cJSON_AddStringToObject(json, "date", day->date)
        || !cJSON_AddNumberToObject(json, "temperature_min_c", day->tmin)
        || !cJSON_AddNumberToObject(json, "temperature_max_c", day->tmax)
        || !cJSON_AddNumberToObject(json, "weather_code", day->code)
        || !cJSON_AddNumberToObject(json, "precip_probability", day->precip_probability))
    {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

static cJSON *hour_json(const weather_hour_t *hour)
{
    cJSON *json = cJSON_CreateObject();
    if (!json || !cJSON_AddNumberToObject(json, "hour", hour->hour)
        || !cJSON_AddNumberToObject(json, "temperature_c", hour->temperature_c)
        || !cJSON_AddNumberToObject(json, "weather_code", hour->code)
        || !cJSON_AddNumberToObject(json, "precip_probability", hour->precip_probability))
    {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

static esp_err_t get_forecast(const esp_mcp_property_list_t *properties, esp_mcp_tool_result_t *result)
{
    (void)properties;
    device_snapshot_t copy = snapshot();
    double age = (double)(esp_timer_get_time() - copy.fetched_uptime_us) / 1000000.0;
    if (age < 0) {
        age = 0;
    }
    bool fetched_at_known = copy.fetched_at >= MIN_SYNCED_EPOCH;
    bool stale = copy.retrying || !fetched_at_known || age >= FORECAST_STALE_SECONDS;
    cJSON *json = cJSON_CreateObject();
    bool ok = json && cJSON_AddBoolToObject(json, "available", copy.weather.ok)
              && cJSON_AddStringToObject(json, "source", "Open-Meteo_cached_forecast")
              && cJSON_AddBoolToObject(json, "retrying", copy.retrying);
    if (!copy.weather.ok) {
        ok = ok && cJSON_AddStringToObject(json, "reason", "forecast_not_available");
        return send_json(result, json, ok);
    }
    ok = ok && cJSON_AddStringToObject(json, "city", copy.weather.city)
         && cJSON_AddNumberToObject(json, "utc_offset_seconds", copy.weather.utc_offset)
         && cJSON_AddBoolToObject(json, "fetched_at_known", fetched_at_known)
         && (fetched_at_known ? cJSON_AddNumberToObject(json, "fetched_at", (double)copy.fetched_at)
                              : cJSON_AddNullToObject(json, "fetched_at"))
         && cJSON_AddNumberToObject(json, "age", age) && cJSON_AddBoolToObject(json, "stale", stale);
    cJSON *days = ok ? cJSON_AddArrayToObject(json, "days") : NULL;
    cJSON *hours = ok ? cJSON_AddArrayToObject(json, "hours") : NULL;
    ok = ok && days && hours;
    for (int i = 0; ok && i < WEATHER_DAYS; i++) {
        cJSON *day = day_json(&copy.weather.days[i]);
        ok = day && cJSON_AddItemToArray(days, day);
        if (!ok) {
            cJSON_Delete(day);
        }
    }
    for (int i = 0; ok && i < WEATHER_HOURS; i++) {
        cJSON *hour = hour_json(&copy.weather.hours[i]);
        ok = hour && cJSON_AddItemToArray(hours, hour);
        if (!ok) {
            cJSON_Delete(hour);
        }
    }
    return send_json(result, json, ok);
}

esp_err_t device_mcp_register(esp_mcp_t *mcp)
{
    if (!mcp) {
        return ESP_ERR_INVALID_ARG;
    }
    static const struct {
        const char *name;
        const char *description;
        esp_mcp_tool_callback_ex_t callback;
    } tools[] = {
        {"self.get_current_time", "Read device local time; synchronized=false means time is unavailable.", get_time},
        {"self.get_weather_forecast",
         "Read cached five-day/21-hour forecast; no network. age: monotonic seconds; weather_code: WMO, "
         "unmapped=unknown. "
         "precip_probability: forecast percent, not observed rain. Hours: ordered at fetch, local hour only, not "
         "rolling.",
         get_forecast},
        {"self.get_sensor_reading", "Read actual local temperature/humidity; available=false means no valid reading.",
         get_sensor},
    };
    for (size_t i = 0; i < sizeof(tools) / sizeof(tools[0]); i++) {
        esp_mcp_tool_t *tool = esp_mcp_tool_create_ex(tools[i].name, NULL, tools[i].description, tools[i].callback);
        if (!tool) {
            return ESP_ERR_NO_MEM;
        }
        esp_err_t err = esp_mcp_tool_set_annotations_json(
            tool, "{\"readOnlyHint\":true,\"destructiveHint\":false,\"idempotentHint\":true,\"openWorldHint\":false}");
        if (err == ESP_OK) {
            err = esp_mcp_add_tool(mcp, tool);
        }
        if (err != ESP_OK) {
            esp_mcp_tool_destroy(tool);
            return err;
        }
    }
    return ESP_OK;
}
