#include "audio.h"
#include "button.h"
#include "button_gpio.h"
#include "device_mcp.h"
#include "driver/gpio.h"
#include "epd.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "iot_button.h"
#include "net.h"
#include "net_portal.h"
#include "net_time.h"
#include "sdkconfig.h"
#include "sensor.h"
#include "ui.h"
#include "voice.h"
#include "voice_feedback.h"
#include "weather.h"

#if CONFIG_SPIRAM_MODE_OCT
_Static_assert(VOICE_LED_GPIO < 33 || VOICE_LED_GPIO > 37,
               "VOICE_LED_GPIO overlaps GPIO33-37 reserved for octal PSRAM");
#endif
_Static_assert(VOICE_LED_GPIO != 19 && VOICE_LED_GPIO != 20, "VOICE_LED_GPIO overlaps GPIO19/20 reserved for USB-JTAG");
_Static_assert(VOICE_LED_GPIO != BUTTON_GPIO && VOICE_LED_GPIO != VOICE_BUTTON_GPIO
                   && VOICE_LED_GPIO != SENSOR_I2C_SDA_GPIO && VOICE_LED_GPIO != SENSOR_I2C_SCL_GPIO
                   && VOICE_LED_GPIO != AUDIO_PA_EN_GPIO && VOICE_LED_GPIO != AUDIO_I2S_DOUT_GPIO
                   && VOICE_LED_GPIO != AUDIO_I2S_WS_GPIO && VOICE_LED_GPIO != AUDIO_I2S_DIN_GPIO
                   && VOICE_LED_GPIO != AUDIO_I2S_BCLK_GPIO && VOICE_LED_GPIO != AUDIO_I2S_MCLK_GPIO
                   && VOICE_LED_GPIO != AUDIO_I2C_SCL_GPIO && VOICE_LED_GPIO != AUDIO_I2C_SDA_GPIO
                   && VOICE_LED_GPIO != EPD_SCK && VOICE_LED_GPIO != EPD_MOSI && VOICE_LED_GPIO != EPD_RST
                   && VOICE_LED_GPIO != EPD_DC && VOICE_LED_GPIO != EPD_CS && VOICE_LED_GPIO != EPD_BUSY,
               "VOICE_LED_GPIO overlaps another peripheral pin");

#define BOOT_WAIT_MS       5000 // let the router/DHCP come up before the first attempt
#define CONNECT_TIMEOUT_MS 25000
#define RESET_NOTICE_MS    1000
#define REFRESH_S          (30 * 60)
#define RETRY_MIN_S        5
#define RETRY_MAX_S        60
#define SENSOR_POLL_US     (10LL * 60 * 1000000)

enum app_view {
    VIEW_DAILY,
    VIEW_HOURLY
};

enum app_phase {
    APP_PHASE_CONNECT,
    APP_PHASE_WEATHER
};

// Everything the button handlers need to repaint the current page.
struct app_state {
    weather_t        w;     // result of the last update; w.ok is false before the first success
    weather_t        shown; // last good forecast, kept because weather_update resets w
                            // on failure
    forecast_stamp_t stamp;
    sensor_reading_t sensor;
    bool             have_weather;
    bool             retrying;
    unsigned         retry_attempt;
    unsigned         retry_delay;
    ui_status_t      status;
    unsigned         status_attempt;
    enum app_view    view;
    enum app_phase   phase;
    bool             boot_wait_done;
    unsigned         connect_attempt;
    bool             network_busy;
    bool             deferred_hold;
};

static const char       *log_tag = "app";
static SemaphoreHandle_t display_lock;
static QueueHandle_t     button_event_queue;
static TaskHandle_t      clock_task_handle;

static void
notify_clock_task(void)
{
    TaskHandle_t handle = clock_task_handle;
    if (handle != NULL) {
        xTaskNotifyGive(handle);
    }
}

static bool
voice_state_is_awake(voice_state_t state)
{
    return state == VOICE_STATE_CONNECTING || state == VOICE_STATE_LISTENING || state == VOICE_STATE_FOLLOWUP
           || state == VOICE_STATE_WAITING || state == VOICE_STATE_SPEAKING;
}

