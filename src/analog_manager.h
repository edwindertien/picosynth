#pragma once
// ============================================================
//  analog_manager.h  –  Polyphonic voice manager
//
//  Voice allocation priority:
//    1. Free (inactive) slot             — best
//    2. Releasing slot (noteOff sent)    — steal quietly
//    3. Oldest active slot               — last resort
//
//  Note tracking: _noteMap[note] = slot index, or 0xFF if none.
//  This survives voice stealing — noteOff always finds the right
//  slot even if the note was re-triggered or stolen.
//
//  Thread model: lock-free tick() on core 1.
//  noteOn sets active=false → setup → DMB → active=true.
//  volatile bool active is the only cross-core flag.
// ============================================================
#include <Arduino.h>
#include "config.h"
#include "analog_voice.h"
#include "lfo.h"

// AKAI MPK mini knob CC remapping
#define AKAI_K1  1   // → Filter cutoff
#define AKAI_K2  2   // → Filter resonance
#define AKAI_K3  3   // → Filter env amount
#define AKAI_K4  4   // → Amp attack
#define AKAI_K5  5   // → Amp release
#define AKAI_K6  6   // → Overdrive
// K7 = CC7 = CC_MASTER_VOL (correct already)
#define AKAI_K8  8   // → Osc mix

class AnalogManager {
public:
    struct Params {
        uint8_t  osc1Wave    = 2;
        uint8_t  osc2Wave    = 2;
        float    osc2Detune  = 0.0f;
        float    oscMix      = 0.0f;
        float    noiseMix    = 0.0f;
        uint8_t  vcfCutoffCC = 90;
        float    vcfRes      = 0.1f;
        float    fltEnvAmt   = 0.5f;
        float    pitchBend   = 0.0f;
        float    lfoVibratoAmt = 0.0f;
        volatile float lfoValue = 0.0f;
        float    lfoHz       = 5.0f;
        uint8_t  lfoWave     = 0;
        bool     portamento  = false;
        float    portaTime   = 0.1f;
        float    masterVol   = 1.0f;
        uint8_t  ampA=3, ampD=30, ampS=100, ampR=25;
        uint8_t  fltA=5, fltD=50, fltS=70, fltR=40;
        uint8_t  overdrive   = 0;
    } params;

    LFO lfo;

    void init() {
        _spinlock = spin_lock_init(spin_lock_claim_unused(true));
        _ageCounter = 0;
        memset(_noteMap, 0xFF, sizeof(_noteMap));
        lfo.setRate(params.lfoHz);
        for (int i = 0; i < MAX_ACTIVE_VOICES; i++) {
            _voices[i].active    = false;
            _voices[i].releasing = false;
            _voices[i].ampEnv.setFromCC(params.ampA,params.ampD,params.ampS,params.ampR);
            _voices[i].filterEnv.setFromCC(params.fltA,params.fltD,params.fltS,params.fltR);
        }
    }

    // ---- Core 0: MIDI handlers -----------------------------

    void noteOn(uint8_t note, uint8_t vel) {
        (void)vel;

        // If this note is already sounding (non-releasing), re-trigger it
        uint8_t existing = _noteMap[note];
        if (existing != 0xFF && _voices[existing].active && !_voices[existing].releasing) {
            _voices[existing].ampEnv.noteOn();
            _voices[existing].filterEnv.noteOn();
            _voices[existing].age = ++_ageCounter;
            return;
        }

        // Find best slot
        uint32_t save = spin_lock_blocking(_spinlock);
        int slot = _findBestSlot();

        // If the slot was previously mapped to another note, unmap it
        if (_voices[slot].active || _voices[slot].releasing) {
            uint8_t oldNote = _voices[slot].note;
            if (oldNote < 128 && _noteMap[oldNote] == slot)
                _noteMap[oldNote] = 0xFF;
        }

        // Silence immediately before re-init to prevent click
        _voices[slot].ampEnv.forceIdle();
        _voices[slot].active    = false;  // hide from core 1
        _voices[slot].releasing = false;
        __dmb();
        spin_unlock(_spinlock, save);

        // Full init on core 0 — powf safe here
        _voices[slot].noteOn(note, params);
        _voices[slot].age = ++_ageCounter;

        // Map note → slot
        _noteMap[note] = (uint8_t)slot;

        __dmb();
        _voices[slot].active = true;  // publish to core 1
    }

