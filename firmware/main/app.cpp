// Pocket Notes screens. One screen is active at a time; each has a draw and a key handler, and
// the main loop redraws the active screen into the canvas every frame (as in esp32_cleaner).
#include "app.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

#include "ai.h"
#include "clock.h"
#include "keys.h"
#include "port.h"
#include "remote.h"
#include "settings.h"
#include "store.h"
#include "tinydecide.h"
#include "ui.h"

#define APP_NAME "Pocket Notes"
#define APP_VERSION "1.0"

static const char* TAG = "app";

using keys::KeyEvent;
using store::cats;
using store::notes;

namespace app {

enum Screen : uint8_t { HOME, LIST, NOTE, EDIT, PICK, SEARCH, SETTINGS, CONFIRM, BOOTSD, INFO, CATS, CATEDIT };

static Screen s_screen = HOME;
static M5Canvas* s_cv = nullptr;
static char s_toast[64] = "";
static uint32_t s_toastUntil = 0;

static void toast(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void toast(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(s_toast, sizeof(s_toast), fmt, ap);
  va_end(ap);
  s_toastUntil = millis() + 2000;
}

// Notes may come from a PC with UTF-8 in them; the bitmap fonts are ASCII, so each multi-byte
// sequence shows as one '?'. The stored text is never changed.
static std::string view(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) { o += (c < 0x20) ? ' ' : (char)c; continue; }
    if ((c & 0xC0) == 0x80) continue;
    o += '?';
  }
  return o;
}

static std::string lower(const std::string& s) {
  std::string o = s;
  for (auto& c : o) c = (char)tolower((unsigned char)c);
  return o;
}

// Footer key hints. The footer holds ~29 characters in the normal font, so screens with more keys
// pass several short pages that take turns every 2.5 s; `status` (e.g. "learning 3/40") is one
// more page when set. A toast replaces the footer for 2 s.
static void footer(std::initializer_list<const char*> pages, const char* status = nullptr) {
  if (s_toastUntil && millis() < s_toastUntil) {
    ui::footer(s_toast);
    return;
  }
  s_toastUntil = 0;
  std::vector<const char*> p(pages);
  if (status && status[0]) p.push_back(status);
  ui::footer(p[(millis() / 2500) % p.size()]);
}

static void headerRight(const char* title) {
  char hm[6];
  if (clk::hhmm(hm)) { ui::header(title, hm, ui::C_DIM); return; }
  const bool sd = store::active() == store::BK_SD;
  ui::header(title, sd ? "On SD" : "On device", sd ? ui::C_OK : ui::C_DIM);
}

static const char* spinner() {
  static const char* f[] = {"|", "/", "-", "\\"};
  return f[(millis() / 120) % 4];
}

// ============================================================================
//  Shared state
// ============================================================================

// Home
static int s_homeIdx = 0, s_homeScroll = 0;
// Category list
static int s_cat = 0, s_listIdx = 0, s_listScroll = 0;
static std::vector<int> s_list;      // indices into notes, display order
// Note view
static uint32_t s_noteId = 0;
static Screen s_noteBack = LIST;
// Editor
enum EditFor : uint8_t { E_NEW_HOME, E_NEW_IN_CAT, E_EDIT_NOTE, E_NEW_CAT, E_RENAME_CAT, E_CAT_HINT, E_WIFI_SSID, E_WIFI_PASS };
static EditFor s_editFor = E_NEW_HOME;
static std::string s_edit;
static int s_cursor = 0, s_editTop = 0;
static const char* s_editTitle = "";
static int s_editMax = 200;
static Screen s_editBack = HOME;
static std::string s_editErr;
// Picker
enum PickFor : uint8_t { P_FILE_NEW, P_MOVE };
static PickFor s_pickFor = P_FILE_NEW;
static int s_pickIdx = 0, s_pickScroll = 0;
static bool s_pickMoved = false;      // the user moved the cursor; a late suggestion leaves it alone
static std::string s_pending;         // text of the note being filed
static int s_lastCat = 0;
static Screen s_pickBack = LIST;     // where a move returns to
// Search
static std::string s_query;
static int s_searchCat = -1;          // -1: all categories
static std::vector<int> s_results;
static int s_searchIdx = 0, s_searchScroll = 0;
static Screen s_searchBack = HOME;
// Settings
static int s_setIdx = 0, s_setScroll = 0;
// Confirm
static std::string s_confirmTitle, s_confirmText;
static std::function<void()> s_onYes;
static Screen s_confirmBack = HOME;
// Info
static std::string s_infoTitle, s_infoText;
static Screen s_infoBack = SETTINGS;
// Boot SD prompt
static int s_bootIdx = 0;
// Category manager (Settings > Categories)
static int s_catsIdx = 0, s_catsScroll = 0;
static int s_ceCat = 0, s_ceIdx = 0, s_ceScroll = 0;

static void go(Screen s) { s_screen = s; }
static void refreshShown();

static void confirm(const std::string& title, const std::string& text, std::function<void()> yes) {
  s_confirmTitle = title;
  s_confirmText = text;
  s_onYes = yes;
  s_confirmBack = s_screen;
  go(CONFIRM);
}

static void info(const std::string& title, const std::string& text) {
  s_infoTitle = title;
  s_infoText = text;
  s_infoBack = s_screen;
  go(INFO);
}

static void openEditor(EditFor f, const char* title, const std::string& init, int maxLen) {
  s_editFor = f;
  s_editTitle = title;
  s_edit = init;
  s_cursor = (int)s_edit.size();
  s_editTop = 0;
  s_editMax = maxLen;
  s_editErr.clear();
  s_editBack = s_screen;
  go(EDIT);
}

// ============================================================================
//  Home
// ============================================================================

static int homeCount() { return 2 + (int)cats.size() + 1; }   // new, search, categories, settings

static void drawHome() {
  headerRight(APP_NAME);
  std::vector<std::string> right(cats.size());
  std::vector<ui::MenuItem> items;
  items.push_back({"+ New note", nullptr, -1, ui::C_HDR_FG});
  items.push_back({"Search", nullptr, -1, 0});
  for (size_t i = 0; i < cats.size(); i++) {
    int open = 0, n = store::countIn((int)i, &open);
    char b[16];
    if (cats[i].check) snprintf(b, sizeof(b), "%d/%d", n - open, n);   // completed / total
    else snprintf(b, sizeof(b), "%d", n);
    right[i] = b;
    ui::MenuItem it{cats[i].name.c_str()};
    it.right = right[i].c_str();
    items.push_back(it);
  }
  items.push_back({"Settings", nullptr, -1, ui::C_DIM});
  ui::menu(ui::M.bodyY, items.data(), (int)items.size(), s_homeIdx, s_homeScroll);
  const bool onCat = s_homeIdx >= 2 && s_homeIdx < 2 + (int)cats.size();
  if (onCat) footer({"enter open  r rename", "d delete  n new  f find"}, ai::status());
  else footer({"\x18\x19 move  enter open", "n new note  f search"}, ai::status());
}

