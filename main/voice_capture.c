#include "voice_capture.h"

#include <string.h>

#include "audio.h"
#include "esp_afe_config.h"
#include "esp_afe_sr_models.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vad.h"
#include "freertos/task.h"
#include "voice_audio.h"
#include "voice_internal.h"

static const char *log_tag = "voice_cap";

#define FOLLOWUP_FRAME_MS       30
#define FOLLOWUP_SAMPLES        (VOICE_SAMPLE_RATE / 1000 * FOLLOWUP_FRAME_MS)
#define FOLLOWUP_HISTORY_FRAMES 4

struct voice_capture_state {
    EventGroupHandle_t events;
    StreamBufferHandle_t microphone;
    StreamBufferHandle_t reference;
    TaskHandle_t task;

    const esp_wn_iface_t *wakenet;
    model_iface_data_t *wake_model;
    int wake_samples;
    int wake_fill;

    // Echo canceller: interleaved microphone + playback reference in, echo-free microphone out.
    const esp_afe_sr_iface_t *afe;
    esp_afe_sr_data_t *afe_data;
    int16_t *afe_in;
    int feed_samples;
    int afe_fill;

    vad_handle_t vad;
    int16_t (*vad_history)[FOLLOWUP_SAMPLES];
    size_t vad_fill, vad_next, vad_count;
    size_t endpoint_fill;
    unsigned endpoint_holdoff_frames;
    bool endpoint_heard_speech;
    bool in_followup;
};

static struct voice_capture_state capture_state;

static struct {
    int16_t stereo[VOICE_FRAME_SAMPLES * 2];
    int16_t mono[VOICE_FRAME_SAMPLES];
    int16_t reference_scratch[VOICE_FRAME_SAMPLES];
    // WakeNet still wants whole chunks, which an AFE frame size need not match.
    int16_t wake_chunk[VOICE_FRAME_SAMPLES];
    int16_t endpoint_frame[FOLLOWUP_SAMPLES];
} capture_buffers;

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
        capture_state.afe->destroy(capture_state.afe_data);
        capture_state.afe_data = NULL;
        capture_state.afe = NULL;
        return ESP_ERR_NOT_SUPPORTED;
    }
    capture_state.afe_in = heap_caps_malloc(sizeof(int16_t) * 2 * (size_t)feed, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (capture_state.afe_in == NULL) {
        capture_state.afe->destroy(capture_state.afe_data);
        capture_state.afe_data = NULL;
        capture_state.afe = NULL;
        return ESP_ERR_NO_MEM;
    }
    capture_state.feed_samples = feed;
    ESP_LOGI(log_tag, "AEC front-end ready: %d samples/frame", feed);
    return ESP_OK;
}

// A hit while a turn is already running means the user talked over the reply. The session task
// mutes the speaker, drops the aborted reply and restarts listening.
static void on_wake_word(void)
{
    // ESP-SR 2.4.7's WakeNet9 clean() can dereference a null queue. Keep the model
    // alive across turns and discard only the application's pending microphone data.
    xEventGroupClearBits(capture_state.events, VOICE_BIT_ARMED);
    capture_state.wake_fill = 0;
    xStreamBufferReset(capture_state.microphone);
    if (capture_state.vad != NULL) {
        vad_reset_trigger(capture_state.vad);
    }
    capture_state.endpoint_fill = 0;
    // The wake word's tail can trigger VAD before the question starts.
    capture_state.endpoint_holdoff_frames = VOICE_WAKE_MIN_CAPTURE_MS / FOLLOWUP_FRAME_MS;
    capture_state.endpoint_heard_speech = false;
    xEventGroupClearBits(capture_state.events, VOICE_BIT_LOCAL_EOS);
    if (xEventGroupGetBits(capture_state.events) & VOICE_BIT_RECEIVING) {
        xEventGroupSetBits(capture_state.events, VOICE_BIT_CAPTURE | VOICE_BIT_INTERRUPT);
        ESP_LOGI(log_tag, "barge-in: wake word detected while speaking");
        return;
    }
    xEventGroupSetBits(capture_state.events, VOICE_BIT_CAPTURE | VOICE_BIT_TRIGGER);
    ESP_LOGI(log_tag, "wake word detected");
}

