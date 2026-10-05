// M0 feasibility: run inbox scenarios through the device model (JS engine, same weights)
// with the exact rules.txt, two passes as on the device.
//   node host/inbox_eval.mjs [--rules sd/inbox/rules.txt] [--scenarios host/inbox_scenarios.jsonl] [--verbose]
//
// Scenario line: {"text": "...", "labels": {"route": "todo", "time": "6pm", "phone": null, ...}, "nofit": false}
//   choice/score label = option name; noul = true/false; span = exact substring, or null = absent.
//   Keys not listed are not scored. "nofit": true = fits no category; scored on low route confidence.
import { readFileSync } from "node:fs";
import { TinyDecide } from "../model/tinydecide.js";
import { loadRules, pass1, pass2 } from "./rules.mjs";

const root = new URL("..", import.meta.url).pathname;
const argv = process.argv.slice(2);
const opt = (k, d) => { const i = argv.indexOf(k); return i >= 0 ? argv[i + 1] : d; };
const verbose = argv.includes("--verbose");
const rules = loadRules(opt("--rules", root + "sd/inbox/rules.txt"));
const scenarios = readFileSync(opt("--scenarios", root + "host/inbox_scenarios.jsonl"), "utf8")
  .split("\n").filter((l) => l.trim()).map((l) => JSON.parse(l));
const settings = Object.fromEntries(readFileSync(root + "sd/inbox/settings.ini", "utf8").split("\n")
  .map((l) => l.match(/^\s*([a-z_]+)\s*=\s*(.*?)\s*$/)).filter(Boolean).map((m) => [m[1], m[2]]));
const THRESH = Number(settings.confidence_threshold ?? 0.35);
const SPAN_MIN = Number(settings.span_min_present ?? 0.5);

const bin = readFileSync(root + "model/model.bin");
const model = new TinyDecide(JSON.parse(readFileSync(root + "model/meta.json", "utf8")),
  bin.buffer.slice(bin.byteOffset, bin.byteOffset + bin.byteLength));

