// =============================================================================
//  layout.h — screen geometry, derived rather than hardcoded
//
//  Split out from ui.h and kept free of Arduino/M5GFX so the arithmetic can be
//  checked on the host at sizes no Cardputer has (see test/test_layout).
// =============================================================================
#pragma once

namespace ui {

struct Metrics {
  int w, h;
  int scale;      // text size multiplier chosen from the display size
  int charW;      // width of one glyph at `scale`
  int lineH;      // height of one text line at `scale`
  int pad;
  int headerH, footerH;
  int rowH;       // one list row
  int bodyY;      // first usable y below the header
  int bodyH;      // usable height between header and footer
};

// glyphW/glyphH are the unscaled font metrics (6x8 for Font0).
Metrics layoutFor(int w, int h, int glyphW, int glyphH);

// Rows of height rowH that fit between y and the top of the footer.
int rowsIn(const Metrics& m, int y, int rowH);

// How long a marquee pauses at each end before and after the scroll.
constexpr unsigned long MARQUEE_HOLD_MS = 800;

// Marquee phase. How far left (px) to shift a textPx-wide string inside a
// boxPx-wide window, elapsedMs into the animation: 0 while it holds at the
// head, a linear ramp while it scrolls, then textPx - boxPx while it holds at
// the tail, and back to 0 on the next cycle. Returns 0 when the text fits.
int marqueeOffset(unsigned long elapsedMs, int textPx, int boxPx, int pxPerSec);

}  // namespace ui
