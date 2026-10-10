// TinyDecide device engine. See tinydecide.h and tinydecide.js (the reference).
//
// Memory plan ("streamed kernel"): one scratch area per call, ~2.2 KB per token. Each layer keeps
// the fp32 residual x plus an int8 copy of the layer input; attention runs one head at a time with
// W_o accumulated straight into x, and the FFN runs fc in 64-neuron chunks into 256-neuron int8
// groups (in the then idle K and V buffers), each group's fc2 slice accumulated into x. Every
// weight row is read once per layer and applied to all tokens while it is hot in cache, four
// tokens per unpacked weight block on the ESP32-S3; rows are split across both cores on dual-core
// chips.
#include "tinydecide.h"
#include "td_meta.h"
#include "td_unicode.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <initializer_list>

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
static uint32_t now_ms() { return (uint32_t)(esp_timer_get_time() / 1000); }
#if CONFIG_IDF_TARGET_ESP32S3
#define TD_PIE 1
#endif
#if !CONFIG_FREERTOS_UNICORE
#define TD_DUAL 1
#endif
#else
#include <chrono>
static uint32_t now_ms() {
  using namespace std::chrono;
  return (uint32_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
#endif

#ifndef TD_HEAP_RESERVE
#define TD_HEAP_RESERVE (24 * 1024)   // heap left to the rest of the app while a pass runs
#endif

namespace td {

static const uint8_t* g_m = nullptr;     // model.bin
static uint32_t g_vn = 0;                // vocab.bin
static const uint32_t* g_voff = nullptr;
static const uint16_t* g_vid = nullptr;
static const char* g_vblob = nullptr;

static inline const float* F(uint32_t off) { return (const float*)(g_m + off); }

const char* statusText(Status s) {
  switch (s) {
    case OK: return "ok";
    case NOT_READY: return "model not loaded";
    case BAD_ARGS: return "bad arguments";
    case QUESTION_TOO_LONG: return "question too long";
    case NO_MEMORY: return "out of memory";
  }
  return "?";
}

// ============================================================================
//  Tokenizer: WordPiece.encode of tinydecide.js, on UTF-8
// ============================================================================

static int vocabFind(const char* s, int len) {
  int lo = 0, hi = (int)g_vn - 1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    const char* e = g_vblob + g_voff[mid];
    int el = (int)(g_voff[mid + 1] - g_voff[mid]);
    int c = memcmp(s, e, len < el ? len : el);
    if (c == 0) c = len - el;
    if (c == 0) return g_vid[mid];
    if (c < 0) hi = mid - 1; else lo = mid + 1;
  }
  return -1;
}

static inline bool asciiPunct(uint32_t c) {
  return (c >= 33 && c <= 47) || (c >= 58 && c <= 64) || (c >= 91 && c <= 96) || (c >= 123 && c <= 126);
}

// What the reference does to a code point >= 0x80 (td_unicode.h). For MAP, *map is set.
static uint8_t classify(uint32_t cp, const tdu::Map** map) {
  int lo = 0, hi = tdu::N_RANGES - 1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    if (cp < tdu::RANGES[mid].lo) hi = mid - 1;
    else if (cp > tdu::RANGES[mid].hi) lo = mid + 1;
    else return tdu::RANGES[mid].kind;
  }
  lo = 0; hi = tdu::N_MAPS - 1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    if (cp < tdu::MAPS[mid].cp) hi = mid - 1;
    else if (cp > tdu::MAPS[mid].cp) lo = mid + 1;
    else { *map = &tdu::MAPS[mid]; return tdu::MAP; }
  }
  return tdu::WORD;
}

// Decodes one UTF-8 code point from at most rem bytes; invalid input becomes U+FFFD (dropped, like
// the reference).
static uint32_t nextCp(const unsigned char* p, size_t rem, int* len) {
  const unsigned char c = p[0];
  if (c < 0x80) { *len = 1; return c; }
  const int n = c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
  if (n == 0 || (size_t)n > rem) { *len = 1; return 0xFFFD; }
  uint32_t cp = c & (0x7F >> n);
  for (int i = 1; i < n; i++) {
    if ((p[i] & 0xC0) != 0x80) { *len = 1; return 0xFFFD; }
    cp = (cp << 6) | (p[i] & 0x3F);
  }
  *len = n;
  if ((n == 3 && cp < 0x800) || (n == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF))
    return 0xFFFD;
  return cp;
}

