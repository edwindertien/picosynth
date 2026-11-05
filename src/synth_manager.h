#pragma once
// ============================================================
//  synth_manager.h  —  polyphony, parameters and the audio tick
//
//  CORE 0  (loop(): MIDI, USB host, UI, serial)  — everything except tick()
//  CORE 1  (loop1(): audio)                      — tick() only
//
//  There is deliberately NO spinlock in this file:
//    * every note/CC/UI call comes from loop() on core 0 (single thread);
//    * core 0 only initialises voices that core 1 has handed back (live == 0)
//      and publishes them with a barrier;
//    * for live voices core 0 only posts release requests (see synth_voice.h).
//  So core 1 can never wait on core 0, and a CC or key press can never corrupt a
//  voice that is being rendered.
//
//  VOICE ALLOCATION — "the new note always wins"
//    1. If MAX_ACTIVE_VOICES are already sounding, pick victims: the quietest
//       voice whose key is already up, otherwise the oldest held one, and fade
//       them out with a short fast-release (STEAL_RELEASE_MS). Faded voices stop
//       counting immediately but keep their slot until silent.
//    2. Start the new note in a FREE slot. SYNTH_POOL > MAX_ACTIVE_VOICES
//       guarantees one exists, so the new note never waits and nothing is ever
//       overwritten mid-sound (no clicks, no races).
// ============================================================
#include <Arduino.h>
#include <hardware/sync.h>
#include "config.h"
#include "wavetable.h"
#include "synth_params.h"
#include "synth_voice.h"
#include "effects.h"

static_assert(SYNTH_POOL <= 32, "tick() keeps one live bit per voice in a uint32_t");
static_assert(SYNTH_POOL > MAX_ACTIVE_VOICES, "the pool needs spare slots for voices that are fading out");

class SynthManager {
public:
    // ---- state read by the UI (core 0) -------------------------------------
    uint8_t      pv[N_PARAMS];          // knob values 0..127
    EngineParams ep;                    // derived engine values (core 1 reads)
    uint8_t      masterVolume = 160;    // 0..255
    Overdrive    overdrive;
    const char*  patchName = "Init";

    void init() {
        for (int i = 0; i < 128; i++)
            _noteInc[i] = hzToInc(440.0f * powf(2.0f, ((float)i - 69.0f) / 12.0f));
        for (int i = 0; i < 128; i++) _map[i] = NONE;
        for (int i = 0; i < SYNTH_POOL; i++) {
            _voices[i].live = 0;
            _held[i] = _dying[i] = false;
            _note[i] = 255;
            _age[i]  = 0;
        }
        _clock = 0; _lastInc = 0.0f; _gctr = 0; _modRatio = 1.0f;
        loadPatch(0);
    }

    // =======================================================================
    //  Parameters  (core 0)
    // =======================================================================
    void setParam(uint8_t id, uint8_t val) {
        if (id >= N_PARAMS) return;
        if (val > 127) val = 127;
        pv[id] = val;
        patchName = "Custom";
        _derive(id);
    }

    void loadPatch(uint8_t idx) {
        idx %= N_PATCHES;
        for (uint8_t i = 0; i < N_PARAMS; i++) { pv[i] = PATCHES[idx][i]; _derive(i); }
        patchName = PATCH_NAMES[idx];
        _patchIdx = idx;
    }
    uint8_t patchIndex() const { return _patchIdx; }

    void setGlide(bool on) { ep.glideOn = on; }
    bool glide() const     { return ep.glideOn; }

    // MIDI CC. Returns true if the CC was used.
    bool handleCC(uint8_t cc, uint8_t val) {
        if (val > 127) val = 127;
        switch (cc) {
            case CC_GLIDE_ONOFF:  setGlide(val >= 64); return true;
            case CC_ALL_NOTES_OFF: allNotesOff();      return true;
            case CC_ALL_SOUND_OFF: panic();            return true;
            default: break;
        }
        const uint8_t id = paramForCC(cc);
        if (id == 0xFF) return false;
        setParam(id, val);
        return true;
    }

