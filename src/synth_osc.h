#pragma once
// ============================================================
//  synth_osc.h  —  phase-accumulator oscillator (one wavetable read per sample)
//
//  `inc` and `tbl` are refreshed by the voice at control rate (pitch, vibrato,
//  bend, detune and waveform changes all arrive through them), so the per-sample
//  work is: one integer add, one table load, one int->float convert.
// ============================================================
#include <stdint.h>
#include "config.h"

static inline float hzToInc(float hz) { return hz * (4294967296.0f / (float)SAMPLE_RATE); }

struct SynthOsc {
    uint32_t       phase = 0;
    uint32_t       inc   = 0;
    const int16_t* tbl   = nullptr;

    HOT float tick() {
        phase += inc;
        return (float)tbl[phase >> PHASE_SHIFT] * (1.0f / 32768.0f);
    }
};
