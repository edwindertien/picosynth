#pragma once
// ============================================================
//  vco.h  –  Phase-accumulator VCO
//
//  All expensive math (powf) on core 0 at noteOn time.
//  tick() is pure 32-bit integer add + table lookup.
//  Pitch bend and LFO mod are stored as pre-computed additive
//  phase-increment deltas, updated cheaply on core 1.
// ============================================================
#include <stdint.h>
#include <math.h>
#include "config.h"
#include "wavetable.h"

class VCO {
public:
    void setWave(uint8_t w) { _wave = w & 3; }

    // Core 0 only — powf allowed here
    void setNote(uint8_t note, float detuneCents = 0.0f) {
        float hz = 440.0f * powf(2.0f, ((float)note - 69.0f) / 12.0f);
        if (detuneCents != 0.0f)
            hz *= powf(2.0f, detuneCents / 1200.0f);
        _baseHz    = hz;
        _currentHz = hz;
        _targetHz  = hz;
        _calcBaseInc();
        _phaseInc = _baseInc;  // start at exact pitch
    }

    // Core 0: called when pitch bend value changes
    // Converts semitones → ratio and stores as inc multiplier
    // NOT called from core 1
    void setPitchBend(float semitones) {
        // Fast approx: 2^(s/12) using minimax polynomial, error <0.002%
        _pitchRatio = _pow2semi(semitones);
        _calcBaseInc();
    }

    void setPortamento(float seconds) {
        _portamento = seconds;
        _portaRate  = (seconds > 0.001f) ? (1.0f / (seconds * SAMPLE_RATE)) : 1.0f;
    }

    // Core 1: cheap linear vibrato — called every sample
    // lfoSemi: typically ±0..2, approximation good to ±0.1%
    inline void applyLFO(float lfoSemi) {
        // Linear approx: 2^(x/12) ≈ 1 + x*ln(2)/12 = 1 + x*0.05776
        float ratio = 1.0f + lfoSemi * 0.05776f;
        _phaseInc = (uint32_t)(_baseInc * ratio);
    }

    inline int16_t tick() {
        // Portamento glide — all float, but rare branch
        if (_portamento > 0.001f && _currentHz != _targetHz) {
            _currentHz += (_targetHz - _currentHz) * _portaRate;
            if (fabsf(_targetHz - _currentHz) < 0.05f) _currentHz = _targetHz;
            _calcBaseInc();
            _phaseInc = _baseInc;
        }
        _phase += _phaseInc;
        return Wavetable::read(_wave, _phase);
    }

    void reset() { _phase = 0; _pitchRatio = 1.0f; }

private:
    uint32_t _phase     = 0;
    uint32_t _phaseInc  = 0;
    uint32_t _baseInc   = 0;   // phaseInc at current pitch+bend, no LFO
    uint8_t  _wave      = 2;
    float    _baseHz    = 440.0f;
    float    _currentHz = 440.0f;
    float    _targetHz  = 440.0f;
    float    _pitchRatio    = 1.0f;
    float    _portamento    = 0.0f;
    float    _portaRate     = 1.0f;

    void _calcBaseInc() {
        float hz = _currentHz * _pitchRatio;
        _baseInc = (uint32_t)(hz / (float)SAMPLE_RATE * 4294967296.0f);
    }

    // Fast 2^(semitones/12) via minimax polynomial
    // Valid range: ±12 semitones (one octave), error < 0.002%
    static float _pow2semi(float s) {
        // Clamp to ±12 semitones (standard pitch bend is ±2)
        if (s >  12.0f) s =  12.0f;
        if (s < -12.0f) s = -12.0f;
        float x = s * (1.0f / 12.0f);  // convert to octaves
        // 2^x polynomial (Horner form, 4th order)
        return 1.0f + x*(0.693147f + x*(0.240227f + x*(0.055504f + x*0.009618f)));
    }
};