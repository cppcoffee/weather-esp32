/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <esp_check.h>
#include <esp_log.h>
#include <esp_transport.h>
#include <esp_transport_ssl.h>
#include <esp_transport_tcp.h>
#include <esp_transport_ws.h>
#include <esp_websocket_client.h>
#include <esp_crt_bundle.h>
#include <http_parser.h>
#include <limits.h>
#include <lwip/sockets.h>
#include <strings.h>

#include "esp_xiaozhi_keystore.h"
#include "esp_xiaozhi_board.h"
#include "esp_xiaozhi_websocket.h"
#include "esp_xiaozhi_payload.h"

typedef struct esp_xiaozhi_websocket_s {
    char url[ESP_XIAOZHI_WEBSOCKET_MAX_STRING_SIZE];
    char token[ESP_XIAOZHI_WEBSOCKET_MAX_STRING_SIZE];
    char protocol_version[16];
    char headers[ESP_XIAOZHI_WEBSOCKET_MAX_HEADER_SIZE];
    char ws_path[ESP_XIAOZHI_WEBSOCKET_MAX_STRING_SIZE];
    bool websocket_connected;
    bool client_started;

    esp_websocket_client_handle_t websocket_client;
    esp_transport_handle_t base_transport;
    esp_transport_handle_t ws_transport;
    esp_xiaozhi_transport_handle_t transport_handle; /*!< Passed to callbacks so chat can resolve by transport handle */
    esp_xiaozhi_transport_callbacks_t callbacks;

    esp_xiaozhi_payload_t payload;
    uint8_t payload_opcode;  /* 1=TEXT, 2=BINARY; 0=idle */
} esp_xiaozhi_websocket_t;

static const char *TAG = "ESP_XIAOZHI_WEBSOCKET";

#define WEBSOCKET_BUFFER_SIZE    4096
#define WEBSOCKET_SEND_BUDGET_MS 5000
#define WEBSOCKET_TASK_STACK     6144
#define WEBSOCKET_TASK_PRIO      6
#define WEBSOCKET_TASK_CORE      0

static void esp_xiaozhi_websocket_destroy_transport(esp_xiaozhi_websocket_t *websocket)
{
    if (websocket->ws_transport) {
        esp_transport_destroy(websocket->ws_transport);
        websocket->ws_transport = NULL;
    }
    if (websocket->base_transport) {
        esp_transport_destroy(websocket->base_transport);
        websocket->base_transport = NULL;
    }
}

static esp_transport_handle_t esp_xiaozhi_websocket_create_transport(esp_xiaozhi_websocket_t *websocket)
{
    struct http_parser_url puri;
    http_parser_url_init(&puri);
    if (http_parser_parse_url(websocket->url, strlen(websocket->url), 0, &puri) != 0) {
        return NULL;
    }
    if (puri.field_data[UF_SCHEMA].len == 0) {
        return NULL;
    }
    const char *scheme = websocket->url + puri.field_data[UF_SCHEMA].off;
    size_t scheme_len = puri.field_data[UF_SCHEMA].len;
    bool is_wss = (scheme_len == 3 && strncasecmp(scheme, "wss", 3) == 0);
    bool is_ws = (scheme_len == 2 && strncasecmp(scheme, "ws", 2) == 0);
    if (!is_ws && !is_wss) {
        return NULL;
    }

    int path_len = puri.field_data[UF_PATH].len;
    int query_len = puri.field_data[UF_QUERY].len;
    const char *path = websocket->url + puri.field_data[UF_PATH].off;
    const char *query = websocket->url + puri.field_data[UF_QUERY].off;
    int written = 0;
    if (path_len > 0 && query_len > 0) {
        written = snprintf(websocket->ws_path, sizeof(websocket->ws_path), "%.*s?%.*s", path_len, path, query_len, query);
    } else if (path_len > 0) {
        written = snprintf(websocket->ws_path, sizeof(websocket->ws_path), "%.*s", path_len, path);
    } else if (query_len > 0) {
        written = snprintf(websocket->ws_path, sizeof(websocket->ws_path), "/?%.*s", query_len, query);
    } else {
        written = snprintf(websocket->ws_path, sizeof(websocket->ws_path), "/");
    }
    if (written <= 0 || (size_t)written >= sizeof(websocket->ws_path)) {
        return NULL;
    }

    esp_transport_handle_t base = is_wss ? esp_transport_ssl_init() : esp_transport_tcp_init();
    if (!base) {
        return NULL;
    }
    int default_port = is_wss ? 443 : 80;
    esp_transport_set_default_port(base, default_port);
    if (is_wss) {
        esp_transport_ssl_crt_bundle_attach(base, esp_crt_bundle_attach);
    }

    esp_transport_handle_t ws = esp_transport_ws_init(base);
    if (!ws) {
        esp_transport_destroy(base);
        return NULL;
    }
    esp_transport_set_default_port(ws, default_port);
    const esp_transport_ws_config_t ws_cfg = {
        .ws_path = websocket->ws_path,
        .headers = websocket->headers,
        .propagate_control_frames = true,
    };
    if (esp_transport_ws_set_config(ws, &ws_cfg) != ESP_OK) {
        esp_transport_destroy(ws);
        esp_transport_destroy(base);
        return NULL;
    }

    websocket->base_transport = base;
    websocket->ws_transport = ws;
    return ws;
}

