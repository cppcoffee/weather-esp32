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
    ("static void close_session(void)", "static esp_err_t prepare_service(void)"),
    ("static void finish_wake(void)", "static void voice_task(void *arg)"),
    ("static void voice_task(void *arg)", "esp_err_t voice_init(void)"),
    ("void voice_enable_wake(void)", "void voice_net_ready(void)"),
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
#include <stdatomic.h>
#include <string.h>
#include <sys/time.h>
#include "net.h"
#include "voice_feedback.h"
#include "voice_internal.h"

static voice_state_t feedback;
bool voice_feedback_publish(voice_state_t state, const char *code) {
    (void)code; feedback = state; return true;
}
voice_feedback_t voice_feedback_get(void) { return (voice_feedback_t){.state = feedback}; }

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
#define ESP_ERR_INVALID_STATE 2
#define pdTRUE 1
#define pdFALSE 0
#define BIT0 1u
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)

enum { WIFI_EVENT, IP_EVENT, NET_EVENTS };
enum { RECONNECT_DUE, RECONNECT_REQUEST };
enum { WIFI_EVENT_STA_START, WIFI_EVENT_STA_DISCONNECTED, WIFI_EVENT_STA_STOP, IP_EVENT_STA_GOT_IP };
enum { WIFI_MODE_STA, WIFI_MODE_APSTA };
enum { WIFI_PS_NONE, WIFI_PS_MAX_MODEM };
typedef int esp_event_base_t, wifi_mode_t;
static EventBits_t wifi_events;
static EventBits_t *sta_events = &wifi_events;
static EventBits_t events;
static unsigned wifi_connects, reconnect_requests, quick_retries, backoff_s;
static bool sta_active, low_latency;
static int64_t reconnect_at_us, timer_at_us, now_us = 100;
static void *reconnect_timer = &timer_at_us;
static wifi_mode_t wifi_mode = WIFI_MODE_STA;
static esp_err_t esp_wifi_connect(void) { wifi_connects++; return ESP_OK; }
static esp_err_t esp_wifi_get_mode(wifi_mode_t *mode) { *mode = wifi_mode; return ESP_OK; }
static esp_err_t esp_wifi_set_ps(int mode) {
    CHECK(wifi_mode == WIFI_MODE_STA && (mode == WIFI_PS_NONE || mode == WIFI_PS_MAX_MODEM));
    low_latency = mode == WIFI_PS_NONE;
    return ESP_OK;
}
static int64_t esp_timer_get_time(void) { return now_us; }
static esp_err_t esp_timer_stop(void *timer) {
    CHECK(timer == reconnect_timer);
    timer_at_us = 0;
    return ESP_OK;
}
static esp_err_t esp_timer_start_once(void *timer, int64_t delay_us) {
    CHECK(timer == reconnect_timer && delay_us > 0);
    if (timer_at_us) return ESP_ERR_INVALID_STATE;
    timer_at_us = now_us + delay_us;
    return ESP_OK;
}
static int queued_net_event = -1;
static esp_err_t esp_event_post(esp_event_base_t base, int id, void *data, size_t size, TickType_t wait) {
    CHECK(base == NET_EVENTS && !data && !size && !wait && queued_net_event == -1);
    queued_net_event = id;
    if (id == RECONNECT_REQUEST) reconnect_requests++;
    return ESP_OK;
}
static void save_channel(void) {}
static TickType_t xTaskGetTickCount(void) { return (TickType_t)(now_us / 1000); }
static void start_time_sync(void);
static atomic_bool time_synced;
static bool sntp_initialized;
static unsigned sntp_starts, sntp_inits;
static void (*sntp_callback)(struct timeval *);
typedef struct {
    const char *server;
    void (*sync_cb)(struct timeval *);
    bool start, wait_for_sync;
} esp_sntp_config_t;
#define ESP_NETIF_SNTP_DEFAULT_CONFIG(name) ((esp_sntp_config_t){.server = name, .start = true, .wait_for_sync = true})
static esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *cfg) {
    CHECK(cfg->start && !cfg->wait_for_sync && cfg->sync_cb);
    CHECK(!strcmp(cfg->server, "pool.ntp.org"));
    if (++sntp_inits == 1) return ESP_FAIL;
    sntp_callback = cfg->sync_cb;
    return ESP_OK;
}
static esp_err_t esp_netif_sntp_start(void) {
    CHECK(sntp_callback);
    sntp_starts++;
    return ESP_OK;
}
static EventBits_t *voice_events = &events;
static int lock_token, queue_token;
static void *network_lock = &lock_token, *rx_audio_queue = &queue_token;
static struct {
    esp_xiaozhi_chat_handle_t chat;
    esp_mcp_t *mcp;
    OpusEncoder *encoder;
    OpusDecoder *decoder;
    bool cleanup_pending;
} session;
static esp_xiaozhi_chat_handle_t live_chat;
static esp_mcp_t *live_mcp;
static OpusEncoder *live_encoder;
static OpusDecoder *live_decoder;
static bool locked, stopped, muted = true;
static int prepares;
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
static esp_err_t prepare_service(void);
static esp_err_t open_session(void);
static esp_err_t run_turn(void);
'''

HARNESS = r'''
#line 1 "lifecycle_harness.c"
enum action { ENABLE_OFFLINE, ENABLE_ONLINE, PROBE, PAUSE, WAKE, IP, TIMEOUT };
enum turn_event { NONE, GOODBYE, REPROVISION, LINK_FLAP };
static enum turn_event during_turn;
static void wifi_event(esp_event_base_t base, int32_t event)
{
    on_event(NULL, base, event, NULL);
}
static void process_net_event(void)
{
    int id = queued_net_event;
    CHECK(id != -1);
    queued_net_event = -1;
    wifi_event(NET_EVENTS, id);
}
static void disconnect(void)
{
    wifi_event(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED);
    CHECK(!wifi_events && !network_ready());
}

