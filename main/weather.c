#include "weather.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "net.h"
#include "weather_json.h"

static const char *log_tag = "weather";

// ip-api.com: free, no key, plain HTTP. Returns lat/lon/city of the exit IP.
#define GEO_URL "http://ip-api.com/json/?fields=status,city,lat,lon"
// open-meteo: free, no key. timezone=auto gives utc_offset_seconds.
// forecast_hours=21 starts at the current local hour, so the hourly strip covers now .. +20 h.
#define WX_URL                                                                                                         \
    "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"                                                  \
    "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m,wind_direction_10m" \
    "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,uv_index_max,sunrise,"    \
    "sunset"                                                                                                           \
    "&hourly=weather_code,temperature_2m,precipitation_probability"                                                    \
    "&temperature_unit=celsius&timezone=auto&forecast_days=5&forecast_hours=21"

// parse_json/json_num/json_int/json_string_with_length/json_str/json_object_array
// live in weather_json.c; only weather-contract validation is below.

static const int month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

// Validated YYYY-MM-DD; ordinal is a timezone/DST-free day number.
static bool parse_date10(const char *p, char out[11], int *ordinal)
{
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) {
            if (p[i] != '-') {
                return false;
            }
        } else if (p[i] < '0' || p[i] > '9') {
            return false;
        }
    }
    int year = (p[0] - '0') * 1000 + (p[1] - '0') * 100 + (p[2] - '0') * 10 + p[3] - '0';
    int month = (p[5] - '0') * 10 + p[6] - '0';
    int day = (p[8] - '0') * 10 + p[9] - '0';
    bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (year < 1 || month < 1 || month > 12 || day < 1 || day > month_days[month - 1] + (month == 2 && leap)) {
        return false;
    }

    // Gregorian day number avoids timezone/DST dependencies when checking consecutive dates.
    int prior_year = year - 1;
    *ordinal = prior_year * 365 + prior_year / 4 - prior_year / 100 + prior_year / 400 + day;
    for (int m = 1; m < month; m++) {
        *ordinal += month_days[m - 1] + (m == 2 && leap);
    }
    memcpy(out, p, 10);
    out[10] = 0;
    return true;
}

static bool json_date(const cJSON *item, char out[11], int *ordinal)
{
    const char *value;
    return json_string_with_length(item, 10, &value) && parse_date10(value, out, ordinal);
}

// YYYY-MM-DDTHH:00: local hour plus an ordinal hour stamp for sequence checks.
static bool json_hour_time(const cJSON *item, char date[11], int *hour, long *stamp)
{
    const char *value;
    if (!json_string_with_length(item, 16, &value) || value[10] != 'T' || value[13] != ':' || value[14] != '0'
        || value[15] != '0' || value[11] < '0' || value[11] > '9' || value[12] < '0' || value[12] > '9')
    {
        return false;
    }
    int ordinal;
    if (!parse_date10(value, date, &ordinal)) {
        return false;
    }
    int h = (value[11] - '0') * 10 + value[12] - '0';
    if (h > 23) {
        return false;
    }
    *hour = h;
    *stamp = (long)ordinal * 24 + h;
    return true;
}

static void copy_hhmm(const cJSON *item, char out[6])
{
    out[0] = '\0';
    const char *value = NULL;
    if (!json_string_with_length(item, 16, &value) || value[10] != 'T' || value[13] != ':' || value[11] < '0'
        || value[11] > '2' || value[12] < '0' || value[12] > '9' || value[14] < '0' || value[14] > '5'
        || value[15] < '0' || value[15] > '9')
    {
        return;
    }
    int hh = (value[11] - '0') * 10 + (value[12] - '0');
    if (hh > 23) {
        return;
    }
    memcpy(out, value + 11, 5);
    out[5] = '\0';
}

