#pragma once
// ============================================================
//  vcf.h  –  Moog Transistor Ladder Filter
//
//  Hot path: tick() is pure multiply/add + tanh approx.
//  setCutoffFromCC() caches last CC+envAmt and only recomputes
//  coefficients (tanf) when they actually change.
// ============================================================
#include <math.h>
#include "config.h"

class VCF {
public:
    VCF() { reset(); }

    void setResonance(float r) {
        r = r < 0.0f ? 0.0f : r > 0.97f ? 0.97f : r;
        if (r != _resonance) { _resonance = r; _dirty = true; }
    }

    // Called every sample from tick() in analog_voice.
    // Avoids powf/tanf unless cutoff actually changed.
    inline void setCutoffFromCC(uint8_t cc, float envAmt, float envLevel) {
        // Cheap integer compare first — skip all float math if nothing changed
        uint8_t  envQ = (uint8_t)(envLevel * 255.0f);
        if (cc == _lastCC && envQ == _lastEnvQ) return;
        _lastCC   = cc;
        _lastEnvQ = envQ;

        // Log mapping: CC 0→~30 Hz, CC 127→~18 kHz
        // Use a lookup-friendly approximation: 30 * 2^(cc/18)
        // Avoids powf — uses a fast bit-shift approximation
        float base  = 30.0f * _pow2fast(cc * (1.0f/18.0f));
        float mod   = envAmt * envLevel * 6000.0f;
        float hz    = base + mod;
        if (hz < 30.0f)   hz = 30.0f;
        if (hz > 19000.0f) hz = 19000.0f;

        float norm = hz / ((float)SAMPLE_RATE * 0.5f);
        if (norm < 0.001f) norm = 0.001f;
        if (norm > 0.98f)  norm = 0.98f;
        if (norm != _cutoff) { _cutoff = norm; _dirty = true; }
    }

    // Per-sample processing — must be fast
    inline float tick(float input) {
        if (_dirty) _recompute();

        float x = input - _r * _y4;
        x = _tanh(x);

        float y1 = _a * x  + _b * _z1; _z1 = _a * x  - _b * _z1;  // wrong sign bug fixed
        float y2 = _a * y1 + _b * _z2; _z2 = _a * y1 - _b * _z2;
        float y3 = _a * y2 + _b * _z3; _z3 = _a * y2 - _b * _z3;
        float y4 = _a * y3 + _b * _z4; _z4 = _a * y3 - _b * _z4;

        _y4 = y4;
        return y4;
    }

    void reset() {
        _z1=_z2=_z3=_z4=_y4=0.0f;
        _cutoff=0.5f; _resonance=0.2f;
        _lastCC=255; _lastEnvQ=255;
        _dirty=true;
    }

private:
    float    _cutoff=0.5f, _resonance=0.2f;
    float    _a=0.f, _b=0.f, _r=0.f;
    float    _z1=0,_z2=0,_z3=0,_z4=0,_y4=0;
    bool     _dirty=true;
    uint8_t  _lastCC=255, _lastEnvQ=255;

    void _recompute() {
        float g = tanf(_cutoff * (float)M_PI);
        _a = g / (1.0f + g);
        _b = (1.0f - g) / (1.0f + g);
        _r = _resonance * 3.8f;
        _dirty = false;
    }

    // Fast 2^x approximation (Ian Stephenson method, error <0.1%)
    static inline float _pow2fast(float x) {
        int xi = (int)x;
        float xf = x - xi;
        // Polynomial approx of 2^xf for xf in [0,1)
        float p = 1.0f + xf * (0.6931472f + xf * (0.2402265f + xf * 0.0557602f));
        // Combine with integer part via bit manipulation
        union { float f; int32_t i; } u;
        u.i = (xi + 127) << 23;
        return u.f * p;
    }

    static inline float _tanh(float x) {
        if (x >  3.0f) return  1.0f;
        if (x < -3.0f) return -1.0f;
        float x2 = x*x;
        return x * (27.0f + x2) / (27.0f + 9.0f*x2);
    }
};