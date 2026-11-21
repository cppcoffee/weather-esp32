#ifndef __DEVICE_MCP_H__
#define __DEVICE_MCP_H__

#include "esp_err.h"
#include "ui.h"

// Protocol dispatch and schemas are owned by the managed mcp-c-sdk.
typedef struct esp_mcp_s esp_mcp_t;

// Publish the same successful forecast used by the UI. A failed fetch passes NULL
// for fetched so it retains the previous data and updates retrying.
forecast_stamp_t device_mcp_publish_forecast(const weather_t *weather, const forecast_stamp_t *fetched, bool retrying);

// Call when sampling completes to publish the values and acquisition time together.
void device_mcp_publish_sensor(const sensor_reading_t *sensor);

// Record the detected local sensor chip summary (e.g. "AHT20+BMP280").
void device_mcp_set_sensor_model(const char *model);

// Publish the current e-paper screen mode (e.g. "daily", "hourly", "portal").
void device_mcp_publish_display_page(const char *page);

// Register the device data and audio controls. The engine owns tools after success;
// the caller must destroy the engine if registration fails partway through.
esp_err_t device_mcp_register(esp_mcp_t *mcp);

#endif