struct step {
    enum action action;
    int expected_turns;
    int expected_prepares;
    EventBits_t required, forbidden;
};
static struct step script[20];
static int script_count, current;

static void add(enum action action, int expected_turns, int expected_prepares,
                EventBits_t required, EventBits_t forbidden)
{
    CHECK(script_count < (int)(sizeof(script) / sizeof(script[0])));
    script[script_count++] = (struct step){action, expected_turns, expected_prepares, required, forbidden};
}

static enum action outer_boundary(void)
{
    CHECK(!locked && takes == gives && !low_latency);
    if (current == script_count) longjmp(finished, 1);
    const struct step *step = &script[current++];
    CHECK((events & step->required) == step->required && !(events & step->forbidden));
    CHECK(turns == step->expected_turns && opens == step->expected_turns && prepares == step->expected_prepares);
    CHECK(deinits == opens && mcp_destroys == opens && encoder_destroys == opens && decoder_destroys == opens);
    CHECK(!session.cleanup_pending && !session.chat && !session.mcp && !session.encoder && !session.decoder);
    CHECK(!live_chat && !live_mcp && !live_encoder && !live_decoder && muted);
    switch (step->action) {
    case ENABLE_OFFLINE:
        wifi_event(WIFI_EVENT, WIFI_EVENT_STA_START);
        voice_enable_wake();
        break;
    case ENABLE_ONLINE:
        wifi_event(WIFI_EVENT, WIFI_EVENT_STA_START);
        wifi_event(IP_EVENT, IP_EVENT_STA_GOT_IP);
        voice_enable_wake();
        break;
    case PROBE:
        voice_net_ready();
        break;
    case IP:
        if (queued_net_event != -1) process_net_event();
        wifi_event(IP_EVENT, IP_EVENT_STA_GOT_IP);
        break;
    case PAUSE:
        voice_net_pause();
        wifi_event(WIFI_EVENT, WIFI_EVENT_STA_STOP);
        break;
    case WAKE:
        CHECK(events & VOICE_BIT_ARMED);
        xEventGroupClearBits(voice_events, VOICE_BIT_ARMED);
        xEventGroupSetBits(voice_events, VOICE_BIT_TRIGGER | VOICE_BIT_CAPTURE);
        break;
    case TIMEOUT:
        CHECK(queued_net_event == RECONNECT_REQUEST);
        process_net_event();
        break;
    }
    return step->action;
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
    CHECK(!(events & mask));
    enum action action = outer_boundary();
    if (action == TIMEOUT) {
        CHECK(timeout == VOICE_NETWORK_WAIT_MS && !(events & mask));
        now_us += (int64_t)timeout * 1000;
    } else {
        CHECK(events & mask);
    }
    return events;
}

