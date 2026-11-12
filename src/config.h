#pragma once
// ============================================================
//  config.h  —  Analog Synth, Pico 2 (RP2350)
//  PURE #defines only. No #includes, no code, no classes.
// ============================================================

// ---- Feature flags -----------------------------------------
#define ENABLE_LCD        1
#define ENABLE_SERIAL     1
#ifndef ENABLE_USB_HOST          // the *_nohost / pico_tone envs pass -DENABLE_USB_HOST=0
#define ENABLE_USB_HOST   1      // USB MIDI host on PIO0/PIO1 (audio lives on PIO2)
#endif

// ---- USB host pins -----------------------------------------
#define MIDI_HOST_DP_PIN  6      // D+ = GP6, D- = GP7 (automatic)

// ---- Polyphony ---------------------------------------------
// MAX_ACTIVE_VOICES : notes that may sound at once (held keys + release tails).
//                     When a new note arrives and this many are sounding, the
//                     quietest releasing voice (or else the oldest held one) is
//                     faded out in STEAL_RELEASE_MS and the NEW NOTE ALWAYS WINS.
// SYNTH_POOL        : physical voice slots. The extra (POOL - ACTIVE) slots are
//                     what lets the new note start immediately while the stolen
//                     voice is still fading. CPU worst case = POOL voices, but
//                     only for a few ms during a steal.
#define MAX_ACTIVE_VOICES  12
#define SYNTH_POOL         20
#define STEAL_RELEASE_MS    4

// ---- MIDI --------------------------------------------------
#define MIDI_CHANNEL        0    // 0 = OMNI
#define AKAI_REMAP          1    // 1: CC1..CC8 = the 8 knobs of an AKAI MPK mini (see synth_params.h)
                                 // 0: CC1 = mod wheel, CC5 = glide time (GM standard)
#define MIDI_PROGCHANGE_PATCHES 1

// ---- Audio shield: Waveshare Pico-Audio, two different boards -------------------
// The two boards carry different DACs and different pinouts, and software cannot tell
// them apart. The wrong setting is not "a bit off", it is SILENT. Check the PCB.
//
//   1 = "Rev2.1" (printed on the board)             CS4344 DAC   DIN GP22, BCK GP26, LRCK GP27
//       The board all development so far used. Default.
//   0 = "Initial Version" (no revision marking)     PCM5101A DAC DIN GP26, BCK GP27, LRCK GP28
//
// Switch without editing this file:  build_flags = ... -DAUDIO_BOARD_REV2_1=0
//
// The setting selects the pins AND the PIO program (see audio_driver.h):
//   1  audio_pio.pio      BCK = 64 x fs, each data bit held for two BCK periods. The Rev2.1 accepts
//                         this; it is the organ's proven program.
//   0  audio_pio_i2s.pio  standard I2S, BCK = 32 x fs, one data bit per BCK. The PCM5101A clocks DIN
//                         in on EVERY rising BCK edge (TI SLAS859C) and needs this. The Rev2.1
//                         program sounds badly distorted on it. It generates its own system clock
//                         from BCK (no MCLK needed).
#ifndef AUDIO_BOARD_REV2_1
#define AUDIO_BOARD_REV2_1  1
#endif
#if AUDIO_BOARD_REV2_1
  #define AUDIO_BOARD_NAME        "Rev2.1 (CS4344)"
  #define AUDIO_DATA_PIN          22   // DIN
  #define AUDIO_CLOCK_PIN_BASE    26   // BCK = GP26, LRCK = GP27 (automatic)
#else
  #define AUDIO_BOARD_NAME        "Initial Version (PCM5101A)"
  #define AUDIO_DATA_PIN          26   // DIN
  #define AUDIO_CLOCK_PIN_BASE    27   // BCK = GP27, LRCK = GP28 (automatic)
#endif

// ---- Audio -------------------------------------------------
#define SAMPLE_RATE    44100
#define BUFFER_FRAMES  256

// ---- Control rate ------------------------------------------
// Envelopes, filter coefficient, pitch modulation and the LFO are updated once
// per CONTROL_DIV samples (2756 Hz) and linearly interpolated in between.
#define CONTROL_DIV    16

// ---- Wavetable (4096 points) -------------------------------
#define WAVETABLE_BITS 12
#define WAVETABLE_SIZE (1 << WAVETABLE_BITS)
#define PHASE_SHIFT    (32 - WAVETABLE_BITS)

// ---- Sound -------------------------------------------------
#define VOICE_SCALE        4800   // per-voice peak (of 32767) at master volume 127
#define MOD_MAX_SEMITONES  2.0f   // vibrato depth at mod = 127
#define FILTER_MAX_HZ      12000.0f // nominal cutoff ceiling. ABOVE ~14 kHz the ladder
                                    // coefficient is unstable (measured: gain 2.8x at 16 kHz,
                                    // 11x at 20 kHz) and the filter is fully open from
                                    // 12 kHz anyway, so nothing audible is lost.
#define FILTER_CHEAP_SAT   0      // 1 = division-free saturator, slightly harder knee. It removes 5
                                  //     float divides per voice-sample, which should matter on the
                                  //     RP2350 FPU (divide ~14 cycles, not pipelined) but is UNMEASURED
                                  //     (no gain on a PC). Try it if the CPU square goes orange.
                                  //     0 = tanh-like Pade (default, the sound you know).

// ---- Misc --------------------------------------------------
#define LED_PIN  25

// Hot-path functions are force-inlined so they end up INSIDE loop1(), which is
// placed in SRAM (__not_in_flash_func). A non-inlined helper would run from flash
// and fight core 0 (USB host, LCD) for the shared XIP cache.
#define HOT      __attribute__((always_inline)) inline

// ---- Constants required by the shared effects.h ------------
#define VIBRATO_MAX_SEMITONES   0.25f
#define CHORUS_MIX_DEFAULT      64

// ---- MIDI CC map -------------------------------------------
#define CC_MASTER_VOL       7
#define CC_AMP_SUSTAIN     64
#define CC_GLIDE_ONOFF     65
#define CC_OSC_WAVE        70
#define CC_VCF_RESONANCE   71
#define CC_AMP_RELEASE     72
#define CC_AMP_ATTACK      73
#define CC_VCF_CUTOFF      74
#define CC_AMP_DECAY       75
#define CC_LFO_RATE        76
#define CC_MOD_DEPTH       77
#define CC_LFO_WAVE        78    // <64 sine, >=64 sample & hold
#define CC_GLIDE_TIME      79
#define CC_FLT_ATTACK      85
#define CC_FLT_DECAY       86
#define CC_FLT_SUSTAIN     87
#define CC_FLT_RELEASE     88
#define CC_FLT_ENV_AMT     89
#define CC_OSC_MIX         90
#define CC_NOISE_MIX       91
#define CC_OVERDRIVE       92
#define CC_OSC_DETUNE      94
#define CC_ALL_SOUND_OFF  120
#define CC_ALL_NOTES_OFF  123

// ---- AKAI MPK mini knobs K1..K8 (factory program: CC1..CC8) -
#define AKAI_K1   1   // -> filter cutoff
#define AKAI_K2   2   // -> filter resonance
#define AKAI_K3   3   // -> filter envelope amount
#define AKAI_K4   4   // -> amp attack
#define AKAI_K5   5   // -> amp release
#define AKAI_K6   6   // -> overdrive
#define AKAI_K7   7   // -> master volume
#define AKAI_K8   8   // -> oscillator mix
