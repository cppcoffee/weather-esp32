#!/usr/bin/env python3
"""Render DEMO five-day weather with the real UI, epd.c and bundled glyphs.

No hardware, network, ESP-IDF, Pillow or firmware build is required. Only Python's
standard library and a host C compiler are used. The weather and sensor samples
are synthetic, not live readings; the clock is fixed to 2026-11-14 08:30 UTC.

    python3 tools/preview_ui.py
    python3 tools/preview_ui.py --sanitize

Default: build/preview-5day.png, enlarged 3x with nearest-neighbor pixels, plus
native 296x128, valid-sensor, wide-value placeholder, retry, initial-failure and
additional weather-icon, temperature-range, dry, unknown, thunder advisory and
hourly scenes. Daily temperatures use one low-high row (temp font, then small font
when needed, or -- for invalid/oversized ranges) written as low°-high°, and today's
column is labelled 今天 with the 16px font. Sensors occupy only the single-line
footer. Daily precipitation is the maximum time-slot chance,
used as a conservative all-day umbrella hint, never a current-rain observation.
Every invocation also runs UI assertions against real SPI framebuffer captures.
Only HEADERS is extracted from test_app.py, without importing its changing
application/weather harness. Temporary C files and binaries are auto-removed.
"""

import argparse
import ast
from datetime import datetime, timezone
import io
import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib


ROOT = Path(__file__).resolve().parents[1]
WIDTH, HEIGHT = 296, 128
FRAME_BYTES = WIDTH * HEIGHT // 8
HEADER_NAMES = (
    "esp_log.h", "esp_timer.h", "freertos/FreeRTOS.h", "freertos/task.h",
    "driver/gpio.h", "driver/spi_master.h",
)


def host_headers():
    """Reuse only the literal hardware stubs, never execute test_app.py."""
    tree = ast.parse((ROOT / "tools/test_app.py").read_text(encoding="utf-8"))
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(
            isinstance(target, ast.Name) and target.id == "HEADERS"
            for target in node.targets
        ):
            headers = ast.literal_eval(node.value)
            break
    else:
        raise RuntimeError("tools/test_app.py must provide the HEADERS literal")
    headers = {name: headers[name] for name in HEADER_NAMES}
    # Observe D/C so a 4736-byte transfer must really be the 0x24 framebuffer.
    gpio_tree = headers["driver/gpio.h"]
    marker = "static inline int gpio_set_level("
    if marker in gpio_tree:
        start = gpio_tree.index(marker)
        end = gpio_tree.index("}", start) + 1
        headers["driver/gpio.h"] = (
            gpio_tree[:start] + "int gpio_set_level(int pin, int level);" + gpio_tree[end:]
        )
    return headers


