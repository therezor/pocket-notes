// =============================================================================
//  ui.h — theme, layout metrics and widgets
//
//  Nothing here hardcodes a screen size. ui::begin() measures the active font
//  and fills ui::M; every screen positions itself from those numbers, so the
//  same code lays out on a 240x135 Cardputer and a 320x240 Core.
//
//  Ported from ../esp32_cleaner/src/ui.h (same palette, fonts and widgets) so the
//  two apps look alike; only the includes changed for the ESP-IDF build.
// =============================================================================
#pragma once

#include <M5Unified.h>
#include <stdint.h>
#include "layout.h"

namespace ui {

// ---- Palette (RGB565) -------------------------------------------------------
// A dark slate scheme rather than pure black: secondary text sits well clear of
// the background, and the selection bar is visible instead of near-invisible.
constexpr uint16_t C_BG     = 0x0862;   // #0B0E14  page
constexpr uint16_t C_PANEL  = 0x10C4;   // #151A23  raised block
constexpr uint16_t C_HDR_BG = 0x1926;   // #1E2530  header / footer bar
constexpr uint16_t C_FG     = 0xE77E;   // #E8EDF2  primary text
constexpr uint16_t C_DIM    = 0xADB9;   // #A8B6C8  secondary text, still legible
constexpr uint16_t C_MUTE   = 0x6B6D;   // #6B7280  disabled / unselected only
constexpr uint16_t C_HDR_FG = 0x3DFF;   // #38BDF8  accent
constexpr uint16_t C_SEL_BG = 0x220D;   // #22406B  selection bar
constexpr uint16_t C_SEL_FG = 0xFFFF;   // white on the selection bar
constexpr uint16_t C_OK     = 0x4EF0;   // #4ADE80
constexpr uint16_t C_WARN   = 0xFDE4;   // #FBBF24
constexpr uint16_t C_ERR    = 0xFB8E;   // #F87171  softer than pure red
constexpr uint16_t C_BAR    = 0x3DFF;
constexpr uint16_t C_TRACK  = 0x2988;   // #2A3341  scrollbar / bar track

// Arrow markers. The bitmap fonts have no glyphs at these codes, so ui::text
// draws them as triangles sized to the current text cell — boards can put them
// in key hints and get a real arrow whatever font is active.
constexpr char GLYPH_UP    = '\x18';
constexpr char GLYPH_DOWN  = '\x19';
constexpr char GLYPH_RIGHT = '\x1A';
constexpr char GLYPH_LEFT  = '\x1B';

// Text is 8x16 by default; Compact drops to 6x8 to trade legibility for rows.
enum FontChoice : uint8_t { FONT_NORMAL, FONT_COMPACT };

extern Metrics   M;
extern M5Canvas* cv;

void begin(M5Canvas* canvas, int w, int h, FontChoice f = FONT_NORMAL);
void setFont(FontChoice f);   // re-measures and re-derives the layout

int rowsFor(int y, int rowH);         // rows that fit between y and the footer
int charsFor(int px);                 // glyphs that fit in px pixels

// ---- frame + chrome --------------------------------------------------------
void beginFrame();
void endFrame();
void header(const char* title, const char* right = nullptr, uint16_t rightCol = C_DIM);
void footer(const char* hint);
void footerf(const char* fmt, ...);

// ---- text ------------------------------------------------------------------
void text(int x, int y, const char* s, uint16_t fg, uint16_t bg = C_BG);
void textRight(int x, int y, const char* s, uint16_t fg, uint16_t bg = C_BG);
// Text in a box w pixels wide, scrolled sideways when it is too long to fit.
// One marquee may be live at a time: the phase lives in a single slot keyed on
// the string, which is all the app needs with one highlighted row per screen.
void marquee(int x, int y, int w, const char* s, uint16_t fg, uint16_t bg = C_BG);
void center(int y, const char* s, uint16_t fg, int scaleMul = 1);
int  wrap(int x, int y, int maxChars, const char* s, uint16_t fg);  // -> next y
int  ellipsize(char* dst, size_t sz, const char* s, int maxChars);  // left "..."
int  clip(char* dst, size_t sz, const char* s, int maxChars);       // right cut
void human(uint64_t bytes, char* out, size_t sz);

// ---- widgets ---------------------------------------------------------------
void scrollbar(int x, int y, int h, int total, int visible, int top);
void progress(int x, int y, int w, int h, float frac, uint16_t col = C_BAR);
void usageBar(int x, int y, int w, uint64_t used, uint64_t total);

struct MenuItem {
  const char* label;
  const char* sub    = nullptr;   // dim second line
  int8_t      check  = -1;        // <0 none, 0 unchecked, 1 checked
  uint16_t    accent = 0;         // 0 -> default foreground
  const char* right  = nullptr;   // right-aligned value (counts, confidence)
  uint16_t    rightCol = 0;       // 0 -> C_DIM
};

const char* menu(int y, const MenuItem* items, int n, int idx, int& scroll);

}  // namespace ui
