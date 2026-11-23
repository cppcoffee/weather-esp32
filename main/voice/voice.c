#include "voice.h"
#include "device_mcp.h"
#include "voice_settings.h"

#include <string.h>

#include "audio.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mcp_engine.h"
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
    int     len;
    uint8_t data[VOICE_RX_OPUS_MAX];
};

static EventGroupHandle_t voice_events;
static SemaphoreHandle_t  network_lock;
static QueueHandle_t      rx_audio_queue;
#if !AUDIO_USE_ES7210_AEC
static StreamBufferHandle_t playback_ref_stream;
#endif
static StreamBufferHandle_t microphone_stream;

static srmodel_list_t *sr_models;

static struct {
    esp_xiaozhi_chat_handle_t chat;
    esp_mcp_t                *mcp;
    OpusEncoder              *encoder;
    OpusDecoder              *decoder;
    bool                      cleanup_pending;
} session;

// Only voice_task touches Opus and PCM. Encoding and decoding therefore share
// one per-task Opus pseudostack, as well as these PCM buffers.
static struct {
    int16_t mono[VOICE_FRAME_SAMPLES];
#if AUDIO_USE_ES7210_AEC
    int16_t tx_mono[VOICE_FRAME_SAMPLES];
#endif
    int16_t             stereo[VOICE_FRAME_SAMPLES * 2];
    uint8_t             tx_opus[VOICE_TX_OPUS_MAX];
    struct voice_packet packet;
} voice_buffers;

// Only websocket_task's on_audio callback writes rx_ingress_packet, keeping
// the 1.4 KB copy off websocket_task's stack.
static struct voice_packet rx_ingress_packet;

static bool
network_ready(void)
{
    return VOICE_NETWORK_READY(xEventGroupGetBits(voice_events));
}

