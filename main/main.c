#include "button.h"
#include "epd.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "ui.h"
#include "weather.h"

static const char *TAG = "app";

#define BOOT_WAIT_MS       5000 // let the router/DHCP come up before the first attempt
#define CONNECT_TIMEOUT_MS 25000
#define REFRESH_S          (30 * 60)
#define RETRY_MIN_S        5
#define RETRY_MAX_S        60

typedef enum { VIEW_DAILY, VIEW_HOURLY } view_t;
typedef enum { APP_PHASE_CONNECT, APP_PHASE_WEATHER } app_phase_t;

// Everything the button handlers need to repaint the current page.
typedef struct {
    weather_t w;     // result of the last update; w.ok is false before the first success
    weather_t shown; // last good forecast, kept because weather_update resets w on failure
    sensor_reading_t sensor;
    bool have_weather;
    bool painted; // a real forecast has been drawn at least once
    bool retrying;
    unsigned retry_attempt;
    unsigned retry_delay;
    ui_status_t status;
    unsigned status_attempt;
    view_t view;
    app_phase_t phase;
    bool boot_wait_done;
    unsigned connect_attempt;
} app_t;

// Stepped retry interval: 5, 10, 20, 40, 60, 60, ... seconds.
static unsigned retry_seconds(unsigned failures)
{
    unsigned s = RETRY_MIN_S;
    while (failures > 1 && s < RETRY_MAX_S) {
        s *= 2;
        failures--;
    }
    return s > RETRY_MAX_S ? RETRY_MAX_S : s;
}

static const weather_t *current_weather(const app_t *app)
{
    return app->have_weather ? &app->shown : &app->w;
}

static void draw_page(const app_t *app)
{
    const weather_t *w = current_weather(app);
    if (app->view == VIEW_HOURLY) {
        ui_draw_hourly(w, &app->sensor);
    } else {
        ui_draw_weather(w, &app->sensor);
    }
    if (app->retrying) {
        ui_draw_retry(app->retry_attempt, app->retry_delay);
    }
    ui_draw_status(app->status, app->status_attempt);
}

// Show the current action before a blocking step, so the footer says what the device is
// waiting for instead of looking stuck.
static void set_status(app_t *app, ui_status_t status, unsigned attempt, bool show_now)
{
    app->status = status;
    app->status_attempt = attempt;
    if (show_now) {
        ui_draw_status(status, attempt);
        epd_refresh();
    }
}

// A page switch replaces the whole screen, so it needs the full waveform to remove the
// previous page's glyphs. Routine updates use the periodic full-refresh policy in epd.c.
static void present(app_t *app, bool force_full)
{
    draw_page(app);
    const weather_t *w = current_weather(app);
    bool full = force_full || (w->ok && !app->painted);
    app->painted = app->painted || w->ok;
    if (full) {
        epd_refresh_full();
    } else {
        epd_refresh();
    }
}

// A long hold erases the stored credentials and goes straight to the portal page, so a
// wrong or missing AP can be fixed without a cable. Returns true when that happened.
static bool button_service(app_t *app, unsigned slice_ms)
{
    switch (button_poll(slice_ms)) {
    case BUTTON_HOLD:
        ESP_LOGW(TAG, "button held: erasing stored credentials");
        net_forget_creds();
        ui_draw_portal_hint();
        epd_refresh_full();
        app->painted = false;
        net_portal();
        return true;

    case BUTTON_TAP:
        app->view = app->view == VIEW_DAILY ? VIEW_HOURLY : VIEW_DAILY;
        present(app, true);
        return false;

    default:
        return false;
    }
}

// Sleeps ms in slices so a hold is serviced while waiting for the network. Returns true on
// credential reset.
static bool wait_serviced(unsigned ms, app_t *app)
{
    unsigned remaining = ms;
    while (remaining) {
        unsigned slice = remaining < BUTTON_POLL_MS ? remaining : BUTTON_POLL_MS;
        vTaskDelay(pdMS_TO_TICKS(slice));
        remaining -= slice;
        if (button_service(app, slice)) {
            return true;
        }
    }
    return false;
}

static void run_connect_phase(app_t *app)
{
    if (!app->boot_wait_done) {
        app->boot_wait_done = true;
        if (wait_serviced(BOOT_WAIT_MS, app)) {
            return;
        }
    }

    if (net_try_stored(CONNECT_TIMEOUT_MS)) {
        ESP_LOGI(TAG, "network ready");
        app->connect_attempt = 0;
        app->phase = APP_PHASE_WEATHER;
        return;
    }

    if (!net_has_stored()) {
        ui_draw_portal_hint();
        epd_refresh_full();
        net_portal();
        app->connect_attempt = 0;
        return;
    }

    // Keep the title honest while the AP is unreachable; the body stays as placeholder.
    unsigned delay_s = retry_seconds(++app->connect_attempt);
    ESP_LOGW(TAG, "connect failed, retry #%u in %us", app->connect_attempt, delay_s);
    ui_draw_retry(app->connect_attempt, delay_s);
    epd_refresh();
    (void)wait_serviced(delay_s * 1000, app);
}

static void run_weather_phase(app_t *app)
{
    set_status(app, UI_STATUS_FETCHING, 0, true);
    bool updated = weather_update(&app->w);
    unsigned wait_s = REFRESH_S;
    if (updated) {
        net_apply_tz(app->w.utc_offset);
        if (!app->have_weather) {
            set_status(app, UI_STATUS_SYNCING, 0, true);
            net_time_sync();
        }
        app->have_weather = true;
        app->retrying = false;
        app->retry_attempt = 0;
        app->retry_delay = 0;
        app->shown = app->w;
        set_status(app, UI_STATUS_UPDATED, 0, false);
    } else {
        app->retrying = true;
        app->retry_delay = retry_seconds(++app->retry_attempt);
        wait_s = app->retry_delay;
        set_status(app, UI_STATUS_RETRY, app->retry_attempt, false);
        ESP_LOGW(TAG, "update failed, retry #%u in %us", app->retry_attempt, app->retry_delay);
    }

    present(app, false);
    if (wait_serviced(wait_s * 1000u, app)) {
        app->connect_attempt = 0;
        app->phase = APP_PHASE_CONNECT;
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "boot");
    epd_init();
    net_init();
    button_init();

    app_t app = {0};
    app.view = VIEW_DAILY;
    app.status = UI_STATUS_CONNECTING;
    app.phase = APP_PHASE_CONNECT;

    // The glass keeps the last session's image, and the first forecast is seconds to minutes
    // away, so paint the placeholder page now. One full refresh both scrubs the old image and
    // leaves something readable; keeping the periodic counter untouched makes the first real
    // forecast a full refresh too.
    draw_page(&app);
    epd_refresh_full();

    for (;;) {
        switch (app.phase) {
        case APP_PHASE_CONNECT:
            run_connect_phase(&app);
            break;
        case APP_PHASE_WEATHER:
            run_weather_phase(&app);
            break;
        }
    }
}
