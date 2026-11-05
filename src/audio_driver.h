#pragma once
// ============================================================
//  audio_driver.h  —  Waveshare Pico-Audio Rev 2.1 PIO driver
//
//  Uses PIO2 (RP2350 has 3 PIO blocks) so PIO0 and PIO1 are
//  free for Pico-PIO-USB (TX=PIO0, RX=PIO1).
//
//  GP22 = DIN   GP26 = BCK   GP27 = LRCK
// ============================================================

#include <Arduino.h>
#include <hardware/pio.h>
#include <hardware/clocks.h>
#include "audio_pio.pio.h"
#include "config.h"

static PIO  _pio;
static uint _sm;
static uint _offset;

inline void audio_driver_init(uint32_t sample_freq)
{
    _pio    = pio2;   // PIO2 — free from USB host (TX=PIO0, RX=PIO1)
    _sm     = pio_claim_unused_sm(_pio, true);
    _offset = pio_add_program(_pio, &audio_pio_program);

    audio_pio_init(_pio, _sm, _offset,
                   AUDIO_DATA_PIN,
                   AUDIO_CLOCK_PIN_BASE,
                   sample_freq);
}

static inline void __not_in_flash_func(audio_driver_put)(int16_t left, int16_t right)
{
    uint32_t word = ((uint32_t)(uint16_t)left << 16) | (uint16_t)right;
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
    float    fs_actual  = (float)sys_clk / divf / 128.0f;
    float    bck_khz    = (float)sys_clk / divf / 2.0f / 1000.0f;
    bool     enabled    = (_pio->ctrl >> _sm) & 1u;
    uint     pc         = _pio->sm[_sm].addr & 0x1f;

    Serial.println("--- PIO debug ---");
    Serial.print("  PIO            : pio");
    Serial.println(_pio == pio0 ? "0" : (_pio == pio1 ? "1" : "2"));
    Serial.print("  SM             : ");    Serial.println(_sm);
    Serial.print("  sys_clk        : ");    Serial.print(sys_clk / 1000000); Serial.println(" MHz");
    Serial.print("  clkdiv         : ");    Serial.println(divf, 3);
    Serial.print("  BCK            : ");    Serial.print(bck_khz, 1); Serial.println(" kHz (expect 2822.4)");
    Serial.print("  LRCK (fs)      : ");    Serial.print(fs_actual, 1); Serial.println(" Hz  (expect 44100)");
    Serial.print("  SM enabled     : ");    Serial.println(enabled ? "YES" : "NO  <-- problem!");
    Serial.print("  PC now         : ");    Serial.println(pc);
    Serial.print("  TX FIFO level  : ");    Serial.println(pio_sm_get_tx_fifo_level(_pio, _sm));
    Serial.print("  Data pin       : GP"); Serial.println(AUDIO_DATA_PIN);
    Serial.print("  BCK  pin       : GP"); Serial.println(AUDIO_CLOCK_PIN_BASE);
    Serial.print("  LRCK pin       : GP"); Serial.println(AUDIO_CLOCK_PIN_BASE + 1);
    Serial.println("-----------------");
}