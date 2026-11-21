#include "epd.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fonts_aa.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *log_tag = "epd";

#define ROW_BYTES (PANEL_W / 8)
#define FB_SIZE   (ROW_BYTES * PANEL_H) // 4736 bytes, 1 = white, 0 = black

#define LUT_BYTES              153
#define FULL_WAVEFORM_BYTES    (LUT_BYTES + 6)
#define PAGE_SWITCH_FULL_EVERY 8

// Waveshare 2.9" V2 (SSD1680) waveform table. Designated initializers keep the
// controller's byte offsets visible; omitted entries are intentionally zero.
static const uint8_t ws_20_30[FULL_WAVEFORM_BYTES] = {
    [0] = 0x80,   [1] = 0x66,   [8] = 0x40,   [12] = 0x10,  [13] = 0x66,  [20] = 0x20,  [24] = 0x80,  [25] = 0x66,
    [32] = 0x40,  [36] = 0x10,  [37] = 0x66,  [44] = 0x20,  [60] = 0x14,  [61] = 0x08,  [66] = 0x01,  [67] = 0x0A,
    [68] = 0x0A,  [70] = 0x0A,  [71] = 0x0A,  [73] = 0x01,  [116] = 0x14, [117] = 0x08, [119] = 0x01, [122] = 0x01,
    [129] = 0x01, [144] = 0x44, [145] = 0x44, [146] = 0x44, [147] = 0x44, [148] = 0x44, [149] = 0x44, [153] = 0x22,
    [154] = 0x17, [155] = 0x41, [157] = 0x32, [158] = 0x36,
};

// Partial updates only use the first 153 bytes, so the unused register tail is omitted.
static const uint8_t ws_partial[LUT_BYTES] = {
    [1] = 0x40,  [12] = 0x80, [13] = 0x80,  [24] = 0x40,  [25] = 0x40,  [37] = 0x80,  [60] = 0x0A,  [66] = 0x02,
    [67] = 0x01, [74] = 0x01, [144] = 0x22, [145] = 0x22, [146] = 0x22, [147] = 0x22, [148] = 0x22, [149] = 0x22,
};

static uint8_t framebuffer[FB_SIZE];
static uint8_t displayed[FB_SIZE];
static spi_device_handle_t panel_spi;
static unsigned page_switches;
static bool displayed_valid;

struct ram_window {
    unsigned x_start, x_end; // byte addresses in a controller row
    unsigned y_start, y_end;
};

static const struct ram_window full_window = {0, ROW_BYTES - 1, 0, PANEL_H - 1};

static void tx(const uint8_t *buf, size_t n)
{
    spi_transaction_t t = {.length = n * 8, .tx_buffer = buf};
    ESP_ERROR_CHECK(spi_device_polling_transmit(panel_spi, &t));
}

static void cmd(uint8_t c)
{
    gpio_set_level(EPD_DC, 0);
    tx(&c, 1);
}

static void data(const uint8_t *d, size_t n)
{
    gpio_set_level(EPD_DC, 1);
    tx(d, n);
}

static void cmd1(uint8_t c, uint8_t d)
{
    cmd(c);
    data(&d, 1);
}