static void buffer_microphone(const int16_t *mono, size_t samples)
{
    size_t bytes = samples * sizeof(int16_t);
    if (xStreamBufferSpacesAvailable(capture_state.microphone) < bytes) {
        xEventGroupClearBits(capture_state.events, VOICE_BIT_CAPTURE);
        xEventGroupSetBits(capture_state.events, VOICE_BIT_ERROR);
    } else {
        xStreamBufferSend(capture_state.microphone, mono, bytes, 0);
    }
}

static void detect_followup(const int16_t *mono, size_t samples)
{
    while (samples > 0) {
        size_t take = FOLLOWUP_SAMPLES - capture_state.vad_fill;
        if (take > samples) {
            take = samples;
        }
        int16_t *frame = capture_state.vad_history[capture_state.vad_next];
        memcpy(frame + capture_state.vad_fill, mono, take * sizeof(int16_t));
        capture_state.vad_fill += take;
        mono += take;
        samples -= take;
        if (capture_state.vad_fill < FOLLOWUP_SAMPLES) {
            return;
        }
        capture_state.vad_fill = 0;
        capture_state.vad_next = (capture_state.vad_next + 1) % FOLLOWUP_HISTORY_FRAMES;
        if (capture_state.vad_count < FOLLOWUP_HISTORY_FRAMES) {
            capture_state.vad_count++;
        }
        if (vad_process_with_trigger(capture_state.vad, frame) != VAD_SPEECH) {
            continue;
        }
        EventBits_t bits = xEventGroupGetBits(capture_state.events);
        if (!(bits & VOICE_BIT_FOLLOWUP) || !VOICE_NETWORK_READY(bits) || (bits & VOICE_END_BITS)) {
            return;
        }
        xStreamBufferReset(capture_state.microphone);
        size_t first = capture_state.vad_next + FOLLOWUP_HISTORY_FRAMES - capture_state.vad_count;
        for (size_t i = 0; i < capture_state.vad_count; i++) {
            buffer_microphone(capture_state.vad_history[(first + i) % FOLLOWUP_HISTORY_FRAMES], FOLLOWUP_SAMPLES);
        }
        buffer_microphone(mono, samples);
        // Continue the same VAD state from speech onset through the question.
        capture_state.endpoint_fill = 0;
        capture_state.endpoint_holdoff_frames = 0;
        capture_state.endpoint_heard_speech = true;
        xEventGroupClearBits(capture_state.events, VOICE_BIT_LOCAL_EOS);
        xEventGroupSetBits(capture_state.events, VOICE_BIT_SPEECH);
        return;
    }
}

static void detect_endpoint(const int16_t *mono, size_t samples)
{
    while (samples > 0) {
        size_t take = FOLLOWUP_SAMPLES - capture_state.endpoint_fill;
        if (take > samples) {
            take = samples;
        }
        memcpy(capture_buffers.endpoint_frame + capture_state.endpoint_fill, mono, take * sizeof(int16_t));
        capture_state.endpoint_fill += take;
        mono += take;
        samples -= take;
        if (capture_state.endpoint_fill == FOLLOWUP_SAMPLES) {
            capture_state.endpoint_fill = 0;
            if (capture_state.endpoint_holdoff_frames > 0) {
                capture_state.endpoint_holdoff_frames--;
            }
#ifdef ESP_PLATFORM
            // Ignore the wake word's own acoustic tail during the first 300 ms after wake
            // so silence while the e-paper refreshes "正在聆听" cannot end the turn early.
            if (capture_state.endpoint_holdoff_frames > (VOICE_WAKE_MIN_CAPTURE_MS - 300) / FOLLOWUP_FRAME_MS) {
                vad_reset_trigger(capture_state.vad);
                continue;
            }
#endif
            if (vad_process_with_trigger(capture_state.vad, capture_buffers.endpoint_frame) == VAD_SPEECH) {
                capture_state.endpoint_heard_speech = true;
            } else if (capture_state.endpoint_heard_speech && capture_state.endpoint_holdoff_frames == 0) {
                // The last microphone chunk is already in the buffer.
                xEventGroupSetBits(capture_state.events, VOICE_BIT_LOCAL_EOS);
                xEventGroupClearBits(capture_state.events, VOICE_BIT_CAPTURE);
                return;
            }
        }
    }
}