static bool json_days(const cJSON *daily, weather_day_t days[WEATHER_DAYS])
{
    cJSON *dates;
    cJSON *codes;
    cJSON *maxima;
    cJSON *minima;
    cJSON *probabilities;
    if (!cJSON_IsObject(daily) || !json_object_array(daily, "time", WEATHER_DAYS, &dates)
        || !json_object_array(daily, "weather_code", WEATHER_DAYS, &codes)
        || !json_object_array(daily, "temperature_2m_max", WEATHER_DAYS, &maxima)
        || !json_object_array(daily, "temperature_2m_min", WEATHER_DAYS, &minima)
        || !json_object_array(daily, "precipitation_probability_max", WEATHER_DAYS, &probabilities))
    {
        return false;
    }
    cJSON *uv_maxima = NULL;
    cJSON *sunrises = NULL;
    cJSON *sunsets = NULL;
    (void)json_object_array(daily, "uv_index_max", WEATHER_DAYS, &uv_maxima);
    (void)json_object_array(daily, "sunrise", WEATHER_DAYS, &sunrises);
    (void)json_object_array(daily, "sunset", WEATHER_DAYS, &sunsets);

    int previous = 0;
    for (int i = 0; i < WEATHER_DAYS; i++) {
        int ordinal, code;
        double tmax, tmin, precip_probability;
        if (!json_date(cJSON_GetArrayItem(dates, i), days[i].date, &ordinal) || (i && ordinal != previous + 1)
            || !json_num(cJSON_GetArrayItem(maxima, i), &tmax) || !json_num(cJSON_GetArrayItem(minima, i), &tmin)
            || fabs(tmax) > FLT_MAX || fabs(tmin) > FLT_MAX || tmin > tmax
            || !json_int(cJSON_GetArrayItem(codes, i), &code) || code < 0 || code > 99
            || !json_num(cJSON_GetArrayItem(probabilities, i), &precip_probability) || precip_probability < 0
            || precip_probability > 100)
        {
            return false;
        }
        days[i].tmax = (float)tmax;
        days[i].tmin = (float)tmin;
        days[i].code = code;
        days[i].precip_probability = (float)precip_probability;

        double uv = 0;
        if (uv_maxima && json_num(cJSON_GetArrayItem(uv_maxima, i), &uv) && uv >= 0 && uv <= 30) {
            days[i].uv_index_max = (float)uv;
            days[i].uv_valid = true;
        }
        if (sunrises) {
            copy_hhmm(cJSON_GetArrayItem(sunrises, i), days[i].sunrise);
        }
        if (sunsets) {
            copy_hhmm(cJSON_GetArrayItem(sunsets, i), days[i].sunset);
        }
        previous = ordinal;
    }
    return true;
}

static bool json_hours(const cJSON *hourly, const char first_date[11], weather_hour_t hours[WEATHER_HOURS])
{
    cJSON *times;
    cJSON *codes;
    cJSON *temperatures;
    cJSON *probabilities;
    if (!cJSON_IsObject(hourly) || !json_object_array(hourly, "time", WEATHER_HOURS, &times)
        || !json_object_array(hourly, "weather_code", WEATHER_HOURS, &codes)
        || !json_object_array(hourly, "temperature_2m", WEATHER_HOURS, &temperatures)
        || !json_object_array(hourly, "precipitation_probability", WEATHER_HOURS, &probabilities))
    {
        return false;
    }
    char date[11];
    long previous = 0;
    for (int i = 0; i < WEATHER_HOURS; i++) {
        int hour, code;
        double temperature, probability;
        long stamp;
        // Local DST can repeat an hour or skip one, so allow -1..+2 between entries.
        if (!json_hour_time(cJSON_GetArrayItem(times, i), date, &hour, &stamp)
            || (i && (stamp < previous - 1 || stamp > previous + 2))
            || !json_num(cJSON_GetArrayItem(temperatures, i), &temperature) || fabs(temperature) > FLT_MAX
            || !json_int(cJSON_GetArrayItem(codes, i), &code) || code < 0 || code > 99
            || !json_num(cJSON_GetArrayItem(probabilities, i), &probability) || probability < 0 || probability > 100)
        {
            return false;
        }
        if (i == 0 && strcmp(date, first_date) != 0) {
            return false;
        }
        hours[i].date = atoi(date) * 10000 + atoi(date + 5) * 100 + atoi(date + 8);
        hours[i].hour = hour;
        hours[i].temperature_c = (float)temperature;
        hours[i].code = code;
        hours[i].precip_probability = (float)probability;
        previous = stamp;
    }
    return true;
}

static bool parse_geo(const char *buf, size_t length, double *lat, double *lon, char *city, size_t city_cap)
{
    cJSON *root = json_parse(buf, length);
    bool ok = root && cJSON_IsObject(root) && json_num(cJSON_GetObjectItemCaseSensitive(root, "lat"), lat)
              && json_num(cJSON_GetObjectItemCaseSensitive(root, "lon"), lon);
    if (ok && !json_str(root, "city", city, city_cap)) {
        strcpy(city, "--");
    }
    cJSON_Delete(root);
    return ok;
}

static int utc_offset_from_coords(double lat, double lon)
{
    if (!isfinite(lat) || !isfinite(lon) || lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
        return 0;
    }
    // Mainland China, Hong Kong, Macau and Taiwan all observe UTC+8 across 73E..135E.
    if (lat >= 18.0 && lat <= 54.0 && lon >= 73.0 && lon <= 135.0) {
        return 8 * 3600;
    }
    int zone = (int)lround(lon / 15.0);
    if (zone < -12) {
        zone = -12;
    } else if (zone > 14) {
        zone = 14;
    }
    return zone * 3600;
}

