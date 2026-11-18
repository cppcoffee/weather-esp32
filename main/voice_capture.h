#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_wn_models.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/stream_buffer.h"
#include "model_path.h"

// Capture task: reads the I2S microphone, runs the AEC front-end when it is available, and
// routes the result to the upload buffer or to WakeNet. It only observes the shared event
// group; the session task owns both stream buffers and every protocol call.
esp_err_t voice_capture_start(EventGroupHandle_t events, StreamBufferHandle_t microphone,
                              StreamBufferHandle_t reference, srmodel_list_t *models, const esp_wn_iface_t *wakenet,
                              model_iface_data_t *wake_model, int wake_samples);
void voice_capture_stop(void);

// False when the echo canceller could not be created. The session must then keep WakeNet
// disarmed while a reply plays, because the microphone would hear the speaker directly.
bool voice_capture_has_aec(void);

// Allocate the optional follow-up detector before enabling its event bit.
bool voice_capture_prepare_vad(void);
