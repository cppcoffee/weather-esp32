/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "cJSON.h"
#include "esp_event.h"
#include "esp_websocket_client.h"
#include "esp_xiaozhi_chat.h"
#include "esp_xiaozhi_mcp.h"
#include "esp_xiaozhi_websocket.h"
#include "nvs.h"
#include "unity.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Linker wrappers forward to the SDK except while these tests are active.
 * Chat, MCP wrapping, transport routing and WebSocket adaptation remain real. */
static struct {
    bool active;
    bool in_callback;
    bool joined;
    bool deinit_on_error;
    esp_event_handler_t handler;
    void *handler_arg;
    esp_xiaozhi_chat_handle_t chat;
    esp_err_t stop_result;
    esp_err_t callback_deinit_result;
    esp_err_t manager_result;
    esp_err_t engine_destroy_result;
    int engines_destroyed;
    esp_mcp_mgr_handle_t manager;
    const char *response;
    int send_result;
    int sends;
    int responses_freed;
    int stops;
    int destroys;
    int errors;
    TickType_t timeout;
    int buffer_size;
    esp_err_t error_code;
    char error_source[32];
} s;

static char fake_client;
#define FAKE_CLIENT ((esp_websocket_client_handle_t) & fake_client)

esp_websocket_client_handle_t __real_esp_websocket_client_init(const esp_websocket_client_config_t *config);
esp_websocket_client_handle_t __wrap_esp_websocket_client_init(const esp_websocket_client_config_t *config)
{
    if (!s.active) {
        return __real_esp_websocket_client_init(config);
    }
    s.buffer_size = config->buffer_size;
    TEST_ASSERT_EQUAL(4096, config->buffer_size);
    TEST_ASSERT_EQUAL(5000, config->network_timeout_ms);
    return FAKE_CLIENT;
}

esp_err_t __real_esp_websocket_register_events(esp_websocket_client_handle_t client, esp_websocket_event_id_t event,
                                               esp_event_handler_t handler, void *arg);
esp_err_t __wrap_esp_websocket_register_events(esp_websocket_client_handle_t client, esp_websocket_event_id_t event,
                                               esp_event_handler_t handler, void *arg)
{
    if (!s.active) {
        return __real_esp_websocket_register_events(client, event, handler, arg);
    }
    TEST_ASSERT_EQUAL_PTR(FAKE_CLIENT, client);
    s.handler = handler;
    s.handler_arg = arg;
    return ESP_OK;
}

esp_err_t __real_esp_websocket_client_start(esp_websocket_client_handle_t client);
esp_err_t __wrap_esp_websocket_client_start(esp_websocket_client_handle_t client)
{
    return s.active ? ESP_OK : __real_esp_websocket_client_start(client);
}

esp_err_t __real_esp_websocket_client_stop(esp_websocket_client_handle_t client);
esp_err_t __wrap_esp_websocket_client_stop(esp_websocket_client_handle_t client)
{
    if (!s.active) {
        return __real_esp_websocket_client_stop(client);
    }
    s.stops++;
    /* Model the SDK's rejection of stop from its own event callback. */
    if (s.in_callback) {
        return ESP_FAIL;
    }
    if (s.stop_result == ESP_OK) {
        s.joined = true;
    }
    return s.stop_result;
}

esp_err_t __real_esp_websocket_client_destroy(esp_websocket_client_handle_t client);
esp_err_t __wrap_esp_websocket_client_destroy(esp_websocket_client_handle_t client)
{
    if (!s.active) {
        return __real_esp_websocket_client_destroy(client);
    }
    TEST_ASSERT_TRUE_MESSAGE(s.joined, "destroy must follow successful stop/join");
    TEST_ASSERT_FALSE(s.in_callback);
    s.destroys++;
    return ESP_OK;
}

