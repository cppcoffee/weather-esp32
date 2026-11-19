#!/usr/bin/env python3
"""Host regression for voice_settings.c with ESP-IDF's real http_parser.c.

Requires only Python's standard library, cc/clang/gcc and an ESP-IDF checkout.
IDF_PATH overrides the local v6.0.2 fallback. NVS and lwIP's socket interface are
stubbed; IP literals use the host's POSIX address converters. This checks storage
and URL business rules, not real flash, DNS resolution, TLS or network access.
All generated files and the executable live in TemporaryDirectory.

    python3 tools/test_voice_settings.py
"""

import os
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

HEADERS = {
    "esp_err.h": r'''
#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
''',
    "nvs.h": r'''
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
#define ESP_ERR_NVS_NOT_INITIALIZED 0x1101
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_TYPE_MISMATCH 0x1103
#define ESP_ERR_NVS_NOT_ENOUGH_SPACE 0x1105
#define ESP_ERR_NVS_INVALID_HANDLE 0x1107
#define ESP_ERR_NVS_INVALID_LENGTH 0x110c
esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *length);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length);
esp_err_t nvs_commit(nvs_handle_t handle);
void nvs_close(nvs_handle_t handle);
''',
    "lwip/inet.h": "#pragma once\n#include <arpa/inet.h>\n",
    "lwip/sockets.h": "#pragma once\n#include <sys/socket.h>\n",
}

