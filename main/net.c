#include "net.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "voice.h"

static const char *log_tag = "net";
static EventGroupHandle_t sta_events; // 仅 GOT_IP；配网握手由 portal_web.c 管理
static atomic_bool time_synced;
static bool sntp_initialized; // Owned by the default event loop.
#define GOT_IP BIT0

static void start_time_sync(void);

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_DISCONNECTED || id == WIFI_EVENT_STA_STOP)) {
        xEventGroupClearBits(sta_events, GOT_IP);
        voice_net_set_connected(false);
        if (id == WIFI_EVENT_STA_DISCONNECTED) {
            ESP_LOGW(log_tag, "disconnected, retrying");
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        start_time_sync();
        voice_net_set_connected(true);
        xEventGroupSetBits(sta_events, GOT_IP);
    }
}

void net_init(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    sta_events = xEventGroupCreate();
    ESP_ERROR_CHECK(sta_events != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, NULL));
}

void net_forget_creds(void)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(log_tag, "no stored credentials to erase");
        return;
    }
    esp_err_t ssid_err = nvs_erase_key(h, "ssid");
    esp_err_t pass_err = nvs_erase_key(h, "pass");
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

static bool load_creds(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t sl = ssid_len, pl = pass_len;
    bool ok = nvs_get_str(h, "ssid", ssid, &sl) == ESP_OK && sl > 1;
    if (ok) {
        ok = nvs_get_str(h, "pass", pass, &pl) == ESP_OK;
    }
    nvs_close(h);
    return ok;
}

static bool sta_connect(const char *ssid, const char *pass, int timeout_ms)
{
    wifi_config_t cfg = {.sta = {.threshold.authmode = WIFI_AUTH_OPEN}}; // 扫到啥就连啥，开放热点也要能用
    // NVS strings include a terminator. The zeroed STA fields can also hold full-width credentials.
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

bool net_try_stored(int timeout_ms)
{
    char ssid[33] = {0}, pass[65] = {0};
    if (!load_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGW(log_tag, "no stored credentials");
        return false;
    }
    return sta_connect(ssid, pass, timeout_ms);
}

bool net_has_stored(void)
{
    char ssid[33], pass[65];
    return load_creds(ssid, sizeof(ssid), pass, sizeof(pass));
}

// ---------------------------------------------------------------- time / http

void net_apply_tz(int off)
{
    // POSIX TZ has inverted sign: UTC+8 -> "XXX-8". Minutes matter for +5:30 etc.
    int hh = off / 3600, mm = labs(off % 3600) / 60;
    char tz[24];
    snprintf(tz, sizeof(tz), "XXX%c%02d:%02d", off < 0 ? '+' : '-', abs(hh), mm);
    setenv("TZ", tz, 1);
    tzset();
    ESP_LOGI(log_tag, "tz %s", tz);
}

static void on_time_sync(struct timeval *tv)
{
    if (tv && tv->tv_sec >= NET_MIN_SYNCED_EPOCH) {
        atomic_store(&time_synced, true);
    }
}

bool net_time_is_synced(void)
{
    return atomic_load(&time_synced);
}

static void start_time_sync(void)
{
    if (sntp_initialized) {
        // Recover promptly after a reconnect, even if the last poll has backed off.
        if (esp_netif_sntp_start() != ESP_OK) {
            ESP_LOGW(log_tag, "sntp restart failed");
        }
        return;
    }
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.sync_cb = on_time_sync;
    cfg.wait_for_sync = false;
    if (esp_netif_sntp_init(&cfg) != ESP_OK) {
        ESP_LOGW(log_tag, "sntp init failed");
        return;
    }
    sntp_initialized = true;
}

int net_http_get(const char *url, char *buf, size_t cap)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return -1;
    }

    int total = 0;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        int r;
        while (total < (int)cap - 1 && (r = esp_http_client_read(c, buf + total, cap - 1 - total)) > 0) {
            total += r;
        }
    }
    buf[total] = 0;
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (total == 0) {
        ESP_LOGW(log_tag, "empty response from %s", url);
    }
    return total > 0 ? total : -1;
}
