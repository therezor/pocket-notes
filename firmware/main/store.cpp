#include "store.h"

#include <algorithm>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <M5Unified.h>
#include <driver/sdspi_host.h>
#include <esp_littlefs.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>

#include "settings.h"

static const char* TAG = "store";

namespace store {

std::vector<Category> cats;
std::vector<Note> notes;

static Backend s_active = BK_NONE;
static bool s_flash = false, s_sd = false;
static sdmmc_card_t* s_card = nullptr;
static bool s_spiInit = false;
static uint32_t s_err = 0;

static const char* FLASH_BASE = "/flash";
static const char* SD_BASE = "/sd";

static std::string dirOf(Backend b) { return std::string(b == BK_SD ? SD_BASE : FLASH_BASE) + "/notes"; }
std::string root() { return dirOf(s_active); }
uint32_t lastError() { return s_err; }
const char* backendName(Backend b) { return b == BK_SD ? "SD card" : b == BK_FLASH ? "Device memory" : "none"; }
Backend active() { return s_active; }
bool mounted(Backend b) { return b == BK_SD ? s_sd : b == BK_FLASH ? s_flash : false; }

// ============================================================================
//  Backends
// ============================================================================

bool mountFlash() {
  if (s_flash) return true;
  esp_vfs_littlefs_conf_t c = {};
  c.base_path = FLASH_BASE;
  c.partition_label = "storage";
  c.format_if_mount_failed = true;   // first boot: the partition is blank
  c.dont_mount = false;
  esp_err_t e = esp_vfs_littlefs_register(&c);
  if (e != ESP_OK) { ESP_LOGE(TAG, "littlefs: %s", esp_err_to_name(e)); return false; }
  s_flash = true;
  return true;
}

bool mountSd() {
  if (s_sd) {
    // Still there? A removed card fails the status query.
    if (s_card && sdmmc_get_status(s_card) == ESP_OK) return true;
    esp_vfs_fat_sdcard_unmount(SD_BASE, s_card);
    s_card = nullptr;
    s_sd = false;
  }
  const int sck = M5.getPin(m5::pin_name_t::sd_spi_sclk), miso = M5.getPin(m5::pin_name_t::sd_spi_miso);
  const int mosi = M5.getPin(m5::pin_name_t::sd_spi_mosi), cs = M5.getPin(m5::pin_name_t::sd_spi_cs);
  if (sck < 0 || miso < 0 || mosi < 0 || cs < 0) return false;
  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = SPI2_HOST;               // M5GFX drives the Cardputer display on SPI3
  host.max_freq_khz = 20000;
  if (!s_spiInit) {
    spi_bus_config_t bus = {};
    bus.mosi_io_num = mosi;
    bus.miso_io_num = miso;
    bus.sclk_io_num = sck;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 4096;
    esp_err_t e = spi_bus_initialize((spi_host_device_t)host.slot, &bus, SDSPI_DEFAULT_DMA);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) { ESP_LOGE(TAG, "spi: %s", esp_err_to_name(e)); return false; }
    s_spiInit = true;
  }
  sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot.gpio_cs = (gpio_num_t)cs;
  slot.host_id = (spi_host_device_t)host.slot;
  esp_vfs_fat_sdmmc_mount_config_t mc = {};
  mc.format_if_mount_failed = false;   // never format someone's card
  mc.max_files = 4;
  mc.allocation_unit_size = 16 * 1024;
  esp_err_t e = esp_vfs_fat_sdspi_mount(SD_BASE, &host, &slot, &mc, &s_card);
  if (e != ESP_OK) { ESP_LOGW(TAG, "no SD card (%s)", esp_err_to_name(e)); s_card = nullptr; return false; }
  s_sd = true;
  return true;
}

bool space(Backend b, uint64_t& used, uint64_t& total) {
  used = total = 0;
  if (!mounted(b)) return false;
  if (b == BK_FLASH) {
    size_t t = 0, u = 0;
    if (esp_littlefs_info("storage", &t, &u) != ESP_OK) return false;
    total = t; used = u;
    return true;
  }
  uint64_t t = 0, f = 0;
  if (esp_vfs_fat_info(SD_BASE, &t, &f) != ESP_OK) return false;
  total = t; used = t - f;
  return true;
}

