#include "audio.h"
#include "button.h"
#include "device_mcp.h"
#include "epd.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "net.h"
#include "ui.h"
#include "voice.h"
#include "voice_feedback.h"
#include "weather.h"

static const char *TAG = "app";
static SemaphoreHandle_t s_display_lock;

// Network and SNTP waits run on the main task. The clock owns no application state
// and can update while they block; the mutex covers drawing through SPI/BUSY completion.
static void clock_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        voice_feedback_t feedback = voice_feedback_get();
        xSemaphoreTake(s_display_lock, portMAX_DELAY);
        bool changed = ui_update_voice(&feedback);
        changed = ui_update_clock() || changed;
        if (changed) {
            epd_refresh();
        }
        xSemaphoreGive(s_display_lock);
    }
}

#define BOOT_WAIT_MS       5000 // let the router/DHCP come up before the first attempt
#define CONNECT_TIMEOUT_MS 25000
#define RESET_NOTICE_MS    1000
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
        xSemaphoreTake(s_display_lock, portMAX_DELAY);
        ui_draw_status(status, attempt);
        epd_refresh();
        xSemaphoreGive(s_display_lock);
    }
}

// Routine updates use the driver's partial-refresh cadence. Page switches use their own
// 1..7 partial / 8th full cadence, so they do not advance the ordinary refresh count.
static void present(app_t *app, bool page_switch)
{
    xSemaphoreTake(s_display_lock, portMAX_DELAY);
    draw_page(app);
    if (page_switch) {
        epd_refresh_page_switch();
    } else {
        epd_refresh();
    }
    xSemaphoreGive(s_display_lock);
}

static void show_portal(void)
{
    xSemaphoreTake(s_display_lock, portMAX_DELAY);
    ui_draw_portal_hint();
    epd_refresh_full();
    xSemaphoreGive(s_display_lock);
}

// Returns true when a long hold clears the stored credentials and opens the portal.
static bool button_service(app_t *app, unsigned slice_ms)
{
    switch (button_poll(slice_ms)) {
    case BUTTON_HOLD:
        ESP_LOGW(TAG, "button held: erasing stored credentials");
        voice_net_pause();
        xSemaphoreTake(s_display_lock, portMAX_DELAY);
        ui_draw_resetting();
        epd_refresh();
        xSemaphoreGive(s_display_lock);
        net_forget_creds();
        // Keep the notice readable even if the NVS erase finishes immediately.
        vTaskDelay(pdMS_TO_TICKS(RESET_NOTICE_MS));
        show_portal();
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
        voice_net_pause();
        show_portal();
        net_portal();
        app->connect_attempt = 0;
        return;
    }

    // Keep the title honest while the AP is unreachable; the body stays as placeholder.
    unsigned delay_s = retry_seconds(++app->connect_attempt);
    ESP_LOGW(TAG, "connect failed, retry #%u in %us", app->connect_attempt, delay_s);
    xSemaphoreTake(s_display_lock, portMAX_DELAY);
    ui_draw_retry(app->connect_attempt, delay_s);
    epd_refresh();
    xSemaphoreGive(s_display_lock);
    (void)wait_serviced(delay_s * 1000, app);
}

static void run_weather_phase(app_t *app)
{
    if (!voice_weather_begin()) {
        if (wait_serviced(BUTTON_POLL_MS, app)) {
            app->connect_attempt = 0;
            app->phase = APP_PHASE_CONNECT;
        }
        return;
    }
    set_status(app, UI_STATUS_FETCHING, 0, true);
    bool updated = weather_update(&app->w);
    unsigned wait_s = REFRESH_S;
    if (updated) {
        xSemaphoreTake(s_display_lock, portMAX_DELAY);
        net_apply_tz(app->w.utc_offset);
        xSemaphoreGive(s_display_lock);
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

    device_mcp_publish(app->have_weather ? &app->shown : NULL, &app->sensor, app->retrying, updated);
    voice_weather_end();
    voice_net_ready();
    present(app, false);
    if (wait_serviced(wait_s * 1000u, app)) {
        app->connect_attempt = 0;
        app->phase = APP_PHASE_CONNECT;
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "boot");
    s_display_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_display_lock != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    epd_init();
    esp_err_t audio_err = audio_init();
    if (audio_err != ESP_OK) {
        ESP_LOGW(TAG, "audio unavailable: %s", esp_err_to_name(audio_err));
    }
    net_init();
    button_init();
    // Voice stays idle until the network is up; it runs in its own task so a microphone
    // read or a chat turn never stalls the weather loop.
    if (voice_init() != ESP_OK) {
        ESP_LOGW(TAG, "voice unavailable");
    }

    app_t app = {0};
    app.view = VIEW_DAILY;
    app.status = UI_STATUS_CONNECTING;
    app.phase = APP_PHASE_CONNECT;

    // The glass keeps the last session's image, and the first forecast is seconds to minutes
    // away, so paint the placeholder page now. One full refresh scrubs the old image and leaves
    // something readable; it does not force a full waveform for the later routine page updates.
    xSemaphoreTake(s_display_lock, portMAX_DELAY);
    draw_page(&app);
    epd_refresh_full();
    xSemaphoreGive(s_display_lock);
    ESP_ERROR_CHECK(xTaskCreate(clock_task, "clock", 2048, NULL, 1, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

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
