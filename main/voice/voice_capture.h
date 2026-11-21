#ifndef __VOICE_CAPTURE_H__
#define __VOICE_CAPTURE_H__

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/stream_buffer.h"
#include "model_path.h"

// Capture task: reads the I2S microphone and feeds ESP-SR AFE (AEC + WakeNet + VAD).
// It only observes the shared event group; the session task owns both stream buffers
// and every protocol call.
esp_err_t voice_capture_start(EventGroupHandle_t events, StreamBufferHandle_t microphone,
                              StreamBufferHandle_t reference, srmodel_list_t *models);
void voice_capture_stop(void);

#endif
