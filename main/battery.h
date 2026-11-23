#ifndef __BATTERY_H__
#define __BATTERY_H__

#include <stdbool.h>

#include "esp_err.h"
#include "ui.h"

// Battery voltage divider (R9=100k / R10=100k behind Q4 AO3401A high-side
// switch on GPIO1 / ADC1_CH0) and charger status inputs (Q3 2N7002 on GPIO13
// PGOOD active-low when USB +5V is present; Q5 2N7002 on GPIO14 CHRG inverted
// from ETA6002 STAT: high while charging, low when charge complete).
#define BATTERY_ADC_GPIO   1
#define BATTERY_PGOOD_GPIO 13
#define BATTERY_CHRG_GPIO  14

// Initialize GPIO13/14 status inputs and ADC1_CH0 (GPIO1) oneshot reader.
esp_err_t battery_init(void);

// Read USB presence, charging state, and battery voltage/percentage into *out.
// When sample_adc is false and a previous reading exists, reuses the cached
// voltage while refreshing USB/charging GPIO state immediately.
bool battery_read(battery_status_t *out, bool sample_adc);

#endif
