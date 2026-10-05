// TinyDecide decision model in plain JavaScript (browser worker or Node). Mirrors the Python
// reference exactly: reflex/format.py (layout + masks), reflex/backbones.py (ELECTRA path),
// reflex/model.py (native path), reflex/heads.py (typed heads, temperatures, prototypes).
// verify.mjs checks it against torch outputs computed from the same int8 weights.

const K_STATE = 0, K_QTEXT = 1, K_ANS = 2, K_OPT = 3, K_LV = 4;
const TYPES = ["choice", "noul", "score", "span"];
const K_BUCKETS = [1, 2, 4, 8];

// ------------------------------------------------------------------ tokenizers
const RE_CTRL = /[\p{Cc}\p{Cf}]/u, RE_ZS = /\p{Zs}/u, RE_P = /\p{P}/u, RE_MN = /\p{Mn}/u;
function isCJK(cp) {
  return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x20000 && cp <= 0x2A6DF) ||
    (cp >= 0x2A700 && cp <= 0x2B73F) || (cp >= 0x2B740 && cp <= 0x2B81F) || (cp >= 0x2B820 && cp <= 0x2CEAF) ||
    (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0x2F800 && cp <= 0x2FA1F);
}
function isPunct(ch) {
  const c = ch.codePointAt(0);
  if ((c >= 33 && c <= 47) || (c >= 58 && c <= 64) || (c >= 91 && c <= 96) || (c >= 123 && c <= 126)) return true;
  return RE_P.test(ch);
}

export class WordPiece {
  constructor(m) {
    this.vocab = new Map();
    m.vocab.forEach((p, i) => { if (p !== null) this.vocab.set(p, i); });
    this.unk = this.vocab.get(m.unk);
    this.prefix = m.prefix; this.maxChars = m.max_chars;
  }
  // -> {ids, offsets} with offsets in JS string (UTF-16) indices of the original text
  encode(text) {
    const chars = [];                       // normalised chars, each tagged with its source range
    let u = 0;
    for (const ch of text) {
      const cp = ch.codePointAt(0), a = u, b = u + ch.length;
      u = b;
      if (cp === 0 || cp === 0xFFFD || (RE_CTRL.test(ch) && ch !== "\t" && ch !== "\n" && ch !== "\r")) continue;
      if (ch === " " || ch === "\t" || ch === "\n" || ch === "\r" || RE_ZS.test(ch)) { chars.push([" ", a, b]); continue; }
      if (isCJK(cp)) { chars.push([" ", a, a]); chars.push([ch, a, b]); chars.push([" ", b, b]); continue; }
      for (const d of ch.normalize("NFD")) {
        if (RE_MN.test(d)) continue;
        for (const l of d.toLowerCase()) chars.push([l, a, b]);
      }
    }
    const words = [];                        // arrays of [ch, a, b]
    let cur = [];
    const flush = () => { if (cur.length) words.push(cur); cur = []; };
    for (const c of chars) {
      if (c[0] === " " || /\s/u.test(c[0])) { flush(); continue; }
      if (isPunct(c[0])) { flush(); words.push([c]); continue; }
      cur.push(c);
    }
    flush();
    const ids = [], offsets = [];
    for (const w of words) {
      if (w.length > this.maxChars) { ids.push(this.unk); offsets.push([w[0][1], w[w.length - 1][2]]); continue; }
      const pieces = [];
      let start = 0, bad = false;
      while (start < w.length) {
        let end = w.length, got = -1;
        while (start < end) {
          let s = "";
          for (let i = start; i < end; i++) s += w[i][0];
          if (start > 0) s = this.prefix + s;
          const id = this.vocab.get(s);
          if (id !== undefined) { got = id; break; }
          end--;
        }
        if (got < 0) { bad = true; break; }
        pieces.push([got, w[start][1], w[end - 1][2]]);
        start = end;
      }
      if (bad) { ids.push(this.unk); offsets.push([w[0][1], w[w.length - 1][2]]); }
      else for (const [id, a, b] of pieces) { ids.push(id); offsets.push([a, b]); }
    }
    return { ids, offsets };
  }
}

