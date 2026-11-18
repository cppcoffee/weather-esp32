#pragma once

#include <stdint.h>

// Shared between the capture task (voice_capture.c) and the session task (voice.c). The two
// tasks meet only through one event group and two stream buffers, so the audio format, the
// buffer sizes and the event-bit contract live in this one header.

#define VOICE_SAMPLE_RATE            16000
#define VOICE_FRAME_MS               60
#define VOICE_FRAME_SAMPLES          (VOICE_SAMPLE_RATE / 1000 * VOICE_FRAME_MS)
#define VOICE_TX_OPUS_MAX            512
#define VOICE_RX_OPUS_MAX            1400
#define VOICE_RX_QUEUE_LEN           8
#define VOICE_TASK_STACK             8192
#define VOICE_TASK_PRIO              5
#define VOICE_TURN_TIMEOUT_MS        15000
#define VOICE_RESPONSE_TIMEOUT_MS    30000
#define VOICE_FIRST_AUDIO_TIMEOUT_MS 90000
#define VOICE_RETRY_MS               30000
#define VOICE_POLL_MS                100
#define VOICE_FOLLOWUP_MS            6000
// Shorter poll while the reply plays so a wake word stops the speaker promptly.
#define VOICE_PLAYBACK_POLL_MS 20
#define WAKE_TASK_STACK        4096
#define WAKE_BUFFER_BYTES      (VOICE_SAMPLE_RATE * sizeof(int16_t) * 12)
// One second of playback reference; the capture task drains it at microphone pace.
#define VOICE_REF_BYTES (VOICE_SAMPLE_RATE * sizeof(int16_t))

#define VOICE_BIT_NET         (1 << 0)
#define VOICE_BIT_TRIGGER     (1 << 1)
#define VOICE_BIT_TTS_STARTED (1 << 2)
#define VOICE_BIT_TTS_STOPPED (1 << 3)
#define VOICE_BIT_ERROR       (1 << 4)
#define VOICE_BIT_RECEIVING   (1 << 5)
#define VOICE_BIT_RESET       (1 << 6)
#define VOICE_BIT_ARMED       (1 << 7)
#define VOICE_BIT_CAPTURE     (1 << 8)
#define VOICE_BIT_HANGUP      (1 << 9)
#define VOICE_BIT_INTERRUPT   (1 << 10)
#define VOICE_BIT_ENABLED     (1 << 11)
#define VOICE_BIT_FOLLOWUP    (1 << 12)
#define VOICE_BIT_SPEECH      (1 << 13)
#define VOICE_BIT_WS_LOST     (1 << 14)
#define VOICE_TURN_BITS       (VOICE_BIT_TTS_STARTED | VOICE_BIT_TTS_STOPPED | VOICE_BIT_ERROR | VOICE_BIT_WS_LOST)
#define VOICE_END_BITS        (VOICE_BIT_RESET | VOICE_BIT_HANGUP | VOICE_BIT_ERROR | VOICE_BIT_WS_LOST)

// Both tasks use the same rule: voice is enabled, STA has IP, and teardown is complete.
#define VOICE_NETWORK_READY(bits)                                                                                      \
    (((bits) & (VOICE_BIT_ENABLED | VOICE_BIT_NET | VOICE_BIT_RESET)) == (VOICE_BIT_ENABLED | VOICE_BIT_NET))
