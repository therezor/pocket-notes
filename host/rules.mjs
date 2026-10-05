// rules.txt parser — the same grammar the firmware will use (see sd/inbox/rules.txt).
//   <type> <key>: <text> | <opt>, <opt>, ...
import { readFileSync } from "node:fs";

const TYPES = new Set(["choice", "score", "noul", "span"]);
// Category id = option text lowercased, non-alphanumerics -> "_" ("shopping list" -> shopping_list).
// It names the [section] and the saved file (/inbox/<id>.md).
export const catId = (o) => o.toLowerCase().replace(/[^a-z0-9]+/g, "_").replace(/^_|_$/g, "");
const LINE = /^(choice|score|noul|span)\s+([A-Za-z_][A-Za-z0-9_]*)\s*:\s*(.+)$/;

export function parseRules(src) {
  const sections = {};
  const errors = [];
  let cur = null;
  src.split(/\r?\n/).forEach((raw, i) => {
    const line = raw.trim(), at = `line ${i + 1}`;
    if (!line || line.startsWith("#")) return;
    const sec = line.match(/^\[([A-Za-z_][A-Za-z0-9_]*)\]$/);
    if (sec) { cur = sec[1].toLowerCase(); sections[cur] = sections[cur] || []; return; }
    if (!cur) { errors.push(`${at}: question before any [section]`); return; }
    const m = line.match(LINE);
    if (!m) { errors.push(`${at}: expected "<type> <key>: <text> | <options>"`); return; }
    const [, type, key, rest] = m;
    const bar = rest.indexOf("|");
    const text = (bar < 0 ? rest : rest.slice(0, bar)).trim();
    const options = bar < 0 ? [] : rest.slice(bar + 1).split(",").map((o) => o.trim()).filter(Boolean);
    if (!TYPES.has(type)) errors.push(`${at}: unknown type ${type}`);
    if ((type === "choice" || type === "score") && (options.length < 2 || options.length > 32))
      errors.push(`${at}: ${type} needs 2-32 options`);
    if ((type === "noul" || type === "span") && options.length) errors.push(`${at}: ${type} takes no options`);
    sections[cur].push({ key, type, text, options });
  });

  const route = sections.route || [];
  if (route.length !== 1 || route[0].type !== "choice") errors.push("[route] must hold exactly one choice question");
  const cats = route[0] ? route[0].options.map(catId) : [];
  for (const s of Object.keys(sections))
    if (s !== "route" && s !== "all" && !cats.includes(s)) errors.push(`[${s}] is not a route option`);
  const keys = new Map();
  for (const [s, qs] of Object.entries(sections))
    for (const q of qs) {
      const prev = keys.get(q.key);
      // a key may repeat across category sections (e.g. "who"), never within the same pass
      if (prev && (prev === s || prev === "route" || prev === "all" || s === "route" || s === "all"))
        errors.push(`key "${q.key}" used twice in one pass ([${prev}] and [${s}])`);
      keys.set(q.key, s);
    }
  if (errors.length) throw new Error("rules.txt:\n  " + errors.join("\n  "));
  return { route: route[0], all: sections.all || [], categories: cats, sections };
}

export const loadRules = (path) => parseRules(readFileSync(path, "utf8"));

// Questions for pass 1 (route + all) and pass 2 (the routed category's own section).
export const pass1 = (rules) => [rules.route, ...rules.all];
export const pass2 = (rules, option) => rules.sections[catId(option)] || [];
