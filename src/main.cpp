// ============================================================
//  main.cpp  —  Analog Synth  (restored on the tonewheel V1.0 architecture)
//
//  Core 0: MIDI (USB device + USB host + UART), LCD, buttons, serial
//          loop() spins flat out — no delay() — so MIDI latency stays minimal.
//  Core 1: audio only. loop1() lives in SRAM (__not_in_flash_func); every hot
//          function it calls is force-inlined (HOT), so the audio path never
//          fetches code from flash. Compute and put() are interleaved per
//          sample: the PIO FIFO never starves, so the DAC never auto-mutes.
//
//  Serial (115200): type  help
// ============================================================
#include <Arduino.h>
#include "config.h"
#include "wavetable.h"
#include "synth_manager.h"
#include "audio_driver.h"
#if ENABLE_LCD
#  include "lcd.h"
#endif
#include "midi_handler.h"
#if ENABLE_USB_HOST
#  include "usb_host.h"
#endif

static_assert(AUDIO_BOARD_REV2_1 == 0 || AUDIO_BOARD_REV2_1 == 1, "AUDIO_BOARD_REV2_1 must be 0 or 1 (see config.h)");

SynthManager synth;
volatile uint8_t g_cpu_load_pct = 0;     // written by core 1, read by core 0 (uint8 write is atomic)

#if ENABLE_SERIAL
static void handleSerial();
#endif

// ============================================================
//  MIDI event log
//  MIDI callbacks only enqueue a tiny record; loop() prints it. A callback must
//  never wait on the USB-CDC serial port (it can block), so a slow terminal can
//  no longer delay the next MIDI event.
// ============================================================
enum : uint8_t { L_ON, L_OFF, L_CC, L_PB, L_PC };
struct LogRec { uint8_t kind, a, b; int16_t v; };
#define LOG_N 64                                  // power of two
static LogRec           _log[LOG_N];
static volatile uint8_t _lw = 0, _lr = 0;
static uint8_t          g_logMask = 1;            // bit0 notes, bit1 CC, bit2 bend/program  (serial: log ...)

static void log_push(uint8_t kind, uint8_t a, uint8_t b, int16_t v = 0) {
#if ENABLE_SERIAL
    const uint8_t bit = (kind == L_ON || kind == L_OFF) ? 1 : (kind == L_CC) ? 2 : 4;
    if (!(g_logMask & bit)) return;
    const uint8_t n = (uint8_t)((_lw + 1) & (LOG_N - 1));
    if (n == _lr) return;                         // full: drop
    _log[_lw] = { kind, a, b, v };
    _lw = n;
#else
    (void)kind; (void)a; (void)b; (void)v;
#endif
}

static void log_flush() {
#if ENABLE_SERIAL
    for (int i = 0; i < 6 && _lr != _lw; i++) {   // at most 6 lines per loop() pass
        const LogRec& r = _log[_lr];
        switch (r.kind) {
            case L_ON:  Serial.printf("ON  %3u v%3u  [%u/%u]\n", r.a, r.b, synth.countedVoices(), MAX_ACTIVE_VOICES); break;
            case L_OFF: Serial.printf("OFF %3u\n", r.a); break;
            case L_CC:  Serial.printf("CC  %3u = %3u%s\n", r.a, r.b, r.v ? "" : "  (unused)"); break;
            case L_PB:  Serial.printf("PB  %d\n", (int)r.v); break;
            case L_PC:  Serial.printf("PC  %u\n", r.a); break;
        }
        _lr = (uint8_t)((_lr + 1) & (LOG_N - 1));
    }
#endif
}

// ============================================================
//  MIDI callbacks (called from loop() via midi_handler_poll / usb_host_poll)
// ============================================================
void midi_note_on(uint8_t note, uint8_t velocity) {
    synth.noteOn(note, velocity);
#if ENABLE_LCD
    ui_midi_activity();
#endif
    log_push(L_ON, note, velocity);
}

void midi_note_off(uint8_t note) {
    synth.noteOff(note);
#if ENABLE_LCD
    ui_midi_activity();
#endif
    log_push(L_OFF, note, 0);
}

void midi_pitch_bend(int16_t bend) {
    synth.pitchBend(bend);
    log_push(L_PB, 0, 0, bend);
}

void midi_cc(uint8_t cc, uint8_t value) {
    const bool used = synth.handleCC(cc, value);
#if ENABLE_LCD
    ui_midi_activity();
#endif
    log_push(L_CC, cc, value, used ? 1 : 0);
}