static int xSemaphoreTake(void *lock, TickType_t timeout)
{
    CHECK(lock == network_lock && !locked && timeout == portMAX_DELAY);
    locked = true;
    takes++;
    return pdTRUE;
}

static int xSemaphoreGive(void *lock)
{
    CHECK(lock == network_lock && locked);
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
    CHECK(queue == rx_audio_queue && locked);
    return pdTRUE;
}

static esp_err_t audio_set_mute(bool mute)
{
    CHECK(locked && mute);
    muted = mute;
    return ESP_OK;
}

static esp_err_t prepare_service(void)
{
    CHECK(locked && network_ready());
    prepares++;
    return ESP_OK;
}

static esp_err_t open_session(void)
{
    CHECK(locked && low_latency && network_ready() && !session.cleanup_pending);
    CHECK(!session.chat && !session.mcp && !session.encoder && !session.decoder && !live_chat);
    CHECK(!live_mcp && !live_encoder && !live_decoder);
    session.mcp = live_mcp = calloc(1, sizeof(*session.mcp));
    session.encoder = live_encoder = calloc(1, sizeof(*session.encoder));
    session.decoder = live_decoder = calloc(1, sizeof(*session.decoder));
    CHECK(session.mcp && session.encoder && session.decoder);
    session.chat = live_chat = (unsigned)++opens;
    session.mcp->generation = session.encoder->generation = session.decoder->generation = session.chat;
    stopped = false;
    return ESP_OK;
}

static esp_err_t run_turn(void)
{
    CHECK(locked && low_latency && network_ready() && !session.cleanup_pending);
    CHECK(session.chat && session.chat == live_chat && !stopped && session.mcp == live_mcp);
    CHECK(session.encoder && session.decoder && (events & VOICE_BIT_CAPTURE));
    xEventGroupClearBits(voice_events, VOICE_BIT_TRIGGER);
    turns++;
    enum turn_event event = during_turn;
    during_turn = NONE;
    if (event == LINK_FLAP) {
        disconnect();
        wifi_event(IP_EVENT, IP_EVENT_STA_GOT_IP);
        CHECK(!network_ready() && (events & VOICE_BIT_LINK_LOST));
        return ESP_FAIL;
    }
    if (event == REPROVISION) {
        voice_net_pause();
        wifi_event(WIFI_EVENT, WIFI_EVENT_STA_STOP);
        CHECK(events & VOICE_BIT_RESET);
        return ESP_FAIL;
    }
    if (event == GOODBYE) {
        xEventGroupSetBits(voice_events, VOICE_BIT_HANGUP);
        return ESP_FAIL;
    }
    xEventGroupSetBits(voice_events, VOICE_BIT_TTS_STARTED | VOICE_BIT_TTS_STOPPED);
    return ESP_OK;
}

static esp_err_t esp_xiaozhi_chat_stop(esp_xiaozhi_chat_handle_t chat)
{
    CHECK(locked && chat && chat == session.chat && chat == live_chat && !stopped);
    CHECK(!(events & VOICE_BIT_RECEIVING));
    CHECK(session.mcp && session.mcp == live_mcp && session.encoder && session.encoder == live_encoder);
    CHECK(session.decoder && session.decoder == live_decoder);
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
    CHECK(locked && stopped && chat && chat == session.chat && chat == live_chat);
    CHECK(session.mcp == live_mcp && session.encoder == live_encoder && session.decoder == live_decoder);
    CHECK(session.mcp && session.encoder && session.decoder);
    deinits++;
    live_chat = 0; /* Contract: consumed even if deinit reports an error. */
    return deinit_error ? ESP_FAIL : ESP_OK;
}

static esp_err_t esp_mcp_destroy(esp_mcp_t *mcp)
{
    CHECK(locked && stopped && !live_chat && !session.chat && !session.cleanup_pending);
    CHECK(mcp && mcp == session.mcp && mcp == live_mcp);
    free(mcp);
    live_mcp = NULL;
    mcp_destroys++;
    return ESP_OK;
}

static void opus_encoder_destroy(OpusEncoder *encoder)
{
    CHECK(locked && !live_chat && !live_mcp && !session.mcp);
    CHECK(encoder && encoder == session.encoder && encoder == live_encoder);
    free(encoder);
    live_encoder = NULL;
    encoder_destroys++;
}

