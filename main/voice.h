// Xiaozhi voice chat: WakeNet listens locally and starts one turn per wake word.
// Protocol work is delegated to esp_xiaozhi.
#pragma once

#include <stdbool.h>

#include "esp_err.h"

// Load the WakeNet model and start the audio tasks. Capture starts after voice_net_ready().
esp_err_t voice_init(void);

// Call after the first weather attempt/time sync, so voice setup does not overlap
// the boot's weather HTTPS request. First pairing still prints a code to the log.
void voice_net_ready(void);

// Cancel recording/playback and discard pending triggers before changing WiFi.
void voice_net_pause(void);

// A successful begin reserves the network until end. False means voice is setting
// up or handling a turn; defer weather while continuing to service the button.
// Both functions are harmless when voice_init failed.
bool voice_weather_begin(void);
void voice_weather_end(void);
