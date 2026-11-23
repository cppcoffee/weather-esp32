#include "voice_audio.h"

#include <math.h>

void
voice_stereo_to_mono(const int16_t *stereo, int16_t *mono, size_t samples)
{
    for (size_t i = 0; i < samples; i++) {
        mono[i] = stereo[i * 2];
    }
}

void
voice_mono_to_stereo(const int16_t *mono, int16_t *stereo, size_t samples)
{
    for (size_t i = 0; i < samples; i++) {
        stereo[i * 2] = mono[i];
        stereo[i * 2 + 1] = mono[i];
    }
}

void
voice_fill_chime(int16_t *mono, size_t samples, uint32_t sample_rate)
{
    if (mono == NULL || samples == 0 || sample_rate == 0) {
        return;
    }
    const float  two_pi = 6.28318530718f;
    const size_t half = samples / 2;
    const size_t fade = samples / 10;
    float        phase = 0.0f;
    for (size_t i = 0; i < samples; i++) {
        float freq = i < half ? 880.0f : 1318.5f;
        float env = 1.0f;
        if (fade > 0 && i < fade) {
            env = (float)i / (float)fade;
        } else if (fade > 0 && i + fade > samples) {
            env = (float)(samples - i) / (float)fade;
        }
        mono[i] = (int16_t)(4800.0f * env * sinf(phase));
        phase += two_pi * freq / (float)sample_rate;
    }
}
