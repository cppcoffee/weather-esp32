#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "epd.h"
#include "font_icons.h"
#include "net.h"

// Five equal day columns, centered on the 296 visible pixels (18 + 5*52 + 18).
#define COL_W  52
#define GRID_X ((PANEL_H - WEATHER_DAYS * COL_W) / 2)

// Row anchors shared by both pages. DATE_BASE leaves the 16px CJK date clear of the
// header rule at y=22; the icons start below the date and end above the temperature.
#define DATE_BASE   42
#define ICON_TOP    46
#define TEMP_BASE   89
#define PRECIP_BASE 103

// Hourly strip: 10 columns, one per hour, covering the current hour .. +9 h.
#define HOUR_COLS   10
#define HOUR_STEP   1
#define HOUR_COL_W  28
#define HOUR_GRID_X ((PANEL_H - HOUR_COLS * HOUR_COL_W) / 2)

_Static_assert(WEATHER_HOURS >= (HOUR_COLS - 1) * HOUR_STEP + 1, "hourly strip needs 10 hours");

static void text_center(int cx, int y_base, const aafont *f, const char *s)
{
    epd_text(cx - epd_text_w(f, s) / 2, y_base, f, s);
}

typedef enum {
    ICON_UNKNOWN,
    ICON_SUN,
    ICON_PARTLY_CLOUDY,
    ICON_CLOUD,
    ICON_FOG,
    ICON_DRIZZLE,
    ICON_RAIN,
    ICON_SNOW,
    ICON_SHOWERS,
    ICON_SNOW_SHOWERS,
    ICON_THUNDER,
    ICON_HAIL,
} weather_icon_t;

static weather_icon_t icon_for_code(int code)
{
    // WMO ranges have gaps: undefined codes must stay unknown.
    switch (code) {
    case 0:
    case 1:
        return ICON_SUN;
    case 2:
        return ICON_PARTLY_CLOUDY;
    case 3:
        return ICON_CLOUD;
    case 45:
    case 48:
        return ICON_FOG;
    case 51:
    case 53:
    case 55:
    case 56:
    case 57:
        return ICON_DRIZZLE;
    case 61:
    case 63:
    case 65:
    case 66:
    case 67:
        return ICON_RAIN;
    case 71:
    case 73:
    case 75:
    case 77:
        return ICON_SNOW;
    case 80:
    case 81:
    case 82:
        return ICON_SHOWERS;
    case 85:
    case 86:
        return ICON_SNOW_SHOWERS;
    case 95:
        return ICON_THUNDER;
    case 96:
    case 99:
        return ICON_HAIL;
    default:
        return ICON_UNKNOWN;
    }
}

static int rounded_probability(float value)
{
    return isfinite(value) && value >= 0 && value <= 100 ? (int)roundf(value) : -1;
}

static const char *today_hint(const weather_t *w)
{
    if (!w || !w->ok) {
        return "预报待更新";
    }
    int probability = rounded_probability(w->days[0].precip_probability);
    if (probability < 0) {
        return "预报待更新";
    }
    weather_icon_t icon = icon_for_code(w->days[0].code);
    switch (icon) {
    case ICON_THUNDER:
    case ICON_HAIL:
        return "雷雨注意安全";
    default:
        if (probability >= 51) {
            return "今日带伞";
        }
        return icon == ICON_UNKNOWN ? "预报待更新" : "降水概率低";
    }
}

static void draw_city(const weather_t *w)
{
    const char *city = w && w->ok ? w->city : "无数据";
    char label[sizeof(w->city) + 4];
    size_t length = strnlen(city, sizeof(w->city));
    memcpy(label, city, length);
    label[length] = '\0';
    if (epd_text_w(&font_mid, label) > 96) {
        int prefix_width = 96 - epd_text_w(&font_mid, "...");
        do {
            do {
                length--;
            } while (length && ((unsigned char)label[length] & 0xc0) == 0x80);
            label[length] = '\0';
        } while (length && epd_text_w(&font_mid, label) > prefix_width);
        memcpy(label + length, "...", 4);
    }
    epd_text(8, 16, &font_mid, label);
}

static bool format_number(char *buf, size_t size, float value, int decimals)
{
    if (!isfinite(value)) {
        return false;
    }
    int n = snprintf(buf, size, "%.*f", decimals, (double)value);
    return n >= 0 && (size_t)n < size;
}

static void sensor_value(int x, int width, float value, bool valid, int decimals, const char *unit, const char *unknown)
{
    char buf[48];
    int n = valid && isfinite(value) ? snprintf(buf, sizeof(buf), "%.*f %s", decimals, (double)value, unit) : -1;
    const char *text = n >= 0 && (size_t)n < sizeof(buf) && epd_text_w(&font_mid, buf) <= width ? buf : unknown;
    epd_text(x, 124, &font_mid, text);
}

