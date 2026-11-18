#!/usr/bin/env python3
"""Bounded host regression of selected, UNMODIFIED functions from main/voice.c.

Extracts the STA event handler and voice lifecycle functions by exact signatures
and known next-function boundaries. Uses the production voice_internal.h. This is NOT a build/test of all voice.c: open_session and run_turn,
RTOS, protocol, audio and Opus are stubs. In particular it does not validate real
SDK stop timing, audio, the voice_capture.c task, WakeNet detection, provisioning,
or concurrency.

Only Python stdlib and a host cc/clang/gcc are needed. Generated C/executables are
confined to TemporaryDirectory. Event waits inject a finite per-iteration script;
setjmp/longjmp exits only at an unlocked outer-loop boundary.

    python3 tools/test_voice_lifecycle.py
    python3 tools/test_voice_lifecycle.py --sanitize
"""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


BOUNDARIES = (
    ("static bool network_ready(void)", "static void on_audio(const uint8_t *data, int len, void *ctx)"),
    ("static void close_session(void)", "static esp_err_t open_session(void)"),
    ("static void voice_task(void *arg)", "esp_err_t voice_init(void)"),
    ("void voice_net_ready(void)", "void voice_net_set_connected(bool connected)"),
    ("void voice_net_set_connected(bool connected)", "void voice_net_pause(void)"),
    ("void voice_net_pause(void)", "bool voice_weather_begin(void)"),
)


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def extract(source, path, boundaries):
    fragments = []
    ranges = []

    def append(text, start):
        line = source.count("\n", 0, start) + 1
        fragments.append(f"#line {line} {json.dumps(str(path))}\n" + text)
        ranges.append(f"{text.splitlines()[0]} [L{line}–{line + text.count(chr(10)) - 1}]")

    for signature, following in boundaries:
        starts = list(re.finditer(r"^" + re.escape(signature) + r"\n", source, re.MULTILINE))
        ends = list(re.finditer(r"^" + re.escape(following) + r"\n", source, re.MULTILINE))
        require(len(starts) == len(ends) == 1, f"function boundary changed: {signature} -> {following}")
        start, end = starts[0].start(), ends[0].start()
        body = source[start:end]
        require(
            start < end and body.startswith(signature + "\n{\n")
            and body.endswith("\n}\n\n") and body.count("\n}\n") == 1,
            f"unexpected function shape/boundary: {signature}; review extraction explicitly",
        )
        append(body, start)

    return "\n".join(fragments), ranges


PRELUDE = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <string.h>
#include "voice_feedback.h"
#include "voice_internal.h"

bool voice_feedback_publish(voice_state_t state, const char *code) {
    (void)state; (void)code; return true;
}

static const char *context = "setup";
#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL [%s] %s:%d: %s\n", context, __FILE__, __LINE__, #x); exit(1); \
} } while (0)
typedef int esp_err_t;
typedef uint32_t EventBits_t;
typedef unsigned TickType_t;
typedef uint32_t esp_xiaozhi_chat_handle_t;
typedef struct { unsigned generation; } esp_mcp_t, OpusEncoder, OpusDecoder;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)

enum { WIFI_EVENT, IP_EVENT };
enum { WIFI_EVENT_STA_START, WIFI_EVENT_STA_DISCONNECTED, WIFI_EVENT_STA_STOP, IP_EVENT_STA_GOT_IP };
typedef int esp_event_base_t;
#define GOT_IP 1u
static EventBits_t wifi_events;
static EventBits_t events;
static unsigned wifi_connects;
static void esp_wifi_connect(void) { wifi_connects++; }
static EventBits_t *s_events = &events;
static int lock_token, queue_token;
static void *s_network_lock = &lock_token, *s_rx_queue = &queue_token;
static esp_xiaozhi_chat_handle_t s_chat, live_chat;
static esp_mcp_t *s_mcp, *live_mcp;
static OpusEncoder *s_encoder, *live_encoder;
static OpusDecoder *s_decoder, *live_decoder;
static bool s_cleanup_pending, locked, stopped, wake_on_wait, muted = true;
static int opens, stops, deinits, mcp_destroys, encoder_destroys, decoder_destroys;
static int turns, takes, gives, waits, delays, failures_left;
static bool deinit_error;
static jmp_buf finished;