void midi_program_change(uint8_t program) {
#if MIDI_PROGCHANGE_PATCHES
    synth.loadPatch(program % N_PATCHES);
#endif
    log_push(L_PC, program, 0);
}

// ============================================================
//  Core 0
// ============================================================
void setup() {
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    midi_handler_init();

#if ENABLE_SERIAL
    Serial.begin(115200);                          // no while(!Serial): runs without a terminal
    delay(100);
    Serial.println("=== Analog Synth ===");
    Serial.printf("voices: %d active / %d pool   control: %d samples\n", MAX_ACTIVE_VOICES, SYNTH_POOL, CONTROL_DIV);
    Serial.printf("audio shield: %s  (DIN GP%d, BCK GP%d, LRCK GP%d)  <- silent if this is not your board\n",
                  AUDIO_BOARD_NAME, AUDIO_DATA_PIN, AUDIO_CLOCK_PIN_BASE, AUDIO_CLOCK_PIN_BASE + 1);
#endif

    Wavetable::init();
    synth.init();
    audio_driver_init(SAMPLE_RATE);                // claims PIO2 first

#if ENABLE_USB_HOST
    usb_host_init();                               // after audio: USB takes PIO0 (TX) / PIO1 (RX)
#endif

#if ENABLE_LCD
    lcd_begin();
#endif

#if ENABLE_SERIAL
    Serial.println("Ready. Type 'help'.");
#endif
    digitalWrite(LED_PIN, HIGH);
}

void loop() {
    midi_handler_poll();
#if ENABLE_USB_HOST
    usb_host_poll();
#endif

#if ENABLE_LCD
    ui_handle_buttons(synth);
    ui_flush(synth);                               // cheap when idle; sends at most one rectangle per call
#endif

#if ENABLE_SERIAL
    log_flush();
    handleSerial();
#endif
    // No delay(): loop() must spin fast so MIDI is serviced with minimum latency.
}

// ============================================================
//  Core 1  —  audio, runs from SRAM
// ============================================================
void setup1() { delay(500); }                      // let core 0 finish init (tables, USB host, LCD)

void __not_in_flash_func(loop1)() {
    // CPU load = cycles spent in tick() / cycles available per buffer.
    // DWT cycle counter via raw registers (CMSIS structs are not exposed in arduino-pico).
    static constexpr uint32_t BUDGET_CY =
        (uint32_t)((uint64_t)F_CPU * BUFFER_FRAMES / SAMPLE_RATE);
    #define _DCB_DEMCR  (*((volatile uint32_t*)0xE000EDFC))
    #define _DWT_CYCCNT (*((volatile uint32_t*)0xE0001004))
    #define _DWT_CTRL   (*((volatile uint32_t*)0xE0001000))
    _DCB_DEMCR |= 0x01000000u;                     // TRCENA
    _DWT_CYCCNT = 0;
    _DWT_CTRL  |= 0x00000001u;                     // CYCCNTENA

    uint32_t total_cycles = 0;
    for (int i = 0; i < BUFFER_FRAMES; i++) {
        const uint32_t c0 = _DWT_CYCCNT;
        const int16_t  s  = synth.tick();
        total_cycles += _DWT_CYCCNT - c0;
        audio_driver_put(s, s);                    // blocks until the FIFO has room: paces the loop
    }

    const uint32_t load_now = (total_cycles * 100) / BUDGET_CY;
    static uint32_t filt16 = 0;                    // x16 fixed point, fast attack / fast decay
    const uint32_t target = load_now * 16;
    if (target > filt16) filt16 = (filt16 * 1 + target * 15) / 16;
    else                 filt16 = (filt16 * 13 + target * 3) / 16;
    g_cpu_load_pct = (uint8_t)((filt16 / 16) > 99 ? 99 : filt16 / 16);
}

// ============================================================
//  Serial commands
// ============================================================
#if ENABLE_SERIAL
static void printVoices() {
    Serial.printf("voices: %u sounding (%u counted) / pool %d, cap %d\n",
                  synth.activeCount(), synth.countedVoices(), SYNTH_POOL, MAX_ACTIVE_VOICES);
    for (int i = 0; i < SYNTH_POOL; i++) {
        if (!synth.voiceLive(i)) continue;
        Serial.printf("  V%02d %s note=%3u env=%.2f %s\n", i, synth.voiceState(i), synth.voiceNote(i),
                      synth.voiceLevel(i), synth.voiceDying(i) ? "FADING" : synth.voiceHeld(i) ? "held" : "released");
    }
}

