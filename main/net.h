#pragma once
#include <stdbool.h>
#include <stddef.h>

void net_init(void);                                      // NVS + netif + wifi 事件
bool net_try_stored(int timeout_ms);                      // 用 NVS 里的凭据连 WiFi
bool net_has_stored(void);                                // NVS 里是否已有 WiFi 凭据
void net_forget_creds(void);                              // 清掉 NVS 里的 WiFi 凭据（长按重置）
void net_portal(void);                                    // SoftAP + 配网页，保存凭据后返回
const char *net_ap_ssid(void);                            // 热点名 "weather-XXXXXX"，后缀是 MAC
bool net_time_is_synced(void);                            // 本次启动已收到 SNTP 成功通知（含后台补对时）
void net_apply_tz(int utc_offset_seconds);                // 按出口 IP 时区设置 TZ
int net_http_get(const char *url, char *buf, size_t cap); // 返回字节数，<0 失败
void net_set_wifi_low_latency(bool enable);               // 语音通话期关闭 Wi-Fi 省电，空闲恢复

#define NET_AP_URL           "192.168.1.1"
#define NET_MIN_SYNCED_EPOCH 1704067200 // 2024-01-01 UTC
