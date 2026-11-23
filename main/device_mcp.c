#include "device_mcp.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "audio.h"
#include "cJSON.h"
#include "esp_mcp_engine.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "net.h"
#include "net_time.h"

#define PRESSURE_HISTORY_CAP        24
#define PRESSURE_SAMPLE_INTERVAL_US (10LL * 60 * 1000000)
#define PRESSURE_WINDOW_US          (3LL * 3600 * 1000000)
#define PRESSURE_MIN_SPAN_US        (30LL * 60 * 1000000)

struct pressure_point {
    int64_t uptime_us;
    float   pressure_hpa;
};

struct sensor_snapshot {
    sensor_reading_t reading;
    time_t           sampled_at;
    int64_t          sampled_uptime_us;
};

struct device_snapshot {
    weather_t              weather;
    struct sensor_snapshot sensor;
    forecast_stamp_t       stamp;
    char                   sensor_model[32];
    char                   display_page[16];
    struct pressure_point  pressure_history[PRESSURE_HISTORY_CAP];
    size_t                 pressure_count;
    size_t                 pressure_head;
};

struct device_tool_spec {
    const char                *name;
    const char                *description;
    esp_mcp_tool_callback_ex_t callback;
    bool                       volume_arg;
};

static struct device_snapshot published_snapshot;
static portMUX_TYPE           snapshot_lock = portMUX_INITIALIZER_UNLOCKED;

forecast_stamp_t
device_mcp_publish_forecast(const weather_t *weather, const forecast_stamp_t *fetched, bool retrying)
{
    portENTER_CRITICAL(&snapshot_lock);
    if (fetched && weather && weather->ok) {
        published_snapshot.weather = *weather;
        published_snapshot.stamp = *fetched;
    } else if (weather && weather->tz_known && !published_snapshot.weather.ok) {
        published_snapshot.weather.utc_offset = weather->utc_offset;
        published_snapshot.weather.tz_known = true;
    }
    published_snapshot.stamp.retrying = retrying;
    forecast_stamp_t stamp = published_snapshot.stamp;
    portEXIT_CRITICAL(&snapshot_lock);
    return stamp;
}

void
device_mcp_publish_sensor(const sensor_reading_t *sensor)
{
    struct sensor_snapshot sample = {0};
    int64_t                now_us = esp_timer_get_time();
    if (sensor) {
        sample.reading = *sensor;
        if (sensor->valid) {
            sample.sampled_at = net_synchronized_time();
            sample.sampled_uptime_us = now_us;
        }
    }
    bool pressure_ok = sensor && sensor->valid && isfinite(sensor->pressure_hpa) && sensor->pressure_hpa >= 300
                       && sensor->pressure_hpa <= 1100;
    portENTER_CRITICAL(&snapshot_lock);
    published_snapshot.sensor = sample;
    if (pressure_ok) {
        bool should_record = published_snapshot.pressure_count == 0;
        if (!should_record) {
            size_t last_idx = (published_snapshot.pressure_head + PRESSURE_HISTORY_CAP - 1) % PRESSURE_HISTORY_CAP;
            if (now_us - published_snapshot.pressure_history[last_idx].uptime_us >= PRESSURE_SAMPLE_INTERVAL_US) {
                should_record = true;
            }
        }
        if (should_record) {
            published_snapshot.pressure_history[published_snapshot.pressure_head] = (struct pressure_point){
                .uptime_us = now_us,
                .pressure_hpa = sensor->pressure_hpa,
            };
            published_snapshot.pressure_head = (published_snapshot.pressure_head + 1) % PRESSURE_HISTORY_CAP;
            if (published_snapshot.pressure_count < PRESSURE_HISTORY_CAP) {
                published_snapshot.pressure_count++;
            }
        }
    }
    portEXIT_CRITICAL(&snapshot_lock);
}

void
device_mcp_set_sensor_model(const char *model)
{
    portENTER_CRITICAL(&snapshot_lock);
    snprintf(published_snapshot.sensor_model, sizeof(published_snapshot.sensor_model), "%s",
             (model && model[0]) ? model : "none");
    portEXIT_CRITICAL(&snapshot_lock);
}

