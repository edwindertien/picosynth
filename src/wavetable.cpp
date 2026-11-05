// ============================================================
//  wavetable.cpp  —  storage + generation of the waveform tables
// ============================================================
#include "wavetable.h"
#include <math.h>

int16_t Wavetable::_tab[WAVE_COUNT][WAVETABLE_SIZE];

void Wavetable::init() {
    for (int i = 0; i < WAVETABLE_SIZE; i++) {
        const float ph = (float)i / (float)WAVETABLE_SIZE;     // 0..1

        _tab[WAVE_SINE][i] = (int16_t)(sinf(ph * 2.0f * (float)M_PI) * 32767.0f);

        const float tri = (ph < 0.25f) ?  ph * 4.0f
                        : (ph < 0.75f) ?  1.0f - (ph - 0.25f) * 4.0f
                        :                -1.0f + (ph - 0.75f) * 4.0f;
        _tab[WAVE_TRI][i] = (int16_t)(tri * 32767.0f);

        const float saw = (ph < 0.5f) ? ph * 2.0f : ph * 2.0f - 2.0f;   // 0 -> +1, -1 -> 0
        _tab[WAVE_SAW][i] = (int16_t)(saw * 32767.0f);

        _tab[WAVE_SQUARE][i] = (ph < 0.5f) ? 32767 : -32767;
    }
}
