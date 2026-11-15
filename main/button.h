// One active-low push button with the internal pull-up: debounced taps plus a long hold.
#pragma once

#include <stdbool.h>

#define BUTTON_GPIO          4
#define BUTTON_POLL_MS       20 // call button_poll() this often
#define BUTTON_LONG_PRESS_MS 3000

typedef enum {
    BUTTON_NONE,
    BUTTON_TAP,  // released before BUTTON_LONG_PRESS_MS
    BUTTON_HOLD, // reported once, as soon as the hold crosses the threshold
} button_event_t;

void button_init(void);
// Call once per slice with the elapsed milliseconds; BUTTON_POLL_MS unless the slice is shorter.
button_event_t button_poll(unsigned elapsed_ms);
