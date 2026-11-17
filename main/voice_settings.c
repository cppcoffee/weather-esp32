#include "voice_settings.h"

#include <string.h>
#include <strings.h>

#include "http_parser.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static bool blocked_ipv4(uint32_t address)
{
    address = ntohl(address);
    return address == 0 || (address >> 24) == 127;
}

static bool numeric_ipv4_text(const char *host)
{
    for (const char *p = host; *p; ++p) {
        if ((*p < '0' || *p > '9') && *p != '.') {
            return false;
        }
    }
    return host[0] != '\0';
}

static bool canonical_ipv4(const char *host)
{
    unsigned int parts = 0;
    unsigned int value = 0;
    unsigned int digits = 0;
    for (const char *p = host;; ++p) {
        if (*p >= '0' && *p <= '9') {
            if (digits > 0 && value == 0) {
                return false;
            }
            if (++digits > 3) {
                return false;
            }
            value = value * 10 + (unsigned int)(*p - '0');
            if (value > 255) {
                return false;
            }
            continue;
        }
        if (*p != '.' && *p != '\0') {
            return false;
        }
        if (digits == 0) {
            return false;
        }
        ++parts;
        if (*p == '\0') {
            return parts == 4;
        }
        value = digits = 0;
    }
}

static bool legacy_numeric_host(const char *host)
{
    if (host[0] != '0' || (host[1] != 'x' && host[1] != 'X')) {
        for (const char *p = host; *p; ++p) {
            if (*p < '0' || *p > '9') {
                return false;
            }
        }
        return host[0] != '\0';
    }
    if (!host[2]) {
        return false;
    }
    for (const char *p = host + 2; *p; ++p) {
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'))) {
            return false;
        }
    }
    return true;
}

static bool allowed_host(const char *url, const struct http_parser_url *parsed)
{
    size_t offset = parsed->field_data[UF_HOST].off;
    size_t length = parsed->field_data[UF_HOST].len;
    bool ipv6_literal = offset > 0 && url[offset - 1] == '[';
    char host[VOICE_URL_CAP];
    memcpy(host, url + parsed->field_data[UF_HOST].off, length);
    host[length] = '\0';

    // A final DNS root dot must not bypass the localhost check.
    if (!ipv6_literal && length > 0 && host[length - 1] == '.') {
        host[--length] = '\0';
    }
    const size_t suffix_length = sizeof(".localhost") - 1;
    if (length == 0 || strcasecmp(host, "localhost") == 0
        || (length > suffix_length && strcasecmp(host + length - suffix_length, ".localhost") == 0))
    {
        return false;
    }

    struct in_addr ipv4;
    if (!ipv6_literal && numeric_ipv4_text(host)) {
        if (!canonical_ipv4(host) || inet_pton(AF_INET, host, &ipv4) != 1) {
            return false;
        }
        return !blocked_ipv4(ipv4.s_addr);
    }
    // Reject legacy integer and hexadecimal spellings that could resolve as an IP literal.
    if (!ipv6_literal && legacy_numeric_host(host)) {
        return false;
    }
    if (ipv6_literal) {
        struct in6_addr ipv6;
        if (inet_pton(AF_INET6, host, &ipv6) != 1) {
            return false;
        }
        const uint8_t *bytes = (const uint8_t *)&ipv6;
        bool all_zero = true;
        for (size_t i = 0; i < 16; ++i) {
            if (bytes[i] != 0) {
                all_zero = false;
                break;
            }
        }
        bool loopback = true;
        for (size_t i = 0; i < 15; ++i) {
            if (bytes[i] != 0) {
                loopback = false;
                break;
            }
        }
        if (all_zero || (loopback && bytes[15] == 1)) {
            return false;
        }
        bool v4_mapped = true;
        for (size_t i = 0; i < 10; ++i) {
            if (bytes[i] != 0) {
                v4_mapped = false;
                break;
            }
        }
        v4_mapped = v4_mapped && bytes[10] == 0xff && bytes[11] == 0xff;
        if (v4_mapped) {
            struct in_addr mapped;
            memcpy(&mapped.s_addr, bytes + 12, sizeof(mapped.s_addr));
            return !blocked_ipv4(mapped.s_addr);
        }
    }
    return true;
}

static bool field_equals(const char *url, const struct http_parser_url *parsed, enum http_parser_url_fields field,
                         const char *expected)
{
    size_t length = strlen(expected);
    return parsed->field_data[field].len == length
           && memcmp(url + parsed->field_data[field].off, expected, length) == 0;
}

static bool valid_url(const char *url, bool websocket)
{
    size_t length = strlen(url);
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)url[i];
        // The parser does not set UF_FRAGMENT for a trailing empty '#'.
        if (c <= 0x20 || c >= 0x7f || c == '#') {
            return false;
        }
    }

    struct http_parser_url parsed = {0};
    if (http_parser_parse_url(url, length, 0, &parsed) != 0 || !(parsed.field_set & (1 << UF_SCHEMA))
        || !(parsed.field_set & (1 << UF_HOST)) || parsed.field_data[UF_HOST].len == 0
        || (parsed.field_set & ((1 << UF_USERINFO) | (1 << UF_FRAGMENT)))
        || ((parsed.field_set & (1 << UF_PORT)) && parsed.port == 0))
    {
        return false;
    }
    // Empty userinfo ('@host') likewise has no UF_USERINFO in this parser.
    if (memchr(url, '@', parsed.field_data[UF_HOST].off)) {
        return false;
    }
    if (!field_equals(url, &parsed, UF_SCHEMA, websocket ? "ws" : "http")
        && !field_equals(url, &parsed, UF_SCHEMA, websocket ? "wss" : "https"))
    {
        return false;
    }
    return allowed_host(url, &parsed);
}

esp_err_t voice_settings_validate(const voice_settings_t *settings)
{
    if (!settings || !memchr(settings->info_url, '\0', VOICE_URL_CAP)
        || !memchr(settings->websocket_url, '\0', VOICE_URL_CAP))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!valid_url(settings->info_url, false)
        || (settings->websocket_url[0] != '\0' && !valid_url(settings->websocket_url, true)))
    {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t voice_settings_load(voice_settings_t *settings)
{
    if (!settings) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open(VOICE_SETTINGS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        memset(settings, 0, sizeof(*settings));
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    voice_settings_t loaded;
    size_t length = sizeof(loaded);
    err = nvs_get_blob(handle, VOICE_SETTINGS_KEY, &loaded, &length);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        memset(settings, 0, sizeof(*settings));
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (length != sizeof(loaded)) {
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    err = voice_settings_validate(&loaded);
    if (err == ESP_OK) {
        *settings = loaded;
    }
    return err;
}

esp_err_t voice_settings_store(nvs_handle_t handle, const voice_settings_t *settings)
{
    esp_err_t err = voice_settings_validate(settings);
    if (err != ESP_OK) {
        return err;
    }
    return nvs_set_blob(handle, VOICE_SETTINGS_KEY, settings, sizeof(*settings));
}
