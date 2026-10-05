// Settings, kept in NVS so they survive without an SD card. settings.ini in the notes folder (if
// present) is imported at boot, so WiFi can also be set on a PC.
#pragma once
#include <stdint.h>

#include <string>

namespace settings {

struct Values {
  uint8_t backend = 0;        // store::Backend chosen by the user (0 = not chosen yet)
  std::string wifiSsid, wifiPass;
  int utcOffsetMin = 0;       // minutes east of UTC, applied to NTP time
  bool compact = false;       // 6x8 font instead of 8x16
  bool ai = true;             // suggest a category for new notes
  bool hideDone = false;      // hide completed items in checklist categories
  uint32_t learnFrom = 0;     // notes with a lower id are not used as examples ("forget learning")
};

extern Values v;

void begin();                 // nvs_flash_init + load
void save();
void importIni(const char* path);

uint32_t takeId();            // next note id (persisted)
uint32_t peekId();            // the id the next note will get
void bumpNextId(uint32_t atLeast);

}  // namespace settings
