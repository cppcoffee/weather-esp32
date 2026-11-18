#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "nvs.h"

#define VOICE_URL_CAP            256
#define VOICE_SETTINGS_NAMESPACE "wifi"
#define VOICE_SETTINGS_KEY       "voice_v3"

typedef struct {
    char info_url[VOICE_URL_CAP];
    uint8_t followup;
} voice_settings_t;

// Only a missing namespace/key yields zeroed settings (未配置). Other errors leave
// settings unchanged: callers must not use a half-configured service after a failed load.
// A wrong blob size returns ESP_ERR_NVS_INVALID_LENGTH; invalid contents return
// ESP_ERR_INVALID_ARG. Other NVS errors are propagated.
esp_err_t voice_settings_load(voice_settings_t *settings);

// Stage one validated blob in the caller's already-open wifi handle. The caller
// owns the commit and close, including any WiFi credential writes in that transaction.
esp_err_t voice_settings_store(nvs_handle_t handle, const voice_settings_t *settings);

// ESP_ERR_INVALID_ARG for NULL, an unterminated array or a disallowed URL.
// info_url is required: there is no built-in default OTA address, and the WebSocket
// address always comes from the info response.
esp_err_t voice_settings_validate(const voice_settings_t *settings);