const norm = (s) => String(s ?? "").toLowerCase().replace(/^[\s.,;:!?'"()]+|[\s.,;:!?'"()]+$/g, "").replace(/\s+/g, " ");

// Answer -> value the app would store.
function value(q, a) {
  if (q.type === "choice" || q.type === "score") return { v: q.options[a.pick], conf: a.confidence, top: Math.max(...a.probs) };
  if (q.type === "noul") return { v: a.p >= 0.5, conf: Math.abs(a.p - 0.5) * 2 };
  return { v: a.p_present >= SPAN_MIN && a.text ? a.text : null, conf: a.p_present };
}

function score(q, got, gold) {
  if (q.type === "span") {
    if (gold === null) return { ok: got === null, lenient: got === null };
    if (got === null) return { ok: false, lenient: false };
    const g = norm(got), t = norm(gold);
    return { ok: g === t, lenient: g === t || g.includes(t) || t.includes(g) };
  }
  const ok = q.type === "noul" ? got === gold : norm(got) === norm(gold);
  return { ok, lenient: ok };
}

const stats = {};           // key -> {n, ok, lenient}
const routeConf = [];       // {conf, top, ok}
const nofit = [];           // {conf, top} on nofit lines
const routed = {};          // pass-2 keys scored only on correctly routed lines: key -> {n, ok}
const confusion = {};
const tok = { p1: [], p2: [] }, ms = { p1: [], p2: [] };
const misses = [];

for (const sc of scenarios) {
  const q1 = pass1(rules);
  const r1 = model.answer({ state: sc.text, questions: q1 });
  tok.p1.push(r1.tokens.total); ms.p1.push(r1.ms);
  const answers = q1.map((q, i) => [q, value(q, r1.answers[i])]);
  const route = answers[0][1];
  const q2 = pass2(rules, route.v);
  if (q2.length) {
    const r2 = model.answer({ state: sc.text, questions: q2 });
    tok.p2.push(r2.tokens.total); ms.p2.push(r2.ms);
    q2.forEach((q, i) => answers.push([q, value(q, r2.answers[i])]));
  }
  if (sc.nofit) { nofit.push({ conf: route.conf, top: route.top }); continue; }
  const labels = sc.labels || {};
  if ("route" in labels) {
    const ok = norm(route.v) === norm(labels.route);
    routeConf.push({ conf: route.conf, top: route.top, ok });
    const row = (confusion[labels.route] = confusion[labels.route] || {});
    row[route.v] = (row[route.v] || 0) + 1;
  }
  const byKey = Object.fromEntries(answers.map(([q, a]) => [q.key, [q, a]]));
  for (const [key, gold] of Object.entries(labels)) {
    const s = (stats[key] = stats[key] || { n: 0, ok: 0, lenient: 0 });
    s.n++;
    let res;
    if (byKey[key]) res = score(byKey[key][0], byKey[key][1].v, gold);
    else res = { ok: false, lenient: false };   // question not asked: routed to a category without it
    s.ok += res.ok; s.lenient += res.lenient;
    const routeOk = "route" in labels && norm(route.v) === norm(labels.route);
    if (key !== "route" && routeOk && !pass1(rules).some((q) => q.key === key)) {
      const r = (routed[key] = routed[key] || { n: 0, ok: 0 }); r.n++; r.ok += res.ok;
    }
    if (!res.ok) misses.push(`${key.padEnd(9)} want ${JSON.stringify(gold)}  got ${byKey[key] ? JSON.stringify(byKey[key][1].v) : "(not asked)"}  | ${sc.text}`);
  }
}

const pct = (a, b) => (b ? (100 * a / b).toFixed(1) + "%" : "-");
const avg = (a) => (a.length ? a.reduce((x, y) => x + y, 0) / a.length : 0);
const max = (a) => (a.length ? Math.max(...a) : 0);

console.log(`scenarios ${scenarios.length} (${nofit.length} nofit)   rules: ${rules.categories.length} categories, pass 1 = ${pass1(rules).length} questions\n`);
console.log("key        n   exact    lenient   given correct route (pass-2 keys)");
for (const [k, s] of Object.entries(stats)) {
  const r = routed[k];
  console.log(`${k.padEnd(9)} ${String(s.n).padStart(3)}   ${pct(s.ok, s.n).padStart(6)}   ${pct(s.lenient, s.n).padStart(6)}` + (r ? `    ${pct(r.ok, r.n).padStart(6)} of ${r.n}` : ""));
}

console.log("\nroute confusion (gold -> predicted):");
for (const [g, row] of Object.entries(confusion))
  console.log(`  ${g.padEnd(9)} ${Object.entries(row).sort((a, b) => b[1] - a[1]).map(([p, n]) => `${p}:${n}`).join("  ")}`);

for (const [field, label] of [["conf", "confidence (1 - normalized entropy)"], ["top", "top probability"]]) {
  console.log(`\nroute threshold sweep on ${label} (coverage = share at or above threshold):`);
  for (const t of field === "conf" ? [0.1, 0.15, 0.2, 0.25, 0.3, 0.35, 0.4] : [0.2, 0.3, 0.4, 0.5, 0.6, 0.7]) {
    const kept = routeConf.filter((r) => r[field] >= t);
    const nf = nofit.filter((c) => c[field] < t).length;
    console.log(`  ${t.toFixed(2)}${field === "conf" && t === THRESH ? "*" : " "} coverage ${pct(kept.length, routeConf.length).padStart(6)}  accuracy ${pct(kept.filter((r) => r.ok).length, kept.length).padStart(6)}  nofit flagged ${nf}/${nofit.length}`);
  }
  const m = (xs) => avg(xs.map((r) => r[field])).toFixed(2);
  console.log(`  mean: correct ${m(routeConf.filter((r) => r.ok))}  wrong ${m(routeConf.filter((r) => !r.ok))}  nofit ${m(nofit)}`);
}

console.log(`\ntokens per pass: pass 1 avg ${avg(tok.p1).toFixed(0)} max ${max(tok.p1)}   pass 2 avg ${avg(tok.p2).toFixed(0)} max ${max(tok.p2)} (${tok.p2.length} entries)`);
console.log(`node ms per pass: pass 1 ${avg(ms.p1).toFixed(0)}   pass 2 ${avg(ms.p2).toFixed(0)}`);

// M0 exit criteria (docs/PLAN.md section 7)
const acc = (k) => (stats[k] ? stats[k].ok / stats[k].n : null);
const checks = [
  ["route >= 85%", acc("route"), (v) => v >= 0.85],
  ["time >= 80%", acc("time"), (v) => v >= 0.8],
  ["day >= 80%", acc("day"), (v) => v >= 0.8],
  ["amount >= 80%", acc("amount"), (v) => v >= 0.8],
  ["pass 1 <= 100 tokens", max(tok.p1), (v) => v <= 100],
];
console.log("\nM0 exit:");
let pass = true;
for (const [name, v, f] of checks) {
  const ok = v !== null && f(v); pass &&= ok;
  console.log(`  ${ok ? "PASS" : "FAIL"}  ${name.padEnd(22)} ${v === null ? "(no labels)" : v <= 1 ? (100 * v).toFixed(1) + "%" : v}`);
}
if (verbose && misses.length) console.log("\nmisses:\n  " + misses.join("\n  "));
else if (misses.length) console.log(`\n${misses.length} misses (--verbose to list)`);
process.exit(pass ? 0 : 1);