static int stub_send(const char *data, int len, TickType_t timeout, bool text)
{
    TEST_ASSERT_NOT_EQUAL(portMAX_DELAY, timeout);
    TEST_ASSERT_GREATER_THAN(0, timeout);
    size_t fragments = len ? 1 + ((size_t)len - 1) / s.buffer_size : 1;
    TEST_ASSERT_TRUE((1 + 3 * fragments) * timeout <= pdMS_TO_TICKS(5000));
    s.timeout = timeout;
    s.sends++;
    if (text && s.response) {
        TEST_ASSERT_GREATER_THAN(3000, len);
        cJSON *root = cJSON_ParseWithLength(data, len);
        TEST_ASSERT_NOT_NULL(root);
        TEST_ASSERT_EQUAL_STRING("mcp", cJSON_GetStringValue(cJSON_GetObjectItem(root, "type")));
        TEST_ASSERT_TRUE(cJSON_IsObject(cJSON_GetObjectItem(root, "payload")));
        cJSON_Delete(root);
    }
    return s.send_result == INT_MAX ? len : s.send_result;
}

int __real_esp_websocket_client_send_text(esp_websocket_client_handle_t client, const char *data, int len,
                                          TickType_t timeout);
int __wrap_esp_websocket_client_send_text(esp_websocket_client_handle_t client, const char *data, int len,
                                          TickType_t timeout)
{
    return s.active ? stub_send(data, len, timeout, true)
                    : __real_esp_websocket_client_send_text(client, data, len, timeout);
}

int __real_esp_websocket_client_send_bin(esp_websocket_client_handle_t client, const char *data, int len,
                                         TickType_t timeout);
int __wrap_esp_websocket_client_send_bin(esp_websocket_client_handle_t client, const char *data, int len,
                                         TickType_t timeout)
{
    return s.active ? stub_send(data, len, timeout, false)
                    : __real_esp_websocket_client_send_bin(client, data, len, timeout);
}

esp_err_t __real_esp_mcp_mgr_req_handle(esp_mcp_mgr_handle_t handle, const char *endpoint, const uint8_t *in,
                                        uint16_t len, uint8_t **out, uint16_t *outlen);
esp_err_t __wrap_esp_mcp_mgr_req_handle(esp_mcp_mgr_handle_t handle, const char *endpoint, const uint8_t *in,
                                        uint16_t len, uint8_t **out, uint16_t *outlen)
{
    if (!s.active) {
        return __real_esp_mcp_mgr_req_handle(handle, endpoint, in, len, out, outlen);
    }
    s.manager = handle;
    *out = s.response ? (uint8_t *)strdup(s.response) : NULL;
    *outlen = s.response ? strlen(s.response) + 1 : 0;
    TEST_ASSERT_TRUE(!s.response || *out);
    return s.manager_result;
}

esp_err_t __real_esp_mcp_mgr_req_destroy_response(esp_mcp_mgr_handle_t handle, uint8_t *response);
esp_err_t __wrap_esp_mcp_mgr_req_destroy_response(esp_mcp_mgr_handle_t handle, uint8_t *response)
{
    if (!s.active) {
        return __real_esp_mcp_mgr_req_destroy_response(handle, response);
    }
    s.responses_freed++;
    free(response);
    return ESP_OK;
}

esp_err_t __real_esp_mcp_destroy(esp_mcp_t *engine);
esp_err_t __wrap_esp_mcp_destroy(esp_mcp_t *engine)
{
    esp_err_t ret = __real_esp_mcp_destroy(engine);
    if (s.active) {
        s.engines_destroyed++;
        if (ret == ESP_OK) {
            /* Inject a cleanup status only after the real engine has been freed.
             * This exercises chat's consume-even-on-cleanup-error contract. */
            return s.engine_destroy_result;
        }
    }
    return ret;
}

static void error_event(esp_xiaozhi_chat_event_t event, void *data, void *ctx)
{
    if (event != ESP_XIAOZHI_CHAT_EVENT_CHAT_ERROR) {
        return;
    }
    esp_xiaozhi_chat_error_info_t *info = data;
    s.errors++;
    s.error_code = info->code;
    snprintf(s.error_source, sizeof(s.error_source), "%s", info->source);
    if (s.deinit_on_error) {
        s.callback_deinit_result = esp_xiaozhi_chat_deinit(s.chat);
    }
}

