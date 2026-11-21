// Sample-format helpers shared by voice.c and the host tests. No ESP-IDF or codec
// dependency on purpose: these are the only parts of the voice path a host test can check.
#ifndef __VOICE_AUDIO_H__
#define __VOICE_AUDIO_H__

#include <stddef.h>
#include <stdint.h>

// Interleaved stereo -> mono. The board wires the microphone to the left channel, so the
// right channel is dropped rather than averaged (it may carry playback).
void voice_stereo_to_mono(const int16_t *stereo, int16_t *mono, size_t samples);

// Mono -> interleaved stereo by duplicating each sample into both channels.
void voice_mono_to_stereo(const int16_t *mono, int16_t *stereo, size_t samples);

#endif
