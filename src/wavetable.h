#pragma once
// ============================================================
//  wavetable.h  —  Four single-cycle waveforms, 4096 points, int16
//
//  Storage lives in wavetable.cpp (defined exactly once).
//  Voices call Wavetable::table(w) at control rate and then index the
//  returned pointer directly — no per-sample switch.
// ============================================================
#include <stdint.h>
#include "config.h"

enum { WAVE_SINE = 0, WAVE_TRI = 1, WAVE_SAW = 2, WAVE_SQUARE = 3, WAVE_COUNT = 4 };

class Wavetable {
public:
    static void init();                                   // call once from core 0, before audio starts
    static inline const int16_t* table(uint8_t w) { return _tab[w & 3]; }
    static inline int16_t sine(uint32_t idx)      { return _tab[WAVE_SINE][idx & (WAVETABLE_SIZE - 1)]; }
    static int16_t _tab[WAVE_COUNT][WAVETABLE_SIZE];
};
