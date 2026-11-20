#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "epd.h"
#include "esp_timer.h"
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
#define HOUR_COL_W  28
#define HOUR_GRID_X ((PANEL_H - HOUR_COLS * HOUR_COL_W) / 2)

#define CLOCK_LEFT 230

static struct {
    char text[10];
    bool visible;
    bool local_time;
} clock_display;

static struct {
    voice_feedback_t voice;
    bool voice_known;
    ui_status_t application;
    unsigned attempt;
} status_display;

static struct {
    char city[24];
    char label[32];
    char retry_title[64];
    bool retrying;
} weather_header;

static struct {
    bool daily_page;
    char first_date[11];
    bool first_is_today;
    bool stale;
    bool stamp_set;
    forecast_stamp_t stamp;
} forecast_display;

static struct {
    weather_hour_t cache[WEATHER_HOURS];
    bool available;
    bool discontinuous;
    int start;
} hourly_display;

static bool date_label(const char date[11], char label[6]);
static void draw_first_date(void);
static void draw_hourly_cells(void);

_Static_assert(WEATHER_HOURS >= HOUR_COLS, "hourly strip needs 10 hours");

static void text_center(int cx, int y_base, const aafont *f, const char *s)
{
    epd_text(cx - epd_text_w(f, s) / 2, y_base, f, s);
}

enum weather_icon {
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
};

static enum weather_icon icon_for_code(int code)
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
    enum weather_icon icon = icon_for_code(w->days[0].code);
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