HARNESS = r'''
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ui.h"
#include "epd.h"
#include "font_icons.h"
#include "net.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"

_Static_assert(WEATHER_DAYS == 5, "preview requires the five-day weather contract");
_Static_assert(EPD_W == 300 && EPD_H == 128 && PANEL_H == 296, "preview expects 296x128 visible");
enum { COL_W = 52, GRID_X = (PANEL_H - WEATHER_DAYS * COL_W) / 2 };
enum { FRAME_BYTES = PANEL_W / 8 * PANEL_H, MAX_TEXT = 48 };
/* Row anchors exactly as ui_draw_weather() paints them; DATE_BASE clears both the
   header rule (y=22) and the 16px CJK 今天; icons fill ICON_TOP..ICON_BOTTOM.
   DAILY_TEMP_BASE mirrors ui.c's TEMP_BASE, which is a file-local #define and therefore
   not visible here; it anchors the five-day range row and the hourly temperatures. */
enum { DATE_BASE = 42, ICON_TOP = 46, ICON_BOTTOM = 72 };
enum { DAILY_TEMP_BASE = 89, PRECIP_BASE = 103, UNKNOWN_ICON_BASE = ICON_TOP + 20 };
enum { ICON_ROWS = ICON_BOTTOM - ICON_TOP + 1, ICON_BYTES = COL_W * ICON_ROWS };
/* The vertical band the single-line range may repaint: 14 rows above its baseline
   (mid font ink) down to 2 rows below it. It moves with DAILY_TEMP_BASE, so at 89 it
   covers rows 75..91 and stays clear of the icon band above and precipitation below. */
enum { TEMP_CELL_TOP = DAILY_TEMP_BASE - 14, TEMP_CELL_BOTTOM = DAILY_TEMP_BASE + 2 };
enum { TEMP_MAX_WIDTH = 48 };
static unsigned char frame[FRAME_BYTES];
static unsigned frames;
static int dc, command;
static bool weather_scene;
static struct { int x, y; const aafont *font; char text[96]; } calls[MAX_TEXT];
static unsigned call_count;

void vTaskDelay(unsigned ms) { (void)ms; }
const char *net_ap_ssid(void) { return "weather-DEMO"; }
time_t time(time_t *out) {
    time_t fixed = @EPOCH@;
    if (out) *out = fixed;
    return fixed;
}
int gpio_config(const gpio_config_t *cfg) {
    (void)cfg;
    return 0;
}
int gpio_set_level(int pin, int level) {
    if (pin == EPD_DC) dc = level;
    return 0;
}
int gpio_get_level(int pin) {
    /* BUSY low: epd.c's wait_idle() returns immediately. */
    (void)pin;
    return 0;
}
int spi_device_polling_transmit(spi_device_handle_t spi, spi_transaction_t *t) {
    (void)spi;
    if (!dc) {
        assert(t->length == 8);
        command = *(const unsigned char *)t->tx_buffer;
    } else if (t->length == FRAME_BYTES * 8) {
        assert(command == 0x24);
        memcpy(frame, t->tx_buffer, FRAME_BYTES);
        ++frames;
    }
    return 0;
}

static const aaglyph *glyph(const aafont *f, unsigned cp) {
    for (unsigned i = 0; i < f->n; ++i) {
        if (f->cps[i] == cp) return &f->glyphs[i];
    }
    return NULL;
}
static unsigned next_cp(const unsigned char **p) {
    unsigned cp = *(*p)++;
    unsigned n = cp >= 0xf0 ? 3 : cp >= 0xe0 ? 2 : cp >= 0xc0 ? 1 : 0;
    cp &= n == 3 ? 0x07 : n == 2 ? 0x0f : n == 1 ? 0x1f : 0x7f;
    while (n--) {
        assert((**p & 0xc0) == 0x80 && "invalid/truncated UTF-8");
        cp = (cp << 6) | (*(*p)++ & 0x3f);
    }
    return cp;
}

/* Forward every call to epd.c, while checking actual glyph ink bounds. */
void preview_real_epd_text(int x, int y, const aafont *f, const char *text);
void epd_text(int x, int y, const aafont *f, const char *text) {
    assert(call_count < MAX_TEXT && strlen(text) < sizeof(calls[0].text));
    calls[call_count].x = x;
    calls[call_count].y = y;
    calls[call_count].font = f;
    strcpy(calls[call_count++].text, text);
    bool icon_font = f == &font_icon_daily || f == &font_icon;
    if (icon_font) {
        bool compact = f == &font_icon;
        int advance = compact ? 26 : 40, height = compact ? 23 : 27;
        int start = compact ? 8 : GRID_X, step = compact ? 28 : COL_W;
        int col = (x - start) / step;
        assert(!weather_scene || !compact); /* Daily scenes must use the larger font. */
        assert(f->line_height == height && y == ICON_TOP + height);
        assert(col >= 0 && col < (compact ? 10 : 5));
        assert(epd_text_w(f, text) == advance && x == start + col * step + step / 2 - advance / 2);
        const unsigned char *next = (const unsigned char *)text;
        unsigned cp = next_cp(&next);
        assert(!*next); /* Exactly one icon, never a string of fallback glyphs. */
        const aaglyph *g = glyph(f, cp);
        assert(g && g->adv == advance && g->w > 0 && g->h > 0);
        assert(g->w <= advance && g->h <= height);
        assert(g->xoff == (advance - g->w) / 2 && g->yoff == -height + (height - g->h) / 2);
        int left = PANEL_H, right = -1, top = EPD_H, bottom = -1;
        for (unsigned row = 0; row < g->h; ++row) {
            for (unsigned bit = 0; bit < g->w; ++bit) {
                if (!(f->data[g->offset + row * ((g->w + 7) / 8) + bit / 8] & (0x80 >> (bit & 7))))
                    continue;
                int px = x + g->xoff + bit, py = y + g->yoff + row;
                if (px < left) left = px;
                if (px > right) right = px;
                if (py < top) top = py;
                if (py > bottom) bottom = py;
            }
        }
        assert(left <= right && top <= bottom); /* Real ink, not just a nonempty rectangle. */
        assert(left >= start + col * step && right < start + (col + 1) * step);
        assert(top >= ICON_TOP && bottom < ICON_TOP + height);
        assert(abs(left + right + 1 - (2 * x + advance)) <= 1);
        assert(abs(top + bottom + 1 - (2 * ICON_TOP + height)) <= 1);
    }
    int pen = x;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        unsigned cp = next_cp(&p);
        const aaglyph *g = glyph(f, cp);
        assert(g && "missing real glyph");
        if (g->w && g->h) {
            int left = pen + g->xoff, top = y + g->yoff;
            int right = left + g->w, bottom = top + g->h;
            assert(left >= 0 && right <= EPD_W && top >= 0 && bottom <= EPD_H);
            if (weather_scene && !icon_font) {
                assert(f != &font_huge);
                assert(y == 16 || y == DATE_BASE || y == UNKNOWN_ICON_BASE ||
                       y == DAILY_TEMP_BASE || y == PRECIP_BASE || y == 124);
            }
            if (weather_scene && y == 16) {
                assert(top >= 0 && bottom <= 22);
                if (f == &font_clock) assert(left >= 230 && right <= 292);
                else if (x == 8) assert(left >= 8 && right <= 104);
                else assert(left >= 126 && right <= 222);
            }
            if (weather_scene && y == 124) {
                assert(f == &font_mid && top >= 109 && bottom <= 128);
                if (x == 46) assert(left >= 46 && right <= 104);
                else if (x == 146) assert(left >= 146 && right <= 196);
                else if (x >= 200) assert(left >= 200 && right <= 292); /* status corner */
                else assert((x == 8 && right <= 40) || (x == 110 && right <= 142));
            }
            if (weather_scene && (y == DATE_BASE || y == UNKNOWN_ICON_BASE ||
                                  y == DAILY_TEMP_BASE || y == PRECIP_BASE)) {
                /* Dates sit between the header rule (22) and the icon band (46); the
                   unknown-icon "?" stays inside the icon band; the range and the daily
                   probability keep to their own bands. */
                if (y == DATE_BASE) {
                    assert((f == &font_mid || f == &font_small) && top >= 24 && bottom <= 45);
                } else if (y == UNKNOWN_ICON_BASE) {
                    assert(f == &font_mid && top >= ICON_TOP && bottom <= ICON_BOTTOM);
                } else if (y == DAILY_TEMP_BASE) {
                    assert(f == &font_temp || f == &font_small);
                    assert(top >= TEMP_CELL_TOP && bottom <= TEMP_CELL_BOTTOM);
                } else {
                    assert(f == &font_small && top >= 90 && bottom <= 104);
                }
                int col = (x - GRID_X) / COL_W;
                assert(col >= 0 && col < 5);
                assert(left >= GRID_X + col * COL_W);
                assert(right <= GRID_X + (col + 1) * COL_W);
                if (y == DAILY_TEMP_BASE) {
                    assert(epd_text_w(f, text) <= TEMP_MAX_WIDTH);
                    assert(left >= GRID_X + 2 + col * COL_W && right <= GRID_X + 50 + col * COL_W);
                }
            }
            if (cp > 127) {
                bool visible = false;
                for (unsigned i = 0; i < (unsigned)((g->w + 7) / 8) * g->h; ++i)
                    visible |= f->data[g->offset + i] != 0;
                assert(visible && "blank Chinese glyph");
            }
        }
        pen += g->adv;
    }
    preview_real_epd_text(x, y, f, text);
}

static void expect(int x, int y, const aafont *f, const char *text) {
    for (unsigned i = 0; i < call_count; ++i) {
        if (calls[i].x == x && calls[i].y == y && calls[i].font == f &&
            !strcmp(calls[i].text, text)) return;
    }
    fprintf(stderr, "missing text '%s' at %d,%d\n", text, x, y);
    abort();
}
static void centered(int cx, int y, const aafont *f, const char *text) {
    expect(cx - epd_text_w(f, text) / 2, y, f, text);
}
static void render(const weather_t *w, const sensor_reading_t *s) {
    call_count = 0;
    weather_scene = true;
    unsigned before = frames;
    ui_draw_weather(w, s);
    unsigned ranges = 0;
    for (unsigned i = 0; i < call_count; ++i) ranges += calls[i].y == DAILY_TEMP_BASE;
    assert(ranges == 5); /* Exactly five single-line low-high ranges. */
    epd_refresh();
    assert(frames == before + 1);
}
static void emit(const char *name) {
    printf("%s\n", name);
    assert(fwrite(frame, 1, FRAME_BYTES, stdout) == FRAME_BYTES);
}
static int pixel(const unsigned char *fb, int x, int y) {
    if (x < 0 || x >= EPD_W || y < 0 || y >= EPD_H || x >= PANEL_H) return EPD_WHITE;
#if EPD_ROT_CW
    int px = EPD_H - 1 - y, py = x;
#else
    int px = y, py = EPD_W - 1 - x;
#endif
    return (fb[py * (PANEL_W / 8) + px / 8] >> (7 - px % 8)) & 1;
}
static void forecast_unchanged(const unsigned char *previous) {
    for (int y = 0; y <= 108; ++y)
        for (int x = 0; x < EPD_W; ++x)
            assert(pixel(previous, x, y) == pixel(frame, x, y));
}
static void retry_preserves_body(const unsigned char *previous, unsigned attempt) {
    call_count = 0;
    weather_scene = false;
    ui_draw_retry(attempt, 30);
    epd_refresh();
    bool changed = false;
    for (int y = 0; y < EPD_H; ++y) {
        for (int x = 0; x < EPD_W; ++x) {
            if (y >= 22) assert(pixel(previous, x, y) == pixel(frame, x, y));
            else changed |= pixel(previous, x, y) != pixel(frame, x, y);
        }
    }
    assert(changed);
    /* Independently rebuild the header, proving no fresh umbrella advice remains. */
    unsigned char retried[FRAME_BYTES];
    char heading[64];
    memcpy(retried, frame, sizeof(retried));
    epd_fill_rect(0, 0, EPD_W, 22, EPD_WHITE);
    snprintf(heading, sizeof(heading), "无网络 30s后重试 #%u", attempt);
    preview_real_epd_text(8, 16, &font_mid, heading);
    epd_refresh();
    assert(!memcmp(retried, frame, sizeof(frame)));
}
static void expect_forecast(const weather_t *w) {
    for (int i = 0; i < 5; ++i) {
        int cx = GRID_X + COL_W / 2 + i * COL_W;
        char text[100];
        if (i == 0) {
            /* Today is the row's only CJK label, so it needs the 16px mid font. */
            centered(cx, DATE_BASE, &font_mid, "今天");
        } else {
            snprintf(text, sizeof(text), "%.2s/%.2s", w->days[i].date + 5, w->days[i].date + 8);
            centered(cx, DATE_BASE, &font_small, text);
        }
        int n = snprintf(text, sizeof(text), "%.0f°-%.0f°", (double)w->days[i].tmin, (double)w->days[i].tmax);
        assert(n >= 0 && (size_t)n < sizeof(text));
        const aafont *font = epd_text_w(&font_temp, text) <= TEMP_MAX_WIDTH ? &font_temp : &font_small;
        centered(cx, DAILY_TEMP_BASE, font, text);
        snprintf(text, sizeof(text), "%ld%%", lroundf(w->days[i].precip_probability));
        centered(cx, PRECIP_BASE, &font_small, text);
    }
}

static int icon_family(int code) {
    if (code == 0 || code == 1) return 1;
    if (code == 2) return 2;
    if (code == 3) return 3;
    if (code == 45 || code == 48) return 4;
    if (code == 51 || code == 53 || code == 55 || code == 56 || code == 57) return 5;
    if (code == 61 || code == 63 || code == 65 || code == 66 || code == 67) return 6;
    if (code == 71 || code == 73 || code == 75 || code == 77) return 7;
    if (code >= 80 && code <= 82) return 8;
    if (code == 85 || code == 86) return 9;
    if (code == 95) return 10;
    if (code == 96 || code == 99) return 11;
    return 0;
}
static void icon_mask(int col, unsigned char mask[ICON_BYTES]) {
    unsigned n = 0, ink = 0;
    for (int y = ICON_TOP; y <= ICON_BOTTOM; ++y) {
        for (int x = GRID_X + col * COL_W; x < GRID_X + (col + 1) * COL_W; ++x) {
            int value = pixel(frame, x, y);
            mask[n++] = value;
            ink += !value;
        }
    }
    assert(ink > 0 && ink < (unsigned)ICON_BYTES);
}
static void check_icons(const weather_t *demo) {
    enum { FAMILIES = 12 };
    const int representative[FAMILIES] = {-1, 0, 2, 3, 45, 51, 61, 71, 80, 85, 95, 96};
    unsigned char samples[FAMILIES][ICON_BYTES], mask[ICON_BYTES], unknown[FRAME_BYTES];
    weather_t base = *demo;
    /* Hold the banner pending while independently exercising today's icon. */
    base.days[0].precip_probability = NAN;
    for (int i = 0; i < 5; ++i) base.days[i].code = -1;
    render(&base, NULL);
    memcpy(unknown, frame, sizeof(unknown));
    for (int i = 0; i < 5; ++i) centered(GRID_X + COL_W / 2 + 52 * i, UNKNOWN_ICON_BASE, &font_mid, "?");
    for (int family = 0; family < FAMILIES; ++family) {
        weather_t sample = base;
        sample.days[0].code = representative[family];
        render(&sample, NULL);
        icon_mask(0, samples[family]);
        for (int previous = 0; previous < family; ++previous)
            assert(memcmp(samples[family], samples[previous], ICON_BYTES));
    }
    /* Every 0..99 code and nearby invalid values, in every column: changes may
       affect only y46..72 of that column, never dates, temperatures or neighbors. */
    for (int col = 0; col < 5; ++col) {
        for (int code = -2; code <= 101; ++code) {
            weather_t candidate = base;
            candidate.days[col].code = code;
            render(&candidate, NULL);
            icon_mask(col, mask);
            assert(!memcmp(mask, samples[icon_family(code)], ICON_BYTES));
            for (int y = 0; y < EPD_H; ++y) {
                for (int x = 0; x < EPD_W; ++x) {
                    if (y >= ICON_TOP && y <= ICON_BOTTOM && x >= GRID_X + col * COL_W &&
                        x < GRID_X + (col + 1) * COL_W)
                        continue;
                    assert(pixel(unknown, x, y) == pixel(frame, x, y));
                }
            }
        }
    }
}

static void check_hints(const weather_t *demo) {
    const struct { float value; int displayed; } probabilities[] = {
        {NAN,-1}, {INFINITY,-1}, {-INFINITY,-1}, {-0.1f,-1}, {100.1f,-1},
        {FLT_MAX,-1}, {-FLT_MAX,-1}, {0,0}, {0.49f,0}, {0.5f,1},
        {39.49f,39}, {39.5f,40}, {40,40}, {49.4f,49}, {49.5f,50}, {49.6f,50},
        {50,50}, {50.4f,50}, {50.5f,51}, {51,51},
        {99.49f,99}, {99.5f,100}, {100,100},
    };
    for (int code = -2; code <= 101; ++code) {
        for (unsigned p = 0; p < sizeof(probabilities) / sizeof(probabilities[0]); ++p) {
            weather_t w = *demo;
            w.days[0].code = code;
            w.days[0].precip_probability = probabilities[p].value;
            render(&w, NULL);
            int rounded = probabilities[p].displayed, family = icon_family(code);
            const char *hint;
            if (rounded < 0) hint = "预报待更新";
            else if (family >= 10) hint = "雷雨注意安全";
            else if (rounded >= 51) hint = "今日带伞";
            else if (family > 0) hint = "降水概率低";
            else hint = "预报待更新";
            centered(174, 16, &font_mid, hint);
            char text[8];
            if (rounded < 0) strcpy(text, "--%");
            else snprintf(text, sizeof(text), "%d%%", rounded);
            centered(GRID_X + COL_W / 2, PRECIP_BASE, &font_small, text);
        }
    }
    /* Invalid daily probabilities are unknown, not fake 0%; other forecast
       rows stay unchanged, and future days cannot change today's decision. */
    unsigned char baseline[FRAME_BYTES];
    render(demo, NULL);
    memcpy(baseline, frame, sizeof(baseline));
    for (int day = 0; day < 5; ++day) {
        for (unsigned p = 0; p < 7; ++p) {
            weather_t w = *demo;
            w.days[day].precip_probability = probabilities[p].value;
            render(&w, NULL);
            centered(GRID_X + COL_W / 2 + day * COL_W, PRECIP_BASE, &font_small, "--%");
            centered(174, 16, &font_mid, day == 0 ? "预报待更新" : "今日带伞");
            for (int y = 22; y < 90; ++y)
                for (int x = 0; x < EPD_W; ++x)
                    assert(pixel(baseline, x, y) == pixel(frame, x, y));
        }
    }
}

static void check_cities(const weather_t *demo) {
    const char *names[] = {"Chengdu", "WWWWWWWWWWWWWWWWWWWWWWW", "温度湿度温度",
        "温度湿度温度湿", "W温度湿度温度W"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        weather_t w = *demo;
        assert(strlen(names[i]) < sizeof(w.city));
        strcpy(w.city, names[i]);
        render(&w, NULL);
        assert(calls[0].x == 8 && calls[0].y == 16 && calls[0].font == &font_mid);
        const char *shown = calls[0].text;
        assert(epd_text_w(&font_mid, shown) <= 96);
        if (epd_text_w(&font_mid, names[i]) <= 96) {
            assert(!strcmp(shown, names[i]));
        } else {
            size_t n = strlen(shown);
            assert(n >= 3 && !strcmp(shown + n - 3, "..."));
            assert(!strncmp(shown, names[i], n - 3));
            /* The next complete codepoint would exceed the width with dots. */
            const unsigned char *next = (const unsigned char *)names[i] + n - 3;
            next_cp(&next);
            size_t extended = next - (const unsigned char *)names[i];
            char longer[32];
            memcpy(longer, names[i], extended);
            memcpy(longer + extended, "...", 4);
            assert(epd_text_w(&font_mid, longer) > 96);
        }
        centered(174, 16, &font_mid, "今日带伞");
    }
    weather_t unterminated = *demo;
    memset(unterminated.city, 'W', sizeof(unterminated.city));
    render(&unterminated, NULL);
    assert(epd_text_w(&font_mid, calls[0].text) <= 96);
    size_t length = strlen(calls[0].text);
    assert(length >= 3 && !strcmp(calls[0].text + length - 3, "..."));
}

static void expect_range(int day, const weather_t *w) {
    const weather_day_t *d = &w->days[day];
    int cx = GRID_X + COL_W / 2 + day * COL_W;
    char text[100];
    if (isfinite(d->tmin) && isfinite(d->tmax) && d->tmin <= d->tmax) {
        int n = snprintf(text, sizeof(text), "%.0f°-%.0f°", (double)d->tmin, (double)d->tmax);
        assert(n >= 0 && (size_t)n < sizeof(text));
        const aafont *font = epd_text_w(&font_temp, text) <= TEMP_MAX_WIDTH ? &font_temp : &font_small;
        if (epd_text_w(font, text) <= TEMP_MAX_WIDTH) {
            centered(cx, DAILY_TEMP_BASE, font, text);
            return;
        }
    }
    centered(cx, DAILY_TEMP_BASE, &font_temp, "--");
}

static void check_ranges(const weather_t *demo) {
    /* Each day owns one low-high row: mid font when it fits, small font for
       wide negative ranges, -- for invalid or still-oversized ranges. */
    const struct { float tmin, tmax; } pairs[] = {
        {12, 21}, {-5, 8}, {-12, -3}, {-99, -10}, {-999, -99}, {-1234, -1000},
        {NAN, 21}, {12, NAN}, {21, 12},
        {INFINITY, 21}, {12, INFINITY}, {-INFINITY, 21}, {12, -INFINITY},
        {FLT_MAX, FLT_MAX}, {-FLT_MAX, -FLT_MAX}, {0, 0}, {-0.4f, 0.4f}, {-0.6f, 0.6f},
    };
    unsigned char baseline[FRAME_BYTES];
    render(demo, NULL);
    memcpy(baseline, frame, sizeof(baseline));
    for (int day = 0; day < 5; ++day) {
        for (unsigned k = 0; k < sizeof(pairs) / sizeof(pairs[0]); ++k) {
            weather_t w = *demo;
            w.days[day].tmin = pairs[k].tmin;
            w.days[day].tmax = pairs[k].tmax;
            render(&w, NULL);
            expect_range(day, &w);
            /* A range change may only repaint that day's temperature cell. */
            for (int y = 0; y < EPD_H; ++y) {
                for (int x = 0; x < EPD_W; ++x) {
                    if (y >= TEMP_CELL_TOP && y <= TEMP_CELL_BOTTOM && x >= GRID_X + day * COL_W &&
                        x < GRID_X + (day + 1) * COL_W)
                        continue;
                    assert(pixel(baseline, x, y) == pixel(frame, x, y));
                }
            }
        }
    }
    /* Show mid-font, small-font and unknown fallbacks side by side. */
    weather_t varied = *demo;
    varied.days[0].tmin = 12;
    varied.days[0].tmax = 21;
    varied.days[1].tmin = -5;
    varied.days[1].tmax = 8;
    varied.days[2].tmin = -12;
    varied.days[2].tmax = -3;
    varied.days[3].tmin = -999;
    varied.days[3].tmax = -100;
    varied.days[4].tmin = NAN;
    varied.days[4].tmax = 9;
    render(&varied, NULL);
    for (int day = 0; day < 5; ++day) expect_range(day, &varied);
    emit("ranges");
}

int main(void) {
    assert(setenv("TZ", "UTC0", 1) == 0);
    tzset();
    epd_init();
    weather_t w = {
        .city = "Chengdu", .utc_offset = 28800, .ok = true,
        .days = {{"2026-11-14",21,12,63,80}, {"2026-11-15",23,14,2,20},
                 {"2026-11-16",20,13,3,30}, {"2026-11-17",18,10,63,70},
                 {"2026-11-18",19,9,95,90}},
    };
    unsigned char unknown[FRAME_BYTES], saved[FRAME_BYTES];
    render(&w, NULL);
    expect(8, 16, &font_mid, "Chengdu");
    expect(EPD_W - 8 - epd_text_w(&font_clock, "08:30"), 16, &font_clock, "08:30");
    expect(46, 124, &font_mid, "--.- °C");
    expect(146, 124, &font_mid, "-- %");
    expect(8, 124, &font_mid, "温度");
    expect(110, 124, &font_mid, "湿度");
    centered(174, 16, &font_mid, "今日带伞");
    expect_forecast(&w);
    memcpy(unknown, frame, sizeof(unknown));
    emit("demo");

    sensor_reading_t s = {.temperature_c = 23.4f, .humidity_percent = 56, .valid = false};
    render(&w, &s);
    assert(!memcmp(unknown, frame, sizeof(frame)));
    s.valid = true;
    render(&w, &s);
    expect(46, 124, &font_mid, "23.4 °C");
    expect(146, 124, &font_mid, "56 %");
    forecast_unchanged(unknown);
    emit("sensor");
    memcpy(saved, frame, sizeof(saved));
    retry_preserves_body(saved, 1);
    emit("retry");
    memcpy(saved, frame, sizeof(saved));
    retry_preserves_body(saved, 12);

    s.temperature_c = -123.4f;
    s.humidity_percent = 100;
    render(&w, &s);
    /* The temperature slot is 58px wide so the status corner stays clear: -123.4 °C does
       not fit and falls back to the placeholder, 100 % still does. */
    expect(46, 124, &font_mid, "--.- °C");
    expect(146, 124, &font_mid, "100 %");
    forecast_unchanged(unknown);
    s.temperature_c = 1e20f;
    render(&w, &s);
    expect(46, 124, &font_mid, "--.- °C");
    expect(146, 124, &font_mid, "100 %");
    forecast_unchanged(unknown);
    emit("wide-values");

    const float invalid_temp[] = {NAN, INFINITY, -INFINITY, FLT_MAX, -FLT_MAX};
    const float invalid_humidity[] = {NAN, INFINITY, -INFINITY, -0.1f, 100.1f};
    for (unsigned i = 0; i < sizeof(invalid_temp) / sizeof(invalid_temp[0]); ++i) {
        s.temperature_c = invalid_temp[i];
        s.humidity_percent = invalid_humidity[i];
        render(&w, &s);
        assert(!memcmp(unknown, frame, sizeof(frame)));
    }
    /* Fields validate independently; genuine measured zero is not an unknown. */
    s.temperature_c = 0;
    s.humidity_percent = NAN;
    render(&w, &s);
    expect(46, 124, &font_mid, "0.0 °C");
    expect(146, 124, &font_mid, "-- %");
    forecast_unchanged(unknown);
    s.temperature_c = NAN;
    s.humidity_percent = 0;
    render(&w, &s);
    expect(46, 124, &font_mid, "--.- °C");
    expect(146, 124, &font_mid, "0 %");
    forecast_unchanged(unknown);

    /* Sensor changes must leave the entire header/forecast/divider untouched. */
    const float temperatures[] = {-1e20f, -10000, -999.9f, -100, -99.9f, -12.3f, -0.1f,
        0, 0.1f, 9.9f, 23.4f, 99.9f, 100, 999.9f, 10000, 1e20f};
    const float humidities[] = {0, 0.1f, 9.9f, 50, 99.9f, 100};
    for (unsigned t = 0; t < sizeof(temperatures) / sizeof(temperatures[0]); ++t) {
        for (unsigned h = 0; h < sizeof(humidities) / sizeof(humidities[0]); ++h) {
            s.temperature_c = temperatures[t];
            s.humidity_percent = humidities[h];
            render(&w, &s);
            forecast_unchanged(unknown);
        }
    }

    /* The API dates, including a month boundary, are not derived from the clock. */
    weather_t crossing = w;
    strcpy(crossing.days[0].date, "2026-11-30");
    for (int i = 1; i < 5; ++i)
        snprintf(crossing.days[i].date, sizeof(crossing.days[i].date), "2026-12-%02d", i);
    render(&crossing, NULL);
    expect_forecast(&crossing);
    strcpy(crossing.days[0].date, "bad");
    strcpy(crossing.days[1].date, "2026-13-01");
    memset(crossing.days[2].date, 'X', sizeof(crossing.days[2].date));
    crossing.days[0].tmax = NAN;
    crossing.days[1].tmin = INFINITY;
    crossing.days[2].tmax = FLT_MAX;
    crossing.days[3].tmin = -FLT_MAX;
    crossing.days[4].tmax = -1234;
    render(&crossing, NULL);
    for (int i = 0; i < 3; ++i)
        centered(GRID_X + COL_W / 2 + i * COL_W, DATE_BASE, &font_small, "--/--");
    for (int i = 0; i < 5; ++i)
        centered(GRID_X + COL_W / 2 + i * COL_W, DAILY_TEMP_BASE, &font_temp, "--");

    weather_t empty = {0};
    render(&empty, NULL);
    expect(46, 124, &font_mid, "--.- °C");
    expect(146, 124, &font_mid, "-- %");
    centered(174, 16, &font_mid, "预报待更新");
    for (int i = 0; i < 5; ++i) {
        centered(GRID_X + COL_W / 2 + i * COL_W, DATE_BASE, &font_small, "--/--");
        centered(GRID_X + COL_W / 2 + i * COL_W, UNKNOWN_ICON_BASE, &font_mid, "?");
        centered(GRID_X + COL_W / 2 + i * COL_W, DAILY_TEMP_BASE, &font_temp, "--");
        centered(GRID_X + COL_W / 2 + i * COL_W, PRECIP_BASE, &font_small, "--%");
    }
    memcpy(saved, frame, sizeof(saved));
    render(NULL, NULL);
    assert(!memcmp(saved, frame, sizeof(frame)));
    retry_preserves_body(saved, 1);
    emit("initial-failure");
    empty = w;
    empty.ok = false;
    render(&empty, NULL);
    assert(!memcmp(saved, frame, sizeof(frame)));
    s.temperature_c = 23.4f;
    s.humidity_percent = 56;
    render(&empty, &s);
    expect(46, 124, &font_mid, "23.4 °C");
    expect(146, 124, &font_mid, "56 %");
    forecast_unchanged(saved);
    centered(GRID_X + COL_W / 2, DATE_BASE, &font_small, "--/--");

    check_ranges(&w);
    check_hints(&w);
    check_cities(&w);
    check_icons(&w);
    weather_t conditions = w;
    const int extra_codes[] = {45, 51, 71, 80, 85};
    for (int i = 0; i < 5; ++i) conditions.days[i].code = extra_codes[i];
    render(&conditions, NULL);
    emit("conditions");

    weather_t advice = w;
    advice.days[0].code = 0;
    advice.days[0].precip_probability = 20;
    render(&advice, NULL);
    centered(174, 16, &font_mid, "降水概率低");
    emit("dry");
    advice.days[0].code = 95;
    advice.days[0].precip_probability = 90;
    render(&advice, NULL);
    centered(174, 16, &font_mid, "雷雨注意安全");
    emit("thunder");
    for (int i = 0; i < 5; ++i) {
        advice.days[i].code = -1;
        advice.days[i].precip_probability = NAN;
    }
    render(&advice, NULL);
    centered(174, 16, &font_mid, "预报待更新");
    emit("unknown");

    /* A real ten-column page with varied synthetic hourly data; daily anchors above
       are unchanged. The icon wrapper checks the compact font's actual ink as well. */
    const int hour_codes[] = {0, 2, 3, 45, 51, 61, 71, 80, 85, 96};
    for (int i = 0; i < WEATHER_HOURS; ++i) {
        w.hours[i].hour = (8 + i) % 24;
        w.hours[i].temperature_c = 14 + i * 0.5f;
        w.hours[i].code = hour_codes[i % 10];
        w.hours[i].precip_probability = (i % 5) * 25;
    }
    weather_scene = false;
    call_count = 0;
    ui_draw_hourly(&w, NULL);
    centered(174, 16, &font_mid, "未来10小时");
    unsigned hourly_icons = 0;
    for (unsigned i = 0; i < call_count; ++i) hourly_icons += calls[i].font == &font_icon;
    assert(hourly_icons == 10);
    epd_refresh();
    emit("hourly");

    weather_scene = false;
    call_count = 0;
    ui_draw_portal_hint();
    epd_refresh();
    expect(8, 16, &font_mid, "初始化设置");
    centered(EPD_W / 2, 56, &font_mid, "WiFi: weather-DEMO");
    centered(EPD_W / 2, 96, &font_mid, "使用浏览器打开 " NET_AP_URL);
    fprintf(stderr, "PASS: %u real SPI frames; sensor footer validity/widths, unchanged "
        "forecast area, single-line low-high ranges/font fallback/invalids/isolated "
        "min-max changes, five API dates, compact WMO icons/column bounds, daily "
        "probabilities/rounded umbrella policy, UTF-8 city elision, offline advice "
        "replacement, retry body retention, portal and Chinese glyphs\n", frames);
    return 0;
}
'''


