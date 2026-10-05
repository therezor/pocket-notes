// Keyboard -> normalized key events. Port of ../esp32_cleaner/src/hal/hal_cardputer.cpp pollKeys()
// on top of the vendored M5Cardputer keyboard driver (keyboard/).
#pragma once
#include <stdint.h>

namespace keys {

struct KeyEvent {
  bool up = 0, down = 0, left = 0, right = 0;   // ; . , /  (with or without fn)
  bool enter = 0, back = 0, esc = 0;           // enter, backspace, ` (esc)
  bool space = 0, tab = 0, fn = 0;
  char ch = 0;        // printable character typed (raw, before nav mapping), 0 if none
  bool any = 0;
  bool repeat = 0;    // synthesized by auto-repeat rather than a fresh press
};

void begin();
KeyEvent poll();      // one event per call (all-false when idle)
const char* boardName();

}  // namespace keys
