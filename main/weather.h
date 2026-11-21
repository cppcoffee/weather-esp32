#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define WEATHER_DAYS           5
#define WEATHER_HOURS          21 // the current local hour plus the next 20
#define FORECAST_STALE_SECONDS (30 * 60)

typedef struct {
    time_t fetched_at; // UTC, zero if the clock was unsynchronized at fetch time
    int64_t fetched_uptime_us;
    bool retrying;
} forecast_stamp_t;

static inline bool forecast_is_stale(forecast_stamp_t stamp, int64_t uptime_us)
{
    return stamp.retrying || stamp.fetched_at == 0
           || uptime_us - stamp.fetched_uptime_us >= (int64_t)FORECAST_STALE_SECONDS * 1000000;
}

typedef struct {
    char date[11]; // YYYY-MM-DD, consecutive local dates starting today
    float tmax, tmin;
    int code;
    float precip_probability; // 0..100%, highest interval probability that day, not current rain conditions
    char sunrise[6];          // HH:MM local time, empty if unavailable
    char sunset[6];           // HH:MM local time, empty if unavailable
    float uv_index_max;
    bool uv_valid;
} weather_day_t;

typedef struct {
    bool valid;
    float temperature_c;
    float apparent_temperature_c;
    float relative_humidity;
    float wind_speed_kmh;
    int wind_direction_deg;
    int code;
} weather_current_t;

typedef struct {
    int date; // local YYYYMMDD, retained from the API for midnight/DST transitions
    int hour; // local hour from the API timestamp, 0..23 (may wrap past midnight)
    float temperature_c;
    int code;
    float precip_probability; // 0..100% for that hour
} weather_hour_t;

typedef struct {
    char city[24];
    weather_day_t days[WEATHER_DAYS];
    weather_hour_t hours[WEATHER_HOURS];
    weather_current_t current;
    int utc_offset; // seconds east of UTC, from the weather API or coordinates
    bool tz_known;
    bool ok;
} weather_t;

// Exit-IP geolocation + five-day forecast + 21-hour data. Resets w even on failure;
// writes fetched only on success when the caller requests acquisition metadata.
bool weather_update(weather_t *w, forecast_stamp_t *fetched);