static int putUtf8(uint32_t cp, char* o) {
  if (cp < 0x80) { o[0] = (char)cp; return 1; }
  if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
  if (cp < 0x10000) {
    o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

namespace {
struct Tok {
  uint16_t* ids; int32_t* starts; int32_t* ends; int max, n;
  // current word: normalised code points with their source byte ranges
  uint32_t cp[tdm::MAX_CHARS];
  int32_t a[tdm::MAX_CHARS], b[tdm::MAX_CHARS];
  int wl;
  bool longWord;
  int32_t firstA, lastB;
  char utf[tdm::MAX_CHARS * 4];       // the word as UTF-8
  char piece[tdm::MAX_CHARS * 4 + 2];
  uint16_t uo[tdm::MAX_CHARS + 1];    // byte offset of each code point in utf
  uint16_t pid[tdm::MAX_CHARS];
  int32_t pa[tdm::MAX_CHARS], pb[tdm::MAX_CHARS];

  void emit(int id, int32_t s, int32_t e) {
    if (n >= max) return;
    ids[n] = (uint16_t)id;
    if (starts) starts[n] = s;
    if (ends) ends[n] = e;
    n++;
  }
  void add(uint32_t c, int32_t s, int32_t e) {
    if (wl == 0 && !longWord) firstA = s;
    lastB = e;
    if (wl < tdm::MAX_CHARS) { cp[wl] = c; a[wl] = s; b[wl] = e; wl++; }
    else longWord = true;
  }
  void flush() {
    if (longWord) emit(tdm::SP_UNK, firstA, lastB);
    else if (wl) encodeWord();
    wl = 0; longWord = false;
  }
  void isolated(uint32_t c, int32_t s, int32_t e) { flush(); add(c, s, e); flush(); }

  void encodeWord() {
    int ub = 0;
    for (int i = 0; i < wl; i++) { uo[i] = (uint16_t)ub; ub += putUtf8(cp[i], utf + ub); }
    uo[wl] = (uint16_t)ub;
    int np = 0, start = 0;
    while (start < wl) {
      int end = wl, got = -1;
      while (start < end) {
        if (start == 0) got = vocabFind(utf, uo[end]);
        else {                                     // continuation piece: "##" + the code points
          const int len = uo[end] - uo[start];
          piece[0] = '#'; piece[1] = '#';
          memcpy(piece + 2, utf + uo[start], len);
          got = vocabFind(piece, len + 2);
        }
        if (got >= 0) break;
        end--;
      }
      if (got < 0) { emit(tdm::SP_UNK, a[0], b[wl - 1]); return; }
      pid[np] = (uint16_t)got; pa[np] = a[start]; pb[np] = b[end - 1]; np++;
      start = end;
    }
    for (int i = 0; i < np; i++) emit(pid[i], pa[i], pb[i]);
  }
};
Tok g_tok;
}  // namespace

int tokenize(const char* text, uint16_t* ids, int max, int32_t* starts, int32_t* ends) {
  return tokenize(text, text ? strlen(text) : 0, ids, max, starts, ends);
}

int tokenize(const char* text, size_t n, uint16_t* ids, int max, int32_t* starts, int32_t* ends) {
  if (!g_m || !text || max <= 0) return 0;
  Tok& t = g_tok;
  t.ids = ids; t.starts = starts; t.ends = ends; t.max = max; t.n = 0; t.wl = 0; t.longWord = false;
  const unsigned char* p = (const unsigned char*)text;
  int32_t i = 0;
  while ((size_t)i < n && t.n < max) {
    int len;
    const uint32_t c = nextCp(p + i, n - (size_t)i, &len);
    const int32_t s = i, e = i + len;
    i = e;
    if (c < 0x80) {
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { t.flush(); continue; }
      if (c < 32 || c == 127) continue;                       // control characters are dropped
      if (asciiPunct(c)) { t.isolated(c, s, e); continue; }
      t.add(c >= 'A' && c <= 'Z' ? c + 32 : c, s, e);
      continue;
    }
    if (c >= 0xAC00 && c <= 0xD7A3) {                         // Hangul syllable: NFD to 2-3 jamo
      const uint32_t k = c - 0xAC00, tj = k % 28;
      t.add(0x1100 + k / 588, s, e);
      t.add(0x1161 + (k % 588) / 28, s, e);
      if (tj) t.add(0x11A7 + tj, s, e);
      continue;
    }
    const tdu::Map* m = nullptr;
    switch (classify(c, &m)) {
      case tdu::DROP: case tdu::MN: break;
      case tdu::SPACE: t.flush(); break;
      case tdu::CJK: case tdu::PUNCT: t.isolated(c, s, e); break;
      case tdu::MAP:
        for (int j = 0; j < m->len; j++) {
          const uint32_t o = tdu::MAP_OUT[m->off + j];
          if (o & tdu::OUT_PUNCT) t.isolated(o & ~tdu::OUT_PUNCT, s, e);
          else t.add(o, s, e);
        }
        break;
      default: t.add(c, s, e);
    }
  }
  t.flush();
  return t.n < max ? t.n : max;
}

// ============================================================================
//  Kernels
// ============================================================================

static inline float bf16(const uint8_t* p) {
  uint32_t u = (uint32_t)(p[0] | (p[1] << 8)) << 16;
  float f;
  memcpy(&f, &u, 4);
  return f;
}

static inline float dotq(const uint8_t* nib, const int8_t* xq, const uint8_t* sc, const float* xs, int nb) {
#if defined(TD_PIE)
  return dot_q4q8_pie(nib, xq, sc, xs, nb);
#else
  float acc = 0.0f;
  for (int b = 0; b < nb; b++) {
    int isum = 0;
    for (int k = 0; k < 16; k++) {
      uint8_t bk = nib[k];
      isum += ((int)(bk & 0x0F) - 8) * (int)xq[k];
      isum += ((int)(bk >> 4) - 8) * (int)xq[k + 16];
    }
    acc += (float)isum * (bf16(sc) * xs[b]);
    nib += 16; xq += 32; sc += 2;
  }
  return acc;
#endif
}

// lrintf for |v| < 2^22 under round-to-nearest-even, without the libm call: adding 1.5 * 2^23
// leaves no fraction bits, so the FPU rounds v to an integer exactly as lrintf does.
static inline int roundEven(float v) { return (int)((v + 12582912.0f) - 12582912.0f); }

// Symmetric int8 per 32-element block, one fp32 scale each (llama.cpp Q8_0).
// No a*b+c fusion here: x * inv must be rounded to float before roundEven, as lrintf would see it.
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("fp-contract=off")))
#endif
static void quant8(const float* x, int8_t* xq, float* xs, int n) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
  for (int b = 0; b < n / 32; b++) {
    float mx = 0.0f;
    for (int i = 0; i < 32; i++) { float a = fabsf(x[i]); if (a > mx) mx = a; }
    float s = mx / 127.0f, inv = s > 0 ? 1.0f / s : 0.0f;
    xs[b] = s;
    for (int i = 0; i < 32; i++) xq[i] = (int8_t)roundEven(x[i] * inv);
    x += 32; xq += 32;
  }
}

// out[t*os + (r - r0)] (=|+=) W[r, blk0*32 .. (blk0+nbk)*32) . x[t] + bias[r], for r in [r0, r1).
struct MM {
  const tdm::Q4* w;
  int blk0, nbk;
  const int8_t* xq; int xqs;     // int8 activations, stride per token (bytes)
  const float* xs;  int xss;     // block scales, stride per token
  int T;
  float* out; int os;
  const float* bias;
  bool acc;
  int r0;
};