static void refreshList();
static void runSearch();

static void openCategory(int c) {
  s_cat = c;
  s_listIdx = 0;
  s_listScroll = 0;
  refreshList();
  go(LIST);
}

static void openSearch(int cat) {
  s_searchCat = cat;
  s_query.clear();
  s_searchIdx = 0;
  s_searchScroll = 0;
  s_searchBack = s_screen;
  s_results.clear();
  go(SEARCH);
}

static void keyHome(const KeyEvent& k) {
  const int n = homeCount();
  if (k.up) s_homeIdx = (s_homeIdx + n - 1) % n;
  else if (k.down) s_homeIdx = (s_homeIdx + 1) % n;
  else if (k.left) s_homeIdx = std::max(0, s_homeIdx - 4);
  else if (k.right) s_homeIdx = std::min(n - 1, s_homeIdx + 4);
  else if (k.enter) {
    if (s_homeIdx == 0) openEditor(E_NEW_HOME, "New note", "", 200);
    else if (s_homeIdx == 1) openSearch(-1);
    else if (s_homeIdx == n - 1) { s_setIdx = 0; s_setScroll = 0; go(SETTINGS); }
    else openCategory(s_homeIdx - 2);
  } else if (k.ch == 'f' || k.ch == 'F') openSearch(-1);
  else if (k.ch == 'n' || k.ch == 'N') openEditor(E_NEW_HOME, "New note", "", 200);
  else if (s_homeIdx >= 2 && s_homeIdx < 2 + (int)cats.size()) {
    const int c = s_homeIdx - 2;
    if (k.ch == 'r' || k.ch == 'R') { s_cat = c; openEditor(E_RENAME_CAT, "Rename category", cats[c].name, 24); }
    else if ((k.ch == 'd' || k.ch == 'D') && cats.size() > 1) {
      // Notes are never deleted with their category: they move to the last remaining category
      // that is not this one (Notes, with the defaults).
      int to = (int)cats.size() - 1;
      if (to == c) to = c - 1;
      const int cnt = store::countIn(c);
      std::string msg = "Delete \"" + cats[c].name + "\"?";
      if (cnt) msg += " Its " + std::to_string(cnt) + " notes move to " + cats[to].name + ".";
      confirm("Delete category", msg, [c, to]() {
        store::deleteCategory(c, to);
        ai::categoriesChanged();
        s_homeIdx = std::min(s_homeIdx, homeCount() - 1);
        toast("Category deleted");
      });
    }
  }
}

// ============================================================================
//  Category list
// ============================================================================

// Pinned first; then (checklists) open before done; newest first within each group.
static bool listOrder(int a, int b) {
  const store::Note &x = notes[a], &y = notes[b];
  if (x.pinned != y.pinned) return x.pinned;
  if (x.done != y.done) return !x.done;
  return x.id > y.id;
}

static void refreshList() {
  s_list.clear();
  if (s_cat >= (int)cats.size()) s_cat = 0;
  const bool hide = cats[s_cat].check && settings::v.hideDone;
  for (size_t i = 0; i < notes.size(); i++)
    if (notes[i].cat == s_cat && !(hide && notes[i].done)) s_list.push_back((int)i);
  std::sort(s_list.begin(), s_list.end(), listOrder);
  const int n = (int)s_list.size() + 1;
  if (s_listIdx >= n) s_listIdx = n - 1;
}

static int selectedNote() { return (s_listIdx >= 1 && s_listIdx - 1 < (int)s_list.size()) ? s_list[s_listIdx - 1] : -1; }

static void drawList() {
  const store::Category& c = cats[s_cat];
  int open = 0, n = store::countIn(s_cat, &open);
  char right[24];
  if (c.check) snprintf(right, sizeof(right), "%d/%d", n - open, n);   // completed / total
  else snprintf(right, sizeof(right), "%d", n);
  ui::header(c.name.c_str(), right, ui::C_DIM);
  std::vector<std::string> labels(s_list.size());
  std::vector<ui::MenuItem> items;
  items.push_back({"+ New note", nullptr, -1, ui::C_HDR_FG});
  for (size_t i = 0; i < s_list.size(); i++) {
    const store::Note& nt = notes[s_list[i]];
    labels[i] = view(nt.text);
    ui::MenuItem it{labels[i].c_str()};
    if (c.check) it.check = nt.done ? 1 : 0;
    if (nt.done) it.accent = ui::C_MUTE;
    if (nt.pinned) { it.right = "*"; it.rightCol = ui::C_WARN; }
    items.push_back(it);
  }
  ui::menu(ui::M.bodyY, items.data(), (int)items.size(), s_listIdx, s_listScroll);
  if (s_list.empty() && c.check && settings::v.hideDone && n > 0)
    ui::text(ui::M.pad * 2, ui::M.bodyY + ui::M.rowH * 2, "completed items hidden (h)", ui::C_MUTE);
  if (selectedNote() < 0) footer({"enter add  f find  esc back"}, ai::status());
  else if (c.check) footer({"space done  enter open", "e edit  m move  p pin", "d delete  h hide done"});
  else footer({"enter open  e edit  m move", "p pin  d delete  f find"});
}

static void openNote(int idx, Screen back) {
  s_noteId = notes[idx].id;
  s_noteBack = back;
  go(NOTE);
}

static void openMove(uint32_t id) {
  s_noteId = id;
  s_pickFor = P_MOVE;
  int i = store::find(id);
  s_pickIdx = i >= 0 ? notes[i].cat : 0;
  s_pickScroll = 0;
  s_pickMoved = true;
  s_pickBack = s_screen;
  go(PICK);
}

