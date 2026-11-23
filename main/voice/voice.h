// Xiaozhi voice chat: WakeNet listens locally and starts one turn per wake
// word. Protocol work is delegated to esp_xiaozhi.
#ifndef __VOICE_H__
#define __VOICE_H__

#include <stdbool.h>

#include "esp_err.h"

// External green wake-indicator LED (active-high; series resistor to GND).
// Avoid GPIO19/20 (USB-Serial/JTAG) and GPIO33-37 (octal PSRAM).
#define VOICE_LED_GPIO         47
#define VOICE_LED_ACTIVE_LEVEL 1

// Load the WakeNet model and start the audio tasks. Capture starts after
// voice_net_ready().
esp_err_t voice_init(void);

// Enable local wake detection once Wi-Fi credentials are configured.
void voice_enable_wake(void);

// Allow background activation checks after the first weather attempt.
void voice_net_ready(void);

// Report STA IP acquisition/loss. Disconnect schedules teardown on the voice
// task.
void voice_net_set_connected(bool connected);

// Cancel recording/playback and discard pending triggers before changing WiFi.
void voice_net_pause(void);

// A successful begin reserves the network until end. False means voice is
// setting up or handling a turn; defer weather while continuing to service the
// button. Both functions are harmless when voice_init failed.
bool voice_weather_begin(void);
void voice_weather_end(void);

// Toggle voice chat from the dedicated voice button: wakes immediately when
// idle, or hangs up cleanly when a conversation is active.
void voice_toggle_chat(void);

#endif
