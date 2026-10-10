// Category suggestions from the on-device TinyDecide model, and learning from filed notes.
//
// The model runs in its own task (both cores during a pass). Everything it needs is copied into a
// job by the UI task, so the note index is only ever touched from the UI task.
//
// Learning: every filed note is an example of its category. The 8 most recent notes of each
// category are embedded in the background (one model pass each, while the app is idle) and turned
// into prototypes (host/learn.mjs is the reference). Embeddings are cached in the notes folder,
// keyed by the category set, so a new or renamed category only re-embeds; nothing is lost.
#pragma once
#include <stdint.h>

#include <string>

namespace ai {

constexpr const char* MODEL_VARIANT = "S768";   // model/model.bin, see model/SOURCE.md

void begin();                     // init the engine and the worker task
bool ok();                        // engine initialised
void tick();                      // call from the UI loop: collects results, schedules learning

// Ask for a suggestion for `text` against the current categories. Replaces any earlier request.
void suggest(const std::string& text);
void cancel();
bool pending();                   // a suggestion is still being computed
// The latest suggestion, once ready: pick (category index), confidence, per-category probabilities.
bool suggestion(int& pick, float& conf, const float** probs);

void categoriesChanged();         // re-key the learning cache (call after add/rename/delete)
void forget();                    // ignore notes filed so far as examples
const char* status();             // "learning 3/40", "" when idle
uint32_t lastMs();                // duration of the last model pass
int lastTokens();

}  // namespace ai
