#include "portal_web.h"

#include "net.h"
#include "voice_settings.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "portal_web";

#define CREDS_SAVED BIT0
#define MAX_APS     16

typedef struct {
    char ssid[33];
} scan_ap_t;

static EventGroupHandle_t s_events;
static httpd_handle_t s_server;

static EventGroupHandle_t portal_events(void)
{
    if (!s_events) {
        s_events = xEventGroupCreate();
    }
    return s_events;
}

// 扫周边热点。扫描要求 WiFi 处于 STA/APSTA 模式，所以配网时开的是 APSTA（STA 不连）。
static int scan_aps(scan_ap_t *out, int max)
{
    // STA 正在重连时扫描会被直接拒掉（ESP_ERR_WIFI_STATE），隔一会儿重试。
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 3; attempt++) {
        err = esp_wifi_scan_start(NULL, true);
        if (err == ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
        return 0;
    }
    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    if (found == 0) {
        return 0;
    }
    if (found > max) {
        found = max;
    }
    wifi_ap_record_t *recs = calloc(found, sizeof(wifi_ap_record_t));
    if (!recs) {
        return 0;
    }
    int n = 0;
    if (esp_wifi_scan_get_ap_records(&found, recs) == ESP_OK) {
        for (int i = 0; i < found; i++) {
            if (!recs[i].ssid[0]) {
                continue; // 隐藏网络没法从下拉里选
            }
            snprintf(out[n].ssid, sizeof(out[n].ssid), "%s", (const char *)recs[i].ssid);
            n++;
        }
    }
    free(recs);
    ESP_LOGI(TAG, "scan: %d APs", n);
    return n;
}

// 热点名是用户/邻居给的，塞进 HTML 前必须转义。
static void html_escape(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (; *in && o + 7 < cap; in++) {
        switch (*in) {
        case '&':
            o += snprintf(out + o, cap - o, "&amp;");
            break;
        case '<':
            o += snprintf(out + o, cap - o, "&lt;");
            break;
        case '>':
            o += snprintf(out + o, cap - o, "&gt;");
            break;
        case '"':
            o += snprintf(out + o, cap - o, "&quot;");
            break;
        case '\'':
            o += snprintf(out + o, cap - o, "&#39;");
            break;
        default:
            out[o++] = *in;
        }
    }
    out[o] = 0;
}

// 往固定长度缓冲里累加，溢出了就停在 cap。
static int addf(char *buf, size_t cap, int pos, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + pos, cap - pos, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)(pos + n) >= cap) {
        return cap;
    }
    return pos + n;
}

static int url_decode(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '+') {
            *o++ = ' ';
        } else if (*p == '%' && p[1] && p[2]) {
            char hex[3] = {p[1], p[2], 0};
            *o++ = (char)strtol(hex, NULL, 16);
            p += 2;
        } else {
            *o++ = *p;
        }
    }
    *o = 0;
    return o - s;
}

static void form_field(const char *body, const char *name, char *out, size_t cap)
{
    char key[16];
    snprintf(key, sizeof(key), "%s=", name);
    const char *p = strstr(body, key);
    out[0] = 0;
    if (!p) {
        return;
    }
    p += strlen(key);
    size_t i = 0;
    while (*p && *p != '&' && i + 1 < cap) {
        out[i++] = *p++;
    }
    out[i] = 0;
    url_decode(out);
}

// Text assets are NUL-terminated by EMBED_TXTFILES. Inline CSS/JS in the response so
// captive portals need no extra requests, especially when /save closes the hotspot.
extern const char portal_index[] asm("_binary_index_html_start");
extern const char portal_result[] asm("_binary_result_html_start");
extern const char portal_style[] asm("_binary_style_css_start");
extern const char portal_script[] asm("_binary_app_js_start");

typedef struct {
    const char *token;
    const char *value;
} page_value_t;

