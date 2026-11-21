#include "net_time.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"

static const char *log_tag = "net_time";
static atomic_bool time_synced;
static bool sntp_initialized; // Owned by the default event loop.

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

void net_time_start(void)
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
