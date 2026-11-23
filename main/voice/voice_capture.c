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

#define WAKE_TAIL_MUTE_MS    120
#define CHIME_TAIL_IGNORE_MS 60
#define CHIME_MUTE_MS        (VOICE_FRAME_MS + AUDIO_PLAYBACK_TAIL_MS + CHIME_TAIL_IGNORE_MS)
#define SAMPLES_PER_MS       (VOICE_SAMPLE_RATE / 1000)

struct voice_capture_state {
    EventGroupHandle_t   events;
    StreamBufferHandle_t microphone;
#if !AUDIO_USE_ES7210_AEC
    StreamBufferHandle_t reference;
#endif
    TaskHandle_t task;

    // Unified ESP-SR AFE pipeline: AEC + WakeNet + WebRTC VAD (with pre-roll
    // vad_cache).
    const esp_afe_sr_iface_t *afe;
    esp_afe_sr_data_t        *afe_data;
    int16_t                  *afe_in;
#if !AUDIO_USE_ES7210_AEC
    int16_t *stereo;
    int16_t *mono;
    int16_t *reference_scratch;
#endif
    int feed_samples;

    bool     wn_enabled;
    unsigned chime_mute_samples;
#if !AUDIO_USE_ES7210_AEC
    bool     in_followup;
    bool     endpoint_heard_speech;
    unsigned endpoint_holdoff_samples;
#endif
};

static struct voice_capture_state capture_state;