static void mmRows(const MM& a, int ra, int rb) {
  const int nbRow = a.w->cols / 32;
  for (int r = ra; r < rb; r++) {
    const uint8_t* nib = g_m + a.w->nib + ((size_t)r * nbRow + a.blk0) * 16;
    const uint8_t* sc  = g_m + a.w->sc + ((size_t)r * nbRow + a.blk0) * 2;
    const float b = a.bias ? a.bias[r] : 0.0f;
    float* o = a.out + (r - a.r0);
    int t = 0;
#if defined(TD_PIE)
    // tiles of four tokens, up to eight tiles per call: each weight block is unpacked once per
    // tile (xss == xqs / 32)
    float vt[32];
    while (t + 4 <= a.T) {
      const int nt = (a.T - t) / 4 < 8 ? (a.T - t) / 4 : 8;
      dot4_q4q8_pie(nib, sc, a.xq + (size_t)t * a.xqs, a.xs + (size_t)t * a.xss, a.nbk, a.xqs, vt, nt);
      for (int j = 0; j < 4 * nt; j++) {
        const float v = vt[j] + b;
        if (a.acc) o[(size_t)(t + j) * a.os] += v; else o[(size_t)(t + j) * a.os] = v;
      }
      t += 4 * nt;
    }
#endif
    for (; t < a.T; t++) {
      float v = dotq(nib, a.xq + (size_t)t * a.xqs, sc, a.xs + (size_t)t * a.xss, a.nbk) + b;
      if (a.acc) o[(size_t)t * a.os] += v; else o[(size_t)t * a.os] = v;
    }
  }
}

// parallel(fn, ctx, n): fn(ctx, a, b) over [0, n), split in two halves across both cores.
typedef void (*RangeFn)(void* ctx, int a, int b);

#if defined(TD_DUAL)
// Persistent worker on the other core; per call it costs two semaphore operations.
static SemaphoreHandle_t s_go = nullptr, s_done = nullptr;
static RangeFn s_fn = nullptr;
static void* s_ctx = nullptr;
static int s_ja = 0, s_jb = 0;
static int s_core = -1;

static void worker(void*) {
  for (;;) {
    xSemaphoreTake(s_go, portMAX_DELAY);
    s_fn(s_ctx, s_ja, s_jb);
    xSemaphoreGive(s_done);
  }
}

static void parallel(RangeFn fn, void* ctx, int n) {
  if (!s_go) {
    s_go = xSemaphoreCreateBinary();
    s_done = xSemaphoreCreateBinary();
    s_core = xPortGetCoreID() == 0 ? 1 : 0;
    xTaskCreatePinnedToCore(worker, "td_mm", 3072, nullptr, 3, nullptr, s_core);
  }
  if (xPortGetCoreID() == s_core || n < 2) { fn(ctx, 0, n); return; }   // caller moved cores: run alone
  const int split = n / 2;
  s_fn = fn; s_ctx = ctx; s_ja = split; s_jb = n;
  xSemaphoreGive(s_go);
  fn(ctx, 0, split);
  xSemaphoreTake(s_done, portMAX_DELAY);
}
#else
static void parallel(RangeFn fn, void* ctx, int n) { fn(ctx, 0, n); }
#endif