static void opus_decoder_destroy(OpusDecoder *decoder)
{
    CHECK(locked && !live_chat && !live_mcp && !live_encoder && !session.encoder);
    CHECK(decoder && decoder == session.decoder && decoder == live_decoder);
    free(decoder);
    live_decoder = NULL;
    decoder_destroys++;
}

static void make_script(const char *route)
{
    EventBits_t local = VOICE_BIT_ENABLED | VOICE_BIT_ARMED;
    EventBits_t online = local | VOICE_BIT_NET;
    EventBits_t pending = VOICE_BIT_TRIGGER | VOICE_BIT_CAPTURE;
    bool offline = !strcmp(route, "offline-wake") || !strcmp(route, "offline-expiry");
    add(offline ? ENABLE_OFFLINE : ENABLE_ONLINE, 0, 0, 0, local | VOICE_BIT_NET);
    if (offline) {
        add(WAKE, 0, 0, local, VOICE_BIT_NET);
        if (!strcmp(route, "offline-expiry")) {
            add(TIMEOUT, 0, 0, pending, local ^ VOICE_BIT_ENABLED);
            add(IP, 0, 0, local, pending | VOICE_BIT_NET);
            add(PROBE, 0, 0, online, pending | VOICE_BIT_CHECK_INFO);
            add(WAKE, 0, 1, online, pending);
        } else {
            add(IP, 0, 0, pending, VOICE_BIT_NET | VOICE_BIT_ARMED);
        }
    } else {
        if (!strcmp(route, "reprovision")) during_turn = REPROVISION;
        if (!strcmp(route, "link-flap")) during_turn = LINK_FLAP;
        if (!strcmp(route, "goodbye")) during_turn = GOODBYE;
        add(PROBE, 0, 0, online, pending | VOICE_BIT_CHECK_INFO);
        add(WAKE, 0, 1, online, pending);
    }

    int configured = 1;
    if (!strcmp(route, "reprovision")) {
        add(ENABLE_ONLINE, 1, 1, 0, online | pending | VOICE_BIT_RESET);
        add(PROBE, 1, 1, online, pending | VOICE_BIT_CHECK_INFO);
        configured = 2;
    }
    add(WAKE, 1, configured, online, pending | VOICE_BIT_LINK_LOST);
    add(PAUSE, 2, configured, online, pending);
    add(PAUSE, 2, configured, 0, online | pending | VOICE_BIT_RESET);
}

static void test_reconnect(void)
{
    wifi_event(WIFI_EVENT, WIFI_EVENT_STA_START);
    wifi_event(IP_EVENT, IP_EVENT_STA_GOT_IP);
    CHECK(wifi_events == GOT_IP && events == VOICE_BIT_NET && !network_ready());
    CHECK(sntp_inits == 1 && !sntp_starts && !net_time_is_synced());
    disconnect();
    CHECK(wifi_connects == 2 && (events & VOICE_BIT_LINK_LOST));
    wifi_event(IP_EVENT, IP_EVENT_STA_GOT_IP);
    CHECK(sntp_inits == 2 && sntp_callback && !sntp_starts && !net_time_is_synced());
    sntp_callback(NULL);
    sntp_callback(&(struct timeval){.tv_sec = 100});
    CHECK(!net_time_is_synced());
    sntp_callback(&(struct timeval){.tv_sec = NET_MIN_SYNCED_EPOCH});
    CHECK(net_time_is_synced());
    disconnect();
    disconnect();
    CHECK(wifi_connects == 4 && !timer_at_us);
    for (unsigned i = 0; i < 6; i++) {
        static const int seconds[] = {5, 10, 20, 40, 60, 60};
        unsigned before = wifi_connects;
        disconnect();
        CHECK(wifi_connects == before && timer_at_us - now_us == seconds[i] * 1000000LL);
        now_us = timer_at_us;
        timer_at_us = 0;
        reconnect_due(NULL);
        process_net_event();
        CHECK(wifi_connects == before + 1 && !timer_at_us);
    }
    disconnect();
    unsigned before = wifi_connects;
    net_request_reconnect();
    process_net_event();
    CHECK(wifi_connects == before + 1 && !timer_at_us);
    disconnect();
    wifi_event(WIFI_EVENT, WIFI_EVENT_STA_STOP);
    CHECK(!timer_at_us && !sta_active && !wifi_events);
    wifi_mode = WIFI_MODE_APSTA;
    wifi_event(WIFI_EVENT, WIFI_EVENT_STA_START);
    wifi_event(IP_EVENT, IP_EVENT_STA_GOT_IP);
    wifi_event(NET_EVENTS, RECONNECT_DUE);
    CHECK(wifi_connects == before + 1 && !wifi_events && !sntp_starts);
    wifi_mode = WIFI_MODE_STA;
    wifi_event(WIFI_EVENT, WIFI_EVENT_STA_START);
    wifi_event(IP_EVENT, IP_EVENT_STA_GOT_IP);
    CHECK(sntp_starts == 1 && net_time_is_synced());
    wifi_event(WIFI_EVENT, WIFI_EVENT_STA_STOP);
    CHECK(!timer_at_us && !wifi_events && !sta_active);
    events = 0;
    reconnect_requests = 0;
}

