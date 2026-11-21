#!/usr/bin/env python3
"""Exercise the production pairing and audio-turn code with SDK and I/O stubs."""
import argparse
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile

from test_voice_lifecycle import extract, run


BOUNDARIES = (
    ("static void on_audio(const uint8_t *data, int len, void *ctx)", "static void on_event(esp_xiaozhi_chat_event_t event, void *data, void *ctx)"),
    ("static void on_event(esp_xiaozhi_chat_event_t event, void *data, void *ctx)",
     "// The session task owns teardown. Callbacks only wake its current wait."),
    ("static void on_chat_lifecycle(void *arg, esp_event_base_t base, int32_t id, void *data)",
     "static void close_session(void)"),
    ("static void close_session(void)", "static esp_err_t open_session(void)"),
    ("static esp_err_t open_session(void)", "static bool turn_can_continue(void)"),
    ("static bool turn_can_continue(void)", "static esp_err_t stream_microphone(void)"),
    ("static esp_err_t stream_microphone(void)", "static esp_err_t play_response(bool *played)"),
    ("static esp_err_t play_response(bool *played)", "// Returns with the network lock held again, including on timeout or disconnect."),
    ("static bool wait_followup(void)", "static esp_err_t run_turn(void)"),
    ("static esp_err_t run_turn(void)", "static void voice_task(void *arg)"),
    ("bool voice_weather_begin(void)", "void voice_weather_end(void)"),
    ("void voice_weather_end(void)", "/* end of voice code */"),
)

STUB_HEADERS = {
    "esp_err.h": """#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_NO_MEM 2
#define ESP_ERR_TIMEOUT 3
""",
    "nvs.h": "typedef int nvs_handle_t;\n",
    "esp_event_base.h": "#define ESP_EVENT_DECLARE_BASE(name) extern const char *name\n",
    "esp_mcp_engine.h": "typedef struct esp_mcp_s { int unused; } esp_mcp_t;\n",
    "esp_heap_caps.h": "",
    "freertos/FreeRTOS.h": """#define portMUX_TYPE int
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
""",
}

PRELUDE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio.h"
#include "esp_xiaozhi_chat.h"
#include "esp_xiaozhi_info.h"
#include "voice_audio.h"
#include "voice_feedback.h"
#include "voice_internal.h"
#include "voice_settings.h"

typedef unsigned EventBits_t, TickType_t;
typedef const char *esp_event_base_t;
typedef int OpusEncoder, OpusDecoder;
struct voice_packet {
    int len;
    uint8_t data[VOICE_RX_OPUS_MAX];
};
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) (ms)
#define portTICK_PERIOD_MS 1
#define portMAX_DELAY UINT32_MAX
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(tag, ...) do { if (0) fprintf(stderr, __VA_ARGS__); } while (0)
#define esp_err_to_name(error) ((void)(error), "stub error")
#define OPUS_OK 0
#define OPUS_APPLICATION_VOIP 1
#define OPUS_SET_BITRATE(x) (x)
#define OPUS_SET_COMPLEXITY(x) (100 + (x))
#define OPUS_RESET_STATE 2

static void net_set_wifi_low_latency(bool enable) { (void)enable; }

static const char *wake_word = "wake";
static EventBits_t event_bits;
static EventBits_t *voice_events = &event_bits;
static struct {
    esp_xiaozhi_chat_handle_t chat;
    esp_mcp_t *mcp;
    OpusEncoder *encoder;
    OpusDecoder *decoder;
    bool cleanup_pending;
    bool followup_enabled;
} session;
static esp_mcp_t engine;
static OpusEncoder encoder;
static OpusDecoder decoder;
static bool muted = true;
static int ref, mic;
static int *playback_ref_stream = &ref, *microphone_stream = &mic;
static struct voice_packet queue[VOICE_RX_QUEUE_LEN];
static struct voice_packet *rx_audio_queue = queue;
static unsigned count;
static struct {
    int16_t mono[VOICE_FRAME_SAMPLES];
    int16_t stereo[VOICE_FRAME_SAMPLES * 2];
    uint8_t tx_opus[VOICE_TX_OPUS_MAX];
    struct voice_packet packet;
} voice_buffers;
static struct voice_packet rx_ingress_packet;
static TickType_t ticks;

enum turn_case { EARLY, BEFORE_TTS, BURST, OVERFLOW, SLOW_FIRST, NO_FIRST, STALL, DROP_LISTEN, DROP_PLAY, BARGE,
                 FOLLOWUP, SILENT, BUSY_WEATHER, GOODBYE_LISTEN, GOODBYE_PLAY, GOODBYE_WINDOW, WS_LOST_WINDOW,
                 EMPTY_RESULT, WEATHER_RECOVERS, LOCAL_ENDPOINT };
