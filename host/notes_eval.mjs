// Category routing for Pocket Notes: zero-shot and after learning from k filed notes per category.
//   node host/notes_eval.mjs [--scenarios host/notes_scenarios.jsonl] [--verbose] [--no-rules]
//
// Scenario line: {"text": "...", "cat": "<category id>"}. Each variant below is one question wording plus
// the option text the model sees for each category (the `hint` column of categories.txt).
import { readFileSync } from "node:fs";
import { TinyDecide } from "../model/tinydecide.js";
import { protosFor } from "./learn.mjs";

const root = new URL("..", import.meta.url).pathname;
const argv = process.argv.slice(2);
const opt = (k, d) => { const i = argv.indexOf(k); return i >= 0 ? argv[i + 1] : d; };
const verbose = argv.includes("--verbose");
const scenarios = readFileSync(opt("--scenarios", root + "host/notes_scenarios.jsonl"), "utf8")
  .split("\n").filter((l) => l.trim()).map((l) => JSON.parse(l));

const meta = JSON.parse(readFileSync(root + "model/meta.json", "utf8"));
const bin = readFileSync(root + "model/model.bin");
const model = new TinyDecide(meta, bin.buffer.slice(bin.byteOffset, bin.byteOffset + bin.byteLength));

const CATS = ["todo", "shopping", "ideas", "events", "contacts", "notes"];
// The shipped wording (sd/notes/categories.txt hints) first, then the alternatives it beat.
const VARIANTS = [
  { q: "What kind of note is this?", o: ["a task to do", "something to buy", "an idea", "an appointment", "a phone number or email", "something to remember"] },
  { q: "What kind of note is this?", o: ["todo", "shopping", "ideas", "events", "contacts", "notes"] },
  { q: "What kind of note is this?", o: ["task to do", "shopping list", "idea", "event or appointment", "contact details", "other note"] },
];
// The firmware's text rules (firmware/main/ai.cpp rulesBias): logit bonuses on the z0 scale.
const PHONE_CAT = 4, TIME_CAT = 3;
function rulesBias(t) {
  const b = CATS.map(() => 0), s = t.toLowerCase();
  if (/[^\s@]@[^\s@.][^\s@]*\./.test(s) || (s.match(/\d/g) || []).length >= 7) b[PHONE_CAT] += 3;
  if (/(^|[^\d:])\d{1,2}(:[0-5]\d(?!\d)| ?[ap]m(?![a-z]))/.test(s)) b[TIME_CAT] += 1.5;
  return b;
}
const rules = !argv.includes("--no-rules");
const KS = [0, 2, 5, 8];

for (const V of VARIANTS) {
  const question = { type: "choice", text: V.q, options: V.o };
  // One pass per note gives both its zero-shot answer and its example vector (qvec, z0).
  const emb = scenarios.map((s) => {
    const a = model.answer({ state: s.text, questions: [question] }).answers[0];
    const bias = rules ? rulesBias(s.text) : CATS.map(() => 0);
    return { v: a.qvec, z: a.z0.map((z, i) => z + bias[i]), gold: CATS.indexOf(s.cat), text: s.text };
  });
  const toks = scenarios.map((s) => model.tok.encode(s.text).ids.length + model.tok.encode(V.q).ids.length +
    V.o.reduce((a, o) => a + model.tok.encode(o).ids.length + 1, 0) + 4);
  const line = [];
  for (const k of KS) {
    let ok = 0;
    const miss = [];
    emb.forEach((e, i) => {
      let z = e.z;
      if (k > 0) {
        // The k most recent other notes of each category (scenario order stands in for time).
        const lists = CATS.map((_, c) => emb.filter((x, j) => j !== i && x.gold === c).slice(-k));
        const P = protosFor(lists, meta.beta);
        z = applyProtos(e, P, meta);
      }
      const pick = z.indexOf(Math.max(...z));
      if (pick === e.gold) ok++; else miss.push(`${CATS[e.gold]}->${CATS[pick]}  ${e.text}`);
    });
    line.push(`k=${k} ${(100 * ok / emb.length).toFixed(0)}%`);
    if (verbose && (k === 0 || k === 8)) console.log(`  misses at k=${k}:\n    ` + miss.join("\n    "));
  }
  console.log(`${JSON.stringify(V.q)} | ${V.o.join(", ")}\n  ${line.join("   ")}   tokens avg ${(toks.reduce((a, b) => a + b, 0) / toks.length).toFixed(0)} max ${Math.max(...toks)}`);
}

// Logits with the prototype term, without another encoder pass: answer() computes
// softmax((raw + lam*t) / T) and z0 = raw / T, so the result is z0 + lam*t/T.
function applyProtos(e, P, meta) {
  const T = meta.temp[0];
  const bucket = (c) => [1, 2, 4, 8].reduce((b, x) => (c > x ? b + 1 : b), 0);
  return e.z.map((z0, i) => {
    if (!(P.cnt[i] > 0)) return z0;
    let d = 0, na = 0, nb = 0;
    for (let j = 0; j < 128; j++) {
      const a = e.v[j] - P.center[j], b = P.vec[i * 128 + j] - P.center[j];
      d += a * b; na += a * a; nb += b * b;
    }
    const t = meta.beta[bucket(P.cnt[i])] * d / (Math.sqrt(na) * Math.sqrt(nb) || 1e-12);
    return z0 + P.lam * t / T;
  });
}
