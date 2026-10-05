#include "clock.h"

#include <string.h>
#include <sys/time.h>
#include <time.h>

#include <M5Unified.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_netif_sntp.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#include "settings.h"

static const char* TAG = "clock";

namespace clk {

enum State : uint8_t { ST_NONE, ST_RTC, ST_NTP, ST_SYNCING, ST_FAILED, ST_NOWIFI };
static volatile State s_state = ST_NONE;
static volatile bool s_busy = false;
static bool s_netInit = false;
static esp_netif_t* s_netif = nullptr;
static EventGroupHandle_t s_ev = nullptr;
static constexpr int EV_IP = 1, EV_FAIL = 2;

bool known() { return time(nullptr) > 1735689600; }   // after 2025-01-01

static bool local(struct tm& t) {
  if (!known()) return false;
  time_t now = time(nullptr) + (time_t)settings::v.utcOffsetMin * 60;
  gmtime_r(&now, &t);
  return true;
}

bool today(char out[11]) {
  struct tm t;
  if (!local(t)) { out[0] = 0; return false; }
  snprintf(out, 11, "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  return true;
}

bool hhmm(char out[6]) {
  struct tm t;
  if (!local(t)) { out[0] = 0; return false; }
  snprintf(out, 6, "%02d:%02d", t.tm_hour, t.tm_min);
  return true;
}

bool busy() { return s_busy; }

const char* status() {
  switch (s_state) {
    case ST_RTC:     return "RTC";
    case ST_NTP:     return "NTP";
    case ST_SYNCING: return "syncing";
    case ST_FAILED:  return known() ? "NTP (old)" : "sync failed";
    case ST_NOWIFI:  return "no WiFi set";
    default:         return known() ? "set" : "no clock";
  }
}

static void onEvent(void*, esp_event_base_t base, int32_t id, void*) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) xEventGroupSetBits(s_ev, EV_FAIL);
  if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) xEventGroupSetBits(s_ev, EV_IP);
}

static void ntpTask(void*) {
  const State before = s_state;
  s_state = ST_SYNCING;
  bool ok = false;
  if (!s_netInit) {
    esp_netif_init();
    esp_event_loop_create_default();
    s_netif = esp_netif_create_default_wifi_sta();
    s_ev = xEventGroupCreate();
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, onEvent, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, onEvent, nullptr);
    s_netInit = true;
  }
  wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
  if (esp_wifi_init(&ic) == ESP_OK) {
    wifi_config_t wc = {};
    strncpy((char*)wc.sta.ssid, settings::v.wifiSsid.c_str(), sizeof(wc.sta.ssid) - 1);
    strncpy((char*)wc.sta.password, settings::v.wifiPass.c_str(), sizeof(wc.sta.password) - 1);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    xEventGroupClearBits(s_ev, EV_IP | EV_FAIL);
    if (esp_wifi_start() == ESP_OK && esp_wifi_connect() == ESP_OK) {
      EventBits_t b = xEventGroupWaitBits(s_ev, EV_IP | EV_FAIL, pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
      if (b & EV_IP) {
        esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        if (esp_netif_sntp_init(&sc) == ESP_OK) {
          ok = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(8000)) == ESP_OK;
          esp_netif_sntp_deinit();
        }
      }
      esp_wifi_disconnect();
      esp_wifi_stop();
    }
    esp_wifi_deinit();   // give the radio's RAM back to the model
  }
  if (ok && M5.Rtc.isEnabled()) {
    time_t now = time(nullptr);
    struct tm t;
    gmtime_r(&now, &t);
    M5.Rtc.setDateTime(&t);   // the RTC keeps UTC, like the system clock
  }
  ESP_LOGI(TAG, "NTP sync %s", ok ? "ok" : "failed");
  s_state = ok ? ST_NTP : (before == ST_RTC ? ST_RTC : ST_FAILED);
  s_busy = false;
  vTaskDelete(nullptr);
}

void syncNow() {
  if (s_busy) return;
  if (settings::v.wifiSsid.empty()) { if (s_state != ST_RTC) s_state = ST_NOWIFI; return; }
  s_busy = true;
  if (xTaskCreatePinnedToCore(ntpTask, "ntp", 4096, nullptr, 2, nullptr, 0) != pdPASS) s_busy = false;
}

void begin() {
  if (M5.Rtc.isEnabled()) {
    auto dt = M5.Rtc.getDateTime();
    if (dt.date.year >= 2025) {
      struct tm t = {};
      t.tm_year = dt.date.year - 1900;
      t.tm_mon = dt.date.month - 1;
      t.tm_mday = dt.date.date;
      t.tm_hour = dt.time.hours;
      t.tm_min = dt.time.minutes;
      t.tm_sec = dt.time.seconds;
      struct timeval tv = {mktime(&t), 0};   // TZ is UTC, so mktime is timegm here
      settimeofday(&tv, nullptr);
      s_state = ST_RTC;
      ESP_LOGI(TAG, "time from RTC");
    }
  }
  if (s_state != ST_RTC && !settings::v.wifiSsid.empty()) syncNow();
}

}  // namespace clk
