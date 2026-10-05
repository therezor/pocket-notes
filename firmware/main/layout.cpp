#include "layout.h"

namespace ui {

Metrics layoutFor(int w, int h, int glyphW, int glyphH) {
  Metrics m{};
  m.w = w;
  m.h = h;

  if (glyphW < 1) glyphW = 6;
  if (glyphH < 1) glyphH = 8;

  // Double the text only when it is small *relative to this panel* — i.e. the
  // unscaled font would fit well over 24 rows and 45 columns. A 6x8 font on a
  // 320x240 panel qualifies; a 8x16 font on the same panel already reads fine.
  m.scale = (h >= glyphH * 24 && w >= glyphW * 45) ? 2 : 1;

  m.charW = glyphW * m.scale;
  m.lineH = glyphH * m.scale;
  m.pad   = 2 * m.scale;

  m.headerH = m.lineH + 3 * m.pad;
  m.footerH = m.lineH + 2 * m.pad;
  m.rowH    = m.lineH + 3 * m.scale;

  // A short panel would otherwise spend its whole height on chrome and leave
  // no room for content, so give the body at least one row and shrink the
  // header and footer to fit around it.
  int chrome = m.headerH + m.footerH;
  if (h - chrome < m.rowH) {
    m.headerH = m.lineH + m.pad;
    m.footerH = m.lineH + m.pad;
    chrome    = m.headerH + m.footerH;
  }
  if (h - chrome < m.rowH) {
    m.footerH = 0;
    chrome    = m.headerH;
  }

  m.bodyY = m.headerH + m.pad;
  m.bodyH = h - chrome - 2 * m.pad;
  if (m.bodyH < m.rowH) m.bodyH = m.rowH;
  return m;
}

int rowsIn(const Metrics& m, int y, int rowH) {
  if (rowH < 1) rowH = m.rowH;
  int n = (m.h - m.footerH - y) / rowH;
  return n < 1 ? 1 : n;
}

int marqueeOffset(unsigned long elapsedMs, int textPx, int boxPx, int pxPerSec) {
  if (boxPx < 1 || pxPerSec < 1) return 0;
  const int span = textPx - boxPx;
  if (span <= 0) return 0;                 // it fits; nothing to scroll

  const unsigned long travel = (unsigned long)span * 1000UL / (unsigned long)pxPerSec;
  const unsigned long cycle  = MARQUEE_HOLD_MS + travel + MARQUEE_HOLD_MS;

  unsigned long t = elapsedMs % cycle;
  if (t < MARQUEE_HOLD_MS) return 0;       // holding at the head
  t -= MARQUEE_HOLD_MS;
  if (t >= travel) return span;            // holding at the tail

  return (int)(t * (unsigned long)pxPerSec / 1000UL);
}

}  // namespace ui
