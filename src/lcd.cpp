// ============================================================
//  lcd.cpp  —  single-page knob panel, Waveshare Pico-LCD-1.14 (ST7789, 240x135)
//
//  WHY THIS IS FAST
//    The old panel re-rendered the whole screen and pushed it one BYTE at a time
//    (~65,000 SPI calls = >100 ms with core 0 stuck, so USB-MIDI polling stalled).
//    Now:
//      * every widget (knob / top bar / status block) remembers what it last drew
//        and is only redrawn when its own value changed;
//      * only that widget's rectangle is sent to the panel (a knob is ~2.5 KB);
//      * rectangles go out as one bulk SPI transfer (spi_write_blocking, ~8 MB/s);
//      * at most ONE rectangle is sent per ui_flush() call, so loop() keeps
//        polling USB/MIDI between rectangles (worst case ~1 ms per call).
//
//  Layout (240 x 135):                       row 0  OSC | FILTER
//    y   0..17   top bar                     row 1  AMP ENV | FILTER ENV
//    y  19..131  3 rows x 8 columns          row 2  VOL DRV LFO MOD GLD | status
//
//  Joystick LEFT/RIGHT select a knob, UP/DOWN change it (hold = repeat),
//  PRESS = next patch, A = panic (silence everything), B = glide on/off.
// ============================================================
#include <Arduino.h>
#include <SPI.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "lcd.h"
#include "synth_manager.h"

extern volatile uint8_t g_cpu_load_pct;     // measured by core 1 (main.cpp)

// ---- Pins (hard-wired on the Pico-LCD-1.14) ----------------
#define LCD_SCK   10
#define LCD_MOSI  11
#define LCD_CS     9
#define LCD_DC     8
#define LCD_RST   12
#define LCD_BL    13
#define BTN_UP     2
#define BTN_DOWN  18
#define BTN_LEFT  16
#define BTN_RIGHT 20
#define BTN_PRESS  3
#define BTN_A     15
#define BTN_B     17

#define LCD_W   240
#define LCD_H   135

// ---- Layout -------------------------------------------------
#define TOP_H     18
#define GRID_Y0   19
#define ROW_H     38
#define CELL_W    29
#define CELL_H    37
#define KNOB_R    12
static inline int cellX(int col) { return col * CELL_W + (col >= 4 ? 8 : 0); }   // col 7 ends at x=239
static inline int cellY(int row) { return GRID_Y0 + row * ROW_H; }
#define SEP_X     120
#define STAT_COL    5          // status block starts at the column-5 cell of row 2

// ---- Colours (RGB565) -----------------------------------------
#define C_BG       0x0841
#define C_TOPBG    0x2104
#define C_KBKG     0x18C3
#define C_KRIM     0x4228
#define C_SEP      0x2965
#define C_GRAY     0x4A49
#define C_WHITE    0xFFFF
#define C_CYAN     0x07FF
#define C_GREEN    0x07E0
#define C_GREENDIM 0x0320
#define C_ORANGE   0xFB00
#define C_RED      0xD8C0
#define C_AMBER    0xFD00
static const uint16_t GROUP_COL[5] = { 0x07E0 /*osc: green*/,  0x07FF /*filter: cyan*/,  0xFC60 /*amp env: orange*/,
                                       0xF81F /*flt env: magenta*/, 0xFFE0 /*fx: yellow*/ };

static const char* const WAVE_NAMES[4] = { "SIN", "TRI", "SAW", "SQR" };

// ---- Framebuffer (native RGB565) + bulk-transfer staging ----------------
static uint16_t FB[LCD_W * LCD_H];
#define TXB_SIZE 4096
static uint8_t  TXB[TXB_SIZE];

