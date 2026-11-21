#include "voice_capture.h"

#include <string.h>

#include "audio.h"
#include "esp_afe_config.h"
#include "esp_afe_sr_models.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "voice_audio.h"
#include "voice_feedback.h"
#include "voice_internal.h"

static const char *log_tag = "voice_cap";

#define WAKE_TAIL_IGNORE_MS 300
#define SAMPLES_PER_MS      (VOICE_SAMPLE_RATE / 1000)

struct voice_capture_state {
    EventGroupHandle_t events;
    StreamBufferHandle_t microphone;
    StreamBufferHandle_t reference;
    TaskHandle_t task;

    // Unified ESP-SR AFE pipeline: AEC + WakeNet + WebRTC VAD (with pre-roll vad_cache).
    const esp_afe_sr_iface_t *afe;
    esp_afe_sr_data_t *afe_data;
    int16_t *afe_in;
    int16_t *stereo;
    int16_t *mono;
    int16_t *reference_scratch;
    int feed_samples;

    bool wn_enabled;
    bool in_followup;
    bool endpoint_heard_speech;
    unsigned endpoint_holdoff_samples;
};

static struct voice_capture_state capture_state;

static esp_err_t init_afe(srmodel_list_t *models)
{
    const char *wn_name = models ? esp_srmodel_filter(models, "wn9_xiaoluxiaolu_tts2", NULL) : NULL;
    if (wn_name == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    afe_config_t *config = afe_config_init("MR", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (config == NULL) {
        return ESP_ERR_NO_MEM;
    }
    config->aec_init = true;
    config->aec_mode = AEC_MODE_SR_LOW_COST;
    config->wakenet_init = true;
    config->wakenet_model_name = (char *)wn_name;
    config->wakenet_mode = DET_MODE_90;
    config->vad_init = true;
    config->vad_mode = VAD_MODE_1;
    config->vad_model_name = NULL; // lightweight WebRTC VAD
    config->vad_min_speech_ms = 64;
    config->vad_min_noise_ms = 900;
    config->vad_delay_ms = 128;
    config->agc_init = false;
    config->se_init = false;
    config->ns_init = false;
    config->fixed_output_channel = true;
    config->output_playback_channel = false;
    config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    config->afe_perferred_core = VOICE_AUDIO_CORE;
    config->afe_perferred_priority = AFE_TASK_PRIO;
    // The AFE keeps its configuration for its lifetime; this component never frees it.
    config = afe_config_check(config);
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    capture_state.afe = esp_afe_handle_from_config(config);
    if (capture_state.afe == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    capture_state.afe_data = capture_state.afe->create_from_config(config);
    if (capture_state.afe_data == NULL) {
        capture_state.afe = NULL;
        return ESP_ERR_NO_MEM;
    }
    int feed = capture_state.afe->get_feed_chunksize(capture_state.afe_data);
    if (feed <= 0 || capture_state.afe->get_feed_channel_num(capture_state.afe_data) != 2
        || capture_state.afe->get_fetch_channel_num(capture_state.afe_data) != 1
        || capture_state.afe->get_samp_rate(capture_state.afe_data) != VOICE_SAMPLE_RATE)
    {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    capture_state.afe_in = heap_caps_malloc(sizeof(int16_t) * 2 * (size_t)feed, caps);
    capture_state.stereo = heap_caps_malloc(sizeof(int16_t) * 2 * (size_t)feed, caps);
    capture_state.mono = heap_caps_malloc(sizeof(int16_t) * (size_t)feed, caps);
    capture_state.reference_scratch = heap_caps_malloc(sizeof(int16_t) * (size_t)feed, caps);
    if (!capture_state.afe_in || !capture_state.stereo || !capture_state.mono || !capture_state.reference_scratch) {
        return ESP_ERR_NO_MEM;
    }
    capture_state.feed_samples = feed;
    capture_state.wn_enabled = true;
    ESP_LOGI(log_tag, "AFE ready (AEC+WakeNet+VAD): %d samples/frame", feed);
    return ESP_OK;
}

static void sync_wakenet(bool enable)
{
    if (enable == capture_state.wn_enabled) {
        return;
    }
    if (enable) {
        capture_state.afe->enable_wakenet(capture_state.afe_data);
    } else {
        capture_state.afe->disable_wakenet(capture_state.afe_data);
    }
    capture_state.wn_enabled = enable;
}

// A hit while a turn is already running means the user talked over the reply. The session task
// mutes the speaker, drops the aborted reply and restarts listening.
static void on_wake_word(void)
{
    EventBits_t bits = xEventGroupClearBits(capture_state.events, VOICE_BIT_ARMED);
    sync_wakenet(false);
    if (!VOICE_LOCAL_READY(bits) || !(bits & VOICE_BIT_ARMED)) {
        return;
    }
    xStreamBufferReset(capture_state.microphone);
    capture_state.afe->reset_vad(capture_state.afe_data);
    // The wake word's tail can trigger VAD before the question starts.
    capture_state.endpoint_holdoff_samples = VOICE_WAKE_MIN_CAPTURE_MS * SAMPLES_PER_MS;
    capture_state.endpoint_heard_speech = false;
    xEventGroupClearBits(capture_state.events, VOICE_BIT_LOCAL_EOS);
    if (bits & VOICE_BIT_RECEIVING) {
        xEventGroupSetBits(capture_state.events, VOICE_BIT_CAPTURE | VOICE_BIT_INTERRUPT);
        ESP_LOGI(log_tag, "barge-in: wake word detected while speaking");
        return;
    }
    voice_feedback_publish(VOICE_STATE_CONNECTING, NULL);
    xEventGroupSetBits(capture_state.events, VOICE_BIT_CAPTURE | VOICE_BIT_TRIGGER);
    ESP_LOGI(log_tag, "wake word detected");
}

static void buffer_microphone(const int16_t *mono, size_t samples)
{
    size_t bytes = samples * sizeof(int16_t);
    if (xStreamBufferSpacesAvailable(capture_state.microphone) < bytes) {
        EventBits_t bits = xEventGroupGetBits(capture_state.events);
        // Keep the buffered question if connection setup outlasts the pre-roll.
        xEventGroupSetBits(capture_state.events, (bits & VOICE_BIT_TRIGGER) ? VOICE_BIT_LOCAL_EOS : VOICE_BIT_ERROR);
        xEventGroupClearBits(capture_state.events, VOICE_BIT_CAPTURE);
    } else {
        xStreamBufferSend(capture_state.microphone, mono, bytes, 0);
    }
}

static void handle_afe_result(const afe_fetch_result_t *result)
{
    size_t samples = (size_t)result->data_size / sizeof(int16_t);
    EventBits_t bits = xEventGroupGetBits(capture_state.events);
    bool followup = (bits & VOICE_BIT_FOLLOWUP) != 0;
    if (followup != capture_state.in_followup) {
        if (followup) {
            capture_state.afe->reset_vad(capture_state.afe_data);
        }
        capture_state.in_followup = followup;
    }

    if ((bits & VOICE_BIT_CAPTURE) || (followup && (bits & VOICE_BIT_SPEECH))) {
        sync_wakenet(false);
        if (bits & VOICE_BIT_LOCAL_EOS) {
            return;
        }
        buffer_microphone(result->data, samples);
        if (capture_state.endpoint_holdoff_samples > samples) {
            capture_state.endpoint_holdoff_samples -= (unsigned)samples;
        } else {
            capture_state.endpoint_holdoff_samples = 0;
        }
        // Ignore the wake word's own acoustic tail during the first 300 ms after wake.
        if (capture_state.endpoint_holdoff_samples > (VOICE_WAKE_MIN_CAPTURE_MS - WAKE_TAIL_IGNORE_MS) * SAMPLES_PER_MS)
        {
            capture_state.afe->reset_vad(capture_state.afe_data);
            return;
        }
        if (result->vad_state == VAD_SPEECH) {
            capture_state.endpoint_heard_speech = true;
        } else if (capture_state.endpoint_heard_speech && capture_state.endpoint_holdoff_samples == 0) {
            xEventGroupSetBits(capture_state.events, VOICE_BIT_LOCAL_EOS);
            xEventGroupClearBits(capture_state.events, VOICE_BIT_CAPTURE);
        }
        return;
    }

    if (followup && !(bits & VOICE_BIT_LOCAL_EOS)) {
        sync_wakenet(false);
        if (result->vad_state != VAD_SPEECH || !VOICE_NETWORK_READY(bits) || (bits & VOICE_END_BITS)) {
            return;
        }
        xStreamBufferReset(capture_state.microphone);
        if (result->vad_cache != NULL && result->vad_cache_size > 0) {
            buffer_microphone(result->vad_cache, (size_t)result->vad_cache_size / sizeof(int16_t));
        }
        buffer_microphone(result->data, samples);
        capture_state.endpoint_holdoff_samples = 0;
        capture_state.endpoint_heard_speech = true;
        xEventGroupClearBits(capture_state.events, VOICE_BIT_LOCAL_EOS);
        xEventGroupSetBits(capture_state.events, VOICE_BIT_SPEECH);
        return;
    }

    bool armed = (bits & VOICE_BIT_ARMED) != 0;
    sync_wakenet(armed);
    if (armed && result->wakeup_state == WAKENET_DETECTED) {
        on_wake_word();
    }
}

static void capture_task(void *arg)
{
    (void)arg;
    const size_t feed = (size_t)capture_state.feed_samples;
    const size_t stereo_bytes = feed * 2 * sizeof(int16_t);
    for (;;) {
        xEventGroupWaitBits(capture_state.events, VOICE_BIT_ARMED | VOICE_BIT_CAPTURE | VOICE_BIT_FOLLOWUP, pdFALSE,
                            pdFALSE, portMAX_DELAY);
        if (!VOICE_LOCAL_READY(xEventGroupGetBits(capture_state.events))) {
            vTaskDelay(pdMS_TO_TICKS(VOICE_POLL_MS));
            continue;
        }
        size_t read = 0;
        esp_err_t err = audio_read(capture_state.stereo, stereo_bytes, &read);
        EventBits_t bits = xEventGroupGetBits(capture_state.events);
        if (!VOICE_LOCAL_READY(bits)) {
            continue;
        }
        if (err != ESP_OK || read != stereo_bytes) {
            if (bits & VOICE_BIT_CAPTURE) {
                xEventGroupSetBits(capture_state.events, VOICE_BIT_ERROR);
            }
            vTaskDelay(pdMS_TO_TICKS(VOICE_POLL_MS));
            continue;
        }
        voice_stereo_to_mono(capture_state.stereo, capture_state.mono, feed);
        size_t got = 0;
        while (got < feed) {
            size_t bytes = xStreamBufferReceive(capture_state.reference, capture_state.reference_scratch + got,
                                                (feed - got) * sizeof(int16_t), 0);
            if (bytes < sizeof(int16_t)) {
                break;
            }
            got += bytes / sizeof(int16_t);
        }
        for (size_t i = 0; i < feed; i++) {
            capture_state.afe_in[i * 2] = capture_state.mono[i];
            capture_state.afe_in[i * 2 + 1] = i < got ? capture_state.reference_scratch[i] : 0;
        }
        capture_state.afe->feed(capture_state.afe_data, capture_state.afe_in);
        for (;;) {
            afe_fetch_result_t *result = capture_state.afe->fetch_with_delay(capture_state.afe_data, 0);
            if (result == NULL || result->data == NULL || result->data_size <= 0) {
                break;
            }
            handle_afe_result(result);
        }
    }
}

esp_err_t voice_capture_start(EventGroupHandle_t events, StreamBufferHandle_t microphone,
                              StreamBufferHandle_t reference, srmodel_list_t *models)
{
    if (events == NULL || microphone == NULL || reference == NULL || models == NULL || capture_state.task != NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    capture_state.events = events;
    capture_state.microphone = microphone;
    capture_state.reference = reference;
    esp_err_t err = init_afe(models);
    if (err != ESP_OK) {
        ESP_LOGW(log_tag, "AFE init failed: %s", esp_err_to_name(err));
        voice_capture_stop();
        return err;
    }
    if (xTaskCreatePinnedToCore(capture_task, "wake", WAKE_TASK_STACK, NULL, WAKE_TASK_PRIO, &capture_state.task,
                                VOICE_AUDIO_CORE)
        != pdPASS)
    {
        voice_capture_stop();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

// Only used before the session task starts, so the blocked capture task can be deleted safely.
void voice_capture_stop(void)
{
    if (capture_state.task != NULL) {
        vTaskDelete(capture_state.task);
    }
    if (capture_state.afe_data != NULL) {
        capture_state.afe->destroy(capture_state.afe_data);
    }
    heap_caps_free(capture_state.afe_in);
    heap_caps_free(capture_state.stereo);
    heap_caps_free(capture_state.mono);
    heap_caps_free(capture_state.reference_scratch);
    capture_state = (struct voice_capture_state){0};
}
