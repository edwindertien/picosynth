#pragma once
// ============================================================
//  synth_env.h  —  linear ADSR, advanced at control rate
//
//  OWNERSHIP RULE: after a voice is published (SynthVoice::live = 1) this
//  object is read/written by core 1 ONLY. Core 0 asks for a release through
//  the voice's request counters; it never touches `state` or `level`.
//  (Two cores writing `state` loses releases = stuck notes.)
//
//  Rates are "level change per sample". advance(n) applies n samples at once;
//  the voice interpolates between control ticks, so segment shapes are exactly
//  the same as a per-sample envelope, only stage *boundaries* are quantised to
//  16 samples (0.36 ms).
// ============================================================
#include <stdint.h>
#include <math.h>
#include "config.h"

// ---- time <-> rate helpers (core 0 only: they call powf) ---
// CC 0..127 -> 3 ms .. 3 s, exponential.
static inline float envTimeFromCC(uint8_t cc) { return 0.003f * powf(1000.0f, cc / 127.0f); }
static inline float envRate(float seconds)    { return 1.0f / (seconds * (float)SAMPLE_RATE); }
static inline float envRateFromCC(uint8_t cc) { return envRate(envTimeFromCC(cc)); }

struct SynthEnv {
    enum : uint8_t { IDLE = 0, ATTACK, DECAY, SUSTAIN, RELEASE };

    uint8_t state = IDLE;
    float   level = 0.0f;
    float   atk = 1.0f, dec = 1.0f, sus = 1.0f, rel = 1.0f;

    void start(float a, float d, float s, float r) {
        atk = a; dec = d; sus = s; rel = r;
        level = 0.0f;
        state = ATTACK;
    }

    HOT void release(float r) {
        if (state != IDLE) { rel = r; state = RELEASE; }
    }

    HOT void advance(int n) {
        switch (state) {
            case ATTACK:
                level += atk * (float)n;
                if (level >= 1.0f) { level = 1.0f; state = DECAY; }
                break;
            case DECAY:
                level -= dec * (float)n;
                if (level <= sus) { level = sus; state = SUSTAIN; }
                break;
            case RELEASE:
                level -= rel * (float)n;
                if (level <= 0.0f) { level = 0.0f; state = IDLE; }
                break;
            default:            // SUSTAIN / IDLE: nothing moves
                break;
        }
    }

    const char* name() const {
        static const char* n[] = { "IDLE", "ATK ", "DEC ", "SUS ", "REL " };
        return n[state > RELEASE ? 0 : state];
    }
};
