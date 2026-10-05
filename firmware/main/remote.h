// Remote control over the USB serial port, for scripted tests (tools/remote.py) and screenshots.
// Line commands from the host:
//   key <name>...   inject keys: up down left right enter back esc space tab, fn+<name>, or one char
//   type <text>     inject each character of <text>
//   shot <name>     reply "<<SHOT name w h\n" + the canvas as raw RGB565 (MSB first)
//   dump            reply the note index, one "##" line per note
//   heap            reply free / largest-block heap
//   demo 1|0        switch to / wipe a throwaway notes folder for screenshots
// Replies are lines starting with "##" (or the binary shot), mixed with the normal log output.
#pragma once
#include "keys.h"

#include <M5Unified.h>

namespace remote {
void begin(M5Canvas* canvas);
bool poll(keys::KeyEvent& out);   // next injected key, if any
}
