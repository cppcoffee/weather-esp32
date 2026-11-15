#pragma once

// 使用 ESP-IDF captive portal DNS 组件，将所有 A 查询指向 SoftAP 地址。
void portal_dns_start(void);
void portal_dns_stop(void);
