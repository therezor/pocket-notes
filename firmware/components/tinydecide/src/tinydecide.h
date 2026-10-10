// TinyDecide for ESP32: answers typed questions about one short text with the 4-bit device build
// (TheREZOR/TinyDecide, model.bin at the repo root).
//
// Reference implementation: tinydecide.js (TinyDecide.answer). The device engine differs only in
// arithmetic: activations are quantized to int8 per 32-element block before every Q4 matmul (the
// ESP32-S3 PIE kernel's format), so probabilities match the JS engine to about 1e-2.
//
// It builds for the ESP32-S3 (PIE kernel, both cores), other ESP32 chips (scalar kernel) and the
// host (scalar kernel), so the host conformance test runs the same code as the device.
//
// Not reentrant: one answer() at a time.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace td {

enum Type : uint8_t { CHOICE = 0, NOUL = 1, SCORE = 2, SPAN = 3 };

enum Status : uint8_t {
  OK = 0,
  NOT_READY,          // init() was not called or failed
  BAD_ARGS,           // no question text, choice / score without 2..32 options, noul / span with
                      // options, too many questions
  QUESTION_TOO_LONG,  // a question block is longer than the model's 192-token limit
  NO_MEMORY,          // not enough heap even for an 8-token state
};
const char* statusText(Status s);

constexpr int MAX_OPTIONS = 32;    // choice and score take 2..32 options
constexpr int MAX_QUESTIONS = 16;
constexpr int QDIM = 128;          // length of Answer::qvec
constexpr int STATE_MAX = 127;     // text tokens the model reads; the rest is dropped (Info::truncated)

// Corrections for one question (see the README): one prototype per option, built from stored
// examples. vec is [n_options * QDIM] (2 rows for noul: false, true), cnt[n_options], center[QDIM].
// With center == nullptr the cosines are centred on the count-weighted mean of the prototypes, as
// tinydecide.js does.
struct Protos {
  const float* vec;
  const int*   cnt;
  const float* center;
  float        lam = 1.0f;   // trust weight (makeProtos' lam); 1 when you have none
};

struct Question {
  Type               type;
  const char*        text;
  const char* const* options = nullptr;   // choice: the options; score: the levels, lowest first
  int                n_options = 0;
  const Protos*      protos = nullptr;    // optional, choice / score / noul
  const float*       bias = nullptr;      // optional, added to the zero-shot logits (z0 scale):
                                          // [n_options] for choice / score, [1] (log-odds) for noul
};

struct Answer {
  Type  type;
  int   n;                      // options (choice / score), 2 for noul
  int   pick;                   // choice / score: the most likely option
  float confidence;             // choice / score: 1 - normalised entropy
  float score;                  // score: expected level, 0 = first, 1 = last
  float p;                      // noul: P(true)
  float probs[MAX_OPTIONS];     // choice / score
  float p_present;              // span: P(the message contains an answer)
  float p_span;                 // span: probability of the chosen start and end
  int   tok[2];                 // span: first and last state token
  int   start, end;             // span: the answer is state[start, end) in bytes; start == end: empty
  float z0[MAX_OPTIONS];        // zero-shot logits / temperature, stored with a correction
  float qvec[QDIM];             // example vector, stored with a correction
};

struct Info {
  int      tokens;              // total sequence length
  int      state_tokens;        // state tokens including the <|state|> marker
  bool     truncated;           // the state was longer than the tokens kept
  uint32_t ms;
};

// model: model.bin, vocab: vocab.bin (tools/pack.mjs). Both must stay valid and readable; model must
// be 16-byte aligned (the PIE kernel loads 128-bit words). Returns false if they do not match the
// build this engine was generated for (td_meta.h).
bool init(const uint8_t* model, size_t model_len, const uint8_t* vocab, size_t vocab_len);

#ifdef ESP_PLATFORM
// init() with the copies embedded in the app image (CONFIG_TINYDECIDE_EMBED_MODEL, ESP-IDF only).
bool initEmbedded();
// init() from a flash data partition holding tinydecide-esp32.bin (tools/pack.mjs --image),
// memory-mapped, so the app image stays small. Works with ESP-IDF and Arduino.
bool initPartition(const char* label = "tinydecide");
#endif

// Answer nq questions about `state` in one pass. out[nq] is filled in order. state_max caps the state
// tokens (1..127); when the heap is too small the engine keeps fewer and sets info->truncated.
// The first form reads state up to its NUL; the second reads state_len bytes, NUL bytes included
// (they are dropped, as the reference drops U+0000).
Status answer(const char* state, const Question* qs, int nq, Answer* out, Info* info = nullptr,
              int state_max = STATE_MAX);
Status answer(const char* state, size_t state_len, const Question* qs, int nq, Answer* out,
              Info* info = nullptr, int state_max = STATE_MAX);

// WordPiece ids of `text` (the reference tokenizer, UTF-8 in). Writes at most max ids and returns
// the count; starts/ends (optional) get each token's byte range in text.
int tokenize(const char* text, uint16_t* ids, int max, int32_t* starts = nullptr, int32_t* ends = nullptr);
int tokenize(const char* text, size_t len, uint16_t* ids, int max, int32_t* starts = nullptr,
             int32_t* ends = nullptr);

// Bytes of scratch heap answer() needs for a sequence of T tokens (about 2.2 KB per token).
size_t scratchBytes(int T);

// Sequence length of answer() for these questions and a state of state_tokens text tokens,
// or -1 if a question is too long.
int requestTokens(const Question* qs, int nq, int state_tokens);

}  // namespace td

#if defined(__XTENSA__)
// Q4_0 row-planar dot product against Q8_0 activations (dot_q4_pie.S, ESP32-S3 only).
extern "C" float dot_q4q8_pie(const uint8_t* nib, const int8_t* xq, const uint8_t* scales,
                              const float* xs, int nb);
// The same for ntile * 4 tokens xqs bytes apart (their scales xqs / 32 floats apart); out[4 * ntile].
extern "C" void dot4_q4q8_pie(const uint8_t* nib, const uint8_t* scales, const int8_t* xq, const float* xs,
                              int nb, int xqs, float* out, int ntile);
#endif