def capture_frames(compiler, sanitize=False):
    weather_header = (ROOT / "main/weather.h").read_text(encoding="utf-8")
    if not all(field in weather_header for field in ("WEATHER_DAYS", "weather_day_t", "precip_probability")):
        raise RuntimeError("main/weather.h is not yet updated to the daily precipitation probability contract")
    flags = ["-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror",
             "-Wno-unused-parameter", "-g"]
    if sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    epoch = int(datetime(2026, 11, 14, 8, 30, tzinfo=timezone.utc).timestamp())
    with tempfile.TemporaryDirectory(prefix="weather-ui-preview-") as directory:
        temporary = Path(directory)
        for name, content in host_headers().items():
            path = temporary / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
        harness = temporary / "preview.c"
        harness.write_text(HARNESS.replace("@EPOCH@", str(epoch)), encoding="utf-8")
        includes = ["-I", str(temporary), "-I", str(ROOT / "main")]
        epd_object = temporary / "epd.o"
        subprocess.run(compiler + flags + includes + [
            "-Depd_text=preview_real_epd_text", "-c", str(ROOT / "main/epd.c"),
            "-o", str(epd_object),
        ], check=True, timeout=60)
        binary = temporary / "preview"
        subprocess.run(compiler + flags + includes + [
            str(harness), str(ROOT / "main/ui.c"), str(ROOT / "main/fonts_aa.c"),
                        str(ROOT / "main/font_clock.c"), str(ROOT / "main/font_icons.c"),
            str(epd_object), "-lm", "-o", str(binary),
        ], check=True, timeout=60)
        result = subprocess.run([str(binary)], check=True, capture_output=True, timeout=30)
    print(result.stderr.decode("utf-8"), end="")
    stream = io.BytesIO(result.stdout)
    frames = {}
    while line := stream.readline():
        name = line.decode("ascii").strip()
        if name in frames:
            raise RuntimeError(f"duplicate SPI frame: {name}")
        frame = stream.read(FRAME_BYTES)
        if len(frame) != FRAME_BYTES:
            raise RuntimeError(f"short SPI frame: {name}")
        frames[name] = frame
    required = {"demo", "sensor", "wide-values", "ranges", "retry", "initial-failure", "conditions",
                "dry", "unknown", "thunder", "hourly"}
    if frames.keys() != required:
        raise RuntimeError(f"unexpected preview scenes: {frames.keys()}")
    # epd.c writes the framebuffer clockwise into 128x296 panel RAM.
    return {name: [bytes(
        255 if frame[x * 16 + (127 - y) // 8] & (0x80 >> ((127 - y) % 8)) else 0
        for x in range(WIDTH)
    ) for y in range(HEIGHT)] for name, frame in frames.items()}


def write_png(path, rows, scale, scene):
    """Lossless grayscale PNG using only struct/zlib, no antialiasing or fonts."""
    width, height = WIDTH * scale, HEIGHT * scale
    scanlines = bytearray()
    for row in rows:
        enlarged = b"".join(bytes([value]) * scale for value in row)
        scanlines.extend((b"\0" + enlarged) * scale)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    metadata = (
        "DEMO only; synthetic Chengdu forecast 2026-11-14 through 2026-11-18; "
        "not live weather or sensor data; scene=" + scene
    ).encode("ascii")
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0))
    png += chunk(b"tEXt", b"Description\0" + metadata)
    png += chunk(b"IDAT", zlib.compress(bytes(scanlines), 9))
    png += chunk(b"IEND", b"")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "build/preview-5day.png")
    parser.add_argument("--scale", type=int, default=3, choices=range(1, 17))
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--sanitize", action="store_true", help="enable host ASan and UBSan")
    args = parser.parse_args()
    compiler = shlex.split(args.cc)
    if not compiler or not shutil.which(compiler[0]):
        parser.error("a host C compiler is required (set --cc or CC)")
    try:
        scenes = capture_frames(compiler, args.sanitize)
        output = args.output.resolve()
        write_png(output, scenes["demo"], args.scale, "daily max chance; umbrella advised; unknown sensor")
        native = output.with_name(output.stem + "-native.png")
        write_png(native, scenes["demo"], 1, "umbrella advised; unknown sensor, native 296x128")
        outputs = [output, native]
        for name in ("sensor", "wide-values", "ranges", "retry", "initial-failure", "conditions",
                     "dry", "unknown", "thunder", "hourly"):
            path = output.with_name(output.stem + "-" + name + ".png")
            write_png(path, scenes[name], args.scale, name)
            outputs.append(path)
    except subprocess.CalledProcessError as error:
        if error.stderr:
            print(error.stderr.decode("utf-8", errors="replace"), end="", file=sys.stderr)
        parser.exit(1, f"host UI validation failed: {error}\n")
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        parser.exit(1, f"preview failed: {error}\n")
    print("DEMO ONLY: synthetic forecast; default sensor is unknown (--.- °C / -- %).")
    print("Daily precipitation is the maximum time-slot chance: a conservative all-day reference, not current rain.")
    print("Main DEMO: WMO 63 + 80% daily max chance -> umbrella advised; all scenes are synthetic.")
    for path in outputs:
        print(path)


if __name__ == "__main__":
    main()
