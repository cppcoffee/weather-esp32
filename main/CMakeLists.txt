idf_component_register(SRCS "main.c" "device_mcp.c" "sensor.c" "audio.c" "voice.c" "voice_feedback.c" "voice_capture.c" "voice_audio.c" "voice_settings.c" "button.c" "ui.c" "epd.c" "fonts_aa.c" "font_clock.c" "font_icons.c" "net.c" "net_portal.c" "portal_web.c" "portal_dns.c" "weather.c" "weather_json.c"
                       INCLUDE_DIRS "."
                       EMBED_TXTFILES "web/index.html" "web/result.html" "web/style.css" "web/app.js"
                       REQUIRES esp_codec_dev esp_xiaozhi mcp-c-sdk espressif__cjson esp_driver_i2c esp_driver_i2s esp_driver_spi esp_driver_gpio esp_timer esp_wifi esp_netif esp_event esp_http_client esp_http_server esp-tls mbedtls nvs_flash dns_server http_parser
                       PRIV_REQUIRES esp-sr)

# Opus comes from the namespace-prefixed managed component; require it by its real name.
idf_build_get_property(build_components BUILD_COMPONENTS)
if("esphome__micro-opus" IN_LIST build_components)
    idf_component_get_property(opus_lib esphome__micro-opus COMPONENT_LIB)
    target_link_libraries(${COMPONENT_LIB} PRIVATE ${opus_lib})
endif()
