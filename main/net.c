#include "net.h"

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

static const char *TAG = "net";
static EventGroupHandle_t s_events; // 仅 GOT_IP；配网握手由 portal_web.c 管理
#define GOT_IP BIT0

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "disconnected, retrying");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_events, GOT_IP);
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
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, NULL));
    s_events = xEventGroupCreate();
}

void net_forget_creds(void)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "no stored credentials to erase");
        return;
    }
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGW(TAG, "stored credentials erased");
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

    xEventGroupClearBits(s_events, GOT_IP);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "connecting to %s", ssid);

    EventBits_t bits = xEventGroupWaitBits(s_events, GOT_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    if (bits & GOT_IP) {
        ESP_LOGI(TAG, "got ip");
        return true;
    }
    ESP_LOGW(TAG, "connect timeout");
    esp_wifi_stop();
    return false;
}

bool net_try_stored(int timeout_ms)
{
    char ssid[33] = {0}, pass[65] = {0};
    if (!load_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGW(TAG, "no stored credentials");
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
    ESP_LOGI(TAG, "tz %s", tz);
}

void net_time_sync(void)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    if (esp_netif_sntp_init(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "sntp init failed");
        return;
    }
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000)) != ESP_OK) {
        ESP_LOGW(TAG, "sntp sync timeout");
        return;
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    ESP_LOGI(TAG, "time %04d-%02d-%02d %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
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
        ESP_LOGW(TAG, "empty response from %s", url);
    }
    return total > 0 ? total : -1;
}
