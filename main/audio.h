#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

// ES8311 + NS415B wiring on the ESP32-S3 board.
// DO/DI use the ESP-IDF I2S convention: DO is ESP32 -> codec, DI is codec -> ESP32.
// GPIO33-37 are reserved by the module's octal Flash/PSRAM interface.
// Keep the moved I2S outputs on adjacent module pins 47/48 for a short PCB route.
#define AUDIO_PA_EN_GPIO    2
#define AUDIO_I2S_DOUT_GPIO 47
#define AUDIO_I2S_WS_GPIO   48
#define AUDIO_I2S_DIN_GPIO  38
#define AUDIO_I2S_BCLK_GPIO 39
#define AUDIO_I2S_MCLK_GPIO 40
#define AUDIO_I2C_SCL_GPIO  41
#define AUDIO_I2C_SDA_GPIO  42

#define AUDIO_SAMPLE_RATE_HZ  16000
#define AUDIO_CHANNEL_COUNT   2
#define AUDIO_BITS_PER_SAMPLE 16

// Initialize the I2C control bus, I2S full-duplex channels and ES8311 codec.
// The output is muted after initialization; call audio_set_mute(false) before playback.
esp_err_t audio_init(void);
bool audio_is_ready(void);

// Read microphone PCM or write speaker PCM. Buffers use interleaved signed 16-bit stereo
// samples at AUDIO_SAMPLE_RATE_HZ. The codec driver blocks until the requested data is moved.
esp_err_t audio_read(void *buffer, size_t length, size_t *bytes_read);
esp_err_t audio_write(void *buffer, size_t length, size_t *bytes_written);

// Volume is in the codec driver's 0..100 scale. Microphone gain is in dB.
esp_err_t audio_set_volume(int volume);
esp_err_t audio_set_mic_gain(float gain_db);
esp_err_t audio_set_mute(bool muted);
