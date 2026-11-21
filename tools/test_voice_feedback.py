#!/usr/bin/env python3
"""Real feedback snapshots and UI: ownership, concurrency, activation and redraw isolation.

Uses the existing glyph-checking UI harness and pthread-backed critical sections.
Does not validate the real speech SDK, server binding, audio, or physical e-paper.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_app import FIXTURES, HEADERS, UI_HARNESS, UI_WRAPPER


CRITICAL = r'''
#pragma once
#include <assert.h>
#include <pthread.h>
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(lock) assert(!pthread_mutex_lock(lock))
#define portEXIT_CRITICAL(lock) assert(!pthread_mutex_unlock(lock))
#define pdMS_TO_TICKS(ms) (ms)
'''

HARNESS = r'''
#include <pthread.h>
#include "voice_feedback.h"
enum { FRAME_BYTES = PANEL_W / 8 * PANEL_H };
static unsigned char frame[FRAME_BYTES];
int spi_device_polling_transmit(spi_device_handle_t spi, spi_transaction_t *transaction) {
    if (transaction->length == FRAME_BYTES * 8) memcpy(frame, transaction->tx_buffer, sizeof(frame));
    return 0;
}
static void *publish_many(void *arg) {
    const char *code = arg;
    for (int i = 0; i < 10000; i++) {
        assert(voice_feedback_publish(VOICE_STATE_READY, NULL));
        assert(voice_feedback_publish(VOICE_STATE_ACTIVATION, code));
    }
    return NULL;
}
static void *read_many(void *arg) {
    for (int i = 0; i < 20000; i++) {
        voice_feedback_t feedback = voice_feedback_get();
        assert(feedback.state == VOICE_STATE_READY || feedback.state == VOICE_STATE_ACTIVATION);
        if (feedback.state == VOICE_STATE_READY) assert(!feedback.activation_code[0]);
        else assert(!strcmp(feedback.activation_code, "AAAAAA") || !strcmp(feedback.activation_code, "BBBB"));
    }
    return NULL;
}
static void isolated(const unsigned char *before, bool title) {
    for (int x = 0; x < 296; x++) for (int y = 0; y < 128; y++) {
        int px = 127 - y, index = x * 16 + (px >> 3), mask = 0x80 >> (px & 7);
        if ((before[index] ^ frame[index]) & mask)
            assert((x >= 200 && y >= 110) || (title && x < 230 && y < 22));
    }
}
static unsigned observer_calls;
static void count_observer(void) { observer_calls++; }
static bool apply(voice_state_t state, const char *code) {
    assert(voice_feedback_publish(state, code));
    voice_feedback_t feedback = voice_feedback_get();
    return ui_update_voice(&feedback);
}
int main(void) {
    assert(voice_feedback_get().state == VOICE_STATE_DISABLED);
    voice_feedback_set_observer(count_observer);
    assert(voice_feedback_publish(VOICE_STATE_READY, NULL));
    assert(observer_calls == 1);
    assert(voice_feedback_publish(VOICE_STATE_READY, NULL));
    assert(observer_calls == 1); /* Duplicate publish does not spuriously wake display. */
    char source[] = "123456";
    assert(voice_feedback_publish(VOICE_STATE_ACTIVATION, source));
    assert(observer_calls == 2);
    memset(source, 'x', 6);
    voice_feedback_t copy = voice_feedback_get();
    assert(!strcmp(copy.activation_code, "123456"));
    copy.activation_code[0] = 'x';
    assert(!strcmp(voice_feedback_get().activation_code, "123456"));
    assert(voice_feedback_publish(VOICE_STATE_CONNECTING, NULL));
    assert(voice_feedback_publish(VOICE_STATE_ERROR, NULL));
    assert(voice_feedback_publish(VOICE_STATE_OFFLINE, NULL));
    assert(observer_calls == 2); /* Ignored outage/retry preserves activation without waking. */
    assert(voice_feedback_get().state == VOICE_STATE_ACTIVATION);
    const char *invalid[] = {NULL, "", "12345678901234567", "12 34", "12\n34", "中文"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        assert(!voice_feedback_publish(VOICE_STATE_ACTIVATION, invalid[i]));
        assert(!strcmp(voice_feedback_get().activation_code, "123456"));
    }
    assert(!voice_feedback_publish((voice_state_t)-1, NULL));
    assert(!voice_feedback_publish((voice_state_t)100, NULL));
    assert(!voice_feedback_publish(VOICE_STATE_READY, "123456"));
    assert(voice_feedback_publish(VOICE_STATE_WAIT_NETWORK, NULL));
    assert(observer_calls == 3);
    assert(!voice_feedback_get().activation_code[0]);
    voice_feedback_set_observer(NULL);
    for (int state = VOICE_STATE_DISABLED; state <= VOICE_STATE_ERROR; state++) {
        assert(voice_feedback_publish((voice_state_t)state, state == VOICE_STATE_ACTIVATION ? "123456" : NULL));
    }
    assert(voice_feedback_publish(VOICE_STATE_READY, NULL));
    pthread_t threads[4];
    assert(!pthread_create(&threads[0], NULL, publish_many, "AAAAAA"));
    assert(!pthread_create(&threads[1], NULL, publish_many, "BBBB"));
    assert(!pthread_create(&threads[2], NULL, read_many, NULL));
    assert(!pthread_create(&threads[3], NULL, read_many, NULL));
    for (int i = 0; i < 4; i++) assert(!pthread_join(threads[i], NULL));

    assert(voice_feedback_publish(VOICE_STATE_READY, NULL));
    setenv("TZ", "UTC0", 1); tzset();
    weather_t weather = sample_weather(0); sensor_reading_t sensor = {0};
    epd_init(); ui_draw_weather(&weather, &sensor); ui_draw_status(UI_STATUS_UPDATED, 0); epd_refresh();
    unsigned char before[FRAME_BYTES], expected[FRAME_BYTES];
    const voice_state_t states[] = {VOICE_STATE_READY, VOICE_STATE_CONNECTING, VOICE_STATE_NEED_SETUP,
        VOICE_STATE_LISTENING, VOICE_STATE_FOLLOWUP, VOICE_STATE_WAITING, VOICE_STATE_SPEAKING,
        VOICE_STATE_OFFLINE, VOICE_STATE_ERROR, VOICE_STATE_DISABLED, VOICE_STATE_WAIT_NETWORK};
    for (unsigned i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        memcpy(before, frame, sizeof(before));
        assert(apply(states[i], NULL)); epd_refresh(); isolated(before, false);
        assert(!apply(states[i], NULL));
    }
    /* Ready voice yields to active connecting/fetching/syncing/retry weather states, then resumes. */
    assert(apply(VOICE_STATE_READY, NULL)); epd_refresh();
    const ui_status_t priority[] = {UI_STATUS_CONNECTING, UI_STATUS_FETCHING, UI_STATUS_SYNCING, UI_STATUS_RETRY};
    for (unsigned i = 0; i < sizeof(priority) / sizeof(priority[0]); i++) {
        memcpy(before, frame, sizeof(before));
        assert(ui_update_status(priority[i], i + 1)); epd_refresh(); isolated(before, false);
        assert(!ui_update_status(priority[i], i + 1));
    }
    memcpy(before, frame, sizeof(before));
    assert(ui_update_status(UI_STATUS_UPDATED, 0)); epd_refresh(); isolated(before, false);
    assert(apply(VOICE_STATE_WAIT_NETWORK, NULL)); epd_refresh();
    memcpy(before, frame, sizeof(before));
    assert(apply(VOICE_STATE_ACTIVATION, "123456")); epd_refresh(); isolated(before, true);
    assert(!apply(VOICE_STATE_ACTIVATION, "123456"));
    memcpy(before, frame, sizeof(before));
    assert(apply(VOICE_STATE_ACTIVATION, "654321")); epd_refresh(); isolated(before, true);
    assert(apply(VOICE_STATE_ACTIVATION, "WWWWWWWWWWWWWWWW")); epd_refresh();
    ui_draw_hourly(&weather, &sensor); ui_draw_status(UI_STATUS_UPDATED, 0); epd_refresh();
    memcpy(before, frame, sizeof(before));
    assert(apply(VOICE_STATE_READY, NULL)); epd_refresh(); isolated(before, true);
    memcpy(expected, frame, sizeof(expected));
    ui_draw_hourly(&weather, &sensor); ui_draw_status(UI_STATUS_UPDATED, 0); epd_refresh();
    assert(!memcmp(expected, frame, sizeof(frame))); /* Restored title equals a complete redraw. */
    ui_draw_retry(12, 60); epd_refresh();
    assert(apply(VOICE_STATE_ACTIVATION, "123456")); epd_refresh();
    ui_draw_retry(13, 60); epd_refresh();
    assert(apply(VOICE_STATE_READY, NULL)); epd_refresh();
    memcpy(expected, frame, sizeof(expected));
    ui_draw_retry(13, 60); epd_refresh();
    assert(!memcmp(expected, frame, sizeof(frame))); /* Restore the latest retry, not old weather. */
    ui_draw_portal_hint(); epd_refresh(); memcpy(before, frame, sizeof(before));
    assert(!apply(VOICE_STATE_ACTIVATION, "123456")); epd_refresh();
    assert(!memcmp(before, frame, sizeof(frame)));
    assert(!apply(VOICE_STATE_LISTENING, NULL));
    ui_draw_weather(&weather, &sensor); ui_draw_status(UI_STATUS_UPDATED, 0); epd_refresh();
    assert(!apply(VOICE_STATE_LISTENING, NULL));
    assert(!ui_update_voice(NULL));
    puts("PASS feedback: code ownership/validation, sticky pairing, concurrent snapshots, all states, isolated pixels, title restoration, page switches, retry and setup precedence");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    project = Path(__file__).resolve().parents[1]
    compiler = shutil.which("cc")
    assert compiler, "host C compiler is required"
    with tempfile.TemporaryDirectory(prefix="weather-feedback-") as directory:
        work = Path(directory)
        for name, contents in HEADERS.items():
            path = work / "stubs" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(CRITICAL if name == "freertos/FreeRTOS.h" else contents)
        (work / "fixtures.h").write_text(FIXTURES)
        (work / "ui_wrapper.c").write_text(UI_WRAPPER)
        (work / "feedback.c").write_text(UI_HARNESS.split("int spi_device_polling_transmit(")[0] + HARNESS)
        flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"] if args.sanitize else []
        sources = ["voice_feedback.c", "epd.c", "fonts_aa.c", "font_clock.c", "font_icons.c"]
        subprocess.run([
            compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-unused-function",
            "-pthread", *flags, "-I", str(work / "stubs"), "-I", str(project / "main"),
            str(work / "ui_wrapper.c"), *(str(project / "main" / name) for name in sources),
            str(work / "feedback.c"), "-lm", "-o", str(work / "feedback"),
        ], check=True, timeout=60)
        result = subprocess.run([str(work / "feedback")], capture_output=True, text=True, timeout=20)
        if result.returncode:
            raise RuntimeError(result.stderr + result.stdout[-1500:])
        labels = {line.split(":", 9)[-1] for line in result.stdout.splitlines()
                  if line.startswith("text:") and line.split(":", 9)[2] == "124"}
        assert {"语音连接", "设置语音", "待唤醒", "正在聆听", "等待追问", "等待回答", "正在说话",
                "无网络", "语音失败", "语音不可用", "等待激活", "正在联网", "正在取数", "同步时间",
                "重试 #4"} <= labels, labels
        print(result.stdout.splitlines()[-1])


if __name__ == "__main__":
    main()
