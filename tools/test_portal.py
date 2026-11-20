#!/usr/bin/env python3
"""Host regression for portal rendering and credential submission.

Uses a host C compiler and the query parser from the ESP-IDF checkout (IDF_PATH).
HTTP, NVS and WiFi operations are stubbed. Browser and hardware behavior are
outside the scope of this test.

    python3 tools/test_portal.py
"""

import html
import os
import re
import shutil
import subprocess
import tempfile
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import urlencode

ROOT = Path(__file__).resolve().parents[1]
OWN = "weather-A1B2C3"
ASSETS = {
    "portal_index": "index.html",
    "portal_result": "result.html",
    "portal_style": "style.css",
    "portal_script": "app.js",
}


def extract(source, pattern):
    matches = re.findall(pattern, source, re.MULTILINE | re.DOTALL)
    assert len(matches) == 1, f"Expected one source fragment: {pattern}"
    return matches[0]


def function(source, name):
    # portal_web.c uses column-zero closing braces only for the function itself.
    return extract(source, rf"(^static [^\n]+\b{name}\([^\n]*\)\n\{{\n.*?^\}})")


STUBS = r'''
#include <assert.h>
#include <ctype.h>
#include <strings.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/types.h>
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG -2
#define ESP_ERR_NOT_FOUND -3
#define ESP_ERR_HTTPD_RESULT_TRUNC -4
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define SEND_ERROR -73
#define HTTPD_RESP_USE_STRLEN -1
#define HTTPD_400_BAD_REQUEST 400
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
typedef int esp_err_t;
typedef struct {
    char body[131072];
    size_t used;
    size_t content_len, read_offset;
    const char *input;
    const char *status, *type;
    int calls, fail_at, finished;
} httpd_req_t;
static esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type)
{
    r->type = type;
    return ESP_OK;
}
static esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status)
{
    r->status = status;
    return ESP_OK;
}
static esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *data, ssize_t len)
{
    assert(!r->finished);
    r->calls++;
    if (r->calls == r->fail_at) return SEND_ERROR;
    if (len == HTTPD_RESP_USE_STRLEN) {
        assert(data);
        len = (ssize_t)strlen(data);
    }
    assert(len >= 0);
    if (!len) {
        r->finished++;
        return ESP_OK;
    }
    assert(data && r->used + (size_t)len < sizeof(r->body));
    memcpy(r->body + r->used, data, (size_t)len);
    r->used += (size_t)len;
    r->body[r->used] = 0;
    return ESP_OK;
}
static esp_err_t httpd_resp_send_500(httpd_req_t *r)
{
    (void)r;
    assert(!"Unexpected HTTP 500");
    return ESP_FAIL;
}
static __attribute__((unused)) esp_err_t httpd_resp_send_err(httpd_req_t *r, int error, const char *message)
{
    (void)r;
    (void)error;
    (void)message;
    assert(!"Unexpected HTTP error");
    return ESP_FAIL;
}
#define VOICE_URL_CAP 256
'''

SCAN_STUB = r'''
typedef struct {
    char info_url[256];
    unsigned char followup;
} voice_settings_t;
static int voice_settings_load(voice_settings_t *settings)
{
    memset(settings, 0, sizeof(*settings));
    return 0;
}
static __attribute__((unused)) int voice_settings_validate(const voice_settings_t *settings)
{
    return settings && settings->info_url[0] ? 0 : -1;
}
static int input_count;
static char **input_ssids;
static int scan_aps(scan_ap_t *out, int max)
{
    assert(max == 16 && input_count <= max);
    for (int i = 0; i < input_count; i++) {
        assert(strlen(input_ssids[i]) <= 32);
        strcpy(out[i].ssid, input_ssids[i]);
    }
    return input_count;
}
static const char *net_ap_ssid(void) { return "weather-A1B2C3"; }
'''

SAVE_STUBS = r'''
#define NVS_READWRITE 1
#define CREDS_SAVED 1
typedef int nvs_handle_t;
static int opened, committed, signaled, closed;
static char saved_ssid[33], saved_pass[65];
static voice_settings_t saved_settings;

static int httpd_req_recv(httpd_req_t *r, char *dst, size_t len)
{
    assert(r->read_offset + len == r->content_len);
    len = MIN(len, 7);
    memcpy(dst, r->input + r->read_offset, len);
    r->read_offset += len;
    return (int)len;
}
static int nvs_open(const char *name, int mode, nvs_handle_t *h)
{
    assert(!strcmp(name, "wifi") && mode == NVS_READWRITE);
    *h = ++opened;
    return ESP_OK;
}
static int nvs_set_str(nvs_handle_t h, const char *key, const char *value)
{
    assert(h == 1);
    if (!strcmp(key, "ssid")) {
        assert(strlen(value) < sizeof(saved_ssid));
        strcpy(saved_ssid, value);
    } else {
        assert(!strcmp(key, "pass") && strlen(value) < sizeof(saved_pass));
        strcpy(saved_pass, value);
    }
    return ESP_OK;
}
static int voice_settings_store(nvs_handle_t h, const voice_settings_t *settings)
{
    assert(h == 1);
    saved_settings = *settings;
    return ESP_OK;
}
static int nvs_commit(nvs_handle_t h) { assert(h == 1); committed++; return ESP_OK; }
static void nvs_close(nvs_handle_t h) { assert(h == 1); closed++; }
static int portal_events(void) { return 1; }
static void xEventGroupSetBits(int event, int bits)
{
    assert(event == 1 && bits == CREDS_SAVED && committed == 1);
    signaled++;
}
'''


