#pragma once
// ============================================================
//  midi_handler.h  –  MIDI input: USB-MIDI + UART MIDI
//
//  Supports two simultaneous MIDI sources:
//    1. USB-MIDI  (class-compliant, no driver needed on host)
//    2. UART MIDI (DIN-5 via optocoupler on UART1, GP4/GP5)
//       GP4 = TX (not used for receive, but set for completeness)
//       GP5 = RX
//
//  Calls midi_note_on(note, velocity) / midi_note_off(note)
//  which you implement in main.cpp.
//
//  Single-note implementation: only the most recent note plays.
//  Polyphony is added in the next step.
// ============================================================

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include "config.h"

// ---- Forward declarations (implemented in main.cpp) --------
void midi_note_on(uint8_t note, uint8_t velocity);
void midi_note_off(uint8_t note);
void midi_pitch_bend(int16_t bend);
void midi_cc(uint8_t cc, uint8_t value);
void midi_program_change(uint8_t program);

// ---- USB-MIDI ----------------------------------------------
Adafruit_USBD_MIDI usb_midi_iface;

// ---- UART MIDI (31250 baud on UART1) -----------------------
// Uses Serial2 in earlephilhower (maps to UART1)
// Wire a 6N138 optocoupler: DIN-5 pin2→GND, pin4→optocoupler,
// output → GP5.  TX (GP4) unused for standard MIDI-in.
#define MIDI_UART_BAUD  31250
#define MIDI_UART_RX    5     // GP5

// ---- Simple UART MIDI parser state machine -----------------
namespace {
    uint8_t _uartStatus  = 0;
    uint8_t _uartData[2] = {0, 0};
    uint8_t _uartIdx     = 0;
    uint8_t _uartExpect  = 0;   // expected data bytes for current status

    uint8_t _expectedBytes(uint8_t status) {
        uint8_t type = status & 0xF0;
        switch (type) {
            case 0x80: return 2;  // note off
            case 0x90: return 2;  // note on
            case 0xA0: return 2;  // aftertouch
            case 0xB0: return 2;  // CC
            case 0xC0: return 1;  // program change
            case 0xD0: return 1;  // channel pressure
            case 0xE0: return 2;  // pitch bend
            default:   return 0;
        }
    }

    void _dispatchUart(uint8_t status, uint8_t d0, uint8_t d1) {
        uint8_t ch   = (status & 0x0F) + 1;
        if (MIDI_CHANNEL != 0 && ch != MIDI_CHANNEL) return;
        uint8_t type = status & 0xF0;
        switch (type) {
            case 0x90:
                if (d1 > 0) { midi_note_on(d0, d1); break; }
            case 0x80:
                midi_note_off(d0);
                break;
            case 0xB0:
                midi_cc(d0, d1);
                break;
            case 0xC0:
                midi_program_change(d0);
                break;
            case 0xE0: {
                int16_t bend = (int16_t)((d1 << 7) | d0) - 8192;
                midi_pitch_bend(bend);
                break;
            }
            default: break;
        }
    }

    void _parseUartByte(uint8_t b) {
        if (b & 0x80) {
            // Status byte
            _uartStatus = b;
            _uartIdx    = 0;
            _uartExpect = _expectedBytes(b);
        } else {
            // Data byte
            if (_uartExpect == 0) return;
            _uartData[_uartIdx++] = b;
            if (_uartIdx >= _uartExpect) {
                _dispatchUart(_uartStatus,
                              _uartData[0],
                              _uartExpect > 1 ? _uartData[1] : 0);
                _uartIdx = 0;   // running status: ready for next message
            }
        }
    }
}

// ---- USB-MIDI packet parser --------------------------------
namespace {
    void _dispatchUsbPacket(uint8_t pkt[4]) {
        uint8_t cin    = pkt[0] & 0x0F;
        uint8_t status = pkt[1];
        uint8_t d0     = pkt[2];
        uint8_t d1     = pkt[3];
        uint8_t ch     = (status & 0x0F) + 1;
        if (MIDI_CHANNEL != 0 && ch != MIDI_CHANNEL) return;
        switch (cin) {
            case 0x9:
                if (d1 > 0) { midi_note_on(d0, d1); break; }
            case 0x8:
                midi_note_off(d0);
                break;
            case 0xB:
                midi_cc(d0, d1);
                break;
            case 0xC:
                midi_program_change(d0);
                break;
            case 0xE: {
                int16_t bend = (int16_t)((d1 << 7) | d0) - 8192;
                midi_pitch_bend(bend);
                break;
            }
            default: break;
        }
    }
}

// ---- Public API --------------------------------------------

inline void midi_handler_init()
{
    // USB-MIDI
    usb_midi_iface.begin();
    TinyUSBDevice.setManufacturerDescriptor("Analog");
    TinyUSBDevice.setProductDescriptor("Analog Synth");

    // UART MIDI on Serial2 (UART1, GP4/GP5)
    pinMode(MIDI_UART_RX, INPUT_PULLUP);
    Serial2.setRX(MIDI_UART_RX);
    Serial2.begin(MIDI_UART_BAUD);
}

inline void midi_handler_poll()
{
    // -- USB-MIDI --
    if (TinyUSBDevice.mounted()) {
        uint8_t pkt[4];
        while (usb_midi_iface.readPacket(pkt)) {
            _dispatchUsbPacket(pkt);
        }
    }

    // -- UART MIDI --
    while (Serial2.available()) {
        _parseUartByte((uint8_t)Serial2.read());
    }
}