#include "portal_web.h"

#include "net_portal.h"
#include "voice_settings.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *log_tag = "portal_web";

#define CREDS_SAVED BIT0
#define MAX_APS     16

extern const char portal_index[] asm("_binary_index_html_start");

static EventGroupHandle_t portal_event_group;
static httpd_handle_t     portal_server;

static EventGroupHandle_t
portal_events(void)
{
    if (!portal_event_group) {
        portal_event_group = xEventGroupCreate();
    }
    return portal_event_group;
}

// 扫周边热点。扫描要求 WiFi 处于 STA/APSTA 模式，所以配网时开的是 APSTA（STA
// 不连）。
static cJSON *
scan_aps_json(const char *own_ssid)
{
    cJSON *list = cJSON_CreateArray();
    if (!list) {
        return NULL;
    }
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 3; attempt++) {
        err = esp_wifi_scan_start(NULL, true);
        if (err == ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    if (err != ESP_OK) {
        ESP_LOGW(log_tag, "scan failed: %s", esp_err_to_name(err));
        return list;
    }
    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    if (found == 0) {
        return list;
    }
    if (found > MAX_APS) {
        found = MAX_APS;
    }
    wifi_ap_record_t *recs = calloc(found, sizeof(wifi_ap_record_t));
    if (!recs) {
        return list;
    }
    int n = 0;
    if (esp_wifi_scan_get_ap_records(&found, recs) == ESP_OK) {
        for (int i = 0; i < found; i++) {
            char ssid[33];
            snprintf(ssid, sizeof(ssid), "%.*s", (int)sizeof(ssid) - 1, (const char *)recs[i].ssid);
            if (!ssid[0] || strcmp(ssid, own_ssid) == 0) {
                continue;
            }
            cJSON_AddItemToArray(list, cJSON_CreateString(ssid));
            n++;
        }
    }
    free(recs);
    ESP_LOGI(log_tag, "scan: %d APs", n);
    return list;
}

// ESP-IDF extracts the complete encoded field. Apply the destination limit
// after decoding, since each original byte can occupy three encoded bytes.
static bool
form_field(const char *body, const char *name, char *out, size_t cap)
{
    char encoded[3 * (VOICE_URL_CAP - 1) + 1];
    out[0] = '\0';
    esp_err_t err = httpd_query_key_value(body, name, encoded, sizeof(encoded));
    if (err != ESP_OK) {
        return err == ESP_ERR_NOT_FOUND;
    }

    size_t len = 0;
    for (const char *p = encoded; *p; p++) {
        unsigned char byte = (unsigned char)*p;
        if (byte == '+') {
            byte = ' ';
        } else if (byte == '%') {
            if (!isxdigit((unsigned char)p[1]) || !isxdigit((unsigned char)p[2])) {
                return false;
            }
            char hex[] = {p[1], p[2], 0};
            byte = (unsigned char)strtoul(hex, NULL, 16);
            p += 2;
        }
        if (!byte || len == cap - 1) {
            return false;
        }
        out[len++] = (char)byte;
    }
    out[len] = '\0';
    return true;
}

static esp_err_t
any_get(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" NET_AP_URL "/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t
root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, portal_index, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t
state_get(httpd_req_t *req)
{
    const char      *own = net_ap_ssid();
    voice_settings_t settings = {0};
    if (voice_settings_load(&settings) != ESP_OK) {
        ESP_LOGW(log_tag, "invalid saved voice settings; showing empty fields");
        memset(&settings, 0, sizeof(settings));
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *aps = root ? scan_aps_json(own) : NULL;
    if (!root || !aps || !cJSON_AddItemToObject(root, "aps", aps)) {
        cJSON_Delete(aps);
        cJSON_Delete(root);
        return httpd_resp_send_500(req);
    }
    bool ok = cJSON_AddStringToObject(root, "voice_url", settings.info_url)
              && cJSON_AddStringToObject(root, "ap_ssid", own) && cJSON_AddStringToObject(root, "ap_url", NET_AP_URL);
    if (!ok) {
        cJSON_Delete(root);
        return httpd_resp_send_500(req);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);
    return err;
}

// title/msg are application-owned literals, never form input.
static esp_err_t
reply(httpd_req_t *req, const char *title, const char *msg, bool ok)
{
    if (!ok) {
        httpd_resp_set_status(req, "400 Bad Request");
    }
    char html[768];
    int  n = snprintf(html, sizeof(html),
                      "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
                      "<meta name=\"viewport\" "
                      "content=\"width=device-width,initial-scale=1\"><title>%s</title>"
                      "<style>body{margin:0;padding:3rem 1.25rem;font:16px/1.6 "
                      "system-ui,sans-serif;max-width:25rem;"
                      "margin:auto}h1{margin:0 0 1rem;font-size:1.75rem}</style></head>"
                      "<body><h1>%s</h1><p>%s</p></body></html>",
                      title, title, msg);
    if (n < 0 || (size_t)n >= sizeof(html)) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, html, n);
}

static esp_err_t
save_post(httpd_req_t *req)
{
    enum {
        BODY_CAP = 4096
    };

    if (req->content_len == 0 || req->content_len >= BODY_CAP) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid request body size");
    }
    char *body = malloc(req->content_len + 1);
    if (!body) {
        return httpd_resp_send_500(req);
    }
    size_t total = 0;
    while (total < req->content_len) {
        int received = httpd_req_recv(req, body + total, req->content_len - total);
        if (received <= 0) {
            free(body);
            return httpd_resp_send_500(req);
        }
        total += received;
    }
    body[total] = '\0';

    char             ssid[33], manual[33], pass[65];
    voice_settings_t settings = {.followup = 1};
    bool valid = !memchr(body, 0, total) && form_field(body, "ssid", ssid, sizeof(ssid))
                 && form_field(body, "ssid2", manual, sizeof(manual)) && form_field(body, "pass", pass, sizeof(pass))
                 && form_field(body, "voice_url", settings.info_url, sizeof(settings.info_url));
    free(body);
    if (!valid) {
        return reply(req, "表单无效", "请检查 WiFi 名称、密码和语音地址的长度后重试。", false);
    }

    // 列表里选的和手动输入的二选一，手填的优先。
    const char *network = manual[0] ? manual : ssid;
    if (!network[0]) {
        return reply(req, "没选网络", "回去选一个网络，或者手动输入名称。", false);
    }

    if (voice_settings_validate(&settings) != ESP_OK) {
        return reply(req, "语音地址无效",
                     "请填写服务信息地址（OTA），例如 "
                     "http://PC局域网IP:8000/xiaozhi/ota/",
                     false);
    }

    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) {
        return httpd_resp_send_500(req);
    }
    esp_err_t err = nvs_set_str(h, "ssid", network);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "pass", pass);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "channel", 0);
    }
    if (err == ESP_OK) {
        err = voice_settings_store(h, &settings);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(log_tag, "failed to save portal settings: %s", esp_err_to_name(err));
        return httpd_resp_send_500(req);
    }

    ESP_LOGI(log_tag, "stored credentials for %s", network);
    reply(req, "正在连接", "保存好了，屏幕稍后显示天气。", true);
    xEventGroupSetBits(portal_events(), CREDS_SAVED);
    return ESP_OK;
}

esp_err_t
portal_web_start(void)
{
    xEventGroupClearBits(portal_events(), CREDS_SAVED);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6 * 1024;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    esp_err_t err = httpd_start(&portal_server, &cfg);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_get},
        {.uri = "/api/state", .method = HTTP_GET, .handler = state_get},
        {.uri = "/save", .method = HTTP_POST, .handler = save_post},
        {.uri = "/*", .method = HTTP_GET, .handler = any_get},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]) && err == ESP_OK; i++) {
        err = httpd_register_uri_handler(portal_server, &routes[i]);
    }
    if (err != ESP_OK) {
        httpd_stop(portal_server);
        portal_server = NULL;
    }
    return err;
}

void
portal_web_wait(void)
{
    xEventGroupWaitBits(portal_events(), CREDS_SAVED, pdTRUE, pdTRUE, portMAX_DELAY);
}

void
portal_web_stop(void)
{
    if (!portal_server) {
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(300)); // let the response flush
    httpd_stop(portal_server);
    portal_server = NULL;
}