static EventBits_t xEventGroupGetBits(EventBits_t *group);
static EventBits_t xEventGroupSetBits(EventBits_t *group, EventBits_t bits);
static EventBits_t xEventGroupClearBits(EventBits_t *group, EventBits_t bits);
static EventBits_t xEventGroupWaitBits(EventBits_t *group, EventBits_t mask,
                                     int clear, int all, TickType_t timeout);
static int xSemaphoreTake(void *lock, TickType_t timeout);
static int xSemaphoreGive(void *lock);
static void vTaskDelay(TickType_t ticks);
static int xQueueReset(void *queue);
static esp_err_t audio_set_mute(bool mute);
static esp_err_t esp_xiaozhi_chat_stop(esp_xiaozhi_chat_handle_t chat);
static esp_err_t esp_xiaozhi_chat_deinit(esp_xiaozhi_chat_handle_t chat);
static esp_err_t esp_mcp_destroy(esp_mcp_t *mcp);
static void opus_encoder_destroy(OpusEncoder *encoder);
static void opus_decoder_destroy(OpusDecoder *decoder);
static esp_err_t open_session(void);
static esp_err_t run_turn(void);
'''

HARNESS = r'''
#line 1 "lifecycle_harness.c"
enum action { NET, GOODBYE, PAUSE, WAKE, IDLE, DROP, FAST_RECONNECT, IP };
static bool drop_in_turn;
static void wifi_event(esp_event_base_t base, int32_t event)
{
    on_event(NULL, base, event, NULL);
}

static void disconnect(void)
{
    wifi_event(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED);
    CHECK(!wifi_events && !network_ready());
}
struct step {
    const char *label;
    enum action action;
    int opens, stops, disposed, turns;
    bool pending;
    EventBits_t bits;
};
static struct step script[20];
static int script_count, current = -1;

static void add(const char *label, enum action action, int nopen, int nstop,
                int disposed, int nturn, bool pending, EventBits_t bits)
{
    CHECK(script_count < (int)(sizeof(script) / sizeof(script[0])));
    script[script_count++] = (struct step){label, action, nopen, nstop, disposed, nturn, pending, bits};
}

static void expect_state(const struct step *step)
{
    CHECK(!locked && takes == gives && !wake_on_wait);
    CHECK(opens == step->opens && stops == step->stops && turns == step->turns);
    CHECK(deinits == step->disposed && mcp_destroys == step->disposed);
    CHECK(encoder_destroys == step->disposed && decoder_destroys == step->disposed);
    CHECK(s_cleanup_pending == step->pending && events == step->bits);
    CHECK(muted);
    bool live = step->opens != step->disposed;
    CHECK((s_chat != 0) == live && s_chat == live_chat);
    CHECK((s_mcp != NULL) == live && s_mcp == live_mcp);
    CHECK((s_encoder != NULL) == live && s_encoder == live_encoder);
    CHECK((s_decoder != NULL) == live && s_decoder == live_decoder);
    if (live) {
        CHECK(!stopped);
        CHECK(s_chat == (unsigned)opens && s_mcp->generation == s_chat);
        CHECK(s_encoder->generation == s_chat && s_decoder->generation == s_chat);
    }
}