// ============================================================================
//  Text helpers
// ============================================================================

std::string slug(const std::string& name) {
  std::string s;
  for (char c : name) {
    if (c >= 'A' && c <= 'Z') s += (char)(c - 'A' + 'a');
    else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) s += c;
    else if (!s.empty() && s.back() != '_') s += '_';
  }
  while (!s.empty() && s.back() == '_') s.pop_back();
  return s.empty() ? std::string("list") : s;
}

static std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
  return s.substr(a, b - a);
}

static bool endsWith(const std::string& s, const char* t) {
  size_t n = strlen(t);
  return s.size() >= n && s.compare(s.size() - n, n, t) == 0;
}

static const char* PLUS = "\xE2\x9E\x95";   // ➕ (Obsidian Tasks "created")

// "- [ ] text ➕ 2026-10-05 #pinned ^n42" -> Note. Returns false for blank lines and headings.
static bool parseLine(std::string l, Note& n, bool& hadId) {
  l = trim(l);
  if (l.empty() || l[0] == '#') return false;
  if (l.compare(0, 2, "- ") == 0 || l.compare(0, 2, "* ") == 0) l = l.substr(2);
  if (l.size() >= 4 && l[0] == '[' && l[2] == ']' && l[3] == ' ') {
    n.done = l[1] == 'x' || l[1] == 'X';
    l = l.substr(4);
  }
  hadId = false;
  // Trailing markers, in any order.
  for (bool more = true; more;) {
    more = false;
    l = trim(l);
    size_t sp = l.rfind(' ');
    std::string last = sp == std::string::npos ? l : l.substr(sp + 1);
    if (last.size() > 2 && last[0] == '^' && last[1] == 'n') {
      char* end = nullptr;
      unsigned long v = strtoul(last.c_str() + 2, &end, 10);
      if (end && *end == 0 && v > 0) { n.id = (uint32_t)v; hadId = true; l = sp == std::string::npos ? "" : l.substr(0, sp); more = true; }
    } else if (last == "#pinned") {
      n.pinned = true; l = sp == std::string::npos ? "" : l.substr(0, sp); more = true;
    } else if (last.size() == 10 && last[4] == '-' && last[7] == '-' && sp != std::string::npos) {
      std::string before = trim(l.substr(0, sp));
      if (endsWith(before, PLUS)) {
        memcpy(n.date, last.c_str(), 10);
        n.date[10] = 0;
        l = before.substr(0, before.size() - strlen(PLUS));
        more = true;
      }
    }
  }
  n.text = trim(l);
  return !n.text.empty();
}

static std::string formatLine(const Note& n, bool check) {
  std::string s = check ? (n.done ? "- [x] " : "- [ ] ") : "- ";
  s += n.text;
  if (n.date[0]) { s += ' '; s += PLUS; s += ' '; s += n.date; }
  if (n.pinned) s += " #pinned";
  s += " ^n" + std::to_string(n.id);
  return s;
}

// ============================================================================
//  Files
// ============================================================================

static bool exists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0; }

static bool readLines(const std::string& path, std::vector<std::string>& out) {
  FILE* f = fopen(path.c_str(), "r");
  if (!f && exists(path + ".tmp")) f = fopen((path + ".tmp").c_str(), "r");   // crashed mid-replace
  if (!f) return false;
  char buf[512];
  std::string cur;
  while (fgets(buf, sizeof(buf), f)) {
    cur += buf;
    if (!cur.empty() && cur.back() == '\n') { out.push_back(cur); cur.clear(); }
  }
  if (!cur.empty()) out.push_back(cur);
  fclose(f);
  return true;
}

