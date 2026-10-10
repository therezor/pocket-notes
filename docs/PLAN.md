# Pocket Notes: plan

## Context
Pocket Inbox (a skeleton: echo loop, a JS reference engine, no device engine) becomes **Pocket Notes**: a note-taking app for the Cardputer that sorts notes into categories.
- **Main screen:** "+ New note", then the categories.
- **Categories:** the on-device TinyDecide model suggests one for each new note.
- **Time:** the Cardputer has no RTC, so the app must not depend on a clock.
- **Look:** a copy of the SD cleaner (`../esp32_cleaner`): palette, 8x16 font, header, footer and menu.
- **Delivery:** push to `git@github.com:therezor/pocket-notes.git`, then flash the Cardputer at `/dev/cu.usbmodem1101`.

What you chose:
- Classification: AI auto-classify now.
- Top item on the main screen: "+ New note".
- Time: no clock, plus optional NTP.
- Names: `pocket-notes` for folders, `notes` for the SD folder.

The routing seed run only reached 50%, so the model **preselects** a category and you confirm it with Enter. Every note you file becomes a training example, so suggestions improve as you use the app.

## Stage 0: get the build working (blocks everything)
`pio run` currently fails with `ModuleNotFoundError: SCons.Tool.FortranCommon`.
1. Run `pio run` in `esp32_cleaner`, which uses the same pioarduino 55.03.39 platform but the Arduino framework.
   - If it also fails, the PlatformIO/SCons install is broken. Read the traceback to find which file imports `FortranCommon`, check `~/.platformio/packages/tool-scons`, then try `pio upgrade` or reinstalling tool-scons.
   - If it builds, the problem is specific to the espidf builder.
2. Fallbacks, in this order:
   - (a) Fix the espidf builder or pin the platform/SCons version.
   - (b) Switch `platformio.ini` to `framework = arduino`, which the cleaner already builds with. Arduino core 3.x is IDF 5.5 underneath, so the IDF APIs, M5Unified and the `.S` kernel all still work; `app_main` becomes `setup()`/`loop()`.
3. The exit test is a flashable echo build.

## Stage 1: rename + notes app with the cleaner UI (first flashable version)
**Rename (inside the repo):**
- CMake `project(pocket_notes)`, the UI title "Pocket Notes", README, PLAN.md.
- `sd/inbox/` → `sd/notes/`, and `host/inbox_*` → `host/notes_*`.
- `rules.txt` is removed: the category question lives in code, and the categories come from `categories.txt`.

**UI: port from the cleaner** into `firmware/main/ui.{h,cpp}` and `layout.{h,cpp}`. This replaces the chat UI.
- **Palette:** `C_BG 0x0862`, `C_HDR_BG 0x1926`, `C_HDR_FG 0x3DFF`, `C_SEL_BG 0x220D`, `C_FG 0xE77E`, `C_DIM 0xADB9`, `C_MUTE 0x6B6D`, `C_OK`, `C_WARN`, `C_ERR`, `C_TRACK`.
- **Helpers copied as they are:** `layoutFor()`, `header`, `footer`/`footerf`, `menu`, `text`/`textRight`, `marquee`, `wrap`, `ellipsize`, `scrollbar`, and `center` for the splash.
- **Fonts:** `fonts::AsciiFont8x16`, with Font0 for "compact" mode.
- **Canvas:** a full-screen 16-bit `M5Canvas` (65 KB), as in the cleaner.
- **SRAM fallback:** if the canvas plus the engine buffers don't fit in SRAM (measured in Stage 2), draw the body straight to the display instead.
- **Keys:** copied from `esp32_cleaner/src/hal/hal_cardputer.cpp` on top of the vendored `keyboard/` driver.
  - New presses are detected by an FNV hash of the held keys.
  - Auto-repeat starts after 400 ms and repeats every 60 ms, for movement keys and backspace only.
  - `;` `.` move up/down, `,` `/` page, Enter, backspace = back, `` ` `` = esc.

**Screens**
- **Home**
  - Header: "Pocket Notes", with the clock (or "SD"/"no SD") on the right.
  - Rows: "+ New note", "Search", then each category with counts (e.g. `Todo  3/7` = 3 open of 7), then "Settings".
  - On a category row: `r` renames it, `d` deletes it (asks first, moves its notes to Notes).
- **Category view**
  - Rows: "+ New note" (filed straight into this category), then notes newest first, pinned ones first.
  - Checklist categories show `[ ]`/`[x]`.
  - Keys: Enter opens, space toggles done, `p` pins, `m` moves, `d` deletes (asks first), esc goes back.
- **Search**
  - Opened from the Home "Search" row, or with `f` on Home (all notes) or `f` in a category view (that category only).
  - Header: a query box in the editor style. Below it, live results that update on every keystroke.
  - Matching: case-insensitive. Every space-separated word must appear (AND). Done notes are included but listed after open ones.
  - Rows: a short category tag in C_DIM (e.g. `Todo`), then the note text with the first match shown in C_HDR_FG. The selected row scrolls with `marquee`.
  - Keys: typing edits the query, `;` `.` move through results, Enter opens the note view (whose esc returns to the results), esc goes back.
  - Implementation: a linear scan over the in-memory note index, which holds every note's text, category, id and flags (loaded at boot; at most a few thousand short lines). No extra files. The header shows the result count on the right.
- **Editor**
  - The cleaner's editor style: an outlined box with wrapped text (up to about 200 chars, one paragraph).
  - Footer: `enter save  esc cancel`.
- **File-as picker** (after saving from Home)
  - The category list with the model's pick preselected and its confidence on the right, plus "+ New category" at the end.
  - A spinner shows while the model runs. If you start moving, the suggestion no longer moves the cursor.
- **Note view:** wrapped text, `#42` or the date on the right. `e` edits, `m` moves, `d` deletes, space toggles.
- **Settings**
  - Storage (SD / Device), copy notes between them, compact font, AI suggest on/off, WiFi SSID/password, NTP status, "Sync time now", "Forget learning".
  - About: model version, free heap.

