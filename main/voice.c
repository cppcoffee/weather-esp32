#include "voice.h"
#include "device_mcp.h"
#include "voice_settings.h"

#include <string.h>

#include "audio.h"
#include "esp_event.h"
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
#include "nvs.h"
#include "opus.h"
#include "voice_audio.h"
#include "voice_capture.h"
#include "voice_feedback.h"
#include "voice_internal.h"

static const char *TAG = "voice";
static const char *WAKE_WORD = "小鹿小鹿";

typedef struct {
    int len;
    uint8_t data[VOICE_RX_OPUS_MAX];
} voice_packet_t;

static EventGroupHandle_t s_events;
static SemaphoreHandle_t s_network_lock;
static QueueHandle_t s_rx_queue;
static srmodel_list_t *s_models;
static const esp_wn_iface_t *s_wakenet;
static model_iface_data_t *s_wake_model;
static int s_wake_samples;
static esp_xiaozhi_chat_handle_t s_chat;
static esp_mcp_t *s_mcp;
static bool s_cleanup_pending;
static bool s_followup_enabled;
static OpusEncoder *s_encoder;
static OpusDecoder *s_decoder;
static StreamBufferHandle_t s_reference;
static StreamBufferHandle_t s_microphone;

// Only voice_task touches Opus and PCM. Encoding and decoding therefore share one
// per-task Opus pseudostack, as well as these PCM buffers.
static int16_t s_mono[VOICE_FRAME_SAMPLES];
static int16_t s_stereo[VOICE_FRAME_SAMPLES * 2];
static uint8_t s_tx_opus[VOICE_TX_OPUS_MAX];
static voice_packet_t s_packet;

static bool network_ready(void)
{
    return VOICE_NETWORK_READY(xEventGroupGetBits(s_events));
}

