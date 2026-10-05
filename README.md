# Pocket Notes

**A note-taking app for the M5Stack Cardputer that files each note into a list for you.** Type a
thought and press Enter. A small AI model running on the Cardputer picks the list. It works offline
and needs no phone or account.

<p align="center"><img src="docs/media/demo.gif" width="480" alt="Pocket Notes running on a Cardputer"></p>
<p align="center"><a href="docs/media/demo.mp4">Watch the 50-second demo (MP4)</a>, recorded from the device's own screen at real speed.</p>

## Why

- **Fast capture.** You type the note, press Enter, and the suggested list is already selected. Press Enter again to save it.
- **No sorting by hand.** Todo, Shopping, Ideas, Events, Contacts and Notes are built in, and you can add your own lists.
- **Gets better as you use it.** Every note you file is an example of its list. It gets about 62% of first guesses right, and about 75% after 5 notes in each list.
- **Private.** The model (6 MB, about 2 s per note) runs on the ESP32-S3. Nothing leaves the device.
- **Your files.** Notes are plain Markdown on the SD card or in the Cardputer's own memory, so no card is needed. They open in Obsidian.

| | | |
|:-:|:-:|:-:|
| <img src="docs/media/home.png" width="240"><br>Your lists | <img src="docs/media/suggest.png" width="240"><br>The AI picks the list | <img src="docs/media/checklist.png" width="240"><br>Checklists |
| <img src="docs/media/search.png" width="240"><br>Search as you type | <img src="docs/media/categories.png" width="240"><br>Edit lists and their AI prompts | |

## Install

Flash with PlatformIO. The same image runs on the Cardputer and the Cardputer ADV.

```sh
cd firmware && pio run -t upload
```

Flashing over USB replaces whatever is on the device. If the uploader can't connect, hold **G0**
while pressing **reset** and try again.

## Use

| Key | Action |
|---|---|
| `n` | new note (the AI suggests a list) |
| `;` `.` | up / down |
| `enter` | open, save, file |
| `space` | tick a checklist item |
| `f` | search |
| `e` `m` `p` `d` | edit, move, pin, delete |
| `` ` `` / `del` | back |

The footer always shows the keys for the current screen. Settings has the lists and their AI
prompts, SD or device storage, and WiFi for setting the clock. The Cardputer has no clock of its
own, so notes are numbered rather than dated unless WiFi is set.

The app keeps one Markdown file per list, plus a file with the lists themselves:

```
/notes/todo.md        - [ ] call the plumber ^n12
/notes/ideas.md       - solar moisture sensor #pinned ^n9
/notes/categories.txt Todo | a task to do | check
```

## How it works

TinyDecide is a 12-layer encoder that answers plain-language questions about a piece of text.
Pocket Notes asks it one question per note: *"What kind of note is this?"*. The options are your
lists' prompts. A C++ port runs the 4-bit weights on both cores of the ESP32-S3 using its SIMD
instructions. Each filed note updates an average vector for its list. The model adds a new note's
similarity to that vector to the list's score, so it learns without retraining. The details and
measurements are in [docs/PLAN.md](docs/PLAN.md).

```sh
node host/notes_eval.mjs                           # list accuracy, zero-shot and after learning
node host/make_ref.mjs && sh host/engine_test.sh   # C++ engine vs the JS reference, on a PC
~/.platformio/penv/bin/python tools/remote.py --script tools/smoke.txt   # scripted test on the device
```

## License

[MIT](LICENSE)