static void draw_sensor(const sensor_reading_t *sensor)
{
    bool valid = sensor && sensor->valid;
    epd_text(8, 124, &font_mid, "温度");
    sensor_value(46, 58, valid ? sensor->temperature_c : NAN, valid, 1, "°C", "--.- °C");
    bool humidity_valid =
        valid && isfinite(sensor->humidity_percent) && sensor->humidity_percent >= 0 && sensor->humidity_percent <= 100;
    epd_text(110, 124, &font_mid, "湿度");
    sensor_value(146, 50, humidity_valid ? sensor->humidity_percent : NAN, humidity_valid, 0, "%", "-- %");
}

// Status corner: right of the sensor values, so it never collides with them.
#define STATUS_LEFT  200
#define STATUS_RIGHT (PANEL_H - 6)
#define STATUS_MAX_W (STATUS_RIGHT - STATUS_LEFT)

static const char *status_text(ui_status_t status, unsigned attempt, char *buf, size_t size)
{
    switch (status) {
    case UI_STATUS_CONNECTING:
        return "正在联网";
    case UI_STATUS_FETCHING:
        return "正在取数";
    case UI_STATUS_SYNCING:
        return "同步时间";
    case UI_STATUS_UPDATED:
        return "已更新";
    case UI_STATUS_RETRY:
        snprintf(buf, size, "重试 #%u", attempt);
        return buf;
    default:
        return "";
    }
}

void ui_draw_status(ui_status_t status, unsigned attempt)
{
    char buf[24];
    const char *text = status_text(status, attempt, buf, sizeof(buf));
    epd_fill_rect(STATUS_LEFT, 110, PANEL_H - STATUS_LEFT, EPD_H - 110, EPD_WHITE);
    int width = epd_text_w(&font_mid, text);
    if (*text && width <= STATUS_MAX_W) {
        epd_text(STATUS_RIGHT - width, 124, &font_mid, text);
    }
}

static void draw_header(const weather_t *w, const char *label)
{
    char buf[32];
    draw_city(w);
    text_center(174, 16, &font_mid, label);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(buf, sizeof(buf), "%H:%M", &tm);
    epd_text(EPD_W - 8 - epd_text_w(&font_clock, buf), 16, &font_clock, buf);
    epd_hline(6, 22, EPD_W - 12, EPD_BLACK);
}

// The generated glyph metrics center visible ink within each fixed icon cell.
static void draw_icon(int cx, int code, const aafont *font)
{
    static const char *const glyphs[] = {
        [ICON_SUN] = WI_SUN,
        [ICON_PARTLY_CLOUDY] = WI_PARTLY_CLOUDY,
        [ICON_CLOUD] = WI_CLOUD,
        [ICON_FOG] = WI_FOG,
        [ICON_DRIZZLE] = WI_DRIZZLE,
        [ICON_RAIN] = WI_RAIN,
        [ICON_SNOW] = WI_SNOW,
        [ICON_SHOWERS] = WI_SHOWERS,
        [ICON_SNOW_SHOWERS] = WI_SNOW_SHOWERS,
        [ICON_THUNDER] = WI_THUNDER,
        [ICON_HAIL] = WI_HAIL,
    };
    weather_icon_t icon = icon_for_code(code);
    if (icon == ICON_UNKNOWN) {
        bool compact = font == &font_icon;
        text_center(cx, ICON_TOP + (compact ? 16 : 20), compact ? &font_small : &font_mid, "?");
        return;
    }
    text_center(cx, ICON_TOP + font->line_height, font, glyphs[icon]);
}

static bool date_label(const char date[11], char label[6])
{
    if (date[4] != '-' || date[7] != '-' || date[10] != '\0') {
        return false;
    }
    for (int i = 0; i < 10; i++) {
        if (i != 4 && i != 7 && (date[i] < '0' || date[i] > '9')) {
            return false;
        }
    }
    int month = (date[5] - '0') * 10 + date[6] - '0';
    int day = (date[8] - '0') * 10 + date[9] - '0';
    if (month < 1 || month > 12 || day < 1 || day > 31) {
        return false;
    }
    snprintf(label, 6, "%.2s/%.2s", date + 5, date + 8);
    return true;
}

static void forecast_range(int cx, const weather_day_t *day)
{
    char low[48], high[48], buf[100];
    const char *text = "--";
    if (day && day->tmin <= day->tmax && format_number(low, sizeof(low), day->tmin, 0)
        && format_number(high, sizeof(high), day->tmax, 0))
    {
        // "12°-25°" is 53px at 16px and does not fit the 48px cap, but 47px at 14px: the
        // range row uses font_temp so both values keep their degree sign and the dash.
        snprintf(buf, sizeof(buf), "%s°-%s°", low, high);
        text = buf;
    }
    const aafont *font = &font_temp;
    if (epd_text_w(font, text) > 48) {
        font = &font_small;
    }
    if (epd_text_w(font, text) > 48) {
        text = "--";
        font = &font_temp;
    }
    text_center(cx, TEMP_BASE, font, text);
}

