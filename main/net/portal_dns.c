#include "portal_dns.h"

#include <stdbool.h>
#include <stddef.h>

#include "esp_netif.h"

// dns_server.h uses the IP address type declared by esp_netif.h.
#include "dns_server.h"

static dns_server_handle_t dns_server;

void
portal_dns_start(void)
{
    if (!dns_server) {
        static dns_server_config_t config = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
        dns_server = start_dns_server(&config);
    }
}

void
portal_dns_stop(void)
{
    if (dns_server) {
        stop_dns_server(dns_server);
        dns_server = NULL;
    }
}
