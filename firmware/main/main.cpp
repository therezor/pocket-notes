// Pocket Inbox — offline quick-capture for the Cardputer, powered by TinyDecide.
//
// Skeleton: boots the display and keyboard and echoes captured lines. The engine
// (components/tinydecide, M1) and the capture loop with SD storage (M2) come next;
// see docs/PLAN.md.

#include <M5Unified.h>
#include <string>
#include "keyboard/Keyboard.h"
#include "ui.h"
#include "port.h"
#include "tinydecide.h"

static ChatUI         ui;
static Keyboard_Class Keyboard;   // vendored M5Cardputer driver (main/keyboard/)

static void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Keyboard.begin();    // picks IOMatrix or TCA8418 reader from M5.getBoard()
  ui.begin();
  ui.ready();
  ui.statusf("Pocket Inbox  model %s (not ported yet)", TD_MODEL_VARIANT);
}

static void loop() {
  M5.update();
  Keyboard.updateKeyList();
  Keyboard.updateKeysState();
  if (!(Keyboard.isChange() && Keyboard.isPressed())) return;
  auto st = Keyboard.keysState();
  if (st.fn) {
    // fn+; / fn+. are the up/down arrows on the Cardputer keyboard.
    for (char c : st.word) {
      if (c == ';') ui.scrollChat(+2);
      if (c == '.') ui.scrollChat(-2);
    }
    return;
  }
  for (char c : st.word) ui.onChar(c);
  if (st.del) ui.onBackspace();
  if (st.enter) {
    std::string line = ui.takeInput();
    if (line.length()) {
      ui.appendUser(line);
      ui.beginBotReply();
      ui.appendBot("[not filed: engine arrives in M1]");
      ui.endBotReply(0, 0.0f, 0, 0);
    }
  }
}

extern "C" void app_main(void) {
  setup();
  for (;;) {
    loop();
    vTaskDelay(1);    // 1 ms tick (CONFIG_FREERTOS_HZ=1000) is plenty for keyboard polling
  }
}
