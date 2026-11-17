#pragma once

#include "esp_err.h"
#include "ui.h"

// Protocol dispatch and schemas are owned by the managed mcp-c-sdk.
typedef struct esp_mcp_s esp_mcp_t;

// Publish the same successful forecast and local sensor values used by the UI.
// Only forecast_updated && weather->ok replaces the cached forecast/fetch time;
// failures retain it and update retrying. Safe to call before voice_init or after it fails.
void device_mcp_publish(const weather_t *weather, const sensor_reading_t *sensor, bool retrying, bool forecast_updated);

// Register three parameterless, read-only tools. The engine owns tools after success;
// the caller must destroy the engine if registration fails partway through.
esp_err_t device_mcp_register(esp_mcp_t *mcp);
