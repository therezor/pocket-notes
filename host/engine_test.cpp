// Host parity test: the firmware's C++ engine (scalar kernel) against the JS engine.
//   node host/make_ref.mjs && sh host/engine_test.sh
// Checks tokenizer ids exactly, then the pick and max|dp| per case. Cases whose note is longer than
// STATE_TOKENS are skipped for probabilities (the device drops the tail, JS keeps it).
#include "tinydecide.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

static constexpr int STATE_TOKENS = 40;   // as firmware/main/ai.cpp

static std::vector<uint8_t> slurp(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) { perror(path); exit(2); }
  std::vector<uint8_t> b;
  uint8_t buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) b.insert(b.end(), buf, buf + n);
  fclose(f);
  return b;
}

static std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> v;
  size_t a = 0;
  for (;;) {
    size_t b = s.find(sep, a);
    v.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
    if (b == std::string::npos) break;
    a = b + 1;
  }
  return v;
}

int main(int argc, char** argv) {
  const char* root = argc > 1 ? argv[1] : ".";
  std::string r(root);
  std::vector<uint8_t> model = slurp((r + "/model/model.bin").c_str());
  std::vector<uint8_t> vocab = slurp((r + "/firmware/components/tinydecide/model/vocab.bin").c_str());
  // The engine needs a 16-byte aligned model, as the device's .incbin provides.
  uint8_t* m = (uint8_t*)aligned_alloc(16, (model.size() + 15) & ~(size_t)15);
  memcpy(m, model.data(), model.size());
  if (!td::init(m, model.size(), vocab.data(), vocab.size())) { fprintf(stderr, "init failed\n"); return 2; }

  std::vector<uint8_t> refb = slurp((r + "/host/engine_ref.txt").c_str());
  std::string ref(refb.begin(), refb.end());
  int cases = 0, idBad = 0, pickBad = 0, skipped = 0;
  double maxDp = 0, maxQv = 0, ms = 0;
  for (const std::string& line : split(ref, '\n')) {
    if (line.empty()) continue;
    std::vector<std::string> c = split(line, '\t');
    if (c.size() < 6) continue;
    cases++;
    uint16_t ids[256];
    int n = td::tokenize(c[0].c_str(), ids, 256);
    std::string got;
    for (int i = 0; i < n; i++) got += (i ? "," : "") + std::to_string(ids[i]);
    if (got != c[3]) {
      idBad++;
      if (idBad <= 5) printf("ids differ: %s\n  js  %s\n  c++ %s\n", c[0].c_str(), c[3].c_str(), got.c_str());
      continue;
    }
    std::vector<std::string> opts = split(c[2], '|');
    std::vector<const char*> op;
    for (auto& o : opts) op.push_back(o.c_str());
    td::Question q{td::CHOICE, c[1].c_str(), op.data(), (int)op.size()};
    static td::Answer ch;
    td::Info info;
    const td::Status st = td::answer(c[0].c_str(), &q, 1, &ch, &info, STATE_TOKENS);
    if (st != td::OK) {
      printf("answer failed (%s): %s\n", td::statusText(st), c[0].c_str());
      idBad++;
      continue;
    }
    ms += info.ms;
    if (info.truncated) { skipped++; continue; }
    std::vector<std::string> ps = split(c[4], ',');
    int jsPick = 0;
    for (size_t i = 0; i < ps.size(); i++) {
      double p = atof(ps[i].c_str());
      if (p > atof(ps[jsPick].c_str())) jsPick = (int)i;
      maxDp = fmax(maxDp, fabs(p - ch.probs[i]));
    }
    std::vector<std::string> qv = split(c[5], ',');
    for (size_t i = 0; i < qv.size(); i++) maxQv = fmax(maxQv, fabs(atof(qv[i].c_str()) - ch.qvec[i]));
    if (jsPick != ch.pick) {
      pickBad++;
      printf("pick differs (js %d, c++ %d, js p %s, c++ p %.3f): %s\n", jsPick, ch.pick, ps[jsPick].c_str(),
             ch.probs[ch.pick], c[0].c_str());
    }
  }
  printf("cases %d  token-id mismatches %d  pick mismatches %d  max|dp| %.4f  max|dqvec| %.4f  skipped(long) %d  avg %.0f ms\n",
         cases, idBad, pickBad, maxDp, maxQv, skipped, cases ? ms / cases : 0);
  return idBad || maxDp > 0.02 ? 1 : 0;
}