static void
sync_voice_led(void)
{
    bool on = voice_state_is_awake(voice_feedback_get().state);
    gpio_set_level(VOICE_LED_GPIO, on ? VOICE_LED_ACTIVE_LEVEL : !VOICE_LED_ACTIVE_LEVEL);
}

static void
on_voice_feedback(void)
{
    sync_voice_led();
    notify_clock_task();
}

static void
init_voice_led(void)
{
    const gpio_config_t led_cfg = {
        .pin_bit_mask = 1ULL << VOICE_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&led_cfg));
    ESP_ERROR_CHECK(gpio_set_level(VOICE_LED_GPIO, !VOICE_LED_ACTIVE_LEVEL));
}

enum app_button {
    APP_BUTTON_TAP,
    APP_BUTTON_HOLD,
};

static void
enqueue_button(enum app_button event)
{
    if (event == APP_BUTTON_HOLD) {
        // Reset takes precedence over page changes that have not been drawn
        // yet.
        xQueueReset(button_event_queue);
    }
    xQueueSend(button_event_queue, &event, 0);
    notify_clock_task();
}

static void
on_button_tap(void *handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    enqueue_button(APP_BUTTON_TAP);
}

static void
on_voice_button_tap(void *handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    voice_toggle_chat();
}

static void
on_button_hold(void *handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    enqueue_button(APP_BUTTON_HOLD);
}

// Stepped retry interval: 5, 10, 20, 40, 60, 60, ... seconds.
static unsigned
retry_seconds(unsigned failures)
{
    unsigned s = RETRY_MIN_S;
    while (failures > 1 && s < RETRY_MAX_S) {
        s *= 2;
        failures--;
    }
    return s > RETRY_MAX_S ? RETRY_MAX_S : s;
}

static const weather_t *
current_weather(const struct app_state *app)
{
    return (app->have_weather || app->shown.tz_known) ? &app->shown : NULL;
}

static void
draw_page(const struct app_state *app)
{
    const weather_t *w = current_weather(app);
    ui_set_forecast_stamp(app->have_weather ? &app->stamp : NULL);
    if (app->view == VIEW_HOURLY) {
        ui_draw_hourly(w, &app->sensor);
    } else {
        ui_draw_weather(w, &app->sensor);
    }
    if (app->retrying) {
        ui_draw_retry(app->retry_attempt, app->retry_delay);
    }
    ui_draw_status(app->status, app->status_attempt);
    const char *page =
        app->have_weather ? (app->view == VIEW_HOURLY ? "hourly" : "daily") : (app->retrying ? "retry" : "placeholder");
    device_mcp_publish_display_page(page);
}

// Show the current action before a blocking step, so the footer says what the
// device is waiting for instead of looking stuck.
static void
set_status(struct app_state *app, ui_status_t status, unsigned attempt, bool show_now)
{
    xSemaphoreTake(display_lock, portMAX_DELAY);
    app->status = status;
    app->status_attempt = attempt;
    if (show_now) {
        ui_draw_status(status, attempt);
        epd_refresh();
    }
    xSemaphoreGive(display_lock);
}

static void
switch_page(struct app_state *app)
{
    app->view = app->view == VIEW_DAILY ? VIEW_HOURLY : VIEW_DAILY;
    draw_page(app);
    epd_refresh_page_switch();
}

static void
present(const struct app_state *app)
{
    xSemaphoreTake(display_lock, portMAX_DELAY);
    draw_page(app);
    epd_refresh();
    xSemaphoreGive(display_lock);
}

static void
set_network_busy(struct app_state *app, bool busy)
{
    xSemaphoreTake(display_lock, portMAX_DELAY);
    app->network_busy = busy;
    xSemaphoreGive(display_lock);
}

