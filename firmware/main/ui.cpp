#include "ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "port.h"

namespace ui {

Metrics   M;
M5Canvas* cv = nullptr;

static FontChoice s_font = FONT_NORMAL;

void setFont(FontChoice f) {
  s_font = f;
  // Both are fixed-width, which the char-count layout math relies on.
  if (f == FONT_COMPACT) cv->setFont(&fonts::Font0);          // 6x8
  else                   cv->setFont(&fonts::AsciiFont8x16);  // 8x16

  cv->setTextSize(1);
  M = layoutFor(M.w, M.h, cv->textWidth("M"), cv->fontHeight());
  cv->setTextSize(M.scale);
}

void begin(M5Canvas* canvas, int w, int h, FontChoice f) {
  cv  = canvas;
  M.w = w;
  M.h = h;
  setFont(f);
}

int rowsFor(int y, int rowH) { return rowsIn(M, y, rowH); }

int charsFor(int px) {
  int n = px / M.charW;
  return n < 1 ? 1 : n;
}

// -----------------------------------------------------------------------------
//  Frame + chrome
// -----------------------------------------------------------------------------

void beginFrame() {
  cv->fillSprite(C_BG);
  cv->setTextSize(M.scale);
  cv->setTextDatum(textdatum_t::top_left);
}

void endFrame() { cv->pushSprite(0, 0); }

void header(const char* title, const char* right, uint16_t rightCol) {
  cv->fillRect(0, 0, M.w, M.headerH, C_HDR_BG);
  cv->setTextSize(M.scale);
  cv->setTextDatum(textdatum_t::middle_left);
  cv->setTextColor(C_HDR_FG, C_HDR_BG);
  cv->drawString(title, M.pad * 2, M.headerH / 2);
  if (right) {
    cv->setTextDatum(textdatum_t::middle_right);
    cv->setTextColor(rightCol, C_HDR_BG);
    cv->drawString(right, M.w - M.pad * 2, M.headerH / 2);
  }
  cv->setTextDatum(textdatum_t::top_left);
}

void footer(const char* hint) {
  int y = M.h - M.footerH;
  cv->fillRect(0, y, M.w, M.footerH, C_HDR_BG);
  cv->setTextSize(M.scale);
  cv->setTextColor(C_DIM, C_HDR_BG);
  cv->setTextDatum(textdatum_t::middle_left);
  char buf[96];
  clip(buf, sizeof(buf), hint, charsFor(M.w - M.pad * 4));
  cv->setTextDatum(textdatum_t::top_left);
  text(M.pad * 2, y + (M.footerH - M.lineH) / 2, buf, C_DIM, C_HDR_BG);
}

void footerf(const char* fmt, ...) {
  char buf[96];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  footer(buf);
}

// -----------------------------------------------------------------------------
//  Text
// -----------------------------------------------------------------------------

static bool isArrow(char c) {
  return c == GLYPH_UP || c == GLYPH_DOWN || c == GLYPH_LEFT || c == GLYPH_RIGHT;
}

static void drawArrow(int x, int y, char code, uint16_t fg, uint16_t bg) {
  const int w = M.charW, h = M.lineH;
  cv->fillRect(x, y, w, h, bg);
  const int cx = x + w / 2, cy = y + h / 2;
  int r = (w < h ? w : h) / 2 - M.scale;
  if (r < 2) r = 2;
  switch (code) {
    case GLYPH_UP:    cv->fillTriangle(cx, cy - r, cx - r, cy + r, cx + r, cy + r, fg); break;
    case GLYPH_DOWN:  cv->fillTriangle(cx, cy + r, cx - r, cy - r, cx + r, cy - r, fg); break;
    case GLYPH_LEFT:  cv->fillTriangle(cx - r, cy, cx + r, cy - r, cx + r, cy + r, fg); break;
    case GLYPH_RIGHT: cv->fillTriangle(cx + r, cy, cx - r, cy - r, cx - r, cy + r, fg); break;
  }
}

void text(int x, int y, const char* s, uint16_t fg, uint16_t bg) {
  cv->setTextSize(M.scale);
  cv->setTextDatum(textdatum_t::top_left);
  cv->setTextColor(fg, bg);

  bool any = false;
  for (const char* p = s; *p; ++p) if (isArrow(*p)) { any = true; break; }
  if (!any) { cv->drawString(s, x, y); return; }

  // Mixed run: the fonts are fixed-width, so each cell is exactly charW wide.
  char run[96];
  int n = 0;
  for (const char* p = s; ; ++p) {
    if (*p == 0 || isArrow(*p)) {
      if (n) {
        run[n] = 0;
        cv->drawString(run, x, y);
        x += n * M.charW;
        n = 0;
      }
      if (*p == 0) break;
      drawArrow(x, y, *p, fg, bg);
      x += M.charW;
    } else if (n < (int)sizeof(run) - 1) {
      run[n++] = *p;
    }
  }
}

void textRight(int x, int y, const char* s, uint16_t fg, uint16_t bg) {
  cv->setTextSize(M.scale);
  cv->setTextDatum(textdatum_t::top_right);
  cv->setTextColor(fg, bg);
  cv->drawString(s, x, y);
  cv->setTextDatum(textdatum_t::top_left);
}

// Seven glyphs a second: fast enough not to test the reader's patience on a
// deep path, slow enough to read without chasing it.
static constexpr int MARQUEE_CPS = 7;

static uint32_t hashOf(const char* s) {
  uint32_t h = 2166136261u;                       // FNV-1a
  for (; *s; ++s) h = (h ^ (uint8_t)*s) * 16777619u;
  return h;
}

void marquee(int x, int y, int w, const char* s, uint16_t fg, uint16_t bg) {
  // The animation phase is derived from millis(), so only two words of state
  // are kept: which string is running, and when it started.
  static uint32_t lastKey = 0;
  static uint32_t startMs = 0;

  const int len    = (int)strlen(s);
  const int textPx = len * M.charW;

  if (textPx <= w) {
    // Nothing to scroll. Clearing the key matters: without it, stepping onto a
    // short name and back would resume the old run mid-cycle instead of
    // restarting from the head.
    lastKey = 0;
    text(x, y, s, fg, bg);
    return;
  }

  const uint32_t key = hashOf(s);
  const uint32_t now = millis();
  if (key != lastKey) { lastKey = key; startMs = now; }

  const int off = marqueeOffset(now - startMs, textPx, w, M.charW * MARQUEE_CPS);

  // Fixed-width font, so the window maps straight onto a slice of the string:
  // draw only the glyphs that can land inside it, however long the path is.
  char slice[96];
  const int first = off / M.charW;
  int take = len - first;
  const int room = charsFor(w) + 2;               // + the two partial edge cells
  if (take > room)                    take = room;
  if (take > (int)sizeof(slice) - 1)  take = (int)sizeof(slice) - 1;
  memcpy(slice, s + first, take);
  slice[take] = 0;

  // Clip so the partial leading glyph cannot bleed into the size column or
  // under the scrollbar; nothing else sets a clip, so clearing restores it all.
  cv->setClipRect(x, y, w, M.lineH);
  cv->fillRect(x, y, w, M.lineH, bg);
  cv->setTextSize(M.scale);
  cv->setTextDatum(textdatum_t::top_left);
  cv->setTextColor(fg, bg);
  cv->drawString(slice, x - (off % M.charW), y);
  cv->clearClipRect();
}

void center(int y, const char* s, uint16_t fg, int scaleMul) {
  cv->setTextSize(M.scale * scaleMul);
  cv->setTextDatum(textdatum_t::middle_center);
  cv->setTextColor(fg, C_BG);
  cv->drawString(s, M.w / 2, y);
  cv->setTextSize(M.scale);
  cv->setTextDatum(textdatum_t::top_left);
}

// Word wrap on spaces, hard-breaking anything longer than a line. Honours '\n'.
int wrap(int x, int y, int maxChars, const char* s, uint16_t fg) {
  if (maxChars < 1) maxChars = 1;
  char line[128];
  if (maxChars > (int)sizeof(line) - 1) maxChars = sizeof(line) - 1;

  const int limit = M.h - M.footerH - M.lineH;
  while (*s) {
    if (y > limit) break;          // never draw underneath the footer
    if (*s == '\n') { ++s; y += M.lineH; continue; }

    int take = 0;
    while (s[take] && s[take] != '\n' && take < maxChars) ++take;

    if (s[take] && s[take] != '\n' && take == maxChars) {
      int brk = take;
      while (brk > 0 && s[brk] != ' ') --brk;
      if (brk > 0) take = brk;
    }

    memcpy(line, s, take);
    line[take] = 0;
    text(x, y, line, fg);
    y += M.lineH;
    s += take;
    while (*s == ' ') ++s;
  }
  return y;
}

int ellipsize(char* dst, size_t sz, const char* s, int maxChars) {
  if (maxChars < 4) maxChars = 4;
  if ((size_t)maxChars > sz - 1) maxChars = (int)sz - 1;
  int len = (int)strlen(s);
  if (len <= maxChars) {
    strncpy(dst, s, sz - 1);
    dst[sz - 1] = 0;
    return len;
  }
  // Paths matter most at the tail, so drop the front.
  snprintf(dst, sz, "...%s", s + (len - (maxChars - 3)));
  return maxChars;
}

// Keeps the head of the string — for labels, where the front carries meaning.
int clip(char* dst, size_t sz, const char* s, int maxChars) {
  if (maxChars < 1) maxChars = 1;
  if ((size_t)maxChars > sz - 1) maxChars = (int)sz - 1;
  int len = (int)strlen(s);
  if (len <= maxChars) {
    strncpy(dst, s, sz - 1);
    dst[sz - 1] = 0;
    return len;
  }
  memcpy(dst, s, maxChars);
  dst[maxChars] = 0;
  if (maxChars >= 1) dst[maxChars - 1] = '.';
  return maxChars;
}

void human(uint64_t b, char* out, size_t sz) {
  if      (b < 1024ULL)                   snprintf(out, sz, "%uB",   (unsigned)b);
  else if (b < 1024ULL * 1024)            snprintf(out, sz, "%.1fK", b / 1024.0);
  else if (b < 1024ULL * 1024 * 1024)     snprintf(out, sz, "%.1fM", b / 1048576.0);
  else                                    snprintf(out, sz, "%.2fG", b / 1073741824.0);
}

// -----------------------------------------------------------------------------
//  Widgets
// -----------------------------------------------------------------------------

void scrollbar(int x, int y, int h, int total, int visible, int top) {
  if (total <= visible || h <= 0) return;
  int w = 2 * M.scale;
  cv->fillRect(x, y, w, h, C_TRACK);
  int knob = h * visible / total;
  if (knob < 3 * M.scale) knob = 3 * M.scale;
  if (knob > h) knob = h;
  int span = total - visible;
  int ky   = y + (span > 0 ? (h - knob) * top / span : 0);
  cv->fillRect(x, ky, w, knob, C_HDR_FG);
}

void progress(int x, int y, int w, int h, float frac, uint16_t col) {
  if (frac < 0) frac = 0;
  if (frac > 1) frac = 1;
  cv->drawRect(x, y, w, h, C_DIM);
  int fill = (int)((w - 2) * frac);
  if (fill > 0) cv->fillRect(x + 1, y + 1, fill, h - 2, col);
}

void usageBar(int x, int y, int w, uint64_t used, uint64_t total) {
  char u[16], t[16], buf[48];
  human(used, u, sizeof(u));
  human(total, t, sizeof(t));
  snprintf(buf, sizeof(buf), "%s / %s", u, t);

  // The label is sized first and the bar takes what is left, so neither runs
  // off the panel however large the card is.
  const int labelW = (int)strlen(buf) * M.charW;
  const int avail  = M.w - x - M.pad * 2;
  if (w > avail - labelW - M.pad * 2) w = avail - labelW - M.pad * 2;
  if (w < M.charW * 4) w = M.charW * 4;

  const int h = M.lineH;
  const float frac = total ? (float)((double)used / (double)total) : 0.0f;
  progress(x, y, w, h, frac, frac > 0.9f ? C_ERR : (frac > 0.7f ? C_WARN : C_OK));
  text(x + w + M.pad * 2, y, buf, C_DIM);
}

// -----------------------------------------------------------------------------
//  Menu
// -----------------------------------------------------------------------------

const char* menu(int y0, const MenuItem* items, int n, int idx, int& scroll) {
  bool anySub = false;
  for (int i = 0; i < n; ++i) if (items[i].sub) anySub = true;

  // Two-line rows are only worth it while enough of them still fit; on a short
  // panel with big text they would leave two items visible out of five.
  const int rhSub   = M.lineH * 2 + 4 * M.scale;
  const int rhPlain = M.lineH + 6 * M.scale;
  const bool inlineSub = anySub && rowsFor(y0, rhSub) >= 3;
  const int  rh   = inlineSub ? rhSub : rhPlain;
  const int  rows = rowsFor(y0, rh);

  if (idx < scroll)         scroll = idx;
  if (idx >= scroll + rows) scroll = idx - rows + 1;
  if (scroll > n - rows)    scroll = n - rows;
  if (scroll < 0)           scroll = 0;

  const int  barX  = M.w - 2 * M.scale - M.pad;
  const bool bar   = n > rows;
  const int  textR = bar ? barX - M.pad : M.w - M.pad;

  char buf[96];
  for (int r = 0; r < rows && scroll + r < n; ++r) {
    const MenuItem& it = items[scroll + r];
    const int  i   = scroll + r;
    const int  y   = y0 + r * rh;
    const bool sel = (i == idx);

    if (sel) cv->fillRect(0, y, M.w, rh - M.scale, C_SEL_BG);
    const uint16_t bg = sel ? C_SEL_BG : C_BG;
    const uint16_t fg = sel ? C_SEL_FG : (it.accent ? it.accent : C_FG);

    int x = M.pad * 2;
    text(x, y + M.scale * 2, sel ? ">" : " ", sel ? C_HDR_FG : fg, bg);
    x += M.charW * 2;

    if (it.check >= 0) {
      snprintf(buf, sizeof(buf), "[%c]", it.check ? 'x' : ' ');
      text(x, y + M.scale * 2, buf, it.check ? C_OK : C_MUTE, bg);
      x += M.charW * 4;
    }

    int labelR = textR;
    if (it.right) {
      const int rw = (int)strlen(it.right) * M.charW;
      textRight(textR, y + M.scale * 2, it.right, sel ? C_SEL_FG : (it.rightCol ? it.rightCol : C_DIM), bg);
      labelR = textR - rw - M.charW;
    }
    clip(buf, sizeof(buf), it.label, charsFor(labelR - x));
    text(x, y + M.scale * 2, buf, fg, bg);

    if (inlineSub && it.sub) {
      ellipsize(buf, sizeof(buf), it.sub, charsFor(textR - x));
      text(x, y + M.scale * 2 + M.lineH, buf, sel ? C_DIM : C_MUTE, bg);
    }
  }

  if (bar) scrollbar(barX, y0, rows * rh, n, rows, scroll);

  // Nothing was lost if the sublabels were drawn; otherwise hand the selected
  // one back so the caller can show it in the footer.
  return inlineSub ? nullptr : (idx >= 0 && idx < n ? items[idx].sub : nullptr);
}

}  // namespace ui
