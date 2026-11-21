#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

// ES8311 + NS415B wiring on the ESP32-S3 board.
// DO/DI use the ESP-IDF I2S convention: DO is ESP32 -> codec, DI is codec -> ESP32.
// GPIO33-37 are reserved by the module's octal Flash/PSRAM interface; GPIO38-42 and
// GPIO21 belong to the e-paper (see epd.h). The board's WS2812 sits on GPIO48, which this
// firmware never drives; anything on GPIO48 would light the LED.
#define AUDIO_PA_EN_GPIO    15
#define AUDIO_I2S_DOUT_GPIO 16
#define AUDIO_I2S_WS_GPIO   17
#define AUDIO_I2S_DIN_GPIO  18
#define AUDIO_I2S_BCLK_GPIO 9
#define AUDIO_I2S_MCLK_GPIO 10
#define AUDIO_I2C_SCL_GPIO  11
#define AUDIO_I2C_SDA_GPIO  12

#define AUDIO_SAMPLE_RATE_HZ   16000
#define AUDIO_DMA_DESC_NUM     6
#define AUDIO_DMA_FRAME_NUM    240
#define AUDIO_PLAYBACK_TAIL_MS (1000 * AUDIO_DMA_DESC_NUM * AUDIO_DMA_FRAME_NUM / AUDIO_SAMPLE_RATE_HZ)
#define AUDIO_CHANNEL_COUNT    2
#define AUDIO_BITS_PER_SAMPLE  16

// Initialize the I2C control bus, I2S full-duplex channels and ES8311 codec.
// The output is muted after initialization; call audio_set_mute(false) before playback.
esp_err_t audio_init(void);
bool audio_is_ready(void);

// Read microphone PCM or write speaker PCM. Buffers use interleaved signed 16-bit stereo
// samples at AUDIO_SAMPLE_RATE_HZ. The codec driver blocks until the requested data is moved.
esp_err_t audio_read(void *buffer, size_t length, size_t *bytes_read);
esp_err_t audio_write(void *buffer, size_t length, size_t *bytes_written);

esp_err_t audio_set_mute(bool muted);
// Speaker volume is an integer 0–100, restored from NVS at boot.
esp_err_t audio_get_volume(int *volume);
esp_err_t audio_set_volume(int volume);