static void emit(esp_websocket_event_id_t id, esp_websocket_event_data_t *event)
{
    s.in_callback = true;
    s.handler(s.handler_arg, WEBSOCKET_EVENTS, id, event);
    s.in_callback = false;
}

static void begin_stub(void)
{
    memset(&s, 0, sizeof(s));
    s.active = true;
    s.send_result = INT_MAX;
    nvs_handle_t nvs;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_open("websocket", NVS_READWRITE, &nvs));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_str(nvs, "url", "wss://example.invalid/test"));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_commit(nvs));
    nvs_close(nvs);
}

static void start_chat(void)
{
    begin_stub();
    esp_xiaozhi_chat_config_t config = ESP_XIAOZHI_CHAT_DEFAULT_CONFIG();
    config.has_websocket_config = true;
    config.event_callback = error_event;
    TEST_ASSERT_EQUAL(ESP_OK, esp_mcp_create(&config.mcp_engine));
    config.owns_mcp_engine = true;
    TEST_ASSERT_EQUAL(ESP_OK, esp_xiaozhi_chat_init(&config, &s.chat));
    TEST_ASSERT_EQUAL(ESP_OK, esp_xiaozhi_chat_start(s.chat));
    emit(WEBSOCKET_EVENT_CONNECTED, NULL);
}

static void receive_mcp(void)
{
    static const char request[] =
        "{\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}}";
    esp_websocket_event_data_t event = {
        .data_ptr = request,
        .data_len = sizeof(request) - 1,
        .payload_len = sizeof(request) - 1,
        .fin = true,
        .op_code = WS_OPCODE_TEXT,
    };
    emit(WEBSOCKET_EVENT_DATA, &event);
}

static void make_large_response(char *response, size_t size)
{
    memset(response, 'w', size - 1);
    const char *prefix = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"weather\":\"";
    memcpy(response, prefix, strlen(prefix));
    memcpy(response + size - 4, "\"}}", 4);
}

TEST_CASE("MCP 3KB response uses finite send budget and releases response", "[esp_xiaozhi][backpressure]")
{
    start_chat();
    char response[3100];
    make_large_response(response, sizeof(response));
    s.response = response;
    receive_mcp();
    TEST_ASSERT_EQUAL(1, s.sends);
    TEST_ASSERT_EQUAL(1, s.responses_freed);
    TEST_ASSERT_EQUAL(0, s.errors);
    TEST_ASSERT_EQUAL(ESP_OK, esp_xiaozhi_chat_deinit(s.chat));
    TEST_ASSERT_EQUAL(1, s.destroys);
    s.active = false;
}

TEST_CASE("MCP negative zero and short sends reach CHAT_ERROR", "[esp_xiaozhi][backpressure]")
{
    const int results[] = {-1, 0, 100};
    for (size_t i = 0; i < sizeof(results) / sizeof(results[0]); i++) {
        start_chat();
        char response[3100];
        make_large_response(response, sizeof(response));
        s.response = response;
        s.send_result = results[i];
        receive_mcp();
        TEST_ASSERT_EQUAL(1, s.sends);
        TEST_ASSERT_EQUAL(1, s.responses_freed);
        TEST_ASSERT_EQUAL(1, s.errors);
        TEST_ASSERT_EQUAL(ESP_FAIL, s.error_code);
        TEST_ASSERT_EQUAL_STRING("mcp", s.error_source);
        /* Also assert the adapter's return, not just the chat callback side effect.
         * The first failed send has marked the socket disconnected. */
        TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                          esp_xiaozhi_mcp_handle_message(s.manager, "mcp_server", "{}", 2, NULL));
        TEST_ASSERT_EQUAL(2, s.responses_freed);
        TEST_ASSERT_EQUAL(ESP_OK, esp_xiaozhi_chat_deinit(s.chat));
        TEST_ASSERT_EQUAL(1, s.destroys);
        s.active = false;
    }
}

