#pragma once
// ============================================================
//  lcd.h  —  public interface of the LCD panel + joystick/buttons
//  Implementation: lcd.cpp
//
//  The panel polls the synth state itself and redraws only what changed, so
//  callers do not have to mark anything dirty when a CC arrives.
// ============================================================
#include <stdint.h>

class SynthManager;                       // forward declaration

void lcd_begin();                         // init SPI1, panel and buttons
void ui_flush(const SynthManager& synth); // call every loop(): cheap when idle, time-sliced when not
void ui_handle_buttons(SynthManager& synth);
void ui_midi_activity();                  // flash the MIDI dot
void ui_mark_all_dirty();                 // force a full redraw
