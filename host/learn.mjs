// Learning from filed notes: the host reference for firmware/main/learn.cpp.
//
// Port of the playground's correction maths (../tinydecide/playground/index.html:224-273), keyed by
// category index instead of a question signature. Each example is one filed note embedded with the
// current question: {v: qvec (h.P projection, 128-d), z: zero-shot logits}. protosFor() turns the
// examples into the `protos` argument of TinyDecide.answer().
export const DH = 128;
const K_BUCKETS = [1, 2, 4, 8];
const bucketK = (c) => K_BUCKETS.reduce((b, e) => (c > e ? b + 1 : b), 0);

export function meanVec(list, dh = DH) {
  const m = new Array(dh).fill(0);
  for (const e of list) for (let i = 0; i < dh; i++) m[i] += e.v[i] / list.length;
  return m;
}
function cosC(a, b, c) {
  let d = 0, na = 0, nb = 0;
  for (let i = 0; i < a.length; i++) { const x = a[i] - c[i], y = b[i] - c[i]; d += x * y; na += x * x; nb += y * y; }
  return d / (Math.sqrt(na * nb) || 1e-12);
}
function termFor(v, lists, c, beta) {
  return lists.map((l) => (l.length ? beta[bucketK(l.length)] * cosC(v, meanVec(l, v.length), c) : 0));
}
// How much to trust the examples: leave-one-out log-likelihood over the examples themselves.
export function lambdaFor(lists, c, beta) {
  const all = []; lists.forEach((l, k) => l.forEach((e, j) => all.push([k, j, e])));
  if (all.length < 2) return 0.25;
  let best = 0, bestLL = -Infinity;
  for (const lam of [0, 0.25, 0.5, 1, 2]) {
    let ll = 0;
    for (const [k, j, e] of all) {
      const rest = lists.map((l, kk) => (kk === k ? l.filter((_, jj) => jj !== j) : l));
      const t = termFor(e.v, rest, c, beta);
      const z = e.z.map((x, i) => x + lam * t[i]);
      const m = Math.max(...z), lse = m + Math.log(z.reduce((a, x) => a + Math.exp(x - m), 0));
      ll += z[k] - lse;
    }
    if (ll > bestLL + 1e-9) { best = lam; bestLL = ll; }
  }
  return best;
}
// lists[k] = examples filed under category k. centre = mean of every example vector.
export function protosFor(lists, beta) {
  const K = lists.length;
  if (!lists.some((l) => l.length)) return null;
  const center = meanVec(lists.flat());
  const vec = new Float32Array(K * DH), cnt = new Int32Array(K);
  lists.forEach((l, k) => { cnt[k] = l.length; if (l.length) meanVec(l).forEach((x, i) => { vec[k * DH + i] = x; }); });
  return { vec, cnt, center: Float32Array.from(center), lam: lambdaFor(lists, center, beta) };
}
