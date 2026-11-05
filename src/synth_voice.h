#pragma once
// ============================================================
//  synth_voice.h  —  one polyphonic voice
//
//    osc1 --+
//    osc2 --+-- mix --+ filter --> x amp --> out        (noise can be mixed in)
//
//  THREADING CONTRACT  (this is what keeps notes from hanging)
//    * `live == 0`  : the voice belongs to core 0. Core 0 may write ANY field,
//                     then publishes with  __dmb(); live = 1;
//    * `live == 1`  : the voice belongs to core 1. Core 0 may only
//                       - increment relReq / fastReq   (ask for a release)
//                       - write tgtInc                  (glide retarget)
//                       - read level()
//                     Core 1 sets `live = 0` itself, as the very last thing it does
//                     with the voice, once the envelope has finished.
//    Every cross-core field is a single aligned word, written by exactly one core
//    per phase, so no lock is needed and neither core can ever block the other.
// ============================================================
#include <stdint.h>
#include <hardware/sync.h>        // __dmb()
#include "config.h"
#include "wavetable.h"
#include "synth_osc.h"
#include "synth_env.h"
#include "synth_filter.h"

// Fast-release rate used when a voice is stolen (level change per sample).
#define STEAL_RATE  (1000.0f / ((float)STEAL_RELEASE_MS * (float)SAMPLE_RATE))

// Engine values derived from the knobs by core 0 (SynthManager::_derive).
// Every field is one word; core 1 only reads.
struct EngineParams {
    uint8_t  wave        = WAVE_SAW;
    float    detuneRatio = 1.0f;     // osc2 / osc1
    float    oscMix      = 0.0f;     // 0 = osc1 only .. 1 = osc2 only
    float    noiseMix    = 0.0f;
    float    cutHz       = 800.0f;   // base cutoff (nominal Hz)
    float    res4        = 0.0f;     // ladder feedback gain
    float    fenv        = 0.5f;     // filter envelope amount 0..1
    float    aAtk = 0.01f, aDec = 0.001f, aSus = 1.0f, aRel = 0.001f;   // per-sample rates / level
    float    fAtk = 0.01f, fDec = 0.001f, fSus = 1.0f, fRel = 0.001f;
    float    glideCoef   = 1.0f;     // per control tick
    bool     glideOn     = false;
    float    modDepth    = 0.0f;     // vibrato depth, semitones
    float    bend        = 0.0f;     // pitch bend, semitones
    uint32_t lfoInc      = 0;
    uint8_t  lfoWave     = 0;        // 0 sine, 1 sample&hold
};

struct SynthVoice {
    // ---- shared between cores (see contract above) --------------------------
    volatile uint8_t live    = 0;
    volatile uint8_t relReq  = 0;
    volatile uint8_t fastReq = 0;
    volatile float   tgtInc  = 0.0f;       // target phase increment (glide)

    // ---- state: core 0 while !live, core 1 while live ------------------------
    SynthOsc    osc1, osc2;
    SynthFilter filter;
    SynthEnv    aenv, fenv;
    float       curInc = 0.0f;             // current (gliding) increment
    float       amp = 0.0f, ampTgt = 0.0f, ampStep = 0.0f;
    float       mixB = 0.0f, noiseAmt = 0.0f;
    uint32_t    seed = 1;
    uint8_t     relSeen = 0, fastSeen = 0, ctr = 1;
    bool        useOsc2 = false, useNoise = false;

    // core 0 may read this for voice-stealing decisions (a stale float is fine)
    float level() const { return aenv.level; }

