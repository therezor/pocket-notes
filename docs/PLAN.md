# Pocket Inbox: offline quick-capture for the Cardputer, powered by TinyDecide

## Context
TinyDecide needs a demo on the Cardputer that is genuinely useful. Today the Cardputer ecosystem is dominated by pentest firmware, launchers and emulators. Productivity apps are thin: the community app lists only show Micro-journal, PDAputer, a stopwatch and a weather app. The only on-device AI app is TinyTalk.

Pocket Inbox turns the Cardputer into a **pocket capture device**. You type any thought in a few seconds and it gets filed: todos, events, expenses, contacts, notes and ideas. Each is saved with its fields pulled out, fully offline, as plain files that open in Obsidian or a spreadsheet.

It shows off everything TinyDecide does:
- all four answer types: choice, yes/no (noul), score and span
- questions that users write themselves at runtime
- corrections that work without retraining
- calibrated confidence

## 1. What the app is for the user

**The problem.** Ideas, tasks and receipts come up while you're away from your desk. Opening a phone means notifications and lost focus, and phone notes end up as an unsorted pile. Proper task or finance apps make you fill in a form for every entry.

**Pocket Inbox.** You type one line in plain language and press Enter. About a second later the line is filed into the right list, with its date, time, amount and person already filled in. You never pick a category or fill a form. Nothing leaves the device.

| You type | What it becomes |
|---|---|
| `call mom tomorrow 6pm` | **Todo**, due Tue 18:00 |
| `lunch 12.40 with Mark` | **Expense**: 12.40 · food · with Mark |
| `Anna new number 555 0134` | **Contact**: Anna · 555 0134 |
| `server down again!! fix before demo friday` | **Todo**, urgency **high**, due Fri |
| `dentist oct 14 at 9:30` | **Event** on Oct 14, 09:30 |
| `idea: solar plant moisture sensor` | **Idea** |
| `buy milk, eggs, coffee` | **Shopping** |

**Why it helps**
- **Capture in under 5 s.** Pull it from a pocket, type, Enter. The raw line is written to SD before the model even runs, so nothing is ever lost.
- **Zero sorting.** The daily **Today** screen shows what's due, what's urgent and what the model wasn't sure about.
- **Your own categories.** Add something like "workout → extract the distance" by editing a text file. No retraining and no PC tools.
- **It learns your habits.** Correct a wrong category once and similar entries are routed right afterwards.
- **Private and offline.** No account, no cloud, no WiFi needed.
- **Your data stays usable.** It is Markdown in Obsidian Tasks format plus CSV for expenses and contacts. Pull the SD card, or use the optional WiFi export.

## 2. Screens and controls
These are built with M5GFX on the 240×135 screen, reusing `cardputer_ai/main/ui.cpp` and `keyboard/`. LVGL is out because flash and SRAM are tight.

1. **Capture (home).**
   - Shows a big input line and a status bar (clock, battery, number of items waiting for review).
   - Enter submits. Input is capped at about 120 tokens, which is one state window, so no windowed reading is needed.
   - You can type the next entry immediately, because the model runs on the other core and works through a queue.
2. **Result card (does not block typing).** This appears above the input line about 1 s after Enter.
   - Shows the category, a confidence bar and the extracted fields.
   - **Typing any character accepts the card** and starts the next entry, so capture is never interrupted.
   - `Fn+F` ("fix last") or the Review screen opens the card for editing:
     - `Tab`/`←→` change the category, which records a correction.
     - `↑↓` + Enter edit a field.
     - `Esc` keeps the entry as a plain note.
   - If confidence is below the threshold (about 0.35, tuned in M0), the card shows "Not sure" and the entry goes to the Review queue.
3. **Today.** Overdue, today and tomorrow items plus high-urgency todos. Shortcuts: `D` done, `S` snooze one day.
4. **Lists.** One screen per category with a simple substring search (`/`), plus expense totals for this week and month. Totals are computed in C++, never by the model.
5. **Review.** Uncertain and recent items for a quick yes, change or correct.
6. **Rules.** Shows the active questions and reloads `rules.txt`. Editing happens in a minimal on-device line editor, or on a PC.
7. **Settings.** Set the clock manually or by NTP over WiFi, the confidence threshold, WiFi export on or off, and a reset-corrections button.

## 3. How the model is used

**Rules file** (`/inbox/rules.txt` on the card, starter copy in `pocket_inbox/sd/inbox/`, one question per line):
```
[route]
choice route: What kind of entry is this? | todo, event, expense, contact, note, idea, shopping

[all]
score urgency: How urgent is it? | low, medium, high
span time: Extract the time.
span day: Extract the day or date.
span person: Extract the person's name.

[expense]
span amount: Extract the amount of money.
choice spent_on: What was the money spent on? | food, transport, shopping, bills, fun, other

[contact]
span phone: Extract the phone number.
span email: Extract the email address.

[event]
span place: Extract the place.
```
- Line grammar: `<type> <key>: <text> | <options>`. The key names the stored field.
- A user-made category is a new option on the `[route]` line plus an optional `[<id>]` section, where the id is the option lowercased with spaces turned into `_`. It writes to `/inbox/<id>.md`.