enum info_case { NO_SETTINGS, INFO_FAILURE, NO_WS, CODE, EMPTY_CODE, CHALLENGE, BOUND, CHAT_INIT_ERROR, CONNECT_TIMEOUT };
static enum turn_case turn_case;
static enum info_case info_case;
static const char *code;
static unsigned info_requests, info_frees, chat_inits, chat_starts, chat_stops, mcp_destroys;
static unsigned listens, closes, aborts, writes;
static int16_t played[VOICE_RX_QUEUE_LEN];
static bool injected, locked;
static unsigned windows, drains, lock_attempts, uploads;
static int *network_lock = &ref;
static int16_t captured[VOICE_FRAME_SAMPLES * 3];
static size_t captured_samples;
static unsigned vad_frames, vad_trigger_frame;
static struct {
    EventBits_t *events;
    int *microphone;
    void *vad;
    int16_t (*vad_history)[FOLLOWUP_SAMPLES];
    size_t vad_fill, vad_next, vad_count;
    size_t endpoint_fill;
    unsigned endpoint_holdoff_frames;
    bool endpoint_heard_speech;
} capture_state = {.events = &event_bits, .microphone = &mic};
static struct {
    int16_t endpoint_frame[FOLLOWUP_SAMPLES];
} capture_buffers;
#define VAD_SPEECH 1

static int vad_process_with_trigger(void *vad, int16_t *frame) {
    assert(vad == &mic && frame[0] != 0);
    return ++vad_frames == vad_trigger_frame;
}

static int xSemaphoreGive(int *lock) {
    assert(lock == network_lock && locked);
    locked = false;
    return pdTRUE;
}
static int xSemaphoreTake(int *lock, TickType_t wait) {
    assert(lock == network_lock);
    if (locked) {
        assert(wait == 0);
        return pdFALSE;
    }
    if (wait != portMAX_DELAY && (turn_case == BUSY_WEATHER || (turn_case == WEATHER_RECOVERS && lock_attempts++ < 3))) {
        ticks += wait;
        return pdFALSE;
    }
    locked = true;
    return pdTRUE;
}
static EventBits_t xEventGroupWaitBits(EventBits_t *group, EventBits_t mask, int clear, int all, TickType_t wait);


static EventBits_t xEventGroupGetBits(EventBits_t *group) { return *group; }
static void xEventGroupSetBits(EventBits_t *group, EventBits_t bits) { *group |= bits; }
static EventBits_t xEventGroupClearBits(EventBits_t *group, EventBits_t bits) {
    EventBits_t previous = *group;
    *group &= ~bits;
    return previous;
}
static TickType_t xTaskGetTickCount(void) { return ticks; }

static void xQueueReset(struct voice_packet *q) { assert(q == queue); count = 0; }
static int xQueueSend(struct voice_packet *q, const struct voice_packet *packet, TickType_t wait) {
    assert(q == queue && wait == 0);
    if (count == VOICE_RX_QUEUE_LEN) return pdFALSE;
    q[count++] = *packet;
    return pdTRUE;
}
static int xQueueReceive(struct voice_packet *q, struct voice_packet *packet, TickType_t wait);
static void xStreamBufferReset(int *buffer) {
    assert(buffer == playback_ref_stream || buffer == microphone_stream);
    if (buffer == microphone_stream) captured_samples = 0;
}
static size_t xStreamBufferSpacesAvailable(int *buffer) {
    assert(buffer == microphone_stream);
    return (sizeof(captured) - captured_samples * sizeof(*captured));
}
static size_t xStreamBufferReceive(int *buffer, void *data, size_t len, TickType_t wait);
static void xStreamBufferSend(int *buffer, const void *data, size_t len, TickType_t wait) {
    if (buffer == microphone_stream) {
        assert(len <= xStreamBufferSpacesAvailable(buffer) && len % sizeof(*captured) == 0 && wait == 0);
        memcpy(captured + captured_samples, data, len);
        captured_samples += len / sizeof(*captured);
    } else {
        assert(buffer == playback_ref_stream && data == voice_buffers.mono && len == sizeof(*voice_buffers.mono) && wait == 0);
    }
}
static bool voice_capture_has_aec(void) { return turn_case == BARGE; }
static bool voice_capture_prepare_vad(void) { return turn_case >= FOLLOWUP && turn_case != LOCAL_ENDPOINT; }
esp_err_t audio_set_mute(bool mute) { muted = mute; return ESP_OK; }
esp_err_t audio_write(void *data, size_t len, size_t *written) {
    const int16_t *pcm = data;
    assert(!muted && data == voice_buffers.stereo && len == 4 && pcm[0] == pcm[1] && writes < VOICE_RX_QUEUE_LEN);
    played[writes++] = pcm[0];
    *written = len;
    if (turn_case == BARGE && listens == 1) event_bits |= VOICE_BIT_INTERRUPT;
    return ESP_OK;
}