static void outer_boundary(void)
{
    CHECK(!locked && takes == gives);
    if (current >= 0) expect_state(&script[current]);
    if (++current == script_count) longjmp(finished, 1);
    context = script[current].label;
    switch (script[current].action) {
    case NET:
        wifi_event(IP_EVENT, IP_EVENT_STA_GOT_IP);
        voice_net_ready();
        break;
    case DROP:
        disconnect();
        break;
    case FAST_RECONNECT:
        disconnect();
        wifi_event(IP_EVENT, IP_EVENT_STA_GOT_IP);
        CHECK(wifi_events == GOT_IP && !network_ready());
        break;
    case IP:
        wifi_event(IP_EVENT, IP_EVENT_STA_GOT_IP);
        break;
    case GOODBYE:
        /* Inject callback fact, not a copy of on_server_goodbye or capture_task. */
        xEventGroupSetBits(s_events, VOICE_BIT_HANGUP | VOICE_BIT_RECEIVING);
        break;
    case PAUSE:
        voice_net_pause();
        CHECK(!(events & (VOICE_BIT_ENABLED | VOICE_BIT_NET | VOICE_BIT_TRIGGER | VOICE_BIT_RECEIVING
                          | VOICE_BIT_ARMED | VOICE_BIT_CAPTURE)));
        CHECK(events & VOICE_BIT_RESET);
        break;
    case WAKE:
        CHECK((events & VOICE_BIT_ARMED) && !s_cleanup_pending);
        wake_on_wait = true;
        break;
    case IDLE:
        break;
    }
}

static EventBits_t xEventGroupGetBits(EventBits_t *group)
{
    CHECK(group == &events || group == &wifi_events);
    return *group;
}

static EventBits_t xEventGroupSetBits(EventBits_t *group, EventBits_t bits)
{
    CHECK(group == &events || group == &wifi_events);
    return *group |= bits;
}

static EventBits_t xEventGroupClearBits(EventBits_t *group, EventBits_t bits)
{
    CHECK(group == &events || group == &wifi_events);
    EventBits_t previous = *group;
    *group &= ~bits;
    return previous;
}

static EventBits_t xEventGroupWaitBits(EventBits_t *group, EventBits_t mask,
                                     int clear, int all, TickType_t timeout)
{
    CHECK(group == &events && clear == pdFALSE && all == pdFALSE && !locked);
    CHECK(++waits <= 100); /* Hard bound even if source starts polling unexpectedly. */
    if (timeout == portMAX_DELAY) {
        EventBits_t needed = (events & VOICE_BIT_ENABLED) ? VOICE_BIT_NET : VOICE_BIT_ENABLED;
        CHECK(mask == (needed | VOICE_BIT_RESET));
        outer_boundary();
        CHECK(events & mask); /* Never pretend an infinite wait returned without an event. */
    } else {
        CHECK(mask == (VOICE_BIT_TRIGGER | VOICE_BIT_RESET)
              || mask == (VOICE_BIT_TRIGGER | VOICE_BIT_RESET | VOICE_BIT_HANGUP));
        CHECK(timeout == pdMS_TO_TICKS(VOICE_POLL_MS));
        if (wake_on_wait) {
            /* Emulate only the capture task's event handoff, not WakeNet detection. */
            CHECK(events & VOICE_BIT_ARMED);
            xEventGroupClearBits(s_events, VOICE_BIT_ARMED);
            xEventGroupSetBits(s_events, VOICE_BIT_TRIGGER | VOICE_BIT_CAPTURE);
            wake_on_wait = false;
        }
    }
    return events;
}

static int xSemaphoreTake(void *lock, TickType_t timeout)
{
    CHECK(lock == s_network_lock && !locked && timeout == pdMS_TO_TICKS(VOICE_POLL_MS));
    locked = true;
    takes++;
    return pdTRUE;
}

static int xSemaphoreGive(void *lock)
{
    CHECK(lock == s_network_lock && locked);
    locked = false;
    gives++;
    return pdTRUE;
}

static void vTaskDelay(TickType_t ticks)
{
    CHECK(!locked && takes == gives && ticks == pdMS_TO_TICKS(VOICE_POLL_MS));
    delays++;
}

static int xQueueReset(void *queue)
{
    CHECK(queue == s_rx_queue && locked);
    return pdTRUE;
}

static esp_err_t audio_set_mute(bool mute)
{
    CHECK(locked && mute);
    muted = mute;
    return ESP_OK;
}

