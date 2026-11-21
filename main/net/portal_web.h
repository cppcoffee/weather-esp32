#ifndef __PORTAL_WEB_H__
#define __PORTAL_WEB_H__

#include "esp_err.h"

// Starts the captive-portal HTTP server and registers all portal routes.
esp_err_t portal_web_start(void);

// Waits until the /save handler has stored WiFi credentials.
void portal_web_wait(void);

// Stops the captive-portal HTTP server.
void portal_web_stop(void);

#endif
