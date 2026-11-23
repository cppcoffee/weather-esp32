#include "net.h"
#include "net_time.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "voice.h"

static const char        *log_tag = "net";
static EventGroupHandle_t sta_events; // 仅 GOT_IP；配网握手由 portal_web.c 管理
static esp_timer_handle_t reconnect_timer;
// The default event loop owns the retry state and all connection attempts.
static bool     sta_active;
static unsigned quick_retries;
static unsigned backoff_s;
static int64_t  reconnect_at_us;
#define GOT_IP        BIT0
#define QUICK_RETRIES 2
#define BACKOFF_MIN_S 5u
#define BACKOFF_MAX_S 60u

ESP_EVENT_DEFINE_BASE(NET_EVENTS);

enum {
    RECONNECT_DUE,
    RECONNECT_REQUEST
};

static void save_channel(void);

static bool
sta_mode(void)
{
    wifi_mode_t mode;
    return esp_wifi_get_mode(&mode) == ESP_OK && mode == WIFI_MODE_STA;
}

static void
cancel_reconnect(void)
{
    (void)esp_timer_stop(reconnect_timer);
    reconnect_at_us = 0;
}

static void
reset_reconnect(void)
{
    cancel_reconnect();
    quick_retries = 0;
    backoff_s = BACKOFF_MIN_S;
}

static void
schedule_reconnect(void)
{
    unsigned delay_s = backoff_s;
    int64_t  delay_us = (int64_t)delay_s * 1000000;
    reconnect_at_us = esp_timer_get_time() + delay_us;
    esp_err_t err = esp_timer_start_once(reconnect_timer, delay_us);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(log_tag, "WiFi reconnect timer: %s", esp_err_to_name(err));
    }
    backoff_s = delay_s * 2 < BACKOFF_MAX_S ? delay_s * 2 : BACKOFF_MAX_S;
    ESP_LOGI(log_tag, "WiFi reconnect in %u s", delay_s);
}

static void
connect_sta(void)
{
    if (sta_active && sta_mode()) {
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(log_tag, "WiFi connect start: %s", esp_err_to_name(err));
            schedule_reconnect();
        }
    }
}

static void
reconnect_due(void *arg)
{
    (void)arg;
    // Don't lose a retry to a temporarily full event queue or block other
    // esp_timer callbacks.
    if (esp_event_post(NET_EVENTS, RECONNECT_DUE, NULL, 0, 0) != ESP_OK) {
        (void)esp_timer_start_once(reconnect_timer, 100000);
    }
}

void
net_request_reconnect(void)
{
    (void)esp_event_post(NET_EVENTS, RECONNECT_REQUEST, NULL, 0, 0);
}

static void
on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == NET_EVENTS) {
        if (!reconnect_at_us) {
            return;
        }
        int64_t remaining = reconnect_at_us - esp_timer_get_time();
        if (id == RECONNECT_REQUEST || remaining <= 0) {
            cancel_reconnect();
            connect_sta();
        } else {
            (void)esp_timer_start_once(reconnect_timer, remaining);
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        sta_active = sta_mode();
        reset_reconnect();
        if (sta_active) {
            net_set_wifi_low_latency(false);
            connect_sta();
        }
    } else if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_DISCONNECTED || id == WIFI_EVENT_STA_STOP)) {
        xEventGroupClearBits(sta_events, GOT_IP);
        voice_net_set_connected(false);
        if (id == WIFI_EVENT_STA_STOP) {
            sta_active = false;
            reset_reconnect();
        } else if (sta_active && sta_mode() && !reconnect_at_us) {
            if (quick_retries < QUICK_RETRIES) {
                quick_retries++;
                connect_sta();
            } else {
                schedule_reconnect();
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP && sta_active && sta_mode()) {
        reset_reconnect();
        net_time_start();
        xEventGroupSetBits(sta_events, GOT_IP);
        voice_net_set_connected(true);
        save_channel();
    }
}

void
net_init(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    sta_events = xEventGroupCreate();
    ESP_ERROR_CHECK(sta_events != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    const esp_timer_create_args_t timer = {.callback = reconnect_due, .name = "wifi_reconnect"};
    ESP_ERROR_CHECK(esp_timer_create(&timer, &reconnect_timer));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(NET_EVENTS, ESP_EVENT_ANY_ID, on_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, NULL));
}

void
net_forget_creds(void)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(log_tag, "no stored credentials to erase");
        return;
    }
    esp_err_t ssid_err = nvs_erase_key(h, "ssid");
    esp_err_t pass_err = nvs_erase_key(h, "pass");
    (void)nvs_erase_key(h, "channel");
    if ((ssid_err != ESP_OK && ssid_err != ESP_ERR_NVS_NOT_FOUND)
        || (pass_err != ESP_OK && pass_err != ESP_ERR_NVS_NOT_FOUND))
    {
        ESP_LOGW(log_tag, "failed to erase WiFi credentials: %s / %s", esp_err_to_name(ssid_err),
                 esp_err_to_name(pass_err));
    }
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGW(log_tag, "stored credentials erased");
}

