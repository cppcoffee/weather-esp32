#include "battery.h"

#include <stdlib.h>

#include "audio.h"
#include "button.h"
#include "driver/gpio.h"
#include "epd.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "sensor.h"
#include "voice.h"

_Static_assert(BATTERY_ADC_GPIO == 1, "BATTERY_ADC_GPIO must be GPIO1 (ADC1_CH0 on ESP32-S3)");
#if CONFIG_SPIRAM_MODE_OCT
#define BATTERY_PIN_OVERLAPS_PSRAM(pin) ((pin) >= 33 && (pin) <= 37)
_Static_assert(!BATTERY_PIN_OVERLAPS_PSRAM(BATTERY_ADC_GPIO) && !BATTERY_PIN_OVERLAPS_PSRAM(BATTERY_PGOOD_GPIO)
                   && !BATTERY_PIN_OVERLAPS_PSRAM(BATTERY_CHRG_GPIO),
               "Battery wiring overlaps GPIO33-37 reserved for octal PSRAM");
#endif
_Static_assert(BATTERY_ADC_GPIO != 19 && BATTERY_ADC_GPIO != 20 && BATTERY_PGOOD_GPIO != 19 && BATTERY_PGOOD_GPIO != 20
                   && BATTERY_CHRG_GPIO != 19 && BATTERY_CHRG_GPIO != 20,
               "Battery pins overlap GPIO19/20 reserved for USB-JTAG");
_Static_assert(BATTERY_ADC_GPIO != BATTERY_PGOOD_GPIO && BATTERY_ADC_GPIO != BATTERY_CHRG_GPIO
                   && BATTERY_PGOOD_GPIO != BATTERY_CHRG_GPIO,
               "Battery pins must be distinct");
#define BATTERY_PIN_CONFLICTS(pin)                                                                                     \
    ((pin) == BUTTON_GPIO || (pin) == VOICE_BUTTON_GPIO || (pin) == VOICE_LED_GPIO || (pin) == SENSOR_I2C_SDA_GPIO     \
     || (pin) == SENSOR_I2C_SCL_GPIO || (pin) == AUDIO_PA_EN_GPIO || (pin) == AUDIO_I2S_DOUT_GPIO                      \
     || (pin) == AUDIO_I2S_WS_GPIO || (pin) == AUDIO_I2S_DIN_GPIO || (pin) == AUDIO_I2S_BCLK_GPIO                      \
     || (pin) == AUDIO_I2S_MCLK_GPIO || (pin) == AUDIO_I2C_SCL_GPIO || (pin) == AUDIO_I2C_SDA_GPIO || (pin) == EPD_SCK \
     || (pin) == EPD_MOSI || (pin) == EPD_RST || (pin) == EPD_DC || (pin) == EPD_CS || (pin) == EPD_BUSY)
_Static_assert(!BATTERY_PIN_CONFLICTS(BATTERY_ADC_GPIO) && !BATTERY_PIN_CONFLICTS(BATTERY_PGOOD_GPIO)
                   && !BATTERY_PIN_CONFLICTS(BATTERY_CHRG_GPIO),
               "Battery pins overlap another peripheral pin");

#define BATTERY_ADC_UNIT           ADC_UNIT_1
#define BATTERY_ADC_CHANNEL        ADC_CHANNEL_0
#define BATTERY_ADC_ATTEN          ADC_ATTEN_DB_12
#define BATTERY_ADC_SAMPLES        8
#define BATTERY_DIVIDER_NUM        2
#define BATTERY_MIN_VALID_MV       2500
#define BATTERY_MAX_VALID_MV       4500
#define BATTERY_CHARGING_OFFSET_MV 80
#define BATTERY_BLINK_WINDOW_US    2500000LL
#define BATTERY_STABLE_WINDOW_US   3000000LL

static const char *log_tag = "battery";

static struct {
    bool                      initialized;
    adc_oneshot_unit_handle_t adc;
    adc_cali_handle_t         cali;
    bool                      has_cali;
    int                       filtered_mv;
    bool                      have_sample;
    bool                      last_chrg_high;
    bool                      chrg_blinking;
    unsigned                  chrg_toggles;
    int64_t                   last_chrg_change_us;
} state;

static int
lerp_percent(int mv, int mv_low, int mv_high, int pct_low, int pct_high)
{
    if (mv <= mv_low) {
        return pct_low;
    }
    if (mv >= mv_high) {
        return pct_high;
    }
    return pct_low + (mv - mv_low) * (pct_high - pct_low) / (mv_high - mv_low);
}

