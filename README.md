<p align="center">
  <img src="docs/media/demo.gif" width="600" alt="Pocket Notes on a Cardputer: a typed note is filed in Shopping by the on-device AI, a Todo item is ticked, search finds a note">
</p>

<h1 align="center">Pocket Notes</h1>

<p align="center">
  <b>Notes that file themselves, on a computer the size of a credit card.</b><br>
  Type a thought and press Enter. A tiny AI on the chip puts it in the right list.<br>
  No internet. No cloud. No phone.
</p>

<p align="center">
  <a href="https://github.com/therezor/pocket-notes/releases/latest"><b>⬇️ Download firmware</b></a> ·
  <a href="#-watch-it-work"><b>▶️ Watch it work</b></a> ·
  <a href="#-try-it-in-3-steps"><b>🚀 Try it</b></a> ·
  <a href="#-how-does-it-fit"><b>🔧 How it works</b></a>
</p>

---

## 🎬 Watch it work

<p align="center">
  <a href="https://github.com/therezor/pocket-notes/releases/download/1.0/pocket-notes-demo.mp4"><img src="docs/media/home.png" width="32%" alt="home"></a>
  <a href="https://github.com/therezor/pocket-notes/releases/download/1.0/pocket-notes-demo.mp4"><img src="docs/media/suggest.png" width="32%" alt="the AI picks the list"></a>
  <a href="https://github.com/therezor/pocket-notes/releases/download/1.0/pocket-notes-demo.mp4"><img src="docs/media/checklist.png" width="32%" alt="checklist"></a>
</p>

<p align="center"><a href="https://github.com/therezor/pocket-notes/releases/download/1.0/pocket-notes-demo.mp4"><b>Download the 50-second demo (MP4)</b></a>. It was recorded from the device's own screen at real speed.</p>

What happens in the demo:

```text
type  "buy oat milk and coffee beans"   ->  AI: Shopping 88%   enter, filed
type  "dentist thursday 4pm"            ->  AI: Events 68%     enter, filed
Todo: tick "call the plumber", tick "book the car service"
search "milk"                           ->  1 note, the match highlighted
Settings > Categories > Shopping        ->  the prompt the AI reads: "something to buy"
```

## 🤯 Why it's different

A note app on a pocket keyboard is easy. Keeping the notes sorted is the part people give up on.

**Pocket Notes sorts them for you, on the chip.** It is a 10-million-parameter language model that
reads your note and picks the list. Shopping, Todo, Events, Contacts, Ideas, Notes, or lists you
make yourself. It runs on an ESP32-S3 with 512 KB of RAM. Unplug the Wi-Fi and it still works.

It also **learns from you**. Every note you file becomes an example for its list, and the next
guess takes it into account. Nothing is retrained and nothing is uploaded.

## 📊 By the numbers

| | Pocket Notes |
|---|---|
| 🧠 **Model** | TinyDecide, 10.4M parameters, 12 layers, 4-bit weights |
| 💾 **Memory** | 512 KB of RAM, no PSRAM |
| 📦 **Model file** | 6.2 MB, built into the firmware |
| ⚡ **Speed** | about 1.8 s per note, on both CPU cores |
| 🎯 **First guess right** | 62% out of the box, 75% after 5 notes in each list |
| 🗂️ **Storage** | SD card or 512 KB of the device's own flash |
| 📡 **Internet** | Not needed. WiFi only sets the clock, if you want dates. |

## 📸 Screens

<p align="center">
  <img src="docs/media/home.png" width="32%" alt="your lists with completed/total counts">
  <img src="docs/media/search.png" width="32%" alt="search as you type">
  <img src="docs/media/categories.png" width="32%" alt="edit a list and the prompt the AI reads">
</p>

## 🧸 What it's good at (and what it's not)