RUNNER = r'''
static esp_err_t invoke(httpd_req_t *r, int argc, char **argv)
{
    if (!strcmp(argv[1], "root")) return root_get(r);
    if (!strcmp(argv[1], "reply")) {
        assert(argc == 5);
        return reply(r, argv[3], argv[4], !strcmp(argv[2], "ok"));
    }
    assert(!strcmp(argv[1], "send") && argc == 3);
    const page_value_t values[] = {
        {"{{EMPTY}}", ""}, {"{{VALUE}}", "replacement{{EMPTY}}"},
    };
    return send_page(r, argv[2], values, sizeof(values) / sizeof(values[0]));
}
int main(int argc, char **argv)
{
    assert(argc >= 2);
    if (!strcmp(argv[1], "root")) {
        input_count = argc - 2;
        input_ssids = argv + 2;
    }
    httpd_req_t good = {.status = "200 OK"};
    if (!strcmp(argv[1], "save")) {
        assert(argc == 3 || argc == 6);
        good.input = argv[2];
        good.content_len = strlen(good.input);
        assert(save_post(&good) == ESP_OK && good.finished == 1);
        if (argc == 3) {
            assert(!strcmp(good.status, "400 Bad Request"));
            assert(!opened && !committed && !closed && !signaled);
        } else {
            assert(!strcmp(good.status, "200 OK"));
            assert(opened == 1 && committed == 1 && closed == 1 && signaled == 1);
            assert(!strcmp(saved_ssid, argv[3]) && !strcmp(saved_pass, argv[4]));
            assert(!strcmp(saved_settings.info_url, argv[5]));
            assert(saved_settings.followup == 1);
        }
        return 0;
    }
    assert(invoke(&good, argc, argv) == ESP_OK);
    assert(good.finished == 1);
    assert(good.type && !strcmp(good.type, "text/html; charset=utf-8"));
    // Inject an error at EVERY send, including the terminating chunk. No later
    // chunk may be attempted and the original error must reach the caller.
    for (int i = 1; i <= good.calls; i++) {
        httpd_req_t failed = {.status = "200 OK", .fail_at = i};
        assert(invoke(&failed, argc, argv) == SEND_ERROR);
        assert(failed.calls == i && !failed.finished);
    }
    httpd_req_t unknown = {0};
    assert(send_page(&unknown, "{{UNKNOWN}}", NULL, 0) == ESP_FAIL);
    assert(!unknown.finished);
    printf("%s\n%s\n", good.status, good.type);
    assert(fwrite(good.body, 1, good.used, stdout) == good.used);
    return 0;
}
'''


def build_harness(directory, assets):
    source = (ROOT / "main/portal_web.c").read_text()
    cmake = (ROOT / "main/CMakeLists.txt").read_text()
    embedded = extract(cmake, r"\bEMBED_TXTFILES\s+(.*?)(?=\b[A-Z_]{3,}\b|\))")
    assert set(re.findall(r'"([^"]+)"', embedded)) == {
        f"web/{name}" for name in ASSETS.values()
    }, "EMBED_TXTFILES must contain the four portal assets"
    definitions = []
    for symbol, filename in ASSETS.items():
        linker_name = "_binary_" + filename.replace(".", "_") + "_start"
        assert re.search(
            rf'extern const char {symbol}\[\]\s+asm\("{linker_name}"\);', source
        ), f"Missing embedded symbol: {symbol}"
        data = assets[filename].encode("utf-8")
        assert b"\0" not in data
        # Fixed-width octal escapes preserve UTF-8 bytes without C escape ambiguity.
        literal = "".join(f"\\{byte:03o}" for byte in data)
        definitions.append(f'static const char {symbol}[] = "{literal}";\n')
    ap_url = extract((ROOT / "main/net.h").read_text(), r'(^#define NET_AP_URL[ \t]+"[^"\n]+")')
    declarations = [
        extract(source, r"(^#define MAX_APS\s+\d+)"),
        extract(source, r"(^typedef struct \{[^{}]*\} scan_ap_t;)"),
        extract(source, r"(^typedef struct \{[^{}]*\} page_value_t;)"),
    ]
    idf = Path(os.environ.get("IDF_PATH", Path.home() / ".espressif/v6.0.2/esp-idf"))
    parser = idf / "components/esp_http_server/src/httpd_parse.c"
    assert parser.is_file(), "Set IDF_PATH to the ESP-IDF checkout used to build the firmware"
    query_parser = extract(
        parser.read_text(), r"(^esp_err_t httpd_query_key_value\([^\n]*\)\n\{\n.*?^\})"
    )
    code = "\n".join([
        STUBS, ap_url, *declarations, SCAN_STUB, SAVE_STUBS, query_parser, *definitions,
        *(function(source, name) for name in
          ("html_escape", "addf", "form_field", "send_page", "root_get", "reply", "save_post")), RUNNER,
    ])
    path = directory / "portal.c"
    path.write_text(code, encoding="utf-8")
    compiler = next((shutil.which(c) for c in ("cc", "clang", "gcc") if shutil.which(c)), None)
    assert compiler, "A host C compiler (cc, clang or gcc) is required"
    executable = directory / "portal"
    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
                    str(path), "-o", str(executable)], check=True, timeout=60)
    return executable, ap_url.split('"')[1]