static esp_err_t esp_mcp_create(esp_mcp_t **out) { *out = &engine; return ESP_OK; }
static esp_err_t device_mcp_register(esp_mcp_t *mcp) { assert(mcp == &engine); return ESP_OK; }
static esp_err_t esp_mcp_destroy(esp_mcp_t *mcp) { assert(mcp == &engine); mcp_destroys++; return ESP_OK; }
static OpusEncoder *opus_encoder_create(int rate, int channels, int type, int *error) {
    assert(rate == VOICE_SAMPLE_RATE && channels == 1 && type == OPUS_APPLICATION_VOIP);
    *error = OPUS_OK;
    return &encoder;
}
static OpusDecoder *opus_decoder_create(int rate, int channels, int *error) {
    assert(rate == VOICE_SAMPLE_RATE && channels == 1);
    *error = OPUS_OK;
    return &decoder;
}
static void opus_encoder_destroy(OpusEncoder *value) { assert(value == &encoder); }
static void opus_decoder_destroy(OpusDecoder *value) { assert(value == &decoder); }
static int opus_encoder_ctl(OpusEncoder *value, int op) { assert(value == &encoder && op > 0); return OPUS_OK; }
static int opus_decoder_ctl(OpusDecoder *value, int op) { assert(value == &decoder && op == OPUS_RESET_STATE); return OPUS_OK; }
static int opus_encode(OpusEncoder *value, const int16_t *pcm, int samples, uint8_t *out, size_t cap) {
    assert(VOICE_NETWORK_READY(event_bits));
    assert(value == &encoder && pcm == voice_buffers.mono && samples == VOICE_FRAME_SAMPLES && cap == sizeof(voice_buffers.tx_opus));
    if (turn_case == LOCAL_ENDPOINT) {
        for (int i = 0; i < samples; i++) assert(pcm[i] == (i < samples / 2 ? 1 : 0));
    }
    out[0] = 42;
    return 1;
}
static int opus_decode(OpusDecoder *value, const uint8_t *bytes, int len, int16_t *pcm, int samples, int fec) {
    assert(value == &decoder && samples == VOICE_FRAME_SAMPLES && len == 1 && fec == 0);
    pcm[0] = bytes[0];
    return 1;
}
'''

HARNESS = r'''
#line 1 "voice_session_harness.c"
static void tts(esp_xiaozhi_chat_tts_state_kind_t state) {
    esp_xiaozhi_chat_tts_state_t event = {.state = state};
    on_event(ESP_XIAOZHI_CHAT_EVENT_CHAT_TTS_STATE, &event, NULL);
}

static void packet(uint8_t original) {
    uint8_t byte = original;
    on_audio(&byte, 1, NULL);
    byte = 0;  /* The callback must have taken a copy. */
    assert(byte == 0);
}

static EventBits_t xEventGroupWaitBits(EventBits_t *group, EventBits_t mask, int clear, int all, TickType_t wait) {
    assert(group == voice_events && !clear && !all);
    if (mask == VOICE_BIT_WS_READY) {
        assert(wait == VOICE_CONNECT_TIMEOUT_MS);
        return *group; /* No ticks: the real connect normally completes before the wait. */
    }
    if (mask == (VOICE_END_BITS | VOICE_BIT_INTERRUPT)) {
        assert(locked && wait == AUDIO_PLAYBACK_TAIL_MS + 1 && !muted);
        drains++;
    } else {
        assert(!locked && mask == (VOICE_BIT_SPEECH | VOICE_END_BITS) && wait == VOICE_FOLLOWUP_MS);
        assert(muted && (event_bits & VOICE_BIT_FOLLOWUP)
               && !(event_bits & (VOICE_BIT_RECEIVING | VOICE_BIT_CAPTURE | VOICE_BIT_LOCAL_EOS)));
        windows++;
        if (((turn_case == FOLLOWUP || turn_case == WEATHER_RECOVERS) && windows == 1) || turn_case == BUSY_WEATHER) {
            event_bits |= VOICE_BIT_SPEECH;
            if (turn_case == WEATHER_RECOVERS) event_bits |= VOICE_BIT_LOCAL_EOS;
        } else if (turn_case == GOODBYE_WINDOW) {
            on_chat_lifecycle(NULL, NULL, ESP_XIAOZHI_CHAT_EVENT_SERVER_GOODBYE, NULL);
        } else if (turn_case == WS_LOST_WINDOW) {
            on_chat_lifecycle(NULL, NULL, ESP_XIAOZHI_CHAT_EVENT_DISCONNECTED, NULL);
        }
    }
    ticks += wait;
    return *group;
}

