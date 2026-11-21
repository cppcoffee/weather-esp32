#include "voice_audio.h"

void voice_stereo_to_mono(const int16_t *stereo, int16_t *mono, size_t samples)
{
    for (size_t i = 0; i < samples; i++) {
        mono[i] = stereo[i * 2];
    }
}

void voice_mono_to_stereo(const int16_t *mono, int16_t *stereo, size_t samples)
{
    for (size_t i = 0; i < samples; i++) {
        stereo[i * 2] = mono[i];
        stereo[i * 2 + 1] = mono[i];
    }
}
