#include "net_portal.h"
#include "portal_dns.h"
#include "portal_web.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"

static const char *log_tag = "net";
static char portal_ssid[24];

const char *net_ap_ssid(void)
{
    // 后缀取 MAC 后三字节：手上几块板子同时上电时热点名字不会撞。
    // 开放热点（无密码）：手机连上会提示「不安全」，但免去输密码。
    if (!portal_ssid[0]) {
        uint8_t mac[6] = {0};
        esp_efuse_mac_get_default(mac);
        snprintf(portal_ssid, sizeof(portal_ssid), "weather-%02X%02X%02X", mac[3], mac[4], mac[5]);
    }
    return portal_ssid;
}

void net_portal(void)
{
    // Stop the previous station before starting the independent APSTA scan path.
    ESP_ERROR_CHECK(esp_wifi_stop());
    const char *name = net_ap_ssid();
    wifi_config_t ap = {.ap = {
                            .channel = 1,
                            .max_connection = 2,
                            .authmode = WIFI_AUTH_OPEN,
                        }};
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s", name);
    ap.ap.ssid_len = strlen(name);
    // Re-provisioning re-enters this function in the same boot; WIFI_AP_DEF may only be
    // created once, and esp_netif_new() asserts on a duplicate key.
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif == NULL) {
        ap_netif = esp_netif_create_default_wifi_ap();
    }
    // Static AP address: 192.168.1.1/24 with the DHCP server handing out 192.168.1.x.
    esp_netif_ip_info_t ip = {0};
    IP4_ADDR(&ip.ip, 192, 168, 1, 1);
    IP4_ADDR(&ip.gw, 192, 168, 1, 1);
    IP4_ADDR(&ip.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(ap_netif);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ip));
    // 下发给手机的 DNS 也是板子自己，好让 captive portal DNS 组件接管。
    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = ip.ip.addr;
    ESP_ERROR_CHECK(esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns));
    esp_netif_dhcps_start(ap_netif);

    // APSTA 而不是纯 AP：扫描周边热点需要 STA 模式。
    // Clear the previous station config before running portal scans.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    wifi_config_t no_sta = {0};
    if (esp_wifi_set_config(WIFI_IF_STA, &no_sta) != ESP_OK) {
        ESP_LOGW(log_tag, "could not clear STA config");
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(log_tag, "AP %s up, open http://%s", name, NET_AP_URL);

    portal_dns_start();
    ESP_ERROR_CHECK(portal_web_start());
    portal_web_wait();
    portal_web_stop();
    portal_dns_stop();
    esp_wifi_stop();
    ESP_LOGI(log_tag, "portal closed, switching to station");
}