TEST_CASE("MCP JSON-RPC error reply is delivered without chat failure", "[esp_xiaozhi][backpressure]")
{
    start_chat();
    char response[3100];
    make_large_response(response, sizeof(response));
    const char *prefix = "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32601,\"message\":\"";
    memcpy(response, prefix, strlen(prefix));
    s.response = response;
    s.manager_result = ESP_ERR_NOT_FOUND;
    receive_mcp();
    TEST_ASSERT_EQUAL(1, s.sends);
    TEST_ASSERT_EQUAL(1, s.responses_freed);
    TEST_ASSERT_EQUAL(0, s.errors);
    TEST_ASSERT_EQUAL(ESP_OK, esp_xiaozhi_chat_deinit(s.chat));
    s.active = false;
}

TEST_CASE("MCP no-response processing failure propagates", "[esp_xiaozhi][backpressure]")
{
    start_chat();
    receive_mcp(); /* Successful notification: no response, no send, no error. */
    TEST_ASSERT_EQUAL(0, s.errors);
    s.manager_result = ESP_ERR_NO_MEM;
    receive_mcp();
    TEST_ASSERT_EQUAL(0, s.sends);
    TEST_ASSERT_EQUAL(1, s.errors);
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, s.error_code);
    TEST_ASSERT_EQUAL(ESP_OK, esp_xiaozhi_chat_deinit(s.chat));
    s.active = false;
}

TEST_CASE("callback deinit rejection preserves live chat for safe retry", "[esp_xiaozhi][backpressure]")
{
    start_chat();
    char response[3100];
    make_large_response(response, sizeof(response));
    s.response = response;
    s.send_result = -1;
    s.deinit_on_error = true;
    receive_mcp();
    TEST_ASSERT_EQUAL(ESP_FAIL, s.callback_deinit_result);
    TEST_ASSERT_EQUAL(0, s.destroys);
    TEST_ASSERT_EQUAL(1, s.responses_freed);
    TEST_ASSERT_EQUAL(ESP_OK, esp_xiaozhi_chat_deinit(s.chat));
    TEST_ASSERT_EQUAL(1, s.destroys);
    s.active = false;
}

TEST_CASE("WS two-phase stop retry retains external MCP until teardown completes", "[esp_xiaozhi][backpressure]")
{
    begin_stub();
    esp_xiaozhi_chat_config_t config = ESP_XIAOZHI_CHAT_DEFAULT_CONFIG();
    config.has_websocket_config = true;
    config.event_callback = error_event;
    TEST_ASSERT_EQUAL(ESP_OK, esp_mcp_create(&config.mcp_engine));
    config.owns_mcp_engine = false;
    TEST_ASSERT_EQUAL(ESP_OK, esp_xiaozhi_chat_init(&config, &s.chat));
    TEST_ASSERT_EQUAL(ESP_OK, esp_xiaozhi_chat_start(s.chat));
    emit(WEBSOCKET_EVENT_CONNECTED, NULL);

    s.stop_result = ESP_ERR_TIMEOUT;
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, esp_xiaozhi_chat_stop(s.chat));
    TEST_ASSERT_EQUAL(0, s.destroys);
    TEST_ASSERT_EQUAL(0, s.engines_destroyed);
    /* Callbacks and the external engine are still live while cleanup is pending. */
    receive_mcp();
    TEST_ASSERT_EQUAL(0, s.errors);
    TEST_ASSERT_EQUAL(ESP_OK, esp_mcp_set_server_info(config.mcp_engine, "retained", NULL, NULL, NULL));

    s.stop_result = ESP_OK;
    TEST_ASSERT_EQUAL(ESP_OK, esp_xiaozhi_chat_stop(s.chat));
    TEST_ASSERT_EQUAL(2, s.stops);
    TEST_ASSERT_EQUAL(1, s.destroys);
    TEST_ASSERT_EQUAL(0, s.engines_destroyed);
    TEST_ASSERT_EQUAL(ESP_OK, esp_xiaozhi_chat_stop(s.chat));
    /* A later deinit must not try to stop an already destroyed WebSocket. */
    s.stop_result = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_OK, esp_xiaozhi_chat_deinit(s.chat));
    s.chat = 0;
    TEST_ASSERT_EQUAL(2, s.stops);
    TEST_ASSERT_EQUAL(0, s.engines_destroyed);
    TEST_ASSERT_EQUAL(ESP_OK, esp_mcp_set_server_info(config.mcp_engine, "caller-owned", NULL, NULL, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, esp_mcp_destroy(config.mcp_engine));
    TEST_ASSERT_EQUAL(1, s.engines_destroyed);
    s.active = false;
}

