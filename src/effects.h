#pragma once
// ============================================================
//  effects.h  —  Post-processing effects chain
//
//  Applied in TonewheelManager::tick() after voice mixing,
//  percussion and click, before master volume and final clip.
//
//  Chain order:
//    1. Overdrive  — cubic soft clip waveshaper
//    2. Vibrato    — sine LFO pitch modulation (all voices)
//    3. Chorus     — BBD-style delay line + LFO
//
//  All methods called from core 1 only. No Serial, no malloc.
//  Parameters are public — set from core 0 via CC or serial.
//  Reads/writes of uint8_t are atomic on ARM; no spinlock needed
//  for parameter updates (worst case one slightly stale sample).
// ============================================================

#include <Arduino.h>
#include "config.h"

// ---- Fast sine approximation (no table, ~10 FPU cycles) ----
// phase: 0.0 .. 1.0 (full cycle, wraps automatically)
static inline float _fx_sin(float phase) {
    phase -= floorf(phase);
    float p = phase * 2.0f - 1.0f;
    float p2 = p * p;
    return p * (3.138982f - p2 * (5.133625f - p2 * 2.532422f));
}

// ============================================================
//  1. OVERDRIVE  (asymmetric tube-style waveshaper)
//
//  Uses an asymmetric rational waveshaper — different curves for
//  positive and negative halves — which generates even harmonics
//  (2nd, 4th) that sound warm and tube-like. The symmetric cubic
//  shaper only generates odd harmonics (3rd, 5th) which sound
//  harsher and more transistor-like.
//
//  Positive half: y = x / (1 + x)    ← gentle, smooth rolloff
//  Negative half: y = x / (1 - 0.5x) ← slightly harder clip
//
//  Both halves stay within ±1 naturally — no hard clamping.
//  The asymmetry is intentional: real tube output stages are not
//  symmetric; this asymmetry is what gives valves their character.
//
//  drive = 0   → bypass (true bypass, zero processing)
//  drive 1–127 → input gain 1.0..5.0, followed by shaper + 1/g comp
// ============================================================
struct Overdrive {
    uint8_t drive = 0;

    inline int32_t process(int32_t x) const {
        if (drive == 0) return x;

        // Input gain: 1.0..5.0
        float g = 1.0f + (float)(drive - 1) * (4.0f / 126.0f);
        float f = (float)x * g * (1.0f / 32768.0f);  // normalise + drive

        // Asymmetric tube waveshaper — generates even harmonics
        if (f >= 0.0f)
            f = f / (1.0f + f);          // positive: smooth rolloff
        else
            f = f / (1.0f - 0.5f * f);  // negative: slightly harder

        // Level compensation: 1/g so output stays close to input level.
        // The shaper adds warmth/character without changing loudness.
        f /= g;

        return (int32_t)(f * 32768.0f);
    }
};

// ============================================================
//  2. VIBRATO
//  Sine LFO modulates pitch of all active voices each sample.
//  The frequency deviation is applied via a shared pitch_bend
//  factor that TonewheelManager reads and multiplies into
//  each voice's oscillator frequency in its tick() call.
//
//  depth = 0 → bypass
//  depth = 1..127 → ±0.006 .. ±0.75 semitones (subtle to strong)
//  rate  = 0..127 → 0.5 Hz .. 9.0 Hz LFO speed
// ============================================================
struct Vibrato {
    uint8_t depth = 0;    // 0 = off
    uint8_t rate  = 40;   // default ~4 Hz

    // Output: pitch multiplier for all voices (1.0 = no shift)
    // TonewheelManager reads this each sample from core 1.
    volatile float pitchMult = 1.0f;

    // Internal LFO state (core 1 only, no volatile needed)
    float _phase = 0.0f;