int main(int argc, char **argv)
{
    CHECK(argc == 4);
    int failures = atoi(argv[2]);
    CHECK(failures >= 0 && failures <= 2);
    failures_left = failures;
    deinit_error = atoi(argv[3]) != 0;
    context = "network backoff and SNTP";
    test_reconnect();
    context = argv[1];
    voice_events = NULL;
    voice_net_pause();
    voice_net_ready();
    voice_net_set_connected(true);
    CHECK(events == 0);
    voice_events = &events;
    make_script(argv[1]);
    if (setjmp(finished) == 0) voice_task(NULL);
    CHECK(current == script_count && !locked && takes == gives && failures_left == 0);
    CHECK(deinits == 2 && opens == 2 && stops == 2 + failures && !session.chat && !live_chat);
    CHECK(!session.cleanup_pending && !session.mcp && !session.encoder && !session.decoder && events == 0);
    CHECK(delays == (failures > 0 ? failures - 1 : 0));
    CHECK(reconnect_requests == (!strncmp(argv[1], "offline-", 8) ? 1u : 0u));
    printf("PASS %-14s stop_failures=%d deinit_error=%d (%d balanced lock pairs)\n",
           argv[1], failures, deinit_error, takes);
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
    net_source = net.read_text(encoding="utf-8")
    constants = re.findall(r"^#define (?:GOT_IP|QUICK_RETRIES|BACKOFF_MIN_S|BACKOFF_MAX_S) .+$",
                           net_source, re.MULTILINE)
    require(len(constants) == 4, "missing production reconnect constants")
    handler, net_ranges = extract(net_source, net, (
        ("static bool sta_mode(void)", "static void cancel_reconnect(void)"),
        ("static void cancel_reconnect(void)", "static void reset_reconnect(void)"),
        ("static void reset_reconnect(void)", "static void schedule_reconnect(void)"),
        ("static void schedule_reconnect(void)", "static void connect_sta(void)"),
        ("static void connect_sta(void)", "static void reconnect_due(void *arg)"),
        ("static void reconnect_due(void *arg)", "void net_request_reconnect(void)"),
        ("void net_request_reconnect(void)", "static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)"),
        ("void net_set_wifi_low_latency(bool enable)", "void net_get_status(net_status_t *out)"),
        ("static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)", "void net_init(void)"),
        ("static void on_time_sync(struct timeval *tv)", "bool net_time_is_synced(void)"),
        ("bool net_time_is_synced(void)", "static void start_time_sync(void)"),
        ("static void start_time_sync(void)", "int net_http_get(const char *url, char *buf, size_t cap)"),
    ))
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
        harness.write_text("\n".join((PRELUDE, *constants, functions, handler, HARNESS)), encoding="utf-8")
        executable = work / "lifecycle.out"
        flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"] if args.sanitize else []
        run([
            compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", *flags,
            "-I", source.parent, harness, "-o", executable,
        ], work, env, 60)
        for route in ("wake", "reprovision", "goodbye", "link-flap", "offline-wake", "offline-expiry"):
            for failures in range(3):
                for deinit_error in range(2):
                    print(run([executable, route, failures, deinit_error], work, env, 5), end="", flush=True)
    print(f"PASS 36 lifecycle scenarios{' (ASan/UBSan)' if args.sanitize else ''}")
    print("PASS Wi-Fi fast retries, scan backoff, wake-triggered reconnect, portal isolation and SNTP")
    print("Scope: does not validate the real SDK stop timing, audio, WakeNet, or all of voice.c.")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, OSError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
