#pragma once
// ============================================================
//  adsr.h  –  ADSR Envelope Generator
//
//  Float output in range 0.0–1.0.
//  Two instances per voice: one for amplitude, one for filter.
//  Times are in seconds; sustain is a level (0–1).
//
//  State machine:
//    IDLE → ATTACK → DECAY → SUSTAIN → RELEASE → IDLE
// ============================================================
#include "config.h"

class ADSR {
public:
    enum class State { IDLE, ATTACK, DECAY, SUSTAIN, RELEASE };

    ADSR() { reset(); }

    void setAttack (float seconds) { _attack  = secondsToRate(seconds); }
    void setDecay  (float seconds) { _decay   = secondsToRate(seconds); }
    void setSustain(float level)   { _sustain = constrain(level, 0.0f, 1.0f); }
    void setRelease(float seconds) { _release = secondsToRate(seconds); }

    // Set all four from 0–127 CC values
    void setFromCC(uint8_t aCC, uint8_t dCC, uint8_t sCC, uint8_t rCC) {
        // Attack/decay/release: CC maps 0→0.001s, 127→8s exponentially
        setAttack (ccToSeconds(aCC));
        setDecay  (ccToSeconds(dCC));
        setSustain(sCC / 127.0f);
        setRelease(ccToSeconds(rCC));
    }

    void noteOn()  {
        _state = State::ATTACK;
        // If legato (already sounding), start attack from current level
    }
    void noteOff() {
        if (_state != State::IDLE)
            _state = State::RELEASE;
    }

    bool  isIdle()    const { return _state == State::IDLE; }
    float level()   const { return _level; }
    void  forceIdle()     { _state = State::IDLE; _level = 0.0f; }

    // Call once per sample from the audio ISR (core 1)
    inline float tick() {
        switch (_state) {
            case State::ATTACK:
                _level += _attack;
                if (_level >= 1.0f) {
                    _level = 1.0f;
                    _state = State::DECAY;
                }
                break;
            case State::DECAY:
                _level -= _decay;
                if (_level <= _sustain) {
                    _level = _sustain;
                    _state = State::SUSTAIN;
                }
                break;
            case State::SUSTAIN:
                // Hold at sustain level until noteOff
                break;
            case State::RELEASE:
                _level -= _release;
                if (_level <= 0.0f) {
                    _level = 0.0f;
                    _state = State::IDLE;
                }
                break;
            default:
                _level = 0.0f;
                break;
        }
        return _level;
    }

    void reset() {
        _state   = State::IDLE;
        _level   = 0.0f;
        // Default: plucky patch
        setAttack (0.003f);
        setDecay  (0.3f);
        setSustain(0.6f);
        setRelease(0.25f);
    }

    State state() const { return _state; }

private:
    State _state   = State::IDLE;
    float _level   = 0.0f;
    float _attack  = 0.001f;
    float _decay   = 0.001f;
    float _sustain = 0.6f;
    float _release = 0.001f;

    // Convert seconds → per-sample increment at SAMPLE_RATE
    static float secondsToRate(float s) {
        if (s <= 0.0f) return 1.0f;
        return 1.0f / (s * (float)SAMPLE_RATE);
    }

    // CC 0–127 → 0.001s–8s exponential mapping
    static float ccToSeconds(uint8_t cc) {
        // 0 → 0.001s, 127 → 8.0s  (exponential)
        float t = cc / 127.0f;
        return 0.001f * powf(8000.0f, t);   // 8s max
    }
};