// Capture owns speech detection. Only the session task can start network capture.
static void route_microphone(const int16_t *mono, size_t samples)
{
    EventBits_t bits = xEventGroupGetBits(capture_state.events);
    bool followup = (bits & VOICE_BIT_FOLLOWUP) != 0;
    if (followup != capture_state.in_followup) {
        if (followup && capture_state.vad != NULL) {
            vad_reset_trigger(capture_state.vad);
            capture_state.vad_next = capture_state.vad_count = capture_state.vad_fill = 0;
        }
        capture_state.in_followup = followup;
    }
    if ((bits & VOICE_BIT_CAPTURE) || ((bits & VOICE_BIT_FOLLOWUP) && (bits & VOICE_BIT_SPEECH))) {
        if (!(bits & VOICE_BIT_LOCAL_EOS)) {
            buffer_microphone(mono, samples);
            if (capture_state.vad != NULL) {
                detect_endpoint(mono, samples);
            }
        }
        capture_state.wake_fill = 0;
        return;
    }
    if ((bits & VOICE_BIT_FOLLOWUP) && !(bits & VOICE_BIT_LOCAL_EOS) && capture_state.vad_history != NULL) {
        detect_followup(mono, samples);
        capture_state.wake_fill = 0;
        return;
    }
    if (!(bits & VOICE_BIT_ARMED)) {
        capture_state.wake_fill = 0;
        return;
    }
    while (samples > 0) {
        size_t take = (size_t)capture_state.wake_samples - (size_t)capture_state.wake_fill;
        if (take > samples) {
            take = samples;
        }
        memcpy(&capture_buffers.wake_chunk[capture_state.wake_fill], mono, take * sizeof(int16_t));
        capture_state.wake_fill += (int)take;
        mono += take;
        samples -= take;
        if (capture_state.wake_fill == capture_state.wake_samples) {
            capture_state.wake_fill = 0;
            if (capture_state.wakenet->detect(capture_state.wake_model, capture_buffers.wake_chunk) == WAKENET_DETECTED)
            {
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
        size_t room = (size_t)capture_state.feed_samples - (size_t)capture_state.afe_fill;
        size_t take = samples - index < room ? samples - index : room;
        size_t got = 0;
        while (got < take) {
            size_t bytes = xStreamBufferReceive(capture_state.reference, capture_buffers.reference_scratch + got,
                                                (take - got) * sizeof(int16_t), 0);
            if (bytes < sizeof(int16_t)) {
                break;
            }
            got += bytes / sizeof(int16_t);
        }
        for (size_t i = 0; i < take; i++) {
            capture_state.afe_in[(capture_state.afe_fill + i) * 2] = mono[index + i];
            capture_state.afe_in[(capture_state.afe_fill + i) * 2 + 1] =
                i < got ? capture_buffers.reference_scratch[i] : 0;
        }
        capture_state.afe_fill += (int)take;
        index += take;
        if (capture_state.afe_fill != capture_state.feed_samples) {
            continue;
        }
        capture_state.afe->feed(capture_state.afe_data, capture_state.afe_in);
        capture_state.afe_fill = 0;
        for (;;) {
            afe_fetch_result_t *result = capture_state.afe->fetch_with_delay(capture_state.afe_data, 0);
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
    const size_t stereo_bytes = capture_state.wake_samples * 2 * sizeof(int16_t);
    for (;;) {
        xEventGroupWaitBits(capture_state.events, VOICE_BIT_ARMED | VOICE_BIT_CAPTURE | VOICE_BIT_FOLLOWUP, pdFALSE,
                            pdFALSE, portMAX_DELAY);
        if (!VOICE_NETWORK_READY(xEventGroupGetBits(capture_state.events))) {
            vTaskDelay(pdMS_TO_TICKS(VOICE_POLL_MS));
            continue;
        }
        size_t read = 0;
        esp_err_t err = audio_read(capture_buffers.stereo, stereo_bytes, &read);
        EventBits_t bits = xEventGroupGetBits(capture_state.events);
        if (!VOICE_NETWORK_READY(bits)) {
            capture_state.wake_fill = 0;
            continue;
        }
        if (err != ESP_OK || read != stereo_bytes) {
            if (bits & VOICE_BIT_CAPTURE) {
                xEventGroupSetBits(capture_state.events, VOICE_BIT_ERROR);
            }
            capture_state.wake_fill = 0;
            vTaskDelay(pdMS_TO_TICKS(VOICE_POLL_MS));
            continue;
        }
        voice_stereo_to_mono(capture_buffers.stereo, capture_buffers.mono, capture_state.wake_samples);
        if (capture_state.afe != NULL) {
            afe_process(capture_buffers.mono, capture_state.wake_samples);
        } else {
            route_microphone(capture_buffers.mono, capture_state.wake_samples);
        }
    }
}

esp_err_t voice_capture_start(EventGroupHandle_t events, StreamBufferHandle_t microphone,
                              StreamBufferHandle_t reference, srmodel_list_t *models, const esp_wn_iface_t *wakenet,
                              model_iface_data_t *wake_model, int wake_samples)
{
    if (events == NULL || microphone == NULL || reference == NULL || wakenet == NULL || wake_model == NULL
        || wake_samples <= 0 || wake_samples > VOICE_FRAME_SAMPLES || capture_state.task != NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    capture_state.events = events;
    capture_state.microphone = microphone;
    capture_state.reference = reference;
    capture_state.wakenet = wakenet;
    capture_state.wake_model = wake_model;
    capture_state.wake_samples = wake_samples;
    // Without AEC the session keeps WakeNet disarmed during playback; that is not an error.
    esp_err_t err = init_afe(models);
    if (err != ESP_OK) {
        ESP_LOGW(log_tag, "AEC unavailable (%s); wake word disabled during playback", esp_err_to_name(err));
    }
    // The capture task owns this detector after startup; its absence leaves the hard timer intact.
    // VAD_MODE_1 rejects quiet ambient hum while catching normal speech; 900 ms tail silence prevents
    // cutting mid-sentence pauses while still closing turns after the user finishes asking.
    capture_state.vad = vad_create_with_param(VAD_MODE_1, VOICE_SAMPLE_RATE, FOLLOWUP_FRAME_MS, 60, 900);
    if (capture_state.vad == NULL) {
        ESP_LOGW(log_tag, "VAD unavailable; using the recording timeout and server endpoint");
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
    if (capture_state.vad != NULL) {
        vad_destroy(capture_state.vad);
    }
    heap_caps_free(capture_state.afe_in);
    heap_caps_free(capture_state.vad_history);
    capture_state = (struct voice_capture_state){0};
}

bool voice_capture_has_aec(void)
{
    return capture_state.afe != NULL;
}

bool voice_capture_prepare_vad(void)
{
    if (capture_state.vad == NULL) {
        return false;
    }
    if (capture_state.vad_history == NULL) {
        capture_state.vad_history = heap_caps_malloc(FOLLOWUP_HISTORY_FRAMES * sizeof(*capture_state.vad_history),
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return capture_state.vad_history != NULL;
}
