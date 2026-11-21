#ifndef __NET_TIME_H__
#define __NET_TIME_H__

#include <stdbool.h>
#include <time.h>

#define NET_MIN_SYNCED_EPOCH 1704067200 // 2024-01-01 UTC

// SNTP time sync and timezone (net_time.c)
void net_time_start(void);                 // STA 联网后启动或重启后台 SNTP
bool net_time_is_synced(void);             // 本次启动已收到 SNTP 成功通知（含后台补对时）
void net_apply_tz(int utc_offset_seconds); // 按出口 IP 时区设置 TZ

// Absolute timestamps are only meaningful after this boot's SNTP sync.
static inline time_t net_synchronized_time(void)
{
    time_t now = net_time_is_synced() ? time(NULL) : 0;
    return now >= NET_MIN_SYNCED_EPOCH ? now : 0;
}

#endif
