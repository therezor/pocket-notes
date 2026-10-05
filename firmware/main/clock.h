// Wall clock without a battery-backed RTC. Order of preference:
//   1. A hardware RTC if M5Unified finds one on this board.
//   2. NTP over WiFi (SSID in Settings or settings.ini): one sync in the background at boot or on
//      demand, then WiFi is shut down again. The time then runs until power-off (and the RTC, if
//      any, is set from it).
//   3. No clock: notes get only their sequence number; nothing in the app depends on the date.
#pragma once
#include <stdint.h>

namespace clk {

void begin();           // reads the RTC; starts an NTP sync if WiFi is configured
bool known();           // the time is valid
void syncNow();         // start an NTP sync in the background (no-op while one is running)
bool busy();
const char* status();   // short text for the header / settings: "RTC", "NTP", "syncing", ...
bool today(char out[11]);   // YYYY-MM-DD local; false when unknown
bool hhmm(char out[6]);     // HH:MM local; false when unknown

}  // namespace clk
