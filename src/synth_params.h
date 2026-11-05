#pragma once
// ============================================================
//  synth_params.h  —  every user-facing parameter, in one table
//
//  The UI, MIDI, serial and patches all talk to the engine through
//  SynthManager::setParam(id, 0..127). Nothing outside the manager
//  touches engine values directly, and nothing needs to know a CC number
//  except the table below (so the panel can never be "re-routed" by the
//  AKAI remap again).
// ============================================================
#include <stdint.h>
#include "config.h"

// Order = left-to-right, top-to-bottom order of the knobs on the panel
// (that is also the joystick LEFT/RIGHT navigation order).
enum ParamId : uint8_t {
    P_WAVE, P_DETUNE, P_OSCMIX, P_NOISE,          // oscillators
    P_CUTOFF, P_RES, P_FENV,                      // filter
    P_AATK, P_ADEC, P_ASUS, P_AREL,               // amp envelope
    P_FATK, P_FDEC, P_FSUS, P_FREL,               // filter envelope
    P_VOL, P_DRIVE, P_LFORATE, P_MOD, P_GLIDE,    // master / fx / modulation
    P_LFOWAVE,                                    // (no knob: CC78 / serial only)
    N_PARAMS
};
#define N_KNOBS  (P_LFOWAVE)     // params that have a knob on the panel

enum ParamGroup : uint8_t { G_OSC, G_FLT, G_AENV, G_FENV, G_FX };

struct ParamInfo {
    const char* label;    // knob label (<= 4 chars)
    const char* name;     // name shown in the top bar
    const char* cmd;      // serial name:  set <cmd> <0-127>
    uint8_t     cc;       // primary MIDI CC
    uint8_t     group;    // colour group
    uint8_t     col;      // knob grid column 0..7  (0xFF = no knob)
    uint8_t     row;      // knob grid row 0..2
};

static const ParamInfo PARAM_INFO[N_PARAMS] = {
    //  label   name           cmd      cc                 group   col row
    { "WAVE", "Waveform",    "wave",  CC_OSC_WAVE,       G_OSC,  0, 0 },
    { "DET",  "Detune",      "det",   CC_OSC_DETUNE,     G_OSC,  1, 0 },
    { "MIX",  "Osc Mix",     "mix",   CC_OSC_MIX,        G_OSC,  2, 0 },
    { "NOI",  "Noise",       "noise", CC_NOISE_MIX,      G_OSC,  3, 0 },
    { "CUT",  "Cutoff",      "cut",   CC_VCF_CUTOFF,     G_FLT,  4, 0 },
    { "RES",  "Resonance",   "res",   CC_VCF_RESONANCE,  G_FLT,  5, 0 },
    { "FENV", "Flt Env Amt", "fenv",  CC_FLT_ENV_AMT,    G_FLT,  6, 0 },
    { "ATK",  "Amp Attack",  "aatk",  CC_AMP_ATTACK,     G_AENV, 0, 1 },
    { "DEC",  "Amp Decay",   "adec",  CC_AMP_DECAY,      G_AENV, 1, 1 },
    { "SUS",  "Amp Sustain", "asus",  CC_AMP_SUSTAIN,    G_AENV, 2, 1 },
    { "REL",  "Amp Release", "arel",  CC_AMP_RELEASE,    G_AENV, 3, 1 },
    { "ATK",  "Flt Attack",  "fatk",  CC_FLT_ATTACK,     G_FENV, 4, 1 },
    { "DEC",  "Flt Decay",   "fdec",  CC_FLT_DECAY,      G_FENV, 5, 1 },
    { "SUS",  "Flt Sustain", "fsus",  CC_FLT_SUSTAIN,    G_FENV, 6, 1 },
    { "REL",  "Flt Release", "frel",  CC_FLT_RELEASE,    G_FENV, 7, 1 },
    { "VOL",  "Volume",      "vol",   CC_MASTER_VOL,     G_FX,   0, 2 },
    { "DRV",  "Drive",       "drive", CC_OVERDRIVE,      G_FX,   1, 2 },
    { "LFO",  "LFO Rate",    "lfor",  CC_LFO_RATE,       G_FX,   2, 2 },
    { "MOD",  "Mod Depth",   "mod",   CC_MOD_DEPTH,      G_FX,   3, 2 },
    { "GLD",  "Glide Time",  "glide", CC_GLIDE_TIME,     G_FX,   4, 2 },
    { "LFOW", "LFO Wave",    "lfow",  CC_LFO_WAVE,       G_FX,   0xFF, 0xFF },
};

// ---- Patches: a COMPLETE set of values each (no state leaks between patches) ----
//   Waveform values are the centre of each quarter: sin 16, tri 48, saw 80, sqr 112.
#define N_PATCHES 3
static const char* const PATCH_NAMES[N_PATCHES] = { "Init", "Pad", "Bass" };
static const uint8_t PATCHES[N_PATCHES][N_PARAMS] = {
    //  WAVE DET MIX NOI | CUT RES FENV | AATK ADEC ASUS AREL | FATK FDEC FSUS FREL | VOL DRV LFOR MOD GLD | LFOW
    {    80, 64,  0,  0,    60, 38,  90,    5,  50,  80,  30,    3,  60,  25,  40,    80,  0,  94,  0, 20,    0 },  // Init : saw, snappy
    {    48, 70, 64,  0,    70, 15,  60,   60,  60, 100,  70,   50,  50,  80,  60,    80,  0,  80, 15, 20,    0 },  // Pad  : tri + detuned osc2, slow
    {   112, 66,  0,  0,    50, 40, 100,    2,  40, 100,  20,    2,  30,  40,  20,    80,  0,  94,  0, 20,    0 },  // Bass : square, resonant sweep
};

// ---- CC -> parameter lookup ----------------------------------------------------
// Returns the parameter a CC controls, or 0xFF. The AKAI knobs are checked first so
// that CC1..CC8 always mean "the 8 knobs", exactly as before.
static inline uint8_t paramForCC(uint8_t cc) {
#if AKAI_REMAP
    switch (cc) {
        case AKAI_K1: return P_CUTOFF;
        case AKAI_K2: return P_RES;
        case AKAI_K3: return P_FENV;
        case AKAI_K4: return P_AATK;
        case AKAI_K5: return P_AREL;
        case AKAI_K6: return P_DRIVE;
        case AKAI_K7: return P_VOL;
        case AKAI_K8: return P_OSCMIX;
        default: break;
    }
#else
    if (cc == 1) return P_MOD;           // GM mod wheel
    if (cc == 5) return P_GLIDE;         // GM portamento time
#endif
    for (uint8_t i = 0; i < N_PARAMS; i++)
        if (PARAM_INFO[i].cc == cc) return i;
    return 0xFF;
}