static int
voltage_to_percent(int vbat_mv, bool usb_present, bool charging)
{
    if (usb_present && !charging && vbat_mv >= 4000) {
        return 100;
    }
    int ocv_mv = charging ? (vbat_mv - BATTERY_CHARGING_OFFSET_MV) : vbat_mv;
    int pct;
    if (ocv_mv <= 3200) {
        pct = 0;
    } else if (ocv_mv < 3500) {
        pct = lerp_percent(ocv_mv, 3200, 3500, 0, 15);
    } else if (ocv_mv < 3680) {
        pct = lerp_percent(ocv_mv, 3500, 3680, 15, 40);
    } else if (ocv_mv < 3850) {
        pct = lerp_percent(ocv_mv, 3680, 3850, 40, 70);
    } else if (ocv_mv < 4050) {
        pct = lerp_percent(ocv_mv, 3850, 4050, 70, 95);
    } else {
        pct = lerp_percent(ocv_mv, 4050, 4150, 95, 100);
    }
    if (charging && pct >= 100) {
        pct = 99;
    }
    return pct;
}

static bool
sample_vbat_mv(int *out_mv)
{
    int sum_mv = 0;
    int count = 0;
    for (int i = 0; i < BATTERY_ADC_SAMPLES; i++) {
        int raw = 0;
        if (adc_oneshot_read(state.adc, BATTERY_ADC_CHANNEL, &raw) != ESP_OK) {
            continue;
        }
        int mv = 0;
        if (state.has_cali && adc_cali_raw_to_voltage(state.cali, raw, &mv) == ESP_OK) {
            sum_mv += mv;
        } else {
            sum_mv += raw * 3100 / 4095;
        }
        count++;
    }
    if (count == 0) {
        return false;
    }
    *out_mv = (sum_mv / count) * BATTERY_DIVIDER_NUM;
    return true;
}

esp_err_t
battery_init(void)
{
    if (state.initialized) {
        return ESP_OK;
    }

    const gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << BATTERY_PGOOD_GPIO) | (1ULL << BATTERY_CHRG_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_cfg);
    if (err != ESP_OK) {
        return err;
    }

    const adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = BATTERY_ADC_UNIT,
    };
    err = adc_oneshot_new_unit(&unit_cfg, &state.adc);
    if (err != ESP_OK) {
        ESP_LOGW(log_tag, "ADC1 init failed: %s", esp_err_to_name(err));
        return err;
    }

    const adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(state.adc, BATTERY_ADC_CHANNEL, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(log_tag, "ADC1 channel config failed: %s", esp_err_to_name(err));
        (void)adc_oneshot_del_unit(state.adc);
        state.adc = NULL;
        return err;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    const adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = BATTERY_ADC_UNIT,
        .chan = BATTERY_ADC_CHANNEL,
        .atten = BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &state.cali) == ESP_OK) {
        state.has_cali = true;
    }
#endif

    state.last_chrg_high = gpio_get_level(BATTERY_CHRG_GPIO) != 0;
    state.last_chrg_change_us = esp_timer_get_time();
    state.initialized = true;
    ESP_LOGI(log_tag, "battery monitor ready (ADC=GPIO%d PGOOD=GPIO%d CHRG=GPIO%d)", BATTERY_ADC_GPIO,
             BATTERY_PGOOD_GPIO, BATTERY_CHRG_GPIO);
    return ESP_OK;
}

bool
battery_read(battery_status_t *out, bool sample_adc)
{
    if (out == NULL) {
        return false;
    }
    *out = (battery_status_t){0};
    if (!state.initialized) {
        return false;
    }

    bool    usb_present = gpio_get_level(BATTERY_PGOOD_GPIO) == 0;
    bool    chrg_high = gpio_get_level(BATTERY_CHRG_GPIO) != 0;
    int64_t now_us = esp_timer_get_time();

    if (!usb_present) {
        state.chrg_blinking = false;
        state.chrg_toggles = 0;
        state.last_chrg_high = chrg_high;
        state.last_chrg_change_us = now_us;
    } else if (chrg_high != state.last_chrg_high) {
        if (now_us - state.last_chrg_change_us <= BATTERY_BLINK_WINDOW_US) {
            state.chrg_toggles++;
            if (state.chrg_toggles >= 2) {
                state.chrg_blinking = true;
            }
        } else {
            state.chrg_toggles = 1;
        }
        state.last_chrg_high = chrg_high;
        state.last_chrg_change_us = now_us;
    } else if (state.chrg_blinking && now_us - state.last_chrg_change_us >= BATTERY_STABLE_WINDOW_US) {
        state.chrg_blinking = false;
        state.chrg_toggles = 0;
    }

    bool charging = usb_present && chrg_high && !state.chrg_blinking;

    if (sample_adc || !state.have_sample) {
        int raw_mv = 0;
        if (sample_vbat_mv(&raw_mv)) {
            if (!state.have_sample || abs(raw_mv - state.filtered_mv) > 250) {
                state.filtered_mv = raw_mv;
            } else {
                state.filtered_mv = (state.filtered_mv * 3 + raw_mv) / 4;
            }
            state.have_sample = true;
        }
    }

    out->usb_present = usb_present;
    out->charging = charging;
    if (!state.have_sample || state.filtered_mv < BATTERY_MIN_VALID_MV || state.filtered_mv > BATTERY_MAX_VALID_MV) {
        return false;
    }

    out->voltage_mv = state.filtered_mv;
    out->percent = voltage_to_percent(state.filtered_mv, usb_present, charging);
    out->valid = true;
    return true;
}
