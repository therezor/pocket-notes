// Reference outputs of the JS engine for the C++ parity test (host/engine_test.cpp, device selftest).
//   node host/make_ref.mjs
// Writes host/engine_ref.txt, one case per line, tab-separated:
//   text  question  options(|)  state ids(,)  probs(,)  qvec[0..3](,)
import { readFileSync, writeFileSync } from "node:fs";
import { TinyDecide } from "../model/tinydecide.js";

const root = new URL("..", import.meta.url).pathname;
const meta = JSON.parse(readFileSync(root + "model/meta.json", "utf8"));
const bin = readFileSync(root + "model/model.bin");
const model = new TinyDecide(meta, bin.buffer.slice(bin.byteOffset, bin.byteOffset + bin.byteLength));

const Q = "What kind of note is this?";
const O = ["a task to do", "something to buy", "an idea", "an appointment", "a phone number or email", "something to remember"];
const lines = (f) => readFileSync(root + f, "utf8").split("\n").filter((l) => l.trim()).map((l) => JSON.parse(l).text);
const texts = [...lines("host/notes_scenarios.jsonl"), ...lines("host/inbox_lines.jsonl"),
  "", "!!!", "Hello,World...  42x  e-mail: A.B@c.io", "supercalifragilisticexpialidocious antidisestablishmentarianism",
  "a ".repeat(70) + "end"];
const out = [];
const seen = new Set();
for (const t of texts) {
  if (seen.has(t)) continue;
  seen.add(t);
  const r = model.answer({ state: t, questions: [{ type: "choice", text: Q, options: O }] });
  const a = r.answers[0];
  const st = model.tok.encode(t).ids;
  out.push([t, Q, O.join("|"), st.join(","), a.probs.map((p) => p.toFixed(6)).join(","),
    a.qvec.slice(0, 4).map((v) => v.toFixed(5)).join(",")].join("\t"));
}
writeFileSync(root + "host/engine_ref.txt", out.join("\n") + "\n");
console.log(`host/engine_ref.txt: ${out.length} cases`);