**Storage backend: SD card or device memory** (the app works without an SD card)
- **Partition table:** add a LittleFS `storage` partition by shrinking the factory app.
  - Today `partitions.csv` gives the whole 0x7F0000 to the app.
  - Target is factory 0x770000 (7.44 MB: 6.18 MB model + about 1.2 MB code) plus storage 0x80000 (512 KB, room for thousands of notes).
  - Check the code size against the Stage 0 build before fixing these numbers. If code needs more room, storage drops to 256 KB.
- **Filesystem:** device memory uses `joltwallet/littlefs`, which is power-loss safe, mounted at `/flash`. The SD card uses `esp_vfs_fat_sdspi_mount`, with pins from `M5.getPin()` as in the cleaner, mounted at `/sd`.
  - Both use the same `/notes/` layout and the same code, through one `store` root-path switch.
- **Choosing the backend:**
  - The choice lives in NVS, because it can't live on a backend that might be missing.
  - First boot: SD if a card is inserted, otherwise device.
  - Settings shows "Storage: SD / Device" and the free space on each.
- **SD selected but no card:** a boot prompt offers "Retry" or "Use device memory". It never writes to the other backend without asking, so notes don't get split.
- **Migration:** Settings has "Copy notes SD → device" and "device → SD". This copies the whole `/notes/` folder, with a confirm prompt when the target already has notes.
- **WiFi without a card:** SSID and password can be entered in Settings with the editor, so NTP also works without `settings.ini` on SD.

**Files** (`/notes/` on the active backend):
- `categories.txt`
  - One per line: `Name [| hint] [| check]`.
  - The hint is the option text the model sees; the name is used when there's no hint.
  - `check` makes it a checklist category.
  - Defaults: Todo (check), Shopping (check), Ideas, Events, Contacts, Notes.
- `<cat_id>.md`: one note per line, Obsidian-friendly.
  - Format: `- [ ] buy milk ➕ 2026-10-05 #pinned ^n42`.
  - The `➕ date` is written only when the clock is known. `^nID` is the stable id.
  - Notes in non-checklist categories are written `- text`.
  - Files are rewritten through a temp file + rename, so a power cut loses at most the note being saved.
- `settings.ini`: wifi_ssid, wifi_pass, utc_offset_min, compact, ai.

**Time without an RTC**
- Every note gets a sequence id `#N`. The next id is kept in NVS, and the max found on SD wins. Ordering uses the id, never time.
- If `M5.Rtc.isEnabled()` (an RTC may exist on the ADV; check at boot), use it.
- Else, if WiFi is set in `settings.ini`, a boot task runs SNTP (8 s timeout) and then turns WiFi fully off.
- Synced: the header shows HH:MM and new notes get a date. Not synced: no dates, and nothing breaks.

**Flash to test:** flash with `pio run -t upload`, and confirm it boots and SD works from the serial log.