static esp_err_t open_session(void)
{
    CHECK(locked && network_ready() && !s_cleanup_pending);
    CHECK(!s_chat && !s_mcp && !s_encoder && !s_decoder && !live_chat);
    CHECK(!live_mcp && !live_encoder && !live_decoder);
    s_mcp = live_mcp = calloc(1, sizeof(*s_mcp));
    s_encoder = live_encoder = calloc(1, sizeof(*s_encoder));
    s_decoder = live_decoder = calloc(1, sizeof(*s_decoder));
    CHECK(s_mcp && s_encoder && s_decoder);
    s_chat = live_chat = (unsigned)++opens;
    s_mcp->generation = s_encoder->generation = s_decoder->generation = s_chat;
    stopped = false;
    return ESP_OK;
}

static esp_err_t run_turn(void)
{
    CHECK(locked && network_ready() && !s_cleanup_pending);
    CHECK(s_chat && s_chat == live_chat && !stopped && s_mcp == live_mcp);
    CHECK(s_encoder && s_decoder && (events & VOICE_BIT_CAPTURE));
    turns++;
    if (drop_in_turn) {
        drop_in_turn = false;
        disconnect();
        wifi_event(IP_EVENT, IP_EVENT_STA_GOT_IP);
        CHECK(!network_ready() && (events & VOICE_BIT_RESET));
        return ESP_FAIL;
    }
    xEventGroupSetBits(s_events, VOICE_BIT_TTS_STARTED | VOICE_BIT_TTS_STOPPED);
    return ESP_OK;
}

static esp_err_t esp_xiaozhi_chat_stop(esp_xiaozhi_chat_handle_t chat)
{
    CHECK(locked && chat && chat == s_chat && chat == live_chat && !stopped);
    CHECK(!(events & VOICE_BIT_RECEIVING));
    CHECK(s_mcp && s_mcp == live_mcp && s_encoder && s_encoder == live_encoder);
    CHECK(s_decoder && s_decoder == live_decoder);
    stops++;
    if (failures_left > 0) {
        failures_left--;
        return ESP_FAIL;
    }
    stopped = true;
    return ESP_OK;
}

static esp_err_t esp_xiaozhi_chat_deinit(esp_xiaozhi_chat_handle_t chat)
{
    CHECK(locked && stopped && chat && chat == s_chat && chat == live_chat);
    CHECK(s_mcp == live_mcp && s_encoder == live_encoder && s_decoder == live_decoder);
    CHECK(s_mcp && s_encoder && s_decoder);
    deinits++;
    live_chat = 0; /* Contract: consumed even if deinit reports an error. */
    return deinit_error ? ESP_FAIL : ESP_OK;
}

static esp_err_t esp_mcp_destroy(esp_mcp_t *mcp)
{
    CHECK(locked && stopped && !live_chat && !s_chat && !s_cleanup_pending);
    CHECK(mcp && mcp == s_mcp && mcp == live_mcp);
    free(mcp);
    live_mcp = NULL;
    mcp_destroys++;
    return ESP_OK;
}

static void opus_encoder_destroy(OpusEncoder *encoder)
{
    CHECK(locked && !live_chat && !live_mcp && !s_mcp);
    CHECK(encoder && encoder == s_encoder && encoder == live_encoder);
    free(encoder);
    live_encoder = NULL;
    encoder_destroys++;
}

static void opus_decoder_destroy(OpusDecoder *decoder)
{
    CHECK(locked && !live_chat && !live_mcp && !live_encoder && !s_encoder);
    CHECK(decoder && decoder == s_decoder && decoder == live_decoder);
    free(decoder);
    live_decoder = NULL;
    decoder_destroys++;
}