static TickType_t esp_xiaozhi_websocket_send_timeout(size_t len)
{
    /* The SDK reuses timeout for the TX lock and each fragment; IDF's WS transport
     * also uses it separately for poll, header and payload writes. Budget those
     * waits together, rather than allowing 5 s for every 1 KB fragment. */
    size_t fragments = len ? 1 + (len - 1) / WEBSOCKET_BUFFER_SIZE : 1;
    return pdMS_TO_TICKS(WEBSOCKET_SEND_BUDGET_MS) / (1 + 3 * fragments);
}

static esp_err_t esp_xiaozhi_websocket_map_missing_config_error(esp_err_t err, const char *field_name)
{
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Required WebSocket config '%s' is missing", field_name);
        return ESP_ERR_NOT_FOUND;
    }
    return err;
}

static void esp_xiaozhi_websocket_build_headers(esp_xiaozhi_websocket_t *websocket)
{
    esp_xiaozhi_chat_board_info_t board_info = {0};
    esp_err_t ret = esp_xiaozhi_chat_get_board_info(&board_info);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get board info: %s", esp_err_to_name(ret));
    }

    websocket->headers[0] = '\0';
    if (strlen(websocket->token) > 0) {
        if (strchr(websocket->token, ' ') == NULL) {
            snprintf(websocket->headers + strlen(websocket->headers),
                     sizeof(websocket->headers) - strlen(websocket->headers),
                     "Authorization: Bearer %s\r\n", websocket->token);
        } else {
            snprintf(websocket->headers + strlen(websocket->headers),
                     sizeof(websocket->headers) - strlen(websocket->headers),
                     "Authorization: %s\r\n", websocket->token);
        }
    }

    snprintf(websocket->headers + strlen(websocket->headers),
             sizeof(websocket->headers) - strlen(websocket->headers),
             "Protocol-Version: %s\r\n", websocket->protocol_version);

    if (board_info.initialized) {
        snprintf(websocket->headers + strlen(websocket->headers),
                 sizeof(websocket->headers) - strlen(websocket->headers),
                 "Device-Id: %s\r\nClient-Id: %s\r\n",
                 board_info.mac_address, board_info.uuid);
    }
}