static size_t xStreamBufferReceive(int *buffer, void *data, size_t len, TickType_t wait) {
    if (turn_case == WEATHER_RECOVERS && listens == 2) {
        assert(wait == 0 && (event_bits & VOICE_BIT_LOCAL_EOS) && !(event_bits & VOICE_BIT_CAPTURE));
        return 0;
    }
    if (turn_case == LOCAL_ENDPOINT && injected) {
        assert(wait == 0 && (event_bits & VOICE_BIT_LOCAL_EOS));
        return 0;
    }
    assert(buffer == microphone_stream && wait == VOICE_CAPTURE_POLL_MS && listens && !injected);
    injected = true;
    ticks += wait;
    if (turn_case == LOCAL_ENDPOINT) {
        for (size_t i = 0; i < len / sizeof(int16_t) / 2; i++) ((int16_t *)data)[i] = 1;
        event_bits = (event_bits & ~VOICE_BIT_CAPTURE) | VOICE_BIT_LOCAL_EOS;
        return len / 2;
    }
    if (turn_case == DROP_LISTEN) {
        event_bits = (event_bits & ~VOICE_BIT_NET) | VOICE_BIT_RESET;
        return len; /* A complete frame was buffered before the disconnect. */
    }
    if (turn_case == GOODBYE_LISTEN) {
        on_chat_lifecycle(NULL, NULL, ESP_XIAOZHI_CHAT_EVENT_SERVER_GOODBYE, NULL);
        return len;
    }
    if (turn_case == BEFORE_TTS) packet(7);
    tts(ESP_XIAOZHI_CHAT_TTS_STATE_START);
    switch (turn_case) {
    case BEFORE_TTS:
        break;
    case BARGE:
        packet(listens == 1 ? 7 : 9);
        if (listens == 1) packet(8);
        break;
    case BURST:
    case OVERFLOW:
        for (unsigned i = 0; i < VOICE_RX_QUEUE_LEN + (turn_case == OVERFLOW); i++) packet(7 + i);
        break;
    case NO_FIRST:
    case GOODBYE_PLAY:
    case EMPTY_RESULT:
    case SLOW_FIRST:
        break;
    default:
        packet(listens == 1 ? 7 : 9);
        break;
    }
    if (turn_case != NO_FIRST && turn_case != SLOW_FIRST && turn_case != STALL && turn_case != DROP_PLAY && turn_case != GOODBYE_PLAY)
        tts(ESP_XIAOZHI_CHAT_TTS_STATE_STOP);
    (void)data;
    (void)len;
    return 0;
}

static int xQueueReceive(struct voice_packet *q, struct voice_packet *packet_out, TickType_t wait) {
    assert(q == queue && wait == VOICE_PLAYBACK_POLL_MS);
    ticks += wait;
    if (turn_case == DROP_PLAY && !(event_bits & VOICE_BIT_RESET)) {
        event_bits = (event_bits & ~VOICE_BIT_NET) | VOICE_BIT_RESET;
        packet(8);
        assert(count == 1); /* Only the frame buffered before disconnect remains. */
    }
    if (turn_case == GOODBYE_PLAY && !(event_bits & VOICE_BIT_HANGUP)) {
        on_chat_lifecycle(NULL, NULL, ESP_XIAOZHI_CHAT_EVENT_SERVER_GOODBYE, NULL);
    }
    if (turn_case == SLOW_FIRST && !writes && ticks >= VOICE_FIRST_AUDIO_TIMEOUT_MS) {
        packet(7);
        tts(ESP_XIAOZHI_CHAT_TTS_STATE_STOP);
    }
    if (!count) return pdFALSE;
    *packet_out = q[0];
    memmove(q, q + 1, --count * sizeof(*q));
    return pdTRUE;
}