static inline void fb_px(int x, int y, uint16_t c) {
    if ((unsigned)x < (unsigned)LCD_W && (unsigned)y < (unsigned)LCD_H) FB[y * LCD_W + x] = c;
}
static void fb_rect(int x, int y, int w, int h, uint16_t c) {
    for (int r = y; r < y + h && r < LCD_H; r++) {
        if (r < 0) continue;
        for (int col = x; col < x + w && col < LCD_W; col++)
            if (col >= 0) FB[r * LCD_W + col] = c;
    }
}
static void fb_disc(int cx, int cy, int r, uint16_t c) {
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r) fb_px(cx + dx, cy + dy, c);
}
static void fb_circle(int cx, int cy, int r, uint16_t c) {
    int x = r, y = 0, d = 1 - r;
    while (x >= y) {
        fb_px(cx+x, cy+y, c); fb_px(cx-x, cy+y, c); fb_px(cx+x, cy-y, c); fb_px(cx-x, cy-y, c);
        fb_px(cx+y, cy+x, c); fb_px(cx-y, cy+x, c); fb_px(cx+y, cy-x, c); fb_px(cx-y, cy-x, c);
        if (d <= 0) { d += 2 * y + 3; y++; } else { d += 2 * (y - x) + 5; x--; y++; }
    }
}

// ---- 5x7 font (identical to the organ's lcd.cpp) -------------------------
static const uint8_t FONT[][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},
    {0x14,0x7F,0x14,0x7F,0x14},{0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},{0x00,0x1C,0x22,0x41,0x00},
    {0x00,0x41,0x22,0x1C,0x00},{0x14,0x08,0x3E,0x08,0x14},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},
    {0x20,0x10,0x08,0x04,0x02},{0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},{0x18,0x14,0x12,0x7F,0x10},
    {0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},
    {0x00,0x56,0x36,0x00,0x00},{0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},
    {0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},{0x32,0x49,0x79,0x41,0x3E},
    {0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A},{0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},{0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},{0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x7F,0x00},{0x04,0x02,0x01,0x02,0x04},
    {0x40,0x40,0x40,0x40,0x40},{0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},
    {0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},{0x38,0x44,0x44,0x48,0x7F},
    {0x38,0x54,0x54,0x54,0x18},{0x08,0x7E,0x09,0x01,0x02},{0x0C,0x52,0x52,0x52,0x3E},
    {0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x20,0x40,0x44,0x3D,0x00},
    {0x7F,0x10,0x28,0x44,0x00},{0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x18,0x04,0x78},
    {0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},{0x7C,0x14,0x14,0x14,0x08},
    {0x08,0x14,0x14,0x18,0x7C},{0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x40,0x3C},{0x1C,0x20,0x40,0x20,0x1C},
    {0x3C,0x40,0x30,0x40,0x3C},{0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},
    {0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00},{0x00,0x00,0x7F,0x00,0x00},
    {0x00,0x41,0x36,0x08,0x00},{0x10,0x08,0x08,0x10,0x08},
};

static void fb_char(int x, int y, char c, uint16_t fg, uint16_t bg) {
    if (c < 32 || c > 126) c = '?';
    const uint8_t* g = FONT[c - 32];
    for (int col = 0; col < 5; col++)
        for (int row = 0; row < 7; row++)
            fb_px(x + col, y + row, ((g[col] >> row) & 1) ? fg : bg);
}
static void fb_str(int x, int y, const char* s, uint16_t fg, uint16_t bg) {
    for (; *s; s++, x += 6) fb_char(x, y, *s, fg, bg);
}
static int fb_strw(const char* s) { int n = 0; while (*s++) n++; return n * 6; }

// ---- Knob arc tables (computed once; no sinf/cosf while drawing) ---------
#define ARC_N 109                          // 270 deg in 2.5 deg steps, bottom-left -> bottom-right
static int8_t ARC_DX[ARC_N], ARC_DY[ARC_N];   // radius KNOB_R-2
static int8_t DOT_DX[ARC_N], DOT_DY[ARC_N];   // radius KNOB_R-5
static void initArcTables() {
    for (int k = 0; k < ARC_N; k++) {
        const float th = (-135.0f + 2.5f * (float)k) * (float)M_PI / 180.0f;   // 0 = straight up
        ARC_DX[k] = (int8_t)lroundf((KNOB_R - 2) * sinf(th));
        ARC_DY[k] = (int8_t)(-lroundf((KNOB_R - 2) * cosf(th)));
        DOT_DX[k] = (int8_t)lroundf((KNOB_R - 5) * sinf(th));
        DOT_DY[k] = (int8_t)(-lroundf((KNOB_R - 5) * cosf(th)));
    }
}