// Write through path.tmp, then swap it in. FAT cannot rename over a file, so the old one goes first;
// readLines() falls back to the .tmp if power fails between the two steps.
static bool writeFile(const std::string& path, const std::string& data) {
  std::string tmp = path + ".tmp";
  FILE* f = fopen(tmp.c_str(), "w");
  if (!f) { s_err = errno; ESP_LOGE(TAG, "open %s: %d", tmp.c_str(), errno); return false; }
  size_t w = fwrite(data.data(), 1, data.size(), f);
  bool ok = w == data.size() && fflush(f) == 0;
  fsync(fileno(f));
  fclose(f);
  if (!ok) { s_err = errno; remove(tmp.c_str()); return false; }
  remove(path.c_str());
  if (rename(tmp.c_str(), path.c_str()) != 0) { s_err = errno; return false; }
  s_err = 0;
  return true;
}

static const char* DEFAULT_CATS =
    "# Pocket Notes categories, one per line:  Name | what the model sees | flags\n"
    "# flags: check (checklist with [ ] / [x]), phone (phone numbers, emails), time (clock times)\n"
    "Todo | a task to do | check\n"
    "Shopping | something to buy | check\n"
    "Ideas | an idea\n"
    "Events | an appointment | time\n"
    "Contacts | a phone number or email | phone\n"
    "Notes | something to remember\n";

static std::vector<std::string> splitBar(const std::string& l) {
  std::vector<std::string> v;
  size_t a = 0;
  for (;;) {
    size_t b = l.find('|', a);
    v.push_back(trim(l.substr(a, b == std::string::npos ? std::string::npos : b - a)));
    if (b == std::string::npos) break;
    a = b + 1;
  }
  return v;
}

static void parseCategories(const std::vector<std::string>& lines) {
  cats.clear();
  for (const std::string& raw : lines) {
    std::string l = trim(raw);
    if (l.empty() || l[0] == '#') continue;
    std::vector<std::string> f = splitBar(l);
    Category c;
    c.name = f[0];
    if (c.name.empty()) continue;
    if (f.size() > 1) c.hint = f[1];
    if (f.size() > 2) {
      c.check = strstr(f[2].c_str(), "check") != nullptr;
      c.phone = strstr(f[2].c_str(), "phone") != nullptr;
      c.time = strstr(f[2].c_str(), "time") != nullptr;
    }
    c.id = slug(c.name);
    bool dup = false;
    for (auto& o : cats) if (o.id == c.id) dup = true;
    if (!dup && cats.size() < 16) cats.push_back(c);
  }
}

static bool saveCategories() {
  std::string s =
      "# Pocket Notes categories, one per line:  Name | what the model sees | flags\n"
      "# flags: check (checklist with [ ] / [x]), phone (phone numbers, emails), time (clock times)\n";
  for (auto& c : cats) {
    s += c.name + " | " + c.hint;
    std::string fl;
    if (c.check) fl += "check ";
    if (c.phone) fl += "phone ";
    if (c.time) fl += "time ";
    if (!fl.empty()) { fl.pop_back(); s += " | " + fl; }
    s += "\n";
  }
  return writeFile(root() + "/categories.txt", s);
}

bool saveCat(int ci) {
  if (ci < 0 || ci >= (int)cats.size()) return false;
  std::vector<const Note*> v;
  for (auto& n : notes) if (n.cat == ci) v.push_back(&n);
  // Newest first, the order the app shows them in.
  std::sort(v.begin(), v.end(), [](const Note* a, const Note* b) { return a->id > b->id; });
  std::string s;
  for (const Note* n : v) s += formatLine(*n, cats[ci].check) + "\n";
  return writeFile(root() + "/" + cats[ci].id + ".md", s);
}

