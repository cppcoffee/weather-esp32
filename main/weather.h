#pragma once
#include <stdbool.h>

#define WEATHER_DAYS  5
#define WEATHER_HOURS 21 // the current local hour plus the next 20

typedef struct {
    char date[11]; // YYYY-MM-DD, consecutive local dates starting today
    float tmax, tmin;
    int code;
    float precip_probability; // 0..100%, highest interval probability that day, not current rain conditions
} weather_day_t;

typedef struct {
    int hour; // local hour from the API timestamp, 0..23 (may wrap past midnight)
    float temperature_c;
    int code;
    float precip_probability; // 0..100% for that hour
} weather_hour_t;

typedef struct {
    char city[24];
    weather_day_t days[WEATHER_DAYS];
    weather_hour_t hours[WEATHER_HOURS];
    int utc_offset; // seconds east of UTC, from the weather API
    bool ok;
} weather_t;

bool weather_update(weather_t *w); // exit-IP geolocation + five-day forecast + 21-hour data; resets w even on failure
