// 2.9" e-paper, SSD1680. Panel RAM is 128x296 portrait; drawing uses landscape coords.
// EPD_W stays 300 for the existing layout, but only x 0..295 exist on the glass; the
// last four logical columns are clipped in epd_pixel().
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "fonts_aa.h"

#define PANEL_W 128 // physical RAM: 128 wide, 296 tall (SSD1680 gate limit)
#define PANEL_H 296

#define EPD_W      300 // logical drawing surface (landscape); visible width is PANEL_H (296)
#define EPD_H      128
#define EPD_ROT_CW 1 // 1: content rotated 90 deg clockwise; 0: counter-clockwise

#define EPD_SCK  9
#define EPD_MOSI 10
#define EPD_RST  11
#define EPD_DC   12
#define EPD_CS   13
#define EPD_BUSY 14

#define EPD_WHITE 1
#define EPD_BLACK 0

void epd_init(void);
void epd_clear(int color);
void epd_pixel(int x, int y, int color);
void epd_hline(int x, int y, int w, int color);

void epd_line(int x0, int y0, int x1, int y1, int color);

void epd_fill_rect(int x, int y, int w, int h, int color);
void epd_fill_circle(int cx, int cy, int r, int color);
void epd_text(int x, int y_baseline, const aafont *f, const char *s); // y = baseline
int epd_text_w(const aafont *f, const char *s);
void epd_refresh(void);
// Page-switch refresh: switches 1..7 use the partial waveform; switch 8 uses a full refresh
// (also consuming any due maintenance refresh) and restarts the switch count.
void epd_refresh_page_switch(void);
// Full waveform now, without consuming the periodic partial-refresh cadence.
void epd_refresh_full(void);