// Space / p / d / m on a note, shared by the list and the note view.
static bool noteAction(const KeyEvent& k, int idx) {
  if (idx < 0) return false;
  store::Note& n = notes[idx];
  if (k.space && cats[n.cat].check) {
    n.done = !n.done;
    store::saveCat(n.cat);
    return true;
  }
  if (k.ch == 'p' || k.ch == 'P') {
    n.pinned = !n.pinned;
    store::saveCat(n.cat);
    toast(n.pinned ? "Pinned" : "Unpinned");
    return true;
  }
  if (k.ch == 'm' || k.ch == 'M') { openMove(n.id); return true; }
  if (k.ch == 'e' || k.ch == 'E') {
    s_noteId = n.id;
    openEditor(E_EDIT_NOTE, "Edit note", n.text, 200);
    return true;
  }
  if (k.ch == 'd' || k.ch == 'D' || (k.back && k.fn)) {
    const uint32_t id = n.id;
    std::string t = view(n.text);
    if (t.size() > 60) t = t.substr(0, 57) + "...";
    confirm("Delete note", "\"" + t + "\"", [id]() {
      int i = store::find(id);
      if (i >= 0) store::removeNote(i);
      if (s_confirmBack == NOTE) s_confirmBack = s_noteBack;
      toast("Deleted");
    });
    return true;
  }
  return false;
}

static void keyList(const KeyEvent& k) {
  const int n = (int)s_list.size() + 1;
  if (k.up) s_listIdx = (s_listIdx + n - 1) % n;
  else if (k.down) s_listIdx = (s_listIdx + 1) % n;
  else if (k.left) s_listIdx = std::max(0, s_listIdx - 4);
  else if (k.right) s_listIdx = std::min(n - 1, s_listIdx + 4);
  else if (k.esc || (k.back && !k.fn)) go(HOME);
  else if (k.enter) {
    if (s_listIdx == 0) openEditor(E_NEW_IN_CAT, "New note", "", 200);
    else openNote(selectedNote(), LIST);
  } else if (k.ch == 'f' || k.ch == 'F') openSearch(s_cat);
  else if (k.ch == 'n' || k.ch == 'N') openEditor(E_NEW_IN_CAT, "New note", "", 200);
  else if (k.ch == 'h' || k.ch == 'H') {
    if (cats[s_cat].check) {
      settings::v.hideDone = !settings::v.hideDone;
      settings::save();
      toast(settings::v.hideDone ? "Completed hidden" : "Completed shown");
    }
  } else noteAction(k, selectedNote());
}

// ============================================================================
//  Note view
// ============================================================================

static void drawNote() {
  int i = store::find(s_noteId);
  if (i < 0) { go(s_noteBack); return; }
  const store::Note& n = notes[i];
  char right[16];
  if (n.date[0]) snprintf(right, sizeof(right), "%.5s", n.date + 5);   // MM-DD
  else snprintf(right, sizeof(right), "#%u", (unsigned)n.id);
  ui::header(cats[n.cat].name.c_str(), right, ui::C_DIM);
  int y = ui::M.bodyY;
  if (cats[n.cat].check || n.pinned || n.date[0]) {
    std::string st;
    if (cats[n.cat].check) st += n.done ? "[x] done" : "[ ] open";
    if (n.pinned) st += st.empty() ? "pinned" : "  pinned";
    if (n.date[0]) { st += st.empty() ? "" : "  "; st += n.date; }
    ui::text(ui::M.pad * 2, y, st.c_str(), n.done ? ui::C_OK : ui::C_DIM);
    y += ui::M.lineH + ui::M.pad;
  }
  ui::wrap(ui::M.pad * 2, y, ui::charsFor(ui::M.w - ui::M.pad * 4), view(n.text).c_str(), n.done ? ui::C_DIM : ui::C_FG);
  if (cats[n.cat].check) footer({"space done  e edit  m move", "p pin  d delete  esc back"});
  else footer({"e edit  m move  p pin", "d delete  esc back"});
}

static void keyNote(const KeyEvent& k) {
  int i = store::find(s_noteId);
  if (k.esc || (k.back && !k.fn) || i < 0) {
    go(s_noteBack);
    refreshShown();
    return;
  }
  noteAction(k, i);
}

// ============================================================================
//  Editor
// ============================================================================

static void drawEdit() {
  char cnt[16];
  snprintf(cnt, sizeof(cnt), "%d/%d", (int)s_edit.size(), s_editMax);
  ui::header(s_editTitle, cnt, ui::C_DIM);
  const int x0 = ui::M.pad * 2, w = ui::M.w - ui::M.pad * 4;
  const int cols = ui::charsFor(w - ui::M.pad * 2);
  const int bottom = ui::M.h - ui::M.footerH - ui::M.pad;
  int boxRows = (bottom - ui::M.bodyY - ui::M.pad * 2) / ui::M.lineH;
  if (!s_editErr.empty()) boxRows--;
  if (boxRows < 1) boxRows = 1;
  // Hard wrap at the box width: simple, and the cursor maps straight onto a cell.
  const bool secret = s_editFor == E_WIFI_PASS;
  std::string shown = secret ? std::string(s_edit.size(), '*') : view(s_edit);
  if (shown.size() != s_edit.size()) shown = std::string(s_edit.size(), '?');   // never for typed text
  const int curRow = s_cursor / cols;
  if (curRow < s_editTop) s_editTop = curRow;
  if (curRow >= s_editTop + boxRows) s_editTop = curRow - boxRows + 1;
  const int boxH = boxRows * ui::M.lineH + ui::M.pad * 2;
  ui::cv->drawRect(x0, ui::M.bodyY, w, boxH, ui::C_DIM);
  for (int r = 0; r < boxRows; r++) {
    const int start = (s_editTop + r) * cols;
    if (start > (int)shown.size()) break;
    std::string line = shown.substr(start, cols);
    ui::text(x0 + ui::M.pad, ui::M.bodyY + ui::M.pad + r * ui::M.lineH, line.c_str(), ui::C_FG);
  }
  const int cx = x0 + ui::M.pad + (s_cursor % cols) * ui::M.charW;
  const int cy = ui::M.bodyY + ui::M.pad + (curRow - s_editTop) * ui::M.lineH;
  if ((millis() / 500) % 2 == 0) ui::cv->fillRect(cx, cy, 2 * ui::M.scale, ui::M.lineH, ui::C_HDR_FG);
  if (!s_editErr.empty()) ui::text(x0, ui::M.bodyY + boxH + ui::M.pad, s_editErr.c_str(), ui::C_ERR);
  footer({"enter save  esc cancel", "fn+, fn+/ move cursor"});
}

static void finishNewNote(const std::string& text, int cat) {
  char date[11];
  clk::today(date);
  store::addNote(text, cat, date);
  s_lastCat = cat;
  toast("Filed in %s", cats[cat].name.c_str());
}