| 👍 Good at | 👎 Not good at |
|---|---|
| Quick capture: type, Enter, Enter | Notes that fit two lists ("call mom at 6pm": Todo or Event?) |
| Shopping items, ideas, phone numbers, emails, times | Long notes (it reads the first ~30 words) |
| Checklists with completed/total counts | Languages other than English |
| Getting better at *your* lists as you file notes | Being right every time, so it always asks first |

It always shows its pick and waits for Enter. A wrong guess costs one key press.

## 🚀 Try it in 3 steps

1. **Get a Cardputer.** An [M5Stack Cardputer ADV](https://docs.m5stack.com/en/core/Cardputer-Adv) or the original Cardputer. The same firmware runs on both.
2. **Flash it.** Grab `pocket_notes_1.0_full.bin` from [Releases](https://github.com/therezor/pocket-notes/releases/latest) and write it at address `0x0`. You can use the [ESP web flasher](https://espressif.github.io/esptool-js/) or run `esptool.py write_flash 0x0 pocket_notes_1.0_full.bin`. You can also build it yourself: `cd firmware && pio run -t upload`.
3. **Press `n`, type, press Enter.** That's all. No account, and the SD card is optional.

| Key | What it does |
|---|---|
| `n` | new note (the AI suggests a list) |
| `;` `.` | move up / down |
| `Enter` | open, save, file |
| `Space` | tick a checklist item complete / incomplete |
| `f` | search; inside a list it searches that list |
| `e` `m` `p` `d` | edit, move, pin, delete |
| `` ` `` or `Del` | back |

The footer always shows the keys for the screen you're on.

## 🗂️ Your notes are plain files

One Markdown file per list, on the SD card or in device memory. They open in Obsidian, where the
Tasks plugin reads the checkboxes.

```text
/notes/todo.md          - [ ] call the plumber ^n12
/notes/ideas.md         - solar moisture sensor #pinned ^n9
/notes/categories.txt   Shopping | something to buy | check
```

The Cardputer has no clock, so notes are numbered and listed newest first. If you add WiFi in
Settings, the clock syncs once at boot and new notes get a date.

## 🔧 How does it fit?

The short version:

1. **It doesn't write, it chooses.** TinyDecide is an encoder that answers a question with a probability per option, in one pass. Pocket Notes asks it *"What kind of note is this?"* and gives your lists as the options.
2. **Your words steer it.** Each list has a prompt, the short text the model reads for it, such as "something to buy" or "a phone number or email". You can edit the prompts in Settings.
3. **Squeeze hard.** The weights are 4 bits each and read straight from flash. A C++ port runs them on both cores using the ESP32-S3's vector (SIMD) instructions. Working memory is about 2 KB per word piece.
4. **Learn without training.** Filed notes become per-list average vectors that are added to the model's scores. Simple text rules add a little evidence: a phone number points to Contacts and a clock time to Events.

Measurements, design notes and the full status log are in **[docs/PLAN.md](docs/PLAN.md)**. To test it:

```sh
node host/notes_eval.mjs                            # accuracy, zero-shot and after learning
node host/make_ref.mjs && sh host/engine_test.sh    # the C++ engine against the JS reference, on a PC
~/.platformio/penv/bin/python tools/remote.py --script tools/smoke.txt   # scripted test on the device
```

<details>
<summary><b>📜 Changelog</b></summary>

- **v1.0** — first release
  - Lists (categories) with an on-device AI suggestion for every new note. It learns from the notes you file.
  - Checklists with completed/total counts, hide completed, pin, move, edit, delete.
  - Search as you type, in every list or one.
  - Settings > Categories edits the names, the AI prompts, the checklist and text-rule flags, and the order.
  - Notes on the SD card or in device memory, with copying between the two.
  - Optional WiFi clock sync. Without it, notes are numbered rather than dated.

</details>

## 📄 License

Code: MIT (see [LICENSE](LICENSE)). The embedded model is TinyDecide (S768 build) by REZOR,
initialised from Google's ELECTRA-small (Apache 2.0). See [model/SOURCE.md](model/SOURCE.md).
Made by **REZOR** ([@therezor](https://github.com/therezor)).
