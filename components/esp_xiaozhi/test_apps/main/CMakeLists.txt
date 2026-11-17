idf_component_register(SRC_DIRS "."
                       PRIV_INCLUDE_DIRS "." "../../src/priv_include"
                       PRIV_REQUIRES unity test_utils esp_xiaozhi mcp-c-sdk nvs_flash esp_event cjson esp_websocket_client)

# Fault injection is test-only; wrappers forward to the real SDK outside their test cases.
foreach(symbol esp_websocket_client_init esp_websocket_register_events
               esp_websocket_client_start esp_websocket_client_stop esp_websocket_client_destroy
               esp_websocket_client_send_text esp_websocket_client_send_bin
               esp_mcp_mgr_req_handle esp_mcp_mgr_req_destroy_response esp_mcp_destroy)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=${symbol}")
endforeach()
