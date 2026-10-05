# Pocket Inbox

Offline quick-capture for the M5Stack Cardputer (original and ADV), powered by the
[TinyDecide](../tinydecide) decision model running on the device.

Type one line in plain language and press Enter. About a second later it is filed:

| You type | It becomes |
|---|---|
| `call mom tomorrow 6pm` | **Todo**, due tomorrow 18:00 |
| `lunch 12.40 with Mark` | **Expense** 12.40 · food · with Mark |
| `Anna new number 555 0134` | **Contact** Anna · 555 0134 |
| `dentist oct 14 at 9:30` | **Event** Oct 14 09:30 |

No forms, no cloud, no phone. Entries land on the SD card as Markdown (Obsidian Tasks
style) and CSV. Categories and extracted fields are questions in `sd/inbox/rules.txt`
that you can edit; correcting a wrong category teaches the model without retraining.

## Status

Skeleton. Nothing is filed on the device yet. Milestones are listed in [docs/PLAN.md](docs/PLAN.md):

- **M0 (next)**: host feasibility on the PC, the go/no-go gate. A seed run already shows routing as the weak spot (see PLAN.md, Status log).
- **M1**: device engine (tokenizer, Q4 encoder, heads) in `firmware/components/tinydecide/`.
- **M2–M5**: capture loop, lists, corrections, release through M5Launcher.

## Layout

```
docs/PLAN.md        product + engineering plan, status log
model/              device model (TinyDecide S768) + JS reference engine; see model/SOURCE.md
sd/inbox/           starter SD contents: rules.txt, settings.ini (copy to the card's /inbox/)
host/               PC tools: rules parser, scenario set, eval
firmware/           ESP-IDF project (PlatformIO), skeleton taken from cardputer_ai
```

## Host tools (Node 18+, no dependencies)

```sh
node model/verify.mjs          # JS engine parity vs the reference (must be 0 mismatches)
node host/inbox_eval.mjs       # run host/inbox_scenarios.jsonl through rules.txt, two passes
node host/inbox_eval.mjs --verbose --rules my_rules.txt --scenarios my.jsonl
```

## Firmware

```sh
cd firmware && pio run -t upload
```

Not built yet. In the current dev environment `pio run` fails with a PlatformIO toolchain error
(`No module named 'SCons.Tool.FortranCommon'`); see the Status log in docs/PLAN.md.

One image runs on both the original Cardputer and the ADV. The partition table gives one
8.32 MB factory app (code plus embedded model) and installs cleanly through M5Launcher.

## License

MIT
