#pragma once

#include "voice_feedback.h"
#include "weather.h"

typedef struct {
    float temperature_c;
    float humidity_percent;
    bool valid;
} sensor_reading_t;

// What the device is doing right now, shown in the footer's right corner.
typedef enum {
    UI_STATUS_NONE,
    UI_STATUS_CONNECTING, // 正在联网
    UI_STATUS_FETCHING,   // 正在取数
    UI_STATUS_SYNCING,    // 同步时间
    UI_STATUS_UPDATED,    // 已更新
    UI_STATUS_RETRY,      // 重试 #n
} ui_status_t;

// NULL/invalid or oversized footer values use placeholders; values must be finite, humidity in [0, 100].
// Umbrella advice uses the rounded daily maximum probability, not current rain conditions.
// Draw into the framebuffer; the caller decides when to call epd_refresh().
void ui_draw_weather(const weather_t *weather, const sensor_reading_t *sensor);
// Hourly strip for the current local hour and the next 9, one column per hour.
void ui_draw_hourly(const weather_t *weather, const sensor_reading_t *sensor);
// Set metadata from device_mcp_publish before drawing the page. NULL means no forecast.
void ui_set_forecast_stamp(const forecast_stamp_t *stamp);
// Redraw only the footer status corner; attempt is used by UI_STATUS_RETRY.
void ui_draw_status(ui_status_t status, unsigned attempt);
// Update the application status snapshot and redraw only the status corner if visible
// and the displayed text changed. Reset and setup screens take precedence.
bool ui_update_status(ui_status_t status, unsigned attempt);
// Update the clock and calendar-dependent forecast labels on a weather page.
// The caller serializes drawing/refreshes and refreshes the panel only when true.
bool ui_update_clock(void);
// Apply a copied voice snapshot; redraw only the status corner and, for pairing,
// the title. Reset and setup screens take precedence. Caller serializes and refreshes.
bool ui_update_voice(const voice_feedback_t *feedback);
// Replace only the title; attempt is the next retry number (1 after the first failure).
void ui_draw_retry(unsigned attempt, unsigned retry_seconds);
void ui_draw_resetting(void);
void ui_draw_portal_hint(void);
