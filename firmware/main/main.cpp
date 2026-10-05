// Pocket Notes: a pocket note-taking app for the M5Stack Cardputer (original and ADV).
// Notes are sorted into categories; the on-device TinyDecide model suggests the category for each
// new note and learns from what you file. See README.md and docs/PLAN.md.
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "app.h"

extern "C" void app_main(void) {
  app::begin();
  for (;;) {
    app::loop();
    vTaskDelay(1);   // 1 ms tick (CONFIG_FREERTOS_HZ=1000) is plenty for keyboard polling
  }
}