static void submitEdit() {
  std::string t = s_edit;
  while (!t.empty() && t.back() == ' ') t.pop_back();
  while (!t.empty() && t.front() == ' ') t.erase(t.begin());
  switch (s_editFor) {
    case E_NEW_HOME:
      if (t.empty()) { go(HOME); return; }
      s_pending = t;
      s_pickFor = P_FILE_NEW;
      s_pickIdx = s_lastCat < (int)cats.size() ? s_lastCat : 0;
      s_pickScroll = 0;
      s_pickMoved = false;
      if (settings::v.ai && ai::ok()) ai::suggest(t);
      go(PICK);
      return;
    case E_NEW_IN_CAT:
      if (!t.empty()) finishNewNote(t, s_cat);
      refreshList();
      go(LIST);
      return;
    case E_EDIT_NOTE: {
      int i = store::find(s_noteId);
      if (i >= 0 && !t.empty() && t != notes[i].text) {
        notes[i].text = t;
        store::saveCat(notes[i].cat);
        toast("Saved");
      }
      go(s_editBack);
      refreshShown();
      return;
    }
    case E_NEW_CAT: {
      int c = store::addCategory(t);
      if (c < 0) { s_editErr = t.empty() ? "Type a name" : "Name taken or 16 categories max"; return; }
      ai::categoriesChanged();
      if (s_editBack == CATS) {          // Settings > Categories: straight into its editor
        s_ceCat = c; s_ceIdx = 0; s_ceScroll = 0; s_catsIdx = c;
        go(CATEDIT);
      } else if (s_editBack == PICK && s_pickFor == P_FILE_NEW && !s_pending.empty()) {
        finishNewNote(s_pending, c);     // from the picker: file the pending note there right away
        s_pending.clear();
        go(HOME);
      } else if (s_editBack == PICK && s_pickFor == P_MOVE) {
        int i = store::find(s_noteId);
        if (i >= 0) store::moveNote(i, c);
        go(s_pickBack);
        refreshShown();
      } else {
        go(HOME);
      }
      return;
    }
    case E_RENAME_CAT:
      if (!t.empty() && t != cats[s_cat].name) {
        if (!store::renameCategory(s_cat, t)) { s_editErr = "Name taken"; return; }
        ai::categoriesChanged();
      }
      go(s_editBack == CATEDIT ? CATEDIT : HOME);
      return;
    case E_CAT_HINT:
      store::setCategoryHint(s_ceCat, t);
      ai::categoriesChanged();
      go(CATEDIT);
      return;
    case E_WIFI_SSID:
      settings::v.wifiSsid = t;
      if (t.empty()) { settings::v.wifiPass.clear(); settings::save(); go(SETTINGS); return; }
      settings::save();
      s_editBack = SETTINGS;
      s_editFor = E_WIFI_PASS;
      s_editTitle = "WiFi password";
      s_edit = settings::v.wifiPass;
      s_cursor = (int)s_edit.size();
      s_editTop = 0;
      s_editMax = 63;
      return;
    case E_WIFI_PASS:
      settings::v.wifiPass = s_edit;   // passwords keep their spaces
      settings::save();
      clk::syncNow();
      go(SETTINGS);
      toast("Syncing time...");
      return;
  }
}

static void keyEdit(const KeyEvent& k) {
  if (k.esc) {
    if (s_editFor == E_WIFI_PASS) { go(SETTINGS); return; }
    go(s_editBack);
    refreshShown();
    return;
  }
  if (k.enter) { submitEdit(); return; }
  if (k.fn && (k.left || k.right || k.up || k.down)) {
    if (k.left && s_cursor > 0) s_cursor--;
    if (k.right && s_cursor < (int)s_edit.size()) s_cursor++;
    if (k.up) s_cursor = 0;
    if (k.down) s_cursor = (int)s_edit.size();
    return;
  }
  if (k.back) {
    if (s_cursor > 0) { s_edit.erase(s_cursor - 1, 1); s_cursor--; }
    else if (s_edit.empty() && !k.repeat) { go(s_editBack); refreshShown(); }
    s_editErr.clear();
    return;
  }
  if (k.ch && (int)s_edit.size() < s_editMax) {
    s_edit.insert(s_edit.begin() + s_cursor, k.ch);
    s_cursor++;
    s_editErr.clear();
  }
}

// ============================================================================
//  File-as / move picker
// ============================================================================

static void drawPick() {
  int pick = -1;
  float conf = 0;
  const float* probs = nullptr;
  const bool sug = s_pickFor == P_FILE_NEW && ai::suggestion(pick, conf, &probs);
  if (sug && !s_pickMoved && pick < (int)cats.size()) { s_pickIdx = pick; s_pickMoved = true; }
  const bool thinking = s_pickFor == P_FILE_NEW && ai::pending();
  if (thinking) {
    char r[16];
    snprintf(r, sizeof(r), "%s thinking", spinner());
    ui::header(s_pickFor == P_MOVE ? "Move to" : "File as", r, ui::C_WARN);
  } else if (sug) {
    char r[16];
    snprintf(r, sizeof(r), "AI %d%%", (int)(probs[pick] * 100 + 0.5f));
    ui::header("File as", r, ui::C_OK);
  } else {
    ui::header(s_pickFor == P_MOVE ? "Move to" : "File as");
  }
  std::vector<std::string> right(cats.size());
  std::vector<ui::MenuItem> items;
  int cur = -1;
  if (s_pickFor == P_MOVE) { int i = store::find(s_noteId); if (i >= 0) cur = notes[i].cat; }
  for (size_t i = 0; i < cats.size(); i++) {
    ui::MenuItem it{cats[i].name.c_str()};
    if (sug) {
      char b[8];
      snprintf(b, sizeof(b), "%d%%", (int)(probs[i] * 100 + 0.5f));
      right[i] = b;
      it.right = right[i].c_str();
      if ((int)i == pick) { it.accent = ui::C_OK; it.rightCol = ui::C_OK; }
    } else if ((int)i == cur) {
      it.right = "now";
    }
    items.push_back(it);
  }
  items.push_back({"+ New category", nullptr, -1, ui::C_HDR_FG});
  ui::menu(ui::M.bodyY, items.data(), (int)items.size(), s_pickIdx, s_pickScroll);
  footer({"\x18\x19 choose  enter file", "esc back to the text"});
}

