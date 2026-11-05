#pragma once
// ============================================================
//  lfo.h  –  Low Frequency Oscillator
//
//  Runs at audio rate but at sub-audio frequency (0.1–20 Hz).
//  Waveforms: sine (0), sample-and-hold (1).
//  Output: -1.0 to +1.0.
//  Used for: vibrato (pitch mod) and filter mod.
// ============================================================
#include <Arduino.h>
#include <math.h>
#include "config.h"
#include "wavetable.h"

class LFO {
public:
    void setRate(float hz) {
        _hz = constrain(hz, 0.01f, 30.0f);
        _phaseInc = (uint32_t)((_hz / (float)SAMPLE_RATE) * 4294967296.0f);
    }
    void setWave(uint8_t w) { _wave = w & 1; }  // 0=sine, 1=S&H

    // Rate from CC (0–127 → 0.1–20 Hz exponential)
    void setRateCC(uint8_t cc) {
        float hz = 0.1f * powf(200.0f, cc / 127.0f);
        setRate(hz);
    }

    // Returns -1.0 to +1.0
    inline float tick() {
        uint32_t prev = _phase;
        _phase += _phaseInc;

        if (_wave == 0) {
            // Sine via wavetable
            uint32_t idx = _phase >> (32 - 11);  // WAVETABLE_SIZE=2048 → 11 bits
            return Wavetable::sine(idx) / 32767.0f;
        } else {
            // Sample & Hold: randomise on phase wrap
            if (_phase < prev) {   // wrapped
                _sah = ((float)random(65536) / 32768.0f) - 1.0f;
            }
            return _sah;
        }
    }

    void reset() { _phase = 0; _sah = 0.0f; }
    float hz() const { return _hz; }

private:
    uint32_t _phase    = 0;
    uint32_t _phaseInc = 0;
    uint8_t  _wave     = 0;
    float    _hz       = 4.0f;
    float    _sah      = 0.0f;
};
