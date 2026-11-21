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
#include "net.h"
#include "nvs.h"
#include "opus.h"
#include "voice_audio.h"
#include "voice_capture.h"
#include "voice_feedback.h"
#include "voice_internal.h"

static const char *log_tag = "voice";
static const char *wake_word = "小鹿小鹿";

struct voice_packet {
    int len;
    uint8_t data[VOICE_RX_OPUS_MAX];
};

static EventGroupHandle_t voice_events;
static SemaphoreHandle_t network_lock;
static QueueHandle_t rx_audio_queue;
static StreamBufferHandle_t playback_ref_stream;
static StreamBufferHandle_t microphone_stream;

static struct {
    srmodel_list_t *models;
    const esp_wn_iface_t *wakenet;
    model_iface_data_t *data;
    int samples;
} wake_model;

static struct {
    esp_xiaozhi_chat_handle_t chat;
    esp_mcp_t *mcp;
    OpusEncoder *encoder;
    OpusDecoder *decoder;
    bool cleanup_pending;
    bool followup_enabled;
} session;

// Only voice_task touches Opus and PCM. Encoding and decoding therefore share one
// per-task Opus pseudostack, as well as these PCM buffers.
static struct {
    int16_t mono[VOICE_FRAME_SAMPLES];
    int16_t stereo[VOICE_FRAME_SAMPLES * 2];
    uint8_t tx_opus[VOICE_TX_OPUS_MAX];
    struct voice_packet packet;
} voice_buffers;

// Only websocket_task's on_audio callback writes rx_ingress_packet, keeping the 1.4 KB
// copy off websocket_task's stack.
static struct voice_packet rx_ingress_packet;

static bool network_ready(void)
{
    return VOICE_NETWORK_READY(xEventGroupGetBits(voice_events));
}

