# Pocket Notes

A pocket note-taking app for the M5Stack Cardputer (original and ADV). Notes are sorted into
categories, Todo and Shopping are checklists, and an on-device model
([TinyDecide](../tinydecide)) suggests the category for every new note and learns from what you
file. Fully offline. Notes are plain Markdown on the SD card or in the Cardputer's own flash.

| You type | It suggests |
|---|---|
| `buy milk, eggs, coffee` | **Shopping** |
| `call mom tomorrow 6pm` | **Todo** (or Events) |
| `Anna new number 555 0134` | **Contacts** |
| `idea: solar plant moisture sensor` | **Ideas** |
| `parked on level 3, row F` | **Notes** |

## Using it

**Home** lists `+ New note`, `Search`, your categories (with open/total counts for
checklists) and `Settings`.

- `+ New note` (or `n`): type, press Enter, and the **File as** list opens with the model's pick
  highlighted and a percentage beside each category. Enter files the note. Arrows pick another
  category, and `+ New category` makes one on the spot.
- Inside a category, `+ New note` files straight into it with no question asked.
- **Checklists** (Todo, Shopping and any category flagged `check`): `space` marks an item complete
  or incomplete. Completed items sink below open ones; `h` hides or shows them.
- On a note: `enter` opens it, `e` edits, `m` moves to another category, `p` pins it to the top,
  `d` deletes (it asks first).
- **Search** (`f`): type words and the results update as you type. Every word must match, and
  matches are highlighted. `f` inside a category searches only that category.
- On Home, on a category row: `r` renames it, `d` deletes it (its notes move to another category).

Keys follow the Cardputer legends: `;` `.` up/down, `,` `/` page, `enter`, `del` back,
`` ` `` esc. In the editor every key types; `fn+,` / `fn+/` move the cursor.

### It learns from you

The model gets about 62% of first guesses right on the bundled test set. Every note you file
becomes an example of its category. The newest 8 per category are embedded in the background (the
footer shows `learning 3/40`), and after about 5 notes per category it gets about 75% right.
Phone numbers, emails and clock times count as extra evidence for Contacts and Events. Renaming or
adding a category keeps everything learned. `Settings > Forget learning` starts suggestions over
without touching your notes.

### No clock needed

The Cardputer has no battery-backed clock, so nothing depends on the date. Each note has a
sequence number (`#42`) and lists sort newest first. If you enter WiFi in Settings (or in
`settings.ini`), the clock is synced over NTP at boot and new notes are stamped with the date
(`➕ 2026-10-05`). WiFi is switched off again right after the sync. A board with an RTC uses it.

### SD card or device memory

`Settings > Storage` chooses where notes live: the SD card (`/notes/`) or the device's own 512 KB
flash, so the app works with no card at all. `Copy notes to …` copies everything between the two.
If the SD card is missing at boot, you are asked whether to retry or use device memory.

## Files

```
/notes/categories.txt   Name | what the model sees | flags (check, phone, time)
/notes/todo.md          - [ ] call the plumber ➕ 2026-10-05 ^n12
/notes/ideas.md         - solar plant moisture sensor #pinned ^n9
/notes/settings.ini     optional: wifi_ssid, wifi_pass, utc_offset_min
```

The `.md` files open in Obsidian; the Tasks plugin reads the checkboxes and dates. Lines added on a
PC without a `^n` id get one on the next boot. Starter files are in [sd/notes/](sd/notes/).

## Building

```sh
cd firmware && pio run -t upload        # ESP-IDF 5.5 via PlatformIO (pioarduino 55.03.312-1)
```

One image runs on both the original Cardputer and the ADV. It is 7.5 MB: 6.2 MB of model plus
code, in a 7.44 MB factory slot, with a 512 KB LittleFS `storage` partition after it. Flashing over
USB replaces whatever was on the device, M5Launcher included. The image also installs through
M5Launcher, which applies the partition table.

If the board does not answer the uploader (`No serial data received`), hold **G0** while pressing
**reset** (or while plugging in USB) to enter download mode, flash, and then press reset.

## Host tools (Node 18+, a C++17 compiler)

```sh
node model/verify.mjs          # JS engine vs the torch reference (0 mismatches)
node host/pack_model.mjs       # model/vocab.bin + firmware/components/tinydecide/td_meta.h
node host/make_ref.mjs && sh host/engine_test.sh   # firmware engine (C++) vs JS engine, on the PC
node host/notes_eval.mjs       # category accuracy: zero-shot and after 2/5/8 notes per category
~/.platformio/penv/bin/python tools/remote.py --script tools/smoke.txt   # scripted test on the device
```

`tools/remote.py` injects keys over USB, saves screenshots, and dumps the notes. See
[firmware/main/remote.h](firmware/main/remote.h) for the commands.

## Layout

```
firmware/main/                 app: screens (app.cpp), storage, settings, clock, AI glue, UI kit
firmware/components/tinydecide engine: tokenizer, Q4 encoder (PIE kernel, both cores), choice head
model/                         TinyDecide S768 weights + JS reference engine (see model/SOURCE.md)
host/                          packer, parity test, accuracy eval, learning reference (learn.mjs)
sd/notes/                      starter SD contents
tools/                         remote control + smoke test
docs/PLAN.md                   plan and status log
```

The UI copies the look of [ESP32 Cleaner](../esp32_cleaner): the same palette, 8x16 font, header,
footer and menu code.

## License

MIT
