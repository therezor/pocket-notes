#include "remote.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <deque>
#include <string>

#include <M5Unified.h>
#include <driver/usb_serial_jtag.h>
#include <driver/usb_serial_jtag_vfs.h>
#include <esp_heap_caps.h>

#include "ai.h"
#include "settings.h"
#include "store.h"

namespace remote {

static M5Canvas* s_cv = nullptr;
static bool s_ok = false;
static std::string s_line;
static std::deque<keys::KeyEvent> s_q;

void begin(M5Canvas* canvas) {
  s_cv = canvas;
  usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
  cfg.rx_buffer_size = 1024;
  cfg.tx_buffer_size = 4096;
  if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) return;
  usb_serial_jtag_vfs_use_driver();   // keep printf / ESP_LOG on the same port
  s_ok = true;
}

static void reply(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void reply(const char* fmt, ...) {
  char b[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof(b) - 1, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (n > (int)sizeof(b) - 2) n = sizeof(b) - 2;
  b[n++] = '\n';
  usb_serial_jtag_write_bytes(b, n, pdMS_TO_TICKS(200));
}

static keys::KeyEvent named(const std::string& tok) {
  keys::KeyEvent k;
  k.any = true;
  std::string t = tok;
  if (t.compare(0, 3, "fn+") == 0) { k.fn = true; t = t.substr(3); }
  if (t == "up") { k.up = true; k.ch = ';'; }
  else if (t == "down") { k.down = true; k.ch = '.'; }
  else if (t == "left") { k.left = true; k.ch = ','; }
  else if (t == "right") { k.right = true; k.ch = '/'; }
  else if (t == "enter") k.enter = true;
  else if (t == "back") k.back = true;
  else if (t == "esc") { k.esc = true; k.ch = '`'; }
  else if (t == "space") { k.space = true; k.ch = ' '; }
  else if (t == "tab") k.tab = true;
  else if (t.size() == 1) k.ch = t[0];
  else k.any = false;
  return k;
}

static keys::KeyEvent charKey(char c) {
  keys::KeyEvent k;
  k.any = true;
  k.ch = c;
  switch (c) {
    case ';': k.up = true; break;
    case '.': k.down = true; break;
    case ',': k.left = true; break;
    case '/': k.right = true; break;
    case ' ': k.space = true; break;
    case '`': k.esc = true; break;
  }
  return k;
}

static void shot(const std::string& name) {
  if (!s_cv) return;
  const int w = s_cv->width(), h = s_cv->height();
  reply("<<SHOT %s %d %d", name.empty() ? "shot" : name.c_str(), w, h);
  const uint8_t* buf = (const uint8_t*)s_cv->getBuffer();
  const size_t n = (size_t)w * h * 2;
  for (size_t off = 0; off < n; off += 1024) {
    size_t c = n - off < 1024 ? n - off : 1024;
    usb_serial_jtag_write_bytes(buf + off, c, pdMS_TO_TICKS(1000));
  }
  usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(2000));
  reply("");
}

static void command(const std::string& l) {
  size_t sp = l.find(' ');
  std::string cmd = l.substr(0, sp), arg = sp == std::string::npos ? "" : l.substr(sp + 1);
  if (cmd == "key") {
    size_t a = 0;
    while (a < arg.size()) {
      size_t b = arg.find(' ', a);
      std::string tok = arg.substr(a, b == std::string::npos ? std::string::npos : b - a);
      keys::KeyEvent k = named(tok);
      if (k.any) s_q.push_back(k);
      if (b == std::string::npos) break;
      a = b + 1;
    }
  } else if (cmd == "type") {
    for (char c : arg) s_q.push_back(charKey(c));
  } else if (cmd == "shot") {
    shot(arg);
  } else if (cmd == "dump") {
    reply("## backend %s, %d categories, %d notes", store::backendName(store::active()), (int)store::cats.size(),
          (int)store::notes.size());
    for (size_t c = 0; c < store::cats.size(); c++) reply("## cat %d %s|%s", (int)c, store::cats[c].name.c_str(), store::cats[c].option());
    for (auto& n : store::notes)
      reply("## note %u cat=%d done=%d pin=%d date=%s %s", (unsigned)n.id, n.cat, n.done, n.pinned, n.date, n.text.c_str());
    reply("## settings backend=%d ai=%d hide=%d compact=%d learn_from=%u", settings::v.backend, settings::v.ai,
          settings::v.hideDone, settings::v.compact, (unsigned)settings::v.learnFrom);
    reply("## end");
  } else if (cmd == "heap") {
    reply("## heap free %u largest %u min %u  last pass %u ms %d tokens  %s", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
          (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT), (unsigned)ai::lastMs(), ai::lastTokens(), ai::status());
  }
}

bool poll(keys::KeyEvent& out) {
  if (!s_ok) return false;
  uint8_t b[64];
  int n;
  while ((n = usb_serial_jtag_read_bytes(b, sizeof(b), 0)) > 0) {
    for (int i = 0; i < n; i++) {
      if (b[i] == '\n' || b[i] == '\r') {
        if (!s_line.empty()) command(s_line);
        s_line.clear();
      } else if (s_line.size() < 300) {
        s_line += (char)b[i];
      }
    }
  }
  if (s_q.empty()) return false;
  out = s_q.front();
  s_q.pop_front();
  return true;
}

}  // namespace remote