// ------------------------------------------------------------------ weights
export class Weights {
  constructor(meta, buf) {
    this.t = {};
    for (const e of meta.tensors) {
      const n = e.shape.reduce((a, b) => a * b, 1);
      let arr;
      if (e.dtype === "f32") arr = new Float32Array(buf, e.offset, n).slice();
      else if (e.dtype === "q4") {                     // device Q4_0: 16 nibble bytes + one bf16 scale per 32
        const rows = e.shape[0], cols = e.shape[1], nb = cols / 32;
        const nib = new Uint8Array(buf, e.offset, rows * nb * 16), sc = new Uint16Array(buf, e.scale_offset, rows * nb);
        const f = new Float32Array(1), u = new Uint32Array(f.buffer);
        arr = new Float32Array(n);
        for (let r = 0; r < rows; r++) for (let b = 0; b < nb; b++) {
          u[0] = sc[r * nb + b] << 16; const d = f[0];
          const no = (r * nb + b) * 16, wo = r * cols + b * 32;
          for (let k = 0; k < 16; k++) { const byte = nib[no + k]; arr[wo + k] = ((byte & 15) - 8) * d; arr[wo + k + 16] = ((byte >> 4) - 8) * d; }
        }
      } else {
        const q = new Int8Array(buf, e.offset, n), s = new Float32Array(buf, e.scale_offset, e.shape[0]);
        const cols = e.shape[1];
        arr = new Float32Array(n);
        for (let r = 0; r < e.shape[0]; r++) { const sc = s[r], o = r * cols; for (let c = 0; c < cols; c++) arr[o + c] = q[o + c] * sc; }
      }
      this.t[e.name] = { data: arr, shape: e.shape };
    }
  }
  get(n) { const x = this.t[n]; if (!x) throw new Error("missing tensor " + n); return x; }
  has(n) { return n in this.t; }
}

// y[t] = W x[t] + b ; W [out, in] row-major, x [T, in]
function linear(x, T, din, W, b, dout) {
  const y = new Float32Array(T * dout);
  for (let t = 0; t < T; t++) {
    const xo = t * din, yo = t * dout;
    for (let j = 0; j < dout; j++) {
      const wo = j * din;
      let s = b ? b[j] : 0;
      for (let i = 0; i < din; i++) s += W[wo + i] * x[xo + i];
      y[yo + j] = s;
    }
  }
  return y;
}
function layernorm(x, T, d, w, b, eps) {
  const y = new Float32Array(T * d);
  for (let t = 0; t < T; t++) {
    const o = t * d;
    let m = 0; for (let i = 0; i < d; i++) m += x[o + i]; m /= d;
    let v = 0; for (let i = 0; i < d; i++) { const z = x[o + i] - m; v += z * z; } v /= d;
    const r = 1 / Math.sqrt(v + eps);
    for (let i = 0; i < d; i++) y[o + i] = (x[o + i] - m) * r * w[i] + (b ? b[i] : 0);
  }
  return y;
}
function rmsnorm(x, T, d, w, eps = 1e-6) {
  const y = new Float32Array(T * d);
  for (let t = 0; t < T; t++) {
    const o = t * d;
    let s = 0; for (let i = 0; i < d; i++) s += x[o + i] * x[o + i];
    const r = 1 / Math.sqrt(s / d + eps);
    for (let i = 0; i < d; i++) y[o + i] = x[o + i] * r * w[i];
  }
  return y;
}
// erf, Abramowitz-Stegun 7.1.26 is 1.5e-7; use a higher-precision rational (W. J. Cody style via erfc series)
function erf(x) {
  const s = Math.sign(x), a = Math.abs(x);
  if (a < 0.5) {                                   // Taylor series, fast convergence near 0
    let sum = a, term = a, a2 = a * a;
    for (let n = 1; n < 12; n++) { term *= -a2 / n; sum += term / (2 * n + 1); }
    return s * 2 / Math.sqrt(Math.PI) * sum;
  }
  // continued fraction for erfc (Lentz), accurate to ~1e-12 for a >= 0.5
  const t = 1 / (1 + 0.5 * a);
  const y = t * Math.exp(-a * a - 1.26551223 + t * (1.00002368 + t * (0.37409196 + t * (0.09678418 + t * (-0.18628806 +
    t * (0.27886807 + t * (-1.13520398 + t * (1.48851587 + t * (-0.82215223 + t * 0.17087277)))))))));
  return s * (1 - y);
}
const gelu = (x) => 0.5 * x * (1 + erf(x / Math.SQRT2));
const geluTanh = (x) => 0.5 * x * (1 + Math.tanh(0.7978845608028654 * (x + 0.044715 * x * x * x)));