    void pitchBend(int16_t bend) {                 // -8192..+8191  ->  +-2 semitones
        ep.bend = (float)bend * (2.0f / 8192.0f);
    }

    // =======================================================================
    //  Notes  (core 0)
    // =======================================================================
    void noteOn(uint8_t note, uint8_t /*velocity*/) {
        if (note > 127) return;

        // Same key pressed again without a note-off (lost message): retire the old voice.
        if (_map[note] != NONE) _retire(_map[note]);

        // Glide = mono legato: re-pitch the sounding held voice, don't start another.
        if (ep.glideOn) {
            const int mv = _youngestHeld();
            if (mv >= 0) {
                _voices[mv].tgtInc = _noteInc[note];
                _lastInc = _noteInc[note];
                if (_note[mv] < 128 && _map[_note[mv]] == mv) _map[_note[mv]] = NONE;
                _note[mv] = note;
                _map[note] = (uint8_t)mv;
                _age[mv] = ++_clock;
                return;
            }
        }

        // Polyphony cap: fade out victims. The new note always wins.
        while (_countCounted() >= MAX_ACTIVE_VOICES) {
            const int v = _pickVictim();
            if (v < 0) break;
            _retire(v);
        }

        int slot = _findFree();
        if (slot < 0) slot = _waitFree();
        if (slot < 0) return;                      // cannot happen unless core 1 is not running
        _start(slot, note);
    }

    void noteOff(uint8_t note) {
        if (note > 127) return;
        const uint8_t v = _map[note];
        if (v == NONE) return;                     // stolen, or never started
        _map[note] = NONE;
        if (_held[v]) {
            _held[v] = false;
            _voices[v].relReq = (uint8_t)(_voices[v].relReq + 1);
        }
    }

    // Release every held note (normal release tails).
    void allNotesOff() {
        for (int i = 0; i < SYNTH_POOL; i++) {
            if (_voices[i].live && _held[i]) {
                _held[i] = false;
                _voices[i].relReq = (uint8_t)(_voices[i].relReq + 1);
            }
        }
        for (int i = 0; i < 128; i++) _map[i] = NONE;
    }

    // Silence everything within STEAL_RELEASE_MS (panic).
    void panic() {
        for (int i = 0; i < SYNTH_POOL; i++) if (_voices[i].live) _retire(i);
        for (int i = 0; i < 128; i++) _map[i] = NONE;
    }

    // =======================================================================
    //  Audio  (core 1)  —  lock-free, no libm, no flash-resident callees
    // =======================================================================
    HOT int16_t tick() {
        if (_gctr == 0) _globalControl();
        if (++_gctr >= CONTROL_DIV) _gctr = 0;

        // 1. Which voices has core 0 published?   2. Acquire barrier AFTER reading the flags
        //    (pairs with the barrier before `live = 1` in _start), so everything core 0 wrote
        //    to a voice before publishing it is visible.   3. Render only those voices.
        uint32_t live = 0;
        for (int i = 0; i < SYNTH_POOL; i++) live |= (uint32_t)(_voices[i].live != 0) << i;
        __dmb();

        int32_t mix = 0;
        while (live) {
            const int i = __builtin_ctz(live);
            live &= live - 1;
            mix += _voices[i].render(ep, _modRatio);
        }

        mix = overdrive.process(mix);              // asymmetric tube drive (shared with the organ)
        mix = (mix * (int32_t)masterVolume) / 255;
        if (mix >  32767) mix =  32767;
        if (mix < -32768) mix = -32768;
        return (int16_t)mix;
    }