esp_err_t voice_settings_load(voice_settings_t *settings) {
    if (info_case != NO_SETTINGS) strcpy(settings->info_url, "https://fixture.invalid/info");
    settings->followup = 0; /* Older firmware had a switch; its saved value must be ignored. */
    return ESP_OK;
}
esp_err_t esp_xiaozhi_chat_get_info_with_url(const char *url, esp_xiaozhi_chat_info_t *info) {
    assert(!strcmp(url, "https://fixture.invalid/info"));
    info_requests++;
    info->has_mqtt_config = true; /* App still selects WebSocket. */
    info->has_websocket_config = info_case != NO_WS;
    info->has_activation_challenge = info_case == CHALLENGE;
    info->has_activation_code = info_case == CODE || info_case == EMPTY_CODE;
    if (info->has_activation_code) {
        info->activation_code = strdup(info_case == CODE ? code : "");
        assert(info->activation_code);
    }
    return info_case == INFO_FAILURE ? ESP_FAIL : ESP_OK;
}
esp_err_t esp_xiaozhi_chat_free_info(esp_xiaozhi_chat_info_t *info) {
    free(info->activation_code);
    *info = (esp_xiaozhi_chat_info_t){0};
    info_frees++;
    return ESP_OK;
}
esp_err_t esp_xiaozhi_chat_init(esp_xiaozhi_chat_config_t *config, esp_xiaozhi_chat_handle_t *chat) {
    assert(config->has_websocket_config && !config->has_mqtt_config && !config->owns_mcp_engine);
    assert(config->audio_callback == on_audio && config->event_callback == on_event && config->mcp_engine == &engine);
    chat_inits++;
    if (info_case == CHAT_INIT_ERROR) return ESP_FAIL;
    *chat = 1;
    return ESP_OK;
}
esp_err_t esp_xiaozhi_chat_start(esp_xiaozhi_chat_handle_t chat) {
    assert(chat == 1);
    chat_starts++;
    // The SDK posts CONNECTED from its own task; the stub delivers it before we wait.
    if (info_case != CONNECT_TIMEOUT) on_chat_lifecycle(NULL, NULL, ESP_XIAOZHI_CHAT_EVENT_CONNECTED, NULL);
    return ESP_OK;
}
esp_err_t esp_xiaozhi_chat_stop(esp_xiaozhi_chat_handle_t chat) { assert(chat == 1); chat_stops++; return ESP_OK; }
esp_err_t esp_xiaozhi_chat_deinit(esp_xiaozhi_chat_handle_t chat) { assert(chat == 1); return ESP_OK; }
esp_err_t esp_xiaozhi_chat_open_audio_channel(esp_xiaozhi_chat_handle_t chat, const esp_xiaozhi_chat_audio_t *audio,
                                             char *msg, size_t len) {
    assert(chat == 1 && audio->sample_rate == VOICE_SAMPLE_RATE && audio->frame_duration == VOICE_FRAME_MS);
    assert(audio->channels == 1 && !strcmp(audio->format, "opus") && !msg && !len);
    return ESP_OK;
}
esp_err_t esp_xiaozhi_chat_send_wake_word(esp_xiaozhi_chat_handle_t chat, const char *word) {
    assert(chat == 1 && word == wake_word);
    return ESP_OK;
}
esp_err_t esp_xiaozhi_chat_send_start_listening(esp_xiaozhi_chat_handle_t chat, int mode) {
    assert(chat == 1 && mode == ESP_XIAOZHI_CHAT_LISTENING_MODE_AUTO);
    assert(locked);
    if (listens && turn_case == FOLLOWUP) assert(event_bits & VOICE_BIT_CAPTURE);
    listens++;
    injected = false;
    return ESP_OK;
}
esp_err_t esp_xiaozhi_chat_send_stop_listening(esp_xiaozhi_chat_handle_t chat) {
    assert(chat == 1);
    if (turn_case == LOCAL_ENDPOINT || (turn_case == WEATHER_RECOVERS && listens == 2)) {
        tts(ESP_XIAOZHI_CHAT_TTS_STATE_START);
        packet(listens == 1 ? 7 : 9);
        tts(ESP_XIAOZHI_CHAT_TTS_STATE_STOP);
    }
    return ESP_OK;
}
esp_err_t esp_xiaozhi_chat_send_audio_data(esp_xiaozhi_chat_handle_t chat, const char *data, size_t len) {
    assert(chat == 1 && len == 1 && data[0] == 42);
    uploads++;
    return ESP_OK;
}
esp_err_t esp_xiaozhi_chat_close_audio_channel(esp_xiaozhi_chat_handle_t chat) {
    assert(chat == 1 && !(event_bits & VOICE_BIT_RECEIVING));
    closes++;
    return ESP_OK;
}
esp_err_t esp_xiaozhi_chat_send_abort_speaking(esp_xiaozhi_chat_handle_t chat, esp_xiaozhi_chat_abort_speaking_reason_t reason) {
    assert(chat == 1 && listens == 1 && reason == ESP_XIAOZHI_CHAT_ABORT_SPEAKING_REASON_WAKE_WORD_DETECTED);
    tts(ESP_XIAOZHI_CHAT_TTS_STATE_STOP);
    assert(!(event_bits & VOICE_BIT_TTS_STOPPED));
    aborts++;
    return ESP_OK;
}