static void make_script(const char *route, int failures)
{
    EventBits_t online = VOICE_BIT_ENABLED | VOICE_BIT_NET;
    EventBits_t net_armed = online | VOICE_BIT_ARMED;
    EventBits_t error = failures ? VOICE_BIT_ERROR : 0;
    bool wake = strcmp(route, "wake") == 0;
    bool reset_live = strcmp(route, "reset-live") == 0;
    add("NET: first session and ARMED", NET, 1, 0, 0, 0, false, net_armed);
    bool fast = strcmp(route, "fast-reconnect") == 0;
    bool active = strcmp(route, "during-turn") == 0;
    if (fast || active || strcmp(route, "disconnect") == 0) {
        if (active) {
            drop_in_turn = true;
            add("turn: lose WiFi and regain IP before returning", WAKE, 1, 0, 0, 1, false, online | VOICE_BIT_RESET);
        }
        EventBits_t surviving = active || fast ? online : VOICE_BIT_ENABLED;
        EventBits_t pending_bits = surviving | VOICE_BIT_RESET | VOICE_BIT_ERROR;
        add("outage: dispose old session on voice task", active ? IDLE : fast ? FAST_RECONNECT : DROP,
            1, 1, !failures, active, failures != 0,
            failures ? pending_bits | (active ? 0 : VOICE_BIT_ARMED) : surviving);
        for (int attempt = 1; attempt <= failures; attempt++) {
            add("outage: retry failed SDK stop", IDLE, 1, 1 + attempt, attempt == failures, active,
                attempt < failures, pending_bits | (active ? 0 : VOICE_BIT_ARMED));
        }
        if (failures) {
            add("outage: complete reset after successful stop", IDLE, 1, failures + 1, 1, active, false, surviving);
        }
        add("IP restored: rebuild and arm once", active || fast ? IDLE : IP,
            2, failures + 1, 1, active, false, net_armed);
        add("wake: first turn after network recovery", WAKE, 2, failures + 1, 1, active + 1, false, net_armed);
        add("final RESET", PAUSE, 2, failures + 2, 2, active + 1, false, 0);
        return;
    }
    if (!reset_live) {
        add("goodbye: close or retain resources", GOODBYE, 1, 1, !failures, 0,
            failures != 0, net_armed | error);
    } else {
        add("pause live session: RESET close or retain", PAUSE, 1, 1, !failures, 0,
            failures != 0, failures ? VOICE_BIT_RESET | error : 0);
    }
    if (wake) {
        for (int attempt = 1; attempt <= failures; attempt++) {
            add("pending cleanup: retain until successful stop", IDLE, 1, 1 + attempt,
                attempt == failures, 0, attempt < failures, net_armed | error);
        }
        add("suspended: no eager reopen without wake", IDLE, 1, failures + 1, 1, 0, false, net_armed | error);
        add("wake: leave suspended without reopening yet", WAKE, 1, failures + 1, 1, 0, false,
            online | VOICE_BIT_TRIGGER | VOICE_BIT_CAPTURE | error);
        add("wake reconnect: new session and turn", IDLE, 2, failures + 1, 1, 1, false, net_armed);
    } else {
        if (!reset_live) {
            /* Pause while goodbye cleanup is still pending, not only after success. */
            add("goodbye then net_pause: RESET preempts wake", PAUSE, 1, failures ? 2 : 1,
                failures < 2, 0, failures == 2, failures ? VOICE_BIT_RESET | error : 0);
        }
        int already_retried = reset_live ? 0 : 1;
        for (int attempt = already_retried + 1; attempt <= failures; attempt++) {
            add("offline pending cleanup: retry with RESET retained", IDLE, 1, 1 + attempt,
                attempt == failures, 0, attempt < failures, VOICE_BIT_RESET | error);
        }
        if (failures) {
            add("RESET after cleanup: clear suspension and reset flags", IDLE, 1, failures + 1,
                1, 0, false, 0);
        }
        add("NET after RESET: rebuild and ARMED without a wake", NET, 2, failures + 1, 1, 0, false, net_armed);
    }
    int nturn = wake ? 1 : 0;
    add("final RESET: consume second session", PAUSE, 2, failures + 2, 2, nturn, false, 0);
    add("repeat RESET: no retry of consumed chat", PAUSE, 2, failures + 2, 2, nturn, false, 0);
}