    // =======================================================================
    //  Diagnostics (core 0)
    // =======================================================================
    uint8_t activeCount() const {
        uint8_t n = 0;
        for (int i = 0; i < SYNTH_POOL; i++) if (_voices[i].live) n++;
        return n;
    }
    uint8_t countedVoices() const { return (uint8_t)_countCounted(); }   // excludes fading ones
    bool        voiceLive(int i)  const { return _voices[i].live; }
    bool        voiceHeld(int i)  const { return _held[i]; }
    bool        voiceDying(int i) const { return _dying[i]; }
    uint8_t     voiceNote(int i)  const { return _note[i]; }
    float       voiceLevel(int i) const { return _voices[i].level(); }
    const char* voiceState(int i) const { return _voices[i].aenv.name(); }
    float       modRatio()        const { return _modRatio; }

private:
    static constexpr uint8_t NONE = 0xFF;

    SynthVoice _voices[SYNTH_POOL];
    // core-0 bookkeeping (never read by core 1)
    bool       _held[SYNTH_POOL];     // key is down
    bool       _dying[SYNTH_POOL];    // fast release requested
    uint8_t    _note[SYNTH_POOL];
    uint32_t   _age[SYNTH_POOL];
    uint8_t    _map[128];             // note -> slot of the voice whose key is down
    float      _noteInc[128];
    uint32_t   _clock   = 0;
    float      _lastInc = 0.0f;
    uint8_t    _patchIdx = 0;

    // core-1 state
    uint8_t    _gctr = 0;
    float      _modRatio = 1.0f;
    uint32_t   _lfoPhase = 0;
    uint32_t   _lfoRng   = 12345u;
    float      _lfoSH    = 0.0f;

    // ---- core 1: once per control block -----------------------------------
    HOT void _globalControl() {
        float lfo;
        const uint32_t prev = _lfoPhase;
        _lfoPhase += ep.lfoInc * (uint32_t)CONTROL_DIV;
        if (ep.lfoWave == 0) {
            lfo = (float)Wavetable::sine(_lfoPhase >> PHASE_SHIFT) * (1.0f / 32767.0f);
        } else {
            if (_lfoPhase < prev) {
                _lfoRng = _lfoRng * 1664525u + 1013904223u;
                _lfoSH  = (float)(int32_t)_lfoRng * (1.0f / 2147483648.0f);
            }
            lfo = _lfoSH;
        }
        // pitch ratio 2^(semitones/12) via 5th-order series (|semi| <= 4: error < 3e-7)
        const float semi = ep.modDepth * lfo + ep.bend;
        const float a    = semi * (0.693147181f / 12.0f);
        _modRatio = 1.0f + a * (1.0f + a * (0.5f + a * (1.0f / 6.0f + a * (1.0f / 24.0f + a * (1.0f / 120.0f)))));
    }

    // ---- core 0: allocation ------------------------------------------------
    int _countCounted() const {
        int n = 0;
        for (int i = 0; i < SYNTH_POOL; i++) if (_voices[i].live && !_dying[i]) n++;
        return n;
    }

    int _findFree() const {
        for (int i = 0; i < SYNTH_POOL; i++) if (!_voices[i].live) return i;
        return -1;
    }

    // Quietest voice whose key is already up; else the oldest held voice.
    int _pickVictim() const {
        int best = -1; float lvl = 2.0f;
        for (int i = 0; i < SYNTH_POOL; i++) {
            if (_voices[i].live && !_dying[i] && !_held[i]) {
                const float l = _voices[i].level();
                if (l < lvl) { lvl = l; best = i; }
            }
        }
        if (best >= 0) return best;
        uint32_t oldest = 0xFFFFFFFFu;
        for (int i = 0; i < SYNTH_POOL; i++) {
            if (_voices[i].live && !_dying[i] && _age[i] < oldest) { oldest = _age[i]; best = i; }
        }
        return best;
    }

    int _youngestHeld() const {
        int best = -1; uint32_t newest = 0;
        for (int i = 0; i < SYNTH_POOL; i++)
            if (_voices[i].live && _held[i] && !_dying[i] && _age[i] >= newest) { newest = _age[i]; best = i; }
        return best;
    }