static void pairing(enum info_case reply, const char *value, esp_err_t result, voice_state_t state,
                    const char *displayed, unsigned expected_inits) {
    info_case = reply;
    code = value;
    unsigned previous = chat_inits;
    assert(open_session() == result);
    assert(info_requests == info_frees && chat_inits - previous == expected_inits);
    voice_feedback_t snapshot = voice_feedback_get();
    assert(snapshot.state == state && !strcmp(snapshot.activation_code, displayed));
    assert((session.chat && session.mcp && session.encoder && session.decoder) == (result == ESP_OK));
}

static void test_pairing(void) {
    pairing(NO_SETTINGS, NULL, ESP_ERR_INVALID_STATE, VOICE_STATE_NEED_SETUP, "", 0);
    assert(!info_requests);
    pairing(INFO_FAILURE, NULL, ESP_FAIL, VOICE_STATE_ERROR, "", 0);
    pairing(NO_WS, NULL, ESP_ERR_INVALID_STATE, VOICE_STATE_ERROR, "", 0);
    pairing(CHALLENGE, NULL, ESP_ERR_INVALID_STATE, VOICE_STATE_ERROR, "", 0);
    pairing(EMPTY_CODE, NULL, ESP_ERR_INVALID_STATE, VOICE_STATE_NEED_SETUP, "", 0);
    pairing(CODE, "123456", ESP_ERR_INVALID_STATE, VOICE_STATE_ACTIVATION, "123456", 0);
    pairing(INFO_FAILURE, NULL, ESP_FAIL, VOICE_STATE_ACTIVATION, "123456", 0);
    pairing(CHALLENGE, NULL, ESP_ERR_INVALID_STATE, VOICE_STATE_ACTIVATION, "123456", 0);
    pairing(CODE, "654321", ESP_ERR_INVALID_STATE, VOICE_STATE_ACTIVATION, "654321", 0);
    pairing(CODE, "bad code", ESP_ERR_INVALID_STATE, VOICE_STATE_NEED_SETUP, "", 0);
    pairing(CHAT_INIT_ERROR, NULL, ESP_FAIL, VOICE_STATE_ERROR, "", 1);
    assert(mcp_destroys == 1 && !chat_starts && !chat_stops);
    pairing(CODE, "654321", ESP_ERR_INVALID_STATE, VOICE_STATE_ACTIVATION, "654321", 0);
    pairing(BOUND, NULL, ESP_OK, VOICE_STATE_ACTIVATION, "654321", 1);
    assert(chat_starts == 1);
    close_session();
    assert(chat_stops == 1 && mcp_destroys == 2 && !session.chat && !session.mcp && !session.encoder && !session.decoder);
    // A socket that never connects must not produce a `chat ready` the wake word cannot use.
    info_case = CONNECT_TIMEOUT;
    event_bits = 0;
    assert(open_session() == ESP_ERR_TIMEOUT);
    assert(chat_starts == 2 && chat_stops == 2 && !session.chat && !session.mcp && !session.encoder && !session.decoder);
    // A retained activation code deliberately survives this failure with a retryable state.
    assert(voice_feedback_get().state == VOICE_STATE_ACTIVATION);
    puts("PASS 14 pairing/setup cases");
}