void
device_mcp_publish_display_page(const char *page)
{
    portENTER_CRITICAL(&snapshot_lock);
    snprintf(published_snapshot.display_page, sizeof(published_snapshot.display_page), "%s",
             (page && page[0]) ? page : "unknown");
    portEXIT_CRITICAL(&snapshot_lock);
}

static struct device_snapshot
snapshot(void)
{
    struct device_snapshot copy;
    // Never allocate JSON or wait for network/audio while holding this lock.
    portENTER_CRITICAL(&snapshot_lock);
    copy = published_snapshot;
    portEXIT_CRITICAL(&snapshot_lock);
    return copy;
}

static esp_err_t
send_json(esp_mcp_tool_result_t *result, cJSON *json, bool complete)
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

static bool
timezone_known(const weather_t *weather)
{
    int offset = weather->utc_offset;
    return (weather->ok || weather->tz_known) && offset >= -14 * 3600 && offset <= 14 * 3600 && offset % 60 == 0;
}

static bool
format_local_time(char iso[40], time_t timestamp, int offset)
{
    if (!timestamp) {
        return false;
    }
    // Use an explicit offset to avoid racing global TZ updates.
    time_t    local = timestamp + offset;
    struct tm tm;
    char      date[24];
    if (!gmtime_r(&local, &tm) || !strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &tm)) {
        return false;
    }
    snprintf(iso, 40, "%s%c%02d:%02d", date, offset < 0 ? '-' : '+', abs(offset / 3600), abs(offset % 3600) / 60);
    return true;
}

static bool
add_timestamp(cJSON *json, const char *key, time_t timestamp, int offset)
{
    char iso[40];
    return format_local_time(iso, timestamp, offset) ? cJSON_AddStringToObject(json, key, iso) != NULL
                                                     : cJSON_AddNullToObject(json, key) != NULL;
}

static esp_err_t
get_time(const esp_mcp_property_list_t *properties, esp_mcp_tool_result_t *result)
{
    (void)properties;
    struct device_snapshot copy = snapshot();
    time_t                 now = net_synchronized_time();
    int                    offset = copy.weather.utc_offset;
    bool                   tz_known = timezone_known(&copy.weather);
    char                   iso[40];
    bool                   synchronized = tz_known && format_local_time(iso, now, offset);
    cJSON                 *json = cJSON_CreateObject();
    bool                   ok = json && cJSON_AddBoolToObject(json, "synchronized", synchronized)
                                && cJSON_AddStringToObject(json, "source", "device_clock_sntp")
                                && cJSON_AddBoolToObject(json, "timezone_known", tz_known);
    if (ok && synchronized) {
        ok = cJSON_AddStringToObject(json, "local_time", iso) && cJSON_AddNumberToObject(json, "unix_time", (double)now)
             && cJSON_AddNumberToObject(json, "utc_offset_seconds", offset);
    } else if (ok) {
        ok = cJSON_AddNullToObject(json, "local_time")
             && cJSON_AddStringToObject(json, "reason", tz_known ? "clock_not_synchronized" : "timezone_unknown");
    }
    return send_json(result, json, ok);
}

static double
round2(double value)
{
    // Strip 32-bit float -> double promotion noise (e.g. 11.7f
    // -> 11.699999809265137) so MCP tool JSON stays compact for LLM prefill.
    return round(value * 100.0) / 100.0;
}