    void noteOff(uint8_t note) {
        uint8_t slot = _noteMap[note];
        if (slot == 0xFF) return;  // never played, or already stolen

        // Only trigger release if this slot is still playing this note
        if (_voices[slot].note == note && _voices[slot].active && !_voices[slot].releasing) {
            _voices[slot].noteOff();
            _voices[slot].releasing = true;
            // Keep _noteMap entry until voice goes fully inactive
            // (so we don't accidentally double-noteOff)
        }
        // Always unmap — we've handled it
        _noteMap[note] = 0xFF;
    }

    void allNotesOff() {
        for (int i = 0; i < MAX_ACTIVE_VOICES; i++) {
            _voices[i].noteOff();
            _voices[i].releasing = true;
        }
        memset(_noteMap, 0xFF, sizeof(_noteMap));
    }

    void pitchBend(int16_t val) {
        float semi = val * (2.0f / 8192.0f);
        params.pitchBend = semi;
        for (int i = 0; i < MAX_ACTIVE_VOICES; i++)
            if (_voices[i].active) {
                _voices[i].vco1.setPitchBend(semi);
                _voices[i].vco2.setPitchBend(semi);
            }
    }

    void handleCC(uint8_t cc, uint8_t val) {
        float f = val / 127.0f;
        // AKAI MPK mini knob remapping
        switch (cc) {
            case AKAI_K1: cc = CC_VCF_CUTOFF;    break;
            case AKAI_K2: cc = CC_VCF_RESONANCE; break;
            case AKAI_K3: cc = CC_FLT_ENV_AMT;   break;
            case AKAI_K4: cc = CC_AMP_ATTACK;    break;
            case AKAI_K5: cc = CC_AMP_RELEASE;   break;
            case AKAI_K6: cc = CC_OVERDRIVE;     break;
            case AKAI_K8: cc = CC_OSC_MIX;       break;
            default: break;
        }
        switch (cc) {
            case CC_VCF_CUTOFF:    params.vcfCutoffCC   = val;                 break;
            case CC_VCF_RESONANCE: params.vcfRes        = f * 0.95f;           break;
            case CC_AMP_ATTACK:    params.ampA = val; _syncADSR();             break;
            case CC_AMP_DECAY:     params.ampD = val; _syncADSR();             break;
            case CC_AMP_SUSTAIN:   params.ampS = val; _syncADSR();             break;
            case CC_AMP_RELEASE:   params.ampR = val; _syncADSR();             break;
            case CC_FLT_ATTACK:    params.fltA = val; _syncADSR();             break;
            case CC_FLT_DECAY:     params.fltD = val; _syncADSR();             break;
            case CC_FLT_SUSTAIN:   params.fltS = val; _syncADSR();             break;
            case CC_FLT_RELEASE:   params.fltR = val; _syncADSR();             break;
            case CC_FLT_ENV_AMT:   params.fltEnvAmt     = f;                   break;
            case CC_OSC_DETUNE:    params.osc2Detune    = (f-0.5f)*100.0f;     break;
            case CC_OSC_WAVE:      params.osc1Wave = params.osc2Wave = val>>5;  break;
            case CC_OSC_MIX:       params.oscMix        = f;                   break;
            case CC_NOISE_MIX:     params.noiseMix      = f * 0.4f;            break;
            case CC_MASTER_VOL:    params.masterVol     = f;                   break;
            case CC_PORTAMENTO:    params.portamento    = (val >= 64);          break;
            case CC_PORTA_TIME:    params.portaTime     = f * 3.0f;            break;
            case CC_MOD_WHEEL:     params.lfoVibratoAmt = f * 2.0f;            break;
            case CC_LFO_RATE:      { float hz=0.1f*powf(200.0f,f);
                                     lfo.setRate(hz); params.lfoHz=hz; }        break;
            case CC_LFO_WAVE:      params.lfoWave=(val>=64)?1:0;
                                   lfo.setWave(params.lfoWave);                 break;
            case CC_OVERDRIVE:     params.overdrive     = val;                  break;
            case CC_ALL_NOTES_OFF: allNotesOff();                               break;
            default: break;
        }
    }