static void keyPick(const KeyEvent& k) {
  const int n = (int)cats.size() + 1;
  if (k.up) { s_pickIdx = (s_pickIdx + n - 1) % n; s_pickMoved = true; }
  else if (k.down) { s_pickIdx = (s_pickIdx + 1) % n; s_pickMoved = true; }
  else if (k.esc || (k.back && !k.repeat)) {
    ai::cancel();
    if (s_pickFor == P_FILE_NEW) {   // back to the text, nothing lost
      s_editFor = E_NEW_HOME;
      s_editTitle = "New note";
      s_edit = s_pending;
      s_cursor = (int)s_edit.size();
      s_editErr.clear();
      s_editBack = HOME;
      go(EDIT);
    } else {
      go(s_pickBack);
      refreshShown();
    }
  } else if (k.enter) {
    if (s_pickIdx == n - 1) {
      openEditor(E_NEW_CAT, "New category", "", 24);
      s_editBack = PICK;
      return;
    }
    ai::cancel();
    if (s_pickFor == P_FILE_NEW) {
      finishNewNote(s_pending, s_pickIdx);
      s_pending.clear();
      go(HOME);
    } else {
      int i = store::find(s_noteId);
      if (i >= 0 && notes[i].cat != s_pickIdx) {
        store::moveNote(i, s_pickIdx);
        toast("Moved to %s", cats[s_pickIdx].name.c_str());
      }
      go(s_pickBack);
      refreshShown();
    }
  }
}

// ============================================================================
//  Search
// ============================================================================

static std::vector<std::string> terms(const std::string& q) {
  std::vector<std::string> t;
  std::string cur;
  for (char c : lower(q)) {
    if (c == ' ') { if (!cur.empty()) t.push_back(cur); cur.clear(); }
    else cur += c;
  }
  if (!cur.empty()) t.push_back(cur);
  return t;
}

static void runSearch() {
  s_results.clear();
  std::vector<std::string> t = terms(s_query);
  if (t.empty()) return;
  for (size_t i = 0; i < notes.size(); i++) {
    if (s_searchCat >= 0 && notes[i].cat != s_searchCat) continue;
    const std::string hay = lower(notes[i].text) + " " + lower(cats[notes[i].cat].name);
    bool all = true;
    for (auto& w : t) if (hay.find(w) == std::string::npos) { all = false; break; }
    if (all) s_results.push_back((int)i);
  }
  std::sort(s_results.begin(), s_results.end(), [](int a, int b) {
    if (notes[a].done != notes[b].done) return !notes[a].done;
    return notes[a].id > notes[b].id;
  });
  if (s_searchIdx >= (int)s_results.size()) s_searchIdx = std::max(0, (int)s_results.size() - 1);
}

static void drawSearch() {
  char right[16];
  snprintf(right, sizeof(right), "%d", (int)s_results.size());
  ui::header(s_searchCat >= 0 ? ("Find in " + cats[s_searchCat].name).c_str() : "Search", s_query.empty() ? nullptr : right, ui::C_DIM);
  const int x0 = ui::M.pad * 2, w = ui::M.w - ui::M.pad * 4;
  const int y0 = ui::M.bodyY;
  const int boxH = ui::M.lineH + ui::M.pad * 2;
  ui::cv->drawRect(x0, y0, w, boxH, ui::C_DIM);
  const int cols = ui::charsFor(w - ui::M.pad * 2) - 1;
  std::string q = view(s_query);
  if ((int)q.size() > cols) q = q.substr(q.size() - cols);
  ui::text(x0 + ui::M.pad, y0 + ui::M.pad, q.c_str(), ui::C_FG);
  if ((millis() / 500) % 2 == 0)
    ui::cv->fillRect(x0 + ui::M.pad + (int)q.size() * ui::M.charW, y0 + ui::M.pad, 2 * ui::M.scale, ui::M.lineH, ui::C_HDR_FG);

  const int ly = y0 + boxH + ui::M.pad;
  const int rh = ui::M.rowH;
  const int rows = ui::rowsFor(ly, rh);
  const int n = (int)s_results.size();
  if (s_searchIdx < s_searchScroll) s_searchScroll = s_searchIdx;
  if (s_searchIdx >= s_searchScroll + rows) s_searchScroll = s_searchIdx - rows + 1;
  if (s_searchScroll > n - rows) s_searchScroll = std::max(0, n - rows);
  std::vector<std::string> t = terms(s_query);
  const bool bar = n > rows;
  const int textR = bar ? ui::M.w - 2 * ui::M.scale - ui::M.pad * 2 : ui::M.w - ui::M.pad;
  for (int r = 0; r < rows && s_searchScroll + r < n; r++) {
    const int i = s_searchScroll + r;
    const store::Note& nt = notes[s_results[i]];
    const int y = ly + r * rh;
    const bool sel = i == s_searchIdx;
    const uint16_t bg = sel ? ui::C_SEL_BG : ui::C_BG;
    if (sel) ui::cv->fillRect(0, y, ui::M.w, rh - ui::M.scale, ui::C_SEL_BG);
    int x = ui::M.pad * 2;
    // Category tag, 4 characters.
    std::string tag = s_searchCat >= 0 ? "" : view(cats[nt.cat].name).substr(0, 4);
    if (!tag.empty()) {
      ui::text(x, y + ui::M.scale, tag.c_str(), sel ? ui::C_DIM : ui::C_MUTE, bg);
      x += 5 * ui::M.charW;
    }
    if (cats[nt.cat].check) {
      ui::text(x, y + ui::M.scale, nt.done ? "x" : "-", nt.done ? ui::C_OK : ui::C_MUTE, bg);
      x += 2 * ui::M.charW;
    }
    const int room = ui::charsFor(textR - x);
    std::string s = view(nt.text);
    // Find the first match and slide the window so it shows.
    std::string ls = lower(s);
    size_t mpos = std::string::npos, mlen = 0;
    for (auto& w2 : t) { size_t p = ls.find(w2); if (p != std::string::npos && p < mpos) { mpos = p; mlen = w2.size(); } }
    size_t start = 0;
    if (mpos != std::string::npos && (int)(mpos + mlen) > room) start = mpos + mlen + 3 - room;
    std::string vis = s.substr(std::min(start, s.size()));
    if (start > 0 && vis.size() >= 3) vis.replace(0, 3, "...");
    if ((int)vis.size() > room) vis = vis.substr(0, room);
    const uint16_t fg = sel ? ui::C_SEL_FG : (nt.done ? ui::C_DIM : ui::C_FG);
    ui::text(x, y + ui::M.scale, vis.c_str(), fg, bg);
    if (mpos != std::string::npos && mpos >= start) {
      const size_t off = mpos - start;
      if ((int)off < room) {
        std::string m = vis.substr(off, std::min(mlen, vis.size() - off));
        ui::text(x + (int)off * ui::M.charW, y + ui::M.scale, m.c_str(), ui::C_HDR_FG, bg);
      }
    }
  }
  if (bar) ui::scrollbar(ui::M.w - 2 * ui::M.scale - ui::M.pad, ly, rows * rh, n, rows, s_searchScroll);
  if (!s_query.empty() && n == 0) ui::text(x0, ly + ui::M.pad, "no matches", ui::C_MUTE);
  footer({"type to find  enter open", "\x18\x19 move  esc back"});
}

