#pragma once
// ============================================================
//  audio_driver.h  —  Waveshare Pico-Audio PIO driver (two boards)
//
//  Uses PIO2 (RP2350 has 3 PIO blocks) so PIO0 and PIO1 are
//  free for Pico-PIO-USB (TX=PIO0, RX=PIO1).
//
//  AUDIO_BOARD_REV2_1 (config.h) selects BOTH the pins and the PIO program:
//
//   1  Rev2.1, CS4344      audio_pio.pio       DIN GP22  BCK GP26  LRCK GP27
//        BCK = 64 x fs, every data bit held for TWO BCK periods (what the Rev2.1 accepts)
//   0  Initial, PCM5101A   audio_pio_i2s.pio   DIN GP26  BCK GP27  LRCK GP28
//        standard I2S: BCK = 32 x fs, ONE data bit per BCK (the PCM5101A clocks DIN in on
//        every rising BCK edge, so the Rev2.1 program sounds badly distorted on it)
// ============================================================

#include <Arduino.h>
#include <hardware/pio.h>
#include <hardware/clocks.h>
#include "config.h"

#if AUDIO_BOARD_REV2_1
  #include "audio_pio.pio.h"
  #define AUDIO_PIO_PROGRAM          audio_pio_program
  #define AUDIO_PIO_INIT             audio_pio_init
  #define AUDIO_PIO_CYCLES_PER_FRAME 128          // 4 instructions per data bit, 32 bits
  #define AUDIO_BCK_EXPECT           "2822.4"
#else
  #include "audio_pio_i2s.pio.h"
  #define AUDIO_PIO_PROGRAM          audio_pio_i2s_program
  #define AUDIO_PIO_INIT             audio_pio_i2s_init
  #define AUDIO_PIO_CYCLES_PER_FRAME 64           // 2 instructions per data bit, 32 bits
  #define AUDIO_BCK_EXPECT           "1411.2"
#endif

static PIO  _pio;
static uint _sm;
static uint _offset;

inline void audio_driver_init(uint32_t sample_freq)
{
    _pio    = pio2;   // PIO2 — free from USB host (TX=PIO0, RX=PIO1)
    _sm     = pio_claim_unused_sm(_pio, true);
    _offset = pio_add_program(_pio, &AUDIO_PIO_PROGRAM);

    AUDIO_PIO_INIT(_pio, _sm, _offset,
                   AUDIO_DATA_PIN,
                   AUDIO_CLOCK_PIN_BASE,
                   sample_freq);
}

inline void audio_driver_put(int16_t left, int16_t right)
{
#if AUDIO_BOARD_REV2_1
    uint32_t word = ((uint32_t)(uint16_t)left  << 16) | (uint16_t)right;   // Rev2.1 program: unchanged
#else
    // I2S: the half sent while LRCK = 0 is the LEFT channel, and that is the LOW half-word
    uint32_t word = ((uint32_t)(uint16_t)right << 16) | (uint16_t)left;
#endif
    pio_sm_put_blocking(_pio, _sm, word);
}

inline void audio_driver_debug()
{
    uint32_t sys_clk    = clock_get_hz(clk_sys);
    // Use _pio pointer instead of hardcoded pio0_hw
    io_rw_32* clkdiv_reg_ptr = &_pio->sm[_sm].clkdiv;
    uint32_t clkdiv_reg = *clkdiv_reg_ptr;
    float    divf       = (float)(clkdiv_reg >> 16) +
                          (float)((clkdiv_reg >> 8) & 0xff) / 256.0f;
    float    fs_actual  = (float)sys_clk / divf / (float)AUDIO_PIO_CYCLES_PER_FRAME;
    float    bck_khz    = (float)sys_clk / divf / 2.0f / 1000.0f;
    bool     enabled    = (_pio->ctrl >> _sm) & 1u;
    uint     pc         = _pio->sm[_sm].addr & 0x1f;

    Serial.println("--- PIO debug ---");
    Serial.print("  Board          : "); Serial.println(AUDIO_BOARD_NAME);
    Serial.print("  PIO            : pio");
    Serial.println(_pio == pio0 ? "0" : (_pio == pio1 ? "1" : "2"));
    Serial.print("  SM             : ");    Serial.println(_sm);
    Serial.print("  sys_clk        : ");    Serial.print(sys_clk / 1000000); Serial.println(" MHz");
    Serial.print("  clkdiv         : ");    Serial.println(divf, 3);
    Serial.print("  BCK            : ");    Serial.print(bck_khz, 1); Serial.println(" kHz (expect " AUDIO_BCK_EXPECT ")");
    Serial.print("  LRCK (fs)      : ");    Serial.print(fs_actual, 1); Serial.println(" Hz  (expect 44100)");
    Serial.print("  SM enabled     : ");    Serial.println(enabled ? "YES" : "NO  <-- problem!");
    Serial.print("  PC now         : ");    Serial.println(pc);
    Serial.print("  TX FIFO level  : ");    Serial.println(pio_sm_get_tx_fifo_level(_pio, _sm));
    Serial.print("  Data pin       : GP"); Serial.println(AUDIO_DATA_PIN);
    Serial.print("  BCK  pin       : GP"); Serial.println(AUDIO_CLOCK_PIN_BASE);
    Serial.print("  LRCK pin       : GP"); Serial.println(AUDIO_CLOCK_PIN_BASE + 1);
    Serial.println("-----------------");
}