static void on_audio(const uint8_t *data, int len, void *ctx)
{
    (void)ctx;
    EventBits_t bits = xEventGroupGetBits(s_events);
    if (!VOICE_NETWORK_READY(bits) || (bits & VOICE_END_BITS) || !(bits & VOICE_BIT_RECEIVING)) {
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
            // An abort may deliver the previous reply's stop after the new round opened.
            if (xEventGroupGetBits(s_events) & VOICE_BIT_TTS_STARTED) {
                xEventGroupSetBits(s_events, VOICE_BIT_TTS_STOPPED);
            }
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

// The session task owns teardown. Callbacks only wake its current wait.
static void on_chat_lifecycle(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id == ESP_XIAOZHI_CHAT_EVENT_SERVER_GOODBYE) {
        ESP_LOGI(TAG, "server goodbye");
        xEventGroupSetBits(s_events, VOICE_BIT_HANGUP);
    } else if (xEventGroupGetBits(s_events) & (VOICE_BIT_RECEIVING | VOICE_BIT_FOLLOWUP)) {
        xEventGroupSetBits(s_events, VOICE_BIT_WS_LOST);
    }
}

static void close_session(void)
{
    xEventGroupClearBits(s_events, VOICE_BIT_RECEIVING);
    if (s_chat != 0) {
        // A failed stop may leave callbacks live: retain all their resources and retry.
        esp_err_t err = esp_xiaozhi_chat_stop(s_chat);
        if (err != ESP_OK) {
            s_cleanup_pending = true;
            voice_feedback_publish(VOICE_STATE_ERROR, NULL);
            xEventGroupSetBits(s_events, VOICE_BIT_ERROR);
            audio_set_mute(true);
            ESP_LOGW(TAG, "session stop pending: %s", esp_err_to_name(err));
            return;
        }
        // After successful WS stop, deinit consumes the handle even on cleanup errors.
        err = esp_xiaozhi_chat_deinit(s_chat);
        s_chat = 0;
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "session cleanup: %s", esp_err_to_name(err));
        }
    }
    s_cleanup_pending = false;
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
    voice_feedback_publish(VOICE_STATE_CONNECTING, NULL);
    esp_xiaozhi_chat_config_t config = ESP_XIAOZHI_CHAT_DEFAULT_CONFIG();
    esp_xiaozhi_chat_info_t info = {0};
    voice_settings_t settings = {0};
    esp_err_t err = voice_settings_load(&settings);
    if (err != ESP_OK) {
        voice_feedback_publish(VOICE_STATE_NEED_SETUP, NULL);
        ESP_LOGW(TAG, "voice settings unavailable: %s", esp_err_to_name(err));
        return err;
    }
    if (settings.info_url[0] == '\0') {
        voice_feedback_publish(VOICE_STATE_NEED_SETUP, NULL);
        // 没有编译期默认地址：配网页未填写就不去连任何服务
        ESP_LOGW(TAG, "service info URL not configured; open the setup page and fill it in");
        return ESP_ERR_INVALID_STATE;
    }
    err = esp_xiaozhi_chat_get_info_with_url(settings.info_url, &info);
    if (err == ESP_OK && (info.has_activation_code || info.has_activation_challenge)) {
        // Copy the code before releasing the response and poll again after binding.
        if (!info.has_activation_code) {
            voice_feedback_publish(VOICE_STATE_ERROR, NULL);
            ESP_LOGW(TAG, "server requires a challenge without an activation code");
        } else if (voice_feedback_publish(VOICE_STATE_ACTIVATION, info.activation_code)) {
            ESP_LOGW(TAG, "activation code: %s %s", info.activation_code,
                     info.activation_message ? info.activation_message : "");
        } else {
            voice_feedback_publish(VOICE_STATE_NEED_SETUP, NULL);
            ESP_LOGW(TAG, "activation code cannot be displayed safely");
        }
        esp_xiaozhi_chat_free_info(&info);
        return ESP_ERR_INVALID_STATE;
    }
    if (err == ESP_OK) {
        // 只走 WebSocket，地址一律用信息接口下发的那条
        config.has_mqtt_config = false;
        config.has_websocket_config = info.has_websocket_config;
    }
    esp_xiaozhi_chat_free_info(&info);
    if (err != ESP_OK || !config.has_websocket_config) {
        voice_feedback_publish(VOICE_STATE_ERROR, NULL);
        ESP_LOGW(TAG, "server config unavailable: %s; retry after network recovery", esp_err_to_name(err));
        return err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
    }

    err = esp_mcp_create(&s_mcp);
    if (err != ESP_OK) {
        goto failed;
    }
    err = device_mcp_register(s_mcp);
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
    s_followup_enabled = settings.followup && voice_capture_prepare_vad();
    if (settings.followup && !s_followup_enabled) {
        ESP_LOGW(TAG, "follow-up VAD unavailable; waiting for wake word");
    }
    ESP_LOGI(TAG, "chat ready; internal heap free=%u, largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return ESP_OK;

failed:
    voice_feedback_publish(VOICE_STATE_ERROR, NULL);
    ESP_LOGW(TAG, "session setup failed: %s", esp_err_to_name(err));
    close_session();
    return err;
}

static bool turn_can_continue(void)
{
    EventBits_t bits = xEventGroupGetBits(s_events);
    return VOICE_NETWORK_READY(bits) && !(bits & VOICE_END_BITS);
}