// While the main task blocks on network I/O, its display data stays stable.
// The clock can use that snapshot for taps. WiFi resets still belong to main.
static void
clock_task(void *arg)
{
    struct app_state *app = arg;
    int64_t           last_sensor_us = esp_timer_get_time();

    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));

        sensor_reading_t sampled = {0};
        bool             sensor_polled = false;
        int64_t          now_us = esp_timer_get_time();
        if (now_us - last_sensor_us >= SENSOR_POLL_US) {
            last_sensor_us = now_us;
            (void)sensor_read(&sampled);
            device_mcp_publish_sensor(&sampled);
            sensor_polled = true;
        }

        xSemaphoreTake(display_lock, portMAX_DELAY);

        voice_feedback_t feedback = voice_feedback_get();
        bool             status_changed = false;
        if (app->status == UI_STATUS_SYNCING && net_time_is_synced()) {
            app->status = UI_STATUS_UPDATED;
            app->status_attempt = 0;
            status_changed = ui_update_status(app->status, app->status_attempt);
        }

        bool sensor_changed = false;
        if (sensor_polled) {
            app->sensor = sampled;
            sensor_changed = ui_update_sensor(&app->sensor);
        }

        bool            voice_changed = ui_update_voice(&feedback);
        bool            clock_changed = ui_update_clock();
        bool            switched = false;
        enum app_button event;
        if (app->network_busy && !app->deferred_hold && xQueueReceive(button_event_queue, &event, 0) == pdTRUE) {
            if (event == APP_BUTTON_HOLD) {
                app->deferred_hold = true;
            } else {
                switch_page(app);
                switched = true;
            }
        }

        if (!switched && (clock_changed || voice_changed || status_changed || sensor_changed)) {
            epd_refresh();
        }

        xSemaphoreGive(display_lock);
    }
}

static void
show_portal(void)
{
    xSemaphoreTake(display_lock, portMAX_DELAY);
    ui_draw_portal_hint();
    device_mcp_publish_display_page("portal");
    epd_refresh();
    xSemaphoreGive(display_lock);
}

// Returns true when a long hold clears the stored credentials and opens the
// portal.
static bool
handle_button_event(struct app_state *app, enum app_button event)
{
    switch (event) {
    case APP_BUTTON_HOLD:
        ESP_LOGW(log_tag, "button held: erasing stored credentials");

        voice_net_pause();
        xSemaphoreTake(display_lock, portMAX_DELAY);
        app->status = UI_STATUS_CONNECTING;
        app->status_attempt = 0;
        ui_draw_resetting();
        device_mcp_publish_display_page("resetting");
        epd_refresh();
        xSemaphoreGive(display_lock);
        net_forget_creds();
        // Keep the notice readable even if the NVS erase finishes immediately.
        vTaskDelay(pdMS_TO_TICKS(RESET_NOTICE_MS));
        show_portal();
        net_portal();
        xQueueReset(button_event_queue);
        return true;

    case APP_BUTTON_TAP:
        xSemaphoreTake(display_lock, portMAX_DELAY);
        switch_page(app);
        xSemaphoreGive(display_lock);
        return false;

    default:
        return false;
    }
}

static bool
button_service(struct app_state *app)
{
    enum app_button event;

    if (app->deferred_hold) {
        app->deferred_hold = false;
        event = APP_BUTTON_HOLD;
    } else if (xQueueReceive(button_event_queue, &event, 0) != pdTRUE) {
        return false;
    }

    return handle_button_event(app, event);
}

// Blocks on the button queue until ms elapses or a hold resets stored
// credentials.
static bool
wait_serviced(unsigned ms, struct app_state *app)
{
    enum app_button event;

    if (app->deferred_hold && button_service(app)) {
        return true;
    }

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ms);

    for (;;) {
        TickType_t now = xTaskGetTickCount();
        TickType_t remaining = (int32_t)(deadline - now) > 0 ? (deadline - now) : 0;

        if (xQueueReceive(button_event_queue, &event, remaining) != pdTRUE) {
            return false;
        }

        if (handle_button_event(app, event)) {
            return true;
        }
    }
}