static void printParams() {
    Serial.printf("patch: %s   glide: %s   bend: %+.2f st   mod ratio: %.4f\n",
                  synth.patchName, synth.glide() ? "on" : "off", synth.ep.bend, synth.modRatio());
    for (int i = 0; i < N_PARAMS; i++)
        Serial.printf("  %-6s %-12s = %3u   (CC %u)\n", PARAM_INFO[i].cmd, PARAM_INFO[i].name, synth.pv[i], PARAM_INFO[i].cc);
}

static void runCommand(String line) {
    line.trim();
    if (!line.length()) return;

    if (line == "help") {
        Serial.println("  stat              CPU load, voices, audio PIO");
        Serial.println("  voices            voice slots (note, envelope, state)");
        Serial.println("  params            all parameter values");
        Serial.println("  set <name> <0-127>   e.g. set cut 70   (names: see 'params')");
        Serial.println("  cc <n> <0-127>    send a MIDI CC to the synth");
        Serial.println("  on <note> | off <note>");
        Serial.println("  patch <0-2>       0 Init  1 Pad  2 Bass");
        Serial.println("  glide on|off");
        Serial.println("  all               release all notes");
        Serial.println("  panic             silence everything now");
        Serial.println("  log notes|cc|all|off   what MIDI events to print");
        Serial.println("  pio               audio PIO state");
    } else if (line == "stat") {
        Serial.printf("CPU: %u%%   voices: %u sounding / %u counted (cap %d, pool %d)\n",
                      (unsigned)g_cpu_load_pct, synth.activeCount(), synth.countedVoices(), MAX_ACTIVE_VOICES, SYNTH_POOL);
        audio_driver_debug();
    } else if (line == "voices") {
        printVoices();
    } else if (line == "params") {
        printParams();
    } else if (line == "all") {
        synth.allNotesOff(); Serial.println("all notes released");
    } else if (line == "panic") {
        synth.panic();       Serial.println("panic");
    } else if (line == "pio") {
        audio_driver_debug();
    } else if (line.startsWith("on ")) {
        synth.noteOn((uint8_t)line.substring(3).toInt(), 100);
    } else if (line.startsWith("off ")) {
        synth.noteOff((uint8_t)line.substring(4).toInt());
    } else if (line.startsWith("cc ")) {
        const int sp = line.indexOf(' ', 3);
        if (sp > 0) {
            const int cc = line.substring(3, sp).toInt(), v = line.substring(sp + 1).toInt();
            Serial.printf("CC %d = %d  %s\n", cc, v, synth.handleCC((uint8_t)cc, (uint8_t)constrain(v, 0, 127)) ? "" : "(unused)");
        } else Serial.println("usage: cc <n> <0-127>");
    } else if (line.startsWith("set ")) {
        const int sp = line.indexOf(' ', 4);
        if (sp > 0) {
            const String name = line.substring(4, sp);
            const int v = constrain((int)line.substring(sp + 1).toInt(), 0, 127);
            int id = -1;
            for (int i = 0; i < N_PARAMS; i++) if (name == PARAM_INFO[i].cmd) id = i;
            if (id >= 0) { synth.setParam((uint8_t)id, (uint8_t)v); Serial.printf("%s = %d\n", PARAM_INFO[id].name, v); }
            else Serial.println("unknown parameter (type 'params')");
        } else Serial.println("usage: set <name> <0-127>");
    } else if (line.startsWith("patch ")) {
        const int p = line.substring(6).toInt();
        synth.loadPatch((uint8_t)p); Serial.printf("patch: %s\n", synth.patchName);
    } else if (line == "glide on")  { synth.setGlide(true);  Serial.println("glide on (mono legato)");
    } else if (line == "glide off") { synth.setGlide(false); Serial.println("glide off");
    } else if (line == "log notes") { g_logMask = 1; Serial.println("log: notes");
    } else if (line == "log cc")    { g_logMask = 3; Serial.println("log: notes + CC");
    } else if (line == "log all")   { g_logMask = 7; Serial.println("log: everything");
    } else if (line == "log off")   { g_logMask = 0; Serial.println("log: off");
    } else {
        Serial.print("Unknown: "); Serial.println(line);
    }
#if ENABLE_LCD
    ui_mark_all_dirty();
#endif
}

static void handleSerial() {
    static String line;
    while (Serial.available()) {
        const char c = (char)Serial.read();
        if (c == '\n' || c == '\r') { runCommand(line); line = ""; }
        else if (line.length() < 80) line += c;
    }
}
#endif