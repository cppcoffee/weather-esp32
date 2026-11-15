#!/usr/bin/env python3
"""Host regression for portal_web.c's real portal rendering functions and web assets.

Only Python's standard library and cc/clang/gcc are required. Generated C and the
executable live in TemporaryDirectory. HTTP, scanning and AP identity are stubs;
this does not test browsers, JavaScript execution, ESP-IDF embedding/linking,
WiFi, credential saving, captive-portal discovery or hardware behavior.

    python3 tools/test_portal.py
"""

import html
import re
import shutil
import subprocess
import tempfile
from html.parser import HTMLParser
from pathlib import Path

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
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/types.h>
#define ESP_OK 0
#define ESP_FAIL -1
#define SEND_ERROR -73
#define HTTPD_RESP_USE_STRLEN -1
#define ESP_LOGE(...) ((void)0)
typedef int esp_err_t;
typedef struct {
    char body[131072];
    size_t used;
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
'''

SCAN_STUB = r'''
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
    ap_url = extract((ROOT / "main/net.h").read_text(), r'(^#define NET_AP_URL "[^"\n]+")')
    declarations = [
        extract(source, r"(^#define MAX_APS\s+\d+)"),
        extract(source, r"(^typedef struct \{[^{}]*\} scan_ap_t;)"),
        extract(source, r"(^typedef struct \{[^{}]*\} page_value_t;)"),
    ]
    code = "\n".join([
        STUBS, ap_url, *declarations, SCAN_STUB, *definitions,
        *(function(source, name) for name in
          ("html_escape", "addf", "send_page", "root_get", "reply")), RUNNER,
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
        "index.html": {"STYLE", "SCRIPT", "OPTIONS", "COUNT", "AP_URL", "AP_SSID"},
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
    assert page.sections["footer"] == [f"扫到 {len(ssids)} 个网络 · {ap_url} · {OWN}"]
    forms = [a for tag, a in page.elements if tag == "form"]
    assert len(forms) == 1 and forms[0].get("method", "").lower() == "post"
    assert forms[0].get("action") == "/save"
    fields = {a.get("name"): (tag, a) for tag, a in page.elements if tag in {"input", "select"}}
    assert set(fields) == {"ssid", "ssid2", "pass"}
    assert fields["ssid"][0] == "select"
    assert fields["ssid2"][1].get("type") == "text"
    assert fields["pass"][1].get("type") == "password"
    assert any(tag == "button" and a.get("type") == "submit" for tag, a in page.elements)
    shown = [ssid for ssid in ssids if ssid != OWN]
    if shown:
        assert [a.get("value") for a, _ in page.options] == shown
        assert [text for _, text in page.options] == shown
        assert [i for i, (a, _) in enumerate(page.options) if "selected" in a] == [0]
        assert all("disabled" not in a for a, _ in page.options)
        for ssid in shown:
            escaped = html.escape(ssid, quote=True).replace("&#x27;", "&#39;")
            assert f'value="{escaped}"' in body
            assert f'>{escaped}</option>' in body
    else:
        assert len(page.options) == 1
        attrs, text = page.options[0]
        assert attrs.get("value") == "" and "selected" in attrs and "disabled" in attrs
        assert "没扫到网络" in text


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
