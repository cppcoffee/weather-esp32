#include "net_http.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *log_tag = "net_http";

int net_http_get(const char *url, char *buf, size_t cap)
{
    if (!url || !buf || cap == 0) {
        return -1;
    }
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
    if (esp_http_client_open(c, 0) == ESP_OK && esp_http_client_fetch_headers(c) >= 0) {
        total = esp_http_client_read_response(c, buf, (int)cap - 1);
    }
    buf[total > 0 ? total : 0] = '\0';
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (total <= 0) {
        ESP_LOGW(log_tag, "empty response from %s", url);
        return -1;
    }
    return total;
}
