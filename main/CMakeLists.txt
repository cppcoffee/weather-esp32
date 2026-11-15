idf_component_register(SRCS "main.c" "button.c" "ui.c" "epd.c" "fonts_aa.c" "font_icons.c" "net.c" "net_portal.c" "portal_web.c" "portal_dns.c" "weather.c" "weather_json.c"
                       INCLUDE_DIRS "."
                       EMBED_TXTFILES "web/index.html" "web/result.html" "web/style.css" "web/app.js"
                       REQUIRES esp_driver_spi esp_driver_gpio esp_timer esp_wifi esp_netif esp_event esp_http_client esp_http_server esp-tls mbedtls nvs_flash cjson dns_server)