**Passes.** The model uses full fusion, so question tokens go through all 12 layers. That means token count drives latency and SRAM (the streamed kernel caps it at about 180 live tokens per pass).
- **Pass 1:** `[route]` + `[all]`. That's a state of about 20–40 tokens plus about 50–60 question tokens, so it fits easily. Target ≤1 s.
- **Pass 2:** only when the chosen category has its own section, so expense, contact or event entries take two passes.
- The card shows the category after pass 1 and fills in the fields after pass 2.
- A packer splits question sets across passes under the token cap. This is needed anyway for user-made rule sets.

**Post-processing (C++, deterministic):**
- Day and time spans are turned into an actual date and time against the clock: today, tomorrow, weekdays, "oct 14", "6pm", "18:30".
- Amount spans are parsed to a number plus currency. Phone and email spans are checked with simple patterns. A span is dropped if its P(present) is under 0.5 or it fails the check.
- The model is weak at arithmetic, so dates, totals and maths never go through it.

**Corrections:**
- Port the playground logic from `playground/index.html` (`sig`, `protosFor`, `lambdaFor`, `noteCentre`, `termFor`, lines ~216–273).
- Each correction stores a small int8 vector plus logits, appended to `/inbox/corrections.bin`. Corrections are keyed by question signature, so **editing a question's wording resets its corrections**. The Rules screen warns about this.
- Choice, noul and score questions learn from corrections. Span edits only fix the saved data; the model's span head has no correction path.

## 4. Data on SD (`/inbox/`)
| File | Format |
|---|---|
| `log.jsonl` | Every entry: raw text, timestamp, all answers and confidences, plus any user edits. This is the source of truth; all other files are views built from it |
| `todo.md`, `shopping.md`, `<custom>.md` | `- [ ] 18:00 call mom 📅 2026-10-06 ⏫` (Obsidian Tasks style: due date + priority; the time stays in the text) |
| `events.md` | `- 2026-10-14 09:30 dentist @place` |
| `notes.md`, `ideas.md` | `- 2026-10-05 14:02 text` |
| `expenses.csv` | `date,amount,currency,category,who,text` |
| `contacts.csv` | `name,phone,email,text` |
| `rules.txt`, `corrections.bin`, `settings.ini` | config and learning state |

"Done" and "snooze" are recorded in `log.jsonl` and also rewritten in the Markdown views.

## 5. Firmware architecture
- **Firmware project:** `pocket_inbox/firmware/` (see section 6), using ESP-IDF/PlatformIO and cloned from the `cardputer_ai` skeleton (`platformio.ini`, `sdkconfig.cardputer*`, `partitions.csv`, `keyboard/`, `ui.cpp`).
- **Reusable engine component:** `components/tinydecide/` holds the tokenizer, encoder, heads and corrections. Later demos (IR remote, text adventure) can reuse it.
- **Model embedding:** the `web_S768` model is embedded with `.incbin`, as `cardputer_ai/main/model_data.cpp` does.
- **Flash:** 6.29 MB of model leaves about 2.0 MB for code.
- **Distribution:** the existing `partitions.csv` (factory app 0x7F0000 = 8.32 MB) already works with M5Launcher, which swaps in the partition table on install. Published via M5Launcher/M5Burner.
- **Tasks:**
  - An inference task is fed by a FreeRTOS queue of entries and returns results by event.
  - The matrix maths uses **both cores**, like the `mm_worker` pinned in `cardputer_ai/main/llm.cpp:246`. The latency estimates likely assume this.
  - During a pass the UI only does a light keyboard scan and redraws at low priority. SD writes happen between passes.
- **SRAM:**
  - The model needs about 144 KB with the streamed kernel (from `docs/PLAN.md`).
  - On top of that come the M5GFX line buffers, FATFS/SD buffers, the entry queue, task stacks and the parsed rules.
  - Use cardputer_ai's measured free heap (model plus UI) as the baseline, and print high-water marks over serial.
  - WiFi is turned on only for NTP or export and fully shut down afterwards, so its stack doesn't compete with inference.
- **Clock:**
  - I believe neither Cardputer model has a battery-backed RTC. Verify this.
  - The clock is set by NTP or by hand, and the last-known time is saved in NVS.
  - Reminder beeps work only while the device is awake (deep-sleep wake timer as a stretch goal).

## 6. Project folder (first step of implementation)
All Pocket Inbox work lives in a **new standalone folder, `/Users/rezor/Code/ESP32/pocket_inbox/`**, with its own `git init` on `main`. Nothing is added to `tinydecide/` or `cardputer_ai/`; both are only read from or copied out of.

