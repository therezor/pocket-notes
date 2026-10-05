# Model source

Copied from `/Users/rezor/Code/ESP32/tinydecide/web_S768/` (device build v2, S768: 12 layers, FFN 768, 16k vocab, Q4 matrices + embeddings, int8 heads) on 2026-10-05.

Parity check: `node model/verify.mjs` — must print 0 token-id / pick / span mismatches.

| file | bytes | sha256 |
|---|---|---|
| `model.bin` | 6181984 | `23fe9d93f2eafadbe4dc686a7dc5f309fbcba9ea1a4873d457e75d17c5bf3441` |
| `meta.json` | 187362 | `2fb895cfb0cdd6b11fcfb5573033e846bf4defadb3c78b9989cccfeefe06758f` |
| `tinydecide.js` | 19419 | `592e3dd7c09804f3ef1de1bc66f8e6419ae058578bd8b2746a5f332d57f5ca2d` |
| `ref.json` | 146237 | `646646c2216b0ad675630d584d3eeb51eec84224b6c1e5cf7c067275806665d6` |
| `heldout_int8.json` | 16282 | `65d3c51cfce12ed72d0114652fd5214aa1b89f16a2146d446eed16f0ed86b9c6` |
| `verify.mjs` | 1830 | `fe64b7ea334e896f64fbf9408751ffc92c4cfdc1c74481becf2d58e419b4e685` |

`vocab.bin` is generated from `meta.json` by `node host/pack_model.mjs` (with `firmware/components/tinydecide/td_meta.h`) and embedded in the firmware next to `model.bin`.
