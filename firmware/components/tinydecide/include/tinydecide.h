// TinyDecide device engine: answers a plain-language choice question about one short text.
//
// Reference implementation: model/tinydecide.js (TinyDecide.answer, choice questions). The device
// build differs only in arithmetic: activations are quantized to int8 per 32-element block before
// every Q4 matmul (the PIE kernel's format), so probabilities match the JS engine to ~1e-2.
//
// Builds for the ESP32-S3 (PIE kernel, both cores) and for the host (scalar kernel), so the parity
// test in host/engine_test.cpp runs the exact same code as the device.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define TD_MODEL_VARIANT "S768"   // model/model.bin, see model/SOURCE.md

namespace td {

constexpr int MAX_OPTIONS = 16;
constexpr int QDIM = 128;           // length of Choice::qvec (the h.P prototype projection)
constexpr int STATE_MAX = 40;       // text tokens kept from the note (~30 words); the rest is dropped

// Learned per-option prototypes (see host/learn.mjs). vec is [n_options * QDIM], cnt[n_options].
struct Protos {
  const float* vec;
  const int*   cnt;
  const float* center;   // [QDIM]
  float        lam;
};

struct Choice {
  int      n;                       // number of options
  int      pick;
  float    confidence;              // 1 - normalised entropy
  float    probs[MAX_OPTIONS];
  float    z0[MAX_OPTIONS];         // zero-shot logits / temperature (stored with examples)
  float    qvec[QDIM];              // example vector for learning
  int      tokens;                  // total sequence length
  bool     truncated;               // note text was longer than STATE_MAX tokens
  uint32_t ms;
};

// model: model/model.bin, vocab: model/vocab.bin (host/pack_model.mjs). Both must stay valid.
// The model must be 16-byte aligned (the PIE kernel loads 128-bit words).
bool init(const uint8_t* model, size_t model_len, const uint8_t* vocab, size_t vocab_len);

#ifdef ESP_PLATFORM
// init() with the copies embedded in flash by model_data.S.
bool initEmbedded();
#endif

// Lowercase ASCII WordPiece. Returns the number of ids written (at most max).
int tokenize(const char* text, uint16_t* ids, int max);

// Answer one choice question about `text`. bias (optional, [n]) is added to the zero-shot logits
// (z0 scale) before protos, which is how the app's text rules (phone number -> Contacts) enter.
// Returns false when out of memory or when the request is longer than the model accepts.
bool choice(const char* text, const char* question, const char* const* options, int n,
            const float* bias, const Protos* protos, Choice* out);

// Bytes choice() would allocate for a sequence of T tokens.
size_t scratchBytes(int T);

// Highest number of tokens one choice() call can have with this question and options.
int requestTokens(const char* question, const char* const* options, int n);

}  // namespace td

// v3 Q4_0 row-planar dot product against Q8_0 activations (dot_q4_pie.S, from cardputer_ai).
extern "C" float dot_q4q8_pie(const uint8_t* nib, const int8_t* xq, const uint8_t* scales,
                              const float* xs, int nb);