class Page(HTMLParser):
    def __init__(self, text):
        super().__init__(convert_charrefs=True)
        self.elements = []
        self.options = []
        self.sections = {}
        self.active = None
        self.feed(text)
        self.close()

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        self.elements.append((tag, attrs))
        # Navigation and form submission are allowed, automatic resource loads aren't.
        assert not (set(attrs) & {"src", "srcset", "poster", "background"}), attrs
        assert tag not in {"link", "iframe", "object", "embed", "base"}, tag
        assert not (tag == "meta" and attrs.get("http-equiv", "").lower() == "refresh")
        if tag in {"style", "script", "title", "h1", "footer", "option", "p"}:
            self.active = tag
            self.sections.setdefault(tag, []).append("")
        if tag == "option":
            self.options.append((attrs, ""))

    def handle_data(self, data):
        if self.active:
            self.sections[self.active][-1] += data
            if self.active == "option":
                attrs, text = self.options[-1]
                self.options[-1] = (attrs, text + data)

    def handle_endtag(self, tag):
        if self.active == tag:
            self.active = None


def check_assets(assets):
    expected = {
        "index.html": {
                "STYLE", "SCRIPT", "OPTIONS", "AP_URL", "AP_SSID",
                "VOICE_URL",
            },
        "result.html": {"STYLE", "TITLE", "MESSAGE"},
        "style.css": set(), "app.js": set(),
    }
    for name, tokens in expected.items():
        assert set(re.findall(r"\{\{(.*?)\}\}", assets[name])) == tokens, name
    # Static guards, not a claim that arbitrary JavaScript/CSS has been executed.
    assert not re.search(r"url\s*\(|@import", assets["style.css"], re.I)
    assert not re.search(
        r"\b(fetch|XMLHttpRequest|WebSocket|EventSource|sendBeacon|importScripts)\b"
        r"|\bimport\s*[(']|\.src\s*=", assets["app.js"]
    ), "Portal script must not request resources"


def run(executable, *args):
    result = subprocess.run([str(executable), *args], check=True, capture_output=True, timeout=10)
    status, content_type, body = result.stdout.decode("utf-8").split("\n", 2)
    assert content_type == "text/html; charset=utf-8"
    return status, body


def check_root(executable, assets, ap_url, ssids):
    status, body = run(executable, "root", *ssids)
    assert status == "200 OK"
    page = Page(body)
    assert page.sections["style"] == [assets["style.css"]]
    assert page.sections["script"] == [assets["app.js"]]
    assert page.sections["footer"] == [f"{OWN} · {ap_url}"]
    forms = [a for tag, a in page.elements if tag == "form"]
    assert len(forms) == 1 and forms[0].get("method", "").lower() == "post"
    assert forms[0].get("action") == "/save"
    fields = {a.get("name"): (tag, a) for tag, a in page.elements if tag in {"input", "select"}}
    assert set(fields) == {"ssid", "ssid2", "pass", "voice_url"}
    assert fields["ssid"][0] == "select"
    assert(fields["ssid2"][1].get("type") == "text")
    assert(fields["pass"][1].get("type") == "password")
    assert(fields["voice_url"][1].get("type") == "url")
    assert "required" in fields["voice_url"][1]
    assert any(tag == "button" and a.get("type") == "submit" for tag, a in page.elements)
    wifi_options = [(attrs, text) for attrs, text in page.options]
    shown = [ssid for ssid in ssids if ssid != OWN]
    if shown:
        assert [a.get("value") for a, _ in wifi_options] == shown
        assert [text for _, text in wifi_options] == shown
        assert [i for i, (a, _) in enumerate(wifi_options) if "selected" in a] == [0]
        assert all("disabled" not in a for a, _ in wifi_options)
        for ssid in shown:
            escaped = html.escape(ssid, quote=True).replace("&#x27;", "&#39;")
            assert f'value="{escaped}"' in body
            assert f'>{escaped}</option>' in body
    else:
        assert len(wifi_options) == 1
        attrs, text = wifi_options[0]
        assert attrs.get("value") == "" and "selected" in attrs and "disabled" in attrs
        assert "没扫到网络" in text



