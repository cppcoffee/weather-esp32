#ifndef __AUDIO_H__
#define __AUDIO_H__

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

// Set to 1 for boards with ES7210 (4-ch TDM ADC: MIC1 = mic, MIC3 = speaker
// loopback reference) + ES8311 (DAC-only) + NS4150B, enabling hardware-aligned
// AEC and full-duplex realtime conversation. Keep 0 for single ES8311 + NS4150B
// boards (half-duplex auto mode).
#ifndef AUDIO_USE_ES7210_AEC
#define AUDIO_USE_ES7210_AEC 0
#endif

// Audio codec + NS4150B wiring on the ESP32-S3 board (same GPIOs for both
// single-ES8311 and ES7210+ES8311 modes; ES7210 shares I2C/MCLK/BCLK/WS and
// drives DIN in 4-slot TDM).
// DO/DI use the ESP-IDF I2S convention: DO is ESP32 -> codec, DI is codec ->
// ESP32. GPIO33-37 are reserved by the module's octal Flash/PSRAM interface;
// GPIO38-42 and GPIO21 belong to the e-paper (see epd.h). The board's WS2812
// sits on GPIO48, which this firmware never drives; anything on GPIO48 would
// light the LED.
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

// Initialize the I2C control bus, I2S full-duplex channels and codec(s).
// The output is muted after initialization; call audio_set_mute(false) before
// playback.
esp_err_t audio_init(void);
bool      audio_is_ready(void);

// Read capture PCM or write speaker PCM at AUDIO_SAMPLE_RATE_HZ.
// - audio_write(): interleaved signed 16-bit stereo samples.
// - audio_read(): interleaved signed 16-bit 2-channel samples; when
//   AUDIO_USE_ES7210_AEC == 1, channels are hardware-aligned [MIC1, MIC3 ref].
// The codec driver blocks until the requested data is moved.
esp_err_t audio_read(void *buffer, size_t length, size_t *bytes_read);
esp_err_t audio_write(void *buffer, size_t length, size_t *bytes_written);

esp_err_t audio_set_mute(bool muted);
// Speaker volume is an integer 0–100, restored from NVS at boot.
esp_err_t audio_get_volume(int *volume);
esp_err_t audio_set_volume(int volume);

#endif