    // Fast-release a voice and forget its key.
    void _retire(int v) {
        if (_held[v] && _note[v] < 128 && _map[_note[v]] == v) _map[_note[v]] = NONE;
        _held[v]  = false;
        _dying[v] = true;
        _voices[v].fastReq = (uint8_t)(_voices[v].fastReq + 1);
    }

    // Every slot is still sounding (a burst of steals): fast-released voices are silent
    // within STEAL_RELEASE_MS, so wait for one. Bounded; only reachable in extreme bursts.
    int _waitFree() {
        const uint32_t t0 = micros();
        while ((uint32_t)(micros() - t0) < (uint32_t)(STEAL_RELEASE_MS * 1000 + 4000)) {
            const int s = _findFree();
            if (s >= 0) return s;
            delayMicroseconds(50);
        }
        return -1;
    }

    void _start(int slot, uint8_t note) {
        __dmb();   // acquire: pairs with the barrier core 1 executes before it clears `live`,
                   // so the previous occupant's last accesses are complete before we overwrite it
        const float tgt  = _noteInc[note];
        const float from = (ep.glideOn && _lastInc > 0.0f) ? _lastInc : tgt;
        _note[slot] = note;
        _age[slot]  = ++_clock;
        _held[slot] = true;
        _dying[slot] = false;
        _map[note]  = (uint8_t)slot;
        _lastInc    = tgt;
        _voices[slot].start(from, tgt, ep, _modRatio, _clock * 2654435761u);
        __dmb();                                   // everything above visible before...
        _voices[slot].live = 1;                    // ...core 1 starts rendering it
    }

    // ---- core 0: knob value -> engine value ----------------------------------
    void _derive(uint8_t id) {
        const float f = pv[id] / 127.0f;
        switch (id) {
            case P_WAVE:    ep.wave = (uint8_t)((pv[id] >> 5) & 3); break;
            case P_DETUNE:  ep.detuneRatio = powf(2.0f, ((float)pv[id] - 64.0f) * (50.0f / 64.0f) / 1200.0f); break;
            case P_OSCMIX:  ep.oscMix   = f;            break;
            case P_NOISE:   ep.noiseMix = f * 0.4f;     break;
            case P_CUTOFF:  ep.cutHz    = 30.0f * powf(2.0f, pv[id] / 18.0f); break;
            case P_RES:     ep.res4     = f * 0.95f * 4.0f; break;
            case P_FENV:    ep.fenv     = f;            break;
            case P_AATK:    ep.aAtk = envRateFromCC(pv[id]); break;
            case P_ADEC:    ep.aDec = envRateFromCC(pv[id]); break;
            case P_ASUS:    ep.aSus = f;                break;
            case P_AREL:    ep.aRel = envRateFromCC(pv[id]); break;
            case P_FATK:    ep.fAtk = envRateFromCC(pv[id]); break;
            case P_FDEC:    ep.fDec = envRateFromCC(pv[id]); break;
            case P_FSUS:    ep.fSus = f;                break;
            case P_FREL:    ep.fRel = envRateFromCC(pv[id]); break;
            case P_VOL:     masterVolume = (uint8_t)((uint16_t)pv[id] * 255 / 127); break;
            case P_DRIVE:   overdrive.drive = pv[id];   break;
            case P_LFORATE: ep.lfoInc  = (uint32_t)hzToInc(0.1f * powf(200.0f, f)); break;
            case P_MOD:     ep.modDepth = f * MOD_MAX_SEMITONES; break;
            case P_GLIDE: {
                const float T   = 0.005f + f * 3.0f;                 // seconds to settle
                const float tau = T / 3.0f;
                ep.glideCoef = 1.0f - expf(-(float)CONTROL_DIV / ((float)SAMPLE_RATE * tau));
                break;
            }
            case P_LFOWAVE: ep.lfoWave = (pv[id] >= 64) ? 1 : 0; break;
            default: break;
        }
    }
};