static esp_err_t stream_microphone(void)
{
    TickType_t started = xTaskGetTickCount();
    size_t filled = 0;
    while (xTaskGetTickCount() - started < pdMS_TO_TICKS(VOICE_TURN_TIMEOUT_MS)) {
        EventBits_t bits = xEventGroupGetBits(s_events);
        if (!turn_can_continue()) {
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
        if (!turn_can_continue()) {
            return ESP_FAIL;
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

static esp_err_t play_response(bool *played)
{
    TickType_t last_audio = xTaskGetTickCount();
    bool received_audio = false;
    esp_err_t err = ESP_ERR_TIMEOUT;

    // The first packets can already be queued while recording winds down.
    xStreamBufferReset(s_reference);
    // Only the echo canceller makes it safe to keep WakeNet armed while the speaker plays;
    // without it the assistant's own voice would interrupt every reply.
    if (voice_capture_has_aec()) {
        xEventGroupSetBits(s_events, VOICE_BIT_ARMED);
    }
    // Tool discovery + LLM/tool rounds + first TTS have a bounded 75 s server budget.
    // Once playback starts, retain the shorter gap limit rather than extending every gap.
    while (xTaskGetTickCount() - last_audio
           < pdMS_TO_TICKS(received_audio ? VOICE_RESPONSE_TIMEOUT_MS : VOICE_FIRST_AUDIO_TIMEOUT_MS))
    {
        if (!turn_can_continue()) {
            err = ESP_FAIL;
            break;
        }
        if (xEventGroupGetBits(s_events) & VOICE_BIT_INTERRUPT) {
            // The next listening iteration drops queued packets before it accepts audio again.
            xEventGroupClearBits(s_events, VOICE_BIT_RECEIVING);
            err = ESP_OK;
            break;
        }
        if (xQueueReceive(s_rx_queue, &s_packet, pdMS_TO_TICKS(VOICE_PLAYBACK_POLL_MS)) == pdTRUE) {
            if (!turn_can_continue()) {
                err = ESP_FAIL;
                break;
            }
            int samples = opus_decode(s_decoder, s_packet.data, s_packet.len, s_mono, VOICE_FRAME_SAMPLES, 0);
            if (samples <= 0) {
                ESP_LOGW(TAG, "opus decode failed: %d", samples);
                err = ESP_FAIL;
                break;
            }
            voice_mono_to_stereo(s_mono, s_stereo, (size_t)samples);
            size_t written = 0;
            esp_err_t write_err = audio_set_mute(false);
            if (write_err == ESP_OK) {
                write_err = audio_write(s_stereo, (size_t)samples * 4, &written);
            }
            if (write_err != ESP_OK || written != (size_t)samples * 4) {
                err = ESP_FAIL;
                break;
            }
            xStreamBufferSend(s_reference, s_mono, (size_t)samples * sizeof(int16_t), 0);
            if (!received_audio) {
                ESP_LOGI(TAG, "first reply PCM after %lu ms",
                         (unsigned long)(xTaskGetTickCount() - last_audio) * portTICK_PERIOD_MS);
                voice_feedback_publish(VOICE_STATE_SPEAKING, NULL);
            }
            received_audio = true;
            last_audio = xTaskGetTickCount();
        } else if (xEventGroupGetBits(s_events) & VOICE_BIT_TTS_STOPPED) {
            // TTS stop can arrive before the worker drains the queued audio.
            err = ESP_OK;
            break;
        }
    }
    // Leave arming to the turn owner: it re-arms WakeNet once this turn is finished.
    xEventGroupClearBits(s_events, VOICE_BIT_ARMED);
    xStreamBufferReset(s_reference);
    *played = received_audio;
    return err;
}

// Returns with the network lock held again, including on timeout or disconnect.
static bool wait_followup(void)
{
    // A successful write can leave up to one DMA ring of speech queued.
    xEventGroupWaitBits(s_events, VOICE_END_BITS, pdFALSE, pdFALSE, pdMS_TO_TICKS(AUDIO_PLAYBACK_TAIL_MS) + 1);
    if (!turn_can_continue() || audio_set_mute(true) != ESP_OK) {
        return false;
    }
    xEventGroupClearBits(s_events, VOICE_BIT_SPEECH);
    xEventGroupSetBits(s_events, VOICE_BIT_FOLLOWUP);
    xEventGroupClearBits(s_events, VOICE_BIT_RECEIVING);
    voice_feedback_publish(VOICE_STATE_LISTENING, NULL);
    xSemaphoreGive(s_network_lock);

    EventBits_t bits = xEventGroupWaitBits(s_events, VOICE_BIT_SPEECH | VOICE_END_BITS, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(VOICE_FOLLOWUP_MS));
    bool speech = (bits & VOICE_BIT_SPEECH) && turn_can_continue();
    if (speech) {
        xEventGroupSetBits(s_events, VOICE_BIT_CAPTURE | VOICE_BIT_RECEIVING);
    }
    xEventGroupClearBits(s_events, VOICE_BIT_FOLLOWUP | VOICE_BIT_SPEECH);

    if (xSemaphoreTake(s_network_lock, pdMS_TO_TICKS(VOICE_POLL_MS)) != pdTRUE) {
        // A weather request already owns the budget. Drop the unstarted follow-up.
        xEventGroupClearBits(s_events, VOICE_BIT_CAPTURE);
        xSemaphoreTake(s_network_lock, portMAX_DELAY);
        return false;
    }
    return speech && turn_can_continue();
}

static esp_err_t run_turn(void)
{
    const esp_xiaozhi_chat_audio_t audio = {
        .format = "opus",
        .sample_rate = VOICE_SAMPLE_RATE,
        .channels = 1,
        .frame_duration = VOICE_FRAME_MS,
    };
    bool channel_open = false;
    bool interrupted = false;
    esp_err_t err = ESP_OK;

    while (turn_can_continue()) {
        voice_feedback_publish(VOICE_STATE_CONNECTING, NULL);
        xQueueReset(s_rx_queue);
        xEventGroupClearBits(s_events, VOICE_BIT_TTS_STARTED | VOICE_BIT_TTS_STOPPED | VOICE_BIT_INTERRUPT);
        opus_encoder_ctl(s_encoder, OPUS_RESET_STATE);
        opus_decoder_ctl(s_decoder, OPUS_RESET_STATE);
        if (!channel_open) {
            err = esp_xiaozhi_chat_open_audio_channel(s_chat, &audio, NULL, 0);
            if (err != ESP_OK) {
                break;
            }
            channel_open = true;
            xEventGroupSetBits(s_events, VOICE_BIT_RECEIVING);
            err = esp_xiaozhi_chat_send_wake_word(s_chat, WAKE_WORD);
            if (err != ESP_OK) {
                break;
            }
        } else if (interrupted) {
            audio_set_mute(true);
            err =
                esp_xiaozhi_chat_send_abort_speaking(s_chat, ESP_XIAOZHI_CHAT_ABORT_SPEAKING_REASON_WAKE_WORD_DETECTED);
            if (err != ESP_OK) {
                break;
            }
            // Accept downstream audio again only after the server was told to stop talking.
            xEventGroupSetBits(s_events, VOICE_BIT_RECEIVING);
            ESP_LOGI(TAG, "reply aborted, listening again");
        }
        err = esp_xiaozhi_chat_send_start_listening(s_chat, ESP_XIAOZHI_CHAT_LISTENING_MODE_AUTO);
        if (err != ESP_OK) {
            break;
        }
        ESP_LOGI(TAG, "listening");
        voice_feedback_publish(VOICE_STATE_LISTENING, NULL);
        err = stream_microphone();
        xEventGroupClearBits(s_events, VOICE_BIT_CAPTURE);
        if (err == ESP_OK && turn_can_continue()) {
            voice_feedback_publish(VOICE_STATE_WAITING, NULL);
            err = esp_xiaozhi_chat_send_stop_listening(s_chat);
        }
        bool played = false;
        if (err == ESP_OK && turn_can_continue()) {
            err = play_response(&played);
        }
        if (err != ESP_OK || !turn_can_continue()) {
            break;
        }
        interrupted = (xEventGroupGetBits(s_events) & VOICE_BIT_INTERRUPT) != 0;
        if (!interrupted && !(s_followup_enabled && played && wait_followup())) {
            break;
        }
    }
    EventBits_t bits = xEventGroupClearBits(s_events, VOICE_BIT_RECEIVING | VOICE_BIT_ARMED | VOICE_BIT_INTERRUPT
                                                          | VOICE_BIT_CAPTURE | VOICE_BIT_FOLLOWUP | VOICE_BIT_SPEECH);
    audio_set_mute(true);
    if (channel_open && VOICE_NETWORK_READY(bits) && !(bits & (VOICE_BIT_WS_LOST | VOICE_BIT_HANGUP))) {
        esp_xiaozhi_chat_close_audio_channel(s_chat);
    }
    xQueueReset(s_rx_queue);
    xStreamBufferReset(s_reference);
    return (bits & (VOICE_BIT_WS_LOST | VOICE_BIT_ERROR)) ? ESP_FAIL : err;
}

static void voice_task(void *arg)
{
    (void)arg;
    bool suspended = false; // 用户说过"退下"：不建会话，只跑本地唤醒
    for (;;) {
        EventBits_t bits = xEventGroupGetBits(s_events);
        EventBits_t needed = (bits & VOICE_BIT_ENABLED) ? VOICE_BIT_NET : VOICE_BIT_ENABLED;
        xEventGroupWaitBits(s_events, needed | VOICE_BIT_RESET, pdFALSE, pdFALSE, portMAX_DELAY);
        if (xSemaphoreTake(s_network_lock, pdMS_TO_TICKS(VOICE_POLL_MS)) != pdTRUE) {
            continue;
        }
        if (s_cleanup_pending) {
            close_session();
            if (!s_cleanup_pending && suspended && network_ready()) {
                xEventGroupSetBits(s_events, VOICE_BIT_ARMED);
                voice_feedback_publish(VOICE_STATE_READY, NULL);
            }
            xSemaphoreGive(s_network_lock);
            vTaskDelay(pdMS_TO_TICKS(VOICE_POLL_MS));
            continue;
        }
        bits = xEventGroupGetBits(s_events);
        if (bits & VOICE_BIT_RESET) {
            close_session();
            if (s_cleanup_pending) {
                xSemaphoreGive(s_network_lock);
                continue;
            }
            suspended = false;
            xEventGroupClearBits(s_events, VOICE_BIT_RESET | VOICE_BIT_ARMED | VOICE_BIT_CAPTURE | VOICE_BIT_TRIGGER
                                               | VOICE_BIT_INTERRUPT | VOICE_BIT_HANGUP | VOICE_BIT_FOLLOWUP
                                               | VOICE_BIT_SPEECH | VOICE_TURN_BITS);
            // An ordinary outage preserves a pending activation code; provisioning clears it.
            voice_feedback_publish((bits & VOICE_BIT_ENABLED) ? VOICE_STATE_ERROR : VOICE_STATE_WAIT_NETWORK, NULL);
            xSemaphoreGive(s_network_lock);
            continue;
        }
        if (!VOICE_NETWORK_READY(bits)) {
            xSemaphoreGive(s_network_lock);
            continue;
        }
        // 服务端说了 goodbye：释放 WS/MCP/编解码器，只留唤醒
        if (xEventGroupGetBits(s_events) & VOICE_BIT_HANGUP) {
            xEventGroupClearBits(s_events, VOICE_BIT_HANGUP);
            close_session();
            suspended = true;
            if (!s_cleanup_pending) {
                xEventGroupClearBits(s_events, VOICE_TURN_BITS | VOICE_BIT_CAPTURE | VOICE_BIT_TRIGGER);
                xEventGroupSetBits(s_events, VOICE_BIT_ARMED);
                voice_feedback_publish(VOICE_STATE_READY, NULL);
            }
            xSemaphoreGive(s_network_lock);
            ESP_LOGI(TAG, "session released, waiting for wake word");
            continue;
        }
        // 挂起中：等唤醒再重建会话
        if (suspended) {
            xSemaphoreGive(s_network_lock);
            bits = xEventGroupWaitBits(s_events, VOICE_BIT_TRIGGER | VOICE_BIT_RESET, pdFALSE, pdFALSE,
                                       pdMS_TO_TICKS(VOICE_POLL_MS));
            if (bits & VOICE_BIT_TRIGGER) {
                suspended = false;
            }
            continue;
        }
        esp_err_t err = ESP_OK;
        if (network_ready() && s_chat == 0) {
            err = open_session();
            if (err == ESP_OK && network_ready()) {
                xEventGroupClearBits(s_events, VOICE_TURN_BITS);
                xEventGroupSetBits(s_events, VOICE_BIT_ARMED);
                voice_feedback_publish(VOICE_STATE_READY, NULL);
            }
        }
        xSemaphoreGive(s_network_lock);
        if (err != ESP_OK) {
            // Failed stop retains callback-owned resources; pending cleanup runs before the next setup.
            xEventGroupWaitBits(s_events, VOICE_BIT_RESET, pdFALSE, pdFALSE, pdMS_TO_TICKS(VOICE_RETRY_MS));
            continue;
        }
        bits = xEventGroupWaitBits(s_events, VOICE_BIT_TRIGGER | VOICE_BIT_RESET | VOICE_BIT_HANGUP, pdFALSE, pdFALSE,
                                   pdMS_TO_TICKS(VOICE_POLL_MS));
        if (!(bits & VOICE_BIT_TRIGGER) || (bits & VOICE_BIT_HANGUP) || !network_ready()) {
            continue;
        }
        if (xSemaphoreTake(s_network_lock, pdMS_TO_TICKS(VOICE_POLL_MS)) != pdTRUE) {
            continue;
        }
        if (network_ready() && s_chat != 0) {
            xEventGroupClearBits(s_events, VOICE_BIT_TRIGGER);
            err = run_turn();
            xEventGroupClearBits(s_events, VOICE_BIT_CAPTURE);
            if (network_ready() && !(xEventGroupGetBits(s_events) & VOICE_BIT_HANGUP)) {
                if (err != ESP_OK) {
                    voice_feedback_publish(VOICE_STATE_ERROR, NULL);
                    ESP_LOGW(TAG, "turn failed: %s", esp_err_to_name(err));
                    close_session();
                } else {
                    xEventGroupClearBits(s_events, VOICE_TURN_BITS);
                    xEventGroupSetBits(s_events, VOICE_BIT_ARMED);
                    voice_feedback_publish(VOICE_STATE_READY, NULL);
                }
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
    const char *name = s_models ? esp_srmodel_filter(s_models, "wn9_xiaoluxiaolu_tts2", NULL) : NULL;
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
        || s_wakenet->get_samp_rate(s_wake_model) != VOICE_SAMPLE_RATE || s_wakenet->get_channel_num(s_wake_model) != 1)
    {
        err = ESP_ERR_NOT_SUPPORTED;
        goto failed;
    }
    err = ESP_ERR_NO_MEM;
    s_reference = xStreamBufferCreateWithCaps(VOICE_REF_BYTES, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_reference == NULL) {
        goto failed;
    }
    // Without AEC the previous behavior stands: no wake detection while a reply plays.
    err = ESP_ERR_NO_MEM;
    s_events = xEventGroupCreate();
    if (s_events != NULL) {
        ESP_ERROR_CHECK(esp_event_handler_register(ESP_XIAOZHI_CHAT_EVENTS, ESP_XIAOZHI_CHAT_EVENT_SERVER_GOODBYE,
                                                   on_chat_lifecycle, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(ESP_XIAOZHI_CHAT_EVENTS, ESP_XIAOZHI_CHAT_EVENT_DISCONNECTED,
                                                   on_chat_lifecycle, NULL));
    }
    s_network_lock = xSemaphoreCreateMutex();
    s_rx_queue = xQueueCreate(VOICE_RX_QUEUE_LEN, sizeof(voice_packet_t));
    s_microphone = xStreamBufferCreateWithCaps(WAKE_BUFFER_BYTES, sizeof(s_mono), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_events && s_network_lock && s_rx_queue && s_microphone) {
        err =
            voice_capture_start(s_events, s_microphone, s_reference, s_models, s_wakenet, s_wake_model, s_wake_samples);
        if (err == ESP_OK) {
            voice_feedback_publish(VOICE_STATE_WAIT_NETWORK, NULL);
            if (xTaskCreate(voice_task, "voice", VOICE_TASK_STACK, NULL, VOICE_TASK_PRIO, NULL) == pdPASS) {
                return ESP_OK;
            }
            err = ESP_ERR_NO_MEM;
        }
    }
failed:
    voice_feedback_publish(VOICE_STATE_DISABLED, NULL);
    voice_capture_stop();
    if (s_reference != NULL) {
        vStreamBufferDeleteWithCaps(s_reference);
        s_reference = NULL;
    }
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
        xEventGroupSetBits(s_events, VOICE_BIT_ENABLED);
    }
}

void voice_net_set_connected(bool connected)
{
    if (s_events == NULL) {
        return;
    }
    if (connected) {
        xEventGroupSetBits(s_events, VOICE_BIT_NET);
    } else {
        EventBits_t previous = xEventGroupClearBits(s_events, VOICE_BIT_NET);
        if (VOICE_NETWORK_READY(previous)) {
            xEventGroupSetBits(s_events, VOICE_BIT_RESET);
        }
    }
}

void voice_net_pause(void)
{
    if (s_events != NULL) {
        xEventGroupClearBits(s_events, VOICE_BIT_ENABLED | VOICE_BIT_NET | VOICE_BIT_TRIGGER | VOICE_BIT_RECEIVING
                                           | VOICE_BIT_ARMED | VOICE_BIT_CAPTURE | VOICE_BIT_INTERRUPT
                                           | VOICE_BIT_FOLLOWUP | VOICE_BIT_SPEECH);
        voice_feedback_publish(VOICE_STATE_WAIT_NETWORK, NULL);
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
