#!/usr/bin/env python3
"""Verify the production speaker volume code with in-memory codec and NVS stubs."""

import os
from pathlib import Path
import re
import shutil
import tempfile

from test_voice_lifecycle import extract, run


STUBS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int esp_err_t, nvs_handle_t;
enum { ESP_OK, ESP_FAIL, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE };
enum { NVS_READONLY, NVS_READWRITE, portMAX_DELAY = 1000 };
static bool s_ready, locked, opened;
static int s_control_lock, s_codec, s_volume;
static int codec_volume, saved = -1, pending = -1;
static bool fail_codec, fail_open, fail_set, fail_commit;

static void xSemaphoreTake(int lock, int ticks) {
    assert(lock == s_control_lock && ticks == portMAX_DELAY && !locked);
    locked = true;
}
static void xSemaphoreGive(int lock) {
    assert(lock == s_control_lock && locked && !opened);
    locked = false;
}
static int esp_codec_dev_set_out_vol(int codec, int volume) {
    assert(codec == s_codec && locked && opened);
    if (fail_codec) return ESP_FAIL;
    codec_volume = volume;
    return ESP_OK;
}
static int check_codec_result(int result, const char *operation) {
    assert(operation);
    return result;
}
static int nvs_open(const char *name, int mode, nvs_handle_t *nvs) {
    assert(!opened && !strcmp(name, "audio") && locked == (mode == NVS_READWRITE));
    if (fail_open) return ESP_FAIL;
    *nvs = 1; opened = true;
    return ESP_OK;
}
static int nvs_get_u8(nvs_handle_t nvs, const char *key, uint8_t *volume) {
    assert(nvs == 1 && opened && !locked && !strcmp(key, "volume"));
    if (saved < 0) return ESP_FAIL;
    *volume = saved;
    return ESP_OK;
}
static int nvs_set_u8(nvs_handle_t nvs, const char *key, uint8_t volume) {
    assert(nvs == 1 && opened && locked && !strcmp(key, "volume"));
    if (fail_set) return ESP_FAIL;
    pending = volume;
    return ESP_OK;
}
static int nvs_commit(nvs_handle_t nvs) {
    assert(nvs == 1 && opened && locked && pending >= 0);
    if (fail_commit) return ESP_FAIL;
    saved = pending;
    return ESP_OK;
}
static void nvs_close(nvs_handle_t nvs) {
    assert(nvs == 1 && opened);
    pending = -1; opened = false;
}
'''

CHECKS = r'''
int main(void) {
    int volume;
    assert(audio_get_volume(&volume) == ESP_ERR_INVALID_STATE);
    assert(audio_set_volume(0) == ESP_ERR_INVALID_STATE);
    assert(load_volume() == AUDIO_DEFAULT_VOLUME);
    saved = 255;
    assert(load_volume() == AUDIO_DEFAULT_VOLUME);
    s_ready = true;
    codec_volume = s_volume = load_volume();
    assert(audio_get_volume(NULL) == ESP_ERR_INVALID_ARG);
    assert(audio_set_volume(-1) == ESP_ERR_INVALID_ARG);
    assert(audio_set_volume(101) == ESP_ERR_INVALID_ARG);
    assert(audio_set_volume(0) == ESP_OK && load_volume() == 0 && codec_volume == 0);
    assert(audio_set_volume(100) == ESP_OK && load_volume() == 100 && codec_volume == 100);
    assert(audio_get_volume(&volume) == ESP_OK && volume == 100);

    fail_open = true;
    assert(audio_set_volume(35) == ESP_FAIL && codec_volume == 100);
    fail_open = false;
    fail_codec = true;
    assert(audio_set_volume(35) == ESP_FAIL && load_volume() == 100 && codec_volume == 100);
    fail_codec = false;
    fail_set = true;
    assert(audio_set_volume(35) == ESP_FAIL && load_volume() == 100 && codec_volume == 100);
    fail_set = false;
    fail_commit = true;
    assert(audio_set_volume(35) == ESP_FAIL && load_volume() == 100 && codec_volume == 100);
    assert(audio_get_volume(&volume) == ESP_OK && volume == 100);
    fail_commit = false;
    assert(audio_set_volume(35) == ESP_OK && load_volume() == 35 && codec_volume == 35);
    puts("PASS audio volume: startup fallback, persistence, range, codec/NVS failures and retry");
}
'''


def main():
    path = Path(__file__).resolve().parents[1] / "main/audio.c"
    source = path.read_text(encoding="utf-8")
    default = re.search(r"^#define AUDIO_DEFAULT_VOLUME\b[^\n]+", source, re.MULTILINE).group()
    functions, _ = extract(source + "\n/* end of volume code */\n", path, (
        ("static int load_volume(void)", "static esp_err_t init_codec(void)"),
        ("esp_err_t audio_get_volume(int *volume)", "esp_err_t audio_set_volume(int volume)"),
        ("esp_err_t audio_set_volume(int volume)", "/* end of volume code */"),
    ))
    env = dict(os.environ, LC_ALL="C", ASAN_OPTIONS="halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1")
    with tempfile.TemporaryDirectory(prefix="weather-audio-volume-") as directory:
        work = Path(directory)
        harness = work / "volume.c"
        exe = work / "volume"
        harness.write_text("\n".join((STUBS, default, functions, CHECKS)), encoding="utf-8")
        run([shutil.which("cc") or "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-fsanitize=address,undefined", harness, "-o", exe], work, env, 30)
        print(run([exe], work, env, 10), end="")


if __name__ == "__main__":
    main()
