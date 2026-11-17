#include "voice_capture.h"

#include <string.h>

#include "audio.h"
#include "esp_afe_config.h"
#include "esp_afe_sr_models.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "voice_audio.h"
#include "voice_internal.h"

static const char *TAG = "voice_cap";

static EventGroupHandle_t s_events;
static StreamBufferHandle_t s_microphone;
static StreamBufferHandle_t s_reference;
static const esp_wn_iface_t *s_wakenet;
static model_iface_data_t *s_wake_model;
static int s_wake_samples;
static TaskHandle_t s_task;

// Echo canceller: interleaved microphone + playback reference in, echo-free microphone out.
static const esp_afe_sr_iface_t *s_afe;
static esp_afe_sr_data_t *s_afe_data;
static int s_feed_samples;
static int16_t *s_afe_in;
static int s_afe_fill;

static int16_t s_capture_stereo[VOICE_FRAME_SAMPLES * 2];
static int16_t s_capture_mono[VOICE_FRAME_SAMPLES];
static int16_t s_ref_scratch[VOICE_FRAME_SAMPLES];
// WakeNet still wants whole chunks, which an AFE frame size need not match.
static int16_t s_wake_chunk[VOICE_FRAME_SAMPLES];
static int s_wake_fill;

// AEC removes the assistant's own playback from the microphone so WakeNet can stay armed while
// the reply plays. WakeNet itself stays outside the AFE: the existing detection path, model
// wiring and chunk checks are unchanged, and only the echo-cancelled signal is added.
static esp_err_t init_afe(srmodel_list_t *models)
{
    afe_config_t *config = afe_config_init("MR", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (config == NULL) {
        return ESP_ERR_NO_MEM;
    }
    config->aec_init = true;
    config->aec_mode = AEC_MODE_SR_LOW_COST;
    config->wakenet_init = false;
    config->vad_init = false;
    config->agc_init = false;
    config->se_init = false;
    config->ns_init = false;
    config->fixed_output_channel = true;
    config->output_playback_channel = false;
    config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    // The AFE keeps its configuration for its lifetime; this component never frees it.
    config = afe_config_check(config);
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_afe = esp_afe_handle_from_config(config);
    if (s_afe == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    s_afe_data = s_afe->create_from_config(config);
    if (s_afe_data == NULL) {
        s_afe = NULL;
        return ESP_ERR_NO_MEM;
    }
    int feed = s_afe->get_feed_chunksize(s_afe_data);
    if (feed <= 0 || s_afe->get_feed_channel_num(s_afe_data) != 2 || s_afe->get_fetch_channel_num(s_afe_data) != 1
        || s_afe->get_samp_rate(s_afe_data) != VOICE_SAMPLE_RATE)
    {
        s_afe->destroy(s_afe_data);
        s_afe_data = NULL;
        s_afe = NULL;
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_afe_in = heap_caps_malloc(sizeof(int16_t) * 2 * (size_t)feed, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_afe_in == NULL) {
        s_afe->destroy(s_afe_data);
        s_afe_data = NULL;
        s_afe = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_feed_samples = feed;
    ESP_LOGI(TAG, "AEC front-end ready: %d samples/frame", feed);
    return ESP_OK;
}

// A hit while a turn is already running means the user talked over the reply. The session task
// mutes the speaker, drops the aborted reply and restarts listening.
static void on_wake_word(void)
{
    s_wakenet->clean(s_wake_model);
    xEventGroupClearBits(s_events, VOICE_BIT_ARMED);
    s_wake_fill = 0;
    xStreamBufferReset(s_microphone);
    if (xEventGroupGetBits(s_events) & VOICE_BIT_RECEIVING) {
        xEventGroupSetBits(s_events, VOICE_BIT_CAPTURE | VOICE_BIT_INTERRUPT);
        ESP_LOGI(TAG, "barge-in: wake word detected while speaking");
        return;
    }
    xEventGroupSetBits(s_events, VOICE_BIT_CAPTURE | VOICE_BIT_TRIGGER);
    ESP_LOGI(TAG, "wake word detected");
}

// Routes processed microphone audio either to the upload buffer or to WakeNet. Capture wins:
// a turn never detects a second wake word while it is recording the question.
static void route_microphone(const int16_t *mono, size_t samples)
{
    if (xEventGroupGetBits(s_events) & VOICE_BIT_CAPTURE) {
        // Preserve the start of the question while the chat audio channel opens.
        size_t bytes = samples * sizeof(int16_t);
        if (xStreamBufferSpacesAvailable(s_microphone) < bytes) {
            xEventGroupClearBits(s_events, VOICE_BIT_CAPTURE);
            xEventGroupSetBits(s_events, VOICE_BIT_ERROR);
        } else {
            xStreamBufferSend(s_microphone, mono, bytes, 0);
        }
        s_wake_fill = 0;
        return;
    }
    if (!(xEventGroupGetBits(s_events) & VOICE_BIT_ARMED)) {
        s_wake_fill = 0;
        return;
    }
    while (samples > 0) {
        size_t take = (size_t)s_wake_samples - (size_t)s_wake_fill;
        if (take > samples) {
            take = samples;
        }
        memcpy(&s_wake_chunk[s_wake_fill], mono, take * sizeof(int16_t));
        s_wake_fill += (int)take;
        mono += take;
        samples -= take;
        if (s_wake_fill == s_wake_samples) {
            s_wake_fill = 0;
            if (s_wakenet->detect(s_wake_model, s_wake_chunk) == WAKENET_DETECTED) {
                on_wake_word();
                return;
            }
        }
    }
}

// AEC needs the playback reference on the same timeline as the microphone, so both go into one
// interleaved frame. Silence stands in whenever nothing is playing, and the echo-cancelled
// result is routed exactly like the raw microphone path.
static void afe_process(const int16_t *mono, size_t samples)
{
    size_t index = 0;
    while (index < samples) {
        size_t room = (size_t)s_feed_samples - (size_t)s_afe_fill;
        size_t take = samples - index < room ? samples - index : room;
        size_t got = 0;
        while (got < take) {
            size_t bytes = xStreamBufferReceive(s_reference, s_ref_scratch + got, (take - got) * sizeof(int16_t), 0);
            if (bytes < sizeof(int16_t)) {
                break;
            }
            got += bytes / sizeof(int16_t);
        }
        for (size_t i = 0; i < take; i++) {
            s_afe_in[(s_afe_fill + i) * 2] = mono[index + i];
            s_afe_in[(s_afe_fill + i) * 2 + 1] = i < got ? s_ref_scratch[i] : 0;
        }
        s_afe_fill += (int)take;
        index += take;
        if (s_afe_fill != s_feed_samples) {
            continue;
        }
        s_afe->feed(s_afe_data, s_afe_in);
        s_afe_fill = 0;
        for (;;) {
            afe_fetch_result_t *result = s_afe->fetch_with_delay(s_afe_data, 0);
            if (result == NULL || result->data == NULL || result->data_size <= 0) {
                break;
            }
            route_microphone(result->data, (size_t)result->data_size / sizeof(int16_t));
        }
    }
}

static void capture_task(void *arg)
{
    (void)arg;
    const size_t stereo_bytes = s_wake_samples * 2 * sizeof(int16_t);
    for (;;) {
        xEventGroupWaitBits(s_events, VOICE_BIT_ARMED | VOICE_BIT_CAPTURE, pdFALSE, pdFALSE, portMAX_DELAY);
        if (!VOICE_NETWORK_READY(xEventGroupGetBits(s_events))) {
            vTaskDelay(pdMS_TO_TICKS(VOICE_POLL_MS));
            continue;
        }
        size_t read = 0;
        esp_err_t err = audio_read(s_capture_stereo, stereo_bytes, &read);
        EventBits_t bits = xEventGroupGetBits(s_events);
        if (err != ESP_OK || read != stereo_bytes) {
            if (bits & VOICE_BIT_CAPTURE) {
                xEventGroupSetBits(s_events, VOICE_BIT_ERROR);
            }
            s_wakenet->clean(s_wake_model);
            s_wake_fill = 0;
            vTaskDelay(pdMS_TO_TICKS(VOICE_POLL_MS));
            continue;
        }
        voice_stereo_to_mono(s_capture_stereo, s_capture_mono, s_wake_samples);
        if (s_afe != NULL) {
            afe_process(s_capture_mono, s_wake_samples);
        } else {
            route_microphone(s_capture_mono, s_wake_samples);
        }
    }
}

esp_err_t voice_capture_start(EventGroupHandle_t events, StreamBufferHandle_t microphone,
                              StreamBufferHandle_t reference, srmodel_list_t *models, const esp_wn_iface_t *wakenet,
                              model_iface_data_t *wake_model, int wake_samples)
{
    if (events == NULL || microphone == NULL || reference == NULL || wakenet == NULL || wake_model == NULL
        || wake_samples <= 0 || wake_samples > VOICE_FRAME_SAMPLES || s_task != NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    s_events = events;
    s_microphone = microphone;
    s_reference = reference;
    s_wakenet = wakenet;
    s_wake_model = wake_model;
    s_wake_samples = wake_samples;
    // Without AEC the session keeps WakeNet disarmed during playback; that is not an error.
    esp_err_t err = init_afe(models);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AEC unavailable (%s); wake word disabled during playback", esp_err_to_name(err));
    }
    if (xTaskCreate(capture_task, "wake", WAKE_TASK_STACK, NULL, VOICE_TASK_PRIO, &s_task) != pdPASS) {
        voice_capture_stop();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

// Only used before the session task starts, so the blocked capture task can be deleted safely.
void voice_capture_stop(void)
{
    if (s_task != NULL) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    if (s_afe_data != NULL) {
        s_afe->destroy(s_afe_data);
        s_afe_data = NULL;
    }
    if (s_afe_in != NULL) {
        heap_caps_free(s_afe_in);
        s_afe_in = NULL;
    }
    s_afe = NULL;
    s_feed_samples = 0;
    s_afe_fill = 0;
    s_wake_fill = 0;
    s_events = NULL;
    s_microphone = NULL;
    s_reference = NULL;
    s_wakenet = NULL;
    s_wake_model = NULL;
    s_wake_samples = 0;
}

bool voice_capture_has_aec(void)
{
    return s_afe != NULL;
}