static esp_err_t
get_sensor(const esp_mcp_property_list_t *properties, esp_mcp_tool_result_t *result)
{
    (void)properties;
    struct device_snapshot copy = snapshot();
    sensor_reading_t       sensor = copy.sensor.reading;
    int64_t                now_us = esp_timer_get_time();
    bool available = sensor.valid && isfinite(sensor.temperature_c) && isfinite(sensor.humidity_percent)
                     && sensor.humidity_percent >= 0 && sensor.humidity_percent <= 100;
    bool pressure_ok =
        available && isfinite(sensor.pressure_hpa) && sensor.pressure_hpa >= 300 && sensor.pressure_hpa <= 1100;
    cJSON *json = cJSON_CreateObject();
    bool   ok = json && cJSON_AddBoolToObject(json, "available", available)
                && cJSON_AddStringToObject(json, "source", "local_sensor")
                && cJSON_AddStringToObject(json, "sensor_model", copy.sensor_model[0] ? copy.sensor_model : "unknown");
    if (ok && available) {
        int     offset = timezone_known(&copy.weather) ? copy.weather.utc_offset : 0;
        int64_t elapsed_us = now_us - copy.sensor.sampled_uptime_us;
        double  age = elapsed_us > 0 ? (double)elapsed_us / 1000000.0 : 0;
        ok = cJSON_AddNumberToObject(json, "temperature_c", round2(sensor.temperature_c))
             && cJSON_AddNumberToObject(json, "humidity_percent", round2(sensor.humidity_percent))
             && (pressure_ok ? cJSON_AddNumberToObject(json, "pressure_hpa", round2(sensor.pressure_hpa))
                             : cJSON_AddNullToObject(json, "pressure_hpa"))
             && add_timestamp(json, "sampled_at", copy.sensor.sampled_at, offset)
             && cJSON_AddNumberToObject(json, "sample_age_seconds", round2(age));
        if (ok && pressure_ok && copy.pressure_count >= 2) {
            size_t oldest_idx =
                (copy.pressure_head + PRESSURE_HISTORY_CAP - copy.pressure_count) % PRESSURE_HISTORY_CAP;
            const struct pressure_point *ref = NULL;
            for (size_t i = 0; i < copy.pressure_count; i++) {
                size_t idx = (oldest_idx + i) % PRESSURE_HISTORY_CAP;
                if (now_us - copy.pressure_history[idx].uptime_us <= PRESSURE_WINDOW_US) {
                    ref = &copy.pressure_history[idx];
                    break;
                }
            }
            if (ref && now_us - ref->uptime_us >= PRESSURE_MIN_SPAN_US) {
                double      delta = round2((double)sensor.pressure_hpa - (double)ref->pressure_hpa);
                const char *trend = delta >= 1.0 ? "rising" : (delta <= -1.0 ? "falling" : "steady");
                int         window_min = (int)((now_us - ref->uptime_us) / 60000000LL);
                ok = cJSON_AddNumberToObject(json, "pressure_delta_hpa", delta)
                     && cJSON_AddStringToObject(json, "pressure_trend", trend)
                     && cJSON_AddNumberToObject(json, "pressure_trend_window_minutes", window_min);
            }
        }
    } else if (ok) {
        ok = cJSON_AddNullToObject(json, "temperature_c") && cJSON_AddNullToObject(json, "humidity_percent")
             && cJSON_AddNullToObject(json, "pressure_hpa")
             && cJSON_AddStringToObject(json, "reason", sensor.valid ? "invalid_sensor_reading" : "sensor_unavailable");
    }
    return send_json(result, json, ok);
}