static bool
load_creds(char *ssid, size_t ssid_len, char *pass, size_t pass_len, uint8_t *channel)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t sl = ssid_len, pl = pass_len;
    bool   ok = nvs_get_str(h, "ssid", ssid, &sl) == ESP_OK && sl > 1 && nvs_get_str(h, "pass", pass, &pl) == ESP_OK;
    if (ok && channel) {
        uint8_t saved = 0;
        if (nvs_get_u8(h, "channel", &saved) == ESP_OK && saved <= 14) {
            *channel = saved;
        }
    }
    nvs_close(h);
    return ok;
}

static void
save_channel(void)
{
    wifi_ap_record_t ap;
    nvs_handle_t     h;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK || ap.primary == 0 || nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    char    ssid[33];
    size_t  len = sizeof(ssid);
    uint8_t channel = 0;
    (void)nvs_get_u8(h, "channel", &channel);
    if (channel != ap.primary && nvs_get_str(h, "ssid", ssid, &len) == ESP_OK
        && strncmp(ssid, (const char *)ap.ssid, sizeof(ap.ssid)) == 0 && nvs_set_u8(h, "channel", ap.primary) == ESP_OK)
    {
        (void)nvs_commit(h);
    }
    nvs_close(h);
}

bool
net_try_stored(int timeout_ms)
{
    char    ssid[33] = {0}, pass[65] = {0};
    uint8_t channel = 0;
    if (!load_creds(ssid, sizeof(ssid), pass, sizeof(pass), &channel)) {
        ESP_LOGW(log_tag, "no stored credentials");
        return false;
    }
    wifi_config_t cfg = {.sta = {.channel = channel, .threshold.authmode = WIFI_AUTH_OPEN}};
    memcpy(cfg.sta.ssid, ssid, strnlen(ssid, sizeof(cfg.sta.ssid)));
    memcpy(cfg.sta.password, pass, strnlen(pass, sizeof(cfg.sta.password)));

    xEventGroupClearBits(sta_events, GOT_IP);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(log_tag, "connecting to %s", ssid);

    EventBits_t bits = xEventGroupWaitBits(sta_events, GOT_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    if (bits & GOT_IP) {
        ESP_LOGI(log_tag, "got ip");
        return true;
    }
    ESP_LOGW(log_tag, "connect timeout");
    esp_wifi_stop();
    return false;
}

bool
net_has_stored(void)
{
    char ssid[33], pass[65];
    return load_creds(ssid, sizeof(ssid), pass, sizeof(pass), NULL);
}

bool
net_has_ip(void)
{
    return sta_events && (xEventGroupGetBits(sta_events) & GOT_IP);
}

void
net_set_wifi_low_latency(bool enable)
{
    if (!sta_mode()) {
        return;
    }
    esp_err_t err = esp_wifi_set_ps(enable ? WIFI_PS_NONE : WIFI_PS_MAX_MODEM);
    if (err != ESP_OK) {
        ESP_LOGW(log_tag, "wifi ps(%s) failed: %s", enable ? "none" : "max_modem", esp_err_to_name(err));
    }
}

void
net_get_status(net_status_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!net_has_ip()) {
        return;
    }
    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        out->connected = true;
        snprintf(out->ssid, sizeof(out->ssid), "%.*s", (int)sizeof(ap.ssid), (const char *)ap.ssid);
        out->rssi = ap.rssi;
    }
}
