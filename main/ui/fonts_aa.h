// Glyphs are 1bpp, grid-fitted by FreeType's mono rasterizer, and each glyph's
// rows are stored padded to whole bytes.
#ifndef __FONTS_AA_H__
#define __FONTS_AA_H__

#include <stdint.h>

typedef struct {
    uint16_t offset;     // byte offset into the font data
    uint8_t  w, h, adv;  // bitmap size, advance
    int8_t   xoff, yoff; // left/top offset from the pen (baseline origin)
} aaglyph;

typedef struct {
    const uint8_t  *data;
    const aaglyph  *glyphs;
    const uint16_t *cps; // sorted codepoints, glyphs[i] belongs to cps[i]
    uint16_t        n;
    uint8_t         line_height;
} aafont;

extern const aafont font_mid;   // 16px medium; the only font with Chinese glyphs
extern const aafont font_temp;  // 14px medium, Latin only, for the five-day ranges
extern const aafont font_small; // 12px regular, Latin only
extern const aafont font_clock; // 12px bold, Latin only, for the top-right clock

#endif