void ui_draw_weather(const weather_t *w, const sensor_reading_t *sensor)
{
    char buf[32];
    bool have_weather = w && w->ok;
    epd_clear(EPD_WHITE);

    draw_header(w, today_hint(w));

    for (int i = 0; i < WEATHER_DAYS; i++) {
        int cx = GRID_X + COL_W / 2 + i * COL_W;
        char date[6];
        bool known_date = have_weather && date_label(w->days[i].date, date);
        // The leftmost column is today: the only CJK label in the row, so it uses font_mid.
        text_center(cx, DATE_BASE, known_date && i == 0 ? &font_mid : &font_small,
                    known_date ? (i == 0 ? "今天" : date) : "--/--");
        draw_icon(cx, have_weather ? w->days[i].code : -1, &font_icon_daily);
        forecast_range(cx, have_weather ? &w->days[i] : NULL);
        int probability = have_weather ? rounded_probability(w->days[i].precip_probability) : -1;
        if (probability >= 0) {
            snprintf(buf, sizeof(buf), "%d%%", probability);
        }
        text_center(cx, PRECIP_BASE, &font_small, probability >= 0 ? buf : "--%");
    }
    epd_hline(6, 108, EPD_W - 12, EPD_BLACK);
    draw_sensor(sensor);
}

void ui_draw_hourly(const weather_t *w, const sensor_reading_t *sensor)
{
    char buf[32];
    bool have_weather = w && w->ok;
    epd_clear(EPD_WHITE);

    draw_header(w, "未来10小时");

    for (int i = 0; i < HOUR_COLS; i++) {
        const weather_hour_t *hour = have_weather ? &w->hours[i * HOUR_STEP] : NULL;
        int cx = HOUR_GRID_X + HOUR_COL_W / 2 + i * HOUR_COL_W;
        if (hour) {
            snprintf(buf, sizeof(buf), "%02d", hour->hour);
        } else {
            strcpy(buf, "--");
        }
        text_center(cx, DATE_BASE, &font_small, buf);
        draw_icon(cx, hour ? hour->code : -1, &font_icon);

        char temp[sizeof buf];
        const char *text = "--";
        const aafont *font = &font_temp;
        if (hour && format_number(temp, sizeof(temp), hour->temperature_c, 0)) {
            // Append the degree sign in place: a plain snprintf("%s°") makes the compiler
            // prove a truncation that cannot happen with a rounded integer.
            size_t digits = strlen(temp);
            if (digits + sizeof("°") <= sizeof(temp)) {
                memcpy(temp + digits, "°", sizeof("°"));
            }
            text = temp;
            if (epd_text_w(font, text) > HOUR_COL_W - 2) {
                font = &font_small;
            }
            if (epd_text_w(font, text) > HOUR_COL_W - 2) {
                text = "--";
                font = &font_temp;
            }
        }
        text_center(cx, TEMP_BASE, font, text);

        int probability = hour ? rounded_probability(hour->precip_probability) : -1;
        if (probability >= 0) {
            snprintf(buf, sizeof(buf), "%d%%", probability);
            // 100% is wider than a 26px column; drop the sign rather than overlap.
            if (epd_text_w(&font_small, buf) > HOUR_COL_W - 2) {
                snprintf(buf, sizeof(buf), "%d", probability);
            }
        }
        text_center(cx, PRECIP_BASE, &font_small, probability >= 0 ? buf : "--%");
    }

    epd_hline(6, 108, EPD_W - 12, EPD_BLACK);
    draw_sensor(sensor);
}

void ui_draw_retry(unsigned attempt, unsigned retry_seconds)
{
    char buf[64];
    epd_fill_rect(0, 0, EPD_W, 22, EPD_WHITE);
    snprintf(buf, sizeof(buf), "无网络 %us后重试 #%u", retry_seconds, attempt);
    if (8 + epd_text_w(&font_mid, buf) > EPD_W) {
        snprintf(buf, sizeof(buf), "无网络 %us重试 #%u", retry_seconds, attempt);
    }
    epd_text(8, 16, &font_mid, buf);
}

void ui_draw_portal_hint(void)
{
    epd_clear(EPD_WHITE);
    char buf[48];
    epd_text(8, 16, &font_mid, "初始化设置");
    epd_hline(6, 22, EPD_W - 12, EPD_BLACK);
    snprintf(buf, sizeof(buf), "WiFi: %s", net_ap_ssid());
    text_center(EPD_W / 2, 56, &font_mid, buf);
    text_center(EPD_W / 2, 96, &font_mid, "使用浏览器打开 " NET_AP_URL);
}