#ifdef ESP_PLATFORM
static void* scratchAlloc(size_t n) {
  void* p = heap_caps_aligned_alloc(16, n, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
#if CONFIG_SPIRAM
  if (!p) p = heap_caps_aligned_alloc(16, n, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
#endif
  return p;
}
static void scratchFree(void* p) { heap_caps_free(p); }
static bool reserveOk() { return heap_caps_get_free_size(MALLOC_CAP_8BIT) >= TD_HEAP_RESERVE; }
#else
static void* scratchAlloc(size_t n) { return aligned_alloc(16, (n + 15) & ~(size_t)15); }
static void scratchFree(void* p) { free(p); }
static bool reserveOk() { return true; }
#endif

static void mmRange(void* ctx, int a, int b) {
  const MM& m = *(const MM*)ctx;
  mmRows(m, m.r0 + a, m.r0 + b);
}
static void mm(const MM& a, int r1) { parallel(mmRange, (void*)&a, r1 - a.r0); }

static void layernorm(float* x, int d, const float* w, const float* b, float eps) {
  float m = 0;
  for (int i = 0; i < d; i++) m += x[i];
  m /= d;
  float v = 0;
  for (int i = 0; i < d; i++) { float z = x[i] - m; v += z * z; }
  v /= d;
  const float r = 1.0f / sqrtf(v + eps);
  for (int i = 0; i < d; i++) x[i] = (x[i] - m) * r * w[i] + b[i];
}

// Exact (erf) GELU from a table with linear interpolation; step 1/64 over [-8, 8], error < 3e-5.
static constexpr int GELU_N = 1024;
static float g_gelu[GELU_N + 1];
static void geluInit() {
  for (int i = 0; i <= GELU_N; i++) {
    const double x = -8.0 + 16.0 * i / GELU_N;
    g_gelu[i] = (float)(0.5 * x * (1.0 + erf(x * 0.70710678118654752)));
  }
}
static inline float gelu(float x) {
  if (x >= 8.0f) return x;
  if (x <= -8.0f) return 0.0f;
  const float f = (x + 8.0f) * (GELU_N / 16.0f);
  const int i = (int)f;
  const float w = f - (float)i;
  return g_gelu[i] + (g_gelu[i + 1] - g_gelu[i]) * w;
}

// y = W v for an int8 head matrix (one f32 scale per row).
static void mvI8(const tdm::I8& w, const float* v, float* y) {
  const int8_t* q = (const int8_t*)(g_m + w.q);
  const float* s = F(w.sc);
  for (int r = 0; r < w.rows; r++) {
    float acc = 0;
    const int8_t* row = q + (size_t)r * w.cols;
    for (int c = 0; c < w.cols; c++) acc += (float)row[c] * v[c];
    y[r] = acc * s[r];
  }
}

// u = W^T v for an int8 head matrix: q . (W h) == u . h, so a head that scores many tokens against
// one query reads W once instead of once per token. u has w.cols entries.
static void mtvI8(const tdm::I8& w, const float* v, float* u) {
  const int8_t* q = (const int8_t*)(g_m + w.q);
  const float* s = F(w.sc);
  for (int c = 0; c < w.cols; c++) u[c] = 0;
  for (int r = 0; r < w.rows; r++) {
    const float k = v[r] * s[r];
    const int8_t* row = q + (size_t)r * w.cols;
    for (int c = 0; c < w.cols; c++) u[c] += k * (float)row[c];
  }
}

// exp(x) for the attention softmax, x <= 0: Cephes expf (within about 1 ulp of expf) at a tenth of
// newlib's cost. Returns 0 below -87, where expf is denormal.
static inline float expNeg(float x) {
  if (x < -87.0f) return 0.0f;
  const float n = (x * 1.44269504089f + 12582912.0f) - 12582912.0f;   // round(x / ln 2)
  const float r = (x - n * 0.693359375f) - n * -2.12194440e-4f;
  float p = 1.9875691500e-4f;
  p = p * r + 1.3981999507e-3f;
  p = p * r + 8.3334519073e-3f;
  p = p * r + 4.1665795894e-2f;
  p = p * r + 1.6666665459e-1f;
  p = p * r + 5.0000001201e-1f;
  p = p * r * r + r + 1.0f;
  const uint32_t bits = (uint32_t)((int)n + 127) << 23;
  float sc;
  memcpy(&sc, &bits, 4);
  return p * sc;
}

// ============================================================================
//  Request layout (encodeRequest in tinydecide.js)
// ============================================================================

struct Req {
  uint16_t* ids;                  // [T]
  uint16_t* pos;                  // [T]
  uint8_t* blk;                   // [T] 0 = state, k = question k
  int T, S;                       // total tokens; state tokens incl. the <|state|> marker
  int nq;
  int qs[MAX_QUESTIONS + 1], qe[MAX_QUESTIONS + 1];   // block k occupies [qs[k], qe[k])
  int ans[MAX_QUESTIONS];
  int opt[MAX_QUESTIONS][MAX_OPTIONS];
  int32_t sa[tdm::TS_MAX], sb[tdm::TS_MAX];           // byte range of each state text token
  bool truncated;
};

// Question block: <|type|> text [<|sep|> (option <|o|> or <|lv|>)*] <|ans|>. Writes up to cap ids
// and returns the block's full length, which may exceed cap (then the question is too long).
static int questionBlock(const Question& q, uint16_t* ids, int cap, int* optLocal, int* ansLocal) {
  static uint16_t tmp[tdm::Q_MAX + 1];
  int m = 0;
  auto put = [&](int id) { if (m < cap) ids[m] = (uint16_t)id; m++; };
  put(tdm::SP_TYPE[q.type]);
  int k = tokenize(q.text, tmp, tdm::Q_MAX + 1);
  for (int i = 0; i < k; i++) put(tmp[i]);
  if (q.n_options > 0) {
    put(tdm::SP_SEP);
    const int mk = q.type == CHOICE ? tdm::SP_O : tdm::SP_LV;
    for (int i = 0; i < q.n_options; i++) {
      k = tokenize(q.options[i] ? q.options[i] : "", tmp, tdm::Q_MAX + 1);
      for (int j = 0; j < k; j++) put(tmp[j]);
      if (optLocal) optLocal[i] = m;
      put(mk);
      if (m > tdm::Q_MAX) return m;
    }
  }
  if (ansLocal) *ansLocal = m;
  put(tdm::SP_ANS);
  return m;
}

static Status checkArgs(const Question* qs, int nq) {
  if (nq < 1 || nq > MAX_QUESTIONS || !qs) return BAD_ARGS;
  for (int k = 0; k < nq; k++) {
    const Question& q = qs[k];
    if (q.type > SPAN || !q.text) return BAD_ARGS;
    if ((q.type == CHOICE || q.type == SCORE) && (q.n_options < 2 || q.n_options > MAX_OPTIONS || !q.options))
      return BAD_ARGS;
    if (q.n_options < 0 || q.n_options > MAX_OPTIONS || (q.n_options && !q.options)) return BAD_ARGS;
    if ((q.type == NOUL || q.type == SPAN) && q.n_options) return BAD_ARGS;
    if (q.protos && (q.type == SPAN || !q.protos->vec || !q.protos->cnt)) return BAD_ARGS;
  }
  return OK;
}

int requestTokens(const Question* qs, int nq, int state_tokens) {
  if (checkArgs(qs, nq) != OK) return -1;
  static uint16_t ids[tdm::Q_MAX];
  int T = 1 + (state_tokens < STATE_MAX ? state_tokens : STATE_MAX);
  for (int k = 0; k < nq; k++) {
    const int n = questionBlock(qs[k], ids, tdm::Q_MAX, nullptr, nullptr);
    if (n > tdm::Q_MAX) return -1;
    T += n;
  }
  return T;
}

// ============================================================================
//  Encoder
// ============================================================================

// Several blocks rather than one, so the scratch fits a fragmented heap (no PSRAM on most boards):
//   A: x [T, D] f32   B: Qh, Kh, Vh [T, 64] f32   C: xq, hq int8 + block scales + scores.
struct Scratch { uint8_t* a; uint8_t* q; uint8_t* k; uint8_t* v; uint8_t* c; };

static size_t bytesA(int T) { return (size_t)T * tdm::D * sizeof(float); }
static size_t bytesB(int T) { return (size_t)T * tdm::DHEAD * sizeof(float); }   // each of Q, K, V
static size_t bytesC(int T) {
  return (size_t)T * (tdm::D + tdm::DHEAD + (tdm::D / 32 + tdm::DHEAD / 32 + 2) * sizeof(float));
}

size_t scratchBytes(int T) { return bytesA(T) + 3 * bytesB(T) + bytesC(T); }

static void freeScratch(Scratch& m) {
  for (uint8_t* p : {m.c, m.v, m.k, m.q, m.a}) if (p) scratchFree(p);
  m = {};
}

static bool allocScratch(Scratch& m, int T) {
  m = {};
  m.a = (uint8_t*)scratchAlloc(bytesA(T));   // largest first
  m.q = m.a ? (uint8_t*)scratchAlloc(bytesB(T)) : nullptr;
  m.k = m.q ? (uint8_t*)scratchAlloc(bytesB(T)) : nullptr;
  m.v = m.k ? (uint8_t*)scratchAlloc(bytesB(T)) : nullptr;
  m.c = m.v ? (uint8_t*)scratchAlloc(bytesC(T)) : nullptr;
  if (m.c && reserveOk()) return true;
  freeScratch(m);
  return false;
}

// GELU over one 64-neuron FFN chunk for tokens [a, b), then int8 into columns of hq (row stride
// qs bytes, scales qs / 32 floats) for fc2.
struct GeluCtx { float* Hc; int8_t* hq; float* hs; int qs; };
static void geluRange(void* ctx, int a, int b) {
  const GeluCtx& g = *(const GeluCtx*)ctx;
  const int DHd = tdm::DHEAD;
  for (int t = a; t < b; t++) {
    float* hrow = g.Hc + (size_t)t * DHd;
    for (int c = 0; c < DHd; c++) hrow[c] = gelu(hrow[c]);
    quant8(hrow, g.hq + (size_t)t * g.qs, g.hs + (size_t)t * (g.qs / 32), DHd);
  }
}

// Optional LayerNorm (w != nullptr), then int8 of x for the next matmuls, for tokens [a, b).
struct NormCtx { float* x; int8_t* xq; float* xs; const float* w; const float* b; };
static void normRange(void* ctx, int a, int b) {
  const NormCtx& n = *(const NormCtx*)ctx;
  const int D = tdm::D;
  for (int t = a; t < b; t++) {
    float* row = n.x + (size_t)t * D;
    if (n.w) layernorm(row, D, n.w, n.b, tdm::LN_EPS);
    quant8(row, n.xq + (size_t)t * D, n.xs + (size_t)t * (D / 32), D);
  }
}

// One head's attention for query rows [a, b): softmax(q k / sqrt(64)) v, written as int8 to hq.
// State tokens see the state; a question's tokens see the state and their own block.
struct AttnCtx {
  const float *Qh, *Kh, *Vh;
  int8_t* hq;
  float* hs;
  float* sc;      // [2, T] scores, one row per core
  const Req* r;
  float scale;
};

static void attnRange(void* ctx, int a, int b) {
  const AttnCtx& c = *(const AttnCtx*)ctx;
  const Req& r = *c.r;
  const int DHd = tdm::DHEAD;
  float* sc = c.sc + (a == 0 ? 0 : r.T);
  float out[64];
  for (int i = a; i < b; i++) {
    const int k = r.blk[i];
    const int ranges[2][2] = {{0, r.S}, {k ? r.qs[k] : 0, k ? r.qe[k] : 0}};
    const float* q = c.Qh + (size_t)i * DHd;
    float mx = -1e30f;
    int n = 0;
    for (const auto& rg : ranges) {
      int j = rg[0];
      // four keys at a time: four independent sums, each in the order of the one-key loop
      for (; j + 4 <= rg[1]; j += 4) {
        const float* k0 = c.Kh + (size_t)j * DHd;
        const float *k1 = k0 + DHd, *k2 = k1 + DHd, *k3 = k2 + DHd;
        float s0 = 0, s1 = 0, s2 = 0, s3 = 0;
        for (int d = 0; d < DHd; d++) {
          const float qd = q[d];
          s0 += qd * k0[d]; s1 += qd * k1[d]; s2 += qd * k2[d]; s3 += qd * k3[d];
        }
        for (float s : {s0, s1, s2, s3}) {
          s *= c.scale;
          sc[n++] = s;
          if (s > mx) mx = s;
        }
      }
      for (; j < rg[1]; j++) {
        const float* kk = c.Kh + (size_t)j * DHd;
        float s = 0;
        for (int d = 0; d < DHd; d++) s += q[d] * kk[d];
        s *= c.scale;
        sc[n++] = s;
        if (s > mx) mx = s;
      }
    }
    float z = 0;
    for (int j = 0; j < n; j++) { sc[j] = expNeg(sc[j] - mx); z += sc[j]; }
    const float iz = 1.0f / z;
    for (int j = 0; j < n; j++) sc[j] *= iz;
    // out = sum_j p_j v_j, eight dimensions at a time so the sums stay in registers
    for (int d0 = 0; d0 < DHd; d0 += 8) {
      float o0 = 0, o1 = 0, o2 = 0, o3 = 0, o4 = 0, o5 = 0, o6 = 0, o7 = 0;
      n = 0;
      for (const auto& rg : ranges)
        for (int j = rg[0]; j < rg[1]; j++) {
          const float p = sc[n++];
          const float* v = c.Vh + (size_t)j * DHd + d0;
          o0 += p * v[0]; o1 += p * v[1]; o2 += p * v[2]; o3 += p * v[3];
          o4 += p * v[4]; o5 += p * v[5]; o6 += p * v[6]; o7 += p * v[7];
        }
      out[d0] = o0; out[d0 + 1] = o1; out[d0 + 2] = o2; out[d0 + 3] = o3;
      out[d0 + 4] = o4; out[d0 + 5] = o5; out[d0 + 6] = o6; out[d0 + 7] = o7;
    }
    quant8(out, c.hq + (size_t)i * DHd, c.hs + (size_t)i * (DHd / 32), DHd);
  }
}

static void encode(const Req& r, const Scratch& m) {
  const int T = r.T, D = tdm::D, DHd = tdm::DHEAD, E = tdm::EMB;
  float* x  = (float*)m.a;                                  // [T, D]
  float* Qh = (float*)m.q;                                  // [T, 64] each
  float* Kh = (float*)m.k;
  float* Vh = (float*)m.v;
  int8_t* xq = (int8_t*)m.c;                                // [T, D]   (16-aligned: offsets are multiples of 64)
  int8_t* hq = xq + (size_t)T * D;                          // [T, 64]
  float* xs = (float*)(hq + (size_t)T * DHd);               // [T, D/32]
  float* hs = xs + (size_t)T * (D / 32);                    // [T, 2]
  float* sc = hs + (size_t)T * (DHd / 32);                  // [2, T]

  // ---- embeddings: word (Q4) + position (int8) + type0, LayerNorm, project to D
  const int8_t* P = (const int8_t*)(g_m + tdm::POS.q);
  const float* Ps = F(tdm::POS.sc);
  const float* t0 = F(tdm::TYPE0);
  for (int t = 0; t < T; t++) {
    float o[tdm::EMB];
    const int nb = E / 32;
    const uint8_t* nib = g_m + tdm::WORD.nib + (size_t)r.ids[t] * nb * 16;
    const uint8_t* ws = g_m + tdm::WORD.sc + (size_t)r.ids[t] * nb * 2;
    for (int b = 0; b < nb; b++) {
      const float d = bf16(ws + 2 * b);
      for (int k = 0; k < 16; k++) {
        const uint8_t byte = nib[b * 16 + k];
        o[b * 32 + k] = ((int)(byte & 15) - 8) * d;
        o[b * 32 + k + 16] = ((int)(byte >> 4) - 8) * d;
      }
    }
    const int8_t* pr = P + (size_t)r.pos[t] * E;
    const float ps = Ps[r.pos[t]];
    for (int c = 0; c < E; c++) o[c] += pr[c] * ps + t0[c];
    layernorm(o, E, F(tdm::ELN_W), F(tdm::ELN_B), tdm::LN_EPS);
    quant8(o, xq + (size_t)t * E, xs + (size_t)t * (E / 32), E);
  }
  mm({&tdm::PROJ, 0, E / 32, xq, E, xs, E / 32, T, x, D, F(tdm::PROJ_B), false, 0}, D);

  const float scale = 1.0f / sqrtf((float)DHd);
  for (int l = 0; l < tdm::LAYERS; l++) {
    const tdm::Block& B = tdm::BLOCKS[l];
    if (l == 0) {
      NormCtx nc{x, xq, xs, nullptr, nullptr};
      parallel(normRange, &nc, T);
    }

    // ---- attention, one head at a time; W_o slice accumulated into the residual
    for (int h = 0; h < tdm::HEADS; h++) {
      const int r0 = h * DHd, r1 = r0 + DHd;
      mm({&B.q, 0, D / 32, xq, D, xs, D / 32, T, Qh, DHd, F(B.qb), false, r0}, r1);
      mm({&B.k, 0, D / 32, xq, D, xs, D / 32, T, Kh, DHd, F(B.kb), false, r0}, r1);
      mm({&B.v, 0, D / 32, xq, D, xs, D / 32, T, Vh, DHd, F(B.vb), false, r0}, r1);
      AttnCtx ac{Qh, Kh, Vh, hq, hs, sc, &r, scale};
      parallel(attnRange, &ac, T);
      mm({&B.o, r0 / 32, DHd / 32, hq, DHd, hs, DHd / 32, T, x, D, h == 0 ? F(B.ob) : nullptr, true, 0}, D);
    }
    NormCtx n1{x, xq, xs, F(B.ln1w), F(B.ln1b)};
    parallel(normRange, &n1, T);

    // ---- FFN: fc in 64-neuron chunks through GELU into int8 groups of G neurons, held in the
    // K and V buffers (free until the next layer); each group's fc2 slice is accumulated into x.
    constexpr int G = 256;
    static_assert(tdm::FFN % G == 0 && G <= tdm::DHEAD * 4, "an FFN group must fit the K buffer");
    float* Hc = Qh;
    int8_t* gq = (int8_t*)Kh;                               // [T, G] int8
    float* gs = Vh;                                         // [T, G / 32]
    for (int g0 = 0; g0 < tdm::FFN; g0 += G) {
      for (int c0 = g0; c0 < g0 + G; c0 += DHd) {
        mm({&B.fc, 0, D / 32, xq, D, xs, D / 32, T, Hc, DHd, F(B.fcb), false, c0}, c0 + DHd);
        GeluCtx gc{Hc, gq + (c0 - g0), gs + (c0 - g0) / 32, G};
        parallel(geluRange, &gc, T);
      }
      mm({&B.fc2, g0 / 32, G / 32, gq, G, gs, G / 32, T, x, D, g0 == 0 ? F(B.fc2b) : nullptr, true, 0}, D);
    }
    NormCtx n2{x, xq, xs, F(B.ln2w), F(B.ln2b)};   // the int8 copy is the next layer's input
    parallel(normRange, &n2, T);
  }
}

// ============================================================================
//  Heads (TinyDecide.answer in tinydecide.js)
// ============================================================================

static int bucketK(int c) { int b = 0; for (int e : {1, 2, 4, 8}) if (c > e) b++; return b; }

// row t of x through the head LayerNorm
static void headRow(const float* x, int t, float* h) {
  memcpy(h, x + (size_t)t * tdm::D, tdm::D * sizeof(float));
  layernorm(h, tdm::D, F(tdm::H_NORM_W), F(tdm::H_NORM_B), 1e-5f);
}

static float dotf(const float* a, const float* b, int n) { float s = 0; for (int i = 0; i < n; i++) s += a[i] * b[i]; return s; }

// The centre the correction cosines use: p.center, or without one the count-weighted mean of the
// prototypes, which is the centre makeProtos uses when it has no running mean. K = options (2 for noul).
static const float* centreOf(const Protos& p, int K, float* buf) {
  if (p.center) return p.center;
  memset(buf, 0, QDIM * sizeof(float));
  int n = 0;
  for (int i = 0; i < K; i++) {
    if (p.cnt[i] <= 0) continue;
    n += p.cnt[i];
    const float* pv = p.vec + (size_t)i * QDIM;
    for (int j = 0; j < QDIM; j++) buf[j] += p.cnt[i] * pv[j];
  }
  if (n > 0) for (int j = 0; j < QDIM; j++) buf[j] /= (float)n;
  return buf;
}

// The correction term of option i: lam * beta(cnt) * cos(qvec - c, vec_i - c).
static float protoTerm(const Protos& p, const float* qvec, const float* c, int i) {
  if (p.cnt[i] <= 0) return 0.0f;
  const float* pv = p.vec + (size_t)i * QDIM;
  float dd = 0, na = 0, nb = 0;
  for (int j = 0; j < QDIM; j++) {
    const float a = qvec[j] - c[j], b = pv[j] - c[j];
    dd += a * b; na += a * a; nb += b * b;
  }
  const float sa = sqrtf(na) > 0 ? sqrtf(na) : 1e-12f, sb = sqrtf(nb) > 0 ? sqrtf(nb) : 1e-12f;
  return p.lam * tdm::BETA[bucketK(p.cnt[i])] * dd / (sa * sb);
}

static void headChoice(const Req& r, const float* x, int k, const Question& q, Answer& out) {
  const int sel = q.type == SCORE ? 1 : 0, n = q.n_options, DH = tdm::DH;
  const float Tt = tdm::TEMP[sel ? 2 : 0], s = expf(F(tdm::H_SCALE)[sel]);
  float h[tdm::D], qa[tdm::DH], u[tdm::D], L[MAX_OPTIONS];
  headRow(x, r.ans[k], h);
  mvI8(tdm::H_A[sel], h, qa);
  mvI8(tdm::H_P, h, out.qvec);
  mtvI8(tdm::H_O[sel], qa, u);                     // qa . (H_O h) == u . h
  float cbuf[QDIM];
  const float* c = q.protos ? centreOf(*q.protos, n, cbuf) : nullptr;
  for (int i = 0; i < n; i++) {
    headRow(x, r.opt[k][i], h);
    out.z0[i] = s * dotf(u, h, tdm::D) / sqrtf((float)DH) / Tt;
    L[i] = out.z0[i] + (q.bias ? q.bias[i] : 0.0f);
    if (q.protos) L[i] += protoTerm(*q.protos, out.qvec, c, i) / Tt;
  }
  float mx = L[0];
  for (int i = 1; i < n; i++) if (L[i] > mx) mx = L[i];
  float Z = 0;
  for (int i = 0; i < n; i++) { out.probs[i] = expf(L[i] - mx); Z += out.probs[i]; }
  float H = 0, sc = 0;
  out.pick = 0;
  for (int i = 0; i < n; i++) {
    out.probs[i] /= Z;
    if (out.probs[i] > out.probs[out.pick]) out.pick = i;
    if (out.probs[i] > 0) H -= out.probs[i] * logf(out.probs[i]);
    sc += out.probs[i] * i / (float)(n - 1);
  }
  out.n = n;
  out.confidence = 1.0f - H / logf((float)n);
  out.score = q.type == SCORE ? sc : 0.0f;
}

static void headNoul(const Req& r, const float* x, int k, const Question& q, Answer& out) {
  const float Tt = tdm::TEMP[1];
  float h[tdm::D];
  headRow(x, r.ans[k], h);
  mvI8(tdm::H_P, h, out.qvec);
  const float z = dotf(F(tdm::H_NOUL_W), h, tdm::D) + F(tdm::H_NOUL_B)[0];
  out.z0[0] = 0.0f;
  out.z0[1] = z / Tt;
  float L = out.z0[1] + (q.bias ? q.bias[0] : 0.0f);
  if (q.protos) {
    float cbuf[QDIM];
    const float* c = centreOf(*q.protos, 2, cbuf);
    L += (protoTerm(*q.protos, out.qvec, c, 1) - protoTerm(*q.protos, out.qvec, c, 0)) / Tt;
  }
  out.n = 2;
  out.p = 1.0f / (1.0f + expf(-L));
  out.probs[0] = 1.0f - out.p;
  out.probs[1] = out.p;
  out.pick = out.p > 0.5f ? 1 : 0;
}

static void headSpan(const Req& r, const float* x, int k, const char* state, Answer& out) {
  const int S = r.S - 1, DH = tdm::DH;           // state text tokens at rows 1..S
  const float Tt = tdm::TEMP[3], isq = 1.0f / sqrtf((float)DH);
  float h[tdm::D], hAns[tdm::D], qS[tdm::DH], qe[tdm::DH], u[tdm::D];
  headRow(x, r.ans[k], hAns);
  mvI8(tdm::H_P, hAns, out.qvec);
  mvI8(tdm::H_SQ, hAns, qS);
  mtvI8(tdm::H_SK, qS, u);                         // qS . (H_SK h) == u . h
  // start logits: null, then each state token; log-softmax at temperature Tt
  const float zNull = dotf(F(tdm::H_SNULL_W), hAns, tdm::D) + F(tdm::H_SNULL_B)[0];
  static float zs[tdm::TS_MAX];
  float mx = zNull;
  for (int t = 0; t < S; t++) {
    headRow(x, 1 + t, h);
    zs[t] = dotf(u, h, tdm::D) * isq;
    if (zs[t] > mx) mx = zs[t];
  }
  float Z = expf((zNull - mx) / Tt);
  for (int t = 0; t < S; t++) Z += expf((zs[t] - mx) / Tt);
  const float lZ = logf(Z);
  int best = 0;
  for (int t = 1; t < S; t++) if (zs[t] > zs[best]) best = t;
  out.p_present = 1.0f - expf((zNull - mx) / Tt - lZ);
  out.tok[0] = out.tok[1] = best;
  out.start = out.end = 0;
  out.p_span = 0.0f;
  out.n = 0;
  if (S == 0) return;
  const float lsBest = (zs[best] - mx) / Tt - lZ;
  // end logits inside [best, best + SPAN_MAX)
  float es[tdm::DH];
  mvI8(tdm::H_EQ, hAns, qe);
  headRow(x, 1 + best, h);
  mvI8(tdm::H_ES, h, es);
  for (int i = 0; i < DH; i++) qe[i] += es[i];
  mtvI8(tdm::H_EK, qe, u);                         // qe . (H_EK h) == u . h
  const int e1 = best + tdm::SPAN_MAX < S ? best + tdm::SPAN_MAX : S;
  float en[tdm::SPAN_MAX], emx = -1e30f;
  for (int t = best; t < e1; t++) {
    headRow(x, 1 + t, h);
    en[t - best] = dotf(u, h, tdm::D) * isq;
    if (en[t - best] > emx) emx = en[t - best];
  }
  float EZ = 0;
  for (int t = best; t < e1; t++) EZ += expf((en[t - best] - emx) / Tt);
  int bestE = best;
  for (int t = best; t < e1; t++) if (en[t - best] > en[bestE - best]) bestE = t;
  out.tok[1] = bestE;
  out.p_span = expf(lsBest + (en[bestE - best] - emx) / Tt - logf(EZ));
  int a = r.sa[best], b = r.sb[bestE];
  auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
  while (a < b && ws(state[a])) a++;
  while (b > a && ws(state[b - 1])) b--;
  out.start = a;
  out.end = b;
}

// ============================================================================
//  Public API
// ============================================================================

bool init(const uint8_t* model, size_t model_len, const uint8_t* vocab, size_t vocab_len) {
  g_m = nullptr;
  if (!model || model_len < tdm::MODEL_BYTES || ((uintptr_t)model & 15)) return false;
  uint32_t fnv = 0x811c9dc5u;
  for (size_t i = 0; i < 65536 && i < tdm::MODEL_BYTES; i++) { fnv ^= model[i]; fnv *= 0x01000193u; }
  if (fnv != tdm::MODEL_FNV64K) return false;                 // a different model.bin than td_meta.h
  if (!vocab || vocab_len < 12 || memcmp(vocab, "TDV1", 4) != 0) return false;
  uint32_t n, blob;
  memcpy(&n, vocab + 4, 4);
  memcpy(&blob, vocab + 8, 4);
  if (12 + 4 * (size_t)(n + 1) + 2 * (size_t)n + blob > vocab_len) return false;
  geluInit();
  g_vn = n;
  g_voff = (const uint32_t*)(vocab + 12);
  g_vid = (const uint16_t*)(vocab + 12 + 4 * (n + 1));
  g_vblob = (const char*)(vocab + 12 + 4 * (n + 1) + 2 * n);
  g_m = model;
  return true;
}

#ifdef ESP_PLATFORM
#if defined(TINYDECIDE_EMBED)
extern "C" const uint8_t td_model_bin[], td_model_bin_end[], td_vocab_bin[], td_vocab_bin_end[];
bool initEmbedded() {
  return init(td_model_bin, (size_t)(td_model_bin_end - td_model_bin), td_vocab_bin,
              (size_t)(td_vocab_bin_end - td_vocab_bin));
}
#else
bool initEmbedded() { return false; }
#endif

bool initPartition(const char* label) {
  const esp_partition_t* part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
  if (!part) { ESP_LOGE("tinydecide", "no data partition named '%s'", label); return false; }
  const void* ptr = nullptr;
  esp_partition_mmap_handle_t handle;
  if (esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &ptr, &handle) != ESP_OK) {
    ESP_LOGE("tinydecide", "could not map partition '%s' (%u bytes)", label, (unsigned)part->size);
    return false;
  }
  const uint8_t* base = (const uint8_t*)ptr;
  const size_t vo = (tdm::MODEL_BYTES + 15) & ~(size_t)15;
  if (part->size < vo + 12 || !init(base, tdm::MODEL_BYTES, base + vo, part->size - vo)) {
    ESP_LOGE("tinydecide", "partition '%s' does not hold this build's tinydecide-esp32.bin", label);
    esp_partition_munmap(handle);
    return false;
  }
  return true;
}
#endif

#ifdef TD_TEST
// Host tests only: the token ids of the last answer() call.
static uint16_t g_lastIds[tdm::TS_MAX + MAX_QUESTIONS * tdm::Q_MAX];
static int g_lastT = 0;
int debugLastIds(const uint16_t** ids) { *ids = g_lastIds; return g_lastT; }
#endif

Status answer(const char* state, const Question* qs, int nq, Answer* out, Info* info, int state_max) {
  return answer(state, state ? strlen(state) : 0, qs, nq, out, info, state_max);
}

Status answer(const char* state, size_t state_len, const Question* qs, int nq, Answer* out, Info* info,
              int state_max) {
  if (!g_m) return NOT_READY;
  if (!state || !out) return BAD_ARGS;
  const Status bad = checkArgs(qs, nq);
  if (bad != OK) return bad;
  const uint32_t t0 = now_ms();
  if (state_max < 1) state_max = 1;
  if (state_max > STATE_MAX) state_max = STATE_MAX;

  Req* r = (Req*)malloc(sizeof(Req));
  if (!r) return NO_MEMORY;
  // state tokens (the reference reads up to ts_max - 1 = 127)
  static uint16_t st[tdm::TS_MAX];
  static int32_t sa[tdm::TS_MAX], sb[tdm::TS_MAX];
  const int ns = tokenize(state, state_len, st, tdm::TS_MAX, sa, sb);
  // question blocks: lengths first, so the sequence buffers can be sized exactly
  int qlen = 0;
  static uint16_t qids[tdm::Q_MAX];
  for (int k = 0; k < nq; k++) {
    const int n = questionBlock(qs[k], qids, tdm::Q_MAX, nullptr, nullptr);
    if (n > tdm::Q_MAX) { free(r); return QUESTION_TOO_LONG; }
    qlen += n;
  }
  const int Tmax = 1 + tdm::TS_MAX + qlen;
  uint8_t* seq = (uint8_t*)malloc((size_t)Tmax * 5);
  if (!seq) { free(r); return NO_MEMORY; }
  r->ids = (uint16_t*)seq;
  r->pos = r->ids + Tmax;
  r->blk = (uint8_t*)(r->pos + Tmax);
  r->nq = nq;

  // When the heap is too fragmented for the whole state, keep fewer of its tokens and try again.
  Scratch mem = {};
  for (int keepMax = state_max;; keepMax -= 8) {
    if (keepMax < 1) keepMax = 1;
    const int keep = ns < keepMax ? ns : keepMax;
    r->truncated = ns > keep;
    r->ids[0] = tdm::SP_STATE; r->pos[0] = 0; r->blk[0] = 0;
    for (int i = 0; i < keep; i++) {
      r->ids[1 + i] = st[i]; r->pos[1 + i] = (uint16_t)(1 + i); r->blk[1 + i] = 0;
      r->sa[i] = sa[i]; r->sb[i] = sb[i];
    }
    r->S = 1 + keep;
    int T = r->S;
    for (int k = 0; k < nq; k++) {
      int ansLocal = 0;
      const int n = questionBlock(qs[k], r->ids + T, tdm::Q_MAX, r->opt[k], &ansLocal);
      r->qs[k + 1] = T; r->qe[k + 1] = T + n;
      for (int j = 0; j < n; j++) { r->pos[T + j] = (uint16_t)(tdm::P_Q + j); r->blk[T + j] = (uint8_t)(k + 1); }
      for (int i = 0; i < qs[k].n_options; i++) r->opt[k][i] += T;
      r->ans[k] = T + ansLocal;
      T += n;
    }
    r->T = T;
    if (allocScratch(mem, T)) break;
    if (keep <= 1 || keepMax <= 1) { free(seq); free(r); return NO_MEMORY; }
  }
#ifdef TD_TEST
  g_lastT = r->T < (int)(sizeof(g_lastIds) / sizeof(g_lastIds[0])) ? r->T : (int)(sizeof(g_lastIds) / sizeof(g_lastIds[0]));
  memcpy(g_lastIds, r->ids, g_lastT * sizeof(uint16_t));
#endif
  encode(*r, mem);
  const float* x = (const float*)mem.a;
  for (int k = 0; k < nq; k++) {
    Answer& a = out[k];
    memset(&a, 0, sizeof(a));
    a.type = qs[k].type;
    if (a.type == CHOICE || a.type == SCORE) headChoice(*r, x, k, qs[k], a);
    else if (a.type == NOUL) headNoul(*r, x, k, qs[k], a);
    else headSpan(*r, x, k, state, a);
  }
  if (info) {
    info->tokens = r->T;
    info->state_tokens = r->S;
    info->truncated = r->truncated;
  }
  freeScratch(mem);
  free(seq);
  free(r);
  if (info) info->ms = now_ms() - t0;
#ifdef ESP_PLATFORM
  ESP_LOGD("tinydecide", "%d questions, %d tokens, %lu ms", nq, info ? info->tokens : 0, (unsigned long)(now_ms() - t0));
#endif
  return OK;
}

}  // namespace td