static void test_turn(enum turn_case scenario) {
    turn_case = scenario;
    info_case = BOUND;
    event_bits = VOICE_BIT_ENABLED | VOICE_BIT_NET | VOICE_BIT_CAPTURE;
    count = writes = listens = closes = aborts = ticks = windows = drains = lock_attempts = uploads = 0;
    locked = true;
    assert(open_session() == ESP_OK);
    packet(1);  /* No active audio channel yet. */
    assert(count == 0);
    queue[count++] = (struct voice_packet){.len = 1, .data = {2}}; /* Stale data must be cleared. */
    esp_err_t result = run_turn();
    if (scenario == DROP_LISTEN || scenario == DROP_PLAY) {
        assert(result == ESP_FAIL && !writes);
    } else if (scenario == OVERFLOW) {
        /* Overflow drops excess packets gracefully instead of tearing down the session. */
        assert(result == ESP_OK && writes == VOICE_RX_QUEUE_LEN && drains == 1);
        for (unsigned i = 0; i < VOICE_RX_QUEUE_LEN; i++) assert(played[i] == (int16_t)(7 + i));
    } else if (scenario == NO_FIRST) {
        assert(result == ESP_ERR_TIMEOUT && !writes && ticks >= VOICE_FIRST_AUDIO_TIMEOUT_MS);
    } else if (scenario == STALL) {
        assert(result == ESP_ERR_TIMEOUT && writes == 1 && played[0] == 7);
        assert(ticks >= VOICE_RESPONSE_TIMEOUT_MS && ticks < VOICE_FIRST_AUDIO_TIMEOUT_MS);
    } else if (scenario == FOLLOWUP || scenario == WEATHER_RECOVERS) {
        assert(result == ESP_OK && !aborts && listens == 2 && writes == 2 && windows == 2);
        assert(played[0] == 7 && played[1] == 9 && drains == 2);
        if (scenario == WEATHER_RECOVERS) assert(lock_attempts > 3);
    } else if (scenario == GOODBYE_LISTEN || scenario == GOODBYE_PLAY) {
        assert(result == ESP_FAIL && !writes && !windows && ticks < 200);
        assert(event_bits & VOICE_BIT_HANGUP);
    } else if (scenario == GOODBYE_WINDOW || scenario == WS_LOST_WINDOW) {
        assert(result == (scenario == WS_LOST_WINDOW ? ESP_FAIL : ESP_OK));
        assert(writes == 1 && windows == 1 && listens == 1);
    } else if (scenario == EMPTY_RESULT) {
        assert(result == ESP_OK && !writes && !windows);
    } else if (scenario == SILENT || scenario == BUSY_WEATHER) {
        assert(result == ESP_OK && !aborts && listens == 1 && writes == 1 && windows == 1);
        if (scenario == BUSY_WEATHER) assert(ticks >= VOICE_FOLLOWUP_LOCK_MS);
    } else if (scenario == BARGE) {
        assert(result == ESP_OK && aborts == 1 && listens == 2 && writes == 2);
        assert(played[0] == 7 && played[1] == 9);
    } else {
        unsigned expected = scenario == BURST ? VOICE_RX_QUEUE_LEN : 1;
        assert(result == ESP_OK && writes == expected && drains == 1);
        if (scenario == LOCAL_ENDPOINT) assert(uploads == 1);
        for (unsigned i = 0; i < expected; i++) assert(played[i] == (int16_t)(7 + i));
    }
    bool remote_end = scenario == DROP_LISTEN || scenario == DROP_PLAY || scenario == GOODBYE_LISTEN
                      || scenario == GOODBYE_PLAY || scenario == GOODBYE_WINDOW || scenario == WS_LOST_WINDOW;
    assert(closes == !remote_end && !count && muted && locked);
    assert(!(event_bits & (VOICE_BIT_RECEIVING | VOICE_BIT_CAPTURE | VOICE_BIT_FOLLOWUP | VOICE_BIT_SPEECH
                          | VOICE_BIT_LOCAL_EOS)));
    close_session();
}

static void test_capture(void) {
    int16_t frames[9 * FOLLOWUP_SAMPLES];
    int16_t history[FOLLOWUP_HISTORY_FRAMES][FOLLOWUP_SAMPLES];
    capture_state.vad = &mic;
    capture_state.vad_history = history;
    for (size_t i = 0; i < sizeof(frames) / sizeof(*frames); i++) frames[i] = i / FOLLOWUP_SAMPLES + 1;
    event_bits = VOICE_BIT_ENABLED | VOICE_BIT_NET | VOICE_BIT_FOLLOWUP;
    vad_trigger_frame = 8;
    detect_followup(frames, 3);
    detect_followup(frames + 3, 7 * FOLLOWUP_SAMPLES - 3);
    assert(!captured_samples && !(event_bits & VOICE_BIT_SPEECH));
    detect_followup(frames + 7 * FOLLOWUP_SAMPLES, FOLLOWUP_SAMPLES + 5);
    assert((event_bits & VOICE_BIT_SPEECH) && !(event_bits & VOICE_BIT_CAPTURE));
    assert(captured_samples == FOLLOWUP_HISTORY_FRAMES * FOLLOWUP_SAMPLES + 5);
    assert(memcmp(captured, frames + 4 * FOLLOWUP_SAMPLES, captured_samples * sizeof(*captured)) == 0);
    event_bits = VOICE_BIT_CAPTURE;
    capture_state.endpoint_heard_speech = false; /* New WakeNet-triggered question. */
    vad_trigger_frame = vad_frames + 2;
    detect_endpoint(frames, FOLLOWUP_SAMPLES);
    assert(event_bits == VOICE_BIT_CAPTURE); /* Silence before speech cannot end a turn. */
    detect_endpoint(frames, FOLLOWUP_SAMPLES - 1);
    assert(!capture_state.endpoint_heard_speech);
    detect_endpoint(frames, 1);
    assert(event_bits == VOICE_BIT_CAPTURE && capture_state.endpoint_heard_speech);
    detect_endpoint(frames, FOLLOWUP_SAMPLES);
    assert(event_bits == VOICE_BIT_LOCAL_EOS);

    event_bits = VOICE_BIT_CAPTURE;
    capture_state.endpoint_heard_speech = false;
    unsigned wake_frames = VOICE_WAKE_MIN_CAPTURE_MS / FOLLOWUP_FRAME_MS;
    capture_state.endpoint_holdoff_frames = wake_frames;
    vad_trigger_frame = vad_frames + 1; /* The wake word's last samples count as speech. */
    for (unsigned i = 1; i < wake_frames; i++) {
        detect_endpoint(frames, FOLLOWUP_SAMPLES);
        assert(event_bits == VOICE_BIT_CAPTURE);
    }
    detect_endpoint(frames, FOLLOWUP_SAMPLES);
    assert(event_bits == VOICE_BIT_LOCAL_EOS);
    puts("PASS local VAD: ordered pre-roll, speech endpoint and minimum capture after wake");
}