static void
run_connect_phase(struct app_state *app)
{
    if (net_has_stored()) {
        voice_enable_wake();
    }

    if (!app->boot_wait_done) {
        app->boot_wait_done = true;
        if (wait_serviced(BOOT_WAIT_MS, app)) {
            return;
        }
    }

    if (app->connect_attempt > 0) {
        set_status(app, UI_STATUS_CONNECTING, 0, true);
    }

    set_network_busy(app, true);
    bool connected = net_try_stored(CONNECT_TIMEOUT_MS);
    set_network_busy(app, false);

    if (app->deferred_hold && button_service(app)) {
        app->connect_attempt = 0;
        return;
    }

    if (connected) {
        ESP_LOGI(log_tag, "network ready");
        app->connect_attempt = 0;
        app->phase = APP_PHASE_WEATHER;
        return;
    }

    if (!net_has_stored()) {
        voice_net_pause();
        set_status(app, UI_STATUS_CONNECTING, 0, false);
        show_portal();
        net_portal();
        xQueueReset(button_event_queue);
        app->connect_attempt = 0;
        return;
    }

    // Keep the title and status corner honest while the AP is unreachable; the
    // body stays as placeholder.
    unsigned delay_s = retry_seconds(++app->connect_attempt);
    ESP_LOGW(log_tag, "connect failed, retry #%u in %us", app->connect_attempt, delay_s);
    xSemaphoreTake(display_lock, portMAX_DELAY);
    app->status = UI_STATUS_RETRY;
    app->status_attempt = app->connect_attempt;
    ui_draw_retry(app->connect_attempt, delay_s);
    device_mcp_publish_display_page("retry");
    ui_draw_status(app->status, app->status_attempt);
    epd_refresh();
    xSemaphoreGive(display_lock);
    (void)wait_serviced(delay_s * 1000, app);
}

// ES8311 sometimes NACKs I2C at boot. Retrying only 150 ms later leaves voice
// dead for the whole session, so try again once the network is up and keep
// trying on later updates.
static void
start_voice(void)
{
    static bool started;

    if (started || audio_init() != ESP_OK || voice_init() != ESP_OK) {
        return;
    }

    started = true;
}

static void
run_weather_phase(struct app_state *app)
{
    if (!voice_weather_begin()) {
        if (wait_serviced(50, app)) {
            app->connect_attempt = 0;
            app->phase = APP_PHASE_CONNECT;
        }
        return;
    }

    set_status(app, UI_STATUS_FETCHING, 0, true);
    set_network_busy(app, true);
    forecast_stamp_t fetched;
    bool             updated = weather_update(&app->w, &fetched);
    set_network_busy(app, false);

    if (app->deferred_hold) {
        voice_weather_end();
        (void)button_service(app);
        app->connect_attempt = 0;
        app->phase = APP_PHASE_CONNECT;
        return;
    }

    unsigned wait_s = REFRESH_S;
    if (updated) {
        xSemaphoreTake(display_lock, portMAX_DELAY);
        net_apply_tz(app->w.utc_offset);
        xSemaphoreGive(display_lock);
        app->have_weather = true;
        app->retrying = false;
        app->retry_attempt = 0;
        app->retry_delay = 0;
        app->shown = app->w;
        set_status(app, net_time_is_synced() ? UI_STATUS_UPDATED : UI_STATUS_SYNCING, 0, false);
    } else {
        if (!app->have_weather && app->w.tz_known) {
            xSemaphoreTake(display_lock, portMAX_DELAY);
            net_apply_tz(app->w.utc_offset);
            app->shown.utc_offset = app->w.utc_offset;
            app->shown.tz_known = true;
            xSemaphoreGive(display_lock);
        }

        app->retrying = true;
        app->retry_delay = retry_seconds(++app->retry_attempt);
        wait_s = app->retry_delay;
        set_status(app, UI_STATUS_RETRY, app->retry_attempt, false);
        ESP_LOGW(log_tag, "update failed, retry #%u in %us", app->retry_attempt, app->retry_delay);
    }

    app->stamp = device_mcp_publish_forecast(current_weather(app), updated ? &fetched : NULL, app->retrying);

    voice_weather_end();
    start_voice();
    voice_net_ready();
    present(app);

    if (wait_serviced(wait_s * 1000u, app)) {
        app->connect_attempt = 0;
        app->phase = APP_PHASE_CONNECT;
    }
}

