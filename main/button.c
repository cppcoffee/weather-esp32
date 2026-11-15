#include "button.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"

#define SETTLE_SAMPLES 2 // consecutive samples before a new level counts (~40 ms)

void button_init(void)
{
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
}

button_event_t button_poll(unsigned elapsed_ms)
{
    static bool stable; // debounced level, true = pressed
    static unsigned streak;
    static unsigned held_ms;
    static bool hold_reported;

    bool pressed = gpio_get_level(BUTTON_GPIO) == 0;
    if (pressed != stable) {
        if (++streak < SETTLE_SAMPLES) {
            return BUTTON_NONE;
        }
        stable = pressed;
        streak = 0;
        if (pressed) {
            held_ms = 0;
            hold_reported = false;
            return BUTTON_NONE;
        }
        if (hold_reported) {
            hold_reported = false;
            return BUTTON_NONE; // the hold was already reported while held
        }
        return BUTTON_TAP;
    }
    streak = 0;
    if (!stable) {
        return BUTTON_NONE;
    }
    held_ms += elapsed_ms;
    if (!hold_reported && held_ms >= BUTTON_LONG_PRESS_MS) {
        hold_reported = true;
        return BUTTON_HOLD;
    }
    return BUTTON_NONE;
}