// ------------------------------------------------------------------ request layout
export function encodeRequest(tok, meta, req) {
  const sp = meta.specials, F = meta.format;
  const st = tok.encode(req.state);
  const n = Math.min(st.ids.length, F.ts_max - 1);
  const ids = [sp["<|state|>"], ...st.ids.slice(0, n)];
  const pos = ids.map((_, i) => i), blk = ids.map(() => 0), kind = ids.map(() => K_STATE);
  const stIdx = []; for (let i = 1; i < ids.length; i++) stIdx.push(i);
  const stOff = st.offsets.slice(0, n);
  const qs = [];
  req.questions.forEach((q, qi) => {
    const k = qi + 1, bIds = [sp["<|" + q.type + "|>"]], bKind = [K_QTEXT];
    const t = tok.encode(q.text).ids; bIds.push(...t); t.forEach(() => bKind.push(K_QTEXT));
    const optLocal = [];
    if (q.options && q.options.length) {
      bIds.push(sp["<|sep|>"]); bKind.push(K_QTEXT);
      const [mk, mkind] = q.type === "choice" ? ["<|o|>", K_OPT] : ["<|lv|>", K_LV];
      for (const o of q.options) {
        const oi = tok.encode(o).ids; bIds.push(...oi); oi.forEach(() => bKind.push(K_QTEXT));
        optLocal.push(bIds.length); bIds.push(sp[mk]); bKind.push(mkind);
      }
    }
    const ansLocal = bIds.length; bIds.push(sp["<|ans|>"]); bKind.push(K_ANS);
    if (bIds.length > F.q_max) throw new Error(`Question ${k} is too long (${bIds.length} tokens, max ${F.q_max}).`);
    const base = ids.length;
    bIds.forEach((id, j) => { ids.push(id); pos.push(F.p_q + j); blk.push(k); kind.push(bKind[j]); });
    qs.push({ type: q.type, ans: base + ansLocal, opt: optLocal.map((j) => base + j) });
  });
  return { ids, pos, blk, kind, stIdx, stOff, qs, truncated: st.ids.length > n };
}

function continues(kind, fusion) {
  if (fusion === "all") return kind.map(() => true);
  if (fusion === "markers") return kind.map((k) => k === K_STATE || k === K_ANS || k === K_OPT || k === K_LV);
  return kind.map((k) => k === K_STATE || k === K_ANS);
}

// ------------------------------------------------------------------ model
export class TinyDecide {
  constructor(meta, buf) {
    this.meta = meta; this.cfg = meta.cfg; this.W = new Weights(meta, buf);
    const tk = meta.tokenizer;
    if (tk.kind !== "wordpiece") throw new Error("this engine build ships the WordPiece tokenizer only");
    this.tok = new WordPiece(tk);
  }
  w(n) { return this.W.get(n).data; }

