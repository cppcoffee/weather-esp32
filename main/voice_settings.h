#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "nvs.h"

#define VOICE_URL_CAP            256
#define VOICE_SETTINGS_NAMESPACE "wifi"
#define VOICE_SETTINGS_KEY       "voice_v1"

typedef struct {
    uint8_t custom;
    char info_url[VOICE_URL_CAP];
    char websocket_url[VOICE_URL_CAP];
} voice_settings_t;

// Only a missing namespace/key yields zeroed official defaults. Other errors leave
// settings unchanged: callers must not use official service settings after a failed load.
// A wrong blob size returns ESP_ERR_NVS_INVALID_LENGTH; invalid contents return
// ESP_ERR_INVALID_ARG. Other NVS errors are propagated.
esp_err_t voice_settings_load(voice_settings_t *settings);

// Stage one validated blob in the caller's already-open wifi handle. The caller
// owns the commit and close, including any WiFi credential writes in that transaction.
esp_err_t voice_settings_store(nvs_handle_t handle, const voice_settings_t *settings);

// ESP_ERR_INVALID_ARG for NULL, invalid mode, unterminated arrays or disallowed URLs.
// Official mode requires empty URLs; custom mode requires info_url and permits an empty WS URL.
esp_err_t voice_settings_validate(const voice_settings_t *settings);
