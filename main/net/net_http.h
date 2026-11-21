#ifndef __NET_HTTP_H__
#define __NET_HTTP_H__

#include <stddef.h>

// Synchronous HTTP GET (net_http.c)
int net_http_get(const char *url, char *buf, size_t cap); // 返回字节数，<0 失败

#endif
