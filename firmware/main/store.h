// Notes storage: two backends with the same /notes/ layout, and the in-memory note index.
//
//   SD card        FAT over SPI, mounted at /sd     -> /sd/notes/
//   Device memory  LittleFS "storage" partition     -> /flash/notes/
//
// Files in the notes folder:
//   categories.txt   one category per line: Name | hint | flags   (flags: check, phone, time)
//   <cat_id>.md      one note per line, Obsidian-friendly:
//                      - [ ] buy milk ➕ 2026-10-05 #pinned ^n42     (checklist category)
//                      - parked on level 3 ^n43                     (plain category)
//   settings.ini     optional; WiFi etc. imported into NVS at boot (handy when editing on a PC)
//
// The whole index lives in RAM (a few hundred short notes is a few tens of KB). Every change
// rewrites the one category file it touches through a temp file, so a power cut loses at most
// the change being saved.
#pragma once
#include <stdint.h>

#include <string>
#include <vector>

namespace store {

enum Backend : uint8_t { BK_NONE = 0, BK_SD = 1, BK_FLASH = 2 };

struct Category {
  std::string name;     // shown in the UI
  std::string hint;     // what the model sees as the option text ("" -> name)
  std::string id;       // file name stem: name lowercased, non-alphanumerics -> '_'
  bool check = false;   // checklist: notes have [ ] / [x]
  bool phone = false;   // text rule: phone numbers and emails point here
  bool time = false;    // text rule: clock times point here
  const char* option() const { return hint.empty() ? name.c_str() : hint.c_str(); }
};

struct Note {
  uint32_t id = 0;
  uint8_t cat = 0;
  bool done = false;
  bool pinned = false;
  char date[11] = {0};  // YYYY-MM-DD when the clock was known at creation, else ""
  std::string text;
};

extern std::vector<Category> cats;
extern std::vector<Note> notes;

// ---- backends
bool mountFlash();
bool mountSd();                    // (re)tries the card; false when there is none
bool mounted(Backend b);
Backend active();
const char* backendName(Backend b);
bool use(Backend b);               // switch the notes folder and reload everything from it
bool space(Backend b, uint64_t& used, uint64_t& total);
bool hasNotes(Backend b);          // the backend's notes folder already holds notes
bool copyAll(Backend from, Backend to);   // copies every file of the notes folder

// ---- notes
int addNote(const std::string& text, int cat, const char* date);   // -> index in notes
bool saveCat(int cat);             // rewrite <id>.md
void removeNote(int idx);
void moveNote(int idx, int cat);
int countIn(int cat, int* open = nullptr);
int find(uint32_t id);             // index of the note with this id, or -1

// ---- categories
int addCategory(const std::string& name);       // -> index, or -1 if the name is taken/empty
bool renameCategory(int cat, const std::string& name);
bool deleteCategory(int cat, int moveTo);      // its notes move to moveTo first
uint32_t catSignature();            // changes whenever the option set the model sees changes

std::string slug(const std::string& name);
std::string root();                 // notes folder of the active backend
uint32_t lastError();               // errno-style code of the last failed write (0 if none)

}  // namespace store
