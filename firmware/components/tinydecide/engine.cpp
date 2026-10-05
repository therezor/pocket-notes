// TinyDecide device engine. See include/tinydecide.h and model/tinydecide.js (the reference).
//
// Memory plan ("streamed kernel", ../tinydecide docs/PLAN.md): one scratch block per call,
// ~2.2 KB per token. Each layer keeps the fp32 residual x plus an int8 copy of the layer input;
// attention runs one head at a time with W_o accumulated straight into x, and the FFN runs in
// 64-neuron chunks with fc2 accumulated into x. Every weight row is still read once per layer and
// applied to all tokens while it is hot in cache; rows are split across both cores on the device.
#include "tinydecide.h"
#include "td_meta.h"

#include <math.h>
#include <stdlib.h>

#include <initializer_list>
#include <string.h>

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
static uint32_t now_ms() { return (uint32_t)(esp_timer_get_time() / 1000); }
#else
#include <chrono>
static uint32_t now_ms() {
  using namespace std::chrono;
  return (uint32_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
#endif

namespace td {

static const uint8_t* g_m = nullptr;     // model.bin
static const uint8_t* g_v = nullptr;     // vocab.bin
static uint32_t g_vn = 0;
static const uint32_t* g_voff = nullptr;
static const uint16_t* g_vid = nullptr;
static const char* g_vblob = nullptr;

static inline const float* F(uint32_t off) { return (const float*)(g_m + off); }

// ============================================================================
//  Tokenizer: lowercase ASCII WordPiece (tinydecide.js WordPiece.encode, ASCII path)
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

static bool isPunct(unsigned char c) {
  return (c >= 33 && c <= 47) || (c >= 58 && c <= 64) || (c >= 91 && c <= 96) || (c >= 123 && c <= 126);
}

// One whitespace/punctuation-delimited word -> pieces. Returns ids written.
static int encodeWord(const char* w, int len, uint16_t* ids, int max) {
  if (max <= 0) return 0;
  if (len > tdm::MAX_CHARS) { ids[0] = tdm::SP_UNK; return 1; }
  uint16_t tmp[64];
  int n = 0, start = 0;
  char buf[tdm::MAX_CHARS + 3];
  while (start < len) {
    int end = len, got = -1;
    while (start < end) {
      int bl = 0;
      if (start > 0) { buf[0] = '#'; buf[1] = '#'; bl = 2; }
      memcpy(buf + bl, w + start, end - start);
      bl += end - start;
      got = vocabFind(buf, bl);
      if (got >= 0) break;
      end--;
    }
    if (got < 0 || n >= (int)(sizeof(tmp) / sizeof(tmp[0]))) { ids[0] = tdm::SP_UNK; return 1; }
    tmp[n++] = (uint16_t)got;
    start = end;
  }
  if (n > max) n = max;
  memcpy(ids, tmp, n * sizeof(uint16_t));
  return n;
}

int tokenize(const char* text, uint16_t* ids, int max) {
  int n = 0;
  char word[tdm::MAX_CHARS + 1];
  int wl = 0;
  bool longWord = false;
  auto flush = [&]() {
    if (wl || longWord) {
      if (longWord) { if (n < max) ids[n++] = tdm::SP_UNK; }
      else n += encodeWord(word, wl, ids + n, max - n);
    }
    wl = 0; longWord = false;
  };
  for (const unsigned char* p = (const unsigned char*)text; *p && n < max; ++p) {
    unsigned char c = *p;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { flush(); continue; }
    if (c < 32 || c == 127) continue;                 // control characters are dropped
    if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
    if (isPunct(c)) {
      flush();
      if (n < max) { char s = (char)c; n += encodeWord(&s, 1, ids + n, max - n); }
      continue;
    }
    if (wl < tdm::MAX_CHARS) word[wl++] = (char)c; else longWord = true;
  }
  flush();
  return n < max ? n : max;
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
#if defined(__XTENSA__)
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

// Symmetric int8 per 32-element block, one fp32 scale each (llama.cpp Q8_0).
static void quant8(const float* x, int8_t* xq, float* xs, int n) {
  for (int b = 0; b < n / 32; b++) {
    float mx = 0.0f;
    for (int i = 0; i < 32; i++) { float a = fabsf(x[i]); if (a > mx) mx = a; }
    float s = mx / 127.0f, inv = s > 0 ? 1.0f / s : 0.0f;
    xs[b] = s;
    for (int i = 0; i < 32; i++) xq[i] = (int8_t)lrintf(x[i] * inv);
    x += 32; xq += 32;
  }
}

// out[t*ostride + (r - r0)] (=|+=) W[r, blk0*32 .. (blk0+nbk)*32) . x[t] + bias[r], for r in [r0, r1).
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
    for (int t = 0; t < a.T; t++) {
      float v = dotq(nib, a.xq + (size_t)t * a.xqs, sc, a.xs + (size_t)t * a.xss, a.nbk) + b;
      if (a.acc) o[(size_t)t * a.os] += v; else o[(size_t)t * a.os] = v;
    }
  }
}

#ifdef ESP_PLATFORM
// Persistent worker on the other core; per call it costs two semaphore operations.
static SemaphoreHandle_t s_go = nullptr, s_done = nullptr;
static MM s_job;
static int s_ja = 0, s_jb = 0;
static int s_core = -1;

static void worker(void*) {
  for (;;) {
    xSemaphoreTake(s_go, portMAX_DELAY);
    mmRows(s_job, s_ja, s_jb);
    xSemaphoreGive(s_done);
  }
}

static void mm(const MM& a, int r1) {
  if (!s_go) {
    s_go = xSemaphoreCreateBinary();
    s_done = xSemaphoreCreateBinary();
    s_core = xPortGetCoreID() == 0 ? 1 : 0;
    // Same priority as the UI task, so the UI keeps time-slicing on that core during inference.
    xTaskCreatePinnedToCore(worker, "td_mm", 3072, nullptr, 1, nullptr, s_core);
  }
  if (xPortGetCoreID() == s_core) { mmRows(a, a.r0, r1); return; }   // caller moved cores: run alone
  const int split = a.r0 + (r1 - a.r0) / 2;
  s_job = a; s_ja = split; s_jb = r1;
  xSemaphoreGive(s_go);
  mmRows(a, a.r0, split);
  xSemaphoreTake(s_done, portMAX_DELAY);
}
static void* scratchAlloc(size_t n) { return heap_caps_aligned_alloc(16, n, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL); }
static void scratchFree(void* p) { heap_caps_free(p); }
#else
static void mm(const MM& a, int r1) { mmRows(a, a.r0, r1); }
static void* scratchAlloc(size_t n) { return aligned_alloc(16, (n + 15) & ~(size_t)15); }
static void scratchFree(void* p) { free(p); }
#endif

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

static inline float gelu(float x) { return 0.5f * x * (1.0f + erff(x * 0.70710678118654752f)); }

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

// ============================================================================
//  Request layout (encodeRequest, one choice question)
// ============================================================================

struct Req {
  uint16_t ids[tdm::TS_MAX + tdm::Q_MAX];
  uint16_t pos[tdm::TS_MAX + tdm::Q_MAX];
  int T, S;                       // total tokens; state tokens incl. the <|state|> marker
  int ans;
  int opt[MAX_OPTIONS];
  bool truncated;
};

// Question block: <|choice|> q <|sep|> (opt <|o|>)* <|ans|>. Returns its length, or -1 if too long.
static int questionBlock(const char* question, const char* const* options, int n, uint16_t* ids,
                         int* optLocal, int* ansLocal) {
  int m = 0;
  const int cap = tdm::Q_MAX;
  ids[m++] = tdm::SP_CHOICE;
  m += tokenize(question, ids + m, cap - m);
  if (n > 0) {
    if (m >= cap) return -1;
    ids[m++] = tdm::SP_SEP;
    for (int i = 0; i < n; i++) {
      m += tokenize(options[i], ids + m, cap - m);
      if (m >= cap) return -1;
      if (optLocal) optLocal[i] = m;
      ids[m++] = tdm::SP_O;
    }
  }
  if (m >= cap) return -1;
  if (ansLocal) *ansLocal = m;
  ids[m++] = tdm::SP_ANS;
  return m;
}

static bool buildRequest(const char* text, const char* question, const char* const* options, int n, Req& r) {
  uint16_t st[tdm::TS_MAX];
  int ns = tokenize(text, st, tdm::TS_MAX);
  const int keep = ns < STATE_MAX ? ns : STATE_MAX;
  r.truncated = ns > keep;
  r.ids[0] = tdm::SP_STATE; r.pos[0] = 0;
  for (int i = 0; i < keep; i++) { r.ids[1 + i] = st[i]; r.pos[1 + i] = (uint16_t)(1 + i); }
  r.S = 1 + keep;
  int optLocal[MAX_OPTIONS], ansLocal = 0;
  int qn = questionBlock(question, options, n, r.ids + r.S, optLocal, &ansLocal);
  if (qn < 0) return false;
  for (int j = 0; j < qn; j++) r.pos[r.S + j] = (uint16_t)(tdm::P_Q + j);
  for (int i = 0; i < n; i++) r.opt[i] = r.S + optLocal[i];
  r.ans = r.S + ansLocal;
  r.T = r.S + qn;
  return true;
}

int requestTokens(const char* question, const char* const* options, int n) {
  uint16_t ids[tdm::Q_MAX];
  int qn = questionBlock(question, options, n, ids, nullptr, nullptr);
  return qn < 0 ? -1 : 1 + STATE_MAX + qn;
}

// ============================================================================
//  Encoder
// ============================================================================

size_t scratchBytes(int T) {
  const size_t f = sizeof(float);
  return (size_t)T * (tdm::D * f            // x
                      + 3 * tdm::DHEAD * f    // Qh, Kh, Vh (also the embedding / FFN chunk buffers)
                      + tdm::D                // xq
                      + tdm::DHEAD            // hq
                      + (tdm::D / 32) * f     // xs
                      + (tdm::DHEAD / 32) * f // hs
                      + f)                    // attention scores
         + 64;
}

static void encode(const Req& r, uint8_t* mem) {
  const int T = r.T, S = r.S, D = tdm::D, DHd = tdm::DHEAD, E = tdm::EMB;
  float* x  = (float*)mem;                                  // [T, D]
  float* Qh = x + (size_t)T * D;                            // [T, 64]
  float* Kh = Qh + (size_t)T * DHd;
  float* Vh = Kh + (size_t)T * DHd;
  int8_t* xq = (int8_t*)(Vh + (size_t)T * DHd);             // [T, D]   (16-aligned: offsets are multiples of 64)
  int8_t* hq = xq + (size_t)T * D;                          // [T, 64]
  float* xs = (float*)(hq + (size_t)T * DHd);               // [T, D/32]
  float* hs = xs + (size_t)T * (D / 32);                    // [T, 2]
  float* sc = hs + (size_t)T * (DHd / 32);                  // [T]

  // ---- embeddings: word (Q4) + position (int8) + type0, LayerNorm, project to D
  float* raw = Qh;                                          // [T, E] spans Qh..Kh
  const int8_t* P = (const int8_t*)(g_m + tdm::POS.q);
  const float* Ps = F(tdm::POS.sc);
  const float* t0 = F(tdm::TYPE0);
  for (int t = 0; t < T; t++) {
    float* o = raw + (size_t)t * E;
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
    for (int t = 0; t < T; t++) quant8(x + (size_t)t * D, xq + (size_t)t * D, xs + (size_t)t * (D / 32), D);

    // ---- attention, one head at a time; W_o slice accumulated into the residual
    for (int h = 0; h < tdm::HEADS; h++) {
      const int r0 = h * DHd, r1 = r0 + DHd;
      mm({&B.q, 0, D / 32, xq, D, xs, D / 32, T, Qh, DHd, F(B.qb), false, r0}, r1);
      mm({&B.k, 0, D / 32, xq, D, xs, D / 32, T, Kh, DHd, F(B.kb), false, r0}, r1);
      mm({&B.v, 0, D / 32, xq, D, xs, D / 32, T, Vh, DHd, F(B.vb), false, r0}, r1);
      float out[64];
      for (int i = 0; i < T; i++) {
        // fusion "all", one question: state tokens see the state; question tokens see everything.
        const int nk = i < S ? S : T;
        const float* q = Qh + (size_t)i * DHd;
        float mx = -1e30f;
        for (int j = 0; j < nk; j++) {
          const float* k = Kh + (size_t)j * DHd;
          float s = 0;
          for (int c = 0; c < DHd; c++) s += q[c] * k[c];
          s *= scale;
          sc[j] = s;
          if (s > mx) mx = s;
        }
        float z = 0;
        for (int j = 0; j < nk; j++) { sc[j] = expf(sc[j] - mx); z += sc[j]; }
        const float iz = 1.0f / z;
        for (int c = 0; c < DHd; c++) out[c] = 0;
        for (int j = 0; j < nk; j++) {
          const float p = sc[j] * iz;
          const float* v = Vh + (size_t)j * DHd;
          for (int c = 0; c < DHd; c++) out[c] += p * v[c];
        }
        quant8(out, hq + (size_t)i * DHd, hs + (size_t)i * (DHd / 32), DHd);
      }
      mm({&B.o, r0 / 32, DHd / 32, hq, DHd, hs, DHd / 32, T, x, D, h == 0 ? F(B.ob) : nullptr, true, 0}, D);
    }
    for (int t = 0; t < T; t++) layernorm(x + (size_t)t * D, D, F(B.ln1w), F(B.ln1b), tdm::LN_EPS);

    // ---- FFN in 64-neuron chunks; fc2 slice accumulated into the residual
    for (int t = 0; t < T; t++) quant8(x + (size_t)t * D, xq + (size_t)t * D, xs + (size_t)t * (D / 32), D);
    float* Hc = Qh;
    for (int c0 = 0; c0 < tdm::FFN; c0 += DHd) {
      mm({&B.fc, 0, D / 32, xq, D, xs, D / 32, T, Hc, DHd, F(B.fcb), false, c0}, c0 + DHd);
      for (int t = 0; t < T; t++) {
        float* hrow = Hc + (size_t)t * DHd;
        for (int c = 0; c < DHd; c++) hrow[c] = gelu(hrow[c]);
        quant8(hrow, hq + (size_t)t * DHd, hs + (size_t)t * (DHd / 32), DHd);
      }
      mm({&B.fc2, c0 / 32, DHd / 32, hq, DHd, hs, DHd / 32, T, x, D, c0 == 0 ? F(B.fc2b) : nullptr, true, 0}, D);
    }
    for (int t = 0; t < T; t++) layernorm(x + (size_t)t * D, D, F(B.ln2w), F(B.ln2b), tdm::LN_EPS);
  }
}

// ============================================================================
//  Public API
// ============================================================================

bool init(const uint8_t* model, size_t model_len, const uint8_t* vocab, size_t vocab_len) {
  if (!model || model_len < tdm::MODEL_BYTES || ((uintptr_t)model & 15)) return false;
  if (!vocab || vocab_len < 12 || memcmp(vocab, "TDV1", 4) != 0) return false;
  uint32_t n, blob;
  memcpy(&n, vocab + 4, 4);
  memcpy(&blob, vocab + 8, 4);
  if (12 + 4 * (size_t)(n + 1) + 2 * (size_t)n + blob > vocab_len) return false;
  g_m = model;
  g_v = vocab;
  g_vn = n;
  g_voff = (const uint32_t*)(vocab + 12);
  g_vid = (const uint16_t*)(vocab + 12 + 4 * (n + 1));
  g_vblob = (const char*)(vocab + 12 + 4 * (n + 1) + 2 * n);
  return true;
}

#ifdef ESP_PLATFORM
extern "C" const uint8_t td_model_bin[], td_model_bin_end[], td_vocab_bin[], td_vocab_bin_end[];
bool initEmbedded() {
  return init(td_model_bin, (size_t)(td_model_bin_end - td_model_bin), td_vocab_bin,
              (size_t)(td_vocab_bin_end - td_vocab_bin));
}
#endif

static int bucketK(int c) { int b = 0; for (int e : {1, 2, 4, 8}) if (c > e) b++; return b; }

bool choice(const char* text, const char* question, const char* const* options, int n,
            const float* bias, const Protos* protos, Choice* out) {
  if (!g_m || n < 1 || n > MAX_OPTIONS) return false;
  const uint32_t t0 = now_ms();
  static Req r;                                   // ~1.3 KB; one inference at a time
  if (!buildRequest(text, question, options, n, r)) return false;
  uint8_t* mem = (uint8_t*)scratchAlloc(scratchBytes(r.T));
  if (!mem) return false;
  encode(r, mem);

  const int D = tdm::D, DH = tdm::DH;
  float* x = (float*)mem;
  const float* nw = F(tdm::H_NORM_W);
  const float* nb = F(tdm::H_NORM_B);
  float ha[256], ho[256], qa[128], ov[128];
  memcpy(ha, x + (size_t)r.ans * D, sizeof(ha));
  layernorm(ha, D, nw, nb, 1e-5f);
  mvI8(tdm::H_A0, ha, qa);
  mvI8(tdm::H_P, ha, out->qvec);
  const float s = expf(F(tdm::H_SCALE)[0]);
  const float Tt = tdm::TEMP[0];
  float L[MAX_OPTIONS];
  for (int i = 0; i < n; i++) {
    memcpy(ho, x + (size_t)r.opt[i] * D, sizeof(ho));
    layernorm(ho, D, nw, nb, 1e-5f);
    mvI8(tdm::H_O0, ho, ov);
    float d = 0;
    for (int c = 0; c < DH; c++) d += qa[c] * ov[c];
    out->z0[i] = s * d / sqrtf((float)DH) / Tt;
    L[i] = out->z0[i] + (bias ? bias[i] : 0.0f);
    if (protos && protos->cnt[i] > 0) {
      const float* c = protos->center;
      const float* pv = protos->vec + (size_t)i * QDIM;
      float dd = 0, na = 0, nb2 = 0;
      for (int j = 0; j < QDIM; j++) {
        const float a = out->qvec[j] - c[j], b = pv[j] - c[j];
        dd += a * b; na += a * a; nb2 += b * b;
      }
      const float cs = dd / ((sqrtf(na) > 0 ? sqrtf(na) : 1e-12f) * (sqrtf(nb2) > 0 ? sqrtf(nb2) : 1e-12f));
      L[i] += protos->lam * tdm::BETA[bucketK(protos->cnt[i])] * cs / Tt;
    }
  }
  scratchFree(mem);

  float mx = L[0];
  for (int i = 1; i < n; i++) if (L[i] > mx) mx = L[i];
  float Z = 0;
  for (int i = 0; i < n; i++) { out->probs[i] = expf(L[i] - mx); Z += out->probs[i]; }
  float H = 0;
  out->pick = 0;
  for (int i = 0; i < n; i++) {
    out->probs[i] /= Z;
    if (out->probs[i] > out->probs[out->pick]) out->pick = i;
    if (out->probs[i] > 0) H -= out->probs[i] * logf(out->probs[i]);
  }
  out->n = n;
  out->confidence = n > 1 ? 1.0f - H / logf((float)n) : 1.0f;
  out->tokens = r.T;
  out->truncated = r.truncated;
  out->ms = now_ms() - t0;
  return true;
}

}  // namespace td
