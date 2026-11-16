#include "voice.h"

#include <string.h>

#include "audio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mcp_engine.h"
#include "esp_wn_models.h"
#include "esp_xiaozhi_chat.h"
#include "esp_xiaozhi_info.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "model_path.h"
#include "opus.h"
#include "voice_audio.h"

static const char *TAG = "voice";
static const char *WAKE_WORD = "你好小智";

#define VOICE_SAMPLE_RATE         16000
#define VOICE_FRAME_MS            60
#define VOICE_FRAME_SAMPLES       (VOICE_SAMPLE_RATE / 1000 * VOICE_FRAME_MS)
#define VOICE_TX_OPUS_MAX         512
#define VOICE_RX_OPUS_MAX         1400
#define VOICE_RX_QUEUE_LEN        8
#define VOICE_TASK_STACK          8192
#define VOICE_TASK_PRIO           5
#define VOICE_TURN_TIMEOUT_MS     15000
#define VOICE_RESPONSE_TIMEOUT_MS 30000
#define VOICE_RETRY_MS            30000
#define VOICE_POLL_MS             100
#define WAKE_TASK_STACK           4096
#define WAKE_BUFFER_BYTES         (VOICE_SAMPLE_RATE * sizeof(int16_t) * 12)

#define VOICE_BIT_NET         (1 << 0)
#define VOICE_BIT_TRIGGER     (1 << 1)
#define VOICE_BIT_TTS_STARTED (1 << 2)
#define VOICE_BIT_TTS_STOPPED (1 << 3)
#define VOICE_BIT_ERROR       (1 << 4)
#define VOICE_BIT_RECEIVING   (1 << 5)
#define VOICE_BIT_RESET       (1 << 6)
#define VOICE_BIT_ARMED       (1 << 7)
#define VOICE_BIT_CAPTURE     (1 << 8)
#define VOICE_TURN_BITS       (VOICE_BIT_TTS_STARTED | VOICE_BIT_TTS_STOPPED | VOICE_BIT_ERROR)

typedef struct {
    int len;
    uint8_t data[VOICE_RX_OPUS_MAX];
} voice_packet_t;

static EventGroupHandle_t s_events;
static SemaphoreHandle_t s_network_lock;
static QueueHandle_t s_rx_queue;
static StreamBufferHandle_t s_microphone;
static srmodel_list_t *s_models;
static const esp_wn_iface_t *s_wakenet;
static model_iface_data_t *s_wake_model;
static int s_wake_samples;
static esp_xiaozhi_chat_handle_t s_chat;
static esp_mcp_t *s_mcp;
static OpusEncoder *s_encoder;
static OpusDecoder *s_decoder;

// Only voice_task touches Opus and PCM. Encoding and decoding therefore share one
// per-task Opus pseudostack, as well as these PCM buffers.
static int16_t s_mono[VOICE_FRAME_SAMPLES];
static int16_t s_stereo[VOICE_FRAME_SAMPLES * 2];
static uint8_t s_tx_opus[VOICE_TX_OPUS_MAX];
static voice_packet_t s_packet;
static int16_t s_capture_stereo[VOICE_FRAME_SAMPLES * 2];
static int16_t s_capture_mono[VOICE_FRAME_SAMPLES];

static bool network_ready(void)
{
    EventBits_t bits = xEventGroupGetBits(s_events);
    return (bits & (VOICE_BIT_NET | VOICE_BIT_RESET)) == VOICE_BIT_NET;
}

static void capture_task(void *arg)
{
    (void)arg;
    const size_t stereo_bytes = s_wake_samples * 2 * sizeof(int16_t);
    const size_t mono_bytes = s_wake_samples * sizeof(int16_t);
    for (;;) {
        xEventGroupWaitBits(s_events, VOICE_BIT_ARMED | VOICE_BIT_CAPTURE, pdFALSE, pdFALSE, portMAX_DELAY);
        if (!network_ready()) {
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
            vTaskDelay(pdMS_TO_TICKS(VOICE_POLL_MS));
            continue;
        }
        voice_stereo_to_mono(s_capture_stereo, s_capture_mono, s_wake_samples);
        if (bits & VOICE_BIT_CAPTURE) {
            // Preserve the start of the question while the chat audio channel opens.
            if (xStreamBufferSpacesAvailable(s_microphone) < mono_bytes) {
                xEventGroupClearBits(s_events, VOICE_BIT_CAPTURE);
                xEventGroupSetBits(s_events, VOICE_BIT_ERROR);
            } else {
                xStreamBufferSend(s_microphone, s_capture_mono, mono_bytes, 0);
            }
        } else if ((bits & VOICE_BIT_ARMED) && s_wakenet->detect(s_wake_model, s_capture_mono) == WAKENET_DETECTED) {
            s_wakenet->clean(s_wake_model);
            xEventGroupClearBits(s_events, VOICE_BIT_ARMED);
            xStreamBufferReset(s_microphone);
            xEventGroupSetBits(s_events, VOICE_BIT_CAPTURE | VOICE_BIT_TRIGGER);
            ESP_LOGI(TAG, "wake word detected");
        }
    }
}

