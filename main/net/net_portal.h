#ifndef __NET_PORTAL_H__
#define __NET_PORTAL_H__

#define NET_AP_URL "192.168.1.1"

// Captive portal provisioning (net_portal.c)
void net_portal(void);         // SoftAP + 配网页，保存凭据后返回
const char *net_ap_ssid(void); // 热点名 "weather-XXXXXX"，后缀是 MAC

#endif