HARNESS = r'''
#include "voice_settings.h"
#include "voice_settings.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

_Static_assert(VOICE_URL_CAP == 256, "URL capacity is part of the storage contract");
_Static_assert(sizeof(voice_settings_t) == 257, "Unexpected persisted layout");
_Static_assert(offsetof(voice_settings_t, info_url) == 0, "Unexpected info_url layout");

#define READ_HANDLE 19
#define WRITE_HANDLE 23

static bool namespace_present, blob_present, pending_present, read_open, write_open;
static unsigned char blob[sizeof(voice_settings_t) + 16], pending[sizeof(voice_settings_t) + 16];
static size_t blob_length, pending_length, reported_length;
static esp_err_t open_error, get_error, set_error;
static int opens, reads, sets, commits, closes;
static int checks;

static void reset_nvs(void)
{
    namespace_present = true;
    blob_present = pending_present = read_open = write_open = false;
    blob_length = pending_length = 0;
    reported_length = SIZE_MAX;
    open_error = get_error = set_error = ESP_OK;
    opens = reads = sets = commits = closes = 0;
    memset(blob, 0, sizeof(blob));
    memset(pending, 0, sizeof(pending));
}

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    ++opens;
    assert(strcmp(name, "wifi") == 0);
    assert(handle);
    if (open_error != ESP_OK) {
        return open_error;
    }
    if (mode == NVS_READONLY) {
        if (!namespace_present) {
            return ESP_ERR_NVS_NOT_FOUND;
        }
        assert(!read_open);
        read_open = true;
        *handle = READ_HANDLE;
    } else {
        assert(mode == NVS_READWRITE && !write_open);
        namespace_present = write_open = true;
        *handle = WRITE_HANDLE;
    }
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *length)
{
    ++reads;
    assert(handle == READ_HANDLE && read_open);
    assert(strcmp(key, "voice_v3") == 0 && length);
    if (get_error != ESP_OK) {
        // Even a failed read which touched its buffer must not mutate the caller's output.
        if (value && *length >= 4) {
            memcpy(value, "oops", 4);
        }
        return get_error;
    }
    if (!blob_present) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    size_t capacity = *length;
    *length = blob_length;
    if (value) {
        if (capacity < blob_length) {
            return ESP_ERR_NVS_INVALID_LENGTH;
        }
        memcpy(value, blob, blob_length);
    }
    if (reported_length != SIZE_MAX) {
        *length = reported_length;
    }
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length)
{
    ++sets;
    assert(strcmp(key, "voice_v3") == 0 && value);
    assert(length == sizeof(voice_settings_t));
    if (handle != WRITE_HANDLE || !write_open) {
        return ESP_ERR_NVS_INVALID_HANDLE;
    }
    if (set_error != ESP_OK) {
        return set_error;
    }
    memcpy(pending, value, length);
    pending_length = length;
    pending_present = true;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    ++commits;
    assert(handle == WRITE_HANDLE && write_open);
    if (pending_present) {
        memcpy(blob, pending, pending_length);
        blob_length = pending_length;
        blob_present = true;
        pending_present = false;
    }
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    ++closes;
    if (handle == READ_HANDLE) {
        assert(read_open);
        read_open = false;
    } else {
        assert(handle == WRITE_HANDLE && write_open);
        write_open = false;
    }
}

static voice_settings_t make_settings(const char *info)
{
    voice_settings_t settings = {0};
    assert(strlen(info) < VOICE_URL_CAP);
    strcpy(settings.info_url, info);
    return settings;
}

static void expect_validation(const voice_settings_t *settings, esp_err_t expected)
{
    voice_settings_t before = *settings;
    esp_err_t actual = voice_settings_validate(settings);
    if (actual != expected) {
        fprintf(stderr, "validate info='%s': got %x, expected %x\n",
                settings->info_url, actual, expected);
    }
    assert(actual == expected);
    assert(memcmp(&before, settings, sizeof(before)) == 0);
    ++checks;
}

static void expect_url(const char *url, bool valid)
{
    voice_settings_t settings = make_settings(url);
    expect_validation(&settings, valid ? ESP_OK : ESP_ERR_INVALID_ARG);
}

static void test_urls(void)
{
    const char *valid_info[] = {
        "http://voice.example", "https://voice.example/info", "http://192.168.1.2:8000/xiaozhi/ota/",
        "http://10.0.0.8", "https://172.16.0.4:65535/?token=a%20b&next=%2Ffoo%23bar",
        "https://VOICE.EXAMPLE:443/v1?email=a@b.example", "http://xiaozhi.local/ota/",
        "http://localhost.example/info", "https://example.com/localhost/127.0.0.1?q=%E4%BD%A0%E5%A5%BD",
        "https://voice.example.:1", "http://[2001:db8::1]:8080/info?token=ok",
        "http://[fd00::12]/ota/", "http://[::ffff:192.168.1.2]/ota/", "http://128.0.0.1/",
        "https://example.com/?", "https://example.com/path/@name?x=%00%0D%0A",
    };
    for (size_t i = 0; i < sizeof(valid_info) / sizeof(valid_info[0]); ++i) {
        expect_url(valid_info[i], true);
    }
    const char *invalid_info[] = {
        "", "/info", "voice.example/info", "//voice.example/info", "http:voice.example",
        "https:/voice.example", "https:///info", "https://", "https://?q=1", "http://:80/",
        "ftp://voice.example", "ws://voice.example", "wss://voice.example", "httpsx://voice.example",
        "HTTP://voice.example", "Https://voice.example", "http://host:0/", "http://host:000/",
        "http://host:/", "http://host:-1/", "http://host:65536/", "http://host:80x/",
        "http://host:99999999999999999999999999999/", "https://user:pass@host/",
        "https://user@host/", "https://@host/", "https://:pass@host/", "https://u%40x:p%23q@host/",
        "https://a@b@host/", "https://host/path#fragment", "https://host/path#",
        "https://host/?q=yes#", "https://host/#?q=yes", "https://localhost/", "http://LOCALHOST:8080/",
        "http://foo.localhost/", "http://A.B.LoCaLhOsT/", "http://localhost./", "http://x.localhost.:80/",
        "http://127.0.0.1/", "http://127.8.9.10:8000/", "http://127.255.255.255/", "http://0.0.0.0/",
        "http://127.1/", "http://2130706433/", "http://0x7f000001/", "http://0177.0.0.1/", "http://0/",
        "http://[::1]/", "http://[0:0:0:0:0:0:0:1]:80/", "http://[::]/", "http://[0:0:0:0:0:0:0:0]/",
        "http://[::ffff:127.10.0.1]/", "http://[::FFFF:0.0.0.0]/", "http://[::ffff:7f00:1]/",
        "http://[::1%25lo0]/", "http://[:::1]/", "http://[2001:db8::1", "http://2001:db8::1/",
        "http://[1234]/", "http://[127.0.0.1]/", "http://[]/", "http://[gggg::2]/",
        " https://host/", "https://host/ ", "https://host/a b", "https://host/\t", "https://host/\r\nX:y",
        "https://host/\x7f", "https://host/\xc3\xa9", "https://h\xc3\xb6st.example/",
    };
    for (size_t i = 0; i < sizeof(invalid_info) / sizeof(invalid_info[0]); ++i) {
        expect_url(invalid_info[i], false);
    }
    // ws/wss 不再作为信息地址接受
    expect_url("ws://voice.example/xiaozhi/v1/", false);
    expect_url("wss://voice.example/ws", false);
    // All literal controls, whitespace and non-ASCII bytes must be rejected.
    char url[] = "https://voice.example/aXb";
    for (unsigned int byte = 1; byte <= 255; ++byte) {
        if (byte > 32 && byte < 127) {
            continue;
        }
        url[sizeof(url) - 3] = (char)byte;
        expect_url(url, false);
    }
}

static void test_settings_and_bounds(void)
{
    // 没有内置默认地址：空 info_url 一律拒绝
    voice_settings_t bounds = {0};
    expect_validation(&bounds, ESP_ERR_INVALID_ARG);
    strcpy(bounds.info_url, "https://voice.example/info");
    expect_validation(&bounds, ESP_OK);
    memset(bounds.info_url, 0, sizeof(bounds.info_url));
    expect_validation(&bounds, ESP_ERR_INVALID_ARG);

    struct {
        unsigned char before[8];
        voice_settings_t settings;
        unsigned char after[8];
    } guarded;
    memset(&guarded, 0xa5, sizeof(guarded));
    guarded.settings.followup = 0;
    memset(guarded.settings.info_url, 'x', VOICE_URL_CAP);
    assert(voice_settings_validate(&guarded.settings) == ESP_ERR_INVALID_ARG);
    strcpy(guarded.settings.info_url, "https://voice.example/info");
    assert(voice_settings_validate(&guarded.settings) == ESP_OK);
    guarded.settings.info_url[0] = '\0';
    assert(voice_settings_validate(&guarded.settings) == ESP_ERR_INVALID_ARG);
    for (size_t i = 0; i < sizeof(guarded.before); ++i) {
        assert(guarded.before[i] == 0xa5 && guarded.after[i] == 0xa5);
    }
    checks += 3;

    {
        voice_settings_t settings = make_settings("https://voice.example/info");
        char *url = settings.info_url;
        const char *prefix = "https://voice.example/";
        memset(url, 'a', VOICE_URL_CAP);
        memcpy(url, prefix, strlen(prefix));
        url[VOICE_URL_CAP - 1] = '\0';
        assert(strlen(url) == 255);
        expect_validation(&settings, ESP_OK);
        url[VOICE_URL_CAP - 1] = 'a';
        assert(voice_settings_validate(&settings) == ESP_ERR_INVALID_ARG);
        ++checks;
    }
    reset_nvs();
    assert(voice_settings_validate(NULL) == ESP_ERR_INVALID_ARG);
    assert(voice_settings_load(NULL) == ESP_ERR_INVALID_ARG);
    assert(voice_settings_store(WRITE_HANDLE, NULL) == ESP_ERR_INVALID_ARG);
    assert(opens == 0 && reads == 0 && sets == 0 && commits == 0 && closes == 0);
    checks += 3;
}

static void seed_blob(const voice_settings_t *settings)
{
    memcpy(blob, settings, sizeof(*settings));
    blob_length = sizeof(*settings);
    blob_present = true;
}

static void expect_failed_load(esp_err_t expected)
{
    voice_settings_t output = make_settings("http://192.168.1.2:8003/xiaozhi/ota/");
    voice_settings_t before = output;
    assert(voice_settings_load(&output) == expected);
    assert(memcmp(&output, &before, sizeof(output)) == 0);
    assert(!read_open && !write_open && sets == 0 && commits == 0);
    ++checks;
}

static void test_load(void)
{
    // Nothing saved: follow-up listening is on by default.
    const voice_settings_t zeroed = {.followup = 1};
    voice_settings_t output;
    reset_nvs();
    namespace_present = false;
    memset(&output, 0xa5, sizeof(output));
    assert(voice_settings_load(&output) == ESP_OK);
    assert(memcmp(&output, &zeroed, sizeof(output)) == 0);
    assert(opens == 1 && reads == 0 && closes == 0 && sets == 0 && commits == 0);
    ++checks;

    reset_nvs();
    memset(&output, 0xa5, sizeof(output));
    assert(voice_settings_load(&output) == ESP_OK);
    assert(memcmp(&output, &zeroed, sizeof(output)) == 0);
    assert(opens == 1 && reads == 1 && closes == 1 && sets == 0 && commits == 0);
    ++checks;

    const esp_err_t open_errors[] = {ESP_FAIL, ESP_ERR_NO_MEM, ESP_ERR_NVS_NOT_INITIALIZED, ESP_ERR_INVALID_STATE};
    for (size_t i = 0; i < sizeof(open_errors) / sizeof(open_errors[0]); ++i) {
        reset_nvs();
        open_error = open_errors[i];
        expect_failed_load(open_error);
        assert(opens == 1 && reads == 0 && closes == 0);
    }
    const esp_err_t read_errors[] = {ESP_FAIL, ESP_ERR_NVS_TYPE_MISMATCH, ESP_ERR_NVS_INVALID_LENGTH,
                                   ESP_ERR_NVS_INVALID_HANDLE};
    for (size_t i = 0; i < sizeof(read_errors) / sizeof(read_errors[0]); ++i) {
        reset_nvs();
        get_error = read_errors[i];
        expect_failed_load(get_error);
        assert(opens == 1 && reads >= 1 && closes == 1);
    }
    reset_nvs();
    voice_settings_t legacy = make_settings("https://voice.example/info");
    seed_blob(&legacy);
    blob_length = VOICE_URL_CAP;
    memset(&output, 0xa5, sizeof(output));
    assert(voice_settings_load(&output) == ESP_OK);
    voice_settings_t upgraded = legacy;
    upgraded.followup = 1;
    assert(memcmp(&output, &upgraded, sizeof(output)) == 0 && output.followup);
    ++checks;

    // Any size other than the current and legacy layouts must fail.
    for (size_t length = 0; length < sizeof(blob); ++length) {
        if (length == sizeof(voice_settings_t) || length == VOICE_URL_CAP) {
            continue;
        }
        reset_nvs();
        blob_present = true;
        blob_length = length;
        expect_failed_load(ESP_ERR_NVS_INVALID_LENGTH);
        assert(opens == 1 && closes == 1);
    }
    for (int delta = -2; delta <= 1; delta += 3) {
        reset_nvs();
        seed_blob(&zeroed);
        reported_length = (size_t)((int)sizeof(zeroed) + delta);
        expect_failed_load(ESP_ERR_NVS_INVALID_LENGTH);
    }
    for (int corruption = 0; corruption < 7; ++corruption) {
        reset_nvs();
        voice_settings_t invalid = make_settings("https://voice.example/info");
        switch (corruption) {
        case 0:
            invalid.info_url[0] = '\0';
            break;
        case 1:
            strcpy(invalid.info_url, "ws://voice.example/info");
            break;
        case 2:
            memset(invalid.info_url, 'a', VOICE_URL_CAP);
            break;
        case 3:
            strcpy(invalid.info_url, "http://127.0.0.1:8003/xiaozhi/ota/");
            break;
        case 4:
            strcpy(invalid.info_url, "http://127.0.0.1/ota/");
            break;
        case 5:
            strcpy(invalid.info_url, "https://user:pass@voice.example/ota/");
            break;
        case 6:
            invalid.followup = 2;
            break;
        }
        seed_blob(&invalid);
        expect_failed_load(ESP_ERR_INVALID_ARG);
        assert(opens == 1 && closes == 1);
    }
}

static void test_store(void)
{
    const voice_settings_t cases[] = {
        {.info_url = "https://voice.example/info"},
        {.info_url = "http://192.168.1.8:8000/xiaozhi/ota/?x=a%20b&token=a%23b", .followup = 1},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        reset_nvs();
        nvs_handle_t handle;
        assert(nvs_open("wifi", NVS_READWRITE, &handle) == ESP_OK);
        voice_settings_t before = cases[i];
        assert(voice_settings_store(handle, &cases[i]) == ESP_OK);
        assert(memcmp(&before, &cases[i], sizeof(before)) == 0);
        assert(opens == 1 && reads == 0 && sets == 1 && commits == 0 && closes == 0 && write_open);
        assert(!blob_present && pending_present && pending_length == sizeof(cases[i]));
        assert(memcmp(pending, &cases[i], sizeof(cases[i])) == 0);
        // Caller can stage WiFi credentials in the same handle before this commit.
        assert(nvs_commit(handle) == ESP_OK);
        nvs_close(handle);
        voice_settings_t output;
        memset(&output, 0xa5, sizeof(output));
        assert(voice_settings_load(&output) == ESP_OK);
        assert(memcmp(&output, &cases[i], sizeof(output)) == 0);
        assert(opens == 2 && sets == 1 && commits == 1 && closes == 2);
        ++checks;
    }

    reset_nvs();
    nvs_handle_t handle;
    assert(nvs_open("wifi", NVS_READWRITE, &handle) == ESP_OK);
    voice_settings_t saved = make_settings("https://voice.example/info");
    assert(voice_settings_store(handle, &saved) == ESP_OK);
    assert(nvs_commit(handle) == ESP_OK);
    voice_settings_t invalid = saved;
    strcpy(invalid.info_url, "http://localhost/ota/");
    assert(voice_settings_store(handle, &invalid) == ESP_ERR_INVALID_ARG);
    assert(sets == 1 && commits == 1 && closes == 0 && write_open);
    const esp_err_t write_errors[] = {ESP_FAIL, ESP_ERR_NVS_NOT_ENOUGH_SPACE, ESP_ERR_NVS_INVALID_HANDLE};
    for (size_t i = 0; i < sizeof(write_errors) / sizeof(write_errors[0]); ++i) {
        set_error = write_errors[i];
        voice_settings_t replacement = make_settings("https://voice.example/other");
        assert(voice_settings_store(handle, &replacement) == set_error);
        assert(commits == 1 && closes == 0 && write_open);
        assert(memcmp(blob, &saved, sizeof(saved)) == 0 && !pending_present);
        ++checks;
    }
    nvs_close(handle);
    voice_settings_t output = {0};
    assert(voice_settings_load(&output) == ESP_OK);
    assert(memcmp(&output, &saved, sizeof(output)) == 0);
    reset_nvs();
    assert(voice_settings_store(0, &saved) == ESP_ERR_NVS_INVALID_HANDLE);
    assert(sets == 1 && opens == 0 && commits == 0 && closes == 0);
    checks += 2;
}

int main(void)
{
    assert(strcmp(VOICE_SETTINGS_NAMESPACE, "wifi") == 0);
    assert(strcmp(VOICE_SETTINGS_KEY, "voice_v3") == 0);
    test_urls();
    test_settings_and_bounds();
    test_load();
    test_store();
    printf("voice_settings: %d checks passed (real ESP-IDF http_parser, stub NVS)\n", checks);
    return 0;
}
'''


