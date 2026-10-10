#include "ai.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <initializer_list>
#include <map>
#include <vector>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "settings.h"
#include "store.h"
#include "tinydecide.h"

static const char* TAG = "ai";

namespace ai {

// The question the model answers; categories.txt supplies the options. Picked with
// host/notes_eval.mjs (best of the wordings tried there).
static const char* QUESTION = "What kind of note is this?";
static constexpr int PER_CAT = 8;            // examples per category (the prototype buckets top out at >8)
static constexpr int K = 16;                 // most categories; also the z[] size in .learn.bin, keep it
static constexpr int STATE_TOKENS = 40;      // note tokens the model reads (~30 words); the rest is dropped
static_assert(K <= td::MAX_OPTIONS, "more categories than the engine takes");

struct Job {
  enum Kind : uint8_t { SUGGEST, EMBED } kind;
  uint32_t seq;          // SUGGEST: request number; EMBED: note id
  uint32_t sig;          // category signature the options belong to
  std::string text;
  std::vector<std::string> opts;
  float bias[K];
  bool hasProtos;
  std::vector<float> pvec, pcenter;
  std::vector<int> pcnt;
  float plam;
  // result
  bool ok;
  td::Answer out;
  td::Info info;
};

struct Example {
  uint32_t id;
  float v[td::QDIM];
  float z[K];            // zero-shot logits + rule bias, for the category set `sig`
};

static bool s_ok = false;
static QueueHandle_t s_in = nullptr, s_out = nullptr;
static uint32_t s_seq = 0;               // latest suggestion request
static bool s_suggestBusy = false;
static bool s_embedBusy = false;
static bool s_have = false;
static int s_pick = 0;
static float s_conf = 0;
static float s_probs[K];
static uint32_t s_lastMs = 0;
static int s_lastTokens = 0;

static uint32_t s_sig = 0;               // signature the cache holds
static std::map<uint32_t, Example> s_cache;
static bool s_cacheDirty = false;
static std::vector<uint32_t> s_failed;   // ids the engine could not embed (too long, OOM)
static char s_status[24] = "";

// ---- worker ---------------------------------------------------------------

static void worker(void*) {
  for (;;) {
    Job* j = nullptr;
    if (xQueueReceive(s_in, &j, portMAX_DELAY) != pdTRUE || !j) continue;
    std::vector<const char*> op;
    for (auto& o : j->opts) op.push_back(o.c_str());
    td::Protos p;
    if (j->hasProtos) { p.vec = j->pvec.data(); p.cnt = j->pcnt.data(); p.center = j->pcenter.data(); p.lam = j->plam; }
    td::Question q{td::CHOICE, QUESTION, op.data(), (int)op.size(), j->hasProtos ? &p : nullptr, j->bias};
    const td::Status st = td::answer(j->text.c_str(), &q, 1, &j->out, &j->info, STATE_TOKENS);
    j->ok = st == td::OK;
    if (!j->ok)
      ESP_LOGW(TAG, "pass failed: %s (largest free block %u)", td::statusText(st),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    xQueueSend(s_out, &j, portMAX_DELAY);
  }
}

void begin() {
  s_ok = td::initEmbedded();
  if (!s_ok) { ESP_LOGE(TAG, "engine init failed"); return; }
  s_in = xQueueCreate(4, sizeof(Job*));
  s_out = xQueueCreate(4, sizeof(Job*));
  xTaskCreatePinnedToCore(worker, "td", 8192, nullptr, 3, nullptr, 1);   // above the UI (1)
  ESP_LOGI(TAG, "engine ready, model %s", MODEL_VARIANT);
}

bool ok() { return s_ok; }
uint32_t lastMs() { return s_lastMs; }
int lastTokens() { return s_lastTokens; }

// ---- text rules -------------------------------------------------------------

static bool hasEmail(const char* s) {
  const char* at = strchr(s, '@');
  return at && at > s && strchr(at, '.') && at[1] && at[1] != '.';
}
static int digits(const char* s) { int n = 0; for (; *s; ++s) if (*s >= '0' && *s <= '9') n++; return n; }

// 6pm, 6 pm, 10:30, 9:15am
static bool hasClockTime(const char* s) {
  for (const char* p = s; *p; ++p) {
    if (!(*p >= '0' && *p <= '9') || (p > s && ((p[-1] >= '0' && p[-1] <= '9') || p[-1] == ':'))) continue;
    const char* q = p;
    int d = 0;
    while (*q >= '0' && *q <= '9') { q++; d++; }
    if (d > 2) continue;
    bool colon = false;
    if (*q == ':' && q[1] >= '0' && q[1] <= '5' && q[2] >= '0' && q[2] <= '9' && !(q[3] >= '0' && q[3] <= '9')) { q += 3; colon = true; }
    const char* r = q;
    if (*r == ' ') r++;
    const bool ampm = (r[0] == 'a' || r[0] == 'p' || r[0] == 'A' || r[0] == 'P') && (r[1] == 'm' || r[1] == 'M') &&
                      !((r[2] >= 'a' && r[2] <= 'z') || (r[2] >= 'A' && r[2] <= 'Z'));
    if (colon || ampm) return true;
  }
  return false;
}

// Evidence the model is weak on, as logit bonuses (z0 scale; host/notes_eval.mjs measured these).
static void rulesBias(const std::string& text, float* bias) {
  for (int i = 0; i < K; i++) bias[i] = 0;
  const char* s = text.c_str();
  const bool contact = hasEmail(s) || digits(s) >= 7;
  const bool clock = hasClockTime(s);
  for (size_t i = 0; i < store::cats.size() && i < (size_t)K; i++) {
    if (store::cats[i].phone && contact) bias[i] += 3.0f;
    if (store::cats[i].time && clock) bias[i] += 1.5f;
  }
}

// ---- learning -----------------------------------------------------------------

static std::string cachePath() { return store::root() + "/.learn.bin"; }

static void saveCache() {
  FILE* f = fopen(cachePath().c_str(), "wb");
  if (!f) return;
  const uint32_t hdr[3] = {0x4E4C5450u /* PTLN */, s_sig, (uint32_t)s_cache.size()};
  fwrite(hdr, sizeof(hdr), 1, f);
  for (auto& kv : s_cache) fwrite(&kv.second, sizeof(Example), 1, f);
  fclose(f);
  s_cacheDirty = false;
}

static void loadCache() {
  s_cache.clear();
  s_failed.clear();
  s_sig = store::catSignature();
  FILE* f = fopen(cachePath().c_str(), "rb");
  if (!f) return;
  uint32_t hdr[3];
  if (fread(hdr, sizeof(hdr), 1, f) == 1 && hdr[0] == 0x4E4C5450u && hdr[1] == s_sig && hdr[2] < 4096) {
    Example e;
    for (uint32_t i = 0; i < hdr[2] && fread(&e, sizeof(e), 1, f) == 1; i++) s_cache[e.id] = e;
  }
  fclose(f);
  ESP_LOGI(TAG, "learning cache: %d examples", (int)s_cache.size());
}

// The notes that should be examples right now: per category, the PER_CAT newest eligible notes.
static void wanted(std::vector<std::vector<int>>& per) {
  per.assign(store::cats.size(), {});
  for (size_t i = 0; i < store::notes.size(); i++) {
    const store::Note& n = store::notes[i];
    if (n.id < settings::v.learnFrom || n.cat >= per.size()) continue;
    if (std::find(s_failed.begin(), s_failed.end(), n.id) != s_failed.end()) continue;
    per[n.cat].push_back((int)i);
  }
  for (auto& v : per) {
    std::sort(v.begin(), v.end(), [](int a, int b) { return store::notes[a].id > store::notes[b].id; });
    if (v.size() > (size_t)PER_CAT) v.resize(PER_CAT);
  }
}

static float cosC(const float* a, const float* b, const float* c) {
  float d = 0, na = 0, nb = 0;
  for (int i = 0; i < td::QDIM; i++) { float x = a[i] - c[i], y = b[i] - c[i]; d += x * y; na += x * x; nb += y * y; }
  float den = sqrtf(na * nb);
  return d / (den > 0 ? den : 1e-12f);
}
static int bucketK(int c) { int b = 0; for (int e : {1, 2, 4, 8}) if (c > e) b++; return b; }
static const float BETA[5] = {5.576540470123291f, 4.381887435913086f, 4.137119770050049f, 2.71045184135437f, 5.0f};

// Port of protosFor / lambdaFor (host/learn.mjs).
static bool buildProtos(Job& j) {
  const int nc = (int)store::cats.size();
  std::vector<std::vector<const Example*>> lists(nc);
  std::vector<std::vector<int>> per;
  wanted(per);
  int total = 0;
  for (int c = 0; c < nc; c++)
    for (int idx : per[c]) {
      auto it = s_cache.find(store::notes[idx].id);
      if (it != s_cache.end()) { lists[c].push_back(&it->second); total++; }
    }
  if (!total) return false;
  const int D = td::QDIM;
  j.pcenter.assign(D, 0.0f);
  j.pvec.assign((size_t)nc * D, 0.0f);
  j.pcnt.assign(nc, 0);
  for (int c = 0; c < nc; c++)
    for (const Example* e : lists[c]) for (int i = 0; i < D; i++) j.pcenter[i] += e->v[i] / total;
  for (int c = 0; c < nc; c++) {
    j.pcnt[c] = (int)lists[c].size();
    for (const Example* e : lists[c]) for (int i = 0; i < D; i++) j.pvec[(size_t)c * D + i] += e->v[i] / lists[c].size();
  }
  // lambda: leave-one-out log-likelihood over the examples themselves.
  float lam = 0.25f;
  if (total >= 2) {
    const float grid[5] = {0, 0.25f, 0.5f, 1, 2};
    double bestLL = -1e30;
    std::vector<float> mean(D);
    for (float g : grid) {
      double ll = 0;
      for (int k = 0; k < nc; k++)
        for (size_t jj = 0; jj < lists[k].size(); jj++) {
          const Example* e = lists[k][jj];
          float z[K];
          for (int c = 0; c < nc; c++) {
            const int cnt = (int)lists[c].size() - (c == k ? 1 : 0);
            float t = 0;
            if (cnt > 0) {
              // mean of category c without e
              for (int i = 0; i < D; i++) {
                float s = j.pvec[(size_t)c * D + i] * lists[c].size();
                if (c == k) s -= e->v[i];
                mean[i] = s / cnt;
              }
              t = BETA[bucketK(cnt)] * cosC(e->v, mean.data(), j.pcenter.data());
            }
            z[c] = e->z[c] + g * t;
          }
          float mx = z[0];
          for (int c = 1; c < nc; c++) mx = fmaxf(mx, z[c]);
          double se = 0;
          for (int c = 0; c < nc; c++) se += exp(z[c] - mx);
          ll += z[k] - (mx + log(se));
        }
      if (ll > bestLL + 1e-9) { bestLL = ll; lam = g; }
    }
  }
  j.plam = lam;
  j.hasProtos = true;
  return true;
}

static Job* newJob(Job::Kind k, const std::string& text) {
  Job* j = new Job();
  j->kind = k;
  j->sig = store::catSignature();
  j->text = text;
  for (auto& c : store::cats) j->opts.push_back(c.option());
  rulesBias(text, j->bias);
  j->hasProtos = false;
  j->plam = 0;
  j->ok = false;
  return j;
}

void categoriesChanged() {
  if (store::catSignature() == s_sig) return;
  s_cache.clear();
  s_failed.clear();
  s_sig = store::catSignature();
  s_cacheDirty = true;
}

void forget() {
  settings::v.learnFrom = settings::peekId();
  settings::save();
  s_cache.clear();
  s_failed.clear();
  s_cacheDirty = true;
}

const char* status() { return s_status; }

// ---- suggestions ----------------------------------------------------------------

void suggest(const std::string& text) {
  s_have = false;
  if (!s_ok) return;
  Job* j = newJob(Job::SUGGEST, text);
  j->seq = ++s_seq;
  buildProtos(*j);
  if (xQueueSend(s_in, &j, 0) != pdTRUE) { delete j; return; }
  s_suggestBusy = true;
}

void cancel() { ++s_seq; s_have = false; s_suggestBusy = false; }
bool pending() { return s_suggestBusy; }

bool suggestion(int& pick, float& conf, const float** probs) {
  if (!s_have) return false;
  pick = s_pick; conf = s_conf;
  if (probs) *probs = s_probs;
  return true;
}

static std::string s_root;   // notes folder the cache belongs to

void tick() {
  if (!s_ok) return;
  if (s_root != store::root()) { s_root = store::root(); loadCache(); }
  if (store::catSignature() != s_sig) categoriesChanged();

  Job* j = nullptr;
  while (xQueueReceive(s_out, &j, 0) == pdTRUE && j) {
    if (j->ok) { s_lastMs = j->info.ms; s_lastTokens = j->info.tokens; }
    if (j->kind == Job::SUGGEST) {
      if (j->seq == s_seq) {
        s_suggestBusy = false;
        if (j->ok && j->sig == store::catSignature()) {
          s_pick = j->out.pick; s_conf = j->out.confidence;
          memcpy(s_probs, j->out.probs, sizeof(s_probs));
          s_have = true;
          ESP_LOGI(TAG, "suggest %d (%.2f) in %u ms, %d tokens", s_pick, s_conf, (unsigned)j->info.ms, j->info.tokens);
        }
      }
    } else {
      s_embedBusy = false;
      if (j->sig == s_sig) {
        if (j->ok) {
          Example e;
          e.id = j->seq;
          memcpy(e.v, j->out.qvec, sizeof(e.v));
          for (int i = 0; i < K; i++) e.z[i] = i < j->out.n ? j->out.z0[i] + j->bias[i] : 0;
          s_cache[e.id] = e;
          s_cacheDirty = true;
        } else {
          s_failed.push_back(j->seq);
        }
      }
    }
    delete j;
  }

  // Schedule the next example, one at a time so a suggestion never waits long.
  std::vector<std::vector<int>> per;
  wanted(per);
  int need = 0, have = 0, next = -1;
  for (auto& v : per)
    for (int idx : v) {
      need++;
      if (s_cache.count(store::notes[idx].id)) have++;
      else if (next < 0) next = idx;
    }
  // Drop cached examples that are no longer wanted (deleted or pushed out by newer notes).
  if ((int)s_cache.size() > have) {
    std::map<uint32_t, Example> keep;
    for (auto& v : per) for (int idx : v) { auto it = s_cache.find(store::notes[idx].id); if (it != s_cache.end()) keep[it->first] = it->second; }
    s_cache.swap(keep);
    s_cacheDirty = true;
  }
  if (next >= 0 && !s_embedBusy && !s_suggestBusy) {
    Job* e = newJob(Job::EMBED, store::notes[next].text);
    e->seq = store::notes[next].id;
    if (xQueueSend(s_in, &e, 0) == pdTRUE) s_embedBusy = true; else delete e;
  }
  if (have < need) snprintf(s_status, sizeof(s_status), "learning %d/%d", have, need);
  else s_status[0] = 0;
  if (s_cacheDirty && !s_embedBusy) saveCache();
}

}  // namespace ai