int main(int argc, char **argv)
{
    CHECK(argc == 4);
    int failures = atoi(argv[2]);
    CHECK(failures >= 0 && failures <= 2);
    failures_left = failures;
    deinit_error = atoi(argv[3]) != 0;
    s_events = NULL;
    voice_net_pause();
    voice_net_ready();
    voice_net_set_connected(true);
    CHECK(events == 0); /* No-op when voice_init never created events. */
    s_events = &events;
    wifi_event(WIFI_EVENT, WIFI_EVENT_STA_START);
    wifi_event(IP_EVENT, IP_EVENT_STA_GOT_IP);
    CHECK(wifi_events == GOT_IP && events == VOICE_BIT_NET && !network_ready());
    disconnect();
    CHECK(wifi_connects == 2 && events == 0);
    wifi_event(IP_EVENT, IP_EVENT_STA_GOT_IP);
    wifi_event(WIFI_EVENT, WIFI_EVENT_STA_STOP);
    CHECK(wifi_connects == 2 && wifi_events == 0 && events == 0);
    make_script(argv[1], failures);
    if (setjmp(finished) == 0) voice_task(NULL);
    CHECK(current == script_count && !locked && takes == gives && failures_left == 0);
    CHECK(deinits == 2 && !s_chat && !live_chat && !s_cleanup_pending);
    CHECK(!s_mcp && !s_encoder && !s_decoder && events == 0);
    CHECK(delays == failures);
    printf("PASS %-11s stop_failures=%d deinit_error=%d (%d iterations, %d balanced lock pairs)\n",
           argv[1], failures, deinit_error, script_count, takes);
    return 0;
}
'''


def run(command, work, env, timeout):
    result = subprocess.run(
        [str(arg) for arg in command], cwd=work, env=env,
        capture_output=True, text=True, timeout=timeout,
    )
    require(
        result.returncode == 0,
        f"command failed ({result.returncode}): {' '.join(map(str, command))}\n{result.stdout}{result.stderr}",
    )
    if result.stderr:
        print(result.stderr, file=sys.stderr, end="")
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--sanitize", action="store_true", help="enable AddressSanitizer / UBSan")
    args = parser.parse_args()
    compiler = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")
    require(compiler, "no host C compiler found (cc/clang/gcc)")
    source = Path(__file__).resolve().parents[1] / "main" / "voice.c"
    functions, ranges = extract(source.read_text(encoding="utf-8"), source, BOUNDARIES)
    net = source.with_name("net.c")
    handler, net_ranges = extract(net.read_text(encoding="utf-8"), net, (
        ("static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)", "void net_init(void)"),
    ))
    handler = "#define s_events (&wifi_events)\n" + handler + "\n#undef s_events\n"
    print("Extracting unchanged source (not the whole voice.c):", flush=True)
    for entry in ranges + net_ranges:
        print(f"  {entry}", flush=True)
    env = dict(os.environ, LC_ALL="C")
    if args.sanitize:
        env["ASAN_OPTIONS"] = "halt_on_error=1:abort_on_error=1"
        env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    with tempfile.TemporaryDirectory(prefix="weather-test-voice-lifecycle-") as directory:
        work = Path(directory)
        harness = work / "lifecycle.c"
        harness.write_text(PRELUDE + "\n" + functions + "\n" + handler + "\n" + HARNESS, encoding="utf-8")
        executable = work / "lifecycle.out"
        flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"] if args.sanitize else []
        run([
            compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", *flags,
            "-I", source.parent, harness, "-o", executable,
        ], work, env, 60)
        for route in ("wake", "reprovision", "reset-live", "disconnect", "fast-reconnect", "during-turn"):
            for failures in range(3):
                for deinit_error in range(2):
                    print(run([executable, route, failures, deinit_error], work, env, 5), end="", flush=True)
    print(f"PASS 36 lifecycle scenarios{' (ASan/UBSan)' if args.sanitize else ''}")
    print("Scope: does not validate the real SDK stop timing, audio, WakeNet, or all of voice.c.")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, OSError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
