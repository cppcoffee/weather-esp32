// 2.9" e-paper, SSD1680. Panel RAM is 128x296 portrait; drawing uses landscape
// coords. EPD_W stays 300 for the existing layout, but only x 0..295 exist on
// the glass; the last four logical columns are clipped in epd_pixel().
#ifndef __EPD_H__
#define __EPD_H__

#include <stdbool.h>
#include <stdint.h>

#include "fonts_aa.h"

#define PANEL_W 128 // physical RAM: 128 wide, 296 tall (SSD1680 gate limit)
#define PANEL_H 296

#define EPD_W 300 // logical drawing surface (landscape); visible width is PANEL_H (296)
#define EPD_H 128

#define EPD_SCK  38
#define EPD_MOSI 39
#define EPD_RST  40
#define EPD_DC   41
#define EPD_CS   42
#define EPD_BUSY 21

#define EPD_WHITE 1
#define EPD_BLACK 0

void epd_init(void);
void epd_clear(int color);
void epd_pixel(int x, int y, int color);
void epd_hline(int x, int y, int w, int color);

void epd_fill_rect(int x, int y, int w, int h, int color);
void epd_text(int x, int y_baseline, const aafont *f,
              const char *s); // y = baseline
int  epd_text_w(const aafont *f, const char *s);
// Update the changed region with the partial waveform.
void epd_refresh(void);
// Only page switches advance the cadence. Every eighth switch uses the full
// waveform.
void epd_refresh_page_switch(void);

#endif