  // attention over allowed keys; q,k,v [T, d]; returns [T, d]
  attend(q, k, v, T, allowed, heads, scale) {
    const d = this.cfg.d, dh = d / heads, out = new Float32Array(T * d);
    const sc = new Float32Array(T);
    for (let i = 0; i < T; i++) {
      const keys = allowed[i];
      if (!keys) continue;
      for (let h = 0; h < heads; h++) {
        const qo = i * d + h * dh;
        let mx = -Infinity;
        for (let a = 0; a < keys.length; a++) {
          const ko = keys[a] * d + h * dh;
          let s = 0; for (let c = 0; c < dh; c++) s += q[qo + c] * k[ko + c];
          s *= scale; sc[a] = s; if (s > mx) mx = s;
        }
        let z = 0; for (let a = 0; a < keys.length; a++) { sc[a] = Math.exp(sc[a] - mx); z += sc[a]; }
        const oo = i * d + h * dh;
        for (let a = 0; a < keys.length; a++) {
          const p = sc[a] / z, vo = keys[a] * d + h * dh;
          for (let c = 0; c < dh; c++) out[oo + c] += p * v[vo + c];
        }
      }
    }
    return out;
  }

  allowedSets(enc, cont, fusionLayer) {
    const T = enc.ids.length, blk = enc.blk;
    const byBlk = new Map();
    for (let j = 0; j < T; j++) {
      if (fusionLayer && !cont[j]) continue;
      if (!byBlk.has(blk[j])) byBlk.set(blk[j], []);
      byBlk.get(blk[j]).push(j);
    }
    const state = byBlk.get(0) || [];
    const sets = new Array(T);
    for (let i = 0; i < T; i++) {
      if (fusionLayer && !cont[i]) { sets[i] = [i]; continue; }
      const own = byBlk.get(blk[i]) || [i];
      sets[i] = (!fusionLayer || blk[i] === 0) ? own : state.concat(own);
    }
    return sets;
  }

  hidden(enc) {
    const cfg = this.cfg, T = enc.ids.length, d = cfg.d, L_a = cfg.L_a || 0;
    const cont = continues(enc.kind, cfg.fusion);
    const setsC = this.allowedSets(enc, cont, false), setsF = this.allowedSets(enc, cont, true);
    let x;
    if (cfg.arch === "electra") {
      const e = cfg.emb_dim, P = this.w("m.posemb.weight"), t0 = this.w("m.type0");
      const raw = new Float32Array(T * e);
      if (this.W.has("m.word.a.weight")) {            // low-rank table: a[id] (rank r) times b (r -> e)
        const A = this.w("m.word.a.weight"), B = this.w("m.word.b.weight"), r = this.W.get("m.word.a.weight").shape[1];
        for (let t = 0; t < T; t++) for (let c = 0; c < e; c++) {
          let s = 0; const ao = enc.ids[t] * r, bo = c * r;
          for (let j = 0; j < r; j++) s += A[ao + j] * B[bo + j];
          raw[t * e + c] = s + P[enc.pos[t] * e + c] + t0[c];
        }
      } else {
        const W = this.w("m.word.weight");
        for (let t = 0; t < T; t++) for (let c = 0; c < e; c++) raw[t * e + c] = W[enc.ids[t] * e + c] + P[enc.pos[t] * e + c] + t0[c];
      }
      const n = layernorm(raw, T, e, this.w("m.eln.weight"), this.w("m.eln.bias"), cfg.ln_eps);
      x = linear(n, T, e, this.w("m.proj.weight"), this.w("m.proj.bias"), d);
      for (let l = 0; l < cfg.layers; l++) {
        const p = `m.blocks.${l}.`, sets = l < L_a ? setsC : setsF;
        const q = linear(x, T, d, this.w(p + "q.weight"), this.w(p + "q.bias"), d);
        const k = linear(x, T, d, this.w(p + "k.weight"), this.w(p + "k.bias"), d);
        const v = linear(x, T, d, this.w(p + "v.weight"), this.w(p + "v.bias"), d);
        const y = this.attend(q, k, v, T, sets, cfg.heads, 1 / Math.sqrt(d / cfg.heads));
        const o = linear(y, T, d, this.w(p + "o.weight"), this.w(p + "o.bias"), d);
        for (let i = 0; i < T * d; i++) o[i] += x[i];
        x = layernorm(o, T, d, this.w(p + "ln1.weight"), this.w(p + "ln1.bias"), cfg.ln_eps);
        const ffDim = this.W.get(p + "fc.weight").shape[0];
        const f = linear(x, T, d, this.w(p + "fc.weight"), this.w(p + "fc.bias"), ffDim);
        for (let i = 0; i < f.length; i++) f[i] = gelu(f[i]);
        const f2 = linear(f, T, ffDim, this.w(p + "fc2.weight"), this.w(p + "fc2.bias"), d);
        for (let i = 0; i < T * d; i++) f2[i] += x[i];
        x = layernorm(f2, T, d, this.w(p + "ln2.weight"), this.w(p + "ln2.bias"), cfg.ln_eps);
      }
      return x;
    }
    throw new Error("unsupported architecture " + cfg.arch);
  }