static bool loadAll() {
  notes.clear();
  cats.clear();
  std::string dir = root();
  mkdir(dir.c_str(), 0775);
  std::vector<std::string> lines;
  if (!readLines(dir + "/categories.txt", lines)) {
    writeFile(dir + "/categories.txt", DEFAULT_CATS);
    lines.clear();
    readLines(dir + "/categories.txt", lines);
    if (lines.empty()) {   // read-only card: still run with the defaults
      const char* p = DEFAULT_CATS;
      while (*p) { const char* e = strchr(p, '\n'); lines.emplace_back(p, e - p); p = e + 1; }
    }
  }
  parseCategories(lines);
  if (cats.empty()) {
    Category c; c.name = "Notes"; c.id = "notes"; c.hint = "something to remember";
    cats.push_back(c);
  }
  uint32_t maxId = 0;
  std::vector<int> needIds;   // lines written on a PC without ^n ids get fresh ones
  for (size_t ci = 0; ci < cats.size(); ci++) {
    std::vector<std::string> nl;
    readLines(dir + "/" + cats[ci].id + ".md", nl);
    for (auto& l : nl) {
      Note n;
      bool hadId = false;
      if (!parseLine(l, n, hadId)) continue;
      n.cat = (uint8_t)ci;
      if (hadId && find(n.id) >= 0) hadId = false;   // duplicate id (copied line): renumber
      if (!hadId) n.id = 0;
      if (n.id > maxId) maxId = n.id;
      notes.push_back(n);
      if (!hadId) needIds.push_back((int)notes.size() - 1);
    }
  }
  settings::bumpNextId(maxId + 1);
  if (!needIds.empty()) {
    std::vector<bool> dirty(cats.size(), false);
    for (int i : needIds) { notes[i].id = settings::takeId(); dirty[notes[i].cat] = true; }
    for (size_t c = 0; c < cats.size(); c++) if (dirty[c]) saveCat((int)c);
  }
  ESP_LOGI(TAG, "%s: %d categories, %d notes", backendName(s_active), (int)cats.size(), (int)notes.size());
  return true;
}

bool use(Backend b) {
  if (!mounted(b)) return false;
  s_active = b;
  settings::importIni((root() + "/settings.ini").c_str());
  return loadAll();
}

bool hasNotes(Backend b) {
  if (!mounted(b)) return false;
  std::string dir = dirOf(b);
  DIR* d = opendir(dir.c_str());
  if (!d) return false;
  bool any = false;
  while (dirent* e = readdir(d)) {
    std::string n = e->d_name;
    if (endsWith(n, ".md")) {
      struct stat st;
      if (stat((dir + "/" + n).c_str(), &st) == 0 && st.st_size > 0) { any = true; break; }
    }
  }
  closedir(d);
  return any;
}

bool copyAll(Backend from, Backend to) {
  if (!mounted(from) || !mounted(to) || from == to) return false;
  std::string src = dirOf(from), dst = dirOf(to);
  mkdir(dst.c_str(), 0775);
  DIR* d = opendir(src.c_str());
  if (!d) return false;
  bool ok = true;
  std::vector<char> buf(2048);
  while (dirent* e = readdir(d)) {
    std::string n = e->d_name;
    if (n == "." || n == ".." || endsWith(n, ".tmp") || e->d_type == DT_DIR) continue;
    FILE* in = fopen((src + "/" + n).c_str(), "rb");
    if (!in) { ok = false; continue; }
    std::string data;
    size_t r;
    while ((r = fread(buf.data(), 1, buf.size(), in)) > 0) data.append(buf.data(), r);
    fclose(in);
    if (!writeFile(dst + "/" + n, data)) ok = false;
  }
  closedir(d);
  return ok;
}

// ============================================================================
//  Notes + categories
// ============================================================================

int find(uint32_t id) {
  for (size_t i = 0; i < notes.size(); i++) if (notes[i].id == id) return (int)i;
  return -1;
}

int addNote(const std::string& text, int cat, const char* date) {
  Note n;
  n.id = settings::takeId();
  n.cat = (uint8_t)cat;
  n.text = trim(text);
  if (date && date[0]) { strncpy(n.date, date, 10); n.date[10] = 0; }
  notes.push_back(n);
  saveCat(cat);
  return (int)notes.size() - 1;
}

void removeNote(int idx) {
  if (idx < 0 || idx >= (int)notes.size()) return;
  int c = notes[idx].cat;
  notes.erase(notes.begin() + idx);
  saveCat(c);
}

