#ifndef __NET_H__
#define __NET_H__

#include <stdbool.h>

// Wi-Fi station lifecycle and stored credentials (net.c)
void net_init(void);                        // NVS + netif + wifi 事件
bool net_try_stored(int timeout_ms);        // 用 NVS 里的凭据连 WiFi
bool net_has_stored(void);                  // NVS 里是否已有 WiFi 凭据
bool net_has_ip(void);                      // STA 已取得 IP
void net_request_reconnect(void);           // 有语音请求时提前执行下一次重连
void net_forget_creds(void);                // 清掉 NVS 里的 WiFi 凭据（长按重置）
void net_set_wifi_low_latency(bool enable); // 语音通话期关闭 Wi-Fi 省电，空闲恢复

typedef struct {
    bool connected;
    char ssid[33];
    int  rssi;
} net_status_t;

void net_get_status(net_status_t *out); // 读取当前 Wi-Fi 连接状态、SSID 与 RSSI

#endif