    // ------------------------------------------------------------------------
    //  Core 0, only while live == 0
    // ------------------------------------------------------------------------
    void start(float fromInc, float toInc, const EngineParams& p, float modRatio, uint32_t noiseSeed) {
        osc1.phase = 0;
        osc2.phase = 0;
        curInc = fromInc;
        tgtInc = toInc;
        aenv.start(p.aAtk, p.aDec, p.aSus, p.aRel);
        fenv.start(p.fAtk, p.fDec, p.fSus, p.fRel);
        amp = ampTgt = ampStep = 0.0f;
        relSeen  = relReq;                 // forget requests made for the previous note
        fastSeen = fastReq;
        seed     = noiseSeed | 1u;
        filter.reset(SynthFilter::coeffForHz(p.cutHz), p.res4);
        control(p, modRatio);              // prime: pitch, tables, first envelope ramp
        ctr = CONTROL_DIV;
    }

    // ------------------------------------------------------------------------
    //  Core 1: once per CONTROL_DIV samples
    // ------------------------------------------------------------------------
    HOT void control(const EngineParams& p, float modRatio) {
        // 0. Finished? The envelope hit IDLE during the previous block and the amp ramp
        //    has played out to zero, so the voice is silent: hand it back to core 0.
        if (aenv.state == SynthEnv::IDLE) {
            __dmb();
            live = 0;                       // LAST access to this voice
            return;
        }

        // 1. Requests from core 0 (counters only ever increase, so nothing is lost)
        const uint8_t fr = fastReq;
        const uint8_t rr = relReq;
        if (fr != fastSeen) {
            fastSeen = fr; relSeen = rr;
            aenv.release(STEAL_RATE);
            fenv.release(STEAL_RATE);
        } else if (rr != relSeen) {
            relSeen = rr;
            aenv.release(p.aRel);
            fenv.release(p.fRel);
        }

        // 2. Envelopes and the amplitude ramp for the coming block
        aenv.advance(CONTROL_DIV);
        fenv.advance(CONTROL_DIV);
        amp     = ampTgt;
        ampTgt  = aenv.level;
        ampStep = (ampTgt - amp) * (1.0f / (float)CONTROL_DIV);

        // 3. Pitch: glide, vibrato / bend (modRatio) and detune
        const float ti = tgtInc;
        if (p.glideOn) curInc += (ti - curInc) * p.glideCoef;
        else           curInc  = ti;
        const float base = curInc * modRatio;
        osc1.inc = (uint32_t)base;
        osc2.inc = (uint32_t)(base * p.detuneRatio);

        // 4. Waveform (live: turning the WAVE knob changes held notes too)
        const int16_t* t = Wavetable::table(p.wave);
        osc1.tbl = t;
        osc2.tbl = t;

        // 5. Filter coefficient: base cutoff + envelope sweep, ramped over the block
        filter.res4 = p.res4;
        filter.setTarget(SynthFilter::coeffForHz(p.cutHz + p.fenv * fenv.level * 12000.0f));
        if (!filter.sane()) {               // numerical runaway (or NaN): restart the ladder
            filter.s1 = filter.s2 = filter.s3 = filter.s4 = 0.0f;
        }

        // 6. Mixer
        useOsc2  = p.oscMix   > 0.004f;
        mixB     = p.oscMix;
        useNoise = p.noiseMix > 0.004f;
        noiseAmt = p.noiseMix;
    }

    // ------------------------------------------------------------------------
    //  Core 1: once per sample. Returns the voice output (about +-VOICE_SCALE).
    // ------------------------------------------------------------------------
    HOT int32_t render(const EngineParams& p, float modRatio) {
        if (--ctr == 0) {
            ctr = CONTROL_DIV;
            control(p, modRatio);
            if (!live) return 0;
        }
        float sig = osc1.tick();
        if (useOsc2)  sig += (osc2.tick() - sig) * mixB;
        if (useNoise) {
            seed = seed * 1664525u + 1013904223u;
            sig += (float)(int32_t)seed * (1.0f / 2147483648.0f) * noiseAmt;
        }
        const float y = filter.tick(sig * 1.2f);
        amp += ampStep;
        return (int32_t)(y * amp * (float)VOICE_SCALE);
    }
};