TEST_CASE("WS deinit after successful stop consumes chat even on owned cleanup error", "[esp_xiaozhi][backpressure]")
{
    start_chat();
    TEST_ASSERT_EQUAL(ESP_OK, esp_xiaozhi_chat_stop(s.chat));
    TEST_ASSERT_EQUAL(1, s.stops);
    TEST_ASSERT_EQUAL(1, s.destroys);
    s.engine_destroy_result = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, esp_xiaozhi_chat_deinit(s.chat));
    s.chat = 0; /* Consumed despite the cleanup status; never retry this handle. */
    TEST_ASSERT_EQUAL(1, s.engines_destroyed);
    TEST_ASSERT_EQUAL(1, s.stops);

    /* Successful registration of another instance proves old chat was unregistered.
     * No fake engine or second teardown of the old handle is used. */
    s.engine_destroy_result = ESP_OK;
    esp_xiaozhi_chat_config_t config = ESP_XIAOZHI_CHAT_DEFAULT_CONFIG();
    TEST_ASSERT_EQUAL(ESP_OK, esp_xiaozhi_chat_init(&config, &s.chat));
    TEST_ASSERT_EQUAL(ESP_OK, esp_xiaozhi_chat_deinit(s.chat));
    s.chat = 0;
    TEST_ASSERT_EQUAL(1, s.engines_destroyed);
    s.active = false;
}

TEST_CASE("WebSocket binary send and failed stop retain safe cleanup", "[esp_xiaozhi][backpressure]")
{
    begin_stub();
    esp_xiaozhi_websocket_handle_t ws;
    esp_xiaozhi_transport_callbacks_t callbacks = {0};
    TEST_ASSERT_EQUAL(ESP_OK, esp_xiaozhi_websocket_init(&ws));
    TEST_ASSERT_EQUAL(ESP_OK, esp_xiaozhi_websocket_set_callbacks(ws, 0, &callbacks));
    TEST_ASSERT_EQUAL(ESP_OK, esp_xiaozhi_websocket_start(ws));
    emit(WEBSOCKET_EVENT_CONNECTED, NULL);
    uint8_t audio[160] = {0};
    TEST_ASSERT_EQUAL(ESP_OK, esp_xiaozhi_websocket_send_binary(ws, audio, sizeof(audio)));
    s.send_result = 0;
    TEST_ASSERT_EQUAL(ESP_FAIL, esp_xiaozhi_websocket_send_binary(ws, audio, sizeof(audio)));
    s.stop_result = ESP_ERR_TIMEOUT;
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, esp_xiaozhi_websocket_deinit(ws));
    TEST_ASSERT_EQUAL(0, s.destroys);
    s.stop_result = ESP_OK;
    TEST_ASSERT_EQUAL(ESP_OK, esp_xiaozhi_websocket_stop(ws));
    TEST_ASSERT_EQUAL(ESP_OK, esp_xiaozhi_websocket_stop(ws));
    TEST_ASSERT_EQUAL(ESP_OK, esp_xiaozhi_websocket_deinit(ws));
    TEST_ASSERT_EQUAL(2, s.stops); /* failed join, then successful join; no double stop */
    TEST_ASSERT_EQUAL(1, s.destroys);
    s.active = false;
}