static void esp_xiaozhi_websocket_event(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)base;
    esp_xiaozhi_websocket_t *websocket = (esp_xiaozhi_websocket_t *)handler_args;
    esp_websocket_event_data_t *event = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGD(TAG, "WEBSOCKET_EVENT_CONNECTED");
        if (websocket->base_transport) {
            int fd = esp_transport_get_socket(websocket->base_transport);
            if (fd >= 0) {
                int one = 1;
                if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) != 0) {
                    ESP_LOGW(TAG, "Failed to set TCP_NODELAY on fd=%d (errno=%d)", fd, errno);
                }
            }
        }
        websocket->websocket_connected = true;
        if (websocket->callbacks.on_connected_callback) {
            websocket->callbacks.on_connected_callback(websocket->transport_handle);
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "WEBSOCKET_EVENT_DISCONNECTED");
        websocket->websocket_connected = false;
        esp_xiaozhi_payload_clear(&websocket->payload);
        websocket->payload_opcode = 0;
        if (websocket->callbacks.on_disconnected_callback) {
            websocket->callbacks.on_disconnected_callback(websocket->transport_handle);
        }
        break;

    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGD(TAG, "WEBSOCKET_EVENT_CLOSED");
        websocket->websocket_connected = false;
        esp_xiaozhi_payload_clear(&websocket->payload);
        websocket->payload_opcode = 0;
        if (websocket->callbacks.on_disconnected_callback) {
            websocket->callbacks.on_disconnected_callback(websocket->transport_handle);
        }
        break;

    case WEBSOCKET_EVENT_FINISH:
        ESP_LOGD(TAG, "WEBSOCKET_EVENT_FINISH");
        websocket->websocket_connected = false;
        esp_xiaozhi_payload_clear(&websocket->payload);
        websocket->payload_opcode = 0;
        if (websocket->callbacks.on_disconnected_callback) {
            websocket->callbacks.on_disconnected_callback(websocket->transport_handle);
        }
        break;

    case WEBSOCKET_EVENT_DATA: {
        /* Only handle WebSocket protocol-layer fragmentation here: reassemble frames (fin + payload_offset
         * /payload_len) into one complete WS message, then pass to upper layer. Application/business-layer
         * message completeness is the responsibility of the application layer. */
        uint8_t op = (uint8_t)event->op_code;
        bool fin = event->fin;
        const char *data = event->data_ptr;
        int data_len = event->data_len;
        int payload_offset = event->payload_offset;
        int payload_len = event->payload_len;

        if (op != WS_OPCODE_TEXT && op != WS_OPCODE_BINARY && op != WS_OPCODE_CONTINUATION) {
            ESP_LOGD(TAG, "Ignore websocket opcode=%d len=%d", op, data_len);
            break;
        }
        if (!data || data_len <= 0) {
            break;
        }

        /* Single-frame message: no append payload needed */
        if ((op == WS_OPCODE_TEXT || op == WS_OPCODE_BINARY) && fin && websocket->payload.buf == NULL) {
            if (op == WS_OPCODE_TEXT && websocket->callbacks.on_message_callback) {
                websocket->callbacks.on_message_callback(websocket->transport_handle, NULL, 0, data, (size_t)data_len);
            } else if (op == WS_OPCODE_BINARY && websocket->callbacks.on_binary_callback) {
                websocket->callbacks.on_binary_callback(websocket->transport_handle, (const uint8_t *)data, (size_t)data_len);
            }
            break;
        }

        /* Multi-frame: append payload until fin */
        if (op == WS_OPCODE_CONTINUATION && websocket->payload_opcode == 0) {
            ESP_LOGW(TAG, "Unexpected continuation frame without prior TEXT/BINARY");
            break;
        }
        if (op == WS_OPCODE_TEXT || op == WS_OPCODE_BINARY) {
            websocket->payload_opcode = op;
        }
        esp_err_t err = esp_xiaozhi_payload_append(&websocket->payload, data, data_len, payload_offset, payload_len, ESP_XIAOZHI_PAYLOAD_MAX_DEFAULT);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "WebSocket payload append failed: %s, offset=%d total=%d chunk=%d opcode=%d",
                     esp_err_to_name(err), payload_offset, payload_len, data_len, op);
            esp_xiaozhi_payload_clear(&websocket->payload);
            websocket->payload_opcode = 0;
            if (websocket->callbacks.on_error_callback) {
                websocket->callbacks.on_error_callback(websocket->transport_handle, err);
            }
            break;
        }
        if (fin) {
            esp_xiaozhi_payload_t *r = &websocket->payload;
            uint8_t o = websocket->payload_opcode;
            if (o == WS_OPCODE_TEXT && websocket->callbacks.on_message_callback) {
                websocket->callbacks.on_message_callback(websocket->transport_handle, NULL, 0, (const char *)r->buf, r->len);
            } else if (o == WS_OPCODE_BINARY && websocket->callbacks.on_binary_callback) {
                websocket->callbacks.on_binary_callback(websocket->transport_handle, r->buf, r->len);
            }
            esp_xiaozhi_payload_clear(&websocket->payload);
            websocket->payload_opcode = 0;
        }
        break;
    }

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WEBSOCKET_EVENT_ERROR");
        break;

    default:
        ESP_LOGD(TAG, "Other websocket event id: %" PRId32, event_id);
        break;
    }
}

esp_err_t esp_xiaozhi_websocket_init(esp_xiaozhi_websocket_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");

    esp_xiaozhi_websocket_t *websocket = calloc(1, sizeof(esp_xiaozhi_websocket_t));
    ESP_RETURN_ON_FALSE(websocket, ESP_ERR_NO_MEM, TAG, "Failed to allocate memory for websocket");

    *handle = (esp_xiaozhi_websocket_handle_t)websocket;

    return ESP_OK;
}