// ---- UI state -----------------------------------------------------------
#define W_TOP    (N_KNOBS)                 // widget ids: 0..N_KNOBS-1 = knobs
#define W_STAT   (N_KNOBS + 1)
#define W_COUNT  (N_KNOBS + 2)

static int      _sel        = P_CUTOFF;    // selected knob
static uint32_t _pending    = 0;           // widgets drawn into FB but not yet sent to the panel
static uint32_t _midi_until = 0;
static bool     _force      = true;        // redraw everything on the next scan
static bool     _first      = true;        // nothing on the panel yet
static uint32_t _lastScan   = 0;
static uint16_t _knobKey[N_KNOBS];
static uint32_t _topKey, _statKey;

// ---- Value text -----------------------------------------------------------
static void valueText(const SynthManager& s, int id, char* out, size_t n) {
    const int v = s.pv[id];
    switch (id) {
        case P_WAVE:    snprintf(out, n, "%s", WAVE_NAMES[(v >> 5) & 3]);    break;
        case P_DETUNE:  snprintf(out, n, "%+dc", ((v - 64) * 50) / 64);      break;
        case P_LFOWAVE: snprintf(out, n, "%s", v >= 64 ? "S&H" : "SIN");     break;
        default:        snprintf(out, n, "%d", v);                           break;
    }
}

// ---- Widget renderers (draw into FB only) ---------------------------------
static void drawKnob(const SynthManager& s, int id, bool sel) {
    const ParamInfo& pi = PARAM_INFO[id];
    const int x0 = cellX(pi.col), y0 = cellY(pi.row);
    const int cx = x0 + CELL_W / 2, cy = y0 + 22;
    const uint16_t gc = GROUP_COL[pi.group];

    fb_rect(x0, y0, CELL_W, CELL_H, C_BG);
    fb_str(cx - fb_strw(pi.label) / 2, y0 + 1, pi.label, sel ? C_WHITE : gc, C_BG);

    fb_disc(cx, cy, KNOB_R - 1, C_KBKG);
    const int n = (s.pv[id] * (ARC_N - 1)) / 127;           // lit arc steps
    for (int k = 0; k < ARC_N; k++) {
        const uint16_t c = (k <= n) ? gc : C_KRIM;
        fb_px(cx + ARC_DX[k],     cy + ARC_DY[k], c);
        fb_px(cx + ARC_DX[k] + 1, cy + ARC_DY[k], c);
    }
    fb_disc(cx + DOT_DX[n], cy + DOT_DY[n], 2, C_WHITE);
    fb_circle(cx, cy, KNOB_R, sel ? C_WHITE : C_KRIM);
    if (sel) fb_circle(cx, cy, KNOB_R - 1, C_WHITE);
}

static void drawTop(const SynthManager& s) {
    fb_rect(0, 0, LCD_W, TOP_H, C_TOPBG);
    fb_str(6, 5, s.patchName, C_WHITE, C_TOPBG);

    char val[12], buf[32];
    valueText(s, _sel, val, sizeof val);
    snprintf(buf, sizeof buf, "%s %s", PARAM_INFO[_sel].name, val);
    fb_str(64, 5, buf, GROUP_COL[PARAM_INFO[_sel].group], C_TOPBG);

    const uint8_t cpu = g_cpu_load_pct;                      // CPU square: green <50, orange <80, red
    fb_rect(218, 5, 7, 7, cpu >= 80 ? C_RED : cpu >= 50 ? C_ORANGE : C_GREEN);

    const uint16_t mc = (millis() < _midi_until) ? C_GREEN : C_GREENDIM;   // MIDI dot
    fb_rect(228, 5, 8, 8, mc);
    fb_px(228, 5, C_TOPBG);  fb_px(235, 5, C_TOPBG);
    fb_px(228, 12, C_TOPBG); fb_px(235, 12, C_TOPBG);
}