static esp_err_t
init_afe(srmodel_list_t *models)
{
    const char *wn_name = models ? esp_srmodel_filter(models, "wn9_xiaoluxiaolu_tts2", NULL) : NULL;
    if (wn_name == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
#if AUDIO_USE_ES7210_AEC
    afe_config_t *config = afe_config_init("MR", models, AFE_TYPE_FD, AFE_MODE_LOW_COST);
#else
    afe_config_t *config = afe_config_init("MR", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
#endif
    if (config == NULL) {
        return ESP_ERR_NO_MEM;
    }
    config->aec_init = true;
#if AUDIO_USE_ES7210_AEC
    config->aec_mode = AEC_MODE_FD_LOW_COST;
    config->aec_nlp_level = AEC_NLP_LEVEL_VERYAGGR;
#else
    config->aec_mode = AEC_MODE_SR_LOW_COST;
#endif
    config->wakenet_init = true;
    config->wakenet_model_name = (char *)wn_name;
    config->wakenet_mode = DET_MODE_90;
    config->vad_init = true;
    config->vad_mode = VAD_MODE_1;
    config->vad_model_name = NULL; // lightweight WebRTC VAD
    config->vad_min_speech_ms = AUDIO_USE_ES7210_AEC ? 128 : 150;
    config->vad_min_noise_ms = AUDIO_USE_ES7210_AEC ? 500 : 900;
    config->vad_delay_ms = 128;
    config->agc_init = false;
    config->se_init = false;
    config->ns_init = false;
    config->fixed_output_channel = true;
    config->output_playback_channel = false;
    config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    config->afe_perferred_core = VOICE_AUDIO_CORE;
    config->afe_perferred_priority = AFE_TASK_PRIO;
    // The AFE keeps its configuration for its lifetime; this component never
    // frees it.
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
#if AUDIO_USE_ES7210_AEC
    if (!capture_state.afe_in) {
        return ESP_ERR_NO_MEM;
    }
#else
    capture_state.stereo = heap_caps_malloc(sizeof(int16_t) * 2 * (size_t)feed, caps);
    capture_state.mono = heap_caps_malloc(sizeof(int16_t) * (size_t)feed, caps);
    capture_state.reference_scratch = heap_caps_malloc(sizeof(int16_t) * (size_t)feed, caps);
    if (!capture_state.afe_in || !capture_state.stereo || !capture_state.mono || !capture_state.reference_scratch) {
        return ESP_ERR_NO_MEM;
    }
#endif
    capture_state.feed_samples = feed;
    capture_state.wn_enabled = true;
    ESP_LOGI(log_tag, "AFE ready (%sAEC+WakeNet+VAD): %d samples/frame", AUDIO_USE_ES7210_AEC ? "ES7210 " : "", feed);
    return ESP_OK;
}

static void
sync_wakenet(bool enable)
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

// Shared capture start for wake word and button wake / barge-in.
static void
begin_capture(bool receiving, bool from_wake_word, const char *reason)
{
    xStreamBufferReset(capture_state.microphone);
    capture_state.afe->reset_vad(capture_state.afe_data);
    capture_state.chime_mute_samples = from_wake_word ? (WAKE_TAIL_MUTE_MS * SAMPLES_PER_MS) : 0;
#if !AUDIO_USE_ES7210_AEC
    // Ignore the wake word's tail or barge-in chime before local EOS can arm.
    capture_state.endpoint_holdoff_samples = VOICE_WAKE_MIN_CAPTURE_MS * SAMPLES_PER_MS;
    capture_state.endpoint_heard_speech = false;
#endif
    xEventGroupClearBits(capture_state.events, VOICE_BIT_LOCAL_EOS | VOICE_BIT_SPEECH);
    if (receiving) {
        xEventGroupSetBits(capture_state.events, VOICE_BIT_CAPTURE | VOICE_BIT_INTERRUPT);
        ESP_LOGI(log_tag, "%s", reason);
        return;
    }
    voice_feedback_publish(VOICE_STATE_CONNECTING, NULL);
    xEventGroupSetBits(capture_state.events, VOICE_BIT_CAPTURE | VOICE_BIT_TRIGGER);
    ESP_LOGI(log_tag, "%s", reason);
}

// A hit while a turn is already running means the user talked over the reply.
// The session task mutes the speaker, drops the aborted reply and restarts
// listening.
static void
on_wake_word(void)
{
    EventBits_t bits = xEventGroupClearBits(capture_state.events, VOICE_BIT_ARMED);
    sync_wakenet(false);
    if (!VOICE_LOCAL_READY(bits) || !(bits & VOICE_BIT_ARMED)) {
        return;
    }
    bool receiving = (bits & VOICE_BIT_RECEIVING) != 0;
    begin_capture(receiving, true, receiving ? "barge-in: wake word detected while speaking" : "wake word detected");
}

static void
buffer_microphone(const int16_t *mono, size_t samples)
{
    size_t bytes = samples * sizeof(int16_t);
    if (xStreamBufferSpacesAvailable(capture_state.microphone) < bytes) {
        EventBits_t bits = xEventGroupGetBits(capture_state.events);
#if AUDIO_USE_ES7210_AEC
        if (!(bits & VOICE_BIT_TRIGGER)) {
            xEventGroupSetBits(capture_state.events, VOICE_BIT_ERROR);
            xEventGroupClearBits(capture_state.events, VOICE_BIT_CAPTURE);
        }
#else
        // Keep the buffered question if connection setup outlasts the pre-roll.
        xEventGroupSetBits(capture_state.events, (bits & VOICE_BIT_TRIGGER) ? VOICE_BIT_LOCAL_EOS : VOICE_BIT_ERROR);
        xEventGroupClearBits(capture_state.events, VOICE_BIT_CAPTURE);
#endif
    } else {
        xStreamBufferSend(capture_state.microphone, mono, bytes, 0);
    }
}

static void
handle_afe_result(const afe_fetch_result_t *result)
{
    size_t      samples = (size_t)result->data_size / sizeof(int16_t);
    EventBits_t bits = xEventGroupGetBits(capture_state.events);
    if (bits & VOICE_BIT_BTN_WAKE) {
        EventBits_t prev = xEventGroupClearBits(capture_state.events, VOICE_BIT_BTN_WAKE | VOICE_BIT_ARMED);
        sync_wakenet(false);
        if (VOICE_LOCAL_READY(prev) && (prev & VOICE_BIT_ARMED)
            && !(prev & (VOICE_BIT_RECEIVING | VOICE_BIT_CAPTURE | VOICE_BIT_TRIGGER | VOICE_BIT_HANGUP)))
        {
            begin_capture(false, false, "wake: button pressed");
        }
        return;
    }
    if (bits & VOICE_BIT_CHIME) {
        xEventGroupClearBits(capture_state.events, VOICE_BIT_CHIME | VOICE_BIT_SPEECH);
        xStreamBufferReset(capture_state.microphone);
        capture_state.afe->reset_vad(capture_state.afe_data);
        capture_state.chime_mute_samples = CHIME_MUTE_MS * SAMPLES_PER_MS;
#if !AUDIO_USE_ES7210_AEC
        capture_state.endpoint_heard_speech = false;
#endif
    }

#if AUDIO_USE_ES7210_AEC
    if (bits & VOICE_BIT_CAPTURE) {
        sync_wakenet(false);
        if (capture_state.chime_mute_samples > 0) {
            if (capture_state.chime_mute_samples > samples) {
                capture_state.chime_mute_samples -= (unsigned)samples;
            } else {
                capture_state.chime_mute_samples = 0;
            }
            capture_state.afe->reset_vad(capture_state.afe_data);
            return;
        }
        buffer_microphone(result->data, samples);
        if (result->vad_state == VAD_SPEECH) {
            if (!(bits & VOICE_BIT_SPEECH)) {
                xEventGroupSetBits(capture_state.events, VOICE_BIT_SPEECH);
            }
        } else if (bits & VOICE_BIT_SPEECH) {
            xEventGroupClearBits(capture_state.events, VOICE_BIT_SPEECH);
        }
        return;
    }
#else
    bool followup = (bits & VOICE_BIT_FOLLOWUP) != 0;
    if (followup != capture_state.in_followup) {
        if (followup) {
            capture_state.afe->reset_vad(capture_state.afe_data);
            capture_state.endpoint_heard_speech = false;
        }
        capture_state.in_followup = followup;
    }

    if (bits & VOICE_BIT_CAPTURE) {
        sync_wakenet(false);
        if (bits & VOICE_BIT_LOCAL_EOS) {
            return;
        }
        if (capture_state.chime_mute_samples > 0) {
            if (capture_state.chime_mute_samples > samples) {
                capture_state.chime_mute_samples -= (unsigned)samples;
            } else {
                capture_state.chime_mute_samples = 0;
            }
            capture_state.afe->reset_vad(capture_state.afe_data);
            return;
        }
        if (followup) {
            // Stream for server VAD (faster), with local VAD (~1 s) as safety net
            // so soft speech or brief clicks never stall the 20 s turn timeout.
            buffer_microphone(result->data, samples);
            if (result->vad_state == VAD_SPEECH) {
                capture_state.endpoint_heard_speech = true;
                if (!(bits & VOICE_BIT_SPEECH)) {
                    xEventGroupSetBits(capture_state.events, VOICE_BIT_SPEECH);
                }
            } else if (capture_state.endpoint_heard_speech) {
                xEventGroupSetBits(capture_state.events, VOICE_BIT_LOCAL_EOS);
                xEventGroupClearBits(capture_state.events, VOICE_BIT_CAPTURE);
            }
            return;
        }
        buffer_microphone(result->data, samples);
        if (capture_state.endpoint_holdoff_samples > samples) {
            capture_state.endpoint_holdoff_samples -= (unsigned)samples;
        } else {
            capture_state.endpoint_holdoff_samples = 0;
        }
        if (result->vad_state == VAD_SPEECH) {
            capture_state.endpoint_heard_speech = true;
            if (!(bits & VOICE_BIT_SPEECH)) {
                xEventGroupSetBits(capture_state.events, VOICE_BIT_SPEECH);
            }
        } else if (capture_state.endpoint_heard_speech && capture_state.endpoint_holdoff_samples == 0) {
            xEventGroupSetBits(capture_state.events, VOICE_BIT_LOCAL_EOS);
            xEventGroupClearBits(capture_state.events, VOICE_BIT_CAPTURE);
        }
        return;
    }

    capture_state.endpoint_heard_speech = false;
#endif
    bool armed = (bits & VOICE_BIT_ARMED) != 0;
    sync_wakenet(armed);
    if (armed && result->wakeup_state == WAKENET_DETECTED) {
        on_wake_word();
    }
}

static void
capture_task(void *arg)
{
    (void)arg;
    const size_t feed = (size_t)capture_state.feed_samples;
    const size_t stereo_bytes = feed * 2 * sizeof(int16_t);
    for (;;) {
        xEventGroupWaitBits(capture_state.events,
                            VOICE_BIT_ARMED | VOICE_BIT_CAPTURE | VOICE_BIT_FOLLOWUP | VOICE_BIT_BTN_WAKE, pdFALSE,
                            pdFALSE, portMAX_DELAY);
        if (!VOICE_LOCAL_READY(xEventGroupGetBits(capture_state.events))) {
            vTaskDelay(pdMS_TO_TICKS(VOICE_POLL_MS));
            continue;
        }
        size_t read = 0;
#if AUDIO_USE_ES7210_AEC
        // ES7210 TDM delivers hardware-aligned [MIC1, MIC3 ref] directly in AFE
        // "MR" interleaved layout.
        esp_err_t err = audio_read(capture_state.afe_in, stereo_bytes, &read);
#else
        esp_err_t err = audio_read(capture_state.stereo, stereo_bytes, &read);
#endif
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
#if !AUDIO_USE_ES7210_AEC
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
#endif
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

esp_err_t
voice_capture_start(EventGroupHandle_t events, StreamBufferHandle_t microphone, StreamBufferHandle_t reference,
                    srmodel_list_t *models)
{
    if (events == NULL || microphone == NULL || models == NULL || capture_state.task != NULL
#if !AUDIO_USE_ES7210_AEC
        || reference == NULL
#endif
    )
    {
        return ESP_ERR_INVALID_ARG;
    }
    capture_state.events = events;
    capture_state.microphone = microphone;
#if AUDIO_USE_ES7210_AEC
    (void)reference;
#else
    capture_state.reference = reference;
#endif
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

// Only used before the session task starts, so the blocked capture task can be
// deleted safely.
void
voice_capture_stop(void)
{
    if (capture_state.task != NULL) {
        vTaskDelete(capture_state.task);
    }
    if (capture_state.afe_data != NULL) {
        capture_state.afe->destroy(capture_state.afe_data);
    }
    heap_caps_free(capture_state.afe_in);
#if !AUDIO_USE_ES7210_AEC
    heap_caps_free(capture_state.stereo);
    heap_caps_free(capture_state.mono);
    heap_caps_free(capture_state.reference_scratch);
#endif
    capture_state = (struct voice_capture_state){0};
}