esp_err_t esp_xiaozhi_websocket_deinit(esp_xiaozhi_websocket_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");

    esp_xiaozhi_websocket_t *websocket = (esp_xiaozhi_websocket_t *)handle;
    /* Join before releasing payload/callback state. SDK destroy alone ignores a
     * failed stop (notably when called from its own event callback). */
    ESP_RETURN_ON_ERROR(esp_xiaozhi_websocket_stop(handle), TAG, "Failed to stop before destroy");
    if (websocket->websocket_client) {
        ESP_RETURN_ON_ERROR(esp_websocket_client_destroy(websocket->websocket_client), TAG, "Failed to destroy client");
        websocket->websocket_client = NULL;
    }
    esp_xiaozhi_websocket_destroy_transport(websocket);
    esp_xiaozhi_payload_clear(&websocket->payload);

    free(websocket);
    return ESP_OK;
}

esp_err_t esp_xiaozhi_websocket_start(esp_xiaozhi_websocket_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");

    esp_xiaozhi_chat_keystore_t xiaozhi_chat_keystore;
    esp_xiaozhi_websocket_t *websocket = (esp_xiaozhi_websocket_t *)handle;
    esp_err_t ret = esp_xiaozhi_chat_keystore_init(&xiaozhi_chat_keystore, "websocket", false);
    ret = esp_xiaozhi_websocket_map_missing_config_error(ret, "namespace");
    ESP_RETURN_ON_FALSE(ret == ESP_OK, ret, TAG, "Failed to initialize keystore: %s", esp_err_to_name(ret));

    int protocol_version = 1;
    esp_xiaozhi_chat_keystore_get_string(&xiaozhi_chat_keystore, "url", "", websocket->url, sizeof(websocket->url));
    esp_xiaozhi_chat_keystore_get_string(&xiaozhi_chat_keystore, "token", "", websocket->token, sizeof(websocket->token));
    esp_xiaozhi_chat_keystore_get_int(&xiaozhi_chat_keystore, "version", 1, (int32_t *)&protocol_version);
    esp_xiaozhi_chat_keystore_deinit(&xiaozhi_chat_keystore);

    ESP_RETURN_ON_FALSE(strlen(websocket->url) > 0, ESP_ERR_NOT_FOUND, TAG, "Websocket url is not specified");

    snprintf(websocket->protocol_version, sizeof(websocket->protocol_version), "%d", protocol_version);
    esp_xiaozhi_websocket_build_headers(websocket);

    if (websocket->websocket_client) {
        ESP_RETURN_ON_ERROR(esp_xiaozhi_websocket_stop(handle), TAG, "Failed to stop previous client");
        ESP_RETURN_ON_ERROR(esp_websocket_client_destroy(websocket->websocket_client), TAG,
                            "Failed to destroy previous client");
        websocket->websocket_client = NULL;
    }
    esp_xiaozhi_websocket_destroy_transport(websocket);

    esp_websocket_client_config_t websocket_cfg = {
        .uri = websocket->url,
        .headers = websocket->headers,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_reconnect = false,
        .enable_close_reconnect = true,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = WEBSOCKET_SEND_BUDGET_MS,
        .buffer_size = WEBSOCKET_BUFFER_SIZE,
        .task_stack = WEBSOCKET_TASK_STACK,
        .task_prio = WEBSOCKET_TASK_PRIO,
        .task_core_id_set = true,
        .task_core_id = WEBSOCKET_TASK_CORE,
        .ext_transport = esp_xiaozhi_websocket_create_transport(websocket),
    };

    websocket->websocket_client = esp_websocket_client_init(&websocket_cfg);
    if (!websocket->websocket_client) {
        esp_xiaozhi_websocket_destroy_transport(websocket);
        ESP_LOGE(TAG, "Failed to initialize websocket client");
        return ESP_ERR_NO_MEM;
    }

    ret = esp_websocket_register_events(websocket->websocket_client, WEBSOCKET_EVENT_ANY, esp_xiaozhi_websocket_event, websocket);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register websocket events: %s", esp_err_to_name(ret));
        esp_websocket_client_destroy(websocket->websocket_client);
        websocket->websocket_client = NULL;
        esp_xiaozhi_websocket_destroy_transport(websocket);
        return ret;
    }

    websocket->client_started = true;
    ret = esp_websocket_client_start(websocket->websocket_client);
    if (ret) {
        websocket->client_started = false;
        ESP_LOGE(TAG, "Failed to start websocket client: %s", esp_err_to_name(ret));
        esp_websocket_client_destroy(websocket->websocket_client);
        websocket->websocket_client = NULL;
        esp_xiaozhi_websocket_destroy_transport(websocket);
        return ret;
    }

    return ESP_OK;
}