static void keySearch(const KeyEvent& k) {
  const int n = (int)s_results.size();
  if (k.esc) { go(s_searchBack); refreshShown(); return; }
  if (k.up) { if (n) s_searchIdx = (s_searchIdx + n - 1) % n; return; }
  if (k.down) { if (n) s_searchIdx = (s_searchIdx + 1) % n; return; }
  if (k.left && k.fn) { s_searchIdx = std::max(0, s_searchIdx - 4); return; }
  if (k.right && k.fn) { if (n) s_searchIdx = std::min(n - 1, s_searchIdx + 4); return; }
  if (k.enter) { if (n) openNote(s_results[s_searchIdx], SEARCH); return; }
  if (k.back) {
    if (!s_query.empty()) { s_query.pop_back(); s_searchIdx = 0; runSearch(); }
    else if (!k.repeat) { go(s_searchBack); refreshShown(); }
    return;
  }
  if (k.ch && s_query.size() < 40) { s_query += k.ch; s_searchIdx = 0; runSearch(); }
}

// ============================================================================
//  Settings
// ============================================================================

enum SetItem { S_CATS, S_STORAGE, S_COPY, S_FONT, S_AI, S_HIDE, S_WIFI, S_CLOCK, S_UTC, S_FORGET, S_ABOUT, S_COUNT };

static store::Backend other() { return store::active() == store::BK_SD ? store::BK_FLASH : store::BK_SD; }

static void drawSettings() {
  ui::header("Settings");
  std::string val[S_COUNT];
  std::string lab[S_COUNT];
  uint64_t used, total;
  char b[48];
  lab[S_CATS] = "Categories";
  val[S_CATS] = std::to_string(cats.size());
  lab[S_STORAGE] = "Storage";
  val[S_STORAGE] = store::backendName(store::active());
  lab[S_COPY] = std::string("Copy notes to ") + (other() == store::BK_SD ? "SD card" : "device");
  val[S_COPY] = store::mounted(other()) ? "" : "no card";
  lab[S_FONT] = "Font";
  val[S_FONT] = settings::v.compact ? "compact" : "normal";
  lab[S_AI] = "AI suggestions";
  val[S_AI] = !ai::ok() ? "unavailable" : settings::v.ai ? "on" : "off";
  lab[S_HIDE] = "Hide completed";
  val[S_HIDE] = settings::v.hideDone ? "on" : "off";
  lab[S_WIFI] = "WiFi";
  val[S_WIFI] = settings::v.wifiSsid.empty() ? "not set" : view(settings::v.wifiSsid);
  lab[S_CLOCK] = "Sync clock";
  val[S_CLOCK] = clk::busy() ? std::string(spinner()) + " syncing" : clk::status();
  lab[S_UTC] = "UTC offset";
  snprintf(b, sizeof(b), "%+d:%02d", settings::v.utcOffsetMin / 60, abs(settings::v.utcOffsetMin % 60));
  val[S_UTC] = b;
  lab[S_FORGET] = "Forget learning";
  val[S_FORGET] = ai::status();
  lab[S_ABOUT] = "About";
  if (store::space(store::active(), used, total)) {
    char u[16], t[16];
    ui::human(used, u, sizeof(u));
    ui::human(total, t, sizeof(t));
    snprintf(b, sizeof(b), "%s/%s", u, t);
    val[S_ABOUT] = b;
  }
  ui::MenuItem items[S_COUNT];
  for (int i = 0; i < S_COUNT; i++) {
    items[i] = ui::MenuItem{lab[i].c_str()};
    items[i].right = val[i].c_str();
    items[i].rightCol = ui::C_HDR_FG;
  }
  ui::menu(ui::M.bodyY, items, S_COUNT, s_setIdx, s_setScroll);
  if (s_setIdx == S_UTC) footer({", / adjust  esc back"});
  else footer({"\x18\x19 move  enter change", "esc back"});
}

static void switchBackend(store::Backend to) {
  if (to == store::BK_SD && !store::mountSd()) { toast("No SD card"); return; }
  if (!store::use(to)) { toast("Could not open %s", store::backendName(to)); store::use(other()); return; }
  settings::v.backend = to;
  settings::save();
  s_homeIdx = 0;
  toast("Using %s", store::backendName(to));
}

static void keySettings(const KeyEvent& k) {
  if (k.up) { s_setIdx = (s_setIdx + S_COUNT - 1) % S_COUNT; return; }
  if (k.down) { s_setIdx = (s_setIdx + 1) % S_COUNT; return; }
  if (k.esc || (k.back && !k.repeat)) { go(HOME); return; }
  if (s_setIdx == S_UTC && (k.left || k.right)) {
    int v = settings::v.utcOffsetMin + (k.right ? 15 : -15);
    settings::v.utcOffsetMin = std::max(-12 * 60, std::min(14 * 60, v));
    settings::save();
    return;
  }
  if (!k.enter && !k.space) return;
  switch (s_setIdx) {
    case S_CATS:
      s_catsIdx = 0;
      s_catsScroll = 0;
      go(CATS);
      break;
    case S_STORAGE: {
      const store::Backend to = other();
      confirm("Storage", std::string("Keep notes on ") + (to == store::BK_SD ? "the SD card" : "device memory") +
                             "? Notes are not moved; use Copy for that.",
              [to]() { switchBackend(to); s_confirmBack = SETTINGS; });
      break;
    }
    case S_COPY: {
      const store::Backend to = other();
      if (to == store::BK_SD && !store::mountSd()) { toast("No SD card"); break; }
      std::string msg = std::string("Copy all notes to ") + (to == store::BK_SD ? "the SD card" : "device memory") + "?";
      if (store::hasNotes(to)) msg += " Files with the same name there are replaced.";
      confirm("Copy notes", msg, [to]() {
        toast(store::copyAll(store::active(), to) ? "Copied" : "Copy failed");
      });
      break;
    }
    case S_FONT:
      settings::v.compact = !settings::v.compact;
      settings::save();
      ui::setFont(settings::v.compact ? ui::FONT_COMPACT : ui::FONT_NORMAL);
      break;
    case S_AI:
      if (ai::ok()) { settings::v.ai = !settings::v.ai; settings::save(); }
      break;
    case S_HIDE:
      settings::v.hideDone = !settings::v.hideDone;
      settings::save();
      break;
    case S_WIFI:
      openEditor(E_WIFI_SSID, "WiFi name (SSID)", settings::v.wifiSsid, 32);
      break;
    case S_CLOCK:
      if (settings::v.wifiSsid.empty()) toast("Set WiFi first");
      else { clk::syncNow(); toast("Syncing time..."); }
      break;
    case S_UTC:
      break;
    case S_FORGET:
      confirm("Forget learning", "Stop using the notes filed so far as examples? Notes stay; only suggestions start over.",
              []() { ai::forget(); toast("Learning reset"); });
      break;
    case S_ABOUT: {
      char b[400];
      uint64_t used = 0, total = 0;
      store::space(store::active(), used, total);
      char u[16], t[16];
      ui::human(used, u, sizeof(u));
      ui::human(total, t, sizeof(t));
      snprintf(b, sizeof(b),
               "%s %s on %s\nModel TinyDecide %s, last pass %u ms (%d tok)\nNotes: %d in %d categories\n"
               "Storage: %s, %s of %s used\nClock: %s\nFree RAM %u KB (largest %u KB)",
               APP_NAME, APP_VERSION, keys::boardName(), TD_MODEL_VARIANT, (unsigned)ai::lastMs(), ai::lastTokens(),
               (int)notes.size(), (int)cats.size(), store::backendName(store::active()), u, t, clk::status(),
               (unsigned)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024),
               (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024));
      info("About", b);
      break;
    }
  }
}

