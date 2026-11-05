#pragma once
// ============================================================
//  synth_filter.h  —  Moog-style 4-pole ladder (Huovilainen topology)
//
//    stage:  s += g * ( sat(in) - sat(s) )        x4, resonance fed back from s4
//
//  Same topology and same coefficient law as the version you were happy with
//  (g = tan(pi*f/fs)), but:
//    * g is computed ONCE PER CONTROL BLOCK by the voice and interpolated per
//      sample (gStep) -> no per-sample pow/tan/quantise work, and no zipper noise.
//    * tan() is a Pade approximation (error < 0.5 % up to FILTER_MAX_HZ) so core 1
//      never calls into libm (flash).
//    * g is limited to the stable range (FILTER_MAX_HZ, see config.h).
//    * sane() lets the voice detect a numerical blow-up and reset instead of
//      screaming (or taking down the audio loop).
// ============================================================
#include <stdint.h>
#include <math.h>
#include "config.h"

struct SynthFilter {
    float s1 = 0, s2 = 0, s3 = 0, s4 = 0;   // stage states
    float g = 0.1f, gStep = 0.0f;           // coefficient and its per-sample ramp
    float res4 = 0.0f;                      // feedback gain (resonance * 4)

#if FILTER_CHEAP_SAT
    // Division-free saturator: x - 4/27 x^3, clamped at +-1.5 (reaches exactly +-1, slope 0).
    static HOT float sat(float x) {
        if (x >  1.5f) x =  1.5f;
        if (x < -1.5f) x = -1.5f;
        return x * (1.0f - (4.0f / 27.0f) * x * x);
    }
#else
    // tanh-like Pade [3/2]; one division.
    static HOT float sat(float x) {
        if (x >  4.0f) return  1.0f;
        if (x < -4.0f) return -1.0f;
        const float x2 = x * x;
        return x * (27.0f + x2) / (27.0f + 9.0f * x2);
    }
#endif

    // g = tan(pi * hz / fs)  (Pade; hz limited to [20, FILTER_MAX_HZ])
    static HOT float coeffForHz(float hz) {
        if (hz < 20.0f)            hz = 20.0f;
        if (hz > FILTER_MAX_HZ)    hz = FILTER_MAX_HZ;
        const float x  = (3.14159265f / (float)SAMPLE_RATE) * hz;
        const float x2 = x * x;
        return x * (15.0f - x2) / (15.0f - 6.0f * x2);
    }

    void reset(float g0, float res4_) {
        s1 = s2 = s3 = s4 = 0.0f;
        g = g0; gStep = 0.0f; res4 = res4_;
    }

    // Called once per control block: aim g at gTarget over the next CONTROL_DIV samples.
    HOT void setTarget(float gTarget) { gStep = (gTarget - g) * (1.0f / (float)CONTROL_DIV); }

    // False if any state is NaN or has run away (|s| >= 16 is far outside the +-1.2 range).
    HOT bool sane() const {
        return (s1 > -16.0f && s1 < 16.0f) && (s2 > -16.0f && s2 < 16.0f) &&
               (s3 > -16.0f && s3 < 16.0f) && (s4 > -16.0f && s4 < 16.0f);
    }

    HOT float tick(float in) {
        g += gStep;
        const float u  = in - res4 * s4;            // input minus resonance feedback
        const float t0 = sat(u);
        const float t1 = sat(s1);
        const float t2 = sat(s2);
        const float t3 = sat(s3);
        const float t4 = sat(s4);
        s1 += g * (t0 - t1);
        s2 += g * (t1 - t2);
        s3 += g * (t2 - t3);
        s4 += g * (t3 - t4);
        return s4;
    }
};