    // ---- Core 1: lock-free audio tick ----------------------
    inline int32_t tick() {
        float lfoVal  = lfo.tick();
        params.lfoValue = lfoVal;
        float lfoSemi = params.lfoVibratoAmt * lfoVal;

        int32_t sum = 0;
        for (int i = 0; i < MAX_ACTIVE_VOICES; i++) {
            if (!_voices[i].active) continue;
            _voices[i].applyLFO(lfoSemi);
            sum += _voices[i].tick(params);
            // When ADSR reaches idle, clear active + releasing
            if (_voices[i].ampEnv.isIdle()) {
                _voices[i].active    = false;
                _voices[i].releasing = false;
                _voices[i].note      = 255;  // mark as free
            }
        }

        // Soft overdrive
        if (params.overdrive > 4) {
            float drive = 1.0f + params.overdrive * (3.0f / 127.0f);
            float s = sum * drive * (1.0f / 32767.0f);
            if (s >  3.0f) s =  3.0f;
            if (s < -3.0f) s = -3.0f;
            sum = (int32_t)(s*(27.0f+s*s)/(27.0f+9.0f*s*s) * 32767.0f);
        }
        if (sum >  32767) sum =  32767;
        if (sum < -32767) sum = -32767;
        return sum;
    }

    // ---- Diagnostics (core 0) ------------------------------
    uint8_t activeVoiceCount() const {
        uint8_t n = 0;
        for (int i = 0; i < MAX_ACTIVE_VOICES; i++)
            if (_voices[i].active) n++;
        return n;
    }
    bool isNotePlaying(uint8_t note) const {
        return _noteMap[note] != 0xFF;
    }
    bool voiceActive(int i) const {
        return (i >= 0 && i < MAX_ACTIVE_VOICES) ? (bool)_voices[i].active : false;
    }
    bool voiceReleasing(int i) const {
        return (i >= 0 && i < MAX_ACTIVE_VOICES) ? (bool)_voices[i].releasing : false;
    }
    uint8_t voiceNote(int i) const {
        return (i >= 0 && i < MAX_ACTIVE_VOICES) ? _voices[i].note : 0;
    }

private:
    AnalogVoice  _voices[MAX_ACTIVE_VOICES];
    spin_lock_t* _spinlock   = nullptr;
    uint32_t     _ageCounter = 0;
    uint8_t      _noteMap[128];   // note → voice slot, 0xFF = unassigned

    // Priority: free > releasing > oldest active
    int _findBestSlot() {
        // 1. Completely free slot
        for (int i = 0; i < MAX_ACTIVE_VOICES; i++)
            if (!_voices[i].active && !_voices[i].releasing) return i;

        // 2. Releasing slot (quietest — steal the one furthest into release)
        int relSlot = -1;
        float lowestAmpEnv = 1.0f;
        for (int i = 0; i < MAX_ACTIVE_VOICES; i++) {
            if (_voices[i].releasing) {
                float lvl = _voices[i].ampEnv.level();
                if (lvl < lowestAmpEnv) { lowestAmpEnv = lvl; relSlot = i; }
            }
        }
        if (relSlot >= 0) return relSlot;

        // 3. Steal oldest active (last resort)
        int oldest = 0;
        uint32_t minAge = _voices[0].age;
        for (int i = 1; i < MAX_ACTIVE_VOICES; i++)
            if (_voices[i].age < minAge) { minAge = _voices[i].age; oldest = i; }
        return oldest;
    }

    void _syncADSR() {
        for (int i = 0; i < MAX_ACTIVE_VOICES; i++) {
            _voices[i].ampEnv.setFromCC(params.ampA,params.ampD,params.ampS,params.ampR);
            _voices[i].filterEnv.setFromCC(params.fltA,params.fltD,params.fltS,params.fltR);
        }
    }
};