    inline void tick() {
        if (depth == 0) {
            pitchMult = 1.0f;
            return;
        }
        // Rate: 0..127 → 0.5..9.0 Hz at 44100 Hz sample rate
        float hz     = 0.5f + (float)rate * (8.5f / 127.0f);
        float inc    = hz / (float)SAMPLE_RATE;
        _phase      += inc;
        if (_phase >= 1.0f) _phase -= 1.0f;

        float lfo    = _fx_sin(_phase);  // -1..+1

        // Depth: 0..127 → 0..VIBRATO_MAX_SEMITONES (from config.h)
        // Default 0.25 semitones is authentic Hammond vibrato.
        // Increase VIBRATO_MAX_SEMITONES in config.h for more effect.
        float semis  = (float)depth * (VIBRATO_MAX_SEMITONES / 127.0f);
        float devHz  = semis * 0.05946f;   // semitone ratio (2^(1/12)-1 ≈ 0.05946)
        pitchMult    = 1.0f + lfo * devHz;
    }
};

// ============================================================
//  3. CHORUS
//  Dual quadrature BBD-style chorus (Boss CE-2 architecture).
//
//  Two delay lines driven by LFOs 90° apart. When one LFO is
//  at its turnaround point (fastest rate of change → most
//  audible artifact), the other is at its flattest — the two
//  lines mask each other's modulation reversals, giving a
//  smooth, natural chorus character with no periodic click.
//
//  depth = 0   → bypass
//  depth 1–127 → modulation swing (0..2ms, ±~20 cents max)
//  rate  0–127 → LFO speed (0.2..3.0 Hz)
//  mix   0–127 → wet/dry mix (0..50% wet)
//
//  Two delay buffers: 2 × 2048 × 2 bytes = 8 KB SRAM
// ============================================================
struct Chorus {
    uint8_t depth = 0;                     // 0 = bypass
    uint8_t rate  = 30;                    // ~0.7 Hz default
    uint8_t mix   = CHORUS_MIX_DEFAULT;   // wet level

    static constexpr int BUF = 2048;
    int16_t _buf0[BUF] = {};   // delay line A (LFO phase 0°)
    int16_t _buf1[BUF] = {};   // delay line B (LFO phase 90°)
    int     _write = 0;
    float   _phase = 0.0f;

    inline int32_t process(int32_t x) {
        // Clamp and write to both delay lines
        int16_t xs = (int16_t)(x < -32768 ? -32768 : x > 32767 ? 32767 : x);
        _buf0[_write & (BUF-1)] = xs;
        _buf1[_write & (BUF-1)] = xs;

        if (depth == 0) {
            _write++;
            return x;
        }

        // LFO: advance phase
        float hz  = 0.2f + (float)rate * (2.8f / 127.0f);
        _phase   += hz / (float)SAMPLE_RATE;
        if (_phase >= 1.0f) _phase -= 1.0f;

        // Quadrature LFOs: 0° and 90°
        float lfo0 = _fx_sin(_phase);           // sin
        float lfo1 = _fx_sin(_phase + 0.25f);   // cos (90° ahead)

        // Modulation depth: 0..2ms swing (±1ms at depth=127)
        float centreMs = 20.0f;
        float modulMs  = (float)depth * (2.0f / 127.0f);

        // Delay line A
        float dA     = (centreMs + lfo0 * modulMs) * (SAMPLE_RATE / 1000.0f);
        int   dA0    = (int)dA;
        float fracA  = dA - (float)dA0;
        float wetA   = _buf0[(_write - dA0)     & (BUF-1)]
                     + fracA * (float)(_buf0[(_write - dA0 - 1) & (BUF-1)]
                                     - _buf0[(_write - dA0)     & (BUF-1)]);

        // Delay line B (90° offset)
        float dB     = (centreMs + lfo1 * modulMs) * (SAMPLE_RATE / 1000.0f);
        int   dB0    = (int)dB;
        float fracB  = dB - (float)dB0;
        float wetB   = _buf1[(_write - dB0)     & (BUF-1)]
                     + fracB * (float)(_buf1[(_write - dB0 - 1) & (BUF-1)]
                                     - _buf1[(_write - dB0)     & (BUF-1)]);

        _write++;

        // Average both wet signals — their artifacts cancel
        float wet    = (wetA + wetB) * 0.5f;

        // Mix: 0..50% wet (mix=127 → 50%)
        float wetMix = (float)mix * (0.5f / 127.0f);
        return (int32_t)((float)x * (1.0f - wetMix) + wet * wetMix);
    }
};