static void drawStatus(const SynthManager& s) {
    const int x0 = cellX(STAT_COL), y0 = cellY(2);
    fb_rect(x0, y0, LCD_W - x0, CELL_H, C_BG);

    const int n = s.countedVoices();                         // voice pips
    for (int i = 0; i < MAX_ACTIVE_VOICES; i++)
        fb_rect(x0 + 3 + i * 6, y0 + 2, 5, 5, i < n ? (n >= MAX_ACTIVE_VOICES ? C_RED : C_GREEN) : C_KBKG);

    fb_str(x0 + 3, y0 + 13, "PB", C_GRAY, C_BG);             // pitch bend, centre = 0
    const int bx = x0 + 20, bw = 60, by = y0 + 14, mid = bx + bw / 2;
    fb_rect(bx, by, bw, 5, C_KBKG);
    const int px = (int)(s.ep.bend * (float)(bw / 2) / 2.0f);
    if (px > 0) fb_rect(mid, by, px, 5, C_CYAN);
    else if (px < 0) fb_rect(mid + px, by, -px, 5, C_CYAN);
    fb_rect(mid, by - 1, 1, 7, C_WHITE);

    fb_str(x0 + 3, y0 + 25, "GLIDE", s.glide() ? C_AMBER : C_GRAY, C_BG);
}

static void drawBackground() {
    fb_rect(0, 0, LCD_W, LCD_H, C_BG);
    for (int y = GRID_Y0; y < LCD_H; y++) fb_px(SEP_X, y, C_SEP);
    for (int r = 0; r < 2; r++) fb_rect(0, cellY(r) + CELL_H, LCD_W, 1, C_SEP);
}

// ---- Panel access (ST7789) -------------------------------------------------
static SPISettings _spi(62500000, MSBFIRST, SPI_MODE0);