static void json_current(const cJSON *current, weather_current_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!cJSON_IsObject(current)) {
        return;
    }
    double temp = 0, apparent = 0, humidity = 0, wind_speed = 0;
    int code = 0, wind_dir = 0;
    if (!json_num(cJSON_GetObjectItemCaseSensitive(current, "temperature_2m"), &temp) || fabs(temp) > FLT_MAX
        || !json_num(cJSON_GetObjectItemCaseSensitive(current, "apparent_temperature"), &apparent)
        || fabs(apparent) > FLT_MAX
        || !json_num(cJSON_GetObjectItemCaseSensitive(current, "relative_humidity_2m"), &humidity) || humidity < 0
        || humidity > 100 || !json_int(cJSON_GetObjectItemCaseSensitive(current, "weather_code"), &code) || code < 0
        || code > 99 || !json_num(cJSON_GetObjectItemCaseSensitive(current, "wind_speed_10m"), &wind_speed)
        || wind_speed < 0 || wind_speed > 500
        || !json_int(cJSON_GetObjectItemCaseSensitive(current, "wind_direction_10m"), &wind_dir) || wind_dir < 0
        || wind_dir > 360)
    {
        return;
    }
    out->temperature_c = (float)temp;
    out->apparent_temperature_c = (float)apparent;
    out->relative_humidity = (float)humidity;
    out->wind_speed_kmh = (float)wind_speed;
    out->wind_direction_deg = wind_dir;
    out->code = code;
    out->valid = true;
}

static bool parse_forecast(const char *buf, size_t length, weather_t *weather)
{
    cJSON *root = json_parse(buf, length);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }
    bool ok = json_int(cJSON_GetObjectItemCaseSensitive(root, "utc_offset_seconds"), &weather->utc_offset)
              && json_days(cJSON_GetObjectItemCaseSensitive(root, "daily"), weather->days)
              && json_hours(cJSON_GetObjectItemCaseSensitive(root, "hourly"), weather->days[0].date, weather->hours);
    if (ok) {
        json_current(cJSON_GetObjectItemCaseSensitive(root, "current"), &weather->current);
    }
    cJSON_Delete(root);
    return ok;
}

bool weather_update(weather_t *w, forecast_stamp_t *fetched)
{
    static char buf[8192];
    double lat, lon;

    memset(w, 0, sizeof(*w));

    int len = net_http_get(GEO_URL, buf, sizeof(buf));
    // A full buffer may hide truncation even if the retained prefix is valid JSON.
    if (len <= 0 || (size_t)len >= sizeof(buf) - 1) {
        return false;
    }
    if (!parse_geo(buf, (size_t)len, &lat, &lon, w->city, sizeof(w->city))) {
        ESP_LOGE(log_tag, "geo parse failed");
        return false;
    }

    int coord_offset = utc_offset_from_coords(lat, lon);
#ifdef ESP_PLATFORM
    w->utc_offset = coord_offset;
    w->tz_known = true;
#endif
    ESP_LOGI(log_tag, "ip located: %s %.4f,%.4f (coord tz %+d)", w->city, lat, lon, coord_offset);

    char lat_s[16], lon_s[16];
    char url[sizeof(WX_URL) + sizeof(lat_s) + sizeof(lon_s)];
    snprintf(lat_s, sizeof(lat_s), "%.4f", lat);
    snprintf(lon_s, sizeof(lon_s), "%.4f", lon);
    snprintf(url, sizeof(url), WX_URL, lat_s, lon_s);

    len = net_http_get(url, buf, sizeof(buf));
    if (len <= 0 || (size_t)len >= sizeof(buf) - 1) {
        return false;
    }
    if (!parse_forecast(buf, (size_t)len, w)) {
        ESP_LOGE(log_tag, "weather parse failed: invalid timezone, daily or hourly data");
        return false;
    }
#ifdef ESP_PLATFORM
    if ((lat >= 18.0 && lat <= 54.0 && lon >= 73.0 && lon <= 135.0) || (w->utc_offset == 0 && coord_offset != 0)) {
        w->utc_offset = coord_offset;
    }
#else
    (void)coord_offset;
#endif
    w->tz_known = true;
    w->ok = true;
    if (fetched) {
        *fetched = (forecast_stamp_t){
            .fetched_at = net_synchronized_time(),
            .fetched_uptime_us = esp_timer_get_time(),
        };
    }
    ESP_LOGI(log_tag, "%s %.1f/%.1fC, %d days, %d hours, tz %+d", w->days[0].date, w->days[0].tmax, w->days[0].tmin,
             WEATHER_DAYS, WEATHER_HOURS, w->utc_offset);
    return true;
}