static void on_audio(const uint8_t *data, int len, void *ctx)
{
    (void)ctx;
    EventBits_t bits = xEventGroupGetBits(voice_events);
    if (!VOICE_NETWORK_READY(bits) || (bits & (VOICE_END_BITS | VOICE_BIT_ABORT_DRAIN))
        || !(bits & VOICE_BIT_RECEIVING))
    {
        return;
    }
    // The component owns data only until this callback returns. Never decode or
    // block on I2S here; queue a copy and drop on burst instead of tearing down the session.
    if (data == NULL || len <= 0 || len > (int)sizeof(rx_ingress_packet.data)) {
        xEventGroupSetBits(voice_events, VOICE_BIT_ERROR);
        return;
    }
    rx_ingress_packet.len = len;
    memcpy(rx_ingress_packet.data, data, (size_t)len);
    if (xQueueSend(rx_audio_queue, &rx_ingress_packet, 0) != pdTRUE) {
        ESP_LOGW(log_tag, "rx audio queue full (%d frames), dropping packet", VOICE_RX_QUEUE_LEN);
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
            xEventGroupClearBits(voice_events, VOICE_BIT_ABORT_DRAIN);
            xEventGroupSetBits(voice_events, VOICE_BIT_TTS_STARTED);
        } else if (state->state == ESP_XIAOZHI_CHAT_TTS_STATE_STOP) {
            xEventGroupClearBits(voice_events, VOICE_BIT_ABORT_DRAIN);
            // An abort may deliver the previous reply's stop after the new round opened.
            if (xEventGroupGetBits(voice_events) & VOICE_BIT_TTS_STARTED) {
                xEventGroupSetBits(voice_events, VOICE_BIT_TTS_STOPPED);
            }
        } else if (state->text != NULL) {
            ESP_LOGI(log_tag, "tts: %s", state->text);
        }
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_TEXT: {
        const esp_xiaozhi_chat_text_data_t *text = data;
        if (text != NULL && text->text != NULL) {
            ESP_LOGI(log_tag, "%s: %s", text->role == ESP_XIAOZHI_CHAT_TEXT_ROLE_USER ? "user" : "bot", text->text);
        }
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_ERROR: {
        const esp_xiaozhi_chat_error_info_t *error = data;
        ESP_LOGW(log_tag, "chat error %s: %s", error ? error->source : "?", error ? esp_err_to_name(error->code) : "?");
        xEventGroupSetBits(voice_events, VOICE_BIT_ERROR);
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_SYSTEM_CMD:
        ESP_LOGW(log_tag, "ignored system command: %s", data ? (const char *)data : "?");
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
    if (id == ESP_XIAOZHI_CHAT_EVENT_CONNECTED) {
        xEventGroupSetBits(voice_events, VOICE_BIT_WS_READY);
        return;
    }
    if (id == ESP_XIAOZHI_CHAT_EVENT_DISCONNECTED) {
        xEventGroupClearBits(voice_events, VOICE_BIT_WS_READY);
    }
    if (id == ESP_XIAOZHI_CHAT_EVENT_SERVER_GOODBYE) {
        ESP_LOGI(log_tag, "server goodbye");
        xEventGroupSetBits(voice_events, VOICE_BIT_HANGUP);
    } else if (xEventGroupGetBits(voice_events) & (VOICE_BIT_RECEIVING | VOICE_BIT_FOLLOWUP)) {
        xEventGroupSetBits(voice_events, VOICE_BIT_WS_LOST);
    }
}

static void close_session(void)
{
    xEventGroupClearBits(voice_events, VOICE_BIT_RECEIVING);
    if (session.chat != 0) {
        // A failed stop may leave callbacks live: retain all their resources and retry.
        esp_err_t err = esp_xiaozhi_chat_stop(session.chat);
        if (err != ESP_OK) {
            session.cleanup_pending = true;
            voice_feedback_publish(VOICE_STATE_ERROR, NULL);
            xEventGroupSetBits(voice_events, VOICE_BIT_ERROR);
            audio_set_mute(true);
            ESP_LOGW(log_tag, "session stop pending: %s", esp_err_to_name(err));
            return;
        }
        // After successful WS stop, deinit consumes the handle even on cleanup errors.
        err = esp_xiaozhi_chat_deinit(session.chat);
        session.chat = 0;
        if (err != ESP_OK) {
            ESP_LOGW(log_tag, "session cleanup: %s", esp_err_to_name(err));
        }
    }
    session.cleanup_pending = false;
    if (session.mcp != NULL) {
        esp_mcp_destroy(session.mcp);
        session.mcp = NULL;
    }
    if (session.encoder != NULL) {
        opus_encoder_destroy(session.encoder);
        session.encoder = NULL;
    }
    if (session.decoder != NULL) {
        opus_decoder_destroy(session.decoder);
        session.decoder = NULL;
    }
    xQueueReset(rx_audio_queue);
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
        ESP_LOGW(log_tag, "voice settings unavailable: %s", esp_err_to_name(err));
        return err;
    }
    if (settings.info_url[0] == '\0') {
        voice_feedback_publish(VOICE_STATE_NEED_SETUP, NULL);
        // 没有编译期默认地址：配网页未填写就不去连任何服务
        ESP_LOGW(log_tag, "service info URL not configured; open the setup page and fill it in");
        return ESP_ERR_INVALID_STATE;
    }
    err = esp_xiaozhi_chat_get_info_with_url(settings.info_url, &info);
    if (err == ESP_OK && (info.has_activation_code || info.has_activation_challenge)) {
        // Copy the code before releasing the response and poll again after binding.
        if (!info.has_activation_code) {
            voice_feedback_publish(VOICE_STATE_ERROR, NULL);
            ESP_LOGW(log_tag, "server requires a challenge without an activation code");
        } else if (voice_feedback_publish(VOICE_STATE_ACTIVATION, info.activation_code)) {
            ESP_LOGW(log_tag, "activation code: %s %s", info.activation_code,
                     info.activation_message ? info.activation_message : "");
        } else {
            voice_feedback_publish(VOICE_STATE_NEED_SETUP, NULL);
            ESP_LOGW(log_tag, "activation code cannot be displayed safely");
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
        ESP_LOGW(log_tag, "server config unavailable from %s: %s; retry after network recovery", settings.info_url,
                 esp_err_to_name(err));
        return err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
    }

    err = esp_mcp_create(&session.mcp);
    if (err != ESP_OK) {
        goto failed;
    }
    err = device_mcp_register(session.mcp);
    if (err != ESP_OK) {
        goto failed;
    }
    config.mcp_engine = session.mcp;
    config.owns_mcp_engine = false;
    config.audio_type = ESP_XIAOZHI_CHAT_AUDIO_TYPE_OPUS;
    config.audio_callback = on_audio;
    config.event_callback = on_event;
    err = esp_xiaozhi_chat_init(&config, &session.chat);
    if (err != ESP_OK) {
        goto failed;
    }
    err = esp_xiaozhi_chat_start(session.chat);
    if (err != ESP_OK) {
        goto failed;
    }
    // `chat ready` must mean the socket is up, otherwise the first wake word is dropped.
    if (!(xEventGroupWaitBits(voice_events, VOICE_BIT_WS_READY, pdFALSE, pdFALSE,
                              pdMS_TO_TICKS(VOICE_CONNECT_TIMEOUT_MS))
          & VOICE_BIT_WS_READY))
    {
        ESP_LOGW(log_tag, "websocket not connected after %d ms", VOICE_CONNECT_TIMEOUT_MS);
        err = ESP_ERR_TIMEOUT;
        goto failed;
    }

    int opus_error = OPUS_OK;
    session.encoder = opus_encoder_create(VOICE_SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &opus_error);
    if (session.encoder == NULL || opus_error != OPUS_OK) {
        err = ESP_ERR_NO_MEM;
        goto failed;
    }
    session.decoder = opus_decoder_create(VOICE_SAMPLE_RATE, 1, &opus_error);
    if (session.decoder == NULL || opus_error != OPUS_OK) {
        err = ESP_ERR_NO_MEM;
        goto failed;
    }
    if (opus_encoder_ctl(session.encoder, OPUS_SET_BITRATE(24000)) != OPUS_OK
        || opus_encoder_ctl(session.encoder, OPUS_SET_COMPLEXITY(0)) != OPUS_OK)
    {
        err = ESP_FAIL;
        goto failed;
    }
    session.followup_enabled = voice_capture_prepare_vad();
    if (!session.followup_enabled) {
        ESP_LOGW(log_tag, "follow-up VAD unavailable; waiting for wake word");
    }
    ESP_LOGI(log_tag, "chat ready; internal heap free=%u, largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return ESP_OK;

failed:
    voice_feedback_publish(VOICE_STATE_ERROR, NULL);
    ESP_LOGW(log_tag, "session setup failed: %s", esp_err_to_name(err));
    close_session();
    return err;
}

static bool turn_can_continue(void)
{
    EventBits_t bits = xEventGroupGetBits(voice_events);
    return VOICE_NETWORK_READY(bits) && !(bits & VOICE_END_BITS);
}

static esp_err_t stream_microphone(void)
{
    TickType_t started = xTaskGetTickCount();
    size_t filled = 0;
    while (xTaskGetTickCount() - started < pdMS_TO_TICKS(VOICE_TURN_TIMEOUT_MS)) {
        EventBits_t bits = xEventGroupGetBits(voice_events);
        if (!turn_can_continue()) {
            return ESP_FAIL;
        }
        if (bits & VOICE_BIT_TTS_STARTED) {
            return ESP_OK;
        }
        bool ending = (bits & VOICE_BIT_LOCAL_EOS) != 0;
        filled += xStreamBufferReceive(microphone_stream, (uint8_t *)voice_buffers.mono + filled,
                                       sizeof(voice_buffers.mono) - filled,
                                       ending ? 0 : pdMS_TO_TICKS(VOICE_CAPTURE_POLL_MS));
        if (filled < sizeof(voice_buffers.mono)) {
            if (!ending) {
                continue;
            }
            if (!filled) {
                return ESP_OK;
            }
            memset((uint8_t *)voice_buffers.mono + filled, 0, sizeof(voice_buffers.mono) - filled);
        }
        if (!turn_can_continue()) {
            return ESP_FAIL;
        }
        filled = 0;
        int encoded = opus_encode(session.encoder, voice_buffers.mono, VOICE_FRAME_SAMPLES, voice_buffers.tx_opus,
                                  sizeof(voice_buffers.tx_opus));
        if (encoded <= 0) {
            ESP_LOGW(log_tag, "opus encode failed: %d", encoded);
            return ESP_FAIL;
        }
        esp_err_t err =
            esp_xiaozhi_chat_send_audio_data(session.chat, (const char *)voice_buffers.tx_opus, (size_t)encoded);
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
    xStreamBufferReset(playback_ref_stream);
    // Only the echo canceller makes it safe to keep WakeNet armed while the speaker plays;
    // without it the assistant's own voice would interrupt every reply.
    if (voice_capture_has_aec()) {
        xEventGroupSetBits(voice_events, VOICE_BIT_ARMED);
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
        if (xEventGroupGetBits(voice_events) & VOICE_BIT_INTERRUPT) {
            // The next listening iteration drops queued packets before it accepts audio again.
            xEventGroupClearBits(voice_events, VOICE_BIT_RECEIVING);
            err = ESP_OK;
            break;
        }
        if (xQueueReceive(rx_audio_queue, &voice_buffers.packet, pdMS_TO_TICKS(VOICE_PLAYBACK_POLL_MS)) == pdTRUE) {
            if (!turn_can_continue()) {
                err = ESP_FAIL;
                break;
            }
            int samples = opus_decode(session.decoder, voice_buffers.packet.data, voice_buffers.packet.len,
                                      voice_buffers.mono, VOICE_FRAME_SAMPLES, 0);
            if (samples <= 0) {
                ESP_LOGW(log_tag, "opus decode failed: %d", samples);
                err = ESP_FAIL;
                break;
            }
            voice_mono_to_stereo(voice_buffers.mono, voice_buffers.stereo, (size_t)samples);
            size_t written = 0;
            esp_err_t write_err = received_audio ? ESP_OK : audio_set_mute(false);
            if (write_err == ESP_OK) {
                write_err = audio_write(voice_buffers.stereo, (size_t)samples * 4, &written);
            }
            if (write_err != ESP_OK || written != (size_t)samples * 4) {
                err = ESP_FAIL;
                break;
            }
            xStreamBufferSend(playback_ref_stream, voice_buffers.mono, (size_t)samples * sizeof(int16_t), 0);
            if (!received_audio) {
                ESP_LOGI(log_tag, "first reply PCM after %lu ms",
                         (unsigned long)(xTaskGetTickCount() - last_audio) * portTICK_PERIOD_MS);
                voice_feedback_publish(VOICE_STATE_SPEAKING, NULL);
            }
            received_audio = true;
            last_audio = xTaskGetTickCount();
        } else if (xEventGroupGetBits(voice_events) & VOICE_BIT_TTS_STOPPED) {
            // TTS stop can arrive before the worker drains the queued audio.
            err = ESP_OK;
            break;
        }
    }
    *played = received_audio;
    return err;
}

// Returns with the network lock held again, including on timeout or disconnect.
static bool wait_followup(void)
{
    xEventGroupClearBits(voice_events, VOICE_BIT_SPEECH | VOICE_BIT_LOCAL_EOS);
    xEventGroupSetBits(voice_events, VOICE_BIT_FOLLOWUP);
    xEventGroupClearBits(voice_events, VOICE_BIT_RECEIVING);
    voice_feedback_publish(VOICE_STATE_FOLLOWUP, NULL);
    xSemaphoreGive(network_lock);

    EventBits_t bits = xEventGroupWaitBits(voice_events, VOICE_BIT_SPEECH | VOICE_END_BITS, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(VOICE_FOLLOWUP_MS));
    bool speech = (bits & VOICE_BIT_SPEECH) && turn_can_continue();
    if (speech) {
        xEventGroupSetBits(voice_events, VOICE_BIT_RECEIVING | ((bits & VOICE_BIT_LOCAL_EOS) ? 0 : VOICE_BIT_CAPTURE));
    }
    xEventGroupClearBits(voice_events, VOICE_BIT_FOLLOWUP | VOICE_BIT_SPEECH);

    TickType_t started = xTaskGetTickCount();
    do {
        if (xSemaphoreTake(network_lock, pdMS_TO_TICKS(VOICE_POLL_MS)) == pdTRUE) {
            return speech && turn_can_continue();
        }
    } while (speech && turn_can_continue() && xTaskGetTickCount() - started < pdMS_TO_TICKS(VOICE_FOLLOWUP_LOCK_MS));
    // A stalled fetch must not keep recording or leave the turn holding stale audio.
    if (speech) {
        xEventGroupClearBits(voice_events, VOICE_BIT_CAPTURE);
    }
    xSemaphoreTake(network_lock, portMAX_DELAY);
    return false;
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

    net_set_wifi_low_latency(true);
    while (turn_can_continue()) {
        xQueueReset(rx_audio_queue);
        xEventGroupClearBits(voice_events, VOICE_BIT_TTS_STARTED | VOICE_BIT_TTS_STOPPED | VOICE_BIT_INTERRUPT
                                               | VOICE_BIT_ABORT_DRAIN);
        opus_encoder_ctl(session.encoder, OPUS_RESET_STATE);
        opus_decoder_ctl(session.decoder, OPUS_RESET_STATE);
        if (!channel_open) {
            err = esp_xiaozhi_chat_open_audio_channel(session.chat, &audio, NULL, 0);
            if (err != ESP_OK) {
                break;
            }
            channel_open = true;
            xEventGroupSetBits(voice_events, VOICE_BIT_RECEIVING);
            err = esp_xiaozhi_chat_send_wake_word(session.chat, wake_word);
            if (err != ESP_OK) {
                break;
            }
        } else if (interrupted) {
            audio_set_mute(true);
            xEventGroupSetBits(voice_events, VOICE_BIT_ABORT_DRAIN);
            err = esp_xiaozhi_chat_send_abort_speaking(session.chat,
                                                       ESP_XIAOZHI_CHAT_ABORT_SPEAKING_REASON_WAKE_WORD_DETECTED);
            if (err != ESP_OK) {
                break;
            }
            xQueueReset(rx_audio_queue);
            // Accept downstream audio again once the abort drain finishes on stop/start.
            xEventGroupSetBits(voice_events, VOICE_BIT_RECEIVING);
            ESP_LOGI(log_tag, "reply aborted, listening again");
        }
        err = esp_xiaozhi_chat_send_start_listening(session.chat, ESP_XIAOZHI_CHAT_LISTENING_MODE_AUTO);
        if (err != ESP_OK) {
            break;
        }
        ESP_LOGI(log_tag, "listening");
        voice_feedback_publish(VOICE_STATE_LISTENING, NULL);
        err = stream_microphone();
        xEventGroupClearBits(voice_events, VOICE_BIT_CAPTURE);
        if (err == ESP_OK && turn_can_continue()) {
            voice_feedback_publish(VOICE_STATE_WAITING, NULL);
            err = esp_xiaozhi_chat_send_stop_listening(session.chat);
        }
        bool played = false;
        if (err == ESP_OK && turn_can_continue()) {
            err = play_response(&played);
        }
        if (err != ESP_OK || !turn_can_continue()) {
            break;
        }
        if (played && !(xEventGroupGetBits(voice_events) & VOICE_BIT_INTERRUPT)) {
            // I2S write completes before all samples from its DMA ring reach the speaker.
            xEventGroupWaitBits(voice_events, VOICE_END_BITS | VOICE_BIT_INTERRUPT, pdFALSE, pdFALSE,
                                pdMS_TO_TICKS(AUDIO_PLAYBACK_TAIL_MS) + 1);
            err = audio_set_mute(true);
            if (err != ESP_OK || !turn_can_continue()) {
                break;
            }
        }
        xEventGroupClearBits(voice_events, VOICE_BIT_ARMED);
        xStreamBufferReset(playback_ref_stream);
        interrupted = (xEventGroupGetBits(voice_events) & VOICE_BIT_INTERRUPT) != 0;
        if (!interrupted && !(session.followup_enabled && played && wait_followup())) {
            break;
        }
    }
    net_set_wifi_low_latency(false);
    EventBits_t bits = xEventGroupClearBits(
        voice_events, VOICE_BIT_RECEIVING | VOICE_BIT_ARMED | VOICE_BIT_INTERRUPT | VOICE_BIT_CAPTURE
                          | VOICE_BIT_FOLLOWUP | VOICE_BIT_SPEECH | VOICE_BIT_LOCAL_EOS | VOICE_BIT_ABORT_DRAIN);
    audio_set_mute(true);
    if (channel_open && VOICE_NETWORK_READY(bits) && !(bits & (VOICE_BIT_WS_LOST | VOICE_BIT_HANGUP))) {
        esp_xiaozhi_chat_close_audio_channel(session.chat);
    }
    xQueueReset(rx_audio_queue);
    xStreamBufferReset(playback_ref_stream);
    return (bits & (VOICE_BIT_WS_LOST | VOICE_BIT_ERROR)) ? ESP_FAIL : err;
}

static void voice_task(void *arg)
{
    (void)arg;
    bool suspended = false; // 用户说过"退下"：不建会话，只跑本地唤醒
    for (;;) {
        EventBits_t bits = xEventGroupGetBits(voice_events);
        EventBits_t needed = (bits & VOICE_BIT_ENABLED) ? VOICE_BIT_NET : VOICE_BIT_ENABLED;
        xEventGroupWaitBits(voice_events, needed | VOICE_BIT_RESET, pdFALSE, pdFALSE, portMAX_DELAY);
        if (xSemaphoreTake(network_lock, pdMS_TO_TICKS(VOICE_POLL_MS)) != pdTRUE) {
            continue;
        }
        if (session.cleanup_pending) {
            close_session();
            if (!session.cleanup_pending && suspended && network_ready()) {
                xEventGroupSetBits(voice_events, VOICE_BIT_ARMED);
                voice_feedback_publish(VOICE_STATE_READY, NULL);
            }
            xSemaphoreGive(network_lock);
            vTaskDelay(pdMS_TO_TICKS(VOICE_POLL_MS));
            continue;
        }
        bits = xEventGroupGetBits(voice_events);
        if (bits & VOICE_BIT_RESET) {
            close_session();
            if (session.cleanup_pending) {
                xSemaphoreGive(network_lock);
                continue;
            }
            suspended = false;
            xEventGroupClearBits(voice_events, VOICE_BIT_RESET | VOICE_BIT_ARMED | VOICE_BIT_CAPTURE | VOICE_BIT_TRIGGER
                                                   | VOICE_BIT_INTERRUPT | VOICE_BIT_HANGUP | VOICE_BIT_FOLLOWUP
                                                   | VOICE_BIT_SPEECH | VOICE_TURN_BITS);
            // An ordinary outage preserves a pending activation code; provisioning clears it.
            voice_feedback_publish((bits & VOICE_BIT_ENABLED) ? VOICE_STATE_OFFLINE : VOICE_STATE_WAIT_NETWORK, NULL);
            xSemaphoreGive(network_lock);
            continue;
        }
        if (!VOICE_NETWORK_READY(bits)) {
            xSemaphoreGive(network_lock);
            continue;
        }
        // 服务端说了 goodbye：释放 WS/MCP/编解码器，只留唤醒
        if (xEventGroupGetBits(voice_events) & VOICE_BIT_HANGUP) {
            xEventGroupClearBits(voice_events, VOICE_BIT_HANGUP);
            close_session();
            suspended = true;
            if (!session.cleanup_pending) {
                xEventGroupClearBits(voice_events, VOICE_TURN_BITS | VOICE_BIT_CAPTURE | VOICE_BIT_TRIGGER);
                xEventGroupSetBits(voice_events, VOICE_BIT_ARMED);
                voice_feedback_publish(VOICE_STATE_READY, NULL);
            }
            xSemaphoreGive(network_lock);
            ESP_LOGI(log_tag, "session released, waiting for wake word");
            continue;
        }
        // 挂起中：等唤醒再重建会话
        if (suspended) {
            xSemaphoreGive(network_lock);
            bits = xEventGroupWaitBits(voice_events, VOICE_BIT_TRIGGER | VOICE_BIT_RESET, pdFALSE, pdFALSE,
                                       pdMS_TO_TICKS(VOICE_POLL_MS));
            if (bits & VOICE_BIT_TRIGGER) {
                suspended = false;
            }
            continue;
        }
        esp_err_t err = ESP_OK;
        if (network_ready() && session.chat == 0) {
            err = open_session();
            if (err == ESP_OK && network_ready()) {
                xEventGroupClearBits(voice_events, VOICE_TURN_BITS);
                xEventGroupSetBits(voice_events, VOICE_BIT_ARMED);
                if (!(xEventGroupGetBits(voice_events) & VOICE_BIT_TRIGGER)) {
                    voice_feedback_publish(VOICE_STATE_READY, NULL);
                }
            }
        }
        xSemaphoreGive(network_lock);
        if (err != ESP_OK) {
            // Failed stop retains callback-owned resources; pending cleanup runs before the next setup.
            xEventGroupWaitBits(voice_events, VOICE_BIT_RESET, pdFALSE, pdFALSE, pdMS_TO_TICKS(VOICE_RETRY_MS));
            continue;
        }
        bits = xEventGroupWaitBits(voice_events, VOICE_BIT_TRIGGER | VOICE_BIT_RESET | VOICE_BIT_HANGUP, pdFALSE,
                                   pdFALSE, pdMS_TO_TICKS(VOICE_POLL_MS));
        if (!(bits & VOICE_BIT_TRIGGER) || (bits & VOICE_BIT_HANGUP) || !network_ready()) {
            continue;
        }
        if (xSemaphoreTake(network_lock, pdMS_TO_TICKS(VOICE_POLL_MS)) != pdTRUE) {
            continue;
        }
        if (network_ready() && session.chat != 0) {
            xEventGroupClearBits(voice_events, VOICE_BIT_TRIGGER);
            err = run_turn();
            ESP_LOGI(log_tag, "voice stack minimum free: %u bytes", (unsigned)uxTaskGetStackHighWaterMark(NULL));
            xEventGroupClearBits(voice_events, VOICE_BIT_CAPTURE);
            if (network_ready() && !(xEventGroupGetBits(voice_events) & VOICE_BIT_HANGUP)) {
                if (err != ESP_OK) {
                    voice_feedback_publish(VOICE_STATE_ERROR, NULL);
                    ESP_LOGW(log_tag, "turn failed: %s", esp_err_to_name(err));
                    close_session();
                } else {
                    xEventGroupClearBits(voice_events, VOICE_TURN_BITS);
                    xEventGroupSetBits(voice_events, VOICE_BIT_ARMED);
                    voice_feedback_publish(VOICE_STATE_READY, NULL);
                }
            }
        }
        xSemaphoreGive(network_lock);
    }
}

esp_err_t voice_init(void)
{
    if (voice_events != NULL) {
        return ESP_OK;
    }
    if (!audio_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0) {
        ESP_LOGW(log_tag, "WakeNet requires PSRAM");
        return ESP_ERR_NOT_SUPPORTED;
    }
    esp_err_t err = ESP_ERR_NOT_FOUND;
    wake_model.models = esp_srmodel_init("model");
    const char *name = wake_model.models ? esp_srmodel_filter(wake_model.models, "wn9_xiaoluxiaolu_tts2", NULL) : NULL;
    wake_model.wakenet = name ? esp_wn_handle_from_name(name) : NULL;
    if (wake_model.wakenet == NULL) {
        goto failed;
    }
    err = ESP_ERR_NO_MEM;
    wake_model.data = wake_model.wakenet->create(name, DET_MODE_90);
    if (wake_model.data == NULL) {
        goto failed;
    }
    wake_model.samples = wake_model.wakenet->get_samp_chunksize(wake_model.data);
    if (wake_model.samples <= 0 || wake_model.samples > VOICE_FRAME_SAMPLES
        || wake_model.wakenet->get_samp_rate(wake_model.data) != VOICE_SAMPLE_RATE
        || wake_model.wakenet->get_channel_num(wake_model.data) != 1)
    {
        err = ESP_ERR_NOT_SUPPORTED;
        goto failed;
    }
    err = ESP_ERR_NO_MEM;
    playback_ref_stream =
        xStreamBufferCreateWithCaps(VOICE_REF_BYTES, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (playback_ref_stream == NULL) {
        goto failed;
    }
    // Without AEC the previous behavior stands: no wake detection while a reply plays.
    err = ESP_ERR_NO_MEM;
    voice_events = xEventGroupCreate();
    if (voice_events != NULL) {
        ESP_ERROR_CHECK(esp_event_handler_register(ESP_XIAOZHI_CHAT_EVENTS, ESP_XIAOZHI_CHAT_EVENT_CONNECTED,
                                                   on_chat_lifecycle, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(ESP_XIAOZHI_CHAT_EVENTS, ESP_XIAOZHI_CHAT_EVENT_SERVER_GOODBYE,
                                                   on_chat_lifecycle, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(ESP_XIAOZHI_CHAT_EVENTS, ESP_XIAOZHI_CHAT_EVENT_DISCONNECTED,
                                                   on_chat_lifecycle, NULL));
    }
    network_lock = xSemaphoreCreateMutex();
    rx_audio_queue =
        xQueueCreateWithCaps(VOICE_RX_QUEUE_LEN, sizeof(struct voice_packet), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    microphone_stream =
        xStreamBufferCreateWithCaps(WAKE_BUFFER_BYTES, sizeof(voice_buffers.mono), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (voice_events && network_lock && rx_audio_queue && microphone_stream) {
        err = voice_capture_start(voice_events, microphone_stream, playback_ref_stream, wake_model.models,
                                  wake_model.wakenet, wake_model.data, wake_model.samples);
        if (err == ESP_OK) {
            voice_feedback_publish(VOICE_STATE_WAIT_NETWORK, NULL);
            if (xTaskCreatePinnedToCore(voice_task, "voice", VOICE_TASK_STACK, NULL, VOICE_TASK_PRIO, NULL,
                                        VOICE_AUDIO_CORE)
                == pdPASS)
            {
                return ESP_OK;
            }
            err = ESP_ERR_NO_MEM;
        }
    }
failed:
    voice_feedback_publish(VOICE_STATE_DISABLED, NULL);
    voice_capture_stop();
    if (playback_ref_stream != NULL) {
        vStreamBufferDeleteWithCaps(playback_ref_stream);
        playback_ref_stream = NULL;
    }
    if (microphone_stream != NULL) {
        vStreamBufferDeleteWithCaps(microphone_stream);
        microphone_stream = NULL;
    }
    if (rx_audio_queue != NULL) {
        vQueueDeleteWithCaps(rx_audio_queue);
        rx_audio_queue = NULL;
    }
    if (network_lock != NULL) {
        vSemaphoreDelete(network_lock);
        network_lock = NULL;
    }
    if (voice_events != NULL) {
        vEventGroupDelete(voice_events);
        voice_events = NULL;
    }
    if (wake_model.data != NULL) {
        wake_model.wakenet->destroy(wake_model.data);
        wake_model.data = NULL;
    }
    if (wake_model.models != NULL) {
        esp_srmodel_deinit(wake_model.models);
        wake_model.models = NULL;
    }
    return err;
}

void voice_net_ready(void)
{
    if (voice_events != NULL) {
        xEventGroupSetBits(voice_events, VOICE_BIT_ENABLED);
    }
}

void voice_net_set_connected(bool connected)
{
    if (voice_events == NULL) {
        return;
    }
    if (connected) {
        xEventGroupSetBits(voice_events, VOICE_BIT_NET);
    } else {
        EventBits_t previous = xEventGroupClearBits(voice_events, VOICE_BIT_NET);
        if (VOICE_NETWORK_READY(previous)) {
            xEventGroupSetBits(voice_events, VOICE_BIT_RESET);
        }
    }
}

void voice_net_pause(void)
{
    if (voice_events != NULL) {
        xEventGroupClearBits(voice_events, VOICE_BIT_ENABLED | VOICE_BIT_NET | VOICE_BIT_TRIGGER | VOICE_BIT_RECEIVING
                                               | VOICE_BIT_ARMED | VOICE_BIT_CAPTURE | VOICE_BIT_INTERRUPT
                                               | VOICE_BIT_FOLLOWUP | VOICE_BIT_SPEECH | VOICE_BIT_LOCAL_EOS);
        voice_feedback_publish(VOICE_STATE_WAIT_NETWORK, NULL);
        xEventGroupSetBits(voice_events, VOICE_BIT_RESET);
    }
}

bool voice_weather_begin(void)
{
    if (network_lock == NULL) {
        return true;
    }
    if (xSemaphoreTake(network_lock, 0) != pdTRUE) {
        return false;
    }
    if (xEventGroupGetBits(voice_events)
        & (VOICE_BIT_TRIGGER | VOICE_BIT_SPEECH | VOICE_BIT_CAPTURE | VOICE_BIT_RECEIVING))
    {
        xSemaphoreGive(network_lock);
        return false;
    }
    return true;
}

void voice_weather_end(void)
{
    if (network_lock != NULL) {
        xSemaphoreGive(network_lock);
    }
}