static cJSON *
day_json(const weather_day_t *day)
{
    cJSON *json = cJSON_CreateObject();
    if (!json || !cJSON_AddStringToObject(json, "date", day->date)
        || !cJSON_AddNumberToObject(json, "temperature_min_c", round2(day->tmin))
        || !cJSON_AddNumberToObject(json, "temperature_max_c", round2(day->tmax))
        || !cJSON_AddNumberToObject(json, "weather_code", day->code)
        || !cJSON_AddNumberToObject(json, "precip_probability", round2(day->precip_probability))
        || (day->uv_valid && !cJSON_AddNumberToObject(json, "uv_index_max", round2(day->uv_index_max)))
        || (day->sunrise[0] && !cJSON_AddStringToObject(json, "sunrise", day->sunrise))
        || (day->sunset[0] && !cJSON_AddStringToObject(json, "sunset", day->sunset)))
    {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

static cJSON *
hour_json(const weather_hour_t *hour)
{
    cJSON *json = cJSON_CreateObject();
    if (!json || !cJSON_AddNumberToObject(json, "hour", hour->hour)
        || !cJSON_AddNumberToObject(json, "temperature_c", round2(hour->temperature_c))
        || !cJSON_AddNumberToObject(json, "weather_code", hour->code)
        || !cJSON_AddNumberToObject(json, "precip_probability", round2(hour->precip_probability)))
    {
        cJSON_Delete(json);
        return NULL;
    }
    if (hour->date > 0 && hour->date <= 99991231) {
        char date[16];
        snprintf(date, sizeof(date), "%04d-%02d-%02d", hour->date / 10000, hour->date / 100 % 100, hour->date % 100);
        if (!cJSON_AddStringToObject(json, "date", date)) {
            cJSON_Delete(json);
            return NULL;
        }
    }
    return json;
}

static esp_err_t
get_forecast(const esp_mcp_property_list_t *properties, esp_mcp_tool_result_t *result)
{
    (void)properties;
    struct device_snapshot copy = snapshot();
    int64_t                uptime = esp_timer_get_time();
    int64_t                elapsed_us = uptime - copy.stamp.fetched_uptime_us;
    double                 age = elapsed_us > 0 ? (double)elapsed_us / 1000000.0 : 0;
    bool                   fetched_at_known = copy.stamp.fetched_at != 0;
    bool                   stale = forecast_is_stale(copy.stamp, uptime);
    cJSON                 *json = cJSON_CreateObject();
    bool                   ok = json && cJSON_AddBoolToObject(json, "available", copy.weather.ok)
                                && cJSON_AddStringToObject(json, "source", "Open-Meteo_cached_forecast")
                                && cJSON_AddBoolToObject(json, "retrying", copy.stamp.retrying);
    if (!copy.weather.ok) {
        ok = ok && cJSON_AddStringToObject(json, "reason", "forecast_not_available");
        return send_json(result, json, ok);
    }
    double next_refresh = (double)FORECAST_STALE_SECONDS - age;
    if (next_refresh < 0) {
        next_refresh = 0;
    }
    ok = ok && cJSON_AddStringToObject(json, "city", copy.weather.city)
         && cJSON_AddNumberToObject(json, "utc_offset_seconds", copy.weather.utc_offset)
         && cJSON_AddBoolToObject(json, "fetched_at_known", fetched_at_known)
         && (fetched_at_known ? cJSON_AddNumberToObject(json, "fetched_at", (double)copy.stamp.fetched_at)
                              : cJSON_AddNullToObject(json, "fetched_at"))
         && cJSON_AddNumberToObject(json, "age", age) && cJSON_AddBoolToObject(json, "stale", stale)
         && cJSON_AddNumberToObject(json, "refresh_interval_seconds", FORECAST_STALE_SECONDS)
         && (!copy.stamp.retrying ? cJSON_AddNumberToObject(json, "next_refresh_in_seconds", next_refresh)
                                  : cJSON_AddNullToObject(json, "next_refresh_in_seconds"));
    if (ok && copy.weather.current.valid) {
        cJSON *cur = cJSON_AddObjectToObject(json, "current");
        ok = cur && cJSON_AddNumberToObject(cur, "temperature_c", round2(copy.weather.current.temperature_c))
             && cJSON_AddNumberToObject(cur, "apparent_temperature_c",
                                        round2(copy.weather.current.apparent_temperature_c))
             && cJSON_AddNumberToObject(cur, "relative_humidity", round2(copy.weather.current.relative_humidity))
             && cJSON_AddNumberToObject(cur, "wind_speed_kmh", round2(copy.weather.current.wind_speed_kmh))
             && cJSON_AddNumberToObject(cur, "wind_direction_deg", copy.weather.current.wind_direction_deg)
             && cJSON_AddNumberToObject(cur, "weather_code", copy.weather.current.code);
    }
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

static esp_err_t
get_device_status(const esp_mcp_property_list_t *properties, esp_mcp_tool_result_t *result)
{
    (void)properties;
    int       volume;
    esp_err_t err = audio_get_volume(&volume);
    if (err != ESP_OK) {
        return err;
    }
    struct device_snapshot copy = snapshot();
    net_status_t           net = {0};
    net_get_status(&net);
    double uptime_s = (double)esp_timer_get_time() / 1000000.0;
    if (uptime_s < 0) {
        uptime_s = 0;
    }
    cJSON *json = cJSON_CreateObject();
    cJSON *speaker = json ? cJSON_AddObjectToObject(json, "audio_speaker") : NULL;
    cJSON *wifi = json ? cJSON_AddObjectToObject(json, "wifi") : NULL;
    cJSON *memory = json ? cJSON_AddObjectToObject(json, "memory") : NULL;
    bool ok = speaker && wifi && memory && cJSON_AddNumberToObject(speaker, "volume", volume)
              && add_timestamp(json, "read_at", net_synchronized_time(), 0)
              && cJSON_AddNumberToObject(json, "uptime_seconds", (double)(int64_t)uptime_s)
              && cJSON_AddStringToObject(json, "display_page", copy.display_page[0] ? copy.display_page : "placeholder")
              && cJSON_AddStringToObject(json, "sensor_model", copy.sensor_model[0] ? copy.sensor_model : "unknown")
              && cJSON_AddBoolToObject(wifi, "connected", net.connected)
              && (net.connected ? cJSON_AddStringToObject(wifi, "ssid", net.ssid) : cJSON_AddNullToObject(wifi, "ssid"))
              && (net.connected ? cJSON_AddNumberToObject(wifi, "rssi", net.rssi) : cJSON_AddNullToObject(wifi, "rssi"))
              && cJSON_AddNumberToObject(memory, "free_heap_bytes", (double)esp_get_free_heap_size())
              && cJSON_AddNumberToObject(memory, "free_internal_bytes", (double)esp_get_free_internal_heap_size());
    return send_json(result, json, ok);
}

static esp_err_t
set_volume(const esp_mcp_property_list_t *properties, esp_mcp_tool_result_t *result)
{
    int       volume = esp_mcp_property_list_get_property_int(properties, "volume");
    esp_err_t err = audio_set_volume(volume);
    if (err != ESP_OK) {
        return err;
    }
    cJSON *json = cJSON_CreateObject();
    bool   ok = json && cJSON_AddNumberToObject(json, "volume", volume) && cJSON_AddBoolToObject(json, "saved", true)
                && add_timestamp(json, "applied_at", net_synchronized_time(), 0);
    return send_json(result, json, ok);
}

static const struct device_tool_spec device_tools[] = {
    {"self.get_current_time", "Read device local time; synchronized=false means time is unavailable.", get_time, false},
    {"self.get_weather_forecast",
     "Read cached five-day/21-hour forecast (auto-refreshed every 1800s / "
     "30min) plus current conditions, UV index, "
     "and sunrise/sunset; no network. age: monotonic seconds; weather_code: "
     "WMO, unmapped=unknown. "
     "precip_probability: forecast percent, not observed rain. Hours: API "
     "order at fetch, each with local date "
     "(YYYY-MM-DD) and hour. A repeated local hour may appear twice near a "
     "clock change.",
     get_forecast, false},
    {"self.get_sensor_reading",
     "Read device temperature, humidity, barometric pressure (hPa), "
     "sensor_model, sampled_at, sample_age_seconds, "
     "and up to 3h pressure trend; available=false means no valid reading.",
     get_sensor, false},
    {"self.get_device_status",
     "Read speaker volume (0–100), read_at, display_page, uptime_seconds, "
     "sensor_model, WiFi SSID/RSSI, and free "
     "memory.",
     get_device_status, false},
    {"self.audio_speaker.set_volume",
     "Set the speaker volume (integer 0–100). Returns volume and saved=true "
     "after applying and saving for reboot.",
     set_volume, true},
};

esp_err_t
device_mcp_register(esp_mcp_t *mcp)
{
    if (!mcp) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < sizeof(device_tools) / sizeof(device_tools[0]); i++) {
        const struct device_tool_spec *spec = &device_tools[i];
        esp_mcp_tool_t *tool = esp_mcp_tool_create_ex(spec->name, NULL, spec->description, spec->callback);
        if (!tool) {
            return ESP_ERR_NO_MEM;
        }
        bool      volume_arg = spec->volume_arg;
        esp_err_t err =
            esp_mcp_tool_set_annotations_json(tool, volume_arg ? "{\"readOnlyHint\":false,\"destructiveHint\":false,"
                                                                 "\"idempotentHint\":true,\"openWorldHint\":false}"
                                                               : "{\"readOnlyHint\":true,\"destructiveHint\":false,"
                                                                 "\"idempotentHint\":true,\"openWorldHint\":false}");
        if (err == ESP_OK && volume_arg) {
            esp_mcp_property_t *property = esp_mcp_property_create_with_range("volume", 0, 100);
            err = property ? esp_mcp_tool_add_property(tool, property) : ESP_ERR_NO_MEM;
            if (property && err != ESP_OK) {
                esp_mcp_property_destroy(property);
            }
        }
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