def check_save(executable):
    url = "http://192.168.1.2:8000/xiaozhi/ota/"
    max_url = url + "x" * (255 - len(url))
    utf8_ssid = "天气" * 5 + "AB"
    password = "p + % & ssid=intruder"
    for pairs, expected in (
        ([("other", "ignored"), ("xssid", "fake"), ("ssid", utf8_ssid),
          ("pass", password), ("voice_url", url)],
         (utf8_ssid, password, url)),
        ([("ssid", "Selected"), ("ssid2", "Manual +%&"), ("voice_url", url)],
         ("Manual +%&", "", url)),
        ([("ssid", "A" * 32), ("pass", "+" * 64), ("voice_url", max_url)],
         ("A" * 32, "+" * 64, max_url)),
    ):
        subprocess.run([executable, "save", urlencode(pairs), *expected], check=True, timeout=5)

    url_field = urlencode({"voice_url": url})
    for malformed in (
        "other=ssid=wrong", "xssid=wrong", "ssid=", "ssid=bad%",
        "ssid=bad%2", "ssid=bad%GG", "ssid=bad%00suffix",
        "ssid=" + "X" * 33, "ssid=valid&pass=" + "%2B" * 65,
    ):
        subprocess.run([executable, "save", f"{malformed}&{url_field}"], check=True, timeout=5)
    oversized_url = urlencode({"ssid": "Home", "voice_url": max_url + "x"})
    subprocess.run([executable, "save", oversized_url], check=True, timeout=5)
    print("PASS save: fragmented body, decoded byte limits, exact keys, manual SSID and voice options")
    print("PASS save: malformed input rejected before NVS")

def main():
    assets = {name: (ROOT / "main/web" / name).read_text(encoding="utf-8") for name in ASSETS.values()}
    check_assets(assets)
    cases = {
        "normal / skip own AP / first selection": [OWN, "Home", OWN, "Cafe"],
        "no networks": [],
        "own AP only": [OWN],
        "HTML escaping": ['<&>"\'WiFi', '"><script>alert(1)</script>'],
        "non-recursive template markers": ["{{STYLE}}{{SCRIPT}}{{OPTIONS}}", "{{COUNT}}{{AP_URL}}{{AP_SSID}}"],
        "16 full-length SSIDs / worst escaping": ['"' * 32] + ['"' * 31 + chr(65 + i) for i in range(15)],
        "32-byte UTF-8 SSID": ["天气" * 5 + "AB"],
    }
    with tempfile.TemporaryDirectory(prefix="weather-portal-") as temp:
        executable, ap_url = build_harness(Path(temp), assets)
        check_save(executable)
        for label, ssids in cases.items():
            check_root(executable, assets, ap_url, ssids)
            print(f"PASS root: {label}")
        for ok, status in (("ok", "200 OK"), ("bad", "400 Bad Request")):
            title, message = "配置结果", "已保存，请等待连接。" if ok == "ok" else "请输入 WiFi 名称。"
            actual, body = run(executable, "reply", ok, title, message)
            assert actual == status
            page = Page(body)
            assert page.sections["title"] == page.sections["h1"] == [title]
            assert message in page.sections["p"]
            assert page.sections["style"] == [assets["style.css"]]
            assert "script" not in page.sections and "{{" not in body
            print(f"PASS reply: {status}, inline CSS, no resource references")
        for template, expected in (
            ("", ""), ("plain", "plain"), ("{{EMPTY}}", ""),
            ("{{EMPTY}}tail", "tail"), ("head{{EMPTY}}", "head"),
            ("a{{EMPTY}}{{EMPTY}}b{{VALUE}}{{EMPTY}}c", "abreplacement{{EMPTY}}c"),
            ("{{VALUE}}{{VALUE}}", "replacement{{EMPTY}}replacement{{EMPTY}}"),
        ):
            status, body = run(executable, "send", template)
            assert status == "200 OK" and body == expected, (template, body)
        print("PASS send_page: empty/repeated/non-recursive values and unknown token rejection")
    print("PASS all cases: every HTTP chunk failure (including final chunk) propagated")
    print("Host/static checks only; browser, WiFi and hardware behavior not validated.")


if __name__ == "__main__":
    main()