## Stage 2: TinyDecide device engine (`firmware/components/tinydecide/`)
**Host packers** (Node, no dependencies):
- `host/pack_model.mjs` turns `meta.json` into:
  - `model/vocab.bin`: sorted wordpieces + ids, for binary-search lookup.
  - `td_meta.h`: tensor offsets/shapes, specials, format, temp/beta.
- `model.bin` is embedded unchanged with `.incbin` (`.align 16`).
  - Its Q4 layout (a nibble plane plus a separate bf16 scale plane, both per row) already matches `dot_q4q8_pie(nib, xq, scales, xs, nb)`.
  - The packer checks that every Q4 offset is 16-byte aligned.

**Engine (C++)**
- **Tokenizer:** ASCII-only WordPiece. The keyboard only types ASCII, so this means lowercase, splitting on punctuation, and the `##` prefix rule.
- **Request layout:** ported from `encodeRequest`.
- **Encoder:** electra embeddings, then 12 layers.
- **Streamed kernel** (tinydecide PLAN, SRAM plan line ~835):
  - Activations are quantized to Q8 once per layer input.
  - K and V for all tokens are kept in fp16.
  - Attention runs one head at a time, and W_o is accumulated into the residual.
  - The FFN runs in 64-neuron chunks.
  - Each weight row is read once per layer and applied to all T tokens. The rows are split across both cores, using the `mm_worker` pattern from `cardputer_ai/main/llm.cpp:232`.
- **Heads:** choice only (route), returning probs, confidence, `qvec` (h.P, 128-d) and `z0`.
- **Buffers:** one allocation at init, sized for T_MAX = 96. Max 48 tokens of note text are encoded.
- **Inference task:** a FreeRTOS task with a request queue. The UI keeps polling keys and redraws the spinner at a low rate.

**Parity test**
- `host/make_ref.mjs` runs the JS engine on about 20 notes with the category question and writes the expected ids and probs to a C table.
- A `-D TD_SELFTEST` build prints mismatches over serial.
- Exit: 0 token-id mismatches, the same pick on every case, and max |dp| < 0.02 (Q8 activations vs the fp32 JS).
- Also log latency (target ≤ 1.5 s) and heap high-water marks.

## Stage 3: auto-classify + learning
- **Question:** `choice: What kind of note is this? | <hint-or-name of each category>`.
  - Before fixing the default wording and hints, run `host/notes_eval.mjs` on relabelled scenarios. The wording is picked from that run.
- **Learning:** every filed note is an example, with no separate corrections file. That covers notes typed inside a category and notes confirmed in the picker.
  - For each category, the 8 most recent notes are embedded (`qvec`, `z0`) by a background task while the device is idle.
  - The embeddings are cached in `/notes/.cache/vectors.bin`, keyed by (note id, signature of the category set).
- **Changing the category set** (add, rename or delete a category) changes the signature. Only the cache is rebuilt, from the note texts on SD, so nothing learned is lost. The header shows `learning 3/40`.
- **Corrections maths:** port of `protosFor`, `lambdaFor`, `termFor` and `noteCentre` (`../tinydecide/playground/index.html:224-273`). The centre is the running mean of `qvec` over the notes it has seen.
- The suggestion is skipped when AI is off, or for notes added inside a category, which are filed straight away.

## Stage 4: publish
- Rewrite README and PLAN.md for Pocket Notes; the old status log is kept as history.
- Commit at each stage boundary.
- `git remote add origin git@github.com:therezor/pocket-notes.git`.
  - First check with `git ls-remote`. If the remote already has commits (README/LICENSE), rebase onto them.
  - Then `git push -u origin main`.
- **Final flash:** `pio run -t upload --upload-port /dev/cu.usbmodem1101`.
  - **This overwrites the bootloader, partition table and app. If M5Launcher is on the device, it is replaced.** The image also stays installable through M5Launcher later.
- **Very last:** rename the folder `~/Code/ESP32/pocket_inbox` → `~/Code/ESP32/pocket-notes` and do a clean `.pio` rebuild.
  - The Claude Code session/memory path follows the old folder name, so start the next session in the new folder.

## Verification
- **Host:**
  - `node model/verify.mjs` still reports 0 mismatches.
  - `node host/notes_eval.mjs` reports routing accuracy zero-shot and after 2/5/8 examples per category, which is the expected device behaviour.