def main():
    idf = Path(os.environ.get("IDF_PATH") or "/Users/sharp/.espressif/v6.0.2/esp-idf").expanduser()
    parser = idf / "components/http_parser"
    if not all((parser / name).is_file() for name in ("http_parser.c", "http_parser.h")):
        raise SystemExit(f"ESP-IDF http_parser source not found in {parser}; set IDF_PATH")
    compiler = next((shutil.which(c) for c in ("cc", "clang", "gcc") if shutil.which(c)), None)
    if not compiler:
        raise SystemExit("A host C compiler (cc, clang or gcc) is required")
    with tempfile.TemporaryDirectory(prefix="voice-settings-") as temporary:
        directory = Path(temporary)
        for name, content in HEADERS.items():
            path = directory / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
        harness = directory / "test_voice_settings.c"
        harness.write_text(HARNESS, encoding="utf-8")
        executable = directory / "test_voice_settings"
        subprocess.run(
            [compiler, "-std=c11", "-D_DEFAULT_SOURCE", "-Wall", "-Wextra", "-Werror", "-O1",
             "-I", str(directory), "-I", str(ROOT / "main"), "-I", str(parser),
             str(ROOT / "main/voice_settings.c"), str(parser / "http_parser.c"),
             str(harness), "-o", str(executable)],
            check=True, timeout=60,
        )
        subprocess.run([str(executable)], check=True, timeout=30)


if __name__ == "__main__":
    main()