// ============================================================================
//  Settings > Categories: list, then one category's fields
// ============================================================================

static void drawCats() {
  ui::header("Categories", (std::to_string(cats.size()) + "/16").c_str(), ui::C_DIM);
  std::vector<std::string> right(cats.size());
  std::vector<ui::MenuItem> items;
  for (size_t i = 0; i < cats.size(); i++) {
    std::string f;
    if (cats[i].check) f += "[x]";
    if (cats[i].phone) f += f.empty() ? "tel" : " tel";
    if (cats[i].time) f += f.empty() ? "time" : " time";
    right[i] = f;
    ui::MenuItem it{cats[i].name.c_str()};
    it.right = right[i].c_str();
    items.push_back(it);
  }
  items.push_back({"+ New category", nullptr, -1, ui::C_HDR_FG});
  ui::menu(ui::M.bodyY, items.data(), (int)items.size(), s_catsIdx, s_catsScroll);
  footer({"\x18\x19 move  enter edit", "esc back"});
}

static void keyCats(const KeyEvent& k) {
  const int n = (int)cats.size() + 1;
  if (k.up) s_catsIdx = (s_catsIdx + n - 1) % n;
  else if (k.down) s_catsIdx = (s_catsIdx + 1) % n;
  else if (k.esc || (k.back && !k.repeat)) go(SETTINGS);
  else if (k.enter) {
    if (s_catsIdx == n - 1) {
      if (cats.size() >= 16) { toast("16 categories max"); return; }
      openEditor(E_NEW_CAT, "New category", "", 24);
    } else {
      s_ceCat = s_catsIdx; s_ceIdx = 0; s_ceScroll = 0;
      go(CATEDIT);
    }
  }
}

enum CatField { CF_NAME, CF_PROMPT, CF_CHECK, CF_PHONE, CF_TIME, CF_UP, CF_DOWN, CF_DELETE, CF_COUNT };

static void drawCatEdit() {
  if (s_ceCat >= (int)cats.size()) { go(CATS); return; }
  const store::Category& c = cats[s_ceCat];
  const int cnt = store::countIn(s_ceCat);
  ui::header(c.name.c_str(), (std::to_string(cnt) + (cnt == 1 ? " note" : " notes")).c_str(), ui::C_DIM);
  std::string prompt = c.hint.empty() ? "(name)" : view(c.hint);
  std::string name = view(c.name);
  const char* lab[CF_COUNT] = {"Name", "Prompt", "Checklist", "Phone/email rule", "Clock time rule",
                               "Move up", "Move down", "Delete category"};
  const char* val[CF_COUNT] = {name.c_str(), prompt.c_str(), c.check ? "on" : "off", c.phone ? "on" : "off",
                               c.time ? "on" : "off", "", "", ""};
  ui::MenuItem items[CF_COUNT];
  for (int i = 0; i < CF_COUNT; i++) {
    items[i] = ui::MenuItem{lab[i]};
    items[i].right = val[i];
    items[i].rightCol = ui::C_HDR_FG;
  }
  items[CF_DELETE].accent = ui::C_ERR;
  ui::menu(ui::M.bodyY, items, CF_COUNT, s_ceIdx, s_ceScroll);
  switch (s_ceIdx) {
    case CF_PROMPT: footer({"what the AI reads for it", "enter edit  esc back"}); break;
    case CF_PHONE:  footer({"phone/email -> this one", "enter toggle  esc back"}); break;
    case CF_TIME:   footer({"clock times -> this one", "enter toggle  esc back"}); break;
    case CF_CHECK:  footer({"[ ] / [x] items", "enter toggle  esc back"}); break;
    default:        footer({"\x18\x19 move  enter change", "esc back"}); break;
  }
}

static void keyCatEdit(const KeyEvent& k) {
  if (s_ceCat >= (int)cats.size()) { go(CATS); return; }
  if (k.up) { s_ceIdx = (s_ceIdx + CF_COUNT - 1) % CF_COUNT; return; }
  if (k.down) { s_ceIdx = (s_ceIdx + 1) % CF_COUNT; return; }
  if (k.esc || (k.back && !k.repeat)) { s_catsIdx = s_ceCat; go(CATS); return; }
  if (!k.enter && !k.space) return;
  store::Category& c = cats[s_ceCat];
  switch (s_ceIdx) {
    case CF_NAME:
      s_cat = s_ceCat;
      openEditor(E_RENAME_CAT, "Category name", c.name, 24);
      break;
    case CF_PROMPT:
      openEditor(E_CAT_HINT, "Prompt for the AI", c.hint.empty() ? lower(c.name) : c.hint, 48);
      break;
    case CF_CHECK: store::setCategoryFlags(s_ceCat, !c.check, c.phone, c.time); break;
    case CF_PHONE: store::setCategoryFlags(s_ceCat, c.check, !c.phone, c.time); break;
    case CF_TIME:  store::setCategoryFlags(s_ceCat, c.check, c.phone, !c.time); break;
    case CF_UP:
    case CF_DOWN: {
      const int dir = s_ceIdx == CF_UP ? -1 : 1;
      if (store::moveCategory(s_ceCat, dir)) { s_ceCat += dir; ai::categoriesChanged(); }
      break;
    }
    case CF_DELETE: {
      if (cats.size() <= 1) { toast("Keep at least one"); break; }
      const int ci = s_ceCat;
      int to = (int)cats.size() - 1;
      if (to == ci) to = ci - 1;
      const int cnt = store::countIn(ci);
      std::string msg = "Delete \"" + c.name + "\"?";
      if (cnt) msg += " Its " + std::to_string(cnt) + " notes move to " + cats[to].name + ".";
      confirm("Delete category", msg, [ci, to]() {
        store::deleteCategory(ci, to);
        ai::categoriesChanged();
        s_catsIdx = 0;
        s_confirmBack = CATS;
        toast("Category deleted");
      });
      break;
    }
  }
}