- **Device, serial:**
  - The selftest parity build passes.
  - Latency and free-heap logs are within budget.
  - Boot log shows the active backend (SD/device) and its free space, the category count, and the time source (RTC/NTP/none).
  - Boot without a card: the app falls back to device memory, notes save, and they survive a power cycle.
  - Copying notes SD → device gives an identical `/notes/` listing.
- **Device, scripted:** a debug env (`cardputer-debug`) accepts key injection over USB serial and dumps the canvas, like the cleaner's `cardputer-shots` + `tools/grab-screenshots.py`.
  - The script adds notes from Home, accepts or changes the suggestion, toggles, moves and deletes, and adds a category.
  - It also searches: a two-word query must return only notes containing both words, across categories, and `f` inside a category must stay within that category.
  - It grabs screenshots to compare against the cleaner's style, and reads `/notes/*.md` back over serial.
- **Manual (you):** type a few notes on the device, pull the SD card, and open `/notes/` in Obsidian.

## Status log
- **2026-10-05: Pocket Inbox becomes Pocket Notes.** The quick-capture inbox, with its 7 hardcoded routes, two passes and span extraction, is replaced by a notes app.
  - Notes are sorted into user-editable categories. Todo and Shopping are checklists, with search, pin, move, an SD or device-memory backend and optional NTP.
  - TinyDecide answers one choice question per note ("What kind of note is this?", with the categories as options), and the user always confirms the pick.
- **Toolchain:** pioarduino 55.03.39 failed with `ModuleNotFoundError: SCons.Tool.FortranCommon`.
  - Cause: the platform pins pioarduino `tool-scons` 4.8.1 and PlatformIO Core 6.2 installs 4.11.1. The platform's version check deletes Core's SCons package while the build is running, and a lazily imported SCons module is then missing.
  - Fix: moved to platform 55.03.312-1 (IDF 5.5.5), which builds.
- **Engine (M1) done in C++** (`firmware/components/tinydecide/engine.cpp`). It builds for the host and the device.
  - Streamed kernel: the residual plus one int8 copy of the layer input; attention one head at a time with W_o accumulated into x; the FFN in 64-neuron chunks with fc2 accumulated into x. Scratch is about 2.2 KB per token.
  - `model.bin` is embedded unchanged: its Q4 planes already match `dot_q4q8_pie`. `vocab.bin` is a sorted table for binary search.
  - Host parity (`host/engine_test.sh`, 62 cases):
    - 0 token-id mismatches.
    - max|dp| 0.012 against the fp32 JS engine (from the Q8 activations).
    - 3 pick flips, all on near-ties (0.256 vs 0.252).
- **Routing** (`host/notes_eval.mjs`, 52 lines, 6 categories):

  | Wording | Zero-shot | k=5 | k=8 |
  |---|---|---|---|
  | Bare category names | 42–46% | 52% | 56% |
  | Descriptive hints ("a task to do", "something to buy", "an idea", "an appointment", "a phone number or email", "something to remember") | 48% | 71% | 73% |
  | The same hints + text rules (phone/email +3 for Contacts, clock time +1.5 for Events) | **62%** | **75%** | **75%** |

  - One yes/no question per category scored 48% at 65 tokens and was dropped.
  - Shipped: the hints + rules. Every filed note is an example (8 newest per category), and the picker always asks the user to confirm.
- **Partitions:** factory 0x770000 (7.44 MB) + LittleFS `storage` 0x80000. The image is 7.53 MB.
- **On the device** (Cardputer ADV):
  - Boot leaves 207 KB of heap free with no SD card (largest block 159 KB), or 178 KB free with the SD card mounted.
  - A first pass at 45 tokens took 3.4 s: matmul 1.7 s, newlib `erff` GELU 1.07 s, single-core attention 0.48 s.
  - After the fixes it takes **1.8 s**:
    - GELU from a 1/64-step table (error below 3e-5).
    - Attention and GELU split across both cores.
    - The worker runs at priority 3, above the UI.
  - Host parity after the fixes: max|dp| 0.015 and 4 near-tie flips.
  - A long note at T=77 needs about 166 KB in a fragmented heap. The scratch is now 5 blocks (x, Q, K, V, rest). If the allocation fails or would leave under 24 KB free, the engine retries with 8 fewer note tokens (seen: T=60, 2.5 s).
  - SD is on SPI2, because M5GFX drives the Cardputer display on SPI3.
  - Opening the USB serial port with RTS high resets the S3. `tools/remote.py` keeps RTS low.
- **Smoke test** (`tools/smoke.txt`, run with `tools/remote.py`) passes on the device:
  - It files a note through the AI picker (Shopping 97%).
  - It adds a note to the Todo checklist, ticks it and deletes it.
  - It runs a two-word search, opens the result and deletes it.