static void _cmd(uint8_t c) {
    digitalWrite(LCD_DC, LOW); digitalWrite(LCD_CS, LOW);
    SPI1.transfer(c);
    digitalWrite(LCD_CS, HIGH);
}
static void _dat(uint8_t d) {
    digitalWrite(LCD_DC, HIGH); digitalWrite(LCD_CS, LOW);
    SPI1.transfer(d);
    digitalWrite(LCD_CS, HIGH);
}
static void _win(int x0, int y0, int x1, int y1) {
    x0 += 40; x1 += 40; y0 += 53; y1 += 53;                  // RAM offset of this 240x135 panel
    const uint8_t cx[4] = { (uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1 };
    const uint8_t cy[4] = { (uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1 };
    _cmd(0x2A);
    digitalWrite(LCD_DC, HIGH); digitalWrite(LCD_CS, LOW); SPI1.transfer(cx, nullptr, 4); digitalWrite(LCD_CS, HIGH);
    _cmd(0x2B);
    digitalWrite(LCD_DC, HIGH); digitalWrite(LCD_CS, LOW); SPI1.transfer(cy, nullptr, 4); digitalWrite(LCD_CS, HIGH);
    _cmd(0x2C);
}

// Send a rectangle of the framebuffer to the panel as bulk transfers.
// Caller holds the SPI transaction.
static void pushRect(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_W) w = LCD_W - x;
    if (y + h > LCD_H) h = LCD_H - y;
    if (w <= 0 || h <= 0) return;

    _win(x, y, x + w - 1, y + h - 1);
    digitalWrite(LCD_DC, HIGH); digitalWrite(LCD_CS, LOW);
    const int rowsPerChunk = TXB_SIZE / (w * 2);
    for (int row = 0; row < h; ) {
        const int n = (h - row < rowsPerChunk) ? (h - row) : rowsPerChunk;
        uint8_t* d = TXB;
        for (int r = 0; r < n; r++) {
            const uint16_t* src = &FB[(y + row + r) * LCD_W + x];
            for (int c = 0; c < w; c++) { const uint16_t px = src[c]; *d++ = (uint8_t)(px >> 8); *d++ = (uint8_t)px; }
        }
        SPI1.transfer(TXB, nullptr, (size_t)(d - TXB));
        row += n;
    }
    digitalWrite(LCD_CS, HIGH);
}

static void widgetRect(int w, int& x, int& y, int& ww, int& hh) {
    if (w < N_KNOBS)       { x = cellX(PARAM_INFO[w].col); y = cellY(PARAM_INFO[w].row); ww = CELL_W; hh = CELL_H; }
    else if (w == W_TOP)   { x = 0; y = 0; ww = LCD_W; hh = TOP_H; }
    else                   { x = cellX(STAT_COL); y = cellY(2); ww = LCD_W - x; hh = CELL_H; }
}

// ---- Change detection: redraw a widget only when what it shows changed --------
static void scan(const SynthManager& s) {
    for (int i = 0; i < N_KNOBS; i++) {
        const uint16_t key = (uint16_t)(s.pv[i] | (i == _sel ? 0x100 : 0));
        if (_force || key != _knobKey[i]) { _knobKey[i] = key; drawKnob(s, i, i == _sel); _pending |= 1u << i; }
    }

    const uint8_t cpu = g_cpu_load_pct;
    const uint32_t topKey = (uint32_t)_sel | ((uint32_t)s.pv[_sel] << 5) |
                            ((uint32_t)(cpu >= 80 ? 2 : cpu >= 50 ? 1 : 0) << 12) |
                            ((uint32_t)(millis() < _midi_until) << 14) |
                            ((uint32_t)(uint8_t)s.patchName[0] << 16);
    if (_force || topKey != _topKey) { _topKey = topKey; drawTop(s); _pending |= 1u << W_TOP; }

    int pbq = (int)((s.ep.bend + 2.0f) * 8.0f + 0.5f);       // 0..32
    if (pbq < 0)  pbq = 0;
    if (pbq > 32) pbq = 32;
    const uint32_t statKey = (uint32_t)s.countedVoices() | ((uint32_t)pbq << 5) | ((uint32_t)s.glide() << 11);
    if (_force || statKey != _statKey) { _statKey = statKey; drawStatus(s); _pending |= 1u << W_STAT; }

    _force = false;
}

// ---- Public API ---------------------------------------------------------------
void ui_mark_all_dirty() { _force = true; }
void ui_midi_activity()  { _midi_until = millis() + 120; }

void ui_flush(const SynthManager& s) {
    if (_first) {                                            // first call: paint everything once
        _first = false;
        drawBackground();
        _force = true; scan(s);
        SPI1.beginTransaction(_spi);
        pushRect(0, 0, LCD_W, LCD_H);
        SPI1.endTransaction();
        _pending = 0;
        _lastScan = millis();
        return;
    }

    const uint32_t now = millis();
    if (now - _lastScan >= 20) { _lastScan = now; scan(s); }

    if (_pending) {                                          // send ONE rectangle, then return to loop()
        const int w = __builtin_ctz(_pending);
        _pending &= ~(1u << w);
        int x, y, ww, hh;
        widgetRect(w, x, y, ww, hh);
        SPI1.beginTransaction(_spi);
        pushRect(x, y, ww, hh);
        SPI1.endTransaction();
    }
}

// ---- Joystick / buttons ---------------------------------------------------------
static const int BPINS[7] = { BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_PRESS, BTN_A, BTN_B };
enum { B_UP, B_DOWN, B_LEFT, B_RIGHT, B_PRESS, B_A, B_B };
static bool     _bc[7], _bp[7];
static uint32_t _bdown[7], _brep[7], _bms = 0;

// true on the press edge, then (for repeat buttons) every 70 ms after 350 ms
static bool fire(int i, bool repeat, uint32_t now) {
    if (_bc[i] && !_bp[i]) { _bdown[i] = _brep[i] = now; return true; }
    if (repeat && _bc[i] && _bp[i] && now - _bdown[i] > 350 && now - _brep[i] >= 70) { _brep[i] = now; return true; }
    return false;
}

void ui_handle_buttons(SynthManager& s) {
    const uint32_t now = millis();
    if (now - _bms < 5) return;                              // 5 ms sampling = cheap debounce
    _bms = now;
    for (int i = 0; i < 7; i++) { _bp[i] = _bc[i]; _bc[i] = !digitalRead(BPINS[i]); }

    if (fire(B_LEFT,  true, now)) _sel = (_sel + N_KNOBS - 1) % N_KNOBS;
    if (fire(B_RIGHT, true, now)) _sel = (_sel + 1) % N_KNOBS;

    const bool up = fire(B_UP, true, now), down = fire(B_DOWN, true, now);
    if (up || down) {
        const int dir = up ? +1 : -1;
        int v = s.pv[_sel];
        if (_sel == P_WAVE) {                                // 4 discrete waveforms
            int w = (v >> 5) + dir;
            if (w < 0) w = 0;
            if (w > 3) w = 3;
            v = w * 32 + 16;
        } else {
            const int held = (int)(now - _bdown[up ? B_UP : B_DOWN]);
            const int step = (held > 700) ? 8 : 2;           // slow, then fast when held
            v += dir * step;
            if (v < 0)   v = 0;
            if (v > 127) v = 127;
        }
        s.setParam((uint8_t)_sel, (uint8_t)v);
    }

    if (fire(B_PRESS, false, now)) s.loadPatch((uint8_t)((s.patchIndex() + 1) % N_PATCHES));
    if (fire(B_A,     false, now)) s.panic();
    if (fire(B_B,     false, now)) s.setGlide(!s.glide());
}

// ---- Init -------------------------------------------------------------------------
void lcd_begin() {
    pinMode(LCD_CS, OUTPUT);  digitalWrite(LCD_CS, HIGH);
    pinMode(LCD_DC, OUTPUT);  digitalWrite(LCD_DC, HIGH);
    pinMode(LCD_RST, OUTPUT); digitalWrite(LCD_RST, HIGH);
    pinMode(LCD_BL, OUTPUT);  digitalWrite(LCD_BL, HIGH);
    for (int p : BPINS) pinMode(p, INPUT_PULLUP);
    initArcTables();

    SPI1.setTX(LCD_MOSI); SPI1.setSCK(LCD_SCK); SPI1.begin();
    SPI1.beginTransaction(_spi);

    digitalWrite(LCD_RST, LOW); delay(10); digitalWrite(LCD_RST, HIGH); delay(120);

    _cmd(0x01); delay(150);
    _cmd(0x11); delay(500);
    _cmd(0x3A); _dat(0x05);
    _cmd(0x36); _dat(0x70);
    _cmd(0xB2); _dat(0x0C); _dat(0x0C); _dat(0x00); _dat(0x33); _dat(0x33);
    _cmd(0xB7); _dat(0x35);
    _cmd(0xBB); _dat(0x19);
    _cmd(0xC0); _dat(0x2C);
    _cmd(0xC2); _dat(0x01);
    _cmd(0xC3); _dat(0x12);
    _cmd(0xC4); _dat(0x20);
    _cmd(0xC6); _dat(0x0F);
    _cmd(0xD0); _dat(0xA4); _dat(0xA1);
    _cmd(0x21); delay(10);
    _cmd(0x13); delay(10);
    _cmd(0x29); delay(20);

    memset(FB, 0, sizeof(FB));                               // clear the panel (one bulk push)
    pushRect(0, 0, LCD_W, LCD_H);
    SPI1.endTransaction();

    _first = true; _force = true; _pending = 0;
}