```
pocket_inbox/
  README.md                 what the app is, how to install via M5Launcher, how to use it
  .gitignore                build/, .pio/, node_modules/, sdkconfig (generated)
  docs/PLAN.md              this plan
  model/                    copied from tinydecide/web_S768: model.bin, meta.json, tinydecide.js (+ refs for parity)
    SOURCE.md               source path, date and sha256 of each file
  sd/inbox/                 starter SD contents: rules.txt, settings.ini (copy to the card's /inbox/)
  host/                     M0 tools (Node, no deps)
    inbox_scenarios.jsonl   ~150 gold-labelled inbox lines
    inbox_eval.mjs          runs scenarios + sd/inbox/rules.txt through model/tinydecide.js
    rules.mjs               rules.txt parser (same grammar the firmware uses)
  firmware/                 ESP-IDF/PlatformIO project, skeleton from cardputer_ai
    platformio.ini, CMakeLists.txt, sdkconfig.cardputer, sdkconfig.cardputer-adv, sdkconfig.defaults, partitions.csv
    components/tinydecide/  tokenizer, encoder, heads, corrections (reusable engine)
    main/                   main.cpp, app/ (capture, card, today, lists, review, rules, settings), store/ (SD writers), keyboard/, ui.cpp
```
- **Kept from the `cardputer_ai` skeleton:** build config, `partitions.csv` (M5Launcher-compatible), `keyboard/`, `ui.cpp`, and `dot_q4_pie.S` (which moves into the engine component).
- **Not kept:** the LLM files `llm.cpp`, `model_data.cpp`, `tok_data.cpp` and `trn2*`.
- **Commit:** a single "Project skeleton" commit once the folder is created and the host eval runs.

## 7. Milestones
**M0 — Host feasibility (no firmware, ~1–2 days). This is the go/no-go gate.**
- Write `host/inbox_scenarios.jsonl`: about 150 realistic inbox lines with gold labels, covering every category, messy typing, no-time and no-amount cases, and lines that fit nothing.
- Script `host/inbox_eval.mjs` runs them through `model/tinydecide.js` with the exact `sd/inbox/rules.txt`. It reports per-question accuracy, confidence calibration, the chosen threshold and token count per pass.
- Tune question wording and category names.
- **Exit:** routing ≥85%, time/date/amount span ≥80%, pass 1 ≤ about 100 tokens. Below that, rework the categories, for example by merging todo and event.

**M1 — Device engine** (in the `tinydecide` component):
- WordPiece tokenizer using the 16k vocabulary and strings from `web_S768/meta.json`/`model.bin`.
- Bidirectional encoder with a batched Q4 GEMM built from `cardputer_ai/main/dot_q4_pie.S`, plus the streamed kernel (planned in `docs/PLAN.md`).
- The choice, noul, score and span heads, and a pass packer.
- **Exit:**
  - Results match the `web_S768` refs, using the same checks as the JS export (token ids, max|dp|).
  - Latency is measured on the device and logged in `docs/PLAN.md`.
  - Target ≤1 s for pass 1. If it's slower, fall back to the 6-layer or "compiled question" options already set out in `docs/PLAN.md`.

**M2 — Capture loop:** capture screen, inference queue, result card, rules parser, `log.jsonl` + Markdown/CSV writers, clock and date resolution.

**M3 — Organise:** Today, Lists, search, expense totals, done and snooze.

**M4 — Learn:** corrections port, Review queue, confidence threshold setting.

**M5 — Polish and release:** Rules screen and line editor, settings, WiFi NTP and export (a small HTTP page to download `/inbox` as files), battery and sleep, README with a demo GIF, M5Launcher release.

**Stretch — "Ask your inbox":**
- The user types a query such as `what did I spend on food this week`.
- The model routes it into intent (list/sum/find), category and time range.
- C++ runs it over `log.jsonl`.

## 8. Verification
- **M0:** run `node host/inbox_eval.mjs (from pocket_inbox/)` and check the accuracy and calibration report against the exit thresholds.
- **M1:** on the device, the parity test over the `web_S768` ref set must give 0 token-id mismatches and a max|dp| within the export tolerance. Latency and SRAM high-water marks are printed over serial.
- **M2–M4:**
  - A scripted run on the device replays the M0 scenario file from SD as though each line were typed. Then compare the resulting `/inbox` files to the expected output.
  - Correction test: correct one category, then check that a similar unseen line is routed the corrected way.
  - Pull-the-SD test: the files open cleanly in Obsidian (the Tasks plugin shows due dates and priorities) and in a spreadsheet.
- **Endurance:** 500 entries in a row with no heap growth, plus a power-cut mid-write that loses at most the one pending entry.

## Open risks
- Latency and SRAM are still estimates until M1.
- Date spans in messy text ("next tue", "in 2 days") may need a C++ fallback using simple patterns.
- Rare words such as names are a known weak spot for the 16k vocabulary. The person span may need user correction more often.

## Status log
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
