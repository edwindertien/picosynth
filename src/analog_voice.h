#pragma once
// ============================================================
//  analog_voice.h  –  One polyphonic voice
//
//  Core 0: noteOn() — all expensive setup (powf, filter coeffs)
//  Core 1: tick()   — pure arithmetic only, no powf/tanf/lock
//
//  updatePitch() applies pitch bend + LFO cheaply on core 1
//  using VCO's applyLFO() (linear approximation, no powf).
// ============================================================
#include "config.h"
#include "vco.h"
#include "vcf.h"
#include "adsr.h"

class AnalogVoice {
public:
    VCO  vco1, vco2;
    VCF  vcf;
    ADSR ampEnv, filterEnv;
    volatile bool active    = false;
    bool     releasing = false;
    uint8_t  note      = 0;
    uint32_t age    = 0;

    // Core 0: full initialisation at note start
    template<typename P>
    void noteOn(uint8_t n, const P& p) {
        note = n;
        vco1.setWave(p.osc1Wave);
        vco2.setWave(p.osc2Wave);
        vco1.setPortamento(p.portamento ? p.portaTime : 0.0f);
        vco2.setPortamento(p.portamento ? p.portaTime : 0.0f);
        vco1.setNote(n, 0.0f);
        vco2.setNote(n, p.osc2Detune);
        // Apply current pitch bend on core 0 (uses _pow2semi, safe)
        vco1.setPitchBend(p.pitchBend);
        vco2.setPitchBend(p.pitchBend);
        ampEnv.setFromCC(p.ampA, p.ampD, p.ampS, p.ampR);
        filterEnv.setFromCC(p.fltA, p.fltD, p.fltS, p.fltR);
        ampEnv.noteOn();
        filterEnv.noteOn();
        vcf.reset();
        vcf.setResonance(p.vcfRes);
        vcf.setCutoffFromCC(p.vcfCutoffCC, p.fltEnvAmt, 0.0f);
    }

    void noteOff() {
        ampEnv.noteOff();
        filterEnv.noteOff();
    }

    // Core 1: apply LFO vibrato cheaply (linear approx, no powf)
    // Pitch bend is handled by core 0 via setPitchBend → _baseInc
    inline void applyLFO(float lfoSemi) {
        vco1.applyLFO(lfoSemi);
        vco2.applyLFO(lfoSemi);
    }

    // Core 1: full per-sample processing
    template<typename P>
    inline int32_t tick(const P& p) {
        if (!active) return 0;

        float o1  = vco1.tick() * (1.0f / 32767.0f);
        float o2  = vco2.tick() * (1.0f / 32767.0f);
        float mix = o1 + (o2 - o1) * p.oscMix;

        if (p.noiseMix > 0.001f) {
            _seed = _seed * 1664525u + 1013904223u;
            float noise = (int32_t)_seed * (1.0f / 2147483648.0f);
            mix += noise * p.noiseMix;
        }

        float fEnv = filterEnv.tick();
        float aEnv = ampEnv.tick();

        vcf.setCutoffFromCC(p.vcfCutoffCC, p.fltEnvAmt, fEnv);
        float out = vcf.tick(mix) * aEnv * p.masterVol;

        if (ampEnv.isIdle()) active = false;

        return (int32_t)(out * 28000.0f);
    }

private:
    uint32_t _seed = 0xDEADBEEFu;
};