// Substitute only trusted template tokens; inserted values are never parsed again.
// Callers must HTML-escape untrusted text before supplying it here.
static esp_err_t send_page(httpd_req_t *req, const char *page, const page_value_t *values, size_t count)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    const char *cursor = page;
    const char *token;
    while ((token = strstr(cursor, "{{")) != NULL) {
        const page_value_t *value = NULL;
        for (size_t i = 0; i < count; i++) {
            if (strncmp(token, values[i].token, strlen(values[i].token)) == 0) {
                value = &values[i];
                break;
            }
        }
        if (!value) {
            ESP_LOGE(TAG, "unknown portal template token");
            return ESP_FAIL;
        }
        esp_err_t err;
        if (token > cursor) {
            err = httpd_resp_send_chunk(req, cursor, token - cursor);
            if (err != ESP_OK) {
                return err;
            }
        }
        // A zero-length chunk terminates the response, so skip empty replacements.
        if (*value->value) {
            err = httpd_resp_send_chunk(req, value->value, HTTPD_RESP_USE_STRLEN);
            if (err != ESP_OK) {
                return err;
            }
        }
        cursor = token + strlen(value->token);
    }
    if (*cursor) {
        esp_err_t err = httpd_resp_send_chunk(req, cursor, HTTPD_RESP_USE_STRLEN);
        if (err != ESP_OK) {
            return err;
        }
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t any_get(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" NET_AP_URL "/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t root_get(httpd_req_t *req)
{
    scan_ap_t aps[MAX_APS];
    int n = scan_aps(aps, MAX_APS);

    // Two escaped copies of each 32-byte SSID plus the option markup fit in 8 KB.
    enum { CAP = 8192 };
    char *options = calloc(1, CAP);
    if (!options) {
        return httpd_resp_send_500(req);
    }

    const char *own = net_ap_ssid();
    int pos = 0;
    int shown = 0;
    for (int i = 0; i < n && pos < CAP; i++) {
        if (strcmp(aps[i].ssid, own) == 0) {
            continue;
        }
        char name[200];
        html_escape(aps[i].ssid, name, sizeof(name));
        pos = addf(options, CAP, pos, "<option value=\"%s\"%s>%s</option>", name, shown ? "" : " selected", name);
        shown++;
    }
    if (!shown) {
        pos = addf(options, CAP, pos, "<option value=\"\" selected disabled>附近没扫到网络，试试重新扫描</option>");
    }
    if (pos >= CAP) {
        free(options);
        return httpd_resp_send_500(req);
    }

    char count[12];
    char escaped_voice_url[VOICE_URL_CAP * 6];
    voice_settings_t settings = {0};
    if (voice_settings_load(&settings) != ESP_OK) {
        ESP_LOGW(TAG, "invalid saved voice settings; showing empty fields");
        memset(&settings, 0, sizeof(settings));
    }
    html_escape(settings.info_url, escaped_voice_url, sizeof(escaped_voice_url));
    snprintf(count, sizeof(count), "%d", n);
    const page_value_t values[] = {
        {"{{STYLE}}", portal_style},
        {"{{SCRIPT}}", portal_script},
        {"{{OPTIONS}}", options},
        {"{{COUNT}}", count},
        {"{{AP_URL}}", NET_AP_URL},
        {"{{AP_SSID}}", own},
        {"{{VOICE_URL}}", escaped_voice_url},
        {"{{VOICE_FOLLOWUP}}", settings.followup ? "checked" : ""},
    };
    esp_err_t err = send_page(req, portal_index, values, sizeof(values) / sizeof(values[0]));
    free(options);
    return err;
}

// title/msg are application-owned literals, never form input. The result is self-contained.
static esp_err_t reply(httpd_req_t *req, const char *title, const char *msg, bool ok)
{
    if (!ok) {
        httpd_resp_set_status(req, "400 Bad Request");
    }
    const page_value_t values[] = {
        {"{{STYLE}}", portal_style},
        {"{{TITLE}}", title},
        {"{{MESSAGE}}", msg},
    };
    return send_page(req, portal_result, values, sizeof(values) / sizeof(values[0]));
}

static esp_err_t save_post(httpd_req_t *req)
{
    enum { BODY_CAP = 4096 };
    char body[BODY_CAP];
    if (req->content_len <= 0 || req->content_len >= BODY_CAP) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "request body too large");
    }
    int total = 0;
    while (total < req->content_len) {
        int received = httpd_req_recv(req, body + total, req->content_len - total);
        if (received <= 0) {
            return httpd_resp_send_500(req);
        }
        total += received;
    }
    body[total] = 0;

    // 列表里选的和手动输入的二选一，手填的优先。
    char ssid[33], manual[33], pass[65];
    form_field(body, "ssid", ssid, sizeof(ssid));
    form_field(body, "ssid2", manual, sizeof(manual));
    form_field(body, "pass", pass, sizeof(pass));
    if (manual[0]) {
        memcpy(ssid, manual, sizeof(ssid));
    }
    if (strlen(ssid) < 1) {
        return reply(req, "没选网络", "回去选一个网络，或者手动输入名称。", false);
    }

    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) {
        return httpd_resp_send_500(req);
    }
    // 信息地址必填：固件没有内置默认 OTA 地址
    voice_settings_t settings = {0};
    form_field(body, "voice_url", settings.info_url, sizeof(settings.info_url));
    char followup[8];
    form_field(body, "voice_followup", followup, sizeof(followup));
    settings.followup = strcmp(followup, "1") == 0;
    esp_err_t voice_err = voice_settings_validate(&settings);
    if (voice_err != ESP_OK) {
        nvs_close(h);
        return reply(req, "语音地址无效", "请填写服务信息地址（OTA），例如 http://PC局域网IP:8000/xiaozhi/ota/", false);
    }
    esp_err_t err = nvs_set_str(h, "ssid", ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "pass", pass);
    }
    if (err == ESP_OK) {
        err = voice_settings_store(h, &settings);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to save portal settings: %s", esp_err_to_name(err));
        return httpd_resp_send_500(req);
    }

    ESP_LOGI(TAG, "stored credentials for %s", ssid);
    reply(req, "正在连接", "保存好了，屏幕稍后显示天气。", true);
    xEventGroupSetBits(portal_events(), CREDS_SAVED);
    return ESP_OK;
}

esp_err_t portal_web_start(void)
{
    xEventGroupClearBits(portal_events(), CREDS_SAVED);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard; // 默认是精确匹配，"/*" 需要通配匹配器
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_get};
    httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = save_post};
    // phones probe their own connectivity-check URLs; send them all to the config page
    httpd_uri_t any = {.uri = "/*", .method = HTTP_GET, .handler = any_get};
    err = httpd_register_uri_handler(s_server, &root);
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(s_server, &save);
    }
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(s_server, &any);
    }
    if (err != ESP_OK) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    return err;
}

void portal_web_wait(void)
{
    xEventGroupWaitBits(portal_events(), CREDS_SAVED, pdTRUE, pdTRUE, portMAX_DELAY);
}

void portal_web_stop(void)
{
    if (!s_server) {
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(300)); // let the response flush
    httpd_stop(s_server);
    s_server = NULL;
}
