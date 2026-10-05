#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <esp_log.h>
#include <nvs.h>
#include <nvs_flash.h>

namespace settings {

Values v;
static nvs_handle_t s_h = 0;
static uint32_t s_next = 1;

static std::string getStr(const char* k) {
  size_t n = 0;
  if (nvs_get_str(s_h, k, nullptr, &n) != ESP_OK || n == 0) return "";
  std::string s(n, '\0');
  nvs_get_str(s_h, k, &s[0], &n);
  s.resize(n ? n - 1 : 0);
  return s;
}

void begin() {
  esp_err_t e = nvs_flash_init();
  if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
  }
  if (nvs_open("notes", NVS_READWRITE, &s_h) != ESP_OK) return;
  uint8_t u8;
  int32_t i32;
  uint32_t u32;
  if (nvs_get_u8(s_h, "backend", &u8) == ESP_OK) v.backend = u8;
  if (nvs_get_i32(s_h, "utc", &i32) == ESP_OK) v.utcOffsetMin = i32;
  if (nvs_get_u8(s_h, "compact", &u8) == ESP_OK) v.compact = u8;
  if (nvs_get_u8(s_h, "ai", &u8) == ESP_OK) v.ai = u8;
  if (nvs_get_u8(s_h, "hidedone", &u8) == ESP_OK) v.hideDone = u8;
  if (nvs_get_u32(s_h, "next_id", &u32) == ESP_OK && u32 > 0) s_next = u32;
  if (nvs_get_u32(s_h, "learn_from", &u32) == ESP_OK) v.learnFrom = u32;
  v.wifiSsid = getStr("ssid");
  v.wifiPass = getStr("pass");
}

void save() {
  if (!s_h) return;
  nvs_set_u8(s_h, "backend", v.backend);
  nvs_set_i32(s_h, "utc", v.utcOffsetMin);
  nvs_set_u8(s_h, "compact", v.compact);
  nvs_set_u8(s_h, "ai", v.ai);
  nvs_set_u8(s_h, "hidedone", v.hideDone);
  nvs_set_u32(s_h, "learn_from", v.learnFrom);
  nvs_set_str(s_h, "ssid", v.wifiSsid.c_str());
  nvs_set_str(s_h, "pass", v.wifiPass.c_str());
  nvs_commit(s_h);
}

uint32_t peekId() { return s_next; }

uint32_t takeId() {
  uint32_t id = s_next++;
  if (s_h) { nvs_set_u32(s_h, "next_id", s_next); nvs_commit(s_h); }
  return id;
}

void bumpNextId(uint32_t atLeast) {
  if (atLeast <= s_next) return;
  s_next = atLeast;
  if (s_h) { nvs_set_u32(s_h, "next_id", s_next); nvs_commit(s_h); }
}

static std::string trim(const char* a, const char* b) {
  while (a < b && (*a == ' ' || *a == '\t')) a++;
  while (b > a && (b[-1] == ' ' || b[-1] == '\t' || b[-1] == '\r' || b[-1] == '\n')) b--;
  return std::string(a, b);
}

// key = value lines; '#' comments. Only keys that are present (and non-empty) override.
void importIni(const char* path) {
  FILE* f = fopen(path, "r");
  if (!f) return;
  char line[160];
  bool changed = false;
  while (fgets(line, sizeof(line), f)) {
    char* eq = strchr(line, '=');
    if (line[0] == '#' || !eq) continue;
    std::string k = trim(line, eq), val = trim(eq + 1, eq + strlen(eq));
    if (val.empty()) continue;
    if (k == "wifi_ssid" && val != v.wifiSsid) { v.wifiSsid = val; changed = true; }
    else if (k == "wifi_pass" && val != v.wifiPass) { v.wifiPass = val; changed = true; }
    else if (k == "utc_offset_min") { int x = atoi(val.c_str()); if (x != v.utcOffsetMin) { v.utcOffsetMin = x; changed = true; } }
  }
  fclose(f);
  if (changed) { save(); ESP_LOGI("settings", "imported %s", path); }
}

}  // namespace settings