static void test_weather_priority(void) {
    event_bits = VOICE_BIT_ENABLED | VOICE_BIT_NET | VOICE_BIT_FOLLOWUP;
    assert(voice_weather_begin() && locked);
    assert(!voice_weather_begin());
    voice_weather_end();
    const EventBits_t pending[] = {VOICE_BIT_TRIGGER, VOICE_BIT_SPEECH, VOICE_BIT_CAPTURE, VOICE_BIT_RECEIVING};
    for (unsigned i = 0; i < sizeof(pending) / sizeof(*pending); i++) {
        event_bits = pending[i];
        assert(!voice_weather_begin() && !locked);
    }
    puts("PASS idle follow-up allows weather and pending speech wins the network lock");
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "pairing")) {
        test_pairing();
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "capture")) {
        test_capture();
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "weather-priority")) {
        test_weather_priority();
        return 0;
    }
    const char *names[] = {"early", "before-tts", "burst", "overflow", "slow-first", "no-first", "stall",
                           "drop-listen", "drop-play", "barge", "followup", "silent", "busy-weather", "goodbye-listen",
                           "goodbye-play", "goodbye-window", "ws-lost-window", "empty-result",
                           "weather-recovers", "local-endpoint"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(*names); i++) {
        if (argc == 1 || !strcmp(argv[1], names[i])) {
            fprintf(stderr, "Testing %s\n", names[i]);
            test_turn((enum turn_case)i);
        }
    }
    puts("PASS audio turn regression");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("scenario", nargs="?", choices=("pairing", "capture", "weather-priority", "early",
                        "before-tts", "burst", "overflow",
                        "slow-first", "no-first", "stall", "drop-listen", "drop-play", "barge",
                        "followup", "silent", "busy-weather", "goodbye-listen", "goodbye-play",
                        "goodbye-window", "ws-lost-window", "empty-result", "weather-recovers", "local-endpoint"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = root / "main/voice.c"
    functions, _ = extract(source.read_text(encoding="utf-8") + "\n/* end of voice code */\n", source, BOUNDARIES)
    capture = root / "main/voice_capture.c"
    capture_source = capture.read_text(encoding="utf-8")
    macros = re.findall(r"^#define FOLLOWUP_[A-Z_]+ [^\n]+", capture_source, re.MULTILINE)
    assert len(macros) == 3
    capture_functions, _ = extract(capture_source, capture, (
        ("static void buffer_microphone(const int16_t *mono, size_t samples)",
         "static void detect_followup(const int16_t *mono, size_t samples)"),
        ("static void detect_followup(const int16_t *mono, size_t samples)",
         "static void detect_endpoint(const int16_t *mono, size_t samples)"),
        ("static void detect_endpoint(const int16_t *mono, size_t samples)",
         "// Capture owns speech detection. Only the session task can start network capture."),
    ))
    flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"] if args.sanitize else []
    with tempfile.TemporaryDirectory(prefix="weather-test-voice-session-") as workspace:
        work = Path(workspace)
        for name, contents in STUB_HEADERS.items():
            header = work / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text(contents, encoding="utf-8")
        harness = work / "session.c"
        harness.write_text("\n".join((*macros, PRELUDE, functions, capture_functions, HARNESS)), encoding="utf-8")
        exe = work / "session.out"
        env = dict(os.environ, LC_ALL="C", ASAN_OPTIONS="halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1")
        run([shutil.which("cc") or "cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror",
             "-I", work, "-I", root / "main", "-I", root / "components/esp_xiaozhi/include", *flags,
             harness, root / "main/voice_audio.c", root / "main/voice_feedback.c", "-o", exe], work, env, 60)
        routes = [[args.scenario]] if args.scenario else [["pairing"], ["capture"], ["weather-priority"], []]
        for route in routes:
            print(run([exe, *route], work, env, 10), end="", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (OSError, AssertionError) as error:
        sys.exit(f"FAIL: {error}")