static void
on_audio(const uint8_t *data, int len, void *ctx)
{
    (void)ctx;
    EventBits_t bits = xEventGroupGetBits(voice_events);
    if (!VOICE_NETWORK_READY(bits) || (bits & (VOICE_END_BITS | VOICE_BIT_ABORT_DRAIN))
        || !(bits & VOICE_BIT_RECEIVING))
    {
        return;
    }
    // The component owns data only until this callback returns. Never decode or
    // block on I2S here; queue a copy and drop on burst instead of tearing down
    // the session.
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

static void
on_event(esp_xiaozhi_chat_event_t event, void *data, void *ctx)
{
    (void)ctx;
    switch (event) {
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_TTS_STATE: {
        const esp_xiaozhi_chat_tts_state_t *state = data;
        if (state == NULL) {
            break;
        }
        // Sticky until the next turn: a fast start/stop must still end
        // recording.
        if (state->state == ESP_XIAOZHI_CHAT_TTS_STATE_START) {
            xEventGroupClearBits(voice_events, VOICE_BIT_ABORT_DRAIN | VOICE_BIT_TTS_STOPPED);
            xEventGroupSetBits(voice_events, VOICE_BIT_TTS_STARTED);
        } else if (state->state == ESP_XIAOZHI_CHAT_TTS_STATE_STOP) {
            xEventGroupClearBits(voice_events, VOICE_BIT_ABORT_DRAIN);
            // An abort may deliver the previous reply's stop after the new
            // round opened.
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
static void
on_chat_lifecycle(void *arg, esp_event_base_t base, int32_t id, void *data)
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

static void
close_session(void)
{
    xEventGroupClearBits(voice_events, VOICE_BIT_RECEIVING);
    if (session.chat != 0) {
        // A failed stop may leave callbacks live: retain all their resources
        // and retry.
        esp_err_t err = esp_xiaozhi_chat_stop(session.chat);
        if (err != ESP_OK) {
            session.cleanup_pending = true;
            voice_feedback_publish(VOICE_STATE_ERROR, NULL);
            xEventGroupSetBits(voice_events, VOICE_BIT_ERROR);
            audio_set_mute(true);
            ESP_LOGW(log_tag, "session stop pending: %s", esp_err_to_name(err));
            return;
        }
        // After successful WS stop, deinit consumes the handle even on cleanup
        // errors.
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

static esp_err_t
prepare_service(void)
{
    esp_xiaozhi_chat_info_t info = {0};
    voice_settings_t        settings = {0};
    esp_err_t               err = voice_settings_load(&settings);
    if (err != ESP_OK) {
        voice_feedback_publish(VOICE_STATE_NEED_SETUP, NULL);
        ESP_LOGW(log_tag, "voice settings unavailable: %s", esp_err_to_name(err));
        return err;
    }
    if (settings.info_url[0] == '\0') {
        voice_feedback_publish(VOICE_STATE_NEED_SETUP, NULL);
        // 没有编译期默认地址：配网页未填写就不去连任何服务
        ESP_LOGW(log_tag, "service info URL not configured; open the setup "
                          "page and fill it in");
        return ESP_ERR_INVALID_STATE;
    }
    err = esp_xiaozhi_chat_get_info_with_url(settings.info_url, &info);
    if (err == ESP_OK && (info.has_activation_code || info.has_activation_challenge)) {
        // Copy the code before releasing the response and poll again after
        // binding.
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
    bool has_websocket_config = info.has_websocket_config;
    esp_xiaozhi_chat_free_info(&info);
    if (err != ESP_OK || !has_websocket_config) {
        voice_feedback_publish(VOICE_STATE_ERROR, NULL);
        ESP_LOGW(log_tag,
                 "server config unavailable from %s: %s; retry after network "
                 "recovery",
                 settings.info_url, esp_err_to_name(err));
        return err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
    }

    return ESP_OK;
}

static esp_err_t
open_session(void)
{
    esp_xiaozhi_chat_config_t config = ESP_XIAOZHI_CHAT_DEFAULT_CONFIG();
    config.has_mqtt_config = false;
    config.has_websocket_config = true; // prepare_service saves the server's parameters in NVS.
    xEventGroupClearBits(voice_events, VOICE_BIT_WS_READY | VOICE_TURN_BITS | VOICE_BIT_HANGUP);
    esp_err_t err = esp_mcp_create(&session.mcp);
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
    // `chat ready` must mean the socket is up, otherwise the first wake word is
    // dropped.
    EventBits_t bits = xEventGroupWaitBits(voice_events, VOICE_BIT_WS_READY | VOICE_END_BITS, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(VOICE_CONNECT_TIMEOUT_MS));
    if (!VOICE_NETWORK_READY(bits) || (bits & VOICE_END_BITS)) {
        err = ESP_FAIL;
        goto failed;
    }
    if (!(bits & VOICE_BIT_WS_READY)) {
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
    ESP_LOGI(log_tag, "chat ready; internal heap free=%u, largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return ESP_OK;

failed:
    if (!(xEventGroupGetBits(voice_events) & VOICE_BIT_HANGUP)) {
        voice_feedback_publish(VOICE_STATE_ERROR, NULL);
        ESP_LOGW(log_tag, "session setup failed: %s", esp_err_to_name(err));
    }
    close_session();
    return err;
}

static bool
turn_can_continue(void)
{
    EventBits_t bits = xEventGroupGetBits(voice_events);
    return VOICE_NETWORK_READY(bits) && !(bits & VOICE_END_BITS);
}

static bool
start_chime(TickType_t *mute_at)
{
    voice_fill_chime(voice_buffers.mono, VOICE_FRAME_SAMPLES, VOICE_SAMPLE_RATE);
    voice_mono_to_stereo(voice_buffers.mono, voice_buffers.stereo, VOICE_FRAME_SAMPLES);
    // Tell capture_task to blank the chime + DMA/acoustic tail before unmuting
    // the PA so neither the PA click nor the chime enters microphone_stream.
    xEventGroupSetBits(voice_events, VOICE_BIT_CHIME);
    if (audio_set_mute(false) != ESP_OK) {
        xEventGroupClearBits(voice_events, VOICE_BIT_CHIME);
        return false;
    }
    size_t written = 0;
    if (audio_write(voice_buffers.stereo, VOICE_FRAME_SAMPLES * 4, &written) != ESP_OK
        || written != VOICE_FRAME_SAMPLES * 4)
    {
        audio_set_mute(true);
        xEventGroupClearBits(voice_events, VOICE_BIT_CHIME);
        return false;
    }
#if !AUDIO_USE_ES7210_AEC
    xStreamBufferReset(playback_ref_stream);
    xStreamBufferSend(playback_ref_stream, voice_buffers.mono, VOICE_FRAME_SAMPLES * sizeof(int16_t), 0);
#endif
    *mute_at = xTaskGetTickCount() + pdMS_TO_TICKS(VOICE_FRAME_MS + AUDIO_PLAYBACK_TAIL_MS) + 1;
    return true;
}

static void
finish_chime(TickType_t mute_at)
{
    TickType_t now = xTaskGetTickCount();
    if ((int32_t)(mute_at - now) > 0) {
        xEventGroupWaitBits(voice_events, VOICE_END_BITS, pdFALSE, pdFALSE, mute_at - now);
    }
    audio_set_mute(true);
}

#if AUDIO_USE_ES7210_AEC
static esp_err_t
run_turn(void)
{
    const esp_xiaozhi_chat_audio_t audio = {
        .format = "opus",
        .sample_rate = VOICE_SAMPLE_RATE,
        .channels = 1,
        .frame_duration = VOICE_FRAME_MS,
    };
    bool      channel_open = false;
    esp_err_t err = ESP_OK;

    xQueueReset(rx_audio_queue);
    xEventGroupClearBits(voice_events,
                         VOICE_BIT_TTS_STARTED | VOICE_BIT_TTS_STOPPED | VOICE_BIT_INTERRUPT | VOICE_BIT_ABORT_DRAIN);
    opus_encoder_ctl(session.encoder, OPUS_RESET_STATE);
    opus_decoder_ctl(session.decoder, OPUS_RESET_STATE);

    if (turn_can_continue()) {
        err = esp_xiaozhi_chat_open_audio_channel(session.chat, &audio, NULL, 0);
        if (err == ESP_OK) {
            channel_open = true;
            xEventGroupSetBits(voice_events, VOICE_BIT_RECEIVING);
            err = esp_xiaozhi_chat_send_wake_word(session.chat, wake_word);
        }
        if (err == ESP_OK) {
            err = esp_xiaozhi_chat_send_start_listening(session.chat, ESP_XIAOZHI_CHAT_LISTENING_MODE_REALTIME);
        }
    }

    xEventGroupClearBits(voice_events, VOICE_BIT_TRIGGER);
    if (err == ESP_OK && turn_can_continue()) {
        ESP_LOGI(log_tag, "listening (realtime, ES7210 AEC)");
        voice_feedback_publish(VOICE_STATE_LISTENING, NULL);
    }

    size_t     tx_filled = 0;
    bool       speaking = false;
    bool       had_reply = false;
    bool       awaiting_reply = false;
    TickType_t last_activity = xTaskGetTickCount();
    TickType_t last_rx_audio = 0;
    TickType_t mute_at = 0;

    while (err == ESP_OK && turn_can_continue()) {
        TickType_t now = xTaskGetTickCount();
        if (mute_at != 0 && (int32_t)(now - mute_at) >= 0) {
            audio_set_mute(true);
            mute_at = 0;
            voice_feedback_publish(VOICE_STATE_LISTENING, NULL);
        }

        // Pump up to 2 uplink frames per iteration so pre-roll drains quickly
        // without starving downlink playback.
        for (int sent = 0; sent < 2 && turn_can_continue(); sent++) {
            TickType_t wait =
                (sent == 0 && uxQueueMessagesWaiting(rx_audio_queue) == 0) ? pdMS_TO_TICKS(VOICE_CAPTURE_POLL_MS) : 0;
            size_t got = xStreamBufferReceive(microphone_stream, (uint8_t *)voice_buffers.tx_mono + tx_filled,
                                              sizeof(voice_buffers.tx_mono) - tx_filled, wait);
            tx_filled += got;
            if (tx_filled < sizeof(voice_buffers.tx_mono)) {
                break;
            }
            tx_filled = 0;
            int encoded = opus_encode(session.encoder, voice_buffers.tx_mono, VOICE_FRAME_SAMPLES,
                                      voice_buffers.tx_opus, sizeof(voice_buffers.tx_opus));
            if (encoded <= 0) {
                ESP_LOGW(log_tag, "opus encode failed: %d", encoded);
                err = ESP_FAIL;
                break;
            }
            err = esp_xiaozhi_chat_send_audio_data(session.chat, (const char *)voice_buffers.tx_opus, (size_t)encoded);
            if (err != ESP_OK) {
                break;
            }
        }
        if (err != ESP_OK || !turn_can_continue()) {
            break;
        }

        EventBits_t bits = xEventGroupGetBits(voice_events);
        now = xTaskGetTickCount();
        if (bits & VOICE_BIT_SPEECH) {
            last_activity = now;
            if (!speaking) {
                awaiting_reply = true;
            }
        }
        if ((bits & VOICE_BIT_TTS_STARTED) && mute_at != 0) {
            mute_at = 0;
        }
        // Server aborted TTS on barge-in: drop queued audio and mute right away.
        if ((bits & VOICE_BIT_TTS_STOPPED) && (bits & VOICE_BIT_SPEECH)) {
            xQueueReset(rx_audio_queue);
            xEventGroupClearBits(voice_events, VOICE_BIT_TTS_STARTED | VOICE_BIT_TTS_STOPPED);
            opus_decoder_ctl(session.decoder, OPUS_RESET_STATE);
            if (speaking || mute_at != 0) {
                audio_set_mute(true);
                speaking = false;
                mute_at = 0;
                ESP_LOGI(log_tag, "barge-in: reply stopped, listening");
                voice_feedback_publish(VOICE_STATE_LISTENING, NULL);
            }
            awaiting_reply = true;
            last_activity = now;
            continue;
        }

        if (xQueueReceive(rx_audio_queue, &voice_buffers.packet, 0) == pdTRUE) {
            int samples = opus_decode(session.decoder, voice_buffers.packet.data, voice_buffers.packet.len,
                                      voice_buffers.mono, VOICE_FRAME_SAMPLES, 0);
            if (samples <= 0) {
                ESP_LOGW(log_tag, "opus decode failed: %d", samples);
                err = ESP_FAIL;
                break;
            }
            voice_mono_to_stereo(voice_buffers.mono, voice_buffers.stereo, (size_t)samples);
            size_t    written = 0;
            esp_err_t write_err = (speaking || mute_at != 0) ? ESP_OK : audio_set_mute(false);
            if (write_err == ESP_OK) {
                write_err = audio_write(voice_buffers.stereo, (size_t)samples * 4, &written);
            }
            if (write_err != ESP_OK || written != (size_t)samples * 4) {
                err = ESP_FAIL;
                break;
            }
            if (!speaking) {
                ESP_LOGI(log_tag, "speaking (realtime)");
                voice_feedback_publish(VOICE_STATE_SPEAKING, NULL);
            }
            speaking = true;
            had_reply = true;
            awaiting_reply = false;
            mute_at = 0;
            last_rx_audio = xTaskGetTickCount();
            last_activity = last_rx_audio;
        } else if (bits & VOICE_BIT_TTS_STOPPED) {
            xEventGroupClearBits(voice_events, VOICE_BIT_TTS_STARTED | VOICE_BIT_TTS_STOPPED);
            opus_decoder_ctl(session.decoder, OPUS_RESET_STATE);
            if (speaking) {
                speaking = false;
                mute_at = xTaskGetTickCount() + pdMS_TO_TICKS(AUDIO_PLAYBACK_TAIL_MS) + 1;
                last_activity = xTaskGetTickCount();
            } else {
                // Empty reply (e.g. blank ASR transcript): stay listening.
                awaiting_reply = false;
            }
        } else if (speaking && now - last_rx_audio >= pdMS_TO_TICKS(VOICE_RESPONSE_TIMEOUT_MS)) {
            err = ESP_ERR_TIMEOUT;
            break;
        } else if (!speaking) {
            TickType_t limit = pdMS_TO_TICKS(
                awaiting_reply ? VOICE_RESPONSE_TIMEOUT_MS : (had_reply ? VOICE_FOLLOWUP_MS : VOICE_WAKE_NO_SPEECH_MS));
            if (now - last_activity >= limit) {
                ESP_LOGI(log_tag, "realtime session idle timeout");
                break;
            }
        }
    }

    EventBits_t bits = xEventGroupClearBits(voice_events, VOICE_BIT_RECEIVING | VOICE_BIT_ARMED | VOICE_BIT_INTERRUPT
                                                              | VOICE_BIT_BTN_WAKE | VOICE_BIT_CHIME | VOICE_BIT_CAPTURE
                                                              | VOICE_BIT_FOLLOWUP | VOICE_BIT_SPEECH
                                                              | VOICE_BIT_LOCAL_EOS | VOICE_BIT_ABORT_DRAIN);
    audio_set_mute(true);
    if (channel_open && VOICE_NETWORK_READY(bits) && !(bits & (VOICE_BIT_WS_LOST | VOICE_BIT_HANGUP))) {
        esp_xiaozhi_chat_close_audio_channel(session.chat);
    }
    xQueueReset(rx_audio_queue);
    return (bits & (VOICE_BIT_WS_LOST | VOICE_BIT_ERROR)) ? ESP_FAIL : err;
}
#else
static esp_err_t
stream_microphone(bool followup, bool *heard_speech)
{
    const TickType_t no_speech_timeout = pdMS_TO_TICKS(followup ? VOICE_FOLLOWUP_MS : VOICE_WAKE_NO_SPEECH_MS);
    TickType_t       started = xTaskGetTickCount();
    TickType_t       speech_started = started;
    bool             speech_seen = false;
    size_t           filled = 0;
    for (;;) {
        EventBits_t bits = xEventGroupGetBits(voice_events);
        if (!turn_can_continue()) {
            *heard_speech = speech_seen;
            return ESP_FAIL;
        }
        if (bits & VOICE_BIT_TTS_STARTED) {
            *heard_speech = speech_seen;
            return ESP_OK;
        }
        bool ending = (bits & VOICE_BIT_LOCAL_EOS) != 0;
        if (!speech_seen && (bits & VOICE_BIT_SPEECH)) {
            speech_seen = true;
            speech_started = xTaskGetTickCount();
            if (followup) {
                voice_feedback_publish(VOICE_STATE_LISTENING, NULL);
            }
        }
        if (!ending) {
            if (!speech_seen) {
                if (xTaskGetTickCount() - started >= no_speech_timeout) {
                    break;
                }
            } else if (xTaskGetTickCount() - speech_started >= pdMS_TO_TICKS(VOICE_TURN_TIMEOUT_MS)) {
                break;
            }
        }
        filled += xStreamBufferReceive(microphone_stream, (uint8_t *)voice_buffers.mono + filled,
                                       sizeof(voice_buffers.mono) - filled,
                                       ending ? 0 : pdMS_TO_TICKS(VOICE_CAPTURE_POLL_MS));
        if (filled < sizeof(voice_buffers.mono)) {
            if (!ending) {
                continue;
            }
            if (!filled) {
                *heard_speech = speech_seen;
                return ESP_OK;
            }
            memset((uint8_t *)voice_buffers.mono + filled, 0, sizeof(voice_buffers.mono) - filled);
        }
        if (!turn_can_continue()) {
            *heard_speech = speech_seen;
            return ESP_FAIL;
        }
        filled = 0;
        int encoded = opus_encode(session.encoder, voice_buffers.mono, VOICE_FRAME_SAMPLES, voice_buffers.tx_opus,
                                  sizeof(voice_buffers.tx_opus));
        if (encoded <= 0) {
            ESP_LOGW(log_tag, "opus encode failed: %d", encoded);
            *heard_speech = speech_seen;
            return ESP_FAIL;
        }
        esp_err_t err =
            esp_xiaozhi_chat_send_audio_data(session.chat, (const char *)voice_buffers.tx_opus, (size_t)encoded);
        if (err != ESP_OK) {
            *heard_speech = speech_seen;
            return err;
        }
    }
    *heard_speech = speech_seen;
    return ESP_OK;
}

static esp_err_t
play_response(bool *played)
{
    TickType_t last_audio = xTaskGetTickCount();
    bool       received_audio = false;
    esp_err_t  err = ESP_ERR_TIMEOUT;

    // The first packets can already be queued while recording winds down.
    xStreamBufferReset(playback_ref_stream);
    // Keep WakeNet armed while the reply plays; AFE AEC suppresses the
    // assistant's echo.
    xEventGroupSetBits(voice_events, VOICE_BIT_ARMED);
    // Tool discovery + LLM/tool rounds + first TTS have a bounded 75 s server
    // budget. Once playback starts, retain the shorter gap limit rather than
    // extending every gap.
    while (xTaskGetTickCount() - last_audio
           < pdMS_TO_TICKS(received_audio ? VOICE_RESPONSE_TIMEOUT_MS : VOICE_FIRST_AUDIO_TIMEOUT_MS))
    {
        if (!turn_can_continue()) {
            err = ESP_FAIL;
            break;
        }
        if (xEventGroupGetBits(voice_events) & VOICE_BIT_INTERRUPT) {
            // The next listening iteration drops queued packets before it
            // accepts audio again.
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
            size_t    written = 0;
            esp_err_t write_err = received_audio ? ESP_OK : audio_set_mute(false);
            if (write_err == ESP_OK) {
                write_err = audio_write(voice_buffers.stereo, (size_t)samples * 4, &written);
            }
            if (write_err != ESP_OK || written != (size_t)samples * 4) {
                err = ESP_FAIL;
                break;
            }
            if (xStreamBufferSpacesAvailable(playback_ref_stream) < (size_t)samples * sizeof(int16_t)) {
                xStreamBufferReset(playback_ref_stream);
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

static esp_err_t
run_turn(void)
{
    const esp_xiaozhi_chat_audio_t audio = {
        .format = "opus",
        .sample_rate = VOICE_SAMPLE_RATE,
        .channels = 1,
        .frame_duration = VOICE_FRAME_MS,
    };
    bool      channel_open = false;
    bool      interrupted = false;
    bool      followup = false;
    int       empty_turns = 0;
    esp_err_t err = ESP_OK;

    while (turn_can_continue()) {
        xQueueReset(rx_audio_queue);
        // Keep VOICE_BIT_LOCAL_EOS / VOICE_BIT_SPEECH from pre-roll capture so a
        // slow connect never strands a finished first utterance for 20 s.
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
            // Accept downstream audio again once the abort drain finishes on
            // stop/start.
            xEventGroupSetBits(voice_events, VOICE_BIT_RECEIVING);
            ESP_LOGI(log_tag, "reply aborted, listening again");
        } else if (followup) {
            xEventGroupClearBits(voice_events, VOICE_BIT_LOCAL_EOS | VOICE_BIT_SPEECH);
            xStreamBufferReset(microphone_stream);
            xEventGroupSetBits(voice_events, VOICE_BIT_RECEIVING | VOICE_BIT_FOLLOWUP | VOICE_BIT_CAPTURE);
        }
        err = esp_xiaozhi_chat_send_start_listening(session.chat, ESP_XIAOZHI_CHAT_LISTENING_MODE_AUTO);
        if (err != ESP_OK) {
            break;
        }
        if (interrupted) {
            TickType_t mute_at = 0;
            if (start_chime(&mute_at)) {
                finish_chime(mute_at);
            }
        }
        xEventGroupClearBits(voice_events, VOICE_BIT_TRIGGER);
        ESP_LOGI(log_tag, "%s", followup ? "listening (followup)" : "listening");
        voice_feedback_publish(followup ? VOICE_STATE_FOLLOWUP : VOICE_STATE_LISTENING, NULL);
        bool heard_speech = false;
        err = stream_microphone(followup, &heard_speech);
        EventBits_t stream_bits = xEventGroupClearBits(voice_events, VOICE_BIT_CAPTURE | VOICE_BIT_FOLLOWUP
                                                                         | VOICE_BIT_SPEECH | VOICE_BIT_LOCAL_EOS);
        bool        tts_started = (stream_bits & VOICE_BIT_TTS_STARTED) != 0;
        bool        local_eos = (stream_bits & VOICE_BIT_LOCAL_EOS) != 0;
        if (err == ESP_OK && turn_can_continue() && !heard_speech && !tts_started && !local_eos) {
            ESP_LOGI(log_tag, "%s timed out with no speech", followup ? "followup" : "listening");
            break;
        }
        if (err == ESP_OK && turn_can_continue() && !tts_started) {
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
            // I2S write completes before all samples from its DMA ring reach
            // the speaker.
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
        if (interrupted || played) {
            empty_turns = 0;
            followup = !interrupted;
        } else if (++empty_turns <= VOICE_MAX_EMPTY_TURNS) {
            ESP_LOGI(log_tag, "empty reply (%d/%d), returning to listening", empty_turns, VOICE_MAX_EMPTY_TURNS);
            followup = true;
        } else {
            break;
        }
    }
    EventBits_t bits = xEventGroupClearBits(voice_events, VOICE_BIT_RECEIVING | VOICE_BIT_ARMED | VOICE_BIT_INTERRUPT
                                                              | VOICE_BIT_BTN_WAKE | VOICE_BIT_CHIME | VOICE_BIT_CAPTURE
                                                              | VOICE_BIT_FOLLOWUP | VOICE_BIT_SPEECH
                                                              | VOICE_BIT_LOCAL_EOS | VOICE_BIT_ABORT_DRAIN);
    audio_set_mute(true);
    if (channel_open && VOICE_NETWORK_READY(bits) && !(bits & (VOICE_BIT_WS_LOST | VOICE_BIT_HANGUP))) {
        esp_xiaozhi_chat_close_audio_channel(session.chat);
    }
    xQueueReset(rx_audio_queue);
    xStreamBufferReset(playback_ref_stream);
    return (bits & (VOICE_BIT_WS_LOST | VOICE_BIT_ERROR)) ? ESP_FAIL : err;
}
#endif

static void
finish_wake(void)
{
    EventBits_t bits =
        xEventGroupClearBits(voice_events, VOICE_BIT_TRIGGER | VOICE_BIT_CAPTURE | VOICE_BIT_LOCAL_EOS
                                               | VOICE_BIT_SPEECH | VOICE_BIT_BTN_WAKE | VOICE_BIT_CHIME
                                               | VOICE_BIT_HANGUP | VOICE_BIT_WS_READY | VOICE_TURN_BITS);
    if (VOICE_LOCAL_READY(bits)) {
        xEventGroupSetBits(voice_events, VOICE_BIT_ARMED);
    }
}

static void
voice_task(void *arg)
{
    (void)arg;
    const EventBits_t work = VOICE_BIT_TRIGGER | VOICE_BIT_RESET | VOICE_BIT_LINK_LOST;
    const TickType_t  retry_interval = pdMS_TO_TICKS(VOICE_RETRY_MS);
    bool              service_ready = false;
    bool              service_checked = false;
    TickType_t        last_service_check = 0;
    for (;;) {
        if (session.cleanup_pending) {
            xSemaphoreTake(network_lock, portMAX_DELAY);
            close_session();
            xSemaphoreGive(network_lock);
            if (session.cleanup_pending) {
                vTaskDelay(pdMS_TO_TICKS(VOICE_POLL_MS));
            } else if (VOICE_LOCAL_READY(xEventGroupGetBits(voice_events))
                       && !(xEventGroupGetBits(voice_events) & VOICE_BIT_TRIGGER))
            {
                voice_feedback_publish(VOICE_STATE_READY, NULL);
            }
            continue;
        }

        EventBits_t bits = xEventGroupGetBits(voice_events);
        if (bits & VOICE_BIT_RESET) {
            service_ready = false;
            service_checked = false;
            xEventGroupClearBits(voice_events, VOICE_BIT_RESET | VOICE_BIT_LINK_LOST);
            finish_wake();
            if (VOICE_LOCAL_READY(xEventGroupGetBits(voice_events))) {
                voice_feedback_publish(VOICE_STATE_READY, NULL);
            } else {
                voice_feedback_publish(VOICE_STATE_WAIT_NETWORK, NULL);
            }
            continue;
        }
        if (bits & VOICE_BIT_LINK_LOST) {
            xEventGroupClearBits(voice_events, VOICE_BIT_LINK_LOST);
            continue;
        }
        if (!(bits & VOICE_BIT_ENABLED)) {
            xEventGroupWaitBits(voice_events, VOICE_BIT_ENABLED | VOICE_BIT_RESET, pdFALSE, pdFALSE, portMAX_DELAY);
            continue;
        }

        bool woke = (bits & VOICE_BIT_TRIGGER) != 0;
        if (!(bits & VOICE_BIT_NET)) {
            TickType_t  wait = woke ? pdMS_TO_TICKS(VOICE_NETWORK_WAIT_MS) : portMAX_DELAY;
            EventBits_t mask = woke ? VOICE_BIT_NET | VOICE_BIT_RESET | VOICE_BIT_HANGUP : work | VOICE_BIT_NET;
            if (woke) {
                net_request_reconnect();
            }
            bits = xEventGroupWaitBits(voice_events, mask, pdFALSE, pdFALSE, wait);
            if (woke && (bits & VOICE_BIT_HANGUP)) {
                voice_feedback_publish(VOICE_STATE_READY, NULL);
                finish_wake();
            } else if (woke && !(bits & mask)) {
                voice_feedback_publish(VOICE_STATE_OFFLINE, NULL);
                finish_wake();
            }
            continue;
        }

        if (!woke) {
            if (voice_feedback_get().state == VOICE_STATE_OFFLINE) {
                voice_feedback_publish(VOICE_STATE_READY, NULL);
            }
            if (!(bits & VOICE_BIT_CHECK_INFO)) {
                xEventGroupWaitBits(voice_events, work | VOICE_BIT_CHECK_INFO, pdFALSE, pdFALSE, portMAX_DELAY);
                continue;
            }
            TickType_t elapsed = xTaskGetTickCount() - last_service_check;
            if (service_ready || (service_checked && elapsed < retry_interval)) {
                TickType_t wait = service_ready ? portMAX_DELAY : retry_interval - elapsed;
                xEventGroupWaitBits(voice_events, work, pdFALSE, pdFALSE, wait);
                continue;
            }
        }

        bool       chime_active = false;
        TickType_t chime_mute_at = 0;
        if (woke) {
            chime_active = start_chime(&chime_mute_at);
        }
        xSemaphoreTake(network_lock, portMAX_DELAY);
        if (!network_ready()) {
            if (chime_active) {
                finish_chime(chime_mute_at);
            }
            xSemaphoreGive(network_lock);
            continue;
        }
        // The capture task may have detected a wake while weather held the
        // network lock.
        woke = (xEventGroupGetBits(voice_events) & VOICE_BIT_TRIGGER) != 0;
        if (woke) {
            if (!chime_active) {
                chime_active = start_chime(&chime_mute_at);
            }
            net_set_wifi_low_latency(true);
        }
        if (!service_ready) {
            service_ready = prepare_service() == ESP_OK;
            service_checked = true;
            last_service_check = xTaskGetTickCount();
        }
        if (woke) {
            if (xEventGroupGetBits(voice_events) & VOICE_BIT_HANGUP) {
                if (VOICE_LOCAL_READY(xEventGroupGetBits(voice_events))) {
                    voice_feedback_publish(VOICE_STATE_READY, NULL);
                }
            } else if (service_ready && network_ready()) {
                if (open_session() == ESP_OK) {
                    if (chime_active) {
                        finish_chime(chime_mute_at);
                        chime_active = false;
                    }
                    esp_err_t err = run_turn();
                    bits = xEventGroupGetBits(voice_events);
                    if (err != ESP_OK && !(bits & (VOICE_BIT_HANGUP | VOICE_BIT_RESET | VOICE_BIT_LINK_LOST))) {
                        ESP_LOGW(log_tag, "turn failed: %s", esp_err_to_name(err));
                    }
                    ESP_LOGI(log_tag, "voice stack minimum free: %u bytes",
                             (unsigned)uxTaskGetStackHighWaterMark(NULL));
                    close_session();
                    if (!session.cleanup_pending && VOICE_LOCAL_READY(xEventGroupGetBits(voice_events))) {
                        voice_state_t state = VOICE_STATE_READY;
                        if (err != ESP_OK && !(bits & VOICE_BIT_HANGUP)) {
                            state = (bits & VOICE_BIT_LINK_LOST) ? VOICE_STATE_OFFLINE : VOICE_STATE_ERROR;
                        }
                        voice_feedback_publish(state, NULL);
                    }
                } else if (xEventGroupGetBits(voice_events) & VOICE_BIT_HANGUP) {
                    if (VOICE_LOCAL_READY(xEventGroupGetBits(voice_events))) {
                        voice_feedback_publish(VOICE_STATE_READY, NULL);
                    }
                } else {
                    // Try fresh service parameters after a failed WebSocket
                    // setup.
                    service_ready = false;
                    last_service_check = xTaskGetTickCount();
                }
            }
            if (chime_active) {
                finish_chime(chime_mute_at);
            }
            net_set_wifi_low_latency(false);
            finish_wake();
        } else if (service_ready && network_ready() && !(xEventGroupGetBits(voice_events) & VOICE_BIT_TRIGGER)) {
            voice_feedback_publish(VOICE_STATE_READY, NULL);
        }
        xSemaphoreGive(network_lock);
    }
}

esp_err_t
voice_init(void)
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
    sr_models = esp_srmodel_init("model");
    if (sr_models == NULL) {
        goto failed;
    }
    err = ESP_ERR_NO_MEM;
#if !AUDIO_USE_ES7210_AEC
    playback_ref_stream =
        xStreamBufferCreateWithCaps(VOICE_REF_BYTES, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (playback_ref_stream == NULL) {
        goto failed;
    }
#endif
    voice_events = xEventGroupCreate();
    if (voice_events != NULL) {
        voice_net_set_connected(net_has_ip());
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
#if AUDIO_USE_ES7210_AEC
        err = voice_capture_start(voice_events, microphone_stream, NULL, sr_models);
#else
        err = voice_capture_start(voice_events, microphone_stream, playback_ref_stream, sr_models);
#endif
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
#if !AUDIO_USE_ES7210_AEC
    if (playback_ref_stream != NULL) {
        vStreamBufferDeleteWithCaps(playback_ref_stream);
        playback_ref_stream = NULL;
    }
#endif
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
    if (sr_models != NULL) {
        esp_srmodel_deinit(sr_models);
        sr_models = NULL;
    }
    return err;
}

void
voice_enable_wake(void)
{
    if (voice_events != NULL && !(xEventGroupGetBits(voice_events) & VOICE_BIT_ENABLED)) {
        xEventGroupSetBits(voice_events, VOICE_BIT_ENABLED | VOICE_BIT_ARMED);
        voice_feedback_publish(VOICE_STATE_READY, NULL);
    }
}

void
voice_net_ready(void)
{
    voice_enable_wake();
    if (voice_events != NULL) {
        xEventGroupSetBits(voice_events, VOICE_BIT_CHECK_INFO);
    }
}

void
voice_net_set_connected(bool connected)
{
    if (voice_events == NULL) {
        return;
    }
    if (connected) {
        xEventGroupSetBits(voice_events, VOICE_BIT_NET);
    } else {
        EventBits_t previous = xEventGroupClearBits(voice_events, VOICE_BIT_NET);
        if (previous & VOICE_BIT_NET) {
            xEventGroupSetBits(voice_events, VOICE_BIT_LINK_LOST);
        }
    }
}

void
voice_net_pause(void)
{
    if (voice_events != NULL) {
        xEventGroupClearBits(voice_events, VOICE_BIT_ENABLED | VOICE_BIT_CHECK_INFO | VOICE_BIT_NET | VOICE_BIT_TRIGGER
                                               | VOICE_BIT_RECEIVING | VOICE_BIT_ARMED | VOICE_BIT_CAPTURE
                                               | VOICE_BIT_INTERRUPT | VOICE_BIT_BTN_WAKE | VOICE_BIT_CHIME
                                               | VOICE_BIT_FOLLOWUP | VOICE_BIT_SPEECH | VOICE_BIT_LOCAL_EOS);
        voice_feedback_publish(VOICE_STATE_WAIT_NETWORK, NULL);
        xEventGroupSetBits(voice_events, VOICE_BIT_RESET);
    }
}

void
voice_toggle_chat(void)
{
    if (voice_events == NULL) {
        return;
    }
    EventBits_t bits = xEventGroupGetBits(voice_events);
    if (!VOICE_LOCAL_READY(bits)) {
        return;
    }
    if (bits & VOICE_BIT_BTN_WAKE) {
        xEventGroupClearBits(voice_events, VOICE_BIT_BTN_WAKE);
        return;
    }
    if (bits & (VOICE_BIT_TRIGGER | VOICE_BIT_CAPTURE | VOICE_BIT_RECEIVING)) {
        xEventGroupSetBits(voice_events, VOICE_BIT_HANGUP);
        return;
    }
    if (bits & VOICE_BIT_ARMED) {
        xEventGroupSetBits(voice_events, VOICE_BIT_BTN_WAKE);
    }
}

bool
voice_weather_begin(void)
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

void
voice_weather_end(void)
{
    if (network_lock != NULL) {
        xSemaphoreGive(network_lock);
    }
}
