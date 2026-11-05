#pragma once
// ============================================================
//  usb_host.h  —  USB MIDI host via Pico-PIO-USB
//
//  Supports one USB MIDI device on the host port.
//  Multi-device hub support requires a newer pio_usb library.
//
//  PIO layout (RP2350 has 3 PIO blocks):
//    PIO0: USB TX   PIO1: USB RX   PIO2: Audio I2S
//
//  Hardware:
//    GP6 ── D+  (15kΩ pull-down to GND)
//    GP7 ── D-  (automatic, DP+1)
//    5V  ── VBUS    GND ── GND
//
//  Call usb_host_init() AFTER audio_driver_init() so PIO2
//  is already claimed before PIO0/PIO1 are used for USB.
// ============================================================

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include "config.h"
#include "synth_manager.h"  // full type needed for allNotesOff()
#include "pio_usb.h"

void midi_note_on(uint8_t note, uint8_t velocity);
void midi_note_off(uint8_t note);
void midi_cc(uint8_t cc, uint8_t value);
void midi_pitch_bend(int16_t bend);
void midi_program_change(uint8_t program);

extern SynthManager synth;

#if ENABLE_USB_HOST

static Adafruit_USBH_Host USBHost;

inline void usb_host_init() {
    static pio_usb_configuration_t config = PIO_USB_DEFAULT_CONFIG;
    config.pin_dp = MIDI_HOST_DP_PIN;
    USBHost.configure_pio_usb(1, &config);
    USBHost.begin(1);
}

inline void usb_host_poll() {
    USBHost.task();
}

extern "C" {

void tuh_mount_cb(uint8_t daddr) {
#if ENABLE_SERIAL
    Serial.print("USB host: device mounted addr=");
    Serial.println(daddr);
#endif
}

void tuh_umount_cb(uint8_t daddr) {
#if ENABLE_SERIAL
    Serial.print("USB host: device unmounted addr=");
    Serial.println(daddr);
#endif
    synth.allNotesOff();
}

void tuh_midi_mount_cb(uint8_t idx, const tuh_midi_mount_cb_t *cb_data) {
#if ENABLE_SERIAL
    Serial.print("MIDI mounted idx="); Serial.print(idx);
    Serial.print(" addr=");            Serial.println(cb_data->daddr);
#endif
}

void tuh_midi_umount_cb(uint8_t idx) {
#if ENABLE_SERIAL
    Serial.print("MIDI unmounted idx="); Serial.println(idx);
#endif
    synth.allNotesOff();
}

void tuh_midi_rx_cb(uint8_t idx, uint32_t xferred_bytes) {
    (void)xferred_bytes;
    if (!tuh_midi_mounted(idx)) return;
    uint8_t packet[4];
    while (tuh_midi_packet_read(idx, packet)) {
        uint8_t cin    = packet[0] & 0x0F;
        uint8_t status = packet[1];
        uint8_t d0     = packet[2];
        uint8_t d1     = packet[3];
        uint8_t ch     = (status & 0x0F) + 1;
        if (MIDI_CHANNEL != 0 && ch != MIDI_CHANNEL) continue;
        switch (cin) {
            case 0x9: if (d1 > 0) { midi_note_on(d0, d1); break; }
            case 0x8: midi_note_off(d0); break;
            case 0xB: midi_cc(d0, d1); break;
            case 0xC: midi_program_change(d0); break;
            case 0xE: midi_pitch_bend((int16_t)((d1<<7)|d0) - 8192); break;
            default:  break;
        }
    }
}

void tuh_midi_tx_cb(uint8_t idx, uint32_t xferred_bytes) {
    (void)idx; (void)xferred_bytes;
}

} // extern "C"

#else

inline void usb_host_init() {}
inline void usb_host_poll() {}

#endif // ENABLE_USB_HOST