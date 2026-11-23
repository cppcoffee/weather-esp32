idf_component_register(SRCS "main.c"
                            "battery.c"
                            "device_mcp.c"
                            "sensor.c"
                            "voice/audio.c"
                            "voice/voice.c"
                            "voice/voice_feedback.c"
                            "voice/voice_capture.c"
                            "voice/voice_audio.c"
                            "voice/voice_settings.c"
                            "ui/ui.c"
                            "ui/epd.c"
                            "ui/fonts_aa.c"
                            "ui/font_clock.c"
                            "ui/font_icons.c"
                            "net/net.c"
                            "net/net_time.c"
                            "net/net_http.c"
                            "net/net_portal.c"
                            "net/portal_web.c"
                            "net/portal_dns.c"
                            "weather/weather.c"
                            "weather/weather_json.c"
                       INCLUDE_DIRS "." "net" "ui" "voice" "weather"
                       EMBED_TXTFILES "web/index.html"
                       REQUIRES esp_adc esp_codec_dev esp_xiaozhi mcp-c-sdk espressif__cjson espressif__button k0i05__esp_ahtxx k0i05__esp_bmp280 esp_driver_i2c esp_driver_i2s esp_driver_spi esp_driver_gpio esp_timer esp_wifi esp_netif esp_event esp_http_client esp_http_server esp-tls mbedtls nvs_flash dns_server http_parser
                       PRIV_REQUIRES esp-sr)

# Opus comes from the namespace-prefixed managed component; require it by its real name.
idf_build_get_property(build_components BUILD_COMPONENTS)
if("esphome__micro-opus" IN_LIST build_components)
    idf_component_get_property(opus_lib esphome__micro-opus COMPONENT_LIB)
    target_link_libraries(${COMPONENT_LIB} PRIVATE ${opus_lib})
endif()