// A panic or watchdog reset otherwise looks the same as a power cycle on the
// panel.
static const char *
reset_reason_name(esp_reset_reason_t reason)
{
    static const char *const names[] = {
        [ESP_RST_UNKNOWN] = "unknown",
        [ESP_RST_POWERON] = "poweron",
        [ESP_RST_EXT] = "external",
        [ESP_RST_SW] = "software",
        [ESP_RST_PANIC] = "panic",
        [ESP_RST_INT_WDT] = "int_wdt",
        [ESP_RST_TASK_WDT] = "task_wdt",
        [ESP_RST_WDT] = "wdt",
        [ESP_RST_DEEPSLEEP] = "deepsleep",
        [ESP_RST_BROWNOUT] = "brownout",
        [ESP_RST_SDIO] = "sdio",
        [ESP_RST_USB] = "usb",
        [ESP_RST_JTAG] = "jtag",
        [ESP_RST_EFUSE] = "efuse",
        [ESP_RST_PWR_GLITCH] = "pwr_glitch",
        [ESP_RST_CPU_LOCKUP] = "cpu_lockup",
    };
    return (size_t)reason < sizeof(names) / sizeof(names[0]) && names[reason] ? names[reason] : "?";
}

void
app_main(void)
{
    ESP_LOGI(log_tag, "boot: reset %s, free heap %u", reset_reason_name(esp_reset_reason()),
             (unsigned)esp_get_free_heap_size());

    display_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(display_lock != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    button_event_queue = xQueueCreate(8, sizeof(enum app_button));
    ESP_ERROR_CHECK(button_event_queue != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    init_voice_led();
    epd_init();
    net_init();

    if (sensor_init() != ESP_OK) {
        ESP_LOGW(log_tag, "local sensor unavailable");
    }

    device_mcp_set_sensor_model(sensor_name());

    esp_err_t audio_err = audio_init();
    if (audio_err == ESP_FAIL) {
        vTaskDelay(pdMS_TO_TICKS(150));
        audio_err = audio_init();
    }
    if (audio_err != ESP_OK) {
        ESP_LOGW(log_tag, "audio unavailable: %s", esp_err_to_name(audio_err));
    }

    const button_config_t btn_cfg = {
        .long_press_time = BUTTON_LONG_PRESS_MS,
        .short_press_time = 50,
    };
    const button_gpio_config_t gpio_cfg = {
        .gpio_num = BUTTON_GPIO,
        .active_level = 0,
    };
    button_handle_t button = NULL;
    ESP_ERROR_CHECK(iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &button));
    ESP_ERROR_CHECK(iot_button_register_cb(button, BUTTON_SINGLE_CLICK, NULL, on_button_tap, NULL));
    ESP_ERROR_CHECK(iot_button_register_cb(button, BUTTON_LONG_PRESS_START, NULL, on_button_hold, NULL));

    const button_config_t voice_btn_cfg = {
        .short_press_time = 50,
    };
    const button_gpio_config_t voice_gpio_cfg = {
        .gpio_num = VOICE_BUTTON_GPIO,
        .active_level = 0,
    };
    button_handle_t voice_button = NULL;
    ESP_ERROR_CHECK(iot_button_new_gpio_device(&voice_btn_cfg, &voice_gpio_cfg, &voice_button));
    ESP_ERROR_CHECK(iot_button_register_cb(voice_button, BUTTON_SINGLE_CLICK, NULL, on_voice_button_tap, NULL));

    // Local wake detection runs in its own task so microphone reads never stall
    // weather.
    if (voice_init() != ESP_OK) {
        ESP_LOGW(log_tag, "voice unavailable");
    }

    struct app_state app = {0};
    app.view = VIEW_DAILY;
    app.status = UI_STATUS_CONNECTING;
    app.phase = APP_PHASE_CONNECT;

    (void)sensor_read(&app.sensor);
    device_mcp_publish_sensor(&app.sensor);

    // Paint a placeholder while the device starts networking and fetches the
    // first forecast.
    xSemaphoreTake(display_lock, portMAX_DELAY);
    draw_page(&app);
    epd_refresh();
    xSemaphoreGive(display_lock);
    ESP_ERROR_CHECK(xTaskCreate(clock_task, "clock", 4096, &app, 1, &clock_task_handle) == pdPASS ? ESP_OK
                                                                                                  : ESP_ERR_NO_MEM);
    voice_feedback_set_observer(on_voice_feedback);
    on_voice_feedback();

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