// ============================================================================
//  Confirm / info / boot prompt
// ============================================================================

static void drawConfirm() {
  ui::header(s_confirmTitle.c_str(), nullptr);
  ui::wrap(ui::M.pad * 2, ui::M.bodyY + ui::M.pad, ui::charsFor(ui::M.w - ui::M.pad * 4), s_confirmText.c_str(), ui::C_FG);
  footer({"enter yes  esc no"});
}

// After any change to the notes, list and search indices are stale; rebuild the one shown.
static void refreshShown() {
  if (s_screen == LIST) refreshList();
  if (s_screen == SEARCH) runSearch();
}

static void keyConfirm(const KeyEvent& k) {
  if (k.enter) {
    auto yes = s_onYes;
    s_onYes = nullptr;
    if (yes) yes();             // may retarget s_confirmBack
    go(s_confirmBack);
    refreshShown();
  } else if (k.esc || (k.back && !k.repeat) || k.ch == 'n' || k.ch == 'N') {
    go(s_confirmBack);
    s_onYes = nullptr;
  }
}

static void drawInfo() {
  ui::header(s_infoTitle.c_str());
  ui::wrap(ui::M.pad * 2, ui::M.bodyY + ui::M.pad, ui::charsFor(ui::M.w - ui::M.pad * 4), s_infoText.c_str(), ui::C_FG);
  footer({"esc back"});
}

static void keyInfo(const KeyEvent& k) {
  if (k.esc || k.enter || (k.back && !k.repeat)) go(s_infoBack);
}

static void drawBootSd() {
  ui::header(APP_NAME, "no SD", ui::C_ERR);
  ui::text(ui::M.pad * 2, ui::M.bodyY + ui::M.pad, "SD card not found.", ui::C_WARN);
  ui::MenuItem items[2] = {{"Retry"}, {"Use device memory"}};
  int scroll = 0;
  ui::menu(ui::M.bodyY + ui::M.lineH + ui::M.pad * 3, items, 2, s_bootIdx, scroll);
  footer({"\x18\x19 move  enter choose"});
}

static void keyBootSd(const KeyEvent& k) {
  if (k.up || k.down) s_bootIdx ^= 1;
  if (!k.enter) return;
  if (s_bootIdx == 0) {
    if (store::mountSd() && store::use(store::BK_SD)) go(HOME);
    else toast("Still no card");
  } else {
    store::use(store::BK_FLASH);
    settings::v.backend = store::BK_FLASH;
    settings::save();
    go(HOME);
  }
}

// ============================================================================
//  Boot + loop
// ============================================================================

static void splash(const char* line) {
  ui::beginFrame();
  ui::center(ui::M.h / 4, APP_NAME, ui::C_HDR_FG, 2);
  ui::center(ui::M.h / 4 + ui::M.lineH * 2, keys::boardName(), ui::C_DIM);
  ui::center(ui::M.h * 3 / 4, line, ui::C_DIM);
  ui::endFrame();
}

void begin() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(1);
  // The full-screen canvas first, while the heap is still in one piece.
  s_cv = new M5Canvas(&M5.Display);
  s_cv->setColorDepth(16);
  s_cv->createSprite(M5.Display.width(), M5.Display.height());
  settings::begin();
  ui::begin(s_cv, M5.Display.width(), M5.Display.height(), settings::v.compact ? ui::FONT_COMPACT : ui::FONT_NORMAL);
  keys::begin();
  remote::begin(s_cv);
  splash("starting...");

  store::mountFlash();
  const bool sd = store::mountSd();
  store::Backend want = (store::Backend)settings::v.backend;
  if (want == store::BK_NONE) {               // first boot: the card if there is one
    want = sd ? store::BK_SD : store::BK_FLASH;
    settings::v.backend = want;
    settings::save();
  }
  if (want == store::BK_SD && !sd) {
    go(BOOTSD);
  } else if (!store::use(want)) {
    store::use(store::BK_FLASH);
  }
  clk::begin();
  splash("loading model...");
  ai::begin();
  ESP_LOGI(TAG, "free heap %u, largest block %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

static void draw() {
  ui::beginFrame();
  switch (s_screen) {
    case HOME: drawHome(); break;
    case LIST: drawList(); break;
    case NOTE: drawNote(); break;
    case EDIT: drawEdit(); break;
    case PICK: drawPick(); break;
    case SEARCH: drawSearch(); break;
    case SETTINGS: drawSettings(); break;
    case CONFIRM: drawConfirm(); break;
    case BOOTSD: drawBootSd(); break;
    case INFO: drawInfo(); break;
    case CATS: drawCats(); break;
    case CATEDIT: drawCatEdit(); break;
  }
  ui::endFrame();
}

void handle(const KeyEvent& k) {
  switch (s_screen) {
    case HOME: keyHome(k); break;
    case LIST: keyList(k); break;
    case NOTE: keyNote(k); break;
    case EDIT: keyEdit(k); break;
    case PICK: keyPick(k); break;
    case SEARCH: keySearch(k); break;
    case SETTINGS: keySettings(k); break;
    case CONFIRM: keyConfirm(k); break;
    case BOOTSD: keyBootSd(k); break;
    case INFO: keyInfo(k); break;
    case CATS: keyCats(k); break;
    case CATEDIT: keyCatEdit(k); break;
  }
}

void loop() {
  static uint32_t lastDraw = 0;
  M5.update();
  KeyEvent k = keys::poll();
  if (!k.any) remote::poll(k);    // scripted keys (tools/remote.py) only when no real key is down
  if (k.any) handle(k);
  if (s_screen != BOOTSD) ai::tick();
  // ~25 fps, or at once after a key. Fewer frames leave more of core 0 to the model's worker.
  const uint32_t now = millis();
  if (k.any || now - lastDraw >= 40) {
    draw();
    lastDraw = now;
  }
}

}  // namespace app
