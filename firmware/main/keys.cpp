#include "keys.h"

#include <M5Unified.h>

#include "keyboard/Keyboard.h"
#include "port.h"

namespace keys {

static Keyboard_Class s_kb;

void begin() { s_kb.begin(); }   // picks the IO-matrix or TCA8418 reader from M5.getBoard()

const char* boardName() {
  switch (M5.getBoard()) {
    case m5::board_t::board_M5CardputerADV: return "Cardputer ADV";
    case m5::board_t::board_M5Cardputer:    return "Cardputer";
    default:                                return "ESP32";
  }
}

// Auto-repeat: the driver only reports state changes, so without this a held key moves one row.
// Only movement keys and backspace repeat; a repeated Enter would confirm twice.
static constexpr uint32_t REPEAT_DELAY_MS = 400;
static constexpr uint32_t REPEAT_RATE_MS  = 60;

static bool     s_held = false;
static uint32_t s_heldSince = 0, s_lastFire = 0, s_lastSig = 0;

// Edge detection over the set of keys held down; the driver's isChange() only compares counts.
static uint32_t keySignature() {
  const auto& k = s_kb.keyList();
  uint32_t h = 2166136261u ^ (uint32_t)k.size();
  for (const auto& p : k) h = (h ^ (uint32_t)((p.y << 8) | p.x)) * 16777619u;
  return h;
}

KeyEvent poll() {
  KeyEvent k;
  s_kb.updateKeyList();
  s_kb.updateKeysState();

  const uint32_t sig = keySignature();
  const bool changed = sig != s_lastSig;
  s_lastSig = sig;
  if (!s_kb.isPressed()) { s_held = false; return k; }

  const uint32_t now = millis();
  bool fire = false;
  if (changed) {
    s_held = true; s_heldSince = now; s_lastFire = now;
    fire = true;
  } else if (s_held && now - s_heldSince >= REPEAT_DELAY_MS && now - s_lastFire >= REPEAT_RATE_MS) {
    s_lastFire = now;
    fire = true;
  }
  if (!fire) return k;

  auto& st = s_kb.keysState();
  k.any = true;
  k.repeat = !changed;
  k.fn = st.fn;
  if (st.enter) k.enter = true;
  if (st.del) k.back = true;
  if (st.tab) k.tab = true;
  for (char c : st.word) {
    switch (c) {
      case ';': k.up = true; break;
      case '.': k.down = true; break;
      case ',': k.left = true; break;
      case '/': k.right = true; break;
      case ' ': k.space = true; break;
      case '`': k.esc = true; break;
      default: break;
    }
    if (!k.ch && c >= 0x20 && c < 0x7F) k.ch = c;
  }
  if (k.repeat) {
    const bool moved = k.up || k.down || k.left || k.right || k.back;
    if (!moved) return KeyEvent{};
    k.enter = k.esc = k.space = k.tab = false;
    // Held printable keys other than the arrows do not repeat; a held backspace does.
    if (!(k.up || k.down || k.left || k.right)) k.ch = 0;
  }
  return k;
}

}  // namespace keys