- **Feedback during testing:**
  - Footer hints rotate as short pages that fit the 29-character footer.
  - Checklist counts show completed/total.
  - The storage label reads "On device" / "On SD".
  - Settings > Categories edits the name, prompt (hint), flags and order.
- **Next:** holding the canvas in 8-bit if the heap gets tight with many notes.
- **2026-10-10: v1.1, upstream engine.** `firmware/components/tinydecide/` is now the `esp32/tinydecide/` component of the TinyDecide repo (commit `aa745b7`), copied unchanged. It started as this engine and adds a 4-token PIE kernel with the tile loop in assembly, LayerNorm and quantisation on both cores, fc2 in 256-neuron groups, and a 64-byte-aligned model.
  - API: `td::answer` with one `CHOICE` question replaces `td::choice`. `ai.cpp` passes `state_max = 40`, the old `STATE_MAX`, so notes keep the ~30-word limit and the same heap use. `K` stays 16, so `.learn.bin` caches still load.
  - The tokenizer is the reference one for all of Unicode, not ASCII-only. `vocab.bin` changed format and moved into the component; `host/pack_model.mjs` is gone.
  - Host parity (`host/engine_test.sh`, 63 cases): 0 token-id mismatches, max|dp| 0.016, 3 near-tie flips (the old engine: 0.015, 4 flips).
  - On the device: **0.67 to 0.9 s** per suggestion at 40 to 53 tokens, down from 1.8 s. The smoke test passes. The image is 7.59 MB of the 7.80 MB slot.

## History: Pocket Inbox (superseded)
- **2026-10-05: project skeleton.**
  - Created this folder. `model/` is copied from `tinydecide/web_S768`, and `node model/verify.mjs` passes: 0 mismatches, max|dp| 2.9e-6, 392 ms/request in Node.
  - **Rules grammar** is now `<type> <key>: <text> | <options>`. The key names the stored field. The category id is the option lowercased with non-alphanumerics turned into `_`, and it names both the `[section]` and the output file. Parser: `host/rules.mjs`.
  - **Firmware skeleton** (`firmware/`): display, keyboard, echo loop, and an engine component holding `dot_q4_pie.S`.
  - **`pio run` fails in this environment** with `ModuleNotFoundError: SCons.Tool.FortranCommon` (PlatformIO Core 6.2.0 / pioarduino 55.03.39). This is a toolchain problem, not a code problem, and is not fixed yet.
- **2026-10-05: seed M0 run** (32 lines in `host/inbox_scenarios.jsonl`, zero-shot, no corrections):

  | Field | Accuracy |
  |---|---|
  | Route | **50%** |
  | Time | 91% |
  | Day | 83% (92% lenient) |
  | Amount | 80% |
  | Person | 73% |
  | Phone | 75% |
  | Email | 100% |
  | Urgency | 25% |
  | Spent_on | 20% |

  - Pass 1 is 71 tokens on average (max 77); pass 2 is 23 on average.
  - **Pass-2 fields given a correct route:** amount 4/4, phone 3/3, email 2/2, spent_on 1/4. The overall pass-2 numbers above are dragged down by misrouted lines, whose fields are never asked.
  - **Route confidence barely separates right from wrong.**
    - Normalized-entropy confidence averages 0.16 when the route is correct and 0.11 when it is wrong. Top probability averages 0.41 vs 0.34.
    - The two nofit lines are low on both measures (0.05 / 0.26).
    - The current 0.35 threshold sends almost every entry to Review.
  - **Rewording the route question moved accuracy only slightly:** three wordings scored 43%, 53% and 50%. Todo is confused with event or idea, and shopping with idea or todo.
  - **Caveat:** this is n=30, labelled by one person, and several lines are genuinely ambiguous (todo vs event: "call mom tomorrow 6pm", "standup moved to 9:15 tomorrow"; note vs idea).
  - **Read:** the seed run suggests extraction is viable and 7-way routing is the weak spot. Confirm this on the full ~150-line set after an ambiguity review.
  - **Options for M0, to be tested:**
    1. Route with several yes/no questions ("mentions money that was spent", "something to buy", "has a phone number or email", "an appointment at a set time"), then use a C++ decision list with the span results as extra evidence (amount present → expense, and so on).
    2. Use fewer, broader categories.
    3. Measure how quickly corrections fix routing, starting from 5 corrections per category.
