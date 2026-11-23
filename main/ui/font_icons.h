// Weather Icons by Erik Flowers, Lukas Bischoff (v1 Art); 1bpp subsets, SIL
// OFL 1.1. Source and complete license:
// third_party/weather-icons/{README.md,OFL.txt}.
#ifndef __FONT_ICONS_H__
#define __FONT_ICONS_H__

#include "fonts_aa.h"

// Fixed cells; xoff=(advance-w)/2, yoff=-line_height+(line_height-h)/2.
extern const aafont font_icon_daily; // advance=40, height=27; baseline 73 -> y=46..72
extern const aafont font_icon;       // advance=26, height=23; baseline 69 -> y=46..68

// UTF-8 bytes, not literal PUA characters (keep genfont.py's CJK scan clean).
#define WI_SUN           "\xEF\x80\x8D" // wi-day-sunny, U+F00D
#define WI_PARTLY_CLOUDY "\xEF\x80\x82" // wi-day-cloudy, U+F002
#define WI_CLOUD         "\xEF\x80\x93" // wi-cloudy, U+F013
#define WI_FOG           "\xEF\x80\x94" // wi-fog, U+F014
#define WI_DRIZZLE       "\xEF\x80\x8B" // wi-day-sprinkle, U+F00B
#define WI_RAIN          "\xEF\x80\x99" // wi-rain, U+F019
#define WI_SNOW          "\xEF\x80\x9B" // wi-snow, U+F01B
#define WI_SHOWERS       "\xEF\x80\x9A" // wi-showers, U+F01A
#define WI_SNOW_SHOWERS  "\xEF\x80\x8A" // wi-day-snow, U+F00A (F01D is wi-storm-showers)
#define WI_THUNDER       "\xEF\x80\x9E" // wi-thunderstorm, U+F01E
#define WI_HAIL          "\xEF\x80\x95" // wi-hail, U+F015

#endif
