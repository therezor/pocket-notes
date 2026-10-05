// Parity: JS engine vs torch (same int8 weights). node web/verify.mjs
import { readFileSync } from "node:fs";
import { TinyDecide } from "./tinydecide.js";
const dir = new URL(".", import.meta.url).pathname;
const meta = JSON.parse(readFileSync(dir + "meta.json", "utf8"));
const bin = readFileSync(dir + "model.bin");
const buf = bin.buffer.slice(bin.byteOffset, bin.byteOffset + bin.byteLength);
const ref = JSON.parse(readFileSync(dir + "ref.json", "utf8"));
const m = new TinyDecide(meta, buf);
let idBad = 0, maxP = 0, spanBad = 0, pickBad = 0, n = 0, ms = 0;
for (const r of ref) {
  const got = m.answer({ state: r.state, questions: r.questions });
  ms += got.ms;
  if (JSON.stringify(got.ids) !== JSON.stringify(r.ids)) {
    idBad++;
    if (idBad <= 3) console.log("ids differ:", JSON.stringify(r.state).slice(0, 80), "\n torch", r.ids.slice(0, 30).join(","), "\n js   ", got.ids.slice(0, 30).join(","));
    continue;
  }
  r.answers.forEach((a, i) => {
    const b = got.answers[i]; n++;
    if (a.type === "choice" || a.type === "score") {
      a.probs.forEach((p, j) => { maxP = Math.max(maxP, Math.abs(p - b.probs[j])); });
      if (a.pick !== b.pick) pickBad++;
    } else if (a.type === "noul") maxP = Math.max(maxP, Math.abs(a.p - b.p));
    else {
      maxP = Math.max(maxP, Math.abs(a.p_present - b.p_present));
      if ((a.text || "") !== (b.text || "") || a.tok[0] !== b.tok[0] || a.tok[1] !== b.tok[1]) { spanBad++; if (spanBad <= 3) console.log("span differs", a.text, "|", b.text, a.tok, b.tok); }
    }
  });
}
console.log(`requests ${ref.length}  token-id mismatches ${idBad}  answers ${n}  max|dp| ${maxP.toExponential(2)}  pick mismatches ${pickBad}  span mismatches ${spanBad}  avg ${(ms / ref.length).toFixed(0)} ms/request`);
process.exit(idBad || maxP > 1e-3 || spanBad || pickBad ? 1 : 0);
