// ----------------------------------------------------------------------------- //
// Assembled from audio_pio_i2s.pio by the same hand assembler that reproduces the    //
// organ's audio_pio.pio.h exactly (12/12 words); behaviour verified by PIO emulation. //
// To use the official tool instead:  pioasm audio_pio_i2s.pio audio_pio_i2s.pio.h     //
// ----------------------------------------------------------------------------- //

#pragma once

#if !PICO_NO_HARDWARE
#include "hardware/pio.h"
#endif

// ------------- //
// audio_pio_i2s //
// ------------- //

#define audio_pio_i2s_wrap_target 0
#define audio_pio_i2s_wrap 7
#define audio_pio_i2s_pio_version 0

#define audio_pio_i2s_offset_entry_point 7u

static const uint16_t audio_pio_i2s_program_instructions[] = {
            //     .wrap_target
    0x7001, //  0: out    pins, 1         side 2
    0x1840, //  1: jmp    x--, 0          side 3
    0x6001, //  2: out    pins, 1         side 0
    0xe82e, //  3: set    x, 14           side 1
    0x6001, //  4: out    pins, 1         side 0
    0x0844, //  5: jmp    x--, 4          side 1
    0x7001, //  6: out    pins, 1         side 2
    0xf82e, //  7: set    x, 14           side 3
            //     .wrap
};

#if !PICO_NO_HARDWARE
static const struct pio_program audio_pio_i2s_program = {
    .instructions = audio_pio_i2s_program_instructions,
    .length = 8,
    .origin = -1,
    .pio_version = audio_pio_i2s_pio_version,
#if PICO_PIO_VERSION > 0
    .used_gpio_ranges = 0x0
#endif
};

static inline pio_sm_config audio_pio_i2s_program_get_default_config(uint offset) {
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + audio_pio_i2s_wrap_target, offset + audio_pio_i2s_wrap);
    sm_config_set_sideset(&c, 2, false, false);
    return c;
}

#include "hardware/clocks.h"

static inline void audio_pio_i2s_init(PIO pio, uint sm, uint offset,
                                      uint data_pin, uint clock_pin_base,
                                      uint32_t sample_freq)
{
    pio_gpio_init(pio, data_pin);
    pio_gpio_init(pio, clock_pin_base);        // BCK
    pio_gpio_init(pio, clock_pin_base + 1);    // LRCK

    pio_sm_config c = audio_pio_i2s_program_get_default_config(offset);
    sm_config_set_out_pins(&c, data_pin, 1);
    sm_config_set_set_pins(&c, data_pin, 1);
    sm_config_set_sideset_pins(&c, clock_pin_base);
    sm_config_set_out_shift(&c, false, true, 32);    // MSB first, autopull 32

    // 2 PIO cycles per BCK, 32 BCK per frame  ->  clkdiv = sys_clk / (64 x fs)
    float clkdiv = (float)clock_get_hz(clk_sys) / (64.0f * (float)sample_freq);
    sm_config_set_clkdiv(&c, clkdiv);

    pio_sm_init(pio, sm, offset, &c);

    uint pin_mask = (1u << data_pin) | (3u << clock_pin_base);
    pio_sm_set_pindirs_with_mask(pio, sm, pin_mask, pin_mask);
    pio_sm_set_pins(pio, sm, 0);

    pio_sm_exec(pio, sm, pio_encode_jmp(offset + audio_pio_i2s_offset_entry_point));
    pio_sm_set_enabled(pio, sm, true);
}

#endif