  // protos: per question {vec: Float32Array[K*dh] | null, cnt: Int32Array[K]} or undefined
  answer(req, protos) {
    const t0 = (typeof performance !== "undefined" ? performance.now() : Date.now());
    const enc = encodeRequest(this.tok, this.meta, req);
    const x = this.hidden(enc);
    const cfg = this.cfg, d = cfg.d, dh = cfg.dh_head, T = enc.ids.length;
    const hq = layernorm(x, T, d, this.w("h.norm.weight"), this.w("h.norm.bias"), 1e-5);
    const row = (i) => hq.subarray(i * d, (i + 1) * d);
    const mv = (name, v, bias) => linear(v, 1, d, this.w(name), bias ? this.w(bias) : null, this.W.get(name).shape[0]);
    const temp = this.meta.temp, beta = this.meta.beta, scale = this.w("h.scale");
    const dot = (a, b) => { let s = 0; for (let i = 0; i < a.length; i++) s += a[i] * b[i]; return s; };
    const norm = (a) => { const n = Math.sqrt(dot(a, a)) || 1e-12; return a.map((z) => z / n); };
    const bucket = (c) => K_BUCKETS.reduce((b, e) => (c > e ? b + 1 : b), 0);
    const out = [];
    enc.qs.forEach((q, qi) => {
      const hAns = row(q.ans), P = protos && protos[qi];
      const hasP = this.W.has("h.P.weight");
      const pv = hasP ? mv("h.P.weight", hAns) : null;
      // centred-cosine prototype term: beta(k) * cos(p - c, mean_k - c)
      const protoTerm = (i) => {
        if (!P || !(P.cnt[i] > 0)) return 0;
        if (hasP && P.center) {
          const c = P.center, a = new Float32Array(dh), b = new Float32Array(dh);
          for (let j = 0; j < dh; j++) { a[j] = pv[j] - c[j]; b[j] = P.vec[i * dh + j] - c[j]; }
          const na = Math.sqrt(dot(a, a)) || 1e-12, nb = Math.sqrt(dot(b, b)) || 1e-12;
          return beta[bucket(P.cnt[i])] * dot(a, b) / (na * nb);
        }
        return null;                                   // older model without a prototype projection
      };
      if (q.type === "choice" || q.type === "score") {
        const sel = q.type === "score" ? 1 : 0;
        const qa = mv(`h.A.${sel}.weight`, hAns);
        const s = Math.exp(scale[sel]);
        const qv = norm(Array.from(qa));
        const lam = P && P.lam !== undefined ? P.lam : 1;
        const zeroShot = [];
        const logits = q.opt.map((oi, i) => {
          const ov = mv(`h.O.${sel}.weight`, row(oi));
          let z = s * dot(qa, ov) / Math.sqrt(dh);
          zeroShot.push(z / temp[sel === 1 ? 2 : 0]);
          const t = protoTerm(i);
          if (t === null) z += (P && P.cnt[i] > 0) ? beta[bucket(P.cnt[i])] * dot(qv, norm(Array.from(P.vec.subarray(i * dh, (i + 1) * dh)))) : 0;
          else z += lam * t;
          return z;
        });
        const Tt = temp[sel === 1 ? 2 : 0];
        const m = Math.max(...logits), ex = logits.map((z) => Math.exp((z - m) / Tt)), Z = ex.reduce((a, b) => a + b, 0);
        const probs = ex.map((e) => e / Z);
        const k = probs.length, H = -probs.reduce((a, p) => a + (p > 0 ? p * Math.log(p) : 0), 0);
        const r = { type: q.type, probs, pick: probs.indexOf(Math.max(...probs)), confidence: 1 - H / Math.log(k),
                    qvec: hasP ? Array.from(pv) : qv, proj: hasP, z0: zeroShot };
        if (q.type === "score") r.score = probs.reduce((a, p, i) => a + p * i / (k - 1), 0);
        out.push(r);
      } else if (q.type === "noul") {
        let z = dot(this.w("h.noul.weight"), hAns) + this.w("h.noul.bias")[0];
        const z0 = z / temp[1];
        const nv = norm(Array.from(mv("h.noul_q.weight", hAns)));
        if (P) {
          const lam = P.lam !== undefined ? P.lam : 1;
          const term = [0, 1].map((i) => {
            const t = protoTerm(i);
            if (t !== null) return lam * t;
            return P.cnt[i] > 0 ? beta[bucket(P.cnt[i])] * dot(nv, norm(Array.from(P.vec.subarray(i * dh, (i + 1) * dh)))) : 0;
          });
          z += term[1] - term[0];
        }
        out.push({ type: "noul", p: 1 / (1 + Math.exp(-z / temp[1])), qvec: hasP ? Array.from(pv) : nv, proj: hasP,
                   z0: [0, z0] });
      } else {
        const S = enc.stIdx.length;
        const qS = mv("h.sq.weight", hAns);
        const ks = enc.stIdx.map((i) => mv("h.sk.weight", row(i)));
        const start = [dot(this.w("h.snull.weight"), hAns) + this.w("h.snull.bias")[0], ...ks.map((kk) => dot(qS, kk) / Math.sqrt(dh))];
        const lsm = (a, Tt) => { const m = Math.max(...a); const l = Math.log(a.reduce((s, z) => s + Math.exp((z - m) / Tt), 0)); return a.map((z) => (z - m) / Tt - l); };
        const ls = lsm(start, temp[3]);
        let best = 0; for (let t = 1; t < S; t++) if (ls[1 + t] > ls[1 + best]) best = t;
        const eq = mv("h.eq.weight", hAns), es = mv("h.es.weight", row(enc.stIdx[best] ?? 0));
        const qe = eq.map((z, i) => z + es[i]);
        const e = enc.stIdx.map((i, t) => (t >= best && t < best + this.meta.format.span_max) ? dot(qe, mv("h.ek.weight", row(i))) / Math.sqrt(dh) : -1e4);
        const le = S ? lsm(e, temp[3]) : [];
        let bestE = best; for (let t = 0; t < S; t++) if (le[t] > le[bestE]) bestE = t;
        const pPresent = 1 - Math.exp(ls[0]);
        let text = "", char = null;
        if (S && enc.stOff[best] && enc.stOff[bestE]) { char = [enc.stOff[best][0], enc.stOff[bestE][1]]; text = req.state.slice(char[0], char[1]).trim(); }
        out.push({ type: "span", p_present: pPresent, tok: [best, bestE], p_span: S ? Math.exp(ls[1 + best] + le[bestE]) : 0, text, char });
      }
    });
    const t1 = (typeof performance !== "undefined" ? performance.now() : Date.now());
    return { answers: out, tokens: { state: enc.stIdx.length + 1, questions: T - enc.stIdx.length - 1, total: T },
             truncated: enc.truncated, ms: t1 - t0, ids: enc.ids };
  }
}