void moveNote(int idx, int cat) {
  if (idx < 0 || idx >= (int)notes.size() || notes[idx].cat == cat) return;
  int old = notes[idx].cat;
  notes[idx].cat = (uint8_t)cat;
  if (!cats[cat].check) notes[idx].done = false;
  saveCat(cat);
  saveCat(old);
}

int countIn(int cat, int* open) {
  int n = 0, o = 0;
  for (auto& x : notes) if (x.cat == cat) { n++; if (!x.done) o++; }
  if (open) *open = o;
  return n;
}

int addCategory(const std::string& nameIn) {
  std::string name = trim(nameIn);
  if (name.empty() || cats.size() >= 16) return -1;
  std::string id = slug(name);
  for (auto& c : cats) if (c.id == id) return -1;
  Category c;
  c.name = name;
  c.id = id;
  // The model reads the option text, so a lowercased name is a fair default hint.
  for (char ch : name) c.hint += (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
  cats.push_back(c);
  saveCategories();
  saveCat((int)cats.size() - 1);
  return (int)cats.size() - 1;
}

bool renameCategory(int ci, const std::string& nameIn) {
  std::string name = trim(nameIn);
  if (ci < 0 || ci >= (int)cats.size() || name.empty()) return false;
  std::string id = slug(name);
  for (size_t i = 0; i < cats.size(); i++) if ((int)i != ci && cats[i].id == id) return false;
  std::string oldPath = root() + "/" + cats[ci].id + ".md";
  // A hint that was just the lowercased old name follows the new name.
  std::string oldLower;
  for (char ch : cats[ci].name) oldLower += (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
  if (cats[ci].hint == oldLower) {
    cats[ci].hint.clear();
    for (char ch : name) cats[ci].hint += (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
  }
  cats[ci].name = name;
  cats[ci].id = id;
  saveCat(ci);
  if (oldPath != root() + "/" + id + ".md") remove(oldPath.c_str());
  return saveCategories();
}

bool deleteCategory(int ci, int moveTo) {
  if (ci < 0 || ci >= (int)cats.size() || cats.size() <= 1 || moveTo == ci) return false;
  for (auto& n : notes) if (n.cat == ci) { n.cat = (uint8_t)moveTo; if (!cats[moveTo].check) n.done = false; }
  remove((root() + "/" + cats[ci].id + ".md").c_str());
  cats.erase(cats.begin() + ci);
  for (auto& n : notes) if (n.cat > ci) n.cat--;
  if (moveTo > ci) moveTo--;
  saveCat(moveTo);
  return saveCategories();
}

bool setCategoryHint(int ci, const std::string& hint) {
  if (ci < 0 || ci >= (int)cats.size()) return false;
  std::string h = trim(hint);
  for (auto& c : h) if (c == '|') c = '/';   // '|' separates the columns of categories.txt
  cats[ci].hint = h;
  return saveCategories();
}

bool setCategoryFlags(int ci, bool check, bool phone, bool time) {
  if (ci < 0 || ci >= (int)cats.size()) return false;
  const bool wasCheck = cats[ci].check;
  cats[ci].check = check;
  cats[ci].phone = phone;
  cats[ci].time = time;
  if (wasCheck != check) saveCat(ci);   // the .md lines gain or lose their [ ] boxes
  return saveCategories();
}

bool moveCategory(int ci, int dir) {
  const int to = ci + dir;
  if (ci < 0 || ci >= (int)cats.size() || to < 0 || to >= (int)cats.size()) return false;
  std::swap(cats[ci], cats[to]);
  for (auto& n : notes) {
    if (n.cat == ci) n.cat = (uint8_t)to;
    else if (n.cat == to) n.cat = (uint8_t)ci;
  }
  return saveCategories();
}

uint32_t catSignature() {
  uint32_t h = 2166136261u;
  for (auto& c : cats) {
    for (char ch : std::string(c.option())) h = (h ^ (uint8_t)ch) * 16777619u;
    h = (h ^ '|') * 16777619u;
  }
  return h;
}

}  // namespace store