// SSD1680: BUSY high while busy, low when idle.
// Use at least 10 ms (1 tick at CONFIG_FREERTOS_HZ=100) so polling sleeps instead of spinning.
static bool wait_idle(const char *what, uint32_t timeout_ms)
{
    vTaskDelay(pdMS_TO_TICKS(10)); // BUSY needs a moment to assert
    if (gpio_get_level(EPD_BUSY) == 0) {
        return true;
    }
    int64_t t0 = esp_timer_get_time();
    while (gpio_get_level(EPD_BUSY) == 1) {
        if (esp_timer_get_time() - t0 > (int64_t)timeout_ms * 1000) {
            ESP_LOGE(log_tag, "%s: BUSY stuck high after %u ms", what, timeout_ms);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGI(log_tag, "%s: busy %lld ms", what, (esp_timer_get_time() - t0) / 1000);
    return true;
}

static void panel_reset(void)
{
    gpio_set_level(EPD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(EPD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(EPD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    wait_idle("reset", 2000);
}

static void set_window(const struct ram_window *window)
{
    cmd(0x44); // RAM X start/end (in bytes)
    data((const uint8_t[]){window->x_start, window->x_end}, 2);
    cmd(0x45); // RAM Y start/end
    data((const uint8_t[]){window->y_start & 0xFF, window->y_start >> 8, window->y_end & 0xFF, window->y_end >> 8}, 4);
    cmd1(0x4E, window->x_start);
    cmd(0x4F);
    data((const uint8_t[]){window->y_start & 0xFF, window->y_start >> 8}, 2);
}

static void write_frame(uint8_t ram, const struct ram_window *window)
{
    set_window(window);
    cmd(ram);
    size_t width = window->x_end - window->x_start + 1;
    if (width == ROW_BYTES) {
        data(framebuffer + window->y_start * ROW_BYTES, (window->y_end - window->y_start + 1) * width);
        return;
    }
    for (unsigned y = window->y_start; y <= window->y_end; y++) {
        data(framebuffer + y * ROW_BYTES + window->x_start, width);
    }
}

static void full_lut(void)
{
    cmd(0x32); // waveform LUT
    data(ws_20_30, LUT_BYTES);
    wait_idle("lut", 2000);
    cmd1(0x3F, ws_20_30[153]);
    cmd1(0x03, ws_20_30[154]);
    cmd(0x04);
    data(&ws_20_30[155], 3);
    cmd1(0x2C, ws_20_30[158]);
}

// Registers needed by both waveforms. Hardware reset does not clear the visible image,
// but it does drop the gate-line/data-entry setup, so re-apply it every time.
static void panel_config(bool soft_reset)
{
    panel_reset();
    if (soft_reset) {
        cmd(0x12);
        wait_idle("soft reset", 2000);
    }
    cmd(0x01); // driver output control: 296 gate lines (SSD1680 maximum)
    data((const uint8_t[]){0x27, 0x01, 0x00}, 3);
    cmd1(0x11, 0x03); // data entry: x+ y+
    cmd(0x21);        // display update control: normal, no inversion
    data((const uint8_t[]){0x00, 0x80}, 2);
    wait_idle("config", 2000);
}

static bool refresh_full(void)
{
    panel_config(true);
    cmd1(0x3C, 0x05); // border waveform
    full_lut();
    write_frame(0x24, &full_window);
    write_frame(0x26, &full_window);
    cmd1(0x22, 0xC7); // full update sequence
    cmd(0x20);
    return wait_idle("full refresh", 15000);
}

// At boot the glass can retain an image independently of RAM. Seed the previous
// image with the inverse of the target so the partial LUT drives every pixel
// toward its intended color.
static void seed_partial_reference(void)
{
    uint8_t rows[ROW_BYTES * 8];
    set_window(&full_window);
    cmd(0x26);
    for (size_t offset = 0; offset < FB_SIZE; offset += sizeof(rows)) {
        size_t count = FB_SIZE - offset < sizeof(rows) ? FB_SIZE - offset : sizeof(rows);
        for (size_t i = 0; i < count; i++) {
            rows[i] = (uint8_t)~framebuffer[offset + i];
        }
        data(rows, count);
    }
}

static bool refresh_partial(const struct ram_window *window)
{
    panel_config(false);
    cmd1(0x3C, 0x80);
    cmd(0x32);
    data(ws_partial, LUT_BYTES);
    wait_idle("partial lut", 2000);
    cmd(0x37);
    // Disable RAM ping-pong. Both banks must keep the last image outside this window.
    data((const uint8_t[10]){0}, 10);
    cmd1(0x22, 0xC0);
    cmd(0x20);
    if (!wait_idle("partial prepare", 15000)) {
        return false;
    }
    if (!displayed_valid) {
        seed_partial_reference();
    }
    write_frame(0x24, window);
    cmd1(0x22, 0x0F);
    cmd(0x20);
    if (!wait_idle("partial refresh", 15000)) {
        return false;
    }
    write_frame(0x26, window);
    return true;
}

void epd_init(void)
{
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << EPD_RST) | (1ULL << EPD_DC),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&out));
    gpio_config_t in = {
        .pin_bit_mask = 1ULL << EPD_BUSY,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in));

    spi_bus_config_t bus = {
        .mosi_io_num = EPD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = EPD_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = FB_SIZE + 16,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = {
        .clock_speed_hz = 4 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = EPD_CS,
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &panel_spi));

    page_switches = 0;
    displayed_valid = false;
}

void epd_clear(int color)
{
    memset(framebuffer, color == EPD_BLACK ? 0x00 : 0xFF, sizeof(framebuffer));
}

void epd_pixel(int x, int y, int color)
{
    if (x < 0 || x >= PANEL_H || y < 0 || y >= EPD_H) {
        return;
    }
    int px = EPD_H - 1 - y; // logical (x, y) -> panel RAM (px, py)
    int py = x;
    uint8_t m = 0x80 >> (px & 7);
    uint8_t *p = &framebuffer[py * ROW_BYTES + (px >> 3)];
    if (color == EPD_BLACK) {
        *p &= ~m;
    } else {
        *p |= m;
    }
}

void epd_hline(int x, int y, int w, int color)
{
    for (int i = 0; i < w; i++) {
        epd_pixel(x + i, y, color);
    }
}

void epd_fill_rect(int x, int y, int w, int h, int color)
{
    for (int j = 0; j < h; j++) {
        epd_hline(x, y + j, w, color);
    }
}

// Glyphs are already 1bpp and grid-fitted when the font was generated, so printing is a plain bit blit.
// Next codepoint of a UTF-8 string. Unknown/invalid bytes come back as-is and simply miss
// the font's table.
static uint32_t utf8_next(const char **ps)
{
    const uint8_t *s = (const uint8_t *)*ps;
    uint32_t c = *s++;
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    c &= n == 3 ? 0x07 : n == 2 ? 0x0F : n == 1 ? 0x1F : 0x7F;
    for (int i = 0; i < n && (*s & 0xC0) == 0x80; i++) {
        c = (c << 6) | (*s++ & 0x3F);
    }
    *ps = (const char *)s;
    return c;
}

static const aaglyph *glyph_of(const aafont *f, uint32_t c)
{
    for (uint16_t i = 0; i < f->n; i++) {
        if (f->cps[i] == c) {
            return &f->glyphs[i];
        }
    }
    return NULL;
}

void epd_text(int x, int y_base, const aafont *f, const char *s)
{
    while (*s) {
        const aaglyph *g = glyph_of(f, utf8_next(&s));
        if (!g) {
            continue;
        }
        int gx = x + g->xoff, gy = y_base + g->yoff;
        const uint8_t *p = f->data + g->offset;
        int stride = (g->w + 7) / 8;
        for (int row = 0; row < g->h; row++) {
            for (int col = 0; col < g->w; col++) {
                if (p[row * stride + (col >> 3)] & (0x80 >> (col & 7))) {
                    epd_pixel(gx + col, gy + row, EPD_BLACK);
                }
            }
        }
        x += g->adv;
    }
}

int epd_text_w(const aafont *f, const char *s)
{
    int w = 0;
    while (*s) {
        const aaglyph *g = glyph_of(f, utf8_next(&s));
        if (g) {
            w += g->adv;
        }
    }
    return w;
}

void epd_refresh(void)
{
    struct ram_window window = full_window;
    if (displayed_valid) {
        window = (struct ram_window){.x_start = ROW_BYTES, .y_start = PANEL_H};
        for (unsigned y = 0; y < PANEL_H; y++) {
            for (unsigned x = 0; x < ROW_BYTES; x++) {
                size_t offset = y * ROW_BYTES + x;
                if (framebuffer[offset] == displayed[offset]) {
                    continue;
                }
                if (x < window.x_start) {
                    window.x_start = x;
                }
                if (x > window.x_end) {
                    window.x_end = x;
                }
                if (y < window.y_start) {
                    window.y_start = y;
                }
                window.y_end = y;
            }
        }
        if (window.y_start == PANEL_H) {
            return;
        }
    }
    displayed_valid = refresh_partial(&window);
    if (displayed_valid) {
        memcpy(displayed, framebuffer, FB_SIZE);
    }
}

void epd_refresh_page_switch(void)
{
    if (++page_switches < PAGE_SWITCH_FULL_EVERY) {
        epd_refresh();
        return;
    }
    page_switches = 0;
    displayed_valid = refresh_full();
    if (displayed_valid) {
        memcpy(displayed, framebuffer, FB_SIZE);
    }
}