static void draw_city(const char *city)
{
    char label[sizeof(weather_header.city) + 4];
    size_t length = strnlen(city, sizeof(weather_header.city));
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

static const char *selected_status(char *buf, size_t size)
{
    if (status_display.voice_known) {
        switch (status_display.voice.state) {
        case VOICE_STATE_DISABLED:
            return "语音不可用";
        case VOICE_STATE_CONNECTING:
            return "语音连接";
        case VOICE_STATE_NEED_SETUP:
            return "设置语音";
        case VOICE_STATE_ACTIVATION:
            return "等待激活";
        case VOICE_STATE_READY:
            if (status_display.application != UI_STATUS_CONNECTING && status_display.application != UI_STATUS_FETCHING
                && status_display.application != UI_STATUS_SYNCING)
            {
                return "待唤醒";
            }
            break;
        case VOICE_STATE_LISTENING:
            return "正在聆听";
        case VOICE_STATE_WAITING:
            return "等待回答";
        case VOICE_STATE_SPEAKING:
            return "正在说话";
        case VOICE_STATE_ERROR:
            return "语音失败";
        default:
            break;
        }
    }
    return status_text(status_display.application, status_display.attempt, buf, size);
}

static void draw_status(void)
{
    char buf[24];
    const char *text = selected_status(buf, sizeof(buf));
    epd_fill_rect(STATUS_LEFT, 110, PANEL_H - STATUS_LEFT, EPD_H - 110, EPD_WHITE);
    int width = epd_text_w(&font_mid, text);
    if (forecast_display.stamp_set && clock_display.local_time && forecast_display.stamp.fetched_at != 0) {
        char updated[6];
        struct tm local;
        if (localtime_r(&forecast_display.stamp.fetched_at, &local)
            && strftime(updated, sizeof(updated), "%H:%M", &local) > 0
            && epd_text_w(&font_small, updated) + width + 4 <= STATUS_MAX_W)
        {
            epd_text(STATUS_LEFT, 124, &font_small, updated);
        }
    }
    if (*text && width <= STATUS_MAX_W) {
        epd_text(STATUS_RIGHT - width, 124, &font_mid, text);
    }
}

void ui_draw_status(ui_status_t status, unsigned attempt)
{
    status_display.application = status;
    status_display.attempt = attempt;
    draw_status();
}

static bool clock_is_known(time_t now)
{
    return now >= NET_MIN_SYNCED_EPOCH && net_time_is_synced();
}

static bool render_clock(time_t now)
{
    char buf[sizeof(clock_display.text)] = "--:--";
    struct tm tm;
    if (clock_is_known(now)) {
        struct tm *converted = clock_display.local_time ? localtime_r(&now, &tm) : gmtime_r(&now, &tm);
        if (!converted || strftime(buf, sizeof(buf), clock_display.local_time ? "%H:%M" : "UTC %H:%M", &tm) == 0) {
            strcpy(buf, "--:--");
        }
    }
    if (strcmp(buf, clock_display.text) == 0) {
        return false;
    }
    epd_fill_rect(CLOCK_LEFT, 0, PANEL_H - CLOCK_LEFT, 22, EPD_WHITE);
    epd_text(EPD_W - 8 - epd_text_w(&font_clock, buf), 16, &font_clock, buf);
    memcpy(clock_display.text, buf, sizeof(clock_display.text));
    return true;
}

static void update_forecast_date(time_t now)
{
    forecast_display.first_is_today = false;
    forecast_display.stale =
        forecast_display.stamp_set && forecast_is_stale(forecast_display.stamp, esp_timer_get_time());
    struct tm local;
    char today[11];
    if (clock_display.local_time && forecast_display.first_date[0] && clock_is_known(now) && localtime_r(&now, &local)
        && strftime(today, sizeof(today), "%Y-%m-%d", &local) > 0)
    {
        int relation = strcmp(today, forecast_display.first_date);
        forecast_display.first_is_today = relation == 0;
        forecast_display.stale = forecast_display.stale || relation > 0;
    }
}

static void render_header(void)
{
    epd_fill_rect(0, 0, EPD_W, 22, EPD_WHITE);
    if (status_display.voice_known && status_display.voice.state == VOICE_STATE_ACTIVATION) {
        epd_text(8, 16, &font_mid, "激活");
        const aafont *font = &font_mid;
        if (epd_text_w(font, status_display.voice.activation_code) > CLOCK_LEFT - 52) {
            font = &font_small;
        }
        epd_text(52, 16, font, status_display.voice.activation_code);
    } else if (weather_header.retrying) {
        epd_text(8, 16, &font_mid, weather_header.retry_title);
    } else {
        draw_city(weather_header.city);
        text_center(174, 16, &font_mid,
                    (forecast_display.stale || (forecast_display.daily_page && !forecast_display.first_is_today))
                        ? "预报待更新"
                        : weather_header.label);
    }
    clock_display.visible = true;
    clock_display.text[0] = '\0';
    (void)render_clock(time(NULL));
}

static void draw_header(const weather_t *w, const char *label)
{
    const char *city = w && w->ok ? w->city : "无数据";
    snprintf(weather_header.city, sizeof(weather_header.city), "%.*s", (int)sizeof(weather_header.city) - 1, city);
    snprintf(weather_header.label, sizeof(weather_header.label), "%s", label);
    clock_display.local_time =
        w && w->ok && w->utc_offset >= -14 * 3600 && w->utc_offset <= 14 * 3600 && w->utc_offset % 60 == 0;
    char label_date[6];
    if (w && w->ok && date_label(w->days[0].date, label_date)) {
        memcpy(forecast_display.first_date, w->days[0].date, sizeof(forecast_display.first_date));
    } else {
        forecast_display.first_date[0] = '\0';
    }
    update_forecast_date(time(NULL));
    weather_header.retrying = false;
    render_header();
    epd_hline(6, 22, EPD_W - 12, EPD_BLACK);
}

void ui_set_forecast_stamp(const forecast_stamp_t *stamp)
{
    forecast_display.stamp_set = stamp != NULL;
    if (forecast_display.stamp_set) {
        forecast_display.stamp = *stamp;
    }
}

static int date_key(const struct tm *local)
{
    return (local->tm_year + 1900) * 10000 + (local->tm_mon + 1) * 100 + local->tm_mday;
}

static int select_hourly_start(time_t now)
{
    struct tm local;
    if (!hourly_display.available || !clock_display.local_time || !clock_is_known(now)
        || hourly_display.cache[0].date == 0 || !localtime_r(&now, &local))
    {
        return 0;
    }
    int date = date_key(&local);
    int64_t current = (int64_t)date * 24 + local.tm_hour;
    // Keep repeated or skipped DST hours in API order when the fetch timestamp
    // anchors the first row. The device only has the last reported UTC offset.
    if (hourly_display.discontinuous && forecast_display.stamp_set && forecast_display.stamp.fetched_at) {
        struct tm fetched;
        if (localtime_r(&forecast_display.stamp.fetched_at, &fetched)
            && date_key(&fetched) == hourly_display.cache[0].date && fetched.tm_hour == hourly_display.cache[0].hour)
        {
            double hours =
                (difftime(now, forecast_display.stamp.fetched_at) + fetched.tm_min * 60 + fetched.tm_sec) / 3600;
            return hours <= 0 ? 0 : hours >= WEATHER_HOURS ? WEATHER_HOURS : (int)hours;
        }
    }
    for (int i = 0; i < WEATHER_HOURS; i++) {
        int64_t hour = (int64_t)hourly_display.cache[i].date * 24 + hourly_display.cache[i].hour;
        if (hour >= current) {
            return i;
        }
    }
    return WEATHER_HOURS;
}

bool ui_update_clock(void)
{
    if (!clock_display.visible) {
        return false;
    }
    time_t now = time(NULL);
    bool was_today = forecast_display.first_is_today;
    bool was_stale = forecast_display.stale;
    update_forecast_date(now);
    bool date_changed = forecast_display.daily_page && was_today != forecast_display.first_is_today;
    bool hours_changed = false;
    if (!forecast_display.daily_page) {
        int start = select_hourly_start(now);
        if (start != hourly_display.start) {
            hourly_display.start = start;
            epd_fill_rect(0, 23, PANEL_H, 108 - 23, EPD_WHITE);
            draw_hourly_cells();
            hours_changed = true;
        }
    }
    if (date_changed) {
        epd_fill_rect(GRID_X, 23, COL_W, ICON_TOP - 23, EPD_WHITE);
        draw_first_date();
    }
    if (date_changed || was_stale != forecast_display.stale) {
        if (!weather_header.retrying
            && !(status_display.voice_known && status_display.voice.state == VOICE_STATE_ACTIVATION))
        {
            render_header();
            return true;
        }
    }
    return render_clock(now) || date_changed || hours_changed;
}

bool ui_update_voice(const voice_feedback_t *feedback)
{
    if (feedback == NULL) {
        return false;
    }
    char old_status[24], buf[24];
    snprintf(old_status, sizeof(old_status), "%s", selected_status(buf, sizeof(buf)));
    bool was_activation = status_display.voice_known && status_display.voice.state == VOICE_STATE_ACTIVATION;
    bool header_changed =
        was_activation != (feedback->state == VOICE_STATE_ACTIVATION)
        || (was_activation && strcmp(status_display.voice.activation_code, feedback->activation_code) != 0);
    status_display.voice = *feedback;
    status_display.voice_known = true;
    // Keep the snapshot current while the reset or setup screen is visible.
    if (!clock_display.visible) {
        return false;
    }
    bool status_changed = strcmp(old_status, selected_status(buf, sizeof(buf))) != 0;
    if (header_changed) {
        render_header();
    }
    if (status_changed) {
        draw_status();
    }
    return header_changed || status_changed;
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
    enum weather_icon icon = icon_for_code(code);
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

static void draw_first_date(void)
{
    char date[6];
    const char *label = date_label(forecast_display.first_date, date) ? date : "--/--";
    text_center(GRID_X + COL_W / 2, DATE_BASE, forecast_display.first_is_today ? &font_mid : &font_small,
                forecast_display.first_is_today ? "今天" : label);
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

    forecast_display.daily_page = true;
    draw_header(w, today_hint(w));
    draw_first_date();

    for (int i = 0; i < WEATHER_DAYS; i++) {
        int cx = GRID_X + COL_W / 2 + i * COL_W;
        char date[6];
        if (i > 0) {
            bool known_date = have_weather && date_label(w->days[i].date, date);
            text_center(cx, DATE_BASE, &font_small, known_date ? date : "--/--");
        }
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

static void draw_hourly_cells(void)
{
    char buf[32];
    for (int i = 0; i < HOUR_COLS; i++) {
        int index = hourly_display.start + i;
        const weather_hour_t *hour =
            hourly_display.available && index < WEATHER_HOURS ? &hourly_display.cache[index] : NULL;
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
}

void ui_draw_hourly(const weather_t *w, const sensor_reading_t *sensor)
{
    epd_clear(EPD_WHITE);
    forecast_display.daily_page = false;
    hourly_display.available = w && w->ok;
    hourly_display.discontinuous = false;
    if (hourly_display.available) {
        memcpy(hourly_display.cache, w->hours, sizeof(hourly_display.cache));
        for (int i = 1; i < WEATHER_HOURS; i++) {
            const weather_hour_t *before = &hourly_display.cache[i - 1], *after = &hourly_display.cache[i];
            if (before->date == after->date ? after->hour != before->hour + 1 : before->hour != 23 || after->hour != 0)
            {
                hourly_display.discontinuous = true;
                break;
            }
        }
    }
    draw_header(w, "未来10小时");
    hourly_display.start = select_hourly_start(time(NULL));
    draw_hourly_cells();
    epd_hline(6, 108, EPD_W - 12, EPD_BLACK);
    draw_sensor(sensor);
}

void ui_draw_retry(unsigned attempt, unsigned retry_seconds)
{
    snprintf(weather_header.retry_title, sizeof(weather_header.retry_title), "无网络 %us后重试 #%u", retry_seconds,
             attempt);
    if (8 + epd_text_w(&font_mid, weather_header.retry_title) > CLOCK_LEFT) {
        snprintf(weather_header.retry_title, sizeof(weather_header.retry_title), "无网络 %us重试 #%u", retry_seconds,
                 attempt);
    }
    if (8 + epd_text_w(&font_mid, weather_header.retry_title) > CLOCK_LEFT) {
        snprintf(weather_header.retry_title, sizeof(weather_header.retry_title), "无网络 #%u", attempt);
    }
    weather_header.retrying = true;
    render_header();
}

void ui_draw_resetting(void)
{
    clock_display.visible = false;
    epd_clear(EPD_WHITE);
    text_center(PANEL_H / 2, PANEL_W / 2 + 6, &font_mid, "WiFi 重置中");
}

void ui_draw_portal_hint(void)
{
    clock_display.visible = false;
    epd_clear(EPD_WHITE);
    char buf[48];
    epd_text(8, 16, &font_mid, "初始化设置");
    epd_hline(6, 22, EPD_W - 12, EPD_BLACK);
    snprintf(buf, sizeof(buf), "WiFi: %s", net_ap_ssid());
    text_center(EPD_W / 2, 56, &font_mid, buf);
    text_center(EPD_W / 2, 96, &font_mid, "使用浏览器打开 " NET_AP_URL);
}