esp_err_t esp_xiaozhi_websocket_stop(esp_xiaozhi_websocket_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");

    esp_xiaozhi_websocket_t *websocket = (esp_xiaozhi_websocket_t *)handle;
    if (!websocket->websocket_client) {
        return ESP_OK;
    }

    if (websocket->client_started) {
        esp_err_t ret = esp_websocket_client_stop(websocket->websocket_client);
        ESP_RETURN_ON_FALSE(ret == ESP_OK, ret, TAG, "Failed to stop websocket client: %s", esp_err_to_name(ret));
        websocket->client_started = false;
    }
    websocket->websocket_connected = false;
    esp_xiaozhi_payload_clear(&websocket->payload);
    websocket->payload_opcode = 0;

    return ESP_OK;
}

esp_err_t esp_xiaozhi_websocket_send_text(esp_xiaozhi_websocket_handle_t handle, const char *text)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");
    ESP_RETURN_ON_FALSE(text, ESP_ERR_INVALID_ARG, TAG, "Invalid text");

    esp_xiaozhi_websocket_t *websocket = (esp_xiaozhi_websocket_t *)handle;
    ESP_RETURN_ON_FALSE(websocket->websocket_client, ESP_ERR_INVALID_STATE, TAG, "Websocket client not initialized");
    ESP_RETURN_ON_FALSE(websocket->websocket_connected, ESP_ERR_INVALID_STATE, TAG, "Websocket is not connected");

    size_t len = strlen(text);
    ESP_RETURN_ON_FALSE(len <= INT_MAX, ESP_ERR_INVALID_SIZE, TAG, "Text too large");
    int sent = esp_websocket_client_send_text(websocket->websocket_client, text, (int)len,
                                             esp_xiaozhi_websocket_send_timeout(len));
    if (sent < 0 || (size_t)sent != len) {
        websocket->websocket_connected = false;
        if (websocket->callbacks.on_disconnected_callback) {
            websocket->callbacks.on_disconnected_callback((esp_xiaozhi_transport_handle_t)websocket->transport_handle);
        }
        ESP_LOGE(TAG, "Failed to send text frame");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t esp_xiaozhi_websocket_send_binary(esp_xiaozhi_websocket_handle_t handle, const uint8_t *data, size_t data_len)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");
    ESP_RETURN_ON_FALSE(data, ESP_ERR_INVALID_ARG, TAG, "Invalid data");
    ESP_RETURN_ON_FALSE(data_len > 0, ESP_ERR_INVALID_ARG, TAG, "Invalid data_len");

    esp_xiaozhi_websocket_t *websocket = (esp_xiaozhi_websocket_t *)handle;
    ESP_RETURN_ON_FALSE(websocket->websocket_client, ESP_ERR_INVALID_STATE, TAG, "Websocket client not initialized");
    ESP_RETURN_ON_FALSE(websocket->websocket_connected, ESP_ERR_INVALID_STATE, TAG, "Websocket is not connected");

    ESP_RETURN_ON_FALSE(data_len <= INT_MAX, ESP_ERR_INVALID_SIZE, TAG, "Binary data too large");
    int sent = esp_websocket_client_send_bin(websocket->websocket_client, (const char *)data, (int)data_len,
                                            esp_xiaozhi_websocket_send_timeout(data_len));
    if (sent < 0 || (size_t)sent != data_len) {
        websocket->websocket_connected = false;
        if (websocket->callbacks.on_disconnected_callback) {
            websocket->callbacks.on_disconnected_callback((esp_xiaozhi_transport_handle_t)websocket->transport_handle);
        }
        ESP_LOGE(TAG, "Failed to send binary frame");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t esp_xiaozhi_websocket_set_callbacks(esp_xiaozhi_websocket_handle_t handle, esp_xiaozhi_transport_handle_t transport_handle, const esp_xiaozhi_transport_callbacks_t *callbacks)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");
    ESP_RETURN_ON_FALSE(callbacks, ESP_ERR_INVALID_ARG, TAG, "Invalid callbacks");

    esp_xiaozhi_websocket_t *websocket = (esp_xiaozhi_websocket_t *)handle;
    websocket->transport_handle = transport_handle;
    websocket->callbacks = *callbacks;

    return ESP_OK;
}