static void on_audio(const uint8_t *data, int len, void *ctx)
{
    (void)ctx;
    if (!(xEventGroupGetBits(s_events) & VOICE_BIT_RECEIVING)) {
        return;
    }
    // The component owns data only until this callback returns. Never decode or
    // block on I2S here; queue a copy and fail the turn rather than growing memory.
    voice_packet_t packet;
    if (data == NULL || len <= 0 || len > (int)sizeof(packet.data)) {
        xEventGroupSetBits(s_events, VOICE_BIT_ERROR);
        return;
    }
    packet.len = len;
    memcpy(packet.data, data, (size_t)len);
    if (xQueueSend(s_rx_queue, &packet, 0) != pdTRUE) {
        xEventGroupSetBits(s_events, VOICE_BIT_ERROR);
    }
}

static void on_event(esp_xiaozhi_chat_event_t event, void *data, void *ctx)
{
    (void)ctx;
    switch (event) {
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_TTS_STATE: {
        const esp_xiaozhi_chat_tts_state_t *state = data;
        if (state == NULL) {
            break;
        }
        // Sticky until the next turn: a fast start/stop must still end recording.
        if (state->state == ESP_XIAOZHI_CHAT_TTS_STATE_START) {
            xEventGroupSetBits(s_events, VOICE_BIT_TTS_STARTED);
        } else if (state->state == ESP_XIAOZHI_CHAT_TTS_STATE_STOP) {
            xEventGroupSetBits(s_events, VOICE_BIT_TTS_STOPPED);
        } else if (state->text != NULL) {
            ESP_LOGI(TAG, "tts: %s", state->text);
        }
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_TEXT: {
        const esp_xiaozhi_chat_text_data_t *text = data;
        if (text != NULL && text->text != NULL) {
            ESP_LOGI(TAG, "%s: %s", text->role == ESP_XIAOZHI_CHAT_TEXT_ROLE_USER ? "user" : "bot", text->text);
        }
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_ERROR: {
        const esp_xiaozhi_chat_error_info_t *error = data;
        ESP_LOGW(TAG, "chat error %s: %s", error ? error->source : "?", error ? esp_err_to_name(error->code) : "?");
        xEventGroupSetBits(s_events, VOICE_BIT_ERROR);
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_SYSTEM_CMD:
        ESP_LOGW(TAG, "ignored system command: %s", data ? (const char *)data : "?");
        break;
    default:
        break;
    }
}

static void close_session(void)
{
    xEventGroupClearBits(s_events, VOICE_BIT_RECEIVING);
    if (s_chat != 0) {
        // Deinit stops transport callbacks before we release the caller-owned MCP.
        esp_xiaozhi_chat_deinit(s_chat);
        s_chat = 0;
    }
    if (s_mcp != NULL) {
        esp_mcp_destroy(s_mcp);
        s_mcp = NULL;
    }
    if (s_encoder != NULL) {
        opus_encoder_destroy(s_encoder);
        s_encoder = NULL;
    }
    if (s_decoder != NULL) {
        opus_decoder_destroy(s_decoder);
        s_decoder = NULL;
    }
    xQueueReset(s_rx_queue);
    audio_set_mute(true);
}

static esp_err_t open_session(void)
{
    esp_xiaozhi_chat_config_t config = ESP_XIAOZHI_CHAT_DEFAULT_CONFIG();
    esp_xiaozhi_chat_info_t info = {0};
    esp_err_t err = esp_xiaozhi_chat_get_info(&info);
    if (err == ESP_OK) {
        if (info.activation_code != NULL) {
            ESP_LOGW(TAG, "activation code: %s %s", info.activation_code,
                     info.activation_message ? info.activation_message : "");
        }
        config.has_mqtt_config = info.has_mqtt_config;
        config.has_websocket_config = info.has_websocket_config;
    }
    esp_xiaozhi_chat_free_info(&info);
    if (err != ESP_OK || (!config.has_mqtt_config && !config.has_websocket_config)) {
        ESP_LOGW(TAG, "server config unavailable: %s; retry after activation/network recovery", esp_err_to_name(err));
        return err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
    }

    // The installed esp_xiaozhi requires a real engine even with no custom tools.
    err = esp_mcp_create(&s_mcp);
    if (err != ESP_OK) {
        goto failed;
    }
    config.mcp_engine = s_mcp;
    config.owns_mcp_engine = false;
    config.audio_type = ESP_XIAOZHI_CHAT_AUDIO_TYPE_OPUS;
    config.audio_callback = on_audio;
    config.event_callback = on_event;
    err = esp_xiaozhi_chat_init(&config, &s_chat);
    if (err != ESP_OK) {
        goto failed;
    }
    err = esp_xiaozhi_chat_start(s_chat);
    if (err != ESP_OK) {
        goto failed;
    }

    int opus_error = OPUS_OK;
    s_encoder = opus_encoder_create(VOICE_SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &opus_error);
    if (s_encoder == NULL || opus_error != OPUS_OK) {
        err = ESP_ERR_NO_MEM;
        goto failed;
    }
    s_decoder = opus_decoder_create(VOICE_SAMPLE_RATE, 1, &opus_error);
    if (s_decoder == NULL || opus_error != OPUS_OK) {
        err = ESP_ERR_NO_MEM;
        goto failed;
    }
    if (opus_encoder_ctl(s_encoder, OPUS_SET_BITRATE(24000)) != OPUS_OK
        || opus_encoder_ctl(s_encoder, OPUS_SET_COMPLEXITY(3)) != OPUS_OK)
    {
        err = ESP_FAIL;
        goto failed;
    }
    ESP_LOGI(TAG, "chat ready; internal heap free=%u, largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return ESP_OK;

failed:
    ESP_LOGW(TAG, "session setup failed: %s", esp_err_to_name(err));
    close_session();
    return err;
}

static esp_err_t stream_microphone(void)
{
    TickType_t started = xTaskGetTickCount();
    size_t filled = 0;
    while (xTaskGetTickCount() - started < pdMS_TO_TICKS(VOICE_TURN_TIMEOUT_MS)) {
        EventBits_t bits = xEventGroupGetBits(s_events);
        if (!network_ready() || (bits & VOICE_BIT_ERROR)) {
            return ESP_FAIL;
        }
        if (bits & VOICE_BIT_TTS_STARTED) {
            return ESP_OK;
        }
        filled += xStreamBufferReceive(s_microphone, (uint8_t *)s_mono + filled, sizeof(s_mono) - filled,
                                       pdMS_TO_TICKS(VOICE_POLL_MS));
        if (filled < sizeof(s_mono)) {
            continue;
        }
        filled = 0;
        int encoded = opus_encode(s_encoder, s_mono, VOICE_FRAME_SAMPLES, s_tx_opus, sizeof(s_tx_opus));
        if (encoded <= 0) {
            ESP_LOGW(TAG, "opus encode failed: %d", encoded);
            return ESP_FAIL;
        }
        esp_err_t err = esp_xiaozhi_chat_send_audio_data(s_chat, (const char *)s_tx_opus, (size_t)encoded);
        if (err != ESP_OK) {
            return err;
        }
    }
    // End the upload at 15 s, then allow time for the server to answer.
    return ESP_OK;
}

static esp_err_t play_response(void)
{
    TickType_t last_audio = xTaskGetTickCount();
    while (xTaskGetTickCount() - last_audio < pdMS_TO_TICKS(VOICE_RESPONSE_TIMEOUT_MS)) {
        if (!network_ready() || (xEventGroupGetBits(s_events) & VOICE_BIT_ERROR)) {
            return ESP_FAIL;
        }
        if (xQueueReceive(s_rx_queue, &s_packet, pdMS_TO_TICKS(VOICE_POLL_MS)) == pdTRUE) {
            int samples = opus_decode(s_decoder, s_packet.data, s_packet.len, s_mono, VOICE_FRAME_SAMPLES, 0);
            if (samples <= 0) {
                ESP_LOGW(TAG, "opus decode failed: %d", samples);
                return ESP_FAIL;
            }
            voice_mono_to_stereo(s_mono, s_stereo, (size_t)samples);
            size_t written = 0;
            esp_err_t err = audio_set_mute(false);
            if (err == ESP_OK) {
                err = audio_write(s_stereo, (size_t)samples * 4, &written);
            }
            if (err != ESP_OK || written != (size_t)samples * 4) {
                return ESP_FAIL;
            }
            last_audio = xTaskGetTickCount();
        } else if (xEventGroupGetBits(s_events) & VOICE_BIT_TTS_STOPPED) {
            // TTS stop can arrive before the worker drains the queued audio.
            return ESP_OK;
        }
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t run_turn(void)
{
    xQueueReset(s_rx_queue);
    if (xEventGroupGetBits(s_events) & VOICE_BIT_ERROR) {
        return ESP_FAIL;
    }
    opus_encoder_ctl(s_encoder, OPUS_RESET_STATE);
    opus_decoder_ctl(s_decoder, OPUS_RESET_STATE);
    esp_xiaozhi_chat_audio_t audio = {
        .format = "opus",
        .sample_rate = VOICE_SAMPLE_RATE,
        .channels = 1,
        .frame_duration = VOICE_FRAME_MS,
    };
    esp_err_t err = esp_xiaozhi_chat_open_audio_channel(s_chat, &audio, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }
    xEventGroupSetBits(s_events, VOICE_BIT_RECEIVING);
    err = esp_xiaozhi_chat_send_wake_word(s_chat, WAKE_WORD);
    if (err == ESP_OK) {
        err = esp_xiaozhi_chat_send_start_listening(s_chat, ESP_XIAOZHI_CHAT_LISTENING_MODE_AUTO);
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "listening");
        err = stream_microphone();
        xEventGroupClearBits(s_events, VOICE_BIT_CAPTURE);
        esp_err_t stop_err = esp_xiaozhi_chat_send_stop_listening(s_chat);
        if (err == ESP_OK) {
            err = stop_err;
        }
        if (err == ESP_OK) {
            err = play_response();
        }
    }
    xEventGroupClearBits(s_events, VOICE_BIT_RECEIVING);
    esp_xiaozhi_chat_close_audio_channel(s_chat);
    xQueueReset(s_rx_queue);
    audio_set_mute(true);
    return err;
}

static void voice_task(void *arg)
{
    (void)arg;
    for (;;) {
        EventBits_t bits =
            xEventGroupWaitBits(s_events, VOICE_BIT_NET | VOICE_BIT_RESET, pdFALSE, pdFALSE, portMAX_DELAY);
        if (xSemaphoreTake(s_network_lock, pdMS_TO_TICKS(VOICE_POLL_MS)) != pdTRUE) {
            continue;
        }
        if (bits & VOICE_BIT_RESET) {
            close_session();
            xEventGroupClearBits(s_events, VOICE_BIT_RESET | VOICE_BIT_ARMED | VOICE_BIT_CAPTURE | VOICE_BIT_TRIGGER);
            xSemaphoreGive(s_network_lock);
            continue;
        }
        esp_err_t err = ESP_OK;
        if (network_ready() && s_chat == 0) {
            err = open_session();
            if (err == ESP_OK && network_ready()) {
                xEventGroupClearBits(s_events, VOICE_TURN_BITS);
                xEventGroupSetBits(s_events, VOICE_BIT_ARMED);
            }
        }
        xSemaphoreGive(s_network_lock);
        if (err != ESP_OK) {
            // No stale handles or partial codec state survive a failed attempt.
            xEventGroupWaitBits(s_events, VOICE_BIT_RESET, pdFALSE, pdFALSE, pdMS_TO_TICKS(VOICE_RETRY_MS));
            continue;
        }
        bits = xEventGroupWaitBits(s_events, VOICE_BIT_TRIGGER | VOICE_BIT_RESET, pdFALSE, pdFALSE,
                                   pdMS_TO_TICKS(VOICE_POLL_MS));
        if (!(bits & VOICE_BIT_TRIGGER) || !network_ready()) {
            continue;
        }
        if (xSemaphoreTake(s_network_lock, pdMS_TO_TICKS(VOICE_POLL_MS)) != pdTRUE) {
            continue;
        }
        if (network_ready() && s_chat != 0) {
            xEventGroupClearBits(s_events, VOICE_BIT_TRIGGER);
            err = run_turn();
            xEventGroupClearBits(s_events, VOICE_BIT_CAPTURE);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "turn failed: %s", esp_err_to_name(err));
                close_session();
            } else if (network_ready()) {
                xEventGroupClearBits(s_events, VOICE_TURN_BITS);
                xEventGroupSetBits(s_events, VOICE_BIT_ARMED);
            }
        }
        xSemaphoreGive(s_network_lock);
    }
}

esp_err_t voice_init(void)
{
    if (s_events != NULL) {
        return ESP_OK;
    }
    if (!audio_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0) {
        ESP_LOGW(TAG, "WakeNet requires PSRAM");
        return ESP_ERR_NOT_SUPPORTED;
    }
    esp_err_t err = ESP_ERR_NOT_FOUND;
    s_models = esp_srmodel_init("model");
    const char *name = s_models ? esp_srmodel_filter(s_models, "wn9_nihaoxiaozhi_tts", NULL) : NULL;
    s_wakenet = name ? esp_wn_handle_from_name(name) : NULL;
    if (s_wakenet == NULL) {
        goto failed;
    }
    err = ESP_ERR_NO_MEM;
    s_wake_model = s_wakenet->create(name, DET_MODE_90);
    if (s_wake_model == NULL) {
        goto failed;
    }
    s_wake_samples = s_wakenet->get_samp_chunksize(s_wake_model);
    if (s_wake_samples <= 0 || s_wake_samples > VOICE_FRAME_SAMPLES
        || s_wakenet->get_samp_rate(s_wake_model) != VOICE_SAMPLE_RATE
        || s_wakenet->get_channel_num(s_wake_model) != 1) {
        err = ESP_ERR_NOT_SUPPORTED;
        goto failed;
    }
    err = ESP_ERR_NO_MEM;
    s_events = xEventGroupCreate();
    s_network_lock = xSemaphoreCreateMutex();
    s_rx_queue = xQueueCreate(VOICE_RX_QUEUE_LEN, sizeof(voice_packet_t));
    s_microphone = xStreamBufferCreateWithCaps(WAKE_BUFFER_BYTES, sizeof(s_mono), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_events && s_network_lock && s_rx_queue && s_microphone) {
        TaskHandle_t capture = NULL;
        if (xTaskCreate(capture_task, "wake", WAKE_TASK_STACK, NULL, VOICE_TASK_PRIO, &capture) == pdPASS) {
            if (xTaskCreate(voice_task, "voice", VOICE_TASK_STACK, NULL, VOICE_TASK_PRIO, NULL) == pdPASS) {
                return ESP_OK;
            }
            vTaskDelete(capture);
        }
    }
failed:
    if (s_microphone != NULL) {
        vStreamBufferDeleteWithCaps(s_microphone);
        s_microphone = NULL;
    }
    if (s_rx_queue != NULL) {
        vQueueDelete(s_rx_queue);
        s_rx_queue = NULL;
    }
    if (s_network_lock != NULL) {
        vSemaphoreDelete(s_network_lock);
        s_network_lock = NULL;
    }
    if (s_events != NULL) {
        vEventGroupDelete(s_events);
        s_events = NULL;
    }
    if (s_wake_model != NULL) {
        s_wakenet->destroy(s_wake_model);
        s_wake_model = NULL;
    }
    if (s_models != NULL) {
        esp_srmodel_deinit(s_models);
        s_models = NULL;
    }
    return err;
}

void voice_net_ready(void)
{
    if (s_events != NULL) {
        xEventGroupSetBits(s_events, VOICE_BIT_NET);
    }
}

void voice_net_pause(void)
{
    if (s_events != NULL) {
        xEventGroupClearBits(s_events, VOICE_BIT_NET | VOICE_BIT_TRIGGER | VOICE_BIT_RECEIVING | VOICE_BIT_ARMED
                            | VOICE_BIT_CAPTURE);
        xEventGroupSetBits(s_events, VOICE_BIT_RESET);
    }
}

bool voice_weather_begin(void)
{
    return s_network_lock == NULL || xSemaphoreTake(s_network_lock, 0) == pdTRUE;
}

void voice_weather_end(void)
{
    if (s_network_lock != NULL) {
        xSemaphoreGive(s_network_lock);
    }
}
